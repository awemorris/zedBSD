/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBMEDIA_AAC_SYNTH_H
#define LIBMEDIA_AAC_SYNTH_H
#include "aac-frame.h"

/* One channel's overlap state, keyed by its element identity in the owning decoder. */
struct media_aac_history {
	float overlap[1024];
	unsigned shape;
};

/* One decoder's reusable transform scratch, never shared between synthesis threads. */
struct media_aac_transform {
	float real[2048];
	float imaginary[2048];
	float cosine[1024];
	float windowed[2048];
};

int media_aac_reconstruct(struct media_aac_frame *frame, uint32_t *random);
int media_aac_synthesize(const struct media_aac_channel *channel, struct media_aac_history *history, struct media_aac_transform *transform, float pcm[1024]);
#endif
