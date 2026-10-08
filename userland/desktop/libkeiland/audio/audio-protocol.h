/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interfaces of Keiland's audio streams as libwayland marshals them
 * (audio-protocol.c; libkeiland/audio/kl-audio-protocol.h has the opcodes).
 * They stay inside the library (exports.map).
 */

#ifndef KEILAND_AUDIO_PROTOCOL_H
#define KEILAND_AUDIO_PROTOCOL_H

#include <wayland-client.h>

extern const struct wl_interface kl_audio_v1_interface;
extern const struct wl_interface kl_audio_stream_v1_interface;

#endif
