#pragma once

#include <cstdint>

namespace OmegaRenderHook
{
	struct stFault
	{
		std::uint32_t uCode = 0;
		void* pAddress = nullptr;
		bool bCppException = false;
	};

	using GuardedFunction = void (*)(void* pContext);
	using FaultObserver = void (*)(const void* pExceptionPointers);

	void SetFaultObserver(FaultObserver pfnObserver);

	[[nodiscard]] bool InvokeGuardedRaw(GuardedFunction pfnFunction, void* pContext, stFault& rFault);

	template <typename F>
	[[nodiscard]] bool InvokeGuarded(F& rFunction, stFault& rFault)
	{
		struct stContext
		{
			F* pFunction;
			stFault* pFault;
		} stState{ &rFunction, &rFault };

		const bool bCompleted = InvokeGuardedRaw([](void* pContext)
		{
			auto* pState = static_cast<stContext*>(pContext);
			try
			{
				(*pState->pFunction)();
			}
			catch (...)
			{
				pState->pFault->bCppException = true;
			}
		}, &stState, rFault);
		return bCompleted && !rFault.bCppException;
	}
}
