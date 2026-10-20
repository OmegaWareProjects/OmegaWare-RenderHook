#include "OmegaRenderHook/Render/DX11/DepthDX11.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"

#include <atomic>
#include <mutex>

namespace OmegaRenderHook::DepthDX11
{
	namespace
	{
		namespace Slots
		{
			constexpr std::size_t uOMSetRenderTargets = 33;
			constexpr std::size_t uOMSetRenderTargetsAndUnorderedAccessViews = 34;
			constexpr std::size_t uClearDepthStencilView = 53;
		}

		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		using OMSetRenderTargetsAndUnorderedAccessViewsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
		using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

		struct stState
		{
			std::mutex mtxDepth;
			bool bInstalled = false;
			clMethodHook clSetTargets;
			clMethodHook clSetTargetsAndViews;
			clMethodHook clClearDepth;
			std::atomic<ID3D11DepthStencilView*> pLastSeen{ nullptr };
			UINT uWidth = 0;
			UINT uHeight = 0;
			UINT uSamples = 1;
			Microsoft::WRL::ComPtr<ID3D11DepthStencilView> pCandidate;
			bool bReversedZ = false;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		[[nodiscard]] bool Matches(const stState& rState, ID3D11DepthStencilView* pView)
		{
			if (!pView || !rState.uWidth)
				return false;
			Microsoft::WRL::ComPtr<ID3D11Resource> pResource;
			pView->GetResource(&pResource);
			Microsoft::WRL::ComPtr<ID3D11Texture2D> pTexture;
			if (!pResource || FAILED(pResource.As(&pTexture)))
				return false;
			D3D11_TEXTURE2D_DESC stDesc{};
			pTexture->GetDesc(&stDesc);
			return stDesc.Width == rState.uWidth && stDesc.Height == rState.uHeight && stDesc.SampleDesc.Count == rState.uSamples;
		}

		void NoteBound(ID3D11DepthStencilView* pView)
		{
			stState& rState = State();
			if (!pView || rState.pLastSeen.exchange(pView, std::memory_order_acq_rel) == pView)
				return;
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (rState.pCandidate.Get() != pView && Matches(rState, pView))
			{
				rState.pCandidate = pView;
				Log(ELogLevel::Debug, "Direct3D 11 scene depth is {}", static_cast<void*>(pView));
			}
		}

		void NoteClear(ID3D11DepthStencilView* pView, UINT uFlags, FLOAT flDepth)
		{
			if (!pView || !(uFlags & D3D11_CLEAR_DEPTH))
				return;
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (rState.pCandidate.Get() == pView || Matches(rState, pView))
			{
				rState.pCandidate = pView;
				rState.bReversedZ = flDepth < .5f;
			}
		}

		void STDMETHODCALLTYPE HookSetTargets(ID3D11DeviceContext* pContext, UINT uViews, ID3D11RenderTargetView* const* ppViews, ID3D11DepthStencilView* pDepth)
		{
			clHookScope clScope;
			NoteBound(pDepth);
			State().clSetTargets.Original<OMSetRenderTargetsFn>()(pContext, uViews, ppViews, pDepth);
		}

		void STDMETHODCALLTYPE HookSetTargetsAndViews(ID3D11DeviceContext* pContext, UINT uViews, ID3D11RenderTargetView* const* ppViews, ID3D11DepthStencilView* pDepth, UINT uStart, UINT uUnordered, ID3D11UnorderedAccessView* const* ppUnordered, const UINT* pCounts)
		{
			clHookScope clScope;
			if (uViews != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
				NoteBound(pDepth);
			State().clSetTargetsAndViews.Original<OMSetRenderTargetsAndUnorderedAccessViewsFn>()(pContext, uViews, ppViews, pDepth, uStart, uUnordered, ppUnordered, pCounts);
		}

		void STDMETHODCALLTYPE HookClearDepth(ID3D11DeviceContext* pContext, ID3D11DepthStencilView* pDepth, UINT uFlags, FLOAT flDepth, UINT8 uStencil)
		{
			clHookScope clScope;
			NoteClear(pDepth, uFlags, flDepth);
			State().clClearDepth.Original<ClearDepthStencilViewFn>()(pContext, pDepth, uFlags, flDepth, uStencil);
		}
	}

	bool Install(ID3D11DeviceContext* pContext)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (rState.bInstalled || !pContext)
			return rState.bInstalled;
		constexpr EHookMethod eMethod = EHookMethod::Inline;
		void** ppVTable = *reinterpret_cast<void***>(pContext);
		const bool bHooked = rState.clSetTargets.Install(eMethod, ppVTable, Slots::uOMSetRenderTargets, reinterpret_cast<void*>(&HookSetTargets)) &&
			rState.clSetTargetsAndViews.Install(eMethod, ppVTable, Slots::uOMSetRenderTargetsAndUnorderedAccessViews, reinterpret_cast<void*>(&HookSetTargetsAndViews)) &&
			rState.clClearDepth.Install(eMethod, ppVTable, Slots::uClearDepthStencilView, reinterpret_cast<void*>(&HookClearDepth));
		if (!bHooked)
		{
			rState.clSetTargets.Disable();
			rState.clSetTargetsAndViews.Disable();
			rState.clClearDepth.Disable();
			Log(ELogLevel::Warning, "could not hook the Direct3D 11 context for depth, world drawing will not be occluded");
			return false;
		}
		rState.bInstalled = true;
		Log(ELogLevel::Debug, "Direct3D 11 depth tracking hooked");
		return true;
	}

	void SetTarget(UINT uWidth, UINT uHeight, UINT uSamples)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (rState.uWidth == uWidth && rState.uHeight == uHeight && rState.uSamples == uSamples)
			return;
		rState.uWidth = uWidth;
		rState.uHeight = uHeight;
		rState.uSamples = uSamples;
		rState.pCandidate.Reset();
		rState.pLastSeen.store(nullptr, std::memory_order_release);
	}

	stDepth Resolve(ID3D11DepthStencilView* pBound)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (rState.pCandidate && Matches(rState, rState.pCandidate.Get()))
			return { rState.pCandidate, rState.bReversedZ };
		if (Matches(rState, pBound))
			return { pBound, rState.bReversedZ };
		return {};
	}

	void Reset()
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		rState.pCandidate.Reset();
		rState.pLastSeen.store(nullptr, std::memory_order_release);
		rState.bReversedZ = false;
	}

	bool Disable()
	{
		stState& rState = State();
		bool bRestored = rState.clSetTargets.Disable();
		bRestored = rState.clSetTargetsAndViews.Disable() && bRestored;
		return rState.clClearDepth.Disable() && bRestored;
	}

	void Release(bool bDrained)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		rState.pCandidate.Reset();
		rState.pLastSeen.store(nullptr, std::memory_order_release);
		rState.bReversedZ = false;
		rState.uWidth = 0;
		if (bDrained)
		{
			rState.clSetTargets.Release();
			rState.clSetTargetsAndViews.Release();
			rState.clClearDepth.Release();
			rState.bInstalled = false;
		}
	}
}
