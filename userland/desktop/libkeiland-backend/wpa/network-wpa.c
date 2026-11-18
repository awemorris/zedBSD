/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Watches wpa_supplicant with separate command and event datagram connections.
 * Requests advance through replies in update, without blocking the caller.
 * Credential operations use independent connections with a two-second bound.
 */
#include "network-wpa.h"
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WPA_DEADLINE_MS 2000U
#define WPA_RETRY_MS 1000U

/* While scans are asked for (ws089-p021), how often the supplicant is asked for one. */
#define WPA_SCAN_MS 10000U

/* Each command has one response and a continuation, never an event response. */
enum network_command {
	COMMAND_NONE,
	COMMAND_STATUS,
	COMMAND_RESULTS,
	COMMAND_SCAN,
	COMMAND_LIST,
	COMMAND_ENABLE,
	COMMAND_SELECT,
	COMMAND_DISCONNECT,
	COMMAND_OFF,
	COMMAND_RECONNECT,
	COMMAND_SCAN_ASKED
};

/*
 * One caller owns both sockets, the cached state and scan, and one request.
 * Closed sockets are -1; pending commands retain their deadline until reply.
 * scan_wanted is set while the compositor asks for scans (ws089-p021), and
 * scan_due is when the supplicant is asked for the next one; such a scan is
 * no request and never finishes one.
 */
struct kl_backend_network {
	struct kl_backend_network *next;
	struct kwpa_socket command;
	struct kwpa_socket events;
	struct kl_backend_network_state state;
	struct kl_backend_network_ap scan[KL_BACKEND_NETWORK_SCAN_MAX];
	size_t scan_count;
	enum network_command pending;
	uint64_t deadline;
	uint64_t retry;
	uint64_t sampled;
	unsigned attached;
	unsigned need_status;
	unsigned need_scan;
	unsigned request;
	unsigned finished;
	int finished_error;
	unsigned profile;
	char joining[KL_BACKEND_NETWORK_SSID_MAX];
	unsigned scan_wanted;
	uint64_t scan_due;
};

/*
 * Live watches belong to the desktop's single event-loop thread.
 * Explicit close removes them; process/library retirement closes the remainder.
 */
static struct kl_backend_network *network_watches;

static void network_cleanup(void) __attribute__((destructor));
static int network_connect(struct kl_backend_network *network);
static void network_drop(struct kl_backend_network *network, unsigned *changed, int error);
static void network_finish(struct kl_backend_network *network, unsigned *changed, int error);
static int network_send(struct kl_backend_network *network, enum network_command pending, const char *command);
static int network_next(struct kl_backend_network *network, unsigned *changed);
static int network_answer(struct kl_backend_network *network, char *reply, unsigned *changed);
static int network_status(struct kl_backend_network *network, char *reply, unsigned *changed);
static int network_scan(struct kl_backend_network *network, char *reply, unsigned *changed);
static void network_events(struct kl_backend_network *network, unsigned *changed);
static unsigned network_wifi(const char *state);
static int wpa_hex(unsigned char digit);

/*
 * Allocates a reconnecting watch without waiting for the first reply.
 */
struct kl_backend_network *
kl_backend_network_open(
	void)
{
	struct kl_backend_network *network;

	/* Keeps a watch even when the service has not started. */
	network = calloc(1, sizeof(*network));
	if (network == NULL)
		return NULL;

	/* Closed descriptors distinguish absent service from an owned socket. */
	network->command.fd = -1;
	network->events.fd = -1;
	network->next = network_watches;
	network_watches = network;
	(void)network_connect(network);

	/* Succeeded: later updates acquire state and reconnect as needed. */
	return network;
}

/*
 * Releases both connections and any outstanding request.
 */
void
kl_backend_network_close(
	struct kl_backend_network *network)
{
	struct kl_backend_network **link;

	/* An absent allocation owns no socket path. */
	if (network == NULL)
		return;

	/* Remove the explicit close from the process's fallback cleanup ownership. */
	for (link = &network_watches; *link != NULL; link = &(*link)->next) {
		if (*link == network) {
			*link = network->next;
			break;
		}
	}

	/* DETACH is best effort; closing also removes the daemon's subscriber. */
	if (network->attached != 0)
		(void)kwpa_send(&network->events, "DETACH");
	kwpa_close(&network->events);
	kwpa_close(&network->command);
	free(network);

	/* Succeeded: no descriptors or local paths remain owned. */
	return;
}

/*
 * Advances replies and events without waiting and reconnects once a second.
 */
int
kl_backend_network_update(
	struct kl_backend_network *network,
	unsigned *changed)
{
	char reply[KWPA_REPLY_MAX];
	struct kl_backend_network_state previous;
	uint64_t now;
	int error;
	int differs;

	/* Requires both the watch and an output mask. */
	if (network == NULL || changed == NULL)
		return EINVAL;
	*changed = 0;
	now = kwpa_milliseconds();

	/* An absent service can start after the desktop has opened its watch. */
	if (network->command.fd < 0 && now >= network->retry) {
		error = network_connect(network);
		if (error == 0)
			*changed |= KL_BACKEND_NETWORK_CHANGED_STATE;
	}

	/* Interface addresses can change independently of supplicant events. */
	if (now >= network->sampled) {
		previous = network->state;
		kwpa_links(&network->state);
		differs = memcmp(&previous, &network->state, sizeof(previous));
		if (differs != 0)
			*changed |= KL_BACKEND_NETWORK_CHANGED_STATE;
		network->sampled = now + WPA_RETRY_MS;
		network->need_status = 1;
	}

	/* No daemon reply can be read while disconnected. */
	if (network->command.fd < 0)
		return 0;
	network_events(network, changed);
	if (network->command.fd < 0)
		return 0;

	/* Reads only the response to the single outstanding command. */
	if (network->pending != COMMAND_NONE) {
		error = kwpa_read(&network->command, reply, sizeof(reply));
		if (error == 0) {
			error = network_answer(network, reply, changed);
			if (error != 0)
				network_drop(network, changed, error);
		} else if (error != EAGAIN && error != EWOULDBLOCK) {
			network_drop(network, changed, error);
		} else if (now >= network->deadline) {
			network_drop(network, changed, ETIMEDOUT);
		}
	}

	/* Starts the next small step only after its predecessor has finished. */
	if (network->command.fd >= 0 &&
	    network->attached != 0 &&
	    network->pending == COMMAND_NONE) {
		error = network_next(network, changed);
		if (error != 0)
			network_drop(network, changed, error);
	}

	/* Succeeded: changes describe only state acquired in this update. */
	return 0;
}

/*
 * Copies the last state without contacting the daemon.
 */
void
kl_backend_network_get_state(
	const struct kl_backend_network *network,
	struct kl_backend_network_state *state)
{
	/* A null watch reports the absent state. */
	if (state == NULL)
		return;
	memset(state, 0, sizeof(*state));
	if (network == NULL)
		return;
	*state = network->state;

	/* Succeeded: the caller owns a stable state snapshot. */
	return;
}

/*
 * Copies the strongest access points from the last completed scan.
 */
size_t
kl_backend_network_get_scan(
	const struct kl_backend_network *network,
	struct kl_backend_network_ap *aps,
	size_t capacity)
{
	size_t count;

	/* An absent watch or destination has no scan entries. */
	if (network == NULL || aps == NULL)
		return 0;
	count = network->scan_count;
	if (count > capacity)
		count = capacity;
	memcpy(aps, network->scan, count * sizeof(*aps));

	/* Succeeded: the caller holds at most its requested capacity. */
	return count;
}

/*
 * Queues one request for the next nonblocking update.
 */
int
kl_backend_network_request(
	struct kl_backend_network *network,
	unsigned request,
	const char *ssid)
{
	size_t length;

	/* Rejects requests outside the public set. */
	if (network == NULL ||
	    request == KL_BACKEND_NETWORK_REQUEST_NONE ||
	    request > KL_BACKEND_NETWORK_REQUEST_PROFILES)
		return EINVAL;
	if (network->command.fd < 0 || network->attached == 0)
		return ENOTCONN;
	if (network->request != KL_BACKEND_NETWORK_REQUEST_NONE)
		return EBUSY;

	/* A join names a bounded nonempty SSID, never command text. */
	if (request == KL_BACKEND_NETWORK_REQUEST_JOIN) {
		if (ssid == NULL)
			return EINVAL;
		length = strlen(ssid);
		if (length == 0 || length >= sizeof(network->joining))
			return EINVAL;
		memcpy(network->joining, ssid, length + 1);
	}

	/* Ownership lasts until update publishes CHANGED_DONE. */
	network->request = request;
	network->finished = KL_BACKEND_NETWORK_REQUEST_NONE;
	network->finished_error = 0;

	/* Succeeded: a later update sends and completes the request. */
	return 0;
}

/*
 * Configures a wired interface (ws089-p022): not on Linux and FreeBSD yet
 * (the beta's backends are zedBSD's; the system's own tools set these).
 */
int
kl_backend_network_configure_wired(
	struct kl_backend_network *network,
	const struct kl_backend_wired_config *config)
{
	(void)network;
	(void)config;

	/* Not supported here. */
	return ENOTSUP;
}

/*
 * Asks the supplicant for scans every WPA_SCAN_MS while on (ws089-p021).
 */
int
kl_backend_network_set_scanning(
	struct kl_backend_network *network,
	unsigned on)
{
	/* Requires the watch. */
	if (network == NULL)
		return EINVAL;

	/* The asking ends: no scan is asked for any more. */
	if (on == 0U) {
		network->scan_wanted = 0U;
		return 0;
	}

	/* The asking begins: a scan is asked for at the next free step. */
	if (network->scan_wanted == 0U) {
		network->scan_wanted = 1U;
		network->scan_due = kwpa_milliseconds();
	}

	/* Succeeded: the asking is recorded. */
	return 0;
}

/*
 * Reports the outstanding request or the most recent completed request.
 */
unsigned
kl_backend_network_get_request(
	const struct kl_backend_network *network,
	int *error)
{
	/* A missing watch has neither pending work nor failure. */
	if (error != NULL)
		*error = 0;
	if (network == NULL)
		return KL_BACKEND_NETWORK_REQUEST_NONE;
	if (network->request != KL_BACKEND_NETWORK_REQUEST_NONE)
		return network->request;
	if (error != NULL)
		*error = network->finished_error;

	/* Succeeded: the completion survives until the next request. */
	return network->finished;
}

/*
 * Opens one private datagram connection to the selected supplicant interface.
 */
int
kwpa_open(
	struct kwpa_socket *connection,
	const char *interface)
{
	DIR *directory;
	struct dirent *entry;
	struct sockaddr_un address;
	struct stat status;
	char peer[108];
	char selected[KL_BACKEND_NETWORK_NAME_MAX];
	const char *control_directory;
	socklen_t address_length;
	char *temporary;
	size_t length;
	int error;
	int socket_node;
	int prefix;
	int written;

	/* Initializes cleanup ownership before any fallible operation. */
	memset(connection, 0, sizeof(*connection));
	connection->fd = -1;
	selected[0] = '\0';

	/* Uses the selected operating system's native control socket directory. */
	control_directory = kwpa_control_directory();

	/* Without an explicit interface, choose the first non-P2P control socket. */
	if (interface == NULL) {
		directory = opendir(control_directory);
		if (directory == NULL)
			return errno;
		for (;;) {
			/* Directory entries are checked as sockets before use. */
			entry = readdir(directory);
			if (entry == NULL)
				break;
			prefix = strncmp(entry->d_name, "p2p-", 4);
			if (entry->d_name[0] == '.' || prefix == 0)
				continue;
			length = strlen(entry->d_name);
			if (length >= sizeof(selected))
				continue;
			(void)snprintf(peer, sizeof(peer), "%s/%s", control_directory, entry->d_name);
			error = stat(peer, &status);
			if (error != 0)
				continue;
			socket_node = S_ISSOCK(status.st_mode);
			if (socket_node == 0)
				continue;
			memcpy(selected, entry->d_name, length + 1);
			break;
		}

		/* Directory iteration never transfers ownership of entry storage. */
		(void)closedir(directory);
		if (selected[0] == '\0')
			return ENOENT;
		interface = selected;
	}

	/* The interface name cannot introduce another path component. */
	length = strlen(interface);
	if (length == 0 || length >= sizeof(connection->interface))
		return EINVAL;
	for (error = 0; (size_t)error < length; error++) {
		if (interface[error] == '/')
			return EINVAL;
	}

	/* Create a private directory rather than unlinking a guessed shared path. */
	memcpy(connection->interface, interface, length + 1);
	(void)snprintf(connection->directory, sizeof(connection->directory), "/tmp/keiland-wpa-XXXXXX");
	temporary = mkdtemp(connection->directory);
	if (temporary == NULL) {
		connection->directory[0] = '\0';
		return errno;
	}

	/* A private nonblocking connection never inherits into child applications. */
	connection->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (connection->fd < 0) {
		error = errno;
		kwpa_close(connection);
		return error;
	}

	/* The local pathname stays owned until close, including failed connect. */
	(void)snprintf(connection->path, sizeof(connection->path), "%s/socket", connection->directory);
	error = kwpa_socket_address(&address, connection->path, &address_length);
	if (error != 0) {
		kwpa_close(connection);
		return error;
	}

	/* Binds the private endpoint with its native address extent. */
	error = bind(connection->fd, (struct sockaddr *)&address, address_length);
	if (error != 0) {
		error = errno;
		kwpa_close(connection);
		return error;
	}

	/* The selected daemon socket is the sole peer of this connection. */
	written = snprintf(peer, sizeof(peer), "%s/%s", control_directory, interface);
	if (written < 0 || (size_t)written >= sizeof(peer)) {
		kwpa_close(connection);
		return EOVERFLOW;
	}

	/* Connecting a datagram socket does not wait for a daemon response. */
	error = kwpa_socket_address(&address, peer, &address_length);
	if (error != 0) {
		kwpa_close(connection);
		return error;
	}

	/* Selects the native datagram peer without waiting for daemon replies. */
	error = connect(connection->fd, (struct sockaddr *)&address, address_length);
	if (error != 0) {
		error = errno;
		kwpa_close(connection);
		return error;
	}

	/* Succeeded: close owns the descriptor, pathname and private directory. */
	return 0;
}

/*
 * Removes only the private paths owned by one control connection.
 */
void
kwpa_close(
	struct kwpa_socket *connection)
{
	/* An owned directory proves which paths this client may unlink. */
	if (connection->fd >= 0)
		(void)close(connection->fd);
	if (connection->directory[0] != '\0') {
		if (connection->path[0] != '\0')
			(void)unlink(connection->path);
		(void)rmdir(connection->directory);
	}

	/* Closed records cannot remove their former paths a second time. */
	connection->fd = -1;
	connection->directory[0] = '\0';
	connection->path[0] = '\0';

	/* Succeeded: local connection ownership is empty. */
	return;
}

/*
 * Sends one command without waiting for its response.
 */
int
kwpa_send(
	struct kwpa_socket *connection,
	const char *command)
{
	ssize_t sent;
	size_t length;

	/* A closed socket cannot own a pending command. */
	if (connection->fd < 0)
		return ENOTCONN;
	length = strlen(command);
	sent = send(connection->fd, command, length, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (sent < 0)
		return errno;
	if ((size_t)sent != length)
		return EIO;

	/* Succeeded: the entire datagram is queued to the service. */
	return 0;
}

/*
 * Reads one complete bounded reply without waiting.
 */
int
kwpa_read(
	struct kwpa_socket *connection,
	char *reply,
	size_t capacity)
{
	ssize_t received;
	char *embedded;

	/* Reserve one byte for a terminating NUL. */
	if (capacity < 2)
		return EINVAL;
	received = recv(connection->fd, reply, capacity - 1, MSG_DONTWAIT | MSG_TRUNC);
	if (received < 0)
		return errno;
	if (received == 0 || (size_t)received >= capacity)
		return EOVERFLOW;
	embedded = memchr(reply, '\0', (size_t)received);
	if (embedded != NULL)
		return EPROTO;
	reply[received] = '\0';

	/* Succeeded: the text belongs to exactly one datagram. */
	return 0;
}

/*
 * Waits at most two seconds for an independent credential command.
 */
int
kwpa_call(
	struct kwpa_socket *connection,
	const char *command,
	char *reply,
	size_t capacity)
{
	struct pollfd ready;
	uint64_t deadline;
	uint64_t now;
	int error;
	int polled;

	/* A failed send has no response that could be mistaken for the next call. */
	error = kwpa_send(connection, command);
	if (error != 0)
		return error;
	deadline = kwpa_milliseconds() + WPA_DEADLINE_MS;
	ready.fd = connection->fd;
	ready.events = POLLIN;
	ready.revents = 0;

	/* Retries interruption within the original deadline. */
	for (;;) {
		now = kwpa_milliseconds();
		if (now >= deadline)
			return ETIMEDOUT;
		polled = poll(&ready, 1, (int)(deadline - now));
		if (polled < 0) {
			if (errno == EINTR)
				continue;
			return errno;
		}

		/* A timeout cannot leave an old response attached to a new command. */
		if (polled == 0)
			return ETIMEDOUT;
		error = kwpa_read(connection, reply, capacity);
		if (error == EAGAIN || error == EWOULDBLOCK)
			continue;
		if (error != 0)
			return error;

		/* Succeeded: the caller holds the sole command response. */
		return 0;
	}
}

/*
 * Classifies a command's acknowledgement without accepting a prefix.
 */
int
kwpa_ok(
	const char *reply)
{
	int same;

	/* The normal daemon acknowledgement is a complete OK line. */
	same = strcmp(reply, "OK\n");
	if (same == 0)
		return 0;
	same = strcmp(reply, "OK");
	if (same != 0)
		return EIO;

	/* Succeeded: the daemon acknowledged the command. */
	return 0;
}

/*
 * Decodes printable supplicant escapes into a bounded SSID.
 */
void
kwpa_decode(
	const char *encoded,
	char *ssid,
	size_t capacity)
{
	size_t copied;
	unsigned char byte;
	int high;
	int low;

	/* A zero-capacity caller cannot receive even the terminator. */
	if (capacity == 0)
		return;
	copied = 0;

	/* Every decoded byte consumes one output slot, including UTF-8 bytes. */
	while (*encoded != '\0' && copied + 1 < capacity) {
		byte = (unsigned char)*encoded++;
		if (byte == '\\' && *encoded != '\0') {
			byte = (unsigned char)*encoded++;
			if (byte == 'x' &&
			    encoded[0] != '\0' &&
			    encoded[1] != '\0') {
				high = wpa_hex((unsigned char)encoded[0]);
				low = wpa_hex((unsigned char)encoded[1]);
				if (high >= 0 && low >= 0) {
					byte = (unsigned char)(high * 16 + low);
					encoded += 2;
				}
			} else if (byte == 'e') {
				byte = 27;
			} else if (byte == 'n') {
				byte = '\n';
			} else if (byte == 'r') {
				byte = '\r';
			} else if (byte == 't') {
				byte = '\t';
			}
		}

		/* Embedded NUL cannot form a printable public SSID. */
		if (byte == 0)
			break;
		ssid[copied++] = (char)byte;
	}

	/* Succeeded: the complete retained prefix is terminated. */
	ssid[copied] = '\0';
	return;
}

/*
 * Finds an exact decoded SSID in a LIST_NETWORKS response.
 */
int
kwpa_profile(
	char *reply,
	const char *ssid,
	unsigned *id)
{
	char *line;
	char *save;
	char *tab;
	char *end;
	char decoded[KL_BACKEND_NETWORK_SSID_MAX];
	unsigned long number;
	int same;

	/* The first line is the table heading. */
	line = strtok_r(reply, "\n", &save);
	if (line == NULL)
		return EPROTO;
	same = strcmp(line, "network id / ssid / bssid / flags");
	if (same != 0)
		return EPROTO;

	/* Exact field boundaries keep SSIDs out of command syntax. */
	for (;;) {
		line = strtok_r(NULL, "\n", &save);
		if (line == NULL)
			break;
		tab = strchr(line, '\t');
		if (tab == NULL)
			continue;
		*tab++ = '\0';
		errno = 0;
		number = strtoul(line, &end, 10);
		if (errno != 0 ||
		    end == line ||
		    *end != '\0' ||
		    number > 0x7fffffffUL)
			continue;
		end = strchr(tab, '\t');
		if (end == NULL)
			continue;
		*end = '\0';
		kwpa_decode(tab, decoded, sizeof(decoded));
		same = strcmp(decoded, ssid);
		if (same == 0) {
			*id = (unsigned)number;
			return 0;
		}
	}

	/* An unsaved SSID cannot be joined without a saved profile. */
	return ENOENT;
}

/*
 * Reads the monotonic clock for reconnect and reply deadlines.
 */
uint64_t
kwpa_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* A clock failure cannot produce an uninitialized deadline. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: wall clock adjustments cannot change reply bounds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Creates command and event peers without waiting for ATTACH acknowledgement. */
static int
network_connect(
	struct kl_backend_network *network)
{
	int error;

	/* A failed discovery is tried again at most once a second. */
	network->retry = kwpa_milliseconds() + WPA_RETRY_MS;
	error = kwpa_open(&network->command, NULL);
	if (error != 0)
		return error;
	error = kwpa_open(&network->events, network->command.interface);
	if (error != 0) {
		kwpa_close(&network->command);
		return error;
	}

	/* Monitoring starts independently of command responses. */
	error = kwpa_send(&network->events, "ATTACH");
	if (error != 0) {
		kwpa_close(&network->events);
		kwpa_close(&network->command);
		return error;
	}

	/* The first STATUS is sent only after the monitor is attached. */
	network->attached = 0;
	network->deadline = kwpa_milliseconds() + WPA_DEADLINE_MS;
	network->need_status = 1;
	(void)snprintf(network->state.wifi_interface, sizeof(network->state.wifi_interface), "%s", network->command.interface);

	/* Succeeded: update owns the pending ATTACH response. */
	return 0;
}

/* Drops both peers so a late reply cannot complete a different request. */
static void
network_drop(
	struct kl_backend_network *network,
	unsigned *changed,
	int error)
{
	/* Service loss terminates any pending user operation with its real error. */
	if (network->request != KL_BACKEND_NETWORK_REQUEST_NONE)
		network_finish(network, changed, error);
	kwpa_close(&network->events);
	kwpa_close(&network->command);
	network->pending = COMMAND_NONE;
	network->attached = 0;
	network->state.reachable = 0;
	network->state.wifi = KL_BACKEND_WIFI_ABSENT;
	network->state.ssid[0] = '\0';
	network->need_scan = 0;
	network->scan_count = 0;
	kwpa_links(&network->state);
	*changed |= KL_BACKEND_NETWORK_CHANGED_SCAN;
	network->retry = kwpa_milliseconds() + WPA_RETRY_MS;
	*changed |= KL_BACKEND_NETWORK_CHANGED_STATE;

	/* Succeeded: only an eventual new connection may supply another reply. */
	return;
}

/* Publishes one completed operation without dropping the watch. */
static void
network_finish(
	struct kl_backend_network *network,
	unsigned *changed,
	int error)
{
	/* The completed kind and error survive until the next request. */
	network->finished = network->request;
	network->finished_error = error;
	network->request = KL_BACKEND_NETWORK_REQUEST_NONE;
	*changed |= KL_BACKEND_NETWORK_CHANGED_DONE;
	network->need_status = 1;

	/* Succeeded: the caller can consume CHANGED_DONE and the saved outcome. */
	return;
}

/* Starts a single command and records its original response deadline. */
static int
network_send(
	struct kl_backend_network *network,
	enum network_command pending,
	const char *command)
{
	int error;

	/* Datagram send must finish before this record owns a response. */
	error = kwpa_send(&network->command, command);
	if (error != 0)
		return error;
	network->pending = pending;
	network->deadline = kwpa_milliseconds() + WPA_DEADLINE_MS;

	/* Succeeded: update waits for exactly this command's response. */
	return 0;
}

/* Chooses user work before refreshed scans and periodic state queries. */
static int
network_next(
	struct kl_backend_network *network,
	unsigned *changed)
{
	enum network_command pending;
	const char *command;
	uint64_t now;
	int error;

	/* Credential refresh is already performed by the independent save operation. */
	if (network->request == KL_BACKEND_NETWORK_REQUEST_PROFILES) {
		network_finish(network, changed, 0);
		return 0;
	}

	/* Each public operation begins with the daemon's corresponding command. */
	now = kwpa_milliseconds();
	pending = COMMAND_NONE;
	command = NULL;
	if (network->request == KL_BACKEND_NETWORK_REQUEST_SCAN) {
		pending = COMMAND_SCAN;
		command = "SCAN";
	} else if (network->request == KL_BACKEND_NETWORK_REQUEST_JOIN) {
		pending = COMMAND_LIST;
		command = "LIST_NETWORKS";
	} else if (network->request == KL_BACKEND_NETWORK_REQUEST_DISCONNECT) {
		pending = COMMAND_DISCONNECT;
		command = "DISCONNECT";
	} else if (network->request == KL_BACKEND_NETWORK_REQUEST_WIFI_OFF) {
		pending = COMMAND_OFF;
		command = "DISCONNECT";
	} else if (network->request == KL_BACKEND_NETWORK_REQUEST_WIFI_ON) {
		error = kwpa_radio(network->command.interface, 1);
		if (error != 0) {
			network_finish(network, changed, error);
			return 0;
		}

		/* A successful interface-up permits reconnect after disconnection. */
		pending = COMMAND_RECONNECT;
		command = "RECONNECT";
	} else if (network->need_scan != 0) {
		network->need_scan = 0;
		pending = COMMAND_RESULTS;
		command = "SCAN_RESULTS";
	} else if (network->scan_wanted != 0U && now >= network->scan_due) {
		/* A scan the compositor asks for; its results come as an event. */
		network->scan_due = now + WPA_SCAN_MS;
		pending = COMMAND_SCAN_ASKED;
		command = "SCAN";
	} else if (network->need_status != 0) {
		network->need_status = 0;
		pending = COMMAND_STATUS;
		command = "STATUS";
	}

	/* An idle watch performs no syscall when it has no work. */
	if (command == NULL)
		return 0;
	error = network_send(network, pending, command);
	if (error != 0)
		return error;

	/* Succeeded: one chosen step is now waiting for its reply. */
	return 0;
}

/* Interprets one response and starts only the next required join step. */
static int
network_answer(
	struct kl_backend_network *network,
	char *reply,
	unsigned *changed)
{
	enum network_command pending;
	char command[64];
	int error;
	int busy;

	/* Releasing this reply slot permits a continuation to own the socket. */
	pending = network->pending;
	network->pending = COMMAND_NONE;
	if (pending == COMMAND_STATUS) {
		error = network_status(network, reply, changed);
		if (error != 0)
			return error;
		return 0;
	}

	/* An asked-for scan's acknowledgement finishes nothing (a busy radio scans already). */
	if (pending == COMMAND_SCAN_ASKED)
		return 0;

	/* Scan results belong to the cache rather than a command acknowledgement. */
	if (pending == COMMAND_RESULTS) {
		error = network_scan(network, reply, changed);
		if (error != 0)
			return error;
		return 0;
	}

	/* JOIN resolves a saved profile before enabling or selecting it. */
	if (pending == COMMAND_LIST) {
		error = kwpa_profile(reply, network->joining, &network->profile);
		if (error != 0) {
			network_finish(network, changed, error);
			return 0;
		}

		/* Enabling is not persisted here; SELECT is the user's explicit join. */
		(void)snprintf(command, sizeof(command), "ENABLE_NETWORK %u", network->profile);
		error = network_send(network, COMMAND_ENABLE, command);
		if (error != 0)
			return error;
		return 0;
	}

	/* An already-running scan will still publish its result event. */
	error = kwpa_ok(reply);
	busy = strncmp(reply, "FAIL-BUSY", 9);
	if (pending == COMMAND_SCAN && busy == 0)
		error = 0;
	if (error != 0) {
		network_finish(network, changed, error);
		return 0;
	}

	/* Selecting never saves the incidental disabled state of other profiles. */
	if (pending == COMMAND_ENABLE) {
		(void)snprintf(command, sizeof(command), "SELECT_NETWORK %u", network->profile);
		error = network_send(network, COMMAND_SELECT, command);
		if (error != 0)
			return error;
		return 0;
	}

	/* WIFI_OFF disables the radio only after disconnection has been accepted. */
	if (pending == COMMAND_OFF)
		error = kwpa_radio(network->command.interface, 0);
	network_finish(network, changed, error);

	/* Succeeded: the user operation has a durable completion outcome. */
	return 0;
}

/* Maps daemon state names to the public Wi-Fi states. */
static unsigned
network_wifi(
	const char *state)
{
	int same;

	/* Disabled, scanning and completed have distinct public meanings. */
	same = strcmp(state, "INTERFACE_DISABLED");
	if (same == 0)
		return KL_BACKEND_WIFI_OFF;
	same = strcmp(state, "SCANNING");
	if (same == 0)
		return KL_BACKEND_WIFI_SEARCHING;
	same = strcmp(state, "COMPLETED");
	if (same == 0)
		return KL_BACKEND_WIFI_CONNECTED;
	same = strcmp(state, "DISCONNECTED");
	if (same == 0)
		return KL_BACKEND_WIFI_DISCONNECTED;
	same = strcmp(state, "INACTIVE");
	if (same == 0)
		return KL_BACKEND_WIFI_DISCONNECTED;

	/* Association and key exchanges share the connecting state. */
	same = strcmp(state, "AUTHENTICATING");
	if (same == 0)
		return KL_BACKEND_WIFI_CONNECTING;
	same = strcmp(state, "ASSOCIATING");
	if (same == 0)
		return KL_BACKEND_WIFI_CONNECTING;
	same = strcmp(state, "ASSOCIATED");
	if (same == 0)
		return KL_BACKEND_WIFI_CONNECTING;
	same = strcmp(state, "4WAY_HANDSHAKE");
	if (same == 0)
		return KL_BACKEND_WIFI_CONNECTING;
	same = strcmp(state, "GROUP_HANDSHAKE");
	if (same == 0)
		return KL_BACKEND_WIFI_CONNECTING;

	/* Unknown daemon states do not falsely announce a connection. */
	return KL_BACKEND_WIFI_ABSENT;
}

/* Replaces the cached radio state from complete key/value lines. */
static int
network_status(
	struct kl_backend_network *network,
	char *reply,
	unsigned *changed)
{
	struct kl_backend_network_state state;
	char *line;
	char *save;
	int prefix;
	int differs;
	unsigned has_state;

	/* Initialize a fresh state so omitted fields cannot retain an old SSID. */
	has_state = 0;
	state = network->state;
	state.reachable = 1;
	state.wifi = KL_BACKEND_WIFI_ABSENT;
	state.ssid[0] = '\0';
	line = strtok_r(reply, "\n", &save);

	/* Unknown keys are harmless extensions of the supplicant protocol. */
	while (line != NULL) {
		prefix = strncmp(line, "wpa_state=", 10);
		if (prefix == 0) {
			state.wifi = network_wifi(line + 10);
			has_state = 1;
		} else {
			prefix = strncmp(line, "ssid=", 5);
			if (prefix == 0)
				kwpa_decode(line + 5, state.ssid, sizeof(state.ssid));
		}

		/* Advance by whole lines without borrowing another message buffer. */
		line = strtok_r(NULL, "\n", &save);
	}

	/* A failed or malformed response cannot claim daemon reachability. */
	if (has_state == 0)
		return EPROTO;

	/* Interface state supplies connection kind independently of authentication. */
	kwpa_links(&state);
	differs = memcmp(&state, &network->state, sizeof(state));
	if (differs != 0) {
		network->state = state;
		*changed |= KL_BACKEND_NETWORK_CHANGED_STATE;
	}

	/* Succeeded: the cache describes the last complete STATUS reply. */
	return 0;
}

/* Keeps the strongest BSS of each SSID and orders the retained scan by RSSI. */
static int
network_scan(
	struct kl_backend_network *network,
	char *reply,
	unsigned *changed)
{
	struct kl_backend_network_ap ap;
	struct kl_backend_network_ap swapped;
	char *line;
	char *save;
	char *fields[5];
	char *tab;
	char *flags;
	size_t field;
	size_t index;
	size_t other;
	size_t weakest;
	int found;
	int same;
	int parsed;

	/* Each completed result table replaces the old scan atomically. */
	line = strtok_r(reply, "\n", &save);
	if (line == NULL)
		return EPROTO;
	same = strcmp(line, "bssid / frequency / signal level / flags / ssid");
	if (same != 0)
		return EPROTO;
	network->scan_count = 0;

	/* Each BSS has five tab-separated fields; the last contains the SSID. */
	for (;;) {
		line = strtok_r(NULL, "\n", &save);
		if (line == NULL)
			break;
		fields[0] = line;
		for (field = 1; field < 5; field++) {
			tab = strchr(fields[field - 1], '\t');
			if (tab == NULL)
				break;
			*tab = '\0';
			fields[field] = tab + 1;
		}

		/* Reject malformed rows without mixing fields from adjacent BSS entries. */
		if (field != 5)
			continue;
		memset(&ap, 0, sizeof(ap));
		kwpa_decode(fields[4], ap.ssid, sizeof(ap.ssid));
		if (ap.ssid[0] == '\0')
			continue;
		parsed = sscanf(fields[2], "%d", &ap.rssi);
		if (parsed != 1)
			continue;
		flags = strstr(fields[3], "WPA");
		if (flags == NULL)
			flags = strstr(fields[3], "WEP");
		if (flags == NULL)
			flags = strstr(fields[3], "SAE");
		if (flags != NULL)
			ap.secured = 1;

		/* Replace duplicates only when the new BSS is stronger. */
		found = 0;
		for (index = 0; index < network->scan_count; index++) {
			same = strcmp(network->scan[index].ssid, ap.ssid);
			if (same != 0)
				continue;
			found = 1;
			if (ap.rssi > network->scan[index].rssi)
				network->scan[index] = ap;
			break;
		}

		/* A bounded cache never writes beyond the public scan capacity. */
		if (found == 0) {
			if (network->scan_count < KL_BACKEND_NETWORK_SCAN_MAX) {
				network->scan[network->scan_count++] = ap;
			} else {
				/* Strong BSS entries beyond capacity can replace the weakest retained one. */
				weakest = 0;
				for (index = 1; index < network->scan_count; index++) {
					if (network->scan[index].rssi < network->scan[weakest].rssi)
						weakest = index;
				}

				/* Preserve the cache when the extra entry is weaker than every retained BSS. */
				if (ap.rssi > network->scan[weakest].rssi)
					network->scan[weakest] = ap;
			}
		}
	}

	/* Sort by signal strength so callers receive the strongest entries first. */
	for (index = 0; index < network->scan_count; index++) {
		for (other = index + 1; other < network->scan_count; other++) {
			if (network->scan[other].rssi > network->scan[index].rssi) {
				swapped = network->scan[index];
				network->scan[index] = network->scan[other];
				network->scan[other] = swapped;
			}
		}
	}

	/* Succeeded: a new completed scan is available independently of STATUS. */
	*changed |= KL_BACKEND_NETWORK_CHANGED_SCAN;
	return 0;
}

/* Drains a bounded number of monitor events without waiting. */
static void
network_events(
	struct kl_backend_network *network,
	unsigned *changed)
{
	char reply[KWPA_REPLY_MAX];
	char *event;
	char *found;
	uint64_t now;
	unsigned count;
	int error;

	/* Monitoring acknowledgement has the same bounded startup deadline. */
	if (network->attached == 0) {
		error = kwpa_read(&network->events, reply, sizeof(reply));
		if (error == EAGAIN || error == EWOULDBLOCK) {
			now = kwpa_milliseconds();
			if (now >= network->deadline)
				network_drop(network, changed, ETIMEDOUT);
			return;
		}

		/* A failed acknowledgement must not create a reachable watch. */
		if (error == 0)
			error = kwpa_ok(reply);
		if (error != 0) {
			network_drop(network, changed, error);
			return;
		}

		/* Acknowledged subscription now permits command queries. */
		network->attached = 1;
	}

	/* A burst cannot keep the application's update loop busy indefinitely. */
	for (count = 0; count < 32; count++) {
		error = kwpa_read(&network->events, reply, sizeof(reply));
		if (error == EAGAIN || error == EWOULDBLOCK)
			break;
		if (error != 0) {
			network_drop(network, changed, error);
			return;
		}

		/* The numeric severity prefix is transport metadata, not event content. */
		event = reply;
		if (reply[0] == '<') {
			found = strchr(reply, '>');
			if (found != NULL)
				event = found + 1;
		}

		/* The result event schedules a command on the other connection. */
		found = strstr(event, "CTRL-EVENT-SCAN-RESULTS");
		if (found != NULL)
			network->need_scan = 1;
		found = strstr(event, "CTRL-EVENT-TERMINATING");
		if (found != NULL) {
			network_drop(network, changed, ENOTCONN);
			return;
		}

		/* Connection and scan events refresh STATUS without parsing event prose. */
		network->need_status = 1;
	}

	/* Succeeded: queued event work is retained for the next command slot. */
	return;
}

/* Converts one hexadecimal digit without accepting malformed escapes. */
static int
wpa_hex(
	unsigned char digit)
{
	/* Decimal digits occupy the first ten values. */
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;

	/* A non-hexadecimal byte cannot decode an escape. */
	return -1;
}

/* Retires watches that a desktop process leaves to library or process teardown. */
static void
network_cleanup(
	void)
{
	/* Explicit close also advances the registry before freeing each allocation. */
	while (network_watches != NULL)
		kl_backend_network_close(network_watches);

	/* Succeeded: ordinary process/library retirement leaves no owned Unix paths. */
	return;
}
