/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * sessiond's answer to "POWER suspend" read into a sleep's outcome
 * (power-outcome.c, ws052-p011).  It knows nothing of the backend's state
 * or descriptors, so the host tests run it alone.
 */

#ifndef KL_BACKEND_POWER_OUTCOME_H
#define KL_BACKEND_POWER_OUTCOME_H

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

int kl_backend_power_parse_outcome(const char *line, struct kl_backend_power_outcome *outcome);

#endif
