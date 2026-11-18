/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop's view of the network (ws035-p013), over networkd.
 *
 * A watch is one connection to networkd that asked SUBSCRIBE and stays
 * open: networkd writes the state on it at once and again whenever it
 * changes.  The state is the text "net show" prints -- a line an interface,
 * "NAME static|link-local|unconfigured online|offline" -- and a last line on the
 * Wi-Fi, "wifi state=NAME interface=IF ssid=HEX radios=N scan=N radio=IF".
 *
 * A request (a scan, a join, a disconnect, the Wi-Fi switch) is one more
 * connection that carries one frame each way, as "net wifi ..." sends it.
 * The frame is written at once and the connection kept until the answer is
 * readable; the link's thread waits on every connection and the updates
 * take what it found without waiting, so the caller's loop never stops for
 * the daemon.
 *
 * Fresh scans (ws089-p021) go on a third connection of their own, one
 * frame each way like a request but beside it, so that a person's request
 * never waits behind a scan: while the compositor asks for scans, the
 * daemon is asked for them (WIFI_SCAN_START, renewed before its lease runs
 * out) and the scan is read every few seconds (WIFI_LIST); when it stops
 * asking, the daemon is told (WIFI_SCAN_STOP).
 *
 * This is the one place in the desktop that knows networkd's protocol
 * (userland/base/net/protocol.h); a change of the daemon's protocol is
 * made here and nowhere else.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"
#include "userland/desktop/libkeiland-backend-zedbsd/daemon-link-zedbsd.h"

#include "userland/base/net/protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The watch's request ID, and the first of the requests'. */
#define NETWORK_WATCH_ID 1U

/* How long a watch that could not be made waits before it is tried again. */
#define NETWORK_RETRY_MS 1000U

/* How long a frame that has begun to arrive may take to arrive whole. */
#define NETWORK_FRAME_SECONDS 2U

/* The longest line of a state or a scan that is read. */
#define NETWORK_LINE_MAX 512U

/* While scans are asked for: how often the scan is read, and how often the daemon's lease is renewed (it lasts 30 s). */
#define NETWORK_SCAN_LIST_MS 3000U
#define NETWORK_SCAN_LEASE_MS 10000U

/* How long the daemon may take to answer a frame of the scans' connection before it is given up. */
#define NETWORK_SCAN_ANSWER_MS 15000U

/*
 * One watch of the network: the SUBSCRIBE connection, the state it last
 * reported, the request outstanding (its connection and kind), the last
 * request that finished with its errno value, and the last scan's
 * networks, the strongest first.
 *
 * watch is -1 while there is no watch; retry_ms is when one is tried
 * again.  request_fd is -1 while no request is outstanding.
 *
 * The scans' connection (ws089-p021): scan_wanted while the compositor
 * asks for scans, scan_leased while the daemon was last asked for them
 * (and not told to stop), scan_fd the frame outstanding on it (-1 for
 * none) with its opcode, ID and the time it is given up, scan_list_ms when
 * the scan is read next and scan_lease_ms when the lease is renewed.
 * Completed requests retain their outcome here after their connection is
 * released: finished_serial is the event loop's number of the request that
 * finished last, request_serial that of the one outstanding.  changed has
 * the KL_BACKEND_NETWORK_CHANGED_* bits found since the last publication.
 *
 * Only the link's thread reads and writes it, from the link's start until
 * its stop; after the stop the record's close releases the connections.
 */
struct network_link {
	int watch;
	uint64_t retry_ms;
	struct kl_backend_network_state state;
	int request_fd;
	unsigned request;
	uint32_t request_opcode;
	uint32_t request_id;
	uint32_t next_id;
	unsigned finished;
	int finished_error;
	struct kl_backend_network_ap scan[KL_BACKEND_NETWORK_SCAN_MAX];
	size_t scan_count;
	unsigned scan_wanted;
	unsigned scan_leased;
	int scan_fd;
	uint32_t scan_opcode;
	uint32_t scan_id;
	uint64_t scan_given_up_ms;
	uint64_t scan_list_ms;
	uint64_t scan_lease_ms;
	uint32_t request_serial;
	uint32_t finished_serial;
	unsigned changed;
};

/* What a request handed to the thread asks. */
#define NETWORK_OP_REQUEST	1U
#define NETWORK_OP_SCANNING	2U

/*
 * What the event loop sees of the watch: the state, the last scan's
 * networks, the last request that finished (its kind, errno value and the
 * event loop's number of it), and whether the watch's connection is there.
 */
struct network_view {
	struct kl_backend_network_state state;
	struct kl_backend_network_ap scan[KL_BACKEND_NETWORK_SCAN_MAX];
	size_t scan_count;
	unsigned finished;
	int finished_error;
	uint32_t finished_serial;
};

/*
 * One watch of the network: the link (its thread and lock), the thread's
 * own record, the view the thread published with the changes the event
 * loop has not taken (both under the link's lock), the event loop's copy
 * of the view, and the event loop's last request: its number (0 before
 * the first), its kind, and whether scans are asked for.
 *
 * The subscriber owns this allocation from open until close.  A request
 * is outstanding while its number is not the view's finished_serial.
 */
struct kl_backend_network {
	struct kl_daemon_link link;
	struct network_link inner;
	struct network_view published;
	unsigned published_changed;
	struct network_view view;
	uint32_t serial;
	unsigned request;
	unsigned scan_wanted;
};

/*
 * One request handed to the thread: a frame (its kind, opcode and payload,
 * built by the event loop, with the event loop's number), or whether scans
 * are asked for.
 */
struct network_op {
	struct kl_daemon_link_op op;
	unsigned kind;
	unsigned request;
	uint32_t opcode;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	size_t length;
	uint32_t serial;
	unsigned on;
};

static void network_step(void *owner);
static size_t network_poll_fill(void *owner, struct pollfd *descriptors, size_t capacity, int *timeout);
static int network_publish(void *owner);

/* What networkd's connections do on their link's thread. */
static const struct kl_daemon_link_ops network_link_ops = {
	network_step,
	network_poll_fill,
	network_publish,
};

static void network_op_run(void *owner, struct kl_daemon_link_op *op);
static int network_submit(struct kl_backend_network *network, unsigned request, uint32_t opcode, const unsigned char *payload, size_t length);
static void network_finish(struct network_link *network, unsigned request, uint32_t serial, int error);
static int network_connect(int *descriptor);
static int network_text_field(struct networkd_field_writer *writer, uint16_t type, const char *text, size_t room, int required);
static int network_send(struct network_link *network, uint32_t opcode, const unsigned char *payload, size_t length, int *descriptor, uint32_t *request_id);
static void network_scan_step(struct network_link *network, unsigned *changed);
static void network_scan_answer(struct network_link *network, unsigned *changed);
static void network_scan_send(struct network_link *network, uint32_t opcode);
static int network_watch(struct network_link *network);
static void network_unwatch(struct network_link *network, unsigned *changed);
static int network_readable(int descriptor);
static int network_read_watch(struct network_link *network, unsigned *changed);
static int network_read_answer(struct network_link *network, unsigned *changed);
static int network_fields(const unsigned char *payload, size_t length, uint32_t *status, uint32_t *error, const char **output, size_t *output_length);
static void network_parse_state(struct kl_backend_network_state *state, const char *output, size_t length);
static void network_parse_interface(struct kl_backend_network_state *state, const char *line, const char *radio, char *wired, size_t wired_size, unsigned *wifi_online);
static void network_parse_wifi(struct kl_backend_network_state *state, const char *line, char *radio, size_t radio_size);
static void network_parse_route(const char *line, char *route, size_t size);
static unsigned network_wifi_state(const char *name);
static int network_line(const char *output, size_t length, size_t *start, char *line, size_t size);
static void network_parse_scan(struct network_link *network, const char *output, size_t length);
static void network_parse_ap(struct network_link *network, const char *line);
static const char *network_word(const char *line, const char *key, char *field_text, size_t size);
static void network_ssid_text(const char *hex, char *text, size_t size);
static uint64_t network_milliseconds(void);

/*
 * Starts watching the network; the link's thread makes the watch at once,
 * and again a second after each watch that could not be made.
 *
 * A daemon that is not running yet is not a failure: the watch is made later.
 */
struct kl_backend_network *
kl_backend_network_open(
	void)
{
	struct kl_backend_network *network;
	int error;

	/* The watch's record, with no connection. */
	network = calloc(1, sizeof(*network));
	if (network == NULL)
		return NULL;

	/* Starts the connections as unowned and reserves the watch request ID. */
	network->inner.watch = -1;
	network->inner.request_fd = -1;
	network->inner.scan_fd = -1;
	network->inner.next_id = NETWORK_WATCH_ID + 1U;

	/* The thread that makes the watch, sends the requests and reads the answers. */
	error = kl_daemon_link_start(&network->link, "networkd", &network_link_ops, network);
	if (error != 0) {
		free(network);
		return NULL;
	}

	/* Succeeded: the watch exists, connected or not. */
	return network;
}

/*
 * Stops watching and drops an outstanding request.
 */
void
kl_backend_network_close(
	struct kl_backend_network *network)
{
	/* Nothing to close. */
	if (network == NULL)
		return;

	/* The thread ends; its connections are this thread's to close. */
	kl_daemon_link_stop(&network->link);
	if (network->inner.watch >= 0)
		(void)close(network->inner.watch);

	/* Retires any independent request connection before freeing its record. */
	if (network->inner.request_fd >= 0)
		(void)close(network->inner.request_fd);

	/* The scans' connection too (a lease the daemon holds runs out by itself). */
	if (network->inner.scan_fd >= 0)
		(void)close(network->inner.scan_fd);

	/* Releases the watch record after no connection can report to it. */
	free(network);

	/* Succeeded: the connections and the watch record are released. */
	return;
}

/*
 * Takes what the link's thread found since the last update: the state,
 * the scan and the request that finished.
 */
int
kl_backend_network_update(
	struct kl_backend_network *network,
	unsigned *changed)
{
	/* Nothing has changed yet. */
	*changed = 0;

	/* Refuses an operation without its network watch record. */
	if (network == NULL)
		return EINVAL;

	/* Copies the published view and takes its changes. */
	kl_daemon_link_lock(&network->link);

	network->view = network->published;
	*changed = network->published_changed;
	network->published_changed = 0U;

	kl_daemon_link_unlock(&network->link);

	/* Succeeded: *changed says what the thread found. */
	return 0;
}

/*
 * Copies the network's state as last reported.
 */
void
kl_backend_network_get_state(
	const struct kl_backend_network *network,
	struct kl_backend_network_state *state)
{
	/* A missing watch knows nothing. */
	memset(state, 0, sizeof(*state));

	/* Leaves the caller's state empty when no watch exists. */
	if (network == NULL)
		return;

	/* The state as the last update took it. */
	*state = network->view.state;

	/* Succeeded: the caller holds the last reported network state. */
	return;
}

/*
 * Copies up to capacity networks of the last scan, the strongest first, and returns how many there are.
 */
size_t
kl_backend_network_get_scan(
	const struct kl_backend_network *network,
	struct kl_backend_network_ap *aps,
	size_t capacity)
{
	size_t count;

	/* A missing watch has scanned nothing. */
	if (network == NULL)
		return 0;

	/* As many as fit. */
	count = network->view.scan_count;
	if (count > capacity)
		count = capacity;

	/* Copies retained access points when the caller provided room. */
	if (count != 0)
		memcpy(aps, network->view.scan, count * sizeof(*aps));

	/* Succeeded: reports the full scan size, even if the copy was bounded. */
	return network->view.scan_count;
}

/*
 * Sends a request to the daemon; its answer arrives through the updates.
 */
int
kl_backend_network_request(
	struct kl_backend_network *network,
	unsigned request,
	const char *ssid)
{
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	uint32_t opcode;
	size_t length;
	int error;

	/* Refuses an operation without its network watch record. */
	if (network == NULL)
		return EINVAL;

	/* The daemon's operation for the request. */
	switch (request) {
	case KL_BACKEND_NETWORK_REQUEST_SCAN:
		opcode = NETWORKD_OP_WIFI_LIST;
		break;
	case KL_BACKEND_NETWORK_REQUEST_JOIN:
		opcode = NETWORKD_OP_WIFI_CONNECT;
		break;
	case KL_BACKEND_NETWORK_REQUEST_DISCONNECT:
		opcode = NETWORKD_OP_WIFI_DISCONNECT;
		break;
	case KL_BACKEND_NETWORK_REQUEST_WIFI_ON:
		opcode = NETWORKD_OP_WIFI_ENABLE;
		break;
	case KL_BACKEND_NETWORK_REQUEST_WIFI_OFF:
		opcode = NETWORKD_OP_WIFI_DISABLE;
		break;
	case KL_BACKEND_NETWORK_REQUEST_PROFILES:
		opcode = NETWORKD_OP_WIFI_PROFILES_CHANGED;
		break;
	default:
		return EINVAL;
	}

	/* A join names its network, and nothing else does. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	if (request == KL_BACKEND_NETWORK_REQUEST_JOIN) {
		/* An SSID of one to 32 bytes. */
		if (ssid == NULL)
			return EINVAL;

		/* Refuses an SSID outside the daemon's bounded name field. */
		length = strlen(ssid);
		if (length == 0 || length > KL_BACKEND_NETWORK_SSID_MAX - 1U)
			return EINVAL;

		/* The SSID field. */
		error = networkd_field_write(&writer, NETWORKD_FIELD_SSID, ssid, length);
		if (error != 0)
			return EINVAL;
	}

	/* The frame, sent by the thread on a connection of the request's own. */
	error = network_submit(network, request, opcode, payload, writer.used);
	if (error != 0)
		return error;

	/* Succeeded: the request is on its way. */
	return 0;
}

/*
 * Asks the daemon for fresh scans, or no longer (ws089-p021).
 */
int
kl_backend_network_set_scanning(
	struct kl_backend_network *network,
	unsigned on)
{
	struct network_op *asking;

	/* Refuses an operation without its network watch record. */
	if (network == NULL)
		return EINVAL;

	/* Asking already: nothing changes. */
	if (on != 0U && network->scan_wanted != 0U)
		return 0;

	/* Not asking already: nothing changes either. */
	if (on == 0U && network->scan_wanted == 0U)
		return 0;

	/* The change, the link's from the submit on. */
	asking = calloc(1U, sizeof(*asking));
	if (asking == NULL)
		return ENOMEM;
	asking->op.run = network_op_run;
	asking->kind = NETWORK_OP_SCANNING;
	asking->on = on;
	kl_daemon_link_submit(&network->link, &asking->op);

	/* Succeeded: the asking is recorded; the thread tells the daemon. */
	network->scan_wanted = 0U;
	if (on != 0U)
		network->scan_wanted = 1U;
	return 0;
}

/*
 * Sends a wired interface's configuration (ws089-p022) as the request
 * LAN_CONFIGURE: the interface, a static one's address, netmask and router,
 * and the DNS servers named; the daemon checks the values.
 */
int
kl_backend_network_configure_wired(
	struct kl_backend_network *network,
	const struct kl_backend_wired_config *config)
{
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	size_t index;
	int error;

	/* A watch, and a configuration of a mode the daemon knows. */
	if (network == NULL || config == NULL)
		return EINVAL;
	if (config->mode != KL_BACKEND_WIRED_DHCP && config->mode != KL_BACKEND_WIRED_STATIC)
		return EINVAL;

	/* The interface. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	error = network_text_field(&writer, NETWORKD_FIELD_INTERFACE, config->interface, sizeof(config->interface), 1);

	/* A static one's address and netmask, and its router when it has one. */
	if (error == 0 && config->mode == KL_BACKEND_WIRED_STATIC) {
		error = network_text_field(&writer, NETWORKD_FIELD_ADDRESS, config->address, sizeof(config->address), 1);
		if (error == 0)
			error = network_text_field(&writer, NETWORKD_FIELD_NETMASK, config->netmask, sizeof(config->netmask), 1);
		if (error == 0)
			error = network_text_field(&writer, NETWORKD_FIELD_GATEWAY, config->router, sizeof(config->router), 0);
	}

	/* The DNS servers named. */
	for (index = 0; error == 0 && index < 2U; index++)
		error = network_text_field(&writer, NETWORKD_FIELD_DNS, config->dns[index], sizeof(config->dns[index]), 0);
	if (error != 0)
		return error;

	/* The frame, sent by the thread on a connection of the request's own. */
	error = network_submit(network, KL_BACKEND_NETWORK_REQUEST_WIRED, NETWORKD_OP_LAN_CONFIGURE, payload, writer.used);
	if (error != 0)
		return error;

	/* Succeeded: the request is on its way. */
	return 0;
}

/*
 * Tells the request outstanding, or the one that finished last and its errno value.
 */
unsigned
kl_backend_network_get_request(
	const struct kl_backend_network *network,
	int *error)
{
	/* No watch, no request. */
	*error = 0;
	if (network == NULL)
		return KL_BACKEND_NETWORK_REQUEST_NONE;

	/* The outstanding one: the thread has not finished the event loop's last request yet. */
	if (network->serial != network->view.finished_serial)
		return network->request;

	/* Succeeded: reports the last request and its stored outcome. */
	*error = network->view.finished_error;
	return network->view.finished;
}

/*
 * Writes a text field of a configuration: one ended within its room, not
 * empty when it is required (an empty one that is not is left out).
 * Returns 0 or EINVAL.
 */
static int
network_text_field(
	struct networkd_field_writer *writer,
	uint16_t type,
	const char *text,
	size_t room,
	int required)
{
	const char *end;
	size_t length;
	int error;

	/* Ended within its room. */
	end = memchr(text, '\0', room);
	if (end == NULL)
		return EINVAL;
	length = (size_t)(end - text);

	/* An empty one: refused when required, else left out. */
	if (length == 0U) {
		if (required)
			return EINVAL;
		return 0;
	}

	/* The field. */
	error = networkd_field_write(writer, type, text, length);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the field is written. */
	return 0;
}

/* Steps the watch on the link's thread: makes it again when its wait is over, reads the watch and the answers, and moves the scans. */
static void
network_step(
	void *owner)
{
	struct kl_backend_network *record;
	struct network_link *network;
	uint64_t now;
	int error;

	/* The thread's own part of the record. */
	record = owner;
	network = &record->inner;

	/* A lost watch is made again when its wait is over. */
	if (network->watch < 0) {
		/* Retries only when the previous watch attempt's delay expires. */
		now = network_milliseconds();
		if (now >= network->retry_ms) {
			error = network_watch(network);
			if (error == 0)
				network->changed |= KL_BACKEND_NETWORK_CHANGED_STATE;
		}
	}

	/* The states the watch has reported. */
	if (network->watch >= 0)
		(void)network_read_watch(network, &network->changed);

	/* The answer to the request outstanding. */
	if (network->request_fd >= 0)
		(void)network_read_answer(network, &network->changed);

	/* The scans asked for, beside the request (ws089-p021). */
	network_scan_step(network, &network->changed);
}

/*
 * Gives the connections to wait for -- the watch, the request outstanding
 * and the scans' frame outstanding, each of which wakes the thread when its
 * daemon writes -- and the wait until what is due next: the watch's next
 * try, the scans' next frame, or the scans' frame given up.
 */
static size_t
network_poll_fill(
	void *owner,
	struct pollfd *descriptors,
	size_t capacity,
	int *timeout)
{
	struct kl_backend_network *record;
	struct network_link *network;
	uint64_t due;
	uint64_t now;
	size_t count;

	/* The thread's own part of the record. */
	record = owner;
	network = &record->inner;
	now = network_milliseconds();

	/* The next try of a lost watch, and the scans' next frame while they are asked for. */
	due = UINT64_MAX;
	if (network->watch < 0)
		due = network->retry_ms;
	if (network->scan_wanted != 0U && network->scan_fd < 0) {
		if (network->scan_lease_ms < due)
			due = network->scan_lease_ms;
		if (network->scan_list_ms < due)
			due = network->scan_list_ms;
	}

	/* A frame of the scans that the daemon has not answered is given up when its time is over. */
	if (network->scan_fd >= 0 && network->scan_given_up_ms < due)
		due = network->scan_given_up_ms;

	/* The scans' stop is told at the next step. */
	if (network->scan_wanted == 0U && network->scan_leased != 0U && network->scan_fd < 0)
		due = now;

	/* Lowers the wait to what is due. */
	if (due <= now) {
		*timeout = 0;
	} else if (due - now < (uint64_t)*timeout) {
		*timeout = (int)(due - now);
	}

	/* The watch wakes the thread with each state. */
	count = 0U;
	if (network->watch >= 0 && count < capacity) {
		descriptors[count].fd = network->watch;
		descriptors[count].events = POLLIN;
		descriptors[count].revents = 0;
		count++;
	}

	/*
	 * The request's connection wakes the thread when its answer arrives.
	 * This side's shutdown for writing does not wake it: a poll reports
	 * only the answer, the daemon's end or an error on it.
	 */
	if (network->request_fd >= 0 && count < capacity) {
		descriptors[count].fd = network->request_fd;
		descriptors[count].events = POLLIN;
		descriptors[count].revents = 0;
		count++;
	}

	/* The scans' connection wakes the thread when its frame's answer arrives. */
	if (network->scan_fd >= 0 && count < capacity) {
		descriptors[count].fd = network->scan_fd;
		descriptors[count].events = POLLIN;
		descriptors[count].revents = 0;
		count++;
	}

	/* Succeeded: the descriptors to wait for. */
	return count;
}

/* Publishes the state, the scan and the request finished to the event loop's side (under the link's lock); 1 when something changed. */
static int
network_publish(
	void *owner)
{
	struct kl_backend_network *record;
	struct network_link *network;

	/* The thread's own part of the record. */
	record = owner;
	network = &record->inner;

	/* Nothing new: no wake. */
	if (network->changed == 0U)
		return 0;

	/* The view and the changes not taken yet. */
	record->published.state = network->state;
	memcpy(record->published.scan, network->scan, network->scan_count * sizeof(network->scan[0]));
	record->published.scan_count = network->scan_count;
	record->published.finished = network->finished;
	record->published.finished_error = network->finished_error;
	record->published.finished_serial = network->finished_serial;
	record->published_changed |= network->changed;
	network->changed = 0U;

	/* Succeeded: the event loop has something to take. */
	return 1;
}

/* Runs a request on the link's thread: sends a frame on a connection of its own, or changes whether scans are asked for. */
static void
network_op_run(
	void *owner,
	struct kl_daemon_link_op *op)
{
	struct kl_backend_network *record;
	struct network_link *network;
	struct network_op *request;
	uint32_t request_id;
	uint64_t now;
	int descriptor;
	int error;

	/* The thread's own part of the record, and the request. */
	record = owner;
	network = &record->inner;
	request = (struct network_op *)op;

	/* The asking for scans begins: the daemon is asked now and the scan it has is read at once. */
	if (request->kind == NETWORK_OP_SCANNING && request->on != 0U) {
		now = network_milliseconds();
		network->scan_wanted = 1U;
		network->scan_lease_ms = now;
		network->scan_list_ms = now;
		return;
	}

	/* The asking ends: the daemon is told on the next step. */
	if (request->kind == NETWORK_OP_SCANNING) {
		network->scan_wanted = 0U;
		return;
	}

	/* One request at a time (the event loop waits for each one's end). */
	if (network->request_fd >= 0) {
		network_finish(network, request->request, request->serial, EBUSY);
		return;
	}

	/* The frame, on a connection of the request's own; one that cannot be sent finishes at once. */
	error = network_send(network, request->opcode, request->payload, request->length, &descriptor, &request_id);
	if (error != 0) {
		network_finish(network, request->request, request->serial, error);
		return;
	}

	/* The request is outstanding until its answer is read. */
	network->request_fd = descriptor;
	network->request = request->request;
	network->request_opcode = request->opcode;
	network->request_id = request_id;
	network->request_serial = request->serial;
}

/* Numbers a frame and hands it to the link's thread; 0, EBUSY (one outstanding) or ENOMEM. */
static int
network_submit(
	struct kl_backend_network *network,
	unsigned request,
	uint32_t opcode,
	const unsigned char *payload,
	size_t length)
{
	struct network_op *frame;

	/* Keeps one request outstanding so its reply has an unambiguous owner. */
	if (network->serial != network->view.finished_serial)
		return EBUSY;

	/* The request, the link's from the submit on. */
	frame = calloc(1U, sizeof(*frame));
	if (frame == NULL)
		return ENOMEM;

	/* The event loop's number of it (never 0, the number of none finished). */
	network->serial++;
	if (network->serial == 0U)
		network->serial = 1U;
	frame->op.run = network_op_run;
	frame->kind = NETWORK_OP_REQUEST;
	frame->request = request;
	frame->opcode = opcode;
	memcpy(frame->payload, payload, length);
	frame->length = length;
	frame->serial = network->serial;
	kl_daemon_link_submit(&network->link, &frame->op);

	/* Succeeded: outstanding until the thread publishes its end. */
	network->request = request;
	return 0;
}

/* Finishes a request that could not be sent, with its errno value, as the next update tells. */
static void
network_finish(
	struct network_link *network,
	unsigned request,
	uint32_t serial,
	int error)
{
	/* The outcome, kept as a request's whose answer came. */
	network->finished = request;
	network->finished_error = error;
	network->finished_serial = serial;
	network->changed |= KL_BACKEND_NETWORK_CHANGED_DONE;
}

/* Connects to the daemon's socket. */
static int
network_connect(
	int *descriptor)
{
	struct sockaddr_un address;
	int connection;
	int error;

	/* A stream socket that the programs the desktop starts do not inherit. */
	connection = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (connection < 0)
		return errno;

	/* The daemon's socket; one that is not there is a daemon not running. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", NETWORKD_SOCKET);
	error = connect(connection, (struct sockaddr *)&address, sizeof(address));
	if (error != 0) {
		error = errno;
		(void)close(connection);

		/* Maps an absent listening daemon to the missing-service convention. */
		if (error == ECONNREFUSED)
			return ENOENT;

		/* Reports the original connection failure after closing its descriptor. */
		return error;
	}

	/* Succeeded: the connection is the caller's. */
	*descriptor = connection;
	return 0;
}

/*
 * Sends one frame on a new connection and ends what this side sends (the
 * daemon answers after it); the caller owns *descriptor and reads the answer
 * with *request_id.
 */
static int
network_send(
	struct network_link *network,
	uint32_t opcode,
	const unsigned char *payload,
	size_t length,
	int *descriptor,
	uint32_t *request_id)
{
	struct networkd_protocol_header header;
	int connection;
	int error;

	/* A connection of the frame's own. */
	error = network_connect(&connection);
	if (error != 0)
		return error;

	/* The frame. */
	header.request_id = network->next_id;
	header.opcode = opcode;
	header.payload_length = (uint32_t)length;
	error = networkd_protocol_write_frame(connection, &header, payload);
	if (error != 0) {
		error = errno;
		(void)close(connection);
		return error;
	}

	/* This side has said all it will. */
	(void)shutdown(connection, SHUT_WR);

	/* The next frame gets the next ID (never the watch's, never 0). */
	network->next_id++;
	if (network->next_id <= NETWORK_WATCH_ID)
		network->next_id = NETWORK_WATCH_ID + 1U;

	/* Succeeded: the frame is on its way. */
	*descriptor = connection;
	*request_id = header.request_id;
	return 0;
}

/*
 * Moves the scans' connection one step: takes the answer of its frame when
 * it came, then sends the next frame that is due -- a stop once the asking
 * ended, a renewal of the lease, or a reading of the scan.
 */
static void
network_scan_step(
	struct network_link *network,
	unsigned *changed)
{
	uint64_t now;

	/* The answer of the frame outstanding, or a daemon that took too long. */
	now = network_milliseconds();
	if (network->scan_fd >= 0) {
		network_scan_answer(network, changed);
		if (network->scan_fd >= 0 && now >= network->scan_given_up_ms) {
			(void)close(network->scan_fd);
			network->scan_fd = -1;
		}
	}

	/* One frame at a time on this connection. */
	if (network->scan_fd >= 0)
		return;

	/* The asking ended while the daemon still holds a lease: it is told. */
	if (network->scan_wanted == 0U) {
		if (network->scan_leased != 0U)
			network_scan_send(network, NETWORKD_OP_WIFI_SCAN_STOP);
		return;
	}

	/* The lease, asked for or renewed before it runs out. */
	if (now >= network->scan_lease_ms) {
		network->scan_lease_ms = now + NETWORK_SCAN_LEASE_MS;
		network_scan_send(network, NETWORKD_OP_WIFI_SCAN_START);
		return;
	}

	/* The scan, read again when its time came. */
	if (now >= network->scan_list_ms) {
		network->scan_list_ms = now + NETWORK_SCAN_LIST_MS;
		network_scan_send(network, NETWORKD_OP_WIFI_LIST);
	}
}

/* Takes the answer of the scans' connection when it has arrived: a reading's networks are the new scan. */
static void
network_scan_answer(
	struct network_link *network,
	unsigned *changed)
{
	struct networkd_protocol_header header;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	const char *output;
	size_t output_length;
	uint32_t status;
	uint32_t error;
	int readable;
	int failed;

	/* Not yet. */
	readable = network_readable(network->scan_fd);
	if (!readable)
		return;

	/* The answer, whole; the frame is over either way. */
	failed = networkd_protocol_read_frame_timed(network->scan_fd, &header, payload, sizeof(payload), NETWORKD_RESPONSE_MAX, NETWORK_FRAME_SECONDS);
	(void)close(network->scan_fd);
	network->scan_fd = -1;
	if (failed != 0)
		return;

	/* An answer to another frame, or one that does not read, is passed over. */
	failed = network_fields(payload, header.payload_length, &status, &error, &output, &output_length);
	if (failed != 0)
		return;
	if (header.request_id != network->scan_id || header.opcode != network->scan_opcode)
		return;

	/* Only a reading carries networks; one done in part still has them. */
	if (header.opcode != NETWORKD_OP_WIFI_LIST)
		return;
	if (status != NETWORKD_RESULT_OK && status != NETWORKD_RESULT_DEGRADED)
		return;

	/* The new scan. */
	network_parse_scan(network, output, output_length);
	*changed |= KL_BACKEND_NETWORK_CHANGED_SCAN;
}

/* Sends one frame on the scans' connection; a daemon not there is asked again at the next step that is due. */
static void
network_scan_send(
	struct network_link *network,
	uint32_t opcode)
{
	uint32_t request_id;
	int descriptor;
	int error;

	/* The frame, which carries no field. */
	error = network_send(network, opcode, NULL, 0U, &descriptor, &request_id);
	if (error != 0)
		return;

	/*
	 * scan_leased follows what the daemon was last asked: a start leaves it
	 * holding a lease, a stop ends it.
	 */
	if (opcode == NETWORKD_OP_WIFI_SCAN_START)
		network->scan_leased = 1U;
	if (opcode == NETWORKD_OP_WIFI_SCAN_STOP)
		network->scan_leased = 0U;

	/* Outstanding until its answer, or until it is given up. */
	network->scan_fd = descriptor;
	network->scan_opcode = opcode;
	network->scan_id = request_id;
	network->scan_given_up_ms = network_milliseconds() + NETWORK_SCAN_ANSWER_MS;
}

/* Makes the watch: a connection that asked SUBSCRIBE. */
static int
network_watch(
	struct network_link *network)
{
	struct networkd_protocol_header header;
	int descriptor;
	int error;

	/* Whatever happens, the next try waits a while. */
	network->retry_ms = network_milliseconds() + NETWORK_RETRY_MS;

	/* The connection. */
	error = network_connect(&descriptor);
	if (error != 0)
		return error;

	/* SUBSCRIBE; the daemon answers with the state and keeps the connection. */
	header.request_id = NETWORK_WATCH_ID;
	header.opcode = NETWORKD_OP_SUBSCRIBE;
	header.payload_length = 0;
	error = networkd_protocol_write_frame(descriptor, &header, NULL);
	if (error != 0) {
		error = errno;
		(void)close(descriptor);
		return error;
	}

	/* Succeeded: the watch waits for its first state. */
	network->watch = descriptor;
	return 0;
}

/* Drops the watch, which leaves the state unknown until it is made again. */
static void
network_unwatch(
	struct network_link *network,
	unsigned *changed)
{
	/* The connection. */
	(void)close(network->watch);
	network->watch = -1;
	network->retry_ms = network_milliseconds() + NETWORK_RETRY_MS;

	/* The daemon can no longer be reached, which the caller shows. */
	memset(&network->state, 0, sizeof(network->state));
	*changed |= KL_BACKEND_NETWORK_CHANGED_STATE;

	/* Succeeded: the watch is retired and unreachable state is published. */
	return;
}

/* Tells whether a connection has something to read (or has ended) now. */
static int
network_readable(
	int descriptor)
{
	struct pollfd poll_descriptor;
	int ready;

	/*
	 * A look without waiting.  A request's connection shut down for writing
	 * is not readable until the answer, the daemon's end or an error.
	 */
	poll_descriptor.fd = descriptor;
	poll_descriptor.events = POLLIN;
	poll_descriptor.revents = 0;
	ready = poll(&poll_descriptor, 1, 0);
	if (ready <= 0)
		return 0;

	/* Succeeded: a byte, the end, or a failure the read will report is there. */
	return 1;
}

/* Reads the states the watch has reported, the last one standing. */
static int
network_read_watch(
	struct network_link *network,
	unsigned *changed)
{
	struct networkd_protocol_header header;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	const char *output;
	size_t output_length;
	uint32_t status;
	uint32_t error;
	int readable;
	int failed;

	/* Each frame that has arrived. */
	for (;;) {
		/* Nothing more has arrived. */
		readable = network_readable(network->watch);
		if (!readable)
			break;

		/* A frame, whole; a watch that ended or broke is dropped. */
		failed = networkd_protocol_read_frame_timed(network->watch, &header, payload, sizeof(payload), NETWORKD_RESPONSE_MAX, NETWORK_FRAME_SECONDS);
		if (failed != 0) {
			network_unwatch(network, changed);
			return EIO;
		}

		/* A frame that is not the watch's, or does not read, ends the watch. */
		failed = network_fields(payload, header.payload_length, &status, &error, &output, &output_length);
		if (failed != 0 || header.opcode != NETWORKD_OP_SUBSCRIBE) {
			network_unwatch(network, changed);
			return EPROTO;
		}

		/* A refusal (no right to look, too many watchers) ends the watch; it is tried again later. */
		if (status != NETWORKD_RESULT_OK) {
			network_unwatch(network, changed);
			return (int)error;
		}

		/* The state. */
		network_parse_state(&network->state, output, output_length);
		*changed |= KL_BACKEND_NETWORK_CHANGED_STATE;
	}

	/* Succeeded: all currently readable state frames have been applied. */
	return 0;
}

/* Reads the answer to the request outstanding, when it has arrived. */
static int
network_read_answer(
	struct network_link *network,
	unsigned *changed)
{
	struct networkd_protocol_header header;
	unsigned char payload[NETWORKD_RESPONSE_MAX];
	const char *output;
	size_t output_length;
	uint32_t status;
	uint32_t error;
	int readable;
	int failed;

	/* Not yet. */
	readable = network_readable(network->request_fd);
	if (!readable)
		return 0;

	/* The answer, whole; the request is over either way. */
	failed = networkd_protocol_read_frame_timed(network->request_fd, &header, payload, sizeof(payload), NETWORKD_RESPONSE_MAX, NETWORK_FRAME_SECONDS);
	(void)close(network->request_fd);
	network->request_fd = -1;
	network->finished = network->request;
	network->finished_serial = network->request_serial;
	network->request = KL_BACKEND_NETWORK_REQUEST_NONE;
	*changed |= KL_BACKEND_NETWORK_CHANGED_DONE;
	if (failed != 0) {
		network->finished_error = EIO;
		return EIO;
	}

	/* An answer to another request, or one that does not read. */
	failed = network_fields(payload, header.payload_length, &status, &error, &output, &output_length);
	if (failed != 0 ||
	    header.request_id != network->request_id ||
	    header.opcode != network->request_opcode) {
		network->finished_error = EPROTO;
		return EPROTO;
	}

	/* A scan's networks, even from a scan that was only partly done. */
	if (network->finished == KL_BACKEND_NETWORK_REQUEST_SCAN) {
		network_parse_scan(network, output, output_length);
		*changed |= KL_BACKEND_NETWORK_CHANGED_SCAN;
	}

	/* A request done in part is done for a scan; otherwise only OK is success. */
	network->finished_error = 0;
	if (status == NETWORKD_RESULT_DEGRADED && network->finished == KL_BACKEND_NETWORK_REQUEST_SCAN)
		return 0;

	/* Retains a refused daemon request as the visible completed outcome. */
	if (status != NETWORKD_RESULT_OK) {
		network->finished_error = EIO;

		/* Keeps the daemon's specific errno when it supplied one. */
		if (error != 0)
			network->finished_error = (int)error;

		/* Reports the refusal retained for the request observer. */
		return network->finished_error;
	}

	/* Succeeded: the request was done. */
	return 0;
}

/* Reads a frame's fields: its status and errno value, and its text (possibly none). */
static int
network_fields(
	const unsigned char *payload,
	size_t length,
	uint32_t *status,
	uint32_t *error,
	const char **output,
	size_t *output_length)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	unsigned seen;
	int failed;

	/* Nothing read yet. */
	*status = NETWORKD_RESULT_ERROR;
	*error = EIO;
	*output = "";
	*output_length = 0;
	seen = 0;

	/* Each field; the ones this side does not use are passed over. */
	networkd_field_reader_init(&reader, payload, length);
	for (;;) {
		/* Stops the field traversal when no complete field remains. */
		failed = networkd_field_read(&reader, &field);
		if (failed != 0)
			break;

		/* The outcome. */
		if (field.type == NETWORKD_FIELD_STATUS) {
			/* Accepts this mandatory outcome field only when its scalar decodes. */
			failed = networkd_field_read_u32(&field, status);
			if (failed == 0)
				seen |= 1U;
			continue;
		}

		/* The reason for a refusal. */
		if (field.type == NETWORKD_FIELD_ERROR) {
			/* Accepts this mandatory outcome field only when its scalar decodes. */
			failed = networkd_field_read_u32(&field, error);
			if (failed == 0)
				seen |= 2U;
			continue;
		}

		/* The text. */
		if (field.type == NETWORKD_FIELD_OUTPUT) {
			*output = (const char *)field.value;
			*output_length = field.length;
		}
	}

	/* A frame needs its outcome and its reason. */
	if ((seen & 3U) != 3U)
		return EPROTO;

	/* Succeeded: the fields are read. */
	return 0;
}

/* Reads the interfaces and Wi-Fi state from the daemon's text. */
static void
network_parse_state(
	struct kl_backend_network_state *state,
	const char *output,
	size_t length)
{
	char line[NETWORK_LINE_MAX];
	char wired[KL_BACKEND_NETWORK_NAME_MAX];
	char route[KL_BACKEND_NETWORK_NAME_MAX];
	char radio[KL_BACKEND_NETWORK_NAME_MAX];
	unsigned wifi_online;
	size_t start;
	int more;
	int differs;
	int routed;

	/*
	 * Reads a state: the interfaces' lines, the Wi-Fi's and the default
	 * route's.  The connection is the one the default route goes through
	 * (BUG-189): the Wi-Fi's when the route goes through the radio and the
	 * Wi-Fi is connected and online, else the other interface it goes
	 * through.  A daemon that names no route has the connection chosen as
	 * networkd chooses the route (ws005-p019 B3): the first other interface
	 * online with an address, else a connected Wi-Fi.
	 */

	/* A reachable daemon with nothing known yet. */
	memset(state, 0, sizeof(*state));
	state->reachable = 1;

	/* Starts interface selection without a wired or online radio candidate, or a route. */
	wired[0] = '\0';
	route[0] = '\0';
	radio[0] = '\0';
	wifi_online = 0;

	/* The Wi-Fi's line and the route's first, so that the interfaces' lines know which one is the radio. */
	start = 0;
	for (;;) {
		/* Stops when the bounded daemon output contains no further line. */
		more = network_line(output, length, &start, line, sizeof(line));
		if (!more)
			break;

		/* The Wi-Fi's line. */
		differs = strncmp(line, "wifi ", 5);
		if (differs == 0)
			network_parse_wifi(state, line, radio, sizeof(radio));

		/* The default route's line. */
		differs = strncmp(line, "route ", 6);
		if (differs == 0)
			network_parse_route(line, route, sizeof(route));
	}

	/* Then each interface's line (not the Wi-Fi's, not an empty one). */
	start = 0;
	for (;;) {
		/* Stops when the bounded daemon output contains no further line. */
		more = network_line(output, length, &start, line, sizeof(line));
		if (!more)
			break;

		/* The Wi-Fi's line and an empty one are no interface. */
		differs = strncmp(line, "wifi ", 5);
		if (differs == 0 || line[0] == '\0')
			continue;

		/* Nor is the route's. */
		differs = strncmp(line, "route ", 6);
		if (differs == 0)
			continue;

		/* An interface's line. */
		network_parse_interface(state, line, radio, wired, sizeof(wired), &wifi_online);
	}

	/* The wired interface, whichever carries the connection. */
	(void)snprintf(state->wired, sizeof(state->wired), "%s", wired);

	/* The default route through the radio (the connection's, or the WLAN interface itself, BUG-169): a connected Wi-Fi carries the connection. */
	routed = 0;
	if (route[0] != '\0' && state->wifi_interface[0] != '\0') {
		differs = strcmp(route, state->wifi_interface);
		if (differs == 0)
			routed = 1;
	}

	/* The WLAN interface itself. */
	if (route[0] != '\0' && radio[0] != '\0') {
		differs = strcmp(route, radio);
		if (differs == 0)
			routed = 1;
	}

	/* Then a connected Wi-Fi that is up carries it. */
	if (routed &&
	    state->wifi == KL_BACKEND_WIFI_CONNECTED &&
	    wifi_online) {
		state->connected = 1;
		state->kind = KL_BACKEND_NETWORK_WIFI;
		(void)snprintf(state->interface, sizeof(state->interface), "%s", state->wifi_interface);
		return;
	}

	/* The default route through another interface: that wired interface carries it. */
	if (route[0] != '\0' && !routed) {
		state->connected = 1;
		state->kind = KL_BACKEND_NETWORK_WIRED;
		(void)snprintf(state->interface, sizeof(state->interface), "%s", route);
		return;
	}

	/* Without a route to go by, a wired interface that is up with an address first. */
	if (wired[0] != '\0') {
		state->connected = 1;
		state->kind = KL_BACKEND_NETWORK_WIRED;
		(void)snprintf(state->interface, sizeof(state->interface), "%s", wired);
		return;
	}

	/* Then a connected Wi-Fi whose radio is online. */
	if (state->wifi == KL_BACKEND_WIFI_CONNECTED && wifi_online) {
		state->connected = 1;
		state->kind = KL_BACKEND_NETWORK_WIFI;
		(void)snprintf(state->interface, sizeof(state->interface), "%s", state->wifi_interface);
	}

	/* Succeeded: the reachable connection state is classified. */
	return;
}

/* Copies the next bounded line and advances the text cursor. */
static int
network_line(
	const char *output,
	size_t length,
	size_t *start,
	char *line,
	size_t size)
{
	size_t end;
	size_t copied;

	/*
	 * Copies the next line of a text from *start into line (cut to its size)
	 * and moves *start past it.  Returns 0 when the text is used up.
	 */

	/* The text is used up. */
	if (*start >= length)
		return 0;

	/* The line runs to its newline or to the end. */
	end = *start;
	while (end < length && output[end] != '\n')
		end++;

	/* Its text, as much as fits. */
	copied = end - *start;
	if (copied >= size)
		copied = size - 1U;

	/* Publishes the bounded line and advances past its original newline. */
	memcpy(line, output + *start, copied);
	line[copied] = '\0';
	*start = end + 1U;

	/* Succeeded: the next line is copied and the cursor advances. */
	return 1;
}

/* Selects an online radio or the first configured wired interface. */
static void
network_parse_interface(
	struct kl_backend_network_state *state,
	const char *line,
	const char *radio,
	char *wired,
	size_t wired_size,
	unsigned *wifi_online)
{
	char name[KL_BACKEND_NETWORK_NAME_MAX];
	char address[32];
	char link[32];
	int count;
	int differs;

	/*
	 * Reads an interface's line, "NAME static|link-local|unconfigured online|offline":
	 * the radio's being online, or the first other interface (not the
	 * loopback) online with an address.
	 */

	/* The three words. */
	count = sscanf(line, "%15s %31s %31s", name, address, link);
	if (count != 3)
		return;

	/* The loopback is no connection. */
	differs = strncmp(name, "lo", 2);
	if (differs == 0)
		return;

	/* An interface that is not up is no connection either. */
	differs = strcmp(link, "online");
	if (differs != 0)
		return;

	/* The radio's interface: its being up is what a connected Wi-Fi needs. */
	differs = strcmp(name, state->wifi_interface);
	if (state->wifi_interface[0] != '\0' && differs == 0) {
		*wifi_online = 1;
		return;
	}

	/* The WLAN interface while no connection names it is no wired connection either (BUG-169). */
	differs = strcmp(name, radio);
	if (radio[0] != '\0' && differs == 0)
		return;

	/* The first other interface with an address carries a wired connection. */
	differs = strcmp(address, "static");
	if (differs == 0 && wired[0] == '\0')
		(void)snprintf(wired, wired_size, "%s", name);

	/* Succeeded: the interface is classified without replacing an earlier wired choice. */
	return;
}

/*
 * Reads the Wi-Fi's line, "wifi state=NAME interface=IF ssid=HEX radios=N
 * scan=N radio=IF"; radio (the WLAN interface even while no connection uses
 * it, BUG-169) is copied to the caller's radio, when given.
 */
static void
network_parse_wifi(
	struct kl_backend_network_state *state,
	const char *line,
	char *radio,
	size_t radio_size)
{
	char field_text[80];
	const char *found;
	int differs;

	/* The WLAN interface, whatever the connection does ("-" for none). */
	found = network_word(line, "radio", field_text, sizeof(field_text));
	differs = strcmp(field_text, "-");
	if (found != NULL && differs != 0)
		(void)snprintf(radio, radio_size, "%s", field_text);

	/* No radio at all. */
	found = network_word(line, "radios", field_text, sizeof(field_text));
	differs = strcmp(field_text, "0");
	if (found != NULL && differs == 0) {
		state->wifi = KL_BACKEND_WIFI_ABSENT;
		return;
	}

	/* The radio the managed connection uses ("-" for none). */
	found = network_word(line, "interface", field_text, sizeof(field_text));
	differs = strcmp(field_text, "-");
	if (found != NULL && differs != 0)
		(void)snprintf(state->wifi_interface, sizeof(state->wifi_interface), "%s", field_text);

	/* The network it is on or joining. */
	found = network_word(line, "ssid", field_text, sizeof(field_text));
	differs = strcmp(field_text, "-");
	if (found != NULL && differs != 0)
		network_ssid_text(field_text, state->ssid, sizeof(state->ssid));

	/* The daemon's state, in the desktop's terms (the ones not named are left unconnected). */
	found = network_word(line, "state", field_text, sizeof(field_text));
	if (found == NULL)
		field_text[0] = '\0';

	/* Classifies the daemon state, including an absent or unknown name. */
	state->wifi = network_wifi_state(field_text);

	/* Succeeded: the radio identity and state are decoded. */
	return;
}

/* Reads the default route's line, "route default=IF" ("-" for none): route gets the interface, or stays empty. */
static void
network_parse_route(
	const char *line,
	char *route,
	size_t size)
{
	char field_text[80];
	const char *found;
	int differs;

	/* The interface named, unless the route is none. */
	found = network_word(line, "default", field_text, sizeof(field_text));
	if (found == NULL)
		return;
	differs = strcmp(field_text, "-");
	if (differs == 0)
		return;

	/* The interface, as much as fits. */
	(void)snprintf(route, size, "%s", field_text);
}

/* Tells the desktop's Wi-Fi state for networkd's name of it. */
static unsigned
network_wifi_state(
	const char *name)
{
	int differs;

	/* Off. */
	differs = strcmp(name, "disabled");
	if (differs == 0)
		return KL_BACKEND_WIFI_OFF;

	/* Looking for a network with a profile. */
	differs = strcmp(name, "auto-searching");
	if (differs == 0)
		return KL_BACKEND_WIFI_SEARCHING;

	/* Joining, for the first time or again. */
	differs = strcmp(name, "connecting");
	if (differs == 0)
		return KL_BACKEND_WIFI_CONNECTING;

	/* Treats a reconnect as the same visible joining state. */
	differs = strcmp(name, "reconnecting");
	if (differs == 0)
		return KL_BACKEND_WIFI_CONNECTING;

	/* On a network. */
	differs = strcmp(name, "connected");
	if (differs == 0)
		return KL_BACKEND_WIFI_CONNECTED;

	/* Succeeded: other state names describe an unconnected radio. */
	return KL_BACKEND_WIFI_DISCONNECTED;
}

/* Reads and sorts the strongest access point of each scanned SSID. */
static void
network_parse_scan(
	struct network_link *network,
	const char *output,
	size_t length)
{
	struct kl_backend_network_ap moved;
	char line[NETWORK_LINE_MAX];
	const char *found;
	size_t start;
	size_t index;
	size_t place;
	int more;

	/*
	 * Reads a scan's networks: its lines
	 * "interface=IF bss index=N ssid=HEX bssid=HEX ... rssi=N ... security=HEX ...",
	 * one network an SSID (the strongest access point), the strongest first.
	 */

	/* A new scan replaces the last. */
	network->scan_count = 0;

	/* Each line that names an access point. */
	start = 0;
	for (;;) {
		/* Stops when the bounded daemon output contains no further line. */
		more = network_line(output, length, &start, line, sizeof(line));
		if (!more)
			break;

		/* An access point's line. */
		found = strstr(line, " bss ");
		if (found != NULL)
			network_parse_ap(network, line);
	}

	/* The strongest first (an insertion sort of a few entries). */
	for (index = 1; index < network->scan_count; index++) {
		/* Inserts this access point after every stronger retained one. */
		moved = network->scan[index];
		place = index;
		while (place > 0 && network->scan[place - 1].rssi < moved.rssi) {
			/* Shifts a weaker access point to reserve this signal-ranked position. */
			network->scan[place] = network->scan[place - 1];
			place--;
		}

		/* The entry goes where the stronger ones end. */
		network->scan[place] = moved;
	}

	/* Succeeded: the scan keeps the strongest access point per SSID in signal order. */
	return;
}

/* Reads one access point's line into the scan (the stronger of two of one SSID stays). */
static void
network_parse_ap(
	struct network_link *network,
	const char *line)
{
	struct kl_backend_network_ap ap;
	char field_text[80];
	const char *found;
	unsigned long security;
	size_t index;
	int differs;

	/* The SSID; a hidden network (no SSID) is not listed. */
	memset(&ap, 0, sizeof(ap));

	/* Skips a hidden access point without a public SSID. */
	found = network_word(line, "ssid", field_text, sizeof(field_text));
	if (found == NULL || field_text[0] == '\0')
		return;

	/* Rejects a hexadecimal SSID that decodes to an empty public name. */
	network_ssid_text(field_text, ap.ssid, sizeof(ap.ssid));
	if (ap.ssid[0] == '\0')
		return;

	/* The signal. */
	ap.rssi = -100;
	found = network_word(line, "rssi", field_text, sizeof(field_text));
	if (found != NULL)
		ap.rssi = atoi(field_text);

	/* Any security bit asks for a key. */
	found = network_word(line, "security", field_text, sizeof(field_text));
	if (found != NULL) {
		/* Marks any advertised security mode as requiring a saved key. */
		security = strtoul(field_text, NULL, 16);
		if (security != 0)
			ap.secured = 1;
	}

	/* An SSID already listed keeps its stronger access point. */
	for (index = 0; index < network->scan_count; index++) {
		/* Skips access points naming a different retained network. */
		differs = strcmp(network->scan[index].ssid, ap.ssid);
		if (differs != 0)
			continue;

		/* The stronger one stays. */
		if (ap.rssi > network->scan[index].rssi)
			network->scan[index] = ap;

		/* Leaves this SSID represented by exactly one strongest access point. */
		return;
	}

	/* A new one, while there is room. */
	if (network->scan_count < KL_BACKEND_NETWORK_SCAN_MAX) {
		network->scan[network->scan_count] = ap;
		network->scan_count++;
	}

	/* Succeeded: the access point is retained when room remains. */
	return;
}

/* Copies a named whole-word field and reports its position. */
static const char *
network_word(
	const char *line,
	const char *key,
	char *field_text,
	size_t size)
{
	const char *at;
	size_t key_length;
	size_t length;

	/*
	 * Finds " KEY=VALUE" (or "KEY=VALUE" at the start) in a line and copies the
	 * value; returns where it was found, or NULL.
	 */

	/* Nothing found yet. */
	field_text[0] = '\0';

	/* Each place the key appears, as a whole word followed by '='. */
	key_length = strlen(key);
	for (at = strstr(line, key);
	     at != NULL;
	     at = strstr(at + 1, key)) {
		/* Part of a longer word. */
		if (at != line && at[-1] != ' ')
			continue;

		/* Requires the complete key to be followed by its value separator. */
		if (at[key_length] != '=')
			continue;

		/* The value, up to the next space. */
		at += key_length + 1U;
		length = strcspn(at, " ");
		if (length >= size)
			length = size - 1U;

		/* Copies and terminates the bounded field before returning its position. */
		memcpy(field_text, at, length);
		field_text[length] = '\0';
		break;
	}

	/* Refuses a field not present as a complete key. */
	if (at == NULL)
		return NULL;

	/* Succeeded: the field text is copied and its position is retained. */
	return at;
}

/* Turns a hexadecimal SSID into text a menu can show (a control byte shows as '?'). */
static void
network_ssid_text(
	const char *hex,
	char *text,
	size_t size)
{
	unsigned decoded_byte;
	size_t used;
	int count;

	/* Each pair of digits is one byte. */
	used = 0;
	while (hex[0] != '\0' &&
	       hex[1] != '\0' &&
	       used + 1U < size) {
		/* A pair that is not hexadecimal ends the SSID. */
		count = sscanf(hex, "%2x", &decoded_byte);
		if (count != 1)
			break;

		/* A control byte cannot be drawn. */
		if (decoded_byte < 0x20U || decoded_byte == 0x7fU)
			decoded_byte = '?';

		/* Publishes one drawable SSID byte and advances to the next pair. */
		text[used] = (char)decoded_byte;
		used++;
		hex += 2;
	}

	/* The text ends where the digits did. */
	text[used] = '\0';

	/* Succeeded: the decoded SSID is terminated. */
	return;
}

/* Reads the monotonic clock in milliseconds. */
static uint64_t
network_milliseconds(
	void)
{
	struct timespec now;
	int error;

	/* The clock; one that fails reads as the start of time. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0)
		return 0;

	/* Succeeded: expresses the monotonic clock in milliseconds. */
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
