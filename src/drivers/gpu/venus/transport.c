/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Modern PCI and split control-queue transport private to the Venus driver.
 */

#include "internal.h"

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

#define VENUS_RING_AVAILABLE 128U
#define VENUS_RING_USED 256U
#define VENUS_WAIT_MILLISECONDS 10000U
#define VENUS_WAIT_POLLS 50000000U
#define VENUS_REQUIRED_FEATURES 0x19U

static int venus_capabilities(struct venus_transport *transport);
static int venus_capability(struct venus_transport *transport, unsigned offset, unsigned length, unsigned type);
static int venus_map_window(struct venus_transport *transport, struct venus_window *window, size_t minimum);
static int venus_map_aperture(struct venus_transport *transport);
static int venus_reset(struct venus_transport *transport);
static int venus_negotiate(struct venus_transport *transport);
static int venus_queue_start(struct venus_transport *transport);
static int venus_capset_find(struct venus_transport *transport);
static int venus_response_error(uint32_t type);

/*
 * Starts one modern PCI transport while retaining every partial acquisition.
 */
int
drv_venus_transport_start(
	struct venus_transport *transport,
	struct drv_pci_device *device)
{
	int error;

	/* Records the parent before any failed step can require rollback. */
	transport->pci = device;
	transport->stage = "dma-provider";
	transport->dma = drv_pci_device_dma(device);
	if (transport->dma == NULL)
		return ENODEV;

	/* Saves the original command bits for final hardware-owner cleanup. */
	transport->stage = "save-pci-state";
	error = drv_pci_device_save_enable_state(device, &transport->enable_state);
	if (error != 0)
		return error;

	/* Saved ownership survives every later attach or detach retry. */
	transport->saved = 1;

	/* Enables register decoding without publishing a userspace GPU. */
	transport->stage = "enable-pci-memory";
	error = drv_pci_device_enable_memory(device);
	if (error != 0)
		return error;

	/* Finds and bounds-checks each vendor capability before mapping it. */
	transport->stage = "pci-capabilities";
	error = venus_capabilities(transport);
	if (error != 0)
		return error;

	/* Maps the complete register BAR and borrows its common-configuration slice. */
	transport->stage = "map-common";
	error = venus_map_window(transport, &transport->common, 56U);
	if (error != 0)
		return error;

	/* Queue notifications need the complete advertised doorbell window. */
	transport->stage = "map-notify";
	error = venus_map_window(transport, &transport->notify, 2U);
	if (error != 0)
		return error;

	/* Reads scanout and capset counts from the bounded device configuration. */
	transport->stage = "map-configuration";
	error = venus_map_window(transport, &transport->configuration, 16U);
	if (error != 0)
		return error;

	/* Removes any firmware queue ownership before writing new addresses. */
	transport->stage = "reset";
	error = venus_reset(transport);
	if (error != 0)
		return error;

	/* Maps the complete bounded aperture before any context may create host blobs. */
	transport->stage = "map-host-visible";
	error = venus_map_aperture(transport);
	if (error != 0)
		return error;

	/* Negotiates only modern split queues and Venus-required GPU features. */
	transport->stage = "negotiate-features";
	error = venus_negotiate(transport);
	if (error != 0)
		return error;

	/* Allocates the persistent control queue and its two DMA directions. */
	transport->stage = "allocate-control-queue";
	error = venus_queue_start(transport);
	if (error != 0)
		return error;

	/* Permits device accesses only after all queue addresses are initialized. */
	transport->stage = "enable-bus-master";
	error = drv_pci_device_set_bus_master(device, true);
	if (error != 0)
		return error;

	/* DRIVER_OK transfers queue ownership to the virtual device. */
	transport->enabled = 1;
	kern_mmio_write8((uint8_t *)transport->common.mapping.address + 20U, 15U);

	/* Requires the advertised Vulkan capset instead of a VirGL-only device. */
	transport->stage = "find-venus-capset";
	error = venus_capset_find(transport);
	if (error != 0)
		return error;

	/* Succeeded: the initialized control queue can serve Venus contexts. */
	return 0;
}

/*
 * Stops DMA before releasing transport memory or restoring PCI command bits.
 */
int
drv_venus_transport_stop(
	struct venus_transport *transport)
{
	unsigned index;
	int error;

	/* A mapped common window permits a checked virtual-device reset. */
	if (transport->common.mapping.address != NULL) {
		/* Reset completion ends all access to the old queue and backing. */
		error = venus_reset(transport);
		if (error != 0)
			return error;
	}

	/* Removes bus-master permission before any DMA allocation can retire. */
	if (transport->saved != 0) {
		error = drv_pci_device_set_bus_master(transport->pci, false);
		if (error != 0)
			return error;
	}

	/* Releases the response buffer only after device ownership has ended. */
	if (transport->response.address != NULL) {
		drv_dma_free_coherent(transport->dma, &transport->response);
		if (transport->response.address != NULL)
			return EBUSY;
	}

	/* Releases the request bytes after the same reset barrier. */
	if (transport->request.address != NULL) {
		drv_dma_free_coherent(transport->dma, &transport->request);
		if (transport->request.address != NULL)
			return EBUSY;
	}

	/* The descriptor ring is last because it names the other allocations. */
	if (transport->ring.address != NULL) {
		drv_dma_free_coherent(transport->dma, &transport->ring);
		if (transport->ring.address != NULL)
			return EBUSY;
	}

	/* Releases the sole aperture mapping after reset ends all blob accesses. */
	if (transport->host_mapping.address != NULL) {
		drv_pci_device_unmap_bar(transport->pci, &transport->host_mapping);
	}

	/* Invalidates the capability view borrowed by every now-inactive blob. */
	memset(&transport->host_visible.mapping, 0, sizeof(transport->host_visible.mapping));

	/* Releases each complete register BAR only once after all queue work ends. */
	for (index = 0; index < 6U; index++) {
		/* Capability windows borrow slices of these independently owned mappings. */
		if (transport->registers[index].address != NULL) {
			drv_pci_device_unmap_bar(transport->pci, &transport->registers[index]);
		}
	}

	/* Invalidates borrowed capability views after their containing BARs retire. */
	memset(&transport->configuration.mapping, 0, sizeof(transport->configuration.mapping));
	memset(&transport->notify.mapping, 0, sizeof(transport->notify.mapping));
	memset(&transport->common.mapping, 0, sizeof(transport->common.mapping));

	/* Restores the saved command state only after all allocations retire. */
	if (transport->saved != 0) {
		error = drv_pci_device_restore_enable_state(
			transport->pci,
			&transport->enable_state);
		if (error != 0)
			return error;

		/* The saved token is no longer owned by a teardown retry. */
		transport->saved = 0;
	}

	/* Gives every claimed register or host-memory BAR back to PCI. */
	for (index = 0; index < 6U; index++) {
		/* Only successfully claimed BARs belong to this transport. */
		if (transport->claimed[index] != 0) {
			drv_pci_device_release_bar(transport->pci, index);
			transport->claimed[index] = 0;
		}
	}

	/* Succeeded: no transport allocation or PCI lease remains owned. */
	return 0;
}

/*
 * Exchanges one serialized control request without promising Vulkan completion.
 */
int
drv_venus_transport_command(
	struct venus_transport *transport,
	const void *command,
	uint32_t command_bytes,
	void *response,
	uint32_t capacity,
	uint32_t *response_bytes)
{
	uint8_t *ring;
	uint8_t *reply;
	uint64_t started;
	uint64_t last_observed;
	uint64_t now;
	uint64_t notify_byte;
	uint32_t length;
	uint32_t descriptor;
	uint32_t type;
	unsigned stalled_polls;
	uint16_t completed;
	uint16_t published;
	int error;

	/* An uncertain completion keeps persistent DMA quarantined until reset. */
	if (transport->failed != 0 || transport->enabled == 0)
		return ENODEV;

	/* Requires complete bounded command and reply storage from the caller. */
	if (command == NULL || response_bytes == NULL)
		return EINVAL;

	/* Every GPU control request contains a complete common header. */
	if (command_bytes < VENUS_HEADER_BYTES || command_bytes > VENUS_COMMAND_BYTES)
		return EINVAL;

	/* Reply storage must be representable before the queue is touched. */
	if (capacity > VENUS_RESPONSE_BYTES ||
	    (capacity != 0 && response == NULL))
		return EINVAL;

	/* Copies transient input into storage retained through any timeout. */
	*response_bytes = 0;
	memcpy(transport->request.address, command, command_bytes);
	memset(transport->response.address, 0, VENUS_RESPONSE_BYTES);
	ring = transport->ring.address;
	reply = transport->response.address;

	/* Descriptor zero contains device-readable command bytes and chains once. */
	drv_venus_store64(ring, transport->request.device_address);
	drv_venus_store32(ring + 8U, command_bytes);
	drv_venus_store16(ring + 12U, 1U);
	drv_venus_store16(ring + 14U, 1U);

	/* Descriptor one receives the device-written control response. */
	drv_venus_store64(ring + 16U, transport->response.device_address);
	drv_venus_store32(ring + 24U, VENUS_RESPONSE_BYTES);
	drv_venus_store16(ring + 28U, 2U);
	drv_venus_store16(ring + 30U, 0U);

	/* Publishes the chain only after all command and descriptor writes. */
	drv_venus_store16(
		ring + VENUS_RING_AVAILABLE + 4U + (transport->available % VENUS_QUEUE_SIZE) * 2U,
		0U);
	kern_io_write_barrier();
	transport->available++;
	published = transport->available;
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
	/* Publishes the aligned index as one indivisible little-endian store. */
	published = __builtin_bswap16(published);
#endif
	__atomic_store_n(
		(uint16_t *)(ring + VENUS_RING_AVAILABLE + 2U),
		published,
		__ATOMIC_RELEASE);
	kern_io_barrier();

	/* Notifies the selected control queue through its validated doorbell. */
	notify_byte = (uint64_t)transport->notify_offset * transport->notify_multiplier;
	kern_mmio_write16((uint8_t *)transport->notify.mapping.address + (size_t)notify_byte, 0U);

	/* Bound elapsed time while allowing asynchronous host teardown to finish. */
	started = clock_milliseconds(NULL);
	last_observed = started;
	now = started;
	stalled_polls = 0U;
	while (stalled_polls < VENUS_WAIT_POLLS) {
		/* Acquires the used index before reading its descriptor and response. */
		completed = __atomic_load_n(
			(uint16_t *)(ring + VENUS_RING_USED + 2U),
			__ATOMIC_ACQUIRE);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
		/* The used index is published atomically in little-endian wire order. */
		completed = __builtin_bswap16(completed);
#endif
		if (completed != transport->used)
			break;

		/* A progressing clock makes the elapsed deadline authoritative. */
		now = clock_milliseconds(NULL);
		if (now != last_observed) {
			/* Restart only the fallback that guards a stopped or pre-tick clock. */
			last_observed = now;
			stalled_polls = 0U;
		} else {
			/* Consecutive unchanged samples remain finite before ticks are active. */
			stalled_polls++;
		}

		/* Unsigned elapsed time also refuses a clock that moved behind its start. */
		if (now - started >= VENUS_WAIT_MILLISECONDS)
			break;

		/* Prevents a polling load from being folded into a cached result. */
		kern_compiler_barrier();
	}

	/* A missing completion prohibits reuse or release of queue memory. */
	if (completed == transport->used) {
		transport->failed = 1;
		type = drv_venus_load32(transport->request.address);
		kern_logf(
			"venus: control queue timeout command=%x elapsed=%llu ms stalled_polls=%u; DMA retained for reset\n",
			type,
			(unsigned long long)(now - started),
			stalled_polls);
		return ETIMEDOUT;
	}

	/* Only the single submitted chain may have completed on this queue. */
	if ((uint16_t)(completed - transport->used) != 1U) {
		transport->failed = 1;
		return EIO;
	}

	/* Reads device-owned response metadata after its completion publication. */
	kern_io_read_barrier();
	descriptor = drv_venus_load32(
		ring + VENUS_RING_USED + 4U + (transport->used % VENUS_QUEUE_SIZE) * 8U);
	length = drv_venus_load32(
		ring + VENUS_RING_USED + 8U + (transport->used % VENUS_QUEUE_SIZE) * 8U);
	transport->used = completed;

	/* Rejects malformed DMA lengths before reading response bytes. */
	if (descriptor != 0U ||
	    length < VENUS_HEADER_BYTES ||
	    length > VENUS_RESPONSE_BYTES) {
		transport->failed = 1;
		return EIO;
	}

	/* Converts protocol errors without confusing them with transport timeout. */
	type = drv_venus_load32(reply);
	error = venus_response_error(type);
	if (error != 0)
		return error;

	/* Reports truncation without copying beyond the caller's reply array. */
	*response_bytes = length;
	if (length > capacity)
		return EMSGSIZE;

	/* Returns only bytes covered by the completed writable descriptor. */
	if (length != 0)
		memcpy(response, reply, length);

	/* Succeeded: QEMU consumed the command, not necessarily the Vulkan work. */
	return 0;
}

/*
 * Initializes an unfenced control header for one context-scoped request.
 */
void
drv_venus_header(
	void *buffer,
	uint32_t command,
	uint32_t context)
{
	uint8_t *bytes;

	/* Clears flags, fence and ring fields instead of inventing GPU completion. */
	bytes = buffer;
	memset(bytes, 0, VENUS_HEADER_BYTES);
	drv_venus_store32(bytes, command);
	drv_venus_store32(bytes + 16U, context);

	/* Succeeded: the remaining payload may be encoded by the backend. */
	return;
}

/*
 * Decodes a little-endian queue field without alignment assumptions.
 */
uint16_t
drv_venus_load16(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;

	/* Reads each wire byte through the device-visible volatile view. */
	bytes = buffer;

	/* Succeeded: returns the host representation of the wire field. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

/*
 * Decodes a little-endian control field without alignment assumptions.
 */
uint32_t
drv_venus_load32(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;

	/* Reads each wire byte through the device-visible volatile view. */
	bytes = buffer;

	/* Succeeded: returns the host representation of the wire field. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*
 * Decodes one split 64-bit wire field.
 */
uint64_t
drv_venus_load64(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;
	uint32_t low;
	uint32_t high;

	/* Combines two independently decoded halves in wire order. */
	bytes = buffer;
	low = drv_venus_load32(bytes);
	high = drv_venus_load32(bytes + 4U);

	/* Succeeded: returns the complete wire value in host representation. */
	return (uint64_t)low | ((uint64_t)high << 32);
}

/*
 * Encodes one little-endian queue field.
 */
void
drv_venus_store16(
	void *buffer,
	uint16_t word)
{
	uint8_t *bytes;

	/* Writes the low and high wire bytes without unaligned stores. */
	bytes = buffer;
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);

	/* Succeeded: the complete field is ready for its publication barrier. */
	return;
}

/*
 * Encodes one little-endian control field.
 */
void
drv_venus_store32(
	void *buffer,
	uint32_t word)
{
	uint8_t *bytes;

	/* Writes every wire byte explicitly instead of relying on host layout. */
	bytes = buffer;
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);

	/* Succeeded: the complete field is ready for its publication barrier. */
	return;
}

/*
 * Encodes one split 64-bit wire field.
 */
void
drv_venus_store64(
	void *buffer,
	uint64_t word)
{
	uint8_t *bytes;

	/* Places the least significant half before the most significant half. */
	bytes = buffer;
	drv_venus_store32(bytes, (uint32_t)word);
	drv_venus_store32(bytes + 4U, (uint32_t)(word >> 32));

	/* Succeeded: both halves use the protocol's little-endian ordering. */
	return;
}

/* Walks the bounded PCI capability chain once and rejects cycles. */
static int
venus_capabilities(
	struct venus_transport *transport)
{
	uint8_t visited[256];
	uint8_t offset;
	uint8_t next;
	uint8_t kind;
	uint8_t length;
	uint8_t type;
	int error;

	/* Tracks each capability offset to reject malformed linked cycles. */
	memset(visited, 0, sizeof(visited));
	error = drv_pci_device_config_read8(transport->pci, 0x34U, &offset);
	if (error != 0)
		return error;

	/* Visits only conventional capabilities inside the PCI header space. */
	while (offset != 0) {
		/* Rejects unaligned or truncated capability headers. */
		if (offset < 0x40U ||
		    offset > 0xfcU ||
		    (offset & 3U) != 0)
			return EINVAL;

		/* A repeated offset would keep enumeration from making progress. */
		if (visited[offset] != 0)
			return EINVAL;

		/* Marks the header before following any device-controlled next link. */
		visited[offset] = 1;
		error = drv_pci_device_config_read8(transport->pci, offset, &kind);
		if (error != 0)
			return error;

		/* Saves the next link before decoding an optional vendor payload. */
		error = drv_pci_device_config_read8(transport->pci, offset + 1U, &next);
		if (error != 0)
			return error;

		/* Only Virtio vendor capabilities describe this device's windows. */
		if (kind == 9U) {
			/* Reads the bounded payload length before its type or BAR fields. */
			error = drv_pci_device_config_read8(transport->pci, offset + 2U, &length);
			if (error != 0)
				return error;

			/* Vendor payloads must contain the standard 16-byte prefix. */
			if (length < 16U || (unsigned)offset + length > 256U)
				return EINVAL;

			/* Classifies the window before decoding its type-specific suffix. */
			error = drv_pci_device_config_read8(transport->pci, offset + 3U, &type);
			if (error != 0)
				return error;

			/* Retains a validated window or ignores unrelated vendor types. */
			error = venus_capability(transport, offset, length, type);
			if (error != 0)
				return error;
		}

		/* Continues from the saved link, including non-Virtio capabilities. */
		offset = next;
	}

	/* Venus requires a host-visible aperture for mapped reply resources. */
	if (transport->host_visible.length == 0)
		return EOPNOTSUPP;

	/* Succeeded: all available windows are recorded with bounded offsets. */
	return 0;
}

/* Records one known window and its type-specific addressing suffix. */
static int
venus_capability(
	struct venus_transport *transport,
	unsigned offset,
	unsigned length,
	unsigned type)
{
	struct venus_window *window;
	struct drv_pci_bar bar;
	uint8_t bar_index;
	uint8_t identifier;
	uint32_t low;
	uint32_t high;
	int error;

	/* Selects only windows consumed by this private transport. */
	window = NULL;
	switch (type) {
	case 1U:
		window = &transport->common;
		break;
	case 2U:
		window = &transport->notify;
		break;
	case 4U:
		window = &transport->configuration;
		break;
	case 8U:
		/* Shared-memory IDs distinguish host visibility from other apertures. */
		error = drv_pci_device_config_read8(transport->pci, offset + 5U, &identifier);
		if (error != 0)
			return error;

		/* Other shared memory has no role in this Venus backend. */
		if (identifier != 1U)
			return 0;

		/* A host-visible capability contains the two high address halves. */
		if (length < 24U)
			return EINVAL;

		/* Keeps the host-visible window for checked blob views of one whole BAR. */
		window = &transport->host_visible;
		break;
	default:
		/* Succeeded: an unrelated capability needs no driver ownership. */
		return 0;
	}

	/* Rejects duplicate windows instead of silently replacing ownership. */
	if (window->length != 0)
		return EINVAL;

	/* Reads the BAR selector before its physical range. */
	error = drv_pci_device_config_read8(transport->pci, offset + 4U, &bar_index);
	if (error != 0)
		return error;

	/* A selector must name one of the conventional BAR positions. */
	if (bar_index >= 6U)
		return EINVAL;

	/* Reads the low offset and retains it in a 64-bit bounded value. */
	error = drv_pci_device_config_read32(transport->pci, offset + 8U, &low);
	if (error != 0)
		return error;

	/* Initializes the complete window before decoding optional high halves. */
	window->bar = bar_index;
	window->offset = low;

	/* Reads the low length independently of the offset transaction. */
	error = drv_pci_device_config_read32(transport->pci, offset + 12U, &low);
	if (error != 0)
		return error;

	/* Conventional windows occupy only their low 32-bit advertised length. */
	window->length = low;

	/* Shared memory may extend above the conventional 32-bit BAR range. */
	if (type == 8U) {
		/* Adds the high offset without narrowing the advertised location. */
		error = drv_pci_device_config_read32(transport->pci, offset + 16U, &high);
		if (error != 0)
			return error;

		/* Completes the offset before reading the separate length suffix. */
		window->offset |= (uint64_t)high << 32;
		error = drv_pci_device_config_read32(transport->pci, offset + 20U, &high);
		if (error != 0)
			return error;

		/* Completes the shared-memory size in its advertised width. */
		window->length |= (uint64_t)high << 32;
	}

	/* Validates the window against the actual BAR rather than trusting config. */
	error = drv_pci_device_bar(transport->pci, bar_index, &bar);
	if (error != 0)
		return error;

	/* Device-memory windows cannot be implemented by a port I/O BAR. */
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return EINVAL;

	/* Bounds subtraction avoids overflow in a device-controlled offset+size. */
	if (window->offset > bar.size || window->length > bar.size - window->offset)
		return EINVAL;

	/* Claims shared BARs only once even when several capabilities use them. */
	if (transport->claimed[bar_index] == 0) {
		error = drv_pci_device_claim_bar(transport->pci, bar_index);
		if (error != 0)
			return error;

		/* Records the claim immediately so every failure can release it. */
		transport->claimed[bar_index] = 1;
	}

	/* Notification offsets are multiplied by the capability's extra field. */
	if (type == 2U) {
		/* The multiplier is unavailable in a short vendor capability. */
		if (length < 20U)
			return EINVAL;

		/* Reads the doorbell scale without assuming a fixed QEMU layout. */
		error = drv_pci_device_config_read32(
			transport->pci,
			offset + 16U,
			&transport->notify_multiplier);
		if (error != 0)
			return error;
	}

	/* Succeeded: the window and its BAR lifetime now belong to the transport. */
	return 0;
}

/* Borrows a checked capability slice from one complete mapped register BAR. */
static int
venus_map_window(
	struct venus_transport *transport,
	struct venus_window *window,
	size_t minimum)
{
	struct drv_pci_bar bar;
	struct drv_pci_mapping *mapping;
	int error;

	/* Requires the exact field prefix consumed by the caller. */
	if (window->length < minimum || window->length > SIZE_MAX)
		return EINVAL;

	/* Resolves the complete BAR so page-sized mapping and relocation stay sound. */
	error = drv_pci_device_bar(transport->pci, window->bar, &bar);
	if (error != 0)
		return error;

	/* Register BARs are small; the separate host-visible aperture is never mapped here. */
	if (bar.size == 0U || bar.size > 65536U)
		return EOPNOTSUPP;

	/* Checks the borrowed view against its containing BAR before pointer arithmetic. */
	if (window->offset > bar.size || window->length > bar.size - window->offset)
		return EINVAL;

	/* Maps each register BAR once, allowing several capability slices to share it. */
	mapping = &transport->registers[window->bar];
	if (mapping->address == NULL) {
		error = drv_pci_device_map_bar(
			transport->pci,
			window->bar,
			DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
			mapping);
		if (error != 0)
			return error;
	}

	/* A slice has no independent unmap ownership; the register array retains it. */
	window->mapping.address = (uint8_t *)mapping->address + (size_t)window->offset;
	window->mapping.size = (size_t)window->length;
	window->mapping.type = mapping->type;

	/* Succeeded: the capability is addressable without mapping an unaligned fragment. */
	return 0;
}

/* Maps one bounded whole host BAR so blob slices never trigger BAR relocation. */
static int
venus_map_aperture(
	struct venus_transport *transport)
{
	struct venus_window *window;
	struct drv_pci_bar bar;
	int error;

	/* Resolves the complete PCI allocation behind the host-visible capability. */
	window = &transport->host_visible;
	error = drv_pci_device_bar(transport->pci, window->bar, &bar);
	if (error != 0)
		return error;

	/* The initial backend admits only its finite eight-MiB mapped-aperture budget. */
	if (bar.size == 0U || bar.size > VENUS_MAX_APERTURE_BYTES)
		return EOPNOTSUPP;

	/* Register and host-memory ownership must not describe the same mapped BAR. */
	if (transport->registers[window->bar].address != NULL)
		return EOPNOTSUPP;

	/* Bounds the shared-memory view before establishing any CPU access. */
	if (window->offset > bar.size || window->length > bar.size - window->offset)
		return EINVAL;

	/* Whole-BAR mapping preserves alignment if the PCI platform relocates it. */
	error = drv_pci_device_map_bar(
		transport->pci,
		window->bar,
		DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
		&transport->host_mapping);
	if (error != 0)
		return error;

	/* Blob resources borrow slices; only transport stop owns the final unmap. */
	window->mapping.address = (uint8_t *)transport->host_mapping.address + (size_t)window->offset;
	window->mapping.size = (size_t)window->length;
	window->mapping.type = transport->host_mapping.type;

	/* Succeeded: future blob mappings never enter the subrange relocation path. */
	return 0;
}

/* Observes reset completion before declaring old DMA ownership finished. */
static int
venus_reset(
	struct venus_transport *transport)
{
	uint8_t *common;
	uint8_t status;
	unsigned attempts;

	/* Withdraws DRIVER_OK before polling the required zero-status barrier. */
	common = transport->common.mapping.address;
	kern_mmio_write8(common + 20U, 0U);

	/* A finite register-read bound also works before the first kernel tick. */
	for (attempts = 0; attempts < 1000000U; attempts++) {
		/* Reads the device acknowledgment of complete transport reset. */
		status = kern_mmio_read8(common + 20U);
		if (status == 0U)
			break;
	}

	/* Failed reset leaves queue memory and backend resources quarantined. */
	if (status != 0U) {
		transport->failed = 1;
		return EBUSY;
	}

	/* No queue is enabled after the acknowledged reset. */
	transport->enabled = 0;

	/* Succeeded: transport-owned DMA allocations can be retired safely. */
	return 0;
}

/* Negotiates modern split queues and the Vulkan context/blob requirements. */
static int
venus_negotiate(
	struct venus_transport *transport)
{
	uint8_t *common;
	uint32_t high;
	uint32_t low;
	uint8_t status;

	/* Acknowledges the device and selects this transport as its driver. */
	common = transport->common.mapping.address;
	kern_mmio_write8(common + 20U, 3U);
	kern_mmio_write32(common, 0U);
	low = kern_mmio_read32(common + 4U);
	kern_mmio_write32(common, 1U);
	high = kern_mmio_read32(common + 4U);

	/* Records the offered feature words when negotiation cannot proceed. */
	kern_logf("venus: offered features %08x:%08x\n", high, low);

	/* VERSION_1 makes the modern little-endian split-queue contract mandatory. */
	if ((high & 1U) == 0)
		return EOPNOTSUPP;

	/* VIRGL, RESOURCE_BLOB and CONTEXT_INIT are required for Venus contexts. */
	if ((low & VENUS_REQUIRED_FEATURES) != VENUS_REQUIRED_FEATURES)
		return EOPNOTSUPP;

	/* Declines packed rings, indirect descriptors, and unimplemented features. */
	transport->features = VENUS_REQUIRED_FEATURES;
	kern_mmio_write32(common + 8U, 0U);
	kern_mmio_write32(common + 12U, transport->features);
	kern_mmio_write32(common + 8U, 1U);
	kern_mmio_write32(common + 12U, 1U);
	kern_mmio_write16(common + 16U, 0xffffU);
	kern_mmio_write8(common + 20U, 11U);
	status = kern_mmio_read8(common + 20U);

	/* The device may reject a feature combination even when bits were offered. */
	if ((status & 8U) == 0)
		return EOPNOTSUPP;

	/* Succeeded: both sides accepted the exact feature subset used below. */
	return 0;
}

/* Prepares a small queue using two persistent descriptors per transaction. */
static int
venus_queue_start(
	struct venus_transport *transport)
{
	uint8_t *common;
	uint8_t *ring;
	uint64_t address;
	uint64_t notify_byte;
	uint16_t maximum;
	int error;

	/* Selects control queue zero and checks its advertised descriptor capacity. */
	common = transport->common.mapping.address;
	kern_mmio_write16(common + 22U, 0U);
	maximum = kern_mmio_read16(common + 24U);
	if (maximum < VENUS_QUEUE_SIZE)
		return EOPNOTSUPP;

	/* Allocates every shared object before enabling the selected queue. */
	error = drv_dma_alloc_coherent(transport->dma, 4096U, 4096U, &transport->ring);
	if (error != 0)
		return error;

	/* Zeroes initial indices and requests polling without queue interrupts. */
	ring = transport->ring.address;
	memset(ring, 0, 4096U);
	drv_venus_store16(ring + VENUS_RING_AVAILABLE, 1U);

	/* The request survives host consumption delays and any finite timeout. */
	error = drv_dma_alloc_coherent(
		transport->dma,
		VENUS_COMMAND_BYTES,
		4096U,
		&transport->request);
	if (error != 0)
		return error;

	/* The response remains owned until reset or matching used-ring completion. */
	error = drv_dma_alloc_coherent(
		transport->dma,
		VENUS_RESPONSE_BYTES,
		4096U,
		&transport->response);
	if (error != 0)
		return error;

	/* Selects the negotiated queue shape without MSI or event-index features. */
	kern_mmio_write16(common + 24U, VENUS_QUEUE_SIZE);
	kern_mmio_write16(common + 26U, 0xffffU);
	transport->notify_offset = kern_mmio_read16(common + 30U);
	notify_byte = (uint64_t)transport->notify_offset * transport->notify_multiplier;
	if (notify_byte > transport->notify.length - 2U)
		return EINVAL;

	/* Publishes descriptor addresses in their required low/high register pairs. */
	address = transport->ring.device_address;
	kern_mmio_write32(common + 32U, (uint32_t)address);
	kern_mmio_write32(common + 36U, (uint32_t)(address >> 32));
	address = transport->ring.device_address + VENUS_RING_AVAILABLE;
	kern_mmio_write32(common + 40U, (uint32_t)address);
	kern_mmio_write32(common + 44U, (uint32_t)(address >> 32));
	address = transport->ring.device_address + VENUS_RING_USED;
	kern_mmio_write32(common + 48U, (uint32_t)address);
	kern_mmio_write32(common + 52U, (uint32_t)(address >> 32));
	kern_io_write_barrier();
	kern_mmio_write16(common + 28U, 1U);

	/* Succeeded: DRIVER_OK may now allow this initialized queue to run. */
	return 0;
}

/* Finds the Venus capset and checks that its response fits bounded storage. */
static int
venus_capset_find(
	struct venus_transport *transport)
{
	uint8_t command[32];
	uint8_t response[40];
	uint8_t *configuration;
	uint32_t count;
	uint32_t index;
	uint32_t bytes;
	uint32_t identifier;
	uint32_t type;
	unsigned found;
	int error;

	/* Reads a bounded count before issuing capability queries. */
	configuration = transport->configuration.mapping.address;
	count = kern_mmio_read32(configuration + 12U);
	if (count == 0 || count > 64U)
		return EOPNOTSUPP;

	/* Selects Venus by capset identity instead of assuming enumeration order. */
	found = 0U;
	for (index = 0; index < count; index++) {
		/* Requests one capability descriptor in the device-advertised range. */
		memset(command, 0, sizeof(command));
		drv_venus_header(command, 0x0108U, 0U);
		drv_venus_store32(command + 24U, index);
		error = drv_venus_transport_command(
			transport,
			command,
			sizeof(command),
			response,
			sizeof(response),
			&bytes);
		if (error != 0)
			return error;

		/* A capset-info response must contain its full identity and size. */
		type = drv_venus_load32(response);
		if (bytes != sizeof(response) || type != 0x1102U)
			return EIO;

		/* Only capset four carries the Venus Vulkan serialization protocol. */
		identifier = drv_venus_load32(response + 24U);
		if (identifier != 4U)
			continue;

		/* Retains only a bounded capability payload supported by the UAPI. */
		transport->capset_size = drv_venus_load32(response + 32U);
		if (transport->capset_size == 0 || transport->capset_size > 256U)
			return EOPNOTSUPP;

		/* A matching complete capset ends the search without further commands. */
		found = 1U;
		break;
	}

	/* Reports a VirGL-only device without attempting an incompatible context. */
	if (found == 0U)
		return EOPNOTSUPP;

	/* Succeeded: a Venus context can be created by each GPU session. */
	return 0;
}

/* Maps protocol refusal codes to the kernel's ordinary error convention. */
static int
venus_response_error(
	uint32_t type)
{
	/* Preserves the renderer's allocation-failure distinction. */
	if (type == 0x1201U)
		return ENOMEM;

	/* A defined protocol refusal indicates an invalid operation or identity. */
	if (type >= 0x1202U && type <= 0x1205U)
		return EINVAL;

	/* Refuses unrecognized or unspecified device failures. */
	if (type < 0x1100U || type > 0x1106U)
		return EIO;

	/* Succeeded: the response belongs to the defined success range. */
	return 0;
}
