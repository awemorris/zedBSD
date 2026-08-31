/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD init userland command.
 */

#include "userland/base/service/service-config.h"
#include "userland/base/service/rcconf.h"
#include "userland/base/service/zsv1-server.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define SERVICE_MAX 32
#define ARGUMENT_MAX 16

enum service_type { SERVICE_DAEMON, SERVICE_ONESHOT, SERVICE_RESPAWN };

enum service_state {
	SERVICE_STOPPED,
	SERVICE_STARTING,
	SERVICE_RUNNING,
	SERVICE_COMPLETED,
	SERVICE_FAILED,
	SERVICE_SKIPPED
};

enum init_action {
	INIT_ACTION_NONE,
	INIT_ACTION_HALT,
	INIT_ACTION_POWEROFF,
	INIT_ACTION_REBOOT
};

struct service {
	char name[64];
	char command[256];
	char arguments[512];
	char after[256];
	char
		requires[
		    256];
	enum service_type type;
	enum service_state state;
	pid_t pid;
	int enabled;
	int required;
	int restart_always;
	int restart_failure;
	int notify_fd3;
	unsigned notify_timeout;
	unsigned failures;
};

static struct service services[SERVICE_MAX];
static size_t service_count;
static volatile sig_atomic_t reload_requested;
static volatile sig_atomic_t action_requested;

enum dependency_result {
	DEPENDENCIES_WAIT,
	DEPENDENCIES_READY,
	DEPENDENCIES_SKIP
};

static void make_runtime_directories(void);
static struct rcconf_model *load_rcconf_snapshot(void);
static void set_configured_hostname(const struct rcconf_model *snapshot);
static void run_mount_all(void);
static int load_services(const struct rcconf_model *snapshot);
static int load_one_service(const char *name, const struct rcconf_model *snapshot);
static int yes(const char *value);
static int on_off(const char *value, int *result);
static int parse_seconds(const char *value, unsigned minimum, unsigned maximum, unsigned *result);
static int open_control_socket(void);
static void start_enabled_services(void);
static enum dependency_result dependencies_state(const struct service *service);
static struct service *find_service(const char *name);
static int terminal_state(enum service_state state);
static int spawn_service(struct service *service);
static int wait_for_notification(struct service *service, int descriptor);
static uint64_t monotonic_milliseconds(void);
static int valid_failure_record(const char *record);
static void reap_children(void);
static void shutdown_system(enum init_action action);
static int stop_service(struct service *service);
static int reload_policy(void);
static void handle_request(int client);
static int receive_request(int client, struct zsv1_request *request);
static int send_service(int client, const struct service *service);
static enum zsv1_service_state zsv1_state(enum service_state state);
static int send_dependencies(int client, const char *list, enum zsv1_record_type type);
static void signal_handler(int number);

/*
 * Runs the init command.
 */
int
main(
	void)
{
	int client;
	struct rcconf_model *snapshot;
	int listener;

	/* Handles a failed getpid operation. */
	if (getpid() != 1) {
		fprintf(stderr, "init: must run as process 1\n");

		/* Reports operation failure. */
		return 1;
	}

	(void)signal(SIGHUP, signal_handler);
	(void)signal(SIGINT, signal_handler);
	(void)signal(SIGTERM, signal_handler);
	(void)signal(SIGCHLD, SIG_DFL);

	make_runtime_directories();
	snapshot = load_rcconf_snapshot();

	/* Handles the snapshot availability. */
	if (snapshot == NULL)
		fprintf(stderr, "init: cannot load %s: %s\n", RCCONF_PATH,
			strerror(errno));
	set_configured_hostname(snapshot);
	run_mount_all();

	/* Handles a failed load services operation. */
	if (load_services(snapshot) != 0)
		fprintf(stderr,
			"init: continuing without service definitions\n");
	free(snapshot);

	listener = open_control_socket();

	/* Handles the listener condition. */
	if (listener < 0)
		fprintf(stderr, "init: control socket: %s\n", strerror(errno));

	start_enabled_services();

	printf("init: system running\n");

	/* Continue until the operation reaches a terminal state. */
	for (;;) {

		reap_children();

		/* Handles the action requested condition. */
		if (action_requested != 0)
			shutdown_system(action_requested);

		/* Handles the reload requested condition. */
		if (reload_requested) {
			reload_requested = 0;

			/* Handles a failed reload policy operation. */
			if (reload_policy() == 0) {
				/*
 * Runtime instances are deliberately preserved.
				 */
				printf("init: configuration reloaded\n");
			}
		}

		/* Handles the listener condition. */
		if (listener < 0) {
			sleep(1);
			continue;
		}

		client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

		/* Handles the client condition. */
		if (client >= 0) {
			handle_request(client);
			close(client);
			continue;
		}

		sleep(1);
	}
}

/* Supports the make runtime directories operation. */
static void
make_runtime_directories(
	void)
{
	(void)mkdir("/var", 0755);
	(void)mkdir("/run", 0755);
	(void)mkdir("/var/log", 0755);
}

/* Supports the load rcconf snapshot operation. */
static struct rcconf_model *
load_rcconf_snapshot(
	void)
{
	struct rcconf_model *snapshot;
	int error;

	snapshot = malloc(sizeof(*snapshot));

	/* Handles the snapshot availability. */
	if (snapshot == NULL)
		return NULL;

	/* Handles a failed rcconf load operation. */
	if (rcconf_load(RCCONF_PATH, snapshot) == 0)
		return snapshot;
	error = errno;
	free(snapshot);
	errno = error;

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the set configured hostname operation. */
static void
set_configured_hostname(
	const struct rcconf_model *snapshot)
{
	/* Handles a failed sethostname operation. */
	if (snapshot != NULL && snapshot->hostname[0] != '\0' &&
	    sethostname(snapshot->hostname, strlen(snapshot->hostname)) != 0)
		fprintf(stderr, "init: sethostname: %s\n", strerror(errno));
}

/* Supports the run mount all operation. */
static void
run_mount_all(
	void)
{
	pid_t child;
	int status;
	char *arguments[] = {"mount", "-a", NULL};

	child = fork();

	/* Checks the child process state. */
	if (child == 0) {
		execv("/sbin/mount", arguments);
		execv("/bin/mount", arguments);
		_exit(127);
	}

	/* Handles a failed waitpid operation. */
	if (child > 0 && waitpid(child, &status, 0) == child &&
	    (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
		fprintf(stderr, "init: mount -a failed\n");
}

/* Supports the load services operation. */
static int
load_services(
	const struct rcconf_model *snapshot)
{
	DIR *directory;
	struct dirent *entry;

	service_count = 0;

	directory = opendir("/etc/service.d");

	/* Handles the directory availability. */
	if (directory == NULL) {
		fprintf(stderr, "init: /etc/service.d: %s\n", strerror(errno));

		/* Reports operation failure. */
		return -1;
	}

	while ((entry = readdir(directory)) != NULL) {
		/* Handles the entry condition. */
		if (entry->d_name[0] == '.')
			continue;

		/* Handles a failed load one service operation. */
		if (load_one_service(entry->d_name, snapshot) != 0)
			fprintf(stderr,
				"init: invalid service definition: %s\n",
				entry->d_name);
	}

	closedir(directory);

	/* Reports successful completion. */
	return 0;
}

/* Supports the load one service operation. */
static int
load_one_service(
	const char *name,
	const struct rcconf_model *snapshot)
{
	struct service *service;
	char path[320], value[64];
	size_t after_count, requires_count;
	int enabled;

	/* Handles a failed service name valid operation. */
	if (!service_name_valid(name) || service_count == SERVICE_MAX ||
	    snprintf(path, sizeof(path), "/etc/service.d/%s", name) >=
		(int)sizeof(path))

		/* Reports operation failure. */
		return -1;

	service = &services[service_count];
	memset(service, 0, sizeof(*service));
	strcpy(service->name, name);

	/* Handles a failed assignment get operation. */
	if (assignment_get(path, "command", service->command,
			   sizeof(service->command)) != 0 ||
	    service->command[0] != '/')

		/* Reports operation failure. */
		return -1;

	(void)assignment_get(path, "arguments", service->arguments,
			     sizeof(service->arguments));

	/* Handles the reported system error. */
	if ((assignment_get(path, "after", service->after,
			    sizeof(service->after)) != 0 &&
	     errno != ENOENT) ||
	    (assignment_get(path, "requires", service->requires,
			    sizeof(service->requires)) != 0 &&
	     errno != ENOENT) ||
	    zsv1_server_dependency_lists_validate(
		service->after, service->requires, &after_count,
		&requires_count) != 0)

		/* Reports operation failure. */
		return -1;
	service->notify_timeout = 10;

	service->type = SERVICE_DAEMON;

	/* Handles a failed assignment get operation. */
	if (assignment_get(path, "type", value, sizeof(value)) == 0) {
		/* Selects the matching value. */
		if (strcmp(value, "oneshot") == 0)
			service->type = SERVICE_ONESHOT;
		else if (strcmp(value, "respawn") == 0)
			service->type = SERVICE_RESPAWN;
		else if (strcmp(value, "daemon") != 0)

			/* Reports operation failure. */
			return -1;
	}

	/* Handles a failed assignment get operation. */
	if (assignment_get(path, "required", value, sizeof(value)) == 0)
		service->required = yes(value);

	/* Handles a failed assignment get operation. */
	if (assignment_get(path, "restart", value, sizeof(value)) == 0) {
		service->restart_always = strcmp(value, "always") == 0;
		service->restart_failure = strcmp(value, "on-failure") == 0;
	}

	/* Handles a failed assignment get operation. */
	if (assignment_get(path, "notify-fd3", value, sizeof(value)) == 0 &&
	    on_off(value, &service->notify_fd3) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles a failed assignment get operation. */
	if (assignment_get(path, "notify-timeout", value, sizeof(value)) == 0 &&
	    parse_seconds(value, 1, 300, &service->notify_timeout) != 0)

		/* Reports operation failure. */
		return -1;

	/* Handles the service condition. */
	if (service->notify_fd3 && service->type == SERVICE_ONESHOT)
		return -1;

	service->enabled =
	    snapshot != NULL &&
	    rcconf_service_enabled(snapshot, name, &enabled) == 0 && enabled;
	service->state = SERVICE_STOPPED;
	service_count++;

	/* Reports successful completion. */
	return 0;
}

/* Supports the yes operation. */
static int
yes(
	const char *value)
{
	int function_result;

	/* Computes the function result. */
	function_result = value != NULL &&
	       (strcmp(value, "YES") == 0 || strcmp(value, "yes") == 0 ||
		strcmp(value, "1") == 0 || strcmp(value, "true") == 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the on off operation. */
static int
on_off(
	const char *value,
	int *result)
{
	/* Selects the matching value. */
	if (strcmp(value, "on") == 0) {
		*result = 1;
		/* Reports successful completion. */
		return 0;
	}

	/* Selects the matching value. */
	if (strcmp(value, "off") == 0) {
		*result = 0;
		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the parse seconds operation. */
static int
parse_seconds(
	const char *value,
	unsigned minimum,
	unsigned maximum,
	unsigned *result)
{
	char *end;
	unsigned long number;

	/* Handles the value availability. */
	if (value == NULL || *value == '\0')
		return -1;
	number = strtoul(value, &end, 10);

	/* Checks the current endpoint. */
	if (*end != '\0' || number < minimum || number > maximum)
		return -1;
	*result = (unsigned)number;
	/* Reports successful completion. */
	return 0;
}

/* Supports the open control socket operation. */
static int
open_control_socket(
	void)
{
	int error;
	struct sockaddr_un address;
	int descriptor;

	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, ZEDBSD_INIT_SOCKET);

	(void)unlink(address.sun_path);

	/* Handles a failed bind operation. */
	if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
		0 ||
	    chmod(address.sun_path, 0600) != 0 || listen(descriptor, 8) != 0) {
				error = errno;

		close(descriptor);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);

	/* Returns the computed result. */
	return descriptor;
}

/* Supports the start enabled services operation. */
static void
start_enabled_services(
	void)
{
	struct service *service;
	enum dependency_result dependencies;
	size_t pass, index;

	/* Process each remaining element. */
	for (pass = 0; pass < service_count; pass++) {
		/* Process each remaining element. */
		for (index = 0; index < service_count; index++) {
						service = &services[index];

			/* Handles the service condition. */
			if (!service->enabled ||
			    service->state != SERVICE_STOPPED)
				continue;
			dependencies = dependencies_state(service);

			/* Handles the dependencies condition. */
			if (dependencies == DEPENDENCIES_READY)
				(void)spawn_service(service);
			else if (dependencies == DEPENDENCIES_SKIP) {
				service->state = SERVICE_SKIPPED;
				fprintf(stderr,
					"init: skipped %s: required dependency "
					"failed\n",
					service->name);
			}
		}
	}

	/* Process each remaining element. */
	for (index = 0; index < service_count; index++) {
		/* Handles the services condition. */
		if (services[index].enabled &&
		    services[index].state == SERVICE_STOPPED) {
			services[index].state = SERVICE_FAILED;
			fprintf(stderr, "init: dependency cycle: %s\n",
				services[index].name);
		}
	}
}

/* Supports the dependencies state operation. */
static enum dependency_result
dependencies_state(
	const struct service *service)
{
	struct service *dependency_local;
	struct service *dependency_local1;
	char copy[256], *name;

	/* Handles the service condition. */
	if (service->after[0] != '\0') {
		strcpy(copy, service->after);

		/* Process each element required by the operation. */
		for (name = strtok(copy, ","); name != NULL;
		     name = strtok(NULL, ",")) {

			dependency_local = find_service(name);

			/* Handles the dependency local availability. */
			if (dependency_local == NULL)
				return DEPENDENCIES_SKIP;

			/* Handles a failed terminal state operation. */
			if (dependency_local->enabled &&
			    !terminal_state(dependency_local->state))

				/* Returns the computed result. */
				return DEPENDENCIES_WAIT;
		}
	}

	/* Handles the service condition. */
	if (service->requires[0] != '\0') {
		strcpy(copy, service->requires);

		/* Process each element required by the operation. */
		for (name = strtok(copy, ","); name != NULL;
		     name = strtok(NULL, ",")) {

			dependency_local1 = find_service(name);

			/* Handles an operation failure. */
			if (dependency_local1 == NULL || !dependency_local1->enabled ||
			    dependency_local1->state == SERVICE_FAILED ||
			    dependency_local1->state == SERVICE_SKIPPED)

				/* Returns the computed result. */
				return DEPENDENCIES_SKIP;

			/* Handles the dependency local1 condition. */
			if (dependency_local1->state != SERVICE_RUNNING &&
			    dependency_local1->state != SERVICE_COMPLETED)

				/* Returns the computed result. */
				return DEPENDENCIES_WAIT;
		}
	}

	/* Returns the computed result. */
	return DEPENDENCIES_READY;
}

/* Supports the find service operation. */
static struct service *
find_service(
	const char *name)
{
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < service_count; index++) {
		/* Selects the matching value. */
		if (strcmp(services[index].name, name) == 0)
			return &services[index];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the terminal state operation. */
static int
terminal_state(
	enum service_state state)
{
	/* Returns the computed result. */
	return state == SERVICE_RUNNING || state == SERVICE_COMPLETED ||
	       state == SERVICE_FAILED || state == SERVICE_SKIPPED;
}

/* Supports the spawn service operation. */
static int
spawn_service(
	struct service *service)
{
	int error_local;
	int error_local1;
	int error_local2;
	char argument_copy[512], *argv[ARGUMENT_MAX];
	char *argument;
	int count = 1, status, notify_pipe[2] = {-1, -1};
	pid_t child;

	/* Handles the service condition. */
	if (service->state == SERVICE_RUNNING ||
	    service->state == SERVICE_STARTING) {
		errno = EBUSY;

		/* Reports operation failure. */
		return -1;
	}

	argv[0] = service->name;

	strcpy(argument_copy, service->arguments);

	/* Process each element required by the operation. */
	for (argument = strtok(argument_copy, " \t"); argument != NULL;
	     argument = strtok(NULL, " \t")) {
		/* Checks the remaining item count. */
		if (count + 1 >= ARGUMENT_MAX) {
			fprintf(stderr, "init: too many arguments for %s\n",
				service->name);
			errno = E2BIG;

			/* Reports operation failure. */
			return -1;
		}
		argv[count++] = argument;
	}

	argv[count] = NULL;

	service->state = SERVICE_STARTING;

	/* Handles a failed pipe2 operation. */
	if (service->notify_fd3 &&
	    pipe2(notify_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		service->state = SERVICE_FAILED;

		/* Reports operation failure. */
		return -1;
	}

	child = fork();

	/* Checks the child process state. */
	if (child == 0) {
		/* Handles the service condition. */
		if (service->notify_fd3) {
			close(notify_pipe[0]);

			/* Handles a failed dup2 operation. */
			if (dup2(notify_pipe[1], 3) < 0 ||
			    fcntl(3, F_SETFD, 0) != 0 ||
			    setenv("ZEDBSD_NOTIFY_FD", "3", 1) != 0)
				_exit(126);

			/* Handles the notify pipe condition. */
			if (notify_pipe[1] != 3)
				close(notify_pipe[1]);
		}
		execv(service->command, argv);
		fprintf(stderr, "init: exec %s: %s\n", service->command,
			strerror(errno));
		_exit(127);
	}

	/* Checks the child process state. */
	if (child < 0) {
				error_local = errno;

		/* Handles the notify pipe condition. */
		if (notify_pipe[0] >= 0)
			close(notify_pipe[0]);

		/* Handles the notify pipe condition. */
		if (notify_pipe[1] >= 0)
			close(notify_pipe[1]);
		service->state = SERVICE_FAILED;
		errno = error_local;

		/* Reports operation failure. */
		return -1;
	}

	service->pid = child;

	/* Handles the service condition. */
	if (service->notify_fd3) {
		close(notify_pipe[1]);

		/* Handles a failed wait for notification operation. */
		if (wait_for_notification(service, notify_pipe[0]) != 0) {
						error_local1 = errno;

			close(notify_pipe[0]);
			(void)kill(child, SIGTERM);
			(void)waitpid(child, &status, 0);
			service->pid = 0;
			service->state = SERVICE_FAILED;
			errno = error_local1;

			/* Reports operation failure. */
			return -1;
		}
		close(notify_pipe[0]);
	}

	/* Handles the service condition. */
	if (service->type != SERVICE_ONESHOT) {
		service->state = SERVICE_RUNNING;
		printf("init: started %s pid %ld\n", service->name,
		       (long)child);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child) {
				error_local2 = errno;

		service->state = SERVICE_FAILED;
		service->pid = 0;
		fprintf(stderr, "init: oneshot %s wait failed\n",
			service->name);
		errno = error_local2;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed WIFEXITED operation. */
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		service->state = SERVICE_FAILED;
		service->pid = 0;
		fprintf(stderr, "init: oneshot %s failed\n", service->name);
		errno = EIO;

		/* Reports operation failure. */
		return -1;
	}

	service->state = SERVICE_COMPLETED;
	service->pid = 0;

	/* Reports successful completion. */
	return 0;
}

/* Supports the wait for notification operation. */
static int
wait_for_notification(
	struct service *service,
	int descriptor)
{
	char *newline;
	ssize_t count;
	char record[513];
	size_t used;
	uint64_t deadline;

	used = 0;
	deadline = monotonic_milliseconds() +
			    (uint64_t)service->notify_timeout * 1000U;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Continue until the operation reaches a terminal state. */
		for (;;) {

			count = read(descriptor, record + used,
					     sizeof(record) - used - 1U);

			/* Handles the reported system error. */
			if (count < 0 && errno == EINTR)
				continue;

			/* Handles the reported system error. */
			if (count < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK))
				break;

			/* Checks the remaining item count. */
			if (count <= 0) {
				fprintf(stderr,
					"init: %s exited before readiness\n",
					service->name);
				errno = EPIPE;

				/* Reports operation failure. */
				return -1;
			}
			used += (size_t)count;

			/* Checks the current capacity usage. */
			if (used >= sizeof(record) - 1U) {
				fprintf(stderr,
					"init: %s oversized readiness record\n",
					service->name);
				errno = EOVERFLOW;

				/* Reports operation failure. */
				return -1;
			}
			record[used] = '\0';

			newline = strchr(record, '\n');

			/* Handles the newline availability. */
			if (newline == NULL)
				continue;

			/* Handles the newline condition. */
			if (newline[1] != '\0') {
				errno = EINVAL;

				/* Reports operation failure. */
				return -1;
			}
			*newline = '\0';
			/* Selects the matching value. */
			if (strcmp(record, "READY") == 0)
				return 0;

			/* Handles an operation failure. */
			if (valid_failure_record(record))
				fprintf(stderr, "init: %s: %s\n",
					service->name, record);
			else
				fprintf(stderr,
					"init: %s malformed readiness "
					"record\n",
					service->name);
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}

		/* Handles a failed monotonic milliseconds operation. */
		if (monotonic_milliseconds() >= deadline) {
			errno = ETIMEDOUT;
			fprintf(stderr, "init: %s readiness timeout\n",
				service->name);

			/* Reports operation failure. */
			return -1;
		}
		usleep(10000);
	}
}

/* Supports the monotonic milliseconds operation. */
static uint64_t
monotonic_milliseconds(
	void)
{
	struct timespec now;

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;

	/* Returns the computed result. */
	return (uint64_t)(uint32_t)now.tv_sec * 1000U +
	       (uint32_t)now.tv_nsec / 1000000U;
}

/* Supports the valid failure record operation. */
static int
valid_failure_record(
	const char *record)
{
	unsigned char character;
	const char *cursor;
	unsigned long code;

	cursor = record + 5;
	code = 0;

	/* Selects the matching prefix. */
	if (strncmp(record, "FAIL ", 5) != 0 || *cursor < '0' || *cursor > '9')
		return 0;

	/* Continue while the operation condition remains true. */
	while (*cursor >= '0' && *cursor <= '9') {
		code = code * 10U + (unsigned)(*cursor++ - '0');

		/* Handles the code condition. */
		if (code > 2147483647UL)
			return 0;
	}

	/* Handles the code condition. */
	if (code == 0 || *cursor++ != ' ' || *cursor == '\0')
		return 0;

	/* Continue while the operation condition remains true. */
	while (*cursor != '\0') {

		character = (unsigned char)*cursor++;

		/* Classifies the current input character. */
		if (character < 32U || character == 127U)
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the reap children operation. */
static void
reap_children(
	void)
{
	struct service *service;
	int success;
	size_t index;
	pid_t child;
	int status;

	/* Continue while the operation condition remains true. */
	while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
		/* Process each remaining element. */
		for (index = 0; index < service_count; index++) {
						service = &services[index];

			/* Handles the service condition. */
			if (service->pid != child)
				continue;

			success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			service->pid = 0;
			service->state =
			    success ? SERVICE_STOPPED : SERVICE_FAILED;

#if 0

			/* Checks the operation status. */
			if (WIFEXITED(status))
				fprintf(stderr, "init: %s exited status=%d\n", service->name, WEXITSTATUS(status));
			else if (WIFSIGNALED(status))
				fprintf(stderr, "init: %s killed signal=%d\n", service->name, WTERMSIG(status));
			else
				fprintf(stderr, "init: %s changed state status=%d\n", service->name, status);
#endif

			/* Handles an operation failure. */
			if (service->enabled && service->failures < 5 &&
			    (service->restart_always ||
			     (service->restart_failure && !success))) {
				service->failures++;
				sleep(1);
				(void)spawn_service(service);
			}

			break;
		}
	}
}

/* Supports the shutdown system operation. */
static void
shutdown_system(
	enum init_action action)
{
	size_t index;
	const char *action_name;
	int system_descriptor, system_action;

	index = service_count;

	printf("init: stopping services\n");

	/* Process each remaining element. */
	while (index > 0)
		(void)stop_service(&services[--index]);

	sync();
	action_name = action == INIT_ACTION_REBOOT     ? "reboot"
		      : action == INIT_ACTION_POWEROFF ? "poweroff"
						       : "halt";
	printf("init: executing system action %s\n", action_name);
	(void)fflush(stdout);

	system_action = action == INIT_ACTION_REBOOT ? ZEDBSD_SYSTEM_REBOOT
						     : ZEDBSD_SYSTEM_HALT;
	system_descriptor = open("/dev/system", O_RDONLY);

	/* Handles a failed ioctl operation. */
	if (system_descriptor < 0 ||
	    ioctl(system_descriptor, system_action) != 0) {
		fprintf(stderr, "init: final system action failed: %s\n",
			strerror(errno));
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;)
		pause();
}

/* Supports the stop service operation. */
static int
stop_service(
	struct service *service)
{
	unsigned attempt;
	int status;

	/* Handles the service condition. */
	if (service->pid <= 0) {
		service->state = SERVICE_STOPPED;

		/* Reports successful completion. */
		return 0;
	}

	/* Handles the reported system error. */
	if (kill(service->pid, SIGTERM) != 0 && errno != ESRCH)
		return -1;

	/* Process each element required by the operation. */
	for (attempt = 0; attempt < 5; attempt++) {
		/* Handles a failed waitpid operation. */
		if (waitpid(service->pid, &status, WNOHANG) == service->pid) {
			service->pid = 0;
			service->state = SERVICE_STOPPED;

			/* Reports successful completion. */
			return 0;
		}
		sleep(1);
	}

	(void)kill(service->pid, SIGKILL);
	(void)waitpid(service->pid, &status, 0);

	service->pid = 0;
	service->state = SERVICE_STOPPED;

	/* Reports successful completion. */
	return 0;
}

/* Supports the reload policy operation. */
static int
reload_policy(
	void)
{
	int error;
	int enabled;
	struct rcconf_model *snapshot;
	size_t index;

	snapshot = load_rcconf_snapshot();

	/* Handles the snapshot availability. */
	if (snapshot == NULL) {
				error = errno;

		fprintf(stderr, "init: cannot reload %s: %s\n", RCCONF_PATH,
			strerror(error));
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Process each remaining element. */
	for (index = 0; index < service_count; index++) {

		services[index].enabled =
		    rcconf_service_enabled(snapshot, services[index].name,
					   &enabled) == 0 &&
		    enabled;
	}
	free(snapshot);

	/* Reports successful completion. */
	return 0;
}

/* Supports the handle request operation. */
static void
handle_request(
	int client)
{
	enum init_action action;
	struct zsv1_request request;
	struct service *service;
	size_t index;
	int error;

	/* Handles a failed receive request operation. */
	if (receive_request(client, &request) != 0) {
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO,
		    error == EPROTONOSUPPORT ? "unknown-version"
		    : error == EOVERFLOW || error == EMSGSIZE
			? "request-too-long"
		    : error == ETIMEDOUT ? "request-timeout"
					 : "malformed-request");

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request.command == ZSV1_COMMAND_LIST) {
		/* Process each remaining element. */
		for (index = 0; index < service_count; index++) {
			/* Handles a failed send service operation. */
			if (send_service(client, &services[index]) != 0)
				return;
		}
		(void)zsv1_server_send_end_fd(client);

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request.command == ZSV1_COMMAND_RELOAD) {
		/* Handles a failed reload policy operation. */
		if (reload_policy() == 0)
			(void)zsv1_server_send_ok_end_fd(client, "reloaded");
		else {
			error = errno;
			(void)zsv1_server_send_error_end_fd(
			    client, error > 0 ? error : EIO, "reload-failed");
		}

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request.command == ZSV1_COMMAND_HALT ||
	    request.command == ZSV1_COMMAND_POWEROFF ||
	    request.command == ZSV1_COMMAND_REBOOT) {
				action = request.command == ZSV1_COMMAND_REBOOT ? INIT_ACTION_REBOOT
		    : request.command == ZSV1_COMMAND_POWEROFF
			? INIT_ACTION_POWEROFF
			: INIT_ACTION_HALT;

		/* Handles a failed zsv1 server send ok end fd operation. */
		if (zsv1_server_send_ok_end_fd(client, "scheduled") == 0)
			action_requested = action;

		/* Returns the computed result. */
		return;
	}

	service = find_service(request.service);

	/* Handles the service availability. */
	if (service == NULL) {
		(void)zsv1_server_send_error_end_fd(client, ENOENT,
						    "unknown-service");

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request.command == ZSV1_COMMAND_SHOW) {
		/* Handles a failed send service operation. */
		if (send_service(client, service) != 0 ||
		    send_dependencies(client, service->after,
				      ZSV1_RECORD_AFTER) != 0 ||
		    send_dependencies(client, service->requires,
				      ZSV1_RECORD_REQUIRES) != 0)

			/* Returns the computed result. */
			return;
		(void)zsv1_server_send_end_fd(client);

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request.command == ZSV1_COMMAND_START) {
		/* Handles a failed spawn service operation. */
		if (spawn_service(service) == 0) {
			(void)zsv1_server_send_ok_end_fd(client, "started");

			/* Returns the computed result. */
			return;
		}
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO, "start-failed");

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request.command == ZSV1_COMMAND_STOP) {
		/* Handles a failed stop service operation. */
		if (stop_service(service) == 0) {
			(void)zsv1_server_send_ok_end_fd(client, "stopped");

			/* Returns the computed result. */
			return;
		}
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO, "stop-failed");

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request.command == ZSV1_COMMAND_RESTART) {
		/* Handles a failed stop service operation. */
		if (stop_service(service) == 0 && spawn_service(service) == 0) {
			(void)zsv1_server_send_ok_end_fd(client, "restarted");

			/* Returns the computed result. */
			return;
		}
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO, "restart-failed");

		/* Returns the computed result. */
		return;
	}

	(void)zsv1_server_send_error_end_fd(client, EINVAL, "unknown-command");
}

/* Supports the receive request operation. */
static int
receive_request(
	int client,
	struct zsv1_request *request)
{
	int function_result;
	struct timespec deadline;

	/* Handles a failed clock gettime operation. */
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
		return -1;
	deadline.tv_sec += 5;

	/* Obtains the zsv1 server receive fd result. */
	function_result = zsv1_server_receive_fd(client, request, &deadline);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the send service operation. */
static int
send_service(
	int client,
	const struct service *service)
{
	int function_result;
	struct zsv1_record record;

	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_SERVICE;
	strcpy(record.service.name, service->name);
	record.service.state = zsv1_state(service->state);
	record.service.enabled = service->enabled;
	record.service.pid = service->pid;

	/* Obtains the zsv1 server send record fd result. */
	function_result = zsv1_server_send_record_fd(client, &record);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the zsv1 state operation. */
static enum zsv1_service_state
zsv1_state(
	enum service_state state)
{
	/* Dispatch the current operation state. */
	switch (state) {
	case SERVICE_STOPPED:
		/* Returns the computed result. */
		return ZSV1_STATE_STOPPED;
	case SERVICE_STARTING:
		/* Returns the computed result. */
		return ZSV1_STATE_STARTING;
	case SERVICE_RUNNING:
		/* Returns the computed result. */
		return ZSV1_STATE_RUNNING;
	case SERVICE_COMPLETED:
		/* Returns the computed result. */
		return ZSV1_STATE_COMPLETED;
	case SERVICE_FAILED:
		/* Returns the computed result. */
		return ZSV1_STATE_FAILED;
	case SERVICE_SKIPPED:
		/* Returns the computed result. */
		return ZSV1_STATE_SKIPPED;
	}

	/* Returns the computed result. */
	return ZSV1_STATE_FAILED;
}

/* Supports the send dependencies operation. */
static int
send_dependencies(
	int client,
	const char *list,
	enum zsv1_record_type type)
{
	struct zsv1_record record;
	char copy[256], *name;

	/* Handles the list condition. */
	if (*list == '\0')
		return 0;
	strcpy(copy, list);

	/* Process each element required by the operation. */
	for (name = strtok(copy, ","); name != NULL; name = strtok(NULL, ",")) {

		memset(&record, 0, sizeof(record));
		record.type = type;
		strcpy(record.name, name);

		/* Handles a failed zsv1 server send record fd operation. */
		if (zsv1_server_send_record_fd(client, &record) != 0)
			return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the signal handler operation. */
static void
signal_handler(
	int number)
{
	/* Handles the number condition. */
	if (number == SIGHUP)
		reload_requested = 1;
	else if (number == SIGINT)
		action_requested = INIT_ACTION_REBOOT;
	else if (number == SIGTERM)
		action_requested = INIT_ACTION_HALT;
}
