/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Bluetooth daemon (ws143-p003 and ws143-p004, plan/ws143/phase003/
 * phase.md and plan/ws143/phase004/phase.md).
 *
 * It listens on /run/bluetoothd.sock as root, then separates (privsep.c):
 * a parent stays root to open the controller's node, and the child runs
 * the rest as the account _bluetooth.  The child opens the first
 * Bluetooth controller's node through the parent, starts it (Intel's
 * firmware when it needs it, then the HCI core), and answers on the
 * socket: the state and the controller, a scan, a pairing and its agent,
 * the bonds and their removal.  A node that goes (the controller was
 * pulled out, or re-enumerated after its firmware's boot) is closed, and
 * the nodes are looked for again every BTD_RETRY_MS; so is a controller
 * whose start failed in a way a new start may mend, BTD_FAILURES_MAX times
 * in a row at most.
 *
 * Intel's firmware is loaded once for a controller (by its USB vendor,
 * product and port): one that comes back in its bootloader after a load
 * is not loaded again until the daemon starts again, and one that does
 * not come back within BTD_REAPPEAR_MS of its boot is a failed load.
 *
 * Who may change things (D8, plan section 6): root, the seat's user (the
 * display's owner, not the greeter), and the members of wheel.  Anyone
 * may read the state, the devices and the bonds.
 *
 *   bluetoothd [-f /dev/bluetoothN]   (a node of its own; else the lowest that opens)
 */

#include "userland/base/bluetoothd/hid.h"
#include "userland/base/bluetoothd/keys.h"
#include "userland/base/bluetoothd/pair.h"
#include "userland/base/bluetoothd/privsep.h"
#include "userland/base/bluetoothd/protocol.h"
#include "userland/base/bluetoothd/router.h"
#include "userland/base/bluetoothd/session.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
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

/* How often the nodes are looked for while there is no controller. */
#define BTD_RETRY_MS		2000U

/* How many starts in a row may fail before the daemon stops trying, and how long a booted controller may take to come back. */
#define BTD_FAILURES_MAX	3U
#define BTD_REAPPEAR_MS		10000U

/* How many controllers' loads are remembered, and the length of a controller's key. */
#define BTD_LOADS_MAX		8U
#define BTD_KEY_MAX		96U

/* How many clients may be connected, and how long a write to one may wait. */
#define BTD_CLIENTS_MAX		8U
#define BTD_CLIENT_WRITE_MS	1000

/* How many packets one round of the loop handles at most (the clients are not starved). */
#define BTD_PACKETS_A_ROUND	64U

/* Marks a parameter a function takes for its signature's sake. */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* How many bonds BONDS lists. */
#define BTD_BONDS_MAX		64U

/* The display whose owner is the seat's user, the greeter's account, and the group whose members may change things. */
#define BTD_SEAT_NODE		"/dev/gpu0"

/* The file that keeps the user's switch (design D11a: the state before is kept, on the first time), in the keys' folder. */
#define BTD_POWER_FILE		BTD_KEYS_FOLDER "/power"
#define BTD_GREETER		"_greeter"
#define BTD_ADMIN_GROUP		"wheel"
#define BTD_GROUPS_MAX		64

/* No client (an index of btd_clients). */
#define BTD_NO_CLIENT		(-1)

/*
 * One client of the socket: its descriptor, its uid, whether it stopped
 * reading (it is closed at the end of the loop's round, where nothing
 * else uses it), the bytes of a line not ended yet, and what it waits for
 * (a scan's end, a pairing's end).
 */
struct btd_client {
	int descriptor;
	uid_t uid;
	int dead;
	char input[BTD_LINE_MAX];
	size_t used;
	int waits_scan;
	int waits_pair;
	int waits_connect;
	uint8_t connect_address[BTD_ADDRESS_BYTES];
};

static void btd_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int btd_node_control(void *context, unsigned long request, void *argument);
static void btd_random(void *context, uint8_t *bytes, size_t length);
static void btd_key(const struct bt_info *info, char *key, size_t size);
static int btd_loaded(const char *key);
static void btd_remember_load(const char *key);
static int btd_listen(void);
static void btd_open(void);
static void btd_close(void);
static void btd_packets(void);
static int btd_timeout(void);
static void btd_accept(int listener);
static void btd_read(int index);
static void btd_line(int index, char *line);
static void btd_show(struct btd_client *client);
static void btd_devices(struct btd_client *client);
static void btd_scan(struct btd_client *client, const char *argument);
static void btd_scan_end(void);
static void btd_pair(int index, const char *argument);
static void btd_agent(int index);
static void btd_answer(int index, int accepted);
static void btd_forget(struct btd_client *client, const char *argument);
static void btd_bonds(struct btd_client *client);
static void btd_power(struct btd_client *client, const char *argument);
static void btd_power_load(void);
static int btd_power_save(void);
static void btd_question_end(void);
static void btd_ask(void *context, unsigned kind, uint32_t number);
static void btd_paired(void *context, const char *answer);
static int btd_permitted(uid_t uid);
static void btd_connect(int index, const char *argument);
static void btd_disconnect(struct btd_client *client, const char *argument);
static void btd_status(struct btd_client *client);
static int btd_hid_bridge(void *context, int *descriptor);
static void btd_hid_told(void *context, const uint8_t *address, const char *line);
static void btd_hid_holding(void);
static int btd_parse_device(const char *text, uint8_t *address, unsigned *type);
static void btd_write(struct btd_client *client, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void btd_client_close(int index);

/* The clients; a free slot has descriptor -1.  The daemon's one thread uses them. */
static struct btd_client btd_clients[BTD_CLIENTS_MAX];

/*
 * The controller's session, open while session_open is nonzero, and when
 * the nodes were last looked for (0: never).
 */
static struct btd_session btd_session;
static int btd_session_open;
static uint64_t btd_looked_ms;

/*
 * The pairing of the controller, the session's handler while the node is
 * open.  It lives as long as the daemon; a closed node ends a pairing.
 */
static struct btd_pair btd_pairing;

/*
 * The router of the session's packets (ws143-p005): the session's handler
 * while the node is open, giving the pairing its device's packets and
 * refusing what nobody owns.  It lives as long as the daemon; a closed
 * node empties its routes.
 */
static struct btd_router btd_routing;

/*
 * The HID host (ws143-p005 i02c): the router's owner of the HID devices'
 * connections.  It lives as long as the daemon; a closed node ends its
 * connections (the devices stay wanted).
 */
static struct btd_hid btd_hid_host;

/*
 * The clients of a pairing: the one that asked for it, the agent that
 * named itself (it answers for pairings of its uid, or of anyone when it
 * is the seat's user), and the one asked the question now
 * (BTD_NO_CLIENT: none).
 */
static int btd_pair_client = BTD_NO_CLIENT;
static int btd_agent_client = BTD_NO_CLIENT;
static int btd_asked_client = BTD_NO_CLIENT;

/* The child's ends of the privilege separation. */
static struct btd_privsep btd_separation;

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
 * Whether the user turned Bluetooth off (POWER off, ws143-p006): the
 * daemon then starts no scan and no pairing (they are answered
 * "ERROR off"), and SHOW says "STATE off" over a ready controller and
 * "POWER off".  The saved keys stay.  It is kept in BTD_POWER_FILE across
 * starts (design D11a, the user's decision); without the file it is on.
 */
static int btd_powered_off;

/*
 * The client last asked a question of a pairing (CONFIRM, CONSENT or the
 * PASSKEY shown; BTD_NO_CLIENT for none): it hears ASK-END when the
 * question is over (answered, or the pairing ended).
 */
static int btd_question_client = BTD_NO_CLIENT;

/*
 * Runs the daemon until it is killed.
 */
int
main(
	int argc,
	char **argv)
{
	struct pollfd descriptors[3U + BTD_CLIENTS_MAX];
	struct btd_hid_hooks hid_hooks;
	struct btd_router_hid router_hid;
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
		(void)fprintf(stderr, "usage: bluetoothd [-f /dev/bluetoothN]\n");
		return 64;
	}

	/* A client that goes away must not end the daemon; what it does goes to the system's log. */
	(void)signal(SIGPIPE, SIG_IGN);
	openlog("bluetoothd", LOG_PID, LOG_DAEMON);

	/* No client yet. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++)
		btd_clients[index].descriptor = -1;

	/* The socket, made as root. */
	listener = btd_listen();
	if (listener < 0) {
		btd_log("bluetoothd: %s: %s\n", BTD_SOCKET, strerror(errno));
		return 1;
	}

	/* The separation: from here on this is the child, running as the daemon's account. */
	error = btd_privsep_start(btd_node, BTD_KEYS_FOLDER, listener, &btd_separation);
	if (error != 0) {
		btd_log("bluetoothd: privilege separation: %s\n", strerror(error));
		return 1;
	}

	/* The user's switch, as it was left (ws143-p006). */
	btd_power_load();

	/* The pairing, handed the session's connection packets from each start on. */
	btd_pair_init(&btd_pairing, &btd_session, BTD_KEYS_FOLDER, btd_ask, btd_paired, NULL, btd_random, NULL);
	btd_router_init(&btd_routing, &btd_pairing);

	/*
	 * The HID host, the router's owner of its connections.  The pairing's
	 * handoff (btd_hid_handoff) is given in i02d, with the loopback
	 * controller that answers SDP (until then a paired device is ended as
	 * in p004, which its tests expect).
	 */
	hid_hooks.context = NULL;
	hid_hooks.open_bridge = btd_hid_bridge;
	hid_hooks.answer = btd_hid_told;
	btd_hid_init(&btd_hid_host, &btd_session, BTD_KEYS_FOLDER, &btd_routing, &hid_hooks);
	router_hid.context = &btd_hid_host;
	router_hid.wants = btd_hid_wants;
	router_hid.claims = btd_hid_claims;
	router_hid.handle = btd_hid_handle;
	btd_router_set_hid(&btd_routing, &router_hid);

	/* The controller there is now. */
	btd_open();
	btd_log("BLUETOOTHD READY state=%s uid=%u\n", btd_state_name(btd_session.state), (unsigned)getuid());

	/* Clients, the controller's packets, the deadlines and the look for nodes. */
	for (;;) {
		/* The listener, the controller's node, the parent's liveness and every client. */
		count = 0U;
		descriptors[count].fd = listener;
		descriptors[count].events = POLLIN;
		count++;
		descriptors[count].fd = -1;
		descriptors[count].events = POLLIN;
		if (btd_session_open)
			descriptors[count].fd = btd_session.descriptor;
		count++;
		descriptors[count].fd = btd_separation.liveness;
		descriptors[count].events = POLLIN;
		count++;

		/* Each client's descriptor. */
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			descriptors[count].fd = btd_clients[index].descriptor;
			descriptors[count].events = POLLIN;
			count++;
		}

		/* The wait, as long as the earliest deadline allows. */
		timeout = btd_timeout();
		for (index = 0U; index < count; index++)
			descriptors[index].revents = 0;
		ready = poll(descriptors, count, timeout);
		if (ready < 0 && errno != EINTR)
			return 1;

		/* The parent went: nothing can be opened any more. */
		if ((descriptors[2].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			btd_log("bluetoothd: the privileged parent went; ending\n");
			return 0;
		}

		/* The controller's packets: those the node has, and those queued while a command waited (review BL2). */
		if (btd_session_open) {
			if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
				btd_packets();
		}

		/* Packets queued while a command waited, even when the node has nothing new. */
		if (btd_session_open) {
			ready = btd_session_pending(&btd_session);
			if (ready)
				btd_packets();
		}

		/* The pairing's deadlines. */
		now = btd_now_ms();
		btd_pair_tick(&btd_pairing, now);

		/* The HID host's deadlines and pages (held while a pairing or a scan runs). */
		if (btd_session_open && btd_session.state == BTD_STATE_READY) {
			btd_hid_holding();
			btd_hid_tick(&btd_hid_host, now);
		}

		/* A scan that is over answers the client that asked. */
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
			if (btd_clients[index].descriptor < 0 || btd_clients[index].dead)
				continue;
			if ((descriptors[3U + index].revents & (POLLIN | POLLHUP | POLLERR)) == 0)
				continue;
			btd_read((int)index);
		}

		/* The clients that stopped reading are closed now. */
		for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
			if (btd_clients[index].descriptor >= 0 && btd_clients[index].dead)
				btd_client_close((int)index);
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
 * Opens the controller's node through the parent and starts it.  A start
 * that failed with the node still there keeps it open (its state says
 * why); a node that went during the start is closed, and so is one whose
 * start a new start may mend.
 */
static void
btd_open(
	void)
{
	char key[BTD_KEY_MAX];
	char path[BTD_PATH_MAX];
	const char *trace_label;
	int descriptor;
	int loaded;
	int error;

	/* Looked for now. */
	btd_looked_ms = btd_now_ms();

	/* The node, from the parent. */
	error = btd_privsep_open(&btd_separation, path, sizeof(path), &descriptor);

	/* No controller (a busy node, another program's, is logged once). */
	if (error != 0) {
		if (error == EBUSY && !btd_busy_logged)
			btd_log("bluetoothd: the controller's node is open in another program\n");
		btd_busy_logged = (error == EBUSY);
		if (error != EBUSY && error != ENOENT)
			btd_log("bluetoothd: the parent could not open a node: %s\n", strerror(error));
		return;
	}

	/* A node that opened ends the busy note. */
	btd_busy_logged = 0;

	/* Its session, its packets handed to the router; a controller loaded before must not be loaded again. */
	btd_session_init(&btd_session, descriptor, btd_node_control, &btd_session, path, BTD_FIRMWARE_FOLDER);
	btd_session.handler = btd_router_handle;
	btd_session.handler_context = &btd_routing;
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
	btd_log("bluetoothd: %s state=%s %s (error %d, stray %u, malformed %u, ssp %d, sc %d%s%s)\n",
		path,
		btd_state_name(btd_session.state),
		btd_session.reason,
		error,
		btd_session.stray_answers,
		btd_session.malformed,
		btd_session.ssp,
		btd_session.secure_connections,
		trace_label,
		btd_session.trace);

	/* A load was sent: the controller is remembered, and one that went to boot is awaited. */
	if (btd_session.load_sent && key[0] != '\0')
		btd_remember_load(key);
	if (btd_session.load_sent && btd_session.state == BTD_STATE_LOST)
		btd_reappear_ms = btd_now_ms() + BTD_REAPPEAR_MS;

	/* A ready start ends a run of failures (the HID devices are read); a failed one counts, and too many stop the tries. */
	if (btd_session.state == BTD_STATE_READY) {
		btd_failures = 0U;
		if (!btd_powered_off)
			btd_hid_refresh(&btd_hid_host);
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

/* Closes the controller's node; a scan's waiting client hears that it ended, and a pairing ends as lost. */
static void
btd_close(
	void)
{
	unsigned index;

	/* Nothing open. */
	if (!btd_session_open)
		return;

	/* A pairing cannot go on without the controller (review S-f), and no connection is left. */
	btd_pair_lost(&btd_pairing);
	btd_hid_lost(&btd_hid_host);
	btd_router_clear(&btd_routing);

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

/*
 * Handles the controller's packets, queued ones first, BTD_PACKETS_A_ROUND
 * at most; a node that went is closed.
 */
static void
btd_packets(
	void)
{
	unsigned handled;
	int error;

	/* Packet by packet until none is left or the round is full. */
	for (handled = 0U; handled < BTD_PACKETS_A_ROUND; handled++) {
		error = btd_session_input(&btd_session);
		if (error == EAGAIN)
			return;

		/* The node went. */
		if (error != 0) {
			btd_close();
			return;
		}

		/* A handler that met a reset leaves the session in error (the loop closes it). */
		if (!btd_session_open)
			return;
	}
}

/*
 * Gives the poll's timeout: 0 while packets are queued, else the earliest
 * of the scan's end, the pairing's deadline, the next look for nodes and a
 * booted controller's return (-1: none).
 */
static int
btd_timeout(
	void)
{
	uint64_t now;
	uint64_t earliest;
	uint64_t deadline;
	int pending;

	/* Queued packets are handled at once. */
	if (btd_session_open) {
		pending = btd_session_pending(&btd_session);
		if (pending)
			return 0;
	}

	/* The earliest deadline. */
	now = btd_now_ms();
	earliest = 0U;
	if (btd_session_open && btd_session.scanning)
		earliest = btd_session.scan_end_ms;
	deadline = btd_pair_deadline(&btd_pairing);
	if (deadline != 0U && (earliest == 0U || deadline < earliest))
		earliest = deadline;
	deadline = btd_hid_deadline(&btd_hid_host);
	if (btd_session_open && deadline != 0U && (earliest == 0U || deadline < earliest))
		earliest = deadline;
	if (!btd_session_open && !btd_stopped) {
		deadline = btd_looked_ms + BTD_RETRY_MS;
		if (earliest == 0U || deadline < earliest)
			earliest = deadline;
	}

	/* A booted controller's return. */
	if (btd_reappear_ms != 0U && (earliest == 0U || btd_reappear_ms < earliest))
		earliest = btd_reappear_ms;

	/* None: wait for a descriptor. */
	if (earliest == 0U)
		return -1;

	/* Due already. */
	if (earliest <= now)
		return 0;

	/* Succeeded: the milliseconds to the earliest. */
	return (int)(earliest - now);
}

/* Takes a new client with its uid, when there is a free slot. */
static void
btd_accept(
	int listener)
{
	unsigned index;
	uid_t uid;
	gid_t gid;
	int descriptor;
	int status;

	/* The connection; its reads never block. */
	descriptor = accept(listener, NULL, NULL);
	if (descriptor < 0)
		return;
	(void)fcntl(descriptor, F_SETFL, O_NONBLOCK);
	(void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);

	/* Who it is; a client that cannot be told is nobody's. */
	status = getpeereid(descriptor, &uid, &gid);
	if (status != 0) {
		(void)close(descriptor);
		return;
	}

	/* A free slot. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		if (btd_clients[index].descriptor >= 0)
			continue;
		btd_clients[index].descriptor = descriptor;
		btd_clients[index].uid = uid;
		btd_clients[index].dead = 0;
		btd_clients[index].used = 0U;
		btd_clients[index].waits_scan = 0;
		btd_clients[index].waits_pair = 0;
		return;
	}

	/* Too many clients. */
	(void)close(descriptor);
}

/* Reads a client's bytes and carries out each whole line; a line too long closes the client. */
static void
btd_read(
	int index)
{
	struct btd_client *client;
	ssize_t count;
	size_t length;
	char *end;
	int error;

	/* What came, without waiting. */
	client = &btd_clients[index];
	count = recv(client->descriptor, client->input + client->used, sizeof(client->input) - 1U - client->used, 0);
	if (count < 0) {
		error = errno;
		if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
			return;
	}

	/* A client that went (or failed). */
	if (count <= 0) {
		btd_client_close(index);
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
		btd_line(index, client->input);
		if (client->descriptor < 0)
			return;
		length = (size_t)(end + 1 - client->input);
		memmove(client->input, end + 1, client->used - length + 1U);
		client->used -= length;
	}

	/* A line that fills the buffer without ending is not one. */
	if (client->used + 1U >= sizeof(client->input))
		btd_client_close(index);
}

/* Carries out one request line, or takes an answer to the question asked. */
static void
btd_line(
	int index,
	char *line)
{
	struct btd_client *client;
	int same;

	/* An answer to the agent's question: YES or NO, from the client asked. */
	client = &btd_clients[index];
	same = strcmp(line, "YES");
	if (same == 0) {
		btd_answer(index, 1);
		return;
	}

	/* NO, likewise. */
	same = strcmp(line, "NO");
	if (same == 0) {
		btd_answer(index, 0);
		return;
	}

	/* A client waiting for its scan, its pairing or its connection asks nothing more until it is answered. */
	if (client->waits_scan || client->waits_pair || client->waits_connect)
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

	/* BONDS. */
	same = strcmp(line, "BONDS");
	if (same == 0) {
		btd_bonds(client);
		return;
	}

	/* AGENT. */
	same = strcmp(line, "AGENT");
	if (same == 0) {
		btd_agent(index);
		return;
	}

	/* SCAN SECONDS. */
	same = strncmp(line, "SCAN ", 5U);
	if (same == 0) {
		btd_scan(client, line + 5);
		return;
	}

	/* PAIR ADDRESS TYPE. */
	same = strncmp(line, "PAIR ", 5U);
	if (same == 0) {
		btd_pair(index, line + 5);
		return;
	}

	/* FORGET ADDRESS TYPE. */
	same = strncmp(line, "FORGET ", 7U);
	if (same == 0) {
		btd_forget(client, line + 7);
		return;
	}

	/* CONNECT ADDRESS TYPE (ws143-p005). */
	same = strncmp(line, "CONNECT ", 8U);
	if (same == 0) {
		btd_connect(index, line + 8);
		return;
	}

	/* DISCONNECT ADDRESS TYPE (ws143-p005). */
	same = strncmp(line, "DISCONNECT ", 11U);
	if (same == 0) {
		btd_disconnect(client, line + 11);
		return;
	}

	/* STATUS (ws143-p005). */
	same = strcmp(line, "STATUS");
	if (same == 0) {
		btd_status(client);
		return;
	}

	/* POWER on|off (ws143-p006). */
	same = strncmp(line, "POWER ", 6U);
	if (same == 0) {
		btd_power(client, line + 6);
		return;
	}

	/* Anything else. */
	btd_write(client, "ERROR request\nDONE\n");
}

/*
 * Answers POWER on|off (ws143-p006): those D8 permits turn Bluetooth on or
 * off.  Off is refused while a scan or a pairing runs (ERROR busy); the
 * saved keys stay either way.
 */
static void
btd_power(
	struct btd_client *client,
	const char *argument)
{
	int permitted;
	int pairing;
	int error;
	int on;
	int off;

	/* Only those D8 permits. */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* on or off. */
	on = strcmp(argument, "on");
	off = strcmp(argument, "off");
	if (on != 0 && off != 0) {
		btd_write(client, "ERROR power\nDONE\n");
		return;
	}

	/* On: scans and pairings may start again; kept for the next start. */
	if (on == 0) {
		btd_powered_off = 0;
		error = btd_power_save();
		btd_log("BLUETOOTHD POWER on uid=%u saved=%d\n", (unsigned)client->uid, error == 0);
		btd_write(client, "POWER on\nDONE\n");
		return;
	}

	/* Off waits for no scan or pairing that runs. */
	pairing = btd_pair_active(&btd_pairing);
	if ((btd_session_open && btd_session.scanning) || pairing || btd_pair_client != BTD_NO_CLIENT) {
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* Succeeded: off until POWER on, kept for the next start. */
	btd_powered_off = 1;
	error = btd_power_save();
	btd_log("BLUETOOTHD POWER off uid=%u saved=%d\n", (unsigned)client->uid, error == 0);
	btd_write(client, "POWER off\nDONE\n");
}

/* Reads the user's switch as it was left: off when the file says so, else on. */
static void
btd_power_load(
	void)
{
	char text[8];
	ssize_t got;
	int descriptor;
	int same;

	/* On unless the file says off. */
	btd_powered_off = 0;
	descriptor = open(BTD_POWER_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	if (descriptor < 0)
		return;
	got = read(descriptor, text, sizeof(text) - 1U);
	(void)close(descriptor);
	if (got <= 0)
		return;

	/* Its word. */
	text[got] = '\0';
	same = strncmp(text, "off", 3U);
	if (same == 0) {
		btd_powered_off = 1;
		btd_log("BLUETOOTHD POWER loaded=off\n");
	}
}

/*
 * Keeps the user's switch for the next start: written beside, synced and
 * renamed in place, as the keys are.  Returns 0 or an errno value (the
 * switch holds for this run either way).
 */
static int
btd_power_save(
	void)
{
	const char *text;
	ssize_t written;
	size_t length;
	int descriptor;
	int error;

	/* The word. */
	text = "on\n";
	if (btd_powered_off)
		text = "off\n";

	/* The temporary file (never a link), written and synced. */
	descriptor = open(BTD_POWER_FILE ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return errno;
	length = strlen(text);
	written = write(descriptor, text, length);
	if (written != (ssize_t)length) {
		(void)close(descriptor);
		return EIO;
	}

	/* On the disk before the rename. */
	error = fsync(descriptor);
	(void)close(descriptor);
	if (error != 0)
		return EIO;

	/* In place of the old one (the folder is the keys', synced with them). */
	error = rename(BTD_POWER_FILE ".tmp", BTD_POWER_FILE);
	if (error != 0)
		return errno;

	/* Succeeded: kept. */
	return 0;
}

/* Tells the client last asked a question of a pairing that the question is over (ASK-END). */
static void
btd_question_end(
	void)
{
	int index;

	/* Only a question asked. */
	index = btd_question_client;
	btd_question_client = BTD_NO_CLIENT;
	if (index == BTD_NO_CLIENT || btd_clients[index].descriptor < 0)
		return;

	/* Succeeded: told. */
	btd_write(&btd_clients[index], "ASK-END\n");
}

/* Answers SHOW: the state, and the controller when one is open. */
static void
btd_show(
	struct btd_client *client)
{
	char address[24];
	char name[4U * BT_TEXT_MAX];
	const char *firmware;
	int pairing;
	int error;

	/* The state, and why; a ready controller the user turned off is "off" (ws143-p006). */
	if (btd_powered_off && btd_session_open && btd_session.state == BTD_STATE_READY)
		btd_write(client, "STATE off\n");
	else if (btd_session.reason[0] != '\0')
		btd_write(client, "STATE %s %s\n", btd_state_name(btd_session.state), btd_session.reason);
	else
		btd_write(client, "STATE %s\n", btd_state_name(btd_session.state));

	/* The controller. */
	firmware = "-";
	if (btd_session.firmware_loaded)
		firmware = "loaded";
	pairing = btd_pair_active(&btd_pairing);
	if (btd_session_open) {
		btd_format_address(btd_session.address, address, sizeof(address));
		if (!btd_session.have_address)
			(void)snprintf(address, sizeof(address), "%s", "-");
		error = btd_escape(btd_session.info.name, name, sizeof(name));
		if (error != 0)
			name[0] = '\0';
		btd_write(client,
			  "CONTROLLER node=%s vendor=%04x product=%04x name=\"%s\" address=%s hci=%u manufacturer=%u le=%d p256=%d dhkey=%d firmware=%s ssp=%d sc=%d pairing=%d hid=%u page_scan=%d le_auto=0\n",
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
			  firmware,
			  btd_session.ssp,
			  btd_session.secure_connections,
			  pairing,
			  btd_hid_open_count(&btd_hid_host),
			  btd_hid_host.page_scan);
	}

	/* The user's switch (ws143-p006), whatever the controller's state. */
	if (btd_powered_off) {
		btd_write(client, "POWER off\n");
	} else {
		btd_write(client, "POWER on\n");
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

/* Starts a scan for one who may change things; the client hears the devices when it ends. */
static void
btd_scan(
	struct btd_client *client,
	const char *argument)
{
	unsigned long seconds;
	char *end;
	int permitted;
	int pairing;
	int error;

	/* Only those D8 permits (plan section 6). */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
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

	/* A controller. */
	if (!btd_session_open) {
		btd_write(client, "ERROR no-controller\nDONE\n");
		return;
	}

	/* None while the user has Bluetooth off (ws143-p006). */
	if (btd_powered_off) {
		btd_write(client, "ERROR off\nDONE\n");
		return;
	}

	/* One scan at a time, and none while a pairing runs. */
	pairing = btd_pair_active(&btd_pairing);
	if (btd_session.scanning || pairing) {
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

/*
 * Starts a pairing for one who may change things (PAIR ADDRESS TYPE): the
 * client hears the questions when no agent answers for it, and the end.
 */
static void
btd_pair(
	int index,
	const char *argument)
{
	struct btd_client *client;
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int permitted;
	int busy;
	int error;

	/* Only those D8 permits. */
	client = &btd_clients[index];
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device. */
	error = btd_parse_device(argument, address, &type);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* A ready controller, which the user has not turned off (ws143-p006). */
	if (!btd_session_open || btd_session.state != BTD_STATE_READY) {
		btd_write(client, "ERROR not-ready\nDONE\n");
		return;
	}

	/* None while the user has Bluetooth off. */
	if (btd_powered_off) {
		btd_write(client, "ERROR off\nDONE\n");
		return;
	}

	/* One pairing at a time, none while a HID device's connection is under way (review B7). */
	busy = btd_hid_busy(&btd_hid_host, address);
	if (btd_pair_client != BTD_NO_CLIENT || busy) {
		btd_write(client, "ERROR busy\nDONE\n");
		return;
	}

	/* The client waits for the end (which may come at once). */
	btd_pair_client = index;
	client->waits_pair = 1;
	btd_log("bluetoothd: pairing %s asked by uid %u\n", argument, (unsigned)client->uid);
	error = btd_pair_start(&btd_pairing, address, type, 1);
	if (error == EBUSY) {
		btd_pair_client = BTD_NO_CLIENT;
		client->waits_pair = 0;
		btd_write(client, "ERROR busy\nDONE\n");
	}
}

/*
 * Makes the client the agent (AGENT): it is asked the questions of
 * pairings of its own uid, or of anyone when it is the seat's user.  An
 * agent of another uid is not displaced while it is there.
 */
static void
btd_agent(
	int index)
{
	struct btd_client *client;
	struct btd_client *agent;
	int permitted;

	/* Only those D8 permits. */
	client = &btd_clients[index];
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* Another uid's agent stays (review M-d); the same uid's is replaced. */
	if (btd_agent_client != BTD_NO_CLIENT && btd_agent_client != index) {
		agent = &btd_clients[btd_agent_client];
		if (agent->uid != client->uid) {
			btd_write(client, "ERROR busy\nDONE\n");
			return;
		}

		/* The old one hears that it is no longer the agent. */
		btd_write(agent, "AGENT-END\n");
	}

	/* The agent from now on. */
	btd_agent_client = index;
	btd_write(client, "AGENT ok\nDONE\n");
}

/* Takes YES or NO from the client asked; from anyone else it is passed over. */
static void
btd_answer(
	int index,
	int accepted)
{
	/* Only the client asked. */
	if (btd_asked_client != index)
		return;

	/* Answered; the question is over for the client asked. */
	btd_asked_client = BTD_NO_CLIENT;
	btd_question_end();
	btd_pair_answer(&btd_pairing, accepted);
}

/* Forgets a bond (FORGET ADDRESS TYPE) for one who may change things. */
static void
btd_forget(
	struct btd_client *client,
	const char *argument)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int permitted;
	int error;

	/* Only those D8 permits. */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device. */
	error = btd_parse_device(argument, address, &type);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* The bonds are the controller's: one must be open. */
	if (!btd_session_open || !btd_session.have_address) {
		btd_write(client, "ERROR no-controller\nDONE\n");
		return;
	}

	/* The bond's file, gone. */
	error = btd_keys_forget(BTD_KEYS_FOLDER, btd_session.address, address, type);
	if (error == ENOENT) {
		btd_write(client, "ERROR not-bonded\nDONE\n");
		return;
	}

	/* Any other failure, named. */
	if (error != 0) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(error));
		return;
	}

	/* A HID device's record goes too; a connected one hears the unplug and is disconnected (design section 6.3). */
	if (type == BTD_ADDRESS_BREDR)
		btd_hid_forget(&btd_hid_host, address);

	/* Succeeded: forgotten. */
	btd_log("bluetoothd: forgot %s\n", argument);
	btd_write(client, "DONE\n");
}

/* Answers BONDS: the controller's bonds, without their keys. */
static void
btd_bonds(
	struct btd_client *client)
{
	static struct btd_bond bonds[BTD_BONDS_MAX];
	char address[24];
	char name[4U * BTD_NAME_MAX];
	unsigned count;
	unsigned index;
	int error;

	/* The controller's bonds. */
	count = 0U;
	error = 0;
	if (btd_session_open && btd_session.have_address)
		error = btd_keys_list(BTD_KEYS_FOLDER, btd_session.address, bonds, BTD_BONDS_MAX, &count);
	if (error != 0) {
		btd_write(client, "ERROR %s\nDONE\n", strerror(error));
		return;
	}

	/* Each bond as one line. */
	for (index = 0U; index < count; index++) {
		btd_format_address(bonds[index].address, address, sizeof(address));
		error = btd_escape(bonds[index].name, name, sizeof(name));
		if (error != 0)
			name[0] = '\0';
		btd_write(client,
			  "BOND address=%s type=%s authenticated=%d secure=%d legacy=%d name=\"%s\"\n",
			  address,
			  btd_address_type_name(bonds[index].type),
			  bonds[index].authenticated,
			  bonds[index].secure,
			  bonds[index].legacy,
			  name);
	}

	/* The keys read are not kept. */
	memset(bonds, 0, sizeof(bonds));

	/* The end. */
	btd_write(client, "DONE\n");
}

/*
 * Asks the agent for the pairing (the pairing's hook): the agent when it
 * answers for the pairing's client, else the pairing's client itself.  A
 * question nobody can be asked is answered no.
 */
static void
btd_ask(
	void *context,
	unsigned kind,
	uint32_t number)
{
	struct btd_client *pairer;
	struct btd_client *agent;
	struct stat status;
	char address[24];
	uid_t seat;
	int asked;
	int error;

	UNUSED_PARAMETER(context);

	/* The pairing's client, which may be asked itself. */
	asked = btd_pair_client;
	if (asked == BTD_NO_CLIENT) {
		btd_pair_answer(&btd_pairing, 0);
		return;
	}

	/* Its client record. */
	pairer = &btd_clients[asked];

	/* The agent answers for its own uid, or for anyone when it is the seat's user. */
	if (btd_agent_client != BTD_NO_CLIENT) {
		agent = &btd_clients[btd_agent_client];
		seat = 0;
		error = stat(BTD_SEAT_NODE, &status);
		if (error == 0)
			seat = status.st_uid;
		if (agent->uid == pairer->uid || (seat != 0 && agent->uid == seat))
			asked = btd_agent_client;
	}

	/*
	 * The device being paired and who started the pairing, after the
	 * question's words (ws143-p006: the desktop's window names them, and
	 * a seat's user may be asked for another user's pairing).
	 */
	btd_format_address(btd_pairing.address, address, sizeof(address));
	btd_question_client = asked;

	/* The question: a number to confirm, an agreement, or a passkey to show (no answer). */
	if (kind == BTD_PAIR_ASK_PASSKEY) {
		btd_write(&btd_clients[asked], "PASSKEY %06u address=%s type=%s uid=%u\n", (unsigned)number, address, btd_address_type_name(btd_pairing.type), (unsigned)pairer->uid);
		return;
	}

	/* A question that waits for YES or NO from the client asked. */
	btd_asked_client = asked;
	if (kind == BTD_PAIR_ASK_CONSENT) {
		btd_write(&btd_clients[asked], "CONSENT address=%s type=%s uid=%u\n", address, btd_address_type_name(btd_pairing.type), (unsigned)pairer->uid);
	} else {
		btd_write(&btd_clients[asked], "CONFIRM %06u address=%s type=%s uid=%u\n", (unsigned)number, address, btd_address_type_name(btd_pairing.type), (unsigned)pairer->uid);
	}
}

/* Tells the pairing's client the end (the pairing's hook) and logs it. */
static void
btd_paired(
	void *context,
	const char *answer)
{
	int index;

	UNUSED_PARAMETER(context);

	/* Logged. */
	btd_log("bluetoothd: pairing: %s\n", answer);

	/* No question waits any more (the one asked hears it is over); the client that asked hears the end. */
	btd_asked_client = BTD_NO_CLIENT;
	btd_question_end();
	index = btd_pair_client;
	btd_pair_client = BTD_NO_CLIENT;
	if (index == BTD_NO_CLIENT)
		return;
	btd_clients[index].waits_pair = 0;
	btd_write(&btd_clients[index], "%s\nDONE\n", answer);
}

/*
 * Tells whether a uid may change things (D8): root, the seat's user (the
 * display's owner, not the greeter), or a member of wheel; the greeter
 * never.
 */
static int
btd_permitted(
	uid_t uid)
{
	struct passwd *account;
	struct group *admin;
	struct stat status;
	gid_t groups[BTD_GROUPS_MAX];
	gid_t admin_gid;
	int group_count;
	int index;
	int same;
	int error;

	/* Root. */
	if (uid == 0)
		return 1;

	/* An account the system knows, and not the greeter's. */
	account = getpwuid(uid);
	if (account == NULL)
		return 0;
	same = strcmp(account->pw_name, BTD_GREETER);
	if (same == 0)
		return 0;

	/* The seat's user. */
	error = stat(BTD_SEAT_NODE, &status);
	if (error == 0 && status.st_uid == uid)
		return 1;

	/* wheel's gid; without the group nobody is a member. */
	admin = getgrnam(BTD_ADMIN_GROUP);
	if (admin == NULL)
		return 0;
	admin_gid = admin->gr_gid;

	/* The account's groups (its own and the supplementary ones). */
	memset(groups, 0, sizeof(groups));
	group_count = BTD_GROUPS_MAX;
	(void)getgrouplist(account->pw_name, account->pw_gid, groups, &group_count);
	if (group_count > BTD_GROUPS_MAX)
		group_count = BTD_GROUPS_MAX;

	/* A member of wheel. */
	for (index = 0; index < group_count; index++) {
		if (groups[index] == admin_gid)
			return 1;
	}

	/* Anyone else. */
	return 0;
}

/* Reads "ADDRESS TYPE" (TYPE bredr, le-public or le-random). Returns 0 or EINVAL. */
static int
btd_parse_device(
	const char *text,
	uint8_t *address,
	unsigned *type)
{
	char address_text[18];
	const char *space;
	int error;

	/* The address, then one space, then the type. */
	space = strchr(text, ' ');
	if (space == NULL || space - text != 17)
		return EINVAL;
	memcpy(address_text, text, 17U);
	address_text[17] = '\0';
	error = btd_address_parse(address_text, address);
	if (error != 0)
		return EINVAL;
	error = btd_address_type_parse(space + 1, type);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the device. */
	return 0;
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

	/* A client already closed, or that stopped reading. */
	if (client->descriptor < 0 || client->dead)
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

		/* A client that does not read is closed at the end of the round. */
		client->dead = 1;
		return;
	}
}

/*
 * Closes a client and frees its slot: a pairing it asked for stops, an
 * agent's question is answered no.
 */
static void
btd_client_close(
	int index)
{
	struct btd_client *client;

	/* The descriptor, and the slot. */
	client = &btd_clients[index];
	if (client->descriptor >= 0)
		(void)close(client->descriptor);
	client->descriptor = -1;
	client->dead = 0;
	client->used = 0U;
	client->waits_scan = 0;
	client->waits_pair = 0;
	client->waits_connect = 0;

	/* The agent is gone; a question it was asked is no, and it hears no ASK-END. */
	if (btd_agent_client == index)
		btd_agent_client = BTD_NO_CLIENT;
	if (btd_question_client == index)
		btd_question_client = BTD_NO_CLIENT;
	if (btd_asked_client == index) {
		btd_asked_client = BTD_NO_CLIENT;
		btd_pair_answer(&btd_pairing, 0);
	}

	/* The client that asked for a pairing is gone: the pairing stops (its end has nobody to tell). */
	if (btd_pair_client == index) {
		btd_pair_client = BTD_NO_CLIENT;
		btd_pair_stop(&btd_pairing, "cancelled");
	}
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

/* Fills bytes with the system's random ones (the Security Manager's nonces and passkeys). */
static void
btd_random(
	void *context,
	uint8_t *bytes,
	size_t length)
{
	UNUSED_PARAMETER(context);

	/* The kernel's random source. */
	arc4random_buf(bytes, length);
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

/*
 * Connects a bonded HID device (CONNECT ADDRESS TYPE, ws143-p005): the
 * client waits for the connection's end (CONNECTED or ERROR), unless it is
 * answered at once.  BR/EDR only for now (LE is i03's).
 */
static void
btd_connect(
	int index,
	const char *argument)
{
	struct btd_client *client;
	char answer[BTD_HID_ANSWER_MAX];
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int permitted;
	int answered;
	int error;

	/* Only those D8 permits. */
	client = &btd_clients[index];
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device: BR/EDR's. */
	error = btd_parse_device(argument, address, &type);
	if (error != 0) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* LE's HID devices are not connected yet (i03). */
	if (type != BTD_ADDRESS_BREDR) {
		btd_write(client, "ERROR not-supported\nDONE\n");
		return;
	}

	/* A ready controller, which the user has not turned off. */
	if (!btd_session_open || btd_session.state != BTD_STATE_READY) {
		btd_write(client, "ERROR not-ready\nDONE\n");
		return;
	}

	/* None while the user has Bluetooth off. */
	if (btd_powered_off) {
		btd_write(client, "ERROR off\nDONE\n");
		return;
	}

	/* The connection: answered now, or the client waits for its end. */
	btd_hid_holding();
	answered = btd_hid_connect(&btd_hid_host, address, answer, sizeof(answer));
	if (answered) {
		btd_write(client, "%s\nDONE\n", answer);
		return;
	}

	/* Succeeded: the client waits. */
	client->waits_connect = 1;
	memcpy(client->connect_address, address, BTD_ADDRESS_BYTES);
	btd_log("bluetoothd: connecting %s asked by uid %u\n", argument, (unsigned)client->uid);
}

/* Disconnects a HID device (DISCONNECT ADDRESS TYPE, ws143-p005); it is not wanted back until connected again. */
static void
btd_disconnect(
	struct btd_client *client,
	const char *argument)
{
	uint8_t address[BTD_ADDRESS_BYTES];
	unsigned type;
	int permitted;
	int error;

	/* Only those D8 permits. */
	permitted = btd_permitted(client->uid);
	if (!permitted) {
		btd_write(client, "ERROR permission\nDONE\n");
		return;
	}

	/* The device. */
	error = btd_parse_device(argument, address, &type);
	if (error != 0 || type != BTD_ADDRESS_BREDR) {
		btd_write(client, "ERROR address\nDONE\n");
		return;
	}

	/* Its disconnection. */
	error = btd_hid_disconnect(&btd_hid_host, address);
	if (error != 0) {
		btd_write(client, "ERROR not-connected\nDONE\n");
		return;
	}

	/* Succeeded: disconnecting. */
	btd_log("bluetoothd: disconnecting %s\n", argument);
	btd_write(client, "DONE\n");
}

/* Answers STATUS: a HID line for each device of the HID host. */
static void
btd_status(
	struct btd_client *client)
{
	char line[512];
	unsigned index;

	/* Each device's line. */
	for (index = 0U; index < BTD_HID_MAX; index++) {
		btd_hid_status(&btd_hid_host, index, line, sizeof(line));
		if (line[0] != '\0')
			btd_write(client, "%s\n", line);
	}

	/* Succeeded: the end. */
	btd_write(client, "DONE\n");
}

/* Opens /dev/input/bridge for the HID host, through the privileged parent. */
static int
btd_hid_bridge(
	void *context,
	int *descriptor)
{
	int error;

	UNUSED_PARAMETER(context);

	/* The parent's open. */
	error = btd_privsep_open_bridge(&btd_separation, descriptor);
	if (error != 0)
		return error;

	/* Succeeded: the open. */
	return 0;
}

/* Tells the client waiting for a device's connection its end (the HID host's answer hook). */
static void
btd_hid_told(
	void *context,
	const uint8_t *address,
	const char *line)
{
	struct btd_client *client;
	unsigned index;
	int same;

	UNUSED_PARAMETER(context);

	/* Logged. */
	btd_log("bluetoothd: hid: %s\n", line);

	/* Each client waiting for that device. */
	for (index = 0U; index < BTD_CLIENTS_MAX; index++) {
		client = &btd_clients[index];
		if (client->descriptor < 0 || !client->waits_connect)
			continue;
		same = memcmp(client->connect_address, address, BTD_ADDRESS_BYTES);
		if (same != 0)
			continue;

		/* The end. */
		client->waits_connect = 0;
		btd_write(client, "%s\nDONE\n", line);
	}
}

/* Holds the HID host's pages while a pairing or a scan runs (review B7). */
static void
btd_hid_holding(
	void)
{
	int pairing;
	int held;

	/* A pairing, or a scan. */
	pairing = btd_pair_active(&btd_pairing);
	held = 0;
	if (pairing || btd_session.scanning)
		held = 1;

	/* Succeeded: told. */
	btd_hid_hold(&btd_hid_host, held);
}
