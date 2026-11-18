/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The background that loads a colour attachment into the tile (load op
 * LOAD, and the partial render's pds_pr_bgnd): a shading cluster program
 * that samples the attachment at the pixel's position and packs it into
 * pixel output register 0, the eight shared words it reads (the image
 * and sampler state), and the data sequencer program that copies them
 * in.  Nothing here touches the core or memory; the host check builds
 * this file unchanged.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_LOAD_PROGRAM_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_LOAD_PROGRAM_H

#include <stdint.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/render-program.h"

/* The shared words the load program reads: the image state, then the sampler state. */
#define POWERVR_RENDER_LOAD_SHARED_WORDS	8U

int powervr_render_load_usc(const struct powervr_format *format, struct powervr_render_usc *program);

/*
 * The load program's probe forms (powervr_render_load_usc_probe): the
 * usual program, the sample at coordinate (0, 0), and the usual program
 * with a wait group after every group (spacing any register a group
 * writes from the next group that reads it).
 */
#define POWERVR_LOAD_PROBE_USUAL	0U
#define POWERVR_LOAD_PROBE_ORIGIN	1U
#define POWERVR_LOAD_PROBE_SPACED	2U
int powervr_render_load_usc_probe(const struct powervr_format *format, unsigned probe, struct powervr_render_usc *program);
int powervr_render_load_words(const struct powervr_view_shape *attachment, uint32_t words[POWERVR_RENDER_LOAD_SHARED_WORDS]);
int powervr_render_load_shared(uint64_t words_address, struct powervr_data_shared_program *program);

/*
 * A partial render's save of the tiles and their load back (the
 * parameter buffer ran out, so the render is split): the partial
 * render's end of tile stores each pixel's output registers, then each
 * tile buffer's words of the pixel, as raw 32-bit words to a scratch
 * buffer of the render's edges (one region each, as R32, R32G32 or
 * R32G32B32A32 UINT texels: linear for one sample, twiddled with four
 * samples a texel for four); the background of the 3D after it samples
 * each region at the pixel (and its sample) and puts the words back
 * into the output registers and the tile buffers.
 *
 * The plan of a render: its edges and samples, the words a pixel the
 * output registers keep (1, 2 or 4, from the pixel's registers 1 to 4),
 * the words a tile buffer keeps (the end of tile loads 2 or 4), its
 * tile buffers, each region's texel format, layout and offset in the
 * scratch buffer, and the scratch buffer's bytes.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/imagination/vulkan/pvr_arch_spm.c
 * (pvr_arch_spm_scratch_buffer_calc_required_size: the samples times the
 * words a pixel times one plus the tile buffers; pvr_arch_spm_init_eot_state:
 * an emit of the output registers then one of each tile buffer, each
 * packed as dword_count U32s from source bit 0; pvr_spm_get_pbe_packmode;
 * pvr_arch_spm_init_bgobj_state: four words a pixel when there are tile
 * buffers; pvr_get_format_from_dword_count) and pvr_usc.c
 * (pvr_uscgen_spm_load).
 */
#define POWERVR_RENDER_PARTIAL_REGIONS_MAX	POWERVR_RENDER_EMITS_MAX
#define POWERVR_RENDER_PARTIAL_WORDS_MAX	(8U + 8U * (POWERVR_RENDER_PARTIAL_REGIONS_MAX - 1U))

struct powervr_render_partial {
	uint32_t width;
	uint32_t height;
	uint32_t samples;
	uint32_t words;
	uint32_t tile_words;
	uint32_t buffers;
	const struct powervr_format *formats[2];
	struct powervr_image_layout layouts[2];
	uint64_t offsets[POWERVR_RENDER_PARTIAL_REGIONS_MAX];
	uint64_t bytes;
};

int powervr_render_partial_plan(uint32_t width, uint32_t height, uint32_t samples, uint32_t registers, uint32_t buffers, struct powervr_render_partial *partial);
int powervr_render_partial_pbe(const struct powervr_render_partial *partial, uint64_t scratch, uint32_t region, uint32_t state[2], uint64_t reg[3]);
int powervr_render_partial_words(const struct powervr_render_partial *partial, uint64_t scratch, const uint64_t *tile_addresses, uint32_t words[POWERVR_RENDER_PARTIAL_WORDS_MAX], uint32_t *count);
int powervr_render_partial_shared(uint64_t words_address, uint32_t count, struct powervr_data_shared_program *program);
int powervr_render_partial_load_usc(const struct powervr_render_partial *partial, struct powervr_render_usc *program);

#endif /* KERN_DRIVERS_GPU_POWERVR_RENDER_LOAD_PROGRAM_H */
