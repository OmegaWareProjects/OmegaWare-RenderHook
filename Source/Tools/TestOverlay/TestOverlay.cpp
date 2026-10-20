#include "OmegaRenderHook/OmegaRenderHook.hpp"

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace
{
	template <typename T>
	using ComPtr = Microsoft::WRL::ComPtr<T>;

	HMODULE g_hModule = nullptr;
	std::mutex g_mtxLog;
	std::ofstream g_fileLog;

	[[nodiscard]] std::filesystem::path ModuleDirectory()
	{
		wchar_t szPath[MAX_PATH]{};
		GetModuleFileNameW(g_hModule, szPath, MAX_PATH);
		return std::filesystem::path(szPath).parent_path();
	}

	void WriteLog(OmegaRenderHook::ELogLevel eLevel, std::string_view strMessage)
	{
		std::scoped_lock lckLog(g_mtxLog);
		const auto tpNow = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
		const std::string strLine = std::format("[{:%T}] [{}] [tid {}] {}\n", tpNow, OmegaRenderHook::ToString(eLevel), GetCurrentThreadId(), strMessage);
		if (g_fileLog)
		{
			g_fileLog << strLine;
			g_fileLog.flush();
		}
		OutputDebugStringA(strLine.c_str());
	}

	[[nodiscard]] std::array<float, 4> Hue(std::uint64_t uFrame)
	{
		const float flPhase = static_cast<float>(uFrame % 360) / 60.f;
		const float flRamp = flPhase - static_cast<float>(static_cast<int>(flPhase));
		switch (static_cast<int>(flPhase))
		{
		case 0: return { 1.f, flRamp, 0.f, 1.f };
		case 1: return { 1.f - flRamp, 1.f, 0.f, 1.f };
		case 2: return { 0.f, 1.f, flRamp, 1.f };
		case 3: return { 0.f, 1.f - flRamp, 1.f, 1.f };
		case 4: return { flRamp, 0.f, 1.f, 1.f };
		default: return { 1.f, 0.f, 1.f - flRamp, 1.f };
		}
	}

	struct stShapes
	{
		D3D12_RECT stFrame;
		D3D12_RECT stFill;
		D3D12_RECT stMarker;
	};

	[[nodiscard]] stShapes Shapes(std::uint64_t uFrame)
	{
		constexpr LONG iLeft = 24;
		constexpr LONG iTop = 24;
		constexpr LONG iWidth = 240;
		constexpr LONG iHeight = 20;
		const LONG iMarker = iLeft + 2 + static_cast<LONG>(uFrame % (iWidth - 12));
		return {
			{ iLeft, iTop, iLeft + iWidth, iTop + iHeight },
			{ iLeft + 2, iTop + 2, iLeft + iWidth - 2, iTop + iHeight - 2 },
			{ iMarker, iTop + 2, iMarker + 8, iTop + iHeight - 2 },
		};
	}

	class clTestOverlay final : public OmegaRenderHook::clRenderListener
	{
	public:
		void OnAttach(const OmegaRenderHook::stDeviceDX12& rDevice) override
		{
			OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Info, "overlay attached to Direct3D 12: window {} {}x{} format {} buffers {} frames in flight {} queue {} ({})",
				static_cast<void*>(rDevice.hWindow), rDevice.uWidth, rDevice.uHeight, static_cast<int>(rDevice.eFormat), rDevice.uBufferCount, rDevice.uFramesInFlight,
				static_cast<void*>(rDevice.pQueue), OmegaRenderHook::ToString(rDevice.eQueueSource));
		}

		void OnFrame(const OmegaRenderHook::stFrameDX12& rFrame) override
		{
			const stShapes stShape = Shapes(rFrame.uFrameNumber);
			constexpr float aBorder[4] = { 0.05f, 0.05f, 0.07f, 1.f };
			const std::array<float, 4> aFill = Hue(rFrame.uFrameNumber);
			constexpr float aMarker[4] = { 1.f, 1.f, 1.f, 1.f };
			rFrame.pCommandList->ClearRenderTargetView(rFrame.hRenderTarget, aBorder, 1, &stShape.stFrame);
			rFrame.pCommandList->ClearRenderTargetView(rFrame.hRenderTarget, aFill.data(), 1, &stShape.stFill);
			rFrame.pCommandList->ClearRenderTargetView(rFrame.hRenderTarget, aMarker, 1, &stShape.stMarker);
		}

		void OnAttach(const OmegaRenderHook::stDeviceDX11& rDevice) override
		{
			OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Info, "overlay attached to Direct3D 11: window {} {}x{} format {} buffers {}",
				static_cast<void*>(rDevice.hWindow), rDevice.uWidth, rDevice.uHeight, static_cast<int>(rDevice.eFormat), rDevice.uBufferCount);
			m_pContext1.Reset();
			rDevice.pContext->QueryInterface(IID_PPV_ARGS(&m_pContext1));
		}

		void OnFrame(const OmegaRenderHook::stFrameDX11& rFrame) override
		{
			if (!m_pContext1)
				return;
			const stShapes stShape = Shapes(rFrame.uFrameNumber);
			constexpr float aBorder[4] = { 0.05f, 0.05f, 0.07f, 1.f };
			const std::array<float, 4> aFill = Hue(rFrame.uFrameNumber);
			constexpr float aMarker[4] = { 1.f, 1.f, 1.f, 1.f };
			m_pContext1->ClearView(rFrame.pRenderTarget, aBorder, &stShape.stFrame, 1);
			m_pContext1->ClearView(rFrame.pRenderTarget, aFill.data(), &stShape.stFill, 1);
			m_pContext1->ClearView(rFrame.pRenderTarget, aMarker, &stShape.stMarker, 1);
		}

		void OnDetach(OmegaRenderHook::ERenderApi eApi, OmegaRenderHook::EDetachReason eReason) override
		{
			OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Info, "overlay detached from {} ({})", OmegaRenderHook::ToString(eApi), OmegaRenderHook::ToString(eReason));
			m_pContext1.Reset();
		}

	private:
		ComPtr<ID3D11DeviceContext1> m_pContext1;
	};

	clTestOverlay g_clOverlay;

	[[nodiscard]] OmegaRenderHook::EHookMethod ReadHookMethod(const std::filesystem::path& pathConfig)
	{
		wchar_t szMethod[32]{};
		GetPrivateProfileStringW(L"Overlay", L"HookMethod", L"vtable", szMethod, 32, pathConfig.c_str());
		return std::wstring_view(szMethod) == L"inline" ? OmegaRenderHook::EHookMethod::Inline : OmegaRenderHook::EHookMethod::VTable;
	}

	void LogStatus()
	{
		const OmegaRenderHook::stPipelineStatus stStatus = OmegaRenderHook::Pipeline::GetStatus();
		OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Info, "status: api {} queue {} {}x{} format {} buffers {} rendered {} skipped {} swap chains {} window hooked {} faulted {} device lost {}",
			OmegaRenderHook::ToString(stStatus.eApi), OmegaRenderHook::ToString(stStatus.eQueueSource), stStatus.uWidth, stStatus.uHeight, static_cast<int>(stStatus.eFormat),
			stStatus.uBufferCount, stStatus.uFramesRendered, stStatus.uFramesSkipped, stStatus.uSwapChainsSeen, stStatus.bWindowHooked, stStatus.bListenerFaulted, stStatus.bDeviceLost);
	}

	DWORD WINAPI MainThread(LPVOID)
	{
		const std::filesystem::path pathDirectory = ModuleDirectory();
		g_fileLog.open(pathDirectory / "OmegaRenderHook-TestOverlay.log", std::ios::out | std::ios::trunc);
		OmegaRenderHook::SetLogSink(&WriteLog);

		OmegaRenderHook::stPipelineConfig stConfig;
		stConfig.pListener = &g_clOverlay;
		stConfig.eHookMethod = ReadHookMethod(pathDirectory / "OmegaRenderHook-TestOverlay.ini");
		OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Info, "test overlay loaded into process {}, installing with {} hooks, press END to unload", GetCurrentProcessId(), OmegaRenderHook::ToString(stConfig.eHookMethod));

		if (auto expInstalled = OmegaRenderHook::Pipeline::Install(stConfig); !expInstalled)
		{
			OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Error, "install failed: {}", expInstalled.error());
			OmegaRenderHook::SetLogSink(nullptr);
			FreeLibraryAndExitThread(g_hModule, 0);
		}

		auto tpNextStatus = std::chrono::steady_clock::now();
		while (!(GetAsyncKeyState(VK_END) & 0x8000))
		{
			if (std::chrono::steady_clock::now() >= tpNextStatus)
			{
				LogStatus();
				tpNextStatus = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		LogStatus();
		const bool bSafe = OmegaRenderHook::Pipeline::Uninstall();
		OmegaRenderHook::Log(OmegaRenderHook::ELogLevel::Info, "{}", bSafe ? "unloading" : "hooks could not be fully removed, staying loaded");
		OmegaRenderHook::SetLogSink(nullptr);
		g_fileLog.close();
		if (bSafe)
			FreeLibraryAndExitThread(g_hModule, 0);
		return 0;
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD uReason, LPVOID)
{
	if (uReason == DLL_PROCESS_ATTACH)
	{
		g_hModule = hModule;
		DisableThreadLibraryCalls(hModule);
		if (HANDLE hThread = CreateThread(nullptr, 0, &MainThread, nullptr, 0, nullptr))
			CloseHandle(hThread);
	}
	return TRUE;
}
