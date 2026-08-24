/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DLFCN_H
#define ZEDBSD_DLFCN_H

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100

typedef struct dl_info {
	const char *dli_fname;
	void *dli_fbase;
	const char *dli_sname;
	void *dli_saddr;
} Dl_info;

void *dlopen(const char *, int);
void *dlsym(void *, const char *);
void *dlvsym(void *, const char *, const char *);
int dladdr(const void *address, Dl_info *information);
int dlclose(void *);
char *dlerror(void);

#endif
