/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SSH file transfer protocol, version 3 (draft-ietf-secsh-filexfer-02,
 * the revision OpenSSH's sftp, sftp-server and scp speak, OpenSSH PROTOCOL
 * preamble): the framing of its packets and the reading and writing of
 * requests, replies and file attributes, for sftp-server.  OpenSSH's scp
 * uses this protocol by default (scp(1) -O names the old one), so this is
 * what makes "scp file host:" work against these programs.
 *
 * The server side only: requests are read, replies built.  No vendor
 * extension is announced in SSH_FXP_VERSION, so a client uses only the
 * draft's requests.  SSH_FXP_EXTENDED requests are answered with
 * SSH_FX_OP_UNSUPPORTED.
 *
 * A packet (3): uint32 length, byte type, uint32 request-id (except INIT
 * and VERSION, which carry a version), then the type's fields.  Paths are
 * strings of bytes (6.2); the server passes them to the file system as
 * they are, after refusing a NUL inside.
 */

#ifndef SSH_SSH_SFTP_H
#define SSH_SSH_SFTP_H

#include "ssh-buf.h"

/* The packet types (3). */
#define SSH_FXP_INIT		1
#define SSH_FXP_VERSION		2
#define SSH_FXP_OPEN		3
#define SSH_FXP_CLOSE		4
#define SSH_FXP_READ		5
#define SSH_FXP_WRITE		6
#define SSH_FXP_LSTAT		7
#define SSH_FXP_FSTAT		8
#define SSH_FXP_SETSTAT		9
#define SSH_FXP_FSETSTAT	10
#define SSH_FXP_OPENDIR		11
#define SSH_FXP_READDIR		12
#define SSH_FXP_REMOVE		13
#define SSH_FXP_MKDIR		14
#define SSH_FXP_RMDIR		15
#define SSH_FXP_REALPATH	16
#define SSH_FXP_STAT		17
#define SSH_FXP_RENAME		18
#define SSH_FXP_READLINK	19
#define SSH_FXP_SYMLINK		20
#define SSH_FXP_STATUS		101
#define SSH_FXP_HANDLE		102
#define SSH_FXP_DATA		103
#define SSH_FXP_NAME		104
#define SSH_FXP_ATTRS		105
#define SSH_FXP_EXTENDED	200
#define SSH_FXP_EXTENDED_REPLY	201

/* The status codes (7). */
#define SSH_FX_OK			0
#define SSH_FX_EOF			1
#define SSH_FX_NO_SUCH_FILE		2
#define SSH_FX_PERMISSION_DENIED	3
#define SSH_FX_FAILURE			4
#define SSH_FX_BAD_MESSAGE		5
#define SSH_FX_NO_CONNECTION		6
#define SSH_FX_CONNECTION_LOST		7
#define SSH_FX_OP_UNSUPPORTED		8

/* The attribute flags (5). */
#define SSH_FILEXFER_ATTR_SIZE		0x00000001U
#define SSH_FILEXFER_ATTR_UIDGID	0x00000002U
#define SSH_FILEXFER_ATTR_PERMISSIONS	0x00000004U
#define SSH_FILEXFER_ATTR_ACMODTIME	0x00000008U
#define SSH_FILEXFER_ATTR_EXTENDED	0x80000000U

/* The open flags (6.3). */
#define SSH_FXF_READ		0x00000001U
#define SSH_FXF_WRITE		0x00000002U
#define SSH_FXF_APPEND		0x00000004U
#define SSH_FXF_CREAT		0x00000008U
#define SSH_FXF_TRUNC		0x00000010U
#define SSH_FXF_EXCL		0x00000020U

/* The protocol version spoken. */
#define SSH_SFTP_VERSION	3U

/* The largest packet accepted (OpenSSH's sftp-server accepts 256 KiB). */
#define SSH_SFTP_PACKET_MAX	(256U * 1024U)

/* The largest READ length served in one DATA reply. */
#define SSH_SFTP_READ_MAX	(64U * 1024U)

/* The longest path accepted. */
#define SSH_SFTP_PATH_MAX	4096U

/* The most extended attribute pairs read from an ATTRS (they are skipped). */
#define SSH_SFTP_EXTENDED_MAX	64U

/*
 * File attributes (5).  flags says which fields are present; the others
 * are 0.
 */
struct ssh_sftp_attrs {
	uint32_t flags;
	uint64_t size;
	uint32_t uid;
	uint32_t gid;
	uint32_t permissions;
	uint32_t atime;
	uint32_t mtime;
};

/*
 * One request as read.
 *
 * type and id; path and path_size, target and target_size (RENAME's new
 * path, SYMLINK's second path), handle and handle_size, data and
 * data_size (WRITE) point into the packet the caller keeps; pflags
 * (OPEN), offset and length (READ, WRITE), attrs (OPEN, SETSTAT,
 * FSETSTAT, MKDIR), version (INIT, with id unused).
 */
struct ssh_sftp_request {
	uint8_t type;
	uint32_t id;
	uint32_t version;
	const uint8_t *path;
	size_t path_size;
	const uint8_t *target;
	size_t target_size;
	const uint8_t *handle;
	size_t handle_size;
	const uint8_t *data;
	size_t data_size;
	uint32_t pflags;
	uint64_t offset;
	uint32_t length;
	struct ssh_sftp_attrs attrs;
};

/*
 * Takes one whole packet from the front of input: *packet and *size (the
 * bytes after the length field, copied into packet_out which the caller
 * reset).  A length of 0 or over SSH_SFTP_PACKET_MAX is EINVAL.  Returns
 * 0, EAGAIN or EINVAL.
 */
int ssh_sftp_next_packet(struct ssh_buf *input, struct ssh_buf *packet_out);

/*
 * Reads a request packet (type first) into request.  Malformed fields,
 * trailing bytes, a path with a NUL or longer than SSH_SFTP_PATH_MAX are
 * EINVAL; an unknown type sets request->type and request->id and returns
 * ENOTSUP (the server answers SSH_FX_OP_UNSUPPORTED).  Returns 0, EINVAL
 * or ENOTSUP.
 */
int ssh_sftp_parse_request(const uint8_t *packet, size_t size, struct ssh_sftp_request *request);

/* Appends the SSH_FXP_VERSION packet (version SSH_SFTP_VERSION, no extensions) with its length to out. */
void ssh_sftp_put_version(struct ssh_buf *out);

/*
 * Appends an SSH_FXP_STATUS packet for id with code, the message (an
 * English text for the code) and an empty language tag (7).
 */
void ssh_sftp_put_status(uint32_t id, uint32_t code, const char *message, struct ssh_buf *out);

/* Appends an SSH_FXP_HANDLE packet with size bytes of handle. */
void ssh_sftp_put_handle(uint32_t id, const uint8_t *handle, size_t size, struct ssh_buf *out);

/* Appends an SSH_FXP_DATA packet with size bytes. */
void ssh_sftp_put_data(uint32_t id, const uint8_t *data, size_t size, struct ssh_buf *out);

/* Appends an SSH_FXP_ATTRS packet. */
void ssh_sftp_put_attrs(uint32_t id, const struct ssh_sftp_attrs *attrs, struct ssh_buf *out);

/*
 * Starts an SSH_FXP_NAME packet for id with count entries (7); the
 * entries follow with ssh_sftp_put_name_entry and the length is fixed by
 * ssh_sftp_end_packet.  *start receives the packet's offset in out.
 */
void ssh_sftp_begin_name(uint32_t id, uint32_t count, struct ssh_buf *out, size_t *start);

/* Appends one name entry: filename, longname (the "ls -l" form, 7), attrs. */
void ssh_sftp_put_name_entry(const char *filename, const char *longname, const struct ssh_sftp_attrs *attrs, struct ssh_buf *out);

/* Writes the length field of the packet that starts at start in out.  Returns 0 or EMSGSIZE. */
int ssh_sftp_end_packet(struct ssh_buf *out, size_t start);

/*
 * Writes the "ls -l" long name of 7 (permissions as ten characters, link
 * count, owner, group, size, date, name) for attrs and name into out
 * (capacity bytes with the NUL; cut when longer).  owner and group are
 * names or decimal numbers.  now is the current time, for the date form
 * (time of day within six months, else the year).
 */
void ssh_sftp_long_name(const struct ssh_sftp_attrs *attrs, uint32_t links, const char *owner, const char *group, const char *name, int64_t now, char *out, size_t capacity);

#endif
