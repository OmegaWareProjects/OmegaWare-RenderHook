#pragma once

#include "OmegaRenderHook/Hook/MethodHook.hpp"

#include <Windows.h>
#include <d3d9.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#if OMEGARENDERHOOK_VULKAN
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#endif

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace OmegaRenderHook
{
	enum class ERenderApi
	{
		None,
		DX9,
		DX10,
		DX11,
		DX12,
		OpenGL,
		Vulkan
	};

	enum class EQueueSource
	{
		None,
		CreationHook,
		SwapChainDevice,
		ObjectScan
	};

	enum class EDetachReason
	{
		SwapChainChanged,
		DeviceLost,
		Uninstall
	};

	[[nodiscard]] std::string_view ToString(ERenderApi eApi);
	[[nodiscard]] std::string_view ToString(EQueueSource eSource);
	[[nodiscard]] std::string_view ToString(EDetachReason eReason);

	struct stDeviceDX12
	{
		ID3D12Device* pDevice = nullptr;
		ID3D12CommandQueue* pQueue = nullptr;
		IDXGISwapChain3* pSwapChain = nullptr;
		HWND hWindow = nullptr;
		DXGI_FORMAT eFormat = DXGI_FORMAT_UNKNOWN;
		UINT uWidth = 0;
		UINT uHeight = 0;
		UINT uBufferCount = 0;
		UINT uFramesInFlight = 0;
		EQueueSource eQueueSource = EQueueSource::None;
	};

	struct stFrameDX12
	{
		const stDeviceDX12* pDevice = nullptr;
		ID3D12GraphicsCommandList* pCommandList = nullptr;
		ID3D12Resource* pBackBuffer = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE hRenderTarget{};
		D3D12_CPU_DESCRIPTOR_HANDLE hDepthStencil{};
		DXGI_FORMAT eDepthFormat = DXGI_FORMAT_UNKNOWN;
		bool bReversedZ = false;
		UINT uBackBufferIndex = 0;
		UINT uFrameSlot = 0;
		std::uint64_t uFrameNumber = 0;
	};

	struct stDeviceDX11
	{
		ID3D11Device* pDevice = nullptr;
		ID3D11DeviceContext* pContext = nullptr;
		IDXGISwapChain* pSwapChain = nullptr;
		HWND hWindow = nullptr;
		DXGI_FORMAT eFormat = DXGI_FORMAT_UNKNOWN;
		UINT uWidth = 0;
		UINT uHeight = 0;
		UINT uBufferCount = 0;
	};

	struct stFrameDX11
	{
		const stDeviceDX11* pDevice = nullptr;
		ID3D11Texture2D* pBackBuffer = nullptr;
		ID3D11RenderTargetView* pRenderTarget = nullptr;
		ID3D11DepthStencilView* pDepthStencil = nullptr;
		bool bReversedZ = false;
		std::uint64_t uFrameNumber = 0;
	};

	struct stDeviceDX10
	{
		ID3D10Device* pDevice = nullptr;
		IDXGISwapChain* pSwapChain = nullptr;
		HWND hWindow = nullptr;
		DXGI_FORMAT eFormat = DXGI_FORMAT_UNKNOWN;
		UINT uWidth = 0;
		UINT uHeight = 0;
		UINT uBufferCount = 0;
	};

	struct stFrameDX10
	{
		const stDeviceDX10* pDevice = nullptr;
		ID3D10Texture2D* pBackBuffer = nullptr;
		ID3D10RenderTargetView* pRenderTarget = nullptr;
		std::uint64_t uFrameNumber = 0;
	};

	struct stDeviceDX9
	{
		IDirect3DDevice9* pDevice = nullptr;
		HWND hWindow = nullptr;
		D3DFORMAT eFormat = D3DFMT_UNKNOWN;
		UINT uWidth = 0;
		UINT uHeight = 0;
	};

	struct stFrameDX9
	{
		const stDeviceDX9* pDevice = nullptr;
		std::uint64_t uFrameNumber = 0;
		IDirect3DSurface9* pDepthStencil = nullptr;
	};

	struct stDeviceOpenGL
	{
		HWND hWindow = nullptr;
		HDC hDC = nullptr;
		HGLRC hContext = nullptr;
		UINT uWidth = 0;
		UINT uHeight = 0;
	};

	struct stFrameOpenGL
	{
		const stDeviceOpenGL* pDevice = nullptr;
		std::uint64_t uFrameNumber = 0;
	};

#if OMEGARENDERHOOK_VULKAN
	struct stDeviceVulkan
	{
		PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr = nullptr;
		VkInstance hInstance = VK_NULL_HANDLE;
		VkPhysicalDevice hPhysicalDevice = VK_NULL_HANDLE;
		VkDevice hDevice = VK_NULL_HANDLE;
		std::uint32_t uQueueFamily = 0;
		VkQueue hQueue = VK_NULL_HANDLE;
		VkRenderPass hRenderPass = VK_NULL_HANDLE;
		VkFormat eFormat = VK_FORMAT_UNDEFINED;
		std::uint32_t uImageCount = 0;
		HWND hWindow = nullptr;
		UINT uWidth = 0;
		UINT uHeight = 0;
	};

	struct stFrameVulkan
	{
		const stDeviceVulkan* pDevice = nullptr;
		VkCommandBuffer hCommandBuffer = VK_NULL_HANDLE;
		VkExtent2D stExtent{};
		bool bHasDepth = false;
		bool bReversedZ = false;
		std::uint32_t uImageIndex = 0;
		std::uint64_t uFrameNumber = 0;
	};
#endif

	class clRenderListener
	{
	public:
		virtual ~clRenderListener() = default;

		virtual void OnAttach(const stDeviceDX12&) {}
		virtual void OnFrame(const stFrameDX12&) {}
		virtual void OnAttach(const stDeviceDX11&) {}
		virtual void OnFrame(const stFrameDX11&) {}
		virtual void OnAttach(const stDeviceDX10&) {}
		virtual void OnFrame(const stFrameDX10&) {}
		virtual void OnAttach(const stDeviceDX9&) {}
		virtual void OnFrame(const stFrameDX9&) {}
		virtual void OnAttach(const stDeviceOpenGL&) {}
		virtual void OnFrame(const stFrameOpenGL&) {}
#if OMEGARENDERHOOK_VULKAN
		virtual void OnAttach(const stDeviceVulkan&) {}
		virtual void OnFrame(const stFrameVulkan&) {}
#endif
		virtual void OnDetach(ERenderApi, EDetachReason) {}

		virtual bool OnWindowMessage(HWND, UINT, WPARAM, LPARAM, LRESULT&) { return false; }
	};

	struct stPipelineConfig
	{
		clRenderListener* pListener = nullptr;
		EHookMethod eHookMethod = EHookMethod::VTable;
		bool bDX9 = true;
		bool bDX10 = true;
		bool bDX11 = true;
		bool bDX12 = true;
		bool bOpenGL = true;
		bool bVulkan = true;
		bool bHookWindow = true;
		bool bGameDepth = true;
		HWND hTargetWindow = nullptr;
	};

	struct stPipelineStatus
	{
		bool bInstalled = false;
		EHookMethod eHookMethod = EHookMethod::VTable;
		ERenderApi eApi = ERenderApi::None;
		EQueueSource eQueueSource = EQueueSource::None;
		HWND hWindow = nullptr;
		DXGI_FORMAT eFormat = DXGI_FORMAT_UNKNOWN;
		UINT uWidth = 0;
		UINT uHeight = 0;
		UINT uBufferCount = 0;
		std::uint64_t uFramesRendered = 0;
		std::uint64_t uFramesSkipped = 0;
		std::uint64_t uSwapChainsSeen = 0;
		bool bListenerFaulted = false;
		bool bDeviceLost = false;
		bool bWindowHooked = false;
	};

	namespace Pipeline
	{
		void PrepareEarly(bool bHookVulkan = true);
		void NoteVulkanDevice(void* hDevice, void* hPhysicalDevice);
		[[nodiscard]] std::expected<void, std::string> Install(const stPipelineConfig& stConfig);
		bool Uninstall();
		[[nodiscard]] bool IsInstalled();
		[[nodiscard]] stPipelineStatus GetStatus();
	}
}
