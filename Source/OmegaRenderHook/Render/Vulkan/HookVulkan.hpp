#pragma once

#include <chrono>

namespace OmegaRenderHook::HookVulkan
{
	void PrepareEarly();
	void NoteDevice(void* hDevice, void* hPhysicalDevice);
	bool Install();
	void RequestDetach(std::chrono::milliseconds msTimeout);
	bool Disable();
	void Release(bool bDrained);
	bool DisableEarly();
	void ReleaseEarly(bool bDrained);
}
