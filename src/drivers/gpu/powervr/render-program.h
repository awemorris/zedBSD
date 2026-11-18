/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The fixed programs and state words of a PowerVR render that the
 * driver writes itself, without the compiler: the end-of-tile program
 * that hands the tile to the pixel back end, the background that fills
 * a tile with the clear colour, the pixel back end's words of a colour
 * attachment, and the kernel's own test triangle (its programs and its
 * vertex and primitive stream).
 *
 * Nothing here touches the core or memory: the callers give the heap
 * offsets and device addresses where the results go, and the host
 * check builds this file unchanged.  Data sequencer and shading cluster
 * addresses are offsets from their heap's base, as the core reads them.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_RENDER_PROGRAM_H
#define KERN_DRIVERS_GPU_POWERVR_RENDER_PROGRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/powervr/data-program.h"
#include "drivers/gpu/powervr/image-state.h"

/*
 * The largest fixed shading cluster program here, in bytes (the transfer's
 * resolve about 420, the end of tile with three tile buffers' loads about
 * 430, the background clearing twelve words of tile buffers about 600).
 */
#define POWERVR_RENDER_USC_MAX		1024U

/*
 * The data and code words of a data sequencer program that only starts
 * a shading cluster task: the task word in the first two of eight data
 * words, one code word after them.  The core assumes the eight data
 * words and finds the code after them.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c (pvr_pds_kick_usc:
 * ROGUE_PDS_FIXED_PIXEL_SHADER_DATA_SIZE 8 data words, one code word);
 * src/imagination/vulkan/pvr_arch_cmd_buffer.c
 * (pvr_cs_write_load_op_for_view: the core assumes two 128-bit data
 * words and the code after them).
 */
#define POWERVR_RENDER_KICK_DATA_WORDS	8U
#define POWERVR_RENDER_KICK_CODE_WORDS	1U

/*
 * The data words of the end-of-tile data sequencer program, whose code
 * is the fixed area's (fixed-area.h): the task word of the end-of-tile
 * shading cluster program.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pds/pvr_pds.c
 * (pvr_pds_generate_pixel_event with no emit word pairs: one 64-bit
 * constant for the task word, data size 2).
 */
#define POWERVR_RENDER_EVENT_DATA_WORDS	2U

/*
 * The most emits of an end-of-tile program (one a colour attachment, the
 * render pass's most), and the most words a clear background writes (the
 * pixel output registers of a pixel: the BXE-4-32 has no eight output
 * registers).
 *
 * Values referenced: Mesa 26.2.6 src/imagination/common/device_info/
 * bxe-4-32.h (no eight_output_registers) and
 * src/imagination/vulkan/pvr_arch_job_common.c (pvr_arch_pbe_pack_state:
 * a source start past BIT96 only with eight output registers).
 */
#define POWERVR_RENDER_EMITS_MAX	4U
#define POWERVR_RENDER_CLEAR_WORDS_MAX	4U

/* The words of the primitive state block of the test triangle. */
#define POWERVR_RENDER_PPP_WORDS	19U

/* The words of the test triangle's vertex control stream. */
#define POWERVR_RENDER_VDM_WORDS	12U

/*
 * A render's terminate state (the primitive state block every stream
 * names before its end) and the stream's end that names it.
 */
#define POWERVR_RENDER_TERMINATE_STATE_WORDS	3U
#define POWERVR_RENDER_TERMINATE_STREAM_WORDS	3U

/* The coordinates of one vertex of the test triangle: x and y in pixels, z. */
#define POWERVR_RENDER_VERTEX_WORDS	3U

/* A shading cluster program and the temporaries it needs. */
struct powervr_render_usc {
	uint8_t code[POWERVR_RENDER_USC_MAX];
	size_t length;
	unsigned temps;
};

/* A data sequencer program that starts one shading cluster task. */
struct powervr_render_kick {
	uint32_t data[POWERVR_RENDER_KICK_DATA_WORDS];
	uint32_t code[POWERVR_RENDER_KICK_CODE_WORDS];
};

/*
 * The colour attachment the pixel back end stores a tile to: its format,
 * its device address, its edges in pixels, whether it is linear (rows of
 * row_pitch bytes) or twiddled (over padded_width texels a row).  A level
 * of a volume, twiddled in three dimensions, has its depth in slices
 * (0 for any other surface) and the slice stored to.
 */
struct powervr_render_surface {
	const struct powervr_format *format;
	uint64_t address;
	uint32_t width;
	uint32_t height;
	bool linear;
	uint32_t row_pitch;
	uint32_t padded_width;
	uint32_t depth;
	uint32_t slice;
};

/* A rectangle of pixels: its corner and its edges. */
struct powervr_render_area {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

/*
 * The data sequencer programs of a background: where the program that
 * starts the shading cluster task lies (its data, the code after it),
 * where the program that fills the shared registers has its code and its
 * data, how many data words that one has and the shared registers it
 * fills.  All are offsets in the data sequencer heap.
 */
struct powervr_render_background {
	uint32_t kick_offset;
	uint32_t shared_code_offset;
	uint32_t shared_data_offset;
	uint32_t shared_data_words;
	uint32_t shared_registers;
};

/*
 * What the test triangle's streams name: the primitive state block's
 * device address, the fragment's background-shaped programs (as for a
 * background), and the vertex data sequencer program's data and code
 * offsets, its data words and temporaries.
 */
struct powervr_render_triangle {
	uint64_t ppp_address;
	uint64_t terminate_address;
	struct powervr_render_background fragment;
	uint32_t vertex_data_offset;
	uint32_t vertex_code_offset;
	uint32_t vertex_data_words;
	uint32_t vertex_temps;
};

int powervr_render_eot_usc(const uint32_t state[2], struct powervr_render_usc *program);
int powervr_render_eot_usc_emits(const uint32_t (*states)[2], uint32_t count, struct powervr_render_usc *program);
int powervr_render_event_data(uint32_t usc_offset, unsigned temps, uint32_t data[POWERVR_RENDER_EVENT_DATA_WORDS]);
int powervr_render_event_words(uint32_t data_offset, uint32_t *code_word, uint32_t *data_word, uint32_t *info_word);
int powervr_render_clear_usc(struct powervr_render_usc *program);
int powervr_render_clear_usc_words(uint32_t words, struct powervr_render_usc *program);
int powervr_render_clear_shared(uint32_t colour, struct powervr_data_shared_program *program);
int powervr_render_clear_shared_words(const uint32_t *colour, uint32_t words, struct powervr_data_shared_program *program);
int powervr_render_colour_usc(uint32_t colour, struct powervr_render_usc *program);

/*
 * A render's colour attachment cleared in a tile buffer (the pixel
 * output registers had no room for it, powervr_shader_pixel_places):
 * the buffer (0 up), the first of its words among the pixel's words of
 * the buffer, and its clear value's words (powervr_format_output_words,
 * one to four).
 */
struct powervr_render_tile_clear {
	uint32_t buffer;
	uint32_t offset;
	uint32_t words;
	uint32_t value[POWERVR_RENDER_CLEAR_WORDS_MAX];
};

/*
 * The shared words of a background with tile buffer clears: the
 * register words from shared register 0, then from the first even one
 * after them a block of POWERVR_RENDER_TILE_CLEAR_WORDS for each clear
 * (the buffer's address, low word first, and the clear's words).
 */
#define POWERVR_RENDER_TILE_CLEAR_WORDS	6U

/*
 * Builds the end-of-tile program of count emits as
 * powervr_render_eot_usc_emits does, an emit whose attachment is in a
 * tile buffer (its address in tile_addresses, 0 for an emit from the
 * pixel output registers) first loading the tile's words of the buffer
 * into the pixel output registers, registers (1, 2 or 4) a pixel.
 */
int powervr_render_eot_usc_tiles(const uint32_t (*states)[2], const uint64_t *tile_addresses, uint32_t count, uint32_t registers, struct powervr_render_usc *program);

/*
 * Builds the background's program clearing words pixel output registers
 * from shared registers as powervr_render_clear_usc_words does, then
 * each tile buffer clear's words into the pixel's words of its buffer
 * (for its covered samples when samples is 4), and its shared register
 * program from the register words, the clears and the tile buffers'
 * addresses; the shared registers it fills.
 */
int powervr_render_clear_usc_tiles(uint32_t words, const struct powervr_render_tile_clear *clears, uint32_t count, uint32_t samples, struct powervr_render_usc *program);
int powervr_render_clear_shared_tiles(const uint32_t *colour, uint32_t words, const struct powervr_render_tile_clear *clears, uint32_t count, const uint64_t *tile_addresses, struct powervr_data_shared_program *program, uint32_t *shared_registers);
int powervr_render_vertex_usc(struct powervr_render_usc *program);
int powervr_render_vertex_pds(uint64_t vertices, uint32_t usc_offset, unsigned temps, struct powervr_data_vertex_program *program);
int powervr_render_kick_build(uint32_t usc_offset, unsigned temps, struct powervr_render_kick *kick);
int powervr_render_fragment_kick_build(uint32_t usc_offset, unsigned temps, struct powervr_render_kick *kick);

/*
 * Builds a draw's fragment task program for a render of samples samples
 * a pixel: as powervr_render_fragment_kick_build, the program run for
 * each sample (the full rate) when there are four.
 */
int powervr_render_fragment_kick_samples(uint32_t usc_offset, unsigned temps, uint32_t samples, struct powervr_render_kick *kick);
int powervr_render_background_words(const struct powervr_render_background *background, uint64_t words[3]);
int powervr_render_pbe_words(const struct powervr_render_surface *surface, const struct powervr_render_area *area, uint32_t state[2], uint64_t reg[3]);
int powervr_render_pbe_place(uint32_t state[2], uint32_t source_register, uint32_t emit);
void powervr_render_pbe_empty(uint32_t state[2], uint64_t reg[3]);

/*
 * Turns a back end's register words (powervr_render_pbe_words) into a
 * resolve's: the samples of a render of four averaged onto the surface
 * of one.
 */
void powervr_render_pbe_resolve(uint64_t reg[3]);
int powervr_render_triangle_ppp(const struct powervr_render_triangle *triangle, uint32_t words[POWERVR_RENDER_PPP_WORDS]);
void powervr_render_terminate_state(uint32_t width, uint32_t height, uint32_t state[POWERVR_RENDER_TERMINATE_STATE_WORDS]);
int powervr_render_terminate_stream(uint64_t state_address, uint32_t words[POWERVR_RENDER_TERMINATE_STREAM_WORDS]);
int powervr_render_triangle_vdm(const struct powervr_render_triangle *triangle, uint32_t words[POWERVR_RENDER_VDM_WORDS]);

#endif /* KERN_DRIVERS_GPU_POWERVR_RENDER_PROGRAM_H */
