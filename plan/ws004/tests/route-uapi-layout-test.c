/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <stddef.h>
#include <zedbsd/route.h>

_Static_assert(sizeof(struct rtm_ifinfo) == 56U, "rtm_ifinfo size");
_Static_assert(offsetof(struct rtm_ifinfo, rtm_sequence) == 8U,
    "rtm_ifinfo sequence offset");
_Static_assert(offsetof(struct rtm_ifinfo, rtm_device_generation) == 16U,
    "rtm_ifinfo generation offset");
_Static_assert(offsetof(struct rtm_ifinfo, rtm_ifindex) == 24U,
    "rtm_ifinfo ifindex offset");
_Static_assert(offsetof(struct rtm_ifinfo, rtm_reserved) == 40U,
    "rtm_ifinfo reserved offset");

int
main(void)
{
	return 0;
}
