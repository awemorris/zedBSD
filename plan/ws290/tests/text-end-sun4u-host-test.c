/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the sun4u console's leaving of the screen: the console
 * (src/drivers/platform/sun4u/sun4u-console.c) and the firmware
 * framebuffer (sun4u-firmware-fb.c), included here so that their state
 * can be read, over the real src/kern/text-display.c and, unless built
 * with CONFIG_DRIVER_GRAPHICS_DEVICE 0, the generic /dev/graphics front
 * end (src/drivers/generic/graphics-device.c); the kernel's services are
 * stubs (text-end-sun4u-stubs.c), and the framebuffer's mapping is an
 * array of this file whose sum shows whether anything drew.
 *
 * Each scene runs in its own process (the layers keep their state in
 * statics): text-end-sun4u-host-test SCENE.  It prints one line a check
 * that fails, then one summary line.  text-end-sun4u-host-test.sh runs
 * them all.
 */

#ifndef CONFIG_DRIVER_GRAPHICS_DEVICE
#define CONFIG_DRIVER_GRAPHICS_DEVICE 1
#endif

#include "src/drivers/platform/sun4u/sun4u-console.c"
#include "src/drivers/platform/sun4u/sun4u-firmware-fb.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/graphics-device.h"

/* The screen the hardware layer names: 640 by 400 four-byte pixels. */
#define TEST_WIDTH	640U
#define TEST_HEIGHT	400U
#define TEST_PITCH	(TEST_WIDTH * 4U)
#define TEST_BYTES	((uint64_t)TEST_PITCH * TEST_HEIGHT)
#define TEST_PHYSICAL	0x000001ff00000000ULL

/* The 16550's port the hardware layer names. */
#define TEST_UART_PORT	0x3f8U

/* What the stubs saw and give (text-end-sun4u-stubs.c). */
extern const struct cdev_ops *stub_graphics_ops;
extern unsigned stub_cdev_registers;
extern int stub_irq_off;
extern const char *stub_held[8];
extern unsigned stub_held_count;
extern uint16_t stub_uart_port;
extern char stub_uart_sent[512];
extern unsigned stub_uart_sent_count;
extern unsigned stub_hal_drawn;

/* What the hardware layer names, and whether it names the screen. */
static uint64_t name_phys = TEST_PHYSICAL;
static uint64_t name_width = TEST_WIDTH;
static uint64_t name_height = TEST_HEIGHT;
static uint64_t name_pitch = TEST_PITCH;
static uint64_t name_format = KERN_BOOT_FRAMEBUFFER_BGRX8888;
static uint64_t name_port = TEST_UART_PORT;
static int names_screen = 1;

/* The framebuffer's memory, which the mapping stub hands out. */
static uint8_t screen_memory[TEST_PITCH * TEST_HEIGHT];

/* What the mapping stubs saw: the calls, and the state at each removal. */
static unsigned map_count;
static unsigned unmap_count;
static void *unmapped_pointer;
static size_t unmapped_size;
static int unmap_irq_off;
static int unmap_graphics_held;
static int unmap_framebuffer_held;
static int unmap_saw_ended;
static int unmap_saw_pixels;
static int unmap_error;

#if CONFIG_DRIVER_GRAPHICS_DEVICE
/* What the test's card backend saw, and what its end reports. */
static unsigned card_ends;
static unsigned card_leaves;
static uint64_t card_physical = 0x0000020000000000ULL;

/* The files that open /dev/graphics. */
static struct file file_first;
static struct file file_second;
#endif

/* The checks made and failed. */
static unsigned checks;
static unsigned failures;

static void expect(int condition, const char *what);
static int held(const char *name);
static uint64_t screen_sum(void);
static void start(int offer_backend, int register_device);
static void put_text(const char *text);
static int grid_has(const char *text);
static void other_putc(int character);
#if CONFIG_DRIVER_GRAPHICS_DEVICE
static int card_ready(void);
static size_t card_get_modes(struct graphics_mode_info *modes, size_t capacity);
static int card_enter(struct graphics_mode *mode);
static void card_leave(void);
static int card_fill(const struct graphics_rect *rect, uint32_t color);
static int card_line(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t color);
static int card_pattern_fill(const struct graphics_rect *rect, uint32_t color, uint64_t pattern);
static int card_blit_row(unsigned x, unsigned y, const struct drv_graphics_row *row, uint64_t pattern, int patterned);
static int card_flush(const struct graphics_rect *rects, size_t count);
static int card_get_glyph(uint32_t codepoint, uint8_t bitmap[32], unsigned *width, unsigned *height);
static int card_end(const struct kern_text_range *ranges, unsigned count);
#endif
static void scene_end(void);
static void scene_resume_reveal(void);
static void scene_unmap_failure(void);
static void scene_unmap_busy(void);
static void scene_backend_without_end(void);
static void scene_judged_again(void);
static void scene_before_register(void);
static void scene_no_framebuffer(void);
static void scene_foreign_output(void);
static void scene_refused_list(void);

#if CONFIG_DRIVER_GRAPHICS_DEVICE
/* A card driver's backend that keeps no mapping: no end. */
static const struct drv_graphics_backend card_without_end = {
	.name = "card without end",
	.ready = card_ready,
	.get_modes = card_get_modes,
	.enter = card_enter,
	.leave = card_leave,
	.fill = card_fill,
	.line = card_line,
	.pattern_fill = card_pattern_fill,
	.blit_row = card_blit_row,
	.flush = card_flush,
	.get_glyph = card_get_glyph
};

/* A card driver's backend whose end keeps a window at card_physical. */
static const struct drv_graphics_backend card_with_end = {
	.name = "card with end",
	.ready = card_ready,
	.get_modes = card_get_modes,
	.enter = card_enter,
	.leave = card_leave,
	.fill = card_fill,
	.line = card_line,
	.pattern_fill = card_pattern_fill,
	.blit_row = card_blit_row,
	.flush = card_flush,
	.get_glyph = card_get_glyph,
	.end = card_end
};
#endif

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

	/* Runs it. */
	switch (scene) {
	case 1:
		scene_end();
		break;
	case 2:
		scene_resume_reveal();
		break;
	case 3:
		scene_unmap_failure();
		break;
	case 4:
		scene_unmap_busy();
		break;
	case 5:
		scene_backend_without_end();
		break;
	case 6:
		scene_judged_again();
		break;
	case 7:
		scene_before_register();
		break;
	case 8:
		scene_no_framebuffer();
		break;
	case 9:
		scene_foreign_output();
		break;
	case 10:
		scene_refused_list();
		break;
	default:
		return 2;
	}

	/* One line for the scene. */
	printf("text-end-sun4u-host-test %d (graphics device %d): %u checks, %u failed: %s\n",
	       scene, CONFIG_DRIVER_GRAPHICS_DEVICE, checks, failures, failures == 0U ? "PASS" : "FAIL");
	if (failures != 0U)
		return 1;
	return 0;
}

/* Answers the boot handoff names. */
void *
kern_boot_handoff(
	const char *name)
{
	/* The serial port is always named. */
	if (strcmp(name, "sun4u.serial.port") == 0)
		return &name_port;

	/* The screen's names, when the scene has one. */
	if (!names_screen)
		return NULL;
	if (strcmp(name, "sun4u.framebuffer.phys") == 0)
		return &name_phys;
	if (strcmp(name, "sun4u.framebuffer.width") == 0)
		return &name_width;
	if (strcmp(name, "sun4u.framebuffer.height") == 0)
		return &name_height;
	if (strcmp(name, "sun4u.framebuffer.pitch") == 0)
		return &name_pitch;
	if (strcmp(name, "sun4u.framebuffer.format") == 0)
		return &name_format;
	return NULL;
}

/* Maps the screen: hands out the test's array. */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	(void)attributes;

	/* Only the named screen is mapped. */
	if (address != TEST_PHYSICAL || size != TEST_BYTES)
		return EINVAL;
	map_count++;
	*mapped = screen_memory;
	return 0;
}

/* Removes a mapping, recording what held and what was seen then. */
int
kern_device_unmap(
	void *mapped,
	size_t size)
{
	/* The state at the removal. */
	unmap_count++;
	unmapped_pointer = mapped;
	unmapped_size = size;
	unmap_irq_off = stub_irq_off;
	unmap_graphics_held = held("graphics device");
	unmap_framebuffer_held = held("sun4u framebuffer");
	unmap_saw_ended = __atomic_load_n(&screen_ended, __ATOMIC_ACQUIRE);
	unmap_saw_pixels = fb_pixels != NULL;

	/* The scene's answer. */
	return unmap_error;
}

/* Counts a check and reports a failed one. */
static void
expect(
	int condition,
	const char *what)
{
	/* Every check counts. */
	checks++;
	if (condition)
		return;

	/* A failed one is named. */
	failures++;
	printf("FAIL: %s\n", what);
}

/* Reports whether a sleepable lock of that name is held. */
static int
held(
	const char *name)
{
	unsigned index;

	/* Each lock held now. */
	for (index = 0; index < stub_held_count && index < 8U; index++) {
		if (strcmp(stub_held[index], name) == 0)
			return 1;
	}
	return 0;
}

/* Sums the screen's bytes, weighted by position, to see whether anything drew. */
static uint64_t
screen_sum(
	void)
{
	uint64_t sum;
	size_t index;

	/* Each byte. */
	sum = 0;
	for (index = 0; index < sizeof(screen_memory); index++)
		sum = sum * 31U + screen_memory[index];
	return sum;
}

/* Starts the console as the board does, with /dev/graphics as asked. */
static void
start(
	int offer_backend,
	int register_device)
{
	/* The serial port the console writes. */
	stub_uart_port = TEST_UART_PORT;

	/* The console, which maps the screen it is named. */
	drv_sun4u_console_init();

	/* The framebuffer offered to /dev/graphics. */
	if (offer_backend)
		(void)drv_sun4u_fb_register_backend();

#if CONFIG_DRIVER_GRAPHICS_DEVICE
	/* /dev/graphics chooses and registers. */
	if (register_device)
		(void)drv_graphics_device_register();
#else
	(void)register_device;
#endif
}

/* Writes characters through the kernel's console. */
static void
put_text(
	const char *text)
{
	/* Each character, as the console's users write it. */
	while (*text != '\0') {
		kern_text_putc((unsigned char)*text);
		text++;
	}
}

/* Reports whether a row of the console's grid begins with the text. */
static int
grid_has(
	const char *text)
{
	unsigned row;
	size_t length;

	/* Each row of the grid. */
	length = strlen(text);
	for (row = 0; row < screen_rows; row++) {
		if (memcmp(screen_grid[row], text, length) == 0)
			return 1;
	}
	return 0;
}

/* Another character path, which the test publishes in the console's stead. */
static void
other_putc(
	int character)
{
	(void)character;
}

#if CONFIG_DRIVER_GRAPHICS_DEVICE
/* The card is always ready. */
static int
card_ready(
	void)
{
	return 1;
}

/* The card's one mode. */
static size_t
card_get_modes(
	struct graphics_mode_info *modes,
	size_t capacity)
{
	/* Describes it when asked. */
	if (modes != NULL && capacity != 0) {
		modes[0].width = 8;
		modes[0].height = 8;
		modes[0].bits_per_pixel = 32;
		modes[0].stride = 32;
	}
	return 1;
}

/* Enters the card's mode. */
static int
card_enter(
	struct graphics_mode *mode)
{
	mode->width = 8;
	mode->height = 8;
	mode->bits_per_pixel = 32;
	mode->stride = 32;
	return 1;
}

/* Leaves the card's mode, counting it. */
static void
card_leave(
	void)
{
	card_leaves++;
}

/* Fills nothing. */
static int
card_fill(
	const struct graphics_rect *rect,
	uint32_t color)
{
	(void)rect;
	(void)color;
	return 1;
}

/* Draws no line. */
static int
card_line(
	unsigned x0,
	unsigned y0,
	unsigned x1,
	unsigned y1,
	uint32_t color)
{
	(void)x0;
	(void)y0;
	(void)x1;
	(void)y1;
	(void)color;
	return 1;
}

/* Fills no pattern. */
static int
card_pattern_fill(
	const struct graphics_rect *rect,
	uint32_t color,
	uint64_t pattern)
{
	(void)rect;
	(void)color;
	(void)pattern;
	return 1;
}

/* Draws no row. */
static int
card_blit_row(
	unsigned x,
	unsigned y,
	const struct drv_graphics_row *row,
	uint64_t pattern,
	int patterned)
{
	(void)x;
	(void)y;
	(void)row;
	(void)pattern;
	(void)patterned;
	return 1;
}

/* Pushes nothing. */
static int
card_flush(
	const struct graphics_rect *rects,
	size_t count)
{
	(void)rects;
	(void)count;
	return 1;
}

/* Has no glyph. */
static int
card_get_glyph(
	uint32_t codepoint,
	uint8_t bitmap[32],
	unsigned *width,
	unsigned *height)
{
	(void)codepoint;
	memset(bitmap, 0, 32);
	*width = 8;
	*height = 16;
	return 1;
}

/* The card's end: a window at card_physical stays mapped, judged against each call's ranges. */
static int
card_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	int overlaps;

	/* Counts the call, made under /dev/graphics's lock. */
	card_ends++;
	expect(held("graphics device"), "the card's end runs under the graphics lock");

	/* The window that stays. */
	overlaps = kern_text_range_overlaps(ranges, count, card_physical, 0x1000U);
	if (overlaps)
		return EBUSY;
	return 0;
}
#endif

/*
 * The end with /dev/graphics in a mode: 0 for ranges away from the
 * screen, the framebuffer's mapping removed once with the writers stopped,
 * /dev/graphics refusing, the grid following new characters, which reach
 * the serial port, and nothing drawn.
 */
static void
scene_end(
	void)
{
	struct kern_text_range ranges[1];
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	struct graphics_mode mode;
	uint32_t caps;
#endif
	uint64_t sum;
	int error;

	/* The console draws, /dev/graphics is registered over the framebuffer. */
	start(1, 1);
	expect(screen_on == 1, "the console draws the screen");
	expect(kernel_putc == console_kernel_putc, "the console publishes kernel_putc");
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	expect(drv_graphics_backend_selected() == &sun4u_fb_backend, "/dev/graphics chose the framebuffer");
	expect(stub_cdev_registers == 1U, "/dev/graphics registered");

	/* The owner enters the mode, and the console stops drawing for it. */
	error = stub_graphics_ops->open(&file_first);
	expect(error == 0, "the first open succeeds");
	memset(&mode, 0, sizeof(mode));
	error = stub_graphics_ops->ioctl(&file_first, KERN_GRAPHICS_ENTER, (uintptr_t)&mode);
	expect(error == 0, "the owner enters the mode");
	expect(screen_suspended == 1, "the mode suspends the console");
#endif

	/* The end, with memory away from the screen. */
	ranges[0].physical = 0x80000000ULL;
	ranges[0].bytes = 0x1000U;
	sum = screen_sum();
	error = kern_text_end(ranges, 1);
	expect(error == 0, "the end returns 0 for ranges away from the screen");
	expect(map_count == 1U, "the screen was mapped once");
	expect(unmap_count == 1U, "the screen's mapping is removed once");
	expect(unmapped_pointer == screen_memory, "the removal names the mapping");
	expect(unmapped_size == TEST_BYTES, "the removal names the mapping's size");
	expect(unmap_irq_off == 0, "the removal runs with interrupts on");
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	expect(unmap_graphics_held, "the removal runs under the graphics lock");
#else
	expect(!unmap_graphics_held, "no graphics lock without /dev/graphics");
#endif
	expect(unmap_framebuffer_held, "the removal runs under the framebuffer's end lock");
	expect(unmap_saw_ended, "the console stopped before the removal");
	expect(!unmap_saw_pixels, "nothing reaches the mapping at the removal");
	expect(stub_held_count == 0U, "no lock is held after the end");
	expect(stub_irq_off == 0, "interrupts are on after the end");
	expect(!drv_sun4u_fb_ready(), "the framebuffer is no longer ready");
	expect(screen_on == 1, "screen_on keeps the console's grid");
	expect(screen_ended == 1, "the console has ended");
	expect(screen_suspended == 1, "the console stays suspended");
	expect(kernel_putc == console_kernel_putc, "kernel_putc stays the console's");

#if CONFIG_DRIVER_GRAPHICS_DEVICE

	/* /dev/graphics refuses its owner and a new opener. */
	error = stub_graphics_ops->ioctl(&file_first, KERN_GRAPHICS_GET_CAPS, (uintptr_t)&caps);
	expect(error == ENODEV, "the owner's request is refused with ENODEV");
	error = stub_graphics_ops->open(&file_second);
	expect(error == ENODEV, "a new open is refused with ENODEV");

	/* Closing does not give the screen back. */
	error = stub_graphics_ops->close(&file_first);
	expect(error == 0, "the owner closes");
	expect(screen_suspended == 1, "the close leaves the console suspended");
	error = stub_graphics_ops->open(&file_first);
	expect(error == ENODEV, "an open after the close is refused with ENODEV");
#endif

	/* New characters reach the serial port and the grid, not the screen. */
	stub_uart_sent_count = 0;
	put_text("after the end\n");
	expect(strstr(stub_uart_sent, "after the end\r\n") != NULL, "the serial port gets the characters");
	expect(grid_has("after the end"), "the grid follows the characters");
	expect(screen_sum() == sum, "nothing is drawn on the screen after the end");
}

/*
 * A resume, /dev/graphics's restore of the text and a fatal stop's reveal
 * after the end draw nothing; before it, the reveal does (so the sum sees
 * drawing).
 */
static void
scene_resume_reveal(
	void)
{
	uint64_t sum;
	int error;

	/* The console draws; a reveal over a suspend draws the grid again. */
	start(1, 1);
	kern_text_suspend();
	sum = screen_sum();
	put_text("hidden\n");
	expect(screen_sum() == sum, "a suspended console does not draw");
	kern_text_reveal_fatal();
	expect(screen_sum() != sum, "a reveal before the end draws");
	expect(screen_suspended == 0, "a reveal before the end takes the screen back");

	/* The end, giving no memory back. */
	error = kern_text_end(NULL, 0);
	expect(error == 0, "the end without ranges returns 0");
	expect(unmap_count == 1U, "the mapping is removed");

	/* Nothing brings the console back to the screen. */
	sum = screen_sum();
	kern_text_resume();
	expect(screen_suspended == 1, "a resume does not take the screen back");
	expect(screen_on == 1, "a resume leaves screen_on as it was");
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	drv_graphics_device_restore_text();
	expect(screen_suspended == 1, "/dev/graphics's restore does not take the screen back");
#endif
	kern_text_suspend();
	kern_text_resume();
	expect(screen_suspended == 1, "a suspend and resume do not take the screen back");
	kern_text_reveal_fatal();
	expect(screen_suspended == 1, "a fatal stop's reveal does not take the screen back");
	put_text("fatal\n");
	expect(screen_sum() == sum, "nothing is drawn after the end");
	expect(grid_has("fatal"), "the grid follows the characters after a reveal");
}

/*
 * A removal that fails: its error for ranges over the screen, 0 for
 * others, and the mapping removed only once.
 */
static void
scene_unmap_failure(
	void)
{
	struct kern_text_range screen[1];
	struct kern_text_range elsewhere[1];
	int error;

	/* The removal will fail. */
	start(1, 1);
	unmap_error = EIO;
	screen[0].physical = TEST_PHYSICAL + 0x1000U;
	screen[0].bytes = 0x1000U;
	elsewhere[0].physical = TEST_PHYSICAL + TEST_BYTES;
	elsewhere[0].bytes = 0x1000U;

	/* Memory over the screen is reported with the removal's error. */
	error = kern_text_end(screen, 1);
	expect(error == EIO, "a failed removal over the ranges reports its error");
	expect(unmap_count == 1U, "the removal was tried once");
	expect(screen_ended == 1, "the console stopped all the same");
	expect(!drv_sun4u_fb_ready(), "nothing draws through the failed mapping");

	/* Memory beside it is not affected. */
	error = kern_text_end(elsewhere, 1);
	expect(error == 0, "the failed mapping does not meet ranges beside it");
	error = kern_text_end(NULL, 0);
	expect(error == 0, "no ranges, nothing to judge");
	expect(unmap_count == 1U, "a later end does not remove again");
}

/* A window the hardware layer keeps: EBUSY over the screen, 0 elsewhere. */
static void
scene_unmap_busy(
	void)
{
	struct kern_text_range ranges[2];
	int error;

	/* The hardware layer keeps the window. */
	start(1, 1);
	unmap_error = EBUSY;
	ranges[0].physical = 0x80000000ULL;
	ranges[0].bytes = 0x1000U;
	ranges[1].physical = TEST_PHYSICAL - 0x1000U;
	ranges[1].bytes = 0x2000U;

	/* The second range meets the screen's first page. */
	error = kern_text_end(ranges, 2);
	expect(error == EBUSY, "a kept window over the ranges is EBUSY");

	/* The first alone does not. */
	error = kern_text_end(ranges, 1);
	expect(error == 0, "a kept window beside the ranges is 0");
	expect(unmap_count == 1U, "the removal was tried once");
}

/*
 * A card's backend without end chosen by /dev/graphics: /dev/graphics is
 * refused and the end is 0; the framebuffer's mapping goes all the same,
 * outside the graphics lock.
 */
static void
scene_backend_without_end(
	void)
{
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	struct kern_text_range ranges[1];
	struct graphics_mode mode;
	int error;

	/* The card outranks the framebuffer. */
	stub_uart_port = TEST_UART_PORT;
	drv_sun4u_console_init();
	(void)drv_sun4u_fb_register_backend();
	error = drv_graphics_backend_register(&card_without_end, DRV_GRAPHICS_PRIORITY_CARD);
	expect(error == 0, "the card's backend is offered");
	(void)drv_graphics_device_register();
	expect(drv_graphics_backend_selected() == &card_without_end, "/dev/graphics chose the card");
	error = stub_graphics_ops->open(&file_first);
	expect(error == 0, "the card's device opens before the end");
	memset(&mode, 0, sizeof(mode));
	error = stub_graphics_ops->ioctl(&file_first, KERN_GRAPHICS_ENTER, (uintptr_t)&mode);
	expect(error == 0, "the owner enters the card's mode");

	/* The end. */
	ranges[0].physical = card_physical;
	ranges[0].bytes = 0x1000U;
	error = kern_text_end(ranges, 1);
	expect(error == 0, "a backend without end gives 0");
	expect(card_leaves == 0U, "the card's mode is not left with the hardware");
	expect(unmap_count == 1U, "the framebuffer's mapping goes all the same");
	expect(!unmap_graphics_held, "the framebuffer's own end is outside the graphics lock");
	expect(unmap_framebuffer_held, "the framebuffer's own end holds its lock");

	/* /dev/graphics is refused. */
	error = stub_graphics_ops->open(&file_second);
	expect(error == ENODEV, "an open after the end is refused with ENODEV");
#else
	expect(1, "no /dev/graphics in this build");
#endif
}

/* A later call is judged again against its own ranges, through the chosen backend's end. */
static void
scene_judged_again(
	void)
{
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	struct kern_text_range card[1];
	struct kern_text_range elsewhere[1];
	int error;

	/* The card with an end outranks the framebuffer. */
	stub_uart_port = TEST_UART_PORT;
	drv_sun4u_console_init();
	(void)drv_sun4u_fb_register_backend();
	(void)drv_graphics_backend_register(&card_with_end, DRV_GRAPHICS_PRIORITY_CARD);
	(void)drv_graphics_device_register();
	card[0].physical = card_physical;
	card[0].bytes = 0x1000U;
	elsewhere[0].physical = 0x80000000ULL;
	elsewhere[0].bytes = 0x1000U;

	/* The first end, away from the card's window. */
	error = kern_text_end(elsewhere, 1);
	expect(error == 0, "the first end away from the card's window is 0");
	expect(card_ends == 1U, "the card's end ran");

	/* A second, over it, is judged again. */
	error = kern_text_end(card, 1);
	expect(error == EBUSY, "a second end over the card's window is EBUSY");
	expect(card_ends == 2U, "the card's end judged again");

	/* A third, away again. */
	error = kern_text_end(elsewhere, 1);
	expect(error == 0, "a third end away from the window is 0");
	expect(unmap_count == 1U, "the framebuffer's mapping was removed once");
#else
	expect(1, "no /dev/graphics in this build");
#endif
}

/*
 * The end before /dev/graphics registers: no lock, the choice fixed, and
 * the device registered later refuses every open.
 */
static void
scene_before_register(
	void)
{
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	int error;

	/* The framebuffer is offered; /dev/graphics has not registered. */
	start(1, 0);

	/* The end. */
	error = kern_text_end(NULL, 0);
	expect(error == 0, "the end before /dev/graphics registers is 0");
	expect(unmap_count == 1U, "the mapping is removed");
	expect(!unmap_graphics_held, "no graphics lock exists yet");

	/* A later offer is refused, and the device registers with the choice made. */
	error = drv_graphics_backend_register(&card_without_end, DRV_GRAPHICS_PRIORITY_CARD);
	expect(error == EBUSY, "a backend offered after the end is refused");
	error = drv_graphics_device_register();
	expect(error == 0, "/dev/graphics registers after the end");
	expect(drv_graphics_backend_selected() == &sun4u_fb_backend, "the choice stays the framebuffer");
	error = stub_graphics_ops->open(&file_first);
	expect(error == ENODEV, "an open of the late device is refused with ENODEV");
	error = kern_text_end(NULL, 0);
	expect(error == 0, "a later end under the lock is 0");
	expect(unmap_count == 1U, "the mapping was removed once");
#else
	expect(1, "no /dev/graphics in this build");
#endif
}

/*
 * No framebuffer the console can draw: the hardware layer draws until the
 * end publishes the console's output, which then writes the serial port.
 */
static void
scene_no_framebuffer(
	void)
{
	struct kern_text_range ranges[1];
	int error;

	/* The hardware layer names no screen the driver maps. */
	names_screen = 0;
	start(1, 1);
	expect(screen_on == 0, "the console does not draw");
	expect(kernel_putc == NULL, "kernel_putc is not published");
	put_text("x");
	expect(stub_hal_drawn == 1U, "the hardware layer draws before the end");

	/* The end. */
	ranges[0].physical = 0x80000000ULL;
	ranges[0].bytes = 0x1000U;
	error = kern_text_end(ranges, 1);
	expect(error == 0, "the end without a framebuffer is 0");
	expect(kernel_putc == console_kernel_putc, "the end publishes the console's output");
	expect(unmap_count == 0U, "nothing was mapped to remove");

	/* The characters reach the serial port only. */
	stub_uart_sent_count = 0;
	put_text("ok\n");
	expect(strcmp(stub_uart_sent, "ok\r\n") == 0, "the serial port gets the characters");
	expect(stub_hal_drawn == 1U, "the hardware layer draws nothing after the end");
}

/* Another character path published: EBUSY with ranges, 0 without. */
static void
scene_foreign_output(
	void)
{
	struct kern_text_range ranges[1];
	int error;

	/* Someone else took the kernel's output. */
	start(1, 1);
	kernel_putc = other_putc;
	ranges[0].physical = 0x80000000ULL;
	ranges[0].bytes = 0x1000U;

	/* It may draw anywhere. */
	error = kern_text_end(ranges, 1);
	expect(error == EBUSY, "another output with ranges is EBUSY");
	expect(kernel_putc == other_putc, "the other output is left published");
	expect(unmap_count == 1U, "the mapping goes all the same");
	error = kern_text_end(NULL, 0);
	expect(error == 0, "another output without ranges is 0");
}

/* A list kern_text_end() refuses changes nothing. */
static void
scene_refused_list(
	void)
{
	struct kern_text_range ranges[5];
	int error;

	/* Five ranges are too many. */
	start(1, 1);
	memset(ranges, 0, sizeof(ranges));
	ranges[0].bytes = 1;
	error = kern_text_end(ranges, 5);
	expect(error == EINVAL, "an oversized list is EINVAL");
	expect(screen_ended == 0, "the refused end leaves the console");
	expect(unmap_count == 0U, "the refused end removes nothing");
	expect(drv_sun4u_fb_ready(), "the framebuffer stays ready");
}
