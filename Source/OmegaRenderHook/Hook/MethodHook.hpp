#pragma once

#include "OmegaRenderHook/Hook/InlineHook.hpp"
#include "OmegaRenderHook/Hook/VTableHook.hpp"

#include <cstddef>
#include <string_view>

namespace OmegaRenderHook
{
	enum class EHookMethod
	{
		VTable,
		Inline
	};

	[[nodiscard]] constexpr std::string_view ToString(EHookMethod eMethod)
	{
		return eMethod == EHookMethod::VTable ? "vtable" : "inline";
	}

	class clMethodHook
	{
	public:
		[[nodiscard]] bool Install(EHookMethod eMethod, void** ppVTable, std::size_t uIndex, void* pDetour);
		bool Disable();
		void Release();

		[[nodiscard]] bool IsInstalled() const { return m_clVTable.IsInstalled() || m_clInline.IsInstalled(); }
		[[nodiscard]] void* Identity() const { return m_pIdentity; }

		template <typename T>
		[[nodiscard]] T Original() const
		{
			return m_clInline.IsInstalled() ? m_clInline.Original<T>() : m_clVTable.Original<T>();
		}

	private:
		clVTableHook m_clVTable;
		clInlineHook m_clInline;
		void* m_pIdentity = nullptr;
	};
}
