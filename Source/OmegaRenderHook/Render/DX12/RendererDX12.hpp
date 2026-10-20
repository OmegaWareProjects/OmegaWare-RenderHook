#pragma once

#include "OmegaRenderHook/Core/Guarded.hpp"
#include "OmegaRenderHook/Render/Pipeline.hpp"
#include "OmegaRenderHook/Render/RenderResult.hpp"

#include <wrl/client.h>

#include <memory>
#include <vector>

namespace OmegaRenderHook
{
	class clRendererDX12
	{
	public:
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		[[nodiscard]] static std::unique_ptr<clRendererDX12> Create(IDXGISwapChain3* pSwapChain, ComPtr<ID3D12Device> pDevice, ComPtr<ID3D12CommandQueue> pQueue, EQueueSource eSource, const DXGI_SWAP_CHAIN_DESC& stDesc);

		~clRendererDX12();

		clRendererDX12(const clRendererDX12&) = delete;
		clRendererDX12& operator=(const clRendererDX12&) = delete;

		[[nodiscard]] ERenderResult Render(IDXGISwapChain3* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, clRenderListener* pListener, stFault& rFault);
		bool WaitIdle(DWORD uTimeoutMs);

		[[nodiscard]] const stDeviceDX12& Device() const { return m_stDevice; }
		[[nodiscard]] ID3D12CommandQueue* Queue() const { return m_pQueue.Get(); }

	private:
		clRendererDX12() = default;

		struct stSlot
		{
			ComPtr<ID3D12CommandAllocator> pAllocator;
			ComPtr<ID3D12GraphicsCommandList> pCommandList;
			UINT64 uFenceValue = 0;
		};

		[[nodiscard]] bool WaitForValue(UINT64 uValue, DWORD uTimeoutMs);

		ComPtr<ID3D12Device> m_pDevice;
		ComPtr<ID3D12CommandQueue> m_pQueue;
		ComPtr<ID3D12DescriptorHeap> m_pRenderTargetHeap;
		ComPtr<ID3D12Fence> m_pFence;
		HANDLE m_hFenceEvent = nullptr;
		UINT64 m_uFenceValue = 0;
		UINT m_uRenderTargetSize = 0;
		std::vector<stSlot> m_aSlots;
		std::uint64_t m_uFrameNumber = 0;
		stDeviceDX12 m_stDevice;
	};
}
