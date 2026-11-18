/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of what the firmware left on a display 9 display before the
 * GT starts (see xd9-firmware.h), through the xd code's display 9 check
 * (xd-takeover.c).  The parts that are not display registers stay here:
 * whether a hypervisor runs the machine, and the GPU's VT-d unit's
 * register page.
 */

#include "internal.h"
#include "xd9-firmware.h"
#include "xd-io.h"
#include "xd-takeover.h"
#include <kern/kcrt.h>

#include "../mmio.h"

#include <kern/pmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The registers of a VT-d unit the check reads (the version, the global
 * status and the protected memory enable) within its 4 KiB page.
 *
 * Values referenced: Intel Virtualization Technology for Directed I/O
 * Architecture Specification (VER_REG, GSTS_REG, PMEN_REG).
 */
#define I915_XD9_FIRMWARE_VTD_VER		0x00U
#define I915_XD9_FIRMWARE_VTD_GSTS		0x1cU
#define I915_XD9_FIRMWARE_VTD_PMEN		0x64U
#define I915_XD9_FIRMWARE_VTD_WINDOW		0x1000U

static int i915_xd9_firmware_hypervisor(void);
static int i915_xd9_firmware_vtd_read(void *context, uint64_t base, uint32_t *version, uint32_t *status, uint32_t *protect);

/*
 * Checks what the firmware scans out on a display 9 device and leaves the
 * display unused.  Nothing is written to the hardware; the display is
 * marked absent in every case.  Returns 0, EBUSY when a picture reaches
 * into the driver's GGTT pages, or EIO when an active VT-d unit would
 * translate the GT's DMA.
 */
int
drv_i915_xd9_firmware_check(
	struct i915_display *display,
	struct i915_mmio *mmio,
	unsigned driver_ggtt_first)
{
	static struct i915_xd_io io;
	static struct i915_xd9_fw_check check;
	struct i915_xd_fw_env env;
	int error;

	/* The environment: the hypervisor, the driver's first GGTT page and the VT-d reader. */
	kern_memset(&env, 0, sizeof(env));
	env.hypervisor = i915_xd9_firmware_hypervisor();
	env.driver_first_page = driver_ggtt_first;
	env.vtd_read = i915_xd9_firmware_vtd_read;

	/* The xd code's display 9 check; the GPU node has no display either way. */
	drv_i915_xd_io_init(&io, mmio, NULL);
	error = drv_i915_xd9_fw_check(&io, &env, &check);
	display->absent = 1;
	if (error != 0)
		return error;

	/* Succeeded: the picture is clear of the driver's GGTT pages and nothing translates the GT's DMA. */
	return 0;
}

/* Reports the hypervisor bit of the processor (CPUID leaf 1, ECX bit 31). */
static int
i915_xd9_firmware_hypervisor(void)
{
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;

	/* Asks CPUID leaf 1. */
	eax = 1U;
	__asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));

	/* Only ECX carries the bit; the other two outputs are dropped. */
	(void)ebx;
	(void)edx;

	/* Succeeded: reports whether the bit is set. */
	return (int)((ecx >> 31) & 1U);
}

/* Reads a VT-d unit's version, global status and protected memory enable at its address: 0, or EIO when it cannot be mapped. */
static int
i915_xd9_firmware_vtd_read(
	void *context,
	uint64_t base,
	uint32_t *version,
	uint32_t *status,
	uint32_t *protect)
{
	const volatile uint8_t *page;
	void *unit;
	int error;

	UNUSED_PARAMETER(context);

	/* The unit's register page, for reading. */
	unit = NULL;
	error = kern_device_map(base, I915_XD9_FIRMWARE_VTD_WINDOW, KERN_DEVICE_UNCACHED | KERN_DEVICE_READONLY, &unit);
	if (error != 0 || unit == NULL)
		return EIO;

	/* Succeeded: the three words, and the mapping dropped. */
	page = (const volatile uint8_t *)unit;
	*version = *(const volatile uint32_t *)(page + I915_XD9_FIRMWARE_VTD_VER);
	*status = *(const volatile uint32_t *)(page + I915_XD9_FIRMWARE_VTD_GSTS);
	*protect = *(const volatile uint32_t *)(page + I915_XD9_FIRMWARE_VTD_PMEN);
	(void)kern_device_unmap(unit, I915_XD9_FIRMWARE_VTD_WINDOW);
	return 0;
}
