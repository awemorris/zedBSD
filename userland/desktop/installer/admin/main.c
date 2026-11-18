/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * install-admin: the installer's helper, set-user-ID root (mode 4550,
 * owner root, group wheel), only in the installer's image.  It asks for
 * no password: a start of the installer's image counts as a start as root.
 * What keeps it out of every other system is that only that image has it,
 * and that it does nothing unless /opt/installer/index is the index it
 * was built with.  Before anything it checks that its caller is in the
 * wheel group and is the user the image logs in automatically (or root),
 * then becomes root wholly -- so the caller can no longer stop it or its
 * children with a signal -- and takes /run/install-admin/lock.
 *
 *   install-admin probe     the disks, read (probe.h)
 *   install-admin session   requests read from the input (plan.h, session.h)
 */

#include "index.h"
#include "plan.h"
#include "probe.h"
#include "session.h"

#include "index-sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

/* The helper's own directory, its lock and its mount points. */
#define ADMIN_RUN_DIRECTORY	"/run/install-admin"
#define ADMIN_LOCK		ADMIN_RUN_DIRECTORY "/lock"
#define ADMIN_TARGET		ADMIN_RUN_DIRECTORY "/target"
#define ADMIN_ESP		ADMIN_RUN_DIRECTORY "/esp"

/* The index the helper was built with, and the user the image logs in. */
#define ADMIN_INDEX		"/opt/installer/index"
#define ADMIN_AUTOLOGIN		"/etc/keiland/autologin"

/* The longest name the automatic login file may give. */
#define ADMIN_NAME_MAX		32U

/* The exit statuses. */
#define ADMIN_EXIT_OK		0
#define ADMIN_EXIT_FAILED	1
#define ADMIN_EXIT_USAGE	2
#define ADMIN_EXIT_REFUSED	3
#define ADMIN_EXIT_BUSY		4

/*
 * The index, read once and checked; the session's requests are checked
 * against it.  Set before the requests are read and never changed.
 */
static char admin_index_text[INDEX_BYTES_MAX];
static struct installer_index admin_index;

/* The bytes of the session's requests received and not yet taken. */
static char admin_request_text[PLAN_REQUEST_MAX];

static void admin_standard_descriptors(void);
static int admin_caller_allowed(uid_t real);
static int admin_in_wheel(void);
static int admin_index_read(void);
static int admin_become_root(void);
static int admin_directory(const char *path);
static int admin_lock(int *fd);
static int admin_session(void);
static void admin_log_request(const struct plan_request *request);

/* Runs a probe or a session for the installer. */
int
main(
	int argc,
	char **argv)
{
	uid_t real;
	uid_t effective;
	int lock;
	int probe;
	int probe_order;
	int session_order;
	int error;

	/* Descriptors 0 to 2 open, whatever the caller left. */
	admin_standard_descriptors();

	/* One request, a probe or a session. */
	if (argc != 2) {
		fprintf(stderr, "usage: install-admin probe | session\n");
		return ADMIN_EXIT_USAGE;
	}

	/* Compares the word with each request the helper knows. */
	probe_order = strcmp(argv[1], "probe");
	session_order = strcmp(argv[1], "session");

	/* Selects a probe, or refuses a word that is neither request. */
	probe = 0;
	if (probe_order == 0) {
		probe = 1;
	} else if (session_order != 0) {
		fprintf(stderr, "usage: install-admin probe | session\n");
		return ADMIN_EXIT_USAGE;
	}

	/* Nothing from the environment; the log. */
	real = getuid();
	(void)clearenv();
	openlog("install-admin", LOG_PID, LOG_AUTH);

	/* Installed set-user-ID root, or it can do nothing. */
	effective = geteuid();
	if (effective != 0) {
		fprintf(stderr, "install-admin: not set-user-ID root\n");
		return ADMIN_EXIT_FAILED;
	}

	/* The caller: the image's own user in the wheel group, or root. */
	error = admin_caller_allowed(real);
	if (error != 0) {
		syslog(LOG_NOTICE, "refused uid %ld: not the installer's user", (long)real);
		fprintf(stderr, "install-admin: refused\n");
		return ADMIN_EXIT_REFUSED;
	}

	/* The installer's image, by its index. */
	error = admin_index_read();
	if (error != 0) {
		syslog(LOG_NOTICE, "refused: %s is not the installer's index (%d)", ADMIN_INDEX, error);
		fprintf(stderr, "install-admin: not the installer's image\n");
		return ADMIN_EXIT_REFUSED;
	}

	/* Root wholly, with nothing of the caller's process left. */
	error = admin_become_root();
	if (error != 0) {
		syslog(LOG_ERR, "cannot become root: %s", strerror(error));
		return ADMIN_EXIT_FAILED;
	}

	/* The helper's directory and its lock: one helper at a time. */
	error = admin_lock(&lock);
	if (error == EWOULDBLOCK || error == EAGAIN) {
		fprintf(stdout, "FAIL lock %d another install-admin runs\n", EBUSY);
		fflush(stdout);
		return ADMIN_EXIT_BUSY;
	}

	/* Reports any other failure to take the lock. */
	if (error != 0) {
		fprintf(stdout, "FAIL lock %d %s\n", error, strerror(error));
		fflush(stdout);
		return ADMIN_EXIT_FAILED;
	}

	/* A probe answers and ends. */
	if (probe) {
		syslog(LOG_INFO, "probe by uid %ld", (long)real);
		error = probe_report(stdout);
		close(lock);
		if (error != 0)
			return ADMIN_EXIT_FAILED;
		return ADMIN_EXIT_OK;
	}

	/* A session runs until the installer ends it. */
	syslog(LOG_INFO, "session by uid %ld", (long)real);
	error = admin_session();
	close(lock);
	if (error != 0)
		return ADMIN_EXIT_FAILED;

	/* Succeeded: the session ended as asked. */
	return ADMIN_EXIT_OK;
}

/* Opens /dev/null for each of the descriptors 0 to 2 that is closed. */
static void
admin_standard_descriptors(void)
{
	int fd;

	/* Until a descriptor above 2 comes back, every lower one is open. */
	for (;;) {
		fd = open("/dev/null", O_RDWR);
		if (fd < 0)
			_exit(ADMIN_EXIT_FAILED);
		if (fd > 2) {
			close(fd);
			return;
		}
	}
}

/*
 * Reports whether the caller may use the helper (0, or EPERM): root, or
 * a member of wheel whose uid is that of the user /etc/keiland/autologin
 * names (the image's own user).
 */
static int
admin_caller_allowed(
	uid_t real)
{
	char name[ADMIN_NAME_MAX + 2U];
	struct passwd *account;
	FILE *file;
	char *line;
	size_t length;
	int in_wheel;

	/* Root may. */
	if (real == 0)
		return 0;

	/* Anyone else is in wheel. */
	in_wheel = admin_in_wheel();
	if (!in_wheel)
		return EPERM;

	/* The image's own user, by the automatic login's first line. */
	file = fopen(ADMIN_AUTOLOGIN, "r");
	if (file == NULL)
		return EPERM;
	memset(name, 0, sizeof(name));
	line = fgets(name, (int)sizeof(name), file);
	if (line == NULL) {
		fclose(file);
		return EPERM;
	}

	/* Closes the file and takes the first word of the line as the name. */
	fclose(file);
	length = strcspn(name, "\r\n \t");
	name[length] = '\0';
	if (length == 0U || length > ADMIN_NAME_MAX)
		return EPERM;

	/* The caller is that user. */
	account = getpwnam(name);
	if (account == NULL || account->pw_uid != real)
		return EPERM;

	/* The caller may. */
	return 0;
}

/* Reports whether the caller's group or one of its groups is wheel (gid 0). */
static int
admin_in_wheel(void)
{
	gid_t groups[64];
	gid_t real_group;
	gid_t effective_group;
	int count;
	int i;

	/* The primary group. */
	real_group = getgid();
	effective_group = getegid();
	if (real_group == 0 || effective_group == 0)
		return 1;

	/* The supplementary groups. */
	count = getgroups((int)(sizeof(groups) / sizeof(groups[0])), groups);
	for (i = 0; i < count; i++) {
		if (groups[i] == 0)
			return 1;
	}

	/* Not in wheel. */
	return 0;
}

/*
 * Reads /opt/installer/index whole, checks it is the index the helper was
 * built with and reads its records.  Returns 0 or an errno value
 * (EBADMSG for another index).
 */
static int
admin_index_read(void)
{
	size_t length;
	ssize_t count;
	int fd;
	int error;

	/* The file, whole. */
	fd = open(ADMIN_INDEX, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return errno;
	length = 0;
	for (;;) {
		count = read(fd, admin_index_text + length, sizeof(admin_index_text) - length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		length += (size_t)count;
		if (length == sizeof(admin_index_text))
			break;
	}

	/* Keeps the read's failure and closes the file. */
	error = 0;
	if (count < 0)
		error = errno;
	close(fd);
	if (error != 0)
		return error;
	if (length == sizeof(admin_index_text))
		return EFBIG;

	/* The index the helper was built with, then its records. */
	error = index_digest_matches(admin_index_text, length, INSTALL_ADMIN_INDEX_SHA256);
	if (error != 0)
		return error;
	error = index_parse(admin_index_text, length, &admin_index);
	if (error != 0)
		return error;

	/* Succeeded: the index is the helper's. */
	return 0;
}

/*
 * Becomes root wholly (real, effective and saved ids, the groups), in a
 * session of its own, with the default mode mask and limits, no
 * descriptor but 0 to 2 and the signals that would stop it halfway
 * ignored.  A helper the caller can signal could leave a disk half
 * written.
 */
static int
admin_become_root(void)
{
	static const gid_t wheel[1] = { 0 };
	struct rlimit limit;
	int error;

	/* The ids and the groups. */
	error = setresgid(0, 0, 0);
	if (error != 0)
		return errno;
	error = setgroups(1, wheel);
	if (error != 0)
		return errno;
	error = setresuid(0, 0, 0);
	if (error != 0)
		return errno;

	/* A session of its own: the caller's terminal and process group signal it no more. */
	(void)setsid();

	/* The mask, the descriptors and the limits a root process starts with. */
	(void)umask(022);
	(void)closefrom(3);
	limit.rlim_cur = RLIM_INFINITY;
	limit.rlim_max = RLIM_INFINITY;
	(void)setrlimit(RLIMIT_FSIZE, &limit);

	/* The signals the installer's end or a terminal would send. */
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGINT, SIG_IGN);
	(void)signal(SIGTERM, SIG_IGN);

	/* Succeeded: root, alone. */
	return 0;
}

/*
 * Makes a directory of the helper's, mode 0700, or checks the one there:
 * a directory (not a symbolic link) of root's, mode 0700.
 */
static int
admin_directory(
	const char *path)
{
	struct stat status;
	mode_t type;
	int error;

	/* Made, or already there. */
	error = mkdir(path, 0700);
	if (error != 0 && errno != EEXIST)
		return errno;

	/* Root's own directory and nothing else. */
	error = lstat(path, &status);
	if (error != 0)
		return errno;
	type = status.st_mode & S_IFMT;
	if (type != S_IFDIR || status.st_uid != 0 || (status.st_mode & 07777) != 0700)
		return EPERM;

	/* Succeeded: the directory is the helper's. */
	return 0;
}

/*
 * Makes the helper's directories and takes its lock (held until the
 * descriptor is closed).  Returns 0 with the lock's descriptor, or an
 * errno value (EWOULDBLOCK while another helper holds it).
 */
static int
admin_lock(
	int *fd)
{
	int descriptor;
	int error;

	/* The directory, then the mount points in it. */
	error = admin_directory(ADMIN_RUN_DIRECTORY);
	if (error != 0)
		return error;
	error = admin_directory(PROBE_MOUNT_POINT);
	if (error != 0)
		return error;
	error = admin_directory(ADMIN_TARGET);
	if (error != 0)
		return error;
	error = admin_directory(ADMIN_ESP);
	if (error != 0)
		return error;

	/* The lock file, never through a link. */
	descriptor = open(ADMIN_LOCK, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return errno;
	error = flock(descriptor, LOCK_EX | LOCK_NB);
	if (error != 0) {
		error = errno;
		close(descriptor);
		return error;
	}

	/* Succeeded: the lock is held. */
	*fd = descriptor;
	return 0;
}

/*
 * Reads the session's requests and carries each out, until "cancel", the
 * end of the input or a request that cannot be read.  Returns 0 for a
 * session ended by the installer, or an errno value.
 */
static int
admin_session(void)
{
	struct admin_session session;
	struct plan_request *request;
	size_t used;
	size_t consumed;
	ssize_t count;
	int error;

	/* The request, too large for the stack. */
	request = malloc(sizeof(*request));
	if (request == NULL)
		return ENOMEM;
	memset(&session, 0, sizeof(session));
	session.index = &admin_index;
	session.output = stdout;
	session.input = 0;

	/* Each request, read as it arrives. */
	used = 0;
	for (;;) {
		error = plan_request_read(admin_request_text, used, request, &consumed);
		if (error == EAGAIN) {
			count = read(0, admin_request_text + used, sizeof(admin_request_text) - used);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0) {
				/* The installer is gone: as a cancellation. */
				syslog(LOG_INFO, "session ended by the end of its input");
				error = 0;
				break;
			}

			/* Keeps the bytes that arrived and reads the request again. */
			used += (size_t)count;
			continue;
		}

		/* A request that cannot be read ends the session. */
		if (error != 0) {
			/* What follows a request that cannot be read cannot be trusted. */
			syslog(LOG_NOTICE, "session ended by a request not understood (%d)", error);
			fprintf(stdout, "FAIL request %d request not understood\n", error);
			fflush(stdout);
			break;
		}

		/* The request taken from the bytes received. */
		memmove(admin_request_text, admin_request_text + consumed, used - consumed);
		used -= consumed;
		admin_log_request(request);

		/* Carry it out. */
		if (request->verb == PLAN_CANCEL) {
			fprintf(stdout, "DONE\n");
			fflush(stdout);
			error = 0;
			break;
		} else if (request->verb == PLAN_PROBE) {
			/* A probe that fails says so, for the installer waits for its end. */
			error = probe_report(stdout);
			if (error != 0)
				fprintf(stdout, "FAIL probe %d %s\n", error, strerror(error));
			error = 0;
		} else if (request->verb == PLAN_INSTALL) {
			error = session_install(&session, request);
		} else {
			error = session_edit(&session, request);
		}

		/* Forgets the request and sends what was written. */
		plan_request_forget(request);
		fflush(stdout);
	}

	/* Nothing of a request is left behind. */
	plan_request_forget(request);
	free(request);
	memset(admin_request_text, 0, sizeof(admin_request_text));
	if (error != 0)
		return error;

	/* Succeeded: the session ended. */
	return 0;
}

/* Records a request in the log, without its passwords. */
static void
admin_log_request(
	const struct plan_request *request)
{
	/* Each kind, with what names its disk. */
	switch (request->verb) {
	case PLAN_PROBE:
		syslog(LOG_INFO, "request probe");
		break;
	case PLAN_CANCEL:
		syslog(LOG_INFO, "request cancel");
		break;
	case PLAN_INSTALL:
		syslog(
			LOG_NOTICE,
			"request install on %s (%llu) for user %s, host %s, %u units",
			request->disk.name,
			(unsigned long long)request->disk.registration,
			request->install.user,
			request->install.hostname,
			request->install.unit_count);
		break;
	default:
		syslog(
			LOG_NOTICE,
			"request edit %d on %s (%llu)",
			(int)request->verb,
			request->disk.name,
			(unsigned long long)request->disk.registration);
		break;
	}
}
