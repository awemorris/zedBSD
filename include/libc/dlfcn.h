/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBC_DLFCN_H
#define LIBC_DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100

/*
 * An extension beyond POSIX, with the value FreeBSD and glibc give it: the
 * object stays mapped after its last dlclose(), until the process ends.
 */
#define RTLD_NODELETE 0x1000

/*
 * An extension beyond POSIX, with FreeBSD's value (glibc has the same
 * name): dlopen() gives a handle on the object only when it is loaded
 * already, and loads nothing; otherwise it reports NULL.
 */
#define RTLD_NOLOAD 0x2000

/*
 * Searches the global symbol scope in dlsym(), using FreeBSD's sentinel
 * value so it does not collide with NULL or a valid dlopen() handle.
 */
#define RTLD_DEFAULT ((void *)-2)

typedef struct dl_info {
	const char *dli_fname;
	void *dli_fbase;
	const char *dli_sname;
	void *dli_saddr;
} Dl_info;

/* POSIX.1-2024's name for the information dladdr() gives. */
typedef Dl_info Dl_info_t;

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
void *dlvsym(void *, const char *, const char *);
int dladdr(const void *address, Dl_info *information);
int dlclose(void *);
char *dlerror(void);

#ifdef __cplusplus
}
#endif

#endif
