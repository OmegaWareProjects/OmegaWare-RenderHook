#include "OmegaRenderHook/Hook/InlineHook.hpp"
#include "OmegaRenderHook/Core/Log.hpp"

namespace OmegaRenderHook
{
	bool clInlineHook::Install(void* pTarget, void* pDetour)
	{
		if (IsInstalled() || !pTarget || !pDetour)
			return false;

		auto expHook = safetyhook::InlineHook::create(pTarget, pDetour);
		if (!expHook)
		{
			Log(ELogLevel::Error, "inline hook on {} failed (safetyhook error {})", pTarget, static_cast<int>(expHook.error().type));
			return false;
		}

		m_clHook = std::move(*expHook);
		return true;
	}

	bool clInlineHook::Disable()
	{
		if (!IsInstalled() || !m_clHook.enabled())
			return true;
		return m_clHook.disable().has_value();
	}

	bool clInlineHook::Uninstall()
	{
		m_clHook.reset();
		return true;
	}
}
