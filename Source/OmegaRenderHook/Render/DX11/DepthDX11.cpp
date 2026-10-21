#include "OmegaRenderHook/Render/DX11/DepthDX11.hpp"
#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Render/DepthCommon.hpp"
#include "OmegaRenderHook/Render/PipelineInternal.hpp"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace OmegaRenderHook::DepthDX11
{
	namespace
	{
		template <typename T>
		using ComPtr = Microsoft::WRL::ComPtr<T>;

		namespace Slots
		{
			constexpr std::size_t uDrawIndexed = 12;
			constexpr std::size_t uDraw = 13;
			constexpr std::size_t uDrawIndexedInstanced = 20;
			constexpr std::size_t uDrawInstanced = 21;
			constexpr std::size_t uOMSetRenderTargets = 33;
			constexpr std::size_t uOMSetRenderTargetsAndUnorderedAccessViews = 34;
			constexpr std::size_t uDrawIndexedInstancedIndirect = 39;
			constexpr std::size_t uDrawInstancedIndirect = 40;
			constexpr std::size_t uRSSetViewports = 44;
			constexpr std::size_t uClearDepthStencilView = 53;
		}

		using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
		using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
		using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
		using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		using OMSetRenderTargetsAndUnorderedAccessViewsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
		using DrawIndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
		using RSSetViewportsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
		using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);

		struct stHooks
		{
			clMethodHook clDrawIndexed;
			clMethodHook clDraw;
			clMethodHook clDrawIndexedInstanced;
			clMethodHook clDrawInstanced;
			clMethodHook clSetTargets;
			clMethodHook clSetTargetsAndViews;
			clMethodHook clDrawIndexedInstancedIndirect;
			clMethodHook clDrawInstancedIndirect;
			clMethodHook clSetViewports;
			clMethodHook clClearDepth;

			template <typename F>
			void ForEach(F&& fnVisit)
			{
				fnVisit(clDrawIndexed);
				fnVisit(clDraw);
				fnVisit(clDrawIndexedInstanced);
				fnVisit(clDrawInstanced);
				fnVisit(clSetTargets);
				fnVisit(clSetTargetsAndViews);
				fnVisit(clDrawIndexedInstancedIndirect);
				fnVisit(clDrawInstancedIndirect);
				fnVisit(clSetViewports);
				fnVisit(clClearDepth);
			}
		};

		struct stState
		{
			std::mutex mtxDepth;
			bool bInstalled = false;
			stHooks stHook;
			ID3D11DeviceContext* pImmediate = nullptr;
			ComPtr<ID3D11Device> pDevice;
			UINT uTargetWidth = 0;
			UINT uTargetHeight = 0;

			ComPtr<ID3D11Texture2D> pBound;
			std::uint32_t* puBoundDraws = nullptr;
			std::unordered_map<ID3D11Texture2D*, std::uint32_t> mapDraws;
			ComPtr<ID3D11Texture2D> pBest;
			std::uint32_t uBestDraws = 0;

			ComPtr<ID3D11Texture2D> pChosen;
			D3D11_TEXTURE2D_DESC stChosen{};
			std::uint32_t uSegmentDraws = 0;
			DepthCommon::stRect stSegmentRect;
			bool bCaptured = false;
			std::uint32_t uCapturedDraws = 0;
			DepthCommon::stRect stCapturedRect;
			bool bReversedZ = false;

			ComPtr<ID3D11Texture2D> pSnapshot;
			ComPtr<ID3D11ShaderResourceView> pSnapshotView;
			ComPtr<ID3D11Texture2D> pResolved;
			ComPtr<ID3D11DepthStencilView> pResolvedView;
			UINT uResolvedWidth = 0;
			UINT uResolvedHeight = 0;
			ComPtr<ID3D11VertexShader> pVertexShader;
			ComPtr<ID3D11PixelShader> pPixelShader;
			ComPtr<ID3D11Buffer> pConstants;
			ComPtr<ID3D11DepthStencilState> pDepthState;
			ComPtr<ID3D11RasterizerState> pRasterizer;
			ComPtr<ID3DDeviceContextState> pContextState;
			bool bPipelineFailed = false;
			DepthCommon::clSwitchVote clVote;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		[[nodiscard]] bool IsOurs(const stState& rState, ID3D11DeviceContext* pContext)
		{
			return pContext != rState.pImmediate || PipelineInternal::clOwnWork::IsActive();
		}

		[[nodiscard]] ComPtr<ID3D11Texture2D> TextureOf(ID3D11DepthStencilView* pView)
		{
			if (!pView)
				return nullptr;
			ComPtr<ID3D11Resource> pResource;
			pView->GetResource(&pResource);
			ComPtr<ID3D11Texture2D> pTexture;
			if (pResource)
				pResource.As(&pTexture);
			return pTexture;
		}

		void Capture(stState& rState)
		{
			if (!rState.pChosen || !rState.pSnapshot || !rState.pImmediate)
				return;
			rState.pImmediate->CopySubresourceRegion(rState.pSnapshot.Get(), 0, 0, 0, 0, rState.pChosen.Get(), 0, nullptr);
			rState.bCaptured = true;
			rState.uCapturedDraws = rState.uSegmentDraws;
			rState.stCapturedRect = rState.stSegmentRect;
		}

		void NoteBound(ID3D11DepthStencilView* pView)
		{
			stState& rState = State();
			ComPtr<ID3D11Texture2D> pTexture = TextureOf(pView);
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (pTexture.Get() == rState.pBound.Get())
				return;
			rState.pBound.Reset();
			rState.puBoundDraws = nullptr;
			if (!pTexture)
				return;
			D3D11_TEXTURE2D_DESC stDesc{};
			pTexture->GetDesc(&stDesc);
			if (stDesc.ArraySize != 1 || !DepthCommon::Qualifies(stDesc.Width, stDesc.Height, stDesc.SampleDesc.Count, rState.uTargetWidth, rState.uTargetHeight) ||
				DepthCommon::FormatsOf(stDesc.Format).eTypeless == DXGI_FORMAT_UNKNOWN)
				return;
			rState.puBoundDraws = &rState.mapDraws[pTexture.Get()];
			rState.pBound = std::move(pTexture);
		}

		void NoteDraw()
		{
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (!rState.puBoundDraws)
				return;
			const std::uint32_t uDraws = ++*rState.puBoundDraws;
			if (rState.pBound.Get() == rState.pChosen.Get())
				++rState.uSegmentDraws;
			if (uDraws > rState.uBestDraws)
			{
				rState.uBestDraws = uDraws;
				if (rState.pBest.Get() != rState.pBound.Get())
					rState.pBest = rState.pBound;
			}
		}

		void NoteViewports(UINT uCount, const D3D11_VIEWPORT* pViewports)
		{
			if (!uCount || !pViewports)
				return;
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (!rState.pChosen || rState.pBound.Get() != rState.pChosen.Get())
				return;
			rState.stSegmentRect.Include(pViewports[0].TopLeftX, pViewports[0].TopLeftY, pViewports[0].Width, pViewports[0].Height);
		}

		void NoteClear(ID3D11DepthStencilView* pView, UINT uFlags, FLOAT flDepth)
		{
			if (!pView || !(uFlags & D3D11_CLEAR_DEPTH))
				return;
			stState& rState = State();
			ComPtr<ID3D11Texture2D> pTexture = TextureOf(pView);
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (!pTexture || pTexture.Get() != rState.pChosen.Get())
				return;
			if (rState.uSegmentDraws > rState.uCapturedDraws)
				Capture(rState);
			rState.uSegmentDraws = 0;
			rState.stSegmentRect = {};
			rState.bReversedZ = flDepth < .5f;
		}

		void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* pContext, UINT uCount, UINT uStart, INT iBase)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteDraw();
			State().stHook.clDrawIndexed.Original<DrawIndexedFn>()(pContext, uCount, uStart, iBase);
		}

		void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* pContext, UINT uCount, UINT uStart)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteDraw();
			State().stHook.clDraw.Original<DrawFn>()(pContext, uCount, uStart);
		}

		void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D11DeviceContext* pContext, UINT uCount, UINT uInstances, UINT uStart, INT iBase, UINT uStartInstance)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteDraw();
			State().stHook.clDrawIndexedInstanced.Original<DrawIndexedInstancedFn>()(pContext, uCount, uInstances, uStart, iBase, uStartInstance);
		}

		void STDMETHODCALLTYPE HookDrawInstanced(ID3D11DeviceContext* pContext, UINT uCount, UINT uInstances, UINT uStart, UINT uStartInstance)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteDraw();
			State().stHook.clDrawInstanced.Original<DrawInstancedFn>()(pContext, uCount, uInstances, uStart, uStartInstance);
		}

		void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirect(ID3D11DeviceContext* pContext, ID3D11Buffer* pArguments, UINT uOffset)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteDraw();
			State().stHook.clDrawIndexedInstancedIndirect.Original<DrawIndirectFn>()(pContext, pArguments, uOffset);
		}

		void STDMETHODCALLTYPE HookDrawInstancedIndirect(ID3D11DeviceContext* pContext, ID3D11Buffer* pArguments, UINT uOffset)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteDraw();
			State().stHook.clDrawInstancedIndirect.Original<DrawIndirectFn>()(pContext, pArguments, uOffset);
		}

		void STDMETHODCALLTYPE HookSetTargets(ID3D11DeviceContext* pContext, UINT uViews, ID3D11RenderTargetView* const* ppViews, ID3D11DepthStencilView* pDepth)
		{
			clHookScope clScope;
			State().stHook.clSetTargets.Original<OMSetRenderTargetsFn>()(pContext, uViews, ppViews, pDepth);
			if (!IsOurs(State(), pContext))
				NoteBound(pDepth);
		}

		void STDMETHODCALLTYPE HookSetTargetsAndViews(ID3D11DeviceContext* pContext, UINT uViews, ID3D11RenderTargetView* const* ppViews, ID3D11DepthStencilView* pDepth, UINT uStart, UINT uUnordered, ID3D11UnorderedAccessView* const* ppUnordered, const UINT* pCounts)
		{
			clHookScope clScope;
			State().stHook.clSetTargetsAndViews.Original<OMSetRenderTargetsAndUnorderedAccessViewsFn>()(pContext, uViews, ppViews, pDepth, uStart, uUnordered, ppUnordered, pCounts);
			if (uViews != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && !IsOurs(State(), pContext))
				NoteBound(pDepth);
		}

		void STDMETHODCALLTYPE HookSetViewports(ID3D11DeviceContext* pContext, UINT uCount, const D3D11_VIEWPORT* pViewports)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteViewports(uCount, pViewports);
			State().stHook.clSetViewports.Original<RSSetViewportsFn>()(pContext, uCount, pViewports);
		}

		void STDMETHODCALLTYPE HookClearDepth(ID3D11DeviceContext* pContext, ID3D11DepthStencilView* pDepth, UINT uFlags, FLOAT flDepth, UINT8 uStencil)
		{
			clHookScope clScope;
			if (!IsOurs(State(), pContext))
				NoteClear(pDepth, uFlags, flDepth);
			State().stHook.clClearDepth.Original<ClearDepthStencilViewFn>()(pContext, pDepth, uFlags, flDepth, uStencil);
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
			if (rState.pContextState)
				return true;
			if (rState.bPipelineFailed || !rState.pDevice)
				return false;
			rState.bPipelineFailed = true;
			ComPtr<ID3D11Device1> pDevice1;
			if (FAILED(rState.pDevice.As(&pDevice1)))
			{
				Log(ELogLevel::Warning, "Direct3D 11.1 is not available, world drawing will not be occluded");
				return false;
			}
			const D3D_FEATURE_LEVEL eLevel = rState.pDevice->GetFeatureLevel();
			ComPtr<ID3DDeviceContextState> pContextState;
			if (FAILED(pDevice1->CreateDeviceContextState(0, &eLevel, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &pContextState)))
				return false;
			const ComPtr<ID3DBlob> pVertex = Compile("VSMain", "vs_4_0");
			const ComPtr<ID3DBlob> pPixel = Compile("PSMain", "ps_4_0");
			if (!pVertex || !pPixel)
				return false;
			if (FAILED(rState.pDevice->CreateVertexShader(pVertex->GetBufferPointer(), pVertex->GetBufferSize(), nullptr, &rState.pVertexShader)) ||
				FAILED(rState.pDevice->CreatePixelShader(pPixel->GetBufferPointer(), pPixel->GetBufferSize(), nullptr, &rState.pPixelShader)))
				return false;
			D3D11_BUFFER_DESC stBuffer{};
			stBuffer.ByteWidth = sizeof(DepthCommon::stResolveConstants);
			stBuffer.Usage = D3D11_USAGE_DYNAMIC;
			stBuffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			stBuffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(rState.pDevice->CreateBuffer(&stBuffer, nullptr, &rState.pConstants)))
				return false;
			D3D11_DEPTH_STENCIL_DESC stDepth{};
			stDepth.DepthEnable = TRUE;
			stDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			stDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
			if (FAILED(rState.pDevice->CreateDepthStencilState(&stDepth, &rState.pDepthState)))
				return false;
			D3D11_RASTERIZER_DESC stRasterizer{};
			stRasterizer.FillMode = D3D11_FILL_SOLID;
			stRasterizer.CullMode = D3D11_CULL_NONE;
			stRasterizer.DepthClipEnable = FALSE;
			if (FAILED(rState.pDevice->CreateRasterizerState(&stRasterizer, &rState.pRasterizer)))
				return false;
			rState.pContextState = std::move(pContextState);
			rState.bPipelineFailed = false;
			return true;
		}

		[[nodiscard]] bool EnsureResolved(stState& rState)
		{
			if (rState.pResolvedView && rState.uResolvedWidth == rState.uTargetWidth && rState.uResolvedHeight == rState.uTargetHeight)
				return true;
			rState.pResolvedView.Reset();
			rState.pResolved.Reset();
			D3D11_TEXTURE2D_DESC stDesc{};
			stDesc.Width = rState.uTargetWidth;
			stDesc.Height = rState.uTargetHeight;
			stDesc.MipLevels = 1;
			stDesc.ArraySize = 1;
			stDesc.Format = DXGI_FORMAT_D32_FLOAT;
			stDesc.SampleDesc.Count = 1;
			stDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			if (FAILED(rState.pDevice->CreateTexture2D(&stDesc, nullptr, &rState.pResolved)) || FAILED(rState.pDevice->CreateDepthStencilView(rState.pResolved.Get(), nullptr, &rState.pResolvedView)))
			{
				rState.pResolved.Reset();
				rState.pResolvedView.Reset();
				return false;
			}
			rState.uResolvedWidth = rState.uTargetWidth;
			rState.uResolvedHeight = rState.uTargetHeight;
			return true;
		}

		[[nodiscard]] bool CreateSnapshot(stState& rState)
		{
			rState.pSnapshotView.Reset();
			rState.pSnapshot.Reset();
			const DepthCommon::stFormats stFormats = DepthCommon::FormatsOf(rState.stChosen.Format);
			D3D11_TEXTURE2D_DESC stDesc{};
			stDesc.Width = rState.stChosen.Width;
			stDesc.Height = rState.stChosen.Height;
			stDesc.MipLevels = 1;
			stDesc.ArraySize = 1;
			stDesc.Format = stFormats.eTypeless;
			stDesc.SampleDesc.Count = 1;
			stDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SHADER_RESOURCE_VIEW_DESC stView{};
			stView.Format = stFormats.eShaderView;
			stView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			stView.Texture2D.MipLevels = 1;
			if (FAILED(rState.pDevice->CreateTexture2D(&stDesc, nullptr, &rState.pSnapshot)) || FAILED(rState.pDevice->CreateShaderResourceView(rState.pSnapshot.Get(), &stView, &rState.pSnapshotView)))
			{
				rState.pSnapshot.Reset();
				rState.pSnapshotView.Reset();
				return false;
			}
			return true;
		}

		[[nodiscard]] bool DrawResolve(stState& rState)
		{
			if (!EnsurePipeline(rState) || !EnsureResolved(rState) || !rState.pSnapshotView)
				return false;
			ComPtr<ID3D11DeviceContext1> pContext;
			if (FAILED(rState.pImmediate->QueryInterface(IID_PPV_ARGS(&pContext))))
				return false;
			D3D11_MAPPED_SUBRESOURCE stMapped{};
			if (FAILED(pContext->Map(rState.pConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &stMapped)))
				return false;
			const DepthCommon::stResolveConstants stConstants = DepthCommon::ResolveConstants(rState.stCapturedRect, rState.stChosen.Width, rState.stChosen.Height, rState.uTargetWidth, rState.uTargetHeight);
			std::memcpy(stMapped.pData, &stConstants, sizeof(stConstants));
			pContext->Unmap(rState.pConstants.Get(), 0);

			ComPtr<ID3DDeviceContextState> pPrevious;
			pContext->SwapDeviceContextState(rState.pContextState.Get(), &pPrevious);
			pContext->OMSetRenderTargets(0, nullptr, rState.pResolvedView.Get());
			const D3D11_VIEWPORT stViewport{ 0.f, 0.f, static_cast<float>(rState.uTargetWidth), static_cast<float>(rState.uTargetHeight), 0.f, 1.f };
			pContext->RSSetViewports(1, &stViewport);
			pContext->RSSetState(rState.pRasterizer.Get());
			pContext->OMSetDepthStencilState(rState.pDepthState.Get(), 0);
			pContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
			pContext->IASetInputLayout(nullptr);
			pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			pContext->VSSetShader(rState.pVertexShader.Get(), nullptr, 0);
			pContext->PSSetShader(rState.pPixelShader.Get(), nullptr, 0);
			ID3D11ShaderResourceView* const aViews[] = { rState.pSnapshotView.Get() };
			pContext->PSSetShaderResources(0, 1, aViews);
			ID3D11Buffer* const aBuffers[] = { rState.pConstants.Get() };
			pContext->PSSetConstantBuffers(0, 1, aBuffers);
			pContext->Draw(3, 0);
			ID3D11ShaderResourceView* const aNone[] = { nullptr };
			pContext->PSSetShaderResources(0, 1, aNone);
			pContext->OMSetRenderTargets(0, nullptr, nullptr);
			pContext->SwapDeviceContextState(pPrevious.Get(), nullptr);
			return true;
		}

		void SelectNext(stState& rState)
		{
			const std::uint32_t uChosenDraws = rState.pChosen ? rState.mapDraws[rState.pChosen.Get()] : 0;
			const bool bWins = rState.pBest && rState.pBest.Get() != rState.pChosen.Get() && DepthCommon::ShouldSwitch(rState.uBestDraws, uChosenDraws);
			if (rState.clVote.Vote(reinterpret_cast<std::uint64_t>(rState.pBest.Get()), bWins, rState.pChosen != nullptr))
			{
				rState.pChosen = rState.pBest;
				rState.pChosen->GetDesc(&rState.stChosen);
				rState.bReversedZ = false;
				if (!CreateSnapshot(rState))
				{
					Log(ELogLevel::Warning, "could not create the Direct3D 11 depth copy, world drawing will not be occluded");
					rState.pChosen.Reset();
				}
				else
					Log(ELogLevel::Debug, "Direct3D 11 scene depth is {} ({}x{}, format {})", static_cast<void*>(rState.pChosen.Get()), rState.stChosen.Width, rState.stChosen.Height, static_cast<int>(rState.stChosen.Format));
			}
			rState.mapDraws.clear();
			rState.puBoundDraws = rState.pBound ? &rState.mapDraws[rState.pBound.Get()] : nullptr;
			rState.pBest.Reset();
			rState.uBestDraws = 0;
			rState.uSegmentDraws = 0;
			rState.stSegmentRect = {};
			rState.bCaptured = false;
			rState.uCapturedDraws = 0;
			rState.stCapturedRect = {};
		}

		void ClearTracking(stState& rState)
		{
			rState.clVote.Reset();
			rState.pBound.Reset();
			rState.puBoundDraws = nullptr;
			rState.mapDraws.clear();
			rState.pBest.Reset();
			rState.uBestDraws = 0;
			rState.pChosen.Reset();
			rState.stChosen = {};
			rState.uSegmentDraws = 0;
			rState.stSegmentRect = {};
			rState.bCaptured = false;
			rState.uCapturedDraws = 0;
			rState.stCapturedRect = {};
			rState.bReversedZ = false;
			rState.pSnapshotView.Reset();
			rState.pSnapshot.Reset();
		}

		void ReleaseResources(stState& rState)
		{
			ClearTracking(rState);
			rState.pResolvedView.Reset();
			rState.pResolved.Reset();
			rState.uResolvedWidth = 0;
			rState.uResolvedHeight = 0;
			rState.pVertexShader.Reset();
			rState.pPixelShader.Reset();
			rState.pConstants.Reset();
			rState.pDepthState.Reset();
			rState.pRasterizer.Reset();
			rState.pContextState.Reset();
			rState.bPipelineFailed = false;
			rState.pDevice.Reset();
			rState.pImmediate = nullptr;
		}
	}

	bool Install(ID3D11DeviceContext* pContext)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (!pContext)
			return false;
		if (rState.pImmediate != pContext)
		{
			ReleaseResources(rState);
			rState.pImmediate = pContext;
			pContext->GetDevice(&rState.pDevice);
		}
		if (rState.bInstalled)
			return true;
		constexpr EHookMethod eMethod = EHookMethod::Inline;
		void** ppVTable = *reinterpret_cast<void***>(pContext);
		stHooks& rHook = rState.stHook;
		const bool bHooked =
			rHook.clDrawIndexed.Install(eMethod, ppVTable, Slots::uDrawIndexed, reinterpret_cast<void*>(&HookDrawIndexed)) &&
			rHook.clDraw.Install(eMethod, ppVTable, Slots::uDraw, reinterpret_cast<void*>(&HookDraw)) &&
			rHook.clDrawIndexedInstanced.Install(eMethod, ppVTable, Slots::uDrawIndexedInstanced, reinterpret_cast<void*>(&HookDrawIndexedInstanced)) &&
			rHook.clDrawInstanced.Install(eMethod, ppVTable, Slots::uDrawInstanced, reinterpret_cast<void*>(&HookDrawInstanced)) &&
			rHook.clSetTargets.Install(eMethod, ppVTable, Slots::uOMSetRenderTargets, reinterpret_cast<void*>(&HookSetTargets)) &&
			rHook.clSetTargetsAndViews.Install(eMethod, ppVTable, Slots::uOMSetRenderTargetsAndUnorderedAccessViews, reinterpret_cast<void*>(&HookSetTargetsAndViews)) &&
			rHook.clDrawIndexedInstancedIndirect.Install(eMethod, ppVTable, Slots::uDrawIndexedInstancedIndirect, reinterpret_cast<void*>(&HookDrawIndexedInstancedIndirect)) &&
			rHook.clDrawInstancedIndirect.Install(eMethod, ppVTable, Slots::uDrawInstancedIndirect, reinterpret_cast<void*>(&HookDrawInstancedIndirect)) &&
			rHook.clSetViewports.Install(eMethod, ppVTable, Slots::uRSSetViewports, reinterpret_cast<void*>(&HookSetViewports)) &&
			rHook.clClearDepth.Install(eMethod, ppVTable, Slots::uClearDepthStencilView, reinterpret_cast<void*>(&HookClearDepth));
		if (!bHooked)
		{
			rHook.ForEach([](clMethodHook& rMethod) { rMethod.Disable(); rMethod.Release(); });
			Log(ELogLevel::Warning, "could not hook the Direct3D 11 context for depth, world drawing will not be occluded");
			return false;
		}
		rState.bInstalled = true;
		Log(ELogLevel::Debug, "Direct3D 11 depth tracking hooked");
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

	stDepth Resolve()
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (!rState.bInstalled || !rState.pImmediate)
			return {};
		if (rState.pChosen && rState.uSegmentDraws > rState.uCapturedDraws)
			Capture(rState);
		stDepth stResult;
		if (rState.bCaptured && DrawResolve(rState))
			stResult = { rState.pResolvedView.Get(), rState.bReversedZ };
		SelectNext(rState);
		return stResult;
	}

	void Reset()
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		ReleaseResources(rState);
	}

	bool Disable()
	{
		stState& rState = State();
		bool bRestored = true;
		rState.stHook.ForEach([&](clMethodHook& rHook) { bRestored = rHook.Disable() && bRestored; });
		return bRestored;
	}

	void Release(bool bDrained)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		ReleaseResources(rState);
		rState.uTargetWidth = 0;
		rState.uTargetHeight = 0;
		if (bDrained)
		{
			rState.stHook.ForEach([](clMethodHook& rHook) { rHook.Release(); });
			rState.bInstalled = false;
		}
	}
}
