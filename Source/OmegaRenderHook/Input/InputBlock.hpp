#pragma once

namespace OmegaRenderHook
{
	struct stInputBlock
	{
		bool bKeyboard = false;
		bool bMouse = false;
		bool bGamepad = false;

		[[nodiscard]] bool operator==(const stInputBlock&) const = default;
	};

	namespace InputBlock
	{
		bool Install();
		bool Uninstall();
		[[nodiscard]] bool IsInstalled();
		void SetBlocking(const stInputBlock& stBlock);
		[[nodiscard]] stInputBlock GetBlocking();
	}
}
