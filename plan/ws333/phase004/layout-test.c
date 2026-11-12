/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The layout check of the proposed include/uapi/thermal.h: it compiles the
 * header with its _Static_asserts on the host (-m64 and -m32), next to the
 * kernel's headers whose values the device copies out, and checks that the
 * shared values agree.  (include/kern/thermal_private.h needs the kernel's
 * lock headers and is compared by hand.)
 *   cc -m64 -std=c11 -Wall -Wextra -Werror -DKERN_UAPI_NATIVE \
 *      -Iplan/ws333/proposed/include -Iinclude plan/ws333/phase004/layout-test.c
 */

#include <stdio.h>
#include <uapi/thermal.h>
#include <kern/thermal.h>
#include <kern/cpuperf.h>

_Static_assert(KERN_THERMAL_CPU_HAS_CONTROL == KERN_CPUPERF_CPU_HAS_CONTROL, "cpu flag");
_Static_assert(KERN_THERMAL_CPU_ESTIMATED_MHZ == KERN_CPUPERF_CPU_ESTIMATED_MHZ, "cpu flag");
_Static_assert(KERN_THERMAL_CPU_LIMITED == KERN_CPUPERF_CPU_LIMITED, "cpu flag");
_Static_assert(KERN_THERMAL_CPU_PLATFORM_LIMIT == KERN_CPUPERF_CPU_PLATFORM_LIMIT, "cpu flag");
_Static_assert(KERN_THERMAL_CPU_CUSTOM == KERN_CPUPERF_CPU_CUSTOM, "cpu flag");
_Static_assert(KERN_THERMAL_CPU_READ_SAMPLE == KERN_CPUPERF_READ_SAMPLE, "cpu request");
_Static_assert(KERN_THERMAL_CPU_DOMAIN_CUSTOM == KERN_CPUPERF_DOMAIN_CUSTOM, "domain flag");
_Static_assert(KERN_THERMAL_CPU_DOMAIN_LIMITED == KERN_CPUPERF_DOMAIN_LIMITED, "domain flag");
_Static_assert(KERN_THERMAL_CPU_DOMAIN_PENDING == KERN_CPUPERF_DOMAIN_PENDING, "domain flag");
_Static_assert(KERN_THERMAL_CPU_LEVELS_RANGE == KERN_CPUPERF_LEVELS_RANGE, "levels");
_Static_assert(KERN_THERMAL_CPU_LEVELS_STEPS == KERN_CPUPERF_LEVELS_STEPS, "levels");
_Static_assert(KERN_THERMAL_CPU_SHARE_THREAD == KERN_CPUPERF_SHARE_THREAD, "share");
_Static_assert(KERN_THERMAL_CPU_SHARE_PACKAGE == KERN_CPUPERF_SHARE_PACKAGE, "share");
_Static_assert(KERN_THERMAL_CPU_SET_MINIMUM == KERN_CPUPERF_SET_MINIMUM, "set");
_Static_assert(KERN_THERMAL_CPU_SET_PREFERENCE == KERN_CPUPERF_SET_PREFERENCE, "set");
_Static_assert(KERN_THERMAL_CPU_RELATIVE_FREQUENCY == KERN_CPUPERF_RELATIVE_FREQUENCY, "relative");
_Static_assert(KERN_THERMAL_CPU_RELATIVE_FEEDBACK == KERN_CPUPERF_RELATIVE_FEEDBACK, "relative");
_Static_assert(KERN_THERMAL_CPU_RELATIVE_PROVIDER == KERN_CPUPERF_RELATIVE_PROVIDER, "relative");
_Static_assert(KERN_THERMAL_CPU_CORE_PERFORMANCE == KERN_CPUPERF_CORE_PERFORMANCE, "core");
_Static_assert(KERN_THERMAL_CPU_CORE_EFFICIENCY == KERN_CPUPERF_CORE_EFFICIENCY, "core");
_Static_assert(KERN_THERMAL_CPU_IDLE_STATE_MAX == KERN_CPUPERF_IDLE_STATE_MAX, "idle");
_Static_assert(KERN_THERMAL_UNKNOWN == KERN_CPUPERF_UNKNOWN, "unknown");
_Static_assert(KERN_THERMAL_CPU_MAX == KERN_CPU_MAX, "cpu max");
_Static_assert(sizeof(((struct thermal_cpu_domain *)0)->cpus) / 8U == KERN_CPU_MASK_WORDS, "cpu mask");
_Static_assert(sizeof(((struct thermal_cpu_domain *)0)->steps) / 4U == KERN_CPUPERF_STEP_MAX, "steps");

int
main(void)
{
	printf("layout-test: PASS (pointer %zu bytes; status %zu, sensor %zu, fan %zu, meter %zu, strategy %zu, receiver %zu, event %zu, cpu %zu, domain %zu, idle %zu)\n",
	    sizeof(void *), sizeof(struct thermal_status), sizeof(struct thermal_sensor_info),
	    sizeof(struct thermal_fan_info), sizeof(struct thermal_meter_info), sizeof(struct thermal_strategy),
	    sizeof(struct thermal_receiver_info), sizeof(struct thermal_event), sizeof(struct thermal_cpu_info),
	    sizeof(struct thermal_cpu_domain), sizeof(struct thermal_cpu_idle));
	return 0;
}
