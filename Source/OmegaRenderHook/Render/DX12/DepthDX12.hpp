#pragma once

#include <d3d12.h>

namespace OmegaRenderHook::DepthDX12
{
	struct stDepth
	{
		D3D12_CPU_DESCRIPTOR_HANDLE hView{};
		DXGI_FORMAT eFormat = DXGI_FORMAT_UNKNOWN;
		bool bReversedZ = false;
	};

	void PrepareEarly();
	bool Install(ID3D12Device* pDevice, UINT uFramesInFlight);
	void SetTarget(UINT uWidth, UINT uHeight);
	[[nodiscard]] stDepth Resolve(ID3D12GraphicsCommandList* pList, UINT uFrameSlot);
	void Reset();
	bool Disable();
	void Release(bool bDrained);
}
