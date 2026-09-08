/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The WLAN station core.
 *
 * A station binds a network device to a radio driver and runs the scan
 * and WPA2 connection state machines on behalf of the network worker.
 * Radio callbacks report scan results, frames, and link loss under the
 * station spinlock; control methods that may sleep in a bus driver are
 * serialized by a separate control gate.  Scan results are staged per
 * scan and published as an immutable snapshot for the ioctl interface.
 */

#include "kern/net/wlan.h"

#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/net/net-device.h"
#include "kern/net/packet-buf.h"
#include "kern/net/wifi/wlan-l2.h"
#include "kern/net/wifi/wlan-wpa2.h"

#include <hal/hal.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#define WLAN_LOCAL_ASSOC_CAPABILITY 0x0011U
#define WLAN_ASSOC_CAPABILITY_SHORT_SLOT_TIME 0x0400U
#define WLAN_BEACON_MISS_MULTIPLIER 20U
#define WLAN_BEACON_WATCH_MIN_TICKS (2U * KERN_CLOCK_HZ)
#define WLAN_BEACON_WATCH_MAX_TICKS (10U * KERN_CLOCK_HZ)
#define WLAN_PROBE_REQUEST_MAX_SIZE (24U + 2U + WLAN_SSID_MAX + 2U + 8U)

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
	int hardware_quiesce;
	int stop_pending;
	int stop_work_active;
	int stop_retry_disabled;
	int stop_error;
	unsigned stop_attempts;
	uint64_t stop_retry_deadline;
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
	uint64_t beacon_watch_deadline;
	uint32_t state;
	int32_t terminal_error;
	uint32_t administrative_up;
	uint32_t authenticated;
	uint32_t associated;
	uint32_t key_installed;
	uint32_t controlled_port;
	uint32_t retry_count;
	int connect_start_pending;
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

static void secure_zero(void *memory, size_t length);
static uint64_t default_clock(void *context);
static uint64_t deadline_after(uint64_t now, uint64_t delta);
static int deadline_checked(uint64_t now, uint64_t delta, uint64_t *result);
static uint64_t deadline_local(uint64_t now, uint64_t delta, uint64_t total_deadline);
static uint64_t station_beacon_watch_ticks(uint16_t beacon_interval_tu);
static void station_beacon_watch_refresh_locked(struct wlan_station *station, uint64_t now);
static int station_beacon_watch_active_locked(const struct wlan_station *station);
static int deadline_expired(uint64_t now, uint64_t deadline);
static void wlan_worker_wakeup(void);
static int bytes_zero(const void *memory, size_t length);
static uint32_t channel_frequency(uint32_t channel);
static int scan_profile_validate(const struct wlan_scan_profile *profile);
static uint64_t scan_deadline_ticks(uint32_t channel_count);
static int device_name_matches(const struct net_device *device, const char *name);
static int header_validate(const struct net_device *device, const struct wlan_ioctl_header *header, size_t size);
static uint64_t station_now_locked(struct wlan_station *station);
static int station_generation_locked(struct wlan_station *station, uint64_t *result);
static void station_clear_connection_locked(struct wlan_station *station);
static void station_finish_connection_retire_locked(struct wlan_station *station);
static int station_carrier_down_locked(struct wlan_station *station);
static uint64_t station_wpa_deadline(struct wlan_station *station);
static uint64_t station_wpa_cleanup_deadline(struct wlan_station *station);
static void station_sync_wpa_locked(struct wlan_station *station);
static int station_wpa_entropy_fill(void *context, void *buffer, size_t length);
static int station_wpa_radio_start(void *context, uint64_t generation, const uint8_t bssid[6], uint32_t channel, uint64_t deadline, uint64_t *completion_ticks);
static int station_wpa_transmit(void *context, uint64_t generation, uint64_t cookie, enum wlan_wpa2_tx_kind kind, const uint8_t destination[6], const uint8_t *frame, size_t length, uint64_t deadline);
static int station_wpa_association_set(void *context, uint64_t generation, const uint8_t bssid[6], uint16_t aid);
static int station_wpa_association_clear(void *context, uint64_t generation);
static int station_wpa_key_install(void *context, uint64_t generation, enum wlan_wpa2_key_kind kind, uint8_t key_index, const uint8_t key[16], uint64_t key_generation, uint64_t receive_packet_number);
static int station_wpa_keys_activate(void *context, uint64_t generation, uint64_t pairwise_key_generation, uint64_t group_key_generation);
static int station_wpa_key_receive_pn_advance(void *context, uint64_t generation, enum wlan_wpa2_key_kind kind, uint8_t key_index, uint64_t key_generation, uint64_t receive_packet_number);
static int station_wpa_key_delete(void *context, uint64_t generation, enum wlan_wpa2_key_kind kind, uint8_t key_index, uint64_t key_generation);
static int station_wpa_authorized_set(void *context, uint64_t generation, int authorized);
static int station_wpa_radio_stop(void *context, uint64_t generation);
static int station_link_lost_controlled(struct wlan_station *station, uint64_t generation, int reason);
static int station_enter(struct wlan_station *station);
static void station_leave(struct wlan_station *station);
static void station_control_enter(struct wlan_station *station);
static void station_control_leave(struct wlan_station *station);
static int station_find_enter(struct net_device *device, struct wlan_station **result);
static int station_index_enter(unsigned index, struct wlan_station **result);
static int bssid_compare(const uint8_t left[6], const uint8_t right[6]);
static int bssid_valid(const uint8_t bssid[6]);
static size_t probe_request_build(const struct wlan_station *station, int directed, uint32_t channel, uint8_t frame[WLAN_PROBE_REQUEST_MAX_SIZE]);
static int cache_entry_worse(const struct wlan_cache_entry *left, const struct wlan_cache_entry *right);
static int cache_insert_locked(struct wlan_station *station, const struct wlan_bss_record *bss, uint64_t now);
static void cache_sort_by_bssid(struct wlan_cache_entry *entries, uint32_t count);
static int bss_security_supported(const struct wlan_bss_record *bss);
static int station_select_bss_locked(struct wlan_station *station, const uint8_t *ssid, uint32_t ssid_length, struct wlan_bss_record *result);
static void scan_request_output_locked(struct wlan_station *station, struct wlan_scan_request *request);
static int ioctl_scan(struct wlan_station *station, struct wlan_scan_request *request);
static int ioctl_scan_status(struct wlan_station *station, struct wlan_scan_status_request *request);
static uint32_t entry_age_ms(uint64_t now, uint64_t last_seen);
static int ioctl_bss(struct wlan_station *station, struct wlan_bss_request *request);
static int ioctl_connect(struct wlan_station *station, struct wlan_connect_request *request);
static int station_retire_controlled(struct wlan_station *station, int keep_administrative_up);
static int station_retire(struct wlan_station *station, int keep_administrative_up);
static int ioctl_disconnect(struct wlan_station *station, struct wlan_disconnect_request *request);
static int ioctl_status(struct wlan_station *station, struct wlan_status_request *request);
static int station_status_device(struct net_device *device, struct wlan_status_request *request);
static struct net_device * station_finalize_locked(struct wlan_station *station);
static void station_scan_failed_locked(struct wlan_station *station, int error);
static int station_scan_publish_locked(struct wlan_station *station, uint64_t generation);
static void station_scan_stop_result(struct wlan_station *station, uint64_t generation, int error);
static void station_connection_start(struct wlan_station *station);
static void station_connection_timer(struct wlan_station *station, uint64_t now);
static void station_scan_timer(struct wlan_station *station, uint64_t now);
static void station_timer_run(struct wlan_station *station, uint64_t now);

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

/*
 * Initializes the station registry once, waiting for a concurrent
 * initializer to finish.
 */
void
wlan_core_init(
	void)
{
	unsigned expected;
	unsigned index;

	expected = 0U;

	/* Only the first caller initializes; later callers wait for it. */
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

	/* Clears every slot and initializes its lock exactly once. */
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

/*
 * Attaches a station to a network device and radio driver.
 *
 * The radio must offer scan start and stop together, and either the
 * complete connection method set or none of it.  Active probing needs a
 * management transmit method.
 */
int
wlan_station_attach(
	struct net_device *device,
	const struct wlan_radio_ops *ops,
	void *radio_context,
	const struct wlan_scan_profile *scan_profile,
	struct wlan_station **result)
{
	unsigned long enabled;
	unsigned index;
	struct wlan_station *free_station;
	int error;
	unsigned long station_enabled;

	free_station = NULL;

	/* Rejects a missing operand, an invalid profile, or a bad address. */
	if (device == NULL ||
	    ops == NULL ||
	    result == NULL ||
	    scan_profile_validate(scan_profile) != 0)
		return EINVAL;
	if (device->hwaddr_len != 6U || !bssid_valid(device->hwaddr))
		return EINVAL;

	/* Scan methods come in pairs; connection methods come as a set. */
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

	/*
	 * The caller retains its allocation-owner reference across this
	 * call.  The station acquires a distinct live reference before any
	 * device mutation or station publication.
	 */
	if (!net_device_ref_live(device))
		return ENODEV;

	/* Finds a free slot while refusing a duplicate attachment. */
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

	/*
	 * Validate and reserve the registry slot before mutating the
	 * device.  The registry (rank 100) may enter the device carrier
	 * guard (rank 125); no device operation enters the WLAN registry
	 * while holding that guard.
	 */
	error = net_device_set_carrier(device, 0);
	if (error != 0) {
		spin_unlock_irqrestore(&wlan_registry_lock, enabled);
		net_device_release(device);
		return error;
	}

	/*
	 * The slot lock is initialized exactly once by wlan_core_init().
	 * Reset only the lifetime payload while the registry excludes all
	 * timer/admission lookups of this unused slot.
	 */
	station_enabled = spin_lock_irqsave(&free_station->lock);
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
	*result = free_station;
	spin_unlock_irqrestore(&wlan_registry_lock, enabled);
	return 0;
}

/*
 * Replaces the scan profile of an idle, administratively down station.
 */
int
wlan_station_scan_profile_update(
	struct wlan_station *station,
	const struct wlan_scan_profile *scan_profile)
{
	unsigned long enabled;
	unsigned index;
	int error;

	/* Validates the profile and takes the station and control gate. */
	error = scan_profile_validate(scan_profile);
	if (error != 0)
		return error;
	error = station_enter(station);
	if (error != 0)
		return error;
	station_control_enter(station);

	/* Only a fully idle station accepts a new profile. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used || station->closing || station->lifecycle_inflight) {
		error = ENODEV;
	} else if (station->administrative_up ||
	    station->state != WLAN_STATE_DOWN ||
	    station->scan_driver_active ||
	    station->connect_driver_active ||
	    station->scan_state == WLAN_SCAN_RUNNING) {
		error = EBUSY;
	} else {
		/* Active probing needs a management transmit method. */
		error = 0;
		if (station->ops->management_transmit == NULL) {
			for (index = 0U; index < scan_profile->channel_count;
			     index++) {
				if ((scan_profile->channels[index].flags &
				    WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED) != 0U) {
					error = EINVAL;
					break;
				}
			}
		}
		if (error == 0)
			station->scan_profile = *scan_profile;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	station_control_leave(station);
	station_leave(station);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Marks a station administratively up.
 */
int
wlan_station_open(
	struct wlan_station *station)
{
	unsigned long enabled;
	int error;

	error = station_enter(station);
	if (error != 0)
		return error;

	/* A down or failed station becomes idle. */
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

/*
 * Records a BSS reported by the radio during the current scan step.
 *
 * The report must match the running scan generation and the channel
 * being dwelled on; an expired scan wakes the worker to time it out.
 */
int
wlan_station_report_scan_bss(
	struct wlan_station *station,
	uint64_t generation,
	const struct wlan_bss_record *bss)
{
	struct wlan_bss_record normalized;
	unsigned long enabled;
	uint64_t now;
	int scan_channel_accepting;
	int wake_worker;
	int error;
	uint32_t expected_frequency;
	const uint32_t known_security = WLAN_SECURITY_PRIVACY |
	    WLAN_SECURITY_WPA1 | WLAN_SECURITY_WPA2 | WLAN_SECURITY_TKIP |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK |
	    WLAN_SECURITY_IEEE8021X | WLAN_SECURITY_SAE |
	    WLAN_SECURITY_PMF_CAPABLE | WLAN_SECURITY_PMF_REQUIRED |
	    WLAN_SECURITY_UNSUPPORTED_SUITE;
#ifdef WLAN_TESTING
	wlan_station_test_hook_fn hook;
	void *hook_context;
#endif

	wake_worker = 0;

	/* Rejects a malformed record. */
	if (bss == NULL ||
	    bss->ssid_length > WLAN_SSID_MAX ||
	    channel_frequency(bss->channel) == 0U ||
	    bss->capability > UINT16_MAX ||
	    bss->beacon_interval_tu > UINT16_MAX ||
	    (bss->security & ~known_security) != 0U ||
	    !bssid_valid(bss->bssid) ||
	    !bytes_zero(bss->reserved, sizeof(bss->reserved)))
		return EINVAL;
	expected_frequency = channel_frequency(bss->channel);
	if (bss->center_frequency_mhz != expected_frequency)
		return EINVAL;

	/* Normalizes the record before staging it. */
	normalized = *bss;
	memset(normalized.ssid + normalized.ssid_length, 0,
	    WLAN_SSID_MAX - normalized.ssid_length);
	normalized.age_ms = 0U;
	error = station_enter(station);
	if (error != 0)
		return error;
#ifdef WLAN_TESTING
	/* Runs a one-shot test hook outside the lock. */
	enabled = spin_lock_irqsave(&station->lock);
	hook = station->test_report_hook;
	hook_context = station->test_report_hook_context;
	station->test_report_hook = NULL;
	station->test_report_hook_context = NULL;
	spin_unlock_irqrestore(&station->lock, enabled);
	if (hook != NULL)
		hook(hook_context);
#endif

	/* Accepts the record only for the channel being scanned right now. */
	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);
	scan_channel_accepting =
	    station->scan_step_state == WLAN_SCAN_STEP_DWELL ||
	    (station->scan_step_state == WLAN_SCAN_STEP_TUNING &&
	    station->scan_ready_pending);
	if (station->scan_state != WLAN_SCAN_RUNNING ||
	    station->scan_generation != generation ||
	    !scan_channel_accepting ||
	    station->scan_step_index >= station->scan_profile.channel_count ||
	    station->scan_profile.channels[station->scan_step_index].channel !=
	    normalized.channel) {
		error = ESTALE;
	} else if (deadline_expired(now, station->scan_deadline) ||
	    deadline_expired(now, station->scan_step_deadline)) {
		/*
		 * Report paths may be IRQ/USB completion context.  They
		 * never call back into a driver or join their own producer.
		 * The network worker owns terminal timeout and synchronous
		 * stop.
		 */
		wake_worker = 1;
		error = ETIMEDOUT;
	} else {
		error = cache_insert_locked(station, &normalized, now);
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (wake_worker)
		wlan_worker_wakeup();
	station_leave(station);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Parses a beacon or probe response and records the BSS it describes.
 */
int
wlan_station_report_scan_frame(
	struct wlan_station *station,
	uint64_t generation,
	const uint8_t *frame,
	size_t length,
	int32_t rssi_dbm,
	uint8_t channel_hint)
{
	struct wlan_bss_record bss;
	int error;

	/* Decodes the beacon or probe response into one BSS record. */
	if (length > WLAN_MANAGEMENT_FRAME_MAX)
		return EMSGSIZE;
	error = wlan_frame_parse_bss(frame, length, rssi_dbm, channel_hint,
	    &bss);
	if (error != 0)
		return error;

	/* Reports the record to the scan. */
	error = wlan_station_report_scan_bss(station, generation, &bss);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Records that the radio has tuned to the requested scan channel.
 */
int
wlan_station_report_scan_channel_ready(
	struct wlan_station *station,
	uint64_t generation,
	uint32_t step_index)
{
	unsigned long enabled;
	int result;

	result = station_enter(station);
	if (result != 0)
		return result;

	/* Only the step being tuned accepts the report. */
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

/*
 * Records a radio error against the running scan for the worker to
 * apply.
 */
int
wlan_station_report_scan_error(
	struct wlan_station *station,
	uint64_t generation,
	int error)
{
	unsigned long enabled;
	int result;

	if (error <= 0)
		return EINVAL;
	result = station_enter(station);
	if (result != 0)
		return result;

	/* Keeps the first error of the running scan. */
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

/*
 * Reports that the radio lost the link of a connection generation.
 */
int
wlan_station_report_link_loss(
	struct wlan_station *station,
	uint64_t generation,
	int error)
{
	int result;

	/* Requires a live generation and a real driver error. */
	if (generation == 0U || error <= 0)
		return EINVAL;

	/* Records the loss under the station's control gate. */
	result = station_enter(station);
	if (result != 0)
		return result;
	station_control_enter(station);
	result = station_link_lost_controlled(station, generation, error);
	station_control_leave(station);
	station_leave(station);

	/* Lets the worker act on the new state. */
	wlan_worker_wakeup();

	/* Reports the result. */
	return result;
}

/*
 * Delivers a received frame to the scan cache, the WPA2 engine, or the
 * network device.
 *
 * Beacons refresh the beacon watch of a connected station and otherwise
 * feed the scan cache with probe responses.  Other management frames and
 * EAPOL go to the WPA2 engine; authorized data frames become Ethernet
 * packets for the device.
 */
int
wlan_station_report_frame(
	struct wlan_station *station,
	const struct wlan_radio_rx_frame *report)
{
	struct wlan_bss_record management_bss;
	struct wlan_l2_rx_security security;
	struct packet_buf *packet;
	uint8_t ethernet[WLAN_L2_ETHERNET_MAX];
	uint16_t frame_control;
	size_t ethernet_length;
	unsigned long enabled;
	uint64_t now;
	int result;

	packet = NULL;
	ethernet_length = 0U;

	/* Rejects a malformed report. */
	if (report == NULL ||
	    report->frame == NULL ||
	    report->length < 2U ||
	    report->length > WLAN_MANAGEMENT_FRAME_MAX ||
	    report->generation == 0U ||
	    channel_frequency(report->channel) == 0U ||
	    report->cipher > WLAN_RADIO_CIPHER_CCMP ||
	    (report->decrypted != 0U && report->decrypted != 1U) ||
	    (report->integrity_error != 0U &&
	    report->integrity_error != 1U) ||
	    report->key_index > 3U ||
	    report->packet_number > 0x0000ffffffffffffULL ||
	    !bytes_zero(report->reserved, sizeof(report->reserved)))
		return EINVAL;
	frame_control = (uint16_t)((uint16_t)report->frame[0] |
	    ((uint16_t)report->frame[1] << 8));

	/* A beacon refreshes the beacon watch; otherwise it feeds the scan. */
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
		result = wlan_station_report_scan_bss(station,
		    report->generation, &management_bss);
		return result;
	}

	/* Everything else belongs to the current connection. */
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

	/* Management frames go to the WPA2 engine or end the connection. */
	if ((frame_control & 0x000cU) == 0U) {
		/*
		 * PMF is outside this first profile.  Management input
		 * therefore carries neither Protected Frame nor data-key
		 * metadata.
		 */
		if ((frame_control & 0x4000U) != 0U ||
		    report->key_generation != 0U ||
		    report->packet_number != 0U ||
		    report->cipher != WLAN_RADIO_CIPHER_NONE ||
		    report->decrypted != 0U ||
		    report->key_index != 0U) {
			result = EACCES;
			goto out;
		}
		if ((frame_control & (uint16_t)~0x0800U) == 0x00a0U ||
		    (frame_control & (uint16_t)~0x0800U) == 0x00c0U) {
			/*
			 * PMF is outside this profile, so a matching
			 * unprotected disassociation/deauthentication is
			 * authoritative.  Frames for another BSS/station are
			 * ordinary unrelated management traffic.
			 */
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

	/* Data frames are converted to Ethernet with their key metadata. */
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
		/*
		 * Clear EAPOL is confined to the initial four-way exchange.
		 * Once a pairwise generation has reached the connected
		 * lifetime, rekey M1/G1 and every response/retry must arrive
		 * through the active CCMP domain.  Otherwise an
		 * unauthenticated clear M1 could close the controlled port.
		 */
		if (station->wpa2.connected_lifetime &&
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

	/* Only protected data of an authorized station reaches the device. */
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
	/* Publishes the engine state the frame produced. */
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

/*
 * Reports the completion of a transmitted frame to the WPA2 engine or
 * the device statistics.
 */
int
wlan_station_report_tx_complete(
	struct wlan_station *station,
	uint64_t generation,
	uint64_t cookie,
	int acknowledged,
	int error)
{
	unsigned long enabled;
	uint64_t now;
	int result;

	/* Rejects an inconsistent completion. */
	if (generation == 0U ||
	    cookie == 0U ||
	    error < 0 ||
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

	/* An engine frame updates the handshake; a data frame counts errors. */
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

/*
 * Transmits an Ethernet packet as a protected data frame.
 *
 * The packet is consumed whatever the outcome.
 */
int
wlan_station_transmit(
	struct wlan_station *station,
	struct packet_buf *packet)
{
	struct wlan_radio_tx_request request;
	uint8_t mpdu[WLAN_L2_MPDU_MAX];
	unsigned long enabled;
	size_t mpdu_length;
	uint64_t now;
	int result;

	mpdu_length = 0U;

	/* Takes the station, discarding the packet if it is gone. */
	if (packet == NULL)
		return EINVAL;
	result = station_enter(station);
	if (result != 0) {
		packet_buf_free(packet);
		return result;
	}
	station_control_enter(station);
	memset(&request, 0, sizeof(request));

	/* Takes the next packet number and cookie of an authorized link. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->wpa2.authorized ||
	    !station->controlled_port ||
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

	/* Builds the MPDU and hands it to the radio. */
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

/*
 * Dispatches a WLAN ioctl to the station of a network device.
 *
 * A connect request's passphrase is preserved only across header and
 * device validation and is redacted on every return path.
 */
int
wlan_station_ioctl(
	struct net_device *device,
	unsigned long request,
	void *argument)
{
	struct wlan_station *station;
	struct wlan_ioctl_header *header;
	size_t expected_size;
	int error;
	struct wlan_connect_request *connect;
	uint8_t saved[WLAN_PASSPHRASE_STORAGE];

	if (argument == NULL)
		return EFAULT;
	if (atomic_load_acquire(&wlan_initialized) != 2U)
		wlan_core_init();

	/* A connect request carries a secret that must not outlive the call. */
	if (request == SIOCSWLANCONNECT) {
		connect = argument;
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

	/* Every other request is validated by its expected size. */
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
	/*
	 * Read-only observation does not join the mutable-operation admission
	 * gate.  Registry and station locks protect this snapshot even during
	 * a checked stop.
	 */
	if (request == SIOCGWLANSTATUS)
		return station_status_device(device, argument);
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Retires a station's scan and connection and marks it administratively
 * down.
 *
 * The close is refused while callers are still active on the station.
 */
int
wlan_station_close(
	struct wlan_station *station)
{
	unsigned long enabled;
	int error;

	if (station == NULL)
		return ENODEV;

	/* Blocks new callers, then waits for none to be active. */
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

	/* Retires everything; a failure leaves the station closing. */
	error = station_retire(station, 0);
	enabled = spin_lock_irqsave(&station->lock);
	station->lifecycle_inflight = 0;
	if (error == 0)
		station->closing = 0;
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Publishes close intent before the driver starts its synchronous join. */
void
wlan_station_stop_request(struct wlan_station *station)
{
	unsigned long enabled;

	if (station == NULL)
		return;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->used) {
		station->stop_pending = 1;
		station->stop_retry_deadline = 0U;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
}

/* Only the checked close owner records completion or arms a bounded retry. */
void
wlan_station_stop_complete(struct wlan_station *station, int error)
{
	unsigned long enabled;
	uint64_t delay;

	if (station == NULL)
		return;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->used) {
		station->stop_pending = error != 0;
		station->stop_error = error;
		station->stop_retry_deadline = 0U;
		if (error == 0)
			station->stop_attempts = 0U;
		else {
			if (station->stop_attempts < 5U)
				station->stop_attempts++;
			delay = (uint64_t)KERN_CLOCK_HZ <<
			    (station->stop_attempts - 1U);
			station->stop_retry_deadline = deadline_after(
			    station_now_locked(station), delay);
		}
	}
	spin_unlock_irqrestore(&station->lock, enabled);
}

/* Open may not reuse an epoch while independent retirement still owns it. */
int
wlan_station_stop_busy(struct wlan_station *station)
{
	unsigned long enabled;
	int busy;

	if (station == NULL)
		return 0;
	enabled = spin_lock_irqsave(&station->lock);
	busy = station->used && (station->stop_pending || station->stop_work_active);
	spin_unlock_irqrestore(&station->lock, enabled);
	return busy;
}

/* Teardown wins future work, but must join any already claimed callback. */
int
wlan_station_stop_cancel(struct wlan_station *station)
{
	unsigned long enabled;
	int error;

	if (station == NULL)
		return 0;
	enabled = spin_lock_irqsave(&station->lock);
	station->stop_retry_disabled = 1;
	error = station->stop_work_active ? EBUSY : 0;
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Retries stopped interfaces from a separate thread.
 *
 * A separate thread retries stopped interfaces without network-worker
 * progress.  The work pin is distinct from active: the callback itself must
 * acquire the common close barrier.  Detach and shutdown cannot finalize a
 * work-pinned station.
 */
void
wlan_retirement_run(uint64_t now_ticks)
{
	struct wlan_station *station;
	unsigned long enabled;
	unsigned index;
	int run;
	int error;

	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		station = &wlan_stations[index];
		enabled = spin_lock_irqsave(&station->lock);
		run = station->used && !station->blocked && !station->shutdown_owned &&
		    station->stop_pending && !station->stop_work_active &&
		    !station->stop_retry_disabled && !station->lifecycle_inflight &&
		    station->active == 0U && !station->control_inflight &&
		    station->stop_retry_deadline != 0U &&
		    now_ticks >= station->stop_retry_deadline &&
		    station->ops->stop_retry != NULL;
		if (run)
			station->stop_work_active = 1;
		spin_unlock_irqrestore(&station->lock, enabled);
		if (!run)
			continue;
		error = station->ops->stop_retry(station->radio_context);
		wlan_station_stop_complete(station, error);
		enabled = spin_lock_irqsave(&station->lock);
		station->stop_work_active = 0;
		spin_unlock_irqrestore(&station->lock, enabled);
	}
}

/*
 * Holds an idle common station closed while its driver stops hardware.
 * No radio callback or wait runs here; existing callers must retire first.
 */
int
wlan_station_quiesce_begin(
	struct wlan_station *station)
{
	unsigned long enabled;

	/* Prevents a reset from racing another lifecycle owner or admitted caller. */
	if (station == NULL)
		return ENODEV;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used || station->blocked) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENODEV;
	}
	station->closing = 1;
	if (station->active != 0U || station->control_inflight ||
	    station->lifecycle_inflight) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return EBUSY;
	}
	station->lifecycle_inflight = 1;
	station->hardware_quiesce = 1;
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

/*
 * Releases the hardware-stop barrier without forgetting common ownership.
 * The driver must retry close after a successful stop; failures stay closed.
 */
void
wlan_station_quiesce_end(
	struct wlan_station *station)
{
	unsigned long enabled;

	/* Requires the paired barrier owner and leaves admission closed for reconciliation. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->hardware_quiesce || !station->lifecycle_inflight)
		__builtin_trap();
	station->hardware_quiesce = 0;
	station->lifecycle_inflight = 0;
	spin_unlock_irqrestore(&station->lock, enabled);
}

/*
 * Detaches a station from its device after quiescing the radio.
 */
int
wlan_station_detach(
	struct wlan_station *station)
{
	unsigned long registry_enabled;
	unsigned long enabled;
	struct net_device *release_device;
	int error;

	if (station == NULL || atomic_load_acquire(&wlan_initialized) != 2U)
		return ENODEV;

	/* Blocks new callers and claims the lifecycle. */
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used) {
		spin_unlock_irqrestore(&station->lock, enabled);
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return ENODEV;
	}
	if (station->shutdown_owned || station->stop_work_active ||
	    station->lifecycle_inflight ||
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

	/* Retires the station and quiesces the radio. */
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

	/* Frees the slot unless a shutdown claimed it meanwhile. */
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

/*
 * Retires and detaches every station for system shutdown.
 *
 * The first pass closes admission to every slot; the second retires each
 * owned station.  A busy station aborts the shutdown with EBUSY.
 */
int
wlan_station_shutdown_all(
	void)
{
	unsigned long registry_enabled;
	unsigned index;
	int first_error;
	int busy;
	struct wlan_station *station;
	unsigned long enabled;
	struct net_device *release_device;
	int owned;
	int error;

	first_error = 0;
	busy = 0;

	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return 0;

	/*
	 * The terminal registry latch prevents attachment and slot reuse
	 * between the admission-closing pass and the checked retirement
	 * pass.
	 */
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	if (wlan_shutdown_inflight) {
		spin_unlock_irqrestore(&wlan_registry_lock, registry_enabled);
		return EBUSY;
	}
	wlan_shutdown_inflight = 1;
	wlan_stopping = 1;
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		station = &wlan_stations[index];
		if (!station->used)
			continue;
		enabled = spin_lock_irqsave(&station->lock);
		if (station->lifecycle_inflight || station->stop_work_active ||
		    (station->stop_pending && !station->stop_retry_disabled)) {
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

	/* Retires and finalizes every owned station. */
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		station = &wlan_stations[index];
		release_device = NULL;
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

/*
 * Runs the connection and scan timers of every station.
 */
void
wlan_timer_run(
	uint64_t now_ticks)
{
	unsigned index;
	struct wlan_station *station;

	/* Runs the timer of every live station. */
	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		if (station_index_enter(index, &station) != 0)
			continue;
		station_timer_run(station, now_ticks);
		station_leave(station);
	}
}

/*
 * Reports the earliest deadline of any station, or zero when none is
 * pending.
 */
uint64_t
wlan_timer_next_deadline(
	void)
{
	uint64_t result;
	unsigned index;
	struct wlan_station *station;
	unsigned long enabled;
	uint64_t candidate;

	/* Starts out with no deadline pending. */
	result = 0U;

	/* Reports no deadline until the subsystem is fully up. */
	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return 0U;

	/* Takes the earliest deadline any live station is waiting on. */
	for (index = 0U; index < NET_DEVICE_MAX; index++) {

		/* Skips an index that holds no live station. */
		candidate = 0U;
		if (station_index_enter(index, &station) != 0)
			continue;

		/* Takes the earliest of the station's live deadlines. */
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
		spin_unlock_irqrestore(&station->lock, enabled);
		station_leave(station);
		if (candidate != 0U && (result == 0U || candidate < result))
			result = candidate;
	}
	return result;
}

/*
 * Tests whether any station has timer work due now.
 */
int
wlan_work_pending(
	void)
{
	unsigned index;
	struct wlan_station *station;
	unsigned long enabled;
	uint64_t now;
	int pending;

	/* Asks every live station whether it has work due. */
	if (atomic_load_acquire(&wlan_initialized) != 2U)
		return 0;
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		if (station_index_enter(index, &station) != 0)
			continue;

		/* A station has work when any of its deadlines has expired. */
		enabled = spin_lock_irqsave(&station->lock);
		now = station_now_locked(station);
		pending = (station->scan_state == WLAN_SCAN_RUNNING &&
		    (station->scan_step_state == WLAN_SCAN_STEP_NEED_TUNE ||
		    station->scan_ready_pending ||
		    station->scan_event_error != 0 ||
		    deadline_expired(now, station->scan_deadline) ||
		    deadline_expired(now, station->scan_step_deadline))) ||
		    (station->scan_driver_active &&
		    station->scan_state != WLAN_SCAN_RUNNING &&
		    (station->scan_retry_deadline == 0U ||
		    deadline_expired(now, station->scan_retry_deadline))) ||
		    station->connect_start_pending ||
		    deadline_expired(now, station->scan_retry_deadline) ||
		    (station->connect_stop_pending &&
		    (station->connect_retry_deadline == 0U ||
		    deadline_expired(now, station->connect_retry_deadline))) ||
		    deadline_expired(now, station->beacon_watch_deadline) ||
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
/*
 * Attaches a station with a test clock.
 */
int
wlan_station_test_attach(
	struct net_device *device,
	const struct wlan_radio_ops *ops,
	void *radio_context,
	const struct wlan_scan_profile *scan_profile,
	wlan_clock_fn clock,
	void *clock_context,
	struct wlan_station **result)
{
	unsigned long enabled;
	int error;

	/* Attaches the station the ordinary way first. */
	error = wlan_station_attach(device, ops, radio_context,
	    scan_profile, result);
	if (error != 0)
		return error;

	/* Substitutes the caller's clock for the default one. */
	enabled = spin_lock_irqsave(&(*result)->lock);
	if (clock != NULL)
		(*result)->clock = clock;
	else
		(*result)->clock = default_clock;
	(*result)->clock_context = clock_context;
	spin_unlock_irqrestore(&(*result)->lock, enabled);

	/* Reports the attached station to the caller. */
	return 0;
}

/*
 * Installs a one-shot hook that runs on the next scan BSS report.
 */
int
wlan_station_test_set_report_hook(
	struct wlan_station *station,
	wlan_station_test_hook_fn hook,
	void *context)
{
	unsigned long enabled;
	int error;

	/* Holds a reference for as long as the hook is installed. */
	error = station_enter(station);
	if (error != 0)
		return error;

	/* Installs the hook under the station lock. */
	enabled = spin_lock_irqsave(&station->lock);
	station->test_report_hook = hook;
	station->test_report_hook_context = context;
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Drops the reference and reports success. */
	station_leave(station);
	return 0;
}

/*
 * Reports how many callers are waiting for the control gate.
 */
unsigned
wlan_station_test_control_waiters(
	struct wlan_station *station)
{
	unsigned long enabled;
	unsigned waiters;

	/* Reports no waiters when the caller names no station. */
	if (station == NULL)
		return 0U;

	/* Samples the waiter count under the station lock. */
	enabled = spin_lock_irqsave(&station->lock);
	waiters = station->test_control_waiters;
	spin_unlock_irqrestore(&station->lock, enabled);
	return waiters;
}

/*
 * Tests whether every secret of a station has been erased.
 */
int
wlan_station_test_secrets_clear(
	struct wlan_station *station)
{
	unsigned long enabled;
	int clear;

	/* Treats a missing station as holding no secrets. */
	if (station == NULL)
		return 1;

	/* Tests every secret the station can hold. */
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

/*
 * Places a station directly in the authorized state for a test.
 */
int
wlan_station_test_seed_authorized(
	struct wlan_station *station,
	const struct wlan_bss_record *bss,
	uint64_t generation,
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

	/* Rejects an invalid BSS or generation. */
	if (station == NULL ||
	    bss == NULL ||
	    generation == 0U ||
	    key_generation == 0U ||
	    key_generation == UINT64_MAX ||
	    !bssid_valid(bss->bssid) ||
	    bss->ssid_length == 0U ||
	    bss->ssid_length > WLAN_SSID_MAX ||
	    channel_frequency(bss->channel) == 0U)
		return EINVAL;
	group_generation = key_generation + 1U;
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used || !station->administrative_up || station->closing) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENETDOWN;
	}

	/* Seeds the connection and receive state. */
	now = station_now_locked(station);
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

	/* Seeds an authorized WPA2 engine with fixed keys. */
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
	station->wpa2.connected_lifetime = 1U;
	station->wpa2.gtk_index = 1U;
	station->wpa2.protocol_version = 2U;
	memset(&station->wpa2.profile, 0, sizeof(station->wpa2.profile));
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

	/* Publishes the connected state and raises the carrier. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Starts a pairwise rekey on an authorized station for a test.
 */
int
wlan_station_test_begin_pairwise_rekey(
	struct wlan_station *station)
{
	unsigned long enabled;
	int error;

	/* Rejects a call that names no station. */
	if (station == NULL)
		return EINVAL;

	/* Refuses a rekey unless the link is authorized and keyed. */
	enabled = spin_lock_irqsave(&station->lock);
	if (wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTHORIZED ||
	    !station->wpa2.connected_lifetime ||
	    !station->wpa2.pairwise_installed) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}

	/* Closes the controlled port and waits for message 3. */
	error = station_carrier_down_locked(station);
	if (error == 0) {
		station->wpa2.authorized = 0U;
		station->wpa2.pairwise_rekey = 1U;
		station->wpa2.state = WLAN_WPA2_STATE_MESSAGE_3;
		station_sync_wpa_locked(station);
	}
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Starts a group rekey on an authorized station for a test.
 */
int
wlan_station_test_begin_group_rekey(
	struct wlan_station *station)
{
	unsigned long enabled;

	/* Rejects a call that names no station. */
	if (station == NULL)
		return EINVAL;

	/* Refuses a rekey unless both key types are installed. */
	enabled = spin_lock_irqsave(&station->lock);
	if (wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTHORIZED ||
	    !station->wpa2.connected_lifetime ||
	    !station->wpa2.authorized ||
	    !station->wpa2.pairwise_installed ||
	    !station->wpa2.group_installed) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}

	/* Rewinds the engine to the group handshake. */
	station->wpa2.tx_cookie_active = 0U;
	station->wpa2.state = WLAN_WPA2_STATE_GROUP_MESSAGE_2_TX;
	station->wpa2.step_deadline_ticks = deadline_after(
	    station_now_locked(station), WLAN_CONNECT_TRANSITION_TICKS);
	station_sync_wpa_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports that the rekey is under way. */
	return 0;
}

/*
 * Advances a station that is still authenticating to a later handshake
 * phase for a test.
 */
int
wlan_station_test_set_initial_phase(
	struct wlan_station *station,
	uint32_t phase)
{
	unsigned long enabled;

	/* Rejects a call that names no station or no known phase. */
	if (station == NULL ||
	    (phase != WLAN_STATION_TEST_PHASE_ASSOCIATING &&
	    phase != WLAN_STATION_TEST_PHASE_FOUR_WAY))
		return EINVAL;

	/* Refuses a phase change once the link is past authentication. */
	enabled = spin_lock_irqsave(&station->lock);
	if ((wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTH_TX &&
	    wlan_wpa2_engine_state(&station->wpa2) !=
	    WLAN_WPA2_STATE_AUTH_RESPONSE) || station->wpa2.connected_lifetime) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}

	/* Places the engine in the requested phase. */
	station->wpa2.tx_cookie_active = 0U;
	station->wpa2.associated = phase == WLAN_STATION_TEST_PHASE_FOUR_WAY;
	if (phase == WLAN_STATION_TEST_PHASE_ASSOCIATING)
		station->wpa2.state = WLAN_WPA2_STATE_ASSOC_RESPONSE;
	else
		station->wpa2.state = WLAN_WPA2_STATE_MESSAGE_3;
	station->wpa2.step_deadline_ticks = deadline_after(
	    station_now_locked(station), WLAN_CONNECT_TRANSITION_TICKS);
	station_sync_wpa_locked(station);
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports that the phase is in place. */
	return 0;
}

/*
 * Completes a handshake in progress as authorized for a test.
 */
int
wlan_station_test_complete_authorized(
	struct wlan_station *station,
	uint64_t key_generation)
{
	unsigned long enabled;
	uint64_t group_generation;
	int error;

	/* Rejects a call that names no station or no usable generation. */
	if (station == NULL ||
	    key_generation == 0U ||
	    key_generation == UINT64_MAX)
		return EINVAL;

	/* Derives the group generation from the pairwise one. */
	group_generation = key_generation + 1U;

	/* Refuses to authorize a link no connect attempt is driving. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->connect_driver_active ||
	    wlan_wpa2_engine_state(&station->wpa2) == WLAN_WPA2_STATE_IDLE ||
	    wlan_wpa2_engine_state(&station->wpa2) == WLAN_WPA2_STATE_FAILED) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}

	/* Installs fixed keys and publishes the authorized state. */
	station->wpa2.key_generation = key_generation;
	station->wpa2.group_key_generation = group_generation;
	station->wpa2.next_key_generation = group_generation;
	station->wpa2.gtk_index = 1U;
	station->wpa2.associated = 1U;
	station->wpa2.pairwise_installed = 1U;
	station->wpa2.group_installed = 1U;
	station->wpa2.authorized = 1U;
	station->wpa2.connected_lifetime = 1U;
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Copies the connection state of a station for a test.
 */
int
wlan_station_test_snapshot(
	struct wlan_station *station,
	struct wlan_station_test_snapshot *snapshot)
{
	unsigned long enabled;

	/* Rejects a call that names no station or no snapshot. */
	if (station == NULL || snapshot == NULL)
		return EINVAL;

	/* Copies the whole station state out under one lock hold. */
	enabled = spin_lock_irqsave(&station->lock);
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->connection_generation = station->connection_generation;
	snapshot->connection_deadline = station->connection_deadline;
	snapshot->connection_step_deadline = station->connection_step_deadline;
	snapshot->reconnect_deadline = 0U;
	snapshot->reconnect_next_attempt = 0U;
	snapshot->reconnect_cleanup_retry = 0U;
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
	snapshot->reconnect_attempts = 0U;
	snapshot->state = station->state;
	snapshot->wpa_state = (uint32_t)station->wpa2.state;
	snapshot->association_capability = station->wpa2.profile.capability;
	snapshot->reconnect_pending = 0U;
	snapshot->reconnect_scan_active = 0U;
	snapshot->controlled_port = station->controlled_port != 0U;
	snapshot->connect_driver_active = station->connect_driver_active != 0;
	snapshot->connect_stop_pending = station->connect_stop_pending != 0;
	snapshot->connect_retire_explicit =
	    station->connect_retire_explicit != 0;
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports the filled snapshot to the caller. */
	return 0;
}

/*
 * Transmits an EAPOL frame through the station's WPA2 transmit path for
 * a test.
 */
int
wlan_station_test_transmit_eapol(
	struct wlan_station *station,
	uint64_t cookie,
	const uint8_t *frame,
	size_t length)
{
	unsigned long enabled;
	uint64_t generation;
	uint64_t deadline;
	int error;

	/* Rejects a call that names no station or no frame. */
	if (station == NULL || cookie == 0U || frame == NULL || length == 0U)
		return EINVAL;

	/* Holds the station and the control gate across the transmit. */
	error = station_enter(station);
	if (error != 0)
		return error;
	station_control_enter(station);

	/* Samples the generation and deadline the frame belongs to. */
	enabled = spin_lock_irqsave(&station->lock);
	generation = station->connection_generation;
	deadline = deadline_local(station_now_locked(station),
	    WLAN_CONNECT_TRANSITION_TICKS, station->connection_deadline);
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Sends the frame and releases what the transmit held. */
	error = station_wpa_transmit(station, generation, cookie,
	    WLAN_WPA2_TX_EAPOL, station->selected.bssid, frame, length, deadline);
	station_control_leave(station);
	station_leave(station);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
#endif

/* Clears memory in a way the compiler cannot elide. */
static void
secure_zero(
	void *memory,
	size_t length)
{
	volatile uint8_t *bytes;

	bytes = memory;
	while (length != 0U) {
		*bytes = 0U;
		bytes++;
		length--;
	}
}

/* Reads the kernel clock for a station without a test clock. */
static uint64_t
default_clock(
	void *context)
{
	(void)context;
	return clock_ticks();
}

/* Adds a delay to a tick count, saturating at the current time on overflow. */
static uint64_t
deadline_after(
	uint64_t now,
	uint64_t delta)
{
	if (UINT64_MAX - now < delta)
		return now;
	return now + delta;
}

/* Adds a delay to a tick count, failing on overflow. */
static int
deadline_checked(
	uint64_t now,
	uint64_t delta,
	uint64_t *result)
{
	if (result == NULL || UINT64_MAX - now < delta)
		return EOVERFLOW;
	*result = now + delta;
	return 0;
}

/* Computes a step deadline bounded by an overall deadline. */
static uint64_t
deadline_local(
	uint64_t now,
	uint64_t delta,
	uint64_t total_deadline)
{
	uint64_t local;

	local = deadline_after(now, delta);
	if (total_deadline != 0U && total_deadline < local)
		return total_deadline;
	return local;
}

/* Converts a beacon interval into the ticks of tolerated beacon silence. */
static uint64_t
station_beacon_watch_ticks(
	uint16_t beacon_interval_tu)
{
	uint64_t microseconds;
	uint64_t ticks;

	/*
	 * One TU is 1024 microseconds.  Twenty missed beacons tolerates
	 * ordinary scheduling/airtime jitter; the two/ten-second bounds keep
	 * both common intervals and malformed/extreme advertisements finite
	 * and conservative.
	 */
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

/* Restarts the beacon watch from now; the caller holds the station lock. */
static void
station_beacon_watch_refresh_locked(
	struct wlan_station *station,
	uint64_t now)
{
	station->beacon_watch_deadline = deadline_after(now,
	    station_beacon_watch_ticks(station->selected.beacon_interval_tu));
}

/* Tests whether the beacon watch applies; the caller holds the station lock. */
static int
station_beacon_watch_active_locked(
	const struct wlan_station *station)
{
	if (!station->connect_driver_active ||
	    station->beacon_watch_deadline == 0U)
		return 0;

	/*
	 * The common state and nonzero latch are published under this lock
	 * after authorization.  They remain valid through pairwise
	 * (FOUR_WAY) and group (CONNECTED) rekey.  Avoid reading
	 * engine-private fields here: beacon ingestion does not own the
	 * serialized WPA control gate.
	 */
	if (station->state == WLAN_STATE_CONNECTED)
		return 1;
	if (station->state == WLAN_STATE_FOUR_WAY)
		return 1;
	return 0;
}

/* Tests whether a nonzero deadline has passed. */
static int
deadline_expired(
	uint64_t now,
	uint64_t deadline)
{
	if (deadline == 0U)
		return 0;
	if (now < deadline)
		return 0;
	return 1;
}

/* Wakes the network worker when one is linked in. */
static void
wlan_worker_wakeup(
	void)
{
	if (net_worker_wakeup != NULL)
		net_worker_wakeup();
}

/* Tests whether a byte range is all zero. */
static int
bytes_zero(
	const void *memory,
	size_t length)
{
	const uint8_t *bytes;

	/* Walks the block and stops at the first non-zero byte. */
	bytes = memory;
	while (length != 0U) {
		if (*bytes != 0U)
			return 0;
		bytes++;
		length--;
	}

	/* Reports that every byte was zero. */
	return 1;
}

/* Maps a channel number to its center frequency, or zero when unknown. */
static uint32_t
channel_frequency(
	uint32_t channel)
{
	if (channel >= 1U && channel <= 13U)
		return 2407U + 5U * channel;
	if (channel == 14U)
		return 2484U;
	if ((channel >= 36U && channel <= 144U &&
	    (channel - 36U) % 4U == 0U) ||
	    (channel >= 149U && channel <= 181U &&
	    (channel - 149U) % 4U == 0U))
		return 5000U + 5U * channel;
	return 0U;
}

/* Validates a scan profile's channels, flags, and padding. */
static int
scan_profile_validate(
	const struct wlan_scan_profile *profile)
{
	uint32_t index;
	uint32_t earlier;
	const struct wlan_scan_channel *channel;

	/* Rejects an empty or oversized profile with nonzero padding. */
	if (profile == NULL ||
	    profile->channel_count == 0U ||
	    profile->channel_count > WLAN_SCAN_CHANNEL_MAX ||
	    !bytes_zero(profile->reserved, sizeof(profile->reserved)))
		return EINVAL;

	/* Every used channel must be known, consistent, and unique. */
	for (index = 0U; index < WLAN_SCAN_CHANNEL_MAX; index++) {
		channel = &profile->channels[index];
		if (index >= profile->channel_count) {
			if (!bytes_zero(channel, sizeof(*channel)))
				return EINVAL;
			continue;
		}
		if (channel_frequency(channel->channel) == 0U ||
		    channel->center_frequency_mhz !=
		    channel_frequency(channel->channel) ||
		    (channel->flags & ~WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED) != 0U ||
		    channel->reserved != 0U)
			return EINVAL;
		for (earlier = 0U; earlier < index; earlier++) {
			if (profile->channels[earlier].channel == channel->channel)
				return EINVAL;
		}
	}
	return 0;
}

/* Computes the overall scan budget for a channel count. */
static uint64_t
scan_deadline_ticks(
	uint32_t channel_count)
{
	if (channel_count <= 14U)
		return WLAN_SCAN_DEADLINE_TICKS;
	return WLAN_SCAN_DEADLINE_TICKS +
	    (uint64_t)(channel_count - 14U) *
	    (WLAN_SCAN_TUNE_DEADLINE_TICKS + WLAN_SCAN_DWELL_TICKS);
}

/* Tests whether an ioctl names a device. */
static int
device_name_matches(
	const struct net_device *device,
	const char *name)
{
	unsigned index;

	/* Compares the names byte by byte up to the terminator. */
	for (index = 0; index < IFNAMSIZ; index++) {
		if ((uint8_t)device->name[index] != (uint8_t)name[index])
			return 0;
		if (name[index] == '\0')
			return 1;
	}

	/* Reports no match when the name fills the field without ending. */
	return 0;
}

/* Validates an ioctl header against the device and the expected size. */
static int
header_validate(
	const struct net_device *device,
	const struct wlan_ioctl_header *header,
	size_t size)
{
	if (device == NULL || header == NULL)
		return ENODEV;
	if (header->version != WLAN_ABI_VERSION || header->size != size)
		return EINVAL;
	if (!device_name_matches(device, header->ifr_name))
		return ENODEV;
	return 0;
}

/* Reads the station clock; the caller holds the station lock. */
static uint64_t
station_now_locked(
	struct wlan_station *station)
{
	return station->clock(station->clock_context);
}

/* Allocates the next operation generation; the caller holds the station lock. */
static int
station_generation_locked(
	struct wlan_station *station,
	uint64_t *result)
{
	if (station->next_generation == UINT64_MAX)
		return EOVERFLOW;
	station->next_generation++;
	if (station->next_generation == 0U)
		return EOVERFLOW;
	*result = station->next_generation;
	return 0;
}

/* Erases the connection state and credential; the caller holds the station lock. */
static void
station_clear_connection_locked(
	struct wlan_station *station)
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
	station->connect_start_pending = 0;
	memset(&station->selected, 0, sizeof(station->selected));
}

/* Completes a connection retirement and settles the station state; the caller holds the station lock. */
static void
station_finish_connection_retire_locked(
	struct wlan_station *station)
{
	int explicit_retire;

	explicit_retire = station->connect_retire_explicit;

	/* Drops every connection artifact. */
	station_clear_connection_locked(station);
	memset(&station->l2_rx, 0, sizeof(station->l2_rx));
	station->transmit_packet_number = 0U;
	station->transmit_cookie = 0U;
	station->connect_driver_active = 0;
	station->connect_stop_pending = 0;
	station->connect_retire_explicit = 0;
	station->connect_retry_deadline = 0U;

	/* An explicit retirement ends idle; an implicit one ends failed. */
	if (explicit_retire) {
		if (station->scan_driver_active) {
			if (station->administrative_up)
				station->state = WLAN_STATE_FAILED;
			else
				station->state = WLAN_STATE_DOWN;
			if (station->scan_error != 0)
				station->terminal_error = station->scan_error;
			else
				station->terminal_error = EBUSY;
		} else {
			if (station->administrative_up)
				station->state = WLAN_STATE_IDLE;
			else
				station->state = WLAN_STATE_DOWN;
			station->terminal_error = 0;
		}
	} else {
		if (station->administrative_up)
			station->state = WLAN_STATE_FAILED;
		else
			station->state = WLAN_STATE_DOWN;
	}
}

/* Closes the controlled port and drops the carrier; the caller holds the station lock. */
static int
station_carrier_down_locked(
	struct wlan_station *station)
{
	int error;

	station->controlled_port = 0U;
	error = net_device_set_carrier(station->device, 0);

	/*
	 * A removed referenced device already has no public carrier.  This
	 * proves only carrier absence, never the success of a key or radio
	 * inverse.
	 */
	if (error == ENODEV && station->device != NULL &&
	    !net_device_carrier(station->device))
		error = 0;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Computes the deadline of one WPA2 transition within the connection budget. */
static uint64_t
station_wpa_deadline(
	struct wlan_station *station)
{
	unsigned long enabled;
	uint64_t deadline;
	uint64_t now;

	/* Derives the step deadline from the connection deadline. */
	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);
	deadline = deadline_local(now, WLAN_CONNECT_TRANSITION_TICKS,
	    station->connection_deadline);
	spin_unlock_irqrestore(&station->lock, enabled);
	return deadline;
}

/* Computes the deadline of a WPA2 cleanup step. */
static uint64_t
station_wpa_cleanup_deadline(
	struct wlan_station *station)
{
	unsigned long enabled;
	uint64_t deadline;

	/*
	 * Cleanup is a safety barrier, not another handshake attempt.  It
	 * gets a fresh finite budget even when the 30-second protocol budget
	 * expired; otherwise an uncertain CAM write could never be proven
	 * absent.
	 */
	enabled = spin_lock_irqsave(&station->lock);
	deadline = deadline_after(station_now_locked(station),
	    WLAN_CONNECT_TRANSITION_TICKS);
	spin_unlock_irqrestore(&station->lock, enabled);
	return deadline;
}

/* Mirrors the WPA2 engine state into the station state; the caller holds the station lock. */
static void
station_sync_wpa_locked(
	struct wlan_station *station)
{
	enum wlan_wpa2_state state;

	state = wlan_wpa2_engine_state(&station->wpa2);

	/* Copies the engine's progress flags. */
	station->connection_step_deadline =
	    wlan_wpa2_engine_next_deadline(&station->wpa2);
	station->authenticated = 0U;
	station->associated = station->wpa2.associated != 0U;
	station->key_installed = station->wpa2.pairwise_installed != 0U &&
	    station->wpa2.group_installed != 0U;
	station->controlled_port = station->wpa2.authorized != 0U;
	station->retry_count = station->wpa2.retry_count;

	/* Maps the engine state to the visible connection state. */
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
	case WLAN_WPA2_STATE_AUTHORIZED:
		station->authenticated = 1U;
		station->associated = 1U;
		station->key_installed = 1U;
		station->controlled_port = 1U;
		station->state = WLAN_STATE_CONNECTED;
		station->terminal_error = 0;
		station->connection_deadline = 0U;
		if (station->beacon_watch_deadline == 0U)
			station_beacon_watch_refresh_locked(station,
			    station_now_locked(station));
		break;
	case WLAN_WPA2_STATE_FAILED:
		station->terminal_error = wlan_wpa2_engine_last_error(
		    &station->wpa2);
		station->state = WLAN_STATE_FAILED;
		station->connect_stop_pending = 1;
		station->connect_retry_deadline = deadline_after(
		    station_now_locked(station), 1U);
		break;
	case WLAN_WPA2_STATE_IDLE:
	default:
		break;
	}
}

/* Fills a buffer with platform entropy for the WPA2 engine. */
static int
station_wpa_entropy_fill(
	void *context,
	void *buffer,
	size_t length)
{
	(void)context;
	if (hal_entropy_fill == NULL)
		return EIO;
	if (!hal_entropy_fill(buffer, length))
		return EIO;
	return 0;
}

/* Starts the radio on the selected BSS for the WPA2 engine. */
static int
station_wpa_radio_start(
	void *context,
	uint64_t generation,
	const uint8_t bssid[6],
	uint32_t channel,
	uint64_t deadline,
	uint64_t *completion_ticks)
{
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	station = context;

	/* The request must still describe the selected BSS and generation. */
	if (station == NULL ||
	    bssid == NULL ||
	    completion_ticks == NULL ||
	    station->ops->connect_start == NULL)
		return EOPNOTSUPP;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->selected.channel != channel ||
	    memcmp(station->selected.bssid, bssid, 6U) != 0) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}
	if (station->connection_generation != generation) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Starts the driver and records that a stop is now owed. */
	error = station->ops->connect_start(station->radio_context, generation,
	    &station->selected, deadline);
	*completion_ticks = station->clock(station->clock_context);
	if (error == 0) {
		enabled = spin_lock_irqsave(&station->lock);
		if (station->connection_generation == generation)
			station->connect_driver_active = 1;
		else
			error = ESTALE;
		spin_unlock_irqrestore(&station->lock, enabled);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Transmits a management or EAPOL frame for the WPA2 engine. */
static int
station_wpa_transmit(
	void *context,
	uint64_t generation,
	uint64_t cookie,
	enum wlan_wpa2_tx_kind kind,
	const uint8_t destination[6],
	const uint8_t *frame,
	size_t length,
	uint64_t deadline)
{
	struct wlan_station *station;
	struct wlan_radio_tx_request request;
	uint8_t mpdu[WLAN_L2_MPDU_MAX];
	const uint8_t *wire_frame;
	size_t wire_length;
	unsigned long enabled;
	uint64_t key_generation;
	uint64_t packet_number;
	int protected_frame;
	int error;
	uint8_t ethernet[WLAN_L2_ETHERNET_HEADER_SIZE +
	    WLAN_WPA2_EAPOL_FRAME_MAX];

	/* Starts out unprotected and with no key generation. */
	station = context;
	wire_frame = frame;
	wire_length = length;
	key_generation = 0U;
	packet_number = 0U;
	protected_frame = 0;

	/* Refuses a transmit the radio cannot carry out. */
	if (station == NULL ||
	    destination == NULL ||
	    frame == NULL ||
	    station->ops->frame_transmit == NULL)
		return EOPNOTSUPP;

	/* Classifies the frame the way the radio expects it. */
	memset(&request, 0, sizeof(request));
	if (kind == WLAN_WPA2_TX_MANAGEMENT) {
		request.frame_class = WLAN_RADIO_FRAME_MANAGEMENT;
	} else if (kind == WLAN_WPA2_TX_EAPOL) {
		if (length > WLAN_WPA2_EAPOL_FRAME_MAX)
			return EMSGSIZE;

		/*
		 * The initial four-way exchange is clear.  Once a pairwise
		 * key is active, group-key responses, pairwise-rekey M2/M4,
		 * and later retransmissions use the active (never staged)
		 * generation.  This is independent of the controlled port,
		 * which is intentionally closed during pairwise rekey.
		 */
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

		/* Wraps the EAPOL payload in an Ethernet header and a data MPDU. */
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

	/* Hands the frame to the radio and erases the working copies. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Records the association in the radio for the WPA2 engine. */
static int
station_wpa_association_set(
	void *context,
	uint64_t generation,
	const uint8_t bssid[6],
	uint16_t aid)
{
	struct wlan_station *station;
	int error;

	/* Refuses the call when the radio offers no association hook. */
	station = context;
	if (station == NULL || station->ops->association_set == NULL)
		return EOPNOTSUPP;

	/* Asks the radio to record the association. */
	error = station->ops->association_set(station->radio_context,
	    generation, bssid, aid, station_wpa_deadline(station));

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Clears the association in the radio for the WPA2 engine. */
static int
station_wpa_association_clear(
	void *context,
	uint64_t generation)
{
	struct wlan_station *station;
	int error;

	/* Refuses the call when the radio offers no teardown hook. */
	station = context;
	if (station == NULL || station->ops->association_clear == NULL)
		return EOPNOTSUPP;

	/* Asks the radio to drop the association. */
	error = station->ops->association_clear(station->radio_context,
	    generation, station_wpa_cleanup_deadline(station));

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Installs a pairwise or group key in the radio for the WPA2 engine. */
static int
station_wpa_key_install(
	void *context,
	uint64_t generation,
	enum wlan_wpa2_key_kind kind,
	uint8_t key_index,
	const uint8_t key[16],
	uint64_t key_generation,
	uint64_t receive_packet_number)
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	struct wlan_station *station;
	struct wlan_radio_key_request request;
	unsigned long enabled;
	int error;

	station = context;

	/* Rejects an unsupported key description. */
	if (station == NULL || key == NULL || station->ops->key_install == NULL)
		return EOPNOTSUPP;
	if (generation == 0U ||
	    key_generation == 0U ||
	    key_index > 3U ||
	    receive_packet_number > 0x0000ffffffffffffULL ||
	    (kind != WLAN_WPA2_KEY_PAIRWISE &&
	    kind != WLAN_WPA2_KEY_GROUP) ||
	    (kind == WLAN_WPA2_KEY_PAIRWISE && key_index != 0U))
		return EINVAL;

	/* Builds the radio request. */
	memset(&request, 0, sizeof(request));
	request.generation = generation;
	request.key_generation = key_generation;
	request.deadline_ticks = station_wpa_deadline(station);
	request.receive_packet_number = receive_packet_number;
	if (kind == WLAN_WPA2_KEY_PAIRWISE)
		request.kind = WLAN_RADIO_KEY_PAIRWISE;
	else
		request.kind = WLAN_RADIO_KEY_GROUP;
	request.key_index = key_index;
	if (kind == WLAN_WPA2_KEY_PAIRWISE)
		memcpy(request.address, station->selected.bssid, 6U);
	else
		memcpy(request.address, broadcast, 6U);
	memcpy(request.key, key, sizeof(request.key));

	/* A key that is not staged takes effect on the receive path now. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Activates staged keys in the radio and on the receive path for the WPA2 engine. */
static int
station_wpa_keys_activate(
	void *context,
	uint64_t generation,
	uint64_t pairwise_key_generation,
	uint64_t group_key_generation)
{
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	station = context;

	/* Refuses to activate keys the caller did not fully name. */
	if (station == NULL ||
	    pairwise_key_generation == 0U ||
	    group_key_generation == 0U ||
	    station->ops->keys_activate == NULL)
		return EOPNOTSUPP;

	/* Asks the radio to install both keys at once. */
	error = station->ops->keys_activate(station->radio_context, generation,
	    pairwise_key_generation, group_key_generation,
	    station_wpa_deadline(station));
	if (error != 0)
		return error;

	/* Publishes the new generations and resets the counters they own. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Raises the receive replay floor of a key for the WPA2 engine. */
static int
station_wpa_key_receive_pn_advance(
	void *context,
	uint64_t generation,
	enum wlan_wpa2_key_kind kind,
	uint8_t key_index,
	uint64_t key_generation,
	uint64_t receive_packet_number)
{
	struct wlan_station *station;
	unsigned long enabled;
	uint64_t *floor;
	uint64_t current_generation;
	int error;

	station = context;
	error = 0;

	/* Rejects a key the caller did not name within range. */
	if (station == NULL ||
	    generation == 0U ||
	    key_generation == 0U ||
	    key_index > 3U ||
	    (kind != WLAN_WPA2_KEY_PAIRWISE && kind != WLAN_WPA2_KEY_GROUP) ||
	    (kind == WLAN_WPA2_KEY_PAIRWISE && key_index != 0U) ||
	    receive_packet_number > 0x0000ffffffffffffULL)
		return EINVAL;

	/* Reads the generation the receive path currently holds. */
	enabled = spin_lock_irqsave(&station->lock);
	if (kind == WLAN_WPA2_KEY_PAIRWISE)
		current_generation = station->l2_rx.pairwise_key_generation;
	else
		current_generation = station->l2_rx.group_key_generation[key_index];
	if (station->connection_generation != generation) {
		error = ESTALE;
	} else if (kind == WLAN_WPA2_KEY_GROUP &&
	    station->wpa2.pending_group_installed &&
	    station->wpa2.pending_group_key_generation == key_generation &&
	    station->wpa2.pending_gtk_index == key_index) {
		/* The staged RSC is published atomically by keys_activate(). */
		error = 0;
	} else if (current_generation != key_generation) {
		error = ESTALE;
	} else {
		if (kind == WLAN_WPA2_KEY_PAIRWISE)
			floor = &station->l2_rx.pairwise_packet_number;
		else
			floor = &station->l2_rx.group_packet_number[key_index];

		/*
		 * RX may already have advanced beyond a freshly sampled AP
		 * RSC.  This barrier is therefore max-assignment, never a
		 * reset.
		 */
		if (receive_packet_number > *floor)
			*floor = receive_packet_number;
	}
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Deletes a key from the radio and the receive path for the WPA2 engine. */
static int
station_wpa_key_delete(
	void *context,
	uint64_t generation,
	enum wlan_wpa2_key_kind kind,
	uint8_t key_index,
	uint64_t key_generation)
{
	struct wlan_station *station;
	unsigned long enabled;
	enum wlan_radio_key_kind radio_kind;
	int error;

	station = context;

	/* Refuses the call when the radio offers no delete hook. */
	if (station == NULL || station->ops->key_delete == NULL)
		return EOPNOTSUPP;

	/* Rejects a key the caller did not name within range. */
	if (generation == 0U ||
	    key_generation == 0U ||
	    key_index > 3U ||
	    (kind != WLAN_WPA2_KEY_PAIRWISE &&
	    kind != WLAN_WPA2_KEY_GROUP) ||
	    (kind == WLAN_WPA2_KEY_PAIRWISE && key_index != 0U))
		return EINVAL;

	/* Asks the radio to drop the key it named. */
	if (kind == WLAN_WPA2_KEY_PAIRWISE)
		radio_kind = WLAN_RADIO_KEY_PAIRWISE;
	else
		radio_kind = WLAN_RADIO_KEY_GROUP;
	error = station->ops->key_delete(station->radio_context, generation,
	    radio_kind, key_index, key_generation,
	    station_wpa_cleanup_deadline(station));

	/* Forgets the generation on the receive path once the radio has. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Opens or closes the controlled port for the WPA2 engine. */
static int
station_wpa_authorized_set(
	void *context,
	uint64_t generation,
	int authorized)
{
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	station = context;

	/* Rejects a call that names no station or no valid state. */
	if (station == NULL || (authorized != 0 && authorized != 1))
		return EINVAL;

	/* Ignores a request that belongs to an older connection. */
	enabled = spin_lock_irqsave(&station->lock);
	if (station->connection_generation != generation) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ESTALE;
	}

	/* The port opens only with both keys installed. */
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Stops the radio for the WPA2 engine. */
static int
station_wpa_radio_stop(
	void *context,
	uint64_t generation)
{
	struct wlan_station *station;
	unsigned long enabled;
	int error;

	station = context;

	/* Refuses the call when the radio offers no disconnect hook. */
	if (station == NULL || station->ops->disconnect == NULL)
		return EOPNOTSUPP;

	/* Retires the connect driver once the radio has stopped. */
	error = station->ops->disconnect(station->radio_context, generation);
	if (error == 0) {
		enabled = spin_lock_irqsave(&station->lock);
		if (station->connection_generation == generation)
			station->connect_driver_active = 0;
		spin_unlock_irqrestore(&station->lock, enabled);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Terminates one connection generation after a link loss; the caller holds the control gate. */
static int
station_link_lost_controlled(
	struct wlan_station *station,
	uint64_t generation,
	int reason)
{
	unsigned long enabled;
	int carrier_error;
	int error;

	/*
	 * A link-loss report terminates exactly one connection generation;
	 * only a later userspace request may start another.
	 */
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
	if (wlan_wpa2_engine_state(&station->wpa2) == WLAN_WPA2_STATE_IDLE) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENOTCONN;
	}

	/* Fails the connection, then stops the engine. */
	carrier_error = station_carrier_down_locked(station);
	station->terminal_error = reason;
	station->state = WLAN_STATE_FAILED;
	station->connect_retire_explicit = 0;
	station->connection_deadline = 0U;
	station->connection_step_deadline = 0U;
	station->beacon_watch_deadline = 0U;
	station->connect_stop_pending = 1;
	station->connect_retry_deadline = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
	error = wlan_wpa2_engine_stop(&station->wpa2);

	/* Retires now, or leaves a stop pending for the timer. */
	enabled = spin_lock_irqsave(&station->lock);
	if (error == 0 && !station->connect_driver_active) {
		station_finish_connection_retire_locked(station);
	} else {
		station->connect_stop_pending = 1;
		station->connect_retry_deadline = deadline_after(
		    station_now_locked(station), 1U);
	}
	station->state = WLAN_STATE_FAILED;
	station->terminal_error = reason;
	spin_unlock_irqrestore(&station->lock, enabled);
	if (error != 0)
		return error;
	return carrier_error;
}

/* Takes an active reference on a station that accepts callers. */
static int
station_enter(
	struct wlan_station *station)
{
	unsigned long enabled;

	/* Rejects a call that names no station. */
	if (station == NULL)
		return ENODEV;

	/* Refuses a station that is closing or otherwise unusable. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->used || station->blocked || station->closing || station->stop_pending) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return ENODEV;
	}

	/* Refuses a reference the counter cannot hold. */
	if (station->active == UINT_MAX) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return EOVERFLOW;
	}

	/* Takes the reference and reports success. */
	station->active++;
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

/* Drops an active reference on a station. */
static void
station_leave(
	struct wlan_station *station)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&station->lock);
	if (station->active == 0U)
		__builtin_trap();
	station->active--;
	spin_unlock_irqrestore(&station->lock, enabled);
}

/* Takes the serial control gate of a station, yielding while it is held. */
static void
station_control_enter(
	struct wlan_station *station)
{
#ifdef WLAN_TESTING
	int waiting;
#endif
	unsigned long enabled;

#ifdef WLAN_TESTING
	waiting = 0;
#endif

	/*
	 * Control methods may sleep in a bus driver and therefore cannot run
	 * under the station spinlock.  This thread-context serial gate stays
	 * held from the state mutation through start/stop completion: a
	 * cancellation barrier can never return before an earlier start
	 * method has itself returned.
	 */
	for (;;) {
		enabled = spin_lock_irqsave(&station->lock);
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

/* Releases the control gate of a station. */
static void
station_control_leave(
	struct wlan_station *station)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&station->lock);
	if (!station->control_inflight)
		__builtin_trap();
	station->control_inflight = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
}

/* Finds the station of a device and takes an active reference on it. */
static int
station_find_enter(
	struct net_device *device,
	struct wlan_station **result)
{
	unsigned long registry_enabled;
	unsigned index;
	int error;
	struct wlan_station *station;
	unsigned long enabled;

	error = EOPNOTSUPP;

	if (device == NULL || result == NULL)
		return ENODEV;

	/* Rechecks the slot under its own lock before admitting the caller. */
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		station = &wlan_stations[index];
		if (!station->used || station->device != device)
			continue;
		enabled = spin_lock_irqsave(&station->lock);
		if (station->used &&
		    !station->blocked &&
		    !station->closing &&
		    !station->stop_pending &&
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Takes an active reference on the station of a slot index. */
static int
station_index_enter(
	unsigned index,
	struct wlan_station **result)
{
	struct wlan_station *station;
	unsigned long registry_enabled;
	unsigned long enabled;
	int error;

	/* Starts out reporting that no station was found. */
	error = ENODEV;

	/* Rejects an index outside the registry. */
	if (index >= NET_DEVICE_MAX || result == NULL)
		return ENODEV;

	/* Takes a reference on the station the index names. */
	registry_enabled = spin_lock_irqsave(&wlan_registry_lock);
	station = &wlan_stations[index];
	if (station->used) {
		enabled = spin_lock_irqsave(&station->lock);
		if (station->used && !station->blocked && !station->closing &&
		    !station->stop_pending) {
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Orders two BSSIDs. */
static int
bssid_compare(
	const uint8_t left[6],
	const uint8_t right[6])
{
	return memcmp(left, right, 6U);
}

/* Tests whether a BSSID is a nonzero unicast address. */
static int
bssid_valid(
	const uint8_t bssid[6])
{
	unsigned index;
	unsigned nonzero;

	/* Rejects a group address. */
	nonzero = 0U;
	if ((bssid[0] & 0x01U) != 0U)
		return 0;

	/* Rejects the all-zero address. */
	for (index = 0; index < 6U; index++)
		nonzero |= bssid[index];
	if (nonzero == 0U)
		return 0;

	/* Reports that the address names one station. */
	return 1;
}

/* Builds a probe request, directed at the selected SSID or broadcast. */
static size_t
probe_request_build(
	const struct wlan_station *station,
	int directed,
	uint32_t channel,
	uint8_t frame[WLAN_PROBE_REQUEST_MAX_SIZE])
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	static const uint8_t rates_24[4] = { 0x82U, 0x84U, 0x8bU, 0x96U };
	static const uint8_t rates_5[8] = {
		0x8cU, 0x12U, 0x98U, 0x24U, 0xb0U, 0x48U, 0x60U, 0x6cU
	};
	const uint8_t *rates;
	size_t rate_count;
	size_t ssid_length;
	size_t offset;

	/* Bounds the directed form before copying any selected SSID bytes. */
	ssid_length = directed ? station->selected.ssid_length : 0U;
	if (ssid_length > WLAN_SSID_MAX)
		return 0U;

	/* Keeps 2.4-GHz behavior and advertises legacy OFDM rates on 5 GHz. */
	if (channel <= 14U) {
		rates = rates_24;
		rate_count = sizeof(rates_24);
	} else {
		rates = rates_5;
		rate_count = sizeof(rates_5);
	}

	/* Leaves capacity for a maximum-length SSID and all eight OFDM rates. */
	memset(frame, 0, WLAN_PROBE_REQUEST_MAX_SIZE);
	frame[0] = 0x40U;
	memcpy(frame + 4U, broadcast, sizeof(broadcast));
	memcpy(frame + 10U, station->device->hwaddr, 6U);
	memcpy(frame + 16U, broadcast, sizeof(broadcast));
	frame[24] = 0U;
	frame[25] = (uint8_t)ssid_length;
	memcpy(frame + 26U, station->selected.ssid, ssid_length);

	/* Appends the rate element after the complete SSID element. */
	offset = 26U + ssid_length;
	frame[offset++] = 1U;
	frame[offset++] = (uint8_t)rate_count;
	memcpy(frame + offset, rates, rate_count);

	/* Reports the encoded frame length without the unused buffer tail. */
	return offset + rate_count;
}

/* Tests whether the left entry is evicted before the right one. */
static int
cache_entry_worse(
	const struct wlan_cache_entry *left,
	const struct wlan_cache_entry *right)
{
	if (left->bss.rssi_dbm != right->bss.rssi_dbm)
		return left->bss.rssi_dbm < right->bss.rssi_dbm;
	if (left->last_seen != right->last_seen)
		return left->last_seen < right->last_seen;
	return bssid_compare(left->bss.bssid, right->bss.bssid) > 0;
}

/* Adds or refreshes a BSS in the staging cache; the caller holds the station lock. */
static int
cache_insert_locked(
	struct wlan_station *station,
	const struct wlan_bss_record *bss,
	uint64_t now)
{
	struct wlan_cache_entry incoming;
	unsigned index;
	unsigned worst;

	worst = 0U;

	/* A known BSSID is refreshed in place. */
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
		station->staging[station->staging_count] = incoming;
		station->staging_count++;
		return 0;
	}

	/* A full cache evicts its worst entry when the new one is better. */
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

/* Sorts cache entries by BSSID with an insertion sort. */
static void
cache_sort_by_bssid(
	struct wlan_cache_entry *entries,
	uint32_t count)
{
	uint32_t index;
	struct wlan_cache_entry value;
	uint32_t position;

	/* Sorts the cache in place by insertion. */
	for (index = 1U; index < count; index++) {
		value = entries[index];
		position = index;
		while (position != 0U && bssid_compare(
		    entries[position - 1U].bss.bssid, value.bss.bssid) > 0) {
			entries[position] = entries[position - 1U];
			position--;
		}
		entries[position] = value;
	}
}

/* Tests whether a BSS offers exactly the WPA2-PSK CCMP profile supported. */
static int
bss_security_supported(
	const struct wlan_bss_record *bss)
{
	const uint32_t required = WLAN_SECURITY_PRIVACY | WLAN_SECURITY_WPA2 |
	    WLAN_SECURITY_CCMP | WLAN_SECURITY_PSK;
	const uint32_t rejected = WLAN_SECURITY_WPA1 |
	    WLAN_SECURITY_PMF_REQUIRED;
	const uint32_t rejected_suites = WLAN_SECURITY_UNSUPPORTED_SUITE;

	if ((bss->security & required) != required)
		return 0;
	if ((bss->security & (rejected | rejected_suites)) != 0U)
		return 0;
	return 1;
}

/* Picks the strongest supported BSS of an SSID from the snapshot; the caller holds the station lock. */
static int
station_select_bss_locked(
	struct wlan_station *station,
	const uint8_t *ssid,
	uint32_t ssid_length,
	struct wlan_bss_record *result)
{
	uint32_t index;
	int found;
	const struct wlan_bss_record *candidate;

	found = 0;

	/* Prefers the higher RSSI, then the lower BSSID. */
	for (index = 0; index < station->snapshot_count; index++) {
		candidate = &station->snapshot[index].bss;
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
	if (!found)
		return ENOENT;
	return 0;
}

/* Fills the output fields of a scan request; the caller holds the station lock. */
static void
scan_request_output_locked(
	struct wlan_station *station,
	struct wlan_scan_request *request)
{
	request->generation = station->scan_generation;
	request->state = station->scan_state;
	request->terminal_error = station->scan_error;
	memset(request->reserved, 0, sizeof(request->reserved));
}

/* Starts or stops a scan for the scan ioctl. */
static int
ioctl_scan(
	struct wlan_station *station,
	struct wlan_scan_request *request)
{
	unsigned long enabled;
	uint64_t generation;
	uint64_t deadline;
	uint64_t now;
	int wake_start;
	int error;

	/* Starts out with no waiter to wake. */
	wake_start = 0;

	/* Rejects a request with reserved fields or an unknown action. */
	if (request->flags != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)) ||
	    (request->action != WLAN_SCAN_START &&
	    request->action != WLAN_SCAN_STOP))
		return EINVAL;

	/* Serializes the request against the other control paths. */
	station_control_enter(station);
	enabled = spin_lock_irqsave(&station->lock);

	/* A start arms a new scan generation for the timer to run. */
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
		error = deadline_checked(now, scan_deadline_ticks(
		    station->scan_profile.channel_count),
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

	/* A stop cancels the scan and leaves the driver stop to the timer. */
	if (station->scan_state != WLAN_SCAN_RUNNING &&
	    !station->scan_driver_active) {
		error = 0;
		goto output;
	}
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
		station->scan_retry_deadline = 0U;
		error = 0;
		goto output;
	}
	station->scan_retry_deadline = station_now_locked(station);
	wake_start = 1;
	error = 0;
output:
	scan_request_output_locked(station, request);
	spin_unlock_irqrestore(&station->lock, enabled);
	station_control_leave(station);
	if (wake_start)
		wlan_worker_wakeup();

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the scan and snapshot state for the scan status ioctl. */
static int
ioctl_scan_status(
	struct wlan_station *station,
	struct wlan_scan_status_request *request)
{
	unsigned long enabled;

	/* Rejects a request with reserved fields set. */
	if (!bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;

	/* Copies the scan state out under one lock hold. */
	enabled = spin_lock_irqsave(&station->lock);
	request->generation = station->snapshot_generation;
	request->scan_generation = station->scan_generation;
	request->cache_sequence = station->cache_sequence;
	if (station->scan_state == WLAN_SCAN_RUNNING)
		request->deadline_ticks = station->scan_deadline;
	else
		request->deadline_ticks = 0U;
	request->state = station->scan_state;
	request->terminal_error = station->scan_error;
	request->result_count = station->snapshot_count;
	request->truncated = station->snapshot_truncated;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Reports the filled request to the caller. */
	return 0;
}

/* Converts the age of a cache entry to saturated milliseconds. */
static uint32_t
entry_age_ms(
	uint64_t now,
	uint64_t last_seen)
{
	uint64_t ticks;

	/* Treats a clock that ran backwards as no elapsed time. */
	if (now >= last_seen)
		ticks = now - last_seen;
	else
		ticks = 0U;

	/* Saturates an age the result cannot hold. */
	if (ticks > (uint64_t)UINT32_MAX / 10U)
		return UINT32_MAX;
	return (uint32_t)(ticks * 10U);
}

/* Copies one snapshot entry for the BSS ioctl. */
static int
ioctl_bss(
	struct wlan_station *station,
	struct wlan_bss_request *request)
{
	unsigned long enabled;

	if (request->reserved0 != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;

	/* The request must name the published snapshot and a valid index. */
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

/* Arms a connection to the best BSS of an SSID for the connect ioctl. */
static int
ioctl_connect(
	struct wlan_station *station,
	struct wlan_connect_request *request)
{
	uint8_t credential[WLAN_PASSPHRASE_STORAGE];
	struct wlan_bss_record selected;
	unsigned long enabled;
	uint64_t generation;
	uint64_t deadline;
	int control_entered;
	int error;

	generation = 0U;
	deadline = 0U;
	control_entered = 0;

	/* Takes the passphrase out of the request before validating it. */
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

	/* An idle station with a snapshot match starts a new generation. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->administrative_up) {
		error = ENETDOWN;
		goto output_locked;
	}
	if (station->ops->connect_start == NULL) {
		error = EOPNOTSUPP;
		goto output_locked;
	}
	if (station->scan_driver_active ||
	    station->connect_driver_active ||
	    station->connect_start_pending ||
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
	if (wlan_wpa2_engine_state(&station->wpa2) != WLAN_WPA2_STATE_IDLE) {
		error = EBUSY;
		goto output_locked;
	}

	/* Records the selection for the timer to start. */
	station_clear_connection_locked(station);
	station->selected = selected;
	memcpy(station->credential, credential, sizeof(station->credential));
	station->credential_length = request->passphrase_length;
	station->operation_generation = generation;
	station->connection_generation = generation;
	station->connection_deadline = deadline;
	station->state = WLAN_STATE_AUTHENTICATING;
	station->terminal_error = 0;
	station->connect_start_pending = 1;
	station->connect_driver_active = 0;
	station->connect_stop_pending = 0;
	station->connect_retire_explicit = 0;
	station->connect_retry_deadline = 0U;
	station->transmit_packet_number = 0U;
	station->transmit_cookie = 0U;
	memset(&station->l2_rx, 0, sizeof(station->l2_rx));
	error = 0;
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

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Stops the scan and connection of a station; the caller holds the control gate. */
static int
station_retire_controlled(
	struct wlan_station *station,
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
	int scan_error;
	int connection_error;
	int error;

	scan_error = 0;
	connection_error = 0;

	/* Cancels everything under the lock and notes what to stop. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!keep_administrative_up)
		station->administrative_up = 0U;
	station->connect_start_pending = 0;
	secure_zero(station->credential, sizeof(station->credential));
	station->credential_length = 0U;
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
	if (station->administrative_up)
		station->state = WLAN_STATE_DISCONNECTING;
	else
		station->state = WLAN_STATE_DOWN;
	station->terminal_error = 0;
#ifdef WLAN_TESTING
	station->test_report_hook = NULL;
	station->test_report_hook_context = NULL;
#endif
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Stops the scan and the engine outside the lock. */
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

	/*
	 * engine_stop() owns the key/association/radio ordering.  A direct
	 * radio stop is only a fallback for an otherwise-idle engine; it
	 * must never run past an uncertain key-delete barrier.
	 */
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

	/* Settles the final state and schedules retries for what failed. */
	enabled = spin_lock_irqsave(&station->lock);
	if (scan_error == 0) {
		station->scan_driver_active = 0;
	} else if (scan_error != EBUSY) {
		station->scan_state = WLAN_SCAN_FAILED;
		station->scan_error = scan_error;
	}
	if (connection_error == 0 && !station->connect_driver_active) {
		station_finish_connection_retire_locked(station);
	} else {
		station->connect_stop_pending = 1;
		station->connect_retry_deadline = deadline_after(
		    station_now_locked(station), 1U);
	}
	if (scan_error != 0)
		station->terminal_error = scan_error;
	else if (connection_error != 0)
		station->terminal_error = connection_error;
	else
		station->terminal_error = carrier_error;
	if (station->terminal_error != 0) {
		if (station->administrative_up)
			station->state = WLAN_STATE_FAILED;
		else
			station->state = WLAN_STATE_DOWN;
	}
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

	/* Reports the first failure in scan, connection, carrier order. */
	if (scan_error != 0)
		return scan_error;
	if (connection_error != 0)
		return connection_error;
	return carrier_error;
}

/* Stops the scan and connection of a station under the control gate. */
static int
station_retire(
	struct wlan_station *station,
	int keep_administrative_up)
{
	int error;

	station_control_enter(station);
	error = station_retire_controlled(station, keep_administrative_up);
	station_control_leave(station);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Retires the connection for the disconnect ioctl. */
static int
ioctl_disconnect(
	struct wlan_station *station,
	struct wlan_disconnect_request *request)
{
	unsigned long enabled;
	uint64_t generation;
	int error;

	if (request->flags != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;
	station_control_enter(station);

	/* The disconnect takes its own operation generation. */
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

	/* Retires while keeping the station administratively up. */
	error = station_retire_controlled(station, 1);
	enabled = spin_lock_irqsave(&station->lock);
	if (station->administrative_up) {
		if (error == 0)
			station->state = WLAN_STATE_IDLE;
		else
			station->state = WLAN_STATE_FAILED;
	}
	request->generation = generation;
	request->state = station->state;
	request->terminal_error = error;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
	station_control_leave(station);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Registry ownership protects a read-only snapshot without delaying stop. */
static int
station_status_device(struct net_device *device, struct wlan_status_request *request)
{
	unsigned long enabled;
	unsigned index;
	int error;

	error = EOPNOTSUPP;
	enabled = spin_lock_irqsave(&wlan_registry_lock);
	for (index = 0U; index < NET_DEVICE_MAX; index++) {
		if (wlan_stations[index].used && wlan_stations[index].device == device) {
			error = ioctl_status(&wlan_stations[index], request);
			break;
		}
	}
	spin_unlock_irqrestore(&wlan_registry_lock, enabled);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports the station state for the status ioctl. */
static int
ioctl_status(
	struct wlan_station *station,
	struct wlan_status_request *request)
{
	unsigned long enabled;

	/* Rejects a request with reserved fields set. */
	if (request->reserved0 != 0U ||
	    !bytes_zero(request->reserved, sizeof(request->reserved)))
		return EINVAL;

	/* Reports the generations the station currently holds. */
	enabled = spin_lock_irqsave(&station->lock);
	request->operation_generation = station->operation_generation;
	request->scan_generation = station->scan_generation;
	request->snapshot_generation = station->snapshot_generation;

	/* Reports the deadline of whichever operation is in progress. */
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
	if (station->terminal_error != 0)
		request->terminal_error = station->terminal_error;
	else if (station->scan_state == WLAN_SCAN_FAILED)
		request->terminal_error = station->scan_error;
	else
		request->terminal_error = 0;
	request->rssi_dbm = station->selected.rssi_dbm;
	memcpy(request->bssid, station->selected.bssid,
	    sizeof(request->bssid));
	request->channel = station->selected.channel;
	request->reserved0 = 0U;
	request->center_frequency_mhz =
	    station->selected.center_frequency_mhz;
	request->security = station->selected.security;
	request->stop_flags = station->stop_pending ? WLAN_STATUS_STOP_PENDING : 0U;
	request->stop_error = station->stop_error;
	if (station->stop_pending) {
		request->state = WLAN_STATE_DISCONNECTING;
		request->administrative_up = 0U;
		request->controlled_port = 0U;
	}
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
	return 0;
}

/* Frees a station slot and returns the device to release; the caller holds the station lock. */
static struct net_device *
station_finalize_locked(
	struct wlan_station *station)
{
	struct net_device *device;

	/* Keeps the device to hand back once the slot is free. */
	device = station->device;

	/* Retires the connection and erases every secret it held. */
	station->state = WLAN_STATE_REMOVED;
	station_clear_connection_locked(station);
	secure_zero(station->staging, sizeof(station->staging));
	secure_zero(station->snapshot, sizeof(station->snapshot));
	secure_zero(&station->scan_profile, sizeof(station->scan_profile));
	station->staging_count = 0U;
	station->snapshot_count = 0U;

	/* Releases the slot for a later attach. */
	station->device = NULL;
	station->ops = NULL;
	station->radio_context = NULL;
	station->clock = NULL;
	station->clock_context = NULL;
	station->lifecycle_inflight = 0;
	station->used = 0;

	/* Hands the detached device back to the caller. */
	return device;
}

/* Fails the running scan; the caller holds the station lock. */
static void
station_scan_failed_locked(
	struct wlan_station *station,
	int error)
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

/* Publishes the staging cache as the new snapshot; the caller holds the station lock. */
static int
station_scan_publish_locked(
	struct wlan_station *station,
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

/* Applies the result of a driver scan stop. */
static void
station_scan_stop_result(
	struct wlan_station *station,
	uint64_t generation,
	int error)
{
	unsigned long enabled;
	uint64_t now;

	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);

	/* A successful stop publishes a finished scan; a failed one retries. */
	if (station->scan_generation == generation) {
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
			/* An asynchronous abort still in flight is retryable, not scan failure. */
			if (station->scan_publish_pending && error != EBUSY)
				station_scan_failed_locked(station, error);
			station->scan_retry_deadline = deadline_after(now, 1U);
		}
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (error != 0)
		wlan_worker_wakeup();
}

/* Starts the WPA2 engine on a pending connection. */
static void
station_connection_start(
	struct wlan_station *station)
{
	static const uint8_t supported_rates_24[12] = {
		0x82U, 0x84U, 0x8bU, 0x96U,
		0x0cU, 0x12U, 0x18U, 0x24U,
		0x30U, 0x48U, 0x60U, 0x6cU
	};
	static const uint8_t supported_rates_5[8] = {
		0x8cU, 0x12U, 0x98U, 0x24U,
		0xb0U, 0x48U, 0x60U, 0x6cU
	};
	uint8_t credential[WLAN_PASSPHRASE_STORAGE];
	struct wlan_bss_record selected;
	struct wlan_wpa2_profile profile;
	unsigned long enabled;
	uint64_t generation;
	uint64_t deadline;
	size_t credential_length;
	int error;

	/* Takes the pending selection and credential out of the station. */
	enabled = spin_lock_irqsave(&station->lock);
	if (!station->connect_start_pending) {
		spin_unlock_irqrestore(&station->lock, enabled);
		return;
	}
	station->connect_start_pending = 0;
	generation = station->connection_generation;
	deadline = station->connection_deadline;
	selected = station->selected;
	credential_length = station->credential_length;
	memcpy(credential, station->credential, sizeof(credential));
	secure_zero(station->credential, sizeof(station->credential));
	station->credential_length = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);

	/* Builds the engine profile for the selected band. */
	memset(&profile, 0, sizeof(profile));
	memcpy(profile.station, station->device->hwaddr,
	    sizeof(profile.station));
	memcpy(profile.bssid, selected.bssid, sizeof(profile.bssid));
	memcpy(profile.ssid, selected.ssid, selected.ssid_length);
	profile.ssid_length = selected.ssid_length;
	if (selected.channel <= 14U) {
		memcpy(profile.rates, supported_rates_24,
		    sizeof(supported_rates_24));
		profile.rate_count = sizeof(supported_rates_24);
	} else {
		memcpy(profile.rates, supported_rates_5,
		    sizeof(supported_rates_5));
		profile.rate_count = sizeof(supported_rates_5);
	}
	profile.channel = selected.channel;
	profile.capability = WLAN_LOCAL_ASSOC_CAPABILITY;
	if (selected.channel > 14U)
		profile.capability |= WLAN_ASSOC_CAPABILITY_SHORT_SLOT_TIME;
	profile.listen_interval = 1U;
	profile.passphrase = credential;
	profile.passphrase_length = credential_length;
	profile.total_deadline_ticks = deadline;
	profile.transition_timeout_ticks = WLAN_CONNECT_TRANSITION_TICKS;
	profile.recovery_timeout_ticks = WLAN_CONNECT_DEADLINE_TICKS;
	error = wlan_wpa2_engine_start(&station->wpa2, generation, &profile,
	    station->clock(station->clock_context));
	wlan_crypto_erase(&profile, sizeof(profile));
	secure_zero(credential, sizeof(credential));

	/* Publishes the engine state; an idle engine after a failure fails. */
	enabled = spin_lock_irqsave(&station->lock);
	if (station->connection_generation == generation) {
		station_sync_wpa_locked(station);
		if (error != 0 && wlan_wpa2_engine_state(&station->wpa2) ==
		    WLAN_WPA2_STATE_IDLE) {
			station->state = WLAN_STATE_FAILED;
			station->terminal_error = error;
		}
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (error != 0)
		wlan_worker_wakeup();
}

/* Drives the connection state machine from the timer. */
static void
station_connection_timer(
	struct wlan_station *station,
	uint64_t now)
{
	unsigned long enabled;
	uint64_t generation;
	enum wlan_wpa2_state wpa_state;
	int cleanup_due;
	int stop;
	int error;

	generation = 0U;
	cleanup_due = 0;
	stop = 0;
	error = 0;

	/* A pending start runs first. */
	enabled = spin_lock_irqsave(&station->lock);
	if (station->connect_start_pending) {
		spin_unlock_irqrestore(&station->lock, enabled);
		station_connection_start(station);
		return;
	}

	/* An expired beacon watch is a link loss. */
	wpa_state = wlan_wpa2_engine_state(&station->wpa2);
	if (deadline_expired(station_now_locked(station),
	    station->beacon_watch_deadline) &&
	    station->wpa2.connected_lifetime) {
		generation = station->connection_generation;
		station->beacon_watch_deadline = 0U;
		spin_unlock_irqrestore(&station->lock, enabled);
		error = station_link_lost_controlled(station, generation,
		    ETIMEDOUT);
		wlan_worker_wakeup();
		(void)error;
		return;
	}
	spin_unlock_irqrestore(&station->lock, enabled);

	/* A live engine runs its own timer. */
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

	/* A failed engine is stopped once its retry deadline passes. */
	if (wpa_state == WLAN_WPA2_STATE_FAILED) {
		enabled = spin_lock_irqsave(&station->lock);
		now = station_now_locked(station);
		cleanup_due = station->connect_stop_pending &&
		    (station->connect_retry_deadline == 0U ||
		    deadline_expired(now, station->connect_retry_deadline));
		spin_unlock_irqrestore(&station->lock, enabled);
	}
	if (wpa_state == WLAN_WPA2_STATE_FAILED && cleanup_due) {
		error = wlan_wpa2_engine_stop(&station->wpa2);
		enabled = spin_lock_irqsave(&station->lock);
		if (error == 0) {
			if (!station->connect_driver_active) {
				station_finish_connection_retire_locked(station);
			} else {
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

	/* An idle engine with a driver still active owes the radio a stop. */
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

/* Drives the scan state machine from the timer. */
static void
station_scan_timer(
	struct wlan_station *station,
	uint64_t now)
{
	unsigned iteration;
	unsigned long enabled;
	uint64_t generation;
	uint64_t deadline;
	uint32_t step;
	uint32_t channel;
	int active_probe;
	int action;
	int error;
	uint8_t probe[WLAN_PROBE_REQUEST_MAX_SIZE];
	size_t probe_length;

	/*
	 * Two immediate transitions per channel plus terminal stop are
	 * bounded; the loop also consumes a ready/error synchronously
	 * reported by a fake or radio callback without relying on another
	 * edge wakeup.
	 */
	for (iteration = 0U;
	    iteration < WLAN_SCAN_CHANNEL_MAX * 2U + 4U; iteration++) {
		generation = 0U;
		deadline = 0U;
		step = 0U;
		channel = 0U;
		active_probe = 0;
		action = 0;
		probe_length = 0U;

		/* Decides the next action under the lock. */
		enabled = spin_lock_irqsave(&station->lock);
		now = station_now_locked(station);
		if (station->scan_state == WLAN_SCAN_RUNNING) {
			generation = station->scan_generation;
			if (deadline_expired(now, station->scan_deadline)) {
				station_scan_failed_locked(station, ETIMEDOUT);
				if (station->scan_driver_active)
					action = 3;
				else
					action = 0;
			} else if (station->scan_event_error != 0) {
				error = station->scan_event_error;
				station_scan_failed_locked(station, error);
				if (station->scan_driver_active)
					action = 3;
				else
					action = 0;
			} else if (station->scan_publish_pending && station->scan_driver_active &&
			    deadline_expired(now, station->scan_retry_deadline)) {
				/*
				 * Final async stop can be busy while the scan
				 * stays RUNNING.  It must retry before any
				 * snapshot can become COMPLETE.
				 */
				action = 4;
			} else if (station->scan_step_state ==
			    WLAN_SCAN_STEP_NEED_TUNE) {
				step = station->scan_step_index;
				channel = station->scan_profile.channels[step].channel;
				deadline = deadline_local(now,
				    WLAN_SCAN_TUNE_DEADLINE_TICKS,
				    station->scan_deadline);
				station->scan_step_deadline = deadline;
				station->scan_step_state = WLAN_SCAN_STEP_TUNING;

				/*
				 * Once start is invoked, stop is the mandatory
				 * producer barrier even when start itself
				 * reports an error.
				 */
				station->scan_driver_active = 1;
				action = 1;
			} else if (station->scan_step_state ==
			    WLAN_SCAN_STEP_TUNING) {
				if (deadline_expired(now,
				    station->scan_step_deadline)) {
					station_scan_failed_locked(station,
					    ETIMEDOUT);
					if (station->scan_driver_active)
						action = 3;
					else
						action = 0;
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
						/* Uses the tuned channel independently of cached BSS state. */
						channel = station->scan_profile.channels[
						    station->scan_step_index].channel;
						probe_length = probe_request_build(station, 0,
						    channel, probe);
						action = 2;
					}
				}
			} else if (station->scan_step_state ==
			    WLAN_SCAN_STEP_DWELL && deadline_expired(now,
			    station->scan_step_deadline)) {
				if (station->scan_step_index + 1U <
				    station->scan_profile.channel_count) {
					station->scan_step_index++;
					station->scan_step_state =
					    WLAN_SCAN_STEP_NEED_TUNE;
					station->scan_step_deadline = 0U;
				} else {
					station->scan_step_state =
					    WLAN_SCAN_STEP_NONE;
					station->scan_step_deadline = 0U;
					station->scan_publish_pending = 1U;
					if (station->scan_driver_active)
						action = 4;
					else
						action = 5;
				}
			}
		} else if (station->scan_driver_active &&
		    (station->scan_retry_deadline == 0U ||
		    deadline_expired(now, station->scan_retry_deadline))) {
			generation = station->scan_generation;
			action = 3;
		}
		spin_unlock_irqrestore(&station->lock, enabled);

		/* Advancing a completed dwell to NEED_TUNE is immediate. */
		if (action == 0) {
			enabled = spin_lock_irqsave(&station->lock);
			active_probe = station->scan_state == WLAN_SCAN_RUNNING &&
			    station->scan_step_state == WLAN_SCAN_STEP_NEED_TUNE;
			spin_unlock_irqrestore(&station->lock, enabled);
			if (active_probe)
				continue;
			return;
		}

		/* Tunes the radio; a failed start still needs the stop barrier. */
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

		/* Sends the active probe request. */
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

		/* Publishes a finished scan without a driver stop. */
		if (action == 5) {
			enabled = spin_lock_irqsave(&station->lock);
			if (station->scan_generation == generation)
				(void)station_scan_publish_locked(station,
				    generation);
			spin_unlock_irqrestore(&station->lock, enabled);
			return;
		}

		/* Stops the driver and applies the result. */
		error = station->ops->scan_stop(station->radio_context,
		    generation);
		station_scan_stop_result(station, generation, error);
		return;
	}
}

/* Runs both timers of a station under the control gate. */
static void
station_timer_run(
	struct wlan_station *station,
	uint64_t now)
{
	station_control_enter(station);
	station_connection_timer(station, now);
	station_scan_timer(station, now);
	station_control_leave(station);
}
