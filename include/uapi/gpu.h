/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Fixed-width requests for the first GPU framework interface.
 */

#ifndef KERN_UAPI_GPU_H
#define KERN_UAPI_GPU_H

#include <stdint.h>
#include <uapi/ioctl.h>

#define GPU_ABI_VERSION			1U
#define GPU_CAP_RESOURCE		1U
#define GPU_CAP_CAPSET			2U
#define GPU_CAP_BLOB			4U
#define GPU_CAP_TRANSFER		8U
#define GPU_CAP_COMMAND			16U
#define GPU_CAP_PRESENT			32U
#define GPU_CAP_MAPPING			64U
#define GPU_CAP_SHARE			256U
#define GPU_CAP_NOTIFICATION		512U

/*
 * The device backs a blob created with GPU_BLOB_HOST in memory the CPU
 * reaches through its cache.  Without it the framework clears the flag
 * before the device sees the request, so asking for it is always safe.
 */
#define GPU_CAP_HOST_BLOB		65536U
#define GPU_COMMAND_CONTEXT_FENCE	1U
#define GPU_WAIT_CONSUME			1U
#define GPU_SUBMIT_MAX			64U
#define GPU_BLOB_MAPPABLE		1U
#define GPU_BLOB_SHAREABLE		2U
#define GPU_BLOB_CROSS_DEVICE		4U

/*
 * Only the CPU touches the blob: the process through its mapping and the
 * renderer's decoder in the kernel (a transport's command streams and
 * replies), never GPU work, a display or another device.  A device that
 * reports GPU_CAP_HOST_BLOB may then back it with cached system memory,
 * which the CPU reads far faster than a device's memory window; any other
 * device makes it as it makes every blob.
 */
#define GPU_BLOB_HOST			8U

/*
 * The core sets it in a GPU_GET_INFO reply when the device names its own
 * image layouts (GPU_IMAGE_LAYOUTS may list more than linear rows, and an
 * image of such a layout can be exported).  A backend never registers it.
 */
#define GPU_CAP_IMAGE_LAYOUTS		131072U

/*
 * An image's layout (gpu_image_descriptor.tiling): a family in the high
 * 16 bits and the family's own layout in the low 16.  Family 0 is common
 * to every device; the others are reserved for the vendors named, whose
 * layouts any driver of that family may read or write.  A device accepts
 * only the layouts its driver knows.
 */
#define GPU_IMAGE_FAMILY_SHIFT		16U
#define GPU_IMAGE_FAMILY_COMMON		0U
#define GPU_IMAGE_FAMILY_NVIDIA		1U
#define GPU_IMAGE_FAMILY_AMD		2U
#define GPU_IMAGE_FAMILY_INTEL		3U
#define GPU_IMAGE_FAMILY_IMAGINATION	4U
#define GPU_IMAGE_FAMILY_ARM		5U
#define GPU_IMAGE_FAMILY_BROADCOM	6U
#define GPU_IMAGE_FAMILY_VERISILICON	7U
#define GPU_IMAGE_FAMILY_LAST		GPU_IMAGE_FAMILY_VERISILICON

/* Rows of pixels one after another, stride bytes apart; layout_parameter is 0. */
#define GPU_IMAGE_LINEAR		1U

/*
 * NVIDIA's 2D block linear: GOBs of 64 bytes by 8 rows, in blocks one GOB
 * wide and 2^h GOBs tall, a row of blocks stride / 64 GOBs wide.  stride
 * is a multiple of 64, offset a multiple of 512, and the image takes
 * stride * (height rounded up to 8 * 2^h rows) bytes.  layout_parameter
 * holds h in bits 3:0 (0 to 5) and the page kind in bits 15:8 (0 for the
 * device's ordinary kind); its other bits are 0.
 */
#define GPU_IMAGE_NV_BLOCK_LINEAR	0x00010001U
#define GPU_IMAGE_NV_BLOCK_HEIGHT_MASK	0x0000000fU
#define GPU_IMAGE_NV_KIND_SHIFT		8U
#define GPU_IMAGE_NV_KIND_MASK		0x0000ff00U
#define GPU_HANDLE_CLOEXEC		1U
#define GPU_HANDLE_CLOFORK		2U
#define GPU_COPY_MAX			65536U
#define GPU_COMMAND_MAX			65536U
#define GPU_CAPSET_MAX			256U
#define GPU_PIXEL_BGRA8888		1U
#define GPU_PIXEL_RGBA8888		2U
#define GPU_RESOURCE_USAGE_STORAGE	1U

#define GPU_GET_INFO			_IOWR('G', 0, struct gpu_info)
#define GPU_RESOURCE_CREATE		_IOWR('G', 1, struct gpu_resource_create)
#define GPU_RESOURCE_DESTROY		_IOW('G', 2, struct gpu_resource_destroy)
#define GPU_GET_CAPSET			_IOWR('G', 3, struct gpu_capset)
#define GPU_BLOB_CREATE			_IOWR('G', 4, struct gpu_blob_create)
#define GPU_RESOURCE_READ		_IOW('G', 5, struct gpu_transfer)
#define GPU_RESOURCE_WRITE		_IOW('G', 6, struct gpu_transfer)
#define GPU_COMMAND			_IOW('G', 7, struct gpu_command)
#define GPU_PRESENT			_IOW('G', 8, struct gpu_present)
#define GPU_RESOURCE_MAP		_IOWR('G', 9, struct gpu_resource_map)
#define GPU_RESOURCE_EXPORT		_IOWR('G', 10, struct gpu_resource_export)
#define GPU_RESOURCE_IMPORT		_IOWR('G', 11, struct gpu_resource_import)
#define GPU_COMMAND_SUBMIT		_IOWR('G', 12, struct gpu_command_submit)
#define GPU_COMMAND_WAIT			_IOWR('G', 13, struct gpu_command_wait)
#define GPU_IMAGE_LAYOUTS		_IOWR('G', 46, struct gpu_image_layouts)

/*
 * One independently retained command and its session-owned completion identity.
 * Context fences select a backend-defined completion domain.
 * Submission success means acceptance; it never substitutes for a Vulkan result.
 * The sequence is copied out before the driver sees the command, so a
 * refused submission leaves a number in it that names no work (a wait on it
 * fails with ENOENT); only a successful return makes it a completion.
 */
struct gpu_command_submit {
	uint32_t version;
	uint32_t size;
	uint64_t address;
	uint32_t bytes;
	uint32_t flags;
	uint32_t timeline;
	uint32_t reserved;
	uint64_t sequence;
};

/*
 * One completion observation; status is a positive transport errno or zero.
 * Zero timeout only observes; UINT64_MAX waits until completion or interruption.
 * CONSUME retires a terminal record only after its complete output is copied.
 */
struct gpu_command_wait {
	uint32_t version;
	uint32_t size;
	uint64_t sequence;
	uint64_t timeout_ns;
	uint32_t flags;
	uint32_t status;
};


/*
 * One immutable image description retained with a shared allocation.
 * Version and size describe this 64-byte record. Device identity is assigned
 * by K on export; import returns the authoritative stored description.
 * tiling names the layout (GPU_IMAGE_*) and layout_parameter the variant
 * the layout's definition gives it; a linear image has 0 there.
 */
struct gpu_image_descriptor {
	uint32_t version;
	uint32_t size;
	uint32_t width;
	uint32_t height;
	uint32_t format;
	uint32_t stride;
	uint64_t offset;
	uint64_t allocation_bytes;
	uint32_t memory_type;
	uint32_t usage;
	uint32_t tiling;
	uint32_t layout_parameter;
	uint64_t device_id;
};

/* The uses of a listed layout (gpu_image_layout.flags, gpu_image_layouts.uses). */
#define GPU_LAYOUT_RENDER		1U
#define GPU_LAYOUT_SAMPLE		2U
#define GPU_LAYOUT_SCANOUT		4U
#define GPU_LAYOUT_SHARE		8U
#define GPU_IMAGE_LAYOUTS_MAX		16U

/*
 * One layout a device offers: tiling and parameter as an image description
 * names them (tiling, layout_parameter), and its uses.  RENDER: the
 * renderer draws to it as a colour attachment.  SAMPLE: the renderer
 * samples it.  SCANOUT: the display the request names shows it.  SHARE: an
 * image of it is exported from one open of the device and imported into
 * another.  reserved is zero.
 */
struct gpu_image_layout {
	uint32_t tiling;
	uint32_t parameter;
	uint32_t flags;
	uint32_t reserved;
};

/*
 * One question about the layouts of an image of a format and size, and its
 * answer, fastest first.  Inputs: version, size, format (GPU_PIXEL_*),
 * width, height, the uses every listed layout must have, and display_id
 * (0 when no display is asked about; SCANOUT needs one).  count and the
 * entries are zero on input.  No listed layout loses colour depth.  A
 * device that names no layouts of its own answers linear rows with the
 * uses its operations give it; which display generations take an image is
 * still GPU_DISPLAY_CONSTRAINTS's answer.
 */
struct gpu_image_layouts {
	uint32_t version;
	uint32_t size;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t uses;
	uint32_t display_id;
	uint32_t count;
	struct gpu_image_layout entry[GPU_IMAGE_LAYOUTS_MAX];
};

/*
 * One allocation capability exported without exposing its rendering session.
 * Input fd is -1 and image.device_id is zero; successful fd installation
 * follows copyout of the complete output, including the device identity.
 */
struct gpu_resource_export {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint32_t flags;
	int32_t fd;
	struct gpu_image_descriptor image;
};

/*
 * One capability imported into this open's independent renderer context.
 * Version, size, fd and flags are inputs. Flags zero selects renderer import;
 * GPU_IMPORT_SCANOUT selects checked native display import. Success returns
 * an owned resource handle and immutable image metadata. A native-only import
 * returns resource_id zero and cannot be used as a renderer resource identity.
 */
struct gpu_resource_import {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t handle;
	uint32_t resource_id;
	uint32_t reserved;
	struct gpu_image_descriptor image;
};

/*
 * One capability snapshot describing supported operations and allocation limits.
 */
struct gpu_info {
	uint32_t version;
	uint32_t size;
	uint32_t capabilities;
	uint32_t max_resources;
	uint64_t max_resource_bytes;
	char driver_name[32];
};

/*
 * One storage allocation whose returned handle belongs to the open session.
 */
struct gpu_resource_create {
	uint32_t version;
	uint32_t size;
	uint64_t bytes;
	uint32_t usage;
	uint32_t flags;
	uint64_t handle;
};

/*
 * One release request, stale or foreign handles never identify a resource.
 */
struct gpu_resource_destroy {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
};

/* A bounded backend capability payload copied inline without user pointers. */
struct gpu_capset {
	uint32_t version;
	uint32_t size;
	uint32_t capset_id;
	uint32_t capset_version;
	uint32_t capacity;
	uint32_t bytes;
	uint8_t data[GPU_CAPSET_MAX];
};

/*
 * One backend allocation whose optional map and independent sharing are
 * selected by flags.  flags is written back on success: a node without
 * GPU_CAP_HOST_BLOB clears GPU_BLOB_HOST, which the reply then lacks.
 */
struct gpu_blob_create {
	uint32_t version;
	uint32_t size;
	uint64_t bytes;
	uint64_t blob_id;
	uint64_t handle;
	uint32_t flags;
	uint32_t resource_id;
};

/* One bounded copy between a session-owned resource and an encoded user address. */
struct gpu_transfer {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint64_t offset;
	uint64_t address;
	uint32_t bytes;
	uint32_t reserved;
};

/* One backend command stream; receipt does not imply rendering completion. */
struct gpu_command {
	uint32_t version;
	uint32_t size;
	uint64_t address;
	uint32_t bytes;
	uint32_t flags;
};

/* One packed four-byte pixel image in an owned storage resource, not a blob. */
struct gpu_present {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
	uint64_t frame;
};

/*
 * A session-local mmap offset, never a physical address. The caller supplies
 * a live resource handle and zero output fields; mmap uses the returned offset.
 */
struct gpu_resource_map {
	uint32_t version;
	uint32_t size;
	uint64_t handle;
	uint64_t offset;
	uint64_t bytes;
};

#endif
