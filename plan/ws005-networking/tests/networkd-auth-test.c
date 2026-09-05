/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Focused production-source test for networkd publication and authorization. */

#include <errno.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fixture_getgrnam_r(const char *, struct group *, char *, size_t,
			      struct group **);
static int fixture_socket(int, int, int);
static int fixture_open(const char *, int, ...);
static ssize_t fixture_read(int, void *, size_t);
static int fixture_bind(int, const struct sockaddr *, socklen_t);
static int fixture_listen(int, int);
static int fixture_close(int);
static int fixture_lstat(const char *, struct stat *);
static int fixture_unlink(const char *);
static int fixture_lchown(const char *, uid_t, gid_t);
static int fixture_chmod(const char *, mode_t);
static mode_t fixture_umask(mode_t);
static int fixture_getsockopt(int, int, int, void *, socklen_t *);
static int fixture_fputs(const char *, FILE *);

#define getgrnam_r fixture_getgrnam_r
#define open fixture_open
#define read fixture_read
#define socket fixture_socket
#define bind fixture_bind
#define listen fixture_listen
#define close fixture_close
#define lstat fixture_lstat
#define unlink fixture_unlink
#define lchown fixture_lchown
#define chmod fixture_chmod
#define umask fixture_umask
#define getsockopt fixture_getsockopt
#define fputs fixture_fputs
#define main networkd_program_main
#include "userland/base/networkd/main.c"
#undef main
#undef fputs
#undef getsockopt
#undef umask
#undef chmod
#undef lchown
#undef unlink
#undef lstat
#undef close
#undef listen
#undef bind
#undef socket
#undef read
#undef open
#undef getgrnam_r

enum group_case {
	GROUP_VALID,
	GROUP_MISSING,
	GROUP_WRONG_GID
};

static int fixture_errno;
static enum group_case current_group_case;
static unsigned group_lookup_count;
static const char *group_database;
static size_t group_database_cursor;
static unsigned group_database_close_count;
static unsigned socket_count;
static unsigned bind_count;
static unsigned listen_count;
static unsigned close_count;
static unsigned unlink_count;
static unsigned lstat_count;
static unsigned owner_count;
static unsigned mode_count;
static unsigned publication_sequence;
static unsigned owner_sequence;
static unsigned mode_sequence;
static unsigned verify_sequence;
static unsigned listen_sequence;
static mode_t active_umask = 0022;
static int path_exists;
static struct stat path_status;
static int fail_chmod;
static int ignore_chmod;
static int getsockopt_error;
static int getsockopt_short;
static struct zedbsd_peercred supplied_peer;
static unsigned auth_log_count;
static char auth_log[NETWORKD_AUTH_LOG_MAX];

int *
__libc_errno_location(void)
{
	return &fixture_errno;
}

static void
fail(const char *message)
{
	fprintf(stderr, "networkd-auth-test: %s\n", message);
	exit(1);
}

static void
expect(int condition, const char *message)
{
	if (!condition)
		fail(message);
}

static int
fixture_getgrnam_r(const char *name, struct group *record, char *buffer,
		   size_t size, struct group **result)
{
	static char *members[] = {NULL};

	group_lookup_count++;
	expect(record != NULL && buffer != NULL && result != NULL,
	       "group lookup storage");
	if (strcmp(name, "network") != 0)
		return ENOENT;
	if (current_group_case == GROUP_MISSING) {
		*result = NULL;
		return 0;
	}
	if (size < sizeof("network"))
		return ERANGE;
	memcpy(buffer, "network", sizeof("network"));
	record->gr_name = buffer;
	record->gr_passwd = (char *)"x";
	record->gr_gid = current_group_case == GROUP_WRONG_GID ? (gid_t)70
							      : (gid_t)69;
	record->gr_mem = members;
	*result = record;
	return 0;
}

static int
fixture_open(const char *path, int flags, ...)
{
	expect(strcmp(path, NETWORKD_GROUP_FILE) == 0, "group database path");
	expect((flags & O_ACCMODE) == O_RDONLY, "group database access mode");
	group_database_cursor = 0;
	return 71;
}

static ssize_t
fixture_read(int descriptor, void *buffer, size_t size)
{
	size_t available, count;

	expect(descriptor == 71, "group database descriptor");
	expect(buffer != NULL, "group database read buffer");
	available = strlen(group_database) - group_database_cursor;
	if (available == 0)
		return 0;
	count = available < size ? available : size;
	memcpy(buffer, group_database + group_database_cursor, count);
	group_database_cursor += count;
	return (ssize_t)count;
}

static int
fixture_socket(int domain, int type, int protocol)
{
	(void)protocol;
	expect(group_lookup_count != 0, "socket created before group lookup");
	expect(domain == AF_UNIX, "listener family");
	expect((type & SOCK_STREAM) != 0, "listener type");
	socket_count++;
	return 42;
}

static int
fixture_bind(int descriptor, const struct sockaddr *address, socklen_t length)
{
	const struct sockaddr_un *unix_address =
	    (const struct sockaddr_un *)address;

	(void)length;
	expect(descriptor == 42, "bind descriptor");
	expect(unix_address != NULL && unix_address->sun_family == AF_UNIX &&
		       strcmp(unix_address->sun_path, NETWORKD_SOCKET) == 0,
	       "bind address");
	bind_count++;
	path_exists = 1;
	memset(&path_status, 0, sizeof(path_status));
	path_status.st_dev = (dev_t)7;
	path_status.st_ino = (ino_t)99;
	path_status.st_mode = S_IFSOCK | (0777U & ~active_umask);
	path_status.st_uid = 0;
	path_status.st_gid = 0;
	return 0;
}

static int
fixture_listen(int descriptor, int backlog)
{
	expect(descriptor == 42 && backlog == 8, "listen arguments");
	expect(path_status.st_uid == 0 && path_status.st_gid == 69 &&
		       (path_status.st_mode & 07777U) == 0660U,
	       "listen before verified publication");
	listen_count++;
	listen_sequence = ++publication_sequence;
	return 0;
}

static int
fixture_close(int descriptor)
{
	if (descriptor == 71) {
		group_database_close_count++;
		return 0;
	}
	expect(descriptor == 42, "close descriptor");
	close_count++;
	return 0;
}

static int
fixture_lstat(const char *path, struct stat *status)
{
	expect(strcmp(path, NETWORKD_SOCKET) == 0, "stat path");
	lstat_count++;
	if (!path_exists) {
		errno = ENOENT;
		return -1;
	}
	*status = path_status;
	if (owner_count != 0 && mode_count != 0)
		verify_sequence = ++publication_sequence;
	return 0;
}

static int
fixture_unlink(const char *path)
{
	expect(strcmp(path, NETWORKD_SOCKET) == 0, "unlink path");
	unlink_count++;
	path_exists = 0;
	return 0;
}

static int
fixture_lchown(const char *path, uid_t uid, gid_t gid)
{
	expect(strcmp(path, NETWORKD_SOCKET) == 0 && path_exists,
	       "owner path");
	expect(uid == 0 && gid == 69, "socket owner");
	path_status.st_uid = uid;
	path_status.st_gid = gid;
	owner_count++;
	owner_sequence = ++publication_sequence;
	return 0;
}

static int
fixture_chmod(const char *path, mode_t mode)
{
	expect(strcmp(path, NETWORKD_SOCKET) == 0 && path_exists,
	       "mode path");
	mode_count++;
	mode_sequence = ++publication_sequence;
	if (fail_chmod) {
		errno = EIO;
		return -1;
	}
	if (!ignore_chmod)
		path_status.st_mode = S_IFSOCK | mode;
	return 0;
}

static mode_t
fixture_umask(mode_t mask)
{
	mode_t old = active_umask;
	active_umask = mask;
	return old;
}

static int
fixture_getsockopt(int descriptor, int level, int option, void *value,
		   socklen_t *length)
{
	expect(descriptor == 51, "peer descriptor");
	expect(level == SOL_SOCKET && option == SO_PEERCRED, "peer option");
	expect(value != NULL && length != NULL &&
		       *length == sizeof(struct zedbsd_peercred),
	       "peer input length");
	if (getsockopt_error != 0) {
		errno = getsockopt_error;
		return -1;
	}
	memcpy(value, &supplied_peer, sizeof(supplied_peer));
	*length = getsockopt_short ? sizeof(supplied_peer) - 1U
				    : sizeof(supplied_peer);
	return 0;
}

static int
fixture_fputs(const char *text, FILE *stream)
{
	size_t length;

	expect(stream == stderr, "auth log stream");
	expect(text != NULL, "auth log text");
	length = strlen(text);
	expect(length + 1U <= NETWORKD_AUTH_LOG_MAX, "auth log bound");
	expect(strstr(text, "SSID") == NULL &&
		       strstr(text, "passphrase") == NULL &&
		       strstr(text, "payload") == NULL,
	       "auth log contains payload identity");
	auth_log_count++;
	memcpy(auth_log, text, length + 1U);
	return 0;
}

static void
reset_fixture(void)
{
	fixture_errno = 0;
	current_group_case = GROUP_VALID;
	group_lookup_count = 0;
	group_database = "wheel:x:0:root\nnetwork:x:69:\n";
	group_database_cursor = 0;
	group_database_close_count = 0;
	socket_count = 0;
	bind_count = 0;
	listen_count = 0;
	close_count = 0;
	unlink_count = 0;
	lstat_count = 0;
	owner_count = 0;
	mode_count = 0;
	publication_sequence = 0;
	owner_sequence = 0;
	mode_sequence = 0;
	verify_sequence = 0;
	listen_sequence = 0;
	active_umask = 0022;
	path_exists = 0;
	memset(&path_status, 0, sizeof(path_status));
	fail_chmod = 0;
	ignore_chmod = 0;
	getsockopt_error = 0;
	getsockopt_short = 0;
	memset(&supplied_peer, 0, sizeof(supplied_peer));
	auth_log_count = 0;
	auth_log[0] = '\0';
}

static void
test_group_contract(void)
{
	gid_t gid = 0;

	reset_fixture();
	expect(resolve_network_group(&gid) == 0 && gid == 69,
	       "valid network group");
	current_group_case = GROUP_MISSING;
	expect(resolve_network_group(&gid) != 0 && errno == ENOENT,
	       "missing network group");
	current_group_case = GROUP_WRONG_GID;
	expect(resolve_network_group(&gid) != 0 && errno == EINVAL,
	       "wrong network gid");

	reset_fixture();
	group_database = "wheel:x:0:root\nnetwork:x:69:\nnetwork:x:69:\n";
	expect(resolve_network_group(&gid) != 0 && errno == EINVAL,
	       "duplicate network records");
	reset_fixture();
	group_database = "wheel:x:0:root\nnetwork:x:69:\nother:x:69:\n";
	expect(resolve_network_group(&gid) != 0 && errno == EINVAL,
	       "ambiguous network gid");
	reset_fixture();
	group_database = "wheel:x:not-a-gid:root\nnetwork:x:69:\n";
	expect(resolve_network_group(&gid) != 0 && errno == EINVAL,
	       "invalid group record");
	reset_fixture();
	group_database = "network:x:69\n";
	expect(resolve_network_group(&gid) != 0 && errno == EINVAL,
	       "truncated network record");
}

static void
test_publication_contract(void)
{
	struct networkd_listener listener;

	reset_fixture();
	current_group_case = GROUP_MISSING;
	errno = EBUSY;
	expect(open_listener(&listener) != 0 && errno == ENOENT &&
		       strcmp(listener.stage, "resolve-group") == 0,
	       "group failure stage and stale errno reset");
	expect(socket_count == 0 && bind_count == 0 && listen_count == 0,
	       "group failure created listener state");

	reset_fixture();
	path_exists = 1;
	path_status.st_mode = S_IFREG | 0600U;
	expect(open_listener(&listener) != 0 && errno == EEXIST &&
		       strcmp(listener.stage, "remove-stale") == 0,
	       "non-socket path refusal");
	expect(unlink_count == 0 && socket_count == 0 && path_exists,
	       "non-socket path was modified");

	reset_fixture();
	expect(open_listener(&listener) == 0, "valid listener publication");
	expect(listener.descriptor == 42 && listener.owns_path &&
		       strcmp(listener.stage, "ready") == 0,
	       "listener publication state");
	expect(socket_count == 1 && bind_count == 1 && listen_count == 1,
	       "listener operation counts");
	expect(active_umask == 0022, "umask restoration");
	expect(owner_sequence < mode_sequence && mode_sequence < verify_sequence &&
		       verify_sequence < listen_sequence,
	       "owner/mode/stat/listen order");
	expect(close_listener(&listener) == 0 && close_count == 1 &&
		       unlink_count == 1 && !path_exists,
	       "normal checked cleanup");

	reset_fixture();
	fail_chmod = 1;
	expect(open_listener(&listener) != 0 && errno == EIO &&
		       strcmp(listener.stage, "mode") == 0,
	       "publication failure stage");
	expect(close_count == 1 && unlink_count == 1 && !path_exists,
	       "publication rollback");

	reset_fixture();
	ignore_chmod = 1;
	expect(open_listener(&listener) != 0 && errno == EINVAL &&
		       strcmp(listener.stage, "verify") == 0,
	       "publication verification");
	expect(listen_count == 0 && close_count == 1 && unlink_count == 1,
	       "no listen or ready after verification failure");

	reset_fixture();
	expect(open_listener(&listener) == 0, "replacement setup");
	path_status.st_ino++;
	expect(close_listener(&listener) != 0 && errno == EBUSY,
	       "replacement cleanup result");
	expect(close_count == 1 && unlink_count == 0 && path_exists,
	       "replacement was not unlinked");
}

static void
test_authentication_contract(void)
{
	struct zedbsd_peercred peer;
	enum networkd_client_role role;

	reset_fixture();
	supplied_peer.pid = 123;
	supplied_peer.euid = 0;
	supplied_peer.egid = 0;
	expect(authenticate_client(51, &peer, &role) == 0 &&
		       role == NETWORKD_CLIENT_ROOT && auth_log_count == 1,
	       "root peer authentication");
	expect(strstr(auth_log, "root-all") != NULL &&
		       strstr(auth_log, "pid=123") != NULL,
	       "root audit attribution");
	expect(operation_allowed(role, "SHOW") &&
		       operation_allowed(role, "STATIC") &&
		       operation_allowed(role, "DNS") &&
		       operation_allowed(role, "RELOAD"),
	       "root operation surface");

	reset_fixture();
	supplied_peer.pid = 456;
	supplied_peer.euid = 1000;
	supplied_peer.egid = 69;
	expect(authenticate_client(51, &peer, &role) == 0 &&
		       role == NETWORKD_CLIENT_MEMBER && auth_log_count == 1,
	       "nonroot peer authentication");
	expect(operation_allowed(role, "SHOW") &&
		       !operation_allowed(role, "UP") &&
		       !operation_allowed(role, "DOWN") &&
		       !operation_allowed(role, "STATIC") &&
		       !operation_allowed(role, "DHCP") &&
		       !operation_allowed(role, "DEFAULTROUTE") &&
		       !operation_allowed(role, "DNS") &&
		       !operation_allowed(role, "RELOAD") &&
		       operation_allowed(role, "WIFI_ENABLE") &&
		       operation_allowed(role, "WIFI_DISABLE") &&
		       operation_allowed(role, "WIFI_LIST") &&
		       operation_allowed(role, "WIFI_CONNECT") &&
		       operation_allowed(role, "WIFI_DISCONNECT") &&
		       operation_allowed(role, "WIFI_PROFILES_CHANGED") &&
		       !operation_allowed(role, "WIFI_UP") &&
		       !operation_allowed(role, "FUTURE"),
	       "nonroot SHOW and global Wi-Fi surface");

	reset_fixture();
	getsockopt_error = ENOPROTOOPT;
	expect(authenticate_client(51, &peer, &role) != 0 &&
		       errno == ENOPROTOOPT && auth_log_count == 1 &&
		       strstr(auth_log, "result=denied") != NULL,
	       "missing peer credential denial");

	reset_fixture();
	getsockopt_short = 1;
	expect(authenticate_client(51, &peer, &role) != 0 && errno == EINVAL &&
		       auth_log_count == 1,
	       "short peer credential denial");
}

int
main(void)
{
	test_group_contract();
	test_publication_contract();
	test_authentication_contract();
	puts("networkd auth/publication test: PASS");
	return 0;
}
