/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Calls to the GSP's resource manager, and the events it sends.
 *
 * A call is put in the command queue and the GSP is told by its doorbell.
 * Its reply comes back in the status queue with the same function and the
 * caller's sequence number; the reply is matched on both.  While
 * the driver waits, the GSP may send events: the CPU sequencer (a list of
 * register operations the driver performs at once) and others, which are
 * reported and dropped in this revision.  One call is made at a time:
 * until the device's threads run only the worker calls, and from then on
 * every call and each turn of the message drain hold the GSP's lock
 * (rpc_lock).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The GSP's doorbell for the command queue.
 *
 * Value referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c (r535_gsp_cmdq_push).
 */
#define RPC_REG_DOORBELL		(NVRTX_FALCON_GSP_BASE + 0xc00U)

/*
 * The GSP falcon's CPU control and its halted bit, which the sequencer waits
 * for.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_msg_run_cpu_sequencer).
 */
#define RPC_GSP_CPU_CONTROL		(NVRTX_FALCON_GSP_BASE + 0x100U)
#define RPC_CPU_HALTED			0x10U

/*
 * What the resume of the GSP's core uses: the GSP falcon's two mailboxes
 * (the start page's address) and its application-version register, the
 * secure processor's mailbox 0 (its result), and the secure scratch register
 * whose bit 26 says the boot has handed over to the firmware.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_msg_run_cpu_sequencer).
 */
#define RPC_GSP_MAILBOX0		(NVRTX_FALCON_GSP_BASE + 0x040U)
#define RPC_GSP_MAILBOX1		(NVRTX_FALCON_GSP_BASE + 0x044U)
#define RPC_GSP_APP_VERSION		(NVRTX_FALCON_GSP_BASE + 0x080U)
#define RPC_SEC2_MAILBOX0		(NVRTX_FALCON_SEC2_BASE + 0x040U)
#define RPC_BOOT_SCRATCH		0x1180f8U
#define RPC_BOOT_HANDED_OVER		0x04000000U
#define RPC_RESUME_TIMEOUT_US		2000000U

/* How long a send waits for room, how long a poll command waits by default, and the step. */
#define RPC_SEND_TIMEOUT_US		1000000U
#define RPC_POLL_DEFAULT_US		4000000U
#define RPC_HALT_TIMEOUT_US		2000000U
#define RPC_STEP_US			10U

/*
 * The firmware error report's fields: its kind, and its text of up to 256
 * bytes, of which the line shows what fits an 80-column screen after its
 * prefix ("Graphics Exception: Shader Program Header 17 Error" whole).
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/generated/g_rpc-structures.h (rpc_os_error_log_v17_00).
 */
#define RPC_ERROR_LOG_KIND_AT		0U
#define RPC_ERROR_LOG_TEXT_AT		12U
#define RPC_ERROR_LOG_TEXT_BYTES	256U
#define RPC_ERROR_LOG_SHOWN		54U

/*
 * The channel-error event's fields: engine, channel, kind of exception,
 * faulting address and fault kind.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/nvidia/generated/g_rpc-structures.h (rpc_rc_triggered_v17_02).
 */
#define RPC_ERROR_ENGINE_AT		0U
#define RPC_ERROR_CHANNEL_AT		4U
#define RPC_ERROR_TYPE_AT		16U
#define RPC_ERROR_FAULT_LOW_AT		28U
#define RPC_ERROR_FAULT_HIGH_AT		32U
#define RPC_ERROR_FAULT_KIND_AT		36U
#define RPC_ERROR_BYTES			40U

/*
 * A graphics class error (a method the 3D engine refused): its kind, and the
 * engine's registers that keep the refused method, its data and the error's
 * code, and the class bound to each subchannel.
 *
 * Values referenced: NVIDIA open-gpu-kernel-modules 570.144
 * src/common/sdk/nvidia/inc/nverror.h (ROBUST_CHANNEL_GR_CLASS_ERROR) and
 * Linux v6.19 drivers/gpu/drm/nouveau/nvkm/engine/gr/gf100.c (gf100_gr_intr:
 * 0x400100, 0x400704, 0x400708, 0x400110, 0x404200).
 */
#define RPC_GR_CLASS_ERROR		69U
#define RPC_GR_STATUS			0x400100U
#define RPC_GR_TRAPPED_ADDR		0x400704U
#define RPC_GR_TRAPPED_DATA		0x400708U
#define RPC_GR_ERROR_CODE		0x400110U
#define RPC_GR_SUBCHANNEL_CLASS		0x404200U

static int rpc_sequencer(struct nvrtx_gpu *gpu, uint8_t *payload, size_t length);
static void rpc_error_log(const uint8_t *payload, size_t length);
static void rpc_gr_class_error(struct nvrtx_gpu *gpu);
static int rpc_core_resume(struct nvrtx_gpu *gpu);
static int rpc_exchange(struct nvrtx_gpu *gpu, uint32_t function, size_t call_length, size_t *reply_length);
static int rpc_allocate(struct nvrtx_gpu *gpu, uint32_t client, uint32_t parent, uint32_t handle, uint32_t class_number, const uint8_t *parameters, size_t length);
static int rpc_control(struct nvrtx_gpu *gpu, uint32_t client, uint32_t object, uint32_t command, const uint8_t *parameters, size_t length, const uint8_t **reply, size_t *reply_length);
static int rpc_free(struct nvrtx_gpu *gpu, uint32_t client, uint32_t object);
static bool rpc_hold(struct nvrtx_gpu *gpu);
static void rpc_release(struct nvrtx_gpu *gpu, bool taken);

/*
 * Puts one call in the command queue and rings the doorbell.
 *
 * A call that waits for its reply takes the next sequence number; one that
 * does not (sent before the GSP runs) takes zero.  Returns ETIMEDOUT when the
 * queue had no room for a second.
 */
int
nvrtx_rpc_send(
	struct nvrtx_gpu *gpu,
	uint32_t function,
	const uint8_t *payload,
	size_t length,
	int with_sequence,
	uint32_t *sequence)
{
	uint64_t start;
	uint64_t now;
	int error;

	/*
	 * Numbers the call: a call that waits for a reply takes the next number
	 * from zero, one sent before the GSP runs takes zero without using a
	 * number.
	 *
	 * Procedure referenced: Linux v6.19
	 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c
	 * (r535_gsp_rpc_send).
	 */
	*sequence = 0U;
	if (with_sequence != 0) {
		*sequence = gpu->call_sequence;
		gpu->call_sequence++;
	}

	/* Puts the call in the queue, waiting for room while the GSP reads. */
	start = nvrtx_now_us();
	for (;;) {
		error = nvrtx_channel_send(&gpu->channel, function, *sequence, payload, length);
		if (error != EAGAIN)
			break;

		/* Gives up when the time, measured, is spent. */
		now = nvrtx_now_us();
		if (now - start > RPC_SEND_TIMEOUT_US)
			break;

		kern_usleep_range(RPC_STEP_US, RPC_STEP_US * 2U);
	}

	/* A full queue that never drained, or a call too large. */
	if (error == EAGAIN)
		return ETIMEDOUT;
	if (error != 0)
		return error;

	/* Tells the GSP there is a call. */
	nvrtx_wr32(gpu, RPC_REG_DOORBELL, 0U);

	/* Succeeded: the call is queued. */
	return 0;
}

/*
 * Waits for the reply (or event) of a function, serving other events meanwhile.
 *
 * The reply is matched on its function alone; the sequence is kept for the
 * marks.  The payload is left in gpu->reply with its length.  Returns EIO when
 * the reply carries an error result, ETIMEDOUT when it did not come in time.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/rpc.c (r535_gsp_msg_recv,
 * r535_gsp_rpc_poll).
 */
int
nvrtx_rpc_wait(
	struct nvrtx_gpu *gpu,
	uint32_t function,
	uint32_t sequence,
	uint32_t timeout_us,
	size_t *length)
{
	uint32_t got_function;
	uint32_t got_sequence;
	uint32_t result;
	uint64_t start;
	uint64_t now;
	int error;

	/* Takes messages until the one waited for arrives or the time is spent. */
	start = nvrtx_now_us();
	for (;;) {
		/* Ends when the time is spent, measured: a short sleep lasts a whole tick. */
		now = nvrtx_now_us();
		if (now - start > timeout_us)
			break;

		/* Waits a step when nothing is there. */
		error = nvrtx_channel_peek(&gpu->channel, &got_function, &got_sequence, length, &result);
		if (error == EAGAIN) {
			kern_usleep_range(RPC_STEP_US, RPC_STEP_US * 2U);
			continue;
		}

		if (error != 0)
			return error;

		/* Takes the message. */
		error = nvrtx_channel_receive(&gpu->channel, gpu->reply, NVRTX_STAGING_BYTES, length);
		if (error != 0)
			return error;

		/* The message waited for ends the wait. */
		if (got_function == function) {
			/* A reply with another number is shown, not refused. */
			if (sequence != 0U && got_sequence != sequence)
				nvrtx_stage_mark("rpc %u: reply sequence %u, sent %u", (unsigned)function, (unsigned)got_sequence, (unsigned)sequence);

			if (result != 0U) {
				nvrtx_stage_mark("rpc %u: result %08x", (unsigned)function, (unsigned)result);
				return EIO;
			}

			return 0;
		}

		/* The CPU sequencer is performed at once. */
		if (got_function == NVRTX_EVENT_SEQUENCER) {
			nvrtx_stage_mark("rpc: sequencer of %zu bytes", *length);
			error = rpc_sequencer(gpu, gpu->reply, *length);
			if (error != 0)
				return error;

			continue;
		}

		/* The firmware's own error report is shown with its kind and text. */
		if (got_function == NVRTX_EVENT_ERROR_LOG) {
			rpc_error_log(gpu->reply, *length);
			continue;
		}

		/*
		 * A channel error is reported with its channel, its kind and
		 * the faulting address (channel-stop).
		 */
		if (got_function == NVRTX_EVENT_CHANNEL_ERROR && *length >= RPC_ERROR_BYTES) {
			nvrtx_stage_mark("channel-stop channel error: eng %x chid %u type %u fault %08x%08x/%u",
			    (unsigned)nvrtx_get32(gpu->reply, RPC_ERROR_ENGINE_AT),
			    (unsigned)nvrtx_get32(gpu->reply, RPC_ERROR_CHANNEL_AT),
			    (unsigned)nvrtx_get32(gpu->reply, RPC_ERROR_TYPE_AT),
			    (unsigned)nvrtx_get32(gpu->reply, RPC_ERROR_FAULT_HIGH_AT),
			    (unsigned)nvrtx_get32(gpu->reply, RPC_ERROR_FAULT_LOW_AT),
			    (unsigned)nvrtx_get32(gpu->reply, RPC_ERROR_FAULT_KIND_AT));
			if (nvrtx_get32(gpu->reply, RPC_ERROR_TYPE_AT) == RPC_GR_CLASS_ERROR)
				rpc_gr_class_error(gpu);

			/* Keeps the record of the notices, which a channel's bring-up reads. */
			gpu->rc_notices++;
			gpu->rc_last_chid = nvrtx_get32(gpu->reply, RPC_ERROR_CHANNEL_AT);
			gpu->rc_last_type = nvrtx_get32(gpu->reply, RPC_ERROR_TYPE_AT);

			/* The renderer loses the channel's session. */
			if (gpu->channel_error != NULL)
				gpu->channel_error(gpu->channel_error_argument, nvrtx_get32(gpu->reply, RPC_ERROR_CHANNEL_AT), nvrtx_get32(gpu->reply, RPC_ERROR_TYPE_AT));
			continue;
		}

		/*
		 * The firmware's prints, bridge updates and crash records are
		 * dropped unseen.
		 */
		if (got_function == NVRTX_EVENT_LIBOS_PRINT ||
		    got_function == NVRTX_EVENT_BRIDGELESS ||
		    got_function == NVRTX_EVENT_POST_NOCAT)
			continue;

		/* Other events are reported and dropped. */
		nvrtx_stage_mark("rpc: message %x (%zu bytes) while waiting for %x", (unsigned)got_function, *length, (unsigned)function);
	}

	/* The message never came. */
	return ETIMEDOUT;
}

/*
 * Allocates one object under the driver's client and checks the status the firmware returns.
 */
int
nvrtx_rm_allocate(
	struct nvrtx_gpu *gpu,
	uint32_t parent,
	uint32_t handle,
	uint32_t class_number,
	const uint8_t *parameters,
	size_t length)
{
	int error;

	/* The driver's own client. */
	error = nvrtx_rm_allocate_in(gpu, NVRTX_HANDLE_CLIENT, parent, handle, class_number, parameters, length);
	if (error != 0)
		return error;

	/* Succeeded: the object exists. */
	return 0;
}

/*
 * Allocates one object under a given client and checks the status the firmware
 * returns; the call takes the GSP's lock unless its caller holds it.
 */
int
nvrtx_rm_allocate_in(
	struct nvrtx_gpu *gpu,
	uint32_t client,
	uint32_t parent,
	uint32_t handle,
	uint32_t class_number,
	const uint8_t *parameters,
	size_t length)
{
	bool taken;
	int error;

	/* The call and its reply under the GSP's lock. */
	taken = rpc_hold(gpu);
	error = rpc_allocate(gpu, client, parent, handle, class_number, parameters, length);
	rpc_release(gpu, taken);
	if (error != 0)
		return error;

	/* Succeeded: the object exists. */
	return 0;
}

/*
 * Sends a control to an object and checks the status the firmware returns;
 * the call takes the GSP's lock unless its caller holds it.
 *
 * The reply's parameters are left at *reply (inside gpu->reply) with their
 * length: a caller that reads them holds the lock (nvrtx_rpc_lock()) from
 * before the call until it has read them, once other threads may call.
 */
int
nvrtx_rm_control(
	struct nvrtx_gpu *gpu,
	uint32_t client,
	uint32_t object,
	uint32_t command,
	const uint8_t *parameters,
	size_t length,
	const uint8_t **reply,
	size_t *reply_length)
{
	bool taken;
	int error;

	/* The call and its reply under the GSP's lock. */
	taken = rpc_hold(gpu);
	error = rpc_control(gpu, client, object, command, parameters, length, reply, reply_length);
	rpc_release(gpu, taken);
	if (error != 0)
		return error;

	/* Succeeded: the reply's parameters are at *reply. */
	return 0;
}

/*
 * Frees one object of the driver's client.
 */
int
nvrtx_rm_free(
	struct nvrtx_gpu *gpu,
	uint32_t object)
{
	int error;

	/* The driver's own client. */
	error = nvrtx_rm_free_in(gpu, NVRTX_HANDLE_CLIENT, object);
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
}

/*
 * Frees one object of a given client (the client itself when object is the
 * client's handle); the call takes the GSP's lock unless its caller holds it.
 */
int
nvrtx_rm_free_in(
	struct nvrtx_gpu *gpu,
	uint32_t client,
	uint32_t object)
{
	bool taken;
	int error;

	/* The call and its reply under the GSP's lock. */
	taken = rpc_hold(gpu);
	error = rpc_free(gpu, client, object);
	rpc_release(gpu, taken);
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
}

/*
 * Takes the GSP's lock for a run of calls whose replies the caller reads,
 * or for one turn of reading the GSP's messages.  The caller is counted as
 * waiting until it holds the lock, so the message drain gives it up.
 */
void
nvrtx_rpc_lock(
	struct nvrtx_gpu *gpu)
{
	/* One more thread waits: the drain yields after its turn while any does. */
	(void)__atomic_add_fetch(&gpu->rpc_wanted, 1U, __ATOMIC_ACQ_REL);

	/* Sleeps until the lock's holder gives it up. */
	mutex_lock(&gpu->rpc_lock);

	/* The thread holds it and no longer waits. */
	(void)__atomic_sub_fetch(&gpu->rpc_wanted, 1U, __ATOMIC_ACQ_REL);
}

/* Gives the GSP's lock back. */
void
nvrtx_rpc_unlock(
	struct nvrtx_gpu *gpu)
{
	mutex_unlock(&gpu->rpc_lock);
}

/*
 * Sends the call built in gpu->call and waits for its numbered reply.
 *
 * Once a reply has failed to come in time, the firmware is taken as not
 * answering: every later call fails at once with ETIMEDOUT, so the teardown
 * of many objects that follows (channel-stop, the reboot's unload) does not
 * wait the whole time for each.
 */
static int
rpc_exchange(
	struct nvrtx_gpu *gpu,
	uint32_t function,
	size_t call_length,
	size_t *reply_length)
{
	uint32_t sequence;
	int error;

	/* A firmware that stopped answering is not asked again. */
	if (gpu->rpc_unanswered != 0)
		return ETIMEDOUT;

	/* Sends the call with the next sequence number. */
	error = nvrtx_rpc_send(gpu, function, gpu->call, call_length, 1, &sequence);
	if (error != 0)
		return error;

	/* Waits for the reply with the same number; a reply that never came marks the firmware as not answering. */
	error = nvrtx_rpc_wait(gpu, function, sequence, NVRTX_CALL_TIMEOUT_US, reply_length);
	if (error == ETIMEDOUT) {
		gpu->rpc_unanswered = 1;
		nvrtx_stage_mark("rpc %u: no reply in time; later calls are not waited for", (unsigned)function);
		return error;
	}

	/* Any other failure of the wait. */
	if (error != 0)
		return error;

	/* Succeeded: the reply is in gpu->reply. */
	return 0;
}

/*
 * Allocates one object under a given client and checks the status the firmware
 * returns.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/alloc.c
 * (r535_gsp_rpc_rm_alloc_get, r535_gsp_rpc_rm_alloc_push).
 */
static int
rpc_allocate(
	struct nvrtx_gpu *gpu,
	uint32_t client,
	uint32_t parent,
	uint32_t handle,
	uint32_t class_number,
	const uint8_t *parameters,
	size_t length)
{
	struct nvrtx_object object;
	size_t call_length;
	size_t reply_length;
	uint32_t status;
	int error;

	/* Builds the call. */
	object.client = client;
	object.parent = parent;
	object.handle = handle;
	object.class_number = class_number;
	call_length = nvrtx_alloc_call(gpu->call, NVRTX_CALL_PAYLOAD_MAX, &object, parameters, length);
	if (call_length == 0U)
		return E2BIG;

	/* Sends it and waits for the reply. */
	error = rpc_exchange(gpu, NVRTX_CALL_ALLOCATE, call_length, &reply_length);
	if (error != 0)
		return error;

	/* The reply's status is the resource manager's verdict. */
	if (reply_length < NVRTX_ALLOC_HEADER_BYTES)
		return EIO;
	status = nvrtx_get32(gpu->reply, NVRTX_ALLOC_STATUS_AT);
	if (status != 0U) {
		nvrtx_stage_mark("rm: class %04x object %08x status %08x", (unsigned)class_number, (unsigned)handle, (unsigned)status);
		return EIO;
	}

	/* Succeeded: the object exists. */
	return 0;
}

/*
 * Sends a control to an object and checks the status the firmware returns.
 *
 * The reply's parameters are left at *reply (inside gpu->reply) with their
 * length.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/ctrl.c
 * (r535_gsp_rpc_rm_ctrl_get, r535_gsp_rpc_rm_ctrl_push).
 */
static int
rpc_control(
	struct nvrtx_gpu *gpu,
	uint32_t client,
	uint32_t object,
	uint32_t command,
	const uint8_t *parameters,
	size_t length,
	const uint8_t **reply,
	size_t *reply_length)
{
	size_t call_length;
	size_t got;
	uint32_t status;
	int error;

	/* Builds the call. */
	call_length = nvrtx_control_call(gpu->call, NVRTX_CALL_PAYLOAD_MAX, client, object, command, parameters, length);
	if (call_length == 0U)
		return E2BIG;

	/* Sends it and waits for the reply. */
	error = rpc_exchange(gpu, NVRTX_CALL_CONTROL, call_length, &got);
	if (error != 0)
		return error;

	/* The reply's status is the resource manager's verdict. */
	if (got < NVRTX_CONTROL_HEADER_BYTES)
		return EIO;
	status = nvrtx_get32(gpu->reply, NVRTX_CONTROL_STATUS_AT);
	if (status != 0U) {
		nvrtx_stage_mark("rm: control %08x status %08x", (unsigned)command, (unsigned)status);
		return EIO;
	}

	/* Succeeded: the reply's parameters follow its header. */
	*reply = gpu->reply + NVRTX_CONTROL_HEADER_BYTES;
	*reply_length = got - NVRTX_CONTROL_HEADER_BYTES;
	return 0;
}

/*
 * Frees one object of a given client (the client itself when object is the
 * client's handle).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/alloc.c
 * (r535_gsp_rpc_rm_free).
 */
static int
rpc_free(
	struct nvrtx_gpu *gpu,
	uint32_t client,
	uint32_t object)
{
	size_t reply_length;
	int error;

	/* Builds the call and waits for its reply. */
	nvrtx_free_params(gpu->call, client, object);
	error = rpc_exchange(gpu, NVRTX_CALL_FREE, NVRTX_FREE_PARAMS_BYTES, &reply_length);
	if (error != 0)
		return error;

	/* Succeeded: the object is gone. */
	return 0;
}

/*
 * Takes the GSP's lock for one call unless the caller holds it already (a
 * run of calls whose replies it reads).  Returns whether it was taken here.
 */
static bool
rpc_hold(
	struct nvrtx_gpu *gpu)
{
	int owned;

	/* A caller holding the lock keeps it. */
	owned = mutex_owned(&gpu->rpc_lock);
	if (owned != 0)
		return false;

	/* Takes it for the call. */
	nvrtx_rpc_lock(gpu);

	/* Succeeded: the call gives it back. */
	return true;
}

/* Gives the GSP's lock back when the call took it. */
static void
rpc_release(
	struct nvrtx_gpu *gpu,
	bool taken)
{
	/* A caller's own hold stays. */
	if (!taken)
		return;

	/* The call's hold ends. */
	nvrtx_rpc_unlock(gpu);
}

/*
 * Performs the register operations of one CPU sequencer message, in order.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_msg_run_cpu_sequencer).
 */
static int
rpc_sequencer(
	struct nvrtx_gpu *gpu,
	uint8_t *payload,
	size_t length)
{
	struct nvrtx_seq_command command;
	uint32_t cursor;
	uint32_t current;
	uint32_t timeout;
	int error;

	/* Decodes and performs each command until the list is done. */
	cursor = 0U;
	for (;;) {
		error = nvrtx_sequencer_next(payload, length, &cursor, &command);
		if (error == ENOENT)
			break;
		if (error != 0) {
			nvrtx_stage_mark("sequencer: bad command at %u", (unsigned)cursor);
			return error;
		}

		/* Performs the command. */
		switch (command.opcode) {
		case NVRTX_SEQ_WRITE:
			nvrtx_wr32(gpu, command.argument[0], command.argument[1]);
			break;
		case NVRTX_SEQ_MODIFY:
			current = nvrtx_rd32(gpu, command.argument[0]);
			nvrtx_wr32(gpu, command.argument[0], (current & ~command.argument[1]) | command.argument[2]);
			break;
		case NVRTX_SEQ_POLL:
			timeout = command.argument[3];
			if (timeout == 0U)
				timeout = RPC_POLL_DEFAULT_US;
			(void)nvrtx_rd32(gpu, command.argument[0]);
			(void)nvrtx_poll(gpu, command.argument[0], command.argument[1], command.argument[2], timeout);
			break;
		case NVRTX_SEQ_DELAY:
			kern_usleep_range(command.argument[0], command.argument[0] + 1U);
			break;
		case NVRTX_SEQ_STORE:
			current = nvrtx_rd32(gpu, command.argument[0]);
			error = nvrtx_sequencer_save(payload, length, command.argument[1], current);
			if (error != 0)
				return error;
			break;
		case NVRTX_SEQ_CORE_RESET:
			error = nvrtx_falcon_reset(gpu, &gpu->gsp_falcon);
			if (error != 0)
				return error;
			nvrtx_falcon_load_physical(gpu, &gpu->gsp_falcon);
			break;
		case NVRTX_SEQ_CORE_START:
			nvrtx_falcon_start(gpu, &gpu->gsp_falcon);
			break;
		case NVRTX_SEQ_CORE_WAIT_HALT:
			(void)nvrtx_poll(gpu, RPC_GSP_CPU_CONTROL, RPC_CPU_HALTED, RPC_CPU_HALTED, RPC_HALT_TIMEOUT_US);
			break;
		case NVRTX_SEQ_CORE_RESUME:
			error = rpc_core_resume(gpu);
			if (error != 0)
				return error;
			break;
		default:
			nvrtx_stage_mark("sequencer: opcode %u not supported", (unsigned)command.opcode);
			return ENOTSUP;
		}
	}

	/* Succeeded: every command was performed. */
	return 0;
}

/* Shows one firmware error report: its kind and the printable start of its text. */
static void
rpc_error_log(
	const uint8_t *payload,
	size_t length)
{
	char text[RPC_ERROR_LOG_SHOWN + 1U];
	size_t limit;
	size_t index;
	char c;

	/* A report too short for its kind is shown by its length only. */
	if (length < RPC_ERROR_LOG_TEXT_AT) {
		nvrtx_stage_mark("rpc: firmware error report of %zu bytes", length);
		return;
	}

	/* Copies the text up to its end, the payload's end or the shown length, keeping printable characters. */
	limit = length - RPC_ERROR_LOG_TEXT_AT;
	if (limit > RPC_ERROR_LOG_TEXT_BYTES)
		limit = RPC_ERROR_LOG_TEXT_BYTES;
	if (limit > RPC_ERROR_LOG_SHOWN)
		limit = RPC_ERROR_LOG_SHOWN;
	for (index = 0U; index < limit; index++) {
		c = (char)payload[RPC_ERROR_LOG_TEXT_AT + index];
		if (c == '\0')
			break;
		if (c < ' ' || c > '~')
			c = '?';
		text[index] = c;
	}
	text[index] = '\0';

	/* Shows the report. */
	nvrtx_stage_mark("rpc: fw error %u: %s", (unsigned)nvrtx_get32(payload, RPC_ERROR_LOG_KIND_AT), text);
}

/*
 * Resumes the GSP's core at the firmware's request: the GSP falcon is put
 * back into its RISC-V mode with the start page's address in its mailboxes,
 * and the secure processor's booter, still loaded from booter, is started
 * again to hand the core back to the firmware.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_msg_run_cpu_sequencer).
 */
static int
rpc_core_resume(
	struct nvrtx_gpu *gpu)
{
	uint64_t libos;
	uint32_t result;
	uint32_t status;
	bool running;
	int error;

	/* Puts the GSP's falcon back into its RISC-V mode and gives it the start page. */
	nvrtx_stage_mark("sequencer: core resume");
	error = nvrtx_falcon_riscv_mode(gpu, &gpu->gsp_falcon);
	if (error != 0)
		return error;

	libos = gpu->memory.libos.dma.device_address;
	nvrtx_wr32(gpu, RPC_GSP_MAILBOX0, (uint32_t)(libos & 0xffffffffU));
	nvrtx_wr32(gpu, RPC_GSP_MAILBOX1, (uint32_t)(libos >> 32));

	/* Starts the secure processor's booter again and waits for the hand-over. */
	nvrtx_falcon_start(gpu, &gpu->sec2_falcon);
	error = nvrtx_poll(gpu, RPC_BOOT_SCRATCH, RPC_BOOT_HANDED_OVER, RPC_BOOT_HANDED_OVER, RPC_RESUME_TIMEOUT_US);
	if (error != 0) {
		nvrtx_stage_mark("sequencer: resume not handed over (boot %08x)", (unsigned)nvrtx_rd32(gpu, RPC_BOOT_SCRATCH));
		return error;
	}

	/* The booter's result is in the secure processor's mailbox 0. */
	result = nvrtx_rd32(gpu, RPC_SEC2_MAILBOX0);
	if (result != 0U) {
		nvrtx_stage_mark("sequencer: resume booter %08x", (unsigned)result);
		return EIO;
	}

	/* Tells the GSP the bootloader's version and checks that its RISC-V core runs. */
	nvrtx_wr32(gpu, RPC_GSP_APP_VERSION, gpu->bootloader_desc.app_version);
	running = nvrtx_falcon_riscv_running(gpu, &gpu->gsp_falcon, &status);
	if (!running) {
		nvrtx_stage_mark("sequencer: resume left RISC-V stopped (%08x)", (unsigned)status);
		return EIO;
	}

	/* Succeeded: the GSP's core runs the firmware again. */
	return 0;
}

/*
 * Shows what the 3D engine kept of a method it refused: the status, the
 * subchannel and method with their data, the error's code and the class on
 * that subchannel, on two lines that fit an 80-column screen.
 */
static void
rpc_gr_class_error(
	struct nvrtx_gpu *gpu)
{
	uint32_t address;
	uint32_t subchannel;

	address = nvrtx_rd32(gpu, RPC_GR_TRAPPED_ADDR);
	subchannel = (address >> 16) & 0x7U;
	nvrtx_stage_mark("gr refused: status %08x subc %u method %04x",
	    (unsigned)nvrtx_rd32(gpu, RPC_GR_STATUS), (unsigned)subchannel, (unsigned)(address & 0x3ffcU));
	nvrtx_stage_mark("gr refused: data %08x code %08x class %04x",
	    (unsigned)nvrtx_rd32(gpu, RPC_GR_TRAPPED_DATA), (unsigned)nvrtx_rd32(gpu, RPC_GR_ERROR_CODE),
	    (unsigned)(nvrtx_rd32(gpu, RPC_GR_SUBCHANNEL_CLASS + subchannel * 4U) & 0xffffU));
}
