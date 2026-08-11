/*
 * Boots image-loader interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef BOOTS_IMAGE_H
#define BOOTS_IMAGE_H

#include "kern/fs.h"

struct boots_image_loader {
	const char *name;
	int (*probe)(struct boots_file *file);
	int (*load)(struct boots_file *file, const char *arguments);
};

int boots_image_boot(const struct boots_image_loader *loader,
		      struct boots_filesystem *filesystem, const char *path,
		      const char *arguments);

#endif
