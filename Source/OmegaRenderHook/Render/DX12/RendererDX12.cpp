#include "OmegaRenderHook/Render/DX12/RendererDX12.hpp"
#include "OmegaRenderHook/Core/Log.hpp"

#include <algorithm>

namespace OmegaRenderHook
{
	namespace
	{
		constexpr UINT uMaxSlots = 8;
		constexpr DWORD uSlotWaitMs = 500;

		[[nodiscard]] D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* pResource, D3D12_RESOURCE_STATES eBefore, D3D12_RESOURCE_STATES eAfter)
		{
			D3D12_RESOURCE_BARRIER stBarrier{};
			stBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			stBarrier.Transition.pResource = pResource;
			stBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			stBarrier.Transition.StateBefore = eBefore;
			stBarrier.Transition.StateAfter = eAfter;
			return stBarrier;
		}
	}

	std::unique_ptr<clRendererDX12> clRendererDX12::Create(IDXGISwapChain3* pSwapChain, ComPtr<ID3D12Device> pDevice, ComPtr<ID3D12CommandQueue> pQueue, EQueueSource eSource, const DXGI_SWAP_CHAIN_DESC& stDesc)
	{
		std::unique_ptr<clRendererDX12> pRenderer(new clRendererDX12());
		pRenderer->m_pDevice = std::move(pDevice);
		pRenderer->m_pQueue = std::move(pQueue);

		ID3D12Device* pRawDevice = pRenderer->m_pDevice.Get();
		const UINT uBufferCount = std::max<UINT>(stDesc.BufferCount, 1);

		D3D12_DESCRIPTOR_HEAP_DESC stHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, uBufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
		if (FAILED(pRawDevice->CreateDescriptorHeap(&stHeap, IID_PPV_ARGS(&pRenderer->m_pRenderTargetHeap))))
			return nullptr;
		pRenderer->m_uRenderTargetSize = pRawDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		if (FAILED(pRawDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pRenderer->m_pFence))))
			return nullptr;
		pRenderer->m_hFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!pRenderer->m_hFenceEvent)
			return nullptr;

		pRenderer->m_aSlots.resize(std::clamp<UINT>(uBufferCount + 1, 2, uMaxSlots));
		for (stSlot& rSlot : pRenderer->m_aSlots)
		{
			if (FAILED(pRawDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&rSlot.pAllocator))))
				return nullptr;
			if (FAILED(pRawDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, rSlot.pAllocator.Get(), nullptr, IID_PPV_ARGS(&rSlot.pCommandList))))
				return nullptr;
			rSlot.pCommandList->Close();
		}

		stDeviceDX12& rDevice = pRenderer->m_stDevice;
		rDevice.pDevice = pRawDevice;
		rDevice.pQueue = pRenderer->m_pQueue.Get();
		rDevice.pSwapChain = pSwapChain;
		rDevice.hWindow = stDesc.OutputWindow;
		rDevice.eFormat = stDesc.BufferDesc.Format;
		rDevice.uWidth = stDesc.BufferDesc.Width;
		rDevice.uHeight = stDesc.BufferDesc.Height;
		rDevice.uBufferCount = stDesc.BufferCount;
		rDevice.uFramesInFlight = static_cast<UINT>(pRenderer->m_aSlots.size());
		rDevice.eQueueSource = eSource;
		return pRenderer;
	}

	clRendererDX12::~clRendererDX12()
	{
		WaitIdle(2000);
		if (m_hFenceEvent)
			CloseHandle(m_hFenceEvent);
	}

	bool clRendererDX12::WaitForValue(UINT64 uValue, DWORD uTimeoutMs)
	{
		if (m_pFence->GetCompletedValue() >= uValue)
			return true;
		if (FAILED(m_pFence->SetEventOnCompletion(uValue, m_hFenceEvent)))
			return false;
		return WaitForSingleObject(m_hFenceEvent, uTimeoutMs) == WAIT_OBJECT_0;
	}

	bool clRendererDX12::WaitIdle(DWORD uTimeoutMs)
	{
		if (!m_pFence)
			return true;
		const bool bIdle = WaitForValue(m_uFenceValue, uTimeoutMs);
		if (!bIdle)
			Log(ELogLevel::Warning, "timed out waiting for overlay GPU work (fence {} of {})", m_pFence->GetCompletedValue(), m_uFenceValue);
		return bIdle;
	}

	ERenderResult clRendererDX12::Render(IDXGISwapChain3* pSwapChain, const DXGI_SWAP_CHAIN_DESC& stDesc, clRenderListener* pListener, stFault& rFault)
	{
		if (stDesc.BufferCount != m_stDevice.uBufferCount || stDesc.BufferDesc.Format != m_stDevice.eFormat)
			return ERenderResult::Rebuild;
		if (FAILED(m_pDevice->GetDeviceRemovedReason()))
			return ERenderResult::DeviceLost;

		m_stDevice.pSwapChain = pSwapChain;
		m_stDevice.uWidth = stDesc.BufferDesc.Width;
		m_stDevice.uHeight = stDesc.BufferDesc.Height;

		const UINT uSlot = static_cast<UINT>(m_uFrameNumber % m_aSlots.size());
		stSlot& rSlot = m_aSlots[uSlot];
		if (!WaitForValue(rSlot.uFenceValue, uSlotWaitMs))
			return ERenderResult::Skipped;
		if (FAILED(rSlot.pAllocator->Reset()))
			return ERenderResult::Skipped;
		ID3D12GraphicsCommandList* pList = rSlot.pCommandList.Get();
		if (FAILED(pList->Reset(rSlot.pAllocator.Get(), nullptr)))
			return ERenderResult::Skipped;

		const UINT uBackBuffer = pSwapChain->GetCurrentBackBufferIndex();
		ComPtr<ID3D12Resource> pBackBuffer;
		if (uBackBuffer >= m_stDevice.uBufferCount || FAILED(pSwapChain->GetBuffer(uBackBuffer, IID_PPV_ARGS(&pBackBuffer))))
		{
			pList->Close();
			return ERenderResult::Skipped;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE hRenderTarget = m_pRenderTargetHeap->GetCPUDescriptorHandleForHeapStart();
		hRenderTarget.ptr += static_cast<SIZE_T>(uBackBuffer) * m_uRenderTargetSize;
		m_pDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, hRenderTarget);

		const D3D12_RESOURCE_BARRIER stToTarget = Transition(pBackBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		pList->ResourceBarrier(1, &stToTarget);
		pList->OMSetRenderTargets(1, &hRenderTarget, FALSE, nullptr);
		const D3D12_VIEWPORT stViewport{ 0.f, 0.f, static_cast<float>(m_stDevice.uWidth), static_cast<float>(m_stDevice.uHeight), 0.f, 1.f };
		const D3D12_RECT stScissor{ 0, 0, static_cast<LONG>(m_stDevice.uWidth), static_cast<LONG>(m_stDevice.uHeight) };
		pList->RSSetViewports(1, &stViewport);
		pList->RSSetScissorRects(1, &stScissor);

		if (pListener)
		{
			stFrameDX12 stFrame;
			stFrame.pDevice = &m_stDevice;
			stFrame.pCommandList = pList;
			stFrame.pBackBuffer = pBackBuffer.Get();
			stFrame.hRenderTarget = hRenderTarget;
			stFrame.uBackBufferIndex = uBackBuffer;
			stFrame.uFrameSlot = uSlot;
			stFrame.uFrameNumber = m_uFrameNumber;
			auto fnFrame = [&] { pListener->OnFrame(stFrame); };
			if (!InvokeGuarded(fnFrame, rFault))
			{
				pList->Close();
				return ERenderResult::Faulted;
			}
		}

		const D3D12_RESOURCE_BARRIER stToPresent = Transition(pBackBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
		pList->ResourceBarrier(1, &stToPresent);
		if (FAILED(pList->Close()))
		{
			Log(ELogLevel::Error, "overlay command list failed to close, dropping the frame");
			return ERenderResult::Skipped;
		}

		ID3D12CommandList* const aLists[] = { pList };
		m_pQueue->ExecuteCommandLists(1, aLists);
		m_pQueue->Signal(m_pFence.Get(), ++m_uFenceValue);
		rSlot.uFenceValue = m_uFenceValue;
		++m_uFrameNumber;
		return ERenderResult::Rendered;
	}
}
