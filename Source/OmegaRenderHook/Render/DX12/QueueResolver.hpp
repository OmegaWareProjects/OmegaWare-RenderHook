#pragma once

#include "OmegaRenderHook/Render/Pipeline.hpp"

#include <wrl/client.h>

#include <span>
#include <unordered_set>

namespace OmegaRenderHook
{
	struct stResolvedQueue
	{
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> pQueue;
		EQueueSource eSource = EQueueSource::None;
	};

	[[nodiscard]] bool IsPresentQueue(ID3D12CommandQueue* pQueue, ID3D12Device* pDevice);

	[[nodiscard]] stResolvedQueue ResolveQueueFromSwapChain(IDXGISwapChain* pSwapChain, ID3D12Device* pDevice, std::span<void* const> aQueueVTables, const std::unordered_set<void*>& setLiveQueues);
}
