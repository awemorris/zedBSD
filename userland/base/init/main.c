/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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

static void
signal_handler(int number)
{
	if (number == SIGHUP)
		reload_requested = 1;
	else if (number == SIGINT)
		action_requested = INIT_ACTION_REBOOT;
	else if (number == SIGTERM)
		action_requested = INIT_ACTION_HALT;
}

static int
yes(const char *value)
{
	return value != NULL &&
	       (strcmp(value, "YES") == 0 || strcmp(value, "yes") == 0 ||
		strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
}

static int
on_off(const char *value, int *result)
{
	if (strcmp(value, "on") == 0) {
		*result = 1;
		return 0;
	}
	if (strcmp(value, "off") == 0) {
		*result = 0;
		return 0;
	}
	return -1;
}

static int
parse_seconds(const char *value, unsigned minimum, unsigned maximum,
	      unsigned *result)
{
	char *end;
	unsigned long number;
	if (value == NULL || *value == '\0')
		return -1;
	number = strtoul(value, &end, 10);
	if (*end != '\0' || number < minimum || number > maximum)
		return -1;
	*result = (unsigned)number;
	return 0;
}

static void
make_runtime_directories(void)
{
	(void)mkdir("/var", 0755);
	(void)mkdir("/run", 0755);
	(void)mkdir("/var/log", 0755);
}

static void
run_mount_all(void)
{
	pid_t child = fork();
	int status;
	char *arguments[] = {"mount", "-a", NULL};

	if (child == 0) {
		execv("/sbin/mount", arguments);
		execv("/bin/mount", arguments);
		_exit(127);
	}

	if (child > 0 && waitpid(child, &status, 0) == child &&
	    (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
		fprintf(stderr, "init: mount -a failed\n");
}

static struct rcconf_model *
load_rcconf_snapshot(void)
{
	struct rcconf_model *snapshot = malloc(sizeof(*snapshot));
	int error;

	if (snapshot == NULL)
		return NULL;
	if (rcconf_load(RCCONF_PATH, snapshot) == 0)
		return snapshot;
	error = errno;
	free(snapshot);
	errno = error;
	return NULL;
}

static void
set_configured_hostname(const struct rcconf_model *snapshot)
{
	if (snapshot != NULL && snapshot->hostname[0] != '\0' &&
	    sethostname(snapshot->hostname, strlen(snapshot->hostname)) != 0)
		fprintf(stderr, "init: sethostname: %s\n", strerror(errno));
}

static int
load_one_service(const char *name, const struct rcconf_model *snapshot)
{
	struct service *service;
	char path[320], value[64];
	size_t after_count, requires_count;
	int enabled;

	if (!service_name_valid(name) || service_count == SERVICE_MAX ||
	    snprintf(path, sizeof(path), "/etc/service.d/%s", name) >=
		(int)sizeof(path))
		return -1;

	service = &services[service_count];
	memset(service, 0, sizeof(*service));
	strcpy(service->name, name);

	if (assignment_get(path, "command", service->command,
			   sizeof(service->command)) != 0 ||
	    service->command[0] != '/')
		return -1;

	(void)assignment_get(path, "arguments", service->arguments,
			     sizeof(service->arguments));
	if ((assignment_get(path, "after", service->after,
			    sizeof(service->after)) != 0 &&
	     errno != ENOENT) ||
	    (assignment_get(path, "requires", service->requires,
			    sizeof(service->requires)) != 0 &&
	     errno != ENOENT) ||
	    zsv1_server_dependency_lists_validate(
		service->after, service->requires, &after_count,
		&requires_count) != 0)
		return -1;
	service->notify_timeout = 10;

	service->type = SERVICE_DAEMON;
	if (assignment_get(path, "type", value, sizeof(value)) == 0) {
		if (strcmp(value, "oneshot") == 0)
			service->type = SERVICE_ONESHOT;
		else if (strcmp(value, "respawn") == 0)
			service->type = SERVICE_RESPAWN;
		else if (strcmp(value, "daemon") != 0)
			return -1;
	}

	if (assignment_get(path, "required", value, sizeof(value)) == 0)
		service->required = yes(value);

	if (assignment_get(path, "restart", value, sizeof(value)) == 0) {
		service->restart_always = strcmp(value, "always") == 0;
		service->restart_failure = strcmp(value, "on-failure") == 0;
	}

	if (assignment_get(path, "notify-fd3", value, sizeof(value)) == 0 &&
	    on_off(value, &service->notify_fd3) != 0)
		return -1;
	if (assignment_get(path, "notify-timeout", value, sizeof(value)) == 0 &&
	    parse_seconds(value, 1, 300, &service->notify_timeout) != 0)
		return -1;
	if (service->notify_fd3 && service->type == SERVICE_ONESHOT)
		return -1;

	service->enabled =
	    snapshot != NULL &&
	    rcconf_service_enabled(snapshot, name, &enabled) == 0 && enabled;
	service->state = SERVICE_STOPPED;
	service_count++;

	return 0;
}

static int
load_services(const struct rcconf_model *snapshot)
{
	DIR *directory;
	struct dirent *entry;

	service_count = 0;

	directory = opendir("/etc/service.d");
	if (directory == NULL) {
		fprintf(stderr, "init: /etc/service.d: %s\n", strerror(errno));
		return -1;
	}

	while ((entry = readdir(directory)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (load_one_service(entry->d_name, snapshot) != 0)
			fprintf(stderr,
				"init: invalid service definition: %s\n",
				entry->d_name);
	}

	closedir(directory);

	return 0;
}

static struct service *
find_service(const char *name)
{
	size_t index;

	for (index = 0; index < service_count; index++) {
		if (strcmp(services[index].name, name) == 0)
			return &services[index];
	}

	return NULL;
}

enum dependency_result {
	DEPENDENCIES_WAIT,
	DEPENDENCIES_READY,
	DEPENDENCIES_SKIP
};

static int
terminal_state(enum service_state state)
{
	return state == SERVICE_RUNNING || state == SERVICE_COMPLETED ||
	       state == SERVICE_FAILED || state == SERVICE_SKIPPED;
}

static enum dependency_result
dependencies_state(const struct service *service)
{
	char copy[256], *name;

	if (service->after[0] != '\0') {
		strcpy(copy, service->after);
		for (name = strtok(copy, ","); name != NULL;
		     name = strtok(NULL, ",")) {
			struct service *dependency = find_service(name);
			if (dependency == NULL)
				return DEPENDENCIES_SKIP;
			if (dependency->enabled &&
			    !terminal_state(dependency->state))
				return DEPENDENCIES_WAIT;
		}
	}
	if (service->requires[0] != '\0') {
		strcpy(copy, service->requires);
		for (name = strtok(copy, ","); name != NULL;
		     name = strtok(NULL, ",")) {
			struct service *dependency = find_service(name);
			if (dependency == NULL || !dependency->enabled ||
			    dependency->state == SERVICE_FAILED ||
			    dependency->state == SERVICE_SKIPPED)
				return DEPENDENCIES_SKIP;
			if (dependency->state != SERVICE_RUNNING &&
			    dependency->state != SERVICE_COMPLETED)
				return DEPENDENCIES_WAIT;
		}
	}
	return DEPENDENCIES_READY;
}

static uint64_t
monotonic_milliseconds(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)(uint32_t)now.tv_sec * 1000U +
	       (uint32_t)now.tv_nsec / 1000000U;
}

static int
valid_failure_record(const char *record)
{
	const char *cursor = record + 5;
	unsigned long code = 0;
	if (strncmp(record, "FAIL ", 5) != 0 || *cursor < '0' || *cursor > '9')
		return 0;
	while (*cursor >= '0' && *cursor <= '9') {
		code = code * 10U + (unsigned)(*cursor++ - '0');
		if (code > 2147483647UL)
			return 0;
	}
	if (code == 0 || *cursor++ != ' ' || *cursor == '\0')
		return 0;
	while (*cursor != '\0') {
		unsigned char character = (unsigned char)*cursor++;
		if (character < 32U || character == 127U)
			return 0;
	}
	return 1;
}

static int
wait_for_notification(struct service *service, int descriptor)
{
	char record[513];
	size_t used = 0;
	uint64_t deadline = monotonic_milliseconds() +
			    (uint64_t)service->notify_timeout * 1000U;
	for (;;) {
		for (;;) {
			ssize_t count = read(descriptor, record + used,
					     sizeof(record) - used - 1U);
			if (count < 0 && errno == EINTR)
				continue;
			if (count < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			if (count <= 0) {
				fprintf(stderr,
					"init: %s exited before readiness\n",
					service->name);
				errno = EPIPE;
				return -1;
			}
			used += (size_t)count;
			if (used >= sizeof(record) - 1U) {
				fprintf(stderr,
					"init: %s oversized readiness record\n",
					service->name);
				errno = EOVERFLOW;
				return -1;
			}
			record[used] = '\0';
			{
				char *newline = strchr(record, '\n');
				if (newline == NULL)
					continue;
				if (newline[1] != '\0') {
					errno = EINVAL;
					return -1;
				}
				*newline = '\0';
				if (strcmp(record, "READY") == 0)
					return 0;
				if (valid_failure_record(record))
					fprintf(stderr, "init: %s: %s\n",
						service->name, record);
				else
					fprintf(stderr,
						"init: %s malformed readiness "
						"record\n",
						service->name);
				errno = EINVAL;
				return -1;
			}
		}
		if (monotonic_milliseconds() >= deadline) {
			errno = ETIMEDOUT;
			fprintf(stderr, "init: %s readiness timeout\n",
				service->name);
			return -1;
		}
		usleep(10000);
	}
}

static int
spawn_service(struct service *service)
{
	char argument_copy[512], *argv[ARGUMENT_MAX];
	char *argument;
	int count = 1, status, notify_pipe[2] = {-1, -1};
	pid_t child;

	if (service->state == SERVICE_RUNNING ||
	    service->state == SERVICE_STARTING) {
		errno = EBUSY;
		return -1;
	}

	argv[0] = service->name;

	strcpy(argument_copy, service->arguments);

	for (argument = strtok(argument_copy, " \t"); argument != NULL;
	     argument = strtok(NULL, " \t")) {
		if (count + 1 >= ARGUMENT_MAX) {
			fprintf(stderr, "init: too many arguments for %s\n",
				service->name);
			errno = E2BIG;
			return -1;
		}
		argv[count++] = argument;
	}

	argv[count] = NULL;

	service->state = SERVICE_STARTING;
	if (service->notify_fd3 &&
	    pipe2(notify_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
		service->state = SERVICE_FAILED;
		return -1;
	}

	child = fork();
	if (child == 0) {
		if (service->notify_fd3) {
			close(notify_pipe[0]);
			if (dup2(notify_pipe[1], 3) < 0 ||
			    fcntl(3, F_SETFD, 0) != 0 ||
			    setenv("ZEDBSD_NOTIFY_FD", "3", 1) != 0)
				_exit(126);
			if (notify_pipe[1] != 3)
				close(notify_pipe[1]);
		}
		execv(service->command, argv);
		fprintf(stderr, "init: exec %s: %s\n", service->command,
			strerror(errno));
		_exit(127);
	}
	if (child < 0) {
		int error = errno;

		if (notify_pipe[0] >= 0)
			close(notify_pipe[0]);
		if (notify_pipe[1] >= 0)
			close(notify_pipe[1]);
		service->state = SERVICE_FAILED;
		errno = error;
		return -1;
	}

	service->pid = child;
	if (service->notify_fd3) {
		close(notify_pipe[1]);
		if (wait_for_notification(service, notify_pipe[0]) != 0) {
			int error = errno;

			close(notify_pipe[0]);
			(void)kill(child, SIGTERM);
			(void)waitpid(child, &status, 0);
			service->pid = 0;
			service->state = SERVICE_FAILED;
			errno = error;
			return -1;
		}
		close(notify_pipe[0]);
	}
	if (service->type != SERVICE_ONESHOT) {
		service->state = SERVICE_RUNNING;
		printf("init: started %s pid %ld\n", service->name,
		       (long)child);
		return 0;
	}

	if (waitpid(child, &status, 0) != child) {
		int error = errno;

		service->state = SERVICE_FAILED;
		service->pid = 0;
		fprintf(stderr, "init: oneshot %s wait failed\n",
			service->name);
		errno = error;
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		service->state = SERVICE_FAILED;
		service->pid = 0;
		fprintf(stderr, "init: oneshot %s failed\n", service->name);
		errno = EIO;
		return -1;
	}

	service->state = SERVICE_COMPLETED;
	service->pid = 0;

	return 0;
}

static void
start_enabled_services(void)
{
	size_t pass, index;

	for (pass = 0; pass < service_count; pass++) {
		for (index = 0; index < service_count; index++) {
			struct service *service = &services[index];
			enum dependency_result dependencies;

			if (!service->enabled ||
			    service->state != SERVICE_STOPPED)
				continue;
			dependencies = dependencies_state(service);
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

	for (index = 0; index < service_count; index++) {
		if (services[index].enabled &&
		    services[index].state == SERVICE_STOPPED) {
			services[index].state = SERVICE_FAILED;
			fprintf(stderr, "init: dependency cycle: %s\n",
				services[index].name);
		}
	}
}

static int
stop_service(struct service *service)
{
	unsigned attempt;
	int status;

	if (service->pid <= 0) {
		service->state = SERVICE_STOPPED;
		return 0;
	}

	if (kill(service->pid, SIGTERM) != 0 && errno != ESRCH)
		return -1;

	for (attempt = 0; attempt < 5; attempt++) {
		if (waitpid(service->pid, &status, WNOHANG) == service->pid) {
			service->pid = 0;
			service->state = SERVICE_STOPPED;
			return 0;
		}
		sleep(1);
	}

	(void)kill(service->pid, SIGKILL);
	(void)waitpid(service->pid, &status, 0);

	service->pid = 0;
	service->state = SERVICE_STOPPED;

	return 0;
}

static void
reap_children(void)
{
	pid_t child;
	int status;

	while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
		size_t index;

		for (index = 0; index < service_count; index++) {
			struct service *service = &services[index];
			int success;

			if (service->pid != child)
				continue;

			success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			service->pid = 0;
			service->state =
			    success ? SERVICE_STOPPED : SERVICE_FAILED;

#if 0
			if (WIFEXITED(status))
				fprintf(stderr, "init: %s exited status=%d\n", service->name, WEXITSTATUS(status));
			else if (WIFSIGNALED(status))
				fprintf(stderr, "init: %s killed signal=%d\n", service->name, WTERMSIG(status));
			else
				fprintf(stderr, "init: %s changed state status=%d\n", service->name, status);
#endif

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

static int
reload_policy(void)
{
	struct rcconf_model *snapshot;
	size_t index;

	snapshot = load_rcconf_snapshot();
	if (snapshot == NULL) {
		int error = errno;

		fprintf(stderr, "init: cannot reload %s: %s\n", RCCONF_PATH,
			strerror(error));
		errno = error;
		return -1;
	}

	for (index = 0; index < service_count; index++) {
		int enabled;

		services[index].enabled =
		    rcconf_service_enabled(snapshot, services[index].name,
					   &enabled) == 0 &&
		    enabled;
	}
	free(snapshot);
	return 0;
}

static enum zsv1_service_state
zsv1_state(enum service_state state)
{
	switch (state) {
	case SERVICE_STOPPED:
		return ZSV1_STATE_STOPPED;
	case SERVICE_STARTING:
		return ZSV1_STATE_STARTING;
	case SERVICE_RUNNING:
		return ZSV1_STATE_RUNNING;
	case SERVICE_COMPLETED:
		return ZSV1_STATE_COMPLETED;
	case SERVICE_FAILED:
		return ZSV1_STATE_FAILED;
	case SERVICE_SKIPPED:
		return ZSV1_STATE_SKIPPED;
	}
	return ZSV1_STATE_FAILED;
}

static int
receive_request(int client, struct zsv1_request *request)
{
	struct timespec deadline;

	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
		return -1;
	deadline.tv_sec += 5;
	return zsv1_server_receive_fd(client, request, &deadline);
}

static int
send_service(int client, const struct service *service)
{
	struct zsv1_record record;

	memset(&record, 0, sizeof(record));
	record.type = ZSV1_RECORD_SERVICE;
	strcpy(record.service.name, service->name);
	record.service.state = zsv1_state(service->state);
	record.service.enabled = service->enabled;
	record.service.pid = service->pid;
	return zsv1_server_send_record_fd(client, &record);
}

static int
send_dependencies(int client, const char *list, enum zsv1_record_type type)
{
	char copy[256], *name;

	if (*list == '\0')
		return 0;
	strcpy(copy, list);
	for (name = strtok(copy, ","); name != NULL; name = strtok(NULL, ",")) {
		struct zsv1_record record;

		memset(&record, 0, sizeof(record));
		record.type = type;
		strcpy(record.name, name);
		if (zsv1_server_send_record_fd(client, &record) != 0)
			return -1;
	}
	return 0;
}

static void
handle_request(int client)
{
	struct zsv1_request request;
	struct service *service;
	size_t index;
	int error;

	if (receive_request(client, &request) != 0) {
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO,
		    error == EPROTONOSUPPORT ? "unknown-version"
		    : error == EOVERFLOW || error == EMSGSIZE
			? "request-too-long"
		    : error == ETIMEDOUT ? "request-timeout"
					 : "malformed-request");
		return;
	}

	if (request.command == ZSV1_COMMAND_LIST) {
		for (index = 0; index < service_count; index++) {
			if (send_service(client, &services[index]) != 0)
				return;
		}
		(void)zsv1_server_send_end_fd(client);
		return;
	}

	if (request.command == ZSV1_COMMAND_RELOAD) {
		if (reload_policy() == 0)
			(void)zsv1_server_send_ok_end_fd(client, "reloaded");
		else {
			error = errno;
			(void)zsv1_server_send_error_end_fd(
			    client, error > 0 ? error : EIO, "reload-failed");
		}
		return;
	}

	if (request.command == ZSV1_COMMAND_HALT ||
	    request.command == ZSV1_COMMAND_POWEROFF ||
	    request.command == ZSV1_COMMAND_REBOOT) {
		enum init_action action =
		    request.command == ZSV1_COMMAND_REBOOT ? INIT_ACTION_REBOOT
		    : request.command == ZSV1_COMMAND_POWEROFF
			? INIT_ACTION_POWEROFF
			: INIT_ACTION_HALT;
		if (zsv1_server_send_ok_end_fd(client, "scheduled") == 0)
			action_requested = action;
		return;
	}

	service = find_service(request.service);
	if (service == NULL) {
		(void)zsv1_server_send_error_end_fd(client, ENOENT,
						    "unknown-service");
		return;
	}

	if (request.command == ZSV1_COMMAND_SHOW) {
		if (send_service(client, service) != 0 ||
		    send_dependencies(client, service->after,
				      ZSV1_RECORD_AFTER) != 0 ||
		    send_dependencies(client, service->requires,
				      ZSV1_RECORD_REQUIRES) != 0)
			return;
		(void)zsv1_server_send_end_fd(client);
		return;
	}

	if (request.command == ZSV1_COMMAND_START) {
		if (spawn_service(service) == 0) {
			(void)zsv1_server_send_ok_end_fd(client, "started");
			return;
		}
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO, "start-failed");
		return;
	}
	if (request.command == ZSV1_COMMAND_STOP) {
		if (stop_service(service) == 0) {
			(void)zsv1_server_send_ok_end_fd(client, "stopped");
			return;
		}
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO, "stop-failed");
		return;
	}
	if (request.command == ZSV1_COMMAND_RESTART) {
		if (stop_service(service) == 0 && spawn_service(service) == 0) {
			(void)zsv1_server_send_ok_end_fd(client, "restarted");
			return;
		}
		error = errno;
		(void)zsv1_server_send_error_end_fd(
		    client, error > 0 ? error : EIO, "restart-failed");
		return;
	}

	(void)zsv1_server_send_error_end_fd(client, EINVAL, "unknown-command");
}

static int
open_control_socket(void)
{
	struct sockaddr_un address;
	int descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (descriptor < 0)
		return -1;

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, ZEDBSD_INIT_SOCKET);

	(void)unlink(address.sun_path);

	if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) !=
		0 ||
	    chmod(address.sun_path, 0600) != 0 || listen(descriptor, 8) != 0) {
		int error = errno;

		close(descriptor);
		errno = error;
		return -1;
	}

	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);

	return descriptor;
}

static void
shutdown_system(enum init_action action)
{
	size_t index = service_count;
	int system_descriptor, system_action;

	printf("init: stopping services\n");

	while (index > 0)
		(void)stop_service(&services[--index]);

	sync();

	system_action = action == INIT_ACTION_REBOOT ? ZEDBSD_SYSTEM_REBOOT
						     : ZEDBSD_SYSTEM_HALT;
	system_descriptor = open("/dev/system", O_RDONLY);
	if (system_descriptor < 0 ||
	    ioctl(system_descriptor, system_action) != 0) {
		fprintf(stderr, "init: final system action failed: %s\n",
			strerror(errno));
	}

	for (;;)
		pause();
}

int
main(void)
{
	struct rcconf_model *snapshot;
	int listener;

	if (getpid() != 1) {
		fprintf(stderr, "init: must run as process 1\n");
		return 1;
	}

	(void)signal(SIGHUP, signal_handler);
	(void)signal(SIGINT, signal_handler);
	(void)signal(SIGTERM, signal_handler);
	(void)signal(SIGCHLD, SIG_DFL);

	make_runtime_directories();
	snapshot = load_rcconf_snapshot();
	if (snapshot == NULL)
		fprintf(stderr, "init: cannot load %s: %s\n", RCCONF_PATH,
			strerror(errno));
	set_configured_hostname(snapshot);
	run_mount_all();

	if (load_services(snapshot) != 0)
		fprintf(stderr,
			"init: continuing without service definitions\n");
	free(snapshot);

	listener = open_control_socket();
	if (listener < 0)
		fprintf(stderr, "init: control socket: %s\n", strerror(errno));

	start_enabled_services();

	printf("init: system running\n");
	for (;;) {
		int client;

		reap_children();

		if (action_requested != 0)
			shutdown_system(action_requested);

		if (reload_requested) {
			reload_requested = 0;
			if (reload_policy() == 0) {
				/* Runtime instances are deliberately preserved.
				 */
				printf("init: configuration reloaded\n");
			}
		}

		if (listener < 0) {
			sleep(1);
			continue;
		}

		client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
		if (client >= 0) {
			handle_request(client);
			close(client);
			continue;
		}

		sleep(1);
	}
}
