#include "OmegaRenderHook/Render/DX11/RendererDX11.hpp"
#include "OmegaRenderHook/Render/DX11/DepthDX11.hpp"

#include <array>

namespace OmegaRenderHook
{
	namespace
	{
		class clSavedTargets
		{
		public:
			explicit clSavedTargets(ID3D11DeviceContext* pContext) : m_pContext(pContext)
			{
				m_pContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, m_aTargets.data(), &m_pDepthStencil);
				m_uViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
				m_pContext->RSGetViewports(&m_uViewports, m_aViewports.data());
			}

			~clSavedTargets()
			{
				m_pContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, m_aTargets.data(), m_pDepthStencil);
				m_pContext->RSSetViewports(m_uViewports, m_aViewports.data());
				for (ID3D11RenderTargetView* pTarget : m_aTargets)
				{
					if (pTarget)
						pTarget->Release();
				}
				if (m_pDepthStencil)
					m_pDepthStencil->Release();
			}

			clSavedTargets(const clSavedTargets&) = delete;
			clSavedTargets& operator=(const clSavedTargets&) = delete;

			[[nodiscard]] ID3D11DepthStencilView* DepthStencil() const { return m_pDepthStencil; }

		private:
			ID3D11DeviceContext* m_pContext;
			std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> m_aTargets{};
			ID3D11DepthStencilView* m_pDepthStencil = nullptr;
			std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> m_aViewports{};
			UINT m_uViewports = 0;
		};
	}

	std::unique_ptr<clRendererDX11> clRendererDX11::Create(IDXGISwapChain* pSwapChain, ComPtr<ID3D11Device> pDevice, const DXGI_SWAP_CHAIN_DESC& stDesc)
	{
		std::unique_ptr<clRendererDX11> pRenderer(new clRendererDX11());
		pRenderer->m_pDevice = std::move(pDevice);
		pRenderer->m_pDevice->GetImmediateContext(&pRenderer->m_pContext);

		stDeviceDX11& rDevice = pRenderer->m_stDevice;
		rDevice.pDevice = pRenderer->m_pDevice.Get();
		rDevice.pContext = pRenderer->m_pContext.Get();
		rDevice.pSwapChain = pSwapChain;
		rDevice.hWindow = stDesc.OutputWindow;
		rDevice.eFormat = stDesc.BufferDesc.Format;
		rDevice.uWidth = stDesc.BufferDesc.Width;
		rDevice.uHeight = stDesc.BufferDesc.Height;
		rDevice.uBufferCount = stDesc.BufferCount;
		return pRenderer;
	}

	ERenderResult clRendererDX11::Render(IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, clRenderListener* pListener, stFault& rFault)
	{
		if (stDesc.BufferCount != m_stDevice.uBufferCount || stDesc.BufferDesc.Format != m_stDevice.eFormat)
			return ERenderResult::Rebuild;
		if (FAILED(m_pDevice->GetDeviceRemovedReason()))
			return ERenderResult::DeviceLost;

		m_stDevice.pSwapChain = pSwapChain;
		m_stDevice.uWidth = stDesc.BufferDesc.Width;
		m_stDevice.uHeight = stDesc.BufferDesc.Height;

		ComPtr<ID3D11Texture2D> pBackBuffer;
		if (FAILED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))))
			return ERenderResult::Skipped;
		ComPtr<ID3D11RenderTargetView> pRenderTarget;
		if (FAILED(m_pDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &pRenderTarget)))
			return ERenderResult::Skipped;

		D3D11_TEXTURE2D_DESC stBackBuffer{};
		pBackBuffer->GetDesc(&stBackBuffer);
		DepthDX11::SetTarget(stBackBuffer.Width, stBackBuffer.Height, stBackBuffer.SampleDesc.Count);

		{
			clSavedTargets clSaved(m_pContext.Get());
			const DepthDX11::stDepth stDepth = DepthDX11::Resolve(clSaved.DepthStencil());
			ID3D11RenderTargetView* const aTargets[] = { pRenderTarget.Get() };
			m_pContext->OMSetRenderTargets(1, aTargets, nullptr);
			const D3D11_VIEWPORT stViewport{ 0.f, 0.f, static_cast<float>(m_stDevice.uWidth), static_cast<float>(m_stDevice.uHeight), 0.f, 1.f };
			m_pContext->RSSetViewports(1, &stViewport);

			if (pListener)
			{
				stFrameDX11 stFrame;
				stFrame.pDevice = &m_stDevice;
				stFrame.pBackBuffer = pBackBuffer.Get();
				stFrame.pRenderTarget = pRenderTarget.Get();
				stFrame.pDepthStencil = stDepth.pView.Get();
				stFrame.bReversedZ = stDepth.bReversedZ;
				stFrame.uFrameNumber = m_uFrameNumber;
				auto fnFrame = [&] { pListener->OnFrame(stFrame); };
				if (!InvokeGuarded(fnFrame, rFault))
					return ERenderResult::Faulted;
			}
		}

		++m_uFrameNumber;
		return ERenderResult::Rendered;
	}
}
