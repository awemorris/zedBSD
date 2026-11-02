/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The xHCI endpoint context of interrupt endpoints below SuperSpeed, on the
 * host (include/drivers/pci/pci-xhci-control.h): the interval rounded down to
 * a power of two of 125 us and kept to 3..10 at full and low speed, bInterval
 * - 1 at high speed, Max ESIT Payload and Average TRB Length both
 * Max Packet Size x (Max Burst + 1), and control and bulk unchanged.  The
 * values are xHCI 1.2's 6.2.3.4, 6.2.3.6 and 6.2.3.8.
 */

#include <drivers/pci/pci-xhci-control.h>

#include <stdio.h>

static unsigned failures;
static unsigned checks;

static void check(int good, const char *what);
static unsigned interval_of(enum drv_usb_speed speed, uint16_t packet, uint8_t binterval, struct drv_xhci_endpoint_context_words *words);

int
main(
	void)
{
	struct drv_xhci_endpoint_context_words words;
	unsigned interval;

	/* A full-speed mouse endpoint of 64 bytes and bInterval 1. */
	interval = interval_of(DRV_USB_SPEED_FULL, 64U, 1U, &words);
	check(interval == 3U, "full speed bInterval 1 -> 3 (1 ms)");
	check(words.word4 == ((64U << 16) | 64U), "full speed ESIT payload and average TRB 64");
	check(((words.word1 >> 3) & 7U) == 7U && ((words.word1 >> 1) & 3U) == 3U, "interrupt IN, CErr 3");
	check(((words.word1 >> 8) & 0xffU) == 0U && (words.word1 >> 16) == 64U, "no burst, packet 64");

	/* Full speed rounds down: 10 ms -> 8 ms, 2 -> 2, 3 -> 2, 255 -> 128, 0 read as 1. */
	check(interval_of(DRV_USB_SPEED_FULL, 8U, 10U, &words) == 6U, "full speed bInterval 10 -> 6 (8 ms)");
	check(interval_of(DRV_USB_SPEED_FULL, 8U, 2U, &words) == 4U, "full speed bInterval 2 -> 4");
	check(interval_of(DRV_USB_SPEED_FULL, 8U, 3U, &words) == 4U, "full speed bInterval 3 -> 4 (2 ms)");
	check(interval_of(DRV_USB_SPEED_FULL, 8U, 128U, &words) == 10U, "full speed bInterval 128 -> 10");
	check(interval_of(DRV_USB_SPEED_FULL, 8U, 255U, &words) == 10U, "full speed bInterval 255 -> 10 (kept to 10)");
	check(interval_of(DRV_USB_SPEED_FULL, 8U, 0U, &words) == 3U, "full speed bInterval 0 -> 3");
	check(interval_of(DRV_USB_SPEED_LOW, 8U, 10U, &words) == 6U, "low speed bInterval 10 -> 6");
	check(words.word4 == ((8U << 16) | 8U), "low speed ESIT payload 8");

	/* High speed: bInterval - 1, and the extra transactions a microframe. */
	check(interval_of(DRV_USB_SPEED_HIGH, 64U, 4U, &words) == 3U, "high speed bInterval 4 -> 3");
	check(interval_of(DRV_USB_SPEED_HIGH, 64U, 16U, &words) == 15U, "high speed bInterval 16 -> 15");
	check(interval_of(DRV_USB_SPEED_HIGH, 0x0800U | 1024U, 1U, &words) == 0U, "high bandwidth interval 0");
	check(((words.word1 >> 8) & 0xffU) == 1U && words.word4 == ((2048U << 16) | 2048U), "high bandwidth burst 1, ESIT 2048");
	(void)interval_of(DRV_USB_SPEED_HIGH, 0x1800U | 1024U, 1U, &words);
	check(((words.word1 >> 8) & 0xffU) == 2U && words.word4 == ((3072U << 16) | 3072U), "reserved mult 3 read as 2");

	/* Control and bulk carry no interval and keep their average TRB lengths. */
	check(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_HIGH, 4U, 64U, 0U, NULL, &words) &&
	      words.word0 == 0U && words.word4 == DRV_XHCI_CONTROL_AVERAGE_TRB_LENGTH, "control unchanged");
	check(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_HIGH, 6U, 512U, 0U, NULL, &words) &&
	      words.word0 == 0U && words.word4 == 512U && ((words.word1 >> 3) & 7U) == 6U, "bulk unchanged");

	/* Interrupt OUT is encoded the same way. */
	check(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_FULL, 3U, 32U, 1U, NULL, &words) &&
	      ((words.word0 >> 16) & 0xffU) == 3U && words.word4 == ((32U << 16) | 32U), "interrupt OUT interval and ESIT");

	/* The summary. */
	if (failures != 0) {
		printf("host-xhci-interrupt: FAIL %u/%u\n", failures, checks);
		return 1;
	}
	printf("host-xhci-interrupt: PASS %u/%u\n", checks, checks);
	return 0;
}

/* Counts one check and prints a failed one. */
static void
check(
	int good,
	const char *what)
{
	checks++;
	if (!good) {
		failures++;
		printf("FAIL: %s\n", what);
		return;
	}
	printf("ok: %s\n", what);
}

/* Encodes an interrupt IN endpoint and gives its Interval field (99 when refused). */
static unsigned
interval_of(
	enum drv_usb_speed speed,
	uint16_t packet,
	uint8_t binterval,
	struct drv_xhci_endpoint_context_words *words)
{
	int made;

	made = drv_xhci_endpoint_context_encode(speed, 7U, packet, binterval, NULL, words);
	if (!made)
		return 99U;
	return (words->word0 >> 16) & 0xffU;
}
