/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GSI1 compiler's uniforms, buffers, shared memory and
 * synchronization.
 *
 * Everything a program binds is reached the way gsi1-abi.h says.  A
 * sampled image takes a texture slot and a sampler slot (a uniform texel
 * buffer a texture slot and a resource table entry for its element
 * count); every other uniform takes an entry of the resource table, a
 * buffer descriptor or a copy of a texture descriptor, which the program
 * reads with lw from the table's address in the constant bank.  The
 * vertex and fragment programs of a pipeline share the slots and the
 * table: a binding the other program already took is found again, and a
 * new one is added after the others.  The push constants and the dynamic
 * offsets are constant-bank words.
 *
 * A buffer access adds the byte offset (and the dynamic offset) to the
 * buffer's address.  With robustBufferAccess, and for a load the IR does
 * not predicate in a graphics stage, the offset is first clamped to the
 * buffer's last word, so that an access stays within the range the
 * descriptor bound.  An access the IR predicates narrows EXEC to the
 * predicate's lanes for that one instruction.
 */

#include "gsi1-private.h"

#include "drivers/gpu/compiler/spirv.h"

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stddef.h>
#include <stdint.h>

static int place_uniform(struct gsi1_lowering *lowering, uint32_t index);
static int place_sampled_image(struct gsi1_lowering *lowering, uint32_t index, struct gsi1_binding *binding);
static int take_slot(struct gsi1_lowering *lowering, struct ghci_gsi1_slot *slots, uint32_t *count, uint32_t limit, uint32_t kind, const struct drv_gpu_shader_ir_uniform *uniform, uint32_t *slot);
static int take_entry(struct gsi1_lowering *lowering, uint32_t kind, const struct drv_gpu_shader_ir_uniform *uniform, uint32_t *entry);
static uint32_t find_dynamic(const struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_uniform *uniform);
static int reads_texel_buffer(const struct gsi1_lowering *lowering, uint32_t uniform);
static int read_entry_word(struct gsi1_lowering *lowering, uint32_t reg, uint32_t offset);
static int lower_uniform_word(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_storage(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_atomic(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int lower_shared(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int shared_atomic_address(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t *address);
static int lower_sync(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction);
static int atomic_sub(uint32_t operation, uint32_t *sub);
static int lower_system_word(struct gsi1_lowering *lowering, const struct drv_gpu_shader_ir_inst *instruction, uint32_t dst);
static int storage_binding(struct gsi1_lowering *lowering, uint32_t uniform, const struct gsi1_binding **binding);
static int predicate_begin(struct gsi1_lowering *lowering, uint32_t predicate, uint32_t *save);
static void predicate_end(struct gsi1_lowering *lowering, uint32_t save);
static void emit_fence(struct gsi1_lowering *lowering, uint32_t fences);

/*
 * Gives the uniforms their slots, resource table entries and dynamic
 * offsets, after the tables the pipeline's other program took.
 */
int
gsi1_memory_prepare(
	struct gsi1_lowering *lowering)
{
	const struct drv_gpu_shader_ir *ir;
	const struct ghci_gsi1_bindings *bindings;
	uint32_t index;
	int error;

	/* Names the module and the tables being laid out. */
	ir = lowering->ir;
	bindings = &lowering->bindings;

	/* The tables handed in are the pipeline's: within their bounds. */
	if (bindings->texture_count > GHCI_GSI1_TEXTURE_SLOTS ||
	    bindings->sampler_count > GHCI_GSI1_SAMPLER_SLOTS ||
	    bindings->resource_count > GHCI_GSI1_RESOURCES ||
	    bindings->resource_bytes > GSI1_ABI_RESOURCE_TABLE_BYTES ||
	    lowering->options.dynamic_count > GHCI_GSI1_DYNAMIC_OFFSETS) {
		error = gsi1_refuse(lowering, EINVAL, "the pipeline's tables are past their bounds");
		return error;
	}

	/* The push constants are 128 bytes at most (c0-c23 and c32-c39). */
	if (ir->push_bytes > GSI1_ABI_PUSH_BYTES) {
		error = gsi1_refuse(lowering, ENOTSUP, "more than 128 bytes of push constants");
		return error;
	}

	/* A workgroup's shared memory is 16 KiB at most. */
	if (ir->shared_bytes > GSI1_ABI_LDS_BYTES) {
		error = gsi1_refuse(lowering, ENOTSUP, "more than 16 KiB of shared memory");
		return error;
	}

	/* Each uniform in the order of the IR's list. */
	for (index = 0U; index < ir->uniform_count; index++) {
		lowering->index = index;
		error = place_uniform(lowering, index);
		if (error != 0)
			return error;
	}

	/* Succeeded: every uniform has its place. */
	lowering->index = 0U;
	return 0;
}

/*
 * Lowers a uniform word, a buffer access, an atomic, a shared access or a
 * synchronization.
 */
int
gsi1_memory_lower(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	int error;

	/* The operation chooses the lowering. */
	switch (instruction->op) {
	case DRV_GPU_IR_LOAD_PUSH:
	case DRV_GPU_IR_LOAD_UBO:
		error = lower_uniform_word(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOAD_STORAGE:
	case DRV_GPU_IR_STORE_STORAGE:
	case DRV_GPU_IR_STORAGE_SIZE:
		error = lower_storage(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_ATOMIC:
		error = lower_atomic(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_LOAD_SHARED:
	case DRV_GPU_IR_STORE_SHARED:
		error = lower_shared(lowering, instruction, dst);
		break;
	case DRV_GPU_IR_BARRIER:
	case DRV_GPU_IR_FENCE:
		error = lower_sync(lowering, instruction);
		break;
	case DRV_GPU_IR_IMAGE_READ:
	case DRV_GPU_IR_IMAGE_WRITE:
		error = gsi1_image_lower(lowering, instruction, dst);
		break;
	default:
		error = gsi1_refuse(lowering, ENOTSUP, "the operation is not compiled for GSI1");
		break;
	}

	/* Reports why the access could not be lowered. */
	if (error != 0)
		return error;

	/* Succeeded: the access is lowered. */
	return 0;
}

/*
 * Reads a constant-bank word as an operand: c0-c31 directly, a later word
 * through a scratch register (lwc).
 */
int
gsi1_bank_operand(
	struct gsi1_lowering *lowering,
	uint32_t bank,
	uint32_t *operand)
{
	uint32_t reg;
	int error;

	/* The first 32 words are operands of their own. */
	if (bank < GSI1_C_COUNT) {
		*operand = GSI1_C(bank);
		return 0;
	}

	/* Takes a scratch register for a later word. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* lwc reads word rs1 + immediate: zero plus the word. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LWC, reg, GSI1_ZERO, (int32_t)bank, 0U));

	/* Succeeded: the word is in the scratch register. */
	*operand = reg;
	return 0;
}

/*
 * Reads a field of a uniform's resource table entry into a scratch
 * register: lw from the table's address (c28) plus the entry's offset
 * plus the field's, with the field's bits taken out of a shared word and
 * a texel buffer's elements divided out of its bytes and stride.
 */
int
gsi1_resource_field(
	struct gsi1_lowering *lowering,
	const struct gsi1_binding *binding,
	uint32_t field,
	uint32_t *operand)
{
	uint32_t reg;
	uint32_t stride;
	uint32_t offset;
	uint32_t mask;
	int error;

	/* A uniform without an entry has no field to read. */
	if (binding->entry == GSI1_NOT_DEFINED) {
		error = gsi1_refuse(lowering, EINVAL, "a descriptor field of a binding without a resource table entry");
		return error;
	}

	/* Takes the scratch register of the field. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* The field's word within the entry, and the bits of it the field has. */
	mask = 0U;
	switch (field) {
	case GSI1_FIELD_BASE:
		offset = GSI1_ABI_BUFFER_BASE;
		break;
	case GSI1_FIELD_BYTES:
	case GSI1_FIELD_ELEMENTS:
		offset = GSI1_ABI_BUFFER_SIZE;
		break;
	case GSI1_FIELD_ROW_BYTES:
		offset = GSI1_ABI_TEXTURE_ROW_PITCH;
		break;
	case GSI1_FIELD_LAYER_BYTES:
		offset = GSI1_ABI_TEXTURE_SLICE_PITCH;
		break;
	case GSI1_FIELD_EXTENT:
		offset = GSI1_ABI_TEXTURE_EXTENT;
		break;
	case GSI1_FIELD_LAYERS:
		offset = GSI1_ABI_TEXTURE_DEPTH;
		mask = GSI1_ABI_LOW_HALF;
		break;
	case GSI1_FIELD_SAMPLES:
		offset = GSI1_ABI_TEXTURE_SAMPLES;
		mask = GSI1_ABI_LOW_BYTE;
		break;
	default:
		error = gsi1_refuse(lowering, EINVAL, "a descriptor field the compiler does not read");
		return error;
	}

	/* The word. */
	error = read_entry_word(lowering, reg, binding->entry + offset);
	if (error != 0)
		return error;

	/* The field's bits of a shared word. */
	if (mask != 0U)
		gsi1_emit_alu_immediate(lowering, GSI1_F3_AND, reg, reg, (int32_t)mask);

	/* A texel buffer's elements: its bytes over its stride (the low half of the next word). */
	if (field == GSI1_FIELD_ELEMENTS) {
		error = gsi1_scratch(lowering, &stride);
		if (error != 0)
			return error;
		error = read_entry_word(lowering, stride, binding->entry + GSI1_ABI_BUFFER_STRIDE);
		if (error != 0)
			return error;
		gsi1_emit_alu_immediate(lowering, GSI1_F3_AND, stride, stride, (int32_t)GSI1_ABI_LOW_HALF);
		gsi1_emit_alu(lowering, GSI1_F3_DIVU, GSI1_F7_MULDIV, reg, reg, stride);
	}

	/* Succeeded: the field is in the scratch register. */
	*operand = reg;
	return 0;
}

/*
 * Reads a word of the driver's table (set 3) into a scratch register: lw
 * from the table's address (c30) plus the word's offset.
 */
int
gsi1_driver_word(
	struct gsi1_lowering *lowering,
	uint32_t offset,
	uint32_t *operand)
{
	uint32_t table;
	uint32_t reg;
	int error;

	/* Takes the scratch register of the word. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* The table's address, the low word of its mirror. */
	error = gsi1_bank_operand(lowering, GSI1_ABI_BANK_SET(GSI1_ABI_SET_DRIVER), &table);
	if (error != 0)
		return error;

	/* lw from it. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LW, reg, table, (int32_t)offset, 0U));

	/* Succeeded: the word is in the scratch register. */
	*operand = reg;
	return 0;
}

/*
 * Computes the address of a buffer access into a scratch register: the
 * buffer's address plus its dynamic offset (a bank word) plus the byte
 * offset (an operand), the offset first clamped to the buffer's last word
 * when `clamp` is nonzero.
 */
int
gsi1_buffer_address(
	struct gsi1_lowering *lowering,
	const struct gsi1_binding *binding,
	uint32_t offset,
	int clamp,
	uint32_t *address)
{
	uint32_t base;
	uint32_t bytes;
	uint32_t dynamic;
	uint32_t reg;
	int error;

	/* Takes the scratch register of the address. */
	error = gsi1_scratch(lowering, &reg);
	if (error != 0)
		return error;

	/* The clamp: the offset of the buffer's last word is its bytes less four. */
	if (clamp != 0) {
		error = gsi1_resource_field(lowering, binding, GSI1_FIELD_BYTES, &bytes);
		if (error != 0)
			return error;

		/* minu(offset, minu(bytes - 4, bytes)): a range under a word does not wrap. */
		gsi1_emit_alu_immediate(lowering, GSI1_F3_ADD, reg, bytes, -4);
		gsi1_emit_alu(lowering, GSI1_F3_MINU, GSI1_F7_MINMAX, reg, reg, bytes);
		gsi1_emit_alu(lowering, GSI1_F3_MINU, GSI1_F7_MINMAX, reg, offset, reg);
		offset = reg;
	}

	/* The buffer's address. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_BASE, &base);
	if (error != 0)
		return error;

	/* address = base + offset. */
	gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, reg, base, offset);

	/* Plus the dynamic offset of a dynamic buffer. */
	if (binding->dynamic != GSI1_NOT_DEFINED) {
		error = gsi1_bank_operand(lowering, GSI1_ABI_BANK_DYNAMIC + binding->dynamic, &dynamic);
		if (error != 0)
			return error;
		gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, reg, reg, dynamic);
	}

	/* Succeeded: the address is in the scratch register. */
	*address = reg;
	return 0;
}

/*
 * Gives one uniform its place: slots for a sampled image, a resource
 * table entry for the others, and a buffer's dynamic offset.
 */
static int
place_uniform(
	struct gsi1_lowering *lowering,
	uint32_t index)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	struct gsi1_binding *binding;
	uint32_t kind;
	uint32_t resource;
	int error;

	/* Names the uniform and its binding, which starts with nothing. */
	uniform = &lowering->ir->uniforms[index];
	binding = &lowering->binding[index];
	binding->kind = GSI1_NO_BINDING;
	binding->entry = GSI1_NOT_DEFINED;
	binding->dynamic = GSI1_NOT_DEFINED;
	binding->format = uniform->format;
	binding->samples = uniform->multisampled;

	/* The system buffer of a compute program is read through special operands. */
	if (uniform->kind == DRV_GPU_IR_UNIFORM_STORAGE && uniform->set == DRV_GPU_IR_SYSTEM_SET)
		return 0;

	/* A sampled image takes slots. */
	if (uniform->kind == DRV_GPU_IR_UNIFORM_SAMPLED_IMAGE) {
		error = place_sampled_image(lowering, index, binding);
		if (error != 0)
			return error;

		/* Succeeded: the image has its slots. */
		return 0;
	}

	/* The kind of the others, and of their entries; a uniform block the program does not read takes nothing. */
	switch (uniform->kind) {
	case DRV_GPU_IR_UNIFORM_BLOCK:
		if (uniform->size == 0U)
			return 0;
		kind = GSI1_BINDING_UNIFORM_BUFFER;
		resource = GHCI_GSI1_RESOURCE_UNIFORM_BUFFER;
		break;
	case DRV_GPU_IR_UNIFORM_STORAGE:
		kind = GSI1_BINDING_STORAGE_BUFFER;
		resource = GHCI_GSI1_RESOURCE_STORAGE_BUFFER;
		break;
	case DRV_GPU_IR_UNIFORM_STORAGE_IMAGE:
		kind = GSI1_BINDING_STORAGE_IMAGE;
		resource = GHCI_GSI1_RESOURCE_STORAGE_IMAGE;
		break;
	case DRV_GPU_IR_UNIFORM_TEXEL_BUFFER:
		kind = GSI1_BINDING_TEXEL_BUFFER;
		resource = GHCI_GSI1_RESOURCE_STORAGE_TEXEL_BUFFER;
		break;
	case DRV_GPU_IR_UNIFORM_INPUT_ATTACHMENT:
		kind = GSI1_BINDING_INPUT_ATTACHMENT;
		resource = GHCI_GSI1_RESOURCE_INPUT_ATTACHMENT;
		break;
	default:
		error = gsi1_refuse(lowering, ENOTSUP, "a kind of uniform GSI1 does not bind");
		return error;
	}

	/* The resource table entry. */
	error = take_entry(lowering, resource, uniform, &binding->entry);
	if (error != 0)
		return error;
	binding->kind = kind;

	/* A buffer's dynamic offset, when the pipeline layout makes it dynamic. */
	if (kind == GSI1_BINDING_UNIFORM_BUFFER || kind == GSI1_BINDING_STORAGE_BUFFER) {
		binding->dynamic = find_dynamic(lowering, uniform);
		if (binding->dynamic != GSI1_NOT_DEFINED)
			lowering->dynamic_used |= 1U << binding->dynamic;
	}

	/* Succeeded: the uniform has its entry. */
	return 0;
}

/*
 * Gives a sampled image its texture slot and its sampler slot; a uniform
 * texel buffer, which texf reads without a sampler, takes a texture slot
 * and a resource table entry for its element count instead.
 */
static int
place_sampled_image(
	struct gsi1_lowering *lowering,
	uint32_t index,
	struct gsi1_binding *binding)
{
	const struct drv_gpu_shader_ir_uniform *uniform;
	struct ghci_gsi1_bindings *bindings;
	int buffer;
	int error;

	/* Names the uniform and the pipeline's tables. */
	uniform = &lowering->ir->uniforms[index];
	bindings = &lowering->bindings;
	binding->kind = GSI1_BINDING_SAMPLED_IMAGE;
	binding->sampler = 0U;

	/* A uniform texel buffer: a texture slot and the entry of its view. */
	buffer = reads_texel_buffer(lowering, index);
	if (buffer != 0) {
		error = take_slot(lowering,
				  bindings->texture,
				  &bindings->texture_count,
				  GHCI_GSI1_TEXTURE_SLOTS,
				  GHCI_GSI1_TEXTURE_TEXEL_BUFFER,
				  uniform,
				  &binding->slot);
		if (error != 0)
			return error;
		error = take_entry(lowering, GHCI_GSI1_RESOURCE_UNIFORM_TEXEL_BUFFER, uniform, &binding->entry);
		if (error != 0)
			return error;

		/* Succeeded: the texel buffer has its slot and its entry. */
		return 0;
	}

	/* An image: its texture slot. */
	error = take_slot(lowering,
			  bindings->texture,
			  &bindings->texture_count,
			  GHCI_GSI1_TEXTURE_SLOTS,
			  GHCI_GSI1_TEXTURE_IMAGE,
			  uniform,
			  &binding->slot);
	if (error != 0)
		return error;

	/* And its sampler's slot. */
	error = take_slot(lowering,
			  bindings->sampler,
			  &bindings->sampler_count,
			  GHCI_GSI1_SAMPLER_SLOTS,
			  GHCI_GSI1_SAMPLER_COMBINED,
			  uniform,
			  &binding->sampler);
	if (error != 0)
		return error;

	/* Succeeded: the image has its slots. */
	return 0;
}

/*
 * Finds the slot of a binding's element in a table of slots, or takes the next one;
 * the pipeline's programs together have at most `limit`.
 */
static int
take_slot(
	struct gsi1_lowering *lowering,
	struct ghci_gsi1_slot *slots,
	uint32_t *count,
	uint32_t limit,
	uint32_t kind,
	const struct drv_gpu_shader_ir_uniform *uniform,
	uint32_t *slot)
{
	uint32_t index;
	int error;

	/* A slot the pipeline already has for the binding. */
	for (index = 0U; index < *count; index++) {
		if (slots[index].kind != kind)
			continue;
		if (slots[index].set != uniform->set || slots[index].binding != uniform->binding ||
		    slots[index].element != uniform->element)
			continue;
		*slot = index;
		return 0;
	}

	/* The pipeline's stages share the slots. */
	if (*count == limit) {
		error = gsi1_refuse(lowering, ENOTSUP, "the pipeline's stages need more than 16 texture or sampler slots");
		return error;
	}

	/* The next slot. */
	index = *count;
	slots[index].kind = kind;
	slots[index].set = uniform->set;
	slots[index].binding = uniform->binding;
	slots[index].element = uniform->element;
	*count = index + 1U;

	/* Succeeded: the binding has its slot. */
	*slot = index;
	return 0;
}

/*
 * Finds the resource table entry of a binding's element, or adds one after the
 * table's last: a buffer descriptor takes one 16-byte unit, a texture
 * descriptor two.
 */
static int
take_entry(
	struct gsi1_lowering *lowering,
	uint32_t kind,
	const struct drv_gpu_shader_ir_uniform *uniform,
	uint32_t *entry)
{
	struct ghci_gsi1_bindings *bindings;
	struct ghci_gsi1_resource *resource;
	uint32_t index;
	uint32_t bytes;
	int error;

	/* Names the pipeline's tables. */
	bindings = &lowering->bindings;

	/* An entry the pipeline already has for the binding. */
	for (index = 0U; index < bindings->resource_count; index++) {
		resource = &bindings->resource[index];
		if (resource->kind != kind)
			continue;
		if (resource->set != uniform->set || resource->binding != uniform->binding ||
		    resource->element != uniform->element)
			continue;
		*entry = resource->offset;
		return 0;
	}

	/* The descriptor's bytes. */
	bytes = GSI1_ABI_BUFFER_BYTES;
	if (kind == GHCI_GSI1_RESOURCE_STORAGE_IMAGE || kind == GHCI_GSI1_RESOURCE_INPUT_ATTACHMENT)
		bytes = GSI1_ABI_TEXTURE_BYTES;

	/* The table has a bound. */
	if (bindings->resource_count == GHCI_GSI1_RESOURCES ||
	    bindings->resource_bytes + bytes > GSI1_ABI_RESOURCE_TABLE_BYTES) {
		error = gsi1_refuse(lowering, ENOTSUP, "the pipeline binds more than its resource table holds");
		return error;
	}

	/* The new entry. */
	resource = &bindings->resource[bindings->resource_count];
	resource->kind = kind;
	resource->set = uniform->set;
	resource->binding = uniform->binding;
	resource->element = uniform->element;
	resource->offset = bindings->resource_bytes;
	bindings->resource_count++;
	bindings->resource_bytes += bytes;

	/* Succeeded: the binding has its entry. */
	*entry = resource->offset;
	return 0;
}

/* Finds the index of a buffer's dynamic offset in the pipeline layout's list, or GSI1_NOT_DEFINED. */
static uint32_t
find_dynamic(
	const struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_uniform *uniform)
{
	uint32_t index;

	/* Each dynamic buffer of the layout. */
	for (index = 0U; index < lowering->options.dynamic_count; index++) {
		if (lowering->options.dynamic[index].set == uniform->set &&
		    lowering->options.dynamic[index].binding == uniform->binding)
			return index;
	}

	/* The buffer is not dynamic. */
	return GSI1_NOT_DEFINED;
}

/* Reports whether a texture instruction fetches a sampled image as a texel buffer. */
static int
reads_texel_buffer(
	const struct gsi1_lowering *lowering,
	uint32_t uniform)
{
	const struct drv_gpu_shader_ir *ir;
	const struct drv_gpu_shader_ir_inst *instruction;
	uint32_t index;

	/* Names the module being scanned. */
	ir = lowering->ir;

	/* Each texture instruction of the uniform's set and binding. */
	for (index = 0U; index < ir->instruction_count; index++) {
		instruction = &ir->instructions[index];
		if (instruction->op != DRV_GPU_IR_TEXTURE)
			continue;
		if (instruction->location != ir->uniforms[uniform].set)
			continue;
		if (instruction->immediate != ir->uniforms[uniform].binding)
			continue;
		if (instruction->element != ir->uniforms[uniform].element)
			continue;
		if ((instruction->image_shape & DRV_GPU_IR_IMAGE_KIND_MASK) == DRV_GPU_IR_IMAGE_BUFFER)
			return 1;
	}

	/* No instruction reads it as a buffer. */
	return 0;
}

/* Emits lw of a word of the resource table (c28 plus the byte offset) into a register. */
static int
read_entry_word(
	struct gsi1_lowering *lowering,
	uint32_t reg,
	uint32_t offset)
{
	uint32_t table;
	int error;

	/* The table's address, the low word of its mirror. */
	error = gsi1_bank_operand(lowering, GSI1_ABI_BANK_SET(GSI1_ABI_SET_RESOURCES), &table);
	if (error != 0)
		return error;

	/* lw from it. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LW, reg, table, (int32_t)offset, 0U));

	/* Succeeded: the word is in the register. */
	return 0;
}

/*
 * Lowers a push-constant word that is not a c operand, from the bank's
 * second part (lwc), and a uniform block's word, with lw from the
 * block's address.
 */
static int
lower_uniform_word(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct drv_gpu_shader_ir *ir;
	const struct gsi1_binding *binding;
	uint32_t word;
	uint32_t offset;
	uint32_t address;
	uint32_t dynamic;
	int error;

	/* A word that is a c operand needs no instruction. */
	if (dst == GSI1_NO_OPERAND)
		return 0;
	ir = lowering->ir;

	/* A push-constant word: bytes 0-95 are c0-c23, bytes 96-127 c32-c39. */
	if (instruction->op == DRV_GPU_IR_LOAD_PUSH) {
		word = instruction->immediate / 4U;
		if (word >= GSI1_ABI_PUSH_LOW_WORDS + GSI1_ABI_PUSH_HIGH_WORDS) {
			error = gsi1_refuse(lowering, EINVAL, "a push-constant word past 128 bytes");
			return error;
		}
		if (word >= GSI1_ABI_PUSH_LOW_WORDS)
			word = GSI1_ABI_BANK_PUSH_HIGH + word - GSI1_ABI_PUSH_LOW_WORDS;
		gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LWC, dst, GSI1_ZERO, (int32_t)word, 0U));
		return 0;
	}

	/* A uniform block's word: its block is one of the program's. */
	if (instruction->location >= ir->uniform_count ||
	    lowering->binding[instruction->location].kind != GSI1_BINDING_UNIFORM_BUFFER) {
		error = gsi1_refuse(lowering, EINVAL, "a uniform word of a block the program does not have");
		return error;
	}
	binding = &lowering->binding[instruction->location];

	/* With robustBufferAccess the word's offset is clamped to the block like a storage buffer's. */
	if (lowering->options.robust_buffer_access != 0U) {
		error = gsi1_scratch(lowering, &offset);
		if (error != 0)
			return error;
		gsi1_emit_constant(lowering, offset, instruction->immediate & ~3U);
		error = gsi1_buffer_address(lowering, binding, offset, 1, &address);
		if (error != 0)
			return error;
		gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LW, dst, address, 0, 0U));
		return 0;
	}

	/* The block's address, plus its dynamic offset. */
	error = gsi1_resource_field(lowering, binding, GSI1_FIELD_BASE, &address);
	if (error != 0)
		return error;
	if (binding->dynamic != GSI1_NOT_DEFINED) {
		error = gsi1_bank_operand(lowering, GSI1_ABI_BANK_DYNAMIC + binding->dynamic, &dynamic);
		if (error != 0)
			return error;
		gsi1_emit_alu(lowering, GSI1_F3_ADD, GSI1_F7_BASE, address, address, dynamic);
	}

	/* lw at the word's offset from it. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LW, dst, address, (int32_t)(instruction->immediate & ~3U), 0U));

	/* Succeeded: the uniform word is in dst. */
	return 0;
}

/*
 * Lowers a storage buffer's load, store and size: lw and sw at the
 * buffer's address plus the offset, predicated and clamped as the
 * file's first comment says; the size is the descriptor's bytes.
 */
static int
lower_storage(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct gsi1_binding *binding;
	uint32_t offset;
	uint32_t data;
	uint32_t address;
	uint32_t bytes;
	uint32_t predicate;
	uint32_t save;
	int predicated;
	int clamp;
	int error;

	/* A group count read through a special operand needs no instruction. */
	if (instruction->op == DRV_GPU_IR_LOAD_STORAGE && dst == GSI1_NO_OPERAND)
		return 0;

	/* The first workgroup of vkCmdDispatchBase, from its bank words. */
	if (instruction->location < lowering->ir->uniform_count && lowering->ir->uniforms[instruction->location].set == DRV_GPU_IR_SYSTEM_SET) {
		error = lower_system_word(lowering, instruction, dst);
		return error;
	}

	/* The buffer. */
	binding = NULL;
	error = storage_binding(lowering, instruction->location, &binding);
	if (error != 0)
		return error;

	/* The size: the descriptor's bytes. */
	if (instruction->op == DRV_GPU_IR_STORAGE_SIZE) {
		error = gsi1_resource_field(lowering, binding, GSI1_FIELD_BYTES, &bytes);
		if (error != 0)
			return error;
		gsi1_emit_move(lowering, dst, bytes);
		return 0;
	}

	/* The offset. */
	error = gsi1_source(lowering, instruction->src[0], &offset);
	if (error != 0)
		return error;

	/*
	 * The clamp: always with robustBufferAccess, and for a load a graphics
	 * stage does not predicate (its lanes outside the block may hold any
	 * offset).
	 */
	predicated = 0;
	if (instruction->component != 0U)
		predicated = 1;
	clamp = 0;
	if (lowering->options.robust_buffer_access != 0U)
		clamp = 1;
	if (instruction->op == DRV_GPU_IR_LOAD_STORAGE && predicated == 0 && lowering->ir->stage != DRV_GPU_STAGE_COMPUTE)
		clamp = 1;

	/* The address. */
	error = gsi1_buffer_address(lowering, binding, offset, clamp, &address);
	if (error != 0)
		return error;

	/* The value of a store. */
	data = 0U;
	if (instruction->op == DRV_GPU_IR_STORE_STORAGE) {
		error = gsi1_source(lowering, instruction->src[1], &data);
		if (error != 0)
			return error;
	}

	/* The predicate narrows EXEC for the access. */
	save = GSI1_NO_OPERAND;
	if (predicated != 0) {
		predicate = instruction->src[1];
		if (instruction->op == DRV_GPU_IR_STORE_STORAGE)
			predicate = instruction->src[2];
		error = predicate_begin(lowering, predicate, &save);
		if (error != 0)
			return error;
	}

	/* The access. */
	if (instruction->op == DRV_GPU_IR_LOAD_STORAGE) {
		gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LW, dst, address, 0, 0U));
	} else {
		gsi1_emit(lowering, gsi1_encode_s(GSI1_OP_STORE, GSI1_F3_SW, address, data, 0));
	}

	/* EXEC comes back. */
	if (save != GSI1_NO_OPERAND)
		predicate_end(lowering, save);

	/* Succeeded: the access is lowered. */
	return 0;
}

/*
 * Lowers an atomic on a storage buffer's word, a storage image's 32-bit
 * texel or a word of shared memory: atom (atoms for shared memory, sub[4])
 * at the address, its old value into dst (a texel outside the image is not
 * changed).
 */
static int
lower_atomic(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	const struct gsi1_binding *binding;
	struct gsi1_extra extra;
	uint32_t offset;
	uint32_t data;
	uint32_t compare;
	uint32_t address;
	uint32_t predicate;
	uint32_t save;
	uint32_t sub;
	uint32_t sources;
	uint32_t inside;
	int clamp;
	int error;

	/* The operation's sub-operation. */
	error = atomic_sub(instruction->immediate, &sub);
	if (error != 0) {
		error = gsi1_refuse(lowering, EINVAL, "an atomic operation that is not one of the IR's");
		return error;
	}

	/*
	 * The word's address: a byte offset in shared memory, a storage image's
	 * texel (with the lanes whose texel is inside), or a storage buffer's
	 * word.
	 */
	inside = GSI1_NO_OPERAND;
	binding = NULL;
	if (instruction->location == DRV_GPU_IR_LOCATION_SHARED) {
		/* Shared memory is a compute program's; atoms wraps at its size, as lds and sts do. */
		error = shared_atomic_address(lowering, instruction, &address);
		if (error != 0)
			return error;
		sub |= GSI1_ATOM_SHARED;
	} else if (instruction->location < lowering->ir->uniform_count &&
	    (lowering->binding[instruction->location].kind == GSI1_BINDING_STORAGE_IMAGE ||
	     lowering->binding[instruction->location].kind == GSI1_BINDING_TEXEL_BUFFER)) {
		error = gsi1_image_texel(lowering, instruction, &lowering->binding[instruction->location], &address, &inside);
		if (error != 0)
			return error;
	} else {
		error = storage_binding(lowering, instruction->location, &binding);
		if (error != 0)
			return error;

		/* The offset. */
		error = gsi1_source(lowering, instruction->src[0], &offset);
		if (error != 0)
			return error;

		/* The address, clamped with robustBufferAccess. */
		clamp = 0;
		if (lowering->options.robust_buffer_access != 0U)
			clamp = 1;
		error = gsi1_buffer_address(lowering, binding, offset, clamp, &address);
		if (error != 0)
			return error;
	}

	/* The value. */
	error = gsi1_source(lowering, instruction->src[1], &data);
	if (error != 0)
		return error;

	/* The compare of a compare-and-exchange. */
	sources = 2U;
	compare = 0U;
	if (instruction->immediate == DRV_GPU_IR_ATOMIC_CMPXCHG) {
		error = gsi1_source(lowering, instruction->src[2], &compare);
		if (error != 0)
			return error;
		sources = 3U;
	}

	/* The predicate narrows EXEC for the atomic. */
	save = GSI1_NO_OPERAND;
	if (instruction->component == DRV_GPU_IR_ATOMIC_PREDICATED) {
		predicate = instruction->src[sources];
		error = predicate_begin(lowering, predicate, &save);
		if (error != 0)
			return error;
	}

	/* So does a texel outside its image. */
	if (inside != GSI1_NO_OPERAND) {
		if (save == GSI1_NO_OPERAND) {
			error = gsi1_scratch(lowering, &save);
			if (error != 0)
				return error;
			gsi1_emit_exec(lowering, GSI1_EXEC_SAVE, save);
		}
		gsi1_emit_exec(lowering, GSI1_EXEC_AND, inside);
	}

	/* atom.op dst, (address), data[, compare]. */
	kern_memset(&extra, 0, sizeof(extra));
	extra.sub = sub;
	gsi1_emit(lowering, gsi1_encode_r4(GSI1_OP_CUSTOM0, GSI1_F3_ATOM, dst, address, data, compare, &extra));

	/* EXEC comes back. */
	if (save != GSI1_NO_OPERAND)
		predicate_end(lowering, save);

	/* Succeeded: the atomic is lowered. */
	return 0;
}

/*
 * Gives the byte address in shared memory of an atomic's word.
 */
static int
shared_atomic_address(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t *address)
{
	int error;

	/* Shared memory is a compute program's. */
	if (lowering->ir->stage != DRV_GPU_STAGE_COMPUTE) {
		error = gsi1_refuse(lowering, EINVAL, "shared memory outside a compute shader");
		return error;
	}

	/* The byte offset is the address; atoms has no immediate. */
	error = gsi1_source(lowering, instruction->src[0], address);
	if (error != 0)
		return error;

	/* Succeeded: the address is in a register or an operand. */
	return 0;
}

/*
 * Lowers a shared-memory load or store: lds and sts at the byte offset
 * (a constant offset in the immediate), predicated when the IR says.
 */
static int
lower_shared(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t constant;
	uint32_t offset;
	uint32_t data;
	uint32_t predicate;
	uint32_t save;
	int32_t immediate;
	int known;
	int fits;
	int error;

	/* Shared memory is a compute program's. */
	if (lowering->ir->stage != DRV_GPU_STAGE_COMPUTE) {
		error = gsi1_refuse(lowering, EINVAL, "shared memory outside a compute shader");
		return error;
	}

	/* A constant offset goes into the immediate, from zero. */
	immediate = 0;
	offset = GSI1_ZERO;
	known = gsi1_value_constant(lowering, instruction->src[0], &constant);
	fits = 0;
	if (known != 0)
		fits = gsi1_immediate_fits((int64_t)(int32_t)constant);
	if (fits != 0) {
		immediate = (int32_t)constant;
	} else {
		error = gsi1_source(lowering, instruction->src[0], &offset);
		if (error != 0)
			return error;
	}

	/* The value of a store. */
	data = 0U;
	if (instruction->op == DRV_GPU_IR_STORE_SHARED) {
		error = gsi1_source(lowering, instruction->src[1], &data);
		if (error != 0)
			return error;
	}

	/* The predicate narrows EXEC for the access. */
	save = GSI1_NO_OPERAND;
	if (instruction->component != 0U) {
		predicate = instruction->src[1];
		if (instruction->op == DRV_GPU_IR_STORE_SHARED)
			predicate = instruction->src[2];
		error = predicate_begin(lowering, predicate, &save);
		if (error != 0)
			return error;
	}

	/* The access. */
	if (instruction->op == DRV_GPU_IR_LOAD_SHARED) {
		gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_CUSTOM0, GSI1_F3_LDS, dst, offset, immediate, 0U));
	} else {
		gsi1_emit(lowering, gsi1_encode_s(GSI1_OP_CUSTOM0, GSI1_F3_STS, offset, data, immediate));
	}

	/* EXEC comes back. */
	if (save != GSI1_NO_OPERAND)
		predicate_end(lowering, save);

	/* Succeeded: the access is lowered. */
	return 0;
}

/*
 * Lowers a barrier and a fence.  A barrier's fences come before bar; a
 * barrier that orders global memory also invalidates the data cache after
 * it, so that the warp reads what the others wrote.
 */
static int
lower_sync(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction)
{
	struct gsi1_extra extra;
	uint32_t fences;

	/* The fences the instruction names. */
	fences = instruction->immediate & (DRV_GPU_IR_FENCE_GLOBAL | DRV_GPU_IR_FENCE_SHARED | DRV_GPU_IR_FENCE_ACQUIRE);

	/* A fence alone. */
	if (instruction->op == DRV_GPU_IR_FENCE) {
		if (fences != 0U)
			emit_fence(lowering, fences);
		return 0;
	}

	/* A barrier's fences before the warps meet. */
	if ((fences & (DRV_GPU_IR_FENCE_GLOBAL | DRV_GPU_IR_FENCE_SHARED)) != 0U)
		emit_fence(lowering, fences & (DRV_GPU_IR_FENCE_GLOBAL | DRV_GPU_IR_FENCE_SHARED));

	/* bar. */
	kern_memset(&extra, 0, sizeof(extra));
	gsi1_emit(lowering, gsi1_encode_r(GSI1_OP_CUSTOM0, GSI1_F3_BAR, 0U, 0U, 0U, 0U, &extra));

	/* What the others wrote to global memory is read again after the barrier. */
	if ((fences & DRV_GPU_IR_FENCE_GLOBAL) != 0U)
		emit_fence(lowering, GSI1_FENCE_ACQUIRE);

	/* Succeeded: the synchronization is lowered. */
	return 0;
}

/*
 * Translates the IR's atomic operation to GSI1's atom sub-operation;
 * returns EINVAL for one the IR does not have.
 */
static int
atomic_sub(
	uint32_t operation,
	uint32_t *sub)
{
	/* Each operation of the IR. */
	switch (operation) {
	case DRV_GPU_IR_ATOMIC_AND:
		*sub = GSI1_ATOM_AND;
		break;
	case DRV_GPU_IR_ATOMIC_OR:
		*sub = GSI1_ATOM_OR;
		break;
	case DRV_GPU_IR_ATOMIC_XOR:
		*sub = GSI1_ATOM_XOR;
		break;
	case DRV_GPU_IR_ATOMIC_XCHG:
		*sub = GSI1_ATOM_XCHG;
		break;
	case DRV_GPU_IR_ATOMIC_ADD:
		*sub = GSI1_ATOM_ADD;
		break;
	case DRV_GPU_IR_ATOMIC_SUB:
		*sub = GSI1_ATOM_SUB;
		break;
	case DRV_GPU_IR_ATOMIC_SMAX:
		*sub = GSI1_ATOM_SMAX;
		break;
	case DRV_GPU_IR_ATOMIC_SMIN:
		*sub = GSI1_ATOM_SMIN;
		break;
	case DRV_GPU_IR_ATOMIC_UMAX:
		*sub = GSI1_ATOM_UMAX;
		break;
	case DRV_GPU_IR_ATOMIC_UMIN:
		*sub = GSI1_ATOM_UMIN;
		break;
	case DRV_GPU_IR_ATOMIC_CMPXCHG:
		*sub = GSI1_ATOM_CMPXCHG;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the sub-operation. */
	return 0;
}

/*
 * Lowers a read of the system buffer past the group counts (which are
 * special operands): the three words of the first workgroup a program
 * made for vkCmdDispatchBase reads at bytes 12, 16 and 20 are bank words
 * 72 to 74 (lwc).  Anything else of the buffer is refused.
 */
static int
lower_system_word(
	struct gsi1_lowering *lowering,
	const struct drv_gpu_shader_ir_inst *instruction,
	uint32_t dst)
{
	uint32_t offset;
	int known;
	int error;

	/* A load at a known offset of the first workgroup's words. */
	known = 0;
	offset = 0U;
	if (instruction->op == DRV_GPU_IR_LOAD_STORAGE && instruction->component == 0U)
		known = gsi1_value_constant(lowering, instruction->src[0], &offset);
	if (known == 0 || offset < DRV_GPU_SPIRV_SYSTEM_BYTES || offset >= DRV_GPU_SPIRV_SYSTEM_BASE_BYTES || (offset & 3U) != 0U) {
		error = gsi1_refuse(lowering, ENOTSUP, "an access to the system buffer GSI1 does not have");
		return error;
	}

	/* lwc reads word zero plus the immediate. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_LOAD, GSI1_F3_LWC, dst, GSI1_ZERO,
		(int32_t)(GSI1_ABI_BANK_DISPATCH_BASE + (offset - DRV_GPU_SPIRV_SYSTEM_BYTES) / 4U), 0U));

	/* Succeeded: the word is in the destination. */
	return 0;
}

/* Finds the place of a storage buffer the program accesses. */
static int
storage_binding(
	struct gsi1_lowering *lowering,
	uint32_t uniform,
	const struct gsi1_binding **binding)
{
	int error;

	/* The uniform is one of the program's storage buffers. */
	if (uniform >= lowering->ir->uniform_count ||
	    lowering->binding[uniform].kind != GSI1_BINDING_STORAGE_BUFFER) {
		error = gsi1_refuse(lowering, EINVAL, "an access to something that is not a storage buffer of the program");
		return error;
	}

	/* Succeeded: the buffer's place. */
	*binding = &lowering->binding[uniform];
	return 0;
}

/*
 * Narrows EXEC to a predicate's lanes: exec.save into a scratch register,
 * then exec.and.
 */
static int
predicate_begin(
	struct gsi1_lowering *lowering,
	uint32_t predicate,
	uint32_t *save)
{
	uint32_t condition;
	int error;

	/* The predicate. */
	error = gsi1_source(lowering, predicate, &condition);
	if (error != 0)
		return error;

	/* Takes the scratch register EXEC is kept in. */
	error = gsi1_scratch(lowering, save);
	if (error != 0)
		return error;

	/* Saves EXEC and keeps the predicate's lanes. */
	gsi1_emit_exec(lowering, GSI1_EXEC_SAVE, *save);
	gsi1_emit_exec(lowering, GSI1_EXEC_AND, condition);

	/* Succeeded: EXEC holds the predicate's lanes. */
	return 0;
}

/* Restores the EXEC predicate_begin() saved. */
static void
predicate_end(
	struct gsi1_lowering *lowering,
	uint32_t save)
{
	/* exec.set from the saved Boolean. */
	gsi1_emit_exec(lowering, GSI1_EXEC_SET, save);
}

/* Emits fence with the IR's fence bits, which are GSI1's. */
static void
emit_fence(
	struct gsi1_lowering *lowering,
	uint32_t fences)
{
	/* The I form with the bits in the immediate. */
	gsi1_emit(lowering, gsi1_encode_i(GSI1_OP_CUSTOM0, GSI1_F3_FENCE, 0U, 0U, (int32_t)fences, 0U));
}
