/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_RTLD_ABI_H
#define ZEDBSD_RTLD_ABI_H

#include <stddef.h>
#include <stdint.h>

#define ZEDBSD_RTLD_ABI_VERSION 3U
#define ZEDBSD_RTLD_DLERROR_SIZE 192U

struct __tls_index {
	uintptr_t module;
	uintptr_t offset;
};

struct __rtld_tcb {
	void **dtv;
	size_t dtv_count;
	uint64_t dtv_generation;
	void *pthread_private;
	char dlerror_buf[ZEDBSD_RTLD_DLERROR_SIZE];
	int dlerror_pending;
	/* Runtime-linker private thread registry link. */
	struct __rtld_tcb *rtld_next;
};

/*
 * Data-based import table used by libc.so.  Function relocations on SPARC V9
 * require a writable/executable PLT, while a read-only table needs only data
 * relocations.  Keeping the table in the ABI also gives the private contract
 * an explicit version and size for future extension.
 */
struct __rtld_exports {
	uint32_t abi_version;
	size_t struct_size;
	void (*startup_init)(void);
	void (*process_fini)(void);
	void *(*dlopen)(const char *, int);
	void *(*dlsym)(void *, const char *);
	void *(*dlvsym)(void *, const char *, const char *);
	int (*dlclose)(void *);
	char *(*dlerror)(void);
	int (*thread_alloc)(void *, struct __rtld_tcb **);
	void (*thread_free)(struct __rtld_tcb *);
	int (*thread_attach)(void *);
	void *(*pthread_private)(void);
	void (*fork_prepare)(void);
	void (*fork_parent)(void);
	void (*fork_child)(void);
	void *(*tls_get_addr)(const struct __tls_index *);
};

extern const struct __rtld_exports __rtld_exports;

unsigned __rtld_abi_version(void);
void __rtld_startup_init(void);
void __rtld_process_fini(void);
void *__rtld_dlopen(const char *, int);
void *__rtld_dlsym(void *, const char *);
void *__rtld_dlvsym(void *, const char *, const char *);
int __rtld_dlclose(void *);
char *__rtld_dlerror(void);
int __rtld_thread_alloc(void *, struct __rtld_tcb **);
void __rtld_thread_free(struct __rtld_tcb *);
int __rtld_thread_attach(void *);
void *__rtld_pthread_private(void);
void __rtld_fork_prepare(void);
void __rtld_fork_parent(void);
void __rtld_fork_child(void);
void *__tls_get_addr(const struct __tls_index *);

#endif
