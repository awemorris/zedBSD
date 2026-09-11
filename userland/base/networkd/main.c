/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD networkd userland command.
 */

#include "userland/base/net/netutil.h"
#include "userland/base/net/protocol.h"
#include "userland/base/net/publication-trace.h"
#include "userland/base/net/wifi-store.h"
#include "userland/base/networkd/confirmed.h"
#include "userland/base/networkd/managed-wlan.h"
#include "userland/base/networkd/wifi-child.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <net/if.h>
#include <net/route.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHILD_OUTPUT_MAX 384
#define NETWORKD_AUTH_LOG_MAX 512U
#define NETWORKD_GROUP_DATABASE_MAX 8192U
#define NETWORKD_GROUP_BUFFER_MAX 2048U
#define NETWORKD_GROUP_FILE "/etc/group"
#define NETWORKD_GROUP_GID ((gid_t)69)
#define NETWORKD_GROUP_NAME "network"
#define NETWORKD_RESPONSE_FIXED_OVERHEAD 20U
#define NETWORKD_RESPONSE_OUTPUT_MAX \
	(NETWORKD_RESPONSE_MAX - NETWORKD_RESPONSE_FIXED_OVERHEAD)
#define NETWORKD_WLAN_RADIO_MAX	16U
#define NETWORKD_WLAN_SCAN_SECONDS	30U
#define NETWORKD_WLAN_CONNECT_SECONDS	35U
#define NETWORKD_WLAN_DHCP_SECONDS	10U
#define NETWORKD_WLAN_RESCAN_SECONDS	5U
#define NETWORKD_WLAN_ATTEMPTS	4U
#define NETWORKD_WIFI_STATUS_RESERVE	128U
#define NETWORKD_CONTROL_INPUT_MAX	4U

enum networkd_client_role {
	NETWORKD_CLIENT_ROOT,
	NETWORKD_CLIENT_MEMBER
};

struct networkd_listener {
	int descriptor;
	dev_t device;
	ino_t inode;
	int owns_path;
	const char *stage;
};

struct networkd_group_scan {
	unsigned network_records;
	unsigned gid_records;
};

struct networkd_request {
	struct networkd_protocol_header header;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	char interface[IFNAMSIZ];
	char address[INET_ADDRSTRLEN];
	char netmask[INET_ADDRSTRLEN];
	char gateway[INET_ADDRSTRLEN];
	char rollback_path[NETWORKD_ROLLBACK_PATH_MAX + 1U];
	char dns[8][INET_ADDRSTRLEN];
	unsigned char ssid[WLAN_SSID_MAX];
	size_t ssid_length;
	unsigned dns_count;
	unsigned timeout;
	uint32_t token;
};

struct networkd_wlan_radio {
	char interface[IFNAMSIZ];
	uint32_t ifindex;
	int ready;
	int administrative_up;
	int association_active;
	uint32_t scan_state;
	uint64_t snapshot_generation;
	/* Last validated snapshot considered by selection for this device identity. */
	uint64_t consumed_snapshot_generation;
	uint32_t stop_flags;
	int observation_error;
};

/* Owns all temporary data for exactly one managed Wi-Fi request. */
struct networkd_wifi_request {
	struct wifi_conf_model profiles;
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_count;
	/* Keep diagnostic and final policy records even when radio output fills. */
	char output[NETWORKD_RESPONSE_OUTPUT_MAX - NETWORKD_DIAGNOSTIC_MAX - 4U];
	size_t output_length;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	const char *stage;
};

/* All subordinate waits consume one actor-owned interval. */
struct networkd_wifi_work {
	uint64_t deadline;
	int background;
	int cancelled;
	int cleanup;
	int profiles_changed;
};

/* Read-only observations let status clients inspect an ongoing RF operation. */
struct networkd_wifi_observation {
	char interface[IFNAMSIZ];
	uint32_t ifindex;
	uint64_t observed_at;
	char *output;
	size_t output_length;
};

struct networkd_wifi_pending {
	struct networkd_request request;
	struct kern_peercred peer;
	enum networkd_client_role role;
};

struct networkd_wifi_candidate {
	const struct wifi_conf_profile *profile;
	size_t radio;
};

/* Partial request bytes belong to transport, never to an RF transaction. */
struct networkd_control_input {
	int active;
	int descriptor;
	uint64_t deadline;
	struct kern_peercred peer;
	enum networkd_client_role role;
	size_t used;
	unsigned char bytes[NETWORKD_PROTOCOL_HEADER_MAX + NETWORKD_REQUEST_MAX + 1U];
};

static volatile sig_atomic_t stopping;
static struct networkd_managed_wlan managed_wlan;
static struct networkd_wlan_radio known_wlan_radios[NETWORKD_WLAN_RADIO_MAX];
static size_t known_wlan_radio_count;
static int wifi_disable_pending;
static struct networkd_confirmed confirmed;
static int route_events = -1;
static uint64_t route_event_sequence;
static uint64_t automatic_retry_at;
static unsigned retirement_retry_seconds = NETWORKD_WLAN_RESCAN_SECONDS;
static size_t automatic_candidate_skip;
static struct networkd_wifi_work wifi_work;
static struct networkd_wifi_observation wifi_observations[NETWORKD_WLAN_RADIO_MAX];
static size_t wifi_observation_bytes;
static struct networkd_wifi_pending wifi_pending;
static int wifi_pending_client = -1;
static int control_listener = -1;
static int (*wifi_wait_pump)(void);
static struct networkd_control_input control_inputs[NETWORKD_CONTROL_INPUT_MAX];
static int control_input_pending(void);
static int receive_wait_request(struct networkd_request *, struct kern_peercred *, enum networkd_client_role *);
static int service_wifi_wait(void);
static void remember_wifi_observation(const char *, const struct networkd_wifi_child_result *);
static void clear_wifi_observation(size_t);
static int append_wifi_snapshot(const char *, const char *, size_t, uint64_t, char *, size_t, size_t *);
static void send_wifi_observation(int, const struct networkd_request *);
static void wifi_profiles_changed(const struct kern_peercred *);
static void dispatch_pending_wifi(void);
static void wifi_work_begin(uint32_t, int);
static void wifi_work_end(void);
static void recover_managed_connection(void);
static void retry_managed_retirement(void);
static void schedule_retirement_retry(void);

static int scan_group_record(char *line, struct networkd_group_scan *scan);
static int validate_network_group_database(void);
static int resolve_network_group(gid_t *result);
static int listener_path_matches(const struct networkd_listener *listener);
static int close_listener(struct networkd_listener *listener);
static int remove_stale_listener(void);
static int open_listener(struct networkd_listener *listener);
static int open_route_events(void);
static int process_route_events(void);
static void process_route_event(const struct rtm_ifinfo *);
static void recover_managed_wlan(void);
static void schedule_automatic_work(unsigned);
static int automatic_poll_timeout(void);
static void run_automatic_work(void);
static int event_poll_timeout(void);
static void run_confirmed_due(void);
static void run_due_work(void);
static int retire_managed_connection(enum networkd_managed_wlan_state, int);
static int retire_removed_connection(enum networkd_managed_wlan_state);
static int retire_managed_policy(void);
static void notify_init(const char *record);
static int write_all(int descriptor, const char *buffer, size_t length);
static void write_auth_log(const struct kern_peercred *peer,
			   enum networkd_client_role role, int error);
static int authenticate_client(int client, struct kern_peercred *peer,
			       enum networkd_client_role *role);
static int operation_allowed(enum networkd_client_role role,
			     const char *operation);
static void handle_request(int, enum networkd_client_role,
	const struct kern_peercred *);
static void dispatch_request(int, struct networkd_request *, enum networkd_client_role, const struct kern_peercred *);
static void send_wired_observation(int, const struct networkd_request *);
static void handle_wifi_request(int, struct networkd_request *,
	const struct kern_peercred *);
static int wifi_request_list(struct networkd_wifi_request *);
static int wifi_request_enable(struct networkd_wifi_request *, const struct kern_peercred *);
static int wifi_request_stop(struct networkd_wifi_request *, int);
static int wifi_request_connect(struct networkd_wifi_request *, const struct networkd_request *);
static int wifi_request_prepare(struct networkd_wifi_request *);
static void process_wifi_request(int, struct networkd_request *, const struct kern_peercred *);
static int read_request(int descriptor, struct networkd_request *request);
static int read_request_end(int descriptor);
static int decode_request(struct networkd_request *request);
static int copy_request_text(char *, size_t, const struct networkd_field *);
static const char *operation_name(uint32_t opcode);
static void send_response(int, uint32_t, uint32_t, uint32_t, int,
	const char *, const void *, size_t);
static void send_error(int client, int error, const char *reason);
static void send_token_response(int, uint32_t, uint32_t, uint32_t);
static int execute_wired_request(struct networkd_request *, char *, size_t,
	char *, size_t, size_t *, int *, uint64_t);
static int rollback_validate(const char *, char *, size_t, void *);
static int rollback_execute(const char *, char *, size_t, void *);
static int rollback_parse(const char *, struct networkd_request *, char *,
	size_t);
static int show_interfaces(const char *name, char *output, size_t capacity);
static int append_interface_status(int descriptor, const char *name, char *output, const size_t capacity, size_t *used);
static int interface_exists(const char *name);
static int interface_index(const char *, uint32_t *);
static int interface_flags(const char *, int *);
static int interface_index_name_matches(uint32_t, const char *);
static int wlan_connected(const char *);
static int managed_connection_usable(void);
static int run_wifi(const char *, const char *, const struct wifi_conf_profile *, unsigned, struct networkd_wifi_child_result *);
static int append_wifi_records(const char *, const char *, size_t, char *, size_t, size_t *);
static int append_wifi_output(const char *, const struct networkd_wifi_child_result *, char *, size_t, size_t *);
static int enumerate_wlan_radios(struct networkd_wlan_radio *, size_t, size_t *);
static int prepare_wlan_radios(struct networkd_wlan_radio *, size_t, char *, size_t, size_t *);
static int run_wifi_append(const char *, const char *, char *, size_t, size_t *);
static int prepare_wlan_radio(struct networkd_wlan_radio *, char *, size_t, size_t *);
static void consume_wlan_snapshot(const struct networkd_wlan_radio *, uint64_t);
static int stop_wlan_radios(const struct networkd_wlan_radio *, size_t, int);
static int wlan_radio_status(const struct networkd_wlan_radio *, struct wlan_status_request *);
static int wifi_disable_defer(void);
static int collect_profile_radios(const struct networkd_wlan_radio *, size_t, const struct wifi_conf_model *, size_t, struct networkd_wifi_candidate *, size_t *, size_t *, uint64_t);
static unsigned wifi_selection_timeout(uint64_t);
static int select_manual_radio(const struct networkd_wlan_radio *, size_t, const struct wifi_conf_profile *, size_t *, uint64_t);
static int connect_automatic(const struct networkd_wlan_radio *, size_t, const struct wifi_conf_model *, uint64_t, char *, size_t, size_t *, int *);
static void stop_losing_scans(const struct networkd_wlan_radio *, size_t, size_t);
static int run_managed_connect(const char *, const struct wifi_conf_profile *, enum networkd_managed_wlan_state, uint64_t, char *, size_t, size_t *, int *);
static int acquire_managed_l3(const char *, uint64_t);
static int reconcile_pending_l3(void);
static const struct wifi_conf_profile *find_profile(const struct wifi_conf_model *, const void *, size_t);
static int load_policy(uid_t, struct wifi_conf_model *, char *, size_t);
static int owner_allowed(const struct kern_peercred *);
static const char *managed_state_name(enum networkd_managed_wlan_state);
static int append_managed_status(char *, size_t, size_t *);
static int snapshot_interface_l3(const char *, uint32_t *, struct networkd_managed_l3 *);
static int snapshot_managed_l3(const struct networkd_managed_wlan *, struct networkd_managed_l3 *);
static int snapshot_resolver(struct networkd_managed_l3 *);
static void identify_l3_ownership(const char *, const struct networkd_managed_l3 *, struct networkd_managed_l3 *);
static int clear_interface_l3(struct networkd_managed_wlan *);
static int find_interface_default(int, uint32_t, struct networkd_managed_l3 *);
static int delete_interface_default_exact(int, const struct networkd_managed_route *);
static int route_matches_owned(const struct rtentry *, const struct networkd_managed_route *);
static int managed_routes_equal(const struct networkd_managed_route *, const struct networkd_managed_route *);
static int get_interface_ipv4(int, const char *, unsigned long, uint32_t *);
static int set_interface_ipv4(int, const char *, unsigned long, uint32_t);
static int unlink_owned_resolver(const struct networkd_managed_wlan *);
static int run_command_until(char *const [], unsigned, uint64_t,
	char [CHILD_OUTPUT_MAX]);
static void clean_diagnostic(char *text);
static int default_route_exists(void);
static int write_resolver(char *const addresses[], int count);
static void handle_signal(int signal_number);
static void ignore_signal(int signal_number);

/*
 * Runs the networkd command.
 */
int
main(
	void)
{
	char record[256];
	int saved;
	int client;
	int status;
	int poll_result;
	int poll_timeout;
	size_t index;
	struct pollfd descriptors[2];
	struct kern_peercred peer;
	enum networkd_client_role role;
	struct networkd_listener listener;

	(void)signal(SIGHUP, ignore_signal);
	(void)signal(SIGPIPE, ignore_signal);
	(void)signal(SIGTERM, handle_signal);
	(void)signal(SIGINT, handle_signal);
	status = 0;
	networkd_managed_wlan_init(&managed_wlan);
	networkd_confirmed_init(&confirmed);

	/* Creates and verifies the privileged network control endpoint. */
	if (open_listener(&listener) != 0) {
		saved = errno;
		(void)snprintf(record, sizeof(record), "FAIL %d %s %s\n", saved,
			       listener.stage != NULL ? listener.stage : "unknown",
			       strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: control socket %s: %s\n",
			listener.stage != NULL ? listener.stage : "unknown",
			strerror(saved));

		/* Reports operation failure. */
		return 1;
	}
	route_events = open_route_events();
	if (route_events < 0) {
		saved = errno != 0 ? errno : EIO;
		(void)close_listener(&listener);
		fprintf(stderr, "networkd: route event socket: %s\n",
		    strerror(saved));
		return 1;
	}
	if (retire_managed_policy() != 0) {
		saved = errno != 0 ? errno : EIO;
		(void)close(route_events);
		route_events = -1;
		(void)close_listener(&listener);
		(void)snprintf(record, sizeof(record), "FAIL %d wifi-normalize %s\n",
		    saved, strerror(saved));
		notify_init(record);
		fprintf(stderr, "networkd: initial Wi-Fi normalization: %s\n",
		    strerror(saved));
		return 1;
	}
	notify_init("READY\n");
	control_listener = listener.descriptor;
	wifi_wait_pump = service_wifi_wait;
	networkd_wifi_child_set_pump(service_wifi_wait);

	/* Polls control and interface events without a one-second accept delay. */
	while (!stopping) {
		if (wifi_pending_client >= 0) {
			dispatch_pending_wifi();
			continue;
		}

		/* Continues partial requests accepted by the previous background wait. */
		if (control_input_pending())
			(void)service_wifi_wait();
		run_due_work();
		if (wifi_pending_client >= 0)
			continue;
		descriptors[0].fd = listener.descriptor;
		descriptors[0].events = POLLIN;
		descriptors[0].revents = 0;
		descriptors[1].fd = networkd_confirmed_active(&confirmed) ? -1 :
		    route_events;
		descriptors[1].events = POLLIN;
		descriptors[1].revents = 0;
		poll_timeout = event_poll_timeout();
		if (control_input_pending() && (poll_timeout < 0 || poll_timeout > 20))
			poll_timeout = 20;
		poll_result = poll(descriptors, 2U, poll_timeout);
		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (poll_result == 0) {
			run_due_work();
			continue;
		}
		if ((descriptors[1].revents & (POLLIN | POLLERR | POLLHUP)) != 0 &&
		    process_route_events() != 0) {
			(void)close(route_events);
			route_events = -1;
		}
		if ((descriptors[0].revents & POLLIN) == 0)
			continue;
		client = accept4(listener.descriptor, NULL, NULL, SOCK_CLOEXEC);

		/* Handles the client condition. */
		if (client >= 0) {
			/* Handles a failed authenticate client operation. */
			if (authenticate_client(client, &peer, &role) == 0)
				handle_request(client, role, &peer);
			else
				send_error(client, EACCES,
				    "authentication failed");
			close(client);
			if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING &&
			    automatic_retry_at == 0U)
				schedule_automatic_work(
				    NETWORKD_WLAN_RESCAN_SECONDS);
			continue;
		}

		/* Handles the reported system error. */
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
	}
	status = stopping ? 0 : 1;
	if (wifi_pending_client >= 0) {
		send_response(wifi_pending_client, wifi_pending.request.header.request_id,
		    wifi_pending.request.header.opcode, NETWORKD_RESULT_ERROR, EINTR,
		    "networkd stopping", NULL, 0U);
		(void)close(wifi_pending_client);
		wifi_pending_client = -1;
		networkd_protocol_clear(&wifi_pending, sizeof(wifi_pending));
	}
	control_listener = -1;

	/* Releases optional observations and incomplete transports on shutdown. */
	for (index = 0U; index < NETWORKD_WLAN_RADIO_MAX; index++)
		clear_wifi_observation(index);
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		if (control_inputs[index].active)
			(void)close(control_inputs[index].descriptor);
	}
	networkd_protocol_clear(control_inputs, sizeof(control_inputs));
	wifi_wait_pump = NULL;
	networkd_wifi_child_set_pump(NULL);
	networkd_confirmed_reset(&confirmed);
	if (retire_managed_policy() != 0)
		status = 1;
	if (route_events >= 0) {
		(void)close(route_events);
		route_events = -1;
	}

	/* Removes only the socket pathname created by this daemon instance. */
	if (close_listener(&listener) != 0) {
		fprintf(stderr, "networkd: control socket cleanup: %s\n",
			strerror(errno));
		status = 1;
	}

	/* Returns the computed result. */
	return status;
}

/* Validates one record from the trusted network group database. */
static int
scan_group_record(
	char *line,
	struct networkd_group_scan *scan)
{
	char *field[4];
	char *cursor;
	char *end;
	unsigned long gid;

	/* Handles the line availability. */
	if (line == NULL || scan == NULL || line[0] == '\0' || line[0] == '#')
		return line != NULL && scan != NULL ? 0 : EINVAL;
	field[0] = line;
	cursor = strchr(field[0], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[1] = cursor + 1;
	cursor = strchr(field[1], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[2] = cursor + 1;
	cursor = strchr(field[2], ':');

	/* Handles the cursor availability. */
	if (cursor == NULL)
		return EINVAL;
	*cursor = '\0';
	field[3] = cursor + 1;

	/* Handles a failed strchr operation. */
	if (strchr(field[3], ':') != NULL || field[0][0] == '\0' ||
	    field[2][0] == '\0')

		/* Returns the computed result. */
		return EINVAL;
	for (cursor = field[0]; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if ((unsigned char)*cursor <= 32U ||
		    (unsigned char)*cursor == 127U)

			/* Returns the computed result. */
			return EINVAL;
	}
	for (cursor = field[2]; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor < '0' || *cursor > '9')
			return EINVAL;
	}
	errno = 0;
	gid = strtoul(field[2], &end, 10);

	/* Handles the reported system error. */
	if (errno != 0 || end == field[2] || *end != '\0' || gid > UINT_MAX)
		return EINVAL;

	/* Selects the matching value. */
	if (strcmp(field[0], NETWORKD_GROUP_NAME) == 0)
		scan->network_records++;

	/* Handles the gid condition. */
	if (gid == (unsigned long)NETWORKD_GROUP_GID)
		scan->gid_records++;

	/* Reports successful completion. */
	return 0;
}

/* Ensures that name and numeric identity each have one unambiguous record. */
static int
validate_network_group_database(
	void)
{
	char database[NETWORKD_GROUP_DATABASE_MAX + 1U];
	struct networkd_group_scan scan;
	char *line;
	char *newline;
	char extra;
	size_t used;
	size_t start;
	size_t length;
	ssize_t count;
	int descriptor;
	int error;

	used = 0;
	error = 0;
	descriptor = open(NETWORKD_GROUP_FILE, O_RDONLY | O_CLOEXEC);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;
	while (used < NETWORKD_GROUP_DATABASE_MAX) {
		count = read(descriptor, database + used,
			     NETWORKD_GROUP_DATABASE_MAX - used);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count < 0) {
			error = errno;
			break;
		}

		/* Checks the remaining item count. */
		if (count == 0)
			break;
		used += (size_t)count;
	}

	/* Handles an operation failure. */
	if (error == 0 && used == NETWORKD_GROUP_DATABASE_MAX) {
		do {
			count = read(descriptor, &extra, 1U);
		} while (count < 0 && errno == EINTR);

		/* Checks the remaining item count. */
		if (count != 0)
			error = count < 0 ? errno : E2BIG;
	}

	/* Handles an operation failure. */
	if (close(descriptor) != 0 && error == 0)
		error = errno;

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed memchr operation. */
	if (memchr(database, '\0', used) != NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	database[used] = '\0';
	memset(&scan, 0, sizeof(scan));
	for (start = 0; start < used;) {
		line = database + start;
		newline = memchr(line, '\n', used - start);
		length = newline != NULL ? (size_t)(newline - line)
					 : used - start;
		line[length] = '\0';

		/* Checks the current data length. */
		if (length != 0 && line[length - 1U] == '\r')
			line[length - 1U] = '\0';

		/* Handles a failed scan group record operation. */
		if (scan_group_record(line, &scan) != 0) {
			errno = EINVAL;

			/* Reports operation failure. */
			return -1;
		}
		start += length + (newline != NULL ? 1U : 0U);
	}

	/* Handles the scan condition. */
	if (scan.network_records != 1U || scan.gid_records != 1U) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Resolves the fixed network operator group without accepting ambiguity. */
static int
resolve_network_group(
	gid_t *result)
{
	char buffer[NETWORKD_GROUP_BUFFER_MAX];
	struct group storage;
	struct group *entry;
	int error;

	entry = NULL;

	/* Handles the result availability. */
	if (result == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	error = getgrnam_r(NETWORKD_GROUP_NAME, &storage, buffer,
			   sizeof(buffer), &entry);

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the entry availability. */
	if (entry == NULL) {
		errno = ENOENT;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the gr name availability. */
	if (entry->gr_name == NULL ||
	    strcmp(entry->gr_name, NETWORKD_GROUP_NAME) != 0 ||
	    entry->gr_gid != NETWORKD_GROUP_GID) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed validate network group database operation. */
	if (validate_network_group_database() != 0)
		return -1;
	*result = entry->gr_gid;
	/* Reports successful completion. */
	return 0;
}

/* Checks whether the public pathname still names this listener instance. */
static int
listener_path_matches(
	const struct networkd_listener *listener)
{
	int function_result;
	struct stat status;

	/* Handles the listener availability. */
	if (listener == NULL || !listener->owns_path)
		return 0;

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return 0;

	/* Computes the function result. */
	function_result = S_ISSOCK(status.st_mode) && status.st_dev == listener->device &&
	       status.st_ino == listener->inode;

	/* Returns the computed result. */
	return function_result;
}

/* Closes a listener without unlinking a pathname replaced by another owner. */
static int
close_listener(
	struct networkd_listener *listener)
{
	struct stat status;
	int error;

	error = 0;

	/* Handles the listener availability. */
	if (listener == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles a failed close operation. */
	if (listener->descriptor >= 0 && close(listener->descriptor) != 0)
		error = errno;
	listener->descriptor = -1;

	/* Handles the listener condition. */
	if (listener->owns_path) {
		/* Handles the listener path matches condition. */
		if (listener_path_matches(listener)) {
			/* Handles an operation failure. */
			if (unlink(NETWORKD_SOCKET) != 0 && error == 0)
				error = errno;
		} else if (lstat(NETWORKD_SOCKET, &status) == 0 && error == 0) {
			/*
			 * A replacement endpoint belongs to another daemon
			 * instance and must never be removed here.
			 */
			error = EBUSY;
		}
	}
	listener->owns_path = 0;

	/* Handles an operation failure. */
	if (error != 0) {
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Removes a stale socket, but refuses to replace a non-socket object. */
static int
remove_stale_listener(
	void)
{
	int function_result;
	struct stat status;

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		return errno == ENOENT ? 0 : -1;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode)) {
		errno = EEXIST;

		/* Reports operation failure. */
		return -1;
	}

	/* Obtains the unlink result. */
	function_result = unlink(NETWORKD_SOCKET);

	/* Returns the computed result. */
	return function_result;
}

/* Creates the group-accessible network control listener. */
static int
open_listener(
	struct networkd_listener *listener)
{
	struct sockaddr_un address;
	struct stat status;
	mode_t old_mask;
	gid_t group;
	int saved;

	/* Handles the listener availability. */
	if (listener == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(listener, 0, sizeof(*listener));
	listener->descriptor = -1;
	listener->stage = "resolve-group";

	/* Handles a failed resolve network group operation. */
	if (resolve_network_group(&group) != 0)
		return -1;
	listener->stage = "remove-stale";

	/* Handles a failed remove stale listener operation. */
	if (remove_stale_listener() != 0)
		return -1;
	listener->stage = "socket";
	listener->descriptor =
	    socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Handles the listener condition. */
	if (listener->descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, NETWORKD_SOCKET);
	listener->stage = "bind";
	old_mask = umask(0177);

	/* Handles a failed bind operation. */
	if (bind(listener->descriptor, (struct sockaddr *)&address,
		 sizeof(address)) != 0) {
		saved = errno;
		(void)umask(old_mask);
		errno = saved;
		goto fail;
	}
	(void)umask(old_mask);
	listener->stage = "identify";

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode)) {
		errno = EINVAL;
		goto fail;
	}
	listener->device = status.st_dev;
	listener->inode = status.st_ino;
	listener->owns_path = 1;
	listener->stage = "owner";

	/* Handles a failed lchown operation. */
	if (lchown(NETWORKD_SOCKET, 0, group) != 0)
		goto fail;
	listener->stage = "mode";

	/* Handles a failed chmod operation. */
	if (chmod(NETWORKD_SOCKET, 0660) != 0)
		goto fail;
	listener->stage = "verify";

	/* Handles a failed lstat operation. */
	if (lstat(NETWORKD_SOCKET, &status) != 0)
		goto fail;

	/* Handles a failed S ISSOCK operation. */
	if (!S_ISSOCK(status.st_mode) || status.st_dev != listener->device ||
	    status.st_ino != listener->inode || status.st_uid != 0 ||
	    status.st_gid != group || (status.st_mode & 07777U) != 0660U) {
		errno = EINVAL;
		goto fail;
	}
	listener->stage = "listen";

	/* Handles a failed listen operation. */
	if (listen(listener->descriptor, 8) != 0)
		goto fail;
	listener->stage = "ready";

	/* Reports successful completion. */
	return 0;

fail:
	saved = errno != 0 ? errno : EIO;
	(void)close_listener(listener);
	errno = saved;

	/* Reports operation failure. */
	return -1;
}

/* Opens the fixed read-only interface-event stream. */
static int
open_route_events(
	void)
{
	int descriptor;

	/* Subscribes before networkd publishes readiness or snapshots links. */
	descriptor = socket(PF_ROUTE,
	    SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	/* Returns the descriptor or the socket error unchanged. */
	return descriptor;
}

/* Drains every currently queued fixed-width interface event. */
static int
process_route_events(
	void)
{
	struct rtm_ifinfo event;
	ssize_t count;
	int saved;

	/* Consumes complete packet records until the nonblocking queue is empty. */
	for (;;) {
		count = read(route_events, &event, sizeof(event));
		if (count == (ssize_t)sizeof(event)) {
			if (event.rtm_sequence > route_event_sequence)
				route_event_sequence = event.rtm_sequence;
			process_route_event(&event);
			continue;
		}
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (count == 0)
			saved = EPIPE;
		else if (count > 0)
			saved = EINVAL;
		else
			saved = errno != 0 ? errno : EIO;
		/* Losing the event channel cannot recursively run policy teardown. */
		if (managed_wlan.connection.interface[0] != '\0') {
			if (managed_wlan.state != NETWORKD_WLAN_RETIRING)
				(void)networkd_managed_wlan_begin_retire(&managed_wlan,
				    NETWORKD_WLAN_AUTO_SEARCHING);
			schedule_automatic_work(0U);
		}
		errno = saved;
		return -1;
	}
}

/* Records event-driven intent; child work is owned only by the outer loop. */
static void
process_route_event(
	const struct rtm_ifinfo *event)
{
	struct networkd_managed_wlan_connection *connection;
	enum networkd_managed_wlan_action action;
	int flags;

	action = networkd_managed_wlan_event(&managed_wlan, event);
	connection = &managed_wlan.connection;

	/* A manual teardown target survives carrier, overflow and removal events. */
	if (managed_wlan.state == NETWORKD_WLAN_RETIRING) {
		if (action == NETWORKD_WLAN_ACTION_RETIRE)
			connection->device_removed = 1;
		if (action != NETWORKD_WLAN_ACTION_NONE)
			schedule_automatic_work(0U);
		return;
	}
	if (action == NETWORKD_WLAN_ACTION_NONE) {
		if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING)
			schedule_automatic_work(0U);
		return;
	}
	if (action == NETWORKD_WLAN_ACTION_RETIRE) {
		connection->device_removed = 1;
		(void)networkd_managed_wlan_begin_retire(&managed_wlan,
		    NETWORKD_WLAN_AUTO_SEARCHING);
		schedule_automatic_work(0U);
		return;
	}
	if (connection->interface[0] == '\0') {
		schedule_automatic_work(0U);
		return;
	}

	if (interface_index_name_matches(connection->ifindex,
	    connection->interface) == 0 &&
	    interface_flags(connection->interface, &flags) == 0 &&
	    (flags & IFF_UP) != 0) {
		if ((flags & IFF_RUNNING) != 0 && managed_connection_usable()) {
			networkd_managed_wlan_recovery_complete(&managed_wlan, 1);
			return;
		}
		if (action == NETWORKD_WLAN_ACTION_RECOVER) {
			schedule_automatic_work(0U);
			return;
		}
	}
	(void)networkd_managed_wlan_begin_retire(&managed_wlan,
	    NETWORKD_WLAN_AUTO_SEARCHING);
	schedule_automatic_work(0U);
}

/* Gives same-profile RF recovery the same cancellation and deadline contract. */
static void
recover_managed_wlan(
	void)
{
	if (managed_wlan.state != NETWORKD_WLAN_RECONNECTING)
		return;
	automatic_retry_at = 0U;
	wifi_work_begin(NETWORKD_OP_WIFI_CONNECT, 1);
	recover_managed_connection();
	wifi_work_end();
}

/* Retries only the retained teardown target, never a fresh RF connection. */
static void
retry_managed_retirement(
	void)
{
	enum networkd_managed_wlan_state target;
	struct networkd_wifi_request work;

	if (managed_wlan.state != NETWORKD_WLAN_RETIRING)
		return;
	automatic_retry_at = 0U;
	if (wifi_disable_pending) {
		memset(&work, 0, sizeof(work));
		wifi_work_begin(NETWORKD_OP_WIFI_DISABLE, 1);
		(void)wifi_request_stop(&work, 1);
		wifi_work_end();
		return;
	}
	target = managed_wlan.retire_target;
	wifi_work_begin(NETWORKD_OP_WIFI_DISCONNECT, 1);
	(void)retire_managed_connection(target, 1);
	wifi_work_end();

	/* Failed retirement already owns a backed-off retry; success may resume search. */
	if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING)
		schedule_automatic_work(NETWORKD_WLAN_RESCAN_SECONDS);
}

/* Backs off persistent OS failures without abandoning an unresolved token. */
static void
schedule_retirement_retry(
	void)
{
	/* Retains a finite retry interval and allows explicit commands to retry sooner. */
	automatic_retry_at = netutil_monotonic_us() +
	    (uint64_t)retirement_retry_seconds * 1000000ULL;
	if (retirement_retry_seconds < 60U) {
		retirement_retry_seconds *= 2U;
		if (retirement_retry_seconds > 60U)
			retirement_retry_seconds = 60U;
	}
}

/* Runs one ordinary 30-second reconnect using a freshly loaded secret. */
static void
recover_managed_connection(
	void)
{
	struct networkd_wifi_child_result result;
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	struct wifi_conf_model model;
	const struct wifi_conf_profile *profile;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	char interface[IFNAMSIZ];
	unsigned char ssid[WLAN_SSID_MAX];
	size_t ssid_length;
	uid_t owner_uid;
	size_t radio_count;
	int flags;
	int succeeded;

	/* Snapshots only nonsecret identity before a fallible store load. */
	if (managed_wlan.state != NETWORKD_WLAN_RECONNECTING)
		return;
	memset(&result, 0, sizeof(result));
	memset(radios, 0, sizeof(radios));
	wifi_conf_model_init(&model);
	memset(diagnostic, 0, sizeof(diagnostic));
	memcpy(interface, managed_wlan.connection.interface,
	    sizeof(interface));
	ssid_length = managed_wlan.connection.ssid_length;
	memcpy(ssid, managed_wlan.connection.ssid, ssid_length);
	owner_uid = managed_wlan.owner_uid;

	/* Reloads the active owner's exact current profile for this SSID. */
	succeeded = load_policy(owner_uid, &model, diagnostic,
	    sizeof(diagnostic)) == 0;
	profile = succeeded ? find_profile(&model, ssid, ssid_length) : NULL;
	if (profile == NULL)
		succeeded = 0;
	if (succeeded) {
		succeeded = run_wifi(interface, "connect", profile,
		    NETWORKD_WLAN_CONNECT_SECONDS, &result) == 0;
	}
	networkd_wifi_child_result_clear(&result);
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
	wifi_conf_explicit_clear(ssid, sizeof(ssid));

	/* Coalesces every event which arrived while the child owned recovery. */
	(void)process_route_events();
	if (managed_wlan.state != NETWORKD_WLAN_RECONNECTING)
		return;
	if (interface_flags(interface, &flags) != 0 ||
	    (flags & (IFF_UP | IFF_RUNNING)) != (IFF_UP | IFF_RUNNING) ||
	    !managed_connection_usable())
		succeeded = 0;

	/* Retains L3 on success and returns failure to automatic searching. */
	if (succeeded) {
		networkd_managed_wlan_recovery_complete(&managed_wlan, 1);
	} else {
		if (retire_managed_connection(
		    NETWORKD_WLAN_AUTO_SEARCHING, 1) != 0)
			return;
		radio_count = 0U;
		if (enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX,
		    &radio_count) == 0) {
			(void)prepare_wlan_radios(radios, radio_count, NULL, 0U,
			    NULL);
		}
		schedule_automatic_work(NETWORKD_WLAN_RESCAN_SECONDS);
	}
	networkd_protocol_clear(radios, sizeof(radios));
}

/* Schedules one future automatic discovery generation without extending it. */
static void
schedule_automatic_work(
	unsigned delay_seconds)
{
	uint64_t candidate;
	uint64_t now;

	/* Keeps no background deadline outside the automatic-search state. */
	if (managed_wlan.state != NETWORKD_WLAN_AUTO_SEARCHING &&
	    managed_wlan.state != NETWORKD_WLAN_RECONNECTING &&
	    managed_wlan.state != NETWORKD_WLAN_RETIRING) {
		automatic_retry_at = 0U;
		return;
	}
	now = netutil_monotonic_us();
	candidate = now + (uint64_t)delay_seconds * 1000000ULL;
	if (automatic_retry_at == 0U || candidate < automatic_retry_at)
		automatic_retry_at = candidate;
}

/* Computes the event-loop wait until the next automatic generation. */
static int
automatic_poll_timeout(
	void)
{
	uint64_t milliseconds;
	uint64_t now;

	/* Disabled, connected, and manually disconnected policies do not wake. */
	if (managed_wlan.state != NETWORKD_WLAN_AUTO_SEARCHING &&
	    managed_wlan.state != NETWORKD_WLAN_RECONNECTING &&
	    managed_wlan.state != NETWORKD_WLAN_RETIRING) {
		automatic_retry_at = 0U;
		return -1;
	}
	if (automatic_retry_at == 0U)
		schedule_automatic_work(NETWORKD_WLAN_RESCAN_SECONDS);
	now = netutil_monotonic_us();
	if (automatic_retry_at <= now)
		return 0;
	milliseconds = (automatic_retry_at - now + 999ULL) / 1000ULL;
	if (milliseconds > (uint64_t)INT_MAX)
		return INT_MAX;
	return (int)milliseconds;
}

/* Runs one bounded automatic scan, selection, association, and DHCP attempt. */
static void
run_automatic_work(
	void)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	struct wifi_conf_model model;
	char diagnostic[WIFI_CONF_DIAGNOSTIC_MAX];
	size_t radio_count;
	uint64_t deadline;
	int no_candidate;
	int saved;
	int ready;
	int changed;

	/* Claims the current wakeup and initializes all secret-bearing storage. */
	if (managed_wlan.state != NETWORKD_WLAN_AUTO_SEARCHING) {
		automatic_retry_at = 0U;
		return;
	}
	automatic_retry_at = 0U;
	wifi_work_begin(NETWORKD_OP_WIFI_ENABLE, 1);
	memset(radios, 0, sizeof(radios));
	wifi_conf_model_init(&model);
	memset(diagnostic, 0, sizeof(diagnostic));
	radio_count = 0U;
	no_candidate = 0;

	/* Keeps an unconsumed completed scan available to the current profiles. */
	ready = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX,
	    &radio_count) == 0 && radio_count != 0U;
	if (ready)
		ready = prepare_wlan_radios(radios, radio_count, NULL, 0U,
		    NULL) == 0;
	if (ready)
		ready = load_policy(managed_wlan.owner_uid, &model, diagnostic,
		    sizeof(diagnostic)) == 0;
	if (ready) {
		deadline = wifi_work.deadline -
		    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
		if (connect_automatic(radios, radio_count, &model, deadline,
		    NULL, 0U, NULL,
		    &no_candidate) != 0 && !no_candidate &&
		    !wifi_work.cancelled && !wifi_work.profiles_changed) {
			saved = errno != 0 ? errno : EIO;
			fprintf(stderr, "networkd: automatic Wi-Fi attempt: %s\n",
			    strerror(saved));
		}
	}

	/* Releases credentials and retries only after a finite idle interval. */
	wifi_conf_model_clear(&model);
	wifi_conf_explicit_clear(diagnostic, sizeof(diagnostic));
	networkd_protocol_clear(radios, sizeof(radios));
	changed = wifi_work.profiles_changed;
	wifi_work_end();
	if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING) {
		automatic_retry_at = 0U;
		schedule_automatic_work(changed ? 0U : NETWORKD_WLAN_RESCAN_SECONDS);
	}
}

/* Selects the earliest volatile networkd deadline. */
static int
event_poll_timeout(
	void)
{
	int automatic;
	int rollback;

	automatic = networkd_confirmed_active(&confirmed) ? -1 :
	    automatic_poll_timeout();
	rollback = networkd_confirmed_poll_timeout(&confirmed,
	    netutil_monotonic_us());
	if (automatic < 0)
		return rollback;
	if (rollback < 0)
		return automatic;
	return automatic < rollback ? automatic : rollback;
}

/* Executes every volatile deadline which is due at this event-loop turn. */
static void
run_confirmed_due(
	void)
{
	char diagnostic[NETWORKD_RESPONSE_MAX];
	int result;

	diagnostic[0] = '\0';
	result = networkd_confirmed_run_due(&confirmed, netutil_monotonic_us(),
	    rollback_execute, NULL, diagnostic, sizeof(diagnostic));
	if (result < 0) {
		fprintf(stderr, "networkd: confirmed rollback degraded: %s",
		    diagnostic[0] != '\0' ? diagnostic : "operation failed\n");
	} else if (result > 0) {
		fprintf(stderr, "networkd: confirmed rollback expired and ran\n");
	}
	networkd_protocol_clear(diagnostic, sizeof(diagnostic));
}

/* Executes confirmed expiry first, then any independent WLAN retry. */
static void
run_due_work(
	void)
{
	run_confirmed_due();
	if (!networkd_confirmed_active(&confirmed) &&
	    automatic_poll_timeout() == 0) {
		if (managed_wlan.state == NETWORKD_WLAN_RETIRING)
			retry_managed_retirement();
		else if (managed_wlan.state == NETWORKD_WLAN_RECONNECTING)
			recover_managed_wlan();
		else
			run_automatic_work();
	}
}

/* Retires the sole connection while preserving its active policy owner. */
static int
retire_managed_connection(
	enum networkd_managed_wlan_state next_state,
	int normalize)
{
	struct networkd_managed_wlan_connection *connection;
	struct networkd_wifi_child_result result;
	const char *cleanup_stage;
	unsigned child_records;
	int child_terminal;
	int child_exit;
	int child_signal;
	int disconnect_error;
	int l3_error;
	int cleanup_error;

	/* Moves an already idle enabled policy directly to its requested state. */
	connection = &managed_wlan.connection;
	if (networkd_managed_wlan_begin_retire(&managed_wlan, next_state) != 0)
		return -1;
	if (connection->interface[0] == '\0')
		return networkd_managed_wlan_finish_connection(&managed_wlan,
		    next_state);
	if (!normalize || connection->device_removed)
		return retire_removed_connection(next_state);
	cleanup_error = 0;
	cleanup_stage = "identity";
	disconnect_error = 0;
	l3_error = 0;
	child_terminal = 0;
	child_exit = 0;
	child_signal = 0;
	child_records = 0U;

	/* Never mutates a later device which reused the recorded identity. */
	if (normalize) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0) {
			cleanup_error = errno != 0 ? errno : ENODEV;
			if (cleanup_error == ENODEV || cleanup_error == ENXIO) {
				connection->device_removed = 1;
				return retire_removed_connection(next_state);
			}
		} else {
			memset(&result, 0, sizeof(result));
			wifi_work.cleanup = 1;
			if (run_wifi(connection->interface, "disconnect", NULL,
			    10U, &result) != 0)
				cleanup_error = errno != 0 ? errno : EIO;
			wifi_work.cleanup = 0;
			disconnect_error = cleanup_error;
			cleanup_stage = "wifi-disconnect";
			child_terminal = result.terminal_error;
			child_exit = result.child_exit_status;
			child_signal = result.child_term_signal;
			child_records = result.output_records;
			networkd_wifi_child_result_clear(&result);
			if (cleanup_error == 0 && connection->l3_pending &&
			    reconcile_pending_l3() != 0) {
				cleanup_error = errno != 0 ? errno : EIO;
				l3_error = cleanup_error;
				cleanup_stage = "l3-snapshot";
			}
			/* Preserve L3 until L2 retirement is proven, then retry as needed. */
			if (cleanup_error == 0 && connection->owns_l3) {
				if (clear_interface_l3(&managed_wlan) != 0) {
					l3_error = errno;
					if (cleanup_error == 0) {
						cleanup_error = l3_error;
						cleanup_stage = "l3";
					}
				} else {
					connection->owns_l3 = 0;
					networkd_protocol_clear(&connection->l3,
					    sizeof(connection->l3));
				}
			}
		}
		if (cleanup_error != 0) {
			fprintf(stderr,
			    "networkd: %s: managed cleanup degraded: %s\n",
			    connection->interface, strerror(cleanup_error));
			/* Distinguish child/setup failure from later L3 cleanup without
			 * exposing any child output, diagnostics, or connection identity. */
			fprintf(stderr,
			    "networkd: managed-cleanup stage=%s error=%d "
			    "disconnect-error=%d l3-error=%d child-error=%d "
			    "child-exit=%d child-signal=%d child-records=%u\n",
			    cleanup_stage, cleanup_error, disconnect_error, l3_error,
			    child_terminal, child_exit, child_signal, child_records);
		}
	}

	/* Retains the exact ownership token until cleanup can be retried. */
	if (cleanup_error != 0) {
		schedule_retirement_retry();
		errno = cleanup_error;
		return -1;
	}
	(void)networkd_managed_wlan_finish_connection(&managed_wlan,
	    next_state);
	return 0;
}

/* A removed device owns no live interface tuple; preserve any replacement. */
static int
retire_removed_connection(
	enum networkd_managed_wlan_state next_state)
{
	struct networkd_managed_wlan_connection *connection;
	struct networkd_managed_l3 current;
	int saved;

	connection = &managed_wlan.connection;
	/* Interface resources vanished; a partial DHCP resolver file may remain. */
	if (connection->l3_pending) {
		memset(&current, 0, sizeof(current));
		if (snapshot_resolver(&current) != 0) {
			saved = errno;
			networkd_protocol_clear(&current, sizeof(current));
			schedule_retirement_retry();
			errno = saved;
			return -1;
		}
		identify_l3_ownership(connection->interface, &connection->l3_before,
		    &current);
		(void)networkd_managed_wlan_track_l3(&managed_wlan, &current);
		networkd_protocol_clear(&current, sizeof(current));
	}
	if (connection->owns_l3 && connection->l3.resolver_owned &&
	    unlink_owned_resolver(&managed_wlan) != 0 &&
	    errno != ESTALE && errno != ENOENT) {
		saved = errno != 0 ? errno : EIO;
		schedule_retirement_retry();
		errno = saved;
		return -1;
	}
	return networkd_managed_wlan_finish_connection(&managed_wlan, next_state);
}

/* Clears policy only after bounded normalization succeeds; failures retain ownership. */
static int
retire_managed_policy(
	void)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_count;
	int first_error;

	/* Retires exact managed state before applying the global disabled state. */
	wifi_work_begin(NETWORKD_OP_WIFI_DISABLE, 0);
	memset(radios, 0, sizeof(radios));
	radio_count = 0U;
	first_error = 0;
	if (managed_wlan.state != NETWORKD_WLAN_DISABLED &&
	    retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1) != 0)
		first_error = errno != 0 ? errno : EIO;

	/* A service start or stop leaves every discovered radio physically down. */
	if (enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX,
	    &radio_count) != 0) {
		if (first_error == 0)
			first_error = errno != 0 ? errno : EIO;
	} else if (stop_wlan_radios(radios, radio_count, 1) != 0 &&
	    first_error == 0) {
		first_error = errno != 0 ? errno : EIO;
	}
	networkd_protocol_clear(radios, sizeof(radios));
	wifi_work_end();
	if (first_error != 0) {
		errno = first_error;
		return -1;
	}
	networkd_managed_wlan_init(&managed_wlan);
	return 0;
}

/* Supports the notify init operation. */
static void
notify_init(
	const char *record)
{
	const char *value;

	value = getenv("KERN_NOTIFY_FD");

	/* Handles the value availability. */
	if (value == NULL || strcmp(value, "3") != 0)
		return;
	(void)write_all(3, record, strlen(record));
	(void)close(3);
}

/* Supports the write all operation. */
static int
write_all(
	int descriptor,
	const char *buffer,
	size_t length)
{
	ssize_t count;
	size_t offset;

	/* Process each remaining element. */
	offset = 0;
	while (offset < length) {
		count = write(descriptor, buffer + offset, length - offset);

		/* Handles the reported system error. */
		if (count < 0 && errno == EINTR)
			continue;

		/* Checks the remaining item count. */
		if (count <= 0)
			return -1;
		offset += (size_t)count;
	}

	/* Reports successful completion. */
	return 0;
}

/* Records the peer credential decision without exposing request contents. */
static void
write_auth_log(
	const struct kern_peercred *peer,
	enum networkd_client_role role,
	int error)
{
	const char fallback[] =
	    "networkd: auth result=denied reason=record-overflow\n";
	char record[NETWORKD_AUTH_LOG_MAX];
	int length;

	/* Handles an operation failure. */
	if (error != 0 || peer == NULL) {
		length = snprintf(record, sizeof(record),
		    "networkd: auth result=denied reason=peercred error=%d",
		    error != 0 ? error : EACCES);
	} else {
		length = snprintf(record, sizeof(record),
		    "networkd: auth result=admitted role=%s pid=%ld euid=%lu egid=%lu",
		    role == NETWORKD_CLIENT_ROOT ? "root-all" : "network-member",
		    (long)peer->pid, (unsigned long)peer->euid,
		    (unsigned long)peer->egid);
	}

	/* Checks the current data length. */
	if (length < 0 || (size_t)length > sizeof(record) - 2U) {
		(void)fputs(fallback, stderr);

		/* Returns the computed result. */
		return;
	}
	record[length++] = '\n';
	record[length] = '\0';
	(void)fputs(record, stderr);
}

/* Obtains immutable credentials for the connected Unix-domain peer. */
static int
authenticate_client(
	int client,
	struct kern_peercred *peer,
	enum networkd_client_role *role)
{
	socklen_t length;
	int error;

	/* Handles the peer availability. */
	if (peer == NULL || role == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memset(peer, 0, sizeof(*peer));
	length = sizeof(*peer);

	/* Handles a failed getsockopt operation. */
	if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, peer, &length) != 0) {
		error = errno != 0 ? errno : EACCES;
		write_auth_log(NULL, NETWORKD_CLIENT_MEMBER, error);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}

	/* Checks the current data length. */
	if (length != sizeof(*peer) || peer->pid < 0) {
		error = EINVAL;
		write_auth_log(NULL, NETWORKD_CLIENT_MEMBER, error);
		errno = error;

		/* Reports operation failure. */
		return -1;
	}
	*role = peer->euid == 0 ? NETWORKD_CLIENT_ROOT
				 : NETWORKD_CLIENT_MEMBER;
	write_auth_log(peer, *role, 0);

	/* Reports successful completion. */
	return 0;
}

/* Restricts non-root clients to read-only state inspection. */
static int
operation_allowed(
	enum networkd_client_role role,
	const char *operation)
{
	int function_result;

	/* Handles the role condition. */
	if (role == NETWORKD_CLIENT_ROOT)
		return 1;

	/* Admits inspection and global WLAN policy operations for socket members. */
	function_result = operation != NULL &&
	    (strcmp(operation, "SHOW") == 0 ||
	    strcmp(operation, "WIFI_ENABLE") == 0 ||
	    strcmp(operation, "WIFI_DISABLE") == 0 ||
	    strcmp(operation, "WIFI_LIST") == 0 ||
	    strcmp(operation, "WIFI_CONNECT") == 0 ||
	    strcmp(operation, "WIFI_DISCONNECT") == 0 ||
	    strcmp(operation, "WIFI_PROFILES_CHANGED") == 0);

	/* Returns the computed result. */
	return function_result;
}

/* Reads one request before dispatching it through the common admission path. */
static void
handle_request(
	int client,
	enum networkd_client_role role,
	const struct kern_peercred *peer)
{
	struct networkd_request request;

	/* Retains decoded storage until the synchronous dispatcher returns. */
	memset(&request, 0, sizeof(request));
	if (read_request(client, &request) != 0)
		send_error(client, errno, "malformed request");
	else
		dispatch_request(client, &request, role, peer);
	networkd_protocol_clear(&request, sizeof(request));
}

/* Applies the same admission and transaction checks to immediate and queued work. */
static void
dispatch_request(
	int client,
	struct networkd_request *request,
	enum networkd_client_role role,
	const struct kern_peercred *peer)
{
	char response[NETWORKD_RESPONSE_MAX];
	char diagnostic[CHILD_OUTPUT_MAX];
	const char *operation;
	uint64_t mutation_deadline;
	size_t response_length;
	int result;
	int error;

	/* Initializes one terminal response without consuming transport again. */
	result = -1;
	error = EINVAL;
	response_length = 0U;
	response[0] = '\0';
	diagnostic[0] = '\0';
	mutation_deadline = 0U;

	/* Applies peer authorization before operation dispatch. */
	NCOM_TRACE("daemon-read-done", request->header.opcode);
	operation = operation_name(request->header.opcode);
	if (operation == NULL || !operation_allowed(role, operation)) {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR, EPERM,
		    "authorization", NULL, 0U);
		return;
	}

	/* A request arriving with the timer event cannot disarm an expired owner. */
	run_confirmed_due();

	/* Owns confirmed-commit control without ever opening net.conf. */
	if (request->header.opcode >= NETWORKD_OP_CONFIRMED_ARM &&
	    request->header.opcode <= NETWORKD_OP_CONFIRMED_CHECK) {
		if (request->header.opcode == NETWORKD_OP_CONFIRMED_CHECK) {
			result = networkd_confirmed_check(&confirmed, request->token);
			error = result == 0 ? 0 : errno;
		} else if (request->header.opcode == NETWORKD_OP_CONFIRMED_ARM) {
			result = networkd_confirmed_arm(&confirmed,
			    request->rollback_path, peer->euid, request->timeout,
			    netutil_monotonic_us(), rollback_validate, NULL,
			    &request->token, diagnostic, sizeof(diagnostic));
			error = result == 0 ? 0 : errno;
			if (result == 0) {
				send_token_response(client, request->header.request_id,
				    request->header.opcode, request->token);
				networkd_protocol_clear(diagnostic,
				    sizeof(diagnostic));
				return;
			}
		} else if (request->header.opcode ==
		    NETWORKD_OP_CONFIRMED_DISARM) {
			result = networkd_confirmed_disarm(&confirmed,
			    request->token);
			error = result == 0 ? 0 : errno;
		} else {
			result = networkd_confirmed_rollback(&confirmed,
			    rollback_execute, NULL, response, sizeof(response));
			error = result == 0 ? 0 : errno;
			response_length = strlen(response);
		}
		if (result == 0) {
			send_response(client, request->header.request_id,
			    request->header.opcode, NETWORKD_RESULT_OK, 0, NULL,
			    response_length != 0U ? response : NULL,
			    response_length);
		} else {
			send_response(client, request->header.request_id,
			    request->header.opcode,
			    request->header.opcode == NETWORKD_OP_CONFIRMED_ROLLBACK &&
			    response_length != 0U ? NETWORKD_RESULT_DEGRADED :
			    NETWORKD_RESULT_ERROR, error != 0 ? error : EIO,
			    diagnostic[0] != '\0' ? diagnostic : operation,
			    response_length != 0U ? response : NULL,
			    response_length);
		}
		networkd_protocol_clear(response, sizeof(response));
		networkd_protocol_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/* Serializes every wired mutation with the volatile transaction owner. */
	if (request->header.opcode >= NETWORKD_OP_UP &&
	    request->header.opcode <= NETWORKD_OP_DNS_CLEAR &&
	    request->header.opcode != NETWORKD_OP_RELOAD &&
	    networkd_confirmed_check(&confirmed, request->token) != 0) {
		error = errno != 0 ? errno : EBUSY;
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR, error,
		    "confirmed transaction", NULL, 0U);
		return;
	}
	if (request->token != 0U)
		mutation_deadline = confirmed.deadline;

	/* WLAN mutations cannot occupy the loop past a wired rollback deadline. */
	if (networkd_confirmed_active(&confirmed) &&
	    request->header.opcode >= NETWORKD_OP_WIFI_ENABLE &&
	    request->header.opcode <= NETWORKD_OP_WIFI_PROFILES_CHANGED &&
	    request->header.opcode != NETWORKD_OP_WIFI_LIST) {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR, EBUSY,
		    "confirmed transaction", NULL, 0U);
		return;
	}

	/* Delegates the complete typed WLAN family to its bounded orchestrator. */
	if (request->header.opcode >= NETWORKD_OP_WIFI_ENABLE &&
	    request->header.opcode <= NETWORKD_OP_WIFI_PROFILES_CHANGED) {
		handle_wifi_request(client, request, peer);
		networkd_protocol_clear(response, sizeof(response));
		networkd_protocol_clear(diagnostic, sizeof(diagnostic));
		return;
	}

	/* Dispatches the validated operation through absolute child paths. */
	NCOM_TRACE("daemon-execute-enter", request->header.opcode);
	result = execute_wired_request(request, response, sizeof(response),
	    diagnostic, sizeof(diagnostic), &response_length, &error,
	    mutation_deadline);
	NCOM_TRACE("daemon-execute-done", request->header.opcode);
	if (mutation_deadline != 0U &&
	    netutil_monotonic_us() >= mutation_deadline) {
		run_confirmed_due();
		result = -1;
		error = ETIMEDOUT;
		(void)snprintf(diagnostic, sizeof(diagnostic),
		    "confirmed transaction expired");
	}

	/* Sends one correlated terminal response and clears request storage. */
	NCOM_TRACE("daemon-response-enter", request->header.opcode);
	if (result == 0) {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_OK, 0, NULL,
		    response_length != 0U ? response : NULL, response_length);
	} else {
		send_response(client, request->header.request_id,
		    request->header.opcode, NETWORKD_RESULT_ERROR,
		    error != 0 ? error : EIO,
		    diagnostic[0] != '\0' ? diagnostic : operation,
		    NULL, 0U);
	}
	NCOM_TRACE("daemon-response-done", request->header.opcode);
	networkd_protocol_clear(response, sizeof(response));
	networkd_protocol_clear(diagnostic, sizeof(diagnostic));
}

/* Executes one decoded non-WLAN request for a client or rollback program. */
static int
execute_wired_request(
	struct networkd_request *request,
	char *response,
	size_t response_capacity,
	char *diagnostic,
	size_t diagnostic_capacity,
	size_t *response_length,
	int *error,
	uint64_t deadline)
{
	struct in_addr address;
	struct in_addr mask;
	struct in_addr gateway;
	unsigned prefix;
	char seconds[16];
	char *arguments[16];
	char *dns[8];
	unsigned index;
	int present;
	int result;

	if (request == NULL || diagnostic == NULL ||
	    diagnostic_capacity < CHILD_OUTPUT_MAX || response_length == NULL ||
	    error == NULL) {
		errno = EINVAL;
		return -1;
	}
	result = -1;
	*error = EINVAL;
	*response_length = 0U;
	diagnostic[0] = '\0';
	if (request->header.opcode == NETWORKD_OP_SHOW) {
		if (response != NULL && response_capacity != 0U &&
		    show_interfaces(request->interface[0] != '\0' ?
		    request->interface : NULL, response, response_capacity) == 0) {
			*response_length = strlen(response);
			result = 0;
		}
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_UP ||
	    request->header.opcode == NETWORKD_OP_DOWN) {
		arguments[0] = "/sbin/ifconfig";
		arguments[1] = request->interface;
		arguments[2] = request->header.opcode == NETWORKD_OP_UP ?
		    "up" : "down";
		arguments[3] = NULL;
		if (interface_exists(request->interface) == 0)
			result = run_command_until(arguments, 10, deadline, diagnostic);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_STATIC) {
		if (interface_exists(request->interface) == 0 &&
		    netutil_parse_ipv4(request->address, &address) == 0 &&
		    netutil_parse_ipv4(request->netmask, &mask) == 0 &&
		    netutil_mask_prefix(mask, &prefix) == 0) {
			arguments[0] = "/sbin/ifconfig";
			arguments[1] = request->interface;
			arguments[2] = "inet";
			arguments[3] = request->address;
			arguments[4] = "netmask";
			arguments[5] = request->netmask;
			arguments[6] = NULL;
			result = run_command_until(arguments, 10, deadline, diagnostic);
		}
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DHCP) {
		(void)snprintf(seconds, sizeof(seconds), "%u", request->timeout);
		arguments[0] = "/sbin/dhcpc";
		arguments[1] = "-t";
		arguments[2] = seconds;
		arguments[3] = request->interface;
		arguments[4] = NULL;
		if (interface_exists(request->interface) == 0)
			result = run_command_until(arguments, request->timeout + 5U,
			    deadline, diagnostic);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DEFAULT_ROUTE) {
		if (netutil_parse_ipv4(request->gateway, &gateway) == 0 &&
		    (present = default_route_exists()) >= 0) {
			if (present) {
				result = 0;
			} else {
				arguments[0] = "/sbin/route";
				arguments[1] = "add";
				arguments[2] = "default";
				arguments[3] = request->gateway;
				arguments[4] = NULL;
				result = run_command_until(arguments, 10, deadline,
				    diagnostic);
			}
		}
		*error = errno;
	} else if (request->header.opcode ==
	    NETWORKD_OP_DEFAULT_ROUTE_CLEAR) {
		present = default_route_exists();
		if (present == 0) {
			result = 0;
		} else if (present > 0) {
			arguments[0] = "/sbin/route";
			arguments[1] = "delete";
			arguments[2] = "default";
			arguments[3] = NULL;
			result = run_command_until(arguments, 10, deadline, diagnostic);
		}
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DNS) {
		for (index = 0U; index < request->dns_count; index++)
			dns[index] = request->dns[index];
		result = write_resolver(dns, (int)request->dns_count);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_DNS_CLEAR) {
		result = write_resolver(NULL, 0);
		*error = errno;
	} else if (request->header.opcode == NETWORKD_OP_RELOAD) {
		result = 0;
		*error = 0;
	} else {
		*error = EOPNOTSUPP;
	}
	if (result != 0 && *error == 0)
		*error = EIO;
	return result;
}

/* Validates one rollback line without changing running state. */
static int
rollback_validate(
	const char *line,
	char *diagnostic,
	size_t capacity,
	void *context)
{
	struct networkd_request request;

	(void)context;
	return rollback_parse(line, &request, diagnostic, capacity);
}

/* Executes one already prevalidated rollback line. */
static int
rollback_execute(
	const char *line,
	char *diagnostic,
	size_t capacity,
	void *context)
{
	struct networkd_request request;
	char output[1];
	size_t output_length;
	int error;

	(void)context;
	if (rollback_parse(line, &request, diagnostic, capacity) != 0)
		return -1;
	if (execute_wired_request(&request, output, sizeof(output), diagnostic,
	    capacity, &output_length, &error, 0U) != 0) {
		if (diagnostic[0] == '\0')
			(void)snprintf(diagnostic, capacity, "%s", strerror(error));
		errno = error;
		return -1;
	}
	return 0;
}

/* Parses the canonical one-command-per-line rollback language. */
static int
rollback_parse(
	const char *line,
	struct networkd_request *request,
	char *diagnostic,
	size_t capacity)
{
	char copy[NETWORKD_ROLLBACK_LINE_MAX + 1U];
	char *word[16];
	char *token;
	char *end;
	struct in_addr parsed;
	struct in_addr mask;
	unsigned prefix;
	unsigned long timeout;
	size_t length;
	unsigned count;
	unsigned index;

#define ROLLBACK_REJECT(text) do { \
	if (diagnostic != NULL && capacity != 0U) \
		(void)snprintf(diagnostic, capacity, "%s", text); \
	errno = EINVAL; \
	return -1; \
} while (0)
	if (line == NULL || request == NULL ||
	    (length = strlen(line)) == 0U || length > NETWORKD_ROLLBACK_LINE_MAX)
		ROLLBACK_REJECT("invalid rollback line length");
	for (index = 0U; index < length; index++) {
		if ((unsigned char)line[index] < 32U ||
		    (unsigned char)line[index] > 126U)
			ROLLBACK_REJECT("non-printable rollback byte");
	}
	strcpy(copy, line);
	count = 0U;
	for (token = strtok(copy, " "); token != NULL;
	    token = strtok(NULL, " ")) {
		if (count == sizeof(word) / sizeof(word[0]))
			ROLLBACK_REJECT("too many rollback operands");
		word[count++] = token;
	}
	if (count < 2U || strcmp(word[0], "V1") != 0)
		ROLLBACK_REJECT("invalid rollback version");
	memset(request, 0, sizeof(*request));
	if ((strcmp(word[1], "UP") == 0 || strcmp(word[1], "DOWN") == 0) &&
	    count == 3U) {
		request->header.opcode = strcmp(word[1], "UP") == 0 ?
		    NETWORKD_OP_UP : NETWORKD_OP_DOWN;
		if (strlen(word[2]) >= sizeof(request->interface))
			ROLLBACK_REJECT("invalid rollback interface");
		strcpy(request->interface, word[2]);
	} else if (strcmp(word[1], "DHCP") == 0 && count == 4U) {
		errno = 0;
		timeout = strtoul(word[3], &end, 10);
		if (errno != 0 || end == word[3] || *end != '\0' || timeout == 0U ||
		    timeout > 3600U || strlen(word[2]) >= sizeof(request->interface))
			ROLLBACK_REJECT("invalid rollback DHCP");
		request->header.opcode = NETWORKD_OP_DHCP;
		request->timeout = (unsigned)timeout;
		strcpy(request->interface, word[2]);
	} else if (strcmp(word[1], "STATIC") == 0 && count == 7U &&
	    strcmp(word[3], "ipv4") == 0 && strcmp(word[5], "netmask") == 0) {
		if (strlen(word[2]) >= sizeof(request->interface) ||
		    strlen(word[4]) >= sizeof(request->address) ||
		    strlen(word[6]) >= sizeof(request->netmask) ||
		    netutil_parse_ipv4(word[4], &parsed) != 0 ||
		    netutil_parse_ipv4(word[6], &mask) != 0 ||
		    netutil_mask_prefix(mask, &prefix) != 0)
			ROLLBACK_REJECT("invalid rollback static address");
		request->header.opcode = NETWORKD_OP_STATIC;
		strcpy(request->interface, word[2]);
		strcpy(request->address, word[4]);
		strcpy(request->netmask, word[6]);
	} else if (strcmp(word[1], "DEFAULTROUTE") == 0 && count == 3U) {
		if (strlen(word[2]) >= sizeof(request->gateway) ||
		    netutil_parse_ipv4(word[2], &parsed) != 0)
			ROLLBACK_REJECT("invalid rollback default route");
		request->header.opcode = NETWORKD_OP_DEFAULT_ROUTE;
		strcpy(request->gateway, word[2]);
	} else if (strcmp(word[1], "DEFAULTROUTE_CLEAR") == 0 && count == 2U) {
		request->header.opcode = NETWORKD_OP_DEFAULT_ROUTE_CLEAR;
	} else if (strcmp(word[1], "DNS_CLEAR") == 0 && count == 2U) {
		request->header.opcode = NETWORKD_OP_DNS_CLEAR;
	} else if (strcmp(word[1], "DNS") == 0 && count >= 3U && count <= 10U) {
		request->header.opcode = NETWORKD_OP_DNS;
		request->dns_count = count - 2U;
		for (index = 0U; index < request->dns_count; index++) {
			if (strlen(word[index + 2U]) >= sizeof(request->dns[index]) ||
			    netutil_parse_ipv4(word[index + 2U], &parsed) != 0)
				ROLLBACK_REJECT("invalid rollback DNS");
			strcpy(request->dns[index], word[index + 2U]);
		}
	} else {
		ROLLBACK_REJECT("unsupported rollback operation");
	}
	if ((request->header.opcode == NETWORKD_OP_UP ||
	    request->header.opcode == NETWORKD_OP_DOWN ||
	    request->header.opcode == NETWORKD_OP_DHCP ||
	    request->header.opcode == NETWORKD_OP_STATIC) &&
	    (request->interface[0] == '\0' ||
	    strspn(request->interface,
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
	    strlen(request->interface)))
		ROLLBACK_REJECT("invalid rollback interface");
	return 0;
#undef ROLLBACK_REJECT
}

/* Prepares radios using one request-owned response buffer. */
static int
wifi_request_prepare(
	struct networkd_wifi_request *work)
{
	/* Retirement may have changed the preflight administrative/scan snapshot. */
	work->stage = "refresh WLAN radios";
	memset(work->radios, 0, sizeof(work->radios));
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;
	work->stage = "prepare WLAN radios";
	return prepare_wlan_radios(work->radios, work->radio_count,
	    work->output, (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE), &work->output_length);
}

/* Lists every radio independently and retains useful partial observations. */
static int
wifi_request_list(
	struct networkd_wifi_request *work)
{
	struct networkd_wifi_child_result child;
	size_t index;
	int first_error;
	int error;
	int count;

	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;
	first_error = 0;
	for (index = 0U; index < work->radio_count; index++) {
		memset(&child, 0, sizeof(child));
		if (work->radios[index].observation_error != 0 ||
		    work->radios[index].stop_flags != 0U) {
			error = work->radios[index].observation_error != 0 ?
			    work->radios[index].observation_error : EBUSY;
			count = snprintf(work->output + work->output_length,
			    sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE - work->output_length,
			    "interface=%s stop-pending=%u status-error=%d\n",
			    work->radios[index].interface,
			    (work->radios[index].stop_flags & WLAN_STATUS_STOP_PENDING) != 0U,
			    work->radios[index].observation_error);
			if (count < 0 || (size_t)count >= sizeof(work->output) -
			    NETWORKD_WIFI_STATUS_RESERVE - work->output_length) {
				errno = EOVERFLOW;
				return -1;
			}
			work->output_length += (size_t)count;
		} else if (run_wifi(work->radios[index].interface, "list", NULL,
		    5U, &child) == 0) {
			error = append_wifi_snapshot(work->radios[index].interface,
			    (const char *)child.output, child.output_length, 0U,
			    work->output, (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE),
			    &work->output_length) == 0 ? 0 : errno;
		} else {
			error = child.terminal_error != 0 ? child.terminal_error : EIO;
			count = snprintf(work->output + work->output_length,
			    (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE) - work->output_length,
			    "interface=%s list-error=%d\n",
			    work->radios[index].interface, error);
			if (count < 0 || (size_t)count >=
			    (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE) - work->output_length) {
				networkd_wifi_child_result_clear(&child);
				errno = EOVERFLOW;
				return -1;
			}
			work->output_length += (size_t)count;
		}
		networkd_wifi_child_result_clear(&child);

		/* Preserves the overflow result even if an earlier radio failed differently. */
		if (error == EOVERFLOW) {
			work->stage = "Wi-Fi list output truncated";
			errno = EOVERFLOW;
			return -1;
		}
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	work->stage = "partial Wi-Fi list";
	errno = first_error;
	return first_error == 0 ? 0 : -1;
}

/* Validates a prospective owner before publishing its enabled intent. */
static int
wifi_request_enable(
	struct networkd_wifi_request *work,
	const struct kern_peercred *peer)
{
	if (wifi_disable_pending) {
		errno = EBUSY;
		return -1;
	}
	work->stage = "load Wi-Fi profiles";
	if (load_policy(peer->euid, &work->profiles, work->diagnostic,
	    sizeof(work->diagnostic)) != 0 && errno != ENOENT)
		return -1;
	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;

	/* An unchanged live owner is already enabled. */
	if (networkd_managed_wlan_owner_matches(&managed_wlan, peer->euid) &&
	    managed_wlan.state == NETWORKD_WLAN_CONNECTED &&
	    managed_connection_usable())
		return 0;

	work->stage = "retire prior Wi-Fi connection";
	if (managed_wlan.state != NETWORKD_WLAN_DISABLED &&
	    retire_managed_connection(NETWORKD_WLAN_AUTO_SEARCHING, 1) != 0)
		return -1;
	work->stage = "enable Wi-Fi policy";
	if (networkd_managed_wlan_enable(&managed_wlan, peer->euid) != 0)
		return -1;

	/* RF association belongs to scheduled work, never to enable dispatch. */
	automatic_candidate_skip = 0U;
	schedule_automatic_work(0U);
	return wifi_request_prepare(work);
}

/* Pauses automatic policy before fallible teardown and radio normalization. */
static int
wifi_request_stop(
	struct networkd_wifi_request *work,
	int disable)
{
	if (!disable && wifi_disable_pending) {
		errno = EBUSY;
		return -1;
	}
	if (disable)
		wifi_disable_pending = 1;
	automatic_retry_at = 0U;
	work->stage = "retire Wi-Fi connection";
	automatic_candidate_skip = 0U;
	if (managed_wlan.owner_valid &&
	    retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1) != 0)
		return disable ? wifi_disable_defer() : -1;
	if (!disable && managed_wlan.state == NETWORKD_WLAN_DISABLED)
		return 0;
	if (!disable)
		return wifi_request_prepare(work);
	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return wifi_disable_defer();
	/* Never report disabled while normalization has an unresolved failure. */
	work->stage = "normalize WLAN radios";
	if (stop_wlan_radios(work->radios, work->radio_count, 1) != 0)
		return wifi_disable_defer();
	work->stage = "disable Wi-Fi policy";
	if (networkd_managed_wlan_disable(&managed_wlan) != 0)
		return wifi_disable_defer();
	wifi_disable_pending = 0;
	return 0;
}

/* Global radio normalization may remain pending even without an L2 token. */
static int
wifi_disable_defer(void)
{
	int saved;

	saved = errno != 0 ? errno : EIO;
	managed_wlan.state = NETWORKD_WLAN_RETIRING;
	managed_wlan.retire_target = NETWORKD_WLAN_MANUAL_DISCONNECTED;
	schedule_retirement_retry();
	errno = saved;
	return -1;
}

/* Performs an explicit target transaction after complete nonmutating preflight. */
static int
wifi_request_connect(
	struct networkd_wifi_request *work,
	const struct networkd_request *request)
{
	const struct wifi_conf_profile *profile;
	uint64_t selection_deadline;
	uint64_t deadline;
	size_t winner;
	int l2_succeeded;

	if (wifi_disable_pending) {
		errno = EBUSY;
		return -1;
	}
	work->stage = "Wi-Fi is disabled";
	if (managed_wlan.state == NETWORKD_WLAN_DISABLED) {
		errno = EPERM;
		return -1;
	}
	work->stage = "load Wi-Fi profiles";
	if (load_policy(managed_wlan.owner_uid, &work->profiles,
	    work->diagnostic, sizeof(work->diagnostic)) != 0)
		return -1;
	work->stage = "unknown Wi-Fi profile";
	profile = find_profile(&work->profiles, request->ssid, request->ssid_length);
	if (profile == NULL) {
		errno = ENOENT;
		return -1;
	}
	work->stage = "enumerate WLAN radios";
	if (enumerate_wlan_radios(work->radios, NETWORKD_WLAN_RADIO_MAX,
	    &work->radio_count) != 0)
		return -1;
	if (work->radio_count == 0U) {
		work->stage = "no WLAN radio";
		errno = ENODEV;
		return -1;
	}

	/* This covers connected, reconnecting, connecting and retiring identities. */
	automatic_retry_at = 0U;
	work->stage = "retire current Wi-Fi connection";
	if (retire_managed_connection(NETWORKD_WLAN_MANUAL_DISCONNECTED, 1) != 0)
		return -1;
	if (wifi_request_prepare(work) != 0)
		return -1;
	deadline = wifi_work.deadline -
	    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
	selection_deadline = netutil_monotonic_us() +
	    NETWORKD_WLAN_SCAN_SECONDS * 1000000ULL;
	if (selection_deadline > deadline)
		selection_deadline = deadline;
	work->stage = "Wi-Fi SSID not visible";
	if (select_manual_radio(work->radios, work->radio_count, profile,
	    &winner, selection_deadline) != 0)
		return -1;
	l2_succeeded = 0;
	work->stage = "wifi connect";
	if (run_managed_connect(work->radios[winner].interface, profile,
	    NETWORKD_WLAN_MANUAL_DISCONNECTED, deadline, work->output,
	    (sizeof(work->output) - NETWORKD_WIFI_STATUS_RESERVE), &work->output_length, &l2_succeeded) != 0) {
		if (l2_succeeded)
			work->stage = "DHCP transaction";
		return -1;
	}
	stop_losing_scans(work->radios, work->radio_count, winner);
	return 0;
}

/* Dispatches one policy operation, then emits and clears one terminal result. */
static void
process_wifi_request(
	int client,
	struct networkd_request *request,
	const struct kern_peercred *peer)
{
	struct networkd_wifi_request work;
	uint32_t opcode;
	uint32_t status;
	int result;
	int error;

	memset(&work, 0, sizeof(work));
	work.stage = "Wi-Fi policy";
	opcode = request->header.opcode;
	result = -1;
	error = EINVAL;

	if (route_events >= 0 && process_route_events() != 0) {
		(void)close(route_events);
		route_events = -1;
	}
	if (peer == NULL) {
		errno = EACCES;
	} else if (opcode != NETWORKD_OP_WIFI_LIST &&
	    opcode != NETWORKD_OP_WIFI_ENABLE &&
	    opcode != NETWORKD_OP_WIFI_PROFILES_CHANGED && !owner_allowed(peer)) {
		work.stage = "Wi-Fi policy owner";
		errno = EPERM;
	} else {
		switch (opcode) {
		case NETWORKD_OP_WIFI_LIST:
			result = wifi_request_list(&work);
			break;
		case NETWORKD_OP_WIFI_ENABLE:
			result = wifi_request_enable(&work, peer);
			break;
		case NETWORKD_OP_WIFI_DISABLE:
			result = wifi_request_stop(&work, 1);
			break;
		case NETWORKD_OP_WIFI_DISCONNECT:
			result = wifi_request_stop(&work, 0);
			break;
		case NETWORKD_OP_WIFI_CONNECT:
			result = wifi_request_connect(&work, request);
			break;
		case NETWORKD_OP_WIFI_PROFILES_CHANGED:
			wifi_profiles_changed(peer);
			result = 0;
			break;
		default:
			errno = EINVAL;
			break;
		}
	}
	error = result == 0 ? 0 : (errno != 0 ? errno : EIO);
	status = result == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_ERROR;
	if (result != 0 && (managed_wlan.state == NETWORKD_WLAN_RETIRING ||
	    opcode == NETWORKD_OP_WIFI_LIST))
		status = NETWORKD_RESULT_DEGRADED;

	/* Makes output exhaustion visible even when complete earlier lines survive. */
	if (opcode == NETWORKD_OP_WIFI_LIST && error == EOVERFLOW) {
		memcpy(work.output + work.output_length, "wifi output-truncated=1\n", 24U);
		work.output_length += 24U;
	}

	/* A policy observation is useful even when one radio or stage failed. */
	if (append_managed_status(work.output, sizeof(work.output),
	    &work.output_length) != 0 && error == 0) {
		error = errno != 0 ? errno : EOVERFLOW;
		status = NETWORKD_RESULT_ERROR;
		work.stage = "Wi-Fi status output";
	}
	send_response(client, request->header.request_id, opcode, status, error,
	    error == 0 ? NULL : (work.diagnostic[0] != '\0' ?
	    work.diagnostic : work.stage), work.output, work.output_length);
	wifi_conf_model_clear(&work.profiles);
	networkd_protocol_clear(&work, sizeof(work));
}

/* Releases one cached radio without changing managed policy. */
static void
clear_wifi_observation(
	size_t slot)
{
	/* Accounts only allocated record bytes, never the metadata array. */
	wifi_observation_bytes -= wifi_observations[slot].output_length;
	free(wifi_observations[slot].output);
	memset(&wifi_observations[slot], 0, sizeof(wifi_observations[slot]));
}

/* Retains nonsecret records under one total response-sized allocation budget. */
static void
remember_wifi_observation(
	const char *interface,
	const struct networkd_wifi_child_result *result)
{
	uint32_t ifindex;
	size_t index;
	size_t slot;
	size_t oldest;
	size_t empty;
	char *output;

	/* Rejects an invalid result or vanished identity before replacing a cache. */
	if (result->output_length == 0U ||
	    result->output_length > NETWORKD_RESPONSE_OUTPUT_MAX ||
	    result->output_records > NETWORKD_WIFI_CHILD_RECORD_MAX ||
	    interface_index(interface, &ifindex) != 0)
		return;

	/* Finds an existing name before considering any earlier empty slot. */
	slot = NETWORKD_WLAN_RADIO_MAX;
	empty = NETWORKD_WLAN_RADIO_MAX;
	oldest = 0U;
	for (index = 0U; index < NETWORKD_WLAN_RADIO_MAX; index++) {
		if (wifi_observations[index].output != NULL &&
		    strcmp(wifi_observations[index].interface, interface) == 0) {
			slot = index;
			break;
		}
		if (wifi_observations[index].output == NULL)
			empty = index;
		if (wifi_observations[index].observed_at <
		    wifi_observations[oldest].observed_at)
			oldest = index;
	}
	if (slot == NETWORKD_WLAN_RADIO_MAX)
		slot = empty == NETWORKD_WLAN_RADIO_MAX ? oldest : empty;
	clear_wifi_observation(slot);

	/* Evicts old observations until the global allocation bound permits a copy. */
	while (wifi_observation_bytes + result->output_length >
	    NETWORKD_RESPONSE_OUTPUT_MAX) {
		oldest = NETWORKD_WLAN_RADIO_MAX;
		for (index = 0U; index < NETWORKD_WLAN_RADIO_MAX; index++) {
			if (wifi_observations[index].output != NULL &&
			    (oldest == NETWORKD_WLAN_RADIO_MAX ||
			    wifi_observations[index].observed_at <
			    wifi_observations[oldest].observed_at))
				oldest = index;
		}
		if (oldest == NETWORKD_WLAN_RADIO_MAX)
			return;
		clear_wifi_observation(oldest);
	}

	/* Allocation failure loses only optional observations, never policy state. */
	output = malloc(result->output_length);
	if (output == NULL)
		return;
	memcpy(output, result->output, result->output_length);
	strcpy(wifi_observations[slot].interface, interface);
	wifi_observations[slot].ifindex = ifindex;
	wifi_observations[slot].observed_at = netutil_monotonic_us();
	wifi_observations[slot].output = output;
	wifi_observations[slot].output_length = result->output_length;
	wifi_observation_bytes += result->output_length;
}

/* Emits the same snapshot metadata for fresh and cached observations. */
static int
append_wifi_snapshot(
	const char *interface,
	const char *records,
	size_t length,
	uint64_t age,
	char *output,
	size_t capacity,
	size_t *used)
{
	char metadata[128];
	int count;
	int result;

	/* Makes missing and evicted caches explicit within the ordinary list grammar. */
	if (records == NULL)
		count = snprintf(metadata, sizeof(metadata),
		    "interface=%s snapshot-age-ms=unknown scan-snapshot=not-yet-observed\n",
		    interface);
	else
		count = snprintf(metadata, sizeof(metadata),
		    "interface=%s snapshot-age-ms=%llu scan-snapshot=available\n",
		    interface, (unsigned long long)age);

	/* Publishes only a complete metadata line. */
	if (count < 0 || (size_t)count >= sizeof(metadata) ||
	    *used > capacity || (size_t)count > capacity - *used) {
		errno = EOVERFLOW;
		return -1;
	}
	memcpy(output + *used, metadata, (size_t)count);
	*used += (size_t)count;

	/* Appends available records using the same converter as fresh results. */
	if (records != NULL) {
		result = append_wifi_records(interface, records, length,
		    output, capacity, used);
		return result;
	}
	return 0;
}

/* Observes an ongoing actor without allocating another policy/credential frame. */
static void
send_wifi_observation(
	int client,
	const struct networkd_request *request)
{
	struct networkd_wlan_radio radios[NETWORKD_WLAN_RADIO_MAX];
	char *output;
	size_t capacity;
	size_t used;
	size_t count;
	size_t radio;
	size_t slot;
	uint64_t now;
	uint64_t age;
	int result;
	int saved;

	/* Keeps this callback's stack independent of the credential and record limits. */
	capacity = NETWORKD_RESPONSE_OUTPUT_MAX - NETWORKD_DIAGNOSTIC_MAX - 4U;
	output = malloc(capacity);
	if (output == NULL) {
		send_response(client, request->header.request_id, request->header.opcode,
		    NETWORKD_RESULT_ERROR, ENOMEM, "Wi-Fi observation", NULL, 0U);
		return;
	}
	used = 0U;
	count = 0U;
	result = enumerate_wlan_radios(radios, NETWORKD_WLAN_RADIO_MAX, &count);
	now = netutil_monotonic_us();

	/* Matches cached data against both the current name and interface identity. */
	for (radio = 0U; result == 0 && radio < count; radio++) {
		for (slot = 0U; slot < NETWORKD_WLAN_RADIO_MAX; slot++) {
			if (wifi_observations[slot].output != NULL &&
			    wifi_observations[slot].ifindex == radios[radio].ifindex &&
			    strcmp(wifi_observations[slot].interface,
			    radios[radio].interface) == 0)
				break;
		}
		age = 0U;
		if (slot != NETWORKD_WLAN_RADIO_MAX &&
		    now >= wifi_observations[slot].observed_at)
			age = (now - wifi_observations[slot].observed_at) / 1000U;
		result = append_wifi_snapshot(radios[radio].interface,
		    slot == NETWORKD_WLAN_RADIO_MAX ? NULL : wifi_observations[slot].output,
		    slot == NETWORKD_WLAN_RADIO_MAX ? 0U : wifi_observations[slot].output_length,
		    age, output, capacity - NETWORKD_WIFI_STATUS_RESERVE, &used);
	}

	/* Reserves an explicit truncation marker and final state even for partial output. */
	saved = result == 0 ? 0 : (errno != 0 ? errno : EIO);
	if (saved == EOVERFLOW) {
		memcpy(output + used, "wifi output-truncated=1\n", 24U);
		used += 24U;
	}
	if (append_managed_status(output, capacity, &used) != 0 && saved == 0)
		saved = errno != 0 ? errno : EOVERFLOW;
	send_response(client, request->header.request_id, request->header.opcode,
	    saved == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_DEGRADED, saved,
	    saved == 0 ? NULL : "Wi-Fi observation", output, used);
	free(output);
}

/* Serves read-only wired status without entering a request or child transaction. */
static void
send_wired_observation(
	int client,
	const struct networkd_request *request)
{
	char *output;
	int result;
	int error;

	/* Uses a bounded temporary buffer instead of growing the active RF stack. */
	output = malloc(NETWORKD_RESPONSE_OUTPUT_MAX);
	if (output == NULL) {
		send_response(client, request->header.request_id, request->header.opcode,
		    NETWORKD_RESULT_ERROR, ENOMEM, "SHOW", NULL, 0U);
		return;
	}
	result = show_interfaces(request->interface[0] != '\0' ?
	    request->interface : NULL, output, NETWORKD_RESPONSE_OUTPUT_MAX);
	error = result == 0 ? 0 : (errno != 0 ? errno : EIO);
	send_response(client, request->header.request_id, request->header.opcode,
	    result == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_ERROR, error,
	    result == 0 ? NULL : "SHOW", result == 0 ? output : NULL,
	    result == 0 ? strlen(output) : 0U);
	free(output);
}

/* Preserves local profile publication while waking the applicable policy only. */
static void
wifi_profiles_changed(
	const struct kern_peercred *peer)
{
	size_t index;

	if (peer == NULL || !networkd_managed_wlan_owner_matches(&managed_wlan,
	    peer->euid))
		return;
	automatic_candidate_skip = 0U;
	for (index = 0U; index < known_wlan_radio_count; index++)
		known_wlan_radios[index].consumed_snapshot_generation = 0U;
	if (wifi_work.background)
		wifi_work.profiles_changed = 1;
	if (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING)
		schedule_automatic_work(0U);
}

/* Resumes deferred control only after the interrupted actor has unwound. */
static void
dispatch_pending_wifi(
	void)
{
	int client;

	client = wifi_pending_client;
	wifi_pending_client = -1;
	dispatch_request(client, &wifi_pending.request, wifi_pending.role,
	    &wifi_pending.peer);
	(void)close(client);
	networkd_protocol_clear(&wifi_pending, sizeof(wifi_pending));
}

/* Reports whether the outer loop must continue receiving an accepted request. */
static int
control_input_pending(
	void)
{
	size_t index;

	/* Retains a short poll interval only while bounded ingress slots are occupied. */
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		if (control_inputs[index].active)
			return 1;
	}
	return 0;
}

/* Receives available bytes without charging a stalled client to the RF wait. */
static int
receive_wait_request(
	struct networkd_request *request,
	struct kern_peercred *peer,
	enum networkd_client_role *role)
{
	struct networkd_control_input *input;
	struct pollfd ready;
	struct timeval timeout;
	unsigned char *newline;
	size_t index;
	size_t header_length;
	size_t iteration;
	ssize_t count;
	int client;
	int error;
	int complete;

	/* Admits at most one new peer per callback under a fixed four-client bound. */
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		if (!control_inputs[index].active)
			break;
	}
	ready.fd = control_listener;
	ready.events = POLLIN;
	ready.revents = 0;
	if (index < NETWORKD_CONTROL_INPUT_MAX && poll(&ready, 1U, 0) > 0 &&
	    (ready.revents & POLLIN) != 0) {
		client = accept4(control_listener, NULL, NULL, SOCK_CLOEXEC);
		if (client >= 0) {
			input = &control_inputs[index];
			timeout.tv_sec = 0;
			timeout.tv_usec = 200000;

			/* Bounds response backpressure independently of partial request input. */
			if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
			    sizeof(timeout)) != 0 ||
			    authenticate_client(client, &input->peer, &input->role) != 0) {
				(void)close(client);
				memset(input, 0, sizeof(*input));
			} else {
				input->descriptor = client;
				input->deadline = netutil_monotonic_us() + 5000000ULL;
				input->active = 1;
			}
		}
	}

	/* Consumes bounded chunks, including EOF, without waiting for another byte. */
	for (index = 0U; index < NETWORKD_CONTROL_INPUT_MAX; index++) {
		input = &control_inputs[index];
		if (!input->active)
			continue;
		error = 0;
		complete = 0;
		if (netutil_monotonic_us() >= input->deadline)
			error = ETIMEDOUT;
		for (iteration = 0U; error == 0 && iteration < 2U; iteration++) {
			count = recv(input->descriptor, input->bytes + input->used,
			    sizeof(input->bytes) - input->used, MSG_DONTWAIT);
			if (count < 0) {
				if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
					error = errno != 0 ? errno : EIO;
				break;
			}
			if (count == 0) {
				complete = 1;
				break;
			}
			input->used += (size_t)count;
			if (input->used == sizeof(input->bytes))
				error = EMSGSIZE;
		}

		/* Requires an exact header/payload and write-side EOF before admission. */
		if (complete && error == 0) {
			newline = memchr(input->bytes, '\n', input->used);
			header_length = newline == NULL ? 0U :
			    (size_t)(newline - input->bytes) + 1U;
			if (header_length == 0U || header_length > NETWORKD_PROTOCOL_HEADER_MAX) {
				error = EINVAL;
			} else if (networkd_protocol_header_decode((char *)input->bytes,
			    header_length, &request->header) != 0) {
				error = errno != 0 ? errno : EINVAL;
			} else if (request->header.payload_length > NETWORKD_REQUEST_MAX ||
			    request->header.payload_length != input->used - header_length) {
				error = EMSGSIZE;
			} else {
				memcpy(request->payload, input->bytes + header_length,
				    request->header.payload_length);
				if (decode_request(request) != 0)
					error = errno != 0 ? errno : EINVAL;
			}
		}

		/* Detaches complete transport before any dispatcher can invoke another wait. */
		if (complete && error == 0) {
			client = input->descriptor;
			memcpy(peer, &input->peer, sizeof(*peer));
			*role = input->role;
			networkd_protocol_clear(input, sizeof(*input));
			return client;
		}

		/* Drops malformed or expired peers while continuing other accepted inputs. */
		if (error != 0) {
			send_error(input->descriptor, error, "incomplete or malformed request");
			(void)close(input->descriptor);
			networkd_protocol_clear(input, sizeof(*input));
			networkd_protocol_clear(request, sizeof(*request));
		}
	}

	/* Leaves incomplete peers owned by their original absolute ingress deadline. */
	return -1;
}

/* Services reads/notifications; defers stop intent without recursive mutation. */
static int
service_wifi_wait(
	void)
{
	struct networkd_request request;
	struct kern_peercred peer;
	enum networkd_client_role role;
	int client;
	int error;
	int deferred;
	uint64_t cleanup_deadline;

	if (wifi_work.cleanup)
		return 0;
	if (stopping || wifi_work.cancelled)
		return EINTR;
	if (control_listener < 0)
		return 0;
	memset(&request, 0, sizeof(request));
	client = receive_wait_request(&request, &peer, &role);
	if (client < 0)
		return 0;
	deferred = 0;
	if (wifi_work.deadline == 0U) {
		dispatch_request(client, &request, role, &peer);
	} else if (operation_name(request.header.opcode) == NULL ||
	    !operation_allowed(role, operation_name(request.header.opcode))) {
		send_response(client, request.header.request_id, request.header.opcode,
		    NETWORKD_RESULT_ERROR, EPERM, "operation denied", NULL, 0U);
	} else if (request.header.opcode == NETWORKD_OP_SHOW) {
		send_wired_observation(client, &request);
	} else if (!networkd_confirmed_active(&confirmed) &&
	    (request.header.opcode == NETWORKD_OP_CONFIRMED_CHECK ||
	    request.header.opcode == NETWORKD_OP_CONFIRMED_DISARM)) {
		/* The inactive transaction cannot roll back or interrupt RF work. */
		if (request.header.opcode == NETWORKD_OP_CONFIRMED_CHECK)
			error = networkd_confirmed_check(&confirmed, request.token);
		else
			error = networkd_confirmed_disarm(&confirmed, request.token);
		error = error == 0 ? 0 : (errno != 0 ? errno : EIO);
		send_response(client, request.header.request_id, request.header.opcode,
		    error == 0 ? NETWORKD_RESULT_OK : NETWORKD_RESULT_ERROR, error,
		    error == 0 ? NULL : "confirmed transaction", NULL, 0U);
	} else if (request.header.opcode == NETWORKD_OP_WIFI_LIST ||
	    (request.header.opcode == NETWORKD_OP_WIFI_ENABLE &&
	    networkd_managed_wlan_owner_matches(&managed_wlan, peer.euid) &&
	    (managed_wlan.state == NETWORKD_WLAN_AUTO_SEARCHING ||
	    managed_wlan.state == NETWORKD_WLAN_CONNECTING ||
	    managed_wlan.state == NETWORKD_WLAN_CONNECTED ||
	    managed_wlan.state == NETWORKD_WLAN_RECONNECTING))) {
		send_wifi_observation(client, &request);
	} else if (request.header.opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED) {
		wifi_profiles_changed(&peer);
		send_response(client, request.header.request_id, request.header.opcode,
		    NETWORKD_RESULT_OK, 0, NULL, NULL, 0U);
	} else {
		error = EBUSY;
		if (wifi_work.background &&
		    (request.header.opcode < NETWORKD_OP_WIFI_ENABLE ||
		    ((request.header.opcode == NETWORKD_OP_WIFI_DISCONNECT ||
		    request.header.opcode == NETWORKD_OP_WIFI_DISABLE) &&
		    owner_allowed(&peer)))) {
			wifi_pending_client = client;
			wifi_pending.role = role;
			memcpy(&wifi_pending.request, &request, sizeof(request));
			memcpy(&wifi_pending.peer, &peer, sizeof(peer));
			wifi_work.cancelled = 1;

			/* Queued control waits only for the reserved cleanup interval. */
			cleanup_deadline = netutil_monotonic_us() +
			    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
			if (wifi_work.deadline > cleanup_deadline)
				wifi_work.deadline = cleanup_deadline;
			deferred = 1;
		} else {
			if (request.header.opcode != NETWORKD_OP_WIFI_ENABLE &&
			    request.header.opcode >= NETWORKD_OP_WIFI_ENABLE &&
			    request.header.opcode <= NETWORKD_OP_WIFI_PROFILES_CHANGED &&
			    !owner_allowed(&peer))
				error = EPERM;
			send_response(client, request.header.request_id, request.header.opcode,
			    NETWORKD_RESULT_ERROR, error,
			    "Wi-Fi operation in progress; retry", NULL, 0U);
		}
	}
	networkd_protocol_clear(&request, sizeof(request));
	if (!deferred)
		(void)close(client);
	return deferred ? EINTR : 0;
}

/* Starts one actor transaction; every exit goes through its outer wrapper. */
static void
wifi_work_begin(
	uint32_t opcode,
	int background)
{
	memset(&wifi_work, 0, sizeof(wifi_work));
	wifi_work.deadline = netutil_monotonic_us() +
	    NETWORKD_WIFI_REQUEST_SECONDS(opcode) * 1000000ULL;
	wifi_work.background = background;
}

/* Drops the completed transaction without changing persistent policy intent. */
static void
wifi_work_end(
	void)
{
	memset(&wifi_work, 0, sizeof(wifi_work));

	/* Starts the next independent retirement at the initial retry interval. */
	if (managed_wlan.state != NETWORKD_WLAN_RETIRING)
		retirement_retry_seconds = NETWORKD_WLAN_RESCAN_SECONDS;
}

/* Gives every primitive and transaction stage the same absolute request end. */
static void
handle_wifi_request(
	int client,
	struct networkd_request *request,
	const struct kern_peercred *peer)
{
	wifi_work_begin(request->header.opcode, 0);
	process_wifi_request(client, request, peer);
	wifi_work_end();
}

/* Reads and decodes one complete request frame. */
static int
read_request(
	int descriptor,
	struct networkd_request *request)
{
	struct timeval timeout;

	/* Installs a finite transport deadline before consuming bytes. */
	if (request == NULL) {
		errno = EINVAL;
		return -1;
	}
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;
	if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
	    sizeof(timeout)) != 0 ||
	    setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
	    sizeof(timeout)) != 0)
		return -1;

	/* Reads and semantically validates one request payload. */
	if (networkd_protocol_read_frame_timed(descriptor, &request->header,
	    request->payload, sizeof(request->payload),
	    NETWORKD_REQUEST_MAX, 5U) != 0 || read_request_end(descriptor) != 0 ||
	    decode_request(request) != 0)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Requires one request frame followed by an orderly write-side close. */
static int
read_request_end(
	int descriptor)
{
	unsigned char byte;
	ssize_t count;

	/* Retries an interrupted EOF probe without accepting trailing data. */
	do {
		count = read(descriptor, &byte, sizeof(byte));
	} while (count < 0 && errno == EINTR);
	if (count == 0)
		return 0;
	if (count > 0)
		errno = EINVAL;

	/* Reports missing EOF or an additional frame as a protocol error. */
	return -1;
}

/* Decodes the exact fields admitted by one wired operation. */
static int
decode_request(
	struct networkd_request *request)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	unsigned seen;
	uint32_t timeout;
	int result;

	/* Reads every field while rejecting duplicates and unknown data. */
	seen = 0U;
	networkd_field_reader_init(&reader, request->payload,
	    request->header.payload_length);
	while ((result = networkd_field_read(&reader, &field)) == 0) {
		if (field.type == NETWORKD_FIELD_INTERFACE && (seen & 1U) == 0U &&
		    copy_request_text(request->interface,
		    sizeof(request->interface), &field) == 0) {
			seen |= 1U;
		} else if (field.type == NETWORKD_FIELD_TIMEOUT &&
		    (seen & 2U) == 0U &&
		    networkd_field_read_u32(&field, &timeout) == 0 &&
		    timeout >= 1U && timeout <= 3600U) {
			request->timeout = (unsigned)timeout;
			seen |= 2U;
		} else if (field.type == NETWORKD_FIELD_ADDRESS &&
		    (seen & 4U) == 0U &&
		    copy_request_text(request->address,
		    sizeof(request->address), &field) == 0) {
			seen |= 4U;
		} else if (field.type == NETWORKD_FIELD_NETMASK &&
		    (seen & 8U) == 0U &&
		    copy_request_text(request->netmask,
		    sizeof(request->netmask), &field) == 0) {
			seen |= 8U;
		} else if (field.type == NETWORKD_FIELD_GATEWAY &&
		    (seen & 16U) == 0U &&
		    copy_request_text(request->gateway,
		    sizeof(request->gateway), &field) == 0) {
			seen |= 16U;
		} else if (field.type == NETWORKD_FIELD_DNS &&
		    request->dns_count < sizeof(request->dns) /
		    sizeof(request->dns[0]) &&
		    copy_request_text(request->dns[request->dns_count],
		    sizeof(request->dns[request->dns_count]), &field) == 0) {
			request->dns_count++;
		} else if (field.type == NETWORKD_FIELD_PATH &&
		    (seen & 64U) == 0U && copy_request_text(
		    request->rollback_path, sizeof(request->rollback_path),
		    &field) == 0) {
			seen |= 64U;
		} else if (field.type == NETWORKD_FIELD_TOKEN &&
		    (seen & 128U) == 0U &&
		    networkd_field_read_u32(&field, &request->token) == 0 &&
		    request->token != 0U) {
			seen |= 128U;
		} else if (field.type == NETWORKD_FIELD_SSID &&
		    (seen & 32U) == 0U && field.length != 0U &&
		    field.length <= sizeof(request->ssid) &&
		    memchr(field.value, '\0', field.length) == NULL) {
			memcpy(request->ssid, field.value, field.length);
			request->ssid_length = field.length;
			seen |= 32U;
		} else {
			errno = EINVAL;
			return -1;
		}
	}
	if (result < 0)
		return -1;

	/* Requires the exact field set belonging to the opcode. */
	if ((request->header.opcode == NETWORKD_OP_SHOW &&
	    (seen == 0U || seen == 1U) && request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_UP ||
	    request->header.opcode == NETWORKD_OP_DOWN) &&
	    (seen == 1U || seen == (1U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_DHCP &&
	    (seen == 3U || seen == (3U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_STATIC &&
	    (seen == 13U || seen == (13U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_DEFAULT_ROUTE &&
	    (seen == 16U || seen == (16U | 128U)) &&
	    request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_DNS &&
	    (seen == 0U || seen == 128U) &&
	    request->dns_count != 0U) ||
	    (request->header.opcode == NETWORKD_OP_RELOAD && seen == 0U &&
	    request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_DEFAULT_ROUTE_CLEAR ||
	    request->header.opcode == NETWORKD_OP_DNS_CLEAR) &&
	    (seen == 0U || seen == 128U) && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_ROLLBACK &&
	    seen == 0U && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_CHECK &&
	    (seen == 0U || seen == 128U) && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_ARM &&
	    seen == (2U | 64U) && request->dns_count == 0U &&
	    request->timeout <= NETWORKD_CONFIRMED_MINUTES_MAX) ||
	    (request->header.opcode == NETWORKD_OP_CONFIRMED_DISARM &&
	    seen == 128U && request->dns_count == 0U) ||
	    ((request->header.opcode == NETWORKD_OP_WIFI_ENABLE ||
	    request->header.opcode == NETWORKD_OP_WIFI_DISABLE ||
	    request->header.opcode == NETWORKD_OP_WIFI_LIST ||
	    request->header.opcode == NETWORKD_OP_WIFI_DISCONNECT ||
	    request->header.opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED) &&
	    seen == 0U && request->dns_count == 0U) ||
	    (request->header.opcode == NETWORKD_OP_WIFI_CONNECT &&
	    seen == 32U && request->dns_count == 0U))
		return 0;
	errno = EINVAL;
	return -1;
}

/* Copies one nonempty field into a NUL-terminated command operand. */
static int
copy_request_text(
	char *output,
	size_t capacity,
	const struct networkd_field *field)
{
	/* Rejects empty, oversized, and embedded-NUL command operands. */
	if (output == NULL || field == NULL || field->length == 0U ||
	    field->length >= capacity ||
	    memchr(field->value, '\0', field->length) != NULL) {
		errno = EINVAL;
		return -1;
	}
	memcpy(output, field->value, field->length);
	output[field->length] = '\0';
	return 0;
}

/* Returns the authorization name for one stable opcode. */
static const char *
operation_name(
	uint32_t opcode)
{
	static const char *const names[] = {
		NULL, "SHOW", "UP", "DOWN", "DHCP", "STATIC",
		"DEFAULTROUTE", "DNS", "RELOAD"
	};

	/* Maps the contiguous wired range. */
	if (opcode < sizeof(names) / sizeof(names[0]))
		return names[opcode];
	if (opcode == NETWORKD_OP_DEFAULT_ROUTE_CLEAR)
		return "DEFAULTROUTE_CLEAR";
	if (opcode == NETWORKD_OP_DNS_CLEAR)
		return "DNS_CLEAR";
	if (opcode == NETWORKD_OP_CONFIRMED_ARM)
		return "CONFIRMED_ARM";
	if (opcode == NETWORKD_OP_CONFIRMED_DISARM)
		return "CONFIRMED_DISARM";
	if (opcode == NETWORKD_OP_CONFIRMED_ROLLBACK)
		return "CONFIRMED_ROLLBACK";
	if (opcode == NETWORKD_OP_CONFIRMED_CHECK)
		return "CONFIRMED_CHECK";

	/* Maps the separately allocated WLAN range. */
	if (opcode == NETWORKD_OP_WIFI_ENABLE)
		return "WIFI_ENABLE";
	if (opcode == NETWORKD_OP_WIFI_DISABLE)
		return "WIFI_DISABLE";
	if (opcode == NETWORKD_OP_WIFI_LIST)
		return "WIFI_LIST";
	if (opcode == NETWORKD_OP_WIFI_CONNECT)
		return "WIFI_CONNECT";
	if (opcode == NETWORKD_OP_WIFI_DISCONNECT)
		return "WIFI_DISCONNECT";
	if (opcode == NETWORKD_OP_WIFI_PROFILES_CHANGED)
		return "WIFI_PROFILES_CHANGED";
	return NULL;
}

/* Sends one bounded correlated terminal response. */
static void
send_response(
	int client,
	uint32_t request_id,
	uint32_t opcode,
	uint32_t status,
	int error,
	const char *stage,
	const void *output,
	size_t output_length)
{
	struct networkd_protocol_header header;
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	size_t required;
	size_t stage_length;

	/* Converts an oversized composition into a correlated terminal error. */
	stage_length = stage != NULL ? strnlen(stage, NETWORKD_DIAGNOSTIC_MAX) :
	    0U;
	required = 16U + (stage_length != 0U ? 4U + stage_length : 0U) +
	    (output_length != 0U ? 4U + output_length : 0U);
	if (required > sizeof(payload)) {
		status = NETWORKD_RESULT_ERROR;
		error = EOVERFLOW;
		stage = "response overflow";
		stage_length = sizeof("response overflow") - 1U;
		output = NULL;
		output_length = 0U;
	}

	/* Encodes the required status and error fields first. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS, status) !=
	    0 || networkd_field_write_u32(&writer, NETWORKD_FIELD_ERROR,
	    error > 0 ? (uint32_t)error : 0U) != 0)
		return;

	/* Appends only bounded sanitized diagnostic and output data. */
	if ((stage_length != 0U && networkd_field_write(&writer,
	    NETWORKD_FIELD_STAGE, stage, stage_length) != 0) ||
	    (output_length != 0U && networkd_field_write(&writer,
	    NETWORKD_FIELD_OUTPUT, output, output_length) != 0)) {
		networkd_protocol_clear(payload, sizeof(payload));
		return;
	}
	header.request_id = request_id != 0U ? request_id : 1U;
	header.opcode = opcode != 0U ? opcode : NETWORKD_OP_SHOW;
	header.payload_length = writer.used;
	(void)networkd_protocol_write_frame(client, &header, payload);
	networkd_protocol_clear(payload, sizeof(payload));
}

/* Sends the sole successful arm response with its opaque token. */
static void
send_token_response(
	int client,
	uint32_t request_id,
	uint32_t opcode,
	uint32_t token)
{
	struct networkd_protocol_header header;
	struct networkd_field_writer writer;
	unsigned char payload[32];

	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS,
	    NETWORKD_RESULT_OK) != 0 || networkd_field_write_u32(&writer,
	    NETWORKD_FIELD_ERROR, 0U) != 0 || networkd_field_write_u32(&writer,
	    NETWORKD_FIELD_TOKEN, token) != 0)
		return;
	header.request_id = request_id;
	header.opcode = opcode;
	header.payload_length = writer.used;
	(void)networkd_protocol_write_frame(client, &header, payload);
	networkd_protocol_clear(payload, sizeof(payload));
}

/* Sends an uncorrelated protocol or authentication error. */
static void
send_error(
	int client,
	int error,
	const char *reason)
{
	/* Uses the reserved correlation pair when no valid header exists. */
	send_response(client, 1U, NETWORKD_OP_SHOW, NETWORKD_RESULT_ERROR,
	    error != 0 ? error : EIO, reason, NULL, 0U);
}

/* Supports the show interfaces operation. */
static int
show_interfaces(
	const char *name,
	char *output,
	size_t capacity)
{
	struct ifreq *items;
	unsigned count, index;
	size_t used;
	int descriptor, result;

	items = NULL;
	count = 0;
	used = 0;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	result = 0;

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles the name availability. */
	if (name != NULL) {
		result = append_interface_status(descriptor, name, output,
						 capacity, &used);
	} else if (netutil_interfaces(descriptor, &items, &count) != 0)
		result = -1;
	else

		/* Process each remaining element. */
		for (index = 0; index < count; index++) {
			/* Handles a failed append interface status operation. */
			if (append_interface_status(
				descriptor, items[index].ifr_name, output,
				capacity, &used) != 0) {
				result = -1;
				break;
			}
		}
	free(items);
	close(descriptor);

	/* Returns the computed result. */
	return result;
}

/* Supports the append interface status operation. */
static int
append_interface_status(
	int descriptor,
	const char *name,
	char *output,
	const size_t capacity,
	size_t *used)
{
	struct ifreq flags, address;
	int count, has_address;

	/* Handles a failed netutil ifreq operation. */
	if (netutil_ifreq(&flags, name) != 0 ||
	    ioctl(descriptor, SIOCGIFFLAGS, &flags) != 0)

		/* Reports operation failure. */
		return -1;
	has_address =
	    netutil_ifreq(&address, name) == 0 &&
	    ioctl(descriptor, SIOCGIFADDR, &address) == 0 &&
	    ((struct sockaddr_in *)&address.ifr_addr)->sin_addr.s_addr != 0;
	count =
	    snprintf(output + *used, capacity - *used, "%s %s %s\n", name,
		     has_address ? "static" : "unconfigured",
		     (flags.ifr_flags & IFF_RUNNING) != 0 ? "online" : "offline");

	/* Checks the remaining item count. */
	if (count < 0 || (size_t)count >= capacity - *used) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	*used += (size_t)count;
	/* Reports successful completion. */
	return 0;
}

/* Supports the interface exists operation. */
static int
interface_exists(
	const char *name)
{
	uint32_t index;
	int descriptor;
	int result;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	result = descriptor >= 0
		? netutil_ifindex(descriptor, name, &index)
		: -1;

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		close(descriptor);

	/* Returns the computed result. */
	return result;
}

/* Resolves one interface name to its stable current index. */
static int
interface_index(
	const char *name,
	uint32_t *index)
{
	int descriptor;
	int result;
	int saved;

	/* Uses the canonical generic network-interface ioctl. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	result = netutil_ifindex(descriptor, name, index);
	saved = errno;
	(void)close(descriptor);
	errno = saved;
	return result;
}

/* Reads the current canonical interface flags. */
static int
interface_flags(
	const char *name,
	int *flags)
{
	struct ifreq request;
	int descriptor;
	int result;
	int saved;

	/* Issues one bounded name-based flags query. */
	if (flags == NULL || netutil_ifreq(&request, name) != 0)
		return -1;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	result = ioctl(descriptor, SIOCGIFFLAGS, &request);
	saved = errno;
	(void)close(descriptor);
	if (result == 0)
		*flags = request.ifr_flags;
	errno = saved;
	return result;
}

/* Verifies that an index still resolves to the managed interface name. */
static int
interface_index_name_matches(
	uint32_t index,
	const char *name)
{
	struct ifreq request;
	int descriptor;
	int result;
	int saved;

	/* Resolves the index independently of the possibly reused name. */
	memset(&request, 0, sizeof(request));
	request.ifr_ifindex = (int)index;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	result = ioctl(descriptor, SIOCGIFNAME, &request);
	saved = errno;
	(void)close(descriptor);
	if (result == 0 && strcmp(request.ifr_name, name) != 0) {
		errno = ENODEV;
		return -1;
	}
	errno = saved;
	return result;
}

/* Confirms that the WLAN controlled port is currently usable. */
static int
wlan_connected(
	const char *interface)
{
	struct wlan_status_request status;
	size_t length;
	int descriptor;
	int result;

	/* Queries one current public WLAN status snapshot. */
	length = strlen(interface);
	if (length == 0U || length >= IFNAMSIZ)
		return 0;
	memset(&status, 0, sizeof(status));
	memcpy(status.ifr_name, interface, length);
	status.version = WLAN_ABI_VERSION;
	status.size = sizeof(status);
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return 0;
	result = ioctl(descriptor, SIOCGWLANSTATUS, &status) == 0 &&
	    status.state == WLAN_STATE_CONNECTED &&
	    status.controlled_port != 0U;
	(void)close(descriptor);
	networkd_protocol_clear(&status, sizeof(status));
	return result;
}

/* A reusable connection needs its live L2 port and a completed usable L3 state. */
static int
managed_connection_usable(
	void)
{
	struct networkd_managed_l3 current;
	int flags;
	int usable;

	if (managed_wlan.connection.interface[0] == '\0' ||
	    !managed_wlan.connection.owns_l3 || managed_wlan.connection.l3_pending)
		return 0;
	memset(&current, 0, sizeof(current));
	usable = interface_index_name_matches(managed_wlan.connection.ifindex,
	    managed_wlan.connection.interface) == 0 &&
	    interface_flags(managed_wlan.connection.interface, &flags) == 0 &&
	    (flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING) &&
	    wlan_connected(managed_wlan.connection.interface) &&
	    snapshot_managed_l3(&managed_wlan, &current) == 0 && current.address != 0U;
	networkd_protocol_clear(&current, sizeof(current));
	return usable;
}

/* Runs one private machine-mode wifi primitive. */
static int
run_wifi(
	const char *interface,
	const char *operation,
	const struct wifi_conf_profile *profile,
	unsigned timeout,
	struct networkd_wifi_child_result *result)
{
	const void *ssid;
	const void *passphrase;
	size_t ssid_length;
	size_t passphrase_length;
	uint64_t remaining;
	uint64_t limit;
	uint64_t now;
	int function_result;

	if (!wifi_work.cleanup && wifi_wait_pump != NULL &&
	    (function_result = wifi_wait_pump()) != 0) {
		networkd_wifi_child_result_clear(result);
		result->terminal_error = function_result;
		errno = function_result;
		return -1;
	}

	/* Keep the last teardown interval available after selection expires. */
	if (wifi_work.deadline != 0U) {
		limit = wifi_work.deadline;
		if (strcmp(operation, "disconnect") != 0 &&
		    strcmp(operation, "search-stop") != 0 &&
		    strcmp(operation, "down") != 0)
			limit -= NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
		now = netutil_monotonic_us();
		remaining = limit > now ? (limit - now) / 1000000ULL : 0U;
		/* Include the runner's one-second termination grace in this bound. */
		if (remaining <= 1U) {
			networkd_wifi_child_result_clear(result);
			result->terminal_error = ETIMEDOUT;
			errno = ETIMEDOUT;
			return -1;
		}
		if (timeout >= remaining)
			timeout = (unsigned)remaining - 1U;
	}

	/* Supplies counted credential views only to connect. */
	ssid = profile != NULL ? profile->ssid : NULL;
	ssid_length = profile != NULL ? profile->ssid_length : 0U;
	passphrase = profile != NULL ? profile->passphrase : NULL;
	passphrase_length = profile != NULL ? profile->passphrase_length : 0U;
	function_result = networkd_wifi_child_run(interface, operation, ssid,
	    ssid_length, passphrase, passphrase_length, timeout, result);
	if (function_result == 0 && strcmp(operation, "list") == 0)
		remember_wifi_observation(interface, result);
	if (function_result != 0)
		errno = result->terminal_error != 0 ?
		    result->terminal_error : EIO;
	return function_result;
}

/* Converts complete private records directly into public source-prefixed lines. */
static int
append_wifi_records(
	const char *interface,
	const char *records,
	size_t length,
	char *output,
	size_t capacity,
	size_t *used)
{
	static const char private_prefix[] = "WIFI1 ";
	static const char terminal[] = "terminal ";
	char prefix[IFNAMSIZ + 16U];
	size_t start;
	size_t end;
	size_t prefix_length;
	int count;

	/* Rejects invalid spans before computing remaining capacity. */
	if (records == NULL || output == NULL || used == NULL || *used > capacity) {
		errno = EINVAL;
		return -1;
	}
	count = snprintf(prefix, sizeof(prefix), "interface=%s ", interface);
	if (count < 0 || (size_t)count >= sizeof(prefix)) {
		errno = EOVERFLOW;
		return -1;
	}
	prefix_length = (size_t)count;

	/* Copies one complete record at a time without a second 32 KB stack buffer. */
	start = 0U;
	while (start < length) {
		end = start;
		while (end < length && records[end] != '\n')
			end++;

		/* Requires the producer's version marker and a complete line. */
		if (end == length || end - start < sizeof(private_prefix) - 1U ||
		    memcmp(records + start, private_prefix, sizeof(private_prefix) - 1U) != 0) {
			errno = EILSEQ;
			return -1;
		}
		start += sizeof(private_prefix) - 1U;

		/* Omits only the private terminal record from public output. */
		if (end - start >= sizeof(terminal) - 1U &&
		    memcmp(records + start, terminal, sizeof(terminal) - 1U) == 0)
			return 0;

		/* Leaves the output cursor at the last complete line on overflow. */
		if (prefix_length + end - start + 1U > capacity - *used) {
			errno = EOVERFLOW;
			return -1;
		}
		memcpy(output + *used, prefix, prefix_length);
		*used += prefix_length;
		memcpy(output + *used, records + start, end - start);
		*used += end - start;
		output[(*used)++] = '\n';
		start = end + 1U;
	}

	/* Reports complete conversion of the available records. */
	return 0;
}

/* Appends one primitive result while identifying its source radio. */
static int
append_wifi_output(
	const char *interface,
	const struct networkd_wifi_child_result *result,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	int status;

	/* Shares conversion between fresh primitive results and cached records. */
	if (result == NULL || result->output_length > sizeof(result->output)) {
		errno = EINVAL;
		return -1;
	}
	status = append_wifi_records(interface, (const char *)result->output, result->output_length,
	    output, capacity, output_length);
	return status;
}

/* Enumerates WLAN interfaces in the kernel's stable interface order. */
static int
enumerate_wlan_radios(
	struct networkd_wlan_radio *radios,
	size_t capacity,
	size_t *radio_count)
{
	struct wlan_status_request status;
	struct ifreq *interfaces;
	unsigned interface_count;
	unsigned index;
	size_t count;
	size_t known;
	uint32_t ifindex;
	int status_error;
	int descriptor;
	int saved;

	/* Obtains the canonical interface list through the generic socket API. */
	if (radios == NULL || radio_count == NULL || capacity == 0U ||
	    capacity > NETWORKD_WLAN_RADIO_MAX) {
		errno = EINVAL;
		return -1;
	}
	interfaces = NULL;
	interface_count = 0U;
	count = 0U;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	if (netutil_interfaces(descriptor, &interfaces, &interface_count) != 0) {
		saved = errno;
		(void)close(descriptor);
		errno = saved;
		return -1;
	}

	/* A successful WLAN status ioctl classifies one interface as a radio. */
	for (index = 0U; index < interface_count; index++) {
		if (netutil_ifindex(descriptor, interfaces[index].ifr_name, &ifindex) != 0) {
			if (errno == ENODEV || errno == ENXIO)
				continue;
			saved = errno;
			free(interfaces);
			(void)close(descriptor);
			errno = saved;
			return -1;
		}
		memset(&status, 0, sizeof(status));
		memcpy(status.ifr_name, interfaces[index].ifr_name,
		    sizeof(status.ifr_name));
		status.version = WLAN_ABI_VERSION;
		status.size = sizeof(status);
		status_error = ioctl(descriptor, SIOCGWLANSTATUS, &status) == 0 ? 0 : errno;
		for (known = 0U; known < known_wlan_radio_count; known++) {
			if (known_wlan_radios[known].ifindex == ifindex &&
			    strcmp(known_wlan_radios[known].interface, status.ifr_name) == 0)
				break;
		}
		if (status_error != 0 && known == known_wlan_radio_count) {
			if (status_error == EOPNOTSUPP || status_error == ENOTTY)
				continue;
			/* Unknown observation failure cannot prove this is not a radio. */
			free(interfaces);
			(void)close(descriptor);
			errno = status_error;
			return -1;
		}
		if (count == capacity) {
			free(interfaces);
			(void)close(descriptor);
			errno = E2BIG;
			return -1;
		}
		memcpy(radios[count].interface, interfaces[index].ifr_name,
		    sizeof(radios[count].interface));
		radios[count].ifindex = ifindex;
		radios[count].observation_error = status_error;
		radios[count].stop_flags = status.stop_flags;
		radios[count].administrative_up = status.administrative_up != 0U;
		radios[count].association_active =
		    status.state >= WLAN_STATE_AUTHENTICATING &&
		    status.state <= WLAN_STATE_DISCONNECTING;
		radios[count].scan_state = status.scan_state;
		radios[count].snapshot_generation = status.snapshot_generation;
		radios[count].consumed_snapshot_generation = 0U;
		if (known < known_wlan_radio_count)
			radios[count].consumed_snapshot_generation =
			    known_wlan_radios[known].consumed_snapshot_generation;
		count++;
	}
	free(interfaces);
	saved = errno;
	if (close(descriptor) != 0)
		return -1;
	errno = saved;
	*radio_count = count;
	memcpy(known_wlan_radios, radios, count * sizeof(*radios));
	known_wlan_radio_count = count;

	/* Reports a possibly empty, stable-order radio list. */
	return 0;
}


/* Runs one nonsecret preparation primitive with uniform output and cleanup. */
static int
run_wifi_append(
	const char *interface,
	const char *operation,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	struct networkd_wifi_child_result child;
	int result;
	int saved;

	memset(&child, 0, sizeof(child));
	result = run_wifi(interface, operation, NULL, 10U, &child);
	if (result == 0 && output != NULL && output_length != NULL)
		result = append_wifi_output(interface, &child, output, capacity,
		    output_length);
	saved = errno;
	networkd_wifi_child_result_clear(&child);
	errno = saved;
	return result;
}

/* Reuses administrative and scan state already observed during enumeration. */
static int
prepare_wlan_radio(
	struct networkd_wlan_radio *radio,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	int reuse_snapshot;

	radio->ready = 0;
	if (radio->observation_error != 0 || radio->stop_flags != 0U) {
		errno = radio->observation_error != 0 ? radio->observation_error : EBUSY;
		return -1;
	}
	/* Existing direct associations cannot become a second managed L2 link. */
	if (radio->association_active) {
		if (run_wifi_append(radio->interface, "disconnect", output,
		    capacity, output_length) != 0)
			return -1;
		radio->association_active = 0;
		radio->scan_state = WLAN_SCAN_IDLE;
	}
	if (!radio->administrative_up) {
		if (run_wifi_append(radio->interface, "up", output,
		    capacity, output_length) != 0)
			return -1;
		radio->administrative_up = 1;
		radio->scan_state = WLAN_SCAN_IDLE;
	}
	/* A scan can finish after the previous selection deadline, during idle. */
	reuse_snapshot = radio->scan_state == WLAN_SCAN_COMPLETE &&
	    radio->snapshot_generation != 0U &&
	    radio->snapshot_generation != radio->consumed_snapshot_generation;
	if (radio->scan_state != WLAN_SCAN_RUNNING && !reuse_snapshot) {
		if (run_wifi_append(radio->interface, "search-start", output,
		    capacity, output_length) != 0)
			return -1;
		radio->scan_state = WLAN_SCAN_RUNNING;
	}
	radio->ready = 1;
	return 0;
}

/* Remembers consumption only for the exact interface observed by selection. */
static void
consume_wlan_snapshot(
	const struct networkd_wlan_radio *radio,
	uint64_t generation)
{
	size_t index;

	if (generation == 0U)
		return;
	for (index = 0U; index < known_wlan_radio_count; index++) {
		if (known_wlan_radios[index].ifindex != radio->ifindex)
			continue;
		if (strcmp(known_wlan_radios[index].interface, radio->interface) != 0)
			continue;
		known_wlan_radios[index].consumed_snapshot_generation = generation;
		return;
	}
}

/* Isolates each radio failure while requiring at least one usable radio. */
static int
prepare_wlan_radios(
	struct networkd_wlan_radio *radios,
	size_t radio_count,
	char *output,
	size_t capacity,
	size_t *output_length)
{
	size_t index;
	size_t prepared;
	int first_error;

	prepared = 0U;
	first_error = 0;
	for (index = 0U; index < radio_count; index++) {
		if (prepare_wlan_radio(&radios[index], output, capacity,
		    output_length) == 0)
			prepared++;
		else if (first_error == 0)
			first_error = errno != 0 ? errno : EIO;
	}
	if (radio_count != 0U && prepared == 0U) {
		errno = first_error != 0 ? first_error : EIO;
		return -1;
	}
	if (first_error != 0)
		fprintf(stderr, "networkd: some WLAN radios could not prepare: %s\n",
		    strerror(first_error));
	return 0;
}

/* Reads an exact retained radio identity; a transient status error is not absence. */
static int
wlan_radio_status(const struct networkd_wlan_radio *radio,
	struct wlan_status_request *status)
{
	int descriptor;
	int error;
	int saved;

	if (interface_index_name_matches(radio->ifindex, radio->interface) != 0)
		return -1;
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	memset(status, 0, sizeof(*status));
	memcpy(status->ifr_name, radio->interface, sizeof(status->ifr_name));
	status->version = WLAN_ABI_VERSION;
	status->size = sizeof(*status);
	error = ioctl(descriptor, SIOCGWLANSTATUS, status);
	saved = errno;
	(void)close(descriptor);
	if (error == 0)
		return interface_index_name_matches(radio->ifindex, radio->interface);
	errno = saved;
	return -1;
}

/* Stops all radios, retaining failures until checked physical stop is observed. */
static int
stop_wlan_radios(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	int lower)
{
	struct networkd_wifi_child_result result;
	struct wlan_status_request status;
	size_t index;
	int first_error;
	int error;

	memset(&result, 0, sizeof(result));
	first_error = 0;
	for (index = 0U; index < radio_count; index++) {
		/* An old identity disappearing permits retirement, never mutation of
		 * the replacement. Only the identity query can establish absence. */
		if (interface_index_name_matches(radios[index].ifindex,
		    radios[index].interface) != 0) {
			if (errno != ENODEV && errno != ENXIO && first_error == 0)
				first_error = errno;
			continue;
		}
		error = 0;
		if (wlan_radio_status(&radios[index], &status) != 0)
			error = errno;
		else if ((status.stop_flags & WLAN_STATUS_STOP_PENDING) != 0U)
			error = EBUSY;
		if (error == 0) {
			if (run_wifi(radios[index].interface, "disconnect", NULL, 10U,
			    &result) != 0)
				error = errno;
			networkd_wifi_child_result_clear(&result);
			if (run_wifi(radios[index].interface, "search-stop", NULL, 10U,
			    &result) != 0 && error == 0)
				error = errno;
			networkd_wifi_child_result_clear(&result);
			if (lower) {
				/* A checked full stop supersedes earlier inverse errors. */
				if (run_wifi(radios[index].interface, "down", NULL, 10U,
				    &result) != 0)
					error = errno;
				networkd_wifi_child_result_clear(&result);
				if (wlan_radio_status(&radios[index], &status) != 0)
					error = errno;
				else if (status.stop_flags != 0U || status.administrative_up ||
				    status.associated || status.key_installed ||
				    status.controlled_port || status.scan_state == WLAN_SCAN_RUNNING)
					error = EBUSY;
				else
					error = 0;
			}
		}
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	if (first_error != 0) {
		errno = first_error;
		return -1;
	}
	return 0;
}

/* Stops scans on every prepared radio except the sole connected winner. */
static void
stop_losing_scans(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	size_t winner)
{
	struct networkd_wifi_child_result result;
	size_t index;
	int saved;

	/* A committed connection remains usable if a losing scan cannot stop. */
	memset(&result, 0, sizeof(result));
	for (index = 0U; index < radio_count; index++) {
		if (index == winner || !radios[index].ready)
			continue;
		if (run_wifi(radios[index].interface, "search-stop", NULL, 10U,
		    &result) != 0) {
			saved = errno != 0 ? errno : EIO;
			fprintf(stderr, "networkd: %s: stop losing scan: %s\n",
			    radios[index].interface, strerror(saved));
		}
		networkd_wifi_child_result_clear(&result);
	}
}

/* Finds an exact saved profile without exposing its passphrase elsewhere. */
static const struct wifi_conf_profile *
find_profile(
	const struct wifi_conf_model *model,
	const void *ssid,
	size_t ssid_length)
{
	size_t index;

	/* Uses the file's stable record order for exact counted SSID matching. */
	if (model == NULL || ssid == NULL || ssid_length == 0U)
		return NULL;
	for (index = 0U; index < model->profile_count; index++) {
		if (model->profiles[index].ssid_length == ssid_length &&
		    memcmp(model->profiles[index].ssid, ssid, ssid_length) == 0)
			return &model->profiles[index];
	}

	/* Reports an absent profile without manufacturing a credential. */
	return NULL;
}

/* Loads one authenticated policy owner's fixed credential store. */
static int
load_policy(
	uid_t owner_uid,
	struct wifi_conf_model *model,
	char *diagnostic,
	size_t diagnostic_capacity)
{
	/* The store layer resolves passwd homes and enforces ownership itself. */
	return wifi_store_load_for_user(owner_uid, model, diagnostic,
	    diagnostic_capacity);
}

/* Tests whether one peer may control the currently enabled policy. */
static int
owner_allowed(
	const struct kern_peercred *peer)
{
	/* Root may override; other admitted peers must own the active policy. */
	if (peer == NULL)
		return 0;
	if (peer->euid == 0)
		return 1;
	if (managed_wlan.state == NETWORKD_WLAN_DISABLED)
		return 1;
	return networkd_managed_wlan_owner_matches(&managed_wlan, peer->euid);
}

/* Returns the public spelling for one managed policy state. */
static const char *
managed_state_name(
	enum networkd_managed_wlan_state state)
{
	/* Maps every closed state-machine value explicitly. */
	if (state == NETWORKD_WLAN_DISABLED)
		return "disabled";
	if (state == NETWORKD_WLAN_AUTO_SEARCHING)
		return "auto-searching";
	if (state == NETWORKD_WLAN_CONNECTING)
		return "connecting";
	if (state == NETWORKD_WLAN_CONNECTED)
		return "connected";
	if (state == NETWORKD_WLAN_MANUAL_DISCONNECTED)
		return "manual-disconnected";
	if (state == NETWORKD_WLAN_RECONNECTING)
		return "reconnecting";
	if (state == NETWORKD_WLAN_RETIRING)
		return "disconnecting";

	/* Keeps a corrupted internal value visibly distinct. */
	return "invalid";
}

/* Appends one nonsecret global managed-policy status record. */
static int
append_managed_status(
	char *output,
	size_t capacity,
	size_t *output_length)
{
	const char *interface;
	int count;

	/* Describes public policy state and the sole selected radio, if any. */
	interface = managed_wlan.connection.interface[0] != '\0' ?
	    managed_wlan.connection.interface : "-";
	count = snprintf(output + *output_length,
	    capacity - *output_length, "wifi state=%s interface=%s\n",
	    managed_state_name(managed_wlan.state), interface);
	if (count < 0 || (size_t)count >= capacity - *output_length) {
		errno = EOVERFLOW;
		return -1;
	}
	*output_length += (size_t)count;

	/* Reports successful bounded status composition. */
	return 0;
}

/* Includes child termination grace in the remaining selection interval. */
static unsigned
wifi_selection_timeout(
	uint64_t deadline)
{
	uint64_t now;
	uint64_t seconds;

	now = netutil_monotonic_us();
	if (now >= deadline)
		return 0U;
	seconds = (deadline - now) / 1000000ULL;
	if (seconds <= 1U)
		return 0U;
	seconds--;
	return seconds > 5U ? 5U : (unsigned)seconds;
}

/* Freezes one bounded candidate wave in profile order then radio order. */
static int
collect_profile_radios(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	const struct wifi_conf_model *model,
	size_t skip,
	struct networkd_wifi_candidate *candidates,
	size_t *candidate_count,
	size_t *total_count,
	uint64_t deadline)
{
	struct networkd_wifi_child_result result;
	struct networkd_wifi_list_result parsed;
	struct timespec delay;
	unsigned char terminal[NETWORKD_WLAN_RADIO_MAX];
	unsigned char visible[WIFI_CONF_PROFILE_MAX]
	    [NETWORKD_WLAN_RADIO_MAX];
	size_t profile_index;
	size_t radio_index;
	size_t candidate_index;
	size_t available;
	unsigned valid_scans;
	unsigned timeout;
	int first_error;
	int automatic_present;
	int all_terminal;
	int parse_error;

	/* Initializes a complete bounded visibility matrix. */
	if (radios == NULL || model == NULL || candidates == NULL ||
	    candidate_count == NULL || total_count == NULL || radio_count == 0U ||
	    radio_count > NETWORKD_WLAN_RADIO_MAX ||
	    model->profile_count > WIFI_CONF_PROFILE_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&result, 0, sizeof(result));
	memset(&parsed, 0, sizeof(parsed));
	memset(terminal, 0, sizeof(terminal));
	memset(visible, 0, sizeof(visible));
	*candidate_count = 0U;
	*total_count = 0U;
	valid_scans = 0U;
	first_error = 0;
	all_terminal = 0;
	automatic_present = 0;
	for (profile_index = 0U; profile_index < model->profile_count;
	    profile_index++) {
		if (model->profiles[profile_index].automatic)
			automatic_present = 1;
	}
	if (!automatic_present) {
		/* No profile can consume this wave; still allow future discovery. */
		for (radio_index = 0U; radio_index < radio_count; radio_index++) {
			if (radios[radio_index].scan_state == WLAN_SCAN_COMPLETE)
				consume_wlan_snapshot(&radios[radio_index],
				    radios[radio_index].snapshot_generation);
		}
		errno = ENOENT;
		return -1;
	}
	delay.tv_sec = 0;
	delay.tv_nsec = 100000000L;
	for (radio_index = 0U; radio_index < radio_count; radio_index++) {
		if (!radios[radio_index].ready)
			terminal[radio_index] = 1U;
	}

	/* Observes each radio without making a ready candidate await another scan. */
	while (netutil_monotonic_us() < deadline) {
		if (wifi_wait_pump != NULL && wifi_wait_pump() != 0) {
			errno = EINTR;
			return -1;
		}
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (terminal[radio_index])
				continue;
			timeout = wifi_selection_timeout(deadline);
			if (timeout == 0U)
				break;
			if (run_wifi(radios[radio_index].interface, "list", NULL,
			    timeout, &result) != 0) {
				if (first_error == 0)
					first_error = errno != 0 ? errno : EIO;
				terminal[radio_index] = 1U;
				networkd_wifi_child_result_clear(&result);
				if (wifi_work.cancelled) {
					errno = EINTR;
					return -1;
				}
				continue;
			}
			parse_error = 0;
			for (profile_index = 0U;
			    profile_index < model->profile_count; profile_index++) {
				if (!model->profiles[profile_index].automatic)
					continue;
				if (networkd_wifi_child_parse_list(&result,
				    model->profiles[profile_index].ssid,
				    model->profiles[profile_index].ssid_length,
				    &parsed) != 0) {
					parse_error = errno;
					break;
				}
				if (parsed.scan_complete)
					consume_wlan_snapshot(&radios[radio_index],
					    parsed.snapshot_generation);
				if (parsed.scan_complete && parsed.ssid_supported)
					visible[profile_index][radio_index] = 1U;
				terminal[radio_index] = parsed.scan_terminal != 0;
			}
			networkd_wifi_child_result_clear(&result);
			if (parse_error == 0 && parsed.scan_complete)
				valid_scans++;
			if (parse_error != 0) {
				/* Retry a malformed completed observation with a fresh scan. */
				consume_wlan_snapshot(&radios[radio_index],
				    radios[radio_index].snapshot_generation);
				if (first_error == 0)
					first_error = parse_error;
				/* A malformed radio must not hide another usable radio. */
				terminal[radio_index] = 1U;
				for (profile_index = 0U;
				    profile_index < model->profile_count; profile_index++)
					visible[profile_index][radio_index] = 0U;
			}
		}

		/* Freeze usable observations now; profile/radio order breaks current ties. */
		available = 0U;
		for (profile_index = 0U; profile_index < model->profile_count;
		    profile_index++) {
			for (radio_index = 0U; radio_index < radio_count; radio_index++) {
				if (visible[profile_index][radio_index])
					available++;
			}
		}

		/* A skipped prefix still waits for later candidates or a terminal wave. */
		if (available > skip)
			break;

		all_terminal = 1;
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (!terminal[radio_index])
				all_terminal = 0;
		}
		if (all_terminal)
			break;
		(void)nanosleep(&delay, NULL);
	}

	/* A slow radio cannot permanently conceal another completed snapshot. */
	candidate_index = 0U;
	for (profile_index = 0U; profile_index < model->profile_count;
	    profile_index++) {
		if (!model->profiles[profile_index].automatic)
			continue;
		for (radio_index = 0U; radio_index < radio_count; radio_index++) {
			if (!visible[profile_index][radio_index])
				continue;
			if ((*total_count)++ >= skip &&
			    candidate_index < NETWORKD_WLAN_ATTEMPTS) {
				candidates[candidate_index].profile = &model->profiles[profile_index];
				candidates[candidate_index].radio = radio_index;
				candidate_index++;
			}
		}
	}
	*candidate_count = candidate_index;
	if (candidate_index != 0U)
		return 0;
	errno = valid_scans != 0U ? ENOENT :
	    (first_error != 0 ? first_error : (all_terminal ? EIO : ETIMEDOUT));
	return -1;
}

/* Selects the first stable-order radio which sees one manual target. */
static int
select_manual_radio(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	const struct wifi_conf_profile *profile,
	size_t *selected_radio,
	uint64_t deadline)
{
	struct networkd_wifi_child_result result;
	struct networkd_wifi_list_result parsed;
	struct timespec delay;
	unsigned char terminal[NETWORKD_WLAN_RADIO_MAX];
	unsigned char visible[NETWORKD_WLAN_RADIO_MAX];
	size_t radio_index;
	unsigned timeout;
	int earlier_terminal;

	/* Initializes one bounded visibility result for each stable-order radio. */
	if (radios == NULL || profile == NULL || selected_radio == NULL ||
	    radio_count == 0U || radio_count > NETWORKD_WLAN_RADIO_MAX) {
		errno = EINVAL;
		return -1;
	}
	memset(&result, 0, sizeof(result));
	memset(&parsed, 0, sizeof(parsed));
	memset(terminal, 0, sizeof(terminal));
	memset(visible, 0, sizeof(visible));
	*selected_radio = 0U;
	delay.tv_sec = 0;
	delay.tv_nsec = 100000000L;
	for (radio_index = 0U; radio_index < radio_count; radio_index++) {
		if (!radios[radio_index].ready)
			terminal[radio_index] = 1U;
	}

	/* Polls all asynchronous scans without preferring the fastest radio. */
	while (netutil_monotonic_us() < deadline) {
		if (wifi_wait_pump != NULL && wifi_wait_pump() != 0) {
			errno = EINTR;
			return -1;
		}
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (terminal[radio_index])
				continue;
			timeout = wifi_selection_timeout(deadline);
			if (timeout == 0U)
				break;
			if (run_wifi(radios[radio_index].interface, "list", NULL,
			    timeout, &result) != 0) {
				terminal[radio_index] = 1U;
				networkd_wifi_child_result_clear(&result);
				if (wifi_work.cancelled) {
					errno = EINTR;
					return -1;
				}
				continue;
			}
			if (networkd_wifi_child_parse_list(&result, profile->ssid,
			    profile->ssid_length, &parsed) != 0) {
				networkd_wifi_child_result_clear(&result);
				terminal[radio_index] = 1U;
				continue;
			}
			terminal[radio_index] = parsed.scan_terminal != 0;
			visible[radio_index] = parsed.scan_complete &&
			    parsed.ssid_supported;
			networkd_wifi_child_result_clear(&result);
		}

		/* Select among completed observations, without waiting for unseen radios. */
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (visible[radio_index]) {
				*selected_radio = radio_index;
				return 0;
			}
		}
		earlier_terminal = 1;
		for (radio_index = 0U; radio_index < radio_count;
		    radio_index++) {
			if (!terminal[radio_index])
				earlier_terminal = 0;
		}
		if (earlier_terminal) {
			errno = ENOENT;
			return -1;
		}
		(void)nanosleep(&delay, NULL);
	}

	/* At the fixed limit, choose among the radios that actually completed. */
	for (radio_index = 0U; radio_index < radio_count; radio_index++) {
		if (visible[radio_index]) {
			*selected_radio = radio_index;
			return 0;
		}
	}
	errno = ETIMEDOUT;
	return -1;
}

/* Tries a frozen candidate wave without renewing selection or work deadlines. */
static int
connect_automatic(
	const struct networkd_wlan_radio *radios,
	size_t radio_count,
	const struct wifi_conf_model *model,
	uint64_t deadline,
	char *output,
	size_t output_capacity,
	size_t *output_length,
	int *no_candidate)
{
	struct networkd_wifi_candidate candidates[NETWORKD_WLAN_ATTEMPTS];
	size_t count;
	size_t total;
	size_t attempt;
	size_t attempted;
	size_t radio;
	uint64_t selection_deadline;
	int l2_succeeded;
	int saved;

	if (no_candidate == NULL) {
		errno = EINVAL;
		return -1;
	}
	*no_candidate = 0;
	memset(candidates, 0, sizeof(candidates));
	selection_deadline = netutil_monotonic_us() +
	    NETWORKD_WLAN_SCAN_SECONDS * 1000000ULL;
	if (selection_deadline > deadline)
		selection_deadline = deadline;
	if (collect_profile_radios(radios, radio_count, model, automatic_candidate_skip,
	    candidates, &count, &total, selection_deadline) != 0) {
		automatic_candidate_skip = 0U;
		if (errno == ENOENT || errno == ETIMEDOUT)
			*no_candidate = 1;
		return -1;
	}
	if (wifi_work.profiles_changed) {
		automatic_candidate_skip = 0U;
		errno = EAGAIN;
		return -1;
	}
	saved = ENOENT;
	attempted = 0U;
	for (attempt = 0U; attempt < count; attempt++) {
		attempted++;
		radio = candidates[attempt].radio;
		l2_succeeded = 0;
		if (run_managed_connect(radios[radio].interface,
		    candidates[attempt].profile, NETWORKD_WLAN_AUTO_SEARCHING,
		    deadline, output, output_capacity, output_length, &l2_succeeded) == 0) {
			stop_losing_scans(radios, radio_count, radio);
			automatic_candidate_skip = 0U;
			return 0;
		}
		saved = errno != 0 ? errno : EIO;
		if (wifi_work.cancelled || wifi_work.profiles_changed ||
		    managed_wlan.connection.interface[0] != '\0' ||
		    netutil_monotonic_us() >= deadline)
			break;
	}
	/* Later waves must reach candidates beyond a repeatedly failing prefix. */
	if (wifi_work.profiles_changed)
		automatic_candidate_skip = 0U;
	else {
		automatic_candidate_skip += attempted;
		if (automatic_candidate_skip >= total)
			automatic_candidate_skip = 0U;
	}
	errno = saved;
	return -1;
}

/* Performs one L2/DHCP transaction with a single failure-retirement path. */
static int
run_managed_connect(
	const char *interface,
	const struct wifi_conf_profile *profile,
	enum networkd_managed_wlan_state failure_state,
	uint64_t deadline,
	char *output,
	size_t output_capacity,
	size_t *output_length,
	int *l2_succeeded)
{
	struct networkd_wifi_child_result child;
	uint64_t now;
	uint32_t ifindex;
	unsigned timeout;
	int result;
	int saved;

	*l2_succeeded = 0;
	if (route_events >= 0 && process_route_events() != 0) {
		(void)close(route_events);
		route_events = -1;
	}
	now = netutil_monotonic_us();
	if (now >= deadline || deadline - now < 1000000ULL) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (interface_index(interface, &ifindex) != 0)
		return -1;
	if (networkd_managed_wlan_begin_connect(&managed_wlan, interface,
	    ifindex, route_event_sequence, profile->ssid, profile->ssid_length) != 0)
		return -1;

	memset(&child, 0, sizeof(child));
	timeout = (unsigned)((deadline - now) / 1000000ULL);
	if (timeout > NETWORKD_WLAN_CONNECT_SECONDS)
		timeout = NETWORKD_WLAN_CONNECT_SECONDS;
	result = run_wifi(interface, "connect", profile, timeout, &child);
	if (result == 0) {
		*l2_succeeded = 1;
		if (output != NULL && output_length != NULL)
			result = append_wifi_output(interface, &child, output,
			    output_capacity, output_length);
	}
	saved = errno;
	networkd_wifi_child_result_clear(&child);
	errno = saved;
	if (result == 0)
		result = acquire_managed_l3(interface, deadline);

	/* A stopped child is never substituted for a proven L2/L3 retirement. */
	saved = errno;
	if (result != 0 && retire_managed_connection(failure_state, 1) != 0)
		saved = errno;
	errno = saved;
	return result;
}

/* Records the baseline before DHCP and commits only a complete transaction. */
static int
acquire_managed_l3(
	const char *interface,
	uint64_t deadline)
{
	struct networkd_managed_l3 before;
	struct networkd_managed_l3 after;
	char diagnostic[CHILD_OUTPUT_MAX];
	char seconds[16];
	char *arguments[5];
	uint64_t now;
	uint32_t ifindex;
	unsigned timeout;
	int result;
	int saved;

	memset(&before, 0, sizeof(before));
	memset(&after, 0, sizeof(after));
	memset(diagnostic, 0, sizeof(diagnostic));
	result = snapshot_interface_l3(interface, &ifindex, &before);
	if (result == 0 && ifindex != managed_wlan.connection.ifindex) {
		errno = ENODEV;
		result = -1;
	}
	if (result == 0)
		result = networkd_managed_wlan_begin_l3(&managed_wlan, &before);
	now = netutil_monotonic_us();
	if (result == 0 && (now >= deadline || deadline - now < 1000000ULL)) {
		errno = ETIMEDOUT;
		result = -1;
	}
	if (result == 0) {
		timeout = (unsigned)((deadline - now) / 1000000ULL);
		if (timeout > NETWORKD_WLAN_DHCP_SECONDS)
			timeout = NETWORKD_WLAN_DHCP_SECONDS;
		(void)snprintf(seconds, sizeof(seconds), "%u", timeout);
		arguments[0] = "/sbin/dhcpc";
		arguments[1] = "-t";
		arguments[2] = seconds;
		arguments[3] = (char *)interface;
		arguments[4] = NULL;
		fprintf(stderr, "networkd: %s: Wi-Fi authenticated; acquiring DHCP\n",
		    interface);
		result = run_command_until(arguments, timeout, deadline, diagnostic);
	}
	if (result == 0)
		result = snapshot_managed_l3(&managed_wlan, &after);
	if (result == 0 && (after.address == 0U || !wlan_connected(interface))) {
		errno = ENETDOWN;
		result = -1;
	}
	if (result == 0) {
		identify_l3_ownership(interface, &before, &after);
		result = networkd_managed_wlan_commit_l3(&managed_wlan, &after);
	}

	/* On failure the retained baseline remains available to retirement. */
	saved = errno;
	networkd_protocol_clear(diagnostic, sizeof(diagnostic));
	networkd_protocol_clear(&before, sizeof(before));
	networkd_protocol_clear(&after, sizeof(after));
	errno = saved;
	return result;
}

/* Reconstructs partial DHCP ownership before retrying a failed transaction. */
static int
reconcile_pending_l3(
	void)
{
	struct networkd_managed_l3 current;
	int result;
	int saved;

	memset(&current, 0, sizeof(current));
	result = snapshot_managed_l3(&managed_wlan, &current);
	if (result == 0) {
		identify_l3_ownership(managed_wlan.connection.interface,
		    &managed_wlan.connection.l3_before, &current);
		result = networkd_managed_wlan_track_l3(&managed_wlan, &current);
	}
	saved = errno;
	networkd_protocol_clear(&current, sizeof(current));
	errno = saved;
	return result;
}

/* Captures one interface's exact current IPv4, route, and resolver state. */
static int
snapshot_interface_l3(
	const char *interface,
	uint32_t *ifindex,
	struct networkd_managed_l3 *snapshot)
{
	int descriptor;
	int saved;

	/* Initializes output before opening any fallible resource. */
	if (interface == NULL || ifindex == NULL || snapshot == NULL) {
		errno = EINVAL;
		return -1;
	}
	memset(snapshot, 0, sizeof(*snapshot));
	*ifindex = 0U;

	/* Captures the name identity and all three IPv4 interface fields. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0)
		return -1;
	if (netutil_ifindex(descriptor, interface, ifindex) != 0 ||
	    get_interface_ipv4(descriptor, interface, SIOCGIFADDR,
	    &snapshot->address) != 0 ||
	    get_interface_ipv4(descriptor, interface, SIOCGIFNETMASK,
	    &snapshot->netmask) != 0 ||
	    get_interface_ipv4(descriptor, interface, SIOCGIFBRDADDR,
	    &snapshot->broadcast) != 0 ||
	    find_interface_default(descriptor, *ifindex, snapshot) != 0) {
		saved = errno;
		(void)close(descriptor);
		networkd_protocol_clear(snapshot, sizeof(*snapshot));
		*ifindex = 0U;
		errno = saved;
		return -1;
	}
	if (close(descriptor) != 0) {
		saved = errno;
		networkd_protocol_clear(snapshot, sizeof(*snapshot));
		*ifindex = 0U;
		errno = saved;
		return -1;
	}

	/* Adds the bounded byte-exact resolver snapshot. */
	if (snapshot_resolver(snapshot) != 0) {
		saved = errno;
		networkd_protocol_clear(snapshot, sizeof(*snapshot));
		*ifindex = 0U;
		errno = saved;
		return -1;
	}

	/* Reports one complete coherent snapshot. */
	return 0;
}

/* Captures one live managed identity with bounded transient retries. */
static int
snapshot_managed_l3(
	const struct networkd_managed_wlan *record,
	struct networkd_managed_l3 *snapshot)
{
	const struct networkd_managed_wlan_connection *connection;
	uint32_t ifindex;
	unsigned attempt;
	int saved;

	/* Retries only the snapshot, never the already successful DHCP child. */
	if (record == NULL) {
		errno = EINVAL;
		return -1;
	}
	connection = &record->connection;
	saved = EIO;
	for (attempt = 0U; attempt < 3U; attempt++) {
		if (snapshot_interface_l3(connection->interface, &ifindex,
		    snapshot) == 0) {
			if (ifindex == connection->ifindex)
				return 0;
			saved = ENODEV;
			break;
		}
		saved = errno != 0 ? errno : EIO;
	}

	/* Reports identity loss or a persistent snapshot failure unchanged. */
	errno = saved;
	return -1;
}

/* Captures bounded resolver contents while preserving other L3 fields. */
static int
snapshot_resolver(
	struct networkd_managed_l3 *snapshot)
{
	unsigned char extra;
	ssize_t count;
	size_t used;
	int descriptor;
	int saved;

	/* Clears prior resolver ownership before reading current file state. */
	if (snapshot == NULL) {
		errno = EINVAL;
		return -1;
	}
	snapshot->resolver_present = 0;
	snapshot->resolver_oversized = 0;
	snapshot->resolver_owned = 0;
	snapshot->resolver_length = 0U;
	networkd_protocol_clear(snapshot->resolver, sizeof(snapshot->resolver));

	/* Treats absence as a complete and exact resolver snapshot. */
	descriptor = open("/etc/resolv.conf", O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}

	/* Reads the complete file without silently truncating ownership data. */
	used = 0U;
	while (used < sizeof(snapshot->resolver)) {
		count = read(descriptor, snapshot->resolver + used,
		    sizeof(snapshot->resolver) - used);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			saved = errno;
			(void)close(descriptor);
			networkd_protocol_clear(snapshot->resolver,
			    sizeof(snapshot->resolver));
			errno = saved;
			return -1;
		}
		if (count == 0)
			break;
		used += (size_t)count;
	}
	if (used == sizeof(snapshot->resolver)) {
		do {
			count = read(descriptor, &extra, sizeof(extra));
		} while (count < 0 && errno == EINTR);
		if (count < 0) {
			saved = errno;
			(void)close(descriptor);
			networkd_protocol_clear(snapshot->resolver,
			    sizeof(snapshot->resolver));
			errno = saved;
			return -1;
		}

		/* Preserve oversized external files without blocking unrelated L3 work. */
		if (count > 0) {
			snapshot->resolver_oversized = 1;
			used = 0U;
			networkd_protocol_clear(snapshot->resolver,
			    sizeof(snapshot->resolver));
		}
	}
	if (close(descriptor) != 0) {
		saved = errno;
		networkd_protocol_clear(snapshot->resolver,
		    sizeof(snapshot->resolver));
		errno = saved;
		return -1;
	}
	snapshot->resolver_present = 1;
	snapshot->resolver_length = used;

	/* Reports exact bytes, absence, or an explicitly unowned oversized file. */
	return 0;
}

/* Marks only post-DHCP values which differ from the saved pre-state. */
static void
identify_l3_ownership(
	const char *interface,
	const struct networkd_managed_l3 *previous,
	struct networkd_managed_l3 *committed)
{
	char marker[80];
	size_t marker_length;
	int changed;
	int length;

	/* Owns an IPv4 tuple only when DHCP changed at least one exact value. */
	committed->ipv4_owned =
	    previous->address != committed->address ||
	    previous->netmask != committed->netmask ||
	    previous->broadcast != committed->broadcast;

	/* Owns a default route only when DHCP changed its canonical value. */
	committed->default_route_owned = committed->default_route_present &&
	    (!previous->default_route_present ||
	    !managed_routes_equal(&previous->default_route,
	    &committed->default_route));

	/* Owns resolver data only when dhcpc replaced the preexisting file. */
	committed->resolver_owned = 0;

	/* An unbounded file can never become a bounded ownership token. */
	if (committed->resolver_oversized)
		return;
	changed = previous->resolver_present != committed->resolver_present ||
	    previous->resolver_oversized != committed->resolver_oversized ||
	    previous->resolver_length != committed->resolver_length;
	if (!changed && committed->resolver_length != 0U)
		changed = memcmp(previous->resolver, committed->resolver,
		    committed->resolver_length) != 0;
	if (!changed || !committed->resolver_present)
		return;

	/* Requires the exact interface-specific dhcpc marker. */
	length = snprintf(marker, sizeof(marker),
	    "# Generated by dhcpc for %s\n", interface);
	if (length < 0 || (size_t)length >= sizeof(marker))
		return;
	marker_length = (size_t)length;
	if (committed->resolver_length < marker_length)
		return;
	if (memcmp(committed->resolver, marker, marker_length) != 0)
		return;
	committed->resolver_owned = 1;
}

/* Clears only L3 resources still equal to one managed ownership token. */
static int
clear_interface_l3(
	struct networkd_managed_wlan *record)
{
	struct networkd_managed_wlan_connection *connection;
	struct networkd_managed_l3_cleanup cleanup;
	struct networkd_managed_l3 current;
	uint32_t current_ifindex;
	uint32_t address;
	uint32_t netmask;
	uint32_t broadcast;
	int descriptor;
	int first_error;
	int planned;

	/* Snapshots live state before authorizing any destructive operation. */
	memset(&cleanup, 0, sizeof(cleanup));
	memset(&current, 0, sizeof(current));
	if (record == NULL) {
		errno = EINVAL;
		return -1;
	}
	connection = &record->connection;
	if (snapshot_interface_l3(connection->interface,
	    &current_ifindex, &current) != 0)
		return -1;
	planned = networkd_managed_wlan_plan_l3_cleanup(record,
	    current_ifindex, &current, &cleanup);
	if (planned != 0 && errno != ESTALE) {
		networkd_protocol_clear(&current, sizeof(current));
		return -1;
	}
	/* A changed resource belongs to its new writer, not to this old token. */
	if (planned != 0) {
		if (!cleanup.clear_ipv4)
			connection->l3.ipv4_owned = 0;
		if (!cleanup.delete_default_route)
			connection->l3.default_route_owned = 0;
		if (!cleanup.unlink_resolver)
			connection->l3.resolver_owned = 0;
		fprintf(stderr, "networkd: preserved externally changed Wi-Fi L3 state\n");
	}
	first_error = 0;

	/* Revalidates and clears only an unchanged owned IPv4 tuple. */
	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (descriptor < 0) {
		if (first_error == 0)
			first_error = errno;
	} else if (cleanup.clear_ipv4) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0 ||
		    get_interface_ipv4(descriptor, connection->interface,
		    SIOCGIFADDR, &address) != 0 ||
		    get_interface_ipv4(descriptor, connection->interface,
		    SIOCGIFNETMASK, &netmask) != 0 ||
		    get_interface_ipv4(descriptor, connection->interface,
		    SIOCGIFBRDADDR, &broadcast) != 0) {
			if (first_error == 0)
				first_error = errno;
		} else if (address != connection->l3.address ||
		    netmask != connection->l3.netmask ||
		    broadcast != connection->l3.broadcast) {
			connection->l3.ipv4_owned = 0;
		} else {
			if (set_interface_ipv4(descriptor, connection->interface,
			    SIOCSIFNETMASK, 0U) != 0)
				first_error = errno;
			else
				connection->l3.netmask = 0U;
			if (set_interface_ipv4(descriptor, connection->interface,
			    SIOCSIFBRDADDR, 0U) != 0) {
				if (first_error == 0)
					first_error = errno;
			} else
				connection->l3.broadcast = 0U;
			if (set_interface_ipv4(descriptor, connection->interface,
			    SIOCSIFADDR, 0U) != 0) {
				if (first_error == 0)
					first_error = errno;
			} else
				connection->l3.address = 0U;
			if (first_error == 0)
				connection->l3.ipv4_owned = 0;
		}
	}

	/* Deletes only the exact route still held by the same interface. */
	if (descriptor >= 0 && cleanup.delete_default_route) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0 ||
		    delete_interface_default_exact(descriptor,
		    &connection->l3.default_route) != 0) {
			if (errno == ESTALE || errno == ENOENT)
				connection->l3.default_route_owned = 0;
			else if (first_error == 0)
				first_error = errno;
		} else
			connection->l3.default_route_owned = 0;
	}
	if (descriptor >= 0 && close(descriptor) != 0 && first_error == 0)
		first_error = errno;

	/* Unlinks only byte-identical resolver data after identity revalidation. */
	if (cleanup.unlink_resolver) {
		if (interface_index_name_matches(connection->ifindex,
		    connection->interface) != 0 ||
		    unlink_owned_resolver(record) != 0) {
			if (errno == ESTALE || errno == ENOENT)
				connection->l3.resolver_owned = 0;
			else if (first_error == 0)
				first_error = errno;
		} else
			connection->l3.resolver_owned = 0;
	}
	networkd_protocol_clear(&current, sizeof(current));
	networkd_protocol_clear(&cleanup, sizeof(cleanup));

	/* Reports exact success or a preserved external-state degradation. */
	if (first_error != 0) {
		errno = first_error;
		return -1;
	}
	return 0;
}

/* Finds at most one exact current default route for an interface. */
static int
find_interface_default(
	int descriptor,
	uint32_t ifindex,
	struct networkd_managed_l3 *snapshot)
{
	struct rtentry entry;
	struct sockaddr_in *destination;
	struct sockaddr_in *gateway;
	struct sockaddr_in *mask;
	unsigned ordinal;
	unsigned matches;

	/* Enumerates the complete route table and rejects ambiguous ownership. */
	matches = 0U;
	for (ordinal = 0U;; ordinal++) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0) {
			if (errno == ENOENT)
				break;
			return -1;
		}
		destination = (struct sockaddr_in *)&entry.rt_dst;
		mask = (struct sockaddr_in *)&entry.rt_genmask;
		if (entry.rt_ifindex != ifindex ||
		    destination->sin_addr.s_addr != 0U ||
		    mask->sin_addr.s_addr != 0U)
			continue;
		matches++;
		if (matches > 1U) {
			errno = EBUSY;
			return -1;
		}
		gateway = (struct sockaddr_in *)&entry.rt_gateway;
		snapshot->default_route_present = 1;
		snapshot->default_route.flags = entry.rt_flags;
		snapshot->default_route.ifindex = entry.rt_ifindex;
		snapshot->default_route.destination =
		    destination->sin_addr.s_addr;
		snapshot->default_route.gateway = gateway->sin_addr.s_addr;
		snapshot->default_route.netmask = mask->sin_addr.s_addr;
	}

	/* Reports a canonical absent or uniquely present route. */
	return 0;
}

/* Deletes one default route only after a second exact table comparison. */
static int
delete_interface_default_exact(
	int descriptor,
	const struct networkd_managed_route *owned)
{
	struct rtentry entry;
	struct rtentry selected;
	unsigned ordinal;
	unsigned matches;

	/* Selects one byte-semantically identical route without broad deletion. */
	matches = 0U;
	memset(&selected, 0, sizeof(selected));
	for (ordinal = 0U;; ordinal++) {
		memset(&entry, 0, sizeof(entry));
		entry.rt_index = ordinal;
		if (ioctl(descriptor, SIOCGRTENTRY, &entry) != 0) {
			if (errno == ENOENT)
				break;
			return -1;
		}
		if (!route_matches_owned(&entry, owned))
			continue;
		matches++;
		selected = entry;
	}
	if (matches != 1U) {
		errno = matches == 0U ? ESTALE : EBUSY;
		return -1;
	}

	/* Deletes exactly the route selected by the stable semantic fields. */
	return ioctl(descriptor, SIOCDELRT, &selected);
}

/* Compares one route-table entry with a canonical managed route. */
static int
route_matches_owned(
	const struct rtentry *entry,
	const struct networkd_managed_route *owned)
{
	const struct sockaddr_in *destination;
	const struct sockaddr_in *gateway;
	const struct sockaddr_in *mask;

	/* Compares every stable field while ignoring the transient ordinal. */
	destination = (const struct sockaddr_in *)&entry->rt_dst;
	gateway = (const struct sockaddr_in *)&entry->rt_gateway;
	mask = (const struct sockaddr_in *)&entry->rt_genmask;
	if (entry->rt_flags != owned->flags)
		return 0;
	if (entry->rt_ifindex != owned->ifindex)
		return 0;
	if (destination->sin_addr.s_addr != owned->destination)
		return 0;
	if (gateway->sin_addr.s_addr != owned->gateway)
		return 0;
	if (mask->sin_addr.s_addr != owned->netmask)
		return 0;

	/* Reports exact canonical equality. */
	return 1;
}

/* Compares every canonical field of two managed default routes. */
static int
managed_routes_equal(
	const struct networkd_managed_route *left,
	const struct networkd_managed_route *right)
{
	/* Compares fields explicitly so no representation padding is relevant. */
	if (left->flags != right->flags)
		return 0;
	if (left->ifindex != right->ifindex)
		return 0;
	if (left->destination != right->destination)
		return 0;
	if (left->gateway != right->gateway)
		return 0;
	if (left->netmask != right->netmask)
		return 0;

	/* Reports exact canonical equality. */
	return 1;
}

/* Reads one IPv4 interface field into a network-order scalar. */
static int
get_interface_ipv4(
	int descriptor,
	const char *interface,
	unsigned long command,
	uint32_t *value)
{
	struct ifreq request;
	struct sockaddr_in *address;

	/* Issues one bounded generic interface query. */
	if (value == NULL || netutil_ifreq(&request, interface) != 0)
		return -1;
	if (ioctl(descriptor, command, &request) != 0)
		return -1;
	address = (struct sockaddr_in *)&request.ifr_addr;
	*value = address->sin_addr.s_addr;
	return 0;
}

/* Sets one IPv4 interface field to the supplied network-order value. */
static int
set_interface_ipv4(
	int descriptor,
	const char *interface,
	unsigned long command,
	uint32_t value)
{
	struct ifreq request;
	struct sockaddr_in *address;

	/* Builds the ordinary generic interface request. */
	if (netutil_ifreq(&request, interface) != 0)
		return -1;
	address = (struct sockaddr_in *)&request.ifr_addr;
	address->sin_family = AF_INET;
	address->sin_addr.s_addr = value;
	return ioctl(descriptor, command, &request);
}

/* Unlinks only a resolver file still equal to the owned bounded contents. */
static int
unlink_owned_resolver(
	const struct networkd_managed_wlan *record)
{
	const struct networkd_managed_wlan_connection *connection;
	struct networkd_managed_l3 current;
	int equal;
	int result;
	int saved;

	/* Re-reads the complete file immediately before the destructive step. */
	if (record == NULL) {
		errno = EINVAL;
		return -1;
	}
	connection = &record->connection;
	memset(&current, 0, sizeof(current));
	if (snapshot_resolver(&current) != 0)
		return -1;
	equal = current.resolver_present && !current.resolver_oversized &&
	    current.resolver_length == connection->l3.resolver_length;
	if (equal && current.resolver_length != 0U)
		equal = memcmp(current.resolver, connection->l3.resolver,
		    current.resolver_length) == 0;
	if (!equal) {
		networkd_protocol_clear(&current, sizeof(current));
		errno = ESTALE;
		return -1;
	}

	/* Removes only the file whose full content passed the second check. */
	result = unlink("/etc/resolv.conf");
	saved = errno;
	networkd_protocol_clear(&current, sizeof(current));
	errno = saved;
	return result;
}

/* Runs one child with both an operation bound and an optional transaction deadline. */
static int
run_command_until(
	char *const arguments[],
	unsigned timeout_seconds,
	uint64_t deadline,
	char diagnostic[CHILD_OUTPUT_MAX])
{
	pid_t result;
	ssize_t count;
	char temporary[96];
	int output, status, child_done;
	int child_error;
	int standard_output, standard_error;
	pid_t child;
	uint64_t now;
	uint64_t operation_deadline;

	status = 0;
	child_done = 0;
	child_error = 0;
	now = netutil_monotonic_us();
	operation_deadline = now + (uint64_t)timeout_seconds * 1000000ULL;
	if (deadline == 0U || deadline > operation_deadline)
		deadline = operation_deadline;
	if (wifi_work.deadline != 0U && (deadline == 0U ||
	    deadline > wifi_work.deadline -
	    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL))
		deadline = wifi_work.deadline -
		    NETWORKD_WIFI_CLEANUP_SECONDS * 1000000ULL;
	diagnostic[0] = '\0';
	if (now >= deadline) {
		errno = ETIMEDOUT;
		return -1;
	}

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "/run/networkd-child.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	output = open(temporary, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	/* Handles the output condition. */
	if (output < 0)
		return -1;
	child = fork();

	/* Checks the child process state. */
	if (child == 0) {
		/* Close every inherited or duplicated descriptor on failure. */
		standard_output = dup2(output, STDOUT_FILENO);

		/* Handles the standard output condition. */
		if (standard_output < 0) {
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			close(output);
			_exit(126);
		}
		standard_error = dup2(output, STDERR_FILENO);

		/* Handles an operation failure. */
		if (standard_error < 0) {
			close(STDOUT_FILENO);
			close(STDERR_FILENO);
			close(output);
			_exit(126);
		}
		close(output);
		execv(arguments[0], arguments);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		_exit(127);
	}

	/* Checks the child process state. */
	if (child < 0) {
		child_error = errno;
		close(output);
		unlink(temporary);
		errno = child_error;

		/* Reports operation failure. */
		return -1;
	}
	while (!child_done) {
		if (wifi_wait_pump != NULL)
			child_error = wifi_wait_pump();
		if (child_error == 0 && netutil_monotonic_us() >= deadline)
			child_error = ETIMEDOUT;
		result = waitpid(child, &status, WNOHANG);

		/* Checks the operation result. */
		if (result == child)
			child_done = 1;
		else if (result < 0 && errno != EINTR) {
			child_error = errno != 0 ? errno : EIO;
			/* ECHILD means there is no remaining child to signal. */
			if (child_error != ECHILD) {
				(void)kill(child, SIGKILL);
				do {
					result = waitpid(child, &status, 0);
				} while (result < 0 && errno == EINTR);
			}
			child_done = 1;
		}

		/* Handles the child done condition. */
		if (child_done)
			break;

		/* Cancellation, deadline and wait errors retain their exact cause. */
		if (child_error != 0) {
			(void)kill(child, SIGKILL);
			do {
				result = waitpid(child, &status, 0);
			} while (result < 0 && errno == EINTR);
			break;
		}
		usleep(10000);
	}

	/* Handles a failed lseek operation. */
	if (lseek(output, 0, SEEK_SET) >= 0) {
		count = read(output, diagnostic, CHILD_OUTPUT_MAX - 1U);

		/* Checks the remaining item count. */
		if (count > 0)
			diagnostic[count] = '\0';
	}
	close(output);
	unlink(temporary);
	clean_diagnostic(diagnostic);

	/* Handles the ticks condition. */
	if (child_error != 0) {
		errno = child_error;
		return -1;
	}

	/* Handles a failed WIFEXITED operation. */
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? ENOENT
									: EIO;

		/* Reports operation failure. */
		return -1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the clean diagnostic operation. */
static void
clean_diagnostic(
	char *text)
{
	size_t index, length;

	/* Process each remaining element. */
	length = strlen(text);
	while (length != 0 &&
	       (text[length - 1U] == '\n' || text[length - 1U] == '\r'))

	/* Process each remaining element. */
		text[--length] = '\0';
	for (index = 0; index < length; index++) {
		/* Validates the current text. */
		if ((unsigned char)text[index] < 32U || text[index] == 127)
			text[index] = ' ';
	}
}

/* Supports the default route exists operation. */
static int
default_route_exists(
	void)
{
	const struct sockaddr_in *destination;
	const struct sockaddr_in *mask;
	struct rtentry route;
	int descriptor;

	descriptor = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Process each remaining element. */
	for (route.rt_index = 0; ioctl(descriptor, SIOCGRTENTRY, &route) == 0;
	     route.rt_index++) {
		destination = (const struct sockaddr_in *)&route.rt_dst;
		mask = (const struct sockaddr_in *)&route.rt_genmask;

		/* Handles the destination condition. */
		if (destination->sin_addr.s_addr == 0 &&
		    mask->sin_addr.s_addr == 0) {
			close(descriptor);

			/* Reports operation failure. */
			return 1;
		}
	}
	close(descriptor);

	/* Returns the computed result. */
	return errno == ENOENT ? 0 : -1;
}

/* Supports the write resolver operation. */
static int
write_resolver(
	char *const addresses[],
	int count)
{
	int length;
	struct in_addr parsed;
	char temporary[128], line[64];
	int descriptor, index, prior;

	/* Handles a failed snprintf operation. */
	if (snprintf(temporary, sizeof(temporary), "/etc/resolv.conf.tmp.%ld",
		     (long)getpid()) >= (int)sizeof(temporary)) {
		errno = EOVERFLOW;

		/* Reports operation failure. */
		return -1;
	}
	NCOM_TRACE("resolver-open-enter", 0);
	descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
	NCOM_TRACE("resolver-open-done", 0);

	/* Checks the file descriptor. */
	if (descriptor < 0)
		return -1;

	/* Handles a failed write all operation. */
	if (write_all(descriptor, "# Generated by networkd\n", 24) != 0)
		goto fail;

	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles a failed netutil parse ipv4 operation. */
		if (netutil_parse_ipv4(addresses[index], &parsed) != 0)
			goto fail;

		/* Process each remaining element. */
		for (prior = 0; prior < index; prior++) {
			/* Selects the matching value. */
			if (strcmp(addresses[prior], addresses[index]) == 0)
				break;
		}

		/* Handles the prior condition. */
		if (prior != index)
			continue;
		length = snprintf(line, sizeof(line), "nameserver %s\n",
	     addresses[index]);

		/* Handles a failed write all operation. */
		if (length < 0 || (size_t)length >= sizeof(line) ||
		    write_all(descriptor, line, (size_t)length) != 0)
			goto fail;
	}

	/* Handles a failed fsync operation. */
	NCOM_TRACE("resolver-fsync-enter", 0);
	if (fsync(descriptor) != 0) {
		descriptor = -1;
		goto fail;
	}
	NCOM_TRACE("resolver-close-enter", 0);
	if (close(descriptor) != 0) {
		descriptor = -1;
		goto fail;
	}
	descriptor = -1;
	NCOM_TRACE("resolver-close-done", 0);

	/* Handles a failed rename operation. */
	NCOM_TRACE("resolver-rename-enter", 0);
	if (rename(temporary, "/etc/resolv.conf") != 0)
		goto fail;
	NCOM_TRACE("resolver-rename-done", 0);

	/* Reports successful completion. */
	return 0;

fail:

	/* Checks the file descriptor. */
	if (descriptor >= 0)
		close(descriptor);
	unlink(temporary);

	/* Reports operation failure. */
	return -1;
}

/* Supports the handle signal operation. */
static void
handle_signal(
	int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

/* Supports the ignore signal operation. */
static void
ignore_signal(
	int signal_number)
{
	(void)signal_number;
}
