#include "OmegaRenderHook/Render/Vulkan/HookVulkan.hpp"

#if OMEGARENDERHOOK_VULKAN

#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Hook/InlineHook.hpp"
#include "OmegaRenderHook/Render/PipelineInternal.hpp"
#include "OmegaRenderHook/Render/Vulkan/DepthVulkan.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define OMEGARENDERHOOK_VK_INSTANCE_FUNCTIONS(X) \
	X(vkDestroyInstance) \
	X(vkEnumeratePhysicalDevices) \
	X(vkGetPhysicalDeviceProperties) \
	X(vkGetPhysicalDeviceQueueFamilyProperties) \
	X(vkCreateDevice) \
	X(vkGetDeviceProcAddr)

#define OMEGARENDERHOOK_VK_DEVICE_FUNCTIONS(X) \
	X(vkDeviceWaitIdle) \
	X(vkQueueWaitIdle) \
	X(vkQueueSubmit) \
	X(vkGetSwapchainImagesKHR) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkCreateRenderPass) \
	X(vkDestroyRenderPass) \
	X(vkCreateFramebuffer) \
	X(vkDestroyFramebuffer) \
	X(vkCreateCommandPool) \
	X(vkDestroyCommandPool) \
	X(vkAllocateCommandBuffers) \
	X(vkResetCommandBuffer) \
	X(vkBeginCommandBuffer) \
	X(vkEndCommandBuffer) \
	X(vkCmdBeginRenderPass) \
	X(vkCmdEndRenderPass) \
	X(vkCreateFence) \
	X(vkDestroyFence) \
	X(vkWaitForFences) \
	X(vkResetFences) \
	X(vkCreateSemaphore) \
	X(vkDestroySemaphore)

#define OMEGARENDERHOOK_VK_DETOURS(Name) \
	{ reinterpret_cast<void*>(&Name<0>), reinterpret_cast<void*>(&Name<1>), reinterpret_cast<void*>(&Name<2>), reinterpret_cast<void*>(&Name<3>) }

namespace OmegaRenderHook::HookVulkan
{
	namespace
	{
		constexpr std::size_t uMaxTargets = 4;
		constexpr std::uint64_t uFenceTimeout = 1'000'000'000;

		struct stInstanceFunctions
		{
#define OMEGARENDERHOOK_VK_DECLARE(Name) PFN_##Name Name = nullptr;
			OMEGARENDERHOOK_VK_INSTANCE_FUNCTIONS(OMEGARENDERHOOK_VK_DECLARE)
#undef OMEGARENDERHOOK_VK_DECLARE
		};

		struct stDeviceFunctions
		{
#define OMEGARENDERHOOK_VK_DECLARE(Name) PFN_##Name Name = nullptr;
			OMEGARENDERHOOK_VK_DEVICE_FUNCTIONS(OMEGARENDERHOOK_VK_DECLARE)
#undef OMEGARENDERHOOK_VK_DECLARE
		};

		struct stHookGroup
		{
			const char* szName = nullptr;
			std::array<void*, uMaxTargets> aDetours{};
			std::array<void*, uMaxTargets> aTargets{};
			std::size_t uTargets = 0;
			std::array<clInlineHook, uMaxTargets> aHooks;

			template <typename T>
			[[nodiscard]] T Original(std::size_t uIndex) const
			{
				return aHooks[uIndex].Original<T>();
			}
		};

		struct stPhysicalDevice
		{
			VkPhysicalDevice hHandle = VK_NULL_HANDLE;
			VkPhysicalDeviceProperties stProperties{};
			std::vector<VkQueueFamilyProperties> aFamilies;
		};

		struct stSwapchain
		{
			VkDevice hDevice = VK_NULL_HANDLE;
			VkSurfaceKHR hSurface = VK_NULL_HANDLE;
			VkFormat eFormat = VK_FORMAT_UNDEFINED;
			VkExtent2D stExtent{};
			VkImageUsageFlags eUsage = 0;
			HWND hWindow = nullptr;
			bool bKnown = false;
			bool bFailed = false;
		};

		struct stQueue
		{
			VkDevice hDevice = VK_NULL_HANDLE;
			std::uint32_t uFamily = 0;
		};

		struct stImage
		{
			VkImageView hView = VK_NULL_HANDLE;
			VkFramebuffer hFramebuffer = VK_NULL_HANDLE;
			VkSemaphore hRenderDone = VK_NULL_HANDLE;
		};

		struct stSlot
		{
			VkCommandBuffer hCommand = VK_NULL_HANDLE;
			VkFence hFence = VK_NULL_HANDLE;
		};

		struct stRenderer
		{
			stDeviceFunctions stVk;
			VkDevice hDevice = VK_NULL_HANDLE;
			VkQueue hQueue = VK_NULL_HANDLE;
			VkSwapchainKHR hSwapchain = VK_NULL_HANDLE;
			VkRenderPass hRenderPass = VK_NULL_HANDLE;
			VkCommandPool hPool = VK_NULL_HANDLE;
			std::vector<stImage> aImages;
			std::vector<stSlot> aSlots;
			std::size_t uSlot = 0;
			stDeviceVulkan stDevice;
			std::uint64_t uFrameNumber = 0;
			bool bAttached = false;
		};

		struct stState
		{
			std::recursive_mutex mtxRender;
			bool bInstalled = false;
			PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr = nullptr;
			PFN_vkGetPhysicalDeviceProperties pfnLoaderPhysicalProperties = nullptr;
			VkInstance hInstance = VK_NULL_HANDLE;
			stInstanceFunctions stInstance;
			std::vector<stPhysicalDevice> aPhysical;

			clInlineHook clCreateDevice;
			clInlineHook clCreateWin32Surface;
			stHookGroup stQueuePresent;
			stHookGroup stCreateSwapchain;
			stHookGroup stDestroySwapchain;
			stHookGroup stAcquireNextImage;
			stHookGroup stAcquireNextImage2;
			stHookGroup stGetDeviceQueue;
			stHookGroup stGetDeviceQueue2;
			stHookGroup stDestroyDevice;

			std::unordered_map<VkDevice, std::size_t> mapDevices;
			std::unordered_map<VkDevice, VkPhysicalDevice> mapEarlyDevices;
			std::unordered_map<VkSurfaceKHR, HWND> mapSurfaces;
			std::unordered_map<VkSwapchainKHR, stSwapchain> mapSwapchains;
			std::unordered_set<VkDevice> setForcedDevices;
			std::unordered_map<VkQueue, stQueue> mapQueues;
			std::unique_ptr<stRenderer> pRenderer;
			PipelineInternal::clDetachRequest clDetach;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		thread_local int t_iPresentDepth = 0;

		template <typename T>
		[[nodiscard]] void* DispatchKey(T hHandle)
		{
			return hHandle ? *reinterpret_cast<void**>(hHandle) : nullptr;
		}

		template <typename F>
		void ForEachGroup(stState& rState, F&& fnVisit)
		{
			fnVisit(rState.stQueuePresent);
			fnVisit(rState.stCreateSwapchain);
			fnVisit(rState.stDestroySwapchain);
			fnVisit(rState.stAcquireNextImage);
			fnVisit(rState.stAcquireNextImage2);
			fnVisit(rState.stGetDeviceQueue);
			fnVisit(rState.stGetDeviceQueue2);
			fnVisit(rState.stDestroyDevice);
		}

		[[nodiscard]] std::size_t FindPhysical(stState& rState, VkPhysicalDevice hLoaderPhysical)
		{
			if (!hLoaderPhysical || !rState.pfnLoaderPhysicalProperties)
				return SIZE_MAX;
			VkPhysicalDeviceProperties stProperties{};
			rState.pfnLoaderPhysicalProperties(hLoaderPhysical, &stProperties);
			for (std::size_t i = 0; i < rState.aPhysical.size(); ++i)
			{
				const VkPhysicalDeviceProperties& rOurs = rState.aPhysical[i].stProperties;
				if (rOurs.vendorID == stProperties.vendorID && rOurs.deviceID == stProperties.deviceID && std::strcmp(rOurs.deviceName, stProperties.deviceName) == 0)
					return i;
			}
			return SIZE_MAX;
		}

		[[nodiscard]] std::size_t GuessPhysical(const stState& rState)
		{
			for (std::size_t i = 0; i < rState.aPhysical.size(); ++i)
				if (rState.aPhysical[i].stProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
					return i;
			return 0;
		}

		struct stWindowSearch
		{
			VkExtent2D stExtent{};
			HWND hExact = nullptr;
			HWND hLargest = nullptr;
			LONG iLargestArea = 0;
		};

		BOOL CALLBACK FindWindowProc(HWND hWindow, LPARAM iParam)
		{
			auto* pSearch = reinterpret_cast<stWindowSearch*>(iParam);
			DWORD uProcessId = 0;
			GetWindowThreadProcessId(hWindow, &uProcessId);
			RECT rcClient{};
			if (uProcessId != GetCurrentProcessId() || !IsWindowVisible(hWindow) || !GetClientRect(hWindow, &rcClient))
				return TRUE;
			const LONG iWidth = rcClient.right - rcClient.left;
			const LONG iHeight = rcClient.bottom - rcClient.top;
			if (!pSearch->hExact && static_cast<std::uint32_t>(iWidth) == pSearch->stExtent.width && static_cast<std::uint32_t>(iHeight) == pSearch->stExtent.height)
				pSearch->hExact = hWindow;
			if (iWidth * iHeight > pSearch->iLargestArea)
			{
				pSearch->iLargestArea = iWidth * iHeight;
				pSearch->hLargest = hWindow;
			}
			return TRUE;
		}

		[[nodiscard]] HWND ResolveWindow(stState& rState, stSwapchain& rSwapchain)
		{
			if (rSwapchain.hWindow && IsWindow(rSwapchain.hWindow))
				return rSwapchain.hWindow;
			if (const auto itSurface = rState.mapSurfaces.find(rSwapchain.hSurface); itSurface != rState.mapSurfaces.end() && IsWindow(itSurface->second))
				return rSwapchain.hWindow = itSurface->second;
			stWindowSearch stSearch{ rSwapchain.stExtent };
			EnumWindows(&FindWindowProc, reinterpret_cast<LPARAM>(&stSearch));
			rSwapchain.hWindow = stSearch.hExact ? stSearch.hExact : stSearch.hLargest;
			if (rSwapchain.hWindow)
				Log(ELogLevel::Debug, "Vulkan surface window not seen at creation, using the process window that matches the swap chain");
			return rSwapchain.hWindow;
		}

		void DestroyObjects(stRenderer& rRenderer)
		{
			const stDeviceFunctions& rVk = rRenderer.stVk;
			const VkDevice hDevice = rRenderer.hDevice;
			for (stSlot& rSlot : rRenderer.aSlots)
				if (rSlot.hFence)
					rVk.vkDestroyFence(hDevice, rSlot.hFence, nullptr);
			rRenderer.aSlots.clear();
			if (rRenderer.hPool)
				rVk.vkDestroyCommandPool(hDevice, rRenderer.hPool, nullptr);
			rRenderer.hPool = VK_NULL_HANDLE;
			for (stImage& rImage : rRenderer.aImages)
			{
				if (rImage.hFramebuffer)
					rVk.vkDestroyFramebuffer(hDevice, rImage.hFramebuffer, nullptr);
				if (rImage.hView)
					rVk.vkDestroyImageView(hDevice, rImage.hView, nullptr);
				if (rImage.hRenderDone)
					rVk.vkDestroySemaphore(hDevice, rImage.hRenderDone, nullptr);
			}
			rRenderer.aImages.clear();
			DepthVulkan::Detach();
			if (rRenderer.hRenderPass)
				rVk.vkDestroyRenderPass(hDevice, rRenderer.hRenderPass, nullptr);
			rRenderer.hRenderPass = VK_NULL_HANDLE;
		}

		void DestroyRenderer(stState& rState, EDetachReason eReason, VkQueue hCurrentQueue)
		{
			if (!rState.pRenderer)
				return;
			stRenderer& rRenderer = *rState.pRenderer;
			if (hCurrentQueue && hCurrentQueue == rRenderer.hQueue)
				rRenderer.stVk.vkQueueWaitIdle(rRenderer.hQueue);
			else
				rRenderer.stVk.vkDeviceWaitIdle(rRenderer.hDevice);
			if (rRenderer.bAttached)
				PipelineInternal::CallListener("OnDetach", [&](clRenderListener* pListener) { pListener->OnDetach(ERenderApi::Vulkan, eReason); });
			DestroyObjects(rRenderer);
			rState.pRenderer.reset();
			PipelineInternal::ReleaseApi(ERenderApi::Vulkan);
			Log(ELogLevel::Info, "Vulkan renderer detached ({})", ToString(eReason));
		}

		[[nodiscard]] bool CreateObjects(stRenderer& rRenderer, const stSwapchain& rSwapchain, std::uint32_t uFamily, VkPhysicalDevice hPhysical)
		{
			PipelineInternal::clOwnWork clOwn;
			const stDeviceFunctions& rVk = rRenderer.stVk;
			const VkDevice hDevice = rRenderer.hDevice;

			std::uint32_t uImages = 0;
			if (rVk.vkGetSwapchainImagesKHR(hDevice, rRenderer.hSwapchain, &uImages, nullptr) != VK_SUCCESS || !uImages)
				return false;
			std::vector<VkImage> aImages(uImages);
			if (rVk.vkGetSwapchainImagesKHR(hDevice, rRenderer.hSwapchain, &uImages, aImages.data()) != VK_SUCCESS)
				return false;
			aImages.resize(uImages);

			VkAttachmentDescription stColor{};
			stColor.format = rSwapchain.eFormat;
			stColor.samples = VK_SAMPLE_COUNT_1_BIT;
			stColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			stColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			stColor.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			stColor.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			stColor.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			stColor.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
			VkAttachmentDescription stDepth{};
			stDepth.format = DepthVulkan::eTargetFormat;
			stDepth.samples = VK_SAMPLE_COUNT_1_BIT;
			stDepth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			stDepth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			stDepth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			stDepth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			stDepth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			stDepth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			const std::array<VkAttachmentDescription, 2> aAttachments{ stColor, stDepth };
			const VkAttachmentReference stReference{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
			const VkAttachmentReference stDepthReference{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
			VkSubpassDescription stSubpass{};
			stSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			stSubpass.colorAttachmentCount = 1;
			stSubpass.pColorAttachments = &stReference;
			stSubpass.pDepthStencilAttachment = &stDepthReference;
			VkSubpassDependency stDependency{};
			stDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
			stDependency.dstSubpass = 0;
			stDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			stDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			stDependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			stDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			VkRenderPassCreateInfo stPass{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
			stPass.attachmentCount = static_cast<std::uint32_t>(aAttachments.size());
			stPass.pAttachments = aAttachments.data();
			stPass.subpassCount = 1;
			stPass.pSubpasses = &stSubpass;
			stPass.dependencyCount = 1;
			stPass.pDependencies = &stDependency;
			if (rVk.vkCreateRenderPass(hDevice, &stPass, nullptr, &rRenderer.hRenderPass) != VK_SUCCESS)
				return false;
			const VkImageView hDepthView = DepthVulkan::Attach({ hDevice, hPhysical, rRenderer.hRenderPass, rSwapchain.stExtent, uImages });
			if (!hDepthView)
				return false;

			rRenderer.aImages.resize(aImages.size());
			for (std::size_t i = 0; i < aImages.size(); ++i)
			{
				stImage& rImage = rRenderer.aImages[i];
				VkImageViewCreateInfo stView{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				stView.image = aImages[i];
				stView.viewType = VK_IMAGE_VIEW_TYPE_2D;
				stView.format = rSwapchain.eFormat;
				stView.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				if (rVk.vkCreateImageView(hDevice, &stView, nullptr, &rImage.hView) != VK_SUCCESS)
					return false;
				VkFramebufferCreateInfo stFramebuffer{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
				stFramebuffer.renderPass = rRenderer.hRenderPass;
				const std::array<VkImageView, 2> aViews{ rImage.hView, hDepthView };
				stFramebuffer.attachmentCount = static_cast<std::uint32_t>(aViews.size());
				stFramebuffer.pAttachments = aViews.data();
				stFramebuffer.width = rSwapchain.stExtent.width;
				stFramebuffer.height = rSwapchain.stExtent.height;
				stFramebuffer.layers = 1;
				if (rVk.vkCreateFramebuffer(hDevice, &stFramebuffer, nullptr, &rImage.hFramebuffer) != VK_SUCCESS)
					return false;
				const VkSemaphoreCreateInfo stSemaphore{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
				if (rVk.vkCreateSemaphore(hDevice, &stSemaphore, nullptr, &rImage.hRenderDone) != VK_SUCCESS)
					return false;
			}

			VkCommandPoolCreateInfo stPool{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			stPool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			stPool.queueFamilyIndex = uFamily;
			if (rVk.vkCreateCommandPool(hDevice, &stPool, nullptr, &rRenderer.hPool) != VK_SUCCESS)
				return false;
			std::vector<VkCommandBuffer> aCommands(aImages.size());
			VkCommandBufferAllocateInfo stAllocate{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			stAllocate.commandPool = rRenderer.hPool;
			stAllocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			stAllocate.commandBufferCount = static_cast<std::uint32_t>(aCommands.size());
			if (rVk.vkAllocateCommandBuffers(hDevice, &stAllocate, aCommands.data()) != VK_SUCCESS)
				return false;
			rRenderer.aSlots.resize(aCommands.size());
			for (std::size_t i = 0; i < aCommands.size(); ++i)
			{
				rRenderer.aSlots[i].hCommand = aCommands[i];
				*reinterpret_cast<void**>(aCommands[i]) = DispatchKey(hDevice);
				VkFenceCreateInfo stFence{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				stFence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
				if (rVk.vkCreateFence(hDevice, &stFence, nullptr, &rRenderer.aSlots[i].hFence) != VK_SUCCESS)
					return false;
			}
			return true;
		}

		[[nodiscard]] stRenderer* CreateRenderer(stState& rState, VkQueue hQueue, VkSwapchainKHR hSwapchain, const stSwapchain& rSwapchain, HWND hWindow)
		{
			if (rState.aPhysical.empty())
				return nullptr;
			if (!(rSwapchain.eUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
			{
				Log(ELogLevel::Warning, "Vulkan swap chain images are not color attachments, overlay disabled for it");
				return nullptr;
			}

			const auto itDevice = rState.mapDevices.find(rSwapchain.hDevice);
			const bool bPhysicalGuessed = itDevice == rState.mapDevices.end();
			const stPhysicalDevice& rPhysical = rState.aPhysical[bPhysicalGuessed ? GuessPhysical(rState) : itDevice->second];

			const auto itQueue = rState.mapQueues.find(hQueue);
			const bool bFamilyGuessed = itQueue == rState.mapQueues.end();
			std::uint32_t uFamily = bFamilyGuessed ? UINT32_MAX : itQueue->second.uFamily;
			if (bFamilyGuessed)
			{
				for (std::uint32_t i = 0; i < rPhysical.aFamilies.size() && uFamily == UINT32_MAX; ++i)
					if (rPhysical.aFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
						uFamily = i;
			}
			if (uFamily >= rPhysical.aFamilies.size() || !(rPhysical.aFamilies[uFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			{
				Log(ELogLevel::Warning, "Vulkan present queue has no graphics support, overlay disabled for this swap chain");
				return nullptr;
			}

			auto pRenderer = std::make_unique<stRenderer>();
			pRenderer->hDevice = rSwapchain.hDevice;
			pRenderer->hQueue = hQueue;
			pRenderer->hSwapchain = hSwapchain;
			bool bLoaded = true;
#define OMEGARENDERHOOK_VK_LOAD(Name) \
			pRenderer->stVk.Name = reinterpret_cast<PFN_##Name>(rState.stInstance.vkGetDeviceProcAddr(pRenderer->hDevice, #Name)); \
			bLoaded = bLoaded && pRenderer->stVk.Name != nullptr;
			OMEGARENDERHOOK_VK_DEVICE_FUNCTIONS(OMEGARENDERHOOK_VK_LOAD)
#undef OMEGARENDERHOOK_VK_LOAD
			if (!bLoaded)
			{
				Log(ELogLevel::Error, "could not load the Vulkan device functions the overlay needs");
				return nullptr;
			}
			DepthVulkan::OnDeviceCreated(pRenderer->hDevice, rPhysical.hHandle);
			if (!CreateObjects(*pRenderer, rSwapchain, uFamily, rPhysical.hHandle))
			{
				Log(ELogLevel::Error, "failed to create the Vulkan overlay render objects");
				pRenderer->stVk.vkDeviceWaitIdle(pRenderer->hDevice);
				DestroyObjects(*pRenderer);
				return nullptr;
			}

			stDeviceVulkan& rDevice = pRenderer->stDevice;
			rDevice.pfnGetInstanceProcAddr = rState.pfnGetInstanceProcAddr;
			rDevice.hInstance = rState.hInstance;
			rDevice.hPhysicalDevice = rPhysical.hHandle;
			rDevice.hDevice = pRenderer->hDevice;
			rDevice.uQueueFamily = uFamily;
			rDevice.hQueue = hQueue;
			rDevice.hRenderPass = pRenderer->hRenderPass;
			rDevice.eFormat = rSwapchain.eFormat;
			rDevice.uImageCount = static_cast<std::uint32_t>(pRenderer->aImages.size());
			rDevice.hWindow = hWindow;
			rDevice.uWidth = rSwapchain.stExtent.width;
			rDevice.uHeight = rSwapchain.stExtent.height;

			Log(ELogLevel::Info, "Vulkan renderer attached: {}x{}, format {}, {} images, {}{}, queue family {}{}", rDevice.uWidth, rDevice.uHeight, static_cast<int>(rDevice.eFormat), rDevice.uImageCount,
				rPhysical.stProperties.deviceName, bPhysicalGuessed ? " (guessed, device created before install)" : "", uFamily, bFamilyGuessed ? " (guessed)" : "");
			rState.pRenderer = std::move(pRenderer);
			rState.pRenderer->bAttached = true;
			PipelineInternal::CallListener("OnAttach", [&](clRenderListener* pListener) { pListener->OnAttach(rState.pRenderer->stDevice); });
			return rState.pRenderer.get();
		}

		[[nodiscard]] bool RenderFrame(stState& rState, VkQueue hQueue, VkSwapchainKHR hSwapchain, stSwapchain& rSwapchain, std::uint32_t uImageIndex, HWND hWindow, const VkPresentInfoKHR& rInfo, VkSemaphore& rWait)
		{
			if (rState.pRenderer && (rState.pRenderer->hSwapchain != hSwapchain || rState.pRenderer->hQueue != hQueue))
				DestroyRenderer(rState, EDetachReason::SwapChainChanged, hQueue);
			stRenderer* pRenderer = rState.pRenderer.get();
			if (!pRenderer && !(pRenderer = CreateRenderer(rState, hQueue, hSwapchain, rSwapchain, hWindow)))
			{
				rSwapchain.bFailed = true;
				return false;
			}
			if (uImageIndex >= pRenderer->aImages.size())
				return false;

			clRenderListener* pListener = PipelineInternal::Listener();
			if (!pListener)
				return false;

			PipelineInternal::clOwnWork clOwn;
			const stDeviceFunctions& rVk = pRenderer->stVk;
			const std::uint32_t uSlotIndex = static_cast<std::uint32_t>(pRenderer->uSlot);
			stSlot& rSlot = pRenderer->aSlots[pRenderer->uSlot];
			if (rVk.vkWaitForFences(pRenderer->hDevice, 1, &rSlot.hFence, VK_TRUE, uFenceTimeout) != VK_SUCCESS)
			{
				PipelineInternal::CountFrame(false);
				return false;
			}
			rVk.vkResetCommandBuffer(rSlot.hCommand, 0);
			VkCommandBufferBeginInfo stBegin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			stBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			if (rVk.vkBeginCommandBuffer(rSlot.hCommand, &stBegin) != VK_SUCCESS)
			{
				PipelineInternal::CountFrame(false);
				return false;
			}
			const bool bDepth = DepthVulkan::BeforePass(rSlot.hCommand, uSlotIndex);
			std::array<VkClearValue, 2> aClear{};
			aClear[1].depthStencil = { 1.f, 0 };
			VkRenderPassBeginInfo stPass{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
			stPass.renderPass = pRenderer->hRenderPass;
			stPass.framebuffer = pRenderer->aImages[uImageIndex].hFramebuffer;
			stPass.renderArea = { { 0, 0 }, rSwapchain.stExtent };
			stPass.clearValueCount = static_cast<std::uint32_t>(aClear.size());
			stPass.pClearValues = aClear.data();
			rVk.vkCmdBeginRenderPass(rSlot.hCommand, &stPass, VK_SUBPASS_CONTENTS_INLINE);
			if (bDepth)
				DepthVulkan::InPass(rSlot.hCommand, uSlotIndex);

			pRenderer->stDevice.hWindow = hWindow;
			pRenderer->stDevice.uWidth = rSwapchain.stExtent.width;
			pRenderer->stDevice.uHeight = rSwapchain.stExtent.height;
			stFrameVulkan stFrame{ &pRenderer->stDevice, rSlot.hCommand, rSwapchain.stExtent, bDepth, bDepth && DepthVulkan::ReversedZ(), uImageIndex, pRenderer->uFrameNumber++ };
			auto fnFrame = [&] { pListener->OnFrame(stFrame); };
			stFault stFault;
			if (!InvokeGuarded(fnFrame, stFault))
				PipelineInternal::ReportFault("OnFrame", stFault);

			rVk.vkCmdEndRenderPass(rSlot.hCommand);
			DepthVulkan::AfterPass(rSlot.hCommand, bDepth);
			if (rVk.vkEndCommandBuffer(rSlot.hCommand) != VK_SUCCESS)
			{
				PipelineInternal::CountFrame(false);
				return false;
			}

			const std::vector<VkPipelineStageFlags> aStages(rInfo.waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
			const VkSemaphore hRenderDone = pRenderer->aImages[uImageIndex].hRenderDone;
			VkSubmitInfo stSubmit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
			stSubmit.waitSemaphoreCount = rInfo.waitSemaphoreCount;
			stSubmit.pWaitSemaphores = rInfo.pWaitSemaphores;
			stSubmit.pWaitDstStageMask = aStages.data();
			stSubmit.commandBufferCount = 1;
			stSubmit.pCommandBuffers = &rSlot.hCommand;
			stSubmit.signalSemaphoreCount = 1;
			stSubmit.pSignalSemaphores = &hRenderDone;
			rVk.vkResetFences(pRenderer->hDevice, 1, &rSlot.hFence);
			if (const VkResult eResult = rVk.vkQueueSubmit(hQueue, 1, &stSubmit, rSlot.hFence); eResult != VK_SUCCESS)
			{
				Log(ELogLevel::Error, "Vulkan overlay submit failed ({})", static_cast<int>(eResult));
				DestroyRenderer(rState, eResult == VK_ERROR_DEVICE_LOST ? EDetachReason::DeviceLost : EDetachReason::SwapChainChanged, hQueue);
				rSwapchain.bFailed = true;
				return false;
			}
			pRenderer->uSlot = (pRenderer->uSlot + 1) % pRenderer->aSlots.size();
			rWait = hRenderDone;
			PipelineInternal::CountFrame(true);
			return true;
		}

		[[nodiscard]] bool PrepareFrame(VkQueue hQueue, const VkPresentInfoKHR& rInfo, VkSemaphore& rWait)
		{
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			if (rState.clDetach.IsRequested())
			{
				DestroyRenderer(rState, EDetachReason::Uninstall, hQueue);
				rState.clDetach.MarkDone();
				return false;
			}
			if (!PipelineInternal::IsActive() || !rInfo.pSwapchains || !rInfo.pImageIndices)
				return false;

			for (std::uint32_t i = 0; i < rInfo.swapchainCount; ++i)
			{
				const auto itSwapchain = rState.mapSwapchains.find(rInfo.pSwapchains[i]);
				if (itSwapchain == rState.mapSwapchains.end())
					continue;
				stSwapchain& rSwapchain = itSwapchain->second;
				if (!rSwapchain.bKnown || rSwapchain.bFailed || DispatchKey(hQueue) != DispatchKey(rSwapchain.hDevice))
					continue;
				const HWND hWindow = ResolveWindow(rState, rSwapchain);
				if (!PipelineInternal::AcquireWindow(hWindow, ERenderApi::Vulkan, rSwapchain.stExtent.width, rSwapchain.stExtent.height))
					continue;
				return RenderFrame(rState, hQueue, rInfo.pSwapchains[i], rSwapchain, rInfo.pImageIndices[i], hWindow, rInfo, rWait);
			}
			return false;
		}

		[[nodiscard]] bool ShouldForceRecreate(VkDevice hDevice, VkSwapchainKHR hSwapchain)
		{
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			if (!PipelineInternal::IsActive() || rState.mapSwapchains.contains(hSwapchain))
				return false;
			stSwapchain& rSwapchain = rState.mapSwapchains[hSwapchain];
			rSwapchain.hDevice = hDevice;
			if (!rState.setForcedDevices.insert(hDevice).second)
			{
				Log(ELogLevel::Warning, "Vulkan swap chain {} was not seen being created even after a forced recreate, the overlay skips it", static_cast<void*>(hSwapchain));
				return false;
			}
			const auto itDevice = rState.mapDevices.find(hDevice);
			if (!rState.aPhysical.empty())
				DepthVulkan::OnDeviceCreated(hDevice, rState.aPhysical[itDevice == rState.mapDevices.end() ? GuessPhysical(rState) : itDevice->second].hHandle);
			Log(ELogLevel::Info, "Vulkan swap chain {} was created before install, reporting it out of date once so the game recreates it", static_cast<void*>(hSwapchain));
			return true;
		}

		VkResult OnQueuePresent(PFN_vkQueuePresentKHR pfnNext, VkQueue hQueue, const VkPresentInfoKHR* pInfo)
		{
			if (t_iPresentDepth > 0 || !pInfo)
				return pfnNext(hQueue, pInfo);
			++t_iPresentDepth;
			VkPresentInfoKHR stInfo = *pInfo;
			VkSemaphore hWait = VK_NULL_HANDLE;
			if (PrepareFrame(hQueue, *pInfo, hWait))
			{
				stInfo.waitSemaphoreCount = 1;
				stInfo.pWaitSemaphores = &hWait;
			}
			const VkResult eResult = pfnNext(hQueue, &stInfo);
			--t_iPresentDepth;
			return eResult;
		}

		VkResult OnCreateSwapchain(PFN_vkCreateSwapchainKHR pfnNext, VkDevice hDevice, const VkSwapchainCreateInfoKHR* pInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
		{
			const VkResult eResult = pfnNext(hDevice, pInfo, pAllocator, pSwapchain);
			if (eResult != VK_SUCCESS)
				Log(ELogLevel::Debug, "Vulkan swap chain creation failed ({})", static_cast<int>(eResult));
			if (eResult != VK_SUCCESS || !pInfo || !pSwapchain || !*pSwapchain)
				return eResult;
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			stSwapchain& rSwapchain = rState.mapSwapchains[*pSwapchain];
			rSwapchain = {};
			rSwapchain.hDevice = hDevice;
			rSwapchain.hSurface = pInfo->surface;
			rSwapchain.eFormat = pInfo->imageFormat;
			rSwapchain.stExtent = pInfo->imageExtent;
			rSwapchain.eUsage = pInfo->imageUsage;
			rSwapchain.bKnown = true;
			Log(ELogLevel::Debug, "Vulkan swap chain {} created ({}x{}, format {}, replaces {})", static_cast<void*>(*pSwapchain), pInfo->imageExtent.width, pInfo->imageExtent.height, static_cast<int>(pInfo->imageFormat), static_cast<void*>(pInfo->oldSwapchain));
			return eResult;
		}

		void OnDestroySwapchain(PFN_vkDestroySwapchainKHR pfnNext, VkDevice hDevice, VkSwapchainKHR hSwapchain, const VkAllocationCallbacks* pAllocator)
		{
			{
				stState& rState = State();
				std::scoped_lock lckRender(rState.mtxRender);
				if (rState.pRenderer && rState.pRenderer->hSwapchain == hSwapchain)
					DestroyRenderer(rState, EDetachReason::SwapChainChanged, VK_NULL_HANDLE);
				rState.mapSwapchains.erase(hSwapchain);
			}
			pfnNext(hDevice, hSwapchain, pAllocator);
		}

		void OnDestroyDevice(PFN_vkDestroyDevice pfnNext, VkDevice hDevice, const VkAllocationCallbacks* pAllocator)
		{
			{
				stState& rState = State();
				std::scoped_lock lckRender(rState.mtxRender);
				if (rState.pRenderer && rState.pRenderer->hDevice == hDevice)
					DestroyRenderer(rState, EDetachReason::DeviceLost, VK_NULL_HANDLE);
				std::erase_if(rState.mapSwapchains, [&](const auto& rEntry) { return rEntry.second.hDevice == hDevice; });
				rState.setForcedDevices.erase(hDevice);
				std::erase_if(rState.mapQueues, [&](const auto& rEntry) { return rEntry.second.hDevice == hDevice; });
				rState.mapDevices.erase(hDevice);
			}
			pfnNext(hDevice, pAllocator);
		}

		void NoteQueue(VkDevice hDevice, std::uint32_t uFamily, VkQueue hQueue)
		{
			if (!hQueue)
				return;
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			rState.mapQueues[hQueue] = { hDevice, uFamily };
		}

		template <std::size_t N>
		VkResult VKAPI_CALL HookQueuePresent(VkQueue hQueue, const VkPresentInfoKHR* pInfo)
		{
			clHookScope clScope;
			PipelineInternal::clLayeredScope clLayered;
			return OnQueuePresent(State().stQueuePresent.Original<PFN_vkQueuePresentKHR>(N), hQueue, pInfo);
		}

		template <std::size_t N>
		VkResult VKAPI_CALL HookCreateSwapchain(VkDevice hDevice, const VkSwapchainCreateInfoKHR* pInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
		{
			clHookScope clScope;
			PipelineInternal::clLayeredScope clLayered;
			return OnCreateSwapchain(State().stCreateSwapchain.Original<PFN_vkCreateSwapchainKHR>(N), hDevice, pInfo, pAllocator, pSwapchain);
		}

		template <std::size_t N>
		void VKAPI_CALL HookDestroySwapchain(VkDevice hDevice, VkSwapchainKHR hSwapchain, const VkAllocationCallbacks* pAllocator)
		{
			clHookScope clScope;
			OnDestroySwapchain(State().stDestroySwapchain.Original<PFN_vkDestroySwapchainKHR>(N), hDevice, hSwapchain, pAllocator);
		}

		template <std::size_t N>
		VkResult VKAPI_CALL HookAcquireNextImage(VkDevice hDevice, VkSwapchainKHR hSwapchain, std::uint64_t uTimeout, VkSemaphore hSemaphore, VkFence hFence, std::uint32_t* pImageIndex)
		{
			clHookScope clScope;
			const auto pfnNext = State().stAcquireNextImage.Original<PFN_vkAcquireNextImageKHR>(N);
			if (ShouldForceRecreate(hDevice, hSwapchain))
				return VK_ERROR_OUT_OF_DATE_KHR;
			return pfnNext(hDevice, hSwapchain, uTimeout, hSemaphore, hFence, pImageIndex);
		}

		template <std::size_t N>
		VkResult VKAPI_CALL HookAcquireNextImage2(VkDevice hDevice, const VkAcquireNextImageInfoKHR* pInfo, std::uint32_t* pImageIndex)
		{
			clHookScope clScope;
			const auto pfnNext = State().stAcquireNextImage2.Original<PFN_vkAcquireNextImage2KHR>(N);
			if (pInfo && ShouldForceRecreate(hDevice, pInfo->swapchain))
				return VK_ERROR_OUT_OF_DATE_KHR;
			return pfnNext(hDevice, pInfo, pImageIndex);
		}

		template <std::size_t N>
		void VKAPI_CALL HookGetDeviceQueue(VkDevice hDevice, std::uint32_t uFamily, std::uint32_t uIndex, VkQueue* pQueue)
		{
			clHookScope clScope;
			State().stGetDeviceQueue.Original<PFN_vkGetDeviceQueue>(N)(hDevice, uFamily, uIndex, pQueue);
			if (pQueue)
				NoteQueue(hDevice, uFamily, *pQueue);
		}

		template <std::size_t N>
		void VKAPI_CALL HookGetDeviceQueue2(VkDevice hDevice, const VkDeviceQueueInfo2* pInfo, VkQueue* pQueue)
		{
			clHookScope clScope;
			State().stGetDeviceQueue2.Original<PFN_vkGetDeviceQueue2>(N)(hDevice, pInfo, pQueue);
			if (pInfo && pQueue)
				NoteQueue(hDevice, pInfo->queueFamilyIndex, *pQueue);
		}

		template <std::size_t N>
		void VKAPI_CALL HookDestroyDevice(VkDevice hDevice, const VkAllocationCallbacks* pAllocator)
		{
			clHookScope clScope;
			OnDestroyDevice(State().stDestroyDevice.Original<PFN_vkDestroyDevice>(N), hDevice, pAllocator);
		}

		VkResult VKAPI_CALL HookCreateDevice(VkPhysicalDevice hPhysical, const VkDeviceCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
		{
			clHookScope clScope;
			stState& rState = State();
			const VkResult eResult = rState.clCreateDevice.Original<PFN_vkCreateDevice>()(hPhysical, pInfo, pAllocator, pDevice);
			if (eResult == VK_SUCCESS && pDevice && *pDevice)
			{
				{
					std::scoped_lock lckRender(rState.mtxRender);
					if (const std::size_t uPhysical = FindPhysical(rState, hPhysical); uPhysical != SIZE_MAX)
						rState.mapDevices[*pDevice] = uPhysical;
					else if (!rState.bInstalled)
						rState.mapEarlyDevices[*pDevice] = hPhysical;
				}
				DepthVulkan::OnDeviceCreated(*pDevice, hPhysical);
			}
			return eResult;
		}

		VkResult VKAPI_CALL HookCreateWin32Surface(VkInstance hInstance, const VkWin32SurfaceCreateInfoKHR* pInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
		{
			clHookScope clScope;
			stState& rState = State();
			const VkResult eResult = rState.clCreateWin32Surface.Original<PFN_vkCreateWin32SurfaceKHR>()(hInstance, pInfo, pAllocator, pSurface);
			if (eResult == VK_SUCCESS && pInfo && pSurface && *pSurface)
			{
				std::scoped_lock lckRender(rState.mtxRender);
				rState.mapSurfaces[*pSurface] = pInfo->hwnd;
			}
			return eResult;
		}

		void SetupGroups(stState& rState)
		{
			auto fnSetup = [](stHookGroup& rGroup, const char* szName, std::array<void*, uMaxTargets> aDetours)
			{
				rGroup.szName = szName;
				rGroup.aDetours = aDetours;
				rGroup.aTargets = {};
				rGroup.uTargets = 0;
			};
			fnSetup(rState.stQueuePresent, "vkQueuePresentKHR", OMEGARENDERHOOK_VK_DETOURS(HookQueuePresent));
			fnSetup(rState.stCreateSwapchain, "vkCreateSwapchainKHR", OMEGARENDERHOOK_VK_DETOURS(HookCreateSwapchain));
			fnSetup(rState.stDestroySwapchain, "vkDestroySwapchainKHR", OMEGARENDERHOOK_VK_DETOURS(HookDestroySwapchain));
			fnSetup(rState.stAcquireNextImage, "vkAcquireNextImageKHR", OMEGARENDERHOOK_VK_DETOURS(HookAcquireNextImage));
			fnSetup(rState.stAcquireNextImage2, "vkAcquireNextImage2KHR", OMEGARENDERHOOK_VK_DETOURS(HookAcquireNextImage2));
			fnSetup(rState.stGetDeviceQueue, "vkGetDeviceQueue", OMEGARENDERHOOK_VK_DETOURS(HookGetDeviceQueue));
			fnSetup(rState.stGetDeviceQueue2, "vkGetDeviceQueue2", OMEGARENDERHOOK_VK_DETOURS(HookGetDeviceQueue2));
			fnSetup(rState.stDestroyDevice, "vkDestroyDevice", OMEGARENDERHOOK_VK_DETOURS(HookDestroyDevice));
		}

		void AddTarget(stHookGroup& rGroup, void* pTarget)
		{
			if (!pTarget)
				return;
			for (std::size_t i = 0; i < rGroup.uTargets; ++i)
				if (rGroup.aTargets[i] == pTarget)
					return;
			if (rGroup.uTargets == uMaxTargets)
			{
				Log(ELogLevel::Warning, "more than {} distinct {} implementations, extra ones are not hooked", uMaxTargets, rGroup.szName);
				return;
			}
			rGroup.aTargets[rGroup.uTargets++] = pTarget;
		}

		void CaptureTargets(stState& rState)
		{
			for (const stPhysicalDevice& rPhysical : rState.aPhysical)
			{
				const float flPriority = 1.f;
				VkDeviceQueueCreateInfo stQueue{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
				stQueue.queueFamilyIndex = 0;
				stQueue.queueCount = 1;
				stQueue.pQueuePriorities = &flPriority;
				const char* szSwapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
				VkDeviceCreateInfo stCreate{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
				stCreate.queueCreateInfoCount = 1;
				stCreate.pQueueCreateInfos = &stQueue;
				stCreate.enabledExtensionCount = 1;
				stCreate.ppEnabledExtensionNames = &szSwapchain;
				VkDevice hDevice = VK_NULL_HANDLE;
				if (rState.stInstance.vkCreateDevice(rPhysical.hHandle, &stCreate, nullptr, &hDevice) != VK_SUCCESS)
				{
					Log(ELogLevel::Debug, "throwaway Vulkan device on {} failed, its functions are not hooked", rPhysical.stProperties.deviceName);
					continue;
				}
				ForEachGroup(rState, [&](stHookGroup& rGroup) { AddTarget(rGroup, reinterpret_cast<void*>(rState.stInstance.vkGetDeviceProcAddr(hDevice, rGroup.szName))); });
				if (const auto pfnDestroy = reinterpret_cast<PFN_vkDestroyDevice>(rState.stInstance.vkGetDeviceProcAddr(hDevice, "vkDestroyDevice")))
					pfnDestroy(hDevice, nullptr);
			}
		}

		void HookSwapchainTracking(stState& rState, VkDevice hDevice)
		{
			const HMODULE hLoader = GetModuleHandleW(L"vulkan-1.dll");
			const auto pfnGetDeviceProcAddr = hLoader ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(hLoader, "vkGetDeviceProcAddr"))) : nullptr;
			if (!pfnGetDeviceProcAddr)
				return;
			if (!rState.stCreateSwapchain.szName)
				SetupGroups(rState);
			std::size_t uHooks = 0;
			for (stHookGroup* pGroup : { &rState.stCreateSwapchain, &rState.stDestroySwapchain })
			{
				AddTarget(*pGroup, reinterpret_cast<void*>(pfnGetDeviceProcAddr(hDevice, pGroup->szName)));
				for (std::size_t i = 0; i < pGroup->uTargets; ++i)
					if (!pGroup->aHooks[i].IsInstalled() && pGroup->aHooks[i].Install(pGroup->aTargets[i], pGroup->aDetours[i]))
						++uHooks;
			}
			if (uHooks)
				Log(ELogLevel::Debug, "Vulkan swap chain creation hooked early on device {} ({} functions)", static_cast<void*>(hDevice), uHooks);
		}

		void UninstallHooks(stState& rState)
		{
			ForEachGroup(rState, [](stHookGroup& rGroup)
			{
				for (clInlineHook& rHook : rGroup.aHooks)
					rHook.Uninstall();
				rGroup.uTargets = 0;
			});
			rState.clCreateDevice.Uninstall();
			rState.clCreateWin32Surface.Uninstall();
		}

		void DestroyInstance(stState& rState)
		{
			if (rState.hInstance && rState.stInstance.vkDestroyInstance)
				rState.stInstance.vkDestroyInstance(rState.hInstance, nullptr);
			rState.hInstance = VK_NULL_HANDLE;
			rState.stInstance = {};
			rState.aPhysical.clear();
		}
	}

	void PrepareEarly()
	{
		stState& rState = State();
		std::scoped_lock lckRender(rState.mtxRender);
		if (rState.bInstalled || rState.clCreateDevice.IsInstalled())
			return;
		const HMODULE hLoader = GetModuleHandleW(L"vulkan-1.dll");
		void* pCreateDevice = hLoader ? reinterpret_cast<void*>(GetProcAddress(hLoader, "vkCreateDevice")) : nullptr;
		if (pCreateDevice && rState.clCreateDevice.Install(pCreateDevice, reinterpret_cast<void*>(&HookCreateDevice)))
			Log(ELogLevel::Debug, "Vulkan device creation hooked early for depth tracking");
	}

	void NoteDevice(void* hDevice, void* hPhysicalDevice)
	{
		const auto hVkDevice = static_cast<VkDevice>(hDevice);
		const auto hVkPhysical = static_cast<VkPhysicalDevice>(hPhysicalDevice);
		if (!hVkDevice)
			return;
		{
			stState& rState = State();
			std::scoped_lock lckRender(rState.mtxRender);
			if (rState.bInstalled)
			{
				if (const std::size_t uPhysical = FindPhysical(rState, hVkPhysical); uPhysical != SIZE_MAX)
					rState.mapDevices[hVkDevice] = uPhysical;
			}
			else
			{
				rState.mapEarlyDevices[hVkDevice] = hVkPhysical;
				HookSwapchainTracking(rState, hVkDevice);
			}
		}
		DepthVulkan::OnDeviceCreated(hVkDevice, hVkPhysical);
	}

	bool DisableEarly()
	{
		stState& rState = State();
		bool bRestored = rState.clCreateDevice.Disable();
		for (stHookGroup* pGroup : { &rState.stCreateSwapchain, &rState.stDestroySwapchain })
			for (clInlineHook& rHook : pGroup->aHooks)
				bRestored = rHook.Disable() && bRestored;
		return DepthVulkan::Disable() && bRestored;
	}

	void ReleaseEarly(bool bDrained)
	{
		stState& rState = State();
		DepthVulkan::Release(bDrained);
		std::scoped_lock lckRender(rState.mtxRender);
		rState.mapEarlyDevices.clear();
		if (bDrained && !rState.bInstalled)
			UninstallHooks(rState);
	}

	bool Install()
	{
		stState& rState = State();
		std::scoped_lock lckRender(rState.mtxRender);
		if (rState.bInstalled)
			return true;
		const HMODULE hLoader = GetModuleHandleW(L"vulkan-1.dll");
		if (!hLoader)
		{
			Log(ELogLevel::Debug, "vulkan-1.dll is not loaded, Vulkan hooks skipped");
			return false;
		}
		rState.pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(hLoader, "vkGetInstanceProcAddr")));
		rState.pfnLoaderPhysicalProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(reinterpret_cast<void*>(GetProcAddress(hLoader, "vkGetPhysicalDeviceProperties")));
		void* pCreateDevice = reinterpret_cast<void*>(GetProcAddress(hLoader, "vkCreateDevice"));
		void* pCreateWin32Surface = reinterpret_cast<void*>(GetProcAddress(hLoader, "vkCreateWin32SurfaceKHR"));
		const auto pfnCreateInstance = rState.pfnGetInstanceProcAddr ? reinterpret_cast<PFN_vkCreateInstance>(rState.pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
		if (!pfnCreateInstance || !pCreateDevice || !rState.pfnLoaderPhysicalProperties)
		{
			Log(ELogLevel::Error, "vulkan-1.dll is missing loader exports, Vulkan hooks skipped");
			return false;
		}

		VkApplicationInfo stApplication{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		stApplication.pApplicationName = "OmegaRenderHook";
		stApplication.apiVersion = VK_API_VERSION_1_0;
		VkInstanceCreateInfo stInstance{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		stInstance.pApplicationInfo = &stApplication;
		if (const VkResult eResult = pfnCreateInstance(&stInstance, nullptr, &rState.hInstance); eResult != VK_SUCCESS)
		{
			Log(ELogLevel::Warning, "could not create a Vulkan instance ({}), Vulkan hooks skipped", static_cast<int>(eResult));
			rState.hInstance = VK_NULL_HANDLE;
			return false;
		}
		bool bLoaded = true;
#define OMEGARENDERHOOK_VK_LOAD(Name) \
		rState.stInstance.Name = reinterpret_cast<PFN_##Name>(rState.pfnGetInstanceProcAddr(rState.hInstance, #Name)); \
		bLoaded = bLoaded && rState.stInstance.Name != nullptr;
		OMEGARENDERHOOK_VK_INSTANCE_FUNCTIONS(OMEGARENDERHOOK_VK_LOAD)
#undef OMEGARENDERHOOK_VK_LOAD

		std::uint32_t uPhysical = 0;
		if (bLoaded && rState.stInstance.vkEnumeratePhysicalDevices(rState.hInstance, &uPhysical, nullptr) == VK_SUCCESS && uPhysical)
		{
			std::vector<VkPhysicalDevice> aHandles(uPhysical);
			if (rState.stInstance.vkEnumeratePhysicalDevices(rState.hInstance, &uPhysical, aHandles.data()) >= VK_SUCCESS)
			{
				aHandles.resize(uPhysical);
				for (VkPhysicalDevice hHandle : aHandles)
				{
					stPhysicalDevice& rPhysical = rState.aPhysical.emplace_back();
					rPhysical.hHandle = hHandle;
					rState.stInstance.vkGetPhysicalDeviceProperties(hHandle, &rPhysical.stProperties);
					std::uint32_t uFamilies = 0;
					rState.stInstance.vkGetPhysicalDeviceQueueFamilyProperties(hHandle, &uFamilies, nullptr);
					rPhysical.aFamilies.resize(uFamilies);
					rState.stInstance.vkGetPhysicalDeviceQueueFamilyProperties(hHandle, &uFamilies, rPhysical.aFamilies.data());
				}
			}
		}
		if (rState.aPhysical.empty())
		{
			Log(ELogLevel::Warning, "no Vulkan physical devices, Vulkan hooks skipped");
			DestroyInstance(rState);
			return false;
		}

		if (!rState.stCreateSwapchain.szName)
			SetupGroups(rState);
		CaptureTargets(rState);
		if (!rState.stQueuePresent.uTargets || !rState.stCreateSwapchain.uTargets || !rState.stDestroySwapchain.uTargets)
		{
			Log(ELogLevel::Warning, "could not resolve the Vulkan swap chain functions, Vulkan hooks skipped");
			DestroyInstance(rState);
			return false;
		}

		bool bHooked = rState.clCreateDevice.IsInstalled() || rState.clCreateDevice.Install(pCreateDevice, reinterpret_cast<void*>(&HookCreateDevice));
		for (const auto& [hDevice, hPhysical] : rState.mapEarlyDevices)
			if (const std::size_t uPhysical = FindPhysical(rState, hPhysical); uPhysical != SIZE_MAX)
				rState.mapDevices[hDevice] = uPhysical;
		rState.mapEarlyDevices.clear();
		if (pCreateWin32Surface)
			bHooked = rState.clCreateWin32Surface.Install(pCreateWin32Surface, reinterpret_cast<void*>(&HookCreateWin32Surface)) && bHooked;
		std::size_t uHooks = 0;
		ForEachGroup(rState, [&](stHookGroup& rGroup)
		{
			for (std::size_t i = 0; i < rGroup.uTargets; ++i)
			{
				if (rGroup.aHooks[i].IsInstalled())
					continue;
				if (rGroup.aHooks[i].Install(rGroup.aTargets[i], rGroup.aDetours[i]))
					++uHooks;
				else
					bHooked = false;
			}
		});
		if (!bHooked || !rState.stQueuePresent.aHooks[0].IsInstalled())
		{
			Log(ELogLevel::Error, "failed to hook Vulkan");
			ForEachGroup(rState, [](stHookGroup& rGroup) { for (clInlineHook& rHook : rGroup.aHooks) rHook.Disable(); });
			rState.clCreateDevice.Disable();
			rState.clCreateWin32Surface.Disable();
			UninstallHooks(rState);
			DestroyInstance(rState);
			return false;
		}

		rState.clDetach.Clear();
		rState.bInstalled = true;
		Log(ELogLevel::Debug, "Vulkan hooked: {} device functions across {} physical devices", uHooks, rState.aPhysical.size());
		return true;
	}

	void RequestDetach(std::chrono::milliseconds msTimeout)
	{
		stState& rState = State();
		{
			std::scoped_lock lckRender(rState.mtxRender);
			if (!rState.pRenderer)
				return;
			rState.clDetach.Request();
		}
		static_cast<void>(rState.clDetach.WaitDone(msTimeout));
	}

	bool Disable()
	{
		stState& rState = State();
		bool bRestored = rState.clCreateDevice.Disable();
		bRestored = DepthVulkan::Disable() && bRestored;
		bRestored = rState.clCreateWin32Surface.Disable() && bRestored;
		ForEachGroup(rState, [&](stHookGroup& rGroup)
		{
			for (clInlineHook& rHook : rGroup.aHooks)
				bRestored = rHook.Disable() && bRestored;
		});
		return bRestored;
	}

	void Release(bool bDrained)
	{
		stState& rState = State();
		std::scoped_lock lckRender(rState.mtxRender);
		DestroyRenderer(rState, EDetachReason::Uninstall, VK_NULL_HANDLE);
		rState.clDetach.Clear();
		rState.mapDevices.clear();
		rState.mapSurfaces.clear();
		rState.mapSwapchains.clear();
		rState.setForcedDevices.clear();
		rState.mapQueues.clear();
		rState.mapEarlyDevices.clear();
		DepthVulkan::Release(bDrained);
		if (bDrained)
		{
			UninstallHooks(rState);
			DestroyInstance(rState);
		}
		rState.bInstalled = false;
	}
}

#else

namespace OmegaRenderHook::HookVulkan
{
	void PrepareEarly()
	{
	}

	void NoteDevice(void*, void*)
	{
	}

	bool Install()
	{
		return false;
	}

	bool DisableEarly()
	{
		return true;
	}

	void ReleaseEarly(bool)
	{
	}

	void RequestDetach(std::chrono::milliseconds)
	{
	}

	bool Disable()
	{
		return true;
	}

	void Release(bool)
	{
	}
}

#endif
