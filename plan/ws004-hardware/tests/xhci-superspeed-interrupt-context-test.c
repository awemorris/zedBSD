/*
 * WS004 HW-T24 SuperSpeed interrupt endpoint-context fixture.
 *
 * SPDX-License-Identifier: Zlib
 */
#include <drivers/pci-xhci-control.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "check %u failed at %s:%d: %s\n", checks, \
		    __FILE__, __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

static struct drv_usb_superspeed_endpoint_companion_descriptor
companion(unsigned maximum_burst, unsigned attributes, unsigned payload)
{
	struct drv_usb_superspeed_endpoint_companion_descriptor descriptor = {
		.length = 6,
		.descriptor_type =
		    DRV_USB_DESCRIPTOR_SUPERSPEED_ENDPOINT_COMPANION,
		.maximum_burst = (uint8_t)maximum_burst,
		.attributes = (uint8_t)attributes,
		.bytes_per_interval = (uint16_t)payload
	};

	return descriptor;
}

static void
check_rejected(uint16_t packet, uint8_t interval,
	const struct drv_usb_superspeed_endpoint_companion_descriptor *descriptor)
{
	struct drv_xhci_endpoint_context_words words = {
		.word0 = UINT32_C(0xaaaaaaaa),
		.word1 = UINT32_C(0xbbbbbbbb),
		.word4 = UINT32_C(0xcccccccc)
	};

	CHECK(!drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 7U,
	    packet, interval, descriptor, &words));
	CHECK(words.word0 == UINT32_C(0xaaaaaaaa));
	CHECK(words.word1 == UINT32_C(0xbbbbbbbb));
	CHECK(words.word4 == UINT32_C(0xcccccccc));
}

int
main(void)
{
	struct drv_usb_superspeed_endpoint_companion_descriptor descriptor;
	struct drv_xhci_endpoint_context_words words;

	/* RTL8156 notification endpoint: IN EP3, bInterval 11. */
	descriptor = companion(0, 0, 16);
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 7U, 16U,
	    11U, &descriptor, &words));
	CHECK(words.word0 == UINT32_C(0x000a0000));
	CHECK(words.word1 == UINT32_C(0x0010003e));
	CHECK(words.word4 == UINT32_C(0x00100010));

	/* Legal under-reporting is retained exactly, never rounded to packet. */
	descriptor.bytes_per_interval = 15U;
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 7U, 16U,
	    11U, &descriptor, &words));
	CHECK(words.word4 == UINT32_C(0x000f000f));
	descriptor.bytes_per_interval = 17U;
	check_rejected(16U, 11U, &descriptor);
	descriptor.bytes_per_interval = 0;
	check_rejected(16U, 11U, &descriptor);

	descriptor = companion(16U, 0, 16U);
	check_rejected(16U, 11U, &descriptor);
	descriptor = companion(0, 1U, 16U);
	check_rejected(16U, 11U, &descriptor);
	descriptor = companion(0, 0, 16U);
	check_rejected(0, 11U, &descriptor);
	check_rejected(1025U, 11U, &descriptor);
	check_rejected(UINT16_C(0x0810), 11U, &descriptor);
	check_rejected(16U, 0, &descriptor);
	check_rejected(16U, 17U, &descriptor);
	check_rejected(16U, 11U, NULL);
	CHECK(!drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 7U, 16U,
	    11U, &descriptor, NULL));

	/* xHCI 1.2b section 4.14.2 caps SS interrupt ESIT payload at 3 KiB. */
	descriptor = companion(2U, 0, 3072U);
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 7U,
	    1024U, 16U, &descriptor, &words));
	CHECK(words.word0 == UINT32_C(0x000f0000));
	CHECK(words.word1 == UINT32_C(0x0400023e));
	CHECK(words.word4 == UINT32_C(0x0c000c00));
	/* Keep capacity above 3 KiB so this isolates the architectural ceiling. */
	descriptor = companion(15U, 0, 3073U);
	check_rejected(1024U, 16U, &descriptor);

	/* OUT interrupt endpoints use the same strict companion contract. */
	descriptor = companion(0, 0, 16U);
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 3U, 16U,
	    11U, &descriptor, &words));
	CHECK(words.word1 == UINT32_C(0x0010001e));
	CHECK(words.word4 == UINT32_C(0x00100010));

	/* Non-target endpoint kinds retain their prior context words. */
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_HIGH, 7U, 16U,
	    11U, NULL, &words));
	CHECK(words.word0 == UINT32_C(0x000a0000));
	CHECK(words.word1 == UINT32_C(0x0010003e));
	CHECK(words.word4 == UINT32_C(0x00000010));
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_FULL, 7U, 16U,
	    11U, NULL, &words));
	CHECK(words.word0 == UINT32_C(0x00070000));
	CHECK(words.word1 == UINT32_C(0x0010003e));
	CHECK(words.word4 == UINT32_C(0x00000010));

	/* SuperSpeedPlus LEC/SSP is explicitly outside p021. */
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER_PLUS, 7U,
	    16U, 11U, &descriptor, &words));
	CHECK(words.word0 == UINT32_C(0x000a0000));
	CHECK(words.word1 == UINT32_C(0x0010003e));
	CHECK(words.word4 == UINT32_C(0x00000010));

	descriptor = companion(3U, 0xffU, 0);
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 6U, 512U,
	    17U, &descriptor, &words));
	CHECK(words.word0 == 0);
	CHECK(words.word1 == UINT32_C(0x02000336));
	CHECK(words.word4 == UINT32_C(0x00000200));
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_SUPER, 5U, 1024U,
	    11U, &descriptor, &words));
	CHECK(words.word0 == UINT32_C(0x000a0000));
	CHECK(words.word1 == UINT32_C(0x0400032e));
	CHECK(words.word4 == UINT32_C(0x00000400));
	CHECK(drv_xhci_endpoint_context_encode(DRV_USB_SPEED_HIGH, 4U, 64U,
	    255U, NULL, &words));
	CHECK(words.word0 == 0);
	CHECK(words.word1 == UINT32_C(0x00400026));
	CHECK(words.word4 == DRV_XHCI_CONTROL_AVERAGE_TRB_LENGTH);

	printf("xHCI SuperSpeed interrupt context: %u checks passed\n", checks);
	return 0;
}
