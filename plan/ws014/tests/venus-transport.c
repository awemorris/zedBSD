/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises modern Venus PCI transport using a synchronous split-queue peer.
 *
 * Capability slices intentionally have subpage lengths. The fixture checks
 * whole-BAR mapping, queue wrap, malformed completion and reset-safe DMA.
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/gpu/venus/transport.c"

/* Holds one deterministic PCI configuration image during each scenario. */
static uint8_t fixture_configuration[256];

/* Holds the complete modern register BAR which capability windows borrow. */
static uint8_t fixture_registers[16384];

/* Represents host visibility as a whole separately mapped BAR. */
static uint8_t fixture_aperture[8192];

/* Selects an oversized BAR to verify rejection before platform mapping. */
static unsigned fixture_oversized_aperture;

/* Records claims so initialization failures must unwind acquired BAR owners. */
static unsigned fixture_claims[6];

/* Counts complete BAR mappings, independently of their borrowed slices. */
static unsigned fixture_maps;

/* Counts live queue allocations until acknowledged reset permits freeing. */
static unsigned fixture_dma;

/* Selects the sole transport whose descriptors are visible to the peer. */
static struct venus_transport *fixture_transport;

/* Drops completions to make the bounded deadline path observable. */
static unsigned fixture_drop;

/* Corrupts one returned descriptor identity to test completion validation. */
static unsigned fixture_bad_descriptor;

/* Refuses reset acknowledgment so cleanup must preserve all DMA allocations. */
static unsigned fixture_reset_busy;

/* Advances synthetic time in finite steps without delaying the host test. */
static uint64_t fixture_clock;

/* Counts clock samples within one configured delayed-completion scenario. */
static uint32_t fixture_clock_calls;

/* Holds time fixed to exercise the pre-tick fallback without a real timer. */
static unsigned fixture_clock_frozen;

/* Models a running clock whose next millisecond needs many short polling loads. */
static unsigned fixture_clock_slow;

/* Publishes the delayed response only after this clock-sample count is reached. */
static uint32_t fixture_delayed_completion;

static void fixture_prepare(struct venus_transport *transport);
static void fixture_capability(unsigned offset, unsigned next, unsigned length, unsigned type, unsigned bar, uint32_t start, uint32_t bytes);
static void fixture_complete(void);
static void fixture_queue(void);
static void fixture_failures(void);
static void fixture_async_wait(void);

/*
 * Reads a bounded byte from the synthetic conventional PCI configuration.
 */
int
drv_pci_device_config_read8(
	struct drv_pci_device *device,
	unsigned offset,
	uint8_t *byte)
{
	/* Every parser read must remain within the advertised configuration space. */
	(void)device;
	assert(offset < sizeof(fixture_configuration));
	*byte = fixture_configuration[offset];

	/* Succeeded: the transport receives one device-controlled configuration byte. */
	return 0;
}

/*
 * Reads a bounded word from the synthetic conventional PCI configuration.
 */
int
drv_pci_device_config_read32(
	struct drv_pci_device *device,
	unsigned offset,
	uint32_t *word)
{
	/* This peer exposes only conventional configuration with aligned word reads. */
	(void)device;
	assert(offset <= sizeof(fixture_configuration) - 4U);
	assert((offset & 3U) == 0U);
	*word = drv_venus_load32(fixture_configuration + offset);

	/* Succeeded: the parser receives the complete little-endian field. */
	return 0;
}

/*
 * Supplies the synthetic PCI parent's coherent DMA provider identity.
 */
struct drv_dma_device *
drv_pci_device_dma(
	struct drv_pci_device *device)
{
	/* No physical address translation is needed by this synchronous host peer. */
	(void)device;

	/* Succeeded: the nonnull provider identity permits queue allocation. */
	return (struct drv_dma_device *)&fixture_dma;
}

/*
 * Saves the PCI command ownership token before transport acquisition.
 */
int
drv_pci_device_save_enable_state(
	struct drv_pci_device *device,
	struct drv_pci_enable_state *state)
{
	/* The fixture token is consumed once by successful final cleanup. */
	(void)device;
	assert(state->private_data[0] == 0U);
	state->private_data[0] = 1U;

	/* Succeeded: the transport owns a restorable command state. */
	return 0;
}

/*
 * Restores the synthetic command state after device ownership has ended.
 */
int
drv_pci_device_restore_enable_state(
	struct drv_pci_device *device,
	struct drv_pci_enable_state *state)
{
	/* Each saved lease must be consumed exactly once. */
	(void)device;
	assert(state->private_data[0] == 1U);
	state->private_data[0] = 0U;

	/* Succeeded: no command-register lease remains owned. */
	return 0;
}

/*
 * Permits decoding of the fixture's mapped modern registers.
 */
int
drv_pci_device_enable_memory(
	struct drv_pci_device *device)
{
	/* Memory decoding has no asynchronous side effect in this peer. */
	(void)device;

	/* Succeeded: capability register mappings may be acquired. */
	return 0;
}

/*
 * Accepts ordered bus-master changes around coherent queue ownership.
 */
int
drv_pci_device_set_bus_master(
	struct drv_pci_device *device,
	bool enabled)
{
	/* Queue allocation and lifetime counters assert the meaningful ownership. */
	(void)device;
	(void)enabled;

	/* Succeeded: the synthetic PCI permission transition was accepted. */
	return 0;
}

/*
 * Supplies a register BAR and a distinct host-visible aperture descriptor.
 */
int
drv_pci_device_bar(
	const struct drv_pci_device *device,
	unsigned index,
	struct drv_pci_bar *bar)
{
	/* Only capability-declared BARs exist in this fixture. */
	(void)device;
	if (index != 2U && index != 4U)
		return EINVAL;

	/* Initializes complete BAR identity before selecting its bounded size. */
	memset(bar, 0, sizeof(*bar));
	bar->index = index;
	bar->type = DRV_PCI_BAR_MEMORY32;
	bar->bus_address = 0xf0800000U;
	bar->size = sizeof(fixture_registers);

	/* Host visibility remains separate from the small register mapping. */
	if (index == 4U) {
		bar->bus_address = 0xf0900000U;
		bar->size = sizeof(fixture_aperture);

		/* Oversized apertures must never reach the platform fallback mapper. */
		if (fixture_oversized_aperture != 0U)
			bar->size = 256U * 1024U * 1024U;
	}

	/* Succeeded: the parser can validate the entire capability extent. */
	return 0;
}

/*
 * Claims one synthetic BAR without allowing duplicate ownership.
 */
int
drv_pci_device_claim_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	/* Multiple capabilities must share one claim for their containing BAR. */
	(void)device;
	assert(index < 6U);
	assert(fixture_claims[index] == 0U);
	fixture_claims[index] = 1U;

	/* Succeeded: the transport retains the selected BAR. */
	return 0;
}

/*
 * Returns one synthetic BAR claim during final transport cleanup.
 */
void
drv_pci_device_release_bar(
	struct drv_pci_device *device,
	unsigned index)
{
	/* Every release must correspond to an earlier successful claim. */
	(void)device;
	assert(index < 6U);
	assert(fixture_claims[index] == 1U);
	fixture_claims[index] = 0U;

	/* Succeeded: another driver could now claim the synthetic BAR. */
	return;
}

/*
 * Maps the complete modern register BAR exactly once.
 */
int
drv_pci_device_map_bar(
	struct drv_pci_device *device,
	unsigned index,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	/* Subpage capability lengths must never become independent MMIO mappings. */
	(void)device;
	assert(index == 2U || index == 4U);
	assert((flags & DRV_PCI_MAP_WRITE) != 0U);
	mapping->type = DRV_PCI_BAR_MEMORY32;

	/* Register and host-memory BARs each receive one complete mapping owner. */
	if (index == 2U) {
		assert(fixture_maps == 0U);
		mapping->address = fixture_registers;
		mapping->size = sizeof(fixture_registers);
	} else {
		assert(fixture_maps == 1U);
		assert(fixture_oversized_aperture == 0U);
		mapping->address = fixture_aperture;
		mapping->size = sizeof(fixture_aperture);
	}

	/* Counts complete BAR owners independently of their borrowed views. */
	fixture_maps++;

	/* Succeeded: capability and blob views may borrow checked slices. */
	return 0;
}

/*
 * Releases the sole complete BAR mapping after reset and DMA retirement.
 */
void
drv_pci_device_unmap_bar(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping)
{
	/* A borrowed slice must never be submitted as an independent mapping owner. */
	(void)device;
	assert(fixture_dma == 0U);

	/* The host aperture retires before the register mapping during normal teardown. */
	if (mapping->address == fixture_aperture) {
		assert(mapping->size == sizeof(fixture_aperture));
		assert(fixture_maps == 2U);
	} else {
		assert(mapping->address == fixture_registers);
		assert(mapping->size == sizeof(fixture_registers));
		assert(fixture_maps == 1U);
	}

	/* Invalidates the complete owner after checking its exact mapping extent. */
	memset(mapping, 0, sizeof(*mapping));
	fixture_maps--;

	/* Succeeded: no capability or blob view can outlive this complete BAR mapping. */
	return;
}

/*
 * Allocates coherent descriptor storage using directly addressable host memory.
 */
int
drv_dma_alloc_coherent(
	struct drv_dma_device *device,
	size_t bytes,
	size_t alignment,
	struct drv_dma_buffer *buffer)
{
	/* Heap alignment suffices for the naturally aligned queue index fixture. */
	(void)device;
	(void)alignment;
	buffer->address = calloc(1U, bytes);
	if (buffer->address == NULL)
		return ENOMEM;

	/* Counts each descriptor, command and response allocation separately. */
	buffer->device_address = (uintptr_t)buffer->address;
	buffer->size = bytes;
	fixture_dma++;

	/* Succeeded: the transport owns one device-visible allocation. */
	return 0;
}

/*
 * Releases coherent queue storage after the checked reset barrier.
 */
void
drv_dma_free_coherent(
	struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	/* No storage may be freed while reset acknowledgment is withheld. */
	(void)device;
	assert(fixture_reset_busy == 0U);
	assert(fixture_dma != 0U);
	free(buffer->address);
	memset(buffer, 0, sizeof(*buffer));
	fixture_dma--;

	/* Succeeded: this coherent allocation has fully retired. */
	return;
}

/*
 * Reads one modern byte register from the synthetic BAR.
 */
uint8_t
kern_mmio_read8(
	const volatile void *address)
{
	/* Succeeded: reads the latest device-status byte. */
	return *(const volatile uint8_t *)address;
}

/*
 * Reads one modern halfword register from the synthetic BAR.
 */
uint16_t
kern_mmio_read16(
	const volatile void *address)
{
	uint16_t word;

	/* Uses the production little-endian helper for fixture register storage. */
	word = drv_venus_load16(address);

	/* Succeeded: returns the selected queue register. */
	return word;
}

/*
 * Reads feature words according to the modern feature-selector register.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uint32_t selector;
	uint32_t word;

	/* Feature word one advertises VERSION_1; word zero advertises Venus support. */
	if (address == fixture_registers + 4U) {
		selector = drv_venus_load32(fixture_registers);
		if (selector == 0U)
			return 0x19U;

		/* The higher feature word contains the modern protocol requirement. */
		return 1U;
	}

	/* Other fields read their currently initialized fixture bytes. */
	word = drv_venus_load32(address);

	/* Succeeded: the transport sees the selected modern configuration word. */
	return word;
}

/*
 * Writes device status while supporting a deliberately withheld reset.
 */
void
kern_mmio_write8(
	volatile void *address,
	uint8_t byte)
{
	/* A busy reset leaves DRIVER_OK visible and blocks safe queue retirement. */
	if (address == fixture_registers + 20U &&
	    byte == 0U &&
	    fixture_reset_busy != 0U)
		return;

	/* Updates the synthetic status visible to the next transport poll. */
	*(volatile uint8_t *)address = byte;

	/* Succeeded: the requested status is visible to subsequent register reads. */
	return;
}

/*
 * Writes a queue field or consumes a notification through the split-queue peer.
 */
void
kern_mmio_write16(
	volatile void *address,
	uint16_t word)
{
	/* A queue-zero notification permits the peer to read the published chain. */
	if (address == fixture_registers + 12288U) {
		assert(word == 0U);
		fixture_complete();
		return;
	}

	/* Ordinary queue configuration is retained in its little-endian BAR field. */
	drv_venus_store16((void *)address, word);

	/* Succeeded: the queue register contains the new configuration. */
	return;
}

/*
 * Writes one little-endian modern configuration field.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t word)
{
	/* Retains queue addresses and feature selections in fixture register memory. */
	drv_venus_store32((void *)address, word);

	/* Succeeded: the peer observes the updated register field. */
	return;
}

/*
 * Supplies monotonic time without making timeout tests wait in real time.
 */
uint64_t
clock_milliseconds(
	void *context)
{
	/* The callback argument carries no peer identity in this serialized fixture. */
	(void)context;

	/* Count samples independently of whether the modeled timer is advancing. */
	fixture_clock_calls++;

	/* Complete delayed host teardown only after the original total-poll cap. */
	if (fixture_delayed_completion != 0U) {
		/* Deliver the response once, after the transport has waited long enough. */
		if (fixture_clock_calls == fixture_delayed_completion) {
			fixture_delayed_completion = 0U;
			fixture_drop = 0U;
			fixture_complete();
		}
	}

	/* Frozen-clock scenarios rely exclusively on the finite fallback bound. */
	if (fixture_clock_frozen == 0U) {
		/* Ordinary failures advance in seconds; asynchronous waits use slow ticks. */
		if (fixture_clock_slow == 0U) {
			fixture_clock += 1000U;
		} else {
			/* Many fast polling iterations fit inside one real clock interval. */
			if (fixture_clock_calls % 1000000U == 0U)
				fixture_clock++;
		}
	}

	/* Succeeded: the transport receives this scenario's monotonic timestamp. */
	return fixture_clock;
}

/*
 * Preserves full I/O ordering boundaries in the sequential test peer.
 */
void
kern_io_barrier(void)
{
	/* Succeeded: every prior peer operation is already visible synchronously. */
	return;
}

/*
 * Preserves I/O read acquisition in the sequential test peer.
 */
void
kern_io_read_barrier(void)
{
	/* Succeeded: response bytes precede the synthetic completion publication. */
	return;
}

/*
 * Preserves I/O write publication in the sequential test peer.
 */
void
kern_io_write_barrier(void)
{
	/* Succeeded: the peer reads only after the explicit queue notification. */
	return;
}

/*
 * Keeps the bounded poll path callable without a real asynchronous device.
 */
void
kern_compiler_barrier(void)
{
	/* Succeeded: synthetic time, rather than compiler folding, ends the poll. */
	return;
}

/*
 * Accepts transport diagnostics without matching test outcomes to log wording.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* State and allocation assertions carry this fixture's observable evidence. */
	(void)format;

	/* Succeeded: logging requires no fixture-side resource ownership. */
	return;
}

/*
 * Runs production transport acquisition, queue and rollback checks.
 */
int
main(void)
{
	/* Checks complete BAR mapping and repeated descriptor/index reuse. */
	fixture_queue();

	/* Checks parser refusal, malformed completions and delayed reset cleanup. */
	fixture_failures();

	/* Distinguish asynchronously advancing time from a stopped pre-tick clock. */
	fixture_async_wait();

	/* No scenario may leave coherent memory or register mappings live. */
	assert(fixture_dma == 0U);
	assert(fixture_maps == 0U);
	puts("Venus transport: whole-BAR/capset/queue-wrap/async-wait/stalled-clock/timeout/reset PASS");

	/* Succeeded: the real transport satisfied its bounded peer contracts. */
	return 0;
}

/* Initializes capabilities with subpage slices sharing one complete register BAR. */
static void
fixture_prepare(
	struct venus_transport *transport)
{
	/* Resets each independent device image and its retained ownership counters. */
	memset(transport, 0, sizeof(*transport));
	memset(fixture_configuration, 0, sizeof(fixture_configuration));
	memset(fixture_registers, 0, sizeof(fixture_registers));
	memset(fixture_claims, 0, sizeof(fixture_claims));
	fixture_transport = transport;
	fixture_drop = 0U;
	fixture_bad_descriptor = 0U;
	fixture_reset_busy = 0U;
	fixture_oversized_aperture = 0U;
	fixture_clock = 0U;
	fixture_clock_calls = 0U;
	fixture_clock_frozen = 0U;
	fixture_clock_slow = 0U;
	fixture_delayed_completion = 0U;

	/* Describes common, notify and device slices plus separate host visibility. */
	fixture_configuration[0x34U] = 0x40U;
	fixture_capability(0x40U, 0x50U, 16U, 1U, 2U, 0U, 2048U);
	fixture_capability(0x50U, 0x64U, 20U, 2U, 2U, 12288U, 2048U);
	drv_venus_store32(fixture_configuration + 0x60U, 4U);
	fixture_capability(0x64U, 0x74U, 16U, 4U, 2U, 8192U, 16U);
	fixture_capability(0x74U, 0U, 24U, 8U, 4U, 0U, 8192U);
	fixture_configuration[0x79U] = 1U;

	/* Advertises one usable queue, one scanout and one Venus capability set. */
	drv_venus_store16(fixture_registers + 24U, 8U);
	drv_venus_store32(fixture_registers + 8192U + 8U, 1U);
	drv_venus_store32(fixture_registers + 8192U + 12U, 1U);

	/* Succeeded: the transport may parse and initialize this synthetic device. */
	return;
}

/* Encodes one synthetic vendor capability using its standardized wire prefix. */
static void
fixture_capability(
	unsigned offset,
	unsigned next,
	unsigned length,
	unsigned type,
	unsigned bar,
	uint32_t start,
	uint32_t bytes)
{
	/* Writes identity, next-link and the bounded window descriptor. */
	fixture_configuration[offset] = 9U;
	fixture_configuration[offset + 1U] = (uint8_t)next;
	fixture_configuration[offset + 2U] = (uint8_t)length;
	fixture_configuration[offset + 3U] = (uint8_t)type;
	fixture_configuration[offset + 4U] = (uint8_t)bar;
	drv_venus_store32(fixture_configuration + offset + 8U, start);
	drv_venus_store32(fixture_configuration + offset + 12U, bytes);

	/* Succeeded: the production parser can consume this capability. */
	return;
}

/* Consumes the actual published descriptor chain and writes a used-ring entry. */
static void
fixture_complete(void)
{
	struct venus_transport *transport;
	uint8_t *ring;
	uint8_t *response;
	uint32_t type;
	uint32_t bytes;
	uint32_t descriptor;
	uint16_t available;
	uint16_t used;
	uint64_t address;

	/* A dropped request remains device-owned until the transport resets. */
	if (fixture_drop != 0U)
		return;

	/* Reads the actual production descriptors after their availability publication. */
	transport = fixture_transport;
	ring = transport->ring.address;
	available = drv_venus_load16(ring + VENUS_RING_AVAILABLE + 2U);
	assert(available == transport->available);
	address = drv_venus_load64(ring);
	assert(address == transport->request.device_address);
	address = drv_venus_load64(ring + 16U);
	assert(address == transport->response.device_address);
	descriptor = drv_venus_load16(ring + 12U);
	assert(descriptor == 1U);
	descriptor = drv_venus_load16(ring + 28U);
	assert(descriptor == 2U);

	/* Replies to initialization's capability query or a no-data test command. */
	type = drv_venus_load32(transport->request.address);
	response = transport->response.address;
	memset(response, 0, VENUS_RESPONSE_BYTES);
	bytes = 24U;
	drv_venus_store32(response, 0x1100U);

	/* Supplies a complete bounded capset descriptor for the required Venus ID. */
	if (type == 0x108U) {
		bytes = 40U;
		drv_venus_store32(response, 0x1102U);
		drv_venus_store32(response + 24U, 4U);
		drv_venus_store32(response + 32U, 32U);
	}

	/* Publishes the completed descriptor and response length before the used index. */
	used = transport->used;
	drv_venus_store32(ring + VENUS_RING_USED + 4U + (used % VENUS_QUEUE_SIZE) * 8U, fixture_bad_descriptor);
	drv_venus_store32(ring + VENUS_RING_USED + 8U + (used % VENUS_QUEUE_SIZE) * 8U, bytes);
	drv_venus_store16(ring + VENUS_RING_USED + 2U, (uint16_t)(used + 1U));

	/* Succeeded: the production poll can observe exactly one completed chain. */
	return;
}

/* Checks initialized capability slices and repeated used/available index turnover. */
static void
fixture_queue(void)
{
	struct venus_transport transport;
	uint8_t command[24];
	uint8_t response[24];
	uint32_t bytes;
	unsigned index;
	int error;

	/* Initializes through the actual parser, feature handshake and split queue. */
	fixture_prepare(&transport);
	error = drv_venus_transport_start(&transport, NULL);
	assert(error == 0);
	assert(fixture_maps == 2U);
	assert(fixture_dma == 3U);
	assert(transport.capset_size == 32U);
	assert(transport.notify.mapping.address == fixture_registers + 12288U);
	assert(transport.configuration.mapping.address == fixture_registers + 8192U);

	/* Crosses both the eight-entry ring boundary and the low index-byte rollover. */
	drv_venus_header(command, 0x201U, 1U);
	for (index = 0; index < 300U; index++) {
		error = drv_venus_transport_command(&transport, command, sizeof(command), response, sizeof(response), &bytes);
		assert(error == 0);
		assert(bytes == 24U);
	}

	/* All issued commands, including initialization, completed exactly once. */
	assert(transport.available == 301U);
	assert(transport.used == 301U);
	error = drv_venus_transport_stop(&transport);
	assert(error == 0);
	assert(fixture_claims[2] == 0U);
	assert(fixture_claims[4] == 0U);

	/* Succeeded: complete BAR ownership and queue reuse both retired cleanly. */
	return;
}

/* Checks malformed capability and completion refusal plus reset-safe timeout cleanup. */
static void
fixture_failures(void)
{
	struct venus_transport transport;
	uint8_t command[24];
	uint8_t response[24];
	uint32_t bytes;
	int error;

	/* A cyclic capability list must fail before mapping or queue allocation. */
	fixture_prepare(&transport);
	fixture_configuration[0x41U] = 0x40U;
	error = drv_venus_transport_start(&transport, NULL);
	assert(error == EINVAL);
	error = drv_venus_transport_stop(&transport);
	assert(error == 0);
	assert(fixture_claims[2] == 0U);
	assert(fixture_dma == 0U);

	/* A 256-MiB aperture is rejected before unsafe subrange relocation is possible. */
	fixture_prepare(&transport);
	fixture_oversized_aperture = 1U;
	error = drv_venus_transport_start(&transport, NULL);
	assert(error == EOPNOTSUPP);
	assert(fixture_maps == 1U);
	assert(fixture_dma == 0U);
	error = drv_venus_transport_stop(&transport);
	assert(error == 0);
	assert(fixture_maps == 0U);

	/* A mismatched used descriptor poisons the queue instead of accepting its reply. */
	fixture_prepare(&transport);
	error = drv_venus_transport_start(&transport, NULL);
	assert(error == 0);
	fixture_bad_descriptor = 1U;
	drv_venus_header(command, 0x201U, 1U);
	error = drv_venus_transport_command(&transport, command, sizeof(command), response, sizeof(response), &bytes);
	assert(error == EIO);
	assert(transport.failed != 0U);
	assert(fixture_dma == 3U);
	error = drv_venus_transport_stop(&transport);
	assert(error == 0);

	/* Missing completion retains every persistent queue allocation until reset. */
	fixture_prepare(&transport);
	error = drv_venus_transport_start(&transport, NULL);
	assert(error == 0);
	fixture_drop = 1U;
	error = drv_venus_transport_command(&transport, command, sizeof(command), response, sizeof(response), &bytes);
	assert(error == ETIMEDOUT);
	assert(fixture_dma == 3U);

	/* A failed reset cannot release even one of the three coherent allocations. */
	fixture_reset_busy = 1U;
	error = drv_venus_transport_stop(&transport);
	assert(error == EBUSY);
	assert(fixture_dma == 3U);
	assert(fixture_maps == 2U);

	/* The eventual acknowledgment permits the same cleanup operation to finish. */
	fixture_reset_busy = 0U;
	error = drv_venus_transport_stop(&transport);
	assert(error == 0);
	assert(fixture_dma == 0U);
	assert(fixture_maps == 0U);

	/* Succeeded: parser and queue failures preserved correct cleanup ownership. */
	return;
}

/* Wait through delayed host completion while preserving the stopped-clock guard. */
static void
fixture_async_wait(void)
{
	struct venus_transport transport;
	uint8_t command[32];
	uint8_t response[24];
	uint32_t bytes;
	int error;

	/* Establish the normal device before delaying one host resource-unmap reply. */
	fixture_prepare(&transport);
	error = drv_venus_transport_start(&transport, NULL);
	assert(error == 0);

	/* A slow advancing clock permits more than the old aggregate poll cap. */
	fixture_drop = 1U;
	fixture_clock_slow = 1U;
	fixture_clock_calls = 0U;
	fixture_delayed_completion = VENUS_WAIT_POLLS + 4U;
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x209U, 1U);
	drv_venus_store32(command + 24U, 3U);
	error = drv_venus_transport_command(&transport, command, sizeof(command), response, sizeof(response), &bytes);
	assert(error == 0);
	assert(fixture_clock_calls > VENUS_WAIT_POLLS);
	assert(transport.failed == 0U);
	assert(transport.available == transport.used);
	assert(fixture_dma == 3U);

	/* Completed asynchronous teardown permits ordinary transport release. */
	error = drv_venus_transport_stop(&transport);
	assert(error == 0);
	assert(fixture_dma == 0U);

	/* Freeze the clock only after initialization has completed successfully. */
	fixture_prepare(&transport);
	error = drv_venus_transport_start(&transport, NULL);
	assert(error == 0);
	fixture_drop = 1U;
	fixture_clock_frozen = 1U;
	fixture_clock_calls = 0U;
	error = drv_venus_transport_command(&transport, command, sizeof(command), response, sizeof(response), &bytes);
	assert(error == ETIMEDOUT);
	assert(fixture_clock_calls == VENUS_WAIT_POLLS + 1U);
	assert(transport.failed != 0U);
	assert(fixture_dma == 3U);

	/* An unacknowledged reset still cannot free timed-out device-owned DMA. */
	fixture_reset_busy = 1U;
	error = drv_venus_transport_stop(&transport);
	assert(error == EBUSY);
	assert(fixture_dma == 3U);

	/* Only the eventual reset acknowledgment returns the retained allocations. */
	fixture_reset_busy = 0U;
	error = drv_venus_transport_stop(&transport);
	assert(error == 0);
	assert(fixture_dma == 0U);

	/* Succeeded: running and stopped clocks both preserve finite safe ownership. */
	return;
}
