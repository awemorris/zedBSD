/* Actual USB wait loop: completion requires a scheduler handoff. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define sched_yield test_usb_sched_yield
#include "../../../src/drivers/usb/usb.c"
static struct drv_usb_urb *waiting;
static unsigned yields, reads, cancels;
static int complete_on_yield = 1;
static uint64_t ticks;
uint64_t sched_ticks(void) { assert(++reads < 5000); return ticks; }
void hal_free(void *pointer) { (void)pointer; abort(); }
void io_stats_record(enum io_stat_event event, uint64_t bytes)
{ (void)event; (void)bytes; }
static int cancel_busy(struct drv_usb_hcd *hcd, struct drv_usb_urb *urb)
{ (void)hcd; assert(urb == waiting); cancels++; return EBUSY; }
void sched_yield(void)
{
    assert(++yields < 120);
    ticks++;
    if (complete_on_yield)
        hal_atomic_store_release(&waiting->status, DRV_USB_URB_COMPLETE);
}
int main(void)
{
    struct drv_usb_urb urb = {0};
    struct drv_usb_hcd_ops ops = {0};
    struct drv_usb_hcd hcd = {0};
    struct drv_usb_bus bus = {0};
    struct drv_usb_device device = {0};
    waiting = &urb;
    urb.timeout_ms = 100;
    hal_atomic_store_release(&urb.status, DRV_USB_URB_PENDING);
    assert(drv_usb_urb_wait(&urb) == 0 && yields == 1);
    assert(drv_usb_urb_wait(&urb) == 0 && yields == 1);
    assert(drv_usb_urb_wait(NULL) == EINVAL);
    urb.timeout_ms = 0;
    hal_atomic_store_release(&urb.status, DRV_USB_URB_PENDING);
    assert(drv_usb_urb_wait(&urb) == 0 && yields == 2);
    complete_on_yield = 0;
    yields = reads = 0; ticks = 0;
    ops.urb_dequeue = cancel_busy; hcd.ops = &ops;
    bus.hcd = &hcd; device.bus = &bus; urb.device = &device;
    urb.timeout_ms = 10;
    hal_atomic_store_release(&urb.status, DRV_USB_URB_PENDING);
    assert(drv_usb_urb_wait(&urb) == ETIMEDOUT);
    assert(cancels && yields >= 100 && yields < 120);
    assert(hal_atomic_load_acquire(&urb.status) == DRV_USB_URB_PENDING);
    hal_atomic_store_release(&urb.status, DRV_USB_URB_STALL);
    assert(drv_usb_urb_wait(&urb) == EPIPE);
    puts("USB wait: PASS scheduled completion, terminal mapping, bounded busy cancellation");
    return 0;
}
