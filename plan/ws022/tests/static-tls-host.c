/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <zedbsd/rtld-abi.h>
#include <zedbsd/syscall.h>
#include <zedbsd/thread.h>

static struct __rtld_tcb *current;
static unsigned live;
static int fail_map;
static int fail_set;

void *
mmap(void *address, size_t size, int protection, int flags, int fd, off_t offset)
{
	void *result;

	(void)address;
	(void)protection;
	(void)flags;
	(void)fd;
	(void)offset;
	if (fail_map)
		return MAP_FAILED;
	assert(posix_memalign(&result, 4096, size) == 0);
	memset(result, 0, size);
	live++;
	return result;
}

int
munmap(void *address, size_t size)
{
	assert(address != NULL && size >= 4096);
	assert(live != 0);
	live--;
	free(address);
	return 0;
}

intptr_t
__syscall6(uint32_t number, uintptr_t op, uintptr_t value, uintptr_t a,
    uintptr_t b, uintptr_t c, uintptr_t d)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	assert(number == ZEDBSD_SYS_thread_self);
	if (op == ZEDBSD_THREAD_SELF_GET_TLS)
		return (intptr_t)current;
	assert(op == ZEDBSD_THREAD_SELF_SET_TLS);
	if (fail_set)
		return -1;
	current = (struct __rtld_tcb *)value;
	return 0;
}

int
main(void)
{
	struct __rtld_tcb parent;
	struct __rtld_tcb *child;
	unsigned char template[4] = {1, 2, 3, 4};
	unsigned char *data;
	unsigned i;
	unsigned j;

	assert(__rtld_thread_alloc(NULL, NULL) == -1);
	fail_map = 1;
	child = (void *)(uintptr_t)1;
	assert(__rtld_thread_alloc(NULL, &child) == -1 && child == NULL);
	fail_map = 0;
	fail_set = 1;
	assert(__rtld_thread_attach(&parent) == -1 && live == 0);
	fail_set = 0;
	assert(__rtld_thread_attach(&parent) == 0 && live == 1);
	assert(current->tls.self == (uintptr_t)current);
	assert(__rtld_pthread_private() == &parent);
	assert(__rtld_thread_attach(&parent) == -1);
	child = current;
	__rtld_thread_free(child);
	assert(live == 1);
	current = NULL;
	__rtld_thread_free(child);
	assert(live == 0);

	memset(&parent, 0, sizeof(parent));
	parent.tls.self = (uintptr_t)&parent;
	parent.tls.template_address = (uintptr_t)template;
	parent.tls.template_size = 4;
	parent.tls.memory_size = 129;
	parent.tls.alignment = 64;
	parent.tls.distance = 192;
	current = &parent;
	for (i = 0; i < 100; i++) {
		assert(__rtld_thread_alloc(&parent, &child) == 0);
		assert(child->tls.self == (uintptr_t)child);
		assert(child->pthread_private == &parent);
		data = (unsigned char *)child - 192;
		assert(((uintptr_t)data & 63U) == 0);
		assert(memcmp(data, template, 4) == 0);
		for (j = 4; j < 129; j++)
			assert(data[j] == 0);
		memset(data, 0xee, 129);
		assert(template[0] == 1);
		__rtld_thread_free(child);
		assert(live == 0);
	}
	fail_map = 1;
	assert(__rtld_thread_alloc(&parent, &child) == -1 && child == NULL);
	assert(live == 0 && template[3] == 4);
	return 0;
}
