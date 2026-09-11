/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef KERN_AMD64_FRAMEBUFFER_MAP_H
#define KERN_AMD64_FRAMEBUFFER_MAP_H
#include <stdint.h>

/* Maps only framebuffer pages; at most the two partial 2 MiB edges need PTs. */
int amd64_framebuffer_map(uint64_t *directory, unsigned capacity,
    uint64_t edges[2][512], const uint64_t edge_physical[2],
    uint64_t base, uint64_t size, uint64_t physical_max);
#endif
