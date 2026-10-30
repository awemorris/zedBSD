/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The zedBSD GPU command protocol, version 1: the numbers of
 * the Vulkan commands in the stream libvulkan submits to a GPU node
 * (GPU_COMMAND, GPU_COMMAND_SUBMIT in uapi/gpu.h), which the i915 Vulkan
 * executor and the bcm2711 (V3D) Vulkan executor read and a virtual
 * machine's host renders.
 *
 * The numbers of version 1 reuse those of the Venus protocol's wire
 * format 1 (virglrenderer 1.1.0) for the same Vulkan commands, so that a
 * Venus renderer on a virtual machine's host accepts the stream as it
 * is.  The names, this file and every later number are zedBSD's own; a
 * command of zedBSD's alone is numbered from GPU_OP_OWN_FIRST, which a
 * Venus host is never sent.
 *
 * What this file is.  The command numbers were collected with the
 * venus-protocol XML registry as the primary source, and everything else
 * written here -- what each command is used for, the order of its
 * arguments and the shape of its reply, which zedBSD function sends it
 * and which decodes it, the order it must keep, and which commands zedBSD
 * never sends -- was found in zedBSD's own implementation and written
 * independently.  This file is a reference written for zedBSD, not an
 * adaptation of the Venus headers, and is meant to be usable on its own
 * by the developer of another system who has to speak the same stream.
 *
 * Source of the reused values (cited, not copied): the venus-protocol
 * repository, commit ca19b6358d7cc491bc3e4de76f04c6700876a8fa (the
 * generator revision git-ca1e9220 that virglrenderer 1.1.0's headers
 * name).  Every command number and the reply flag are those of
 * xmls/VK_EXT_command_serialization.xml; the arguments of the commands
 * 178 to 180 and the two record types are those of
 * xmls/VK_MESA_venus_protocol.xml.  Each entry below names the line of
 * VK_EXT_command_serialization.xml that holds its number
 * (VK_COMMAND_TYPE_<vulkan command>_EXT); the 145 numbers 0 to 180 were
 * compared with that file one by one.
 *
 * Where the code is.  Paths in the entries are short:
 *
 *   libvulkan/  userland/desktop/libvulkan/, the sender;
 *   i915/       src/drivers/gpu/i915/vk/, the native i915 executor;
 *   bcm2711/    src/drivers/gpu/bcm2711/, the native V3D executor;
 *   venus/      src/drivers/gpu/venus/, the virtual machine's driver.
 *
 * The entry "Sent by" names the libvulkan function that encodes the
 * command (and the public call that reaches it); "Decoded by" names the
 * dispatcher and the handler of each native executor.  The test client
 * userland/tests/venus-frame/ also sends a subset of the commands
 * directly and is not listed.
 *
 * The wire form.  Every value is little-endian.  A u32 is 4 bytes, a u64
 * 8 bytes; enums, flags, counts and indices are u32, sizes and offsets
 * (VkDeviceSize) u64, floats their 32 IEEE bits.  An object is named by a
 * u64 identity that libvulkan chooses (libvulkan/objects.c:
 * vulkan_object_reserve_id, never reused in a session) and the executor
 * adopts; the null handle is 0.  A pointer argument is a u64 presence
 * word (1 or 0) followed by what it points to.  An array argument is its
 * u32 count parameter, then a u64 element count and the elements.  A
 * structure is its fields in declaration order: sType as a u32, pNext as
 * a u64 presence word followed by any chained record, then the rest
 * (libvulkan/codec.c: vulkan_encode_<type>, libvulkan/wire.c).
 *
 * The command header.  Every command starts [u32 number][u32 flags].
 * The flags are GPU_OP_FLAG_REPLY for a command that wants its reply
 * (libvulkan/wire.c: vulkan_command_begin) and 0 for a recorded vkCmd*
 * and for the transport commands 178 to 180.  A command carries no
 * length, so an executor that does not know a number cannot find the
 * next command: it stops the stream there.
 *
 * The reply.  A command that asked for a reply gets, at the reply
 * cursor, [u32 its number] (libvulkan/wire.c: vulkan_command_execute
 * refuses any other), then [u32 VkResult] when the Vulkan command returns
 * one, then its outputs.  A void command's reply is the echoed number
 * alone.
 *
 * Two forms recur and are named in the entries:
 *
 *   the create form: [device][u64 1][the create info][u64 0, no
 *   pAllocator][u64 1][the new object's identity], reply [number]
 *   [VkResult][u64 1][the same identity] (libvulkan/objects.c:
 *   vulkan_object_create_complete, which refuses a different identity
 *   and destroys the object again if it cannot publish it);
 *
 *   the destroy form: [device][the object's identity][u64 0, no
 *   pAllocator], reply [number] (libvulkan/objects.c:
 *   vulkan_object_destroy_remote).
 *
 * One submission.  libvulkan/context.c: vulkan_context_transaction
 * frames every command it sends as
 *
 *   GPU_OP_SET_REPLY_STREAM (178)   the replies go to a session blob;
 *   the command, or GPU_OP_EXECUTE_STREAMS (180) naming it;
 *   GPU_OP_SEEK_REPLY_STREAM (179)  to the blob's last 20 bytes;
 *   GPU_OP_ENUMERATE_INSTANCE_VERSION (137)  the completion trailer,
 *
 * and polls the trailer's last word (libvulkan/context.c:
 * vulkan_context_poll).  Only that one command asks for a reply, except
 * that the recorded vkCmd* travel together with the vkEndCommandBuffer
 * that asks for its own (GPU_OP_END_COMMAND_BUFFER).
 *
 * The executors.  On i915 the stream reaches i915/transport.c:
 * drv_i915_render_transport_execute and each command
 * i915/dispatch.c: drv_i915_render_dispatch, which offers it to the
 * object commands (i915/objects.c: drv_i915_gfx_obj_dispatch), then to
 * the command buffers and recordings (i915/command.c:
 * drv_i915_gfx_rec_dispatch), then routes it by its range (fences and
 * queries to i915/fence.c, video to i915/video.c), and offers the rest
 * to the transport and to i915/instance.c.  A number none of them owns
 * is refused.  On bcm2711 the stream reaches bcm2711/vulkan-stream.c:
 * execute_command, which runs the transport commands itself and hands
 * every other one to the routes of bcm2711/vulkan-dispatch.c:
 * bcm2711_vulkan_dispatch; a number no route owns is refused (ENOTSUP).
 * A refused command stops the stream before the trailer, which
 * libvulkan reports as VK_ERROR_DEVICE_LOST for the whole session.  On a
 * virtual machine venus/venus.c: venus_command_submit passes the stream
 * unread to the host (virtio-gpu SUBMIT_3D, venus/transport.c), whose
 * Venus renderer decodes it; no zedBSD code decodes it there.
 */

#ifndef UAPI_GPU_OP_H
#define UAPI_GPU_OP_H

/*
 * The protocol's version, and the first number of zedBSD's own commands.
 * Version 2 adds the video commands; version 1's numbers are
 * unchanged, and only a backend whose capset declares video is sent the
 * new ones.  The version records the protocol's revision; no code
 * compares it.  i915/dispatch.c routes GPU_OP_OWN_FIRST upward to the
 * video part.
 */
#define GPU_OP_PROTOCOL_VERSION	2U
#define GPU_OP_OWN_FIRST	0x10000U

/*
 * The flag word that follows each command's number: GPU_OP_FLAG_REPLY asks
 * the renderer to write the command's reply (bit 0 of the command flags,
 * VK_COMMAND_GENERATE_REPLY_BIT_EXT in VK_EXT_command_serialization.xml).
 * libvulkan/wire.c: vulkan_command_begin sets it on every command that is
 * not a recording or a transport command; bcm2711/vulkan-stream.c:
 * execute_command refuses any other value than 0 or 1.
 */
#define GPU_OP_FLAG_REPLY	1U

/*
 * The structure types of the two private records a command may chain:
 * GPU_OP_RECORD_IMPORT_RESOURCE, followed by the u32 id of a resource the
 * context imported, makes vkAllocateMemory import that resource;
 * GPU_OP_RECORD_QUEUE_TIMELINE, followed by a u32 ring index, binds the
 * queue vkGetDeviceQueue2 returns to that timeline.  The values are offsets
 * 2 and 5 of extension 385 in VK_MESA_venus_protocol.xml
 * (VkImportMemoryResourceInfoMESA, VkDeviceQueueTimelineInfoMESA).
 *
 * The import record is chained by libvulkan/memory.c: memory_allocate
 * after the resource was attached to the context (on a virtual machine
 * venus/share.c: share_context_add), and is read by i915/memory.c:
 * drv_i915_gfx_allocate_memory and bcm2711/vulkan-memory.c:
 * allocate_memory.  The timeline record is chained by libvulkan/device.c:
 * device_queue_remote; bcm2711/vulkan-device.c: device_queue requires it,
 * and i915/instance.c: i915_instance_get_device_queue2 skips it (every
 * timeline runs on one ring there).
 */
#define GPU_OP_RECORD_IMPORT_RESOURCE	1000384002U
#define GPU_OP_RECORD_QUEUE_TIMELINE	1000384005U

/*
 * Each command of the protocol, in number order.  The entry of each
 * names the Vulkan command it carries, where its number is defined, what
 * it does in zedBSD, its arguments after the header and its reply, and
 * who sends and decodes it.
 */
enum gpu_op {
	/*
	 * vkCreateInstance: creates the renderer's instance in one session.
	 * Number: VK_EXT_command_serialization.xml:24.
	 * Arguments: [u64 1][VkInstanceCreateInfo][u64 0][u64 1][instance].
	 * Reply: [number][VkResult][u64 1][instance].
	 * Sent by: libvulkan/instance.c: instance_create_remote, from
	 * vkCreateInstance, once for every GPU node session it opens.
	 * Decoded by: i915/instance.c: drv_i915_render_instance_dispatch ->
	 * i915_instance_create; bcm2711/vulkan-device.c:
	 * bcm2711_vulkan_device_dispatch -> create_instance.
	 * Order: the first command of a session; GPU_OP_ENUMERATE_PHYSICAL_DEVICES
	 * follows at once.
	 */
	GPU_OP_CREATE_INSTANCE = 0,

	/*
	 * vkDestroyInstance: forgets the renderer's instance.
	 * Number: VK_EXT_command_serialization.xml:25.
	 * Arguments (as the executors decode it): [instance][u64 0].
	 * Reply: [number].
	 * Sent by: none.  vkDestroyInstance closes each session instead
	 * (libvulkan/instance.c: instance_finish -> libvulkan/context.c:
	 * vulkan_context_close), which retires the session's renderer objects.
	 * Decoded by: i915/instance.c: i915_instance_destroy;
	 * bcm2711/vulkan-device.c: destroy_root, which also retires the
	 * physical devices under it.
	 */
	GPU_OP_DESTROY_INSTANCE = 1,

	/*
	 * vkEnumeratePhysicalDevices: names the renderer's physical devices.
	 * Number: VK_EXT_command_serialization.xml:26.
	 * Arguments: [instance][u64 1][u32 count][u64 count][count x id].  The
	 * first call sends count 0 and no identities; the second sends the
	 * identities libvulkan chose for the devices.
	 * Reply: [number][VkResult][u64 1][u32 count][u64 count][count x id],
	 * the identities echoed in the same order (none in the first reply).
	 * Sent by: libvulkan/instance.c: instance_enumerate_remote, twice,
	 * from vkCreateInstance.
	 * Decoded by: i915/instance.c: i915_instance_enumerate_physical_devices;
	 * bcm2711/vulkan-device.c: enumerate_physical.
	 * Order: after GPU_OP_CREATE_INSTANCE; then, for each device,
	 * GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES, _FEATURES, _MEMORY_PROPERTIES,
	 * _QUEUE_FAMILY_PROPERTIES (twice) and, on a video renderer,
	 * GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES.
	 */
	GPU_OP_ENUMERATE_PHYSICAL_DEVICES = 2,

	/*
	 * vkGetPhysicalDeviceFeatures: reads the device's core features.
	 * Number: VK_EXT_command_serialization.xml:27.
	 * Arguments: [physical device][u64 1].
	 * Reply: [number][u64 1][VkPhysicalDeviceFeatures] (no VkResult).
	 * Sent by: libvulkan/instance.c: physical_load -> physical_query_begin,
	 * once at discovery; the answer is cached for vkGetPhysicalDeviceFeatures.
	 * Decoded by: i915/instance.c: i915_instance_features;
	 * bcm2711/vulkan-query.c: bcm2711_vulkan_query_dispatch -> fixed_query,
	 * which claims no optional feature.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_FEATURES = 3,

	/*
	 * vkGetPhysicalDeviceFormatProperties: reads one format's features.
	 * Number: VK_EXT_command_serialization.xml:28.
	 * Arguments: [physical device][u32 format][u64 1].
	 * Reply: [number][u64 1][VkFormatProperties] (no VkResult).
	 * Sent by: libvulkan/instance.c: vkGetPhysicalDeviceFormatProperties,
	 * on every call; without the video capset it clears the video format
	 * features of the answer.
	 * Decoded by: i915/instance.c: i915_instance_format_properties;
	 * bcm2711/vulkan-query.c: format_query.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_FORMAT_PROPERTIES = 4,

	/*
	 * vkGetPhysicalDeviceImageFormatProperties: asks whether an image of
	 * one format, type, tiling, usage and flags can be made, and its limits.
	 * Number: VK_EXT_command_serialization.xml:29.
	 * Arguments: [physical device][u32 format][u32 type][u32 tiling]
	 * [u32 usage][u32 flags][u64 1].
	 * Reply: [number][VkResult][u64 1][VkImageFormatProperties].
	 * Sent by: libvulkan/instance.c: vkGetPhysicalDeviceImageFormatProperties.
	 * Decoded by: i915/instance.c: i915_instance_image_format_properties;
	 * bcm2711/vulkan-query.c: image_query.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES = 5,

	/*
	 * vkGetPhysicalDeviceProperties: reads the device's identity and limits.
	 * Number: VK_EXT_command_serialization.xml:30.
	 * Arguments: [physical device][u64 1].
	 * Reply: [number][u64 1][VkPhysicalDeviceProperties] (no VkResult).
	 * Sent by: libvulkan/instance.c: physical_load -> physical_query_begin,
	 * the first query of discovery.  A device whose apiVersion is below
	 * 1.1.0 is refused (VK_ERROR_INCOMPATIBLE_DRIVER); the cached copy then
	 * reports 1.0 and a minMemoryMapAlignment of 64.
	 * Decoded by: i915/instance.c: i915_instance_properties;
	 * bcm2711/vulkan-query.c: fixed_query.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES = 6,

	/*
	 * vkGetPhysicalDeviceQueueFamilyProperties: lists the queue families.
	 * Number: VK_EXT_command_serialization.xml:31.
	 * Arguments: [physical device][u64 1][u32 count][u64 count]; the first
	 * call sends count 0, the second the count the first returned.
	 * Reply: [number][u64 1][u32 count][u64 count]
	 * [count x VkQueueFamilyProperties] (no VkResult; no array in the first).
	 * Sent by: libvulkan/instance.c: physical_load_queues, twice at
	 * discovery; a count of 0 is refused.
	 * Decoded by: i915/instance.c: i915_instance_queue_families;
	 * bcm2711/vulkan-query.c: queue_properties.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_PROPERTIES = 7,

	/*
	 * vkGetPhysicalDeviceMemoryProperties: lists the memory types and heaps.
	 * Number: VK_EXT_command_serialization.xml:32.
	 * Arguments: [physical device][u64 1][u64 32][u64 16], the fixed
	 * extents VK_MAX_MEMORY_TYPES and VK_MAX_MEMORY_HEAPS of the arrays.
	 * Reply: [number][u64 1][VkPhysicalDeviceMemoryProperties] (no VkResult).
	 * Sent by: libvulkan/instance.c: physical_load -> physical_query_begin,
	 * once at discovery; the type indices are used as the renderer numbers
	 * them.
	 * Decoded by: i915/instance.c: i915_instance_memory_properties;
	 * bcm2711/vulkan-query.c: fixed_query, which refuses other extents.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_MEMORY_PROPERTIES = 8,

	/*
	 * vkGetInstanceProcAddr.
	 * Number: VK_EXT_command_serialization.xml:33.
	 * Not used by zedBSD: libvulkan/dispatch.c: vkGetInstanceProcAddr
	 * answers from its own table, and no executor decodes the number.
	 */
	GPU_OP_GET_INSTANCE_PROC_ADDR = 9,

	/*
	 * vkGetDeviceProcAddr.
	 * Number: VK_EXT_command_serialization.xml:34.
	 * Not used by zedBSD: libvulkan/dispatch.c: vkGetDeviceProcAddr
	 * answers from its own table, and no executor decodes the number.
	 */
	GPU_OP_GET_DEVICE_PROC_ADDR = 10,

	/*
	 * vkCreateDevice: creates the logical device with its queues.
	 * Number: VK_EXT_command_serialization.xml:35.
	 * Arguments: [physical device][u64 1][VkDeviceCreateInfo][u64 0][u64 1]
	 * [device].
	 * Reply: [number][VkResult][u64 1][device].
	 * Sent by: libvulkan/device.c: device_create_remote, from vkCreateDevice,
	 * with the application's features, queue families and priorities.
	 * Decoded by: i915/instance.c: i915_instance_create_device;
	 * bcm2711/vulkan-device.c: create_device.
	 * Order: GPU_OP_GET_DEVICE_QUEUE2 follows for every queue created.
	 */
	GPU_OP_CREATE_DEVICE = 11,

	/*
	 * vkDestroyDevice: forgets the logical device.
	 * Number: VK_EXT_command_serialization.xml:36.
	 * Arguments: [device][u64 0].
	 * Reply: [number].
	 * Sent by: libvulkan/device.c: device_destroy_remote, from vkDestroyDevice.
	 * Decoded by: i915/instance.c: i915_instance_destroy;
	 * bcm2711/vulkan-device.c: destroy_root, which also retires the
	 * device's queues.
	 */
	GPU_OP_DESTROY_DEVICE = 12,

	/*
	 * vkEnumerateInstanceExtensionProperties.
	 * Number: VK_EXT_command_serialization.xml:37.
	 * Not used by zedBSD: libvulkan/instance.c lists the extensions it
	 * implements itself, and no executor decodes the number.
	 */
	GPU_OP_ENUMERATE_INSTANCE_EXTENSION_PROPERTIES = 13,

	/*
	 * vkEnumerateDeviceExtensionProperties.
	 * Number: VK_EXT_command_serialization.xml:38.
	 * Not used by zedBSD: libvulkan/instance.c lists the device extensions
	 * it implements (the video ones only on a video renderer), and no
	 * executor decodes the number.
	 */
	GPU_OP_ENUMERATE_DEVICE_EXTENSION_PROPERTIES = 14,

	/*
	 * vkEnumerateInstanceLayerProperties.
	 * Number: VK_EXT_command_serialization.xml:39.
	 * Not used by zedBSD: libvulkan/instance.c reports no layer, and no
	 * executor decodes the number.
	 */
	GPU_OP_ENUMERATE_INSTANCE_LAYER_PROPERTIES = 15,

	/*
	 * vkEnumerateDeviceLayerProperties.
	 * Number: VK_EXT_command_serialization.xml:40.
	 * Not used by zedBSD: libvulkan/instance.c reports no layer, and no
	 * executor decodes the number.
	 */
	GPU_OP_ENUMERATE_DEVICE_LAYER_PROPERTIES = 16,

	/*
	 * vkGetDeviceQueue.
	 * Number: VK_EXT_command_serialization.xml:41.
	 * Not used by zedBSD: every queue is fetched once with
	 * GPU_OP_GET_DEVICE_QUEUE2 at vkCreateDevice, and libvulkan/device.c:
	 * vkGetDeviceQueue returns that cached queue.  No executor decodes it.
	 */
	GPU_OP_GET_DEVICE_QUEUE = 17,

	/*
	 * vkQueueSubmit: hands command buffers and semaphores to a queue.
	 * Number: VK_EXT_command_serialization.xml:42.
	 * Arguments: [queue][u32 count][u64 count][count x VkSubmitInfo][fence],
	 * each VkSubmitInfo with its wait semaphores and stages, its command
	 * buffers' identities and its signal semaphores; the fence is 0 for none.
	 * Reply: [number][VkResult].  VK_SUCCESS means the executor accepted the
	 * work, not that it finished; completion is the fence's.
	 * Sent by: libvulkan/queue.c: queue_attempt, from vkQueueSubmit and
	 * from the empty submission of vulkan_queue_idle.
	 * Before it, a kernel job is reserved for the fence (libvulkan/sync.c:
	 * vulkan_sync_job_reserve, GPU_JOB_RESERVE); a wait on a semaphore that
	 * is already signaled in software is left out (queue_write_waits).
	 * Decoded by: i915/command.c: drv_i915_gfx_rec_dispatch ->
	 * i915_queue_submit; bcm2711/vulkan-queue.c:
	 * bcm2711_vulkan_queue_dispatch (check_admission, decode_submission).
	 * Order: each command buffer must have ended (GPU_OP_END_COMMAND_BUFFER).
	 */
	GPU_OP_QUEUE_SUBMIT = 18,

	/*
	 * vkQueueWaitIdle.
	 * Number: VK_EXT_command_serialization.xml:43.
	 * Arguments (as i915 decodes it): [queue].  Reply: [number][VkResult].
	 * Sent by: none.  libvulkan/queue.c: vulkan_queue_idle submits an empty
	 * batch with a private fence and waits for the fence instead.
	 * Decoded by: i915/instance.c: i915_instance_wait_idle, which answers
	 * VK_SUCCESS at once because its worker runs the requests in order;
	 * bcm2711/vulkan-device.c: bcm2711_vulkan_device_dispatch refuses it
	 * (EOPNOTSUPP).
	 */
	GPU_OP_QUEUE_WAIT_IDLE = 19,

	/*
	 * vkDeviceWaitIdle.
	 * Number: VK_EXT_command_serialization.xml:44.
	 * Arguments (as i915 decodes it): [device].  Reply: [number][VkResult].
	 * Sent by: none.  libvulkan/queue.c: vkDeviceWaitIdle waits on each
	 * queue with vulkan_queue_idle instead.
	 * Decoded by: as GPU_OP_QUEUE_WAIT_IDLE (i915 answers, bcm2711 refuses).
	 */
	GPU_OP_DEVICE_WAIT_IDLE = 20,

	/*
	 * vkAllocateMemory: allocates, exports or imports device memory.
	 * Number: VK_EXT_command_serialization.xml:45.
	 * Arguments: [device][u64 1][u32 VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO]
	 * [u64 chain present], then, when present, one record [u32 sType]
	 * [u64 0][u32 value] -- GPU_OP_RECORD_IMPORT_RESOURCE with a resource id,
	 * or VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO with the context's
	 * native handle type -- then [u64 size][u32 memory type][u64 0][u64 1]
	 * [memory].  The size is the page-rounded native size.
	 * Reply: [number][VkResult][u64 1][memory].
	 * Sent by: libvulkan/memory.c: memory_allocate, from vkAllocateMemory
	 * and libvulkan's own shared, placed and imported allocations
	 * (vulkan_memory_allocate, vulkan_memory_allocate_placed,
	 * vulkan_memory_import).
	 * Decoded by: i915/objects.c: drv_i915_gfx_obj_dispatch ->
	 * i915/memory.c: drv_i915_gfx_allocate_memory;
	 * bcm2711/vulkan-memory.c: bcm2711_vulkan_memory_dispatch ->
	 * allocate_memory.
	 */
	GPU_OP_ALLOCATE_MEMORY = 21,

	/*
	 * vkFreeMemory: frees an allocation.
	 * Number: VK_EXT_command_serialization.xml:46.
	 * Arguments: [device][memory][u64 0].  Reply: [number].
	 * Sent by: libvulkan/memory.c: memory_native_free, from vkFreeMemory.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_free_memory;
	 * bcm2711/vulkan-memory.c: free_memory.
	 */
	GPU_OP_FREE_MEMORY = 22,

	/*
	 * vkMapMemory.
	 * Number: VK_EXT_command_serialization.xml:47.
	 * Not used by zedBSD: libvulkan/memory.c: vkMapMemory returns the CPU
	 * view of the allocation's shared storage without a command, and no
	 * executor decodes the number.
	 */
	GPU_OP_MAP_MEMORY = 23,

	/*
	 * vkUnmapMemory.
	 * Number: VK_EXT_command_serialization.xml:48.
	 * Not used by zedBSD: libvulkan/memory.c: vkUnmapMemory retires the
	 * CPU view locally, and no executor decodes the number.
	 */
	GPU_OP_UNMAP_MEMORY = 24,

	/*
	 * vkFlushMappedMemoryRanges.
	 * Number: VK_EXT_command_serialization.xml:49.
	 * Not used by zedBSD: the mapped memory is coherent, and
	 * libvulkan/memory.c: vkFlushMappedMemoryRanges only checks the ranges
	 * and orders the CPU's stores.  No executor decodes the number.
	 */
	GPU_OP_FLUSH_MAPPED_MEMORY_RANGES = 25,

	/*
	 * vkInvalidateMappedMemoryRanges.
	 * Number: VK_EXT_command_serialization.xml:50.
	 * Not used by zedBSD: as GPU_OP_FLUSH_MAPPED_MEMORY_RANGES
	 * (libvulkan/memory.c: vkInvalidateMappedMemoryRanges).
	 */
	GPU_OP_INVALIDATE_MAPPED_MEMORY_RANGES = 26,

	/*
	 * vkGetDeviceMemoryCommitment: reads how much of a lazily allocated
	 * memory object is backed.
	 * Number: VK_EXT_command_serialization.xml:51.
	 * Arguments: [device][memory][u64 1].
	 * Reply: [number][u64 1][u64 bytes].
	 * Sent by: libvulkan/memory.c: vkGetDeviceMemoryCommitment.
	 * Decoded by: neither native executor (i915 refuses it, bcm2711 has
	 * no route); only a Venus host answers it.
	 */
	GPU_OP_GET_DEVICE_MEMORY_COMMITMENT = 27,

	/*
	 * vkBindBufferMemory: binds a buffer to memory at an offset.
	 * Number: VK_EXT_command_serialization.xml:52.
	 * Arguments: [device][buffer][memory][u64 offset].
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/resources.c: resource_bind, from vkBindBufferMemory;
	 * memory imported for one image refuses any other resource or a
	 * non-zero offset locally (VK_ERROR_OUT_OF_DEVICE_MEMORY).
	 * Decoded by: i915/objects.c -> drv_i915_gfx_bind;
	 * bcm2711/vulkan-resource.c: bcm2711_vulkan_resource_dispatch ->
	 * bind_memory.
	 */
	GPU_OP_BIND_BUFFER_MEMORY = 28,

	/*
	 * vkBindImageMemory: binds an image to memory at an offset.
	 * Number: VK_EXT_command_serialization.xml:53.
	 * Arguments: [device][image][memory][u64 offset].
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/resources.c: resource_bind, from vkBindImageMemory
	 * (the same local check as GPU_OP_BIND_BUFFER_MEMORY).
	 * Decoded by: i915/objects.c -> drv_i915_gfx_bind;
	 * bcm2711/vulkan-resource.c: bind_memory.
	 */
	GPU_OP_BIND_IMAGE_MEMORY = 29,

	/*
	 * vkGetBufferMemoryRequirements: reads a buffer's size, alignment and
	 * allowed memory types.
	 * Number: VK_EXT_command_serialization.xml:54.
	 * Arguments: [device][buffer][u64 1].
	 * Reply: [number][u64 1][VkMemoryRequirements] (no VkResult).
	 * Sent by: libvulkan/resources.c: resource_requirements, from
	 * vkGetBufferMemoryRequirements; a malformed answer gives all zeros.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_requirements;
	 * bcm2711/vulkan-resource.c: memory_requirements.
	 */
	GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS = 30,

	/*
	 * vkGetImageMemoryRequirements: as GPU_OP_GET_BUFFER_MEMORY_REQUIREMENTS
	 * for an image.
	 * Number: VK_EXT_command_serialization.xml:55.
	 * Arguments: [device][image][u64 1].
	 * Reply: [number][u64 1][VkMemoryRequirements].
	 * Sent by: libvulkan/resources.c: resource_requirements, from
	 * vkGetImageMemoryRequirements.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_requirements;
	 * bcm2711/vulkan-resource.c: memory_requirements.
	 */
	GPU_OP_GET_IMAGE_MEMORY_REQUIREMENTS = 31,

	/*
	 * vkGetImageSparseMemoryRequirements: reads an image's sparse binding
	 * requirements.
	 * Number: VK_EXT_command_serialization.xml:56.
	 * Arguments: [device][image][u64 1][u32 capacity][u64 capacity].
	 * Reply: [number][u64 1][u32 count][u64 count]
	 * [count x VkSparseImageMemoryRequirements].
	 * Sent by: libvulkan/resources.c: vkGetImageSparseMemoryRequirements.
	 * Decoded by: neither native executor; only a Venus host answers it.
	 */
	GPU_OP_GET_IMAGE_SPARSE_MEMORY_REQUIREMENTS = 32,

	/*
	 * vkGetPhysicalDeviceSparseImageFormatProperties: reads the sparse
	 * image formats of one format, type, sample count, usage and tiling.
	 * Number: VK_EXT_command_serialization.xml:57.
	 * Arguments: [physical device][u32 format][u32 type][u32 samples]
	 * [u32 usage][u32 tiling][u64 1][u32 capacity][u64 capacity].
	 * Reply: [number][u64 1][u32 count][u64 count]
	 * [count x VkSparseImageFormatProperties].
	 * Sent by: libvulkan/resources.c:
	 * vkGetPhysicalDeviceSparseImageFormatProperties.
	 * Decoded by: neither native executor; only a Venus host answers it.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_PROPERTIES = 33,

	/*
	 * vkQueueBindSparse: binds sparse memory on a queue.
	 * Number: VK_EXT_command_serialization.xml:58.
	 * Arguments: [queue][u32 count][u64 count][count x VkBindSparseInfo]
	 * [fence], with the same count and semaphore layout as
	 * GPU_OP_QUEUE_SUBMIT.
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/queue.c: queue_attempt, from vkQueueBindSparse.
	 * Decoded by: neither native executor; only a Venus host answers it.
	 */
	GPU_OP_QUEUE_BIND_SPARSE = 34,

	/*
	 * vkCreateFence: creates a fence, signaled or not.
	 * Number: VK_EXT_command_serialization.xml:59.
	 * Arguments: the create form with [u32 VK_STRUCTURE_TYPE_FENCE_CREATE_INFO]
	 * [u64 0][u32 flags] as the create info.
	 * Reply: [number][VkResult][u64 1][fence].
	 * Sent by: libvulkan/sync.c: sync_create, from vkCreateFence.
	 * Decoded by: i915/fence.c: drv_i915_render_fence_dispatch ->
	 * i915_fence_create; bcm2711/vulkan-sync.c: bcm2711_vulkan_sync_dispatch
	 * -> create_sync.
	 */
	GPU_OP_CREATE_FENCE = 35,

	/*
	 * vkDestroyFence.
	 * Number: VK_EXT_command_serialization.xml:60.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/sync.c: sync_destroy, from vkDestroyFence and when
	 * vkCreateFence fails after the fence was made.
	 * Decoded by: i915/fence.c: i915_fence_destroy; bcm2711/vulkan-sync.c:
	 * destroy_sync.
	 */
	GPU_OP_DESTROY_FENCE = 36,

	/*
	 * vkResetFences: returns fences to the unsignaled state.
	 * Number: VK_EXT_command_serialization.xml:61.
	 * Arguments: [device][u32 count][u64 count][count x fence].
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/sync.c: vkResetFences, and
	 * libvulkan/external-fence.c: vulkan_external_fence_prepare_locked
	 * before an exported fence is submitted again (an imported alias may
	 * have reset the shared payload); a fence never submitted since its
	 * unsignaled creation is not reset.
	 * Decoded by: i915/fence.c: i915_fence_reset; bcm2711/vulkan-sync.c:
	 * reset_fences.
	 */
	GPU_OP_RESET_FENCES = 37,

	/*
	 * vkGetFenceStatus: reads whether a fence is signaled.
	 * Number: VK_EXT_command_serialization.xml:62.
	 * Arguments: [device][fence].
	 * Reply: [number][VkResult], VK_SUCCESS for signaled and VK_NOT_READY
	 * for not yet.
	 * Sent by: libvulkan/sync.c: vulkan_sync_status_locked, from
	 * vkGetFenceStatus (when no local state already answers) and from
	 * libvulkan/external-fence.c: vkImportFenceFdKHR.
	 * Decoded by: i915/fence.c: i915_fence_status; bcm2711/vulkan-sync.c:
	 * fence_status.
	 */
	GPU_OP_GET_FENCE_STATUS = 38,

	/*
	 * vkWaitForFences.
	 * Number: VK_EXT_command_serialization.xml:63.
	 * Not used by zedBSD: libvulkan/sync.c: vulkan_fences_wait sleeps on
	 * the kernel's job notifications (the sequence GPU_JOB_RESERVE gave
	 * the fence's submission) instead.  i915/fence.c refuses the
	 * number and bcm2711 has no route for it.
	 */
	GPU_OP_WAIT_FOR_FENCES = 39,

	/*
	 * vkCreateSemaphore: creates a binary semaphore.
	 * Number: VK_EXT_command_serialization.xml:64.
	 * Arguments: the create form with
	 * [u32 VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO][u64 0][u32 flags].
	 * Reply: [number][VkResult][u64 1][semaphore].
	 * Sent by: libvulkan/sync.c: sync_create, from vkCreateSemaphore.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_semaphore;
	 * bcm2711/vulkan-sync.c: create_sync.
	 */
	GPU_OP_CREATE_SEMAPHORE = 40,

	/*
	 * vkDestroySemaphore.
	 * Number: VK_EXT_command_serialization.xml:65.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/sync.c: sync_destroy, from vkDestroySemaphore.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-sync.c: destroy_sync.
	 */
	GPU_OP_DESTROY_SEMAPHORE = 41,

	/*
	 * vkCreateEvent: creates an event shared by the host and the GPU.
	 * Number: VK_EXT_command_serialization.xml:99.
	 * Arguments: the create form with [u32 VK_STRUCTURE_TYPE_EVENT_CREATE_INFO]
	 * [u64 0][u32 flags].
	 * Reply: [number][VkResult][u64 1][event].
	 * Sent by: libvulkan/sync.c: sync_create, from vkCreateEvent.
	 * Decoded by: neither native executor (i915/fence.c refuses events);
	 * only a Venus host answers it.
	 */
	GPU_OP_CREATE_EVENT = 42,

	/*
	 * vkDestroyEvent.
	 * Number: VK_EXT_command_serialization.xml:100.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/sync.c: sync_destroy, from vkDestroyEvent.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_DESTROY_EVENT = 43,

	/*
	 * vkGetEventStatus: reads whether an event is set.
	 * Number: VK_EXT_command_serialization.xml:101.
	 * Arguments: [device][event].
	 * Reply: [number][VkResult], VK_EVENT_SET or VK_EVENT_RESET.
	 * Sent by: libvulkan/sync.c: sync_event_operation ->
	 * vulkan_sync_status_locked, from vkGetEventStatus.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_GET_EVENT_STATUS = 44,

	/*
	 * vkSetEvent: sets an event from the host.
	 * Number: VK_EXT_command_serialization.xml:102.
	 * Arguments: [device][event].  Reply: [number][VkResult].
	 * Sent by: libvulkan/sync.c: sync_event_operation, from vkSetEvent.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_SET_EVENT = 45,

	/*
	 * vkResetEvent: resets an event from the host.
	 * Number: VK_EXT_command_serialization.xml:103.
	 * Arguments: [device][event].  Reply: [number][VkResult].
	 * Sent by: libvulkan/sync.c: sync_event_operation, from vkResetEvent.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_RESET_EVENT = 46,

	/*
	 * vkCreateQueryPool: creates a pool of queries of one type.
	 * Number: VK_EXT_command_serialization.xml:66.
	 * Arguments: the create form with
	 * [u32 VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO][u64 0][u32 flags]
	 * [u32 query type][u32 query count][u32 pipeline statistics].
	 * Reply: [number][VkResult][u64 1][pool].
	 * Sent by: libvulkan/query.c: vkCreateQueryPool.
	 * Decoded by: i915/fence.c: i915_query_pool_create; bcm2711 has no route.
	 */
	GPU_OP_CREATE_QUERY_POOL = 47,

	/*
	 * vkDestroyQueryPool.
	 * Number: VK_EXT_command_serialization.xml:67.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/query.c: vkDestroyQueryPool.
	 * Decoded by: i915/fence.c: i915_query_pool_destroy; bcm2711 has no route.
	 */
	GPU_OP_DESTROY_QUERY_POOL = 48,

	/*
	 * vkGetQueryPoolResults: copies query results to the host.
	 * Number: VK_EXT_command_serialization.xml:68.
	 * Arguments: [device][pool][u32 first][u32 count][u64 bytes][u64 bytes]
	 * [u64 stride][u32 flags]; libvulkan asks for packed records (each
	 * query's results and its availability word) and copies them to the
	 * caller's stride itself (libvulkan/query.c: query_copy).
	 * Reply: [number][VkResult][u64 bytes][the result bytes].
	 * Sent by: libvulkan/query.c: query_fetch, from vkGetQueryPoolResults.
	 * Decoded by: i915/fence.c: i915_query_pool_results; bcm2711 has no route.
	 */
	GPU_OP_GET_QUERY_POOL_RESULTS = 49,

	/*
	 * vkCreateBuffer: creates a buffer (no memory yet).
	 * Number: VK_EXT_command_serialization.xml:69.
	 * Arguments: the create form with VkBufferCreateInfo (an external
	 * buffer chains VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO
	 * with the context's native handle type, libvulkan/wire.c:
	 * vulkan_encode_buffer_external).
	 * Reply: [number][VkResult][u64 1][buffer].
	 * Sent by: libvulkan/resources-generated.inc: vkCreateBuffer.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_buffer;
	 * bcm2711/vulkan-resource.c: create_buffer.
	 */
	GPU_OP_CREATE_BUFFER = 50,

	/*
	 * vkDestroyBuffer.
	 * Number: VK_EXT_command_serialization.xml:70.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: resource_destroy, from vkDestroyBuffer.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-resource.c: destroy_resource.
	 */
	GPU_OP_DESTROY_BUFFER = 51,

	/*
	 * vkCreateBufferView: creates a typed view of a buffer.
	 * Number: VK_EXT_command_serialization.xml:104.
	 * Arguments: the create form with VkBufferViewCreateInfo.
	 * Reply: [number][VkResult][u64 1][view].
	 * Sent by: libvulkan/resources-generated.inc: vkCreateBufferView.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_buffer_view;
	 * bcm2711 has no route.
	 */
	GPU_OP_CREATE_BUFFER_VIEW = 52,

	/*
	 * vkDestroyBufferView.
	 * Number: VK_EXT_command_serialization.xml:105.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: resource_destroy.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain; bcm2711 has no route.
	 */
	GPU_OP_DESTROY_BUFFER_VIEW = 53,

	/*
	 * vkCreateImage: creates an image (no memory yet).
	 * Number: VK_EXT_command_serialization.xml:71.
	 * Arguments: the create form with VkImageCreateInfo (a WSI image chains
	 * its external memory declaration, libvulkan/wire.c:
	 * vulkan_encode_image_external).
	 * Reply: [number][VkResult][u64 1][image].
	 * Sent by: libvulkan/resources-generated.inc: vkCreateImage.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_image;
	 * bcm2711/vulkan-resource.c: create_image.
	 */
	GPU_OP_CREATE_IMAGE = 54,

	/*
	 * vkDestroyImage.
	 * Number: VK_EXT_command_serialization.xml:72.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: resource_destroy.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-resource.c: destroy_resource.
	 */
	GPU_OP_DESTROY_IMAGE = 55,

	/*
	 * vkGetImageSubresourceLayout: reads the offset and pitches of one
	 * subresource of a linear image.
	 * Number: VK_EXT_command_serialization.xml:73.
	 * Arguments: [device][image][u64 1][VkImageSubresource][u64 1].
	 * Reply: [number][u64 1][VkSubresourceLayout] (no VkResult).
	 * Sent by: libvulkan/resources.c: vkGetImageSubresourceLayout.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_subresource_layout;
	 * bcm2711/vulkan-resource.c: image_layout.
	 */
	GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT = 56,

	/*
	 * vkCreateImageView: creates a view of an image.
	 * Number: VK_EXT_command_serialization.xml:74.
	 * Arguments: the create form with VkImageViewCreateInfo.
	 * Reply: [number][VkResult][u64 1][view].
	 * Sent by: libvulkan/resources-generated.inc: vkCreateImageView.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_image_view;
	 * bcm2711/vulkan-input.c: bcm2711_vulkan_input_dispatch -> create_input.
	 */
	GPU_OP_CREATE_IMAGE_VIEW = 57,

	/*
	 * vkDestroyImageView.
	 * Number: VK_EXT_command_serialization.xml:75.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: resource_destroy.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-input.c: destroy_input.
	 */
	GPU_OP_DESTROY_IMAGE_VIEW = 58,

	/*
	 * vkCreateShaderModule: hands a SPIR-V module to the executor.
	 * Number: VK_EXT_command_serialization.xml:106.
	 * Arguments: the create form with VkShaderModuleCreateInfo (the code
	 * size and the words).
	 * Reply: [number][VkResult][u64 1][module].
	 * Sent by: libvulkan/resources-generated.inc: vkCreateShaderModule.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_shader;
	 * bcm2711/vulkan-input.c: create_input.
	 */
	GPU_OP_CREATE_SHADER_MODULE = 59,

	/*
	 * vkDestroyShaderModule.
	 * Number: VK_EXT_command_serialization.xml:107.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: resource_destroy.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-input.c: destroy_input.
	 */
	GPU_OP_DESTROY_SHADER_MODULE = 60,

	/*
	 * vkCreatePipelineCache: creates a pipeline cache.
	 * Number: VK_EXT_command_serialization.xml:108.
	 * Arguments: the create form with VkPipelineCacheCreateInfo.
	 * Reply: [number][VkResult][u64 1][cache].
	 * Sent by: libvulkan/pipeline.c: pipeline_single_create, from
	 * vkCreatePipelineCache.
	 * Decoded by: neither native executor (i915 refuses the pipeline range
	 * outside its object commands, bcm2711 has no route); only a Venus host.
	 */
	GPU_OP_CREATE_PIPELINE_CACHE = 61,

	/*
	 * vkDestroyPipelineCache.
	 * Number: VK_EXT_command_serialization.xml:109.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/pipeline.c: pipeline_destroy.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_DESTROY_PIPELINE_CACHE = 62,

	/*
	 * vkGetPipelineCacheData: reads a cache's contents.
	 * Number: VK_EXT_command_serialization.xml:110.
	 * Arguments: [device][cache][u64 1][u64 capacity][u64 capacity].
	 * Reply: [number][VkResult][u64 1][u64 size][u64 size][the bytes]
	 * (VK_INCOMPLETE when the capacity was short).
	 * Sent by: libvulkan/pipeline.c: vkGetPipelineCacheData.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_GET_PIPELINE_CACHE_DATA = 63,

	/*
	 * vkMergePipelineCaches: merges caches into one.
	 * Number: VK_EXT_command_serialization.xml:111.
	 * Arguments: [device][destination][u32 count][u64 count][count x cache].
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/pipeline.c: vkMergePipelineCaches.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_MERGE_PIPELINE_CACHES = 64,

	/*
	 * vkCreateGraphicsPipelines: creates graphics pipelines in one batch.
	 * Number: VK_EXT_command_serialization.xml:137.
	 * Arguments: [device][cache][u32 count][u64 count]
	 * [count x VkGraphicsPipelineCreateInfo][u64 0][u64 count]
	 * [count x pipeline], one identity reserved for every pipeline.
	 * Reply: [number][VkResult][u64 count][count x pipeline], 0 for a
	 * pipeline not created; a failed batch may still create some.
	 * Sent by: libvulkan/pipeline.c: pipeline_batch, from
	 * vkCreateGraphicsPipelines (pipeline_encode_graphics).
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_pipelines;
	 * bcm2711/vulkan-pipeline.c: bcm2711_vulkan_pipeline_dispatch ->
	 * create_pipelines.
	 */
	GPU_OP_CREATE_GRAPHICS_PIPELINES = 65,

	/*
	 * vkCreateComputePipelines: as GPU_OP_CREATE_GRAPHICS_PIPELINES with
	 * VkComputePipelineCreateInfo.
	 * Number: VK_EXT_command_serialization.xml:112.
	 * Sent by: libvulkan/pipeline.c: pipeline_batch, from
	 * vkCreateComputePipelines (pipeline_encode_compute).
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_compute_pipelines;
	 * bcm2711 has no route.
	 */
	GPU_OP_CREATE_COMPUTE_PIPELINES = 66,

	/*
	 * vkDestroyPipeline.
	 * Number: VK_EXT_command_serialization.xml:113.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/pipeline.c: pipeline_destroy, from vkDestroyPipeline,
	 * and pipeline_batch for a created pipeline it cannot publish.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_destroy_pipeline;
	 * bcm2711/vulkan-pipeline.c: destroy_pipeline.
	 */
	GPU_OP_DESTROY_PIPELINE = 67,

	/*
	 * vkCreatePipelineLayout: creates a pipeline layout (set layouts and
	 * push constant ranges).
	 * Number: VK_EXT_command_serialization.xml:114.
	 * Arguments: the create form with VkPipelineLayoutCreateInfo.
	 * Reply: [number][VkResult][u64 1][layout].
	 * Sent by: libvulkan/pipeline.c: pipeline_single_create, from
	 * vkCreatePipelineLayout.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_pipeline_layout;
	 * bcm2711/vulkan-layout.c: bcm2711_vulkan_layout_dispatch ->
	 * create_pipeline_layout.
	 */
	GPU_OP_CREATE_PIPELINE_LAYOUT = 68,

	/*
	 * vkDestroyPipelineLayout.
	 * Number: VK_EXT_command_serialization.xml:115.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/pipeline.c: pipeline_destroy.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-layout.c: destroy_layout.
	 */
	GPU_OP_DESTROY_PIPELINE_LAYOUT = 69,

	/*
	 * vkCreateSampler: creates a sampler.
	 * Number: VK_EXT_command_serialization.xml:116.
	 * Arguments: the create form with VkSamplerCreateInfo.
	 * Reply: [number][VkResult][u64 1][sampler].
	 * Sent by: libvulkan/resources-generated.inc: vkCreateSampler.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_sampler;
	 * bcm2711/vulkan-input.c: create_input.
	 */
	GPU_OP_CREATE_SAMPLER = 70,

	/*
	 * vkDestroySampler.
	 * Number: VK_EXT_command_serialization.xml:117.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: resource_destroy.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-input.c: destroy_input.
	 */
	GPU_OP_DESTROY_SAMPLER = 71,

	/*
	 * vkCreateDescriptorSetLayout: creates a descriptor set layout.
	 * Number: VK_EXT_command_serialization.xml:118.
	 * Arguments: the create form with VkDescriptorSetLayoutCreateInfo.
	 * Reply: [number][VkResult][u64 1][layout].
	 * Sent by: libvulkan/descriptors.c: vkCreateDescriptorSetLayout.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_dsl;
	 * bcm2711/vulkan-layout.c: create_set_layout.
	 */
	GPU_OP_CREATE_DESCRIPTOR_SET_LAYOUT = 72,

	/*
	 * vkDestroyDescriptorSetLayout.
	 * Number: VK_EXT_command_serialization.xml:119.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/descriptors.c: vkDestroyDescriptorSetLayout, and
	 * vkCreateDescriptorSetLayout when it cannot publish the layout.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-layout.c: destroy_layout.
	 */
	GPU_OP_DESTROY_DESCRIPTOR_SET_LAYOUT = 73,

	/*
	 * vkCreateDescriptorPool: creates a pool for descriptor sets.
	 * Number: VK_EXT_command_serialization.xml:120.
	 * Arguments: the create form with VkDescriptorPoolCreateInfo.
	 * Reply: [number][VkResult][u64 1][pool].
	 * Sent by: libvulkan/descriptors.c: vkCreateDescriptorPool.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_dpool;
	 * bcm2711/vulkan-descriptor-pool.c: bcm2711_vulkan_descriptor_pool_dispatch
	 * -> create_pool.
	 */
	GPU_OP_CREATE_DESCRIPTOR_POOL = 74,

	/*
	 * vkDestroyDescriptorPool: destroys a pool and every set in it.
	 * Number: VK_EXT_command_serialization.xml:121.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/descriptors.c: vkDestroyDescriptorPool, and
	 * vkCreateDescriptorPool when it cannot publish the pool.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_destroy_dpool;
	 * bcm2711/vulkan-descriptor-pool.c: retire_pool.
	 */
	GPU_OP_DESTROY_DESCRIPTOR_POOL = 75,

	/*
	 * vkResetDescriptorPool: frees every set of a pool.
	 * Number: VK_EXT_command_serialization.xml:122.
	 * Arguments: [device][pool][u32 flags].  Reply: [number][VkResult].
	 * Sent by: libvulkan/descriptors.c: vkResetDescriptorPool.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_reset_dpool;
	 * bcm2711/vulkan-descriptor-pool.c: retire_pool.
	 */
	GPU_OP_RESET_DESCRIPTOR_POOL = 76,

	/*
	 * vkAllocateDescriptorSets: allocates descriptor sets from a pool.
	 * Number: VK_EXT_command_serialization.xml:123.
	 * Arguments: [device][u64 1][VkDescriptorSetAllocateInfo][u64 count]
	 * [count x set], one identity reserved for every set.
	 * Reply: [number][VkResult][u64 count][count x set].
	 * Sent by: libvulkan/descriptors.c: vkAllocateDescriptorSets.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_allocate_dsets;
	 * bcm2711/vulkan-descriptor-sets.c:
	 * bcm2711_vulkan_descriptor_sets_dispatch -> allocate_sets.
	 */
	GPU_OP_ALLOCATE_DESCRIPTOR_SETS = 77,

	/*
	 * vkFreeDescriptorSets: returns sets to their pool.
	 * Number: VK_EXT_command_serialization.xml:124.
	 * Arguments: [device][pool][u32 count][u64 count][count x set].
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/descriptors.c: vkFreeDescriptorSets.
	 * Decoded by: bcm2711/vulkan-descriptor-sets.c: free_sets.  i915 has
	 * no handler and refuses it.
	 */
	GPU_OP_FREE_DESCRIPTOR_SETS = 78,

	/*
	 * vkUpdateDescriptorSets: writes and copies descriptors.
	 * Number: VK_EXT_command_serialization.xml:125.
	 * Arguments: [device][u32 writes][u64 writes][writes x
	 * VkWriteDescriptorSet][u32 copies][u64 copies][copies x
	 * VkCopyDescriptorSet]; the writes come first, as Vulkan orders them.
	 * Reply: [number].
	 * Sent by: libvulkan/descriptors.c: vkUpdateDescriptorSets
	 * (descriptor_write for each write).
	 * Decoded by: i915/objects.c -> drv_i915_gfx_update_dsets;
	 * bcm2711/vulkan-descriptor-update.c:
	 * bcm2711_vulkan_descriptor_update_dispatch -> update_sets.
	 * Order: on bcm2711 a recorded command buffer that bound a set updated
	 * after the bind becomes invalid (bcm2711/vulkan-record.c:
	 * bcm2711_vulkan_record_current).
	 */
	GPU_OP_UPDATE_DESCRIPTOR_SETS = 79,

	/*
	 * vkCreateFramebuffer: creates a framebuffer of image views.
	 * Number: VK_EXT_command_serialization.xml:138.
	 * Arguments: the create form with VkFramebufferCreateInfo.
	 * Reply: [number][VkResult][u64 1][framebuffer].
	 * Sent by: libvulkan/resources-generated.inc: vkCreateFramebuffer.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_framebuffer;
	 * bcm2711/vulkan-target.c: bcm2711_vulkan_target_dispatch ->
	 * create_target.
	 */
	GPU_OP_CREATE_FRAMEBUFFER = 80,

	/*
	 * vkDestroyFramebuffer.
	 * Number: VK_EXT_command_serialization.xml:139.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: resource_destroy.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-target.c: destroy_target.
	 */
	GPU_OP_DESTROY_FRAMEBUFFER = 81,

	/*
	 * vkCreateRenderPass: creates a render pass.
	 * Number: VK_EXT_command_serialization.xml:140.
	 * Arguments: the create form with VkRenderPassCreateInfo.
	 * Reply: [number][VkResult][u64 1][render pass].
	 * Sent by: libvulkan/resources.c: vkCreateRenderPass, which keeps a
	 * copy of the attachments and subpasses for itself.
	 * Decoded by: i915/objects.c -> drv_i915_gfx_create_render_pass;
	 * bcm2711/vulkan-target.c: create_target.
	 */
	GPU_OP_CREATE_RENDER_PASS = 82,

	/*
	 * vkDestroyRenderPass.
	 * Number: VK_EXT_command_serialization.xml:141.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/resources.c: vkDestroyRenderPass, and
	 * vkCreateRenderPass when it cannot publish the pass.
	 * Decoded by: i915/objects.c: i915_gfx_destroy_plain;
	 * bcm2711/vulkan-target.c: destroy_target.
	 */
	GPU_OP_DESTROY_RENDER_PASS = 83,

	/*
	 * vkGetRenderAreaGranularity: reads the best render area alignment.
	 * Number: VK_EXT_command_serialization.xml:142.
	 * Arguments: [device][render pass][u64 1].
	 * Reply: [number][u64 1][VkExtent2D] (no VkResult).
	 * Sent by: libvulkan/resources.c: vkGetRenderAreaGranularity.
	 * Decoded by: bcm2711/vulkan-target.c: granularity.  i915 has no
	 * handler and refuses it.
	 */
	GPU_OP_GET_RENDER_AREA_GRANULARITY = 84,

	/*
	 * vkCreateCommandPool: creates a command pool for one queue family.
	 * Number: VK_EXT_command_serialization.xml:76.
	 * Arguments: the create form with VkCommandPoolCreateInfo.
	 * Reply: [number][VkResult][u64 1][pool].
	 * Sent by: libvulkan/commands.c: vkCreateCommandPool.
	 * Decoded by: i915/command.c: drv_i915_gfx_rec_dispatch ->
	 * i915_command_pool_create; bcm2711/vulkan-command-pool.c:
	 * bcm2711_vulkan_command_pool_dispatch -> create_pool.
	 */
	GPU_OP_CREATE_COMMAND_POOL = 85,

	/*
	 * vkDestroyCommandPool: destroys a pool and its command buffers.
	 * Number: VK_EXT_command_serialization.xml:77.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/commands.c: vkDestroyCommandPool, and
	 * vkCreateCommandPool when it cannot publish the pool.
	 * Decoded by: i915/command.c: i915_command_pool_destroy;
	 * bcm2711/vulkan-command-pool.c: retire_pool.
	 */
	GPU_OP_DESTROY_COMMAND_POOL = 86,

	/*
	 * vkResetCommandPool: resets every command buffer of a pool.
	 * Number: VK_EXT_command_serialization.xml:78.
	 * Arguments: [device][pool][u32 flags].  Reply: [number][VkResult].
	 * Sent by: libvulkan/commands.c: vkResetCommandPool.
	 * Decoded by: i915/command.c: i915_command_pool_reset;
	 * bcm2711/vulkan-command-pool.c: retire_pool.
	 */
	GPU_OP_RESET_COMMAND_POOL = 87,

	/*
	 * vkAllocateCommandBuffers: allocates command buffers from a pool.
	 * Number: VK_EXT_command_serialization.xml:79.
	 * Arguments: [device][u64 1][VkCommandBufferAllocateInfo][u64 count]
	 * [count x command buffer].
	 * Reply: [number][VkResult][u64 count][count x command buffer].
	 * Sent by: libvulkan/commands.c: vkAllocateCommandBuffers.
	 * Decoded by: i915/command.c: i915_command_buffers_allocate;
	 * bcm2711/vulkan-command-batch.c: bcm2711_vulkan_command_batch_dispatch
	 * -> allocate_buffers.
	 */
	GPU_OP_ALLOCATE_COMMAND_BUFFERS = 88,

	/*
	 * vkFreeCommandBuffers: returns command buffers to their pool.
	 * Number: VK_EXT_command_serialization.xml:80.
	 * Arguments: [device][pool][u32 count][u64 count][count x command buffer].
	 * Reply: [number].
	 * Sent by: libvulkan/commands.c: vkFreeCommandBuffers.
	 * Decoded by: i915/command.c: i915_command_buffers_free;
	 * bcm2711/vulkan-command-batch.c: free_buffers.
	 */
	GPU_OP_FREE_COMMAND_BUFFERS = 89,

	/*
	 * vkBeginCommandBuffer: starts recording a command buffer.
	 * Number: VK_EXT_command_serialization.xml:81.
	 * Arguments: [command buffer][u64 1][VkCommandBufferBeginInfo]; the
	 * inheritance of a secondary buffer is cleaned of fields Vulkan says
	 * to ignore.
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/commands.c: vkBeginCommandBuffer, at once (it is
	 * not batched with the records).
	 * Decoded by: i915/command.c: i915_command_buffer_begin;
	 * bcm2711/vulkan-command-buffer.c: bcm2711_vulkan_command_buffer_dispatch
	 * -> begin_buffer.
	 * Order: the recorded vkCmd* (93 to 136 and the video ones) of the
	 * buffer may follow only after this succeeded.
	 */
	GPU_OP_BEGIN_COMMAND_BUFFER = 90,

	/*
	 * vkEndCommandBuffer: ends recording.
	 * Number: VK_EXT_command_serialization.xml:82.
	 * Arguments: [command buffer].  Reply: [number][VkResult].
	 * Sent by: libvulkan/commands.c: vkEndCommandBuffer.  The records kept
	 * since the last flush go in the same stream, before it, and only it
	 * asks for a reply.  libvulkan returns the first failure of a record
	 * it kept instead of the reply's result, and bcm2711 keeps the first
	 * failure of a decoded record for this result.
	 * Decoded by: i915/command.c: i915_command_buffer_end;
	 * bcm2711/vulkan-command-buffer.c: finish_buffer.
	 */
	GPU_OP_END_COMMAND_BUFFER = 91,

	/*
	 * vkResetCommandBuffer: returns a command buffer to the initial state.
	 * Number: VK_EXT_command_serialization.xml:83.
	 * Arguments: [command buffer][u32 flags].  Reply: [number][VkResult].
	 * Sent by: libvulkan/commands.c: vkResetCommandBuffer.
	 * Decoded by: i915/command.c: i915_command_buffer_reset;
	 * bcm2711/vulkan-command-buffer.c: finish_buffer.
	 */
	GPU_OP_RESET_COMMAND_BUFFER = 92,

	/*
	 * The recorded commands, 93 to 136.  Each is
	 * [u32 number][u32 0][command buffer] followed by the arguments of the
	 * Vulkan command, and has no reply.  libvulkan/commands.c:
	 * command_record_begin and command_record_finish (the wrappers in
	 * libvulkan/commands-generated.inc) keep them in the command buffer
	 * and send them with GPU_OP_END_COMMAND_BUFFER; a recording that grows
	 * past 1 MiB (or past the context's resource limit) is sent early
	 * without a reply, and one record larger than that is sent alone
	 * (through GPU_OP_EXECUTE_STREAMS when it does not fit one submission).
	 * i915/command.c: drv_i915_gfx_rec_dispatch owns the whole range and
	 * records them in i915_record_command; one it does not implement is
	 * refused (ENOTSUP).  bcm2711 refuses a record that asks for a reply.
	 * The recorded work runs when a GPU_OP_QUEUE_SUBMIT names the buffer.
	 */

	/*
	 * vkCmdBindPipeline.  Number: VK_EXT_command_serialization.xml:126.
	 * Arguments: [u32 bind point][pipeline].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdBindPipeline.
	 * Decoded by: i915/command.c: i915_record_command;
	 * bcm2711/vulkan-record.c: bcm2711_vulkan_record_dispatch ->
	 * bcm2711/vulkan-record-decode.c: bcm2711_vulkan_record_decode.
	 */
	GPU_OP_CMD_BIND_PIPELINE = 93,

	/*
	 * vkCmdSetViewport.  Number: VK_EXT_command_serialization.xml:143.
	 * Arguments: [u32 first][u32 count][u64 count][count x VkViewport].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetViewport.
	 * Decoded by: i915/command.c: i915_record_set_viewport;
	 * bcm2711/vulkan-record-decode.c: decode_area.
	 */
	GPU_OP_CMD_SET_VIEWPORT = 94,

	/*
	 * vkCmdSetScissor.  Number: VK_EXT_command_serialization.xml:144.
	 * Arguments: [u32 first][u32 count][u64 count][count x VkRect2D].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetScissor.
	 * Decoded by: i915/command.c: i915_record_set_scissor;
	 * bcm2711/vulkan-record-decode.c: decode_area.
	 */
	GPU_OP_CMD_SET_SCISSOR = 95,

	/*
	 * vkCmdSetLineWidth.  Number: VK_EXT_command_serialization.xml:145.
	 * Arguments: [float width].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetLineWidth.
	 * Decoded by: i915/command.c: i915_record_set_unused, which reads it
	 * and keeps nothing: its lines are one pixel wide, and a width other
	 * than 1.0 is logged once.  bcm2711 has no route.
	 */
	GPU_OP_CMD_SET_LINE_WIDTH = 96,

	/*
	 * vkCmdSetDepthBias.  Number: VK_EXT_command_serialization.xml:146.
	 * Arguments: [float constant factor][float clamp][float slope factor].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetDepthBias.
	 * Decoded by: i915/command.c: i915_record_set_unused (read and not
	 * kept: its draws have no depth bias); bcm2711 has no route.
	 */
	GPU_OP_CMD_SET_DEPTH_BIAS = 97,

	/*
	 * vkCmdSetBlendConstants.  Number: VK_EXT_command_serialization.xml:147.
	 * Arguments: [u64 4][4 x float].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetBlendConstants.
	 * Decoded by: i915/command.c: i915_record_set_blend_constants;
	 * bcm2711 has no route.
	 */
	GPU_OP_CMD_SET_BLEND_CONSTANTS = 98,

	/*
	 * vkCmdSetDepthBounds.  Number: VK_EXT_command_serialization.xml:148.
	 * Arguments: [float minimum][float maximum].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetDepthBounds.
	 * Decoded by: neither native executor (i915 refuses it as an
	 * unimplemented recording); only a Venus host.
	 */
	GPU_OP_CMD_SET_DEPTH_BOUNDS = 99,

	/*
	 * vkCmdSetStencilCompareMask.
	 * Number: VK_EXT_command_serialization.xml:149.
	 * Arguments: [u32 face mask][u32 compare mask].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetStencilCompareMask.
	 * Decoded by: i915/command.c: i915_record_set_stencil, which tells the
	 * three stencil commands apart by their number minus 100; bcm2711 has
	 * no route.
	 */
	GPU_OP_CMD_SET_STENCIL_COMPARE_MASK = 100,

	/*
	 * vkCmdSetStencilWriteMask.
	 * Number: VK_EXT_command_serialization.xml:150.
	 * Arguments: [u32 face mask][u32 write mask].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetStencilWriteMask.
	 * Decoded by: i915/command.c: i915_record_set_stencil; bcm2711 has no
	 * route.
	 */
	GPU_OP_CMD_SET_STENCIL_WRITE_MASK = 101,

	/*
	 * vkCmdSetStencilReference.
	 * Number: VK_EXT_command_serialization.xml:151.
	 * Arguments: [u32 face mask][u32 reference].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetStencilReference.
	 * Decoded by: i915/command.c: i915_record_set_stencil; bcm2711 has no
	 * route.
	 */
	GPU_OP_CMD_SET_STENCIL_REFERENCE = 102,

	/*
	 * vkCmdBindDescriptorSets.  Number: VK_EXT_command_serialization.xml:127.
	 * Arguments: [u32 bind point][layout][u32 first set][u32 count]
	 * [u64 count][count x set][u32 dynamic count][u64 dynamic count]
	 * [dynamic count x u32 offset].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdBindDescriptorSets.
	 * Decoded by: i915/command.c: i915_record_bind_descriptor_sets;
	 * bcm2711/vulkan-record-decode.c: decode_sets (each set's generation is
	 * kept, see GPU_OP_UPDATE_DESCRIPTOR_SETS).
	 */
	GPU_OP_CMD_BIND_DESCRIPTOR_SETS = 103,

	/*
	 * vkCmdBindIndexBuffer.  Number: VK_EXT_command_serialization.xml:152.
	 * Arguments: [buffer][u64 offset][u32 index type].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdBindIndexBuffer.
	 * Decoded by: i915/command.c: i915_record_bind_index;
	 * bcm2711/vulkan-record-decode.c: bcm2711_vulkan_record_decode.
	 */
	GPU_OP_CMD_BIND_INDEX_BUFFER = 104,

	/*
	 * vkCmdBindVertexBuffers.  Number: VK_EXT_command_serialization.xml:153.
	 * Arguments: [u32 first binding][u32 count][u64 count][count x buffer]
	 * [u64 count][count x u64 offset].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdBindVertexBuffers.
	 * Decoded by: i915/command.c: i915_record_bind_vertex;
	 * bcm2711/vulkan-record-decode.c: decode_vertices.
	 */
	GPU_OP_CMD_BIND_VERTEX_BUFFERS = 105,

	/*
	 * vkCmdDraw.  Number: VK_EXT_command_serialization.xml:154.
	 * Arguments: [u32 vertices][u32 instances][u32 first vertex]
	 * [u32 first instance].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdDraw.
	 * Decoded by: i915/command.c: i915_record_command (an I915_GFX_OP_DRAW
	 * operation); bcm2711/vulkan-record-decode.c:
	 * bcm2711_vulkan_record_decode, run by bcm2711/vulkan-draw.c:
	 * walk_records.
	 * Order: inside a render pass (GPU_OP_CMD_BEGIN_RENDER_PASS).
	 */
	GPU_OP_CMD_DRAW = 106,

	/*
	 * vkCmdDrawIndexed.  Number: VK_EXT_command_serialization.xml:155.
	 * Arguments: [u32 indices][u32 instances][u32 first index]
	 * [u32 vertex offset, signed][u32 first instance].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdDrawIndexed.
	 * Decoded by: i915/command.c: i915_record_command;
	 * bcm2711/vulkan-record-decode.c: bcm2711_vulkan_record_decode, run by
	 * bcm2711/vulkan-draw.c: walk_records.
	 */
	GPU_OP_CMD_DRAW_INDEXED = 107,

	/*
	 * vkCmdDrawIndirect.  Number: VK_EXT_command_serialization.xml:156.
	 * Arguments: [buffer][u64 offset][u32 draw count][u32 stride].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdDrawIndirect.
	 * Decoded by: neither native executor (i915 refuses it as an
	 * unimplemented recording); only a Venus host.
	 */
	GPU_OP_CMD_DRAW_INDIRECT = 108,

	/*
	 * vkCmdDrawIndexedIndirect.
	 * Number: VK_EXT_command_serialization.xml:157.
	 * Arguments: [buffer][u64 offset][u32 draw count][u32 stride].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdDrawIndexedIndirect.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_DRAW_INDEXED_INDIRECT = 109,

	/*
	 * vkCmdDispatch.  Number: VK_EXT_command_serialization.xml:129.
	 * Arguments: [u32 groups x][u32 groups y][u32 groups z].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdDispatch.
	 * Decoded by: i915/command.c: i915_record_command; bcm2711 has no route.
	 */
	GPU_OP_CMD_DISPATCH = 110,

	/*
	 * vkCmdDispatchIndirect.  Number: VK_EXT_command_serialization.xml:130.
	 * Arguments: [buffer][u64 offset].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdDispatchIndirect.
	 * Decoded by: i915/command.c: i915_record_command; bcm2711 has no route.
	 */
	GPU_OP_CMD_DISPATCH_INDIRECT = 111,

	/*
	 * vkCmdCopyBuffer.  Number: VK_EXT_command_serialization.xml:84.
	 * Arguments: [source buffer][destination buffer][u32 count][u64 count]
	 * [count x VkBufferCopy].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdCopyBuffer.
	 * Decoded by: i915/command.c: i915_record_copy_buffer;
	 * bcm2711/vulkan-buffer-copy.c: bcm2711_vulkan_buffer_copy_dispatch ->
	 * decode_copy.
	 */
	GPU_OP_CMD_COPY_BUFFER = 112,

	/*
	 * vkCmdCopyImage.  Number: VK_EXT_command_serialization.xml:85.
	 * Arguments: [source image][u32 layout][destination image][u32 layout]
	 * [u32 count][u64 count][count x VkImageCopy].  libvulkan sends
	 * VK_IMAGE_LAYOUT_GENERAL for VK_IMAGE_LAYOUT_PRESENT_SRC_KHR here and
	 * wherever it sends an image layout (libvulkan/wire.c:
	 * vulkan_wire_image_layout).
	 * Sent by: libvulkan/commands-generated.inc: vkCmdCopyImage.
	 * Decoded by: i915/command.c: i915_record_image_copy;
	 * bcm2711/vulkan-transfer.c: bcm2711_vulkan_transfer_dispatch ->
	 * decode_transfer.
	 */
	GPU_OP_CMD_COPY_IMAGE = 113,

	/*
	 * vkCmdBlitImage.  Number: VK_EXT_command_serialization.xml:158.
	 * Arguments: [source image][u32 layout][destination image][u32 layout]
	 * [u32 count][u64 count][count x VkImageBlit][u32 filter].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdBlitImage.
	 * Decoded by: i915/command.c: i915_record_image_copy;
	 * bcm2711/vulkan-transfer.c: decode_transfer.
	 */
	GPU_OP_CMD_BLIT_IMAGE = 114,

	/*
	 * vkCmdCopyBufferToImage.  Number: VK_EXT_command_serialization.xml:86.
	 * Arguments: [source buffer][destination image][u32 layout][u32 count]
	 * [u64 count][count x VkBufferImageCopy].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdCopyBufferToImage.
	 * Decoded by: i915/command.c: i915_record_buffer_image_copy;
	 * bcm2711/vulkan-buffer-copy.c: decode_copy.
	 */
	GPU_OP_CMD_COPY_BUFFER_TO_IMAGE = 115,

	/*
	 * vkCmdCopyImageToBuffer.  Number: VK_EXT_command_serialization.xml:87.
	 * Arguments: [source image][u32 layout][destination buffer][u32 count]
	 * [u64 count][count x VkBufferImageCopy].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdCopyImageToBuffer.
	 * Decoded by: i915/command.c: i915_record_buffer_image_copy;
	 * bcm2711/vulkan-buffer-copy.c: decode_copy.
	 */
	GPU_OP_CMD_COPY_IMAGE_TO_BUFFER = 116,

	/*
	 * vkCmdUpdateBuffer.  Number: VK_EXT_command_serialization.xml:88.
	 * Arguments: [buffer][u64 offset][u64 size][u64 size][the bytes].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdUpdateBuffer.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_UPDATE_BUFFER = 117,

	/*
	 * vkCmdFillBuffer.  Number: VK_EXT_command_serialization.xml:89.
	 * Arguments: [buffer][u64 offset][u64 size][u32 data].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdFillBuffer.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_FILL_BUFFER = 118,

	/*
	 * vkCmdClearColorImage.  Number: VK_EXT_command_serialization.xml:128.
	 * Arguments: [image][u32 layout][u64 1][u32 2][u64 4][4 x u32]
	 * [u32 count][u64 count][count x VkImageSubresourceRange].  The clear
	 * color is always sent as the union's uint32 member (tag 2) so its bits
	 * pass unchanged (libvulkan/commands.c: command_encode_color).
	 * Sent by: libvulkan/commands-generated.inc: vkCmdClearColorImage.
	 * Decoded by: i915/command.c: i915_record_clear_image;
	 * bcm2711/vulkan-record-decode.c: decode_clear.
	 */
	GPU_OP_CMD_CLEAR_COLOR_IMAGE = 119,

	/*
	 * vkCmdClearDepthStencilImage.
	 * Number: VK_EXT_command_serialization.xml:159.
	 * Arguments: [image][u32 layout][u64 1][float depth][u32 stencil]
	 * [u32 count][u64 count][count x VkImageSubresourceRange]; a member the
	 * ranges do not clear is sent as a fixed value
	 * (libvulkan/commands.c: command_encode_depth_ranges).
	 * Sent by: libvulkan/commands-generated.inc: vkCmdClearDepthStencilImage.
	 * Decoded by: ghci/vk/vk-command.c: record_arguments;
	 * mali/vk/vk-command.c: decode_regions; i915 and bcm2711 do not
	 * decode it; a Venus host answers it.
	 */
	GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE = 120,

	/*
	 * vkCmdClearAttachments.  Number: VK_EXT_command_serialization.xml:160.
	 * Arguments: [u32 count][u64 count][count x VkClearAttachment][u32 rects]
	 * [u64 rects][rects x VkClearRect]; each attachment is its aspect mask,
	 * its color attachment index and a tagged clear value
	 * (libvulkan/commands.c: command_encode_clear_attachment).
	 * Sent by: libvulkan/commands-generated.inc: vkCmdClearAttachments.
	 * Decoded by: i915/command.c: i915_record_clear_attachments; bcm2711 has
	 * no route.
	 */
	GPU_OP_CMD_CLEAR_ATTACHMENTS = 121,

	/*
	 * vkCmdResolveImage.  Number: VK_EXT_command_serialization.xml:161.
	 * Arguments: [source image][u32 layout][destination image][u32 layout]
	 * [u32 count][u64 count][count x VkImageResolve].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdResolveImage.
	 * Decoded by: i915/command.c: i915_record_image_copy; bcm2711 has no
	 * route.
	 */
	GPU_OP_CMD_RESOLVE_IMAGE = 122,

	/*
	 * vkCmdSetEvent.  Number: VK_EXT_command_serialization.xml:131.
	 * Arguments: [event][u32 stage mask].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdSetEvent.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_SET_EVENT = 123,

	/*
	 * vkCmdResetEvent.  Number: VK_EXT_command_serialization.xml:132.
	 * Arguments: [event][u32 stage mask].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdResetEvent.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_RESET_EVENT = 124,

	/*
	 * vkCmdWaitEvents.  Number: VK_EXT_command_serialization.xml:133.
	 * Arguments: [u32 events][u64 events][events x event][u32 source stages]
	 * [u32 destination stages], then the memory, buffer and image barriers,
	 * each as [u32 count][u64 count][count x barrier].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdWaitEvents.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_WAIT_EVENTS = 125,

	/*
	 * vkCmdPipelineBarrier.  Number: VK_EXT_command_serialization.xml:90.
	 * Arguments: [u32 source stages][u32 destination stages]
	 * [u32 dependency flags], then the VkMemoryBarrier,
	 * VkBufferMemoryBarrier and VkImageMemoryBarrier arrays, each as
	 * [u32 count][u64 count][count x barrier].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdPipelineBarrier.
	 * Decoded by: i915/command.c: i915_record_barrier;
	 * bcm2711/vulkan-barrier.c: bcm2711_vulkan_barrier_dispatch.
	 */
	GPU_OP_CMD_PIPELINE_BARRIER = 126,

	/*
	 * vkCmdBeginQuery.  Number: VK_EXT_command_serialization.xml:91.
	 * Arguments: [pool][u32 query][u32 flags].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdBeginQuery.
	 * Decoded by: i915/command.c: i915_record_query; bcm2711 has no route.
	 */
	GPU_OP_CMD_BEGIN_QUERY = 127,

	/*
	 * vkCmdEndQuery.  Number: VK_EXT_command_serialization.xml:92.
	 * Arguments: [pool][u32 query].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdEndQuery.
	 * Decoded by: i915/command.c: i915_record_query; bcm2711 has no route.
	 */
	GPU_OP_CMD_END_QUERY = 128,

	/*
	 * vkCmdResetQueryPool.  Number: VK_EXT_command_serialization.xml:93.
	 * Arguments: [pool][u32 first][u32 count].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdResetQueryPool.
	 * Decoded by: i915/command.c: i915_record_query; bcm2711 has no route.
	 */
	GPU_OP_CMD_RESET_QUERY_POOL = 129,

	/*
	 * vkCmdWriteTimestamp.  Number: VK_EXT_command_serialization.xml:94.
	 * Arguments: [u32 stage][pool][u32 query].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdWriteTimestamp.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_WRITE_TIMESTAMP = 130,

	/*
	 * vkCmdCopyQueryPoolResults.  Number: VK_EXT_command_serialization.xml:95.
	 * Arguments: [pool][u32 first][u32 count][buffer][u64 offset]
	 * [u64 stride][u32 flags].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdCopyQueryPoolResults.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_CMD_COPY_QUERY_POOL_RESULTS = 131,

	/*
	 * vkCmdPushConstants.  Number: VK_EXT_command_serialization.xml:134.
	 * Arguments: [layout][u32 stages][u32 offset][u32 size][u64 size]
	 * [the bytes].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdPushConstants.
	 * Decoded by: i915/command.c: i915_record_push_constants;
	 * bcm2711/vulkan-record-decode.c: decode_push.
	 */
	GPU_OP_CMD_PUSH_CONSTANTS = 132,

	/*
	 * vkCmdBeginRenderPass.  Number: VK_EXT_command_serialization.xml:162.
	 * Arguments: [u64 1][u32 sType][u64 0][render pass][framebuffer]
	 * [VkRect2D render area][u32 clears][u64 clears][clears x clear value]
	 * [u32 contents]; a clear value no attachment clears is not read from
	 * the caller (libvulkan/commands.c: command_encode_render_begin).
	 * Sent by: libvulkan/commands-generated.inc: vkCmdBeginRenderPass.
	 * Decoded by: i915/command.c: i915_record_begin_pass;
	 * bcm2711/vulkan-record-decode.c: decode_pass.
	 * Order: closed by GPU_OP_CMD_END_RENDER_PASS in the same buffer.
	 */
	GPU_OP_CMD_BEGIN_RENDER_PASS = 133,

	/*
	 * vkCmdNextSubpass.  Number: VK_EXT_command_serialization.xml:163.
	 * Arguments: [u32 contents].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdNextSubpass.
	 * Decoded by: neither native executor (i915 refuses it as an
	 * unimplemented recording); only a Venus host.
	 */
	GPU_OP_CMD_NEXT_SUBPASS = 134,

	/*
	 * vkCmdEndRenderPass.  Number: VK_EXT_command_serialization.xml:164.
	 * Arguments: none.
	 * Sent by: libvulkan/commands-generated.inc: vkCmdEndRenderPass.
	 * Decoded by: i915/command.c: i915_record_command (an
	 * I915_GFX_OP_END_PASS operation); bcm2711/vulkan-record.c:
	 * bcm2711_vulkan_record_dispatch.
	 */
	GPU_OP_CMD_END_RENDER_PASS = 135,

	/*
	 * vkCmdExecuteCommands.  Number: VK_EXT_command_serialization.xml:96.
	 * Arguments: [u32 count][u64 count][count x command buffer].
	 * Sent by: libvulkan/commands-generated.inc: vkCmdExecuteCommands.
	 * Decoded by: neither native executor (i915 owns the number in its
	 * recording range but refuses it); only a Venus host.
	 */
	GPU_OP_CMD_EXECUTE_COMMANDS = 136,

	/*
	 * vkEnumerateInstanceVersion, used only as the completion trailer of
	 * every submission.
	 * Number: VK_EXT_command_serialization.xml:169.
	 * Arguments: [u64 1], with GPU_OP_FLAG_REPLY.
	 * Reply: the 20 bytes [number][VkResult 0][u64 1][u32 apiVersion] at the
	 * position GPU_OP_SEEK_REPLY_STREAM chose, the last 20 bytes of the reply
	 * blob.  The executor writes the version last; libvulkan/context.c:
	 * vulkan_context_poll waits until it reads at least 1.1.0 there and the
	 * other fields match.
	 * Sent by: libvulkan/context.c: vulkan_encode_reply_trailer, at the end
	 * of every vulkan_context_transaction.  libvulkan does not export
	 * vkEnumerateInstanceVersion itself.
	 * Decoded by: i915/transport.c: drv_i915_render_transport_dispatch ->
	 * i915_transport_version (it reports 1.1.0); bcm2711/vulkan-stream.c:
	 * execute_command, which refuses it without the reply flag, ->
	 * version_reply.
	 */
	GPU_OP_ENUMERATE_INSTANCE_VERSION = 137,

	/*
	 * vkGetPhysicalDeviceProperties2, asked only for the device's ID
	 * properties (the device and driver UUIDs that pair shared memory).
	 * Number: VK_EXT_command_serialization.xml:188.
	 * Arguments: [physical device][u64 1]
	 * [u32 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2][u64 1]
	 * [u32 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES][u64 0].
	 * Reply: [number][u64 1] then the two headers, the device UUID, driver
	 * UUID, device LUID, node mask, LUID-valid word and
	 * VkPhysicalDeviceProperties (no VkResult).
	 * Sent by: libvulkan/external-properties.c: vulkan_physical_identity,
	 * from vkGetPhysicalDeviceProperties2KHR, and libvulkan/memory.c:
	 * vkGetMemoryFdKHR and memory_import_fd.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_PROPERTIES2 = 148,

	/*
	 * vkGetPhysicalDeviceImageFormatProperties2, asked with an external
	 * memory handle type.
	 * Number: VK_EXT_command_serialization.xml:192.
	 * Arguments: [physical device][u64 1][u32 IMAGE_FORMAT_INFO_2 sType]
	 * [u64 1][u32 EXTERNAL_IMAGE_FORMAT_INFO sType][u64 0][u32 native
	 * handle type][u32 format][u32 type][u32 tiling][u32 usage][u32 flags]
	 * [u64 1][u32 IMAGE_FORMAT_PROPERTIES_2 sType][u64 1]
	 * [u32 EXTERNAL_IMAGE_FORMAT_PROPERTIES sType][u64 0].
	 * Reply: [number][VkResult][u64 1] then the external memory properties
	 * and VkImageFormatProperties.
	 * Sent by: libvulkan/external-properties.c: external_image_properties,
	 * from vkGetPhysicalDeviceImageFormatProperties2KHR.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_IMAGE_FORMAT_PROPERTIES2 = 150,

	/*
	 * vkGetDeviceQueue2: fetches one queue of the device and binds it to a
	 * timeline.
	 * Number: VK_EXT_command_serialization.xml:202.
	 * Arguments: [device][u64 1][u32 VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2]
	 * [u64 1][u32 GPU_OP_RECORD_QUEUE_TIMELINE][u64 0][u32 ring index]
	 * [u32 flags 0][u32 family][u32 index][u64 1][queue].
	 * Reply: [number][u64 1][queue] (no VkResult).
	 * Sent by: libvulkan/device.c: device_queue_remote, from vkCreateDevice
	 * for every queue it created.
	 * Decoded by: i915/instance.c: i915_instance_get_device_queue2, which
	 * remembers the family and serves every timeline on one ring;
	 * bcm2711/vulkan-device.c: device_queue, which requires the timeline
	 * record.
	 * Order: after GPU_OP_CREATE_DEVICE; a GPU_OP_QUEUE_SUBMIT names only a
	 * queue fetched this way.
	 */
	GPU_OP_GET_DEVICE_QUEUE2 = 155,

	/*
	 * vkGetPhysicalDeviceExternalBufferProperties, asked with the context's
	 * native handle type.
	 * Number: VK_EXT_command_serialization.xml:203.
	 * Arguments: [physical device][u64 1]
	 * [u32 PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO sType][u64 0][u32 flags]
	 * [u32 usage][u32 native handle type][u64 1]
	 * [u32 EXTERNAL_BUFFER_PROPERTIES sType][u64 0].
	 * Reply: [number][u64 1] then the external memory properties.
	 * Sent by: libvulkan/external-properties.c: external_buffer_properties,
	 * from vkGetPhysicalDeviceExternalBufferPropertiesKHR.
	 * Decoded by: neither native executor; only a Venus host.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_EXTERNAL_BUFFER_PROPERTIES = 161,

	/*
	 * vkSetReplyCommandStreamMESA (the command's name in the cited XML):
	 * selects where the replies of the stream go.
	 * Number: VK_EXT_command_serialization.xml:553.
	 * Arguments: [u64 1][u32 resource id][u64 offset 0][u64 capacity], with
	 * flags 0: the session blob the replies are written to.
	 * Reply: none.
	 * Sent by: libvulkan/context.c: vulkan_encode_reply_stream, first in
	 * every submission.
	 * Decoded by: i915/transport.c: drv_i915_render_transport_reply reads it
	 * as a fixed 36-byte record at the start of the stream before decoding,
	 * and i915_transport_set_reply restarts the reply cursor;
	 * bcm2711/vulkan-stream.c: execute_command -> select_reply.  bcm2711
	 * refuses it, 179 and 180 with the reply flag set.
	 */
	GPU_OP_SET_REPLY_STREAM = 178,

	/*
	 * vkSeekReplyCommandStreamMESA: moves the reply cursor.
	 * Number: VK_EXT_command_serialization.xml:554.
	 * Arguments: [u64 position], with flags 0; libvulkan sends the reply
	 * capacity minus the 20 bytes of the trailer.
	 * Reply: none.
	 * Sent by: libvulkan/context.c: vulkan_encode_reply_trailer, before
	 * GPU_OP_ENUMERATE_INSTANCE_VERSION.
	 * Decoded by: i915/transport.c: i915_transport_seek_reply, which refuses
	 * a position past the region; bcm2711/vulkan-stream.c: seek_reply.
	 */
	GPU_OP_SEEK_REPLY_STREAM = 179,

	/*
	 * vkExecuteCommandStreamsMESA: runs a command stream held in a session
	 * blob in place of this command.
	 * Number: VK_EXT_command_serialization.xml:555.
	 * Arguments: [u32 1 stream][u64 1][u32 resource id][u64 offset 0]
	 * [u64 bytes][u64 0, no reply offsets][u32 0 dependencies][u64 0]
	 * [u32 flags 0], with flags 0.
	 * Reply: none of its own; the commands of the stream reply as usual.
	 * Sent by: libvulkan/context.c: vulkan_encode_external_stream, for a
	 * command longer than GPU_COMMAND_MAX (uapi/gpu.h) less 128 bytes,
	 * copied first into the context's stream blob.
	 * Decoded by: i915/transport.c: i915_transport_execute_streams, which
	 * decodes at most 64 MiB from a private copy; bcm2711/vulkan-stream.c:
	 * external_stream.
	 */
	GPU_OP_EXECUTE_STREAMS = 180,

	/*
	 * zedBSD's own commands: Vulkan Video, H.264 decode.  Only a
	 * backend whose capset declares video takes them (the native i915);
	 * they are never sent to a Venus host.  The promise is bit 0 of the
	 * word at byte 172 of the 176-byte native capset (magic 0x5a4e4154 at
	 * byte 168), read by libvulkan/context.c: vulkan_context_open.  On i915
	 * the queries, sessions and parameters go to i915/video.c:
	 * drv_i915_video_dispatch, which refuses them on a device without
	 * video decode; the four recorded ones go through
	 * i915/command.c: i915_record_command to i915/video.c:
	 * drv_i915_video_record.  bcm2711 has no route for any of them.  A
	 * video structure is sent with its whole chain
	 * (libvulkan/video.c: video_encode_struct).
	 */

	/*
	 * vkGetPhysicalDeviceVideoCapabilitiesKHR: reads the limits of one video
	 * profile.
	 * Arguments: [physical device][u64 1][VkVideoProfileInfoKHR and chain]
	 * [u64 1][the output structure headers to fill].
	 * Reply: [number][VkResult][u64 1][the filled structures].
	 * Sent by: libvulkan/video.c: vkGetPhysicalDeviceVideoCapabilitiesKHR.
	 * Decoded by: i915/video.c: i915_video_capabilities.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_CAPABILITIES = 0x10000,

	/*
	 * vkGetPhysicalDeviceVideoFormatPropertiesKHR: lists the image formats a
	 * profile decodes into.
	 * Arguments: [physical device][u64 1][VkPhysicalDeviceVideoFormatInfoKHR
	 * and chain][u64 1][u32 32][u64 32], libvulkan always asking for up to
	 * 32.
	 * Reply: [number][VkResult][u64 1][u32 count][u64 count]
	 * [count x VkVideoFormatPropertiesKHR].
	 * Sent by: libvulkan/video.c: vkGetPhysicalDeviceVideoFormatPropertiesKHR.
	 * Decoded by: i915/video.c: i915_video_format_properties.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_VIDEO_FORMAT_PROPERTIES = 0x10001,

	/*
	 * The codec operations of each queue family (zedBSD's own; Vulkan
	 * carries them in VkQueueFamilyVideoPropertiesKHR).
	 * Arguments: [physical device][u32 family count].
	 * Reply: [number][u64 1][u32 count][count x u32 codec operations]
	 * (no VkResult); the count must equal the family count.
	 * Sent by: libvulkan/instance.c: physical_load_video, at discovery and
	 * only with the video capset; it keeps only H.264 decode, and only on a
	 * family with the video decode flag.
	 * Decoded by: i915/video.c: i915_video_family_properties.
	 */
	GPU_OP_GET_PHYSICAL_DEVICE_QUEUE_FAMILY_VIDEO_PROPERTIES = 0x10002,

	/*
	 * vkCreateVideoSessionKHR: creates a decode session.
	 * Arguments: the create form with VkVideoSessionCreateInfoKHR and chain.
	 * Reply: [number][VkResult][u64 1][session].
	 * Sent by: libvulkan/video.c: vkCreateVideoSessionKHR.
	 * Decoded by: i915/video.c: i915_video_session_create.
	 */
	GPU_OP_CREATE_VIDEO_SESSION = 0x10003,

	/*
	 * vkDestroyVideoSessionKHR.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/video.c: video_destroy, from
	 * vkDestroyVideoSessionKHR, and by the create form when
	 * vkCreateVideoSessionKHR cannot publish the session.
	 * Decoded by: i915/video.c: i915_video_session_destroy.
	 */
	GPU_OP_DESTROY_VIDEO_SESSION = 0x10004,

	/*
	 * vkGetVideoSessionMemoryRequirementsKHR: lists the memory bindings a
	 * session needs.
	 * Arguments: [device][session][u64 1][u32 32][u64 32].
	 * Reply: [number][VkResult][u64 1][u32 count][u64 count]
	 * [count x VkVideoSessionMemoryRequirementsKHR].
	 * Sent by: libvulkan/video.c: vkGetVideoSessionMemoryRequirementsKHR.
	 * Decoded by: i915/video.c: i915_video_requirements.
	 */
	GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS = 0x10005,

	/*
	 * vkBindVideoSessionMemoryKHR: binds memory to a session's bindings.
	 * Arguments: [device][session][u32 count][u64 count]
	 * [count x VkBindVideoSessionMemoryInfoKHR].
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/video.c: vkBindVideoSessionMemoryKHR.
	 * Decoded by: i915/video.c: i915_video_bind.
	 * Order: after GPU_OP_GET_VIDEO_SESSION_MEMORY_REQUIREMENTS, before the
	 * session is used in a recording.
	 */
	GPU_OP_BIND_VIDEO_SESSION_MEMORY = 0x10006,

	/*
	 * vkCreateVideoSessionParametersKHR: creates the parameter sets (the
	 * H.264 SPS and PPS) of a session.
	 * Arguments: the create form with VkVideoSessionParametersCreateInfoKHR
	 * and chain.
	 * Reply: [number][VkResult][u64 1][parameters].
	 * Sent by: libvulkan/video.c: vkCreateVideoSessionParametersKHR.
	 * Decoded by: i915/video.c: i915_video_parameters_create.
	 */
	GPU_OP_CREATE_VIDEO_SESSION_PARAMETERS = 0x10007,

	/*
	 * vkUpdateVideoSessionParametersKHR: adds parameter sets.
	 * Arguments: [device][parameters][u64 1]
	 * [VkVideoSessionParametersUpdateInfoKHR and chain].
	 * Reply: [number][VkResult].
	 * Sent by: libvulkan/video.c: vkUpdateVideoSessionParametersKHR.
	 * Decoded by: i915/video.c: i915_video_parameters_update.
	 */
	GPU_OP_UPDATE_VIDEO_SESSION_PARAMETERS = 0x10008,

	/*
	 * vkDestroyVideoSessionParametersKHR.
	 * Arguments: the destroy form.  Reply: [number].
	 * Sent by: libvulkan/video.c: video_destroy.
	 * Decoded by: i915/video.c: i915_video_parameters_destroy.  It is the
	 * last number i915/dispatch.c routes to i915/video.c
	 * (I915_VK_VIDEO_OP_LAST_ROUTED); the ones after it are recordings.
	 */
	GPU_OP_DESTROY_VIDEO_SESSION_PARAMETERS = 0x10009,

	/*
	 * vkCmdBeginVideoCodingKHR, recorded like 93 to 136 (no reply).
	 * Arguments: [u64 1][VkVideoBeginCodingInfoKHR and chain]: the flags,
	 * session, parameters and reference slots.
	 * Sent by: libvulkan/video.c: vkCmdBeginVideoCodingKHR, through
	 * libvulkan/commands.c: vulkan_command_record_begin.
	 * Decoded by: i915/command.c: i915_record_video -> i915/video.c:
	 * drv_i915_video_record.
	 * Order: opens a coding scope closed by GPU_OP_CMD_END_VIDEO_CODING.
	 */
	GPU_OP_CMD_BEGIN_VIDEO_CODING = 0x1000a,

	/*
	 * vkCmdEndVideoCodingKHR, recorded (no reply).
	 * Arguments: [u64 1][VkVideoEndCodingInfoKHR and chain] (its flags).
	 * Sent by: libvulkan/video.c: vkCmdEndVideoCodingKHR.
	 * Decoded by: i915/video.c: drv_i915_video_record.
	 */
	GPU_OP_CMD_END_VIDEO_CODING = 0x1000b,

	/*
	 * vkCmdControlVideoCodingKHR, recorded (no reply): controls the
	 * session, such as its reset.
	 * Arguments: [u64 1][VkVideoCodingControlInfoKHR and chain] (its flags).
	 * Sent by: libvulkan/video.c: vkCmdControlVideoCodingKHR.
	 * Decoded by: i915/video.c: drv_i915_video_record.
	 */
	GPU_OP_CMD_CONTROL_VIDEO_CODING = 0x1000c,

	/*
	 * vkCmdDecodeVideoKHR, recorded (no reply): decodes one picture.
	 * Arguments: [u64 1][VkVideoDecodeInfoKHR and chain]: the flags, the
	 * bitstream buffer, offset and range, the destination picture, the
	 * optional setup slot, the reference slots, and in the chain the H.264
	 * picture information.
	 * Sent by: libvulkan/video.c: vkCmdDecodeVideoKHR.
	 * Decoded by: i915/video.c: drv_i915_video_record; the decode runs when
	 * the buffer is submitted on the video decode queue family.
	 * Order: inside a coding scope (GPU_OP_CMD_BEGIN_VIDEO_CODING).
	 */
	GPU_OP_CMD_DECODE_VIDEO = 0x1000d
};

#endif
