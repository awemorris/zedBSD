/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the WLAN core's FullMAC engine and station branch
 * (ws206-p008).
 *
 * The sources of src/kern/net/wifi are compiled with WLAN_TESTING against
 * stubs of the kernel (locks, clock, net device, packets) and a fake FullMAC
 * radio.  The engine is first driven directly, then a FullMAC station is
 * driven through the same entry points a driver and the wifi command use:
 * attach, scan, connect, the firmware's events, the data path, link loss,
 * the deadline, disconnect, close and detach.  A SoftMAC station checks
 * that the new entry points refuse it.  Built with FULLMAC_TEST_THREADS
 * the stubs use real locks and a second thread runs the data path against
 * the connection cycle, for ThreadSanitizer.
 *
 * Prints one line a check and "fullmac-host-test: PASS" or FAIL at the end.
 *
 *   sh plan/ws206/tests/fullmac-host-test.sh
 */

#include "kern/net/wlan.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/net/wifi/wlan-crypto.h"
#include "kern/net/wifi/wlan-fullmac.h"

#include <uapi/errno.h>
#include <uapi/wlan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FULLMAC_TEST_THREADS
#include <pthread.h>
#endif

#define TEST_SSID "Kei Lab"
#define TEST_SSID_LENGTH 7U
#define TEST_PASSPHRASE "correct horse battery"
#define TEST_PASSPHRASE_LENGTH 21U
#define TEST_PACKET_CAPACITY 2048U
#define TEST_THREAD_ROUNDS 200U

/*
 * What the fake FullMAC radio was asked to do, and what it answers.
 *
 * The test reads and arms it between calls; the data thread of the
 * threaded build only counts transmits, under the transmit lock below.
 */
struct fake_radio {
	unsigned scan_starts;
	uint64_t scan_generation;
	uint32_t scan_step;
	uint32_t scan_channel;
	unsigned scan_stops;
	unsigned connects;
	int connect_error;
	struct wlan_fullmac_connect_request last_request;
	unsigned disconnects;
	int disconnect_error;
	unsigned transmits;
	uint64_t transmit_generation;
	size_t transmit_length;
	unsigned transmit_without_carrier;
};

/* The checks that failed. */
static int test_failures;

/* The tick the test clock reports; a case moves it to reach a deadline. */
static uint64_t test_now = 1000U;

/* The fake radio of the FullMAC station. */
static struct fake_radio fake;

/* The FullMAC and SoftMAC devices; static so the station's pointers stay valid. */
static struct net_device fullmac_device;
static struct net_device softmac_device;

/* How many packets the stub network stack received, and the last one's length. */
static unsigned received_packets;
static size_t received_length;

/* The radio methods; filled at run time so that no designated initializer is needed. */
static struct wlan_radio_ops fullmac_ops;
static struct wlan_radio_ops softmac_ops;

/* What the engine's own fake callbacks were asked. */
static unsigned engine_connects;
static int engine_connect_error;
static uint8_t engine_pmk[WLAN_FULLMAC_PMK_LENGTH];
static uint64_t engine_request_generation;
static unsigned engine_port_opens;
static unsigned engine_port_closes;
static int engine_port_error;
static unsigned engine_radio_stops;
static int engine_radio_stop_error;

#ifdef FULLMAC_TEST_THREADS
/*
 * The one lock every stubbed spinlock maps to.
 *
 * It is recursive because the core nests the registry and station locks.
 * Mapping all of them to one lock keeps every happens-before the kernel's
 * locks give, which is what ThreadSanitizer needs to judge the core's own
 * accesses.
 */
static pthread_mutex_t test_spin_mutex;

/* Set once the connection cycle is over, so the data thread stops. */
static volatile int thread_done;
static pthread_mutex_t thread_done_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The generation the data thread uses: the last one the radio connected. */
static uint64_t thread_generation;
#endif

static void check(const char *what, int passed);
static int status_read(struct net_device *device, struct wlan_status_request *status);
static int scan_once(struct wlan_station *station, struct net_device *device);
static int connect_ioctl(struct net_device *device, const char *passphrase, size_t length);
static int disconnect_ioctl(struct net_device *device);
static size_t probe_response_build(uint8_t *frame);
static size_t ethernet_build(uint8_t *frame, uint16_t ethertype);
static int transmit_packet(struct wlan_station *station, uint16_t ethertype);
static uint64_t test_clock(void *context);
static void ops_fill(void);
static void engine_cases(void);
static void station_attach_cases(void);
static struct wlan_station *station_cases(void);
static void softmac_cases(void);
#ifdef FULLMAC_TEST_THREADS
static void thread_cases(struct wlan_station *station);
static void *thread_data_path(void *argument);
#endif

/*
 * Reads the kernel clock.
 */
uint64_t
clock_ticks(
	void)
{
	/* Reports the test's own time. */
	return test_now;
}

/*
 * Initializes a stubbed spinlock.
 */
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

/*
 * Takes a stubbed spinlock.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
#ifdef FULLMAC_TEST_THREADS
	/* Every spinlock is the one recursive test lock. */
	pthread_mutex_lock(&test_spin_mutex);
#endif

	/* Reports no interrupt state to restore. */
	return 0;
}

/*
 * Releases a stubbed spinlock.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
#ifdef FULLMAC_TEST_THREADS
	/* Every spinlock is the one recursive test lock. */
	pthread_mutex_unlock(&test_spin_mutex);
#endif
}

/*
 * Takes a live reference on a device.
 */
int
net_device_ref_live(
	struct net_device *device)
{
	(void)device;

	/* Every test device is live. */
	return 1;
}

/*
 * Drops a device reference.
 */
void
net_device_release(
	struct net_device *device)
{
	(void)device;
}

/*
 * Raises or lowers a device's carrier.
 */
int
net_device_set_carrier(
	struct net_device *device,
	int carrier)
{
	/* Records the carrier the core published. */
	device->carrier = (unsigned)carrier;

	/* Succeeded. */
	return 0;
}

/*
 * Reads a device's carrier.
 */
int
net_device_carrier(
	const struct net_device *device)
{
	/* Reports the carrier last published. */
	return (int)device->carrier;
}

/*
 * Counts a transmit error.
 */
void
net_device_tx_error(
	struct net_device *device)
{
	(void)device;
}

/*
 * Takes a received packet into the stub network stack.
 */
void
net_device_receive(
	struct net_device *device,
	struct packet_buf *packet)
{
	(void)device;

	/* Counts the packet and keeps its length, then frees it. */
	received_packets++;
	received_length = packet->length;
	packet_buf_free(packet);
}

/*
 * Allocates an empty packet.
 */
struct packet_buf *
packet_buf_alloc(
	size_t headroom)
{
	struct packet_buf *packet;

	(void)headroom;

	/* Allocates the packet. */
	packet = calloc(1U, sizeof(*packet));
	if (packet == NULL)
		return NULL;

	/* Allocates its storage. */
	packet->storage = calloc(1U, TEST_PACKET_CAPACITY);
	if (packet->storage == NULL) {
		free(packet);
		return NULL;
	}

	/* Starts the data at the beginning of an empty storage. */
	packet->data = packet->storage;
	packet->capacity = TEST_PACKET_CAPACITY;
	packet->length = 0U;

	/* Succeeded. */
	return packet;
}

/*
 * Frees a packet and its storage.
 */
void
packet_buf_free(
	struct packet_buf *packet)
{
	/* A missing packet has nothing to free. */
	if (packet == NULL)
		return;

	/* Frees the storage, then the packet. */
	free(packet->storage);
	free(packet);
}

/*
 * Grows a packet's data at its tail.
 */
void *
packet_buf_append(
	struct packet_buf *packet,
	size_t length)
{
	uint8_t *tail;

	/* Refuses growth past the storage. */
	if (packet->length + length > packet->capacity)
		return NULL;

	/* Hands out the new tail. */
	tail = packet->data + packet->length;
	packet->length += length;

	/* Succeeded. */
	return tail;
}

/* Starts a firmware scan of one channel. */
static int
fake_scan_start(void *context, uint64_t generation, uint32_t step, uint32_t channel, uint64_t deadline)
{
	(void)context;
	(void)deadline;

	/* Records the channel the core asked for. */
	fake.scan_starts++;
	fake.scan_generation = generation;
	fake.scan_step = step;
	fake.scan_channel = channel;

	/* Succeeded. */
	return 0;
}

/* Stops the firmware scan. */
static int
fake_scan_stop(void *context, uint64_t generation)
{
	(void)context;
	(void)generation;

	/* Counts the barrier. */
	fake.scan_stops++;

	/* Succeeded. */
	return 0;
}

/* Disconnects the firmware. */
static int
fake_disconnect(void *context, uint64_t generation)
{
	(void)context;
	(void)generation;

	/* Counts the barrier and answers what the case armed. */
	fake.disconnects++;

	/* Reports the armed result. */
	return fake.disconnect_error;
}

/* Quiesces the radio. */
static int
fake_quiesce(void *context)
{
	(void)context;

	/* Succeeded. */
	return 0;
}

/* Retries a stop. */
static int
fake_stop_retry(void *context)
{
	(void)context;

	/* Succeeded. */
	return 0;
}

/* Takes a FullMAC connection request. */
static int
fake_fullmac_connect(void *context, const struct wlan_fullmac_connect_request *request)
{
	(void)context;

	/* Copies the request, as a driver copies it into its slot. */
	fake.connects++;
	fake.last_request = *request;
#ifdef FULLMAC_TEST_THREADS
	/* Lets the data thread use the generation from now on. */
	pthread_mutex_lock(&thread_done_mutex);
	thread_generation = request->generation;
	pthread_mutex_unlock(&thread_done_mutex);
#endif

	/* Reports the armed result. */
	return fake.connect_error;
}

/* Queues one Ethernet frame. */
static int
fake_ethernet_transmit(void *context, uint64_t generation, const uint8_t *frame, size_t length)
{
	(void)context;
	(void)frame;

	/*
	 * The core holds the control gate across the call, and every change
	 * of the controlled port is made under that gate, so the carrier must
	 * be up for the whole call.
	 */
	if (fullmac_device.carrier == 0U)
		fake.transmit_without_carrier++;

	/* Records the frame. */
	fake.transmits++;
	fake.transmit_generation = generation;
	fake.transmit_length = length;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_scan_start(void *context, uint64_t generation, uint32_t step, uint32_t channel, uint64_t deadline)
{
	(void)context;
	(void)generation;
	(void)step;
	(void)channel;
	(void)deadline;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_generation(void *context, uint64_t generation)
{
	(void)context;
	(void)generation;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_connect(void *context, uint64_t generation, const struct wlan_bss_record *bss, uint64_t deadline)
{
	(void)context;
	(void)generation;
	(void)bss;
	(void)deadline;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_management(void *context, uint64_t generation, const uint8_t *frame, size_t length, uint64_t deadline)
{
	(void)context;
	(void)generation;
	(void)frame;
	(void)length;
	(void)deadline;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_association_set(void *context, uint64_t generation, const uint8_t bssid[6], uint16_t aid, uint64_t deadline)
{
	(void)context;
	(void)generation;
	(void)bssid;
	(void)aid;
	(void)deadline;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_association_clear(void *context, uint64_t generation, uint64_t deadline)
{
	(void)context;
	(void)generation;
	(void)deadline;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_frame(void *context, const struct wlan_radio_tx_request *request)
{
	(void)context;
	(void)request;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_key_install(void *context, const struct wlan_radio_key_request *request)
{
	(void)context;
	(void)request;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_key_delete(void *context, uint64_t generation, enum wlan_radio_key_kind kind, uint8_t index, uint64_t key_generation, uint64_t deadline)
{
	(void)context;
	(void)generation;
	(void)kind;
	(void)index;
	(void)key_generation;
	(void)deadline;

	/* Succeeded. */
	return 0;
}

/* A SoftMAC method that succeeds at once. */
static int
softmac_keys_activate(void *context, uint64_t generation, uint64_t pairwise, uint64_t group, uint64_t deadline)
{
	(void)context;
	(void)generation;
	(void)pairwise;
	(void)group;
	(void)deadline;

	/* Succeeded. */
	return 0;
}

/* The engine's connect callback. */
static int
engine_fake_connect(void *context, uint64_t generation, const struct wlan_fullmac_connect_request *request)
{
	(void)context;

	/* Keeps the PMK and generation the engine handed over. */
	engine_connects++;
	memcpy(engine_pmk, request->pmk, sizeof(engine_pmk));
	engine_request_generation = generation;

	/* Reports the armed result. */
	return engine_connect_error;
}

/* The engine's port callback. */
static int
engine_fake_authorized_set(void *context, uint64_t generation, int authorized)
{
	(void)context;
	(void)generation;

	/* Counts the opens and closes. */
	if (authorized)
		engine_port_opens++;
	else
		engine_port_closes++;

	/* Reports the armed result. */
	return engine_port_error;
}

/* The engine's radio-stop callback. */
static int
engine_fake_radio_stop(void *context, uint64_t generation)
{
	(void)context;
	(void)generation;

	/* Counts the barrier. */
	engine_radio_stops++;

	/* Reports the armed result. */
	return engine_radio_stop_error;
}

/* The engine's callbacks. */
static const struct wlan_fullmac_ops engine_ops = {
	engine_fake_connect,
	engine_fake_authorized_set,
	engine_fake_radio_stop
};

/* Prints one check's verdict. */
static void
check(
	const char *what,
	int passed)
{
	/* Prints the line and counts a failure. */
	if (passed) {
		printf("%s: ok\n", what);
	} else {
		printf("%s: FAIL\n", what);
		test_failures++;
	}
}

/* Reads a station's status through the ioctl the wifi command uses. */
static int
status_read(
	struct net_device *device,
	struct wlan_status_request *status)
{
	int error;

	/* Describes the request. */
	memset(status, 0, sizeof(*status));
	(void)snprintf(status->ifr_name, sizeof(status->ifr_name), "%s", device->name);
	status->version = WLAN_ABI_VERSION;
	status->size = sizeof(*status);

	/* Asks the core. */
	error = wlan_station_ioctl(device, SIOCGWLANSTATUS, status);

	/* Reports the ioctl's answer. */
	return error;
}

/* Builds a probe response of the test network on channel 6 with WPA2-PSK and CCMP. */
static size_t
probe_response_build(
	uint8_t *frame)
{
	static const uint8_t rsn[] = {
		0x30U, 0x14U,
		0x01U, 0x00U,
		0x00U, 0x0fU, 0xacU, 0x04U,
		0x01U, 0x00U, 0x00U, 0x0fU, 0xacU, 0x04U,
		0x01U, 0x00U, 0x00U, 0x0fU, 0xacU, 0x02U,
		0x00U, 0x00U
	};
	size_t offset;

	/* The header: a probe response from the access point 02:00:00:00:00:10. */
	memset(frame, 0, 128U);
	frame[0] = 0x50U;
	frame[10] = 0x02U;
	frame[15] = 0x10U;
	frame[16] = 0x02U;
	frame[21] = 0x10U;

	/* The fixed fields: a beacon interval of 100 TU and ESS with privacy. */
	frame[32] = 100U;
	frame[34] = 0x11U;

	/* The SSID element. */
	offset = 36U;
	frame[offset++] = 0U;
	frame[offset++] = (uint8_t)TEST_SSID_LENGTH;
	memcpy(frame + offset, TEST_SSID, TEST_SSID_LENGTH);
	offset += TEST_SSID_LENGTH;

	/* The DS parameter element: channel 6. */
	frame[offset++] = 3U;
	frame[offset++] = 1U;
	frame[offset++] = 6U;

	/* The RSN element. */
	memcpy(frame + offset, rsn, sizeof(rsn));
	offset += sizeof(rsn);

	/* Reports the frame length. */
	return offset;
}

/* Builds a minimal Ethernet frame of one EtherType to the station. */
static size_t
ethernet_build(
	uint8_t *frame,
	uint16_t ethertype)
{
	/* Addresses the station from the access point. */
	memset(frame, 0, 60U);
	memcpy(frame, fullmac_device.hwaddr, 6U);
	frame[6] = 0x02U;
	frame[11] = 0x10U;

	/* The EtherType, then a short payload. */
	frame[12] = (uint8_t)(ethertype >> 8);
	frame[13] = (uint8_t)(ethertype & 0xffU);
	frame[14] = 0x45U;

	/* Reports the minimum frame length. */
	return 60U;
}

/* Sends one packet of an EtherType through the station's transmit path. */
static int
transmit_packet(
	struct wlan_station *station,
	uint16_t ethertype)
{
	struct packet_buf *packet;
	uint8_t *data;
	size_t length;
	int error;

	/* Allocates the packet. */
	packet = packet_buf_alloc(0U);
	if (packet == NULL)
		return ENOMEM;

	/* Fills it with one frame. */
	data = packet_buf_append(packet, 60U);
	length = ethernet_build(data, ethertype);
	(void)length;

	/* Hands it to the core, which owns it from here. */
	error = wlan_station_transmit(station, packet);

	/* Reports the core's answer. */
	return error;
}

/* Runs one complete two-channel scan the way a FullMAC driver reports it. */
static int
scan_once(
	struct wlan_station *station,
	struct net_device *device)
{
	struct wlan_scan_request request;
	struct wlan_scan_status_request status;
	uint8_t frame[128];
	size_t length;
	uint32_t step;
	uint64_t generation;
	unsigned starts;
	unsigned stops;
	unsigned pass;
	int error;
	char what[160];

	/* Starts the scan. */
	stops = fake.scan_stops;
	memset(&request, 0, sizeof(request));
	(void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", device->name);
	request.version = WLAN_ABI_VERSION;
	request.size = sizeof(request);
	request.action = WLAN_SCAN_START;
	error = wlan_station_ioctl(device, SIOCSWLANSCAN, &request);
	if (error != 0)
		return error;

	/* Builds the result the firmware returns on channel 6. */
	length = probe_response_build(frame);

	/* Reports each channel as escan does: ready, then results, then complete. */
	for (step = 0U; step < 2U; step++) {
		/* Runs the worker until it asks the firmware for the next channel. */
		starts = fake.scan_starts;
		for (pass = 0U; pass < 4U && fake.scan_starts == starts; pass++)
			wlan_timer_run(test_now);
		generation = fake.scan_generation;
		snprintf(what, sizeof(what), "scan step %u asked: step=%u channel=%u", step, fake.scan_step, fake.scan_channel);
		check(what, fake.scan_step == step);

		/* A result before the firmware accepted the channel is refused by the core. */
		if (fake.scan_channel == 6U) {
			error = wlan_station_report_scan_frame(station, generation, frame, length, -40, 6U);
			snprintf(what, sizeof(what), "scan result before ready error=%d", error);
			check(what, error == ESTALE);
		}

		/* The firmware accepted the channel. */
		error = wlan_station_report_scan_channel_ready(station, generation, step);
		snprintf(what, sizeof(what), "scan ready step %u error=%d", step, error);
		check(what, error == 0);

		/* The result between ready and complete is taken. */
		if (fake.scan_channel == 6U) {
			error = wlan_station_report_scan_frame(station, generation, frame, length, -40, 6U);
			snprintf(what, sizeof(what), "scan result after ready error=%d", error);
			check(what, error == 0);
		}

		/* The firmware finished the channel. */
		error = wlan_station_report_scan_channel_complete(station, generation, step);
		snprintf(what, sizeof(what), "scan complete step %u error=%d", step, error);
		check(what, error == 0);
	}

	/* The last completion publishes the snapshot after the stop barrier. */
	for (pass = 0U; pass < 4U && fake.scan_stops == stops; pass++)
		wlan_timer_run(test_now);
	memset(&status, 0, sizeof(status));
	(void)snprintf(status.ifr_name, sizeof(status.ifr_name), "%s", device->name);
	status.version = WLAN_ABI_VERSION;
	status.size = sizeof(status);
	error = wlan_station_ioctl(device, SIOCGWLANSCAN, &status);
	snprintf(what, sizeof(what), "scan published: error=%d state=%u results=%u", error, status.state, status.result_count);
	check(what, error == 0 && status.state == WLAN_SCAN_COMPLETE && status.result_count == 1U);

	/* Reports a published scan. */
	return 0;
}

/* Asks the station to connect to the test network. */
static int
connect_ioctl(
	struct net_device *device,
	const char *passphrase,
	size_t length)
{
	struct wlan_connect_request request;
	int error;

	/* Describes the request. */
	memset(&request, 0, sizeof(request));
	(void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", device->name);
	request.version = WLAN_ABI_VERSION;
	request.size = sizeof(request);
	memcpy(request.ssid, TEST_SSID, TEST_SSID_LENGTH);
	request.ssid_length = TEST_SSID_LENGTH;
	memcpy(request.passphrase, passphrase, length);
	request.passphrase_length = (uint32_t)length;

	/* Asks the core. */
	error = wlan_station_ioctl(device, SIOCSWLANCONNECT, &request);

	/* Reports the ioctl's answer. */
	return error;
}

/* Asks the station to disconnect. */
static int
disconnect_ioctl(
	struct net_device *device)
{
	struct wlan_disconnect_request request;
	int error;

	/* Describes the request. */
	memset(&request, 0, sizeof(request));
	(void)snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", device->name);
	request.version = WLAN_ABI_VERSION;
	request.size = sizeof(request);

	/* Asks the core. */
	error = wlan_station_ioctl(device, SIOCSWLANDISCONNECT, &request);

	/* Reports the ioctl's answer. */
	return error;
}

/* Reads the test clock. */
static uint64_t
test_clock(
	void *context)
{
	(void)context;

	/* Reports the test's own time. */
	return test_now;
}

/* Fills both radios' method tables. */
static void
ops_fill(
	void)
{
	/* The FullMAC radio: scans, connects itself, sends Ethernet. */
	memset(&fullmac_ops, 0, sizeof(fullmac_ops));
	fullmac_ops.scan_channel_start = fake_scan_start;
	fullmac_ops.scan_stop = fake_scan_stop;
	fullmac_ops.disconnect = fake_disconnect;
	fullmac_ops.quiesce = fake_quiesce;
	fullmac_ops.stop_retry = fake_stop_retry;
	fullmac_ops.fullmac_connect = fake_fullmac_connect;
	fullmac_ops.ethernet_transmit = fake_ethernet_transmit;

	/* The SoftMAC radio: the complete protocol method set. */
	memset(&softmac_ops, 0, sizeof(softmac_ops));
	softmac_ops.scan_channel_start = softmac_scan_start;
	softmac_ops.scan_stop = softmac_generation;
	softmac_ops.connect_start = softmac_connect;
	softmac_ops.disconnect = softmac_generation;
	softmac_ops.management_transmit = softmac_management;
	softmac_ops.association_set = softmac_association_set;
	softmac_ops.association_clear = softmac_association_clear;
	softmac_ops.frame_transmit = softmac_frame;
	softmac_ops.key_install = softmac_key_install;
	softmac_ops.key_delete = softmac_key_delete;
	softmac_ops.keys_activate = softmac_keys_activate;
	softmac_ops.quiesce = fake_quiesce;
}

/* Drives the FullMAC engine directly. */
static void
engine_cases(
	void)
{
	/* IEEE 802.11 (2016) J.4.2: passphrase "password", SSID "IEEE". */
	static const uint8_t ieee_pmk[32] = {
		0xf4U, 0x2cU, 0x6fU, 0xc5U, 0x2dU, 0xf0U, 0xebU, 0xefU,
		0x9eU, 0xbbU, 0x4bU, 0x90U, 0xb3U, 0x8aU, 0x5fU, 0x90U,
		0x2eU, 0x83U, 0xfeU, 0x1bU, 0x13U, 0x5aU, 0x70U, 0xe2U,
		0x3aU, 0xedU, 0x76U, 0x2eU, 0x97U, 0x10U, 0xa1U, 0x2eU
	};
	static const struct wlan_fullmac_ops missing_ops = {
		engine_fake_connect,
		NULL,
		engine_fake_radio_stop
	};
	struct wlan_fullmac_engine engine;
	struct wlan_fullmac_profile profile;
	enum wlan_fullmac_state state;
	int error;
	char what[160];

	/* An incomplete callback set is refused. */
	error = wlan_fullmac_engine_init(&engine, &missing_ops, NULL);
	snprintf(what, sizeof(what), "engine: incomplete ops error=%d", error);
	check(what, error == EINVAL);

	/* A complete set makes an idle engine. */
	error = wlan_fullmac_engine_init(&engine, &engine_ops, NULL);
	state = wlan_fullmac_engine_state(&engine);
	snprintf(what, sizeof(what), "engine: init error=%d state=%d", error, (int)state);
	check(what, error == 0 && state == WLAN_FULLMAC_STATE_IDLE);

	/* The profile of the IEEE test vector. */
	memset(&profile, 0, sizeof(profile));
	memcpy(profile.bss.ssid, "IEEE", 4U);
	profile.bss.ssid_length = 4U;
	profile.bss.bssid[0] = 0x02U;
	profile.bss.channel = 6U;
	profile.passphrase = (const uint8_t *)"password";
	profile.passphrase_length = 8U;
	profile.total_deadline_ticks = 5000U;

	/* A passphrase too short for WPA2 is refused before anything starts. */
	profile.passphrase_length = 7U;
	error = wlan_fullmac_engine_start(&engine, 9U, &profile, 100U);
	state = wlan_fullmac_engine_state(&engine);
	snprintf(what, sizeof(what), "engine: short passphrase error=%d state=%d connects=%u", error, (int)state, engine_connects);
	check(what, error == EINVAL && state == WLAN_FULLMAC_STATE_IDLE && engine_connects == 0U);
	profile.passphrase_length = 8U;

	/* A start hands the radio the PMK of the IEEE vector and keeps no copy. */
	error = wlan_fullmac_engine_start(&engine, 9U, &profile, 100U);
	state = wlan_fullmac_engine_state(&engine);
	snprintf(what, sizeof(what), "engine: start error=%d state=%d generation=%llu", error, (int)state, (unsigned long long)engine_request_generation);
	check(what, error == 0 && state == WLAN_FULLMAC_STATE_CONNECTING && engine_request_generation == 9U);
	check("engine: PMK equals IEEE 802.11 J.4.2", memcmp(engine_pmk, ieee_pmk, sizeof(ieee_pmk)) == 0);
	check("engine: no PMK kept after start", wlan_fullmac_engine_secrets_clear(&engine));
	check("engine: deadline armed", wlan_fullmac_engine_next_deadline(&engine) == 5000U);

	/* A second start while one attempt runs is busy. */
	error = wlan_fullmac_engine_start(&engine, 10U, &profile, 100U);
	snprintf(what, sizeof(what), "engine: start while connecting error=%d", error);
	check(what, error == EBUSY);

	/* Events of another generation are stale, unknown events invalid. */
	error = wlan_fullmac_engine_report(&engine, 8U, WLAN_FULLMAC_EVENT_ASSOCIATED, 200U);
	snprintf(what, sizeof(what), "engine: stale event error=%d", error);
	check(what, error == ESTALE);
	error = wlan_fullmac_engine_report(&engine, 9U, (enum wlan_fullmac_event)7, 200U);
	snprintf(what, sizeof(what), "engine: unknown event error=%d", error);
	check(what, error == EINVAL);

	/* ASSOCIATED, and the same again, leave one associated attempt. */
	error = wlan_fullmac_engine_report(&engine, 9U, WLAN_FULLMAC_EVENT_ASSOCIATED, 200U);
	check("engine: associated", error == 0 && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_ASSOCIATED);
	error = wlan_fullmac_engine_report(&engine, 9U, WLAN_FULLMAC_EVENT_ASSOCIATED, 201U);
	check("engine: repeated associated changes nothing", error == 0 && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_ASSOCIATED);

	/* AUTHORIZED opens the port once; a repeat opens nothing. */
	error = wlan_fullmac_engine_report(&engine, 9U, WLAN_FULLMAC_EVENT_AUTHORIZED, 300U);
	snprintf(what, sizeof(what), "engine: authorized error=%d opens=%u", error, engine_port_opens);
	check(what, error == 0 && engine_port_opens == 1U && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_AUTHORIZED);
	error = wlan_fullmac_engine_report(&engine, 9U, WLAN_FULLMAC_EVENT_AUTHORIZED, 301U);
	check("engine: repeated authorized opens nothing", error == 0 && engine_port_opens == 1U);
	error = wlan_fullmac_engine_report(&engine, 9U, WLAN_FULLMAC_EVENT_ASSOCIATED, 302U);
	check("engine: associated after authorized changes nothing", error == 0 && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_AUTHORIZED);

	/* After authorization the deadline is the firmware's concern. */
	error = wlan_fullmac_engine_timer(&engine, 999999U);
	check("engine: no timeout once authorized", error == 0 && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_AUTHORIZED);
	check("engine: no deadline once authorized", wlan_fullmac_engine_next_deadline(&engine) == 0U);

	/* A loss closes the port and keeps its first reason. */
	error = wlan_fullmac_engine_fail(&engine, 9U, ECONNRESET);
	snprintf(what, sizeof(what), "engine: fail error=%d closes=%u last=%d", error, engine_port_closes, wlan_fullmac_engine_last_error(&engine));
	check(what, error == 0 && engine_port_closes == 1U && wlan_fullmac_engine_last_error(&engine) == ECONNRESET);
	error = wlan_fullmac_engine_fail(&engine, 9U, EIO);
	check("engine: second loss keeps the first reason", error == 0 && wlan_fullmac_engine_last_error(&engine) == ECONNRESET);

	/* A late ASSOCIATED of the lost attempt is stale. */
	error = wlan_fullmac_engine_report(&engine, 9U, WLAN_FULLMAC_EVENT_ASSOCIATED, 400U);
	snprintf(what, sizeof(what), "engine: late event after the loss error=%d", error);
	check(what, error == ESTALE && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_FAILED);

	/* A radio stop that fails keeps the engine failed for a retry. */
	engine_radio_stop_error = EIO;
	error = wlan_fullmac_engine_stop(&engine);
	snprintf(what, sizeof(what), "engine: failed stop error=%d stops=%u last=%d", error, engine_radio_stops, wlan_fullmac_engine_last_error(&engine));
	check(what, error == EIO && engine_radio_stops == 1U && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_FAILED && wlan_fullmac_engine_last_error(&engine) == ECONNRESET);

	/* The retry finishes the stop; the port is not closed twice. */
	engine_radio_stop_error = 0;
	error = wlan_fullmac_engine_stop(&engine);
	snprintf(what, sizeof(what), "engine: retried stop error=%d stops=%u closes=%u", error, engine_radio_stops, engine_port_closes);
	check(what, error == 0 && engine_radio_stops == 2U && engine_port_closes == 1U && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_IDLE);

	/* An idle engine's stop is a no-op. */
	error = wlan_fullmac_engine_stop(&engine);
	check("engine: idle stop", error == 0 && engine_radio_stops == 2U);

	/* The deadline fails an attempt the firmware never authorizes. */
	error = wlan_fullmac_engine_start(&engine, 11U, &profile, 100U);
	check("engine: second start", error == 0);
	error = wlan_fullmac_engine_timer(&engine, 4999U);
	check("engine: timer before the deadline", error == 0 && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_CONNECTING);
	error = wlan_fullmac_engine_timer(&engine, 5000U);
	snprintf(what, sizeof(what), "engine: timer at the deadline error=%d last=%d", error, wlan_fullmac_engine_last_error(&engine));
	check(what, error == ETIMEDOUT && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_FAILED);
	error = wlan_fullmac_engine_stop(&engine);
	check("engine: stop after the deadline stops the radio", error == 0 && engine_radio_stops == 3U);

	/* An event after the deadline cannot rescue the attempt. */
	error = wlan_fullmac_engine_start(&engine, 12U, &profile, 100U);
	error = wlan_fullmac_engine_report(&engine, 12U, WLAN_FULLMAC_EVENT_AUTHORIZED, 6000U);
	snprintf(what, sizeof(what), "engine: authorized after the deadline error=%d opens=%u", error, engine_port_opens);
	check(what, error == ETIMEDOUT && engine_port_opens == 1U && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_FAILED);
	(void)wlan_fullmac_engine_stop(&engine);

	/* The firmware may report AUTHORIZED without ASSOCIATED first. */
	error = wlan_fullmac_engine_start(&engine, 13U, &profile, 100U);
	error = wlan_fullmac_engine_report(&engine, 13U, WLAN_FULLMAC_EVENT_AUTHORIZED, 200U);
	check("engine: authorized straight from connecting", error == 0 && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_AUTHORIZED);
	(void)wlan_fullmac_engine_stop(&engine);

	/* A port that cannot open fails the attempt, and stop still closes it. */
	engine_port_error = EIO;
	error = wlan_fullmac_engine_start(&engine, 14U, &profile, 100U);
	error = wlan_fullmac_engine_report(&engine, 14U, WLAN_FULLMAC_EVENT_AUTHORIZED, 200U);
	check("engine: port open fails", error == EIO && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_FAILED);
	engine_port_error = 0;
	engine_port_closes = 0U;
	error = wlan_fullmac_engine_stop(&engine);
	check("engine: stop closes the half-open port", error == 0 && engine_port_closes == 1U);

	/* A radio that refuses the connection fails the attempt; the stop is still owed. */
	engine_connect_error = EBUSY;
	engine_radio_stops = 0U;
	error = wlan_fullmac_engine_start(&engine, 15U, &profile, 100U);
	snprintf(what, sizeof(what), "engine: connect refused error=%d last=%d", error, wlan_fullmac_engine_last_error(&engine));
	check(what, error == EBUSY && wlan_fullmac_engine_state(&engine) == WLAN_FULLMAC_STATE_FAILED && wlan_fullmac_engine_last_error(&engine) == EBUSY);
	check("engine: no PMK kept after a refused connect", wlan_fullmac_engine_secrets_clear(&engine));
	engine_connect_error = 0;
	error = wlan_fullmac_engine_stop(&engine);
	check("engine: stop after a refused connect stops the radio", error == 0 && engine_radio_stops == 1U);

	/* A loss of an idle engine has nothing to end. */
	error = wlan_fullmac_engine_fail(&engine, 15U, EIO);
	check("engine: loss of an idle engine", error == ESTALE || error == ENOTCONN);
}

/* Checks the mode rules of attach. */
static void
station_attach_cases(
	void)
{
	struct wlan_radio_ops mixed;
	struct wlan_scan_profile profile;
	struct wlan_station *station;
	int error;
	char what[160];

	/* A two-channel profile that the firmware times. */
	memset(&profile, 0, sizeof(profile));
	profile.channel_count = 2U;
	profile.channels[0].channel = 1U;
	profile.channels[0].center_frequency_mhz = 2412U;
	profile.channels[0].flags = WLAN_SCAN_CHANNEL_OFFLOADED_DWELL;
	profile.channels[1].channel = 6U;
	profile.channels[1].center_frequency_mhz = 2437U;
	profile.channels[1].flags = WLAN_SCAN_CHANNEL_OFFLOADED_DWELL;

	/* A FullMAC radio with a SoftMAC method mixed in is refused. */
	mixed = fullmac_ops;
	mixed.connect_start = softmac_connect;
	error = wlan_station_test_attach(&fullmac_device, &mixed, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "attach: mixed methods error=%d", error);
	check(what, error == EINVAL);

	/* A FullMAC radio without Ethernet transmit is refused. */
	mixed = fullmac_ops;
	mixed.ethernet_transmit = NULL;
	error = wlan_station_test_attach(&fullmac_device, &mixed, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "attach: no ethernet_transmit error=%d", error);
	check(what, error == EINVAL);

	/* A FullMAC radio without a disconnect barrier is refused. */
	mixed = fullmac_ops;
	mixed.disconnect = NULL;
	error = wlan_station_test_attach(&fullmac_device, &mixed, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "attach: no disconnect error=%d", error);
	check(what, error == EINVAL);

	/* A SoftMAC radio with Ethernet transmit is refused. */
	mixed = softmac_ops;
	mixed.ethernet_transmit = fake_ethernet_transmit;
	error = wlan_station_test_attach(&softmac_device, &mixed, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "attach: SoftMAC with ethernet_transmit error=%d", error);
	check(what, error == EINVAL);

	/* A FullMAC channel the core would have to time itself is refused. */
	profile.channels[1].flags = 0U;
	error = wlan_station_test_attach(&fullmac_device, &fullmac_ops, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "attach: FullMAC software-dwell channel error=%d", error);
	check(what, error == EINVAL);

	/* An active FullMAC channel needs a management transmit it cannot have. */
	profile.channels[1].flags = WLAN_SCAN_CHANNEL_OFFLOADED_DWELL | WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED;
	error = wlan_station_test_attach(&fullmac_device, &fullmac_ops, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "attach: FullMAC active channel error=%d", error);
	check(what, error == EINVAL);
}

/* Drives a FullMAC station through a whole connection life. */
static struct wlan_station *
station_cases(
	void)
{
	static const uint8_t hwaddr[6] = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U };
	struct wlan_scan_profile profile;
	struct wlan_status_request status;
	struct wlan_station_test_snapshot snapshot;
	struct wlan_radio_rx_frame rx;
	struct wlan_station *station;
	uint8_t expected_pmk[32];
	uint8_t frame[64];
	uint64_t generation;
	size_t length;
	unsigned received_before;
	int error;
	char what[200];

	/* The device and a two-channel profile the firmware times. */
	(void)snprintf(fullmac_device.name, sizeof(fullmac_device.name), "%s", "wlan0");
	memcpy(fullmac_device.hwaddr, hwaddr, sizeof(hwaddr));
	fullmac_device.hwaddr_len = 6U;
	memset(&profile, 0, sizeof(profile));
	profile.channel_count = 2U;
	profile.channels[0].channel = 1U;
	profile.channels[0].center_frequency_mhz = 2412U;
	profile.channels[0].flags = WLAN_SCAN_CHANNEL_OFFLOADED_DWELL;
	profile.channels[1].channel = 6U;
	profile.channels[1].center_frequency_mhz = 2437U;
	profile.channels[1].flags = WLAN_SCAN_CHANNEL_OFFLOADED_DWELL;

	/* The station attaches in FullMAC mode. */
	error = wlan_station_test_attach(&fullmac_device, &fullmac_ops, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "station: attach error=%d", error);
	check(what, error == 0);
	if (error != 0)
		return NULL;
	(void)wlan_station_test_snapshot(station, &snapshot);
	check("station: snapshot says FullMAC", snapshot.fullmac == 1U);

	/* Brought up, it scans the firmware's way. */
	error = wlan_station_open(station);
	check("station: open", error == 0);
	(void)scan_once(station, &fullmac_device);
	check("station: scan stopped once", fake.scan_stops == 1U);

	/* The SoftMAC entry points refuse a FullMAC station. */
	memset(&rx, 0, sizeof(rx));
	rx.generation = 1U;
	rx.channel = 6U;
	rx.frame = frame;
	rx.length = 24U;
	memset(frame, 0, sizeof(frame));
	frame[0] = 0x08U;
	error = wlan_station_report_frame(station, &rx);
	snprintf(what, sizeof(what), "station: report_frame error=%d", error);
	check(what, error == EOPNOTSUPP);
	error = wlan_station_report_tx_complete(station, 1U, 1U, 1, 0);
	snprintf(what, sizeof(what), "station: report_tx_complete error=%d", error);
	check(what, error == EOPNOTSUPP);
	error = wlan_station_test_begin_group_rekey(station);
	snprintf(what, sizeof(what), "station: SoftMAC rekey test hook error=%d", error);
	check(what, error == EOPNOTSUPP);

	/* A connect hands the firmware the BSS and the PMK of the passphrase. */
	error = connect_ioctl(&fullmac_device, TEST_PASSPHRASE, TEST_PASSPHRASE_LENGTH);
	snprintf(what, sizeof(what), "station: connect ioctl error=%d", error);
	check(what, error == 0);
	wlan_timer_run(test_now);
	error = wlan_pbkdf2_hmac_sha1(TEST_PASSPHRASE, TEST_PASSPHRASE_LENGTH, TEST_SSID, TEST_SSID_LENGTH, 4096U, expected_pmk, sizeof(expected_pmk));
	generation = fake.last_request.generation;
	snprintf(what, sizeof(what), "station: radio got the connection: connects=%u generation=%llu channel=%u", fake.connects, (unsigned long long)generation, fake.last_request.bss.channel);
	check(what, error == 0 && fake.connects == 1U && generation != 0U && fake.last_request.bss.channel == 6U && fake.last_request.bss.bssid[5] == 0x10U);
	check("station: radio got the PMK of the passphrase", memcmp(fake.last_request.pmk, expected_pmk, sizeof(expected_pmk)) == 0);
	check("station: deadline handed over", fake.last_request.deadline_ticks == test_now + WLAN_CONNECT_DEADLINE_TICKS);
	check("station: no secret left in the core", wlan_station_test_secrets_clear(station));
	error = status_read(&fullmac_device, &status);
	snprintf(what, sizeof(what), "station: connecting: state=%u port=%u carrier=%u", status.state, status.controlled_port, fullmac_device.carrier);
	check(what, error == 0 && status.state == WLAN_STATE_ASSOCIATING && status.controlled_port == 0U && fullmac_device.carrier == 0U);

	/* The data path is closed until the firmware authorizes. */
	length = ethernet_build(frame, 0x0800U);
	error = wlan_station_report_ethernet(station, generation, frame, length);
	snprintf(what, sizeof(what), "station: receive before authorized error=%d", error);
	check(what, error == EACCES && received_packets == 0U);
	error = transmit_packet(station, 0x0800U);
	snprintf(what, sizeof(what), "station: transmit before authorized error=%d", error);
	check(what, error == ENETDOWN && fake.transmits == 0U);

	/* ASSOCIATED shows as the four-way phase. */
	error = wlan_station_report_fullmac_event(station, generation + 7U, WLAN_FULLMAC_EVENT_ASSOCIATED);
	snprintf(what, sizeof(what), "station: event of another generation error=%d", error);
	check(what, error == ESTALE);
	error = wlan_station_report_fullmac_event(station, generation, WLAN_FULLMAC_EVENT_ASSOCIATED);
	(void)status_read(&fullmac_device, &status);
	snprintf(what, sizeof(what), "station: associated error=%d state=%u associated=%u", error, status.state, status.associated);
	check(what, error == 0 && status.state == WLAN_STATE_FOUR_WAY && status.associated == 1U && status.authenticated == 1U);

	/* AUTHORIZED opens the controlled port; no beacon watch is armed. */
	error = wlan_station_report_fullmac_event(station, generation, WLAN_FULLMAC_EVENT_AUTHORIZED);
	(void)status_read(&fullmac_device, &status);
	(void)wlan_station_test_snapshot(station, &snapshot);
	snprintf(what, sizeof(what), "station: authorized error=%d state=%u port=%u carrier=%u", error, status.state, status.controlled_port, fullmac_device.carrier);
	check(what, error == 0 && status.state == WLAN_STATE_CONNECTED && status.controlled_port == 1U && status.key_installed == 1U && fullmac_device.carrier == 1U);
	check("station: no beacon watch and no deadline once connected", snapshot.beacon_watch_deadline == 0U && snapshot.connection_deadline == 0U);

	/* Data now passes both ways, but never EAPOL and never an old generation. */
	error = wlan_station_report_ethernet(station, generation, frame, length);
	snprintf(what, sizeof(what), "station: receive error=%d received=%u", error, received_packets);
	check(what, error == 0 && received_packets == 1U && received_length == length);
	received_before = received_packets;
	length = ethernet_build(frame, 0x888eU);
	error = wlan_station_report_ethernet(station, generation, frame, length);
	snprintf(what, sizeof(what), "station: EAPOL consumed error=%d received=%u", error, received_packets);
	check(what, error == 0 && received_packets == received_before);
	length = ethernet_build(frame, 0x0800U);
	error = wlan_station_report_ethernet(station, generation - 1U, frame, length);
	snprintf(what, sizeof(what), "station: receive of an old generation error=%d", error);
	check(what, error == ESTALE && received_packets == received_before);
	error = wlan_station_report_ethernet(station, generation, frame, 13U);
	check("station: receive of a runt frame", error == EMSGSIZE);
	error = transmit_packet(station, 0x0800U);
	snprintf(what, sizeof(what), "station: transmit error=%d transmits=%u generation=%llu length=%zu", error, fake.transmits, (unsigned long long)fake.transmit_generation, fake.transmit_length);
	check(what, error == 0 && fake.transmits == 1U && fake.transmit_generation == generation && fake.transmit_length == 60U);
	error = transmit_packet(station, 0x888eU);
	snprintf(what, sizeof(what), "station: EAPOL transmit refused error=%d", error);
	check(what, error == EACCES && fake.transmits == 1U);

	/* A link loss closes the port, stops the radio, and reports its reason. */
	error = wlan_station_report_link_loss(station, generation, ECONNRESET);
	(void)status_read(&fullmac_device, &status);
	(void)wlan_station_test_snapshot(station, &snapshot);
	snprintf(what, sizeof(what), "station: link loss error=%d state=%u terminal=%d carrier=%u disconnects=%u engine=%u", error, status.state, status.terminal_error, fullmac_device.carrier, fake.disconnects, snapshot.wpa_state);
	check(what, error == 0 && status.state == WLAN_STATE_FAILED && status.terminal_error == ECONNRESET && fullmac_device.carrier == 0U && fake.disconnects == 1U && snapshot.connect_driver_active == 0U && snapshot.wpa_state == WLAN_FULLMAC_STATE_IDLE);
	error = wlan_station_report_fullmac_event(station, generation, WLAN_FULLMAC_EVENT_ASSOCIATED);
	snprintf(what, sizeof(what), "station: late event after the loss error=%d", error);
	check(what, error == ESTALE);

	/* A connection the firmware never authorizes fails at the 30-second deadline. */
	error = connect_ioctl(&fullmac_device, TEST_PASSPHRASE, TEST_PASSPHRASE_LENGTH);
	check("station: second connect", error == 0);
	wlan_timer_run(test_now);
	generation = fake.last_request.generation;
	test_now += WLAN_CONNECT_DEADLINE_TICKS;
	wlan_timer_run(test_now);
	test_now += 1U;
	wlan_timer_run(test_now);
	(void)status_read(&fullmac_device, &status);
	(void)wlan_station_test_snapshot(station, &snapshot);
	snprintf(what, sizeof(what), "station: deadline: state=%u terminal=%d disconnects=%u engine=%u", status.state, status.terminal_error, fake.disconnects, snapshot.wpa_state);
	check(what, status.state == WLAN_STATE_FAILED && status.terminal_error == ETIMEDOUT && fake.disconnects == 2U && snapshot.connect_driver_active == 0U && snapshot.wpa_state == WLAN_FULLMAC_STATE_IDLE);

	/* A refused key whose disconnect fails keeps EACCES through the recovery's loss (BUG-157). */
	error = connect_ioctl(&fullmac_device, TEST_PASSPHRASE, TEST_PASSPHRASE_LENGTH);
	check("station: third connect", error == 0);
	wlan_timer_run(test_now);
	generation = fake.last_request.generation;
	(void)wlan_station_report_fullmac_event(station, generation, WLAN_FULLMAC_EVENT_ASSOCIATED);
	fake.disconnect_error = EIO;
	error = wlan_station_report_link_loss(station, generation, EACCES);
	(void)status_read(&fullmac_device, &status);
	snprintf(what, sizeof(what), "station: refused key, disconnect fails: error=%d state=%u terminal=%d", error, status.state, status.terminal_error);
	check(what, status.state == WLAN_STATE_FAILED && status.terminal_error == EACCES);
	error = wlan_station_report_link_loss(station, generation, EIO);
	(void)status_read(&fullmac_device, &status);
	snprintf(what, sizeof(what), "station: recovery loss: state=%u terminal=%d", status.state, status.terminal_error);
	check(what, status.state == WLAN_STATE_FAILED && status.terminal_error == EACCES);
	fake.disconnect_error = 0;
	test_now += 2U;
	wlan_timer_run(test_now);
	(void)status_read(&fullmac_device, &status);
	(void)wlan_station_test_snapshot(station, &snapshot);
	snprintf(what, sizeof(what), "station: retried stop: state=%u terminal=%d driver=%u engine=%u", status.state, status.terminal_error, snapshot.connect_driver_active, snapshot.wpa_state);
	check(what, status.state == WLAN_STATE_FAILED && status.terminal_error == EACCES && snapshot.connect_driver_active == 0U && snapshot.wpa_state == WLAN_FULLMAC_STATE_IDLE);

	/* An explicit disconnect of a connected station ends idle with its own outcome. */
	error = connect_ioctl(&fullmac_device, TEST_PASSPHRASE, TEST_PASSPHRASE_LENGTH);
	wlan_timer_run(test_now);
	generation = fake.last_request.generation;
	(void)wlan_station_report_fullmac_event(station, generation, WLAN_FULLMAC_EVENT_AUTHORIZED);
	(void)status_read(&fullmac_device, &status);
	check("station: fourth connection authorized", status.state == WLAN_STATE_CONNECTED && fullmac_device.carrier == 1U);
	error = disconnect_ioctl(&fullmac_device);
	(void)status_read(&fullmac_device, &status);
	snprintf(what, sizeof(what), "station: disconnect error=%d state=%u terminal=%d carrier=%u", error, status.state, status.terminal_error, fullmac_device.carrier);
	check(what, error == 0 && status.state == WLAN_STATE_IDLE && status.terminal_error == 0 && fullmac_device.carrier == 0U);
	check("station: no secret left after the connections", wlan_station_test_secrets_clear(station));

	/* Reports the station for the threaded cases and the close. */
	return station;
}

/* Checks that a SoftMAC station refuses the FullMAC entry points. */
static void
softmac_cases(
	void)
{
	static const uint8_t hwaddr[6] = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U };
	struct wlan_scan_profile profile;
	struct wlan_station_test_snapshot snapshot;
	struct wlan_station *station;
	uint8_t frame[64];
	size_t length;
	int error;
	char what[160];

	/* A SoftMAC device and a one-channel profile. */
	(void)snprintf(softmac_device.name, sizeof(softmac_device.name), "%s", "wlan1");
	memcpy(softmac_device.hwaddr, hwaddr, sizeof(hwaddr));
	softmac_device.hwaddr_len = 6U;
	memset(&profile, 0, sizeof(profile));
	profile.channel_count = 1U;
	profile.channels[0].channel = 6U;
	profile.channels[0].center_frequency_mhz = 2437U;

	/* The station attaches in SoftMAC mode. */
	error = wlan_station_test_attach(&softmac_device, &softmac_ops, NULL, &profile, test_clock, NULL, &station);
	snprintf(what, sizeof(what), "softmac: attach error=%d", error);
	check(what, error == 0);
	if (error != 0)
		return;
	(void)wlan_station_test_snapshot(station, &snapshot);
	check("softmac: snapshot says SoftMAC", snapshot.fullmac == 0U);

	/* The FullMAC entry points refuse it. */
	error = wlan_station_open(station);
	error = wlan_station_report_fullmac_event(station, 1U, WLAN_FULLMAC_EVENT_ASSOCIATED);
	snprintf(what, sizeof(what), "softmac: report_fullmac_event error=%d", error);
	check(what, error == EOPNOTSUPP);
	length = ethernet_build(frame, 0x0800U);
	error = wlan_station_report_ethernet(station, 1U, frame, length);
	snprintf(what, sizeof(what), "softmac: report_ethernet error=%d", error);
	check(what, error == EOPNOTSUPP);

	/* It detaches cleanly. */
	error = wlan_station_close(station);
	error = wlan_station_detach(station);
	snprintf(what, sizeof(what), "softmac: detach error=%d", error);
	check(what, error == 0);
}

#ifdef FULLMAC_TEST_THREADS
/* Runs the data path in a second thread while the connection cycles. */
static void
thread_cases(
	struct wlan_station *station)
{
	pthread_t data_thread;
	uint64_t generation;
	unsigned round;
	int error;
	char what[160];

	/* Starts the data thread. */
	pthread_mutex_lock(&thread_done_mutex);
	thread_done = 0;
	pthread_mutex_unlock(&thread_done_mutex);
	error = pthread_create(&data_thread, NULL, thread_data_path, station);
	check("threads: data thread started", error == 0);
	if (error != 0)
		return;

	/* Cycles connect, associated, authorized, and loss. */
	for (round = 0U; round < TEST_THREAD_ROUNDS; round++) {
		error = connect_ioctl(&fullmac_device, TEST_PASSPHRASE, TEST_PASSPHRASE_LENGTH);
		if (error != 0)
			break;
		wlan_timer_run(test_now);
		generation = fake.last_request.generation;
		(void)wlan_station_report_fullmac_event(station, generation, WLAN_FULLMAC_EVENT_ASSOCIATED);
		(void)wlan_station_report_fullmac_event(station, generation, WLAN_FULLMAC_EVENT_AUTHORIZED);
		(void)wlan_station_report_link_loss(station, generation, ECONNRESET);
	}

	/* Stops the data thread. */
	pthread_mutex_lock(&thread_done_mutex);
	thread_done = 1;
	pthread_mutex_unlock(&thread_done_mutex);
	(void)pthread_join(data_thread, NULL);

	/* Every transmit the core let through found the port open. */
	snprintf(what, sizeof(what), "threads: %u rounds, transmits=%u, without carrier=%u", round, fake.transmits, fake.transmit_without_carrier);
	check(what, round == TEST_THREAD_ROUNDS && fake.transmit_without_carrier == 0U);
}

/* Receives and transmits against whatever connection is current. */
static void *
thread_data_path(
	void *argument)
{
	struct wlan_station *station;
	uint8_t frame[64];
	uint64_t generation;
	size_t length;
	int done;

	/* Uses the station the main thread cycles. */
	station = argument;

	/* Loops until the main thread is done. */
	for (;;) {
		pthread_mutex_lock(&thread_done_mutex);
		done = thread_done;
		generation = thread_generation;
		pthread_mutex_unlock(&thread_done_mutex);
		if (done)
			break;

		/* Receives one frame and sends one, whatever the port says. */
		length = ethernet_build(frame, 0x0800U);
		if (generation != 0U)
			(void)wlan_station_report_ethernet(station, generation, frame, length);
		(void)transmit_packet(station, 0x0800U);
	}

	/* Reports nothing. */
	return NULL;
}
#endif

/*
 * Runs the engine, attach, station, SoftMAC, and threaded cases.
 */
int
main(
	void)
{
	struct wlan_station *station;
	int error;
	char what[160];
#ifdef FULLMAC_TEST_THREADS
	pthread_mutexattr_t attributes;

	/* Makes the one test lock recursive, as the core nests its spinlocks. */
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&test_spin_mutex, &attributes);
#endif

	/* Fills the method tables, then runs every case. */
	ops_fill();
	engine_cases();
	station_attach_cases();
	station = station_cases();
#ifdef FULLMAC_TEST_THREADS
	if (station != NULL)
		thread_cases(station);
#endif
	softmac_cases();

	/* The FullMAC station closes and detaches. */
	if (station != NULL) {
		error = wlan_station_close(station);
		snprintf(what, sizeof(what), "station: close error=%d", error);
		check(what, error == 0);
		error = wlan_station_detach(station);
		snprintf(what, sizeof(what), "station: detach error=%d", error);
		check(what, error == 0);
	}

	/* Prints the verdict. */
	if (test_failures != 0) {
		printf("fullmac-host-test: FAIL (%d)\n", test_failures);
		return 1;
	}

	/* Succeeded. */
	printf("fullmac-host-test: PASS\n");
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
