/* SWAP-T007: compile-only ILP32/LP64 runtime-swap UAPI contract. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <zedbsd/system.h>

_Static_assert((ZEDBSD_SYSTEM_SWAP_ADD & 0xffUL) == 9UL,
    "SWAP_ADD ioctl number changed");
_Static_assert((ZEDBSD_SYSTEM_SWAP_REMOVE & 0xffUL) == 10UL,
    "SWAP_REMOVE ioctl number changed");
_Static_assert((ZEDBSD_SYSTEM_GET_SWAP_SOURCE & 0xffUL) == 11UL,
    "GET_SWAP_SOURCE ioctl number changed");
_Static_assert((ZEDBSD_SYSTEM_SWAP_ADD & ZEDBSD_IOC_INOUT) == ZEDBSD_IOC_IN,
    "SWAP_ADD must be _IOW");
_Static_assert((ZEDBSD_SYSTEM_SWAP_REMOVE & ZEDBSD_IOC_INOUT) ==
    ZEDBSD_IOC_IN, "SWAP_REMOVE must be _IOW");
_Static_assert((ZEDBSD_SYSTEM_GET_SWAP_SOURCE & ZEDBSD_IOC_INOUT) ==
    ZEDBSD_IOC_INOUT, "GET_SWAP_SOURCE must be _IOWR");
_Static_assert(((ZEDBSD_SYSTEM_SWAP_ADD >> 16) & 0x1fffUL) ==
    sizeof(struct system_swap_control), "SWAP_ADD ioctl size changed");
_Static_assert(((ZEDBSD_SYSTEM_SWAP_REMOVE >> 16) & 0x1fffUL) ==
    sizeof(struct system_swap_control), "SWAP_REMOVE ioctl size changed");
_Static_assert(((ZEDBSD_SYSTEM_GET_SWAP_SOURCE >> 16) & 0x1fffUL) ==
    sizeof(struct system_swap_source_info),
    "GET_SWAP_SOURCE ioctl size changed");

int swap_uapi_layout_compiles;
