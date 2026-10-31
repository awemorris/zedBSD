/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Stage marks of the nvrtx driver, and its boot parameters.
 *
 * The driver is brought up on the board one stage at a time.  Each stage
 * writes a line when it begins and when it ends, so the console on the
 * firmware's framebuffer, and the kernel log, show how far the driver came.
 * Lines are cut to the console's width.
 *
 * The stages that make the display work run by default (without them the
 * desktop does not run on a machine whose display is on the NVIDIA GPU): the
 * GSP boot, the VRAM window (vram-window), the display's readout, the
 * takeover of the firmware's head showing the console (takeover) and the GPU
 * device (gpu-device).  The test stages run only when asked.
 *
 * The parameters are read from the raw boot command line, because the driver
 * attaches while the platform discovers its devices, before the kernel parses
 * the line:
 *
 *   nvrtx.off=1, nvrtx=off
 *                         the driver does not look at the GPU at all
 *   nvrtx.stop=<stage>    the driver stops before that stage
 *   nvrtx.gsp=0           the GPU is left to the firmware after console (the
 *                         stages that write never run); nvrtx.gsp=1 also
 *                         boots the GSP when the firmware's picture is
 *                         not on the GPU or cannot be kept (a test run)
 *   nvrtx.disp=0          the driver stops after the display's readout
 *                         (display-client) instead of taking the firmware's
 *                         head over
 *   nvrtx.fail=<point>    a test of the way back to the firmware's picture:
 *                         the GSP boot gives up at that point as if a
 *                         program there had not answered (frts: after
 *                         FWSEC-FRTS, booter: after booter_load, gsp-init:
 *                         200 ms into the firmware's init); the GPU is then
 *                         held until a power cycle
 *   nvrtx.resident=0      after the takeover the GSP is stopped instead of
 *                         staying up as a GPU device (gpu-device)
 *   nvrtx.disp.own=1      the takeover shows the driver's own surface (the
 *                         test's bands) at once, without the test's flip and
 *                         waits: for a GPU the firmware's console is not on
 *                         (a VFIO guest), so the GPU device is still
 *                         registered
 *   nvrtx.disp.bl=0       the display scans out linear rows only (a
 *                         block-linear image is refused for scanout and
 *                         shown by a copy); by default it also scans out
 *                         block-linear images (applications' and the
 *                         compositor's shared images in the GPU's own
 *                         layout)
 *   nvrtx.disp.test=1     the takeover shows two test surfaces of bands,
 *                         flips between them and waits 5 s at each step, so
 *                         the monitor can be watched
 *   nvrtx.gr=1            after the copy channel, the driver has the
 *                         resource manager make the graphics golden context
 *                         (and the TU10x scrubber channel), the golden-context
 *                         stage
 *   nvrtx.compute=1       the first compute launch (compute-test, a test)
 *   nvrtx.draw=1          the first draw (draw-test, a test)
 *   nvrtx.work=1          the work channel the GPU device keeps (work-channel,
 *                         with the golden context it needs; the renderer
 *                         implies it)
 *   nvrtx.vk=0            the GPU device is a display alone; by default it
 *                         is also a renderer that takes the Vulkan
 *                         protocol's commands and runs them on the work
 *                         channel, which it implies
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include <kern/boot.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/platform.h>

#include "drivers/gpu/nvrtx/nvrtx-private.h"

/* The handoff object that holds the raw boot command line. */
#define STAGE_COMMAND_LINE_HANDOFF	"boot.command-line"

/* The whole tokens that keep the driver away from the GPU. */
#define STAGE_OFF_TOKEN			"nvrtx.off=1"
#define STAGE_OFF_SHORT_TOKEN		"nvrtx=off"

/* The whole tokens that keep the GSP boot path off, and that force it on. */
#define STAGE_GSP_OFF_TOKEN		"nvrtx.gsp=0"
#define STAGE_GSP_TOKEN			"nvrtx.gsp=1"

/* The whole token that asks for the graphics golden context. */
#define STAGE_GR_TOKEN			"nvrtx.gr=1"

/* The whole token that keeps the display takeover off, and the one that makes it a test. */
#define STAGE_DISP_OFF_TOKEN		"nvrtx.disp=0"
#define STAGE_DISP_TEST_TOKEN		"nvrtx.disp.test=1"

/* The whole token that makes the takeover show the driver's own surfaces at once (no console on the GPU, no waits). */
#define STAGE_DISP_OWN_TOKEN		"nvrtx.disp.own=1"

/* The whole token that keeps the display to linear rows (no block-linear scanout). */
#define STAGE_DISP_BL_OFF_TOKEN		"nvrtx.disp.bl=0"

/* The whole token that asks for the first compute launch. */
#define STAGE_COMPUTE_TOKEN		"nvrtx.compute=1"

/* The whole token that asks for the first draw. */
#define STAGE_DRAW_TOKEN		"nvrtx.draw=1"

/* The whole token that asks for the work channel. */
#define STAGE_WORK_TOKEN		"nvrtx.work=1"

/* The whole token that makes the GPU device a renderer. */
#define STAGE_VK_OFF_TOKEN		"nvrtx.vk=0"

/* The whole token that asks for the video decoder's channel. */
#define STAGE_VIDEO_TOKEN		"nvrtx.video=1"

/* The whole token that breaks the first copy-engine clear of the VRAM on purpose. */
#define STAGE_CLEAR_FAULT_TOKEN		"nvrtx.clear.fault=1"

/* The whole token that has the CPU clear the VRAM, never the copy engine. */
#define STAGE_CLEAR_CPU_TOKEN		"nvrtx.clear.cpu=1"

/*
 * The whole token that asks for the renderer's and the display's counts every
 * 5 s.
 */
#define STAGE_VK_PERF_TOKEN		"nvrtx.vk.perf=1"

/*
 * The whole token that puts every session's work on the one work channel
 * instead of a channel of its own.
 */
#define STAGE_CHANNELS_OFF_TOKEN	"nvrtx.channels=0"

/*
 * The whole token that keeps a session's channel on the GPU when its work
 * did not end (the loss is only reported), to watch what the GSP does.
 */
#define STAGE_KILL_OFF_TOKEN		"nvrtx.kill=0"

/*
 * The whole token that stops the GSP after the takeover instead of keeping the
 * GPU device.
 */
#define STAGE_RESIDENT_OFF_TOKEN	"nvrtx.resident=0"

/* The longest stop or fail token: "nvrtx.stop=" or "nvrtx.fail=" and a name. */
#define STAGE_TOKEN_CAPACITY		32U

static const char *stage_command_line(void);
static bool stage_token_present(const char *token);

/*
 * Reports whether the boot asked the driver to stay out entirely (nvrtx.off=1 or nvrtx=off).
 */
bool
nvrtx_stage_driver_off(void)
{
	bool present;

	/* Looks for either token among the boot parameters. */
	present = stage_token_present(STAGE_OFF_TOKEN);
	if (present)
		return true;

	present = stage_token_present(STAGE_OFF_SHORT_TOKEN);
	if (present)
		return true;

	/* The driver looks at the GPU by default. */
	return false;
}

/*
 * Reports whether the GSP boot path runs: by default, unless the boot says nvrtx.gsp=0.
 */
bool
nvrtx_stage_gsp_wanted(void)
{
	bool present;

	/* Looks for nvrtx.gsp=0 among the boot parameters. */
	present = stage_token_present(STAGE_GSP_OFF_TOKEN);
	if (present)
		return false;

	/* The GSP boot runs by default. */
	return true;
}

/*
 * Reports whether the boot forced the GSP boot path (nvrtx.gsp=1), even
 * where the firmware's picture on the GPU cannot be kept.
 */
bool
nvrtx_stage_gsp_forced(void)
{
	bool present;

	/* Looks for nvrtx.gsp=1 among the boot parameters. */
	present = stage_token_present(STAGE_GSP_TOKEN);
	if (present)
		return true;

	/* Without it the GSP boot keeps the firmware's picture or does not run. */
	return false;
}

/*
 * Reports whether the boot asked for the graphics golden context (nvrtx.gr=1).
 */
bool
nvrtx_stage_gr_wanted(void)
{
	bool present;

	/* Looks for nvrtx.gr=1 among the boot parameters. */
	present = stage_token_present(STAGE_GR_TOKEN);
	if (present)
		return true;

	/* Without it the boot keeps to the copy channel. */
	return false;
}

/*
 * Reports whether the display takeover runs: by default, unless the boot says nvrtx.disp=0.
 */
bool
nvrtx_stage_disp_wanted(void)
{
	bool present;

	/* Looks for nvrtx.disp=0 among the boot parameters. */
	present = stage_token_present(STAGE_DISP_OFF_TOKEN);
	if (present)
		return false;

	/* The takeover runs by default. */
	return true;
}

/*
 * Reports whether the display scans out block-linear images (the default;
 * nvrtx.disp.bl=0 keeps it to linear rows).
 */
bool
nvrtx_stage_disp_bl_wanted(void)
{
	bool present;

	/* Looks for nvrtx.disp.bl=0 among the boot parameters. */
	present = stage_token_present(STAGE_DISP_BL_OFF_TOKEN);
	if (present)
		return false;

	/* Without it the display scans out block-linear images too. */
	return true;
}

/*
 * Reports whether the takeover shows the driver's own surface at once
 * (nvrtx.disp.own=1) instead of the console.
 */
bool
nvrtx_stage_disp_own_wanted(void)
{
	bool present;

	/* Looks for nvrtx.disp.own=1 among the boot parameters. */
	present = stage_token_present(STAGE_DISP_OWN_TOKEN);
	if (present)
		return true;

	/* Without it the takeover shows the console (or is the test). */
	return false;
}

/*
 * Reports whether the takeover is the monitor test (nvrtx.disp.test=1):
 * test surfaces, a flip and a wait at each step instead of the console.
 */
bool
nvrtx_stage_disp_test_wanted(void)
{
	bool present;

	/* Looks for nvrtx.disp.test=1 among the boot parameters. */
	present = stage_token_present(STAGE_DISP_TEST_TOKEN);
	if (present)
		return true;

	/* By default the takeover shows the console and does not wait. */
	return false;
}

/*
 * Reports whether the GPU stays up as a GPU device after the takeover: by
 * default, unless the boot says nvrtx.resident=0.
 */
bool
nvrtx_stage_resident_wanted(void)
{
	bool present;

	/* Looks for nvrtx.resident=0 among the boot parameters. */
	present = stage_token_present(STAGE_RESIDENT_OFF_TOKEN);
	if (present)
		return false;

	/* The GPU device is kept by default. */
	return true;
}

/*
 * Reports whether the boot asked for the first compute launch (nvrtx.compute=1).
 */
bool
nvrtx_stage_compute_wanted(void)
{
	bool present;

	/* Looks for nvrtx.compute=1 among the boot parameters. */
	present = stage_token_present(STAGE_COMPUTE_TOKEN);
	if (present)
		return true;

	/* Without it the boot runs no compute program. */
	return false;
}

/*
 * Reports whether the boot asked for the first draw (nvrtx.draw=1).
 */
bool
nvrtx_stage_draw_wanted(void)
{
	bool present;

	/* Looks for nvrtx.draw=1 among the boot parameters. */
	present = stage_token_present(STAGE_DRAW_TOKEN);
	if (present)
		return true;

	/* Without it the boot draws nothing. */
	return false;
}

/*
 * Reports whether the boot asked for the work channel (nvrtx.work=1), or
 * wants the renderer that runs its work there (the default, nvrtx.vk=0 off).
 */
bool
nvrtx_stage_work_wanted(void)
{
	bool present;

	/* Looks for nvrtx.work=1 among the boot parameters. */
	present = stage_token_present(STAGE_WORK_TOKEN);
	if (present)
		return true;

	/* The renderer needs the channel too. */
	if (nvrtx_stage_vk_wanted())
		return true;

	/* Without it the GPU device has no work channel. */
	return false;
}

/*
 * Reports whether the boot asked for the video decoder's channel
 * (nvrtx.video=1), which the renderer's video decode runs on.
 */
bool
nvrtx_stage_video_wanted(void)
{
	bool present;

	/* Looks for nvrtx.video=1 among the boot parameters. */
	present = stage_token_present(STAGE_VIDEO_TOKEN);
	if (present)
		return true;

	/* Without it the GPU device decodes no video. */
	return false;
}

/*
 * Reports whether the boot asked for the first copy-engine clear of the
 * VRAM to be broken on purpose (nvrtx.clear.fault=1): a test of the way
 * back to the CPU's clears.
 */
bool
nvrtx_stage_clear_fault_wanted(void)
{
	bool present;

	/* Looks for nvrtx.clear.fault=1 among the boot parameters. */
	present = stage_token_present(STAGE_CLEAR_FAULT_TOKEN);
	if (present)
		return true;

	/* Without it every clear is as it should be. */
	return false;
}

/*
 * Reports whether the boot asked for the VRAM to be cleared by the CPU only
 * (nvrtx.clear.cpu=1), as before the copy engine cleared it.
 */
bool
nvrtx_stage_clear_cpu_wanted(void)
{
	bool present;

	/* Looks for nvrtx.clear.cpu=1 among the boot parameters. */
	present = stage_token_present(STAGE_CLEAR_CPU_TOKEN);
	if (present)
		return true;

	/* Without it the copy engine clears the large blobs. */
	return false;
}

/*
 * Reports whether the GPU device is also a renderer: the default, unless the
 * boot turned it off with nvrtx.vk=0.
 */
bool
nvrtx_stage_vk_wanted(void)
{
	bool present;

	/* Looks for nvrtx.vk=0 among the boot parameters. */
	present = stage_token_present(STAGE_VK_OFF_TOKEN);
	if (present)
		return false;

	/* Without it the GPU device is also a renderer. */
	return true;
}

/*
 * Reports whether the boot asked for the renderer's and the display's counts
 * in the kernel log every 5 s (nvrtx.vk.perf=1; off by default).
 */
bool
nvrtx_stage_vk_perf_wanted(void)
{
	bool present;

	/* Looks for nvrtx.vk.perf=1 among the boot parameters. */
	present = stage_token_present(STAGE_VK_PERF_TOKEN);
	if (present)
		return true;

	/* Without it no counts are written. */
	return false;
}

/*
 * Reports whether each session gets a channel of its own (nvrtx.channels=0
 * puts every session's work on the one work channel instead; on by default).
 */
bool
nvrtx_stage_channels_wanted(void)
{
	bool present;

	/* Looks for nvrtx.channels=0 among the boot parameters. */
	present = stage_token_present(STAGE_CHANNELS_OFF_TOKEN);
	if (present)
		return false;

	/* Without it every session gets its own channel. */
	return true;
}

/*
 * Reports whether a session's channel whose work did not end is stopped on
 * the GPU (nvrtx.kill=0 keeps it running and only reports the loss; on by
 * default).
 */
bool
nvrtx_stage_kill_wanted(void)
{
	bool present;

	/* Looks for nvrtx.kill=0 among the boot parameters. */
	present = stage_token_present(STAGE_KILL_OFF_TOKEN);
	if (present)
		return false;

	/* Without it a channel whose work did not end is stopped. */
	return true;
}

/*
 * Reports whether a stage may run; nvrtx.stop=<stage> stops before it.
 *
 * The driver then stops before that stage and every later one.
 */
bool
nvrtx_stage_allowed(
	const char *stage)
{
	char token[STAGE_TOKEN_CAPACITY];
	int length;
	bool present;

	/* Spells the stop token of this stage; a name too long to spell never matches. */
	length = kern_snprintf(token, sizeof(token), "nvrtx.stop=%s", stage);
	if (length < 0 || (size_t)length >= sizeof(token))
		return false;

	/* Looks for the token among the boot parameters. */
	present = stage_token_present(token);
	if (present)
		return false;

	/* Succeeded: nothing stops the stage. */
	return true;
}

/*
 * Reports whether the boot asks the GSP boot to give up at a point on purpose
 * (nvrtx.fail=<point>).
 */
bool
nvrtx_stage_fail_wanted(
	const char *point)
{
	char token[STAGE_TOKEN_CAPACITY];
	int length;
	bool present;

	/* Spells the token of this point; a name too long to spell never matches. */
	length = kern_snprintf(token, sizeof(token), "nvrtx.fail=%s", point);
	if (length < 0 || (size_t)length >= sizeof(token))
		return false;

	/* Looks for the token among the boot parameters. */
	present = stage_token_present(token);
	if (present)
		return true;

	/* The boot gives up only where a program does not answer. */
	return false;
}

/*
 * Writes one stage-mark line to the console and the kernel log.
 *
 * The line reads "nvrtx: <text>".  Whatever does not fit in
 * NVRTX_STAGE_LINE_COLUMNS columns is dropped, so one mark never wraps and
 * pushes the earlier marks off the console.
 */
void
nvrtx_stage_mark(
	const char *format,
	...)
{
	char line[NVRTX_STAGE_LINE_COLUMNS + 1U];
	va_list arguments;
	int prefix;

	/* Writes the driver's prefix. */
	prefix = kern_snprintf(line, sizeof(line), "nvrtx: ");
	if (prefix < 0)
		return;

	/* Writes the text after the prefix; the formatter cuts what does not fit. */
	va_start(arguments, format);
	(void)kern_vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, arguments);
	va_end(arguments);

	/* Publishes the line to the console and the kernel log. */
	kern_logf("%s\n", line);

	/* Succeeded: the stage diagnostic has reached the kernel log. */
	return;
}

/* Finds the raw boot command line, or NULL when the boot gave none. */
static const char *
stage_command_line(void)
{
	const char *line;

	/* Asks the platform for the line the loader handed over. */
	line = kern_boot_handoff(STAGE_COMMAND_LINE_HANDOFF);

	/* Reports the line; the platform keeps owning it. */
	return line;
}

/* Reports whether one whole token is among the boot parameters. */
static bool
stage_token_present(
	const char *token)
{
	const char *line;
	int present;

	/* A boot without a command line holds no token. */
	line = stage_command_line();
	if (line == NULL)
		return false;

	/* Compares the token with each word of the line. */
	present = kern_boot_parameters_token_present(line, token);
	if (present != 0)
		return true;

	/* The token is not on the line. */
	return false;
}
