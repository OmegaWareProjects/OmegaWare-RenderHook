#pragma once

#include "OmegaRenderHook/Core/Guarded.hpp"
#include "OmegaRenderHook/Render/Pipeline.hpp"
#include "OmegaRenderHook/Render/RenderResult.hpp"

#include <wrl/client.h>

#include <memory>

namespace OmegaRenderHook
{
	class clRendererDX11
	{
	public:
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		[[nodiscard]] static std::unique_ptr<clRendererDX11> Create(IDXGISwapChain* pSwapChain, ComPtr<ID3D11Device> pDevice, const DXGI_SWAP_CHAIN_DESC& stDesc);

		clRendererDX11(const clRendererDX11&) = delete;
		clRendererDX11& operator=(const clRendererDX11&) = delete;

		[[nodiscard]] ERenderResult Render(IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, clRenderListener* pListener, stFault& rFault);

		[[nodiscard]] const stDeviceDX11& Device() const { return m_stDevice; }

	private:
		clRendererDX11() = default;

		ComPtr<ID3D11Device> m_pDevice;
		ComPtr<ID3D11DeviceContext> m_pContext;
		std::uint64_t m_uFrameNumber = 0;
		stDeviceDX11 m_stDevice;
	};
}
