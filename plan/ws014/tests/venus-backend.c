/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises real Venus resource ownership against a deterministic host peer.
 *
 * This fixture checks protocol fields, display arbitration, context isolation,
 * timeout retention and aperture reuse. It does not emulate Vulkan execution.
 */

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/gpu/venus/venus.c"

/* Counts local ownership so quarantine and final retirement are observable. */
static unsigned fixture_allocations;

/* Counts coherent buffers independently of the resource wrappers owning them. */
static unsigned fixture_dma;

/* Counts unmap calls so borrowed blob views cannot release a shared mapping. */
static unsigned fixture_mappings;

/* Represents one complete transport-owned aperture throughout a scenario. */
static uint8_t fixture_aperture[8192];

/* Selects one protocol command whose completion becomes uncertain once. */
static uint32_t fixture_timeout;

/* Records actual command bytes for structural checks by the synthetic host. */
static unsigned fixture_commands[0x300];

/* Holds the last selected scanout and backing resource for display checks. */
static uint32_t fixture_scanout;

/* Retains PCI backend ownership until a checked detach consumes it. */
static void *fixture_private;

/* Injects a failed reset without allowing the backend to retire DMA. */
static unsigned fixture_reset_busy;

/* Keeps one synthetic GPU registration handle alive through EBUSY retry. */
static unsigned fixture_registration;

/* Models a retained session which blocks publication withdrawal. */
static unsigned fixture_unregister_busy;

static void fixture_lifecycle(void);
static void fixture_storage(void);
static void fixture_blobs(void);
static void fixture_timeout_retention(void);
static void fixture_controller(struct venus_controller *controller, uint8_t *configuration);
static void fixture_drain(struct venus_controller *controller);

/*
 * Allocates counted host memory for production kernel ownership records.
 */
void *
kern_malloc(
	size_t bytes)
{
	void *allocation;

	/* Makes allocation ownership visible to the final leak assertion. */
	allocation = malloc(bytes);
	if (allocation == NULL)
		return NULL;

	/* Counts only successfully allocated records. */
	fixture_allocations++;

	/* Succeeded: the test's kernel caller owns the allocation. */
	return allocation;
}

/*
 * Allocates zeroed memory through the same counted kernel allocator.
 */
void *
kern_calloc(
	size_t count,
	size_t bytes)
{
	void *allocation;

	/* The bounded fixture inputs cannot overflow the requested byte product. */
	allocation = kern_malloc(count * bytes);
	if (allocation == NULL)
		return NULL;

	/* Matches the production allocator's zero-initialization contract. */
	memset(allocation, 0, count * bytes);

	/* Succeeded: the caller owns a fully zeroed record. */
	return allocation;
}

/*
 * Releases one counted kernel ownership record.
 */
void
kern_free(
	void *allocation)
{
	/* A null cleanup carries no ownership obligation. */
	if (allocation == NULL)
		return;

	/* Verifies that every free consumes an existing allocation. */
	assert(fixture_allocations != 0U);
	fixture_allocations--;
	free(allocation);

	/* Succeeded: no test-owned allocation remains at this address. */
	return;
}

/*
 * Initializes a deterministic single-threaded mutex fixture.
 */
int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	/* Scheduler behavior is outside this ownership fixture's scope. */
	(void)rank;
	(void)name;
	memset(mutex, 0, sizeof(*mutex));

	/* Succeeded: callbacks may use the single-threaded lock contract. */
	return 0;
}

/*
 * Acquires the fixture's nonconcurrent callback ownership.
 */
void
mutex_lock(
	struct mutex *mutex)
{
	/* This host test executes callbacks sequentially without a kernel scheduler. */
	(void)mutex;

	/* Succeeded: the sole test thread owns this callback interval. */
	return;
}

/*
 * Releases the fixture's nonconcurrent callback ownership.
 */
void
mutex_unlock(
	struct mutex *mutex)
{
	/* The fixture has no competing callback thread. */
	(void)mutex;

	/* Succeeded: the next callback can enter its interval. */
	return;
}

/*
 * Supplies counted coherent storage using host heap memory.
 */
int
drv_dma_alloc_coherent(
	struct drv_dma_device *device,
	size_t bytes,
	size_t alignment,
	struct drv_dma_buffer *buffer)
{
	/* This peer observes addresses directly without physical DMA translation. */
	(void)device;
	(void)alignment;
	buffer->address = calloc(1U, bytes);
	if (buffer->address == NULL)
		return ENOMEM;

	/* Tracks coherent ownership separately from ordinary kernel allocations. */
	buffer->size = bytes;
	buffer->device_address = (uintptr_t)buffer->address;
	fixture_dma++;

	/* Succeeded: the backend owns one coherent allocation. */
	return 0;
}

/*
 * Releases coherent storage only when the driver requests final retirement.
 */
void
drv_dma_free_coherent(
	struct drv_dma_device *device,
	struct drv_dma_buffer *buffer)
{
	/* Only allocated buffers participate in the ownership count. */
	(void)device;
	assert(buffer->address != NULL);
	assert(fixture_dma != 0U);
	free(buffer->address);
	memset(buffer, 0, sizeof(*buffer));
	fixture_dma--;

	/* Succeeded: the descriptor no longer names live host memory. */
	return;
}

/*
 * Maps a synthetic host-visible aperture extent for copied resource access.
 */
int
drv_pci_device_map_bar_region(
	struct drv_pci_device *device,
	unsigned bar,
	uint64_t offset,
	size_t bytes,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	/* The backend must request the retained host-visible BAR and writable view. */
	(void)device;
	(void)offset;
	assert(bar == 4U);
	assert((flags & DRV_PCI_MAP_WRITE) != 0U);

	/* Gives each mapped blob an independent synthetic host allocation. */
	mapping->address = calloc(1U, bytes);
	if (mapping->address == NULL)
		return ENOMEM;

	/* Keeps mapping lifetime observable across failed resource destruction. */
	mapping->size = bytes;
	fixture_mappings++;

	/* Succeeded: resource copies can reach this host-backed extent. */
	return 0;
}

/*
 * Retires one synthetic aperture mapping.
 */
void
drv_pci_device_unmap_bar(
	struct drv_pci_device *device,
	struct drv_pci_mapping *mapping)
{
	/* A missing mapping requires no fixture storage. */
	(void)device;
	if (mapping->address == NULL)
		return;

	/* Verifies and releases exactly one previously mapped allocation. */
	assert(fixture_mappings != 0U);
	free(mapping->address);
	memset(mapping, 0, sizeof(*mapping));
	fixture_mappings--;

	/* Succeeded: this kernel view no longer exposes host memory. */
	return;
}

/*
 * Reads one synthetic device-configuration field.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uint32_t word;

	/* Uses the same little-endian device field convention as the peer. */
	word = drv_venus_load32(address);

	/* Succeeded: the caller receives the configuration field. */
	return word;
}

/*
 * Preserves the production read-ordering call in a sequential host fixture.
 */
void
kern_io_read_barrier(void)
{
	/* Succeeded: the single host thread has no reordered synthetic DMA. */
	return;
}

/*
 * Preserves the production write-ordering call in a sequential host fixture.
 */
void
kern_io_write_barrier(void)
{
	/* Succeeded: the synthetic peer observes all prior fixture writes. */
	return;
}

/*
 * Accepts diagnostic output without coupling assertions to log wording.
 */
void
kern_logf(
	const char *format,
	...)
{
	/* Protocol and ownership counters provide the assertions for this fixture. */
	(void)format;

	/* Succeeded: logging imposes no additional test behavior. */
	return;
}

/*
 * Encodes one protocol header for the production backend's host peer.
 */
void
drv_venus_header(
	void *buffer,
	uint32_t command,
	uint32_t context)
{
	/* Matches the transport header without executing any device queue code. */
	memset(buffer, 0, 24U);
	drv_venus_store32(buffer, command);
	drv_venus_store32((uint8_t *)buffer + 16U, context);

	/* Succeeded: the peer can independently inspect the command identity. */
	return;
}

/*
 * Decodes a little-endian field using the independent test peer.
 */
uint32_t
drv_venus_load32(
	const volatile void *buffer)
{
	const volatile uint8_t *bytes;

	/* Device protocol reads must remain observable to the compiler. */
	bytes = buffer;

	/* Succeeded: the field is represented in native test arithmetic. */
	return bytes[0] | ((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

/*
 * Encodes one little-endian field for fixture response and driver requests.
 */
void
drv_venus_store32(
	void *buffer,
	uint32_t word)
{
	uint8_t *bytes;

	/* Writes a complete wire field without alignment assumptions. */
	bytes = buffer;
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);

	/* Succeeded: the peer can decode this initialized wire field. */
	return;
}

/*
 * Encodes a split 64-bit protocol quantity for fixture requests.
 */
void
drv_venus_store64(
	void *buffer,
	uint64_t word)
{
	/* Retains low-before-high wire byte order independently of host alignment. */
	drv_venus_store32(buffer, (uint32_t)word);
	drv_venus_store32((uint8_t *)buffer + 4U, (uint32_t)(word >> 32));

	/* Succeeded: the complete quantity occupies its protocol field. */
	return;
}

/*
 * Responds as a strict bounded peer to the actual backend control payloads.
 */
int
drv_venus_transport_command(
	struct venus_transport *transport,
	const void *input,
	uint32_t bytes,
	void *output,
	uint32_t capacity,
	uint32_t *response_bytes)
{
	const uint8_t *command;
	uint32_t type;
	uint32_t context;
	uint32_t field;

	/* A timed-out transport must refuse subsequent work until reset. */
	if (transport->failed != 0U)
		return ENODEV;

	/* Checks common payload bounds before reading protocol identity. */
	command = input;
	assert(bytes >= 24U);
	type = drv_venus_load32(command);
	context = drv_venus_load32(command + 16U);
	assert(type < 0x300U);
	fixture_commands[type]++;

	/* Injects uncertainty before the host acknowledgment, preserving driver DMA. */
	if (type == fixture_timeout) {
		fixture_timeout = 0U;
		transport->failed = 1U;
		return ETIMEDOUT;
	}

	/* Checks command-specific protocol structure independently of callbacks. */
	switch (type) {
	case 0x200U:
		assert(bytes == 96U);
		assert(context != 0U);
		field = drv_venus_load32(command + 28U);
		assert(field == 4U);
		break;
	case 0x10cU:
		assert(bytes == 56U);
		assert(context != 0U);
		field = drv_venus_load32(command + 28U);
		assert(field == 2U);
		field = drv_venus_load32(command + 32U);
		assert(field == GPU_BLOB_MAPPABLE);
		field = drv_venus_load32(command + 36U);
		assert(field == 0U);
		break;
	case 0x208U:
		assert(bytes == 40U);
		assert(capacity >= 32U);
		memset(output, 0, 32U);
		drv_venus_store32(output, 0x1106U);
		drv_venus_store32((uint8_t *)output + 24U, 1U);
		*response_bytes = 32U;
		return 0;
	case 0x207U:
		assert(context != 0U);
		field = drv_venus_load32(command + 24U);
		assert(bytes == field + 32U);
		break;
	case 0x106U:
		assert(bytes == 48U);
		field = drv_venus_load32(command + 28U);
		assert(field == 1U);
		break;
	case 0x103U:
		assert(bytes == 48U);
		fixture_scanout = drv_venus_load32(command + 44U);
		break;
	case 0x105U:
		assert(bytes == 56U);
		break;
	default:
		break;
	}

	/* Supplies the no-data acknowledgment expected by state-changing commands. */
	assert(capacity >= 24U);
	memset(output, 0, 24U);
	drv_venus_store32(output, 0x1100U);
	*response_bytes = 24U;

	/* Succeeded: this fixture command was structurally valid and acknowledged. */
	return 0;
}

/*
 * Gives the synthetic PCI owner a retryable backend pointer.
 */
int
drv_pci_device_set_driver_data(
	struct drv_pci_device *device,
	void *private_data)
{
	/* Device identity is irrelevant to this single-controller lifecycle fixture. */
	(void)device;
	fixture_private = private_data;

	/* Succeeded: PCI retains the exact staged backend pointer. */
	return 0;
}

/*
 * Resolves the synthetic PCI owner's retained backend pointer.
 */
void *
drv_pci_device_driver_data(
	const struct drv_pci_device *device)
{
	/* The fixture supplies only one independently owned PCI controller. */
	(void)device;

	/* Succeeded: the callback receives its current hardware owner. */
	return fixture_private;
}

/*
 * Retains the existence of PCI's staged publication contract.
 */
int
drv_pci_device_set_service(
	struct drv_pci_device *device,
	const struct drv_pci_service_interface *service,
	void *argument)
{
	/* Checks the backend-owned adapter without replacing the generic PCI core. */
	(void)device;
	assert(service->publish != NULL);
	assert(service->unpublish != NULL);
	assert(argument == fixture_private);

	/* Succeeded: the fixture recognizes a complete publication contract. */
	return 0;
}

/*
 * Supplies a transport initialization boundary for retained lifecycle references.
 */
int
drv_venus_transport_start(
	struct venus_transport *transport,
	struct drv_pci_device *device)
{
	/* Lifecycle tests initialize resources independently of PCI capability parsing. */
	transport->pci = device;
	transport->stage = "fixture";
	transport->enabled = 1U;

	/* Succeeded: this fixture models a ready transport without hardware queues. */
	return 0;
}

/*
 * Models the checked reset boundary required before DMA retirement.
 */
int
drv_venus_transport_stop(
	struct venus_transport *transport)
{
	/* A missing reset acknowledgment preserves every backend allocation. */
	if (fixture_reset_busy != 0U)
		return EBUSY;

	/* Acknowledged reset ends all simulated device access to retained storage. */
	transport->enabled = 0U;

	/* Succeeded: the backend may now retire quarantined allocations. */
	return 0;
}

/*
 * Supplies one ordinary GPU publication handle to the backend's PCI adapter.
 */
int
drv_gpu_register(
	const struct drv_gpu_ops *operations,
	void *private_data,
	struct drv_gpu_device **result)
{
	/* Checks the current typed operations and their actual backend owner. */
	assert(operations->version == DRV_GPU_INTERFACE_VERSION);
	assert(private_data == fixture_private);
	assert(fixture_registration == 0U);

	/* The handle is only an opaque identity in this backend lifecycle fixture. */
	fixture_registration = 1U;
	*result = (struct drv_gpu_device *)&fixture_registration;

	/* Succeeded: the service owns one withdrawable publication handle. */
	return 0;
}

/*
 * Models EBUSY-preserving withdrawal and one-time registration consumption.
 */
int
drv_gpu_unregister(
	struct drv_gpu_device *device)
{
	/* Only the retained publication handle may be consumed. */
	assert(device == (struct drv_gpu_device *)&fixture_registration);
	assert(fixture_registration == 1U);

	/* A retained session prevents the hardware owner's detach callback. */
	if (fixture_unregister_busy != 0U)
		return EBUSY;

	/* A successful withdrawal consumes the registration exactly once. */
	fixture_registration = 0U;

	/* Succeeded: the backend may clear its publication handle. */
	return 0;
}

/*
 * Accepts registration of the production backend's bounded PCI identity table.
 */
int
drv_pci_driver_register(
	struct drv_pci_driver *driver)
{
	/* The fixture does not replace real PCI matching tests. */
	assert(driver->id_count == 1U);
	assert(driver->ids[0].vendor == 0x1af4U);
	assert(driver->ids[0].device == 0x1050U);

	/* Succeeded: the backend supplied its expected modern GPU identity. */
	return 0;
}

/*
 * Runs bounded backend ownership checks against the synthetic host peer.
 */
int
main(void)
{
	/* Checks publication withdrawal and reset failure retain their private owner. */
	fixture_lifecycle();

	/* Checks context separation, display control and coherent backing lifetime. */
	fixture_storage();

	/* Checks blob mapping, copied bytes and aperture extent reuse. */
	fixture_blobs();

	/* Checks failure retention until a device reset ends uncertain ownership. */
	fixture_timeout_retention();

	/* Confirms every wrapper, DMA buffer and mapping eventually retires. */
	assert(fixture_allocations == 0U);
	assert(fixture_dma == 0U);
	assert(fixture_mappings == 0U);
	puts("Venus backend: context/storage/blob/present/timeout ownership PASS");

	/* Succeeded: the actual backend met all bounded fixture contracts. */
	return 0;
}

/* Checks the backend adapter and hardware reset retry through real callbacks. */
static void
fixture_lifecycle(void)
{
	struct venus_controller *controller;
	struct drv_gpu_device *registration;
	int error;

	/* Attaches through the real callback, retaining PCI's private owner. */
	error = venus_attach(NULL, NULL);
	assert(error == 0);
	controller = fixture_private;
	assert(controller != NULL);

	/* Publishes the real typed operations through the backend's own adapter. */
	error = venus_publish(NULL, controller);
	assert(error == 0);
	registration = controller->gpu;
	assert(registration != NULL);

	/* EBUSY preserves publication and prevents premature hardware detach. */
	fixture_unregister_busy = 1U;
	error = venus_unpublish(NULL, controller);
	assert(error == EBUSY);
	assert(controller->gpu == registration);
	error = venus_detach(NULL, 0U);
	assert(error == EBUSY);
	assert(fixture_private == controller);

	/* Successful retry consumes only the GPU registration before reset. */
	fixture_unregister_busy = 0U;
	error = venus_unpublish(NULL, controller);
	assert(error == 0);
	assert(controller->gpu == NULL);

	/* Failed reset still retains the private owner despite completed withdrawal. */
	fixture_reset_busy = 1U;
	error = venus_detach(NULL, 0U);
	assert(error == EBUSY);
	assert(fixture_private == controller);

	/* Acknowledged reset permits the final controller allocation to retire. */
	fixture_reset_busy = 0U;
	error = venus_detach(NULL, 0U);
	assert(error == 0);
	assert(fixture_private == NULL);

	/* Succeeded: publication and reset retries preserved all required owners. */
	return;
}

/* Initializes a controller with a small deterministic host-visible aperture. */
static void
fixture_controller(
	struct venus_controller *controller,
	uint8_t *configuration)
{
	int error;

	/* Gives each scenario fresh IDs, scanout configuration and aperture bounds. */
	memset(controller, 0, sizeof(*controller));
	memset(configuration, 0, 16U);
	drv_venus_store32(configuration + 8U, 1U);
	controller->next_context = 1U;
	controller->next_resource = 1U;
	controller->transport.configuration.mapping.address = configuration;
	controller->transport.host_visible.length = 8192U;
	controller->transport.host_visible.bar = 4U;
	controller->transport.host_visible.mapping.address = fixture_aperture;
	controller->transport.host_visible.mapping.size = sizeof(fixture_aperture);
	controller->transport.enabled = 1U;

	/* Preserves the backend's mutex initialization contract in this fixture. */
	error = mutex_init(&controller->mutex, LOCK_RANK_DEVICE, "venus-test");
	assert(error == 0);

	/* Succeeded: the scenario may open production backend sessions. */
	return;
}

/* Checks independent contexts and exclusive scanout with real backend callbacks. */
static void
fixture_storage(void)
{
	struct venus_controller controller;
	struct gpu_resource_create allocation;
	struct gpu_present present;
	uint8_t configuration[16];
	uint8_t pixels[64];
	uint8_t copied[64];
	void *first;
	void *second;
	void *storage;
	void *second_storage;
	void *oversized_stride;
	unsigned host_creates;
	int error;

	/* Opens two sessions sharing one actual backend controller. */
	fixture_controller(&controller, configuration);
	error = venus_open(&controller, &first);
	assert(error == 0);
	error = venus_open(&controller, &second);
	assert(error == 0);
	assert(((struct venus_session *)first)->context != ((struct venus_session *)second)->context);

	/* Creates storage large enough for a padded four-row image. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.bytes = sizeof(pixels);
	error = venus_resource_create(&controller, first, &allocation, &storage);
	assert(error == 0);
	assert(fixture_dma == 1U);

	/* Round-trips copied pixels without exposing the coherent memory pointer. */
	memset(pixels, 0xa5, sizeof(pixels));
	error = venus_resource_write(&controller, first, storage, 0U, pixels, sizeof(pixels));
	assert(error == 0);
	error = venus_resource_read(&controller, first, storage, 0U, copied, sizeof(copied));
	assert(error == 0);
	error = memcmp(pixels, copied, sizeof(pixels));
	assert(error == 0);

	/* Rejects a foreign context and any range beyond the owned allocation. */
	error = venus_resource_read(&controller, second, storage, 0U, copied, 1U);
	assert(error == EINVAL);
	error = venus_resource_read(&controller, first, storage, 64U, copied, 1U);
	assert(error == EINVAL);

	/* Rejects excessive row padding even when the resource fully contains it. */
	allocation.bytes = 16388U;
	error = venus_resource_create(&controller, first, &allocation, &oversized_stride);
	assert(error == 0);
	memset(&present, 0, sizeof(present));
	present.width = 1U;
	present.height = 1U;
	present.stride = 16388U;
	present.format = GPU_PIXEL_BGRA8888;
	host_creates = fixture_commands[0x101U];
	error = venus_present(&controller, first, oversized_stride, &present);
	assert(error == EINVAL);
	assert(fixture_commands[0x101U] == host_creates);
	venus_resource_destroy(&controller, first, oversized_stride);

	/* Restores the compact storage shape for the remaining display checks. */
	allocation.bytes = sizeof(pixels);

	/* Presents the first session's image and verifies actual scanout selection. */
	memset(&present, 0, sizeof(present));
	present.width = 3U;
	present.height = 4U;
	present.stride = 16U;
	present.format = GPU_PIXEL_BGRA8888;
	error = venus_present(&controller, first, storage, &present);
	assert(error == 0);
	assert(controller.display_owner == first);
	assert(fixture_scanout != 0U);

	/* Foreign resources remain invalid even when a display is already reserved. */
	error = venus_present(&controller, second, storage, &present);
	assert(error == EINVAL);

	/* A second session's own valid resource still cannot replace the active display. */
	error = venus_resource_create(&controller, second, &allocation, &second_storage);
	assert(error == 0);
	error = venus_present(&controller, second, second_storage, &present);
	assert(error == EBUSY);
	assert(controller.scanout == storage);
	venus_resource_destroy(&controller, second, second_storage);

	/* Reconfiguration replaces the host image while preserving the owned guest storage. */
	present.width = 4U;
	present.height = 2U;
	present.offset = 16U;
	present.format = GPU_PIXEL_RGBA8888;
	error = venus_present(&controller, first, storage, &present);
	assert(error == 0);
	assert(((struct venus_resource *)storage)->format == 67U);
	assert(((struct venus_resource *)storage)->height == 2U);

	/* Destroying scanned storage first disables its hardware display reference. */
	venus_resource_destroy(&controller, first, storage);
	assert(controller.resources == NULL);
	assert(controller.scanout == NULL);
	assert(fixture_scanout == 0U);
	assert(fixture_dma == 0U);

	/* Closing the display owner releases arbitration before either wrapper retires. */
	venus_close(&controller, first);
	assert(controller.display_owner == NULL);
	venus_close(&controller, second);

	/* Succeeded: context and storage ownership retired in the correct order. */
	return;
}

/* Checks mapped blob bounds, hole reuse and simultaneous reservation exhaustion. */
static void
fixture_blobs(void)
{
	struct venus_controller controller;
	struct gpu_blob_create allocation;
	struct venus_resource *resource;
	uint8_t configuration[16];
	uint8_t command[4];
	uint8_t copied[4];
	void *session;
	void *first;
	void *second;
	void *third;
	uint32_t identifier;
	int error;

	/* Opens one context with an aperture which can hold exactly two blobs. */
	fixture_controller(&controller, configuration);
	error = venus_open(&controller, &session);
	assert(error == 0);
	memset(&allocation, 0, sizeof(allocation));
	allocation.bytes = 4096U;
	allocation.flags = GPU_BLOB_MAPPABLE;

	/* Reserves two independent host allocations and aperture extents. */
	error = venus_blob_create(&controller, session, &allocation, &first, &identifier);
	assert(error == 0);
	assert(identifier != 0U);
	assert(((struct venus_resource *)first)->mapping.address == fixture_aperture);
	error = venus_blob_create(&controller, session, &allocation, &second, &identifier);
	assert(error == 0);
	assert(fixture_mappings == 0U);

	/* Host-visible copies include the last valid bytes and preserve their contents. */
	memset(command, 0x7e, sizeof(command));
	error = venus_resource_write(&controller, session, second, 4092U, command, sizeof(command));
	assert(error == 0);
	error = venus_resource_read(&controller, session, second, 4092U, copied, sizeof(copied));
	assert(error == 0);
	error = memcmp(command, copied, sizeof(command));
	assert(error == 0);

	/* Exhaustion cannot expose a partial output resource or mapping. */
	error = venus_blob_create(&controller, session, &allocation, &third, &identifier);
	assert(error == ENOSPC);
	assert(third == NULL);
	assert(identifier == 0U);
	assert(fixture_mappings == 0U);

	/* Retiring one host blob releases its low aperture extent for reuse. */
	venus_resource_destroy(&controller, session, first);
	allocation.blob_id = 8U;
	error = venus_blob_create(&controller, session, &allocation, &third, &identifier);
	assert(error == 0);
	resource = third;
	assert(resource->aperture_offset == 0U);
	assert(resource->mapping.address == fixture_aperture);

	/* Opaque stream submission carries the session context and exact byte length. */
	memset(command, 0, sizeof(command));
	error = venus_command(&controller, session, command, sizeof(command));
	assert(error == 0);
	assert(fixture_commands[0x207U] == 1U);

	/* Releases all host mappings before destroying the renderer context. */
	venus_resource_destroy(&controller, session, second);
	venus_resource_destroy(&controller, session, third);
	venus_close(&controller, session);
	assert(controller.resources == NULL);
	assert(fixture_mappings == 0U);

	/* Succeeded: aperture reservations follow actual resource lifetime. */
	return;
}

/* Checks that an uncertain host command never frees hardware-visible storage. */
static void
fixture_timeout_retention(void)
{
	struct venus_controller controller;
	struct gpu_resource_create allocation;
	struct gpu_present present;
	uint8_t configuration[16];
	void *session;
	void *storage;
	unsigned context_destroys;
	int error;

	/* Creates owned coherent storage before the simulated timeout. */
	fixture_controller(&controller, configuration);
	error = venus_open(&controller, &session);
	assert(error == 0);
	memset(&allocation, 0, sizeof(allocation));
	allocation.bytes = 64U;
	error = venus_resource_create(&controller, session, &allocation, &storage);
	assert(error == 0);

	/* Times out backing attachment after the host image has already been created. */
	memset(&present, 0, sizeof(present));
	present.width = 4U;
	present.height = 4U;
	present.stride = 16U;
	present.format = GPU_PIXEL_RGBA8888;
	fixture_timeout = 0x106U;
	error = venus_present(&controller, session, storage, &present);
	assert(error == ETIMEDOUT);

	/* Core handle destruction must preserve coherent bytes while DMA is uncertain. */
	venus_resource_destroy(&controller, session, storage);
	assert(controller.resources != NULL);
	assert(fixture_dma == 1U);

	/* Closing the wrapper cannot destroy context state still retained for reset. */
	context_destroys = fixture_commands[0x201U];
	venus_close(&controller, session);
	assert(fixture_commands[0x201U] == context_destroys);
	assert(fixture_dma == 1U);

	/* Models the acknowledged reset barrier required by PCI detach. */
	fixture_drain(&controller);
	assert(fixture_dma == 0U);
	assert(controller.resources == NULL);

	/* Succeeded: uncertain host references retained memory until reset cleanup. */
	return;
}

/* Models final local retirement after the transport has acknowledged reset. */
static void
fixture_drain(
	struct venus_controller *controller)
{
	int error;

	/* Reset has ended all device access before this fixture cleanup begins. */
	controller->transport.enabled = 0U;
	while (controller->resources != NULL) {
		error = venus_resource_retire(controller, controller->resources);
		assert(error == 0);
	}

	/* Succeeded: no quarantined allocation remains owned by this controller. */
	return;
}
