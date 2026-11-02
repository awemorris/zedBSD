/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the USB HID worker's re-arm (src/drivers/usb/usb-hid.c,
 * included whole so its static worker runs): a finished report is copied
 * out of its transfer, the transfer goes back on the endpoint before the
 * report is handed to the input glue, the glue gets the report as it came
 * even when the next transfer has already written the buffer, its time is
 * the finished transfer's, a failed re-arm still hands the report on and
 * then ends the service, and a stall is cleared and re-armed as before.
 *
 * The USB core, the input glue and the scheduler are stand-ins below; only
 * the calls the worker makes are given (the link ignores the others).
 */

#include "src/drivers/usb/usb-hid.c"

#include <stdio.h>
#include <string.h>

#define STEPS_MAX	32U

/* One scripted completion: its status, report and time. */
struct step {
	enum drv_usb_urb_status status;
	uint8_t bytes[9];
	size_t length;
	uint64_t milliseconds;
	int submit_error;
};

static struct usb_hid test_hid;
static uint8_t test_buffer[9];
static uint8_t test_report[9];
static struct step steps[STEPS_MAX];
static unsigned step_count;
static unsigned step_next;
static enum drv_usb_urb_status urb_status;
static size_t urb_length;
static int next_submit_error;

/* What happened, in order: 's' submit, 'p' publish, 'h' clear halt. */
static char events[128];
static unsigned event_count;
static uint8_t published[STEPS_MAX][9];
static uint64_t published_ms[STEPS_MAX];
static unsigned published_count;
static unsigned unpublished;
static unsigned failures;
static unsigned checks;

static void check(int good, const char *what);
static void deliver(void);
static void run(void);

/* The stand-ins the worker calls. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

void
kern_thread_block(
	void)
{
	/* The next scripted completion, or the end of the run. */
	if (step_next < step_count) {
		deliver();
		return;
	}
	test_hid.stopping = 1U;
}

int
drv_usb_urb_drain(
	struct drv_usb_urb *u,
	unsigned timeout_ms)
{
	(void)u;
	(void)timeout_ms;
	return 0;
}

enum drv_usb_urb_status
drv_usb_urb_status(
	const struct drv_usb_urb *u)
{
	(void)u;
	return urb_status;
}

size_t
drv_usb_urb_actual_length(
	const struct drv_usb_urb *u)
{
	(void)u;
	return urb_length;
}

void
drv_usb_urb_hcd_result(
	const struct drv_usb_urb *u,
	struct drv_usb_urb_hcd_result *result)
{
	(void)u;
	memset(result, 0, sizeof(*result));
}

int
drv_usb_urb_setup(
	struct drv_usb_urb *u,
	void *b,
	size_t n,
	unsigned f,
	unsigned t,
	drv_usb_urb_callback_t cb,
	void *a)
{
	(void)u;
	(void)n;
	(void)f;
	(void)t;
	(void)cb;
	(void)a;
	if (b != test_buffer)
		check(0, "the transfer reads into the interface's buffer");
	return 0;
}

int
drv_usb_urb_submit(
	struct drv_usb_urb *u)
{
	(void)u;
	events[event_count++] = 's';

	/* A device that answers at once: the next report is in the buffer before the last is handed on. */
	if (step_next < step_count)
		memcpy(test_buffer, steps[step_next].bytes, sizeof(test_buffer));
	return next_submit_error;
}

int
drv_usb_endpoint_clear_halt(
	struct drv_usb_endpoint *endpoint)
{
	(void)endpoint;
	events[event_count++] = 'h';
	return 0;
}

void
drv_hid_input_report(
	struct hid_input *input,
	const uint8_t *report,
	size_t length,
	uint64_t milliseconds)
{
	(void)input;
	events[event_count++] = 'p';
	memset(published[published_count], 0, sizeof(published[0]));
	memcpy(published[published_count], report, length);
	published_ms[published_count] = milliseconds;
	published_count++;
}

void
drv_hid_input_unpublish(
	struct hid_input *input)
{
	(void)input;
	unpublished++;
}

void *
kern_boot_handoff(
	const char *name)
{
	(void)name;
	return NULL;
}

int
kern_boot_parameters_token_present(
	const char *text,
	const char *token)
{
	(void)text;
	(void)token;
	return 0;
}

void
kern_logf(
	const char *format,
	...)
{
	(void)format;
}

unsigned
drv_usb_interface_number(
	const struct drv_usb_interface *i)
{
	(void)i;
	return 1;
}

int
main(
	void)
{
	static const uint8_t press[9] = { 0x02, 0x01 };
	static const uint8_t release[9] = { 0x02, 0x00 };

	/* A press and its release 1 ms apart, the device answering each re-arm at once. */
	steps[0] = (struct step){ DRV_USB_URB_COMPLETE, { 0x02, 0x01 }, 9, 1000, 0 };
	steps[1] = (struct step){ DRV_USB_URB_COMPLETE, { 0x02, 0x00 }, 9, 1001, 0 };
	step_count = 2;
	run();
	check(strcmp(events, "spsp") == 0, "each transfer is armed again before its report is handed on (spsp)");
	check(published_count == 2 && memcmp(published[0], press, 9) == 0, "the press is handed on as it came");
	check(memcmp(published[1], release, 9) == 0, "the release is handed on as it came");
	check(published_ms[0] == 1000 && published_ms[1] == 1001, "each report has its own transfer's time");
	check(test_hid.stopping == 1U && unpublished == 0, "the worker ends only at the end of the script");

	/* A short report keeps its length. */
	steps[0] = (struct step){ DRV_USB_URB_COMPLETE, { 0x01, 0x02, 0x03 }, 3, 5, 0 };
	step_count = 1;
	run();
	check(published_count == 1 && published[0][0] == 0x01 && published[0][2] == 0x03 && published[0][3] == 0, "a 3-byte report is handed on with 3 bytes");

	/* A re-arm that fails: the report still goes on, then the service ends. */
	steps[0] = (struct step){ DRV_USB_URB_COMPLETE, { 0x02, 0x01 }, 9, 7, EIO };
	steps[1] = (struct step){ DRV_USB_URB_COMPLETE, { 0x02, 0x00 }, 9, 8, 0 };
	step_count = 2;
	run();
	check(strcmp(events, "sp") == 0 && published_count == 1, "a failed re-arm hands the report on and stops");
	check(unpublished == 1, "a failed re-arm unpublishes the input");

	/* A stall: cleared, re-armed, and the next report handed on after its own re-arm. */
	steps[0] = (struct step){ DRV_USB_URB_STALL, { 0 }, 0, 9, 0 };
	steps[1] = (struct step){ DRV_USB_URB_COMPLETE, { 0x02, 0x01 }, 9, 10, 0 };
	step_count = 2;
	run();
	check(strcmp(events, "hssp") == 0 && published_count == 1, "a stall is cleared and re-armed (hssp)");

	/* The summary. */
	if (failures != 0) {
		printf("host-hid-rearm: FAIL %u/%u\n", failures, checks);
		return 1;
	}
	printf("host-hid-rearm: PASS %u/%u\n", checks, checks);
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
		printf("FAIL: %s (events %s)\n", what, events);
		return;
	}
	printf("ok: %s\n", what);
}

/* Finishes the transfer with the next scripted completion, as the completion callback would. */
static void
deliver(
	void)
{
	struct step *step;

	step = &steps[step_next++];
	urb_status = step->status;
	urb_length = step->length;
	memcpy(test_buffer, step->bytes, sizeof(test_buffer));
	next_submit_error = step->submit_error;
	test_hid.completed_milliseconds = step->milliseconds;
	test_hid.work_pending |= USB_HID_WORK_COMPLETE;
}

/* Runs the worker over the script from a fresh interface. */
static void
run(
	void)
{
	memset(&test_hid, 0, sizeof(test_hid));
	memset(events, 0, sizeof(events));
	event_count = 0;
	published_count = 0;
	unpublished = 0;
	step_next = 0;
	next_submit_error = 0;
	test_hid.urb = (struct drv_usb_urb *)(void *)&test_hid;
	test_hid.buffer = test_buffer;
	test_hid.report = test_report;
	test_hid.buffer_size = sizeof(test_buffer);
	test_hid.hidinput = (struct hid_input *)(void *)&test_hid;
	usb_hid_worker(&test_hid);
}
