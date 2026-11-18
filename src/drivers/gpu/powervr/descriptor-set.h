/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR descriptor set's memory: where each binding of a set
 * layout lies, and the bytes one buffer, image, sampler or combined
 * image and sampler descriptor holds there.  The driver's data sequencer
 * programs copy these bytes into shared registers for the shader.  This
 * part only computes; it holds no lock and no memory, and it builds on
 * the host too.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_DESCRIPTOR_SET_H
#define KERN_DRIVERS_GPU_POWERVR_DESCRIPTOR_SET_H

#include <stdint.h>

#include "drivers/gpu/powervr/compiler/shader.h"

/*
 * The binding numbers a set layout may use, from 0.  The numbers may be
 * sparse: a layout of a few bindings may name high ones (the GL
 * translation numbers its samplers from 1, its uniform blocks from 32,
 * its captured outputs 48, its storage buffers from 56 and its eight
 * image units' storage images from 64), so the room is for the numbers,
 * not only for as many bindings as a stage reads.
 */
#define POWERVR_SET_BINDINGS_MAX	72U

/* The bytes of each kind of descriptor. */
#define POWERVR_DESCRIPTOR_BUFFER_BYTES		16U
#define POWERVR_DESCRIPTOR_IMAGE_BYTES		32U
#define POWERVR_DESCRIPTOR_SAMPLER_BYTES	48U
#define POWERVR_DESCRIPTOR_COMBINED_BYTES	80U

/*
 * A storage image's descriptor: the buffer descriptor of its texels (the
 * view's first texel and the bytes to the image's end), then the five
 * words of its view's shape the lowered shader reads, padded to whole
 * 16-byte buffer descriptors.
 */
#define POWERVR_DESCRIPTOR_STORAGE_IMAGE_BYTES	48U

/* The pack information of a format the shader's pack instruction has no format for. */
#define POWERVR_PACK_FORMAT_INVALID	0x1fU

/* One binding a set layout's create names: its number, its Vulkan type and its descriptor count. */
struct powervr_set_binding_info {
	uint32_t binding;
	uint32_t type;
	uint32_t count;
};

/*
 * Where one binding's descriptors lie in a set's memory: the first at
 * offset, each next one stride bytes on.  A binding the layout does not
 * name has a count of 0.
 */
struct powervr_set_binding {
	uint32_t type;
	uint32_t count;
	uint32_t offset;
	uint32_t stride;
};

/* A set layout's bindings by number, and the bytes of a set of it. */
struct powervr_set_layout {
	struct powervr_set_binding bindings[POWERVR_SET_BINDINGS_MAX];
	uint32_t bytes;
};

int powervr_set_layout_build(const struct powervr_set_binding_info *infos, uint32_t count, struct powervr_set_layout *layout);
int powervr_set_descriptor_offset(const struct powervr_set_layout *layout, uint32_t binding, uint32_t element, uint32_t part, uint32_t *offset);
uint32_t powervr_set_dynamic_count(const struct powervr_set_layout *layout);
int powervr_set_dynamic_place(const struct powervr_set_layout *layout, uint32_t index, uint32_t *offset);
uint32_t powervr_pack_info(uint32_t vk_format);
void powervr_descriptor_buffer(uint8_t bytes[POWERVR_DESCRIPTOR_BUFFER_BYTES], uint64_t address, uint32_t range);
void powervr_descriptor_image(uint8_t bytes[POWERVR_DESCRIPTOR_IMAGE_BYTES], const uint64_t words[2], uint32_t layer_bytes, uint32_t pack_info);
void powervr_descriptor_sampler(uint8_t bytes[POWERVR_DESCRIPTOR_SAMPLER_BYTES], const uint64_t words[2], uint32_t compare_op);
void powervr_descriptor_storage_image(uint8_t bytes[POWERVR_DESCRIPTOR_STORAGE_IMAGE_BYTES], uint64_t address, uint32_t range, const uint32_t shape[POWERVR_SHADER_SHAPE_WORDS]);

#endif /* KERN_DRIVERS_GPU_POWERVR_DESCRIPTOR_SET_H */
