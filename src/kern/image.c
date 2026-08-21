/*
 * zedBSD image-loader dispatch
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/image.h"

int boot_image_load(const struct boot_image_loader *loader,
		      struct bootfs *filesystem, const char *path,
		      const char *arguments)
{
	struct bootfs_file file;

	if (!loader || !loader->probe || !loader->load ||
	    !bootfs_open(filesystem, path, &file) || !loader->probe(&file))
		return 0;
	return loader->load(&file, arguments ? arguments : "");
}
