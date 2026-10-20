#include "OmegaRenderHook/Render/OpenGL/HookOpenGL.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Hook/InlineHook.hpp"
#include "OmegaRenderHook/Render/PipelineInternal.hpp"

#include <GL/gl.h>

#include <mutex>

namespace OmegaRenderHook::HookOpenGL
{
	namespace
	{
		using SwapBuffersFn = BOOL(WINAPI*)(HDC);

		struct stState
		{
			std::recursive_mutex mtxRender;
			bool bInstalled = false;
			clInlineHook clSwapBuffers;
			HGLRC hContext = nullptr;
			HWND hWindow = nullptr;
			stDeviceOpenGL stDevice;
			std::uint64_t uFrameNumber = 0;
			PipelineInternal::clDetachRequest clDetach;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		thread_local int t_iSwapDepth = 0;

		class clCurrentContext
		{
		public:
			clCurrentContext() : m_hDC(wglGetCurrentDC()), m_hContext(wglGetCurrentContext()) {}
			~clCurrentContext() { wglMakeCurrent(m_hDC, m_hContext); }

			clCurrentContext(const clCurrentContext&) = delete;
			clCurrentContext& operator=(const clCurrentContext&) = delete;

		private:
			HDC m_hDC;
			HGLRC m_hContext;
		};

		void DetachWithDC(stState& rState, HDC hDC, EDetachReason eReason)
		{
			if (!rState.hContext)
				return;
			{
				clCurrentContext clRestore;
				if (hDC && wglMakeCurrent(hDC, rState.hContext))
				{
					PipelineInternal::CallListener("OnDetach", [&](clRenderListener* pListener) { pListener->OnDetach(ERenderApi::OpenGL, eReason); });
					glFinish();
				}
				else
					Log(ELogLevel::Warning, "could not make the overlay OpenGL context current to release it");
			}
			wglDeleteContext(rState.hContext);
			rState.hContext = nullptr;
			rState.hWindow = nullptr;
			rState.stDevice = {};
			PipelineInternal::ReleaseApi(ERenderApi::OpenGL);
			Log(ELogLevel::Info, "OpenGL renderer detached ({})", ToString(eReason));
		}

		void DetachOutOfFrame(stState& rState, EDetachReason eReason)
		{
			if (!rState.hContext)
				return;
			const HWND hWindow = rState.hWindow;
			const HDC hDC = hWindow && IsWindow(hWindow) ? GetDC(hWindow) : nullptr;
			DetachWithDC(rState, hDC, eReason);
			if (hDC)
				ReleaseDC(hWindow, hDC);
		}

		void OnSwapBuffers(HDC hDC)
		{
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			if (rState.clDetach.IsRequested())
			{
				DetachWithDC(rState, hDC, EDetachReason::Uninstall);
				rState.clDetach.MarkDone();
				return;
			}
			if (!PipelineInternal::IsActive())
				return;

			const HWND hWindow = WindowFromDC(hDC);
			RECT rcClient{};
			if (!hWindow || !GetClientRect(hWindow, &rcClient))
				return;
			const UINT uWidth = static_cast<UINT>(rcClient.right - rcClient.left);
			const UINT uHeight = static_cast<UINT>(rcClient.bottom - rcClient.top);
			if (!uWidth || !uHeight || !PipelineInternal::AcquireWindow(hWindow, ERenderApi::OpenGL, uWidth, uHeight))
				return;

			if (rState.hContext && rState.hWindow != hWindow)
				DetachWithDC(rState, hDC, EDetachReason::SwapChainChanged);

			clCurrentContext clRestore;
			if (!rState.hContext)
			{
				rState.hContext = wglCreateContext(hDC);
				if (!rState.hContext || !wglMakeCurrent(hDC, rState.hContext))
				{
					if (rState.hContext)
						wglDeleteContext(rState.hContext);
					rState.hContext = nullptr;
					return;
				}
				rState.hWindow = hWindow;
				rState.stDevice = { hWindow, hDC, rState.hContext, uWidth, uHeight };
				Log(ELogLevel::Info, "OpenGL renderer attached: {}x{} ({})", uWidth, uHeight, reinterpret_cast<const char*>(glGetString(GL_VERSION)));
				PipelineInternal::CallListener("OnAttach", [&](clRenderListener* pListener) { pListener->OnAttach(rState.stDevice); });
			}
			else if (!wglMakeCurrent(hDC, rState.hContext))
			{
				PipelineInternal::CountFrame(false);
				return;
			}

			rState.stDevice.hDC = hDC;
			rState.stDevice.uWidth = uWidth;
			rState.stDevice.uHeight = uHeight;
			clRenderListener* pListener = PipelineInternal::Listener();
			if (!pListener)
				return;
			glViewport(0, 0, static_cast<GLsizei>(uWidth), static_cast<GLsizei>(uHeight));
			stFrameOpenGL stFrame{ &rState.stDevice, rState.uFrameNumber++ };
			auto fnFrame = [&] { pListener->OnFrame(stFrame); };
			stFault stFault;
			if (!InvokeGuarded(fnFrame, stFault))
				PipelineInternal::ReportFault("OnFrame", stFault);
			glFlush();
			PipelineInternal::CountFrame(true);
		}

		BOOL WINAPI HookSwapBuffers(HDC hDC)
		{
			clHookScope clScope;
			PipelineInternal::clLayeredScope clLayered;
			const auto pfnOriginal = State().clSwapBuffers.Original<SwapBuffersFn>();
			if (++t_iSwapDepth == 1 && hDC)
				OnSwapBuffers(hDC);
			const BOOL bResult = pfnOriginal(hDC);
			--t_iSwapDepth;
			return bResult;
		}
	}

	bool Install()
	{
		stState& rState = State();
		if (rState.bInstalled)
			return true;
		const HMODULE hOpenGL = GetModuleHandleW(L"opengl32.dll");
		void* pTarget = hOpenGL ? reinterpret_cast<void*>(GetProcAddress(hOpenGL, "wglSwapBuffers")) : nullptr;
		if (!pTarget)
		{
			Log(ELogLevel::Debug, "opengl32.dll is not loaded, OpenGL hooks skipped");
			return false;
		}
		if (!rState.clSwapBuffers.Install(pTarget, reinterpret_cast<void*>(&HookSwapBuffers)))
		{
			Log(ELogLevel::Error, "failed to hook wglSwapBuffers");
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
			if (!rState.hContext)
				return;
			rState.clDetach.Request();
		}
		static_cast<void>(rState.clDetach.WaitDone(msTimeout));
	}

	bool Disable()
	{
		return State().clSwapBuffers.Disable();
	}

	void Release(bool bDrained)
	{
		stState& rState = State();
		{
			std::scoped_lock lckRender(rState.mtxRender);
			DetachOutOfFrame(rState, EDetachReason::Uninstall);
			rState.clDetach.Clear();
		}
		if (bDrained)
			rState.clSwapBuffers.Uninstall();
		rState.bInstalled = false;
	}
}
