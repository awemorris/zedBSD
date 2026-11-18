/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The render scenarios the test runner can select.
 *
 * They drive the Vulkan executor itself, so they need the node served: the
 * scenario starts a thread and returns, and the thread runs its steps once
 * the node is published, logging one line per step and a verdict line.
 */

#ifndef DRIVERS_GPU_I915_TESTS_RENDER_SCENARIOS_H
#define DRIVERS_GPU_I915_TESTS_RENDER_SCENARIOS_H

struct i915_device;

void drv_i915_test_render_executor(struct i915_device *device);
void drv_i915_test_render_compiler(struct i915_device *device);
void drv_i915_test_render_features(struct i915_device *device);
void drv_i915_test_render_generality(struct i915_device *device);
void drv_i915_test_render_compute(struct i915_device *device);

#endif
