/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check recorder shared by the contract tests.
 *
 * Each contract test is one host program.  It announces itself, names each
 * contract group it checks, records every check, and exits with the status
 * the recorder reports: zero only when every check passed.
 */

#ifndef DRIVERS_GPU_I915_TESTS_CONTRACTS_CONTRACT_H
#define DRIVERS_GPU_I915_TESTS_CONTRACTS_CONTRACT_H

/*
 * Marks a parameter a mock operation receives by contract but does not use.
 *
 * It is the driver's own definition (i915.h), repeated here because the
 * mocks do not include the device header.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

void contract_begin(const char *title);
void contract_section(const char *name);
void contract_check(int passed, const char *message);
int contract_end(void);

#endif
