/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The one-lane model of the Valhall instructions and the IR reference of
 * the Mali compiler's host check (tests/mali-compiler-host-test.c).
 */

#ifndef TESTS_MALI_COMPILER_MODEL_H
#define TESTS_MALI_COMPILER_MODEL_H

#include "drivers/gpu/mali/compiler/shader.h"

#include <stddef.h>
#include <stdint.h>

/* Runs a compiled program on the model against the IR reference for random inputs: 0 when every trial agrees. */
int mali_model_check(const uint32_t *words, size_t count, uint32_t stage, const struct mali_shader_program *program, unsigned trials, const char *name);

#endif /* TESTS_MALI_COMPILER_MODEL_H */
