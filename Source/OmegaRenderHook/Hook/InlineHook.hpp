#pragma once

#include <safetyhook.hpp>

namespace OmegaRenderHook
{
	class clInlineHook
	{
	public:
		clInlineHook() = default;
		~clInlineHook() = default;

		clInlineHook(const clInlineHook&) = delete;
		clInlineHook& operator=(const clInlineHook&) = delete;

		[[nodiscard]] bool Install(void* pTarget, void* pDetour);
		bool Disable();
		bool Uninstall();

		[[nodiscard]] bool IsInstalled() const { return static_cast<bool>(m_clHook); }
		[[nodiscard]] void* Target() const { return m_clHook.target(); }

		template <typename T>
		[[nodiscard]] T Original() const
		{
			return m_clHook.original<T>();
		}

	private:
		safetyhook::InlineHook m_clHook;
	};
}
