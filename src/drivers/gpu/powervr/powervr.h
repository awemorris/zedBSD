/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Imagination PowerVR Rogue GPU core (the JH7110's BXE-4-32).
 *
 * The SoC's glue describes the core -- where its registers are, its
 * interrupt, how it reaches memory -- and supplies the steps that power
 * it, start its clocks and release its resets, which differ by SoC.  The
 * driver itself only talks to the core's registers.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_H
#define KERN_DRIVERS_GPU_POWERVR_H

#include <stdint.h>

/*
 * Whether the driver and its board's glue say how the start and the
 * sessions went (1, the build's CONFIG_POWERVR_DEBUG=y, the default):
 * each start step's start and end, the clocks, the coherency checks
 * (which run only then), the self-test's and the stages' lines, each
 * session's contexts and each display lease's counts; the firmware keeps
 * a trace, which a reset prints with its records.  0
 * (CONFIG_POWERVR_DEBUG=n, a release image) says only the core found,
 * the firmware run and what failed; the firmware keeps no trace.
 */
#ifndef CONFIG_POWERVR_DEBUG
#define CONFIG_POWERVR_DEBUG	1
#endif
#define POWERVR_DEBUG	CONFIG_POWERVR_DEBUG

/*
 * The SoC's steps around the core.
 *
 * Power_up powers the core's domain, starts its clocks at their rates and
 * releases its resets, and reports the core clock's real rate in hertz;
 * on failure it leaves everything off.  Power_down undoes it in reverse.
 */
struct drv_powervr_power {
	int (*power_up)(void *context, uint64_t *core_rate);
	void (*power_down)(void *context);
};

/*
 * One core as the SoC's glue describes it.
 *
 * Coherent says the core snoops the processor's caches; the JH7110's does
 * not, as far as is known, so its glue passes zero.  DMA limit is the
 * highest physical address the core is let name.  The description is
 * copied by drv_powervr_attach(); the power steps and their context must
 * live as long as the system.
 */
struct drv_powervr_description {
	uint64_t registers;
	uint64_t register_size;
	uint32_t interrupt;
	int coherent;
	uint64_t dma_limit;
	const struct drv_powervr_power *power;
	void *context;
};

/*
 * Brings the core up as far as reading its identity, and checks the
 * uncached view of memory its firmware's structures will need.
 *
 * ENODEV means the core is not the one the driver knows; it is then
 * powered down again.  Each outcome is one line of the boot log.
 */
int drv_powervr_attach(const struct drv_powervr_description *description);

/*
 * Publishes the loader's screen as the driver's display node.
 *
 * Runs before drv_powervr_attach(), so the display node is the first GPU
 * device and the render node the second; it does not depend on the core.
 * ENOENT means the loader left no screen.  Each outcome is one line of
 * the boot log.
 */
int drv_powervr_display_attach(void);

/*
 * Where the board's display controller is.
 *
 * Control holds the identity and interrupt registers; display holds the
 * planes and panels, at the controller's offsets from control's base.
 */
struct drv_powervr_dc_place {
	uint64_t control;
	uint64_t control_size;
	uint64_t display;
	uint64_t display_size;
	uint32_t interrupt;
};

/*
 * Reads the display controller the loader left on and decides whether its
 * screen can be taken over, without changing any of its registers.
 *
 * The decision, the controller's identity and the plane's settings are one
 * line of the boot log, which the screen shows.  The controller's power
 * domain and clocks must be on.  ENOENT means the loader left no screen.
 */
int drv_powervr_dc_probe(const struct drv_powervr_dc_place *place);

/*
 * Where the parts the driver sets when it lights the output itself are:
 * the controller, the HDMI transmitter's window, and the display
 * subsystem's control block (subsystem 0 when the loader powered the
 * output and its bits are left alone).
 */
struct drv_powervr_dc_own_place {
	struct drv_powervr_dc_place controller;
	uint64_t hdmi;
	uint64_t hdmi_size;
	uint64_t subsystem;
	uint64_t subsystem_size;
};

/*
 * Lights the HDMI output at the mode the boot line names with
 * display.mode= (1920x1080 at 60 Hz without one) and shows a framebuffer
 * on the HDMI panel's plane: the loader's when it holds the mode, so the
 * early console stays visible, otherwise one allocated below 4 GiB and
 * painted with colour bars.
 *
 * The plane and the framebuffer become the place a takeover and a fatal
 * stop return the screen to.  The output's power domain, clocks and
 * resets must be on.  One line of the boot log reports the outcome.
 */
int drv_powervr_dc_own_mode(const struct drv_powervr_dc_own_place *place);

/*
 * Reports whether the driver lights the output itself: when the loader
 * left no screen, or when the boot line names display.mode=.  The loader's
 * screen is taken over otherwise.
 */
int drv_powervr_dc_own_wanted(void);

/*
 * Returns the screen to the loader's plane for a fatal stop.
 *
 * Marks the controller fatal first, so no later commit is written, then
 * writes the plane registers drv_powervr_dc_probe() read back and commits
 * them, without waiting for the frame to take them.  The commit lock is
 * tried a bounded number of times and the registers are written even
 * when it stays held, since the CPU holding it may be the one stopping.
 * Does nothing unless the probe found the screen could be taken over.
 * Runs with interrupts disabled and takes no other lock.
 */
void drv_powervr_dc_restore_fatal(void);

/*
 * Reports whether a fatal stop has taken the screen back; a commit checks
 * it under the commit lock and writes nothing once it is set.
 */
int drv_powervr_dc_fatal(void);

/*
 * A program's frame for the display controller's plane: where it starts
 * in physical memory (below 4 GiB, in one contiguous run), its row pitch
 * and extent, and whether its bytes are R, G, B, X (nonzero) or B, G, R,
 * X (zero).
 */
struct drv_powervr_dc_frame {
	uint64_t physical;
	uint32_t stride;
	uint32_t width;
	uint32_t height;
	int rgba;
};

/*
 * Turns on the HDMI panel's vertical-blank interrupt and starts counting
 * the panel's frames for the life of the system.
 *
 * ENODEV unless the probe found the screen could be taken over.
 */
int drv_powervr_dc_vblank_start(void);

/*
 * Reports whether a program's buffer may be committed to the plane: the
 * screen could be taken over, a vertical blank has been counted, and no
 * fatal stop took the screen back.
 */
int drv_powervr_dc_scanout_ready(void);

/*
 * Reports whether a frame of this extent and row pitch is the plane's as
 * the loader set it up; only such a frame is committed.
 */
int drv_powervr_dc_frame_fits(uint32_t width, uint32_t height, uint32_t stride);

/*
 * Commits a frame, or the loader's framebuffer when frame is NULL, to the
 * plane, holding the shadowed registers back while they are written, and
 * stores the vertical-blank count read right after the commit.
 *
 * The plane takes the commit at one of the next blanks; it counts as
 * taken two blanks after the stored count.  ENXIO once a fatal stop took
 * the screen back: nothing is written then.  Takes the commit lock with
 * interrupts off; must not be called with it held.
 */
int drv_powervr_dc_commit(const struct drv_powervr_dc_frame *frame, uint64_t *base);

/*
 * Where the last commit was written in its frame (the display's flip
 * report): the blank count read after it, how far after the newest blank
 * and the period then (microseconds), and whether it counted as early,
 * taken at the next blank.  drv_powervr_dc_phase() gives the same place
 * for now.
 */
struct drv_powervr_dc_commit_note {
	uint64_t sequence;
	uint64_t since_us;
	uint64_t period_us;
	int early;
};

void drv_powervr_dc_commit_noted(struct drv_powervr_dc_commit_note *note);
void drv_powervr_dc_phase(uint64_t *since_us, uint64_t *period_us);
int drv_powervr_dc_cursor(int shown, uint64_t physical, int32_t x, int32_t y);

/* Reports the HDMI panel's vertical-blank count. */
uint64_t drv_powervr_dc_sequence(void);

/*
 * Waits until the vertical-blank count reaches target.
 *
 * ETIMEDOUT when no blank came for 100 ms: the panel stopped.  The count
 * reached is stored through reached either way.  Sleeps; thread context
 * only.
 */
int drv_powervr_dc_wait(uint64_t target, uint64_t *reached);

/*
 * Writes the vertical-blank count and the range of scan positions the
 * interrupt read to the boot log.
 */
void drv_powervr_dc_vblank_report(void);

/*
 * The codec operations a video engine beside the core performs (bits of
 * the operations word): the render node shows them as Vulkan Video on its
 * one queue family.
 */
#define DRV_POWERVR_VIDEO_DECODE_H264	0x1U
#define DRV_POWERVR_VIDEO_DECODE_H265	0x2U
#define DRV_POWERVR_VIDEO_ENCODE_H265	0x4U

/*
 * The operations of a video engine (a codec core the SoC has beside the
 * GPU) the render node runs Vulkan Video on.
 *
 * Operations reports the codec operations the engine performs, once its
 * firmware has started; it must not change afterwards, since every
 * session reads it once, when it opens, and keeps the answer.  The table
 * and the engine must live as long as the system.
 */
struct drv_powervr_video_ops {
	uint32_t (*operations)(void *engine);
};

/*
 * Registers the board's one video engine with the render node.  Sessions
 * opened afterwards offer its operations; those opened before offer no
 * video.  EINVAL for a table without its operations, EBUSY when an engine
 * is registered already.
 */
int drv_powervr_video_register(const struct drv_powervr_video_ops *ops, void *engine);

#endif
