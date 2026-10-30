/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the console's leaving of the display on the boards other
 * than the PC/AT: the PC-98 (its text layer, /dev/graphics and backend),
 * the Raspberry Pi 4's console and the VisionFive 2's binding over the
 * boot framebuffer's text console, the X68000 console, the polled sending
 * of the 16550 UART driver, and the serial-only console, each over the real src/kern/text-display.c and src/kern/klog.c,
 * with the kernel's services as stubs (text-end-boards-stubs.c).
 *
 * Each scene runs in its own process (the layers keep their state in
 * statics): text-end-boards-host-test SCENE.  The PC-98 text layer writes
 * text memory at its fixed kernel address, which the test maps; a build
 * with TEXT_END_NO_PC98 leaves the PC-98 scenes out (the address lies in
 * the address sanitizer's shadow memory).  text-end-boards-host-test.sh
 * runs them all.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "hal/hal.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/klog.h"
#include "kern/text-display.h"
#include "uapi/errno.h"
#include "uapi/graphics.h"
#include "drivers/generic/fdt.h"
#include "drivers/generic/serial-console.h"
#include "drivers/platform/pc98/graphics/backend.h"
#include "drivers/platform/pc98/graphics/text.h"
#include "drivers/platform/rpi4/rpi4-console.h"
#include "drivers/platform/x68k/x68k-console.h"
#include "drivers/serial/ns16550.h"
#include "kern/boot.h"
#include "kern/platform.h"

/* The PC-98 text memory: its kernel address and the bytes of both planes. */
#define PC98_TEXT_ADDRESS	0x800a0000UL
#define PC98_TEXT_BYTES		0x4000U

/* The PC-98's prepare, which the board's start-up calls. */
int drv_pc98_graphics_prepare(void);
int drv_graphics_device_register(void);
void drv_graphics_device_restore_text(void);

/* What the stubs saw and give (text-end-boards-stubs.c). */
extern unsigned hal_drawn;
extern uint32_t uart_console_registers[64];
extern uint8_t uart_byte_registers[16];
extern unsigned uart_busy_reads;
extern int uart_never_ready;
extern unsigned uart_status_reads;
extern uint8_t uart_sent[512];
extern unsigned uart_sent_count;
extern unsigned uart_other_writes;
extern unsigned mapping_count;
extern unsigned unmap_count;
extern uint64_t unmapped_physical[16];
extern size_t unmapped_size[16];
extern uint64_t unmap_fail_physical;
extern int unmap_fail_error;
extern const char *screen_name;
extern uint64_t screen_physical;
extern uint64_t screen_size;
extern unsigned irq_disables;
extern unsigned irq_enables;
extern unsigned pc98_enters;
extern unsigned pc98_fills;
extern unsigned pc98_leaves;
extern const struct cdev_ops *graphics_cdev;
extern uint64_t fdt_range[2];

void vf2_test_set_fatal_display(void (*display)(void));
int vf2_test_uart_present(void);
uint64_t vf2_test_uart_physical(void);

/* How the test's device tree names the console. */
enum tree_console {
	TREE_ALIAS,
	TREE_PATH,
	TREE_NONE,
	TREE_NO_INTERRUPT
};

/* A flattened device tree under construction. */
struct tree_builder {
	uint8_t structure[2048];
	uint32_t structure_size;
	char strings[512];
	uint32_t strings_size;
};

static uint8_t tree_blob[4096] __attribute__((aligned(8)));
static unsigned fatal_displays;
static unsigned failures;
static unsigned checks;

static void expect(int condition, const char *what);
static void tree_word(struct tree_builder *tree, uint32_t value);
static void tree_begin(struct tree_builder *tree, const char *name);
static void tree_end(struct tree_builder *tree);
static void tree_property(struct tree_builder *tree, const char *name, const void *value, uint32_t length);
static void tree_cells(struct tree_builder *tree, const char *name, const uint32_t *cells, unsigned count);
static void tree_string(struct tree_builder *tree, const char *name, const char *value);
static void tree_uart(struct tree_builder *tree, const char *name, uint32_t base, int interrupt);
static void tree_build(enum tree_console console, struct drv_fdt *fdt);
static void vf2_start(enum tree_console console);
static void fatal_display(void);
static void other_putc(int character);
static int sent_is(const char *text);
static void sent_reset(void);
#ifndef TEXT_END_NO_PC98
static uint64_t pc98_text_sum(void);
static void pc98_start(int register_device);
static void scene_pc98(void);
static void scene_pc98_text_memory(void);
static void scene_pc98_unmap_failure(void);
static void scene_pc98_before_device(void);
static void scene_pc98_kernel_path(void);
static void scene_pc98_unmap_busy(void);
#endif
static void scene_rpi4(void);
static void scene_x68k(void);
static void scene_ns16550_send(void);
static void scene_ns16550_probe(void);
static void scene_vf2(void);
static void scene_vf2_ranges(void);
static void scene_vf2_no_uart(void);
static void scene_serial_only(void);

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
	kern_log_init();
	switch (scene) {
#ifndef TEXT_END_NO_PC98
	case 1:
		scene_pc98();
		break;
	case 2:
		scene_pc98_text_memory();
		break;
	case 3:
		scene_pc98_unmap_failure();
		break;
	case 4:
		scene_pc98_before_device();
		break;
	case 5:
		scene_pc98_kernel_path();
		break;
	case 14:
		scene_pc98_unmap_busy();
		break;
#else
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 14:
		printf("text-end-boards-host-test %d: PC-98 left out of this build\n", scene);
		return 0;
#endif
	case 6:
		scene_rpi4();
		break;
	case 7:
		scene_x68k();
		break;
	case 8:
		scene_ns16550_send();
		break;
	case 9:
		scene_ns16550_probe();
		break;
	case 10:
		scene_vf2();
		break;
	case 11:
		scene_vf2_ranges();
		break;
	case 12:
		scene_vf2_no_uart();
		break;
	case 13:
		scene_serial_only();
		break;
	default:
		return 2;
	}

	/* The scene's summary. */
	printf("text-end-boards-host-test %d: %u passed, %u failed\n", scene, checks - failures, failures);
	if (failures != 0U)
		return 1;
	return 0;
}

#ifndef TEXT_END_NO_PC98
/* PC-98: the end stops the text and /dev/graphics, and removes the planes' and the Cirrus aperture's mappings. */
static void
scene_pc98(void)
{
	struct graphics_mode mode;
	struct graphics_fill fill;
	struct kern_text_range range;
	struct file owner;
	unsigned row;
	unsigned column;
	unsigned column_after;
	unsigned index;
	uint64_t before;
	int visible;
	int found;
	int error;

	/* The board up, /dev/graphics registered, the text drawing. */
	pc98_start(1);
	before = pc98_text_sum();
	kern_text_putc('A');
	expect(pc98_text_sum() != before, "pc98: a character reaches text memory");
	expect(kernel_putc == drv_pc98_text_putc, "pc98: the text layer is the kernel's path");

	/* A graphics mode entered by an owner. */
	memset(&owner, 0, sizeof(owner));
	error = graphics_cdev->open(&owner);
	expect(error == 0, "pc98: /dev/graphics opens");
	memset(&mode, 0, sizeof(mode));
	error = graphics_cdev->ioctl(&owner, KERN_GRAPHICS_ENTER, (uintptr_t)&mode);
	expect(error == 0 && pc98_enters == 1U, "pc98: the mode is entered");

	/* The end, with a range away from everything the console keeps. */
	range.physical = 0xc0000000ULL;
	range.bytes = 0x10000000ULL;
	error = kern_text_end(&range, 1U);
	expect(error == 0, "pc98: end 0 for a range away from the display");
	expect(unmap_count == 5U, "pc98: the four planes and the Cirrus aperture are unmapped");
	found = 0;
	for (index = 0U; index < unmap_count && index < 16U; index++) {
		if (unmapped_physical[index] == 0xa8000U && unmapped_size[index] == 0x8000U)
			found |= 1;
		if (unmapped_physical[index] == 0xb0000U && unmapped_size[index] == 0x8000U)
			found |= 2;
		if (unmapped_physical[index] == 0xb8000U && unmapped_size[index] == 0x8000U)
			found |= 4;
		if (unmapped_physical[index] == 0xe0000U && unmapped_size[index] == 0x8000U)
			found |= 8;
		if (unmapped_physical[index] == 0xf0000000U && unmapped_size[index] == 0x400000U)
			found |= 16;
	}
	expect(found == 31, "pc98: each mapping is removed at its place and size");
	expect(pc98_leaves == 0U, "pc98: the end writes no hardware to leave the mode");

	/* /dev/graphics refuses everything; the close does not bring the text back. */
	memset(&fill, 0, sizeof(fill));
	fill.rect.width = 1U;
	fill.rect.height = 1U;
	error = graphics_cdev->ioctl(&owner, KERN_GRAPHICS_FILL_RECT, (uintptr_t)&fill);
	expect(error == ENODEV && pc98_fills == 0U, "pc98: a request after the end is ENODEV");
	before = pc98_text_sum();
	error = graphics_cdev->close(&owner);
	expect(error == 0 && pc98_text_sum() == before, "pc98: the close is 0 and redraws nothing");
	error = graphics_cdev->open(&owner);
	expect(error == ENODEV, "pc98: an open after the end is ENODEV");
	drv_graphics_device_restore_text();
	expect(pc98_text_sum() == before, "pc98: restoring the text draws nothing");

	/* Characters move the cursor (the cells) and draw nothing; a resume draws nothing. */
	kern_text_get_cursor(&row, &column, &visible);
	kern_text_putc('B');
	kern_text_putc('C');
	kern_text_get_cursor(&row, &column_after, &visible);
	expect(column_after == column + 2U, "pc98: the cells follow the characters");
	kern_text_write(2U, 0U, KERN_TEXT_ATTRIB_NORMAL, "XYZ");
	kern_text_clear();
	(void)kern_text_set_cursor(3U, 3U);
	kern_text_suspend();
	kern_text_resume();
	kern_text_reveal_fatal();
	expect(pc98_text_sum() == before, "pc98: write, clear, cursor, suspend, resume and reveal_fatal draw nothing");

	/* A second end judges again and removes nothing more. */
	error = kern_text_end(NULL, 0U);
	expect(error == 0 && unmap_count == 5U, "pc98: a second end is 0 and unmaps nothing");
}

/* PC-98: text memory stays mapped, so a range over it is EBUSY, and the console still ends. */
static void
scene_pc98_text_memory(void)
{
	struct kern_text_range range;
	uint64_t before;
	int error;

	/* The board up. */
	pc98_start(1);

	/* A range over the text memory's attribute plane. */
	range.physical = 0xa3000U;
	range.bytes = 0x1000U;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY, "pc98 text: EBUSY for a range over text memory");
	expect(unmap_count == 5U, "pc98 text: the mappings are removed all the same");

	/* The console has ended all the same. */
	before = pc98_text_sum();
	kern_text_putc('Q');
	kern_text_resume();
	expect(pc98_text_sum() == before, "pc98 text: the console draws nothing");

	/* A range just past it is 0. */
	range.physical = 0xa4000U;
	range.bytes = 0x4000U;
	error = kern_text_end(&range, 1U);
	expect(error == 0, "pc98 text: 0 for a range just past text memory");
}

/* PC-98: a removal that fails is left, and judged by every end against its own ranges. */
static void
scene_pc98_unmap_failure(void)
{
	struct kern_text_range range;
	int error;

	/* The board up, the second plane's removal failing. */
	pc98_start(1);
	unmap_fail_physical = 0xb0000U;
	unmap_fail_error = EIO;

	/* A range over that plane: its error. */
	range.physical = 0xb0000U;
	range.bytes = 0x100U;
	error = kern_text_end(&range, 1U);
	expect(error == EIO && unmap_count == 4U, "pc98 failure: the removal's error, the others removed");

	/* Again: the same, with no new removal. */
	error = kern_text_end(&range, 1U);
	expect(error == EIO && unmap_count == 4U, "pc98 failure: a second end reports it again, unmapping nothing");

	/* A range away from it, and no ranges: 0. */
	range.physical = 0xc0000000ULL;
	range.bytes = 0x1000U;
	error = kern_text_end(&range, 1U);
	expect(error == 0, "pc98 failure: 0 for a range away from it");
	error = kern_text_end(NULL, 0U);
	expect(error == 0, "pc98 failure: 0 without ranges");
}

/* PC-98: a mapping the hardware layer keeps (its EBUSY) is left without an error: EBUSY over it. */
static void
scene_pc98_unmap_busy(void)
{
	struct kern_text_range range;
	int error;

	/* The board up, the Cirrus aperture kept by the hardware layer. */
	pc98_start(1);
	unmap_fail_physical = 0xf0000000U;
	unmap_fail_error = EBUSY;

	/* A range over it: EBUSY; a range away from it: 0. */
	range.physical = 0xf0100000ULL;
	range.bytes = 0x1000U;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && unmap_count == 4U, "pc98 busy: EBUSY over a mapping the hardware layer keeps");
	range.physical = 0xf0400000ULL;
	error = kern_text_end(&range, 1U);
	expect(error == 0, "pc98 busy: 0 just past it");
}

/* PC-98: an end before /dev/graphics is registered; the device then refuses its first open. */
static void
scene_pc98_before_device(void)
{
	struct file owner;
	int error;

	/* The board up, the device not yet registered. */
	pc98_start(0);
	error = kern_text_end(NULL, 0U);
	expect(error == 0 && unmap_count == 5U, "pc98 early: 0 and the mappings removed without the lock");

	/* The device registered later refuses opens. */
	error = drv_graphics_device_register();
	expect(error == 0, "pc98 early: the device registers");
	memset(&owner, 0, sizeof(owner));
	error = graphics_cdev->open(&owner);
	expect(error == ENODEV, "pc98 early: its open is ENODEV");
}

/* PC-98: the kernel's path is published when absent; another one makes ranges EBUSY. */
static void
scene_pc98_kernel_path(void)
{
	struct kern_text_range range;
	int error;

	/* The board up with nothing published as the kernel's path. */
	pc98_start(1);
	kernel_putc = NULL;
	error = kern_text_end(NULL, 0U);
	expect(error == 0 && kernel_putc == drv_pc98_text_putc, "pc98 path: the text layer's path is published");

	/* Another path: EBUSY with ranges, left alone; 0 without. */
	kernel_putc = other_putc;
	range.physical = 0xc0000000ULL;
	range.bytes = 0x1000U;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && kernel_putc == other_putc, "pc98 path: EBUSY with ranges under another path");
	error = kern_text_end(NULL, 0U);
	expect(error == 0, "pc98 path: 0 without ranges");
}
#endif

/* Raspberry Pi 4: the console draws the firmware's screen itself, and the end gives its mapping back. */
static void
scene_rpi4(void)
{
	struct kern_text_range range;
	unsigned mapped;
	int error;

	/* The console up over the firmware's screen: the kernel's path is the console's from the start. */
	screen_name = "rpi4.framebuffer";
	screen_physical = 0x3e000000ULL;
	screen_size = 640U * 480U * 4U;
	drv_rpi4_console_init();
	mapped = mapping_count;
	expect(mapped == 1U && kernel_putc == kern_text_kernel_putc, "rpi4: the screen mapped and the kernel's path published");

	/* Characters, a suspend and a resume never reach the hardware layer. */
	hal_drawn = 0U;
	kern_text_putc('A');
	kern_text_suspend();
	kern_text_putc('B');
	kern_text_resume();
	expect(hal_drawn == 0U, "rpi4: the hardware layer draws nothing");

	/* The end over the screen: the mapping goes, and the screen the hardware layer keeps counts as left. */
	range.physical = 0x3e040000ULL;
	range.bytes = 0x1000U;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && unmap_count == 1U && unmapped_physical[0] == 0x3e000000ULL, "rpi4: EBUSY over the screen, the mapping given back");

	/* A range away from it: 0, nothing more given back. */
	range.physical = 0x600000000ULL;
	error = kern_text_end(&range, 1U);
	expect(error == 0 && unmap_count == 1U, "rpi4: 0 for a range away from it");

	/* Characters after the end still go nowhere near the hardware layer, and a resume draws nothing. */
	kern_text_putc('C');
	kern_text_resume();
	kern_text_reveal_fatal();
	expect(hal_drawn == 0U, "rpi4: after the end the hardware layer still draws nothing");
}

/* X68000: the end publishes a path that drops every character. */
static void
scene_x68k(void)
{
	struct kern_text_range range;
	int error;

	/* The console up: characters reach the hardware layer's text screen. */
	drv_x68k_console_init();
	kern_text_putc('A');
	expect(hal_drawn == 1U, "x68k: a character is drawn before the end");

	/* The end. */
	error = kern_text_end(NULL, 0U);
	expect(error == 0 && kernel_putc != NULL, "x68k: 0, a path published");

	/* Nothing is drawn again, by the console or by the hardware layer's own lines. */
	kern_text_putc('B');
	hal_putc('C');
	kern_logf("x68k log line\n");
	expect(hal_drawn == 1U, "x68k: nothing is drawn after the end");

	/* Again, with ranges: nothing is left. */
	range.physical = 0x00e00000ULL;
	range.bytes = 0x80000ULL;
	error = kern_text_end(&range, 1U);
	expect(error == 0, "x68k: a later end with ranges is 0");

	/* Another path published: EBUSY with ranges only. */
	kernel_putc = other_putc;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && kernel_putc == other_putc, "x68k: EBUSY under another path with ranges");
	error = kern_text_end(NULL, 0U);
	expect(error == 0, "x68k: 0 under another path without ranges");
}

/* 16550: the polled send waits for the transmitter, drops a byte it never frees for, and gives the line back. */
static void
scene_ns16550_send(void)
{
	struct drv_ns16550 port;

	/* A 32-bit UART four bytes apart. */
	memset(&port, 0, sizeof(port));
	port.base = (uintptr_t)uart_console_registers;
	port.reg_shift = 2U;
	port.io_width = 4U;

	/* A transmitter busy for three reads. */
	uart_busy_reads = 3U;
	drv_ns16550_putc_polled(&port, 'a');
	expect(uart_sent_count == 1U && uart_sent[0] == 'a', "16550: the byte is sent");
	expect(uart_status_reads == 4U, "16550: the status is read until the transmitter frees");
	expect(port.output_lock == 0U && irq_disables == 1U && irq_enables == 1U, "16550: the line and the interrupts are given back");

	/* A transmitter that never frees: the byte is dropped after a bounded wait. */
	uart_never_ready = 1;
	uart_status_reads = 0U;
	drv_ns16550_putc_polled(&port, 'b');
	expect(uart_sent_count == 1U, "16550: a byte the transmitter never takes is dropped");
	expect(uart_status_reads == 100000U, "16550: the wait is bounded");
	expect(port.output_lock == 0U && irq_enables == 2U, "16550: the line and the interrupts are given back after a drop");

	/* A line another processor never lets go: the byte goes out without it, the holder's mark kept. */
	uart_never_ready = 0;
	port.output_lock = 1U;
	drv_ns16550_putc_polled(&port, 'c');
	expect(uart_sent_count == 2U && uart_sent[1] == 'c', "16550: a byte goes out past a holder that never lets go");
	expect(port.output_lock == 1U, "16550: the holder's mark is not cleared");

	/* A byte-wide UART one byte apart. */
	memset(&port, 0, sizeof(port));
	port.base = (uintptr_t)uart_byte_registers;
	port.io_width = 1U;
	drv_ns16550_putc_polled(&port, 'd');
	expect(uart_sent_count == 3U && uart_sent[2] == 'd', "16550: a byte-wide UART is sent to");
	expect(uart_other_writes == 0U, "16550: nothing reached another UART");
}

/* 16550: the console probe follows /chosen/stdout-path; the first-UART probe is unchanged. */
static void
scene_ns16550_probe(void)
{
	struct drv_ns16550 port;
	struct drv_fdt fdt;
	int error;

	/* The first-UART probe takes the first present UART of the tree, as before. */
	tree_build(TREE_ALIAS, &fdt);
	memset(&port, 0, sizeof(port));
	error = drv_ns16550_probe(&fdt, &port);
	expect(error == 0 && port.physical == 0x10010000ULL && port.irq == 33, "16550 probe: the first present UART");

	/* The console probe resolves the alias to the console's UART. */
	memset(&port, 0, sizeof(port));
	error = drv_ns16550_probe_console(&fdt, &port);
	expect(error == 0 && port.physical == 0x10000000ULL, "16550 probe: the alias names the console's UART");
	expect(port.reg_shift == 2U && port.io_width == 4U && port.busy_detect && port.irq == 32, "16550 probe: its spacing, width, kind and interrupt");
	expect(port.base == (uintptr_t)uart_console_registers && !port.input_started, "16550 probe: mapped, input not started");

	/* A path with options names the same UART. */
	tree_build(TREE_PATH, &fdt);
	memset(&port, 0, sizeof(port));
	error = drv_ns16550_probe_console(&fdt, &port);
	expect(error == 0 && port.physical == 0x10000000ULL, "16550 probe: a path names the console's UART");

	/* A tree that names no console. */
	tree_build(TREE_NONE, &fdt);
	error = drv_ns16550_probe_console(&fdt, &port);
	expect(error == ENOENT, "16550 probe: ENOENT without stdout-path");

	/* A console UART without an interrupt: sent to, not read. */
	tree_build(TREE_NO_INTERRUPT, &fdt);
	memset(&port, 0, sizeof(port));
	error = drv_ns16550_probe_console(&fdt, &port);
	expect(error == 0 && port.irq == 0, "16550 probe: a console UART without an interrupt is mapped");
	error = drv_ns16550_start_input(&port, drv_serial_console_receive);
	expect(error == ENOENT, "16550 probe: its input cannot start");
}

/* VisionFive 2: the console sends to the UART and draws the loader's screen; the end leaves the screen. */
static void
scene_vf2(void)
{
	char line[] = "vf2 log line\n";
	int error;

	/* The board up with its console UART and the loader's screen; the kernel's path is the console's. */
	screen_name = "riscv64.framebuffer";
	screen_physical = 0xfe000000ULL;
	screen_size = 640U * 480U * 4U;
	vf2_start(TREE_ALIAS);
	expect(vf2_test_uart_present() && vf2_test_uart_physical() == 0x10000000ULL, "vf2: the console UART found by stdout-path");
	expect(kernel_putc == kern_text_kernel_putc, "vf2: the kernel's path published at the start");

	/* The console's characters reach the UART, a new line with its carriage return; the hardware layer draws nothing. */
	kern_text_putc('B');
	kern_text_putc('\n');
	expect(sent_is("B\r\n") && hal_drawn == 0U, "vf2: the console's characters reach the UART only");

	/* The log's record reaches the UART once. */
	sent_reset();
	kern_log_write(line, strlen(line));
	expect(sent_is("vf2 log line\r\n"), "vf2: a log record reaches the UART once");

	/* Before the end a fatal stop takes the display back. */
	kern_text_reveal_fatal();
	expect(fatal_displays == 1U, "vf2: before the end a fatal stop takes the display back");

	/* The end. */
	error = kern_text_end(NULL, 0U);
	expect(error == 0 && kernel_putc == kern_text_kernel_putc, "vf2: end 0, the path kept");

	/* After the end the characters still reach the UART, and a fatal stop takes no display back. */
	sent_reset();
	kern_text_putc('C');
	kern_text_reveal_fatal();
	expect(sent_is("C") && fatal_displays == 1U && hal_drawn == 0U, "vf2: after the end the UART only, no display taken back");
	expect(uart_other_writes == 0U, "vf2: nothing reached the UART that is not the console");
}

/* VisionFive 2: the loader's screen, which the hardware layer keeps mapped, judged against the ranges. */
static void
scene_vf2_ranges(void)
{
	struct kern_text_range range;
	int error;

	/* The board up, with the loader's screen. */
	screen_name = "riscv64.framebuffer";
	screen_physical = 0xfe000000ULL;
	screen_size = 0x1000000ULL;
	vf2_start(TREE_ALIAS);

	/* A range over the screen: EBUSY, the console's mapping given back all the same. */
	range.physical = 0xfe800000ULL;
	range.bytes = 0x1000U;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && unmap_count == 1U && kernel_putc == kern_text_kernel_putc, "vf2 ranges: EBUSY over the screen, mapping given back");

	/* A range away from it: 0. */
	range.physical = 0x40000000ULL;
	range.bytes = 0x10000000ULL;
	error = kern_text_end(&range, 1U);
	expect(error == 0, "vf2 ranges: 0 for a range away from the screen");

	/* Another path published: EBUSY with ranges. */
	kernel_putc = other_putc;
	error = kern_text_end(&range, 1U);
	expect(error == EBUSY && kernel_putc == other_putc, "vf2 ranges: EBUSY under another path");
}

/* VisionFive 2: without the console's UART or a screen the end changes nothing. */
static void
scene_vf2_no_uart(void)
{
	int error;

	/* The board up with a tree that names no console, and no screen. */
	vf2_start(TREE_NONE);
	expect(!vf2_test_uart_present() && kernel_putc == NULL, "vf2 no UART: none found, nothing published");

	/* ENOTSUP, nothing published, the hardware layer prints and a stop takes the display back. */
	error = kern_text_end(NULL, 0U);
	expect(error == ENOTSUP && kernel_putc == NULL, "vf2 no UART: ENOTSUP, nothing changed");
	kern_text_putc('A');
	kern_platform_debug_write("x");
	kern_text_reveal_fatal();
	expect(hal_drawn == 2U && fatal_displays == 1U && uart_sent_count == 0U, "vf2 no UART: the hardware layer prints and a stop takes the display back");
}

/* The serial-only console has no end. */
static void
scene_serial_only(void)
{
	struct kern_text_range range;
	int error;

	/* The teletype published. */
	drv_serial_console_init();

	/* ENOTSUP with or without ranges; nothing published. */
	error = kern_text_end(NULL, 0U);
	expect(error == ENOTSUP && kernel_putc == NULL, "serial: ENOTSUP without ranges");
	range.physical = 0x80000000ULL;
	range.bytes = 0x1000U;
	error = kern_text_end(&range, 1U);
	expect(error == ENOTSUP && kernel_putc == NULL, "serial: ENOTSUP with ranges");
	kern_text_putc('A');
	expect(hal_drawn == 1U, "serial: characters still reach the hardware layer");
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

/* Appends one big-endian word to the structure block. */
static void
tree_word(
	struct tree_builder *tree,
	uint32_t value)
{
	tree->structure[tree->structure_size + 0U] = (uint8_t)(value >> 24);
	tree->structure[tree->structure_size + 1U] = (uint8_t)(value >> 16);
	tree->structure[tree->structure_size + 2U] = (uint8_t)(value >> 8);
	tree->structure[tree->structure_size + 3U] = (uint8_t)value;
	tree->structure_size += 4U;
}

/* Opens a node. */
static void
tree_begin(
	struct tree_builder *tree,
	const char *name)
{
	size_t length;

	/* The token, then the name padded to a word. */
	tree_word(tree, 1U);
	length = strlen(name) + 1U;
	memcpy(tree->structure + tree->structure_size, name, length);
	tree->structure_size += (uint32_t)((length + 3U) & ~(size_t)3U);
}

/* Closes a node. */
static void
tree_end(
	struct tree_builder *tree)
{
	tree_word(tree, 2U);
}

/* Adds a property with its name in the strings block. */
static void
tree_property(
	struct tree_builder *tree,
	const char *name,
	const void *value,
	uint32_t length)
{
	size_t name_length;

	/* The token, the value's length and the name's offset. */
	tree_word(tree, 3U);
	tree_word(tree, length);
	tree_word(tree, tree->strings_size);
	name_length = strlen(name) + 1U;
	memcpy(tree->strings + tree->strings_size, name, name_length);
	tree->strings_size += (uint32_t)name_length;

	/* The value padded to a word. */
	memcpy(tree->structure + tree->structure_size, value, length);
	tree->structure_size += (length + 3U) & ~3U;
}

/* Adds a property of big-endian cells. */
static void
tree_cells(
	struct tree_builder *tree,
	const char *name,
	const uint32_t *cells,
	unsigned count)
{
	uint8_t bytes[64];
	unsigned index;

	/* Each cell big-endian. */
	for (index = 0U; index < count; index++) {
		bytes[index * 4U + 0U] = (uint8_t)(cells[index] >> 24);
		bytes[index * 4U + 1U] = (uint8_t)(cells[index] >> 16);
		bytes[index * 4U + 2U] = (uint8_t)(cells[index] >> 8);
		bytes[index * 4U + 3U] = (uint8_t)cells[index];
	}
	tree_property(tree, name, bytes, count * 4U);
}

/* Adds a property holding a terminated string. */
static void
tree_string(
	struct tree_builder *tree,
	const char *name,
	const char *value)
{
	tree_property(tree, name, value, (uint32_t)strlen(value) + 1U);
}

/* Adds a DesignWare UART node. */
static void
tree_uart(
	struct tree_builder *tree,
	const char *name,
	uint32_t base,
	int interrupt)
{
	uint32_t reg[4];
	uint32_t cell;

	/* Its kind, registers, spacing and width. */
	tree_begin(tree, name);
	tree_string(tree, "compatible", "snps,dw-apb-uart");
	reg[0] = 0U;
	reg[1] = base;
	reg[2] = 0U;
	reg[3] = 0x10000U;
	tree_cells(tree, "reg", reg, 4U);
	cell = 2U;
	tree_cells(tree, "reg-shift", &cell, 1U);
	cell = 4U;
	tree_cells(tree, "reg-io-width", &cell, 1U);

	/* Its interrupt, when it has one. */
	if (interrupt != 0) {
		cell = (uint32_t)interrupt;
		tree_cells(tree, "interrupts", &cell, 1U);
	}
	tree_string(tree, "status", "okay");
	tree_end(tree);
}

/*
 * Builds the test's tree: a UART that is not the console first, then the
 * console's at 0x10000000, which /chosen/stdout-path names as the scene
 * asks, and opens it.
 */
static void
tree_build(
	enum tree_console console,
	struct drv_fdt *fdt)
{
	static struct tree_builder tree;
	uint32_t cells;
	uint32_t total;
	uint32_t header[10];
	unsigned index;
	int error;

	/* The root, with two-cell addresses and sizes. */
	memset(&tree, 0, sizeof(tree));
	tree_begin(&tree, "");
	cells = 2U;
	tree_cells(&tree, "#address-cells", &cells, 1U);
	tree_cells(&tree, "#size-cells", &cells, 1U);

	/* The console's name. */
	tree_begin(&tree, "chosen");
	if (console == TREE_ALIAS)
		tree_string(&tree, "stdout-path", "serial0:115200n8");
	if (console == TREE_PATH || console == TREE_NO_INTERRUPT)
		tree_string(&tree, "stdout-path", "/soc/serial@10000000:115200n8");
	tree_end(&tree);
	tree_begin(&tree, "aliases");
	tree_string(&tree, "serial1", "/soc/serial@10010000");
	tree_string(&tree, "serial0", "/soc/serial@10000000");
	tree_end(&tree);

	/* The bus with the two UARTs, the console's second. */
	tree_begin(&tree, "soc");
	tree_cells(&tree, "#address-cells", &cells, 1U);
	tree_cells(&tree, "#size-cells", &cells, 1U);
	tree_property(&tree, "ranges", "", 0U);
	tree_uart(&tree, "serial@10010000", 0x10010000U, 33);
	if (console == TREE_NO_INTERRUPT) {
		tree_uart(&tree, "serial@10000000", 0x10000000U, 0);
	} else {
		tree_uart(&tree, "serial@10000000", 0x10000000U, 32);
	}
	tree_end(&tree);
	tree_end(&tree);
	tree_word(&tree, 9U);

	/* The header, the structure block and the strings block. */
	total = 40U + tree.structure_size + tree.strings_size;
	header[0] = 0xd00dfeedU;
	header[1] = total;
	header[2] = 40U;
	header[3] = 40U + tree.structure_size;
	header[4] = 40U;
	header[5] = 17U;
	header[6] = 16U;
	header[7] = 0U;
	header[8] = tree.strings_size;
	header[9] = tree.structure_size;
	memset(tree_blob, 0, sizeof(tree_blob));
	for (index = 0U; index < 10U; index++) {
		tree_blob[index * 4U + 0U] = (uint8_t)(header[index] >> 24);
		tree_blob[index * 4U + 1U] = (uint8_t)(header[index] >> 16);
		tree_blob[index * 4U + 2U] = (uint8_t)(header[index] >> 8);
		tree_blob[index * 4U + 3U] = (uint8_t)header[index];
	}
	memcpy(tree_blob + 40U, tree.structure, tree.structure_size);
	memcpy(tree_blob + 40U + tree.structure_size, tree.strings, tree.strings_size);

	/* Opened as the board's code opens it. */
	error = drv_fdt_open(fdt, tree_blob, total);
	expect(error == 0, "tree: the test's tree opens");
	fdt_range[0] = (uint64_t)(uintptr_t)tree_blob;
	fdt_range[1] = total;
}

/* Starts the VisionFive 2's binding over the test's tree, with a display a fatal stop takes back. */
static void
vf2_start(
	enum tree_console console)
{
	static struct kern_boot_device devices[1];
	struct kern_boot_handoff handoff;
	struct drv_fdt fdt;

	/* The tree the hardware layer hands over. */
	tree_build(console, &fdt);

	/* The common record, as the loader leaves it. */
	memset(&handoff, 0, sizeof(handoff));
	handoff.magic = KERN_HANDOFF_MAGIC;
	handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	handoff.size = sizeof(handoff);
	(void)kern_platform_init(&handoff, devices, 1U);

	/* What a fatal stop calls to take the display back. */
	vf2_test_set_fatal_display(fatal_display);
	hal_drawn = 0U;
	sent_reset();
}

/* Counts a fatal stop's taking back of the display. */
static void
fatal_display(void)
{
	fatal_displays++;
}

/* A character path that is not the board's. */
static void
other_putc(
	int character)
{
	(void)character;
}

/* Reports whether the UART received exactly text. */
static int
sent_is(
	const char *text)
{
	size_t length;

	/* The same bytes, no more. */
	length = strlen(text);
	if (uart_sent_count != length)
		return 0;
	if (memcmp(uart_sent, text, length) != 0)
		return 0;
	return 1;
}

/* Forgets what the UART received. */
static void
sent_reset(void)
{
	uart_sent_count = 0U;
	memset(uart_sent, 0, sizeof(uart_sent));
}

#ifndef TEXT_END_NO_PC98
/* Sums the PC-98 text memory's bytes with their places, so that any change shows. */
static uint64_t
pc98_text_sum(void)
{
	const volatile uint8_t *memory;
	uint64_t sum;
	size_t index;

	/* Every byte of both planes, weighted by its index. */
	memory = (const volatile uint8_t *)PC98_TEXT_ADDRESS;
	sum = 0U;
	for (index = 0U; index < PC98_TEXT_BYTES; index++)
		sum += (uint64_t)memory[index] * (index + 1U);
	return sum;
}

/* Maps the PC-98 text memory and brings the board's display up, registering /dev/graphics when asked. */
static void
pc98_start(
	int register_device)
{
	void *memory;
	int prepared;
	int error;

	/* The text memory at its fixed kernel address. */
	memory = mmap((void *)PC98_TEXT_ADDRESS, PC98_TEXT_BYTES, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (memory != (void *)PC98_TEXT_ADDRESS) {
		printf("FAIL pc98: text memory cannot be mapped at its kernel address\n");
		exit(1);
	}

	/* The planes and the console. */
	prepared = drv_pc98_graphics_prepare();
	expect(prepared == 1 && mapping_count == 5U, "pc98: the planes and the Cirrus aperture are mapped");

	/* /dev/graphics, when the scene wants it registered. */
	if (register_device) {
		error = drv_graphics_device_register();
		expect(error == 0 && graphics_cdev != NULL, "pc98: /dev/graphics registers");
	}
}
#endif
