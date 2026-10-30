/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pure part of the Raspberry Pi 4 onboard Bluetooth's UART transport
 * (ws215-p003): the PL011's baud divisor, the search for a Command Complete
 * or Command Status event to fall in step with after the line was reset
 * ("hunting"), the history of the transport's own resets, and the state of
 * the line with its generation.
 *
 * Nothing here locks, sleeps, allocates or touches a register; the driver
 * (rpi4-bt.c) holds its spinlock around the line's state, and the host test
 * builds this file as it is.
 */

#ifndef KERN_DRIVERS_RPI4_BT_LINK_H
#define KERN_DRIVERS_RPI4_BT_LINK_H

#include <stddef.h>
#include <stdint.h>

/* The most a baud rate may differ from the one asked for, in thousandths (1.5 %). */
#define DRV_RPI4_BT_BAUD_ERROR_PERMILLE	15U

/* The bytes a hunted event may take: the type's byte, its code and length, and 255 bytes of parameters, rounded up. */
#define DRV_RPI4_BT_HUNT_MAX		260U

/* What one hunted byte leaves: an event still being looked for, or one found. */
#define DRV_RPI4_BT_HUNT_MORE		0
#define DRV_RPI4_BT_HUNT_FOUND		1

/* How many of its own resets the transport makes within the window before it stops. */
#define DRV_RPI4_BT_RESETS_MAX		3U
#define DRV_RPI4_BT_RESET_WINDOW_MS	60000U

/*
 * What the transport decides on a reset of its own: give it up (the line
 * was reset meanwhile, or is not running), make it, or stop the controller
 * (too many of them within the window).
 */
#define DRV_RPI4_BT_SELF_RESET_CANCEL	0
#define DRV_RPI4_BT_SELF_RESET_GO	1
#define DRV_RPI4_BT_SELF_RESET_STOP	2

/*
 * Where the line is.
 *
 * OFF: the controller was never powered.  QUIET: the line is being reset
 * or its speed changed, and every byte is dropped.  HUNT: the transport
 * looks for an answer to a command to fall in step with.  SYNC: in step,
 * every packet goes to the class.  STOPPED: the controller is powered
 * down after too many resets, until the program resets it.
 */
enum drv_rpi4_bt_mode {
	DRV_RPI4_BT_MODE_OFF = 0,
	DRV_RPI4_BT_MODE_QUIET = 1,
	DRV_RPI4_BT_MODE_HUNT = 2,
	DRV_RPI4_BT_MODE_SYNC = 3,
	DRV_RPI4_BT_MODE_STOPPED = 4
};

/*
 * The bytes gathered while the transport hunts for an event to fall in
 * step with.
 *
 * Only a Command Complete or Command Status event can be the anchor,
 * because the program sends a command after every reset and its answer is
 * the first thing the controller says.  bytes holds the candidate from its
 * first byte on; a byte that cannot continue it drops the candidate's first
 * byte only, and the rest is judged again from its start, so a real event
 * that began inside a false one is still found.  opcode is the last command
 * sent since the line was reset (opcode_known 0 before any), which the
 * answer must name.  dropped counts the bytes passed over.
 */
struct drv_rpi4_bt_hunt {
	uint8_t bytes[DRV_RPI4_BT_HUNT_MAX];
	size_t have;
	uint16_t opcode;
	unsigned opcode_known;
	uint64_t dropped;
};

/*
 * The times of the transport's last resets of its own, in milliseconds,
 * oldest replaced first.  The program's reset does not empty it, so a line
 * that keeps falling out of step stops again soon after it was restarted.
 */
struct drv_rpi4_bt_history {
	uint64_t at_ms[DRV_RPI4_BT_RESETS_MAX];
	unsigned count;
	unsigned next;
};

/*
 * The line's state: its mode, and the generation that every reset and
 * every change of speed moves on.
 *
 * Whoever looked at the line before letting its lock go compares the
 * generation it saw with the one now: a different one means the line was
 * reset meanwhile, and what was gathered or decided before belongs to the
 * old line.  The generation wraps; only equality is ever tested.
 */
struct drv_rpi4_bt_link {
	enum drv_rpi4_bt_mode mode;
	uint32_t generation;
	struct drv_rpi4_bt_history history;
};

int drv_rpi4_bt_divisor(uint32_t clock_hz, uint32_t baud, uint32_t *integer, uint32_t *fraction);
uint32_t drv_rpi4_bt_speed_limit(uint32_t clock_hz, uint32_t tree_max);

void drv_rpi4_bt_hunt_restart(struct drv_rpi4_bt_hunt *hunt);
void drv_rpi4_bt_hunt_expect(struct drv_rpi4_bt_hunt *hunt, uint16_t opcode);
int drv_rpi4_bt_hunt_byte(struct drv_rpi4_bt_hunt *hunt, uint8_t byte, size_t *length);
int drv_rpi4_bt_hunt_give_up(struct drv_rpi4_bt_hunt *hunt, size_t *length);

void drv_rpi4_bt_history_init(struct drv_rpi4_bt_history *history);
int drv_rpi4_bt_history_admit(struct drv_rpi4_bt_history *history, uint64_t now_ms);

void drv_rpi4_bt_link_init(struct drv_rpi4_bt_link *link);
void drv_rpi4_bt_link_transition_begin(struct drv_rpi4_bt_link *link);
void drv_rpi4_bt_link_transition_end(struct drv_rpi4_bt_link *link);
void drv_rpi4_bt_link_anchor(struct drv_rpi4_bt_link *link);
int drv_rpi4_bt_link_self_reset(struct drv_rpi4_bt_link *link, uint32_t observed, uint64_t now_ms);
void drv_rpi4_bt_link_stop(struct drv_rpi4_bt_link *link);

#endif
