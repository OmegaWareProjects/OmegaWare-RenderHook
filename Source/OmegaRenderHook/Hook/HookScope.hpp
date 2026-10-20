#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace OmegaRenderHook
{
	enum class EHookDomain : std::uint8_t
	{
		Render,
		Input,
		Game,
		Engine,
	};

	[[nodiscard]] bool WaitForCounterDrain(const std::atomic<int>& rCounter, std::chrono::milliseconds msTimeout);

	template <EHookDomain eDomain>
	class clHookScopeOf
	{
	public:
		clHookScopeOf() { s_iActive.fetch_add(1, std::memory_order_acq_rel); }
		~clHookScopeOf() { s_iActive.fetch_sub(1, std::memory_order_acq_rel); }

		clHookScopeOf(const clHookScopeOf&) = delete;
		clHookScopeOf& operator=(const clHookScopeOf&) = delete;

		[[nodiscard]] static int Active() { return s_iActive.load(std::memory_order_acquire); }
		[[nodiscard]] static bool WaitForDrain(std::chrono::milliseconds msTimeout) { return WaitForCounterDrain(s_iActive, msTimeout); }

	private:
		static inline std::atomic<int> s_iActive{ 0 };
	};

	using clHookScope = clHookScopeOf<EHookDomain::Render>;
}
