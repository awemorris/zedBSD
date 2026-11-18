/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the device, as the driver's own files see it.
 *
 * Private to src/drivers/wifi/bcm43455/ and its host test.  The radio
 * operations (scan, connect, the FullMAC data path; bcm43455-station.c)
 * build on what is here: the bus, the rings the bus thread fills and
 * drains, the scan and connection slots, and the runtime's start and
 * stop.
 */

#ifndef KERN_DRIVERS_BCM43455_DEVICE_H
#define KERN_DRIVERS_BCM43455_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#include <kern/lock.h>
#include <kern/net/wlan.h>

#include "drivers/wifi/bcm43455/bcm43455.h"
#include "drivers/wifi/bcm43455/bcm43455-bus.h"
#include "drivers/wifi/bcm43455/bcm43455-firmware.h"
#include "drivers/wifi/bcm43455/bcm43455-radio.h"

/* How many received and how many outgoing Ethernet frames wait at most. */
#define BCM43455_RX_RING_SIZE		128U
#define BCM43455_TX_RING_SIZE		64U

/* The most valid channels the firmware's list may hold. */
#define BCM43455_CHANNELS_MAX		64U

/*
 * How many events are logged after each start, so a board's first bring-up
 * shows them.
 */
#define BCM43455_EVENT_LOG_LIMIT	16U

/* The network device's MTU. */
#define BCM43455_MTU			1500U

/* How many networks a scanned channel may report before the poll takes them. */
#define BCM43455_SCAN_RING_SIZE		32U

/* How many restarts in a row a failing chip gets before it is left down. */
#define BCM43455_RESTARTS_MAX		3U

/*
 * One Ethernet frame waiting in a ring, with the connection generation
 * it belongs to: a frame of an older connection is dropped.
 */
struct bcm43455_frame_entry {
	uint64_t generation;
	size_t length;
	uint8_t bytes[BCM43455_BUS_ETHERNET_MAX];
};

/*
 * A bounded queue of frames between the bus thread and the rest of the
 * kernel.
 *
 * entries holds capacity frames from head on, count of them in use; a
 * frame that finds the ring full is dropped and counted.  Guarded by the
 * device's ring_lock.
 */
struct bcm43455_frame_ring {
	struct bcm43455_frame_entry *entries;
	unsigned capacity;
	unsigned head;
	unsigned count;
	uint64_t dropped;
};

/*
 * The firmware's identity, as the running firmware reports it.
 *
 * mac is the chip's address from its OTP, version the firmware's
 * version text, capabilities its feature list, supplicant whether the
 * file's build features name the in-firmware supplicant (idsup) this
 * driver relies on and the running firmware has it (in the list, or by
 * answering its variable sup_wpa), and
 * profile the channels the firmware allows, as the WLAN station scans
 * them.  io_type is how the firmware lays out channel specifications,
 * scan_layout which escan request it takes.
 */
struct bcm43455_identity {
	uint8_t mac[6];
	char version[BCM43455_VERSION_TEXT_MAX];
	char capabilities[BCM43455_BCDC_CONTROL_MAX];
	unsigned supplicant;
	unsigned io_type;
	unsigned scan_layout;
	struct wlan_scan_profile profile;
};

/*
 * The states of the scan slot, one channel at a time.
 *
 * IDLE: no scan.  REQUESTED: the WLAN core asked for a channel and the bus
 * thread has not started it.  ISSUING: the bus thread is sending the
 * escan request.  RUNNING: the firmware took it and scans.  DONE: the
 * channel ended (complete or failed); the next channel may be asked for.
 * STOPPING: scan_stop is aborting it.
 */
enum bcm43455_scan_state {
	BCM43455_SCAN_IDLE = 0,
	BCM43455_SCAN_REQUESTED,
	BCM43455_SCAN_ISSUING,
	BCM43455_SCAN_RUNNING,
	BCM43455_SCAN_DONE,
	BCM43455_SCAN_STOPPING
};

/*
 * The channel being scanned, and what the poll still owes the WLAN core.
 *
 * generation and step are the core's; sync_id names the escan request
 * in the firmware's results.  ready_pending, complete_pending and
 * error_pending are latches the bus thread sets and the poll takes, in
 * that order of report; ready_reported says the core accepts results.
 * firmware_scanning says an escan the firmware took has not ended, so a
 * stop must abort it.  Guarded by the device's ring_lock.
 */
struct bcm43455_scan {
	enum bcm43455_scan_state state;
	uint64_t generation;
	uint32_t step;
	uint32_t channel;
	uint16_t sync_id;
	uint16_t next_sync_id;
	unsigned ready_pending;
	unsigned ready_reported;
	unsigned complete_pending;
	unsigned error_pending;
	int error;
	unsigned firmware_scanning;
};

/*
 * One network a scanned channel found, as the probe response the core
 * parses, with the scan step it belongs to.
 */
struct bcm43455_scan_entry {
	uint64_t generation;
	uint32_t step;
	uint32_t channel;
	int32_t rssi;
	size_t length;
	uint8_t frame[BCM43455_SCAN_FRAME_MAX];
};

/*
 * The networks found and not yet reported.  Guarded by ring_lock; a
 * network that finds it full is dropped and counted.
 */
struct bcm43455_scan_ring {
	struct bcm43455_scan_entry *entries;
	unsigned capacity;
	unsigned head;
	unsigned count;
	uint64_t dropped;
};

/*
 * The states of the connection slot.
 *
 * IDLE: no connection.  REQUESTED: fullmac_connect copied the request and
 * the bus thread has not taken it.  ISSUING: the bus thread is sending the
 * connection's commands.  JOINING: the commands went out and the firmware
 * joins, or has joined; its events tell how it goes.  ENDED: the commands
 * failed and were undone; only the report and disconnect remain.
 */
enum bcm43455_connect_state {
	BCM43455_CONNECT_IDLE = 0,
	BCM43455_CONNECT_REQUESTED,
	BCM43455_CONNECT_ISSUING,
	BCM43455_CONNECT_JOINING,
	BCM43455_CONNECT_ENDED
};

/*
 * The one connection the WLAN core asked for, and its progress.
 *
 * The request (generation, bss, pmk) is copied by fullmac_connect; the
 * PMK stays here only until the bus thread takes it.  cancel is set by
 * disconnect so the bus thread sends no more of the connection's
 * commands.  associated, authorized and lost are the latch of the
 * firmware's events for this generation (no ring, so nothing is lost when
 * the poll is late); the *_reported flags say what the poll already told
 * the core.  Guarded by ring_lock.
 */
struct bcm43455_connection {
	enum bcm43455_connect_state state;
	uint64_t generation;
	struct wlan_bss_record bss;
	uint8_t pmk[BCM43455_PMK_LENGTH];
	unsigned cancel;
	unsigned associated;
	unsigned authorized;
	unsigned lost;
	int lost_error;
	unsigned associated_reported;
	unsigned authorized_reported;
	unsigned lost_reported;
};

/*
 * One BCM43455.
 *
 * Made by bcm43455_device_create() and, once published, alive for as long
 * as the kernel runs.  lifecycle_lock serializes probe, publish, open,
 * close, restart and the station's quiesce and retry; the bus thread
 * never takes it.  ring_lock (a leaf spinlock) guards the rings, the scan
 * and connection slots, the event count, the failure latch and
 * runtime_active, which the bus thread's hooks and the radio operations
 * read and change.  powered and firmware_running record what a stop must
 * undo; runtime_active is set while the bus thread serves an opened
 * device; opened while the interface is up, which a restart needs.
 * board_mac, set before the probe when has_board_mac is, is the address
 * every start gives a chip whose OTP carries none.
 *
 * work_key and work_buffer belong to the bus thread's radio work: the key
 * message and the commands it builds, cleared as soon as they are sent.
 * poll_frame belongs to the poll, which copies a scan result into it.
 */
struct bcm43455_device {
	const struct bcm43455_bus_ops *ops;
	void *context;
	struct bcm43455_files files;
	char file_version[BCM43455_VERSION_TEXT_MAX];
	uint8_t nvram[BCM43455_NVRAM_BLOB_MAX];
	size_t nvram_length;
	uint8_t placeholder_mac[6];
	unsigned has_placeholder;
	uint8_t board_mac[6];
	unsigned has_board_mac;
	struct bcm43455_identity identity;

	struct bcm43455_bus bus;

	struct mutex lifecycle_lock;
	unsigned powered;
	unsigned firmware_running;
	unsigned runtime_active;
	unsigned published;
	struct net_device *net;
	struct wlan_station *station;

	struct spinlock ring_lock;
	struct bcm43455_frame_ring rx_ring;
	struct bcm43455_frame_ring tx_ring;
	unsigned events_logged;
	uint64_t events;
	int bus_error;
	unsigned opened;
	struct bcm43455_scan scan;
	struct bcm43455_scan_ring scan_ring;
	struct bcm43455_connection connection;
	unsigned failure_pending;
	unsigned failure_reported;
	unsigned restart_pending;
	unsigned restarts;

	uint8_t work_key[BCM43455_KEY_MESSAGE_LENGTH];
	uint8_t work_buffer[BCM43455_BCDC_CONTROL_MAX];
	uint8_t poll_frame[BCM43455_SCAN_FRAME_MAX];
};

/*
 * The radio operations of the WLAN station: the FullMAC method set
 * (bcm43455-station.c).
 */
extern const struct wlan_radio_ops bcm43455_radio_operations;

/*
 * Starts the chip: power, firmware, frame function, the settings every
 * start makes, and, when up is set, the radio and the bus thread.  The
 * caller holds lifecycle_lock; on failure everything is undone.
 */
int bcm43455_device_start(struct bcm43455_device *device, int up);

/*
 * Stops the bus thread, takes the radio down and cuts the chip's power;
 * drops whatever the rings hold.  The caller holds lifecycle_lock.
 */
void bcm43455_device_stop(struct bcm43455_device *device);

/*
 * Queues an Ethernet frame of a connection generation for the firmware to
 * send and wakes the bus thread.  Returns ENETDOWN while the device does
 * not run or its bus failed, ESTALE for a generation that is not the
 * connection's, EMSGSIZE for a frame too long, and ENOBUFS when the ring
 * is full.  Any thread context.
 */
int bcm43455_device_ethernet_transmit(struct bcm43455_device *device, uint64_t generation, const uint8_t *frame, size_t length);

/*
 * Takes the oldest received Ethernet frame into frame, with the
 * connection generation it arrived in; returns 1 when one was taken, 0
 * when the ring is empty.  Poll context.
 */
int bcm43455_device_ethernet_receive(struct bcm43455_device *device, uint8_t *frame, size_t capacity, size_t *length, uint64_t *generation);

/*
 * The station's quiesce: stops the chip if it still runs.  Called by the
 * WLAN layer with the station's control gate held.
 */
int bcm43455_device_quiesce(struct bcm43455_device *device);

/*
 * The station's retry of a close that failed: runs the close again.
 */
int bcm43455_device_stop_retry(struct bcm43455_device *device);

/*
 * Restarts a chip whose bus failed while the interface stayed up: closes
 * the station, stops the chip, starts it again and reopens the station.
 * Called by the WLAN retirement thread after
 * wlan_station_restart_request(); a device closed meanwhile needs nothing.
 */
int bcm43455_device_restart(struct bcm43455_device *device);

/*
 * Clears the scan and connection slots and the scan results, erasing a
 * PMK still held.  The caller holds lifecycle_lock and the bus thread
 * does not run.
 */
void bcm43455_station_reset(struct bcm43455_device *device);

/*
 * Takes what one firmware event means to the scan or the connection.
 * Bus thread (or direct mode); spinlocks only.
 */
void bcm43455_station_event(struct bcm43455_device *device, const struct bcm43455_event *event, const uint8_t *frame, size_t length);

/*
 * Runs the radio work the operations queued: a scan channel's escan
 * request, a connection's commands.  The bus thread's work hook; returns
 * the bus's error.
 */
int bcm43455_station_work(struct bcm43455_device *device);

/*
 * Records that the bus thread stopped on error, for the poll to report.
 * Bus thread.
 */
void bcm43455_station_bus_failed(struct bcm43455_device *device, int error);

/*
 * Tells the WLAN core what the scan, the connection and a bus failure
 * owe it, in their order, then delivers up to budget received frames.
 * Poll context; returns how many frames were taken.
 */
unsigned bcm43455_station_poll(struct bcm43455_device *device, unsigned budget);

/*
 * Reports the generation a received data frame belongs to, or 0 when no
 * connection carries data.  The caller holds ring_lock.
 */
uint64_t bcm43455_station_data_generation_locked(const struct bcm43455_device *device);

/*
 * Builds the scan profile from the firmware's list of valid channels:
 * every channel the WLAN station knows, scanned by the firmware, never
 * probed by the host.
 */
int bcm43455_device_scan_profile(const uint32_t *channels, unsigned count, struct wlan_scan_profile *profile);

/*
 * Reports whether a space-separated capability list names a capability.
 */
int bcm43455_device_has_capability(const char *list, const char *name);

/*
 * Reports whether a firmware build's version text ("43455c0-roml/a-b-c
 * Version: ...") names a feature among its hyphenated words.
 */
int bcm43455_device_has_feature(const char *version, const char *name);

#endif
