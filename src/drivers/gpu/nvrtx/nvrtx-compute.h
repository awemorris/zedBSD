/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The first compute launch of nvrtx (the compute-test stage).
 *
 * compute-launch.c holds the parts that touch no hardware, so the host
 * test runs them as the kernel does: the compute queue meta data (QMD
 * version 2.2 on Turing, 3.0 on Ampere and Ada) a launch reads, the push
 * buffer that binds the generation's compute class and launches a QMD, and
 * the IR of the first program (every invocation writes 42 at its index).
 * compute.c runs the stage on the GPU.
 */

#ifndef DRIVERS_GPU_NVRTX_NVRTX_COMPUTE_H
#define DRIVERS_GPU_NVRTX_NVRTX_COMPUTE_H

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/compiler/ir.h"

/*
 * The compute class of Turing (the classes of later generations come from
 * the chip's family record), and the subchannel it is bound to.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc5c0.h (TURING_COMPUTE_A) and
 * src/nouveau/vulkan/nvk_queue.c (nvk_queue_init_context_state).
 */
#define NVRTX_CLASS_COMPUTE		0xc5c0U
#define NVRTX_COMPUTE_SUBCHANNEL	1U

/*
 * A QMD is 64 words (256 bytes) and is read at a 256-byte boundary.
 *
 * Values referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc3c0qmd.h (the 2048-bit
 * NVC3C0_QMDV02_02_* layout) and src/nouveau/vulkan/nvk_cmd_dispatch.c
 * (nvk_cmd_upload_qmd).
 */
#define NVRTX_QMD_WORDS			64U
#define NVRTX_QMD_ALIGN			0x100U

/*
 * The constant buffers a QMD binds, and the alignment of their addresses
 * and sizes.
 *
 * Value referenced: Mesa 25.3.6
 * src/nouveau/headers/nvidia/classes/clc3c0qmd.h
 * (NVC3C0_QMDV02_02_CONSTANT_BUFFER_VALID(i), eight buffers).
 */
#define NVRTX_QMD_CBUFS			8U
#define NVRTX_QMD_CBUF_ALIGN		0x100U

/* The first program: its workgroup width and the value every invocation writes. */
#define NVRTX_COMPUTE_FIRST_WIDTH	32U
#define NVRTX_COMPUTE_FIRST_VALUE	42U

/* The values and instructions of the first program's IR. */
#define NVRTX_COMPUTE_FIRST_VALUES	8U
#define NVRTX_COMPUTE_FIRST_INSTRUCTIONS	9U

/*
 * What one launch's QMD says.
 *
 * cbuf_size[i] zero leaves constant buffer i unbound; the addresses and
 * sizes are multiples of NVRTX_QMD_CBUF_ALIGN, the program's address of
 * 256 bytes.
 */
struct nvrtx_qmd_info {
	uint64_t program;
	uint32_t groups[3];
	uint32_t local_size[3];
	uint32_t gpr_count;
	uint32_t shared_bytes;
	uint32_t barrier_count;
	uint64_t cbuf_address[NVRTX_QMD_CBUFS];
	uint32_t cbuf_size[NVRTX_QMD_CBUFS];
};

/*
 * What a generation's compute launches are made of: the QMD version
 * (major, minor) and the compute class.  The caller fills it from the
 * chip's family record; class numbers after Turing's launch with the
 * second signalling method.
 */
struct nvrtx_compute_kind {
	uint32_t qmd_major;
	uint32_t qmd_minor;
	uint32_t compute_class;
};

/*
 * The IR of the first program and the storage it lives in.
 *
 * Filled by nvrtx_compute_first_ir(); the IR's lists point into the record,
 * which the caller keeps while the IR is compiled.
 */
struct nvrtx_compute_first {
	struct drv_gpu_shader_ir ir;
	struct drv_gpu_shader_ir_inst instructions[NVRTX_COMPUTE_FIRST_INSTRUCTIONS];
	struct drv_gpu_shader_ir_uniform uniform;
};

/* Builds a launch's QMD for Turing (version 2.2): 0, or EINVAL for a field it cannot hold. */
int nvrtx_qmd_build(uint32_t *qmd, const struct nvrtx_qmd_info *info);

/* Builds a launch's QMD in the generation's version: 0, or EINVAL for a field it cannot hold or an unknown version. */
int nvrtx_qmd_build_for(uint32_t *qmd, const struct nvrtx_qmd_info *info, const struct nvrtx_compute_kind *kind);

/* Writes the push buffer that binds Turing's compute class and launches the QMD at qmd_va; reports its length. */
size_t nvrtx_push_compute_launch(uint8_t *push, uint64_t qmd_va);

/* Writes the same push buffer for the generation's compute class and launch method; reports its length. */
size_t nvrtx_push_compute_launch_for(uint8_t *push, uint64_t qmd_va, const struct nvrtx_compute_kind *kind);

/* Builds the IR of the first program: every invocation writes 42 at word gl_GlobalInvocationID.x of storage buffer (0, 0). */
void nvrtx_compute_first_ir(struct nvrtx_compute_first *first);

#endif /* DRIVERS_GPU_NVRTX_NVRTX_COMPUTE_H */
