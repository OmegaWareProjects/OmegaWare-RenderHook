#pragma once

#include "OmegaRenderHook/Core/Guarded.hpp"
#include "OmegaRenderHook/Render/Pipeline.hpp"

#include <atomic>
#include <chrono>
#include <string_view>

namespace OmegaRenderHook::PipelineInternal
{
	[[nodiscard]] bool IsActive();
	[[nodiscard]] bool AcquireWindow(HWND hWindow, ERenderApi eApi, UINT uWidth, UINT uHeight);
	[[nodiscard]] clRenderListener* Listener();
	void ReportFault(std::string_view strWhere, const stFault& stFault);
	void CountFrame(bool bRendered);
	void ReleaseApi(ERenderApi eApi);

	class clLayeredScope
	{
	public:
		clLayeredScope() { ++s_iDepth; }
		~clLayeredScope() { --s_iDepth; }

		clLayeredScope(const clLayeredScope&) = delete;
		clLayeredScope& operator=(const clLayeredScope&) = delete;

		[[nodiscard]] static bool IsActive() { return s_iDepth > 0; }

	private:
		static inline thread_local int s_iDepth = 0;
	};

	class clOwnWork
	{
	public:
		clOwnWork() { ++s_iDepth; }
		~clOwnWork() { --s_iDepth; }

		clOwnWork(const clOwnWork&) = delete;
		clOwnWork& operator=(const clOwnWork&) = delete;

		[[nodiscard]] static bool IsActive() { return s_iDepth > 0; }

	private:
		static inline thread_local int s_iDepth = 0;
	};

	template <typename F>
	void CallListener(std::string_view strWhere, F&& fnCall)
	{
		clRenderListener* pListener = Listener();
		if (!pListener)
			return;
		auto fnInvoke = [&] { fnCall(pListener); };
		stFault stFault;
		if (!InvokeGuarded(fnInvoke, stFault))
			ReportFault(strWhere, stFault);
	}

	class clDetachRequest
	{
	public:
		void Request() { m_bRequested.store(true, std::memory_order_release); m_bDone.store(false, std::memory_order_release); }
		void Clear() { m_bRequested.store(false, std::memory_order_release); m_bDone.store(false, std::memory_order_release); }
		[[nodiscard]] bool IsRequested() const { return m_bRequested.load(std::memory_order_acquire); }
		void MarkDone() { m_bDone.store(true, std::memory_order_release); }
		[[nodiscard]] bool WaitDone(std::chrono::milliseconds msTimeout) const;

	private:
		std::atomic<bool> m_bRequested = false;
		std::atomic<bool> m_bDone = false;
	};
}
