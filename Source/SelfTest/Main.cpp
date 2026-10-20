#include "OmegaRenderHook/OmegaRenderHook.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	using OmegaRenderHook::EHookMethod;
	using OmegaRenderHook::EQueueSource;
	using OmegaRenderHook::ERenderApi;

	constexpr UINT uProbeX = 16;
	constexpr UINT uProbeY = 16;
	constexpr UINT uGameX = 120;
	constexpr UINT uGameY = 100;
	constexpr std::uint32_t uMagenta = 0xFFFF00FF;

	int g_iFailures = 0;
	bool g_bVerbose = false;
	bool g_bWarp = false;

	void Check(bool bCondition, std::string_view strName, std::string_view strDetail = {})
	{
		if (bCondition)
			std::println("  [pass] {}", strName);
		else
		{
			++g_iFailures;
			std::println("  [FAIL] {}{}{}", strName, strDetail.empty() ? "" : ": ", strDetail);
		}
	}

	[[nodiscard]] std::array<std::uint8_t, 4> GameColorBytes(std::uint32_t uFrame)
	{
		return { 0x10, static_cast<std::uint8_t>((uFrame * 7 + 3) & 0xFF), 0x20, 0xFF };
	}

	[[nodiscard]] std::array<float, 4> GameColor(std::uint32_t uFrame)
	{
		const auto aBytes = GameColorBytes(uFrame);
		return { aBytes[0] / 255.f, aBytes[1] / 255.f, aBytes[2] / 255.f, aBytes[3] / 255.f };
	}

	[[nodiscard]] std::uint32_t PackBytes(const std::array<std::uint8_t, 4>& aBytes)
	{
		return aBytes[0] | (aBytes[1] << 8) | (aBytes[2] << 16) | (static_cast<std::uint32_t>(aBytes[3]) << 24);
	}

	class clTestWindow
	{
	public:
		clTestWindow()
		{
			WNDCLASSEXW stClass{ sizeof(WNDCLASSEXW) };
			stClass.lpfnWndProc = &WindowProc;
			stClass.hInstance = GetModuleHandleW(nullptr);
			stClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
			stClass.lpszClassName = L"OmegaRenderHookSelfTest";
			RegisterClassExW(&stClass);
			RECT stRect{ 0, 0, 320, 240 };
			AdjustWindowRect(&stRect, WS_OVERLAPPEDWINDOW, FALSE);
			m_hWindow = CreateWindowExW(0, stClass.lpszClassName, L"OmegaRenderHook self-test", WS_OVERLAPPEDWINDOW, 64, 64, stRect.right - stRect.left, stRect.bottom - stRect.top, nullptr, nullptr, stClass.hInstance, nullptr);
			ShowWindow(m_hWindow, SW_SHOWNOACTIVATE);
		}

		~clTestWindow()
		{
			DestroyWindow(m_hWindow);
		}

		clTestWindow(const clTestWindow&) = delete;
		clTestWindow& operator=(const clTestWindow&) = delete;

		[[nodiscard]] HWND Handle() const { return m_hWindow; }

		static void Pump()
		{
			MSG stMessage;
			while (PeekMessageW(&stMessage, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&stMessage);
				DispatchMessageW(&stMessage);
			}
		}

		static LRESULT CALLBACK WindowProc(HWND hWindow, UINT uMessage, WPARAM uWParam, LPARAM iLParam)
		{
			if (uMessage == WM_APP + 1)
				return 0x1234;
			return DefWindowProcW(hWindow, uMessage, uWParam, iLParam);
		}

	private:
		HWND m_hWindow = nullptr;
	};

	class clFakeGameDX12
	{
	public:
		bool Create()
		{
			if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&m_pFactory))))
				return false;
			ComPtr<IDXGIAdapter> pAdapter;
			if (g_bWarp)
			{
				ComPtr<IDXGIFactory4> pFactory4;
				if (FAILED(m_pFactory.As(&pFactory4)) || FAILED(pFactory4->EnumWarpAdapter(IID_PPV_ARGS(&pAdapter))))
					return false;
			}
			if (FAILED(D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDevice))))
				return false;
			D3D12_COMMAND_QUEUE_DESC stDirect{ D3D12_COMMAND_LIST_TYPE_DIRECT };
			D3D12_COMMAND_QUEUE_DESC stCompute{ D3D12_COMMAND_LIST_TYPE_COMPUTE };
			for (auto& pDecoy : m_aDecoys)
				m_pDevice->CreateCommandQueue(&stDirect, IID_PPV_ARGS(&pDecoy));
			m_pDevice->CreateCommandQueue(&stCompute, IID_PPV_ARGS(&m_pComputeDecoy));
			m_pDevice->CreateCommandQueue(&stDirect, IID_PPV_ARGS(&m_pQueueA));
			m_pDevice->CreateCommandQueue(&stDirect, IID_PPV_ARGS(&m_pQueueB));
			m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence));
			m_hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

			for (auto& pAllocator : m_aAllocators)
				m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pAllocator));
			m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_aAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_pList));
			m_pList->Close();
			m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pDecoyAllocator));
			m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pDecoyAllocator.Get(), nullptr, IID_PPV_ARGS(&m_pDecoyList));
			m_pDecoyList->Close();
			m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_pComputeAllocator));
			m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_pComputeAllocator.Get(), nullptr, IID_PPV_ARGS(&m_pComputeList));
			m_pComputeList->Close();

			D3D12_DESCRIPTOR_HEAP_DESC stHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
			m_pDevice->CreateDescriptorHeap(&stHeap, IID_PPV_ARGS(&m_pHeap));
			return m_pQueueA && m_pQueueB && m_pComputeDecoy && m_pHeap && m_pList;
		}

		~clFakeGameDX12()
		{
			Flush();
			if (m_hEvent)
				CloseHandle(m_hEvent);
		}

		HRESULT CreateSwapChain(HWND hWindow, ID3D12CommandQueue* pQueue, DXGI_FORMAT eFormat, UINT uWidth, UINT uHeight)
		{
			DXGI_SWAP_CHAIN_DESC1 stDesc{};
			stDesc.Width = uWidth;
			stDesc.Height = uHeight;
			stDesc.Format = eFormat;
			stDesc.SampleDesc.Count = 1;
			stDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			stDesc.BufferCount = 3;
			stDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			ComPtr<IDXGISwapChain1> pSwapChain;
			const HRESULT hResult = m_pFactory->CreateSwapChainForHwnd(pQueue, hWindow, &stDesc, nullptr, nullptr, &pSwapChain);
			if (FAILED(hResult))
				return hResult;
			m_pPresentQueue = pQueue;
			return pSwapChain.As(&m_pSwapChain);
		}

		void ReleaseSwapChain()
		{
			Flush();
			m_pSwapChain.Reset();
		}

		HRESULT Resize(UINT uWidth, UINT uHeight)
		{
			Flush();
			return m_pSwapChain->ResizeBuffers(0, uWidth, uHeight, DXGI_FORMAT_UNKNOWN, 0);
		}

		HRESULT Frame()
		{
			const UINT uSlot = m_uFrame % static_cast<UINT>(m_aAllocators.size());
			Wait(m_aSlotFence[uSlot]);

			m_pDecoyAllocator->Reset();
			m_pDecoyList->Reset(m_pDecoyAllocator.Get(), nullptr);
			m_pDecoyList->Close();
			m_pComputeAllocator->Reset();
			m_pComputeList->Reset(m_pComputeAllocator.Get(), nullptr);
			m_pComputeList->Close();
			ID3D12CommandList* const aDecoyLists[] = { m_pDecoyList.Get() };
			ID3D12CommandList* const aComputeLists[] = { m_pComputeList.Get() };
			m_pComputeDecoy->ExecuteCommandLists(1, aComputeLists);
			for (auto& pDecoy : m_aDecoys)
			{
				pDecoy->ExecuteCommandLists(1, aDecoyLists);
				pDecoy->Signal(m_pFence.Get(), ++m_uFenceValue);
				Wait(m_uFenceValue);
			}

			const UINT uIndex = m_pSwapChain->GetCurrentBackBufferIndex();
			ComPtr<ID3D12Resource> pBuffer;
			m_pSwapChain->GetBuffer(uIndex, IID_PPV_ARGS(&pBuffer));
			const D3D12_CPU_DESCRIPTOR_HANDLE hTarget = m_pHeap->GetCPUDescriptorHandleForHeapStart();
			m_pDevice->CreateRenderTargetView(pBuffer.Get(), nullptr, hTarget);

			m_aAllocators[uSlot]->Reset();
			m_pList->Reset(m_aAllocators[uSlot].Get(), nullptr);
			D3D12_RESOURCE_BARRIER stBarrier{};
			stBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			stBarrier.Transition.pResource = pBuffer.Get();
			stBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			stBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			stBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			m_pList->ResourceBarrier(1, &stBarrier);
			const std::array<float, 4> aColor = GameColor(m_uFrame);
			m_pList->ClearRenderTargetView(hTarget, aColor.data(), 0, nullptr);
			std::swap(stBarrier.Transition.StateBefore, stBarrier.Transition.StateAfter);
			m_pList->ResourceBarrier(1, &stBarrier);
			m_pList->Close();
			ID3D12CommandList* const aLists[] = { m_pList.Get() };
			m_pPresentQueue->ExecuteCommandLists(1, aLists);
			pBuffer.Reset();

			const HRESULT hResult = m_pSwapChain->Present(0, 0);
			m_pPresentQueue->Signal(m_pFence.Get(), ++m_uFenceValue);
			m_aSlotFence[uSlot] = m_uFenceValue;
			m_uLastFrame = m_uFrame++;
			return hResult;
		}

		void Flush()
		{
			if (!m_pPresentQueue)
				return;
			m_pPresentQueue->Signal(m_pFence.Get(), ++m_uFenceValue);
			Wait(m_uFenceValue);
		}

		[[nodiscard]] std::uint32_t LastFrame() const { return m_uLastFrame; }
		[[nodiscard]] ID3D12CommandQueue* QueueA() const { return m_pQueueA.Get(); }
		[[nodiscard]] ID3D12CommandQueue* QueueB() const { return m_pQueueB.Get(); }
		[[nodiscard]] ID3D12Device* Device() const { return m_pDevice.Get(); }

	private:
		void Wait(UINT64 uValue)
		{
			if (m_pFence->GetCompletedValue() >= uValue)
				return;
			m_pFence->SetEventOnCompletion(uValue, m_hEvent);
			WaitForSingleObject(m_hEvent, 5000);
		}

		ComPtr<ID3D12Device> m_pDevice;
		ComPtr<IDXGIFactory2> m_pFactory;
		std::array<ComPtr<ID3D12CommandQueue>, 2> m_aDecoys;
		ComPtr<ID3D12CommandQueue> m_pComputeDecoy;
		ComPtr<ID3D12CommandQueue> m_pQueueA;
		ComPtr<ID3D12CommandQueue> m_pQueueB;
		ID3D12CommandQueue* m_pPresentQueue = nullptr;
		ComPtr<IDXGISwapChain3> m_pSwapChain;
		std::array<ComPtr<ID3D12CommandAllocator>, 3> m_aAllocators;
		std::array<UINT64, 3> m_aSlotFence{};
		ComPtr<ID3D12GraphicsCommandList> m_pList;
		ComPtr<ID3D12CommandAllocator> m_pDecoyAllocator;
		ComPtr<ID3D12GraphicsCommandList> m_pDecoyList;
		ComPtr<ID3D12CommandAllocator> m_pComputeAllocator;
		ComPtr<ID3D12GraphicsCommandList> m_pComputeList;
		ComPtr<ID3D12DescriptorHeap> m_pHeap;
		ComPtr<ID3D12Fence> m_pFence;
		HANDLE m_hEvent = nullptr;
		UINT64 m_uFenceValue = 0;
		std::uint32_t m_uFrame = 0;
		std::uint32_t m_uLastFrame = 0;
	};

	enum class EFaultMode
	{
		None,
		CppException,
		AccessViolation
	};

	class clProbeListener final : public OmegaRenderHook::clRenderListener
	{
	public:
		void OnAttach(const OmegaRenderHook::stDeviceDX12& rDevice) override
		{
			++m_iAttaches;
			m_stLast12 = rDevice;
			m_pReadback12.Reset();
		}

		void OnFrame(const OmegaRenderHook::stFrameDX12& rFrame) override
		{
			++m_iFrames;
			m_stLast12 = *rFrame.pDevice;
			MaybeFault();

			const float aMagenta[4] = { 1.f, 0.f, 1.f, 1.f };
			const D3D12_RECT stRect{ 8, 8, 40, 40 };
			rFrame.pCommandList->ClearRenderTargetView(rFrame.hRenderTarget, aMagenta, 1, &stRect);

			const D3D12_RESOURCE_DESC stDesc = rFrame.pBackBuffer->GetDesc();
			UINT64 uTotal = 0;
			rFrame.pDevice->pDevice->GetCopyableFootprints(&stDesc, 0, 1, 0, &m_stFootprint, nullptr, nullptr, &uTotal);
			if (!m_pReadback12 || m_uReadbackBytes < uTotal)
			{
				D3D12_HEAP_PROPERTIES stHeap{ D3D12_HEAP_TYPE_READBACK };
				D3D12_RESOURCE_DESC stBuffer{};
				stBuffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				stBuffer.Width = uTotal;
				stBuffer.Height = 1;
				stBuffer.DepthOrArraySize = 1;
				stBuffer.MipLevels = 1;
				stBuffer.SampleDesc.Count = 1;
				stBuffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				m_pReadback12.Reset();
				rFrame.pDevice->pDevice->CreateCommittedResource(&stHeap, D3D12_HEAP_FLAG_NONE, &stBuffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_pReadback12));
				m_uReadbackBytes = uTotal;
			}

			D3D12_RESOURCE_BARRIER stBarrier{};
			stBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			stBarrier.Transition.pResource = rFrame.pBackBuffer;
			stBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			stBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			stBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			rFrame.pCommandList->ResourceBarrier(1, &stBarrier);
			D3D12_TEXTURE_COPY_LOCATION stSource{ rFrame.pBackBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
			stSource.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION stDest{ m_pReadback12.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
			stDest.PlacedFootprint = m_stFootprint;
			rFrame.pCommandList->CopyTextureRegion(&stDest, 0, 0, 0, &stSource, nullptr);
			std::swap(stBarrier.Transition.StateBefore, stBarrier.Transition.StateAfter);
			rFrame.pCommandList->ResourceBarrier(1, &stBarrier);
		}

		void OnAttach(const OmegaRenderHook::stDeviceDX11& rDevice) override
		{
			++m_iAttaches;
			m_stLast11 = rDevice;
			m_pStaging11.Reset();
		}

		void OnFrame(const OmegaRenderHook::stFrameDX11& rFrame) override
		{
			++m_iFrames;
			m_stLast11 = *rFrame.pDevice;
			MaybeFault();

			ComPtr<ID3D11DeviceContext1> pContext1;
			rFrame.pDevice->pContext->QueryInterface(IID_PPV_ARGS(&pContext1));
			const float aMagenta[4] = { 1.f, 0.f, 1.f, 1.f };
			const D3D11_RECT stRect{ 8, 8, 40, 40 };
			if (pContext1)
				pContext1->ClearView(rFrame.pRenderTarget, aMagenta, &stRect, 1);

			D3D11_TEXTURE2D_DESC stDesc{};
			rFrame.pBackBuffer->GetDesc(&stDesc);
			if (!m_pStaging11)
			{
				stDesc.Usage = D3D11_USAGE_STAGING;
				stDesc.BindFlags = 0;
				stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				stDesc.MiscFlags = 0;
				rFrame.pDevice->pDevice->CreateTexture2D(&stDesc, nullptr, &m_pStaging11);
			}
			rFrame.pDevice->pContext->CopyResource(m_pStaging11.Get(), rFrame.pBackBuffer);
		}

		void OnDetach(ERenderApi, OmegaRenderHook::EDetachReason) override
		{
			++m_iDetaches;
		}

		bool OnWindowMessage(HWND, UINT uMessage, WPARAM, LPARAM, LRESULT& rResult) override
		{
			if (uMessage != WM_APP + 2)
				return false;
			++m_iMessages;
			rResult = 0x5678;
			return true;
		}

		[[nodiscard]] std::uint32_t Pixel12(UINT uX, UINT uY) const
		{
			if (!m_pReadback12)
				return 0;
			void* pData = nullptr;
			const D3D12_RANGE stRange{ 0, static_cast<SIZE_T>(m_uReadbackBytes) };
			if (FAILED(m_pReadback12->Map(0, &stRange, &pData)))
				return 0;
			std::uint32_t uPixel = 0;
			std::memcpy(&uPixel, static_cast<const std::uint8_t*>(pData) + m_stFootprint.Offset + uY * m_stFootprint.Footprint.RowPitch + uX * 4, 4);
			const D3D12_RANGE stNone{ 0, 0 };
			m_pReadback12->Unmap(0, &stNone);
			return uPixel;
		}

		[[nodiscard]] std::uint32_t Pixel11(ID3D11DeviceContext* pContext, UINT uX, UINT uY) const
		{
			if (!m_pStaging11)
				return 0;
			D3D11_MAPPED_SUBRESOURCE stMapped{};
			if (FAILED(pContext->Map(m_pStaging11.Get(), 0, D3D11_MAP_READ, 0, &stMapped)))
				return 0;
			std::uint32_t uPixel = 0;
			std::memcpy(&uPixel, static_cast<const std::uint8_t*>(stMapped.pData) + uY * stMapped.RowPitch + uX * 4, 4);
			pContext->Unmap(m_pStaging11.Get(), 0);
			return uPixel;
		}

		void SetFault(EFaultMode eMode, int iAfterFrames)
		{
			m_eFault = eMode;
			m_iFaultAt = m_iFrames + iAfterFrames;
		}

		int m_iAttaches = 0;
		int m_iDetaches = 0;
		int m_iFrames = 0;
		int m_iMessages = 0;
		OmegaRenderHook::stDeviceDX12 m_stLast12;
		OmegaRenderHook::stDeviceDX11 m_stLast11;

	private:
		void MaybeFault()
		{
			if (m_eFault == EFaultMode::None || m_iFrames < m_iFaultAt)
				return;
			const EFaultMode eMode = m_eFault;
			m_eFault = EFaultMode::None;
			if (eMode == EFaultMode::CppException)
				throw std::runtime_error("self-test listener exception");
			*static_cast<volatile int*>(m_pNull) = 1;
		}

		ComPtr<ID3D12Resource> m_pReadback12;
		UINT64 m_uReadbackBytes = 0;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_stFootprint{};
		ComPtr<ID3D11Texture2D> m_pStaging11;
		EFaultMode m_eFault = EFaultMode::None;
		int m_iFaultAt = 0;
		void* m_pNull = nullptr;
	};

	[[nodiscard]] bool InstallPipeline(clProbeListener& rListener, EHookMethod eMethod, bool bDX11 = true, bool bDX12 = true)
	{
		OmegaRenderHook::stPipelineConfig stConfig;
		stConfig.pListener = &rListener;
		stConfig.eHookMethod = eMethod;
		stConfig.bDX11 = bDX11;
		stConfig.bDX12 = bDX12;
		auto expInstalled = OmegaRenderHook::Pipeline::Install(stConfig);
		Check(expInstalled.has_value(), "pipeline installs", expInstalled ? "" : expInstalled.error());
		return expInstalled.has_value();
	}

	[[nodiscard]] bool RunFrames(clFakeGameDX12& rGame, int iFrames)
	{
		bool bOk = true;
		for (int i = 0; i < iFrames; ++i)
		{
			bOk = SUCCEEDED(rGame.Frame()) && bOk;
			clTestWindow::Pump();
		}
		rGame.Flush();
		return bOk;
	}

	void CheckPixels12(clFakeGameDX12& rGame, clProbeListener& rListener, std::string_view strWhen)
	{
		const std::uint32_t uOverlay = rListener.Pixel12(uProbeX, uProbeY);
		const std::uint32_t uGame = rListener.Pixel12(uGameX, uGameY);
		const std::uint32_t uExpected = PackBytes(GameColorBytes(rGame.LastFrame()));
		Check(uOverlay == uMagenta, std::format("overlay pixel drawn ({})", strWhen), std::format("got 0x{:08X}", uOverlay));
		Check(uGame == uExpected, std::format("overlay ran after this frame's game rendering ({})", strWhen), std::format("got 0x{:08X}, expected 0x{:08X}", uGame, uExpected));
	}

	void TestDX12(EHookMethod eMethod)
	{
		std::println("Direct3D 12 ({} hooks)", OmegaRenderHook::ToString(eMethod));
		clTestWindow clWindow;
		clFakeGameDX12 clGame;
		if (!clGame.Create())
		{
			Check(false, "create Direct3D 12 test device");
			return;
		}

		Check(SUCCEEDED(clGame.CreateSwapChain(clWindow.Handle(), clGame.QueueA(), DXGI_FORMAT_R8G8B8A8_UNORM, 320, 240)), "game swap chain created before install");
		static_cast<void>(RunFrames(clGame, 3));

		clProbeListener clListener;
		if (!InstallPipeline(clListener, eMethod))
			return;

		Check(RunFrames(clGame, 6), "presents succeed with the overlay active");
		OmegaRenderHook::stPipelineStatus stStatus = OmegaRenderHook::Pipeline::GetStatus();
		Check(stStatus.eApi == ERenderApi::DX12, "late attach detected Direct3D 12");
		Check(clListener.m_stLast12.pQueue == clGame.QueueA(), "late attach resolved the swap chain's own queue despite decoy queues", std::format("source {}", OmegaRenderHook::ToString(stStatus.eQueueSource)));
		Check(stStatus.eQueueSource == EQueueSource::SwapChainDevice || stStatus.eQueueSource == EQueueSource::ObjectScan, "late attach queue source", OmegaRenderHook::ToString(stStatus.eQueueSource));
		std::println("  [info] late attach queue source: {}", OmegaRenderHook::ToString(stStatus.eQueueSource));
		Check(clListener.m_iFrames >= 5, "overlay frames rendered", std::format("{}", clListener.m_iFrames));
		CheckPixels12(clGame, clListener, "late attach");

		Check(SUCCEEDED(clGame.Resize(400, 300)), "game ResizeBuffers succeeds while hooked");
		static_cast<void>(RunFrames(clGame, 4));
		Check(clListener.m_stLast12.uWidth == 400 && clListener.m_stLast12.uHeight == 300, "overlay follows the resize");
		CheckPixels12(clGame, clListener, "after resize");

		const int iAttachesBefore = clListener.m_iAttaches;
		clGame.ReleaseSwapChain();
		const HRESULT hRecreate = clGame.CreateSwapChain(clWindow.Handle(), clGame.QueueB(), DXGI_FORMAT_R10G10B10A2_UNORM, 320, 240);
		Check(SUCCEEDED(hRecreate), "game recreates its swap chain on the same window (no leaked references)", std::format("hr 0x{:08X}", static_cast<std::uint32_t>(hRecreate)));
		if (SUCCEEDED(hRecreate))
		{
			static_cast<void>(RunFrames(clGame, 4));
			stStatus = OmegaRenderHook::Pipeline::GetStatus();
			Check(clListener.m_iAttaches == iAttachesBefore + 1 && clListener.m_iDetaches >= 1, "listener detached and re-attached");
			Check(clListener.m_stLast12.pQueue == clGame.QueueB(), "recreated swap chain resolved to its new queue");
			Check(stStatus.eQueueSource == EQueueSource::CreationHook, "recreated swap chain queue came from the creation hook", OmegaRenderHook::ToString(stStatus.eQueueSource));
			Check(clListener.m_stLast12.eFormat == DXGI_FORMAT_R10G10B10A2_UNORM, "overlay reports the new back buffer format");

			const int iFramesBefore = clListener.m_iFrames;
			std::thread thrPresent([&] { static_cast<void>(RunFrames(clGame, 8)); });
			thrPresent.join();
			Check(clListener.m_iFrames >= iFramesBefore + 8, "presenting from another thread (frame generation style) still renders");
		}

		Check(SendMessageW(clWindow.Handle(), WM_APP + 2, 0, 0) == 0x5678 && clListener.m_iMessages == 1, "window messages reach the listener");
		Check(SendMessageW(clWindow.Handle(), WM_APP + 1, 0, 0) == 0x1234, "unhandled window messages reach the game");

		Check(OmegaRenderHook::Pipeline::Uninstall(), "uninstall restores every hook and drains");
		Check(GetWindowLongPtrW(clWindow.Handle(), GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&clTestWindow::WindowProc), "window procedure restored");
		const int iFramesAfter = clListener.m_iFrames;
		Check(RunFrames(clGame, 3) && clListener.m_iFrames == iFramesAfter, "no overlay frames after uninstall");

		clGame.ReleaseSwapChain();
		clProbeListener clEarly;
		if (!InstallPipeline(clEarly, eMethod))
			return;
		Check(SUCCEEDED(clGame.CreateSwapChain(clWindow.Handle(), clGame.QueueA(), DXGI_FORMAT_R8G8B8A8_UNORM, 320, 240)), "swap chain created after install");
		static_cast<void>(RunFrames(clGame, 5));
		stStatus = OmegaRenderHook::Pipeline::GetStatus();
		Check(clEarly.m_stLast12.pQueue == clGame.QueueA() && stStatus.eQueueSource == EQueueSource::CreationHook, "early install resolves the queue from the creation hook", OmegaRenderHook::ToString(stStatus.eQueueSource));
		CheckPixels12(clGame, clEarly, "early install");

		clEarly.SetFault(EFaultMode::CppException, 1);
		Check(RunFrames(clGame, 4), "game keeps presenting after the listener throws");
		Check(OmegaRenderHook::Pipeline::GetStatus().bListenerFaulted, "C++ exception in the listener is contained and reported");
		Check(OmegaRenderHook::Pipeline::Uninstall(), "uninstall after a listener fault");

		clProbeListener clCrash;
		if (!InstallPipeline(clCrash, eMethod))
			return;
		clCrash.SetFault(EFaultMode::AccessViolation, 1);
		Check(RunFrames(clGame, 4), "game keeps presenting after the listener access-violates");
		Check(OmegaRenderHook::Pipeline::GetStatus().bListenerFaulted, "access violation in the listener is contained and reported");
		Check(OmegaRenderHook::Pipeline::Uninstall(), "uninstall after an access violation");
	}

	void TestDX11()
	{
		std::println("Direct3D 11");
		clTestWindow clWindow;
		ComPtr<ID3D11Device> pDevice;
		ComPtr<ID3D11DeviceContext> pContext;
		ComPtr<IDXGIFactory2> pFactory;
		if (FAILED(D3D11CreateDevice(nullptr, g_bWarp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &pDevice, nullptr, &pContext)) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory))))
		{
			Check(false, "create Direct3D 11 test device");
			return;
		}

		DXGI_SWAP_CHAIN_DESC1 stDesc{};
		stDesc.Width = 320;
		stDesc.Height = 240;
		stDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		stDesc.SampleDesc.Count = 1;
		stDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		stDesc.BufferCount = 2;
		stDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		ComPtr<IDXGISwapChain1> pSwapChain;
		Check(SUCCEEDED(pFactory->CreateSwapChainForHwnd(pDevice.Get(), clWindow.Handle(), &stDesc, nullptr, nullptr, &pSwapChain)), "game swap chain created before install");

		clProbeListener clListener;
		if (!InstallPipeline(clListener, EHookMethod::VTable))
			return;

		D3D11_TEXTURE2D_DESC stOffscreen{};
		stOffscreen.Width = 64;
		stOffscreen.Height = 64;
		stOffscreen.MipLevels = 1;
		stOffscreen.ArraySize = 1;
		stOffscreen.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		stOffscreen.SampleDesc.Count = 1;
		stOffscreen.BindFlags = D3D11_BIND_RENDER_TARGET;
		ComPtr<ID3D11Texture2D> pOffscreen;
		ComPtr<ID3D11RenderTargetView> pOffscreenTarget;
		pDevice->CreateTexture2D(&stOffscreen, nullptr, &pOffscreen);
		pDevice->CreateRenderTargetView(pOffscreen.Get(), nullptr, &pOffscreenTarget);
		const D3D11_VIEWPORT stGameViewport{ 3.f, 5.f, 50.f, 40.f, 0.f, 1.f };

		std::uint32_t uFrame = 0;
		bool bRestored = true;
		for (int i = 0; i < 5; ++i, ++uFrame)
		{
			ComPtr<ID3D11Texture2D> pBuffer;
			pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBuffer));
			ComPtr<ID3D11RenderTargetView> pTarget;
			pDevice->CreateRenderTargetView(pBuffer.Get(), nullptr, &pTarget);
			const std::array<float, 4> aColor = GameColor(uFrame);
			pContext->ClearRenderTargetView(pTarget.Get(), aColor.data());
			pContext->OMSetRenderTargets(1, pOffscreenTarget.GetAddressOf(), nullptr);
			pContext->RSSetViewports(1, &stGameViewport);
			pSwapChain->Present(0, 0);
			ComPtr<ID3D11RenderTargetView> pBound;
			pContext->OMGetRenderTargets(1, &pBound, nullptr);
			D3D11_VIEWPORT stBoundViewport{};
			UINT uViewports = 1;
			pContext->RSGetViewports(&uViewports, &stBoundViewport);
			bRestored = bRestored && pBound.Get() == pOffscreenTarget.Get() && uViewports == 1 && stBoundViewport.TopLeftX == 3.f && stBoundViewport.Width == 50.f;
			clTestWindow::Pump();
		}

		const OmegaRenderHook::stPipelineStatus stStatus = OmegaRenderHook::Pipeline::GetStatus();
		Check(stStatus.eApi == ERenderApi::DX11 && clListener.m_iFrames >= 4, "late attach detected Direct3D 11 and rendered", std::format("{} frames", clListener.m_iFrames));
		Check(bRestored, "game render targets and viewport restored after the overlay");
		const std::uint32_t uOverlay = clListener.Pixel11(pContext.Get(), uProbeX, uProbeY);
		const std::uint32_t uGame = clListener.Pixel11(pContext.Get(), uGameX, uGameY);
		Check(uOverlay == uMagenta, "overlay pixel drawn", std::format("got 0x{:08X}", uOverlay));
		Check(uGame == PackBytes(GameColorBytes(uFrame - 1)), "game pixel preserved", std::format("got 0x{:08X}", uGame));
		Check(OmegaRenderHook::Pipeline::Uninstall(), "uninstall restores every hook and drains");
	}
}

int main(int iArgCount, char** pszArgs)
{
	for (int i = 1; i < iArgCount; ++i)
	{
		g_bVerbose = g_bVerbose || std::string_view(pszArgs[i]) == "--verbose";
		g_bWarp = g_bWarp || std::string_view(pszArgs[i]) == "--warp";
	}
	if (g_bWarp)
		std::println("Using the WARP software adapter");

	OmegaRenderHook::SetLogSink([](OmegaRenderHook::ELogLevel eLevel, std::string_view strMessage)
	{
		if (g_bVerbose || eLevel >= OmegaRenderHook::ELogLevel::Warning)
			std::println("  [log:{}] {}", OmegaRenderHook::ToString(eLevel), strMessage);
	});

	TestDX12(EHookMethod::VTable);
	TestDX12(EHookMethod::Inline);
	TestDX11();

	std::println("{}", g_iFailures == 0 ? "all self-tests passed" : std::format("{} self-test check(s) failed", g_iFailures));
	return g_iFailures == 0 ? 0 : 1;
}
