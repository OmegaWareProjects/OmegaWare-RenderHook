#pragma once

#include "OmegaRenderHook/Hook/MethodHook.hpp"

#include <d3d11.h>

namespace OmegaRenderHook::DepthDX11
{
	struct stDepth
	{
		ID3D11DepthStencilView* pView = nullptr;
		bool bReversedZ = false;
	};

	bool Install(ID3D11DeviceContext* pContext);
	void SetTarget(UINT uWidth, UINT uHeight);
	[[nodiscard]] stDepth Resolve();
	void Reset();
	bool Disable();
	void Release(bool bDrained);
}
