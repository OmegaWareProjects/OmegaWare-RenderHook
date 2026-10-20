# OmegaWare-RenderHook

The render and hook pipeline for the next [OmegaWare Framework](https://github.com/Omega172/OmegaWare-Framework),
written in C++26. It finds a game's swap chain or device, gives you a place to draw at the right point in
every frame on Direct3D 9, 10, 11 and 12 and OpenGL, forwards window messages and can block the game's
polled input while your menu is open. It is built to survive NVIDIA DLSS, DLSS Frame Generation, Reflex and
Streamline, the Steam overlay and the game recreating or resizing its swap chain.

It pairs with [OmegaWare-GUI](https://github.com/Omega172/OmegaWare-GUI): the pipeline hands over device, queue, format and command
list, the GUI backend draws.

Scope is the same as the framework: single-player and co-op games that allow modding.

## Why public D3D12 hooks crash with DLSS

DLSS and Streamline change three things that most public hooks (and the old framework's
`D3D12Hooks.cpp`) quietly depend on:

1. **There is more than one direct queue.** NGX, Reflex and DLSS-G create their own command queues. The
   usual trick of "take the first queue that calls `ExecuteCommandLists`" picks one of those, so the
   overlay is submitted on a queue that is not the swap chain's. It then races the game's frame, touches
   a back buffer in the wrong state and ends in a device removal.
2. **The swap chain gets recreated and resized.** Holding `ID3D12Resource` references to the back buffers
   makes `ResizeBuffers` fail, and because a window can only have one flip-model swap chain, it also makes
   the recreation Streamline does when Frame Generation is toggled fail. Either one takes the game down.
3. **The back buffer is not what you expect.** Palworld presents `R10G10B10A2_UNORM` with 4 buffers at
   native resolution, while most hooks hard-code `R8G8B8A8_UNORM` pipeline states and render target views.
   With Frame Generation on, `Present` is also called from Streamline's own present thread at twice the
   game's frame rate, so code that assumes the game thread or patches a prologue another thread is
   executing is fragile.

## How this pipeline handles it

- **Queue resolution that never guesses.** In order:
  1. the queue passed to `CreateSwapChain*` when the pipeline was installed early, or when the game
     recreated its swap chain later (the factory is always hooked),
  2. `IDXGISwapChain::GetDevice(ID3D12CommandQueue)` (kept for drivers where it works),
  3. a scan of the swap chain object for a pointer whose vtable is the real `ID3D12CommandQueue` vtable,
     validated as a direct queue on the same device. On current DXGI it sits at offset `0x140`.

  If none match, `ExecuteCommandLists` is hooked only to learn which queue objects are alive, and the scan
  is retried. Rendering never starts on an unverified queue.
- **No long-lived references.** The pipeline never holds the swap chain or its buffers between frames.
  The RTV is rebuilt in a private heap each frame, so the game's `ResizeBuffers` and swap chain
  recreation always succeed.
- **Own frame slots.** One allocator and command list per slot (buffer count + 1, up to 8) with a fence.
  A slot that is still busy after 500 ms skips the overlay for that frame instead of stalling the game.
- **Atomic hooks by default.** `Present`, `Present1`, `ResizeBuffers`, `ResizeBuffers1` and the four
  factory `CreateSwapChain*` methods are hooked by swapping the vtable slot of the real DXGI objects with
  one interlocked write. Streamline's proxies and DLSS-G call through those slots, and the Steam overlay's
  inline patch inside the function still runs. `EHookMethod::Inline` uses
  [SafetyHook](https://github.com/cursey/safetyhook) instead.
- **Fault containment.** Listener code runs under SEH and C++ exception guards. A crash in overlay code
  disables the listener and logs the code and address. The game keeps running.
- **Clean unload.** `Pipeline::Uninstall()` restores every hook and the window procedure, waits for calls
  in flight to drain and returns `true` only when it is safe to `FreeLibrary`. Render, input, game and engine
  hooks each count their own calls in flight (`clHookScopeOf<EHookDomain>`), so one never holds up another.

## Other graphics APIs

- **Direct3D 10** rides on the same DXGI hooks. A swap chain whose `GetDevice` returns an `ID3D10Device` is
  drawn with the Direct3D 10 renderer (a D3D10 device also answers for `ID3D11Device`, so this is checked
  first).
- **Direct3D 9** hooks `Present` and `Reset` (and `PresentEx` / `ResetEx` for 9Ex) inline, from addresses
  read off a throwaway device while it is alive. The game's render state is saved in a state block and
  restored after drawing, and the overlay detaches before every `Reset` and comes back on the next frame.
- **OpenGL** hooks `wglSwapBuffers` and draws with its own context on the game's device context, so the
  game's GL state is never touched.
- Direct3D 9 and OpenGL release their resources inside the game's own `Present` or `wglSwapBuffers` when
  unloading, with a fallback if the game has stopped presenting.

Each API has its own `OnAttach` / `OnFrame` overload on `clRenderListener` and can be turned off in
`stPipelineConfig`.

## Input blocking

`InputBlock::Install()` hooks the ways games read input without window messages. `InputBlock::SetBlocking()`
then hides keyboard, mouse or gamepad input from the game while leaving it visible to your own module:

- `GetAsyncKeyState`, `GetKeyState` and `GetKeyboardState`
- `GetCursorPos`, `SetCursorPos` and `ClipCursor` (the cursor is freed and the game's clip restored after)
- `GetRawInputData` and `GetRawInputBuffer`
- `XInputGetState` and its ordinal 100 variant in every XInput version
- DirectInput 8 `GetDeviceState` and `GetDeviceData` for keyboards, mice and joysticks/gamepads. Joystick
  axes report the center of the range the game set (`DIPROP_RANGE`), POVs report centered and buttons
  report released. The layout is found by probing the game's data format with `GetObjectInfo`.
- GameInput (`GameInput.dll`, x64 only, GameInput has no 32-bit runtime): the reading state getters for
  keys, mouse, gamepad, controller axes/buttons/switches, arcade stick, flight stick, racing wheel and UI
  navigation. Controller axes report 0.5, the mouse is frozen at its last position with no buttons.
  Readings delivered through `RegisterReadingCallback` are not covered.

Calls made from inside your own DLL always get the real values, so the GUI keeps working.

When keyboard or mouse blocking starts, the window hook sends the game's window procedure a release for
every key and mouse button it saw go down, so nothing stays held while the menu is open or after it closes.
A release the menu swallows is still forwarded when the game saw the matching press.

## Game depth

The listener gets the game's scene depth where it can be found, so world-space drawing can hide behind
game geometry (OmegaGUI's world layer):

| API | How | Frame field |
|-----|-----|-------------|
| Direct3D 11 | Inline hooks on the immediate context's `OMSetRenderTargets`, `OMSetRenderTargetsAndUnorderedAccessViews` and `ClearDepthStencilView` track the last depth view matching the back buffer's size and sample count. A clear to a value below 0.5 marks reversed Z. Falls back to the view bound at present. | `stFrameDX11::pDepthStencil`, `bReversedZ` |
| Direct3D 9 | The depth surface bound at present, if it matches the back buffer. Reversed Z is not detected. | `stFrameDX9::pDepthStencil` |
| OpenGL | Nothing to pass: the overlay context draws into the window's default framebuffer, whose depth buffer the game shares when it renders its scene there. | |
| Direct3D 10, 12, Vulkan | Not covered. World drawing works but is not occluded. | |

The depth only helps if the game has not cleared or reused it before `Present`. Games that render the
scene at a different resolution (DLSS, FSR, dynamic resolution) or with MSAA into a separate target get no
depth because the sizes do not match.

## Fault observer

`SetFaultObserver()` registers a callback that runs inside the exception filter of every guarded call, while
the faulting stack is still intact. The OmegaWare Framework uses it to write crash dumps.

## Verified

`SelfTest.bat` builds a fake game with two decoy direct queues and a compute queue that submit before
the real one every frame (the layout DLSS creates), and checks with both hook methods:

- late attach resolves the swap chain's own queue and the overlay pixel is drawn after the game's frame
  (read back from the GPU)
- `ResizeBuffers` succeeds and the overlay follows the new size
- the swap chain can be released and recreated on the same window on a different queue and format
- presenting from a second thread (frame generation style) still renders
- window messages reach the listener and unhandled ones reach the game
- early install resolves the queue from the creation hook
- a C++ exception and an access violation in the listener are contained
- uninstall restores everything and stops rendering
- Direct3D 11: overlay pixel drawn and the game's render targets and viewport restored

All checks pass on x64 and x86 in Debug and Release.

Through the OmegaWare Framework, the overlay was also loaded into OmegaWare-GUI's demo on Direct3D 9, 10,
11 and 12 and OpenGL on x64 and x86: each one attached, drew the menu and unloaded with the host still
running.

In Palworld (Direct3D 12, DLSS Performance, Frame Generation and Reflex on, Steam overlay loaded) the test
overlay late-attached through the object scan, rendered at about 158 presents per second with no
skipped frames using both hook methods and unloaded cleanly each time. Switching between borderless,
windowed and exclusive fullscreen and back, and toggling Frame Generation on and off, kept rendering with
no skipped frames. Palworld's Streamline toggles Frame Generation without recreating the DXGI swap chain,
so the recreation path is covered by the self-test only.

## Layout

```
Source/
  OmegaRenderHook/     the library (static)
    Core/              logging, SEH/C++ fault guard
    Hook/              vtable slot hooks, SafetyHook inline hooks, method hook selector, in-flight scope
    Input/             polled input blocking
    Render/            public pipeline API, DXGI and factory detours, swap chain tracking
      DX12/            queue resolver, per-swap-chain Direct3D 12 renderer
      DX11/            Direct3D 11 renderer with render target and viewport restore
      DX10/            Direct3D 10 renderer
      DX9/             Direct3D 9 Present/Reset hooks and renderer
      OpenGL/          wglSwapBuffers hook and own-context renderer
    Window/            window procedure subclassing
  Tools/
    Injector/          LoadLibraryW remote-thread injector
    TestOverlay/       DLL that draws an animated bar, logs status, unloads on END
  SelfTest/            fake DLSS-style game and pipeline checks
```

Build output goes to `Build/<Debug|Release>/<x64|x86>/`.

## Building

Requirements: Visual Studio 2022 or newer with the C++ workload and [xmake](https://xmake.io).
SafetyHook and Zydis are fetched by xmake on the first configure.

| Script | Purpose |
|---|---|
| `Build.bat` / `.sh` | configure and build (`mode`, `arch`) |
| `SelfTest.bat` / `.sh` | build and run the self-test (`mode`, `arch`, `--verbose` or `--warp` for the WARP software adapter) |
| `Inject.bat` / `.sh` | inject the test overlay into a running process (`process`, `mode`, `arch`) |
| `UpdateCompileCommands.bat` / `.sh` | write `.vscode/compile_commands.json` |

```bat
Build.bat release x64
SelfTest.bat debug x86 --verbose
Inject.bat Palworld-Win64-Shipping.exe
```

The test overlay writes `OmegaRenderHook-TestOverlay.log` next to the DLL. Put an `OmegaRenderHook-TestOverlay.ini`
beside it with `[Overlay]` `HookMethod=inline` to try the inline hooks.

## Using it with OmegaGUI

```cpp
#include "OmegaRenderHook/OmegaRenderHook.hpp"
#include "OmegaGUI/OmegaGUI.hpp"
#include "OmegaGUI/Backends/DX12/BackendDX12.hpp"

class clOverlay final : public OmegaRenderHook::clRenderListener
{
public:
	void OnAttach(const OmegaRenderHook::stDeviceDX12& rDevice) override
	{
		OmegaGUI::stDX12Config stConfig;
		stConfig.pDevice = rDevice.pDevice;
		stConfig.pQueue = rDevice.pQueue;
		stConfig.iFramesInFlight = static_cast<int>(rDevice.uFramesInFlight);
		stConfig.eRenderTargetFormat = rDevice.eFormat;
		m_clBackend.Initialize(stConfig);
		OmegaGUI::Initialize(&m_clBackend, rDevice.hWindow);
	}

	void OnFrame(const OmegaRenderHook::stFrameDX12& rFrame) override
	{
		m_clBackend.BeginFrame(rFrame.pCommandList);
		OmegaGUI::NewFrame();
		DrawMenu();
		OmegaGUI::EndFrame();
		OmegaGUI::Render();
	}

	void OnDetach(OmegaRenderHook::ERenderApi, OmegaRenderHook::EDetachReason) override
	{
		m_clBackend.Shutdown();
	}

	bool OnWindowMessage(HWND hWindow, UINT uMessage, WPARAM uWParam, LPARAM iLParam, LRESULT& rResult) override
	{
		if (!OmegaGUI::ProcessWin32Message(hWindow, uMessage, uWParam, iLParam))
			return false;
		rResult = uMessage == WM_SETCURSOR ? TRUE : 0;
		return true;
	}

private:
	OmegaGUI::clBackendDX12 m_clBackend;
};

clOverlay g_clOverlay;

OmegaRenderHook::stPipelineConfig stConfig;
stConfig.pListener = &g_clOverlay;
if (auto expInstalled = OmegaRenderHook::Pipeline::Install(stConfig); !expInstalled)
	OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Error, "{}", expInstalled.error());
```

When `OnFrame` runs, the back buffer is in `RENDER_TARGET` state, its RTV, a full-size viewport and
scissor are bound, and the list is submitted on the swap chain's queue right before the real `Present`.
D3D12 command lists do not leak state, so nothing needs restoring.

Things the framework layer still has to own:

- **Threads.** With Frame Generation on, `OnFrame` runs on Streamline's present thread while
  `OnWindowMessage` runs on the window thread. Input handed to the GUI must be queued or locked.
- **Generated frames.** Like the Steam overlay, the overlay is drawn on top of every presented image,
  generated ones included.
- **Reinitialising.** `OnAttach` fires again after a swap chain change (Frame Generation toggle, HDR
  toggle, buffer count change) and is preceded by `OnDetach`. A plain resize does not detach.

## Known limitations

- Vulkan builds only when `VULKAN_SDK` is set. A Vulkan device created before install gets a guessed
  physical device (discrete GPU first) and queue family (first graphics family), and a swap chain created
  before install is reported out of date once from `vkAcquireNextImageKHR` so the game recreates it.
  DXGI swap chains the Vulkan or OpenGL driver creates internally (NVIDIA layered present) are ignored.
- Direct3D 9, 10 and OpenGL were only tested in the GUI demo, not in real games. Direct3D 9 device reset
  has not been exercised.
- Input blocking is tested with fake drivers chained under the hooks (OmegaWare-Framework's
  `OmegaWareTests`), not with real controllers. GameInput reading callbacks and DirectInput 7 are not blocked.
- Game depth is only passed on for Direct3D 11 and 9 (and shared implicitly on OpenGL), see Game depth.
  It was verified against the GUI demo's depth buffer, not a real game.
- The object scan depends on DXGI storing the queue pointer inside the swap chain. It does on the
  Windows 11 build this was verified on; if a future DXGI changes that, early install (creation hook)
  still works.
- Swap chain recreation (released and created again on the same window) has only been exercised by the
  self-test. Palworld never recreates its swap chain, even when Frame Generation is toggled.
- Direct3D 11on12 and multi-adapter swap chains are untested.

## Licence

GNU GPL v3, matching the OmegaWare Framework. See `LICENCE`. SafetyHook is BSL-1.0 and Zydis is MIT.
