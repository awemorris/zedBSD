/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/usb.h>
#include <drivers/usb-uas.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct drv_usb_device { int unused; };
struct drv_usb_endpoint { struct drv_usb_endpoint_descriptor descriptor; unsigned pipe; };
struct drv_usb_urb { unsigned pipe; void *buffer; size_t length; size_t actual; };
static unsigned wait_failure;
static unsigned step, fail_step, short_command, short_data, drain_failure;
static unsigned freed, allocated, reserve_failure, reserve_count, fast_time;
static unsigned write_command, no_data, check_condition;
static uint16_t tag;
static uint64_t ticks;
static unsigned timeouts[32];
static unsigned recovering, recovery_case, recovery_step, drains;
static uint16_t failed_tag, management_tag;

uint64_t sched_ticks(void) { return ticks; }
void sched_yield(void) { ticks++; }
int drv_usb_endpoint_configure_streams(struct drv_usb_endpoint *e, unsigned n)
{ (void)e; (void)n; assert(0); return EOPNOTSUPP; }
int drv_usb_urb_setup_stream(struct drv_usb_urb *u, unsigned stream, void *b, size_t n, unsigned f, unsigned t, drv_usb_urb_callback_t cb, void *a)
{ (void)u; (void)stream; (void)b; (void)n; (void)f; (void)t; (void)cb; (void)a; assert(0); return EOPNOTSUPP; }
enum drv_usb_urb_status drv_usb_urb_status(const struct drv_usb_urb *u)
{ (void)u; assert(0); return DRV_USB_URB_IO_ERROR; }
enum drv_usb_speed drv_usb_device_speed(const struct drv_usb_device *d)
{ (void)d; return DRV_USB_SPEED_HIGH; }
unsigned drv_usb_device_hcd_capabilities(const struct drv_usb_device *d)
{ (void)d; return DRV_USB_HCD_CAP_TRANSFER_RESERVE; }
const struct drv_usb_endpoint_descriptor *drv_usb_endpoint_descriptor(const struct drv_usb_endpoint *e)
{ return &e->descriptor; }
struct drv_usb_urb *drv_usb_urb_alloc(struct drv_usb_device *d, struct drv_usb_endpoint *e, unsigned count)
{
	struct drv_usb_urb *u = calloc(1, sizeof(*u));
	(void)d; assert(count == 0); assert(u); u->pipe = e->pipe; allocated++; return u;
}
void drv_usb_urb_free(struct drv_usb_urb *u) { if (u) { freed++; free(u); } }
int drv_usb_urb_reserve_sync(struct drv_usb_urb *u, size_t n)
{ (void)u; assert(n); return ++reserve_count == reserve_failure ? ENOMEM : 0; }
int drv_usb_urb_reserve_transfer(struct drv_usb_urb *u, size_t n)
{ return drv_usb_urb_reserve_sync(u, n); }
int drv_usb_urb_setup(struct drv_usb_urb *u, void *b, size_t n, unsigned flags,
    unsigned timeout, drv_usb_urb_callback_t cb, void *arg)
{
	assert(cb == NULL && arg == NULL); assert(timeout > 0); (void)flags;
	u->buffer = b; u->length = n; timeouts[step] = timeout; return 0;
}
int drv_usb_urb_submit(struct drv_usb_urb *u)
{
	unsigned char *bytes = u->buffer;
	unsigned expected[] = { 0, 1, 2, 1 };
	if (recovering) {
		assert(drains == 4);
		assert(u->pipe == (recovery_step == 0 ? 0 : 1));
		if (recovery_step == 0) {
			assert(u->length == 16 && bytes[0] == 5 && bytes[4] == 1);
			assert((((unsigned)bytes[6] << 8) | bytes[7]) == failed_tag);
			assert(bytes[9] == 7);
			management_tag = ((unsigned)bytes[2] << 8) | bytes[3];
			assert(management_tag != 0 && management_tag != failed_tag);
		}
		recovery_step++;
		return recovery_case == 7 ? EIO : 0;
	}
	if (write_command) expected[2] = 3;
	assert(step < 4); assert(u->pipe == expected[step]);
	step++;
	if (step == 1) tag = ((unsigned)bytes[2] << 8) | bytes[3];
	return step == fail_step ? EIO : 0;
}
int drv_usb_urb_wait_reusable(struct drv_usb_urb *u)
{
	unsigned char *bytes = u->buffer;
	if (recovering) {
		if (recovery_case == 8) return ETIMEDOUT;
		u->actual = u->length;
		if (recovery_step == 1) {
			if (recovery_case == 9) u->actual--;
			return 0;
		}
		memset(bytes, 0, u->length);
		bytes[0] = 4;
		bytes[2] = management_tag >> 8;
		bytes[3] = management_tag;
		u->actual = 8;
		if (recovery_case == 1 && recovery_step <= 3) {
			bytes[0] = recovery_step == 2 ? 6 : 3;
			bytes[2] = failed_tag >> 8; bytes[3] = failed_tag;
			u->actual = recovery_step == 2 ? 4 : 16;
		}
		if (recovery_case == 2) bytes[7] = 5;
		if (recovery_case == 3) bytes[3] ^= 0x40;
		if (recovery_case == 4) u->actual = 7;
		if (recovery_case == 5 || recovery_case == 10) {
			bytes[0] = 6;
			bytes[2] = failed_tag >> 8; bytes[3] = failed_tag;
			u->actual = recovery_case == 10 ? 5 : 4;
		}
		if (recovery_case == 11) bytes[4] = 1;
		return 0;
	}
	if (step == wait_failure) return ETIMEDOUT;
	u->actual = u->length;
	if (step == 1 && short_command) u->actual--;
	if (step == 2 || step == 4) {
		memset(bytes, 0, u->length);
		bytes[2] = tag >> 8; bytes[3] = tag;
		if (step == 2 && !no_data && !check_condition) {
			bytes[0] = write_command ? 7 : 6; u->actual = 4;
		} else {
			bytes[0] = 3; bytes[6] = check_condition ? 2 : 0;
			u->actual = 16;
		}
	}
	if (step == 3) {
		if (short_data) u->actual--;
		if (!write_command) memset(bytes, 0xa5, u->actual);
	}
	ticks += fast_time ? 100 : 1;
	return 0;
}
size_t drv_usb_urb_actual_length(const struct drv_usb_urb *u) { return u->actual; }
int drv_usb_urb_cancel(struct drv_usb_urb *u) { (void)u; return 0; }
int drv_usb_urb_drain(struct drv_usb_urb *u, unsigned timeout)
{ (void)u; assert(timeout); drains++; return drain_failure ? ETIMEDOUT : 0; }

static void reset(void)
{
	recovering = recovery_case = recovery_step = drains = 0;
	wait_failure = 0;
	step = fail_step = short_command = short_data = drain_failure = 0;
	freed = allocated = reserve_failure = reserve_count = fast_time = 0;
	write_command = no_data = check_condition = 0; ticks = 0;
}

int main(void)
{
	struct drv_usb_device device;
	struct drv_usb_endpoint endpoints[4];
	struct drv_usb_endpoint *pipes[4];
	struct drv_usb_uas_transport transport;
	struct drv_usb_uas_result result;
	unsigned char cdb[10] = { 0x28 };
	unsigned char data[512];
	unsigned i, scenario, old_step;
	int error;

	memset(endpoints, 0, sizeof(endpoints));
	for (i = 0; i < 4; i++) {
		endpoints[i].pipe = i;
		endpoints[i].descriptor.address = (i + 1) | ((i == 1 || i == 2) ? 0x80 : 0);
		endpoints[i].descriptor.attributes = 2;
		endpoints[i].descriptor.maximum_packet_size = 512;
		pipes[i] = &endpoints[i];
	}
	for (scenario = 0; scenario < 16; scenario++) {
		reset(); memset(&transport, 0, sizeof(transport));
		assert(drv_usb_uas_transport_init(&transport, &device, pipes, 65536) == 0);
		assert(drv_usb_uas_transport_init(&transport, &device, pipes, 65536) == EBUSY);
		if (scenario >= 1 && scenario <= 4) fail_step = scenario;
		if (scenario >= 12) wait_failure = scenario - 11;
		short_command = scenario == 5;
		short_data = scenario == 6;
		fast_time = scenario == 7;
		write_command = scenario == 8;
		no_data = scenario == 9;
		check_condition = scenario == 10;
		if (scenario == 11) transport.next_tag = 65535;
		error = drv_usb_uas_transport_execute(&transport, 0, cdb, sizeof(cdb),
		    no_data ? DRV_USB_UAS_NO_DATA : (write_command ? DRV_USB_UAS_WRITE : DRV_USB_UAS_READ),
		    data, no_data ? 0 : sizeof(data), 100, &result);
		if ((scenario >= 1 && scenario <= 7 && scenario != 6) || scenario >= 12) {
			assert(error != 0); assert(transport.stopped);
			old_step = step;
			assert(drv_usb_uas_transport_execute(&transport, 0, cdb, 10,
			    DRV_USB_UAS_READ, data, 512, 100, &result) == EIO);
			assert(step == old_step);
		} else {
			assert(error == 0); assert(!transport.stopped);
			assert(result.status == (check_condition ? 2 : 0));
			assert(result.transferred == (no_data || check_condition ? 0 : (short_data ? 511 : 512)));
			if (!no_data && !check_condition) {
				assert(step == 4); assert(timeouts[3] < timeouts[0]);
				if (!write_command) assert(data[result.transferred - 1] == 0xa5);
			}
			if (scenario == 11) assert(transport.next_tag == 1);
		}
		drain_failure = 1;
		assert(drv_usb_uas_transport_stop(&transport) == ETIMEDOUT);
		assert(freed == 0);
		for (i = 0; i < 4; i++) assert(transport.urbs[i]);
		drain_failure = 0;
		assert(drv_usb_uas_transport_stop(&transport) == 0);
		assert(freed == allocated);
	}
	for (i = 1; i <= 4; i++) {
		reset(); memset(&transport, 0, sizeof(transport)); reserve_failure = i;
		assert(drv_usb_uas_transport_init(&transport, &device, pipes, 65536) == ENOMEM);
		assert(allocated == freed);
	}
	for (scenario = 0; scenario < 12; scenario++) {
		reset(); memset(&transport, 0, sizeof(transport));
		assert(drv_usb_uas_transport_init(&transport, &device, pipes, 65536) == 0);
		transport.next_tag = 65535;
		wait_failure = 2;
		assert(drv_usb_uas_transport_execute(&transport, 7, cdb, 10,
		    DRV_USB_UAS_READ, data, 512, 100, &result) == ETIMEDOUT);
		failed_tag = transport.failed_tag;
		assert(failed_tag == 65535 && transport.failed_lun == 7);
		recovering = 1; recovery_case = scenario; drain_failure = scenario == 6;
		error = drv_usb_uas_transport_recover(&transport, 100);
		if (scenario <= 1) {
			assert(error == 0 && !transport.stopped && transport.failed_tag == 0);
			assert(transport.next_tag == 2);
			assert(recovery_step == (scenario == 1 ? 4 : 2));
			/* Only the caller's new command is issued, never the failed CDB. */
			recovering = 0; wait_failure = 0; step = 0;
			assert(drv_usb_uas_transport_execute(&transport, 7, cdb, 10,
			    DRV_USB_UAS_READ, data, 512, 100, &result) == 0);
			assert(tag == 2);
		} else {
			assert(error != 0 && transport.stopped && transport.failed_tag == failed_tag);
			old_step = recovery_step;
			assert(drv_usb_uas_transport_recover(&transport, 100) == EIO);
			assert(recovery_step == old_step && freed == 0);
			if (scenario == 6) assert(recovery_step == 0);
			if (scenario == 5) assert(recovery_step == 10);
		}
		drain_failure = 0;
		assert(drv_usb_uas_transport_stop(&transport) == 0);
		assert(freed == allocated);
		assert(drv_usb_uas_transport_recover(&transport, 100) == EIO);
	}
	puts("UAS synchronous transport and task abort: PASS");
	return 0;
}
