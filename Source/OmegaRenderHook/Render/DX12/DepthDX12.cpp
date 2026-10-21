#include "OmegaRenderHook/Render/DX12/DepthDX12.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Hook/InlineHook.hpp"
#include "OmegaRenderHook/Hook/MethodHook.hpp"
#include "OmegaRenderHook/Render/DepthCommon.hpp"
#include "OmegaRenderHook/Render/PipelineInternal.hpp"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OmegaRenderHook::DepthDX12
{
	namespace
	{
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		namespace Slots
		{
			constexpr std::size_t uCreateDepthStencilView = 21;
			constexpr std::size_t uCopyDescriptors = 23;
			constexpr std::size_t uCopyDescriptorsSimple = 24;
			constexpr std::size_t uRSSetViewports = 21;
			constexpr std::size_t uResourceBarrier = 26;
			constexpr std::size_t uOMSetRenderTargets = 46;
			constexpr std::size_t uClearDepthStencilView = 47;
			constexpr std::size_t uBeginRenderPass = 68;
			constexpr std::size_t uBarrier = 80;
		}

		constexpr std::uint64_t uRetireFrames = 8;

		using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
		using CreateDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
		using CopyDescriptorsFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
		using CopyDescriptorsSimpleFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
		using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
		using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
		using BeginRenderPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
		using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList7*, UINT32, const D3D12_BARRIER_GROUP*);

		struct stBound
		{
			ID3D12GraphicsCommandList* pList = nullptr;
			ID3D12Resource* pResource = nullptr;
		};

		thread_local stBound t_stBound;

		struct stRetired
		{
			std::uint64_t uFrame = 0;
			ComPtr<ID3D12Resource> pResource;
		};

		struct stState
		{
			std::mutex mtxHooks;
			clInlineHook clCreateDevice;
			bool bDeviceHooked = false;
			bool bListsHooked = false;
			clMethodHook clCreateDepthStencilView;
			clMethodHook clCopyDescriptors;
			clMethodHook clCopyDescriptorsSimple;
			clMethodHook clSetViewports;
			clMethodHook clResourceBarrier;
			clMethodHook clSetTargets;
			clMethodHook clClearDepth;
			clMethodHook clBeginRenderPass;
			clMethodHook clBarrier;
			std::atomic<UINT> uDepthIncrement{ 0 };

			std::shared_mutex mtxViews;
			std::unordered_map<SIZE_T, ID3D12Resource*> mapViews;

			std::mutex mtxDepth;
			ComPtr<ID3D12Device> pDevice;
			UINT uFramesInFlight = 0;
			UINT uTargetWidth = 0;
			UINT uTargetHeight = 0;
			std::uint64_t uFrame = 0;

			std::unordered_map<ID3D12Resource*, std::uint32_t> mapScores;
			ComPtr<ID3D12Resource> pBest;
			std::uint32_t uBestScore = 0;

			std::atomic<ID3D12Resource*> pChosenRaw{ nullptr };
			ComPtr<ID3D12Resource> pChosen;
			D3D12_RESOURCE_DESC stChosen{};
			D3D12_RESOURCE_STATES eChosenState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			bool bChosenUnsupported = false;
			bool bReversedKnown = false;
			bool bReversedZ = false;
			int iReversedVotes = 0;
			bool bCaptured = false;
			DepthCommon::stRect stViewport;

			ComPtr<ID3D12Resource> pSnapshot;
			ComPtr<ID3D12Resource> pResolved;
			UINT uResolvedWidth = 0;
			UINT uResolvedHeight = 0;
			ComPtr<ID3D12DescriptorHeap> pDepthHeap;
			ComPtr<ID3D12DescriptorHeap> pShaderHeap;
			UINT uShaderIncrement = 0;
			ComPtr<ID3D12RootSignature> pRootSignature;
			ComPtr<ID3D12PipelineState> pPipeline;
			bool bPipelineFailed = false;
			std::vector<stRetired> aRetired;
			DepthCommon::clSwitchVote clVote;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		[[nodiscard]] bool IsOurs()
		{
			return PipelineInternal::clOwnWork::IsActive();
		}

		[[nodiscard]] ID3D12Resource* ResourceOfView(SIZE_T uHandle)
		{
			stState& rState = State();
			std::shared_lock lckViews(rState.mtxViews);
			const auto itView = rState.mapViews.find(uHandle);
			return itView == rState.mapViews.end() ? nullptr : itView->second;
		}

		void Retire(stState& rState, ComPtr<ID3D12Resource>& rResource)
		{
			if (rResource)
				rState.aRetired.push_back({ rState.uFrame, std::move(rResource) });
			rResource.Reset();
		}

		void NoteDepthUse(ID3D12Resource* pResource)
		{
			if (!pResource)
				return;
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (!rState.uTargetWidth)
				return;
			auto [itScore, bInserted] = rState.mapScores.try_emplace(pResource, 0);
			if (bInserted)
			{
				const D3D12_RESOURCE_DESC stDesc = pResource->GetDesc();
				const bool bQualifies = stDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && stDesc.DepthOrArraySize == 1 && (stDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
					DepthCommon::FormatsOf(stDesc.Format).eTypeless != DXGI_FORMAT_UNKNOWN &&
					DepthCommon::Qualifies(static_cast<UINT>(stDesc.Width), stDesc.Height, stDesc.SampleDesc.Count, rState.uTargetWidth, rState.uTargetHeight);
				if (!bQualifies)
					itScore->second = UINT32_MAX;
			}
			if (itScore->second == UINT32_MAX)
				return;
			const std::uint32_t uScore = ++itScore->second;
			if (uScore > rState.uBestScore)
			{
				rState.uBestScore = uScore;
				if (rState.pBest.Get() != pResource)
					rState.pBest = pResource;
			}
		}

		void NoteClear(ID3D12Resource* pResource, float flDepth)
		{
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (pResource && pResource == rState.pChosen.Get())
			{
				rState.bReversedKnown = true;
				rState.bReversedZ = flDepth < .5f;
				return;
			}
			rState.iReversedVotes = std::clamp(rState.iReversedVotes + (flDepth < .5f ? 1 : -1), -64, 64);
		}

		void SetBound(ID3D12GraphicsCommandList* pList, ID3D12Resource* pResource)
		{
			t_stBound = { pList, pResource };
			NoteDepthUse(pResource);
		}

		void STDMETHODCALLTYPE HookCreateDepthStencilView(ID3D12Device* pDevice, ID3D12Resource* pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC* pDesc, D3D12_CPU_DESCRIPTOR_HANDLE hView)
		{
			clHookScope clScope;
			stState& rState = State();
			rState.clCreateDepthStencilView.Original<CreateDepthStencilViewFn>()(pDevice, pResource, pDesc, hView);
			if (IsOurs())
				return;
			std::unique_lock lckViews(rState.mtxViews);
			if (pResource)
				rState.mapViews[hView.ptr] = pResource;
			else
				rState.mapViews.erase(hView.ptr);
		}

		void CopyViews(stState& rState, SIZE_T uDest, SIZE_T uSource, UINT uCount)
		{
			const UINT uIncrement = rState.uDepthIncrement.load(std::memory_order_acquire);
			if (!uIncrement)
				return;
			for (UINT i = 0; i < uCount; ++i)
			{
				const SIZE_T uFrom = uSource + static_cast<SIZE_T>(i) * uIncrement;
				const SIZE_T uTo = uDest + static_cast<SIZE_T>(i) * uIncrement;
				if (const auto itView = rState.mapViews.find(uFrom); itView != rState.mapViews.end())
					rState.mapViews[uTo] = itView->second;
				else
					rState.mapViews.erase(uTo);
			}
		}

		void STDMETHODCALLTYPE HookCopyDescriptors(ID3D12Device* pDevice, UINT uDestRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pDestStarts, const UINT* pDestSizes, UINT uSourceRanges, const D3D12_CPU_DESCRIPTOR_HANDLE* pSourceStarts, const UINT* pSourceSizes, D3D12_DESCRIPTOR_HEAP_TYPE eType)
		{
			clHookScope clScope;
			stState& rState = State();
			rState.clCopyDescriptors.Original<CopyDescriptorsFn>()(pDevice, uDestRanges, pDestStarts, pDestSizes, uSourceRanges, pSourceStarts, pSourceSizes, eType);
			if (eType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || IsOurs() || !pDestStarts || !pSourceStarts)
				return;
			std::unique_lock lckViews(rState.mtxViews);
			UINT uDestRange = 0, uDestOffset = 0, uSourceRange = 0, uSourceOffset = 0;
			while (uDestRange < uDestRanges && uSourceRange < uSourceRanges)
			{
				const UINT uDestSize = pDestSizes ? pDestSizes[uDestRange] : 1;
				const UINT uSourceSize = pSourceSizes ? pSourceSizes[uSourceRange] : 1;
				const UINT uCount = std::min(uDestSize - uDestOffset, uSourceSize - uSourceOffset);
				const UINT uIncrement = rState.uDepthIncrement.load(std::memory_order_acquire);
				CopyViews(rState, pDestStarts[uDestRange].ptr + static_cast<SIZE_T>(uDestOffset) * uIncrement, pSourceStarts[uSourceRange].ptr + static_cast<SIZE_T>(uSourceOffset) * uIncrement, uCount);
				uDestOffset += uCount;
				uSourceOffset += uCount;
				if (uDestOffset >= uDestSize)
				{
					++uDestRange;
					uDestOffset = 0;
				}
				if (uSourceOffset >= uSourceSize)
				{
					++uSourceRange;
					uSourceOffset = 0;
				}
				if (!uCount)
					break;
			}
		}

		void STDMETHODCALLTYPE HookCopyDescriptorsSimple(ID3D12Device* pDevice, UINT uCount, D3D12_CPU_DESCRIPTOR_HANDLE hDest, D3D12_CPU_DESCRIPTOR_HANDLE hSource, D3D12_DESCRIPTOR_HEAP_TYPE eType)
		{
			clHookScope clScope;
			stState& rState = State();
			rState.clCopyDescriptorsSimple.Original<CopyDescriptorsSimpleFn>()(pDevice, uCount, hDest, hSource, eType);
			if (eType != D3D12_DESCRIPTOR_HEAP_TYPE_DSV || IsOurs())
				return;
			std::unique_lock lckViews(rState.mtxViews);
			CopyViews(rState, hDest.ptr, hSource.ptr, uCount);
		}

		void STDMETHODCALLTYPE HookSetViewports(ID3D12GraphicsCommandList* pList, UINT uCount, const D3D12_VIEWPORT* pViewports)
		{
			clHookScope clScope;
			stState& rState = State();
			if (!IsOurs() && uCount && pViewports && t_stBound.pList == pList && t_stBound.pResource && t_stBound.pResource == rState.pChosenRaw.load(std::memory_order_acquire))
			{
				std::scoped_lock lckDepth(rState.mtxDepth);
				rState.stViewport.Include(pViewports[0].TopLeftX, pViewports[0].TopLeftY, pViewports[0].Width, pViewports[0].Height);
			}
			rState.clSetViewports.Original<RSSetViewportsFn>()(pList, uCount, pViewports);
		}

		[[nodiscard]] D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* pResource, UINT uSubresource, D3D12_RESOURCE_STATES eBefore, D3D12_RESOURCE_STATES eAfter)
		{
			D3D12_RESOURCE_BARRIER stBarrier{};
			stBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			stBarrier.Transition.pResource = pResource;
			stBarrier.Transition.Subresource = uSubresource;
			stBarrier.Transition.StateBefore = eBefore;
			stBarrier.Transition.StateAfter = eAfter;
			return stBarrier;
		}

		[[nodiscard]] D3D12_RESOURCE_BARRIER FlushBarrier()
		{
			D3D12_RESOURCE_BARRIER stBarrier{};
			stBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
			return stBarrier;
		}

		void RecordCopy(ID3D12GraphicsCommandList* pList, ResourceBarrierFn pfnBarrier, ID3D12Resource* pSource, D3D12_RESOURCE_STATES eSourceState, ID3D12Resource* pSnapshot)
		{
			const bool bTransition = !(eSourceState & D3D12_RESOURCE_STATE_COPY_SOURCE);
			std::array<D3D12_RESOURCE_BARRIER, 2> aBefore{ FlushBarrier(), Transition(pSource, 0, eSourceState, D3D12_RESOURCE_STATE_COPY_SOURCE) };
			pfnBarrier(pList, bTransition ? 2 : 1, aBefore.data());
			D3D12_TEXTURE_COPY_LOCATION stDest{};
			stDest.pResource = pSnapshot;
			stDest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			D3D12_TEXTURE_COPY_LOCATION stSource{};
			stSource.pResource = pSource;
			stSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			pList->CopyTextureRegion(&stDest, 0, 0, 0, &stSource, nullptr);
			if (bTransition)
			{
				const D3D12_RESOURCE_BARRIER stAfter = Transition(pSource, 0, D3D12_RESOURCE_STATE_COPY_SOURCE, eSourceState);
				pfnBarrier(pList, 1, &stAfter);
			}
		}

		void STDMETHODCALLTYPE HookResourceBarrier(ID3D12GraphicsCommandList* pList, UINT uCount, const D3D12_RESOURCE_BARRIER* pBarriers)
		{
			clHookScope clScope;
			stState& rState = State();
			const auto pfnOriginal = rState.clResourceBarrier.Original<ResourceBarrierFn>();
			if (IsOurs() || !pBarriers)
				return pfnOriginal(pList, uCount, pBarriers);
			ID3D12Resource* const pChosen = rState.pChosenRaw.load(std::memory_order_acquire);
			for (UINT i = 0; i < uCount; ++i)
			{
				const D3D12_RESOURCE_BARRIER& rBarrier = pBarriers[i];
				if (rBarrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !rBarrier.Transition.pResource)
					continue;
				const D3D12_RESOURCE_TRANSITION_BARRIER& rTransition = rBarrier.Transition;
				if (rTransition.StateAfter & D3D12_RESOURCE_STATE_DEPTH_WRITE)
					NoteDepthUse(rTransition.pResource);
				if (rTransition.pResource != pChosen || (rTransition.Subresource != 0 && rTransition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
					continue;
				std::scoped_lock lckDepth(rState.mtxDepth);
				if (rState.pChosen.Get() != pChosen)
					continue;
				if ((rTransition.StateBefore & D3D12_RESOURCE_STATE_DEPTH_WRITE) && !(rTransition.StateAfter & D3D12_RESOURCE_STATE_DEPTH_WRITE) && rState.pSnapshot && !rState.bChosenUnsupported)
				{
					RecordCopy(pList, pfnOriginal, pChosen, rTransition.StateBefore, rState.pSnapshot.Get());
					rState.bCaptured = true;
				}
				rState.eChosenState = rTransition.StateAfter;
			}
			pfnOriginal(pList, uCount, pBarriers);
		}

		void STDMETHODCALLTYPE HookSetTargets(ID3D12GraphicsCommandList* pList, UINT uTargets, const D3D12_CPU_DESCRIPTOR_HANDLE* pTargets, BOOL bSingle, const D3D12_CPU_DESCRIPTOR_HANDLE* pDepth)
		{
			clHookScope clScope;
			State().clSetTargets.Original<OMSetRenderTargetsFn>()(pList, uTargets, pTargets, bSingle, pDepth);
			if (!IsOurs())
				SetBound(pList, pDepth ? ResourceOfView(pDepth->ptr) : nullptr);
		}

		void STDMETHODCALLTYPE HookClearDepth(ID3D12GraphicsCommandList* pList, D3D12_CPU_DESCRIPTOR_HANDLE hView, D3D12_CLEAR_FLAGS eFlags, FLOAT flDepth, UINT8 uStencil, UINT uRects, const D3D12_RECT* pRects)
		{
			clHookScope clScope;
			if (!IsOurs() && (eFlags & D3D12_CLEAR_FLAG_DEPTH))
				NoteClear(ResourceOfView(hView.ptr), flDepth);
			State().clClearDepth.Original<ClearDepthStencilViewFn>()(pList, hView, eFlags, flDepth, uStencil, uRects, pRects);
		}

		void STDMETHODCALLTYPE HookBeginRenderPass(ID3D12GraphicsCommandList4* pList, UINT uTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* pTargets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* pDepth, D3D12_RENDER_PASS_FLAGS eFlags)
		{
			clHookScope clScope;
			State().clBeginRenderPass.Original<BeginRenderPassFn>()(pList, uTargets, pTargets, pDepth, eFlags);
			if (IsOurs())
				return;
			ID3D12Resource* pResource = pDepth ? ResourceOfView(pDepth->cpuDescriptor.ptr) : nullptr;
			if (pDepth && pDepth->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
				NoteClear(pResource, pDepth->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth);
			SetBound(pList, pResource);
		}

		void STDMETHODCALLTYPE HookBarrier(ID3D12GraphicsCommandList7* pList, UINT32 uGroups, const D3D12_BARRIER_GROUP* pGroups)
		{
			clHookScope clScope;
			stState& rState = State();
			ID3D12Resource* const pChosen = rState.pChosenRaw.load(std::memory_order_acquire);
			if (!IsOurs() && pChosen && pGroups)
			{
				for (UINT32 i = 0; i < uGroups; ++i)
				{
					if (pGroups[i].Type != D3D12_BARRIER_TYPE_TEXTURE)
						continue;
					for (UINT32 j = 0; j < pGroups[i].NumBarriers; ++j)
					{
						if (pGroups[i].pTextureBarriers[j].pResource != pChosen)
							continue;
						std::scoped_lock lckDepth(rState.mtxDepth);
						if (!rState.bChosenUnsupported)
							Log(ELogLevel::Warning, "the game moves its depth buffer with enhanced barriers, world drawing will not be occluded");
						rState.bChosenUnsupported = true;
					}
				}
			}
			rState.clBarrier.Original<BarrierFn>()(pList, uGroups, pGroups);
		}

		bool InstallDeviceHooks(ID3D12Device* pDevice)
		{
			stState& rState = State();
			std::scoped_lock lckHooks(rState.mtxHooks);
			if (rState.bDeviceHooked || !pDevice)
				return rState.bDeviceHooked;
			rState.uDepthIncrement.store(pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV), std::memory_order_release);
			constexpr EHookMethod eMethod = EHookMethod::Inline;
			void** ppVTable = *reinterpret_cast<void***>(pDevice);
			const bool bHooked = rState.clCreateDepthStencilView.Install(eMethod, ppVTable, Slots::uCreateDepthStencilView, reinterpret_cast<void*>(&HookCreateDepthStencilView)) &&
				rState.clCopyDescriptors.Install(eMethod, ppVTable, Slots::uCopyDescriptors, reinterpret_cast<void*>(&HookCopyDescriptors)) &&
				rState.clCopyDescriptorsSimple.Install(eMethod, ppVTable, Slots::uCopyDescriptorsSimple, reinterpret_cast<void*>(&HookCopyDescriptorsSimple));
			if (!bHooked)
			{
				for (clMethodHook* pHook : { &rState.clCreateDepthStencilView, &rState.clCopyDescriptors, &rState.clCopyDescriptorsSimple })
				{
					pHook->Disable();
					pHook->Release();
				}
				Log(ELogLevel::Warning, "could not hook Direct3D 12 depth view creation");
				return false;
			}
			rState.bDeviceHooked = true;
			Log(ELogLevel::Debug, "Direct3D 12 depth view creation hooked");
			return true;
		}

		bool InstallListHooks(ID3D12Device* pDevice)
		{
			stState& rState = State();
			std::scoped_lock lckHooks(rState.mtxHooks);
			if (rState.bListsHooked)
				return true;
			ComPtr<ID3D12CommandAllocator> pAllocator;
			ComPtr<ID3D12GraphicsCommandList> pList;
			if (FAILED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&pAllocator))) ||
				FAILED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pAllocator.Get(), nullptr, IID_PPV_ARGS(&pList))))
				return false;
			pList->Close();
			constexpr EHookMethod eMethod = EHookMethod::Inline;
			void** ppVTable = *reinterpret_cast<void***>(pList.Get());
			bool bHooked = rState.clSetViewports.Install(eMethod, ppVTable, Slots::uRSSetViewports, reinterpret_cast<void*>(&HookSetViewports)) &&
				rState.clResourceBarrier.Install(eMethod, ppVTable, Slots::uResourceBarrier, reinterpret_cast<void*>(&HookResourceBarrier)) &&
				rState.clSetTargets.Install(eMethod, ppVTable, Slots::uOMSetRenderTargets, reinterpret_cast<void*>(&HookSetTargets)) &&
				rState.clClearDepth.Install(eMethod, ppVTable, Slots::uClearDepthStencilView, reinterpret_cast<void*>(&HookClearDepth));
			ComPtr<ID3D12GraphicsCommandList4> pList4;
			if (bHooked && SUCCEEDED(pList.As(&pList4)))
				bHooked = rState.clBeginRenderPass.Install(eMethod, ppVTable, Slots::uBeginRenderPass, reinterpret_cast<void*>(&HookBeginRenderPass));
			ComPtr<ID3D12GraphicsCommandList7> pList7;
			if (bHooked && SUCCEEDED(pList.As(&pList7)))
				bHooked = rState.clBarrier.Install(eMethod, ppVTable, Slots::uBarrier, reinterpret_cast<void*>(&HookBarrier));
			if (!bHooked)
			{
				for (clMethodHook* pHook : { &rState.clSetViewports, &rState.clResourceBarrier, &rState.clSetTargets, &rState.clClearDepth, &rState.clBeginRenderPass, &rState.clBarrier })
				{
					pHook->Disable();
					pHook->Release();
				}
				Log(ELogLevel::Warning, "could not hook Direct3D 12 command lists for depth, world drawing will not be occluded");
				return false;
			}
			rState.bListsHooked = true;
			Log(ELogLevel::Debug, "Direct3D 12 depth tracking hooked");
			return true;
		}

		HRESULT WINAPI HookCreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL eLevel, REFIID riid, void** ppDevice)
		{
			clHookScope clScope;
			const HRESULT hResult = State().clCreateDevice.Original<CreateDeviceFn>()(pAdapter, eLevel, riid, ppDevice);
			if (SUCCEEDED(hResult) && ppDevice && *ppDevice)
			{
				ComPtr<ID3D12Device> pDevice;
				if (SUCCEEDED(static_cast<IUnknown*>(*ppDevice)->QueryInterface(IID_PPV_ARGS(&pDevice))))
					InstallDeviceHooks(pDevice.Get());
			}
			return hResult;
		}

		[[nodiscard]] ComPtr<ID3DBlob> Compile(const char* szEntry, const char* szTarget)
		{
			ComPtr<ID3DBlob> pBlob;
			ComPtr<ID3DBlob> pErrors;
			if (FAILED(D3DCompile(DepthCommon::szResolveHLSL, sizeof(DepthCommon::szResolveHLSL) - 1, nullptr, nullptr, nullptr, szEntry, szTarget, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pBlob, &pErrors)))
			{
				Log(ELogLevel::Error, "depth resolve shader failed to compile: {}", pErrors ? static_cast<const char*>(pErrors->GetBufferPointer()) : "unknown error");
				return nullptr;
			}
			return pBlob;
		}

		[[nodiscard]] bool EnsurePipeline(stState& rState)
		{
			if (rState.pPipeline)
				return true;
			if (rState.bPipelineFailed || !rState.pDevice)
				return false;
			rState.bPipelineFailed = true;
			ID3D12Device* pDevice = rState.pDevice.Get();

			D3D12_DESCRIPTOR_HEAP_DESC stDepthHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
			D3D12_DESCRIPTOR_HEAP_DESC stShaderHeap{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, std::max<UINT>(rState.uFramesInFlight, 1), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			if (FAILED(pDevice->CreateDescriptorHeap(&stDepthHeap, IID_PPV_ARGS(&rState.pDepthHeap))) || FAILED(pDevice->CreateDescriptorHeap(&stShaderHeap, IID_PPV_ARGS(&rState.pShaderHeap))))
				return false;
			rState.uShaderIncrement = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

			D3D12_DESCRIPTOR_RANGE stRange{};
			stRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			stRange.NumDescriptors = 1;
			std::array<D3D12_ROOT_PARAMETER, 2> aParameters{};
			aParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			aParameters[0].Constants.Num32BitValues = 4;
			aParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			aParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			aParameters[1].DescriptorTable.NumDescriptorRanges = 1;
			aParameters[1].DescriptorTable.pDescriptorRanges = &stRange;
			aParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_ROOT_SIGNATURE_DESC stRoot{};
			stRoot.NumParameters = static_cast<UINT>(aParameters.size());
			stRoot.pParameters = aParameters.data();
			ComPtr<ID3DBlob> pRootBlob;
			ComPtr<ID3DBlob> pRootErrors;
			if (FAILED(D3D12SerializeRootSignature(&stRoot, D3D_ROOT_SIGNATURE_VERSION_1, &pRootBlob, &pRootErrors)) ||
				FAILED(pDevice->CreateRootSignature(0, pRootBlob->GetBufferPointer(), pRootBlob->GetBufferSize(), IID_PPV_ARGS(&rState.pRootSignature))))
				return false;

			const ComPtr<ID3DBlob> pVertex = Compile("VSMain", "vs_5_0");
			const ComPtr<ID3DBlob> pPixel = Compile("PSMain", "ps_5_0");
			if (!pVertex || !pPixel)
				return false;
			D3D12_GRAPHICS_PIPELINE_STATE_DESC stPipeline{};
			stPipeline.pRootSignature = rState.pRootSignature.Get();
			stPipeline.VS = { pVertex->GetBufferPointer(), pVertex->GetBufferSize() };
			stPipeline.PS = { pPixel->GetBufferPointer(), pPixel->GetBufferSize() };
			stPipeline.SampleMask = UINT_MAX;
			stPipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			stPipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			stPipeline.DepthStencilState.DepthEnable = TRUE;
			stPipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			stPipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			stPipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			stPipeline.NumRenderTargets = 0;
			stPipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			stPipeline.SampleDesc.Count = 1;
			if (FAILED(pDevice->CreateGraphicsPipelineState(&stPipeline, IID_PPV_ARGS(&rState.pPipeline))))
				return false;
			rState.bPipelineFailed = false;
			return true;
		}

		[[nodiscard]] bool EnsureResolved(stState& rState)
		{
			if (rState.pResolved && rState.uResolvedWidth == rState.uTargetWidth && rState.uResolvedHeight == rState.uTargetHeight)
				return true;
			Retire(rState, rState.pResolved);
			const D3D12_HEAP_PROPERTIES stHeap{ D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC stDesc{};
			stDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			stDesc.Width = rState.uTargetWidth;
			stDesc.Height = rState.uTargetHeight;
			stDesc.DepthOrArraySize = 1;
			stDesc.MipLevels = 1;
			stDesc.Format = DXGI_FORMAT_D32_FLOAT;
			stDesc.SampleDesc.Count = 1;
			stDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
			D3D12_CLEAR_VALUE stClear{};
			stClear.Format = DXGI_FORMAT_D32_FLOAT;
			if (FAILED(rState.pDevice->CreateCommittedResource(&stHeap, D3D12_HEAP_FLAG_NONE, &stDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &stClear, IID_PPV_ARGS(&rState.pResolved))))
				return false;
			{
				PipelineInternal::clOwnWork clOwn;
				rState.pDevice->CreateDepthStencilView(rState.pResolved.Get(), nullptr, rState.pDepthHeap->GetCPUDescriptorHandleForHeapStart());
			}
			rState.uResolvedWidth = rState.uTargetWidth;
			rState.uResolvedHeight = rState.uTargetHeight;
			return true;
		}

		[[nodiscard]] bool CreateSnapshot(stState& rState)
		{
			Retire(rState, rState.pSnapshot);
			const D3D12_HEAP_PROPERTIES stHeap{ D3D12_HEAP_TYPE_DEFAULT };
			D3D12_RESOURCE_DESC stDesc{};
			stDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			stDesc.Width = rState.stChosen.Width;
			stDesc.Height = rState.stChosen.Height;
			stDesc.DepthOrArraySize = 1;
			stDesc.MipLevels = 1;
			stDesc.Format = DepthCommon::FormatsOf(rState.stChosen.Format).eTypeless;
			stDesc.SampleDesc.Count = 1;
			stDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			return SUCCEEDED(rState.pDevice->CreateCommittedResource(&stHeap, D3D12_HEAP_FLAG_NONE, &stDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rState.pSnapshot)));
		}

		void SelectNext(stState& rState)
		{
			const auto itChosen = rState.pChosen ? rState.mapScores.find(rState.pChosen.Get()) : rState.mapScores.end();
			const std::uint32_t uChosenScore = itChosen == rState.mapScores.end() || itChosen->second == UINT32_MAX ? 0 : itChosen->second;
			const bool bWins = rState.pBest && rState.pBest.Get() != rState.pChosen.Get() && rState.pDevice && DepthCommon::ShouldSwitch(rState.uBestScore, uChosenScore);
			if (rState.clVote.Vote(reinterpret_cast<std::uint64_t>(rState.pBest.Get()), bWins, rState.pChosen != nullptr))
			{
				Retire(rState, rState.pChosen);
				rState.pChosen = rState.pBest;
				rState.stChosen = rState.pChosen->GetDesc();
				rState.eChosenState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
				rState.bChosenUnsupported = false;
				rState.bReversedKnown = false;
				if (!CreateSnapshot(rState))
				{
					Log(ELogLevel::Warning, "could not create the Direct3D 12 depth copy, world drawing will not be occluded");
					Retire(rState, rState.pChosen);
				}
				else
					Log(ELogLevel::Debug, "Direct3D 12 scene depth is {} ({}x{}, format {})", static_cast<void*>(rState.pChosen.Get()), rState.stChosen.Width, rState.stChosen.Height, static_cast<int>(rState.stChosen.Format));
				rState.pChosenRaw.store(rState.pChosen.Get(), std::memory_order_release);
			}
			rState.mapScores.clear();
			rState.pBest.Reset();
			rState.uBestScore = 0;
			rState.bCaptured = false;
			rState.stViewport = {};
			++rState.uFrame;
			std::erase_if(rState.aRetired, [&](const stRetired& rRetired) { return rState.uFrame - rRetired.uFrame > uRetireFrames; });
		}

		void ClearTracking(stState& rState)
		{
			rState.clVote.Reset();
			rState.mapScores.clear();
			rState.pBest.Reset();
			rState.uBestScore = 0;
			rState.pChosenRaw.store(nullptr, std::memory_order_release);
			Retire(rState, rState.pChosen);
			Retire(rState, rState.pSnapshot);
			rState.stChosen = {};
			rState.eChosenState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			rState.bChosenUnsupported = false;
			rState.bReversedKnown = false;
			rState.bCaptured = false;
			rState.stViewport = {};
		}

		void ReleaseResources(stState& rState, bool bRetired)
		{
			ClearTracking(rState);
			Retire(rState, rState.pResolved);
			rState.uResolvedWidth = 0;
			rState.uResolvedHeight = 0;
			rState.pPipeline.Reset();
			rState.pRootSignature.Reset();
			rState.pDepthHeap.Reset();
			rState.pShaderHeap.Reset();
			rState.bPipelineFailed = false;
			if (bRetired)
				rState.aRetired.clear();
		}
	}

	void PrepareEarly()
	{
		stState& rState = State();
		std::scoped_lock lckHooks(rState.mtxHooks);
		if (rState.clCreateDevice.IsInstalled())
			return;
		const HMODULE hModule = GetModuleHandleW(L"d3d12.dll");
		void* pCreateDevice = hModule ? reinterpret_cast<void*>(GetProcAddress(hModule, "D3D12CreateDevice")) : nullptr;
		if (pCreateDevice && rState.clCreateDevice.Install(pCreateDevice, reinterpret_cast<void*>(&HookCreateDevice)))
			Log(ELogLevel::Debug, "Direct3D 12 device creation hooked early for depth tracking");
	}

	bool Install(ID3D12Device* pDevice, UINT uFramesInFlight)
	{
		if (!pDevice || !InstallDeviceHooks(pDevice) || !InstallListHooks(pDevice))
			return false;
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (rState.pDevice.Get() != pDevice || rState.uFramesInFlight != uFramesInFlight)
		{
			ReleaseResources(rState, false);
			rState.pDevice = pDevice;
			rState.uFramesInFlight = uFramesInFlight;
		}
		return true;
	}

	void SetTarget(UINT uWidth, UINT uHeight)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (rState.uTargetWidth == uWidth && rState.uTargetHeight == uHeight)
			return;
		rState.uTargetWidth = uWidth;
		rState.uTargetHeight = uHeight;
		ClearTracking(rState);
	}

	stDepth Resolve(ID3D12GraphicsCommandList* pList, UINT uFrameSlot)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		stDepth stResult;
		const auto itUsed = rState.pChosen ? rState.mapScores.find(rState.pChosen.Get()) : rState.mapScores.end();
		const bool bUsedThisFrame = itUsed != rState.mapScores.end() && itUsed->second != UINT32_MAX && itUsed->second > 0;
		if (rState.pChosen && rState.pSnapshot && !rState.bChosenUnsupported && (rState.bCaptured || bUsedThisFrame) && EnsurePipeline(rState) && EnsureResolved(rState))
		{
			const auto pfnBarrier = rState.clResourceBarrier.IsInstalled() ? rState.clResourceBarrier.Original<ResourceBarrierFn>() : nullptr;
			auto fnBarrier = [&](UINT uCount, const D3D12_RESOURCE_BARRIER* pBarriers)
			{
				if (pfnBarrier)
					pfnBarrier(pList, uCount, pBarriers);
				else
					pList->ResourceBarrier(uCount, pBarriers);
			};
			if (!rState.bCaptured)
			{
				const bool bTransition = !(rState.eChosenState & D3D12_RESOURCE_STATE_COPY_SOURCE);
				std::array<D3D12_RESOURCE_BARRIER, 2> aBefore{ FlushBarrier(), Transition(rState.pChosen.Get(), 0, rState.eChosenState, D3D12_RESOURCE_STATE_COPY_SOURCE) };
				fnBarrier(bTransition ? 2 : 1, aBefore.data());
				D3D12_TEXTURE_COPY_LOCATION stDest{};
				stDest.pResource = rState.pSnapshot.Get();
				stDest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				D3D12_TEXTURE_COPY_LOCATION stSource{};
				stSource.pResource = rState.pChosen.Get();
				stSource.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				pList->CopyTextureRegion(&stDest, 0, 0, 0, &stSource, nullptr);
				if (bTransition)
				{
					const D3D12_RESOURCE_BARRIER stAfter = Transition(rState.pChosen.Get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE, rState.eChosenState);
					fnBarrier(1, &stAfter);
				}
			}

			std::array<D3D12_RESOURCE_BARRIER, 2> aToRead{ FlushBarrier(), Transition(rState.pSnapshot.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) };
			fnBarrier(2, aToRead.data());

			const UINT uSlot = rState.uFramesInFlight ? uFrameSlot % rState.uFramesInFlight : 0;
			D3D12_CPU_DESCRIPTOR_HANDLE hShaderCpu = rState.pShaderHeap->GetCPUDescriptorHandleForHeapStart();
			hShaderCpu.ptr += static_cast<SIZE_T>(uSlot) * rState.uShaderIncrement;
			D3D12_GPU_DESCRIPTOR_HANDLE hShaderGpu = rState.pShaderHeap->GetGPUDescriptorHandleForHeapStart();
			hShaderGpu.ptr += static_cast<UINT64>(uSlot) * rState.uShaderIncrement;
			D3D12_SHADER_RESOURCE_VIEW_DESC stView{};
			stView.Format = DepthCommon::FormatsOf(rState.stChosen.Format).eShaderView;
			stView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			stView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			stView.Texture2D.MipLevels = 1;
			rState.pDevice->CreateShaderResourceView(rState.pSnapshot.Get(), &stView, hShaderCpu);

			const D3D12_CPU_DESCRIPTOR_HANDLE hDepth = rState.pDepthHeap->GetCPUDescriptorHandleForHeapStart();
			ID3D12DescriptorHeap* const aHeaps[] = { rState.pShaderHeap.Get() };
			pList->SetDescriptorHeaps(1, aHeaps);
			pList->SetGraphicsRootSignature(rState.pRootSignature.Get());
			pList->SetPipelineState(rState.pPipeline.Get());
			const DepthCommon::stResolveConstants stConstants = DepthCommon::ResolveConstants(rState.stViewport, static_cast<UINT>(rState.stChosen.Width), rState.stChosen.Height, rState.uTargetWidth, rState.uTargetHeight);
			pList->SetGraphicsRoot32BitConstants(0, 4, stConstants.aSourceRect, 0);
			pList->SetGraphicsRootDescriptorTable(1, hShaderGpu);
			pList->OMSetRenderTargets(0, nullptr, FALSE, &hDepth);
			const D3D12_VIEWPORT stViewport{ 0.f, 0.f, static_cast<float>(rState.uTargetWidth), static_cast<float>(rState.uTargetHeight), 0.f, 1.f };
			const D3D12_RECT stScissor{ 0, 0, static_cast<LONG>(rState.uTargetWidth), static_cast<LONG>(rState.uTargetHeight) };
			pList->RSSetViewports(1, &stViewport);
			pList->RSSetScissorRects(1, &stScissor);
			pList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			pList->DrawInstanced(3, 1, 0, 0);

			const D3D12_RESOURCE_BARRIER stToCopy = Transition(rState.pSnapshot.Get(), 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			fnBarrier(1, &stToCopy);
			stResult = { hDepth, DXGI_FORMAT_D32_FLOAT, rState.bReversedKnown ? rState.bReversedZ : rState.iReversedVotes > 0 };
		}
		SelectNext(rState);
		return stResult;
	}

	void Reset()
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		ReleaseResources(rState, false);
		rState.pDevice.Reset();
	}

	bool Disable()
	{
		stState& rState = State();
		std::scoped_lock lckHooks(rState.mtxHooks);
		bool bRestored = rState.clCreateDevice.Disable();
		for (clMethodHook* pHook : { &rState.clCreateDepthStencilView, &rState.clCopyDescriptors, &rState.clCopyDescriptorsSimple, &rState.clSetViewports, &rState.clResourceBarrier, &rState.clSetTargets, &rState.clClearDepth, &rState.clBeginRenderPass, &rState.clBarrier })
			bRestored = pHook->Disable() && bRestored;
		return bRestored;
	}

	void Release(bool bDrained)
	{
		stState& rState = State();
		{
			std::scoped_lock lckDepth(rState.mtxDepth);
			ReleaseResources(rState, true);
			rState.pDevice.Reset();
			rState.uTargetWidth = 0;
			rState.uTargetHeight = 0;
		}
		{
			std::unique_lock lckViews(rState.mtxViews);
			rState.mapViews.clear();
		}
		if (!bDrained)
			return;
		std::scoped_lock lckHooks(rState.mtxHooks);
		rState.clCreateDevice.Uninstall();
		for (clMethodHook* pHook : { &rState.clCreateDepthStencilView, &rState.clCopyDescriptors, &rState.clCopyDescriptorsSimple, &rState.clSetViewports, &rState.clResourceBarrier, &rState.clSetTargets, &rState.clClearDepth, &rState.clBeginRenderPass, &rState.clBarrier })
			pHook->Release();
		rState.bDeviceHooked = false;
		rState.bListsHooked = false;
	}
}
