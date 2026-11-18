/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The alignment table a Series6 firmware checks the host's record
 * layouts against.
 */

#include "drivers/gpu/powervr/s6/fwif.h"

/*
 * The table, built from this driver's own layouts.
 *
 * The firmware compares each word with the value of its own build, so
 * the order of the words is part of the interface: first the kernel's
 * part (the init record, the render context, the common context), then
 * the client's (the trace buffer, the five commands, the free list, the
 * render target, the target data, the counter block, the guest queues
 * and the counter set-up).  For each record its size comes first, then
 * the offsets of the fields that move when a build's options differ.
 * The table is constant and read without a lock.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/rogue/rgx_fwif_alignchecks_km.h
 * (RGXFW_ALIGN_CHECKS_INIT_KM: the records and fields in this order),
 * rgx_fwif_alignchecks.h (RGXFW_ALIGN_CHECKS_INIT0, without the ray
 * tracing part) and rgxfwutils.c (RGXFWSetupAlignChecks: the kernel's
 * part first, the client's after it, in one block).
 */
static const uint32_t alignment_table[POWERVR_S6_ALIGN_WORDS] = {
	sizeof(struct powervr_s6_fw_init),
	offsetof(struct powervr_s6_fw_init, fault_page),
	offsetof(struct powervr_s6_fw_init, pds_code_base),
	offsetof(struct powervr_s6_fw_init, usc_code_base),
	offsetof(struct powervr_s6_fw_init, kernel_queue_control),
	offsetof(struct powervr_s6_fw_init, kernel_queue),
	offsetof(struct powervr_s6_fw_init, firmware_queue_control),
	offsetof(struct powervr_s6_fw_init, firmware_queue),
	offsetof(struct powervr_s6_fw_init, signature_buffers),
	offsetof(struct powervr_s6_fw_init, trace_buffer),
	offsetof(struct powervr_s6_fw_init, compat),
	sizeof(struct powervr_s6_fw_render_context),
	offsetof(struct powervr_s6_fw_render_context, geometry),
	offsetof(struct powervr_s6_fw_render_context, fragment),
	sizeof(struct powervr_s6_fw_common_context),
	offsetof(struct powervr_s6_fw_common_context, memory_context),
	offsetof(struct powervr_s6_fw_common_context, run_node),
	offsetof(struct powervr_s6_fw_common_context, client_queue),
	offsetof(struct powervr_s6_fw_common_context, fence_address),

	sizeof(struct powervr_s6_fw_trace),
	offsetof(struct powervr_s6_fw_trace, log_type),
	offsetof(struct powervr_s6_fw_trace, thread),
	offsetof(struct powervr_s6_fw_trace, locked_up),
	offsetof(struct powervr_s6_fw_trace, overran),
	offsetof(struct powervr_s6_fw_trace, recovered),
	offsetof(struct powervr_s6_fw_trace, false_detect),
	sizeof(struct powervr_s6_fw_geometry_command),
	offsetof(struct powervr_s6_fw_geometry_command, registers),
	sizeof(struct powervr_s6_fw_fragment_command),
	offsetof(struct powervr_s6_fw_fragment_command, registers),
	sizeof(struct powervr_s6_fw_transfer_command),
	offsetof(struct powervr_s6_fw_transfer_command, registers),
	sizeof(struct powervr_s6_fw_2d_command),
	offsetof(struct powervr_s6_fw_2d_command, registers),
	sizeof(struct powervr_s6_fw_compute_command),
	offsetof(struct powervr_s6_fw_compute_command, registers),
	sizeof(struct powervr_s6_fw_free_list),
	offsetof(struct powervr_s6_fw_free_list, list),
	offsetof(struct powervr_s6_fw_free_list, max_pages),
	offsetof(struct powervr_s6_fw_free_list, current_pages),
	offsetof(struct powervr_s6_fw_free_list, hwr_counter),
	sizeof(struct powervr_s6_fw_render_target),
	offsetof(struct powervr_s6_fw_render_target, vheap_table),
	sizeof(struct powervr_s6_fw_target_data),
	offsetof(struct powervr_s6_fw_target_data, mlist),
	offsetof(struct powervr_s6_fw_target_data, free_lists),
	offsetof(struct powervr_s6_fw_target_data, vce_catalogue),
	offsetof(struct powervr_s6_fw_target_data, parent_target),
	offsetof(struct powervr_s6_fw_target_data, state),
	offsetof(struct powervr_s6_fw_target_data, partial_renders),
	sizeof(struct powervr_s6_fw_counter_block),
	offsetof(struct powervr_s6_fw_counter_block, counter_config),
	sizeof(struct powervr_s6_fw_guest_queues),
	offsetof(struct powervr_s6_fw_guest_queues, guest),
	offsetof(struct powervr_s6_fw_guest_queues, kernel_queue_control),
	offsetof(struct powervr_s6_fw_guest_queues, kernel_queue),
	offsetof(struct powervr_s6_fw_guest_queues, firmware_queue_control),
	offsetof(struct powervr_s6_fw_guest_queues, firmware_queue),
	sizeof(struct powervr_s6_fw_counters),
	offsetof(struct powervr_s6_fw_counters, custom)
};

/*
 * Builds the alignment table the host hands the firmware.
 */
size_t
powervr_s6_fw_alignment_table(
	uint32_t *words,
	size_t capacity)
{
	size_t index;

	/* Refuses a buffer that cannot hold the whole table. */
	if (capacity < POWERVR_S6_ALIGN_WORDS)
		return 0;

	/* Copies the table word by word into the buffer the firmware will read. */
	for (index = 0; index < POWERVR_S6_ALIGN_WORDS; index++)
		words[index] = alignment_table[index];

	/* Succeeded: reports how many words were written. */
	return POWERVR_S6_ALIGN_WORDS;
}

/*
 * Compares two alignment tables.
 */
size_t
powervr_s6_fw_alignment_compare(
	const uint32_t *left,
	const uint32_t *right,
	size_t count)
{
	size_t index;

	/* Looks for the first word on which the two disagree. */
	for (index = 0; index < count; index++) {
		if (left[index] != right[index])
			return index;
	}

	/* The two tables agree throughout. */
	return count;
}
