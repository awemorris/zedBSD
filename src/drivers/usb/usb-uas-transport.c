/*
 * USB Attached SCSI serialized command and endpoint owner.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
#include <drivers/usb.h>
#include <drivers/usb-uas.h>
#include <kern/sched.h>
#include <errno.h>
#include <string.h>

static int uas_stream_submit(struct drv_usb_uas_transport *, unsigned, unsigned,
    void *, size_t);
static int uas_super_transfer(struct drv_usb_uas_transport *, struct drv_usb_uas_command *,
    uint8_t *, void *, size_t);
static int uas_transfer(struct drv_usb_uas_transport *transport, unsigned pipe,
    void *buffer, size_t length, size_t *actual);

int
drv_usb_uas_transport_init(struct drv_usb_uas_transport *transport,
    struct drv_usb_device *device, struct drv_usb_endpoint *pipes[4],
    size_t capacity)
{
	const struct drv_usb_endpoint_descriptor *descriptor;
	unsigned i;
	unsigned j;
	unsigned direction;
	int reserved;
	int super_speed;
	int error;
	size_t size;

	if (transport == NULL || device == NULL || pipes == NULL)
		return EINVAL;
	for (i = 0; i < 4; i++) {
		if (transport->urbs[i] != NULL)
			return EBUSY;
	}
	super_speed = drv_usb_device_speed(device) == DRV_USB_SPEED_SUPER;
	if (!super_speed && drv_usb_device_speed(device) != DRV_USB_SPEED_HIGH)
		return EINVAL;
	if (capacity == 0 || capacity > DRV_USB_TRANSFER_RESERVE_MAX_SIZE)
		return EINVAL;
	for (i = 0; i < 4; i++) {
		if (pipes[i] == NULL)
			return EINVAL;
		descriptor = drv_usb_endpoint_descriptor(pipes[i]);
		if (descriptor == NULL)
			return EINVAL;
		direction = (i == 1 || i == 2) ? DRV_USB_DIR_IN : 0;
		if ((descriptor->address & DRV_USB_DIR_IN) != direction ||
		    (descriptor->attributes & 3) != DRV_USB_TRANSFER_BULK ||
		    descriptor->maximum_packet_size != (super_speed ? 1024 : 512) ||
		    (descriptor->address & 15) == 0)
			return EINVAL;
		for (j = 0; j < i; j++) {
			if (drv_usb_endpoint_descriptor(pipes[j])->address == descriptor->address)
				return EINVAL;
		}
	}

	reserved = (drv_usb_device_hcd_capabilities(device) &
	    DRV_USB_HCD_CAP_TRANSFER_RESERVE) != 0;
	if (!reserved && capacity > DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE)
		capacity = DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE;
	memset(transport, 0, sizeof(*transport));
	transport->stopped = 1;
	transport->super_speed = super_speed;
	if (super_speed) {
		for (i = 1; i < 4; i++) {
			error = drv_usb_endpoint_configure_streams(pipes[i], 3);
			if (error != 0)
				return error;
		}
	}
	for (i = 0; i < 4; i++) {
		transport->urbs[i] = drv_usb_urb_alloc(device, pipes[i], 0);
		if (transport->urbs[i] == NULL) {
			error = ENOMEM;
			goto fail;
		}
		size = i == 0 ? 32 : (i == 1 ? 512 : capacity);
		if (reserved)
			error = drv_usb_urb_reserve_transfer(transport->urbs[i], size);
		else
			error = drv_usb_urb_reserve_sync(transport->urbs[i], size);
		if (error != 0)
			goto fail;
	}
	transport->capacity = capacity;
	transport->next_tag = 1;
	transport->stopped = 0;
	return 0;

fail:
	/* No URB has been submitted, so partial initialization owns all storage. */
	for (i = 0; i < 4; i++) {
		drv_usb_urb_free(transport->urbs[i]);
		transport->urbs[i] = NULL;
	}
	return error;
}

int
drv_usb_uas_transport_execute(struct drv_usb_uas_transport *transport,
    unsigned lun, const void *cdb, size_t cdb_length,
    enum drv_usb_uas_direction direction, void *buffer, size_t length,
    unsigned timeout_ms, struct drv_usb_uas_result *result)
{
	struct drv_usb_uas_command command;
	uint8_t wire[32];
	uint8_t status[512];
	uint64_t now;
	uint64_t ticks;
	size_t actual;
	size_t sense_length;
	unsigned pipe;
	int error;

	if (result == NULL)
		return EINVAL;
	memset(result, 0, sizeof(*result));
	if (transport == NULL || timeout_ms == 0)
		return EINVAL;
	if (transport->stopped || transport->next_tag == 0)
		return EIO;
	if (length > transport->capacity || (length != 0 && buffer == NULL))
		return EINVAL;

	memset(&command, 0, sizeof(command));
	error = drv_usb_uas_command_begin(&command, (uint16_t)transport->next_tag,
	    lun, cdb, cdb_length, direction, length, wire);
	if (error != 0)
		return error;
	/* Only a validated terminal status permits wrap/reuse of a depth-one tag. */
	now = sched_ticks();
	ticks = ((uint64_t)timeout_ms + 9) / 10;
	transport->deadline = UINT64_MAX - now < ticks ? UINT64_MAX : now + ticks;
	error = uas_transfer(transport, 0, wire, sizeof(wire), &actual);
	if (error != 0)
		goto fail;
	if (actual != sizeof(wire)) {
		error = EIO;
		goto fail;
	}

	if (transport->super_speed) {
		error = uas_super_transfer(transport, &command, status, buffer, length);
		if (error != 0)
			goto fail;
		goto completed;
	}

	/* At most READY + Sense: no unbounded polling of malformed status traffic. */
	for (;;) {
		error = uas_transfer(transport, 1, status, sizeof(status), &actual);
		if (error != 0)
			goto fail;
		error = drv_usb_uas_command_status(&command, status, actual);
		if (error != 0)
			goto fail;
		if (command.state == DRV_USB_UAS_COMPLETE)
			break;
		pipe = direction == DRV_USB_UAS_READ ? 2 : 3;
		error = uas_transfer(transport, pipe, buffer, length, &actual);
		if (error != 0)
			goto fail;
		error = drv_usb_uas_command_data(&command, actual);
		if (error != 0)
			goto fail;
	}
completed:
	result->transferred = command.transferred;
	result->status = command.scsi_status;
	sense_length = ((size_t)status[14] << 8) | status[15];
	if (sense_length > sizeof(result->sense))
		sense_length = sizeof(result->sense);
	result->sense_length = sense_length;
	memcpy(result->sense, status + 16, sense_length);
	transport->next_tag = transport->next_tag == (transport->super_speed ? 3U : UINT16_MAX) ? 1 : transport->next_tag + 1;
	return 0;

fail:
	/* Host cancellation is not a SCSI task abort. Refuse all subsequent I/O. */
	drv_usb_uas_command_fail(&command);
	transport->stopped = 1;
	transport->failed_tag = command.tag;
	transport->failed_lun = (uint8_t)lun;
	transport->recovery_attempted = 0;
	return error;
}


int
drv_usb_uas_transport_recover(struct drv_usb_uas_transport *transport,
    unsigned timeout_ms)
{
	uint8_t command[16];
	uint8_t status[512];
	uint16_t tag;
	unsigned response_tag;
	unsigned i;
	unsigned stale;
	size_t actual;
	size_t sense_length;
	uint64_t now;
	uint64_t ticks;
	int error;

	if (transport == NULL || timeout_ms == 0)
		return EINVAL;
	if (!transport->stopped || transport->failed_tag == 0 || transport->recovery_attempted)
		return EIO;
	if (transport->super_speed)
		return EOPNOTSUPP;
	transport->recovery_attempted = 1;
	/* First retire every host reference; cancellation alone proves nothing. */
	for (i = 0; i < 4; i++) {
		if (transport->urbs[i] == NULL)
			return EIO;
		(void)drv_usb_urb_cancel(transport->urbs[i]);
		error = drv_usb_urb_drain(transport->urbs[i], 1000);
		if (error != 0)
			return error;
	}

	tag = transport->failed_tag == UINT16_MAX ? 1 : transport->failed_tag + 1;
	memset(command, 0, sizeof(command));
	command[0] = 5; /* Task Management IU. */
	command[2] = (uint8_t)(tag >> 8);
	command[3] = (uint8_t)tag;
	command[4] = 1; /* ABORT TASK, not a media/LUN reset. */
	command[6] = (uint8_t)(transport->failed_tag >> 8);
	command[7] = (uint8_t)transport->failed_tag;
	command[9] = transport->failed_lun;
	now = sched_ticks();
	ticks = ((uint64_t)timeout_ms + 9) / 10;
	transport->deadline = UINT64_MAX - now < ticks ? UINT64_MAX : now + ticks;
	error = uas_transfer(transport, 0, command, sizeof(command), &actual);
	if (error != 0)
		return error;
	if (actual != sizeof(command))
		return EIO;

	for (stale = 0; stale <= 8; stale++) {
		error = uas_transfer(transport, 1, status, sizeof(status), &actual);
		if (error != 0)
			return error;
		if (actual < 4)
			return EIO;
		response_tag = ((unsigned)status[2] << 8) | status[3];
		if (response_tag == tag) {
			/* TMF COMPLETE is the abort's device-task retirement barrier. */
			if (actual != 8 || status[0] != 4 || status[7] != 0 ||
			    status[4] != 0 || status[5] != 0 || status[6] != 0)
				return EIO;
			transport->failed_tag = 0;
			transport->next_tag = tag == UINT16_MAX ? 1 : tag + 1;
			transport->stopped = 0;
			return 0;
		}
		if (response_tag != transport->failed_tag)
			return EIO;
		/* Previously queued READY/Sense can precede the abort response. */
		if ((status[0] == 6 || status[0] == 7) && actual == 4)
			continue;
		if (status[0] != 3 || actual < 16)
			return EIO;
		sense_length = ((size_t)status[14] << 8) | status[15];
		if (sense_length != actual - 16)
			return EIO;
	}
	return EIO;
}

int
drv_usb_uas_transport_stop(struct drv_usb_uas_transport *transport)
{
	unsigned i;
	int error;
	int first_error;

	if (transport == NULL)
		return EINVAL;
	transport->stopped = 1;
	transport->failed_tag = 0;
	transport->recovery_attempted = 1;
	first_error = 0;
	for (i = 0; i < 4; i++) {
		if (transport->urbs[i] == NULL)
			continue;
		(void)drv_usb_urb_cancel(transport->urbs[i]);
		error = drv_usb_urb_drain(transport->urbs[i], 1000);
		if (error != 0 && first_error == 0)
			first_error = error;
	}
	if (first_error != 0)
		return first_error;
	for (i = 0; i < 4; i++) {
		drv_usb_urb_free(transport->urbs[i]);
		transport->urbs[i] = NULL;
	}
	return 0;
}


/* Starts a reserved, callback-free stream URB without waiting for its peer. */
static int
uas_stream_submit(struct drv_usb_uas_transport *transport, unsigned pipe,
    unsigned stream, void *buffer, size_t length)
{
	uint64_t now;
	uint64_t remaining;
	unsigned timeout;
	unsigned flags;
	int error;

	now = sched_ticks();
	if (now >= transport->deadline)
		return ETIMEDOUT;
	remaining = transport->deadline - now;
	timeout = remaining > UINT32_MAX / 10 ? UINT32_MAX : (unsigned)remaining * 10;
	flags = length <= DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE ? DRV_USB_URB_RECLAIM_SAFE : 0;
	error = drv_usb_urb_setup_stream(transport->urbs[pipe], stream, buffer,
	    length, flags, timeout, NULL, NULL);
	if (error == 0)
		error = drv_usb_urb_submit(transport->urbs[pipe]);
	return error;
}

/* SuperSpeed has no READY IU: both status and data must be admitted together. */
static int
uas_super_transfer(struct drv_usb_uas_transport *transport,
    struct drv_usb_uas_command *command, uint8_t *status, void *buffer, size_t length)
{
	unsigned pipe;
	int status_started;
	int data_started;
	int status_done;
	int data_done;
	int error;
	size_t actual;
	size_t transferred;

	pipe = command->direction == DRV_USB_UAS_READ ? 2 : 3;
	status_started = 0;
	data_started = 0;
	status_done = 0;
	data_done = length == 0;
	transferred = 0;
	error = uas_stream_submit(transport, 1, command->tag, status, 512);
	if (error != 0)
		return error;
	status_started = 1;
	if (length != 0) {
		error = uas_stream_submit(transport, pipe, command->tag, buffer, length);
		if (error != 0)
			goto fail;
		data_started = 1;
	}
	command->data_seen = length != 0;
	while (!status_done || !data_done) {
		if (!status_done && drv_usb_urb_status(transport->urbs[1]) != DRV_USB_URB_PENDING) {
			error = drv_usb_urb_wait_reusable(transport->urbs[1]);
			status_started = 0;
			if (error != 0)
				goto fail;
			actual = drv_usb_urb_actual_length(transport->urbs[1]);
			if (actual > 512) {
				error = EIO;
				goto fail;
			}
			error = drv_usb_uas_command_status(command, status, actual);
			if (error != 0 || command->state != DRV_USB_UAS_COMPLETE) {
				error = EIO;
				goto fail;
			}
			status_done = 1;
			if (command->scsi_status != 0 && !data_done) {
				(void)drv_usb_urb_cancel(transport->urbs[pipe]);
				(void)drv_usb_urb_wait_reusable(transport->urbs[pipe]);
				data_started = 0;
				error = drv_usb_urb_drain(transport->urbs[pipe], 1000);
				if (error != 0)
					goto fail;
				data_done = 1;
			}
		}
		if (!data_done && drv_usb_urb_status(transport->urbs[pipe]) != DRV_USB_URB_PENDING) {
			error = drv_usb_urb_wait_reusable(transport->urbs[pipe]);
			data_started = 0;
			if (error != 0)
				goto fail;
			transferred = drv_usb_urb_actual_length(transport->urbs[pipe]);
			if (transferred > length) {
				error = EIO;
				goto fail;
			}
			data_done = 1;
		}
		if (status_done && data_done)
			break;
		if (sched_ticks() >= transport->deadline) {
			error = ETIMEDOUT;
			goto fail;
		}
		sched_yield();
	}
	command->transferred = transferred;
	return 0;

fail:
	/* wait_reusable detaches caller buffers even when HCD staging is retained. */
	if (data_started) {
		(void)drv_usb_urb_cancel(transport->urbs[pipe]);
		(void)drv_usb_urb_wait_reusable(transport->urbs[pipe]);
	}
	if (status_started) {
		(void)drv_usb_urb_cancel(transport->urbs[1]);
		(void)drv_usb_urb_wait_reusable(transport->urbs[1]);
	}
	return error;
}

static int
uas_transfer(struct drv_usb_uas_transport *transport, unsigned pipe,
    void *buffer, size_t length, size_t *actual)
{
	uint64_t now;
	uint64_t remaining;
	unsigned flags;
	unsigned timeout;
	int error;

	*actual = 0;
	now = sched_ticks();
	if (now >= transport->deadline)
		return ETIMEDOUT;
	remaining = transport->deadline - now;
	timeout = remaining > UINT32_MAX / 10 ? UINT32_MAX : (unsigned)remaining * 10;
	flags = length <= DRV_USB_URB_RECLAIM_SAFE_MAX_SIZE ? DRV_USB_URB_RECLAIM_SAFE : 0;
	error = drv_usb_urb_setup(transport->urbs[pipe], buffer, length, flags,
	    timeout, NULL, NULL);
	if (error == 0)
		error = drv_usb_urb_submit(transport->urbs[pipe]);
	if (error == 0)
		error = drv_usb_urb_wait_reusable(transport->urbs[pipe]);
	if (error != 0)
		return error;
	*actual = drv_usb_urb_actual_length(transport->urbs[pipe]);
	if (*actual > length)
		return EIO;
	return 0;
}
