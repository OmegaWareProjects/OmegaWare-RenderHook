#include "OmegaRenderHook/Core/Guarded.hpp"

#include <Windows.h>

#include <atomic>

namespace OmegaRenderHook
{
	namespace
	{
		std::atomic<FaultObserver> g_pfnObserver = nullptr;

		int CaptureFault(const EXCEPTION_POINTERS* pPointers, stFault& rFault)
		{
			rFault.uCode = pPointers->ExceptionRecord->ExceptionCode;
			rFault.pAddress = pPointers->ExceptionRecord->ExceptionAddress;
			if (const FaultObserver pfnObserver = g_pfnObserver.load(std::memory_order_acquire))
				pfnObserver(pPointers);
			return EXCEPTION_EXECUTE_HANDLER;
		}
	}

	void SetFaultObserver(FaultObserver pfnObserver)
	{
		g_pfnObserver.store(pfnObserver, std::memory_order_release);
	}

	bool InvokeGuardedRaw(GuardedFunction pfnFunction, void* pContext, stFault& rFault)
	{
		__try
		{
			pfnFunction(pContext);
			return true;
		}
		__except (CaptureFault(GetExceptionInformation(), rFault))
		{
			return false;
		}
	}
}
