/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's character-output face of the display device.
 *
 * /dev/graphics owns the display on every board, including the ability to
 * put characters on it. What that means in hardware differs: a linear
 * framebuffer draws glyphs as pixels, VGA text memory takes a cell word,
 * and the PC-98 keeps characters and attributes in two separate planes.
 * /dev/console needs none of that. It calls through this table, so it
 * stays one platform-independent multiplexer over the display and evdev.
 *
 * The board's display driver registers one table during its own bring-up.
 * Until it does, kern_text_ready() reports zero and every call here is a
 * no-op, which is the correct behaviour on a board with no display.
 */

#ifndef KERN_TEXT_DISPLAY_H
#define KERN_TEXT_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

struct spinlock;
struct wait_queue;

/*
 * A caller-owned notification link for retained text changes.
 *
 * The condition lock and wait queue remain live until unobserve returns.
 * The lock is a leaf above LOCK_RANK_CONSOLE_TEXT and below the scheduler.
 * Register and unregister without holding that lock. Notifications take it
 * with interrupts disabled and only wake the queue, never call a driver.
 */
struct kern_text_observer {
	struct kern_text_observer *next;
	struct spinlock *lock;
	struct wait_queue *queue;
};

/* Light grey on black, the default text attribute on every board. */
#define KERN_TEXT_ATTRIB_NORMAL	0x07U

/*
 * One caller-owned RAM destination for a retained text-grid snapshot.
 *
 * Null pixels with zero bytes queries geometry. Successful rendering writes
 * tightly packed BGRA8888 pixels with zero alpha; stride is reported in bytes.
 * The destination remains caller-owned and no framebuffer MMIO is read.
 */
struct kern_text_snapshot {
	uint32_t *pixels;
	size_t bytes;
	unsigned width;
	unsigned height;
	unsigned stride;
};

/*
 * One range of processor physical addresses a display driver decodes, which
 * it may give to others once the console has left the display.
 */
struct kern_text_range {
	uint64_t physical;
	uint64_t bytes;
};

/* The most ranges one call to kern_text_end() takes. */
#define KERN_TEXT_END_RANGES	4U

/*
 * Reports whether [physical, physical + bytes) meets one of the count
 * ranges.  Every range and the one asked about are non-empty and do not
 * wrap; kern_text_end() refuses a list that breaks this.
 */
static inline int
kern_text_range_overlaps(
	const struct kern_text_range *ranges,
	unsigned count,
	uint64_t physical,
	uint64_t bytes)
{
	unsigned index;

	/* Each range against the one asked about. */
	for (index = 0U; index < count; index++) {
		/* Two half-open ranges meet when each begins before the other ends. */
		if (physical < ranges[index].physical + ranges[index].bytes &&
		    ranges[index].physical < physical + bytes)
			return 1;
	}

	/* None meets it. */
	return 0;
}

/*
 * One board's character output.
 *
 * Every entry before snapshot is required. The display driver holds the cell state; this
 * table is only the way in.
 */
struct kern_text_ops {
	/* Reports the grid size in character cells. */
	void (*get_size)(unsigned *columns, unsigned *rows);

	/* Writes one character at the cursor, scrolling as needed. */
	void (*putc)(int character);

	/* Writes a terminated string at one cell with one attribute. */
	void (*write)(unsigned row, unsigned column, uint8_t attribute,
		      const char *utf8);

	/* Blanks the grid and homes the cursor. */
	void (*clear)(void);

	/* Moves the cursor; returns zero when the position is outside. */
	int (*set_cursor)(unsigned row, unsigned column);

	/* Reports the cursor position and whether it is shown. */
	void (*get_cursor)(unsigned *row, unsigned *column, int *visible);

	/* Shows or hides the cursor. */
	void (*show_cursor)(int visible);

	/* Repaints the cursor after a stream of writes. */
	void (*update_cursor)(void);

	/* Stops and restarts output while a graphics mode owns the screen. */
	void (*suspend)(void);
	void (*resume)(void);

	/* Optionally renders retained text into an independent caller-owned RAM image. */
	int (*snapshot)(struct kern_text_snapshot *snapshot);

	/*
	 * Optionally shows a console that has been kept off the screen (a quiet
	 * boot, ws035-p097): the screen is cleared and the retained text drawn.
	 */
	void (*reveal)(void);

	/*
	 * Optionally shows the console for a fatal stop (BUG-158): like reveal,
	 * but also over a graphics mode that suspended the text, and without
	 * waiting on a lock this CPU may already hold.  Interrupts are disabled.
	 */
	void (*reveal_fatal)(void);

	/*
	 * Optionally shows that a quiet boot goes on: a record went to the log
	 * only (end 0: the boot splash's spinner turns), or the
	 * boot's screen is over (end 1: a display was taken for graphics, and
	 * the splash stops for good).
	 */
	void (*progress)(int end);

	/*
	 * Optionally ends the console's use of the display for good.  After it
	 * returns 0 or EBUSY nothing of the console (its text, a quiet boot's
	 * splash, a graphics mode the board's /dev/graphics entered, the
	 * reveal of a fatal stop, the hardware layer's early console) draws
	 * on the display again, and every mapping of the display memory made
	 * for the console or handed over by the hardware layer that can be
	 * removed is removed; the console's characters still reach its other
	 * outputs (a serial line, the log), and its cells keep following them.
	 * Returns 0 when nothing left (a mapping that could not be removed, a
	 * writer that could not be stopped) lies in one of the count ranges
	 * (none with count 0); EBUSY when something left does; the error of a
	 * removal that failed for a mapping in the ranges; or ENOTSUP, changing
	 * nothing, when the board cannot stop drawing.  A later call returns
	 * the first result, judged again against its own ranges.  Called in
	 * thread context with no lock held; it may sleep.
	 */
	int (*end)(const struct kern_text_range *ranges, unsigned count);
};

/*
 * Publish one board's character output.
 *
 * Called once, from the display driver's bring-up. A second call replaces
 * the first, which is how a board that gains a better display later can
 * hand over.
 */
void kern_text_register(const struct kern_text_ops *ops);

/* Reports whether a board has published character output. */
int kern_text_ready(void);

/* The calls /dev/console makes. Each is a no-op with no display. */
void kern_text_get_size(unsigned *columns, unsigned *rows);
void kern_text_putc(int character);
void kern_text_write(unsigned row, unsigned column, uint8_t attribute,
		     const char *utf8);
void kern_text_clear(void);
int kern_text_set_cursor(unsigned row, unsigned column);
void kern_text_get_cursor(unsigned *row, unsigned *column, int *visible);
void kern_text_show_cursor(int visible);
void kern_text_update_cursor(void);
void kern_text_suspend(void);
void kern_text_resume(void);

/*
 * The quiet console (ws035-p097): reveal shows it and makes the log loud;
 * kernel_putc is the kernel's own character path (the HAL's lines, a fatal
 * error), which goes to the log while the console is quiet.
 */
void kern_text_reveal(void);
void kern_text_reveal_fatal(void);
void kern_text_kernel_putc(int character);
void kern_text_progress(void);
void kern_text_progress_end(void);

/*
 * The kernel's console output, or NULL until a console draws it.
 *
 * A console driver publishes its character routine here with a release
 * store once it can draw, and reads it with an acquire load to see whether
 * another path was published first. Once it is set, the early console
 * hands every character to it. It is called from interrupt context and
 * from a fatal error, so the routine must not sleep and must take only
 * interrupt-safe locks.
 */
extern void (*kernel_putc)(int c);

/*
 * Ends the console's use of the display for good, for a display driver
 * that flipped its first image to the display or is about to give the
 * memory behind ranges (the processor physical ranges the driver's device
 * decodes, count of them; count 0, with ranges NULL, gives none) to
 * others.  The console stops drawing on the display whether or not it
 * lies in the ranges; its serial output goes on.
 *
 * Returns 0 once nothing of the console draws on the display and nothing
 * left of it can write to or keeps a mapping of memory in the ranges;
 * EBUSY when the console stopped but a mapping or writer it could not
 * remove lies in the ranges; ENOTSUP, changing nothing, when no console is
 * published or the board's console cannot stop; EINVAL, changing nothing,
 * for an oversized list or an empty or wrapping range; or the error of a
 * removal that failed for a mapping in the ranges.  The caller holds no
 * lock; the call may sleep.  There is no way back.
 */
int kern_text_end(const struct kern_text_range *ranges, unsigned count);

/* Renders or queries an optional retained-cell snapshot, without reading display memory. */
int kern_text_snapshot(struct kern_text_snapshot *snapshot);

/* Changes after completed text mutations; consumers compare successive observations. */
uint32_t kern_text_generation(void);

/*
 * Subscribes an initialized caller-owned condition queue to text mutations.
 * Read the generation after registration to include earlier text changes.
 */
int
kern_text_observe(
	struct kern_text_observer *observer,
	struct spinlock *lock,
	struct wait_queue *queue);

/*
 * Removes a subscription and waits for every in-flight wake to finish.
 * Repeated removal is harmless, and the caller may then free the link.
 */
void
kern_text_unobserve(
	struct kern_text_observer *observer);

#endif
