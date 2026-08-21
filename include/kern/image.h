/*
 * Image-loader interface
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_IMAGE_H
#define ZEDBSD_IMAGE_H

#include "kern/fs.h"

struct boot_image_loader {
	const char *name;
	int (*probe)(struct bootfs_file *file);
	int (*load)(struct bootfs_file *file, const char *arguments);
};

int boot_image_load(const struct boot_image_loader *loader,
		      struct bootfs *filesystem, const char *path,
		      const char *arguments);

#endif
