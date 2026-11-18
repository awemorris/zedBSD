/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The last passes of the amdrdna shader compiler: the waits for the
 * results of memory instructions, the breaks of the GFX10 hazards, and
 * the writing of the instruction list as code with its branches resolved.
 */

#ifndef DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_WAIT_H
#define DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_WAIT_H

#include "gfx10-encode.h"

#include <stdint.h>

/*
 * Puts the waits and the hazard breaks into an instruction list.
 *
 * Makes a new list in *out (kern_free() it) with *out_count entries; the
 * input is only read.  Returns 0, or ENOMEM.
 */
int amdrdna_gfx10_place_waits(const struct amdrdna_gfx10_inst *list, uint32_t count, struct amdrdna_gfx10_inst **out, uint32_t *out_count);

/*
 * Writes an instruction list as code.
 *
 * Resolves every branch to its label, moves a branch off the distance
 * GFX10 mishandles, and pads the end for the instruction prefetcher.  The
 * list's branches are rewritten in place.  Makes the code in *words
 * (kern_free() it) with *word_count words.  Returns 0, EINVAL for an
 * instruction the encoder refuses or a branch that does not reach its
 * label, or ENOMEM; *failed is then the index of the instruction at fault.
 */
int amdrdna_gfx10_assemble(struct amdrdna_gfx10_inst **list, uint32_t *count, uint32_t **words, uint32_t *word_count, uint32_t *failed);

#endif /* DRIVERS_GPU_AMDRDNA_COMPILER_GFX10_WAIT_H */
