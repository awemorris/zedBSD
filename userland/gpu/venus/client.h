/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Bounded, independent Venus wire-format-1 client shared by GPU programs. */

#ifndef ZEDBSD_VENUS_CLIENT_H
#define ZEDBSD_VENUS_CLIENT_H

#include <stdint.h>
#include <uapi/gpu.h>

#define VENUS_CLIENT_STREAM_BYTES 16384U
#define VENUS_CLIENT_REPLY_BYTES 4096U
#define VENUS_CLIENT_POLL_LIMIT 1000U

/* Bootstrap objects occupy these identities for the lifetime of one session. */
enum venus_client_object {
	VENUS_OBJECT_INSTANCE = 1,
	VENUS_OBJECT_PHYSICAL_DEVICE = 2,
	VENUS_OBJECT_DEVICE = 3,
	VENUS_OBJECT_QUEUE = 4,
	VENUS_OBJECT_FIRST_APPLICATION = 5
};

/*
 * One caller-owned session with bounded single-flight command and reply storage.
 *
 * Open initializes the whole structure; the caller serializes every operation.
 * Successful open transfers the descriptor to this structure until close.
 * Vulkan initialization may fail after creating objects; close remains valid
 * and releases the session, including its reply blob and renderer objects.
 * Initialization cannot be retried on the same session after any attempt.
 * Applications use object IDs from FIRST_APPLICATION upward, finish GPU work
 * before normal object cleanup, then close the shared session. A failed close
 * still consumes its descriptor and must not be retried against that number.
 */
struct venus_client {
	int fd;
	struct gpu_info info;
	uint64_t reply_handle;
	uint32_t reply_resource;
	uint8_t stream[VENUS_CLIENT_STREAM_BYTES];
	uint32_t stream_bytes;
	uint8_t reply[VENUS_CLIENT_REPLY_BYTES];
	uint32_t reply_cursor;
	int wire_error;
	uint32_t active_command;
	uint32_t queue_family;
	uint32_t memory_count;
	uint32_t memory_flags[32];
	uint32_t wire_version;
	uint32_t xml_version;
	uint32_t capset_bytes;
	unsigned initialization_state;
};

/* Open supports ordinary 2D operations without requesting a Venus capset. */
int venus_client_open(struct venus_client *client, const char *path);
int venus_client_close(struct venus_client *client);
int venus_client_init_vulkan(struct venus_client *client);

/* Encoders and decoders retain a sticky wire_error until the next begin. */
void venus_client_wire_u32(struct venus_client *client, uint32_t word);
void venus_client_wire_u64(struct venus_client *client, uint64_t number);
void venus_client_wire_structure(struct venus_client *client, uint32_t type);
void venus_client_command_begin(struct venus_client *client, uint32_t command);
uint32_t venus_client_reply_u32(struct venus_client *client);
uint64_t venus_client_reply_u64(struct venus_client *client);

/* Finish returns 0, explicitly allowed VK_NOT_READY/VK_INCOMPLETE, or -1. */
int venus_client_command_finish(struct venus_client *client, int has_result, int allow_pending);
int venus_client_reply_handle(struct venus_client *client, uint64_t expected);
int venus_client_resource_copy(struct venus_client *client, uint64_t handle, uint64_t offset, void *buffer, uint32_t bytes, int write);
int venus_client_poll_pause(struct venus_client *client);

#endif
