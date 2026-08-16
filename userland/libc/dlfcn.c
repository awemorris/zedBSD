/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <dlfcn.h>
#include <zedbsd/rtld-abi.h>

extern void *__zedbsd_rtld_dlopen(const char *, int) __attribute__((weak));
extern void *__zedbsd_rtld_dlsym(void *, const char *) __attribute__((weak));
extern int __zedbsd_rtld_dlclose(void *) __attribute__((weak));
extern char *__zedbsd_rtld_dlerror(void) __attribute__((weak));

static char static_dlerror[] = "dynamic loading is unavailable";
static int static_error_pending;

void *
dlopen(const char *path, int flags)
{
	if (__zedbsd_rtld_dlopen != 0)
		return __zedbsd_rtld_dlopen(path, flags);
	static_error_pending = 1;
	return 0;
}

void *
dlsym(void *handle, const char *name)
{
	if (__zedbsd_rtld_dlsym != 0)
		return __zedbsd_rtld_dlsym(handle, name);
	(void)handle;
	(void)name;
	static_error_pending = 1;
	return 0;
}

int
dlclose(void *handle)
{
	if (__zedbsd_rtld_dlclose != 0)
		return __zedbsd_rtld_dlclose(handle);
	(void)handle;
	static_error_pending = 1;
	return -1;
}

char *
dlerror(void)
{
	if (__zedbsd_rtld_dlerror != 0)
		return __zedbsd_rtld_dlerror();
	if (!static_error_pending)
		return 0;
	static_error_pending = 0;
	return static_dlerror;
}
