/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * installer-helper: the installer's helper, which reads and writes the
 * disks and downloads the system.  It is root's alone (mode 0500, not
 * set-user-ID): the installer, which the desktop starts as root once the
 * person has allowed it (klsudo, the admin action "installer"), starts it
 * as its child, and root may start it by hand.  It refuses to run as
 * anyone else.  It becomes root wholly in a session of its own -- so
 * nothing the installer's process group is sent stops it or its children
 * halfway -- and takes /run/installer-helper/lock.
 *
 *   installer-helper probe          the disks, read (probe.h)
 *   installer-helper releases       the published releases offered (payload.h)
 *   installer-helper download TAG   the system of a release, downloaded and checked (payload.h)
 *   installer-helper session        requests read from the input (plan.h, session.h)
 */

#include "index.h"
#include "payload.h"
#include "plan.h"
#include "probe.h"
#include "release.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
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
#define ADMIN_RUN_DIRECTORY	PAYLOAD_DIRECTORY
#define ADMIN_LOCK		ADMIN_RUN_DIRECTORY "/lock"
#define ADMIN_TARGET		ADMIN_RUN_DIRECTORY "/target"
#define ADMIN_ESP		ADMIN_RUN_DIRECTORY "/esp"

/* The exit statuses. */
#define ADMIN_EXIT_OK		0
#define ADMIN_EXIT_FAILED	1
#define ADMIN_EXIT_USAGE	2
#define ADMIN_EXIT_REFUSED	3
#define ADMIN_EXIT_BUSY		4

/* The helper's requests, by their word. */
enum admin_verb {
	ADMIN_PROBE,
	ADMIN_RELEASES,
	ADMIN_DOWNLOAD,
	ADMIN_SESSION
};

/*
 * The downloaded system: its members and its index, which the session
 * checks requests against.  Read once at the session's start and never
 * changed.
 */
static char admin_index_text[INDEX_BYTES_MAX];
static struct installer_index admin_index;
static struct payload admin_payload;

/* The bytes of the session's requests received and not yet taken. */
static char admin_request_text[PLAN_REQUEST_MAX];

static void admin_standard_descriptors(void);
static int admin_verb(int argc, char **argv, enum admin_verb *verb);
static int admin_become_root(void);
static int admin_directory(const char *path);
static int admin_lock(int *fd);
static int admin_session(void);
static void admin_log_request(const struct plan_request *request);

/* Runs a probe, a list of the releases, a download or a session for the installer. */
int
main(
	int argc,
	char **argv)
{
	enum admin_verb verb;
	uid_t effective;
	uid_t real;
	int lock;
	int error;

	/* Descriptors 0 to 2 open, whatever the caller left. */
	admin_standard_descriptors();

	/* One request. */
	error = admin_verb(argc, argv, &verb);
	if (error != 0) {
		fprintf(stderr, "usage: installer-helper probe | releases | download TAG | session\n");
		return ADMIN_EXIT_USAGE;
	}

	/* Nothing from the environment; the log. */
	real = getuid();
	effective = geteuid();
	(void)clearenv();
	openlog("installer-helper", LOG_PID, LOG_AUTH);

	/* Root's alone: started by root, never set-user-ID. */
	if (real != 0 || effective != 0) {
		syslog(LOG_NOTICE, "refused uid %ld (effective %ld): not root", (long)real, (long)effective);
		fprintf(stderr, "installer-helper: only root runs the installer's helper\n");
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
		fprintf(stdout, "FAIL lock %d another installer-helper runs\n", EBUSY);
		fflush(stdout);
		return ADMIN_EXIT_BUSY;
	}

	/* Reports any other failure to take the lock. */
	if (error != 0) {
		fprintf(stdout, "FAIL lock %d %s\n", error, strerror(error));
		fflush(stdout);
		return ADMIN_EXIT_FAILED;
	}

	/* By the request. */
	switch (verb) {
	case ADMIN_PROBE:
		syslog(LOG_INFO, "probe");
		error = probe_report(stdout);
		break;
	case ADMIN_RELEASES:
		syslog(LOG_INFO, "releases");
		error = payload_releases(stdout, 0);
		break;
	case ADMIN_DOWNLOAD:
		syslog(LOG_NOTICE, "download of %s", argv[2]);
		error = payload_download(stdout, 0, argv[2]);
		break;
	default:
		syslog(LOG_INFO, "session");
		error = admin_session();
		break;
	}

	/* The lock goes with the request's end. */
	close(lock);
	if (error != 0)
		return ADMIN_EXIT_FAILED;

	/* Succeeded: the request is done. */
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
 * Reads the request from the arguments: one word, or download and a tag.
 * Returns 0, or EINVAL for anything else.
 */
static int
admin_verb(
	int argc,
	char **argv,
	enum admin_verb *verb)
{
	int probe;
	int releases;
	int download;
	int session;
	int error;

	/* A word at least. */
	if (argc < 2)
		return EINVAL;

	/* Compares the word with each request the helper knows. */
	probe = strcmp(argv[1], "probe");
	releases = strcmp(argv[1], "releases");
	download = strcmp(argv[1], "download");
	session = strcmp(argv[1], "session");

	/* A download names a tag a release may have; the others take nothing more. */
	if (download == 0) {
		if (argc != 3)
			return EINVAL;
		error = release_tag_valid(argv[2]);
		if (error != 0)
			return error;
		*verb = ADMIN_DOWNLOAD;
		return 0;
	}
	if (argc != 2)
		return EINVAL;

	/* By the word. */
	if (probe == 0) {
		*verb = ADMIN_PROBE;
	} else if (releases == 0) {
		*verb = ADMIN_RELEASES;
	} else if (session == 0) {
		*verb = ADMIN_SESSION;
	} else {
		/* A word the helper does not know. */
		return EINVAL;
	}

	/* Succeeded: the request is known. */
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

	/* The downloaded system, when there is one: without it the disks may be edited, not installed. */
	memset(&session, 0, sizeof(session));
	error = payload_load(&admin_payload, &admin_index, admin_index_text, sizeof(admin_index_text));
	if (error == 0) {
		session.index = &admin_index;
		session.payload = &admin_payload;
	} else {
		syslog(LOG_INFO, "session without a downloaded system (%d)", error);
	}

	/* The request, too large for the stack. */
	request = malloc(sizeof(*request));
	if (request == NULL)
		return ENOMEM;
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
