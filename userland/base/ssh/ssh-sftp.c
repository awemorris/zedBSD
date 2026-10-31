/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SSH file transfer protocol version 3, the server side.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-sftp.h"

/*
 * Takes one whole packet from the front of input: *packet and *size (the
 * bytes after the length field, copied into packet_out which the caller
 * reset).
 */
int
ssh_sftp_next_packet(
	struct ssh_buf *input,
	struct ssh_buf *packet_out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)input;
	(void)packet_out;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a request packet (type first) into request.
 */
int
ssh_sftp_parse_request(
	const uint8_t *packet,
	size_t size,
	struct ssh_sftp_request *request)
{
	/* Marks the arguments used until the function is implemented. */
	(void)packet;
	(void)size;
	(void)request;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Appends the SSH_FXP_VERSION packet (version SSH_SFTP_VERSION, no
 * extensions) with its length to out.
 */
void
ssh_sftp_put_version(
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)out;
}

/*
 * Appends an SSH_FXP_STATUS packet for id with code, the message (an
 * English text for the code) and an empty language tag (7).
 */
void
ssh_sftp_put_status(
	uint32_t id,
	uint32_t code,
	const char *message,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)id;
	(void)code;
	(void)message;
	(void)out;
}

/*
 * Appends an SSH_FXP_HANDLE packet with size bytes of handle.
 */
void
ssh_sftp_put_handle(
	uint32_t id,
	const uint8_t *handle,
	size_t size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)id;
	(void)handle;
	(void)size;
	(void)out;
}

/*
 * Appends an SSH_FXP_DATA packet with size bytes.
 */
void
ssh_sftp_put_data(
	uint32_t id,
	const uint8_t *data,
	size_t size,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)id;
	(void)data;
	(void)size;
	(void)out;
}

/*
 * Appends an SSH_FXP_ATTRS packet.
 */
void
ssh_sftp_put_attrs(
	uint32_t id,
	const struct ssh_sftp_attrs *attrs,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)id;
	(void)attrs;
	(void)out;
}

/*
 * Starts an SSH_FXP_NAME packet for id with count entries (7); the
 * entries follow with ssh_sftp_put_name_entry and the length is fixed by
 * ssh_sftp_end_packet.
 */
void
ssh_sftp_begin_name(
	uint32_t id,
	uint32_t count,
	struct ssh_buf *out,
	size_t *start)
{
	/* Marks the arguments used until the function is implemented. */
	(void)id;
	(void)count;
	(void)out;
	(void)start;
}

/*
 * Appends one name entry: filename, longname (the "ls -l" form, 7),
 * attrs.
 */
void
ssh_sftp_put_name_entry(
	const char *filename,
	const char *longname,
	const struct ssh_sftp_attrs *attrs,
	struct ssh_buf *out)
{
	/* Marks the arguments used until the function is implemented. */
	(void)filename;
	(void)longname;
	(void)attrs;
	(void)out;
}

/*
 * Writes the length field of the packet that starts at start in out.
 */
int
ssh_sftp_end_packet(
	struct ssh_buf *out,
	size_t start)
{
	/* Marks the arguments used until the function is implemented. */
	(void)out;
	(void)start;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Writes the "ls -l" long name of 7 (permissions as ten characters, link
 * count, owner, group, size, date, name) for attrs and name into out
 * (capacity bytes with the NUL; cut when longer).
 */
void
ssh_sftp_long_name(
	const struct ssh_sftp_attrs *attrs,
	uint32_t links,
	const char *owner,
	const char *group,
	const char *name,
	int64_t now,
	char *out,
	size_t capacity)
{
	/* Marks the arguments used until the function is implemented. */
	(void)attrs;
	(void)links;
	(void)owner;
	(void)group;
	(void)name;
	(void)now;
	(void)out;
	(void)capacity;
}
