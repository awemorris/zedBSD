/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pure part of the Raspberry Pi 4 onboard Bluetooth's UART transport
 * (rpi4-bt-link.h): the PL011's divisor, the hunt for an event
 * to fall in step with, the history of the transport's own resets, and the
 * line's mode and generation.
 *
 * The 1.5 % tolerance, the three resets a minute and the hunt's rules are
 * this driver's own choices.
 */

#include "drivers/platform/rpi4/rpi4-bt-link.h"

#include <uapi/errno.h>

/*
 * The H4 type of an event, and the two events an answer to a command
 * comes in.
 *
 * Values referenced: Bluetooth Core Specification 5.4, Vol 4 Part E
 * 7.7.14 and 7.7.15 (the codes and parameters of Command Complete and
 * Command Status).
 */
#define HUNT_TYPE_EVENT			0x04U
#define HUNT_COMMAND_COMPLETE		0x0eU
#define HUNT_COMMAND_STATUS		0x0fU

/* The bytes before an event's parameters: its type, code and length. */
#define HUNT_EVENT_HEADER		3U

/*
 * The parameters' lengths: a Command Status has exactly four (status,
 * credits, opcode), a Command Complete at least four (credits, opcode,
 * and the returned status at least).
 */
#define HUNT_STATUS_LENGTH		4U
#define HUNT_COMPLETE_LENGTH_MIN	4U

/* Where the answered opcode's low byte is: after the credits, or after the status and the credits. */
#define HUNT_COMPLETE_OPCODE		4U
#define HUNT_STATUS_OPCODE		5U

/* What the bytes gathered so far are: not an answer, the start of one, or a whole one. */
#define HUNT_MISMATCH			0
#define HUNT_PARTIAL			1
#define HUNT_COMPLETE			2

static int hunt_judge(const struct drv_rpi4_bt_hunt *hunt, size_t *length);
static int hunt_settle(struct drv_rpi4_bt_hunt *hunt, size_t *length);
static void hunt_drop_first(struct drv_rpi4_bt_hunt *hunt);

/*
 * Works out the PL011's divisor for a baud rate from the clock it divides.
 *
 * Returns 0 with the integer and fractional parts, or EINVAL for a rate of
 * nothing, one past a sixteenth of the clock, or one the divisor cannot
 * come within DRV_RPI4_BT_BAUD_ERROR_PERMILLE of.
 *
 * Values referenced: ARM PrimeCell UART (PL011) Technical Reference Manual
 * (the divisor: a 16-bit integer part and a 6-bit fraction of the clock
 * over sixteen times the baud rate).
 */
int
drv_rpi4_bt_divisor(
	uint32_t clock_hz,
	uint32_t baud,
	uint32_t *integer,
	uint32_t *fraction)
{
	uint64_t divisor;
	uint64_t actual;
	uint64_t difference;

	/* A rate and a clock to divide. */
	if (clock_hz == 0U || baud == 0U)
		return EINVAL;

	/* The PL011 samples each bit sixteen times, so it is no faster than a sixteenth of its clock. */
	if (baud > clock_hz / 16U)
		return EINVAL;

	/*
	 * The divisor in 64ths: clock / (16 * baud) * 64 is clock * 4 / baud,
	 * rounded to the nearest 64th.
	 */
	divisor = ((uint64_t)clock_hz * 4U + (uint64_t)baud / 2U) / (uint64_t)baud;

	/* The integer part takes 16 bits and may not be zero; at its highest the fraction must be zero. */
	if ((divisor >> 6) == 0U)
		return EINVAL;
	if ((divisor >> 6) > 0xffffU)
		return EINVAL;
	if ((divisor >> 6) == 0xffffU && (divisor & 0x3fU) != 0U)
		return EINVAL;

	/* The rate the divisor gives, and how far it is from the one asked for. */
	actual = (uint64_t)clock_hz * 4U / divisor;
	if (actual > baud)
		difference = actual - baud;
	else
		difference = baud - actual;

	/* A line the controller's UART would sample wrongly. */
	if (difference * 1000U > (uint64_t)baud * DRV_RPI4_BT_BAUD_ERROR_PERMILLE)
		return EINVAL;

	/* Succeeded: the two parts the IBRD and FBRD registers take. */
	*integer = (uint32_t)(divisor >> 6);
	*fraction = (uint32_t)(divisor & 0x3fU);
	return 0;
}

/*
 * Reports the fastest line the transport offers: the tree's max-speed (0
 * for none), but no faster than a sixteenth of the clock.
 */
uint32_t
drv_rpi4_bt_speed_limit(
	uint32_t clock_hz,
	uint32_t tree_max)
{
	uint32_t limit;

	/* The PL011's own limit. */
	limit = clock_hz / 16U;

	/* The board's limit, when it is lower. */
	if (tree_max != 0U && tree_max < limit)
		limit = tree_max;

	/* Succeeded: the speed set_speed takes at most. */
	return limit;
}

/* Starts a hunt with nothing gathered, no command known and nothing dropped. */
void
drv_rpi4_bt_hunt_restart(
	struct drv_rpi4_bt_hunt *hunt)
{
	/* Nothing gathered and nothing passed over yet. */
	hunt->have = 0U;
	hunt->dropped = 0U;

	/* No command was sent on the new line yet, so any answer will do. */
	hunt->opcode = 0U;
	hunt->opcode_known = 0U;
}

/*
 * Tells the hunt which command was sent last, whose answer is the only
 * one that may be the anchor from now on.
 */
void
drv_rpi4_bt_hunt_expect(
	struct drv_rpi4_bt_hunt *hunt,
	uint16_t opcode)
{
	/* The answer must name this command. */
	hunt->opcode = opcode;
	hunt->opcode_known = 1U;
}

/*
 * Adds one received byte to the hunt.
 *
 * Returns DRV_RPI4_BT_HUNT_FOUND when the bytes now begin with a whole
 * answer: *length is its length (the type's byte included), and any bytes
 * after it, bytes[*length] to bytes[have - 1], came after it on the line.
 * Returns DRV_RPI4_BT_HUNT_MORE otherwise.
 */
int
drv_rpi4_bt_hunt_byte(
	struct drv_rpi4_bt_hunt *hunt,
	uint8_t byte,
	size_t *length)
{
	int verdict;

	/* A whole buffer can only hold a false candidate: its first byte goes. */
	if (hunt->have == DRV_RPI4_BT_HUNT_MAX)
		hunt_drop_first(hunt);

	/* The byte joins the candidate. */
	hunt->bytes[hunt->have] = byte;
	hunt->have++;

	/* The candidate judged again, dropping its first byte for as long as it cannot be an answer. */
	verdict = hunt_settle(hunt, length);

	/* Reports a whole answer, or a candidate that waits for more. */
	return verdict;
}

/*
 * Gives the candidate up when no byte came for too long, or when a byte
 * came with a receive error (the byte itself is not added): its first
 * byte is dropped and the rest is judged again.  Returns as
 * drv_rpi4_bt_hunt_byte() does.
 */
int
drv_rpi4_bt_hunt_give_up(
	struct drv_rpi4_bt_hunt *hunt,
	size_t *length)
{
	int verdict;

	/* Nothing gathered: nothing to give up. */
	if (hunt->have == 0U)
		return DRV_RPI4_BT_HUNT_MORE;

	/* The candidate's first byte goes. */
	hunt_drop_first(hunt);

	/* What is left is judged again from its start. */
	verdict = hunt_settle(hunt, length);

	/* Reports a whole answer found in the rest, or a candidate that waits for more. */
	return verdict;
}

/* Starts an empty history of the transport's own resets. */
void
drv_rpi4_bt_history_init(
	struct drv_rpi4_bt_history *history)
{
	unsigned index;

	/* No reset yet. */
	for (index = 0U; index < DRV_RPI4_BT_RESETS_MAX; index++)
		history->at_ms[index] = 0U;
	history->count = 0U;
	history->next = 0U;
}

/*
 * Admits one more reset of the transport's own at now_ms.
 *
 * Returns 1 and records it when fewer than DRV_RPI4_BT_RESETS_MAX resets
 * were made within the last DRV_RPI4_BT_RESET_WINDOW_MS; returns 0, and
 * records nothing, when that many were.  Old resets fall out of the window
 * only by time.
 */
int
drv_rpi4_bt_history_admit(
	struct drv_rpi4_bt_history *history,
	uint64_t now_ms)
{
	unsigned recent;
	unsigned index;

	/* The resets still within the window. */
	recent = 0U;
	for (index = 0U; index < history->count; index++) {
		if (now_ms - history->at_ms[index] < DRV_RPI4_BT_RESET_WINDOW_MS)
			recent++;
	}

	/* Too many: the controller is stopped instead. */
	if (recent >= DRV_RPI4_BT_RESETS_MAX)
		return 0;

	/* The reset replaces the oldest time. */
	history->at_ms[history->next] = now_ms;
	history->next = (history->next + 1U) % DRV_RPI4_BT_RESETS_MAX;
	if (history->count < DRV_RPI4_BT_RESETS_MAX)
		history->count++;

	/* Succeeded: the reset is admitted and recorded. */
	return 1;
}

/* Starts a line that was never powered, with an empty history. */
void
drv_rpi4_bt_link_init(
	struct drv_rpi4_bt_link *link)
{
	/* Off, at the first generation. */
	link->mode = DRV_RPI4_BT_MODE_OFF;
	link->generation = 0U;
	drv_rpi4_bt_history_init(&link->history);
}

/*
 * Begins a reset or a change of speed: the line drops every byte until the
 * change ends, and whatever was looked at before belongs to the old line.
 * A stopped line starts again this way.
 */
void
drv_rpi4_bt_link_transition_begin(
	struct drv_rpi4_bt_link *link)
{
	/*
	 * The new generation tells everyone who saw the old one that the line
	 * was reset under them; it wraps, and only equality is tested.
	 */
	link->generation++;
	link->mode = DRV_RPI4_BT_MODE_QUIET;
}

/* Ends a reset or a change of speed: the transport hunts for an answer to fall in step with. */
void
drv_rpi4_bt_link_transition_end(
	struct drv_rpi4_bt_link *link)
{
	/* Only a line in the middle of a change hunts now (a stop meanwhile stays). */
	if (link->mode == DRV_RPI4_BT_MODE_QUIET)
		link->mode = DRV_RPI4_BT_MODE_HUNT;
}

/* Records that an answer was found: the line is in step. */
void
drv_rpi4_bt_link_anchor(
	struct drv_rpi4_bt_link *link)
{
	/* Only a hunting line falls in step. */
	if (link->mode == DRV_RPI4_BT_MODE_HUNT)
		link->mode = DRV_RPI4_BT_MODE_SYNC;
}

/*
 * Decides on a reset of the transport's own, which whoever wants it
 * decided on while it saw the generation observed.
 *
 * Returns DRV_RPI4_BT_SELF_RESET_CANCEL when the line was reset or had its
 * speed changed since (that reset is the one wanted), or is not running;
 * DRV_RPI4_BT_SELF_RESET_GO when the history admits it (the caller then
 * begins the transition); DRV_RPI4_BT_SELF_RESET_STOP when too many were
 * made within the window (the caller then stops the line).
 */
int
drv_rpi4_bt_link_self_reset(
	struct drv_rpi4_bt_link *link,
	uint32_t observed,
	uint64_t now_ms)
{
	int admitted;

	/* Another reset came first: it is the one that was needed. */
	if (link->generation != observed)
		return DRV_RPI4_BT_SELF_RESET_CANCEL;

	/* A line that is not running has nothing to reset. */
	if (link->mode == DRV_RPI4_BT_MODE_OFF)
		return DRV_RPI4_BT_SELF_RESET_CANCEL;
	if (link->mode == DRV_RPI4_BT_MODE_STOPPED)
		return DRV_RPI4_BT_SELF_RESET_CANCEL;

	/* Too many resets within the window stop the controller. */
	admitted = drv_rpi4_bt_history_admit(&link->history, now_ms);
	if (!admitted)
		return DRV_RPI4_BT_SELF_RESET_STOP;

	/* Succeeded: the reset is to be made. */
	return DRV_RPI4_BT_SELF_RESET_GO;
}

/* Stops the line: the controller is powered down until the program resets it. */
void
drv_rpi4_bt_link_stop(
	struct drv_rpi4_bt_link *link)
{
	/* A new generation, so a sender or a gatherer of the old line gives up. */
	link->generation++;
	link->mode = DRV_RPI4_BT_MODE_STOPPED;
}

/*
 * Judges the gathered bytes as the beginning of an answer to the command
 * sent last: HUNT_MISMATCH when they cannot be one, HUNT_PARTIAL when they
 * may become one, HUNT_COMPLETE with its *length when they begin with one.
 */
static int
hunt_judge(
	const struct drv_rpi4_bt_hunt *hunt,
	size_t *length)
{
	const uint8_t *bytes;
	size_t parameters;
	size_t opcode_at;

	/* The candidate's bytes, from its first on. */
	bytes = hunt->bytes;

	/* Nothing gathered may still become anything. */
	if (hunt->have == 0U)
		return HUNT_PARTIAL;

	/* An event, the only type an answer comes in. */
	if (bytes[0] != HUNT_TYPE_EVENT)
		return HUNT_MISMATCH;
	if (hunt->have < 2U)
		return HUNT_PARTIAL;

	/* Command Complete or Command Status, and where each says its command. */
	if (bytes[1] == HUNT_COMMAND_COMPLETE) {
		opcode_at = HUNT_COMPLETE_OPCODE;
	} else if (bytes[1] == HUNT_COMMAND_STATUS) {
		opcode_at = HUNT_STATUS_OPCODE;
	} else {
		/* Any other event is not an answer to a command. */
		return HUNT_MISMATCH;
	}

	/* The parameters' length is still to come. */
	if (hunt->have < 3U)
		return HUNT_PARTIAL;

	/* The parameters' length each of the two events has. */
	parameters = (size_t)bytes[2];
	if (bytes[1] == HUNT_COMMAND_STATUS && parameters != HUNT_STATUS_LENGTH)
		return HUNT_MISMATCH;
	if (bytes[1] == HUNT_COMMAND_COMPLETE && parameters < HUNT_COMPLETE_LENGTH_MIN)
		return HUNT_MISMATCH;

	/* The answered command's low byte, when a command was sent since the reset. */
	if (hunt->have <= opcode_at)
		return HUNT_PARTIAL;
	if (hunt->opcode_known && bytes[opcode_at] != (uint8_t)(hunt->opcode & 0xffU))
		return HUNT_MISMATCH;

	/* Its high byte. */
	if (hunt->have <= opcode_at + 1U)
		return HUNT_PARTIAL;
	if (hunt->opcode_known && bytes[opcode_at + 1U] != (uint8_t)(hunt->opcode >> 8))
		return HUNT_MISMATCH;

	/* The rest of the parameters. */
	if (hunt->have < HUNT_EVENT_HEADER + parameters)
		return HUNT_PARTIAL;

	/* Succeeded: the bytes begin with the whole answer. */
	*length = HUNT_EVENT_HEADER + parameters;
	return HUNT_COMPLETE;
}

/*
 * Drops the candidate's first byte for as long as the gathered bytes
 * cannot begin an answer; reports DRV_RPI4_BT_HUNT_FOUND with the
 * answer's *length, or DRV_RPI4_BT_HUNT_MORE.
 */
static int
hunt_settle(
	struct drv_rpi4_bt_hunt *hunt,
	size_t *length)
{
	int verdict;

	/* Judges the bytes from their start again after each byte dropped. */
	for (;;) {
		verdict = hunt_judge(hunt, length);
		if (verdict != HUNT_MISMATCH)
			break;
		hunt_drop_first(hunt);
	}

	/* A whole answer. */
	if (verdict == HUNT_COMPLETE)
		return DRV_RPI4_BT_HUNT_FOUND;

	/* Succeeded: the bytes may still become an answer. */
	return DRV_RPI4_BT_HUNT_MORE;
}

/* Passes over the candidate's first byte; the rest moves to the start. */
static void
hunt_drop_first(
	struct drv_rpi4_bt_hunt *hunt)
{
	size_t index;

	/* Moves every later byte one place down. */
	for (index = 1U; index < hunt->have; index++)
		hunt->bytes[index - 1U] = hunt->bytes[index];
	hunt->have--;

	/* The byte is counted as passed over. */
	hunt->dropped++;
}
