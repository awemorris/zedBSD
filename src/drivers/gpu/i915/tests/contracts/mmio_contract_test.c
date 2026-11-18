/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The register access, forcewake and steering contract, checked on the host.
 *
 * Runs mmio.c against the mock register file: the domain map, the counted
 * forcewake holds, the refusal of an access whose domain is asleep, the
 * read-modify-write against the single Intel masked write, the posting read,
 * the self-holding access, the acknowledge timeout, the release without a
 * hold, and the exclusive steering section.  It proves the contract, not
 * the hardware.
 */

#include "contract.h"
#include "mock_mmio.h"

#include "../../mmio.h"
#include "../../trace.h"

#include <errno.h>
#include <stdint.h>

static void mmio_check_domains(struct i915_mmio *mmio);
static void mmio_check_forcewake_count(struct i915_mmio *mmio, struct mock_mmio *mock);
static void mmio_check_held_access(struct i915_mmio *mmio, struct mock_mmio *mock);
static void mmio_check_auto_access(struct i915_mmio *mmio, struct mock_mmio *mock);
static void mmio_check_forcewake_failures(struct i915_mmio *mmio, struct mock_mmio *mock);
static void mmio_check_steering(struct i915_mmio *mmio);
static void mmio_check_gen9_domains(struct mock_mmio *mock);

/*
 * Runs the register access contract checks.
 */
int
main(void)
{
	static struct i915_trace trace;
	static struct i915_mmio mmio;
	static struct mock_mmio mock;
	const struct i915_mmio_range *ranges;
	unsigned range_count;
	int held;
	int status;

	contract_begin("MMIO/forcewake/MCR contract tests (mock, GPU-free)");

	/* Binds the register access to an empty mock register file. */
	drv_i915_trace_init(&trace);
	mock_mmio_reset(&mock);
	ranges = mock_mmio_ranges(&range_count);
	drv_i915_mmio_init(&mmio, mock_mmio_ops(), &mock, ranges, range_count, &trace);

	/* Runs each contract group in the order the old suite ran them. */
	mmio_check_domains(&mmio);
	mmio_check_forcewake_count(&mmio, &mock);
	mmio_check_held_access(&mmio, &mock);
	mmio_check_auto_access(&mmio, &mock);
	mmio_check_forcewake_failures(&mmio, &mock);
	mmio_check_steering(&mmio);
	mmio_check_gen9_domains(&mock);

	/* No group may leave a domain held behind it. */
	contract_section("balance: no forcewake left held at end");
	held = drv_i915_forcewake_held(&mmio, I915_FORCEWAKE_RENDER);
	contract_check(held == 0, "render balanced");
	held = drv_i915_forcewake_held(&mmio, I915_FORCEWAKE_GT);
	contract_check(held == 0, "GT balanced");

	/* Reports whether every check held. */
	status = contract_end();
	if (status != 0)
		return status;

	/* Succeeded: the register access keeps its contract. */
	return 0;
}

/* Checks that registers fall into the domain their range names. */
static void
mmio_check_domains(
	struct i915_mmio *mmio)
{
	int domain;

	contract_section("domain: register-to-forcewake-domain mapping");

	/* A render command-streamer register needs the render domain. */
	domain = drv_i915_mmio_domain_of(mmio, 0x2244U);
	contract_check(domain == I915_FORCEWAKE_RENDER, "0x2244 is render domain");
	domain = drv_i915_mmio_domain_of(mmio, 0xe18cU);
	contract_check(domain == I915_FORCEWAKE_RENDER, "0xe18c is render domain");

	/* A GT register needs the GT domain. */
	domain = drv_i915_mmio_domain_of(mmio, 0x9424U);
	contract_check(domain == I915_FORCEWAKE_GT, "0x9424 is GT domain");

	/* A register in no range is always on. */
	domain = drv_i915_mmio_domain_of(mmio, 0x1234U);
	contract_check(domain == -1, "0x1234 is always-on");
}

/*
 * Checks the Gen9 register-to-domain map the driver ships (intel_uncore.c
 * __gen9_fw_ranges): one media domain, the copy engine under GT, the
 * uncore hole always on, and a held media domain covering both media engines.
 */
static void
mmio_check_gen9_domains(
	struct mock_mmio *mock)
{
	static struct i915_trace trace;
	static struct i915_mmio gen9;
	const struct i915_mmio_range *ranges;
	unsigned range_count;
	int domain;
	int error;

	/* Names the section the checks below belong to. */
	contract_section("domain: the Gen9 register-to-forcewake-domain map");

	/* Binds a second access to the Gen9 map. */
	drv_i915_trace_init(&trace);
	ranges = drv_i915_mmio_gen9_ranges(&range_count);
	drv_i915_mmio_init(&gen9, mock_mmio_ops(), mock, ranges, range_count, &trace);

	/* The render command streamer and the render chicken registers need the render domain. */
	domain = drv_i915_mmio_domain_of(&gen9, 0x2244U);
	contract_check(domain == I915_FORCEWAKE_RENDER, "0x2244 is render domain");
	domain = drv_i915_mmio_domain_of(&gen9, 0xe194U);
	contract_check(domain == I915_FORCEWAKE_RENDER, "0xe194 is render domain");

	/* The copy engine, the fuses and the low GT registers need the GT domain. */
	domain = drv_i915_mmio_domain_of(&gen9, 0x22034U);
	contract_check(domain == I915_FORCEWAKE_GT, "0x22034 (BCS) is GT domain");
	domain = drv_i915_mmio_domain_of(&gen9, 0x9120U);
	contract_check(domain == I915_FORCEWAKE_GT, "0x9120 (GEN8_FUSE2) is GT domain");
	domain = drv_i915_mmio_domain_of(&gen9, 0x0a00U);
	contract_check(domain == I915_FORCEWAKE_GT, "0xa00 is GT domain");

	/* Both media engines sit in the one media domain. */
	domain = drv_i915_mmio_domain_of(&gen9, 0x12034U);
	contract_check(domain == I915_FORCEWAKE_MEDIA_GEN9, "0x12034 (VCS0) is the Gen9 media domain");
	domain = drv_i915_mmio_domain_of(&gen9, 0x1a034U);
	contract_check(domain == I915_FORCEWAKE_MEDIA_GEN9, "0x1a034 (VECS0) is the Gen9 media domain");

	/* The uncore hole, the forcewake requests and the display are always on. */
	domain = drv_i915_mmio_domain_of(&gen9, 0x0fdcU);
	contract_check(domain == -1, "0xfdc (MCR selector) is always-on");
	domain = drv_i915_mmio_domain_of(&gen9, 0xa270U);
	contract_check(domain == I915_FORCEWAKE_GT, "0xa270 sits in the GT range as the reference lists it");
	domain = drv_i915_mmio_domain_of(&gen9, 0x44200U);
	contract_check(domain == -1, "0x44200 (GEN8_MASTER_IRQ) is always-on");

	/* The media domain wakes and is given back by its own count. */
	error = drv_i915_forcewake_get(&gen9, I915_FORCEWAKE_MEDIA_GEN9);
	contract_check(error == 0, "the Gen9 media domain wakes");
	(void)drv_i915_read32(&gen9, 0x1a034U);
	error = drv_i915_forcewake_put(&gen9, I915_FORCEWAKE_MEDIA_GEN9);
	contract_check(error == 0, "the Gen9 media domain is given back");
	contract_check(drv_i915_forcewake_held(&gen9, I915_FORCEWAKE_MEDIA_GEN9) == 0, "the Gen9 media domain is balanced");
}

/* Checks that nested holds wake the domain once and let it sleep once. */
static void
mmio_check_forcewake_count(
	struct i915_mmio *mmio,
	struct mock_mmio *mock)
{
	int error;
	int held;

	contract_section("FW-1: refcounted get/nested/put/put with one wake + one sleep");

	/* The first hold wakes the domain. */
	error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_RENDER);
	contract_check(error == 0, "get render ok");
	held = drv_i915_forcewake_held(mmio, I915_FORCEWAKE_RENDER);
	contract_check(held != 0, "render held");
	contract_check(mock->forcewake_request_calls[I915_FORCEWAKE_RENDER] == 1, "one wake request on 0->1");

	/* A nested hold finds the domain awake and asks nothing of it. */
	error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_RENDER);
	contract_check(error == 0, "nested get ok");
	contract_check(mock->forcewake_request_calls[I915_FORCEWAKE_RENDER] == 1, "no extra request on nested get");

	/* The first release leaves the other holder's domain awake. */
	error = drv_i915_forcewake_put(mmio, I915_FORCEWAKE_RENDER);
	contract_check(error == 0, "first put ok");
	held = drv_i915_forcewake_held(mmio, I915_FORCEWAKE_RENDER);
	contract_check(held != 0, "still held after first put");
	contract_check(mock->forcewake_request_calls[I915_FORCEWAKE_RENDER] == 1, "no sleep yet");

	/* The last release lets the domain sleep. */
	error = drv_i915_forcewake_put(mmio, I915_FORCEWAKE_RENDER);
	contract_check(error == 0, "second put ok");
	held = drv_i915_forcewake_held(mmio, I915_FORCEWAKE_RENDER);
	contract_check(held == 0, "released after last put");
	contract_check(mock->forcewake_request_calls[I915_FORCEWAKE_RENDER] == 2, "sleep request on 1->0");
}

/* Checks the accesses that rely on the caller's hold. */
static void
mmio_check_held_access(
	struct i915_mmio *mmio,
	struct mock_mmio *mock)
{
	uint32_t value;
	int reads_before;
	int writes_before;
	int error;
	int held;

	contract_section("MMIO-1: forcewaked access requires the domain held");

	/* A register whose domain sleeps reads the all-ones the hardware would return. */
	mock_mmio_preset(mock, 0x2244U, 0x00000008U);
	value = drv_i915_read32(mmio, 0x2244U);
	contract_check(value == 0xffffffffU, "read without forcewake -> sentinel");

	/* Once the domain is held the register reads its real value. */
	error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_RENDER);
	contract_check(error == 0, "get render");
	value = drv_i915_read32(mmio, 0x2244U);
	contract_check(value == 0x00000008U, "read with forcewake -> real value");

	/* An always-on register needs no hold. */
	mock_mmio_preset(mock, 0x1234U, 0xdeadbeefU);
	value = drv_i915_read32(mmio, 0x1234U);
	contract_check(value == 0xdeadbeefU, "always-on read ok without forcewake");

	contract_section("MMIO-2: masked RMW touches only masked bits");

	/* Replaces the bits inside the mask and keeps every bit outside it. */
	mock_mmio_preset(mock, 0x2580U, 0x0000ffffU);
	drv_i915_rmw32(mmio, 0x2580U, 0xff00U, 0x1234U);
	value = mock_mmio_peek(mock, 0x2580U);
	contract_check(value == 0x000012ffU, "RMW: (0xffff & ~0xff00)|(0x1234 & 0xff00)=0x12ff");

	contract_section("MMIO-2b: Intel masked write: single write, no read-modify-write");

	/* Writes an enable-bit-2, disable-bit-1 word and counts the bus traffic. */
	reads_before = mock->read_calls;
	writes_before = mock->write_calls;
	drv_i915_write32_masked(mmio, 0x2588U, 0x00060004U);
	contract_check(mock->read_calls == reads_before, "no read performed (not an RMW)");
	contract_check(mock->write_calls == writes_before + 1, "exactly one write");
	value = mock_mmio_peek(mock, 0x2588U);
	contract_check(value == 0x00060004U, "composed word written verbatim");

	contract_section("MMIO-3: posting read issues a bus read");

	/* A posting read reaches the bus once. */
	reads_before = mock->read_calls;
	drv_i915_posting_read32(mmio, 0x2244U);
	contract_check(mock->read_calls == reads_before + 1, "posting read performed one read");

	/* Gives back the hold the group took. */
	error = drv_i915_forcewake_put(mmio, I915_FORCEWAKE_RENDER);
	contract_check(error == 0, "put render");
	held = drv_i915_forcewake_held(mmio, I915_FORCEWAKE_RENDER);
	contract_check(held == 0, "render released");
}

/* Checks the accesses that take and give back the domain themselves. */
static void
mmio_check_auto_access(
	struct i915_mmio *mmio,
	struct mock_mmio *mock)
{
	uint32_t value;
	int requests_before;
	int held;

	contract_section("MMIO-4: auto access takes/releases forcewake around the access");

	/* Starts from a sleeping domain. */
	requests_before = mock->forcewake_request_calls[I915_FORCEWAKE_RENDER];
	mock_mmio_preset(mock, 0x2244U, 0x11223344U);
	held = drv_i915_forcewake_held(mmio, I915_FORCEWAKE_RENDER);
	contract_check(held == 0, "nothing held before auto access");

	/* Reads without any hold by the caller. */
	value = drv_i915_read32_auto(mmio, 0x2244U);
	contract_check(value == 0x11223344U, "auto read returns the real value without caller forcewake");
	contract_check(mock->forcewake_request_calls[I915_FORCEWAKE_RENDER] == requests_before + 2, "auto access performed one wake + one sleep");
	held = drv_i915_forcewake_held(mmio, I915_FORCEWAKE_RENDER);
	contract_check(held == 0, "domain released after auto access");

	/* Writes without any hold by the caller. */
	drv_i915_write32_auto(mmio, 0x2248U, 0xcafef00dU);
	value = mock_mmio_peek(mock, 0x2248U);
	contract_check(value == 0xcafef00dU, "auto write reached the register");
}

/* Checks the acknowledge timeout and the release without a hold. */
static void
mmio_check_forcewake_failures(
	struct i915_mmio *mmio,
	struct mock_mmio *mock)
{
	int error;
	int held;

	contract_section("FW-2: ACK timeout returns error and does not leave a held ref");

	/* A domain that never acknowledges fails the hold and leaves it untaken. */
	mock->forcewake_never_ack[I915_FORCEWAKE_GT] = 1;
	error = drv_i915_forcewake_get(mmio, I915_FORCEWAKE_GT);
	contract_check(error == ETIMEDOUT, "get with no ack -> ETIMEDOUT");
	held = drv_i915_forcewake_held(mmio, I915_FORCEWAKE_GT);
	contract_check(held == 0, "no held ref after ack timeout");
	contract_check(mmio->forcewake_ack_timeouts == 1U, "ack timeout counted");
	mock->forcewake_never_ack[I915_FORCEWAKE_GT] = 0;

	contract_section("FW-3: over-put is detected, not silently ignored");

	/* A release nobody held is refused and flagged. */
	error = drv_i915_forcewake_put(mmio, I915_FORCEWAKE_GT);
	contract_check(error == EINVAL, "put with count 0 -> EINVAL");
	contract_check(mmio->forcewake_underflow == 1, "underflow flagged");
}

/* Checks that the steering section admits one owner at a time. */
static void
mmio_check_steering(
	struct i915_mmio *mmio)
{
	int error;
	int locked;

	contract_section("MCR: steering is an exclusive locked section");

	/* Opens the section. */
	error = drv_i915_mcr_lock(mmio, 0x0U);
	contract_check(error == 0, "lock steer ok");
	locked = drv_i915_mcr_locked(mmio);
	contract_check(locked != 0, "mcr locked");

	/* A second steer inside the open section is refused. */
	error = drv_i915_mcr_lock(mmio, 0x1U);
	contract_check(error == EBUSY, "re-lock while held -> EBUSY");

	/* Closing the section admits the next owner. */
	drv_i915_mcr_unlock(mmio);
	locked = drv_i915_mcr_locked(mmio);
	contract_check(locked == 0, "mcr unlocked");
	error = drv_i915_mcr_lock(mmio, 0x2U);
	contract_check(error == 0, "lock again after unlock ok");
	drv_i915_mcr_unlock(mmio);
}
