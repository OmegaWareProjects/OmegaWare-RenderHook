#include "OmegaRenderHook/Hook/HookScope.hpp"

#include <thread>

namespace OmegaRenderHook
{
	bool WaitForCounterDrain(const std::atomic<int>& rCounter, std::chrono::milliseconds msTimeout)
	{
		const auto tpDeadline = std::chrono::steady_clock::now() + msTimeout;
		while (rCounter.load(std::memory_order_acquire) > 0)
		{
			if (std::chrono::steady_clock::now() >= tpDeadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		return rCounter.load(std::memory_order_acquire) == 0;
	}
}
