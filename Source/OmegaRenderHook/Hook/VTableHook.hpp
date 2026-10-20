#pragma once

#include <atomic>
#include <cstddef>

namespace OmegaRenderHook
{
	class clVTableHook
	{
	public:
		clVTableHook() = default;
		~clVTableHook();

		clVTableHook(const clVTableHook&) = delete;
		clVTableHook& operator=(const clVTableHook&) = delete;

		[[nodiscard]] bool Install(void** ppVTable, std::size_t uIndex, void* pDetour);
		bool Uninstall();

		[[nodiscard]] bool IsInstalled() const { return m_ppSlot != nullptr; }
		[[nodiscard]] void** Slot() const { return m_ppSlot; }

		template <typename T>
		[[nodiscard]] T Original() const
		{
			return reinterpret_cast<T>(m_pOriginal.load(std::memory_order_acquire));
		}

	private:
		void** m_ppSlot = nullptr;
		void* m_pDetour = nullptr;
		std::atomic<void*> m_pOriginal{ nullptr };
	};
}
