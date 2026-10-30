/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The writing requests of install-admin's session (session.h).  The disk
 * is checked again before each write against what the screen showed: its
 * registration, name, size and the digest of its partition table, and
 * each partition by its disk, first sector, size and PARTUUID.  The
 * tables are written by diskpart and the filesystems by mkfs and mkswap,
 * each run as root with an empty environment and given the confirmation
 * phrase made from the kernel's description of the device just before.
 * Every mount of the disk is undone before each of them.  The units are
 * unpacked into the new root mounted without set-ID, the settings
 * written, the root unmounted, and only then are the loader and
 * zedbsd.cfg put on the ESP, so that a system half installed never looks
 * like one that starts.
 */

#include "session.h"

#include "probe.h"
#include "target.h"
#include "unpack.h"

#include "userland/base/diskpart/table.h"
#include "include/kern/os-name.h"

#include <uapi/gpt-type.h>
#include <uapi/mountinfo.h>
#include <uapi/sysctl.h>
#include <uapi/system.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* The programs the session runs. */
#define SESSION_DISKPART	"/sbin/diskpart"
#define SESSION_MKFS		"/sbin/mkfs"
#define SESSION_MKSWAP		"/sbin/mkswap"
#define SESSION_SWAPON		"/sbin/swapon"
#define SESSION_SWAPOFF		"/sbin/swapoff"
#define SESSION_EFIBOOT		"/sbin/efiboot"

/* Where the new root and the ESP are mounted while they are written. */
#define SESSION_TARGET		"/run/install-admin/target"
#define SESSION_ESP		"/run/install-admin/esp"

/* The payload of the installer's image. */
#define SESSION_PAYLOAD		"/opt/installer"
#define SESSION_UNITS		SESSION_PAYLOAD "/units"
#define SESSION_BOOT		SESSION_PAYLOAD "/boot"

/* The installed system's record of its units, from its root. */
#define SESSION_RECORDS		"var/db/installer"

/* The swap file of a system without a swap partition, from its root and as the live system sees it. */
#define SESSION_SWAPFILE	"swapfile"
#define SESSION_SWAPFILE_PATH	SESSION_TARGET "/" SESSION_SWAPFILE

/* The loader's place on an ESP that has another system's loader at the firmware's default. */
#define SESSION_LOADER_DEFAULT	"EFI/BOOT"
#define SESSION_LOADER_OWN	"EFI/ZEDBSD"

/* The configuration's name on the ESP, and the names files are written under before their rename. */
#define SESSION_CFG		"zedbsd.cfg"
#define SESSION_CFG_NEW		"zedbsd.new"
#define SESSION_NEW_SUFFIX	".new"

/* The names of the partitions an installation makes. */
#define SESSION_ESP_NAME	"EFI System"
#define SESSION_ROOT_NAME	"zedBSD installed root"
#define SESSION_SWAP_NAME	"zedBSD installed swap"

/* The bytes cleared at both ends of a partition before it is formatted. */
#define SESSION_CLEAR_BYTES	(1024U * 1024U)

/* The largest swap file mkswap makes, and the room an ESP keeps besides what is put on it. */
#define SESSION_SWAPFILE_MAX	((2ULL << 30) - 4096ULL)
#define SESSION_ESP_SPARE	(16ULL * 1024ULL * 1024ULL)

/* The room the root keeps besides what is installed on it. */
#define SESSION_ROOT_SPARE	(1ULL << 30)

/* The seconds a trial swapon or swapoff of the new swap file may take. */
#define SESSION_SWAP_SECONDS	30U

/* The time efiboot may take to write the boot entry; the firmware writes its flash meanwhile. */
#define SESSION_EFIBOOT_SECONDS	60U

/* The bytes unpacked between two BYTES lines, and the entries between two looks for a cancellation. */
#define SESSION_REPORT_BYTES	(8ULL * 1024ULL * 1024ULL)
#define SESSION_CANCEL_ENTRIES	256U

/* The times diskpart, mkfs and mkswap are tried when a partition was busy for a moment. */
#define SESSION_TRIES		3U

/* The steps of an installation. */
#define SESSION_STEPS		10U

/* The most arguments a program is run with, and the longest output read from one. */
#define SESSION_ARGUMENTS_MAX	32U
#define SESSION_CAPTURE_MAX	512U

/* The longest path the session makes of a device or a file. */
#define SESSION_PATH_MAX	256U

/* What one look into the disk of a request found. */
struct session_disk {
	struct probe_scan *scan;
	const struct probe_device *device;
	struct dp_table table;
	int loaded;
	uint8_t digest[32];
};

/* An installation as it goes: what it found, what it made and how far it is. */
struct session_installation {
	struct admin_session *session;
	struct plan_request *request;
	struct plan_tuple esp;
	struct plan_tuple root;
	struct plan_tuple swap;
	unsigned esp_slot;
	unsigned root_slot;
	unsigned swap_slot;
	int has_swap_partition;
	int new_esp;
	char disk_uuid[PLAN_GUID_TEXT + 1U];
	unsigned order[INDEX_UNITS_MAX];
	unsigned order_count;
	uint64_t total_bytes;
	uint64_t total_entries;
	uint64_t done_bytes;
	uint64_t reported_bytes;
	unsigned entries_since_look;
	uint64_t swapfile_bytes;
	int swapfile_made;
	int graphical;
	int root_mounted;
	int root_fd;
	int esp_mounted;
	FILE *files;
	int loader_own;
	char loader_place[SESSION_PATH_MAX];
};

static void session_say(struct admin_session *session, const char *format, ...);
static void session_fail(struct admin_session *session, const char *step, int error, const char *text);
static void session_step(struct admin_session *session, unsigned step, const char *name);
static int session_look_for_cancel(struct admin_session *session);
static int session_run(char *const argument[], const char *answer, char *capture, size_t capacity, unsigned seconds, int *status);
static int session_wait(pid_t child, unsigned seconds, int *status);
static int session_random_guid(char text[PLAN_GUID_TEXT + 1U]);
static int session_bind(struct admin_session *session, const struct plan_disk *disk);
static int session_disk_load(const struct plan_disk *disk, struct session_disk *found);
static void session_disk_free(struct session_disk *found);
static int session_unmount_disk(uint32_t registration);
static const struct dp_part *session_find_entry(const struct dp_table *table, const struct plan_tuple *tuple);
static int session_free_slot(const struct dp_table *table, unsigned after, unsigned *slot);
static int session_overlaps(const struct dp_table *table, uint64_t start, uint64_t sectors);
static int session_has_esp(const struct dp_table *table);
static int session_partition_ready(const struct plan_tuple *tuple, struct kern_block_info *info);
static int session_kernel_matches(const struct probe_scan *scan, const struct probe_device *disk, const struct dp_table *table);
static int session_clear_ends(const struct kern_block_info *info);
static int session_format(const struct plan_tuple *tuple, const char *program, const char *kind);
static int session_diskpart(const struct plan_disk *disk, char *const operation[], unsigned count, int *reload_failed);
static int session_edit_delete(struct admin_session *session, const struct plan_request *request, struct session_disk *found);
static int session_edit_create(struct admin_session *session, const struct plan_request *request, struct session_disk *found);
static int session_payload_directory(const char *path);
static int session_payload_open(const char *directory, const char *name, int *fd);
static int session_memory_bytes(uint64_t *bytes);
static int session_root_capacity(uint32_t sector_bytes, uint64_t bytes, uint64_t *inodes);
static int session_open_directory(int base, const char *path, int create, int *fd);
static int session_write_new(int directory, const char *leaf, const uint8_t *data, size_t length, mode_t mode);
static int session_copy_boot(const struct index_boot *boot, int directory, const char *leaf);
static int session_esp_look(struct session_installation *installation, const struct kern_block_info *info);
static int session_install_check(struct session_installation *installation);
static int session_install_whole_layout(struct session_installation *installation);
static int session_install_chosen_layout(struct session_installation *installation, const struct session_disk *found);
static int session_install_size(struct session_installation *installation);
static int session_install_claim(struct session_installation *installation);
static int session_install_table(struct session_installation *installation);
static int session_install_format(struct session_installation *installation);
static int session_install_mount(struct session_installation *installation);
static int session_install_unpack(struct session_installation *installation);
static int session_entry_done(void *context, const char *path, enum unpack_kind kind, uint64_t bytes);
static int session_unpack_unit(struct session_installation *installation, unsigned unit);
static int session_install_records(struct session_installation *installation);
static int session_install_boot_files(struct session_installation *installation);
static int session_install_settings(struct session_installation *installation);
static int session_install_swapfile(struct session_installation *installation);
static int session_install_release_root(struct session_installation *installation);
static int session_install_esp(struct session_installation *installation);
static int session_install_release_esp(struct session_installation *installation);
static void session_install_boot_entry(struct session_installation *installation);
static void session_install_cleanup(struct session_installation *installation);

/*
 * Carries out one change of a partition table: a delete, a
 * create, or a create with an ESP before it.  Reports DONE with the new
 * table's digest and the partitions made, or FAIL.  Returns 0 or the
 * errno value of the failure.
 */
int
session_edit(
	struct admin_session *session,
	const struct plan_request *request)
{
	struct session_disk found;
	int error;

	/* The request as it was read, and the disk the session is bound to. */
	error = plan_edit_check(request);
	if (error != 0) {
		session_fail(session, "edit", error, "the change does not hold together");
		return error;
	}

	/* Binds the session to the request's disk, refusing another one. */
	error = session_bind(session, &request->disk);
	if (error != 0) {
		session_fail(session, "edit", error, "another disk than the session's");
		return error;
	}

	/* The disk as it is now, as the screen showed it. */
	error = session_disk_load(&request->disk, &found);
	if (error != 0) {
		session_disk_free(&found);
		session_fail(session, "edit", error, "the disk changed");
		return error;
	}

	/* The change. */
	if (request->verb == PLAN_EDIT_DELETE)
		error = session_edit_delete(session, request, &found);
	else
		error = session_edit_create(session, request, &found);
	session_disk_free(&found);
	if (error != 0)
		return error;

	/* The table as it is now. */
	error = session_disk_load(&request->disk, &found);
	if (error == ESTALE) {
		/* Changed, as it was to be: the new digest is the one to quote. */
		error = 0;
	}

	/* Reports a table that cannot be read after the change. */
	if (error != 0) {
		session_fail(session, "edit", error, "the table cannot be read again");
		return error;
	}

	/* Reports the digest of the new table, which the installer quotes next. */
	session_say(
		session,
		"STATE table %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
		"%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
		found.digest[0], found.digest[1], found.digest[2], found.digest[3],
		found.digest[4], found.digest[5], found.digest[6], found.digest[7],
		found.digest[8], found.digest[9], found.digest[10], found.digest[11],
		found.digest[12], found.digest[13], found.digest[14], found.digest[15],
		found.digest[16], found.digest[17], found.digest[18], found.digest[19],
		found.digest[20], found.digest[21], found.digest[22], found.digest[23],
		found.digest[24], found.digest[25], found.digest[26], found.digest[27],
		found.digest[28], found.digest[29], found.digest[30], found.digest[31]);
	session_disk_free(&found);
	session_say(session, "DONE\n");

	/* Succeeded: the table has the change. */
	return 0;
}

/*
 * Carries out an installation in its steps, each reported as
 * it begins; a failure or a cancellation stops it at that step with FAIL
 * and leaves what was written (nothing is rolled back).  Returns 0 or the
 * errno value of the failure.
 */
int
session_install(
	struct admin_session *session,
	struct plan_request *request)
{
	struct session_installation *installation;
	const char *step;
	int error;

	/* The installation's state, too large for the stack. */
	installation = calloc(1, sizeof(*installation));
	if (installation == NULL) {
		session_fail(session, "check", ENOMEM, "out of memory");
		return ENOMEM;
	}

	/* Hands the installation its session, its request and no root descriptor yet. */
	installation->session = session;
	installation->request = request;
	installation->root_fd = -1;

	/* Each step in its order; the first failure ends the walk. */
	step = "check";
	session_step(session, 1, step);
	error = session_install_check(installation);
	if (error == 0) {
		step = "claim";
		session_step(session, 2, step);
		error = session_install_claim(installation);
	}

	/* Runs the claim step once the check has passed. */
	if (error == 0) {
		step = "table";
		session_step(session, 3, step);
		error = session_install_table(installation);
	}

	/* Runs the table step once the claim has been made. */
	if (error == 0) {
		step = "format";
		session_step(session, 4, step);
		error = session_install_format(installation);
	}

	/* Runs the format step once the table is written. */
	if (error == 0) {
		step = "mount";
		session_step(session, 5, step);
		error = session_install_mount(installation);
	}

	/* Runs the mount step once the filesystems are made. */
	if (error == 0) {
		step = "unpack";
		session_step(session, 6, step);
		error = session_install_unpack(installation);
	}

	/* Runs the unpack step once the new root is mounted. */
	if (error == 0) {
		step = "settings";
		session_step(session, 7, step);
		error = session_install_settings(installation);
	}

	/* Runs the settings step once the units are unpacked. */
	if (error == 0) {
		step = "sync";
		session_step(session, 8, step);
		error = session_install_release_root(installation);
	}

	/* Runs the sync step once the settings are written. */
	if (error == 0) {
		step = "esp";
		session_step(session, 9, step);
		error = session_install_esp(installation);
	}

	/* Runs the ESP step once the root has been released. */
	if (error == 0) {
		step = "finish";
		session_step(session, 10, step);
		error = session_install_release_esp(installation);
	}

	/* A failure: nothing is left mounted, and the installer is told where it stopped. */
	if (error != 0) {
		session_install_cleanup(installation);
		session_fail(session, step, error, strerror(error));
		free(installation);
		return error;
	}

	/* The firmware's boot entry for the loader; the installation stands without one. */
	session_install_boot_entry(installation);

	/* The loader's place, for the last page, and the end. */
	session_say(session, "STATE loader %s\n", installation->loader_place);
	if (installation->has_swap_partition)
		session_say(session, "STATE swap partition\n");
	else if (installation->swapfile_made)
		session_say(session, "STATE swap file\n");
	else
		session_say(session, "STATE swap none\n");
	session_say(session, "DONE\n");
	free(installation);

	/* Succeeded: the disk holds a system that starts. */
	return 0;
}

/* Writes one line of the protocol, at once. */
static void
session_say(
	struct admin_session *session,
	const char *format,
	...)
{
	va_list arguments;

	/* The line, then the flush: the installer reads it as it comes. */
	va_start(arguments, format);
	(void)vfprintf(session->output, format, arguments);
	va_end(arguments);
	(void)fflush(session->output);
}

/* Reports a failure at a step, and logs it. */
static void
session_fail(
	struct admin_session *session,
	const char *step,
	int error,
	const char *text)
{
	/* The line the installer reads, then the log. */
	session_say(session, "FAIL %s %d %s\n", step, error, text);
	syslog(LOG_NOTICE, "failed at %s: %s (%d)", step, text, error);
}

/* Reports the step begun. */
static void
session_step(
	struct admin_session *session,
	unsigned step,
	const char *name)
{
	/* One line a step. */
	session_say(session, "STEP %u %u %s\n", step, SESSION_STEPS, name);
}

/*
 * Looks, without waiting, for a cancellation on the input: a line
 * "cancel", or its end (the installer gone).  Reports 1 once one came;
 * the session keeps it.
 */
static int
session_look_for_cancel(
	struct admin_session *session)
{
	struct pollfd ready;
	char text[256];
	const char *cancel;
	ssize_t count;
	int found;

	/* Cancelled before. */
	if (session->canceled)
		return 1;

	/* Anything to read now. */
	memset(&ready, 0, sizeof(ready));
	ready.fd = session->input;
	ready.events = POLLIN;
	found = poll(&ready, 1, 0);
	if (found <= 0)
		return 0;

	/* What came: the end of the input or a line. */
	count = read(session->input, text, sizeof(text) - 1U);
	if (count < 0)
		return 0;
	if (count == 0) {
		session->canceled = 1;
		return 1;
	}

	/* Treats a line holding "cancel" as the installer's cancellation. */
	text[count] = '\0';
	cancel = strstr(text, "cancel");
	if (cancel != NULL)
		session->canceled = 1;

	/* Whether the installer cancelled. */
	return session->canceled;
}

/*
 * Runs a program as root with an empty environment: its input is the
 * answer (and its end), its output goes to the helper's error output, or
 * into capture when one is given.  seconds, when not 0, limits the time
 * the program may take (it is killed after).  Returns 0 with the
 * program's exit status (-1 when a signal ended it), ETIMEDOUT, or the
 * errno value of starting it.
 */
static int
session_run(
	char *const argument[],
	const char *answer,
	char *capture,
	size_t capacity,
	unsigned seconds,
	int *status)
{
	static char *const environment[1] = { NULL };
	int input[2];
	int output[2];
	size_t length;
	size_t taken;
	ssize_t count;
	pid_t child;
	int error;

	/* The pipe of the answer, and the one of the output to keep. */
	*status = -1;
	error = pipe2(input, O_CLOEXEC);
	if (error != 0)
		return errno;
	output[0] = -1;
	output[1] = -1;
	if (capture != NULL) {
		error = pipe2(output, O_CLOEXEC);
		if (error != 0) {
			error = errno;
			close(input[0]);
			close(input[1]);
			return error;
		}
	}

	/* The child: the pipes in place of its input and output, then the program. */
	child = fork();
	if (child < 0) {
		error = errno;
		close(input[0]);
		close(input[1]);
		if (output[0] >= 0) {
			close(output[0]);
			close(output[1]);
		}

		/* Reports why the child could not be started. */
		return error;
	}

	/* Becomes the program in the child, with the pipes as its input and output. */
	if (child == 0) {
		(void)dup2(input[0], 0);
		if (output[1] >= 0)
			(void)dup2(output[1], 1);
		else
			(void)dup2(2, 1);
		(void)execve(argument[0], argument, environment);
		_exit(127);
	}

	/* The answer, then its end; a program that stopped reading early is not an error here. */
	close(input[0]);
	if (answer != NULL) {
		length = strlen(answer);
		(void)write(input[1], answer, length);
	}

	/* Ends the program's input, so it sees the end of the answer. */
	close(input[1]);

	/* The output, to its end. */
	if (capture != NULL) {
		close(output[1]);
		taken = 0;
		for (;;) {
			count = read(output[0], capture + taken, capacity - 1U - taken);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				break;
			taken += (size_t)count;
			if (taken == capacity - 1U)
				break;
		}

		/* Ends the captured text and closes the output pipe. */
		capture[taken] = '\0';
		close(output[0]);
	}

	/* Its end. */
	error = session_wait(child, seconds, status);
	if (error != 0)
		return error;

	/* Succeeded: the program ran; its status says how it ended. */
	return 0;
}

/*
 * Waits for a child, at most seconds when that is not 0 (then it is
 * killed).  Returns 0 with its exit status (-1 for a signal), ETIMEDOUT,
 * or the wait's errno value.
 */
static int
session_wait(
	pid_t child,
	unsigned seconds,
	int *status)
{
	struct timespec pause;
	unsigned waited;
	int options;
	int raw;
	int exited;
	pid_t ended;

	/* Each tenth of a second until it ends or the time is up. */
	pause.tv_sec = 0;
	pause.tv_nsec = 100000000L;
	waited = 0;
	options = 0;
	if (seconds != 0U)
		options = WNOHANG;
	for (;;) {
		ended = waitpid(child, &raw, options);
		if (ended < 0 && errno == EINTR)
			continue;
		if (ended < 0)
			return errno;
		if (ended == child)
			break;
		if (waited >= seconds * 10U) {
			/* Too long: it is stopped, and its end waited for. */
			(void)kill(child, SIGKILL);
			do {
				ended = waitpid(child, &raw, 0);
			} while (ended < 0 && errno == EINTR);
			return ETIMEDOUT;
		}

		/* Waits a tenth of a second before asking about the child again. */
		(void)nanosleep(&pause, NULL);
		waited++;
	}

	/* How it ended. */
	*status = -1;
	exited = WIFEXITED(raw);
	if (exited)
		*status = WEXITSTATUS(raw);

	/* Succeeded: the child ended. */
	return 0;
}

/* Makes a random (version 4) GUID's text, in lower case. */
static int
session_random_guid(
	char text[PLAN_GUID_TEXT + 1U])
{
	uint8_t bytes[16];
	int error;

	/* Sixteen random bytes, marked as a random GUID of the RFC 4122 variant. */
	error = getentropy(bytes, sizeof(bytes));
	if (error != 0)
		return errno;
	bytes[6] = (uint8_t)((bytes[6] & 0x0fU) | 0x40U);
	bytes[8] = (uint8_t)((bytes[8] & 0x3fU) | 0x80U);

	/* The text, 8-4-4-4-12. */
	snprintf(
		text,
		PLAN_GUID_TEXT + 1U,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		bytes[0], bytes[1], bytes[2], bytes[3],
		bytes[4], bytes[5],
		bytes[6], bytes[7],
		bytes[8], bytes[9],
		bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);

	/* Succeeded: a new GUID. */
	return 0;
}

/*
 * Binds the session to the disk of its first writing request, and refuses
 * any other disk afterwards (EINVAL).
 */
static int
session_bind(
	struct admin_session *session,
	const struct plan_disk *disk)
{
	/* The first: the session's disk from now on. */
	if (!session->bound) {
		session->disk = disk->registration;
		session->bound = 1;
		return 0;
	}

	/* Another disk than the session's. */
	if (session->disk != disk->registration)
		return EINVAL;

	/* Succeeded: the session's own disk. */
	return 0;
}

/*
 * Looks at a request's disk as it is now: found by its registration, it
 * has the name, the sector size and the size the screen showed, the
 * running system does not use it, and its table has the digest the screen
 * showed (zero for a disk whose table the codec does not read).  Returns
 * 0 with the scan and the table (session_disk_free releases them),
 * ENODEV for a disk that is gone, EBUSY for one the system uses, ESTALE
 * for one that changed (the scan and the table are then still given).
 */
static int
session_disk_load(
	const struct plan_disk *disk,
	struct session_disk *found)
{
	int same;
	int error;

	/* The devices, scanned anew. */
	memset(found, 0, sizeof(*found));
	found->scan = malloc(sizeof(*found->scan));
	if (found->scan == NULL)
		return ENOMEM;
	error = probe_scan_devices(found->scan);
	if (error != 0) {
		session_disk_free(found);
		return error;
	}

	/* The disk, by its registration. */
	if (disk->registration > UINT32_MAX) {
		session_disk_free(found);
		return EINVAL;
	}

	/* Finds the disk by the registration the screen showed. */
	found->device = probe_find_device(found->scan, (uint32_t)disk->registration);
	if (found->device == NULL) {
		session_disk_free(found);
		return ENODEV;
	}

	/* Refuses a device that is a partition, since only a disk is written. */
	if (found->device->info.parent_device != 0U || (found->device->info.flags & KERN_BLOCK_PARTITION) != 0U) {
		session_disk_free(found);
		return EINVAL;
	}

	/* Not a disk the running system lives on. */
	if (found->device->live) {
		session_disk_free(found);
		return EBUSY;
	}

	/* Its table and the table's digest, as the probe gave them. */
	error = probe_table_load(found->device, &found->table);
	if (error == 0) {
		found->loaded = 1;
		probe_table_digest(&found->table, found->digest);
	}

	/* The disk the screen showed. */
	same = strcmp(found->device->info.name, disk->name);
	if (same != 0)
		return ESTALE;
	if (found->device->info.sector_size != disk->sector_bytes)
		return ESTALE;
	if (found->device->info.sector_count != disk->sectors)
		return ESTALE;
	same = memcmp(found->digest, disk->table_digest, sizeof(found->digest));
	if (same != 0)
		return ESTALE;

	/* Succeeded: the disk is as the screen showed it. */
	return 0;
}

/* Releases what a look into a disk holds. */
static void
session_disk_free(
	struct session_disk *found)
{
	/* The table, then the scan. */
	if (found->loaded)
		dp_free(&found->table);
	found->loaded = 0;
	free(found->scan);
	found->scan = NULL;
	found->device = NULL;
}

/*
 * Unmounts every filesystem mounted from a disk or its partitions (the
 * ones Files had mounted), the last mounted first.  Returns 0, or EBUSY
 * when one stays mounted.
 */
static int
session_unmount_disk(
	uint32_t registration)
{
	struct kern_mount_query *query;
	const struct kern_mount_info *entry;
	struct probe_scan *scan;
	const struct probe_device *device;
	char source[KERN_MOUNT_INFO_PATH_MAX + 1U];
	unsigned pass;
	unsigned left;
	unsigned i;
	unsigned j;
	int same;
	int fd;
	int error;

	/* Room for the devices and the whole mount table. */
	scan = malloc(sizeof(*scan));
	if (scan == NULL)
		return ENOMEM;
	query = calloc(1, sizeof(*query) + KERN_MOUNT_INFO_MAX * sizeof(query->entries[0]));
	if (query == NULL) {
		free(scan);
		return ENOMEM;
	}

	/* A few passes: a mount under another one is undone before it. */
	left = 0;
	for (pass = 0; pass < 4U; pass++) {
		error = probe_scan_devices(scan);
		if (error != 0)
			break;
		memset(query, 0, sizeof(*query));
		query->version = KERN_MOUNT_INFO_VERSION;
		query->struct_size = sizeof(*query);
		query->capacity = KERN_MOUNT_INFO_MAX;
		fd = open("/dev/system", O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			error = errno;
			break;
		}

		/* Reads the whole mount table of the running system. */
		error = ioctl(fd, KERN_SYSTEM_GET_MOUNTS, query);
		if (error != 0)
			error = errno;
		close(fd);
		if (error != 0)
			break;

		/* Each mount of the disk's, the last first. */
		left = 0;
		for (i = query->count; i > 0U; i--) {
			if (i > KERN_MOUNT_INFO_MAX)
				continue;
			entry = &query->entries[i - 1U];
			if (entry->device == 0U || (entry->kind & KERN_MOUNT_INFO_BIND) != 0U)
				continue;
			snprintf(source, sizeof(source), "%.*s", (int)sizeof(entry->source), entry->source);
			device = NULL;
			for (j = 0; j < scan->count; j++) {
				same = strncmp(scan->device[j].info.name, source, sizeof(scan->device[j].info.name));
				if (same == 0)
					device = &scan->device[j];
			}

			/* Skips a mount that is not on a device of the scan. */
			if (device == NULL)
				continue;
			if (device->info.device != registration && device->info.parent_device != registration)
				continue;
			left++;
			error = unmount(entry->target, 0);
			if (error == 0) {
				syslog(LOG_INFO, "unmounted %s from %s", entry->target, source);
				left--;
			}
		}

		/* Forgets the error of the last pass, as the count of mounts left decides. */
		error = 0;
		if (left == 0U)
			break;
	}

	/* Releases the buffers and reports an error that ended the passes. */
	free(query);
	free(scan);
	if (error != 0)
		return error;

	/* A mount that would not go. */
	if (left != 0U)
		return EBUSY;

	/* Succeeded: nothing of the disk is mounted. */
	return 0;
}

/* Finds the entry of a table a tuple names: the same range and PARTUUID. */
static const struct dp_part *
session_find_entry(
	const struct dp_table *table,
	const struct plan_tuple *tuple)
{
	const struct dp_part *part;
	unsigned i;
	int differs;

	/* Each entry. */
	for (i = 0; i < table->count; i++) {
		part = &table->parts[i];
		if (part->start != tuple->start || part->count != tuple->sectors)
			continue;
		differs = strcasecmp(part->uuid, tuple->partuuid);
		if (differs == 0)
			return part;
	}

	/* No entry is the tuple's. */
	return NULL;
}

/* Finds the lowest slot of a table no entry has, above a slot (0 for any). */
static int
session_free_slot(
	const struct dp_table *table,
	unsigned after,
	unsigned *slot)
{
	unsigned candidate;
	unsigned i;
	int taken;

	/* Each slot from the lowest. */
	for (candidate = after + 1U; candidate <= table->slots; candidate++) {
		taken = 0;
		for (i = 0; i < table->count; i++) {
			if (table->parts[i].slot == candidate)
				taken = 1;
		}

		/* Takes the first slot no entry has. */
		if (!taken) {
			*slot = candidate;
			return 0;
		}
	}

	/* Every slot has an entry. */
	return ENOSPC;
}

/* Reports whether a range of sectors meets an entry of a table. */
static int
session_overlaps(
	const struct dp_table *table,
	uint64_t start,
	uint64_t sectors)
{
	const struct dp_part *part;
	unsigned i;

	/* Each entry. */
	for (i = 0; i < table->count; i++) {
		part = &table->parts[i];
		if (start + sectors <= part->start)
			continue;
		if (part->start + part->count <= start)
			continue;
		return 1;
	}

	/* The range is free. */
	return 0;
}

/* Reports whether a table has an ESP. */
static int
session_has_esp(
	const struct dp_table *table)
{
	unsigned i;
	int differs;

	/* Each entry, by its type. */
	for (i = 0; i < table->count; i++) {
		differs = strcasecmp(table->parts[i].type, KERN_GPT_TYPE_EFI_SYSTEM);
		if (differs == 0)
			return 1;
	}

	/* None is an ESP. */
	return 0;
}

/*
 * Checks a partition right before it is cleared or formatted:
 * nothing of its disk mounted, the disk not the running system's, the
 * kernel maps a partition at the tuple's range on the tuple's disk with
 * the tuple's PARTUUID, and the kernel's partitions are the disk's table.
 * Returns 0 with the partition's description (the phrase is made from
 * it), EBUSY, ESTALE when the partition or the table is not the planned
 * one, or another errno value.
 */
static int
session_partition_ready(
	const struct plan_tuple *tuple,
	struct kern_block_info *info)
{
	struct probe_scan *scan;
	const struct probe_device *disk;
	const struct probe_device *partition;
	struct dp_table table;
	int differs;
	int error;

	/* Nothing of the disk mounted. */
	if (tuple->disk > UINT32_MAX)
		return EINVAL;
	error = session_unmount_disk((uint32_t)tuple->disk);
	if (error != 0)
		return error;

	/* The devices, scanned anew. */
	scan = malloc(sizeof(*scan));
	if (scan == NULL)
		return ENOMEM;
	error = probe_scan_devices(scan);
	if (error != 0) {
		free(scan);
		return error;
	}

	/* The disk, not one the running system uses. */
	disk = probe_find_device(scan, (uint32_t)tuple->disk);
	if (disk == NULL) {
		free(scan);
		return ENODEV;
	}

	/* Refuses the disk the running system lives on. */
	if (disk->live) {
		free(scan);
		return EBUSY;
	}

	/* The partition at the range, with the PARTUUID. */
	partition = probe_find_partition(scan, (uint32_t)tuple->disk, tuple->start, tuple->sectors);
	if (partition == NULL || !partition->has_identity || (partition->identity.flags & KERN_BLKID_PARTUUID) == 0U) {
		free(scan);
		return ESTALE;
	}

	/* Refuses a partition with another PARTUUID than the planned one. */
	differs = strcasecmp(partition->identity.partuuid, tuple->partuuid);
	if (differs != 0) {
		free(scan);
		return ESTALE;
	}

	/* The kernel's partitions are the disk's table (a failed reload leaves old ones). */
	error = probe_table_load(disk, &table);
	if (error != 0) {
		free(scan);
		return error;
	}

	/* Checks that the kernel's partitions are the table's. */
	error = session_kernel_matches(scan, disk, &table);
	dp_free(&table);
	if (error != 0) {
		free(scan);
		return error;
	}

	/* Succeeded: the partition is the planned one. */
	*info = partition->info;
	free(scan);
	return 0;
}

/*
 * Checks that the kernel maps exactly the partitions of a disk's table,
 * each at its range (diskpart's own check before a write).  Returns 0 or
 * ESTALE.
 */
static int
session_kernel_matches(
	const struct probe_scan *scan,
	const struct probe_device *disk,
	const struct dp_table *table)
{
	const struct probe_device *partition;
	unsigned mapped;
	unsigned i;

	/* Each entry of the table, mapped by the kernel. */
	for (i = 0; i < table->count; i++) {
		partition = probe_find_partition(scan, disk->info.device, table->parts[i].start, table->parts[i].count);
		if (partition == NULL)
			return ESTALE;
	}

	/* No partition the table has not. */
	mapped = 0;
	for (i = 0; i < scan->count; i++) {
		if (scan->device[i].info.parent_device == disk->info.device)
			mapped++;
	}

	/* Refuses a kernel that maps a partition the table does not have. */
	if (mapped != table->count)
		return ESTALE;

	/* Succeeded: the kernel uses the table. */
	return 0;
}

/*
 * Clears the first and the last MiB of a partition, so that nothing of an
 * earlier filesystem (a FAT with a zedbsd.cfg) is found on it.
 */
static int
session_clear_ends(
	const struct kern_block_info *info)
{
	char path[SESSION_PATH_MAX];
	uint64_t bytes;
	uint64_t piece;
	uint8_t *zero;
	ssize_t count;
	int synced;
	int fd;
	int error;

	/* The partition's size, and the zeros. */
	bytes = info->sector_count * info->sector_size;
	piece = SESSION_CLEAR_BYTES;
	if (piece > bytes)
		piece = bytes;
	zero = calloc(1, (size_t)piece);
	if (zero == NULL)
		return ENOMEM;

	/* The partition, for writing. */
	snprintf(path, sizeof(path), "/dev/%s", info->name);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		error = errno;
		free(zero);
		return error;
	}

	/* Its first MiB, its last, then to the device. */
	error = 0;
	count = pwrite(fd, zero, (size_t)piece, 0);
	if (count < 0)
		error = errno;
	else if (count != (ssize_t)piece)
		error = EIO;
	if (error == 0) {
		count = pwrite(fd, zero, (size_t)piece, (off_t)(bytes - piece));
		if (count < 0)
			error = errno;
		else if (count != (ssize_t)piece)
			error = EIO;
	}

	/* Writes the cleared ends out to the device. */
	if (error == 0) {
		synced = fsync(fd);
		if (synced != 0)
			error = errno;
	}

	/* Releases the partition and the zeros, and reports a failed clearing. */
	close(fd);
	free(zero);
	if (error != 0)
		return error;

	/* Succeeded: both ends are zero. */
	return 0;
}

/*
 * Clears a partition's ends and formats it: mkfs (kind "fat32" or "ufs")
 * or mkswap (kind NULL), given the phrase that names the partition the
 * kernel describes now.  Returns 0, ESTALE for another partition than the
 * planned one, EIO when the program failed, or another errno value.
 */
static int
session_format(
	const struct plan_tuple *tuple,
	const char *program,
	const char *kind)
{
	struct kern_block_info info;
	char path[SESSION_PATH_MAX];
	char answer[KERN_BLOCK_NAME_MAX + 32U];
	char *argument[8];
	struct timespec pause;
	unsigned tries;
	unsigned count;
	int differs;
	int status;
	int error;

	/* The partition, the planned one. */
	error = session_partition_ready(tuple, &info);
	if (error != 0)
		return error;

	/* Its ends cleared. */
	error = session_clear_ends(&info);
	if (error != 0)
		return error;

	/* The program's command line and the phrase. */
	snprintf(path, sizeof(path), "/dev/%s", info.name);
	snprintf(answer, sizeof(answer), "FORMAT %s:%u\n", info.name, info.device);
	count = 0;
	argument[count++] = (char *)program;
	if (kind != NULL) {
		argument[count++] = "-t";
		argument[count++] = (char *)kind;
		differs = strcmp(kind, "ufs");
		if (differs == 0)
			argument[count++] = "--profile=native";
	}

	/* Ends the command line with the partition's device. */
	argument[count++] = path;
	argument[count] = NULL;

	/*
	 * The partition checked again (the clearing took time), then the
	 * format; a few tries, for the partition may be open for a moment
	 * (volumed looks at a disk again after each reload) and a format
	 * begun again writes the same.
	 */
	pause.tv_sec = 1;
	pause.tv_nsec = 0;
	for (tries = 0;; tries++) {
		error = session_partition_ready(tuple, &info);
		if (error != 0)
			return error;
		error = session_run(argument, answer, NULL, 0, 0, &status);
		if (error != 0)
			return error;
		if (status == 0)
			break;
		syslog(LOG_NOTICE, "%s on %s ended %d", program, path, status);
		if (tries + 1U >= SESSION_TRIES)
			return EIO;
		(void)nanosleep(&pause, NULL);
	}

	/* Succeeded: the partition holds a new filesystem or swap area. */
	return 0;
}

/*
 * Runs diskpart on a disk: "init" or "batch" and their operations, given
 * the phrase made from the disk as the kernel describes it now (ERASE for
 * init, WRITE for a batch).  Returns 0 with reload_failed set when the
 * table was written but the kernel could not use it (exit 3), EIO when
 * diskpart failed, or another errno value.
 */
static int
session_diskpart(
	const struct plan_disk *disk,
	char *const operation[],
	unsigned count,
	int *reload_failed)
{
	struct session_disk found;
	char *argument[SESSION_ARGUMENTS_MAX];
	char path[SESSION_PATH_MAX];
	char answer[KERN_BLOCK_NAME_MAX + 32U];
	struct timespec pause;
	uint8_t before[32];
	const char *word;
	unsigned tries;
	unsigned i;
	int initialize;
	int differs;
	int status;
	int error;

	/* The command line: diskpart, the verb, the disk, the operations. */
	*reload_failed = 0;
	pause.tv_sec = 1;
	pause.tv_nsec = 0;
	if (count < 1U || count + 3U > SESSION_ARGUMENTS_MAX)
		return EINVAL;
	initialize = 0;
	differs = strcmp(operation[0], "init");
	if (differs == 0)
		initialize = 1;
	snprintf(path, sizeof(path), "/dev/%s", disk->name);
	argument[0] = SESSION_DISKPART;
	argument[1] = operation[0];
	argument[2] = path;
	for (i = 1; i < count; i++)
		argument[i + 2U] = operation[i];
	argument[count + 2U] = NULL;

	/*
	 * A few tries: diskpart refuses a disk whose partition another
	 * program has open for a moment (volumed looks at a disk again after
	 * each reload), and is tried again while the table is as it was.
	 */
	for (tries = 0;; tries++) {
		/* Nothing of the disk mounted, and the disk as the kernel describes it now. */
		error = session_unmount_disk((uint32_t)disk->registration);
		if (error != 0)
			return error;
		error = session_disk_load(disk, &found);
		if (error == ESTALE) {
			/* A disk whose table the request does not quote again: it is the one by its name and size. */
			error = 0;
		}

		/* Reports a disk that could not be looked at again. */
		if (error != 0) {
			session_disk_free(&found);
			return error;
		}

		/* Makes the phrase diskpart is given: WRITE for a batch, ERASE for init. */
		word = "WRITE";
		if (initialize)
			word = "ERASE";
		snprintf(answer, sizeof(answer), "%s %s:%u\n", word, found.device->info.name, found.device->info.device);
		differs = strcmp(found.device->info.name, disk->name);
		if (found.device->info.sector_size != disk->sector_bytes || found.device->info.sector_count != disk->sectors)
			differs = 1;
		memcpy(before, found.digest, sizeof(before));
		session_disk_free(&found);
		if (differs != 0)
			return ESTALE;

		/* diskpart, and how it ended. */
		error = session_run(argument, answer, NULL, 0, 0, &status);
		if (error != 0)
			return error;
		if (status == 3) {
			*reload_failed = 1;
			return 0;
		}

		/* Ends the tries at a success, and logs any other status. */
		if (status == 0)
			break;
		syslog(LOG_NOTICE, "diskpart %s %s ended %d", operation[0], path, status);

		/* A failure that wrote nothing, tried again a second later. */
		error = session_disk_load(disk, &found);
		if (error != 0 && error != ESTALE) {
			session_disk_free(&found);
			return error;
		}

		/* Refuses to try again when the table changed or the tries are used up. */
		differs = memcmp(before, found.digest, sizeof(before));
		session_disk_free(&found);
		if (differs != 0 || tries + 1U >= SESSION_TRIES)
			return EIO;
		(void)nanosleep(&pause, NULL);
	}

	/* Succeeded: the table is written and the kernel uses it. */
	return 0;
}

/* Deletes the partition a request names. */
static int
session_edit_delete(
	struct admin_session *session,
	const struct plan_request *request,
	struct session_disk *found)
{
	const struct dp_part *part;
	char slot[16];
	char *operation[3];
	int reload_failed;
	int error;

	/* The entry, in the table the screen showed. */
	if (!found->loaded || found->table.format != DP_GPT) {
		session_fail(session, "edit", EINVAL, "not a GPT disk");
		return EINVAL;
	}

	/* Finds the entry the request names. */
	part = session_find_entry(&found->table, &request->part);
	if (part == NULL) {
		session_fail(session, "edit", ESTALE, "the partition is not there");
		return ESTALE;
	}

	/* The delete, one batch. */
	snprintf(slot, sizeof(slot), "%u", part->slot);
	operation[0] = "batch";
	operation[1] = "delete";
	operation[2] = slot;
	error = session_diskpart(&request->disk, operation, 3, &reload_failed);
	if (error != 0) {
		session_fail(session, "edit", error, "the table was not changed");
		return error;
	}

	/* Reports a table the kernel could not use until a restart. */
	if (reload_failed) {
		session_say(session, "STATE reload-failed\n");
		session_fail(session, "edit", EBUSY, "the table was written; the kernel uses it after a restart");
		return EBUSY;
	}

	/* Succeeded: the partition is gone. */
	return 0;
}

/*
 * Creates a partition of the zedBSD "unassigned" type with no name and a
 * new PARTUUID, and before it, for a disk without one, an ESP, in one
 * write.  The new partitions' ends are cleared and the
 * ESP formatted; if that fails, both entries are deleted again in one
 * write.
 */
static int
session_edit_create(
	struct admin_session *session,
	const struct plan_request *request,
	struct session_disk *found)
{
	struct plan_tuple esp;
	struct plan_tuple part;
	char esp_slot[16];
	char esp_start[24];
	char esp_count[24];
	char part_slot[16];
	char part_start[24];
	char part_count[24];
	char *operation[17];
	struct kern_block_info info;
	unsigned esp_number;
	unsigned part_number;
	unsigned entries;
	unsigned count;
	int with_esp;
	int undone;
	int has_esp;
	int reload_failed;
	int overlapping;
	int error;

	/* A GPT disk with room for the entries. */
	with_esp = 0;
	entries = 1;
	if (request->verb == PLAN_EDIT_CREATE_WITH_ESP) {
		with_esp = 1;
		entries = 2;
	}

	/* Refuses a disk that does not have a GPT. */
	if (!found->loaded || found->table.format != DP_GPT) {
		session_fail(session, "edit", EINVAL, "not a GPT disk");
		return EINVAL;
	}

	/* Refuses a table with no room for the new entries. */
	if (found->table.count + entries > 16U) {
		session_fail(session, "edit", ENOSPC, "the table has no room for another entry");
		return ENOSPC;
	}

	/* An ESP made exactly when the disk has none. */
	has_esp = session_has_esp(&found->table);
	if (with_esp && has_esp) {
		session_fail(session, "edit", EEXIST, "the disk has an ESP");
		return EEXIST;
	}

	/* Refuses a disk with no ESP when none is made with the partition. */
	if (!with_esp && !has_esp) {
		session_fail(session, "edit", EINVAL, "the disk has no ESP; one is made with the partition");
		return EINVAL;
	}

	/* Free sectors for the new partitions. */
	overlapping = session_overlaps(&found->table, request->part_new.start, request->part_new.sectors);
	if (!overlapping && with_esp)
		overlapping = session_overlaps(&found->table, request->esp_new.start, request->esp_new.sectors);
	if (overlapping) {
		session_fail(session, "edit", EINVAL, "the sectors are not free");
		return EINVAL;
	}

	/* The slots and the PARTUUIDs. */
	memset(&esp, 0, sizeof(esp));
	memset(&part, 0, sizeof(part));
	esp_number = 0;
	error = 0;
	if (with_esp)
		error = session_free_slot(&found->table, 0, &esp_number);
	if (error == 0)
		error = session_free_slot(&found->table, esp_number, &part_number);
	if (error == 0 && with_esp)
		error = session_random_guid(esp.partuuid);
	if (error == 0)
		error = session_random_guid(part.partuuid);
	if (error != 0) {
		session_fail(session, "edit", error, "no slot or GUID for the partition");
		return error;
	}

	/* Names the new partitions' disk and ranges. */
	esp.disk = request->disk.registration;
	esp.start = request->esp_new.start;
	esp.sectors = request->esp_new.sectors;
	part.disk = request->disk.registration;
	part.start = request->part_new.start;
	part.sectors = request->part_new.sectors;

	/* The batch: the ESP's addition first, then the partition's. */
	count = 0;
	operation[count++] = "batch";
	if (with_esp) {
		snprintf(esp_slot, sizeof(esp_slot), "%u", esp_number);
		snprintf(esp_start, sizeof(esp_start), "%llu", (unsigned long long)esp.start);
		snprintf(esp_count, sizeof(esp_count), "%llu", (unsigned long long)esp.sectors);
		operation[count++] = "add";
		operation[count++] = esp_slot;
		operation[count++] = esp_start;
		operation[count++] = esp_count;
		operation[count++] = KERN_GPT_TYPE_EFI_SYSTEM;
		operation[count++] = esp.partuuid;
		operation[count++] = SESSION_ESP_NAME;
	}

	/* Adds the partition itself, after the ESP when there is one. */
	snprintf(part_slot, sizeof(part_slot), "%u", part_number);
	snprintf(part_start, sizeof(part_start), "%llu", (unsigned long long)part.start);
	snprintf(part_count, sizeof(part_count), "%llu", (unsigned long long)part.sectors);
	operation[count++] = "add";
	operation[count++] = part_slot;
	operation[count++] = part_start;
	operation[count++] = part_count;
	operation[count++] = KERN_GPT_TYPE_ZEDBSD_UNASSIGNED;
	operation[count++] = part.partuuid;
	operation[count++] = "";
	error = session_diskpart(&request->disk, operation, count, &reload_failed);
	if (error != 0) {
		session_fail(session, "edit", error, "the table was not changed");
		return error;
	}

	/* Reports a table the kernel could not use until a restart. */
	if (reload_failed) {
		session_say(session, "STATE reload-failed\n");
		session_fail(session, "edit", EBUSY, "the table was written; the kernel uses it after a restart");
		return EBUSY;
	}

	/* The new partitions cleared at both ends, the ESP formatted. */
	error = 0;
	if (with_esp)
		error = session_format(&esp, SESSION_MKFS, "fat32");
	if (error == 0)
		error = session_partition_ready(&part, &info);
	if (error == 0)
		error = session_clear_ends(&info);
	if (error != 0) {
		/* Undone: both entries deleted in one write, as no use was made of them. */
		count = 0;
		operation[count++] = "batch";
		if (with_esp) {
			operation[count++] = "delete";
			operation[count++] = esp_slot;
		}

		/* Deletes the partition itself, after the ESP's entry. */
		operation[count++] = "delete";
		operation[count++] = part_slot;
		undone = session_diskpart(&request->disk, operation, count, &reload_failed);
		if (undone != 0 || reload_failed)
			session_say(session, "STATE created-not-deleted\n");
		session_fail(session, "edit", error, "the new partitions could not be prepared");
		return error;
	}

	/* What was made, for the screen. */
	if (with_esp)
		session_say(session, "STATE created esp %llu %llu %s\n", (unsigned long long)esp.start, (unsigned long long)esp.sectors, esp.partuuid);
	session_say(session, "STATE created part %llu %llu %s\n", (unsigned long long)part.start, (unsigned long long)part.sectors, part.partuuid);

	/* Succeeded: the partitions are made. */
	return 0;
}

/*
 * Checks a directory of the payload: a directory of root's that no group
 * or other may write.  Returns 0 or EPERM.
 */
static int
session_payload_directory(
	const char *path)
{
	struct stat status;
	int is_directory;
	int error;

	/* The directory itself, not a link. */
	error = lstat(path, &status);
	if (error != 0)
		return errno;
	is_directory = S_ISDIR(status.st_mode);
	if (!is_directory || status.st_uid != 0 || (status.st_mode & 022) != 0)
		return EPERM;

	/* Succeeded: only root changes it. */
	return 0;
}

/*
 * Opens a file of the payload, never through a link, and checks it is a
 * regular file of root's that no group or other may write.
 */
static int
session_payload_open(
	const char *directory,
	const char *name,
	int *fd)
{
	char path[SESSION_PATH_MAX];
	struct stat status;
	const char *slash;
	int descriptor;
	int regular;
	int error;

	/* The file, by a name with no directory in it. */
	slash = strchr(name, '/');
	if (slash != NULL || name[0] == '\0' || name[0] == '.')
		return EINVAL;
	snprintf(path, sizeof(path), "%s/%s", directory, name);
	descriptor = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return errno;

	/* Root's alone. */
	error = fstat(descriptor, &status);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* Refuses a file that is not a regular file of root's that others cannot write. */
	regular = S_ISREG(status.st_mode);
	if (!regular || status.st_uid != 0 || (status.st_mode & 022) != 0) {
		close(descriptor);
		return EPERM;
	}

	/* Succeeded: the caller reads and closes it. */
	*fd = descriptor;
	return 0;
}

/* Reads the memory the system manages, for the swap file's size. */
static int
session_memory_bytes(
	uint64_t *bytes)
{
	static const int name[2] = { CTL_HW, HW_MEMORY_STATS };
	struct memory_stats stats;
	size_t length;
	int error;

	/* The kernel's counts. */
	memset(&stats, 0, sizeof(stats));
	length = sizeof(stats);
	error = sysctl(name, 2, &stats, &length, NULL, 0);
	if (error != 0)
		return errno;

	/* Succeeded: the managed memory. */
	*bytes = stats.physical_managed_bytes;
	return 0;
}

/*
 * Asks mkfs how many inodes a native UFS of a size has (mkfs --check-size,
 * which writes nothing).  Returns 0 with the count, or EINVAL when mkfs
 * cannot make a UFS of that size.
 */
static int
session_root_capacity(
	uint32_t sector_bytes,
	uint64_t bytes,
	uint64_t *inodes)
{
	char sector_option[32];
	char size[32];
	char output[SESSION_CAPTURE_MAX];
	char format[32];
	char *argument[8];
	unsigned long long sector;
	unsigned long long total;
	unsigned long long free_bytes;
	unsigned long long free_inodes;
	unsigned version;
	unsigned unit;
	int fields;
	int status;
	int error;

	/* The question, by the sector size of the disk. */
	snprintf(sector_option, sizeof(sector_option), "--sector-size=%u", sector_bytes);
	snprintf(size, sizeof(size), "%llu", (unsigned long long)bytes);
	argument[0] = SESSION_MKFS;
	argument[1] = "-t";
	argument[2] = "ufs";
	argument[3] = "--profile=native";
	argument[4] = sector_option;
	argument[5] = "--check-size";
	argument[6] = size;
	argument[7] = NULL;
	error = session_run(argument, NULL, output, sizeof(output), 0, &status);
	if (error != 0)
		return error;
	if (status != 0)
		return EINVAL;

	/* The answer: format 1 ufs-native SECTOR BYTES FREE-BYTES FREE-INODES UNIT. */
	fields = sscanf(output, "format\t%u\t%31s\t%llu\t%llu\t%llu\t%llu\t%u", &version, format, &sector, &total, &free_bytes, &free_inodes, &unit);
	if (fields != 7 || version != 1U)
		return EINVAL;

	/* Succeeded: the inodes the root would have. */
	*inodes = free_inodes;
	return 0;
}

/*
 * Opens a directory under another, one component at a time without
 * following a link, making the missing ones (mode 0755) when asked.  An
 * empty path is the base itself.  Returns 0 with a descriptor the caller
 * closes, or an errno value.
 */
static int
session_open_directory(
	int base,
	const char *path,
	int create,
	int *fd)
{
	char component[SESSION_PATH_MAX];
	const char *start;
	const char *slash;
	size_t length;
	int current;
	int next;
	int made;
	int dot;
	int dotdot;
	int error;

	/* The base, as a descriptor of its own. */
	current = fcntl(base, F_DUPFD_CLOEXEC, 3);
	if (current < 0)
		return errno;

	/* Each component, under the one before. */
	start = path;
	while (*start != '\0') {
		slash = strchr(start, '/');
		if (slash == NULL)
			length = strlen(start);
		else
			length = (size_t)(slash - start);
		if (length == 0U || length >= sizeof(component)) {
			close(current);
			return EINVAL;
		}

		/* Takes the next component of the path, and refuses "." and "..". */
		memcpy(component, start, length);
		component[length] = '\0';
		dot = strcmp(component, ".");
		dotdot = strcmp(component, "..");
		if (dot == 0 || dotdot == 0) {
			close(current);
			return EINVAL;
		}

		/* Made when missing, then opened. */
		if (create) {
			made = mkdirat(current, component, 0755);
			if (made != 0 && errno != EEXIST) {
				error = errno;
				close(current);
				return error;
			}
		}

		/* Opens the component, never through a link. */
		next = openat(current, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		error = errno;
		close(current);
		if (next < 0)
			return error;
		current = next;
		start += length;
		if (*start == '/')
			start++;
	}

	/* Succeeded: the directory. */
	*fd = current;
	return 0;
}

/*
 * Writes a file under a directory: the bytes go to LEAF.new, which is
 * synced and renamed over LEAF, and the directory synced.  Returns 0 or
 * an errno value.
 */
static int
session_write_new(
	int directory,
	const char *leaf,
	const uint8_t *data,
	size_t length,
	mode_t mode)
{
	char temporary[SESSION_PATH_MAX];
	size_t written;
	ssize_t count;
	int differs;
	int ended;
	int fd;
	int error;

	/* The new name, free of an earlier attempt's file. */
	snprintf(temporary, sizeof(temporary), "%s%s", leaf, SESSION_NEW_SUFFIX);
	differs = strcmp(leaf, SESSION_CFG);
	if (differs == 0)
		snprintf(temporary, sizeof(temporary), "%s", SESSION_CFG_NEW);
	(void)unlinkat(directory, temporary, 0);
	fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
	if (fd < 0)
		return errno;

	/* The bytes, all of them, then to the device. */
	error = 0;
	written = 0;
	while (written < length) {
		count = write(fd, data + written, length - written);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			error = errno;
			break;
		}

		/* Refuses a write that wrote nothing. */
		if (count == 0) {
			error = EIO;
			break;
		}

		/* Counts the bytes written so far. */
		written += (size_t)count;
	}

	/* Writes the file out to the device. */
	if (error == 0) {
		ended = fsync(fd);
		if (ended != 0)
			error = errno;
	}

	/* Closes the file, and removes the new name when the write failed. */
	ended = close(fd);
	if (ended != 0 && error == 0)
		error = errno;
	if (error != 0) {
		(void)unlinkat(directory, temporary, 0);
		return error;
	}

	/* The rename. */
	error = renameat(directory, temporary, directory, leaf);
	if (error != 0) {
		error = errno;
		(void)unlinkat(directory, temporary, 0);
		return error;
	}

	/*
	 * The directory to the device; a filesystem that cannot sync a
	 * directory alone (the FAT) has it written by the sync that ends the
	 * step.
	 */
	ended = fsync(directory);
	if (ended != 0) {
		error = errno;
		if (error != EOPNOTSUPP && error != EINVAL)
			return error;
	}

	/* Succeeded: the file has its new bytes. */
	return 0;
}

/*
 * Copies a boot file of the payload under a directory, its size and
 * SHA-256 checked against the index on the bytes read (one read of one
 * descriptor).
 */
static int
session_copy_boot(
	const struct index_boot *boot,
	int directory,
	const char *leaf)
{
	uint8_t *data;
	size_t length;
	int fd;
	int error;

	/* The file, read whole and checked. */
	error = session_payload_open(SESSION_BOOT, boot->name, &fd);
	if (error != 0)
		return error;
	error = unpack_read_whole(fd, boot->bytes, boot->sha256, &data, &length);
	close(fd);
	if (error != 0)
		return error;

	/* Its copy. */
	error = session_write_new(directory, leaf, data, length, 0644);
	free(data);
	if (error != 0)
		return error;

	/* Succeeded: the file is in place. */
	return 0;
}

/*
 * Looks into an existing ESP before anything is written: a
 * FAT the kernel mounts, without zedbsd.cfg or the zedBSD loader of
 * another system, with room for the loader and 16 MiB.  Notes whether
 * the firmware's default loader is another system's (then the loader goes
 * to EFI/ZEDBSD).  Returns 0, EEXIST for another zedBSD's ESP, ENOSPC, or
 * another errno value.
 */
static int
session_esp_look(
	struct session_installation *installation,
	const struct kern_block_info *info)
{
	const struct installer_index *index;
	struct mount_args arguments;
	struct statvfs room;
	struct stat status;
	uint64_t needed;
	unsigned i;
	int other;
	int found;
	int differs;
	int error;

	/* The room the loader needs. */
	index = installation->session->index;
	needed = SESSION_ESP_SPARE;
	for (i = 0; i < index->boot_count; i++) {
		differs = strncmp(index->boot[i].place, "esp:", 4U);
		if (differs == 0)
			needed += index->boot[i].bytes;
	}

	/* The FAT, read-only for a moment. */
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = KERN_MOUNT_ARGS_VERSION;
	snprintf(arguments.fspec, sizeof(arguments.fspec), "%s", info->name);
	error = mount("fat", PROBE_MOUNT_POINT, (int)(MNT_RDONLY | MNT_NOSUID | MNT_NOEXEC), &arguments);
	if (error != 0)
		return errno;

	/* Its files: another zedBSD's configuration and loader, another system's default loader. */
	other = 0;
	found = lstat(PROBE_MOUNT_POINT "/" SESSION_CFG, &status);
	if (found == 0)
		other = 1;
	found = lstat(PROBE_MOUNT_POINT "/" SESSION_LOADER_OWN "/BOOTX64.EFI", &status);
	if (found == 0)
		other = 1;
	found = lstat(PROBE_MOUNT_POINT "/" SESSION_LOADER_DEFAULT "/BOOTX64.EFI", &status);
	if (found == 0)
		installation->loader_own = 1;

	/* Its room, then the unmount. */
	error = statvfs(PROBE_MOUNT_POINT, &room);
	if (error != 0)
		error = errno;
	found = unmount(PROBE_MOUNT_POINT, 0);
	if (found != 0 && error == 0)
		error = errno;
	if (error != 0)
		return error;

	/* Another zedBSD's, or too small. */
	if (other)
		return EEXIST;
	if ((uint64_t)room.f_bavail * (uint64_t)room.f_frsize < needed)
		return ENOSPC;

	/* Succeeded: the ESP takes the loader. */
	return 0;
}

/*
 * Step 1: the plan checked again, the disk as the final page showed it,
 * the payload root's, the units and their order, the partitions laid out
 * and the root large enough.
 */
static int
session_install_check(
	struct session_installation *installation)
{
	struct plan_request *request;
	const struct installer_index *index;
	struct session_disk found;
	unsigned chosen[PLAN_UNITS_MAX];
	unsigned slot;
	unsigned i;
	int has_base;
	int same;
	int canceled;
	int error;

	/* The plan, as the helper reads it, and the session's disk. */
	request = installation->request;
	index = installation->session->index;
	error = plan_install_check(&request->disk, &request->install);
	if (error != 0)
		return error;
	error = session_bind(installation->session, &request->disk);
	if (error != 0)
		return error;

	/* The payload, root's alone. */
	error = session_payload_directory(SESSION_PAYLOAD);
	if (error == 0)
		error = session_payload_directory(SESSION_UNITS);
	if (error == 0)
		error = session_payload_directory(SESSION_BOOT);
	if (error != 0)
		return error;

	/* The units, the base among them, and their order with what they need. */
	has_base = 0;
	for (i = 0; i < request->install.unit_count; i++) {
		error = index_find_unit(index, request->install.unit[i], &slot);
		if (error != 0)
			return error;
		chosen[i] = slot;
		same = strcmp(request->install.unit[i], "base");
		if (same == 0)
			has_base = 1;
	}

	/* Refuses a choice without the base unit. */
	if (!has_base)
		return EINVAL;
	error = index_closure(index, chosen, request->install.unit_count, installation->order, &installation->order_count);
	if (error != 0)
		return error;

	/* What they hold, and whether the desktop is among them (a graphical login, an automatic one). */
	for (i = 0; i < installation->order_count; i++) {
		slot = installation->order[i];
		installation->total_bytes += index->unit[slot].bytes;
		installation->total_entries += index->unit[slot].entries;
		same = strcmp(index->unit[slot].name, "desktop");
		if (same == 0)
			installation->graphical = 1;
	}

	/* Drops the autologin of a system with no graphical session. */
	if (!installation->graphical)
		request->install.autologin = 0;

	/* The disk as the final page showed it. */
	error = session_disk_load(&request->disk, &found);
	if (error != 0) {
		session_disk_free(&found);
		return error;
	}

	/* The partitions: laid out anew, or the chosen ones found in the table. */
	if (request->install.layout == PLAN_WHOLE)
		error = session_install_whole_layout(installation);
	else
		error = session_install_chosen_layout(installation, &found);
	session_disk_free(&found);
	if (error != 0)
		return error;

	/* The root, large enough. */
	error = session_install_size(installation);
	if (error != 0)
		return error;

	/* Cancelled before anything was written. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: the installation may begin. */
	return 0;
}

/*
 * Lays out a whole disk: the ESP at the first MiB, the swap
 * partition after it when the plan has one, and the root to the last MiB,
 * each with a new PARTUUID, and a new GUID for the disk.
 */
static int
session_install_whole_layout(
	struct session_installation *installation)
{
	const struct plan_disk *disk;
	const struct install_plan *plan;
	uint64_t grid;
	uint64_t end;
	int error;

	/* The MiB in sectors, and the end of the last whole MiB before the disk's last. */
	disk = &installation->request->disk;
	plan = &installation->request->install;
	grid = PLAN_ALIGN_BYTES / disk->sector_bytes;
	end = ((disk->sectors * disk->sector_bytes - PLAN_ALIGN_BYTES) / PLAN_ALIGN_BYTES) * grid;

	/* The ESP. */
	installation->new_esp = 1;
	installation->esp.disk = disk->registration;
	installation->esp.start = grid;
	installation->esp.sectors = plan->whole_esp_bytes / disk->sector_bytes;
	installation->esp_slot = 1;

	/* The swap partition, when the plan has one. */
	installation->swap.disk = disk->registration;
	installation->swap.start = installation->esp.start + installation->esp.sectors;
	installation->swap.sectors = plan->whole_swap_bytes / disk->sector_bytes;
	installation->has_swap_partition = 0;
	if (plan->whole_swap_bytes != 0U) {
		installation->has_swap_partition = 1;
		installation->swap_slot = 2;
	}

	/* The root, the rest. */
	installation->root.disk = disk->registration;
	installation->root.start = installation->swap.start + installation->swap.sectors;
	if (end <= installation->root.start)
		return ENOSPC;
	installation->root.sectors = end - installation->root.start;
	installation->root_slot = 2U;
	if (installation->has_swap_partition)
		installation->root_slot = 3U;

	/* The new GUIDs. */
	error = session_random_guid(installation->disk_uuid);
	if (error == 0)
		error = session_random_guid(installation->esp.partuuid);
	if (error == 0)
		error = session_random_guid(installation->root.partuuid);
	if (error == 0 && installation->has_swap_partition)
		error = session_random_guid(installation->swap.partuuid);
	if (error != 0)
		return error;

	/* Succeeded: the layout is planned. */
	return 0;
}

/*
 * Finds the chosen partitions in the table: the ESP an ESP, the
 * root and the swap partition other entries; the root and the swap
 * partition take new PARTUUIDs.
 */
static int
session_install_chosen_layout(
	struct session_installation *installation,
	const struct session_disk *found)
{
	const struct install_plan *plan;
	const struct dp_part *esp;
	const struct dp_part *root;
	const struct dp_part *swap;
	int differs;
	int error;

	/* A GPT disk. */
	plan = &installation->request->install;
	if (!found->loaded || found->table.format != DP_GPT)
		return EINVAL;

	/* The ESP, an ESP of the table. */
	esp = session_find_entry(&found->table, &plan->esp);
	if (esp == NULL)
		return ESTALE;
	differs = strcasecmp(esp->type, KERN_GPT_TYPE_EFI_SYSTEM);
	if (differs != 0)
		return EINVAL;
	installation->esp = plan->esp;
	installation->esp_slot = esp->slot;

	/* The root, another entry. */
	root = session_find_entry(&found->table, &plan->root);
	if (root == NULL)
		return ESTALE;
	differs = strcasecmp(root->type, KERN_GPT_TYPE_EFI_SYSTEM);
	if (differs == 0)
		return EINVAL;
	installation->root = plan->root;
	installation->root_slot = root->slot;
	error = session_random_guid(installation->root.partuuid);
	if (error != 0)
		return error;

	/* The swap partition, when the plan has one. */
	installation->has_swap_partition = plan->has_swap_partition;
	if (!plan->has_swap_partition)
		return 0;
	swap = session_find_entry(&found->table, &plan->swap);
	if (swap == NULL)
		return ESTALE;
	differs = strcasecmp(swap->type, KERN_GPT_TYPE_EFI_SYSTEM);
	if (differs == 0 || swap->slot == root->slot)
		return EINVAL;
	installation->swap = plan->swap;
	installation->swap_slot = swap->slot;
	error = session_random_guid(installation->swap.partuuid);
	if (error != 0)
		return error;

	/* Succeeded: the chosen partitions are the table's. */
	return 0;
}

/*
 * Checks the root's size: what is installed and a tenth more,
 * the swap file, and 1 GiB; the inodes for the entries and a fifth more.
 */
static int
session_install_size(
	struct session_installation *installation)
{
	const struct plan_disk *disk;
	uint64_t memory;
	uint64_t root_bytes;
	uint64_t needed;
	uint64_t inodes;
	int error;

	/* The swap file, when there is no swap partition: the memory, at most 2 GiB less a page. */
	disk = &installation->request->disk;
	installation->swapfile_bytes = 0;
	if (!installation->has_swap_partition) {
		error = session_memory_bytes(&memory);
		if (error != 0)
			return error;
		if (memory > SESSION_SWAPFILE_MAX)
			memory = SESSION_SWAPFILE_MAX;
		installation->swapfile_bytes = memory & ~(uint64_t)4095U;
	}

	/* The bytes. */
	root_bytes = installation->root.sectors * disk->sector_bytes;
	needed = installation->total_bytes + installation->total_bytes / 10U;
	needed += installation->swapfile_bytes + SESSION_ROOT_SPARE;
	if (root_bytes < needed) {
		syslog(LOG_NOTICE, "root of %llu bytes, %llu needed", (unsigned long long)root_bytes, (unsigned long long)needed);
		return ENOSPC;
	}

	/* The inodes. */
	error = session_root_capacity(disk->sector_bytes, root_bytes, &inodes);
	if (error != 0)
		return error;
	if (inodes < installation->total_entries + installation->total_entries / 5U) {
		syslog(LOG_NOTICE, "root of %llu inodes, %llu entries", (unsigned long long)inodes, (unsigned long long)installation->total_entries);
		return ENOSPC;
	}

	/* Succeeded: the root holds the installation. */
	return 0;
}

/*
 * Step 2: nothing of the disk mounted, and an existing ESP looked into
 * while nothing is written yet.
 */
static int
session_install_claim(
	struct session_installation *installation)
{
	struct kern_block_info info;
	int canceled;
	int error;

	/* Nothing of the disk mounted. */
	error = session_unmount_disk((uint32_t)installation->request->disk.registration);
	if (error != 0)
		return error;

	/* An existing ESP, one the loader may go on. */
	if (!installation->new_esp) {
		error = session_partition_ready(&installation->esp, &info);
		if (error != 0)
			return error;
		error = session_esp_look(installation, &info);
		if (error != 0)
			return error;
	}

	/* Cancelled before anything was written. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: the disk is the installation's. */
	return 0;
}

/*
 * Step 3: the table: a new GPT for the whole disk, or the
 * chosen root and swap partition marked with their types, names and new
 * PARTUUIDs and no attributes, in one write.
 */
static int
session_install_table(
	struct session_installation *installation)
{
	struct plan_tuple *part[3];
	const char *type[3];
	const char *name[3];
	char start[3][24];
	char count[3][24];
	char slot[3][16];
	char *operation[SESSION_ARGUMENTS_MAX];
	unsigned parts;
	unsigned words;
	unsigned i;
	int reload_failed;
	int canceled;
	int error;

	/* The partitions the table step writes, in their slots' order. */
	parts = 0;
	if (installation->new_esp) {
		part[parts] = &installation->esp;
		type[parts] = KERN_GPT_TYPE_EFI_SYSTEM;
		name[parts] = SESSION_ESP_NAME;
		snprintf(slot[parts], sizeof(slot[parts]), "%u", installation->esp_slot);
		parts++;
	}

	/* Adds the swap partition when the installation makes one. */
	if (installation->has_swap_partition) {
		part[parts] = &installation->swap;
		type[parts] = KERN_GPT_TYPE_ZEDBSD_SWAP;
		name[parts] = SESSION_SWAP_NAME;
		snprintf(slot[parts], sizeof(slot[parts]), "%u", installation->swap_slot);
		parts++;
	}

	/* Adds the root, the last partition of the batch. */
	part[parts] = &installation->root;
	type[parts] = KERN_GPT_TYPE_ZEDBSD_UFS;
	name[parts] = SESSION_ROOT_NAME;
	snprintf(slot[parts], sizeof(slot[parts]), "%u", installation->root_slot);
	parts++;

	/* The command: init with the disk's GUID and every partition, or a batch of sets. */
	words = 0;
	if (installation->request->install.layout == PLAN_WHOLE) {
		operation[words++] = "init";
		operation[words++] = installation->disk_uuid;
		for (i = 0; i < parts; i++) {
			snprintf(start[i], sizeof(start[i]), "%llu", (unsigned long long)part[i]->start);
			snprintf(count[i], sizeof(count[i]), "%llu", (unsigned long long)part[i]->sectors);
			operation[words++] = start[i];
			operation[words++] = count[i];
			operation[words++] = (char *)type[i];
			operation[words++] = part[i]->partuuid;
			operation[words++] = (char *)name[i];
		}
	} else {
		operation[words++] = "batch";
		for (i = 0; i < parts; i++) {
			operation[words++] = "set";
			operation[words++] = slot[i];
			operation[words++] = (char *)type[i];
			operation[words++] = part[i]->partuuid;
			operation[words++] = (char *)name[i];
			operation[words++] = "0";
		}
	}

	/* The write. */
	error = session_diskpart(&installation->request->disk, operation, words, &reload_failed);
	if (error != 0)
		return error;
	if (reload_failed) {
		session_say(installation->session, "STATE table written, reload-failed\n");
		return EBUSY;
	}

	/* Tells the installer the table is written. */
	session_say(installation->session, "STATE table written\n");

	/* Cancelled: taken at the end of the step. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: the kernel uses the new table. */
	return 0;
}

/*
 * Step 4: the filesystems: a new ESP as FAT32, the swap
 * partition's header, and the root as native UFS, each partition checked
 * and its ends cleared first.
 */
static int
session_install_format(
	struct session_installation *installation)
{
	int canceled;
	int error;

	/* The new ESP. */
	if (installation->new_esp) {
		error = session_format(&installation->esp, SESSION_MKFS, "fat32");
		if (error != 0)
			return error;
	}

	/* The swap partition. */
	if (installation->has_swap_partition) {
		error = session_format(&installation->swap, SESSION_MKSWAP, NULL);
		if (error != 0)
			return error;
	}

	/* The root. */
	error = session_format(&installation->root, SESSION_MKFS, "ufs");
	if (error != 0)
		return error;
	session_say(installation->session, "STATE formatted\n");

	/* Cancelled: taken at the end of the step. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: the partitions are formatted. */
	return 0;
}

/* Step 5: the new root mounted without set-ID, and opened. */
static int
session_install_mount(
	struct session_installation *installation)
{
	struct kern_block_info info;
	struct mount_args arguments;
	int canceled;
	int error;

	/* The root, the planned partition. */
	error = session_partition_ready(&installation->root, &info);
	if (error != 0)
		return error;

	/* The mount. */
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = KERN_MOUNT_ARGS_VERSION;
	snprintf(arguments.fspec, sizeof(arguments.fspec), "%s", info.name);
	error = mount("ufs", SESSION_TARGET, (int)MNT_NOSUID, &arguments);
	if (error != 0)
		return errno;
	installation->root_mounted = 1;

	/* Its top, which every file is made under. */
	installation->root_fd = open(SESSION_TARGET, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (installation->root_fd < 0)
		return errno;

	/* Cancelled: taken at the end of the step. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: the root is mounted. */
	return 0;
}

/*
 * Step 6: the units unpacked in their order, the kernel and the logo put
 * on the root, and the record of what was installed.
 */
static int
session_install_unpack(
	struct session_installation *installation)
{
	unsigned i;
	int canceled;
	int error;

	/* Each unit, its files recorded as they are made. */
	session_say(installation->session, "BYTES 0 %llu\n", (unsigned long long)installation->total_bytes);
	for (i = 0; i < installation->order_count; i++) {
		error = session_unpack_unit(installation, installation->order[i]);
		if (error != 0)
			return error;
	}

	/* Reports the bytes unpacked so far. */
	session_say(
		installation->session,
		"BYTES %llu %llu\n",
		(unsigned long long)installation->done_bytes,
		(unsigned long long)installation->total_bytes);

	/* The boot files of the root, then the record. */
	error = session_install_boot_files(installation);
	if (error != 0)
		return error;
	error = session_install_records(installation);
	if (error != 0)
		return error;

	/* Cancelled: taken at the end of the step. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: everything is unpacked. */
	return 0;
}

/*
 * Told of each entry unpacked: records its path, counts its bytes, reports
 * the progress and looks for a cancellation now and then.
 */
static int
session_entry_done(
	void *context,
	const char *path,
	enum unpack_kind kind,
	uint64_t bytes)
{
	struct session_installation *installation;
	int canceled;
	int written;

	/* The record of the unit's files. */
	installation = context;
	written = fprintf(installation->files, "%s\n", path);
	if (written < 0)
		return EIO;

	/* The progress. */
	if (kind == UNPACK_FILE)
		installation->done_bytes += bytes;
	if (installation->done_bytes - installation->reported_bytes >= SESSION_REPORT_BYTES) {
		installation->reported_bytes = installation->done_bytes;
		session_say(
			installation->session,
			"BYTES %llu %llu\n",
			(unsigned long long)installation->done_bytes,
			(unsigned long long)installation->total_bytes);
	}

	/* A cancellation, between files. */
	installation->entries_since_look++;
	if (installation->entries_since_look >= SESSION_CANCEL_ENTRIES) {
		installation->entries_since_look = 0;
		canceled = session_look_for_cancel(installation->session);
		if (canceled)
			return ECANCELED;
	}

	/* Succeeded: the next entry may come. */
	return 0;
}

/*
 * Unpacks one unit: its tarball read whole and checked against the index,
 * inflated, and its entries made under the root, each recorded in the
 * unit's list of files.
 */
static int
session_unpack_unit(
	struct session_installation *installation,
	unsigned unit)
{
	const struct index_unit *record;
	char list[INDEX_NAME_MAX + 16U];
	char tarball[INDEX_NAME_MAX + 16U];
	uint8_t *data;
	uint8_t *archive;
	size_t length;
	size_t archive_length;
	uint64_t limit;
	int records;
	int list_fd;
	int ended;
	int fd;
	int error;

	/* The unit, told to the installer. */
	record = &installation->session->index->unit[unit];
	session_say(installation->session, "UNIT %s\n", record->name);

	/* Its list of files, new. */
	error = session_open_directory(installation->root_fd, SESSION_RECORDS, 1, &records);
	if (error != 0)
		return error;
	snprintf(list, sizeof(list), "%s.files", record->name);
	list_fd = openat(records, list, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	error = errno;
	close(records);
	if (list_fd < 0)
		return error;
	installation->files = fdopen(list_fd, "w");
	if (installation->files == NULL) {
		error = errno;
		close(list_fd);
		return error;
	}

	/* The tarball, read whole from one descriptor and checked. */
	snprintf(tarball, sizeof(tarball), "%s.tar.gz", record->name);
	error = session_payload_open(SESSION_UNITS, tarball, &fd);
	if (error == 0) {
		error = unpack_read_whole(fd, record->tar_bytes, record->sha256, &data, &length);
		close(fd);
	}

	/* Inflated: no more than the files, a block of header and padding each, and the end. */
	archive = NULL;
	if (error == 0) {
		limit = record->bytes + record->entries * 1024U + PLAN_ALIGN_BYTES;
		error = unpack_gunzip(data, length, limit, &archive, &archive_length);
		free(data);
	}

	/* Its entries. */
	if (error == 0) {
		error = unpack_tar(installation->root_fd, archive, archive_length, installation->session->index, unit, session_entry_done, installation);
		free(archive);
	}

	/* The list, whole on the disk. */
	ended = fflush(installation->files);
	if (ended != 0 && error == 0)
		error = errno;
	list_fd = fileno(installation->files);
	ended = fsync(list_fd);
	if (ended != 0 && error == 0)
		error = errno;
	ended = fclose(installation->files);
	if (ended != 0 && error == 0)
		error = errno;
	installation->files = NULL;
	if (error != 0)
		return error;

	/* Succeeded: the unit is installed. */
	return 0;
}

/* Writes the record of the installed units and their versions. */
static int
session_install_records(
	struct session_installation *installation)
{
	const struct index_unit *record;
	char text[INDEX_UNITS_MAX * (INDEX_NAME_MAX + INDEX_VERSION_MAX + 2U)];
	size_t used;
	unsigned i;
	int records;
	int written;
	int error;

	/* One line a unit: its name and its version. */
	used = 0;
	for (i = 0; i < installation->order_count; i++) {
		record = &installation->session->index->unit[installation->order[i]];
		written = snprintf(text + used, sizeof(text) - used, "%s\t%s\n", record->name, record->version);
		if (written < 0 || (size_t)written >= sizeof(text) - used)
			return ERANGE;
		used += (size_t)written;
	}

	/* The file. */
	error = session_open_directory(installation->root_fd, SESSION_RECORDS, 1, &records);
	if (error != 0)
		return error;
	error = session_write_new(records, "units", (const uint8_t *)text, used, 0644);
	close(records);
	if (error != 0)
		return error;

	/* Succeeded: the record is written. */
	return 0;
}

/* Puts the boot files of the root (the kernel and the logo) at their places. */
static int
session_install_boot_files(
	struct session_installation *installation)
{
	const struct installer_index *index;
	const char *place;
	const char *leaf;
	char directory_path[SESSION_PATH_MAX];
	size_t length;
	unsigned i;
	int directory;
	int differs;
	int error;

	/* Each boot file of the root (the loader is the ESP's). */
	index = installation->session->index;
	for (i = 0; i < index->boot_count; i++) {
		place = index->boot[i].place;
		differs = strncmp(place, "esp:", 4U);
		if (differs == 0)
			continue;
		while (*place == '/')
			place++;
		leaf = strrchr(place, '/');
		if (leaf == NULL || leaf[1] == '\0')
			return EINVAL;
		length = (size_t)(leaf - place);
		if (length >= sizeof(directory_path))
			return ENAMETOOLONG;
		memcpy(directory_path, place, length);
		directory_path[length] = '\0';
		leaf++;

		/* Its directory, then the copy. */
		error = session_open_directory(installation->root_fd, directory_path, 1, &directory);
		if (error != 0)
			return error;
		error = session_copy_boot(&index->boot[i], directory, leaf);
		close(directory);
		if (error != 0)
			return error;
	}

	/* Succeeded: the kernel and the logo are on the root. */
	return 0;
}

/*
 * Step 7: the installed system's settings: its user and both
 * passwords, the automatic login, the hostname, sshd kept from root
 * logins, and the swap file when there is no swap partition.
 */
static int
session_install_settings(
	struct session_installation *installation)
{
	int canceled;
	int error;

	/* The accounts, then the host. */
	error = target_accounts(installation->root_fd, &installation->request->install);
	if (error != 0)
		return error;
	error = target_hostname_set(installation->root_fd, installation->request->install.hostname);
	if (error != 0)
		return error;
	error = target_sshd_set(installation->root_fd);
	if (error != 0)
		return error;

	/* The swap file. */
	if (!installation->has_swap_partition) {
		error = session_install_swapfile(installation);
		if (error != 0)
			return error;
	}

	/* Cancelled: taken at the end of the step. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: the system is set up. */
	return 0;
}

/*
 * Makes the swap file: zeros written whole (no hole), mkswap,
 * then a trial swapon and swapoff, each limited in time, for the kernel
 * takes a file of at most so many extents.  A file the kernel refuses is
 * removed and the system has no swap; one the live system will not give
 * back stops the installation.
 */
static int
session_install_swapfile(
	struct session_installation *installation)
{
	char *argument[3];
	uint8_t *zero;
	uint64_t written;
	size_t piece;
	ssize_t count;
	int status;
	int ended;
	int fd;
	int error;

	/* The zeros. */
	zero = calloc(1, SESSION_CLEAR_BYTES);
	if (zero == NULL)
		return ENOMEM;

	/* The file, new, root's alone, written whole. */
	fd = openat(installation->root_fd, SESSION_SWAPFILE, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		error = errno;
		free(zero);
		return error;
	}

	/* Writes the swap file out as zeros. */
	error = 0;
	written = 0;
	while (written < installation->swapfile_bytes) {
		piece = SESSION_CLEAR_BYTES;
		if (installation->swapfile_bytes - written < piece)
			piece = (size_t)(installation->swapfile_bytes - written);
		count = write(fd, zero, piece);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			error = errno;
			break;
		}

		/* Refuses a write that wrote nothing. */
		if (count == 0) {
			error = EIO;
			break;
		}

		/* Counts the bytes written so far. */
		written += (uint64_t)count;
	}

	/* Writes the swap file out to the device. */
	if (error == 0) {
		ended = fsync(fd);
		if (ended != 0)
			error = errno;
	}

	/* Closes the swap file, and reports a failed write. */
	ended = close(fd);
	if (ended != 0 && error == 0)
		error = errno;
	free(zero);
	if (error != 0)
		return error;

	/* Its header. */
	argument[0] = SESSION_MKSWAP;
	argument[1] = SESSION_SWAPFILE_PATH;
	argument[2] = NULL;
	error = session_run(argument, NULL, NULL, 0, 0, &status);
	if (error != 0)
		return error;
	if (status != 0)
		return EIO;

	/* The trial: a file the kernel does not take is no swap file. */
	argument[0] = SESSION_SWAPON;
	error = session_run(argument, NULL, NULL, 0, SESSION_SWAP_SECONDS, &status);
	if (error == ETIMEDOUT)
		return ETIMEDOUT;
	if (error != 0 || status != 0) {
		syslog(LOG_NOTICE, "the swap file is refused (%d, %d): none", error, status);
		(void)unlinkat(installation->root_fd, SESSION_SWAPFILE, 0);
		return 0;
	}

	/* Stops using the swap file after the trial. */
	argument[0] = SESSION_SWAPOFF;
	error = session_run(argument, NULL, NULL, 0, SESSION_SWAP_SECONDS, &status);
	if (error != 0 || status != 0) {
		session_say(installation->session, "STATE swapfile-in-use\n");
		return EBUSY;
	}

	/* The line of /etc/fstab. */
	error = target_fstab_set(installation->root_fd);
	if (error != 0)
		return error;
	installation->swapfile_made = 1;

	/* Succeeded: the swap file is there. */
	return 0;
}

/* Step 8: the root written out and unmounted (an unmount ends the journal clean). */
static int
session_install_release_root(
	struct session_installation *installation)
{
	int canceled;
	int error;

	/* The root's descriptor closed, everything written, the unmount. */
	close(installation->root_fd);
	installation->root_fd = -1;
	sync();
	error = unmount(SESSION_TARGET, 0);
	if (error != 0)
		return errno;
	installation->root_mounted = 0;
	sync();

	/* Cancelled: taken at the end of the step. */
	canceled = session_look_for_cancel(installation->session);
	if (canceled)
		return ECANCELED;

	/* Succeeded: the root is whole on the disk. */
	return 0;
}

/*
 * Step 9: the ESP published: the loader at the firmware's
 * default place unless another system's loader is there (then
 * EFI/ZEDBSD), and zedbsd.cfg last, written under another name and
 * renamed.
 */
static int
session_install_esp(
	struct session_installation *installation)
{
	const struct installer_index *index;
	const struct index_boot *loader;
	struct kern_block_info info;
	struct mount_args arguments;
	struct plan_cfg cfg;
	struct stat status;
	char text[PLAN_CFG_MAX];
	const char *leaf;
	const char *place;
	size_t length;
	unsigned i;
	int found;
	int esp;
	int directory;
	int differs;
	int error;

	/* The loader of the index. */
	index = installation->session->index;
	loader = NULL;
	for (i = 0; i < index->boot_count; i++) {
		differs = strncmp(index->boot[i].place, "esp:", 4U);
		if (differs == 0)
			loader = &index->boot[i];
	}

	/* Refuses an index with no loader for this machine. */
	if (loader == NULL)
		return ENOENT;
	leaf = strrchr(loader->place, '/');
	if (leaf == NULL || leaf[1] == '\0')
		return EINVAL;
	leaf++;

	/* The ESP, the planned partition, mounted. */
	error = session_partition_ready(&installation->esp, &info);
	if (error != 0)
		return error;
	memset(&arguments, 0, sizeof(arguments));
	arguments.size = sizeof(arguments);
	arguments.version = KERN_MOUNT_ARGS_VERSION;
	snprintf(arguments.fspec, sizeof(arguments.fspec), "%s", info.name);
	error = mount("fat", SESSION_ESP, (int)(MNT_NOSUID | MNT_NOEXEC), &arguments);
	if (error != 0)
		return errno;
	installation->esp_mounted = 1;
	esp = open(SESSION_ESP, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (esp < 0)
		return errno;

	/* Not another zedBSD's ESP. */
	error = 0;
	found = fstatat(esp, SESSION_CFG, &status, AT_SYMLINK_NOFOLLOW);
	if (found == 0)
		error = EEXIST;
	found = fstatat(esp, SESSION_LOADER_OWN "/BOOTX64.EFI", &status, AT_SYMLINK_NOFOLLOW);
	if (found == 0)
		error = EEXIST;
	if (error != 0) {
		close(esp);
		return error;
	}

	/* The loader's place: the firmware's default, unless another system's loader is there. */
	found = fstatat(esp, SESSION_LOADER_DEFAULT "/BOOTX64.EFI", &status, AT_SYMLINK_NOFOLLOW);
	if (found == 0)
		installation->loader_own = 1;
	place = SESSION_LOADER_DEFAULT;
	if (installation->loader_own)
		place = SESSION_LOADER_OWN;
	snprintf(installation->loader_place, sizeof(installation->loader_place), "%s/%s", place, leaf);

	/* The loader. */
	error = session_open_directory(esp, place, 1, &directory);
	if (error == 0) {
		error = session_copy_boot(loader, directory, leaf);
		close(directory);
	}

	/* Closes the ESP and reports a failed write. */
	if (error != 0) {
		close(esp);
		return error;
	}

	/* The configuration, last. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.root_partuuid = installation->root.partuuid;
	cfg.swap_partuuid = NULL;
	if (installation->has_swap_partition)
		cfg.swap_partuuid = installation->swap.partuuid;
	cfg.graphical = installation->graphical;
	error = plan_cfg_text(&cfg, text, sizeof(text), &length);
	if (error == 0)
		error = session_write_new(esp, SESSION_CFG, (const uint8_t *)text, length, 0644);
	close(esp);
	if (error != 0)
		return error;

	/* Succeeded: the ESP starts the installed system. */
	return 0;
}

/* Step 10: the ESP written out and unmounted. */
static int
session_install_release_esp(
	struct session_installation *installation)
{
	int error;

	/* Everything written, the unmount. */
	sync();
	error = unmount(SESSION_ESP, 0);
	if (error != 0)
		return errno;
	installation->esp_mounted = 0;
	sync();

	/* Succeeded: the installation is on the disk. */
	return 0;
}

/*
 * Makes the firmware's boot entry, named after the system, for the loader
 * on the ESP, at the end of the boot order (efiboot uses an entry with
 * the same device path again).  Says "STATE boot-entry XXXX" with its
 * number, or "STATE boot-entry none" when the firmware's variables could
 * not be written: a BIOS boot, or a firmware that refused.  Then the last
 * page says to choose the disk in the firmware's boot menu.
 */
static void
session_install_boot_entry(
	struct session_installation *installation)
{
	char *argument[8];
	char disk[SESSION_PATH_MAX];
	char slot[16];
	char loader[SESSION_PATH_MAX + 1U];
	char output[64];
	char number[8];
	int matched;
	int status;
	int error;

	/* The command line: efiboot create, the label, the disk, the ESP's slot and the loader's path. */
	snprintf(disk, sizeof(disk), "/dev/%s", installation->request->disk.name);
	snprintf(slot, sizeof(slot), "%u", installation->esp_slot);
	snprintf(loader, sizeof(loader), "/%s", installation->loader_place);
	argument[0] = SESSION_EFIBOOT;
	argument[1] = "create";
	argument[2] = "-l";
	argument[3] = ZEDBSD_OS_NAME;
	argument[4] = disk;
	argument[5] = slot;
	argument[6] = loader;
	argument[7] = NULL;

	/* Runs it, keeping the line that names the entry. */
	number[0] = '\0';
	error = session_run(argument, NULL, output, sizeof(output), SESSION_EFIBOOT_SECONDS, &status);
	if (error == 0 && status == 0) {
		matched = sscanf(output, "Boot%4[0-9A-F]", number);
		if (matched != 1)
			number[0] = '\0';
	}

	/* Tells the installer there is no entry. */
	if (number[0] == '\0') {
		session_say(installation->session, "STATE boot-entry none\n");
		return;
	}

	/* Succeeded: tells the installer the entry's number. */
	session_say(installation->session, "STATE boot-entry %s\n", number);
}

/* Leaves nothing of a failed installation open or mounted. */
static void
session_install_cleanup(
	struct session_installation *installation)
{
	int failed;

	/* A unit's list of files being written. */
	if (installation->files != NULL) {
		(void)fclose(installation->files);
		installation->files = NULL;
	}

	/* The root. */
	if (installation->root_fd >= 0) {
		close(installation->root_fd);
		installation->root_fd = -1;
	}

	/* Writes out and unmounts the new root. */
	if (installation->root_mounted) {
		sync();
		failed = unmount(SESSION_TARGET, 0);
		if (failed == 0)
			installation->root_mounted = 0;
		else
			session_say(installation->session, "STATE root-still-mounted\n");
	}

	/* The ESP. */
	if (installation->esp_mounted) {
		sync();
		failed = unmount(SESSION_ESP, 0);
		if (failed == 0)
			installation->esp_mounted = 0;
		else
			session_say(installation->session, "STATE esp-still-mounted\n");
	}
}
