#pragma once

#include "OmegaRenderHook/Core/Guarded.hpp"
#include "OmegaRenderHook/Render/Pipeline.hpp"
#include "OmegaRenderHook/Render/RenderResult.hpp"

#include <wrl/client.h>

#include <memory>

namespace OmegaRenderHook
{
	class clRendererDX10
	{
	public:
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		[[nodiscard]] static std::unique_ptr<clRendererDX10> Create(IDXGISwapChain* pSwapChain, ComPtr<ID3D10Device> pDevice, const DXGI_SWAP_CHAIN_DESC& stDesc);

		clRendererDX10(const clRendererDX10&) = delete;
		clRendererDX10& operator=(const clRendererDX10&) = delete;

		[[nodiscard]] ERenderResult Render(IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, clRenderListener* pListener, stFault& rFault);

		[[nodiscard]] const stDeviceDX10& Device() const { return m_stDevice; }

	private:
		clRendererDX10() = default;

		ComPtr<ID3D10Device> m_pDevice;
		std::uint64_t m_uFrameNumber = 0;
		stDeviceDX10 m_stDevice;
	};
}
