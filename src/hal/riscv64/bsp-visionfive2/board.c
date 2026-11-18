/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The StarFive VisionFive 2 board (JH7110).
 *
 * The shared L2 cache controller is the one board device the hardware
 * layer drives: cache maintenance writes each line's physical address to
 * its flush register, which writes the line back and discards it.
 *
 * The board's display is the one the firmware left on, when the UEFI
 * loader handed its graphics output over.  The early console draws on it
 * through the direct map, cached, and flushes each range it drew, since
 * the display controller is not known to see the processor's cache.  A
 * screen the direct map leaves out (no-map RAM, or device memory) is drawn
 * through the device window instead, RAM still flushed.  Once the kernel's
 * tables are in use, the console draws through its own window of the
 * screen (space.c), which the kernel's console takes over and removes.
 * The board names the screen to the kernel as "riscv64.framebuffer.*".
 */

#include <hal/hal.h>
#include "../bootinfo.h"
#include "../bsp.h"
#include "../defs.h"
#include "../fbcons.h"
#include "../page.h"
#include "../space.h"

/*
 * The offset of the cache controller's register that flushes one line by
 * its physical address, and the controller's line size in bytes.
 *
 * Values referenced: OpenBSD sys/arch/riscv64/dev/sfcc.c (SFCC_FLUSH64
 * 0x200) and Linux v6.12 arch/riscv/boot/dts/starfive/jh7110.dtsi (the
 * cache-controller node: cache-block-size 64).
 */
#define VF2_CCACHE_FLUSH64		0x200U
#define VF2_CCACHE_LINE_SIZE		64U

/* The register's end: the controller's range must reach past it. */
#define VF2_CCACHE_FLUSH64_END		(VF2_CCACHE_FLUSH64 + 8U)

/*
 * The distance from the memory port's DRAM to the same DRAM seen through
 * the system port, which the processor reaches without caching.
 *
 * Values referenced: StarFive JH7110 TRM Preliminary V2, Table 2-4
 * System Memory Map (DDR for memory port at 0x00_4000_0000, DDR for
 * system port at 0x04_4000_0000).
 */
#define VF2_UNCACHED_ALIAS_OFFSET	0x400000000ULL

/*
 * The boot parameter that has the early console draw on the screen without
 * flushing, to find out whether the display sees the processor's cache.
 */
#define VF2_SCREEN_NOCACHE_PARAMETER	"vf2.fb=nocache"

/*
 * How the early console reaches the screen: RAM through the direct map,
 * RAM the direct map leaves out through the device window, either without
 * the flush when the boot asked for that, or device memory.
 */
enum board_screen_window {
	BOARD_SCREEN_NONE = 0,
	BOARD_SCREEN_RAM_DIRECT,
	BOARD_SCREEN_RAM_WINDOW,
	BOARD_SCREEN_RAM_UNFLUSHED,
	BOARD_SCREEN_DEVICE
};

/*
 * The cache controller's flush register in the device window, or NULL
 * when the device tree names no controller.  Set once by
 * riscv64_board_init() on the boot hart before any other hart runs, and
 * read only afterwards.
 */
static volatile uint64_t *board_flush64;

/*
 * The controller's physical base, kept for the boot log; zero when there
 * is no controller.  Set with board_flush64.
 */
static uint64_t board_ccache_base;

/*
 * The physical address of the screen's memory, the first of the screen's
 * named values.  Each is a 64-bit word as hal_get_arch_handoff() hands it
 * over; all six are filled once by riscv64_board_init() on the boot hart
 * when the loader handed a screen over, and not changed afterwards.
 */
static uint64_t board_framebuffer_phys;

/* The size of the screen's memory in bytes, a named value. */
static uint64_t board_framebuffer_size;

/* The screen's width in pixels, a named value. */
static uint64_t board_framebuffer_width;

/* The screen's height in pixels, a named value. */
static uint64_t board_framebuffer_height;

/* The bytes from one row of the screen to the next, a named value. */
static uint64_t board_framebuffer_pitch;

/* The screen's KERN_BOOT_FRAMEBUFFER_* format, a named value. */
static uint64_t board_framebuffer_format;

/* Nonzero once the six named values above are filled; set with them. */
static int board_screen_named;

/*
 * How the early console reaches the screen.  Set with the named values,
 * for the boot log.
 */
static enum board_screen_window board_screen_window;

/*
 * Why the early console does not reach the screen, or HAL_OK when it
 * does.  Set with the named values, for the boot log.
 */
static int board_screen_error = HAL_ERR_UNSUPPORTED;

/*
 * The kernel address the console draws the screen's first byte at, which
 * the screen's flush turns back into physical addresses.  Set by
 * show_screen() before the console is attached, and once more by
 * riscv64_board_screen_window() on the boot hart before any other runs.
 */
static uintptr_t board_screen_address;

static void find_ccache(const struct riscv64_fdt_info *info);
static void show_screen(void);
static int screen_address(const struct riscv64_framebuffer *framebuffer, uintptr_t *address, enum board_screen_window *window);
static void flush_screen(uintptr_t address, size_t size);
static void flush_range(uintptr_t address, size_t size);
static void flush_physical(uint64_t physical, uint64_t size);
static int kernel_to_physical(uintptr_t address, uint64_t *physical);
static void fence_all(void);

/*
 * Finds the board's cache controller and shows the screen the firmware
 * left on.
 *
 * The cache controller comes first: the screen is flushed through it.
 */
void
riscv64_board_init(
	const struct riscv64_fdt_info *info)
{
	/* Finds the flush register, which drawing on the screen needs. */
	find_ccache(info);

	/* Names the screen and draws the early console on it. */
	show_screen();
}

/*
 * Answers the board's own handoff names: the cache controller's base as
 * "riscv64.ccache.base" (NULL when it was not found, so cache maintenance
 * does nothing), and the screen the firmware left on, as six 64-bit words
 * under "riscv64.framebuffer.".
 *
 * Every screen name is NULL when the loader handed no screen over.
 */
void *
riscv64_board_handoff(
	const char *name)
{
	bool match;

	/* The cache controller, when one was found; the kernel logs it. */
	match = riscv64_boot_name_is(name, "riscv64.ccache.base");
	if (match) {
		if (board_flush64 == NULL)
			return NULL;
		return &board_ccache_base;
	}

	/* A board without a screen names none of it. */
	if (!board_screen_named)
		return NULL;

	/* The physical address of the screen's memory. */
	match = riscv64_boot_name_is(name, "riscv64.framebuffer.phys");
	if (match)
		return &board_framebuffer_phys;

	/* The bytes of the screen's memory. */
	match = riscv64_boot_name_is(name, "riscv64.framebuffer.size");
	if (match)
		return &board_framebuffer_size;

	/* The width in pixels. */
	match = riscv64_boot_name_is(name, "riscv64.framebuffer.width");
	if (match)
		return &board_framebuffer_width;

	/* The height in pixels. */
	match = riscv64_boot_name_is(name, "riscv64.framebuffer.height");
	if (match)
		return &board_framebuffer_height;

	/* The bytes from one row to the next. */
	match = riscv64_boot_name_is(name, "riscv64.framebuffer.pitch");
	if (match)
		return &board_framebuffer_pitch;

	/* The pixel format, a KERN_BOOT_FRAMEBUFFER_* code. */
	match = riscv64_boot_name_is(name, "riscv64.framebuffer.format");
	if (match)
		return &board_framebuffer_format;

	/* An unknown handoff. */
	return NULL;
}

/*
 * Reports the screen the early console draws on, the loader's.
 */
int
riscv64_board_screen(
	uint64_t *physical,
	uint64_t *size)
{
	/* A screen the console does not draw on has no window to hold. */
	if (board_screen_window == BOARD_SCREEN_NONE)
		return 0;

	/* Succeeded: the loader's screen. */
	*physical = board_framebuffer_phys;
	*size = board_framebuffer_size;
	return 1;
}

/*
 * Has the early console draw the screen through its window, flushed by
 * the window's addresses from now on.
 */
void
riscv64_board_screen_window(
	uintptr_t address)
{
	/* The flush translates the window's addresses, and the console draws there. */
	board_screen_address = address;
	riscv64_fbcons_move(address);
}

/*
 * Writes the board's cache controller and screen to the boot log.
 */
void
riscv64_board_describe(
	void)
{
	/* Names the board and its cache controller, or says there is none. */
	if (board_flush64 != NULL) {
		hal_printf("RISCV64 BOARD visionfive2 ccache=%lx\n", board_ccache_base);
	} else {
		hal_printf("RISCV64 BOARD visionfive2 ccache unavailable; no cache maintenance\n");
	}

	/* Names how the console reaches the screen, or why there is none. */
	switch (board_screen_window) {
	case BOARD_SCREEN_RAM_DIRECT:
		hal_printf("RISCV64 FRAMEBUFFER %lux%lu phys=%lx ram flush\n",
			   board_framebuffer_width,
			   board_framebuffer_height,
			   board_framebuffer_phys);
		break;
	case BOARD_SCREEN_RAM_WINDOW:
		hal_printf("RISCV64 FRAMEBUFFER %lux%lu phys=%lx ram-nomap flush\n",
			   board_framebuffer_width,
			   board_framebuffer_height,
			   board_framebuffer_phys);
		break;
	case BOARD_SCREEN_RAM_UNFLUSHED:
		hal_printf("RISCV64 FRAMEBUFFER %lux%lu phys=%lx ram nocache\n",
			   board_framebuffer_width,
			   board_framebuffer_height,
			   board_framebuffer_phys);
		break;
	case BOARD_SCREEN_DEVICE:
		hal_printf("RISCV64 FRAMEBUFFER %lux%lu phys=%lx device\n",
			   board_framebuffer_width,
			   board_framebuffer_height,
			   board_framebuffer_phys);
		break;
	case BOARD_SCREEN_NONE:
		hal_printf("RISCV64 FRAMEBUFFER unavailable (%u); firmware console only\n",
			   (uint32_t)board_screen_error);
		break;
	}
}

/*
 * Writes cached lines of a range back to memory.
 *
 * The controller's flush also discards the lines, which is harmless for a
 * clean.
 */
void
hal_dcache_clean_range(
	uintptr_t address,
	size_t size)
{
	/* Writes back and discards every line of the range. */
	flush_range(address, size);
}

/*
 * Discards cached lines of a range.
 *
 * The controller writes a dirty line back before it discards it, so bytes
 * sharing an edge line with the range are kept.
 */
void
hal_dcache_invalidate_range(
	uintptr_t address,
	size_t size)
{
	/* Writes back and discards every line of the range. */
	flush_range(address, size);
}

/*
 * Writes back and discards cached lines of a range.
 */
void
hal_dcache_clean_invalidate_range(
	uintptr_t address,
	size_t size)
{
	/* Writes back and discards every line of the range. */
	flush_range(address, size);
}

/*
 * Reports the distance from RAM to its uncached alias.
 *
 * The alias is offered only with the cache controller: a view that is
 * kept apart from the caches needs the lines written back and discarded
 * around it, which only the controller can do.
 */
uint64_t
riscv64_board_uncached_offset(
	void)
{
	/* Without the controller the alias cannot be kept coherent. */
	if (board_flush64 == NULL)
		return 0;

	/* Succeeded: the system port's alias. */
	return VF2_UNCACHED_ALIAS_OFFSET;
}

/*
 * Writes back and discards every cached line of a physical range.
 */
void
riscv64_board_flush_physical(
	uint64_t physical,
	uint64_t size)
{
	/* Each line through the controller's flush register. */
	flush_physical(physical, size);
}

/*
 * Finds the board's cache controller.
 *
 * A tree without one, or with a range too small for the flush register,
 * leaves cache maintenance doing nothing: the board's devices are then
 * taken to snoop the caches.
 */
static void
find_ccache(
	const struct riscv64_fdt_info *info)
{
	/* Leaves maintenance off for a tree without the controller. */
	if (info->ccache_size < VF2_CCACHE_FLUSH64_END)
		return;

	/* Refuses a controller the device window does not reach. */
	if (info->ccache_base >= RISCV64_DEVICE_LIMIT - VF2_CCACHE_FLUSH64_END)
		return;

	/* Reaches the flush register through the device window. */
	board_ccache_base = info->ccache_base;
	board_flush64 = (volatile uint64_t *)(uintptr_t)(RISCV64_DEVICE_BASE + info->ccache_base + VF2_CCACHE_FLUSH64);
}

/*
 * Names the screen the loader handed over and attaches the early console
 * to it.
 *
 * The names stand whether or not the console can draw there: they
 * describe the firmware's screen, which the kernel may take over.
 */
static void
show_screen(
	void)
{
	const struct riscv64_framebuffer *framebuffer;
	struct riscv64_fbcons_screen screen;
	enum board_screen_window window;
	uintptr_t address;
	bool unflushed;
	int error;

	/* A boot without the loader's screen keeps the firmware's console alone. */
	framebuffer = riscv64_boot_framebuffer();
	if (framebuffer == NULL)
		return;

	/* Names the screen to the kernel. */
	board_framebuffer_phys = framebuffer->phys;
	board_framebuffer_size = framebuffer->size;
	board_framebuffer_width = framebuffer->width;
	board_framebuffer_height = framebuffer->height;
	board_framebuffer_pitch = framebuffer->pitch;
	board_framebuffer_format = framebuffer->format;
	board_screen_named = 1;

	/* Finds where the console reaches the screen's memory. */
	error = screen_address(framebuffer, &address, &window);
	if (error != HAL_OK) {
		board_screen_error = error;
		return;
	}

	/* Describes the screen to the console, keeping its address for the flush. */
	board_screen_address = address;
	screen.address = address;
	screen.width = framebuffer->width;
	screen.height = framebuffer->height;
	screen.pitch = framebuffer->pitch;
	screen.format = framebuffer->format;
	screen.flush = NULL;

	/* Writes RAM through the cache controller, unless the boot asked to see the screen without. */
	unflushed = riscv64_boot_parameter_given(VF2_SCREEN_NOCACHE_PARAMETER);
	if (window == BOARD_SCREEN_DEVICE) {
		/* Device memory is not cached, so there is nothing to write through. */
		screen.flush = NULL;
	} else if (unflushed) {
		/* The boot asked to see whether the display sees the cache. */
		window = BOARD_SCREEN_RAM_UNFLUSHED;
	} else {
		/* RAM in either window is cached and written through after each drawing. */
		screen.flush = flush_screen;
	}

	/* Attaches the console, which clears the screen and draws its text. */
	error = riscv64_fbcons_attach(&screen);
	board_screen_error = error;
	if (error != HAL_OK)
		return;

	/* Succeeded: the console draws on the screen. */
	board_screen_window = window;
}

/*
 * Finds the kernel address the console draws a screen's memory at: the
 * direct map when it holds the whole screen, otherwise the device window,
 * which the hardware layer maps the screen into (space.c,
 * map_hal_devices).
 *
 * Returns HAL_OK, or HAL_ERR_UNSUPPORTED for memory neither reaches.
 */
static int
screen_address(
	const struct riscv64_framebuffer *framebuffer,
	uintptr_t *address,
	enum board_screen_window *window)
{
	bool direct;
	bool in_ram;

	/* RAM the direct map holds is drawn there, the same cached memory the allocator would hand out. */
	direct = riscv64_space_in_direct_map(framebuffer->phys, framebuffer->size);
	if (direct) {
		*address = (uintptr_t)(RISCV64_DIRECT_BASE + framebuffer->phys);
		*window = BOARD_SCREEN_RAM_DIRECT;
		return HAL_OK;
	}

	/* Refuses memory above what the device window holds. */
	if (framebuffer->phys >= RISCV64_DEVICE_LIMIT ||
	    framebuffer->size > RISCV64_DEVICE_LIMIT - framebuffer->phys)
		return HAL_ERR_UNSUPPORTED;

	/* Anything else is reached through the device window; RAM there is still cached. */
	*address = (uintptr_t)(RISCV64_DEVICE_BASE + framebuffer->phys);
	*window = BOARD_SCREEN_DEVICE;
	in_ram = riscv64_range_touches_ram(framebuffer->phys, framebuffer->size);
	if (in_ram)
		*window = BOARD_SCREEN_RAM_WINDOW;

	/* Succeeded: the screen's memory in the device window. */
	return HAL_OK;
}

/*
 * Writes a drawn range of the screen through the cache controller to the
 * display, from the screen's own kernel address, which may be in the
 * device window where the general translation does not look.
 */
static void
flush_screen(
	uintptr_t address,
	size_t size)
{
	uint64_t physical;

	/* Refuses a range that does not start inside the screen. */
	if (address < board_screen_address || address - board_screen_address >= board_framebuffer_size)
		return;

	/* The screen is one run of physical memory from its first byte. */
	physical = board_framebuffer_phys + (uint64_t)(address - board_screen_address);
	flush_physical(physical, size);
}

/*
 * Flushes every cache-controller line a kernel range touches.
 *
 * Procedure referenced: OpenBSD sys/arch/riscv64/dev/sfcc.c
 * (sfcc_cache_wbinv_range): a full fence, then each line's physical
 * address written to the flush register with a full fence after each.
 */
static void
flush_range(
	uintptr_t address,
	size_t size)
{
	uintptr_t line;
	uintptr_t end;
	uint64_t physical;
	int error;

	/* A board without the controller has nothing to flush. */
	if (board_flush64 == NULL || size == 0)
		return;

	/* Refuses a range that wraps the address space. */
	if (address > UINTPTR_MAX - size)
		return;

	/* Starts at the line that holds the first byte. */
	line = address & ~((uintptr_t)VF2_CCACHE_LINE_SIZE - 1U);
	end = address + size;

	/* Orders the earlier memory accesses before the first flush. */
	fence_all();

	/* Flushes each line that has a physical address the kernel knows. */
	while (line < end) {
		error = kernel_to_physical(line, &physical);
		if (error == 0) {
			hal_mmio_write64(board_flush64, physical);
			fence_all();
		}

		/* Stops at the top of the address space. */
		if (line > UINTPTR_MAX - VF2_CCACHE_LINE_SIZE)
			break;
		line += VF2_CCACHE_LINE_SIZE;
	}
}

/*
 * Flushes every cache-controller line of a physical range.
 *
 * Procedure referenced: OpenBSD sys/arch/riscv64/dev/sfcc.c
 * (sfcc_cache_wbinv_range): a full fence, then each line's physical
 * address written to the flush register with a full fence after each.
 */
static void
flush_physical(
	uint64_t physical,
	uint64_t size)
{
	uint64_t line;
	uint64_t end;

	/* A board without the controller has nothing to flush. */
	if (board_flush64 == NULL || size == 0)
		return;

	/* Refuses a range that wraps the address space. */
	if (physical > ~0ULL - size)
		return;

	/* Starts at the line that holds the first byte. */
	line = physical & ~((uint64_t)VF2_CCACHE_LINE_SIZE - 1U);
	end = physical + size;

	/* Orders the earlier memory accesses before the first flush. */
	fence_all();

	/* Flushes each line. */
	while (line < end) {
		hal_mmio_write64(board_flush64, line);
		fence_all();

		/* Stops at the top of the address space. */
		if (line > ~0ULL - VF2_CCACHE_LINE_SIZE)
			break;
		line += VF2_CCACHE_LINE_SIZE;
	}
}

/*
 * Translates an address in the kernel image or in the direct map to its
 * physical address.
 *
 * Returns HAL_OK, or HAL_ERR_INVALID for any other address (the device
 * window holds no cacheable memory).
 */
static int
kernel_to_physical(
	uintptr_t address,
	uint64_t *physical)
{
	/* An address in the image's window is an offset from the load address. */
	if (address >= (uintptr_t)RISCV64_KERNEL_VIRT_BASE) {
		*physical = (uint64_t)(address - (uintptr_t)RISCV64_KERNEL_VIRT_BASE) + riscv64_kernel_phys_start();
		return HAL_OK;
	}

	/* Refuses an address below the direct map. */
	if (address < (uintptr_t)RISCV64_DIRECT_BASE)
		return HAL_ERR_INVALID;

	/* Refuses an address above the direct map. */
	if (address - (uintptr_t)RISCV64_DIRECT_BASE >= RISCV64_DIRECT_LIMIT)
		return HAL_ERR_INVALID;

	/* Succeeded: the direct map is a fixed offset. */
	*physical = (uint64_t)(address - (uintptr_t)RISCV64_DIRECT_BASE);
	return HAL_OK;
}

/* Orders every earlier memory and device access before every later one. */
static void
fence_all(
	void)
{
	/* A fence over device input and output and memory reads and writes. */
	__asm__ volatile("fence iorw, iorw" : : : "memory");
}
