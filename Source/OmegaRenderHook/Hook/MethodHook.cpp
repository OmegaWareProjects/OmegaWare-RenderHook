#include "OmegaRenderHook/Hook/MethodHook.hpp"

namespace OmegaRenderHook
{
	bool clMethodHook::Install(EHookMethod eMethod, void** ppVTable, std::size_t uIndex, void* pDetour)
	{
		if (IsInstalled() || !ppVTable)
			return false;

		if (eMethod == EHookMethod::VTable)
		{
			if (!m_clVTable.Install(ppVTable, uIndex, pDetour))
				return false;
			m_pIdentity = &ppVTable[uIndex];
			return true;
		}

		if (!m_clInline.Install(ppVTable[uIndex], pDetour))
			return false;
		m_pIdentity = m_clInline.Target();
		return true;
	}

	bool clMethodHook::Disable()
	{
		const bool bVTable = m_clVTable.Uninstall();
		const bool bInline = m_clInline.Disable();
		return bVTable && bInline;
	}

	void clMethodHook::Release()
	{
		m_clInline.Uninstall();
		if (!m_clVTable.IsInstalled())
			m_pIdentity = nullptr;
	}
}
