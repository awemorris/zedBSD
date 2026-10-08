/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The interfaces of Keiland's audio streams (libkeiland/audio/
 * kl-audio-protocol.h has the opcodes, WS191), described as wayland-scanner
 * would make them, over libwayland's marshalling.  They stay inside the
 * library (exports.map).
 */

#include "audio-protocol.h"

#include "userland/desktop/libkeiland/audio/kl-audio-protocol.h"

#include <stddef.h>

/* create_stream's argument types: the stream it makes, then five words. */
static const struct wl_interface *audio_create_types[] = {
	&kl_audio_stream_v1_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The arguments of messages that name no interface (at most four, ready's). */
static const struct wl_interface *audio_plain_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

/* The requests of kl_audio_v1. */
static const struct wl_message audio_requests[] = {
	{ "destroy", "", NULL },
	{ "create_stream", "nuuuuu", audio_create_types },
};

/* kl_audio_v1: two requests and no event.  It lives for the program. */
const struct wl_interface kl_audio_v1_interface = {
	KL_AUDIO_NAME,
	KL_AUDIO_VERSION,
	2,
	audio_requests,
	0,
	NULL
};

/* The requests of kl_audio_stream_v1. */
static const struct wl_message audio_stream_requests[] = {
	{ "destroy", "", NULL },
	{ "start", "u", audio_plain_types },
	{ "stop", "u", audio_plain_types },
	{ "flush", "u", audio_plain_types },
	{ "drain", "u", audio_plain_types },
};

/* The events of kl_audio_stream_v1. */
static const struct wl_message audio_stream_events[] = {
	{ "ready", "huuu", audio_plain_types },
	{ "failed", "u", audio_plain_types },
	{ "result", "uu", audio_plain_types },
	{ "drained", "u", audio_plain_types },
	{ "underrun", "u", audio_plain_types },
	{ "lost", "u", audio_plain_types },
};

/* kl_audio_stream_v1: five requests and six events.  It lives for the program. */
const struct wl_interface kl_audio_stream_v1_interface = {
	KL_AUDIO_STREAM_NAME,
	KL_AUDIO_VERSION,
	5,
	audio_stream_requests,
	6,
	audio_stream_events
};
