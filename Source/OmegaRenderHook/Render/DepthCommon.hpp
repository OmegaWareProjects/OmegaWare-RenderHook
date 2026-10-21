#pragma once

#include <Windows.h>
#include <dxgiformat.h>

#include <algorithm>
#include <cstdint>

namespace OmegaRenderHook::DepthCommon
{
	inline constexpr char szResolveHLSL[] = R"(
cbuffer ResolveBuffer : register(b0)
{
	float4 SourceRect;
};
Texture2D<float> SourceDepth : register(t0);
float4 VSMain(uint uVertex : SV_VertexID) : SV_Position
{
	float2 uv = float2((uVertex << 1) & 2, uVertex & 2);
	return float4(uv * float2(2.f, -2.f) + float2(-1.f, 1.f), 0.f, 1.f);
}
float PSMain(float4 pos : SV_Position) : SV_Depth
{
	return SourceDepth.Load(int3(SourceRect.xy + pos.xy * SourceRect.zw, 0));
}
)";

	struct stRect
	{
		float flLeft = 0.f;
		float flTop = 0.f;
		float flRight = 0.f;
		float flBottom = 0.f;

		[[nodiscard]] bool IsEmpty() const { return flRight <= flLeft || flBottom <= flTop; }

		void Include(float flX, float flY, float flWidth, float flHeight)
		{
			if (flWidth <= 0.f || flHeight <= 0.f)
				return;
			if (IsEmpty())
			{
				*this = { flX, flY, flX + flWidth, flY + flHeight };
				return;
			}
			flLeft = std::min(flLeft, flX);
			flTop = std::min(flTop, flY);
			flRight = std::max(flRight, flX + flWidth);
			flBottom = std::max(flBottom, flY + flHeight);
		}
	};

	struct stResolveConstants
	{
		float aSourceRect[4]{};
	};

	[[nodiscard]] inline stResolveConstants ResolveConstants(const stRect& stSource, UINT uSourceWidth, UINT uSourceHeight, UINT uTargetWidth, UINT uTargetHeight)
	{
		stRect stClamped = stSource;
		if (stClamped.IsEmpty())
			stClamped = { 0.f, 0.f, static_cast<float>(uSourceWidth), static_cast<float>(uSourceHeight) };
		stClamped.flLeft = std::clamp(stClamped.flLeft, 0.f, static_cast<float>(uSourceWidth));
		stClamped.flTop = std::clamp(stClamped.flTop, 0.f, static_cast<float>(uSourceHeight));
		stClamped.flRight = std::clamp(stClamped.flRight, stClamped.flLeft, static_cast<float>(uSourceWidth));
		stClamped.flBottom = std::clamp(stClamped.flBottom, stClamped.flTop, static_cast<float>(uSourceHeight));
		stResolveConstants stConstants;
		stConstants.aSourceRect[0] = stClamped.flLeft;
		stConstants.aSourceRect[1] = stClamped.flTop;
		stConstants.aSourceRect[2] = uTargetWidth ? (stClamped.flRight - stClamped.flLeft) / static_cast<float>(uTargetWidth) : 1.f;
		stConstants.aSourceRect[3] = uTargetHeight ? (stClamped.flBottom - stClamped.flTop) / static_cast<float>(uTargetHeight) : 1.f;
		return stConstants;
	}

	[[nodiscard]] inline bool Qualifies(UINT uWidth, UINT uHeight, UINT uSamples, UINT uTargetWidth, UINT uTargetHeight)
	{
		if (uSamples != 1 || !uWidth || !uHeight || !uTargetWidth || !uTargetHeight)
			return false;
		if (uWidth == uTargetWidth && uHeight == uTargetHeight)
			return true;
		if (uWidth * 4 < uTargetWidth || uHeight * 4 < uTargetHeight || uWidth > uTargetWidth * 2 || uHeight > uTargetHeight * 2)
			return false;
		const double dAspect = static_cast<double>(uWidth) / uHeight;
		const double dTarget = static_cast<double>(uTargetWidth) / uTargetHeight;
		return dAspect > dTarget * 0.98 && dAspect < dTarget * 1.02;
	}

	struct stFormats
	{
		DXGI_FORMAT eTypeless = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT eShaderView = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT eDepthView = DXGI_FORMAT_UNKNOWN;
	};

	[[nodiscard]] inline stFormats FormatsOf(DXGI_FORMAT eFormat)
	{
		switch (eFormat)
		{
		case DXGI_FORMAT_D32_FLOAT:
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT:
			return { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT };
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
			return { DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT };
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
			return { DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT };
		case DXGI_FORMAT_D16_UNORM:
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_R16_UNORM:
			return { DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM };
		default:
			return {};
		}
	}

	[[nodiscard]] inline bool ShouldSwitch(std::uint32_t uBestDraws, std::uint32_t uChosenDraws)
	{
		return uBestDraws > 0 && (uChosenDraws == 0 || uBestDraws > uChosenDraws + uChosenDraws / 2);
	}

	class clSwitchVote
	{
	public:
		static constexpr std::uint32_t kFrames = 30;

		[[nodiscard]] bool Vote(std::uint64_t uCandidate, bool bWins, bool bHasChosen)
		{
			if (!uCandidate || !bWins)
			{
				Reset();
				return false;
			}
			if (!bHasChosen)
			{
				Reset();
				return true;
			}
			if (uCandidate != m_uCandidate)
			{
				m_uCandidate = uCandidate;
				m_uStreak = 0;
			}
			if (++m_uStreak < kFrames)
				return false;
			Reset();
			return true;
		}

		void Reset()
		{
			m_uCandidate = 0;
			m_uStreak = 0;
		}

	private:
		std::uint64_t m_uCandidate = 0;
		std::uint32_t m_uStreak = 0;
	};
}
