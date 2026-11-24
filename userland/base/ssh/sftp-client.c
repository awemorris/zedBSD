/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client side of the SSH file transfer protocol, version 3
 * (draft-ietf-secsh-filexfer-02), without I/O.
 *
 * Requests are queued into an output buffer with unique non-zero
 * request-ids tracked in an in-flight table of SFTP_CLIENT_REQUESTS
 * slots.  Replies are taken from an input buffer one packet at a time.
 */

#include <stdlib.h>
#include <string.h>

#include "sftp-client.h"

/* The capacity of the client's input buffer: two maximum-sized packets. */
#define CLIENT_INPUT_MAX	(SSH_SFTP_PACKET_MAX * 2U)

/* The capacity of the client's output buffer: enough for 64 pipelined 32 KiB writes. */
#define CLIENT_OUTPUT_MAX	(4U * 1024U * 1024U)

/* The longest handle accepted from a server (draft-ietf-secsh-filexfer-02 6.3). */
#define CLIENT_HANDLE_MAX	256U

/* The attribute flag bits defined by SFTP version 3 (5). */
#define CLIENT_ATTR_KNOWN	(SSH_FILEXFER_ATTR_SIZE | \
				 SSH_FILEXFER_ATTR_UIDGID | \
				 SSH_FILEXFER_ATTR_PERMISSIONS | \
				 SSH_FILEXFER_ATTR_ACMODTIME | \
				 SSH_FILEXFER_ATTR_EXTENDED)

/* The attribute flag bits written into requests (extended pairs are not sent). */
#define CLIENT_ATTR_WRITTEN	(SSH_FILEXFER_ATTR_SIZE | \
				 SSH_FILEXFER_ATTR_UIDGID | \
				 SSH_FILEXFER_ATTR_PERMISSIONS | \
				 SSH_FILEXFER_ATTR_ACMODTIME)

/* One request waiting for its reply from the server. */
struct sftp_inflight {
	int active;
	uint32_t id;
	uint8_t type;
	uint32_t read_length;
};

/*
 * The state of one SFTP client session.
 *
 * input holds bytes fed from the channel; packet holds the last packet
 * taken from input so reply pointers stay valid until the next
 * sftp_client_next; out holds queued outgoing packets.  name_reader and
 * name_left walk the entries of the last SSH_FXP_NAME reply.
 */
struct sftp_client {
	struct ssh_buf input;
	struct ssh_buf packet;
	struct ssh_buf out;
	struct ssh_reader name_reader;
	uint32_t name_left;
	int version_seen;
	uint32_t next_id;
	struct sftp_inflight inflight[SFTP_CLIENT_REQUESTS];
	unsigned inflight_count;
};

static int client_id_in_flight(const struct sftp_client *client, uint32_t id, unsigned *slot);
static uint32_t client_alloc_id(struct sftp_client *client);
static int client_begin_request(struct sftp_client *client, uint8_t type, uint32_t read_length, unsigned *slot, size_t *start);
static int client_finish_request(struct sftp_client *client, unsigned slot, size_t start, uint32_t *id);
static int client_check_path(const uint8_t *path, size_t size);
static int client_check_handle(const uint8_t *handle, size_t size);
static void client_put_attrs(const struct ssh_sftp_attrs *attrs, struct ssh_buf *out);
static int client_parse_attrs(struct ssh_reader *reader, struct ssh_sftp_attrs *attrs);
static int client_parse_version(struct sftp_client *client, struct ssh_reader *reader, struct sftp_client_reply *reply);
static int client_reply_matches(uint8_t request_type, uint8_t reply_type);
static void client_parse_status(struct ssh_reader *reader, struct sftp_client_reply *reply);
static void client_parse_handle(struct ssh_reader *reader, struct sftp_client_reply *reply);
static void client_parse_data(struct ssh_reader *reader, uint32_t read_length, struct sftp_client_reply *reply);
static void client_parse_name(struct sftp_client *client, struct ssh_reader *reader, struct sftp_client_reply *reply);
static void client_parse_attrs_reply(struct ssh_reader *reader, struct sftp_client_reply *reply);
static int client_queue_path_only(struct sftp_client *client, uint8_t type, const uint8_t *path, size_t size, uint32_t *id);
static int client_queue_handle_only(struct sftp_client *client, uint8_t type, const uint8_t *handle, size_t size, uint32_t *id);

/*
 * Makes an SFTP client and queues SSH_FXP_INIT (version 3) in its
 * output.
 */
int
sftp_client_open(
	struct sftp_client **client)
{
	struct sftp_client *made;
	size_t start;
	int error;

	/* Allocates and zeroes the client state. */
	*client = NULL;
	made = malloc(sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	memset(made, 0, sizeof(*made));
	made->next_id = 1U;

	/* Prepares the stream and packet buffers. */
	ssh_buf_init(&made->input, CLIENT_INPUT_MAX, 0);
	ssh_buf_init(&made->packet, SSH_SFTP_PACKET_MAX, 0);
	ssh_buf_init(&made->out, CLIENT_OUTPUT_MAX, 0);

	/* Queues SSH_FXP_INIT with version 3 (draft-ietf-secsh-filexfer-02 4). */
	start = made->out.size;
	ssh_buf_put_u32(&made->out, 0U);
	ssh_buf_put_u8(&made->out, SSH_FXP_INIT);
	ssh_buf_put_u32(&made->out, SSH_SFTP_VERSION);
	error = ssh_sftp_end_packet(&made->out, start);
	if (error != 0) {
		sftp_client_close(made);
		return error;
	}

	/* Succeeded: the client is ready for the server's VERSION. */
	*client = made;
	return 0;
}

/*
 * Frees the client and its buffers; NULL is ignored.
 */
void
sftp_client_close(
	struct sftp_client *client)
{
	/* Ignores a NULL pointer. */
	if (client == NULL)
		return;

	/* Releases the internal buffers and the client structure. */
	ssh_buf_free(&client->input);
	ssh_buf_free(&client->packet);
	ssh_buf_free(&client->out);
	free(client);
}

/*
 * Feeds up to size received bytes into the client's input buffer.
 */
int
sftp_client_input(
	struct sftp_client *client,
	const uint8_t *bytes,
	size_t size,
	size_t *taken)
{
	size_t room;
	size_t chunk;
	int error;

	/* Starts with nothing taken. */
	*taken = 0;
	if (size == 0)
		return 0;

	/* Takes as many bytes as fit in the input buffer's limit. */
	room = client->input.limit - client->input.size;
	if (room == 0)
		return 0;
	chunk = size;
	if (chunk > room)
		chunk = room;

	/* Appends the bytes to the input buffer. */
	ssh_buf_put_bytes(&client->input, bytes, chunk);
	error = ssh_buf_error(&client->input);
	if (error != 0)
		return error;

	/* Reports how many bytes were taken. */
	*taken = chunk;
	return 0;
}

/*
 * Returns the bytes waiting to be written (*size of them; NULL when
 * none).
 */
const uint8_t *
sftp_client_output(
	const struct sftp_client *client,
	size_t *size)
{
	/* Returns NULL when the output buffer is empty. */
	*size = client->out.size;
	if (client->out.size == 0)
		return NULL;

	/* Returns the buffered bytes. */
	return client->out.data;
}

/*
 * Removes size written bytes from the output buffer.
 */
void
sftp_client_written(
	struct sftp_client *client,
	size_t size)
{
	/* Consumes the written prefix from the output buffer. */
	ssh_buf_consume(&client->out, size);
}

/*
 * Takes the next reply from the input buffer into reply.
 */
int
sftp_client_next(
	struct sftp_client *client,
	struct sftp_client_reply *reply)
{
	struct ssh_reader reader;
	uint8_t type;
	uint32_t id;
	uint32_t read_length;
	unsigned slot;
	int error;

	/* Clears any leftover NAME iteration and initializes the reply. */
	client->name_left = 0;
	memset(reply, 0, sizeof(*reply));

	/* Takes one framed packet from the input stream. */
	ssh_buf_reset(&client->packet);
	error = ssh_sftp_next_packet(&client->input, &client->packet);
	if (error == EAGAIN)
		return EAGAIN;
	if (error != 0)
		return EPROTO;

	/* Reads the packet type byte. */
	ssh_reader_init(&reader, client->packet.data, client->packet.size);
	type = ssh_reader_get_u8(&reader);
	if (ssh_reader_error(&reader) != 0)
		return EPROTO;

	/* The first packet from the server must be SSH_FXP_VERSION. */
	if (!client->version_seen) {
		if (type != SSH_FXP_VERSION)
			return EPROTO;
		return client_parse_version(client, &reader, reply);
	}

	/* Rejects unexpected handshake packets after VERSION. */
	if (type == SSH_FXP_VERSION || type == SSH_FXP_INIT)
		return EPROTO;

	/* Reads the request-id and matches it against in-flight requests. */
	id = ssh_reader_get_u32(&reader);
	if (ssh_reader_error(&reader) != 0)
		return EPROTO;
	if (!client_id_in_flight(client, id, &slot))
		return EPROTO;

	/* Rejects a reply type that cannot answer the matching request. */
	if (!client_reply_matches(client->inflight[slot].type, type))
		return EPROTO;

	/* Retires the in-flight slot now that its reply arrived. */
	read_length = client->inflight[slot].read_length;
	client->inflight[slot].active = 0;
	client->inflight_count--;

	/* Populates the common reply header. */
	reply->type = type;
	reply->id = id;

	/* Parses the reply payload; a field error fails only this request. */
	switch (type) {
	case SSH_FXP_STATUS:
		client_parse_status(&reader, reply);
		break;
	case SSH_FXP_HANDLE:
		client_parse_handle(&reader, reply);
		break;
	case SSH_FXP_DATA:
		client_parse_data(&reader, read_length, reply);
		break;
	case SSH_FXP_NAME:
		client_parse_name(client, &reader, reply);
		break;
	case SSH_FXP_ATTRS:
		client_parse_attrs_reply(&reader, reply);
		break;
	default:
		return EPROTO;
	}

	/* Succeeded: reply holds the parsed reply or reply->error == EPROTO. */
	return 0;
}

/*
 * Takes the next entry of the last SSH_FXP_NAME reply.
 */
int
sftp_client_name_next(
	struct sftp_client *client,
	const uint8_t **name,
	size_t *name_size,
	const uint8_t **long_name,
	size_t *long_size,
	struct ssh_sftp_attrs *attrs)
{
	int error;

	/* Clears the output fields first. */
	*name = NULL;
	*name_size = 0;
	*long_name = NULL;
	*long_size = 0;
	memset(attrs, 0, sizeof(*attrs));

	/* Reports EAGAIN when no entries remain. */
	if (client->name_left == 0)
		return EAGAIN;

	/* Reads the validated entry from the NAME packet cursor. */
	ssh_reader_get_string(&client->name_reader, name, name_size);
	ssh_reader_get_string(&client->name_reader, long_name, long_size);
	error = client_parse_attrs(&client->name_reader, attrs);
	if (error != 0 || ssh_reader_error(&client->name_reader) != 0) {
		client->name_left = 0;
		return EAGAIN;
	}

	/* Advances to the next entry. */
	client->name_left--;
	return 0;
}

/*
 * Returns how many more requests may still be queued.
 */
unsigned
sftp_client_room(
	const struct sftp_client *client)
{
	/* No requests may be queued before SSH_FXP_VERSION arrives. */
	if (!client->version_seen)
		return 0;

	/* Returns the remaining in-flight slots. */
	return SFTP_CLIENT_REQUESTS - client->inflight_count;
}

/*
 * Queues SSH_FXP_OPEN for path with pflags and attrs.
 */
int
sftp_client_open_file(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t pflags,
	const struct ssh_sftp_attrs *attrs,
	uint32_t *id)
{
	struct ssh_sftp_attrs empty_attrs;
	unsigned slot;
	size_t start;
	int error;

	/* Validates the path and reserves an in-flight slot. */
	if (client_check_path(path, size) != 0)
		return EINVAL;
	error = client_begin_request(client, SSH_FXP_OPEN, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Uses empty attributes when the caller passed NULL. */
	if (attrs == NULL) {
		memset(&empty_attrs, 0, sizeof(empty_attrs));
		attrs = &empty_attrs;
	}

	/* Appends path, pflags and attrs after the packet header. */
	ssh_buf_put_string(&client->out, path, size);
	ssh_buf_put_u32(&client->out, pflags);
	client_put_attrs(attrs, &client->out);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/*
 * Queues SSH_FXP_CLOSE for handle.
 */
int
sftp_client_close_handle(
	struct sftp_client *client,
	const uint8_t *handle,
	size_t handle_size,
	uint32_t *id)
{
	return client_queue_handle_only(client, SSH_FXP_CLOSE, handle, handle_size, id);
}

/*
 * Queues SSH_FXP_READ for handle at offset for length bytes.
 */
int
sftp_client_read(
	struct sftp_client *client,
	const uint8_t *handle,
	size_t handle_size,
	uint64_t offset,
	uint32_t length,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates the handle and requested length. */
	if (client_check_handle(handle, handle_size) != 0)
		return EINVAL;
	if (length == 0 || length > SSH_SFTP_READ_MAX)
		return EINVAL;

	/* Reserves an in-flight slot recording the requested length. */
	error = client_begin_request(client, SSH_FXP_READ, length, &slot, &start);
	if (error != 0)
		return error;

	/* Appends handle, offset and length. */
	ssh_buf_put_string(&client->out, handle, handle_size);
	ssh_buf_put_u64(&client->out, offset);
	ssh_buf_put_u32(&client->out, length);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/*
 * Queues SSH_FXP_WRITE for handle at offset with data.
 */
int
sftp_client_write(
	struct sftp_client *client,
	const uint8_t *handle,
	size_t handle_size,
	uint64_t offset,
	const uint8_t *data,
	size_t size,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates the handle and data pointer. */
	if (client_check_handle(handle, handle_size) != 0)
		return EINVAL;
	if (size > 0 && data == NULL)
		return EINVAL;
	if (size > SSH_SFTP_PACKET_MAX)
		return EMSGSIZE;

	/* Reserves an in-flight slot. */
	error = client_begin_request(client, SSH_FXP_WRITE, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Appends handle, offset and data string. */
	ssh_buf_put_string(&client->out, handle, handle_size);
	ssh_buf_put_u64(&client->out, offset);
	ssh_buf_put_string(&client->out, data, size);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/*
 * Queues SSH_FXP_LSTAT for path.
 */
int
sftp_client_lstat(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	return client_queue_path_only(client, SSH_FXP_LSTAT, path, size, id);
}

/*
 * Queues SSH_FXP_FSTAT for handle.
 */
int
sftp_client_fstat(
	struct sftp_client *client,
	const uint8_t *handle,
	size_t handle_size,
	uint32_t *id)
{
	return client_queue_handle_only(client, SSH_FXP_FSTAT, handle, handle_size, id);
}

/*
 * Queues SSH_FXP_SETSTAT for path and attrs.
 */
int
sftp_client_setstat(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	const struct ssh_sftp_attrs *attrs,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates the path and attributes. */
	if (client_check_path(path, size) != 0 || attrs == NULL)
		return EINVAL;
	error = client_begin_request(client, SSH_FXP_SETSTAT, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Appends path and attrs. */
	ssh_buf_put_string(&client->out, path, size);
	client_put_attrs(attrs, &client->out);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/*
 * Queues SSH_FXP_FSETSTAT for handle and attrs.
 */
int
sftp_client_fsetstat(
	struct sftp_client *client,
	const uint8_t *handle,
	size_t handle_size,
	const struct ssh_sftp_attrs *attrs,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates the handle and attributes. */
	if (client_check_handle(handle, handle_size) != 0 || attrs == NULL)
		return EINVAL;
	error = client_begin_request(client, SSH_FXP_FSETSTAT, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Appends handle and attrs. */
	ssh_buf_put_string(&client->out, handle, handle_size);
	client_put_attrs(attrs, &client->out);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/*
 * Queues SSH_FXP_OPENDIR for path.
 */
int
sftp_client_opendir(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	return client_queue_path_only(client, SSH_FXP_OPENDIR, path, size, id);
}

/*
 * Queues SSH_FXP_READDIR for handle.
 */
int
sftp_client_readdir(
	struct sftp_client *client,
	const uint8_t *handle,
	size_t handle_size,
	uint32_t *id)
{
	return client_queue_handle_only(client, SSH_FXP_READDIR, handle, handle_size, id);
}

/*
 * Queues SSH_FXP_REMOVE for path.
 */
int
sftp_client_remove(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	return client_queue_path_only(client, SSH_FXP_REMOVE, path, size, id);
}

/*
 * Queues SSH_FXP_MKDIR for path and attrs.
 */
int
sftp_client_mkdir(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	const struct ssh_sftp_attrs *attrs,
	uint32_t *id)
{
	struct ssh_sftp_attrs empty_attrs;
	unsigned slot;
	size_t start;
	int error;

	/* Validates the path and reserves an in-flight slot. */
	if (client_check_path(path, size) != 0)
		return EINVAL;
	error = client_begin_request(client, SSH_FXP_MKDIR, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Uses empty attributes when the caller passed NULL. */
	if (attrs == NULL) {
		memset(&empty_attrs, 0, sizeof(empty_attrs));
		attrs = &empty_attrs;
	}

	/* Appends path and attrs. */
	ssh_buf_put_string(&client->out, path, size);
	client_put_attrs(attrs, &client->out);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/*
 * Queues SSH_FXP_RMDIR for path.
 */
int
sftp_client_rmdir(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	return client_queue_path_only(client, SSH_FXP_RMDIR, path, size, id);
}

/*
 * Queues SSH_FXP_REALPATH for path.
 */
int
sftp_client_realpath(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	return client_queue_path_only(client, SSH_FXP_REALPATH, path, size, id);
}

/*
 * Queues SSH_FXP_STAT for path.
 */
int
sftp_client_stat(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	return client_queue_path_only(client, SSH_FXP_STAT, path, size, id);
}

/*
 * Queues SSH_FXP_RENAME from old_path to new_path.
 */
int
sftp_client_rename(
	struct sftp_client *client,
	const uint8_t *old_path,
	size_t old_size,
	const uint8_t *new_path,
	size_t new_size,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates both paths and reserves an in-flight slot. */
	if (client_check_path(old_path, old_size) != 0 ||
	    client_check_path(new_path, new_size) != 0)
		return EINVAL;
	error = client_begin_request(client, SSH_FXP_RENAME, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Appends oldpath and newpath. */
	ssh_buf_put_string(&client->out, old_path, old_size);
	ssh_buf_put_string(&client->out, new_path, new_size);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/*
 * Queues SSH_FXP_READLINK for path.
 */
int
sftp_client_readlink(
	struct sftp_client *client,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	return client_queue_path_only(client, SSH_FXP_READLINK, path, size, id);
}

/*
 * Queues SSH_FXP_SYMLINK using OpenSSH's argument order (target first,
 * link_path second).
 */
int
sftp_client_symlink(
	struct sftp_client *client,
	const uint8_t *target,
	size_t target_size,
	const uint8_t *link_path,
	size_t link_size,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates both paths and reserves an in-flight slot. */
	if (client_check_path(target, target_size) != 0 ||
	    client_check_path(link_path, link_size) != 0)
		return EINVAL;
	error = client_begin_request(client, SSH_FXP_SYMLINK, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Writes target first and link_path second as OpenSSH's sftp-server expects. */
	ssh_buf_put_string(&client->out, target, target_size);
	ssh_buf_put_string(&client->out, link_path, link_size);

	/* Finalizes the packet length and commits the in-flight slot. */
	return client_finish_request(client, slot, start, id);
}

/* Reports whether id is currently in flight and sets *slot to its index. */
static int
client_id_in_flight(
	const struct sftp_client *client,
	uint32_t id,
	unsigned *slot)
{
	unsigned index;

	/* Searches the in-flight table for an active entry matching id. */
	for (index = 0; index < SFTP_CLIENT_REQUESTS; index++) {
		if (client->inflight[index].active && client->inflight[index].id == id) {
			if (slot != NULL)
				*slot = index;
			return 1;
		}
	}

	/* Not found. */
	return 0;
}

/* Picks the next non-zero request-id not currently in flight. */
static uint32_t
client_alloc_id(
	struct sftp_client *client)
{
	uint32_t candidate;

	/* Advances next_id, skipping 0 and any id still in flight. */
	for (;;) {
		candidate = client->next_id;
		client->next_id++;
		if (candidate == 0)
			continue;
		if (!client_id_in_flight(client, candidate, NULL))
			return candidate;
	}
}

/* Finds a free in-flight slot and writes the packet header into client->out. */
static int
client_begin_request(
	struct sftp_client *client,
	uint8_t type,
	uint32_t read_length,
	unsigned *slot,
	size_t *start)
{
	unsigned index;
	uint32_t req_id;

	/* Refuses requests before SSH_FXP_VERSION or when the window is full. */
	if (!client->version_seen)
		return EINVAL;
	if (client->inflight_count >= SFTP_CLIENT_REQUESTS)
		return EAGAIN;

	/* Finds an inactive slot in the in-flight table. */
	for (index = 0; index < SFTP_CLIENT_REQUESTS; index++) {
		if (!client->inflight[index].active)
			break;
	}
	if (index >= SFTP_CLIENT_REQUESTS)
		return EAGAIN;

	/* Allocates a request-id and writes the packet length placeholder, type and id. */
	req_id = client_alloc_id(client);
	*start = client->out.size;
	ssh_buf_put_u32(&client->out, 0U);
	ssh_buf_put_u8(&client->out, type);
	ssh_buf_put_u32(&client->out, req_id);

	/* Stores the tentative slot metadata (activated in client_finish_request). */
	client->inflight[index].id = req_id;
	client->inflight[index].type = type;
	client->inflight[index].read_length = read_length;
	*slot = index;
	return 0;
}

/* Finalizes the packet length and activates the in-flight slot on success. */
static int
client_finish_request(
	struct sftp_client *client,
	unsigned slot,
	size_t start,
	uint32_t *id)
{
	int error;

	/* Patches the packet length; rolls back out on overflow or allocation failure. */
	error = ssh_sftp_end_packet(&client->out, start);
	if (error != 0) {
		client->out.size = start;
		client->out.error = 0;
		return error;
	}

	/* Activates the in-flight slot and reports the assigned id. */
	client->inflight[slot].active = 1;
	client->inflight_count++;
	if (id != NULL)
		*id = client->inflight[slot].id;
	return 0;
}

/* Validates a path argument: at most SSH_SFTP_PATH_MAX bytes and no NUL inside. */
static int
client_check_path(
	const uint8_t *path,
	size_t size)
{
	/* Rejects a NULL pointer for a non-empty path or an overlong path. */
	if (size > 0 && path == NULL)
		return EINVAL;
	if (size > SSH_SFTP_PATH_MAX)
		return EINVAL;

	/* Rejects an embedded NUL byte. */
	if (size > 0 && memchr(path, '\0', size) != NULL)
		return EINVAL;

	/* Valid path. */
	return 0;
}

/* Validates a handle argument: 1 to CLIENT_HANDLE_MAX bytes. */
static int
client_check_handle(
	const uint8_t *handle,
	size_t size)
{
	/* Rejects empty, NULL, or overlong handles. */
	if (handle == NULL || size == 0 || size > CLIENT_HANDLE_MAX)
		return EINVAL;

	/* Valid handle. */
	return 0;
}

/* Appends encoded file attributes to out (version 3, no extended pairs). */
static void
client_put_attrs(
	const struct ssh_sftp_attrs *attrs,
	struct ssh_buf *out)
{
	uint32_t flags;

	/* Writes only the flags whose fields are defined in version 3. */
	flags = attrs->flags & CLIENT_ATTR_WRITTEN;
	ssh_buf_put_u32(out, flags);

	/* Appends size, uid/gid, permissions and timestamps in draft order. */
	if ((flags & SSH_FILEXFER_ATTR_SIZE) != 0)
		ssh_buf_put_u64(out, attrs->size);
	if ((flags & SSH_FILEXFER_ATTR_UIDGID) != 0) {
		ssh_buf_put_u32(out, attrs->uid);
		ssh_buf_put_u32(out, attrs->gid);
	}
	if ((flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0)
		ssh_buf_put_u32(out, attrs->permissions);
	if ((flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0) {
		ssh_buf_put_u32(out, attrs->atime);
		ssh_buf_put_u32(out, attrs->mtime);
	}
}

/* Reads file attributes from reader, skipping up to SSH_SFTP_EXTENDED_MAX extended pairs. */
static int
client_parse_attrs(
	struct ssh_reader *reader,
	struct ssh_sftp_attrs *attrs)
{
	const uint8_t *skipped;
	size_t skipped_size;
	uint32_t flags;
	uint32_t count;
	uint32_t index;
	int error;

	/* Clears the output structure. */
	memset(attrs, 0, sizeof(*attrs));

	/* Reads the flags and rejects bits outside version 3. */
	flags = ssh_reader_get_u32(reader);
	error = ssh_reader_error(reader);
	if (error != 0)
		return EINVAL;
	if ((flags & ~CLIENT_ATTR_KNOWN) != 0)
		return EINVAL;

	/* Reads each present field in draft order. */
	if ((flags & SSH_FILEXFER_ATTR_SIZE) != 0)
		attrs->size = ssh_reader_get_u64(reader);
	if ((flags & SSH_FILEXFER_ATTR_UIDGID) != 0) {
		attrs->uid = ssh_reader_get_u32(reader);
		attrs->gid = ssh_reader_get_u32(reader);
	}
	if ((flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0)
		attrs->permissions = ssh_reader_get_u32(reader);
	if ((flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0) {
		attrs->atime = ssh_reader_get_u32(reader);
		attrs->mtime = ssh_reader_get_u32(reader);
	}

	/* Skips any extended attribute pairs up to SSH_SFTP_EXTENDED_MAX. */
	error = ssh_reader_error(reader);
	if (error == 0 && (flags & SSH_FILEXFER_ATTR_EXTENDED) != 0) {
		count = ssh_reader_get_u32(reader);
		error = ssh_reader_error(reader);
		if (error == 0 && count > SSH_SFTP_EXTENDED_MAX)
			error = EINVAL;
		for (index = 0; error == 0 && index < count; index++) {
			ssh_reader_get_string(reader, &skipped, &skipped_size);
			ssh_reader_get_string(reader, &skipped, &skipped_size);
			error = ssh_reader_error(reader);
		}
	}

	/* Clears attrs on error. */
	if (error != 0) {
		memset(attrs, 0, sizeof(*attrs));
		return EINVAL;
	}

	/* Records the kept flags without the skipped extended bit. */
	attrs->flags = flags & ~SSH_FILEXFER_ATTR_EXTENDED;
	return 0;
}

/* Parses SSH_FXP_VERSION and skips any server extension pairs. */
static int
client_parse_version(
	struct sftp_client *client,
	struct ssh_reader *reader,
	struct sftp_client_reply *reply)
{
	const uint8_t *ext_name;
	size_t ext_name_size;
	const uint8_t *ext_data;
	size_t ext_data_size;
	uint32_t version;
	unsigned pairs;

	/* Reads the server's version and refuses anything below version 3. */
	version = ssh_reader_get_u32(reader);
	if (ssh_reader_error(reader) != 0 || version < SSH_SFTP_VERSION)
		return EPROTO;

	/* Skips any extension name/data pairs (capped at SSH_SFTP_EXTENDED_MAX). */
	pairs = 0;
	while (ssh_reader_left(reader) > 0) {
		if (pairs >= SSH_SFTP_EXTENDED_MAX)
			return EPROTO;
		ssh_reader_get_string(reader, &ext_name, &ext_name_size);
		ssh_reader_get_string(reader, &ext_data, &ext_data_size);
		if (ssh_reader_error(reader) != 0)
			return EPROTO;
		pairs++;
	}

	/* Treats any version above 3 as version 3. */
	if (version > SSH_SFTP_VERSION)
		version = SSH_SFTP_VERSION;

	/* Marks the handshake complete and returns the VERSION reply. */
	client->version_seen = 1;
	reply->type = SSH_FXP_VERSION;
	reply->version = version;
	return 0;
}

/* Reports whether reply_type is a valid response for request_type. */
static int
client_reply_matches(
	uint8_t request_type,
	uint8_t reply_type)
{
	/* SSH_FXP_STATUS is valid for every request. */
	if (reply_type == SSH_FXP_STATUS)
		return 1;

	/* Checks the request-specific data-bearing reply type. */
	switch (request_type) {
	case SSH_FXP_OPEN:
	case SSH_FXP_OPENDIR:
		return reply_type == SSH_FXP_HANDLE;
	case SSH_FXP_READ:
		return reply_type == SSH_FXP_DATA;
	case SSH_FXP_READDIR:
	case SSH_FXP_REALPATH:
	case SSH_FXP_READLINK:
		return reply_type == SSH_FXP_NAME;
	case SSH_FXP_LSTAT:
	case SSH_FXP_FSTAT:
	case SSH_FXP_STAT:
		return reply_type == SSH_FXP_ATTRS;
	default:
		return 0;
	}
}

/* Parses an SSH_FXP_STATUS payload into reply. */
static void
client_parse_status(
	struct ssh_reader *reader,
	struct sftp_client_reply *reply)
{
	const uint8_t *lang;
	size_t lang_size;

	/* Reads code, message and language tag, rejecting trailing bytes. */
	reply->code = ssh_reader_get_u32(reader);
	ssh_reader_get_string(reader, &reply->message, &reply->message_size);
	ssh_reader_get_string(reader, &lang, &lang_size);
	if (ssh_reader_finish(reader) != 0) {
		reply->error = EPROTO;
		reply->message = NULL;
		reply->message_size = 0;
	}
}

/* Parses an SSH_FXP_HANDLE payload into reply. */
static void
client_parse_handle(
	struct ssh_reader *reader,
	struct sftp_client_reply *reply)
{
	/* Reads the handle and checks its bounds and exact packet end. */
	ssh_reader_get_string(reader, &reply->handle, &reply->handle_size);
	if (ssh_reader_finish(reader) != 0 ||
	    reply->handle_size == 0 ||
	    reply->handle_size > CLIENT_HANDLE_MAX) {
		reply->error = EPROTO;
		reply->handle = NULL;
		reply->handle_size = 0;
	}
}

/* Parses an SSH_FXP_DATA payload into reply, checking against read_length. */
static void
client_parse_data(
	struct ssh_reader *reader,
	uint32_t read_length,
	struct sftp_client_reply *reply)
{
	/* Reads the data string and rejects replies longer than requested. */
	ssh_reader_get_string(reader, &reply->data, &reply->data_size);
	if (ssh_reader_finish(reader) != 0 || reply->data_size > read_length) {
		reply->error = EPROTO;
		reply->data = NULL;
		reply->data_size = 0;
	}
}

/* Validates an SSH_FXP_NAME payload and sets up sftp_client_name_next. */
static void
client_parse_name(
	struct sftp_client *client,
	struct ssh_reader *reader,
	struct sftp_client_reply *reply)
{
	struct ssh_sftp_attrs attrs;
	const uint8_t *name;
	size_t name_size;
	const uint8_t *long_name;
	size_t long_size;
	uint32_t count;
	uint32_t index;
	size_t entries_offset;

	/* Reads the entry count and remembers where the entries start. */
	count = ssh_reader_get_u32(reader);
	if (ssh_reader_error(reader) != 0) {
		reply->error = EPROTO;
		return;
	}
	entries_offset = reader->offset;

	/* Validates every entry upfront so sftp_client_name_next cannot fail mid-reply. */
	for (index = 0; index < count; index++) {
		ssh_reader_get_string(reader, &name, &name_size);
		ssh_reader_get_string(reader, &long_name, &long_size);
		if (ssh_reader_error(reader) != 0) {
			reply->error = EPROTO;
			return;
		}
		if (name_size > SSH_SFTP_PATH_MAX) {
			reply->error = EPROTO;
			return;
		}
		if (name_size > 0 && memchr(name, '\0', name_size) != NULL) {
			reply->error = EPROTO;
			return;
		}
		if (long_size > 0 && memchr(long_name, '\0', long_size) != NULL) {
			reply->error = EPROTO;
			return;
		}
		if (client_parse_attrs(reader, &attrs) != 0) {
			reply->error = EPROTO;
			return;
		}
	}

	/* Rejects trailing bytes after the last entry. */
	if (ssh_reader_finish(reader) != 0) {
		reply->error = EPROTO;
		return;
	}

	/* Sets up the cursor for sftp_client_name_next. */
	reply->count = count;
	ssh_reader_init(&client->name_reader,
	    client->packet.data + entries_offset,
	    client->packet.size - entries_offset);
	client->name_left = count;
}

/* Parses an SSH_FXP_ATTRS payload into reply. */
static void
client_parse_attrs_reply(
	struct ssh_reader *reader,
	struct sftp_client_reply *reply)
{
	/* Reads the attributes and rejects trailing bytes. */
	if (client_parse_attrs(reader, &reply->attrs) != 0 ||
	    ssh_reader_finish(reader) != 0) {
		reply->error = EPROTO;
		memset(&reply->attrs, 0, sizeof(reply->attrs));
	}
}

/* Queues a request whose only payload after id is a single path string. */
static int
client_queue_path_only(
	struct sftp_client *client,
	uint8_t type,
	const uint8_t *path,
	size_t size,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates the path and reserves an in-flight slot. */
	if (client_check_path(path, size) != 0)
		return EINVAL;
	error = client_begin_request(client, type, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Appends the path and finalizes the packet. */
	ssh_buf_put_string(&client->out, path, size);
	return client_finish_request(client, slot, start, id);
}

/* Queues a request whose only payload after id is a single handle string. */
static int
client_queue_handle_only(
	struct sftp_client *client,
	uint8_t type,
	const uint8_t *handle,
	size_t size,
	uint32_t *id)
{
	unsigned slot;
	size_t start;
	int error;

	/* Validates the handle and reserves an in-flight slot. */
	if (client_check_handle(handle, size) != 0)
		return EINVAL;
	error = client_begin_request(client, type, 0U, &slot, &start);
	if (error != 0)
		return error;

	/* Appends the handle and finalizes the packet. */
	ssh_buf_put_string(&client->out, handle, size);
	return client_finish_request(client, slot, start, id);
}
