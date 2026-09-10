/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdio.h>
#include "../../../src/drivers/usb/usb.c"

void io_stats_record(enum io_stat_event event, uint64_t bytes)
{ (void)event; (void)bytes; }

int main(void)
{
	struct drv_usb_urb urb, saved;
	struct drv_usb_device device;
	struct drv_usb_endpoint endpoint;
	struct drv_usb_bus bus;
	struct drv_usb_hcd hcd;
	struct drv_usb_control_request control = { 0 };
	unsigned char buffer[8];

	memset(&urb, 0, sizeof(urb)); memset(&device, 0, sizeof(device));
	memset(&endpoint, 0, sizeof(endpoint)); memset(&bus, 0, sizeof(bus));
	memset(&hcd, 0, sizeof(hcd));
	urb.device = &device; urb.endpoint = &endpoint; device.bus = &bus;
	bus.hcd = &hcd; device.speed = DRV_USB_SPEED_SUPER;
	endpoint.type = DRV_USB_TRANSFER_BULK;
	endpoint.companion_valid = 1; endpoint.companion.attributes = 4;
	assert(drv_usb_urb_setup(&urb, buffer, 8, 0, 100, NULL, NULL) == 0);
	saved = urb;
	assert(drv_usb_urb_setup_stream(&urb, 1, buffer, 8, 0, 100, NULL, NULL) == EOPNOTSUPP);
	assert(memcmp(&urb, &saved, sizeof(urb)) == 0);
	hcd.capabilities = DRV_USB_HCD_CAP_BULK_STREAMS;
	assert(drv_usb_urb_setup_stream(&urb, 2, buffer, 8, 0, 100, NULL, NULL) == 0);
	assert(drv_usb_urb_stream_id(&urb) == 2);
	saved = urb;
	assert(drv_usb_urb_setup_stream(&urb, 65536, buffer, 8, 0, 100, NULL, NULL) == EINVAL);
	assert(memcmp(&urb, &saved, sizeof(urb)) == 0);
	assert(drv_usb_urb_setup_stream(&urb, 1, NULL, 8, 0, 100, NULL, NULL) == EINVAL);
	assert(memcmp(&urb, &saved, sizeof(urb)) == 0);
	device.speed = DRV_USB_SPEED_HIGH;
	assert(drv_usb_urb_setup_stream(&urb, 1, buffer, 8, 0, 100, NULL, NULL) == EINVAL);
	device.speed = DRV_USB_SPEED_SUPER;
	endpoint.type = DRV_USB_TRANSFER_INTERRUPT;
	assert(drv_usb_urb_setup_stream(&urb, 1, buffer, 8, 0, 100, NULL, NULL) == EINVAL);
	endpoint.type = DRV_USB_TRANSFER_BULK;
	endpoint.companion_valid = 0;
	assert(drv_usb_urb_setup_stream(&urb, 1, buffer, 8, 0, 100, NULL, NULL) == EINVAL);
	endpoint.companion_valid = 1;
	endpoint.companion.attributes = 0;
	assert(drv_usb_urb_setup_stream(&urb, 1, buffer, 8, 0, 100, NULL, NULL) == EINVAL);
	endpoint.companion.attributes = 4;
	assert(memcmp(&urb, &saved, sizeof(urb)) == 0);
	urb.status = DRV_USB_URB_PENDING; saved = urb;
	assert(drv_usb_urb_setup_stream(&urb, 1, buffer, 8, 0, 100, NULL, NULL) == EBUSY);
	assert(memcmp(&urb, &saved, sizeof(urb)) == 0);
	urb.status = DRV_USB_URB_COMPLETE; urb.hcd_owned = 1; saved = urb;
	assert(drv_usb_urb_setup_stream(&urb, 1, buffer, 8, 0, 100, NULL, NULL) == EBUSY);
	assert(memcmp(&urb, &saved, sizeof(urb)) == 0);
	urb.hcd_owned = 0;
	assert(drv_usb_urb_setup(&urb, buffer, 8, 0, 100, NULL, NULL) == 0);
	assert(drv_usb_urb_stream_id(&urb) == 0);
	assert(drv_usb_urb_setup_stream(&urb, 2, buffer, 8, 0, 100, NULL, NULL) == 0);
	endpoint.type = DRV_USB_TRANSFER_CONTROL;
	assert(drv_usb_urb_setup_control(&urb, &control, NULL, 0, 100, NULL, NULL) == 0);
	assert(drv_usb_urb_stream_id(&urb) == 0);
	hcd.capabilities = 0;
	assert(drv_usb_urb_setup_stream(&urb, 0, NULL, 0, 0, 100, NULL, NULL) == 0);
	assert(drv_usb_urb_stream_id(NULL) == 0);
	puts("USB core stream identity: PASS");
	return 0;
}
