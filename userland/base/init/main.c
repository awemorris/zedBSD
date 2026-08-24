/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "userland/base/service/service-config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zedbsd/system.h>

#define SERVICE_MAX 32
#define ARGUMENT_MAX 16

enum service_type {
	SERVICE_DAEMON,
	SERVICE_ONESHOT,
	SERVICE_RESPAWN
};

enum service_state {
	SERVICE_STOPPED,
	SERVICE_STARTING,
	SERVICE_RUNNING,
	SERVICE_FAILED
};

struct service {
	char name[64];
	char command[256];
	char arguments[512];
	char after[256];
	enum service_type type;
	enum service_state state;
	pid_t pid;
	int enabled;
	int required;
	int restart_always;
	int restart_failure;
	unsigned failures;
};

static struct service services[SERVICE_MAX];
static size_t service_count;
static volatile sig_atomic_t reload_requested;
static volatile sig_atomic_t action_requested;

static void
signal_handler(
	int number)
{
	if (number == SIGHUP)
		reload_requested = 1;
	else if (number == SIGINT)
		action_requested = ZEDBSD_SYSTEM_REBOOT;
	else if (number == SIGTERM)
		action_requested = ZEDBSD_SYSTEM_HALT;
}

static int
yes(
    const char *value)
{
	return value != NULL &&
	       (strcmp(value, "YES") == 0 ||
		strcmp(value, "yes") == 0 ||
		strcmp(value, "1") == 0 ||
		strcmp(value, "true") == 0);
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

	if (child > 0 &&
	    waitpid(child, &status, 0) == child &&
	    (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
		fprintf(stderr, "init: mount -a failed\n");
}

static void
set_configured_hostname(void)
{
	char hostname[256];

	if (rcconf_get(ZEDBSD_RC_CONF, "hostname", hostname, sizeof(hostname)) == 0 &&
	    hostname[0] != '\0' &&
	    sethostname(hostname, strlen(hostname)) != 0)
		fprintf(stderr, "init: sethostname: %s\n", strerror(errno));
}

static int
load_one_service(
	const char *name)
{
	struct service *service;
	char path[320], value[64], key[80];

	if (!service_name_valid(name) ||
	    service_count == SERVICE_MAX ||
	    snprintf(path, sizeof(path), "/etc/service.d/%s", name) >= (int)sizeof(path))
		return -1;

	service = &services[service_count];
	memset(service, 0, sizeof(*service));
	strcpy(service->name, name);

	if (rcconf_get(path, "command", service->command, sizeof(service->command)) != 0 ||
	    service->command[0] != '/')
		return -1;

	(void)rcconf_get(path, "arguments", service->arguments, sizeof(service->arguments));
	(void)rcconf_get(path, "after", service->after, sizeof(service->after));

	service->type = SERVICE_DAEMON;
	if (rcconf_get(path, "type", value, sizeof(value)) == 0) {
		if (strcmp(value, "oneshot") == 0)
			service->type = SERVICE_ONESHOT;
		else if (strcmp(value, "respawn") == 0)
			service->type = SERVICE_RESPAWN;
		else if (strcmp(value, "daemon") != 0)
			return -1;
	}

	if (rcconf_get(path, "required", value, sizeof(value)) == 0)
		service->required = yes(value);

	if (rcconf_get(path, "restart", value, sizeof(value)) == 0) {
		service->restart_always = strcmp(value, "always") == 0;
		service->restart_failure = strcmp(value, "on-failure") == 0;
	}

	if (snprintf(key, sizeof(key), "%s_enable", name) >= (int)sizeof(key))
		return -1;

	service->enabled = rcconf_get(ZEDBSD_RC_CONF, key, value, sizeof(value)) == 0 && yes(value);
	service->state = SERVICE_STOPPED;
	service_count++;

	return 0;
}

static int
load_services(void)
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
		if (load_one_service(entry->d_name) != 0)
			fprintf(stderr, "init: invalid service definition: %s\n", entry->d_name);
	}

	closedir(directory);

	return 0;
}

static struct service *
find_service(
	const char *name)
{
	size_t index;

	for (index = 0; index < service_count; index++) {
		if (strcmp(services[index].name, name) == 0)
			return &services[index];
	}

	return NULL;
}

static int
dependencies_ready(
	const struct service *service)
{
	char copy[256], *name;

	if (service->after[0] == '\0')
		return 1;

	strcpy(copy, service->after);

	for (name = strtok(copy, ","); name != NULL; name = strtok(NULL, ",")) {
		struct service *dependency = find_service(name);
		if (dependency != NULL &&
		    dependency->enabled &&
		    dependency->state != SERVICE_RUNNING)
			return 0;
	}

	return 1;
}

static int
spawn_service(
	struct service *service)
{
	char argument_copy[512], *argv[ARGUMENT_MAX];
	char *argument;
	int count = 1, status;
	pid_t child;

	argv[0] = service->name;

	strcpy(argument_copy, service->arguments);

	for (argument = strtok(argument_copy, " \t");
	     argument != NULL;
	     argument = strtok(NULL, " \t")) {
		if (count + 1 >= ARGUMENT_MAX) {
			fprintf(stderr, "init: too many arguments for %s\n", service->name);
			return -1;
		}
		argv[count++] = argument;
	}

	argv[count] = NULL;

	service->state = SERVICE_STARTING;

	child = fork();
	if (child == 0) {
		execv(service->command, argv);
		fprintf(stderr, "init: exec %s: %s\n", service->command, strerror(errno));
		_exit(127);
	}
	if (child < 0) {
		service->state = SERVICE_FAILED;
		return -1;
	}

	service->pid = child;
	if (service->type != SERVICE_ONESHOT) {
		service->state = SERVICE_RUNNING;
		printf("init: started %s pid %ld\n", service->name,
		       (long)child);
		return 0;
	}

	if (waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		service->state = SERVICE_FAILED;
		service->pid = 0;
		fprintf(stderr, "init: oneshot %s failed\n", service->name);
		return -1;
	}

	service->state = SERVICE_RUNNING;
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

			if (service->enabled &&
			    service->state == SERVICE_STOPPED &&
			    dependencies_ready(service))
				(void)spawn_service(service);
		}
	}

	for (index = 0; index < service_count; index++) {
		if (services[index].enabled &&
		    services[index].state == SERVICE_STOPPED) {
			fprintf(stderr,
				"init: dependency cycle or unavailable dependency: %s\n",
				services[index].name);
		}
	}
}

static int
stop_service(
	struct service *service)
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
			service->state = success ? SERVICE_STOPPED : SERVICE_FAILED;

#if 0
			if (WIFEXITED(status))
				fprintf(stderr, "init: %s exited status=%d\n", service->name, WEXITSTATUS(status));
			else if (WIFSIGNALED(status))
				fprintf(stderr, "init: %s killed signal=%d\n", service->name, WTERMSIG(status));
			else
				fprintf(stderr, "init: %s changed state status=%d\n", service->name, status);
#endif

			if (service->enabled &&
			    service->failures < 5 &&
			    (service->restart_always || (service->restart_failure && !success))) {
				service->failures++;
				sleep(1);
				(void)spawn_service(service);
			}

			break;
		}
	}
}

static const char *
state_name(
	enum service_state state)
{
	static const char *const names[] = {
		"stopped",
		"starting",
		"running",
		"failed"
	};

	return names[state];
}

static void
reload_policy(void)
{
	size_t index;

	for (index = 0; index < service_count; index++) {
		char key[80], value[64];

		if (snprintf(key, sizeof(key), "%s_enable", services[index].name) >= (int)sizeof(key))
			continue;

		services[index].enabled = rcconf_get(ZEDBSD_RC_CONF,
						     key,
						     value,
						     sizeof(value)) == 0
			&& yes(value);
	}
}

static void
write_response(
	int client,
	const char *response)
{
	(void)write(client, response, strlen(response));
}

static void
handle_request(
	int client)
{
	char request[256], response[512], *command, *name;
	ssize_t length = read(client, request, sizeof(request) - 1);
	struct service *service;
	size_t index;

	if (length <= 0)
		return;

	request[length] = '\0';
	if ((name = strchr(request, '\n')) != NULL)
		*name = '\0';

	command = strtok(request, " \t");
	name = strtok(NULL, " \t");

	if (command == NULL) {
		write_response(client, "ERR empty request\n");
		return;
	}

	if (strcmp(command, "list") == 0) {
		for (index = 0; index < service_count; index++) {
			snprintf(response,
				 sizeof(response),
				 "%s\t%s\t%s\n",
				 services[index].name,
				 services[index].enabled ? "enabled" : "disabled",
				 state_name(services[index].state));

			write_response(client, response);
		}
		return;
	}

	if (strcmp(command, "reload") == 0 && name == NULL) {
		reload_requested = 1;
		write_response(client, "OK reload scheduled\n");
		return;
	}

	if ((strcmp(command, "halt") == 0 ||
	     strcmp(command, "poweroff") == 0 ||
	     strcmp(command, "reboot") == 0)
	    && name == NULL) {
		action_requested = strcmp(command, "reboot") == 0
				       ? ZEDBSD_SYSTEM_REBOOT
				       : ZEDBSD_SYSTEM_HALT;
		write_response(client, "OK shutdown scheduled\n");
		return;
	}

	service = name != NULL ? find_service(name) : NULL;
	if (service == NULL) {
		write_response(client, "ERR unknown service\n");
		return;
	}

	if (strcmp(command, "status") == 0) {
		snprintf(response, sizeof(response), "OK %s %s %s pid=%ld\n",
			 service->name, service->enabled ? "enabled" : "disabled",
			 state_name(service->state), (long)service->pid);
		write_response(client, response);
	} else if (strcmp(command, "start") == 0) {
		write_response(client,
			       spawn_service(service) == 0 ? "OK started\n" : "ERR start failed\n");
	} else if (strcmp(command, "stop") == 0) {
		write_response(client, stop_service(service) == 0 ? "OK stopped\n" : "ERR stop failed\n");
	} else if (strcmp(command, "restart") == 0) {
		(void)stop_service(service);
		write_response(client, spawn_service(service) == 0 ? "OK restarted\n" : "ERR restart failed\n");
	} else {
		write_response(client, "ERR unknown command\n");
	}
}

static int
open_control_socket(void)
{
	struct sockaddr_un address;
	int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (descriptor < 0)
		return -1;

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, ZEDBSD_INIT_SOCKET);

	(void)unlink(address.sun_path);

	if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    chmod(address.sun_path, 0600) != 0 ||
	    listen(descriptor, 8) != 0) {
		close(descriptor);
		return -1;
	}

	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);

	return descriptor;
}

static void
shutdown_system(
	int action)
{
	size_t index = service_count;
	int system_descriptor;

	printf("init: stopping services\n");

	while (index > 0)
		(void)stop_service(&services[--index]);

	sync();

	system_descriptor = open("/dev/system", O_RDONLY);
	if (system_descriptor < 0 || ioctl(system_descriptor, action) != 0) {
		fprintf(stderr, "init: final system action failed: %s\n", strerror(errno));
	}

	for (;;)
		pause();
}

int
main(void)
{
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
	set_configured_hostname();
	run_mount_all();

	if (load_services() != 0)
		fprintf(stderr, "init: continuing without service definitions\n");

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
			reload_policy();

			/* Runtime instances are deliberately preserved. */
			printf("init: configuration reloaded\n");
		}

		if (listener < 0) {
			sleep(1);
			continue;
		}

		client = accept(listener, NULL, NULL);
		if (client >= 0) {
			handle_request(client);
			close(client);
			continue;
		}

		sleep(1);
	}
}
