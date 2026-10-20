#include "OmegaRenderHook/Render/DX9/HookDX9.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Render/PipelineInternal.hpp"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>

namespace OmegaRenderHook::HookDX9
{
	namespace
	{
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		namespace Slots
		{
			constexpr std::size_t uReset = 16;
			constexpr std::size_t uPresent = 17;
			constexpr std::size_t uPresentEx = 121;
			constexpr std::size_t uResetEx = 132;
		}

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
		using PresentExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
		using ResetExFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

		constexpr std::size_t uKinds = 2;

		struct stHooks
		{
			clMethodHook clPresent;
			clMethodHook clReset;
			clMethodHook clPresentEx;
			clMethodHook clResetEx;
		};

		struct stState
		{
			std::recursive_mutex mtxRender;
			bool bInstalled = false;
			std::array<stHooks, uKinds> aHooks;
			IDirect3DDevice9* pDevice = nullptr;
			ComPtr<IDirect3DStateBlock9> pStateBlock;
			stDeviceDX9 stDevice;
			std::uint64_t uFrameNumber = 0;
			PipelineInternal::clDetachRequest clDetach;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		thread_local int t_iPresentDepth = 0;

		void DetachLocked(stState& rState, EDetachReason eReason)
		{
			if (!rState.pDevice)
				return;
			PipelineInternal::CallListener("OnDetach", [&](clRenderListener* pListener) { pListener->OnDetach(ERenderApi::DX9, eReason); });
			rState.pStateBlock.Reset();
			rState.pDevice = nullptr;
			rState.stDevice = {};
			PipelineInternal::ReleaseApi(ERenderApi::DX9);
			Log(ELogLevel::Info, "Direct3D 9 renderer detached ({})", ToString(eReason));
		}

		[[nodiscard]] bool ReadPresentParameters(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS& rParameters)
		{
			ComPtr<IDirect3DSwapChain9> pSwapChain;
			if (FAILED(pDevice->GetSwapChain(0, &pSwapChain)) || FAILED(pSwapChain->GetPresentParameters(&rParameters)))
				return false;
			if (!rParameters.hDeviceWindow)
			{
				D3DDEVICE_CREATION_PARAMETERS stCreation{};
				if (SUCCEEDED(pDevice->GetCreationParameters(&stCreation)))
					rParameters.hDeviceWindow = stCreation.hFocusWindow;
			}
			return rParameters.hDeviceWindow != nullptr;
		}

		void OnPresent(IDirect3DDevice9* pDevice, HWND hOverride)
		{
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			if (rState.clDetach.IsRequested())
			{
				DetachLocked(rState, EDetachReason::Uninstall);
				rState.clDetach.MarkDone();
				return;
			}
			if (!PipelineInternal::IsActive() || pDevice->TestCooperativeLevel() != D3D_OK)
				return;

			D3DPRESENT_PARAMETERS stParameters{};
			if (!ReadPresentParameters(pDevice, stParameters))
				return;
			const HWND hWindow = hOverride ? hOverride : stParameters.hDeviceWindow;
			UINT uWidth = stParameters.BackBufferWidth;
			UINT uHeight = stParameters.BackBufferHeight;
			if (!uWidth || !uHeight)
			{
				RECT rcClient{};
				GetClientRect(hWindow, &rcClient);
				uWidth = static_cast<UINT>(rcClient.right - rcClient.left);
				uHeight = static_cast<UINT>(rcClient.bottom - rcClient.top);
			}
			if (!PipelineInternal::AcquireWindow(hWindow, ERenderApi::DX9, uWidth, uHeight))
				return;

			if (rState.pDevice != pDevice)
			{
				DetachLocked(rState, EDetachReason::SwapChainChanged);
				if (FAILED(pDevice->CreateStateBlock(D3DSBT_ALL, &rState.pStateBlock)))
					return;
				rState.pDevice = pDevice;
				rState.stDevice = { pDevice, hWindow, stParameters.BackBufferFormat, uWidth, uHeight };
				Log(ELogLevel::Info, "Direct3D 9 renderer attached: {}x{} format {}", uWidth, uHeight, static_cast<int>(stParameters.BackBufferFormat));
				PipelineInternal::CallListener("OnAttach", [&](clRenderListener* pListener) { pListener->OnAttach(rState.stDevice); });
			}
			rState.stDevice.hWindow = hWindow;
			rState.stDevice.uWidth = uWidth;
			rState.stDevice.uHeight = uHeight;

			clRenderListener* pListener = PipelineInternal::Listener();
			if (!pListener)
				return;

			ComPtr<IDirect3DSurface9> pBackBuffer;
			ComPtr<IDirect3DSurface9> pSavedTarget;
			if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || FAILED(pDevice->GetRenderTarget(0, &pSavedTarget)))
			{
				PipelineInternal::CountFrame(false);
				return;
			}

			ComPtr<IDirect3DSurface9> pDepth;
			D3DSURFACE_DESC stBackDesc{};
			D3DSURFACE_DESC stDepthDesc{};
			if (FAILED(pDevice->GetDepthStencilSurface(&pDepth)) || !pDepth || FAILED(pBackBuffer->GetDesc(&stBackDesc)) || FAILED(pDepth->GetDesc(&stDepthDesc)) ||
				stDepthDesc.Width != stBackDesc.Width || stDepthDesc.Height != stBackDesc.Height || stDepthDesc.MultiSampleType != stBackDesc.MultiSampleType)
				pDepth.Reset();

			rState.pStateBlock->Capture();
			pDevice->SetRenderTarget(0, pBackBuffer.Get());
			const D3DVIEWPORT9 stViewport{ 0, 0, uWidth, uHeight, 0.f, 1.f };
			pDevice->SetViewport(&stViewport);
			bool bRendered = false;
			if (SUCCEEDED(pDevice->BeginScene()))
			{
				stFrameDX9 stFrame{ &rState.stDevice, rState.uFrameNumber++, pDepth.Get() };
				auto fnFrame = [&] { pListener->OnFrame(stFrame); };
				stFault stFault;
				if (!InvokeGuarded(fnFrame, stFault))
					PipelineInternal::ReportFault("OnFrame", stFault);
				pDevice->EndScene();
				bRendered = true;
			}
			pDevice->SetRenderTarget(0, pSavedTarget.Get());
			rState.pStateBlock->Apply();
			PipelineInternal::CountFrame(bRendered);
		}

		void OnBeforeReset(IDirect3DDevice9* pDevice)
		{
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			if (rState.pDevice == pDevice)
				DetachLocked(rState, EDetachReason::SwapChainChanged);
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* pDevice, const RECT* pSource, const RECT* pDest, HWND hOverride, const RGNDATA* pDirty)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aHooks[uKind].clPresent.Original<PresentFn>();
			if (++t_iPresentDepth == 1)
				OnPresent(pDevice, hOverride);
			const HRESULT hResult = pfnOriginal(pDevice, pSource, pDest, hOverride, pDirty);
			--t_iPresentDepth;
			return hResult;
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookPresentEx(IDirect3DDevice9Ex* pDevice, const RECT* pSource, const RECT* pDest, HWND hOverride, const RGNDATA* pDirty, DWORD uFlags)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aHooks[uKind].clPresentEx.Original<PresentExFn>();
			if (++t_iPresentDepth == 1 && !(uFlags & D3DPRESENT_DONOTWAIT))
				OnPresent(pDevice, hOverride);
			const HRESULT hResult = pfnOriginal(pDevice, pSource, pDest, hOverride, pDirty, uFlags);
			--t_iPresentDepth;
			return hResult;
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pParameters)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aHooks[uKind].clReset.Original<ResetFn>();
			OnBeforeReset(pDevice);
			return pfnOriginal(pDevice, pParameters);
		}

		template <std::size_t uKind>
		HRESULT STDMETHODCALLTYPE HookResetEx(IDirect3DDevice9Ex* pDevice, D3DPRESENT_PARAMETERS* pParameters, D3DDISPLAYMODEEX* pMode)
		{
			clHookScope clScope;
			const auto pfnOriginal = State().aHooks[uKind].clResetEx.Original<ResetExFn>();
			OnBeforeReset(pDevice);
			return pfnOriginal(pDevice, pParameters, pMode);
		}

		class clDummyWindow
		{
		public:
			clDummyWindow()
			{
				WNDCLASSEXW stClass{ sizeof(WNDCLASSEXW) };
				stClass.lpfnWndProc = DefWindowProcW;
				stClass.hInstance = GetModuleHandleW(nullptr);
				stClass.lpszClassName = L"OmegaRenderHookDummyWindow9";
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

		constexpr std::size_t uTableSize = Slots::uResetEx + 1;
		using FunctionTable = std::array<void*, uTableSize>;

		void CopyTable(void* pObject, std::size_t uCount, FunctionTable& rTable)
		{
			void** ppVTable = *static_cast<void***>(pObject);
			for (std::size_t i = 0; i < uCount; ++i)
				rTable[i] = ppVTable[i];
		}

		[[nodiscard]] std::array<bool, uKinds> CaptureVTables(std::array<FunctionTable, uKinds>& rTables)
		{
			std::array<bool, uKinds> aTables{};
			const HMODULE hD3D9 = GetModuleHandleW(L"d3d9.dll");
			if (!hD3D9)
				return aTables;
			using Direct3DCreate9ExFn = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
			const auto pfnCreateEx = reinterpret_cast<Direct3DCreate9ExFn>(GetProcAddress(hD3D9, "Direct3DCreate9Ex"));
			const auto pfnCreate = reinterpret_cast<decltype(&Direct3DCreate9)>(GetProcAddress(hD3D9, "Direct3DCreate9"));

			clDummyWindow clWindow;
			if (!clWindow.Handle())
				return aTables;
			D3DPRESENT_PARAMETERS stParameters{};
			stParameters.Windowed = TRUE;
			stParameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
			stParameters.BackBufferFormat = D3DFMT_UNKNOWN;
			stParameters.hDeviceWindow = clWindow.Handle();
			constexpr DWORD uFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT;

			if (pfnCreate)
			{
				ComPtr<IDirect3D9> pD3D;
				pD3D.Attach(pfnCreate(D3D_SDK_VERSION));
				ComPtr<IDirect3DDevice9> pDevice;
				if (pD3D && (SUCCEEDED(pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, clWindow.Handle(), uFlags, &stParameters, &pDevice)) ||
					SUCCEEDED(pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, clWindow.Handle(), uFlags, &stParameters, &pDevice))))
				{
					CopyTable(pDevice.Get(), Slots::uPresent + 1, rTables[0]);
					aTables[0] = true;
				}
			}
			if (pfnCreateEx)
			{
				ComPtr<IDirect3D9Ex> pD3D;
				ComPtr<IDirect3DDevice9Ex> pDevice;
				if (SUCCEEDED(pfnCreateEx(D3D_SDK_VERSION, &pD3D)) && SUCCEEDED(pD3D->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, clWindow.Handle(), uFlags, &stParameters, nullptr, &pDevice)))
				{
					CopyTable(pDevice.Get(), uTableSize, rTables[1]);
					aTables[1] = true;
				}
			}
			return aTables;
		}

		template <std::size_t uKind>
		bool InstallKind(FunctionTable& rTable, std::vector<void*>& rIdentities)
		{
			stHooks& rHooks = State().aHooks[uKind];
			const auto fnInstall = [&](clMethodHook& rHook, std::size_t uSlot, void* pDetour) {
				void* pIdentity = rTable[uSlot];
				if (!pIdentity || std::find(rIdentities.begin(), rIdentities.end(), pIdentity) != rIdentities.end())
					return true;
				if (!rHook.Install(EHookMethod::Inline, rTable.data(), uSlot, pDetour))
					return false;
				rIdentities.push_back(pIdentity);
				return true;
			};
			bool bInstalled = fnInstall(rHooks.clPresent, Slots::uPresent, reinterpret_cast<void*>(&HookPresent<uKind>));
			bInstalled = fnInstall(rHooks.clReset, Slots::uReset, reinterpret_cast<void*>(&HookReset<uKind>)) && bInstalled;
			if (uKind == 1)
			{
				bInstalled = fnInstall(rHooks.clPresentEx, Slots::uPresentEx, reinterpret_cast<void*>(&HookPresentEx<uKind>)) && bInstalled;
				bInstalled = fnInstall(rHooks.clResetEx, Slots::uResetEx, reinterpret_cast<void*>(&HookResetEx<uKind>)) && bInstalled;
			}
			return bInstalled;
		}

		template <typename F>
		void ForEachHook(F&& fnVisit)
		{
			for (stHooks& rHooks : State().aHooks)
			{
				fnVisit(rHooks.clPresent);
				fnVisit(rHooks.clReset);
				fnVisit(rHooks.clPresentEx);
				fnVisit(rHooks.clResetEx);
			}
		}
	}

	bool Install(EHookMethod)
	{
		stState& rState = State();
		if (rState.bInstalled)
			return true;
		if (!GetModuleHandleW(L"d3d9.dll"))
		{
			Log(ELogLevel::Debug, "d3d9.dll is not loaded, Direct3D 9 hooks skipped");
			return false;
		}
		std::array<FunctionTable, uKinds> aTables{};
		const std::array<bool, uKinds> aCaptured = CaptureVTables(aTables);
		if (!aCaptured[0] && !aCaptured[1])
		{
			Log(ELogLevel::Warning, "could not create a dummy Direct3D 9 device, Direct3D 9 hooks skipped");
			return false;
		}
		std::vector<void*> aIdentities;
		bool bInstalled = true;
		if (aCaptured[0])
			bInstalled = InstallKind<0>(aTables[0], aIdentities) && bInstalled;
		if (aCaptured[1])
			bInstalled = InstallKind<1>(aTables[1], aIdentities) && bInstalled;
		if (!bInstalled)
		{
			ForEachHook([](clMethodHook& rHook) { rHook.Disable(); rHook.Release(); });
			Log(ELogLevel::Error, "failed to hook Direct3D 9");
			return false;
		}
		rState.clDetach.Clear();
		rState.bInstalled = true;
		return true;
	}

	void RequestDetach(std::chrono::milliseconds msTimeout)
	{
		stState& rState = State();
		{
			std::scoped_lock lckRender(rState.mtxRender);
			if (!rState.pDevice)
				return;
			rState.clDetach.Request();
		}
		static_cast<void>(rState.clDetach.WaitDone(msTimeout));
	}

	bool Disable()
	{
		bool bRestored = true;
		ForEachHook([&](clMethodHook& rHook) { bRestored = rHook.Disable() && bRestored; });
		return bRestored;
	}

	void Release(bool bDrained)
	{
		stState& rState = State();
		{
			std::scoped_lock lckRender(rState.mtxRender);
			DetachLocked(rState, EDetachReason::Uninstall);
			rState.clDetach.Clear();
		}
		if (bDrained)
			ForEachHook([](clMethodHook& rHook) { rHook.Release(); });
		rState.bInstalled = false;
	}
}
