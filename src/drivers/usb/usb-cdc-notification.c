/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reader of a USB CDC network function's notifications (BUG-222):
 * joins the interrupt endpoint's transfers and cuts them into whole
 * notifications, for the ECM and NCM drivers alike.
 */

#include <drivers/usb/usb-cdc-notification.h>
#include <kern/kcrt.h>

static uint16_t notification_le16(const uint8_t *bytes);
static uint32_t notification_le32(const uint8_t *bytes);

/*
 * Empties the reader.
 *
 * A driver calls it when its adapter opens and when a transfer failed, so
 * the start of a notification never joins bytes from after a gap.
 */
void
drv_usb_cdc_notification_reset(
	struct drv_usb_cdc_notification_reader *reader)
{
	/* Nothing is kept. */
	reader->length = 0U;
}

/*
 * Adds the bytes of one completed transfer to what the reader keeps.
 *
 * Bytes that do not fit mean the stream is not one the reader
 * understands; it starts again from the new transfer, or drops it too
 * when the transfer alone does not fit.
 */
void
drv_usb_cdc_notification_add(
	struct drv_usb_cdc_notification_reader *reader,
	const uint8_t *bytes,
	size_t length)
{
	size_t room;

	/* Starts again when the transfer does not fit behind what is kept. */
	room = sizeof(reader->bytes) - reader->length;
	if (length > room)
		reader->length = 0U;

	/* Drops a transfer longer than the whole buffer. */
	if (length > sizeof(reader->bytes))
		return;

	/* Keeps the transfer behind the earlier bytes. */
	kern_memcpy(reader->bytes + reader->length, bytes, length);
	reader->length += length;
}

/*
 * Takes the next whole notification from the reader.
 *
 * Returns 1 with the notification filled in, or 0 when no whole one is
 * kept yet (or what is kept is not a notification and was dropped).
 */
int
drv_usb_cdc_notification_next(
	struct drv_usb_cdc_notification_reader *reader,
	struct drv_usb_cdc_notification *notification)
{
	size_t total;
	uint16_t data_length;

	/* Waits for a whole header. */
	if (reader->length < DRV_USB_CDC_NOTIFICATION_HEADER)
		return 0;

	/* Drops bytes that do not start a notification: the stream lost its place. */
	if (reader->bytes[0] != DRV_USB_CDC_NOTIFICATION_REQUEST_TYPE) {
		reader->length = 0U;
		return 0;
	}

	/* Drops a notification longer than any a network function sends. */
	data_length = notification_le16(reader->bytes + 6U);
	if (data_length > DRV_USB_CDC_NOTIFICATION_MAX - DRV_USB_CDC_NOTIFICATION_HEADER) {
		reader->length = 0U;
		return 0;
	}

	/* Waits for the rest of its data. */
	total = DRV_USB_CDC_NOTIFICATION_HEADER + data_length;
	if (reader->length < total)
		return 0;

	/* The header: the code, the value, the interface and the data's length. */
	notification->code = reader->bytes[1];
	notification->value = notification_le16(reader->bytes + 2U);
	notification->interface_number = notification_le16(reader->bytes + 4U);
	notification->data_length = data_length;

	/* A speed's two directions, downstream first (CDC ECM 1.2 table 21). */
	notification->downstream_bps = 0U;
	notification->upstream_bps = 0U;
	if (notification->code == DRV_USB_CDC_CONNECTION_SPEED_CHANGE && data_length == 8U) {
		notification->downstream_bps = notification_le32(reader->bytes + 8U);
		notification->upstream_bps = notification_le32(reader->bytes + 12U);
	}

	/* Moves the bytes after it to the front. */
	kern_memmove(reader->bytes, reader->bytes + total, reader->length - total);
	reader->length -= total;

	/* Succeeded: one notification taken. */
	return 1;
}

/*
 * Tells how long a transfer to ask of an interrupt endpoint of a packet
 * size.
 *
 * A transfer ends at a short packet or when its buffer is full.  Asking
 * for more than one packet of an 8-byte endpoint would keep the second
 * half of the speed's notification waiting in the transfer until the next
 * notification, so such an endpoint is read one packet at a time.
 */
size_t
drv_usb_cdc_notification_transfer_length(
	uint16_t max_packet_size)
{
	/* An endpoint of short packets: one packet a transfer. */
	if (max_packet_size != 0U && max_packet_size < DRV_USB_CDC_NOTIFICATION_TRANSFER)
		return max_packet_size;

	/* Succeeded: a whole notification fits one transfer. */
	return DRV_USB_CDC_NOTIFICATION_TRANSFER;
}

/* Reads a little-endian 16-bit field. */
static uint16_t
notification_le16(
	const uint8_t *bytes)
{
	uint16_t value;

	/* The low byte, then the high one. */
	value = (uint16_t)bytes[0];
	value = (uint16_t)(value | ((uint16_t)bytes[1] << 8));

	/* The field's value. */
	return value;
}

/* Reads a little-endian 32-bit field. */
static uint32_t
notification_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* The bytes from the lowest. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* The field's value. */
	return value;
}
