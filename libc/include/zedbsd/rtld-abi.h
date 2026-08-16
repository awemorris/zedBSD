/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_RTLD_ABI_H
#define ZEDBSD_RTLD_ABI_H

#include <stddef.h>
#include <stdint.h>

#define ZEDBSD_RTLD_ABI_VERSION 1U
#define ZEDBSD_RTLD_DLERROR_SIZE 192U

struct zedbsd_tls_index {
	uintptr_t module;
	uintptr_t offset;
};

struct zedbsd_rtld_tcb {
	void **dtv;
	size_t dtv_count;
	uint64_t dtv_generation;
	void *pthread_private;
	char dlerror_buf[ZEDBSD_RTLD_DLERROR_SIZE];
	int dlerror_pending;
};

unsigned __zedbsd_rtld_abi_version(void);
void __zedbsd_rtld_startup_init(void);
void __zedbsd_rtld_process_fini(void);
void *__zedbsd_rtld_dlopen(const char *, int);
void *__zedbsd_rtld_dlsym(void *, const char *);
int __zedbsd_rtld_dlclose(void *);
char *__zedbsd_rtld_dlerror(void);
int __zedbsd_rtld_thread_alloc(void *, struct zedbsd_rtld_tcb **);
void __zedbsd_rtld_thread_free(struct zedbsd_rtld_tcb *);
int __zedbsd_rtld_thread_attach(void *);
void *__zedbsd_rtld_pthread_private(void);
void __zedbsd_rtld_fork_prepare(void);
void __zedbsd_rtld_fork_parent(void);
void __zedbsd_rtld_fork_child(void);
void *__tls_get_addr(const struct zedbsd_tls_index *);

#endif
