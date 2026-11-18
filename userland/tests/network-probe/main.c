/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A networkd stand-in with a Wi-Fi radio, for the tests of the system bar's
 * network menu (ws035-p013).  QEMU has no Wi-Fi, so this answers on
 * networkd's socket as if it had one.
 *
 * The test moves networkd's socket aside and starts this in its place; it
 * speaks the same protocol (userland/base/net/protocol.h) for the requests
 * the desktop makes:
 *
 *   SUBSCRIBE        the state now and after each change: a wired
 *                    interface em9 online, the radio wlan0, and the Wi-Fi's
 *                    line ("wifi state=... interface=wlan0 ssid=HEX radios=1")
 *   WIFI_LIST        three networks: "Kei Lab" (strong, secured),
 *                    "Cafe Guest" (fair, open), "Neighbor 5G" (weak,
 *                    secured, with no saved profile)
 *   WIFI_CONNECT     joins a listed network with a profile; "Neighbor 5G"
 *                    is refused with ENOENT (no saved profile) until the
 *                    saved profiles changed
 *   WIFI_PROFILES_CHANGED   (ws089-p003: Settings saved a key) from then on
 *                    "Neighbor 5G" has a profile too
 *   WIFI_DISCONNECT, WIFI_DISABLE, WIFI_ENABLE   as networkd's states
 *   WIFI_SCAN_START, WIFI_SCAN_STOP   (ws089-p021) the desktop asking for
 *                    scans, or no longer: answered, and the state's scan=
 *                    follows (no lease runs out here)
 *
 * The state ends, as networkd's does, with the default route's line: the
 * wired interface em9 carries it ("route default=em9", BUG-189).
 *
 * It starts with the Wi-Fi on and not connected, logs each request
 * ("NETPROBE request op=N ssid=S"), and ends after the seconds given
 * (default 300), removing its socket.  JOIN-SECONDS (default 0) makes each
 * join take that long before it is answered, with the state left as it
 * was meanwhile, as a slow radio's join looks to the desktop (BUG-154).
 * REFUSE names a network whose key is wrong: its join is answered EACCES
 * and the Wi-Fi left manual-disconnected, joining nothing else, as networkd
 * does after an explicit join failed (BUG-187); "-" for none.  TELL 1 makes
 * the state say connecting (with the network) while a join waits, as
 * networkd tells its watchers during a join (BUG-185).
 *
 *   network-probe [SECONDS [JOIN-SECONDS [REFUSE [TELL]]]]
 */

#include "userland/base/net/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The most watchers kept, as networkd. */
#define PROBE_WATCHERS		8

/* The network with no saved profile. */
#define PROBE_UNKNOWN		"Neighbor 5G"

/*
 * The stand-in's network: the Wi-Fi's state as networkd names it, the SSID
 * it is on (empty when none), whether the desktop asks for scans, whether
 * the saved profiles changed (which
 * gives "Neighbor 5G" a profile), the seconds a join takes, and the
 * watchers' connections (-1 when free).
 */
struct probe_network {
	const char *wifi;
	char ssid[33];
	int scanning;
	int profiles_changed;
	int join_seconds;
	const char *refuse;
	int tell;
	int watchers[PROBE_WATCHERS];
	uint32_t watcher_ids[PROBE_WATCHERS];
};

/* The one network; only main's loop touches it. */
static struct probe_network probe;

/* Set by SIGTERM and SIGINT: the loop ends. */
static volatile sig_atomic_t probe_stop;

static void probe_signal(int signal_number);
static int probe_listen(void);
static void probe_accept(int listener);
static void probe_answer(int client, const struct networkd_protocol_header *request, const unsigned char *payload);
static void probe_watch(int client, const struct networkd_protocol_header *request);
static void probe_join(int client, const struct networkd_protocol_header *request, const char *ssid);
static void probe_send(int client, uint32_t id, uint32_t opcode, uint32_t status, uint32_t error, const char *output);
static void probe_state(char *text, size_t size);
static void probe_notify(void);
static void probe_hex(const char *text, char *hex, size_t size);

/*
 * Answers on networkd's socket until the time is up or a signal comes.
 */
int
main(
	int count,
	char **arguments)
{
	struct pollfd poll_descriptors[1 + PROBE_WATCHERS];
	time_t end;
	time_t now;
	int listener;
	int seconds;
	int index;
	int ready;

	/* The time it runs. */
	seconds = 300;
	if (count > 1)
		seconds = atoi(arguments[1]);
	end = time(NULL) + seconds;

	/* How long a join takes (none: answered at once). */
	probe.join_seconds = 0;
	if (count > 2)
		probe.join_seconds = atoi(arguments[2]);

	/* The network whose key is wrong ("-": none), and whether a waiting join is told as connecting. */
	probe.refuse = "";
	if (count > 3)
		probe.refuse = arguments[3];
	index = strcmp(probe.refuse, "-");
	if (index == 0)
		probe.refuse = "";
	probe.tell = 0;
	if (count > 4)
		probe.tell = atoi(arguments[4]);

	/* Wi-Fi on, not connected, nobody watching. */
	probe.wifi = "manual-disconnected";
	probe.ssid[0] = '\0';
	for (index = 0; index < PROBE_WATCHERS; index++)
		probe.watchers[index] = -1;

	/* A signal ends the loop; a watcher gone mid-write is not a signal. */
	(void)signal(SIGTERM, probe_signal);
	(void)signal(SIGINT, probe_signal);
	(void)signal(SIGPIPE, SIG_IGN);

	/* The socket. */
	listener = probe_listen();
	if (listener < 0)
		return 1;
	printf("NETPROBE listening\n");
	(void)fflush(stdout);

	/* Each connection and each watcher that goes, until the end. */
	while (!probe_stop) {
		/* The time is up. */
		now = time(NULL);
		if (now >= end)
			break;

		/* The listener and the watchers (to see them go). */
		poll_descriptors[0].fd = listener;
		poll_descriptors[0].events = POLLIN;
		poll_descriptors[0].revents = 0;
		for (index = 0; index < PROBE_WATCHERS; index++) {
			poll_descriptors[1 + index].fd = probe.watchers[index];
			poll_descriptors[1 + index].events = POLLIN;
			poll_descriptors[1 + index].revents = 0;
		}

		/* Half a second at most, so that the end comes on time. */
		ready = poll(poll_descriptors, 1 + PROBE_WATCHERS, 500);
		if (ready <= 0)
			continue;

		/* A new connection. */
		if (poll_descriptors[0].revents != 0)
			probe_accept(listener);

		/* A watcher that went (it sends nothing after SUBSCRIBE). */
		for (index = 0; index < PROBE_WATCHERS; index++) {
			if (probe.watchers[index] < 0 || poll_descriptors[1 + index].revents == 0)
				continue;
			(void)close(probe.watchers[index]);
			probe.watchers[index] = -1;
			printf("NETPROBE watcher gone\n");
			(void)fflush(stdout);
		}
	}

	/* The socket goes with the stand-in. */
	(void)close(listener);
	(void)unlink(NETWORKD_SOCKET);
	printf("NETPROBE end\n");

	/* Succeeded: the time was up. */
	return 0;
}

/* Asks the loop to end. */
static void
probe_signal(
	int signal_number)
{
	/* Any of the two signals. */
	(void)signal_number;
	probe_stop = 1;
}

/* Makes the listening socket at networkd's path (which the test has moved aside). */
static int
probe_listen(
	void)
{
	struct sockaddr_un address;
	int listener;
	int error;

	/* A stream socket. */
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (listener < 0) {
		perror("network-probe: socket");
		return -1;
	}

	/* At networkd's path. */
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", NETWORKD_SOCKET);
	error = bind(listener, (struct sockaddr *)&address, sizeof(address));
	if (error != 0) {
		perror("network-probe: bind");
		(void)close(listener);
		return -1;
	}

	/* Anyone may connect (the desktop's user). */
	(void)chmod(NETWORKD_SOCKET, 0666);
	error = listen(listener, 8);
	if (error != 0) {
		perror("network-probe: listen");
		(void)close(listener);
		return -1;
	}

	/* Succeeded: the stand-in listens. */
	return listener;
}

/* Takes one connection: reads its request and answers it (a watcher is kept). */
static void
probe_accept(
	int listener)
{
	struct networkd_protocol_header request;
	unsigned char payload[NETWORKD_REQUEST_MAX];
	int client;
	int failed;

	/* The connection. */
	client = accept(listener, NULL, NULL);
	if (client < 0)
		return;

	/* Its one request. */
	failed = networkd_protocol_read_frame_timed(client, &request, payload, sizeof(payload), NETWORKD_REQUEST_MAX, 2U);
	if (failed != 0) {
		(void)close(client);
		return;
	}

	/* The answer. */
	probe_answer(client, &request, payload);
}

/* Answers one request, and closes the connection unless it is a watcher's. */
static void
probe_answer(
	int client,
	const struct networkd_protocol_header *request,
	const unsigned char *payload)
{
	struct networkd_field_reader reader;
	struct networkd_field field;
	char state[2048];
	char ssid[33];
	int failed;

	/* The SSID a join names. */
	ssid[0] = '\0';
	networkd_field_reader_init(&reader, payload, request->payload_length);
	for (;;) {
		failed = networkd_field_read(&reader, &field);
		if (failed != 0)
			break;

		/* The SSID field, when it fits. */
		if (field.type == NETWORKD_FIELD_SSID && field.length < sizeof(ssid)) {
			memcpy(ssid, field.value, field.length);
			ssid[field.length] = '\0';
		}
	}

	/* The request, for the test. */
	printf("NETPROBE request op=%u ssid=%s\n", (unsigned)request->opcode, ssid);
	(void)fflush(stdout);

	/* A watcher is kept. */
	if (request->opcode == NETWORKD_OP_SUBSCRIBE) {
		probe_watch(client, request);
		return;
	}

	/* What the request does. */
	switch (request->opcode) {
	case NETWORKD_OP_WIFI_LIST:
		/* Three networks, then the Wi-Fi's line as networkd ends a list. */
		(void)snprintf(state, sizeof(state),
		    "interface=wlan0 scan state=2 generation=7 results=3\n"
		    "interface=wlan0 bss index=0 ssid=4b6569204c6162 bssid=020000000001 channel=6 frequency=2437 rssi=-48 age=120 security=00000034 flags=00000011\n"
		    "interface=wlan0 bss index=1 ssid=43616665204775657374 bssid=020000000002 channel=11 frequency=2462 rssi=-66 age=300 security=00000000 flags=00000001\n"
		    "interface=wlan0 bss index=2 ssid=4e65696768626f72203547 bssid=020000000003 channel=36 frequency=5180 rssi=-81 age=500 security=00000034 flags=00000011\n"
		    "wifi state=%s interface=wlan0\n", probe.wifi);
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, state);
		break;
	case NETWORKD_OP_WIFI_CONNECT:
		probe_join(client, request, ssid);
		break;
	case NETWORKD_OP_WIFI_DISCONNECT:
		probe.wifi = "manual-disconnected";
		probe.ssid[0] = '\0';
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, NULL);
		probe_notify();
		break;
	case NETWORKD_OP_WIFI_DISABLE:
		probe.wifi = "disabled";
		probe.ssid[0] = '\0';
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, NULL);
		probe_notify();
		break;
	case NETWORKD_OP_WIFI_ENABLE:
		probe.wifi = "auto-searching";
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, NULL);
		probe_notify();
		break;
	case NETWORKD_OP_WIFI_SCAN_START:
	case NETWORKD_OP_WIFI_SCAN_STOP:
		/* The desktop asks for scans, or no longer: the watchers see scan= change. */
		probe.scanning = 0;
		if (request->opcode == NETWORKD_OP_WIFI_SCAN_START)
			probe.scanning = 1;
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, NULL);
		probe_notify();
		break;
	case NETWORKD_OP_WIFI_PROFILES_CHANGED:
		/* A key was saved: the network without a profile has one from now on. */
		probe.profiles_changed = 1;
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, NULL);
		break;
	default:
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_ERROR, EINVAL, NULL);
		break;
	}

	/* One request a connection. */
	(void)close(client);
}

/* Keeps a watcher's connection and tells it the state now (a full list refuses it). */
static void
probe_watch(
	int client,
	const struct networkd_protocol_header *request)
{
	char state[2048];
	int index;

	/* A free place. */
	for (index = 0; index < PROBE_WATCHERS; index++) {
		if (probe.watchers[index] < 0)
			break;
	}

	/* None: refused, as networkd refuses a ninth watcher. */
	if (index == PROBE_WATCHERS) {
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_ERROR, EBUSY, NULL);
		(void)close(client);
		return;
	}

	/* The watcher, and its first state. */
	probe.watchers[index] = client;
	probe.watcher_ids[index] = request->request_id;
	probe_state(state, sizeof(state));
	probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, state);
}

/* Joins a network: one with no profile (or no SSID) is refused with ENOENT. */
static void
probe_join(
	int client,
	const struct networkd_protocol_header *request,
	const char *ssid)
{
	int differs;

	/* No profile (the unknown network has one once the saved profiles changed). */
	differs = strcmp(ssid, PROBE_UNKNOWN);
	if ((differs == 0 && probe.profiles_changed == 0) || ssid[0] == '\0') {
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_ERROR, ENOENT, NULL);
		return;
	}

	/* A slow join: the answer and the new state come only after the wait (told as connecting with TELL). */
	if (probe.join_seconds > 0) {
		printf("NETPROBE join waits seconds=%d ssid=%s\n", probe.join_seconds, ssid);
		(void)fflush(stdout);
		if (probe.tell != 0) {
			probe.wifi = "connecting";
			(void)snprintf(probe.ssid, sizeof(probe.ssid), "%s", ssid);
			probe_notify();
		}

		/* The wait. */
		(void)sleep((unsigned)probe.join_seconds);
	}

	/* The network whose key is wrong: refused, and the Wi-Fi left unconnected (nothing else is joined). */
	differs = strcmp(ssid, probe.refuse);
	if (probe.refuse[0] != '\0' && differs == 0) {
		printf("NETPROBE join refused ssid=%s\n", ssid);
		(void)fflush(stdout);
		probe.wifi = "manual-disconnected";
		probe.ssid[0] = '\0';
		probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_ERROR, EACCES, NULL);
		probe_notify();
		return;
	}

	/* Connected, and the watchers are told. */
	probe.wifi = "connected";
	(void)snprintf(probe.ssid, sizeof(probe.ssid), "%s", ssid);
	probe_send(client, request->request_id, request->opcode, NETWORKD_RESULT_OK, 0, NULL);
	probe_notify();
}

/* Sends one frame: its status, its errno value and its text. */
static void
probe_send(
	int client,
	uint32_t id,
	uint32_t opcode,
	uint32_t status,
	uint32_t error,
	const char *output)
{
	struct networkd_protocol_header header;
	struct networkd_field_writer writer;
	unsigned char payload[NETWORKD_RESPONSE_MAX];

	/* The fields. */
	networkd_field_writer_init(&writer, payload, sizeof(payload));
	(void)networkd_field_write_u32(&writer, NETWORKD_FIELD_STATUS, status);
	(void)networkd_field_write_u32(&writer, NETWORKD_FIELD_ERROR, error);
	if (output != NULL)
		(void)networkd_field_write(&writer, NETWORKD_FIELD_OUTPUT, output, strlen(output));

	/* The frame. */
	header.request_id = id;
	header.opcode = opcode;
	header.payload_length = writer.used;
	(void)networkd_protocol_write_frame(client, &header, payload);
}

/* Writes the state a watcher is told, as networkd's watch_state does. */
static void
probe_state(
	char *text,
	size_t size)
{
	char hex[80];
	const char *radio;
	int differs;

	/* The radio's interface is up with an address only while connected. */
	radio = "wlan0 unconfigured offline";
	differs = strcmp(probe.wifi, "connected");
	if (differs == 0)
		radio = "wlan0 static online";

	/* The SSID as hexadecimal ("-" for none). */
	probe_hex(probe.ssid, hex, sizeof(hex));

	/* The loopback, the wired interface, the radio, the Wi-Fi's line and the default route's (through em9). */
	(void)snprintf(text, size, "lo0 static online\nem9 static online\n%s\nwifi state=%s interface=wlan0 ssid=%s radios=1 scan=%d\nroute default=em9\n",
	    radio, probe.wifi, hex, probe.scanning);
}

/* Tells every watcher the state. */
static void
probe_notify(
	void)
{
	char state[2048];
	int index;

	/* The state now. */
	probe_state(state, sizeof(state));

	/* Each watcher. */
	for (index = 0; index < PROBE_WATCHERS; index++) {
		if (probe.watchers[index] >= 0)
			probe_send(probe.watchers[index], probe.watcher_ids[index], NETWORKD_OP_SUBSCRIBE, NETWORKD_RESULT_OK, 0, state);
	}

	/* The change, for the test. */
	printf("NETPROBE notify wifi=%s ssid=%s scan=%d\n", probe.wifi, probe.ssid, probe.scanning);
	(void)fflush(stdout);
}

/* Writes text as lower-case hexadecimal ("-" for an empty text). */
static void
probe_hex(
	const char *text,
	char *hex,
	size_t size)
{
	static const char digits[] = "0123456789abcdef";
	size_t index;

	/* Nothing is "-". */
	if (text[0] == '\0') {
		(void)snprintf(hex, size, "-");
		return;
	}

	/* Two digits a byte. */
	for (index = 0; text[index] != '\0' && index * 2U + 2U < size; index++) {
		hex[index * 2U] = digits[(unsigned char)text[index] >> 4];
		hex[index * 2U + 1U] = digits[(unsigned char)text[index] & 0x0fU];
	}

	/* The text ends after the last pair. */
	hex[index * 2U] = '\0';
}
