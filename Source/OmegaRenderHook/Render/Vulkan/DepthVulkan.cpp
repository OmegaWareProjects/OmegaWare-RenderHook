#include "OmegaRenderHook/Render/Vulkan/DepthVulkan.hpp"

#if OMEGARENDERHOOK_VULKAN

#include "OmegaRenderHook/Core/Log.hpp"
#include "OmegaRenderHook/Hook/HookScope.hpp"
#include "OmegaRenderHook/Hook/InlineHook.hpp"
#include "OmegaRenderHook/Render/DepthCommon.hpp"
#include "OmegaRenderHook/Render/PipelineInternal.hpp"
#include "OmegaRenderHook/Render/Vulkan/Shaders/DepthResolveFragment.hpp"
#include "OmegaRenderHook/Render/Vulkan/Shaders/DepthResolveVertex.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>

#define OMEGARENDERHOOK_VK_DEPTH_FUNCTIONS(X) \
	X(vkCreateImage) \
	X(vkDestroyImage) \
	X(vkCreateImageView) \
	X(vkDestroyImageView) \
	X(vkGetImageMemoryRequirements) \
	X(vkAllocateMemory) \
	X(vkFreeMemory) \
	X(vkBindImageMemory) \
	X(vkCreateSampler) \
	X(vkDestroySampler) \
	X(vkCreateDescriptorSetLayout) \
	X(vkDestroyDescriptorSetLayout) \
	X(vkCreateDescriptorPool) \
	X(vkDestroyDescriptorPool) \
	X(vkAllocateDescriptorSets) \
	X(vkUpdateDescriptorSets) \
	X(vkCreatePipelineLayout) \
	X(vkDestroyPipelineLayout) \
	X(vkCreateShaderModule) \
	X(vkDestroyShaderModule) \
	X(vkCreateGraphicsPipelines) \
	X(vkDestroyPipeline) \
	X(vkCmdPipelineBarrier) \
	X(vkCmdCopyImage) \
	X(vkCmdBindPipeline) \
	X(vkCmdBindDescriptorSets) \
	X(vkCmdPushConstants) \
	X(vkCmdSetViewport) \
	X(vkCmdSetScissor) \
	X(vkCmdDraw)

namespace OmegaRenderHook::DepthVulkan
{
	namespace
	{
		enum EHook : std::size_t
		{
			kCreateImage,
			kDestroyImage,
			kCreateImageView,
			kDestroyImageView,
			kCreateRenderPass,
			kCreateRenderPass2,
			kCreateRenderPass2KHR,
			kDestroyRenderPass,
			kCreateFramebuffer,
			kDestroyFramebuffer,
			kCmdBeginRenderPass,
			kCmdBeginRenderPass2,
			kCmdBeginRenderPass2KHR,
			kCmdEndRenderPass,
			kCmdEndRenderPass2,
			kCmdEndRenderPass2KHR,
			kCmdBeginRendering,
			kCmdBeginRenderingKHR,
			kCmdEndRendering,
			kCmdEndRenderingKHR,
			kCmdPipelineBarrier,
			kCmdPipelineBarrier2,
			kCmdPipelineBarrier2KHR,
			kHookCount
		};

		constexpr std::uint64_t uRetireFrames = 8;

		struct stFunctions
		{
#define OMEGARENDERHOOK_VK_DECLARE(Name) PFN_##Name Name = nullptr;
			OMEGARENDERHOOK_VK_DEPTH_FUNCTIONS(OMEGARENDERHOOK_VK_DECLARE)
#undef OMEGARENDERHOOK_VK_DECLARE
		};

		struct stImageInfo
		{
			VkFormat eFormat = VK_FORMAT_UNDEFINED;
			VkExtent3D stExtent{};
			VkSampleCountFlagBits eSamples = VK_SAMPLE_COUNT_1_BIT;
			std::uint32_t uLayers = 1;
			VkImageUsageFlags eUsage = 0;
		};

		struct stPassInfo
		{
			std::uint32_t uDepth = UINT32_MAX;
			VkAttachmentLoadOp eLoad = VK_ATTACHMENT_LOAD_OP_LOAD;
			VkAttachmentStoreOp eStore = VK_ATTACHMENT_STORE_OP_STORE;
			VkImageLayout eFinal = VK_IMAGE_LAYOUT_UNDEFINED;
		};

		struct stActive
		{
			VkImage hImage = VK_NULL_HANDLE;
			VkImageLayout eLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			bool bStore = false;
		};

		struct stRetiredImage
		{
			std::uint64_t uFrame = 0;
			VkImage hImage = VK_NULL_HANDLE;
			VkImageView hView = VK_NULL_HANDLE;
			VkDeviceMemory hMemory = VK_NULL_HANDLE;
		};

		struct stOwnImage
		{
			VkImage hImage = VK_NULL_HANDLE;
			VkImageView hView = VK_NULL_HANDLE;
			VkDeviceMemory hMemory = VK_NULL_HANDLE;
		};

		struct stState
		{
			std::mutex mtxHooks;
			VkDevice hHookedDevice = VK_NULL_HANDLE;
			std::array<clInlineHook, kHookCount> aHooks;
			PFN_vkGetPhysicalDeviceFormatProperties pfnFormatProperties = nullptr;
			PFN_vkGetPhysicalDeviceMemoryProperties pfnMemoryProperties = nullptr;
			VkPhysicalDevice hHookedPhysical = VK_NULL_HANDLE;

			std::mutex mtxDepth;
			std::unordered_map<VkFormat, VkFormatFeatureFlags> mapFeatures;
			std::unordered_map<VkImage, stImageInfo> mapImages;
			std::unordered_map<VkImageView, VkImage> mapViews;
			std::unordered_map<VkRenderPass, stPassInfo> mapPasses;
			std::unordered_map<VkFramebuffer, std::vector<VkImageView>> mapFramebuffers;
			std::unordered_map<VkCommandBuffer, stActive> mapActive;

			std::unordered_map<VkImage, std::uint32_t> mapScores;
			VkImage hBest = VK_NULL_HANDLE;
			std::uint32_t uBestScore = 0;
			VkImage hChosen = VK_NULL_HANDLE;
			stImageInfo stChosen;
			VkImageLayout eChosenLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			bool bReversedKnown = false;
			bool bReversedZ = false;
			int iReversedVotes = 0;
			bool bCaptured = false;
			DepthCommon::stRect stArea;
			std::uint64_t uFrame = 0;

			bool bAttached = false;
			VkDevice hDevice = VK_NULL_HANDLE;
			VkPhysicalDevice hPhysical = VK_NULL_HANDLE;
			stFunctions stVk;
			VkExtent2D stExtent{};
			std::uint32_t uFrames = 0;
			stOwnImage stTarget;
			stOwnImage stSnapshot;
			bool bSnapshotFresh = false;
			std::vector<stRetiredImage> aRetired;
			VkSampler hSampler = VK_NULL_HANDLE;
			VkDescriptorSetLayout hSetLayout = VK_NULL_HANDLE;
			VkDescriptorPool hPool = VK_NULL_HANDLE;
			std::vector<VkDescriptorSet> aSets;
			VkPipelineLayout hLayout = VK_NULL_HANDLE;
			VkShaderModule hVertex = VK_NULL_HANDLE;
			VkShaderModule hFragment = VK_NULL_HANDLE;
			VkPipeline hPipeline = VK_NULL_HANDLE;
			DepthCommon::stResolveConstants stConstants;
			DepthCommon::clSwitchVote clVote;
		};

		stState& State()
		{
			static stState* s_pState = new stState();
			return *s_pState;
		}

		template <typename T>
		[[nodiscard]] T Original(EHook eHook)
		{
			return State().aHooks[eHook].Original<T>();
		}

		[[nodiscard]] bool IsOurs()
		{
			return PipelineInternal::clOwnWork::IsActive();
		}

		[[nodiscard]] bool IsDepthFormat(VkFormat eFormat)
		{
			switch (eFormat)
			{
			case VK_FORMAT_D16_UNORM:
			case VK_FORMAT_X8_D24_UNORM_PACK32:
			case VK_FORMAT_D32_SFLOAT:
			case VK_FORMAT_D16_UNORM_S8_UINT:
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return true;
			default:
				return false;
			}
		}

		[[nodiscard]] VkImageAspectFlags FullAspect(VkFormat eFormat)
		{
			switch (eFormat)
			{
			case VK_FORMAT_D16_UNORM_S8_UINT:
			case VK_FORMAT_D24_UNORM_S8_UINT:
			case VK_FORMAT_D32_SFLOAT_S8_UINT:
				return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			default:
				return VK_IMAGE_ASPECT_DEPTH_BIT;
			}
		}

		[[nodiscard]] VkFormatFeatureFlags Features(stState& rState, VkFormat eFormat)
		{
			if (const auto itFeatures = rState.mapFeatures.find(eFormat); itFeatures != rState.mapFeatures.end())
				return itFeatures->second;
			VkFormatProperties stProperties{};
			if (rState.pfnFormatProperties && rState.hHookedPhysical)
				rState.pfnFormatProperties(rState.hHookedPhysical, eFormat, &stProperties);
			return rState.mapFeatures[eFormat] = stProperties.optimalTilingFeatures;
		}

		void NoteDepthUse(stState& rState, VkImage hImage)
		{
			if (!hImage || !rState.stExtent.width)
				return;
			auto [itScore, bInserted] = rState.mapScores.try_emplace(hImage, 0);
			if (bInserted)
			{
				const auto itImage = rState.mapImages.find(hImage);
				const bool bQualifies = itImage != rState.mapImages.end() && itImage->second.uLayers == 1 && (itImage->second.eUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
					DepthCommon::Qualifies(itImage->second.stExtent.width, itImage->second.stExtent.height, itImage->second.eSamples, rState.stExtent.width, rState.stExtent.height);
				if (!bQualifies)
					itScore->second = UINT32_MAX;
			}
			if (itScore->second == UINT32_MAX)
				return;
			if (++itScore->second > rState.uBestScore)
			{
				rState.uBestScore = itScore->second;
				rState.hBest = hImage;
			}
		}

		void NoteClear(stState& rState, VkImage hImage, float flDepth)
		{
			if (hImage && hImage == rState.hChosen)
			{
				rState.bReversedKnown = true;
				rState.bReversedZ = flDepth < .5f;
				return;
			}
			rState.iReversedVotes = std::clamp(rState.iReversedVotes + (flDepth < .5f ? 1 : -1), -64, 64);
		}

		[[nodiscard]] VkImageMemoryBarrier ImageBarrier(VkImage hImage, VkImageAspectFlags eAspect, VkImageLayout eOld, VkImageLayout eNew, VkAccessFlags eSource, VkAccessFlags eDest)
		{
			VkImageMemoryBarrier stBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			stBarrier.srcAccessMask = eSource;
			stBarrier.dstAccessMask = eDest;
			stBarrier.oldLayout = eOld;
			stBarrier.newLayout = eNew;
			stBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			stBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			stBarrier.image = hImage;
			stBarrier.subresourceRange = { eAspect, 0, 1, 0, 1 };
			return stBarrier;
		}

		void RecordCopy(stState& rState, VkCommandBuffer hCommand, VkImageLayout eLayout)
		{
			const auto pfnBarrier = rState.aHooks[kCmdPipelineBarrier].IsInstalled() ? Original<PFN_vkCmdPipelineBarrier>(kCmdPipelineBarrier) : rState.stVk.vkCmdPipelineBarrier;
			const VkImageAspectFlags eAspect = FullAspect(rState.stChosen.eFormat);
			const std::array<VkImageMemoryBarrier, 2> aBefore{
				ImageBarrier(rState.hChosen, eAspect, eLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
				ImageBarrier(rState.stSnapshot.hImage, eAspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT)
			};
			pfnBarrier(hCommand, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, static_cast<std::uint32_t>(aBefore.size()), aBefore.data());
			VkImageCopy stRegion{};
			stRegion.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
			stRegion.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
			stRegion.extent = { rState.stChosen.stExtent.width, rState.stChosen.stExtent.height, 1 };
			rState.stVk.vkCmdCopyImage(hCommand, rState.hChosen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rState.stSnapshot.hImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &stRegion);
			const VkImageMemoryBarrier stAfter = ImageBarrier(rState.hChosen, eAspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, eLayout, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
			pfnBarrier(hCommand, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &stAfter);
		}

		void BeginDepth(VkCommandBuffer hCommand, VkImageView hView, VkImageLayout eLayout, bool bStore, const VkRect2D& stArea, const VkClearValue* pClear)
		{
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			const auto itView = hView ? rState.mapViews.find(hView) : rState.mapViews.end();
			const VkImage hImage = itView == rState.mapViews.end() ? VK_NULL_HANDLE : itView->second;
			if (!hImage)
			{
				rState.mapActive.erase(hCommand);
				return;
			}
			rState.mapActive[hCommand] = { hImage, eLayout, bStore };
			NoteDepthUse(rState, hImage);
			if (pClear)
				NoteClear(rState, hImage, pClear->depthStencil.depth);
			if (hImage == rState.hChosen)
				rState.stArea.Include(static_cast<float>(stArea.offset.x), static_cast<float>(stArea.offset.y), static_cast<float>(stArea.extent.width), static_cast<float>(stArea.extent.height));
		}

		void EndDepth(VkCommandBuffer hCommand)
		{
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			const auto itActive = rState.mapActive.find(hCommand);
			if (itActive == rState.mapActive.end())
				return;
			const stActive stDepth = itActive->second;
			rState.mapActive.erase(itActive);
			if (stDepth.hImage != rState.hChosen || !stDepth.bStore || !rState.stSnapshot.hImage || !rState.bAttached || stDepth.eLayout == VK_IMAGE_LAYOUT_UNDEFINED)
				return;
			RecordCopy(rState, hCommand, stDepth.eLayout);
			rState.eChosenLayout = stDepth.eLayout;
			rState.bCaptured = true;
		}

		void BeginRenderPass(VkCommandBuffer hCommand, const VkRenderPassBeginInfo* pInfo)
		{
			if (IsOurs() || !pInfo)
				return;
			stState& rState = State();
			VkImageView hView = VK_NULL_HANDLE;
			stPassInfo stPass;
			{
				std::scoped_lock lckDepth(rState.mtxDepth);
				const auto itPass = rState.mapPasses.find(pInfo->renderPass);
				if (itPass == rState.mapPasses.end() || itPass->second.uDepth == UINT32_MAX)
				{
					rState.mapActive.erase(hCommand);
					return;
				}
				stPass = itPass->second;
				for (auto* pNext = static_cast<const VkBaseInStructure*>(pInfo->pNext); pNext; pNext = pNext->pNext)
				{
					if (pNext->sType != VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
						continue;
					const auto* pAttachments = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(pNext);
					if (stPass.uDepth < pAttachments->attachmentCount)
						hView = pAttachments->pAttachments[stPass.uDepth];
				}
				if (!hView)
				{
					const auto itFramebuffer = rState.mapFramebuffers.find(pInfo->framebuffer);
					if (itFramebuffer != rState.mapFramebuffers.end() && stPass.uDepth < itFramebuffer->second.size())
						hView = itFramebuffer->second[stPass.uDepth];
				}
			}
			const VkClearValue* pClear = stPass.eLoad == VK_ATTACHMENT_LOAD_OP_CLEAR && pInfo->pClearValues && stPass.uDepth < pInfo->clearValueCount ? &pInfo->pClearValues[stPass.uDepth] : nullptr;
			BeginDepth(hCommand, hView, stPass.eFinal, stPass.eStore == VK_ATTACHMENT_STORE_OP_STORE, pInfo->renderArea, pClear);
		}

		VkResult VKAPI_CALL HookCreateImage(VkDevice hDevice, const VkImageCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage)
		{
			clHookScope clScope;
			stState& rState = State();
			const auto pfnOriginal = Original<PFN_vkCreateImage>(kCreateImage);
			if (IsOurs() || !pInfo || !pImage || pInfo->imageType != VK_IMAGE_TYPE_2D || !IsDepthFormat(pInfo->format) || !(pInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
				return pfnOriginal(hDevice, pInfo, pAllocator, pImage);
			VkImageCreateInfo stInfo = *pInfo;
			{
				std::scoped_lock lckDepth(rState.mtxDepth);
				if (pInfo->samples == VK_SAMPLE_COUNT_1_BIT && (Features(rState, pInfo->format) & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
					stInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			}
			VkResult eResult = pfnOriginal(hDevice, &stInfo, pAllocator, pImage);
			if (eResult != VK_SUCCESS && stInfo.usage != pInfo->usage)
			{
				stInfo.usage = pInfo->usage;
				eResult = pfnOriginal(hDevice, &stInfo, pAllocator, pImage);
			}
			if (eResult == VK_SUCCESS)
			{
				std::scoped_lock lckDepth(rState.mtxDepth);
				rState.mapImages[*pImage] = { stInfo.format, stInfo.extent, stInfo.samples, stInfo.arrayLayers, stInfo.usage };
			}
			return eResult;
		}

		void VKAPI_CALL HookDestroyImage(VkDevice hDevice, VkImage hImage, const VkAllocationCallbacks* pAllocator)
		{
			clHookScope clScope;
			if (hImage && !IsOurs())
			{
				stState& rState = State();
				std::scoped_lock lckDepth(rState.mtxDepth);
				rState.mapImages.erase(hImage);
				rState.mapScores.erase(hImage);
				if (rState.hBest == hImage)
				{
					rState.hBest = VK_NULL_HANDLE;
					rState.uBestScore = 0;
				}
				if (rState.hChosen == hImage)
				{
					rState.hChosen = VK_NULL_HANDLE;
					rState.bCaptured = false;
				}
			}
			Original<PFN_vkDestroyImage>(kDestroyImage)(hDevice, hImage, pAllocator);
		}

		VkResult VKAPI_CALL HookCreateImageView(VkDevice hDevice, const VkImageViewCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkImageView* pView)
		{
			clHookScope clScope;
			const VkResult eResult = Original<PFN_vkCreateImageView>(kCreateImageView)(hDevice, pInfo, pAllocator, pView);
			if (eResult == VK_SUCCESS && pInfo && pView && !IsOurs())
			{
				stState& rState = State();
				std::scoped_lock lckDepth(rState.mtxDepth);
				if (rState.mapImages.contains(pInfo->image))
					rState.mapViews[*pView] = pInfo->image;
			}
			return eResult;
		}

		void VKAPI_CALL HookDestroyImageView(VkDevice hDevice, VkImageView hView, const VkAllocationCallbacks* pAllocator)
		{
			clHookScope clScope;
			if (hView && !IsOurs())
			{
				stState& rState = State();
				std::scoped_lock lckDepth(rState.mtxDepth);
				rState.mapViews.erase(hView);
			}
			Original<PFN_vkDestroyImageView>(kDestroyImageView)(hDevice, hView, pAllocator);
		}

		template <typename TAttachment, typename TSubpass>
		[[nodiscard]] stPassInfo DescribePass(std::uint32_t uAttachments, TAttachment* pAttachments, std::uint32_t uSubpasses, const TSubpass* pSubpasses)
		{
			stPassInfo stPass;
			for (std::uint32_t i = 0; i < uSubpasses && stPass.uDepth == UINT32_MAX; ++i)
				if (pSubpasses[i].pDepthStencilAttachment && pSubpasses[i].pDepthStencilAttachment->attachment < uAttachments)
					stPass.uDepth = pSubpasses[i].pDepthStencilAttachment->attachment;
			if (stPass.uDepth == UINT32_MAX || !IsDepthFormat(pAttachments[stPass.uDepth].format))
				return {};
			TAttachment& rDepth = pAttachments[stPass.uDepth];
			if (rDepth.samples == VK_SAMPLE_COUNT_1_BIT && rDepth.storeOp != VK_ATTACHMENT_STORE_OP_STORE)
				rDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			stPass.eLoad = rDepth.loadOp;
			stPass.eStore = rDepth.storeOp;
			stPass.eFinal = rDepth.finalLayout;
			return stPass;
		}

		void NotePass(VkRenderPass hPass, const stPassInfo& stPass)
		{
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (stPass.uDepth == UINT32_MAX)
				rState.mapPasses.erase(hPass);
			else
				rState.mapPasses[hPass] = stPass;
		}

		VkResult VKAPI_CALL HookCreateRenderPass(VkDevice hDevice, const VkRenderPassCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pPass)
		{
			clHookScope clScope;
			const auto pfnOriginal = Original<PFN_vkCreateRenderPass>(kCreateRenderPass);
			if (IsOurs() || !pInfo || !pPass || !pInfo->pAttachments)
				return pfnOriginal(hDevice, pInfo, pAllocator, pPass);
			std::vector<VkAttachmentDescription> aAttachments(pInfo->pAttachments, pInfo->pAttachments + pInfo->attachmentCount);
			const stPassInfo stPass = DescribePass(pInfo->attachmentCount, aAttachments.data(), pInfo->subpassCount, pInfo->pSubpasses);
			VkRenderPassCreateInfo stInfo = *pInfo;
			stInfo.pAttachments = aAttachments.data();
			const VkResult eResult = pfnOriginal(hDevice, &stInfo, pAllocator, pPass);
			if (eResult == VK_SUCCESS)
				NotePass(*pPass, stPass);
			return eResult;
		}

		template <EHook eHook>
		VkResult VKAPI_CALL HookCreateRenderPass2(VkDevice hDevice, const VkRenderPassCreateInfo2* pInfo, const VkAllocationCallbacks* pAllocator, VkRenderPass* pPass)
		{
			clHookScope clScope;
			const auto pfnOriginal = Original<PFN_vkCreateRenderPass2>(eHook);
			if (IsOurs() || !pInfo || !pPass || !pInfo->pAttachments)
				return pfnOriginal(hDevice, pInfo, pAllocator, pPass);
			std::vector<VkAttachmentDescription2> aAttachments(pInfo->pAttachments, pInfo->pAttachments + pInfo->attachmentCount);
			const stPassInfo stPass = DescribePass(pInfo->attachmentCount, aAttachments.data(), pInfo->subpassCount, pInfo->pSubpasses);
			VkRenderPassCreateInfo2 stInfo = *pInfo;
			stInfo.pAttachments = aAttachments.data();
			const VkResult eResult = pfnOriginal(hDevice, &stInfo, pAllocator, pPass);
			if (eResult == VK_SUCCESS)
				NotePass(*pPass, stPass);
			return eResult;
		}

		void VKAPI_CALL HookDestroyRenderPass(VkDevice hDevice, VkRenderPass hPass, const VkAllocationCallbacks* pAllocator)
		{
			clHookScope clScope;
			if (hPass && !IsOurs())
			{
				stState& rState = State();
				std::scoped_lock lckDepth(rState.mtxDepth);
				rState.mapPasses.erase(hPass);
			}
			Original<PFN_vkDestroyRenderPass>(kDestroyRenderPass)(hDevice, hPass, pAllocator);
		}

		VkResult VKAPI_CALL HookCreateFramebuffer(VkDevice hDevice, const VkFramebufferCreateInfo* pInfo, const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer)
		{
			clHookScope clScope;
			const VkResult eResult = Original<PFN_vkCreateFramebuffer>(kCreateFramebuffer)(hDevice, pInfo, pAllocator, pFramebuffer);
			if (eResult == VK_SUCCESS && pInfo && pFramebuffer && !IsOurs() && !(pInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) && pInfo->pAttachments)
			{
				stState& rState = State();
				std::scoped_lock lckDepth(rState.mtxDepth);
				rState.mapFramebuffers[*pFramebuffer].assign(pInfo->pAttachments, pInfo->pAttachments + pInfo->attachmentCount);
			}
			return eResult;
		}

		void VKAPI_CALL HookDestroyFramebuffer(VkDevice hDevice, VkFramebuffer hFramebuffer, const VkAllocationCallbacks* pAllocator)
		{
			clHookScope clScope;
			if (hFramebuffer && !IsOurs())
			{
				stState& rState = State();
				std::scoped_lock lckDepth(rState.mtxDepth);
				rState.mapFramebuffers.erase(hFramebuffer);
			}
			Original<PFN_vkDestroyFramebuffer>(kDestroyFramebuffer)(hDevice, hFramebuffer, pAllocator);
		}

		void VKAPI_CALL HookCmdBeginRenderPass(VkCommandBuffer hCommand, const VkRenderPassBeginInfo* pInfo, VkSubpassContents eContents)
		{
			clHookScope clScope;
			BeginRenderPass(hCommand, pInfo);
			Original<PFN_vkCmdBeginRenderPass>(kCmdBeginRenderPass)(hCommand, pInfo, eContents);
		}

		template <EHook eHook>
		void VKAPI_CALL HookCmdBeginRenderPass2(VkCommandBuffer hCommand, const VkRenderPassBeginInfo* pInfo, const VkSubpassBeginInfo* pSubpass)
		{
			clHookScope clScope;
			BeginRenderPass(hCommand, pInfo);
			Original<PFN_vkCmdBeginRenderPass2>(eHook)(hCommand, pInfo, pSubpass);
		}

		void VKAPI_CALL HookCmdEndRenderPass(VkCommandBuffer hCommand)
		{
			clHookScope clScope;
			Original<PFN_vkCmdEndRenderPass>(kCmdEndRenderPass)(hCommand);
			if (!IsOurs())
				EndDepth(hCommand);
		}

		template <EHook eHook>
		void VKAPI_CALL HookCmdEndRenderPass2(VkCommandBuffer hCommand, const VkSubpassEndInfo* pInfo)
		{
			clHookScope clScope;
			Original<PFN_vkCmdEndRenderPass2>(eHook)(hCommand, pInfo);
			if (!IsOurs())
				EndDepth(hCommand);
		}

		template <EHook eHook>
		void VKAPI_CALL HookCmdBeginRendering(VkCommandBuffer hCommand, const VkRenderingInfo* pInfo)
		{
			clHookScope clScope;
			const auto pfnOriginal = Original<PFN_vkCmdBeginRendering>(eHook);
			if (IsOurs() || !pInfo || !pInfo->pDepthAttachment || !pInfo->pDepthAttachment->imageView)
			{
				if (!IsOurs())
				{
					stState& rState = State();
					std::scoped_lock lckDepth(rState.mtxDepth);
					rState.mapActive.erase(hCommand);
				}
				return pfnOriginal(hCommand, pInfo);
			}
			VkRenderingAttachmentInfo stDepth = *pInfo->pDepthAttachment;
			if (stDepth.resolveMode == VK_RESOLVE_MODE_NONE && stDepth.storeOp != VK_ATTACHMENT_STORE_OP_STORE)
				stDepth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			VkRenderingInfo stInfo = *pInfo;
			stInfo.pDepthAttachment = &stDepth;
			const bool bResuming = pInfo->flags & VK_RENDERING_RESUMING_BIT;
			if (!bResuming)
				BeginDepth(hCommand, stDepth.imageView, stDepth.imageLayout, stDepth.storeOp == VK_ATTACHMENT_STORE_OP_STORE, pInfo->renderArea, stDepth.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? &stDepth.clearValue : nullptr);
			pfnOriginal(hCommand, &stInfo);
		}

		template <EHook eHook>
		void VKAPI_CALL HookCmdEndRendering(VkCommandBuffer hCommand)
		{
			clHookScope clScope;
			Original<PFN_vkCmdEndRendering>(eHook)(hCommand);
			if (!IsOurs())
				EndDepth(hCommand);
		}

		void NoteLayout(VkImage hImage, VkImageLayout eLayout)
		{
			stState& rState = State();
			std::scoped_lock lckDepth(rState.mtxDepth);
			if (hImage == rState.hChosen)
				rState.eChosenLayout = eLayout;
		}

		void VKAPI_CALL HookCmdPipelineBarrier(VkCommandBuffer hCommand, VkPipelineStageFlags eSource, VkPipelineStageFlags eDest, VkDependencyFlags eFlags, std::uint32_t uMemory, const VkMemoryBarrier* pMemory, std::uint32_t uBuffers, const VkBufferMemoryBarrier* pBuffers, std::uint32_t uImages, const VkImageMemoryBarrier* pImages)
		{
			clHookScope clScope;
			Original<PFN_vkCmdPipelineBarrier>(kCmdPipelineBarrier)(hCommand, eSource, eDest, eFlags, uMemory, pMemory, uBuffers, pBuffers, uImages, pImages);
			if (IsOurs() || !pImages)
				return;
			const VkImage hChosen = State().hChosen;
			for (std::uint32_t i = 0; i < uImages; ++i)
				if (hChosen && pImages[i].image == hChosen)
					NoteLayout(pImages[i].image, pImages[i].newLayout);
		}

		template <EHook eHook>
		void VKAPI_CALL HookCmdPipelineBarrier2(VkCommandBuffer hCommand, const VkDependencyInfo* pInfo)
		{
			clHookScope clScope;
			Original<PFN_vkCmdPipelineBarrier2>(eHook)(hCommand, pInfo);
			if (IsOurs() || !pInfo || !pInfo->pImageMemoryBarriers)
				return;
			const VkImage hChosen = State().hChosen;
			for (std::uint32_t i = 0; i < pInfo->imageMemoryBarrierCount; ++i)
				if (hChosen && pInfo->pImageMemoryBarriers[i].image == hChosen)
					NoteLayout(pInfo->pImageMemoryBarriers[i].image, pInfo->pImageMemoryBarriers[i].newLayout);
		}

		struct stHookTarget
		{
			EHook eHook;
			const char* szName;
			void* pDetour;
		};

		[[nodiscard]] std::uint32_t FindMemoryType(stState& rState, std::uint32_t uBits, VkMemoryPropertyFlags eFlags)
		{
			VkPhysicalDeviceMemoryProperties stMemory{};
			if (rState.pfnMemoryProperties)
				rState.pfnMemoryProperties(rState.hPhysical, &stMemory);
			for (std::uint32_t i = 0; i < stMemory.memoryTypeCount; ++i)
				if ((uBits & (1u << i)) && (stMemory.memoryTypes[i].propertyFlags & eFlags) == eFlags)
					return i;
			return UINT32_MAX;
		}

		void DestroyImage(stState& rState, stOwnImage& rImage)
		{
			if (rImage.hView)
				rState.stVk.vkDestroyImageView(rState.hDevice, rImage.hView, nullptr);
			if (rImage.hImage)
				rState.stVk.vkDestroyImage(rState.hDevice, rImage.hImage, nullptr);
			if (rImage.hMemory)
				rState.stVk.vkFreeMemory(rState.hDevice, rImage.hMemory, nullptr);
			rImage = {};
		}

		[[nodiscard]] bool CreateImage(stState& rState, stOwnImage& rImage, VkFormat eFormat, VkExtent2D stExtent, VkImageUsageFlags eUsage)
		{
			PipelineInternal::clOwnWork clOwn;
			VkImageCreateInfo stInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			stInfo.imageType = VK_IMAGE_TYPE_2D;
			stInfo.format = eFormat;
			stInfo.extent = { stExtent.width, stExtent.height, 1 };
			stInfo.mipLevels = 1;
			stInfo.arrayLayers = 1;
			stInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			stInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			stInfo.usage = eUsage;
			if (rState.stVk.vkCreateImage(rState.hDevice, &stInfo, nullptr, &rImage.hImage) != VK_SUCCESS)
				return false;
			VkMemoryRequirements stRequirements{};
			rState.stVk.vkGetImageMemoryRequirements(rState.hDevice, rImage.hImage, &stRequirements);
			VkMemoryAllocateInfo stAllocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
			stAllocate.allocationSize = stRequirements.size;
			stAllocate.memoryTypeIndex = FindMemoryType(rState, stRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (stAllocate.memoryTypeIndex == UINT32_MAX || rState.stVk.vkAllocateMemory(rState.hDevice, &stAllocate, nullptr, &rImage.hMemory) != VK_SUCCESS ||
				rState.stVk.vkBindImageMemory(rState.hDevice, rImage.hImage, rImage.hMemory, 0) != VK_SUCCESS)
			{
				DestroyImage(rState, rImage);
				return false;
			}
			VkImageViewCreateInfo stView{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
			stView.image = rImage.hImage;
			stView.viewType = VK_IMAGE_VIEW_TYPE_2D;
			stView.format = eFormat;
			stView.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
			if (rState.stVk.vkCreateImageView(rState.hDevice, &stView, nullptr, &rImage.hView) != VK_SUCCESS)
			{
				DestroyImage(rState, rImage);
				return false;
			}
			return true;
		}

		void RetireSnapshot(stState& rState)
		{
			if (rState.stSnapshot.hImage)
				rState.aRetired.push_back({ rState.uFrame, rState.stSnapshot.hImage, rState.stSnapshot.hView, rState.stSnapshot.hMemory });
			rState.stSnapshot = {};
			rState.bSnapshotFresh = false;
		}

		void DestroyRetired(stState& rState, bool bAll)
		{
			std::erase_if(rState.aRetired, [&](const stRetiredImage& rRetired)
			{
				if (!bAll && rState.uFrame - rRetired.uFrame <= uRetireFrames)
					return false;
				stOwnImage stImage{ rRetired.hImage, rRetired.hView, rRetired.hMemory };
				DestroyImage(rState, stImage);
				return true;
			});
		}

		[[nodiscard]] bool CreatePipeline(stState& rState, VkRenderPass hRenderPass)
		{
			PipelineInternal::clOwnWork clOwn;
			const stFunctions& rVk = rState.stVk;
			VkSamplerCreateInfo stSampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
			stSampler.magFilter = VK_FILTER_NEAREST;
			stSampler.minFilter = VK_FILTER_NEAREST;
			stSampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			stSampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			stSampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			if (rVk.vkCreateSampler(rState.hDevice, &stSampler, nullptr, &rState.hSampler) != VK_SUCCESS)
				return false;
			VkDescriptorSetLayoutBinding stBinding{};
			stBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			stBinding.descriptorCount = 1;
			stBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			VkDescriptorSetLayoutCreateInfo stSetLayout{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
			stSetLayout.bindingCount = 1;
			stSetLayout.pBindings = &stBinding;
			if (rVk.vkCreateDescriptorSetLayout(rState.hDevice, &stSetLayout, nullptr, &rState.hSetLayout) != VK_SUCCESS)
				return false;
			const std::uint32_t uSets = std::max<std::uint32_t>(rState.uFrames, 1);
			const VkDescriptorPoolSize stPoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uSets };
			VkDescriptorPoolCreateInfo stPool{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
			stPool.maxSets = uSets;
			stPool.poolSizeCount = 1;
			stPool.pPoolSizes = &stPoolSize;
			if (rVk.vkCreateDescriptorPool(rState.hDevice, &stPool, nullptr, &rState.hPool) != VK_SUCCESS)
				return false;
			const std::vector<VkDescriptorSetLayout> aLayouts(uSets, rState.hSetLayout);
			rState.aSets.resize(uSets);
			VkDescriptorSetAllocateInfo stAllocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
			stAllocate.descriptorPool = rState.hPool;
			stAllocate.descriptorSetCount = uSets;
			stAllocate.pSetLayouts = aLayouts.data();
			if (rVk.vkAllocateDescriptorSets(rState.hDevice, &stAllocate, rState.aSets.data()) != VK_SUCCESS)
				return false;
			const VkPushConstantRange stPush{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DepthCommon::stResolveConstants) };
			VkPipelineLayoutCreateInfo stLayout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
			stLayout.setLayoutCount = 1;
			stLayout.pSetLayouts = &rState.hSetLayout;
			stLayout.pushConstantRangeCount = 1;
			stLayout.pPushConstantRanges = &stPush;
			if (rVk.vkCreatePipelineLayout(rState.hDevice, &stLayout, nullptr, &rState.hLayout) != VK_SUCCESS)
				return false;
			VkShaderModuleCreateInfo stModule{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			stModule.codeSize = sizeof(g_aDepthResolveVertexSPIRV);
			stModule.pCode = g_aDepthResolveVertexSPIRV;
			if (rVk.vkCreateShaderModule(rState.hDevice, &stModule, nullptr, &rState.hVertex) != VK_SUCCESS)
				return false;
			stModule.codeSize = sizeof(g_aDepthResolveFragmentSPIRV);
			stModule.pCode = g_aDepthResolveFragmentSPIRV;
			if (rVk.vkCreateShaderModule(rState.hDevice, &stModule, nullptr, &rState.hFragment) != VK_SUCCESS)
				return false;

			std::array<VkPipelineShaderStageCreateInfo, 2> aStages{};
			aStages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, rState.hVertex, "main", nullptr };
			aStages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, rState.hFragment, "main", nullptr };
			const VkPipelineVertexInputStateCreateInfo stVertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
			VkPipelineInputAssemblyStateCreateInfo stAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
			stAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkPipelineViewportStateCreateInfo stViewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
			stViewport.viewportCount = 1;
			stViewport.scissorCount = 1;
			VkPipelineRasterizationStateCreateInfo stRaster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
			stRaster.polygonMode = VK_POLYGON_MODE_FILL;
			stRaster.cullMode = VK_CULL_MODE_NONE;
			stRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
			stRaster.lineWidth = 1.f;
			VkPipelineMultisampleStateCreateInfo stMultisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
			stMultisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo stDepth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
			stDepth.depthTestEnable = VK_TRUE;
			stDepth.depthWriteEnable = VK_TRUE;
			stDepth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
			VkPipelineColorBlendAttachmentState stAttachment{};
			VkPipelineColorBlendStateCreateInfo stBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
			stBlend.attachmentCount = 1;
			stBlend.pAttachments = &stAttachment;
			const std::array<VkDynamicState, 2> aDynamic{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
			VkPipelineDynamicStateCreateInfo stDynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
			stDynamic.dynamicStateCount = static_cast<std::uint32_t>(aDynamic.size());
			stDynamic.pDynamicStates = aDynamic.data();
			VkGraphicsPipelineCreateInfo stPipeline{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
			stPipeline.stageCount = static_cast<std::uint32_t>(aStages.size());
			stPipeline.pStages = aStages.data();
			stPipeline.pVertexInputState = &stVertexInput;
			stPipeline.pInputAssemblyState = &stAssembly;
			stPipeline.pViewportState = &stViewport;
			stPipeline.pRasterizationState = &stRaster;
			stPipeline.pMultisampleState = &stMultisample;
			stPipeline.pDepthStencilState = &stDepth;
			stPipeline.pColorBlendState = &stBlend;
			stPipeline.pDynamicState = &stDynamic;
			stPipeline.layout = rState.hLayout;
			stPipeline.renderPass = hRenderPass;
			return rVk.vkCreateGraphicsPipelines(rState.hDevice, VK_NULL_HANDLE, 1, &stPipeline, nullptr, &rState.hPipeline) == VK_SUCCESS;
		}

		void DestroyObjects(stState& rState)
		{
			if (!rState.hDevice || !rState.stVk.vkDestroyImage)
				return;
			PipelineInternal::clOwnWork clOwn;
			const stFunctions& rVk = rState.stVk;
			DestroyImage(rState, rState.stTarget);
			RetireSnapshot(rState);
			DestroyRetired(rState, true);
			if (rState.hPipeline)
				rVk.vkDestroyPipeline(rState.hDevice, rState.hPipeline, nullptr);
			if (rState.hVertex)
				rVk.vkDestroyShaderModule(rState.hDevice, rState.hVertex, nullptr);
			if (rState.hFragment)
				rVk.vkDestroyShaderModule(rState.hDevice, rState.hFragment, nullptr);
			if (rState.hLayout)
				rVk.vkDestroyPipelineLayout(rState.hDevice, rState.hLayout, nullptr);
			if (rState.hPool)
				rVk.vkDestroyDescriptorPool(rState.hDevice, rState.hPool, nullptr);
			if (rState.hSetLayout)
				rVk.vkDestroyDescriptorSetLayout(rState.hDevice, rState.hSetLayout, nullptr);
			if (rState.hSampler)
				rVk.vkDestroySampler(rState.hDevice, rState.hSampler, nullptr);
			rState.hPipeline = VK_NULL_HANDLE;
			rState.hVertex = VK_NULL_HANDLE;
			rState.hFragment = VK_NULL_HANDLE;
			rState.hLayout = VK_NULL_HANDLE;
			rState.hPool = VK_NULL_HANDLE;
			rState.hSetLayout = VK_NULL_HANDLE;
			rState.hSampler = VK_NULL_HANDLE;
			rState.aSets.clear();
		}

		void ResetSelection(stState& rState)
		{
			rState.clVote.Reset();
			rState.mapScores.clear();
			rState.hBest = VK_NULL_HANDLE;
			rState.uBestScore = 0;
			rState.hChosen = VK_NULL_HANDLE;
			rState.stChosen = {};
			rState.eChosenLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			rState.bReversedKnown = false;
			rState.bCaptured = false;
			rState.stArea = {};
		}

		void SelectNext(stState& rState, VkCommandBuffer hCommand)
		{
			const auto itChosen = rState.hChosen ? rState.mapScores.find(rState.hChosen) : rState.mapScores.end();
			const std::uint32_t uChosenScore = itChosen == rState.mapScores.end() || itChosen->second == UINT32_MAX ? 0 : itChosen->second;
			const bool bWins = rState.hBest && rState.hBest != rState.hChosen && DepthCommon::ShouldSwitch(rState.uBestScore, uChosenScore);
			if (rState.clVote.Vote(std::bit_cast<std::uint64_t>(rState.hBest), bWins, rState.hChosen != VK_NULL_HANDLE))
			{
				const auto itImage = rState.mapImages.find(rState.hBest);
				RetireSnapshot(rState);
				rState.hChosen = VK_NULL_HANDLE;
				const VkFormatFeatureFlags eNeeded = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
				if (itImage != rState.mapImages.end() && (Features(rState, itImage->second.eFormat) & eNeeded) == eNeeded &&
					CreateImage(rState, rState.stSnapshot, itImage->second.eFormat, { itImage->second.stExtent.width, itImage->second.stExtent.height }, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
				{
					rState.hChosen = rState.hBest;
					rState.stChosen = itImage->second;
					rState.eChosenLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					rState.bReversedKnown = false;
					const VkImageMemoryBarrier stBarrier = ImageBarrier(rState.stSnapshot.hImage, FullAspect(rState.stChosen.eFormat), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
					rState.stVk.vkCmdPipelineBarrier(hCommand, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &stBarrier);
					Log(ELogLevel::Debug, "Vulkan scene depth is image {} ({}x{}, format {})", reinterpret_cast<void*>(rState.hChosen), rState.stChosen.stExtent.width, rState.stChosen.stExtent.height, static_cast<int>(rState.stChosen.eFormat));
				}
				else
					Log(ELogLevel::Warning, "could not copy the Vulkan depth image, world drawing will not be occluded");
			}
			rState.mapScores.clear();
			rState.hBest = VK_NULL_HANDLE;
			rState.uBestScore = 0;
			rState.bCaptured = false;
			rState.stArea = {};
			++rState.uFrame;
			DestroyRetired(rState, false);
		}
	}

	void OnDeviceCreated(VkDevice hDevice, VkPhysicalDevice hPhysicalDevice)
	{
		stState& rState = State();
		std::scoped_lock lckHooks(rState.mtxHooks);
		if (!hDevice || rState.hHookedDevice)
			return;
		const HMODULE hLoader = GetModuleHandleW(L"vulkan-1.dll");
		const auto pfnGetDeviceProcAddr = hLoader ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(hLoader, "vkGetDeviceProcAddr"))) : nullptr;
		if (!pfnGetDeviceProcAddr)
			return;
		{
			std::scoped_lock lckDepth(rState.mtxDepth);
			rState.pfnFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(reinterpret_cast<void*>(GetProcAddress(hLoader, "vkGetPhysicalDeviceFormatProperties")));
			rState.pfnMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(reinterpret_cast<void*>(GetProcAddress(hLoader, "vkGetPhysicalDeviceMemoryProperties")));
			rState.hHookedPhysical = hPhysicalDevice;
			rState.mapFeatures.clear();
		}
		const std::array<stHookTarget, kHookCount> aTargets{ {
			{ kCreateImage, "vkCreateImage", reinterpret_cast<void*>(&HookCreateImage) },
			{ kDestroyImage, "vkDestroyImage", reinterpret_cast<void*>(&HookDestroyImage) },
			{ kCreateImageView, "vkCreateImageView", reinterpret_cast<void*>(&HookCreateImageView) },
			{ kDestroyImageView, "vkDestroyImageView", reinterpret_cast<void*>(&HookDestroyImageView) },
			{ kCreateRenderPass, "vkCreateRenderPass", reinterpret_cast<void*>(&HookCreateRenderPass) },
			{ kCreateRenderPass2, "vkCreateRenderPass2", reinterpret_cast<void*>(&HookCreateRenderPass2<kCreateRenderPass2>) },
			{ kCreateRenderPass2KHR, "vkCreateRenderPass2KHR", reinterpret_cast<void*>(&HookCreateRenderPass2<kCreateRenderPass2KHR>) },
			{ kDestroyRenderPass, "vkDestroyRenderPass", reinterpret_cast<void*>(&HookDestroyRenderPass) },
			{ kCreateFramebuffer, "vkCreateFramebuffer", reinterpret_cast<void*>(&HookCreateFramebuffer) },
			{ kDestroyFramebuffer, "vkDestroyFramebuffer", reinterpret_cast<void*>(&HookDestroyFramebuffer) },
			{ kCmdBeginRenderPass, "vkCmdBeginRenderPass", reinterpret_cast<void*>(&HookCmdBeginRenderPass) },
			{ kCmdBeginRenderPass2, "vkCmdBeginRenderPass2", reinterpret_cast<void*>(&HookCmdBeginRenderPass2<kCmdBeginRenderPass2>) },
			{ kCmdBeginRenderPass2KHR, "vkCmdBeginRenderPass2KHR", reinterpret_cast<void*>(&HookCmdBeginRenderPass2<kCmdBeginRenderPass2KHR>) },
			{ kCmdEndRenderPass, "vkCmdEndRenderPass", reinterpret_cast<void*>(&HookCmdEndRenderPass) },
			{ kCmdEndRenderPass2, "vkCmdEndRenderPass2", reinterpret_cast<void*>(&HookCmdEndRenderPass2<kCmdEndRenderPass2>) },
			{ kCmdEndRenderPass2KHR, "vkCmdEndRenderPass2KHR", reinterpret_cast<void*>(&HookCmdEndRenderPass2<kCmdEndRenderPass2KHR>) },
			{ kCmdBeginRendering, "vkCmdBeginRendering", reinterpret_cast<void*>(&HookCmdBeginRendering<kCmdBeginRendering>) },
			{ kCmdBeginRenderingKHR, "vkCmdBeginRenderingKHR", reinterpret_cast<void*>(&HookCmdBeginRendering<kCmdBeginRenderingKHR>) },
			{ kCmdEndRendering, "vkCmdEndRendering", reinterpret_cast<void*>(&HookCmdEndRendering<kCmdEndRendering>) },
			{ kCmdEndRenderingKHR, "vkCmdEndRenderingKHR", reinterpret_cast<void*>(&HookCmdEndRendering<kCmdEndRenderingKHR>) },
			{ kCmdPipelineBarrier, "vkCmdPipelineBarrier", reinterpret_cast<void*>(&HookCmdPipelineBarrier) },
			{ kCmdPipelineBarrier2, "vkCmdPipelineBarrier2", reinterpret_cast<void*>(&HookCmdPipelineBarrier2<kCmdPipelineBarrier2>) },
			{ kCmdPipelineBarrier2KHR, "vkCmdPipelineBarrier2KHR", reinterpret_cast<void*>(&HookCmdPipelineBarrier2<kCmdPipelineBarrier2KHR>) },
		} };
		std::vector<void*> aHooked;
		std::size_t uHooks = 0;
		bool bCore = true;
		for (const stHookTarget& rTarget : aTargets)
		{
			void* pFunction = reinterpret_cast<void*>(pfnGetDeviceProcAddr(hDevice, rTarget.szName));
			if (!pFunction || std::ranges::find(aHooked, pFunction) != aHooked.end())
				continue;
			if (rState.aHooks[rTarget.eHook].Install(pFunction, rTarget.pDetour))
			{
				aHooked.push_back(pFunction);
				++uHooks;
			}
			else if (rTarget.eHook <= kCmdEndRenderPass)
				bCore = false;
		}
		if (!bCore || !rState.aHooks[kCreateImage].IsInstalled() || !rState.aHooks[kCmdBeginRenderPass].IsInstalled())
		{
			for (clInlineHook& rHook : rState.aHooks)
			{
				rHook.Disable();
				rHook.Uninstall();
			}
			Log(ELogLevel::Warning, "could not hook Vulkan for depth tracking, world drawing will not be occluded");
			return;
		}
		rState.hHookedDevice = hDevice;
		Log(ELogLevel::Debug, "Vulkan depth tracking hooked ({} functions)", uHooks);
	}

	VkImageView Attach(const stAttachInfo& stInfo)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (!stInfo.hDevice)
			return VK_NULL_HANDLE;
		const HMODULE hLoader = GetModuleHandleW(L"vulkan-1.dll");
		const auto pfnGetDeviceProcAddr = hLoader ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(reinterpret_cast<void*>(GetProcAddress(hLoader, "vkGetDeviceProcAddr"))) : nullptr;
		if (!pfnGetDeviceProcAddr)
			return VK_NULL_HANDLE;
		DestroyObjects(rState);
		rState.hDevice = stInfo.hDevice;
		rState.hPhysical = stInfo.hPhysicalDevice;
		bool bLoaded = true;
#define OMEGARENDERHOOK_VK_LOAD(Name) \
		rState.stVk.Name = reinterpret_cast<PFN_##Name>(pfnGetDeviceProcAddr(rState.hDevice, #Name)); \
		bLoaded = bLoaded && rState.stVk.Name != nullptr;
		OMEGARENDERHOOK_VK_DEPTH_FUNCTIONS(OMEGARENDERHOOK_VK_LOAD)
#undef OMEGARENDERHOOK_VK_LOAD
		rState.stExtent = stInfo.stExtent;
		rState.uFrames = stInfo.uFrames;
		if (!bLoaded || !CreateImage(rState, rState.stTarget, eTargetFormat, stInfo.stExtent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) || !CreatePipeline(rState, stInfo.hRenderPass))
		{
			Log(ELogLevel::Warning, "could not create the Vulkan depth resolve objects, world drawing will not be occluded");
			DestroyObjects(rState);
			rState.hDevice = VK_NULL_HANDLE;
			return VK_NULL_HANDLE;
		}
		ResetSelection(rState);
		rState.bAttached = true;
		return rState.stTarget.hView;
	}

	bool BeforePass(VkCommandBuffer hCommand, std::uint32_t uSlot)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (!rState.bAttached || !rState.hChosen || !rState.stSnapshot.hImage || rState.aSets.empty())
			return false;
		if (!rState.bCaptured)
		{
			if (rState.eChosenLayout == VK_IMAGE_LAYOUT_UNDEFINED)
				return false;
			RecordCopy(rState, hCommand, rState.eChosenLayout);
		}
		const VkImageMemoryBarrier stBarrier = ImageBarrier(rState.stSnapshot.hImage, FullAspect(rState.stChosen.eFormat), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
		rState.stVk.vkCmdPipelineBarrier(hCommand, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &stBarrier);
		const VkDescriptorImageInfo stImage{ rState.hSampler, rState.stSnapshot.hView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet stWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		stWrite.dstSet = rState.aSets[uSlot % rState.aSets.size()];
		stWrite.descriptorCount = 1;
		stWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		stWrite.pImageInfo = &stImage;
		rState.stVk.vkUpdateDescriptorSets(rState.hDevice, 1, &stWrite, 0, nullptr);
		rState.stConstants = DepthCommon::ResolveConstants(rState.stArea, rState.stChosen.stExtent.width, rState.stChosen.stExtent.height, rState.stExtent.width, rState.stExtent.height);
		return true;
	}

	void InPass(VkCommandBuffer hCommand, std::uint32_t uSlot)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (!rState.bAttached || rState.aSets.empty())
			return;
		const stFunctions& rVk = rState.stVk;
		rVk.vkCmdBindPipeline(hCommand, VK_PIPELINE_BIND_POINT_GRAPHICS, rState.hPipeline);
		const VkDescriptorSet hSet = rState.aSets[uSlot % rState.aSets.size()];
		rVk.vkCmdBindDescriptorSets(hCommand, VK_PIPELINE_BIND_POINT_GRAPHICS, rState.hLayout, 0, 1, &hSet, 0, nullptr);
		rVk.vkCmdPushConstants(hCommand, rState.hLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rState.stConstants), &rState.stConstants);
		const VkViewport stViewport{ 0.f, 0.f, static_cast<float>(rState.stExtent.width), static_cast<float>(rState.stExtent.height), 0.f, 1.f };
		const VkRect2D stScissor{ { 0, 0 }, rState.stExtent };
		rVk.vkCmdSetViewport(hCommand, 0, 1, &stViewport);
		rVk.vkCmdSetScissor(hCommand, 0, 1, &stScissor);
		rVk.vkCmdDraw(hCommand, 3, 1, 0, 0);
	}

	void AfterPass(VkCommandBuffer hCommand, bool bUsed)
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		if (!rState.bAttached)
			return;
		if (bUsed && rState.stSnapshot.hImage)
		{
			const VkImageMemoryBarrier stBarrier = ImageBarrier(rState.stSnapshot.hImage, FullAspect(rState.stChosen.eFormat), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
			rState.stVk.vkCmdPipelineBarrier(hCommand, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &stBarrier);
		}
		SelectNext(rState, hCommand);
	}

	bool ReversedZ()
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		return rState.bReversedKnown ? rState.bReversedZ : rState.iReversedVotes > 0;
	}

	void Detach()
	{
		stState& rState = State();
		std::scoped_lock lckDepth(rState.mtxDepth);
		DestroyObjects(rState);
		ResetSelection(rState);
		rState.bAttached = false;
		rState.hDevice = VK_NULL_HANDLE;
	}

	bool Disable()
	{
		stState& rState = State();
		std::scoped_lock lckHooks(rState.mtxHooks);
		bool bRestored = true;
		for (clInlineHook& rHook : rState.aHooks)
			bRestored = rHook.Disable() && bRestored;
		return bRestored;
	}

	void Release(bool bDrained)
	{
		stState& rState = State();
		{
			std::scoped_lock lckDepth(rState.mtxDepth);
			DestroyObjects(rState);
			ResetSelection(rState);
			rState.bAttached = false;
			rState.hDevice = VK_NULL_HANDLE;
			rState.mapImages.clear();
			rState.mapViews.clear();
			rState.mapPasses.clear();
			rState.mapFramebuffers.clear();
			rState.mapActive.clear();
			rState.mapFeatures.clear();
		}
		if (!bDrained)
			return;
		std::scoped_lock lckHooks(rState.mtxHooks);
		for (clInlineHook& rHook : rState.aHooks)
			rHook.Uninstall();
		rState.hHookedDevice = VK_NULL_HANDLE;
	}
}

#endif
