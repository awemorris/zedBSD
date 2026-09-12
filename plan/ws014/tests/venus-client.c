/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Exercise the shared production client through independent synthetic sessions. */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../../userland/gpu/venus/client.h"

#define PEER_COUNT 2U
#define STORAGE_BYTES (GPU_COPY_MAX * 2U + 17U)

/* Each peer exists until close consumes its descriptor, including failed close. */
struct test_peer {
	int live;
	uint32_t closes;
	uint32_t commands;
	uint32_t capsets;
	uint32_t copies;
	uint32_t full_replies;
	uint32_t fail_operation;
	uint32_t fail_command;
	uint32_t pending;
	int no_completion;
	int bad_trailer;
	int fail_close;
	uint8_t reply[VENUS_CLIENT_REPLY_BYTES];
	uint8_t storage[STORAGE_BYTES];
};

/* Tests serialize peer changes; resetting this array starts a new ownership case. */
static struct test_peer peers[PEER_COUNT];

/* Each mocked sleep consumes one finite polling step without delaying the test. */
static uint32_t pause_count;

static struct test_peer *get_peer(int descriptor);
static uint32_t read_word(const uint8_t *bytes);
static void store_word(uint8_t *bytes, uint32_t word);
static void append_word(uint8_t *bytes, uint32_t *cursor, uint32_t word);
static void append_number(uint8_t *bytes, uint32_t *cursor, uint64_t number);
static void make_reply(struct test_peer *peer, uint32_t command);
static void test_independent_sessions(void);
static void test_partial_bootstrap(void);
static void test_boundaries(void);
static void test_completion(void);

int client_test_open(const char *path, int flags, ...);
int client_test_close(int descriptor);
int client_test_ioctl(int descriptor, unsigned long operation, ...);
int client_test_nanosleep(const struct timespec *delay, struct timespec *remaining);

/*
 * Runs the finite shared-client ownership and protocol boundary cases.
 */
int
main(void)
{
	/* Exercise per-session storage and ordinary 2D acquisition first. */
	test_independent_sessions();

	/* Verify teardown after every bootstrap stage, including failed close. */
	test_partial_bootstrap();

	/* Exercise stream, reply and resource-copy boundaries. */
	test_boundaries();

	/* Reject asynchronous receipt, malformed trailers and Vulkan errors. */
	test_completion();

	/* Succeeded: every shared-client ownership and boundary case passed. */
	puts("venus-client: independent sessions, partial bootstrap, bounds and completion PASS");
	return 0;
}

/*
 * Assigns one fresh descriptor without sharing its reply or storage memory.
 */
int
client_test_open(
	const char *path,
	int flags,
	...)
{
	uint32_t index;

	/* Require the public client's ordinary read/write acquisition contract. */
	assert(path != NULL);
	assert(flags == O_RDWR);

	/* Allocate the first unused synthetic descriptor. */
	for (index = 0; index < PEER_COUNT; index++) {
		/* Preserve failure controls installed before this open. */
		if (!peers[index].live) {
			break;
		}
	}

	/* Refuse an accidental third live session in this bounded fixture. */
	if (index == PEER_COUNT) {
		errno = EMFILE;
		return -1;
	}

	/* Publish ownership only after finding a free synthetic descriptor. */
	peers[index].live = 1;

	/* Succeeded: the selected descriptor belongs to this new session. */
	return (int)index + 100;
}

/*
 * Consumes a descriptor before reporting a final device-release error.
 */
int
client_test_close(
	int descriptor)
{
	struct test_peer *peer;

	/* Resolve the descriptor while its session still exists. */
	peer = get_peer(descriptor);
	assert(peer->live);

	/* Retire ownership even when final backend cleanup reports a failure. */
	peer->live = 0;
	peer->closes++;

	/* Model zedBSD filedesc_close removing the slot before file_close. */
	if (peer->fail_close) {
		errno = EIO;
		return -1;
	}

	/* Succeeded: this descriptor cannot be closed a second time. */
	return 0;
}

/*
 * Serves bounded GPU requests from descriptor-specific reply and storage bytes.
 */
int
client_test_ioctl(
	int descriptor,
	unsigned long operation,
	...)
{
	struct test_peer *peer;
	struct gpu_info *info;
	struct gpu_capset *capset;
	struct gpu_blob_create *blob;
	struct gpu_transfer *transfer;
	struct gpu_command *submission;
	const uint8_t *stream;
	uint8_t *memory;
	void *argument;
	va_list arguments;
	uint64_t capacity;
	uint32_t command;
	uint32_t word;

	/* Resolve exactly the session named by the production request. */
	peer = get_peer(descriptor);
	assert(peer->live);
	va_start(arguments, operation);
	argument = va_arg(arguments, void *);
	va_end(arguments);

	/* Fail one selected bootstrap boundary before producing any output. */
	if (operation == peer->fail_operation) {
		errno = ENOMEM;
		return -1;
	}

	/* Fill ordinary capabilities without invoking Vulkan initialization. */
	if (operation == GPU_GET_INFO) {
		info = argument;
		assert(info->version == GPU_ABI_VERSION);
		assert(info->size == sizeof(*info));
		memcpy(info->driver_name, "fixture", 8);
		return 0;
	}

	/* Advertise the exact wire prefix and multiple-timeline bootstrap contract. */
	if (operation == GPU_GET_CAPSET) {
		capset = argument;
		assert(capset->capset_id == 4);
		assert(capset->capacity == GPU_CAPSET_MAX);
		capset->bytes = 160;
		store_word(capset->data, 1);
		store_word(capset->data + 4, 4206861);
		store_word(capset->data + 152, 1);
		peer->capsets++;
		return 0;
	}

	/* Give each context a distinct owned handle and renderer resource identity. */
	if (operation == GPU_BLOB_CREATE) {
		blob = argument;
		assert(blob->blob_id == 0);
		assert(blob->bytes == VENUS_CLIENT_REPLY_BYTES);
		assert(blob->flags == GPU_BLOB_MAPPABLE);
		blob->handle = (uint64_t)(unsigned)descriptor + 1;
		blob->resource_id = (uint32_t)descriptor + 2;
		return 0;
	}

	/* Validate reply ownership and complete only commands accepted by this peer. */
	if (operation == GPU_COMMAND) {
		submission = argument;
		assert(submission->bytes >= 76);
		assert(submission->bytes <= VENUS_CLIENT_STREAM_BYTES);
		assert((submission->bytes & 3) == 0);
		stream = (const uint8_t *)(uintptr_t)submission->address;

		/* The command prefix must select this peer's own reply resource. */
		word = read_word(stream);
		assert(word == 178);
		word = read_word(stream + 16);
		assert(word == (uint32_t)descriptor + 2);
		command = read_word(stream + 36);
		peer->commands++;

		/* Refuse a selected Vulkan command after all earlier objects exist. */
		if (peer->fail_command == command + 1) {
			errno = ENOMEM;
			return -1;
		}

		/* Queue receipt alone deliberately leaves the reply marker absent. */
		if (peer->no_completion) {
			return 0;
		}

		/* Publish the synthetic reply before its independent completion marker. */
		make_reply(peer, command);
		return 0;
	}

	/* Any remaining operation must be a bounded owned resource copy. */
	assert(operation == GPU_RESOURCE_READ || operation == GPU_RESOURCE_WRITE);
	transfer = argument;
	assert(transfer->version == GPU_ABI_VERSION);
	assert(transfer->size == sizeof(*transfer));
	assert(transfer->bytes <= GPU_COPY_MAX);
	peer->copies++;

	/* Resource handle 900 is ordinary storage; the reply handle is peer-specific. */
	memory = peer->storage;
	capacity = sizeof(peer->storage);
	if (transfer->handle != 900) {
		assert(transfer->handle == (uint64_t)(unsigned)descriptor + 1);
		memory = peer->reply;
		capacity = sizeof(peer->reply);

		/* Count full snapshots separately from the polled completion word. */
		if (transfer->bytes == VENUS_CLIENT_REPLY_BYTES) {
			peer->full_replies++;
		}
	}

	/* Enforce an interval before either copy can touch synthetic device memory. */
	assert(transfer->offset <= capacity);
	assert(transfer->bytes <= capacity - transfer->offset);

	/* Preserve the requested transfer direction without inventing returned bytes. */
	if (operation == GPU_RESOURCE_WRITE) {
		memcpy(
			memory + transfer->offset,
			(const void *)(uintptr_t)transfer->address,
			transfer->bytes);
	} else {
		memcpy(
			(void *)(uintptr_t)transfer->address,
			memory + transfer->offset,
			transfer->bytes);
	}

	/* Succeeded: exactly this session's requested span was copied. */
	return 0;
}

/*
 * Consumes polling intervals without sleeping during deterministic host tests.
 */
int
client_test_nanosleep(
	const struct timespec *delay,
	struct timespec *remaining)
{
	/* The production loop requests fixed short intervals and no remainder. */
	assert(delay->tv_sec == 0);
	assert(delay->tv_nsec == 10000000);
	assert(remaining == NULL);
	pause_count++;

	/* Succeeded: one bounded polling opportunity has elapsed. */
	return 0;
}

/* Resolve a bounded descriptor without allowing cross-session indexing. */
static struct test_peer *
get_peer(
	int descriptor)
{
	/* Only the two synthetic descriptors belong to this fixture. */
	assert(descriptor >= 100);
	assert(descriptor < 100 + (int)PEER_COUNT);

	/* Succeeded: return the sole owner of this descriptor's memory. */
	return &peers[descriptor - 100];
}

/* Decode protocol bytes independently of the production client's cursor. */
static uint32_t
read_word(
	const uint8_t *bytes)
{
	uint32_t word;

	/* Join bytes using the public little-endian protocol representation. */
	word = (uint32_t)bytes[0];
	word |= (uint32_t)bytes[1] << 8;
	word |= (uint32_t)bytes[2] << 16;
	word |= (uint32_t)bytes[3] << 24;

	/* Succeeded: return the unaligned protocol word. */
	return word;
}

/* Store one protocol word without using the production encoder. */
static void
store_word(
	uint8_t *bytes,
	uint32_t word)
{
	/* Serialize the independent peer's four low-to-high bytes. */
	bytes[0] = (uint8_t)word;
	bytes[1] = (uint8_t)(word >> 8);
	bytes[2] = (uint8_t)(word >> 16);
	bytes[3] = (uint8_t)(word >> 24);

	/* Succeeded: the peer's protocol word is stored. */
	return;
}

/* Append one word to a fixture-generated bootstrap response. */
static void
append_word(
	uint8_t *bytes,
	uint32_t *cursor,
	uint32_t word)
{
	/* Keep synthetic bootstrap output outside its reserved completion trailer. */
	assert(*cursor <= VENUS_CLIENT_REPLY_BYTES - 24);
	store_word(bytes + *cursor, word);
	*cursor += 4;

	/* Succeeded: the peer's response cursor follows the appended word. */
	return;
}

/* Append an unpadded pointer count, object identity or memory size. */
static void
append_number(
	uint8_t *bytes,
	uint32_t *cursor,
	uint64_t number)
{
	/* Emit the protocol's consecutive low and high words. */
	append_word(bytes, cursor, (uint32_t)number);
	append_word(bytes, cursor, (uint32_t)(number >> 32));

	/* Succeeded: eight response bytes were appended. */
	return;
}

/* Produce fixed bounded Vulkan outputs with a separately validated trailer. */
static void
make_reply(
	struct test_peer *peer,
	uint32_t command)
{
	uint32_t cursor;
	uint32_t index;
	uint32_t result;

	/* Start one response in only the selected session's reply memory. */
	memset(peer->reply, 0, sizeof(peer->reply));
	cursor = 0;
	append_word(peer->reply, &cursor, command);

	/* Bootstrap Vulkan results are successful; fence results are configurable. */
	if (command == 0 ||
		command == 2 ||
		command == 11 ||
		command == 38) {
		result = 0;

		/* Exercise Vulkan's allowed pending and rejected error outcomes. */
		if (command == 38) {
			result = peer->pending;
		}

		/* Append the result before the command's ordinary return payload. */
		append_word(peer->reply, &cursor, result);
	}

	/* Supply only the bootstrap operations used by the shared production client. */
	switch (command) {
	case 0:
		/* Create-instance returns a pointer count and reserved instance identity. */
		append_number(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, VENUS_OBJECT_INSTANCE);
		break;
	case 2:
		/* Enumeration returns one physical device through both count and array. */
		append_number(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, VENUS_OBJECT_PHYSICAL_DEVICE);
		break;
	case 7:
		/* The first of one returned families supports one graphics queue. */
		append_number(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 64);
		append_word(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 1);
		break;
	case 8:
		/* Vulkan memory properties contain fixed 32-type and 16-heap arrays. */
		append_number(peer->reply, &cursor, 1);
		append_word(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, 32);

		/* Every fixture type uses the same visible/coherent heap. */
		for (index = 0; index < 32; index++) {
			append_word(peer->reply, &cursor, 7);
			append_word(peer->reply, &cursor, 0);
		}

		/* Include every fixed heap slot even though only the first is live. */
		append_word(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, 16);
		for (index = 0; index < 16; index++) {
			append_number(peer->reply, &cursor, 1048576);
			append_word(peer->reply, &cursor, 1);
		}
		break;
	case 11:
		/* Create-device retains the common reserved device identity. */
		append_number(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, VENUS_OBJECT_DEVICE);
		break;
	case 155:
		/* Queue2 returns the common reserved queue identity without VkResult. */
		append_number(peer->reply, &cursor, 1);
		append_number(peer->reply, &cursor, VENUS_OBJECT_QUEUE);
		break;
	default:
		/* Other test commands need only the common command/result prefix. */
		break;
	}

	/* A separate API-version reply proves renderer completion after the body. */
	store_word(peer->reply + VENUS_CLIENT_REPLY_BYTES - 20, 137);
	store_word(peer->reply + VENUS_CLIENT_REPLY_BYTES - 12, 1);
	store_word(peer->reply + VENUS_CLIENT_REPLY_BYTES - 4, 0x00401000U);

	/* A visible marker alone must not make malformed trailer fields acceptable. */
	if (peer->bad_trailer) {
		store_word(peer->reply + VENUS_CLIENT_REPLY_BYTES - 8, 1);
	}

	/* Succeeded: the peer completed one independently generated response. */
	return;
}

/* Verify two open clients never share command cursors, resources or replies. */
static void
test_independent_sessions(void)
{
	struct venus_client first;
	struct venus_client second;
	uint32_t first_bytes;
	uint32_t saved_command;
	int status;

	/* Start two sessions through the ordinary non-Vulkan acquisition path. */
	memset(peers, 0, sizeof(peers));
	status = venus_client_open(&first, "first");
	assert(status == 0);
	status = venus_client_open(&second, "second");
	assert(status == 0);
	assert(first.fd != second.fd);
	assert(peers[0].capsets == 0);
	assert(peers[1].capsets == 0);

	/* Bootstrap both sessions, retaining distinct kernel and renderer identities. */
	status = venus_client_init_vulkan(&first);
	assert(status == 0);
	status = venus_client_init_vulkan(&second);
	assert(status == 0);
	assert(first.reply_handle != second.reply_handle);
	assert(first.reply_resource != second.reply_resource);
	assert(first.memory_count == 1);
	assert(first.memory_flags[0] == 7);
	assert(second.queue_family == 0);
	assert(peers[0].commands == 6);
	assert(peers[1].commands == 6);

	/* Interleave encoders without disturbing the first client's partial command. */
	venus_client_command_begin(&first, 38);
	venus_client_wire_u64(&first, 0x12345678U);
	first_bytes = first.stream_bytes;
	venus_client_command_begin(&second, 38);
	venus_client_wire_u32(&second, 0xabcdef01U);
	assert(first.stream_bytes == first_bytes);
	assert(first.stream[44] == 0x78);
	assert(second.stream[44] == 1);

	/* Independent command completion must preserve the other client's bytes. */
	status = venus_client_command_finish(&second, 1, 0);
	assert(status == 0);
	assert(first.stream_bytes == first_bytes);
	status = venus_client_command_finish(&first, 1, 0);
	assert(status == 0);

	/* Reject duplicate bootstrap rather than reusing live reserved object IDs. */
	status = venus_client_init_vulkan(&first);
	assert(status == -1);
	assert(errno == EALREADY);

	/* Keep diagnostic command identity after descriptor consumption. */
	saved_command = first.active_command;
	status = venus_client_close(&first);
	assert(status == 0);
	assert(first.fd == -1);
	assert(first.active_command == saved_command);
	assert(peers[1].live);
	status = venus_client_close(&first);
	assert(status == 0);
	assert(peers[0].closes == 1);
	status = venus_client_close(&second);
	assert(status == 0);

	/* Succeeded: neither session used or consumed the other's state. */
	return;
}

/* Fail each acquisition and bootstrap boundary, then release exactly once. */
static void
test_partial_bootstrap(void)
{
	static const uint32_t commands[] = { 0, 2, 7, 8, 11, 155 };
	struct venus_client client;
	uint32_t stage;
	int status;

	/* Preserve GET_INFO's error even if automatic close encounters another error. */
	memset(peers, 0, sizeof(peers));
	peers[0].fail_operation = GPU_GET_INFO;
	peers[0].fail_close = 1;
	status = venus_client_open(&client, "info-failure");
	assert(status == -1);
	assert(errno == ENOMEM);
	assert(client.fd == -1);
	assert(peers[0].closes == 1);

	/* Cover capset, reply allocation and every partial Vulkan bootstrap stage. */
	for (stage = 0; stage < 8; stage++) {
		/* Reset ownership before selecting this stage's failure boundary. */
		memset(peers, 0, sizeof(peers));
		status = venus_client_open(&client, "partial-bootstrap");
		assert(status == 0);

		/* Early boundaries fail before any Vulkan command owns object IDs. */
		if (stage == 0) {
			peers[0].fail_operation = GPU_GET_CAPSET;
		} else if (stage == 1) {
			peers[0].fail_operation = GPU_BLOB_CREATE;
		} else {
			peers[0].fail_command = commands[stage - 2] + 1;
		}

		/* Every failure retains the descriptor for normal caller cleanup. */
		status = venus_client_init_vulkan(&client);
		assert(status == -1);
		assert(errno == ENOMEM);
		assert(client.fd == 100);
		assert(peers[0].live);

		/* A partial initialization must not retry reserved identities. */
		status = venus_client_init_vulkan(&client);
		assert(status == -1);
		assert(errno == EALREADY);

		/* Failed final release still consumes the descriptor exactly once. */
		peers[0].fail_close = 1;
		status = venus_client_close(&client);
		assert(status == -1);
		assert(client.fd == -1);
		assert(client.reply_handle == 0);
		assert(client.reply_resource == 0);
		assert(!peers[0].live);
		status = venus_client_close(&client);
		assert(status == 0);
		assert(peers[0].closes == 1);
	}

	/* Succeeded: partial Vulkan ownership always ends with one descriptor release. */
	return;
}

/* Check meaningful stream/reply exclusion and complete resource-copy chunking. */
static void
test_boundaries(void)
{
	struct venus_client client;
	uint8_t original[STORAGE_BYTES];
	uint8_t copied[STORAGE_BYTES];
	uint32_t index;
	uint32_t count;
	uint32_t word;
	int comparison;
	int status;

	/* Open ordinary storage access without activating the Vulkan bootstrap. */
	memset(peers, 0, sizeof(peers));
	status = venus_client_open(&client, "boundaries");
	assert(status == 0);

	/* Populate a pattern crossing two complete ABI-sized copy boundaries. */
	for (index = 0; index < sizeof(original); index++) {
		original[index] = (uint8_t)(index * 17U + index / 251U);
	}

	/* Write and read the entire resource through three calls in each direction. */
	status = venus_client_resource_copy(&client, 900, 0, original, sizeof(original), 1);
	assert(status == 0);
	assert(peers[0].copies == 3);
	status = venus_client_resource_copy(&client, 900, 0, copied, sizeof(copied), 0);
	assert(status == 0);
	assert(peers[0].copies == 6);
	comparison = memcmp(original, copied, sizeof(original));
	assert(comparison == 0);

	/* Reject wrapping offsets before a partial copy can reach the peer. */
	count = peers[0].copies;
	status = venus_client_resource_copy(&client, 900, UINT64_MAX, original, 1, 1);
	assert(status == -1);
	assert(errno == EOVERFLOW);
	assert(peers[0].copies == count);

	/* Exhaust the bounded encoder while retaining its first overflow error. */
	for (index = 0; index < VENUS_CLIENT_STREAM_BYTES / 4; index++) {
		venus_client_wire_u32(&client, index);
	}

	/* Encoding overflow must never submit or modify shared reply memory. */
	venus_client_wire_u32(&client, 1);
	assert(client.wire_error == EOVERFLOW);
	status = venus_client_command_finish(&client, 0, 0);
	assert(status == -1);
	assert(errno == EOVERFLOW);
	assert(peers[0].copies == count);
	assert(peers[0].commands == 0);

	/* A fresh command resets the earlier stream failure before reply decoding. */
	venus_client_command_begin(&client, 38);

	/* Allow the final reply word but exclude the twenty-byte completion trailer. */
	client.reply_cursor = VENUS_CLIENT_REPLY_BYTES - 24;
	store_word(client.reply + client.reply_cursor, 0x12345678U);
	word = venus_client_reply_u32(&client);
	assert(word == 0x12345678U);
	word = venus_client_reply_u32(&client);
	assert(word == 0);
	assert(client.wire_error == EIO);

	/* Release the non-Vulkan session after its bounded-copy tests. */
	status = venus_client_close(&client);
	assert(status == 0);

	/* Succeeded: boundary refusals preceded transport side effects. */
	return;
}

/* Require genuine reply completion and validate Vulkan's separate result space. */
static void
test_completion(void)
{
	struct venus_client client;
	uint32_t full_replies;
	int status;

	/* Establish the normal production bootstrap before probing completion errors. */
	memset(peers, 0, sizeof(peers));
	status = venus_client_open(&client, "completion");
	assert(status == 0);
	status = venus_client_init_vulkan(&client);
	assert(status == 0);

	/* VK_NOT_READY is returned only when explicitly allowed by the caller. */
	peers[0].pending = 1;
	venus_client_command_begin(&client, 38);
	status = venus_client_command_finish(&client, 1, 1);
	assert(status == 1);
	venus_client_command_begin(&client, 38);
	status = venus_client_command_finish(&client, 1, 0);
	assert(status == -1);
	assert(errno == EIO);

	/* VK_INCOMPLETE remains distinct from transport success. */
	peers[0].pending = 5;
	venus_client_command_begin(&client, 38);
	status = venus_client_command_finish(&client, 1, 1);
	assert(status == 5);

	/* A matching final API-version store cannot excuse a malformed trailer. */
	peers[0].pending = 0;
	peers[0].bad_trailer = 1;
	venus_client_command_begin(&client, 38);
	status = venus_client_command_finish(&client, 1, 0);
	assert(status == -1);
	assert(errno == EIO);

	/* Virtqueue receipt alone must exhaust a finite budget without reading body. */
	peers[0].bad_trailer = 0;
	peers[0].no_completion = 1;
	full_replies = peers[0].full_replies;
	pause_count = 0;
	venus_client_command_begin(&client, 38);
	status = venus_client_command_finish(&client, 1, 0);
	assert(status == -1);
	assert(errno == ETIMEDOUT);
	assert(pause_count == VENUS_CLIENT_POLL_LIMIT);
	assert(peers[0].full_replies == full_replies);

	/* Closing a timed-out session consumes its descriptor through normal cleanup. */
	status = venus_client_close(&client);
	assert(status == 0);
	assert(peers[0].closes == 1);

	/* Succeeded: the client accepts only completed, structurally valid replies. */
	return;
}
