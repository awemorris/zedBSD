/*
 * WS003 BR-T27 xHCI control-transfer, EP0, and port-reset fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-xhci-control.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_TYPE_MASK	0x0000fc00U

static void
assert_setup_reserved_zero(const struct drv_xhci_trb_words *words)
{
	assert(words->status == 8U);
	assert((words->status & DRV_XHCI_TRB_TD_SIZE_MASK) == 0);
	assert((words->control & DRV_XHCI_TRB_CHAIN) == 0);
	assert((words->control & DRV_XHCI_TRB_IOC) == 0);
	assert((words->control & TEST_TYPE_MASK) == DRV_XHCI_TRB_TYPE(2));
}

static void
assert_data_reserved_zero(const struct drv_xhci_trb_words *words,
	uint32_t length)
{
	assert(words->status == length);
	assert((words->status & DRV_XHCI_TRB_TD_SIZE_MASK) == 0);
	assert((words->control & DRV_XHCI_TRB_CHAIN) == 0);
	assert((words->control & DRV_XHCI_TRB_IOC) == 0);
	assert((words->control & TEST_TYPE_MASK) == DRV_XHCI_TRB_TYPE(3));
}

static void
assert_status_words(const struct drv_xhci_trb_words *words,
	uint32_t expected_control)
{
	assert(words->parameter_low == 0);
	assert(words->parameter_high == 0);
	assert(words->status == 0);
	assert(words->control == expected_control);
	assert((words->control & DRV_XHCI_TRB_CHAIN) == 0);
	assert((words->control & TEST_TYPE_MASK) == DRV_XHCI_TRB_TYPE(4));
}

static void
test_no_data_control(void)
{
	const uint64_t setup = UINT64_C(0x8877665544332211);
	struct drv_xhci_trb_words words;

	assert(drv_xhci_control_setup_words(setup,
	    DRV_XHCI_CONTROL_NO_DATA, &words));
	assert(words.parameter_low == 0x44332211U);
	assert(words.parameter_high == 0x88776655U);
	assert_setup_reserved_zero(&words);
	assert(words.control == 0x00000840U);

	assert(drv_xhci_control_status_words(DRV_XHCI_CONTROL_NO_DATA,
	    &words));
	assert_status_words(&words, 0x00011020U);
}

static void
test_out_data_control(void)
{
	const uint64_t setup = UINT64_C(0x0800000001000900);
	const uint64_t dma = UINT64_C(0x1234567887654000);
	struct drv_xhci_trb_words words;

	assert(drv_xhci_control_setup_words(setup,
	    DRV_XHCI_CONTROL_DATA_OUT, &words));
	assert_setup_reserved_zero(&words);
	assert(words.control == 0x00020840U);

	assert(drv_xhci_control_data_words(dma, 8U,
	    DRV_XHCI_CONTROL_DATA_OUT, &words));
	assert(words.parameter_low == 0x87654000U);
	assert(words.parameter_high == 0x12345678U);
	assert_data_reserved_zero(&words, 8U);
	assert(words.control == 0x00000c00U);

	assert(drv_xhci_control_status_words(DRV_XHCI_CONTROL_DATA_OUT,
	    &words));
	assert_status_words(&words, 0x00011020U);
}

static void
test_in_data_control(void)
{
	const uint64_t setup = UINT64_C(0x0800000001000680);
	const uint64_t dma = UINT64_C(0xfedcba9876540000);
	struct drv_xhci_trb_words words;

	assert(drv_xhci_control_setup_words(setup,
	    DRV_XHCI_CONTROL_DATA_IN, &words));
	assert_setup_reserved_zero(&words);
	assert(words.control == 0x00030840U);

	assert(drv_xhci_control_data_words(dma, 64U,
	    DRV_XHCI_CONTROL_DATA_IN, &words));
	assert(words.parameter_low == 0x76540000U);
	assert(words.parameter_high == 0xfedcba98U);
	assert_data_reserved_zero(&words, 64U);
	assert(words.control == 0x00010c04U);

	assert(drv_xhci_control_status_words(DRV_XHCI_CONTROL_DATA_IN,
	    &words));
	assert_status_words(&words, 0x00001020U);
}

static void
test_control_short_event(void)
{
	assert(drv_xhci_control_short_data_event(1, 1U, 3U, 13U));
	assert(!drv_xhci_control_short_data_event(0, 1U, 3U, 13U));
	assert(!drv_xhci_control_short_data_event(1, 0U, 3U, 13U));
	assert(!drv_xhci_control_short_data_event(1, 2U, 3U, 13U));
	assert(!drv_xhci_control_short_data_event(1, 1U, 2U, 13U));
	assert(!drv_xhci_control_short_data_event(1, 1U, 3U, 1U));
}

static void
test_invalid_control_words(void)
{
	struct drv_xhci_trb_words words;

	assert(!drv_xhci_control_setup_words(0,
	    (enum drv_xhci_control_data)99, &words));
	assert(!drv_xhci_control_setup_words(0,
	    DRV_XHCI_CONTROL_NO_DATA, NULL));
	assert(!drv_xhci_control_data_words(0, 0,
	    DRV_XHCI_CONTROL_DATA_IN, &words));
	assert(drv_xhci_control_data_words(0,
	    DRV_XHCI_CONTROL_DATA_MAX_LENGTH,
	    DRV_XHCI_CONTROL_DATA_IN, &words));
	assert_data_reserved_zero(&words, DRV_XHCI_CONTROL_DATA_MAX_LENGTH);
	assert(!drv_xhci_control_data_words(0,
	    DRV_XHCI_CONTROL_DATA_MAX_LENGTH + 1U,
	    DRV_XHCI_CONTROL_DATA_IN, &words));
	assert(!drv_xhci_control_data_words(0, 1,
	    DRV_XHCI_CONTROL_NO_DATA, &words));
	assert(!drv_xhci_control_data_words(0, 1,
	    DRV_XHCI_CONTROL_DATA_OUT, NULL));
	assert(!drv_xhci_control_status_words(
	    (enum drv_xhci_control_data)99, &words));
	assert(!drv_xhci_control_status_words(
	    DRV_XHCI_CONTROL_NO_DATA, NULL));
}

static void
assert_packet(enum drv_usb_speed speed, uint8_t encoded, uint16_t expected)
{
	uint16_t packet = UINT16_MAX;

	assert(drv_xhci_ep0_max_packet_size(speed, encoded, &packet));
	assert(packet == expected);
}

static void
assert_bad_packet(enum drv_usb_speed speed, uint8_t encoded)
{
	uint16_t packet = UINT16_MAX;

	assert(!drv_xhci_ep0_max_packet_size(speed, encoded, &packet));
	assert(packet == 0);
}

static void
test_ep0_packet_sizes(void)
{
	assert(DRV_XHCI_CONTROL_AVERAGE_TRB_LENGTH == 8U);
	assert_packet(DRV_USB_SPEED_LOW, 8U, 8U);
	assert_bad_packet(DRV_USB_SPEED_LOW, 16U);
	assert_packet(DRV_USB_SPEED_FULL, 8U, 8U);
	assert_packet(DRV_USB_SPEED_FULL, 16U, 16U);
	assert_packet(DRV_USB_SPEED_FULL, 32U, 32U);
	assert_packet(DRV_USB_SPEED_FULL, 64U, 64U);
	assert_bad_packet(DRV_USB_SPEED_FULL, 9U);
	assert_packet(DRV_USB_SPEED_HIGH, 64U, 64U);
	assert_bad_packet(DRV_USB_SPEED_HIGH, 8U);
	assert_packet(DRV_USB_SPEED_SUPER, 9U, 512U);
	assert_packet(DRV_USB_SPEED_SUPER_PLUS, 9U, 512U);
	assert_bad_packet(DRV_USB_SPEED_SUPER, 8U);
	assert_bad_packet(DRV_USB_SPEED_SUPER, 64U);
	assert_bad_packet(DRV_USB_SPEED_UNKNOWN, 8U);
	assert(!drv_xhci_ep0_max_packet_size(DRV_USB_SPEED_FULL, 8U, NULL));
}

static void
test_ep0_context(void)
{
	struct drv_xhci_ep0_context_words context;
	uint64_t dequeue = UINT64_C(0x0000001234567811);

	assert(drv_xhci_ep0_context(512U, dequeue, &context));
	assert(context.words[0] == 0);
	assert(context.words[1] == 0x02000026U);
	assert(context.words[2] == 0x34567811U);
	assert(context.words[3] == 0x00000012U);
	assert(context.words[4] == DRV_XHCI_CONTROL_AVERAGE_TRB_LENGTH);
	assert(drv_xhci_ep0_context(64U,
	    UINT64_C(0x0000001234567800), &context));
	assert(context.words[1] == 0x00400026U);
	assert(context.words[2] == 0x34567800U);
	assert(!drv_xhci_ep0_context(0, dequeue, &context));
	assert(!drv_xhci_ep0_context(2048U, dequeue, &context));
	assert(!drv_xhci_ep0_context(64U, dequeue | 2U, &context));
	assert(!drv_xhci_ep0_context(64U, dequeue, NULL));
}

static void
test_superspeed_context_fields(void)
{
	assert(drv_xhci_port_speed_id(4U << 10) == 4U);
	assert(drv_xhci_port_speed_id(5U << 10) == 5U);
	assert(drv_xhci_port_speed_id(UINT32_MAX) == 15U);
	assert(drv_xhci_endpoint_context_word1(6U, 1024U, 0U) ==
	    0x04000036U);
	assert(drv_xhci_endpoint_context_word1(6U, 1024U, 15U) ==
	    0x04000f36U);
}

static void
test_port_reset_status(void)
{
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CCS |
	    DRV_XHCI_PORTSC_PED | DRV_XHCI_PORTSC_PRC) ==
	    DRV_XHCI_PORT_RESET_SUCCESS);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CCS |
	    DRV_XHCI_PORTSC_PED) == DRV_XHCI_PORT_RESET_WAIT);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CCS |
	    DRV_XHCI_PORTSC_PED | DRV_XHCI_PORTSC_PRC | (3U << 5)) ==
	    DRV_XHCI_PORT_RESET_WAIT);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CCS |
	    DRV_XHCI_PORTSC_PED | DRV_XHCI_PORTSC_PRC |
	    DRV_XHCI_PORTSC_PR) ==
	    DRV_XHCI_PORT_RESET_WAIT);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CCS |
	    DRV_XHCI_PORTSC_PR) == DRV_XHCI_PORT_RESET_WAIT);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CCS) ==
	    DRV_XHCI_PORT_RESET_WAIT);
	assert(drv_xhci_port_reset_status(0) ==
	    DRV_XHCI_PORT_RESET_DISCONNECTED);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_PED) ==
	    DRV_XHCI_PORT_RESET_DISCONNECTED);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CCS |
	    DRV_XHCI_PORTSC_PED | DRV_XHCI_PORTSC_PRC |
	    DRV_XHCI_PORTSC_CSC) == DRV_XHCI_PORT_RESET_DISCONNECTED);
	assert(drv_xhci_port_reset_status(DRV_XHCI_PORTSC_CSC) ==
	    DRV_XHCI_PORT_RESET_DISCONNECTED);
	assert(drv_xhci_port_reset_status(UINT32_MAX) ==
	    DRV_XHCI_PORT_RESET_INVALID);
}

static void
test_endpoint_reset_admission(void)
{
	assert(drv_xhci_endpoint_reset_admit(0U, 0U, 0U) ==
	    DRV_XHCI_ENDPOINT_RESET_ACQUIRE);
	assert(drv_xhci_endpoint_reset_admit(1U, 0U, 0U) ==
	    DRV_XHCI_ENDPOINT_RESET_BUSY);
	assert(drv_xhci_endpoint_reset_admit(0U, 1U, 0U) ==
	    DRV_XHCI_ENDPOINT_RESET_BUSY);
	assert(drv_xhci_endpoint_reset_admit(0U, 1U, 1U) ==
	    DRV_XHCI_ENDPOINT_RESET_WAIT_PUBLICATION);
	assert(drv_xhci_endpoint_reset_admit(1U, 1U, 1U) ==
	    DRV_XHCI_ENDPOINT_RESET_BUSY);
	/* A publication marker without its recovery owner is inconsistent and
	 * must never be treated as an idle endpoint. */
	assert(drv_xhci_endpoint_reset_admit(0U, 0U, 1U) ==
	    DRV_XHCI_ENDPOINT_RESET_BUSY);
}

int
main(void)
{
	test_no_data_control();
	test_out_data_control();
	test_in_data_control();
	test_invalid_control_words();
	test_control_short_event();
	test_ep0_packet_sizes();
	test_ep0_context();
	test_superspeed_context_fields();
	test_port_reset_status();
	test_endpoint_reset_admission();
	puts("xHCI control/EP0/reset test: PASS");
	return 0;
}
