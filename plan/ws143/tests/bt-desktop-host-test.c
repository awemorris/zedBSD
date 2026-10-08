/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the desktop's Bluetooth backend (ws143-p006,
 * plan/ws143/phase006/phase.md section 8): libkeiland-backend-zedbsd's
 * bluetooth-zedbsd.c built with its socket at a path of the test's, and a
 * fake bluetoothd on a thread of the same process that speaks the text
 * lines (plan/ws143/phase004 section 5, userland/base/bluetoothd/protocol.h).
 *
 * It checks: no daemon (unreachable, then reached when it starts); the
 * reading (state, controller, the switch, bonds, devices, connections,
 * names with \xNN, kinds, the paired first); the agent; POWER; a pairing
 * with a number to compare (YES), one another user started with a
 * consent (NO), one with a number to type given up; an answer to a
 * question that is over; FORGET; a request asked during a scan, sent when
 * it ends; one request at a time; the role of the agent taken by another
 * program and asked for again before a pairing; a full daemon (the state
 * stays); an older daemon without POWER, CONNECT and STATUS; the daemon
 * gone.
 * usage: bt-desktop-host-test FOLDER   (a new folder for the socket)
 */

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* The backend, with its socket where the test puts it. */
static char bt_test_socket[108];
#define BT_SOCKET_PATH bt_test_socket
#include "userland/desktop/libkeiland-backend-zedbsd/bluetooth-zedbsd.c"

#define FAKE_CLIENTS	8
#define FAKE_BONDS	8

/* One client of the fake daemon: its connection and its line not ended. */
struct fake_client {
	int fd;
	char input[1024];
	size_t used;
	int agent;
	int pairing;
};

/* A bond of the fake daemon. */
struct fake_bond {
	char address[18];
	int used;
};

/*
 * The fake daemon: its listener, its clients, its state (the switch, what
 * it can do, the bonds, a device connected), the pairing going on (its
 * client, kind, the address), the scan going on and until when, what it
 * saw (counts the test reads), and whether to refuse every client (full).
 */
static struct {
	pthread_mutex_t lock;
	pthread_t thread;
	int listener;
	int stop;
	struct fake_client clients[FAKE_CLIENTS];
	int power;
	int has_power;
	int has_status;
	int connected;
	struct fake_bond bonds[FAKE_BONDS];
	int pair_client;
	char pair_address[18];
	int pair_kind;
	int scan_client;
	uint64_t scan_until;
	int scans;
	int agents;
	int connect_in_scan;
	int connects;
	int full;
} fake;

static int failures;

static void check(int good, const char *format, ...);
static uint64_t now_ms(void);
static void fake_write(int client, const char *format, ...);
static void fake_bond_add(const char *address);
static void fake_bond_remove(const char *address);
static int fake_bonded(const char *address);
static void fake_line(int client, char *line);
static void fake_close(int client);
static void *fake_main(void *data);
static void fake_start(void);
static void fake_stop(void);
static unsigned pump(struct kl_backend_bluetooth *bluetooth, unsigned want, unsigned timeout);
static int wait_result(struct kl_backend_bluetooth *bluetooth, uint32_t id, int *error);
static int wait_question(struct kl_backend_bluetooth *bluetooth, struct kl_backend_bluetooth_question *question);
static const struct kl_backend_bluetooth_device *find(const struct kl_backend_bluetooth_device *devices, size_t count, const char *address);

/* Notes a check. */
static void
check(
	int good,
	const char *format,
	...)
{
	va_list arguments;

	/* Passed. */
	if (good)
		return;

	/* Failed: said. */
	failures++;
	va_start(arguments, format);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, format, arguments);
	fprintf(stderr, "\n");
	va_end(arguments);
}

/* The monotonic clock in milliseconds. */
static uint64_t
now_ms(
	void)
{
	struct timespec now;

	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Writes a line (or lines) to a client of the fake daemon. */
static void
fake_write(
	int client,
	const char *format,
	...)
{
	char text[1024];
	va_list arguments;
	int length;

	/* Only a client there. */
	if (client < 0 || fake.clients[client].fd < 0)
		return;
	va_start(arguments, format);
	length = vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	if (length > 0)
		(void)send(fake.clients[client].fd, text, (size_t)length, MSG_NOSIGNAL);
}

/* Adds a bond. */
static void
fake_bond_add(
	const char *address)
{
	int index;

	for (index = 0; index < FAKE_BONDS; index++) {
		if (fake.bonds[index].used)
			continue;
		fake.bonds[index].used = 1;
		(void)snprintf(fake.bonds[index].address, sizeof(fake.bonds[index].address), "%s", address);
		return;
	}
}

/* Removes a bond. */
static void
fake_bond_remove(
	const char *address)
{
	int index;

	for (index = 0; index < FAKE_BONDS; index++) {
		if (fake.bonds[index].used && strcmp(fake.bonds[index].address, address) == 0)
			fake.bonds[index].used = 0;
	}
}

/* Tells whether an address is bonded. */
static int
fake_bonded(
	const char *address)
{
	int index;

	for (index = 0; index < FAKE_BONDS; index++) {
		if (fake.bonds[index].used && strcmp(fake.bonds[index].address, address) == 0)
			return 1;
	}
	return 0;
}

/* The agent's client, or -1. */
static int
fake_agent(
	void)
{
	int index;

	for (index = 0; index < FAKE_CLIENTS; index++) {
		if (fake.clients[index].fd >= 0 && fake.clients[index].agent)
			return index;
	}
	return -1;
}

/* Carries out one line of a client of the fake daemon. */
static void
fake_line(
	int client,
	char *line)
{
	char address[32];
	char type[32];
	int asked;
	int index;
	int previous;

	/* What came, for the one reading a failure. */
	if (getenv("BT_TEST_TRACE") != NULL)
		fprintf(stderr, "fake %d: %s\n", client, line);

	/* SHOW: the state, the controller, the switch. */
	if (strcmp(line, "SHOW") == 0) {
		if (!fake.power)
			fake_write(client, "STATE off\n");
		else if (fake.scan_client >= 0)
			fake_write(client, "STATE scanning\n");
		else
			fake_write(client, "STATE ready\n");
		fake_write(client, "CONTROLLER node=/dev/hci0 vendor=8087 product=0032 name=\"Kei\\x20PC\" address=00:11:22:33:44:55 hci=12 manufacturer=2 le=1 p256=1 dhkey=1 firmware=loaded ssp=1 sc=1 pairing=%d\n",
		    fake.pair_client >= 0);
		if (fake.has_power)
			fake_write(client, "POWER %s\n", fake.power ? "on" : "off");
		fake_write(client, "DONE\n");
		return;
	}

	/* BONDS. */
	if (strcmp(line, "BONDS") == 0) {
		for (index = 0; index < FAKE_BONDS; index++) {
			if (!fake.bonds[index].used)
				continue;
			fake_write(client, "BOND address=%s type=bredr authenticated=1 secure=0 legacy=%d name=\"%s\"\n", fake.bonds[index].address,
			    strcmp(fake.bonds[index].address, "0A:0B:0C:0D:0E:02") == 0,
			    strcmp(fake.bonds[index].address, "0A:0B:0C:0D:0E:02") == 0 ? "Keys" : "");
		}
		fake_write(client, "DONE\n");
		return;
	}

	/* DEVICES: a phone and a keyboard seen. */
	if (strcmp(line, "DEVICES") == 0) {
		fake_write(client, "DEVICE address=0A:0B:0C:0D:0E:01 type=bredr rssi=-40 class=0x5a020c name=\"Phone\\x21\"\n");
		fake_write(client, "DEVICE address=0A:0B:0C:0D:0E:02 type=bredr rssi=-55 class=0x002540 name=\"Keys\"\n");
		fake_write(client, "DEVICE address=0A:0B:0C:0D:0E:05 type=le-random rssi=-70 appearance=0x03c2 name=\"\"\n");
		fake_write(client, "DONE\n");
		return;
	}

	/* STATUS: the keyboard connected with its battery (an older daemon does not know it). */
	if (strcmp(line, "STATUS") == 0) {
		if (!fake.has_status) {
			fake_write(client, "ERROR request\nDONE\n");
			return;
		}
		if (fake.connected)
			fake_write(client, "HID address=0A:0B:0C:0D:0E:02 type=bredr state=open battery=80\n");
		fake_write(client, "DONE\n");
		return;
	}

	/* POWER. */
	if (strncmp(line, "POWER ", 6U) == 0) {
		if (!fake.has_power) {
			fake_write(client, "ERROR request\nDONE\n");
			return;
		}
		fake.power = strcmp(line + 6, "on") == 0;
		fake_write(client, "POWER %s\nDONE\n", fake.power ? "on" : "off");
		return;
	}

	/* AGENT: this client answers; the one before hears AGENT-END. */
	if (strcmp(line, "AGENT") == 0) {
		previous = fake_agent();
		if (previous >= 0 && previous != client) {
			fake.clients[previous].agent = 0;
			fake_write(previous, "AGENT-END\n");
		}
		fake.clients[client].agent = 1;
		fake.agents++;
		fake_write(client, "AGENT ok\nDONE\n");
		return;
	}

	/* SCAN: answered when it ends; a request during it is a mistake of the backend's. */
	if (strncmp(line, "SCAN ", 5U) == 0) {
		fake.scan_client = client;
		fake.scan_until = now_ms() + 400U;
		fake.scans++;
		return;
	}

	/* YES or NO from the agent or the pairing's client: the pairing ends. */
	if ((strcmp(line, "YES") == 0 || strcmp(line, "NO") == 0) && fake.pair_client >= 0) {
		asked = fake_agent();
		if (asked >= 0)
			fake_write(asked, "ASK-END\n");
		if (strcmp(line, "YES") == 0) {
			fake_bond_add(fake.pair_address);
			fake_write(fake.pair_client, "PAIRED address=%s type=bredr authenticated=1 secure=1 legacy=0 key_size=16 stored=1 l2cap=0\nDONE\n", fake.pair_address);
		} else {
			fake_write(fake.pair_client, "ERROR rejected\nDONE\n");
		}
		fake.clients[fake.pair_client].pairing = 0;
		fake.pair_client = -1;
		return;
	}

	/* A device's request: its address and type. */
	address[0] = '\0';
	type[0] = '\0';
	if (sscanf(line, "%*s %31s %31s", address, type) != 2) {
		fake_write(client, "ERROR request\nDONE\n");
		return;
	}

	/* CONNECT and DISCONNECT. */
	if (strncmp(line, "CONNECT ", 8U) == 0 || strncmp(line, "DISCONNECT ", 11U) == 0) {
		if (!fake.has_status) {
			fake_write(client, "ERROR request\nDONE\n");
			return;
		}
		if (fake.scan_client >= 0)
			fake.connect_in_scan++;
		fake.connects++;
		fake.connected = strncmp(line, "CONNECT ", 8U) == 0;
		fake_write(client, "DONE\n");
		return;
	}

	/* FORGET. */
	if (strncmp(line, "FORGET ", 7U) == 0) {
		fake_bond_remove(address);
		fake_write(client, "DONE\n");
		return;
	}

	/* PAIR: off refuses; a question to the agent (or the client): 01 compares a number, 03 asks consent of a pairing root started, 04 shows a number. */
	if (strncmp(line, "PAIR ", 5U) == 0) {
		if (!fake.power) {
			fake_write(client, "ERROR off\nDONE\n");
			return;
		}
		fake.pair_client = client;
		fake.clients[client].pairing = 1;
		(void)snprintf(fake.pair_address, sizeof(fake.pair_address), "%s", address);
		asked = fake_agent();
		if (asked < 0)
			asked = client;
		if (strcmp(address, "0A:0B:0C:0D:0E:03") == 0) {
			fake.pair_kind = 2;
			fake_write(asked, "CONSENT address=%s type=%s uid=0\n", address, type);
		} else if (strcmp(address, "0A:0B:0C:0D:0E:04") == 0) {
			fake.pair_kind = 3;
			fake_write(asked, "PASSKEY 654321 address=%s type=%s uid=%u\n", address, type, (unsigned)getuid());
		} else {
			fake.pair_kind = 1;
			fake_write(asked, "CONFIRM 123456 address=%s type=%s uid=%u\n", address, type, (unsigned)getuid());
		}
		return;
	}

	/* Anything else. */
	fake_write(client, "ERROR request\nDONE\n");
}

/* Closes a client of the fake daemon: a pairing it asked is cancelled, the agent's question ends. */
static void
fake_close(
	int client)
{
	int asked;

	/* Its pairing. */
	if (fake.pair_client == client) {
		fake.pair_client = -1;
		asked = fake_agent();
		if (asked >= 0 && asked != client)
			fake_write(asked, "ASK-END\n");
	}
	if (fake.scan_client == client)
		fake.scan_client = -1;
	(void)close(fake.clients[client].fd);
	memset(&fake.clients[client], 0, sizeof(fake.clients[client]));
	fake.clients[client].fd = -1;
}

/* The fake daemon's thread: its listener and its clients, polled; a scan ended in time. */
static void *
fake_main(
	void *data)
{
	struct pollfd fds[FAKE_CLIENTS + 1];
	char *end;
	ssize_t count;
	size_t length;
	int index;
	int fd;

	(void)data;
	for (;;) {
		/* The descriptors. */
		pthread_mutex_lock(&fake.lock);
		if (fake.stop) {
			pthread_mutex_unlock(&fake.lock);
			return NULL;
		}
		fds[0].fd = fake.listener;
		fds[0].events = POLLIN;
		for (index = 0; index < FAKE_CLIENTS; index++) {
			fds[index + 1].fd = fake.clients[index].fd;
			fds[index + 1].events = POLLIN;
			fds[index + 1].revents = 0;
		}
		pthread_mutex_unlock(&fake.lock);
		(void)poll(fds, FAKE_CLIENTS + 1, 20);

		pthread_mutex_lock(&fake.lock);
		/* A scan that ended. */
		if (fake.scan_client >= 0 && now_ms() >= fake.scan_until) {
			fake_write(fake.scan_client, "DEVICE address=0A:0B:0C:0D:0E:01 type=bredr rssi=-40 class=0x5a020c name=\"Phone\\x21\"\nDONE\n");
			fake.scan_client = -1;
		}

		/* A new client (refused at once while full). */
		if ((fds[0].revents & POLLIN) != 0) {
			fd = accept(fake.listener, NULL, NULL);
			if (fd >= 0 && fake.full) {
				(void)close(fd);
				fd = -1;
			}
			for (index = 0; fd >= 0 && index < FAKE_CLIENTS; index++) {
				if (fake.clients[index].fd >= 0)
					continue;
				memset(&fake.clients[index], 0, sizeof(fake.clients[index]));
				fake.clients[index].fd = fd;
				fd = -1;
			}
			if (fd >= 0)
				(void)close(fd);
		}

		/* Each client's lines. */
		for (index = 0; index < FAKE_CLIENTS; index++) {
			if (fds[index + 1].fd < 0 || fds[index + 1].revents == 0 || fake.clients[index].fd != fds[index + 1].fd)
				continue;
			count = recv(fake.clients[index].fd, fake.clients[index].input + fake.clients[index].used, sizeof(fake.clients[index].input) - 1U - fake.clients[index].used, MSG_DONTWAIT);
			if (count <= 0) {
				fake_close(index);
				continue;
			}
			fake.clients[index].used += (size_t)count;
			fake.clients[index].input[fake.clients[index].used] = '\0';
			for (;;) {
				end = strchr(fake.clients[index].input, '\n');
				if (end == NULL || fake.clients[index].fd < 0)
					break;
				*end = '\0';
				length = (size_t)(end + 1 - fake.clients[index].input);
				fake_line(index, fake.clients[index].input);
				memmove(fake.clients[index].input, end + 1, fake.clients[index].used - length + 1U);
				fake.clients[index].used -= length;
			}
		}
		pthread_mutex_unlock(&fake.lock);
	}
}

/* Starts the fake daemon: its socket, its thread. */
static void
fake_start(
	void)
{
	struct sockaddr_un address;
	int index;

	fake.listener = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", bt_test_socket);
	if (bind(fake.listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(fake.listener, 8) != 0) {
		perror("fake bind");
		exit(1);
	}
	for (index = 0; index < FAKE_CLIENTS; index++)
		fake.clients[index].fd = -1;
	fake.stop = 0;
	fake.pair_client = -1;
	fake.scan_client = -1;
	(void)pthread_create(&fake.thread, NULL, fake_main, NULL);
}

/* Stops the fake daemon: its clients and its listener go (the socket's file stays, refusing). */
static void
fake_stop(
	void)
{
	int index;

	pthread_mutex_lock(&fake.lock);
	fake.stop = 1;
	pthread_mutex_unlock(&fake.lock);
	(void)pthread_join(fake.thread, NULL);
	for (index = 0; index < FAKE_CLIENTS; index++) {
		if (fake.clients[index].fd >= 0)
			fake_close(index);
	}
	(void)close(fake.listener);
	fake.listener = -1;
}

/* Updates the backend until a change of want comes (0: for the whole time); returns the changes seen. */
static unsigned
pump(
	struct kl_backend_bluetooth *bluetooth,
	unsigned want,
	unsigned timeout)
{
	struct timespec pause;
	unsigned changed;
	unsigned seen;
	uint64_t until;

	seen = 0U;
	until = now_ms() + timeout;
	pause.tv_sec = 0;
	pause.tv_nsec = 5000000L;
	while (now_ms() < until) {
		changed = 0U;
		(void)kl_backend_bluetooth_update(bluetooth, &changed);
		seen |= changed;
		if (want != 0U && (changed & want) != 0U)
			break;
		(void)nanosleep(&pause, NULL);
	}
	return seen;
}

/* Waits for the answer of a request; 1 with its errno value. */
static int
wait_result(
	struct kl_backend_bluetooth *bluetooth,
	uint32_t id,
	int *error)
{
	char reason[KL_BACKEND_BT_REASON_MAX];
	uint32_t answered;
	uint64_t until;

	until = now_ms() + 4000U;
	while (now_ms() < until) {
		(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_RESULT, 200U);
		while (kl_backend_bluetooth_take_result(bluetooth, &answered, error, reason, sizeof(reason))) {
			if (answered == id)
				return 1;
		}
	}
	return 0;
}

/* Waits for a question; 1 with it. */
static int
wait_question(
	struct kl_backend_bluetooth *bluetooth,
	struct kl_backend_bluetooth_question *question)
{
	uint64_t until;

	until = now_ms() + 4000U;
	while (now_ms() < until) {
		if (kl_backend_bluetooth_take_question(bluetooth, question))
			return 1;
		(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_QUESTION, 200U);
	}
	return 0;
}

/* Finds a device by its address. */
static const struct kl_backend_bluetooth_device *
find(
	const struct kl_backend_bluetooth_device *devices,
	size_t count,
	const char *address)
{
	size_t index;

	for (index = 0; index < count; index++) {
		if (strcmp(devices[index].address, address) == 0)
			return &devices[index];
	}
	return NULL;
}

int
main(
	int argc,
	char **argv)
{
	struct kl_backend_bluetooth_device devices[KL_BACKEND_BT_DEVICES_MAX];
	struct kl_backend_bluetooth_question question;
	struct kl_backend_bluetooth_state state;
	const struct kl_backend_bluetooth_device *device;
	struct kl_backend_bluetooth *bluetooth;
	struct sockaddr_un address;
	uint32_t id;
	uint32_t other;
	size_t count;
	int answered;
	int error;
	int raw;
	char text[256];
	ssize_t got;

	/* The socket's folder. */
	if (argc != 2) {
		fprintf(stderr, "usage: bt-desktop-host-test FOLDER\n");
		return 2;
	}
	(void)snprintf(bt_test_socket, sizeof(bt_test_socket), "%s/bt.sock", argv[1]);
	(void)pthread_mutex_init(&fake.lock, NULL);

	/* 1. No daemon: unreachable, a request refused. */
	bluetooth = kl_backend_bluetooth_open();
	check(bluetooth != NULL, "open");
	(void)pump(bluetooth, 0U, 200U);
	kl_backend_bluetooth_get_state(bluetooth, &state);
	check(state.reachable == 0U && state.state == KL_BACKEND_BT_ABSENT, "no daemon: unreachable (%u %u)", state.reachable, state.state);
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_POWER_OFF, NULL, 0U, &id) == ENOTCONN, "no daemon: request refused");

	/* 2. The daemon starts: reached within the retry, the whole reading. */
	fake.power = 1;
	fake.has_power = 1;
	fake.has_status = 1;
	fake.connected = 1;
	fake_bond_add("0A:0B:0C:0D:0E:02");
	fake_start();
	kl_backend_bluetooth_set_watching(bluetooth, 1U);
	(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_STATE, 3000U);
	(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_STATE, 3000U);
	kl_backend_bluetooth_get_state(bluetooth, &state);
	check(state.reachable == 1U && state.state == KL_BACKEND_BT_ON && state.power == 1U, "reached: on (%u %u %u)", state.reachable, state.state, state.power);
	check(strcmp(state.name, "Kei PC") == 0 && strcmp(state.address, "00:11:22:33:44:55") == 0, "controller: [%s] [%s]", state.name, state.address);
	check(state.features == (KL_BACKEND_BT_CAN_POWER | KL_BACKEND_BT_CAN_CONNECT), "features %u", state.features);
	count = kl_backend_bluetooth_get_devices(bluetooth, devices, KL_BACKEND_BT_DEVICES_MAX);
	check(count == 3U && devices[0].paired == 1U && strcmp(devices[0].address, "0A:0B:0C:0D:0E:02") == 0, "devices: three, the paired first (%zu)", count);
	device = find(devices, count, "0A:0B:0C:0D:0E:02");
	check(device != NULL && device->legacy == 1U && device->connected == 1U && device->battery == 80 && device->kind == KL_BACKEND_BT_KIND_KEYBOARD && strcmp(device->name, "Keys") == 0,
	    "keyboard: legacy, connected, battery, kind");
	device = find(devices, count, "0A:0B:0C:0D:0E:01");
	check(device != NULL && device->paired == 0U && device->rssi == -40 && device->kind == KL_BACKEND_BT_KIND_PHONE && strcmp(device->name, "Phone!") == 0, "phone: seen, named, kind");
	device = find(devices, count, "0A:0B:0C:0D:0E:05");
	check(device != NULL && device->type == KL_BACKEND_BT_LE_RANDOM && device->kind == KL_BACKEND_BT_KIND_MOUSE && strcmp(device->name, device->address) == 0, "mouse: LE, unnamed, kind");
	check(state.agent == 1U && fake.agents == 1, "agent held (%u, %d)", state.agent, fake.agents);

	/* 3. POWER off and on. */
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_POWER_OFF, NULL, 0U, &id) == 0, "power off asked");
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_POWER_ON, NULL, 0U, &other) == EBUSY, "one request at a time");
	check(wait_result(bluetooth, id, &error) && error == 0, "power off answered");
	(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_STATE, 2000U);
	kl_backend_bluetooth_get_state(bluetooth, &state);
	check(state.state == KL_BACKEND_BT_OFF && state.power == 0U, "off read (%u %u)", state.state, state.power);
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_PAIR, "0A:0B:0C:0D:0E:01", KL_BACKEND_BT_BREDR, &id) == 0, "pair while off asked");
	check(wait_result(bluetooth, id, &error) && error == ENETDOWN, "pair while off: ENETDOWN (%d)", error);
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_POWER_ON, NULL, 0U, &id) == 0, "power on asked");
	check(wait_result(bluetooth, id, &error) && error == 0, "power on answered");
	(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_STATE, 2000U);

	/* 4. A pairing with a number to compare, answered yes by its id. */
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_PAIR, "0A:0B:0C:0D:0E:01", KL_BACKEND_BT_BREDR, &id) == 0, "pair asked");
	check(wait_question(bluetooth, &question) && question.kind == KL_BACKEND_BT_ASK_CONFIRM && question.number == 123456U && question.own == 1U &&
	    strcmp(question.address, "0A:0B:0C:0D:0E:01") == 0 && strcmp(question.name, "Phone!") == 0, "confirm question (%u %u %s)", question.kind, question.number, question.name);
	check(kl_backend_bluetooth_answer(bluetooth, question.id + 7U, 1U) == ENOENT, "an answer to another id refused");
	check(kl_backend_bluetooth_answer(bluetooth, question.id, 1U) == 0, "yes sent");
	check(wait_result(bluetooth, id, &error) && error == 0, "paired");
	other = question.id;
	check(wait_question(bluetooth, &question) && question.kind == KL_BACKEND_BT_ASK_END && question.id == other, "the question ended");
	check(kl_backend_bluetooth_answer(bluetooth, other, 1U) == ENOENT, "an answer after the end refused");
	(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_DEVICES, 2000U);
	count = kl_backend_bluetooth_get_devices(bluetooth, devices, KL_BACKEND_BT_DEVICES_MAX);
	device = find(devices, count, "0A:0B:0C:0D:0E:01");
	check(device != NULL && device->paired == 1U, "the phone is paired");

	/* 5. A pairing root started: consent, the user named, answered no. */
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_PAIR, "0A:0B:0C:0D:0E:03", KL_BACKEND_BT_BREDR, &id) == 0, "pair 03 asked");
	check(wait_question(bluetooth, &question) && question.kind == KL_BACKEND_BT_ASK_CONSENT && question.own == (getuid() == 0U) && strcmp(question.user, "root") == 0,
	    "consent of root's pairing (%u %u %s)", question.kind, question.own, question.user);
	check(kl_backend_bluetooth_answer(bluetooth, question.id, 0U) == 0, "no sent");
	check(wait_result(bluetooth, id, &error) && error == ECONNREFUSED, "rejected (%d)", error);
	(void)wait_question(bluetooth, &question);

	/* 6. A number to type, given up by this program. */
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_PAIR, "0A:0B:0C:0D:0E:04", KL_BACKEND_BT_BREDR, &id) == 0, "pair 04 asked");
	check(wait_question(bluetooth, &question) && question.kind == KL_BACKEND_BT_ASK_PASSKEY && question.number == 654321U, "passkey shown");
	check(kl_backend_bluetooth_answer(bluetooth, question.id, 1U) == ENOENT, "a passkey takes no answer");
	other = question.id;
	check(kl_backend_bluetooth_cancel(bluetooth) == 0, "given up");
	check(wait_result(bluetooth, id, &error) && error == ECANCELED, "cancelled (%d)", error);
	check(wait_question(bluetooth, &question) && question.kind == KL_BACKEND_BT_ASK_END && question.id == other, "the passkey ended");
	check(kl_backend_bluetooth_cancel(bluetooth) == ENOENT, "nothing more to give up");

	/* 7. FORGET. */
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_FORGET, "0A:0B:0C:0D:0E:01", KL_BACKEND_BT_BREDR, &id) == 0, "forget asked");
	check(wait_result(bluetooth, id, &error) && error == 0 && !fake_bonded("0A:0B:0C:0D:0E:01"), "forgotten");
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_FORGET, "0A:0B:0C", KL_BACKEND_BT_BREDR, &id) == EINVAL, "a short address refused");

	/* 8. A request during a scan waits for its end. */
	kl_backend_bluetooth_set_scanning(bluetooth, 1U);
	(void)pump(bluetooth, 0U, 100U);
	check(fake.scans >= 1 && fake.scan_client >= 0, "a scan runs");
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_DISCONNECT, "0A:0B:0C:0D:0E:02", KL_BACKEND_BT_BREDR, &id) == 0, "disconnect asked during the scan");
	check(wait_result(bluetooth, id, &error) && error == 0, "disconnected");
	check(fake.connects == 1 && fake.connect_in_scan == 0, "sent after the scan (%d %d)", fake.connects, fake.connect_in_scan);
	kl_backend_bluetooth_set_scanning(bluetooth, 0U);
	(void)pump(bluetooth, 0U, 1500U);
	count = kl_backend_bluetooth_get_devices(bluetooth, devices, KL_BACKEND_BT_DEVICES_MAX);
	device = find(devices, count, "0A:0B:0C:0D:0E:02");
	check(device != NULL && device->connected == 0U, "the keyboard told disconnected");

	/* 9. Another program takes the agent's role: not taken back until this program pairs. */
	raw = socket(AF_UNIX, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", bt_test_socket);
	check(connect(raw, (struct sockaddr *)&address, sizeof(address)) == 0, "the other program connects");
	(void)send(raw, "AGENT\n", 6U, MSG_NOSIGNAL);
	(void)pump(bluetooth, 0U, 2500U);
	kl_backend_bluetooth_get_state(bluetooth, &state);
	check(state.agent == 0U && fake.agents == 2, "the role went to the other (%u %d)", state.agent, fake.agents);
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_PAIR, "0A:0B:0C:0D:0E:01", KL_BACKEND_BT_BREDR, &id) == 0, "pair asked again");
	check(wait_question(bluetooth, &question) && question.kind == KL_BACKEND_BT_ASK_CONFIRM, "the question came");
	check(fake.agents == 3, "the role asked for again before the pairing (%d)", fake.agents);
	(void)kl_backend_bluetooth_answer(bluetooth, question.id, 1U);
	check(wait_result(bluetooth, id, &error) && error == 0, "paired again");
	got = recv(raw, text, sizeof(text) - 1U, MSG_DONTWAIT);
	if (got > 0) {
		text[got] = '\0';
		check(strstr(text, "AGENT-END") != NULL, "the other program heard AGENT-END");
	}
	(void)close(raw);

	/* 10. A full daemon: the state stays. */
	pthread_mutex_lock(&fake.lock);
	fake.full = 1;
	pthread_mutex_unlock(&fake.lock);
	(void)pump(bluetooth, 0U, 2500U);
	kl_backend_bluetooth_get_state(bluetooth, &state);
	check(state.reachable == 1U && state.state == KL_BACKEND_BT_ON, "full: the state stays (%u %u)", state.reachable, state.state);
	pthread_mutex_lock(&fake.lock);
	fake.full = 0;
	pthread_mutex_unlock(&fake.lock);

	/* 11. The daemon goes: unreachable. */
	fake_stop();
	(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_STATE, 4000U);
	kl_backend_bluetooth_get_state(bluetooth, &state);
	check(state.reachable == 0U, "gone: unreachable");
	kl_backend_bluetooth_close(bluetooth);

	/* 12. An older daemon: no POWER line, no STATUS, CONNECT unknown. */
	(void)snprintf(bt_test_socket, sizeof(bt_test_socket), "%s/bt2.sock", argv[1]);
	fake.has_power = 0;
	fake.has_status = 0;
	fake.power = 1;
	fake_start();
	bluetooth = kl_backend_bluetooth_open();
	kl_backend_bluetooth_set_watching(bluetooth, 1U);
	(void)pump(bluetooth, KL_BACKEND_BT_CHANGED_STATE, 3000U);
	(void)pump(bluetooth, 0U, 300U);
	kl_backend_bluetooth_get_state(bluetooth, &state);
	check(state.reachable == 1U && state.features == 0U, "older daemon: no features (%u)", state.features);
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_CONNECT, "0A:0B:0C:0D:0E:02", KL_BACKEND_BT_BREDR, &id) == ENOTSUP, "older daemon: connect not offered");
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_POWER_OFF, NULL, 0U, &id) == 0, "older daemon: power asked");
	answered = wait_result(bluetooth, id, &error);
	check(answered && error == ENOTSUP, "older daemon: power unknown (%d)", error);
	check(kl_backend_bluetooth_request(bluetooth, KL_BACKEND_BT_POWER_OFF, NULL, 0U, &id) == ENOTSUP, "older daemon: power not asked again");
	kl_backend_bluetooth_close(bluetooth);
	fake_stop();

	/* The result. */
	if (failures != 0) {
		fprintf(stderr, "bt-desktop-host-test: %d failures\n", failures);
		return 1;
	}
	printf("bt-desktop-host-test: PASS\n");
	return 0;
}
