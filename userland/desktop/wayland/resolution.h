/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The choice of a display's resolution and refresh (resolution.c): the
 * modes offered to the user, the monitor a choice is kept for, and the
 * trial that puts a choice back when the user does not keep it.
 *
 * The display's driver lists the timings it can switch the output to,
 * its default first (VK_KHR_display's modes).  The user is offered the
 * default (marked native, the recommended one), the timings whose sizes
 * are common resolutions (the table in resolution.c), and the mode shown
 * now.  A display offered one mode cannot be changed (a tablet's panel, a
 * driver that does not switch timings).
 *
 * A choice is kept for the monitor, not the connector: its identity is
 * made from the EDID's manufacturer, product and serial number
 * ("edid-v1:DEL:a0b1:0001e240"); a serial number the EDID does not give
 * is made from the serial string when it has one, else the connector's
 * key is added so that two monitors of one model on two ports differ.
 * A display without an EDID is known by its connector's key.
 *
 * A change is a trial: the output is opened again at the new mode
 * (SWITCHING, at most KWL_RESOLUTION_SWITCH_MS until its first frame),
 * then the user has KWL_RESOLUTION_TRIAL_MS to keep it (TRIAL); otherwise
 * the output goes back to the mode it showed when the trial began
 * (REVERTING), or, when that cannot be shown either, to the display's
 * default.  Only a kept trial is written to displays.conf.
 *
 * Nothing here knows the server or Vulkan, so the host tests run it alone.
 */

#ifndef KWL_RESOLUTION_H
#define KWL_RESOLUTION_H

#include <stddef.h>
#include <stdint.h>

/* The most modes a display offers the user, and the most the driver's list is read for. */
#define KWL_RESOLUTION_MODES		64U

/* A mode's marks: the display's default (the recommended one), and the mode shown now. */
#define KWL_RESOLUTION_NATIVE		0x1U
#define KWL_RESOLUTION_CURRENT		0x2U

/* Two refreshes closer than this (mHz) are the same refresh. */
#define KWL_RESOLUTION_SAME_MHZ		50U

/* The longest identity of a monitor, with its terminator (it fits a displays.conf key). */
#define KWL_RESOLUTION_IDENTITY		64U

/* How long a switch may take to its first frame, and how long the user has to keep a new mode (ms). */
#define KWL_RESOLUTION_SWITCH_MS	3000U
#define KWL_RESOLUTION_TRIAL_MS		15000U

/* The trial's states (the same numbers as libkeiland's KL_DISPLAY_RESOLUTION_*). */
#define KWL_RESOLUTION_IDLE		0U
#define KWL_RESOLUTION_SWITCHING	1U
#define KWL_RESOLUTION_TRIAL		2U
#define KWL_RESOLUTION_REVERTING	3U

/*
 * Why a display's resolution cannot be chosen now (the same bits as
 * libkeiland's KL_DISPLAY_RESOLUTION_WHY_*): it offers one mode, another
 * display is shown beside it (or the same desktop on several), a game is
 * shown on it, or it is not the display the desktop is shown on.
 */
#define KWL_RESOLUTION_WHY_FIXED	0x1U
#define KWL_RESOLUTION_WHY_ALONE	0x2U
#define KWL_RESOLUTION_WHY_GAME		0x4U
#define KWL_RESOLUTION_WHY_ANCHOR	0x8U

/*
 * What a step of the trial asks of the compositor: nothing, the output
 * opened at a mode (open), the choice written (save: the mode for the
 * monitor; forget: the monitor's line taken out), and the request that
 * began the trial answered (answer, with answer_error).
 */
#define KWL_RESOLUTION_DO_OPEN		0x1U
#define KWL_RESOLUTION_DO_SAVE		0x2U
#define KWL_RESOLUTION_DO_FORGET	0x4U
#define KWL_RESOLUTION_DO_ANSWER	0x8U

/* One mode of a display: its size, its refresh and its marks. */
struct kwl_resolution_mode {
	uint32_t width;
	uint32_t height;
	uint32_t refresh_mhz;
	unsigned flags;
};

/*
 * The trial of a new mode, one for the session: its state, the display
 * (its connector's key) and the monitor's identity, the mode tried, the
 * mode it goes back to, the display's default (the last resort), when the
 * state ends, whether a reset began it (keeping it forgets the monitor's
 * choice), whether the default is being opened as the last resort, and
 * whether a cancel came while the switch ran.
 */
struct kwl_resolution_trial {
	unsigned state;
	char key[KWL_RESOLUTION_IDENTITY];
	char identity[KWL_RESOLUTION_IDENTITY];
	struct kwl_resolution_mode target;
	struct kwl_resolution_mode back;
	struct kwl_resolution_mode fallback;
	uint64_t deadline_ms;
	unsigned reset;
	unsigned last_resort;
	unsigned cancelled;
};

/*
 * What a step of the trial asks (KWL_RESOLUTION_DO_*): the mode to open,
 * the mode to save, and the error to answer the request with.
 */
struct kwl_resolution_step {
	unsigned what;
	struct kwl_resolution_mode open;
	struct kwl_resolution_mode save;
	int answer_error;
};

int kwl_resolution_same_refresh(uint32_t first, uint32_t second);
int kwl_resolution_common(uint32_t width, uint32_t height);
unsigned kwl_resolution_candidates(const struct kwl_resolution_mode *listed, unsigned count, const struct kwl_resolution_mode *current, struct kwl_resolution_mode *offered, unsigned capacity);
int kwl_resolution_find(const struct kwl_resolution_mode *modes, unsigned count, uint32_t width, uint32_t height, uint32_t refresh_mhz);
int kwl_resolution_pick(const struct kwl_resolution_mode *modes, unsigned count, uint32_t width, uint32_t height, uint32_t refresh_mhz, uint32_t now_mhz);
int kwl_resolution_identity(const uint8_t *edid, size_t length, const char *connector, char *identity, size_t size);
void kwl_resolution_trial_init(struct kwl_resolution_trial *trial);
int kwl_resolution_begin(struct kwl_resolution_trial *trial, const char *key, const char *identity, const struct kwl_resolution_mode *target, const struct kwl_resolution_mode *current, const struct kwl_resolution_mode *fallback, unsigned reset, uint64_t now_ms, struct kwl_resolution_step *step);
void kwl_resolution_shown(struct kwl_resolution_trial *trial, uint64_t now_ms, struct kwl_resolution_step *step);
void kwl_resolution_failed(struct kwl_resolution_trial *trial, uint64_t now_ms, struct kwl_resolution_step *step);
void kwl_resolution_tick(struct kwl_resolution_trial *trial, uint64_t now_ms, struct kwl_resolution_step *step);
int kwl_resolution_keep(struct kwl_resolution_trial *trial, const char *key, struct kwl_resolution_step *step);
int kwl_resolution_revert(struct kwl_resolution_trial *trial, const char *key, uint64_t now_ms, struct kwl_resolution_step *step);
void kwl_resolution_cancel(struct kwl_resolution_trial *trial, uint64_t now_ms, struct kwl_resolution_step *step);
uint32_t kwl_resolution_remaining(const struct kwl_resolution_trial *trial, uint64_t now_ms);

#endif
