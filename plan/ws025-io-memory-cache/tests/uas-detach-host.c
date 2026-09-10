/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
typedef int tid_t;
#define sigset_t zedbsd_sigset_t
#include "../../../src/drivers/usb/usb-uas-disk.c"

static struct uas_disk owner;
static struct thread worker;
static unsigned ticks;
static int join_error;
static int tearing, revoked, busy, destroyed, stopped, freed, depth;
void kernel_notify_task(hal_task_t task) { (void)task; assert(depth == 0); }
uint64_t sched_ticks(void) { ticks += 1000; return ticks; }
void sched_yield(void) { assert(depth == 0); }
int thread_wait(struct thread *t, int *status)
{ (void)status; assert(t == &worker && depth == 0); return join_error; }
void mutex_lock(struct mutex *m) { (void)m; depth++; }
void mutex_unlock(struct mutex *m) { (void)m; assert(depth); depth--; }
void *drv_usb_interface_driver_data(const struct drv_usb_interface *i)
{ (void)i; return &owner; }
int drv_usb_interface_set_driver_data(struct drv_usb_interface *i, void *d)
{ (void)i; assert(d == NULL && stopped && !freed); return 0; }
int drv_usb_device_is_tearing_down(const struct drv_usb_device *d)
{ (void)d; return tearing; }
void disk_media_revoke(struct disk *d) { assert(d == owner.disk); revoked = 1; }
int disk_media_status(const struct disk *d) { (void)d; return revoked ? ENODEV : 0; }
int partition_retire_media(struct disk *d)
{ (void)d; assert(revoked); return busy ? EBUSY : 0; }
int disk_gone_if_idle(struct disk *d)
{ (void)d; assert(!tearing && !revoked); return busy ? EBUSY : 0; }
int disk_destroy(struct disk *d) { (void)d; destroyed++; return 0; }
int drv_usb_uas_transport_stop(struct drv_usb_uas_transport *t)
{ (void)t; assert(destroyed); stopped++; return 0; }
void hal_free(void *p) { assert(p == &owner && stopped && depth == 0); freed++; }
int main(void)
{
    struct disk disk;
    for (tearing = 0; tearing <= 1; tearing++) {
        memset(&owner, 0, sizeof(owner)); owner.disk = &disk;
        revoked = destroyed = stopped = freed = depth = 0;
        busy = 1;
        assert(uas_detach(NULL, 0) == EBUSY);
        assert(revoked == tearing && !destroyed && !stopped && !freed);
        busy = 0;
        assert(uas_detach(NULL, 0) == 0);
        assert(destroyed == 1 && stopped == 1 && freed == 1);
    }
    memset(&owner, 0, sizeof(owner)); owner.disk = &disk;
    owner.control_worker = &worker;
    worker.state = THREAD_RUNNING;
    revoked = destroyed = stopped = freed = depth = 0;
    ticks = 0;
    assert(uas_detach(NULL, 0) == EBUSY);
    assert(owner.control_stopping && owner.control_worker == &worker);
    assert(!destroyed && !stopped && !freed);
    worker.state = THREAD_ZOMBIE;
    join_error = EIO;
    assert(uas_detach(NULL, 0) == EIO);
    assert(owner.control_worker == &worker && !destroyed && !stopped && !freed);
    join_error = 0;
    assert(uas_detach(NULL, 0) == 0);
    assert(owner.control_worker == NULL && destroyed && stopped && freed);
    puts("UAS detached media retirement and worker join: PASS");
    return 0;
}
