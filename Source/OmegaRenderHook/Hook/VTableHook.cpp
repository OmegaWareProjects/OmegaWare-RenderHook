#include "OmegaRenderHook/Hook/VTableHook.hpp"
#include "OmegaRenderHook/Core/Log.hpp"

#include <Windows.h>

namespace OmegaRenderHook
{
	namespace
	{
		[[nodiscard]] bool ExchangeSlot(void** ppSlot, void* pExpected, void* pReplacement)
		{
			DWORD uOldProtect = 0;
			if (!VirtualProtect(ppSlot, sizeof(void*), PAGE_READWRITE, &uOldProtect))
				return false;
			const void* pPrevious = InterlockedCompareExchangePointer(ppSlot, pReplacement, pExpected);
			DWORD uIgnored = 0;
			VirtualProtect(ppSlot, sizeof(void*), uOldProtect, &uIgnored);
			return pPrevious == pExpected;
		}
	}

	clVTableHook::~clVTableHook()
	{
		Uninstall();
	}

	bool clVTableHook::Install(void** ppVTable, std::size_t uIndex, void* pDetour)
	{
		if (m_ppSlot || !ppVTable || !pDetour)
			return false;

		void** ppSlot = &ppVTable[uIndex];
		void* pOriginal = *ppSlot;
		if (pOriginal == pDetour)
			return false;

		m_pOriginal.store(pOriginal, std::memory_order_release);
		if (!ExchangeSlot(ppSlot, pOriginal, pDetour))
		{
			m_pOriginal.store(nullptr, std::memory_order_release);
			Log(ELogLevel::Error, "vtable slot {} changed while hooking", static_cast<void*>(ppSlot));
			return false;
		}

		m_ppSlot = ppSlot;
		m_pDetour = pDetour;
		return true;
	}

	bool clVTableHook::Uninstall()
	{
		if (!m_ppSlot)
			return true;

		if (!ExchangeSlot(m_ppSlot, m_pDetour, m_pOriginal.load(std::memory_order_acquire)))
		{
			Log(ELogLevel::Warning, "vtable slot {} was re-hooked by someone else, leaving the detour in place", static_cast<void*>(m_ppSlot));
			return false;
		}

		m_ppSlot = nullptr;
		m_pDetour = nullptr;
		return true;
	}
}
