#pragma once

#include "OmegaRenderHook/Hook/MethodHook.hpp"

#include <chrono>

namespace OmegaRenderHook::HookDX9
{
	bool Install(EHookMethod eMethod);
	void RequestDetach(std::chrono::milliseconds msTimeout);
	bool Disable();
	void Release(bool bDrained);
}
