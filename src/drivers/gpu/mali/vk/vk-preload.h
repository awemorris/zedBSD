/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pre-frame draw of a render pass that loads its colour attachment:
 * before the pass's own draws, every tile a primitive touches is filled
 * from the attachment's memory by a small fragment program, so the draws
 * blend over what the attachment held and the tiles no primitive touched
 * are not written back at all.
 *
 * The program is compiled once for the node, the first time a pass
 * loads; its code is copied into each session's space the first time
 * one of the session's passes loads.  When either cannot be made, the
 * caller draws the pass on cleared tiles as before.
 */

#ifndef KERN_DRIVERS_GPU_MALI_VK_PRELOAD_H
#define KERN_DRIVERS_GPU_MALI_VK_PRELOAD_H

#include <stdint.h>

#include "drivers/gpu/mali/vk/vk-build.h"

/*
 * What a pass's pre-frame draw reads back: the view of the loaded
 * colour attachment (render target 0), the blend descriptor that writes
 * the program's colour into that target, and the pass's thread storage.
 */
struct mali_vk_preload {
	const struct mali_vk_image_view *view;
	uint32_t blend[4];
	uint64_t storage;
};

/*
 * Writes a pass's frame shader draws into the stage and reports their
 * address: 0 with *draws set, ENOTSUP when the program or its code is
 * not there (said once; the pass then draws on cleared tiles), or the
 * stage's error.
 */
int mali_vk_preload_draws(struct mali_vk_build *build, const struct mali_vk_preload *preload, uint64_t *draws);

#endif
