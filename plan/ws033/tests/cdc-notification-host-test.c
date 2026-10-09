/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the USB CDC notification reader (BUG-222,
 * src/drivers/usb/usb-cdc-notification.c), built with the host's compiler
 * under ASan and UBSan: whole notifications in one transfer, a speed's
 * notification split over 8-byte packets and behind a NetworkConnection,
 * bytes that start no notification, a notification too long, a transfer
 * too long, and the transfer length asked of an endpoint.
 *
 *   plan/ws033/tests/cdc-notification-host-test.sh
 */

#include <drivers/usb/usb-cdc-notification.h>

#include <stdio.h>
#include <string.h>

/* The checks that failed, and those that ran. */
static unsigned failures;
static unsigned checks;

/* NetworkConnection, connected, of interface 0. */
static const uint8_t connected[8] = { 0xa1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* ConnectionSpeedChange of interface 1: 2.5 Gb/s down, 1 Gb/s up. */
static const uint8_t speed[16] = {
	0xa1, 0x2a, 0x00, 0x00, 0x01, 0x00, 0x08, 0x00,
	0x00, 0xf9, 0x02, 0x95, 0x00, 0xca, 0x9a, 0x3b
};

static void check(int condition, const char *what);
static void test_whole(void);
static void test_split(void);
static void test_garbage(void);
static void test_lengths(void);

/*
 * Runs every part and reports the checks.
 */
int
main(void)
{
	/* Each part. */
	test_whole();
	test_split();
	test_garbage();
	test_lengths();

	/* The count of what failed. */
	printf("cdc-notification-host-test: %u checks, %u failed\n", checks, failures);
	if (failures != 0U)
		return 1;

	/* Succeeded: every check held. */
	return 0;
}

/* Counts a check and reports the one that failed. */
static void
check(
	int condition,
	const char *what)
{
	/* Counted. */
	checks++;
	if (condition)
		return;

	/* Reported. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Each notification in a transfer of its own. */
static void
test_whole(void)
{
	struct drv_usb_cdc_notification_reader reader;
	struct drv_usb_cdc_notification notification;
	int whole;

	/* The connection. */
	drv_usb_cdc_notification_reset(&reader);
	drv_usb_cdc_notification_add(&reader, connected, sizeof(connected));
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1, "whole: the connection taken");
	check(notification.code == DRV_USB_CDC_NETWORK_CONNECTION && notification.value == 1U, "whole: connected");
	check(notification.interface_number == 0U && notification.data_length == 0U, "whole: interface 0, no data");
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 0, "whole: nothing more");

	/* The speed. */
	drv_usb_cdc_notification_add(&reader, speed, sizeof(speed));
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1, "whole: the speed taken");
	check(notification.code == DRV_USB_CDC_CONNECTION_SPEED_CHANGE && notification.interface_number == 1U, "whole: a speed of interface 1");
	check(notification.downstream_bps == 2500000000U && notification.upstream_bps == 1000000000U, "whole: 2.5 Gb/s down, 1 Gb/s up");
	check(reader.length == 0U, "whole: nothing kept");
}

/* The connection and the speed as one stream, in 8-byte packets and in a 16-byte transfer with the rest after it. */
static void
test_split(void)
{
	struct drv_usb_cdc_notification_reader reader;
	struct drv_usb_cdc_notification notification;
	uint8_t stream[24];
	int whole;

	/* The stream: the connection, then the speed. */
	memcpy(stream, connected, sizeof(connected));
	memcpy(stream + 8, speed, sizeof(speed));

	/* 8-byte packets: the connection at once, the speed after its second half. */
	drv_usb_cdc_notification_reset(&reader);
	drv_usb_cdc_notification_add(&reader, stream, 8U);
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1 && notification.code == DRV_USB_CDC_NETWORK_CONNECTION, "split: the connection from its packet");
	drv_usb_cdc_notification_add(&reader, stream + 8, 8U);
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 0, "split: the speed's header alone waits");
	drv_usb_cdc_notification_add(&reader, stream + 16, 8U);
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1 && notification.downstream_bps == 2500000000U, "split: the speed after its second half");

	/* A 16-byte transfer of the connection and the speed's header, then the speed's data. */
	drv_usb_cdc_notification_reset(&reader);
	drv_usb_cdc_notification_add(&reader, stream, 16U);
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1 && notification.code == DRV_USB_CDC_NETWORK_CONNECTION, "split: the connection in front");
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 0, "split: the speed waits for its data");
	drv_usb_cdc_notification_add(&reader, stream + 16, 8U);
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1 && notification.upstream_bps == 1000000000U, "split: the speed joined");
}

/* Bytes that start no notification, a notification too long, and a transfer too long are dropped. */
static void
test_garbage(void)
{
	struct drv_usb_cdc_notification_reader reader;
	struct drv_usb_cdc_notification notification;
	uint8_t bytes[40];
	int whole;

	/* A transfer that starts no notification is dropped; the next one is taken. */
	drv_usb_cdc_notification_reset(&reader);
	memset(bytes, 0x55, sizeof(bytes));
	drv_usb_cdc_notification_add(&reader, bytes, 8U);
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 0 && reader.length == 0U, "garbage: dropped");
	drv_usb_cdc_notification_add(&reader, connected, sizeof(connected));
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1, "garbage: the next notification taken");

	/* A notification with more data than any network function sends. */
	memcpy(bytes, connected, sizeof(connected));
	bytes[6] = 0x40U;
	drv_usb_cdc_notification_add(&reader, bytes, 16U);
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 0 && reader.length == 0U, "garbage: too long dropped");

	/* A transfer longer than the reader holds. */
	drv_usb_cdc_notification_add(&reader, bytes, sizeof(bytes));
	check(reader.length == 0U, "garbage: a transfer too long dropped");

	/* A transfer that does not fit behind what is kept starts again. */
	drv_usb_cdc_notification_add(&reader, speed, 8U);
	drv_usb_cdc_notification_add(&reader, speed, 8U);
	drv_usb_cdc_notification_add(&reader, speed, 8U);
	drv_usb_cdc_notification_add(&reader, speed, sizeof(speed));
	check(reader.length == sizeof(speed), "garbage: started again from the transfer");
	whole = drv_usb_cdc_notification_next(&reader, &notification);
	check(whole == 1 && notification.downstream_bps == 2500000000U, "garbage: the speed after starting again");
}

/* The transfer asked of an endpoint: one packet of a short one, else a whole notification. */
static void
test_lengths(void)
{
	/* The endpoint's packet sizes. */
	check(drv_usb_cdc_notification_transfer_length(8U) == 8U, "lengths: 8-byte packets");
	check(drv_usb_cdc_notification_transfer_length(16U) == 16U, "lengths: 16-byte packets");
	check(drv_usb_cdc_notification_transfer_length(64U) == 16U, "lengths: 64-byte packets");
	check(drv_usb_cdc_notification_transfer_length(0U) == 16U, "lengths: an unknown size");
}
