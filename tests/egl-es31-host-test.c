/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit test for libegl's OpenGL ES 3.1 context creation gating
 * against Vulkan physical device capabilities.
 */

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Mock Vulkan physical device state controlled by each test case.
 */
struct mock_device {
	VkQueueFlags queue_flags;
	VkBool32 vertex_stores;
	uint32_t max_dim_3d;
	uint32_t max_array_layers;
	uint32_t max_wg_invocations;
	uint32_t max_wg_size[3];
	uint32_t max_wg_count[3];
	uint32_t max_shared_mem;
	uint32_t max_stage_storage_buffers;
	uint32_t max_set_storage_buffers;
	uint32_t max_storage_buffer_range;
	uint32_t max_stage_storage_images;
	uint32_t max_set_storage_images;
	VkSampleCountFlags no_attach_samples;
	int storage_image_supported;
};

static struct mock_device mock;
static int checks;
static int failures;

static void mock_reset_es31(void);
static void check_true(const char *name, int condition);
static void check_context_ok(const char *name, EGLDisplay display, EGLConfig config, EGLint major, EGLint minor);
static void check_context_bad_match(const char *name, EGLDisplay display, EGLConfig config, EGLint major, EGLint minor);
static void run_case(const char *name, int expect_es30, int expect_es31);

/* Resets the mock device to a full OpenGL ES 3.1-capable configuration. */
static void
mock_reset_es31(void)
{
	memset(&mock, 0, sizeof(mock));
	mock.queue_flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
	mock.vertex_stores = VK_TRUE;
	mock.max_dim_3d = 256U;
	mock.max_array_layers = 256U;
	mock.max_wg_invocations = 128U;
	mock.max_wg_size[0] = 128U;
	mock.max_wg_size[1] = 128U;
	mock.max_wg_size[2] = 64U;
	mock.max_wg_count[0] = 65535U;
	mock.max_wg_count[1] = 65535U;
	mock.max_wg_count[2] = 65535U;
	mock.max_shared_mem = 16384U;
	mock.max_stage_storage_buffers = 4U;
	mock.max_set_storage_buffers = 4U;
	mock.max_storage_buffer_range = (1U << 27);
	mock.max_stage_storage_images = 4U;
	mock.max_set_storage_images = 4U;
	mock.no_attach_samples = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
	mock.storage_image_supported = 1;
}

/* Records one boolean assertion. */
static void
check_true(
	const char *name,
	int condition)
{
	checks++;
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", name);
		failures++;
	}
}

/* Verifies that eglCreateContext succeeds for the given ES version. */
static void
check_context_ok(
	const char *name,
	EGLDisplay display,
	EGLConfig config,
	EGLint major,
	EGLint minor)
{
	EGLint attrs[5];
	EGLContext ctx;
	EGLint err;

	attrs[0] = EGL_CONTEXT_MAJOR_VERSION;
	attrs[1] = major;
	attrs[2] = EGL_CONTEXT_MINOR_VERSION;
	attrs[3] = minor;
	attrs[4] = EGL_NONE;

	ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
	err = eglGetError();
	check_true(name, ctx != EGL_NO_CONTEXT && err == EGL_SUCCESS);
	if (ctx != EGL_NO_CONTEXT)
		(void)eglDestroyContext(display, ctx);
}

/* Verifies that eglCreateContext fails with EGL_BAD_MATCH for the given ES version. */
static void
check_context_bad_match(
	const char *name,
	EGLDisplay display,
	EGLConfig config,
	EGLint major,
	EGLint minor)
{
	EGLint attrs[5];
	EGLContext ctx;
	EGLint err;

	attrs[0] = EGL_CONTEXT_MAJOR_VERSION;
	attrs[1] = major;
	attrs[2] = EGL_CONTEXT_MINOR_VERSION;
	attrs[3] = minor;
	attrs[4] = EGL_NONE;

	ctx = eglCreateContext(display, config, EGL_NO_CONTEXT, attrs);
	err = eglGetError();
	check_true(name, ctx == EGL_NO_CONTEXT && err == EGL_BAD_MATCH);
	if (ctx != EGL_NO_CONTEXT)
		(void)eglDestroyContext(display, ctx);
}

/* Initializes a surfaceless display and checks ES 2.0, 3.0, 3.1 and 3.2 context creation. */
static void
run_case(
	const char *name,
	int expect_es30,
	int expect_es31)
{
	static int dummy_native;
	EGLDisplay display;
	EGLConfig config;
	EGLint count;
	EGLBoolean ok;
	char label[128];

	/* Use distinct native pointers per call so eglGetDisplay returns a fresh display. */
	dummy_native++;
	display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, (void *)(uintptr_t)dummy_native, NULL);
	check_true(name, display != EGL_NO_DISPLAY);
	if (display == EGL_NO_DISPLAY)
		return;

	ok = eglInitialize(display, NULL, NULL);
	check_true(name, ok == EGL_TRUE);
	if (!ok)
		return;

	count = 0;
	config = NULL;
	ok = eglGetConfigs(display, &config, 1, &count);
	check_true(name, ok == EGL_TRUE && count == 1);

	snprintf(label, sizeof(label), "%s: es2.0", name);
	check_context_ok(label, display, config, 2, 0);

	snprintf(label, sizeof(label), "%s: es3.0", name);
	if (expect_es30)
		check_context_ok(label, display, config, 3, 0);
	else
		check_context_bad_match(label, display, config, 3, 0);

	snprintf(label, sizeof(label), "%s: es3.1 (config)", name);
	if (expect_es31)
		check_context_ok(label, display, config, 3, 1);
	else
		check_context_bad_match(label, display, config, 3, 1);

	snprintf(label, sizeof(label), "%s: es3.1 (no config)", name);
	if (expect_es31)
		check_context_ok(label, display, EGL_NO_CONFIG_KHR, 3, 1);
	else
		check_context_bad_match(label, display, EGL_NO_CONFIG_KHR, 3, 1);

	snprintf(label, sizeof(label), "%s: es3.2", name);
	check_context_bad_match(label, display, config, 3, 2);

	(void)eglTerminate(display);
}

/* Runs all device capability scenarios. */
int
main(void)
{
	/* Full ES 3.1 device. */
	mock_reset_es31();
	run_case("es31-capable", 1, 1);

	/* ES 3.0 device without compute queue bit. */
	mock_reset_es31();
	mock.queue_flags = VK_QUEUE_GRAPHICS_BIT;
	run_case("es30-no-compute-queue", 1, 0);

	/* ES 3.0 device with insufficient compute workgroup invocations. */
	mock_reset_es31();
	mock.max_wg_invocations = 64U;
	run_case("es30-low-wg-invocations", 1, 0);

	/* ES 3.0 device with insufficient compute shared memory. */
	mock_reset_es31();
	mock.max_shared_mem = 8192U;
	run_case("es30-low-shared-mem", 1, 0);

	/* ES 3.0 device with insufficient storage buffers per stage. */
	mock_reset_es31();
	mock.max_stage_storage_buffers = 2U;
	run_case("es30-low-storage-buffers", 1, 0);

	/* ES 3.0 device with insufficient storage buffer range. */
	mock_reset_es31();
	mock.max_storage_buffer_range = (1U << 26);
	run_case("es30-low-storage-range", 1, 0);

	/* ES 3.0 device with insufficient storage images per stage. */
	mock_reset_es31();
	mock.max_stage_storage_images = 2U;
	run_case("es30-low-storage-images", 1, 0);

	/* ES 3.0 device without 4x attachmentless framebuffer sample count. */
	mock_reset_es31();
	mock.no_attach_samples = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT;
	run_case("es30-low-no-attach-samples", 1, 0);

	/* ES 3.0 device without any storable image format. */
	mock_reset_es31();
	mock.storage_image_supported = 0;
	run_case("es30-no-storage-image-format", 1, 0);

	/* ES 2.0-only device (no vertexPipelineStoresAndAtomics). */
	mock_reset_es31();
	mock.vertex_stores = VK_FALSE;
	run_case("es20-only", 0, 0);

	if (failures != 0) {
		printf("%d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("all checks passed (%d)\n", checks);
	return 0;
}

/*
 * Vulkan stubs required by vulkan.c.
 */

VkResult
vkCreateInstance(
	const VkInstanceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkInstance *pInstance)
{
	(void)pCreateInfo;
	(void)pAllocator;
	*pInstance = (VkInstance)(uintptr_t)0x1;
	return VK_SUCCESS;
}

void
vkDestroyInstance(
	VkInstance instance,
	const VkAllocationCallbacks *pAllocator)
{
	(void)instance;
	(void)pAllocator;
}

VkResult
vkEnumeratePhysicalDevices(
	VkInstance instance,
	uint32_t *pPhysicalDeviceCount,
	VkPhysicalDevice *pPhysicalDevices)
{
	(void)instance;
	if (pPhysicalDevices == NULL) {
		*pPhysicalDeviceCount = 1U;
		return VK_SUCCESS;
	}
	*pPhysicalDeviceCount = 1U;
	pPhysicalDevices[0] = (VkPhysicalDevice)(uintptr_t)0x2;
	return VK_SUCCESS;
}

void
vkGetPhysicalDeviceQueueFamilyProperties(
	VkPhysicalDevice physicalDevice,
	uint32_t *pQueueFamilyPropertyCount,
	VkQueueFamilyProperties *pQueueFamilyProperties)
{
	(void)physicalDevice;
	if (pQueueFamilyProperties == NULL) {
		*pQueueFamilyPropertyCount = 1U;
		return;
	}
	*pQueueFamilyPropertyCount = 1U;
	memset(&pQueueFamilyProperties[0], 0, sizeof(pQueueFamilyProperties[0]));
	pQueueFamilyProperties[0].queueFlags = mock.queue_flags;
	pQueueFamilyProperties[0].queueCount = 1U;
}

void
vkGetPhysicalDeviceFeatures(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceFeatures *pFeatures)
{
	(void)physicalDevice;
	memset(pFeatures, 0, sizeof(*pFeatures));
	pFeatures->vertexPipelineStoresAndAtomics = mock.vertex_stores;
}

void
vkGetPhysicalDeviceProperties(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceProperties *pProperties)
{
	(void)physicalDevice;
	memset(pProperties, 0, sizeof(*pProperties));
	pProperties->limits.maxImageDimension3D = mock.max_dim_3d;
	pProperties->limits.maxImageArrayLayers = mock.max_array_layers;
	pProperties->limits.maxComputeWorkGroupInvocations = mock.max_wg_invocations;
	pProperties->limits.maxComputeWorkGroupSize[0] = mock.max_wg_size[0];
	pProperties->limits.maxComputeWorkGroupSize[1] = mock.max_wg_size[1];
	pProperties->limits.maxComputeWorkGroupSize[2] = mock.max_wg_size[2];
	pProperties->limits.maxComputeWorkGroupCount[0] = mock.max_wg_count[0];
	pProperties->limits.maxComputeWorkGroupCount[1] = mock.max_wg_count[1];
	pProperties->limits.maxComputeWorkGroupCount[2] = mock.max_wg_count[2];
	pProperties->limits.maxComputeSharedMemorySize = mock.max_shared_mem;
	pProperties->limits.maxPerStageDescriptorStorageBuffers = mock.max_stage_storage_buffers;
	pProperties->limits.maxDescriptorSetStorageBuffers = mock.max_set_storage_buffers;
	pProperties->limits.maxStorageBufferRange = mock.max_storage_buffer_range;
	pProperties->limits.maxPerStageDescriptorStorageImages = mock.max_stage_storage_images;
	pProperties->limits.maxDescriptorSetStorageImages = mock.max_set_storage_images;
	pProperties->limits.framebufferNoAttachmentsSampleCounts = mock.no_attach_samples;
}

VkResult
vkGetPhysicalDeviceImageFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkImageType type,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkImageCreateFlags flags,
	VkImageFormatProperties *pImageFormatProperties)
{
	(void)physicalDevice;
	(void)format;
	(void)type;
	(void)tiling;
	(void)usage;
	(void)flags;
	memset(pImageFormatProperties, 0, sizeof(*pImageFormatProperties));
	pImageFormatProperties->maxArrayLayers = mock.max_array_layers;
	return VK_SUCCESS;
}

void
vkGetPhysicalDeviceFormatProperties(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkFormatProperties *pFormatProperties)
{
	(void)physicalDevice;
	(void)format;
	memset(pFormatProperties, 0, sizeof(*pFormatProperties));
	if (mock.storage_image_supported)
		pFormatProperties->optimalTilingFeatures |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
}

VkResult
vkCreateDevice(
	VkPhysicalDevice physicalDevice,
	const VkDeviceCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDevice *pDevice)
{
	(void)physicalDevice;
	(void)pCreateInfo;
	(void)pAllocator;
	*pDevice = (VkDevice)(uintptr_t)0x3;
	return VK_SUCCESS;
}

void
vkDestroyDevice(
	VkDevice device,
	const VkAllocationCallbacks *pAllocator)
{
	(void)device;
	(void)pAllocator;
}

VkResult
vkDeviceWaitIdle(
	VkDevice device)
{
	(void)device;
	return VK_SUCCESS;
}

void
vkGetDeviceQueue(
	VkDevice device,
	uint32_t queueFamilyIndex,
	uint32_t queueIndex,
	VkQueue *pQueue)
{
	(void)device;
	(void)queueFamilyIndex;
	(void)queueIndex;
	*pQueue = (VkQueue)(uintptr_t)0x4;
}

void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties *m) { (void)p; memset(m, 0, sizeof(*m)); }
VkResult vkCreateWaylandSurfaceKHR(VkInstance i, const VkWaylandSurfaceCreateInfoKHR *c, const VkAllocationCallbacks *a, VkSurfaceKHR *s) { (void)i; (void)c; (void)a; *s = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { (void)i; (void)s; (void)a; }
VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice p, uint32_t q, VkSurfaceKHR s, VkBool32 *b) { (void)p; (void)q; (void)s; *b = VK_TRUE; return VK_SUCCESS; }
VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) { (void)p; (void)s; memset(c, 0, sizeof(*c)); return VK_SUCCESS; }
VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) { (void)p; (void)s; *n = 0U; (void)f; return VK_SUCCESS; }
VkResult vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) { (void)p; (void)s; *n = 0U; (void)m; return VK_SUCCESS; }
VkResult vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *c, const VkAllocationCallbacks *a, VkSwapchainKHR *s) { (void)d; (void)c; (void)a; *s = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) { (void)d; (void)s; (void)a; }
VkResult vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *i) { (void)d; (void)s; *n = 0U; (void)i; return VK_SUCCESS; }
VkResult vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore sem, VkFence f, uint32_t *i) { (void)d; (void)s; (void)t; (void)sem; (void)f; *i = 0U; return VK_SUCCESS; }
VkResult vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *p) { (void)q; (void)p; return VK_SUCCESS; }
VkResult vkCreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *c, const VkAllocationCallbacks *a, VkCommandPool *p) { (void)d; (void)c; (void)a; *p = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)p; (void)a; }
VkResult vkAllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *i, VkCommandBuffer *b) { (void)d; (void)i; *b = NULL; return VK_SUCCESS; }
VkResult vkResetCommandBuffer(VkCommandBuffer b, VkCommandBufferResetFlags f) { (void)b; (void)f; return VK_SUCCESS; }
VkResult vkBeginCommandBuffer(VkCommandBuffer b, const VkCommandBufferBeginInfo *i) { (void)b; (void)i; return VK_SUCCESS; }
VkResult vkEndCommandBuffer(VkCommandBuffer b) { (void)b; return VK_SUCCESS; }
VkResult vkCreateFence(VkDevice d, const VkFenceCreateInfo *c, const VkAllocationCallbacks *a, VkFence *f) { (void)d; (void)c; (void)a; *f = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a) { (void)d; (void)f; (void)a; }
VkResult vkWaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 w, uint64_t t) { (void)d; (void)n; (void)f; (void)w; (void)t; return VK_SUCCESS; }
VkResult vkResetFences(VkDevice d, uint32_t n, const VkFence *f) { (void)d; (void)n; (void)f; return VK_SUCCESS; }
VkResult vkGetFenceStatus(VkDevice d, VkFence f) { (void)d; (void)f; return VK_SUCCESS; }
VkResult vkCreateSemaphore(VkDevice d, const VkSemaphoreCreateInfo *c, const VkAllocationCallbacks *a, VkSemaphore *s) { (void)d; (void)c; (void)a; *s = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) { (void)d; (void)s; (void)a; }
VkResult vkCreateRenderPass(VkDevice d, const VkRenderPassCreateInfo *c, const VkAllocationCallbacks *a, VkRenderPass *r) { (void)d; (void)c; (void)a; *r = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroyRenderPass(VkDevice d, VkRenderPass r, const VkAllocationCallbacks *a) { (void)d; (void)r; (void)a; }
VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo *c, const VkAllocationCallbacks *a, VkImage *i) { (void)d; (void)c; (void)a; *i = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a) { (void)d; (void)i; (void)a; }
void vkGetImageMemoryRequirements(VkDevice d, VkImage i, VkMemoryRequirements *m) { (void)d; (void)i; memset(m, 0, sizeof(*m)); }
VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *m) { (void)d; (void)i; (void)a; *m = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { (void)d; (void)m; (void)a; }
VkResult vkBindImageMemory(VkDevice d, VkImage i, VkDeviceMemory m, VkDeviceSize o) { (void)d; (void)i; (void)m; (void)o; return VK_SUCCESS; }
VkResult vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *c, const VkAllocationCallbacks *a, VkImageView *v) { (void)d; (void)c; (void)a; *v = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroyImageView(VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)v; (void)a; }
VkResult vkCreateFramebuffer(VkDevice d, const VkFramebufferCreateInfo *c, const VkAllocationCallbacks *a, VkFramebuffer *f) { (void)d; (void)c; (void)a; *f = VK_NULL_HANDLE; return VK_SUCCESS; }
void vkDestroyFramebuffer(VkDevice d, VkFramebuffer f, const VkAllocationCallbacks *a) { (void)d; (void)f; (void)a; }
void vkCmdBeginRenderPass(VkCommandBuffer b, const VkRenderPassBeginInfo *i, VkSubpassContents s) { (void)b; (void)i; (void)s; }
void vkCmdEndRenderPass(VkCommandBuffer b) { (void)b; }
void vkCmdPipelineBarrier(VkCommandBuffer b, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t mc, const VkMemoryBarrier *m, uint32_t bc, const VkBufferMemoryBarrier *bb, uint32_t ic, const VkImageMemoryBarrier *ib) { (void)b; (void)s; (void)d; (void)f; (void)mc; (void)m; (void)bc; (void)bb; (void)ic; (void)ib; }
VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) { (void)q; (void)n; (void)s; (void)f; return VK_SUCCESS; }
VkResult vkQueueWaitIdle(VkQueue q) { (void)q; return VK_SUCCESS; }
VkResult vkGetPhysicalDeviceDisplayPropertiesKHR(VkPhysicalDevice p, uint32_t *n, VkDisplayPropertiesKHR *d) { (void)p; *n = 0U; (void)d; return VK_SUCCESS; }
VkResult vkGetPhysicalDeviceDisplayPlanePropertiesKHR(VkPhysicalDevice p, uint32_t *n, VkDisplayPlanePropertiesKHR *d) { (void)p; *n = 0U; (void)d; return VK_SUCCESS; }
VkResult vkGetDisplayPlaneSupportedDisplaysKHR(VkPhysicalDevice p, uint32_t i, uint32_t *n, VkDisplayKHR *d) { (void)p; (void)i; *n = 0U; (void)d; return VK_SUCCESS; }
VkResult vkGetDisplayModePropertiesKHR(VkPhysicalDevice p, VkDisplayKHR d, uint32_t *n, VkDisplayModePropertiesKHR *m) { (void)p; (void)d; *n = 0U; (void)m; return VK_SUCCESS; }
VkResult vkCreateDisplayPlaneSurfaceKHR(VkInstance i, const VkDisplaySurfaceCreateInfoKHR *c, const VkAllocationCallbacks *a, VkSurfaceKHR *s) { (void)i; (void)c; (void)a; *s = VK_NULL_HANDLE; return VK_SUCCESS; }
