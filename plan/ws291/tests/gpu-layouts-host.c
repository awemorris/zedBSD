/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the GPU core's image layouts on the host: GPU_IMAGE_LAYOUTS
 * with and without a backend's own layouts (the core's linear answer, the
 * input checks, the check of a backend's answer), the capability bit, the
 * export of a layout other than linear rows (refused without the backend's
 * operations, bounded by the bytes the backend reports), and the refusal
 * of such an image by another device.  It reuses the real GPU, cdev and
 * handle cores and the sharing backend of the ws014 fixture.
 */

#define GPU_SHARING_ENTRY gpu_sharing_unused_main
#include "../../ws014/tests/gpu-sharing.c"
#undef GPU_SHARING_ENTRY

#include <kern/clock.h>
#include <kern/platform.h>
#include <uapi/gpu-scanout.h>

/* The host build has no architecture header that names an unused parameter. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

static int layouts_check(void *opaque, const struct gpu_image_descriptor *image, uint64_t *bytes);
static int layouts_list(void *opaque, void *session, struct gpu_image_layouts *request);
static void layouts_prepare(struct gpu_image_layouts *request, uint32_t uses, uint32_t display_id);

/* Makes layouts_list break the contract (a use the request did not get) once set. */
static unsigned layouts_broken;

/*
 * Checks the core's handling of image layouts against two devices: gpu0
 * names NVIDIA's block linear, gpu1 names no layouts.
 */
int
main(void)
{
	struct drv_gpu_ops operations;
	struct drv_gpu_ops plain_operations;
	struct drv_gpu_share_ops sharing;
	struct drv_gpu_layout_ops layouts;
	struct drv_gpu_device *device;
	struct drv_gpu_device *plain_device;
	struct drv_gpu_device *refused;
	struct test_backend backend;
	struct test_backend plain_backend;
	struct test_file named;
	struct test_file plain;
	struct process producer;
	struct gpu_image_layouts request;
	struct gpu_info info;
	struct gpu_blob_create create;
	struct gpu_resource_export exported;
	struct gpu_resource_import imported;
	int error;

	/* One process holds every fd of the test. */
	memset(&producer, 0, sizeof(producer));
	producer.fd = filedesc_create(&producer);
	assert(producer.fd != NULL);
	gpu_test_set_process(&producer);

	/* gpu0 shares images and names its own layouts; gpu1 shares linear images only. */
	memset(&backend, 0, sizeof(backend));
	memset(&plain_backend, 0, sizeof(plain_backend));
	memset(&operations, 0, sizeof(operations));
	memset(&sharing, 0, sizeof(sharing));
	memset(&layouts, 0, sizeof(layouts));
	operations.version = DRV_GPU_INTERFACE_VERSION;
	operations.size = sizeof(operations);
	operations.capabilities = GPU_CAP_BLOB | GPU_CAP_SHARE;
	operations.open = backend_open;
	operations.close = backend_close;
	operations.get_info = backend_get_info;
	operations.blob_create = sharing_blob;
	operations.resource_destroy = sharing_destroy;
	sharing.export_resource = sharing_export;
	sharing.release = sharing_release;
	sharing.import_resource = sharing_import;
	operations.share = &sharing;
	plain_operations = operations;
	layouts.image_layout = layouts_check;
	layouts.image_layouts = layouts_list;
	operations.layouts = &layouts;
	error = drv_gpu_register(&operations, &backend, &device);
	assert(error == 0);
	error = drv_gpu_register(&plain_operations, &plain_backend, &plain_device);
	assert(error == 0);
	error = open_file(&named, "gpu0", O_RDWR);
	assert(error == 0);
	error = open_file(&plain, "gpu1", O_RDWR);
	assert(error == 0);

	/* A table with one of its two operations missing is refused at registration. */
	layouts.image_layouts = NULL;
	error = drv_gpu_register(&operations, &backend, &refused);
	assert(error == EINVAL);
	layouts.image_layouts = layouts_list;

	/* The capability bit is the core's, set only for the device that names layouts. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	error = sharing_ioctl(&named, GPU_GET_INFO, &info);
	assert(error == 0);
	assert((info.capabilities & GPU_CAP_IMAGE_LAYOUTS) != 0U);
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	error = sharing_ioctl(&plain, GPU_GET_INFO, &info);
	assert(error == 0);
	assert((info.capabilities & GPU_CAP_IMAGE_LAYOUTS) == 0U);

	/* Without layouts of its own a device answers linear rows with the uses its operations give. */
	layouts_prepare(&request, GPU_LAYOUT_SHARE, 0U);
	error = sharing_ioctl(&plain, GPU_IMAGE_LAYOUTS, &request);
	assert(error == 0);
	assert(request.count == 1U);
	assert(request.entry[0].tiling == GPU_IMAGE_LINEAR);
	assert(request.entry[0].parameter == 0U);
	assert(request.entry[0].flags == GPU_LAYOUT_SHARE);

	/* A use the device's operations do not give leaves no layout. */
	layouts_prepare(&request, GPU_LAYOUT_RENDER, 0U);
	error = sharing_ioctl(&plain, GPU_IMAGE_LAYOUTS, &request);
	assert(error == 0);
	assert(request.count == 0U);

	/* A scanout asked about without a display, a stale answer and a narrow format are refused. */
	layouts_prepare(&request, GPU_LAYOUT_SCANOUT, 0U);
	error = sharing_ioctl(&plain, GPU_IMAGE_LAYOUTS, &request);
	assert(error == EINVAL);
	layouts_prepare(&request, 0U, 0U);
	request.count = 1U;
	error = sharing_ioctl(&plain, GPU_IMAGE_LAYOUTS, &request);
	assert(error == EINVAL);
	layouts_prepare(&request, 0U, 0U);
	request.entry[3].flags = 1U;
	error = sharing_ioctl(&plain, GPU_IMAGE_LAYOUTS, &request);
	assert(error == EINVAL);
	layouts_prepare(&request, 0U, 0U);
	request.format = 3U;
	error = sharing_ioctl(&plain, GPU_IMAGE_LAYOUTS, &request);
	assert(error == EOPNOTSUPP);

	/* The device's own answer comes back in its order. */
	layouts_prepare(&request, GPU_LAYOUT_SHARE, 0U);
	error = sharing_ioctl(&named, GPU_IMAGE_LAYOUTS, &request);
	assert(error == 0);
	assert(request.count == 2U);
	assert(request.entry[0].tiling == GPU_IMAGE_NV_BLOCK_LINEAR);
	assert(request.entry[1].tiling == GPU_IMAGE_LINEAR);

	/* An answer missing a use asked for is the backend's fault. */
	layouts_broken = 1U;
	layouts_prepare(&request, GPU_LAYOUT_SHARE, 0U);
	error = sharing_ioctl(&named, GPU_IMAGE_LAYOUTS, &request);
	assert(error == EIO);
	layouts_broken = 0U;

	/* One allocation on each device, for the exports below. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = 4096U;
	create.blob_id = 7U;
	create.flags = GPU_BLOB_SHAREABLE;
	error = sharing_ioctl(&plain, GPU_BLOB_CREATE, &create);
	assert(error == 0);

	/* A device without layouts exports block linear as before: not supported. */
	export_prepare(&exported, create.handle);
	exported.image.tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	error = sharing_ioctl(&plain, GPU_RESOURCE_EXPORT, &exported);
	assert(error == EOPNOTSUPP);

	/* Linear rows keep no variant. */
	export_prepare(&exported, create.handle);
	exported.image.layout_parameter = 1U;
	error = sharing_ioctl(&plain, GPU_RESOURCE_EXPORT, &exported);
	assert(error == EINVAL);
	error = destroy_handle(&plain, create.handle);
	assert(error == 0);

	/* The named device's allocation. */
	create.handle = 0U;
	create.resource_id = 0U;
	error = sharing_ioctl(&named, GPU_BLOB_CREATE, &create);
	assert(error == 0);

	/* 32 rows of 128 bytes in GOB rows fill the 4096-byte allocation exactly. */
	export_prepare(&exported, create.handle);
	exported.image.tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	exported.image.height = 33U;
	error = sharing_ioctl(&named, GPU_RESOURCE_EXPORT, &exported);
	assert(error == EINVAL);
	export_prepare(&exported, create.handle);
	exported.image.tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	exported.image.layout_parameter = 1U;
	error = sharing_ioctl(&named, GPU_RESOURCE_EXPORT, &exported);
	assert(error == EOPNOTSUPP);
	export_prepare(&exported, create.handle);
	exported.image.tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	error = sharing_ioctl(&named, GPU_RESOURCE_EXPORT, &exported);
	assert(error == 0);
	assert(exported.image.tiling == GPU_IMAGE_NV_BLOCK_LINEAR);

	/* Another device that names no layouts never reads it, even for a scanout. */
	memset(&imported, 0, sizeof(imported));
	imported.version = GPU_ABI_VERSION;
	imported.size = sizeof(imported);
	imported.fd = exported.fd;
	imported.flags = GPU_IMPORT_SCANOUT;
	error = sharing_ioctl(&plain, GPU_RESOURCE_IMPORT, &imported);
	assert(error == EXDEV);

	/* The device's own other open imports it with the stored layout. */
	memset(&imported, 0, sizeof(imported));
	imported.version = GPU_ABI_VERSION;
	imported.size = sizeof(imported);
	imported.fd = exported.fd;
	error = sharing_ioctl(&named, GPU_RESOURCE_IMPORT, &imported);
	assert(error == 0);
	assert(imported.image.tiling == GPU_IMAGE_NV_BLOCK_LINEAR);
	assert(imported.image.layout_parameter == 0U);

	/* Everything goes. */
	error = destroy_handle(&named, imported.handle);
	assert(error == 0);
	error = destroy_handle(&named, create.handle);
	assert(error == 0);
	error = filedesc_close(producer.fd, exported.fd);
	assert(error == 0);
	close_file(&named);
	close_file(&plain);
	error = drv_gpu_unregister(device);
	assert(error == 0);
	error = drv_gpu_unregister(plain_device);
	assert(error == 0);
	filedesc_destroy(producer.fd);
	producer.fd = NULL;
	gpu_test_set_process(NULL);
	assert(shared_allocations == 0U);
	assert(held_spinlocks == 0U);
	puts("gpu-layouts-host: PASS");

	/* Succeeded: every case behaved as the contract says. */
	return 0;
}

/* Turns a time into ticks of a 1000 Hz clock, rounding up, for the core's deadlines. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	/* Succeeded: one tick a millisecond. */
	return milliseconds;
}

/* Turns nanoseconds into ticks of a 1000 Hz clock, rounding up. */
uint64_t
kern_ns_to_ticks(
	uint64_t nanoseconds)
{
	/* Succeeded: whole milliseconds, a part counted as one. */
	return (nanoseconds + 999999U) / 1000000U;
}

/* Reports that the host handed over no boot framebuffer. */
int
kern_boot_framebuffer(
	const char *prefix,
	struct kern_boot_framebuffer *framebuffer)
{
	UNUSED_PARAMETER(prefix);
	UNUSED_PARAMETER(framebuffer);

	/* Succeeded: there is none. */
	return ENOENT;
}

/* Takes NVIDIA's block linear with one-GOB blocks, rows on 64 bytes, as its stride times the GOB rows. */
static int
layouts_check(
	void *opaque,
	const struct gpu_image_descriptor *image,
	uint64_t *bytes)
{
	UNUSED_PARAMETER(opaque);

	/* Only the variant this device writes. */
	if (image->tiling != GPU_IMAGE_NV_BLOCK_LINEAR || image->layout_parameter != 0U)
		return EOPNOTSUPP;
	if ((image->stride % 64U) != 0U || (image->offset % 512U) != 0U)
		return EOPNOTSUPP;

	/* Succeeded: whole GOB rows. */
	*bytes = (uint64_t)image->stride * ((image->height + 7U) & ~7U);
	return 0;
}

/* Lists block linear, then linear rows, both drawn, sampled and shared. */
static int
layouts_list(
	void *opaque,
	void *session,
	struct gpu_image_layouts *request)
{
	uint32_t flags;

	UNUSED_PARAMETER(opaque);
	UNUSED_PARAMETER(session);

	/* The uses this device gives both layouts; broken, it leaves out SHARE. */
	flags = GPU_LAYOUT_RENDER | GPU_LAYOUT_SAMPLE | GPU_LAYOUT_SHARE;
	if (layouts_broken != 0U)
		flags = GPU_LAYOUT_RENDER;

	/* Block linear first: it is the faster one. */
	request->entry[0].tiling = GPU_IMAGE_NV_BLOCK_LINEAR;
	request->entry[0].flags = flags;
	request->entry[1].tiling = GPU_IMAGE_LINEAR;
	request->entry[1].flags = flags;
	request->count = 2U;

	/* Succeeded: two layouts, fastest first. */
	return 0;
}

/* Initializes a layouts question for a 32x32 RGBA image. */
static void
layouts_prepare(
	struct gpu_image_layouts *request,
	uint32_t uses,
	uint32_t display_id)
{
	/* The header, the image and the uses; the answer starts empty. */
	memset(request, 0, sizeof(*request));
	request->version = GPU_ABI_VERSION;
	request->size = sizeof(*request);
	request->format = GPU_PIXEL_RGBA8888;
	request->width = 32U;
	request->height = 32U;
	request->uses = uses;
	request->display_id = display_id;
}
