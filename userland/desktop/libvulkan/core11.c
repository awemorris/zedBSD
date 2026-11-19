/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan 1.1 core commands: the core names of the commands the library
 * already offered as extensions, the 1.1 feature and property structures,
 * the one-device group, descriptor update templates and the capabilities
 * the library does not have (sampler YCbCr conversion, protected memory,
 * external semaphores), which it answers as absent.
 */

#include <string.h>
#include "internal.h"

/*
 * One descriptor update template: the entries it was created with, read
 * again at each update.  It is the library's own object; the renderer never
 * sees it.
 */
struct vulkan_descriptor_template {
	struct vulkan_object object;
	uint32_t entry_count;
	VkDescriptorUpdateTemplateEntry *entries;
};

/* Writes the fields of one chained input structure of a forwarded chain. */
typedef void (*core11_self)(struct vulkan_writer *writer, const VkBaseInStructure *record);

static void core11_chain(struct vulkan_writer *writer, const VkBaseInStructure *chain, const VkStructureType *types, uint32_t count, core11_self self);
static void core11_device_self(struct vulkan_writer *writer, const VkBaseInStructure *record);
static void core11_render_pass_self(struct vulkan_writer *writer, const VkBaseInStructure *record);
static void core11_write_array(struct vulkan_writer *writer, const uint32_t *values, uint32_t count);
static VkResult core11_check_flag(VkBool32 requested, VkBool32 reported);
static uint32_t core11_descriptor_kind(VkDescriptorType type);

/* The kinds of descriptor a template entry carries, by the array it reads into. */
#define CORE11_DESCRIPTOR_IMAGE		1U
#define CORE11_DESCRIPTOR_BUFFER	2U
#define CORE11_DESCRIPTOR_VIEW		3U

/*
 * The 1.1 feature structures a device creation forwards to a renderer
 * that answers the Vulkan 1.1 queries, and the one render pass structure.
 */
static const VkStructureType core11_device_types[] = {
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
	VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES
};
static const VkStructureType core11_render_pass_types[] = {
	VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO
};

/* The number of entries of a table. */
#define CORE11_COUNT(table)		((uint32_t)(sizeof(table) / sizeof((table)[0])))

/*
 * Reports the library's version, the highest core an instance may use.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumerateInstanceVersion(
	uint32_t *pApiVersion)
{
	/* The library's own version, whatever the renderers name. */
	*pApiVersion = VULKAN_LIBRARY_VERSION;

	/* Succeeded: the version is written. */
	return VK_SUCCESS;
}

/*
 * Lists the device groups: each physical device is a group of its own.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDeviceGroups(
	VkInstance instance,
	uint32_t *pPhysicalDeviceGroupCount,
	VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
	struct VkInstance_T *owner;
	VkPhysicalDeviceGroupProperties *group;
	uint32_t count;
	uint32_t index;

	/* A null array is the count query; the groups are the physical devices. */
	owner = vulkan_instance(instance);
	if (pPhysicalDeviceGroupProperties == NULL) {
		*pPhysicalDeviceGroupCount = owner->physical_device_count;
		return VK_SUCCESS;
	}

	/* The caller's room bounds what is written. */
	count = *pPhysicalDeviceGroupCount;
	if (count > owner->physical_device_count)
		count = owner->physical_device_count;

	/* One device in each group, no allocation spread over a subset of it; the caller's header is kept. */
	for (index = 0; index < count; index++) {
		group = &pPhysicalDeviceGroupProperties[index];
		group->physicalDeviceCount = 1;
		memset(group->physicalDevices, 0, sizeof(group->physicalDevices));
		group->physicalDevices[0] = (VkPhysicalDevice)owner->physical_devices[index];
		group->subsetAllocation = VK_FALSE;
	}
	*pPhysicalDeviceGroupCount = count;

	/* A short array leaves groups unwritten. */
	if (count < owner->physical_device_count)
		return VK_INCOMPLETE;

	/* Succeeded: every group is written. */
	return VK_SUCCESS;
}

/* The core name of vkGetPhysicalDeviceFeatures2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceFeatures2 *pFeatures)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceFeatures2KHR(physicalDevice, pFeatures);

	/* Succeeded: the features and their chain are answered. */
	return;
}

/* The core name of vkGetPhysicalDeviceProperties2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceProperties2(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceProperties2 *pProperties)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceProperties2KHR(physicalDevice, pProperties);

	/* Succeeded: the properties and their chain are answered. */
	return;
}

/* The core name of vkGetPhysicalDeviceFormatProperties2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2(
	VkPhysicalDevice physicalDevice,
	VkFormat format,
	VkFormatProperties2 *pFormatProperties)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceFormatProperties2KHR(physicalDevice, format, pFormatProperties);

	/* Succeeded: the format's properties are answered. */
	return;
}

/* The core name of vkGetPhysicalDeviceImageFormatProperties2KHR. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties2(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
	VkImageFormatProperties2 *pImageFormatProperties)
{
	VkResult status;

	/* The same answer under either name. */
	status = vkGetPhysicalDeviceImageFormatProperties2KHR(physicalDevice, pImageFormatInfo, pImageFormatProperties);

	/* Succeeded or refused as the extension's name answers. */
	return status;
}

/* The core name of vkGetPhysicalDeviceQueueFamilyProperties2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties2(
	VkPhysicalDevice physicalDevice,
	uint32_t *pQueueFamilyPropertyCount,
	VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceQueueFamilyProperties2KHR(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);

	/* Succeeded: the queue families are answered. */
	return;
}

/* The core name of vkGetPhysicalDeviceMemoryProperties2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2(
	VkPhysicalDevice physicalDevice,
	VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceMemoryProperties2KHR(physicalDevice, pMemoryProperties);

	/* Succeeded: the memory types and heaps are answered. */
	return;
}

/* The core name of vkGetPhysicalDeviceSparseImageFormatProperties2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceSparseImageFormatProperties2(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
	uint32_t *pPropertyCount,
	VkSparseImageFormatProperties2 *pProperties)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceSparseImageFormatProperties2KHR(physicalDevice, pFormatInfo, pPropertyCount, pProperties);

	/* Succeeded: the sparse formats are answered. */
	return;
}

/* The core name of vkGetPhysicalDeviceExternalBufferPropertiesKHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceExternalBufferProperties(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceExternalBufferInfo *pExternalBufferInfo,
	VkExternalBufferProperties *pExternalBufferProperties)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceExternalBufferPropertiesKHR(physicalDevice, pExternalBufferInfo, pExternalBufferProperties);

	/* Succeeded: the buffer's external memory capabilities are answered. */
	return;
}

/* The core name of vkGetPhysicalDeviceExternalFencePropertiesKHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceExternalFenceProperties(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
	VkExternalFenceProperties *pExternalFenceProperties)
{
	/* The same answer under either name. */
	vkGetPhysicalDeviceExternalFencePropertiesKHR(physicalDevice, pExternalFenceInfo, pExternalFenceProperties);

	/* Succeeded: the fence's external capabilities are answered. */
	return;
}

/*
 * Answers that no semaphore handle type can be exported or imported: the
 * library shares no semaphore with another API or process.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetPhysicalDeviceExternalSemaphoreProperties(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
	VkExternalSemaphoreProperties *pExternalSemaphoreProperties)
{
	/* Neither the device nor the handle type changes the answer. */
	(void)physicalDevice;
	(void)pExternalSemaphoreInfo;

	/* No capability for any handle type; the caller's header is kept. */
	pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
	pExternalSemaphoreProperties->compatibleHandleTypes = 0;
	pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;

	/* Succeeded: the absent capability is answered. */
	return;
}

/* The core name of vkGetImageMemoryRequirements2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements2(
	VkDevice device,
	const VkImageMemoryRequirementsInfo2 *pInfo,
	VkMemoryRequirements2 *pMemoryRequirements)
{
	/* The same answer under either name. */
	vkGetImageMemoryRequirements2KHR(device, pInfo, pMemoryRequirements);

	/* Succeeded: the image's requirements are answered. */
	return;
}

/* The core name of vkGetBufferMemoryRequirements2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements2(
	VkDevice device,
	const VkBufferMemoryRequirementsInfo2 *pInfo,
	VkMemoryRequirements2 *pMemoryRequirements)
{
	/* The same answer under either name. */
	vkGetBufferMemoryRequirements2KHR(device, pInfo, pMemoryRequirements);

	/* Succeeded: the buffer's requirements are answered. */
	return;
}

/* The core name of vkGetImageSparseMemoryRequirements2KHR. */
VKAPI_ATTR void VKAPI_CALL
vkGetImageSparseMemoryRequirements2(
	VkDevice device,
	const VkImageSparseMemoryRequirementsInfo2 *pInfo,
	uint32_t *pSparseMemoryRequirementCount,
	VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
	/* The same answer under either name. */
	vkGetImageSparseMemoryRequirements2KHR(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);

	/* Succeeded: the image's sparse requirements are answered. */
	return;
}

/*
 * Binds each buffer to its memory in order, as vkBindBufferMemory does;
 * the device-group record of the one-device group names device 0 only.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory2(
	VkDevice device,
	uint32_t bindInfoCount,
	const VkBindBufferMemoryInfo *pBindInfos)
{
	VkResult status;
	uint32_t index;

	/* Each binding, the first failure ending the call. */
	for (index = 0; index < bindInfoCount; index++) {
		status = vkBindBufferMemory(device, pBindInfos[index].buffer, pBindInfos[index].memory, pBindInfos[index].memoryOffset);
		if (status != VK_SUCCESS)
			return status;
	}

	/* Succeeded: every buffer is bound. */
	return VK_SUCCESS;
}

/*
 * Binds each image to its memory in order, as vkBindImageMemory does; no
 * image has planes to bind apart (no sampler YCbCr conversion).
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory2(
	VkDevice device,
	uint32_t bindInfoCount,
	const VkBindImageMemoryInfo *pBindInfos)
{
	VkResult status;
	uint32_t index;

	/* Each binding, the first failure ending the call. */
	for (index = 0; index < bindInfoCount; index++) {
		status = vkBindImageMemory(device, pBindInfos[index].image, pBindInfos[index].memory, pBindInfos[index].memoryOffset);
		if (status != VK_SUCCESS)
			return status;
	}

	/* Succeeded: every image is bound. */
	return VK_SUCCESS;
}

/*
 * Answers how a device of the group reaches another's memory.  The group
 * has one device, which reaches its own memory in every way.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceGroupPeerMemoryFeatures(
	VkDevice device,
	uint32_t heapIndex,
	uint32_t localDeviceIndex,
	uint32_t remoteDeviceIndex,
	VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
	/* The heap does not change the answer. */
	(void)device;
	(void)heapIndex;

	/* No other device exists. */
	*pPeerMemoryFeatures = 0;
	if (localDeviceIndex != 0 || remoteDeviceIndex != 0)
		return;

	/* The one device and its own memory. */
	*pPeerMemoryFeatures = VK_PEER_MEMORY_FEATURE_COPY_SRC_BIT |
	    VK_PEER_MEMORY_FEATURE_COPY_DST_BIT |
	    VK_PEER_MEMORY_FEATURE_GENERIC_SRC_BIT |
	    VK_PEER_MEMORY_FEATURE_GENERIC_DST_BIT;

	/* Succeeded: the features are written. */
	return;
}

/*
 * Trims a command pool.  The renderer keeps the pool's storage, and giving
 * it back is only a hint, so the library has nothing to do.
 *
 * Procedure referenced: the Vulkan 1.1 specification (section 6.2,
 * vkTrimCommandPool: trimming does not change the pool's command buffers).
 */
VKAPI_ATTR void VKAPI_CALL
vkTrimCommandPool(
	VkDevice device,
	VkCommandPool commandPool,
	VkCommandPoolTrimFlags flags)
{
	/* Nothing the caller can observe changes. */
	(void)device;
	(void)commandPool;
	(void)flags;

	/* Succeeded: the pool is unchanged. */
	return;
}

/*
 * Returns a queue by its creation flags as well as its family and index.
 * No queue is created protected, so a request with flags finds none.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue2(
	VkDevice device,
	const VkDeviceQueueInfo2 *pQueueInfo,
	VkQueue *pQueue)
{
	/* No queue was created with flags. */
	if (pQueueInfo->flags != 0) {
		*pQueue = VK_NULL_HANDLE;
		return;
	}

	/* The queue of the family and index, as vkGetDeviceQueue finds it. */
	vkGetDeviceQueue(device, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, pQueue);

	/* Succeeded: the queue, or the null handle for none. */
	return;
}

/*
 * Refuses a sampler YCbCr conversion: the library reports the feature as
 * absent, so no valid call reaches this.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSamplerYcbcrConversion(
	VkDevice device,
	const VkSamplerYcbcrConversionCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkSamplerYcbcrConversion *pYcbcrConversion)
{
	/* No conversion exists to make. */
	(void)device;
	(void)pCreateInfo;
	(void)pAllocator;
	*pYcbcrConversion = VK_NULL_HANDLE;

	/* Refused: the feature is absent. */
	return VK_ERROR_FEATURE_NOT_PRESENT;
}

/* Destroys a sampler YCbCr conversion: only the null handle can exist. */
VKAPI_ATTR void VKAPI_CALL
vkDestroySamplerYcbcrConversion(
	VkDevice device,
	VkSamplerYcbcrConversion ycbcrConversion,
	const VkAllocationCallbacks *pAllocator)
{
	/* No conversion was ever made. */
	(void)device;
	(void)ycbcrConversion;
	(void)pAllocator;

	/* Succeeded: nothing to release. */
	return;
}

/*
 * Creates a descriptor update template: the library keeps the entries and
 * turns each update with it into vkUpdateDescriptorSets.  Only the
 * descriptor-set form exists (push descriptors are not offered).
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDescriptorUpdateTemplate(
	VkDevice device,
	const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate)
{
	struct VkDevice_T *owner;
	struct vulkan_descriptor_template *template;
	struct vulkan_object *object;
	size_t bytes;
	VkResult status;

	/* Only templates that update descriptor sets. */
	if (pCreateInfo->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The object and its entries in one allocation, the size checked on the user ABI. */
	bytes = (size_t)pCreateInfo->descriptorUpdateEntryCount * sizeof(VkDescriptorUpdateTemplateEntry);
	if (bytes / sizeof(VkDescriptorUpdateTemplateEntry) != pCreateInfo->descriptorUpdateEntryCount)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	if (bytes > SIZE_MAX - sizeof(*template))
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A local object of the device, with the caller's allocator. */
	owner = vulkan_device(device);
	status = vulkan_object_alloc(sizeof(*template) + bytes, __alignof__(struct vulkan_descriptor_template), VULKAN_OBJECT_DESCRIPTOR_UPDATE_TEMPLATE, &owner->object, owner->object.context, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (status != VK_SUCCESS)
		return status;

	/* The entries follow the object. */
	template = (struct vulkan_descriptor_template *)object;
	template->entry_count = pCreateInfo->descriptorUpdateEntryCount;
	template->entries = (VkDescriptorUpdateTemplateEntry *)(template + 1);
	if (bytes != 0)
		memcpy(template->entries, pCreateInfo->pDescriptorUpdateEntries, bytes);

	/* The device owns the template until it is destroyed. */
	status = vulkan_object_publish(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}
	*pDescriptorUpdateTemplate = (VkDescriptorUpdateTemplate)(uintptr_t)vulkan_nondispatchable_handle(object);

	/* Succeeded: the template is made. */
	return VK_SUCCESS;
}

/* Destroys a descriptor update template. */
VKAPI_ATTR void VKAPI_CALL
vkDestroyDescriptorUpdateTemplate(
	VkDevice device,
	VkDescriptorUpdateTemplate descriptorUpdateTemplate,
	const VkAllocationCallbacks *pAllocator)
{
	struct vulkan_object *object;

	/* A null handle is ignored. */
	(void)device;
	if (descriptorUpdateTemplate == VK_NULL_HANDLE)
		return;

	/* The local object, with the caller's allocator. */
	object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)descriptorUpdateTemplate);
	vulkan_object_free_with_allocator(object, pAllocator);

	/* Succeeded: the template is released. */
	return;
}

/*
 * Updates a descriptor set from a template: each entry reads its
 * descriptors from pData at its offset and stride, and the whole becomes
 * one vkUpdateDescriptorSets call.
 */
VKAPI_ATTR void VKAPI_CALL
vkUpdateDescriptorSetWithTemplate(
	VkDevice device,
	VkDescriptorSet descriptorSet,
	VkDescriptorUpdateTemplate descriptorUpdateTemplate,
	const void *pData)
{
	struct VkDevice_T *owner;
	struct vulkan_descriptor_template *template;
	const VkDescriptorUpdateTemplateEntry *entry;
	VkWriteDescriptorSet *writes;
	VkDescriptorImageInfo *images;
	VkDescriptorBufferInfo *buffers;
	VkBufferView *views;
	const uint8_t *source;
	uint8_t *storage;
	size_t total;
	size_t used;
	size_t bytes;
	uint32_t written;
	uint32_t kind;
	uint32_t index;
	uint32_t element;

	/* The template and the number of descriptors it writes. */
	owner = vulkan_device(device);
	template = (struct vulkan_descriptor_template *)vulkan_nondispatchable_object((uint64_t)(uintptr_t)descriptorUpdateTemplate);
	total = 0;
	for (index = 0; index < template->entry_count; index++)
		total += template->entries[index].descriptorCount;
	if (template->entry_count == 0)
		return;

	/* The writes and one array for each kind of descriptor, each long enough for all, in one allocation. */
	bytes = (size_t)template->entry_count * sizeof(*writes) + total * (sizeof(*images) + sizeof(*buffers) + sizeof(*views));
	storage = vulkan_allocate(&owner->object.allocator, bytes, __alignof__(VkDescriptorImageInfo), VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
	if (storage == NULL)
		return;
	images = (VkDescriptorImageInfo *)storage;
	buffers = (VkDescriptorBufferInfo *)(images + total);
	writes = (VkWriteDescriptorSet *)(buffers + total);
	views = (VkBufferView *)(writes + template->entry_count);

	/* Each entry: its descriptors gathered from the caller's data into the array of their kind. */
	written = 0;
	used = 0;
	for (index = 0; index < template->entry_count; index++) {
		entry = &template->entries[index];
		kind = core11_descriptor_kind(entry->descriptorType);

		/* A kind the library does not know cannot be written. */
		if (kind == 0)
			continue;

		/* The write, as vkUpdateDescriptorSets takes it. */
		memset(&writes[written], 0, sizeof(writes[written]));
		writes[written].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[written].dstSet = descriptorSet;
		writes[written].dstBinding = entry->dstBinding;
		writes[written].dstArrayElement = entry->dstArrayElement;
		writes[written].descriptorCount = entry->descriptorCount;
		writes[written].descriptorType = entry->descriptorType;
		writes[written].pImageInfo = &images[used];
		writes[written].pBufferInfo = &buffers[used];
		writes[written].pTexelBufferView = &views[used];

		/* Each descriptor at its offset and stride in the caller's data. */
		for (element = 0; element < entry->descriptorCount; element++) {
			source = (const uint8_t *)pData + entry->offset + (size_t)element * entry->stride;
			if (kind == CORE11_DESCRIPTOR_IMAGE)
				memcpy(&images[used + element], source, sizeof(*images));
			if (kind == CORE11_DESCRIPTOR_BUFFER)
				memcpy(&buffers[used + element], source, sizeof(*buffers));
			if (kind == CORE11_DESCRIPTOR_VIEW)
				memcpy(&views[used + element], source, sizeof(*views));
		}
		used += entry->descriptorCount;
		written++;
	}

	/* The writes in one update, then the storage given back. */
	if (written != 0)
		vkUpdateDescriptorSets(device, written, writes, 0, NULL);
	vulkan_free(&owner->object.allocator, storage);

	/* Succeeded: the set is updated. */
	return;
}

/*
 * Answers whether a descriptor set layout can be made: a set of at most
 * maxPerSetDescriptors descriptors is always supported, and the library
 * promises no more than that.
 */
VKAPI_ATTR void VKAPI_CALL
vkGetDescriptorSetLayoutSupport(
	VkDevice device,
	const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
	VkDescriptorSetLayoutSupport *pSupport)
{
	struct VkDevice_T *owner;
	uint64_t total;
	uint32_t index;

	/* Every descriptor of every binding. */
	owner = vulkan_device(device);
	total = 0;
	for (index = 0; index < pCreateInfo->bindingCount; index++)
		total += pCreateInfo->pBindings[index].descriptorCount;

	/* Within the limit the renderer reported; the caller's header is kept. */
	pSupport->supported = VK_FALSE;
	if (total <= owner->physical->core11.max_per_set_descriptors)
		pSupport->supported = VK_TRUE;

	/* Succeeded: the answer is written. */
	return;
}

/*
 * Answers the Vulkan 1.1 feature structures chained to
 * VkPhysicalDeviceFeatures2 from what the renderer reported (nothing, so
 * every feature absent, when it was not asked).  Protected memory and
 * sampler YCbCr conversion are absent whatever the renderer has: the
 * library implements neither.
 */
void
vulkan_core11_features(
	struct VkPhysicalDevice_T *physical,
	VkBaseOutStructure *chain)
{
	const struct vulkan_core11_facts *facts;
	VkBaseOutStructure *next;
	VkPhysicalDeviceMultiviewFeatures *multiview;
	VkPhysicalDevice16BitStorageFeatures *storage;
	VkPhysicalDeviceVariablePointersFeatures *pointers;
	VkPhysicalDeviceProtectedMemoryFeatures *protection;
	VkPhysicalDeviceSamplerYcbcrConversionFeatures *conversion;
	VkPhysicalDeviceShaderDrawParametersFeatures *parameters;

	/* Each structure wherever the caller chained it; its header is kept. */
	facts = &physical->core11;
	for (next = chain; next != NULL; next = next->pNext) {
		switch (next->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
			multiview = (VkPhysicalDeviceMultiviewFeatures *)next;
			multiview->multiview = facts->multiview;
			multiview->multiviewGeometryShader = facts->multiview_geometry_shader;
			multiview->multiviewTessellationShader = facts->multiview_tessellation_shader;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
			storage = (VkPhysicalDevice16BitStorageFeatures *)next;
			storage->storageBuffer16BitAccess = facts->storage_buffer_16bit;
			storage->uniformAndStorageBuffer16BitAccess = facts->uniform_storage_buffer_16bit;
			storage->storagePushConstant16 = facts->storage_push_constant_16;
			storage->storageInputOutput16 = facts->storage_input_output_16;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
			pointers = (VkPhysicalDeviceVariablePointersFeatures *)next;
			pointers->variablePointersStorageBuffer = facts->variable_pointers_storage_buffer;
			pointers->variablePointers = facts->variable_pointers;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
			protection = (VkPhysicalDeviceProtectedMemoryFeatures *)next;
			protection->protectedMemory = VK_FALSE;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
			conversion = (VkPhysicalDeviceSamplerYcbcrConversionFeatures *)next;
			conversion->samplerYcbcrConversion = VK_FALSE;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
			parameters = (VkPhysicalDeviceShaderDrawParametersFeatures *)next;
			parameters->shaderDrawParameters = facts->shader_draw_parameters;
			break;
		default:
			break;
		}
	}

	/* Succeeded: the structures the library knows are answered. */
	return;
}

/*
 * Answers the Vulkan 1.1 property structures chained to
 * VkPhysicalDeviceProperties2 from what the renderer reported.  No memory
 * is protected, so protectedNoFault is false.
 */
void
vulkan_core11_properties(
	struct VkPhysicalDevice_T *physical,
	VkBaseOutStructure *chain)
{
	const struct vulkan_core11_facts *facts;
	VkBaseOutStructure *next;
	VkPhysicalDeviceSubgroupProperties *subgroup;
	VkPhysicalDevicePointClippingProperties *clipping;
	VkPhysicalDeviceMultiviewProperties *multiview;
	VkPhysicalDeviceProtectedMemoryProperties *protection;
	VkPhysicalDeviceMaintenance3Properties *maintenance;

	/* Each structure wherever the caller chained it; its header is kept. */
	facts = &physical->core11;
	for (next = chain; next != NULL; next = next->pNext) {
		switch (next->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES:
			subgroup = (VkPhysicalDeviceSubgroupProperties *)next;
			subgroup->subgroupSize = facts->subgroup_size;
			subgroup->supportedStages = facts->subgroup_stages;
			subgroup->supportedOperations = facts->subgroup_operations;
			subgroup->quadOperationsInAllStages = facts->subgroup_quad_all_stages;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES:
			clipping = (VkPhysicalDevicePointClippingProperties *)next;
			clipping->pointClippingBehavior = (VkPointClippingBehavior)facts->point_clipping;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES:
			multiview = (VkPhysicalDeviceMultiviewProperties *)next;
			multiview->maxMultiviewViewCount = facts->max_multiview_view_count;
			multiview->maxMultiviewInstanceIndex = facts->max_multiview_instance_index;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES:
			protection = (VkPhysicalDeviceProtectedMemoryProperties *)next;
			protection->protectedNoFault = VK_FALSE;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES:
			maintenance = (VkPhysicalDeviceMaintenance3Properties *)next;
			maintenance->maxPerSetDescriptors = facts->max_per_set_descriptors;
			maintenance->maxMemoryAllocationSize = facts->max_memory_allocation_size;
			break;
		default:
			break;
		}
	}

	/* Succeeded: the structures the library knows are answered. */
	return;
}

/*
 * Checks the Vulkan 1.1 records chained to a device creation: the core
 * features through VkPhysicalDeviceFeatures2 (written to features, NULL
 * when the caller chained none), every 1.1 feature asked for being one the
 * device reports, and a device group of this one device.
 */
VkResult
vulkan_core11_device_check(
	struct VkPhysicalDevice_T *physical,
	const VkDeviceCreateInfo *info,
	const VkPhysicalDeviceFeatures **features)
{
	const struct vulkan_core11_facts *facts;
	const VkBaseInStructure *next;
	const VkPhysicalDeviceMultiviewFeatures *multiview;
	const VkPhysicalDevice16BitStorageFeatures *storage;
	const VkPhysicalDeviceVariablePointersFeatures *pointers;
	const VkPhysicalDeviceProtectedMemoryFeatures *protection;
	const VkPhysicalDeviceSamplerYcbcrConversionFeatures *conversion;
	const VkPhysicalDeviceShaderDrawParametersFeatures *parameters;
	const VkDeviceGroupDeviceCreateInfo *group;
	VkResult status;
	uint32_t index;

	/* No chained core features yet. */
	facts = &physical->core11;
	*features = NULL;
	status = VK_SUCCESS;
	for (next = info->pNext; next != NULL && status == VK_SUCCESS; next = next->pNext) {
		switch (next->sType) {
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
			*features = &((const VkPhysicalDeviceFeatures2 *)next)->features;
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
			multiview = (const VkPhysicalDeviceMultiviewFeatures *)next;
			status = core11_check_flag(multiview->multiview, facts->multiview);
			if (status == VK_SUCCESS)
				status = core11_check_flag(multiview->multiviewGeometryShader, facts->multiview_geometry_shader);
			if (status == VK_SUCCESS)
				status = core11_check_flag(multiview->multiviewTessellationShader, facts->multiview_tessellation_shader);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
			storage = (const VkPhysicalDevice16BitStorageFeatures *)next;
			status = core11_check_flag(storage->storageBuffer16BitAccess, facts->storage_buffer_16bit);
			if (status == VK_SUCCESS)
				status = core11_check_flag(storage->uniformAndStorageBuffer16BitAccess, facts->uniform_storage_buffer_16bit);
			if (status == VK_SUCCESS)
				status = core11_check_flag(storage->storagePushConstant16, facts->storage_push_constant_16);
			if (status == VK_SUCCESS)
				status = core11_check_flag(storage->storageInputOutput16, facts->storage_input_output_16);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
			pointers = (const VkPhysicalDeviceVariablePointersFeatures *)next;
			status = core11_check_flag(pointers->variablePointersStorageBuffer, facts->variable_pointers_storage_buffer);
			if (status == VK_SUCCESS)
				status = core11_check_flag(pointers->variablePointers, facts->variable_pointers);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
			protection = (const VkPhysicalDeviceProtectedMemoryFeatures *)next;
			status = core11_check_flag(protection->protectedMemory, VK_FALSE);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
			conversion = (const VkPhysicalDeviceSamplerYcbcrConversionFeatures *)next;
			status = core11_check_flag(conversion->samplerYcbcrConversion, VK_FALSE);
			break;
		case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES:
			parameters = (const VkPhysicalDeviceShaderDrawParametersFeatures *)next;
			status = core11_check_flag(parameters->shaderDrawParameters, facts->shader_draw_parameters);
			break;
		case VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO:
			/* The group may name this device alone. */
			group = (const VkDeviceGroupDeviceCreateInfo *)next;
			if (group->physicalDeviceCount > 1)
				status = VK_ERROR_INITIALIZATION_FAILED;
			for (index = 0; index < group->physicalDeviceCount && status == VK_SUCCESS; index++) {
				if (vulkan_physical_device(group->pPhysicalDevices[index]) != physical)
					status = VK_ERROR_INITIALIZATION_FAILED;
			}
			break;
		default:
			break;
		}
	}

	/* Refused: a feature the device lacks, or a group of other devices. */
	if (status != VK_SUCCESS)
		return status;

	/* The core features in one place only. */
	if (*features != NULL && info->pEnabledFeatures != NULL)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* No queue is created protected. */
	for (index = 0; index < info->queueCreateInfoCount; index++) {
		if (info->pQueueCreateInfos[index].flags != 0)
			return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	/* Succeeded: the device can be made with what was asked. */
	return VK_SUCCESS;
}

/*
 * Encodes the chain of a device creation: the 1.1 feature structures the
 * caller chained, to a renderer that answers the Vulkan 1.1 queries (a
 * renderer that does not gets an empty chain, and its device names 1.0).
 */
void
vulkan_encode_device_chain(
	struct vulkan_writer *writer,
	const void *chain)
{
	/* A renderer without the 1.1 queries reads no chain. */
	if (!writer->core11_chains) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* The 1.1 feature structures in the caller's order. */
	core11_chain(writer, chain, core11_device_types, CORE11_COUNT(core11_device_types), core11_device_self);

	/* Succeeded: the chain is encoded. */
	return;
}

/*
 * Encodes the chain of a render pass creation: the multiview views of each
 * subpass, to a renderer that answers the Vulkan 1.1 queries.
 */
void
vulkan_encode_render_pass_chain(
	struct vulkan_writer *writer,
	const void *chain)
{
	/* A renderer without the 1.1 queries reads no chain. */
	if (!writer->core11_chains) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* The multiview record, if the caller chained one. */
	core11_chain(writer, chain, core11_render_pass_types, CORE11_COUNT(core11_render_pass_types), core11_render_pass_self);

	/* Succeeded: the chain is encoded. */
	return;
}

/*
 * Encodes the structures of a chain whose types are in types, others left
 * out: each present one's header ([u64 1][u32 sType]), the end of the chain
 * ([u64 0]), then the fields of each from the last to the first.
 *
 * Procedure referenced: venus-protocol docs/VK_EXT_command_serialization.txt
 * (the encoding of a pNext chain), commit
 * ca19b6358d7cc491bc3e4de76f04c6700876a8fa.
 */
static void
core11_chain(
	struct vulkan_writer *writer,
	const VkBaseInStructure *chain,
	const VkStructureType *types,
	uint32_t count,
	core11_self self)
{
	const VkBaseInStructure *next;
	VkBool32 known;
	uint32_t index;

	/* The next structure the renderer is to read. */
	for (next = chain; next != NULL; next = next->pNext) {
		known = VK_FALSE;
		for (index = 0; index < count; index++) {
			if (next->sType == types[index])
				known = VK_TRUE;
		}
		if (known)
			break;
	}

	/* The end of the chain. */
	if (next == NULL) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* Its header, the rest of the chain, then its fields. */
	vulkan_write_u64(writer, 1U);
	vulkan_write_u32(writer, (uint32_t)next->sType);
	core11_chain(writer, next->pNext, types, count, self);
	self(writer, next);

	/* Succeeded: this structure and those after it are encoded. */
	return;
}

/* Writes the fields of one 1.1 feature structure of a device creation. */
static void
core11_device_self(
	struct vulkan_writer *writer,
	const VkBaseInStructure *record)
{
	const VkPhysicalDeviceMultiviewFeatures *multiview;
	const VkPhysicalDevice16BitStorageFeatures *storage;
	const VkPhysicalDeviceVariablePointersFeatures *pointers;
	const VkPhysicalDeviceProtectedMemoryFeatures *protection;
	const VkPhysicalDeviceSamplerYcbcrConversionFeatures *conversion;
	const VkPhysicalDeviceShaderDrawParametersFeatures *parameters;

	/* The fields in their declared order, each a 32-bit word. */
	switch (record->sType) {
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:
		multiview = (const VkPhysicalDeviceMultiviewFeatures *)record;
		vulkan_write_u32(writer, multiview->multiview);
		vulkan_write_u32(writer, multiview->multiviewGeometryShader);
		vulkan_write_u32(writer, multiview->multiviewTessellationShader);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:
		storage = (const VkPhysicalDevice16BitStorageFeatures *)record;
		vulkan_write_u32(writer, storage->storageBuffer16BitAccess);
		vulkan_write_u32(writer, storage->uniformAndStorageBuffer16BitAccess);
		vulkan_write_u32(writer, storage->storagePushConstant16);
		vulkan_write_u32(writer, storage->storageInputOutput16);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES:
		pointers = (const VkPhysicalDeviceVariablePointersFeatures *)record;
		vulkan_write_u32(writer, pointers->variablePointersStorageBuffer);
		vulkan_write_u32(writer, pointers->variablePointers);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES:
		protection = (const VkPhysicalDeviceProtectedMemoryFeatures *)record;
		vulkan_write_u32(writer, protection->protectedMemory);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES:
		conversion = (const VkPhysicalDeviceSamplerYcbcrConversionFeatures *)record;
		vulkan_write_u32(writer, conversion->samplerYcbcrConversion);
		break;
	default:
		parameters = (const VkPhysicalDeviceShaderDrawParametersFeatures *)record;
		vulkan_write_u32(writer, parameters->shaderDrawParameters);
		break;
	}

	/* Succeeded: the fields are written. */
	return;
}

/* Writes the fields of the multiview record of a render pass creation. */
static void
core11_render_pass_self(
	struct vulkan_writer *writer,
	const VkBaseInStructure *record)
{
	const VkRenderPassMultiviewCreateInfo *multiview;

	/* The view mask of each subpass, the view offset of each dependency, and the correlation masks. */
	multiview = (const VkRenderPassMultiviewCreateInfo *)record;
	vulkan_write_u32(writer, multiview->subpassCount);
	core11_write_array(writer, multiview->pViewMasks, multiview->subpassCount);
	vulkan_write_u32(writer, multiview->dependencyCount);
	core11_write_array(writer, (const uint32_t *)multiview->pViewOffsets, multiview->dependencyCount);
	vulkan_write_u32(writer, multiview->correlationMaskCount);
	core11_write_array(writer, multiview->pCorrelationMasks, multiview->correlationMaskCount);

	/* Succeeded: the fields are written. */
	return;
}

/* Writes an array of 32-bit words: its length (0 when absent), then each. */
static void
core11_write_array(
	struct vulkan_writer *writer,
	const uint32_t *values,
	uint32_t count)
{
	uint32_t index;

	/* An absent array is an empty one. */
	if (values == NULL) {
		vulkan_write_u64(writer, 0U);
		return;
	}

	/* The length, then the words. */
	vulkan_write_u64(writer, count);
	for (index = 0; index < count; index++)
		vulkan_write_u32(writer, values[index]);

	/* Succeeded: the array is written. */
	return;
}

/* Refuses a feature asked for that the device does not report. */
static VkResult
core11_check_flag(
	VkBool32 requested,
	VkBool32 reported)
{
	/* A feature not asked for, or one the device has. */
	if (!requested || reported)
		return VK_SUCCESS;

	/* Refused: the feature is absent. */
	return VK_ERROR_FEATURE_NOT_PRESENT;
}

/* The array a template entry's descriptors are read into, 0 for a kind the library does not write. */
static uint32_t
core11_descriptor_kind(
	VkDescriptorType type)
{
	/* By the record vkUpdateDescriptorSets reads for the type. */
	switch (type) {
	case VK_DESCRIPTOR_TYPE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		return CORE11_DESCRIPTOR_IMAGE;
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		return CORE11_DESCRIPTOR_VIEW;
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		return CORE11_DESCRIPTOR_BUFFER;
	default:
		break;
	}

	/* Not a kind the library writes. */
	return 0;
}
