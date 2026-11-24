/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The client side of the SSH file transfer protocol, version 3
 * (draft-ietf-secsh-filexfer-02), as a state machine that does no I/O.
 *
 * The caller owns the channel: it feeds received bytes with
 * sftp_client_input, writes what sftp_client_output returns and reports
 * it with sftp_client_written, queues requests while sftp_client_room is
 * nonzero, and takes each reply with sftp_client_next (and the entries
 * of an SSH_FXP_NAME reply with sftp_client_name_next) until it answers
 * EAGAIN.
 *
 * sftp_client_open queues SSH_FXP_INIT (version 3); the caller waits for
 * the SSH_FXP_VERSION reply before queueing requests.  Up to
 * SFTP_CLIENT_REQUESTS requests may be in flight at once.
 *
 * Two levels of protocol error are distinguished: a broken packet frame,
 * an unexpected message, an unknown request id, or a reply type that does
 * not match its request fails sftp_client_next with EPROTO (the caller
 * closes the channel); a well-framed reply whose fields cannot be parsed
 * (such as an unknown attribute flag, a NUL in a name, or a DATA reply
 * longer than requested) returns 0 with reply->error set to EPROTO so
 * only that request fails and the channel continues.
 */

#ifndef SSH_SFTP_CLIENT_H
#define SSH_SFTP_CLIENT_H

#include "ssh-sftp.h"

/* The most requests in flight at once (OpenSSH's sftp -R default). */
#define SFTP_CLIENT_REQUESTS	64U

/* The length of one READ or WRITE request (OpenSSH's sftp -B default). */
#define SFTP_CLIENT_CHUNK	32768U

/* The opaque state of one SFTP client session; made by sftp_client_open, freed by sftp_client_close. */
struct sftp_client;

/*
 * One reply read by sftp_client_next.
 *
 * type is SSH_FXP_VERSION, SSH_FXP_STATUS, SSH_FXP_HANDLE, SSH_FXP_DATA,
 * SSH_FXP_NAME or SSH_FXP_ATTRS; id is the request-id (0 for VERSION).
 * error is 0, or EPROTO when this reply's payload could not be parsed
 * (the request failed, while the channel continues).  Pointers (message,
 * handle, data) point into the client's internal packet buffer and stay
 * valid until the next call to sftp_client_next or sftp_client_close.
 */
struct sftp_client_reply {
	uint8_t type;
	uint32_t id;
	int error;
	uint32_t code;
	const uint8_t *message;
	size_t message_size;
	const uint8_t *handle;
	size_t handle_size;
	const uint8_t *data;
	size_t data_size;
	uint32_t count;
	struct ssh_sftp_attrs attrs;
	uint32_t version;
};

/*
 * Makes an SFTP client and queues SSH_FXP_INIT (version 3) in its
 * output.  Returns 0 or ENOMEM.
 */
int sftp_client_open(struct sftp_client **client);

/* Frees the client and its buffers; NULL is ignored. */
void sftp_client_close(struct sftp_client *client);

/*
 * Feeds up to size received bytes into the client's input buffer; *taken
 * receives how many were taken (fewer when the buffer is full).  Returns
 * 0 or ENOMEM.
 */
int sftp_client_input(struct sftp_client *client, const uint8_t *bytes, size_t size, size_t *taken);

/* Returns the bytes waiting to be written (*size of them; NULL when none). */
const uint8_t *sftp_client_output(const struct sftp_client *client, size_t *size);

/* Removes size written bytes from the output buffer. */
void sftp_client_written(struct sftp_client *client, size_t size);

/*
 * Takes the next reply into reply.  Returns 0 (a reply, possibly with
 * reply->error == EPROTO), EAGAIN (no whole packet yet), or EPROTO (the
 * stream is broken: the caller should close the channel).
 */
int sftp_client_next(struct sftp_client *client, struct sftp_client_reply *reply);

/*
 * Takes the next entry of an SSH_FXP_NAME reply last returned by
 * sftp_client_next.  Pointers remain valid until the next call to
 * sftp_client_next or sftp_client_close.  Returns 0 or EAGAIN (no more
 * entries).
 */
int sftp_client_name_next(struct sftp_client *client, const uint8_t **name, size_t *name_size, const uint8_t **long_name, size_t *long_size, struct ssh_sftp_attrs *attrs);

/* Returns how many more requests may still be queued (0 before SSH_FXP_VERSION or when SFTP_CLIENT_REQUESTS are in flight). */
unsigned sftp_client_room(const struct sftp_client *client);

/*
 * Queues SSH_FXP_OPEN (6.3) for path (size bytes, no NUL, at most
 * SSH_SFTP_PATH_MAX) with pflags and attrs (NULL for empty attrs).  *id
 * receives the request-id.  Returns 0, EAGAIN (full), EINVAL, or EMSGSIZE.
 */
int sftp_client_open_file(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t pflags, const struct ssh_sftp_attrs *attrs, uint32_t *id);

/*
 * Queues SSH_FXP_CLOSE (6.3) for handle (handle_size bytes).  Returns 0,
 * EAGAIN, EINVAL, or EMSGSIZE.
 */
int sftp_client_close_handle(struct sftp_client *client, const uint8_t *handle, size_t handle_size, uint32_t *id);

/*
 * Queues SSH_FXP_READ (6.4) for handle at offset for length bytes (1 to
 * SSH_SFTP_READ_MAX).  Returns 0, EAGAIN, EINVAL, or EMSGSIZE.
 */
int sftp_client_read(struct sftp_client *client, const uint8_t *handle, size_t handle_size, uint64_t offset, uint32_t length, uint32_t *id);

/*
 * Queues SSH_FXP_WRITE (6.4) for handle at offset with data (size bytes).
 * Returns 0, EAGAIN, EINVAL, or EMSGSIZE.
 */
int sftp_client_write(struct sftp_client *client, const uint8_t *handle, size_t handle_size, uint64_t offset, const uint8_t *data, size_t size, uint32_t *id);

/* Queues SSH_FXP_LSTAT (6.7) for path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_lstat(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t *id);

/* Queues SSH_FXP_FSTAT (6.7) for handle.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_fstat(struct sftp_client *client, const uint8_t *handle, size_t handle_size, uint32_t *id);

/* Queues SSH_FXP_SETSTAT (6.8) for path and attrs.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_setstat(struct sftp_client *client, const uint8_t *path, size_t size, const struct ssh_sftp_attrs *attrs, uint32_t *id);

/* Queues SSH_FXP_FSETSTAT (6.8) for handle and attrs.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_fsetstat(struct sftp_client *client, const uint8_t *handle, size_t handle_size, const struct ssh_sftp_attrs *attrs, uint32_t *id);

/* Queues SSH_FXP_OPENDIR (6.6) for path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_opendir(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t *id);

/* Queues SSH_FXP_READDIR (6.6) for handle.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_readdir(struct sftp_client *client, const uint8_t *handle, size_t handle_size, uint32_t *id);

/* Queues SSH_FXP_REMOVE (6.9) for path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_remove(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t *id);

/* Queues SSH_FXP_MKDIR (6.9) for path and attrs (NULL for empty).  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_mkdir(struct sftp_client *client, const uint8_t *path, size_t size, const struct ssh_sftp_attrs *attrs, uint32_t *id);

/* Queues SSH_FXP_RMDIR (6.9) for path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_rmdir(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t *id);

/* Queues SSH_FXP_REALPATH (6.11) for path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_realpath(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t *id);

/* Queues SSH_FXP_STAT (6.7) for path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_stat(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t *id);

/* Queues SSH_FXP_RENAME (6.5) from old_path to new_path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_rename(struct sftp_client *client, const uint8_t *old_path, size_t old_size, const uint8_t *new_path, size_t new_size, uint32_t *id);

/* Queues SSH_FXP_READLINK (6.10) for path.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE. */
int sftp_client_readlink(struct sftp_client *client, const uint8_t *path, size_t size, uint32_t *id);

/*
 * Queues SSH_FXP_SYMLINK (6.10) using OpenSSH's argument order
 * (OpenSSH PROTOCOL 4.1): target (link contents) first, link_path (new
 * symlink path) second.  Returns 0, EAGAIN, EINVAL, or EMSGSIZE.
 */
int sftp_client_symlink(struct sftp_client *client, const uint8_t *target, size_t target_size, const uint8_t *link_path, size_t link_size, uint32_t *id);

#endif
