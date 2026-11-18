/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The words of a PowerVR transfer job (copy and clear) and its fixed
 * programs: the fragment registers of the transfer command, the copy's
 * shading cluster program, the shared register words the copy reads its
 * source with, and the packed clear colour.
 *
 * A transfer is a full-rate tile render without geometry: the copy is a
 * background object whose program reads the source texel for each
 * destination pixel, the clear has no object at all and lets the pixel
 * back end store the tile's clear registers; either way the pixel back
 * end's clip is the destination rectangle.  Nothing here touches the core
 * or memory, and the host check builds this file unchanged.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_TRANSFER_PROGRAM_H
#define KERN_DRIVERS_GPU_POWERVR_TRANSFER_PROGRAM_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/image-state.h"
#include "drivers/gpu/powervr/render-program.h"

/* The shared register words the copy program reads: image state, sampler state, the four coordinate floats. */
#define POWERVR_TRANSFER_SHARED_WORDS	16U

/*
 * The shared register words a merging copy reads: the copy's sixteen,
 * then the destination's image state.
 */
#define POWERVR_TRANSFER_MERGE_WORDS	20U

/* The pixel back end's register words of the transfer command (three for each of three targets). */
#define POWERVR_TRANSFER_PBE_WORDS	9U

/* What a transfer job does. */
enum powervr_transfer_kind {
	POWERVR_TRANSFER_CLEAR = 0,
	POWERVR_TRANSFER_COPY,
	POWERVR_TRANSFER_BLIT
};

/*
 * The registers of one transfer command, in the order the firmware's
 * transfer register block holds them.
 */
struct powervr_transfer_regs {
	uint64_t pds_bgnd[3];
	uint64_t isp_mtile_base;
	uint64_t pbe[POWERVR_TRANSFER_PBE_WORDS];
	uint32_t isp_bgobjvals;
	uint32_t usc_pixel_output_ctrl;
	uint32_t usc_clear[4];
	uint32_t isp_mtile_size;
	uint32_t isp_render_origin;
	uint32_t isp_ctl;
	uint32_t isp_aa;
	uint32_t event_pixel_pds_info;
	uint32_t event_pixel_pds_code;
	uint32_t event_pixel_pds_data;
	uint32_t isp_render;
	uint32_t isp_rgn;
	uint32_t frag_screen;
};

/*
 * One transfer job: its kind, the destination and the rectangle written,
 * the clear colour (four float or integer bit patterns) or the source
 * corner the rectangle's corner reads (a blit's source rectangle and
 * filter go in its shared words, powervr_transfer_blit_words), and where
 * the job's data sequencer programs lie (offsets in the data sequencer
 * heap): the copy or blit program's task, the shared register program's
 * code and data and its data words, the shared registers it fills (0 for
 * the copy's POWERVR_TRANSFER_SHARED_WORDS), and the end-of-tile
 * program's data.
 * Samples is 4 for a copy between two images of four samples a pixel
 * (the ISP's 4x antialiasing, the program run for each sample), else 0
 * or 1.
 */
struct powervr_transfer_job {
	enum powervr_transfer_kind kind;
	struct powervr_render_surface destination;
	struct powervr_render_area area;
	uint32_t colour[4];
	uint32_t source_x;
	uint32_t source_y;
	uint32_t copy_kick_offset;
	uint32_t shared_code_offset;
	uint32_t shared_data_offset;
	uint32_t shared_data_words;
	uint32_t shared_registers;
	uint32_t event_data_offset;
	uint32_t samples;
};

const struct powervr_format *powervr_transfer_raw_format(uint32_t texel_bytes);
bool powervr_transfer_needs_rows(const struct powervr_render_surface *surface, const struct powervr_render_area *area);
int powervr_transfer_copy_usc(const struct powervr_format *format, struct powervr_render_usc *program);
int powervr_transfer_copy_usc_volume(const struct powervr_format *format, uint32_t slice, struct powervr_render_usc *program);
int powervr_transfer_copy_usc_samples(const struct powervr_format *format, struct powervr_render_usc *program);
int powervr_transfer_blit_usc(const struct powervr_format *format, struct powervr_render_usc *program);

/*
 * The samples of each pixel the resolve's program reads of a format (4
 * averaged for a float or normalised colour format, 1 -- sample 0 -- for
 * an integer one, 0 when not resolved), whether it averages them, and
 * the program of a transfer that resolves a source of four samples a
 * pixel onto a destination of one (a blit from a multisampled source):
 * the four samples by number, averaged, left in the pixel output as a
 * render pass leaves the format's colour; or sample 0's raw words.
 */
unsigned powervr_transfer_resolve_samples(const struct powervr_format *format);
bool powervr_transfer_resolves(const struct powervr_format *format);
int powervr_transfer_resolve_usc(const struct powervr_format *format, struct powervr_render_usc *program);
int powervr_transfer_shared_words(const struct powervr_view_shape *source, uint32_t source_x, uint32_t source_y, const struct powervr_render_area *area, uint32_t words[POWERVR_TRANSFER_SHARED_WORDS]);
int powervr_transfer_shared_words_rows(const struct powervr_view_shape *source, uint32_t source_x, uint32_t source_y, const struct powervr_render_area *area, uint32_t total, uint32_t step, uint32_t phase, uint32_t words[POWERVR_TRANSFER_SHARED_WORDS]);
int powervr_transfer_blit_words(const struct powervr_view_shape *source, const struct powervr_render_area *source_area, const struct powervr_render_area *area, bool flip_x, bool flip_y, bool linear, uint32_t words[POWERVR_TRANSFER_SHARED_WORDS]);

/*
 * Whether a blit between two formats moves raw texels (the same format,
 * one the blit's program does not pack), and the shared words of such a
 * blit: the source as the raw format of its size, point sampled; the
 * copy's program then runs it.
 */
bool powervr_transfer_blits_raw(const struct powervr_format *source, const struct powervr_format *destination);
int powervr_transfer_raw_blit_words(const struct powervr_view_shape *source, const struct powervr_render_area *source_area, const struct powervr_render_area *area, bool flip_x, bool flip_y, uint32_t words[POWERVR_TRANSFER_SHARED_WORDS]);

/*
 * The program and shared words of a copy that merges its source into
 * the bits of a packed depth and stencil destination outside one
 * aspect: each pixel's source word (shifted left by shift) masked by
 * mask, with the destination's own texel outside mask; the destination
 * is read through the view of it the words name.
 */
int powervr_transfer_merge_usc(uint32_t shift, uint32_t mask, struct powervr_render_usc *program);
int powervr_transfer_merge_words(const struct powervr_view_shape *source, uint32_t source_x, uint32_t source_y, const struct powervr_render_area *area, const struct powervr_view_shape *destination, uint32_t words[POWERVR_TRANSFER_MERGE_WORDS]);

/*
 * The program of a copy that moves one aspect out of a packed depth and
 * stencil source: each pixel's source word shifted right by shift (24:
 * the stencil of D24_UNORM_S8_UINT) into the destination's raw texel.
 * Its shared words are the copy's.
 */
int powervr_transfer_extract_usc(uint32_t shift, struct powervr_render_usc *program);
int powervr_transfer_shared_program(uint64_t words_address, struct powervr_data_shared_program *program);
int powervr_transfer_shared_program_words(uint64_t words_address, uint32_t count, struct powervr_data_shared_program *program);
int powervr_transfer_clear_colour(const struct powervr_format *format, const uint32_t colour[4], uint32_t packed[4]);
int powervr_transfer_regs_build(const struct powervr_transfer_job *job, struct powervr_transfer_regs *regs, uint32_t state[2]);

#endif /* KERN_DRIVERS_GPU_POWERVR_TRANSFER_PROGRAM_H */
