/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Carries Vulkan Video H.264 decode (VK_KHR_video_queue, VK_KHR_video_decode_queue
 * and VK_KHR_video_decode_h264) to the native renderer.
 *
 * The renderer is the authority on sessions, parameters and decoding; this
 * file checks what can be checked without it (the one supported profile, the
 * keys of the parameter sets), and encodes every call in the protocol's
 * ordinary form: a structure is its sType, its chain, then its fields in
 * declared order; a chain link is a 64-bit presence word followed by the next
 * known structure, and unknown structures are skipped; a pointer is a
 * presence word followed by what it points to; an array is a 64-bit count
 * followed by its elements.  The H.264 standard structures carry their bit
 * field flags as one 32-bit word, packed by name with the first declared flag
 * in bit 0, and drop the VUI.
 */

#include <string.h>
#include "internal.h"

/* The largest enumeration the renderer answers in one reply: formats or session bindings. */
#define VIDEO_ENUMERATION_MAX 32U

/* How many sequence parameter set ids H.264 has (seq_parameter_set_id is 0 to 31). */
#define VIDEO_SPS_IDS 32U

/* How many picture parameter set ids H.264 has (pic_parameter_set_id is 0 to 255). */
#define VIDEO_PPS_IDS 256U

/* The words of one bit for each (SPS id, PPS id) pair, the key of a picture parameter set. */
#define VIDEO_PPS_KEY_WORDS (VIDEO_SPS_IDS * VIDEO_PPS_IDS / 32U)

/*
 * One video session parameters object as this library sees it: the keys of
 * the parameter sets it holds.
 *
 * The renderer keeps the parameter sets themselves.  The keys are kept here
 * so that adding a set whose key is already present is refused with the
 * standard error before the renderer is asked.  They change only after the
 * renderer accepted the change, under the application's external
 * synchronization of the object.
 */
struct vulkan_video_parameters {
	struct vulkan_object object;
	uint32_t sps_keys;
	uint32_t pps_keys[VIDEO_PPS_KEY_WORDS];
};

static VkResult video_profile_check(struct VkPhysicalDevice_T *physical, const VkVideoProfileInfoKHR *profile);
static const VkBaseInStructure *video_find_input(const void *chain, VkStructureType type);
static VkBool32 video_known_input(VkStructureType type);
static void video_encode_struct(struct vulkan_writer *writer, const VkBaseInStructure *record);
static void video_encode_next(struct vulkan_writer *writer, const void *chain);
static void video_encode_fields(struct vulkan_writer *writer, const VkBaseInStructure *record);
static void video_encode_session_create(struct vulkan_writer *writer, const VkVideoSessionCreateInfoKHR *record);
static void video_encode_parameters_add(struct vulkan_writer *writer, const VkVideoDecodeH264SessionParametersAddInfoKHR *record);
static void video_encode_begin(struct vulkan_writer *writer, const VkVideoBeginCodingInfoKHR *record);
static void video_encode_slot(struct vulkan_writer *writer, const VkVideoReferenceSlotInfoKHR *record);
static void video_encode_resource(struct vulkan_writer *writer, const VkVideoPictureResourceInfoKHR *record);
static void video_encode_decode(struct vulkan_writer *writer, const VkVideoDecodeInfoKHR *record);
static void video_encode_picture(struct vulkan_writer *writer, const VkVideoDecodeH264PictureInfoKHR *record);
static void video_encode_sps(struct vulkan_writer *writer, const StdVideoH264SequenceParameterSet *record);
static void video_encode_pps(struct vulkan_writer *writer, const StdVideoH264PictureParameterSet *record);
static void video_encode_scaling(struct vulkan_writer *writer, const StdVideoH264ScalingLists *record);
static void video_encode_std_picture(struct vulkan_writer *writer, const StdVideoDecodeH264PictureInfo *record);
static void video_encode_std_reference(struct vulkan_writer *writer, const StdVideoDecodeH264ReferenceInfo *record);
static uint32_t video_sps_flags(const StdVideoH264SpsFlags *flags);
static uint32_t video_pps_flags(const StdVideoH264PpsFlags *flags);
static uint32_t video_picture_flags(const StdVideoDecodeH264PictureInfoFlags *flags);
static uint32_t video_reference_flags(const StdVideoDecodeH264ReferenceInfoFlags *flags);
static void video_encode_output_request(struct vulkan_writer *writer, VkBaseOutStructure *record);
static void video_decode_output(struct vulkan_reader *reader, VkBaseOutStructure *record);
static void video_decode_next(struct vulkan_reader *reader, VkBaseOutStructure *chain);
static void video_decode_fields(struct vulkan_reader *reader, VkBaseOutStructure *record);
static VkResult video_enumeration_reply(struct vulkan_context *context, struct vulkan_reader *reader, uint32_t *count);
static struct vulkan_video_parameters *video_parameters_object(VkVideoSessionParametersKHR parameters);
static VkResult video_keys_add(uint32_t *sps_keys, uint32_t *pps_keys, const VkVideoDecodeH264SessionParametersAddInfoKHR *add, VkBool32 replace);
static VkBool32 video_key_test(const uint32_t *words, uint32_t index);
static void video_key_set(uint32_t *words, uint32_t index);
static void video_destroy(VkDevice device, uint64_t handle, uint32_t opcode, const VkAllocationCallbacks *allocator);

/*
 * Checks that every profile of an image or format query is the supported H.264 decode profile.
 */
VkResult
vulkan_video_profile_list_check(
	struct VkPhysicalDevice_T *physical,
	const VkVideoProfileListInfoKHR *list)
{
	uint32_t index;
	VkResult status;

	/* An empty list names no profile the picture could be decoded with. */
	if (list->profileCount == 0)
		return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;

	/* Refuses the list at its first unsupported profile, with that profile's reason. */
	for (index = 0; index < list->profileCount; index++) {
		status = video_profile_check(physical, &list->pProfiles[index]);
		if (status != VK_SUCCESS)
			return status;
	}

	/* Succeeded: every listed profile is H.264 decode. */
	return VK_SUCCESS;
}

/*
 * Reports the decode limits of one video profile.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceVideoCapabilitiesKHR(
	VkPhysicalDevice physicalDevice,
	const VkVideoProfileInfoKHR *pVideoProfile,
	VkVideoCapabilitiesKHR *pCapabilities)
{
	struct VkPhysicalDevice_T *physical;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	VkBool32 present;

	/* Refuses a profile other than H.264 decode with the standard reason. */
	physical = vulkan_physical_device(physicalDevice);
	status = video_profile_check(physical, pVideoProfile);
	if (status != VK_SUCCESS)
		return status;

	/*
	 * Sends the profile and the shape of the caller's output chain, so the
	 * renderer answers exactly the capability records the caller chained.
	 */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pVideoProfile);
	vulkan_write_u64(&writer, 1);
	video_encode_output_request(&writer, (VkBaseOutStructure *)pCapabilities);
	status = vulkan_command_execute(physical->object.context, &writer, 4096, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(physical->object.context, &reader, status);
		return status;
	}

	/* Fills the caller's records in place, keeping every header and chain link. */
	present = vulkan_reply_pointer(&reader);
	if (present)
		video_decode_output(&reader, (VkBaseOutStructure *)pCapabilities);

	/* A malformed answer is terminal for the session, as for every reply. */
	status = vulkan_reply_finish(physical->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the caller holds the profile's limits. */
	return VK_SUCCESS;
}

/*
 * Enumerates the image formats a video profile decodes into.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceVideoFormatPropertiesKHR(
	VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceVideoFormatInfoKHR *pVideoFormatInfo,
	uint32_t *pVideoFormatPropertyCount,
	VkVideoFormatPropertiesKHR *pVideoFormatProperties)
{
	struct VkPhysicalDevice_T *physical;
	const VkVideoProfileListInfoKHR *list;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkVideoFormatPropertiesKHR formats[VIDEO_ENUMERATION_MAX];
	VkImageUsageFlags video_usage;
	VkResult status;
	uint32_t count;
	uint32_t index;

	/* The query names its profiles in a profile list, which must name H.264 decode. */
	physical = vulkan_physical_device(physicalDevice);
	list = (const VkVideoProfileListInfoKHR *)video_find_input(pVideoFormatInfo->pNext, VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR);
	if (list == NULL)
		return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;

	/* Refuses the first unsupported profile with its standard reason. */
	status = vulkan_video_profile_list_check(physical, list);
	if (status != VK_SUCCESS)
		return status;

	/* Decode pictures may also be copied into a staging buffer for host readback. */
	video_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if ((pVideoFormatInfo->imageUsage & ~video_usage) != 0)
		return VK_ERROR_IMAGE_USAGE_NOT_SUPPORTED_KHR;

	/* Asks for every format at once; the renderer has far fewer than the reply holds. */
	vulkan_writer_init_for_object(&writer, &physical->object);
	vulkan_command_begin(&writer, GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES);
	vulkan_write_u64(&writer, physical->object.wire_id);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pVideoFormatInfo);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, VIDEO_ENUMERATION_MAX);
	vulkan_write_u64(&writer, VIDEO_ENUMERATION_MAX);
	status = vulkan_command_execute(physical->object.context, &writer, 4096, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(physical->object.context, &reader, status);
		return status;
	}

	/* Reads the format count, which cannot exceed what was asked for. */
	status = video_enumeration_reply(physical->object.context, &reader, &count);
	if (status != VK_SUCCESS)
		return status;

	/* Decodes every format into private storage before touching the caller's array. */
	memset(formats, 0, sizeof(formats));
	for (index = 0; index < count; index++) {
		formats[index].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
		video_decode_output(&reader, (VkBaseOutStructure *)&formats[index]);
	}

	/* A malformed answer is terminal for the session, as for every reply. */
	status = vulkan_reply_finish(physical->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* A null array is the standard count query. */
	if (pVideoFormatProperties == NULL) {
		*pVideoFormatPropertyCount = count;
		return VK_SUCCESS;
	}

	/* Copies the fields of the formats that fit, keeping each caller header and chain. */
	for (index = 0; index < count && index < *pVideoFormatPropertyCount; index++) {
		pVideoFormatProperties[index].format = formats[index].format;
		pVideoFormatProperties[index].componentMapping = formats[index].componentMapping;
		pVideoFormatProperties[index].imageCreateFlags = formats[index].imageCreateFlags;
		pVideoFormatProperties[index].imageType = formats[index].imageType;
		pVideoFormatProperties[index].imageTiling = formats[index].imageTiling;
		pVideoFormatProperties[index].imageUsageFlags = formats[index].imageUsageFlags;
	}

	/* A caller array smaller than the format count receives a prefix and VK_INCOMPLETE. */
	*pVideoFormatPropertyCount = index;
	if (index < count)
		return VK_INCOMPLETE;

	/* Succeeded: the caller holds every format the profiles decode into. */
	return VK_SUCCESS;
}

/*
 * Creates a video session on the native renderer.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateVideoSessionKHR(
	VkDevice device,
	const VkVideoSessionCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkVideoSessionKHR *pVideoSession)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves the local session and its never-reused identity before the renderer acts. */
	owner = vulkan_device(device);
	status = vulkan_object_alloc(sizeof(*object), __alignof__(struct vulkan_object), VULKAN_OBJECT_VIDEO_SESSION, &owner->object, owner->object.context, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (status != VK_SUCCESS)
		return status;

	/* A failed identity reservation leaves no inaccessible local allocation. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* The renderer checks the queue family, profile, limits and header version. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, GPU_OP_CREATE_VIDEO_SESSION);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, GPU_OP_DESTROY_VIDEO_SESSION);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes the caller's output only after both sides created the session. */
	handle = vulkan_nondispatchable_handle(object);
	*pVideoSession = (VkVideoSessionKHR)(uintptr_t)handle;

	/* Succeeded: the handle owns one renderer video session. */
	return VK_SUCCESS;
}

/*
 * Destroys a video session; memory bound to it stays the caller's.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyVideoSessionKHR(
	VkDevice device,
	VkVideoSessionKHR videoSession,
	const VkAllocationCallbacks *pAllocator)
{
	/* Withdraws the renderer session, then the local record. */
	video_destroy(device, (uint64_t)(uintptr_t)videoSession, GPU_OP_DESTROY_VIDEO_SESSION, pAllocator);

	/* Succeeded: a null handle is ignored and the session is gone. */
	return;
}

/*
 * Enumerates the memory a video session needs bound before it decodes.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetVideoSessionMemoryRequirementsKHR(
	VkDevice device,
	VkVideoSessionKHR videoSession,
	uint32_t *pMemoryRequirementsCount,
	VkVideoSessionMemoryRequirementsKHR *pMemoryRequirements)
{
	struct VkDevice_T *owner;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkVideoSessionMemoryRequirementsKHR bindings[VIDEO_ENUMERATION_MAX];
	VkResult status;
	uint32_t count;
	uint32_t index;

	/* Asks for every binding at once; a session has at most the DPB slots and four stores. */
	owner = vulkan_device(device);
	vulkan_writer_init_for_object(&writer, &owner->object);
	vulkan_command_begin(&writer, GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_encode_handle(&writer, (uint64_t)(uintptr_t)videoSession);
	vulkan_write_u64(&writer, 1);
	vulkan_write_u32(&writer, VIDEO_ENUMERATION_MAX);
	vulkan_write_u64(&writer, VIDEO_ENUMERATION_MAX);
	status = vulkan_command_execute(owner->object.context, &writer, 4096, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		status = vulkan_reply_finish(owner->object.context, &reader, status);
		return status;
	}

	/* Reads the binding count, which cannot exceed what was asked for. */
	status = video_enumeration_reply(owner->object.context, &reader, &count);
	if (status != VK_SUCCESS)
		return status;

	/* Decodes every binding into private storage before touching the caller's array. */
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < count; index++) {
		bindings[index].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
		video_decode_output(&reader, (VkBaseOutStructure *)&bindings[index]);
	}

	/* A malformed answer is terminal for the session, as for every reply. */
	status = vulkan_reply_finish(owner->object.context, &reader, VK_SUCCESS);
	if (status != VK_SUCCESS)
		return status;

	/* A null array is the standard count query. */
	if (pMemoryRequirements == NULL) {
		*pMemoryRequirementsCount = count;
		return VK_SUCCESS;
	}

	/* Copies the bindings that fit, keeping each caller header and chain. */
	for (index = 0; index < count && index < *pMemoryRequirementsCount; index++) {
		pMemoryRequirements[index].memoryBindIndex = bindings[index].memoryBindIndex;
		pMemoryRequirements[index].memoryRequirements = bindings[index].memoryRequirements;
	}

	/* A caller array smaller than the binding count receives a prefix and VK_INCOMPLETE. */
	*pMemoryRequirementsCount = index;
	if (index < count)
		return VK_INCOMPLETE;

	/* Succeeded: the caller holds every binding the session needs. */
	return VK_SUCCESS;
}

/*
 * Binds device memory to the bindings of a video session.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkBindVideoSessionMemoryKHR(
	VkDevice device,
	VkVideoSessionKHR videoSession,
	uint32_t bindSessionMemoryInfoCount,
	const VkBindVideoSessionMemoryInfoKHR *pBindSessionMemoryInfos)
{
	struct VkDevice_T *owner;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	VkResult status;
	uint32_t index;

	/* Sends every binding; the renderer checks the indices, sizes and memory types. */
	owner = vulkan_device(device);
	vulkan_writer_init_for_object(&writer, &owner->object);
	vulkan_command_begin(&writer, GPU_OP_BIND_VIDEO_SESSION_MEMORY);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_encode_handle(&writer, (uint64_t)(uintptr_t)videoSession);
	vulkan_write_u32(&writer, bindSessionMemoryInfoCount);
	vulkan_write_u64(&writer, bindSessionMemoryInfoCount);
	for (index = 0; index < bindSessionMemoryInfoCount; index++)
		video_encode_struct(&writer, (const VkBaseInStructure *)&pBindSessionMemoryInfos[index]);

	/* The renderer's result is the call's result. */
	status = vulkan_command_execute(owner->object.context, &writer, 16, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the bindings hold the given memory. */
	return VK_SUCCESS;
}

/*
 * Creates a video session parameters object holding H.264 parameter sets.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateVideoSessionParametersKHR(
	VkDevice device,
	const VkVideoSessionParametersCreateInfoKHR *pCreateInfo,
	const VkAllocationCallbacks *pAllocator,
	VkVideoSessionParametersKHR *pVideoSessionParameters)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	struct vulkan_video_parameters *parameters;
	struct vulkan_video_parameters *source;
	const VkVideoDecodeH264SessionParametersCreateInfoKHR *h264;
	struct vulkan_writer writer;
	VkResult status;
	uint64_t handle;

	/* Reserves the local object before the renderer acts. */
	owner = vulkan_device(device);
	status = vulkan_object_alloc(sizeof(*parameters), __alignof__(struct vulkan_video_parameters), VULKAN_OBJECT_VIDEO_SESSION_PARAMETERS, &owner->object, owner->object.context, pAllocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (status != VK_SUCCESS)
		return status;

	/* A new object starts with the keys of its template, which its own sets may replace. */
	parameters = (struct vulkan_video_parameters *)object;
	source = video_parameters_object(pCreateInfo->videoSessionParametersTemplate);
	if (source != NULL) {
		parameters->sps_keys = source->sps_keys;
		memcpy(parameters->pps_keys, source->pps_keys, sizeof(parameters->pps_keys));
	}

	/* Takes the keys of the sets the create call adds, refusing an id outside H.264. */
	h264 = (const VkVideoDecodeH264SessionParametersCreateInfoKHR *)video_find_input(pCreateInfo->pNext, VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR);
	if (h264 != NULL && h264->pParametersAddInfo != NULL) {
		status = video_keys_add(&parameters->sps_keys, parameters->pps_keys, h264->pParametersAddInfo, VK_TRUE);
		if (status != VK_SUCCESS) {
			vulkan_object_free(object);
			return status;
		}
	}

	/* A failed identity reservation leaves no inaccessible local allocation. */
	status = vulkan_object_reserve_id(object);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* The renderer copies the template and keeps the parameter sets. */
	vulkan_writer_init_for_object(&writer, object);
	vulkan_command_begin(&writer, GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pCreateInfo);
	status = vulkan_object_create_complete(owner, object, &writer, GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS);
	vulkan_writer_finish(&writer);
	if (status != VK_SUCCESS) {
		vulkan_object_free(object);
		return status;
	}

	/* Changes the caller's output only after both sides created the object. */
	handle = vulkan_nondispatchable_handle(object);
	*pVideoSessionParameters = (VkVideoSessionParametersKHR)(uintptr_t)handle;

	/* Succeeded: the handle owns one renderer parameters object. */
	return VK_SUCCESS;
}

/*
 * Adds H.264 parameter sets to a video session parameters object.
 */
VKAPI_ATTR VkResult VKAPI_CALL
vkUpdateVideoSessionParametersKHR(
	VkDevice device,
	VkVideoSessionParametersKHR videoSessionParameters,
	const VkVideoSessionParametersUpdateInfoKHR *pUpdateInfo)
{
	struct VkDevice_T *owner;
	struct vulkan_video_parameters *parameters;
	const VkVideoDecodeH264SessionParametersAddInfoKHR *add;
	struct vulkan_writer writer;
	struct vulkan_reader reader;
	uint32_t sps_keys;
	uint32_t pps_keys[VIDEO_PPS_KEY_WORDS];
	VkResult status;

	/*
	 * Works on a copy of the keys: an update that adds a key already present
	 * is refused whole, and the keys change only once the renderer agrees.
	 */
	owner = vulkan_device(device);
	parameters = video_parameters_object(videoSessionParameters);
	sps_keys = parameters->sps_keys;
	memcpy(pps_keys, parameters->pps_keys, sizeof(pps_keys));
	add = (const VkVideoDecodeH264SessionParametersAddInfoKHR *)video_find_input(pUpdateInfo->pNext, VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR);
	if (add != NULL) {
		status = video_keys_add(&sps_keys, pps_keys, add, VK_FALSE);
		if (status != VK_SUCCESS)
			return status;
	}

	/* The renderer checks the update sequence count and keeps the new sets. */
	vulkan_writer_init_for_object(&writer, &parameters->object);
	vulkan_command_begin(&writer, GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS);
	vulkan_write_u64(&writer, owner->object.wire_id);
	vulkan_write_u64(&writer, parameters->object.wire_id);
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pUpdateInfo);
	status = vulkan_command_execute(owner->object.context, &writer, 16, &reader, VK_TRUE);
	vulkan_writer_finish(&writer);
	status = vulkan_reply_finish(owner->object.context, &reader, status);
	if (status != VK_SUCCESS)
		return status;

	/* Publishes the keys the renderer now holds. */
	parameters->sps_keys = sps_keys;
	memcpy(parameters->pps_keys, pps_keys, sizeof(pps_keys));

	/* Succeeded: the object holds the added parameter sets. */
	return VK_SUCCESS;
}

/*
 * Destroys a video session parameters object.
 */
VKAPI_ATTR void VKAPI_CALL
vkDestroyVideoSessionParametersKHR(
	VkDevice device,
	VkVideoSessionParametersKHR videoSessionParameters,
	const VkAllocationCallbacks *pAllocator)
{
	/* Withdraws the renderer object, then the local record. */
	video_destroy(device, (uint64_t)(uintptr_t)videoSessionParameters, GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS, pAllocator);

	/* Succeeded: a null handle is ignored and the object is gone. */
	return;
}

/*
 * Records the start of a video coding scope and the pictures it binds.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdBeginVideoCodingKHR(
	VkCommandBuffer commandBuffer,
	const VkVideoBeginCodingInfoKHR *pBeginInfo)
{
	struct vulkan_writer writer;
	VkBool32 active;

	/* Keeps the first recording failure for vkEndCommandBuffer to report. */
	active = vulkan_command_record_begin(commandBuffer, &writer, GPU_OP_CMD_BEGIN_VIDEO_CODING);
	if (!active)
		return;

	/* Copies the scope's session, parameters and bound pictures into the record. */
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pBeginInfo);
	vulkan_command_record_finish(commandBuffer, &writer);

	/* Succeeded: the command is recorded or its failure is kept by the command buffer. */
	return;
}

/*
 * Records the end of a video coding scope.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdEndVideoCodingKHR(
	VkCommandBuffer commandBuffer,
	const VkVideoEndCodingInfoKHR *pEndCodingInfo)
{
	struct vulkan_writer writer;
	VkBool32 active;

	/* Keeps the first recording failure for vkEndCommandBuffer to report. */
	active = vulkan_command_record_begin(commandBuffer, &writer, GPU_OP_CMD_END_VIDEO_CODING);
	if (!active)
		return;

	/* Copies the end record. */
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pEndCodingInfo);
	vulkan_command_record_finish(commandBuffer, &writer);

	/* Succeeded: the command is recorded or its failure is kept by the command buffer. */
	return;
}

/*
 * Records a video coding control operation, such as resetting the session.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdControlVideoCodingKHR(
	VkCommandBuffer commandBuffer,
	const VkVideoCodingControlInfoKHR *pCodingControlInfo)
{
	struct vulkan_writer writer;
	VkBool32 active;

	/* Keeps the first recording failure for vkEndCommandBuffer to report. */
	active = vulkan_command_record_begin(commandBuffer, &writer, GPU_OP_CMD_CONTROL_VIDEO_CODING);
	if (!active)
		return;

	/* Copies the control flags. */
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pCodingControlInfo);
	vulkan_command_record_finish(commandBuffer, &writer);

	/* Succeeded: the command is recorded or its failure is kept by the command buffer. */
	return;
}

/*
 * Records the decode of one H.264 picture.
 */
VKAPI_ATTR void VKAPI_CALL
vkCmdDecodeVideoKHR(
	VkCommandBuffer commandBuffer,
	const VkVideoDecodeInfoKHR *pDecodeInfo)
{
	struct vulkan_writer writer;
	VkBool32 active;

	/* Keeps the first recording failure for vkEndCommandBuffer to report. */
	active = vulkan_command_record_begin(commandBuffer, &writer, GPU_OP_CMD_DECODE_VIDEO);
	if (!active)
		return;

	/*
	 * Copies the bitstream range, the pictures and the H.264 picture and
	 * reference information, so the caller's storage is free once this returns.
	 */
	vulkan_write_u64(&writer, 1);
	video_encode_struct(&writer, (const VkBaseInStructure *)pDecodeInfo);
	vulkan_command_record_finish(commandBuffer, &writer);

	/* Succeeded: the command is recorded or its failure is kept by the command buffer. */
	return;
}

/* Checks that a profile is the H.264 decode profile this library implements. */
static VkResult
video_profile_check(
	struct VkPhysicalDevice_T *physical,
	const VkVideoProfileInfoKHR *profile)
{
	const VkVideoDecodeH264ProfileInfoKHR *h264;

	/* A physical device without video decode has no supported operation. */
	if ((physical->supported_extensions & VULKAN_DEVICE_VIDEO_DECODE_H264) == 0)
		return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;

	/* H.264 decode is the one codec operation. */
	if (profile->videoCodecOperation != VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)
		return VK_ERROR_VIDEO_PROFILE_OPERATION_NOT_SUPPORTED_KHR;

	/* The decoder writes 8-bit 4:2:0 pictures only. */
	if (profile->chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
		return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;
	if (profile->lumaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)
		return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;
	if (profile->chromaBitDepth != VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR)
		return VK_ERROR_VIDEO_PROFILE_FORMAT_NOT_SUPPORTED_KHR;

	/* An H.264 profile names its H.264 profile and picture layout in a chained record. */
	h264 = (const VkVideoDecodeH264ProfileInfoKHR *)video_find_input(profile->pNext, VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR);
	if (h264 == NULL)
		return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;

	/* Interlaced pictures are not decoded. */
	if (h264->pictureLayout != VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR)
		return VK_ERROR_VIDEO_PICTURE_LAYOUT_NOT_SUPPORTED_KHR;

	/* Baseline, Main and High are the H.264 profiles the decoder takes. */
	if (h264->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_BASELINE &&
	    h264->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_MAIN &&
	    h264->stdProfileIdc != STD_VIDEO_H264_PROFILE_IDC_HIGH)
		return VK_ERROR_VIDEO_PROFILE_CODEC_NOT_SUPPORTED_KHR;

	/* Succeeded: the profile is supported. */
	return VK_SUCCESS;
}

/* Finds the first record of one type in an input chain. */
static const VkBaseInStructure *
video_find_input(
	const void *chain,
	VkStructureType type)
{
	const VkBaseInStructure *record;

	/* Walks the chain in the caller's order. */
	for (record = chain; record != NULL; record = record->pNext) {
		/* The first record of the type is the one the call uses. */
		if (record->sType == type)
			return record;
	}

	/* The chain has no record of the type. */
	return NULL;
}

/* Tells whether the protocol carries an input record of this type. */
static VkBool32
video_known_input(
	VkStructureType type)
{
	/* Lists every video input record the renderer decodes. */
	switch (type) {
	case VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR:
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR:
	case VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR:
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR:
		return VK_TRUE;
	default:
		break;
	}

	/* Any other record is skipped in a chain. */
	return VK_FALSE;
}

/* Encodes one input record: its type, its chain, then its fields. */
static void
video_encode_struct(
	struct vulkan_writer *writer,
	const VkBaseInStructure *record)
{
	/* The type tells the renderer which fields follow. */
	vulkan_write_u32(writer, (uint32_t)record->sType);

	/* The chain comes before the record's own fields. */
	video_encode_next(writer, record->pNext);

	/* The fields in declared order. */
	video_encode_fields(writer, record);

	/* Succeeded: the record is encoded or the writer keeps its first error. */
	return;
}

/* Encodes the known records of a chain, each one link deeper than the last. */
static void
video_encode_next(
	struct vulkan_writer *writer,
	const void *chain)
{
	const VkBaseInStructure *record;
	VkBool32 known;

	/* Skips the records the protocol does not carry, as the renderer would not know them. */
	record = chain;
	while (record != NULL) {
		/* The first record the protocol carries is the next link. */
		known = video_known_input(record->sType);
		if (known)
			break;

		/* An unknown record contributes only its link to the next. */
		record = record->pNext;
	}

	/* The end of the chain is an absent link. */
	if (record == NULL) {
		vulkan_write_u64(writer, 0);
		return;
	}

	/* A present link carries the next known record, which carries the rest of the chain. */
	vulkan_write_u64(writer, 1);
	video_encode_struct(writer, record);

	/* Succeeded: the chain is encoded or the writer keeps its first error. */
	return;
}

/* Encodes the fields of one known input record in declared order. */
static void
video_encode_fields(
	struct vulkan_writer *writer,
	const VkBaseInStructure *record)
{
	const VkVideoProfileInfoKHR *profile;
	const VkVideoDecodeH264ProfileInfoKHR *h264_profile;
	const VkVideoDecodeUsageInfoKHR *usage;
	const VkVideoProfileListInfoKHR *list;
	const VkPhysicalDeviceVideoFormatInfoKHR *format;
	const VkBindVideoSessionMemoryInfoKHR *bind;
	const VkVideoSessionParametersCreateInfoKHR *parameters;
	const VkVideoDecodeH264SessionParametersCreateInfoKHR *h264_parameters;
	const VkVideoSessionParametersUpdateInfoKHR *update;
	const VkVideoDecodeH264DpbSlotInfoKHR *dpb;
	const VkVideoEndCodingInfoKHR *end;
	const VkVideoCodingControlInfoKHR *control;
	uint32_t index;

	/* Encodes each record type with the fields it declares. */
	switch (record->sType) {
	case VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR:
		/* The codec operation, chroma subsampling and component depths. */
		profile = (const VkVideoProfileInfoKHR *)record;
		vulkan_write_u32(writer, (uint32_t)profile->videoCodecOperation);
		vulkan_write_u32(writer, profile->chromaSubsampling);
		vulkan_write_u32(writer, profile->lumaBitDepth);
		vulkan_write_u32(writer, profile->chromaBitDepth);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR:
		/* The H.264 profile and the picture layout. */
		h264_profile = (const VkVideoDecodeH264ProfileInfoKHR *)record;
		vulkan_write_u32(writer, (uint32_t)h264_profile->stdProfileIdc);
		vulkan_write_u32(writer, (uint32_t)h264_profile->pictureLayout);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR:
		/* The application's hints about how decoded pictures are used. */
		usage = (const VkVideoDecodeUsageInfoKHR *)record;
		vulkan_write_u32(writer, usage->videoUsageHints);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR:
		/* The count, then every profile as a record of its own. */
		list = (const VkVideoProfileListInfoKHR *)record;
		vulkan_write_u32(writer, list->profileCount);
		vulkan_write_u64(writer, list->profileCount);
		for (index = 0; index < list->profileCount; index++)
			video_encode_struct(writer, (const VkBaseInStructure *)&list->pProfiles[index]);
		break;
	case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR:
		/* The usage the queried pictures would have. */
		format = (const VkPhysicalDeviceVideoFormatInfoKHR *)record;
		vulkan_write_u32(writer, format->imageUsage);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR:
		/* The session's family, profile, formats and limits. */
		video_encode_session_create(writer, (const VkVideoSessionCreateInfoKHR *)record);
		break;
	case VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR:
		/* The binding index and the memory range bound to it. */
		bind = (const VkBindVideoSessionMemoryInfoKHR *)record;
		vulkan_write_u32(writer, bind->memoryBindIndex);
		vulkan_encode_handle(writer, (uint64_t)(uintptr_t)bind->memory);
		vulkan_write_u64(writer, bind->memoryOffset);
		vulkan_write_u64(writer, bind->memorySize);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR:
		/* The flags, the template whose sets are copied, and the session. */
		parameters = (const VkVideoSessionParametersCreateInfoKHR *)record;
		vulkan_write_u32(writer, parameters->flags);
		vulkan_encode_handle(writer, (uint64_t)(uintptr_t)parameters->videoSessionParametersTemplate);
		vulkan_encode_handle(writer, (uint64_t)(uintptr_t)parameters->videoSession);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR:
		/* The capacity of the object, then the sets added at creation, if any. */
		h264_parameters = (const VkVideoDecodeH264SessionParametersCreateInfoKHR *)record;
		vulkan_write_u32(writer, h264_parameters->maxStdSPSCount);
		vulkan_write_u32(writer, h264_parameters->maxStdPPSCount);
		vulkan_write_pointer(writer, h264_parameters->pParametersAddInfo);
		if (h264_parameters->pParametersAddInfo != NULL)
			video_encode_struct(writer, (const VkBaseInStructure *)h264_parameters->pParametersAddInfo);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR:
		/* The sequence and picture parameter sets added. */
		video_encode_parameters_add(writer, (const VkVideoDecodeH264SessionParametersAddInfoKHR *)record);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR:
		/* The sequence number that orders the updates. */
		update = (const VkVideoSessionParametersUpdateInfoKHR *)record;
		vulkan_write_u32(writer, update->updateSequenceCount);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR:
		/* The session, the parameters and the pictures the scope binds. */
		video_encode_begin(writer, (const VkVideoBeginCodingInfoKHR *)record);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR:
		/* The slot index and the picture it holds or is to hold. */
		video_encode_slot(writer, (const VkVideoReferenceSlotInfoKHR *)record);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR:
		/* The image view and the coded area within it. */
		video_encode_resource(writer, (const VkVideoPictureResourceInfoKHR *)record);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR:
		/* The H.264 reference information of the slot's picture. */
		dpb = (const VkVideoDecodeH264DpbSlotInfoKHR *)record;
		vulkan_write_pointer(writer, dpb->pStdReferenceInfo);
		if (dpb->pStdReferenceInfo != NULL)
			video_encode_std_reference(writer, dpb->pStdReferenceInfo);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR:
		/* The end flags, none of which are defined. */
		end = (const VkVideoEndCodingInfoKHR *)record;
		vulkan_write_u32(writer, end->flags);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR:
		/* The control flags, of which reset is the one used by decode. */
		control = (const VkVideoCodingControlInfoKHR *)record;
		vulkan_write_u32(writer, control->flags);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR:
		/* The bitstream range, the output picture and the reference pictures. */
		video_encode_decode(writer, (const VkVideoDecodeInfoKHR *)record);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR:
		/* The H.264 picture information and the slice offsets. */
		video_encode_picture(writer, (const VkVideoDecodeH264PictureInfoKHR *)record);
		break;
	default:
		/* Only known records reach here; another would have no defined fields. */
		writer->error = VK_ERROR_INITIALIZATION_FAILED;
		break;
	}

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the fields of a video session's creation record. */
static void
video_encode_session_create(
	struct vulkan_writer *writer,
	const VkVideoSessionCreateInfoKHR *record)
{
	/* The family the session decodes on and its creation flags. */
	vulkan_write_u32(writer, record->queueFamilyIndex);
	vulkan_write_u32(writer, record->flags);

	/* The profile the session decodes. */
	vulkan_write_pointer(writer, record->pVideoProfile);
	if (record->pVideoProfile != NULL)
		video_encode_struct(writer, (const VkBaseInStructure *)record->pVideoProfile);

	/* The picture formats and the session's limits. */
	vulkan_write_u32(writer, (uint32_t)record->pictureFormat);
	vulkan_encode_VkExtent2D(writer, &record->maxCodedExtent);
	vulkan_write_u32(writer, (uint32_t)record->referencePictureFormat);
	vulkan_write_u32(writer, record->maxDpbSlots);
	vulkan_write_u32(writer, record->maxActiveReferencePictures);

	/* The name and version of the H.264 header the application was built with. */
	vulkan_write_pointer(writer, record->pStdHeaderVersion);
	if (record->pStdHeaderVersion != NULL)
		vulkan_encode_VkExtensionProperties(writer, record->pStdHeaderVersion);

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the parameter sets of one add record. */
static void
video_encode_parameters_add(
	struct vulkan_writer *writer,
	const VkVideoDecodeH264SessionParametersAddInfoKHR *record)
{
	uint32_t index;

	/* The sequence parameter sets, each a standard structure. */
	vulkan_write_u32(writer, record->stdSPSCount);
	vulkan_write_u64(writer, record->stdSPSCount);
	for (index = 0; index < record->stdSPSCount; index++)
		video_encode_sps(writer, &record->pStdSPSs[index]);

	/* The picture parameter sets, each a standard structure. */
	vulkan_write_u32(writer, record->stdPPSCount);
	vulkan_write_u64(writer, record->stdPPSCount);
	for (index = 0; index < record->stdPPSCount; index++)
		video_encode_pps(writer, &record->pStdPPSs[index]);

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the fields of a video coding scope's begin record. */
static void
video_encode_begin(
	struct vulkan_writer *writer,
	const VkVideoBeginCodingInfoKHR *record)
{
	uint32_t index;

	/* The flags, the session and the parameters the scope decodes with. */
	vulkan_write_u32(writer, record->flags);
	vulkan_encode_handle(writer, (uint64_t)(uintptr_t)record->videoSession);
	vulkan_encode_handle(writer, (uint64_t)(uintptr_t)record->videoSessionParameters);

	/*
	 * The bound pictures.  A slot with a picture binds it; a slot index of
	 * -1 with a picture binds a picture without a slot; a slot without a
	 * picture deactivates the slot.
	 */
	vulkan_write_u32(writer, record->referenceSlotCount);
	vulkan_write_u64(writer, record->referenceSlotCount);
	for (index = 0; index < record->referenceSlotCount; index++)
		video_encode_struct(writer, (const VkBaseInStructure *)&record->pReferenceSlots[index]);

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the fields of one reference slot record. */
static void
video_encode_slot(
	struct vulkan_writer *writer,
	const VkVideoReferenceSlotInfoKHR *record)
{
	/* The slot index, which is -1 for a picture without a slot. */
	vulkan_write_u32(writer, (uint32_t)record->slotIndex);

	/* The picture, whose absence deactivates the slot when given to a begin record. */
	vulkan_write_pointer(writer, record->pPictureResource);
	if (record->pPictureResource != NULL)
		video_encode_struct(writer, (const VkBaseInStructure *)record->pPictureResource);

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the fields of one picture resource record. */
static void
video_encode_resource(
	struct vulkan_writer *writer,
	const VkVideoPictureResourceInfoKHR *record)
{
	/* The coded area within the picture. */
	vulkan_encode_VkOffset2D(writer, &record->codedOffset);
	vulkan_encode_VkExtent2D(writer, &record->codedExtent);

	/* The layer and the view that hold the picture. */
	vulkan_write_u32(writer, record->baseArrayLayer);
	vulkan_encode_handle(writer, (uint64_t)(uintptr_t)record->imageViewBinding);

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the fields of one decode record. */
static void
video_encode_decode(
	struct vulkan_writer *writer,
	const VkVideoDecodeInfoKHR *record)
{
	uint32_t index;

	/* The flags and the bitstream range the picture is read from. */
	vulkan_write_u32(writer, record->flags);
	vulkan_encode_handle(writer, (uint64_t)(uintptr_t)record->srcBuffer);
	vulkan_write_u64(writer, record->srcBufferOffset);
	vulkan_write_u64(writer, record->srcBufferRange);

	/* The picture written, a record held by value. */
	video_encode_struct(writer, (const VkBaseInStructure *)&record->dstPictureResource);

	/* The slot the decoded picture becomes a reference in, if any. */
	vulkan_write_pointer(writer, record->pSetupReferenceSlot);
	if (record->pSetupReferenceSlot != NULL)
		video_encode_struct(writer, (const VkBaseInStructure *)record->pSetupReferenceSlot);

	/* The reference pictures the decode reads, each with its H.264 reference information. */
	vulkan_write_u32(writer, record->referenceSlotCount);
	vulkan_write_u64(writer, record->referenceSlotCount);
	for (index = 0; index < record->referenceSlotCount; index++)
		video_encode_struct(writer, (const VkBaseInStructure *)&record->pReferenceSlots[index]);

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the fields of one H.264 picture record. */
static void
video_encode_picture(
	struct vulkan_writer *writer,
	const VkVideoDecodeH264PictureInfoKHR *record)
{
	uint32_t index;

	/* The H.264 picture information. */
	vulkan_write_pointer(writer, record->pStdPictureInfo);
	if (record->pStdPictureInfo != NULL)
		video_encode_std_picture(writer, record->pStdPictureInfo);

	/* The offset of each slice within the bitstream range. */
	vulkan_write_u32(writer, record->sliceCount);
	vulkan_write_u64(writer, record->sliceCount);
	for (index = 0; index < record->sliceCount; index++)
		vulkan_write_u32(writer, record->pSliceOffsets[index]);

	/* Succeeded: the fields are encoded or the writer keeps its first error. */
	return;
}

/* Encodes one H.264 sequence parameter set; its VUI is not sent. */
static void
video_encode_sps(
	struct vulkan_writer *writer,
	const StdVideoH264SequenceParameterSet *record)
{
	uint8_t index;

	/* The flags, profile, level and the sampling of the sequence. */
	vulkan_write_u32(writer, video_sps_flags(&record->flags));
	vulkan_write_u32(writer, (uint32_t)record->profile_idc);
	vulkan_write_u32(writer, (uint32_t)record->level_idc);
	vulkan_write_u32(writer, (uint32_t)record->chroma_format_idc);
	vulkan_write_u32(writer, record->seq_parameter_set_id);
	vulkan_write_u32(writer, record->bit_depth_luma_minus8);
	vulkan_write_u32(writer, record->bit_depth_chroma_minus8);

	/* The frame numbering and the picture order count rules. */
	vulkan_write_u32(writer, record->log2_max_frame_num_minus4);
	vulkan_write_u32(writer, (uint32_t)record->pic_order_cnt_type);
	vulkan_write_u32(writer, (uint32_t)record->offset_for_non_ref_pic);
	vulkan_write_u32(writer, (uint32_t)record->offset_for_top_to_bottom_field);
	vulkan_write_u32(writer, record->log2_max_pic_order_cnt_lsb_minus4);
	vulkan_write_u32(writer, record->num_ref_frames_in_pic_order_cnt_cycle);
	vulkan_write_u32(writer, record->max_num_ref_frames);
	vulkan_write_u32(writer, record->reserved1);

	/* The picture size in macroblocks and the cropping of the output. */
	vulkan_write_u32(writer, record->pic_width_in_mbs_minus1);
	vulkan_write_u32(writer, record->pic_height_in_map_units_minus1);
	vulkan_write_u32(writer, record->frame_crop_left_offset);
	vulkan_write_u32(writer, record->frame_crop_right_offset);
	vulkan_write_u32(writer, record->frame_crop_top_offset);
	vulkan_write_u32(writer, record->frame_crop_bottom_offset);
	vulkan_write_u32(writer, record->reserved2);

	/* The offsets of the picture order count cycle, one for each frame of it. */
	if (record->pOffsetForRefFrame == NULL) {
		vulkan_write_u64(writer, 0);
	} else {
		vulkan_write_u64(writer, record->num_ref_frames_in_pic_order_cnt_cycle);
		for (index = 0; index < record->num_ref_frames_in_pic_order_cnt_cycle; index++)
			vulkan_write_u32(writer, (uint32_t)record->pOffsetForRefFrame[index]);
	}

	/* The sequence's scaling lists, if it has its own. */
	vulkan_write_pointer(writer, record->pScalingLists);
	if (record->pScalingLists != NULL)
		video_encode_scaling(writer, record->pScalingLists);

	/* The VUI does not affect decoding and is never sent. */
	vulkan_write_u64(writer, 0);

	/* Succeeded: the set is encoded or the writer keeps its first error. */
	return;
}

/* Encodes one H.264 picture parameter set. */
static void
video_encode_pps(
	struct vulkan_writer *writer,
	const StdVideoH264PictureParameterSet *record)
{
	/* The flags and the key: the sequence and picture parameter set ids. */
	vulkan_write_u32(writer, video_pps_flags(&record->flags));
	vulkan_write_u32(writer, record->seq_parameter_set_id);
	vulkan_write_u32(writer, record->pic_parameter_set_id);

	/* The default reference counts and the weighted prediction mode. */
	vulkan_write_u32(writer, record->num_ref_idx_l0_default_active_minus1);
	vulkan_write_u32(writer, record->num_ref_idx_l1_default_active_minus1);
	vulkan_write_u32(writer, (uint32_t)record->weighted_bipred_idc);

	/* The signed quantizer offsets, each widened with its sign. */
	vulkan_write_u32(writer, (uint32_t)(int32_t)record->pic_init_qp_minus26);
	vulkan_write_u32(writer, (uint32_t)(int32_t)record->pic_init_qs_minus26);
	vulkan_write_u32(writer, (uint32_t)(int32_t)record->chroma_qp_index_offset);
	vulkan_write_u32(writer, (uint32_t)(int32_t)record->second_chroma_qp_index_offset);

	/* The picture's scaling lists, if it has its own. */
	vulkan_write_pointer(writer, record->pScalingLists);
	if (record->pScalingLists != NULL)
		video_encode_scaling(writer, record->pScalingLists);

	/* Succeeded: the set is encoded or the writer keeps its first error. */
	return;
}

/* Encodes H.264 scaling lists, which stay in their zig-zag scan order. */
static void
video_encode_scaling(
	struct vulkan_writer *writer,
	const StdVideoH264ScalingLists *record)
{
	/* Which lists are present and which of those use the default matrix. */
	vulkan_write_u32(writer, record->scaling_list_present_mask);
	vulkan_write_u32(writer, record->use_default_scaling_matrix_mask);

	/* The six 4x4 lists of sixteen coefficients, list after list. */
	vulkan_write_u64(writer, sizeof(record->ScalingList4x4));
	vulkan_write_bytes(writer, record->ScalingList4x4, sizeof(record->ScalingList4x4));

	/* The six 8x8 lists of sixty-four coefficients, list after list. */
	vulkan_write_u64(writer, sizeof(record->ScalingList8x8));
	vulkan_write_bytes(writer, record->ScalingList8x8, sizeof(record->ScalingList8x8));

	/* Succeeded: the lists are encoded or the writer keeps its first error. */
	return;
}

/* Encodes the H.264 information of the picture being decoded. */
static void
video_encode_std_picture(
	struct vulkan_writer *writer,
	const StdVideoDecodeH264PictureInfo *record)
{
	/* The flags and the parameter sets the picture uses. */
	vulkan_write_u32(writer, video_picture_flags(&record->flags));
	vulkan_write_u32(writer, record->seq_parameter_set_id);
	vulkan_write_u32(writer, record->pic_parameter_set_id);
	vulkan_write_u32(writer, record->reserved1);
	vulkan_write_u32(writer, record->reserved2);

	/* The frame number, the IDR picture id and the two field order counts. */
	vulkan_write_u32(writer, record->frame_num);
	vulkan_write_u32(writer, record->idr_pic_id);
	vulkan_write_u64(writer, STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_LIST_SIZE);
	vulkan_write_u32(writer, (uint32_t)record->PicOrderCnt[0]);
	vulkan_write_u32(writer, (uint32_t)record->PicOrderCnt[1]);

	/* Succeeded: the information is encoded or the writer keeps its first error. */
	return;
}

/* Encodes the H.264 information of one reference picture. */
static void
video_encode_std_reference(
	struct vulkan_writer *writer,
	const StdVideoDecodeH264ReferenceInfo *record)
{
	/* The flags and the frame number of the reference. */
	vulkan_write_u32(writer, video_reference_flags(&record->flags));
	vulkan_write_u32(writer, record->FrameNum);
	vulkan_write_u32(writer, record->reserved);

	/* The two field order counts. */
	vulkan_write_u64(writer, STD_VIDEO_DECODE_H264_FIELD_ORDER_COUNT_LIST_SIZE);
	vulkan_write_u32(writer, (uint32_t)record->PicOrderCnt[0]);
	vulkan_write_u32(writer, (uint32_t)record->PicOrderCnt[1]);

	/* Succeeded: the information is encoded or the writer keeps its first error. */
	return;
}

/* Packs the sequence parameter set flags, the first declared flag in bit 0. */
static uint32_t
video_sps_flags(
	const StdVideoH264SpsFlags *flags)
{
	uint32_t word;

	/* Each one-bit field moves to its position in declaration order. */
	word = 0;
	word |= (uint32_t)flags->constraint_set0_flag << 0;
	word |= (uint32_t)flags->constraint_set1_flag << 1;
	word |= (uint32_t)flags->constraint_set2_flag << 2;
	word |= (uint32_t)flags->constraint_set3_flag << 3;
	word |= (uint32_t)flags->constraint_set4_flag << 4;
	word |= (uint32_t)flags->constraint_set5_flag << 5;
	word |= (uint32_t)flags->direct_8x8_inference_flag << 6;
	word |= (uint32_t)flags->mb_adaptive_frame_field_flag << 7;
	word |= (uint32_t)flags->frame_mbs_only_flag << 8;
	word |= (uint32_t)flags->delta_pic_order_always_zero_flag << 9;
	word |= (uint32_t)flags->separate_colour_plane_flag << 10;
	word |= (uint32_t)flags->gaps_in_frame_num_value_allowed_flag << 11;
	word |= (uint32_t)flags->qpprime_y_zero_transform_bypass_flag << 12;
	word |= (uint32_t)flags->frame_cropping_flag << 13;
	word |= (uint32_t)flags->seq_scaling_matrix_present_flag << 14;
	word |= (uint32_t)flags->vui_parameters_present_flag << 15;

	/* Succeeded: reports the packed word. */
	return word;
}

/* Packs the picture parameter set flags, the first declared flag in bit 0. */
static uint32_t
video_pps_flags(
	const StdVideoH264PpsFlags *flags)
{
	uint32_t word;

	/* Each one-bit field moves to its position in declaration order. */
	word = 0;
	word |= (uint32_t)flags->transform_8x8_mode_flag << 0;
	word |= (uint32_t)flags->redundant_pic_cnt_present_flag << 1;
	word |= (uint32_t)flags->constrained_intra_pred_flag << 2;
	word |= (uint32_t)flags->deblocking_filter_control_present_flag << 3;
	word |= (uint32_t)flags->weighted_pred_flag << 4;
	word |= (uint32_t)flags->bottom_field_pic_order_in_frame_present_flag << 5;
	word |= (uint32_t)flags->entropy_coding_mode_flag << 6;
	word |= (uint32_t)flags->pic_scaling_matrix_present_flag << 7;

	/* Succeeded: reports the packed word. */
	return word;
}

/* Packs the decoded picture's flags, the first declared flag in bit 0. */
static uint32_t
video_picture_flags(
	const StdVideoDecodeH264PictureInfoFlags *flags)
{
	uint32_t word;

	/* Each one-bit field moves to its position in declaration order. */
	word = 0;
	word |= (uint32_t)flags->field_pic_flag << 0;
	word |= (uint32_t)flags->is_intra << 1;
	word |= (uint32_t)flags->IdrPicFlag << 2;
	word |= (uint32_t)flags->bottom_field_flag << 3;
	word |= (uint32_t)flags->is_reference << 4;
	word |= (uint32_t)flags->complementary_field_pair << 5;

	/* Succeeded: reports the packed word. */
	return word;
}

/* Packs a reference picture's flags, the first declared flag in bit 0. */
static uint32_t
video_reference_flags(
	const StdVideoDecodeH264ReferenceInfoFlags *flags)
{
	uint32_t word;

	/* Each one-bit field moves to its position in declaration order. */
	word = 0;
	word |= (uint32_t)flags->top_field_flag << 0;
	word |= (uint32_t)flags->bottom_field_flag << 1;
	word |= (uint32_t)flags->used_for_long_term_reference << 2;
	word |= (uint32_t)flags->is_non_existing << 3;

	/* Succeeded: reports the packed word. */
	return word;
}

/*
 * Encodes the shape of an output record: its type and the types of the
 * records chained to it that the renderer can fill.
 */
static void
video_encode_output_request(
	struct vulkan_writer *writer,
	VkBaseOutStructure *record)
{
	VkBaseOutStructure *next;

	/* The record's own type. */
	vulkan_write_u32(writer, (uint32_t)record->sType);

	/* Finds the next chained record the renderer can fill; the others are skipped. */
	next = record->pNext;
	while (next != NULL &&
	       next->sType != VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR &&
	       next->sType != VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR)
		next = next->pNext;

	/* The end of the chain is an absent link. */
	if (next == NULL) {
		vulkan_write_u64(writer, 0);
		return;
	}

	/* A present link carries the shape of the rest of the chain. */
	vulkan_write_u64(writer, 1);
	video_encode_output_request(writer, next);

	/* Succeeded: the shape is encoded or the writer keeps its first error. */
	return;
}

/* Decodes one output record whose type the reply repeats, chain first. */
static void
video_decode_output(
	struct vulkan_reader *reader,
	VkBaseOutStructure *record)
{
	uint32_t type;

	/* The reply names the record it answers, which must be this one. */
	type = vulkan_read_u32(reader);
	if (reader->error == VK_SUCCESS && type != (uint32_t)record->sType)
		reader->error = VK_ERROR_DEVICE_LOST;

	/* The records chained to this one, then its own fields. */
	video_decode_next(reader, record->pNext);
	video_decode_fields(reader, record);

	/* Succeeded: the record is filled or the reader keeps its first error. */
	return;
}

/* Decodes the chained output records of a reply into the caller's matching records. */
static void
video_decode_next(
	struct vulkan_reader *reader,
	VkBaseOutStructure *chain)
{
	VkBaseOutStructure *record;
	uint64_t present;
	uint32_t type;

	/* An absent link ends the chain. */
	present = vulkan_read_u64(reader);
	if (reader->error != VK_SUCCESS || present == 0)
		return;

	/* The answered record must be one the caller chained after this point. */
	type = vulkan_read_u32(reader);
	record = chain;
	while (record != NULL && (uint32_t)record->sType != type)
		record = record->pNext;

	/* A record the caller did not chain cannot be answered. */
	if (record == NULL) {
		reader->error = VK_ERROR_DEVICE_LOST;
		return;
	}

	/* The record's own chain, then its fields. */
	video_decode_next(reader, record->pNext);
	video_decode_fields(reader, record);

	/* Succeeded: the records are filled or the reader keeps its first error. */
	return;
}

/* Decodes the fields of one known output record, leaving its header untouched. */
static void
video_decode_fields(
	struct vulkan_reader *reader,
	VkBaseOutStructure *record)
{
	VkVideoCapabilitiesKHR *capabilities;
	VkVideoDecodeCapabilitiesKHR *decode;
	VkVideoDecodeH264CapabilitiesKHR *h264;
	VkVideoFormatPropertiesKHR *format;
	VkVideoSessionMemoryRequirementsKHR *binding;

	/* A reply that already failed is not read further. */
	if (reader->error != VK_SUCCESS)
		return;

	/* Decodes each record type with the fields it declares. */
	switch (record->sType) {
	case VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR:
		/* The general limits of a video profile. */
		capabilities = (VkVideoCapabilitiesKHR *)record;
		capabilities->flags = vulkan_read_u32(reader);
		capabilities->minBitstreamBufferOffsetAlignment = vulkan_read_u64(reader);
		capabilities->minBitstreamBufferSizeAlignment = vulkan_read_u64(reader);
		vulkan_decode_VkExtent2D(reader, &capabilities->pictureAccessGranularity);
		vulkan_decode_VkExtent2D(reader, &capabilities->minCodedExtent);
		vulkan_decode_VkExtent2D(reader, &capabilities->maxCodedExtent);
		capabilities->maxDpbSlots = vulkan_read_u32(reader);
		capabilities->maxActiveReferencePictures = vulkan_read_u32(reader);
		vulkan_decode_VkExtensionProperties(reader, &capabilities->stdHeaderVersion);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR:
		/* The decode flags, such as whether output and reference pictures coincide. */
		decode = (VkVideoDecodeCapabilitiesKHR *)record;
		decode->flags = vulkan_read_u32(reader);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR:
		/* The highest H.264 level and the field offset granularity. */
		h264 = (VkVideoDecodeH264CapabilitiesKHR *)record;
		h264->maxLevelIdc = (StdVideoH264LevelIdc)vulkan_read_u32(reader);
		vulkan_decode_VkOffset2D(reader, &h264->fieldOffsetGranularity);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR:
		/* One format a profile decodes into and how its pictures may be created. */
		format = (VkVideoFormatPropertiesKHR *)record;
		format->format = (VkFormat)vulkan_read_u32(reader);
		vulkan_decode_VkComponentMapping(reader, &format->componentMapping);
		format->imageCreateFlags = vulkan_read_u32(reader);
		format->imageType = (VkImageType)vulkan_read_u32(reader);
		format->imageTiling = (VkImageTiling)vulkan_read_u32(reader);
		format->imageUsageFlags = vulkan_read_u32(reader);
		break;
	case VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR:
		/* One binding of a session and the memory it needs. */
		binding = (VkVideoSessionMemoryRequirementsKHR *)record;
		binding->memoryBindIndex = vulkan_read_u32(reader);
		vulkan_decode_VkMemoryRequirements(reader, &binding->memoryRequirements);
		break;
	default:
		/* The renderer answered a record that was never asked for. */
		reader->error = VK_ERROR_DEVICE_LOST;
		break;
	}

	/* Succeeded: the record is filled or the reader keeps its first error. */
	return;
}

/* Reads the count of an enumeration reply, which is bounded by the request. */
static VkResult
video_enumeration_reply(
	struct vulkan_context *context,
	struct vulkan_reader *reader,
	uint32_t *count)
{
	VkBool32 present;
	uint64_t array_count;
	VkResult status;

	/* The count and the number of elements that follow must agree and fit. */
	present = vulkan_reply_pointer(reader);
	*count = vulkan_read_u32(reader);
	array_count = vulkan_read_u64(reader);
	if (present && (array_count != *count || *count > VIDEO_ENUMERATION_MAX))
		reader->error = VK_ERROR_DEVICE_LOST;

	/* A malformed count ends the reply and is terminal for the session. */
	if (reader->error != VK_SUCCESS) {
		status = vulkan_reply_finish(context, reader, VK_SUCCESS);
		return status;
	}

	/* Succeeded: the caller decodes exactly the counted elements. */
	return VK_SUCCESS;
}

/* Converts a parameters handle into its local record, or a null handle into NULL. */
static struct vulkan_video_parameters *
video_parameters_object(
	VkVideoSessionParametersKHR parameters)
{
	struct vulkan_object *object;

	/* Keeps the local pointer bits out of the protocol. */
	object = vulkan_nondispatchable_object((uint64_t)(uintptr_t)parameters);

	/* Succeeded: every parameters record begins with its common ownership object. */
	return (struct vulkan_video_parameters *)object;
}

/*
 * Adds the keys of an add record's parameter sets to a key set.
 *
 * A set whose id lies outside the H.264 range is refused.  When replace is
 * false, a set whose key is already present is refused as well (an update);
 * when it is true, such a set replaces the old one (a creation over a
 * template).  The refusal is VK_ERROR_INITIALIZATION_FAILED: the error made
 * for invalid parameter sets is a beta enumerant of video encode in the
 * pinned header.
 */
static VkResult
video_keys_add(
	uint32_t *sps_keys,
	uint32_t *pps_keys,
	const VkVideoDecodeH264SessionParametersAddInfoKHR *add,
	VkBool32 replace)
{
	uint32_t index;
	uint32_t sps;
	uint32_t key;
	VkBool32 taken;

	/* Takes the key of every sequence parameter set, its seq_parameter_set_id. */
	for (index = 0; index < add->stdSPSCount; index++) {
		sps = add->pStdSPSs[index].seq_parameter_set_id;
		if (sps >= VIDEO_SPS_IDS)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* An update cannot add a set whose key the object already holds. */
		taken = video_key_test(sps_keys, sps);
		if (taken && !replace)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* The object now holds a set under this key. */
		video_key_set(sps_keys, sps);
	}

	/* Takes the key of every picture parameter set, the pair of its two ids. */
	for (index = 0; index < add->stdPPSCount; index++) {
		sps = add->pStdPPSs[index].seq_parameter_set_id;
		if (sps >= VIDEO_SPS_IDS)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* An update cannot add a set whose key the object already holds. */
		key = sps * VIDEO_PPS_IDS + add->pStdPPSs[index].pic_parameter_set_id;
		taken = video_key_test(pps_keys, key);
		if (taken && !replace)
			return VK_ERROR_INITIALIZATION_FAILED;

		/* The object now holds a set under this key. */
		video_key_set(pps_keys, key);
	}

	/* Succeeded: the key set holds every added key. */
	return VK_SUCCESS;
}

/* Tells whether one key is present in a key set. */
static VkBool32
video_key_test(
	const uint32_t *words,
	uint32_t index)
{
	/* Each key is one bit, thirty-two to a word. */
	if ((words[index / 32U] & (1U << (index % 32U))) != 0)
		return VK_TRUE;

	/* The key is absent. */
	return VK_FALSE;
}

/* Adds one key to a key set. */
static void
video_key_set(
	uint32_t *words,
	uint32_t index)
{
	/* Each key is one bit, thirty-two to a word. */
	words[index / 32U] |= 1U << (index % 32U);

	/* Succeeded: the key is present. */
	return;
}

/* Destroys a video session or parameters object on the renderer, then locally. */
static void
video_destroy(
	VkDevice device,
	uint64_t handle,
	uint32_t opcode,
	const VkAllocationCallbacks *allocator)
{
	struct VkDevice_T *owner;
	struct vulkan_object *object;
	VkResult status;

	/* A null handle has nothing to destroy. */
	owner = vulkan_device(device);
	object = vulkan_nondispatchable_object(handle);
	if (object == NULL)
		return;

	/* The destruction command and the final free both use the caller's callbacks. */
	if (allocator != NULL) {
		object->allocator.callbacks = *allocator;
		object->allocator.has_callbacks = VK_TRUE;
	}

	/*
	 * Withdraws the renderer object.  A destroy returns nothing, so a failure
	 * here is left for the session's next result-bearing call to report.
	 */
	status = vulkan_object_destroy_remote(owner, object, opcode);
	(void)status;
	vulkan_object_free(object);

	/* Succeeded: no public handle refers to the released record. */
	return;
}
