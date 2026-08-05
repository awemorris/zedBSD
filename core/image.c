/*
 * Boots image-loader dispatch
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "core/image.h"

int boots_image_boot(const struct boots_image_loader *loader,
		      struct boots_filesystem *filesystem, const char *path,
		      const char *arguments)
{
	struct boots_file file;

	if (!loader || !loader->probe || !loader->load ||
	    !boots_fs_open(filesystem, path, &file) || !loader->probe(&file))
		return 0;
	return loader->load(&file, arguments ? arguments : "");
}
