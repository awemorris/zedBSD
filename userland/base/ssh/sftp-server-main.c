/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sftp-server program: serves the SSH file transfer protocol version 3
 * on its standard input and output, as the logged-in user.
 *
 * sshd starts it for the "sftp" subsystem (with no arguments) with the
 * channel as its standard streams; OpenSSH's sftp and scp (its default
 * mode) speak to it.  It answers one request at a time, in order.  The
 * first packet must be SSH_FXP_INIT; it is answered with SSH_FXP_VERSION
 * (version 3, no extension), and a request before it ends the server.  A
 * malformed request is answered with SSH_FX_BAD_MESSAGE when its type and
 * id can be read, and the server goes on; the input ending ends the
 * server.
 *
 * Handles are 4 bytes: the big-endian index of an entry in a table of
 * SFTP_HANDLES_MAX open files and directories (an index that is out of
 * range, free, or of the other kind is SSH_FX_FAILURE).
 *
 * SSH_FXP_SYMLINK takes the link's target first and the new link's path
 * second, as OpenSSH sends them (OpenSSH PROTOCOL 4.1; ssh-sftp.h).
 * SSH_FXP_RENAME refuses to replace an existing name (draft 6.5): a file
 * is moved with link and unlink, which fails when the new name exists; a
 * name that cannot be linked (a directory) is renamed after a check that
 * the new name is free.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "ssh-crypto.h"
#include "ssh-sftp.h"

/* The most files and directories open at once. */
#define SFTP_HANDLES_MAX	64U

/* The size of a handle: a uint32 index. */
#define SFTP_HANDLE_SIZE	4U

/* The most entries one READDIR reply carries. */
#define SFTP_READDIR_COUNT	64U

/* The room for one long name. */
#define SFTP_LONG_NAME_MAX	512U

/* The room for an owner's or a group's name or number. */
#define SFTP_OWNER_MAX		64U

/* The most bytes one read of the standard input takes. */
#define SFTP_READ_CHUNK		16384U

/* The largest output kept before it is written (a DATA reply and room). */
#define SFTP_OUTPUT_MAX		(SSH_SFTP_PACKET_MAX + 4096U)

/* The kinds of a handle. */
#define SFTP_HANDLE_FREE	0
#define SFTP_HANDLE_FILE	1
#define SFTP_HANDLE_DIRECTORY	2

/*
 * One open file or directory a handle names: its kind, the descriptor of
 * a file, the stream and the path of a directory (the entries' attributes
 * are looked up below it).
 */
struct sftp_handle {
	int kind;
	int descriptor;
	DIR *directory;
	char path[SSH_SFTP_PATH_MAX + 1];
};

/*
 * The open handles of the session.  Filled by OPEN and OPENDIR, emptied
 * by CLOSE; the program is single-threaded and the table lives as long as
 * the process.
 */
static struct sftp_handle sftp_handles[SFTP_HANDLES_MAX];

static int sftp_serve(void);
static int sftp_handle_request(const uint8_t *packet, size_t size, int *initialized, struct ssh_buf *out);
static void sftp_bad_message(const uint8_t *packet, size_t size, struct ssh_buf *out);
static void sftp_put_errno(uint32_t id, int error, struct ssh_buf *out);
static void sftp_put_ok(uint32_t id, struct ssh_buf *out);
static void sftp_copy_path(const uint8_t *data, size_t size, char *path);
static struct sftp_handle *sftp_find_handle(const struct ssh_sftp_request *request, int kind);
static int sftp_new_handle(void);
static void sftp_put_new_handle(uint32_t id, unsigned index, struct ssh_buf *out);
static void sftp_attrs_of(const struct stat *status, struct ssh_sftp_attrs *attrs);
static void sftp_open(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_close(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_read(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_write(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_stat(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_setstat(const struct ssh_sftp_request *request, struct ssh_buf *out);
static int sftp_apply_attrs(const char *path, int descriptor, const struct ssh_sftp_attrs *attrs);
static void sftp_opendir(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_readdir(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_name_entry(const char *name, const struct stat *status, int have_status, struct ssh_buf *out);
static void sftp_path_call(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_rename(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_realpath(const struct ssh_sftp_request *request, struct ssh_buf *out);
static void sftp_readlink(const struct ssh_sftp_request *request, struct ssh_buf *out);
static char *sftp_realpath_new(const char *path);
static void sftp_put_one_name(uint32_t id, const char *name, struct ssh_buf *out);
static int sftp_write_all(const uint8_t *data, size_t size);

/*
 * Runs the sftp-server program.
 */
int
main(
	int argc,
	char **argv)
{
	unsigned index;
	int status;

	/* Takes no argument: sshd starts it with none. */
	(void)argv;
	if (argc > 1) {
		fputs("usage: sftp-server\n", stderr);
		return 1;
	}

	/* A write past the file size limit fails with EFBIG (a FAILURE answer) instead of ending the server. */
	(void)signal(SIGXFSZ, SIG_IGN);

	/* Starts with every handle free. */
	for (index = 0; index < SFTP_HANDLES_MAX; index++) {
		sftp_handles[index].kind = SFTP_HANDLE_FREE;
		sftp_handles[index].descriptor = -1;
		sftp_handles[index].directory = NULL;
	}

	/* Serves until the input ends, then closes what the client left open. */
	status = sftp_serve();
	for (index = 0; index < SFTP_HANDLES_MAX; index++) {
		if (sftp_handles[index].kind == SFTP_HANDLE_FILE)
			(void)close(sftp_handles[index].descriptor);
		if (sftp_handles[index].kind == SFTP_HANDLE_DIRECTORY)
			(void)closedir(sftp_handles[index].directory);
		sftp_handles[index].kind = SFTP_HANDLE_FREE;
	}

	/* Returns the failure of the loop. */
	if (status != 0)
		return 1;

	/* Succeeded: the client went. */
	return 0;
}

/*
 * Reads packets from the standard input and answers each on the
 * standard output until the input ends (0) or a framing or I/O error, or
 * a request before INIT (1).
 */
static int
sftp_serve(void)
{
	struct ssh_buf input;
	struct ssh_buf packet;
	struct ssh_buf out;
	uint8_t chunk[SFTP_READ_CHUNK];
	ssize_t got;
	int initialized;
	int taken;
	int status;
	int failed;

	/* The buffers; nothing here is secret, a file's content is the user's own. */
	ssh_buf_init(&input, (size_t)SSH_SFTP_PACKET_MAX + 4U + SFTP_READ_CHUNK, 0);
	ssh_buf_init(&packet, (size_t)SSH_SFTP_PACKET_MAX, 0);
	ssh_buf_init(&out, SFTP_OUTPUT_MAX, 0);
	initialized = 0;
	status = 0;

	/* Takes whole packets, reading more when none is whole. */
	for (;;) {
		ssh_buf_reset(&packet);
		taken = ssh_sftp_next_packet(&input, &packet);
		if (taken == 0) {
			/* Answers the packet. */
			ssh_buf_reset(&out);
			status = sftp_handle_request(packet.data, packet.size, &initialized, &out);
			if (status != 0)
				break;
			failed = ssh_buf_error(&out);
			if (failed != 0) {
				status = 1;
				break;
			}

			/* Writes the response to the client. */
			status = sftp_write_all(out.data, out.size);
			if (status != 0)
				break;
			continue;
		}

		/* Fails on any result other than a request that needs more bytes. */
		if (taken != EAGAIN) {
			/* A length the protocol does not allow: the framing is lost. */
			status = 1;
			break;
		}

		/* Reads more; the end of the input is the client going. */
		got = read(0, chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		ssh_buf_put_bytes(&input, chunk, (size_t)got);
		failed = ssh_buf_error(&input);
		if (failed != 0) {
			status = 1;
			break;
		}
	}

	/* Lets everything go. */
	ssh_buf_free(&input);
	ssh_buf_free(&packet);
	ssh_buf_free(&out);
	if (status != 0)
		return 1;

	/* Succeeded: the input ended. */
	return 0;
}

/*
 * Answers one packet into out.  Returns 0, or 1 to end the server (a
 * request before INIT).
 */
static int
sftp_handle_request(
	const uint8_t *packet,
	size_t size,
	int *initialized,
	struct ssh_buf *out)
{
	struct ssh_sftp_request request;
	int parsed;

	/* Reads the request; a malformed one is answered when its id can be told. */
	memset(&request, 0, sizeof(request));
	parsed = ssh_sftp_parse_request(packet, size, &request);
	if (parsed == EINVAL) {
		if (!*initialized)
			return 1;
		sftp_bad_message(packet, size, out);
		return 0;
	}

	/* INIT comes first, once (a second one is not answered); nothing comes before it. */
	if (request.type == SSH_FXP_INIT && parsed == 0) {
		if (!*initialized)
			ssh_sftp_put_version(out);
		*initialized = 1;
		return 0;
	}

	/* Refuses any request before the version exchange. */
	if (!*initialized)
		return 1;

	/* An unknown request, or an extension: not supported. */
	if (parsed == ENOTSUP || request.type == SSH_FXP_EXTENDED) {
		ssh_sftp_put_status(request.id, SSH_FX_OP_UNSUPPORTED, "Operation unsupported", out);
		return 0;
	}

	/* Carries the request out by its type. */
	switch (request.type) {
	case SSH_FXP_OPEN:
		sftp_open(&request, out);
		break;
	case SSH_FXP_CLOSE:
		sftp_close(&request, out);
		break;
	case SSH_FXP_READ:
		sftp_read(&request, out);
		break;
	case SSH_FXP_WRITE:
		sftp_write(&request, out);
		break;
	case SSH_FXP_LSTAT:
	case SSH_FXP_STAT:
	case SSH_FXP_FSTAT:
		sftp_stat(&request, out);
		break;
	case SSH_FXP_SETSTAT:
	case SSH_FXP_FSETSTAT:
		sftp_setstat(&request, out);
		break;
	case SSH_FXP_OPENDIR:
		sftp_opendir(&request, out);
		break;
	case SSH_FXP_READDIR:
		sftp_readdir(&request, out);
		break;
	case SSH_FXP_REMOVE:
	case SSH_FXP_MKDIR:
	case SSH_FXP_RMDIR:
	case SSH_FXP_SYMLINK:
		sftp_path_call(&request, out);
		break;
	case SSH_FXP_RENAME:
		sftp_rename(&request, out);
		break;
	case SSH_FXP_REALPATH:
		sftp_realpath(&request, out);
		break;
	case SSH_FXP_READLINK:
		sftp_readlink(&request, out);
		break;
	default:
		/* A reply type sent as a request. */
		ssh_sftp_put_status(request.id, SSH_FX_BAD_MESSAGE, "Bad message", out);
		break;
	}

	/* Succeeded: the answer is in out. */
	return 0;
}

/*
 * Answers a malformed request with SSH_FX_BAD_MESSAGE when its type and
 * id can be read (a type other than INIT and four bytes), and with
 * nothing otherwise.
 */
static void
sftp_bad_message(
	const uint8_t *packet,
	size_t size,
	struct ssh_buf *out)
{
	uint32_t id;

	/* The id follows the type byte. */
	if (size < 5U || packet[0] == SSH_FXP_INIT)
		return;
	id = ssh_load_be32(packet + 1);
	ssh_sftp_put_status(id, SSH_FX_BAD_MESSAGE, "Bad message", out);
}

/*
 * Answers a failed system call with the status of its errno (7): a name
 * that does not exist is NO_SUCH_FILE, a refusal PERMISSION_DENIED, the
 * rest FAILURE.
 */
static void
sftp_put_errno(
	uint32_t id,
	int error,
	struct ssh_buf *out)
{
	/* Picks the code by the error. */
	if (error == ENOENT || error == ENOTDIR || error == ELOOP || error == ENAMETOOLONG) {
		ssh_sftp_put_status(id, SSH_FX_NO_SUCH_FILE, "No such file", out);
	} else if (error == EACCES || error == EPERM || error == EROFS) {
		ssh_sftp_put_status(id, SSH_FX_PERMISSION_DENIED, "Permission denied", out);
	} else {
		ssh_sftp_put_status(id, SSH_FX_FAILURE, "Failure", out);
	}
}

/* Answers SSH_FX_OK. */
static void
sftp_put_ok(
	uint32_t id,
	struct ssh_buf *out)
{
	/* The success. */
	ssh_sftp_put_status(id, SSH_FX_OK, "Success", out);
}

/* Copies size bytes of a path (the parser bounds it by SSH_SFTP_PATH_MAX and refuses a NUL) into path with its NUL. */
static void
sftp_copy_path(
	const uint8_t *data,
	size_t size,
	char *path)
{
	/* The bytes, then the end. */
	if (size > SSH_SFTP_PATH_MAX)
		size = SSH_SFTP_PATH_MAX;
	if (size > 0)
		memcpy(path, data, size);
	path[size] = '\0';
}

/* Returns the open handle the request names, of kind, or NULL. */
static struct sftp_handle *
sftp_find_handle(
	const struct ssh_sftp_request *request,
	int kind)
{
	uint32_t index;

	/* A handle is the 4-byte index of a used entry of the kind. */
	if (request->handle == NULL || request->handle_size != SFTP_HANDLE_SIZE)
		return NULL;
	index = ssh_load_be32(request->handle);
	if (index >= SFTP_HANDLES_MAX)
		return NULL;
	if (sftp_handles[index].kind != kind)
		return NULL;

	/* The entry. */
	return &sftp_handles[index];
}

/* Returns the index of a free handle, or -1 when SFTP_HANDLES_MAX are open. */
static int
sftp_new_handle(void)
{
	unsigned index;

	/* The first free entry. */
	for (index = 0; index < SFTP_HANDLES_MAX; index++) {
		if (sftp_handles[index].kind == SFTP_HANDLE_FREE)
			return (int)index;
	}

	/* Every entry is in use. */
	return -1;
}

/* Answers SSH_FXP_HANDLE with the 4-byte form of index. */
static void
sftp_put_new_handle(
	uint32_t id,
	unsigned index,
	struct ssh_buf *out)
{
	uint8_t handle[SFTP_HANDLE_SIZE];

	/* The big-endian index. */
	ssh_store_be32(handle, (uint32_t)index);
	ssh_sftp_put_handle(id, handle, sizeof(handle), out);
}

/* Fills attrs from a struct stat: size, owner and group, the mode with its type bits, the two times (5). */
static void
sftp_attrs_of(
	const struct stat *status,
	struct ssh_sftp_attrs *attrs)
{
	/* Every field the draft has. */
	memset(attrs, 0, sizeof(*attrs));
	attrs->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
	attrs->size = 0;
	if (status->st_size > 0)
		attrs->size = (uint64_t)status->st_size;
	attrs->uid = (uint32_t)status->st_uid;
	attrs->gid = (uint32_t)status->st_gid;
	attrs->permissions = (uint32_t)status->st_mode;
	attrs->atime = (uint32_t)status->st_atime;
	attrs->mtime = (uint32_t)status->st_mtime;
}

/*
 * OPEN (6.3): the flags as open(2) takes them; a new file gets the
 * request's permissions (0666 without them, the umask applies); a
 * directory is refused (it is opened with OPENDIR).
 */
static void
sftp_open(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	struct stat status;
	char path[SSH_SFTP_PATH_MAX + 1];
	mode_t mode;
	int flags;
	int slot;
	int descriptor;
	int looked;
	int saved;

	/* The access and the creation flags. */
	flags = O_RDONLY;
	if ((request->pflags & SSH_FXF_READ) != 0 && (request->pflags & SSH_FXF_WRITE) != 0)
		flags = O_RDWR;
	else if ((request->pflags & SSH_FXF_WRITE) != 0)
		flags = O_WRONLY;
	if ((request->pflags & SSH_FXF_APPEND) != 0)
		flags |= O_APPEND;
	if ((request->pflags & SSH_FXF_CREAT) != 0)
		flags |= O_CREAT;
	if ((request->pflags & SSH_FXF_TRUNC) != 0)
		flags |= O_TRUNC;
	if ((request->pflags & SSH_FXF_EXCL) != 0)
		flags |= O_EXCL;
	mode = 0666;
	if ((request->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0)
		mode = (mode_t)(request->attrs.permissions & 07777U);

	/* A free handle first. */
	slot = sftp_new_handle();
	if (slot < 0) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Too many open files", out);
		return;
	}

	/* Opens the file. */
	sftp_copy_path(request->path, request->path_size, path);
	descriptor = open(path, flags | O_NOCTTY | O_CLOEXEC, mode);
	if (descriptor < 0) {
		saved = errno;
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Refuses a directory. */
	looked = fstat(descriptor, &status);
	if (looked != 0 || S_ISDIR(status.st_mode)) {
		(void)close(descriptor);
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Is a directory", out);
		return;
	}

	/* Keeps it under its handle. */
	sftp_handles[slot].kind = SFTP_HANDLE_FILE;
	sftp_handles[slot].descriptor = descriptor;
	sftp_handles[slot].directory = NULL;
	sftp_put_new_handle(request->id, (unsigned)slot, out);
}

/* CLOSE (6.3): a file or a directory. */
static void
sftp_close(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	struct sftp_handle *handle;
	int closed;
	int saved;

	/* Either kind. */
	handle = sftp_find_handle(request, SFTP_HANDLE_FILE);
	if (handle == NULL)
		handle = sftp_find_handle(request, SFTP_HANDLE_DIRECTORY);
	if (handle == NULL) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Invalid handle", out);
		return;
	}

	/* Closes it and frees the entry, whatever close says. */
	if (handle->kind == SFTP_HANDLE_FILE)
		closed = close(handle->descriptor);
	else
		closed = closedir(handle->directory);
	saved = errno;
	handle->kind = SFTP_HANDLE_FREE;
	handle->descriptor = -1;
	handle->directory = NULL;
	if (closed != 0) {
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Succeeded. */
	sftp_put_ok(request->id, out);
}

/* READ (6.4): at most SSH_SFTP_READ_MAX bytes at the offset; nothing there is EOF. */
static void
sftp_read(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	static uint8_t data[SSH_SFTP_READ_MAX];
	struct sftp_handle *handle;
	size_t length;
	ssize_t got;
	int saved;

	/* A file handle and an offset off_t holds. */
	handle = sftp_find_handle(request, SFTP_HANDLE_FILE);
	if (handle == NULL) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Invalid handle", out);
		return;
	}

	/* Reports the end of the file for an offset no file can have. */
	if (request->offset > (uint64_t)LONG_MAX) {
		ssh_sftp_put_status(request->id, SSH_FX_EOF, "End of file", out);
		return;
	}

	/* Reads what is there. */
	length = request->length;
	if (length > sizeof(data))
		length = sizeof(data);
	do {
		got = pread(handle->descriptor, data, length, (off_t)request->offset);
	} while (got < 0 && errno == EINTR);
	if (got < 0) {
		saved = errno;
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Reports the end of the file when nothing was read. */
	if (got == 0 && length > 0) {
		ssh_sftp_put_status(request->id, SSH_FX_EOF, "End of file", out);
		return;
	}

	/* The bytes. */
	ssh_sftp_put_data(request->id, data, (size_t)got, out);
}

/* WRITE (6.4): all the data at the offset (at the end for a file opened to append). */
static void
sftp_write(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	struct sftp_handle *handle;
	size_t done;
	ssize_t written;
	int saved;

	/* A file handle and an end that off_t holds. */
	handle = sftp_find_handle(request, SFTP_HANDLE_FILE);
	if (handle == NULL) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Invalid handle", out);
		return;
	}

	/* Refuses an offset that would overflow with the data. */
	if (request->offset > (uint64_t)LONG_MAX - (uint64_t)request->data_size) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Offset too large", out);
		return;
	}

	/* Writes every byte. */
	done = 0;
	while (done < request->data_size) {
		written = pwrite(handle->descriptor, request->data + done, request->data_size - done, (off_t)(request->offset + done));
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			saved = EIO;
			if (written < 0)
				saved = errno;
			sftp_put_errno(request->id, saved, out);
			return;
		}

		/* Counts the bytes that were written. */
		done += (size_t)written;
	}

	/* Succeeded. */
	sftp_put_ok(request->id, out);
}

/* LSTAT, STAT (6.8) and FSTAT: the attributes. */
static void
sftp_stat(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	struct stat status;
	struct ssh_sftp_attrs attrs;
	struct sftp_handle *handle;
	char path[SSH_SFTP_PATH_MAX + 1];
	int looked;
	int saved;

	/* The attributes of a handle's file, of a path, or of a path not followed when it is a link. */
	if (request->type == SSH_FXP_FSTAT) {
		handle = sftp_find_handle(request, SFTP_HANDLE_FILE);
		if (handle == NULL) {
			ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Invalid handle", out);
			return;
		}

		/* Reads the attributes of the open file. */
		looked = fstat(handle->descriptor, &status);
	} else {
		sftp_copy_path(request->path, request->path_size, path);
		if (request->type == SSH_FXP_LSTAT)
			looked = lstat(path, &status);
		else
			looked = stat(path, &status);
	}

	/* Reports a failure to read the attributes. */
	if (looked != 0) {
		saved = errno;
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Answers them. */
	sftp_attrs_of(&status, &attrs);
	ssh_sftp_put_attrs(request->id, &attrs, out);
}

/* SETSTAT and FSETSTAT (6.9). */
static void
sftp_setstat(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	struct sftp_handle *handle;
	char path[SSH_SFTP_PATH_MAX + 1];
	int error;

	/* A path, or a file handle. */
	if (request->type == SSH_FXP_FSETSTAT) {
		handle = sftp_find_handle(request, SFTP_HANDLE_FILE);
		if (handle == NULL) {
			ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Invalid handle", out);
			return;
		}

		/* Applies the attributes to the open file. */
		error = sftp_apply_attrs(NULL, handle->descriptor, &request->attrs);
	} else {
		sftp_copy_path(request->path, request->path_size, path);
		error = sftp_apply_attrs(path, -1, &request->attrs);
	}

	/* Reports a failure to apply the attributes. */
	if (error != 0) {
		sftp_put_errno(request->id, error, out);
		return;
	}

	/* Succeeded. */
	sftp_put_ok(request->id, out);
}

/*
 * Applies the attributes present to path (descriptor -1) or to
 * descriptor: the size, the owner and group, the permissions (07777),
 * the access and modification times.  Returns 0 or the errno of the first
 * that failed.
 */
static int
sftp_apply_attrs(
	const char *path,
	int descriptor,
	const struct ssh_sftp_attrs *attrs)
{
	struct timespec times[2];
	int changed;

	/* The size. */
	if ((attrs->flags & SSH_FILEXFER_ATTR_SIZE) != 0) {
		if (attrs->size > (uint64_t)LONG_MAX)
			return EINVAL;
		if (descriptor >= 0)
			changed = ftruncate(descriptor, (off_t)attrs->size);
		else
			changed = truncate(path, (off_t)attrs->size);
		if (changed != 0)
			return errno;
	}

	/* The owner and the group. */
	if ((attrs->flags & SSH_FILEXFER_ATTR_UIDGID) != 0) {
		if (descriptor >= 0)
			changed = fchown(descriptor, (uid_t)attrs->uid, (gid_t)attrs->gid);
		else
			changed = chown(path, (uid_t)attrs->uid, (gid_t)attrs->gid);
		if (changed != 0)
			return errno;
	}

	/* The permissions. */
	if ((attrs->flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0) {
		if (descriptor >= 0)
			changed = fchmod(descriptor, (mode_t)(attrs->permissions & 07777U));
		else
			changed = chmod(path, (mode_t)(attrs->permissions & 07777U));
		if (changed != 0)
			return errno;
	}

	/* The two times, in whole seconds. */
	if ((attrs->flags & SSH_FILEXFER_ATTR_ACMODTIME) != 0) {
		times[0].tv_sec = (time_t)attrs->atime;
		times[0].tv_nsec = 0;
		times[1].tv_sec = (time_t)attrs->mtime;
		times[1].tv_nsec = 0;
		if (descriptor >= 0)
			changed = futimens(descriptor, times);
		else
			changed = utimensat(AT_FDCWD, path, times, 0);
		if (changed != 0)
			return errno;
	}

	/* Succeeded: every attribute present was applied. */
	return 0;
}

/* OPENDIR (6.7). */
static void
sftp_opendir(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	DIR *directory;
	int slot;
	int saved;

	/* A free handle first. */
	slot = sftp_new_handle();
	if (slot < 0) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Too many open files", out);
		return;
	}

	/* Opens the directory and keeps its path for the entries' attributes. */
	sftp_copy_path(request->path, request->path_size, sftp_handles[slot].path);
	directory = opendir(sftp_handles[slot].path);
	if (directory == NULL) {
		saved = errno;
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Records the directory in the slot. */
	sftp_handles[slot].kind = SFTP_HANDLE_DIRECTORY;
	sftp_handles[slot].descriptor = -1;
	sftp_handles[slot].directory = directory;
	sftp_put_new_handle(request->id, (unsigned)slot, out);
}

/*
 * READDIR (6.7): up to SFTP_READDIR_COUNT entries with their long names
 * and attributes (an entry that cannot be looked at gets none); none left
 * is EOF.
 */
static void
sftp_readdir(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	static char entry_path[2 * SSH_SFTP_PATH_MAX + 2];
	struct sftp_handle *handle;
	struct dirent *entry;
	struct stat status;
	struct ssh_buf entries;
	size_t start;
	uint32_t count;
	int looked;
	int written;
	int failed;

	/* A directory handle. */
	handle = sftp_find_handle(request, SFTP_HANDLE_DIRECTORY);
	if (handle == NULL) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Invalid handle", out);
		return;
	}

	/* Takes the next entries. */
	ssh_buf_init(&entries, SSH_SFTP_PACKET_MAX - 1024U, 0);
	count = 0;
	while (count < SFTP_READDIR_COUNT) {
		entry = readdir(handle->directory);
		if (entry == NULL)
			break;
		written = snprintf(entry_path, sizeof(entry_path), "%s/%s", handle->path, entry->d_name);
		looked = -1;
		if (written > 0 && (size_t)written < sizeof(entry_path))
			looked = lstat(entry_path, &status);
		sftp_name_entry(entry->d_name, &status, looked == 0, &entries);
		count++;
	}

	/* None left: the end. */
	failed = ssh_buf_error(&entries);
	if (count == 0 || failed != 0) {
		ssh_buf_free(&entries);
		ssh_sftp_put_status(request->id, SSH_FX_EOF, "End of file", out);
		return;
	}

	/* The NAME reply with the entries. */
	ssh_sftp_begin_name(request->id, count, out, &start);
	ssh_buf_put_bytes(out, entries.data, entries.size);
	(void)ssh_sftp_end_packet(out, start);
	ssh_buf_free(&entries);
}

/* Appends one name entry: name, its long name and its attributes (none when have_status is 0). */
static void
sftp_name_entry(
	const char *name,
	const struct stat *status,
	int have_status,
	struct ssh_buf *out)
{
	struct ssh_sftp_attrs attrs;
	struct passwd *owner_entry;
	struct group *group_entry;
	char long_name[SFTP_LONG_NAME_MAX];
	char owner[SFTP_OWNER_MAX];
	char group[SFTP_OWNER_MAX];
	size_t length;
	time_t now;

	/* Without attributes, the long name is the name. */
	if (!have_status) {
		memset(&attrs, 0, sizeof(attrs));
		ssh_sftp_put_name_entry(name, name, &attrs, out);
		return;
	}

	/* The owner and group by name when they have one. */
	sftp_attrs_of(status, &attrs);
	sprintf(owner, "%lu", (unsigned long)status->st_uid);
	owner_entry = getpwuid(status->st_uid);
	if (owner_entry != NULL) {
		length = strlen(owner_entry->pw_name);
		if (length < sizeof(owner))
			memcpy(owner, owner_entry->pw_name, length + 1U);
	}

	/* Takes the group name, or its number when it has none. */
	sprintf(group, "%lu", (unsigned long)status->st_gid);
	group_entry = getgrgid(status->st_gid);
	if (group_entry != NULL) {
		length = strlen(group_entry->gr_name);
		if (length < sizeof(group))
			memcpy(group, group_entry->gr_name, length + 1U);
	}

	/* The "ls -l" form and the entry. */
	now = time(NULL);
	ssh_sftp_long_name(&attrs, (uint32_t)status->st_nlink, owner, group, name, (int64_t)now, long_name, sizeof(long_name));
	ssh_sftp_put_name_entry(name, long_name, &attrs, out);
}

/*
 * REMOVE (6.5), MKDIR and RMDIR (6.6), SYMLINK (OpenSSH's order: the
 * target, then the new link).  A new directory gets the request's
 * permissions (0777 without them, the umask applies).
 */
static void
sftp_path_call(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	char path[SSH_SFTP_PATH_MAX + 1];
	char target[SSH_SFTP_PATH_MAX + 1];
	mode_t mode;
	int done;
	int saved;

	/* The call of the request. */
	sftp_copy_path(request->path, request->path_size, path);
	if (request->type == SSH_FXP_REMOVE) {
		done = unlink(path);
	} else if (request->type == SSH_FXP_MKDIR) {
		mode = 0777;
		if ((request->attrs.flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0)
			mode = (mode_t)(request->attrs.permissions & 07777U);
		done = mkdir(path, mode);
	} else if (request->type == SSH_FXP_RMDIR) {
		done = rmdir(path);
	} else {
		sftp_copy_path(request->target, request->target_size, target);
		done = symlink(path, target);
	}

	/* Reports a failure to create the link. */
	if (done != 0) {
		saved = errno;
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Succeeded. */
	sftp_put_ok(request->id, out);
}

/*
 * RENAME (6.5), never over an existing name: a file by link and unlink,
 * what cannot be linked (a directory) by rename after a check.
 */
static void
sftp_rename(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	struct stat status;
	char path[SSH_SFTP_PATH_MAX + 1];
	char target[SSH_SFTP_PATH_MAX + 1];
	int done;
	int looked;
	int saved;

	/* The two names. */
	sftp_copy_path(request->path, request->path_size, path);
	sftp_copy_path(request->target, request->target_size, target);

	/* A link to the new name, which fails when the name is taken, then the old one goes. */
	done = link(path, target);
	if (done == 0) {
		done = unlink(path);
		if (done != 0) {
			saved = errno;
			sftp_put_errno(request->id, saved, out);
			return;
		}

		/* Reports success. */
		sftp_put_ok(request->id, out);
		return;
	}

	/* Refuses a name that is taken. */
	saved = errno;
	if (saved == EEXIST) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "File exists", out);
		return;
	}

	/* What cannot be linked is renamed when the new name is free. */
	looked = lstat(target, &status);
	if (looked == 0) {
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "File exists", out);
		return;
	}

	/* Renames the file to the new name. */
	done = rename(path, target);
	if (done != 0) {
		saved = errno;
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Succeeded. */
	sftp_put_ok(request->id, out);
}

/* REALPATH (6.11): the absolute form of the path ("" and "." are the working directory). */
static void
sftp_realpath(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	char path[SSH_SFTP_PATH_MAX + 1];
	char *resolved;
	size_t length;
	int saved;

	/* An empty path is the working directory. */
	sftp_copy_path(request->path, request->path_size, path);
	if (path[0] == '\0')
		strcpy(path, ".");

	/* Resolves it; a last name that is not there yet is put after its resolved directory. */
	resolved = realpath(path, NULL);
	if (resolved == NULL) {
		saved = errno;
		if (saved == ENOENT)
			resolved = sftp_realpath_new(path);
		if (resolved == NULL) {
			sftp_put_errno(request->id, saved, out);
			return;
		}
	}

	/* Refuses a path longer than the protocol allows. */
	length = strlen(resolved);
	if (length > SSH_SFTP_PATH_MAX) {
		free(resolved);
		ssh_sftp_put_status(request->id, SSH_FX_FAILURE, "Path too long", out);
		return;
	}

	/* Returns the resolved path as the only name. */
	sftp_put_one_name(request->id, resolved, out);
	free(resolved);
}

/*
 * Resolves a path whose last name does not exist yet (the name an upload
 * will create, as scp asks for): the directory part with realpath, then
 * "/" and the last name.  A last name of "", "." or "..", or a directory
 * part that does not resolve, gives NULL.  The result is malloc'd.
 */
static char *
sftp_realpath_new(
	const char *path)
{
	char directory[SSH_SFTP_PATH_MAX + 1];
	const char *slash;
	const char *base;
	char *parent;
	char *joined;
	size_t parent_size;
	size_t base_size;
	int same;

	/* Splits the last name off. */
	slash = strrchr(path, '/');
	if (slash == NULL) {
		strcpy(directory, ".");
		base = path;
	} else if (slash == path) {
		strcpy(directory, "/");
		base = slash + 1;
	} else {
		memcpy(directory, path, (size_t)(slash - path));
		directory[slash - path] = '\0';
		base = slash + 1;
	}

	/* Refuses a name of nothing or of the parent directory. */
	same = strcmp(base, "..");
	if (base[0] == '\0' || same == 0)
		return NULL;
	same = strcmp(base, ".");
	if (same == 0)
		return NULL;

	/* Resolves the directory, which must exist. */
	parent = realpath(directory, NULL);
	if (parent == NULL)
		return NULL;

	/* Joins the two ("/" and the name, without a doubled slash at the root). */
	parent_size = strlen(parent);
	base_size = strlen(base);
	joined = malloc(parent_size + 1U + base_size + 1U);
	if (joined == NULL) {
		free(parent);
		return NULL;
	}

	/* Copies the parent into the joined path. */
	memcpy(joined, parent, parent_size);
	if (parent_size == 1 && parent[0] == '/')
		parent_size = 0;
	joined[parent_size] = '/';
	memcpy(joined + parent_size + 1U, base, base_size + 1U);
	free(parent);

	/* Succeeded: the path of the name to be. */
	return joined;
}

/* READLINK (6.10). */
static void
sftp_readlink(
	const struct ssh_sftp_request *request,
	struct ssh_buf *out)
{
	char path[SSH_SFTP_PATH_MAX + 1];
	char target[SSH_SFTP_PATH_MAX + 1];
	ssize_t got;
	int saved;

	/* The link's contents. */
	sftp_copy_path(request->path, request->path_size, path);
	got = readlink(path, target, sizeof(target) - 1U);
	if (got < 0) {
		saved = errno;
		sftp_put_errno(request->id, saved, out);
		return;
	}

	/* Terminates the target and returns it as the only name. */
	target[got] = '\0';
	sftp_put_one_name(request->id, target, out);
}

/* Answers a NAME with one entry whose long name is the name and which has no attributes. */
static void
sftp_put_one_name(
	uint32_t id,
	const char *name,
	struct ssh_buf *out)
{
	struct ssh_sftp_attrs attrs;
	size_t start;

	/* The one entry. */
	memset(&attrs, 0, sizeof(attrs));
	ssh_sftp_begin_name(id, 1, out, &start);
	ssh_sftp_put_name_entry(name, name, &attrs, out);
	(void)ssh_sftp_end_packet(out, start);
}

/* Writes all size bytes to the standard output; 0 or 1. */
static int
sftp_write_all(
	const uint8_t *data,
	size_t size)
{
	size_t done;
	ssize_t written;

	/* Writes until every byte has gone. */
	done = 0;
	while (done < size) {
		written = write(1, data + done, size - done);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return 1;
		done += (size_t)written;
	}

	/* Succeeded: every byte was written. */
	return 0;
}
