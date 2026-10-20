#include "OmegaRenderHook/Window/WindowHook.hpp"
#include "OmegaRenderHook/Core/Guarded.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Render/Pipeline.hpp"

#include <array>
#include <atomic>
#include <bitset>

namespace OmegaRenderHook::WindowHook
{
	namespace
	{
		std::atomic<HWND> s_hWindow{ nullptr };
		std::atomic<WNDPROC> s_pfnOriginal{ nullptr };
		std::atomic<clRenderListener*> s_pListener{ nullptr };
		std::atomic<bool> s_bUnicode{ true };

		constexpr WPARAM uReleaseKeyboard = 1;
		constexpr WPARAM uReleaseMouse = 2;

		struct stButton
		{
			UINT uDown;
			UINT uDoubleClick;
			UINT uUp;
			WPARAM uMask;
			WORD uXButton;
		};

		constexpr std::array<stButton, 5> kButtons = { {
			{ WM_LBUTTONDOWN, WM_LBUTTONDBLCLK, WM_LBUTTONUP, MK_LBUTTON, 0 },
			{ WM_RBUTTONDOWN, WM_RBUTTONDBLCLK, WM_RBUTTONUP, MK_RBUTTON, 0 },
			{ WM_MBUTTONDOWN, WM_MBUTTONDBLCLK, WM_MBUTTONUP, MK_MBUTTON, 0 },
			{ WM_XBUTTONDOWN, WM_XBUTTONDBLCLK, WM_XBUTTONUP, MK_XBUTTON1, XBUTTON1 },
			{ WM_XBUTTONDOWN, WM_XBUTTONDBLCLK, WM_XBUTTONUP, MK_XBUTTON2, XBUTTON2 },
		} };

		struct stHeldInput
		{
			std::bitset<256> bsKeys;
			std::array<LPARAM, 256> aKeyParams{};
			std::array<bool, 256> aSystemKeys{};
			std::bitset<kButtons.size()> bsButtons;
		};

		stHeldInput s_stHeld;

		[[nodiscard]] UINT ReleaseMessage()
		{
			static const UINT s_uMessage = RegisterWindowMessageW(L"OmegaRenderHook.ReleaseHeldInput");
			return s_uMessage;
		}

		[[nodiscard]] int ButtonIndex(UINT uMessage, WPARAM uWParam, bool& rbDown)
		{
			for (std::size_t i = 0; i < kButtons.size(); ++i)
			{
				const stButton& rButton = kButtons[i];
				if (rButton.uXButton && GET_XBUTTON_WPARAM(uWParam) != rButton.uXButton)
					continue;
				if (uMessage == rButton.uDown || uMessage == rButton.uDoubleClick || uMessage == rButton.uUp)
				{
					rbDown = uMessage != rButton.uUp;
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		void TrackDelivered(UINT uMessage, WPARAM uWParam, LPARAM iLParam)
		{
			const std::size_t uKey = uWParam & 0xFF;
			switch (uMessage)
			{
			case WM_KEYDOWN:
			case WM_SYSKEYDOWN:
				s_stHeld.bsKeys.set(uKey);
				s_stHeld.aKeyParams[uKey] = iLParam;
				s_stHeld.aSystemKeys[uKey] = uMessage == WM_SYSKEYDOWN;
				return;
			case WM_KEYUP:
			case WM_SYSKEYUP:
				s_stHeld.bsKeys.reset(uKey);
				return;
			default:
				break;
			}
			bool bDown = false;
			if (const int iButton = ButtonIndex(uMessage, uWParam, bDown); iButton >= 0)
				s_stHeld.bsButtons.set(static_cast<std::size_t>(iButton), bDown);
		}

		[[nodiscard]] bool IsHeldRelease(UINT uMessage, WPARAM uWParam)
		{
			if (uMessage == WM_KEYUP || uMessage == WM_SYSKEYUP)
				return s_stHeld.bsKeys.test(uWParam & 0xFF);
			bool bDown = false;
			const int iButton = ButtonIndex(uMessage, uWParam, bDown);
			return iButton >= 0 && !bDown && s_stHeld.bsButtons.test(static_cast<std::size_t>(iButton));
		}

		LRESULT CallOriginal(WNDPROC pfnOriginal, bool bUnicode, HWND hWindow, UINT uMessage, WPARAM uWParam, LPARAM iLParam)
		{
			if (!pfnOriginal)
				return bUnicode ? DefWindowProcW(hWindow, uMessage, uWParam, iLParam) : DefWindowProcA(hWindow, uMessage, uWParam, iLParam);
			return bUnicode ? CallWindowProcW(pfnOriginal, hWindow, uMessage, uWParam, iLParam) : CallWindowProcA(pfnOriginal, hWindow, uMessage, uWParam, iLParam);
		}

		void ReleaseHeld(WNDPROC pfnOriginal, bool bUnicode, HWND hWindow, WPARAM uWhat)
		{
			if (uWhat & uReleaseKeyboard)
			{
				for (std::size_t uKey = 0; uKey < s_stHeld.bsKeys.size(); ++uKey)
				{
					if (!s_stHeld.bsKeys.test(uKey))
						continue;
					s_stHeld.bsKeys.reset(uKey);
					const LPARAM iParam = (s_stHeld.aKeyParams[uKey] & 0x01FF0000) | 1 | static_cast<LPARAM>(0xC0000000u);
					CallOriginal(pfnOriginal, bUnicode, hWindow, s_stHeld.aSystemKeys[uKey] ? WM_SYSKEYUP : WM_KEYUP, uKey, iParam);
				}
			}
			if ((uWhat & uReleaseMouse) && s_stHeld.bsButtons.any())
			{
				POINT stCursor{};
				GetCursorPos(&stCursor);
				ScreenToClient(hWindow, &stCursor);
				const LPARAM iPosition = MAKELPARAM(stCursor.x, stCursor.y);
				for (std::size_t i = 0; i < kButtons.size(); ++i)
				{
					if (!s_stHeld.bsButtons.test(i))
						continue;
					s_stHeld.bsButtons.reset(i);
					WPARAM uMask = 0;
					for (std::size_t j = 0; j < kButtons.size(); ++j)
						if (s_stHeld.bsButtons.test(j))
							uMask |= kButtons[j].uMask;
					const WPARAM uParam = kButtons[i].uXButton ? MAKEWPARAM(uMask, kButtons[i].uXButton) : uMask;
					CallOriginal(pfnOriginal, bUnicode, hWindow, kButtons[i].uUp, uParam, iPosition);
				}
			}
		}

		LRESULT CALLBACK WindowProc(HWND hWindow, UINT uMessage, WPARAM uWParam, LPARAM iLParam)
		{
			clHookScope clScope;
			const WNDPROC pfnOriginal = s_pfnOriginal.load(std::memory_order_acquire);
			const bool bUnicode = s_bUnicode.load(std::memory_order_relaxed);

			if (uMessage == ReleaseMessage())
			{
				ReleaseHeld(pfnOriginal, bUnicode, hWindow, uWParam);
				return 0;
			}

			if (clRenderListener* pListener = s_pListener.load(std::memory_order_acquire))
			{
				LRESULT iResult = 0;
				bool bHandled = false;
				auto fnMessage = [&] { bHandled = pListener->OnWindowMessage(hWindow, uMessage, uWParam, iLParam, iResult); };
				stFault stFault;
				if (!InvokeGuarded(fnMessage, stFault))
				{
					s_pListener.store(nullptr, std::memory_order_release);
					Log(ELogLevel::Error, "listener faulted in OnWindowMessage (code 0x{:08X} at {}), window input forwarding disabled", stFault.uCode, stFault.pAddress);
				}
				else if (bHandled)
				{
					if (IsHeldRelease(uMessage, uWParam))
					{
						TrackDelivered(uMessage, uWParam, iLParam);
						CallOriginal(pfnOriginal, bUnicode, hWindow, uMessage, uWParam, iLParam);
					}
					return iResult;
				}
			}

			TrackDelivered(uMessage, uWParam, iLParam);
			return CallOriginal(pfnOriginal, bUnicode, hWindow, uMessage, uWParam, iLParam);
		}
	}

	bool Attach(HWND hWindow, clRenderListener* pListener)
	{
		if (s_hWindow.load() || !hWindow || !IsWindow(hWindow))
			return false;

		const bool bUnicode = IsWindowUnicode(hWindow) != FALSE;
		s_bUnicode.store(bUnicode);
		s_pListener.store(pListener, std::memory_order_release);

		const LONG_PTR iCurrent = bUnicode ? GetWindowLongPtrW(hWindow, GWLP_WNDPROC) : GetWindowLongPtrA(hWindow, GWLP_WNDPROC);
		s_pfnOriginal.store(reinterpret_cast<WNDPROC>(iCurrent), std::memory_order_release);

		SetLastError(ERROR_SUCCESS);
		const LONG_PTR iNew = reinterpret_cast<LONG_PTR>(&WindowProc);
		const LONG_PTR iPrevious = bUnicode ? SetWindowLongPtrW(hWindow, GWLP_WNDPROC, iNew) : SetWindowLongPtrA(hWindow, GWLP_WNDPROC, iNew);
		if (!iPrevious && GetLastError() != ERROR_SUCCESS)
		{
			s_pListener.store(nullptr);
			s_pfnOriginal.store(nullptr);
			Log(ELogLevel::Error, "failed to subclass window {} (error {})", static_cast<void*>(hWindow), GetLastError());
			return false;
		}

		s_pfnOriginal.store(reinterpret_cast<WNDPROC>(iPrevious), std::memory_order_release);
		s_stHeld = {};
		static_cast<void>(ReleaseMessage());
		s_hWindow.store(hWindow);
		Log(ELogLevel::Info, "window {} subclassed ({})", static_cast<void*>(hWindow), bUnicode ? "unicode" : "ansi");
		return true;
	}

	bool Detach()
	{
		s_pListener.store(nullptr, std::memory_order_release);
		const HWND hWindow = s_hWindow.load();
		if (!hWindow)
			return true;

		bool bRestored = true;
		if (IsWindow(hWindow))
		{
			const bool bUnicode = s_bUnicode.load();
			const LONG_PTR iCurrent = bUnicode ? GetWindowLongPtrW(hWindow, GWLP_WNDPROC) : GetWindowLongPtrA(hWindow, GWLP_WNDPROC);
			if (iCurrent == reinterpret_cast<LONG_PTR>(&WindowProc))
			{
				const LONG_PTR iOriginal = reinterpret_cast<LONG_PTR>(s_pfnOriginal.load());
				bUnicode ? SetWindowLongPtrW(hWindow, GWLP_WNDPROC, iOriginal) : SetWindowLongPtrA(hWindow, GWLP_WNDPROC, iOriginal);
			}
			else
			{
				bRestored = false;
				Log(ELogLevel::Warning, "window {} was subclassed again after us, leaving a pass-through window procedure in place", static_cast<void*>(hWindow));
			}
		}

		if (bRestored)
			s_hWindow.store(nullptr);
		return bRestored;
	}

	void SetListener(clRenderListener* pListener)
	{
		if (s_hWindow.load())
			s_pListener.store(pListener, std::memory_order_release);
	}

	bool IsAttached()
	{
		return s_hWindow.load() != nullptr;
	}

	HWND Window()
	{
		return s_hWindow.load();
	}

	void ReleaseHeldInput(bool bKeyboard, bool bMouse)
	{
		const HWND hWindow = s_hWindow.load();
		const WPARAM uWhat = (bKeyboard ? uReleaseKeyboard : 0) | (bMouse ? uReleaseMouse : 0);
		if (hWindow && uWhat)
			PostMessageW(hWindow, ReleaseMessage(), uWhat, 0);
	}
}
