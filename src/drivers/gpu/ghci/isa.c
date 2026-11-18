/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The shader ISAs the GHCI driver compiles for.
 *
 * Every GHCI GPU with a shader unit runs programs of the standard ISA,
 * GSI1: natively, or translated by its command processor when its own ISA
 * differs (NATIVE_ISA_ID says which).  The driver therefore needs one
 * compiler, GSI1's, and takes it for a device whose capabilities report
 * shader units and the GSI1 core; a vendor's own ISA would add a row to
 * the table.
 */

#include <kern/kcrt.h>

#include <stddef.h>
#include <stdint.h>

#include "ghci-regs.h"
#include "compiler/ghci-isa.h"

/* The most registers a GSI1 lane has (r0-r62). */
#define ISA_GSI1_REGISTERS		63U

/* The compilers, by ISA; the table lives as long as the kernel. */
static const struct ghci_isa_backend isa_backends[] = {
	{ GHCI_NATIVE_ISA_GSI1, "GSI1", ghci_gsi1_compile, ghci_gsi1_program_free }
};

/*
 * Reports the compiler of a device: GSI1's for one with shader units and
 * the GSI1 core, NULL for one that only shows images.
 */
const struct ghci_isa_backend *
ghci_isa_select(
	const struct ghci_caps *caps)
{
	int shader_units;
	int gsi1;

	/* A GPU without a shader unit runs no program. */
	shader_units = ghci_caps_has(caps, GHCI_HAS_SHADER_UNITS);
	if (shader_units == 0)
		return NULL;

	/* Nor does one without the GSI1 core. */
	gsi1 = ghci_caps_has(caps, GHCI_HAS_GSI1);
	if (gsi1 == 0)
		return NULL;

	/* Succeeded: the standard ISA, whatever the native one. */
	return &isa_backends[0];
}

/* Reports whether the device runs GSI1 natively rather than translated. */
int
ghci_isa_native(
	const struct ghci_caps *caps)
{
	/* NATIVE_ISA_ID holds the FourCC of the GPU's own ISA. */
	if (caps->native_isa != GHCI_NATIVE_ISA_GSI1)
		return 0;

	/* Succeeded: the shader units run GSI1 as it is. */
	return 1;
}

/*
 * Fills the options a device gives every compile: the registers a lane
 * may use, min(63, RESIDENT_WARPS[23:16]) (63 when the field reads 0), and
 * whether the rasterizer writes the sample mask in (DEVICE_CAPS[31]).
 */
void
ghci_isa_options(
	const struct ghci_caps *caps,
	struct ghci_gsi1_options *options)
{
	int multisampling;

	/* Starts from the zeroed request. */
	kern_memset(options, 0, sizeof(*options));

	/* The registers of a lane. */
	options->registers = caps->lane_registers;
	if (options->registers == 0U || options->registers > ISA_GSI1_REGISTERS)
		options->registers = ISA_GSI1_REGISTERS;

	/* The sample mask in r6. */
	multisampling = ghci_caps_has(caps, GHCI_HAS_MSAA);
	if (multisampling != 0)
		options->sample_mask_input = 1U;
}
