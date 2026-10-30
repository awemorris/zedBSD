/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arm64 stall watch (ws208-p007).
 *
 * A Raspberry Pi 4 that stops with several CPUs has no serial console and
 * a screen that a display driver owns, so a CPU that stops leaves no trace
 * anyone can read.  The watch writes a description of the stop into the
 * image the display is showing, where a photograph of the screen reads it,
 * and keeps the same text in memory for a debugger (arm64_watch_text).
 *
 * Every CPU keeps a heartbeat: its scheduling tick stamps the time in its
 * record.  Each tick that falls on a multiple of the check interval looks
 * at the other CPUs; one whose heartbeat is older than the threshold has
 * stopped taking interrupts, and is described by what its record says:
 * since when and where its interrupts went off, which interrupt it was
 * serving, and how many task switches it has made.  A CPU that spins with
 * its interrupts off notices the same of itself from hal_cpu_current(),
 * which every kernel spin lock calls on each try, and from the HAL's own
 * spin loops; it adds its call chain to the description.  hal_fatal()
 * describes its own stop at once.
 *
 * What it cannot see: when every CPU stops with its interrupts off in a
 * loop that calls neither hal_cpu_current() nor a HAL spin lock, nobody
 * is left to write.  GIC-400's FIQ would be the way out, but under the
 * firmware the non-secure kernel cannot claim Group 0, and the BCM2711's
 * watchdog only resets the board, which loses the screen.
 *
 * The text goes to the planes of the display list the compositor (HVS)
 * scans out when a display driver owns the screen, and to the firmware's
 * framebuffer through the early console's cell painter otherwise.  A stop
 * that lasts long enough also replaces the display list with one plane,
 * so that a window above the text cannot hide it.  Once the kernel's
 * console has given the firmware's framebuffer up, the text goes nowhere
 * near it: a plane on it is passed over and the cell painter paints
 * nothing.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "percpu.h"
#include "space.h"
#include "watch.h"
#include "bsp-rpi-common/font.h"
#include "bsp-rpi-common/framebuffer.h"

/* How long a CPU may go without its tick before it counts as stopped, in seconds. */
#define WATCH_STALL_SECONDS 2U

/* How long a stop lasts before the display list is replaced, in seconds. */
#define WATCH_TAKEOVER_SECONDS 10U

/* How often, in ticks, a CPU looks at the others' heartbeats. */
#define WATCH_CHECK_TICKS 128U

/* The report: lines of text, each at most this wide. */
#define WATCH_LINES 40U
#define WATCH_COLUMNS 120U

/* How many lines of the kernel's log the report ends with. */
#define WATCH_LOG_LINES 12U

/* The bytes of the kernel log's tail kept from hal_putc(); a power of two. */
#define WATCH_TAIL_BYTES 4096U

/* How many return addresses a CPU records of its own call chain. */
#define WATCH_CHAIN_DEPTH 6U

/*
 * The BCM2711 compositor (HVS) the board maps (arm64_bsp_watch_compositor()):
 * its display-list register, its display-list memory and its size in words.
 */
#define WATCH_HVS_DISPLIST0 0x20U
#define WATCH_HVS_DLIST 0x4000U
#define WATCH_HVS_DLIST_WORDS 2048U

/* Where the watch's own one-plane list goes: above every list the display driver uses. */
#define WATCH_HVS_OWN_LIST 1984U

/* A display-list control word: end of list, valid, size, unscaled, and the pixel format. */
#define WATCH_CTL_END 0x80000000U
#define WATCH_CTL_VALID 0x40000000U
#define WATCH_CTL_SIZE_SHIFT 24U
#define WATCH_CTL_SIZE_MASK 0x3fU
#define WATCH_CTL_UNITY 0x00008000U
#define WATCH_CTL_FORMAT_MASK 0x0fU
#define WATCH_CTL_FORMAT_RGBA8888 7U

/* An unscaled plane is eight words; the most planes the watch looks at. */
#define WATCH_PLANE_WORDS 8U
#define WATCH_PLANES_MAX 8U

/* The smallest plane drawn on, which keeps the text off the mouse cursor. */
#define WATCH_PLANE_MIN_SIZE 128U

/* The place of the text on the screen, in pixels. */
#define WATCH_ORIGIN_X 16U
#define WATCH_ORIGIN_Y 16U

/*
 * The colours of the text.  The background is opaque whichever byte the
 * plane takes alpha from, and pure red or pure blue by its byte order;
 * white on either reads in a photograph.
 */
#define WATCH_FOREGROUND 0xffffffffU
#define WATCH_BACKGROUND 0xff0000ffU

/* The early console's colours: white on blue. */
#define WATCH_CELL_ATTRIBUTE 0x1fU

/* The early console's grid. */
#define WATCH_CELL_ROWS 25U
#define WATCH_CELL_COLUMNS 80U

/*
 * One plane of the display list that the watch can draw on: where its
 * image is, how it is laid out, and where it sits on the screen.
 */
struct watch_plane {
	uint32_t list_index;
	uint64_t physical;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
};

/*
 * The report, one terminated line per row.
 *
 * Written only by the CPU that holds watch_report_lock; a debugger reads
 * it as arm64_watch_text after a stop (x/40s arm64_watch_text).
 */
char arm64_watch_text[WATCH_LINES][WATCH_COLUMNS + 1U];

/*
 * Taken, without waiting, by the CPU that writes a report.  Zero when
 * free.  A CPU that finds it held skips its report: the watch never
 * spins, so it cannot join the stop it describes.
 */
static volatile unsigned watch_report_lock;

/*
 * The counter's frequency and the thresholds in counter ticks.  Set once
 * by arm64_watch_init() on the boot CPU; zero until then, which keeps the
 * watch from looking at anything.
 */
static uint64_t watch_frequency;
static uint64_t watch_stall_ticks;
static uint64_t watch_takeover_ticks;

/*
 * The compositor's registers through the direct map, or NULL when the
 * window could not be mapped.  Mapped once at boot; read only to draw.
 */
static volatile uint32_t *watch_hvs;

/*
 * The time of the last report and how many were written.  Under
 * watch_report_lock.  Reports during one stop are at least a second
 * apart, so a compositor that still runs on another CPU and draws over
 * the text gets it back.
 */
static uint64_t watch_last_report;
static unsigned watch_report_count;

/*
 * The time the current stop was first reported, 0 when nothing is
 * stopped, and whether its display list was replaced.  Under
 * watch_report_lock.
 */
static uint64_t watch_stall_start;
static int watch_taken_over;

/*
 * The first fatal stop: its CPU plus one (0 for none) and its text.
 * Written once under watch_report_lock and kept in every later report,
 * since the other CPUs report that CPU's silence after it.
 */
static unsigned watch_fatal_cpu;
static char watch_fatal_text[WATCH_COLUMNS + 1U];

/*
 * What each CPU last found of itself while it spun with interrupts off:
 * when, and the return addresses of its call chain.  Written by that CPU
 * under watch_report_lock.  A zero time means it never did.
 */
static uint64_t watch_chain_time[ARM64_CPU_MAX];
static uint64_t watch_chain[ARM64_CPU_MAX][WATCH_CHAIN_DEPTH];

/*
 * The last characters written to hal_putc(), which on this board carries
 * every record of the kernel's log.  watch_tail_next counts every
 * character ever noted; the ring holds the last WATCH_TAIL_BYTES of them.
 * Two CPUs writing at once may mix characters; the tail is a hint, not a
 * log.
 */
static char watch_tail[WATCH_TAIL_BYTES];
static unsigned watch_tail_next;

/*
 * The tail put in order for a report, under watch_report_lock.  Kept out
 * of the stack, which may be a small one on a CPU in trouble.
 */
static char watch_tail_copy[WATCH_TAIL_BYTES];

static int report_try_enter(struct arm64_percpu *cpu);
static void report_leave(struct arm64_percpu *cpu);
static void report_compose(struct arm64_percpu *self, const char *headline, uint64_t now);
static void report_show(uint64_t now);
static void check_others(struct arm64_percpu *self, uint64_t now);
static uint64_t time_since(uint64_t now, uint64_t then);
static int cpu_is_stopped(struct arm64_percpu *cpu, uint64_t now, uint64_t threshold);
static void record_chain(struct arm64_percpu *self, uint64_t now);
static void compose_cpu(unsigned *line, hal_cpu_id_t index, uint64_t now);
static void compose_log(unsigned *line);
static void fatal_compose(const char *file, int line, const char *message);
static void line_clear(unsigned line);
static void line_text(unsigned line, unsigned *column, const char *text);
static void line_hex(unsigned line, unsigned *column, uint64_t value, unsigned digits);
static void line_decimal(unsigned line, unsigned *column, uint64_t value);
static void line_seconds(unsigned line, unsigned *column, uint64_t ticks);
static void line_address(unsigned line, unsigned *column, uint64_t address);
static unsigned find_planes(struct watch_plane *planes, unsigned capacity);
static void draw_plane(const struct watch_plane *plane);
static void draw_cells(void);
static void take_over(const struct watch_plane *planes, unsigned count);

/*
 * Prepares the watch on the boot CPU, once the kernel's tables exist.
 *
 * The compositor's window is mapped now, while no CPU can be stuck
 * holding the table lock that the mapping takes.
 */
void
arm64_watch_init(
	void)
{
	uint64_t frequency;

	/* Reads the counter's frequency, which turns seconds into counter ticks. */
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	if (frequency == 0)
		return;

	/* Maps the compositor the board has, whose display list the report is written into. */
	watch_hvs = arm64_bsp_watch_compositor();

	/* The thresholds come last: a nonzero frequency turns the watch on. */
	watch_stall_ticks = frequency * WATCH_STALL_SECONDS;
	watch_takeover_ticks = frequency * WATCH_TAKEOVER_SECONDS;
	__atomic_store_n(&watch_frequency, frequency, __ATOMIC_RELEASE);
}

/*
 * Stamps the calling CPU's heartbeat at the start of its tick, and now
 * and then looks at the other CPUs' heartbeats.
 */
void
arm64_watch_beat(
	struct arm64_percpu *cpu,
	uint64_t now)
{
	uint64_t frequency;

	/* The heartbeat: this CPU takes interrupts. */
	__atomic_store_n(&cpu->watch_beat_time, now, __ATOMIC_RELAXED);

	/* Looks at the others only every WATCH_CHECK_TICKS ticks. */
	if ((cpu->timer_ticks % WATCH_CHECK_TICKS) != 0)
		return;

	/* Nothing is looked at before the watch knows the counter's speed. */
	frequency = __atomic_load_n(&watch_frequency, __ATOMIC_ACQUIRE);
	if (frequency == 0)
		return;

	/* Describes any CPU that stopped taking its tick. */
	check_others(cpu, now);
}

/*
 * Looks at the calling CPU's own heartbeat while it spins.
 *
 * Called now and then from the loops that wait for a lock.  A CPU that is
 * still running code but has not taken its tick for the threshold spins
 * with its interrupts off; it records its call chain and writes a report.
 */
void
arm64_watch_self_check(
	void)
{
	struct arm64_percpu *cpu;
	uint64_t frequency;
	uint64_t beat;
	uint64_t now;
	uint64_t age;
	uint64_t since_report;
	int masked;
	int entered;

	/*
	 * A caller with interrupts on takes its tick, and could be moved to
	 * another CPU after reading its record and describe that CPU as itself.
	 */
	masked = arm64_irq_masked();
	if (!masked)
		return;

	/* Nothing is looked at before the watch, the CPU's record and its first tick exist. */
	frequency = __atomic_load_n(&watch_frequency, __ATOMIC_ACQUIRE);
	if (frequency == 0)
		return;
	cpu = arm64_watch_record();
	if (cpu == NULL)
		return;
	beat = __atomic_load_n(&cpu->watch_beat_time, __ATOMIC_RELAXED);
	if (beat == 0)
		return;

	/* A recent tick means this CPU takes interrupts: nothing is wrong here. */
	now = arm64_watch_counter();
	age = time_since(now, beat);
	if (age < watch_stall_ticks)
		return;

	/* Writes the report unless another CPU is writing one. */
	entered = report_try_enter(cpu);
	if (!entered)
		return;

	/* Reports at most once a second during one stop. */
	since_report = time_since(now, watch_last_report);
	if (watch_last_report != 0 && since_report < watch_frequency) {
		report_leave(cpu);
		return;
	}

	/* The call chain says what this CPU waits for. */
	record_chain(cpu, now);
	report_compose(cpu, "spins with interrupts off", now);
	report_show(now);

	report_leave(cpu);
}

/*
 * Describes a fatal stop of the calling CPU at once.
 *
 * Called by hal_fatal() and hal_assert() before the CPU halts.  The first
 * fatal stop's text stays in every later report.
 */
void
arm64_watch_fatal(
	const char *file,
	int line,
	const char *message)
{
	struct arm64_percpu *cpu;
	uint64_t frequency;
	uint64_t now;
	int entered;

	/*
	 * The CPU halts with interrupts off after this; turning them off now
	 * keeps the caller on the CPU whose record describes the stop.
	 */
	arm64_irq_mask();

	/* Nothing is drawn before the watch and the CPU's record exist. */
	frequency = __atomic_load_n(&watch_frequency, __ATOMIC_ACQUIRE);
	if (frequency == 0)
		return;
	cpu = arm64_watch_record();
	if (cpu == NULL)
		return;

	/* A fatal stop inside a report, or during another CPU's, is not drawn. */
	entered = report_try_enter(cpu);
	if (!entered)
		return;

	/* Keeps the first fatal stop's site and message for every later report. */
	if (watch_fatal_cpu == 0) {
		watch_fatal_cpu = cpu->logical_id + 1U;
		fatal_compose(file, line, message);
	}

	/* Draws at once: this CPU writes nothing after it. */
	now = arm64_watch_counter();
	record_chain(cpu, now);
	report_compose(cpu, "stopped on a fatal error", now);
	report_show(now);

	report_leave(cpu);
}

/*
 * Keeps an unhandled exception's syndrome and addresses as the fatal stop.
 *
 * Called by the synchronous exception handler just before HAL_FATAL(),
 * whose own line then adds nothing: the first fatal stop's text is kept.
 */
void
arm64_watch_fault(
	uint64_t esr,
	uint64_t elr,
	uint64_t far)
{
	struct arm64_percpu *cpu;
	uint64_t frequency;
	unsigned row;
	unsigned column;
	int entered;

	/*
	 * HAL_FATAL() follows and halts with interrupts off; turning them off
	 * now keeps the caller on the CPU whose record keeps the fault.
	 */
	arm64_irq_mask();

	/* Nothing is kept before the watch and the CPU's record exist. */
	frequency = __atomic_load_n(&watch_frequency, __ATOMIC_ACQUIRE);
	if (frequency == 0)
		return;
	cpu = arm64_watch_record();
	if (cpu == NULL)
		return;

	/* A fault inside a report, or during another CPU's, is not kept. */
	entered = report_try_enter(cpu);
	if (!entered)
		return;

	/* "exception esr=... elr=... far=...", in full: the addresses may be user ones. */
	if (watch_fatal_cpu == 0) {
		watch_fatal_cpu = cpu->logical_id + 1U;
		row = WATCH_LINES - 1U;
		column = 0;
		line_clear(row);
		line_text(row, &column, "unhandled exception esr=");
		line_hex(row, &column, esr, 8U);
		line_text(row, &column, " elr=");
		line_hex(row, &column, elr, 16U);
		line_text(row, &column, " far=");
		line_hex(row, &column, far, 16U);
		hal_memcpy(watch_fatal_text, arm64_watch_text[row], sizeof(watch_fatal_text));
	}

	report_leave(cpu);
}

/*
 * Keeps one character written to the early console in the tail.
 */
void
arm64_watch_note_char(
	int character)
{
	unsigned position;

	/* Claims the next place in the ring and writes the character there. */
	position = __atomic_fetch_add(&watch_tail_next, 1U, __ATOMIC_RELAXED);
	watch_tail[position % WATCH_TAIL_BYTES] = (char)character;
}

/* Takes the report without waiting; reports whether this CPU may write one. */
static int
report_try_enter(
	struct arm64_percpu *cpu)
{
	int acquired;

	/* A report that faults into another one stops here. */
	if (cpu->watch_reporting != 0)
		return 0;

	/* Another CPU's report is left to finish. */
	acquired = hal_atomic_uint_try_acquire(&watch_report_lock);
	if (!acquired)
		return 0;

	/* Succeeded: this CPU writes the report. */
	cpu->watch_reporting = 1;
	return 1;
}

/* Releases the report for the next writer. */
static void
report_leave(
	struct arm64_percpu *cpu)
{
	/* The CPU is out of the report before another may take it. */
	cpu->watch_reporting = 0;
	__atomic_store_n(&watch_report_lock, 0U, __ATOMIC_RELEASE);
}

/*
 * Looks at every other CPU's heartbeat and reports the stopped ones.
 *
 * The report lists every CPU; a stopped one is marked.  When none is
 * stopped any more, the stop is over and the next one starts afresh.
 */
static void
check_others(
	struct arm64_percpu *self,
	uint64_t now)
{
	struct arm64_percpu *other;
	char headline[64];
	hal_cpu_id_t index;
	hal_cpu_id_t stopped;
	unsigned count;
	uint64_t started;
	uint64_t since_report;
	int silent;
	int entered;

	/* Finds the first CPU, other than this one, that is up but has not ticked. */
	stopped = HAL_CPU_MAX;
	count = hal_cpu_count();
	for (index = 0; index < count; index++) {
		/* This CPU is ticking: it is running this code. */
		if (index == self->logical_id)
			continue;

		/* A heartbeat older than the threshold is a stop. */
		other = arm64_percpu_get(index);
		silent = cpu_is_stopped(other, now, watch_stall_ticks);
		if (silent) {
			stopped = index;
			break;
		}
	}

	/* Nothing stopped: a stop that was reported is over, and the next one starts afresh. */
	if (stopped == HAL_CPU_MAX) {
		started = __atomic_load_n(&watch_stall_start, __ATOMIC_RELAXED);
		if (started == 0)
			return;
		entered = report_try_enter(self);
		if (!entered)
			return;
		watch_stall_start = 0;
		watch_taken_over = 0;
		report_leave(self);
		return;
	}

	/* Writes the report unless another CPU is writing one. */
	entered = report_try_enter(self);
	if (!entered)
		return;

	/* Reports at most once a second during one stop. */
	since_report = time_since(now, watch_last_report);
	if (watch_last_report != 0 && since_report < watch_frequency) {
		report_leave(self);
		return;
	}

	/* Names the stopped CPU in the headline. */
	hal_memcpy(headline, "cpuN has not taken its tick", sizeof("cpuN has not taken its tick"));
	headline[3] = (char)('0' + (stopped % 10U));
	report_compose(self, headline, now);
	report_show(now);

	report_leave(self);
}

/*
 * Reports how long ago a time was, or 0 for a time not before now.
 *
 * The counter is the same on every CPU, but a CPU reads it at its tick's
 * start and then reads what the others stamped: another CPU that took its
 * tick in between stamped a time after this one's now.  Such a time is no
 * time ago; the plain difference would wrap to a huge age and report a
 * CPU that ticks every millisecond as stopped.
 */
static uint64_t
time_since(
	uint64_t now,
	uint64_t then)
{
	/* A time stamped after now is as fresh as now. */
	if (then >= now)
		return 0;

	/* Succeeded: the counter ticks since then. */
	return now - then;
}

/*
 * Reports whether a CPU is up but has not taken its tick for the threshold.
 *
 * A CPU that is not up, that stopped on purpose, or that has not taken its
 * first tick has no heartbeat to judge, and is not stopped.
 */
static int
cpu_is_stopped(
	struct arm64_percpu *cpu,
	uint64_t now,
	uint64_t threshold)
{
	unsigned ready;
	unsigned parked;
	uint64_t beat;
	uint64_t age;

	/* A CPU that is not up yet has no heartbeat. */
	ready = __atomic_load_n(&cpu->ready, __ATOMIC_ACQUIRE);
	if (ready == 0)
		return 0;

	/* A CPU parked after a panic or for power-off is silent on purpose. */
	parked = __atomic_load_n(&cpu->watch_parked, __ATOMIC_ACQUIRE);
	if (parked != 0)
		return 0;

	/* A CPU that has not taken its first tick yet has nothing to be late with. */
	beat = __atomic_load_n(&cpu->watch_beat_time, __ATOMIC_RELAXED);
	if (beat == 0)
		return 0;

	/* A heartbeat within the threshold, or after now, is a CPU that ticks. */
	age = time_since(now, beat);
	if (age < threshold)
		return 0;

	/* Succeeded: the CPU stopped taking its tick. */
	return 1;
}

/* Records the calling CPU's call chain from its frame records. */
static void
record_chain(
	struct arm64_percpu *self,
	uint64_t now)
{
	const uint64_t *frame;
	const uint64_t *next;
	unsigned depth;
	hal_cpu_id_t index;

	/* The chain starts at this function's own frame record. */
	index = self->logical_id;
	frame = (const uint64_t *)__builtin_frame_address(0);
	for (depth = 0; depth < WATCH_CHAIN_DEPTH; depth++)
		watch_chain[index][depth] = 0;

	/*
	 * Follows the records while they stay in the kernel half, aligned and
	 * going up the stack; anything else ends the chain.
	 */
	for (depth = 0; depth < WATCH_CHAIN_DEPTH; depth++) {
		if ((uintptr_t)frame < ARM64_DIRECT_BASE)
			break;
		if (((uintptr_t)frame & 7U) != 0)
			break;
		watch_chain[index][depth] = frame[1];
		next = (const uint64_t *)(uintptr_t)frame[0];
		if (next <= frame)
			break;
		frame = next;
	}

	/* The chain is as recent as now. */
	watch_chain_time[index] = now;
}

/*
 * Writes the whole report into arm64_watch_text.
 *
 * The headline names what the writing CPU saw; then come the times, the
 * first fatal stop, every CPU's state, and the tail of the kernel's log.
 */
static void
report_compose(
	struct arm64_percpu *self,
	const char *headline,
	uint64_t now)
{
	unsigned line;
	unsigned column;
	hal_cpu_id_t index;
	unsigned count;

	/* Starts a stop at its first report. */
	if (watch_stall_start == 0)
		__atomic_store_n(&watch_stall_start, now, __ATOMIC_RELAXED);
	watch_report_count++;

	/* Empties every line. */
	for (line = 0; line < WATCH_LINES; line++)
		line_clear(line);

	/* The headline: who saw what. */
	line = 0;
	column = 0;
	line_text(line, &column, "zedBSD stall watch: ");
	line_text(line, &column, headline);
	line_text(line, &column, " (seen by cpu");
	line_decimal(line, &column, self->logical_id);
	line_text(line, &column, ")");
	line++;

	/* The time, the report's number and how long the stop has lasted. */
	column = 0;
	line_text(line, &column, "uptime ");
	line_seconds(line, &column, now);
	line_text(line, &column, "  report ");
	line_decimal(line, &column, watch_report_count);
	line_text(line, &column, "  stop for ");
	line_seconds(line, &column, time_since(now, watch_stall_start));
	line_text(line, &column, "  addresses: ffff0000 + shown");
	line++;

	/* The first fatal stop, kept from when it happened. */
	if (watch_fatal_cpu != 0) {
		column = 0;
		line_text(line, &column, "cpu");
		line_decimal(line, &column, watch_fatal_cpu - 1U);
		line_text(line, &column, " ");
		line_text(line, &column, watch_fatal_text);
		line++;
	}

	/* Every CPU's state. */
	count = hal_cpu_count();
	for (index = 0; index < count; index++)
		compose_cpu(&line, index, now);

	/* The tail of the kernel's log. */
	compose_log(&line);
}

/* Writes one CPU's lines of the report. */
static void
compose_cpu(
	unsigned *line,
	hal_cpu_id_t index,
	uint64_t now)
{
	struct arm64_percpu *cpu;
	unsigned column;
	unsigned depth;
	uint64_t beat;
	uint64_t off_time;
	uint64_t irq_id;
	uint64_t irq_time;
	unsigned parked;
	int silent;

	/* Leaves room for the log's heading. */
	if (*line + 3U >= WATCH_LINES)
		return;

	/* Reads the CPU's record, which it may be changing. */
	cpu = arm64_percpu_get(index);
	beat = __atomic_load_n(&cpu->watch_beat_time, __ATOMIC_RELAXED);
	off_time = __atomic_load_n(&cpu->watch_irq_off_time, __ATOMIC_RELAXED);
	irq_id = __atomic_load_n(&cpu->watch_irq_id, __ATOMIC_RELAXED);
	irq_time = __atomic_load_n(&cpu->watch_irq_time, __ATOMIC_RELAXED);
	parked = __atomic_load_n(&cpu->watch_parked, __ATOMIC_RELAXED);

	/* Judges the CPU as the look at the heartbeats does. */
	silent = cpu_is_stopped(cpu, now, watch_stall_ticks);

	/* The first line: the tick, and where interrupts went off. */
	column = 0;
	line_text(*line, &column, "cpu");
	line_decimal(*line, &column, index);
	if (silent) {
		/* The CPU stopped taking its tick without meaning to. */
		line_text(*line, &column, " STOPPED");
	} else if (parked != 0) {
		/* The CPU halted on purpose, after a panic or for power-off. */
		line_text(*line, &column, " parked");
	}
	line_text(*line, &column, " tick ");
	if (beat == 0) {
		line_text(*line, &column, "never");
	} else {
		line_text(*line, &column, "-");
		line_seconds(*line, &column, time_since(now, beat));
	}
	line_text(*line, &column, " irq-off ");
	if (off_time == 0) {
		line_text(*line, &column, "no");
	} else {
		line_text(*line, &column, "-");
		line_seconds(*line, &column, time_since(now, off_time));
		line_text(*line, &column, " at ");
		line_address(*line, &column, __atomic_load_n(&cpu->watch_irq_off_pc, __ATOMIC_RELAXED));
		line_text(*line, &column, " < ");
		line_address(*line, &column, __atomic_load_n(&cpu->watch_irq_off_caller, __ATOMIC_RELAXED));
	}
	*line += 1U;

	/* The second line: the interrupt it serves, its switches, ticks and task. */
	column = 0;
	line_text(*line, &column, "     in-irq ");
	if (irq_id == 0) {
		line_text(*line, &column, "no");
	} else {
		line_decimal(*line, &column, irq_id - 1U);
		line_text(*line, &column, " -");
		line_seconds(*line, &column, time_since(now, irq_time));
		line_text(*line, &column, " from ");
		line_address(*line, &column, __atomic_load_n(&cpu->watch_irq_elr, __ATOMIC_RELAXED));
	}
	line_text(*line, &column, " switches ");
	line_decimal(*line, &column, __atomic_load_n(&cpu->watch_switches, __ATOMIC_RELAXED));
	line_text(*line, &column, " ticks ");
	line_decimal(*line, &column, __atomic_load_n(&cpu->timer_ticks, __ATOMIC_RELAXED));
	line_text(*line, &column, " task ");
	line_hex(*line, &column, (uint64_t)(uintptr_t)__atomic_load_n(&cpu->running_task, __ATOMIC_RELAXED), 12U);
	*line += 1U;

	/* A third line when the CPU recorded its call chain while it spun. */
	if (watch_chain_time[index] == 0)
		return;
	column = 0;
	line_text(*line, &column, "     chain -");
	line_seconds(*line, &column, time_since(now, watch_chain_time[index]));
	line_text(*line, &column, ":");
	for (depth = 0; depth < WATCH_CHAIN_DEPTH; depth++) {
		if (watch_chain[index][depth] == 0)
			break;
		line_text(*line, &column, " ");
		line_address(*line, &column, watch_chain[index][depth]);
	}
	*line += 1U;
}

/* Writes the last lines of the kernel's log that passed through the early console. */
static void
compose_log(
	unsigned *line)
{
	char *copy;
	unsigned next;
	unsigned length;
	unsigned start;
	unsigned position;
	unsigned found;
	unsigned column;
	char character;

	/* Leaves out the log when the CPUs filled the screen. */
	if (*line + 2U >= WATCH_LINES)
		return;

	/* The heading. */
	column = 0;
	line_text(*line, &column, "kernel log (last lines):");
	*line += 1U;

	/* Copies the ring in order, oldest first. */
	copy = watch_tail_copy;
	next = __atomic_load_n(&watch_tail_next, __ATOMIC_RELAXED);
	length = next;
	if (length > WATCH_TAIL_BYTES)
		length = WATCH_TAIL_BYTES;
	for (position = 0; position < length; position++)
		copy[position] = watch_tail[(next - length + position) % WATCH_TAIL_BYTES];

	/* Finds where the last lines start, counting newlines from the end. */
	start = 0;
	found = 0;
	position = length;
	while (position > 0) {
		position--;
		if (copy[position] != '\n' || position + 1U == length)
			continue;
		found++;
		if (found == WATCH_LOG_LINES) {
			start = position + 1U;
			break;
		}
	}

	/* Writes them, one report line per log line, as far as room lasts. */
	column = 0;
	for (position = start; position < length; position++) {
		if (*line >= WATCH_LINES)
			break;
		character = copy[position];
		if (character == '\n') {
			*line += 1U;
			column = 0;
			continue;
		}
		if (character == '\r')
			continue;
		if (column < WATCH_COLUMNS) {
			if (character < 0x20 || character >= 0x7f)
				character = '?';
			arm64_watch_text[*line][column] = character;
			column++;
			arm64_watch_text[*line][column] = '\0';
		}
	}
}

/*
 * Writes the first fatal stop's line, which every later report repeats.
 *
 * The line is composed in the report's last row, which the report
 * rewrites afterwards, and kept apart.
 */
static void
fatal_compose(
	const char *file,
	int line,
	const char *message)
{
	unsigned row;
	unsigned column;
	uint64_t number;

	/* Names a site or a message that did not come. */
	if (file == NULL)
		file = "?";
	if (message == NULL)
		message = "?";
	number = 0;
	if (line > 0)
		number = (uint64_t)line;

	/* "fatal: file:line: message", cut at the line's width. */
	row = WATCH_LINES - 1U;
	column = 0;
	line_clear(row);
	line_text(row, &column, "fatal: ");
	line_text(row, &column, file);
	line_text(row, &column, ":");
	line_decimal(row, &column, number);
	line_text(row, &column, ": ");
	line_text(row, &column, message);

	/* Keeps the line apart from the report that overwrites the row. */
	hal_memcpy(watch_fatal_text, arm64_watch_text[row], sizeof(watch_fatal_text));
}

/* Empties one line of the report. */
static void
line_clear(
	unsigned line)
{
	/* A terminator at the start makes the line empty. */
	arm64_watch_text[line][0] = '\0';
}

/* Appends text to a line, cut at the line's width. */
static void
line_text(
	unsigned line,
	unsigned *column,
	const char *text)
{
	/* Copies characters while both the text and the line last. */
	while (*text != '\0' && *column < WATCH_COLUMNS) {
		arm64_watch_text[line][*column] = *text;
		*column += 1U;
		text++;
	}

	/* Keeps the line terminated. */
	arm64_watch_text[line][*column] = '\0';
}

/* Appends a number in hexadecimal with a fixed count of digits. */
static void
line_hex(
	unsigned line,
	unsigned *column,
	uint64_t value,
	unsigned digits)
{
	static const char hex[] = "0123456789abcdef";
	char text[17];
	unsigned index;

	/* Writes the digits from the least significant end. */
	if (digits > 16U)
		digits = 16U;
	text[digits] = '\0';
	for (index = digits; index > 0; index--) {
		text[index - 1U] = hex[value & 15U];
		value >>= 4;
	}

	/* Appends them. */
	line_text(line, column, text);
}

/* Appends a number in decimal. */
static void
line_decimal(
	unsigned line,
	unsigned *column,
	uint64_t value)
{
	char text[21];
	unsigned index;

	/* Writes the digits from the least significant end. */
	index = sizeof(text) - 1U;
	text[index] = '\0';
	do {
		index--;
		text[index] = (char)('0' + (value % 10U));
		value /= 10U;
	} while (value != 0 && index > 0);

	/* Appends them. */
	line_text(line, column, &text[index]);
}

/* Appends counter ticks as seconds with milliseconds. */
static void
line_seconds(
	unsigned line,
	unsigned *column,
	uint64_t ticks)
{
	uint64_t seconds;
	uint64_t milliseconds;
	char text[4];

	/* Splits the ticks into whole seconds and the milliseconds after them. */
	seconds = ticks / watch_frequency;
	milliseconds = (ticks % watch_frequency) * 1000U / watch_frequency;

	/* Writes seconds, the point and three digits of milliseconds. */
	line_decimal(line, column, seconds);
	text[0] = (char)('0' + (milliseconds / 100U) % 10U);
	text[1] = (char)('0' + (milliseconds / 10U) % 10U);
	text[2] = (char)('0' + milliseconds % 10U);
	text[3] = '\0';
	line_text(line, column, ".");
	line_text(line, column, text);
	line_text(line, column, "s");
}

/*
 * Appends a kernel address as its low 32 bits.
 *
 * The kernel's text sits at ffff0000 00000000 and up, so the low half is
 * what a symbol table lookup needs; the report's second line says so.
 */
static void
line_address(
	unsigned line,
	unsigned *column,
	uint64_t address)
{
	/* The low eight hexadecimal digits. */
	line_hex(line, column, address & 0xffffffffU, 8U);
}

/*
 * Puts the report where a photograph of the screen reads it.
 *
 * The text goes onto the planes the compositor shows now; without a
 * compositor, or with no plane it can draw on, onto the firmware's
 * framebuffer.  A stop that lasts long enough replaces the display list
 * with the largest plane alone, so no window above it hides the text.
 */
static void
report_show(
	uint64_t now)
{
	struct watch_plane planes[WATCH_PLANES_MAX];
	unsigned count;
	unsigned index;
	uint64_t stop_length;

	/* The report is as recent as now. */
	watch_last_report = now;

	/* The firmware's framebuffer on a board without a compositor. */
	if (watch_hvs == NULL) {
		draw_cells();
		return;
	}

	/* The planes the compositor shows now. */
	count = find_planes(planes, WATCH_PLANES_MAX);
	if (count == 0) {
		draw_cells();
		return;
	}

	/* Draws on every plane under the text's place. */
	for (index = 0; index < count; index++)
		draw_plane(&planes[index]);

	/* A long stop gets the screen to itself. */
	stop_length = time_since(now, watch_stall_start);
	if (!watch_taken_over && stop_length >= watch_takeover_ticks) {
		take_over(planes, count);
		watch_taken_over = 1;
	}
}

/*
 * Finds the unscaled 32-bit planes of the display list the compositor
 * shows on its first channel.
 *
 * The list is walked by each entry's own size; an entry that does not
 * look like a list entry ends the walk.
 */
static unsigned
find_planes(
	struct watch_plane *planes,
	unsigned capacity)
{
	volatile uint32_t *list;
	uint32_t index;
	uint32_t control;
	uint32_t size;
	uint32_t pointer;
	uint32_t extent;
	unsigned count;
	unsigned entries;
	int given_up;

	/* The list the hardware follows starts at the word DISPLIST0 names. */
	list = (volatile uint32_t *)((volatile uint8_t *)watch_hvs + WATCH_HVS_DLIST);
	index = watch_hvs[WATCH_HVS_DISPLIST0 / 4U];
	count = 0;

	/* Walks the entries until the end marker, a bad entry, or enough planes. */
	for (entries = 0; entries < 16U && count < capacity; entries++) {
		/* An index past the list memory is not a list. */
		if (index >= WATCH_HVS_DLIST_WORDS)
			break;
		control = list[index];
		if ((control & WATCH_CTL_END) != 0)
			break;
		if ((control & WATCH_CTL_VALID) == 0)
			break;

		/* An entry's size covers at least its control word, and stays in the memory. */
		size = (control >> WATCH_CTL_SIZE_SHIFT) & WATCH_CTL_SIZE_MASK;
		if (size == 0 || index + size > WATCH_HVS_DLIST_WORDS)
			break;

		/* Only an unscaled 32-bit plane has its image where the watch can draw. */
		if (size == WATCH_PLANE_WORDS &&
		    (control & WATCH_CTL_UNITY) != 0 &&
		    (control & WATCH_CTL_FORMAT_MASK) == WATCH_CTL_FORMAT_RGBA8888) {
			pointer = list[index + 5U];
			extent = list[index + 3U];
			planes[count].list_index = index;
			planes[count].physical = (uint64_t)(pointer & 0x3fffffffU);
			planes[count].pitch = list[index + 7U] & 0xffffU;
			planes[count].width = extent & 0xffffU;
			planes[count].height = extent >> 16;
			planes[count].x = list[index + 1U] & 0x3fffU;
			planes[count].y = (list[index + 1U] >> 16) & 0x3fffU;

			/*
			 * Keeps a plane big enough to hold text and laid out as its pitch
			 * says, and not on the firmware's framebuffer once the kernel's
			 * console gave that up: nothing in the HAL paints there then.
			 */
			given_up = rpi_framebuffer_given_up(planes[count].physical, (uint64_t)planes[count].pitch * planes[count].height);
			if (planes[count].width >= WATCH_PLANE_MIN_SIZE &&
			    planes[count].height >= WATCH_PLANE_MIN_SIZE &&
			    planes[count].pitch >= planes[count].width * 4U &&
			    planes[count].physical != 0 &&
			    !given_up)
				count++;
		}

		/* The next entry follows this one. */
		index += size;
	}

	/* Succeeded: the planes found. */
	return count;
}

/*
 * Draws the report onto one plane's image, where the plane covers the
 * text's place on the screen, and cleans what it drew out to memory.
 */
static void
draw_plane(
	const struct watch_plane *plane)
{
	volatile uint32_t *row;
	const uint8_t *glyph;
	unsigned line;
	unsigned glyph_row;
	unsigned column;
	unsigned bit;
	unsigned screen_x;
	unsigned screen_y;
	unsigned local_x;
	unsigned local_y;
	unsigned first_x;
	unsigned last_x;
	uint32_t value;
	char character;
	int ended;

	/* Every pixel row of every text line. */
	for (line = 0; line < WATCH_LINES; line++) {
		for (glyph_row = 0; glyph_row < RPI_FONT_HEIGHT; glyph_row++) {
			/* The row on the screen, and in the plane when the plane covers it. */
			screen_y = WATCH_ORIGIN_Y + line * RPI_FONT_HEIGHT + glyph_row;
			if (screen_y < plane->y)
				continue;
			local_y = screen_y - plane->y;
			if (local_y >= plane->height)
				continue;
			row = (volatile uint32_t *)arm64_phys_to_direct((uintptr_t)(plane->physical + (uint64_t)local_y * plane->pitch));

			/* Paints the line's cells; past the text's end, background. */
			first_x = plane->width;
			last_x = 0;
			ended = 0;
			for (column = 0; column < WATCH_COLUMNS; column++) {
				character = ' ';
				if (!ended) {
					character = arm64_watch_text[line][column];
					if (character == '\0') {
						ended = 1;
						character = ' ';
					}
				}
				glyph = &rpi_font8x16[((unsigned)(unsigned char)character) * RPI_FONT_HEIGHT];
				for (bit = 0; bit < 8U; bit++) {
					screen_x = WATCH_ORIGIN_X + column * 8U + bit;
					if (screen_x < plane->x)
						continue;
					local_x = screen_x - plane->x;
					if (local_x >= plane->width)
						continue;
					value = WATCH_BACKGROUND;
					if ((glyph[glyph_row] & (0x80U >> bit)) != 0)
						value = WATCH_FOREGROUND;
					row[local_x] = value;
					if (local_x < first_x)
						first_x = local_x;
					if (local_x > last_x)
						last_x = local_x;
				}
			}

			/* The display reads memory, not the CPU's cache: cleans the painted run. */
			if (first_x <= last_x)
				hal_dcache_clean_range((uintptr_t)&row[first_x], (size_t)(last_x - first_x + 1U) * 4U);
		}
	}

	/* The compositor is outside the CPU's shareable domain: a full barrier. */
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Draws the report with the early console's cell painter, white on blue,
 * as far as its 80 by 25 grid goes.
 */
static void
draw_cells(
	void)
{
	unsigned row;
	unsigned column;
	char character;
	int ended;

	/* Every cell of the grid: the text, then blanks. */
	for (row = 0; row < WATCH_CELL_ROWS; row++) {
		ended = 0;
		for (column = 0; column < WATCH_CELL_COLUMNS; column++) {
			character = ' ';
			if (!ended && row < WATCH_LINES) {
				character = arm64_watch_text[row][column];
				if (character == '\0') {
					ended = 1;
					character = ' ';
				}
			}
			rpi_framebuffer_cell(row, column, character, WATCH_CELL_ATTRIBUTE);
		}
	}
}

/*
 * Points the compositor at a list of the largest plane alone.
 *
 * The list goes into display-list memory above every list the display
 * driver writes, with fresh context words as the driver writes them.  The
 * compositor takes it at its next frame.  A display driver that still
 * runs points it back at its own list with its next flip.
 */
static void
take_over(
	const struct watch_plane *planes,
	unsigned count)
{
	volatile uint32_t *list;
	const struct watch_plane *largest;
	uint64_t area;
	uint64_t largest_area;
	unsigned index;

	/* Picks the plane that covers the most of the screen. */
	largest = NULL;
	largest_area = 0;
	for (index = 0; index < count; index++) {
		area = (uint64_t)planes[index].width * planes[index].height;
		if (area > largest_area) {
			largest_area = area;
			largest = &planes[index];
		}
	}
	if (largest == NULL)
		return;

	/* Copies the plane's words, with fresh context words, then the end marker. */
	list = (volatile uint32_t *)((volatile uint8_t *)watch_hvs + WATCH_HVS_DLIST);
	for (index = 0; index < WATCH_PLANE_WORDS; index++)
		list[WATCH_HVS_OWN_LIST + index] = list[largest->list_index + index];
	list[WATCH_HVS_OWN_LIST + 4U] = 0xc0c0c0c0U;
	list[WATCH_HVS_OWN_LIST + 6U] = 0xc0c0c0c0U;
	list[WATCH_HVS_OWN_LIST + WATCH_PLANE_WORDS] = WATCH_CTL_END;

	/* The complete list precedes the pointer the compositor follows. */
	__asm__ volatile("dsb sy" ::: "memory");
	watch_hvs[WATCH_HVS_DISPLIST0 / 4U] = WATCH_HVS_OWN_LIST;
	__asm__ volatile("dsb sy" ::: "memory");
}
