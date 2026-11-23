/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * xserver's Vulkan, shared by the drawing layer (render-vulkan.c) and the
 * swapchains (present-vulkan.c): the instance, the device and its queue,
 * and the table of the Vulkan functions the core calls.
 *
 * The core links no Vulkan library.  Every function is found through the
 * loader's vkGetInstanceProcAddr, which the program gives the core
 * (zedBSD's libvulkan, or vulkan-1.dll that mikuterm opens at run time),
 * so the same code runs where the loader is a library the program links
 * and where it is opened only if the system has one.
 *
 * Only the files that draw or show with Vulkan include this header.
 */

#ifndef KEILAND_X11SERVER_RENDER_VULKAN_H
#define KEILAND_X11SERVER_RENDER_VULKAN_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "userland/desktop/xserver/render.h"

struct x11server;
struct x11_render_vulkan;

/*
 * The Vulkan functions the core calls, found through the loader with the
 * instance (the device's functions too: the loader dispatches them).
 */
struct x11_vulkan_functions {
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	PFN_vkCreateInstance vkCreateInstance;
	PFN_vkDestroyInstance vkDestroyInstance;
	PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
	PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
	PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures;
	PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
	PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
	PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
	PFN_vkCreateDevice vkCreateDevice;
	PFN_vkDestroyDevice vkDestroyDevice;
	PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
	PFN_vkGetDeviceQueue vkGetDeviceQueue;
	PFN_vkQueueSubmit vkQueueSubmit;
	PFN_vkAllocateMemory vkAllocateMemory;
	PFN_vkFreeMemory vkFreeMemory;
	PFN_vkMapMemory vkMapMemory;
	PFN_vkInvalidateMappedMemoryRanges vkInvalidateMappedMemoryRanges;
	PFN_vkFlushMappedMemoryRanges vkFlushMappedMemoryRanges;
	PFN_vkCreateBuffer vkCreateBuffer;
	PFN_vkDestroyBuffer vkDestroyBuffer;
	PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
	PFN_vkBindBufferMemory vkBindBufferMemory;
	PFN_vkCreateImage vkCreateImage;
	PFN_vkDestroyImage vkDestroyImage;
	PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
	PFN_vkBindImageMemory vkBindImageMemory;
	PFN_vkCreateImageView vkCreateImageView;
	PFN_vkDestroyImageView vkDestroyImageView;
	PFN_vkCreateSampler vkCreateSampler;
	PFN_vkDestroySampler vkDestroySampler;
	PFN_vkCreateShaderModule vkCreateShaderModule;
	PFN_vkDestroyShaderModule vkDestroyShaderModule;
	PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
	PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
	PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
	PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
	PFN_vkResetDescriptorPool vkResetDescriptorPool;
	PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
	PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
	PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
	PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
	PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
	PFN_vkDestroyPipeline vkDestroyPipeline;
	PFN_vkCreateRenderPass vkCreateRenderPass;
	PFN_vkDestroyRenderPass vkDestroyRenderPass;
	PFN_vkCreateFramebuffer vkCreateFramebuffer;
	PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
	PFN_vkCreateCommandPool vkCreateCommandPool;
	PFN_vkDestroyCommandPool vkDestroyCommandPool;
	PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
	PFN_vkResetCommandBuffer vkResetCommandBuffer;
	PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
	PFN_vkEndCommandBuffer vkEndCommandBuffer;
	PFN_vkCreateFence vkCreateFence;
	PFN_vkDestroyFence vkDestroyFence;
	PFN_vkResetFences vkResetFences;
	PFN_vkGetFenceStatus vkGetFenceStatus;
	PFN_vkWaitForFences vkWaitForFences;
	PFN_vkCreateSemaphore vkCreateSemaphore;
	PFN_vkDestroySemaphore vkDestroySemaphore;
	PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
	PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
	PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
	PFN_vkCmdBindPipeline vkCmdBindPipeline;
	PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
	PFN_vkCmdBindVertexBuffers vkCmdBindVertexBuffers;
	PFN_vkCmdPushConstants vkCmdPushConstants;
	PFN_vkCmdSetViewport vkCmdSetViewport;
	PFN_vkCmdSetScissor vkCmdSetScissor;
	PFN_vkCmdDraw vkCmdDraw;
	PFN_vkCmdClearAttachments vkCmdClearAttachments;
	PFN_vkCmdClearColorImage vkCmdClearColorImage;
	PFN_vkCmdCopyImage vkCmdCopyImage;
	PFN_vkCmdBlitImage vkCmdBlitImage;
	PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
	PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
	PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR;
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceSupportKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceFormatsKHR;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR vkGetPhysicalDeviceSurfacePresentModesKHR;
	PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR;
	PFN_vkDestroySwapchainKHR vkDestroySwapchainKHR;
	PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR;
	PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR;
	PFN_vkQueuePresentKHR vkQueuePresentKHR;
};

/*
 * The server's Vulkan: made when the server starts (with the loader the
 * program gave), ended with the server.  The device is the first one with
 * a graphics queue, a GPU before a processor's; windows whose surface that
 * queue cannot present to are shown through the backend.
 */
struct x11_vulkan {
	struct x11_vulkan_functions vk;
	struct x11server *server;
	VkInstance instance;

	/* Nonzero when the instance has the WSI extensions and the device the swapchain's. */
	int wsi;

	/* The device, its one queue and the family it is of, and its memory types. */
	VkPhysicalDevice physical;
	uint32_t family;
	VkDevice device;
	VkQueue queue;
	VkPhysicalDeviceMemoryProperties memory;

	/*
	 * What the device can do that the drawing asks of it: logic
	 * operations, the largest image side, and the most bytes a shader
	 * reads of a storage buffer.
	 */
	int logic_op;
	uint32_t image_side_max;
	uint32_t storage_range_max;

	/*
	 * Nonzero once the device was lost: nothing more is drawn or shown on
	 * it, and the server ends its clients and goes on without Vulkan.
	 */
	int lost;

	/* The drawing layer on the device (NULL: the windows and pixmaps are drawn in memory). */
	struct x11_render_vulkan *render;
};

/* render-vulkan.c: the drawing layer's state, and the frames the swapchains record into its stream. */
int x11_render_vulkan_open(struct x11_vulkan *vulkan);
void x11_render_vulkan_close(struct x11_vulkan *vulkan);
VkImage x11_render_vulkan_image(const struct x11_image *image);
VkCommandBuffer x11_render_vulkan_command(struct x11_vulkan *vulkan);
VkResult x11_render_vulkan_submit(struct x11_vulkan *vulkan, VkSemaphore wait, VkSemaphore signal);
uint32_t x11_vulkan_memory_type(const struct x11_vulkan *vulkan, uint32_t allowed, VkMemoryPropertyFlags wanted);
void x11_vulkan_failed(struct x11_vulkan *vulkan, VkResult error, const char *what);

#endif
