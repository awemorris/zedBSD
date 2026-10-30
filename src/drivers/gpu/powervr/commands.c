/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR firmware's two command buffers as the host uses them: the
 * kernel's buffer, where the host writes commands and the firmware leaves
 * an answer in each command's return slot, and the firmware's buffer,
 * where the firmware queues notices for the host.
 *
 * Both live in blocks the host reaches through their uncached aliases, so
 * a store is in memory when it completes; a barrier orders the command
 * before the write offset that publishes it, and the offset before the
 * kick.  Callers hold the firmware's lock once the render node runs.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/powervr/powervr-private.h"
#include "drivers/gpu/powervr/interface.h"
#include "drivers/gpu/powervr/vk/vk.h"

/*
 * How many times a wait reads a return slot before it starts sleeping a
 * tick at a time: most commands are answered within microseconds.
 */
#define COMMAND_SPINS		4096U

static uint64_t ms_deadline(unsigned milliseconds);

/*
 * Writes one command into the kernel's buffer and kicks the firmware.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_ccb.c
 * (pvr_kccb_send_cmd_reserved_powered: the command into the slot at the
 * write offset, its return slot cleared, a barrier, the write offset
 * moved, then a kick of the general purpose data master) and pvr_fw.c
 * (pvr_fw_mts_schedule: the kick ordered after the memory writes).
 */
int
powervr_command_send(
	struct powervr_core *core,
	const uint32_t command[POWERVR_COMMAND_WORDS],
	uint32_t *slot)
{
	struct powervr_firmware *firmware;
	uint32_t write;
	uint32_t next;
	uint32_t read;
	uint32_t index;

	/* Refuses a full buffer; the firmware has not read what was written. */
	firmware = &core->firmware;
	write = powervr_object_get32(&firmware->kccb_control, POWERVR_CCB_WRITE);
	read = powervr_object_get32(&firmware->kccb_control, POWERVR_CCB_READ);
	next = (write + 1U) & (POWERVR_KERNEL_CCB_SLOTS - 1U);
	if (next == read)
		return EBUSY;

	/* Writes the command a word at a time and clears its return slot. */
	for (index = 0; index < POWERVR_COMMAND_WORDS; index++)
		powervr_object_put32(&firmware->kccb, write * POWERVR_COMMAND_BYTES + index * 4U, command[index]);

	/*
	 * The return slot was cleared when its last command was answered
	 * (and is zero from the start), so no store to it goes out just
	 * before the kick, where a late one could land over the firmware's
	 * answer; one left set is cleared now, then read back.
	 */
	if (powervr_object_get32(&firmware->kccb_returns, write * 4U) != POWERVR_RETURN_MARKER(write)) {
		powervr_object_put32(&firmware->kccb_returns, write * 4U, POWERVR_RETURN_MARKER(write));
		kern_io_barrier();
		(void)powervr_object_get32(&firmware->kccb_returns, write * 4U);
	}

	/* The firmware's executed count that marks this command done, for when its return slot is not seen. */
	firmware->kccb_sent++;
	firmware->kccb_expect[write] = firmware->kccb_sent;

	/* Publishes the command, then kicks the firmware once it is in memory. */
	kern_io_barrier();
	powervr_object_put32(&firmware->kccb_control, POWERVR_CCB_WRITE, next);
	kern_io_barrier();
	powervr_write32(core, POWERVR_MTS_SCHEDULE, 0);
	kern_io_barrier();

	/* Succeeded: the slot whose return the caller may wait on. */
	*slot = write;
	return 0;
}

/*
 * Waits, bounded, for the firmware to execute the command in a slot, and
 * reports its answer.
 */
int
powervr_command_wait(
	struct powervr_core *core,
	uint32_t slot,
	unsigned milliseconds,
	uint32_t *answer)
{
	struct powervr_firmware *firmware;
	uint64_t deadline;
	uint64_t now;
	uint32_t value;
	uint32_t executed;
	unsigned spins;

	/* Reads the return slot, first in a short spin, then once a tick. */
	firmware = &core->firmware;
	deadline = ms_deadline(milliseconds);
	spins = 0;
	for (;;) {
		value = powervr_object_get32(&firmware->kccb_returns, slot * 4U);
		if ((value & POWERVR_RETURN_EXECUTED) != 0U)
			break;

		/*
		 * The return slot the firmware writes has been seen not to reach
		 * the processor's uncached view on the VF2, while the per-OS
		 * executed count it writes beside it does: a command whose count
		 * the firmware has reached is taken as executed, once noted.
		 */
		executed = powervr_object_get32(&firmware->os_data, POWERVR_OS_DATA_KCCB_EXECUTED);
		if ((int32_t)(executed - firmware->kccb_expect[slot]) >= 0) {
			if (!firmware->return_fallback_noted) {
				if (POWERVR_DEBUG)
					kern_logf("powervr: firmware: slot %u: return 0x%x not seen, executed count %u reached; taken as executed\n",
						  (unsigned)slot, (unsigned)value, (unsigned)executed);
				firmware->return_fallback_noted = true;
				if (POWERVR_DIAGNOSTICS)
					powervr_firmware_return_probe(core, slot);
			}
			value = POWERVR_RETURN_EXECUTED;
			break;
		}

		/* Keeps spinning while the short budget lasts. */
		spins++;
		if (spins < COMMAND_SPINS)
			continue;

		/* The bound is spent without an answer. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Sleeps one tick. */
		sched_sleep(now + 1U);
	}

	/* Clears the slot for its next command, now that its answer is read. */
	powervr_object_put32(&firmware->kccb_returns, slot * 4U, POWERVR_RETURN_MARKER(slot));

	/* Succeeded: the firmware's answer. */
	*answer = value;
	return 0;
}

/*
 * Waits, bounded, for the firmware's executed count to reach a command,
 * for a command whose return slot the firmware does not write (the health
 * check: mainline sends it without waiting on its slot).
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_power.c (the watchdog's
 * health check sent with no slot to wait on) and pvr_rogue_fwif.h
 * (struct rogue_fwif_osdata: kccb_cmds_executed).
 */
int
powervr_command_wait_executed(
	struct powervr_core *core,
	uint32_t slot,
	unsigned milliseconds)
{
	struct powervr_firmware *firmware;
	uint64_t deadline;
	uint64_t now;
	uint32_t executed;

	/* Reads the count once a tick until it reaches the command or the bound is spent. */
	firmware = &core->firmware;
	deadline = ms_deadline(milliseconds);
	for (;;) {
		executed = powervr_object_get32(&firmware->os_data, POWERVR_OS_DATA_KCCB_EXECUTED);
		if ((int32_t)(executed - firmware->kccb_expect[slot]) >= 0) {
			powervr_object_put32(&firmware->kccb_returns, slot * 4U, POWERVR_RETURN_MARKER(slot));
			return 0;
		}

		/* The bound is spent without the count. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Sleeps one tick. */
		sched_sleep(now + 1U);
	}
}

/*
 * Sends one command and waits, bounded, for its answer.
 *
 * A kernel buffer that is full only means the firmware has not read the
 * commands before it yet (the kicks of every session share it), so the
 * send is tried again once a tick within the same bound, as a kick's is,
 * rather than failing the caller's operation (a resource's creation, a
 * flush) at once.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_ccb.c
 * (pvr_kccb_reserve_slot_sync: waits, bounded, for a free slot).
 */
int
powervr_command_run(
	struct powervr_core *core,
	const uint32_t command[POWERVR_COMMAND_WORDS],
	unsigned milliseconds,
	uint32_t *answer)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t slot;
	int error;

	/* Sends the command, waiting while the kernel's buffer is full. */
	deadline = ms_deadline(milliseconds);
	for (;;) {
		error = powervr_command_send(core, command, &slot);
		if (error != EBUSY)
			break;

		/* The bound is spent with the buffer still full. */
		now = sched_ticks();
		if (now >= deadline)
			return ETIMEDOUT;

		/* Sleeps one tick. */
		sched_sleep(now + 1U);
	}

	/* The firmware's buffer refused it for another reason. */
	if (error != 0)
		return error;

	/* Waits for the firmware to execute it. */
	error = powervr_command_wait(core, slot, milliseconds, answer);
	if (error != 0)
		return error;

	/* Succeeded: the command was executed. */
	return 0;
}

/*
 * Asks the firmware to drop what the core's MMU cached of every memory
 * context's tables, and waits for it.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/imagination/pvr_mmu.c (pvr_mmu_flush_exec: the MMU
 * cache command with the cache flags and the sync word, then a bounded
 * wait on its return slot).
 */
int
powervr_command_mmu_flush(
	struct powervr_core *core)
{
	struct powervr_firmware *firmware;
	uint32_t command[POWERVR_COMMAND_WORDS];
	uint32_t answer;
	uint32_t index;
	uint64_t started;
	int error;

	/* Builds the command: every table level and the TLBs, the sync word written with zero. */
	firmware = &core->firmware;
	for (index = 0; index < POWERVR_COMMAND_WORDS; index++)
		command[index] = 0;
	command[POWERVR_COMMAND_TYPE / 4U] = POWERVR_COMMAND_MMU_CACHE;
	command[(POWERVR_COMMAND_DATA + POWERVR_MMU_CACHE_FLAGS) / 4U] = POWERVR_MMU_CACHE_ALL;
	command[(POWERVR_COMMAND_DATA + POWERVR_MMU_CACHE_SYNC) / 4U] = powervr_object_address(&firmware->mmu_sync, 0);
	command[(POWERVR_COMMAND_DATA + POWERVR_MMU_CACHE_VALUE) / 4U] = 0;

	/* Sends it and waits for the firmware to execute it, timed for the submission's timing. */
	started = powervr_timing_now();
	error = powervr_command_run(core, command, POWERVR_COMMAND_WAIT_MS, &answer);
	powervr_timing_add(POWERVR_TIMING_MMU_FLUSH, started);
	if (error != 0)
		return error;

	/* Succeeded: the core holds nothing stale of the tables. */
	return 0;
}

/*
 * Has the firmware write back and drop every line of the system level
 * cache, and waits for it; the core's lock is held.
 */
int
powervr_command_slc_flush(
	struct powervr_core *core)
{
	uint32_t command[POWERVR_COMMAND_WORDS];
	uint32_t answer;
	uint32_t index;
	int error;

	/* The whole cache, invalidated as well. */
	for (index = 0; index < POWERVR_COMMAND_WORDS; index++)
		command[index] = 0;
	command[POWERVR_COMMAND_TYPE / 4U] = POWERVR_COMMAND_SLC_FLUSH;
	command[(POWERVR_COMMAND_DATA + POWERVR_SLC_FLUSH_INVALIDATE) / 4U] = 1U;

	/* Sends it and waits for the firmware to execute it. */
	error = powervr_command_run(core, command, POWERVR_COMMAND_WAIT_MS, &answer);
	if (error != 0)
		return error;

	/* Succeeded: nothing of freed memory stays in the cache. */
	return 0;
}

/*
 * Takes every notice the firmware queued for the host, handing each
 * well-formed one to a callback, and reports how many there were.
 *
 * A notice without the interface's magic is skipped.
 */
unsigned
powervr_notices_take(
	struct powervr_core *core,
	void (*handle)(void *context, uint32_t type, const struct powervr_object *buffer, uint32_t offset),
	void *context)
{
	struct powervr_firmware *firmware;
	uint32_t write;
	uint32_t type;
	uint32_t offset;
	unsigned count;

	/* Reads up to the firmware's write offset, moving the read offset past each notice. */
	firmware = &core->firmware;
	count = 0;
	write = powervr_object_get32(&firmware->fwccb_control, POWERVR_CCB_WRITE) & (POWERVR_FIRMWARE_CCB_SLOTS - 1U);
	while (firmware->fwccb_read != write) {
		offset = firmware->fwccb_read * POWERVR_COMMAND_BYTES;
		type = powervr_object_get32(&firmware->fwccb, offset + POWERVR_COMMAND_TYPE);
		if ((type & POWERVR_COMMAND_MAGIC_MASK) == POWERVR_COMMAND_MAGIC) {
			count++;
			if (handle != NULL)
				handle(context, type, &firmware->fwccb, offset);
		}

		/* Hands the slot back to the firmware. */
		firmware->fwccb_read = (firmware->fwccb_read + 1U) & (POWERVR_FIRMWARE_CCB_SLOTS - 1U);
		powervr_object_put32(&firmware->fwccb_control, POWERVR_CCB_READ, firmware->fwccb_read);
	}

	/* Succeeded: the number of well-formed notices taken. */
	return count;
}

/* Reports the scheduler tick a number of milliseconds from now, at least one tick away. */
static uint64_t
ms_deadline(
	unsigned milliseconds)
{
	uint64_t ticks;

	/* Rounds up, so a short bound still waits one tick. */
	ticks = kern_ms_to_ticks(milliseconds);
	if (ticks == 0U)
		ticks = 1;

	/* Succeeded: the deadline. */
	return sched_ticks() + ticks;
}
