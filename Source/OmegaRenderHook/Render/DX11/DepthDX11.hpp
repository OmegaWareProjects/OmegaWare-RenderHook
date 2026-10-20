#pragma once

#include "OmegaRenderHook/Hook/MethodHook.hpp"

#include <d3d11.h>
#include <wrl/client.h>

namespace OmegaRenderHook::DepthDX11
{
	struct stDepth
	{
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> pView;
		bool bReversedZ = false;
	};

	bool Install(ID3D11DeviceContext* pContext);
	void SetTarget(UINT uWidth, UINT uHeight, UINT uSamples);
	[[nodiscard]] stDepth Resolve(ID3D11DepthStencilView* pBound);
	void Reset();
	bool Disable();
	void Release(bool bDrained);
}
