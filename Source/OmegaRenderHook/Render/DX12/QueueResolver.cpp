#include "OmegaRenderHook/Render/DX12/QueueResolver.hpp"
#include "OmegaRenderHook/Core/Log.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace OmegaRenderHook
{
	namespace
	{
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		constexpr std::size_t uRootScanBytes = 0x800;
		constexpr std::size_t uChildScanBytes = 0x200;
		constexpr std::size_t uMaxChildren = 128;

		[[nodiscard]] bool IsReadableProtection(DWORD uProtect)
		{
			if (uProtect & (PAGE_NOACCESS | PAGE_GUARD))
				return false;
			return (uProtect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
		}

		[[nodiscard]] std::vector<std::uintptr_t> ReadPointers(std::uintptr_t uAddress, std::size_t uBytes)
		{
			if (uAddress < 0x10000 || uAddress % sizeof(void*) != 0)
				return {};

			MEMORY_BASIC_INFORMATION stInfo{};
			if (!VirtualQuery(reinterpret_cast<const void*>(uAddress), &stInfo, sizeof(stInfo)))
				return {};
			if (stInfo.State != MEM_COMMIT || !IsReadableProtection(stInfo.Protect))
				return {};

			const std::uintptr_t uRegionEnd = reinterpret_cast<std::uintptr_t>(stInfo.BaseAddress) + stInfo.RegionSize;
			const std::size_t uReadable = std::min<std::size_t>(uBytes, uRegionEnd - uAddress) / sizeof(void*) * sizeof(void*);
			if (uReadable == 0)
				return {};

			std::vector<std::uintptr_t> aPointers(uReadable / sizeof(void*));
			SIZE_T uRead = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(uAddress), aPointers.data(), uReadable, &uRead))
				return {};
			aPointers.resize(uRead / sizeof(void*));
			return aPointers;
		}

		[[nodiscard]] bool HasQueueVTable(std::uintptr_t uObject, std::span<void* const> aQueueVTables)
		{
			const std::vector<std::uintptr_t> aHeader = ReadPointers(uObject, sizeof(void*));
			if (aHeader.empty())
				return false;
			const void* pVTable = reinterpret_cast<const void*>(aHeader[0]);
			return std::ranges::find(aQueueVTables, pVTable) != aQueueVTables.end();
		}

		[[nodiscard]] ComPtr<ID3D12CommandQueue> AcceptCandidate(std::uintptr_t uObject, ID3D12Device* pDevice)
		{
			auto* pQueue = reinterpret_cast<ID3D12CommandQueue*>(uObject);
			if (!IsPresentQueue(pQueue, pDevice))
				return nullptr;
			return ComPtr<ID3D12CommandQueue>(pQueue);
		}
	}

	bool IsPresentQueue(ID3D12CommandQueue* pQueue, ID3D12Device* pDevice)
	{
		if (!pQueue || !pDevice)
			return false;
		if (pQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
			return false;
		ComPtr<ID3D12Device> pOwner;
		if (FAILED(pQueue->GetDevice(IID_PPV_ARGS(&pOwner))))
			return false;
		return pOwner.Get() == pDevice;
	}

	stResolvedQueue ResolveQueueFromSwapChain(IDXGISwapChain* pSwapChain, ID3D12Device* pDevice, std::span<void* const> aQueueVTables, const std::unordered_set<void*>& setLiveQueues)
	{
		ComPtr<ID3D12CommandQueue> pDeviceQueue;
		if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&pDeviceQueue))) && IsPresentQueue(pDeviceQueue.Get(), pDevice))
			return { pDeviceQueue, EQueueSource::SwapChainDevice };

		const std::vector<std::uintptr_t> aRoot = ReadPointers(reinterpret_cast<std::uintptr_t>(pSwapChain), uRootScanBytes);
		for (std::size_t i = 0; i < aRoot.size(); ++i)
		{
			if (!HasQueueVTable(aRoot[i], aQueueVTables))
				continue;
			if (ComPtr<ID3D12CommandQueue> pQueue = AcceptCandidate(aRoot[i], pDevice))
			{
				Log(ELogLevel::Debug, "present queue found at swap chain offset 0x{:X}", i * sizeof(void*));
				return { pQueue, EQueueSource::ObjectScan };
			}
		}

		if (setLiveQueues.empty())
			return {};

		std::size_t uChildren = 0;
		for (std::size_t i = 0; i < aRoot.size() && uChildren < uMaxChildren; ++i)
		{
			const std::vector<std::uintptr_t> aChild = ReadPointers(aRoot[i], uChildScanBytes);
			if (aChild.empty())
				continue;
			++uChildren;
			for (std::size_t j = 0; j < aChild.size(); ++j)
			{
				if (!setLiveQueues.contains(reinterpret_cast<void*>(aChild[j])) || !HasQueueVTable(aChild[j], aQueueVTables))
					continue;
				if (ComPtr<ID3D12CommandQueue> pQueue = AcceptCandidate(aChild[j], pDevice))
				{
					Log(ELogLevel::Debug, "present queue found at swap chain offset 0x{:X} -> 0x{:X}", i * sizeof(void*), j * sizeof(void*));
					return { pQueue, EQueueSource::ObjectScan };
				}
			}
		}
		return {};
	}
}
