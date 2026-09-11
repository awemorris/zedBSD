/* q086 production USB core, fake HCD only. SPDX-License-Identifier: Zlib */
#define main recovery_regression_main
#include "usb-recovery-contract-test.c"
#undef main

int main(void)
{
	struct fake_controller controller;
	struct drv_usb_device *device;
	struct drv_usb_urb *urb, *other;
	unsigned char *client;
	unsigned char byte = 7;
	size_t baseline;
	uint64_t start;

	CHECK(drv_usb_init() == 0);
	CHECK(drv_usb_driver_register(&recovery_primary_driver) == 0);
	CHECK(drv_usb_driver_register(&recovery_secondary_driver) == 0);
	set_binding_mode(RECOVERY_BIND_CLAIM_SIBLING);
	baseline = atomic_load(&live_allocations);
	device = register_controller(&controller);
	urb = drv_usb_urb_alloc(device, find_endpoint(device, 0U, 0x81U), 0);
	other = drv_usb_urb_alloc(device, find_endpoint(device, 1U, 0x84U), 0);
	CHECK(urb != NULL && other != NULL);
	CHECK(drv_usb_urb_reserve_sync(urb, 512) == 0);
	client = malloc(512);
	CHECK(client != NULL);
	memset(client, 0x35, 512);
	CHECK(drv_usb_urb_setup(urb, client, 512, 0, 10, NULL, NULL) == 0);
	controller.data_behavior = DATA_HOLD;
	CHECK(drv_usb_urb_submit(urb) == 0);
	cancel_failures = 1;
	CHECK(drv_usb_urb_wait_reusable(urb) != 0);
	CHECK(drv_usb_urb_drain(urb, 10) == 0);
	CHECK(drv_usb_urb_setup(urb, client, 512, 0, 10, NULL, NULL) == 0);
	printf("S05 PASS cancel failure then checked retry retires ownership\n");

	/* Seed device-owned input staging: IN setup does not copy caller bytes. */
	memset(drv_usb_urb_buffer(urb), 0x35, 512);
	CHECK(drv_usb_urb_submit(urb) == 0);
	cancel_failures = 100;
	start = sched_ticks();
	CHECK(drv_usb_urb_wait_reusable(urb) != 0);
	CHECK(sched_ticks() - start < 200);
	CHECK(drv_usb_urb_status(urb) == DRV_USB_URB_PENDING);
	CHECK(drv_usb_urb_setup(urb, &byte, 1, 0, 10, NULL, NULL) == EBUSY);
	CHECK(((unsigned char *)drv_usb_urb_buffer(urb))[0] == 0x35);
	printf("S07 PASS retained request rejects reuse without overwriting staging\n");
	free(client);
	controller.data_behavior = DATA_COMPLETE;
	CHECK(drv_usb_urb_setup(other, &byte, 1, 0, 10, NULL, NULL) == 0);
	CHECK(drv_usb_urb_submit(other) == 0);
	CHECK(drv_usb_urb_wait_reusable(other) == 0);
	printf("S08 PASS unrelated endpoint progresses during retained ownership\n");
	memset(drv_usb_urb_buffer(urb), 0x92, 512);
	controller.held_data = NULL;
	fake_complete(&controller, urb, DRV_USB_URB_COMPLETE, 512);
	CHECK(drv_usb_urb_drain(urb, 10) == 0);
	CHECK(urb->sync_client == NULL);
	CHECK(drv_usb_urb_setup(urb, &byte, 1, 0, 10, NULL, NULL) == 0);
	drv_usb_urb_free(urb);
	drv_usb_urb_free(other);
	cancel_failures = 0;
	unregister_controller(&controller);
	CHECK(atomic_load(&live_allocations) == baseline);
	CHECK(drv_usb_driver_unregister(&recovery_secondary_driver) == 0);
	CHECK(drv_usb_driver_unregister(&recovery_primary_driver) == 0);
	printf("S06 PASS late completion after caller free uses retained staging only\n");
	return 0;
}
