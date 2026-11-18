/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Imagination PowerVR Rogue GPU core: the early stage of its attach.
 *
 * The early stage runs while the board brings its devices up, before the
 * root file system is mounted.  It powers the core through the SoC's
 * glue, reads the core's identity (branch, version, number of scalable
 * units, configuration) and keeps the core only when it is the BXE-4-32
 * whose firmware the system carries.  It also checks, once, that the
 * uncached view of memory the firmware's shared structures will live in
 * shows the same memory as the cached one; the later stage that loads
 * the firmware relies on that.
 *
 * Every outcome is one short line of the boot log, which the board's
 * screen shows.
 */

#include "drivers/gpu/powervr/powervr.h"
#include "drivers/gpu/powervr/powervr-private.h"
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/pmem.h>
#include <kern/sysctl.h>
#include <uapi/errno.h>

/*
 * The core's identity register and its four fields.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/imagination/pvr_rogue_cr_defs.h
 * (ROGUE_CR_CORE_ID__PBVNC at 0x0020: the branch in bits 48 to 63, the
 * version in bits 32 to 47, the number of scalable units in bits 16 to
 * 31 and the configuration in bits 0 to 15).
 */
#define POWERVR_IDENTITY		0x0020U
#define POWERVR_BRANCH_SHIFT		48U
#define POWERVR_VERSION_SHIFT		32U
#define POWERVR_UNITS_SHIFT		16U
#define POWERVR_FIELD_MASK		0xffffU

/*
 * The one core the driver knows: the JH7110's BXE-4-32.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/img/img-rogue/hwdefs/rogue/km/cores/rgxcore_km_36.50.54.182.h
 * (the core's numbers); Imagination linux-firmware branch powervr
 * 8a58f818 powervr/rogue_36.50.54.182_v1.fw (the firmware's name).
 */
#define POWERVR_KNOWN_BRANCH		36U
#define POWERVR_KNOWN_VERSION		50U
#define POWERVR_KNOWN_UNITS		54U
#define POWERVR_KNOWN_CONFIG		182U

/* The bytes the identity register needs of the register block. */
#define POWERVR_MINIMUM_REGISTERS	0x1000U

/*
 * The memory the uncached-view check writes: big enough to cross several
 * cache sets, small enough to take below 4 GiB at boot.
 */
#define POWERVR_ALIAS_CHECK_BYTES	0x10000U

/* The two patterns the check writes, one through each view. */
#define POWERVR_ALIAS_PATTERN_A		0x0aU
#define POWERVR_ALIAS_PATTERN_B		0x0bU

/*
 * What the uncached-view check found.
 *
 * OK: each view reads what the other wrote.  MISMATCH: one does not, so
 * the uncached view is not the same memory.  UNAVAILABLE: the check could
 * not run (no memory, or no view on this board).
 */
enum powervr_alias_result {
	POWERVR_ALIAS_OK = 0,
	POWERVR_ALIAS_MISMATCH,
	POWERVR_ALIAS_UNAVAILABLE
};

/*
 * The board's one core, kept for the later stage.
 *
 * The early stage fills it once on the boot CPU; from the moment the
 * later stage's thread starts, that thread alone changes it.  It lives as
 * long as the system.
 */
static struct powervr_core board_core;

static void read_identity(struct powervr_core *core);
static int identity_known(const struct powervr_core *core);
static enum powervr_alias_result check_alias(int *error);
static void fill_pattern(volatile uint64_t *words, size_t count, uint64_t tag);
static int pattern_matches(const volatile uint64_t *words, size_t count, uint64_t tag);

/*
 * Brings the core up as far as reading its identity, and checks the
 * uncached view of memory its firmware's structures will need.
 */
int
drv_powervr_attach(
	const struct drv_powervr_description *description)
{
	struct powervr_core *core;
	enum powervr_alias_result alias;
	void *mapped;
	int alias_error;
	int known;
	int error;

	/* Refuses a missing or incomplete description, or a second core. */
	core = &board_core;
	if (description == NULL || description->power == NULL)
		return EINVAL;
	if (description->power->power_up == NULL || description->power->power_down == NULL)
		return EINVAL;
	if (description->register_size < POWERVR_MINIMUM_REGISTERS)
		return EINVAL;
	if (core->registers != NULL)
		return EALREADY;

	/* Keeps the description for the later stage. */
	core->description = *description;

	/* Powers the core, starts its clocks and releases its resets. */
	error = description->power->power_up(description->context, &core->core_rate);
	if (error != 0) {
		kern_logf("powervr: power up failed (%d)\n", error);
		return error;
	}

	/* Maps the core's registers. */
	error = kern_device_map(description->registers, (size_t)description->register_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		powervr_power_down(core);
		kern_logf("powervr: register map failed (%d); powered down\n", error);
		return error;
	}

	core->registers = mapped;

	/* Reads the core's identity and keeps only the core the firmware is for. */
	read_identity(core);
	known = identity_known(core);
	if (!known) {
		powervr_power_down(core);
		kern_logf("powervr: unknown core %u.%u.%u.%u; powered down\n",
			  (unsigned)core->branch,
			  (unsigned)core->version,
			  (unsigned)core->units,
			  (unsigned)core->config);
		return ENODEV;
	}

	/* Checks that the uncached view shows the same memory as the cached one. */
	alias_error = 0;
	alias = check_alias(&alias_error);

	/* Reports the core, its clock and the check on one line. */
	if (alias == POWERVR_ALIAS_OK) {
		kern_logf("powervr: BXE %u.%u.%u.%u at %llu Hz, ALIAS OK\n",
			  (unsigned)core->branch,
			  (unsigned)core->version,
			  (unsigned)core->units,
			  (unsigned)core->config,
			  (unsigned long long)core->core_rate);
	} else if (alias == POWERVR_ALIAS_MISMATCH) {
		kern_logf("powervr: BXE %u.%u.%u.%u at %llu Hz, ALIAS MISMATCH\n",
			  (unsigned)core->branch,
			  (unsigned)core->version,
			  (unsigned)core->units,
			  (unsigned)core->config,
			  (unsigned long long)core->core_rate);
	} else {
		kern_logf("powervr: BXE %u.%u.%u.%u at %llu Hz, ALIAS UNAVAILABLE (%d)\n",
			  (unsigned)core->branch,
			  (unsigned)core->version,
			  (unsigned)core->units,
			  (unsigned)core->config,
			  (unsigned long long)core->core_rate,
			  alias_error);
	}

	/* The stage lines and hw.gpu.powervr.stages from here on. */
	powervr_stage_init();

	/* Without a working uncached view the firmware's memory cannot be shared; the core stays idle. */
	if (alias != POWERVR_ALIAS_OK) {
		powervr_stage_fail(POWERVR_STAGE_FIRMWARE, POWERVR_REASON_COHERENCY);
		return 0;
	}

	/*
	 * Starts the later stage, which loads the firmware once the root is
	 * live.  Until it has published the render node or given up, the GPU
	 * counts as attaching (hw.gpu.attaching), so the graphical login waits
	 * for both nodes; the later stage ends the count on every way out.
	 */
	kern_gpu_attach_begin();
	error = powervr_late_start(core);
	if (error != 0) {
		kern_gpu_attach_end();
		powervr_power_down(core);
		kern_logf("powervr: later stage not started (%d); powered down\n", error);
		powervr_stage_fail(POWERVR_STAGE_FIRMWARE, powervr_stage_reason_of(error));
		return error;
	}

	/* Succeeded: the core is up and the later stage owns it. */
	return 0;
}

/*
 * Reads a 32-bit register of the core.
 */
uint32_t
powervr_read32(
	const struct powervr_core *core,
	uint32_t offset)
{
	uint32_t value;

	/* One read of the register window. */
	value = kern_mmio_read32((const volatile void *)(core->registers + offset));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Reads a 64-bit register of the core.
 */
uint64_t
powervr_read64(
	const struct powervr_core *core,
	uint32_t offset)
{
	uint64_t value;

	/* One read of the register window. */
	value = kern_mmio_read64((const volatile void *)(core->registers + offset));

	/* Succeeded: the register's value. */
	return value;
}

/*
 * Writes a 32-bit register of the core.
 */
void
powervr_write32(
	struct powervr_core *core,
	uint32_t offset,
	uint32_t value)
{
	/* One write of the register window. */
	kern_mmio_write32((volatile void *)(core->registers + offset), value);
}

/*
 * Writes a 64-bit register of the core.
 */
void
powervr_write64(
	struct powervr_core *core,
	uint32_t offset,
	uint64_t value)
{
	/* One write of the register window. */
	kern_mmio_write64((volatile void *)(core->registers + offset), value);
}

/*
 * Powers the core up again after powervr_power_down() and maps its
 * registers; the identity read at attach still holds.
 */
int
powervr_power_up(
	struct powervr_core *core)
{
	void *mapped;
	int error;

	/* Powers the core, starts its clocks and releases its resets. */
	error = core->description.power->power_up(core->description.context, &core->core_rate);
	if (error != 0)
		return error;

	/* Maps the core's registers. */
	error = kern_device_map(core->description.registers, (size_t)core->description.register_size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		core->description.power->power_down(core->description.context);
		return error;
	}

	/* Succeeded: the core is powered and its registers mapped. */
	core->registers = mapped;
	return 0;
}

/*
 * Releases the core's registers and powers it down: the resets held, the
 * clocks stopped in reverse and the domain off.
 */
void
powervr_power_down(
	struct powervr_core *core)
{
	/* Releases the register window when it was made. */
	if (core->registers != NULL) {
		(void)kern_device_unmap((void *)core->registers, (size_t)core->description.register_size);
		core->registers = NULL;
	}

	/* Powers the core down through the SoC's glue. */
	core->description.power->power_down(core->description.context);
}

/* Reads the four fields of the core's identity register. */
static void
read_identity(
	struct powervr_core *core)
{
	uint64_t identity;

	/* The register holds all four fields in one 64-bit word. */
	identity = kern_mmio_read64((const volatile void *)(core->registers + POWERVR_IDENTITY));

	/* Splits it into the branch, version, units and configuration. */
	core->branch = (uint32_t)((identity >> POWERVR_BRANCH_SHIFT) & POWERVR_FIELD_MASK);
	core->version = (uint32_t)((identity >> POWERVR_VERSION_SHIFT) & POWERVR_FIELD_MASK);
	core->units = (uint32_t)((identity >> POWERVR_UNITS_SHIFT) & POWERVR_FIELD_MASK);
	core->config = (uint32_t)(identity & POWERVR_FIELD_MASK);
}

/* Reports whether the core is the one whose firmware the system carries. */
static int
identity_known(
	const struct powervr_core *core)
{
	/* Each field must match; a core of another branch has other firmware. */
	if (core->branch != POWERVR_KNOWN_BRANCH)
		return 0;
	if (core->version != POWERVR_KNOWN_VERSION)
		return 0;
	if (core->units != POWERVR_KNOWN_UNITS)
		return 0;
	if (core->config != POWERVR_KNOWN_CONFIG)
		return 0;

	/* Succeeded: the core is the known one. */
	return 1;
}

/*
 * Checks that the uncached view of a run of memory is the same memory as
 * its cached view.
 *
 * A pattern written cached and pushed out of the caches must read back
 * through the uncached view; a pattern written uncached must read back
 * cached once lines a speculative read may have brought in are dropped.
 * Only plain loads and stores touch the uncached view.  Both directions
 * must match before anything relies on the alias: a mismatch means the
 * alias does not name the same memory, or a write through it does not
 * arrive.
 */
static enum powervr_alias_result
check_alias(
	int *error)
{
	enum powervr_alias_result result;
	struct kern_pmem run;
	volatile uint64_t *cached;
	volatile uint64_t *uncached;
	void *view;
	size_t count;
	int matches;
	int status;

	/* Takes a run below 4 GiB, as the core's memory will be. */
	status = kern_pmem_alloc_limited(POWERVR_ALIAS_CHECK_BYTES, POWERVR_ALIAS_CHECK_BYTES, 0xffffffffULL, 0, &run);
	if (status != 0) {
		*error = status;
		return POWERVR_ALIAS_UNAVAILABLE;
	}

	/* Makes the uncached view; the HAL pushes the run out of the caches first. */
	status = kern_pmem_map_uncached(&run, &view);
	if (status != 0) {
		*error = status;
		(void)kern_pmem_free(&run);
		return POWERVR_ALIAS_UNAVAILABLE;
	}

	cached = kern_pmem_to_kernel(run.paddr);
	uncached = view;
	count = POWERVR_ALIAS_CHECK_BYTES / sizeof(uint64_t);

	/* Writes A cached, pushes it to memory, and expects the uncached view to read A. */
	fill_pattern(cached, count, POWERVR_ALIAS_PATTERN_A);
	kern_dcache_clean_range((const void *)cached, POWERVR_ALIAS_CHECK_BYTES);
	kern_dcache_invalidate_range((const void *)cached, POWERVR_ALIAS_CHECK_BYTES);
	matches = pattern_matches(uncached, count, POWERVR_ALIAS_PATTERN_A);
	result = POWERVR_ALIAS_MISMATCH;
	if (matches) {
		/* Writes B uncached, drops stale cached lines, and expects the cached view to read B. */
		fill_pattern(uncached, count, POWERVR_ALIAS_PATTERN_B);
		kern_dcache_invalidate_range((const void *)cached, POWERVR_ALIAS_CHECK_BYTES);
		matches = pattern_matches(cached, count, POWERVR_ALIAS_PATTERN_B);
		if (matches)
			result = POWERVR_ALIAS_OK;
	}

	/* Ends the view, which drops the run's lines again, and returns the run. */
	(void)kern_pmem_unmap_uncached(view, POWERVR_ALIAS_CHECK_BYTES);
	(void)kern_pmem_free(&run);

	/* Succeeded: the check ran; the result says what it found. */
	return result;
}

/* Writes a pattern that differs for every word and every tag. */
static void
fill_pattern(
	volatile uint64_t *words,
	size_t count,
	uint64_t tag)
{
	size_t index;

	/* One word at a time, plain stores only. */
	for (index = 0; index < count; index++)
		words[index] = (tag << 56) ^ ((uint64_t)index * 0x9e3779b97f4a7c15ULL);
}

/* Reports whether every word holds the pattern of a tag. */
static int
pattern_matches(
	const volatile uint64_t *words,
	size_t count,
	uint64_t tag)
{
	uint64_t expected;
	size_t index;

	/* Compares every word, plain loads only. */
	for (index = 0; index < count; index++) {
		expected = (tag << 56) ^ ((uint64_t)index * 0x9e3779b97f4a7c15ULL);
		if (words[index] != expected)
			return 0;
	}

	/* Succeeded: the whole run holds the pattern. */
	return 1;
}
