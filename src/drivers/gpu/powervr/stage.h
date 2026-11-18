/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stage lines of the PowerVR driver: one short line a
 * stage on the console, "powervr: <stage>: ok" or "fail(<reason>)", that
 * says how far the driver has come since boot, and the same as the text of
 * hw.gpu.powervr.stages.  This part holds only the rules (what to print,
 * what the text says) and builds on the host too; stage-log.c keeps the
 * one record of the boot and prints.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_STAGE_H
#define KERN_DRIVERS_GPU_POWERVR_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The stages, in the order a boot reaches them. */
enum powervr_stage {
	POWERVR_STAGE_FIRMWARE,
	POWERVR_STAGE_RENDER_NODE,
	POWERVR_STAGE_COMPUTE,
	POWERVR_STAGE_TA,
	POWERVR_STAGE_3D,
	POWERVR_STAGE_TRANSFER,
	POWERVR_STAGE_PRESENT,
	POWERVR_STAGE_COUNT
};

/* Why a stage failed: a meaning, not an errno. */
enum powervr_stage_reason {
	POWERVR_REASON_ERROR,
	POWERVR_REASON_ENOENT,
	POWERVR_REASON_TIMEOUT,
	POWERVR_REASON_BAD_IMAGE,
	POWERVR_REASON_EBUSY,
	POWERVR_REASON_COHERENCY,
	POWERVR_REASON_FAULT,
	POWERVR_REASON_SPIRV_SUBSET,
	POWERVR_REASON_FREE_LIST,
	POWERVR_REASON_FORMAT,
	POWERVR_REASON_IMPORT,
	POWERVR_REASON_STRIDE,
	POWERVR_REASON_CLAIM,
	POWERVR_REASON_RESET,
	POWERVR_REASON_COUNT
};

/* The failures of one stage printed before the rest are only counted. */
#define POWERVR_STAGE_FAILS_SHOWN	3U

/* No value with a failure. */
#define POWERVR_STAGE_NO_VALUE		UINT64_MAX

/* The most bytes of one stage line, its NUL included. */
#define POWERVR_STAGE_LINE_MAX		64U

/* What one boot has seen of each stage. */
struct powervr_stage_record {
	uint8_t state[POWERVR_STAGE_COUNT];
	uint8_t reason[POWERVR_STAGE_COUNT];
	uint8_t ok_shown[POWERVR_STAGE_COUNT];
	uint32_t fails[POWERVR_STAGE_COUNT];
};

/* A stage's state in the record. */
#define POWERVR_STAGE_UNSEEN	0U
#define POWERVR_STAGE_OK	1U
#define POWERVR_STAGE_FAILED	2U

bool
powervr_stage_note_ok(
	struct powervr_stage_record *record,
	enum powervr_stage stage,
	char *line,
	size_t capacity);

bool
powervr_stage_note_fail(
	struct powervr_stage_record *record,
	enum powervr_stage stage,
	enum powervr_stage_reason reason,
	char *line,
	size_t capacity);

/*
 * Like powervr_stage_note_fail(), with the value the reason carries: the
 * faulting device address for POWERVR_REASON_FAULT ("fault 0x..."), the
 * firmware's reset reason for POWERVR_REASON_RESET ("reset lockup").  Other
 * reasons ignore it.
 */
bool
powervr_stage_note_fail_value(
	struct powervr_stage_record *record,
	enum powervr_stage stage,
	enum powervr_stage_reason reason,
	uint64_t value,
	char *line,
	size_t capacity);

size_t
powervr_stage_text(
	const struct powervr_stage_record *record,
	char *text,
	size_t capacity);

enum powervr_stage_reason
powervr_stage_reason_of(
	int error);

#endif
