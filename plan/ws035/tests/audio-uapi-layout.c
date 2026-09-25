/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Compile-only audio request layout checks for ILP32 and LP64 callers.
 *
 * Each array typedef needs a positive bound when the layout holds.
 */

#include <uapi/audio.h>
#include <stddef.h>

/* A format is four 32-bit fields. */
typedef char audio_format_size_check[sizeof(struct audio_format) == 16 ? 1 : -1];

/* The ring description is four 32-bit fields. */
typedef char audio_buffer_size_check[sizeof(struct audio_buffer_info) == 16 ? 1 : -1];

/* The 64-bit transfer count sits at offset 16 for both data models. */
typedef char audio_space_transferred_check[offsetof(struct audio_space, transferred) == 16 ? 1 : -1];

/* The space report is 32 bytes with no tail padding difference. */
typedef char audio_space_size_check[sizeof(struct audio_space) == 32 ? 1 : -1];

/* A volume is four 32-bit fields. */
typedef char audio_volume_size_check[sizeof(struct audio_volume) == 16 ? 1 : -1];

/* The ioctl numbers carry the request sizes. */
typedef char audio_space_ioctl_check[KERN_AUDIO_GET_OSPACE == _IOR('A', 4, struct audio_space) ? 1 : -1];

/* ws035-p049: the capability report is four 32-bit fields. */
typedef char audio_caps_size_check[sizeof(struct audio_caps) == 16 ? 1 : -1];

/* The mapped position is 16 bytes with the 64-bit count first for both data models. */
typedef char audio_mmap_position_size_check[sizeof(struct audio_mmap_position) == 16 ? 1 : -1];
typedef char audio_mmap_position_offset_check[offsetof(struct audio_mmap_position, offset) == 12 ? 1 : -1];
typedef char audio_optr_ioctl_check[KERN_AUDIO_GET_OPTR == _IOR('A', 10, struct audio_mmap_position) ? 1 : -1];
