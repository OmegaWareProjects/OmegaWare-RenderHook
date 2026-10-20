#include "OmegaRenderHook/Render/DX10/RendererDX10.hpp"

#include <array>

namespace OmegaRenderHook
{
	namespace
	{
		class clSavedTargets
		{
		public:
			explicit clSavedTargets(ID3D10Device* pDevice) : m_pDevice(pDevice)
			{
				m_pDevice->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, m_aTargets.data(), &m_pDepthStencil);
				m_uViewports = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
				m_pDevice->RSGetViewports(&m_uViewports, m_aViewports.data());
			}

			~clSavedTargets()
			{
				m_pDevice->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, m_aTargets.data(), m_pDepthStencil);
				m_pDevice->RSSetViewports(m_uViewports, m_aViewports.data());
				for (ID3D10RenderTargetView* pTarget : m_aTargets)
				{
					if (pTarget)
						pTarget->Release();
				}
				if (m_pDepthStencil)
					m_pDepthStencil->Release();
			}

			clSavedTargets(const clSavedTargets&) = delete;
			clSavedTargets& operator=(const clSavedTargets&) = delete;

		private:
			ID3D10Device* m_pDevice;
			std::array<ID3D10RenderTargetView*, D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT> m_aTargets{};
			ID3D10DepthStencilView* m_pDepthStencil = nullptr;
			std::array<D3D10_VIEWPORT, D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> m_aViewports{};
			UINT m_uViewports = 0;
		};
	}

	std::unique_ptr<clRendererDX10> clRendererDX10::Create(IDXGISwapChain* pSwapChain, ComPtr<ID3D10Device> pDevice, const DXGI_SWAP_CHAIN_DESC& stDesc)
	{
		std::unique_ptr<clRendererDX10> pRenderer(new clRendererDX10());
		pRenderer->m_pDevice = std::move(pDevice);

		stDeviceDX10& rDevice = pRenderer->m_stDevice;
		rDevice.pDevice = pRenderer->m_pDevice.Get();
		rDevice.pSwapChain = pSwapChain;
		rDevice.hWindow = stDesc.OutputWindow;
		rDevice.eFormat = stDesc.BufferDesc.Format;
		rDevice.uWidth = stDesc.BufferDesc.Width;
		rDevice.uHeight = stDesc.BufferDesc.Height;
		rDevice.uBufferCount = stDesc.BufferCount;
		return pRenderer;
	}

	ERenderResult clRendererDX10::Render(IDXGISwapChain* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, clRenderListener* pListener, stFault& rFault)
	{
		if (stDesc.BufferCount != m_stDevice.uBufferCount || stDesc.BufferDesc.Format != m_stDevice.eFormat)
			return ERenderResult::Rebuild;
		if (FAILED(m_pDevice->GetDeviceRemovedReason()))
			return ERenderResult::DeviceLost;

		m_stDevice.pSwapChain = pSwapChain;
		m_stDevice.uWidth = stDesc.BufferDesc.Width;
		m_stDevice.uHeight = stDesc.BufferDesc.Height;

		ComPtr<ID3D10Texture2D> pBackBuffer;
		if (FAILED(pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))))
			return ERenderResult::Skipped;
		ComPtr<ID3D10RenderTargetView> pRenderTarget;
		if (FAILED(m_pDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &pRenderTarget)))
			return ERenderResult::Skipped;

		{
			clSavedTargets clSaved(m_pDevice.Get());
			ID3D10RenderTargetView* const aTargets[] = { pRenderTarget.Get() };
			m_pDevice->OMSetRenderTargets(1, aTargets, nullptr);
			const D3D10_VIEWPORT stViewport{ 0, 0, m_stDevice.uWidth, m_stDevice.uHeight, 0.f, 1.f };
			m_pDevice->RSSetViewports(1, &stViewport);

			if (pListener)
			{
				stFrameDX10 stFrame;
				stFrame.pDevice = &m_stDevice;
				stFrame.pBackBuffer = pBackBuffer.Get();
				stFrame.pRenderTarget = pRenderTarget.Get();
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
