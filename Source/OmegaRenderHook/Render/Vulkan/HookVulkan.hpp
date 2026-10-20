#pragma once

#include <chrono>

namespace OmegaRenderHook::HookVulkan
{
	bool Install();
	void RequestDetach(std::chrono::milliseconds msTimeout);
	bool Disable();
	void Release(bool bDrained);
}
