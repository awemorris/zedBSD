/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the console's leaving of the display on the PC/AT: the real
 * src/kern/text-display.c and src/drivers/platform/pcat/graphics/text.c
 * over a framebuffer in RAM, with /dev/graphics and its backend, the font,
 * the splash, the serial mirror and the kernel's services as stubs.
 *
 * Each scene runs in its own process (the text layer keeps its state in
 * statics): text-end-host-test SCENE.  text-end-host-test.sh runs them all.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal/hal.h"
#include "kern/device-io.h"
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/pmem.h"
#include "kern/sched.h"
#include "kern/text-display.h"
#include "kern/thread.h"
#include "kern/waitq.h"
#include "uapi/errno.h"
#include "drivers/platform/pcat/graphics/backend.h"
#include "drivers/platform/pcat/graphics/font.h"
#include "drivers/platform/pcat/graphics/splash.h"
#include "drivers/platform/pcat/graphics/text.h"
#include "drivers/platform/pcat/serial-mirror.h"

/* The framebuffer in RAM: 640x480 pixels, a pitch of 640. */
#define FB_WIDTH	640U
#define FB_HEIGHT	480U

static uint32_t framebuffer[FB_WIDTH * FB_HEIGHT];

/* What the stubs saw and give. */
static unsigned mirrored;
static unsigned graphics_ends;
static int graphics_result;
static unsigned splash_stops;
static unsigned io_writes;
static unsigned failures;
static unsigned checks;

/* The kernel's character path, which the HAL reads. */
void (*kernel_putc)(int c);

static void expect(int condition, const char *what);
static uint64_t framebuffer_sum(void);
static void other_putc(int character);
static void scene_draw(void);
static void scene_end(void);
static void scene_again(void);
static void scene_foreign(void);
static void scene_arguments(void);
static void scene_none(void);

int
main(
	int argc,
	char **argv)
{
	int scene;

	/* The scene the script asked for. */
	if (argc != 2)
		return 2;
	scene = atoi(argv[1]);
	switch (scene) {
	case 1:
		scene_draw();
		break;
	case 2:
		scene_end();
		break;
	case 3:
		scene_again();
		break;
	case 4:
		scene_foreign();
		break;
	case 5:
		scene_arguments();
		break;
	case 6:
		scene_none();
		break;
	default:
		return 2;
	}

	/* The scene's summary. */
	printf("text-end-host-test %d: %u passed, %u failed\n", scene, checks - failures, failures);
	if (failures != 0U)
		return 1;
	return 0;
}

/* Before the end: writes draw, a suspend stops them and a resume draws again. */
static void
scene_draw(void)
{
	uint64_t before;

	/* The layer over the framebuffer. */
	drv_pcat_text_init();
	expect(kern_text_ready(), "draw: the console is published");

	/* A character draws. */
	before = framebuffer_sum();
	kern_text_putc('A');
	expect(framebuffer_sum() != before, "draw: a character reaches the framebuffer");

	/* Suspended: nothing drawn; resumed: drawn again. */
	kern_text_suspend();
	before = framebuffer_sum();
	kern_text_putc('B');
	expect(framebuffer_sum() == before, "draw: nothing drawn while suspended");
	kern_text_resume();
	expect(framebuffer_sum() != before, "draw: the resume draws again");
}

/* The end: nothing reaches the framebuffer again, while the cells, the serial line and the kernel's path go on. */
static void
scene_end(void)
{
	unsigned row;
	unsigned column;
	unsigned column_after;
	uint64_t before;
	int visible;
	int error;

	/* The layer over the framebuffer, with nothing published yet as the kernel's path. */
	drv_pcat_text_init();
	kern_text_putc('A');
	kernel_putc = NULL;

	/* The end without ranges (a GPU's first flip). */
	error = kern_text_end(NULL, 0U);
	expect(error == 0 && graphics_ends == 1U && splash_stops == 1U, "end: 0, /dev/graphics ended, the splash stopped");
	expect(kernel_putc == kern_text_kernel_putc, "end: the kernel's path is the console's");

	/* Nothing draws; the cursor (the cells) and the serial line follow the characters. */
	before = framebuffer_sum();
	kern_text_get_cursor(&row, &column, &visible);
	mirrored = 0U;
	kern_text_putc('B');
	kern_text_putc('C');
	kern_text_get_cursor(&row, &column_after, &visible);
	expect(framebuffer_sum() == before, "end: a character draws nothing");
	expect(column_after == column + 2U && mirrored == 2U, "end: the cells and the serial line follow");

	/* Every other way of drawing draws nothing. */
	kern_text_write(1U, 0U, KERN_TEXT_ATTRIB_NORMAL, "XYZ");
	kern_text_clear();
	(void)kern_text_set_cursor(3U, 3U);
	kern_text_update_cursor();
	kern_text_resume();
	kern_text_reveal();
	kern_text_reveal_fatal();
	expect(framebuffer_sum() == before, "end: write, clear, cursor, resume, reveal and reveal_fatal draw nothing");
	kern_text_get_cursor(&row, &column, &visible);
	expect(row == 3U && column == 3U, "end: the cursor still follows");
	expect(io_writes == 0U, "end: no VGA register written on the framebuffer surface");
}

/* A later end: /dev/graphics is asked again (it waits for the first and judges), and its result comes back. */
static void
scene_again(void)
{
	struct kern_text_range range;
	int error;

	/* The first end. */
	drv_pcat_text_init();
	error = kern_text_end(NULL, 0U);
	expect(error == 0 && graphics_ends == 1U, "again: the first end");

	/* The second, with a range /dev/graphics says something stays mapped in. */
	range.physical = 0xe0000000ULL;
	range.bytes = 0x10000000ULL;
	graphics_result = EBUSY;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && graphics_ends == 2U && splash_stops == 1U, "again: judged by /dev/graphics, the text not ended twice");
}

/* Another character path published: EBUSY only for the caller's ranges. */
static void
scene_foreign(void)
{
	struct kern_text_range range;
	int error;

	/* Something else is the kernel's path. */
	drv_pcat_text_init();
	kernel_putc = other_putc;

	/* With ranges: it may draw over them. */
	range.physical = 0xe0000000ULL;
	range.bytes = 0x1000ULL;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && kernel_putc == other_putc, "foreign: EBUSY with ranges, the path left alone");

	/* Without ranges: nothing to judge. */
	error = kern_text_end(NULL, 0U);
	expect(error == 0, "foreign: 0 without ranges");
}

/* Malformed lists change nothing. */
static void
scene_arguments(void)
{
	struct kern_text_range ranges[KERN_TEXT_END_RANGES + 1U];
	unsigned index;
	uint64_t before;
	int error;

	/* The layer, drawing. */
	drv_pcat_text_init();
	for (index = 0U; index <= KERN_TEXT_END_RANGES; index++) {
		ranges[index].physical = 0x1000ULL * (index + 1U);
		ranges[index].bytes = 0x1000ULL;
	}

	/* Too many, NULL with a count, an empty range, a wrapping range. */
	error = kern_text_end(ranges, KERN_TEXT_END_RANGES + 1U);
	expect(error == EINVAL, "arguments: too many ranges");
	error = kern_text_end(NULL, 1U);
	expect(error == EINVAL, "arguments: NULL with a count");
	ranges[0].bytes = 0U;
	error = kern_text_end(ranges, 1U);
	expect(error == EINVAL, "arguments: an empty range");
	ranges[0].physical = UINT64_MAX - 0xfffULL;
	ranges[0].bytes = 0x2000ULL;
	error = kern_text_end(ranges, 1U);
	expect(error == EINVAL, "arguments: a wrapping range");

	/* Nothing changed: the console still draws. */
	before = framebuffer_sum();
	kern_text_putc('Q');
	expect(graphics_ends == 0U && framebuffer_sum() != before, "arguments: the console still draws");
}

/* No console published: ENOTSUP. */
static void
scene_none(void)
{
	int error;

	/* No board registered a table. */
	error = kern_text_end(NULL, 0U);
	expect(error == ENOTSUP && graphics_ends == 0U, "none: ENOTSUP without a console");
}

/* Counts one check and reports a failure. */
static void
expect(
	int condition,
	const char *what)
{
	/* One more check. */
	checks++;
	if (condition)
		return;

	/* A failure, named. */
	failures++;
	printf("FAIL %s\n", what);
}

/* Sums the framebuffer's pixels with their places, so that any change shows. */
static uint64_t
framebuffer_sum(void)
{
	uint64_t sum;
	size_t index;

	/* Every pixel, weighted by its index. */
	sum = 0U;
	for (index = 0U; index < FB_WIDTH * FB_HEIGHT; index++)
		sum += (uint64_t)framebuffer[index] * (index + 1U);
	return sum;
}

/* A character path that is not the console's. */
static void
other_putc(
	int character)
{
	(void)character;
}

/* The stubs. */

int
drv_pcat_graphics_backend_get_framebuffer(
	volatile uint32_t **pixels,
	unsigned *width,
	unsigned *height,
	unsigned *stride,
	int *rgbx)
{
	/* The framebuffer in RAM. */
	*pixels = framebuffer;
	*width = FB_WIDTH;
	*height = FB_HEIGHT;
	*stride = FB_WIDTH;
	*rgbx = 0;
	return 1;
}

int
drv_pcat_graphics_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	(void)ranges;
	(void)count;

	/* Counted, with the test's result. */
	graphics_ends++;
	return graphics_result;
}

int
drv_pcat_font_get_glyph(
	uint32_t character,
	uint8_t glyph[32],
	unsigned *width,
	unsigned *height)
{
	unsigned line;

	/* Every glyph is a block with its code in the first line. */
	for (line = 0U; line < 16U; line++)
		glyph[line] = 0xffU;
	glyph[0] = (uint8_t)character;
	*width = 8U;
	*height = 16U;
	return 1;
}

void
drv_pcat_serial_mirror(
	int character)
{
	(void)character;
	mirrored++;
}

void
drv_pcat_splash_retarget(
	volatile uint32_t *pixels)
{
	(void)pixels;
}

void
drv_pcat_splash_step(void)
{
}

void
drv_pcat_splash_stop(void)
{
	splash_stops++;
}

hal_cpu_id_t
hal_cpu_current(void)
{
	return 0;
}

unsigned
kern_cpu_current(void)
{
	return 0U;
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	(void)address;
	(void)size;
	(void)attributes;
	(void)mapped;

	/* The framebuffer surface needs no mapping here. */
	return ENOMEM;
}

void
kern_io_out8(
	uint16_t port,
	uint8_t value)
{
	(void)port;
	(void)value;
	io_writes++;
}

int
kern_log_quiet(void)
{
	return 0;
}

void
kern_log_set_quiet(
	int quiet)
{
	(void)quiet;
}

void
kern_log_write(
	const char *bytes,
	size_t length)
{
	(void)bytes;
	(void)length;
}

int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	(void)entry;
	(void)arg;
	(void)priority;
	(void)result;
	return ENOMEM;
}

void
thread_start(
	struct thread *thread)
{
	(void)thread;
}

void
sched_sleep(
	uint64_t timeout_tick)
{
	(void)timeout_tick;
}

uint64_t
sched_ticks(void)
{
	return 0U;
}

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

void
spin_lock(
	struct spinlock *lock)
{
	lock->held.value = 1U;
}

int
spin_trylock(
	struct spinlock *lock)
{
	/* Taken when free. */
	if (lock->held.value != 0U)
		return 0;
	lock->held.value = 1U;
	return 1;
}

void
spin_unlock(
	struct spinlock *lock)
{
	lock->held.value = 0U;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* A second take on the one thread would never come back. */
	if (lock->held.value != 0U) {
		failures++;
		printf("FAIL %s taken twice\n", lock->name);
	}
	lock->held.value = 1U;
	return 0UL;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)enabled;
	lock->held.value = 0U;
}

void
waitq_wake_all(
	struct wait_queue *queue)
{
	(void)queue;
}

/* A tick of the test's clock is one millisecond. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	return milliseconds;
}
