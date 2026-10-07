/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The engine reset and quarantine release contract (ws083-p007), checked on
 * the host.
 *
 * Runs reset.c's reset of one engine against the mock register file with a
 * model of the hardware's answers: the ready handshake through RESET_CTL,
 * the forced lock of the scaler and format converter, and GDRST clearing
 * its request bits.  It checks that VCS0 is reset through its own GDRST
 * domain (GEN11_GRDOM_MEDIA), twice, between the ready request and its
 * withdrawal; that a converter in use is locked, reset with the engine and
 * unlocked; that an engine that never becomes ready is not reset; that a
 * catastrophic error skips the handshake; and that a GDRST that never clears
 * fails the reset.  It then runs the checked reset of the recovery
 * operations with the worker's resets standing in, and checks that the
 * quarantine release frees the quarantined object and address space and
 * reclaims the retained video records.  It proves the contract, not the
 * hardware.
 */

#include "contract.h"
#include "mock_mmio.h"

#include "../../device-info.h"
#include "../../i915.h"
#include "../../memory.h"
#include "../../mmio.h"
#include "../../ppgtt.h"
#include "../../request-queue.h"
#include "../../reset.h"
#include "../../trace.h"
#include "../../worker.h"

#include <drivers/gpu/gpu.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The registers the model answers and the checks read back. */
#define RESET_GDRST			0x941cU
#define RESET_VCS0_BASE			0x1c0000U
#define RESET_VECS0_BASE		0x1c8000U
#define RESET_CTL(base)			((base) + 0xd0U)
#define RESET_SFC_LOCK(base)		((base) + 0x88cU)
#define RESET_SFC_STATUS(base)		((base) + 0x890U)

/* The RESET_CTL bits: request, ready, catastrophic error. */
#define RESET_CTL_REQUEST		(1U << 0)
#define RESET_CTL_READY			(1U << 1)
#define RESET_CTL_CAT			(1U << 2)

/* The converter status bits of a video decode engine: in use, lock acknowledged. */
#define RESET_SFC_USAGE			(1U << 0)
#define RESET_SFC_ACK			(1U << 1)

/* The GDRST bits of VCS0 (GEN11_GRDOM_MEDIA) and of its converter (GEN11_GRDOM_SFC0). */
#define RESET_GRDOM_MEDIA		(1U << 5)
#define RESET_GRDOM_SFC0		(1U << 17)

/* How many register writes the model keeps, in order. */
#define RESET_LOG_MAX			64U

/* How many reads a wait makes before it times out, standing in for real time. */
#define RESET_WAIT_POLLS		3U

/*
 * The model of the hardware's answers and the writes it saw.
 *
 * A case sets the faults it wants, resets the log, and reads the log back.
 */
struct reset_model {
	/* Nonzero: the engine never says it is ready. */
	int never_ready;

	/* Nonzero: GDRST never clears its request bits. */
	int gdrst_stuck;

	/* The writes the model saw, oldest first. */
	uint32_t offsets[RESET_LOG_MAX];
	uint32_t values[RESET_LOG_MAX];
	unsigned count;
};

/* The register access the reset writes through, the register file behind it, and the model. */
static struct i915_mmio mmio;
static struct mock_mmio mock;
static struct reset_model model;

/* The GT the engine belongs to: VCS0 with its converter, and VECS0 that shares it. */
static struct i915_gt_info gt;

/* The uncore lock the reset takes. */
static struct spinlock uncore_lock;

/* The device of the checked reset, and what the stand-ins of its collaborators saw. */
static struct i915_device device;
static unsigned gt_resets;
static unsigned engine_resets;
static unsigned objects_destroyed;
static unsigned spaces_destroyed;
static unsigned video_reclaims;

static void reset_model_write(struct mock_mmio *file, uint32_t offset, uint32_t value, void *context);
static void reset_model_clear(void);
static int reset_log_find(uint32_t offset, uint32_t value, unsigned from);
static void reset_gt_make(void);
static void reset_check_domain(void);
static void reset_check_plain(void);
static void reset_check_converter(void);
static void reset_check_not_ready(void);
static void reset_check_catastrophic(void);
static void reset_check_stuck(void);
static void reset_check_release(void);

/*
 * Runs the engine reset and quarantine release contract checks.
 */
int
main(void)
{
	static struct i915_trace trace;
	const struct i915_mmio_range *ranges;
	unsigned range_count;
	int status;

	/* Names the program in the report. */
	contract_begin("engine reset contract tests (mock, GPU-free, ws083-p007)");

	/* Binds the register access to the mock register file and its model. */
	drv_i915_trace_init(&trace);
	mock_mmio_reset(&mock);
	mock.write_hook = reset_model_write;
	mock.write_hook_context = &model;
	ranges = mock_mmio_ranges(&range_count);
	drv_i915_mmio_init(&mmio, mock_mmio_ops(), &mock, ranges, range_count, &trace);
	reset_gt_make();

	/* The checks. */
	reset_check_domain();
	reset_check_plain();
	reset_check_converter();
	reset_check_not_ready();
	reset_check_catastrophic();
	reset_check_stuck();
	reset_check_release();

	/* Reports whether every check held. */
	status = contract_end();
	return status;
}

/* Answers a write as the hardware would, and logs it. */
static void
reset_model_write(
	struct mock_mmio *file,
	uint32_t offset,
	uint32_t value,
	void *context)
{
	struct reset_model *state;
	uint32_t status;
	uint32_t ctl;
	uint32_t lock;
	uint32_t lock_status;
	int asked;

	/* The model the hook was given, and the registers of VCS0 it answers. */
	state = context;
	ctl = RESET_CTL(RESET_VCS0_BASE);
	lock = RESET_SFC_LOCK(RESET_VCS0_BASE);
	lock_status = RESET_SFC_STATUS(RESET_VCS0_BASE);

	/* Logs the write. */
	if (state->count < RESET_LOG_MAX) {
		state->offsets[state->count] = offset;
		state->values[state->count] = value;
		state->count++;
	}

	/* RESET_CTL takes masked words: a request becomes ready, a catastrophic error clears, a withdrawal clears. */
	if (offset == ctl) {
		/* A request is a masked word that enables the request bit. */
		asked = 0;
		if ((value & (RESET_CTL_REQUEST << 16)) != 0U && (value & RESET_CTL_REQUEST) != 0U)
			asked = 1;

		/* The engine answers it with the ready bit unless the case says it never is ready. */
		status = 0U;
		if (asked) {
			status = RESET_CTL_REQUEST;
			if (state->never_ready == 0)
				status |= RESET_CTL_READY;
		}

		/* The register reads back the answer. */
		mock_mmio_preset(file, offset, status);
		return;
	}

	/* GDRST clears the bits it was asked for, unless the case says it never does. */
	if (offset == RESET_GDRST) {
		if (state->gdrst_stuck == 0)
			mock_mmio_preset(file, offset, 0U);
		return;
	}

	/* The converter's forced lock is acknowledged while it is asked for. */
	if (offset == lock) {
		/* The status says the lock is held while the lock bit is written. */
		status = mock_mmio_peek(file, lock_status);
		if ((value & 1U) != 0U) {
			status |= RESET_SFC_ACK;
		} else {
			status &= ~RESET_SFC_ACK;
		}

		/* The status reads back the answer. */
		mock_mmio_preset(file, lock_status, status);
	}
}

/* Clears the log and the faults, and the registers the model answers. */
static void
reset_model_clear(void)
{
	/* No fault and an empty log. */
	memset(&model, 0, sizeof(model));

	/* The engine idle, not ready, its converter free and unlocked. */
	mock_mmio_preset(&mock, RESET_CTL(RESET_VCS0_BASE), 0U);
	mock_mmio_preset(&mock, RESET_GDRST, 0U);
	mock_mmio_preset(&mock, RESET_SFC_LOCK(RESET_VCS0_BASE), 0U);
	mock_mmio_preset(&mock, RESET_SFC_STATUS(RESET_VCS0_BASE), 0U);
}

/* Finds the first logged write of a value to a register at or after an index; -1 when there is none. */
static int
reset_log_find(
	uint32_t offset,
	uint32_t value,
	unsigned from)
{
	unsigned index;

	/* Looks through the log from the index on. */
	for (index = from; index < model.count; index++) {
		if (model.offsets[index] == offset && model.values[index] == value)
			return (int)index;
	}

	/* No such write. */
	return -1;
}

/* Makes the GT the checks reset: VCS0 with access to its converter, and VECS0. */
static void
reset_gt_make(void)
{
	/* VCS0, its domain from the device information's table. */
	memset(&gt, 0, sizeof(gt));
	gt.engines[0].id = I915_VCS0;
	gt.engines[0].class = I915_VIDEO_DECODE_CLASS;
	gt.engines[0].instance = 0;
	gt.engines[0].mmio_base = RESET_VCS0_BASE;
	gt.engines[0].reset_domain = RESET_GRDOM_MEDIA;
	gt.engines[0].name = "vcs0";
	gt.engines[0].in_use = 1;

	/* VECS0, which shares VCS0's converter. */
	gt.engines[1].id = I915_VECS0;
	gt.engines[1].class = I915_VIDEO_ENHANCEMENT_CLASS;
	gt.engines[1].instance = 0;
	gt.engines[1].mmio_base = RESET_VECS0_BASE;
	gt.engines[1].reset_domain = 1U << 13;
	gt.engines[1].name = "vecs0";
	gt.engines[1].in_use = 1;
	gt.num_engines = 2U;

	/* VCS0 reaches its converter. */
	gt.vdbox_sfc_access = 1U;
	gt.sfc_mask = ~0U;
}

/* An engine without a reset domain is refused before any register is touched. */
static void
reset_check_domain(void)
{
	struct i915_engine_info engine;
	int error;

	/* Starts the section. */
	contract_section("engine without a reset domain");
	reset_model_clear();

	/* A render engine description with the domain left out. */
	memset(&engine, 0, sizeof(engine));
	engine.class = I915_RENDER_CLASS;
	engine.mmio_base = 0x2000U;
	engine.name = "rcs0";
	error = drv_i915_gt_reset_engine(&uncore_lock, &mmio, &gt, &engine, I915_GT_RESET_ACK_US);
	contract_check(error == EINVAL, "an engine without a reset domain is refused with EINVAL");
	contract_check(model.count == 0U, "no register is written");
}

/* VCS0 with its converter free: ready request, two GDRST passes of its own domain, withdrawal. */
static void
reset_check_plain(void)
{
	int ready;
	int first;
	int second;
	int withdrawn;
	int error;

	/* Starts the section. */
	contract_section("VCS0 reset, converter free");
	reset_model_clear();

	/* Resets VCS0. */
	error = drv_i915_gt_reset_engine(&uncore_lock, &mmio, &gt, &gt.engines[0], I915_GT_RESET_ACK_US);
	contract_check(error == 0, "the reset succeeds");

	/* The order of the writes. */
	ready = reset_log_find(RESET_CTL(RESET_VCS0_BASE), (RESET_CTL_REQUEST << 16) | RESET_CTL_REQUEST, 0U);
	first = reset_log_find(RESET_GDRST, RESET_GRDOM_MEDIA, 0U);
	second = -1;
	if (first >= 0)
		second = reset_log_find(RESET_GDRST, RESET_GRDOM_MEDIA, (unsigned)first + 1U);
	withdrawn = reset_log_find(RESET_CTL(RESET_VCS0_BASE), RESET_CTL_REQUEST << 16, 0U);
	contract_check(ready >= 0, "RESET_CTL is asked for the ready state");
	contract_check(first > ready, "GDRST is written with GEN11_GRDOM_MEDIA (0x20) after the engine is ready");
	contract_check(second > first, "GDRST is written a second time (two passes on Alder Lake-P)");
	contract_check(withdrawn > second, "the ready request is withdrawn after the reset");
	contract_check(reset_log_find(RESET_SFC_LOCK(RESET_VCS0_BASE), 1U, 0U) < 0, "a free converter is not locked");
	contract_check(mmio.forcewake_count[I915_FORCEWAKE_GT] == 0, "the GT forcewake hold is given back");
}

/* VCS0 with its converter in use: the converter is locked, reset with the engine and unlocked. */
static void
reset_check_converter(void)
{
	int locked;
	int reset;
	int unlocked;
	int error;

	/* Starts the section. */
	contract_section("VCS0 reset, converter in use");
	reset_model_clear();
	mock_mmio_preset(&mock, RESET_SFC_STATUS(RESET_VCS0_BASE), RESET_SFC_USAGE);

	/* Resets VCS0. */
	error = drv_i915_gt_reset_engine(&uncore_lock, &mmio, &gt, &gt.engines[0], I915_GT_RESET_ACK_US);
	contract_check(error == 0, "the reset succeeds");

	/* The lock, the reset of both domains, and the unlock, in that order. */
	locked = reset_log_find(RESET_SFC_LOCK(RESET_VCS0_BASE), 1U, 0U);
	reset = reset_log_find(RESET_GDRST, RESET_GRDOM_MEDIA | RESET_GRDOM_SFC0, 0U);
	unlocked = reset_log_find(RESET_SFC_LOCK(RESET_VCS0_BASE), 0U, 0U);
	contract_check(locked >= 0, "the converter's forced lock is asked for");
	contract_check(reset > locked, "GDRST resets the engine and its converter (0x20 | 0x20000)");
	contract_check(unlocked > reset, "the converter is unlocked after the reset");
}

/* An engine that never becomes ready is not reset, and the request is withdrawn. */
static void
reset_check_not_ready(void)
{
	int withdrawn;
	int error;

	/* Starts the section. */
	contract_section("VCS0 never ready");
	reset_model_clear();
	model.never_ready = 1;

	/* Resets VCS0. */
	error = drv_i915_gt_reset_engine(&uncore_lock, &mmio, &gt, &gt.engines[0], I915_GT_RESET_ACK_US);
	contract_check(error == ETIMEDOUT, "the reset fails with ETIMEDOUT");
	contract_check(reset_log_find(RESET_GDRST, RESET_GRDOM_MEDIA, 0U) < 0, "GDRST is not written (one attempt, as for an engine mask)");
	withdrawn = reset_log_find(RESET_CTL(RESET_VCS0_BASE), RESET_CTL_REQUEST << 16, 0U);
	contract_check(withdrawn >= 0, "the ready request is withdrawn");
	contract_check(mmio.forcewake_count[I915_FORCEWAKE_GT] == 0, "the GT forcewake hold is given back");
}

/* A catastrophic error skips the ready handshake: it is cleared, and the engine is reset. */
static void
reset_check_catastrophic(void)
{
	int cleared;
	int error;

	/* Starts the section. */
	contract_section("VCS0 with a catastrophic error");
	reset_model_clear();
	mock_mmio_preset(&mock, RESET_CTL(RESET_VCS0_BASE), RESET_CTL_CAT);

	/* Resets VCS0. */
	error = drv_i915_gt_reset_engine(&uncore_lock, &mmio, &gt, &gt.engines[0], I915_GT_RESET_ACK_US);
	contract_check(error == 0, "the reset succeeds");
	cleared = reset_log_find(RESET_CTL(RESET_VCS0_BASE), (RESET_CTL_CAT << 16) | RESET_CTL_CAT, 0U);
	contract_check(cleared >= 0, "the catastrophic error bit is written instead of the ready request");
	contract_check(reset_log_find(RESET_CTL(RESET_VCS0_BASE), (RESET_CTL_REQUEST << 16) | RESET_CTL_REQUEST, 0U) < 0,
		       "the ready request is not made");
	contract_check(reset_log_find(RESET_GDRST, RESET_GRDOM_MEDIA, 0U) > cleared, "GDRST resets the engine afterwards");
}

/* A GDRST that never clears fails the reset after one pass. */
static void
reset_check_stuck(void)
{
	int first;
	int error;

	/* Starts the section. */
	contract_section("GDRST never acknowledged");
	reset_model_clear();
	model.gdrst_stuck = 1;

	/* Resets VCS0. */
	error = drv_i915_gt_reset_engine(&uncore_lock, &mmio, &gt, &gt.engines[0], I915_GT_RESET_ACK_US);
	contract_check(error == ETIMEDOUT, "the reset fails with ETIMEDOUT");
	first = reset_log_find(RESET_GDRST, RESET_GRDOM_MEDIA, 0U);
	contract_check(first >= 0, "GDRST was written once");
	contract_check(reset_log_find(RESET_GDRST, RESET_GRDOM_MEDIA, (unsigned)first + 1U) < 0, "no second pass follows a timed-out one");
	contract_check(reset_log_find(RESET_CTL(RESET_VCS0_BASE), RESET_CTL_REQUEST << 16, 0U) > first, "the ready request is still withdrawn");
}

/* The checked reset releases the quarantine: the object, the address space and the retained video records. */
static void
reset_check_release(void)
{
	struct drv_gpu_ops ops;
	struct i915_gem_object *object;
	struct i915_ppgtt *vm;
	int error;

	/* Starts the section. */
	contract_section("checked reset releases the quarantine");

	/* A device that faulted, with one quarantined object and one quarantined address space. */
	memset(&device, 0, sizeof(device));
	object = calloc(1U, sizeof(*object));
	vm = calloc(1U, sizeof(*vm));
	if (object == NULL || vm == NULL) {
		contract_check(0, "the host allocates the quarantined state");
		free(object);
		free(vm);
		return;
	}

	/* The object and the address space are retained, and the device faulted. */
	object->quarantined = 1U;
	device.gem.objects = object;
	device.gem.quarantined_objects = 1U;
	device.quarantined_vms = vm;
	device.failed = 1U;

	/* Runs the checked reset through the node's recovery operations. */
	memset(&ops, 0, sizeof(ops));
	drv_i915_recovery_bind_ops(&ops);
	error = ops.recovery->reset(&device);
	contract_check(error == 0, "the checked reset succeeds when the GT and engine resets do");
	contract_check(gt_resets == 1U, "the GT is reset once");
	contract_check(engine_resets == I915_ENGINE_COUNT, "every engine record, the video one too, is reset");
	contract_check(objects_destroyed == 1U, "the quarantined object is freed");
	contract_check(device.gem.quarantined_objects == 0U, "no object is counted as quarantined");
	contract_check(spaces_destroyed == 1U, "the quarantined address space is freed");
	contract_check(device.quarantined_vms == NULL, "no address space is left quarantined");
	contract_check(video_reclaims == 1U, "the retained video records are reclaimed");
	contract_check(device.failed == 0U, "the failed mark is cleared");
}

/*
 * The stand-ins of the services reset.c uses that the contract does not
 * check: the waits count reads, the worker's resets succeed, and the
 * releases count.
 */

/* Polls the mock register a few times for the value; a read is all the time that passes on the host. */
int
drv_i915_wait_reg(
	struct i915_mmio *access,
	uint32_t reg,
	uint32_t mask,
	uint32_t value,
	unsigned fast_us,
	unsigned slow_ms,
	uint32_t *last)
{
	unsigned poll;
	uint32_t observed;

	UNUSED_PARAMETER(fast_us);
	UNUSED_PARAMETER(slow_ms);

	/* Reads until the bits match or the polls run out. */
	observed = 0U;
	for (poll = 0U; poll < RESET_WAIT_POLLS; poll++) {
		observed = drv_i915_raw_read32(access, reg);
		if ((observed & mask) == value) {
			if (last != NULL)
				*last = observed;
			return 0;
		}
	}

	/* The bits never matched. */
	if (last != NULL)
		*last = observed;
	return ETIMEDOUT;
}

/* Lets no time pass: the model answers at once. */
int
drv_i915_udelay(
	unsigned microseconds)
{
	UNUSED_PARAMETER(microseconds);
	return 0;
}

/* Takes the uncore lock and the IRQ lock: one host thread needs none. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	UNUSED_PARAMETER(lock);
	return 0UL;
}

/* Releases nothing. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	UNUSED_PARAMETER(lock);
	UNUSED_PARAMETER(enabled);
}

/* Takes the device mutex: one host thread needs none. */
void
mutex_lock(
	struct mutex *lock)
{
	UNUSED_PARAMETER(lock);
}

/* Releases nothing. */
void
mutex_unlock(
	struct mutex *lock)
{
	UNUSED_PARAMETER(lock);
}

/* Prints a kernel log line on the host's output. */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* The log goes to the test's output. */
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
}

/* Frees host memory. */
void
kern_free(
	void *pointer)
{
	free(pointer);
}

/* Counts the GT reset of the checked reset, which succeeds. */
int
drv_i915_worker_gt_reset(
	struct i915_device *owner)
{
	UNUSED_PARAMETER(owner);
	gt_resets++;
	return 0;
}

/* Counts an engine record's reset, which succeeds. */
int
drv_i915_worker_engine_reset(
	struct i915_engine *engine)
{
	UNUSED_PARAMETER(engine);
	engine_resets++;
	return 0;
}

/* Counts the reclaim of the retained video records. */
void
drv_i915_worker_video_reclaim(
	struct i915_device *owner)
{
	UNUSED_PARAMETER(owner);
	video_reclaims++;
}

/* Unbinds nothing: the quarantined object of the check has no binding. */
void
drv_i915_gem_unbind_vm(
	struct i915_gem_object *object)
{
	UNUSED_PARAMETER(object);
}

/* Counts and frees an object that is no longer quarantined. */
void
drv_i915_gem_destroy(
	struct i915_gem_registry *registry,
	struct i915_gem_object *object)
{
	UNUSED_PARAMETER(registry);
	if (object->quarantined == 0U)
		objects_destroyed++;
	free(object);
}

/* Counts an address space's teardown; reset.c frees the structure. */
void
drv_i915_ppgtt_destroy(
	struct i915_ppgtt *vm)
{
	UNUSED_PARAMETER(vm);
	spaces_destroyed++;
}

/* The session operations no check reaches end the run. */
void
drv_i915_request_fail(
	struct i915_engine *engine,
	struct i915_session *session,
	int error,
	struct i915_request **retired)
{
	UNUSED_PARAMETER(engine);
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(error);
	UNUSED_PARAMETER(retired);
	fprintf(stderr, "reset contract test: drv_i915_request_fail reached\n");
	abort();
}

/* Ends the run: no check delivers completions. */
void
drv_i915_request_complete_list(
	struct i915_engine *engine,
	struct i915_request *retired)
{
	UNUSED_PARAMETER(engine);
	UNUSED_PARAMETER(retired);
	fprintf(stderr, "reset contract test: drv_i915_request_complete_list reached\n");
	abort();
}

/* Ends the run: no check isolates a session. */
int
drv_i915_worker_engine_recover(
	struct i915_engine *engine,
	struct i915_session *session,
	int error)
{
	UNUSED_PARAMETER(engine);
	UNUSED_PARAMETER(session);
	UNUSED_PARAMETER(error);
	fprintf(stderr, "reset contract test: drv_i915_worker_engine_recover reached\n");
	abort();
}

/* Ends the run: the mock register file replaces the register window. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	UNUSED_PARAMETER(address);
	fprintf(stderr, "reset contract test: kern_mmio_read32 reached\n");
	abort();
}

/* Ends the run: the mock register file replaces the register window. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	UNUSED_PARAMETER(address);
	UNUSED_PARAMETER(value);
	fprintf(stderr, "reset contract test: kern_mmio_write32 reached\n");
	abort();
}
