/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/lock.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/wlan.h"
#include "kern/net/wlan-wpa2.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct fake_radio {
	struct wlan_station *station;
	uint64_t now;
	uint64_t scan_generation;
	uint64_t scan_step_deadline;
	uint64_t connect_generation;
	uint64_t connect_deadline;
	struct wlan_bss_record selected;
	unsigned scan_start_calls;
	unsigned scan_stop_calls;
	unsigned management_calls;
	unsigned connect_start_calls;
	unsigned disconnect_calls;
	unsigned quiesce_calls;
	unsigned frame_tx_calls;
	unsigned key_delete_calls;
	unsigned pair_delete_calls;
	unsigned group_delete_calls;
	unsigned key_delete_busy_remaining;
	int scan_start_error;
	int scan_stop_error;
	int management_error;
	int connect_start_error;
	int disconnect_error;
	int quiesce_error;
	struct wlan_radio_tx_request last_tx;
	uint8_t last_tx_frame[WLAN_MANAGEMENT_FRAME_MAX];
	uint64_t tx_packet_numbers[64];
	uint64_t tx_key_generations[64];
	uint8_t tx_encrypted[64];
	struct block_gate *scan_start_gate;
	int scan_start_returned;
	int scan_stop_saw_start_returned;
	uint32_t scan_steps[256];
	uint32_t scan_channels[256];
	uint64_t scan_deadlines[256];
	uint8_t management_frame[64];
	size_t management_length;
	uint64_t management_deadline;
	uint8_t hwaddr[6];
	uint64_t scan_start_advance;
	uint64_t management_advance;
	uint64_t scan_stop_advance;
	uint64_t connect_start_advance;
	int scan_start_report_ready;
};

struct block_gate {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	int entered;
	int released;
};

struct blocked_report {
	struct wlan_station *station;
	uint64_t generation;
	struct wlan_bss_record bss;
	int error;
};

struct scan_race_task {
	struct net_device *device;
	struct wlan_station *station;
	uint64_t scan_generation;
	uint64_t snapshot_generation;
	struct wlan_bss_record bss;
	int writer;
	int error;
};

struct scan_ioctl_task {
	struct net_device *device;
	struct wlan_scan_request request;
	int error;
};

struct timer_task {
	uint64_t now;
};

static void block_gate_hook(void *context);
int sched_yield(void);

static unsigned worker_wake_calls;
static int carrier_up_error;
static int carrier_down_error;
static unsigned carrier_down_calls;
static int net_device_reference_balance;
static int net_device_ref_live_failure;

void
net_worker_wakeup(void)
{
	worker_wake_calls++;
}

int
net_device_ref_live(struct net_device *device)
{
	if (device == NULL || net_device_ref_live_failure)
		return 0;
	net_device_reference_balance++;
	return 1;
}

void
net_device_release(struct net_device *device)
{
	assert(device != NULL);
	assert(net_device_reference_balance > 0);
	net_device_reference_balance--;
}

struct packet_buf *
packet_buf_alloc(size_t headroom)
{
	(void)headroom;
	return NULL;
}

void *
packet_buf_append(struct packet_buf *packet, size_t length)
{
	(void)packet;
	(void)length;
	return NULL;
}

void
packet_buf_free(struct packet_buf *packet)
{
	(void)packet;
}

void
net_device_receive(struct net_device *device, struct packet_buf *packet)
{
	(void)device;
	(void)packet;
}

void
net_device_tx_error(struct net_device *device)
{
	(void)device;
}

void
spin_init(struct spinlock *lock, enum lock_rank rank, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	while (__atomic_exchange_n(&lock->held.value, 1U,
	    __ATOMIC_ACQUIRE) != 0U)
		__asm__ volatile("" ::: "memory");
	return 0U;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long enabled)
{
	(void)enabled;
	__atomic_store_n(&lock->held.value, 0U, __ATOMIC_RELEASE);
}

uint64_t
clock_ticks(void)
{
	return 0U;
}

int
net_device_set_carrier(struct net_device *device, int carrier)
{
	if (device == NULL)
		return ENODEV;
	if (!carrier)
		carrier_down_calls++;
	if (carrier && carrier_up_error != 0)
		return carrier_up_error;
	if (!carrier && carrier_down_error != 0)
		return carrier_down_error;
	device->carrier = carrier != 0;
	if (device->carrier)
		device->flags |= NET_DEVICE_RUNNING;
	else
		device->flags &= ~NET_DEVICE_RUNNING;
	return 0;
}

static uint64_t
fake_clock(void *context)
{
	return ((struct fake_radio *)context)->now;
}

static int
fake_scan_channel_start(void *context, uint64_t generation,
	uint32_t step_index, uint32_t channel, uint64_t deadline)
{
	struct fake_radio *fake = context;
	unsigned call = fake->scan_start_calls;

	assert(call < sizeof(fake->scan_steps) / sizeof(fake->scan_steps[0]));
	fake->scan_start_calls++;
	fake->scan_generation = generation;
	fake->scan_step_deadline = deadline;
	fake->scan_steps[call] = step_index;
	fake->scan_channels[call] = channel;
	fake->scan_deadlines[call] = deadline;
	if (fake->scan_start_gate != NULL)
		block_gate_hook(fake->scan_start_gate);
	if (fake->scan_start_report_ready)
		assert(wlan_station_report_scan_channel_ready(fake->station,
		    generation, step_index) == 0);
	fake->now += fake->scan_start_advance;
	fake->scan_start_returned = 1;
	return fake->scan_start_error;
}

static int
fake_management_transmit(void *context, uint64_t generation,
	const uint8_t *frame, size_t length, uint64_t deadline)
{
	struct fake_radio *fake = context;

	assert(generation == fake->scan_generation);
	assert(length <= sizeof(fake->management_frame));
	fake->management_calls++;
	fake->management_length = length;
	fake->management_deadline = deadline;
	memcpy(fake->management_frame, frame, length);
	fake->now += fake->management_advance;
	return fake->management_error;
}

static int
fake_scan_stop(void *context, uint64_t generation)
{
	struct fake_radio *fake = context;

	fake->scan_stop_calls++;
	assert(generation == fake->scan_generation);
	fake->scan_stop_saw_start_returned = fake->scan_start_returned;
	fake->now += fake->scan_stop_advance;
	return fake->scan_stop_error;
}

static int
fake_connect_start(void *context, uint64_t generation,
	const struct wlan_bss_record *bss, uint64_t deadline)
{
	struct fake_radio *fake = context;

	fake->connect_start_calls++;
	fake->connect_generation = generation;
	fake->connect_deadline = deadline;
	fake->selected = *bss;
	fake->now += fake->connect_start_advance;
	return fake->connect_start_error;
}

static int
fake_disconnect(void *context, uint64_t generation)
{
	struct fake_radio *fake = context;

	fake->disconnect_calls++;
	assert(generation == fake->connect_generation);
	return fake->disconnect_error;
}

static int
fake_association_set(void *context, uint64_t generation,
	const uint8_t bssid[6], uint16_t aid, uint64_t deadline)
{
	struct fake_radio *fake = context;

	assert(generation == fake->connect_generation);
	assert(memcmp(bssid, fake->selected.bssid, 6U) == 0);
	assert(aid != 0U && deadline <= fake->connect_deadline);
	return 0;
}

static int
fake_association_clear(void *context, uint64_t generation,
	uint64_t deadline)
{
	struct fake_radio *fake = context;

	assert(generation == fake->connect_generation);
	assert(deadline <= fake->connect_deadline);
	return 0;
}

static int
fake_frame_transmit(void *context,
	const struct wlan_radio_tx_request *request)
{
	struct fake_radio *fake = context;

	assert(request != NULL && request->frame != NULL &&
	    request->length != 0U);
	assert(request->generation == fake->connect_generation);
	assert(request->deadline_ticks <= fake->connect_deadline);
	assert(fake->frame_tx_calls < 64U);
	fake->tx_packet_numbers[fake->frame_tx_calls] = request->packet_number;
	fake->tx_key_generations[fake->frame_tx_calls] = request->key_generation;
	fake->tx_encrypted[fake->frame_tx_calls] = request->encrypted;
	fake->last_tx = *request;
	assert(request->length <= sizeof(fake->last_tx_frame));
	memcpy(fake->last_tx_frame, request->frame, request->length);
	fake->last_tx.frame = fake->last_tx_frame;
	fake->frame_tx_calls++;
	return 0;
}

static int
fake_key_install(void *context,
	const struct wlan_radio_key_request *request)
{
	struct fake_radio *fake = context;

	assert(request != NULL && request->generation ==
	    fake->connect_generation);
	return 0;
}

static int
fake_key_delete(void *context, uint64_t generation,
	enum wlan_radio_key_kind kind, uint8_t key_index,
	uint64_t key_generation, uint64_t deadline)
{
	struct fake_radio *fake = context;

	(void)kind;
	(void)key_index;
	(void)key_generation;
	assert(generation == fake->connect_generation);
	assert(deadline <= fake->connect_deadline);
	fake->key_delete_calls++;
	if (kind == WLAN_RADIO_KEY_PAIRWISE)
		fake->pair_delete_calls++;
	else
		fake->group_delete_calls++;
	if (fake->key_delete_busy_remaining != 0U) {
		fake->key_delete_busy_remaining--;
		return EBUSY;
	}
	return 0;
}

static int
fake_keys_activate(void *context, uint64_t generation,
	uint64_t pairwise_key_generation, uint64_t group_key_generation,
	uint64_t deadline)
{
	struct fake_radio *fake = context;

	assert(generation == fake->connect_generation);
	assert(pairwise_key_generation != 0U && group_key_generation != 0U);
	assert(deadline <= fake->connect_deadline);
	return 0;
}

bool
hal_entropy_fill(void *buffer, size_t length)
{
	memset(buffer, 0x5a, length);
	return true;
}

static int
fake_quiesce(void *context)
{
	struct fake_radio *fake = context;

	fake->quiesce_calls++;
	return fake->quiesce_error;
}

static const struct wlan_radio_ops fake_ops = {
	.scan_channel_start = fake_scan_channel_start,
	.scan_stop = fake_scan_stop,
	.connect_start = fake_connect_start,
	.disconnect = fake_disconnect,
	.management_transmit = fake_management_transmit,
	.association_set = fake_association_set,
	.association_clear = fake_association_clear,
	.frame_transmit = fake_frame_transmit,
	.key_install = fake_key_install,
	.key_delete = fake_key_delete,
	.keys_activate = fake_keys_activate,
	.quiesce = fake_quiesce
};

static const struct wlan_radio_ops scan_only_ops = {
	.scan_channel_start = fake_scan_channel_start,
	.scan_stop = fake_scan_stop,
	.management_transmit = fake_management_transmit,
	.quiesce = fake_quiesce
};

static const struct wlan_radio_ops empty_ops = {0};

static void
request_header(void *request, size_t size, const char *name)
{
	struct wlan_ioctl_header *header = request;
	size_t length = strlen(name);

	assert(length < IFNAMSIZ);
	memset(request, 0, size);
	memcpy(header->ifr_name, name, length + 1U);
	header->version = WLAN_ABI_VERSION;
	header->size = (uint32_t)size;
}

static struct wlan_bss_record
make_bss(uint8_t identity, const uint8_t *ssid, uint8_t ssid_length,
	int32_t rssi)
{
	struct wlan_bss_record bss;

	memset(&bss, 0, sizeof(bss));
	bss.bssid[0] = 0x02U;
	bss.bssid[5] = identity;
	memcpy(bss.ssid, ssid, ssid_length);
	bss.ssid_length = ssid_length;
	bss.channel = 6U;
	bss.center_frequency_mhz = 2437U;
	bss.rssi_dbm = rssi;
	bss.beacon_interval_tu = 100U;
	bss.capability = 0x0011U;
	bss.security = WLAN_SECURITY_PRIVACY | WLAN_SECURITY_WPA2 |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK;
	return bss;
}

static void
block_gate_init(struct block_gate *gate)
{
	memset(gate, 0, sizeof(*gate));
	assert(pthread_mutex_init(&gate->mutex, NULL) == 0);
	assert(pthread_cond_init(&gate->condition, NULL) == 0);
}

static void
block_gate_destroy(struct block_gate *gate)
{
	assert(pthread_cond_destroy(&gate->condition) == 0);
	assert(pthread_mutex_destroy(&gate->mutex) == 0);
}

static void
block_gate_hook(void *context)
{
	struct block_gate *gate = context;

	assert(pthread_mutex_lock(&gate->mutex) == 0);
	gate->entered = 1;
	assert(pthread_cond_broadcast(&gate->condition) == 0);
	while (!gate->released)
		assert(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
	assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void
block_gate_wait(struct block_gate *gate)
{
	assert(pthread_mutex_lock(&gate->mutex) == 0);
	while (!gate->entered)
		assert(pthread_cond_wait(&gate->condition, &gate->mutex) == 0);
	assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void
block_gate_release(struct block_gate *gate)
{
	assert(pthread_mutex_lock(&gate->mutex) == 0);
	gate->released = 1;
	assert(pthread_cond_broadcast(&gate->condition) == 0);
	assert(pthread_mutex_unlock(&gate->mutex) == 0);
}

static void *
blocked_report_main(void *argument)
{
	struct blocked_report *report = argument;

	report->error = wlan_station_report_scan_bss(report->station,
	    report->generation, &report->bss);
	return NULL;
}

static void *
scan_race_main(void *argument)
{
	struct scan_race_task *task = argument;
	unsigned iteration;

	for (iteration = 0U; iteration < 2000U; iteration++) {
		if (task->writer) {
			task->error = wlan_station_report_scan_bss(task->station,
			    task->scan_generation, &task->bss);
		} else {
			struct wlan_bss_request bss;
			struct wlan_status_request status;

			request_header(&bss, sizeof(bss), "wlan0");
			bss.generation = task->snapshot_generation;
			bss.index = 0U;
			task->error = wlan_station_ioctl(task->device,
			    SIOCGWLANBSS, &bss);
			if (task->error == 0 &&
			    bss.generation != task->snapshot_generation)
				task->error = EIO;
			request_header(&status, sizeof(status), "wlan0");
			if (task->error == 0)
				task->error = wlan_station_ioctl(task->device,
				    SIOCGWLANSTATUS, &status);
			if (task->error == 0 &&
			    (status.snapshot_generation !=
			    task->snapshot_generation ||
			    status.scan_generation != task->scan_generation))
				task->error = EIO;
		}
		if (task->error != 0)
			break;
	}
	return NULL;
}

static void *
scan_ioctl_main(void *argument)
{
	struct scan_ioctl_task *task = argument;

	task->error = wlan_station_ioctl(task->device, SIOCSWLANSCAN,
	    &task->request);
	return NULL;
}

static void *
timer_main(void *argument)
{
	struct timer_task *task = argument;

	wlan_timer_run(task->now);
	return NULL;
}

static size_t
build_frame_ciphers(uint8_t *frame, const uint8_t *ssid,
	uint8_t ssid_length, uint8_t bssid_identity, uint8_t group_cipher,
	uint8_t pairwise_cipher, uint8_t akm,
	uint16_t rsn_capabilities, int include_rsn)
{
	size_t offset = 36U;
	static const uint8_t rsn_oui[3] = { 0x00U, 0x0fU, 0xacU };

	memset(frame, 0, 256U);
	frame[0] = 0x80U;
	frame[16] = 0x02U;
	frame[21] = bssid_identity;
	frame[32] = 100U;
	if (include_rsn)
		frame[34] = 0x10U;
	frame[offset++] = 0U;
	frame[offset++] = ssid_length;
	memcpy(frame + offset, ssid, ssid_length);
	offset += ssid_length;
	frame[offset++] = 3U;
	frame[offset++] = 1U;
	frame[offset++] = 6U;
	if (!include_rsn)
		return offset;
	frame[offset++] = 48U;
	frame[offset++] = 20U;
	frame[offset++] = 1U;
	frame[offset++] = 0U;
	memcpy(frame + offset, rsn_oui, sizeof(rsn_oui));
	offset += sizeof(rsn_oui);
	frame[offset++] = group_cipher;
	frame[offset++] = 1U;
	frame[offset++] = 0U;
	memcpy(frame + offset, rsn_oui, sizeof(rsn_oui));
	offset += sizeof(rsn_oui);
	frame[offset++] = pairwise_cipher;
	frame[offset++] = 1U;
	frame[offset++] = 0U;
	memcpy(frame + offset, rsn_oui, sizeof(rsn_oui));
	offset += sizeof(rsn_oui);
	frame[offset++] = akm;
	frame[offset++] = (uint8_t)rsn_capabilities;
	frame[offset++] = (uint8_t)(rsn_capabilities >> 8);
	return offset;
}

static size_t
build_frame(uint8_t *frame, const uint8_t *ssid, uint8_t ssid_length,
	uint8_t bssid_identity, uint8_t cipher, uint8_t akm,
	uint16_t rsn_capabilities, int include_rsn)
{
	return build_frame_ciphers(frame, ssid, ssid_length, bssid_identity,
	    cipher, cipher, akm, rsn_capabilities, include_rsn);
}

static void
test_frame_parser(void)
{
	uint8_t frame[256];
	uint8_t maximum[WLAN_SSID_MAX];
	const uint8_t binary[] = { 'a', 0U, 'b' };
	struct wlan_bss_record bss;
	size_t length;
	unsigned index;

	for (index = 0; index < sizeof(maximum); index++)
		maximum[index] = (uint8_t)(0x80U + index);
	length = build_frame(frame, binary, sizeof(binary), 1U, 4U, 2U,
	    0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -42, 0U, &bss) == 0);
	assert(bss.ssid_length == sizeof(binary));
	assert(memcmp(bss.ssid, binary, sizeof(binary)) == 0);
	assert(bss.channel == 6U && bss.center_frequency_mhz == 2437U);
	assert((bss.security & (WLAN_SECURITY_WPA2 |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK)) ==
	    (WLAN_SECURITY_WPA2 | WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK));
	length = build_frame_ciphers(frame, binary, sizeof(binary), 11U,
	    4U, 6U, 2U, 0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -2, 0U, &bss) == 0);
	assert((bss.security & WLAN_SECURITY_UNSUPPORTED_SUITE) != 0U);
	assert((bss.security & WLAN_SECURITY_CCMP) == 0U);
	length = build_frame_ciphers(frame, binary, sizeof(binary), 12U,
	    2U, 4U, 2U, 0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -2, 0U, &bss) == 0);
	assert((bss.security & (WLAN_SECURITY_TKIP |
	    WLAN_SECURITY_CCMP)) ==
	    (WLAN_SECURITY_TKIP | WLAN_SECURITY_CCMP));
	length = build_frame_ciphers(frame, binary, sizeof(binary), 13U,
	    6U, 4U, 2U, 0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -2, 0U, &bss) == 0);
	assert((bss.security & (WLAN_SECURITY_UNSUPPORTED_SUITE |
	    WLAN_SECURITY_CCMP)) ==
	    (WLAN_SECURITY_UNSUPPORTED_SUITE | WLAN_SECURITY_CCMP));

	length = build_frame(frame, maximum, sizeof(maximum), 2U, 4U, 2U,
	    0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -1, 0U, &bss) == 0);
	assert(bss.ssid_length == WLAN_SSID_MAX);
	length = build_frame(frame, maximum, 0U, 3U, 4U, 2U, 0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == 0);
	assert(bss.ssid_length == 0U);

	length = build_frame(frame, binary, sizeof(binary), 4U, 2U, 2U,
	    0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == 0);
	assert((bss.security & WLAN_SECURITY_TKIP) != 0U);
	length = build_frame(frame, binary, sizeof(binary), 5U, 4U, 8U,
	    0U, 1);
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == 0);
	assert((bss.security & WLAN_SECURITY_SAE) != 0U);
	length = build_frame(frame, binary, sizeof(binary), 6U, 4U, 2U,
	    0x0040U, 1);
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == 0);
	assert((bss.security & WLAN_SECURITY_PMF_REQUIRED) != 0U);
	length = build_frame(frame, binary, sizeof(binary), 7U, 4U, 2U,
	    0U, 0);
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == 0);
	assert(bss.security == 0U);

	length = build_frame(frame, binary, sizeof(binary), 8U, 4U, 2U,
	    0U, 1);
	frame[length++] = 0U;
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == EINVAL);
	length = build_frame(frame, binary, sizeof(binary), 9U, 4U, 2U,
	    0U, 1);
	frame[length++] = 0U;
	frame[length++] = 1U;
	frame[length++] = 'x';
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == EINVAL);
	length = build_frame(frame, binary, sizeof(binary), 10U, 4U, 2U,
	    0U, 1);
	memcpy(frame + length, frame + length - 22U, 22U);
	length += 22U;
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == EINVAL);
	length = build_frame(frame, binary, sizeof(binary), 10U, 4U, 2U,
	    0U, 1);
	frame[length - 21U] = 48U;
	assert(wlan_frame_parse_bss(frame, length - 1U, -20, 0U, &bss) ==
	    EINVAL);
	frame[0] = 0x08U;
	assert(wlan_frame_parse_bss(frame, length, -20, 0U, &bss) == EINVAL);
}

static void
test_validation(struct net_device *device)
{
	struct wlan_scan_request scan;
	unsigned long wrong;

	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	scan.version++;
	assert(wlan_station_ioctl(device, SIOCSWLANSCAN, &scan) == EINVAL);
	scan.version = WLAN_ABI_VERSION;
	scan.size--;
	assert(wlan_station_ioctl(device, SIOCSWLANSCAN, &scan) == EINVAL);
	scan.size = sizeof(scan);
	scan.reserved[0] = 1U;
	assert(wlan_station_ioctl(device, SIOCSWLANSCAN, &scan) == EINVAL);
	scan.reserved[0] = 0U;
	memcpy(scan.ifr_name, "wrong0", 7U);
	assert(wlan_station_ioctl(device, SIOCSWLANSCAN, &scan) == ENODEV);
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	wrong = ZEDBSD_IOC(ZEDBSD_IOC_INOUT, 'W', 1,
	    sizeof(struct wlan_scan_request) + 4U);
	assert(wlan_station_ioctl(device, wrong, &scan) == ENOTTY);
	assert(wlan_station_ioctl(device, SIOCSWLANSCAN, NULL) == EFAULT);
}

static const struct wlan_scan_profile test_scan_profile = {
	.channel_count = 1U,
	.channels = {
		{
			.channel = 6U,
			.center_frequency_mhz = 2437U,
			.flags = WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED
		}
	}
};

static void
scan_drive_to_dwell(struct wlan_station *station, struct fake_radio *fake,
	uint64_t generation)
{
	unsigned starts = fake->scan_start_calls;
	unsigned probes = fake->management_calls;

	assert(wlan_work_pending());
	wlan_timer_run(fake->now);
	assert(fake->scan_start_calls == starts + 1U);
	assert(fake->scan_steps[starts] == 0U);
	assert(fake->scan_channels[starts] == 6U);
	assert(fake->scan_deadlines[starts] ==
	    fake->now + WLAN_SCAN_TUNE_DEADLINE_TICKS);
	assert(!wlan_work_pending());
	assert(wlan_station_report_scan_channel_ready(station, generation,
	    1U) == ESTALE);
	assert(wlan_station_report_scan_channel_ready(station, generation + 1U,
	    0U) == ESTALE);
	assert(wlan_station_report_scan_channel_ready(station, generation,
	    0U) == 0);
	assert(wlan_work_pending());
	wlan_timer_run(fake->now);
	assert(fake->management_calls == probes + 1U);
	assert(fake->management_length == 32U);
	assert(fake->management_frame[0] == 0x40U);
	assert(memcmp(fake->management_frame + 4U,
	    (uint8_t[6]){ 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU },
	    6U) == 0);
	assert(memcmp(fake->management_frame + 10U, fake->hwaddr,
	    sizeof(fake->hwaddr)) == 0);
	assert(fake->management_frame[24] == 0U &&
	    fake->management_frame[25] == 0U);
	assert(fake->management_frame[26] == 1U &&
	    fake->management_frame[27] == 4U);
	assert(fake->management_deadline == fake->now + WLAN_SCAN_DWELL_TICKS);
}

static void
scan_complete(struct wlan_station *station, struct fake_radio *fake,
	uint64_t generation)
{
	unsigned stops = fake->scan_stop_calls;

	fake->now += WLAN_SCAN_DWELL_TICKS;
	wlan_timer_run(fake->now);
	assert(fake->scan_stop_calls == stops + 1U);
	assert(wlan_station_report_scan_channel_ready(station, generation,
	    0U) == ESTALE);
}

static uint64_t
reconnect_scan_drive_to_dwell(struct wlan_station *station,
	struct fake_radio *fake)
{
	unsigned starts = fake->scan_start_calls;
	unsigned probes = fake->management_calls;
	uint64_t generation;

	wlan_timer_run(fake->now);
	assert(fake->scan_start_calls == starts + 1U);
	generation = fake->scan_generation;
	assert(wlan_station_report_scan_channel_ready(station, generation,
	    0U) == 0);
	wlan_timer_run(fake->now);
	assert(fake->management_calls == probes + 1U);
	return generation;
}

static size_t
clear_eapol_from_ap(uint8_t *frame, const uint8_t station_address[6],
	const uint8_t bssid_address[6])
{
	static const uint8_t llc[] = {
		0xaaU, 0xaaU, 0x03U, 0U, 0U, 0U, 0x88U, 0x8eU
	};

	memset(frame, 0, 36U);
	frame[0] = 0x08U;
	frame[1] = 0x02U;
	memcpy(frame + 4U, station_address, 6U);
	memcpy(frame + 10U, bssid_address, 6U);
	memcpy(frame + 16U, bssid_address, 6U);
	memcpy(frame + 24U, llc, sizeof(llc));
	frame[32U] = 2U;
	frame[33U] = 3U;
	return 36U;
}

static void
disconnect_station(struct net_device *device)
{
	struct wlan_disconnect_request disconnect;

	request_header(&disconnect, sizeof(disconnect), "wlan0");
	assert(wlan_station_ioctl(device, SIOCSWLANDISCONNECT, &disconnect) == 0);
	assert(disconnect.state == WLAN_STATE_IDLE);
}

static uint64_t
connect_cached_station(struct net_device *device, const uint8_t *ssid,
	uint8_t ssid_length)
{
	struct wlan_connect_request connect;

	request_header(&connect, sizeof(connect), "wlan0");
	memcpy(connect.ssid, ssid, ssid_length);
	connect.ssid_length = ssid_length;
	memset(connect.passphrase, 0x5a, WLAN_PASSPHRASE_MIN);
	connect.passphrase_length = WLAN_PASSPHRASE_MIN;
	assert(wlan_station_ioctl(device, SIOCSWLANCONNECT, &connect) == 0);
	assert(connect.generation != 0U &&
	    connect.state == WLAN_STATE_AUTHENTICATING);
	return connect.generation;
}

static void
assert_station_retired(struct wlan_station *station)
{
	struct wlan_station_test_snapshot snapshot;

	assert(wlan_station_test_snapshot(station, &snapshot) == 0);
	assert(!snapshot.reconnect_pending &&
	    !snapshot.reconnect_scan_active &&
	    snapshot.reconnect_deadline == 0U &&
	    snapshot.reconnect_next_attempt == 0U &&
	    snapshot.reconnect_cleanup_retry == 0U &&
	    snapshot.connection_deadline == 0U &&
	    snapshot.connection_step_deadline == 0U &&
	    snapshot.connect_retry_deadline == 0U &&
	    snapshot.scan_retry_deadline == 0U &&
	    snapshot.beacon_watch_deadline == 0U &&
	    snapshot.pairwise_key_generation == 0U &&
	    snapshot.group_key_generation == 0U &&
	    snapshot.pending_pairwise_key_generation == 0U &&
	    snapshot.pending_group_key_generation == 0U &&
	    snapshot.pairwise_receive_packet_number == 0U &&
	    memcmp(snapshot.group_receive_packet_number,
	    (uint64_t[4]){ 0U, 0U, 0U, 0U },
	    sizeof(snapshot.group_receive_packet_number)) == 0 &&
	    snapshot.pending_group_receive_packet_number == 0U &&
	    snapshot.transmit_packet_number == 0U &&
	    !snapshot.controlled_port && !snapshot.connect_driver_active &&
	    !snapshot.connect_stop_pending &&
	    !snapshot.connect_retire_explicit &&
	    wlan_station_test_secrets_clear(station));
}

static void
reconnect_fail_scans(struct wlan_station *station, struct fake_radio *fake,
	unsigned count)
{
	struct wlan_station_test_snapshot snapshot;
	uint64_t generation;
	unsigned index;

	for (index = 0U; index < count; index++) {
		generation = reconnect_scan_drive_to_dwell(station, fake);
		scan_complete(station, fake, generation);
		assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
		    snapshot.reconnect_attempts == index + 1U);
		if (index + 1U < count)
			fake->now = snapshot.reconnect_next_attempt;
	}
}

static void
test_common_wpa_lifecycle(struct net_device *device,
	struct wlan_station *station, struct fake_radio *fake)
{
	static const uint8_t target[] = { 'n', 'e', 't' };
	static const uint8_t old_target[] = { 'o', 'l', 'd' };
	static const uint64_t backoff_delays[] = {
		0U, 100U, 200U, 400U, 800U
	};
	struct wlan_connect_request connect;
	struct wlan_disconnect_request disconnect;
	struct wlan_station_test_snapshot snapshot;
	struct wlan_radio_rx_frame report;
	struct wlan_bss_record bss;
	struct wlan_bss_record old_bss;
	struct wlan_bss_record foreign_bss;
	uint8_t frame[256];
	uint8_t eapol[4] = { 2U, 3U, 0U, 0U };
	size_t length;
	uint64_t generation;
	uint64_t cancelled_deadline;
	uint64_t prior_deadline;
	unsigned frame_tx_base;
	unsigned scan_start_base;
	unsigned scan_count;
	unsigned iteration;
	unsigned phase;

	bss = make_bss(0x71U, target, sizeof(target), -30);
	fake->now = 2000U;
	fake->connect_generation = 500U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	fake->selected = bss;
	assert(wlan_station_test_seed_authorized(station, &bss, 500U, 600U) == 0);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0);
	assert(snapshot.state == WLAN_STATE_CONNECTED &&
	    snapshot.controlled_port && snapshot.association_capability == 0x0011U &&
	    snapshot.beacon_watch_deadline > fake->now);

	/* Only a fresh beacon from the selected BSSID refreshes the finite
	 * activity latch.  Foreign and stale-generation beacons remain irrelevant. */
	prior_deadline = snapshot.beacon_watch_deadline;
	fake->now += 100U;
	length = build_frame(frame, target, sizeof(target), bss.bssid[5], 4U,
	    2U, 0U, 1);
	memset(&report, 0, sizeof(report));
	report.generation = 500U;
	report.frame = frame;
	report.length = length;
	report.rssi_dbm = -30;
	report.channel = 6U;
	assert(wlan_station_report_frame(station, &report) == 0);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0);
	assert(snapshot.beacon_watch_deadline > prior_deadline);
	prior_deadline = snapshot.beacon_watch_deadline;
	length = build_frame(frame, target, sizeof(target),
	    (uint8_t)(bss.bssid[5] + 1U), 4U, 2U, 0U, 1);
	report.length = length;
	assert(wlan_station_report_frame(station, &report) == ESTALE);
	report.generation--;
	length = build_frame(frame, target, sizeof(target), bss.bssid[5], 4U,
	    2U, 0U, 1);
	report.length = length;
	assert(wlan_station_report_frame(station, &report) == ESTALE);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.beacon_watch_deadline == prior_deadline);
	report.generation = 500U;

	/* Active-key lifetime rejects clear EAPOL before it can reach the engine
	 * and close the controlled port. */
	length = clear_eapol_from_ap(frame, device->hwaddr, bss.bssid);
	report.frame = frame;
	report.length = length;
	report.rssi_dbm = -30;
	report.channel = 6U;
	assert(wlan_station_report_frame(station, &report) == EACCES);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.controlled_port && snapshot.wpa_state ==
	    WLAN_WPA2_STATE_AUTHORIZED);

	/* EAPOL retransmission through an active PTK uses a fresh protected PN
	 * every time; a failed/delayed report can never cause PN reuse. */
	frame_tx_base = fake->frame_tx_calls;
	assert(wlan_station_test_transmit_eapol(station, 1U, eapol,
	    sizeof(eapol)) == 0);
	assert(wlan_station_test_transmit_eapol(station, 2U, eapol,
	    sizeof(eapol)) == 0);
	assert(fake->frame_tx_calls == frame_tx_base + 2U &&
	    fake->tx_encrypted[frame_tx_base] &&
	    fake->tx_encrypted[frame_tx_base + 1U] &&
	    fake->tx_packet_numbers[frame_tx_base] != 0U &&
	    fake->tx_packet_numbers[frame_tx_base + 1U] ==
	    fake->tx_packet_numbers[frame_tx_base] + 1U &&
	    fake->tx_key_generations[frame_tx_base] == 600U &&
	    fake->tx_key_generations[frame_tx_base + 1U] == 600U);

	/* Watchdog expiry enters the same checked link-loss boundary. */
	fake->now = prior_deadline - 1U;
	wlan_timer_run(fake->now);
	assert(device->carrier != 0U);
	fake->now = prior_deadline;
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.reconnect_pending && snapshot.beacon_watch_deadline == 0U &&
	    !snapshot.controlled_port && device->carrier == 0U);
	disconnect_station(device);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    !snapshot.reconnect_pending && snapshot.beacon_watch_deadline == 0U &&
	    wlan_station_test_secrets_clear(station));

	/* Pairwise rekey closes the controlled port but keeps the selected radio
	 * association alive, so matching beacons must still refresh its watchdog. */
	fake->now = 2500U;
	fake->connect_generation = 600U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	fake->selected = bss;
	assert(wlan_station_test_seed_authorized(station, &bss, 600U, 610U) == 0);
	assert(wlan_station_test_begin_pairwise_rekey(station) == 0);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.state == WLAN_STATE_FOUR_WAY && !snapshot.controlled_port);
	prior_deadline = snapshot.beacon_watch_deadline;
	fake->now += 100U;
	length = build_frame(frame, target, sizeof(target), bss.bssid[5], 4U,
	    2U, 0U, 1);
	memset(&report, 0, sizeof(report));
	report.generation = 600U;
	report.frame = frame;
	report.length = length;
	report.rssi_dbm = -30;
	report.channel = 6U;
	assert(wlan_station_report_frame(station, &report) == 0);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.beacon_watch_deadline > prior_deadline);
	disconnect_station(device);

	/* Explicit disconnect owns an uncertain key-delete until a checked retry
	 * succeeds.  Neither a new request nor state scrubbing may cross it. */
	fake->now = 2750U;
	fake->connect_generation = 650U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	fake->selected = bss;
	assert(wlan_station_test_seed_authorized(station, &bss, 650U, 660U) == 0);
	fake->key_delete_busy_remaining = 1U;
	request_header(&disconnect, sizeof(disconnect), "wlan0");
	assert(wlan_station_ioctl(device, SIOCSWLANDISCONNECT, &disconnect) ==
	    EBUSY);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_FAILED &&
	    snapshot.pairwise_key_generation == 660U &&
	    !wlan_station_test_secrets_clear(station));
	assert(wlan_station_report_link_loss(station, 650U, EIO) == ESTALE);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    !snapshot.reconnect_pending && snapshot.connect_retire_explicit);
	request_header(&connect, sizeof(connect), "wlan0");
	memcpy(connect.ssid, target, sizeof(target));
	connect.ssid_length = sizeof(target);
	memset(connect.passphrase, 0x5a, WLAN_PASSPHRASE_MIN);
	connect.passphrase_length = WLAN_PASSPHRASE_MIN;
	assert(wlan_station_ioctl(device, SIOCSWLANCONNECT, &connect) == EBUSY);
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_FAILED);
	fake->now++;
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_IDLE &&
	    snapshot.state == WLAN_STATE_IDLE &&
	    snapshot.pairwise_key_generation == 0U &&
	    wlan_station_test_secrets_clear(station));

	/* Common link-loss ownership is identical across the initial auth,
	 * association, and four-way states.  These phase seeds retain the real
	 * engine cleanup/callback path while avoiding a second fake authenticator. */
	for (phase = 0U; phase < 3U; phase++) {
		fake->now = 2800U + phase * 10U;
		generation = connect_cached_station(device, target, sizeof(target));
		if (phase == 1U)
			assert(wlan_station_test_set_initial_phase(station,
			    WLAN_STATION_TEST_PHASE_ASSOCIATING) == 0);
		else if (phase == 2U)
			assert(wlan_station_test_set_initial_phase(station,
			    WLAN_STATION_TEST_PHASE_FOUR_WAY) == 0);
		assert(wlan_station_report_link_loss(station, generation, EIO) == 0);
		assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
		    snapshot.state == WLAN_STATE_FAILED &&
		    snapshot.wpa_state == WLAN_WPA2_STATE_IDLE &&
		    !snapshot.reconnect_pending);
		assert_station_retired(station);
		disconnect_station(device);
	}

	/* Connected data, group rekey, and pairwise rekey all converge on the
	 * bounded full-reconnect boundary with carrier already down. */
	for (phase = 0U; phase < 3U; phase++) {
		generation = 670U + phase * 10U;
		fake->now = 2900U + phase * 10U;
		fake->connect_generation = generation;
		fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
		fake->selected = bss;
		assert(wlan_station_test_seed_authorized(station, &bss, generation,
		    generation + 1U) == 0);
		if (phase == 1U)
			assert(wlan_station_test_begin_group_rekey(station) == 0);
		else if (phase == 2U)
			assert(wlan_station_test_begin_pairwise_rekey(station) == 0);
		assert(wlan_station_report_link_loss(station, generation, EIO) == 0);
		assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
		    snapshot.reconnect_pending &&
		    snapshot.wpa_state == WLAN_WPA2_STATE_RECONNECT_WAIT &&
		    !snapshot.controlled_port && device->carrier == 0U);
		disconnect_station(device);
		assert_station_retired(station);
	}

	/* An uncertain key delete must complete before targeted scan or hardware
	 * restart. */
	fake->now = 3000U;
	fake->connect_generation = 700U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	old_bss = make_bss(0x72U, old_target, sizeof(old_target), -31);
	fake->selected = old_bss;
	assert(wlan_station_test_seed_authorized(station, &old_bss, 700U,
	    800U) == 0);
	fake->key_delete_busy_remaining = 1U;
	scan_start_base = fake->scan_start_calls;
	assert(wlan_station_report_link_loss(station, 700U, EIO) == EBUSY);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_FAILED &&
	    snapshot.reconnect_pending &&
	    fake->scan_start_calls == scan_start_base);
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_FAILED &&
	    fake->scan_start_calls == scan_start_base);
	fake->now++;
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_RECONNECT_WAIT &&
	    fake->scan_start_calls == scan_start_base);
	generation = reconnect_scan_drive_to_dwell(station, fake);
	assert(fake->scan_channels[scan_start_base] == old_bss.channel &&
	    fake->management_length == 32U + sizeof(old_target) &&
	    fake->management_frame[24] == 0U &&
	    fake->management_frame[25] == sizeof(old_target) &&
	    memcmp(fake->management_frame + 26U, old_target,
	    sizeof(old_target)) == 0);
	foreign_bss = old_bss;
	foreign_bss.bssid[5]++;
	assert(wlan_station_report_scan_bss(station, generation,
	    &foreign_bss) == 0);
	scan_complete(station, fake, generation);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.reconnect_attempts == 1U &&
	    snapshot.reconnect_next_attempt == fake->now + 100U);
	/* A new SSID request cancels the old backoff, checks retirement, and starts
	 * only after the old engine reaches IDLE. */
	request_header(&connect, sizeof(connect), "wlan0");
	memcpy(connect.ssid, target, sizeof(target));
	connect.ssid_length = sizeof(target);
	memset(connect.passphrase, 0x5a, WLAN_PASSPHRASE_MIN);
	connect.passphrase_length = WLAN_PASSPHRASE_MIN;
	assert(wlan_station_ioctl(device, SIOCSWLANCONNECT, &connect) == 0);
	assert(connect.generation != 700U &&
	    connect.state == WLAN_STATE_AUTHENTICATING);
	disconnect_station(device);
	fake->now += 100U;
	wlan_timer_run(fake->now);
	assert(fake->scan_start_calls == scan_start_base + 1U &&
	    wlan_station_test_secrets_clear(station));

	/* Reappearance in the next targeted generation starts a fresh connection
	 * generation only after the scan-stop barrier. */
	fake->now = 4000U;
	fake->connect_generation = 900U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	fake->selected = bss;
	assert(wlan_station_test_seed_authorized(station, &bss, 900U, 1000U) == 0);
	assert(wlan_station_report_link_loss(station, 900U, EIO) == 0);
	generation = reconnect_scan_drive_to_dwell(station, fake);
	assert(wlan_station_report_scan_bss(station, generation, &bss) == 0);
	scan_complete(station, fake, generation);
	wlan_timer_run(fake->now);
	assert(fake->connect_start_calls != 0U &&
	    fake->connect_generation != 900U);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.connection_generation == fake->connect_generation &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_AUTH_TX);
	/* Loss during the fresh reconnect handshake retires that new generation
	 * and returns to checked RECONNECT_WAIT rather than overlapping attempts. */
	assert(wlan_station_report_link_loss(station,
	    snapshot.connection_generation, EIO) == 0);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.reconnect_pending &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_RECONNECT_WAIT);
	disconnect_station(device);

	/* A successful fresh generation clears every backoff counter/deadline. */
	fake->now = 4500U;
	fake->connect_generation = 1000U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	fake->selected = bss;
	assert(wlan_station_test_seed_authorized(station, &bss, 1000U,
	    1010U) == 0);
	assert(wlan_station_report_link_loss(station, 1000U, EIO) == 0);
	generation = reconnect_scan_drive_to_dwell(station, fake);
	assert(wlan_station_report_scan_bss(station, generation, &bss) == 0);
	scan_complete(station, fake, generation);
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.reconnect_pending &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_AUTH_TX);
	assert(wlan_station_test_complete_authorized(station, 1020U) == 0);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.state == WLAN_STATE_CONNECTED &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_AUTHORIZED &&
	    !snapshot.reconnect_pending && snapshot.reconnect_attempts == 0U &&
	    snapshot.reconnect_deadline == 0U &&
	    snapshot.reconnect_next_attempt == 0U && snapshot.controlled_port &&
	    device->carrier != 0U);
	disconnect_station(device);

	/* The finite 0/1/2/4/8 policy is relative to each completed failed scan,
	 * including dwell time; the fifth failure exhausts without a sixth scan. */
	fake->now = 5000U;
	fake->connect_generation = 1100U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	fake->selected = bss;
	assert(wlan_station_test_seed_authorized(station, &bss, 1100U, 1200U) == 0);
	assert(wlan_station_report_link_loss(station, 1100U, EIO) == 0);
	scan_count = fake->scan_start_calls;
	for (iteration = 0U; iteration < 5U; iteration++) {
		generation = reconnect_scan_drive_to_dwell(station, fake);
		scan_complete(station, fake, generation);
		assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
		    snapshot.reconnect_attempts == iteration + 1U);
		if (iteration < 4U) {
			assert(snapshot.reconnect_next_attempt == fake->now +
			    backoff_delays[iteration + 1U]);
			fake->now = snapshot.reconnect_next_attempt;
		}
	}
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    !snapshot.reconnect_pending && snapshot.state == WLAN_STATE_FAILED &&
	    fake->scan_start_calls == scan_count + 5U &&
	    wlan_station_test_secrets_clear(station));
	assert_station_retired(station);
	disconnect_station(device);

	/* Every frozen backoff slot is cancellable.  One iteration uses close/open
	 * to cover administrative down; the others use explicit disconnect. */
	for (phase = 0U; phase < 5U; phase++) {
		generation = 1300U + phase * 10U;
		fake->now = 7000U + phase * 2000U;
		fake->connect_generation = generation;
		fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
		fake->selected = bss;
		assert(wlan_station_test_seed_authorized(station, &bss,
		    generation, generation + 1U) == 0);
		assert(wlan_station_report_link_loss(station, generation, EIO) == 0);
		reconnect_fail_scans(station, fake, phase);
		assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
		    snapshot.reconnect_pending &&
		    snapshot.reconnect_next_attempt == fake->now +
		    backoff_delays[phase]);
		cancelled_deadline = snapshot.reconnect_next_attempt;
		scan_count = fake->scan_start_calls;
		if (phase == 3U) {
			assert(wlan_station_close(station) == 0);
			assert_station_retired(station);
			assert(wlan_station_open(station) == 0);
		} else {
			disconnect_station(device);
			assert_station_retired(station);
		}
		fake->now = cancelled_deadline;
		wlan_timer_run(fake->now);
		assert(fake->scan_start_calls == scan_count);
	}

	/* Detach is the removal-equivalent cancellation boundary.  Exercise it at
	 * the longest pending delay on an independent station instance. */
	{
		struct net_device remove_device;
		struct fake_radio remove_fake;
		struct wlan_station *remove_station;
		struct wlan_station_test_snapshot remove_snapshot;
		int references = net_device_reference_balance;

		memset(&remove_device, 0, sizeof(remove_device));
		memset(&remove_fake, 0, sizeof(remove_fake));
		memcpy(remove_device.name, "wlan9", 6U);
		memcpy(remove_device.hwaddr,
		    (uint8_t[6]){ 0x02U, 0U, 0U, 0U, 0U, 0x79U }, 6U);
		remove_device.hwaddr_len = 6U;
		remove_device.flags = NET_DEVICE_UP;
		memcpy(remove_fake.hwaddr, remove_device.hwaddr, 6U);
		remove_fake.now = 17000U;
		remove_fake.connect_generation = 1500U;
		remove_fake.connect_deadline = remove_fake.now +
		    WLAN_CONNECT_DEADLINE_TICKS;
		remove_fake.selected = bss;
		assert(wlan_station_test_attach(&remove_device, &fake_ops,
		    &remove_fake, &test_scan_profile, fake_clock, &remove_fake,
		    &remove_station) == 0);
		remove_fake.station = remove_station;
		assert(net_device_reference_balance == references + 1);
		assert(wlan_station_open(remove_station) == 0);
		assert(wlan_station_test_seed_authorized(remove_station, &bss,
		    1500U, 1510U) == 0);
		assert(wlan_station_report_link_loss(remove_station, 1500U,
		    EIO) == 0);
		reconnect_fail_scans(remove_station, &remove_fake, 4U);
		assert(wlan_station_test_snapshot(remove_station,
		    &remove_snapshot) == 0 && remove_snapshot.reconnect_pending &&
		    remove_snapshot.reconnect_next_attempt == remove_fake.now + 800U);
		cancelled_deadline = remove_snapshot.reconnect_next_attempt;
		scan_count = remove_fake.scan_start_calls;
		assert(wlan_station_detach(remove_station) == 0);
		assert(net_device_reference_balance == references);
		assert_station_retired(remove_station);
		remove_fake.now = cancelled_deadline;
		wlan_timer_run(remove_fake.now);
		assert(remove_fake.scan_start_calls == scan_count);
	}

	/* The independent 30-second cap also exhausts and scrubs even before the
	 * five-attempt count is reached. */
	fake->now = 18000U;
	fake->connect_generation = 1400U;
	fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
	fake->selected = bss;
	assert(wlan_station_test_seed_authorized(station, &bss, 1400U,
	    1410U) == 0);
	assert(wlan_station_report_link_loss(station, 1400U, EIO) == 0);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.reconnect_deadline > fake->now);
	fake->now = snapshot.reconnect_deadline;
	wlan_timer_run(fake->now);
	assert(wlan_station_test_snapshot(station, &snapshot) == 0 &&
	    snapshot.state == WLAN_STATE_FAILED &&
	    snapshot.wpa_state == WLAN_WPA2_STATE_IDLE);
	assert_station_retired(station);
	disconnect_station(device);

	/* Repeated link-loss/cancel leaves no PMK, key generation, beacon timer,
	 * scan generation, or reconnect timer owned by the common station. */
	for (iteration = 0U; iteration < 100U; iteration++) {
		uint64_t current = 2000U + iteration * 2U;

		fake->now++;
		fake->connect_generation = current;
		fake->connect_deadline = fake->now + WLAN_CONNECT_DEADLINE_TICKS;
		fake->selected = bss;
		assert(wlan_station_test_seed_authorized(station, &bss, current,
		    current + 500U) == 0);
		assert(wlan_station_report_link_loss(station, current, EIO) == 0);
		disconnect_station(device);
		assert_station_retired(station);
	}
}

static void
test_core(void)
{
	static const uint8_t target[] = { 'n', 'e', 't' };
	static const uint8_t filler[] = { 'x' };
	struct net_device device;
	struct net_device unsupported_device;
	struct net_device shutdown_device;
	struct net_device capacity_devices[NET_DEVICE_MAX - 1U];
	struct net_device overflow_device;
	struct wlan_station *station;
	struct wlan_station *unsupported_station;
	struct wlan_station *shutdown_station;
	struct wlan_station *capacity_stations[NET_DEVICE_MAX - 1U];
	struct fake_radio fake;
	struct fake_radio unsupported;
	struct fake_radio shutdown_fake;
	struct wlan_scan_request scan;
	struct wlan_scan_status_request scan_status;
	struct wlan_bss_request query;
	struct wlan_connect_request connect;
	struct wlan_disconnect_request disconnect;
	struct wlan_status_request status;
	struct wlan_station_test_snapshot station_snapshot;
	struct wlan_bss_record bss;
	uint8_t scan_frame[256];
	size_t scan_frame_length;
	struct wlan_scan_profile profile;
	struct wlan_scan_profile passive_profile;
	struct wlan_scan_profile invalid_profile;
	struct wlan_scan_profile multi_profile;
	struct wlan_radio_ops invalid_ops;
	struct scan_race_task race_tasks[4];
	pthread_t race_threads[4];
	uint64_t first_scan_generation;
	uint64_t saved_now;
	uint64_t shutdown_cancelled_deadline;
	unsigned index;
	unsigned carrier_calls_before;
	unsigned shutdown_scan_calls;
	int found_oldest;
	int connect_result;

	memset(&device, 0, sizeof(device));
	memset(&unsupported_device, 0, sizeof(unsupported_device));
	memset(&shutdown_device, 0, sizeof(shutdown_device));
	memset(capacity_devices, 0, sizeof(capacity_devices));
	memset(&overflow_device, 0, sizeof(overflow_device));
	memset(&fake, 0, sizeof(fake));
	memset(&unsupported, 0, sizeof(unsupported));
	memset(&shutdown_fake, 0, sizeof(shutdown_fake));
	memcpy(device.name, "wlan0", 6U);
	memcpy(unsupported_device.name, "wlan1", 6U);
	memcpy(shutdown_device.name, "wlan2", 6U);
	memcpy(device.hwaddr,
	    (uint8_t[6]){ 0x02U, 0U, 0U, 0U, 0U, 0x10U }, 6U);
	memcpy(unsupported_device.hwaddr,
	    (uint8_t[6]){ 0x02U, 0U, 0U, 0U, 0U, 0x11U }, 6U);
	memcpy(shutdown_device.hwaddr,
	    (uint8_t[6]){ 0x02U, 0U, 0U, 0U, 0U, 0x12U }, 6U);
	device.hwaddr_len = 6U;
	unsupported_device.hwaddr_len = 6U;
	shutdown_device.hwaddr_len = 6U;
	memcpy(fake.hwaddr, device.hwaddr, 6U);
	memcpy(unsupported.hwaddr, unsupported_device.hwaddr, 6U);
	memcpy(shutdown_fake.hwaddr, shutdown_device.hwaddr, 6U);
	device.flags = NET_DEVICE_UP;
	unsupported_device.flags = NET_DEVICE_UP;
	shutdown_device.flags = NET_DEVICE_UP;
	wlan_core_init();
	profile = test_scan_profile;
	invalid_profile = profile;
	invalid_profile.channel_count = 0U;
	assert(wlan_station_test_attach(&device, &fake_ops, &fake,
	    &invalid_profile, fake_clock, &fake, &station) == EINVAL);
	invalid_profile = profile;
	invalid_profile.channels[0].center_frequency_mhz++;
	assert(wlan_station_test_attach(&device, &fake_ops, &fake,
	    &invalid_profile, fake_clock, &fake, &station) == EINVAL);
	invalid_profile = profile;
	invalid_profile.channels[1].reserved = 1U;
	assert(wlan_station_test_attach(&device, &fake_ops, &fake,
	    &invalid_profile, fake_clock, &fake, &station) == EINVAL);
	invalid_profile = profile;
	invalid_profile.channel_count = 2U;
	invalid_profile.channels[1] = invalid_profile.channels[0];
	assert(wlan_station_test_attach(&device, &fake_ops, &fake,
	    &invalid_profile, fake_clock, &fake, &station) == EINVAL);
	invalid_ops = fake_ops;
	invalid_ops.management_transmit = NULL;
	assert(wlan_station_test_attach(&device, &invalid_ops, &fake, &profile,
	    fake_clock, &fake, &station) == EINVAL);
	net_device_ref_live_failure = 1;
	assert(wlan_station_test_attach(&device, &fake_ops, &fake, &profile,
	    fake_clock, &fake, &station) == ENODEV);
	assert(net_device_reference_balance == 0);
	net_device_ref_live_failure = 0;
	carrier_down_error = EIO;
	assert(wlan_station_test_attach(&device, &fake_ops, &fake, &profile,
	    fake_clock, &fake, &station) == EIO);
	assert(net_device_reference_balance == 0);
	carrier_down_error = 0;
	assert(wlan_station_test_attach(&device, &fake_ops, &fake,
	    &profile, fake_clock, &fake, &station) == 0);
	fake.station = station;
	device.carrier = 1U;
	device.flags |= NET_DEVICE_RUNNING;
	carrier_calls_before = carrier_down_calls;
	assert(wlan_station_test_attach(&device, &fake_ops, &fake,
	    &profile, fake_clock, &fake, &station) == EEXIST);
	assert(device.carrier == 1U);
	assert(carrier_down_calls == carrier_calls_before);
	device.carrier = 0U;
	device.flags &= ~NET_DEVICE_RUNNING;

	/* Capacity and duplicate validation precede device mutation. */
	passive_profile = profile;
	passive_profile.channels[0].flags = 0U;
	for (index = 0U; index < NET_DEVICE_MAX - 1U; index++) {
		capacity_devices[index].hwaddr_len = 6U;
		capacity_devices[index].hwaddr[0] = 0x02U;
		capacity_devices[index].hwaddr[5] = (uint8_t)(0x20U + index);
		capacity_devices[index].flags = NET_DEVICE_UP;
		assert(wlan_station_test_attach(&capacity_devices[index],
		    &empty_ops, NULL, &passive_profile, NULL, NULL,
		    &capacity_stations[index]) == 0);
	}
	overflow_device.hwaddr_len = 6U;
	overflow_device.hwaddr[0] = 0x02U;
	overflow_device.hwaddr[5] = 0x40U;
	overflow_device.carrier = 1U;
	overflow_device.flags = NET_DEVICE_UP | NET_DEVICE_RUNNING;
	carrier_calls_before = carrier_down_calls;
	assert(wlan_station_test_attach(&overflow_device, &empty_ops, NULL,
	    &passive_profile, NULL, NULL, &unsupported_station) == ENOSPC);
	assert(overflow_device.carrier == 1U);
	assert(carrier_down_calls == carrier_calls_before);
	assert(net_device_reference_balance == (int)NET_DEVICE_MAX);
	for (index = 0U; index < NET_DEVICE_MAX - 1U; index++)
		assert(wlan_station_detach(capacity_stations[index]) == 0);
	assert(net_device_reference_balance == 1);
	profile.channels[0].channel = 1U;
	profile.channels[0].center_frequency_mhz = 2412U;
	assert(wlan_station_open(station) == 0);
	test_validation(&device);
	fake.now = UINT64_MAX - WLAN_SCAN_DEADLINE_TICKS + 1U;
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == EOVERFLOW);
	assert(scan.state == WLAN_SCAN_IDLE);
	fake.now = 0U;
	{
		struct block_gate gate;
		struct scan_ioctl_task stop_task;
		struct timer_task timer_task;
		pthread_t timer_thread;
		pthread_t stop_thread;
		unsigned spins;

		block_gate_init(&gate);
		memset(&stop_task, 0, sizeof(stop_task));
		stop_task.device = &device;
		request_header(&stop_task.request,
		    sizeof(stop_task.request), "wlan0");
		stop_task.request.action = WLAN_SCAN_STOP;
		fake.scan_start_gate = &gate;
		request_header(&scan, sizeof(scan), "wlan0");
		scan.action = WLAN_SCAN_START;
		assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
		timer_task.now = fake.now;
		assert(pthread_create(&timer_thread, NULL, timer_main,
		    &timer_task) == 0);
		block_gate_wait(&gate);
		assert(pthread_create(&stop_thread, NULL, scan_ioctl_main,
		    &stop_task) == 0);
		for (spins = 0U; spins < 100000U &&
		    wlan_station_test_control_waiters(station) == 0U; spins++)
			assert(sched_yield() == 0);
		assert(wlan_station_test_control_waiters(station) != 0U);
		assert(fake.scan_stop_calls == 0U);
		block_gate_release(&gate);
		assert(pthread_join(timer_thread, NULL) == 0);
		assert(pthread_join(stop_thread, NULL) == 0);
		assert(stop_task.error == 0);
		assert(fake.scan_stop_saw_start_returned);
		assert(wlan_station_test_control_waiters(station) == 0U);
		fake.scan_start_gate = NULL;
		fake.scan_start_calls = 0U;
		fake.scan_stop_calls = 0U;
		fake.scan_start_returned = 0;
		fake.scan_stop_saw_start_returned = 0;
		worker_wake_calls = 0U;
		block_gate_destroy(&gate);
	}

	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	fake.now = 100U;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	assert(scan.state == WLAN_SCAN_RUNNING && scan.generation != 0U);
	assert(worker_wake_calls == 1U);
	first_scan_generation = scan.generation;
	scan_drive_to_dwell(station, &fake, first_scan_generation);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.deadline_ticks ==
	    100U + WLAN_SCAN_DEADLINE_TICKS);
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	assert(scan.generation == first_scan_generation);
	assert(fake.scan_start_calls == 1U);
	assert(wlan_station_report_scan_frame(station, first_scan_generation,
	    NULL, WLAN_MANAGEMENT_FRAME_MAX + 1U, -30, 6U) == EMSGSIZE);

	bss = make_bss(1U, target, sizeof(target), -30);
	assert(wlan_station_report_scan_bss(station, first_scan_generation,
	    &bss) == 0);
	fake.now++;
	bss = make_bss(2U, target, sizeof(target), -30);
	assert(wlan_station_report_scan_bss(station, first_scan_generation,
	    &bss) == 0);
	for (index = 3U; index <= 64U; index++) {
		fake.now++;
		bss = make_bss((uint8_t)index, filler, sizeof(filler), -100);
		assert(wlan_station_report_scan_bss(station,
		    first_scan_generation, &bss) == 0);
	}
	fake.now++;
	bss = make_bss(254U, filler, sizeof(filler), -20);
	assert(wlan_station_report_scan_bss(station, first_scan_generation,
	    &bss) == 0);
	fake.now++;
	bss = make_bss(253U, target, sizeof(target), -1);
	bss.capability = 0x0431U;
	bss.security |= WLAN_SECURITY_SAE;
	assert(wlan_station_report_scan_bss(station, first_scan_generation,
	    &bss) == 0);
	scan_frame_length = build_frame_ciphers(scan_frame, target,
	    sizeof(target), 252U, 4U, 6U, 2U, 0U, 1);
	assert(wlan_station_report_scan_frame(station, first_scan_generation,
	    scan_frame, scan_frame_length, -2, 6U) == 0);
	scan_frame_length = build_frame_ciphers(scan_frame, target,
	    sizeof(target), 251U, 2U, 4U, 2U, 0U, 1);
	assert(wlan_station_report_scan_frame(station, first_scan_generation,
	    scan_frame, scan_frame_length, -3, 6U) == 0);
	scan_frame_length = build_frame_ciphers(scan_frame, target,
	    sizeof(target), 250U, 6U, 4U, 2U, 0U, 1);
	assert(wlan_station_report_scan_frame(station, first_scan_generation,
	    scan_frame, scan_frame_length, -4, 6U) == 0);
	fake.now++;
	bss = make_bss(1U, target, sizeof(target), -30);
	assert(wlan_station_report_scan_bss(station, first_scan_generation,
	    &bss) == 0);
	scan_complete(station, &fake, first_scan_generation);

	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_COMPLETE);
	assert(scan_status.generation == first_scan_generation);
	assert(scan_status.scan_generation == first_scan_generation);
	assert(scan_status.result_count == WLAN_BSS_MAX);
	assert(scan_status.truncated == 1U);
	request_header(&query, sizeof(query), "wlan0");
	query.generation = first_scan_generation + 1U;
	assert(wlan_station_ioctl(&device, SIOCGWLANBSS, &query) == ESTALE);
	request_header(&query, sizeof(query), "wlan0");
	query.generation = first_scan_generation;
	query.index = WLAN_BSS_MAX;
	assert(wlan_station_ioctl(&device, SIOCGWLANBSS, &query) == ENOENT);
	found_oldest = 0;
	for (index = 0U; index < WLAN_BSS_MAX; index++) {
		request_header(&query, sizeof(query), "wlan0");
		query.generation = first_scan_generation;
		query.index = index;
		assert(wlan_station_ioctl(&device, SIOCGWLANBSS, &query) == 0);
		if (query.bss.bssid[5] == 3U)
			found_oldest = 1;
	}
	assert(!found_oldest);

	/* A new staging generation may change concurrently, while readers see
	 * only the immutable last-complete snapshot and coherent status. */
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	memset(race_tasks, 0, sizeof(race_tasks));
	for (index = 0U; index < 4U; index++) {
		race_tasks[index].device = &device;
		race_tasks[index].station = station;
		race_tasks[index].scan_generation = scan.generation;
		race_tasks[index].snapshot_generation = first_scan_generation;
		race_tasks[index].bss = make_bss(200U, filler,
		    sizeof(filler), -50);
		race_tasks[index].writer = index < 2U;
		assert(pthread_create(&race_threads[index], NULL,
		    scan_race_main, &race_tasks[index]) == 0);
	}
	for (index = 0U; index < 4U; index++) {
		assert(pthread_join(race_threads[index], NULL) == 0);
		assert(race_tasks[index].error == 0);
	}
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_STOP;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.generation == first_scan_generation);
	assert(scan_status.scan_generation == scan.generation);
	assert(scan_status.result_count == WLAN_BSS_MAX);
	saved_now = fake.now;
	fake.now = UINT64_MAX - WLAN_CONNECT_DEADLINE_TICKS + 1U;
	request_header(&connect, sizeof(connect), "wlan0");
	memcpy(connect.ssid, target, sizeof(target));
	connect.ssid_length = sizeof(target);
	memset(connect.passphrase, 0x7a, sizeof(connect.passphrase));
	connect.passphrase_length = WLAN_PASSPHRASE_MIN;
	assert(wlan_station_ioctl(&device, SIOCSWLANCONNECT, &connect) ==
	    EOVERFLOW);
	assert(wlan_station_test_secrets_clear(station));
	fake.now = saved_now;

	request_header(&connect, sizeof(connect), "wlan0");
	memcpy(connect.ssid, "missing", 7U);
	connect.ssid_length = 7U;
	memset(connect.passphrase, 0xa5, sizeof(connect.passphrase));
	connect.passphrase_length = 8U;
	assert(wlan_station_ioctl(&device, SIOCSWLANCONNECT, &connect) ==
	    ENOENT);
	assert(memcmp(connect.passphrase,
	    (uint8_t[WLAN_PASSPHRASE_STORAGE]){ 0 },
	    sizeof(connect.passphrase)) == 0);
	assert(wlan_station_test_secrets_clear(station));

	request_header(&connect, sizeof(connect), "wlan0");
	memcpy(connect.ssid, target, sizeof(target));
	connect.ssid_length = sizeof(target);
	memset(connect.passphrase, 0xa5, sizeof(connect.passphrase));
	connect.passphrase_length = WLAN_PASSPHRASE_MIN;
	connect_result = wlan_station_ioctl(&device, SIOCSWLANCONNECT,
	    &connect);
	if (connect_result != 0) {
		request_header(&status, sizeof(status), "wlan0");
		assert(wlan_station_ioctl(&device, SIOCGWLANSTATUS, &status) == 0);
		fprintf(stderr, "initial WPA2 connect failed: %d starts=%u "
		    "bssid=%u station=%u channel=%u now=%llu driver-deadline=%llu "
		    "total=%llu state=%u\n", connect_result,
		    fake.connect_start_calls, status.bssid[5],
		    device.hwaddr[5], status.channel,
		    (unsigned long long)fake.now,
		    (unsigned long long)fake.connect_deadline,
		    (unsigned long long)status.deadline_ticks, status.state);
	}
	assert(connect_result == 0);
	assert(connect.state == WLAN_STATE_AUTHENTICATING);
	assert(memcmp(connect.passphrase,
	    (uint8_t[WLAN_PASSPHRASE_STORAGE]){ 0 },
	    sizeof(connect.passphrase)) == 0);
	/* A transition AP advertising PSK and SAE remains usable through its
	 * explicitly selected PSK AKM. */
	assert(fake.selected.bssid[5] == 253U);
	assert(fake.selected.capability == 0x0431U);
	assert(wlan_station_test_snapshot(station, &station_snapshot) == 0 &&
	    station_snapshot.association_capability == 0x0011U);
	assert(fake.connect_deadline == fake.now +
	    WLAN_CONNECT_TRANSITION_TICKS);
	assert(worker_wake_calls >= 2U);
	assert(!wlan_station_test_secrets_clear(station));
	request_header(&status, sizeof(status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSTATUS, &status) == 0);
	assert(status.state == WLAN_STATE_AUTHENTICATING &&
	    status.controlled_port == 0U && device.carrier == 0U);
	/* Link loss during the initial (not yet reconnectable) handshake still
	 * performs a checked stop and leaves no stale timer or transmit producer. */
	assert(wlan_station_report_link_loss(station, connect.generation,
	    ECONNRESET) == 0);
	request_header(&status, sizeof(status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSTATUS, &status) == 0);
	assert(status.state == WLAN_STATE_FAILED &&
	    status.controlled_port == 0U && device.carrier == 0U &&
	    wlan_station_test_secrets_clear(station));
	request_header(&disconnect, sizeof(disconnect), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCSWLANDISCONNECT,
	    &disconnect) == 0);
	assert(disconnect.state == WLAN_STATE_IDLE);
	assert(wlan_station_test_secrets_clear(station));
	test_common_wpa_lifecycle(&device, station, &fake);

	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	assert(wlan_timer_next_deadline() ==
	    fake.now + WLAN_SCAN_DWELL_TICKS);
	fake.scan_stop_error = EBUSY;
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_STOP;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == EBUSY);
	assert(scan.state == WLAN_SCAN_FAILED);
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == EBUSY);
	fake.scan_stop_error = 0;
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_STOP;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	index = fake.scan_stop_calls;
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_STOP;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	assert(fake.scan_stop_calls == index);
	assert(wlan_station_report_scan_bss(station, scan.generation, &bss) ==
	    ESTALE);
	request_header(&query, sizeof(query), "wlan0");
	query.generation = first_scan_generation;
	assert(wlan_station_ioctl(&device, SIOCGWLANBSS, &query) == 0);

	/* A failed final stop is terminal for that generation.  The later cleanup
	 * retry may retire the producer, but must never commit or flip FAILED to
	 * COMPLETE. */
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	fake.scan_stop_error = EBUSY;
	scan_complete(station, &fake, scan.generation);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED &&
	    scan_status.terminal_error == EBUSY &&
	    scan_status.generation == first_scan_generation);
	fake.scan_stop_error = 0;
	fake.now++;
	wlan_timer_run(fake.now);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED &&
	    scan_status.generation == first_scan_generation);

	/* Driver error is an IRQ-safe persistent latch; only the worker calls the
	 * synchronous stop barrier and publishes the terminal state. */
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	wlan_timer_run(fake.now);
	index = fake.scan_stop_calls;
	assert(wlan_station_report_scan_error(station, scan.generation, 0) ==
	    EINVAL);
	assert(wlan_station_report_scan_error(station, scan.generation + 1U,
	    EIO) == ESTALE);
	assert(wlan_station_report_scan_error(station, scan.generation, EIO) == 0);
	assert(fake.scan_stop_calls == index && wlan_work_pending());
	wlan_timer_run(fake.now);
	assert(fake.scan_stop_calls == index + 1U);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED &&
	    scan_status.terminal_error == EIO);

	/* A ready acknowledgement does not override its tune deadline. */
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	wlan_timer_run(fake.now);
	assert(wlan_station_report_scan_channel_ready(station, scan.generation,
	    0U) == 0);
	fake.now = fake.scan_step_deadline;
	wlan_timer_run(fake.now);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED &&
	    scan_status.terminal_error == ETIMEDOUT);

	/* A start callback that reports ready but returns only at its deadline is
	 * rechecked against a fresh clock sample before any probe is sent. */
	fake.scan_start_report_ready = 1;
	fake.scan_start_advance = WLAN_SCAN_TUNE_DEADLINE_TICKS;
	index = fake.management_calls;
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	wlan_timer_run(fake.now);
	assert(fake.management_calls == index);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED &&
	    scan_status.terminal_error == ETIMEDOUT);
	fake.scan_start_report_ready = 0;
	fake.scan_start_advance = 0U;

	/* Probe-transmit failure is likewise retired only by the worker barrier. */
	fake.management_error = EIO;
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	wlan_timer_run(fake.now);
	assert(wlan_station_report_scan_channel_ready(station, scan.generation,
	    0U) == 0);
	wlan_timer_run(fake.now);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED &&
	    scan_status.terminal_error == EIO);
	fake.management_error = 0;

	/* Snapshot publication rechecks the total deadline after the synchronous
	 * stop barrier returns. */
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	fake.scan_stop_advance = scan_status.deadline_ticks -
	    (fake.now + WLAN_SCAN_DWELL_TICKS);
	scan_complete(station, &fake, scan.generation);
	fake.scan_stop_advance = 0U;
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED &&
	    scan_status.terminal_error == ETIMEDOUT);

	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	fake.now = scan_status.deadline_ticks;
	index = fake.scan_stop_calls;
	bss = make_bss(210U, filler, sizeof(filler), -70);
	assert(wlan_station_report_scan_bss(station, scan.generation, &bss) ==
	    ETIMEDOUT);
	assert(wlan_station_report_scan_error(station, scan.generation, EIO) == 0);
	assert(fake.scan_stop_calls == index);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_RUNNING);
	fake.scan_stop_error = EBUSY;
	wlan_timer_run(fake.now);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_FAILED);
	assert(scan_status.terminal_error == ETIMEDOUT);
	assert(scan_status.generation == first_scan_generation);
	assert(wlan_timer_next_deadline() == fake.now + 1U);
	fake.scan_stop_error = 0;
	fake.now++;
	wlan_timer_run(fake.now);

	fake.now++;
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	scan_complete(station, &fake, scan.generation);
	request_header(&scan_status, sizeof(scan_status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.result_count == 0U);
	request_header(&query, sizeof(query), "wlan0");
	query.generation = scan_status.generation;
	assert(wlan_station_ioctl(&device, SIOCGWLANBSS, &query) == ENOENT);

	memset(&multi_profile, 0, sizeof(multi_profile));
	multi_profile.channel_count = 3U;
	multi_profile.channels[0].channel = 1U;
	multi_profile.channels[0].center_frequency_mhz = 2412U;
	multi_profile.channels[1].channel = 14U;
	multi_profile.channels[1].center_frequency_mhz = 2484U;
	multi_profile.channels[1].flags = WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED;
	multi_profile.channels[2].channel = 6U;
	multi_profile.channels[2].center_frequency_mhz = 2437U;
	multi_profile.channels[2].flags = WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED;
	assert(wlan_station_test_attach(&unsupported_device, &scan_only_ops,
	    &unsupported, &multi_profile, fake_clock, &unsupported,
	    &unsupported_station) == 0);
	unsupported.station = unsupported_station;
	assert(wlan_station_open(unsupported_station) == 0);
	request_header(&connect, sizeof(connect), "wlan1");
	connect.ssid_length = 0U;
	memset(connect.passphrase, 0xa5, sizeof(connect.passphrase));
	connect.passphrase_length = WLAN_PASSPHRASE_MIN;
	assert(wlan_station_ioctl(&unsupported_device, SIOCSWLANCONNECT,
	    &connect) == EOPNOTSUPP);
	assert(memcmp(connect.passphrase,
	    (uint8_t[WLAN_PASSPHRASE_STORAGE]){ 0 },
	    sizeof(connect.passphrase)) == 0);
	request_header(&scan, sizeof(scan), "wlan1");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&unsupported_device, SIOCSWLANSCAN, &scan) == 0);
	wlan_timer_run(unsupported.now);
	assert(unsupported.scan_start_calls == 1U);
	assert(unsupported.scan_steps[0] == 0U &&
	    unsupported.scan_channels[0] == 1U);
	assert(wlan_station_report_scan_channel_ready(unsupported_station,
	    scan.generation, 0U) == 0);
	wlan_timer_run(unsupported.now);
	assert(unsupported.management_calls == 0U);
	unsupported.now += WLAN_SCAN_DWELL_TICKS;
	wlan_timer_run(unsupported.now);
	assert(unsupported.scan_steps[1] == 1U &&
	    unsupported.scan_channels[1] == 14U);
	assert(wlan_station_report_scan_channel_ready(unsupported_station,
	    scan.generation, 1U) == 0);
	wlan_timer_run(unsupported.now);
	assert(unsupported.management_calls == 1U);
	unsupported.now += WLAN_SCAN_DWELL_TICKS;
	wlan_timer_run(unsupported.now);
	assert(unsupported.scan_steps[2] == 2U &&
	    unsupported.scan_channels[2] == 6U);
	assert(wlan_station_report_scan_channel_ready(unsupported_station,
	    scan.generation, 2U) == 0);
	wlan_timer_run(unsupported.now);
	assert(unsupported.management_calls == 2U);
	unsupported.now += WLAN_SCAN_DWELL_TICKS;
	wlan_timer_run(unsupported.now);
	request_header(&scan_status, sizeof(scan_status), "wlan1");
	assert(wlan_station_ioctl(&unsupported_device, SIOCGWLANSCAN,
	    &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_COMPLETE &&
	    scan_status.generation == scan.generation);
	request_header(&scan, sizeof(scan), "wlan1");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&unsupported_device, SIOCSWLANSCAN, &scan) == 0);
	wlan_timer_run(unsupported.now);
	assert(wlan_station_report_scan_channel_ready(unsupported_station,
	    scan.generation, 0U) == 0);
	wlan_timer_run(unsupported.now);
	{
		struct block_gate gate;
		struct blocked_report report;
		pthread_t thread;

		block_gate_init(&gate);
		memset(&report, 0, sizeof(report));
		report.station = unsupported_station;
		report.generation = scan.generation;
		report.bss = make_bss(203U, filler, sizeof(filler), -60);
		report.bss.channel = 1U;
		report.bss.center_frequency_mhz = 2412U;
		assert(wlan_station_test_set_report_hook(unsupported_station,
		    block_gate_hook, &gate) == 0);
		assert(pthread_create(&thread, NULL, blocked_report_main,
		    &report) == 0);
		block_gate_wait(&gate);
		assert(wlan_station_detach(unsupported_station) == EBUSY);
		block_gate_release(&gate);
		assert(pthread_join(thread, NULL) == 0);
		assert(report.error == 0);
		block_gate_destroy(&gate);
	}
	unsupported.quiesce_error = EBUSY;
	assert(wlan_station_detach(unsupported_station) == EBUSY);
	assert(net_device_reference_balance == 2);
	unsupported.quiesce_error = 0;
	assert(wlan_station_detach(unsupported_station) == 0);
	assert(net_device_reference_balance == 1);

	/* Global shutdown is also a cancellation boundary while a finite
	 * reconnect delay is pending.  Keep this independent station asleep at
	 * the two-second backoff while the primary station exercises shutdown's
	 * active-callback and quiesce retry barriers below. */
	bss = make_bss(0x73U, target, sizeof(target), -32);
	shutdown_fake.now = 30000U;
	shutdown_fake.connect_generation = 1600U;
	shutdown_fake.connect_deadline = shutdown_fake.now +
	    WLAN_CONNECT_DEADLINE_TICKS;
	shutdown_fake.selected = bss;
	assert(wlan_station_test_attach(&shutdown_device, &fake_ops,
	    &shutdown_fake, &test_scan_profile, fake_clock, &shutdown_fake,
	    &shutdown_station) == 0);
	shutdown_fake.station = shutdown_station;
	assert(net_device_reference_balance == 2);
	assert(wlan_station_open(shutdown_station) == 0);
	assert(wlan_station_test_seed_authorized(shutdown_station, &bss,
	    1600U, 1610U) == 0);
	assert(wlan_station_report_link_loss(shutdown_station, 1600U, EIO) == 0);
	reconnect_fail_scans(shutdown_station, &shutdown_fake, 2U);
	assert(wlan_station_test_snapshot(shutdown_station,
	    &station_snapshot) == 0 && station_snapshot.reconnect_pending &&
	    station_snapshot.reconnect_next_attempt == shutdown_fake.now + 200U);
	shutdown_cancelled_deadline = station_snapshot.reconnect_next_attempt;

	assert(wlan_station_open(station) == 0);
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	{
		struct block_gate gate;
		struct blocked_report report;
		pthread_t thread;

		block_gate_init(&gate);
		memset(&report, 0, sizeof(report));
		report.station = station;
		report.generation = scan.generation;
		report.bss = make_bss(201U, filler, sizeof(filler), -60);
		assert(wlan_station_test_set_report_hook(station,
		    block_gate_hook, &gate) == 0);
		assert(pthread_create(&thread, NULL, blocked_report_main,
		    &report) == 0);
		block_gate_wait(&gate);
		assert(wlan_station_close(station) == EBUSY);
		block_gate_release(&gate);
		assert(pthread_join(thread, NULL) == 0);
		assert(report.error == 0);
		block_gate_destroy(&gate);
	}
	fake.scan_stop_error = EBUSY;
	assert(wlan_station_close(station) == EBUSY);
	request_header(&status, sizeof(status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSTATUS, &status) == ENODEV);
	fake.scan_stop_error = 0;
	assert(wlan_station_close(station) == 0);
	assert(wlan_station_test_secrets_clear(station));
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == ENETDOWN);
	assert(wlan_station_open(station) == 0);
	request_header(&scan, sizeof(scan), "wlan0");
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_drive_to_dwell(station, &fake, scan.generation);
	{
		struct block_gate gate;
		struct blocked_report report;
		pthread_t thread;

		block_gate_init(&gate);
		memset(&report, 0, sizeof(report));
		report.station = station;
		report.generation = scan.generation;
		report.bss = make_bss(202U, filler, sizeof(filler), -60);
		assert(wlan_station_test_set_report_hook(station,
		    block_gate_hook, &gate) == 0);
		assert(pthread_create(&thread, NULL, blocked_report_main,
		    &report) == 0);
		block_gate_wait(&gate);
		fake.quiesce_error = EBUSY;
		assert(wlan_station_shutdown_all() == EBUSY);
		block_gate_release(&gate);
		assert(pthread_join(thread, NULL) == 0);
		assert(report.error == 0);
		block_gate_destroy(&gate);
	}
	assert(wlan_station_shutdown_all() == EBUSY);
	assert(net_device_reference_balance == 1);
	assert_station_retired(shutdown_station);
	shutdown_scan_calls = shutdown_fake.scan_start_calls;
	shutdown_fake.now = shutdown_cancelled_deadline;
	wlan_timer_run(shutdown_fake.now);
	assert(shutdown_fake.scan_start_calls == shutdown_scan_calls);
	request_header(&status, sizeof(status), "wlan0");
	assert(wlan_station_ioctl(&device, SIOCGWLANSTATUS, &status) == ENODEV);
	assert(wlan_station_test_secrets_clear(station));
	unsupported_device.carrier = 1U;
	unsupported_device.flags |= NET_DEVICE_RUNNING;
	carrier_calls_before = carrier_down_calls;
	assert(wlan_station_test_attach(&unsupported_device, &scan_only_ops,
	    &unsupported, &multi_profile, fake_clock, &unsupported,
	    &unsupported_station) == EBUSY);
	assert(unsupported_device.carrier == 1U);
	assert(carrier_down_calls == carrier_calls_before);
	fake.quiesce_error = 0;
	assert(wlan_station_shutdown_all() == 0);
	assert(wlan_station_test_secrets_clear(station));
	assert(net_device_reference_balance == 0);
}

int
main(void)
{
	test_frame_parser();
	test_core();
	assert(wlan_station_shutdown_all() == 0);
	return 0;
}
