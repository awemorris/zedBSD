/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of the Vulkan loader's delayed import: linked with the import
 * library of vulkan-1.dll and --delayload=vulkan-1.dll, the program starts
 * whether the DLL is there or not, looks for it, and calls Vulkan only when
 * it is found.  It prints one line and returns 0, or 1 when the loader is
 * found but its version cannot be read.
 */

#include <windows.h>

#include <stdio.h>

#include <vulkan/vulkan.h>

/*
 * Reports the loader's version, or that there is no loader.
 */
int
main(void)
{
	HMODULE loader;
	uint32_t version;
	VkResult result;

	/* The loader, looked for where Windows keeps it, without calling it. */
	loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (loader == NULL) {
		printf("delay-load: no vulkan-1.dll, no Vulkan call\n");
		return 0;
	}
	FreeLibrary(loader);

	/* The first call loads the DLL through the delayed import. */
	version = 0;
	result = vkEnumerateInstanceVersion(&version);
	if (result != VK_SUCCESS) {
		printf("delay-load: vkEnumerateInstanceVersion returned %d\n", (int)result);
		return 1;
	}
	printf("delay-load: Vulkan %u.%u\n", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version));
	return 0;
}
