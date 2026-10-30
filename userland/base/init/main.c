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

#include "root-check.h"
#include "userland/base/service/service-config.h"
#include "userland/base/service/rcconf.h"
#include "userland/base/service/zsv1-server.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <uapi/system.h>

#define SERVICE_MAX 32
#define ARGUMENT_MAX 16

/* The record of when this boot started, readied and ended each service, for reading how long the boot took. */
#define TIMELINE_PATH "/var/log/init.log"

/* The boot parameters that choose the order of the services, as the kernel reports them. */
#define BOOT_LOGIN_SYSCTL "kern.boot.login"
#define BOOT_KMSG_SYSCTL "kern.boot.kmsg"

/* The room for one boot parameter's value, which is one short word. */
#define BOOT_PARAMETER_MAX 32

/*
 * How long a boot that shows the desktop first holds the late services back
 * for the desktop to take the display, and how often it looks.  A desktop
 * that is slower than this, or that never comes, has them start anyway.
 */
#define DESKTOP_WAIT_MS 30000U
#define DESKTOP_POLL_MS 100

/* Requests held while a oneshot service runs, and how often init looks at the socket and the service then. */
#define DEFERRED_MAX 8
#define ONESHOT_POLL_MS 100

/*
 * How long a stopping process is given: a service after SIGTERM and after
 * SIGKILL, and the processes no service owns after each of the two.  A
 * process stuck in the kernel is left behind after these, so the shutdown
 * still reaches its sync.
 */
#define STOP_GRACE_MS 5000U
#define STOP_POLL_MS 100U

/*
 * The grace of each step once the kernel told init of a thermal emergency
 * with SIGUSR2: the power must go soon, so every process gets two seconds
 * after SIGTERM and two after SIGKILL, also in a shutdown already under
 * way.
 */
#define EMERGENCY_GRACE_MS 2000U

/* The record of the thermal emergencies init was told of, one line each. */
#define EMERGENCY_PATH "/var/log/thermal-emergency"

/*
 * The process states KERN_SYSTEM_GET_PROCESS reports for a process that has
 * already exited (the kernel's PROCESS_ZOMBIE and PROCESS_DEAD).
 */
#define PROCESS_STATE_ZOMBIE 4U
#define PROCESS_STATE_DEAD 5U

/*
 * How the final system action is retried when the kernel refuses it, which
 * it does while the volumes cannot be written out.  The first retries come
 * every few seconds; after them a warning says what is wrong, and the
 * retries go on at a slower pace in case the storage recovers.  The machine
 * is never reset or halted with the data unwritten.
 */
#define ACTION_QUICK_RETRIES 12U
#define ACTION_QUICK_RETRY_SECONDS 5U
#define ACTION_SLOW_RETRY_SECONDS 60U

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
	/*
	 * The service this one stands in for (replaces=): while
	 * this one is enabled the other is not started; when this one ends and
	 * is not started again, the other is.  The graphical login's greeter
	 * replaces the console's getty this way.
	 */
	char replaces[64];
	/*
	 * Set by late=yes for a service the desktop does not need at once (the
	 * network, sound, sshd, cron).  A boot that shows the desktop first
	 * starts it only after the other services have been started, and does
	 * not hold them for it; a debug boot starts it in its usual place.
	 */
	int late;
	/*
	 * Set by desktop=yes for the service that shows the desktop (the
	 * graphical login).  While it runs, a boot that shows the desktop
	 * first starts the late services only once the desktop has taken the
	 * display (KERN_INIT_DESKTOP_SHOWN) or the wait is over.
	 */
	int desktop;
	/*
	 * Set once init has started the service in this boot.  The rounds of
	 * the boot start a service only once: one that ended while init
	 * waited for the desktop (a graphical login that gave the screen to
	 * the console) is not started again by the next round.
	 */
	int started_once;
};

static struct service services[SERVICE_MAX];
static size_t service_count;
static volatile sig_atomic_t reload_requested;
static volatile sig_atomic_t action_requested;

/*
 * Set by SIGUSR2, the kernel's word that the machine overheats: the
 * shutdown becomes a power-off with a short grace, and no later request
 * changes that.  emergency_noted says init already wrote it down.
 */
static volatile sig_atomic_t emergency_requested;
static int emergency_noted;

/* Set when the system is being stopped: a replaced service is not started then. */
static int services_stopping;

/*
 * The control socket's listener (-1 without one).  It is served while a
 * oneshot service runs too: a oneshot that asks for the power to be
 * cut, through /sbin/poweroff, waits for init's answer while init waits for it.
 */
static int control_listener = -1;

/*
 * A request that starts or stops services, received while a oneshot service
 * ran: it is carried out when init is back in its loop, as it was when such a
 * request waited in the listen queue.
 */
struct deferred_request {
	int client;
	struct zsv1_request request;
};

static struct deferred_request deferred_requests[DEFERRED_MAX];
static size_t deferred_count;

enum dependency_result {
	DEPENDENCIES_WAIT,
	DEPENDENCIES_READY,
	DEPENDENCIES_SKIP
};

/*
 * Which services one round of starting takes.  A boot that shows the desktop
 * first starts the services that are not late, then all that are left; any
 * other boot starts all of them in one round.
 */
enum start_round {
	START_ROUND_DESKTOP,
	START_ROUND_ALL
};

/*
 * Nonzero when this boot shows the desktop first: the boot parameters ask for
 * the graphical login (login=graphical) and keep the kernel's messages off the
 * screen (kmsg=quiet).  Set once before the services start.  A debug boot (the
 * messages on the console, or the console's login) leaves it 0 and starts the
 * services in the order their dependencies alone give.
 */
static int desktop_first;

/*
 * The descriptor of the boot's timeline (TIMELINE_PATH), or -1 before the
 * file systems are mounted or when the file cannot be written.  Opened once and
 * kept for init's life; the services do not inherit it.
 */
static int timeline_descriptor = -1;

/* The monotonic time init started at, the timeline's first line once the timeline is open. */
static uint64_t init_started_ms;

static void make_runtime_directories(void);
static struct rcconf_model *load_rcconf_snapshot(void);
static void set_configured_hostname(const struct rcconf_model *snapshot);
static void run_startup_command(const char *path, char *name);
static int load_services(const struct rcconf_model *snapshot);
static int load_one_service(const char *name, const struct rcconf_model *snapshot);
static void report_unknown_services(const struct rcconf_model *snapshot);
static int yes(const char *value);
static int on_off(const char *value, int *result);
static int parse_seconds(const char *value, unsigned minimum, unsigned maximum, unsigned *result);
static int open_control_socket(void);
static void start_enabled_services(void);
static void start_round(enum start_round round);
static enum dependency_result dependencies_state(const struct service *service, enum start_round round);
static enum dependency_result order_state(const struct service *service, enum start_round round);
static enum dependency_result requirements_state(const struct service *service);
static int boot_shows_desktop_first(void);
static int boot_parameter_is(const char *name, const char *expected);
static void wait_for_desktop(void);
static int desktop_running(void);
static void serve_one_request(int milliseconds);
static void timeline_open(void);
static void timeline_record(const char *event, const char *name);
static void timeline_write(uint64_t milliseconds, const char *event, const char *name);
static struct service *find_service(const char *name);
static struct service *replacer_of(const struct service *service);
static void release_replaced(const struct service *service);
static int terminal_state(enum service_state state);
static int spawn_service(struct service *service);
static int wait_for_oneshot(pid_t child, int *status);
static void serve_while_oneshot(void);
static void handle_deferred_requests(void);
static int wait_for_notification(struct service *service, int descriptor);
static uint64_t monotonic_milliseconds(void);
static int valid_failure_record(const char *record);
static void reap_children(void);
static void shutdown_system(enum init_action action);
static int stop_service(struct service *service);
static int wait_for_exit(pid_t pid, unsigned milliseconds);
static void stop_other_processes(void);
static unsigned signal_other_processes(int signal_number);
static unsigned wait_for_other_processes(unsigned milliseconds);
static void pause_milliseconds(unsigned milliseconds);
static int reload_policy(void);
static void handle_request(int client);
static void refuse_request(int client, int error);
static void dispatch_request(int client, const struct zsv1_request *request);
static int receive_request(int client, struct zsv1_request *request);
static int send_service(int client, const struct service *service);
static enum zsv1_service_state zsv1_state(enum service_state state);
static int send_dependencies(int client, const char *list, enum zsv1_record_type type);
static void signal_handler(int number);
static unsigned stop_grace_ms(void);
static void note_emergency(void);

/*
 * Runs the init command.
 */
int
main(
	void)
{
	int client;
	struct rcconf_model *snapshot;

	/* The time init started, which the timeline begins with. */
	init_started_ms = monotonic_milliseconds();

	/* Handles a failed getpid operation. */
	if (getpid() != 1) {
		fprintf(stderr, "init: must run as process 1\n");

		/* Reports operation failure. */
		return 1;
	}

	(void)signal(SIGHUP, signal_handler);
	(void)signal(SIGINT, signal_handler);
	(void)signal(SIGTERM, signal_handler);
	(void)signal(SIGUSR2, signal_handler);
	(void)signal(SIGCHLD, SIG_DFL);

	/* A root the kernel could only mount read-only is checked before anything else. */
	root_check();

	make_runtime_directories();
	snapshot = load_rcconf_snapshot();

	/* Handles the snapshot availability. */
	if (snapshot == NULL) {
		fprintf(stderr, "init: cannot load %s: %s\n", RCCONF_PATH,
			strerror(errno));
	}
	set_configured_hostname(snapshot);
	run_startup_command("/sbin/mount", "mount");
	run_startup_command("/sbin/swapon", "swapon");

	/* The timeline, now that /var/log is where it stays. */
	timeline_open();

	/* Handles a failed load services operation. */
	if (load_services(snapshot) != 0) {
		fprintf(stderr,
			"init: continuing without service definitions\n");
	}
	report_unknown_services(snapshot);
	free(snapshot);

	/* Opens the control socket. */
	control_listener = open_control_socket();

	/* Handles the listener condition. */
	if (control_listener < 0)
		fprintf(stderr, "init: control socket: %s\n", strerror(errno));

	/* The boot parameters choose whether the desktop's services come first. */
	desktop_first = boot_shows_desktop_first();

	start_enabled_services();

	printf("init: system running\n");
	timeline_record("system", "running");

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

		/* Carries out the requests held while a oneshot service ran. */
		handle_deferred_requests();

		/* Handles the listener condition. */
		if (control_listener < 0) {
			sleep(1);
			continue;
		}

		/* Answers the next request. */
		client = accept4(control_listener, NULL, NULL, SOCK_CLOEXEC);
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

	/* The mark of a desktop shown is this boot's to leave, not the last one's. */
	(void)unlink(KERN_INIT_DESKTOP_SHOWN);
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

/* Runs filesystem setup in order while retaining console recovery on failure. */
static void
run_startup_command(
	const char *path,
	char *name)
{
	pid_t child;
	pid_t waited;
	int status;
	char *arguments[] = {name, "-a", NULL};

	child = fork();
	if (child < 0) {
		fprintf(stderr, "init: %s -a: fork: %s\n", name, strerror(errno));
		return;
	}
	if (child == 0) {
		execv(path, arguments);
		fprintf(stderr, "init: exec %s: %s\n", path,
			strerror(errno));
		_exit(127);
	}

	/* Signals must not let service startup race an unfinished setup command. */
	do {
		waited = waitpid(child, &status, 0);
	} while (waited < 0 && errno == EINTR);
	if (waited != child) {
		fprintf(stderr, "init: %s -a: wait: %s\n", name, strerror(errno));
		return;
	}
	/* Says how it failed: an exit status, or the signal that killed it. */
	if (WIFSIGNALED(status))
		fprintf(stderr, "init: %s -a killed by signal %d\n", name,
			WTERMSIG(status));
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		fprintf(stderr, "init: %s -a failed (status %d)\n", name,
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
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
		if (load_one_service(entry->d_name, snapshot) != 0) {
			fprintf(stderr,
				"init: invalid service definition: %s\n",
				entry->d_name);
		}
	}

	closedir(directory);

	/* Reports successful completion. */
	return 0;
}

/*
 * Supports the report unknown services operation.
 *
 * A configuration that names a service with no definition has usually been
 * written wrongly, and saying so is more use than silence.  One written
 * down as optional is expected to be absent whenever the package that
 * carries it was left out, so that one is passed over without a word.
 */
static void
report_unknown_services(
	const struct rcconf_model *snapshot)
{
	const struct rcconf_service *configured;
	size_t index;
	size_t loaded;
	int found;

	/* Handles the snapshot availability. */
	if (snapshot == NULL)
		return;

	/* Process each element required by the operation. */
	for (index = 0; index < snapshot->service_count; index++) {
		configured = &snapshot->services[index];
		found = 0;

		/* Process each remaining element. */
		for (loaded = 0; loaded < service_count; loaded++) {
			if (strcmp(services[loaded].name,
			    configured->name) == 0) {
				found = 1;
				break;
			}
		}

		/* Says nothing about one that is there, or one that may not be. */
		if (found || configured->optional)
			continue;
		fprintf(stderr,
			"init: %s: no definition in /etc/service.d\n",
			configured->name);
	}
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
	int replaces_error;

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

	/* The service this one stands in for, when it names one. */
	replaces_error = assignment_get(path, "replaces", service->replaces,
					sizeof(service->replaces));
	if (replaces_error != 0)
		service->replaces[0] = '\0';

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

	/* A service the desktop does not need at once, which a desktop boot starts last. */
	if (assignment_get(path, "late", value, sizeof(value)) == 0)
		service->late = yes(value);

	/* The service that shows the desktop, which the late ones wait for in a desktop boot. */
	if (assignment_get(path, "desktop", value, sizeof(value)) == 0)
		service->desktop = yes(value);

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
	strcpy(address.sun_path, KERN_INIT_SOCKET);

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

/*
 * Starts the enabled services in the order their dependencies give.
 *
 * A boot that shows the desktop first starts the services that are not late
 * in a first round, so the graphical login does not wait for the network,
 * sound or sshd, and the late ones in a second round after it.
 */
static void
start_enabled_services(
	void)
{
	size_t index;

	/* The desktop's round: every service but the late ones. */
	if (desktop_first) {
		timeline_record("order", "desktop-first");
		start_round(START_ROUND_DESKTOP);
		timeline_record("round", "desktop-started");

		/* The late services wait for the desktop to take the display, so that they do not slow its start. */
		wait_for_desktop();
	} else {
		timeline_record("order", "dependencies");
	}

	/* A halt, power-off or reboot asked for meanwhile starts nothing more; the loop carries it out. */
	if (action_requested != 0)
		return;

	/* Every service still waiting, the late ones included. */
	start_round(START_ROUND_ALL);
	if (action_requested != 0)
		return;

	/* A service still waiting after every pass waits for itself. */
	for (index = 0; index < service_count; index++) {
		/* Only an enabled service that was never started is in a cycle. */
		if (!services[index].enabled)
			continue;
		if (services[index].state != SERVICE_STOPPED)
			continue;
		if (services[index].started_once)
			continue;

		/* Marks it failed, which is what a service that can never start is. */
		services[index].state = SERVICE_FAILED;
		fprintf(stderr, "init: dependency cycle: %s\n", services[index].name);
		timeline_record("cycle", services[index].name);
	}
}

/*
 * Starts each service of one round whose dependencies are met, pass after
 * pass, so that a service waiting for another starts in the pass after it.
 */
static void
start_round(
	enum start_round round)
{
	struct service *service;
	struct service *replacer;
	enum dependency_result dependencies;
	size_t pass;
	size_t index;

	/* As many passes as there are services: each pass starts at least one, or none is left that can start. */
	for (pass = 0; pass < service_count; pass++) {
		/* Each service not started yet. */
		for (index = 0; index < service_count; index++) {
			service = &services[index];

			/* A halt, power-off or reboot asked for meanwhile starts nothing more; the loop carries it out. */
			if (action_requested != 0)
				return;

			/* A disabled service, or one this boot has already dealt with, is passed over. */
			if (!service->enabled)
				continue;
			if (service->state != SERVICE_STOPPED)
				continue;
			if (service->started_once)
				continue;

			/* The desktop's round leaves the late services for the next. */
			if (round == START_ROUND_DESKTOP && service->late)
				continue;

			/* A service another enabled one stands in for waits until that one gives up (replaces=). */
			replacer = replacer_of(service);
			if (replacer != NULL) {
				service->state = SERVICE_SKIPPED;
				fprintf(stderr, "init: %s replaced by %s\n", service->name, replacer->name);
				timeline_record("replaced", service->name);
				continue;
			}

			/* The dependencies decide whether it starts now, later, or not at all. */
			dependencies = dependencies_state(service, round);
			if (dependencies == DEPENDENCIES_READY) {
				(void)spawn_service(service);
			} else if (dependencies == DEPENDENCIES_SKIP) {
				service->state = SERVICE_SKIPPED;
				fprintf(stderr, "init: skipped %s: required dependency failed\n", service->name);
				timeline_record("skipped", service->name);
			}
		}
	}
}

/*
 * Reports whether a service can start now: the services it is ordered after
 * have settled, and the ones it requires are up.
 */
static enum dependency_result
dependencies_state(
	const struct service *service,
	enum start_round round)
{
	enum dependency_result order;
	enum dependency_result requirements;

	/* The services named in after=. */
	order = order_state(service, round);
	if (order != DEPENDENCIES_READY)
		return order;

	/* The services named in requires=. */
	requirements = requirements_state(service);
	if (requirements != DEPENDENCIES_READY)
		return requirements;

	/* Succeeded: the service can start now. */
	return DEPENDENCIES_READY;
}

/*
 * Reports whether the services a service is ordered after (after=) have
 * settled.  While the desktop's services start, a late service in the list
 * is not waited for: it starts in the next round, after this one.
 */
static enum dependency_result
order_state(
	const struct service *service,
	enum start_round round)
{
	struct service *dependency;
	char copy[256];
	char *name;
	int settled;

	/* A service ordered after nothing can start. */
	if (service->after[0] == '\0')
		return DEPENDENCIES_READY;

	/* Each name of the list. */
	strcpy(copy, service->after);
	for (name = strtok(copy, ",");
	     name != NULL;
	     name = strtok(NULL, ",")) {
		/* A name no definition has is a broken order: the service is not started. */
		dependency = find_service(name);
		if (dependency == NULL)
			return DEPENDENCIES_SKIP;

		/* A late service is not waited for in the desktop's round. */
		if (round == START_ROUND_DESKTOP && dependency->late)
			continue;

		/* A disabled service never starts, so nothing waits for it. */
		if (!dependency->enabled)
			continue;

		/* One still on its way holds the service back. */
		settled = terminal_state(dependency->state);
		if (!settled)
			return DEPENDENCIES_WAIT;
	}

	/* Succeeded: everything it is ordered after has settled. */
	return DEPENDENCIES_READY;
}

/*
 * Reports whether the services a service requires (requires=) are up.  A
 * requirement that is late is not up in the desktop's round, so the service
 * waits for the next round with it.
 */
static enum dependency_result
requirements_state(
	const struct service *service)
{
	struct service *dependency;
	char copy[256];
	char *name;

	/* A service that requires nothing can start. */
	if (service->requires[0] == '\0')
		return DEPENDENCIES_READY;

	/* Each name of the list. */
	strcpy(copy, service->requires);
	for (name = strtok(copy, ",");
	     name != NULL;
	     name = strtok(NULL, ",")) {
		/* A requirement that is missing or disabled never comes up: the service is not started. */
		dependency = find_service(name);
		if (dependency == NULL)
			return DEPENDENCIES_SKIP;
		if (!dependency->enabled)
			return DEPENDENCIES_SKIP;

		/* A requirement that failed or was not started takes the service with it. */
		if (dependency->state == SERVICE_FAILED)
			return DEPENDENCIES_SKIP;
		if (dependency->state == SERVICE_SKIPPED)
			return DEPENDENCIES_SKIP;

		/* A requirement neither running nor completed is still to come. */
		if (dependency->state != SERVICE_RUNNING && dependency->state != SERVICE_COMPLETED)
			return DEPENDENCIES_WAIT;
	}

	/* Succeeded: everything it requires is up. */
	return DEPENDENCIES_READY;
}

/*
 * Waits, a bounded time, for the desktop to take the display.
 *
 * The wait ends when the mark is there, when no desktop service is running
 * any more (it gave the screen back to the console), when the bound is
 * reached, or when a halt, power-off or reboot is asked for.  Meanwhile the
 * services that end are collected (a greeter that gave up lets the console's
 * getty start) and the control socket is answered.
 */
static void
wait_for_desktop(
	void)
{
	uint64_t deadline;
	uint64_t now;
	int running;
	int shown;

	/* The bound, from now. */
	deadline = monotonic_milliseconds() + DESKTOP_WAIT_MS;

	/* Looks until one of the ends comes. */
	for (;;) {
		/* A system action is carried out by the loop. */
		if (action_requested != 0)
			return;

		/* The desktop has taken the display. */
		shown = access(KERN_INIT_DESKTOP_SHOWN, F_OK);
		if (shown == 0) {
			timeline_record("desktop", "shown");
			return;
		}

		/* No desktop runs (none is enabled, or it gave the screen to the console). */
		running = desktop_running();
		if (!running) {
			timeline_record("desktop", "not-running");
			return;
		}

		/* The bound is reached. */
		now = monotonic_milliseconds();
		if (now >= deadline) {
			timeline_record("desktop", "wait-expired");
			return;
		}

		/* Collects the services that ended, then answers a request or waits a little. */
		reap_children();
		serve_one_request(DESKTOP_POLL_MS);
	}
}

/* Reports whether a service that shows the desktop is running. */
static int
desktop_running(
	void)
{
	size_t index;

	/* Each enabled desktop service. */
	for (index = 0; index < service_count; index++) {
		/* Only a desktop service that this boot enabled counts. */
		if (!services[index].desktop)
			continue;
		if (!services[index].enabled)
			continue;

		/* One that runs is still on its way to the display. */
		if (services[index].state == SERVICE_RUNNING)
			return 1;
	}

	/* None runs. */
	return 0;
}

/*
 * Answers one request on the control socket, or waits up to the given time
 * for one.  Without a socket it only waits.
 */
static void
serve_one_request(
	int milliseconds)
{
	struct pollfd listener;
	int client;

	/* Without a socket there is nothing to answer. */
	if (control_listener < 0) {
		pause_milliseconds((unsigned)milliseconds);
		return;
	}

	/* A connection waiting now is answered at once. */
	client = accept4(control_listener, NULL, NULL, SOCK_CLOEXEC);
	if (client >= 0) {
		handle_request(client);
		close(client);
		return;
	}

	/* Otherwise waits a little for one. */
	listener.fd = control_listener;
	listener.events = POLLIN;
	listener.revents = 0;
	(void)poll(&listener, 1, milliseconds);
}

/*
 * Reports whether this boot shows the desktop first: the boot parameters ask
 * for the graphical login and keep the kernel's messages off the screen.
 *
 * kmsg=console (Ctrl at the loader) and login=console (Shift) are the debug
 * boots, which start the network and the others in their usual place.
 */
static int
boot_shows_desktop_first(
	void)
{
	int graphical;
	int quiet;

	/* The graphical login is asked for. */
	graphical = boot_parameter_is(BOOT_LOGIN_SYSCTL, "graphical");
	if (!graphical)
		return 0;

	/* The kernel's messages stay off the screen. */
	quiet = boot_parameter_is(BOOT_KMSG_SYSCTL, "quiet");
	if (!quiet)
		return 0;

	/* Succeeded: the desktop comes first. */
	return 1;
}

/*
 * Reports whether a boot parameter, read through its sysctl, has the given
 * value.  A kernel that cannot say is taken as a boot without it.
 */
static int
boot_parameter_is(
	const char *name,
	const char *expected)
{
	char value[BOOT_PARAMETER_MAX];
	size_t size;
	int error;
	int differs;

	/* Asks the kernel for the parameter. */
	size = sizeof(value);
	error = sysctlbyname(name, value, &size, NULL, 0);
	if (error != 0)
		return 0;

	/* An answer that is not one terminated string is not the value. */
	if (size == 0 || size > sizeof(value))
		return 0;
	if (value[size - 1] != '\0')
		return 0;

	/* Compares it with the value asked about. */
	differs = strcmp(value, expected);
	if (differs != 0)
		return 0;

	/* Succeeded: the parameter has the value. */
	return 1;
}

/*
 * Opens a new timeline for this boot and writes its first lines: when init
 * started and when the file systems were mounted.
 */
static void
timeline_open(
	void)
{
	int descriptor;

	/* Each boot writes its own timeline over the last one. */
	descriptor = open(TIMELINE_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (descriptor < 0) {
		fprintf(stderr, "init: %s: %s\n", TIMELINE_PATH, strerror(errno));
		return;
	}

	/* The descriptor stays open for the services started later. */
	timeline_descriptor = descriptor;

	/* The start of init, which came before the file could be written, and the mounts. */
	timeline_write(init_started_ms, "init", "started");
	timeline_record("mount", "done");
}

/* Writes one event of the boot into the timeline at the present time. */
static void
timeline_record(
	const char *event,
	const char *name)
{
	uint64_t now;

	/* The time since the kernel started, which the services' own logs also count in. */
	now = monotonic_milliseconds();
	timeline_write(now, event, name);
}

/*
 * Writes one line of the timeline: the seconds since the kernel started,
 * to the millisecond, the event and the service or the step it is about.
 */
static void
timeline_write(
	uint64_t milliseconds,
	const char *event,
	const char *name)
{
	char line[160];
	int length;

	/* Without a timeline there is nothing to write to. */
	if (timeline_descriptor < 0)
		return;

	/* Formats the line; one that does not fit is cut. */
	length = snprintf(line, sizeof(line), "%llu.%03llu %s %s\n",
			  (unsigned long long)(milliseconds / 1000U),
			  (unsigned long long)(milliseconds % 1000U),
			  event,
			  name);
	if (length < 0)
		return;
	if ((size_t)length >= sizeof(line))
		length = (int)sizeof(line) - 1;

	/* Writes it whole; a failed write loses only this line. */
	(void)write(timeline_descriptor, line, (size_t)length);
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

/* Finds the enabled service that stands in for a service (replaces=), or NULL. */
static struct service *
replacer_of(
	const struct service *service)
{
	size_t index;
	int same;

	/* Each other enabled service that names this one. */
	for (index = 0; index < service_count; index++) {
		if (&services[index] == service || !services[index].enabled)
			continue;
		if (services[index].replaces[0] == '\0')
			continue;
		same = strcmp(services[index].replaces, service->name);
		if (same == 0)
			return &services[index];
	}

	/* No service stands in for it. */
	return NULL;
}

/* Starts the service an ended one stood in for, unless the system is being stopped. */
static void
release_replaced(
	const struct service *service)
{
	struct service *replaced;

	/* Nothing named, or the system is stopping. */
	if (service->replaces[0] == '\0' || services_stopping)
		return;

	/* The service it stood in for, when that one is enabled and was held back. */
	replaced = find_service(service->replaces);
	if (replaced == NULL || !replaced->enabled || replaced->state != SERVICE_SKIPPED)
		return;

	/* Starts it. */
	fprintf(stderr, "init: %s ended; starting %s\n", service->name, replaced->name);
	replaced->state = SERVICE_STOPPED;
	(void)spawn_service(replaced);
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
	int waited;
	pid_t child;

	/* Handles the service condition. */
	if (service->state == SERVICE_RUNNING ||
	    service->state == SERVICE_STARTING) {
		errno = EBUSY;

		/* Reports operation failure. */
		return -1;
	}

	/*
	 * The program is named to itself by the path it was run from, not by
	 * the name of the service.  A program that re-executes itself has
	 * only this to go on, and one that is handed a bare name cannot find
	 * itself again; it is also what a program prints when it reports its
	 * own name.
	 */
	argv[0] = service->command;

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
			    setenv("KERN_NOTIFY_FD", "3", 1) != 0)
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
	service->started_once = 1;
	timeline_record("start", service->name);

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
			timeline_record("failed", service->name);
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
		timeline_record("running", service->name);

		/* Reports successful completion. */
		return 0;
	}

	/* Waits for the oneshot, serving the control socket meanwhile. */
	waited = wait_for_oneshot(child, &status);
	if (waited > 0) {
		/* A halt, power-off or reboot was asked for: the service is stopped with the others. */
		printf("init: oneshot %s still running at the system action\n",
		       service->name);

		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed wait. */
	if (waited < 0) {
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
		timeline_record("failed", service->name);
		errno = EIO;

		/* Reports operation failure. */
		return -1;
	}

	service->state = SERVICE_COMPLETED;
	service->pid = 0;
	timeline_record("done", service->name);

	/* Reports successful completion. */
	return 0;
}

/*
 * Waits for a oneshot service to end while serving the control socket.
 * Returns 0 when it ended (its status in *status), 1 when a halt, power-off or
 * reboot was asked for first, and -1 when the wait failed (errno).
 */
static int
wait_for_oneshot(
	pid_t child,
	int *status)
{
	struct pollfd listener;
	pid_t reaped;

	/* Looks at the service and the socket until one of them ends the wait. */
	for (;;) {
		/* Collects the service when it has ended. */
		reaped = waitpid(child, status, WNOHANG);
		if (reaped == child)
			return 0;

		/* Handles a failed wait. */
		if (reaped < 0 && errno != EINTR)
			return -1;

		/* A system action asked for meanwhile ends the wait: the action stops the service. */
		if (action_requested != 0)
			return 1;

		/* Answers the requests that came. */
		serve_while_oneshot();

		/* Waits a little for a connection, or only sleeps without a socket. */
		if (control_listener >= 0) {
			listener.fd = control_listener;
			listener.events = POLLIN;
			listener.revents = 0;
			(void)poll(&listener, 1, ONESHOT_POLL_MS);
		} else {
			(void)usleep(ONESHOT_POLL_MS * 1000);
		}
	}
}

/*
 * Serves the control socket while a oneshot service runs.  A system action
 * and the requests that only read are answered at once; one that starts or
 * stops services is held for init's loop (deferred_requests), or refused
 * when too many are held.
 */
static void
serve_while_oneshot(
	void)
{
	struct zsv1_request request;
	int client;
	int received;
	int error;

	/* Takes each connection waiting now. */
	for (;;) {
		client = accept4(control_listener, NULL, NULL, SOCK_CLOEXEC);
		if (client < 0)
			return;

		/* Reads the request. */
		received = receive_request(client, &request);
		if (received != 0) {
			error = errno;
			refuse_request(client, error);
			close(client);
			continue;
		}

		/* Decides whether the request is answered now. */
		switch (request.command) {
		case ZSV1_COMMAND_HALT:
		case ZSV1_COMMAND_POWEROFF:
		case ZSV1_COMMAND_REBOOT:
		case ZSV1_COMMAND_LIST:
		case ZSV1_COMMAND_SHOW:
		case ZSV1_COMMAND_RELOAD:
			dispatch_request(client, &request);
			close(client);
			break;
		default:
			/* Starting or stopping services waits for init's loop. */
			if (deferred_count < DEFERRED_MAX) {
				deferred_requests[deferred_count].client = client;
				deferred_requests[deferred_count].request = request;
				deferred_count++;
			} else {
				(void)zsv1_server_send_error_end_fd(client, EBUSY, "oneshot-running");
				close(client);
			}

			break;
		}
	}
}

/* Carries out the requests held while a oneshot service ran, oldest first. */
static void
handle_deferred_requests(
	void)
{
	struct deferred_request deferred;

	/* Takes the oldest each time: carrying one out can hold new ones behind it. */
	while (deferred_count > 0) {
		deferred = deferred_requests[0];
		deferred_count--;
		memmove(&deferred_requests[0], &deferred_requests[1],
			deferred_count * sizeof(deferred_requests[0]));
		dispatch_request(deferred.client, &deferred.request);
		close(deferred.client);
	}
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
			if (valid_failure_record(record)) {
				fprintf(stderr, "init: %s: %s\n",
					service->name, record);
			} else {
				fprintf(stderr,
					"init: %s malformed readiness "
					"record\n",
					service->name);
			}
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
			} else {
				/* Not started again: the service it stands in for starts now (replaces=). */
				release_replaced(service);
			}

			break;
		}
	}
}

/*
 * Stops the system and asks the kernel for the final action.
 *
 * The services are stopped in reverse order, then every other process,
 * so nothing writes after the sync.  The kernel writes the volumes out
 * once more before it acts, and refuses the action while that fails;
 * the action is then retried, so data that can still be written is not
 * lost to a reset.
 */
static void
shutdown_system(
	enum init_action action)
{
	size_t index;
	const char *action_name;
	unsigned retry_seconds;
	unsigned retries;
	int system_descriptor, system_action;
	int status;

	index = service_count;
	services_stopping = 1;

	/* Writes the thermal emergency down before anything else, while the disks still take it. */
	note_emergency();

	printf("init: stopping services\n");

	/* Stops the services, the last started first. */
	while (index > 0)
		(void)stop_service(&services[--index]);

	/* Stops what no service owns: login shells, their children, orphans. */
	stop_other_processes();

	/* Writes the volumes out before the kernel is asked to act. */
	sync();

	/* A thermal emergency that came during the shutdown makes it a power-off. */
	note_emergency();
	if (emergency_requested)
		action = INIT_ACTION_POWEROFF;

	/* A power-off asks the kernel to cut the power; a halt and a reboot ask for those. */
	if (action == INIT_ACTION_REBOOT) {
		action_name = "reboot";
		system_action = KERN_SYSTEM_REBOOT;
	} else if (action == INIT_ACTION_POWEROFF) {
		action_name = "poweroff";
		system_action = KERN_SYSTEM_POWEROFF;
	} else {
		action_name = "halt";
		system_action = KERN_SYSTEM_HALT;
	}
	printf("init: executing system action %s\n", action_name);
	(void)fflush(stdout);

	/* Keeps the requested action pending while storage can still recover. */
	retries = 0;
	for (;;) {
		system_descriptor = open("/dev/system", O_RDONLY);
		if (system_descriptor >= 0) {
			/* An action the kernel takes does not return here. */
			status = ioctl(system_descriptor, system_action);
			if (status == 0) {
				(void)close(system_descriptor);
				break;
			}

			/* A kernel or a machine that cannot cut its power is halted instead. */
			if (system_action == KERN_SYSTEM_POWEROFF && errno == EOPNOTSUPP) {
				fprintf(stderr, "init: the machine cannot turn its power off; halting\n");
				system_action = KERN_SYSTEM_HALT;
				(void)close(system_descriptor);
				continue;
			}
		}

		/* The first retries come quickly; after them the user is told what is at stake. */
		retries++;
		retry_seconds = ACTION_QUICK_RETRY_SECONDS;
		if (retries > ACTION_QUICK_RETRIES)
			retry_seconds = ACTION_SLOW_RETRY_SECONDS;
		fprintf(stderr, "init: final system action failed: %s; retrying in %u seconds\n",
			strerror(errno), retry_seconds);
		if (retries == ACTION_QUICK_RETRIES) {
			fprintf(stderr, "init: the volumes could not be written out; "
				"turning the power off now loses what was not written (see the kernel log)\n");
		}
		if (system_descriptor >= 0)
			(void)close(system_descriptor);

		/* Writes the volumes out again before the next try. */
		(void)sleep(retry_seconds);
		sync();

		/* A thermal emergency that came meanwhile asks for the power-off again. */
		note_emergency();
		if (emergency_requested && system_action != KERN_SYSTEM_POWEROFF) {
			action_name = "poweroff";
			system_action = KERN_SYSTEM_POWEROFF;
		}
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;)
		pause();
}

/*
 * Stops one service: SIGTERM, a grace period, then SIGKILL.
 *
 * A process that does not exit even after SIGKILL is stuck in the kernel;
 * it is reported and left behind rather than waited for, so the shutdown
 * still reaches its sync.
 */
static int
stop_service(
	struct service *service)
{
	int exited;
	int status;

	/* A service with no process is already stopped. */
	if (service->pid <= 0) {
		service->state = SERVICE_STOPPED;

		/* Succeeded: nothing was running. */
		return 0;
	}

	/* Asks the service to stop; one that is already gone is waited for all the same. */
	status = kill(service->pid, SIGTERM);
	if (status != 0 && errno != ESRCH)
		return -1;

	/* Gives the service its grace period to exit. */
	exited = wait_for_exit(service->pid, stop_grace_ms());
	if (!exited) {
		/* Ends the service, and gives the kernel the same time to finish it. */
		fprintf(stderr, "init: %s (pid %ld) did not stop; killing it\n",
			service->name, (long)service->pid);
		(void)kill(service->pid, SIGKILL);
		exited = wait_for_exit(service->pid, stop_grace_ms());
		if (!exited) {
			fprintf(stderr, "init: %s (pid %ld) did not exit after SIGKILL; leaving it\n",
				service->name, (long)service->pid);
		}
	}

	service->pid = 0;
	service->state = SERVICE_STOPPED;

	/* Succeeded: the service is stopped or left behind. */
	return 0;
}

/* Reaps one child, waiting at most the given time; reports whether it exited. */
static int
wait_for_exit(
	pid_t pid,
	unsigned milliseconds)
{
	uint64_t started;
	uint64_t now;
	unsigned limit;
	pid_t reaped;
	int status;

	/* Polls the child until it is reaped or the time, which a thermal emergency shortens, is up. */
	started = monotonic_milliseconds();
	for (;;) {
		/* A child that is reaped, or that is no longer ours, has exited. */
		reaped = waitpid(pid, &status, WNOHANG);
		if (reaped == pid)
			return 1;
		if (reaped < 0 && errno == ECHILD)
			return 1;

		/* Writes down a thermal emergency that came during the wait. */
		note_emergency();

		/* A child still running when the time is up has not exited. */
		limit = stop_grace_ms();
		if (limit > milliseconds)
			limit = milliseconds;
		now = monotonic_milliseconds();
		if (now - started >= limit)
			return 0;
		pause_milliseconds(STOP_POLL_MS);
	}
}

/*
 * Stops every process that no service owns.
 *
 * A login shell, a program it started, or an orphan would otherwise keep
 * writing after the sync, and what it wrote would be lost to the reset.
 * Each is sent SIGTERM, then SIGKILL after a grace period.
 * Init itself is never signalled.
 */
static void
stop_other_processes(
	void)
{
	unsigned count;

	/* Asks every other process to stop. */
	count = signal_other_processes(SIGTERM);
	if (count == 0)
		return;
	printf("init: stopping %u other processes\n", count);

	/* Gives them their grace period. */
	count = wait_for_other_processes(stop_grace_ms());
	if (count == 0)
		return;

	/* Ends the ones still running, and gives the kernel the time to finish them. */
	printf("init: killing %u processes that did not stop\n", count);
	(void)signal_other_processes(SIGKILL);
	count = wait_for_other_processes(stop_grace_ms());
	if (count != 0)
		fprintf(stderr, "init: %u processes did not exit after SIGKILL; leaving them\n", count);
}

/*
 * Sends a signal to every live process other than init and the kernel.
 *
 * The processes are listed through KERN_SYSTEM_GET_PROCESS rather than
 * reached with kill(-1), which would signal init as well.  A signal number
 * of 0 only counts them.  Reports how many processes were live.
 */
static unsigned
signal_other_processes(
	int signal_number)
{
	struct process_info process;
	unsigned count;
	int descriptor;
	int status;

	/* Opens the system's device, which lists the processes. */
	descriptor = open("/dev/system", O_RDONLY);
	if (descriptor < 0)
		return 0;

	/* Asks for each process after the one before, until there is none. */
	count = 0;
	memset(&process, 0, sizeof(process));
	process.pid = -1;
	for (;;) {
		status = ioctl(descriptor, KERN_SYSTEM_GET_PROCESS, &process);
		if (status != 0)
			break;

		/* The kernel, init, and a process that has exited are passed over. */
		if (process.pid <= 1)
			continue;
		if (process.state == PROCESS_STATE_ZOMBIE)
			continue;
		if (process.state == PROCESS_STATE_DEAD)
			continue;

		/* Counts the process and signals it. */
		count++;
		if (signal_number != 0)
			(void)kill((pid_t)process.pid, signal_number);
	}

	(void)close(descriptor);

	/* Succeeded: reports how many processes were live. */
	return count;
}

/*
 * Waits for the other processes to exit, reaping the ones init inherited.
 *
 * Reports how many are still live when the time is up, or 0 once none is.
 */
static unsigned
wait_for_other_processes(
	unsigned milliseconds)
{
	uint64_t started;
	uint64_t now;
	unsigned limit;
	unsigned count;
	pid_t reaped;
	int status;

	/* Polls until no other process is live or the time, which a thermal emergency shortens, is up. */
	started = monotonic_milliseconds();
	for (;;) {
		/* Reaps every child that has exited, orphans included. */
		for (;;) {
			reaped = waitpid(-1, &status, WNOHANG);
			if (reaped <= 0)
				break;
		}

		/* Counts what is still live. */
		count = signal_other_processes(0);
		if (count == 0)
			return 0;

		/* Writes down a thermal emergency that came during the wait. */
		note_emergency();

		/* Reports what is left when the time is up. */
		limit = stop_grace_ms();
		if (limit > milliseconds)
			limit = milliseconds;
		now = monotonic_milliseconds();
		if (now - started >= limit)
			return count;
		pause_milliseconds(STOP_POLL_MS);
	}
}

/* Sleeps for the given number of milliseconds. */
static void
pause_milliseconds(
	unsigned milliseconds)
{
	struct timespec interval;

	/* Splits the pause into seconds and nanoseconds. */
	interval.tv_sec = (time_t)(milliseconds / 1000U);
	interval.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	(void)nanosleep(&interval, NULL);
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
	struct zsv1_request request;
	int received;
	int error;

	/* Handles a failed receive request operation. */
	received = receive_request(client, &request);
	if (received != 0) {
		error = errno;
		refuse_request(client, error);

		/* Returns the computed result. */
		return;
	}

	/* Carries the request out. */
	dispatch_request(client, &request);
}

/* Answers a request that could not be read. */
static void
refuse_request(
	int client,
	int error)
{
	const char *reason;

	/* Names what was wrong with the request. */
	if (error == EPROTONOSUPPORT) {
		reason = "unknown-version";
	} else if (error == EOVERFLOW || error == EMSGSIZE) {
		reason = "request-too-long";
	} else if (error == ETIMEDOUT) {
		reason = "request-timeout";
	} else {
		reason = "malformed-request";
	}

	/* A failure without its own number is reported as an I/O error. */
	if (error <= 0)
		error = EIO;

	/* Answers the client. */
	(void)zsv1_server_send_error_end_fd(client, error, reason);
}

/* Carries out one received request and answers it. */
static void
dispatch_request(
	int client,
	const struct zsv1_request *request)
{
	enum init_action action;
	struct service *service;
	size_t index;
	int error;

	/* Handles the request condition. */
	if (request->command == ZSV1_COMMAND_LIST) {
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
	if (request->command == ZSV1_COMMAND_RELOAD) {
		/* Handles a failed reload policy operation. */
		if (reload_policy() == 0) {
			(void)zsv1_server_send_ok_end_fd(client, "reloaded");
		} else {
			error = errno;
			(void)zsv1_server_send_error_end_fd(
			    client, error > 0 ? error : EIO, "reload-failed");
		}

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request->command == ZSV1_COMMAND_HALT ||
	    request->command == ZSV1_COMMAND_POWEROFF ||
	    request->command == ZSV1_COMMAND_REBOOT) {
		action = request->command == ZSV1_COMMAND_REBOOT ? INIT_ACTION_REBOOT
	    : request->command == ZSV1_COMMAND_POWEROFF
	? INIT_ACTION_POWEROFF
	: INIT_ACTION_HALT;

		/* Schedules the action, unless the thermal emergency's power-off is under way. */
		if (zsv1_server_send_ok_end_fd(client, "scheduled") == 0 && !emergency_requested)
			action_requested = action;

		/* Returns the computed result. */
		return;
	}

	/* Finds the service the request names. */
	service = find_service(request->service);

	/* Handles the service availability. */
	if (service == NULL) {
		(void)zsv1_server_send_error_end_fd(client, ENOENT,
						    "unknown-service");

		/* Returns the computed result. */
		return;
	}

	/* Handles the request condition. */
	if (request->command == ZSV1_COMMAND_SHOW) {
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
	if (request->command == ZSV1_COMMAND_START) {
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
	if (request->command == ZSV1_COMMAND_STOP) {
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
	if (request->command == ZSV1_COMMAND_RESTART) {
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
	/* The thermal emergency's power-off stands; nothing changes it afterwards. */
	if (number == SIGUSR2) {
		emergency_requested = 1;
		action_requested = INIT_ACTION_POWEROFF;
		return;
	}
	if (emergency_requested)
		return;

	/* A reload, a reboot or a halt. */
	if (number == SIGHUP)
		reload_requested = 1;
	else if (number == SIGINT)
		action_requested = INIT_ACTION_REBOOT;
	else if (number == SIGTERM)
		action_requested = INIT_ACTION_HALT;
}

/* The grace a stopping process gets now: shorter once the thermal emergency began. */
static unsigned
stop_grace_ms(
	void)
{
	/* The emergency shortens every grace still to come or under way. */
	if (emergency_requested)
		return EMERGENCY_GRACE_MS;

	/* Succeeded: the ordinary grace. */
	return STOP_GRACE_MS;
}

/*
 * Says once that the kernel reported a thermal emergency, on the console
 * and in its record file, written out at once since the power goes soon.
 */
static void
note_emergency(
	void)
{
	char line[128];
	uint64_t since_start;
	time_t now;
	int descriptor;
	int length;

	/* Only once, and only in an emergency. */
	if (!emergency_requested || emergency_noted)
		return;
	emergency_noted = 1;

	/* Tells the console. */
	fprintf(stderr, "init: thermal emergency: turning the power off\n");
	timeline_record("thermal", "emergency");

	/* The wall time and the time since init started tell the emergency and the boot apart. */
	now = time(NULL);
	since_start = monotonic_milliseconds() - init_started_ms;
	length = snprintf(line, sizeof(line), "thermal emergency at %lld, %llu ms after init started\n",
		(long long)now, (unsigned long long)since_start);
	if (length <= 0 || (size_t)length >= sizeof(line))
		return;

	/* Appends it to the record and writes it out. */
	descriptor = open(EMERGENCY_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (descriptor < 0) {
		fprintf(stderr, "init: %s: %s\n", EMERGENCY_PATH, strerror(errno));
		return;
	}
	(void)write(descriptor, line, (size_t)length);
	(void)fsync(descriptor);
	(void)close(descriptor);
}
