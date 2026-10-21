#pragma once

#include <Windows.h>

namespace OmegaRenderHook
{
	class clRenderListener;

	namespace WindowHook
	{
		[[nodiscard]] bool Attach(HWND hWindow, clRenderListener* pListener);
		bool Detach();
		void SetListener(clRenderListener* pListener);
		[[nodiscard]] bool IsAttached();
		[[nodiscard]] HWND Window();
		void ReleaseHeldInput(bool bKeyboard, bool bMouse);
		void Wake();
	}
}
