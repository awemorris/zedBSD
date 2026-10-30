/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_KERN_NET_WLAN_H
#define KERN_KERN_NET_WLAN_H

#include <stddef.h>
#include <stdint.h>
#include <kern/clock.h>
#include <uapi/wlan.h>

#define WLAN_SCAN_DEADLINE_TICKS kern_ms_to_ticks(15000U)
#define WLAN_SCAN_TUNE_DEADLINE_TICKS kern_ms_to_ticks(500U)
/* Software scans listen for 100 ms actively, or 200 ms without probes. */
#define WLAN_SCAN_DWELL_TICKS kern_ms_to_ticks(100U)
#define WLAN_SCAN_PASSIVE_DWELL_TICKS kern_ms_to_ticks(200U)
/* Firmware completion is required; this is a timeout, never a dwell delay. */
#define WLAN_SCAN_OFFLOAD_DEADLINE_TICKS kern_ms_to_ticks(1000U)
#define WLAN_CONNECT_DEADLINE_TICKS kern_ms_to_ticks(30000U)
#define WLAN_CONNECT_TRANSITION_TICKS kern_ms_to_ticks(1000U)
#define WLAN_MANAGEMENT_FRAME_MAX 2304U

#define WLAN_SCAN_CHANNEL_MAX 51U
#define WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED 0x00000001U
/* The driver reports completion after firmware finishes the channel scan. */
#define WLAN_SCAN_CHANNEL_OFFLOADED_DWELL 0x00000002U

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

struct net_device;
struct packet_buf;
struct wlan_station;

#define WLAN_FULLMAC_PMK_LENGTH 32U

/*
 * One connection a FullMAC radio is asked to make.
 *
 * The common core derives the PMK from the passphrase and owns every value
 * here.  The driver copies the request into its own slot before
 * fullmac_connect returns, keeps the PMK only until it has handed it to the
 * firmware, and then erases its copy; the core erases this request as soon as
 * the call returns.
 */
struct wlan_fullmac_connect_request {
	uint64_t generation;
	uint64_t deadline_ticks;
	struct wlan_bss_record bss;
	uint8_t pmk[WLAN_FULLMAC_PMK_LENGTH];
};

/*
 * The firmware's own progress of a FullMAC connection.
 *
 * ASSOCIATED means the firmware has joined the access point, and AUTHORIZED
 * means its supplicant has finished the four-way and group handshakes.  A
 * failure or a disconnection is reported with wlan_station_report_link_loss.
 */
enum wlan_fullmac_event {
	WLAN_FULLMAC_EVENT_ASSOCIATED = 1,
	WLAN_FULLMAC_EVENT_AUTHORIZED = 2
};

typedef uint64_t (*wlan_clock_fn)(void *context);

enum wlan_radio_frame_class {
	WLAN_RADIO_FRAME_MANAGEMENT = 1,
	WLAN_RADIO_FRAME_EAPOL = 2,
	WLAN_RADIO_FRAME_DATA = 3
};

enum wlan_radio_key_kind {
	WLAN_RADIO_KEY_PAIRWISE = 1,
	WLAN_RADIO_KEY_GROUP = 2
};

#define WLAN_RADIO_CIPHER_NONE 0U
#define WLAN_RADIO_CIPHER_CCMP 1U

/* The common core owns every protocol value in this request.  The driver
 * copies the frame before returning and may retain only the scalar generation
 * and cookie needed for its later completion report. */
struct wlan_radio_tx_request {
	uint64_t generation;
	uint64_t cookie;
	uint64_t deadline_ticks;
	uint64_t key_generation;
	uint64_t packet_number;
	enum wlan_radio_frame_class frame_class;
	uint8_t encrypted;
	uint8_t key_index;
	uint8_t reserved[6];
	const uint8_t *frame;
	size_t length;
};

/* A key request is one transaction input and is never retained by a driver.
 * receive_packet_number is the AP-provided initial group receive PN. */
struct wlan_radio_key_request {
	uint64_t generation;
	uint64_t key_generation;
	uint64_t deadline_ticks;
	uint64_t receive_packet_number;
	enum wlan_radio_key_kind kind;
	uint8_t key_index;
	uint8_t address[6];
	uint8_t key[16];
};

/* RX reports are bounded thread/poll-context objects.  For a clear frame all
 * security fields are zero.  A CCMP report carries the key generation chosen
 * by the driver, the 48-bit PN decoded from the MPDU, and hardware integrity
 * status; the common core independently validates the in-frame CCMP header. */
struct wlan_radio_rx_frame {
	uint64_t generation;
	uint64_t key_generation;
	uint64_t packet_number;
	const uint8_t *frame;
	size_t length;
	int32_t rssi_dbm;
	uint8_t channel;
	uint8_t cipher;
	uint8_t decrypted;
	uint8_t key_index;
	uint8_t integrity_error;
	uint8_t reserved[3];
};

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
	int (*association_set)(void *context, uint64_t generation,
		const uint8_t bssid[6], uint16_t aid, uint64_t deadline_ticks);
	int (*association_clear)(void *context, uint64_t generation,
		uint64_t deadline_ticks);
	int (*frame_transmit)(void *context,
		const struct wlan_radio_tx_request *request);
	int (*key_install)(void *context,
		const struct wlan_radio_key_request *request);
	int (*key_delete)(void *context, uint64_t generation,
		enum wlan_radio_key_kind kind, uint8_t key_index,
		uint64_t key_generation, uint64_t deadline_ticks);
	int (*keys_activate)(void *context, uint64_t generation,
		uint64_t pairwise_key_generation,
		uint64_t group_key_generation, uint64_t deadline_ticks);
	int (*quiesce)(void *context);
	/* Independent retirement thread; no common control/active lease is held.
	 * The driver serializes hardware access and retries its complete close.
	 * Success proves hardware stop AND common ownership reconciliation. */
	int (*stop_retry)(void *context);
	/*
	 * Optional.  Independent retirement thread, once a driver asked for it
	 * with wlan_station_restart_request(): no common control/active lease is
	 * held and the station's work pin keeps detach out.  The driver restarts
	 * a hardware epoch it stopped by itself while the device stayed up, so a
	 * failed scan does not leave the interface down until it is cycled
	 * (BUG-158).  The result is the driver's own; common keeps no state.
	 */
	int (*restart)(void *context);
	/*
	 * FullMAC only.  A radio whose firmware runs the association and the
	 * WPA2 supplicant itself sets these two and leaves every SoftMAC
	 * method (connect_start, management_transmit, association_set,
	 * association_clear, frame_transmit, key_install, key_delete,
	 * keys_activate) NULL; it must offer scan_channel_start, scan_stop,
	 * disconnect, quiesce and stop_retry too, and every scan channel must
	 * be OFFLOADED_DWELL.  fullmac_connect copies the request and returns
	 * at once; the result arrives later as report_fullmac_event or
	 * report_link_loss.  ethernet_transmit copies one 802.3 frame of the
	 * open connection generation into the driver's queue and returns at
	 * once (ENOBUFS when the queue is full).
	 */
	int (*fullmac_connect)(void *context,
		const struct wlan_fullmac_connect_request *request);
	int (*ethernet_transmit)(void *context, uint64_t generation,
		const uint8_t *frame, size_t length);
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
/* A driver which learns its regulatory channel set only after firmware start
 * may replace the copied profile while the station is still administratively
 * down.  The input is copied before this call returns. */
int wlan_station_scan_profile_update(struct wlan_station *station,
	const struct wlan_scan_profile *scan_profile);
int wlan_station_open(struct wlan_station *station);
/* Close and detach first block new station operations.  EBUSY is a checked
 * join result: an already admitted callback must retire before retry.  A
 * driver-stop/quiesce error likewise retains the complete object for retry. */
int wlan_station_close(struct wlan_station *station);
/* Driver close publishes intent before joining, then records the result.
 * Failed results arm independent retry; success alone removes pending state. */
void wlan_station_stop_request(struct wlan_station *station);
void wlan_station_stop_complete(struct wlan_station *station, int error);
int wlan_station_stop_busy(struct wlan_station *station);
/* Terminal driver teardown cancels future retries, returning EBUSY while an
 * existing retry owns the station/context. Retry teardown after dropping locks. */
int wlan_station_stop_cancel(struct wlan_station *station);
/* Called only by the independent retirement thread, or a deterministic fixture.
 * Station ownership protects the device and driver context across each callback. */
void wlan_retirement_run(uint64_t now_ticks);
/*
 * Asks the retirement thread to call the driver's restart on its next pass,
 * outside the network worker and every common lease.  Repeated requests before
 * that pass are one restart.
 */
void wlan_station_restart_request(struct wlan_station *station);
/* Holds common admission closed across a driver's checked hardware stop.
 * begin requires every common caller to have returned; EBUSY keeps closing set
 * to refuse new callers. end keeps closing set
 * until wlan_station_close retries the exact outstanding inverses. */
int wlan_station_quiesce_begin(struct wlan_station *station);
void wlan_station_quiesce_end(struct wlan_station *station);
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
/* These methods are IRQ-safe latches: they never invoke driver methods,
 * wait for a control gate, parse frames, or publish a snapshot. */
int wlan_station_report_scan_channel_ready(struct wlan_station *station,
	uint64_t generation, uint32_t step_index);
int wlan_station_report_scan_channel_complete(struct wlan_station *station,
	uint64_t generation, uint32_t step_index);
int wlan_station_report_scan_error(struct wlan_station *station,
	uint64_t generation, int error);
/* Only these common-core entry points can advance a connection.  Drivers
 * cannot publish AUTHORIZED directly: that transition is made after the WPA2
 * engine receives the matching M4 TX acknowledgement.  A FullMAC driver
 * reports its firmware's AUTHORIZED through wlan_station_report_fullmac_event;
 * the common FullMAC engine maps it into the connection state, and the common
 * core still opens the controlled port (the carrier) itself.  The two SoftMAC
 * reports below return EOPNOTSUPP on a FullMAC station. */
int wlan_station_report_frame(struct wlan_station *station,
	const struct wlan_radio_rx_frame *report);
int wlan_station_report_tx_complete(struct wlan_station *station,
	uint64_t generation, uint64_t cookie, int acknowledged, int error);
/* Poll/thread context, FullMAC only (EOPNOTSUPP on a SoftMAC station).  The
 * control gate is taken; an event of an older generation is ESTALE. */
int wlan_station_report_fullmac_event(struct wlan_station *station,
	uint64_t generation, enum wlan_fullmac_event event);
/* Poll/thread context, FullMAC only.  Delivers one received 802.3 frame of
 * the given generation to the device once the controlled port is open; an
 * EAPOL frame (the firmware supplicant's own) is consumed and dropped. */
int wlan_station_report_ethernet(struct wlan_station *station,
	uint64_t generation, const uint8_t *frame, size_t length);
/* Poll/thread-context transport/firmware loss.  It closes carrier and retires
 * the reported generation.  A new connection requires a userspace request. */
int wlan_station_report_link_loss(struct wlan_station *station,
	uint64_t generation, int error);
int wlan_station_transmit(struct wlan_station *station,
	struct packet_buf *packet);

void wlan_timer_run(uint64_t now_ticks);
uint64_t wlan_timer_next_deadline(void);
/* Persistent work predicate for the network worker's final pre-sleep check.
 * START/ready/error remain visible here even if the edge wakeup was lost. */
int wlan_work_pending(void);

#ifdef WLAN_TESTING
typedef void (*wlan_station_test_hook_fn)(void *context);
struct wlan_station_test_snapshot {
	uint64_t connection_generation;
	uint64_t connection_deadline;
	uint64_t connection_step_deadline;
	uint64_t reconnect_deadline;
	uint64_t reconnect_next_attempt;
	uint64_t reconnect_cleanup_retry;
	uint64_t connect_retry_deadline;
	uint64_t scan_retry_deadline;
	uint64_t beacon_watch_deadline;
	uint64_t pairwise_key_generation;
	uint64_t group_key_generation;
	uint64_t pending_pairwise_key_generation;
	uint64_t pending_group_key_generation;
	uint64_t pairwise_receive_packet_number;
	uint64_t group_receive_packet_number[4];
	uint64_t pending_group_receive_packet_number;
	uint64_t transmit_packet_number;
	uint32_t reconnect_attempts;
	uint32_t state;
	uint32_t wpa_state;
	uint16_t association_capability;
	uint8_t reconnect_pending;
	uint8_t reconnect_scan_active;
	uint8_t controlled_port;
	uint8_t connect_driver_active;
	uint8_t connect_stop_pending;
	uint8_t connect_retire_explicit;
	/* The station runs the FullMAC engine; wpa_state then holds its state. */
	uint8_t fullmac;
	uint8_t reserved[1];
};
#define WLAN_STATION_TEST_PHASE_ASSOCIATING 1U
#define WLAN_STATION_TEST_PHASE_FOUR_WAY 2U
int wlan_station_test_set_report_hook(struct wlan_station *station,
	wlan_station_test_hook_fn hook, void *context);
int wlan_station_test_attach(struct net_device *device,
	const struct wlan_radio_ops *ops, void *radio_context,
	const struct wlan_scan_profile *scan_profile, wlan_clock_fn clock,
	void *clock_context, struct wlan_station **result);
unsigned wlan_station_test_control_waiters(struct wlan_station *station);
int wlan_station_test_secrets_clear(struct wlan_station *station);
int wlan_station_test_seed_authorized(struct wlan_station *station,
	const struct wlan_bss_record *bss, uint64_t generation,
	uint64_t key_generation);
int wlan_station_test_begin_pairwise_rekey(struct wlan_station *station);
int wlan_station_test_begin_group_rekey(struct wlan_station *station);
int wlan_station_test_set_initial_phase(struct wlan_station *station,
	uint32_t phase);
int wlan_station_test_complete_authorized(struct wlan_station *station,
	uint64_t key_generation);
int wlan_station_test_snapshot(struct wlan_station *station,
	struct wlan_station_test_snapshot *snapshot);
int wlan_station_test_transmit_eapol(struct wlan_station *station,
	uint64_t cookie, const uint8_t *frame, size_t length);
#endif

#endif
