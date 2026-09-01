/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/wlan.h"

#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/wlan-l2.h"
#include "kern/net/wlan-wpa2.h"

#include <hal/hal.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define WLAN_LOCAL_ASSOC_CAPABILITY 0x0011U
#define WLAN_RECONNECT_ATTEMPT_MAX 5U
#define WLAN_RECONNECT_SCAN_DEADLINE_TICKS 200U
#define WLAN_BEACON_MISS_MULTIPLIER 20U
#define WLAN_BEACON_WATCH_MIN_TICKS (2U * KERN_CLOCK_HZ)
#define WLAN_BEACON_WATCH_MAX_TICKS (10U * KERN_CLOCK_HZ)

static const uint64_t wlan_reconnect_delay_ticks[
    WLAN_RECONNECT_ATTEMPT_MAX] = { 0U, 100U, 200U, 400U, 800U };

extern void net_worker_wakeup(void) __attribute__((weak));
extern void sched_yield(void) __attribute__((weak));
extern bool hal_entropy_fill(void *, size_t) __attribute__((weak));

struct wlan_cache_entry {
	struct wlan_bss_record bss;
	uint64_t last_seen;
};

enum wlan_scan_step_state {
	WLAN_SCAN_STEP_NONE = 0,
	WLAN_SCAN_STEP_NEED_TUNE,
	WLAN_SCAN_STEP_TUNING,
	WLAN_SCAN_STEP_DWELL
};

struct wlan_station {
	struct spinlock lock;
	int used;
	int blocked;
	int closing;
	int lifecycle_inflight;
	int shutdown_owned;
	unsigned control_inflight;
	unsigned active;
	struct net_device *device;
	const struct wlan_radio_ops *ops;
	void *radio_context;
	wlan_clock_fn clock;
	void *clock_context;
	struct wlan_scan_profile scan_profile;

	uint64_t next_generation;
	uint64_t operation_generation;
	uint64_t connection_generation;
	uint64_t connection_deadline;
	uint64_t connection_step_deadline;
	uint64_t reconnect_started;
	uint64_t reconnect_deadline;
	uint64_t reconnect_next_attempt;
	uint64_t reconnect_cleanup_retry;
	uint64_t reconnect_generation;
	uint64_t beacon_watch_deadline;
	uint32_t reconnect_attempts;
	int reconnect_pending;
	int reconnect_scan_active;
	int reconnect_scan_ready;
	int reconnect_bss_seen;
	uint32_t state;
	int32_t terminal_error;
	uint32_t administrative_up;
	uint32_t authenticated;
	uint32_t associated;
	uint32_t key_installed;
	uint32_t controlled_port;
	uint32_t retry_count;
	int connect_driver_active;
	int connect_stop_pending;
	int connect_retire_explicit;
	struct wlan_bss_record selected;
	uint8_t credential[WLAN_PASSPHRASE_STORAGE];
	uint32_t credential_length;
	struct wlan_wpa2_engine wpa2;
	struct wlan_l2_rx_state l2_rx;
	uint64_t transmit_packet_number;
	uint64_t transmit_cookie;

	uint64_t scan_generation;
	uint64_t scan_deadline;
	uint64_t scan_retry_deadline;
	uint64_t connect_retry_deadline;
	uint64_t snapshot_generation;
	uint64_t cache_sequence;
	uint32_t scan_state;
	int32_t scan_error;
	uint32_t staging_count;
	uint32_t staging_truncated;
	uint32_t snapshot_count;
	uint32_t snapshot_truncated;
	uint32_t scan_step_index;
	uint32_t scan_step_state;
	uint32_t scan_ready_pending;
	uint32_t scan_publish_pending;
	int32_t scan_event_error;
	uint64_t scan_step_deadline;
	int scan_driver_active;
	struct wlan_cache_entry staging[WLAN_BSS_MAX];
	struct wlan_cache_entry snapshot[WLAN_BSS_MAX];
#ifdef WLAN_TESTING
	wlan_station_test_hook_fn test_report_hook;
	void *test_report_hook_context;
	unsigned test_control_waiters;
#endif
};

static struct spinlock wlan_registry_lock;
static struct wlan_station wlan_stations[NET_DEVICE_MAX];
static atomic_uint_t wlan_initialized;
static int wlan_stopping;
static int wlan_shutdown_inflight;

static const struct wlan_wpa2_ops station_wpa2_ops;
static void station_reconnect_schedule_locked(struct wlan_station *,
	uint64_t);
static int station_retire_controlled(struct wlan_station *, int);

static void
secure_zero(void *memory, size_t length)
{
	volatile uint8_t *bytes = memory;

	while (length-- != 0U)
		*bytes++ = 0U;
}

static uint64_t
default_clock(void *context)
{
	(void)context;
	return clock_ticks();
}

static uint64_t
deadline_after(uint64_t now, uint64_t delta)
{
	if (UINT64_MAX - now < delta)
		return now;
	return now + delta;
}

static int
deadline_checked(uint64_t now, uint64_t delta, uint64_t *result)
{
	if (result == NULL || UINT64_MAX - now < delta)
		return EOVERFLOW;
	*result = now + delta;
	return 0;
}

static uint64_t
deadline_local(uint64_t now, uint64_t delta, uint64_t total_deadline)
{
	uint64_t local = deadline_after(now, delta);

	if (total_deadline != 0U && total_deadline < local)
		return total_deadline;
	return local;
}

static uint64_t
station_beacon_watch_ticks(uint16_t beacon_interval_tu)
{
	uint64_t microseconds;
	uint64_t ticks;

	/* One TU is 1024 microseconds.  Twenty missed beacons tolerates ordinary
	 * scheduling/airtime jitter; the two/ten-second bounds keep both common
	 * intervals and malformed/extreme advertisements finite and conservative. */
	microseconds = (uint64_t)beacon_interval_tu * 1024U *
	    WLAN_BEACON_MISS_MULTIPLIER;
	if (microseconds > (UINT64_MAX - 999999U) / KERN_CLOCK_HZ)
		ticks = WLAN_BEACON_WATCH_MAX_TICKS;
	else
		ticks = (microseconds * KERN_CLOCK_HZ + 999999U) / 1000000U;
	if (ticks < WLAN_BEACON_WATCH_MIN_TICKS)
		ticks = WLAN_BEACON_WATCH_MIN_TICKS;
	if (ticks > WLAN_BEACON_WATCH_MAX_TICKS)
		ticks = WLAN_BEACON_WATCH_MAX_TICKS;
	return ticks;
}

static void
station_beacon_watch_refresh_locked(struct wlan_station *station, uint64_t now)
{
	station->beacon_watch_deadline = deadline_after(now,
	    station_beacon_watch_ticks(station->selected.beacon_interval_tu));
}

static int
station_beacon_watch_active_locked(const struct wlan_station *station)
{
	if (!station->connect_driver_active || station->reconnect_pending ||
	    station->beacon_watch_deadline == 0U)
		return 0;
	/* The common state and nonzero latch are published under this lock after
	 * authorization.  They remain valid through pairwise (FOUR_WAY) and group
	 * (CONNECTED) rekey.  Avoid reading engine-private fields here: beacon
	 * ingestion does not own the serialized WPA control gate. */
	return station->state == WLAN_STATE_CONNECTED ||
	    station->state == WLAN_STATE_FOUR_WAY;
}

static int
deadline_expired(uint64_t now, uint64_t deadline)
{
	return deadline != 0U && now >= deadline;
}

static void
wlan_worker_wakeup(void)
{
	if (net_worker_wakeup != NULL)
		net_worker_wakeup();
}

static int
bytes_zero(const void *memory, size_t length)
{
	const uint8_t *bytes = memory;

	while (length-- != 0U) {
		if (*bytes++ != 0U)
			return 0;
	}
	return 1;
}

static uint32_t
channel_frequency(uint32_t channel)
{
	return channel == 14U ? 2484U : 2407U + 5U * channel;
}

static int
scan_profile_validate(const struct wlan_scan_profile *profile)
{
	uint32_t index;
	uint32_t seen = 0U;

	if (profile == NULL || profile->channel_count == 0U ||
	    profile->channel_count > WLAN_SCAN_CHANNEL_MAX ||
	    !bytes_zero(profile->reserved, sizeof(profile->reserved)))
		return EINVAL;
	for (index = 0U; index < WLAN_SCAN_CHANNEL_MAX; index++) {
		const struct wlan_scan_channel *channel =
		    &profile->channels[index];

		if (index >= profile->channel_count) {
			if (!bytes_zero(channel, sizeof(*channel)))
				return EINVAL;
			continue;
		}
		if (channel->channel == 0U || channel->channel > 14U ||
		    channel->center_frequency_mhz !=
		    channel_frequency(channel->channel) ||
		    (channel->flags & ~WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED) != 0U ||
		    channel->reserved != 0U ||
		    (seen & (1U << (channel->channel - 1U))) != 0U)
			return EINVAL;
		seen |= 1U << (channel->channel - 1U);
	}
	return 0;
}

static int
device_name_matches(const struct net_device *device, const char *name)
{
	unsigned index;

	for (index = 0; index < IFNAMSIZ; index++) {
		if ((uint8_t)device->name[index] != (uint8_t)name[index])
			return 0;
		if (name[index] == '\0')
			return 1;
	}
	return 0;
}

static int
header_validate(const struct net_device *device,
	const struct wlan_ioctl_header *header, size_t size)
{
	if (device == NULL || header == NULL)
		return ENODEV;
	if (header->version != WLAN_ABI_VERSION || header->size != size)
		return EINVAL;
	if (!device_name_matches(device, header->ifr_name))
		return ENODEV;
	return 0;
}

static uint64_t
station_now_locked(struct wlan_station *station)
{
	return station->clock(station->clock_context);
}

static int
station_generation_locked(struct wlan_station *station, uint64_t *result)
{
	if (station->next_generation == UINT64_MAX)
		return EOVERFLOW;
	station->next_generation++;
	if (station->next_generation == 0U)
		return EOVERFLOW;
	*result = station->next_generation;
	return 0;
}

static void
station_cancel_reconnect_locked(struct wlan_station *station)
{
	station->reconnect_started = 0U;
	station->reconnect_deadline = 0U;
	station->reconnect_next_attempt = 0U;
	station->reconnect_cleanup_retry = 0U;
	station->reconnect_generation = 0U;
	station->reconnect_attempts = 0U;
	station->reconnect_pending = 0;
	station->reconnect_scan_active = 0;
	station->reconnect_scan_ready = 0;
	station->reconnect_bss_seen = 0;
}

static void
station_clear_connection_locked(struct wlan_station *station)
{
	secure_zero(station->credential, sizeof(station->credential));
	station->credential_length = 0U;
	station->authenticated = 0U;
	station->associated = 0U;
	station->key_installed = 0U;
	station->controlled_port = 0U;
	station->retry_count = 0U;
	station->connection_deadline = 0U;
	station->connection_step_deadline = 0U;
	station->beacon_watch_deadline = 0U;
	station_cancel_reconnect_locked(station);
	memset(&station->selected, 0, sizeof(station->selected));
}

static void
station_finish_connection_retire_locked(struct wlan_station *station)
{
	int explicit_retire = station->connect_retire_explicit;

	station_clear_connection_locked(station);
	memset(&station->l2_rx, 0, sizeof(station->l2_rx));
	station->transmit_packet_number = 0U;
	station->transmit_cookie = 0U;
	station->connect_driver_active = 0;
	station->connect_stop_pending = 0;
	station->connect_retire_explicit = 0;
	station->connect_retry_deadline = 0U;
	if (explicit_retire) {
		if (station->scan_driver_active) {
			station->state = station->administrative_up ?
			    WLAN_STATE_FAILED : WLAN_STATE_DOWN;
			station->terminal_error = station->scan_error != 0 ?
			    station->scan_error : EBUSY;
		} else {
			station->state = station->administrative_up ?
			    WLAN_STATE_IDLE : WLAN_STATE_DOWN;
			station->terminal_error = 0;
		}
	} else {
		station->state = station->administrative_up ?
		    WLAN_STATE_FAILED : WLAN_STATE_DOWN;
	}
}

static int
station_carrier_down_locked(struct wlan_station *station)
{
	station->controlled_port = 0U;
	return net_device_set_carrier(station->device, 0);
}

static int
station_arm_reconnect_locked(struct wlan_station *station, int reason)
{
	uint64_t deadline;
	uint64_t now;
	int error;

	if (station->reconnect_pending)
		return EALREADY;
	if (!station->administrative_up ||
	    !wlan_wpa2_engine_can_reconnect(&station->wpa2))
		return ENOTCONN;
	now = station_now_locked(station);
	error = deadline_checked(now, WLAN_CONNECT_DEADLINE_TICKS, &deadline);
	if (error != 0)
		return error;
	station->reconnect_pending = 1;
	station->reconnect_started = now;
	station->reconnect_deadline = deadline;
	station->reconnect_next_attempt = now;
	/* A protocol failure may have left an inverse driver barrier uncertain.
	 * Give that checked cleanup its own visible worker deadline.  A caller
	 * which already completed cleanup synchronously replaces this with zero
	 * and schedules the immediate first scan. */
	station->reconnect_cleanup_retry = deadline_after(now, 1U);
	station->reconnect_generation = 0U;
	station->reconnect_attempts = 0U;
	station->reconnect_scan_active = 0;
	station->reconnect_scan_ready = 0;
	station->reconnect_bss_seen = 0;
	station->connect_retire_explicit = 0;
	station->beacon_watch_deadline = 0U;
	station->connection_deadline = deadline;
	station->connection_step_deadline = 0U;
	station->connect_stop_pending = 0;
	station->connect_retry_deadline = 0U;
	station->terminal_error = reason != 0 ? reason : ENETDOWN;
	station->retry_count = 0U;
	station->state = WLAN_STATE_AUTHENTICATING;
	return 0;
}

static uint64_t
station_wpa_deadline(struct wlan_station *station)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t now;

	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);
	deadline = deadline_local(now, WLAN_CONNECT_TRANSITION_TICKS,
	    station->connection_deadline);
	spin_unlock_irqrestore(&station->lock, enabled);
	return deadline;
}

static uint64_t
station_wpa_cleanup_deadline(struct wlan_station *station)
{
	unsigned long enabled;
	uint64_t deadline;

	/* Cleanup is a safety barrier, not another handshake attempt.  It gets a
	 * fresh finite budget even when the 30-second protocol budget expired;
	 * otherwise an uncertain CAM write could never be proven absent. */
	enabled = spin_lock_irqsave(&station->lock);
	deadline = deadline_after(station_now_locked(station),
	    WLAN_CONNECT_TRANSITION_TICKS);
	spin_unlock_irqrestore(&station->lock, enabled);
	return deadline;
}

static void
station_sync_wpa_locked(struct wlan_station *station)
{
	enum wlan_wpa2_state state = wlan_wpa2_engine_state(&station->wpa2);

	station->connection_step_deadline =
	    wlan_wpa2_engine_next_deadline(&station->wpa2);
	station->authenticated = 0U;
	station->associated = station->wpa2.associated != 0U;
	station->key_installed = station->wpa2.pairwise_installed != 0U &&
	    station->wpa2.group_installed != 0U;
	station->controlled_port = station->wpa2.authorized != 0U;
	station->retry_count = station->wpa2.retry_count;
	switch (state) {
	case WLAN_WPA2_STATE_AUTH_TX:
	case WLAN_WPA2_STATE_AUTH_RESPONSE:
		station->state = WLAN_STATE_AUTHENTICATING;
		break;
	case WLAN_WPA2_STATE_ASSOC_TX:
	case WLAN_WPA2_STATE_ASSOC_RESPONSE:
		station->authenticated = 1U;
		station->state = WLAN_STATE_ASSOCIATING;
		break;
	case WLAN_WPA2_STATE_MESSAGE_1:
	case WLAN_WPA2_STATE_MESSAGE_2_TX:
	case WLAN_WPA2_STATE_MESSAGE_3:
	case WLAN_WPA2_STATE_MESSAGE_4_TX:
	case WLAN_WPA2_STATE_MESSAGE_4_RETRANSMIT_TX:
	case WLAN_WPA2_STATE_PAIRWISE_STAGE:
	case WLAN_WPA2_STATE_PAIRWISE_ACTIVATE:
		station->authenticated = 1U;
		station->state = WLAN_STATE_FOUR_WAY;
		break;
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX:
	case WLAN_WPA2_STATE_GROUP_MESSAGE_2_RETRANSMIT_TX:
	case WLAN_WPA2_STATE_GROUP_STAGE:
	case WLAN_WPA2_STATE_GROUP_ACTIVATE:
		station->authenticated = 1U;
		station->associated = 1U;
		station->key_installed = 1U;
		station->controlled_port = 1U;
		station->state = WLAN_STATE_CONNECTED;
		break;
	case WLAN_WPA2_STATE_RECONNECT_WAIT:
		station->state = WLAN_STATE_AUTHENTICATING;
		break;
	case WLAN_WPA2_STATE_AUTHORIZED:
		station->authenticated = 1U;
		station->associated = 1U;
		station->key_installed = 1U;
		station->controlled_port = 1U;
		station->state = WLAN_STATE_CONNECTED;
		station->terminal_error = 0;
		station->connection_deadline = 0U;
		station_cancel_reconnect_locked(station);
		if (station->beacon_watch_deadline == 0U)
			station_beacon_watch_refresh_locked(station,
			    station_now_locked(station));
		break;
	case WLAN_WPA2_STATE_FAILED:
		station->terminal_error = wlan_wpa2_engine_last_error(
		    &station->wpa2);
		if (!station->reconnect_pending &&
		    station_arm_reconnect_locked(station,
		    station->terminal_error) == 0)
			break;
		if (station->reconnect_pending) {
			if (station->reconnect_cleanup_retry == 0U)
				station->reconnect_cleanup_retry = deadline_after(
				    station_now_locked(station), 1U);
			station->state = WLAN_STATE_AUTHENTICATING;
			break;
		}
		station->state = WLAN_STATE_FAILED;
		if (!station->reconnect_pending)
			station->connect_stop_pending = station->wpa2.configured ||
			    station->wpa2.associated ||
			    station->wpa2.pairwise_installed ||
			    station->wpa2.group_installed || station->wpa2.authorized;
		break;
	case WLAN_WPA2_STATE_IDLE:
	default:
		break;
	}
}

static int
station_wpa_entropy_fill(void *context, void *buffer, size_t length)
{
	(void)context;
	return hal_entropy_fill != NULL && hal_entropy_fill(buffer, length) ?
	    0 : EIO;
}

static int
station_wpa_radio_start(void *context, uint64_t generation,
	const uint8_t bssid[6], uint32_t channel, uint64_t deadline)
{
	struct wlan_station *station = context;
	unsigned long enabled;
	int error;

	if (station == NULL || bssid == NULL ||
	    station->ops->connect_start == NULL)
		return EOPNOTSUPP;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->selected.channel != channel ||
	    memcmp(station->selected.bssid, bssid, 6U) != 0) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}
	if (station->connection_generation != generation) {
		/* reconnect() retires the old generation before this callback.  Publish
		 * the freshly reserved generation only at that checked boundary, never
		 * before cleanup of the old driver/key producers has completed. */
		if (!station->reconnect_pending ||
		    station->reconnect_generation != generation) {
			spin_unlock_irqrestore(&station->lock, enabled);
			return ESTALE;
		}
		station->connection_generation = generation;
		station->reconnect_generation = 0U;
		station->connect_driver_active = 0;
		station->transmit_packet_number = 0U;
		station->transmit_cookie = 0U;
		memset(&station->l2_rx, 0, sizeof(station->l2_rx));
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	error = station->ops->connect_start(station->radio_context, generation,
	    &station->selected, deadline);
	if (error == 0) {
		enabled = spin_lock_irqsave(&station->lock);
		if (station->connection_generation == generation)
			station->connect_driver_active = 1;
		else
			error = ESTALE;
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	return error;
}

static int
station_wpa_transmit(void *context, uint64_t generation, uint64_t cookie,
	enum wlan_wpa2_tx_kind kind, const uint8_t destination[6],
	const uint8_t *frame, size_t length, uint64_t deadline)
{
	struct wlan_station *station = context;
	struct wlan_radio_tx_request request;
	uint8_t ethernet[WLAN_L2_ETHERNET_HEADER_SIZE +
	    WLAN_WPA2_EAPOL_FRAME_MAX];
	uint8_t mpdu[WLAN_L2_MPDU_MAX];
	const uint8_t *wire_frame = frame;
	size_t wire_length = length;
	unsigned long enabled;
	uint64_t key_generation = 0U;
	uint64_t packet_number = 0U;
	int protected_frame = 0;
	int error;

	if (station == NULL || destination == NULL || frame == NULL ||
	    station->ops->frame_transmit == NULL)
		return EOPNOTSUPP;
	memset(&request, 0, sizeof(request));
	if (kind == WLAN_WPA2_TX_MANAGEMENT) {
		request.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	} else if (kind == WLAN_WPA2_TX_EAPOL) {
		if (length > WLAN_WPA2_EAPOL_FRAME_MAX)
			return EMSGSIZE;
		/* The initial four-way exchange is clear.  Once a pairwise key is
		 * active, group-key responses, pairwise-rekey M2/M4, and later
		 * retransmissions use the active (never staged) generation.  This is
		 * independent of the controlled port, which is intentionally closed
		 * during pairwise rekey. */
		enabled = spin_lock_irqsave(&station->lock);
		if (station->connection_generation != generation) {
			spin_unlock_irqrestore(&station->lock, enabled);
			return ESTALE;
		}
		protected_frame = station->wpa2.pairwise_installed &&
		    (station->wpa2.authorized || station->wpa2.pairwise_rekey);
		if (protected_frame) {
			if (station->transmit_packet_number >=
			    0x0000ffffffffffffULL) {
				spin_unlock_irqrestore(&station->lock, enabled);
				return EOVERFLOW;
			}
			station->transmit_packet_number++;
			packet_number = station->transmit_packet_number;
			key_generation = station->wpa2.key_generation;
		}
		spin_unlock_irqrestore(&station->lock, enabled);
		memcpy(ethernet, destination, 6U);
		memcpy(ethernet + 6U, station->device->hwaddr, 6U);
		ethernet[12U] = 0x88U;
		ethernet[13U] = 0x8eU;
		memcpy(ethernet + WLAN_L2_ETHERNET_HEADER_SIZE, frame, length);
		error = wlan_l2_build_data(station->device->hwaddr,
		    station->selected.bssid, ethernet,
		    WLAN_L2_ETHERNET_HEADER_SIZE + length, protected_frame, 0U,
		    packet_number, mpdu,
		    sizeof(mpdu), &wire_length);
		if (error != 0)
			return error;
		request.frame_class = WLAN_RADIO_FRAME_EAPOL;
		wire_frame = mpdu;
	} else {
		return EINVAL;
	}
	request.generation = generation;
	request.cookie = cookie;
	request.deadline_ticks = deadline;
	request.frame = wire_frame;
	request.length = wire_length;
	request.encrypted = protected_frame != 0;
	request.key_generation = key_generation;
	request.packet_number = packet_number;
	error = station->ops->frame_transmit(station->radio_context, &request);
	wlan_crypto_erase(ethernet, sizeof(ethernet));
	wlan_crypto_erase(mpdu, sizeof(mpdu));
	return error;
}

static int
station_wpa_association_set(void *context, uint64_t generation,
	const uint8_t bssid[6], uint16_t aid)
{
	struct wlan_station *station = context;

	if (station == NULL || station->ops->association_set == NULL)
		return EOPNOTSUPP;
	return station->ops->association_set(station->radio_context,
	    generation, bssid, aid, station_wpa_deadline(station));
}

static int
station_wpa_association_clear(void *context, uint64_t generation)
{
	struct wlan_station *station = context;

	if (station == NULL || station->ops->association_clear == NULL)
		return EOPNOTSUPP;
	return station->ops->association_clear(station->radio_context,
	    generation, station_wpa_cleanup_deadline(station));
}

static int
station_wpa_key_install(void *context, uint64_t generation,
	enum wlan_wpa2_key_kind kind, uint8_t key_index,
	const uint8_t key[16], uint64_t key_generation,
	uint64_t receive_packet_number)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct wlan_station *station = context;
	struct wlan_radio_key_request request;
	unsigned long enabled;
	int error;

	if (station == NULL || key == NULL || station->ops->key_install == NULL)
		return EOPNOTSUPP;
	if (generation == 0U || key_generation == 0U || key_index > 3U ||
	    receive_packet_number > 0x0000ffffffffffffULL ||
	    (kind != WLAN_WPA2_KEY_PAIRWISE &&
	    kind != WLAN_WPA2_KEY_GROUP) ||
	    (kind == WLAN_WPA2_KEY_PAIRWISE && key_index != 0U))
		return EINVAL;
	memset(&request, 0, sizeof(request));
	request.generation = generation;
	request.key_generation = key_generation;
	request.deadline_ticks = station_wpa_deadline(station);
	request.receive_packet_number = receive_packet_number;
	request.kind = kind == WLAN_WPA2_KEY_PAIRWISE ?
	    WLAN_RADIO_KEY_PAIRWISE : WLAN_RADIO_KEY_GROUP;
	request.key_index = key_index;
	memcpy(request.address, kind == WLAN_WPA2_KEY_PAIRWISE ?
	    station->selected.bssid : broadcast, 6U);
	memcpy(request.key, key, sizeof(request.key));
	error = station->ops->key_install(station->radio_context, &request);
	if (error == 0) {
		enabled = spin_lock_irqsave(&station->lock);
		if (kind == WLAN_WPA2_KEY_PAIRWISE &&
		    !station->wpa2.pending_pairwise_installed) {
			station->l2_rx.pairwise_key_generation = key_generation;
			station->l2_rx.pairwise_packet_number =
			    receive_packet_number;
		} else if (kind == WLAN_WPA2_KEY_GROUP &&
		    !station->wpa2.pending_group_installed) {
			station->l2_rx.group_key_generation[key_index] =
			    key_generation;
			station->l2_rx.group_packet_number[key_index] =
			    receive_packet_number;
		}
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	wlan_crypto_erase(&request, sizeof(request));
	return error;
}

static int
station_wpa_keys_activate(void *context, uint64_t generation,
	uint64_t pairwise_key_generation, uint64_t group_key_generation)
{
	struct wlan_station *station = context;
	unsigned long enabled;
	int error;

	if (station == NULL || pairwise_key_generation == 0U ||
	    group_key_generation == 0U || station->ops->keys_activate == NULL)
		return EOPNOTSUPP;
	error = station->ops->keys_activate(station->radio_context, generation,
	    pairwise_key_generation, group_key_generation,
	    station_wpa_deadline(station));
	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->connection_generation != generation) {
		error = ESTALE;
	} else {
		if (station->l2_rx.pairwise_key_generation !=
		    pairwise_key_generation) {
			station->l2_rx.pairwise_key_generation =
			    pairwise_key_generation;
			station->l2_rx.pairwise_packet_number = 0U;
			station->transmit_packet_number = 0U;
		}
		station->l2_rx.group_key_generation[
		    station->wpa2.pending_gtk_index] = group_key_generation;
		station->l2_rx.group_packet_number[
		    station->wpa2.pending_gtk_index] =
		    station->wpa2.pending_group_receive_packet_number;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	return error;
}

static int
station_wpa_key_receive_pn_advance(void *context, uint64_t generation,
	enum wlan_wpa2_key_kind kind, uint8_t key_index,
	uint64_t key_generation, uint64_t receive_packet_number)
{
	struct wlan_station *station = context;
	unsigned long enabled;
	uint64_t *floor;
	int error = 0;

	if (station == NULL || generation == 0U || key_generation == 0U ||
	    key_index > 3U ||
	    (kind != WLAN_WPA2_KEY_PAIRWISE && kind != WLAN_WPA2_KEY_GROUP) ||
	    (kind == WLAN_WPA2_KEY_PAIRWISE && key_index != 0U) ||
	    receive_packet_number > 0x0000ffffffffffffULL)
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->connection_generation != generation) {
		error = ESTALE;
	} else if (kind == WLAN_WPA2_KEY_GROUP &&
	    station->wpa2.pending_group_installed &&
	    station->wpa2.pending_group_key_generation == key_generation &&
	    station->wpa2.pending_gtk_index == key_index) {
		/* The staged RSC is published atomically by keys_activate(). */
		error = 0;
	} else if ((kind == WLAN_WPA2_KEY_PAIRWISE ?
	    station->l2_rx.pairwise_key_generation :
	    station->l2_rx.group_key_generation[key_index]) != key_generation) {
		error = ESTALE;
	} else {
		floor = kind == WLAN_WPA2_KEY_PAIRWISE ?
		    &station->l2_rx.pairwise_packet_number :
		    &station->l2_rx.group_packet_number[key_index];
		/* RX may already have advanced beyond a freshly sampled AP RSC.
		 * This barrier is therefore max-assignment, never a reset. */
		if (receive_packet_number > *floor)
			*floor = receive_packet_number;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	return error;
}

static int
station_wpa_key_delete(void *context, uint64_t generation,
	enum wlan_wpa2_key_kind kind, uint8_t key_index,
	uint64_t key_generation)
{
	struct wlan_station *station = context;
	unsigned long enabled;
	int error;

	if (station == NULL || station->ops->key_delete == NULL)
		return EOPNOTSUPP;
	if (generation == 0U || key_generation == 0U || key_index > 3U ||
	    (kind != WLAN_WPA2_KEY_PAIRWISE &&
	    kind != WLAN_WPA2_KEY_GROUP) ||
	    (kind == WLAN_WPA2_KEY_PAIRWISE && key_index != 0U))
		return EINVAL;
	error = station->ops->key_delete(station->radio_context, generation,
	    kind == WLAN_WPA2_KEY_PAIRWISE ? WLAN_RADIO_KEY_PAIRWISE :
	    WLAN_RADIO_KEY_GROUP, key_index, key_generation,
	    station_wpa_cleanup_deadline(station));
	if (error == 0) {
		enabled = spin_lock_irqsave(&station->lock);
		if (kind == WLAN_WPA2_KEY_PAIRWISE &&
		    station->l2_rx.pairwise_key_generation == key_generation) {
			station->l2_rx.pairwise_key_generation = 0U;
			station->l2_rx.pairwise_packet_number = 0U;
		} else if (kind == WLAN_WPA2_KEY_GROUP &&
		    station->l2_rx.group_key_generation[key_index] ==
		    key_generation) {
			station->l2_rx.group_key_generation[key_index] = 0U;
				station->l2_rx.group_packet_number[key_index] = 0U;
		}
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	return error;
}

static int
station_wpa_authorized_set(void *context, uint64_t generation,
	int authorized)
{
	struct wlan_station *station = context;
	unsigned long enabled;
	int error;

	if (station == NULL || (authorized != 0 && authorized != 1))
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->connection_generation != generation) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}
	if (authorized) {
		if (!station->wpa2.pairwise_installed ||
		    !station->wpa2.group_installed) {
			spin_unlock_irqrestore(&station->lock, enabled);
			return EACCES;
		}
		error = net_device_set_carrier(station->device, 1);
		if (error == 0) {
			station->controlled_port = 1U;
			station->key_installed = 1U;
		}
	} else {
		error = station_carrier_down_locked(station);
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	return error;
}

static int
station_wpa_radio_stop(void *context, uint64_t generation)
{
	struct wlan_station *station = context;
	unsigned long enabled;
	int error;

	if (station == NULL || station->ops->disconnect == NULL)
		return EOPNOTSUPP;
	error = station->ops->disconnect(station->radio_context, generation);
	if (error == 0) {
		enabled = spin_lock_irqsave(&station->lock);
		if (station->connection_generation == generation)
			station->connect_driver_active = 0;
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	return error;
}

static const struct wlan_wpa2_ops station_wpa2_ops = {
	.entropy_fill = station_wpa_entropy_fill,
	.radio_start = station_wpa_radio_start,
	.transmit = station_wpa_transmit,
	.association_set = station_wpa_association_set,
	.association_clear = station_wpa_association_clear,
	.key_install = station_wpa_key_install,
	.key_receive_pn_advance = station_wpa_key_receive_pn_advance,
	.key_delete = station_wpa_key_delete,
	.keys_activate = station_wpa_keys_activate,
	.authorized_set = station_wpa_authorized_set,
	.radio_stop = station_wpa_radio_stop
};

/* Caller owns the station control gate.  This is deliberately thread/poll
 * context: link loss closes carrier and crosses checked driver/key barriers
 * before the worker may start a fresh generation. */
static int
station_link_lost_controlled(struct wlan_station *station,
	uint64_t generation, int reason)
{
	unsigned long enabled;
	int carrier_error;
	int reconnectable;
	int error;

	if (reason <= 0)
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->administrative_up ||
	    station->connection_generation != generation) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}
	if (station->connect_retire_explicit) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}
	if (station->reconnect_pending) {
		enum wlan_wpa2_state state =
		    wlan_wpa2_engine_state(&station->wpa2);

		spin_unlock_irqrestore(&station->lock, enabled);
		if (state == WLAN_WPA2_STATE_RECONNECT_WAIT)
			return EALREADY;
		error = wlan_wpa2_engine_link_lost(&station->wpa2, reason);
		enabled = spin_lock_irqsave(&station->lock);
		station_sync_wpa_locked(station);
		if (station->reconnect_pending) {
			uint64_t now = station_now_locked(station);

			if (error == 0) {
				station->reconnect_cleanup_retry = 0U;
				station_reconnect_schedule_locked(station, now);
			} else {
				station->reconnect_cleanup_retry = deadline_after(now, 1U);
			}
		}
		spin_unlock_irqrestore(&station->lock, enabled);
		return error;
	}
	reconnectable = wlan_wpa2_engine_can_reconnect(&station->wpa2);
	if (!reconnectable) {
		enum wlan_wpa2_state state =
		    wlan_wpa2_engine_state(&station->wpa2);

		if (state == WLAN_WPA2_STATE_IDLE ||
		    state == WLAN_WPA2_STATE_FAILED) {
			spin_unlock_irqrestore(&station->lock, enabled);
			return ENOTCONN;
		}
		carrier_error = station_carrier_down_locked(station);
		station->terminal_error = reason;
		station->state = WLAN_STATE_FAILED;
		station->connect_retire_explicit = 0;
		station->connection_deadline = 0U;
		station->connection_step_deadline = 0U;
		spin_unlock_irqrestore(&station->lock, enabled);
		error = wlan_wpa2_engine_stop(&station->wpa2);
		enabled = spin_lock_irqsave(&station->lock);
		if (error != 0) {
			station->connect_stop_pending = 1;
			station->connect_retry_deadline = deadline_after(
			    station_now_locked(station), 1U);
		}
		station->state = WLAN_STATE_FAILED;
		station->terminal_error = reason;
		spin_unlock_irqrestore(&station->lock, enabled);
		return error != 0 ? error : carrier_error;
	}
	carrier_error = station_carrier_down_locked(station);
	error = station_arm_reconnect_locked(station, reason);
	if (error != 0) {
		spin_unlock_irqrestore(&station->lock, enabled);
		(void)wlan_wpa2_engine_link_lost(&station->wpa2, reason);
		(void)wlan_wpa2_engine_stop(&station->wpa2);
		return error;
	}
	spin_unlock_irqrestore(&station->lock, enabled);

	error = wlan_wpa2_engine_link_lost(&station->wpa2, reason);
	enabled = spin_lock_irqsave(&station->lock);
	station_sync_wpa_locked(station);
	if (station->reconnect_pending) {
		uint64_t now = station_now_locked(station);

		station->state = WLAN_STATE_AUTHENTICATING;
		station->terminal_error = reason;
		station->retry_count = 0U;
		if (error == 0) {
			station->reconnect_cleanup_retry = 0U;
			station_reconnect_schedule_locked(station, now);
		} else {
			station->reconnect_cleanup_retry = deadline_after(now, 1U);
		}
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (error != 0)
		return error;
	return carrier_error;
}

static int
station_enter(struct wlan_station *station)
{
	unsigned long enabled;

	if (station == NULL)
		return ENODEV;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used || station->blocked || station->closing) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENODEV;
	}
	if (station->active == UINT_MAX) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return EOVERFLOW;
	}
	station->active++;
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

static void
station_leave(struct wlan_station *station)
{
	unsigned long enabled = spin_lock_irqsave(&station->lock);

	if (station->active == 0U)
		__builtin_trap();
	station->active--;
	spin_unlock_irqrestore(&station->lock, enabled);
}

/* Control methods may sleep in a bus driver and therefore cannot run under
 * the station spinlock.  This thread-context serial gate stays held from the
 * state mutation through start/stop completion: a cancellation barrier can
 * never return before an earlier start method has itself returned. */
static void
station_control_enter(struct wlan_station *station)
{
#ifdef WLAN_TESTING
	int waiting = 0;
#endif
	for (;;) {
		unsigned long enabled = spin_lock_irqsave(&station->lock);

		if (!station->control_inflight) {
			station->control_inflight = 1U;
#ifdef WLAN_TESTING
			if (waiting) {
				if (station->test_control_waiters == 0U)
					__builtin_trap();
				station->test_control_waiters--;
			}
#endif
			spin_unlock_irqrestore(&station->lock, enabled);
			return;
		}
#ifdef WLAN_TESTING
		if (!waiting) {
			station->test_control_waiters++;
			waiting = 1;
		}
#endif
		spin_unlock_irqrestore(&station->lock, enabled);
		if (sched_yield != NULL)
			sched_yield();
		else
			__asm__ volatile("" ::: "memory");
	}
}

static void
station_control_leave(struct wlan_station *station)
{
	unsigned long enabled = spin_lock_irqsave(&station->lock);

	if (!station->control_inflight)
		__builtin_trap();
	station->control_inflight = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
}

static int
station_find_enter(struct net_device *device, struct wlan_station **result)
{
	unsigned long registry_enabled;
	unsigned index;
	int error = EOPNOTSUPP;

	if (device == NULL || result == NULL)
		return ENODEV;
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		struct wlan_station *station = &wlan_stations[index];
		unsigned long enabled;

		if (!station->used || station->device != device)
			continue;
		enabled = spin_lock_irqsave(&station->lock);
		if (station->used && !station->blocked && !station->closing &&
		    station->device == device) {
			if (station->active == UINT_MAX) {
				error = EOVERFLOW;
			} else {
				station->active++;
				*result = station;
				error = 0;
			}
		} else {
			error = ENODEV;
		}
		spin_unlock_irqrestore(&station->lock, enabled);
		break;
	}
	spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
	return error;
}

static int
station_index_enter(unsigned index, struct wlan_station **result)
{
	struct wlan_station *station;
	unsigned long registry_enabled;
	unsigned long enabled;
	int error = ENODEV;

	if (index >= NET_DEVICE_MAX || result == NULL)
		return ENODEV;
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	station = &wlan_stations[index];
	if (station->used) {
		enabled = spin_lock_irqsave(&station->lock);
		if (station->used && !station->blocked && !station->closing) {
			if (station->active != UINT_MAX) {
				station->active++;
				*result = station;
				error = 0;
			} else {
				error = EOVERFLOW;
			}
		}
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
	return error;
}

static int
bssid_compare(const uint8_t left[6], const uint8_t right[6])
{
	return memcmp(left, right, 6U);
}

static int
bssid_valid(const uint8_t bssid[6])
{
	unsigned index;
	unsigned nonzero = 0U;

	if ((bssid[0] & 0x01U) != 0U)
		return 0;
	for (index = 0; index < 6U; index++)
		nonzero |= bssid[index];
	return nonzero != 0U;
}

static size_t
probe_request_build(const struct wlan_station *station, int directed,
	uint8_t frame[64])
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	static const uint8_t rates[4] = { 0x82U, 0x84U, 0x8bU, 0x96U };
	size_t ssid_length = directed ? station->selected.ssid_length : 0U;
	size_t offset;

	memset(frame, 0, 64U);
	frame[0] = 0x40U;
	memcpy(frame + 4U, broadcast, sizeof(broadcast));
	memcpy(frame + 10U, station->device->hwaddr, 6U);
	memcpy(frame + 16U, broadcast, sizeof(broadcast));
	frame[24] = 0U;
	frame[25] = (uint8_t)ssid_length;
	memcpy(frame + 26U, station->selected.ssid, ssid_length);
	offset = 26U + ssid_length;
	frame[offset++] = 1U;
	frame[offset++] = sizeof(rates);
	memcpy(frame + offset, rates, sizeof(rates));
	return offset + sizeof(rates);
}

/* True when left is the entry evicted before right. */
static int
cache_entry_worse(const struct wlan_cache_entry *left,
	const struct wlan_cache_entry *right)
{
	if (left->bss.rssi_dbm != right->bss.rssi_dbm)
		return left->bss.rssi_dbm < right->bss.rssi_dbm;
	if (left->last_seen != right->last_seen)
		return left->last_seen < right->last_seen;
	return bssid_compare(left->bss.bssid, right->bss.bssid) > 0;
}

static int
cache_insert_locked(struct wlan_station *station,
	const struct wlan_bss_record *bss, uint64_t now)
{
	struct wlan_cache_entry incoming;
	unsigned index;
	unsigned worst = 0U;

	for (index = 0; index < station->staging_count; index++) {
		if (bssid_compare(station->staging[index].bss.bssid,
		    bss->bssid) != 0)
			continue;
		station->staging[index].bss = *bss;
		station->staging[index].bss.age_ms = 0U;
		station->staging[index].last_seen = now;
		return 0;
	}
	incoming.bss = *bss;
	incoming.bss.age_ms = 0U;
	incoming.last_seen = now;
	if (station->staging_count < WLAN_BSS_MAX) {
		station->staging[station->staging_count++] = incoming;
		return 0;
	}
	for (index = 1U; index < station->staging_count; index++) {
		if (cache_entry_worse(&station->staging[index],
		    &station->staging[worst]))
			worst = index;
	}
	if (!station->staging_truncated ||
	    cache_entry_worse(&station->staging[worst], &incoming))
		station->staging_truncated = 1U;
	if (cache_entry_worse(&station->staging[worst], &incoming))
		station->staging[worst] = incoming;
	return 0;
}

static void
cache_sort_by_bssid(struct wlan_cache_entry *entries, uint32_t count)
{
	uint32_t index;

	for (index = 1U; index < count; index++) {
		struct wlan_cache_entry value = entries[index];
		uint32_t position = index;

		while (position != 0U && bssid_compare(
		    entries[position - 1U].bss.bssid, value.bss.bssid) > 0) {
			entries[position] = entries[position - 1U];
			position--;
		}
		entries[position] = value;
	}
}

static int
bss_security_supported(const struct wlan_bss_record *bss)
{
	const uint32_t required = WLAN_SECURITY_PRIVACY | WLAN_SECURITY_WPA2 |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK;
	const uint32_t rejected = WLAN_SECURITY_WPA1 |
	    WLAN_SECURITY_PMF_REQUIRED;
	const uint32_t rejected_suites = WLAN_SECURITY_UNSUPPORTED_SUITE;

	return (bss->security & required) == required &&
	    (bss->security & (rejected | rejected_suites)) == 0U;
}

static int
station_select_bss_locked(struct wlan_station *station, const uint8_t *ssid,
	uint32_t ssid_length, struct wlan_bss_record *result)
{
	uint32_t index;
	int found = 0;

	for (index = 0; index < station->snapshot_count; index++) {
		const struct wlan_bss_record *candidate =
		    &station->snapshot[index].bss;

		if (candidate->ssid_length != ssid_length ||
		    memcmp(candidate->ssid, ssid, ssid_length) != 0 ||
		    !bss_security_supported(candidate))
			continue;
		if (!found || candidate->rssi_dbm > result->rssi_dbm ||
		    (candidate->rssi_dbm == result->rssi_dbm &&
		    bssid_compare(candidate->bssid, result->bssid) < 0)) {
			*result = *candidate;
			found = 1;
		}
	}
	return found ? 0 : ENOENT;
}

void
wlan_core_init(void)
{
	unsigned expected = 0U;
	unsigned index;

	if (atomic_load_acquire(&wlan_initialized) == 2U)
		return;
	if (!atomic_compare_exchange(&wlan_initialized, &expected, 1U)) {
		while (atomic_load_acquire(&wlan_initialized) != 2U) {
			if (sched_yield != NULL)
				sched_yield();
			else
				__asm__ volatile("" ::: "memory");
		}
		return;
	}
	memset(wlan_stations, 0, sizeof(wlan_stations));
	spin_init(&wlan_registry_lock, LOCK_RANK_SOCKET_REGISTRY,
	    "wlan-registry");
	for (index = 0U; index < NET_DEVICE_MAX; index++)
		spin_init(&wlan_stations[index].lock, LOCK_RANK_NETWORK,
		    "wlan-station");
	wlan_stopping = 0;
	wlan_shutdown_inflight = 0;
	atomic_store_release(&wlan_initialized, 2U);
}

int
wlan_station_attach(struct net_device *device,
	const struct wlan_radio_ops *ops, void *radio_context,
	const struct wlan_scan_profile *scan_profile,
	struct wlan_station **result)
{
	unsigned long enabled;
	unsigned index;
	struct wlan_station *free_station = NULL;
	int error;

	if (device == NULL || ops == NULL || result == NULL ||
	    scan_profile_validate(scan_profile) != 0)
		return EINVAL;
	if (device->hwaddr_len != 6U || !bssid_valid(device->hwaddr))
		return EINVAL;
	if ((ops->scan_channel_start != NULL) != (ops->scan_stop != NULL) ||
	    ((ops->connect_start != NULL || ops->disconnect != NULL ||
	    ops->association_set != NULL || ops->association_clear != NULL ||
	    ops->frame_transmit != NULL || ops->key_install != NULL ||
	    ops->key_delete != NULL || ops->keys_activate != NULL) &&
	    (ops->connect_start == NULL ||
	    ops->disconnect == NULL || ops->association_set == NULL ||
	    ops->association_clear == NULL || ops->frame_transmit == NULL ||
	    ops->key_install == NULL || ops->key_delete == NULL ||
	    ops->keys_activate == NULL)))
		return EINVAL;
	if (ops->management_transmit == NULL) {
		for (index = 0U; index < scan_profile->channel_count; index++) {
			if ((scan_profile->channels[index].flags &
			    WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED) != 0U)
				return EINVAL;
		}
	}
	if (atomic_load_acquire(&wlan_initialized) != 2U)
		wlan_core_init();
	/* The caller retains its allocation-owner reference across this call.
	 * The station acquires a distinct live reference before any device
	 * mutation or station publication. */
	if (!net_device_ref_live(device))
		return ENODEV;
	enabled = spin_lock_irqsave(&wlan_registry_lock);
	if (wlan_stopping) {
		spin_unlock_irqrestore(&wlan_registry_lock, enabled);
		net_device_release(device);
		return EBUSY;
	}
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (wlan_stations[index].used &&
		    wlan_stations[index].device == device) {
			spin_unlock_irqrestore(&wlan_registry_lock, enabled);
			net_device_release(device);
			return EEXIST;
		}
		if (!wlan_stations[index].used && free_station == NULL)
			free_station = &wlan_stations[index];
	}
	if (free_station == NULL) {
		spin_unlock_irqrestore(&wlan_registry_lock, enabled);
		net_device_release(device);
		return ENOSPC;
	}
	/* Validate and reserve the registry slot before mutating the device.  The
	 * registry (rank 100) may enter the device carrier guard (rank 125); no
	 * device operation enters the WLAN registry while holding that guard. */
	error = net_device_set_carrier(device, 0);
	if (error != 0) {
		spin_unlock_irqrestore(&wlan_registry_lock, enabled);
		net_device_release(device);
		return error;
	}
	{
		unsigned long station_enabled =
		    spin_lock_irqsave(&free_station->lock);

		/* The slot lock is initialized exactly once by wlan_core_init().
		 * Reset only the lifetime payload while the registry excludes all
		 * timer/admission lookups of this unused slot. */
		memset(&free_station->used, 0,
		    sizeof(*free_station) - offsetof(struct wlan_station, used));
		free_station->device = device;
		free_station->ops = ops;
		free_station->radio_context = radio_context;
		free_station->clock = default_clock;
		free_station->clock_context = NULL;
		free_station->scan_profile = *scan_profile;
		free_station->state = WLAN_STATE_DOWN;
		free_station->scan_state = WLAN_SCAN_IDLE;
		error = wlan_wpa2_engine_init(&free_station->wpa2,
		    &station_wpa2_ops, free_station);
		if (error != 0) {
			spin_unlock_irqrestore(&free_station->lock,
			    station_enabled);
			spin_unlock_irqrestore(&wlan_registry_lock, enabled);
			net_device_release(device);
			return error;
		}
		free_station->used = 1;
		spin_unlock_irqrestore(&free_station->lock, station_enabled);
	}
	*result = free_station;
	spin_unlock_irqrestore(&wlan_registry_lock, enabled);
	return 0;
}

int
wlan_station_open(struct wlan_station *station)
{
	unsigned long enabled;
	int error = station_enter(station);

	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&station->lock);
	station->administrative_up = 1U;
	if (station->state == WLAN_STATE_DOWN ||
	    station->state == WLAN_STATE_FAILED) {
		station->state = WLAN_STATE_IDLE;
		station->terminal_error = 0;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	station_leave(station);
	return 0;
}

int
wlan_station_report_scan_bss(struct wlan_station *station,
	uint64_t generation, const struct wlan_bss_record *bss)
{
	struct wlan_bss_record normalized;
	unsigned long enabled;
	uint64_t now;
	int wake_worker = 0;
	int error;

	uint32_t expected_frequency;
	const uint32_t known_security = WLAN_SECURITY_PRIVACY |
	    WLAN_SECURITY_WPA1 | WLAN_SECURITY_WPA2 | WLAN_SECURITY_TKIP |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK |
	    WLAN_SECURITY_IEEE8021X | WLAN_SECURITY_SAE |
	    WLAN_SECURITY_PMF_CAPABLE | WLAN_SECURITY_PMF_REQUIRED |
	    WLAN_SECURITY_UNSUPPORTED_SUITE;

	if (bss == NULL || bss->ssid_length > WLAN_SSID_MAX ||
	    bss->channel == 0U || bss->channel > 14U ||
	    bss->capability > UINT16_MAX ||
	    bss->beacon_interval_tu > UINT16_MAX ||
	    (bss->security & ~known_security) != 0U ||
	    !bssid_valid(bss->bssid) ||
	    !bytes_zero(bss->reserved, sizeof(bss->reserved)))
		return EINVAL;
	expected_frequency = channel_frequency(bss->channel);
	if (bss->center_frequency_mhz != expected_frequency)
		return EINVAL;
	normalized = *bss;
	memset(normalized.ssid + normalized.ssid_length, 0,
	    WLAN_SSID_MAX - normalized.ssid_length);
	normalized.age_ms = 0U;
	error = station_enter(station);
	if (error != 0)
		return error;
#ifdef WLAN_TESTING
	{
		wlan_station_test_hook_fn hook;
		void *hook_context;

		enabled = spin_lock_irqsave(&station->lock);
		hook = station->test_report_hook;
		hook_context = station->test_report_hook_context;
		station->test_report_hook = NULL;
		station->test_report_hook_context = NULL;
		spin_unlock_irqrestore(&station->lock, enabled);
		if (hook != NULL)
			hook(hook_context);
	}
#endif
	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);
	if (station->scan_state != WLAN_SCAN_RUNNING ||
	    station->scan_generation != generation ||
	    station->scan_step_state != WLAN_SCAN_STEP_DWELL ||
	    station->scan_step_index >= station->scan_profile.channel_count ||
	    station->scan_profile.channels[station->scan_step_index].channel !=
	    normalized.channel) {
		error = ESTALE;
	} else if (deadline_expired(now, station->scan_deadline) ||
	    deadline_expired(now, station->scan_step_deadline)) {
		/* Report paths may be IRQ/USB completion context.  They never call
		 * back into a driver or join their own producer.  The network worker
		 * owns terminal timeout and synchronous stop. */
		wake_worker = 1;
		error = ETIMEDOUT;
	} else if (station->reconnect_scan_active) {
		/* Recovery deliberately revalidates the exact selected BSSID on its
		 * legal channel.  Roaming to another BSSID is a separate policy; keeping
		 * this first contract exact also lets the retained PMK/profile remain
		 * immutable across attempts. */
		if (normalized.ssid_length == station->selected.ssid_length &&
		    memcmp(normalized.ssid, station->selected.ssid,
		    normalized.ssid_length) == 0 &&
		    memcmp(normalized.bssid, station->selected.bssid, 6U) == 0) {
			station->selected = normalized;
			station->reconnect_bss_seen = 1;
		}
		error = 0;
	} else {
		error = cache_insert_locked(station, &normalized, now);
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (wake_worker)
		wlan_worker_wakeup();
	station_leave(station);
	return error;
}

int
wlan_station_report_scan_frame(struct wlan_station *station,
	uint64_t generation, const uint8_t *frame, size_t length,
	int32_t rssi_dbm, uint8_t channel_hint)
{
	struct wlan_bss_record bss;
	int error;

	if (length > WLAN_MANAGEMENT_FRAME_MAX)
		return EMSGSIZE;
	error = wlan_frame_parse_bss(frame, length, rssi_dbm, channel_hint,
	    &bss);
	if (error != 0)
		return error;
	return wlan_station_report_scan_bss(station, generation, &bss);
}

int
wlan_station_report_scan_channel_ready(struct wlan_station *station,
	uint64_t generation, uint32_t step_index)
{
	unsigned long enabled;
	int result;

	result = station_enter(station);
	if (result != 0)
		return result;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->scan_state != WLAN_SCAN_RUNNING ||
	    station->scan_generation != generation ||
	    station->scan_step_state != WLAN_SCAN_STEP_TUNING ||
	    station->scan_step_index != step_index) {
		result = ESTALE;
	} else {
		station->scan_ready_pending = 1U;
		result = 0;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	wlan_worker_wakeup();
	station_leave(station);
	return result;
}

int
wlan_station_report_scan_error(struct wlan_station *station,
	uint64_t generation, int error)
{
	unsigned long enabled;
	int result;

	if (error <= 0)
		return EINVAL;
	result = station_enter(station);
	if (result != 0)
		return result;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->scan_state != WLAN_SCAN_RUNNING ||
	    station->scan_generation != generation) {
		result = ESTALE;
	} else {
		if (station->scan_event_error == 0)
			station->scan_event_error = error;
		result = 0;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	wlan_worker_wakeup();
	station_leave(station);
	return result;
}

int
wlan_station_report_link_loss(struct wlan_station *station,
	uint64_t generation, int error)
{
	int result;

	if (generation == 0U || error <= 0)
		return EINVAL;
	result = station_enter(station);
	if (result != 0)
		return result;
	station_control_enter(station);
	result = station_link_lost_controlled(station, generation, error);
	station_control_leave(station);
	station_leave(station);
	wlan_worker_wakeup();
	return result;
}

int
wlan_station_report_frame(struct wlan_station *station,
	const struct wlan_radio_rx_frame *report)
{
	struct wlan_bss_record management_bss;
	struct wlan_l2_rx_security security;
	struct packet_buf *packet = NULL;
	uint8_t ethernet[WLAN_L2_ETHERNET_MAX];
	uint16_t frame_control;
	size_t ethernet_length = 0U;
	unsigned long enabled;
	uint64_t now;
	int result;

	if (report == NULL || report->frame == NULL || report->length < 2U ||
	    report->length > WLAN_MANAGEMENT_FRAME_MAX ||
	    report->generation == 0U || report->channel > 14U ||
	    report->cipher > WLAN_RADIO_CIPHER_CCMP ||
	    (report->decrypted != 0U && report->decrypted != 1U) ||
	    (report->integrity_error != 0U &&
	    report->integrity_error != 1U) || report->key_index > 3U ||
	    report->packet_number > 0x0000ffffffffffffULL ||
	    !bytes_zero(report->reserved, sizeof(report->reserved)))
		return EINVAL;
	frame_control = (uint16_t)((uint16_t)report->frame[0] |
	    ((uint16_t)report->frame[1] << 8));
	if ((frame_control & 0x000cU) == 0U &&
	    ((frame_control & 0x00f0U) == 0x0080U ||
	    (frame_control & 0x00f0U) == 0x0050U)) {
		result = wlan_frame_parse_bss(report->frame, report->length,
		    report->rssi_dbm, report->channel, &management_bss);
		if (result != 0)
			return result;
		if ((frame_control & 0x00f0U) == 0x0080U) {
			result = station_enter(station);
			if (result != 0)
				return result;
			enabled = spin_lock_irqsave(&station->lock);
			if (report->generation == station->connection_generation &&
			    station_beacon_watch_active_locked(station) &&
			    memcmp(management_bss.bssid, station->selected.bssid,
			    6U) == 0) {
				station_beacon_watch_refresh_locked(station,
				    station_now_locked(station));
				result = 0;
			} else {
				result = ESTALE;
			}
			spin_unlock_irqrestore(&station->lock, enabled);
			station_leave(station);
			if (result == 0)
				return 0;
		}
		return wlan_station_report_scan_bss(station,
		    report->generation, &management_bss);
	}
	result = station_enter(station);
	if (result != 0)
		return result;
	station_control_enter(station);
	enabled = spin_lock_irqsave(&station->lock);
	if (report->generation != station->connection_generation ||
	    station->state == WLAN_STATE_DOWN ||
	    station->state == WLAN_STATE_IDLE ||
	    station->state == WLAN_STATE_SCANNING ||
	    station->state == WLAN_STATE_FAILED) {
		spin_unlock_irqrestore(&station->lock, enabled);
		result = ESTALE;
		goto out;
	}
	now = station_now_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);
	if (report->integrity_error) {
		result = EACCES;
		goto out;
	}
	if ((frame_control & 0x000cU) == 0U) {
		/* PMF is outside this first profile.  Management input therefore
		 * carries neither Protected Frame nor data-key metadata. */
		if ((frame_control & 0x4000U) != 0U ||
		    report->key_generation != 0U || report->packet_number != 0U ||
		    report->cipher != WLAN_RADIO_CIPHER_NONE ||
		    report->decrypted != 0U || report->key_index != 0U) {
			result = EACCES;
			goto out;
		}
		if ((frame_control & (uint16_t)~0x0800U) == 0x00a0U ||
		    (frame_control & (uint16_t)~0x0800U) == 0x00c0U) {
			/* PMF is outside this profile, so a matching unprotected
			 * disassociation/deauthentication is authoritative.  Frames for
			 * another BSS/station are ordinary unrelated management traffic. */
			if (report->length != 26U ||
			    memcmp(report->frame + 4U, station->device->hwaddr, 6U) != 0 ||
			    memcmp(report->frame + 10U, station->selected.bssid, 6U) != 0 ||
			    memcmp(report->frame + 16U, station->selected.bssid, 6U) != 0) {
				result = ESTALE;
				goto out;
			}
			result = station_link_lost_controlled(station,
			    report->generation, ECONNRESET);
			goto out;
		}
		result = wlan_wpa2_engine_receive_management(&station->wpa2,
		    report->generation, report->frame, report->length, now);
		goto sync;
	}
	if ((frame_control & 0x000cU) != 0x0008U) {
		result = EPROTONOSUPPORT;
		goto out;
	}
	memset(&security, 0, sizeof(security));
	security.key_generation = report->key_generation;
	security.packet_number = report->packet_number;
	security.decrypted = report->decrypted;
	security.cipher_ccmp = report->cipher == WLAN_RADIO_CIPHER_CCMP;
	security.key_index = report->key_index;
	result = wlan_l2_parse_data(station->device->hwaddr,
	    station->selected.bssid, report->frame, report->length, &security,
	    &station->l2_rx, ethernet, sizeof(ethernet), &ethernet_length);
	if (result != 0)
		goto out;
	if (ethernet_length >= WLAN_L2_ETHERNET_HEADER_SIZE &&
	    ethernet[12U] == 0x88U && ethernet[13U] == 0x8eU) {
		/* Clear EAPOL is confined to the initial four-way exchange.  Once a
		 * pairwise generation has reached the connected lifetime, rekey M1/G1
		 * and every response/retry must arrive through the active CCMP domain.
		 * Otherwise an unauthenticated clear M1 could close the controlled port. */
		if (station->wpa2.reconnectable &&
		    station->wpa2.pairwise_installed &&
		    ((frame_control & 0x4000U) == 0U ||
		    report->cipher != WLAN_RADIO_CIPHER_CCMP ||
		    report->decrypted == 0U || report->key_index != 0U ||
		    report->key_generation !=
		    station->l2_rx.pairwise_key_generation)) {
			result = EACCES;
			goto out;
		}
		result = wlan_wpa2_engine_receive_eapol(&station->wpa2,
		    report->generation, ethernet + 6U, ethernet,
		    ethernet + WLAN_L2_ETHERNET_HEADER_SIZE,
		    ethernet_length - WLAN_L2_ETHERNET_HEADER_SIZE, now);
		goto sync;
	}
	if (!station->wpa2.authorized ||
	    (frame_control & 0x4000U) == 0U) {
		result = EACCES;
		goto out;
	}
	packet = packet_buf_alloc(0U);
	if (packet == NULL) {
		result = ENOBUFS;
		goto out;
	}
	if (packet_buf_append(packet, ethernet_length) == NULL) {
		result = EMSGSIZE;
		goto out;
	}
	memcpy(packet->data, ethernet, ethernet_length);
	net_device_receive(station->device, packet);
	packet = NULL;
	result = 0;
	goto out;

sync:
	enabled = spin_lock_irqsave(&station->lock);
	station_sync_wpa_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);
out:
	if (packet != NULL)
		packet_buf_free(packet);
	wlan_crypto_erase(ethernet, sizeof(ethernet));
	station_control_leave(station);
	station_leave(station);
	if (result != 0)
		wlan_worker_wakeup();
	return result;
}

int
wlan_station_report_tx_complete(struct wlan_station *station,
	uint64_t generation, uint64_t cookie, int acknowledged, int error)
{
	unsigned long enabled;
	uint64_t now;
	int result;

	if (generation == 0U || cookie == 0U || error < 0 ||
	    (acknowledged != 0 && acknowledged != 1) ||
	    (acknowledged && error != 0))
		return EINVAL;
	result = station_enter(station);
	if (result != 0)
		return result;
	station_control_enter(station);
	enabled = spin_lock_irqsave(&station->lock);
	if (generation != station->connection_generation) {
		spin_unlock_irqrestore(&station->lock, enabled);
		result = ESTALE;
		goto out;
	}
	now = station_now_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);
	if (cookie == station->wpa2.tx_cookie_active) {
		result = wlan_wpa2_engine_report_tx(&station->wpa2, generation,
		    cookie, acknowledged, error, now);
		enabled = spin_lock_irqsave(&station->lock);
		station_sync_wpa_locked(station);
		spin_unlock_irqrestore(&station->lock, enabled);
	} else if (cookie <= station->transmit_cookie &&
	    station->wpa2.authorized) {
		if (!acknowledged || error != 0)
			net_device_tx_error(station->device);
		result = 0;
	} else {
		result = ESTALE;
	}
out:
	station_control_leave(station);
	station_leave(station);
	if (result != 0)
		wlan_worker_wakeup();
	return result;
}

int
wlan_station_transmit(struct wlan_station *station,
	struct packet_buf *packet)
{
	struct wlan_radio_tx_request request;
	uint8_t mpdu[WLAN_L2_MPDU_MAX];
	unsigned long enabled;
	size_t mpdu_length = 0U;
	uint64_t now;
	int result;

	if (packet == NULL)
		return EINVAL;
	result = station_enter(station);
	if (result != 0) {
		packet_buf_free(packet);
		return result;
	}
	station_control_enter(station);
	memset(&request, 0, sizeof(request));
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->wpa2.authorized || !station->controlled_port ||
	    station->ops->frame_transmit == NULL) {
		spin_unlock_irqrestore(&station->lock, enabled);
		result = ENETDOWN;
		goto out;
	}
	if (station->transmit_packet_number >= 0x0000ffffffffffffULL ||
	    station->transmit_cookie == UINT64_MAX) {
		spin_unlock_irqrestore(&station->lock, enabled);
		result = EOVERFLOW;
		goto out;
	}
	station->transmit_packet_number++;
	station->transmit_cookie++;
	request.generation = station->connection_generation;
	request.cookie = station->transmit_cookie;
	request.key_generation = station->wpa2.key_generation;
	request.packet_number = station->transmit_packet_number;
	now = station_now_locked(station);
	request.deadline_ticks = deadline_after(now,
	    WLAN_CONNECT_TRANSITION_TICKS);
	spin_unlock_irqrestore(&station->lock, enabled);
	result = wlan_l2_build_data(station->device->hwaddr,
	    station->selected.bssid, packet->data, packet->length, 1, 0U,
	    request.packet_number, mpdu, sizeof(mpdu), &mpdu_length);
	if (result != 0)
		goto out;
	request.frame_class = WLAN_RADIO_FRAME_DATA;
	request.encrypted = 1U;
	request.key_index = 0U;
	request.frame = mpdu;
	request.length = mpdu_length;
	result = station->ops->frame_transmit(station->radio_context, &request);
out:
	wlan_crypto_erase(mpdu, sizeof(mpdu));
	packet_buf_free(packet);
	station_control_leave(station);
	station_leave(station);
	return result;
}

static void
scan_request_output_locked(struct wlan_station *station,
	struct wlan_scan_request *request)
{
	request->generation = station->scan_generation;
	request->state = station->scan_state;
	request->terminal_error = station->scan_error;
	memset(request->reserved, 0, sizeof(request->reserved));
}

static int
ioctl_scan(struct wlan_station *station, struct wlan_scan_request *request)
{
	unsigned long enabled;
	uint64_t generation;
	uint64_t deadline;
	uint64_t now;
	int wake_retry = 0;
	int wake_start = 0;
	int error;

	if (request->flags != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)) ||
	    (request->action != WLAN_SCAN_START &&
	    request->action != WLAN_SCAN_STOP))
		return EINVAL;
	station_control_enter(station);
	enabled = spin_lock_irqsave(&station->lock);
	if (station->reconnect_pending) {
		error = EBUSY;
		goto output;
	}
	if (request->action == WLAN_SCAN_START) {
		if (!station->administrative_up) {
			error = ENETDOWN;
			goto output;
		}
		if (station->scan_state == WLAN_SCAN_RUNNING) {
			error = 0;
			goto output;
		}
		if (station->connect_driver_active ||
		    (station->state != WLAN_STATE_IDLE &&
		    station->state != WLAN_STATE_FAILED)) {
			error = EBUSY;
			goto output;
		}
		if (station->scan_driver_active) {
			error = EBUSY;
			goto output;
		}
		if (station->ops->scan_channel_start == NULL) {
			error = EOPNOTSUPP;
			goto output;
		}
		now = station_now_locked(station);
		error = deadline_checked(now, WLAN_SCAN_DEADLINE_TICKS,
		    &deadline);
		if (error != 0)
			goto output;
		error = station_generation_locked(station, &generation);
		if (error != 0)
			goto output;
		station->operation_generation = generation;
		station->scan_generation = generation;
		station->scan_deadline = deadline;
		station->scan_state = WLAN_SCAN_RUNNING;
		station->scan_error = 0;
		station->scan_driver_active = 0;
		station->scan_step_index = 0U;
		station->scan_step_state = WLAN_SCAN_STEP_NEED_TUNE;
		station->scan_ready_pending = 0U;
		station->scan_publish_pending = 0U;
		station->scan_event_error = 0;
		station->scan_step_deadline = 0U;
		station->staging_count = 0U;
		station->staging_truncated = 0U;
		memset(station->staging, 0, sizeof(station->staging));
		if (station->state == WLAN_STATE_IDLE ||
		    station->state == WLAN_STATE_FAILED)
			station->state = WLAN_STATE_SCANNING;
		wake_start = 1;
		error = 0;
		goto output;
	}
	if (station->scan_state != WLAN_SCAN_RUNNING &&
	    !station->scan_driver_active) {
		error = 0;
		goto output;
	}
	generation = station->scan_generation;
	if (station->scan_state == WLAN_SCAN_RUNNING) {
		station->scan_state = WLAN_SCAN_CANCELLED;
		station->scan_error = ECANCELED;
	}
	station->scan_step_state = WLAN_SCAN_STEP_NONE;
	station->scan_ready_pending = 0U;
	station->scan_publish_pending = 0U;
	station->scan_event_error = 0;
	station->scan_step_deadline = 0U;
	if (station->state == WLAN_STATE_SCANNING)
		station->state = WLAN_STATE_IDLE;
	if (!station->scan_driver_active || station->ops->scan_stop == NULL) {
		station->scan_driver_active = 0;
		error = 0;
		goto output;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	error = station->ops->scan_stop(station->radio_context, generation);
	enabled = spin_lock_irqsave(&station->lock);
	if (station->scan_generation == generation) {
		if (error == 0) {
			station->scan_driver_active = 0;
			station->scan_retry_deadline = 0U;
		} else {
			station->scan_state = WLAN_SCAN_FAILED;
			station->scan_error = error;
			station->scan_retry_deadline = deadline_after(
			    station_now_locked(station), 1U);
			wake_retry = 1;
		}
	}
output:
	scan_request_output_locked(station, request);
	spin_unlock_irqrestore(&station->lock, enabled);
	station_control_leave(station);
	if (wake_start || wake_retry)
		wlan_worker_wakeup();
	return error;
}

static int
ioctl_scan_status(struct wlan_station *station,
	struct wlan_scan_status_request *request)
{
	unsigned long enabled;

	if (!bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	request->generation = station->snapshot_generation;
	request->scan_generation = station->scan_generation;
	request->cache_sequence = station->cache_sequence;
	request->deadline_ticks = station->scan_state == WLAN_SCAN_RUNNING ?
	    station->scan_deadline : 0U;
	request->state = station->scan_state;
	request->terminal_error = station->scan_error;
	request->result_count = station->snapshot_count;
	request->truncated = station->snapshot_truncated;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

static uint32_t
entry_age_ms(uint64_t now, uint64_t last_seen)
{
	uint64_t ticks = now >= last_seen ? now - last_seen : 0U;

	if (ticks > (uint64_t)UINT32_MAX / 10U)
		return UINT32_MAX;
	return (uint32_t)(ticks * 10U);
}

static int
ioctl_bss(struct wlan_station *station, struct wlan_bss_request *request)
{
	unsigned long enabled;

	if (request->reserved0 != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	if (request->generation != station->snapshot_generation) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}
	if (request->index >= station->snapshot_count) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOENT;
	}
	request->bss = station->snapshot[request->index].bss;
	request->bss.age_ms = entry_age_ms(station_now_locked(station),
	    station->snapshot[request->index].last_seen);
	memset(request->bss.reserved, 0, sizeof(request->bss.reserved));
	request->reserved0 = 0U;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

static int
ioctl_connect(struct wlan_station *station, struct wlan_connect_request *request)
{
	static const uint8_t supported_rates[12] = {
		0x82U, 0x84U, 0x8bU, 0x96U,
		0x0cU, 0x12U, 0x18U, 0x24U,
		0x30U, 0x48U, 0x60U, 0x6cU
	};
	uint8_t credential[WLAN_PASSPHRASE_STORAGE];
	struct wlan_bss_record selected;
	struct wlan_wpa2_profile profile;
	unsigned long enabled;
	uint64_t generation = 0U;
	uint64_t deadline = 0U;
	int control_entered = 0;
	int error;

	memcpy(credential, request->passphrase, sizeof(credential));
	secure_zero(request->passphrase, sizeof(request->passphrase));
	if (request->ssid_length > WLAN_SSID_MAX ||
	    request->passphrase_length < WLAN_PASSPHRASE_MIN ||
	    request->passphrase_length > WLAN_PASSPHRASE_MAX ||
	    !bytes_zero(request->reserved, sizeof(request->reserved))) {
		error = EINVAL;
		goto done;
	}
	station_control_enter(station);
	control_entered = 1;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->administrative_up) {
		error = ENETDOWN;
		goto output_locked;
	}
	if (station->ops->connect_start == NULL) {
		error = EOPNOTSUPP;
		goto output_locked;
	}
	if (station->reconnect_pending) {
		spin_unlock_irqrestore(&station->lock, enabled);
		error = station_retire_controlled(station, 1);
		enabled = spin_lock_irqsave(&station->lock);
		if (error != 0)
			goto output_locked;
	}
	if (station->scan_driver_active || station->connect_driver_active ||
	    station->scan_state == WLAN_SCAN_RUNNING ||
	    (station->state != WLAN_STATE_IDLE &&
	    station->state != WLAN_STATE_FAILED)) {
		error = EBUSY;
		goto output_locked;
	}
	error = station_select_bss_locked(station, request->ssid,
	    request->ssid_length, &selected);
	if (error != 0)
		goto output_locked;
	error = deadline_checked(station_now_locked(station),
	    WLAN_CONNECT_DEADLINE_TICKS, &deadline);
	if (error != 0)
		goto output_locked;
	error = station_carrier_down_locked(station);
	if (error != 0)
		goto output_locked;
	error = station_generation_locked(station, &generation);
	if (error != 0)
		goto output_locked;
	if (wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_IDLE) {
		spin_unlock_irqrestore(&station->lock, enabled);
		error = wlan_wpa2_engine_stop(&station->wpa2);
		enabled = spin_lock_irqsave(&station->lock);
		if (error != 0)
			goto output_locked;
	}
	station_clear_connection_locked(station);
	station->selected = selected;
	station->operation_generation = generation;
	station->connection_generation = generation;
	station->connection_deadline = deadline;
	station->state = WLAN_STATE_AUTHENTICATING;
	station->terminal_error = 0;
	station->connect_driver_active = 0;
	station->connect_stop_pending = 0;
	station->connect_retire_explicit = 0;
	station->connect_retry_deadline = 0U;
	station->transmit_packet_number = 0U;
	station->transmit_cookie = 0U;
	memset(&station->l2_rx, 0, sizeof(station->l2_rx));
	spin_unlock_irqrestore(&station->lock, enabled);
	memset(&profile, 0, sizeof(profile));
	memcpy(profile.station, station->device->hwaddr,
	    sizeof(profile.station));
	memcpy(profile.bssid, selected.bssid, sizeof(profile.bssid));
	memcpy(profile.ssid, selected.ssid, selected.ssid_length);
	profile.ssid_length = selected.ssid_length;
	memcpy(profile.rates, supported_rates, sizeof(supported_rates));
	profile.rate_count = sizeof(supported_rates);
	profile.channel = selected.channel;
	/* Association capability is our implemented local feature set.  AP
	 * optional claims from its beacon are input to compatibility selection,
	 * never capabilities that this station may echo as its own. */
	profile.capability = WLAN_LOCAL_ASSOC_CAPABILITY;
	profile.listen_interval = 1U;
	profile.initial_sequence = 0U;
	profile.passphrase = credential;
	profile.passphrase_length = request->passphrase_length;
	profile.total_deadline_ticks = deadline;
	profile.transition_timeout_ticks = WLAN_CONNECT_TRANSITION_TICKS;
	profile.recovery_timeout_ticks = WLAN_CONNECT_DEADLINE_TICKS;
	error = wlan_wpa2_engine_start(&station->wpa2, generation, &profile,
	    station->clock(station->clock_context));
	wlan_crypto_erase(&profile, sizeof(profile));
	enabled = spin_lock_irqsave(&station->lock);
	station_sync_wpa_locked(station);
output_locked:
	request->generation = station->connection_generation;
	request->state = station->state;
	request->terminal_error = station->terminal_error;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
done:
	if (control_entered)
		station_control_leave(station);
	wlan_worker_wakeup();
	secure_zero(credential, sizeof(credential));
	secure_zero(request->passphrase, sizeof(request->passphrase));
	request->passphrase_length = 0U;
	return error;
}

static int
station_retire_controlled(struct wlan_station *station,
	int keep_administrative_up)
{
	unsigned long enabled;
	uint64_t scan_generation;
	uint64_t connection_generation;
	int stop_scan;
	int stop_connection;
	int connection_still_active;
	int engine_stop_needed;
	int carrier_error;
	int scan_error = 0;
	int connection_error = 0;
	int error;

	enabled = spin_lock_irqsave(&station->lock);
	if (!keep_administrative_up)
		station->administrative_up = 0U;
	/* Explicit policy/lifecycle operations cancel recovery before crossing
	 * any driver barrier.  The following engine_stop() erases the retained
	 * PMK, so no later worker tick can resurrect this desired connection. */
	station_cancel_reconnect_locked(station);
	carrier_error = station_carrier_down_locked(station);
	scan_generation = station->scan_generation;
	connection_generation = station->connection_generation;
	stop_scan = station->scan_driver_active;
	stop_connection = station->connect_driver_active;
	engine_stop_needed = wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_IDLE;
	station->connect_retire_explicit = 1;
	station->connect_stop_pending = 0;
	station->connect_retry_deadline = 0U;
	if (station->scan_state == WLAN_SCAN_RUNNING) {
		station->scan_state = WLAN_SCAN_CANCELLED;
		station->scan_error = ECANCELED;
	}
	station->scan_step_state = WLAN_SCAN_STEP_NONE;
	station->scan_ready_pending = 0U;
	station->scan_publish_pending = 0U;
	station->scan_event_error = 0;
	station->scan_step_deadline = 0U;
	station->state = station->administrative_up ?
	    WLAN_STATE_DISCONNECTING : WLAN_STATE_DOWN;
	station->terminal_error = 0;
#ifdef WLAN_TESTING
	station->test_report_hook = NULL;
	station->test_report_hook_context = NULL;
#endif
	spin_unlock_irqrestore(&station->lock, enabled);

	if (stop_scan) {
		if (station->ops->scan_stop == NULL)
			scan_error = EOPNOTSUPP;
		else
			scan_error = station->ops->scan_stop(
			    station->radio_context, scan_generation);
	}
	if (engine_stop_needed)
		connection_error = wlan_wpa2_engine_stop(&station->wpa2);
	enabled = spin_lock_irqsave(&station->lock);
	connection_still_active = station->connect_driver_active;
	spin_unlock_irqrestore(&station->lock, enabled);
	/* engine_stop() owns the key/association/radio ordering.  A direct radio
	 * stop is only a fallback for an otherwise-idle engine; it must never run
	 * past an uncertain key-delete barrier. */
	if (connection_error == 0 && stop_connection &&
	    connection_still_active) {
		if (station->ops->disconnect == NULL)
			error = EOPNOTSUPP;
		else
			error = station->ops->disconnect(station->radio_context,
			    connection_generation);
		if (error == 0) {
			enabled = spin_lock_irqsave(&station->lock);
			station->connect_driver_active = 0;
			spin_unlock_irqrestore(&station->lock, enabled);
		}
		if (connection_error == 0)
			connection_error = error;
	}
	enabled = spin_lock_irqsave(&station->lock);
	if (scan_error == 0)
		station->scan_driver_active = 0;
	else {
		station->scan_state = WLAN_SCAN_FAILED;
		station->scan_error = scan_error;
	}
	if (connection_error == 0 && !station->connect_driver_active)
		station_finish_connection_retire_locked(station);
	else {
		station->connect_stop_pending = 1;
		station->connect_retry_deadline = deadline_after(
		    station_now_locked(station), 1U);
	}
	station->terminal_error = scan_error != 0 ? scan_error :
	    (connection_error != 0 ? connection_error : carrier_error);
	if (station->terminal_error != 0)
		station->state = station->administrative_up ?
		    WLAN_STATE_FAILED : WLAN_STATE_DOWN;
	if (station->scan_driver_active && scan_error != 0)
		station->scan_retry_deadline = deadline_after(
		    station_now_locked(station), 1U);
	else if (!station->scan_driver_active)
		station->scan_retry_deadline = 0U;
	if (connection_error == 0 && !station->connect_driver_active)
		station->connect_retry_deadline = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
	if (scan_error != 0 || connection_error != 0 || carrier_error != 0)
		wlan_worker_wakeup();
	return scan_error != 0 ? scan_error :
	    (connection_error != 0 ? connection_error : carrier_error);
}

static int
station_retire(struct wlan_station *station, int keep_administrative_up)
{
	int error;

	station_control_enter(station);
	error = station_retire_controlled(station, keep_administrative_up);
	station_control_leave(station);
	return error;
}

static int
ioctl_disconnect(struct wlan_station *station,
	struct wlan_disconnect_request *request)
{
	unsigned long enabled;
	uint64_t generation;
	int error;

	if (request->flags != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;
	station_control_enter(station);
	enabled = spin_lock_irqsave(&station->lock);
	error = station_generation_locked(station, &generation);
	if (error != 0) {
		request->generation = station->operation_generation;
		request->state = station->state;
		request->terminal_error = error;
		memset(request->reserved, 0, sizeof(request->reserved));
		spin_unlock_irqrestore(&station->lock, enabled);
		station_control_leave(station);
		return error;
	}
	station->operation_generation = generation;
	station->state = WLAN_STATE_DISCONNECTING;
	spin_unlock_irqrestore(&station->lock, enabled);
	error = station_retire_controlled(station, 1);
	enabled = spin_lock_irqsave(&station->lock);
	if (station->administrative_up)
		station->state = error == 0 ? WLAN_STATE_IDLE : WLAN_STATE_FAILED;
	request->generation = generation;
	request->state = station->state;
	request->terminal_error = error;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
	station_control_leave(station);
	return error;
}

static int
ioctl_status(struct wlan_station *station, struct wlan_status_request *request)
{
	unsigned long enabled;

	if (request->reserved0 != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	request->operation_generation = station->operation_generation;
	request->scan_generation = station->scan_generation;
	request->snapshot_generation = station->snapshot_generation;
	if (station->state == WLAN_STATE_AUTHENTICATING ||
	    station->state == WLAN_STATE_ASSOCIATING ||
	    station->state == WLAN_STATE_FOUR_WAY)
		request->deadline_ticks = station->connection_deadline;
	else if (station->scan_state == WLAN_SCAN_RUNNING)
		request->deadline_ticks = station->scan_deadline;
	else
		request->deadline_ticks = 0U;
	request->cache_sequence = station->cache_sequence;
	request->state = station->state;
	request->scan_state = station->scan_state;
	request->administrative_up = station->administrative_up;
	request->authenticated = station->authenticated;
	request->associated = station->associated;
	request->key_installed = station->key_installed;
	request->controlled_port = station->controlled_port;
	request->retry_count = station->retry_count;
	request->terminal_error = station->terminal_error != 0 ?
	    station->terminal_error :
	    (station->scan_state == WLAN_SCAN_FAILED ? station->scan_error : 0);
	request->rssi_dbm = station->selected.rssi_dbm;
	memcpy(request->bssid, station->selected.bssid,
	    sizeof(request->bssid));
	request->channel = station->selected.channel;
	request->reserved0 = 0U;
	request->center_frequency_mhz =
	    station->selected.center_frequency_mhz;
	request->security = station->selected.security;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

int
wlan_station_ioctl(struct net_device *device, unsigned long request,
	void *argument)
{
	struct wlan_station *station;
	struct wlan_ioctl_header *header;
	size_t expected_size;
	int error;

	if (argument == NULL)
		return EFAULT;
	if (atomic_load_acquire(&wlan_initialized) != 2U)
		wlan_core_init();
	if (request == SIOCSWLANCONNECT) {
		struct wlan_connect_request *connect = argument;
		uint8_t saved[WLAN_PASSPHRASE_STORAGE];

		/* Preserve the input only across header/device validation, and make
		 * every recognized CONNECT return path visibly redacted. */
		memcpy(saved, connect->passphrase, sizeof(saved));
		secure_zero(connect->passphrase, sizeof(connect->passphrase));
		error = header_validate(device,
		    (const struct wlan_ioctl_header *)connect, sizeof(*connect));
		if (error != 0) {
			secure_zero(saved, sizeof(saved));
			connect->passphrase_length = 0U;
			return error;
		}
		error = station_find_enter(device, &station);
		if (error == 0) {
			memcpy(connect->passphrase, saved, sizeof(saved));
			error = ioctl_connect(station, connect);
			station_leave(station);
		}
		secure_zero(saved, sizeof(saved));
		secure_zero(connect->passphrase, sizeof(connect->passphrase));
		connect->passphrase_length = 0U;
		return error;
	}
	if (request == SIOCSWLANSCAN)
		expected_size = sizeof(struct wlan_scan_request);
	else if (request == SIOCGWLANSCAN)
		expected_size = sizeof(struct wlan_scan_status_request);
	else if (request == SIOCGWLANBSS)
		expected_size = sizeof(struct wlan_bss_request);
	else if (request == SIOCSWLANDISCONNECT)
		expected_size = sizeof(struct wlan_disconnect_request);
	else if (request == SIOCGWLANSTATUS)
		expected_size = sizeof(struct wlan_status_request);
	else
		return ENOTTY;
	header = argument;
	error = header_validate(device, header, expected_size);
	if (error != 0)
		return error;
	error = station_find_enter(device, &station);
	if (error != 0)
		return error;
	if (request == SIOCSWLANSCAN)
		error = ioctl_scan(station, argument);
	else if (request == SIOCGWLANSCAN)
		error = ioctl_scan_status(station, argument);
	else if (request == SIOCGWLANBSS)
		error = ioctl_bss(station, argument);
	else if (request == SIOCSWLANDISCONNECT)
		error = ioctl_disconnect(station, argument);
	else
		error = ioctl_status(station, argument);
	station_leave(station);
	return error;
}

int
wlan_station_close(struct wlan_station *station)
{
	unsigned long enabled;
	int error;

	if (station == NULL)
		return ENODEV;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used || station->blocked) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENODEV;
	}
	if (station->lifecycle_inflight) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return EBUSY;
	}
	station->closing = 1;
	if (station->active != 0U) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return EBUSY;
	}
	station->lifecycle_inflight = 1;
	spin_unlock_irqrestore(&station->lock, enabled);
	error = station_retire(station, 0);
	enabled = spin_lock_irqsave(&station->lock);
	station->lifecycle_inflight = 0;
	if (error == 0)
		station->closing = 0;
	spin_unlock_irqrestore(&station->lock, enabled);
	return error;
}

static struct net_device *
station_finalize_locked(struct wlan_station *station)
{
	struct net_device *device = station->device;

	station->state = WLAN_STATE_REMOVED;
	station_clear_connection_locked(station);
	secure_zero(station->staging, sizeof(station->staging));
	secure_zero(station->snapshot, sizeof(station->snapshot));
	secure_zero(&station->scan_profile, sizeof(station->scan_profile));
	station->staging_count = 0U;
	station->snapshot_count = 0U;
	station->device = NULL;
	station->ops = NULL;
	station->radio_context = NULL;
	station->clock = NULL;
	station->clock_context = NULL;
	station->lifecycle_inflight = 0;
	station->used = 0;
	return device;
}

int
wlan_station_detach(struct wlan_station *station)
{
	unsigned long registry_enabled;
	unsigned long enabled;
	struct net_device *release_device;
	int error;

	if (station == NULL || atomic_load_acquire(&wlan_initialized) != 2U)
		return ENODEV;
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used) {
		spin_unlock_irqrestore(&station->lock, enabled);
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return ENODEV;
	}
	if (station->shutdown_owned || station->lifecycle_inflight ||
	    station->closing) {
		spin_unlock_irqrestore(&station->lock, enabled);
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return EBUSY;
	}
	if (station->active != 0U) {
		station->blocked = 1;
		spin_unlock_irqrestore(&station->lock, enabled);
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return EBUSY;
	}
	station->blocked = 1;
	station->lifecycle_inflight = 1;
	spin_unlock_irqrestore(&station->lock, enabled);
	spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);

	station_control_enter(station);
	error = station_retire_controlled(station, 0);
	if (error == 0 && station->ops->quiesce != NULL)
		error = station->ops->quiesce(station->radio_context);
	station_control_leave(station);
	if (error != 0) {
		enabled = spin_lock_irqsave(&station->lock);
		station->lifecycle_inflight = 0;
		spin_unlock_irqrestore(&station->lock, enabled);
		return error;
	}
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	enabled = spin_lock_irqsave(&station->lock);
	if (station->active != 0U || station->shutdown_owned) {
		station->lifecycle_inflight = 0;
		spin_unlock_irqrestore(&station->lock, enabled);
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return EBUSY;
	}
	release_device = station_finalize_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);
	spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
	net_device_release(release_device);
	return 0;
}

int
wlan_station_shutdown_all(void)
{
	unsigned long registry_enabled;
	unsigned index;
	int first_error = 0;
	int busy = 0;

	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return 0;
	/* The terminal registry latch prevents attachment and slot reuse between
	 * the admission-closing pass and the checked retirement pass. */
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	if (wlan_shutdown_inflight) {
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return EBUSY;
	}
	wlan_shutdown_inflight = 1;
	wlan_stopping = 1;
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		struct wlan_station *station = &wlan_stations[index];
		unsigned long enabled;

		if (!station->used)
			continue;
		enabled = spin_lock_irqsave(&station->lock);
		if (station->lifecycle_inflight) {
			busy = 1;
		} else {
			station->shutdown_owned = 1;
			station->blocked = 1;
			station->closing = 0;
			if (station->active != 0U)
				busy = 1;
		}
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
	if (busy) {
		registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
		wlan_shutdown_inflight = 0;
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return EBUSY;
	}
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		struct wlan_station *station = &wlan_stations[index];
		unsigned long enabled;
		struct net_device *release_device = NULL;
		int owned;
		int error;

		enabled = spin_lock_irqsave(&station->lock);
		owned = station->used && station->shutdown_owned;
		spin_unlock_irqrestore(&station->lock, enabled);
		if (!owned)
			continue;
		station_control_enter(station);
		error = station_retire_controlled(station, 0);
		if (error == 0 && station->ops->quiesce != NULL)
			error = station->ops->quiesce(station->radio_context);
		station_control_leave(station);
		if (error != 0) {
			if (first_error == 0)
				first_error = error;
			continue;
		}
		registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
		enabled = spin_lock_irqsave(&station->lock);
		if (station->active != 0U || !station->shutdown_owned) {
			if (first_error == 0)
				first_error = EBUSY;
		} else {
			release_device = station_finalize_locked(station);
		}
		spin_unlock_irqrestore(&station->lock, enabled);
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		if (release_device != NULL)
			net_device_release(release_device);
	}
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	wlan_shutdown_inflight = 0;
	spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
	return first_error;
}

static void
station_scan_failed_locked(struct wlan_station *station, int error)
{
	station->scan_state = WLAN_SCAN_FAILED;
	station->scan_error = error;
	station->scan_step_state = WLAN_SCAN_STEP_NONE;
	station->scan_step_deadline = 0U;
	station->scan_ready_pending = 0U;
	station->scan_publish_pending = 0U;
	station->scan_event_error = 0;
	if (station->state == WLAN_STATE_SCANNING)
		station->state = WLAN_STATE_IDLE;
}

static int
station_scan_publish_locked(struct wlan_station *station,
	uint64_t generation)
{
	if (station->cache_sequence == UINT64_MAX) {
		station_scan_failed_locked(station, EOVERFLOW);
		return EOVERFLOW;
	}
	memcpy(station->snapshot, station->staging,
	    sizeof(station->snapshot));
	station->snapshot_count = station->staging_count;
	station->snapshot_truncated = station->staging_truncated;
	cache_sort_by_bssid(station->snapshot, station->snapshot_count);
	station->snapshot_generation = generation;
	station->cache_sequence++;
	station->scan_state = WLAN_SCAN_COMPLETE;
	station->scan_error = 0;
	station->scan_step_state = WLAN_SCAN_STEP_NONE;
	station->scan_step_deadline = 0U;
	station->scan_ready_pending = 0U;
	station->scan_publish_pending = 0U;
	station->scan_event_error = 0;
	if (station->state == WLAN_STATE_SCANNING)
		station->state = WLAN_STATE_IDLE;
	return 0;
}

static void
station_scan_stop_result(struct wlan_station *station, uint64_t generation,
	int error)
{
	unsigned long enabled = spin_lock_irqsave(&station->lock);
	uint64_t now = station_now_locked(station);

	if (station->scan_generation == generation) {
		if (station->reconnect_scan_active &&
		    station->reconnect_pending) {
			if (error == 0) {
				int found = station->scan_error == 0 &&
				    station->reconnect_bss_seen;

				station->scan_driver_active = 0;
				station->scan_retry_deadline = 0U;
				station->scan_state = WLAN_SCAN_IDLE;
				station->scan_error = found ? 0 : ENOENT;
				station->scan_step_state = WLAN_SCAN_STEP_NONE;
				station->scan_step_deadline = 0U;
				station->scan_ready_pending = 0U;
				station->scan_publish_pending = 0U;
				station->scan_event_error = 0;
				station->reconnect_scan_active = 0;
				station->reconnect_scan_ready = found;
				station->reconnect_bss_seen = 0;
				if (found)
					station->reconnect_next_attempt = 0U;
				else {
					station->terminal_error = ENOENT;
					station_reconnect_schedule_locked(station, now);
				}
			} else {
				station->scan_retry_deadline = deadline_after(now, 1U);
			}
			spin_unlock_irqrestore(&station->lock, enabled);
			wlan_worker_wakeup();
			return;
		}
		if (error == 0) {
			station->scan_driver_active = 0;
			if (station->scan_publish_pending) {
				if (deadline_expired(now, station->scan_deadline))
					station_scan_failed_locked(station,
					    ETIMEDOUT);
				else
					(void)station_scan_publish_locked(station,
					    generation);
			}
			station->scan_retry_deadline = 0U;
		} else {
			if (station->scan_publish_pending)
				station_scan_failed_locked(station, error);
			station->scan_retry_deadline = deadline_after(now, 1U);
		}
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (error != 0)
		wlan_worker_wakeup();
}

static void
station_reconnect_schedule_locked(struct wlan_station *station, uint64_t now)
{
	uint64_t delay;
	uint64_t target;

	if (!station->reconnect_pending)
		return;
	station->retry_count = station->reconnect_attempts;
	if (station->reconnect_attempts >= WLAN_RECONNECT_ATTEMPT_MAX ||
	    deadline_expired(now, station->reconnect_deadline)) {
		station->reconnect_next_attempt = now;
		return;
	}
	delay = wlan_reconnect_delay_ticks[station->reconnect_attempts];
	target = UINT64_MAX - now < delay ? UINT64_MAX : now + delay;
	if (target > station->reconnect_deadline)
		target = station->reconnect_deadline;
	station->reconnect_next_attempt = target;
}

static int
station_reconnect_scan_start_locked(struct wlan_station *station, uint64_t now)
{
	uint64_t generation;
	uint64_t deadline;
	uint32_t index;
	int error;

	if (wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_RECONNECT_WAIT)
		return EBUSY;
	if (station->ops->scan_channel_start == NULL ||
	    station->ops->scan_stop == NULL)
		return EOPNOTSUPP;
	for (index = 0U; index < station->scan_profile.channel_count; index++) {
		if (station->scan_profile.channels[index].channel ==
		    station->selected.channel)
			break;
	}
	if (index == station->scan_profile.channel_count)
		return EINVAL;
	error = station_generation_locked(station, &generation);
	if (error != 0)
		return error;
	deadline = deadline_local(now, WLAN_RECONNECT_SCAN_DEADLINE_TICKS,
	    station->reconnect_deadline);
	station->scan_generation = generation;
	station->scan_deadline = deadline;
	station->scan_state = WLAN_SCAN_RUNNING;
	station->scan_error = 0;
	station->scan_driver_active = 0;
	station->scan_step_index = index;
	station->scan_step_state = WLAN_SCAN_STEP_NEED_TUNE;
	station->scan_ready_pending = 0U;
	station->scan_publish_pending = 0U;
	station->scan_event_error = 0;
	station->scan_step_deadline = 0U;
	station->scan_retry_deadline = 0U;
	station->reconnect_scan_active = 1;
	station->reconnect_scan_ready = 0;
	station->reconnect_bss_seen = 0;
	station->reconnect_next_attempt = 0U;
	return 0;
}

static void
station_connection_timer(struct wlan_station *station, uint64_t now)
{
	unsigned long enabled;
	uint64_t generation = 0U;
	uint64_t reconnect_deadline = 0U;
	enum wlan_wpa2_state wpa_state;
	uint32_t completed_attempts = 0U;
	int reconnect_cleanup = 0;
	int cleanup_due = 0;
	int reconnect = 0;
	int reconnect_exhausted = 0;
	int stop = 0;
	int error = 0;

	wpa_state = wlan_wpa2_engine_state(&station->wpa2);
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->reconnect_pending &&
	    deadline_expired(station_now_locked(station),
	    station->beacon_watch_deadline) &&
	    wlan_wpa2_engine_can_reconnect(&station->wpa2)) {
		generation = station->connection_generation;
		station->beacon_watch_deadline = 0U;
		spin_unlock_irqrestore(&station->lock, enabled);
		error = station_link_lost_controlled(station, generation,
		    ETIMEDOUT);
		wlan_worker_wakeup();
		(void)error;
		return;
	}
	if (station->reconnect_pending) {
		now = station_now_locked(station);
		if (station->reconnect_scan_active) {
			spin_unlock_irqrestore(&station->lock, enabled);
			return;
		}
		if (wpa_state == WLAN_WPA2_STATE_FAILED) {
			if (station->reconnect_attempts >= WLAN_RECONNECT_ATTEMPT_MAX ||
			    deadline_expired(now, station->reconnect_deadline)) {
				/* The common exhaustion path below owns final checked stop. */
			} else if (station->reconnect_cleanup_retry == 0U ||
			    now >= station->reconnect_cleanup_retry) {
				reconnect_cleanup = 1;
			}
			if (reconnect_cleanup) {
				int reason = station->terminal_error != 0 ?
				    station->terminal_error : ENETDOWN;

				spin_unlock_irqrestore(&station->lock, enabled);
				error = wlan_wpa2_engine_link_lost(&station->wpa2,
				    reason);
				enabled = spin_lock_irqsave(&station->lock);
				station_sync_wpa_locked(station);
				now = station_now_locked(station);
				if (station->reconnect_pending) {
					if (error == 0) {
						station->reconnect_cleanup_retry = 0U;
						station_reconnect_schedule_locked(station,
						    now);
					} else {
						station->reconnect_cleanup_retry =
						    deadline_after(now, 1U);
					}
				}
				spin_unlock_irqrestore(&station->lock, enabled);
				wlan_worker_wakeup();
				return;
			}
			if (station->reconnect_attempts < WLAN_RECONNECT_ATTEMPT_MAX &&
			    !deadline_expired(now, station->reconnect_deadline)) {
				spin_unlock_irqrestore(&station->lock, enabled);
				return;
			}
		}
		if (wpa_state != WLAN_WPA2_STATE_FAILED &&
		    wpa_state != WLAN_WPA2_STATE_RECONNECT_WAIT &&
		    wpa_state != WLAN_WPA2_STATE_IDLE) {
			spin_unlock_irqrestore(&station->lock, enabled);
			error = wlan_wpa2_engine_timer(&station->wpa2, now);
			enabled = spin_lock_irqsave(&station->lock);
			station_sync_wpa_locked(station);
			if (station->reconnect_pending &&
			    wlan_wpa2_engine_state(&station->wpa2) ==
			    WLAN_WPA2_STATE_FAILED)
				station->reconnect_cleanup_retry =
				    deadline_after(station_now_locked(station), 1U);
			spin_unlock_irqrestore(&station->lock, enabled);
			if (error != 0)
				wlan_worker_wakeup();
			return;
		}
		if (station->reconnect_next_attempt == 0U)
			station_reconnect_schedule_locked(station, now);
		if (wpa_state == WLAN_WPA2_STATE_IDLE ||
		    station->reconnect_attempts >= WLAN_RECONNECT_ATTEMPT_MAX ||
		    deadline_expired(now, station->reconnect_deadline)) {
			completed_attempts = station->reconnect_attempts;
			station_cancel_reconnect_locked(station);
			station->connection_deadline = 0U;
			station->connection_step_deadline = 0U;
			station->retry_count = completed_attempts;
			station->state = WLAN_STATE_FAILED;
			if (station->terminal_error == 0)
				station->terminal_error = ETIMEDOUT;
			reconnect_exhausted = 1;
		} else if (station->reconnect_scan_ready) {
			error = station_generation_locked(station, &generation);
			if (error == 0) {
				station->reconnect_generation = generation;
				station->reconnect_scan_ready = 0;
				station->reconnect_next_attempt = 0U;
				reconnect_deadline = station->reconnect_deadline;
				station->connection_deadline = reconnect_deadline;
				station->state = WLAN_STATE_AUTHENTICATING;
				reconnect = 1;
			} else {
				completed_attempts = station->reconnect_attempts;
				station_cancel_reconnect_locked(station);
				station->retry_count = completed_attempts;
				station->state = WLAN_STATE_FAILED;
				station->terminal_error = error;
				reconnect_exhausted = 1;
			}
		} else if (now >= station->reconnect_next_attempt) {
			station->reconnect_attempts++;
			station->retry_count = station->reconnect_attempts;
			error = station_reconnect_scan_start_locked(station, now);
			if (error != 0) {
				station->terminal_error = error;
				station_reconnect_schedule_locked(station, now);
			}
		}
		spin_unlock_irqrestore(&station->lock, enabled);
		if (reconnect_exhausted) {
			int stop_error = wlan_wpa2_engine_stop(&station->wpa2);

			enabled = spin_lock_irqsave(&station->lock);
			station->connect_retire_explicit = 0;
			if (stop_error != 0) {
				station->connect_stop_pending = 1;
				station->connect_retry_deadline = deadline_after(
				    station_now_locked(station), 1U);
			} else if (!station->connect_driver_active)
				station_finish_connection_retire_locked(station);
			spin_unlock_irqrestore(&station->lock, enabled);
			wlan_worker_wakeup();
			return;
		}
		if (reconnect) {
			error = wlan_wpa2_engine_reconnect(&station->wpa2, generation,
			    reconnect_deadline, now);
			enabled = spin_lock_irqsave(&station->lock);
			if (station->reconnect_generation == generation)
				station->reconnect_generation = 0U;
			station_sync_wpa_locked(station);
			if (station->reconnect_pending && error != 0)
				station->reconnect_cleanup_retry =
				    deadline_after(station_now_locked(station), 1U);
			spin_unlock_irqrestore(&station->lock, enabled);
			if (error != 0)
				wlan_worker_wakeup();
			return;
		}
		return;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (wpa_state != WLAN_WPA2_STATE_IDLE &&
	    wpa_state != WLAN_WPA2_STATE_FAILED) {
		error = wlan_wpa2_engine_timer(&station->wpa2, now);
		enabled = spin_lock_irqsave(&station->lock);
		station_sync_wpa_locked(station);
		spin_unlock_irqrestore(&station->lock, enabled);
		if (error != 0)
			wlan_worker_wakeup();
		return;
	}
	if (wpa_state == WLAN_WPA2_STATE_FAILED) {
		enabled = spin_lock_irqsave(&station->lock);
		now = station_now_locked(station);
		cleanup_due = station->connect_stop_pending &&
		    (station->connect_retry_deadline == 0U ||
		    deadline_expired(now, station->connect_retry_deadline));
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	if (wpa_state == WLAN_WPA2_STATE_FAILED && cleanup_due &&
	    (station->wpa2.configured || station->wpa2.associated ||
	    station->wpa2.pairwise_installed ||
	    station->wpa2.group_installed || station->wpa2.authorized)) {
		error = wlan_wpa2_engine_stop(&station->wpa2);
		enabled = spin_lock_irqsave(&station->lock);
		if (error == 0) {
			if (!station->connect_driver_active)
				station_finish_connection_retire_locked(station);
			else {
				station->connect_stop_pending = 1;
				station->connect_retry_deadline = 0U;
			}
		} else {
			station->connect_stop_pending = 1;
			station->connect_retry_deadline = deadline_after(
			    station_now_locked(station), 1U);
		}
		spin_unlock_irqrestore(&station->lock, enabled);
		if (error != 0)
			wlan_worker_wakeup();
		return;
	}
	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);
	if (station->connect_stop_pending &&
	    (station->connect_retry_deadline == 0U ||
	    deadline_expired(now, station->connect_retry_deadline)) &&
	    station->connect_driver_active) {
		generation = station->connection_generation;
		stop = 1;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (stop)
		error = station->ops->disconnect(station->radio_context,
		    generation);
	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);
	if (stop && station->connection_generation == generation) {
		if (error == 0) {
			station->connect_driver_active = 0;
			if (wlan_wpa2_engine_state(&station->wpa2) ==
			    WLAN_WPA2_STATE_IDLE)
				station_finish_connection_retire_locked(station);
		} else {
			station->connect_stop_pending = 1;
			station->connect_retry_deadline = deadline_after(now, 1U);
		}
	}
	if (!station->connect_driver_active &&
	    wlan_wpa2_engine_state(&station->wpa2) == WLAN_WPA2_STATE_IDLE &&
	    station->connect_stop_pending)
		station_finish_connection_retire_locked(station);
	else if (!station->connect_driver_active &&
	    !station->connect_stop_pending)
		station->connect_retry_deadline = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
	if (error != 0)
		wlan_worker_wakeup();
}

static void
station_scan_timer(struct wlan_station *station, uint64_t now)
{
	unsigned iteration;

	/* Two immediate transitions per channel plus terminal stop are bounded;
	 * the loop also consumes a ready/error synchronously reported by a fake or
	 * radio callback without relying on another edge wakeup. */
	for (iteration = 0U;
	    iteration < WLAN_SCAN_CHANNEL_MAX * 2U + 4U; iteration++) {
		unsigned long enabled;
		uint64_t generation = 0U;
		uint64_t deadline = 0U;
		uint32_t step = 0U;
		uint32_t channel = 0U;
		int active_probe = 0;
		int action = 0;
		int error;
		uint8_t probe[64];
		size_t probe_length = 0U;

		enabled = spin_lock_irqsave(&station->lock);
		now = station_now_locked(station);
		if (station->scan_state == WLAN_SCAN_RUNNING) {
			generation = station->scan_generation;
			if (deadline_expired(now, station->scan_deadline)) {
				station_scan_failed_locked(station, ETIMEDOUT);
				action = station->scan_driver_active ? 3 : 0;
			} else if (station->scan_event_error != 0) {
				error = station->scan_event_error;
				station_scan_failed_locked(station, error);
				action = station->scan_driver_active ? 3 : 0;
			} else if (station->scan_step_state ==
			    WLAN_SCAN_STEP_NEED_TUNE) {
				step = station->scan_step_index;
				channel = station->scan_profile.channels[step].channel;
				deadline = deadline_local(now,
				    WLAN_SCAN_TUNE_DEADLINE_TICKS,
				    station->scan_deadline);
				station->scan_step_deadline = deadline;
				station->scan_step_state = WLAN_SCAN_STEP_TUNING;
				/* Once start is invoked, stop is the mandatory producer
				 * barrier even when start itself reports an error. */
				station->scan_driver_active = 1;
				action = 1;
			} else if (station->scan_step_state ==
			    WLAN_SCAN_STEP_TUNING) {
				if (deadline_expired(now,
				    station->scan_step_deadline)) {
					station_scan_failed_locked(station,
					    ETIMEDOUT);
					action = station->scan_driver_active ? 3 : 0;
				} else if (station->scan_ready_pending) {
					station->scan_ready_pending = 0U;
					station->scan_step_state =
					    WLAN_SCAN_STEP_DWELL;
					deadline = deadline_local(now,
					    WLAN_SCAN_DWELL_TICKS,
					    station->scan_deadline);
					station->scan_step_deadline = deadline;
					active_probe = (station->scan_profile.channels[
					    station->scan_step_index].flags &
					    WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED) != 0U;
					if (active_probe) {
						probe_length = probe_request_build(station,
						    station->reconnect_scan_active, probe);
						action = 2;
					}
				}
			} else if (station->scan_step_state ==
			    WLAN_SCAN_STEP_DWELL && deadline_expired(now,
			    station->scan_step_deadline)) {
				if (!station->reconnect_scan_active &&
				    station->scan_step_index + 1U <
				    station->scan_profile.channel_count) {
					station->scan_step_index++;
					station->scan_step_state =
					    WLAN_SCAN_STEP_NEED_TUNE;
					station->scan_step_deadline = 0U;
				} else {
					station->scan_step_state =
					    WLAN_SCAN_STEP_NONE;
					station->scan_step_deadline = 0U;
					station->scan_publish_pending =
					    station->reconnect_scan_active ? 0U : 1U;
					action = station->scan_driver_active ? 4 : 5;
				}
			}
		} else if (deadline_expired(now,
		    station->scan_retry_deadline) &&
		    station->scan_driver_active) {
			generation = station->scan_generation;
			action = 3;
		}
		spin_unlock_irqrestore(&station->lock, enabled);

		if (action == 0) {
			/* Advancing a completed dwell to NEED_TUNE is immediate. */
			enabled = spin_lock_irqsave(&station->lock);
			active_probe = station->scan_state == WLAN_SCAN_RUNNING &&
			    station->scan_step_state == WLAN_SCAN_STEP_NEED_TUNE;
			spin_unlock_irqrestore(&station->lock, enabled);
			if (active_probe)
				continue;
			return;
		}
		if (action == 1) {
			error = station->ops->scan_channel_start(
			    station->radio_context, generation, step, channel,
			    deadline);
			if (error == 0)
				continue;
			enabled = spin_lock_irqsave(&station->lock);
			if (station->scan_generation == generation &&
			    station->scan_state == WLAN_SCAN_RUNNING)
				station_scan_failed_locked(station, error);
			spin_unlock_irqrestore(&station->lock, enabled);
			error = station->ops->scan_stop(station->radio_context,
			    generation);
			station_scan_stop_result(station, generation, error);
			return;
		}
		if (action == 2) {
			error = station->ops->management_transmit(
			    station->radio_context, generation, probe, probe_length,
			    deadline);
			if (error == 0)
				continue;
			enabled = spin_lock_irqsave(&station->lock);
			if (station->scan_generation == generation &&
			    station->scan_state == WLAN_SCAN_RUNNING)
				station_scan_failed_locked(station, error);
			spin_unlock_irqrestore(&station->lock, enabled);
			error = station->ops->scan_stop(station->radio_context,
			    generation);
			station_scan_stop_result(station, generation, error);
			return;
		}
		if (action == 5) {
			enabled = spin_lock_irqsave(&station->lock);
			if (station->scan_generation == generation)
				(void)station_scan_publish_locked(station,
				    generation);
			spin_unlock_irqrestore(&station->lock, enabled);
			return;
		}
		error = station->ops->scan_stop(station->radio_context,
		    generation);
		station_scan_stop_result(station, generation, error);
		return;
	}
}

static void
station_timer_run(struct wlan_station *station, uint64_t now)
{
	station_control_enter(station);
	station_connection_timer(station, now);
	station_scan_timer(station, now);
	station_control_leave(station);
}

void
wlan_timer_run(uint64_t now_ticks)
{
	unsigned index;

	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		struct wlan_station *station;

		if (station_index_enter(index, &station) != 0)
			continue;
		station_timer_run(station, now_ticks);
		station_leave(station);
	}
}

uint64_t
wlan_timer_next_deadline(void)
{
	uint64_t result = 0U;
	unsigned index;

	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return 0U;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		struct wlan_station *station;
		unsigned long enabled;
		uint64_t candidate = 0U;

		if (station_index_enter(index, &station) != 0)
			continue;
		enabled = spin_lock_irqsave(&station->lock);
		if (station->scan_state == WLAN_SCAN_RUNNING) {
			candidate = station->scan_deadline;
			if (station->scan_step_deadline != 0U &&
			    station->scan_step_deadline < candidate)
				candidate = station->scan_step_deadline;
		}
		if ((station->state == WLAN_STATE_AUTHENTICATING ||
		    station->state == WLAN_STATE_ASSOCIATING ||
		    station->state == WLAN_STATE_FOUR_WAY) &&
		    (candidate == 0U ||
		    station->connection_deadline < candidate))
			candidate = station->connection_deadline;
		if (station->connection_step_deadline != 0U &&
		    (candidate == 0U ||
		    station->connection_step_deadline < candidate))
			candidate = station->connection_step_deadline;
		if (station->scan_retry_deadline != 0U &&
		    (candidate == 0U ||
		    station->scan_retry_deadline < candidate))
			candidate = station->scan_retry_deadline;
		if (station->connect_retry_deadline != 0U &&
		    (candidate == 0U ||
		    station->connect_retry_deadline < candidate))
			candidate = station->connect_retry_deadline;
		if (station->beacon_watch_deadline != 0U &&
		    (candidate == 0U ||
		    station->beacon_watch_deadline < candidate))
			candidate = station->beacon_watch_deadline;
		if (station->reconnect_pending) {
			if (station->reconnect_cleanup_retry != 0U &&
			    (candidate == 0U ||
			    station->reconnect_cleanup_retry < candidate))
				candidate = station->reconnect_cleanup_retry;
			if (station->reconnect_next_attempt != 0U &&
			    (candidate == 0U ||
			    station->reconnect_next_attempt < candidate))
				candidate = station->reconnect_next_attempt;
			if (station->reconnect_deadline != 0U &&
			    (candidate == 0U ||
			    station->reconnect_deadline < candidate))
				candidate = station->reconnect_deadline;
		}
		spin_unlock_irqrestore(&station->lock, enabled);
		station_leave(station);
		if (candidate != 0U && (result == 0U || candidate < result))
			result = candidate;
	}
	return result;
}

int
wlan_work_pending(void)
{
	unsigned index;

	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return 0;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		struct wlan_station *station;
		unsigned long enabled;
		uint64_t now;
		int pending;

		if (station_index_enter(index, &station) != 0)
			continue;
		enabled = spin_lock_irqsave(&station->lock);
		now = station_now_locked(station);
		pending = (station->scan_state == WLAN_SCAN_RUNNING &&
		    (station->scan_step_state == WLAN_SCAN_STEP_NEED_TUNE ||
		    station->scan_ready_pending ||
		    station->scan_event_error != 0 ||
		    deadline_expired(now, station->scan_deadline) ||
		    deadline_expired(now, station->scan_step_deadline))) ||
		    deadline_expired(now, station->scan_retry_deadline) ||
		    (station->connect_stop_pending &&
		    (station->connect_retry_deadline == 0U ||
		    deadline_expired(now, station->connect_retry_deadline))) ||
		    deadline_expired(now, station->beacon_watch_deadline) ||
		    (station->reconnect_pending &&
		    (deadline_expired(now, station->reconnect_deadline) ||
		    (station->reconnect_cleanup_retry != 0U &&
		    now >= station->reconnect_cleanup_retry) ||
		    (!station->reconnect_scan_active &&
		    station->reconnect_cleanup_retry == 0U &&
		    !station->connect_driver_active &&
		    (station->reconnect_next_attempt == 0U ||
		    now >= station->reconnect_next_attempt)))) ||
		    ((station->state == WLAN_STATE_AUTHENTICATING ||
		    station->state == WLAN_STATE_ASSOCIATING ||
		    station->state == WLAN_STATE_FOUR_WAY) &&
		    (deadline_expired(now, station->connection_deadline) ||
		    deadline_expired(now,
		    station->connection_step_deadline)));
		spin_unlock_irqrestore(&station->lock, enabled);
		station_leave(station);
		if (pending)
			return 1;
	}
	return 0;
}

#ifdef WLAN_TESTING
int
wlan_station_test_attach(struct net_device *device,
	const struct wlan_radio_ops *ops, void *radio_context,
	const struct wlan_scan_profile *scan_profile, wlan_clock_fn clock,
	void *clock_context, struct wlan_station **result)
{
	unsigned long enabled;
	int error = wlan_station_attach(device, ops, radio_context,
	    scan_profile, result);

	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&(*result)->lock);
	(*result)->clock = clock != NULL ? clock : default_clock;
	(*result)->clock_context = clock_context;
	spin_unlock_irqrestore(&(*result)->lock, enabled);
	return 0;
}

int
wlan_station_test_set_report_hook(struct wlan_station *station,
	wlan_station_test_hook_fn hook, void *context)
{
	unsigned long enabled;
	int error = station_enter(station);

	if (error != 0)
		return error;
	enabled = spin_lock_irqsave(&station->lock);
	station->test_report_hook = hook;
	station->test_report_hook_context = context;
	spin_unlock_irqrestore(&station->lock, enabled);
	station_leave(station);
	return 0;
}

unsigned
wlan_station_test_control_waiters(struct wlan_station *station)
{
	unsigned long enabled;
	unsigned waiters;

	if (station == NULL)
		return 0U;
	enabled = spin_lock_irqsave(&station->lock);
	waiters = station->test_control_waiters;
	spin_unlock_irqrestore(&station->lock, enabled);
	return waiters;
}

int
wlan_station_test_secrets_clear(struct wlan_station *station)
{
	unsigned long enabled;
	int clear;

	if (station == NULL)
		return 1;
	enabled = spin_lock_irqsave(&station->lock);
	clear = station->credential_length == 0U &&
	    bytes_zero(station->credential, sizeof(station->credential)) &&
	    bytes_zero(station->wpa2.pmk, sizeof(station->wpa2.pmk)) &&
	    bytes_zero(station->wpa2.ptk, sizeof(station->wpa2.ptk)) &&
	    bytes_zero(station->wpa2.anonce, sizeof(station->wpa2.anonce)) &&
	    bytes_zero(station->wpa2.snonce, sizeof(station->wpa2.snonce)) &&
	    bytes_zero(station->wpa2.gtk, sizeof(station->wpa2.gtk)) &&
	    bytes_zero(station->wpa2.tx_frame,
	    sizeof(station->wpa2.tx_frame));
	spin_unlock_irqrestore(&station->lock, enabled);
	return clear;
}

int
wlan_station_test_seed_authorized(struct wlan_station *station,
	const struct wlan_bss_record *bss, uint64_t generation,
	uint64_t key_generation)
{
	static const uint8_t test_rates[WLAN_WPA2_RATE_MAX] = {
		0x82U, 0x84U, 0x8bU, 0x96U, 0x0cU, 0x12U,
		0x18U, 0x24U, 0x30U, 0x48U, 0x60U, 0x6cU
	};
	unsigned long enabled;
	uint64_t group_generation;
	uint64_t now;
	int error;

	if (station == NULL || bss == NULL || generation == 0U ||
	    key_generation == 0U || key_generation == UINT64_MAX ||
	    !bssid_valid(bss->bssid) || bss->ssid_length == 0U ||
	    bss->ssid_length > WLAN_SSID_MAX || bss->channel == 0U ||
	    bss->channel > 11U)
		return EINVAL;
	group_generation = key_generation + 1U;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used || !station->administrative_up || station->closing) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENETDOWN;
	}
	now = station_now_locked(station);
	station_cancel_reconnect_locked(station);
	station->selected = *bss;
	station->connection_generation = generation;
	if (station->next_generation < generation)
		station->next_generation = generation;
	station->connection_deadline = deadline_after(now,
	    WLAN_CONNECT_DEADLINE_TICKS);
	station->connection_step_deadline = 0U;
	station->connect_driver_active = 1;
	station->connect_stop_pending = 0;
	station->connect_retire_explicit = 0;
	station->connect_retry_deadline = 0U;
	station->transmit_packet_number = 0U;
	station->transmit_cookie = 0U;
	memset(&station->l2_rx, 0, sizeof(station->l2_rx));
	station->l2_rx.pairwise_key_generation = key_generation;
	station->l2_rx.group_key_generation[1] = group_generation;
	secure_zero(station->credential, sizeof(station->credential));
	station->credential_length = 0U;
	memset(&station->wpa2, 0, sizeof(station->wpa2));
	station->wpa2.ops = &station_wpa2_ops;
	station->wpa2.callback_context = station;
	station->wpa2.generation = generation;
	station->wpa2.key_generation = key_generation;
	station->wpa2.group_key_generation = group_generation;
	station->wpa2.next_key_generation = group_generation;
	station->wpa2.state = WLAN_WPA2_STATE_AUTHORIZED;
	station->wpa2.configured = 1U;
	station->wpa2.associated = 1U;
	station->wpa2.pairwise_installed = 1U;
	station->wpa2.group_installed = 1U;
	station->wpa2.authorized = 1U;
	station->wpa2.reconnectable = 1U;
	station->wpa2.gtk_index = 1U;
	station->wpa2.protocol_version = 2U;
	station->wpa2.profile = (struct wlan_wpa2_profile){0};
	memcpy(station->wpa2.profile.station, station->device->hwaddr, 6U);
	memcpy(station->wpa2.profile.bssid, bss->bssid, 6U);
	memcpy(station->wpa2.profile.ssid, bss->ssid, bss->ssid_length);
	station->wpa2.profile.ssid_length = bss->ssid_length;
	memcpy(station->wpa2.profile.rates, test_rates, sizeof(test_rates));
	station->wpa2.profile.rate_count = sizeof(test_rates);
	station->wpa2.profile.channel = bss->channel;
	station->wpa2.profile.capability = WLAN_LOCAL_ASSOC_CAPABILITY;
	station->wpa2.profile.listen_interval = 10U;
	station->wpa2.profile.total_deadline_ticks = station->connection_deadline;
	station->wpa2.profile.transition_timeout_ticks =
	    WLAN_CONNECT_TRANSITION_TICKS;
	station->wpa2.profile.recovery_timeout_ticks =
	    WLAN_CONNECT_TRANSITION_TICKS * 3U;
	memset(station->wpa2.pmk, 0x11, sizeof(station->wpa2.pmk));
	memset(station->wpa2.ptk, 0x22, sizeof(station->wpa2.ptk));
	memset(station->wpa2.gtk, 0x33, sizeof(station->wpa2.gtk));
	station->authenticated = 1U;
	station->associated = 1U;
	station->key_installed = 1U;
	station->controlled_port = 1U;
	station->state = WLAN_STATE_CONNECTED;
	station_beacon_watch_refresh_locked(station, now);
	error = net_device_set_carrier(station->device, 1);
	if (error != 0)
		station->controlled_port = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
	return error;
}

int
wlan_station_test_begin_pairwise_rekey(struct wlan_station *station)
{
	unsigned long enabled;
	int error;

	if (station == NULL)
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	if (wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTHORIZED || !station->wpa2.reconnectable ||
	    !station->wpa2.pairwise_installed) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}
	error = station_carrier_down_locked(station);
	if (error == 0) {
		station->wpa2.authorized = 0U;
		station->wpa2.pairwise_rekey = 1U;
		station->wpa2.state = WLAN_WPA2_STATE_MESSAGE_3;
		station_sync_wpa_locked(station);
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	return error;
}

int
wlan_station_test_begin_group_rekey(struct wlan_station *station)
{
	unsigned long enabled;

	if (station == NULL)
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	if (wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTHORIZED || !station->wpa2.reconnectable ||
	    !station->wpa2.authorized || !station->wpa2.pairwise_installed ||
	    !station->wpa2.group_installed) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}
	station->wpa2.tx_cookie_active = 0U;
	station->wpa2.state = WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX;
	station->wpa2.step_deadline_ticks = deadline_after(
	    station_now_locked(station), WLAN_CONNECT_TRANSITION_TICKS);
	station_sync_wpa_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

int
wlan_station_test_set_initial_phase(struct wlan_station *station,
	uint32_t phase)
{
	unsigned long enabled;

	if (station == NULL ||
	    (phase != WLAN_STATION_TEST_PHASE_ASSOCIATING &&
	    phase != WLAN_STATION_TEST_PHASE_FOUR_WAY))
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	if ((wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTH_TX &&
	    wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTH_RESPONSE) || station->wpa2.reconnectable) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}
	station->wpa2.tx_cookie_active = 0U;
	station->wpa2.associated = phase == WLAN_STATION_TEST_PHASE_FOUR_WAY;
	station->wpa2.state = phase == WLAN_STATION_TEST_PHASE_ASSOCIATING ?
	    WLAN_WPA2_STATE_ASSOC_RESPONSE : WLAN_WPA2_STATE_MESSAGE_3;
	station->wpa2.step_deadline_ticks = deadline_after(
	    station_now_locked(station), WLAN_CONNECT_TRANSITION_TICKS);
	station_sync_wpa_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

int
wlan_station_test_complete_authorized(struct wlan_station *station,
	uint64_t key_generation)
{
	unsigned long enabled;
	uint64_t group_generation;
	int error;

	if (station == NULL || key_generation == 0U ||
	    key_generation == UINT64_MAX)
		return EINVAL;
	group_generation = key_generation + 1U;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->reconnect_pending || !station->connect_driver_active ||
	    wlan_wpa2_engine_state(&station->wpa2) == WLAN_WPA2_STATE_IDLE ||
	    wlan_wpa2_engine_state(&station->wpa2) == WLAN_WPA2_STATE_FAILED ||
	    wlan_wpa2_engine_state(&station->wpa2) ==
	    WLAN_WPA2_STATE_RECONNECT_WAIT) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}
	station->wpa2.key_generation = key_generation;
	station->wpa2.group_key_generation = group_generation;
	station->wpa2.next_key_generation = group_generation;
	station->wpa2.gtk_index = 1U;
	station->wpa2.associated = 1U;
	station->wpa2.pairwise_installed = 1U;
	station->wpa2.group_installed = 1U;
	station->wpa2.authorized = 1U;
	station->wpa2.reconnectable = 1U;
	station->wpa2.pairwise_rekey = 0U;
	station->wpa2.state = WLAN_WPA2_STATE_AUTHORIZED;
	station->wpa2.step_deadline_ticks = 0U;
	memset(station->wpa2.ptk, 0x44, sizeof(station->wpa2.ptk));
	memset(station->wpa2.gtk, 0x55, sizeof(station->wpa2.gtk));
	memset(&station->l2_rx, 0, sizeof(station->l2_rx));
	station->l2_rx.pairwise_key_generation = key_generation;
	station->l2_rx.group_key_generation[1] = group_generation;
	station->transmit_packet_number = 0U;
	station_sync_wpa_locked(station);
	error = net_device_set_carrier(station->device, 1);
	if (error != 0)
		station->controlled_port = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
	return error;
}

int
wlan_station_test_snapshot(struct wlan_station *station,
	struct wlan_station_test_snapshot *snapshot)
{
	unsigned long enabled;

	if (station == NULL || snapshot == NULL)
		return EINVAL;
	enabled = spin_lock_irqsave(&station->lock);
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->connection_generation = station->connection_generation;
	snapshot->connection_deadline = station->connection_deadline;
	snapshot->connection_step_deadline = station->connection_step_deadline;
	snapshot->reconnect_deadline = station->reconnect_deadline;
	snapshot->reconnect_next_attempt = station->reconnect_next_attempt;
	snapshot->reconnect_cleanup_retry = station->reconnect_cleanup_retry;
	snapshot->connect_retry_deadline = station->connect_retry_deadline;
	snapshot->scan_retry_deadline = station->scan_retry_deadline;
	snapshot->beacon_watch_deadline = station->beacon_watch_deadline;
	snapshot->pairwise_key_generation =
	    station->l2_rx.pairwise_key_generation;
	snapshot->group_key_generation = station->wpa2.group_key_generation;
	snapshot->pending_pairwise_key_generation =
	    station->wpa2.pending_pairwise_key_generation;
	snapshot->pending_group_key_generation =
	    station->wpa2.pending_group_key_generation;
	snapshot->pairwise_receive_packet_number =
	    station->l2_rx.pairwise_packet_number;
	memcpy(snapshot->group_receive_packet_number,
	    station->l2_rx.group_packet_number,
	    sizeof(snapshot->group_receive_packet_number));
	snapshot->pending_group_receive_packet_number =
	    station->wpa2.pending_group_receive_packet_number;
	snapshot->transmit_packet_number = station->transmit_packet_number;
	snapshot->reconnect_attempts = station->reconnect_attempts;
	snapshot->state = station->state;
	snapshot->wpa_state = (uint32_t)station->wpa2.state;
	snapshot->association_capability = station->wpa2.profile.capability;
	snapshot->reconnect_pending = station->reconnect_pending != 0;
	snapshot->reconnect_scan_active = station->reconnect_scan_active != 0;
	snapshot->controlled_port = station->controlled_port != 0U;
	snapshot->connect_driver_active = station->connect_driver_active != 0;
	snapshot->connect_stop_pending = station->connect_stop_pending != 0;
	snapshot->connect_retire_explicit =
	    station->connect_retire_explicit != 0;
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

int
wlan_station_test_transmit_eapol(struct wlan_station *station,
	uint64_t cookie, const uint8_t *frame, size_t length)
{
	unsigned long enabled;
	uint64_t generation;
	uint64_t deadline;
	int error;

	if (station == NULL || cookie == 0U || frame == NULL || length == 0U)
		return EINVAL;
	error = station_enter(station);
	if (error != 0)
		return error;
	station_control_enter(station);
	enabled = spin_lock_irqsave(&station->lock);
	generation = station->connection_generation;
	deadline = deadline_local(station_now_locked(station),
	    WLAN_CONNECT_TRANSITION_TICKS, station->connection_deadline);
	spin_unlock_irqrestore(&station->lock, enabled);
	error = station_wpa_transmit(station, generation, cookie,
	    WLAN_WPA2_TX_EAPOL, station->selected.bssid, frame, length, deadline);
	station_control_leave(station);
	station_leave(station);
	return error;
}
#endif
