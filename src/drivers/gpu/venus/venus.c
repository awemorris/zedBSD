/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Venus contexts, host-visible blobs and copied scanout over modern PCI.
 */

#include "internal.h"

#include <drivers/gpu.h>
#include <drivers/venus.h>
#include <kern/device-io.h>
#include <kern/kmem.h>
#include <kern/klog.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

#define VENUS_CAPABILITIES	(63U | GPU_CAP_DISPLAY | GPU_CAP_MAPPING)

static int venus_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int venus_start(struct venus_controller *controller, struct drv_pci_device *device);
static int venus_detach(struct drv_pci_device *device, unsigned flags);
static int venus_publish(struct drv_pci_device *device, void *argument);
static int venus_unpublish(struct drv_pci_device *device, void *argument);
static int venus_open(void *device, void **result);
static void venus_close(void *device, void *private_session);
static int venus_get_info(void *device, void *private_session, struct gpu_info *info);
static int venus_resource_create(void *device, void *private_session, const struct gpu_resource_create *request, void **result);
static void venus_resource_destroy(void *device, void *private_session, void *object);
static int venus_get_capset(void *device, void *private_session, struct gpu_capset *request);
static int venus_blob_create(void *device, void *private_session, const struct gpu_blob_create *request, void **result, uint32_t *resource_id);
static int venus_resource_read(void *device, void *private_session, void *object, uint64_t offset, void *buffer, uint32_t bytes);
static int venus_resource_write(void *device, void *private_session, void *object, uint64_t offset, const void *buffer, uint32_t bytes);
static int venus_command(void *device, void *private_session, const void *buffer, uint32_t bytes);
static int venus_present(void *device, void *private_session, void *object, const struct gpu_present *request);
static int venus_resource_map(void *device, void *private_session, void *object, struct drv_gpu_mapping *mapping);
static int venus_control(struct venus_controller *controller, const void *command, uint32_t bytes);
static int venus_resource_request(struct venus_controller *controller, uint32_t command, uint32_t context, uint32_t identifier);
static int venus_resource_allocate(struct venus_controller *controller, struct venus_session *session, uint64_t bytes, uint32_t kind, struct venus_resource **result);
static int venus_resource_release(struct venus_controller *controller, struct venus_resource *resource);
static int venus_resource_retire(struct venus_controller *controller, struct venus_resource *resource);
static int venus_resource_validate(struct venus_controller *controller, struct venus_session *session, struct venus_resource *resource, uint64_t offset, uint32_t bytes);
static int venus_aperture_reserve(struct venus_controller *controller, struct venus_resource *resource);
static int venus_blob_initialize(struct venus_controller *controller, struct venus_resource *resource, const struct gpu_blob_create *request);
static int venus_scanout_disable(struct venus_controller *controller);
static int venus_storage_initialize(struct venus_controller *controller, struct venus_resource *resource, const struct gpu_present *request);
static int venus_present_image(struct venus_controller *controller, struct venus_session *session, struct venus_resource *resource, const struct gpu_present *request);

/*
 * Registers the Venus PCI backend for modern virtio GPU devices.
 */
int
drv_venus_pci_driver_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		{ 0x1af4U, 0x1050U, 0xffffU, 0xffffU, 0U, 0U, 0U }
	};
	static struct drv_pci_driver driver = {
		"venus", identifiers, 1U, NULL, venus_attach, venus_detach,
		NULL, NULL, NULL, { 0U, 0U, 0U, 0U }
	};
	int error;

	/* Lets PCI select and own every compatible device's complete lifecycle. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: later PCI probing can publish initialized Venus devices. */
	return 0;
}

/*
 * Allocates coherent storage while the caller holds the controller mutex.
 * The resource list retains every uncertain hardware or allocation lifetime.
 */
int
drv_venus_storage_create_locked(
	struct venus_controller *controller,
	struct venus_session *session,
	uint64_t bytes,
	struct venus_resource **result)
{
	struct venus_resource *resource;
	int error;
	int cleanup;

	/* No failure may transfer a partial resource to its caller. */
	*result = NULL;
	error = venus_resource_allocate(controller, session, bytes, VENUS_RESOURCE_STORAGE, &resource);
	if (error != 0)
		return error;

	/* Creates one bounded coherent backing extent for the host image. */
	error = drv_dma_alloc_coherent(controller->transport.dma, (size_t)bytes, VENUS_PAGE_BYTES, &resource->backing);
	if (error != 0) {
		cleanup = venus_resource_retire(controller, resource);
		if (cleanup != 0)
			controller->transport.failed = 1U;
		return error;
	}

	/* Neither userspace nor the display may observe uninitialized pixels. */
	memset(resource->backing.address, 0, (size_t)bytes);
	*result = resource;

	/* Succeeded: the caller owns a fully initialized resource. */
	return 0;
}

/* Prepares private display storage without recursively acquiring its mutex. */
int
drv_venus_storage_prepare_locked(
	struct venus_controller *controller,
	struct venus_resource *resource,
	const struct gpu_present *request)
{
	int error;

	/* Ordinary and direct display paths share the same host image geometry. */
	error = venus_storage_initialize(controller, resource, request);
	if (error != 0)
		return error;

	/* Succeeded: coherent backing is attached to a compatible host image. */
	return 0;
}

/* Retires a private resource only after its caller has ended scanout ownership. */
int
drv_venus_resource_release_locked(
	struct venus_controller *controller,
	struct venus_resource *resource)
{
	int error;

	/* A failed cleanup remains linked until an acknowledged controller reset. */
	error = venus_resource_release(controller, resource);
	if (error != 0)
		return error;

	/* Succeeded: no hardware reference retains the private resource. */
	return 0;
}

/* Acquires one controller and stages its ordinary GPU registration for PCI. */
static int
venus_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct venus_controller *controller;
	int error;
	int cleanup;

	/* Device matching has already consumed the immutable identity record. */
	(void)id;

	/* Allocates private state before acquiring hardware ownership. */
	controller = kern_calloc(1U, sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;

	/* Context and resource zero are reserved by the device protocol. */
	controller->next_context = 1U;
	controller->next_resource = 1U;

	/* Serializes callbacks from different sessions without masking interrupts. */
	error = mutex_init(&controller->mutex, LOCK_RANK_DEVICE, "venus");
	if (error != 0) {
		kern_free(controller);
		return error;
	}

	/* Gives PCI a retryable owner before the first hardware acquisition. */
	error = drv_pci_device_set_driver_data(device, controller);
	if (error != 0) {
		kern_free(controller);
		return error;
	}

	/* Initializes hardware and stages publication before attach can succeed. */
	error = venus_start(controller, device);
	if (error != 0) {
		/* Identifies the initialization boundary which prevented GPU publication. */
		kern_logf("venus: attach stopped at %s: %d\n", controller->transport.stage, error);

		/* A failed reset keeps an unpublished owner for safe detach retry. */
		cleanup = drv_venus_transport_stop(&controller->transport);
		if (cleanup != 0) {
			kern_logf("venus: attach cleanup retained %d\n", cleanup);
			return 0;
		}

		/* Removes PCI's pointer only after every hardware lease has retired. */
		cleanup = drv_pci_device_set_driver_data(device, NULL);
		if (cleanup != 0)
			return 0;

		/* Releases an attach allocation which hardware can no longer access. */
		kern_free(controller);
		return error;
	}

	/* Succeeded: PCI may publish this complete backend after binding it. */
	return 0;
}

/* Prepares hardware and the backend-owned adapter for PCI publication. */
static int
venus_start(
	struct venus_controller *controller,
	struct drv_pci_device *device)
{
	static const struct drv_pci_service_interface service = {
		venus_publish, venus_unpublish
	};
	int error;

	/* Initializes the Venus-only transport before any GPU node exists. */
	error = drv_venus_transport_start(&controller->transport, device);
	if (error != 0)
		return error;

	/* PCI owns publication after attach and withdrawal before hardware detach. */
	controller->transport.stage = "stage-gpu-publication";
	error = drv_pci_device_set_service(device, &service, controller);
	if (error != 0)
		return error;

	/* Succeeded: the backend and its publication contract are both initialized. */
	return 0;
}

/* Resets an unpublished controller before reclaiming quarantined resources. */
static int
venus_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	struct venus_controller *controller;
	struct venus_resource *resource;
	int error;

	/* PCI already applied detach policy before invoking its backend owner. */
	(void)flags;

	/* An already-cleaned attach has no backend state left to detach. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL)
		return 0;

	/* Publication must retire all sessions before their hardware disappears. */
	if (controller->gpu != NULL)
		return EBUSY;

	/* No console worker may submit another command while controller reset retires DMA. */
	error = drv_venus_display_stop(controller);
	if (error != 0)
		return error;

	/* Reset completion ends all queue, guest-backing and host-blob accesses. */
	error = drv_venus_transport_stop(&controller->transport);
	if (error != 0)
		return error;

	/* Frees even quarantined resources after the acknowledged reset barrier. */
	while (controller->resources != NULL) {
		resource = controller->resources;
		error = venus_resource_retire(controller, resource);
		if (error != 0)
			return error;
	}

	/* Display metadata contains no remaining hardware references after reset. */
	drv_venus_display_finish(controller);

	/* Removes PCI's final reference before releasing the backend wrapper. */
	error = drv_pci_device_set_driver_data(device, NULL);
	if (error != 0)
		return error;

	/* No session, service or hardware path retains the controller. */
	kern_free(controller);

	/* Succeeded: PCI may clear the driver binding. */
	return 0;
}

/* Publishes a fully initialized backend through ordinary dynamic GPU APIs. */
static int
venus_publish(
	struct drv_pci_device *device,
	void *argument)
{
	static const struct drv_gpu_ops operations = {
		DRV_GPU_INTERFACE_VERSION, sizeof(struct drv_gpu_ops),
		VENUS_CAPABILITIES, 0U, venus_open, venus_close, venus_get_info,
		venus_resource_create, venus_resource_destroy, venus_get_capset,
		venus_blob_create, venus_resource_read, venus_resource_write,
		venus_command, venus_present, &drv_venus_display_operations,
		venus_resource_map
	};
	struct venus_controller *controller;
	int error;

	/* PCI passes the same controller staged during attach. */
	(void)device;
	controller = argument;

	/* Transfers only publication ownership; PCI keeps hardware ownership. */
	error = drv_gpu_register(&operations, controller, &controller->gpu);
	if (error != 0)
		return error;

	/* Identifies the actual backend without claiming a rendered frame. */
	kern_logf("venus: registered modern PCI Vulkan capset 4\n");

	/* Succeeded: the dynamic GPU node now accepts independent contexts. */
	return 0;
}

/* Preserves the backend while retained GPU sessions prevent withdrawal. */
static int
venus_unpublish(
	struct drv_pci_device *device,
	void *argument)
{
	struct venus_controller *controller;
	int error;

	/* Publication identity is carried by the staged controller argument. */
	(void)device;
	controller = argument;

	/* A failed publication has no registration handle to consume. */
	if (controller->gpu == NULL)
		return 0;

	/* EBUSY deliberately preserves the handle and every hardware allocation. */
	error = drv_gpu_unregister(controller->gpu);
	if (error != 0)
		return error;

	/* Successful unregister consumes the core handle permanently. */
	controller->gpu = NULL;

	/* Succeeded: PCI may now reset and detach the private hardware state. */
	return 0;
}

/* Creates a distinct Venus renderer context for one GPU session. */
static int
venus_open(
	void *device,
	void **result)
{
	struct venus_controller *controller;
	struct venus_session *session;
	uint8_t command[96];
	int error;

	/* Failure never transfers a partial session to the GPU core. */
	controller = device;
	*result = NULL;

	/* Allocates the wrapper before entering serialized device operations. */
	session = kern_calloc(1U, sizeof(*session));
	if (session == NULL)
		return ENOMEM;

	/* Assigns a never-reused context identity and submits its initialization. */
	mutex_lock(&controller->mutex);

	/* Exhaustion cannot recycle identities while earlier commands may exist. */
	if (controller->next_context == 0U) {
		mutex_unlock(&controller->mutex);
		kern_free(session);
		return EOVERFLOW;
	}

	/* Reserving the identity before submission also covers uncertain errors. */
	session->context = controller->next_context;
	controller->next_context++;

	/* Selects the Venus capset explicitly in the context initialization field. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0200U, session->context);
	drv_venus_store32(command + 24U, 8U);
	drv_venus_store32(command + 28U, 4U);
	memcpy(command + 32U, "zedvenus", 8U);
	error = venus_control(controller, command, sizeof(command));
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		kern_free(session);
		return error;
	}

	mutex_unlock(&controller->mutex);

	/* Only a successfully created renderer context is exposed to its open. */
	*result = session;

	/* Succeeded: this open owns its independent Venus context. */
	return 0;
}

/* Releases display ownership and the context after the core destroys resources. */
static void
venus_close(
	void *device,
	void *private_session)
{
	struct venus_controller *controller;
	struct venus_session *session;
	uint8_t command[24];
	int error;

	/* Numeric context IDs, not this wrapper, are visible to the device. */
	controller = device;
	session = private_session;

	/* Serializes display withdrawal and context teardown with other sessions. */
	mutex_lock(&controller->mutex);

	/* Retires direct-display leases before their owning context disappears. */
	drv_venus_display_close_locked(controller, session);

	/* Only the presenting session may release its display reservation. */
	if (controller->display_owner == session) {
		error = venus_scanout_disable(controller);
		if (error != 0)
			controller->transport.failed = 1U;

		/* A dead session cannot retain a pointer in display arbitration. */
		controller->display_owner = NULL;
	}

	/* An uncertain command stream is retained intact for controller reset. */
	if (controller->transport.failed == 0U) {
		drv_venus_header(command, 0x0201U, session->context);
		error = venus_control(controller, command, sizeof(command));
		if (error != 0)
			controller->transport.failed = 1U;
	}

	mutex_unlock(&controller->mutex);

	/* The controller owns any quarantined numeric context and resources. */
	kern_free(session);

	/* Succeeded: no callback can use this session wrapper again. */
	return;
}

/* Reports only the implemented copied-I/O Venus backend capabilities. */
static int
venus_get_info(
	void *device,
	void *private_session,
	struct gpu_info *info)
{
	/* Capabilities and resource limits are immutable for each registration. */
	(void)device;
	(void)private_session;
	info->capabilities = VENUS_CAPABILITIES;
	info->max_resources = UINT32_MAX;
	info->max_resource_bytes = VENUS_MAX_RESOURCE_BYTES;
	memcpy(info->driver_name, "venus", sizeof("venus"));

	/* Succeeded: the caller can select supported operations and limits. */
	return 0;
}

/* Allocates coherent storage without assigning display geometry prematurely. */
static int
venus_resource_create(
	void *device,
	void *private_session,
	const struct gpu_resource_create *request,
	void **result)
{
	struct venus_controller *controller;
	struct venus_session *session;
	struct venus_resource *resource;
	int error;

	/* Keeps a failed creation invisible to the core's resource table. */
	controller = device;
	session = private_session;
	*result = NULL;

	/* Serializes allocation ownership with detach and the control queue. */
	mutex_lock(&controller->mutex);

	/* Uses the same durable allocation owner as private display buffers. */
	error = drv_venus_storage_create_locked(controller, session, request->bytes, &resource);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Transfers this zeroed allocation only after complete acquisition. */
	*result = resource;

	mutex_unlock(&controller->mutex);

	/* Succeeded: the session owns zeroed storage suitable for copied scanout. */
	return 0;
}

/* Retires a core-owned resource or retains it safely after uncertain cleanup. */
static void
venus_resource_destroy(
	void *device,
	void *private_session,
	void *object)
{
	struct venus_controller *controller;
	struct venus_session *session;
	struct venus_resource *resource;
	int error;

	/* The core resolved both the opaque resource and its owning session. */
	controller = device;
	session = private_session;
	resource = object;

	/* Serializes final hardware references with copies and presentation. */
	mutex_lock(&controller->mutex);

	/* Refuses an internal ownership mismatch without touching foreign DMA. */
	if (resource->controller != controller || resource->context != session->context) {
		mutex_unlock(&controller->mutex);
		return;
	}

	/* Any failure leaves the allocation on the controller's quarantine list. */
	error = venus_resource_release(controller, resource);
	if (error != 0) {
		controller->transport.failed = 1U;
		kern_logf("venus: resource %u retained for reset: %d\n", resource->identifier, error);
	}

	mutex_unlock(&controller->mutex);

	/* Succeeded: the core may forget its handle; retained DMA remains owned. */
	return;
}

/* Retrieves the exact advertised Venus capset into the bounded kernel request. */
static int
venus_get_capset(
	void *device,
	void *private_session,
	struct gpu_capset *request)
{
	struct venus_controller *controller;
	uint8_t command[32];
	uint8_t response[VENUS_HEADER_BYTES + GPU_CAPSET_MAX];
	uint32_t bytes;
	uint32_t type;
	int error;

	/* Capsets describe the device independently of a particular context. */
	(void)private_session;
	controller = device;

	/* This backend exposes only the protocol selected during attach. */
	if (request->capset_id != 4U || request->capset_version != 0U)
		return EOPNOTSUPP;

	/* The caller must provide room for the complete advertised payload. */
	if (request->capacity < controller->transport.capset_size)
		return EMSGSIZE;

	/* Encodes the immutable capset identity and version. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0109U, 0U);
	drv_venus_store32(command + 24U, request->capset_id);
	drv_venus_store32(command + 28U, request->capset_version);

	/* Uses the sole control queue without exposing transient buffers to DMA. */
	mutex_lock(&controller->mutex);

	error = drv_venus_transport_command(
		&controller->transport,
		command,
		sizeof(command),
		response,
		sizeof(response),
		&bytes);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	mutex_unlock(&controller->mutex);

	/* Accepts only a complete capability reply with the advertised size. */
	type = drv_venus_load32(response);
	if (type != 0x1103U || bytes != VENUS_HEADER_BYTES + controller->transport.capset_size)
		return EIO;

	/* Copies only bytes covered by the caller's capacity and device reply. */
	request->bytes = controller->transport.capset_size;
	memcpy(request->data, response + VENUS_HEADER_BYTES, request->bytes);

	/* Succeeded: userspace can check renderer protocol compatibility. */
	return 0;
}

/* Creates a host-visible Venus blob and returns its protocol resource identity. */
static int
venus_blob_create(
	void *device,
	void *private_session,
	const struct gpu_blob_create *request,
	void **result,
	uint32_t *resource_id)
{
	struct venus_controller *controller;
	struct venus_session *session;
	struct venus_resource *resource;
	int error;
	int cleanup;

	/* Output identities remain absent unless every mapping step succeeds. */
	controller = device;
	session = private_session;
	*result = NULL;
	*resource_id = 0U;

	/* Copied resource access requires a host-visible mapping. */
	if (request->flags != GPU_BLOB_MAPPABLE)
		return EOPNOTSUPP;

	/* Context resources and aperture extents share controller serialization. */
	mutex_lock(&controller->mutex);

	/* Reserves the resource identity before any host allocation can succeed. */
	error = venus_resource_allocate(
		controller,
		session,
		request->bytes,
		VENUS_RESOURCE_BLOB,
		&resource);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Creates, attaches and maps the host allocation as one owned operation. */
	error = venus_blob_initialize(controller, resource, request);
	if (error != 0) {
		cleanup = venus_resource_release(controller, resource);
		if (cleanup != 0)
			controller->transport.failed = 1U;
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* The opaque core handle remains distinct from Venus's numeric resource. */
	*result = resource;
	*resource_id = resource->identifier;

	mutex_unlock(&controller->mutex);

	/* Succeeded: userspace may reference and copy this session's mapped blob. */
	return 0;
}

/* Copies resource bytes after validating the backend's independent ownership. */
static int
venus_resource_read(
	void *device,
	void *private_session,
	void *object,
	uint64_t offset,
	void *buffer,
	uint32_t bytes)
{
	struct venus_controller *controller;
	struct venus_resource *resource;
	volatile uint8_t *source;
	uint8_t *destination;
	uint32_t index;
	int error;

	/* Kernel buffers are temporary copies owned by the GPU core. */
	controller = device;
	resource = object;
	destination = buffer;

	/* Keeps each allocation mapped for the entire bounded copy. */
	mutex_lock(&controller->mutex);

	/* Checks backend ownership even though the core already checked handles. */
	error = venus_resource_validate(controller, private_session, resource, offset, bytes);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Storage and host-visible blobs retain different local address owners. */
	if (resource->kind == VENUS_RESOURCE_STORAGE) {
		source = resource->backing.address;
	} else {
		source = resource->mapping.address;
	}

	/* Acquires host writes before taking a fresh volatile snapshot. */
	kern_io_read_barrier();
	source += (size_t)offset;
	for (index = 0; index < bytes; index++)
		destination[index] = source[index];

	mutex_unlock(&controller->mutex);

	/* Succeeded: no device-owned pointer escapes to the GPU core or user. */
	return 0;
}

/* Copies kernel-owned input into retained storage or a host-visible blob. */
static int
venus_resource_write(
	void *device,
	void *private_session,
	void *object,
	uint64_t offset,
	const void *buffer,
	uint32_t bytes)
{
	struct venus_controller *controller;
	struct venus_resource *resource;
	volatile uint8_t *destination;
	const uint8_t *source;
	uint32_t index;
	int error;

	/* The GPU core has copied the user's complete bounded input already. */
	controller = device;
	resource = object;
	source = buffer;

	/* Keeps the destination alive through publication of the copied bytes. */
	mutex_lock(&controller->mutex);

	/* Enforces the driver's own resource bounds and context identity. */
	error = venus_resource_validate(controller, private_session, resource, offset, bytes);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Selects the local mapping owned by this resource kind. */
	if (resource->kind == VENUS_RESOURCE_STORAGE) {
		destination = resource->backing.address;
	} else {
		destination = resource->mapping.address;
	}

	/* Stores each requested byte before a later command can consume it. */
	destination += (size_t)offset;
	for (index = 0; index < bytes; index++)
		destination[index] = source[index];

	/* Orders writes against later submissions and renderer reply processing. */
	kern_io_write_barrier();

	mutex_unlock(&controller->mutex);

	/* Succeeded: no temporary source buffer remains referenced by hardware. */
	return 0;
}

/* Submits an opaque Venus stream without treating queue receipt as execution. */
static int
venus_command(
	void *device,
	void *private_session,
	const void *buffer,
	uint32_t bytes)
{
	struct venus_controller *controller;
	struct venus_session *session;
	uint8_t *command;
	int error;

	/* The stream format and Vulkan completion protocol remain in userspace. */
	controller = device;
	session = private_session;

	/* Defends transport capacity independently of the checked UAPI caller. */
	if (bytes == 0U ||
	    bytes > GPU_COMMAND_MAX ||
	    (bytes & 3U) != 0U)
		return EINVAL;

	/* Builds a transient control payload which the transport copies for DMA. */
	command = kern_malloc((size_t)bytes + 32U);
	if (command == NULL)
		return ENOMEM;

	/* Selects this session's renderer context and preserves opaque command bytes. */
	memset(command, 0, 32U);
	drv_venus_header(command, 0x0207U, session->context);
	drv_venus_store32(command + 24U, bytes);
	memcpy(command + 32U, buffer, bytes);

	/* Serializes the single request slot while allowing timer interrupts. */
	mutex_lock(&controller->mutex);

	error = venus_control(controller, command, bytes + 32U);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		kern_free(command);
		return error;
	}

	mutex_unlock(&controller->mutex);

	/* DMA used persistent transport storage, so this copy is no longer needed. */
	kern_free(command);

	/* Succeeded: userspace must still check its reply marker and Vulkan fence. */
	return 0;
}

/* Presents copied storage while enforcing one display-owning session. */
static int
venus_present(
	void *device,
	void *private_session,
	void *object,
	const struct gpu_present *request)
{
	struct venus_controller *controller;
	int error;

	/* Resolves the controller whose display arbitration covers all opens. */
	controller = device;

	/* Serializes geometry, backing and scanout replacement as one operation. */
	mutex_lock(&controller->mutex);

	error = venus_present_image(controller, private_session, object, request);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	mutex_unlock(&controller->mutex);

	/* Succeeded: the virtual display has accepted the copied image update. */
	return 0;
}

/* Describes one retained host-visible blob for a real shared user mapping. */
static int
venus_resource_map(
	void *device,
	void *private_session,
	void *object,
	struct drv_gpu_mapping *mapping)
{
	struct venus_controller *controller;
	struct venus_session *session;
	struct venus_resource *resource;
	uint64_t physical;
	int error;

	/* GPU core pins this session and resource until every user mapping retires. */
	controller = device;
	session = private_session;
	resource = object;
	mutex_lock(&controller->mutex);

	/* Mapping cannot bypass the same ownership checks as copied resource I/O. */
	error = venus_resource_validate(controller, session, resource, 0U, 0U);
	if (error != 0) {
		mutex_unlock(&controller->mutex);
		return error;
	}

	/* Ordinary guest storage has no exported host-visible device mapping. */
	if (resource->kind != VENUS_RESOURCE_BLOB || resource->mapped == 0U) {
		mutex_unlock(&controller->mutex);
		return ENOTSUP;
	}

	/* PCI supplies the actual CPU physical mapping after any BAR relocation. */
	physical = controller->transport.host_mapping.physical_address;
	if (controller->transport.host_visible.offset > UINT64_MAX - physical) {
		mutex_unlock(&controller->mutex);
		return EOVERFLOW;
	}
	physical += controller->transport.host_visible.offset;
	if (resource->aperture_offset > UINT64_MAX - physical) {
		mutex_unlock(&controller->mutex);
		return EOVERFLOW;
	}
	physical += resource->aperture_offset;

	/* The retained page-rounded extent preserves the kernel alias cache policy. */
	memset(mapping, 0, sizeof(*mapping));
	mapping->physical = physical;
	mapping->address = resource->mapping.address;
	mapping->bytes = resource->aperture_bytes;
	mapping->attributes = DRV_GPU_MAPPING_DEVICE;
	mutex_unlock(&controller->mutex);

	/* Succeeded: the core can map and pin this exact device extent. */
	return 0;
}

/* Requires a complete no-data response for a state-changing control command. */
static int
venus_control(
	struct venus_controller *controller,
	const void *command,
	uint32_t bytes)
{
	uint8_t response[24];
	uint32_t response_bytes;
	uint32_t type;
	int error;

	/* Copies the complete response before the persistent slot may be reused. */
	error = drv_venus_transport_command(
		&controller->transport,
		command,
		bytes,
		response,
		sizeof(response),
		&response_bytes);
	if (error != 0)
		return error;

	/* A wrong success type cannot prove this state transition completed. */
	type = drv_venus_load32(response);
	if (response_bytes != sizeof(response) || type != 0x1100U) {
		controller->transport.failed = 1U;
		return EIO;
	}

	/* Succeeded: the device acknowledged this control operation. */
	return 0;
}

/* Encodes a context resource attachment, detachment or destruction request. */
static int
venus_resource_request(
	struct venus_controller *controller,
	uint32_t command_type,
	uint32_t context,
	uint32_t identifier)
{
	uint8_t command[32];
	int error;

	/* Resource commands share one bounded identifier-and-padding payload. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, command_type, context);
	drv_venus_store32(command + 24U, identifier);

	/* Requires acknowledgement before changing local ownership flags. */
	error = venus_control(controller, command, sizeof(command));
	if (error != 0)
		return error;

	/* Succeeded: the requested resource transition completed on the host. */
	return 0;
}

/* Reserves an identity and links resource ownership before host operations. */
static int
venus_resource_allocate(
	struct venus_controller *controller,
	struct venus_session *session,
	uint64_t bytes,
	uint32_t kind,
	struct venus_resource **result)
{
	struct venus_resource *resource;

	/* A failed transport cannot safely create or recycle hardware resources. */
	if (controller->transport.failed != 0U)
		return ENODEV;

	/* Bounded size makes every kernel cast and coherent allocation representable. */
	if (bytes == 0U || bytes > VENUS_MAX_RESOURCE_BYTES)
		return EINVAL;

	/* Never reuses a protocol identity while host references might remain. */
	if (controller->next_resource == 0U)
		return EOVERFLOW;

	/* Allocates the independent lifetime record before publishing any pointers. */
	resource = kern_calloc(1U, sizeof(*resource));
	if (resource == NULL)
		return ENOMEM;

	/* Initializes the immutable ownership and bounded allocation description. */
	resource->controller = controller;
	resource->context = session->context;
	resource->bytes = bytes;
	resource->kind = kind;
	resource->identifier = controller->next_resource;

	/* Reserving an ID permanently prevents stale protocol references from aliasing. */
	controller->next_resource++;

	/* The controller now owns cleanup even when later host commands time out. */
	resource->next = controller->resources;
	controller->resources = resource;
	*result = resource;

	/* Succeeded: subsequent acquisition failures have a durable cleanup owner. */
	return 0;
}

/* Ends host references before making any resource storage reusable. */
static int
venus_resource_release(
	struct venus_controller *controller,
	struct venus_resource *resource)
{
	int error;

	/* Unknown command completion requires controller reset instead of freeing. */
	if (controller->transport.failed != 0U)
		return ENODEV;

	/* Scanout cannot continue displaying storage which is about to retire. */
	if (controller->scanout == resource) {
		error = venus_scanout_disable(controller);
		if (error != 0)
			return error;
	}

	/* Host-visible aperture removal must complete before its extent is reused. */
	if (resource->mapped != 0U) {
		error = venus_resource_request(controller, 0x0209U, resource->context, resource->identifier);
		if (error != 0)
			return error;

		/* The host no longer exposes this blob in the PCI aperture. */
		resource->mapped = 0U;
	}

	/* Removes the context's explicit reference before global resource unref. */
	if (resource->attached != 0U) {
		error = venus_resource_request(controller, 0x0203U, resource->context, resource->identifier);
		if (error != 0)
			return error;

		/* Context teardown no longer needs to name this resource. */
		resource->attached = 0U;
	}

	/* Unref also detaches guest backing before its coherent allocation retires. */
	if (resource->created != 0U) {
		error = venus_resource_request(controller, 0x0102U, resource->context, resource->identifier);
		if (error != 0)
			return error;

		/* The host has relinquished the resource and any backing references. */
		resource->created = 0U;
	}

	/* Releases the local ownership record only after every host operation. */
	error = venus_resource_retire(controller, resource);
	if (error != 0)
		return error;

	/* Succeeded: neither host nor controller retains this resource. */
	return 0;
}

/* Releases local mappings and memory after host cleanup or acknowledged reset. */
static int
venus_resource_retire(
	struct venus_controller *controller,
	struct venus_resource *resource)
{
	struct venus_resource **link;

	/* Returns guest physical memory only after device ownership has ended. */
	if (resource->backing.address != NULL) {
		drv_dma_free_coherent(controller->transport.dma, &resource->backing);
		if (resource->backing.address != NULL)
			return EBUSY;
	}

	/* Blob views borrow the transport mapping and carry no independent unmap lease. */
	memset(&resource->mapping, 0, sizeof(resource->mapping));

	/* Finds the durable ownership record without trusting list position. */
	link = &controller->resources;
	while (*link != NULL && *link != resource)
		link = &(*link)->next;

	/* A missing record indicates an internal ownership error, never a free. */
	if (*link == NULL)
		return EINVAL;

	/* Removing this node releases its aperture reservation for later blobs. */
	*link = resource->next;
	kern_free(resource);

	/* Succeeded: the local resource and every allocation it owned retired. */
	return 0;
}

/* Checks the bounds and identity required by every copied resource access. */
static int
venus_resource_validate(
	struct venus_controller *controller,
	struct venus_session *session,
	struct venus_resource *resource,
	uint64_t offset,
	uint32_t bytes)
{
	/* A timed-out queue makes host resource state unsafe for continued access. */
	if (controller->transport.failed != 0U)
		return ENODEV;

	/* The core's opaque pointer must still name this context's backend object. */
	if (resource == NULL || resource->controller != controller)
		return EINVAL;

	/* Prevents one renderer context from copying another context's storage. */
	if (resource->context != session->context)
		return EINVAL;

	/* Subtraction follows the ordered-offset check to avoid unsigned wrap. */
	if (offset > resource->bytes || bytes > resource->bytes - offset)
		return EINVAL;

	/* Succeeded: every copied byte belongs to the supplied session resource. */
	return 0;
}

/* Finds a reusable page-aligned hole in the advertised host-visible aperture. */
static int
venus_aperture_reserve(
	struct venus_controller *controller,
	struct venus_resource *resource)
{
	struct venus_resource *other;
	uint64_t offset;
	uint64_t extent;
	uint64_t end;
	unsigned collision;

	/* Rounds blob extents so neighboring host allocations never share a page. */
	extent = (resource->bytes + VENUS_PAGE_BYTES - 1U) & ~(uint64_t)(VENUS_PAGE_BYTES - 1U);
	offset = 0U;

	/* Advances beyond overlapping live or quarantined blob reservations. */
	do {
		/* Rejects exhaustion before computing the proposed extent end. */
		if (offset > controller->transport.host_visible.length ||
		    extent > controller->transport.host_visible.length - offset) {
			return ENOSPC;
		}

		/* Tests this complete candidate against every retained reservation. */
		end = offset + extent;
		collision = 0U;
		other = controller->resources;
		while (other != NULL) {
			/* Storage allocations and this unassigned resource reserve no aperture. */
			if (other->aperture_bytes != 0U &&
			    offset < other->aperture_offset + other->aperture_bytes &&
			    other->aperture_offset < end) {
				offset = other->aperture_offset + other->aperture_bytes;
				collision = 1U;
				break;
			}

			/* Retained nodes include failed creates awaiting safe reset cleanup. */
			other = other->next;
		}
	} while (collision != 0U);

	/* The linked resource holds this extent until its final retirement. */
	resource->aperture_offset = offset;
	resource->aperture_bytes = extent;

	/* Succeeded: a nonoverlapping aperture extent is reserved for this blob. */
	return 0;
}

/* Creates and maps one host allocation through the owning Venus context. */
static int
venus_blob_initialize(
	struct venus_controller *controller,
	struct venus_resource *resource,
	const struct gpu_blob_create *request)
{
	uint8_t command[56];
	uint8_t response[32];
	uint32_t bytes;
	uint32_t type;
	uint32_t map_info;
	int error;

	/* Reserves aperture space before asking the renderer to allocate memory. */
	error = venus_aperture_reserve(controller, resource);
	if (error != 0)
		return error;

	/* HOST3D blob zero creates reply shared memory; other IDs export Vulkan memory. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x010cU, resource->context);
	drv_venus_store32(command + 24U, resource->identifier);
	drv_venus_store32(command + 28U, 2U);
	drv_venus_store32(command + 32U, 1U);
	drv_venus_store64(command + 40U, request->blob_id);
	drv_venus_store64(command + 48U, request->bytes);
	error = venus_control(controller, command, sizeof(command));
	if (error != 0)
		return error;

	/* Global unref is now required before this identity can retire locally. */
	resource->created = 1U;

	/* Makes the protocol resource visible to this context's reply stream. */
	error = venus_resource_request(controller, 0x0202U, resource->context, resource->identifier);
	if (error != 0)
		return error;

	/* Context detach must precede unref from this point onward. */
	resource->attached = 1U;

	/* Maps the host allocation into its exclusively reserved aperture extent. */
	memset(command, 0, 40U);
	drv_venus_header(command, 0x0208U, resource->context);
	drv_venus_store32(command + 24U, resource->identifier);
	drv_venus_store64(command + 32U, resource->aperture_offset);
	error = drv_venus_transport_command(
		&controller->transport,
		command,
		40U,
		response,
		sizeof(response),
		&bytes);
	if (error != 0)
		return error;

	/* Successful mapping requires cleanup even if its cache metadata is invalid. */
	resource->mapped = 1U;
	type = drv_venus_load32(response);
	if (type != 0x1106U || bytes != sizeof(response))
		return EIO;

	/* Accepts defined cache modes while choosing conservative uncached access. */
	map_info = drv_venus_load32(response + 24U);
	if ((map_info & ~0x0fU) != 0U || (map_info & 0x0fU) > 3U)
		return EOPNOTSUPP;

	/* The transport retains one complete aperture mapping for every blob view. */
	if (controller->transport.host_visible.mapping.address == NULL)
		return ENODEV;

	/* Rechecks the borrowed extent against the actual retained kernel mapping. */
	if (resource->aperture_offset > controller->transport.host_visible.mapping.size ||
	    resource->aperture_bytes > controller->transport.host_visible.mapping.size - resource->aperture_offset) {
		return EINVAL;
	}

	/* Only this checked slice is exposed to the resource's copied-access callbacks. */
	resource->mapping.address = (uint8_t *)controller->transport.host_visible.mapping.address + (size_t)resource->aperture_offset;
	resource->mapping.size = (size_t)resource->aperture_bytes;
	resource->mapping.type = controller->transport.host_visible.mapping.type;

	/* Succeeded: the context resource has a retained kernel-only mapped view. */
	return 0;
}

/* Disables the active scanout before its backing can be released or replaced. */
static int
venus_scanout_disable(
	struct venus_controller *controller)
{
	uint8_t command[48];
	int error;

	/* An unused scanout has no resource reference to withdraw. */
	if (controller->scanout == NULL)
		return 0;

	/* Resource zero requests a disabled scanout with an empty rectangle. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0103U, 0U);
	error = venus_control(controller, command, sizeof(command));
	if (error != 0)
		return error;

	/* The display no longer references the previously scanned resource. */
	controller->scanout = NULL;
	controller->primary_scanout = NULL;
	controller->primary_width = 0U;
	controller->primary_height = 0U;

	/* Succeeded: the former scanout backing may be replaced or retired. */
	return 0;
}

/* Creates a two-dimensional host image using the storage allocation as backing. */
static int
venus_storage_initialize(
	struct venus_controller *controller,
	struct venus_resource *resource,
	const struct gpu_present *request)
{
	uint8_t command[48];
	uint32_t width;
	uint32_t format;
	int error;

	/* Host image width includes row padding so transfer pitch matches the UAPI. */
	width = request->stride / 4U;
	format = 1U;

	/* Virtio uses a different numeric format for little-endian RGBA pixels. */
	if (request->format == GPU_PIXEL_RGBA8888)
		format = 67U;

	/* Existing compatible images retain their host allocation and backing. */
	if (resource->created != 0U &&
	    resource->width == width &&
	    resource->height == request->height &&
	    resource->format == format) {
		return 0;
	}

	/* A changed geometry must withdraw any display reference first. */
	if (controller->scanout == resource) {
		error = venus_scanout_disable(controller);
		if (error != 0)
			return error;
	}

	/* Releases the old host image while retaining its coherent guest allocation. */
	if (resource->created != 0U) {
		error = venus_resource_request(controller, 0x0102U, 0U, resource->identifier);
		if (error != 0)
			return error;

		/* Acknowledged unref permits the same private storage identity to recur. */
		resource->created = 0U;
	}

	/* Creates the host image without exposing unrelated user resource IDs. */
	memset(command, 0, 40U);
	drv_venus_header(command, 0x0101U, 0U);
	drv_venus_store32(command + 24U, resource->identifier);
	drv_venus_store32(command + 28U, format);
	drv_venus_store32(command + 32U, width);
	drv_venus_store32(command + 36U, request->height);
	error = venus_control(controller, command, 40U);
	if (error != 0)
		return error;

	/* The new host image must be unreferenced on any later failure. */
	resource->created = 1U;
	resource->width = width;
	resource->height = request->height;
	resource->format = format;

	/* Attaches one bounded guest DMA extent containing the copied pixel storage. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0106U, 0U);
	drv_venus_store32(command + 24U, resource->identifier);
	drv_venus_store32(command + 28U, 1U);
	drv_venus_store64(command + 32U, resource->backing.device_address);
	drv_venus_store32(command + 40U, (uint32_t)resource->bytes);
	error = venus_control(controller, command, sizeof(command));
	if (error != 0) {
		/* Forces a later presentation to rebuild instead of assuming attached backing. */
		resource->width = 0U;
		return error;
	}

	/* Succeeded: the host image has complete geometry and guest pixel backing. */
	return 0;
}

/* Transfers one complete image and selects it as the sole active scanout. */
static int
venus_present_image(
	struct venus_controller *controller,
	struct venus_session *session,
	struct venus_resource *resource,
	const struct gpu_present *request)
{
	uint8_t command[56];
	uint64_t bytes;
	uint32_t scanouts;
	int error;

	/* Display control requires a storage allocation owned by this context. */
	error = venus_resource_validate(controller, session, resource, request->offset, 0U);
	if (error != 0)
		return error;

	/* Blobs carry renderer memory rather than guest-backed scanout storage. */
	if (resource->kind != VENUS_RESOURCE_STORAGE)
		return EINVAL;

	/* Padded rows must fit the same finite texture extent as visible pixels. */
	if (request->width == 0U ||
	    request->height == 0U ||
	    request->width > 4096U ||
	    request->height > 4096U ||
	    request->stride > 4096U * 4U ||
	    (request->stride & 3U) != 0U ||
	    request->stride < request->width * 4U) {
		return EINVAL;
	}

	/* Only the two explicitly defined packed pixel formats are accepted. */
	if (request->format != GPU_PIXEL_BGRA8888 && request->format != GPU_PIXEL_RGBA8888)
		return EINVAL;

	/* Checked subtraction includes all row padding in the owned byte range. */
	bytes = (uint64_t)request->stride * request->height;
	if (bytes > resource->bytes - request->offset)
		return EINVAL;

	/* Direct-display ownership excludes every legacy presentation path. */
	error = drv_venus_display_legacy_available_locked(controller);
	if (error != 0)
		return error;

	/* A second open cannot replace a display reserved by an active presenter. */
	if (controller->display_owner != NULL && controller->display_owner != session)
		return EBUSY;

	/* A device with no supported scanouts cannot accept display ownership. */
	scanouts = kern_mmio_read32((uint8_t *)controller->transport.configuration.mapping.address + 8U);
	if (scanouts == 0U)
		return ENODEV;

	/* Establishes compatible host image geometry and guest backing first. */
	error = venus_storage_initialize(controller, resource, request);
	if (error != 0)
		return error;

	/* Copies the requested guest-backed rectangle into the host image. */
	memset(command, 0, sizeof(command));
	drv_venus_header(command, 0x0105U, 0U);
	drv_venus_store32(command + 32U, request->width);
	drv_venus_store32(command + 36U, request->height);
	drv_venus_store64(command + 40U, request->offset);
	drv_venus_store32(command + 48U, resource->identifier);
	kern_io_write_barrier();
	error = venus_control(controller, command, sizeof(command));
	if (error != 0)
		return error;

	/* Selects the transferred visible rectangle on scanout zero. */
	memset(command, 0, 48U);
	drv_venus_header(command, 0x0103U, 0U);
	drv_venus_store32(command + 32U, request->width);
	drv_venus_store32(command + 36U, request->height);
	drv_venus_store32(command + 44U, resource->identifier);
	error = venus_control(controller, command, 48U);
	if (error != 0)
		return error;

	/* Accepted scanout retains this resource and reserves the display session. */
	controller->scanout = resource;
	controller->primary_scanout = resource;
	controller->primary_width = request->width;
	controller->primary_height = request->height;
	controller->display_owner = session;

	/* Requests a display update of the newly selected complete rectangle. */
	memset(command, 0, 48U);
	drv_venus_header(command, 0x0104U, 0U);
	drv_venus_store32(command + 32U, request->width);
	drv_venus_store32(command + 36U, request->height);
	drv_venus_store32(command + 40U, resource->identifier);
	error = venus_control(controller, command, 48U);
	if (error != 0)
		return error;

	/* Succeeded: QEMU accepted the flushed image for its display surface. */
	return 0;
}
