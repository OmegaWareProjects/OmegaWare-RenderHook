#include "OmegaRenderHook/Input/InputBlock.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/InlineHook.hpp"
#include "OmegaRenderHook/Window/WindowHook.hpp"

#include <Windows.h>
#include <intrin.h>
#include <Xinput.h>

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <GameInput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OmegaRenderHook
{
	namespace
	{
		std::atomic<int> g_iCallsInFlight{ 0 };

		class clInputScope
		{
		public:
			clInputScope() { g_iCallsInFlight.fetch_add(1, std::memory_order_acq_rel); }
			~clInputScope() { g_iCallsInFlight.fetch_sub(1, std::memory_order_acq_rel); }

			clInputScope(const clInputScope&) = delete;
			clInputScope& operator=(const clInputScope&) = delete;
		};

		[[nodiscard]] bool WaitForInputDrain(std::chrono::milliseconds msTimeout)
		{
			const auto tpDeadline = std::chrono::steady_clock::now() + msTimeout;
			while (g_iCallsInFlight.load(std::memory_order_acquire) > 0)
			{
				if (std::chrono::steady_clock::now() >= tpDeadline)
					return false;
				Sleep(1);
			}
			Sleep(20);
			return g_iCallsInFlight.load(std::memory_order_acquire) == 0;
		}

		using FnGetAsyncKeyState = SHORT(WINAPI*)(int);
		using FnGetKeyState = SHORT(WINAPI*)(int);
		using FnGetKeyboardState = BOOL(WINAPI*)(PBYTE);
		using FnGetCursorPos = BOOL(WINAPI*)(LPPOINT);
		using FnSetCursorPos = BOOL(WINAPI*)(int, int);
		using FnClipCursor = BOOL(WINAPI*)(const RECT*);
		using FnSetCursor = HCURSOR(WINAPI*)(HCURSOR);
		using FnGetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
		using FnGetRawInputBuffer = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
		using FnXInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
		using FnGetDeviceState = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD, LPVOID);
		using FnGetDeviceData = HRESULT(WINAPI*)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);

		constexpr std::array<std::wstring_view, 5> kXInputModules = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll" };
		constexpr std::array<std::string_view, 5> kXInputNames = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll" };

		enum class EDeviceKind
		{
			Other,
			Keyboard,
			Mouse,
			Gamepad,
		};

		struct stJoystickLayout
		{
			DWORD uSize = 0;
			std::vector<std::pair<DWORD, DWORD>> aNeutral;
		};

		namespace GameInputSlots
		{
			constexpr std::array<std::size_t, 4> kReadings = { 4, 5, 6, 7 };
			constexpr std::size_t uAxisState = 9;
			constexpr std::size_t uButtonState = 11;
			constexpr std::size_t uSwitchState = 13;
			constexpr std::size_t uKeyState = 15;
			constexpr std::size_t uMouseState = 16;
			constexpr std::size_t uArcadeStickState = 20;
			constexpr std::size_t uFlightStickState = 21;
			constexpr std::size_t uGamepadState = 22;
			constexpr std::size_t uRacingWheelState = 23;
			constexpr std::size_t uUiNavigationState = 24;
			constexpr std::array<std::size_t, 10> kStates = { uAxisState, uButtonState, uSwitchState, uKeyState, uMouseState, uArcadeStickState, uFlightStickState, uGamepadState, uRacingWheelState, uUiNavigationState };
		}

		struct stGameInputHooks
		{
			bool bInputHooked = false;
			std::atomic<bool> bReadingHooked = false;
			std::array<clInlineHook, GameInputSlots::kReadings.size()> aReadings;
			std::array<clInlineHook, GameInputSlots::kStates.size()> aStates;
			std::mutex mtxMice;
			std::unordered_map<void*, GameInputMouseState> mapMice;
		};

		struct stXInputHooks
		{
			HMODULE hModule = nullptr;
			clInlineHook clGetState;
			clInlineHook clGetStateEx;
		};

		struct stState
		{
			std::recursive_mutex mtxInstall;
			bool bInstalled = false;
			std::uintptr_t uOwnBase = 0;
			std::uintptr_t uOwnEnd = 0;
			std::atomic<bool> bKeyboard = false;
			std::atomic<bool> bMouse = false;
			std::atomic<bool> bGamepad = false;
			POINT stFrozenCursor{};
			bool bHasGameClip = false;
			RECT rcGameClip{};

			clInlineHook clGetAsyncKeyState;
			clInlineHook clGetKeyState;
			clInlineHook clGetKeyboardState;
			clInlineHook clGetCursorPos;
			clInlineHook clSetCursorPos;
			clInlineHook clClipCursor;
			clInlineHook clSetCursor;
			clInlineHook clGetRawInputData;
			clInlineHook clGetRawInputBuffer;
			std::array<stXInputHooks, kXInputModules.size()> aXInput;
			bool bDirectInputHooked = false;
			std::vector<std::unique_ptr<clInlineHook>> aDirectInput;
			std::unordered_map<void*, void*> mapDirectInputOriginals;
			std::mutex mtxDevices;
			std::unordered_map<void*, EDeviceKind> mapDevices;
			std::unordered_map<void*, stJoystickLayout> mapJoysticks;
			std::mutex mtxGameInput;
			stGameInputHooks stGameInput;
		};

		stState& State()
		{
			static stState s_stState;
			return s_stState;
		}

		[[nodiscard]] bool FromOwnModule(void* pReturn)
		{
			const auto uAddress = reinterpret_cast<std::uintptr_t>(pReturn);
			return uAddress >= State().uOwnBase && uAddress < State().uOwnEnd;
		}

		[[nodiscard]] bool IsMouseKey(int iKey)
		{
			return iKey == VK_LBUTTON || iKey == VK_RBUTTON || iKey == VK_MBUTTON || iKey == VK_XBUTTON1 || iKey == VK_XBUTTON2;
		}

		[[nodiscard]] bool BlocksKey(int iKey)
		{
			return IsMouseKey(iKey) ? State().bMouse.load() : State().bKeyboard.load();
		}

		SHORT WINAPI DetourGetAsyncKeyState(int iKey)
		{
			clInputScope clScope;
			const SHORT sResult = State().clGetAsyncKeyState.Original<FnGetAsyncKeyState>()(iKey);
			return !FromOwnModule(_ReturnAddress()) && BlocksKey(iKey) ? 0 : sResult;
		}

		SHORT WINAPI DetourGetKeyState(int iKey)
		{
			clInputScope clScope;
			const SHORT sResult = State().clGetKeyState.Original<FnGetKeyState>()(iKey);
			if (FromOwnModule(_ReturnAddress()) || !BlocksKey(iKey))
				return sResult;
			return static_cast<SHORT>(sResult & 0x0001);
		}

		BOOL WINAPI DetourGetKeyboardState(PBYTE pKeys)
		{
			clInputScope clScope;
			const BOOL bResult = State().clGetKeyboardState.Original<FnGetKeyboardState>()(pKeys);
			if (bResult && pKeys && !FromOwnModule(_ReturnAddress()))
				for (int iKey = 0; iKey < 256; ++iKey)
					if (BlocksKey(iKey))
						pKeys[iKey] &= 0x01;
			return bResult;
		}

		BOOL WINAPI DetourGetCursorPos(LPPOINT pPoint)
		{
			clInputScope clScope;
			if (!State().bMouse || FromOwnModule(_ReturnAddress()) || !pPoint)
				return State().clGetCursorPos.Original<FnGetCursorPos>()(pPoint);
			*pPoint = State().stFrozenCursor;
			return TRUE;
		}

		BOOL WINAPI DetourSetCursorPos(int iX, int iY)
		{
			clInputScope clScope;
			if (!State().bMouse || FromOwnModule(_ReturnAddress()))
				return State().clSetCursorPos.Original<FnSetCursorPos>()(iX, iY);
			State().stFrozenCursor = { iX, iY };
			return TRUE;
		}

		BOOL WINAPI DetourClipCursor(const RECT* pRect)
		{
			clInputScope clScope;
			if (!State().bMouse || FromOwnModule(_ReturnAddress()))
				return State().clClipCursor.Original<FnClipCursor>()(pRect);
			State().bHasGameClip = pRect != nullptr;
			if (pRect)
				State().rcGameClip = *pRect;
			return TRUE;
		}

		HCURSOR WINAPI DetourSetCursor(HCURSOR hCursor)
		{
			clInputScope clScope;
			if (!State().bMouse || FromOwnModule(_ReturnAddress()))
				return State().clSetCursor.Original<FnSetCursor>()(hCursor);
			return GetCursor();
		}

		void NeutralizeRawInput(RAWINPUT& rInput)
		{
			if (rInput.header.dwType == RIM_TYPEMOUSE && State().bMouse)
			{
				RAWMOUSE& rMouse = rInput.data.mouse;
				const USHORT uDown = rMouse.usButtonFlags & (RI_MOUSE_BUTTON_1_DOWN | RI_MOUSE_BUTTON_2_DOWN | RI_MOUSE_BUTTON_3_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN);
				const USHORT uUp = rMouse.usButtonFlags & (RI_MOUSE_BUTTON_1_UP | RI_MOUSE_BUTTON_2_UP | RI_MOUSE_BUTTON_3_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP);
				rMouse.usFlags = MOUSE_MOVE_RELATIVE;
				rMouse.usButtonFlags = static_cast<USHORT>((uDown << 1) | uUp);
				rMouse.usButtonData = 0;
				rMouse.ulRawButtons = 0;
				rMouse.lLastX = 0;
				rMouse.lLastY = 0;
			}
			else if (rInput.header.dwType == RIM_TYPEKEYBOARD && State().bKeyboard)
			{
				RAWKEYBOARD& rKeyboard = rInput.data.keyboard;
				rKeyboard.Flags |= RI_KEY_BREAK;
				rKeyboard.Message = rKeyboard.Message == WM_SYSKEYDOWN || rKeyboard.Message == WM_SYSKEYUP ? WM_SYSKEYUP : WM_KEYUP;
			}
		}

		UINT WINAPI DetourGetRawInputData(HRAWINPUT hRawInput, UINT uCommand, LPVOID pData, PUINT pSize, UINT uHeaderSize)
		{
			clInputScope clScope;
			const UINT uResult = State().clGetRawInputData.Original<FnGetRawInputData>()(hRawInput, uCommand, pData, pSize, uHeaderSize);
			if (uCommand == RID_INPUT && pData && uResult != static_cast<UINT>(-1) && uResult >= sizeof(RAWINPUTHEADER) && !FromOwnModule(_ReturnAddress()))
				NeutralizeRawInput(*static_cast<RAWINPUT*>(pData));
			return uResult;
		}

		UINT WINAPI DetourGetRawInputBuffer(PRAWINPUT pData, PUINT pSize, UINT uHeaderSize)
		{
			clInputScope clScope;
			const UINT uResult = State().clGetRawInputBuffer.Original<FnGetRawInputBuffer>()(pData, pSize, uHeaderSize);
			if (pData && uResult != static_cast<UINT>(-1) && !FromOwnModule(_ReturnAddress()))
			{
				RAWINPUT* pInput = pData;
				for (UINT i = 0; i < uResult; ++i)
				{
					NeutralizeRawInput(*pInput);
					pInput = reinterpret_cast<RAWINPUT*>((reinterpret_cast<std::uintptr_t>(pInput) + pInput->header.dwSize + 7) & ~std::uintptr_t{ 7 });
				}
			}
			return uResult;
		}

		template <std::size_t uIndex, bool bEx>
		DWORD WINAPI DetourXInputGetState(DWORD uUser, XINPUT_STATE* pState)
		{
			clInputScope clScope;
			stXInputHooks& rHooks = State().aXInput[uIndex];
			const DWORD uResult = (bEx ? rHooks.clGetStateEx : rHooks.clGetState).template Original<FnXInputGetState>()(uUser, pState);
			if (uResult == ERROR_SUCCESS && pState && State().bGamepad && !FromOwnModule(_ReturnAddress()))
				pState->Gamepad = {};
			return uResult;
		}

		template <std::size_t... uIndices>
		constexpr auto MakeXInputDetours(std::index_sequence<uIndices...>)
		{
			return std::array<std::pair<void*, void*>, sizeof...(uIndices)>{ std::pair<void*, void*>{ reinterpret_cast<void*>(&DetourXInputGetState<uIndices, false>), reinterpret_cast<void*>(&DetourXInputGetState<uIndices, true>) }... };
		}

		const auto kXInputDetours = MakeXInputDetours(std::make_index_sequence<kXInputModules.size()>{});

		EDeviceKind DeviceKind(IDirectInputDevice8W* pDevice)
		{
			std::scoped_lock lckDevices(State().mtxDevices);
			if (const auto itDevice = State().mapDevices.find(pDevice); itDevice != State().mapDevices.end())
				return itDevice->second;
			DIDEVCAPS stCaps{ sizeof(DIDEVCAPS) };
			EDeviceKind eKind = EDeviceKind::Other;
			if (SUCCEEDED(pDevice->GetCapabilities(&stCaps)))
			{
				const BYTE byType = GET_DIDEVICE_TYPE(stCaps.dwDevType);
				if (byType == DI8DEVTYPE_KEYBOARD)
					eKind = EDeviceKind::Keyboard;
				else if (byType == DI8DEVTYPE_MOUSE)
					eKind = EDeviceKind::Mouse;
				else if ((byType >= DI8DEVTYPE_JOYSTICK && byType <= DI8DEVTYPE_1STPERSON) || byType == DI8DEVTYPE_SUPPLEMENTAL)
					eKind = EDeviceKind::Gamepad;
			}
			State().mapDevices[pDevice] = eKind;
			return eKind;
		}

		[[nodiscard]] bool BlocksDevice(IDirectInputDevice8W* pDevice, EDeviceKind& rKind)
		{
			if (!State().bKeyboard && !State().bMouse && !State().bGamepad)
				return false;
			rKind = DeviceKind(pDevice);
			return (rKind == EDeviceKind::Keyboard && State().bKeyboard) || (rKind == EDeviceKind::Mouse && State().bMouse) || (rKind == EDeviceKind::Gamepad && State().bGamepad);
		}

		[[nodiscard]] DWORD ObjectType(IDirectInputDevice8W* pDevice, DWORD uOffset)
		{
			DIDEVICEOBJECTINSTANCEW stWide{ sizeof(DIDEVICEOBJECTINSTANCEW) };
			const HRESULT hResult = pDevice->GetObjectInfo(&stWide, uOffset, DIPH_BYOFFSET);
			if (SUCCEEDED(hResult))
				return stWide.dwType;
			if (hResult != DIERR_INVALIDPARAM)
				return 0;
			DIDEVICEOBJECTINSTANCEA stAnsi{ sizeof(DIDEVICEOBJECTINSTANCEA) };
			return SUCCEEDED(reinterpret_cast<IDirectInputDevice8A*>(pDevice)->GetObjectInfo(&stAnsi, uOffset, DIPH_BYOFFSET)) ? stAnsi.dwType : 0;
		}

		[[nodiscard]] stJoystickLayout JoystickLayout(IDirectInputDevice8W* pDevice, DWORD uSize)
		{
			std::scoped_lock lckDevices(State().mtxDevices);
			stJoystickLayout& rLayout = State().mapJoysticks[pDevice];
			if (rLayout.uSize >= uSize)
				return rLayout;
			rLayout = { uSize, {} };
			for (DWORD uOffset = 0; uOffset + sizeof(DWORD) <= uSize; uOffset += sizeof(DWORD))
			{
				const DWORD uType = ObjectType(pDevice, uOffset);
				if (uType & DIDFT_AXIS)
				{
					DIPROPRANGE stRange{};
					stRange.diph = { sizeof(DIPROPRANGE), sizeof(DIPROPHEADER), uOffset, DIPH_BYOFFSET };
					LONG iCenter = 0;
					if (SUCCEEDED(pDevice->GetProperty(DIPROP_RANGE, &stRange.diph)))
						iCenter = static_cast<LONG>(stRange.lMin + (static_cast<std::int64_t>(stRange.lMax) - stRange.lMin) / 2);
					rLayout.aNeutral.emplace_back(uOffset, static_cast<DWORD>(iCenter));
				}
				else if (uType & DIDFT_POV)
					rLayout.aNeutral.emplace_back(uOffset, 0xFFFFFFFFu);
			}
			return rLayout;
		}

		[[nodiscard]] DWORD NeutralValue(const stJoystickLayout& rLayout, DWORD uOffset)
		{
			for (const auto& [uNeutralOffset, uValue] : rLayout.aNeutral)
				if (uNeutralOffset == uOffset)
					return uValue;
			return 0;
		}

		template <std::size_t uSlot>
		HRESULT WINAPI DetourGetDeviceState(IDirectInputDevice8W* pDevice, DWORD uSize, LPVOID pData)
		{
			clInputScope clScope;
			const auto pfnOriginal = reinterpret_cast<FnGetDeviceState>(State().aDirectInput[uSlot]->Original<void*>());
			const HRESULT hResult = pfnOriginal(pDevice, uSize, pData);
			EDeviceKind eKind = EDeviceKind::Other;
			if (SUCCEEDED(hResult) && pData && !FromOwnModule(_ReturnAddress()) && BlocksDevice(pDevice, eKind))
			{
				std::memset(pData, 0, uSize);
				if (eKind == EDeviceKind::Gamepad)
					for (const auto& [uOffset, uValue] : JoystickLayout(pDevice, uSize).aNeutral)
						if (uOffset + sizeof(uValue) <= uSize)
							std::memcpy(static_cast<std::uint8_t*>(pData) + uOffset, &uValue, sizeof(uValue));
			}
			return hResult;
		}

		template <std::size_t uSlot>
		HRESULT WINAPI DetourGetDeviceData(IDirectInputDevice8W* pDevice, DWORD uObjectSize, LPDIDEVICEOBJECTDATA pData, LPDWORD pInOut, DWORD uFlags)
		{
			clInputScope clScope;
			const auto pfnOriginal = reinterpret_cast<FnGetDeviceData>(State().aDirectInput[uSlot]->Original<void*>());
			const HRESULT hResult = pfnOriginal(pDevice, uObjectSize, pData, pInOut, uFlags);
			EDeviceKind eKind = EDeviceKind::Other;
			if (SUCCEEDED(hResult) && pData && pInOut && uObjectSize >= sizeof(DIDEVICEOBJECTDATA) && !FromOwnModule(_ReturnAddress()) && BlocksDevice(pDevice, eKind))
			{
				const stJoystickLayout stLayout = eKind == EDeviceKind::Gamepad ? JoystickLayout(pDevice, sizeof(DIJOYSTATE2)) : stJoystickLayout{};
				for (DWORD i = 0; i < *pInOut; ++i)
				{
					auto* pEvent = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<std::uint8_t*>(pData) + i * uObjectSize);
					pEvent->dwData = eKind == EDeviceKind::Gamepad ? NeutralValue(stLayout, pEvent->dwOfs) : 0;
				}
			}
			return hResult;
		}

		const std::array<void*, 4> kDirectInputDetours = {
			reinterpret_cast<void*>(&DetourGetDeviceState<0>),
			reinterpret_cast<void*>(&DetourGetDeviceData<1>),
			reinterpret_cast<void*>(&DetourGetDeviceState<2>),
			reinterpret_cast<void*>(&DetourGetDeviceData<3>),
		};

		[[nodiscard]] constexpr std::size_t GameInputStateIndex(std::size_t uSlot)
		{
			for (std::size_t i = 0; i < GameInputSlots::kStates.size(); ++i)
				if (GameInputSlots::kStates[i] == uSlot)
					return i;
			return SIZE_MAX;
		}

		template <std::size_t uSlot, typename Fn>
		[[nodiscard]] Fn GameInputOriginal()
		{
			return State().stGameInput.aStates[GameInputStateIndex(uSlot)].template Original<Fn>();
		}

		[[nodiscard]] bool BlocksGameInput(void* pReturn, bool bFlag)
		{
			return bFlag && !FromOwnModule(pReturn);
		}

		uint32_t STDMETHODCALLTYPE DetourGameInputAxisState(IGameInputReading* pReading, uint32_t uCount, float* pAxes)
		{
			clInputScope clScope;
			const uint32_t uResult = GameInputOriginal<GameInputSlots::uAxisState, uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, uint32_t, float*)>()(pReading, uCount, pAxes);
			if (pAxes && BlocksGameInput(_ReturnAddress(), State().bGamepad))
				std::fill_n(pAxes, std::min(uResult, uCount), 0.5f);
			return uResult;
		}

		uint32_t STDMETHODCALLTYPE DetourGameInputButtonState(IGameInputReading* pReading, uint32_t uCount, bool* pButtons)
		{
			clInputScope clScope;
			const uint32_t uResult = GameInputOriginal<GameInputSlots::uButtonState, uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, uint32_t, bool*)>()(pReading, uCount, pButtons);
			if (pButtons && BlocksGameInput(_ReturnAddress(), State().bGamepad))
				std::fill_n(pButtons, std::min(uResult, uCount), false);
			return uResult;
		}

		uint32_t STDMETHODCALLTYPE DetourGameInputSwitchState(IGameInputReading* pReading, uint32_t uCount, GameInputSwitchPosition* pSwitches)
		{
			clInputScope clScope;
			const uint32_t uResult = GameInputOriginal<GameInputSlots::uSwitchState, uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, uint32_t, GameInputSwitchPosition*)>()(pReading, uCount, pSwitches);
			if (pSwitches && BlocksGameInput(_ReturnAddress(), State().bGamepad))
				std::fill_n(pSwitches, std::min(uResult, uCount), GameInputSwitchCenter);
			return uResult;
		}

		uint32_t STDMETHODCALLTYPE DetourGameInputKeyState(IGameInputReading* pReading, uint32_t uCount, GameInputKeyState* pKeys)
		{
			clInputScope clScope;
			const uint32_t uResult = GameInputOriginal<GameInputSlots::uKeyState, uint32_t(STDMETHODCALLTYPE*)(IGameInputReading*, uint32_t, GameInputKeyState*)>()(pReading, uCount, pKeys);
			return BlocksGameInput(_ReturnAddress(), State().bKeyboard) ? 0 : uResult;
		}

		bool STDMETHODCALLTYPE DetourGameInputMouseState(IGameInputReading* pReading, GameInputMouseState* pMouse)
		{
			clInputScope clScope;
			const bool bResult = GameInputOriginal<GameInputSlots::uMouseState, bool(STDMETHODCALLTYPE*)(IGameInputReading*, GameInputMouseState*)>()(pReading, pMouse);
			if (!bResult || !pMouse || FromOwnModule(_ReturnAddress()))
				return bResult;
			IGameInputDevice* pDevice = nullptr;
			pReading->GetDevice(&pDevice);
			stGameInputHooks& rHooks = State().stGameInput;
			{
				std::scoped_lock lckMice(rHooks.mtxMice);
				if (!State().bMouse)
					rHooks.mapMice[pDevice] = *pMouse;
				else if (const auto itMouse = rHooks.mapMice.find(pDevice); itMouse != rHooks.mapMice.end())
					*pMouse = itMouse->second;
				if (State().bMouse)
					pMouse->buttons = GameInputMouseNone;
			}
			if (pDevice)
				pDevice->Release();
			return bResult;
		}

		template <std::size_t uSlot, typename T, typename F>
		bool GameInputControllerState(IGameInputReading* pReading, T* pState, void* pReturn, F&& fnNeutralize)
		{
			const bool bResult = GameInputOriginal<uSlot, bool(STDMETHODCALLTYPE*)(IGameInputReading*, T*)>()(pReading, pState);
			if (bResult && pState && BlocksGameInput(pReturn, State().bGamepad))
				fnNeutralize(*pState);
			return bResult;
		}

		bool STDMETHODCALLTYPE DetourGameInputArcadeStickState(IGameInputReading* pReading, GameInputArcadeStickState* pState)
		{
			clInputScope clScope;
			return GameInputControllerState<GameInputSlots::uArcadeStickState>(pReading, pState, _ReturnAddress(), [](GameInputArcadeStickState& r) { r = {}; });
		}

		bool STDMETHODCALLTYPE DetourGameInputFlightStickState(IGameInputReading* pReading, GameInputFlightStickState* pState)
		{
			clInputScope clScope;
			return GameInputControllerState<GameInputSlots::uFlightStickState>(pReading, pState, _ReturnAddress(), [](GameInputFlightStickState& r) { r = {}; r.hatSwitch = GameInputSwitchCenter; });
		}

		bool STDMETHODCALLTYPE DetourGameInputGamepadState(IGameInputReading* pReading, GameInputGamepadState* pState)
		{
			clInputScope clScope;
			return GameInputControllerState<GameInputSlots::uGamepadState>(pReading, pState, _ReturnAddress(), [](GameInputGamepadState& r) { r = {}; });
		}

		bool STDMETHODCALLTYPE DetourGameInputRacingWheelState(IGameInputReading* pReading, GameInputRacingWheelState* pState)
		{
			clInputScope clScope;
			return GameInputControllerState<GameInputSlots::uRacingWheelState>(pReading, pState, _ReturnAddress(), [](GameInputRacingWheelState& r) { const int32_t iGear = r.patternShifterGear; r = {}; r.patternShifterGear = iGear; });
		}

		bool STDMETHODCALLTYPE DetourGameInputUiNavigationState(IGameInputReading* pReading, GameInputUiNavigationState* pState)
		{
			clInputScope clScope;
			return GameInputControllerState<GameInputSlots::uUiNavigationState>(pReading, pState, _ReturnAddress(), [](GameInputUiNavigationState& r) { r = {}; });
		}

		const std::array<void*, GameInputSlots::kStates.size()> kGameInputStateDetours = {
			reinterpret_cast<void*>(&DetourGameInputAxisState),
			reinterpret_cast<void*>(&DetourGameInputButtonState),
			reinterpret_cast<void*>(&DetourGameInputSwitchState),
			reinterpret_cast<void*>(&DetourGameInputKeyState),
			reinterpret_cast<void*>(&DetourGameInputMouseState),
			reinterpret_cast<void*>(&DetourGameInputArcadeStickState),
			reinterpret_cast<void*>(&DetourGameInputFlightStickState),
			reinterpret_cast<void*>(&DetourGameInputGamepadState),
			reinterpret_cast<void*>(&DetourGameInputRacingWheelState),
			reinterpret_cast<void*>(&DetourGameInputUiNavigationState),
		};

		void HookGameInputReading(IGameInputReading* pReading)
		{
			stGameInputHooks& rHooks = State().stGameInput;
			if (!pReading || rHooks.bReadingHooked.load(std::memory_order_acquire))
				return;
			std::scoped_lock lckGameInput(State().mtxGameInput);
			if (rHooks.bReadingHooked)
				return;
			void** ppVTable = *reinterpret_cast<void***>(pReading);
			std::size_t uHooked = 0;
			for (std::size_t i = 0; i < GameInputSlots::kStates.size(); ++i)
				if (rHooks.aStates[i].IsInstalled() || rHooks.aStates[i].Install(ppVTable[GameInputSlots::kStates[i]], kGameInputStateDetours[i]))
					++uHooked;
			rHooks.bReadingHooked = true;
			Log(ELogLevel::Info, "input block hooked {} gameinput reading methods", uHooked);
		}

		template <std::size_t uIndex, typename Fn>
		[[nodiscard]] Fn GameInputReadingOriginal()
		{
			return State().stGameInput.aReadings[uIndex].template Original<Fn>();
		}

		HRESULT STDMETHODCALLTYPE DetourGameInputCurrentReading(IGameInput* pInput, GameInputKind eKind, IGameInputDevice* pDevice, IGameInputReading** ppReading)
		{
			clInputScope clScope;
			const HRESULT hResult = GameInputReadingOriginal<0, HRESULT(STDMETHODCALLTYPE*)(IGameInput*, GameInputKind, IGameInputDevice*, IGameInputReading**)>()(pInput, eKind, pDevice, ppReading);
			if (SUCCEEDED(hResult) && ppReading)
				HookGameInputReading(*ppReading);
			return hResult;
		}

		template <std::size_t uIndex>
		HRESULT STDMETHODCALLTYPE DetourGameInputRelativeReading(IGameInput* pInput, IGameInputReading* pReference, GameInputKind eKind, IGameInputDevice* pDevice, IGameInputReading** ppReading)
		{
			clInputScope clScope;
			const HRESULT hResult = GameInputReadingOriginal<uIndex, HRESULT(STDMETHODCALLTYPE*)(IGameInput*, IGameInputReading*, GameInputKind, IGameInputDevice*, IGameInputReading**)>()(pInput, pReference, eKind, pDevice, ppReading);
			if (SUCCEEDED(hResult) && ppReading)
				HookGameInputReading(*ppReading);
			return hResult;
		}

		HRESULT STDMETHODCALLTYPE DetourGameInputTemporalReading(IGameInput* pInput, uint64_t uTimestamp, IGameInputDevice* pDevice, IGameInputReading** ppReading)
		{
			clInputScope clScope;
			const HRESULT hResult = GameInputReadingOriginal<3, HRESULT(STDMETHODCALLTYPE*)(IGameInput*, uint64_t, IGameInputDevice*, IGameInputReading**)>()(pInput, uTimestamp, pDevice, ppReading);
			if (SUCCEEDED(hResult) && ppReading)
				HookGameInputReading(*ppReading);
			return hResult;
		}

		const std::array<void*, GameInputSlots::kReadings.size()> kGameInputReadingDetours = {
			reinterpret_cast<void*>(&DetourGameInputCurrentReading),
			reinterpret_cast<void*>(&DetourGameInputRelativeReading<1>),
			reinterpret_cast<void*>(&DetourGameInputRelativeReading<2>),
			reinterpret_cast<void*>(&DetourGameInputTemporalReading),
		};

		void HookGameInput()
		{
			stGameInputHooks& rHooks = State().stGameInput;
			const HMODULE hModule = GetModuleHandleW(L"GameInput.dll");
			if (rHooks.bInputHooked || !hModule)
				return;
			using FnGameInputCreate = HRESULT(WINAPI*)(IGameInput**);
			const auto pfnCreate = reinterpret_cast<FnGameInputCreate>(GetProcAddress(hModule, "GameInputCreate"));
			IGameInput* pInput = nullptr;
			if (!pfnCreate || FAILED(pfnCreate(&pInput)) || !pInput)
				return;
			{
				std::scoped_lock lckGameInput(State().mtxGameInput);
				void** ppVTable = *reinterpret_cast<void***>(pInput);
				for (std::size_t i = 0; i < GameInputSlots::kReadings.size(); ++i)
					static_cast<void>(rHooks.aReadings[i].Install(ppVTable[GameInputSlots::kReadings[i]], kGameInputReadingDetours[i]));
				rHooks.bInputHooked = true;
			}
			IGameInputReading* pReading = nullptr;
			if (SUCCEEDED(pInput->GetCurrentReading(GameInputKindKeyboard | GameInputKindMouse | GameInputKindController | GameInputKindGamepad, nullptr, &pReading)) && pReading)
			{
				HookGameInputReading(pReading);
				pReading->Release();
			}
			pInput->Release();
			Log(ELogLevel::Info, "input block hooked gameinput");
		}

		bool HookExport(clInlineHook& rHook, const wchar_t* szModule, const char* szName, void* pDetour)
		{
			if (rHook.IsInstalled())
				return true;
			const HMODULE hModule = GetModuleHandleW(szModule);
			void* pTarget = hModule ? reinterpret_cast<void*>(GetProcAddress(hModule, szName)) : nullptr;
			if (!pTarget)
				return false;
			if (!rHook.Install(pTarget, pDetour))
			{
				Log(ELogLevel::Warning, "input block could not hook {}", szName);
				return false;
			}
			return true;
		}

		void HookXInput()
		{
			for (std::size_t i = 0; i < kXInputModules.size(); ++i)
			{
				stXInputHooks& rHooks = State().aXInput[i];
				const HMODULE hModule = GetModuleHandleW(kXInputModules[i].data());
				if (!hModule || rHooks.hModule)
					continue;
				rHooks.hModule = hModule;
				void* pGetState = reinterpret_cast<void*>(GetProcAddress(hModule, "XInputGetState"));
				void* pGetStateEx = reinterpret_cast<void*>(GetProcAddress(hModule, MAKEINTRESOURCEA(100)));
				if (pGetState)
					static_cast<void>(rHooks.clGetState.Install(pGetState, kXInputDetours[i].first));
				if (pGetStateEx && pGetStateEx != pGetState)
					static_cast<void>(rHooks.clGetStateEx.Install(pGetStateEx, kXInputDetours[i].second));
				Log(ELogLevel::Info, "input block hooked {}", kXInputNames[i]);
			}
		}

		void HookDirectInput()
		{
			stState& r = State();
			if (r.bDirectInputHooked || !GetModuleHandleW(L"dinput8.dll"))
				return;
			r.bDirectInputHooked = true;

			using FnDirectInput8Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
			const auto pfnCreate = reinterpret_cast<FnDirectInput8Create>(GetProcAddress(GetModuleHandleW(L"dinput8.dll"), "DirectInput8Create"));
			if (!pfnCreate)
				return;

			std::vector<void*> aTargets;
			const auto Capture = [&](void** ppVTable) {
				for (const std::size_t uSlot : { std::size_t{ 9 }, std::size_t{ 10 } })
					aTargets.push_back(ppVTable[uSlot]);
			};

			IDirectInput8W* pInputW = nullptr;
			if (SUCCEEDED(pfnCreate(reinterpret_cast<HINSTANCE>(r.uOwnBase), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&pInputW), nullptr)) && pInputW)
			{
				IDirectInputDevice8W* pDevice = nullptr;
				if (SUCCEEDED(pInputW->CreateDevice(GUID_SysKeyboard, &pDevice, nullptr)) && pDevice)
				{
					Capture(*reinterpret_cast<void***>(pDevice));
					pDevice->Release();
				}
				pInputW->Release();
			}
			IDirectInput8A* pInputA = nullptr;
			if (SUCCEEDED(pfnCreate(reinterpret_cast<HINSTANCE>(r.uOwnBase), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&pInputA), nullptr)) && pInputA)
			{
				IDirectInputDevice8A* pDevice = nullptr;
				if (SUCCEEDED(pInputA->CreateDevice(GUID_SysKeyboard, &pDevice, nullptr)) && pDevice)
				{
					Capture(*reinterpret_cast<void***>(pDevice));
					pDevice->Release();
				}
				pInputA->Release();
			}

			std::vector<void*> aHooked;
			r.aDirectInput.reserve(kDirectInputDetours.size());
			for (std::size_t i = 0; i < aTargets.size() && i < kDirectInputDetours.size(); ++i)
			{
				auto pHook = std::make_unique<clInlineHook>();
				const bool bDuplicate = std::find(aHooked.begin(), aHooked.end(), aTargets[i]) != aHooked.end();
				if (!bDuplicate && pHook->Install(aTargets[i], kDirectInputDetours[i]))
					aHooked.push_back(aTargets[i]);
				r.aDirectInput.push_back(std::move(pHook));
			}
			Log(ELogLevel::Info, "input block hooked {} directinput8 device methods", aHooked.size());
		}
	}

	namespace InputBlock
	{
		bool Install()
		{
			stState& r = State();
			std::scoped_lock lckInstall(r.mtxInstall);
			if (r.bInstalled)
				return true;

			HMODULE hOwn = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Install), &hOwn);
			const auto pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hOwn);
			const auto pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const std::uint8_t*>(hOwn) + pDos->e_lfanew);
			r.uOwnBase = reinterpret_cast<std::uintptr_t>(hOwn);
			r.uOwnEnd = r.uOwnBase + pNt->OptionalHeader.SizeOfImage;

			bool bComplete = true;
			bComplete &= HookExport(r.clGetAsyncKeyState, L"user32.dll", "GetAsyncKeyState", reinterpret_cast<void*>(&DetourGetAsyncKeyState));
			bComplete &= HookExport(r.clGetKeyState, L"user32.dll", "GetKeyState", reinterpret_cast<void*>(&DetourGetKeyState));
			bComplete &= HookExport(r.clGetKeyboardState, L"user32.dll", "GetKeyboardState", reinterpret_cast<void*>(&DetourGetKeyboardState));
			bComplete &= HookExport(r.clGetCursorPos, L"user32.dll", "GetCursorPos", reinterpret_cast<void*>(&DetourGetCursorPos));
			bComplete &= HookExport(r.clSetCursorPos, L"user32.dll", "SetCursorPos", reinterpret_cast<void*>(&DetourSetCursorPos));
			bComplete &= HookExport(r.clClipCursor, L"user32.dll", "ClipCursor", reinterpret_cast<void*>(&DetourClipCursor));
			bComplete &= HookExport(r.clSetCursor, L"user32.dll", "SetCursor", reinterpret_cast<void*>(&DetourSetCursor));
			bComplete &= HookExport(r.clGetRawInputData, L"user32.dll", "GetRawInputData", reinterpret_cast<void*>(&DetourGetRawInputData));
			bComplete &= HookExport(r.clGetRawInputBuffer, L"user32.dll", "GetRawInputBuffer", reinterpret_cast<void*>(&DetourGetRawInputBuffer));
			HookXInput();
			HookDirectInput();
			HookGameInput();
			r.bInstalled = true;
			Log(bComplete ? ELogLevel::Info : ELogLevel::Warning, "input block installed{}", bComplete ? "" : " (some user32 hooks failed)");
			return bComplete;
		}

		bool Uninstall()
		{
			stState& r = State();
			std::scoped_lock lckInstall(r.mtxInstall);
			if (!r.bInstalled)
				return true;
			SetBlocking({});

			std::vector<clInlineHook*> aHooks = { &r.clGetAsyncKeyState, &r.clGetKeyState, &r.clGetKeyboardState, &r.clGetCursorPos, &r.clSetCursorPos, &r.clClipCursor, &r.clSetCursor, &r.clGetRawInputData, &r.clGetRawInputBuffer };
			for (stXInputHooks& rXInput : r.aXInput)
			{
				aHooks.push_back(&rXInput.clGetState);
				aHooks.push_back(&rXInput.clGetStateEx);
			}
			for (const auto& pHook : r.aDirectInput)
				aHooks.push_back(pHook.get());
			for (clInlineHook& rHook : r.stGameInput.aReadings)
				aHooks.push_back(&rHook);
			for (clInlineHook& rHook : r.stGameInput.aStates)
				aHooks.push_back(&rHook);

			bool bDisabled = true;
			for (clInlineHook* pHook : aHooks)
				bDisabled = pHook->Disable() && bDisabled;
			const bool bDrained = WaitForInputDrain(std::chrono::milliseconds(2000));
			if (!bDisabled || !bDrained)
			{
				Log(ELogLevel::Warning, "input block hooks are still in use, leaving them in place");
				return false;
			}
			for (clInlineHook* pHook : aHooks)
				pHook->Uninstall();
			for (stXInputHooks& rXInput : r.aXInput)
				rXInput.hModule = nullptr;
			r.aDirectInput.clear();
			r.bDirectInputHooked = false;
			{
				std::scoped_lock lckDevices(r.mtxDevices);
				r.mapDevices.clear();
				r.mapJoysticks.clear();
			}
			{
				std::scoped_lock lckMice(r.stGameInput.mtxMice);
				r.stGameInput.mapMice.clear();
			}
			r.stGameInput.bInputHooked = false;
			r.stGameInput.bReadingHooked = false;
			r.bInstalled = false;
			Log(ELogLevel::Info, "input block removed");
			return true;
		}

		bool IsInstalled()
		{
			return State().bInstalled;
		}

		void SetBlocking(const stInputBlock& stBlock)
		{
			stState& r = State();
			std::scoped_lock lckInstall(r.mtxInstall);
			const bool bMouseBefore = r.bMouse;
			const bool bKeyboardRising = stBlock.bKeyboard && !r.bKeyboard;
			const bool bMouseRising = stBlock.bMouse && !bMouseBefore;
			if (r.bInstalled && stBlock.bGamepad && !r.bGamepad)
			{
				HookXInput();
				std::scoped_lock lckDevices(r.mtxDevices);
				r.mapJoysticks.clear();
			}
			if (r.bInstalled && (stBlock.bKeyboard || stBlock.bMouse || stBlock.bGamepad))
			{
				HookDirectInput();
				HookGameInput();
			}
			if (bKeyboardRising || bMouseRising)
				WindowHook::ReleaseHeldInput(bKeyboardRising, bMouseRising);

			if (stBlock.bMouse && !bMouseBefore && r.bInstalled)
			{
				RECT rcClip{};
				const auto pfnGetCursorPos = r.clGetCursorPos.IsInstalled() ? r.clGetCursorPos.Original<FnGetCursorPos>() : &::GetCursorPos;
				pfnGetCursorPos(&r.stFrozenCursor);
				r.bHasGameClip = GetClipCursor(&rcClip) && (rcClip.right - rcClip.left < GetSystemMetrics(SM_CXVIRTUALSCREEN) || rcClip.bottom - rcClip.top < GetSystemMetrics(SM_CYVIRTUALSCREEN));
				r.rcGameClip = rcClip;
				r.bMouse = true;
				const auto pfnClipCursor = r.clClipCursor.IsInstalled() ? r.clClipCursor.Original<FnClipCursor>() : &::ClipCursor;
				pfnClipCursor(nullptr);
			}
			else if (!stBlock.bMouse && bMouseBefore)
			{
				r.bMouse = false;
				if (r.bHasGameClip)
				{
					const auto pfnClipCursor = r.clClipCursor.IsInstalled() ? r.clClipCursor.Original<FnClipCursor>() : &::ClipCursor;
					pfnClipCursor(&r.rcGameClip);
				}
			}
			r.bKeyboard = stBlock.bKeyboard;
			r.bGamepad = stBlock.bGamepad;
		}

		stInputBlock GetBlocking()
		{
			return { State().bKeyboard, State().bMouse, State().bGamepad };
		}
	}
}
