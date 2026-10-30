/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ws005-p028 (BUG-157): the host test of the WLAN common core's terminal
 * error through a retirement.  The sources of src/kern/net/wifi are compiled with
 * WLAN_TESTING against stubs of the kernel (locks, clock, net device,
 * packets) and a radio whose every method succeeds.  The cases:
 *   1. a connection fails with EACCES (the key refused) and the station
 *      is then closed, as a driver's recovery or quiesce does: the status
 *      still says EACCES (before the fix: 0 with the state DOWN, which the
 *      wifi command reports as ENETDOWN);
 *   2. opening the station again clears it;
 *   3. an explicit disconnect after a failed connection reports its own
 *      outcome (terminal error 0), as before;
 *   4. (ws005-p031) a connection fails with EACCES while the radio cannot
 *      delete its keys, so the handshake engine stays failed, and the
 *      driver's recovery then reports a second loss (EIO) for the same
 *      generation: the status still says EACCES (before the fix: EIO).
 *   5. (ws005-p032, BUG-158) a driver's restart request runs the radio's
 *      restart once on the retirement thread's next pass, however many
 *      requests came before it; not while a stop is still owed (until the
 *      stop completes), and never after a teardown cancelled retirement.
 * Prints one line a case and "host-wlan-retire: PASS" or FAIL at the end.
 *
 *   plan/ws005/tests/host-wlan-retire.sh
 */

#include "kern/net/wlan.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/clock.h"
#include "kern/lock.h"

#include <uapi/errno.h>
#include <uapi/wlan.h>

#include <stdio.h>
#include <string.h>

/* The cases that failed. */
static int retire_failures;

/* The error the radio's key deletion reports (case 4 makes the engine's cleanup fail). */
static int retire_key_delete_error;

/* How many times the retirement thread called the radio's restart (case 5). */
static int retire_restart_calls;

/* The clock the station reads: one tick a call, so deadlines are never reached in a case. */
static uint64_t retire_ticks;

static void retire_check(const char *what, int passed);
static int retire_status(struct net_device *device, struct wlan_status_request *status);

/* The kernel's pieces the core calls. */
uint64_t
clock_ticks(void)
{
	return ++retire_ticks;
}

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

int
net_device_ref_live(
	struct net_device *device)
{
	(void)device;
	return 1;
}

void
net_device_release(
	struct net_device *device)
{
	(void)device;
}

int
net_device_set_carrier(
	struct net_device *device,
	int carrier)
{
	device->carrier = (unsigned)carrier;
	return 0;
}

int
net_device_carrier(
	const struct net_device *device)
{
	return (int)device->carrier;
}

void
net_device_tx_error(
	struct net_device *device)
{
	(void)device;
}

void
net_device_receive(
	struct net_device *device,
	struct packet_buf *packet)
{
	(void)device;
	(void)packet;
}

struct packet_buf *
packet_buf_alloc(
	size_t headroom)
{
	(void)headroom;
	return NULL;
}

void
packet_buf_free(
	struct packet_buf *packet)
{
	(void)packet;
}

void *
packet_buf_append(
	struct packet_buf *packet,
	size_t length)
{
	(void)packet;
	(void)length;
	return NULL;
}

/* The radio: every method succeeds at once. */
static int
radio_scan_start(void *context, uint64_t generation, uint32_t step, uint32_t channel, uint64_t deadline)
{
	(void)context; (void)generation; (void)step; (void)channel; (void)deadline;
	return 0;
}

static int
radio_generation(void *context, uint64_t generation)
{
	(void)context; (void)generation;
	return 0;
}

static int
radio_connect(void *context, uint64_t generation, const struct wlan_bss_record *bss, uint64_t deadline)
{
	(void)context; (void)generation; (void)bss; (void)deadline;
	return 0;
}

static int
radio_management(void *context, uint64_t generation, const uint8_t *frame, size_t length, uint64_t deadline)
{
	(void)context; (void)generation; (void)frame; (void)length; (void)deadline;
	return 0;
}

static int
radio_association_set(void *context, uint64_t generation, const uint8_t bssid[6], uint16_t aid, uint64_t deadline)
{
	(void)context; (void)generation; (void)bssid; (void)aid; (void)deadline;
	return 0;
}

static int
radio_association_clear(void *context, uint64_t generation, uint64_t deadline)
{
	(void)context; (void)generation; (void)deadline;
	return 0;
}

static int
radio_frame(void *context, const struct wlan_radio_tx_request *request)
{
	(void)context; (void)request;
	return 0;
}

static int
radio_key_install(void *context, const struct wlan_radio_key_request *request)
{
	(void)context; (void)request;
	return 0;
}

static int
radio_key_delete(void *context, uint64_t generation, enum wlan_radio_key_kind kind, uint8_t index, uint64_t key_generation, uint64_t deadline)
{
	(void)context; (void)generation; (void)kind; (void)index; (void)key_generation; (void)deadline;
	return retire_key_delete_error;
}

static int
radio_keys_activate(void *context, uint64_t generation, uint64_t pairwise, uint64_t group, uint64_t deadline)
{
	(void)context; (void)generation; (void)pairwise; (void)group; (void)deadline;
	return 0;
}

static int
radio_quiesce(void *context)
{
	(void)context;
	return 0;
}

static int
radio_restart(void *context)
{
	(void)context;
	retire_restart_calls++;
	return 0;
}

/* The radio's methods. */
static const struct wlan_radio_ops retire_ops = {
	radio_scan_start,
	radio_generation,
	radio_connect,
	radio_generation,
	radio_management,
	radio_association_set,
	radio_association_clear,
	radio_frame,
	radio_key_install,
	radio_key_delete,
	radio_keys_activate,
	radio_quiesce,
	NULL,
	radio_restart
};

/* Prints one case's verdict. */
static void
retire_check(
	const char *what,
	int passed)
{
	printf("%s: %s\n", what, passed ? "ok" : "FAIL");
	if (!passed)
		retire_failures++;
}

/* Reads the station's status through the ioctl the wifi command uses. */
static int
retire_status(
	struct net_device *device,
	struct wlan_status_request *status)
{
	int error;

	memset(status, 0, sizeof(*status));
	(void)snprintf(status->ifr_name, sizeof(status->ifr_name), "%s", device->name);
	status->version = WLAN_ABI_VERSION;
	status->size = sizeof(*status);
	error = wlan_station_ioctl(device, SIOCGWLANSTATUS, status);
	return error;
}

/*
 * Runs the three cases on one station.
 */
int
main(void)
{
	static const uint8_t hwaddr[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
	static struct net_device device;
	struct wlan_scan_profile profile;
	struct wlan_status_request status;
	struct wlan_disconnect_request disconnect;
	struct wlan_bss_record bss;
	struct wlan_station *station;
	char what[128];
	int error;

	/* A device and a one-channel profile. */
	(void)snprintf(device.name, sizeof(device.name), "%s", "wlan0");
	memcpy(device.hwaddr, hwaddr, sizeof(hwaddr));
	device.hwaddr_len = 6U;
	memset(&profile, 0, sizeof(profile));
	profile.channel_count = 1U;
	profile.channels[0].channel = 6U;
	profile.channels[0].center_frequency_mhz = 2437U;
	memset(&bss, 0, sizeof(bss));
	memcpy(bss.ssid, "Kei Lab", 7U);
	bss.ssid_length = 7U;
	bss.bssid[0] = 0x02;
	bss.bssid[5] = 0x10;
	bss.channel = 6U;

	/* The station, up. */
	error = wlan_station_test_attach(&device, &retire_ops, NULL, &profile, NULL, NULL, &station);
	snprintf(what, sizeof(what), "attach error=%d", error);
	retire_check(what, error == 0);
	if (error != 0) {
		printf("host-wlan-retire: FAIL\n");
		return 1;
	}
	error = wlan_station_open(station);
	retire_check("open", error == 0);

	/* 1. A connection that fails with EACCES, then the station closed (a driver's recovery). */
	error = wlan_station_test_seed_authorized(station, &bss, 5U, 7U);
	retire_check("seed connection", error == 0);
	error = wlan_station_report_link_loss(station, 5U, EACCES);
	snprintf(what, sizeof(what), "link loss EACCES error=%d", error);
	retire_check(what, error == 0);
	error = retire_status(&device, &status);
	snprintf(what, sizeof(what), "before the close: state=%u terminal=%d", status.state, status.terminal_error);
	retire_check(what, error == 0 && status.state == WLAN_STATE_FAILED && status.terminal_error == EACCES);
	error = wlan_station_close(station);
	snprintf(what, sizeof(what), "close error=%d", error);
	retire_check(what, error == 0);
	error = retire_status(&device, &status);
	snprintf(what, sizeof(what), "after the close: state=%u terminal=%d (EACCES kept)", status.state, status.terminal_error);
	retire_check(what, error == 0 && status.state == WLAN_STATE_DOWN && status.terminal_error == EACCES);

	/* 2. Opened again: idle, the old reason gone. */
	error = wlan_station_open(station);
	retire_check("open again", error == 0);
	error = retire_status(&device, &status);
	snprintf(what, sizeof(what), "after the open: state=%u terminal=%d", status.state, status.terminal_error);
	retire_check(what, error == 0 && status.state == WLAN_STATE_IDLE && status.terminal_error == 0);

	/* 3. A failed connection, then an explicit disconnect: its own outcome. */
	error = wlan_station_test_seed_authorized(station, &bss, 20U, 21U);
	retire_check("seed second connection", error == 0);
	error = wlan_station_report_link_loss(station, 20U, EACCES);
	retire_check("second link loss", error == 0);
	memset(&disconnect, 0, sizeof(disconnect));
	(void)snprintf(disconnect.ifr_name, sizeof(disconnect.ifr_name), "%s", device.name);
	disconnect.version = WLAN_ABI_VERSION;
	disconnect.size = sizeof(disconnect);
	error = wlan_station_ioctl(&device, SIOCSWLANDISCONNECT, &disconnect);
	snprintf(what, sizeof(what), "disconnect error=%d", error);
	retire_check(what, error == 0);
	error = retire_status(&device, &status);
	snprintf(what, sizeof(what), "after the disconnect: state=%u terminal=%d", status.state, status.terminal_error);
	retire_check(what, error == 0 && status.state == WLAN_STATE_IDLE && status.terminal_error == 0);

	/* 4. A refused key whose cleanup fails, then the recovery's own loss: the first reason stays. */
	error = wlan_station_test_seed_authorized(station, &bss, 30U, 31U);
	retire_check("seed third connection", error == 0);
	retire_key_delete_error = EIO;
	error = wlan_station_report_link_loss(station, 30U, EACCES);
	snprintf(what, sizeof(what), "third link loss EACCES (cleanup fails) error=%d", error);
	retire_check(what, 1);
	error = retire_status(&device, &status);
	snprintf(what, sizeof(what), "after the refusal: state=%u terminal=%d", status.state, status.terminal_error);
	retire_check(what, error == 0 && status.state == WLAN_STATE_FAILED && status.terminal_error == EACCES);
	error = wlan_station_report_link_loss(station, 30U, EIO);
	snprintf(what, sizeof(what), "recovery link loss EIO error=%d", error);
	retire_check(what, 1);
	error = retire_status(&device, &status);
	snprintf(what, sizeof(what), "after the recovery: state=%u terminal=%d (EACCES kept)", status.state, status.terminal_error);
	retire_check(what, error == 0 && status.state == WLAN_STATE_FAILED && status.terminal_error == EACCES);
	retire_key_delete_error = 0;

	/* 5. Restart requests run on the retirement thread's pass, once, and only when nothing else owns the station. */
	wlan_retirement_run(clock_ticks());
	snprintf(what, sizeof(what), "no request: restarts=%d", retire_restart_calls);
	retire_check(what, retire_restart_calls == 0);
	wlan_station_restart_request(station);
	wlan_station_restart_request(station);
	wlan_retirement_run(clock_ticks());
	wlan_retirement_run(clock_ticks());
	snprintf(what, sizeof(what), "two requests, two passes: restarts=%d", retire_restart_calls);
	retire_check(what, retire_restart_calls == 1);
	wlan_station_stop_request(station);
	wlan_station_restart_request(station);
	wlan_retirement_run(clock_ticks());
	snprintf(what, sizeof(what), "a stop still owed: restarts=%d", retire_restart_calls);
	retire_check(what, retire_restart_calls == 1);
	wlan_station_stop_complete(station, 0);
	wlan_retirement_run(clock_ticks());
	snprintf(what, sizeof(what), "the stop completed: restarts=%d", retire_restart_calls);
	retire_check(what, retire_restart_calls == 2);
	error = wlan_station_stop_cancel(station);
	wlan_station_restart_request(station);
	wlan_retirement_run(clock_ticks());
	snprintf(what, sizeof(what), "after a teardown (cancel error=%d): restarts=%d", error, retire_restart_calls);
	retire_check(what, error == 0 && retire_restart_calls == 2);

	/* The verdict. */
	if (retire_failures != 0) {
		printf("host-wlan-retire: FAIL (%d)\n", retire_failures);
		return 1;
	}
	printf("host-wlan-retire: PASS\n");
	return 0;
}

/* A tick of the test's clock is one millisecond. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	return milliseconds;
}

/* The test's ticks are milliseconds already. */
uint64_t
kern_ticks_to_ms(
	uint64_t ticks)
{
	return ticks;
}

/* Microseconds to the test's millisecond ticks, rounding up. */
uint64_t
kern_us_to_ticks(
	uint64_t microseconds)
{
	return (microseconds + 999U) / 1000U;
}

/* The test's millisecond ticks in nanoseconds. */
uint64_t
kern_ticks_to_ns(
	uint64_t ticks)
{
	return ticks * 1000000U;
}
