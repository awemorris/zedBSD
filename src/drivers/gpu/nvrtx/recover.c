/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decisions of the renderer's recovery that touch no hardware: which
 * session's number a new channel takes on the runlist, which channel holds
 * up the graphics engine when work does not end, what a channel's stop
 * left of it, and the stop's and the reset's parameters.
 *
 * The numbers are handed out after the one handed out last, wrapping, so a
 * number freed now is taken again only after every other free one: an
 * error report the GSP sends late for a channel just freed does not reach
 * the next channel that would have taken the same number.
 *
 * The graphics engine switches channels only between draws (wait for idle
 * preemption), so a draw that never ends keeps its channel on the engine
 * and every other channel's work waits behind it, though that work may have
 * begun (a channel the engine was taken from mid-run).  The channel the
 * engine holds is the culprit; when the engine cannot be asked, the one
 * whose unfinished run began last is (it took the engine from the others).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-work.h"

/*
 * The stop's and the reset's parameters: their sizes, and the reset's
 * fields (the engine, the subdevice, the reason, whether an error report
 * on the channel is pending).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrla06f/ctrla06fgpfifo.h
 * (NVA06F_CTRL_STOP_CHANNEL_PARAMS) and
 * src/common/sdk/nvidia/inc/ctrl/ctrl906f.h (NV906F_CTRL_CMD_RESET_CHANNEL_PARAMS,
 * NV906F_CTRL_CMD_RESET_CHANNEL_REASON_DEFAULT).
 */
#define RECOVER_STOP_BYTES		1U
#define RECOVER_RESET_ENGINE_AT		0U
#define RECOVER_RESET_SUBDEVICE_AT	4U
#define RECOVER_RESET_REASON_AT		8U
#define RECOVER_RESET_PENDING_AT	12U
#define RECOVER_RESET_BYTES		16U
#define RECOVER_RESET_REASON_DEFAULT	0U

static void recover_put32(uint8_t *bytes, size_t at, uint32_t value);
static int recover_overdue(const struct nvrtx_recover_view *view, uint64_t now_us, uint64_t kill_end_us, uint32_t run_us);

/*
 * Picks the sessions' number (0 to count - 1) a new channel takes: the
 * first free one after last, wrapping, among the bits of used.  Returns
 * the number, or -1 when every one is in use.
 */
int
nvrtx_recover_chid_next(
	uint32_t used,
	uint32_t last,
	uint32_t count)
{
	uint32_t step;
	uint32_t number;

	/* No more numbers than the bits hold. */
	if (count == 0U || count > 32U)
		return -1;

	/* Each number after the last one, wrapping, the last one itself at the end. */
	for (step = 1U; step <= count; step++) {
		number = (last + step) % count;
		if ((used & (1U << number)) == 0U)
			return (int)number;
	}

	/* Every number is in use. */
	return -1;
}

/*
 * Picks the channel that holds up the graphics engine, among count views
 * of the channels, once a run has waited run_us: since its submission, or
 * since the last stop ended (kill_end_us) when that came later.  The
 * channel the engine holds comes first, then the one whose unfinished run
 * began last, then (no run began) the one whose oldest unfinished run was
 * submitted first.  Only a channel that can be stopped is picked.
 *
 * Returns the view's index, or -1 when no run waited that long or none can
 * be stopped.
 */
int
nvrtx_recover_culprit(
	const struct nvrtx_recover_view *views,
	uint32_t count,
	uint64_t now_us,
	uint64_t kill_end_us,
	uint32_t run_us)
{
	const struct nvrtx_recover_view *view;
	uint64_t latest_ns;
	uint64_t oldest_us;
	uint32_t index;
	int overdue;
	int waited;
	int resident;
	int latest;
	int oldest;

	/* Whether any run has waited its time. */
	waited = 0;
	for (index = 0U; index < count; index++) {
		overdue = recover_overdue(&views[index], now_us, kill_end_us, run_us);
		if (overdue != 0)
			waited = 1;
	}

	/* No run has waited long enough: no channel is to blame yet. */
	if (waited == 0)
		return -1;

	/* The channel on the engine, the one whose run began last and the one submitted first, among those that can be stopped. */
	resident = -1;
	latest = -1;
	oldest = -1;
	latest_ns = 0U;
	oldest_us = 0U;
	for (index = 0U; index < count; index++) {
		view = &views[index];

		/* A channel with nothing unfinished, or one that may not be stopped, is passed over. */
		if (view->busy == 0 || view->stoppable == 0)
			continue;

		/* The channel the engine holds. */
		if (view->resident != 0)
			resident = (int)index;

		/* The channel whose run began last. */
		if (view->started != 0 &&
		    (latest < 0 ||
		     view->start_ns > latest_ns)) {
			latest = (int)index;
			latest_ns = view->start_ns;
		}

		/* The channel whose oldest unfinished run was submitted first. */
		if (oldest < 0 || view->oldest_submit_us < oldest_us) {
			oldest = (int)index;
			oldest_us = view->oldest_submit_us;
		}
	}

	/* The engine's own channel when it was asked. */
	if (resident >= 0)
		return resident;

	/* The run that took the engine last. */
	if (latest >= 0)
		return latest;

	/* Succeeded: no run began, so the one waiting longest (or none). */
	return oldest;
}

/*
 * Tells what a channel's stop left: freed when the GPU stopped running it
 * (the stop answered, the GSP reported an error on it, or the reset
 * answered) and the resource manager freed it, stopped when it stopped but
 * the free failed, stuck when nothing showed that it stopped.
 */
uint32_t
nvrtx_recover_kill_state(
	int stopped,
	int reported,
	int reset,
	int freed)
{
	int evidence;

	/* Any sign that the GPU no longer runs the channel. */
	evidence = 0;
	if (stopped != 0 ||
	    reported != 0 ||
	    reset != 0)
		evidence = 1;

	/* Nothing showed it stopped. */
	if (evidence == 0)
		return NVRTX_WORK_CHANNEL_STUCK;

	/* Stopped, though the resource manager keeps it. */
	if (freed == 0)
		return NVRTX_WORK_CHANNEL_STOPPED;

	/* Succeeded: stopped and freed. */
	return NVRTX_WORK_CHANNEL_FREED;
}

/*
 * Reports whether a session's work has stopped on the GPU, for the stop's
 * poll: its channel freed or stopped, or none, or ready with no run of the
 * session's left unfinished (busy).
 */
bool
nvrtx_recover_stopped(
	uint32_t state,
	int busy)
{
	/* A channel the GPU no longer runs, or none. */
	if (state == NVRTX_WORK_CHANNEL_FREED ||
	    state == NVRTX_WORK_CHANNEL_STOPPED ||
	    state == NVRTX_WORK_CHANNEL_NONE)
		return true;

	/* A ready channel whose runs have all ended. */
	if (state == NVRTX_WORK_CHANNEL_READY && busy == 0)
		return true;

	/* Lost, being stopped, stuck, or still running. */
	return false;
}

/*
 * Writes the stop's parameters (stop at once: preempt the channel off its
 * runlist without waiting for it to idle).  Returns their size.
 */
size_t
nvrtx_recover_stop_params(
	uint8_t *params)
{
	/* bImmediate. */
	params[0] = 1U;

	/* Succeeded: one byte. */
	return RECOVER_STOP_BYTES;
}

/*
 * Writes the reset's parameters for the graphics engine of engine_type on
 * the first subdevice, for the default reason, saying whether an error
 * report on the channel is pending.  Returns their size.
 */
size_t
nvrtx_recover_reset_params(
	uint8_t *params,
	uint32_t engine_type,
	int reported)
{
	size_t at;

	/* Every byte not named is zero. */
	for (at = 0U; at < RECOVER_RESET_BYTES; at++)
		params[at] = 0U;

	/* The engine, the subdevice, the reason and the pending report. */
	recover_put32(params, RECOVER_RESET_ENGINE_AT, engine_type);
	recover_put32(params, RECOVER_RESET_SUBDEVICE_AT, 0U);
	recover_put32(params, RECOVER_RESET_REASON_AT, RECOVER_RESET_REASON_DEFAULT);
	if (reported != 0)
		params[RECOVER_RESET_PENDING_AT] = 1U;

	/* Succeeded: the parameters' size. */
	return RECOVER_RESET_BYTES;
}

/* Writes a 32-bit value at a byte offset, low byte first. */
static void
recover_put32(
	uint8_t *bytes,
	size_t at,
	uint32_t value)
{
	/* Each byte, low first. */
	bytes[at] = (uint8_t)value;
	bytes[at + 1U] = (uint8_t)(value >> 8);
	bytes[at + 2U] = (uint8_t)(value >> 16);
	bytes[at + 3U] = (uint8_t)(value >> 24);
}

/* Reports whether a channel's oldest unfinished run has waited run_us since its submission or the last stop's end. */
static int
recover_overdue(
	const struct nvrtx_recover_view *view,
	uint64_t now_us,
	uint64_t kill_end_us,
	uint32_t run_us)
{
	uint64_t since;

	/* A channel with no unfinished run waits for nothing. */
	if (view->busy == 0)
		return 0;

	/* Counted from the later of the two. */
	since = view->oldest_submit_us;
	if (kill_end_us > since)
		since = kill_end_us;
	if (now_us < since || now_us - since < run_us)
		return 0;

	/* Succeeded: it has waited its time. */
	return 1;
}
