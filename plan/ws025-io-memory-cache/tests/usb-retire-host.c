/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdio.h>
#include "../../../src/drivers/usb/usb.c"

static int disable_error, add_owner, calls;
static struct drv_usb_device device;
int hal_printf(const char *format, ...) { (void)format; return 0; }
static int disable(struct drv_usb_hcd *h, struct drv_usb_endpoint *e)
{ (void)h; (void)e; calls++; if (add_owner) device.hcd_urb_count++; return disable_error; }
int main(void)
{
    struct drv_usb_hcd_ops ops = {0};
    struct drv_usb_hcd hcd = {0};
    struct drv_usb_bus bus = {0};
    struct drv_usb_configuration configuration = {0};
    struct drv_usb_interface interface = {0};
    struct drv_usb_host_interface alternate = {0};
    struct drv_usb_endpoint endpoint = {0};
    bus.hcd = &hcd; hcd.ops = &ops; device.bus = &bus;
    device.active_configuration = &configuration;
    configuration.device = &device; configuration.interfaces = &interface;
    interface.device = &device; interface.active_alternate = &alternate;
    alternate.interface = &interface; alternate.endpoints = &endpoint; alternate.endpoint_count = 1;
    device.quarantined = 1;
    assert(device_quiesce(&bus, &device) == EBUSY && device.quarantined);
    ops.endpoint_disable = disable;
    device.hcd_urb_count = 1;
    assert(device_quiesce(&bus, &device) == EBUSY && calls == 0 && device.quarantined);
    device.hcd_urb_count = 0; disable_error = EIO;
    assert(device_quiesce(&bus, &device) == EIO && calls == 1 && device.quarantined);
    disable_error = 0; add_owner = 1;
    assert(device_quiesce(&bus, &device) == EBUSY && calls == 2 && device.quarantined);
    add_owner = 0; device.hcd_urb_count = 0;
    assert(device_quiesce(&bus, &device) == 0 && calls == 3 && !device.quarantined);
    puts("USB retained teardown endpoint barrier: PASS");
    return 0;
}
