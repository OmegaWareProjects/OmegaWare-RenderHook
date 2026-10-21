#include "OmegaRenderHook/Render/Pipeline.hpp"
#include "OmegaRenderHook/Core/Guarded.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Render/DX10/RendererDX10.hpp"
#include "OmegaRenderHook/Render/DX11/DepthDX11.hpp"
#include "OmegaRenderHook/Render/DX11/RendererDX11.hpp"
#include "OmegaRenderHook/Render/DX12/DepthDX12.hpp"
#include "OmegaRenderHook/Render/DX12/QueueResolver.hpp"
#include "OmegaRenderHook/Render/DX12/RendererDX12.hpp"
#include "OmegaRenderHook/Render/DX9/HookDX9.hpp"
#include "OmegaRenderHook/Render/OpenGL/HookOpenGL.hpp"
#include "OmegaRenderHook/Render/PipelineInternal.hpp"
#include "OmegaRenderHook/Render/Vulkan/HookVulkan.hpp"
#include "OmegaRenderHook/Window/WindowHook.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OmegaRenderHook
{
	std::string_view ToString(ERenderApi eApi)
	{
		switch (eApi)
		{
		case ERenderApi::DX9: return "Direct3D 9";
		case ERenderApi::DX10: return "Direct3D 10";
		case ERenderApi::DX11: return "Direct3D 11";
		case ERenderApi::DX12: return "Direct3D 12";
		case ERenderApi::OpenGL: return "OpenGL";
		case ERenderApi::Vulkan: return "Vulkan";
		default: return "none";
		}
	}

	std::string_view ToString(EQueueSource eSource)
	{
		switch (eSource)
		{
		case EQueueSource::CreationHook: return "creation hook";
		case EQueueSource::SwapChainDevice: return "swap chain device";
		case EQueueSource::ObjectScan: return "swap chain object scan";
		default: return "none";
		}
	}

	std::string_view ToString(EDetachReason eReason)
	{
		switch (eReason)
		{
		case EDetachReason::SwapChainChanged: return "swap chain changed";
		case EDetachReason::DeviceLost: return "device lost";
		default: return "uninstall";
		}
	}

	namespace
	{
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		namespace Slots
		{
			constexpr std::size_t uPresent = 8;
			constexpr std::size_t uResizeBuffers = 13;
			constexpr std::size_t uPresent1 = 22;
			constexpr std::size_t uResizeBuffers1 = 39;
			constexpr std::size_t uCreateSwapChain = 10;
			constexpr std::size_t uCreateSwapChainForHwnd = 15;
			constexpr std::size_t uCreateSwapChainForCoreWindow = 16;
			constexpr std::size_t uCreateSwapChainForComposition = 24;
			constexpr std::size_t uExecuteCommandLists = 10;
		}

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
		using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
		using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
		using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
		using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
		using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
		using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

		constexpr std::size_t uSwapChainKinds = 2;
		constexpr std::size_t uKindDX12 = 0;
		constexpr std::size_t uKindDX11 = 1;
		constexpr std::uint32_t uQueueRetryInterval = 30;
		constexpr std::size_t uMaxLiveQueues = 64;
		constexpr std::size_t uMaxQueueVTables = 16;

		struct stVTables
		{
			std::array<void**, uSwapChainKinds> aSwapChains{};
			void** ppFactory = nullptr;
			void** ppQueue = nullptr;
		};

		struct stSwapChainHooks
		{
			clMethodHook clPresent;
			clMethodHook clPresent1;
			clMethodHook clResizeBuffers;
			clMethodHook clResizeBuffers1;
		};

		struct stState
		{
			std::mutex mtxState;
			std::atomic<bool> bActive{ false };
			bool bInstalled = false;
			stPipelineConfig stConfig;
			std::array<stSwapChainHooks, uSwapChainKinds> aSwapChainHooks;
			clMethodHook clCreateSwapChain;
			clMethodHook clCreateSwapChainForHwnd;
			clMethodHook clCreateSwapChainForCoreWindow;
			clMethodHook clCreateSwapChainForComposition;
			clMethodHook clExecuteCommandLists;
			void** ppQueueVTable = nullptr;
			std::unordered_map<IDXGISwapChain*, ComPtr<ID3D12CommandQueue>> mapCreatedQueues;
			IDXGISwapChain* pTarget = nullptr;
			IDXGISwapChain* pDeadSwapChain = nullptr;
			HWND hLayeredWindow = nullptr;
			std::unordered_set<IDXGISwapChain*> setLayeredSwapChains;
			HWND hWindow = nullptr;
			std::unique_ptr<clRendererDX12> pDX12;
			std::unique_ptr<clRendererDX11> pDX11;
			std::unique_ptr<clRendererDX10> pDX10;
			std::uint32_t uRetryCountdown = 0;
			bool bListenerFaulted = false;
			bool bReportedUnresolved = false;
			bool bWindowAttempted = false;
			stPipelineStatus stStatus;

			std::shared_mutex mtxQueues;
			std::vector<void*> aQueueVTables;
			std::unordered_set<void*> setLiveQueues;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		thread_local int t_iPresentDepth = 0;

		[[nodiscard]] void** VTableOf(void* pObject)
		{
			return *static_cast<void***>(pObject);
		}

		[[nodiscard]] clRenderListener* Listener(stState& rState)
		{
			return rState.bListenerFaulted ? nullptr : rState.stConfig.pListener;
		}

		void ReportFault(stState& rState, std::string_view strWhere, const stFault& stFault)
		{
			rState.bListenerFaulted = true;
			rState.stStatus.bListenerFaulted = true;
			WindowHook::SetListener(nullptr);
			if (stFault.bCppException)
				Log(ELogLevel::Error, "listener threw a C++ exception in {}, listener disabled", strWhere);
			else
				Log(ELogLevel::Error, "listener faulted in {} (code 0x{:08X} at {}), listener disabled", strWhere, stFault.uCode, stFault.pAddress);
		}

		template <typename F>
		void CallListener(stState& rState, std::string_view strWhere, F&& fnCall)
		{
			clRenderListener* pListener = Listener(rState);
			if (!pListener)
				return;
			auto fnInvoke = [&] { fnCall(pListener); };
			stFault stFault;
			if (!InvokeGuarded(fnInvoke, stFault))
				ReportFault(rState, strWhere, stFault);
		}

		void DestroyRenderers(stState& rState, EDetachReason eReason)
		{
			if (!rState.pDX12 && !rState.pDX11 && !rState.pDX10)
				return;

			const ERenderApi eApi = rState.pDX12 ? ERenderApi::DX12 : rState.pDX11 ? ERenderApi::DX11 : ERenderApi::DX10;
			if (rState.pDX12)
				rState.pDX12->WaitIdle(1000);
			CallListener(rState, "OnDetach", [&](clRenderListener* pListener) { pListener->OnDetach(eApi, eReason); });
			rState.pDX12.reset();
			rState.pDX11.reset();
			rState.pDX10.reset();
			DepthDX11::Reset();
			DepthDX12::Reset();
			rState.stStatus.eApi = ERenderApi::None;
			rState.stStatus.eQueueSource = EQueueSource::None;
			Log(ELogLevel::Info, "{} renderer detached ({})", ToString(eApi), ToString(eReason));
		}

		[[nodiscard]] bool AcceptWindow(stState& rState, HWND hWindow)
		{
			if (!hWindow)
				return false;
			if (rState.stConfig.hTargetWindow)
				return hWindow == rState.stConfig.hTargetWindow;
			if (rState.hWindow && rState.hWindow != hWindow && IsWindow(rState.hWindow))
				return false;
			DWORD uProcessId = 0;
			GetWindowThreadProcessId(hWindow, &uProcessId);
			return uProcessId == GetCurrentProcessId();
		}

		[[nodiscard]] std::vector<void*> SnapshotQueueVTables(stState& rState, std::unordered_set<void*>& rLiveQueues)
		{
			std::shared_lock lckQueues(rState.mtxQueues);
			rLiveQueues = rState.setLiveQueues;
			return rState.aQueueVTables;
		}

		void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT uCount, ID3D12CommandList* const* ppLists);

		[[nodiscard]] bool CreateRendererDX12(stState& rState, IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, ID3D12Resource* pBuffer)
		{
			ComPtr<ID3D12Device> pDevice;
			ComPtr<IDXGISwapChain3> pSwapChain3;
			if (FAILED(pBuffer->GetDevice(IID_PPV_ARGS(&pDevice))) || FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3))))
				return false;

			stResolvedQueue stQueue;
			if (auto itCreated = rState.mapCreatedQueues.find(pSwapChain); itCreated != rState.mapCreatedQueues.end() && IsPresentQueue(itCreated->second.Get(), pDevice.Get()))
				stQueue = { itCreated->second, EQueueSource::CreationHook };

			if (!stQueue.pQueue)
			{
				std::unordered_set<void*> setLiveQueues;
				const std::vector<void*> aVTables = SnapshotQueueVTables(rState, setLiveQueues);
				stQueue = ResolveQueueFromSwapChain(pSwapChain, pDevice.Get(), aVTables, setLiveQueues);
			}

			if (!stQueue.pQueue)
			{
				if (!rState.bReportedUnresolved)
				{
					rState.bReportedUnresolved = true;
					Log(ELogLevel::Warning, "present queue for swap chain {} not resolved yet, watching command queue submissions", static_cast<void*>(pSwapChain));
				}
				if (!rState.clExecuteCommandLists.IsInstalled() && rState.ppQueueVTable)
				{
					if (!rState.clExecuteCommandLists.Install(rState.stConfig.eHookMethod, rState.ppQueueVTable, Slots::uExecuteCommandLists, reinterpret_cast<void*>(&HookExecuteCommandLists)))
						Log(ELogLevel::Error, "failed to hook ID3D12CommandQueue::ExecuteCommandLists");
				}
				rState.uRetryCountdown = uQueueRetryInterval;
				return false;
			}

			rState.pDX12 = clRendererDX12::Create(pSwapChain3.Get(), pDevice, stQueue.pQueue, stQueue.eSource, stDesc);
			if (!rState.pDX12)
			{
				Log(ELogLevel::Error, "failed to create Direct3D 12 overlay resources");
				rState.uRetryCountdown = uQueueRetryInterval;
				return false;
			}

			static_cast<void>(DepthDX12::Install(pDevice.Get(), rState.pDX12->Device().uFramesInFlight));
			rState.bReportedUnresolved = false;
			rState.stStatus.eApi = ERenderApi::DX12;
			rState.stStatus.eQueueSource = stQueue.eSource;
			Log(ELogLevel::Info, "Direct3D 12 renderer attached: {}x{} format {} buffers {} queue {} via {}", stDesc.BufferDesc.Width, stDesc.BufferDesc.Height,
				static_cast<int>(stDesc.BufferDesc.Format), stDesc.BufferCount, static_cast<void*>(stQueue.pQueue.Get()), ToString(stQueue.eSource));
			const stDeviceDX12& rDevice = rState.pDX12->Device();
			CallListener(rState, "OnAttach", [&](clRenderListener* pListener) { pListener->OnAttach(rDevice); });
			return true;
		}

		[[nodiscard]] bool CreateRendererDX11(stState& rState, IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, ID3D11Texture2D* pBuffer)
		{
			ComPtr<ID3D11Device> pDevice;
			pBuffer->GetDevice(&pDevice);
			if (!pDevice)
				return false;

			rState.pDX11 = clRendererDX11::Create(pSwapChain, pDevice, stDesc);
			static_cast<void>(DepthDX11::Install(rState.pDX11->Device().pContext));
			rState.stStatus.eApi = ERenderApi::DX11;
			rState.stStatus.eQueueSource = EQueueSource::None;
			Log(ELogLevel::Info, "Direct3D 11 renderer attached: {}x{} format {} buffers {}", stDesc.BufferDesc.Width, stDesc.BufferDesc.Height, static_cast<int>(stDesc.BufferDesc.Format), stDesc.BufferCount);
			const stDeviceDX11& rDevice = rState.pDX11->Device();
			CallListener(rState, "OnAttach", [&](clRenderListener* pListener) { pListener->OnAttach(rDevice); });
			return true;
		}

		[[nodiscard]] bool CreateRendererDX10(stState& rState, IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, ComPtr<ID3D10Device> pDevice)
		{
			rState.pDX10 = clRendererDX10::Create(pSwapChain, std::move(pDevice), stDesc);
			rState.stStatus.eApi = ERenderApi::DX10;
			rState.stStatus.eQueueSource = EQueueSource::None;
			Log(ELogLevel::Info, "Direct3D 10 renderer attached: {}x{} format {} buffers {}", stDesc.BufferDesc.Width, stDesc.BufferDesc.Height, static_cast<int>(stDesc.BufferDesc.Format), stDesc.BufferCount);
			const stDeviceDX10& rDevice = rState.pDX10->Device();
			CallListener(rState, "OnAttach", [&](clRenderListener* pListener) { pListener->OnAttach(rDevice); });
			return true;
		}

		[[nodiscard]] bool CreateRenderer(stState& rState, IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc)
		{
			if (rState.uRetryCountdown > 0)
			{
				--rState.uRetryCountdown;
				return false;
			}

			if (rState.stConfig.bDX12)
			{
				ComPtr<ID3D12Resource> pBuffer;
				if (SUCCEEDED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer))))
					return CreateRendererDX12(rState, pSwapChain, stDesc, pBuffer.Get());
			}
			if (rState.stConfig.bDX10)
			{
				ComPtr<ID3D10Device> pDevice10;
				if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&pDevice10))))
					return CreateRendererDX10(rState, pSwapChain, stDesc, std::move(pDevice10));
			}
			if (rState.stConfig.bDX11)
			{
				ComPtr<ID3D11Texture2D> pBuffer;
				if (SUCCEEDED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer))))
					return CreateRendererDX11(rState, pSwapChain, stDesc, pBuffer.Get());
			}
			rState.pDeadSwapChain = pSwapChain;
			return false;
		}

		void HandleResult(stState& rState, IDXGISwapChain* pSwapChain, ERenderResult eResult, const stFault& stFault)
		{
			switch (eResult)
			{
			case ERenderResult::Rendered:
				++rState.stStatus.uFramesRendered;
				break;
			case ERenderResult::Skipped:
				++rState.stStatus.uFramesSkipped;
				break;
			case ERenderResult::Rebuild:
				DestroyRenderers(rState, EDetachReason::SwapChainChanged);
				break;
			case ERenderResult::DeviceLost:
				Log(ELogLevel::Error, "graphics device removed, overlay stopped for this swap chain");
				rState.stStatus.bDeviceLost = true;
				rState.pDeadSwapChain = pSwapChain;
				DestroyRenderers(rState, EDetachReason::DeviceLost);
				break;
			case ERenderResult::Faulted:
				ReportFault(rState, "OnFrame", stFault);
				break;
			}
		}

		void OnPresent(IDXGISwapChain* pSwapChain)
		{
			stState& rState = State();
			if (!rState.bActive.load(std::memory_order_acquire))
				return;

			std::scoped_lock lckState(rState.mtxState);
			if (!rState.bActive.load(std::memory_order_acquire) || pSwapChain == rState.pDeadSwapChain)
				return;

			if (PipelineInternal::clLayeredScope::IsActive() || rState.setLayeredSwapChains.contains(pSwapChain))
				return;
			DXGI_SWAP_CHAIN_DESC stDesc{};
			if (FAILED(pSwapChain->GetDesc(&stDesc)) || stDesc.OutputWindow == rState.hLayeredWindow || !AcceptWindow(rState, stDesc.OutputWindow))
				return;

			if (pSwapChain != rState.pTarget)
			{
				DestroyRenderers(rState, EDetachReason::SwapChainChanged);
				rState.pTarget = pSwapChain;
				rState.uRetryCountdown = 0;
				++rState.stStatus.uSwapChainsSeen;
				if (rState.hWindow != stDesc.OutputWindow)
				{
					if (WindowHook::IsAttached())
						WindowHook::Detach();
					rState.bWindowAttempted = false;
					rState.stStatus.bWindowHooked = false;
				}
				rState.hWindow = stDesc.OutputWindow;
			}

			if (!rState.pDX12 && !rState.pDX11 && !rState.pDX10 && !CreateRenderer(rState, pSwapChain, stDesc))
				return;

			if (rState.stConfig.bHookWindow && !rState.bWindowAttempted)
			{
				rState.bWindowAttempted = true;
				rState.stStatus.bWindowHooked = WindowHook::Attach(rState.hWindow, Listener(rState));
			}

			rState.stStatus.hWindow = stDesc.OutputWindow;
			rState.stStatus.eFormat = stDesc.BufferDesc.Format;
			rState.stStatus.uWidth = stDesc.BufferDesc.Width;
			rState.stStatus.uHeight = stDesc.BufferDesc.Height;
			rState.stStatus.uBufferCount = stDesc.BufferCount;

			stFault stFault;
			if (rState.pDX12)
			{
				ComPtr<IDXGISwapChain3> pSwapChain3;
				if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3))))
					return;
				HandleResult(rState, pSwapChain, rState.pDX12->Render(pSwapChain3.Get(), stDesc, Listener(rState), stFault), stFault);
			}
			else if (rState.pDX11)
				HandleResult(rState, pSwapChain, rState.pDX11->Render(pSwapChain, stDesc, Listener(rState), stFault), stFault);
			else if (rState.pDX10)
				HandleResult(rState, pSwapChain, rState.pDX10->Render(pSwapChain, stDesc, Listener(rState), stFault), stFault);
		}

		void OnBeforeResize(IDXGISwapChain* pSwapChain, IUnknown* const* ppPresentQueues)
		{
			stState& rState = State();
			if (!rState.bActive.load(std::memory_order_acquire))
				return;

			std::scoped_lock lckState(rState.mtxState);
			if (pSwapChain != rState.pTarget)
				return;

			if (ppPresentQueues && ppPresentQueues[0])
			{
				ComPtr<ID3D12CommandQueue> pQueue;
				if (SUCCEEDED(ppPresentQueues[0]->QueryInterface(IID_PPV_ARGS(&pQueue))))
				{
					rState.mapCreatedQueues[pSwapChain] = pQueue;
					if (rState.pDX12 && rState.pDX12->Queue() != pQueue.Get())
						DestroyRenderers(rState, EDetachReason::SwapChainChanged);
				}
			}

			if (rState.pDX12)
				rState.pDX12->WaitIdle(1000);
		}

		void OnSwapChainCreated(IUnknown* pDevice, IDXGISwapChain* pSwapChain, HWND hWindow)
		{
			stState& rState = State();
			if (!rState.bActive.load(std::memory_order_acquire) || !pDevice || !pSwapChain)
				return;

			std::scoped_lock lckState(rState.mtxState);
			if (PipelineInternal::clLayeredScope::IsActive())
			{
				rState.setLayeredSwapChains.insert(pSwapChain);
				Log(ELogLevel::Debug, "swap chain {} was created by the OpenGL or Vulkan driver, DXGI hooks ignore it", static_cast<void*>(pSwapChain));
				return;
			}
			rState.setLayeredSwapChains.erase(pSwapChain);
			ComPtr<ID3D12CommandQueue> pQueue;
			if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))))
			{
				rState.mapCreatedQueues[pSwapChain] = pQueue;
				Log(ELogLevel::Debug, "swap chain {} created on queue {}", static_cast<void*>(pSwapChain), static_cast<void*>(pQueue.Get()));
			}
			else
				rState.mapCreatedQueues.erase(pSwapChain);

			if (pSwapChain == rState.pDeadSwapChain)
				rState.pDeadSwapChain = nullptr;
			if (pSwapChain == rState.pTarget || (hWindow && hWindow == rState.hWindow))
			{
				DestroyRenderers(rState, EDetachReason::SwapChainChanged);
				rState.pTarget = nullptr;
			}
		}

		void NoteSubmittingQueue(ID3D12CommandQueue* pQueue)
		{
			stState& rState = State();
			{
				std::shared_lock lckQueues(rState.mtxQueues);
				if (rState.setLiveQueues.contains(pQueue))
					return;
			}

			std::unique_lock lckQueues(rState.mtxQueues);
			if (rState.setLiveQueues.size() >= uMaxLiveQueues)
				return;
			rState.setLiveQueues.insert(pQueue);
			void* pVTable = VTableOf(pQueue);
			if (rState.aQueueVTables.size() < uMaxQueueVTables && std::ranges::find(rState.aQueueVTables, pVTable) == rState.aQueueVTables.end())
				rState.aQueueVTables.push_back(pVTable);
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* pSwapChain, UINT uSyncInterval, UINT uFlags)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aSwapChainHooks[uKind].clPresent.Original<PresentFn>();
			if (++t_iPresentDepth == 1 && !(uFlags & DXGI_PRESENT_TEST))
				OnPresent(pSwapChain);
			const HRESULT hResult = pfnOriginal(pSwapChain, uSyncInterval, uFlags);
			--t_iPresentDepth;
			return hResult;
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* pSwapChain, UINT uSyncInterval, UINT uFlags, const DXGI_PRESENT_PARAMETERS* pParameters)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aSwapChainHooks[uKind].clPresent1.Original<Present1Fn>();
			if (++t_iPresentDepth == 1 && !(uFlags & DXGI_PRESENT_TEST))
				OnPresent(pSwapChain);
			const HRESULT hResult = pfnOriginal(pSwapChain, uSyncInterval, uFlags, pParameters);
			--t_iPresentDepth;
			return hResult;
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* pSwapChain, UINT uBufferCount, UINT uWidth, UINT uHeight, DXGI_FORMAT eFormat, UINT uFlags)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aSwapChainHooks[uKind].clResizeBuffers.Original<ResizeBuffersFn>();
			OnBeforeResize(pSwapChain, nullptr);
			return pfnOriginal(pSwapChain, uBufferCount, uWidth, uHeight, eFormat, uFlags);
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* pSwapChain, UINT uBufferCount, UINT uWidth, UINT uHeight, DXGI_FORMAT eFormat, UINT uFlags, const UINT* pNodeMasks, IUnknown* const* ppPresentQueues)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aSwapChainHooks[uKind].clResizeBuffers1.Original<ResizeBuffers1Fn>();
			OnBeforeResize(pSwapChain, ppPresentQueues);
			return pfnOriginal(pSwapChain, uBufferCount, uWidth, uHeight, eFormat, uFlags, pNodeMasks, ppPresentQueues);
		}

		HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().clCreateSwapChain.Original<CreateSwapChainFn>();
			const HRESULT hResult = pfnOriginal(pFactory, pDevice, pDesc, ppSwapChain);
			if (SUCCEEDED(hResult) && ppSwapChain)
				OnSwapChainCreated(pDevice, *ppSwapChain, pDesc ? pDesc->OutputWindow : nullptr);
			return hResult;
		}

		HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* pFactory, IUnknown* pDevice, HWND hWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pOutput, IDXGISwapChain1** ppSwapChain)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().clCreateSwapChainForHwnd.Original<CreateSwapChainForHwndFn>();
			const HRESULT hResult = pfnOriginal(pFactory, pDevice, hWindow, pDesc, pFullscreenDesc, pOutput, ppSwapChain);
			if (SUCCEEDED(hResult) && ppSwapChain)
				OnSwapChainCreated(pDevice, *ppSwapChain, hWindow);
			return hResult;
		}

		HRESULT STDMETHODCALLTYPE HookCreateSwapChainForCoreWindow(IDXGIFactory2* pFactory, IUnknown* pDevice, IUnknown* pWindow, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pOutput, IDXGISwapChain1** ppSwapChain)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().clCreateSwapChainForCoreWindow.Original<CreateSwapChainForCoreWindowFn>();
			const HRESULT hResult = pfnOriginal(pFactory, pDevice, pWindow, pDesc, pOutput, ppSwapChain);
			if (SUCCEEDED(hResult) && ppSwapChain)
				OnSwapChainCreated(pDevice, *ppSwapChain, nullptr);
			return hResult;
		}

		HRESULT STDMETHODCALLTYPE HookCreateSwapChainForComposition(IDXGIFactory2* pFactory, IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pOutput, IDXGISwapChain1** ppSwapChain)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().clCreateSwapChainForComposition.Original<CreateSwapChainForCompositionFn>();
			const HRESULT hResult = pfnOriginal(pFactory, pDevice, pDesc, pOutput, ppSwapChain);
			if (SUCCEEDED(hResult) && ppSwapChain)
				OnSwapChainCreated(pDevice, *ppSwapChain, nullptr);
			return hResult;
		}

		void STDMETHODCALLTYPE HookExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT uCount, ID3D12CommandList* const* ppLists)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().clExecuteCommandLists.Original<ExecuteCommandListsFn>();
			if (pQueue)
				NoteSubmittingQueue(pQueue);
			pfnOriginal(pQueue, uCount, ppLists);
		}

		class clDummyWindow
		{
		public:
			clDummyWindow()
			{
				WNDCLASSEXW stClass{ sizeof(WNDCLASSEXW) };
				stClass.lpfnWndProc = DefWindowProcW;
				stClass.hInstance = GetModuleHandleW(nullptr);
				stClass.lpszClassName = L"OmegaRenderHookDummyWindow";
				RegisterClassExW(&stClass);
				m_hWindow = CreateWindowExW(0, stClass.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, stClass.hInstance, nullptr);
			}

			~clDummyWindow()
			{
				if (m_hWindow)
					DestroyWindow(m_hWindow);
			}

			clDummyWindow(const clDummyWindow&) = delete;
			clDummyWindow& operator=(const clDummyWindow&) = delete;

			[[nodiscard]] HWND Handle() const { return m_hWindow; }

		private:
			HWND m_hWindow = nullptr;
		};

		[[nodiscard]] DXGI_SWAP_CHAIN_DESC1 DummySwapChainDesc()
		{
			DXGI_SWAP_CHAIN_DESC1 stDesc{};
			stDesc.Width = 64;
			stDesc.Height = 64;
			stDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			stDesc.SampleDesc.Count = 1;
			stDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			stDesc.BufferCount = 2;
			stDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			return stDesc;
		}

		[[nodiscard]] std::expected<stVTables, std::string> CaptureVTables(const stPipelineConfig& stConfig)
		{
			stVTables stTables;
			ComPtr<IDXGIFactory2> pFactory;
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory))))
				return std::unexpected("CreateDXGIFactory1 failed");
			stTables.ppFactory = VTableOf(pFactory.Get());
			const DXGI_SWAP_CHAIN_DESC1 stDesc = DummySwapChainDesc();

			if (stConfig.bDX12)
			{
				clDummyWindow clWindow;
				ComPtr<ID3D12Device> pDevice;
				ComPtr<ID3D12CommandQueue> pQueue;
				ComPtr<IDXGISwapChain1> pSwapChain;
				D3D12_COMMAND_QUEUE_DESC stQueue{ D3D12_COMMAND_LIST_TYPE_DIRECT };
				if (clWindow.Handle() && SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) &&
					SUCCEEDED(pDevice->CreateCommandQueue(&stQueue, IID_PPV_ARGS(&pQueue))) &&
					SUCCEEDED(pFactory->CreateSwapChainForHwnd(pQueue.Get(), clWindow.Handle(), &stDesc, nullptr, nullptr, &pSwapChain)))
				{
					stTables.aSwapChains[uKindDX12] = VTableOf(pSwapChain.Get());
					stTables.ppQueue = VTableOf(pQueue.Get());
				}
				else
					Log(ELogLevel::Warning, "could not create a dummy Direct3D 12 swap chain, Direct3D 12 hooks skipped");
			}

			if (stConfig.bDX11)
			{
				clDummyWindow clWindow;
				ComPtr<ID3D11Device> pDevice;
				ComPtr<IDXGISwapChain1> pSwapChain;
				if (clWindow.Handle() && SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pDevice, nullptr, nullptr)) &&
					SUCCEEDED(pFactory->CreateSwapChainForHwnd(pDevice.Get(), clWindow.Handle(), &stDesc, nullptr, nullptr, &pSwapChain)))
					stTables.aSwapChains[uKindDX11] = VTableOf(pSwapChain.Get());
				else
					Log(ELogLevel::Warning, "could not create a dummy Direct3D 11 swap chain, Direct3D 11 hooks skipped");
			}

			if (!stTables.aSwapChains[uKindDX12] && !stTables.aSwapChains[uKindDX11])
				return std::unexpected("no dummy swap chain could be created");
			return stTables;
		}

		[[nodiscard]] void* HookIdentity(EHookMethod eMethod, void** ppVTable, std::size_t uIndex)
		{
			return eMethod == EHookMethod::VTable ? static_cast<void*>(&ppVTable[uIndex]) : ppVTable[uIndex];
		}

		template <std::size_t uKind>
		[[nodiscard]] bool InstallSwapChainHooks(stState& rState, EHookMethod eMethod, void** ppVTable, std::vector<void*>& rIdentities)
		{
			stSwapChainHooks& rHooks = rState.aSwapChainHooks[uKind];
			bool bPresent = false;
			auto fnInstall = [&](clMethodHook& rHook, std::size_t uIndex, void* pDetour, std::string_view strName)
			{
				void* pIdentity = HookIdentity(eMethod, ppVTable, uIndex);
				if (std::ranges::find(rIdentities, pIdentity) != rIdentities.end())
					return true;
				if (!rHook.Install(eMethod, ppVTable, uIndex, pDetour))
				{
					Log(ELogLevel::Error, "failed to hook {} ({})", strName, ToString(eMethod));
					return false;
				}
				rIdentities.push_back(pIdentity);
				return true;
			};

			bPresent = fnInstall(rHooks.clPresent, Slots::uPresent, reinterpret_cast<void*>(&HookPresent<uKind>), "IDXGISwapChain::Present");
			bPresent = fnInstall(rHooks.clPresent1, Slots::uPresent1, reinterpret_cast<void*>(&HookPresent1<uKind>), "IDXGISwapChain1::Present1") && bPresent;
			const bool bResize = fnInstall(rHooks.clResizeBuffers, Slots::uResizeBuffers, reinterpret_cast<void*>(&HookResizeBuffers<uKind>), "IDXGISwapChain::ResizeBuffers") &&
				fnInstall(rHooks.clResizeBuffers1, Slots::uResizeBuffers1, reinterpret_cast<void*>(&HookResizeBuffers1<uKind>), "IDXGISwapChain3::ResizeBuffers1");
			return bPresent && bResize;
		}

		template <typename F>
		void ForEachHook(stState& rState, F&& fnVisit)
		{
			for (stSwapChainHooks& rHooks : rState.aSwapChainHooks)
			{
				fnVisit(rHooks.clPresent);
				fnVisit(rHooks.clPresent1);
				fnVisit(rHooks.clResizeBuffers);
				fnVisit(rHooks.clResizeBuffers1);
			}
			fnVisit(rState.clCreateSwapChain);
			fnVisit(rState.clCreateSwapChainForHwnd);
			fnVisit(rState.clCreateSwapChainForCoreWindow);
			fnVisit(rState.clCreateSwapChainForComposition);
			fnVisit(rState.clExecuteCommandLists);
		}

		[[nodiscard]] bool RemoveHooks(stState& rState)
		{
			bool bAllRestored = true;
			ForEachHook(rState, [&](clMethodHook& rHook) { bAllRestored = rHook.Disable() && bAllRestored; });
			return bAllRestored;
		}

		void ReleaseHooks(stState& rState)
		{
			ForEachHook(rState, [](clMethodHook& rHook) { rHook.Release(); });
		}
	}

	namespace PipelineInternal
	{
		bool IsActive()
		{
			return State().bActive.load(std::memory_order_acquire);
		}

		bool AcquireWindow(HWND hWindow, ERenderApi eApi, UINT uWidth, UINT uHeight)
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			if (!rState.bActive.load(std::memory_order_acquire) || !AcceptWindow(rState, hWindow))
				return false;
			if (rState.hWindow != hWindow)
			{
				if (WindowHook::IsAttached())
					WindowHook::Detach();
				rState.bWindowAttempted = false;
				rState.stStatus.bWindowHooked = false;
				rState.hWindow = hWindow;
			}
			if ((eApi == ERenderApi::OpenGL || eApi == ERenderApi::Vulkan) && rState.hLayeredWindow != hWindow)
			{
				rState.hLayeredWindow = hWindow;
				if (rState.pDX12 || rState.pDX11 || rState.pDX10)
				{
					Log(ELogLevel::Info, "{} owns the window, dropping the driver's internal DXGI swap chain", ToString(eApi));
					DestroyRenderers(rState, EDetachReason::SwapChainChanged);
				}
				rState.pTarget = nullptr;
			}
			if (rState.stConfig.bHookWindow && !rState.bWindowAttempted)
			{
				rState.bWindowAttempted = true;
				rState.stStatus.bWindowHooked = WindowHook::Attach(hWindow, Listener(rState));
			}
			rState.stStatus.eApi = eApi;
			rState.stStatus.hWindow = hWindow;
			rState.stStatus.uWidth = uWidth;
			rState.stStatus.uHeight = uHeight;
			return true;
		}

		clRenderListener* Listener()
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			return Listener(rState);
		}

		void ReportFault(std::string_view strWhere, const stFault& stFault)
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			ReportFault(rState, strWhere, stFault);
		}

		void CountFrame(bool bRendered)
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			++(bRendered ? rState.stStatus.uFramesRendered : rState.stStatus.uFramesSkipped);
		}

		void ReleaseApi(ERenderApi eApi)
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			if (rState.stStatus.eApi == eApi)
				rState.stStatus.eApi = ERenderApi::None;
		}

		bool clDetachRequest::WaitDone(std::chrono::milliseconds msTimeout) const
		{
			const auto tpDeadline = std::chrono::steady_clock::now() + msTimeout;
			while (!m_bDone.load(std::memory_order_acquire))
			{
				if (std::chrono::steady_clock::now() >= tpDeadline)
					return false;
				Sleep(5);
			}
			return true;
		}
	}

	namespace Pipeline
	{
		void PrepareEarly(bool bHookVulkan)
		{
			DepthDX12::PrepareEarly();
			if (bHookVulkan)
				HookVulkan::PrepareEarly();
		}

		void NoteVulkanDevice(void* hDevice, void* hPhysicalDevice)
		{
			HookVulkan::NoteDevice(hDevice, hPhysicalDevice);
		}

		std::expected<void, std::string> Install(const stPipelineConfig& stConfig)
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			if (rState.bInstalled)
				return std::unexpected("pipeline already installed");
			if (!stConfig.bDX11 && !stConfig.bDX12)
				return std::unexpected("no graphics API enabled");

			auto expTables = CaptureVTables(stConfig);
			if (!expTables)
				return std::unexpected(expTables.error());

			rState.stConfig = stConfig;
			rState.stStatus = {};
			rState.stStatus.eHookMethod = stConfig.eHookMethod;
			rState.bListenerFaulted = false;
			rState.bReportedUnresolved = false;
			rState.bWindowAttempted = false;
			rState.ppQueueVTable = expTables->ppQueue;
			{
				std::unique_lock lckQueues(rState.mtxQueues);
				rState.aQueueVTables.clear();
				rState.setLiveQueues.clear();
				if (expTables->ppQueue)
					rState.aQueueVTables.push_back(expTables->ppQueue);
			}

			std::vector<void*> aIdentities;
			bool bSwapChainHooks = false;
			if (void** ppVTable = expTables->aSwapChains[uKindDX12])
				bSwapChainHooks = InstallSwapChainHooks<uKindDX12>(rState, stConfig.eHookMethod, ppVTable, aIdentities) || bSwapChainHooks;
			if (void** ppVTable = expTables->aSwapChains[uKindDX11])
				bSwapChainHooks = InstallSwapChainHooks<uKindDX11>(rState, stConfig.eHookMethod, ppVTable, aIdentities) || bSwapChainHooks;

			void** ppFactory = expTables->ppFactory;
			const bool bFactoryHooks =
				rState.clCreateSwapChain.Install(stConfig.eHookMethod, ppFactory, Slots::uCreateSwapChain, reinterpret_cast<void*>(&HookCreateSwapChain)) &&
				rState.clCreateSwapChainForHwnd.Install(stConfig.eHookMethod, ppFactory, Slots::uCreateSwapChainForHwnd, reinterpret_cast<void*>(&HookCreateSwapChainForHwnd)) &&
				rState.clCreateSwapChainForCoreWindow.Install(stConfig.eHookMethod, ppFactory, Slots::uCreateSwapChainForCoreWindow, reinterpret_cast<void*>(&HookCreateSwapChainForCoreWindow)) &&
				rState.clCreateSwapChainForComposition.Install(stConfig.eHookMethod, ppFactory, Slots::uCreateSwapChainForComposition, reinterpret_cast<void*>(&HookCreateSwapChainForComposition));

			if (!bSwapChainHooks || !bFactoryHooks)
			{
				static_cast<void>(RemoveHooks(rState));
				ReleaseHooks(rState);
				return std::unexpected(bSwapChainHooks ? "failed to hook the DXGI factory" : "failed to hook the DXGI swap chain");
			}

			const bool bDX9 = stConfig.bDX9 && HookDX9::Install(stConfig.eHookMethod);
			const bool bOpenGL = stConfig.bOpenGL && HookOpenGL::Install();
			const bool bVulkan = stConfig.bVulkan && HookVulkan::Install();

			rState.bInstalled = true;
			rState.stStatus.bInstalled = true;
			rState.bActive.store(true, std::memory_order_release);
			Log(ELogLevel::Info, "pipeline installed ({} hooks, Direct3D 12 {}, Direct3D 11 {}, Direct3D 10 {}, Direct3D 9 {}, OpenGL {}, Vulkan {})", ToString(stConfig.eHookMethod),
				expTables->aSwapChains[uKindDX12] ? "on" : "off", expTables->aSwapChains[uKindDX11] ? "on" : "off", stConfig.bDX10 && expTables->aSwapChains[uKindDX11] ? "on" : "off",
				bDX9 ? "on" : "off", bOpenGL ? "on" : "off", bVulkan ? "on" : "off");
			return {};
		}

		bool Uninstall()
		{
			stState& rState = State();
			if (!IsInstalled())
			{
				bool bEarlyRestored = DepthDX12::Disable();
				bEarlyRestored = HookVulkan::DisableEarly() && bEarlyRestored;
				const bool bEarlyDrained = clHookScope::WaitForDrain(std::chrono::milliseconds(2000));
				DepthDX12::Release(bEarlyDrained);
				HookVulkan::ReleaseEarly(bEarlyDrained);
				return bEarlyRestored && bEarlyDrained;
			}
			HookDX9::RequestDetach(std::chrono::milliseconds(1000));
			HookOpenGL::RequestDetach(std::chrono::milliseconds(1000));
			HookVulkan::RequestDetach(std::chrono::milliseconds(1000));

			bool bRestored = true;
			{
				std::scoped_lock lckState(rState.mtxState);
				if (!rState.bInstalled)
					return true;
				rState.bActive.store(false, std::memory_order_release);
				DestroyRenderers(rState, EDetachReason::Uninstall);
				bRestored = WindowHook::Detach() && bRestored;
				bRestored = RemoveHooks(rState) && bRestored;
				bRestored = HookDX9::Disable() && bRestored;
				bRestored = HookOpenGL::Disable() && bRestored;
				bRestored = HookVulkan::Disable() && bRestored;
				bRestored = DepthDX11::Disable() && bRestored;
				bRestored = DepthDX12::Disable() && bRestored;
			}

			const bool bDrained = clHookScope::WaitForDrain(std::chrono::milliseconds(2000));
			if (!bDrained)
				Log(ELogLevel::Warning, "hooked calls still in flight after uninstall, the module is not safe to unload");
			HookDX9::Release(bDrained);
			HookOpenGL::Release(bDrained);
			HookVulkan::Release(bDrained);
			DepthDX11::Release(bDrained);
			DepthDX12::Release(bDrained);

			std::scoped_lock lckState(rState.mtxState);
			if (bDrained)
				ReleaseHooks(rState);
			rState.mapCreatedQueues.clear();
			rState.pTarget = nullptr;
			rState.pDeadSwapChain = nullptr;
			rState.hLayeredWindow = nullptr;
			rState.setLayeredSwapChains.clear();
			rState.hWindow = nullptr;
			rState.bInstalled = false;
			rState.stStatus.bInstalled = false;
			rState.stStatus.bWindowHooked = false;
			Log(ELogLevel::Info, "pipeline uninstalled{}", bRestored && bDrained ? "" : " (not safe to unload)");
			return bRestored && bDrained;
		}

		bool IsInstalled()
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			return rState.bInstalled;
		}

		stPipelineStatus GetStatus()
		{
			stState& rState = State();
			std::scoped_lock lckState(rState.mtxState);
			return rState.stStatus;
		}
	}
}
