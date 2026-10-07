/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth daemon (ws143-p003, plan/ws143/phase003/phase.md).
 *
 * It opens the first Bluetooth controller's node (/dev/bt0 to /dev/bt15),
 * starts it (Intel's firmware when it needs it, then the HCI core), and
 * answers on /run/bluetoothd.sock: the state and the controller, and a
 * scan of the devices around.  A node that goes (the controller was pulled
 * out, or re-enumerated after its firmware's boot) is closed, and the
 * nodes are looked for again every BTD_RETRY_MS; so is a controller whose
 * start failed in a way a new start may mend, BTD_FAILURES_MAX times in a
 * row at most.
 *
 * Intel's firmware is loaded once for a controller (by its USB vendor,
 * product and port): one that comes back in its bootloader after a load
 * is not loaded again until the daemon starts again, and one that does
 * not come back within BTD_REAPPEAR_MS of its boot is a failed load.
 *
 *   bluetoothd [-f /dev/btN]   (a node of its own; else the lowest that opens)
 *
 * It runs as root in this Phase; the privilege separation is p004's.
 */

#include "userland/base/bluetoothd/protocol.h"
#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* How often the nodes are looked for while there is no controller, and how many nodes there can be. */
#define BTD_RETRY_MS		2000U
#define BTD_NODES		16U

/* How many starts in a row may fail before the daemon stops trying, and how long a booted controller may take to come back. */
#define BTD_FAILURES_MAX	3U
#define BTD_REAPPEAR_MS		10000U

/* How many controllers' loads are remembered, and the length of a controller's key. */
#define BTD_LOADS_MAX		8U
#define BTD_KEY_MAX		96U

/* How many clients may be connected, and how long a write to one may wait. */
#define BTD_CLIENTS_MAX		8U
#define BTD_CLIENT_WRITE_MS	1000

/* One client of the socket: its descriptor, the bytes of a line not ended yet, and whether it waits for a scan's end. */
struct btd_client {
	int descriptor;
	char input[BTD_LINE_MAX];
	size_t used;
	int waits_scan;
};

static void btd_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int btd_node_control(void *context, unsigned long request, void *argument);
static void btd_key(const struct bt_info *info, char *key, size_t size);
static int btd_loaded(const char *key);
static void btd_remember_load(const char *key);
static int btd_listen(void);
static void btd_open(void);
static void btd_close(void);
static void btd_accept(int listener);
static void btd_read(struct btd_client *client);
static void btd_line(struct btd_client *client, char *line);
static void btd_show(struct btd_client *client);
static void btd_devices(struct btd_client *client);
static void btd_scan(struct btd_client *client, const char *argument);
static void btd_scan_end(void);
static void btd_write(struct btd_client *client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void btd_client_close(struct btd_client *client);

/* The clients; a free slot has descriptor -1.  The daemon's one thread uses them. */
static struct btd_client btd_clients[BTD_CLIENTS_MAX];

/*
 * The controller's session, open while session_open is nonzero, and when
 * the nodes were last looked for (0: never).
 */
static struct btd_session btd_session;
static int btd_session_open;
static uint64_t btd_looked_ms;

/* The node given with -f, or NULL for the lowest that opens. */
static const char *btd_node;

/*
 * How many starts in a row failed (0 again after a ready start), and
 * whether the daemon stopped trying; whether the last open's busy node was
 * logged already.
 */
static unsigned btd_failures;
static int btd_stopped;
static int btd_busy_logged;

/*
 * The controllers whose firmware was loaded (their keys, see btd_key), and
 * when the last booted one is due back (0: none awaited).  Kept for the
 * daemon's life.
 */
static char btd_loads[BTD_LOADS_MAX][BTD_KEY_MAX];
static unsigned btd_load_count;
static uint64_t btd_reappear_ms;

/*
 * Runs the daemon until it is killed.
 */
int
main(
	int argc,
	char **argv)
{
	struct pollfd descriptors[2U + BTD_CLIENTS_MAX];
	unsigned count;
	unsigned index;
	uint64_t now;
	int listener;
	int timeout;
	int ready;
	int error;
	int same;

	/* The node of its own, when given. */
	if (argc == 3) {
		same = strcmp(argv[1], "-f");
		if (same == 0)
			btd_node = argv[2];
	}

	/* Any other argument is a misuse. */
	if (argc != 1 && btd_node == NULL) {
		(void)fprintf(stderr, "usage: bluetoothd [-f /dev/btN]\n");
		return 64;
	}

	/* A client that goes away must not end the daemon; what it does goes to the system's log. */
	(void)signal(SIGPIPE, SIG_IGN);
	openlog("bluetoothd", LOG_PID, LOG_DAEMON);

	/* No client yet. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++)
		btd_clients[index].descriptor = -1;

	/* The socket. */
	listener = btd_listen();
	if (listener < 0) {
		btd_log("bluetoothd: %s: %s\n", BTD_SOCKET, strerror(errno));
		return 1;
	}

	/* The controller there is now. */
	btd_open();
	btd_log("BLUETOOTHD READY state=%s\n", btd_state_name(btd_session.state));

	/* Clients, the controller's events, the scan's end and the look for nodes. */
	for (;;) {
		/* The listener, the controller's node and every client. */
		count = 0U;
		descriptors[count].fd = listener;
		descriptors[count].events = POLLIN;
		count++;
		descriptors[count].fd = -1;
		descriptors[count].events = POLLIN;
		if (btd_session_open)
			descriptors[count].fd = btd_session.descriptor;
		count++;

		/* Each client's descriptor. */
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			descriptors[count].fd = btd_clients[index].descriptor;
			descriptors[count].events = POLLIN;
			count++;
		}

		/* Waits for something, the scan's end, or the next look for nodes. */
		now = btd_now_ms();
		timeout = -1;
		if (btd_session_open && btd_session.scanning) {
			timeout = 0;
			if (btd_session.scan_end_ms > now)
				timeout = (int)(btd_session.scan_end_ms - now);
		} else if (!btd_session_open && !btd_stopped) {
			timeout = 0;
			if (btd_looked_ms + BTD_RETRY_MS > now)
				timeout = (int)(btd_looked_ms + BTD_RETRY_MS - now);
		}

		/* The wait. */
		for (index = 0U; index < count; index++)
			descriptors[index].revents = 0;
		ready = poll(descriptors, count, timeout);
		if (ready < 0 && errno != EINTR)
			return 1;

		/* The controller's packets; a node that went is closed. */
		if (btd_session_open && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			error = btd_session_input(&btd_session);
			if (error != 0 && error != EAGAIN)
				btd_close();
		}

		/* A scan that is over answers the client that asked. */
		now = btd_now_ms();
		if (btd_session_open && btd_session.scanning && now >= btd_session.scan_end_ms)
			btd_scan_end();

		/* A controller reset under the daemon is started again. */
		if (btd_session_open && btd_session.state == BTD_STATE_ERROR)
			btd_close();

		/* A booted controller that did not come back is a failed load. */
		if (!btd_session_open && btd_reappear_ms != 0U && now >= btd_reappear_ms) {
			btd_reappear_ms = 0U;
			btd_session.state = BTD_STATE_FIRMWARE_FAILED;
			(void)snprintf(btd_session.reason, sizeof(btd_session.reason), "%s", "the controller did not come back after its boot");
			btd_log("bluetoothd: %s\n", btd_session.reason);
		}

		/* No controller: the nodes are looked for again, unless the daemon stopped trying. */
		if (!btd_session_open && !btd_stopped && now >= btd_looked_ms + BTD_RETRY_MS)
			btd_open();

		/* A new client, then each client's lines. */
		if ((descriptors[0].revents & POLLIN) != 0)
			btd_accept(listener);
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			if (btd_clients[index].descriptor < 0)
				continue;
			if ((descriptors[2U + index].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
				continue;
			btd_read(&btd_clients[index]);
		}
	}
}

/* Writes a line to the system's log and to standard error. */
static void
btd_log(
	const char *format,
	...)
{
	va_list arguments;

	/* The system's log. */
	va_start(arguments, format);
	vsyslog(LOG_INFO, format, arguments);
	va_end(arguments);

	/* Standard error, which the service's log keeps. */
	va_start(arguments, format);
	(void)vfprintf(stderr, format, arguments);
	va_end(arguments);
}

/* Listens on the socket, which anyone may use (a change is checked by uid). */
static int
btd_listen(
	void)
{
	struct sockaddr_un address;
	int descriptor;
	int status;
	int error;

	/* The socket, in place of an old one. */
	descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (descriptor < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", BTD_SOCKET);
	(void)unlink(BTD_SOCKET);

	/* Bound, readable and writable by everyone, listening. */
	status = bind(descriptor, (struct sockaddr *)&address, sizeof(address));
	if (status == 0)
		status = chmod(BTD_SOCKET, 0666);
	if (status == 0)
		status = listen(descriptor, (int)BTD_CLIENTS_MAX);
	if (status != 0) {
		error = errno;
		(void)close(descriptor);
		errno = error;
		return -1;
	}

	/* Succeeded: the listener. */
	return descriptor;
}

/*
 * Opens the controller's node (the one given, or the lowest that opens)
 * and starts it.  A start that failed with the node still there keeps it
 * open (its state says why); a node that went during the start is closed,
 * and so is one whose start a new start may mend.
 */
static void
btd_open(
	void)
{
	char key[BTD_KEY_MAX];
	char path[BTD_PATH_MAX];
	const char *trace_label;
	unsigned index;
	int descriptor;
	int loaded;
	int error;

	/* Looked for now. */
	btd_looked_ms = btd_now_ms();

	/* The node given, or the first that opens. */
	descriptor = -1;
	error = ENOENT;
	for (index = 0U; index < BTD_NODES; index++) {
		if (btd_node != NULL)
			(void)snprintf(path, sizeof(path), "%s", btd_node);
		else
			(void)snprintf(path, sizeof(path), "/dev/bt%u", index);
		descriptor = open(path, O_RDWR | O_CLOEXEC);
		if (descriptor >= 0)
			break;
		if (errno == EBUSY)
			error = EBUSY;
		if (btd_node != NULL)
			break;
	}

	/* No controller (a busy node, another program's, is logged once). */
	if (descriptor < 0) {
		if (error == EBUSY && !btd_busy_logged)
			btd_log("bluetoothd: the controller's node is open in another program\n");
		btd_busy_logged = (error == EBUSY);
		return;
	}

	/* A node that opened ends the busy note. */
	btd_busy_logged = 0;

	/* Its session; a controller loaded before must not be loaded again. */
	btd_session_init(&btd_session, descriptor, btd_node_control, &btd_session, path, BTD_FIRMWARE_FOLDER);
	btd_session_open = 1;
	btd_reappear_ms = 0U;
	error = ioctl(descriptor, BT_IOC_GET_INFO, &btd_session.info);
	key[0] = '\0';
	if (error == 0)
		btd_key(&btd_session.info, key, sizeof(key));
	loaded = 0;
	if (key[0] != '\0')
		loaded = btd_loaded(key);
	if (loaded)
		btd_session.load_allowed = 0;

	/* The start, logged with the first Secure Send answers when a load was sent. */
	error = btd_session_start(&btd_session);
	trace_label = "";
	if (btd_session.trace[0] != '\0')
		trace_label = ", secure send answers ";
	btd_log("bluetoothd: %s state=%s %s (error %d, stray %u, malformed %u%s%s)\n",
		path,
		btd_state_name(btd_session.state),
		btd_session.reason,
		error,
		btd_session.stray_answers,
		btd_session.malformed,
		trace_label,
		btd_session.trace);

	/* A load was sent: the controller is remembered, and one that went to boot is awaited. */
	if (btd_session.load_sent && key[0] != '\0')
		btd_remember_load(key);
	if (btd_session.load_sent && btd_session.state == BTD_STATE_LOST)
		btd_reappear_ms = btd_now_ms() + BTD_REAPPEAR_MS;

	/* A ready start ends a run of failures; a failed one counts, and too many stop the tries. */
	if (btd_session.state == BTD_STATE_READY) {
		btd_failures = 0U;
	} else if (btd_session.state == BTD_STATE_ERROR) {
		btd_failures++;
		if (btd_failures >= BTD_FAILURES_MAX) {
			btd_stopped = 1;
			btd_log("bluetoothd: %u starts failed in a row; not trying again until restarted\n", btd_failures);
		}
	}

	/* A node that went, or a step that a new start may mend, is closed (looked for again later). */
	if (btd_session.state == BTD_STATE_LOST || btd_session.state == BTD_STATE_ERROR)
		btd_close();
}

/* Closes the controller's node; a scan's waiting client hears that it ended. */
static void
btd_close(
	void)
{
	unsigned index;

	/* Nothing open. */
	if (!btd_session_open)
		return;

	/* A client waiting for a scan is answered. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor < 0 || !btd_clients[index].waits_scan)
			continue;
		btd_clients[index].waits_scan = 0;
		btd_write(&btd_clients[index], "ERROR lost\nDONE\n");
	}

	/* The node closed; the nodes are looked for again after the retry time. */
	btd_log("bluetoothd: %s closed (%s %s)\n", btd_session.path, btd_state_name(btd_session.state), btd_session.reason);
	(void)close(btd_session.descriptor);
	btd_session_open = 0;
	if (!btd_stopped)
		btd_session.state = BTD_STATE_NONE;
	btd_looked_ms = btd_now_ms();
}

/* Takes a new client, when there is a free slot. */
static void
btd_accept(
	int listener)
{
	unsigned index;
	int descriptor;

	/* The connection; its reads never block. */
	descriptor = accept(listener, NULL, NULL);
	if (descriptor < 0)
		return;
	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);
	(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);

	/* A free slot. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor >= 0)
			continue;
		btd_clients[index].descriptor = descriptor;
		btd_clients[index].used = 0U;
		btd_clients[index].waits_scan = 0;
		return;
	}

	/* Too many clients. */
	(void)close(descriptor);
}

/* Reads a client's bytes and carries out each whole line; a line too long closes the client. */
static void
btd_read(
	struct btd_client *client)
{
	ssize_t count;
	size_t length;
	char *end;
	int error;

	/* What came, without waiting. */
	count = recv(client->descriptor, client->input + client->used, sizeof(client->input) - 1U - client->used, 0);
	if (count < 0) {
		error = errno;
		if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
			return;
	}

	/* A client that went (or failed). */
	if (count <= 0) {
		btd_client_close(client);
		return;
	}

	/* The bytes added to the line. */
	client->used += (size_t)count;
	client->input[client->used] = '\0';

	/* Each whole line. */
	for (;;) {
		end = strchr(client->input, '\n');
		if (end == NULL)
			break;
		*end = '\0';
		btd_line(client, client->input);
		if (client->descriptor < 0)
			return;
		length = (size_t)(end + 1 - client->input);
		memmove(client->input, end + 1, client->used - length + 1U);
		client->used -= length;
	}

	/* A line that fills the buffer without ending is not one. */
	if (client->used + 1U >= sizeof(client->input))
		btd_client_close(client);
}

/* Carries out one request line. */
static void
btd_line(
	struct btd_client *client,
	char *line)
{
	int same;

	/* A client waiting for its scan asks nothing more until it is answered. */
	if (client->waits_scan)
		return;

	/* SHOW. */
	same = strcmp(line, "SHOW");
	if (same == 0) {
		btd_show(client);
		return;
	}

	/* DEVICES. */
	same = strcmp(line, "DEVICES");
	if (same == 0) {
		btd_devices(client);
		return;
	}

	/* SCAN SECONDS. */
	same = strncmp(line, "SCAN ", 5U);
	if (same == 0) {
		btd_scan(client, line + 5);
		return;
	}

	/* Anything else. */
	btd_write(client, "ERROR request\nDONE\n");
}

/* Answers SHOW: the state, and the controller when one is open. */
static void
btd_show(
	struct btd_client *client)
{
	char address[24];
	char name[4U * BT_TEXT_MAX];
	const char *firmware;
	int error;

	/* The state, and why. */
	if (btd_session.reason[0] != '\0')
		btd_write(client, "STATE %s %s\n", btd_state_name(btd_session.state), btd_session.reason);
	else
		btd_write(client, "STATE %s\n", btd_state_name(btd_session.state));

	/* The controller. */
	firmware = "-";
	if (btd_session.firmware_loaded)
		firmware = "loaded";
	if (btd_session_open) {
		btd_format_address(btd_session.address, address, sizeof(address));
		if (!btd_session.have_address)
			(void)snprintf(address, sizeof(address), "%s", "-");
		error = btd_escape(btd_session.info.name, name, sizeof(name));
		if (error != 0)
			name[0] = '\0';
		btd_write(client,
			  "CONTROLLER node=%s vendor=%04x product=%04x name=\"%s\" address=%s hci=%u manufacturer=%u le=%d p256=%d dhkey=%d firmware=%s\n",
			  btd_session.path,
			  (unsigned)btd_session.info.vendor,
			  (unsigned)btd_session.info.product,
			  name,
			  address,
			  (unsigned)btd_session.hci_version,
			  (unsigned)btd_session.manufacturer,
			  btd_session.le,
			  btd_session.p256,
			  btd_session.dhkey,
			  firmware);
	}

	/* The end. */
	btd_write(client, "DONE\n");
}

/* Answers DEVICES: the last scan's devices. */
static void
btd_devices(
	struct btd_client *client)
{
	const struct btd_device *device;
	char address[24];
	char name[4U * BTD_NAME_MAX];
	char extra[64];
	unsigned index;
	size_t used;
	int error;

	/* Each device, as one line. */
	for (index = 0U; index < btd_session.devices.count; index++) {
		device = &btd_session.devices.entries[index];
		btd_format_address(device->address, address, sizeof(address));
		error = btd_escape(device->name, name, sizeof(name));
		if (error != 0)
			name[0] = '\0';

		/* The fields that were seen. */
		extra[0] = '\0';
		used = 0U;
		if (device->has_rssi)
			used += (size_t)snprintf(extra + used, sizeof(extra) - used, " rssi=%d", device->rssi);
		if (device->has_class && used < sizeof(extra))
			used += (size_t)snprintf(extra + used, sizeof(extra) - used, " class=0x%06x", (unsigned)device->class_of_device);
		if (device->has_appearance && used < sizeof(extra))
			(void)snprintf(extra + used, sizeof(extra) - used, " appearance=0x%04x", (unsigned)device->appearance);

		/* The line. */
		btd_write(client, "DEVICE address=%s type=%s%s name=\"%s\"\n", address, btd_address_type_name(device->type), extra, name);
	}

	/* Devices the table had no room for. */
	if (btd_session.devices.dropped != 0U)
		btd_write(client, "DROPPED %u\n", btd_session.devices.dropped);

	/* The end. */
	btd_write(client, "DONE\n");
}

/* Starts a scan for root; the client hears the devices when it ends. */
static void
btd_scan(
	struct btd_client *client,
	const char *argument)
{
	unsigned long seconds;
	char *end;
	uid_t uid;
	gid_t gid;
	int status;
	int error;

	/* Only root changes the controller's state in this Phase. */
	status = getpeereid(client->descriptor, &uid, &gid);
	if (status != 0 || uid != 0) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* A length in range. */
	errno = 0;
	seconds = strtoul(argument, &end, 10);
	if (errno != 0 || end == argument || *end != '\0' || seconds == 0U || seconds > BTD_SCAN_SECONDS_MAX) {
		btd_write(client, "ERROR seconds\nDONE\n");
		return;
	}

	/* A ready controller, and no scan running. */
	if (!btd_session_open) {
		btd_write(client, "ERROR no-controller\nDONE\n");
		return;
	}

	/* One scan at a time. */
	if (btd_session.scanning) {
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* The scan, started (a controller that is not ready says so). */
	error = btd_session_scan_start(&btd_session, (unsigned)seconds);
	if (error == EBUSY) {
		btd_write(client, "ERROR not-ready\nDONE\n");
		return;
	}

	/* Any other failure, named. */
	if (error != 0) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(error));
		if (error == ENODEV)
			btd_close();
		return;
	}

	/* Succeeded: the client waits for its end. */
	client->waits_scan = 1;
	btd_log("bluetoothd: scan of %lu s started\n", seconds);
}

/* Ends a scan that is over and answers the client that asked for it. */
static void
btd_scan_end(
	void)
{
	unsigned index;
	int error;

	/* The scan, stopped. */
	error = btd_session_scan_stop(&btd_session);
	btd_log("bluetoothd: scan ended: %u devices (dropped %u, malformed %u, error %d)\n",
		btd_session.devices.count,
		btd_session.devices.dropped,
		btd_session.malformed,
		error);

	/* The waiting client hears the devices. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor < 0 || !btd_clients[index].waits_scan)
			continue;
		btd_clients[index].waits_scan = 0;
		btd_devices(&btd_clients[index]);
	}

	/* A node that went during the stop is closed. */
	if (error == ENODEV)
		btd_close();
}

/* Writes a formatted text to a client, waiting a little for room; a client that cannot take it is closed. */
static void
btd_write(
	struct btd_client *client,
	const char *format,
	...)
{
	struct pollfd room;
	char text[BTD_LINE_MAX + 4U * BTD_NAME_MAX];
	va_list arguments;
	ssize_t sent;
	size_t length;
	size_t done;
	int ready;
	int written;

	/* A client already closed. */
	if (client->descriptor < 0)
		return;

	/* The text. */
	va_start(arguments, format);
	written = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	if (written < 0)
		return;
	length = (size_t)written;
	if (length >= sizeof(text))
		length = sizeof(text) - 1U;

	/* All of it, waiting for room at most BTD_CLIENT_WRITE_MS each time. */
	done = 0U;
	while (done < length) {
		sent = send(client->descriptor, text + done, length - done, 0);
		if (sent > 0) {
			done += (size_t)sent;
			continue;
		}

		/* An interrupted send is tried again. */
		if (sent < 0 && errno == EINTR)
			continue;
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			/* Waits for room. */
			room.fd = client->descriptor;
			room.events = POLLOUT;
			room.revents = 0;
			ready = poll(&room, 1U, BTD_CLIENT_WRITE_MS);
			if (ready > 0)
				continue;
		}

		/* A client that does not read is closed. */
		btd_client_close(client);
		return;
	}
}

/* Closes a client and frees its slot. */
static void
btd_client_close(
	struct btd_client *client)
{
	/* The descriptor, and the slot. */
	(void)close(client->descriptor);
	client->descriptor = -1;
	client->used = 0U;
	client->waits_scan = 0;
}

/* Calls an ioctl of the controller's node (the session's control). */
static int
btd_node_control(
	void *context,
	unsigned long request,
	void *argument)
{
	struct btd_session *session;
	int status;

	/* The session's node. */
	session = context;
	status = ioctl(session->descriptor, request, argument);
	if (status != 0)
		return errno;

	/* Succeeded. */
	return 0;
}

/*
 * Writes a controller's key: its USB vendor and product and its port,
 * "VVVV:PPPP usbB/portN", without the USB address, which a re-enumeration
 * changes.  An empty key when the path is not of that form.
 */
static void
btd_key(
	const struct bt_info *info,
	char *key,
	size_t size)
{
	const char *device;
	size_t length;

	/* The path up to its device part ("usbB/portN/deviceD/interfaceI"). */
	key[0] = '\0';
	device = strstr(info->physical_path, "/device");
	if (device == NULL)
		return;
	length = (size_t)(device - info->physical_path);

	/* The key. */
	(void)snprintf(key, size, "%04x:%04x %.*s", (unsigned)info->vendor, (unsigned)info->product, (int)length, info->physical_path);
}

/* Tells whether a controller's firmware was loaded before. */
static int
btd_loaded(
	const char *key)
{
	unsigned index;
	int same;

	/* Each controller remembered. */
	for (index = 0U; index < btd_load_count; index++) {
		same = strcmp(btd_loads[index], key);
		if (same == 0)
			return 1;
	}

	/* Never loaded. */
	return 0;
}

/* Remembers that a controller's firmware was loaded (the oldest goes when the table is full). */
static void
btd_remember_load(
	const char *key)
{
	int loaded;

	/* Once. */
	loaded = btd_loaded(key);
	if (loaded)
		return;

	/* A full table drops its oldest. */
	if (btd_load_count >= BTD_LOADS_MAX) {
		memmove(btd_loads[0], btd_loads[1], sizeof(btd_loads[0]) * (BTD_LOADS_MAX - 1U));
		btd_load_count--;
	}

	/* At the end. */
	(void)snprintf(btd_loads[btd_load_count], sizeof(btd_loads[0]), "%s", key);
	btd_load_count++;
}
