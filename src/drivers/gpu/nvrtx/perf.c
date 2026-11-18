/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU's performance level.
 *
 * The resource manager in the GSP chooses the clocks of the GPU's
 * performance states by itself; the driver gives it no advice.  After the
 * work channel is up, this file asks the resource manager which state the
 * GPU is in and writes it into the kernel log, and when the boot asks
 * (nvrtx.boost=1) it asks the resource manager to hold the GPU at its
 * highest state until power-off, then reads the state again.  The
 * controls are the subdevice's performance controls.
 *
 * It runs during the boot's stages, before the message drain starts, so
 * the calls do not share the GSP's queues with the drain.  Nothing here
 * fails the boot: a refused control is only written into the log.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/boot.h>
#include <kern/platform.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-compute.h"
#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The controls: the current performance state, and a boost of it.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080perf.h
 * (NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, NV2080_CTRL_CMD_PERF_BOOST).
 */
#define PERF_CTL_CURRENT_STATE		0x20802068U
#define PERF_CTL_BOOST			0x2080200aU

/*
 * The current state's parameters: one word, a mask with one bit for the state
 * (bit 0 for P0).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080perf.h
 * (NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS, NV2080_CTRL_PERF_PSTATES_P0).
 */
#define PERF_CURRENT_STATE_BYTES	4U
#define PERF_CURRENT_STATE_AT		0U

/*
 * The boost's parameters: the command (to the highest state) and its duration
 * (until it is cleared).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080perf.h
 * (NV2080_CTRL_PERF_BOOST_PARAMS,
 * NV2080_CTRL_PERF_BOOST_FLAGS_CMD_BOOST_TO_MAX,
 * NV2080_CTRL_PERF_BOOST_DURATION_INFINITE).
 */
#define PERF_BOOST_BYTES		8U
#define PERF_BOOST_FLAGS_AT		0U
#define PERF_BOOST_DURATION_AT		4U
#define PERF_BOOST_TO_MAX		2U
#define PERF_BOOST_FOREVER		0xffffffffU

/*
 * The highest state number the mask can name.
 *
 * Value referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080perf.h
 * (NV2080_CTRL_PERF_PSTATES_MAX).
 */
#define PERF_STATE_LAST			15U

/* The boot word that asks for the boost, and where the boot's command line is found. */
#define PERF_BOOST_TOKEN		"nvrtx.boost=1"
#define PERF_COMMAND_LINE_HANDOFF	"boot.command-line"

static int perf_state(struct nvrtx_gpu *gpu, uint32_t *state);
static int perf_boost(struct nvrtx_gpu *gpu);
static bool perf_boost_wanted(void);

/*
 * Writes the GPU's performance state into the kernel log, and holds the
 * GPU at its highest state when the boot asks (nvrtx.boost=1).
 *
 * Called once from the work channel's start, before the message drain.
 */
void
nvrtx_perf_start(
	struct nvrtx_gpu *gpu)
{
	uint32_t state;
	bool wanted;
	int error;

	/* The state the resource manager chose. */
	error = perf_state(gpu, &state);
	if (error != 0) {
		nvrtx_stage_mark("performance: no performance state (%d)", error);
	} else {
		nvrtx_stage_mark("performance: performance state P%u", (unsigned)state);
	}

	/* Leaves the choice to the resource manager unless the boot asks for the boost. */
	wanted = perf_boost_wanted();
	if (!wanted)
		return;

	/* Asks for the highest state until power-off. */
	error = perf_boost(gpu);
	if (error != 0) {
		nvrtx_stage_mark("performance: boost refused (%d)", error);
		return;
	}

	/* The state after the boost. */
	error = perf_state(gpu, &state);
	if (error != 0) {
		nvrtx_stage_mark("performance: boosted, no performance state (%d)", error);
		return;
	}

	/* Succeeded: the log says which state the boost brought. */
	nvrtx_stage_mark("performance: boosted to performance state P%u", (unsigned)state);
}

/* Asks the resource manager for the current performance state's number. */
static int
perf_state(
	struct nvrtx_gpu *gpu,
	uint32_t *state)
{
	uint8_t params[PERF_CURRENT_STATE_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	uint32_t mask;
	uint32_t number;
	int error;

	/* Prepares the query for the subdevice's current state. */
	nvrtx_put32(params, PERF_CURRENT_STATE_AT, 0U);
	mask = 0U;

	/* Holds the GSP's lock from the call to the read of its reply (the one reply buffer). */
	nvrtx_rpc_lock(gpu);

	/* Asks the subdevice for its current state. */
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_SUBDEVICE, PERF_CTL_CURRENT_STATE, params, sizeof(params), &reply, &reply_length);

	/* A reply too short to hold the mask is no reply. */
	if (error == 0 && reply_length < PERF_CURRENT_STATE_BYTES)
		error = EIO;

	/* Reads the mask while the reply is still the GSP's last. */
	if (error == 0)
		mask = nvrtx_get32(reply, PERF_CURRENT_STATE_AT);

	nvrtx_rpc_unlock(gpu);

	/* A call that failed, or a reply too short to hold the mask. */
	if (error != 0)
		return error;

	/* The state is the mask's lowest set bit. */
	for (number = 0U; number <= PERF_STATE_LAST; number++) {
		if ((mask & (1U << number)) != 0U)
			break;
	}

	/* A mask with no state bit names no state. */
	if (number > PERF_STATE_LAST)
		return ENOENT;

	/* Succeeded: the state's number (0 for P0). */
	*state = number;
	return 0;
}

/* Asks the resource manager to hold the GPU at its highest performance state until the boost is cleared. */
static int
perf_boost(
	struct nvrtx_gpu *gpu)
{
	uint8_t params[PERF_BOOST_BYTES];
	const uint8_t *reply;
	size_t reply_length;
	int error;

	/* To the highest state, with no end. */
	nvrtx_put32(params, PERF_BOOST_FLAGS_AT, PERF_BOOST_TO_MAX);
	nvrtx_put32(params, PERF_BOOST_DURATION_AT, PERF_BOOST_FOREVER);
	error = nvrtx_rm_control(gpu, NVRTX_HANDLE_CLIENT, NVRTX_HANDLE_SUBDEVICE, PERF_CTL_BOOST, params, sizeof(params), &reply, &reply_length);
	if (error != 0)
		return error;

	/* Succeeded: the resource manager took the boost. */
	return 0;
}

/* Reports whether the boot asked for the boost (nvrtx.boost=1). */
static bool
perf_boost_wanted(void)
{
	const char *line;
	int present;

	/* A boot without a command line asks for nothing. */
	line = kern_boot_handoff(PERF_COMMAND_LINE_HANDOFF);
	if (line == NULL)
		return false;

	/* Looks for the word among the boot parameters. */
	present = kern_boot_parameters_token_present(line, PERF_BOOST_TOKEN);
	if (present != 0)
		return true;

	/* The word is not on the line. */
	return false;
}
