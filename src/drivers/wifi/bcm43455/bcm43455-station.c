/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the WLAN station's radio operations.
 *
 * The chip is a FullMAC radio: its firmware scans, joins and runs the
 * WPA2 supplicant, and the WLAN core's FullMAC engine follows it.  The
 * operations here never wait for the chip.  A scan channel and a
 * connection are copied into the device's slots and the bus thread is
 * asked to run the radio work, which sends the escan request or the
 * connection's commands itself, reading the chip's answers meanwhile.
 * The firmware's events are latched in the slots by the bus thread; the
 * poll (the network worker) reports them to the core in their order:
 * a channel's ready, its networks, its end; a connection's join, its
 * authorization, its loss.  Only the barriers wait, bounded by one
 * command's timeout: scan_stop for the escan abort, disconnect for the
 * leave command.  Both are sent through the bus's request slot, which the
 * bus thread serves only after the radio work it is running, so a stop
 * always reaches the firmware after the start it stops.
 *
 * Every copy of the PMK is erased as soon as it has moved on: the slot's
 * when the bus thread takes it, the key message as soon as it is sent
 * (or the connection fails or is cancelled), the bus's frames by the bus.
 */

#include "drivers/wifi/bcm43455/bcm43455-device.h"
#include "drivers/wifi/bcm43455/bcm43455-bcdc.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include "drivers/wifi/bcm43455/bcm43455-radio.h"
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/net/net-device.h>
#include <kern/net/wlan.h>
#include <uapi/errno.h>

/*
 * Marks a parameter the function's contract passes but this driver does not
 * need.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter)	((void)(parameter))
#endif

/*
 * The key timeout the supplicant reports a missing handshake message after, in
 * milliseconds.
 *
 * Value referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (line 79) and
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (line 1141).
 */
#define SUPPLICANT_KEY_TIMEOUT_MS	3000U

/*
 * The EAPOL version the supplicant answers with: whatever the access point
 * uses.
 *
 * Value referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/include/whd_wlioctl.h (line 1103) and
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (lines 716-735 and 1817-1852).
 */
#define SUPPLICANT_EAPOL_ANY		0xffffffffU

/* The interface the station runs on: the firmware's primary one. */
#define PRIMARY_BSSCFG			0U

static int radio_scan_channel_start(void *context, uint64_t generation, uint32_t step_index, uint32_t channel, uint64_t step_deadline_ticks);
static int radio_scan_stop(void *context, uint64_t generation);
static int radio_disconnect(void *context, uint64_t generation);
static int radio_quiesce(void *context);
static int radio_stop_retry(void *context);
static int radio_restart(void *context);
static int radio_fullmac_connect(void *context, const struct wlan_fullmac_connect_request *request);
static int radio_ethernet_transmit(void *context, uint64_t generation, const uint8_t *frame, size_t length);

/*
 * The radio operations: the FullMAC method set.  The SoftMAC methods stay
 * NULL, as the WLAN core requires of a radio whose firmware connects by
 * itself.
 */
const struct wlan_radio_ops bcm43455_radio_operations = {
	radio_scan_channel_start,
	radio_scan_stop,
	NULL,
	radio_disconnect,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	radio_quiesce,
	radio_stop_retry,
	radio_restart,
	radio_fullmac_connect,
	radio_ethernet_transmit
};

static int channel_passive(uint32_t channel);
static int runtime_usable_locked(const struct bcm43455_device *device);
static void scan_clear_locked(struct bcm43455_device *device);
static void scan_drop_locked(struct bcm43455_device *device);
static void connection_clear_locked(struct bcm43455_device *device);
static void scan_event_locked(struct bcm43455_device *device, const struct bcm43455_event *event, const uint8_t *frame, int *wake);
static void scan_results_locked(struct bcm43455_device *device, const uint8_t *data, size_t length);
static void connect_event_locked(struct bcm43455_device *device, const struct bcm43455_event *event, int *wake);
static int work_scan(struct bcm43455_device *device, int *handled);
static int work_connect(struct bcm43455_device *device, int *handled);
static int connect_commands(struct bcm43455_device *device, uint64_t generation, const struct wlan_bss_record *bss, int *outcome);
static int send_command(struct bcm43455_device *device, uint64_t generation, uint32_t command, const uint8_t *payload, size_t length, int *outcome);
static int send_command_u32(struct bcm43455_device *device, uint64_t generation, uint32_t command, uint32_t value, int *outcome);
static int send_bsscfg_u32(struct bcm43455_device *device, uint64_t generation, const char *name, uint32_t value, int *outcome);
static int connection_cancelled(struct bcm43455_device *device, uint64_t generation);
static void schedule_poll(struct bcm43455_device *device);
static void report_scan(struct bcm43455_device *device);
static void report_connection(struct bcm43455_device *device);
static void report_failure(struct bcm43455_device *device);
static unsigned deliver_frames(struct bcm43455_device *device, unsigned budget);
static int connect_error(int outcome);

/*
 * Clears the scan and connection slots and the scan results.
 */
void
bcm43455_station_reset(
	struct bcm43455_device *device)
{
	unsigned long enabled;

	/*
	 * Forgets the scan, the networks found and the connection, erasing the
	 * PMK.
	 */
	enabled = spin_lock_irqsave(&device->ring_lock);

	scan_clear_locked(device);
	scan_drop_locked(device);
	connection_clear_locked(device);

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* The bus thread's buffers, which no thread uses now. */
	kern_memset(device->work_key, 0, sizeof(device->work_key));
	kern_memset(device->work_buffer, 0, sizeof(device->work_buffer));
}

/*
 * Takes what one firmware event means to the scan or the connection.
 */
void
bcm43455_station_event(
	struct bcm43455_device *device,
	const struct bcm43455_event *event,
	const uint8_t *frame,
	size_t length)
{
	unsigned long enabled;
	int wake;

	UNUSED_PARAMETER(length);

	/*
	 * Latches what the event says, under the lock the poll takes them
	 * under.
	 */
	wake = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (event->type == BCM43455_EVENT_SCAN_RESULT)
		scan_event_locked(device, event, frame, &wake);
	else
		connect_event_locked(device, event, &wake);

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Lets the network worker report it. */
	if (wake)
		schedule_poll(device);
}

/*
 * Runs the radio work the operations queued.
 */
int
bcm43455_station_work(
	struct bcm43455_device *device)
{
	int handled;
	int error;

	/* Runs the waiting scan channel and connection until neither waits. */
	for (;;) {
		handled = 0;

		/* A scan channel's escan request. */
		error = work_scan(device, &handled);
		if (error != 0)
			return error;

		/* A connection's commands. */
		error = work_connect(device, &handled);
		if (error != 0)
			return error;

		/* Nothing more was queued meanwhile. */
		if (!handled)
			break;
	}

	/* Succeeded: the bus still works. */
	return 0;
}

/*
 * Records that the bus thread stopped on error.
 */
void
bcm43455_station_bus_failed(
	struct bcm43455_device *device,
	int error)
{
	unsigned long enabled;

	/*
	 * failure_pending refuses new frames and tells the poll to report the
	 * lost link and the scan, then ask for a restart; bus_error keeps the
	 * first error for the log.
	 */
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->bus_error == 0)
		device->bus_error = error;
	device->failure_pending = 1;

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Lets the network worker report it. */
	schedule_poll(device);
}

/*
 * Tells the WLAN core what the scan, the connection and a bus failure
 * owe it, then delivers received frames.
 */
unsigned
bcm43455_station_poll(
	struct bcm43455_device *device,
	unsigned budget)
{
	unsigned done;

	/* The scan channel: ready, its networks, its end or its error. */
	report_scan(device);

	/* The connection: joined, authorized, lost. */
	report_connection(device);

	/* A bus failure: the lost link, the failed scan, and a restart. */
	report_failure(device);

	/* The received frames of the connection. */
	done = deliver_frames(device, budget);

	/* Reports how many frames were taken. */
	return done;
}

/*
 * Reports the generation a received data frame belongs to.
 */
uint64_t
bcm43455_station_data_generation_locked(
	const struct bcm43455_device *device)
{
	/*
	 * Only a connection the firmware joined, and has not lost, carries
	 * data.
	 */
	if (device->connection.state != BCM43455_CONNECT_JOINING)
		return 0;
	if (device->connection.lost)
		return 0;

	/* Reports the connection's generation. */
	return device->connection.generation;
}

/*
 * The core asks for one channel to be scanned: queues it for the bus
 * thread and returns.
 */
static int
radio_scan_channel_start(
	void *context,
	uint64_t generation,
	uint32_t step_index,
	uint32_t channel,
	uint64_t step_deadline_ticks)
{
	struct bcm43455_device *device;
	unsigned long enabled;
	uint16_t chanspec;
	int usable;
	int error;

	UNUSED_PARAMETER(step_deadline_ticks);

	/* The context is the device the station drives. */
	device = context;

	/* Refuses a channel the chip's bands do not have. */
	chanspec = bcm43455_chanspec_from_channel(device->identity.io_type, channel);
	if (chanspec == 0)
		return EINVAL;

	/*
	 * Puts the channel in the scan slot unless a scan of another
	 * generation, or an unfinished channel, holds it.
	 */
	enabled = spin_lock_irqsave(&device->ring_lock);

	usable = runtime_usable_locked(device);
	error = 0;
	if (!usable) {
		error = ENETDOWN;
	} else if (device->scan.state != BCM43455_SCAN_IDLE && device->scan.generation != generation) {
		error = EBUSY;
	} else if (device->scan.state != BCM43455_SCAN_IDLE && device->scan.state != BCM43455_SCAN_DONE) {
		error = EBUSY;
	} else {
		scan_drop_locked(device);
		device->scan.state = BCM43455_SCAN_REQUESTED;
		device->scan.generation = generation;
		device->scan.step = step_index;
		device->scan.channel = channel;
		device->scan.sync_id = 0;
		device->scan.ready_pending = 0;
		device->scan.ready_reported = 0;
		device->scan.complete_pending = 0;
		device->scan.error_pending = 0;
		device->scan.error = 0;
		device->scan.firmware_scanning = 0;
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Reports why the channel was not queued. */
	if (error != 0)
		return error;

	/* The bus thread sends the escan request. */
	bcm43455_bus_request_work(&device->bus);

	/*
	 * Succeeded: the ready, the networks and the end follow from the poll.
	 */
	return 0;
}

/*
 * The core stops a scan: aborts what the firmware still scans and forgets the
 * scan, so nothing more of it is reported.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (whd_wifi_stop_scan(), lines
 * 3394-3419).
 */
static int
radio_scan_stop(
	void *context,
	uint64_t generation)
{
	struct bcm43455_device *device;
	uint8_t request[BCM43455_ESCAN_REQUEST_MAX];
	unsigned long enabled;
	size_t length;
	unsigned abort;
	unsigned layout;
	int error;

	/* The context is the device the station drives. */
	device = context;

	/*
	 * Decides whether the firmware may still scan: a channel the bus
	 * thread is starting, or one the firmware took and has not ended.  A
	 * channel never started or already ended is only forgotten.
	 */
	abort = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->scan.state != BCM43455_SCAN_IDLE && device->scan.generation == generation) {
		if (device->scan.state == BCM43455_SCAN_ISSUING ||
		    device->scan.state == BCM43455_SCAN_RUNNING ||
		    device->scan.state == BCM43455_SCAN_STOPPING ||
		    device->scan.firmware_scanning) {
			device->scan.state = BCM43455_SCAN_STOPPING;
			abort = 1;
		} else {
			scan_clear_locked(device);
			scan_drop_locked(device);
		}
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* A scan the firmware does not run needs no abort. */
	if (!abort)
		return 0;

	/*
	 * Aborts the escan.  The bus thread sends it after the radio work it
	 * runs, so after the start it stops.  A firmware with no scan
	 * running may refuse it, and a stopped bus has none running.
	 */
	layout = device->identity.scan_layout;
	error = bcm43455_escan_encode(request, sizeof(request), layout, BCM43455_ESCAN_ABORT, 0, 0, 0, &length);
	if (error == 0)
		error = bcm43455_bus_iovar_set(&device->bus, "escan", request, length);
	if (error == ETIMEDOUT) {
		kern_logf("bcm43455: scan abort not answered; retried\n");
		return ETIMEDOUT;
	}

	/* Forgets the scan and what it found. */
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->scan.generation == generation) {
		scan_clear_locked(device);
		scan_drop_locked(device);
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Succeeded: no more of the scan is reported. */
	return 0;
}

/*
 * The core ends a connection: stops its commands, has the firmware leave
 * the access point, and forgets the connection.
 */
static int
radio_disconnect(
	void *context,
	uint64_t generation)
{
	struct bcm43455_device *device;
	unsigned long enabled;
	unsigned leave;
	int error;

	/* The context is the device the station drives. */
	device = context;

	/*
	 * A connection the bus thread has not taken is only forgotten; one it
	 * took is cancelled, so it sends nothing more of it, and the firmware
	 * is told to leave.  Another generation's connection is not this
	 * call's.
	 */
	leave = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->connection.state != BCM43455_CONNECT_IDLE && device->connection.generation == generation) {
		if (device->connection.state == BCM43455_CONNECT_REQUESTED) {
			connection_clear_locked(device);
		} else {
			device->connection.cancel = 1;
			leave = 1;
		}
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* A connection never handed to the firmware needs no leave. */
	if (!leave)
		return 0;

	/*
	 * Leaves the access point.  The bus thread sends the command after the
	 * connection's commands it may be sending, which stop at the cancel.
	 * A stopped bus has no association left; a firmware that refuses has
	 * none either.
	 */
	error = bcm43455_bus_control(&device->bus, BCM43455_COMMAND_LEAVE, 1, NULL, 0, NULL, 0, NULL);
	if (error == ETIMEDOUT) {
		kern_logf("bcm43455: leave not answered; retried\n");
		return ETIMEDOUT;
	}

	/* Forgets the connection, which erases a PMK not yet taken. */
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->connection.generation == generation)
		connection_clear_locked(device);

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Succeeded: nothing more of the connection is reported or sent. */
	return 0;
}

/* The core's quiesce: stops the chip if it still runs. */
static int
radio_quiesce(
	void *context)
{
	int error;

	/* Stops it under the device's lifecycle. */
	error = bcm43455_device_quiesce(context);
	if (error != 0)
		return error;

	/* Succeeded: no producer is left. */
	return 0;
}

/* The core's retry of a failed close. */
static int
radio_stop_retry(
	void *context)
{
	int error;

	/* Runs the close again. */
	error = bcm43455_device_stop_retry(context);
	if (error != 0)
		return error;

	/* Succeeded: the radio is stopped and the station reconciled. */
	return 0;
}

/* The core's call of a restart the poll asked for. */
static int
radio_restart(
	void *context)
{
	int error;

	/* Stops the failed chip and starts it again. */
	error = bcm43455_device_restart(context);
	if (error != 0)
		return error;

	/* Succeeded: the radio is back, or nothing was owed. */
	return 0;
}

/*
 * The core hands over a connection: copies it into the connection slot
 * for the bus thread and returns.
 */
static int
radio_fullmac_connect(
	void *context,
	const struct wlan_fullmac_connect_request *request)
{
	struct bcm43455_device *device;
	unsigned long enabled;
	uint16_t chanspec;
	int usable;
	int error;

	/* The context is the device the station drives. */
	device = context;

	/*
	 * Refuses a firmware without its supplicant, and a channel the chip
	 * does not have.
	 */
	if (request == NULL || request->generation == 0)
		return EINVAL;
	if (!device->identity.supplicant)
		return EOPNOTSUPP;
	chanspec = bcm43455_chanspec_from_channel(device->identity.io_type, request->bss.channel);
	if (chanspec == 0)
		return EINVAL;

	/*
	 * Copies the request unless another generation's connection still holds
	 * the slot.
	 */
	enabled = spin_lock_irqsave(&device->ring_lock);

	usable = runtime_usable_locked(device);
	error = 0;
	if (!usable) {
		error = ENETDOWN;
	} else if (device->connection.state != BCM43455_CONNECT_IDLE) {
		error = EBUSY;
	} else {
		connection_clear_locked(device);
		device->connection.state = BCM43455_CONNECT_REQUESTED;
		device->connection.generation = request->generation;
		device->connection.bss = request->bss;
		kern_memcpy(device->connection.pmk, request->pmk, sizeof(device->connection.pmk));
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Reports why the connection was not queued. */
	if (error != 0)
		return error;

	/* The bus thread sends the connection's commands. */
	bcm43455_bus_request_work(&device->bus);

	/* Succeeded: the join's progress follows from the poll. */
	return 0;
}

/* The core sends an Ethernet frame of the open connection. */
static int
radio_ethernet_transmit(
	void *context,
	uint64_t generation,
	const uint8_t *frame,
	size_t length)
{
	int error;

	/* Queues a copy for the bus thread. */
	error = bcm43455_device_ethernet_transmit(context, generation, frame, length);
	if (error != 0)
		return error;

	/* Succeeded: the frame waits for the firmware's credit. */
	return 0;
}

/*
 * Reports whether a channel is scanned without probing.  The world-wide
 * regulatory domain lets a station probe only on 2.4 GHz channels 1 to 11
 * and 5 GHz channels 36 to 48; on every other channel it listens.
 */
static int
channel_passive(
	uint32_t channel)
{
	/* The 2.4 GHz channels every country allows. */
	if (channel >= 1U && channel <= 11U)
		return 0;

	/* The 5 GHz channels every country allows without radar detection. */
	if (channel >= 36U && channel <= 48U)
		return 0;

	/* Any other channel is listened to. */
	return 1;
}

/*
 * Tells whether the bus thread serves the device; the caller holds ring_lock.
 */
static int
runtime_usable_locked(
	const struct bcm43455_device *device)
{
	/* A device that does not run, or whose bus failed, takes no request. */
	if (!device->runtime_active)
		return 0;
	if (device->failure_pending)
		return 0;

	/* Succeeded: the bus thread serves requests. */
	return 1;
}

/* Forgets the scan slot; the caller holds ring_lock. */
static void
scan_clear_locked(
	struct bcm43455_device *device)
{
	uint16_t next;

	/* Keeps only the counter that names the next escan request. */
	next = device->scan.next_sync_id;
	kern_memset(&device->scan, 0, sizeof(device->scan));
	device->scan.next_sync_id = next;
}

/* Drops every network found and not reported; the caller holds ring_lock. */
static void
scan_drop_locked(
	struct bcm43455_device *device)
{
	struct bcm43455_scan_ring *ring;
	struct bcm43455_scan_entry *entry;

	/* Clears the entries from the oldest on. */
	ring = &device->scan_ring;
	while (ring->count != 0) {
		entry = &ring->entries[ring->head];
		kern_memset(entry, 0, sizeof(*entry));
		ring->head = (ring->head + 1U) % ring->capacity;
		ring->count--;
	}

	/* An empty ring starts again at its first entry. */
	ring->head = 0;
}

/* Forgets the connection slot, erasing its PMK; the caller holds ring_lock. */
static void
connection_clear_locked(
	struct bcm43455_device *device)
{
	/* The whole slot, PMK included, goes back to zero. */
	kern_memset(&device->connection, 0, sizeof(device->connection));
}

/*
 * Latches an escan result event for the scan slot: networks into the ring,
 * the channel's end or error into the latches.  The caller holds
 * ring_lock.
 */
static void
scan_event_locked(
	struct bcm43455_device *device,
	const struct bcm43455_event *event,
	const uint8_t *frame,
	int *wake)
{
	struct bcm43455_scan *scan;
	enum bcm43455_scan_status status;
	const uint8_t *data;
	uint16_t sync_id;
	unsigned count;
	size_t first;
	int error;

	/*
	 * Only a channel the bus thread started, or the firmware scans, takes
	 * results.
	 */
	scan = &device->scan;
	if (scan->state != BCM43455_SCAN_ISSUING && scan->state != BCM43455_SCAN_RUNNING)
		return;

	/*
	 * A result that names its scan must name this one; an end without data
	 * is this scan's.
	 */
	data = frame + event->data_start;
	error = bcm43455_escan_result_decode(data, event->data_length, &sync_id, &count, &first);
	if (error == 0 && sync_id != scan->sync_id)
		return;

	/* Tells the result apart. */
	status = bcm43455_scan_classify(event->status);
	switch (status) {
	case BCM43455_SCAN_STATUS_RESULTS:
		/* Networks, as probe responses for the poll. */
		if (error == 0)
			scan_results_locked(device, data, event->data_length);
		break;

	case BCM43455_SCAN_STATUS_COMPLETE:
		/* The channel's end. */
		scan->complete_pending = 1;
		scan->firmware_scanning = 0;
		if (scan->state == BCM43455_SCAN_RUNNING)
			scan->state = BCM43455_SCAN_DONE;
		break;

	case BCM43455_SCAN_STATUS_BUSY:
		/* Another scan or a join took the radio. */
		scan->error_pending = 1;
		scan->error = EBUSY;
		scan->firmware_scanning = 0;
		if (scan->state == BCM43455_SCAN_RUNNING)
			scan->state = BCM43455_SCAN_DONE;
		break;

	default:
		/* An abort nobody asked for, or a failure. */
		scan->error_pending = 1;
		scan->error = EIO;
		scan->firmware_scanning = 0;
		if (scan->state == BCM43455_SCAN_RUNNING)
			scan->state = BCM43455_SCAN_DONE;
		break;
	}

	/* The poll has something to report. */
	*wake = 1;
}

/*
 * Turns every network an escan result describes into a probe response in
 * the scan ring.  The caller holds ring_lock.
 */
static void
scan_results_locked(
	struct bcm43455_device *device,
	const uint8_t *data,
	size_t length)
{
	struct bcm43455_scan_ring *ring;
	struct bcm43455_scan_entry *entry;
	uint16_t sync_id;
	unsigned count;
	unsigned i;
	size_t offset;
	size_t bss_length;
	int error;

	/* Finds the first description of the networks the ring takes. */
	ring = &device->scan_ring;
	error = bcm43455_escan_result_decode(data, length, &sync_id, &count, &offset);
	if (error != 0)
		return;

	/*
	 * Converts each description into the next entry; a full ring or a
	 * malformed one drops it.
	 */
	for (i = 0; i < count; i++) {
		error = bcm43455_bss_length(data, length, offset, &bss_length);
		if (error != 0)
			break;

		/*
		 * A full ring drops the network; the core hears it on the next
		 * scan.
		 */
		if (ring->count == ring->capacity) {
			ring->dropped++;
			offset += bss_length;
			continue;
		}

		/*
		 * Turns the description into a probe response in the next free
		 * entry.
		 */
		entry = &ring->entries[(ring->head + ring->count) % ring->capacity];
		error = bcm43455_bss_to_frame(
			data + offset,
			bss_length,
			device->identity.io_type,
			entry->frame,
			sizeof(entry->frame),
			&entry->length,
			&entry->channel,
			&entry->rssi);
		if (error == 0) {
			entry->generation = device->scan.generation;
			entry->step = device->scan.step;
			ring->count++;
		} else {
			kern_memset(entry, 0, sizeof(*entry));
			ring->dropped++;
		}

		/* Moves on to the next description. */
		offset += bss_length;
	}
}

/*
 * Latches what an event means to the connection.  The caller holds
 * ring_lock.
 */
static void
connect_event_locked(
	struct bcm43455_device *device,
	const struct bcm43455_event *event,
	int *wake)
{
	struct bcm43455_connection *connection;
	struct bcm43455_connect_outcome outcome;

	/*
	 * Only a connection the bus thread took, and nobody cancelled, hears
	 * events.
	 */
	connection = &device->connection;
	if (connection->state == BCM43455_CONNECT_IDLE || connection->state == BCM43455_CONNECT_REQUESTED)
		return;
	if (connection->cancel)
		return;

	/* Reads what the event means at the connection's progress. */
	bcm43455_connect_classify(event, connection->associated, connection->authorized, &outcome);
	if (outcome.step == BCM43455_CONNECT_NONE)
		return;

	/*
	 * A lost connection takes no more progress; a loss not yet reported
	 * still takes the evidence of a wrong key that came after it (a
	 * supplicant's failure after the access point left), which says more
	 * than the plain loss.
	 */
	if (connection->lost) {
		/*
		 * Takes a wrong key's evidence over an unreported plain loss.
		 */
		if (outcome.step == BCM43455_CONNECT_LOST &&
		    outcome.error == EACCES &&
		    connection->lost_error == ECONNRESET &&
		    !connection->lost_reported) {
			connection->lost_error = EACCES;
			*wake = 1;
		}

		/* Takes nothing else after the loss. */
		return;
	}

	/* Latches the step. */
	switch (outcome.step) {
	case BCM43455_CONNECT_ASSOCIATED:
		/* The firmware joined. */
		connection->associated = 1;
		break;

	case BCM43455_CONNECT_AUTHORIZED:
		/* Its supplicant finished; a join always comes before. */
		connection->associated = 1;
		connection->authorized = 1;
		break;

	default:
		/* The connection ends, with the first reason. */
		connection->lost = 1;
		connection->lost_error = outcome.error;
		break;
	}

	/* The poll has something to report. */
	*wake = 1;
}

/*
 * Takes the waiting scan channel and sends its escan request.  handled is set
 * when there was one.  Returns the bus's error.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (whd_wifi_scan()); OpenBSD
 * sys/dev/ic/bwfm.c revision 1.113 (bwfm_scan_v0()).
 */
static int
work_scan(
	struct bcm43455_device *device,
	int *handled)
{
	uint8_t request[BCM43455_ESCAN_REQUEST_MAX];
	unsigned long enabled;
	uint64_t generation;
	uint32_t step;
	uint32_t channel;
	uint16_t sync_id;
	uint16_t chanspec;
	size_t length;
	size_t payload_length;
	int outcome;
	int taken;
	int error;

	/* Takes the requested channel and names its escan request, never 0. */
	taken = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->scan.state == BCM43455_SCAN_REQUESTED) {
		device->scan.next_sync_id++;
		if (device->scan.next_sync_id == 0)
			device->scan.next_sync_id = 1;
		device->scan.sync_id = device->scan.next_sync_id;
		device->scan.state = BCM43455_SCAN_ISSUING;
		taken = 1;
	}

	/* Samples the scan the request belongs to. */
	generation = device->scan.generation;
	step = device->scan.step;
	channel = device->scan.channel;
	sync_id = device->scan.sync_id;

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* No channel waits. */
	if (!taken)
		return 0;
	*handled = 1;

	/*
	 * Builds the escan variable for the one channel, probing only where the
	 * domain allows it.
	 */
	chanspec = bcm43455_chanspec_from_channel(device->identity.io_type, channel);
	error = bcm43455_escan_encode(
		request,
		sizeof(request),
		device->identity.scan_layout,
		BCM43455_ESCAN_START,
		sync_id,
		chanspec,
		channel_passive(channel),
		&length);
	if (error == 0)
		error = bcm43455_bcdc_iovar(device->work_buffer, sizeof(device->work_buffer), "escan", request, length, &payload_length);

	/* Sends it; outcome says whether the firmware took it. */
	outcome = error;
	if (error == 0) {
		error = bcm43455_bus_thread_control(
			&device->bus,
			BCM43455_COMMAND_WRITE_VARIABLE,
			1,
			device->work_buffer,
			payload_length,
			NULL,
			0,
			NULL,
			&outcome);
		kern_memset(device->work_buffer, 0, payload_length);
		if (error != 0)
			return error;
	}

	/*
	 * Records the outcome for the channel still being started: the
	 * firmware scans and the core may hear the ready, or the channel
	 * failed.  A channel stopped meanwhile only notes that the firmware
	 * scans, for the abort.
	 */
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->scan.generation == generation &&
	    device->scan.step == step &&
	    device->scan.sync_id == sync_id) {
		if (device->scan.state == BCM43455_SCAN_ISSUING) {
			if (outcome == 0) {
				device->scan.ready_pending = 1;
				if (device->scan.complete_pending || device->scan.error_pending) {
					device->scan.state = BCM43455_SCAN_DONE;
				} else {
					device->scan.state = BCM43455_SCAN_RUNNING;
					device->scan.firmware_scanning = 1;
				}
			} else {
				device->scan.state = BCM43455_SCAN_DONE;
				device->scan.error_pending = 1;
				device->scan.error = EIO;
				if (outcome == ETIMEDOUT)
					device->scan.error = ETIMEDOUT;
				if (outcome == ECANCELED)
					device->scan.error = ENETDOWN;
			}
		} else if (device->scan.state == BCM43455_SCAN_STOPPING && outcome == 0) {
			device->scan.firmware_scanning = 1;
		}
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Lets the network worker report the ready or the failure. */
	schedule_poll(device);

	/* Succeeded: the bus still works. */
	return 0;
}

/*
 * Takes the waiting connection and sends its commands.  handled is set
 * when there was one.  Returns the bus's error.
 */
static int
work_connect(
	struct bcm43455_device *device,
	int *handled)
{
	struct wlan_bss_record bss;
	unsigned long enabled;
	uint64_t generation;
	int outcome;
	int taken;
	int error;

	/*
	 * Takes the requested connection.  The PMK leaves the slot at once,
	 * as the key message the commands send.
	 */
	taken = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	if (device->connection.state == BCM43455_CONNECT_REQUESTED && !device->connection.cancel) {
		bcm43455_key_encode(device->work_key, device->connection.pmk);
		kern_memset(device->connection.pmk, 0, sizeof(device->connection.pmk));
		device->connection.state = BCM43455_CONNECT_ISSUING;
		taken = 1;
	}

	/* Samples the connection the request belongs to. */
	generation = device->connection.generation;
	bss = device->connection.bss;

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* No connection waits. */
	if (!taken)
		return 0;
	*handled = 1;

	/*
	 * Sends the connection's commands; the key message is erased whatever
	 * came of them.
	 */
	outcome = 0;
	error = connect_commands(device, generation, &bss, &outcome);
	kern_memset(device->work_key, 0, sizeof(device->work_key));
	kern_memset(&bss, 0, sizeof(bss));
	if (error != 0)
		return error;

	/*
	 * Records how the commands went: the firmware joins, or the connection
	 * failed and its half-made settings are undone.  A cancelled one is
	 * disconnect's.
	 */
	enabled = spin_lock_irqsave(&device->ring_lock);

	taken = 0;
	if (device->connection.generation == generation &&
	    device->connection.state == BCM43455_CONNECT_ISSUING &&
	    !device->connection.cancel) {
		if (outcome == 0) {
			device->connection.state = BCM43455_CONNECT_JOINING;
		} else {
			device->connection.state = BCM43455_CONNECT_ENDED;
			if (!device->connection.lost) {
				device->connection.lost = 1;
				device->connection.lost_error = connect_error(outcome);
			}

			/* The failed connection's settings are undone below. */
			taken = 1;
		}
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/*
	 * Undoes a failed connection's settings: the firmware leaves whatever
	 * it began.
	 */
	if (taken) {
		kern_logf("bcm43455: connection's commands failed (%d)\n", outcome);
		error = bcm43455_bus_thread_control(&device->bus, BCM43455_COMMAND_LEAVE, 1, NULL, 0, NULL, 0, NULL, &outcome);
		if (error != 0)
			return error;
	}

	/* Lets the network worker report a failure. */
	schedule_poll(device);

	/* Succeeded: the bus still works. */
	return 0;
}

/*
 * Sends a WPA2-PSK connection's commands in their order, stopping at the first
 * one that fails or at a cancel.  outcome receives the failure.  Returns the
 * bus's error.
 *
 * Procedure referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/whd_wifi_api.c (whd_wifi_prepare_join(), lines
 * 1717-2077, and the join, lines 2433-2490).
 */
static int
connect_commands(
	struct bcm43455_device *device,
	uint64_t generation,
	const struct wlan_bss_record *bss,
	int *outcome)
{
	uint16_t chanspec;
	size_t length;
	int optional;
	int error;

	/* An infrastructure network. */
	error = send_command_u32(device, generation, BCM43455_COMMAND_INFRASTRUCTURE, 1, outcome);
	if (error != 0 || *outcome != 0)
		return error;

	/* Open authentication; WPA2 runs after it. */
	error = send_command_u32(device, generation, BCM43455_COMMAND_AUTHENTICATION, BCM43455_AUTHENTICATION_OPEN, outcome);
	if (error != 0 || *outcome != 0)
		return error;

	/* The AES (CCMP) cipher. */
	error = send_command_u32(device, generation, BCM43455_COMMAND_CIPHERS, BCM43455_CIPHER_AES, outcome);
	if (error != 0 || *outcome != 0)
		return error;

	/* WPA2 with a pre-shared key. */
	error = send_command_u32(device, generation, BCM43455_COMMAND_KEY_MANAGEMENT, BCM43455_KEY_MANAGEMENT_WPA2_PSK, outcome);
	if (error != 0 || *outcome != 0)
		return error;

	/*
	 * The firmware's own supplicant runs the four-way and group handshakes.
	 */
	error = send_bsscfg_u32(device, generation, "bsscfg:sup_wpa", 1, outcome);
	if (error != 0 || *outcome != 0)
		return error;

	/*
	 * The EAPOL version the access point uses, and the key timeout that
	 * makes the supplicant report a missing message; a firmware without
	 * them still connects.
	 */
	error = send_bsscfg_u32(device, generation, "bsscfg:sup_wpa2_eapver", SUPPLICANT_EAPOL_ANY, &optional);
	if (error != 0)
		return error;
	if (optional == ECANCELED) {
		*outcome = ECANCELED;
		return 0;
	}

	/*
	 * Sets the key timeout, which a firmware without it also does without.
	 */
	error = send_bsscfg_u32(device, generation, "bsscfg:sup_wpa_tmo", SUPPLICANT_KEY_TIMEOUT_MS, &optional);
	if (error != 0)
		return error;
	if (optional == ECANCELED) {
		*outcome = ECANCELED;
		return 0;
	}

	/*
	 * The PMK, as the supplicant's key; its message is erased right after.
	 */
	error = send_command(device, generation, BCM43455_COMMAND_SUPPLICANT_KEY, device->work_key, sizeof(device->work_key), outcome);
	kern_memset(device->work_key, 0, sizeof(device->work_key));
	if (error != 0 || *outcome != 0)
		return error;

	/*
	 * The join: the SSID, the access point the core chose and its channel.
	 */
	chanspec = bcm43455_chanspec_from_channel(device->identity.io_type, bss->channel);
	error = bcm43455_join_encode(
		device->work_buffer,
		sizeof(device->work_buffer),
		bss->ssid,
		bss->ssid_length,
		bss->bssid,
		chanspec,
		&length);
	if (error != 0) {
		*outcome = error;
		return 0;
	}

	/* Sends the join, then clears the request. */
	error = send_command(device, generation, BCM43455_COMMAND_JOIN, device->work_buffer, length, outcome);
	kern_memset(device->work_buffer, 0, length);
	if (error != 0)
		return error;

	/*
	 * Succeeded: the bus still works; outcome says whether the firmware
	 * joins.
	 */
	return 0;
}

/*
 * Sends one of a connection's commands unless the connection was
 * cancelled (outcome ECANCELED).  Returns the bus's error.
 */
static int
send_command(
	struct bcm43455_device *device,
	uint64_t generation,
	uint32_t command,
	const uint8_t *payload,
	size_t length,
	int *outcome)
{
	int cancelled;
	int error;

	/* A cancelled connection sends nothing more. */
	cancelled = connection_cancelled(device, generation);
	if (cancelled) {
		*outcome = ECANCELED;
		return 0;
	}

	/* Sends it and waits for the firmware's answer. */
	error = bcm43455_bus_thread_control(&device->bus, command, 1, payload, length, NULL, 0, NULL, outcome);
	if (error != 0)
		return error;

	/* Succeeded: the bus still works. */
	return 0;
}

/* Sends a connection's command that takes one 32-bit number. */
static int
send_command_u32(
	struct bcm43455_device *device,
	uint64_t generation,
	uint32_t command,
	uint32_t value,
	int *outcome)
{
	uint8_t bytes[4];
	int error;

	/* The number is little-endian. */
	bcm43455_put_le32(bytes, value);
	error = send_command(device, generation, command, bytes, sizeof(bytes), outcome);
	if (error != 0)
		return error;

	/* Succeeded: the bus still works. */
	return 0;
}

/*
 * Sets a variable of the primary interface ("bsscfg:" and its index
 * before the number), as a connection's command.
 */
static int
send_bsscfg_u32(
	struct bcm43455_device *device,
	uint64_t generation,
	const char *name,
	uint32_t value,
	int *outcome)
{
	uint8_t bytes[8];
	size_t length;
	int error;

	/* The interface's index, then the number, both little-endian. */
	bcm43455_put_le32(bytes, PRIMARY_BSSCFG);
	bcm43455_put_le32(bytes + 4, value);
	error = bcm43455_bcdc_iovar(device->work_buffer, sizeof(device->work_buffer), name, bytes, sizeof(bytes), &length);
	if (error != 0) {
		*outcome = error;
		return 0;
	}

	/* Sends it as a variable. */
	error = send_command(device, generation, BCM43455_COMMAND_WRITE_VARIABLE, device->work_buffer, length, outcome);
	kern_memset(device->work_buffer, 0, length);
	if (error != 0)
		return error;

	/* Succeeded: the bus still works. */
	return 0;
}

/*
 * Tells whether disconnect cancelled the connection, or another took its place.
 */
static int
connection_cancelled(
	struct bcm43455_device *device,
	uint64_t generation)
{
	unsigned long enabled;
	int cancelled;

	/* Samples whether the slot's connection was cancelled or replaced. */
	enabled = spin_lock_irqsave(&device->ring_lock);

	cancelled = 0;
	if (device->connection.cancel)
		cancelled = 1;
	if (device->connection.generation != generation)
		cancelled = 1;

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Reports whether the commands should stop. */
	return cancelled;
}

/* Schedules the network worker's poll of the published device. */
static void
schedule_poll(
	struct bcm43455_device *device)
{
	unsigned long enabled;
	struct net_device *net;

	/* Samples the network device, which exists from the publication on. */
	enabled = spin_lock_irqsave(&device->ring_lock);

	net = device->net;

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Lets the network worker run the poll. */
	if (net != NULL)
		net_device_schedule_poll(net);
}

/*
 * Reports the scan channel's latches in the order the core requires:
 * ready, then the networks, then the end; an error at any time.
 */
static void
report_scan(
	struct bcm43455_device *device)
{
	struct bcm43455_scan_ring *ring;
	struct bcm43455_scan_entry *entry;
	unsigned long enabled;
	uint64_t generation;
	uint32_t step;
	uint32_t channel;
	int32_t rssi;
	size_t length;
	int ready;
	int complete;
	int failed;
	int found;
	int error;

	/* Reports from the ring of networks found. */
	ring = &device->scan_ring;

	/*
	 * Takes the ready, which lets the core accept the channel's networks.
	 */
	ready = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	generation = device->scan.generation;
	step = device->scan.step;
	if (device->scan.ready_pending) {
		device->scan.ready_pending = 0;
		device->scan.ready_reported = 1;
		ready = 1;
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Lets the core accept the networks. */
	if (ready)
		(void)wlan_station_report_scan_channel_ready(device->station, generation, step);

	/*
	 * Reports the networks of the ready channel, dropping those of an older
	 * one.
	 */
	for (;;) {
		found = 0;
		enabled = spin_lock_irqsave(&device->ring_lock);

		while (ring->count != 0) {
			entry = &ring->entries[ring->head];

			/*
			 * A network of the channel the core accepts results for
			 * is reported.
			 */
			if (entry->generation == device->scan.generation &&
			    entry->step == device->scan.step &&
			    device->scan.state != BCM43455_SCAN_IDLE) {
				if (!device->scan.ready_reported)
					break;
				kern_memcpy(device->poll_frame, entry->frame, entry->length);
				length = entry->length;
				channel = entry->channel;
				rssi = entry->rssi;
				generation = entry->generation;
				found = 1;
			}

			/* The entry is taken, or dropped as another scan's. */
			kern_memset(entry, 0, sizeof(*entry));
			ring->head = (ring->head + 1U) % ring->capacity;
			ring->count--;
			if (found)
				break;
		}

		spin_unlock_irqrestore(&device->ring_lock, enabled);

		/* No network of the ready channel is left. */
		if (!found)
			break;

		/* The core parses the probe response and keeps the network. */
		error = wlan_station_report_scan_frame(device->station, generation, device->poll_frame, length, rssi, (uint8_t)channel);
		kern_memset(device->poll_frame, 0, length);
		if (error != 0 && error != ESTALE)
			kern_logf("bcm43455: scan result on channel %u not taken (%d)\n", channel, error);
	}

	/*
	 * Takes the end once every network before it was reported, and an
	 * error.
	 */
	complete = 0;
	failed = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	generation = device->scan.generation;
	step = device->scan.step;
	error = device->scan.error;
	if (device->scan.complete_pending &&
	    device->scan.ready_reported &&
	    ring->count == 0) {
		device->scan.complete_pending = 0;
		device->restarts = 0;
		complete = 1;
	}

	/* Takes the scan's error. */
	if (device->scan.error_pending) {
		device->scan.error_pending = 0;
		failed = 1;
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Tells the core the channel's end and the scan's error. */
	if (complete)
		(void)wlan_station_report_scan_channel_complete(device->station, generation, step);
	if (failed)
		(void)wlan_station_report_scan_error(device->station, generation, error);
}

/*
 * Reports the connection's latch in its order: joined, authorized, lost.
 */
static void
report_connection(
	struct bcm43455_device *device)
{
	struct bcm43455_connection *connection;
	unsigned long enabled;
	uint64_t generation;
	int associated;
	int authorized;
	int lost;
	int lost_error;

	/* Takes what was latched and not yet told. */
	connection = &device->connection;
	associated = 0;
	authorized = 0;
	lost = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	generation = connection->generation;
	lost_error = connection->lost_error;
	if (connection->state != BCM43455_CONNECT_IDLE) {
		if (connection->associated && !connection->associated_reported) {
			connection->associated_reported = 1;
			associated = 1;
		}

		/* The authorization also starts the restart count afresh. */
		if (connection->authorized && !connection->authorized_reported) {
			connection->authorized_reported = 1;
			device->restarts = 0;
			authorized = 1;
		}

		/* The loss is told once. */
		if (connection->lost && !connection->lost_reported) {
			connection->lost_reported = 1;
			lost = 1;
		}
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/*
	 * Tells the core's FullMAC engine, which opens or closes the controlled
	 * port itself.
	 */
	if (associated)
		(void)wlan_station_report_fullmac_event(device->station, generation, WLAN_FULLMAC_EVENT_ASSOCIATED);
	if (authorized)
		(void)wlan_station_report_fullmac_event(device->station, generation, WLAN_FULLMAC_EVENT_AUTHORIZED);
	if (lost) {
		kern_logf("bcm43455: connection lost (%d)\n", lost_error);
		(void)wlan_station_report_link_loss(device->station, generation, lost_error);
	}
}

/*
 * Reports a bus failure once: the connection and the scan are lost with
 * EIO, and an open interface is restarted.
 */
static void
report_failure(
	struct bcm43455_device *device)
{
	unsigned long enabled;
	uint64_t connection_generation;
	uint64_t scan_generation;
	int connection_lost;
	int scan_failed;
	int restart;

	/* Takes the failure and decides what it ends. */
	connection_lost = 0;
	scan_failed = 0;
	restart = 0;
	enabled = spin_lock_irqsave(&device->ring_lock);

	connection_generation = device->connection.generation;
	scan_generation = device->scan.generation;
	if (device->failure_pending && !device->failure_reported) {
		device->failure_reported = 1;
		if (device->connection.state != BCM43455_CONNECT_IDLE && !device->connection.lost_reported) {
			device->connection.lost = 1;
			device->connection.lost_error = EIO;
			device->connection.lost_reported = 1;
			connection_lost = 1;
		}

		/* A running scan fails with the bus. */
		if (device->scan.state != BCM43455_SCAN_IDLE)
			scan_failed = 1;

		/* An open interface asks for a restart, up to the limit. */
		if (device->opened && device->restarts < BCM43455_RESTARTS_MAX) {
			device->restarts++;
			device->restart_pending = 1;
			restart = 1;
		}
	}

	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Reports what the failure ended. */
	if (connection_lost)
		(void)wlan_station_report_link_loss(device->station, connection_generation, EIO);
	if (scan_failed)
		(void)wlan_station_report_scan_error(device->station, scan_generation, EIO);

	/*
	 * Asks the retirement thread for the restart, or says why there is
	 * none.
	 */
	if (restart) {
		wlan_station_restart_request(device->station);
	} else if (connection_lost || scan_failed) {
		kern_logf("bcm43455: chip left down after a bus failure; cycle Wi-Fi to retry\n");
	}
}

/*
 * Hands up to budget received frames to the station, which delivers them
 * when the controlled port of their connection is open.
 */
static unsigned
deliver_frames(
	struct bcm43455_device *device,
	unsigned budget)
{
	uint8_t frame[BCM43455_BUS_ETHERNET_MAX];
	uint64_t generation;
	size_t length;
	unsigned done;
	int taken;
	int error;

	/* Drains up to the budget. */
	for (done = 0; done < budget; done++) {
		generation = 0;
		taken = bcm43455_device_ethernet_receive(device, frame, sizeof(frame), &length, &generation);
		if (!taken)
			break;

		/*
		 * The station refuses a frame of an older connection, or one
		 * before the port opens.
		 */
		error = wlan_station_report_ethernet(device->station, generation, frame, length);
		if (error != 0)
			device->net->rx_dropped++;
	}

	/* Clears the last frame's bytes. */
	kern_memset(frame, 0, sizeof(frame));

	/* Reports how many frames were taken. */
	return done;
}

/* The error a failed connection command is reported with. */
static int
connect_error(
	int outcome)
{
	/* A command without an answer timed out. */
	if (outcome == ETIMEDOUT)
		return ETIMEDOUT;

	/* A stopped bus thread is the radio going down. */
	if (outcome == ECANCELED)
		return ENETDOWN;

	/* Reports any refusal as the device's failure. */
	return EIO;
}
