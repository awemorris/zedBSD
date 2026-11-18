/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPU commands: native command streams, markers, drain and the capset.
 *
 * A native stream is a header, a table of relocations and a batch.  It is
 * validated, copied into a batch object of the session's pool, its
 * relocations are patched with the named objects' GPU addresses, and it is
 * queued as a request on the engine record the header names.  An empty
 * submission is a marker: a request without a batch on a timeline.
 */

#ifndef DRIVERS_GPU_I915_COMMAND_H
#define DRIVERS_GPU_I915_COMMAND_H

#include <stdint.h>

struct drv_gpu_ops;

/* The native stream header: magic "XI91", version 1, 32 bytes. */
#define I915_STREAM_MAGIC		0x31394958U
#define I915_STREAM_VERSION		1U
#define I915_STREAM_HEADER_BYTES	32U

/* One relocation: the batch dword to patch, a reserved word and the object's handle. */
#define I915_STREAM_RELOCATION_BYTES	16U

/* The bounds that keep a stream within one batch object. */
#define I915_STREAM_MAX_RELOCATIONS	64U
#define I915_STREAM_MAX_DWORDS		16384U

/* The engines a stream header may name. */
#define I915_STREAM_ENGINE_RCS0		0U
#define I915_STREAM_ENGINE_BCS0		1U

/*
 * One decoded native stream.
 *
 * It lives on the submitter's stack while the stream is copied.  The
 * relocation and batch pointers point into the caller's copy of the stream.
 */
struct i915_stream {
	/* The engine the header names, and how many relocations and batch dwords follow. */
	uint32_t engine;
	uint32_t relocation_count;
	uint32_t batch_dwords;

	/* Where the relocation table and the batch start. */
	const uint8_t *relocations;
	const uint32_t *batch;
};

void drv_i915_command_bind_ops(struct drv_gpu_ops *ops);
int drv_i915_stream_parse(const void *buffer, uint32_t bytes, struct i915_stream *stream);

#endif
