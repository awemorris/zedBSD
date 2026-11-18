/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The driver's own compute programs for the PowerVR core: the shading
 * cluster programs that write a buffer, the data sequencer programs that
 * start them and end a stream, the compute data master's control stream
 * words, and the firmware's compute command around them.
 *
 * They exist so the driver can run work on the core without a user
 * program, to check the core's compute path and what the processor sees
 * of the core's writes.  The same code runs on the build host.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_COMPUTE_PROGRAM_H
#define KERN_DRIVERS_GPU_POWERVR_COMPUTE_PROGRAM_H

#include <stddef.h>
#include <stdint.h>

/*
 * The instances of one task, the words of one kernel in the control
 * stream, and the data and code words of the data sequencer programs.
 *
 * Values referenced:
 * Mesa 26.2.5 src/imagination/include/hwdef/rogue_hw_defs.h
 * (ROGUE_MAX_INSTANCES_PER_TASK 32); src/imagination/csbgen/rogue/cdm.xml
 * (KERNEL0, 1, 2, 3, 4, 5 and 8 without an indirect buffer or global
 * offsets).
 */
#define POWERVR_COMPUTE_INSTANCES	32U

/*
 * The compute limits the Vulkan device reports (vk/vk-device.c), which
 * are what the executor and the compiler run: the invocations of one
 * workgroup and its size along each axis, the workgroups of one
 * dispatch along each axis, and the shared memory of one workgroup.
 */
#define POWERVR_VK_COMPUTE_INVOCATIONS	128U
#define POWERVR_VK_COMPUTE_SIZE_X	128U
#define POWERVR_VK_COMPUTE_SIZE_Y	128U
#define POWERVR_VK_COMPUTE_SIZE_Z	64U
#define POWERVR_VK_COMPUTE_GROUPS_X	65535U
#define POWERVR_VK_COMPUTE_GROUPS_Y	65535U
#define POWERVR_VK_COMPUTE_GROUPS_Z	65535U
#define POWERVR_VK_COMPUTE_SHARED_BYTES	16384U
#define POWERVR_COMPUTE_KERNEL_WORDS	7U
#define POWERVR_COMPUTE_PDS_DATA_WORDS	2U
#define POWERVR_COMPUTE_PDS_CODE_MAX	10U

/*
 * The workgroup program's (powervr_compute_workgroup_program, Mesa's):
 * its data words, its code words, where its code starts after the data
 * (bytes), and the unified store registers its kernel declares (the
 * local id the program writes there; KERNEL0 usc_unified_size 16 bytes).
 */
#define POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS	8U
#define POWERVR_COMPUTE_PDS_GROUP_CODE_WORDS	10U
#define POWERVR_COMPUTE_PDS_GROUP_CODE		0x20U
#define POWERVR_COMPUTE_GROUP_USC_INPUTS	4U

/*
 * Whether a compute stream is Mesa's three kernels: the shared register
 * allocation kernel (powervr_compute_shared_kernel_words), the work (its
 * shared data the shading cluster's, sd_type USC), and the fence kernel
 * that waits for the work and frees the registers.  A fence without the
 * first kernel locked the compute data master up on the VF2; with it
 * the fence ran, and without a fence the kick finished before the stores
 * of the check's hand-written kernel did (only 6944 to 9456 of its 16384
 * words written when the kick was reported done).
 */
#ifndef POWERVR_COMPUTE_FENCE_KERNEL
#define POWERVR_COMPUTE_FENCE_KERNEL	1
#endif

/*
 * Whether the compute check runs the probes that lock the compute data
 * master up or fault on the VF2 (the fence variants, the executor
 * layouts, the no-operation probe, the compiled fill's own workgroup id
 * probe), and then only with POWERVR_DIAGNOSTICS; off by default, as
 * their resets mark the compute stage failed and keep the Vulkan device
 * closed.  Build with -DPOWERVR_DIAGNOSTICS=1
 * -DPOWERVR_COMPUTE_LOCKUP_PROBES=1 to run them.
 */
#ifndef POWERVR_COMPUTE_LOCKUP_PROBES
#define POWERVR_COMPUTE_LOCKUP_PROBES	0
#endif

/* The words of the firmware's compute command. */
#define POWERVR_COMPUTE_COMMAND_WORDS	24U

/*
 * The temporaries each program uses per instance, the words one instance
 * of the store program writes per step, and how far apart its steps are.
 */
/*
 * The temporaries the store program's kick asks for: more than the ten
 * it uses (r0 to r14), so that a task issue counting them in other units
 * than assumed still gives each instance its own.
 */
#define POWERVR_COMPUTE_STORE_TEMPORARIES	16U
#define POWERVR_COMPUTE_TEMPS_TEMPORARIES	8U
#define POWERVR_COMPUTE_FILL_TEMPORARIES	13U
#define POWERVR_COMPUTE_STORE_STRIDE		(POWERVR_COMPUTE_INSTANCES * 4U)

/*
 * One kernel of the control stream.
 *
 * Data_offset and code_offset are the data sequencer program's places
 * in its heap.  Usc_inputs (the input registers the data sequencer
 * writes into the unified store, as Mesa's vtxins) and pds_temporaries
 * are counts of 32-bit registers and go into KERNEL0.  Usc_temporaries
 * is not written to the kernel's words: the task issue word's field
 * carries the temporaries, and on the board KERNEL0's unified size
 * came out of the same allocation (2026-10-20), so writing the
 * temporaries there as well left an instance too few.  Instances is the workgroup's size, all in x;
 * max_instances is how many go in one task, 32 written as zero.  Fence
 * makes the kernel wait for the work before it.  Shared_from_pds and
 * common_shared say the data sequencer loads the shared registers and
 * the kernel frees them; any_cluster lets the task run on any cluster.
 * Shared_from_usc names the shading cluster as the one that loads the
 * shared registers (sd_type USC), as Mesa's dispatch kernels do.
 * Workgroups is the count of workgroups in each dimension (zero taken as
 * one), and usc_common the coefficient registers each workgroup's
 * instances share (the data sequencer writes the workgroup id there).
 */
struct powervr_compute_kernel {
	uint32_t data_offset;
	uint32_t code_offset;
	unsigned data_words;
	unsigned pds_temporaries;
	unsigned usc_inputs;
	unsigned usc_temporaries;
	unsigned instances;
	unsigned max_instances;
	unsigned fence;
	unsigned shared_from_pds;
	unsigned shared_from_usc;
	unsigned common_shared;
	unsigned any_cluster;
	unsigned workgroups[3];
	unsigned usc_common;
};

int powervr_compute_kernel_words(const struct powervr_compute_kernel *kernel, uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS]);
uint32_t powervr_compute_terminate_word(void);
int powervr_compute_temporaries_field(unsigned temporaries, unsigned unit, unsigned *field);
int powervr_compute_task_field(unsigned temporaries, unsigned *field);
int powervr_compute_kick_program(uint64_t usc_offset, unsigned temporaries_field, uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS], uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX], unsigned *code_words);
int powervr_compute_workgroup_program(uint64_t usc_offset, unsigned temporaries_field, uint32_t data[POWERVR_COMPUTE_PDS_GROUP_DATA_WORDS], uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX], unsigned *code_words);
/*
 * The shared register allocation kernel: one instance on every cluster
 * (usc_target ALL), POWERVR_COMPUTE_SHARED_COMMON registers of common
 * store kept for the kernels after it, no shared data, eight instances
 * a task; its data sequencer program is two zero data words and a halt.
 *
 * Values referenced:
 * Mesa 26.2.5's vkcompute stream on BXE-4-32, as its driver wrote it
 * on the host with the drm shim (kernel 0: usc_common_size
 * 64 bytes, usc_target ALL, sd_type NONE, usc_common_shared,
 * max_instances 8, one instance).
 */
#define POWERVR_COMPUTE_SHARED_COMMON	16U
#define POWERVR_COMPUTE_FENCE_INSTANCES	8U
int powervr_compute_shared_program(uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS], uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX], unsigned *code_words);
int powervr_compute_shared_kernel_words(uint32_t data_offset, uint32_t code_offset, uint32_t words[POWERVR_COMPUTE_KERNEL_WORDS]);
int powervr_compute_fence_program(uint32_t data[POWERVR_COMPUTE_PDS_DATA_WORDS], uint32_t code[POWERVR_COMPUTE_PDS_CODE_MAX], unsigned *code_words);
int powervr_compute_store_program(uint64_t base, unsigned steps, uint8_t *output, size_t room, size_t *length);
/*
 * The probe programs of the compute check (compute-check.c), each a few
 * groups that tell one part of the shading cluster's integer path apart:
 * a multiply-add of constants, the instance number, the instance number
 * times four, and the word each instance addresses by its number.
 */
#define POWERVR_COMPUTE_PROBE_MADD		0U
#define POWERVR_COMPUTE_PROBE_INSTANCE		1U
#define POWERVR_COMPUTE_PROBE_INSTANCE_MADD	2U
#define POWERVR_COMPUTE_PROBE_ADDRESS		3U
#define POWERVR_COMPUTE_PROBE_STRIDE_16		4U
#define POWERVR_COMPUTE_PROBE_STRIDE_64		5U
#define POWERVR_COMPUTE_PROBE_STRIDE_128	6U
#define POWERVR_COMPUTE_PROBE_ADDRESS_WAIT	7U
#define POWERVR_COMPUTE_PROBE_COMPUTED		8U
#define POWERVR_COMPUTE_PROBE_ZERO_MADD		9U
#define POWERVR_COMPUTE_PROBE_CONSTANT		10U
#define POWERVR_COMPUTE_PROBE_MOVED		11U
#define POWERVR_COMPUTE_PROBE_LIKE_STORE	12U
#define POWERVR_COMPUTE_PROBE_LIKE_STORE_NUMBER	13U
#define POWERVR_COMPUTE_PROBE_LIKE_STORE_IN_PLACE 14U
#define POWERVR_COMPUTE_PROBE_LIKE_STORE_EARLY	15U
#define POWERVR_COMPUTE_PROBE_REWRITE		16U
#define POWERVR_COMPUTE_PROBE_REWRITE_LATER	17U
#define POWERVR_COMPUTE_PROBE_NOP_BETWEEN	18U
#define POWERVR_COMPUTE_PROBE_WAIT_BETWEEN	19U
#define POWERVR_COMPUTE_PROBES			20U

/* The bytes between two instances' words in each probe (4 but for the strided ones). */
#define POWERVR_COMPUTE_PROBE_STRIDE_MAX	128U

/* The temporaries a probe program uses. */
#define POWERVR_COMPUTE_PROBE_TEMPORARIES	15U

int powervr_compute_probe_program(unsigned kind, uint64_t base, uint8_t *output, size_t room, size_t *length);
uint32_t powervr_compute_probe_stride(unsigned kind);
int powervr_compute_temps_program(uint64_t base, uint8_t *output, size_t room, size_t *length);
int powervr_compute_fill_program(uint64_t base, unsigned instances, uint32_t multiplier, uint32_t addend, uint8_t *output, size_t room, size_t *length);
void powervr_compute_command(uint64_t stream, uint64_t resume, uint32_t words[POWERVR_COMPUTE_COMMAND_WORDS]);

#endif /* KERN_DRIVERS_GPU_POWERVR_COMPUTE_PROGRAM_H */
