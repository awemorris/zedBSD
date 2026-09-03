/*
 * Intel AX211 private-radio/common-WLAN integration fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "kern/lock.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/wlan.h"
#include "kern/net/wlan-crypto.h"
#include "kern/net/wlan-l2.h"
#include "kern/net/wlan-wpa2.h"
#include "kern/net/wlan-wpa2-codec.h"

#include "../../../src/drivers/intel-ax211-assoc.h"
#include "../../../src/drivers/intel-ax211-bss.h"
#include "../../../src/drivers/intel-ax211-key.h"
#include "../../../src/drivers/intel-ax211-rx.h"
#include "../../../src/drivers/intel-ax211-scan.h"
#include "../../../src/drivers/intel-ax211-tx.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))
#define FIXTURE_HARDWARE_EPOCH 9U
#define FIXTURE_COMMAND_BUDGET 64U
#define RX_STATUS_CRC_OK 0x00000001U
#define RX_STATUS_OVERRUN_OK 0x00000002U
#define RX_STATUS_MIC_OK 0x00000040U
#define RX_STATUS_CCMP 0x00000200U
#define RX_STATUS_DECRYPTED 0x00000800U
#define FIXTURE_CHANNEL 36U
#define FIXTURE_FREQUENCY_MHZ 5180U

static const uint8_t fixture_station[6] = {
	0x02U, 0x16U, 0x3eU, 0x21U, 0x10U, 0x01U
};
static const uint8_t fixture_bssid[6] = {
	0x02U, 0x16U, 0x3eU, 0x21U, 0x20U, 0x01U
};
static const uint8_t fixture_peer[6] = {
	0x02U, 0x16U, 0x3eU, 0x21U, 0x30U, 0x01U
};
static const uint8_t fixture_ssid[] = {
	'f', 'i', 'x', 't', 'u', 'r', 'e', '-', 'a', 'p'
};
static const uint8_t fixture_passphrase[] = "fixture-passphrase";
static const uint8_t fixture_rsn[WLAN_WPA2_RSN_IE_LENGTH] = {
	48U, 20U, 1U, 0U, 0U, 15U, 172U, 4U,
	1U, 0U, 0U, 15U, 172U, 4U, 1U, 0U,
	0U, 15U, 172U, 2U, 0U, 0U
};

struct fixture_packet {
	struct packet_buf packet;
	uint8_t storage[PACKET_BUF_STORAGE_SIZE];
	uint8_t used;
};

struct finite_transport {
	unsigned remaining;
	unsigned scan_commands;
	unsigned association_commands;
	unsigned key_commands;
	unsigned tx_commands;
	unsigned rx_events;
};

struct ax211_radio_fixture {
	struct wlan_station *station;
	struct finite_transport transport;
	struct intel_ax211_protocol_command_table table;
	struct intel_ax211_scan_state scan;
	struct intel_ax211_assoc_state association;
	struct intel_ax211_key_state keys;
	uint8_t table_bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint64_t now;
	uint64_t scan_generation;
	uint64_t connection_generation;
	uint64_t pairwise_generation;
	uint64_t group_generation;
	uint64_t pending_generation;
	uint64_t pending_cookie;
	uint8_t pending_frame[WLAN_MANAGEMENT_FRAME_MAX];
	size_t pending_length;
	uint8_t pending;
	uint8_t selected_bssid[6];
	unsigned scan_publications;
	unsigned assoc_exchanges;
	unsigned pairwise_installs;
	unsigned group_installs;
	unsigned data_tx;
	unsigned data_rx;
};

static unsigned receive_count;
static uint8_t received_ethernet[128];
static size_t received_length;
static int reference_balance;
static struct fixture_packet packet_pool[4];

static void put_le16(uint8_t *bytes, uint16_t value);
static void put_le32(uint8_t *bytes, uint32_t value);
static uint32_t get_le32(const uint8_t *bytes);
static void put_be16(uint8_t *bytes, uint16_t value);
static void put_be64(uint8_t *bytes, uint64_t value);
static void request_header(void *request, size_t size);
static void table_version(uint8_t *bytes, size_t index, uint8_t group,
	uint8_t opcode, uint8_t command_version, uint8_t notification_version);
static void make_api89_table(struct ax211_radio_fixture *fixture);
static int transport_take(struct ax211_radio_fixture *fixture,
	unsigned *counter);
static uint64_t fixture_clock(void *context);
static struct intel_ax211_assoc_reply assoc_reply(
	const struct intel_ax211_assoc_command *command);
static int assoc_exchange(void *argument,
	const struct intel_ax211_assoc_command *command,
	struct intel_ax211_assoc_reply *reply);
static uint64_t assoc_clock(void *argument);
static int radio_scan_channel_start(void *context, uint64_t generation,
	uint32_t step_index, uint32_t channel, uint64_t deadline);
static int radio_scan_stop(void *context, uint64_t generation);
static int radio_management_transmit(void *context, uint64_t generation,
	const uint8_t *frame, size_t length, uint64_t deadline);
static int radio_connect_start(void *context, uint64_t generation,
	const struct wlan_bss_record *bss, uint64_t deadline);
static int radio_disconnect(void *context, uint64_t generation);
static int radio_association_set(void *context, uint64_t generation,
	const uint8_t bssid[6], uint16_t aid, uint64_t deadline);
static int radio_association_clear(void *context, uint64_t generation,
	uint64_t deadline);
static int radio_frame_transmit(void *context,
	const struct wlan_radio_tx_request *request);
static int radio_key_install(void *context,
	const struct wlan_radio_key_request *request);
static int radio_key_delete(void *context, uint64_t generation,
	enum wlan_radio_key_kind kind, uint8_t key_index,
	uint64_t key_generation, uint64_t deadline);
static int radio_keys_activate(void *context, uint64_t generation,
	uint64_t pairwise_generation, uint64_t group_generation,
	uint64_t deadline);
static int radio_quiesce(void *context);
static size_t build_beacon(uint8_t *frame);
static size_t build_authentication_response(uint8_t *frame);
static size_t build_association_response(uint8_t *frame);
static void assert_association_rates(const uint8_t *frame, size_t length);
static size_t build_from_ds_data(uint8_t *frame, const uint8_t source[6],
	uint16_t ether_type, const uint8_t *payload, size_t payload_length,
	int protected_frame, uint8_t key_index, uint64_t packet_number);
static int deliver_rx(struct ax211_radio_fixture *fixture,
	uint64_t common_generation, const uint8_t *frame, size_t frame_length,
	int protected_frame);
static void complete_pending(struct ax211_radio_fixture *fixture);
static void fill_bytes(uint8_t *bytes, size_t length, uint8_t seed);
static void ordered_copy(uint8_t *output, const uint8_t *left,
	const uint8_t *right, size_t length);
static void derive_ptk(const uint8_t snonce[WLAN_WPA2_NONCE_LENGTH],
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH],
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH]);
static void eapol_sign(uint8_t *frame, size_t length,
	const uint8_t kck[WLAN_WPA2_KCK_LENGTH]);
static void xor_t(uint8_t accumulator[8], uint64_t value);
static size_t rfc3394_wrap(const uint8_t kek[WLAN_WPA2_KEK_LENGTH],
	const uint8_t *plaintext, size_t plaintext_length, uint8_t *wrapped,
	size_t capacity);
static size_t build_message_1(uint8_t *frame, uint64_t replay,
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH]);
static size_t build_message_3(uint8_t *frame, uint64_t replay,
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH], const uint8_t *ptk,
	const uint8_t gtk[WLAN_WPA2_GTK_LENGTH], uint8_t gtk_index,
	uint64_t receive_pn);
static void run_integration(void);

static const struct intel_ax211_assoc_ops assoc_ops = {
	.clock_us = assoc_clock,
	.exchange = assoc_exchange
};

static const struct wlan_radio_ops radio_ops = {
	.scan_channel_start = radio_scan_channel_start,
	.scan_stop = radio_scan_stop,
	.connect_start = radio_connect_start,
	.disconnect = radio_disconnect,
	.management_transmit = radio_management_transmit,
	.association_set = radio_association_set,
	.association_clear = radio_association_clear,
	.frame_transmit = radio_frame_transmit,
	.key_install = radio_key_install,
	.key_delete = radio_key_delete,
	.keys_activate = radio_keys_activate,
	.quiesce = radio_quiesce
};

void
net_worker_wakeup(void)
{
}

int
net_device_ref_live(struct net_device *device)
{
	assert(device != NULL);
	reference_balance++;
	return 1;
}

void
net_device_release(struct net_device *device)
{
	assert(device != NULL && reference_balance > 0);
	reference_balance--;
}

int
net_device_set_carrier(struct net_device *device, int carrier)
{
	assert(device != NULL);
	device->carrier = carrier != 0;
	if (device->carrier)
		device->flags |= NET_DEVICE_RUNNING;
	else
		device->flags &= ~NET_DEVICE_RUNNING;
	return 0;
}

void
net_device_tx_error(struct net_device *device)
{
	assert(device != NULL);
}

struct packet_buf *
packet_buf_alloc(size_t headroom)
{
	struct fixture_packet *slot = NULL;
	size_t index;

	if (headroom > PACKET_BUF_STORAGE_SIZE)
		return NULL;
	for (index = 0U; index < ARRAY_COUNT(packet_pool); index++) {
		if (!packet_pool[index].used) {
			slot = &packet_pool[index];
			break;
		}
	}
	if (slot == NULL)
		return NULL;
	memset(&slot->packet, 0, sizeof(slot->packet));
	slot->used = 1U;
	slot->packet.storage = slot->storage;
	slot->packet.data = slot->storage + headroom;
	slot->packet.capacity = sizeof(slot->storage);
	slot->packet.l2_offset = PACKET_OFFSET_NONE;
	slot->packet.l3_offset = PACKET_OFFSET_NONE;
	slot->packet.l4_offset = PACKET_OFFSET_NONE;
	return &slot->packet;
}

void *
packet_buf_append(struct packet_buf *packet, size_t length)
{
	void *tail;
	size_t headroom;

	if (packet == NULL)
		return NULL;
	headroom = (size_t)(packet->data - packet->storage);
	if (headroom > packet->capacity || packet->length >
	    packet->capacity - headroom || length >
	    packet->capacity - headroom - packet->length)
		return NULL;
	tail = packet->data + packet->length;
	packet->length += length;
	return tail;
}

void
packet_buf_free(struct packet_buf *packet)
{
	struct fixture_packet *slot;

	if (packet == NULL)
		return;
	slot = (struct fixture_packet *)packet;
	assert(slot >= packet_pool && slot < packet_pool + ARRAY_COUNT(packet_pool));
	assert(slot->used);
	memset(&slot->packet, 0, sizeof(slot->packet));
	slot->used = 0U;
}

void
net_device_receive(struct net_device *device, struct packet_buf *packet)
{
	assert(device != NULL && packet != NULL);
	assert(packet->length <= sizeof(received_ethernet));
	receive_count++;
	received_length = packet->length;
	memcpy(received_ethernet, packet->data, packet->length);
	packet_buf_free(packet);
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

bool
hal_entropy_fill(void *buffer, size_t length)
{
	memset(buffer, 0x5a, length);
	return true;
}

static void
put_le16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
put_be16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)(value >> 8);
	bytes[1] = (uint8_t)value;
}

static void
put_be64(uint8_t *bytes, uint64_t value)
{
	unsigned index;

	for (index = 0U; index < 8U; index++) {
		bytes[7U - index] = (uint8_t)value;
		value >>= 8;
	}
}

static void
request_header(void *request, size_t size)
{
	struct wlan_ioctl_header *header = request;

	memset(request, 0, size);
	memcpy(header->ifr_name, "wlan0", 6U);
	header->version = WLAN_ABI_VERSION;
	header->size = (uint32_t)size;
}

static void
table_version(uint8_t *bytes, size_t index, uint8_t group, uint8_t opcode,
	uint8_t command_version, uint8_t notification_version)
{
	uint8_t *entry = bytes +
	    index * INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;

	entry[0] = opcode;
	entry[1] = group;
	entry[2] = command_version;
	entry[3] = notification_version;
}

static void
make_api89_table(struct ax211_radio_fixture *fixture)
{
	size_t index;

	for (index = 0U;
	    index < INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U; index++)
		table_version(fixture->table_bytes, index, 0x10U,
		    (uint8_t)index, 1U, 1U);
	table_version(fixture->table_bytes, 0U, 0U, 0x01U, 99U, 6U);
	table_version(fixture->table_bytes, 1U, 1U, 0x0cU, 5U, 0U);
	table_version(fixture->table_bytes, 2U, 1U, 0x0dU, 17U, 0U);
	table_version(fixture->table_bytes, 3U, 12U, 0x00U, 1U, 0U);
	table_version(fixture->table_bytes, 4U, 12U, 0x02U, 1U, 4U);
	table_version(fixture->table_bytes, 5U, 12U, 0xfeU, 99U, 1U);
	table_version(fixture->table_bytes, 6U, 1U, 0x08U, 4U, 0U);
	table_version(fixture->table_bytes, 7U, 5U, 0x08U, 2U, 0U);
	table_version(fixture->table_bytes, 8U, 5U, 0x17U, 3U, 2U);
	table_version(fixture->table_bytes, 9U, 3U, 0x05U, 2U, 0U);
	table_version(fixture->table_bytes, 10U, 3U, 0xfbU, 99U, 3U);
	table_version(fixture->table_bytes, 11U, 1U, 0xd0U, 1U, 0U);
	table_version(fixture->table_bytes, 12U, 1U, 0xa9U, 1U, 0U);
	table_version(fixture->table_bytes, 13U, 1U, 0x0eU, 1U, 0U);
	table_version(fixture->table_bytes, 14U, INTEL_AX211_KEY_GROUP,
	    INTEL_AX211_KEY_OPCODE, INTEL_AX211_KEY_COMMAND_VERSION,
	    INTEL_AX211_KEY_RESPONSE_VERSION);
	table_version(fixture->table_bytes, 15U, INTEL_AX211_TX_GROUP,
	    INTEL_AX211_TX_OPCODE, INTEL_AX211_TX_COMMAND_VERSION,
	    INTEL_AX211_TX_NOTIFICATION_VERSION);
	table_version(fixture->table_bytes, 16U, INTEL_AX211_RX_MPDU_GROUP,
	    INTEL_AX211_RX_MPDU_OPCODE, 99U,
	    INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION);
	table_version(fixture->table_bytes, 17U,
	    INTEL_AX211_ASSOC_GROUP_MAC_CONFIG,
	    INTEL_AX211_ASSOC_MAC_CONFIG_OPCODE,
	    INTEL_AX211_ASSOC_MAC_CONFIG_VERSION, 0U);
	table_version(fixture->table_bytes, 18U,
	    INTEL_AX211_ASSOC_GROUP_MAC_CONFIG,
	    INTEL_AX211_ASSOC_LINK_CONFIG_OPCODE,
	    INTEL_AX211_ASSOC_LINK_CONFIG_VERSION, 0U);
	table_version(fixture->table_bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U, 0U, 0U, 0U, 0U);
	assert(intel_ax211_protocol_command_table_parse(fixture->table_bytes,
	    sizeof(fixture->table_bytes), &fixture->table) ==
	    INTEL_AX211_PROTOCOL_OK);
}

static int
transport_take(struct ax211_radio_fixture *fixture, unsigned *counter)
{
	if (fixture->transport.remaining == 0U)
		return EIO;
	fixture->transport.remaining--;
	(*counter)++;
	return 0;
}

static uint64_t
fixture_clock(void *context)
{
	return ((struct ax211_radio_fixture *)context)->now;
}

static struct intel_ax211_assoc_reply
assoc_reply(const struct intel_ax211_assoc_command *command)
{
	struct intel_ax211_assoc_reply reply;

	memset(&reply, 0, sizeof(reply));
	reply.step = command->step;
	reply.response_version = command->response_version;
	reply.sequence = command->sequence;
	reply.common_generation = command->common_generation;
	reply.hardware_epoch = command->hardware_epoch;
	if (command->response_kind == INTEL_AX211_ASSOC_RESPONSE_STATUS_ZERO)
		reply.payload_length = 4U;
	else if (command->response_kind == INTEL_AX211_ASSOC_RESPONSE_QUEUE) {
		put_le16(reply.payload, 0x101U);
		put_le16(reply.payload + 4U, 0x345U);
		reply.payload_length = 8U;
	} else if (command->response_kind ==
	    INTEL_AX211_ASSOC_RESPONSE_IGNORED)
		reply.payload_length = 0U;
	return reply;
}

static int
assoc_exchange(void *argument, const struct intel_ax211_assoc_command *command,
	struct intel_ax211_assoc_reply *reply)
{
	struct ax211_radio_fixture *fixture = argument;

	assert(command != NULL && reply != NULL);
	assert(command->payload_length <= INTEL_AX211_ASSOC_PAYLOAD_MAX);
	if (transport_take(fixture,
	    &fixture->transport.association_commands) != 0)
		return INTEL_AX211_ASSOC_TIMEOUT;
	fixture->assoc_exchanges++;
	*reply = assoc_reply(command);
	fixture->now++;
	return INTEL_AX211_ASSOC_OK;
}

static uint64_t
assoc_clock(void *argument)
{
	return ((struct ax211_radio_fixture *)argument)->now;
}

static int
radio_scan_channel_start(void *context, uint64_t generation,
	uint32_t step_index, uint32_t channel, uint64_t deadline)
{
	struct ax211_radio_fixture *fixture = context;
	struct intel_ax211_scan_profile profile;
	uint8_t request[INTEL_AX211_SCAN_REQUEST_SIZE];
	int result;

	assert(step_index == 0U && channel == FIXTURE_CHANNEL &&
	    deadline > fixture->now);
	if (transport_take(fixture, &fixture->transport.scan_commands) != 0)
		return EIO;
	memset(&profile, 0, sizeof(profile));
	memcpy(profile.station_address, fixture_station, sizeof(fixture_station));
	profile.channel_width_mhz = INTEL_AX211_SCAN_CHANNEL_WIDTH_MHZ;
	profile.channel[0] = FIXTURE_CHANNEL;
	profile.channel_count = 1U;
	assert(intel_ax211_scan_request_encode(&profile, request) ==
	    INTEL_AX211_SCAN_OK);
	result = intel_ax211_scan_begin(&fixture->scan, &fixture->table, &profile,
	    (uint32_t)generation, fixture->now);
	assert(result == INTEL_AX211_SCAN_OK);
	assert(intel_ax211_scan_request_ack(&fixture->scan,
	    (uint32_t)generation, fixture->now + 1U) == INTEL_AX211_SCAN_OK);
	fixture->scan_generation = generation;
	return 0;
}

static int
radio_scan_stop(void *context, uint64_t generation)
{
	struct ax211_radio_fixture *fixture = context;
	struct intel_ax211_protocol_message message;
	struct intel_ax211_scan_event event;
	uint8_t payload[16];

	assert(generation == fixture->scan_generation);
	memset(payload, 0, sizeof(payload));
	payload[6] = 1U;
	memset(&message, 0, sizeof(message));
	message.group = INTEL_AX211_SCAN_GROUP_LEGACY;
	message.opcode = INTEL_AX211_SCAN_COMPLETE_OPCODE;
	message.version = INTEL_AX211_SCAN_NOTIFICATION_VERSION;
	message.generation = (uint32_t)generation;
	message.payload = payload;
	message.payload_length = sizeof(payload);
	assert(intel_ax211_scan_event_accept(&fixture->scan, &message,
	    fixture->now, &event) == INTEL_AX211_SCAN_COMPLETE);
	return 0;
}

static int
radio_management_transmit(void *context, uint64_t generation,
	const uint8_t *frame, size_t length, uint64_t deadline)
{
	struct ax211_radio_fixture *fixture = context;
	struct intel_ax211_tx_request request;
	struct intel_ax211_tx_prepared prepared;

	assert(generation == fixture->scan_generation && deadline > fixture->now);
	if (transport_take(fixture, &fixture->transport.tx_commands) != 0)
		return EIO;
	memset(&request, 0, sizeof(request));
	request.connection_generation = generation;
	request.cookie = 1U;
	request.frame = frame;
	request.length = length;
	request.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
	request.band_5ghz = 1U;
	assert(intel_ax211_tx_prepare(&request, &prepared) == INTEL_AX211_TX_OK);
	assert(get_le32(prepared.command + 16U) == 0x4100U);
	return 0;
}

static int
radio_connect_start(void *context, uint64_t generation,
	const struct wlan_bss_record *bss, uint64_t deadline)
{
	struct ax211_radio_fixture *fixture = context;
	struct intel_ax211_assoc_profile profile;
	int result;

	assert(bss != NULL && deadline > fixture->now &&
	    bss->channel == FIXTURE_CHANNEL);
	memset(&profile, 0, sizeof(profile));
	memcpy(profile.station_address, fixture_station, sizeof(fixture_station));
	memcpy(profile.bssid, bss->bssid, sizeof(profile.bssid));
	profile.channel = bss->channel;
	profile.channel_width_mhz = INTEL_AX211_ASSOC_CHANNEL_WIDTH_MHZ;
	profile.rx_chain_mask = 1U;
	profile.cck_ack_rates = 0U;
	profile.ofdm_ack_rates = 0x15U;
	profile.short_preamble = 0U;
	profile.short_slot = 1U;
	profile.qos = 1U;
	profile.beacon_interval_tu = 100U;
	profile.queue_byte_count_address = UINT64_C(0x100000);
	profile.queue_descriptor_address = UINT64_C(0x200000);
	profile.edca[0].ecw_min = 4U;
	profile.edca[0].ecw_max = 10U;
	profile.edca[0].aifsn = 3U;
	fixture->connection_generation = generation;
	memcpy(fixture->selected_bssid, bss->bssid, 6U);
	assert(intel_ax211_key_state_init(&fixture->keys,
	    FIXTURE_HARDWARE_EPOCH, generation) == INTEL_AX211_KEY_OK);
	result = intel_ax211_assoc_begin(&fixture->association, &fixture->table,
	    &profile, generation, FIXTURE_HARDWARE_EPOCH, fixture->now);
	assert(result == INTEL_AX211_ASSOC_OK);
	result = intel_ax211_assoc_drive(&fixture->association, &assoc_ops,
	    fixture);
	assert(result == INTEL_AX211_ASSOC_AUTH_READY);
	return 0;
}

static int
radio_disconnect(void *context, uint64_t generation)
{
	struct ax211_radio_fixture *fixture = context;
	int result;

	if (generation != fixture->connection_generation)
		return ESTALE;
	if (fixture->association.phase == INTEL_AX211_ASSOC_PHASE_IDLE)
		return 0;
	result = intel_ax211_assoc_cancel(&fixture->association, generation,
	    FIXTURE_HARDWARE_EPOCH, fixture->now);
	if (result == INTEL_AX211_ASSOC_OK ||
	    result == INTEL_AX211_ASSOC_PENDING)
		result = intel_ax211_assoc_drive(&fixture->association, &assoc_ops,
		    fixture);
	return result == INTEL_AX211_ASSOC_ROLLED_BACK ? 0 : EIO;
}

static int
radio_association_set(void *context, uint64_t generation,
	const uint8_t bssid[6], uint16_t aid, uint64_t deadline)
{
	struct ax211_radio_fixture *fixture = context;
	struct intel_ax211_assoc_update update;
	int result;

	assert(generation == fixture->connection_generation && aid != 0U &&
	    deadline > fixture->now &&
	    memcmp(bssid, fixture->selected_bssid, 6U) == 0);
	memset(&update, 0, sizeof(update));
	update.association_id = aid;
	update.dtim_period = 2U;
	result = intel_ax211_assoc_begin_update(&fixture->association, &update,
	    generation, FIXTURE_HARDWARE_EPOCH, fixture->now);
	assert(result == INTEL_AX211_ASSOC_OK);
	result = intel_ax211_assoc_drive(&fixture->association, &assoc_ops,
	    fixture);
	assert(result == INTEL_AX211_ASSOC_COMPLETE);
	return 0;
}

static int
radio_association_clear(void *context, uint64_t generation,
	uint64_t deadline)
{
	(void)deadline;
	return radio_disconnect(context, generation);
}

static int
radio_frame_transmit(void *context,
	const struct wlan_radio_tx_request *request)
{
	struct ax211_radio_fixture *fixture = context;
	struct intel_ax211_tx_request private_request;
	struct intel_ax211_tx_prepared prepared;

	assert(request != NULL && !fixture->pending);
	if (transport_take(fixture, &fixture->transport.tx_commands) != 0)
		return EIO;
	memset(&private_request, 0, sizeof(private_request));
	private_request.connection_generation = request->generation;
	private_request.cookie = request->cookie;
	private_request.key_generation = request->key_generation;
	private_request.packet_number = request->packet_number;
	private_request.frame = request->frame;
	private_request.length = request->length;
	private_request.encrypted = request->encrypted;
	private_request.key_index = request->key_index;
	private_request.band_5ghz = 1U;
	if (request->frame_class == WLAN_RADIO_FRAME_MANAGEMENT)
		private_request.frame_class = INTEL_AX211_TX_FRAME_MANAGEMENT;
	else if (request->frame_class == WLAN_RADIO_FRAME_EAPOL)
		private_request.frame_class = INTEL_AX211_TX_FRAME_EAPOL;
	else
		private_request.frame_class = INTEL_AX211_TX_FRAME_DATA;
	if (request->encrypted)
		assert(intel_ax211_key_state_tx_validate(&fixture->keys,
		    request->generation, request->key_generation,
		    request->key_index, request->packet_number,
		    FIXTURE_HARDWARE_EPOCH) == INTEL_AX211_KEY_OK);
	assert(intel_ax211_tx_prepare(&private_request, &prepared) ==
	    INTEL_AX211_TX_OK);
	assert(get_le32(prepared.command + 16U) == 0x4100U);
	assert(request->length <= sizeof(fixture->pending_frame));
	fixture->pending_generation = request->generation;
	fixture->pending_cookie = request->cookie;
	fixture->pending_length = request->length;
	memcpy(fixture->pending_frame, request->frame, request->length);
	fixture->pending = 1U;
	if (request->frame_class == WLAN_RADIO_FRAME_DATA)
		fixture->data_tx++;
	return 0;
}

static int
radio_key_install(void *context,
	const struct wlan_radio_key_request *request)
{
	struct ax211_radio_fixture *fixture = context;
	struct intel_ax211_key_request private_request;
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];

	assert(request != NULL && request->generation ==
	    fixture->connection_generation);
	if (transport_take(fixture, &fixture->transport.key_commands) != 0)
		return EIO;
	memset(&private_request, 0, sizeof(private_request));
	private_request.connection_generation = request->generation;
	private_request.key_generation = request->key_generation;
	private_request.receive_packet_number = request->receive_packet_number;
	private_request.key_index = request->key_index;
	private_request.kind = request->kind == WLAN_RADIO_KEY_PAIRWISE ?
	    INTEL_AX211_KEY_PAIRWISE : INTEL_AX211_KEY_GROUP_KEY;
	memcpy(private_request.key, request->key, sizeof(private_request.key));
	assert(intel_ax211_key_add_encode(&private_request, command) ==
	    INTEL_AX211_KEY_OK);
	assert(intel_ax211_key_state_installed(&fixture->keys, &private_request,
	    FIXTURE_HARDWARE_EPOCH) == INTEL_AX211_KEY_OK);
	if (private_request.kind == INTEL_AX211_KEY_PAIRWISE) {
		fixture->pairwise_generation = request->key_generation;
		fixture->pairwise_installs++;
	} else {
		fixture->group_generation = request->key_generation;
		fixture->group_installs++;
	}
	if (fixture->pairwise_generation != 0U &&
	    fixture->group_generation != 0U)
		assert(intel_ax211_key_state_activate(&fixture->keys,
		    request->generation, fixture->pairwise_generation,
		    fixture->group_generation, FIXTURE_HARDWARE_EPOCH) ==
		    INTEL_AX211_KEY_OK);
	intel_ax211_key_command_scrub(command);
	memset(&private_request, 0, sizeof(private_request));
	return 0;
}

static int
radio_key_delete(void *context, uint64_t generation,
	enum wlan_radio_key_kind kind, uint8_t key_index,
	uint64_t key_generation, uint64_t deadline)
{
	struct ax211_radio_fixture *fixture = context;
	enum intel_ax211_key_kind private_kind;
	uint8_t command[INTEL_AX211_KEY_COMMAND_SIZE];
	int result;

	(void)deadline;
	if (transport_take(fixture, &fixture->transport.key_commands) != 0)
		return EIO;
	private_kind = kind == WLAN_RADIO_KEY_PAIRWISE ?
	    INTEL_AX211_KEY_PAIRWISE : INTEL_AX211_KEY_GROUP_KEY;
	assert(intel_ax211_key_remove_encode(generation, key_generation,
	    private_kind, key_index, command) == INTEL_AX211_KEY_OK);
	result = intel_ax211_key_state_removed(&fixture->keys, generation,
	    private_kind, key_index, key_generation, FIXTURE_HARDWARE_EPOCH);
	assert(result == INTEL_AX211_KEY_OK || result == INTEL_AX211_KEY_MISSING);
	intel_ax211_key_command_scrub(command);
	return 0;
}

static int
radio_keys_activate(void *context, uint64_t generation,
	uint64_t pairwise_generation, uint64_t group_generation,
	uint64_t deadline)
{
	struct ax211_radio_fixture *fixture = context;
	int result;

	(void)deadline;
	result = intel_ax211_key_state_activate(&fixture->keys, generation,
	    pairwise_generation, group_generation, FIXTURE_HARDWARE_EPOCH);
	return result == INTEL_AX211_KEY_OK ||
	    result == INTEL_AX211_KEY_DUPLICATE ? 0 : EIO;
}

static int
radio_quiesce(void *context)
{
	struct ax211_radio_fixture *fixture = context;

	assert(!fixture->pending);
	return 0;
}

static size_t
build_beacon(uint8_t *frame)
{
	static const uint8_t rates[] = {
		0x8cU, 0x12U, 0x98U, 0x24U, 0xb0U, 0x48U, 0x60U, 0x6cU
	};
	size_t offset = 36U;

	memset(frame, 0, 256U);
	put_le16(frame, 0x0080U);
	memset(frame + 4U, 0xff, 6U);
	memcpy(frame + 10U, fixture_bssid, 6U);
	memcpy(frame + 16U, fixture_bssid, 6U);
	put_le16(frame + 32U, 100U);
	put_le16(frame + 34U, 0x0011U);
	frame[offset++] = 0U;
	frame[offset++] = sizeof(fixture_ssid);
	memcpy(frame + offset, fixture_ssid, sizeof(fixture_ssid));
	offset += sizeof(fixture_ssid);
	frame[offset++] = 1U;
	frame[offset++] = sizeof(rates);
	memcpy(frame + offset, rates, sizeof(rates));
	offset += sizeof(rates);
	/* No DS parameter IE: the RX channel hint must retain 5 GHz. */
	memcpy(frame + offset, fixture_rsn, sizeof(fixture_rsn));
	offset += sizeof(fixture_rsn);
	return offset;
}

static size_t
build_authentication_response(uint8_t *frame)
{
	memset(frame, 0, 64U);
	put_le16(frame, 0x00b0U);
	memcpy(frame + 4U, fixture_station, 6U);
	memcpy(frame + 10U, fixture_bssid, 6U);
	memcpy(frame + 16U, fixture_bssid, 6U);
	put_le16(frame + 26U, 2U);
	return 30U;
}

static size_t
build_association_response(uint8_t *frame)
{
	static const uint8_t rates[] = {
		0x8cU, 0x12U, 0x98U, 0x24U, 0xb0U, 0x48U, 0x60U, 0x6cU
	};
	size_t offset = 30U;

	memset(frame, 0, 64U);
	put_le16(frame, 0x0010U);
	memcpy(frame + 4U, fixture_station, 6U);
	memcpy(frame + 10U, fixture_bssid, 6U);
	memcpy(frame + 16U, fixture_bssid, 6U);
	put_le16(frame + 24U, 0x0431U);
	put_le16(frame + 28U, 0xc02aU);
	frame[offset++] = 1U;
	frame[offset++] = sizeof(rates);
	memcpy(frame + offset, rates, sizeof(rates));
	return offset + sizeof(rates);
}

static void
assert_association_rates(const uint8_t *frame, size_t length)
{
	static const uint8_t expected[] = {
		0x8cU, 0x12U, 0x98U, 0x24U, 0xb0U, 0x48U, 0x60U, 0x6cU
	};
	size_t offset;
	int found = 0;

	assert(frame != NULL && length >= 28U);
	assert((frame[0] & 0xfcU) == 0U);
	assert(frame[24U] == 0x11U && frame[25U] == 0x04U);
	for (offset = 28U; offset + 2U <= length;) {
		uint8_t identifier = frame[offset++];
		uint8_t ie_length = frame[offset++];

		assert(offset + ie_length <= length);
		if (identifier == 1U) {
			assert(ie_length == sizeof(expected));
			assert(memcmp(frame + offset, expected,
			    sizeof(expected)) == 0);
			found = 1;
		}
		offset += ie_length;
	}
	assert(found);
}

static size_t
build_from_ds_data(uint8_t *frame, const uint8_t source[6],
	uint16_t ether_type, const uint8_t *payload, size_t payload_length,
	int protected_frame, uint8_t key_index, uint64_t packet_number)
{
	size_t offset = 24U;

	memset(frame, 0, WLAN_MANAGEMENT_FRAME_MAX);
	put_le16(frame, (uint16_t)(0x0208U |
	    (protected_frame ? 0x4000U : 0U)));
	memcpy(frame + 4U, fixture_station, 6U);
	memcpy(frame + 10U, fixture_bssid, 6U);
	memcpy(frame + 16U, source, 6U);
	if (protected_frame) {
		frame[offset++] = (uint8_t)packet_number;
		frame[offset++] = (uint8_t)(packet_number >> 8);
		frame[offset++] = 0U;
		frame[offset++] = (uint8_t)(0x20U | (key_index << 6));
		frame[offset++] = (uint8_t)(packet_number >> 16);
		frame[offset++] = (uint8_t)(packet_number >> 24);
		frame[offset++] = (uint8_t)(packet_number >> 32);
		frame[offset++] = (uint8_t)(packet_number >> 40);
	}
	memcpy(frame + offset, (uint8_t[]){
	    0xaaU, 0xaaU, 0x03U, 0U, 0U, 0U }, 6U);
	offset += 6U;
	frame[offset++] = (uint8_t)(ether_type >> 8);
	frame[offset++] = (uint8_t)ether_type;
	memcpy(frame + offset, payload, payload_length);
	offset += payload_length;
	if (protected_frame) {
		memset(frame + offset, 0, WLAN_L2_CCMP_MIC_SIZE);
		offset += WLAN_L2_CCMP_MIC_SIZE;
	}
	return offset;
}

static int
deliver_rx(struct ax211_radio_fixture *fixture, uint64_t common_generation,
	const uint8_t *frame, size_t frame_length, int protected_frame)
{
	struct intel_ax211_protocol_message message;
	struct intel_ax211_rx_mpdu decoded;
	struct wlan_radio_rx_frame report;
	struct intel_ax211_bss_entry bss;
	uint8_t payload[INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE +
	    WLAN_MANAGEMENT_FRAME_MAX];
	uint8_t output[WLAN_MANAGEMENT_FRAME_MAX];
	uint32_t status = RX_STATUS_CRC_OK | RX_STATUS_OVERRUN_OK;
	uint64_t key_generation = 0U;
	int result;

	assert(transport_take(fixture, &fixture->transport.rx_events) == 0);
	memset(payload, 0, sizeof(payload));
	put_le16(payload, (uint16_t)frame_length);
	if (protected_frame) {
		payload[2] = 0x40U;
		status |= RX_STATUS_MIC_OK | RX_STATUS_CCMP | RX_STATUS_DECRYPTED;
	}
	put_le32(payload + 12U, status);
	payload[40U] = 35U;
	payload[42U] = FIXTURE_CHANNEL;
	memcpy(payload + INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE, frame,
	    frame_length);
	memset(&message, 0, sizeof(message));
	message.opcode = INTEL_AX211_RX_MPDU_OPCODE;
	message.group = INTEL_AX211_RX_MPDU_GROUP;
	message.version = INTEL_AX211_RX_MPDU_NOTIFICATION_VERSION;
	message.generation = FIXTURE_HARDWARE_EPOCH;
	message.payload = payload;
	message.payload_length = INTEL_AX211_RX_MPDU_DESCRIPTOR_SIZE +
	    frame_length;
	assert(intel_ax211_rx_mpdu_decode(&message, FIXTURE_HARDWARE_EPOCH,
	    output, sizeof(output), &decoded) == INTEL_AX211_RX_OK);
	if ((frame[0] & 0xfcU) == 0x80U) {
		assert(intel_ax211_bss_decode(&decoded, common_generation,
		    FIXTURE_HARDWARE_EPOCH, &bss) == INTEL_AX211_BSS_OK);
		assert(memcmp(bss.bssid, fixture_bssid, 6U) == 0);
		fixture->scan_publications++;
	}
	if (protected_frame) {
		assert(intel_ax211_key_state_rx_generation(&fixture->keys,
		    common_generation, INTEL_AX211_KEY_PAIRWISE, 0U,
		    FIXTURE_HARDWARE_EPOCH, &key_generation) ==
		    INTEL_AX211_KEY_OK);
	}
	memset(&report, 0, sizeof(report));
	report.generation = common_generation;
	report.key_generation = key_generation;
	report.packet_number = decoded.packet_number;
	report.frame = decoded.frame;
	report.length = decoded.length;
	report.rssi_dbm = decoded.rssi_dbm;
	report.channel = decoded.channel;
	report.cipher = decoded.cipher == INTEL_AX211_RX_CIPHER_CCMP ?
	    WLAN_RADIO_CIPHER_CCMP : WLAN_RADIO_CIPHER_NONE;
	report.decrypted = decoded.decrypted;
	report.key_index = decoded.key_index;
	result = wlan_station_report_frame(fixture->station, &report);
	if (protected_frame && result == 0)
		fixture->data_rx++;
	return result;
}

static void
complete_pending(struct ax211_radio_fixture *fixture)
{
	uint64_t generation;
	uint64_t cookie;

	assert(fixture->pending);
	generation = fixture->pending_generation;
	cookie = fixture->pending_cookie;
	fixture->pending = 0U;
	assert(wlan_station_report_tx_complete(fixture->station, generation,
	    cookie, 1, 0) == 0);
}

static void
fill_bytes(uint8_t *bytes, size_t length, uint8_t seed)
{
	size_t index;

	for (index = 0U; index < length; index++)
		bytes[index] = (uint8_t)(seed + index * 3U);
}

static void
ordered_copy(uint8_t *output, const uint8_t *left, const uint8_t *right,
	size_t length)
{
	if (memcmp(left, right, length) < 0) {
		memcpy(output, left, length);
		memcpy(output + length, right, length);
	} else {
		memcpy(output, right, length);
		memcpy(output + length, left, length);
	}
}

static void
derive_ptk(const uint8_t snonce[WLAN_WPA2_NONCE_LENGTH],
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH],
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH])
{
	static const uint8_t label[] = "Pairwise key expansion";
	uint8_t pmk[WLAN_WPA2_PMK_LENGTH];
	uint8_t data[2U * WLAN_WPA2_MAC_LENGTH +
	    2U * WLAN_WPA2_NONCE_LENGTH];

	assert(wlan_pbkdf2_hmac_sha1(fixture_passphrase,
	    sizeof(fixture_passphrase) - 1U, fixture_ssid,
	    sizeof(fixture_ssid), 4096U, pmk, sizeof(pmk)) == 0);
	ordered_copy(data, fixture_bssid, fixture_station, 6U);
	ordered_copy(data + 12U, anonce, snonce, WLAN_WPA2_NONCE_LENGTH);
	assert(wlan_crypto_prf_sha1(pmk, sizeof(pmk), label,
	    sizeof(label) - 1U, data, sizeof(data), ptk,
	    WLAN_WPA2_PTK_LENGTH) == 0);
	wlan_crypto_erase(pmk, sizeof(pmk));
	wlan_crypto_erase(data, sizeof(data));
}

static void
eapol_sign(uint8_t *frame, size_t length,
	const uint8_t kck[WLAN_WPA2_KCK_LENGTH])
{
	uint8_t digest[WLAN_SHA1_DIGEST_SIZE];

	memset(frame + 81U, 0, WLAN_WPA2_KEY_MIC_LENGTH);
	assert(wlan_hmac_sha1(kck, WLAN_WPA2_KCK_LENGTH, frame, length,
	    digest) == 0);
	memcpy(frame + 81U, digest, WLAN_WPA2_KEY_MIC_LENGTH);
	wlan_crypto_erase(digest, sizeof(digest));
}

static void
xor_t(uint8_t accumulator[8], uint64_t value)
{
	unsigned index;

	for (index = 0U; index < 8U; index++) {
		accumulator[7U - index] ^= (uint8_t)value;
		value >>= 8;
	}
}

static size_t
rfc3394_wrap(const uint8_t kek[WLAN_WPA2_KEK_LENGTH],
	const uint8_t *plaintext, size_t plaintext_length, uint8_t *wrapped,
	size_t capacity)
{
	uint8_t accumulator[8] = {
		0xa6U, 0xa6U, 0xa6U, 0xa6U,
		0xa6U, 0xa6U, 0xa6U, 0xa6U
	};
	uint8_t block[16];
	uint8_t encrypted[16];
	size_t n = plaintext_length / 8U;
	size_t index;
	unsigned round;

	assert(plaintext_length >= 16U && plaintext_length % 8U == 0U &&
	    capacity >= plaintext_length + 8U);
	memcpy(wrapped + 8U, plaintext, plaintext_length);
	for (round = 0U; round < 6U; round++) {
		for (index = 1U; index <= n; index++) {
			memcpy(block, accumulator, 8U);
			memcpy(block + 8U, wrapped + index * 8U, 8U);
			assert(wlan_aes128_encrypt_block(kek, block, encrypted) == 0);
			memcpy(accumulator, encrypted, 8U);
			xor_t(accumulator,
			    (uint64_t)round * (uint64_t)n + (uint64_t)index);
			memcpy(wrapped + index * 8U, encrypted + 8U, 8U);
		}
	}
	memcpy(wrapped, accumulator, 8U);
	wlan_crypto_erase(block, sizeof(block));
	wlan_crypto_erase(encrypted, sizeof(encrypted));
	return plaintext_length + 8U;
}

static size_t
build_message_1(uint8_t *frame, uint64_t replay,
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH])
{
	memset(frame, 0, 99U);
	frame[0] = 2U;
	frame[1] = 3U;
	put_be16(frame + 2U, 95U);
	frame[4] = 2U;
	put_be16(frame + 5U, 0x008aU);
	put_be16(frame + 7U, WLAN_WPA2_TK_LENGTH);
	put_be64(frame + 9U, replay);
	memcpy(frame + 17U, anonce, WLAN_WPA2_NONCE_LENGTH);
	return 99U;
}

static size_t
build_message_3(uint8_t *frame, uint64_t replay,
	const uint8_t anonce[WLAN_WPA2_NONCE_LENGTH], const uint8_t *ptk,
	const uint8_t gtk[WLAN_WPA2_GTK_LENGTH], uint8_t gtk_index,
	uint64_t receive_pn)
{
	uint8_t plaintext[64];
	uint8_t wrapped[72];
	size_t wrapped_length;
	size_t length;
	unsigned index;

	memcpy(plaintext, fixture_rsn, sizeof(fixture_rsn));
	plaintext[22] = 221U;
	plaintext[23] = 22U;
	plaintext[24] = 0U;
	plaintext[25] = 15U;
	plaintext[26] = 172U;
	plaintext[27] = 1U;
	plaintext[28] = gtk_index;
	plaintext[29] = 0U;
	memcpy(plaintext + 30U, gtk, WLAN_WPA2_GTK_LENGTH);
	plaintext[46] = 221U;
	plaintext[47] = 0U;
	wrapped_length = rfc3394_wrap(ptk + WLAN_WPA2_KCK_LENGTH,
	    plaintext, 48U, wrapped, sizeof(wrapped));
	length = 99U + wrapped_length;
	memset(frame, 0, length);
	frame[0] = 2U;
	frame[1] = 3U;
	put_be16(frame + 2U, (uint16_t)(95U + wrapped_length));
	frame[4] = 2U;
	put_be16(frame + 5U, 0x13caU);
	put_be16(frame + 7U, WLAN_WPA2_TK_LENGTH);
	put_be64(frame + 9U, replay);
	memcpy(frame + 17U, anonce, WLAN_WPA2_NONCE_LENGTH);
	for (index = 0U; index < 6U; index++) {
		frame[65U + index] = (uint8_t)receive_pn;
		receive_pn >>= 8;
	}
	put_be16(frame + 97U, (uint16_t)wrapped_length);
	memcpy(frame + 99U, wrapped, wrapped_length);
	eapol_sign(frame, length, ptk);
	wlan_crypto_erase(plaintext, sizeof(plaintext));
	wlan_crypto_erase(wrapped, sizeof(wrapped));
	return length;
}

static void
run_integration(void)
{
	struct ax211_radio_fixture fixture;
	struct net_device device;
	struct wlan_scan_profile scan_profile;
	struct wlan_scan_request scan;
	struct wlan_scan_status_request scan_status;
	struct wlan_connect_request connect;
	struct wlan_status_request status;
	struct wlan_disconnect_request disconnect;
	struct wlan_wpa2_eapol_key m2;
	struct packet_buf *packet;
	uint8_t frame[WLAN_MANAGEMENT_FRAME_MAX];
	uint8_t eapol[WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t anonce[WLAN_WPA2_NONCE_LENGTH];
	uint8_t ptk[WLAN_WPA2_PTK_LENGTH];
	uint8_t gtk[WLAN_WPA2_GTK_LENGTH];
	uint8_t ethernet[64];
	size_t length;
	uint64_t scan_generation;
	uint64_t connection_generation;

	memset(&fixture, 0, sizeof(fixture));
	memset(&device, 0, sizeof(device));
	memset(&scan_profile, 0, sizeof(scan_profile));
	fixture.transport.remaining = FIXTURE_COMMAND_BUDGET;
	fixture.now = 10U;
	make_api89_table(&fixture);
	memcpy(device.name, "wlan0", 6U);
	memcpy(device.hwaddr, fixture_station, sizeof(fixture_station));
	device.hwaddr_len = 6U;
	device.flags = NET_DEVICE_UP;
	scan_profile.channel_count = 1U;
	scan_profile.channels[0].channel = FIXTURE_CHANNEL;
	scan_profile.channels[0].center_frequency_mhz = FIXTURE_FREQUENCY_MHZ;
	scan_profile.channels[0].flags = WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED;
	wlan_core_init();
	assert(wlan_station_test_attach(&device, &radio_ops, &fixture,
	    &scan_profile, fixture_clock, &fixture, &fixture.station) == 0);
	assert(wlan_station_open(fixture.station) == 0);

	request_header(&scan, sizeof(scan));
	scan.action = WLAN_SCAN_START;
	assert(wlan_station_ioctl(&device, SIOCSWLANSCAN, &scan) == 0);
	scan_generation = scan.generation;
	wlan_timer_run(fixture.now);
	assert(fixture.scan_generation == scan_generation);
	assert(wlan_station_report_scan_channel_ready(fixture.station,
	    scan_generation, 0U) == 0);
	wlan_timer_run(fixture.now);
	length = build_beacon(frame);
	assert(deliver_rx(&fixture, scan_generation, frame, length, 0) == 0);
	fixture.now += WLAN_SCAN_DWELL_TICKS;
	wlan_timer_run(fixture.now);
	request_header(&scan_status, sizeof(scan_status));
	assert(wlan_station_ioctl(&device, SIOCGWLANSCAN, &scan_status) == 0);
	assert(scan_status.state == WLAN_SCAN_COMPLETE &&
	    scan_status.result_count == 1U && fixture.scan_publications == 1U);

	request_header(&connect, sizeof(connect));
	memcpy(connect.ssid, fixture_ssid, sizeof(fixture_ssid));
	connect.ssid_length = sizeof(fixture_ssid);
	memcpy(connect.passphrase, fixture_passphrase,
	    sizeof(fixture_passphrase) - 1U);
	connect.passphrase_length = sizeof(fixture_passphrase) - 1U;
	assert(wlan_station_ioctl(&device, SIOCSWLANCONNECT, &connect) == 0);
	connection_generation = connect.generation;
	assert(fixture.pending);
	complete_pending(&fixture);
	length = build_authentication_response(frame);
	assert(deliver_rx(&fixture, connection_generation, frame, length, 0) == 0);
	assert(fixture.pending);
	assert_association_rates(fixture.pending_frame, fixture.pending_length);
	complete_pending(&fixture);
	length = build_association_response(frame);
	assert(deliver_rx(&fixture, connection_generation, frame, length, 0) == 0);

	fill_bytes(anonce, sizeof(anonce), 0x21U);
	length = build_message_1(eapol, 17U, anonce);
	length = build_from_ds_data(frame, fixture_bssid, 0x888eU, eapol,
	    length, 0, 0U, 0U);
	assert(deliver_rx(&fixture, connection_generation, frame, length, 0) == 0);
	assert(fixture.pending);
	assert(fixture.pending_length > 32U);
	assert(wlan_wpa2_eapol_key_parse(fixture.pending_frame + 32U,
	    fixture.pending_length - 32U, &m2) == 0);
	assert(m2.message == WLAN_WPA2_EAPOL_MESSAGE_2);
	derive_ptk(m2.nonce, anonce, ptk);
	complete_pending(&fixture);
	fill_bytes(gtk, sizeof(gtk), 0x91U);
	length = build_message_3(eapol, 18U, anonce, ptk, gtk, 1U, 7U);
	length = build_from_ds_data(frame, fixture_bssid, 0x888eU, eapol,
	    length, 0, 0U, 0U);
	assert(deliver_rx(&fixture, connection_generation, frame, length, 0) == 0);
	assert(fixture.pending && fixture.pairwise_installs == 1U &&
	    fixture.group_installs == 1U);
	complete_pending(&fixture);
	request_header(&status, sizeof(status));
	assert(wlan_station_ioctl(&device, SIOCGWLANSTATUS, &status) == 0);
	assert(status.state == WLAN_STATE_CONNECTED && status.controlled_port &&
	    status.key_installed && device.carrier);

	memset(ethernet, 0, sizeof(ethernet));
	memcpy(ethernet, fixture_peer, 6U);
	memcpy(ethernet + 6U, fixture_station, 6U);
	ethernet[12] = 0x08U;
	ethernet[13] = 0x00U;
	memcpy(ethernet + 14U, "synthetic-tx", 12U);
	packet = packet_buf_alloc(0U);
	assert(packet != NULL && packet_buf_append(packet, 26U) != NULL);
	memcpy(packet->data, ethernet, 26U);
	assert(wlan_station_transmit(fixture.station, packet) == 0);
	assert(fixture.pending && fixture.data_tx == 1U);
	complete_pending(&fixture);

	memset(ethernet, 0, sizeof(ethernet));
	memcpy(ethernet, fixture_station, 6U);
	memcpy(ethernet + 6U, fixture_peer, 6U);
	ethernet[12] = 0x08U;
	ethernet[13] = 0x00U;
	memcpy(ethernet + 14U, "synthetic-rx", 12U);
	length = build_from_ds_data(frame, fixture_peer, 0x0800U,
	    ethernet + 14U, 12U, 1, 0U, 1U);
	assert(deliver_rx(&fixture, connection_generation, frame, length, 1) == 0);
	assert(receive_count == 1U && fixture.data_rx == 1U &&
	    received_length == 26U &&
	    memcmp(received_ethernet, ethernet, 26U) == 0);

	request_header(&disconnect, sizeof(disconnect));
	assert(wlan_station_ioctl(&device, SIOCSWLANDISCONNECT, &disconnect) == 0);
	assert(wlan_station_close(fixture.station) == 0);
	assert(wlan_station_detach(fixture.station) == 0);
	assert(reference_balance == 0 && fixture.transport.remaining > 0U);
	assert(fixture.transport.scan_commands != 0U &&
	    fixture.transport.association_commands != 0U &&
	    fixture.transport.key_commands != 0U &&
	    fixture.transport.tx_commands != 0U &&
	    fixture.transport.rx_events != 0U && fixture.assoc_exchanges >= 9U);
	wlan_crypto_erase(ptk, sizeof(ptk));
	wlan_crypto_erase(gtk, sizeof(gtk));
}

int
main(void)
{
	run_integration();
	puts("intel ax211/common WLAN integration: PASS");
	return 0;
}
