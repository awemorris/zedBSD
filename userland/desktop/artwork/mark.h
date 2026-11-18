/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The mark as coverage masks, for the programs that draw it
 * themselves: the compositor (the greeter and the lock screen) and the file
 * manager (its Welcome and its empty folders).  The source is compiled into
 * each of them.
 */

#ifndef KEILAND_ARTWORK_MARK_H
#define KEILAND_ARTWORK_MARK_H

#include <stddef.h>
#include <stdint.h>

/*
 * The layers of the mark, drawn in this order, each in its own colour: the
 * bar, the bar's shade (deeper towards its foot), the leaf, the leaf's
 * shade (deeper towards its lower point), the overlap of the two panes
 * (darker, as two sheets of tinted glass are), the soft light along every
 * edge, and the sheen on the upper part of the panes (ws035-p109).  The
 * last two are drawn in white.
 */
enum kl_mark_layer {
	KL_MARK_BAR,
	KL_MARK_BAR_SHADE,
	KL_MARK_LEAF,
	KL_MARK_LEAF_SHADE,
	KL_MARK_OVERLAP,
	KL_MARK_RIM,
	KL_MARK_SHEEN,
	KL_MARK_LAYERS
};

void kl_mark_raster(unsigned layer, unsigned pixels, uint8_t *coverage, size_t stride);

#endif
