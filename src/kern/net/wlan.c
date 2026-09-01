/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/net/wlan.h"

#include "kern/clock.h"
#include "kern/lock.h"
#include "kern/net/net-device.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

extern void net_worker_wakeup(void) __attribute__((weak));
extern void sched_yield(void) __attribute__((weak));

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
	struct wlan_bss_record selected;
	uint8_t credential[WLAN_PASSPHRASE_STORAGE];
	uint32_t credential_length;

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
	memset(&station->selected, 0, sizeof(station->selected));
}

static int
station_carrier_down_locked(struct wlan_station *station)
{
	station->controlled_port = 0U;
	return net_device_set_carrier(station->device, 0);
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
wildcard_probe_build(const struct wlan_station *station, uint8_t frame[32])
{
	static const uint8_t broadcast[6] = {
		0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
	};
	static const uint8_t rates[4] = { 0x82U, 0x84U, 0x8bU, 0x96U };

	memset(frame, 0, 32U);
	frame[0] = 0x40U;
	memcpy(frame + 4U, broadcast, sizeof(broadcast));
	memcpy(frame + 10U, station->device->hwaddr, 6U);
	memcpy(frame + 16U, broadcast, sizeof(broadcast));
	frame[24] = 0U;
	frame[25] = 0U;
	frame[26] = 1U;
	frame[27] = sizeof(rates);
	memcpy(frame + 28U, rates, sizeof(rates));
	return 32U;
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
	    WLAN_SECURITY_TKIP | WLAN_SECURITY_IEEE8021X |
	    WLAN_SECURITY_SAE | WLAN_SECURITY_PMF_REQUIRED;
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
	    (ops->connect_start != NULL) != (ops->disconnect != NULL))
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
wlan_station_report_connect_event(struct wlan_station *station,
	uint64_t generation, uint32_t event, int error)
{
	unsigned long enabled;
	int carrier_error;
	int wake_worker = 0;
	int result;

	if (event < WLAN_CONNECT_EVENT_AUTHENTICATED ||
	    event > WLAN_CONNECT_EVENT_FAILED || error < 0)
		return EINVAL;
	if (event == WLAN_CONNECT_EVENT_FAILED && error == 0)
		error = EIO;
	if (event != WLAN_CONNECT_EVENT_FAILED && error != 0)
		return EINVAL;
	result = station_enter(station);
	if (result != 0)
		return result;
	enabled = spin_lock_irqsave(&station->lock);
	if (station->connection_generation != generation ||
	    (station->state != WLAN_STATE_AUTHENTICATING &&
	    station->state != WLAN_STATE_ASSOCIATING &&
	    station->state != WLAN_STATE_FOUR_WAY)) {
		result = ESTALE;
	} else if (deadline_expired(station_now_locked(station),
	    station->connection_deadline)) {
		/* The worker performs driver cancellation.  A report callback must
		 * not synchronously enter its producer's disconnect/self-join path. */
		wake_worker = 1;
		result = ETIMEDOUT;
	} else if (event == WLAN_CONNECT_EVENT_FAILED) {
		carrier_error = station_carrier_down_locked(station);
		station->state = WLAN_STATE_FAILED;
		station->terminal_error = carrier_error != 0 ? carrier_error : error;
		station->connect_stop_pending =
		    station->connect_driver_active != 0;
		station_clear_connection_locked(station);
		wake_worker = 1;
		result = carrier_error;
	} else if (event == WLAN_CONNECT_EVENT_RETRY) {
		if (station->retry_count >= WLAN_CONNECT_RETRY_MAX) {
			carrier_error = station_carrier_down_locked(station);
			station->state = WLAN_STATE_FAILED;
			station->terminal_error = carrier_error != 0 ?
			    carrier_error : EOVERFLOW;
			station->connect_stop_pending =
			    station->connect_driver_active != 0;
			station_clear_connection_locked(station);
			station->retry_count = WLAN_CONNECT_RETRY_MAX;
			wake_worker = 1;
			result = carrier_error != 0 ? carrier_error : EOVERFLOW;
		} else {
			station->retry_count++;
			carrier_error = station_carrier_down_locked(station);
			if (carrier_error != 0) {
				station->state = WLAN_STATE_FAILED;
				station->terminal_error = carrier_error;
				station->connect_stop_pending =
				    station->connect_driver_active != 0;
				station_clear_connection_locked(station);
				wake_worker = 1;
				result = carrier_error;
			} else {
				station->authenticated = 0U;
				station->associated = 0U;
				station->key_installed = 0U;
				station->state = WLAN_STATE_AUTHENTICATING;
				result = 0;
			}
		}
	} else if (event == WLAN_CONNECT_EVENT_AUTHENTICATED &&
	    station->state == WLAN_STATE_AUTHENTICATING) {
		station->authenticated = 1U;
		station->state = WLAN_STATE_ASSOCIATING;
		result = 0;
	} else if (event == WLAN_CONNECT_EVENT_ASSOCIATED &&
	    station->state == WLAN_STATE_ASSOCIATING) {
		station->associated = 1U;
		station->state = WLAN_STATE_FOUR_WAY;
		result = 0;
	} else if (event == WLAN_CONNECT_EVENT_KEY_INSTALLED &&
	    station->state == WLAN_STATE_FOUR_WAY && station->associated) {
		station->key_installed = 1U;
		result = 0;
	} else if (event == WLAN_CONNECT_EVENT_AUTHORIZED &&
	    station->state == WLAN_STATE_FOUR_WAY &&
	    station->authenticated && station->associated &&
	    station->key_installed) {
		result = net_device_set_carrier(station->device, 1);
		if (result == 0) {
			station->controlled_port = 1U;
			station->state = WLAN_STATE_CONNECTED;
			station->terminal_error = 0;
			station->connection_deadline = 0U;
			station->retry_count = 0U;
			secure_zero(station->credential,
			    sizeof(station->credential));
			station->credential_length = 0U;
		} else {
			carrier_error = station_carrier_down_locked(station);
			if (carrier_error != 0)
				result = carrier_error;
			station->state = WLAN_STATE_REMOVED;
			station->terminal_error = result;
			station->connect_stop_pending =
			    station->connect_driver_active != 0;
			station_clear_connection_locked(station);
			wake_worker = 1;
		}
	} else {
		result = EINVAL;
	}
	spin_unlock_irqrestore(&station->lock, enabled);
	if (wake_worker)
		wlan_worker_wakeup();
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
	uint8_t credential[WLAN_PASSPHRASE_STORAGE];
	struct wlan_bss_record selected;
	unsigned long enabled;
	uint64_t generation = 0U;
	uint64_t deadline = 0U;
	int control_entered = 0;
	int disconnect_error = 0;
	int stop_connection = 0;
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
	station_clear_connection_locked(station);
	memcpy(station->credential, credential, request->passphrase_length);
	station->credential_length = request->passphrase_length;
	station->selected = selected;
	station->operation_generation = generation;
	station->connection_generation = generation;
	station->connection_deadline = deadline;
	station->state = WLAN_STATE_AUTHENTICATING;
	station->terminal_error = 0;
	station->connect_driver_active = 1;
	spin_unlock_irqrestore(&station->lock, enabled);
	wlan_worker_wakeup();
	error = station->ops->connect_start(station->radio_context, generation,
	    &selected, deadline);
	enabled = spin_lock_irqsave(&station->lock);
	if (error == 0 && station->connection_generation == generation &&
	    (station->state == WLAN_STATE_AUTHENTICATING ||
	    station->state == WLAN_STATE_ASSOCIATING ||
	    station->state == WLAN_STATE_FOUR_WAY) &&
	    deadline_expired(station_now_locked(station), deadline))
		error = ETIMEDOUT;
	if (error != 0 && station->connection_generation == generation) {
		int carrier_error = station_carrier_down_locked(station);

		if (carrier_error != 0)
			error = carrier_error;
		station->state = WLAN_STATE_FAILED;
		station->terminal_error = error;
		stop_connection = station->connect_driver_active;
		station->connect_stop_pending = 0;
		station_clear_connection_locked(station);
	}
	if (error != 0 && stop_connection) {
		spin_unlock_irqrestore(&station->lock, enabled);
		disconnect_error = station->ops->disconnect(
		    station->radio_context, generation);
		enabled = spin_lock_irqsave(&station->lock);
		if (station->connection_generation == generation) {
			if (disconnect_error == 0) {
				station->connect_driver_active = 0;
				station->connect_retry_deadline = 0U;
			} else {
				station->connect_retry_deadline = deadline_after(
				    station_now_locked(station), 1U);
			}
		}
	}
output_locked:
	request->generation = station->connection_generation;
	request->state = station->state;
	request->terminal_error = station->terminal_error;
	memset(request->reserved, 0, sizeof(request->reserved));
	spin_unlock_irqrestore(&station->lock, enabled);
done:
	if (control_entered)
		station_control_leave(station);
	if (disconnect_error != 0)
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
	int carrier_error;
	int scan_error = 0;
	int connection_error = 0;

	enabled = spin_lock_irqsave(&station->lock);
	if (!keep_administrative_up)
		station->administrative_up = 0U;
	carrier_error = station_carrier_down_locked(station);
	scan_generation = station->scan_generation;
	connection_generation = station->connection_generation;
	stop_scan = station->scan_driver_active;
	stop_connection = station->connect_driver_active;
	if (stop_connection)
		station->connect_stop_pending = 0;
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
	station_clear_connection_locked(station);
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
	if (stop_connection) {
		if (station->ops->disconnect == NULL)
			connection_error = EOPNOTSUPP;
		else
			connection_error = station->ops->disconnect(
			    station->radio_context, connection_generation);
	}
	enabled = spin_lock_irqsave(&station->lock);
	if (scan_error == 0)
		station->scan_driver_active = 0;
	else {
		station->scan_state = WLAN_SCAN_FAILED;
		station->scan_error = scan_error;
	}
	if (connection_error == 0) {
		station->connect_driver_active = 0;
		station->connect_stop_pending = 0;
	}
	station->terminal_error = scan_error != 0 ? scan_error :
	    (connection_error != 0 ? connection_error : carrier_error);
	station->state = station->administrative_up ?
	    (station->terminal_error == 0 ? WLAN_STATE_IDLE : WLAN_STATE_FAILED) :
	    WLAN_STATE_DOWN;
	if (station->scan_driver_active && scan_error != 0)
		station->scan_retry_deadline = deadline_after(
		    station_now_locked(station), 1U);
	else if (!station->scan_driver_active)
		station->scan_retry_deadline = 0U;
	if (station->connect_driver_active && connection_error != 0)
		station->connect_retry_deadline = deadline_after(
		    station_now_locked(station), 1U);
	else if (!station->connect_driver_active)
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
station_connection_timer(struct wlan_station *station, uint64_t now)
{
	unsigned long enabled;
	uint64_t generation = 0U;
	int stop = 0;
	int carrier_error = 0;
	int error = 0;

	enabled = spin_lock_irqsave(&station->lock);
	now = station_now_locked(station);
	if (station->connect_stop_pending &&
	    station->connect_driver_active) {
		generation = station->connection_generation;
		station->connect_stop_pending = 0;
		stop = 1;
	} else if ((station->state == WLAN_STATE_AUTHENTICATING ||
	    station->state == WLAN_STATE_ASSOCIATING ||
	    station->state == WLAN_STATE_FOUR_WAY) &&
	    deadline_expired(now, station->connection_deadline)) {
		generation = station->connection_generation;
		stop = station->connect_driver_active;
		carrier_error = station_carrier_down_locked(station);
		station->state = WLAN_STATE_FAILED;
		station->terminal_error = carrier_error != 0 ?
		    carrier_error : ETIMEDOUT;
		station_clear_connection_locked(station);
	} else if (deadline_expired(now, station->connect_retry_deadline) &&
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
		if (error == 0)
			station->connect_driver_active = 0;
		else
			station->connect_retry_deadline = deadline_after(now, 1U);
	}
	if (!station->connect_driver_active)
		station->connect_retry_deadline = 0U;
	spin_unlock_irqrestore(&station->lock, enabled);
	if (error != 0 || carrier_error != 0)
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
		uint8_t probe[32];
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
						probe_length = wildcard_probe_build(
						    station, probe);
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
		if (station->scan_retry_deadline != 0U &&
		    (candidate == 0U ||
		    station->scan_retry_deadline < candidate))
			candidate = station->scan_retry_deadline;
		if (station->connect_retry_deadline != 0U &&
		    (candidate == 0U ||
		    station->connect_retry_deadline < candidate))
			candidate = station->connect_retry_deadline;
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
		    deadline_expired(now, station->connect_retry_deadline) ||
		    station->connect_stop_pending ||
		    ((station->state == WLAN_STATE_AUTHENTICATING ||
		    station->state == WLAN_STATE_ASSOCIATING ||
		    station->state == WLAN_STATE_FOUR_WAY) &&
		    deadline_expired(now, station->connection_deadline));
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
	    bytes_zero(station->credential, sizeof(station->credential));
	spin_unlock_irqrestore(&station->lock, enabled);
	return clear;
}
#endif
