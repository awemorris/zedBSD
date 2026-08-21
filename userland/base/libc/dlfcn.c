/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <dlfcn.h>
#include <zedbsd/rtld-abi.h>

#if !defined(ZEDBSD_DYNAMIC_LIBC)
extern void *__rtld_dlopen(const char *, int) __attribute__((weak));
extern void *__rtld_dlsym(void *, const char *) __attribute__((weak));
extern void *__rtld_dlvsym(void *, const char *, const char *)
    __attribute__((weak));
extern int __rtld_dlclose(void *) __attribute__((weak));
extern char *__rtld_dlerror(void) __attribute__((weak));
#endif

#if !defined(ZEDBSD_DYNAMIC_LIBC)
static char static_dlerror[] = "dynamic loading is unavailable";
static int static_error_pending;
#endif

void *
dlopen(const char *path, int flags)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	return __rtld_exports.dlopen(path, flags);
#else
	if (__rtld_dlopen != 0)
		return __rtld_dlopen(path, flags);
	static_error_pending = 1;
	return 0;
#endif
}

void *
dlvsym(void *handle, const char *name, const char *version)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	return __rtld_exports.dlvsym(handle, name, version);
#else
	if (__rtld_dlvsym != 0)
		return __rtld_dlvsym(handle, name, version);
	(void)handle;
	(void)name;
	(void)version;
	static_error_pending = 1;
	return 0;
#endif
}

void *
dlsym(void *handle, const char *name)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	return __rtld_exports.dlsym(handle, name);
#else
	if (__rtld_dlsym != 0)
		return __rtld_dlsym(handle, name);
	(void)handle;
	(void)name;
	static_error_pending = 1;
	return 0;
#endif
}

int
dlclose(void *handle)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	return __rtld_exports.dlclose(handle);
#else
	if (__rtld_dlclose != 0)
		return __rtld_dlclose(handle);
	(void)handle;
	static_error_pending = 1;
	return -1;
#endif
}

char *
dlerror(void)
{
#if defined(ZEDBSD_DYNAMIC_LIBC)
	return __rtld_exports.dlerror();
#else
	if (__rtld_dlerror != 0)
		return __rtld_dlerror();
	if (!static_error_pending)
		return 0;
	static_error_pending = 0;
	return static_dlerror;
#endif
}
