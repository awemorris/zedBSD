/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What a session may ask of the machine's heat and power through sessiond
 * (thermal-rules.c): the words of a THERMAL request, who may ask, and the
 * line that answers it.  Nothing here opens a device or a socket, so the
 * host tests run it alone.
 */

#ifndef SESSIOND_THERMAL_RULES_H
#define SESSIOND_THERMAL_RULES_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* What a THERMAL request asks: a strategy, or a fan's boost. */
#define SESSIOND_THERMAL_STRATEGY	1U
#define SESSIOND_THERMAL_BOOST		2U

/* The strategies a session may choose (the firmware's own state is never chosen). */
#define SESSIOND_THERMAL_PERFORMANCE	1U
#define SESSIOND_THERMAL_BALANCED	2U
#define SESSIOND_THERMAL_POWERSAVE	3U
#define SESSIOND_THERMAL_MANUAL		4U

/* The largest value of a manual axis (100 is the most of the thing). */
#define SESSIOND_THERMAL_AXIS_MAX	100U

/* The fan argument that stands for every fan, and the largest fan number and boost level a request may name. */
#define SESSIOND_THERMAL_FAN_ALL	0xffffffffU
#define SESSIOND_THERMAL_FAN_MAX	255U
#define SESSIOND_THERMAL_LEVEL_MAX	255U

/* What an answered request reports beyond its errno, as bits. */
#define SESSIOND_THERMAL_PARTIAL	0x1U	/* some of the drivers failed */
#define SESSIOND_THERMAL_PENDING	0x2U	/* the drivers had not all answered in time */
#define SESSIOND_THERMAL_SUPERSEDED	0x4U	/* a later choice replaced this one */

/* The longest request (the line after "THERMAL ") and the longest answer, each with its end. */
#define SESSIOND_THERMAL_REQUEST_MAX	64U
#define SESSIOND_THERMAL_ANSWER_MAX	48U

/*
 * A THERMAL request as decided: what it asks; for a strategy, its value
 * and the three axes (0 unless MANUAL); for a boost, the fan (or
 * SESSIOND_THERMAL_FAN_ALL) and the level.
 */
struct sessiond_thermal_request {
	unsigned kind;
	unsigned strategy;
	unsigned speed;
	unsigned noise;
	unsigned gating;
	uint32_t fan;
	uint32_t level;
};

int sessiond_thermal_decide(const char *arguments, uid_t uid, int in_wheel, struct sessiond_thermal_request *request);
int sessiond_thermal_answer(int error, unsigned flags, char *answer, size_t size);

#endif
