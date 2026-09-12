/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Compile-only GPU request layout checks for ILP32 and LP64 callers.
 *
 * Each array typedef requires a positive bound when the wire contract holds.
 * These assertions use ANSI C declarations and produce no executable code.
 */

#include <uapi/gpu.h>
#include <stddef.h>

/* The complete capability response occupies 56 bytes on either data model. */
typedef char gpu_info_size_check[sizeof(struct gpu_info) == 56 ? 1 : -1];

/* The capability response begins with the interface version. */
typedef char gpu_info_version_check[offsetof(struct gpu_info, version) == 0 ? 1 : -1];

/* The declared response size follows the version without ABI padding. */
typedef char gpu_info_header_check[offsetof(struct gpu_info, size) == 4 ? 1 : -1];

/* Capability bits retain the same offset for both userspace word sizes. */
typedef char gpu_info_capabilities_check[offsetof(struct gpu_info, capabilities) == 8 ? 1 : -1];

/* The resource count remains a 32-bit field before the byte limit. */
typedef char gpu_info_count_check[offsetof(struct gpu_info, max_resources) == 12 ? 1 : -1];

/* The 64-bit resource limit is identically aligned in both layouts. */
typedef char gpu_info_limit_check[offsetof(struct gpu_info, max_resource_bytes) == 16 ? 1 : -1];

/* The driver name occupies the fixed trailing portion of the response. */
typedef char gpu_info_name_check[offsetof(struct gpu_info, driver_name) == 24 ? 1 : -1];

/* Resource creation is a 32-byte request and response on both data models. */
typedef char gpu_create_size_check[sizeof(struct gpu_resource_create) == 32 ? 1 : -1];

/* A create request begins with the same version discriminator as a query. */
typedef char gpu_create_version_check[offsetof(struct gpu_resource_create, version) == 0 ? 1 : -1];

/* The create size field identifies the complete fixed-width request. */
typedef char gpu_create_header_check[offsetof(struct gpu_resource_create, size) == 4 ? 1 : -1];

/* The requested byte count starts at offset eight in both layouts. */
typedef char gpu_create_bytes_check[offsetof(struct gpu_resource_create, bytes) == 8 ? 1 : -1];

/* The usage flags follow the byte count without pointer-sized fields. */
typedef char gpu_create_usage_check[offsetof(struct gpu_resource_create, usage) == 16 ? 1 : -1];

/* The optional flags occupy their fixed word after resource usage. */
typedef char gpu_create_flags_check[offsetof(struct gpu_resource_create, flags) == 20 ? 1 : -1];

/* The returned handle is a fixed-width value at the end of the request. */
typedef char gpu_create_handle_check[offsetof(struct gpu_resource_create, handle) == 24 ? 1 : -1];

/* Resource destruction accepts exactly sixteen bytes on either data model. */
typedef char gpu_destroy_size_check[sizeof(struct gpu_resource_destroy) == 16 ? 1 : -1];

/* A destroy request begins with its version discriminator. */
typedef char gpu_destroy_version_check[offsetof(struct gpu_resource_destroy, version) == 0 ? 1 : -1];

/* The destroy size field follows the version without ABI-dependent padding. */
typedef char gpu_destroy_header_check[offsetof(struct gpu_resource_destroy, size) == 4 ? 1 : -1];

/* The handle preserves its full 64 bits even for a 32-bit caller. */
typedef char gpu_destroy_handle_check[offsetof(struct gpu_resource_destroy, handle) == 8 ? 1 : -1];

/* Query command zero uses group G, both copy directions and a 56-byte body. */
typedef char gpu_info_command_check[GPU_GET_INFO == 0xc0384700UL ? 1 : -1];

/* Create command one uses group G, both copy directions and a 32-byte body. */
typedef char gpu_create_command_check[GPU_RESOURCE_CREATE == 0xc0204701UL ? 1 : -1];

/* Destroy command two copies a sixteen-byte request into the kernel only. */
typedef char gpu_destroy_command_check[GPU_RESOURCE_DESTROY == 0x80104702UL ? 1 : -1];

/* The inline capset has a fixed header and 256-byte pointer-free payload. */
typedef char gpu_capset_size_check[sizeof(struct gpu_capset) == 280 ? 1 : -1];

/* Capability bytes begin after six fixed-width header words. */
typedef char gpu_capset_data_check[offsetof(struct gpu_capset, data) == 24 ? 1 : -1];

/* Blob creation has identical protocol and opaque identity fields on both ABIs. */
typedef char gpu_blob_size_check[sizeof(struct gpu_blob_create) == 40 ? 1 : -1];

/* The protocol blob ID retains all 64 bits independently of pointer size. */
typedef char gpu_blob_id_check[offsetof(struct gpu_blob_create, blob_id) == 16 ? 1 : -1];

/* The core-owned blob handle uses the same fixed offset on both data models. */
typedef char gpu_blob_handle_check[offsetof(struct gpu_blob_create, handle) == 24 ? 1 : -1];

/* The backend protocol resource ID occupies the final 32-bit field. */
typedef char gpu_blob_resource_check[offsetof(struct gpu_blob_create, resource_id) == 36 ? 1 : -1];

/* Transfers encode pointers explicitly rather than using a native pointer field. */
typedef char gpu_transfer_size_check[sizeof(struct gpu_transfer) == 40 ? 1 : -1];

/* The encoded copy address remains a 64-bit field at offset twenty-four. */
typedef char gpu_transfer_address_check[offsetof(struct gpu_transfer, address) == 24 ? 1 : -1];

/* Copy lengths remain a bounded 32-bit field after the encoded address. */
typedef char gpu_transfer_bytes_check[offsetof(struct gpu_transfer, bytes) == 32 ? 1 : -1];

/* Commands retain one encoded address and two 32-bit control fields. */
typedef char gpu_command_size_check[sizeof(struct gpu_command) == 24 ? 1 : -1];

/* Command payload address encoding begins immediately after the fixed header. */
typedef char gpu_command_address_check[offsetof(struct gpu_command, address) == 8 ? 1 : -1];

/* Presentation describes a packed-pixel image without ABI-dependent padding. */
typedef char gpu_present_size_check[sizeof(struct gpu_present) == 48 ? 1 : -1];

/* Presentation dimensions begin after the handle and starting offset. */
typedef char gpu_present_width_check[offsetof(struct gpu_present, width) == 24 ? 1 : -1];

/* The diagnostic frame field occupies the final eight bytes of the request. */
typedef char gpu_present_frame_check[offsetof(struct gpu_present, frame) == 40 ? 1 : -1];

/* Capset query copies its complete inline payload in both directions. */
typedef char gpu_capset_command_check[GPU_GET_CAPSET == 0xc1184703UL ? 1 : -1];

/* Blob allocation returns both its ownership token and protocol identifier. */
typedef char gpu_blob_command_check[GPU_BLOB_CREATE == 0xc0284704UL ? 1 : -1];

/* Resource read copies a descriptor in; its encoded buffer receives the bytes. */
typedef char gpu_read_command_check[GPU_RESOURCE_READ == 0x80284705UL ? 1 : -1];

/* Resource write copies the same fixed descriptor before its payload. */
typedef char gpu_write_command_check[GPU_RESOURCE_WRITE == 0x80284706UL ? 1 : -1];

/* Command submission copies one fixed descriptor into the kernel. */
typedef char gpu_submit_command_check[GPU_COMMAND == 0x80184707UL ? 1 : -1];

/* Presentation copies one fixed packed-image descriptor into the kernel. */
typedef char gpu_present_command_check[GPU_PRESENT == 0x80304708UL ? 1 : -1];
