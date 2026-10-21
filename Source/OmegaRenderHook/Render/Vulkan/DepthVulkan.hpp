#pragma once

#include "OmegaRenderHook/Render/Pipeline.hpp"

#if OMEGARENDERHOOK_VULKAN

#include <cstdint>

namespace OmegaRenderHook::DepthVulkan
{
	struct stAttachInfo
	{
		VkDevice hDevice = VK_NULL_HANDLE;
		VkPhysicalDevice hPhysicalDevice = VK_NULL_HANDLE;
		VkRenderPass hRenderPass = VK_NULL_HANDLE;
		VkExtent2D stExtent{};
		std::uint32_t uFrames = 0;
	};

	inline constexpr VkFormat eTargetFormat = VK_FORMAT_D32_SFLOAT;

	void OnDeviceCreated(VkDevice hDevice, VkPhysicalDevice hPhysicalDevice);
	[[nodiscard]] VkImageView Attach(const stAttachInfo& stInfo);
	[[nodiscard]] bool BeforePass(VkCommandBuffer hCommand, std::uint32_t uSlot);
	void InPass(VkCommandBuffer hCommand, std::uint32_t uSlot);
	void AfterPass(VkCommandBuffer hCommand, bool bUsed);
	[[nodiscard]] bool ReversedZ();
	void Detach();
	bool Disable();
	void Release(bool bDrained);
}

#endif
