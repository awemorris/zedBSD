/*
 * USB Attached SCSI descriptor capabilities.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#ifndef KERN_DRIVERS_USB_UAS_H
#define KERN_DRIVERS_USB_UAS_H

#include <stddef.h>
#include <stdint.h>

enum drv_usb_uas_profile {
	DRV_USB_UAS_HIGH_SPEED,
	DRV_USB_UAS_SUPER_SPEED
};

struct drv_usb_uas_pipe {
	uint8_t address;
	uint8_t maximum_burst;
	uint8_t stream_exponent;
	uint16_t maximum_packet;
};

struct drv_usb_uas_capabilities {
	/* Pipe Usage IDs 1 through 4: command, status, data-in, data-out. */
	struct drv_usb_uas_pipe pipes[4];
	uint8_t interface_number;
	uint8_t alternate_setting;
	uint8_t minimum_stream_exponent;
};

/*
 * Decodes one alternate from a complete configuration. A valid capability
 * description does not imply that the host implements its stream transport.
 * Failure clears the output; input and output must not overlap.
 */
int drv_usb_uas_decode_configuration(const void *raw, size_t length,
    unsigned interface_number, unsigned alternate_setting,
    enum drv_usb_uas_profile profile, struct drv_usb_uas_capabilities *result);

/* One high-speed command, owned and serialized by the transport until retired.
 * Zero-initialize only after host DMA and any previous device task are retired.
 * FAILED is terminal: clearing this object alone does not retire a task/tag. */
enum drv_usb_uas_command_state {
	DRV_USB_UAS_IDLE,
	DRV_USB_UAS_WAIT_STATUS,
	DRV_USB_UAS_DATA,
	DRV_USB_UAS_COMPLETE,
	DRV_USB_UAS_FAILED
};

enum drv_usb_uas_direction {
	DRV_USB_UAS_NO_DATA,
	DRV_USB_UAS_READ,
	DRV_USB_UAS_WRITE
};

struct drv_usb_uas_command {
	enum drv_usb_uas_command_state state;
	enum drv_usb_uas_direction direction;
	size_t expected;
	size_t transferred;
	uint16_t tag;
	int data_seen;
	uint8_t scsi_status;
};

/* Output command IU is exactly 32 bytes. No allocation or USB operation.
 * Inputs and outputs must not overlap. Invalid begin leaves state unchanged. */
int drv_usb_uas_command_begin(struct drv_usb_uas_command *command,
    uint16_t tag, unsigned lun, const void *cdb, size_t cdb_length,
    enum drv_usb_uas_direction direction, size_t expected, uint8_t wire[32]);
/* A successful call publishes DATA or COMPLETE. COMPLETE is transport success,
 * not SCSI success: inspect scsi_status. Sense bytes remain in the caller's IU.
 * Invalid status poisons state, returning EIO. */
int drv_usb_uas_command_status(struct drv_usb_uas_command *command,
    const void *wire, size_t length);
int drv_usb_uas_command_data(struct drv_usb_uas_command *command, size_t actual);
/* Call on timeout, cancellation, or any failed USB transfer before recovery. */
void drv_usb_uas_command_fail(struct drv_usb_uas_command *command);

struct drv_usb_device;
struct drv_usb_endpoint;
struct drv_usb_urb;

/* Serialized by the class owner, including init/execute/stop. Zero-initialize
 * before first init. No endpoint/device may disappear before successful stop.
 * A failed stop retains every URB. Reinitialization requires device-task reset,
 * not merely successful host drain. */
struct drv_usb_uas_transport {
	struct drv_usb_urb *urbs[4];
	size_t capacity;
	uint32_t next_tag;
	uint64_t deadline;
	int stopped;
	uint16_t failed_tag;
	uint8_t failed_lun;
	int recovery_attempted;
	int super_speed;
};

/* Caller-owned completion; sense is truncated to 252 bytes with length reported.
 * error-free transport may still return nonzero SCSI status. */
struct drv_usb_uas_result {
	size_t transferred;
	size_t sense_length;
	uint8_t status;
	uint8_t sense[252];
};

int drv_usb_uas_transport_init(struct drv_usb_uas_transport *transport,
    struct drv_usb_device *device, struct drv_usb_endpoint *pipes[4],
    size_t capacity);
int drv_usb_uas_transport_execute(struct drv_usb_uas_transport *transport,
    unsigned lun, const void *cdb, size_t cdb_length,
    enum drv_usb_uas_direction direction, void *buffer, size_t length,
    unsigned timeout_ms, struct drv_usb_uas_result *result);
/* One bounded ABORT TASK attempt after a failed command. Requires class-owner
 * serialization, and never retries the failed CDB or clears write uncertainty. */
int drv_usb_uas_transport_recover(struct drv_usb_uas_transport *transport,
    unsigned timeout_ms);
int drv_usb_uas_transport_stop(struct drv_usb_uas_transport *transport);

int drv_usb_uas_driver_register(void);

#endif
