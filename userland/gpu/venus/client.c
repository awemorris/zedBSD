/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Independent, bounded Venus wire-format-1 encoding and Vulkan bootstrap.
 * Each caller owns its transport descriptor and single-flight command state.
 * This finite protocol client is not a general Vulkan loader or implementation.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "client.h"

#define VK_VERSION_1_1 0x00401000U

/* Bootstrap and reply-stream operations use stable public wire command IDs. */
enum venus_command {
	VENUS_CREATE_INSTANCE = 0,
	VENUS_ENUMERATE_PHYSICAL_DEVICES = 2,
	VENUS_GET_QUEUE_FAMILIES = 7,
	VENUS_GET_MEMORY_PROPERTIES = 8,
	VENUS_CREATE_DEVICE = 11,
	VENUS_ENUMERATE_INSTANCE_VERSION = 137,
	VENUS_GET_DEVICE_QUEUE2 = 155,
	VENUS_SET_REPLY_STREAM = 178,
	VENUS_SEEK_REPLY_STREAM = 179
};

/* Structure tags describe serialized Vulkan inputs without native padding. */
enum venus_structure {
	STRUCTURE_APPLICATION_INFO = 0,
	STRUCTURE_INSTANCE_CREATE_INFO = 1,
	STRUCTURE_DEVICE_QUEUE_CREATE_INFO = 2,
	STRUCTURE_DEVICE_CREATE_INFO = 3,
	STRUCTURE_DEVICE_QUEUE_INFO2 = 1000145003,
	STRUCTURE_DEVICE_QUEUE_TIMELINE_INFO = 1000384005
};

/* A partial bootstrap retains its identities until the owning session closes. */
enum venus_initialization {
	VENUS_INITIALIZATION_NONE = 0,
	VENUS_INITIALIZATION_STARTED = 1,
	VENUS_INITIALIZATION_READY = 2
};

static uint32_t load_u32(const uint8_t *data);
static int create_instance(struct venus_client *client);
static int select_physical_device(struct venus_client *client);
static int query_memory(struct venus_client *client);
static int create_device(struct venus_client *client);

/*
 * Opens a caller-owned GPU session and reads its device capabilities.
 *
 * The caller supplies fresh storage or a previously closed client. Failure
 * leaves no descriptor to release, including when the capability query fails.
 */
int
venus_client_open(
	struct venus_client *client,
	const char *path)
{
	int status;
	int saved_errno;
	int cleanup_status;

	/* Require storage before establishing any descriptor ownership. */
	if (client == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Make every later acquisition failure safe for optional caller cleanup. */
	memset(client, 0, sizeof(*client));
	client->fd = -1;

	/* Reject a missing path without entering the device namespace. */
	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Open this session with transfer and presentation rights. */
	client->fd = open(path, O_RDWR);
	if (client->fd < 0) {
		return -1;
	}

	/* Read capabilities without activating the optional Venus protocol. */
	client->info.version = GPU_ABI_VERSION;
	client->info.size = sizeof(client->info);
	status = ioctl(client->fd, GPU_GET_INFO, &client->info);
	if (status != 0) {
		/* Preserve the acquisition failure even if final release also fails. */
		saved_errno = errno;
		cleanup_status = venus_client_close(client);
		if (cleanup_status != 0) {
			errno = saved_errno;
			return -1;
		}

		/* Report the capability-query failure after returning the descriptor. */
		errno = saved_errno;
		return -1;
	}

	/* Succeeded: this client owns one independently initialized GPU session. */
	return 0;
}

/*
 * Closes the session and releases any complete or partial Vulkan bootstrap.
 *
 * zedBSD removes the descriptor before final device release, even on failure.
 * Keep diagnostic fields available, but never retry the consumed descriptor.
 */
int
venus_client_close(
	struct venus_client *client)
{
	int descriptor;
	int status;

	/* Require a valid ownership record even for an already closed session. */
	if (client == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Consume only a descriptor this client still owns. */
	if (client->fd >= 0) {
		/* Retire client ownership before the descriptor can be reused. */
		descriptor = client->fd;
		client->fd = -1;
		client->reply_handle = 0;
		client->reply_resource = 0;

		/* Let session teardown reclaim its resources and renderer context. */
		status = close(descriptor);
		if (status != 0) {
			return -1;
		}
	}

	/* Succeeded: neither a fresh failure nor repeated cleanup owns a descriptor. */
	return 0;
}

/*
 * Initializes a finite Vulkan instance, physical device, device and queue.
 *
 * A failed attempt retains all acquired state for close and cannot be retried.
 * Rendering applications own all object identities following the reserved four.
 */
int
venus_client_init_vulkan(
	struct venus_client *client)
{
	struct gpu_capset capset;
	struct gpu_blob_create blob;
	uint32_t multiple_timelines;
	int status;

	/* Reject a missing session before consulting its bootstrap state. */
	if (client == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* Require the GPU session established by the common open operation. */
	if (client->fd < 0) {
		errno = EBADF;
		return -1;
	}

	/* Partial object identities cannot be created again on the same context. */
	if (client->initialization_state != VENUS_INITIALIZATION_NONE) {
		errno = EALREADY;
		return -1;
	}

	/* Retain the attempt even when a later command only partially succeeds. */
	client->initialization_state = VENUS_INITIALIZATION_STARTED;

	/* Query the bounded Venus capability payload with no prefilled outputs. */
	memset(&capset, 0, sizeof(capset));
	capset.version = GPU_ABI_VERSION;
	capset.size = sizeof(capset);
	capset.capset_id = 4;
	capset.capacity = GPU_CAPSET_MAX;
	status = ioctl(client->fd, GPU_GET_CAPSET, &capset);
	if (status != 0) {
		return -1;
	}

	/* Require the fixed prefix before decoding its optional timeline field. */
	if (capset.bytes < 156) {
		errno = ENOTSUP;
		return -1;
	}

	/* Refuse an output length that exceeds the supplied capability array. */
	if (capset.bytes > GPU_CAPSET_MAX) {
		errno = ENOTSUP;
		return -1;
	}

	/* Expose the selected host revision without making application output. */
	client->capset_bytes = capset.bytes;
	client->wire_version = load_u32(capset.data);
	client->xml_version = load_u32(capset.data + 4);

	/* Refuse revisions that require a different serialization contract. */
	if (client->wire_version != 1) {
		errno = ENOTSUP;
		return -1;
	}

	/* Queue2 registration requires the advertised multiple-timeline protocol. */
	multiple_timelines = load_u32(capset.data + 152);
	if (multiple_timelines == 0) {
		errno = ENOTSUP;
		return -1;
	}

	/* Blob ID zero requests fresh shared reply storage for this session. */
	memset(&blob, 0, sizeof(blob));
	blob.version = GPU_ABI_VERSION;
	blob.size = sizeof(blob);
	blob.bytes = VENUS_CLIENT_REPLY_BYTES;
	blob.flags = GPU_BLOB_MAPPABLE;
	status = ioctl(client->fd, GPU_BLOB_CREATE, &blob);
	if (status != 0) {
		return -1;
	}

	/* Keep both the owned handle and renderer identity until session teardown. */
	client->reply_handle = blob.handle;
	client->reply_resource = blob.resource_id;

	/* Establish the context's Vulkan instance. */
	status = create_instance(client);
	if (status != 0) {
		return -1;
	}

	/* Select a physical device and a usable graphics queue. */
	status = select_physical_device(client);
	if (status != 0) {
		return -1;
	}

	/* Discover memory types before applications allocate Vulkan objects. */
	status = query_memory(client);
	if (status != 0) {
		return -1;
	}

	/* Create the logical device and obtain its queue identity. */
	status = create_device(client);
	if (status != 0) {
		return -1;
	}

	/* Publish complete bootstrap output only after all identities are accepted. */
	client->initialization_state = VENUS_INITIALIZATION_READY;

	/* Succeeded: the caller may allocate its application-owned Vulkan objects. */
	return 0;
}

/*
 * Append one protocol word with sticky bounds failure.
 */
void
venus_client_wire_u32(
	struct venus_client *client,
	uint32_t word)
{
	uint32_t position;

	/* Keep the first encoding failure until a new command begins. */
	if (client->wire_error != 0) {
		return;
	}

	/* Keep malformed or oversized streams out of the kernel. */
	if (client->stream_bytes > VENUS_CLIENT_STREAM_BYTES - 4) {
		client->wire_error = EOVERFLOW;
		return;
	}

	/* Store the low-to-high bytes of this little-endian word. */
	position = client->stream_bytes;
	client->stream[position] = (uint8_t)word;
	client->stream[position + 1] = (uint8_t)(word >> 8);
	client->stream[position + 2] = (uint8_t)(word >> 16);
	client->stream[position + 3] = (uint8_t)(word >> 24);
	client->stream_bytes += 4;

	/* Succeeded. */
	return;
}

/*
 * Serialize an eight-byte handle, pointer count or Vulkan size.
 */
void
venus_client_wire_u64(
	struct venus_client *client,
	uint64_t number)
{
	/* Emit the low and high halves in protocol byte order. */
	venus_client_wire_u32(client, (uint32_t)number);
	venus_client_wire_u32(client, (uint32_t)(number >> 32));

	/* Succeeded. */
	return;
}

/*
 * Consume one response word while retaining a malformed-reply error.
 */
uint32_t
venus_client_reply_u32(
	struct venus_client *client)
{
	uint32_t word;

	/* Keep the first decoding failure until a new command begins. */
	if (client->wire_error != 0) {
		return 0;
	}

	/* The last twenty bytes belong exclusively to the completion trailer. */
	if (client->reply_cursor > VENUS_CLIENT_REPLY_BYTES - 24) {
		client->wire_error = EIO;
		return 0;
	}

	/* Consume the next validated response word. */
	word = load_u32(client->reply + client->reply_cursor);
	client->reply_cursor += 4;

	/* Succeeded. */
	return word;
}

/*
 * Consume a wire handle or count without native structure padding.
 */
uint64_t
venus_client_reply_u64(
	struct venus_client *client)
{
	uint64_t number;
	uint32_t high;

	/* Combine consecutive low and high protocol words. */
	number = venus_client_reply_u32(client);
	high = venus_client_reply_u32(client);
	number |= (uint64_t)high << 32;

	/* Succeeded. */
	return number;
}

/*
 * Start a Vulkan structure with an absent extension chain.
 */
void
venus_client_wire_structure(
	struct venus_client *client,
	uint32_t type)
{
	/* Emit the Vulkan structure tag and an absent extension chain. */
	venus_client_wire_u32(client, type);
	venus_client_wire_u64(client, 0);

	/* Succeeded. */
	return;
}

/*
 * Prefix one reply-producing operation with its owned reply storage.
 */
void
venus_client_command_begin(
	struct venus_client *client,
	uint32_t command)
{
	/* Reset the single-flight command and response cursors. */
	client->stream_bytes = 0;
	client->reply_cursor = 0;
	client->wire_error = 0;
	client->active_command = command;

	/* SetReply takes a pointer to resource ID, byte offset and byte length. */
	venus_client_wire_u32(client, VENUS_SET_REPLY_STREAM);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u32(client, client->reply_resource);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u64(client, VENUS_CLIENT_REPLY_BYTES);
	venus_client_wire_u32(client, command);
	venus_client_wire_u32(client, 1);

	/* Succeeded. */
	return;
}

/*
 * Copy resource bytes through the bounded, owned GPU resource interface.
 */
int
venus_client_resource_copy(
	struct venus_client *client,
	uint64_t handle,
	uint64_t offset,
	void *buffer,
	uint32_t bytes,
	int write)
{
	struct gpu_transfer transfer;
	uint8_t *position;
	uint32_t amount;
	unsigned long operation;
	int status;

	/* A closed session cannot own a resource, even for an empty copy. */
	if (client->fd < 0) {
		errno = EBADF;
		return -1;
	}

	/* Reject a wrapped resource interval before issuing a partial transfer. */
	if (offset > UINT64_MAX - bytes) {
		errno = EOVERFLOW;
		return -1;
	}

	/* A nonempty transfer needs caller-owned storage for the entire copy. */
	if (bytes != 0) {
		/* An absent buffer cannot back a nonempty user-space copy. */
		if (buffer == NULL) {
			errno = EINVAL;
			return -1;
		}
	}

	/* Select the copy direction and the first caller byte. */
	position = buffer;
	operation = GPU_RESOURCE_READ;
	if (write) {
		operation = GPU_RESOURCE_WRITE;
	}

	/* Each system call copies at most the ABI limit. */
	while (bytes != 0) {
		/* Limit this copy to one ABI-sized transfer. */
		amount = bytes;
		if (amount > GPU_COPY_MAX) {
			amount = GPU_COPY_MAX;
		}

		/* Describe one validated resource span without reserved input bits. */
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = handle;
		transfer.offset = offset;
		transfer.address = (uintptr_t)position;
		transfer.bytes = amount;
		status = ioctl(client->fd, operation, &transfer);
		if (status != 0) {
			return -1;
		}

		/* Advance to the remaining bytes after a successful copy. */
		position += amount;
		offset += amount;
		bytes -= amount;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Yield during finite renderer and fence polling.
 */
int
venus_client_poll_pause(
	struct venus_client *client)
{
	struct timespec delay;
	int status;

	/* Retain the session precondition while yielding between its commands. */
	if (client->fd < 0) {
		errno = EBADF;
		return -1;
	}

	/* Request a short yield without extending the finite poll count. */
	delay.tv_sec = 0;
	delay.tv_nsec = 10000000;
	status = nanosleep(&delay, NULL);
	if (status != 0 && errno != EINTR) {
		return -1;
	}

	/* Succeeded, including an interrupted interval within the finite budget. */
	return 0;
}

/*
 * Wait for a renderer-written trailer before inspecting the command reply.
 */
int
venus_client_command_finish(
	struct venus_client *client,
	int has_result,
	int allow_pending)
{
	struct gpu_command request;
	uint8_t marker[4];
	uint32_t poll;
	uint32_t version;
	uint32_t type;
	int32_t result;
	int status;

	/* A final API-version store proves the renderer consumed this stream. */
	venus_client_wire_u32(client, VENUS_SEEK_REPLY_STREAM);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, VENUS_CLIENT_REPLY_BYTES - 20);
	venus_client_wire_u32(client, VENUS_ENUMERATE_INSTANCE_VERSION);
	venus_client_wire_u32(client, 1);
	venus_client_wire_u64(client, 1);

	/* Reject encoding or decoding overflow before using the result. */
	if (client->wire_error != 0) {
		errno = client->wire_error;
		return -1;
	}

	/* Clear the prior completion store before submitting a new stream. */
	memset(marker, 0, sizeof(marker));

	/* Copy the requested bytes through the owned resource interface. */
	status = venus_client_resource_copy(client, client->reply_handle, VENUS_CLIENT_REPLY_BYTES - 4, marker, 4, 1);
	if (status != 0) {
		return -1;
	}

	/* Submit only the bounded aligned bytes encoded by this client. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.address = (uintptr_t)client->stream;
	request.bytes = client->stream_bytes;
	status = ioctl(client->fd, GPU_COMMAND, &request);
	if (status != 0) {
		return -1;
	}

	/* Virtqueue receipt alone does not imply Venus decoder completion. */
	version = 0;
	for (poll = 0; poll < VENUS_CLIENT_POLL_LIMIT; poll++) {
		/* Copy the requested bytes through the owned resource interface. */
		status = venus_client_resource_copy(client, client->reply_handle, VENUS_CLIENT_REPLY_BYTES - 4, marker, 4, 0);
		if (status != 0) {
			return -1;
		}

		/* Retry a torn bytewise snapshot until its API-version prefix is present. */
		version = load_u32(marker);
		if (version >= VK_VERSION_1_1) {
			break;
		}

		/* Yield before the next bounded completion check. */
		status = venus_client_poll_pause(client);
		if (status != 0) {
			return -1;
		}
	}

	/* Fail when the renderer does not produce a compatible completion trailer. */
	if (version < VK_VERSION_1_1) {
		fprintf(
			stderr,
			"Venus reply timeout command=%u marker=%u\n",
			client->active_command,
			version);
		errno = ETIMEDOUT;
		return -1;
	}

	/* Fetch the body after observing the marker, never in the earlier copy. */
	status = venus_client_resource_copy(client, client->reply_handle, 0, client->reply, VENUS_CLIENT_REPLY_BYTES, 0);
	if (status != 0) {
		return -1;
	}

	/* Check the entire fixed trailer before trusting its final store. */
	type = load_u32(client->reply + VENUS_CLIENT_REPLY_BYTES - 20);
	result = (int32_t)load_u32(client->reply + VENUS_CLIENT_REPLY_BYTES - 16);
	version = load_u32(client->reply + VENUS_CLIENT_REPLY_BYTES - 12);
	poll = load_u32(client->reply + VENUS_CLIENT_REPLY_BYTES - 8);
	if (type != VENUS_ENUMERATE_INSTANCE_VERSION ||
		result != 0 ||
		version != 1 ||
		poll != 0) {
		errno = EIO;
		return -1;
	}

	/* Require the response to belong to this single outstanding command. */
	type = venus_client_reply_u32(client);
	if (type != client->active_command) {
		fprintf(stderr, "Venus reply command=%u expected=%u\n", type, client->active_command);
		errno = EIO;
		return -1;
	}

	/* VkResult belongs to Vulkan, separate from successful transport. */
	if (has_result) {
		/* Interpret Vulkan's result independently of the transport result. */
		result = (int32_t)venus_client_reply_u32(client);
		if (result != 0) {
			/* Allow only explicitly requested incomplete or pending Vulkan results. */
			if (allow_pending &&
				(result == 1 || result == 5)) {
				return result;
			}

			/* Preserve the Vulkan failure in diagnostics before returning EIO. */
			fprintf(stderr, "Vulkan command=%u result=%d\n", type, result);
			errno = EIO;
			return -1;
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Validate the returned pointer and guest-selected object identity.
 */
int
venus_client_reply_handle(
	struct venus_client *client,
	uint64_t expected)
{
	uint64_t count;
	uint64_t handle;

	/* Decode the returned pointer count and object identity. */
	count = venus_client_reply_u64(client);
	handle = venus_client_reply_u64(client);
	if (count != 1 ||
		handle != expected ||
		client->wire_error != 0) {
		errno = EIO;
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Decode an unaligned little-endian protocol word. */
static uint32_t
load_u32(
	const uint8_t *data)
{
	uint32_t word;

	/* Reassemble one protocol word without alignment assumptions. */
	word = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
		((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);

	/* Succeeded. */
	return word;
}

/* Create an extension-free Vulkan 1.1 instance. */
static int
create_instance(
	struct venus_client *client)
{
	int status;

	/* Encode create instance. */
	venus_client_command_begin(client, VENUS_CREATE_INSTANCE);
	venus_client_wire_u64(client, 1);
	venus_client_wire_structure(client, STRUCTURE_INSTANCE_CREATE_INFO);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 1);
	venus_client_wire_structure(client, STRUCTURE_APPLICATION_INFO);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u32(client, VK_VERSION_1_1);

	/* Empty layer and extension arrays, no allocation callbacks. */
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u64(client, VENUS_OBJECT_INSTANCE);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(client, VENUS_OBJECT_INSTANCE);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Select the first physical device and a graphics-capable queue family. */
static int
select_physical_device(
	struct venus_client *client)
{
	uint64_t present;
	uint64_t length;
	uint64_t handle;
	uint32_t count;
	uint32_t index;
	uint32_t flags;
	uint32_t queues;
	int status;

	/* Encode enumerate physical devices. */
	venus_client_command_begin(client, VENUS_ENUMERATE_PHYSICAL_DEVICES);
	venus_client_wire_u64(client, VENUS_OBJECT_INSTANCE);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u32(client, 1);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u64(client, VENUS_OBJECT_PHYSICAL_DEVICE);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(client, 1, 1);
	if (status < 0) {
		return -1;
	}

	/* Validate the returned pointer and bounded output payload. */
	present = venus_client_reply_u64(client);
	count = venus_client_reply_u32(client);
	length = venus_client_reply_u64(client);
	handle = venus_client_reply_u64(client);
	if (present != 1 ||
		count != 1 ||
		length != 1 ||
		handle != VENUS_OBJECT_PHYSICAL_DEVICE) {
		errno = ENODEV;
		return -1;
	}

	/* Sixteen families bound this diagnostic's output allocation. */
	venus_client_command_begin(client, VENUS_GET_QUEUE_FAMILIES);
	venus_client_wire_u64(client, VENUS_OBJECT_PHYSICAL_DEVICE);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u32(client, 16);
	venus_client_wire_u64(client, 16);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Validate the returned pointer and bounded output payload. */
	present = venus_client_reply_u64(client);
	count = venus_client_reply_u32(client);
	length = venus_client_reply_u64(client);
	if (present != 1 ||
		count == 0 ||
		count > 16 ||
		length != count) {
		errno = EIO;
		return -1;
	}

	/* Start with no matching family before examining the returned list. */
	client->queue_family = UINT32_MAX;

	/* Consume all fields to retain exact wire alignment. */
	for (index = 0; index < count; index++) {
		/* Read queue capabilities, capacity and transfer granularity. */
		flags = venus_client_reply_u32(client);
		queues = venus_client_reply_u32(client);
		venus_client_reply_u32(client);
		venus_client_reply_u32(client);
		venus_client_reply_u32(client);
		venus_client_reply_u32(client);

		/* Retain the first graphics-capable family with an available queue. */
		if ((flags & 1) != 0 &&
			queues != 0 &&
			client->queue_family == UINT32_MAX) {
			client->queue_family = index;
		}
	}

	/* Reject a device without a usable queue or complete response. */
	if (client->queue_family == UINT32_MAX || client->wire_error != 0) {
		errno = ENODEV;
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Query the physical device's fixed Vulkan memory-type and heap arrays. */
static int
query_memory(
	struct venus_client *client)
{
	uint64_t present;
	uint64_t length;
	uint32_t index;
	uint32_t heaps;
	int status;

	/* Encode get memory properties. */
	venus_client_command_begin(client, VENUS_GET_MEMORY_PROPERTIES);
	venus_client_wire_u64(client, VENUS_OBJECT_PHYSICAL_DEVICE);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u64(client, 32);
	venus_client_wire_u64(client, 16);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Validate the returned pointer and bounded output payload. */
	present = venus_client_reply_u64(client);
	client->memory_count = venus_client_reply_u32(client);
	length = venus_client_reply_u64(client);
	if (present != 1 ||
		client->memory_count == 0 ||
		client->memory_count > 32 ||
		length != 32) {
		errno = EIO;
		return -1;
	}

	/* Heap identity is irrelevant after selecting the returned type flags. */
	for (index = 0; index < 32; index++) {
		client->memory_flags[index] = venus_client_reply_u32(client);
		venus_client_reply_u32(client);
	}

	/* Validate the fixed-size Vulkan heap description array. */
	heaps = venus_client_reply_u32(client);
	length = venus_client_reply_u64(client);
	if (heaps == 0 ||
		heaps > 16 ||
		length != 16) {
		errno = EIO;
		return -1;
	}

	/* Consume every heap size and flag pair, including unused entries. */
	for (index = 0; index < 16; index++) {
		venus_client_reply_u64(client);
		venus_client_reply_u32(client);
	}

	/* Reject encoding or decoding overflow before using the result. */
	if (client->wire_error != 0) {
		errno = client->wire_error;
		return -1;
	}

	/* Succeeded. */
	return 0;
}

/* Create one Vulkan queue without optional device features or extensions. */
static int
create_device(
	struct venus_client *client)
{
	int status;

	/* Encode create device. */
	venus_client_command_begin(client, VENUS_CREATE_DEVICE);
	venus_client_wire_u64(client, VENUS_OBJECT_PHYSICAL_DEVICE);
	venus_client_wire_u64(client, 1);
	venus_client_wire_structure(client, STRUCTURE_DEVICE_CREATE_INFO);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u32(client, 1);
	venus_client_wire_u64(client, 1);
	venus_client_wire_structure(client, STRUCTURE_DEVICE_QUEUE_CREATE_INFO);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u32(client, client->queue_family);
	venus_client_wire_u32(client, 1);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u32(client, 0x3f800000U);

	/* Empty layers/extensions, absent feature structure and allocator. */
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u64(client, 0);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u64(client, VENUS_OBJECT_DEVICE);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(client, 1, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(client, VENUS_OBJECT_DEVICE);
	if (status != 0) {
		return -1;
	}

	/* Register the queue once through the renderer's required Queue2 operation. */
	venus_client_command_begin(client, VENUS_GET_DEVICE_QUEUE2);
	venus_client_wire_u64(client, VENUS_OBJECT_DEVICE);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u32(client, STRUCTURE_DEVICE_QUEUE_INFO2);

	/* Bind this queue to timeline one; zero is reserved for decoder completion. */
	venus_client_wire_u64(client, 1);
	venus_client_wire_structure(client, STRUCTURE_DEVICE_QUEUE_TIMELINE_INFO);
	venus_client_wire_u32(client, 1);

	/* Select unprotected queue zero of the device's created graphics family. */
	venus_client_wire_u32(client, 0);
	venus_client_wire_u32(client, client->queue_family);
	venus_client_wire_u32(client, 0);
	venus_client_wire_u64(client, 1);
	venus_client_wire_u64(client, VENUS_OBJECT_QUEUE);

	/* Submit the completed encoding and validate its renderer reply. */
	status = venus_client_command_finish(client, 0, 0);
	if (status != 0) {
		return -1;
	}

	/* Confirm the renderer retained this client-selected object identity. */
	status = venus_client_reply_handle(client, VENUS_OBJECT_QUEUE);
	if (status != 0) {
		return -1;
	}

	/* Succeeded. */
	return 0;
}

