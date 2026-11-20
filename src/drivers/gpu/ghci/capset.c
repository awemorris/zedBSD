/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The capability record of the GHCI device that libvulkan reads before it
 * makes a Vulkan device on a native renderer.  The record is the same for
 * every GHCI GPU: what differs between GPUs is answered by the executor
 * from the GPU's own capability block.
 */

#include <stddef.h>
#include <stdint.h>

#include "ghci-words.h"

/*
 * The places of the record's words and their values: the wire version,
 * the XML version of the protocol (1.3.269), the timelines word, the
 * vendor suffix with its flags (opaque sharing, strict queues and checked
 * quiescence, the profile that lets libvulkan make a device on a native
 * renderer), and the native renderer's word: no optional queue, and the
 * Vulkan 1.1 queries answered (VULKAN_NATIVE_CAPSET_CORE11_QUERIES: the
 * executor answers vkGetPhysicalDeviceFeatures2 and Properties2 with the
 * 1.1 structures, and reads the 1.1 chains of a device's and a render
 * pass's creation and vkCmdDispatchBase) and the Vulkan 1.2 ones
 * (VULKAN_NATIVE_CAPSET_CORE12_QUERIES: the same with
 * VkPhysicalDeviceVulkan12Features and Properties, the 1.2 chain of a
 * device's creation and vkCreateRenderPass2).
 *
 * Values referenced: zedBSD userland/desktop/libvulkan/context.c
 * (vulkan_context_open: the capset id 4 and the words it reads).
 */
#define CAPSET_WIRE_AT			0U
#define CAPSET_XML_AT			4U
#define CAPSET_TIMELINES_AT		152U
#define CAPSET_VENDOR_MAGIC_AT		160U
#define CAPSET_VENDOR_FLAGS_AT		164U
#define CAPSET_NATIVE_MAGIC_AT		168U
#define CAPSET_NATIVE_FLAGS_AT		172U
#define CAPSET_WIRE_VERSION		1U
#define CAPSET_XML_VERSION		((1U << 22) | (3U << 12) | 269U)
#define CAPSET_TIMELINES		1U
#define CAPSET_VENDOR_MAGIC		0x5a424453U
#define CAPSET_VENDOR_FLAGS		7U
#define CAPSET_NATIVE_MAGIC		0x5a4e4154U
#define CAPSET_NATIVE_FLAGS		24U

static void put32(uint8_t *bytes, uint32_t at, uint32_t value);

/*
 * Fills the capability record of the native renderer protocol.
 */
void
ghci_capset_fill(
	uint8_t *data)
{
	uint32_t index;

	/* Every byte the record does not name is zero. */
	for (index = 0U; index < GHCI_CAPSET_BYTES; index++)
		data[index] = 0U;

	/* The protocol: the wire format and the XML it was generated from. */
	put32(data, CAPSET_WIRE_AT, CAPSET_WIRE_VERSION);
	put32(data, CAPSET_XML_AT, CAPSET_XML_VERSION);

	/* The device completes work on timelines. */
	put32(data, CAPSET_TIMELINES_AT, CAPSET_TIMELINES);

	/* The vendor suffix that selects the native renderer's profile. */
	put32(data, CAPSET_VENDOR_MAGIC_AT, CAPSET_VENDOR_MAGIC);
	put32(data, CAPSET_VENDOR_FLAGS_AT, CAPSET_VENDOR_FLAGS);

	/* The native renderer's own word: no optional queue family, the Vulkan 1.1 and 1.2 queries. */
	put32(data, CAPSET_NATIVE_MAGIC_AT, CAPSET_NATIVE_MAGIC);
	put32(data, CAPSET_NATIVE_FLAGS_AT, CAPSET_NATIVE_FLAGS);
}

/* Stores a 32-bit word into bytes at an offset, the lowest byte first. */
static void
put32(
	uint8_t *bytes,
	uint32_t at,
	uint32_t value)
{
	/* One byte at a time, so the record's alignment does not matter. */
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
	bytes[at + 2U] = (uint8_t)(value >> 16);
	bytes[at + 3U] = (uint8_t)(value >> 24);
}
