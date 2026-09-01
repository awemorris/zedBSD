/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_NET_WLAN_H
#define ZEDBSD_KERN_NET_WLAN_H

#include <stddef.h>
#include <stdint.h>
#include <zedbsd/wlan.h>

#define WLAN_SCAN_DEADLINE_TICKS 1500ULL
#define WLAN_SCAN_TUNE_DEADLINE_TICKS 50ULL
#define WLAN_SCAN_DWELL_TICKS 100ULL
#define WLAN_CONNECT_DEADLINE_TICKS 3000ULL
#define WLAN_MANAGEMENT_FRAME_MAX 2304U

#define WLAN_SCAN_CHANNEL_MAX 14U
#define WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED 0x00000001U

struct wlan_scan_channel {
	uint32_t channel;
	uint32_t center_frequency_mhz;
	uint32_t flags;
	uint32_t reserved;
};

/* The profile is copied by attach.  Drivers may release their source copy
 * immediately after attach returns. */
struct wlan_scan_profile {
	uint32_t channel_count;
	uint32_t reserved[3];
	struct wlan_scan_channel channels[WLAN_SCAN_CHANNEL_MAX];
};

#define WLAN_CONNECT_EVENT_AUTHENTICATED 1U
#define WLAN_CONNECT_EVENT_ASSOCIATED    2U
#define WLAN_CONNECT_EVENT_KEY_INSTALLED 3U
#define WLAN_CONNECT_EVENT_AUTHORIZED    4U
#define WLAN_CONNECT_EVENT_RETRY         5U
#define WLAN_CONNECT_EVENT_FAILED        6U
#define WLAN_CONNECT_RETRY_MAX           5U

struct net_device;
struct wlan_station;

typedef uint64_t (*wlan_clock_fn)(void *context);

/* Radio methods run in thread context without the station lock.  Every method
 * must return promptly within a documented driver-owned finite bound and must
 * never wait for network-worker progress.  A method with a deadline must not
 * block past that deadline.  Barrier methods without a deadline (scan_stop,
 * disconnect, and quiesce) must either finish within their driver-owned bound
 * or return an error for checked retry; success synchronously retires every
 * producer covered by that barrier.  No method may retain a BSS, frame, or key
 * pointer after returning. */
struct wlan_radio_ops {
	int (*scan_channel_start)(void *context, uint64_t generation,
		uint32_t step_index, uint32_t channel,
		uint64_t step_deadline_ticks);
	int (*scan_stop)(void *context, uint64_t generation);
	int (*connect_start)(void *context, uint64_t generation,
		const struct wlan_bss_record *bss, uint64_t deadline_ticks);
	int (*disconnect)(void *context, uint64_t generation);
	int (*management_transmit)(void *context, uint64_t generation,
		const uint8_t *frame, size_t length, uint64_t deadline_ticks);
	int (*key_install)(void *context, uint64_t generation,
		uint32_t key_index, const uint8_t *key, size_t length);
	int (*key_delete)(void *context, uint64_t generation,
		uint32_t key_index);
	int (*quiesce)(void *context);
};

void wlan_core_init(void);

/* The caller owns a live device reference while attach runs and must serialize
 * the entire call against net_device_gone(), device destruction, and device
 * shutdown; a live reference preserves the object but does not pin its LIVE
 * state.  On success the station holds its own live reference through
 * detach/shutdown finalization; ops and radio_context must remain valid until
 * that operation succeeds.  Production deadlines use the single clock_ticks()
 * domain. */
int wlan_station_attach(struct net_device *device,
	const struct wlan_radio_ops *ops, void *radio_context,
	const struct wlan_scan_profile *scan_profile,
	struct wlan_station **result);
int wlan_station_open(struct wlan_station *station);
/* Close and detach first block new station operations.  EBUSY is a checked
 * join result: an already admitted callback must retire before retry.  A
 * driver-stop/quiesce error likewise retains the complete object for retry. */
int wlan_station_close(struct wlan_station *station);
int wlan_station_detach(struct wlan_station *station);
/* Terminal shutdown closes every admission gate in one pass and returns
 * EBUSY if an admitted operation still needs to be joined by a retry. */
int wlan_station_shutdown_all(void);

/* This has the exact shape of net_device_ops.ioctl and receives only a
 * kernel-local, exact-size request selected by the INET dispatcher. */
int wlan_station_ioctl(struct net_device *device, unsigned long request,
	void *argument);

int wlan_frame_parse_bss(const uint8_t *frame, size_t length,
	int32_t rssi_dbm, uint8_t channel_hint,
	struct wlan_bss_record *result);

int wlan_station_report_scan_frame(struct wlan_station *station,
	uint64_t generation, const uint8_t *frame, size_t length,
	int32_t rssi_dbm, uint8_t channel_hint);
/* Frame/BSS reports are bounded poll/thread-context ingestion.  A USB/IRQ
 * completion must enqueue a bounded copy and let its poll path call these. */
int wlan_station_report_scan_bss(struct wlan_station *station,
	uint64_t generation, const struct wlan_bss_record *bss);
/* These two methods are IRQ-safe latches: they never invoke driver methods,
 * wait for a control gate, parse frames, or publish a snapshot. */
int wlan_station_report_scan_channel_ready(struct wlan_station *station,
	uint64_t generation, uint32_t step_index);
int wlan_station_report_scan_error(struct wlan_station *station,
	uint64_t generation, int error);
int wlan_station_report_connect_event(struct wlan_station *station,
	uint64_t generation, uint32_t event, int error);

void wlan_timer_run(uint64_t now_ticks);
uint64_t wlan_timer_next_deadline(void);
/* Persistent work predicate for the network worker's final pre-sleep check.
 * START/ready/error remain visible here even if the edge wakeup was lost. */
int wlan_work_pending(void);

#ifdef WLAN_TESTING
typedef void (*wlan_station_test_hook_fn)(void *context);
int wlan_station_test_set_report_hook(struct wlan_station *station,
	wlan_station_test_hook_fn hook, void *context);
int wlan_station_test_attach(struct net_device *device,
	const struct wlan_radio_ops *ops, void *radio_context,
	const struct wlan_scan_profile *scan_profile, wlan_clock_fn clock,
	void *clock_context, struct wlan_station **result);
unsigned wlan_station_test_control_waiters(struct wlan_station *station);
int wlan_station_test_secrets_clear(struct wlan_station *station);
#endif

#endif
