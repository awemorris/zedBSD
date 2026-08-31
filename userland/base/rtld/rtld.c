/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * zedBSD ELF runtime linker
 */

#include "userland/base/rtld/rtld.h"

#include <zedbsd/auxv.h>
#include <zedbsd/rtld-abi.h>
#include <zedbsd/syscall.h>
#include <zedbsd/thread.h>
#include <zedbsd/usync.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#if defined(HAL_ARCH_I386)
#define RTLD_MACHINE EM_386
#define RTLD_DATA ELFDATA2LSB
#define RTLD_RELATIVE R_386_RELATIVE
#elif defined(HAL_ARCH_AMD64)
#define RTLD_MACHINE EM_X86_64
#define RTLD_DATA ELFDATA2LSB
#define RTLD_RELATIVE R_X86_64_RELATIVE
#elif defined(HAL_ARCH_ARM64)
#define RTLD_MACHINE EM_AARCH64
#define RTLD_DATA ELFDATA2LSB
#define RTLD_RELATIVE R_AARCH64_RELATIVE
#elif defined(HAL_ARCH_SPARCV9)
#define RTLD_MACHINE EM_SPARCV9
#define RTLD_DATA ELFDATA2MSB
#define RTLD_RELATIVE R_SPARC_RELATIVE
#else
#error unsupported runtime-linker architecture
#endif

struct rtld_object {
	char path[RTLD_PATH_MAX];
	uintptr_t base;
	int type;
	Elf_Phdr phdr[64];
	unsigned phnum;
	Elf_Dyn *dynamic;
	size_t dynamic_count;
	const char *strtab;
	size_t strsz;
	Elf_Sym *symtab;
	uint32_t *hash;
	Elf_Addr *gnu_bloom;
	uint32_t *gnu_bucket;
	uint32_t *gnu_chain;
	uint32_t gnu_bucket_count;
	uint32_t gnu_symbol_offset;
	uint32_t gnu_bloom_count;
	uint32_t gnu_bloom_shift;
	uint32_t symbol_count;
	Elf_Versym *versym;
	Elf_Addr verdef_value;
	Elf_Addr verneed_value;
	uint32_t verdef_count;
	uint32_t verneed_count;
	Elf_Rel *rel;
	size_t rel_count;
	Elf_Rela *rela;
	size_t rela_count;
	void *jmprel;
	size_t jmprel_size;
	int pltrel;
	uintptr_t init;
	uintptr_t fini;
	uintptr_t *init_array;
	size_t init_count;
	uintptr_t *fini_array;
	size_t fini_count;
	uintptr_t *preinit_array;
	size_t preinit_count;
	uint32_t needed_offset[RTLD_NEEDED_MAX];
	struct rtld_object *needed[RTLD_NEEDED_MAX];
	unsigned needed_count;
	const char *rpath;
	const char *runpath;
	struct rtld_object *loader_parent;
	unsigned relative_done;
	unsigned relocating;
	unsigned relocated;
	unsigned initializing;
	unsigned initialized;
	dev_t device;
	ino_t inode;
	unsigned has_identity;
	uintptr_t mapping_start[64];
	size_t mapping_size[64];
	unsigned mapping_count;
	uintptr_t tls_module_id;
	unsigned active;
	unsigned unloading;
	unsigned permanent;
	unsigned direct_refs;
	unsigned dependency_refs;
	uint32_t generation;
	struct {
		struct __tls_index index;
	} tlsdesc_argument[64];
	unsigned tlsdesc_count;
};

struct rtld_tlsdesc {
	uintptr_t resolver;
	uintptr_t argument;
};

#define RTLD_HANDLE_MAX 64U
#define RTLD_HANDLE_MAGIC 0x5a444c48U

struct rtld_handle {
	uint32_t magic;
	uint32_t generation;
	struct rtld_object *object;
	unsigned references;
	unsigned active;
	unsigned main_scope;
};

struct rtld_tls_module {
	uintptr_t id;
	const void *init_image;
	size_t file_size;
	size_t memory_size;
	size_t alignment;
	struct rtld_object *owner;
	unsigned active;
};

static struct rtld_object objects[RTLD_OBJECT_MAX];
static unsigned object_count;
static struct rtld_object *main_object;
static struct rtld_object *interpreter_object;
static struct rtld_object *initialization_order[RTLD_OBJECT_MAX];
static unsigned initialization_count;
static unsigned startup_initialized;
static unsigned process_finalized;
static struct rtld_handle handles[RTLD_HANDLE_MAX];
static uint32_t next_handle_generation = 1;
static volatile uint32_t loader_lock_word;
static uintptr_t loader_lock_owner;
static unsigned loader_lock_depth;
static char loader_error[ZEDBSD_RTLD_DLERROR_SIZE];
static unsigned loader_error_pending;
static struct rtld_tls_module tls_modules[RTLD_OBJECT_MAX + 1U];
static uintptr_t tls_module_count;
static uint64_t tls_generation;
static struct __rtld_tcb *rtld_threads;
static uint32_t next_object_generation = 1;

__attribute__((visibility("default")))
const struct __rtld_exports __rtld_exports = {
    .abi_version = ZEDBSD_RTLD_ABI_VERSION,
    .struct_size = sizeof(struct __rtld_exports),
    .startup_init = __rtld_startup_init,
    .process_fini = __rtld_process_fini,
    .dlopen = __rtld_dlopen,
    .dlsym = __rtld_dlsym,
    .dlvsym = __rtld_dlvsym,
    .dlclose = __rtld_dlclose,
    .dlerror = __rtld_dlerror,
    .thread_alloc = __rtld_thread_alloc,
    .thread_free = __rtld_thread_free,
    .thread_attach = __rtld_thread_attach,
    .pthread_private = __rtld_pthread_private,
    .fork_prepare = __rtld_fork_prepare,
    .fork_parent = __rtld_fork_parent,
    .fork_child = __rtld_fork_child,
    .tls_get_addr = __tls_get_addr,
    .dladdr = __rtld_dladdr,
};

static intptr_t syscall6(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5);
static void *tls_map(size_t size);
static uintptr_t page_ceil(uintptr_t value);
static intptr_t map_call(uintptr_t address, size_t size, int prot, int flags, int fd, uintptr_t offset);
static int raw_error(intptr_t value);
static void tls_unmap(void *address, size_t size);
static void loader_lock(void);
static uintptr_t current_tid(void);
static void loader_unlock(void);
static void *allocate_tls_block(const struct rtld_tls_module *module);
static void initialize_object(struct rtld_object *object);
static void clear_loader_error(void);
static void set_loader_error(const char *message);
static struct rtld_handle *allocate_handle(struct rtld_object *object, int main_scope);
static const char *dlopen_bare_name(const char *path);
static int preflight_dlopen_file(int fd);
static int valid_elf_header(const Elf_Ehdr *header, int expected_type);
static int validate_file_programs(const Elf_Ehdr *header, const Elf_Phdr *phdr, off_t file_size);
static int temporary_writable_plt(const Elf_Phdr *program);
static uintptr_t page_floor(uintptr_t value);
static struct rtld_object *load_object(const char *name, struct rtld_object *requester);
static intptr_t open_dependency(const char *name, size_t name_length, const struct rtld_object *requester, char path[RTLD_PATH_MAX]);
static intptr_t open_search_list(const char *list, const struct rtld_object *owner, const char *name, size_t name_length, char path[RTLD_PATH_MAX]);
static intptr_t open_search_candidate(const char *directory, size_t directory_length, const char *name, size_t name_length, char path[RTLD_PATH_MAX]);
static struct rtld_object *find_identity(const struct stat *status);
static struct rtld_object *new_object(const char *path);
static void copy_path(char destination[RTLD_PATH_MAX], const char *source);
static void map_one_segment(struct rtld_object *object, int fd, const Elf_Phdr *program, int choose_base);
static int segment_prot(uint32_t flags);
static void remember_mapping(struct rtld_object *object, uintptr_t start, size_t size);
static void parse_dynamic(struct rtld_object *object);
static uintptr_t object_pointer(const struct rtld_object *object, Elf_Addr value, size_t size, uint32_t required);
static int object_contains(const struct rtld_object *object, uintptr_t address, size_t size, uint32_t required);
static size_t object_readable_bytes(const struct rtld_object *object, Elf_Addr value);
static void validate_verdef(struct rtld_object *object);
static Elf_Addr version_offset(Elf_Addr value, uint32_t offset);
static const char *dynamic_string(struct rtld_object *object, uint32_t offset);
static int bounded_string(const char *string, size_t capacity, size_t *length_out);
static void validate_verneed(struct rtld_object *object);
static void register_tls_module(struct rtld_object *object);
static void load_dependencies(struct rtld_object *object);
static void relocate_object(struct rtld_object *object);
static void apply_value(struct rtld_object *object, uintptr_t offset, uint32_t type, uint32_t symbol_index, uintptr_t addend, int is_rela);
static uintptr_t resolve_relocation_symbol(struct rtld_object *object, uint32_t index);
static uintptr_t symbol_value(struct rtld_object *object, const Elf_Sym *symbol);
static uintptr_t lookup_symbol_version(const char *name, const char *required_version, int weak);
static int reserved_loader_symbol(const char *name);
static Elf_Sym *lookup_in_object_version(struct rtld_object *object, const char *name, const char *required_version);
static Elf_Sym *lookup_gnu_hash(struct rtld_object *object, const char *name, const char *required_version);
static uint32_t gnu_hash_name(const char *name);
static Elf_Sym *match_symbol(struct rtld_object *object, uint32_t index, const char *name, const char *required_version);
static int symbol_version_matches(struct rtld_object *object, uint32_t symbol_index, const char *required_version);
static const char *defined_version_name(struct rtld_object *object, uint16_t version_index);
static uint32_t elf_hash(const char *name);
static const char *relocation_version_name(struct rtld_object *object, uint32_t symbol_index);
static const char *required_version_name(struct rtld_object *object, uint16_t version_index);
static Elf_Sym *resolve_tls_symbol(struct rtld_object *object, uint32_t index, struct rtld_object **owner);
static void unload_object_locked(struct rtld_object *object);
static void remove_initialization_record(struct rtld_object *object);
static void finalize_object_unlocked(struct rtld_object *object);
static void *rtld_dlsym_common(void *value, const char *name, const char *version);
static struct rtld_handle *validate_handle(void *value);
static uintptr_t lookup_global_optional(const char *name, const char *version, int *found);
static uintptr_t lookup_handle_graph(struct rtld_object *object, const char *name, const char *version, uint32_t *visited, int *found);
static int bootstrap_relative(uintptr_t base, const Elf_Phdr *phdr, unsigned phnum);
static void setup_premapped_object(struct rtld_object *object, uintptr_t base, const Elf_Phdr *phdr, unsigned phnum, int type);
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64)
static void install_tlsdesc(struct rtld_object *object, uintptr_t address, uint32_t symbol_index, uintptr_t addend);
#endif
#if defined(HAL_ARCH_SPARCV9)
static void sparcv9_patch_jmp_slot(uint32_t *where, uintptr_t value);
#endif

/*
 * Implements the rtld debug operation.
 */
void
rtld_debug(
	const char *message)
{
	/* Handles the message availability. */
	if (message != NULL) {
		(void)syscall6(ZEDBSD_SYS_write, 2, (uintptr_t)message,
			       rtld_strlen(message), 0, 0, 0);
	}
}

/*
 * Implements the rtld fatal operation.
 */
void
rtld_fatal(
	const char *message)
{
	rtld_debug("ld.so: ");
	rtld_debug(message != NULL ? message : "runtime linker failure");
	rtld_debug("\n");
	(void)syscall6(ZEDBSD_SYS_exit, 127, 0, 0, 0, 0, 0);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
	}
}

/*
 * Implements the rtld abi version operation.
 */
__attribute__((visibility("default"))) unsigned
__rtld_abi_version(
	void)
{
	/* Returns the computed result. */
	return ZEDBSD_RTLD_ABI_VERSION;
}

/*
 * Implements the rtld thread alloc operation.
 */
__attribute__((visibility("default"))) int
__rtld_thread_alloc(
	void *pthread_private,
	struct __rtld_tcb **out)
{
	struct __rtld_tcb *tcb;
	void **dtv;

	/* Handles the out availability. */
	if (out == NULL)
		return -1;
	*out = NULL;
	tcb = tls_map(sizeof(*tcb));

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return -1;
	dtv = tls_map((RTLD_OBJECT_MAX + 1U) * sizeof(*dtv));

	/* Handles the dtv availability. */
	if (dtv == NULL) {
		tls_unmap(tcb, sizeof(*tcb));

		/* Reports operation failure. */
		return -1;
	}
	rtld_memset(tcb, 0, sizeof(*tcb));
	rtld_memset(dtv, 0, (RTLD_OBJECT_MAX + 1U) * sizeof(*dtv));
	tcb->dtv = dtv;
	tcb->dtv_count = RTLD_OBJECT_MAX + 1U;
	tcb->dtv_generation = tls_generation;
	tcb->pthread_private = pthread_private;
	loader_lock();
	tcb->rtld_next = rtld_threads;
	rtld_threads = tcb;
	loader_unlock();
	*out = tcb;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rtld thread free operation.
 */
__attribute__((visibility("default"))) void
__rtld_thread_free(
	struct __rtld_tcb *tcb)
{
	intptr_t current;
	uintptr_t id;
	struct __rtld_tcb **link;

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return;
	current = syscall6(ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_GET_TLS,
			   0, 0, 0, 0, 0);

	/* Handles an operation failure. */
	if (!raw_error(current) && (uintptr_t)current == (uintptr_t)tcb)
		rtld_fatal("attempt to free current thread TLS");
	loader_lock();

	/* Process each element required by the operation. */
	for (link = &rtld_threads; *link != NULL; link = &(*link)->rtld_next) {
		/* Handles the link condition. */
		if (*link == tcb) {
			*link = tcb->rtld_next;
			break;
		}
	}
	loader_unlock();

	/* Handles the dtv availability. */
	if (tcb->dtv != NULL) {
		/* Process each remaining element. */
		for (id = 1; id < tcb->dtv_count && id <= tls_module_count;
		     id++) {
			/* Handles the tcb condition. */
			if (tcb->dtv[id] != NULL) {
				tls_unmap(tcb->dtv[id],
					  tls_modules[id].memory_size);
			}
		}
	}
	tls_unmap(tcb->dtv, (RTLD_OBJECT_MAX + 1U) * sizeof(*tcb->dtv));
	tcb->dtv = NULL;
	tls_unmap(tcb, sizeof(*tcb));
}

/*
 * Implements the rtld thread attach operation.
 */
__attribute__((visibility("default"))) int
__rtld_thread_attach(
	void *pthread_private)
{
	intptr_t value;
	struct __rtld_tcb *tcb;

	value = syscall6(ZEDBSD_SYS_thread_self,
				  ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(value))
		return -1;

	/* Validates the current value. */
	if (value == 0) {
		/* Handles a failed rtld thread alloc operation. */
		if (__rtld_thread_alloc(pthread_private, &tcb) != 0)
			return -1;
		value =
		    syscall6(ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_SET_TLS,
			     (uintptr_t)tcb, 0, 0, 0, 0);

		/* Handles an operation failure. */
		if (raw_error(value)) {
			__rtld_thread_free(tcb);

			/* Reports operation failure. */
			return -1;
		}

		/* Reports successful completion. */
		return 0;
	}
	tcb = (struct __rtld_tcb *)(uintptr_t)value;

	/* Handles the dtv availability. */
	if (tcb->dtv == NULL || tcb->pthread_private != NULL)
		return -1;
	tcb->pthread_private = pthread_private;

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rtld pthread private operation.
 */
__attribute__((visibility("default"))) void *
__rtld_pthread_private(
	void)
{
	intptr_t value;

	value = syscall6(ZEDBSD_SYS_thread_self,
				  ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(value) || value == 0)
		return NULL;

	/* Returns the computed result. */
	return ((struct __rtld_tcb *)(uintptr_t)value)->pthread_private;
}

/*
 * Implements the tls get addr operation.
 */
__attribute__((visibility("default"))) void *
__tls_get_addr(
	const struct __tls_index *index)
{
	intptr_t value;
	struct __rtld_tcb *tcb;
	struct rtld_tls_module *module;
	void *block;

	/* Handles the index availability. */
	if (index == NULL || index->module == 0 ||
	    index->module > tls_module_count)
		rtld_fatal("invalid TLS index");
	value = syscall6(ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_GET_TLS, 0,
			 0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(value) || value == 0)
		rtld_fatal("thread has no TLS control block");
	tcb = (struct __rtld_tcb *)(uintptr_t)value;
	module = &tls_modules[index->module];

	/* Handles the dtv availability. */
	if (!module->active || index->offset >= module->memory_size ||
	    tcb->dtv == NULL || index->module >= tcb->dtv_count)
		rtld_fatal("invalid TLS module access");
	block = tcb->dtv[index->module];

	/* Handles the block availability. */
	if (block == NULL) {
		block = allocate_tls_block(module);

		/* Handles the block availability. */
		if (block == NULL)
			rtld_fatal("cannot allocate TLS block");
		tcb->dtv[index->module] = block;
	}
	tcb->dtv_generation = tls_generation;

	/* Returns the computed result. */
	return (unsigned char *)block + index->offset;
}

#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64)
/*
 * Implements the d tlsdesc resolve operation.
 */
__attribute__((visibility("hidden"))) uintptr_t
d_tlsdesc_resolve(
	const struct rtld_tlsdesc *descriptor)
{
	const struct __tls_index *index;
	intptr_t thread_pointer;
	void *address;

	/* Handles the descriptor availability. */
	if (descriptor == NULL || descriptor->argument == 0)
		rtld_fatal("invalid TLSDESC argument");
	index = (const struct __tls_index *)descriptor->argument;
	address = __tls_get_addr(index);
	thread_pointer = syscall6(ZEDBSD_SYS_thread_self,
				  ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(thread_pointer) || thread_pointer == 0)
		rtld_fatal("thread has no TLSDESC base");

	/* Returns the computed result. */
	return (uintptr_t)address - (uintptr_t)thread_pointer;
}
#endif

#if defined(HAL_ARCH_I386)
/*
 * Implements the tls get addr operation.
 */
__attribute__((visibility("default"), regparm(1))) void *
___tls_get_addr(
	const struct __tls_index *index)
{
	void *function_result;

	/* Obtains the tls get addr result. */
	function_result = __tls_get_addr(index);

	/* Returns the computed result. */
	return function_result;
}
#endif

/*
 * Implements the rtld fork prepare operation.
 */
__attribute__((visibility("default"))) void
__rtld_fork_prepare(
	void)
{
	loader_lock();
}

/*
 * Implements the rtld fork parent operation.
 */
__attribute__((visibility("default"))) void
__rtld_fork_parent(
	void)
{
	loader_unlock();
}

/*
 * Implements the rtld fork child operation.
 */
__attribute__((visibility("default"))) void
__rtld_fork_child(
	void)
{
	loader_lock_owner = current_tid();
	loader_lock_depth = 1;
	loader_lock_word = 1;
	loader_unlock();
}

/*
 * Implements the rtld startup init operation.
 */
__attribute__((visibility("default"))) void
__rtld_startup_init(
	void)
{
	size_t i;

	/* Handles the startup initialized condition. */
	if (startup_initialized)
		return;

	/* Process each remaining element. */
	startup_initialized = 1;
	for (i = 0; i < main_object->preinit_count; i++) {
		/* Handles the main object condition. */
		if (main_object->preinit_array[i] != 0)
			((void (*)(void))main_object->preinit_array[i])();
	}
	initialize_object(main_object);
}

/*
 * Implements the rtld process fini operation.
 */
__attribute__((visibility("default"))) void
__rtld_process_fini(
	void)
{
	struct rtld_object *object;
	size_t i;

	/* Handles the process finalized condition. */
	if (process_finalized)
		return;

	/* Process each remaining element. */
	process_finalized = 1;
	while (initialization_count != 0) {
		/* Continue while the operation condition remains true. */
		object = initialization_order[--initialization_count];
		i = object->fini_count;
		while (i != 0) {
			i--;

			/* Checks the current object. */
			if (object->fini_array[i] != 0)
				((void (*)(void))object->fini_array[i])();
		}

		/* Checks the current object. */
		if (object->fini != 0)
			((void (*)(void))object->fini)();
	}
}

/*
 * Implements the rtld dlopen operation.
 */
__attribute__((visibility("default"))) void *
__rtld_dlopen(
	const char *path,
	int flags)
{
	struct rtld_object *object;
	struct rtld_handle *handle;
	const char *name;
	char full_path[RTLD_PATH_MAX];
	intptr_t fd;
	size_t length;
	unsigned i;

	clear_loader_error();

	/* Checks the active flags. */
	if ((flags & ~(RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL)) != 0 ||
	    ((flags & (RTLD_LAZY | RTLD_NOW)) != RTLD_LAZY &&
	     (flags & (RTLD_LAZY | RTLD_NOW)) != RTLD_NOW)) {
		set_loader_error("invalid dlopen flags");

		/* Reports that no result is available. */
		return NULL;
	}
	loader_lock();

	/* Handles the path availability. */
	if (path == NULL) {
		handle = allocate_handle(main_object, 1);

		/* Handles the handle availability. */
		if (handle == NULL)
			set_loader_error("too many dynamic-loader handles");
		loader_unlock();

		/* Returns the computed result. */
		return handle;
	}
	name = dlopen_bare_name(path);

	/* Handles a failed rtld strlen operation. */
	if (name == NULL || (length = rtld_strlen(name)) == 0 ||
	    length >= RTLD_NAME_MAX) {
		set_loader_error("invalid shared-object path");
		loader_unlock();

		/* Reports that no result is available. */
		return NULL;
	}

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Handles a failed rtld strcmp operation. */
		if (objects[i].active && !objects[i].unloading &&
		    (rtld_strcmp(objects[i].path, path) == 0 ||
		     (objects[i].path[0] == '/' &&
		      rtld_strcmp(objects[i].path + 5, name) == 0))) {
			object = &objects[i];
			goto loaded;
		}
	}
	rtld_memcpy(full_path, "/lib/", 5);
	rtld_memcpy(full_path + 5, name, length + 1U);
	fd = syscall6(ZEDBSD_SYS_open, (uintptr_t)full_path, O_RDONLY, 0, 0, 0,
		      0);

	/* Handles an operation failure. */
	if (raw_error(fd)) {
		set_loader_error("shared object not found");
		loader_unlock();

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles a failed preflight dlopen file operation. */
	if (preflight_dlopen_file((int)fd) != 0) {
		(void)syscall6(ZEDBSD_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
		set_loader_error("invalid shared object");
		loader_unlock();

		/* Reports that no result is available. */
		return NULL;
	}
	(void)syscall6(ZEDBSD_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
	object = load_object(name, NULL);
	relocate_object(object);

	/* Constructors may call dlopen recursively, so do not hold the lock. */
	loader_unlock();
	initialize_object(object);
	loader_lock();
loaded:
	handle = allocate_handle(object, 0);

	/* Handles the handle availability. */
	if (handle == NULL) {
		set_loader_error("too many dynamic-loader handles");
		unload_object_locked(object);
	}
	loader_unlock();

	/* Returns the computed result. */
	return handle;
}

/*
 * Implements the rtld dlsym operation.
 */
__attribute__((visibility("default"))) void *
__rtld_dlsym(
	void *value,
	const char *name)
{
	void *function_result;

	/* Obtains the rtld dlsym common result. */
	function_result = rtld_dlsym_common(value, name, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the rtld dlvsym operation.
 */
__attribute__((visibility("default"))) void *
__rtld_dlvsym(
	void *value,
	const char *name,
	const char *version)
{
	void *function_result;

	/* Handles the version availability. */
	if (version == NULL || version[0] == '\0') {
		clear_loader_error();
		set_loader_error("invalid symbol version");

		/* Reports that no result is available. */
		return NULL;
	}

	/* Obtains the rtld dlsym common result. */
	function_result = rtld_dlsym_common(value, name, version);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the rtld dladdr operation.
 */
__attribute__((visibility("default"))) int
__rtld_dladdr(
	const void *value,
	Dl_info *information)
{
	Elf_Sym *symbol;
	uintptr_t symbol_address;
	unsigned type;
	struct rtld_object *object;
	uintptr_t address;
	uintptr_t best_address;
	const char *best_name;
	unsigned i;

	object = NULL;
	address = (uintptr_t)value;
	best_address = 0;
	best_name = NULL;

	/* Handles the information availability. */
	if (information == NULL)
		return 0;
	loader_lock();

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Handles a failed object contains operation. */
		if (objects[i].active && !objects[i].unloading &&
		    object_contains(&objects[i], address, 1, 0)) {
			object = &objects[i];
			break;
		}
	}

	/* Handles the object availability. */
	if (object == NULL) {
		loader_unlock();

		/* Reports successful completion. */
		return 0;
	}

	/* Process each remaining element. */
	for (i = 1; i < object->symbol_count; i++) {
		symbol = &object->symtab[i];

		type = ELF_ST_TYPE(symbol->st_info);

		/* Handles the symbol condition. */
		if (symbol->st_shndx == SHN_UNDEF || symbol->st_name == 0 ||
		    symbol->st_name >= object->strsz ||
		    (type != STT_NOTYPE && type != STT_OBJECT &&
		     type != STT_FUNC))
			continue;

		/* Handles a failed bounded string operation. */
		if (!bounded_string(object->strtab + symbol->st_name,
				    object->strsz - symbol->st_name, NULL))
			continue;

		/* Handles the symbol condition. */
		if (symbol->st_shndx == SHN_ABS) {
			symbol_address = (uintptr_t)symbol->st_value;
		} else {
			/* Handles the uintptr t condition. */
			if ((uintptr_t)symbol->st_value >
			    UINTPTR_MAX - object->base)
				continue;
			symbol_address =
			    object->base + (uintptr_t)symbol->st_value;
		}

		/* Handles the best name availability. */
		if (symbol_address <= address &&
		    (best_name == NULL || symbol_address > best_address)) {
			best_address = symbol_address;
			best_name = object->strtab + symbol->st_name;
		}
	}
	information->dli_fname = object->path;
	information->dli_fbase = (void *)object->base;
	information->dli_sname = best_name;
	information->dli_saddr =
	    best_name != NULL ? (void *)best_address : NULL;
	loader_unlock();

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the rtld dlclose operation.
 */
__attribute__((visibility("default"))) int
__rtld_dlclose(
	void *value)
{
	struct rtld_handle *handle;
	struct rtld_object *object;
	unsigned main_scope;

	clear_loader_error();
	loader_lock();
	handle = validate_handle(value);

	/* Handles the handle availability. */
	if (handle == NULL) {
		set_loader_error("invalid dynamic-loader handle");
		loader_unlock();

		/* Reports operation failure. */
		return -1;
	}
	object = handle->object;
	main_scope = handle->main_scope;

	/* Handles the handle condition. */
	if (--handle->references == 0) {
		handle->active = 0;
		handle->object = NULL;
		handle->main_scope = 0;

		/* Handles the main scope condition. */
		if (!main_scope) {
			/* Checks the current object. */
			if (object->direct_refs == 0) {
				rtld_fatal(
				    "invalid shared-object direct reference");
			}
			object->direct_refs--;
			unload_object_locked(object);
		}
	}
	loader_unlock();

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the rtld dlerror operation.
 */
__attribute__((visibility("default"))) char *
__rtld_dlerror(
	void)
{
	intptr_t value;
	struct __rtld_tcb *tcb;

	value = syscall6(ZEDBSD_SYS_thread_self,
				  ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	tcb = raw_error(value) || value == 0
		? NULL
		: (struct __rtld_tcb *)(uintptr_t)value;

	/* Handles the tcb availability. */
	if (tcb != NULL) {
		/* Handles an operation failure. */
		if (!tcb->dlerror_pending)
			return NULL;
		tcb->dlerror_pending = 0;

		/* Returns the computed result. */
		return tcb->dlerror_buf;
	}

	/* Handles an operation failure. */
	if (!loader_error_pending)
		return NULL;
	loader_error_pending = 0;

	/* Returns the computed result. */
	return loader_error;
}

/*
 * Implements the rtld main operation.
 */
uintptr_t
rtld_main(
	uintptr_t *initial_stack)
{
	uintptr_t phdr_vaddr;
	Elf_Ehdr *main_header;
	uintptr_t *cursor, *auxv;
	uintptr_t at_base, at_phdr, at_phnum, at_phent;
	uintptr_t at_entry;
	uintptr_t main_base;
	int main_type;
	Elf_Ehdr *self_header;
	Elf_Phdr *self_phdr;
	unsigned i;
	struct __rtld_tcb *initial_tcb;
	intptr_t tls_result;

	at_base = 0;
	at_phdr = 0;
	at_phnum = 0;
	at_phent = 0;
	at_entry = 0;
	main_base = 0;
	main_type = ET_EXEC;

	/* Handles the initial stack availability. */
	if (initial_stack == NULL)
		rtld_fatal("missing initial stack");

	/* Continue while the operation condition remains true. */
	cursor = initial_stack + 1U + initial_stack[0] + 1U;
	while (*cursor++ != 0) {
	}

	/* Process each element required by the operation. */
	auxv = cursor;
	for (i = 0; i < 64; i++, auxv += 2) {
		/* Handles the auxv condition. */
		if (auxv[0] == AT_NULL)
			break;

		/* Dispatch the selected operation case. */
		switch (auxv[0]) {
		case AT_BASE:
			at_base = auxv[1];
			break;
		case AT_PHDR:
			at_phdr = auxv[1];
			break;
		case AT_PHNUM:
			at_phnum = auxv[1];
			break;
		case AT_PHENT:
			at_phent = auxv[1];
			break;
		case AT_ENTRY:
			at_entry = auxv[1];
			break;
		default:
			break;
		}
	}

	/* Checks the current index. */
	if (i == 64 || at_base == 0 || at_phdr == 0 || at_phnum == 0 ||
	    at_phnum > 64 || at_phent != sizeof(Elf_Phdr) || at_entry == 0)
		rtld_fatal("invalid ELF auxiliary vector");
	self_header = (Elf_Ehdr *)at_base;

	/* Handles a failed valid elf header operation. */
	if (!valid_elf_header(self_header, ET_DYN))
		rtld_fatal("invalid interpreter ELF header");
	self_phdr = (Elf_Phdr *)(at_base + (uintptr_t)self_header->e_phoff);

	/* Handles a failed bootstrap relative operation. */
	if (bootstrap_relative(at_base, self_phdr, self_header->e_phnum) != 0)
		rtld_fatal("interpreter bootstrap relocation failed");

	/* Process each element required by the operation. */
	for (i = 0; i < at_phnum; i++) {
		/* Handles the Elf Phdr condition. */
		if (((Elf_Phdr *)at_phdr)[i].p_type == PT_PHDR) {
			phdr_vaddr = (uintptr_t)((Elf_Phdr *)at_phdr)[i].p_vaddr;

			/* Handles the phdr vaddr condition. */
			if (phdr_vaddr > at_phdr) {
				rtld_fatal(
				    "invalid main program-header address");
			}
			main_base = at_phdr - phdr_vaddr;
			break;
		}
	}

	/* Handles the main base condition. */
	if (main_base != 0) {
		main_header = (Elf_Ehdr *)main_base;

		/* Handles a failed valid elf header operation. */
		if (!valid_elf_header(main_header, ET_DYN))
			rtld_fatal("invalid PIE executable header");
		main_type = ET_DYN;
	}

	main_object = new_object("<main>");
	interpreter_object = new_object(RTLD_INTERP_PATH);
	setup_premapped_object(main_object, main_base, (Elf_Phdr *)at_phdr,
			       (unsigned)at_phnum, main_type);
	setup_premapped_object(interpreter_object, at_base, self_phdr,
			       self_header->e_phnum, ET_DYN);
	interpreter_object->relative_done = 1;
	load_dependencies(main_object);

	/* Handles the interpreter object condition. */
	if (interpreter_object->needed_count != 0)
		rtld_fatal("interpreter must not have dependencies");

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Handles the objects condition. */
		if (objects[i].active)
			relocate_object(&objects[i]);
	}

	/*
 * The initial executable, interpreter, and DT_NEEDED closure remain
	 * mapped until process termination.  Only later dlopen() objects are
	 * candidates for physical unload. */
	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Handles the objects condition. */
		if (objects[i].active)
			objects[i].permanent = 1;
	}

	/* Handles a failed rtld thread alloc operation. */
	if (__rtld_thread_alloc(NULL, &initial_tcb) != 0)
		rtld_fatal("cannot allocate initial TLS");
	tls_result =
	    syscall6(ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_SET_TLS,
		     (uintptr_t)initial_tcb, 0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(tls_result))
		rtld_fatal("cannot install initial TLS");

	/* Returns the computed result. */
	return at_entry;
}

/* Supports the syscall6 operation. */
static intptr_t
syscall6(
	uint32_t number,
	uintptr_t a0,
	uintptr_t a1,
	uintptr_t a2,
	uintptr_t a3,
	uintptr_t a4,
	uintptr_t a5)
{
	intptr_t function_result;

	/* Obtains the rtld syscall6 result. */
	function_result = rtld_syscall6(number, a0, a1, a2, a3, a4, a5);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the tls map operation. */
static void *
tls_map(
	size_t size)
{
	void *function_result;
	intptr_t result;

	size = (size_t)page_ceil(size);
	result = map_call(0, size, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	/* Computes the function result. */
	function_result = raw_error(result) ? NULL : (void *)(uintptr_t)result;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the page ceil operation. */
static uintptr_t
page_ceil(
	uintptr_t value)
{
	/* Validates the current value. */
	if (value > UINTPTR_MAX - (RTLD_PAGE_SIZE - 1U))
		rtld_fatal("address overflow");

	/* Returns the computed result. */
	return (value + RTLD_PAGE_SIZE - 1U) &
	       ~(uintptr_t)(RTLD_PAGE_SIZE - 1U);
}

/* Supports the map call operation. */
static intptr_t
map_call(
	uintptr_t address,
	size_t size,
	int prot,
	int flags,
	int fd,
	uintptr_t offset)
{
	intptr_t function_result;

	/* Obtains the syscall6 result. */
	function_result = syscall6(ZEDBSD_SYS_mmap, address, size, (uintptr_t)prot,
			(uintptr_t)flags, (uintptr_t)fd, offset);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the raw error operation. */
static int
raw_error(
	intptr_t value)
{
	/* Returns the computed result. */
	return value < 0 && value >= -4095;
}

/* Supports the tls unmap operation. */
static void
tls_unmap(
	void *address,
	size_t size)
{
	/* Handles the address availability. */
	if (address != NULL) {
		(void)syscall6(ZEDBSD_SYS_munmap, (uintptr_t)address,
			       page_ceil(size), 0, 0, 0, 0);
	}
}

/* Supports the loader lock operation. */
static void
loader_lock(
	void)
{
	uintptr_t tid;

	tid = current_tid();

	/* Handles the tid condition. */
	if (tid != 0 && loader_lock_owner == tid) {
		loader_lock_depth++;

		/* Returns the computed result. */
		return;
	}

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Handles a failed atomic exchange n operation. */
		if (__atomic_exchange_n(&loader_lock_word, 1,
					__ATOMIC_ACQUIRE) == 0)
			break;
		(void)syscall6(ZEDBSD_SYS_usync, (uintptr_t)&loader_lock_word,
			       ZEDBSD_USYNC_WAIT, 1, 0, 0,
			       ZEDBSD_USYNC_PRIVATE);
	}
	loader_lock_owner = tid;
	loader_lock_depth = 1;
}

/* Supports the current tid operation. */
static uintptr_t
current_tid(
	void)
{
	uintptr_t function_result;
	intptr_t value;

	value = syscall6(ZEDBSD_SYS_thread_self,
				  ZEDBSD_THREAD_SELF_TID, 0, 0, 0, 0, 0);

	/* Computes the function result. */
	function_result = raw_error(value) ? 0 : (uintptr_t)value;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the loader unlock operation. */
static void
loader_unlock(
	void)
{
	/* Handles a failed current tid operation. */
	if (loader_lock_depth == 0 || loader_lock_owner != current_tid())
		rtld_fatal("loader lock ownership failure");

	/* Handles the loader lock depth condition. */
	if (--loader_lock_depth != 0)
		return;
	loader_lock_owner = 0;
	__atomic_store_n(&loader_lock_word, 0, __ATOMIC_RELEASE);
	(void)syscall6(ZEDBSD_SYS_usync, (uintptr_t)&loader_lock_word,
		       ZEDBSD_USYNC_WAKE, 0, 0, 1, ZEDBSD_USYNC_PRIVATE);
}

/* Supports the allocate tls block operation. */
static void *
allocate_tls_block(
	const struct rtld_tls_module *module)
{
	void *block;

	/* Handles the module availability. */
	if (module == NULL || !module->active || module->memory_size == 0)
		return NULL;
	block = tls_map(module->memory_size);

	/* Handles the block availability. */
	if (block == NULL)
		return NULL;

	/* Handles the module condition. */
	if (module->file_size != 0)
		rtld_memcpy(block, module->init_image, module->file_size);

	/* Handles the module condition. */
	if (module->memory_size > module->file_size) {
		rtld_memset((unsigned char *)block + module->file_size, 0,
			    module->memory_size - module->file_size);
	}

	/* Returns the computed result. */
	return block;
}

/* Supports the initialize object operation. */
static void
initialize_object(
	struct rtld_object *object)
{
	size_t i;

	/* Handles the object availability. */
	if (object == NULL || object->initialized ||
	    object == interpreter_object)

		/* Returns the computed result. */
		return;

	/* Checks the current object. */
	if (object->initializing)
		return;

	/* Process each remaining element. */
	object->initializing = 1;
	for (i = 0; i < object->needed_count; i++)
		initialize_object(object->needed[i]);
	object->initialized = 1;

	/* Checks the current object. */
	if (object->init != 0)
		((void (*)(void))object->init)();

	/* Process each remaining element. */
	for (i = 0; i < object->init_count; i++) {
		/* Checks the current object. */
		if (object->init_array[i] != 0)
			((void (*)(void))object->init_array[i])();
	}

	/* Handles the initialization count condition. */
	if (initialization_count == RTLD_OBJECT_MAX)
		rtld_fatal("initialization order overflow");
	initialization_order[initialization_count++] = object;
	object->initializing = 0;
}

/* Supports the clear loader error operation. */
static void
clear_loader_error(
	void)
{
	intptr_t value;
	struct __rtld_tcb *tcb;

	value = syscall6(ZEDBSD_SYS_thread_self,
				  ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	tcb = raw_error(value) || value == 0
		? NULL
		: (struct __rtld_tcb *)(uintptr_t)value;

	/* Handles the tcb availability. */
	if (tcb != NULL) {
		tcb->dlerror_pending = 0;
		tcb->dlerror_buf[0] = '\0';
	} else {
		loader_error_pending = 0;
		loader_error[0] = '\0';
	}
}

/* Supports the set loader error operation. */
static void
set_loader_error(
	const char *message)
{
	size_t length;
	intptr_t value;
	struct __rtld_tcb *tcb;
	char *buffer;
	size_t capacity;

	value = syscall6(ZEDBSD_SYS_thread_self,
				  ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	tcb = raw_error(value) || value == 0
		? NULL
		: (struct __rtld_tcb *)(uintptr_t)value;
	buffer = tcb != NULL ? tcb->dlerror_buf : loader_error;
	capacity = tcb != NULL
		? sizeof(tcb->dlerror_buf)
		: sizeof(loader_error);
	length = 0;

	/* Handles the message availability. */
	if (message == NULL)

	/* Process each remaining element. */
		message = "runtime linker error";
	while (message[length] != '\0' && length + 1U < capacity) {
		buffer[length] = message[length];
		length++;
	}
	buffer[length] = '\0';

	/* Handles the tcb availability. */
	if (tcb != NULL)
		tcb->dlerror_pending = 1;
	else
		loader_error_pending = 1;
}

/* Supports the allocate handle operation. */
static struct rtld_handle *
allocate_handle(
	struct rtld_object *object,
	int main_scope)
{
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < RTLD_HANDLE_MAX; i++) {
		/* Handles the handles condition. */
		if (!handles[i].active) {
			handles[i].magic = RTLD_HANDLE_MAGIC;
			handles[i].generation = next_handle_generation++;

			/* Handles the next handle generation condition. */
			if (next_handle_generation == 0)
				next_handle_generation = 1;
			handles[i].object = object;
			handles[i].references = 1;
			handles[i].active = 1;
			handles[i].main_scope = (unsigned)main_scope;

			/* Handles the main scope condition. */
			if (!main_scope)
				object->direct_refs++;

			/* Returns the computed result. */
			return &handles[i];
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the dlopen bare name operation. */
static const char *
dlopen_bare_name(
	const char *path)
{
	const char *cursor;

	/* Handles the path availability. */
	if (path == NULL || path[0] == '\0')
		return NULL;

	/* Handles the path condition. */
	if (path[0] == '/') {
		/* Handles the path condition. */
		if (path[1] != 'l' || path[2] != 'i' || path[3] != 'b' ||
		    path[4] != '/')

			/* Reports that no result is available. */
			return NULL;
		path += 5;
	}

	/* Process each element required by the operation. */
	for (cursor = path; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor == '/')
			return NULL;
	}

	/* Returns the computed result. */
	return path;
}

/* Supports the preflight dlopen file operation. */
static int
preflight_dlopen_file(
	int fd)
{
	struct stat status;
	Elf_Ehdr header;
	Elf_Phdr phdr[64];
	intptr_t result;
	size_t phdr_size;

	result = syscall6(ZEDBSD_SYS_fstat, (uintptr_t)fd, (uintptr_t)&status,
			  0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(result) || status.st_size < (off_t)sizeof(header))
		return -1;
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd, (uintptr_t)&header,
			  sizeof(header), 0, 0, 0);

	/* Handles a failed valid elf header operation. */
	if (result != (intptr_t)sizeof(header) ||
	    !valid_elf_header(&header, ET_DYN) ||
	    header.e_phoff > (Elf_Off)status.st_size ||
	    header.e_phnum >
		((Elf_Off)status.st_size - header.e_phoff) / sizeof(Elf_Phdr))

		/* Reports operation failure. */
		return -1;
	phdr_size = (size_t)header.e_phnum * sizeof(Elf_Phdr);
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd, (uintptr_t)phdr,
			  phdr_size, (uintptr_t)header.e_phoff, 0, 0);

	/* Handles a failed validate file programs operation. */
	if (result != (intptr_t)phdr_size ||
	    validate_file_programs(&header, phdr, status.st_size) != 0)

		/* Reports operation failure. */
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the valid elf header operation. */
static int
valid_elf_header(
	const Elf_Ehdr *header,
	int expected_type)
{
	/* Handles the header availability. */
	if (header == NULL || header->e_ident[EI_MAG0] != ELFMAG0 ||
	    header->e_ident[EI_MAG1] != ELFMAG1 ||
	    header->e_ident[EI_MAG2] != ELFMAG2 ||
	    header->e_ident[EI_MAG3] != ELFMAG3 ||
	    header->e_ident[EI_CLASS] != ELF_CLASS ||
	    header->e_ident[EI_DATA] != RTLD_DATA ||
	    header->e_ident[EI_VERSION] != EV_CURRENT ||
	    header->e_type != expected_type ||
	    header->e_machine != RTLD_MACHINE ||
	    header->e_version != EV_CURRENT ||
	    header->e_ehsize != sizeof(*header) ||
	    header->e_phentsize != sizeof(Elf_Phdr) || header->e_phnum == 0 ||
	    header->e_phnum > 64)

		/* Reports successful completion. */
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the validate file programs operation. */
static int
validate_file_programs(
	const Elf_Ehdr *header,
	const Elf_Phdr *phdr,
	off_t file_size)
{
	uintptr_t other_start, other_end;
	uintptr_t start, end;
	unsigned i, j, loads, dynamics;

	/* Process each element required by the operation. */
	loads = 0;
	dynamics = 0;
	for (i = 0; i < header->e_phnum; i++) {
		/* Handles the phdr condition. */
		if (phdr[i].p_type == PT_DYNAMIC)
			dynamics++;

		/* Handles the phdr condition. */
		if (phdr[i].p_type != PT_LOAD)
			continue;

		/* Handles the phdr condition. */
		if (phdr[i].p_memsz == 0) {
			/* Handles the phdr condition. */
			if (phdr[i].p_filesz != 0)
				return -1;
			continue;
		}
		loads++;

		/* Handles a failed temporary writable plt operation. */
		if (phdr[i].p_filesz > phdr[i].p_memsz ||
		    phdr[i].p_offset > (Elf_Off)file_size ||
		    phdr[i].p_filesz > (Elf_Off)file_size - phdr[i].p_offset ||
		    ((phdr[i].p_offset ^ phdr[i].p_vaddr) &
		     (RTLD_PAGE_SIZE - 1U)) != 0 ||
		    phdr[i].p_vaddr > (Elf_Addr)UINTPTR_MAX - phdr[i].p_memsz ||
		    (((phdr[i].p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) &&
		     !temporary_writable_plt(&phdr[i])))

			/* Reports operation failure. */
			return -1;
		start = page_floor((uintptr_t)phdr[i].p_vaddr);

		/* Handles the uintptr t condition. */
		if ((uintptr_t)(phdr[i].p_vaddr + phdr[i].p_memsz) >
		    UINTPTR_MAX - (RTLD_PAGE_SIZE - 1U))

			/* Reports operation failure. */
			return -1;

		/* Process each element required by the operation. */
		end = page_ceil((uintptr_t)(phdr[i].p_vaddr + phdr[i].p_memsz));
		for (j = 0; j < i; j++) {
			/* Handles the phdr condition. */
			if (phdr[j].p_type != PT_LOAD)
				continue;
			other_start = page_floor((uintptr_t)phdr[j].p_vaddr);
			other_end = page_ceil(
			    (uintptr_t)(phdr[j].p_vaddr + phdr[j].p_memsz));

			/* Handles the start condition. */
			if (start < other_end && other_start < end)
				return -1;
		}
	}

	/* Handles the loads condition. */
	if (loads == 0 || dynamics != 1)
		return -1;

	/* Reports successful completion. */
	return 0;
}

/* Supports the temporary writable plt operation. */
static int
temporary_writable_plt(
	const Elf_Phdr *program)
{
#if defined(HAL_ARCH_SPARCV9)

	/* Returns the computed result. */
	return program->p_flags == (PF_R | PF_W | PF_X) &&
	       program->p_filesz == program->p_memsz && program->p_memsz != 0 &&
	       program->p_memsz <= RTLD_PAGE_SIZE &&
	       ((uintptr_t)program->p_vaddr & (RTLD_PAGE_SIZE - 1U)) == 0 &&
	       ((uintptr_t)program->p_offset & (RTLD_PAGE_SIZE - 1U)) == 0;
#else
	(void)program;

	/* Reports successful completion. */
	return 0;
#endif
}

/* Supports the page floor operation. */
static uintptr_t
page_floor(
	uintptr_t value)
{
	/* Returns the computed result. */
	return value & ~(uintptr_t)(RTLD_PAGE_SIZE - 1U);
}

/* Supports the load object operation. */
static struct rtld_object *
load_object(
	const char *name,
	struct rtld_object *requester)
{
	char path[RTLD_PATH_MAX];
	struct stat status;
	Elf_Ehdr header;
	Elf_Phdr phdr[64];
	struct rtld_object *object, *existing;
	intptr_t fd, result;
	size_t length;
	unsigned i, first;
	uintptr_t minimum;

	first = 0;
	minimum = UINTPTR_MAX;

	/* Handles the name availability. */
	if (name == NULL || name[0] == '\0')
		rtld_fatal("empty dependency name");
	length = rtld_strlen(name);

	/* Checks the current data length. */
	if (length >= RTLD_NAME_MAX)
		rtld_fatal("dependency name too long");

	/* Process each remaining element. */
	for (i = 0; i < length; i++) {
		/* Validates the current name. */
		if (name[i] == '/')
			rtld_fatal("dependency path must be a bare name");
	}
	fd = open_dependency(name, length, requester, path);

	/* Handles an operation failure. */
	if (raw_error(fd))
		rtld_fatal("cannot open dependency");
	result = syscall6(ZEDBSD_SYS_fstat, (uintptr_t)fd, (uintptr_t)&status,
			  0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(result) || status.st_size < (off_t)sizeof(header))
		rtld_fatal("cannot stat dependency");
	existing = find_identity(&status);

	/* Handles the existing availability. */
	if (existing != NULL) {
		(void)syscall6(ZEDBSD_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);

		/* Returns the computed result. */
		return existing;
	}
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd, (uintptr_t)&header,
			  sizeof(header), 0, 0, 0);

	/* Handles a failed valid elf header operation. */
	if (result != (intptr_t)sizeof(header) ||
	    !valid_elf_header(&header, ET_DYN) ||
	    header.e_phoff > (Elf_Off)status.st_size ||
	    header.e_phnum >
		((Elf_Off)status.st_size - header.e_phoff) / sizeof(Elf_Phdr))
		rtld_fatal("invalid dependency ELF header");
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd, (uintptr_t)phdr,
			  (size_t)header.e_phnum * sizeof(Elf_Phdr),
			  (uintptr_t)header.e_phoff, 0, 0);

	/* Checks the operation result. */
	if (result != (intptr_t)((size_t)header.e_phnum * sizeof(Elf_Phdr)))
		rtld_fatal("cannot read dependency headers");

	/* Handles a failed validate file programs operation. */
	if (validate_file_programs(&header, phdr, status.st_size) != 0)
		rtld_fatal("invalid shared object program headers");
	object = new_object(path);
	object->loader_parent = requester;
	object->type = ET_DYN;
	object->device = status.st_dev;
	object->inode = status.st_ino;
	object->has_identity = 1;
	object->phnum = header.e_phnum;
	rtld_memcpy(object->phdr, phdr, object->phnum * sizeof(Elf_Phdr));

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Handles a failed page floor operation. */
		if (object->phdr[i].p_type == PT_LOAD &&
		    object->phdr[i].p_memsz != 0 &&
		    page_floor((uintptr_t)object->phdr[i].p_vaddr) < minimum) {
			minimum =
			    page_floor((uintptr_t)object->phdr[i].p_vaddr);
			first = i;
		}
	}
	map_one_segment(object, (int)fd, &object->phdr[first], 1);

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Checks the current index. */
		if (i != first && object->phdr[i].p_type == PT_LOAD &&
		    object->phdr[i].p_memsz != 0)
			map_one_segment(object, (int)fd, &object->phdr[i], 0);
	}
	(void)syscall6(ZEDBSD_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
	parse_dynamic(object);
	load_dependencies(object);

	/* Returns the computed result. */
	return object;
}

/* Supports the open dependency operation. */
static intptr_t
open_dependency(
	const char *name,
	size_t name_length,
	const struct rtld_object *requester,
	char path[RTLD_PATH_MAX])
{
	intptr_t function_result;
	const struct rtld_object *owner;
	intptr_t fd;

	/* Handles the requester availability. */
	if (requester != NULL && requester->runpath != NULL) {
		fd = open_search_list(requester->runpath, requester, name,
				      name_length, path);

		/* Handles an operation failure. */
		if (!raw_error(fd))
			return fd;
	} else {
		/* Process each element required by the operation. */
		for (owner = requester; owner != NULL;
		     owner = owner->loader_parent) {
			/* Handles the rpath availability. */
			if (owner->rpath != NULL) {
				fd = open_search_list(owner->rpath, owner, name,
						      name_length, path);

				/* Handles an operation failure. */
				if (!raw_error(fd))
					return fd;
			}
		}
	}

	/* Obtains the open search candidate result. */
	function_result = open_search_candidate("/lib", 4U, name, name_length, path);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the open search list operation. */
static intptr_t
open_search_list(
	const char *list,
	const struct rtld_object *owner,
	const char *name,
	size_t name_length,
	char path[RTLD_PATH_MAX])
{
	const char *slash;
	const char *cursor;
	size_t origin_length, suffix_length;
	const char *end;
	char directory[RTLD_PATH_MAX];
	size_t length;
	intptr_t fd;
	const char *component;

	/* Handles the list availability. */
	if (list == NULL)
		return -1;

	/* Continue until the operation reaches a terminal state. */
	component = list;
	for (;;) {
		end = component;
		fd = -1;

		/* Continue while the operation condition remains true. */
		while (*end != '\0' && *end != ':')
			end++;
		length = (size_t)(end - component);

		/* Checks the current data length. */
		if (length != 0 && component[0] == '/') {
			/* Checks the current data length. */
			if (length < sizeof(directory)) {
				rtld_memcpy(directory, component, length);
				directory[length] = '\0';
				fd = open_search_candidate(
				    directory, length, name, name_length, path);
			}
		} else if (length >= 7U && component[0] == '$' &&
			   component[1] == 'O' && component[2] == 'R' &&
			   component[3] == 'I' && component[4] == 'G' &&
			   component[5] == 'I' && component[6] == 'N' &&
			   (length == 7U || component[7] == '/') &&
			   owner != NULL && owner->path[0] == '/') {
			slash = owner->path;

			suffix_length = length - 7U;

			/* Process each element required by the operation. */
			for (cursor = owner->path; *cursor != '\0'; cursor++) {
				/* Checks the current cursor position. */
				if (*cursor == '/')
					slash = cursor;
			}
			origin_length = (size_t)(slash - owner->path);

			/* Handles the origin length condition. */
			if (origin_length == 0)
				origin_length = 1;

			/* Handles the origin length condition. */
			if (origin_length <=
			    sizeof(directory) - suffix_length - 1U) {
				rtld_memcpy(directory, owner->path,
					    origin_length);

				/* Handles the suffix length condition. */
				if (suffix_length != 0) {
					rtld_memcpy(directory + origin_length,
						    component + 7U,
						    suffix_length);
				}
				length = origin_length + suffix_length;
				directory[length] = '\0';
				fd = open_search_candidate(
				    directory, length, name, name_length, path);
			}
		}

		/* Handles an operation failure. */
		if (!raw_error(fd))
			return fd;

		/* Checks the current endpoint. */
		if (*end == '\0')
			break;
		component = end + 1;
	}

	/* Reports operation failure. */
	return -1;
}

/* Supports the open search candidate operation. */
static intptr_t
open_search_candidate(
	const char *directory,
	size_t directory_length,
	const char *name,
	size_t name_length,
	char path[RTLD_PATH_MAX])
{
	intptr_t function_result;

	/* Handles the directory length condition. */
	if (directory_length == 0 || directory_length >= RTLD_PATH_MAX ||
	    name_length == 0 ||
	    directory_length > RTLD_PATH_MAX - name_length - 2U)

		/* Reports operation failure. */
		return -1;
	rtld_memcpy(path, directory, directory_length);

	/* Handles the path condition. */
	if (path[directory_length - 1U] != '/')
		path[directory_length++] = '/';
	rtld_memcpy(path + directory_length, name, name_length);
	path[directory_length + name_length] = '\0';

	/* Obtains the syscall6 result. */
	function_result = syscall6(ZEDBSD_SYS_open, (uintptr_t)path, O_RDONLY, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the find identity operation. */
static struct rtld_object *
find_identity(
	const struct stat *status)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Handles the objects condition. */
		if (objects[i].active && !objects[i].unloading &&
		    objects[i].has_identity &&
		    objects[i].device == status->st_dev &&
		    objects[i].inode == status->st_ino)

			/* Returns the computed result. */
			return &objects[i];
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the new object operation. */
static struct rtld_object *
new_object(
	const char *path)
{
	struct rtld_object *object;
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Handles the objects condition. */
		if (!objects[i].active)
			break;
	}

	/* Checks the current index. */
	if (i == object_count) {
		/* Handles the object count condition. */
		if (object_count == RTLD_OBJECT_MAX)
			rtld_fatal("too many shared objects");
		object_count++;
	}
	object = &objects[i];
	rtld_memset(object, 0, sizeof(*object));
	object->active = 1;
	object->generation = next_object_generation++;

	/* Handles the next object generation condition. */
	if (next_object_generation == 0)
		next_object_generation = 1;
	copy_path(object->path, path);

	/* Returns the computed result. */
	return object;
}

/* Supports the copy path operation. */
static void
copy_path(
	char destination[RTLD_PATH_MAX],
	const char *source)
{
	size_t length;

	length = rtld_strlen(source);

	/* Checks the current data length. */
	if (length == 0 || length >= RTLD_PATH_MAX)
		rtld_fatal("invalid object path");
	rtld_memcpy(destination, source, length + 1U);
}

/* Supports the map one segment operation. */
static void
map_one_segment(
	struct rtld_object *object,
	int fd,
	const Elf_Phdr *program,
	int choose_base)
{
	uintptr_t anonymous;
	size_t anonymous_size;
	uintptr_t zero_start;
	uintptr_t zero_end;
	uintptr_t file_page_end;
	intptr_t result;
	uintptr_t virtual_page;
	uintptr_t page_delta;
	uintptr_t file_bytes;
	uintptr_t memory_bytes;
	size_t file_map_size;
	size_t memory_map_size;
	uintptr_t file_offset;
	uintptr_t requested;
	int flags;
	int final_prot;
	int map_prot;
	int need_zero;
	intptr_t mapped;

	virtual_page = page_floor((uintptr_t)program->p_vaddr);
	page_delta = (uintptr_t)program->p_vaddr - virtual_page;
	file_bytes = page_delta + (uintptr_t)program->p_filesz;
	memory_bytes = page_delta + (uintptr_t)program->p_memsz;
	file_map_size =
		program->p_filesz != 0 ? (size_t)page_ceil(file_bytes) : 0;
	memory_map_size = (size_t)page_ceil(memory_bytes);
	file_offset = page_floor((uintptr_t)program->p_offset);
	requested = choose_base ? 0 : object->base + virtual_page;
	flags = MAP_PRIVATE | (choose_base ? 0 : MAP_FIXED_NOREPLACE);
	final_prot = segment_prot(program->p_flags);
	map_prot = final_prot;
	need_zero = program->p_memsz > program->p_filesz;

	/* Handles the temporary writable plt condition. */
	if (temporary_writable_plt(program)) {
		final_prot = PROT_READ | PROT_WRITE;
		map_prot = final_prot;
	}

	/* Handles the zero condition. */
	if (need_zero && (map_prot & PROT_WRITE) == 0) {
		/* Handles the map prot condition. */
		if (map_prot & PROT_EXEC)
			rtld_fatal("executable BSS segment is unsupported");
		map_prot |= PROT_WRITE;
	}

	/* Handles the file map size condition. */
	if (file_map_size != 0) {
		mapped = map_call(requested, file_map_size, map_prot, flags, fd,
				  file_offset);

		/* Handles an operation failure. */
		if (raw_error(mapped))
			rtld_fatal("cannot map shared object segment");
	} else {
		mapped = map_call(requested, memory_map_size, map_prot,
				  flags | MAP_ANONYMOUS, -1, 0);

		/* Handles an operation failure. */
		if (raw_error(mapped))
			rtld_fatal("cannot map shared object BSS");
	}

	/* Handles the choose base condition. */
	if (choose_base) {
		/* Handles the uintptr t condition. */
		if ((uintptr_t)mapped < virtual_page)
			rtld_fatal("invalid shared object load bias");
		object->base = (uintptr_t)mapped - virtual_page;
	}
	remember_mapping(object, (uintptr_t)mapped,
			 file_map_size != 0 ? file_map_size : memory_map_size);

	/* Handles the file map size condition. */
	if (file_map_size < memory_map_size) {
		anonymous = object->base + virtual_page + file_map_size;
		anonymous_size = memory_map_size - file_map_size;
		mapped = map_call(
		    anonymous, anonymous_size, map_prot,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

		/* Handles an operation failure. */
		if (raw_error(mapped) || (uintptr_t)mapped != anonymous)
			rtld_fatal("cannot map shared object zero fill");
		remember_mapping(object, anonymous, anonymous_size);
	}

	/* Handles the zero condition. */
	if (need_zero && program->p_filesz != 0) {
		zero_start = object->base +
		       (uintptr_t)program->p_vaddr +
		       (uintptr_t)program->p_filesz;
		zero_end = object->base +
		     (uintptr_t)program->p_vaddr +
		     (uintptr_t)program->p_memsz;
		file_page_end = object->base + virtual_page + file_map_size;

		/* Handles the zero end condition. */
		if (zero_end > file_page_end)
			zero_end = file_page_end;

		/* Handles the zero end condition. */
		if (zero_end > zero_start) {
			rtld_memset((void *)zero_start, 0,
				    zero_end - zero_start);
		}
	}

	/* Handles the map prot condition. */
	if (map_prot != final_prot) {
		result = syscall6(ZEDBSD_SYS_mprotect, object->base + virtual_page,
	     memory_map_size, (uintptr_t)final_prot, 0, 0, 0);

		/* Handles an operation failure. */
		if (raw_error(result))
			rtld_fatal("cannot protect shared object segment");
	}
}

/* Supports the segment prot operation. */
static int
segment_prot(
	uint32_t flags)
{
	int prot;

	prot = 0;
#if !defined(HAL_ARCH_SPARCV9)

	/* Checks the active flags. */
	if ((flags & (PF_W | PF_X)) == (PF_W | PF_X))
		rtld_fatal("writable executable segment");
#endif

	/* Checks the active flags. */
	if (flags & PF_R)
		prot |= PROT_READ;

	/* Checks the active flags. */
	if (flags & PF_W)
		prot |= PROT_WRITE;

	/* Checks the active flags. */
	if (flags & PF_X)
		prot |= PROT_EXEC;

	/* Returns the computed result. */
	return prot;
}

/* Supports the remember mapping operation. */
static void
remember_mapping(
	struct rtld_object *object,
	uintptr_t start,
	size_t size)
{
	/* Checks the current object. */
	if (object->mapping_count == 64)
		rtld_fatal("too many object mappings");
	object->mapping_start[object->mapping_count] = start;
	object->mapping_size[object->mapping_count++] = size;
}

/* Supports the parse dynamic operation. */
static void
parse_dynamic(
	struct rtld_object *object)
{
	Elf_Dyn *dynamic;
	size_t hash_words;
	uint32_t symbol;
	size_t chain_index;
	uint32_t *header;
	size_t bloom_bytes, bucket_bytes, chain_capacity, chain_bytes;
	unsigned bucket_index;
	uint32_t gnu_symbol_count;
	size_t i;
	size_t relsz, relasz, relent;
	size_t relaent, syment;
	size_t init_array_size, fini_array_size;
	size_t preinit_array_size;
	uintptr_t strtab_value, symtab_value, hash_value;
	uintptr_t gnu_hash_value;
	uintptr_t versym_value, verdef_value, verneed_value;
	uintptr_t rpath_offset, runpath_offset;
	uintptr_t rel_value, rela_value, jmprel_value;
	uintptr_t offset;

	relsz = 0;
	relasz = 0;
	relent = sizeof(Elf_Rel);
	relaent = sizeof(Elf_Rela);
	syment = sizeof(Elf_Sym);
	init_array_size = 0;
	fini_array_size = 0;
	preinit_array_size = 0;
	strtab_value = 0;
	symtab_value = 0;
	hash_value = 0;
	gnu_hash_value = 0;
	versym_value = 0;
	verdef_value = 0;
	verneed_value = 0;
	rpath_offset = UINTPTR_MAX;
	runpath_offset = UINTPTR_MAX;
	rel_value = 0;
	rela_value = 0;
	jmprel_value = 0;

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Checks the current object. */
		if (object->phdr[i].p_type == PT_DYNAMIC) {
			/* Handles the dynamic availability. */
			if (object->dynamic != NULL ||
			    object->phdr[i].p_memsz < sizeof(Elf_Dyn))
				rtld_fatal("invalid dynamic segment");
			object->dynamic = (Elf_Dyn *)object_pointer(
			    object, object->phdr[i].p_vaddr,
			    (size_t)object->phdr[i].p_memsz, PF_R);
			object->dynamic_count =
			    (size_t)object->phdr[i].p_memsz / sizeof(Elf_Dyn);
		}
	}

	/* Handles the dynamic availability. */
	if (object->dynamic == NULL)
		rtld_fatal("missing dynamic segment");

	/* Process each remaining element. */
	for (i = 0; i < object->dynamic_count; i++) {
		dynamic = &object->dynamic[i];

		/* Handles the dynamic condition. */
		if (dynamic->d_tag == DT_NULL)
			break;

		/* Dispatch the selected operation case. */
		switch ((int)dynamic->d_tag) {
		case DT_NEEDED:
			/* Checks the current object. */
			if (object->needed_count == RTLD_NEEDED_MAX)
				rtld_fatal("too many dependencies");
			object->needed_offset[object->needed_count++] =
			    (uint32_t)dynamic->d_un.d_val;
			break;
		case DT_HASH:
			hash_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_GNU_HASH:
			gnu_hash_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_STRTAB:
			strtab_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_STRSZ:
			object->strsz = (size_t)dynamic->d_un.d_val;
			break;
		case DT_SYMTAB:
			symtab_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_SYMENT:
			syment = (size_t)dynamic->d_un.d_val;
			break;
		case DT_VERSYM:
			versym_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_VERDEF:
			verdef_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_VERDEFNUM:
			/* Handles the dynamic condition. */
			if (dynamic->d_un.d_val > UINT32_MAX) {
				rtld_fatal(
				    "too many symbol version definitions");
			}
			object->verdef_count = (uint32_t)dynamic->d_un.d_val;
			break;
		case DT_VERNEED:
			verneed_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_VERNEEDNUM:
			/* Handles the dynamic condition. */
			if (dynamic->d_un.d_val > UINT32_MAX) {
				rtld_fatal(
				    "too many symbol version requirements");
			}
			object->verneed_count = (uint32_t)dynamic->d_un.d_val;
			break;
		case DT_REL:
			rel_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_RELSZ:
			relsz = (size_t)dynamic->d_un.d_val;
			break;
		case DT_RELENT:
			relent = (size_t)dynamic->d_un.d_val;
			break;
		case DT_RELA:
			rela_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_RELASZ:
			relasz = (size_t)dynamic->d_un.d_val;
			break;
		case DT_RELAENT:
			relaent = (size_t)dynamic->d_un.d_val;
			break;
		case DT_JMPREL:
			jmprel_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_PLTRELSZ:
			object->jmprel_size = (size_t)dynamic->d_un.d_val;
			break;
		case DT_PLTREL:
			object->pltrel = (int)dynamic->d_un.d_val;
			break;
		case DT_INIT:
			object->init =
			    object->base + (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_FINI:
			object->fini =
			    object->base + (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_INIT_ARRAY:
			object->init_array =
			    (uintptr_t *)(object->base +
					  (uintptr_t)dynamic->d_un.d_ptr);
			break;
		case DT_INIT_ARRAYSZ:
			init_array_size = (size_t)dynamic->d_un.d_val;
			break;
		case DT_FINI_ARRAY:
			object->fini_array =
			    (uintptr_t *)(object->base +
					  (uintptr_t)dynamic->d_un.d_ptr);
			break;
		case DT_FINI_ARRAYSZ:
			fini_array_size = (size_t)dynamic->d_un.d_val;
			break;
		case DT_PREINIT_ARRAY:
			object->preinit_array =
			    (uintptr_t *)(object->base +
					  (uintptr_t)dynamic->d_un.d_ptr);
			break;
		case DT_PREINIT_ARRAYSZ:
			preinit_array_size = (size_t)dynamic->d_un.d_val;
			break;
		case DT_RPATH:
			rpath_offset = (uintptr_t)dynamic->d_un.d_val;
			break;
		case DT_RUNPATH:
			runpath_offset = (uintptr_t)dynamic->d_un.d_val;
			break;
		case DT_TEXTREL:
			rtld_fatal("unsupported dynamic feature");
		default:
			break;
		}
	}

	/* Checks the current index. */
	if (i == object->dynamic_count || object->strsz == 0 ||
	    strtab_value == 0 || symtab_value == 0 ||
	    (hash_value == 0 && gnu_hash_value == 0) ||
	    syment != sizeof(Elf_Sym) || relent != sizeof(Elf_Rel) ||
	    relaent != sizeof(Elf_Rela) || relsz % sizeof(Elf_Rel) != 0 ||
	    relasz % sizeof(Elf_Rela) != 0 ||
	    init_array_size % sizeof(uintptr_t) != 0 ||
	    fini_array_size % sizeof(uintptr_t) != 0 ||
	    preinit_array_size % sizeof(uintptr_t) != 0)
		rtld_fatal("malformed dynamic table");
	object->strtab = (const char *)object_pointer(
	    object, (Elf_Addr)strtab_value, object->strsz, PF_R);
	object->symtab = (Elf_Sym *)object_pointer(
	    object, (Elf_Addr)symtab_value, sizeof(Elf_Sym), PF_R);

	/* Handles the hash value condition. */
	if (hash_value != 0) {
		object->hash = (uint32_t *)object_pointer(
		    object, (Elf_Addr)hash_value, 2U * sizeof(uint32_t), PF_R);

		/* Checks the current object. */
		if (object->hash[0] == 0 || object->hash[1] == 0)
			rtld_fatal("invalid SysV hash");
		hash_words = 2U;

		/* Checks the current object. */
		if ((size_t)object->hash[0] > SIZE_MAX - hash_words)
			rtld_fatal("invalid SysV hash size");
		hash_words += object->hash[0];

		/* Checks the current object. */
		if ((size_t)object->hash[1] > SIZE_MAX - hash_words ||
		    sizeof(uint32_t) >
			SIZE_MAX / (hash_words + object->hash[1]))
			rtld_fatal("invalid SysV hash size");
		hash_words += object->hash[1];
		object->symbol_count = object->hash[1];
		(void)object_pointer(object, (Elf_Addr)hash_value,
				     hash_words * sizeof(uint32_t), PF_R);
	}

	/* Handles the gnu hash value condition. */
	if (gnu_hash_value != 0) {
		header =
		    (uint32_t *)object_pointer(object, (Elf_Addr)gnu_hash_value,
					       4U * sizeof(uint32_t), PF_R);
		object->gnu_bucket_count = header[0];
		object->gnu_symbol_offset = header[1];
		object->gnu_bloom_count = header[2];
		object->gnu_bloom_shift = header[3];

		/* Checks the current object. */
		if (object->gnu_bucket_count == 0 ||
		    object->gnu_bloom_count == 0 ||
		    (object->gnu_bloom_count &
		     (object->gnu_bloom_count - 1U)) != 0 ||
		    object->gnu_bloom_shift >= sizeof(Elf_Addr) * 8U ||
		    sizeof(Elf_Addr) > SIZE_MAX / object->gnu_bloom_count ||
		    sizeof(uint32_t) > SIZE_MAX / object->gnu_bucket_count)
			rtld_fatal("invalid GNU hash header");
		bloom_bytes =
		    (size_t)object->gnu_bloom_count * sizeof(Elf_Addr);
		bucket_bytes =
		    (size_t)object->gnu_bucket_count * sizeof(uint32_t);

		/* Handles the gnu hash value condition. */
		if (gnu_hash_value > UINTPTR_MAX - 4U * sizeof(uint32_t))
			rtld_fatal("GNU hash address overflow");
		offset = gnu_hash_value + 4U * sizeof(uint32_t);
		object->gnu_bloom = (Elf_Addr *)object_pointer(
		    object, (Elf_Addr)offset, bloom_bytes, PF_R);

		/* Checks the current offset. */
		if (offset > UINTPTR_MAX - bloom_bytes)
			rtld_fatal("GNU hash address overflow");
		offset += bloom_bytes;
		object->gnu_bucket = (uint32_t *)object_pointer(
		    object, (Elf_Addr)offset, bucket_bytes, PF_R);

		/* Checks the current offset. */
		if (offset > UINTPTR_MAX - bucket_bytes)
			rtld_fatal("GNU hash address overflow");
		offset += bucket_bytes;
		chain_bytes = object_readable_bytes(object, (Elf_Addr)offset);
		chain_capacity = chain_bytes / sizeof(uint32_t);

		/* Handles the chain capacity condition. */
		if (chain_capacity != 0) {
			object->gnu_chain = (uint32_t *)object_pointer(
			    object, (Elf_Addr)offset, sizeof(uint32_t), PF_R);
		}

		/* Process each remaining element. */
		gnu_symbol_count = object->gnu_symbol_offset;
		for (bucket_index = 0; bucket_index < object->gnu_bucket_count;
		     bucket_index++) {
			symbol = object->gnu_bucket[bucket_index];

			/* Handles the symbol condition. */
			if (symbol == 0)
				continue;

			/* Handles the symbol condition. */
			if (symbol < object->gnu_symbol_offset)
				rtld_fatal("invalid GNU hash bucket");
			chain_index =
			    (size_t)symbol - object->gnu_symbol_offset;

			/* Continue until the operation reaches a terminal state. */
			for (;;) {
				/* Handles the chain index condition. */
				if (chain_index >= chain_capacity) {
					rtld_fatal(
					    "unterminated GNU hash chain");
				}

				/* Checks the current object. */
				if ((object->gnu_chain[chain_index] & 1U) != 0)
					break;

				/* Handles the symbol condition. */
				if (symbol == UINT32_MAX) {
					rtld_fatal(
					    "unterminated GNU hash chain");
				}
				symbol++;
				chain_index++;
			}

			/* Handles the symbol condition. */
			if (symbol == UINT32_MAX)
				rtld_fatal("GNU hash symbol count overflow");

			/* Handles the symbol condition. */
			if (symbol + 1U > gnu_symbol_count)
				gnu_symbol_count = symbol + 1U;
		}

		/* Handles the hash availability. */
		if (object->hash != NULL) {
			/* Checks the current object. */
			if (object->gnu_symbol_offset > object->symbol_count ||
			    gnu_symbol_count > object->symbol_count)
				rtld_fatal("GNU and SysV hash disagree");
		} else {
			/* Handles the gnu symbol count condition. */
			if (gnu_symbol_count == 0)
				rtld_fatal("empty GNU symbol table");
			object->symbol_count = gnu_symbol_count;
		}
		chain_capacity =
		    (size_t)object->symbol_count - object->gnu_symbol_offset;

		/* Handles the chain capacity condition. */
		if (chain_capacity > SIZE_MAX / sizeof(uint32_t))
			rtld_fatal("invalid GNU hash chain size");

		/* Handles the chain capacity condition. */
		if (chain_capacity != 0) {
			(void)object_pointer(object, (Elf_Addr)offset,
					     chain_capacity * sizeof(uint32_t),
					     PF_R);
		}
	}

	/* Checks the current object. */
	if (object->symbol_count == 0 ||
	    sizeof(Elf_Sym) > SIZE_MAX / object->symbol_count)
		rtld_fatal("invalid dynamic symbol count");
	(void)object_pointer(object, (Elf_Addr)symtab_value,
			     (size_t)object->symbol_count * sizeof(Elf_Sym),
			     PF_R);

	/* Handles the verdef value condition. */
	if ((verdef_value == 0) != (object->verdef_count == 0) ||
	    (verneed_value == 0) != (object->verneed_count == 0) ||
	    ((verdef_value != 0 || verneed_value != 0) && versym_value == 0))
		rtld_fatal("incomplete symbol version tables");

	/* Handles the versym value condition. */
	if (versym_value != 0) {
		/* Handles the sizeof condition. */
		if (sizeof(Elf_Versym) > SIZE_MAX / object->symbol_count)
			rtld_fatal("invalid symbol version table size");
		object->versym = (Elf_Versym *)object_pointer(
		    object, (Elf_Addr)versym_value,
		    (size_t)object->symbol_count * sizeof(Elf_Versym), PF_R);
	}

	/* Handles the verdef value condition. */
	if (verdef_value != 0) {
		object->verdef_value = (Elf_Addr)verdef_value;
		validate_verdef(object);
	}

	/* Handles the verneed value condition. */
	if (verneed_value != 0) {
		object->verneed_value = (Elf_Addr)verneed_value;
		validate_verneed(object);
	}

	/* Handles the rel value condition. */
	if (rel_value != 0) {
		object->rel = (Elf_Rel *)object_pointer(
		    object, (Elf_Addr)rel_value, relsz, PF_R);
		object->rel_count = relsz / sizeof(Elf_Rel);
	}

	/* Handles the rela value condition. */
	if (rela_value != 0) {
		object->rela = (Elf_Rela *)object_pointer(
		    object, (Elf_Addr)rela_value, relasz, PF_R);
		object->rela_count = relasz / sizeof(Elf_Rela);
	}

	/* Handles the jmprel value condition. */
	if (jmprel_value != 0) {
		object->jmprel = (void *)object_pointer(
		    object, (Elf_Addr)jmprel_value, object->jmprel_size, PF_R);

		/* Checks the current object. */
		if ((object->pltrel == DT_REL &&
		     object->jmprel_size % sizeof(Elf_Rel) != 0) ||
		    (object->pltrel == DT_RELA &&
		     object->jmprel_size % sizeof(Elf_Rela) != 0) ||
		    (object->pltrel != DT_REL && object->pltrel != DT_RELA))
			rtld_fatal("invalid PLT relocations");
	}
	object->init_count = init_array_size / sizeof(uintptr_t);
	object->fini_count = fini_array_size / sizeof(uintptr_t);
	object->preinit_count = preinit_array_size / sizeof(uintptr_t);

	/* Handles the init array availability. */
	if (object->init_array != NULL) {
		(void)object_pointer(
		    object,
		    (Elf_Addr)((uintptr_t)object->init_array - object->base),
		    init_array_size, PF_R);
	}

	/* Handles the fini array availability. */
	if (object->fini_array != NULL) {
		(void)object_pointer(
		    object,
		    (Elf_Addr)((uintptr_t)object->fini_array - object->base),
		    fini_array_size, PF_R);
	}

	/* Handles the preinit array availability. */
	if (object->preinit_array != NULL) {
		(void)object_pointer(
		    object,
		    (Elf_Addr)((uintptr_t)object->preinit_array - object->base),
		    preinit_array_size, PF_R);
	}

	/* Process each remaining element. */
	for (i = 0; i < object->needed_count; i++) {
		offset = object->needed_offset[i];

		/* Handles a failed bounded string operation. */
		if (offset >= object->strsz ||
		    !bounded_string(object->strtab + offset,
				    object->strsz - offset, NULL))
			rtld_fatal("invalid dependency name");
	}

	/* Handles the rpath offset condition. */
	if (rpath_offset != UINTPTR_MAX) {
		/* Handles a failed bounded string operation. */
		if (rpath_offset >= object->strsz ||
		    !bounded_string(object->strtab + rpath_offset,
				    object->strsz - rpath_offset, NULL))
			rtld_fatal("invalid RPATH");
		object->rpath = object->strtab + rpath_offset;
	}

	/* Handles the runpath offset condition. */
	if (runpath_offset != UINTPTR_MAX) {
		/* Handles a failed bounded string operation. */
		if (runpath_offset >= object->strsz ||
		    !bounded_string(object->strtab + runpath_offset,
				    object->strsz - runpath_offset, NULL))
			rtld_fatal("invalid RUNPATH");
		object->runpath = object->strtab + runpath_offset;
	}
	register_tls_module(object);
}

/* Supports the object pointer operation. */
static uintptr_t
object_pointer(
	const struct rtld_object *object,
	Elf_Addr value,
	size_t size,
	uint32_t required)
{
	uintptr_t address;

	/* Handles the uintptr t condition. */
	if ((uintptr_t)value > UINTPTR_MAX - object->base)
		rtld_fatal("dynamic pointer overflow");
	address = object->base + (uintptr_t)value;

	/* Handles a failed object contains operation. */
	if (!object_contains(object, address, size, required))
		rtld_fatal("dynamic pointer outside object");

	/* Returns the computed result. */
	return address;
}

/* Supports the object contains operation. */
static int
object_contains(
	const struct rtld_object *object,
	uintptr_t address,
	size_t size,
	uint32_t required)
{
	uintptr_t start, end;
	unsigned i;

	/* Handles the object availability. */
	if (object == NULL || size == 0 || address > UINTPTR_MAX - size)
		return 0;

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Checks the current object. */
		if (object->phdr[i].p_type != PT_LOAD ||
		    (object->phdr[i].p_flags & required) != required)
			continue;
		start = object->base + (uintptr_t)object->phdr[i].p_vaddr;

		/* Handles the uintptr t condition. */
		if ((uintptr_t)object->phdr[i].p_memsz > UINTPTR_MAX - start)
			continue;
		end = start + (uintptr_t)object->phdr[i].p_memsz;

		/* Handles the address condition. */
		if (address >= start && address + size <= end)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the object readable bytes operation. */
static size_t
object_readable_bytes(
	const struct rtld_object *object,
	Elf_Addr value)
{
	Elf_Addr start, end;
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Checks the current object. */
		if (object->phdr[i].p_type != PT_LOAD ||
		    (object->phdr[i].p_flags & PF_R) == 0)
			continue;
		start = object->phdr[i].p_vaddr;

		/* Handles the uint64 t condition. */
		if ((uint64_t)object->phdr[i].p_memsz >
		    (uint64_t)(~(Elf_Addr)0 - start))
			continue;
		end = start + (Elf_Addr)object->phdr[i].p_memsz;

		/* Validates the current value. */
		if (value >= start && value < end)
			return (size_t)(end - value);
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the validate verdef operation. */
static void
validate_verdef(
	struct rtld_object *object)
{
	Elf_Verdaux *name;
	Elf_Verdef *definition;
	Elf_Addr auxiliary;
	uint16_t item;
	Elf_Addr cursor;
	uint32_t record;

	cursor = object->verdef_value;

	/* Process each remaining element. */
	for (record = 0; record < object->verdef_count; record++) {
		definition = (Elf_Verdef *)object_pointer(
		    object, cursor, sizeof(*definition), PF_R);

		/* Handles the definition condition. */
		if (definition->vd_version != VER_DEF_CURRENT ||
		    definition->vd_ndx == VER_NDX_LOCAL ||
		    (definition->vd_ndx & VER_NDX_HIDDEN) != 0 ||
		    ((definition->vd_ndx == VER_NDX_GLOBAL) !=
		     ((definition->vd_flags & VER_FLG_BASE) != 0)) ||
		    definition->vd_cnt == 0 || definition->vd_aux == 0)
			rtld_fatal("invalid symbol version definition");

		/* Process each element required by the operation. */
		auxiliary = version_offset(cursor, definition->vd_aux);
		for (item = 0; item < definition->vd_cnt; item++) {
			name = (Elf_Verdaux *)object_pointer(
			    object, auxiliary, sizeof(*name), PF_R);
			(void)dynamic_string(object, name->vda_name);

			/* Handles the item condition. */
			if (item + 1U < definition->vd_cnt) {
				/* Validates the current name. */
				if (name->vda_next == 0) {
					rtld_fatal("truncated symbol version "
						   "definition");
				}
				auxiliary =
				    version_offset(auxiliary, name->vda_next);
			}
		}

		/* Handles the record condition. */
		if (record + 1U < object->verdef_count) {
			/* Handles the definition condition. */
			if (definition->vd_next == 0) {
				rtld_fatal(
				    "truncated symbol version definitions");
			}
			cursor = version_offset(cursor, definition->vd_next);
		} else if (definition->vd_next != 0) {
			rtld_fatal("extra symbol version definitions");
		}
	}
}

/* Supports the version offset operation. */
static Elf_Addr
version_offset(
	Elf_Addr value,
	uint32_t offset)
{
	/* Handles the Elf Addr condition. */
	if ((Elf_Addr)offset > (Elf_Addr) ~(Elf_Addr)0 - value)
		rtld_fatal("symbol version table overflow");

	/* Returns the computed result. */
	return value + (Elf_Addr)offset;
}

/* Supports the dynamic string operation. */
static const char *
dynamic_string(
	struct rtld_object *object,
	uint32_t offset)
{
	/* Handles a failed bounded string operation. */
	if (offset >= object->strsz ||
	    !bounded_string(object->strtab + offset, object->strsz - offset,
			    NULL))
		rtld_fatal("invalid version string");

	/* Returns the computed result. */
	return object->strtab + offset;
}

/* Supports the bounded string operation. */
static int
bounded_string(
	const char *string,
	size_t capacity,
	size_t *length_out)
{
	size_t length;

	/* Process each remaining element. */
	for (length = 0; length < capacity; length++) {
		/* Handles the string condition. */
		if (string[length] == '\0') {
			/* Handles the length out availability. */
			if (length_out != NULL)
				*length_out = length;
			/* Reports operation failure. */
			return 1;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the validate verneed operation. */
static void
validate_verneed(
	struct rtld_object *object)
{
	Elf_Vernaux *name;
	Elf_Verneed *need;
	Elf_Addr auxiliary;
	uint16_t item;
	Elf_Addr cursor;
	uint32_t record;

	cursor = object->verneed_value;

	/* Process each remaining element. */
	for (record = 0; record < object->verneed_count; record++) {
		need = (Elf_Verneed *)object_pointer(
		    object, cursor, sizeof(*need), PF_R);

		/* Handles the need condition. */
		if (need->vn_version != VER_NEED_CURRENT || need->vn_cnt == 0 ||
		    need->vn_aux == 0)
			rtld_fatal("invalid symbol version requirement");
		(void)dynamic_string(object, need->vn_file);

		/* Process each element required by the operation. */
		auxiliary = version_offset(cursor, need->vn_aux);
		for (item = 0; item < need->vn_cnt; item++) {
			name = (Elf_Vernaux *)object_pointer(
			    object, auxiliary, sizeof(*name), PF_R);

			/* Validates the current name. */
			if ((name->vna_other & VER_NDX_MASK) <= VER_NDX_GLOBAL) {
				rtld_fatal(
				    "invalid required symbol version index");
			}
			(void)dynamic_string(object, name->vna_name);

			/* Handles the item condition. */
			if (item + 1U < need->vn_cnt) {
				/* Validates the current name. */
				if (name->vna_next == 0) {
					rtld_fatal("truncated symbol version "
						   "requirement");
				}
				auxiliary =
				    version_offset(auxiliary, name->vna_next);
			}
		}

		/* Handles the record condition. */
		if (record + 1U < object->verneed_count) {
			/* Handles the need condition. */
			if (need->vn_next == 0) {
				rtld_fatal(
				    "truncated symbol version requirements");
			}
			cursor = version_offset(cursor, need->vn_next);
		} else if (need->vn_next != 0) {
			rtld_fatal("extra symbol version requirements");
		}
	}
}

/* Supports the register tls module operation. */
static void
register_tls_module(
	struct rtld_object *object)
{
	struct rtld_tls_module *module;
	size_t alignment;
	unsigned i, id;

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Checks the current object. */
		if (object->phdr[i].p_type == PT_TLS) {
			alignment = (size_t)object->phdr[i].p_align;

			/* Checks the current object. */
			if (object->tls_module_id != 0 ||
			    object->phdr[i].p_filesz >
				object->phdr[i].p_memsz ||
			    object->phdr[i].p_memsz == 0)
				rtld_fatal("invalid TLS segment");

			/* Handles the alignment condition. */
			if (alignment == 0)
				alignment = 1;

			/* Handles the alignment condition. */
			if ((alignment & (alignment - 1U)) != 0 ||
			    alignment > RTLD_PAGE_SIZE ||
			    object->phdr[i].p_memsz > RTLD_PAGE_SIZE)
				rtld_fatal("unsupported TLS alignment or size");

			/* Process each remaining element. */
			for (id = 1; id <= tls_module_count; id++) {
				/* Handles the tls modules condition. */
				if (!tls_modules[id].active)
					break;
			}

			/* Handles the id condition. */
			if (id > tls_module_count) {
				/* Handles the tls module count condition. */
				if (tls_module_count == RTLD_OBJECT_MAX)
					rtld_fatal("too many TLS modules");
				id = ++tls_module_count;
			}
			object->tls_module_id = id;
			module = &tls_modules[object->tls_module_id];
			rtld_memset(module, 0, sizeof(*module));
			module->id = object->tls_module_id;
			module->file_size = (size_t)object->phdr[i].p_filesz;
			module->memory_size = (size_t)object->phdr[i].p_memsz;
			module->alignment = alignment;
			module->owner = object;

			/* Handles the module condition. */
			if (module->file_size != 0) {
				module->init_image =
				    (const void *)object_pointer(
					object, object->phdr[i].p_vaddr,
					module->file_size, PF_R);
			}
			module->active = 1;
			tls_generation++;
		}
	}
}

/* Supports the load dependencies operation. */
static void
load_dependencies(
	struct rtld_object *object)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < object->needed_count; i++) {
		object->needed[i] = load_object(
		    object->strtab + object->needed_offset[i], object);
		object->needed[i]->dependency_refs++;
	}
}

/* Supports the relocate object operation. */
static void
relocate_object(
	struct rtld_object *object)
{
	Elf_Rel *rel;
	Elf_Rela *rela;
	size_t i;
	uint32_t type;
	uintptr_t start;
	uintptr_t end;
	intptr_t result;

	/* Checks the current object. */
	if (object->relocated)
		return;

	/* Checks the current object. */
	if (object->relocating)
		return;

	/* Process each remaining element. */
	object->relocating = 1;
	for (i = 0; i < object->needed_count; i++)
		relocate_object(object->needed[i]);

	/* Process each remaining element. */
	for (i = 0; i < object->rel_count; i++) {
		type = ELF_R_TYPE(object->rel[i].r_info);

		/* Checks the current object. */
		if (object->relative_done && type == RTLD_RELATIVE)
			continue;
		apply_value(object, (uintptr_t)object->rel[i].r_offset, type,
			    ELF_R_SYM(object->rel[i].r_info), 0, 0);
	}

	/* Process each remaining element. */
	for (i = 0; i < object->rela_count; i++) {
		type = ELF_R_TYPE(object->rela[i].r_info);

		/* Checks the current object. */
		if (object->relative_done && type == RTLD_RELATIVE)
			continue;
		apply_value(object, (uintptr_t)object->rela[i].r_offset, type,
			    ELF_R_SYM(object->rela[i].r_info),
			    (uintptr_t)object->rela[i].r_addend, 1);
	}

	/* Handles the jmprel availability. */
	if (object->jmprel != NULL && object->pltrel == DT_REL) {
		/* Process each remaining element. */
		rel = object->jmprel;
		for (i = 0; i < object->jmprel_size / sizeof(*rel); i++) {
			apply_value(object, (uintptr_t)rel[i].r_offset,
				    ELF_R_TYPE(rel[i].r_info),
				    ELF_R_SYM(rel[i].r_info), 0, 0);
		}
	} else if (object->jmprel != NULL && object->pltrel == DT_RELA) {
		/* Process each remaining element. */
		rela = object->jmprel;
		for (i = 0; i < object->jmprel_size / sizeof(*rela); i++) {
			apply_value(object, (uintptr_t)rela[i].r_offset,
				    ELF_R_TYPE(rela[i].r_info),
				    ELF_R_SYM(rela[i].r_info),
				    (uintptr_t)rela[i].r_addend, 1);
		}
	}
#if defined(HAL_ARCH_SPARCV9)

	/*
 * SPARC PLT instructions are writable only while JMP_SLOT is applied.
	 */
	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Handles a failed temporary writable plt operation. */
		if (!temporary_writable_plt(&object->phdr[i]))
			continue;
		start = object->base + (uintptr_t)object->phdr[i].p_vaddr;
		result = syscall6(ZEDBSD_SYS_mprotect, start, RTLD_PAGE_SIZE,
				  PROT_READ | PROT_EXEC, 0, 0, 0);

		/* Handles an operation failure. */
		if (raw_error(result))
			rtld_fatal("cannot seal SPARC PLT");
	}
#endif

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Checks the current object. */
		if (object->phdr[i].p_type != PT_GNU_RELRO ||
		    object->phdr[i].p_memsz == 0)
			continue;

		/* Checks the current object. */
		if (object->phdr[i].p_vaddr >
		    (Elf_Addr)UINTPTR_MAX - object->phdr[i].p_memsz)
			rtld_fatal("invalid GNU_RELRO range");
		(void)object_pointer(object, object->phdr[i].p_vaddr,
				     (size_t)object->phdr[i].p_memsz, PF_R);
		start = page_floor(object->base +
				   (uintptr_t)object->phdr[i].p_vaddr);
		end = page_ceil(object->base +
				(uintptr_t)object->phdr[i].p_vaddr +
				(uintptr_t)object->phdr[i].p_memsz);
		result = syscall6(ZEDBSD_SYS_mprotect, start, end - start,
				  PROT_READ, 0, 0, 0);

		/* Handles an operation failure. */
		if (raw_error(result)) {
			rtld_debug("ld.so: GNU_RELRO mprotect failed for ");
			rtld_debug(object->path);

			/* Checks the operation result. */
			if (result == -3)
				rtld_debug(" (EINVAL)");
			else if (result == -4)
				rtld_debug(" (ENOMEM)");
			else if (result == -25)
				rtld_debug(" (EACCES)");
			rtld_debug("\n");
			rtld_fatal("cannot protect GNU_RELRO");
		}
	}
	object->relocating = 0;
	object->relocated = 1;
}

/* Supports the apply value operation. */
static void
apply_value(
	struct rtld_object *object,
	uintptr_t offset,
	uint32_t type,
	uint32_t symbol_index,
	uintptr_t addend,
	int is_rela)
{
	uintptr_t address, symbol, value;
	uintptr_t *where;
	struct rtld_object *tls_owner;
	Elf_Sym *tls_symbol;

	symbol = 0;
	tls_owner = NULL;

	/* NONE relocations have neither a target nor a symbol to validate. */
#if defined(HAL_ARCH_I386)

	/* Handles the type condition. */
	if (type == R_386_NONE)
		return;
#elif defined(HAL_ARCH_AMD64)

	/* Handles the type condition. */
	if (type == R_X86_64_NONE)
		return;
#elif defined(HAL_ARCH_ARM64)

	/* Handles the type condition. */
	if (type == R_AARCH64_NONE)
		return;
#elif defined(HAL_ARCH_SPARCV9)

	/* Handles the type condition. */
	if (type == R_SPARC_NONE)
		return;
#endif

	/* Checks the current offset. */
	if (offset > UINTPTR_MAX - object->base)
		rtld_fatal("relocation target overflow");
	address = object->base + offset;

	/* Handles a failed object contains operation. */
	if (!object_contains(object, address,
#if defined(HAL_ARCH_SPARCV9)
			     type == R_SPARC_JMP_SLOT ? 8U * sizeof(uint32_t) :
#endif
						      sizeof(uintptr_t),
			     PF_W))
		rtld_fatal("relocation target is not writable");
	where = (uintptr_t *)address;

	/* Handles the rela condition. */
	if (!is_rela)
		addend = *where;

#if defined(HAL_ARCH_I386)

	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case R_386_NONE:
		/* Returns the computed result. */
		return;
	case R_386_RELATIVE:
		/* Handles the symbol index condition. */
		if (symbol_index != 0)
			rtld_fatal("invalid relative relocation");
		value = object->base + addend;
		break;
	case R_386_32:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend;
		break;
	case R_386_PC32:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend - address;
		break;
	case R_386_GLOB_DAT:
	case R_386_JMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index);
		break;
	case R_386_TLS_DTPMOD32:
		/* Handles the symbol index condition. */
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
							&tls_owner);

		/* Handles the tls owner availability. */
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id;
		break;
	case R_386_TLS_DTPOFF32:
		tls_symbol =
		    resolve_tls_symbol(object, symbol_index, &tls_owner);

		/* Handles the tls symbol availability. */
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend;
		break;
	default:
		rtld_fatal("unsupported i386 relocation");
	}
#elif defined(HAL_ARCH_AMD64)

	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case R_X86_64_NONE:
		/* Returns the computed result. */
		return;
	case R_X86_64_RELATIVE:
		/* Handles the symbol index condition. */
		if (symbol_index != 0)
			rtld_fatal("invalid relative relocation");
		value = object->base + addend;
		break;
	case R_X86_64_64:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend;
		break;
	case R_X86_64_GLOB_DAT:
	case R_X86_64_JUMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index);
		break;
	case R_X86_64_DTPMOD64:
		/* Handles the symbol index condition. */
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
							&tls_owner);

		/* Handles the tls owner availability. */
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id;
		break;
	case R_X86_64_DTPOFF64:
		tls_symbol =
		    resolve_tls_symbol(object, symbol_index, &tls_owner);

		/* Handles the tls symbol availability. */
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend;
		break;
	case R_X86_64_TLSDESC:
		install_tlsdesc(object, address, symbol_index, addend);

		/* Returns the computed result. */
		return;
	default:
		rtld_fatal("unsupported amd64 relocation");
	}
#elif defined(HAL_ARCH_ARM64)

	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case R_AARCH64_NONE:
		/* Returns the computed result. */
		return;
	case R_AARCH64_RELATIVE:
		/* Handles the symbol index condition. */
		if (symbol_index != 0)
			rtld_fatal("invalid relative relocation");
		value = object->base + addend;
		break;
	case R_AARCH64_ABS64:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend;
		break;
	case R_AARCH64_GLOB_DAT:
	case R_AARCH64_JUMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index);
		break;
	case R_AARCH64_TLS_DTPMOD64:
		/* Handles the symbol index condition. */
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
							&tls_owner);

		/* Handles the tls owner availability. */
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id;
		break;
	case R_AARCH64_TLS_DTPREL64:
		tls_symbol =
		    resolve_tls_symbol(object, symbol_index, &tls_owner);

		/* Handles the tls symbol availability. */
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend;
		break;
	case R_AARCH64_TLSDESC:
		install_tlsdesc(object, address, symbol_index, addend);

		/* Returns the computed result. */
		return;
	default:
		rtld_fatal("unsupported aarch64 relocation");
	}
#elif defined(HAL_ARCH_SPARCV9)

	/* Dispatch the selected syntax or record type. */
	switch (type) {
	case R_SPARC_NONE:
		/* Returns the computed result. */
		return;
	case R_SPARC_RELATIVE:
		/* Handles the symbol index condition. */
		if (symbol_index != 0)
			rtld_fatal("invalid relative relocation");
		value = object->base + addend;
		break;
	case R_SPARC_64:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend;
		break;
	case R_SPARC_GLOB_DAT:
		value = resolve_relocation_symbol(object, symbol_index);
		break;
	case R_SPARC_JMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index);
		sparcv9_patch_jmp_slot((uint32_t *)where, value);

		/* Returns the computed result. */
		return;
	case R_SPARC_TLS_DTPMOD64:
		/* Handles the symbol index condition. */
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
							&tls_owner);

		/* Handles the tls owner availability. */
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id;
		break;
	case R_SPARC_TLS_DTPOFF64:
		tls_symbol =
		    resolve_tls_symbol(object, symbol_index, &tls_owner);

		/* Handles the tls symbol availability. */
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend;
		break;
	default:
		rtld_fatal("unsupported sparcv9 relocation");
	}
#endif
	*where = value;
}

/* Supports the resolve relocation symbol operation. */
static uintptr_t
resolve_relocation_symbol(
	struct rtld_object *object,
	uint32_t index)
{
	uintptr_t function_result;
	Elf_Sym *symbol;
	const char *name;
	unsigned binding;

	/* Checks the current index. */
	if (index == 0)
		return 0;

	/* Checks the current index. */
	if (index >= object->symbol_count)
		rtld_fatal("invalid relocation symbol");
	symbol = &object->symtab[index];

	/* Handles the symbol condition. */
	if (symbol->st_name >= object->strsz)
		rtld_fatal("invalid relocation symbol name");
	binding = ELF_ST_BIND(symbol->st_info);

	/* Handles the symbol condition. */
	if (symbol->st_shndx != SHN_UNDEF && binding == STB_LOCAL) {
		/* Obtains the symbol value result. */
		function_result = symbol_value(object, symbol);

		/* Returns the computed result. */
		return function_result;
	}

	name = object->strtab + symbol->st_name;

	/* Obtains the lookup symbol version result. */
	function_result = lookup_symbol_version(
	    name, relocation_version_name(object, index), binding == STB_WEAK);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the symbol value operation. */
static uintptr_t
symbol_value(
	struct rtld_object *object,
	const Elf_Sym *symbol)
{
	/* Handles the symbol condition. */
	if (symbol->st_shndx == SHN_ABS)
		return (uintptr_t)symbol->st_value;

	/* Handles the uintptr t condition. */
	if ((uintptr_t)symbol->st_value > UINTPTR_MAX - object->base)
		rtld_fatal("symbol address overflow");

	/* Returns the computed result. */
	return object->base + (uintptr_t)symbol->st_value;
}

/* Supports the lookup symbol version operation. */
static uintptr_t
lookup_symbol_version(
	const char *name,
	const char *required_version,
	int weak)
{
	uintptr_t function_result;
	struct rtld_object *object;
	Elf_Sym *symbol;
	unsigned i;

	/* Handles the reserved loader symbol condition. */
	if (reserved_loader_symbol(name)) {
		symbol = lookup_in_object_version(interpreter_object, name,
						  required_version);

		/* Handles the symbol availability. */
		if (symbol != NULL) {
			/* Obtains the symbol value result. */
			function_result = symbol_value(interpreter_object, symbol);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the weak condition. */
		if (weak)
			return 0;
		rtld_fatal("missing private loader symbol");
	}
	symbol = lookup_in_object_version(main_object, name, required_version);

	/* Handles the symbol availability. */
	if (symbol != NULL) {
		/* Obtains the symbol value result. */
		function_result = symbol_value(main_object, symbol);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		object = &objects[i];

		/* Checks the current object. */
		if (!object->active || object->unloading ||
		    object == main_object || object == interpreter_object)
			continue;
		symbol =
		    lookup_in_object_version(object, name, required_version);

		/* Handles the symbol availability. */
		if (symbol != NULL) {
			/* Obtains the symbol value result. */
			function_result = symbol_value(object, symbol);

			/* Returns the computed result. */
			return function_result;
		}
	}
	symbol = lookup_in_object_version(interpreter_object, name,
					  required_version);

	/* Handles the symbol availability. */
	if (symbol != NULL) {
		/* Obtains the symbol value result. */
		function_result = symbol_value(interpreter_object, symbol);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the weak condition. */
	if (weak)
		return 0;
	rtld_fatal("undefined symbol");
}

/* Supports the reserved loader symbol operation. */
static int
reserved_loader_symbol(
	const char *name)
{
	static const char *const names[] = {
	    "__tls_get_addr",	      "___tls_get_addr",
	    "__rtld_abi_version",     "__rtld_thread_alloc",
	    "__rtld_thread_free",     "__rtld_thread_attach",
	    "__rtld_pthread_private", "__rtld_startup_init",
	    "__rtld_fork_prepare",    "__rtld_fork_parent",
	    "__rtld_fork_child",      "__rtld_dlopen",
	    "__rtld_dlsym",	      "__rtld_dlvsym",
	    "__rtld_dlclose",	      "__rtld_dlerror",
	    "__rtld_process_fini"};
	size_t i;

	/* Process each remaining element. */
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		/* Handles a failed rtld strcmp operation. */
		if (rtld_strcmp(name, names[i]) == 0)
			return 1;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the lookup in object version operation. */
static Elf_Sym *
lookup_in_object_version(
	struct rtld_object *object,
	const char *name,
	const char *required_version)
{
	Elf_Sym *function_result;
	Elf_Sym *symbol;
	uint32_t buckets, index, *bucket, *chain;
	unsigned traversed;

	traversed = 0;

	/* Handles the object availability. */
	if (object == NULL || !object->active || object->unloading ||
	    (object->hash == NULL && object->gnu_bloom == NULL))

		/* Reports that no result is available. */
		return NULL;

	/* Handles the gnu bloom availability. */
	if (object->gnu_bloom != NULL) {
		/* Obtains the lookup gnu hash result. */
		function_result = lookup_gnu_hash(object, name, required_version);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	buckets = object->hash[0];
	bucket = object->hash + 2;
	chain = bucket + buckets;
	index = bucket[elf_hash(name) % buckets];
	while (index != STN_UNDEF && traversed++ < object->symbol_count) {
		/* Checks the current index. */
		if (index >= object->symbol_count)
			rtld_fatal("corrupt symbol hash chain");
		symbol = match_symbol(object, index, name, required_version);

		/* Handles the symbol availability. */
		if (symbol != NULL)
			return symbol;
		index = chain[index];
	}

	/* Handles the traversed condition. */
	if (traversed > object->symbol_count)
		rtld_fatal("cyclic symbol hash chain");

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the lookup gnu hash operation. */
static Elf_Sym *
lookup_gnu_hash(
	struct rtld_object *object,
	const char *name,
	const char *required_version)
{
	uint32_t chain_hash;
	Elf_Sym *symbol;
	const unsigned word_bits = sizeof(Elf_Addr) * 8U;
	uint32_t hash;
	Elf_Addr mask, bloom;
	uint32_t index;

	hash = gnu_hash_name(name);

	bloom = object->gnu_bloom[(hash / word_bits) &
				  (object->gnu_bloom_count - 1U)];
	mask =
	    ((Elf_Addr)1U << (hash % word_bits)) |
	    ((Elf_Addr)1U << ((hash >> object->gnu_bloom_shift) % word_bits));

	/* Handles the bloom condition. */
	if ((bloom & mask) != mask)
		return NULL;
	index = object->gnu_bucket[hash % object->gnu_bucket_count];

	/* Checks the current index. */
	if (index == 0)
		return NULL;

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		/* Checks the current index. */
		if (index < object->gnu_symbol_offset ||
		    index >= object->symbol_count)
			rtld_fatal("corrupt GNU hash chain");
		chain_hash =
		    object->gnu_chain[index - object->gnu_symbol_offset];

		/* Handles the chain hash condition. */
		if ((chain_hash | 1U) == (hash | 1U)) {
			symbol =
			    match_symbol(object, index, name, required_version);

			/* Handles the symbol availability. */
			if (symbol != NULL)
				return symbol;
		}

		/* Handles the chain hash condition. */
		if ((chain_hash & 1U) != 0)
			return NULL;
		index++;
	}
}

/* Supports the gnu hash name operation. */
static uint32_t
gnu_hash_name(
	const char *name)
{
	uint32_t hash;

	/* Continue while the operation condition remains true. */
	hash = 5381U;
	while (*name != '\0')
		hash = hash * 33U + (unsigned char)*name++;

	/* Returns the computed result. */
	return hash;
}

/* Supports the match symbol operation. */
static Elf_Sym *
match_symbol(
	struct rtld_object *object,
	uint32_t index,
	const char *name,
	const char *required_version)
{
	Elf_Sym *symbol;
	const char *symbol_name;
	unsigned binding, visibility;

	/* Checks the current index. */
	if (index >= object->symbol_count)
		rtld_fatal("symbol index outside table");
	symbol = &object->symtab[index];

	/* Handles the symbol condition. */
	if (symbol->st_name >= object->strsz)
		rtld_fatal("invalid symbol name");
	symbol_name = object->strtab + symbol->st_name;
	binding = ELF_ST_BIND(symbol->st_info);
	visibility = ELF_ST_VISIBILITY(symbol->st_other);

	/* Handles a failed rtld strcmp operation. */
	if (symbol->st_shndx != SHN_UNDEF &&
	    (binding == STB_GLOBAL || binding == STB_WEAK) &&
	    visibility != STV_HIDDEN && rtld_strcmp(symbol_name, name) == 0 &&
	    symbol_version_matches(object, index, required_version))

		/* Returns the computed result. */
		return symbol;

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the symbol version matches operation. */
static int
symbol_version_matches(
	struct rtld_object *object,
	uint32_t symbol_index,
	const char *required_version)
{
	int function_result;
	uint16_t raw, index;
	const char *provided;

	/* Handles the versym availability. */
	if (object->versym == NULL)
		return required_version == NULL;
	raw = object->versym[symbol_index];
	index = raw & VER_NDX_MASK;

	/* Checks the current index. */
	if (index <= VER_NDX_GLOBAL)
		return required_version == NULL;
	provided = defined_version_name(object, index);

	/* Handles the provided availability. */
	if (provided == NULL)
		rtld_fatal("unknown defined symbol version");

	/* Handles the required version availability. */
	if (required_version != NULL) {
		/* Computes the function result. */
		function_result = rtld_strcmp(provided, required_version) == 0;

		/* Returns the computed result. */
		return function_result;
	}

	/* Returns the computed result. */
	return (raw & VER_NDX_HIDDEN) == 0;
}

/* Supports the defined version name operation. */
static const char *
defined_version_name(
	struct rtld_object *object,
	uint16_t version_index)
{
	const char *function_result;
	Elf_Addr auxiliary;
	Elf_Verdaux *name;
	Elf_Verdef *definition;
	Elf_Addr cursor;
	uint32_t record;

	cursor = object->verdef_value;

	/* Process each remaining element. */
	for (record = 0; record < object->verdef_count; record++) {
		definition = (Elf_Verdef *)object_pointer(
		    object, cursor, sizeof(*definition), PF_R);

		/* Handles the definition condition. */
		if ((definition->vd_ndx & VER_NDX_MASK) == version_index) {
			auxiliary = version_offset(cursor, definition->vd_aux);
			name = (Elf_Verdaux *)object_pointer(
		    object, auxiliary, sizeof(*name), PF_R);

			/* Obtains the dynamic string result. */
			function_result = dynamic_string(object, name->vda_name);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the definition condition. */
		if (definition->vd_next == 0)
			break;
		cursor = version_offset(cursor, definition->vd_next);
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the elf hash operation. */
static uint32_t
elf_hash(
	const char *name)
{
	uint32_t hash, high;

	/* Continue while the operation condition remains true. */
	hash = 0;
	while (*name != '\0') {
		hash = (hash << 4) + (unsigned char)*name++;
		high = hash & 0xf0000000U;

		/* Handles the high condition. */
		if (high != 0)
			hash ^= high >> 24;
		hash &= ~high;
	}

	/* Returns the computed result. */
	return hash;
}

/* Supports the relocation version name operation. */
static const char *
relocation_version_name(
	struct rtld_object *object,
	uint32_t symbol_index)
{
	Elf_Sym *symbol;
	uint16_t index;
	const char *name;

	/* Handles the versym availability. */
	if (object->versym == NULL)
		return NULL;
	index = object->versym[symbol_index] & VER_NDX_MASK;

	/* Checks the current index. */
	if (index <= VER_NDX_GLOBAL)
		return NULL;
	symbol = &object->symtab[symbol_index];
	name = symbol->st_shndx == SHN_UNDEF
		   ? required_version_name(object, index)
		   : defined_version_name(object, index);

	/* Handles the name availability. */
	if (name == NULL)
		rtld_fatal("unknown relocation symbol version");

	/* Returns the computed result. */
	return name;
}

/* Supports the required version name operation. */
static const char *
required_version_name(
	struct rtld_object *object,
	uint16_t version_index)
{
	const char *function_result;
	Elf_Vernaux *name;
	Elf_Verneed *need;
	Elf_Addr auxiliary;
	uint16_t item;
	Elf_Addr cursor;
	uint32_t record;

	cursor = object->verneed_value;

	/* Process each remaining element. */
	for (record = 0; record < object->verneed_count; record++) {
		need = (Elf_Verneed *)object_pointer(
		    object, cursor, sizeof(*need), PF_R);
		auxiliary = version_offset(cursor, need->vn_aux);

		/* Process each element required by the operation. */
		for (item = 0; item < need->vn_cnt; item++) {
			name = (Elf_Vernaux *)object_pointer(
			    object, auxiliary, sizeof(*name), PF_R);

			/* Validates the current name. */
			if ((name->vna_other & VER_NDX_MASK) == version_index) {
				/* Obtains the dynamic string result. */
				function_result = dynamic_string(object, name->vna_name);

				/* Returns the computed result. */
				return function_result;
			}

			/* Validates the current name. */
			if (name->vna_next == 0)
				break;
			auxiliary = version_offset(auxiliary, name->vna_next);
		}

		/* Handles the need condition. */
		if (need->vn_next == 0)
			break;
		cursor = version_offset(cursor, need->vn_next);
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Supports the resolve tls symbol operation. */
static Elf_Sym *
resolve_tls_symbol(
	struct rtld_object *object,
	uint32_t index,
	struct rtld_object **owner)
{
	struct rtld_object *candidate;
	Elf_Sym *symbol;
	const char *name, *version;
	unsigned binding, i;

	/* Checks the current index. */
	if (index == 0 || index >= object->symbol_count)
		rtld_fatal("invalid TLS relocation symbol");
	symbol = &object->symtab[index];

	/* Handles the symbol condition. */
	if (symbol->st_name >= object->strsz)
		rtld_fatal("invalid TLS symbol name");
	binding = ELF_ST_BIND(symbol->st_info);

	/* Handles the symbol condition. */
	if (symbol->st_shndx != SHN_UNDEF && binding == STB_LOCAL) {
		*owner = object;
		/* Returns the computed result. */
		return symbol;
	}
	name = object->strtab + symbol->st_name;
	version = relocation_version_name(object, index);
	symbol = lookup_in_object_version(main_object, name, version);

	/* Handles the symbol availability. */
	if (symbol != NULL) {
		*owner = main_object;
		/* Returns the computed result. */
		return symbol;
	}

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		candidate = &objects[i];

		/* Handles the candidate condition. */
		if (!candidate->active || candidate->unloading ||
		    candidate == main_object || candidate == interpreter_object)
			continue;
		symbol = lookup_in_object_version(candidate, name, version);

		/* Handles the symbol availability. */
		if (symbol != NULL) {
			*owner = candidate;
			/* Returns the computed result. */
			return symbol;
		}
	}

	/* Handles the binding condition. */
	if (binding == STB_WEAK)
		return NULL;
	rtld_fatal("undefined TLS symbol");
}

#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64)
extern uintptr_t d_tlsdesc_resolver(void);

/* Supports the install tlsdesc operation. */
static void
install_tlsdesc(
	struct rtld_object *object,
	uintptr_t address,
	uint32_t symbol_index,
	uintptr_t addend)
{
	struct rtld_object *owner;
	struct rtld_tlsdesc *descriptor;
	struct __tls_index *index;
	Elf_Sym *symbol;
	uintptr_t offset;

	owner = object;
	symbol = NULL;
	offset = addend;

	/* Handles a failed object contains operation. */
	if (!object_contains(object, address, sizeof(*descriptor), PF_W))
		rtld_fatal("TLSDESC target is not writable");

	/* Checks the current object. */
	if (object->tlsdesc_count == sizeof(object->tlsdesc_argument) /
					 sizeof(object->tlsdesc_argument[0]))
		rtld_fatal("too many TLSDESC relocations");

	/* Handles the symbol index condition. */
	if (symbol_index != 0) {
		symbol = resolve_tls_symbol(object, symbol_index, &owner);

		/* Handles the symbol availability. */
		if (symbol == NULL)
			rtld_fatal("weak TLSDESC symbol is unavailable");
		offset += (uintptr_t)symbol->st_value;
	}

	/* Handles the owner availability. */
	if (owner == NULL || owner->tls_module_id == 0 ||
	    offset >= tls_modules[owner->tls_module_id].memory_size)
		rtld_fatal("invalid TLSDESC module or offset");
	index = &object->tlsdesc_argument[object->tlsdesc_count++].index;
	index->module = owner->tls_module_id;
	index->offset = offset;
	descriptor = (struct rtld_tlsdesc *)address;
	descriptor->resolver = (uintptr_t)d_tlsdesc_resolver;
	descriptor->argument = (uintptr_t)index;
}
#endif

#if defined(HAL_ARCH_SPARCV9)
/* Supports the sparcv9 patch jmp slot operation. */
static void
sparcv9_patch_jmp_slot(
	uint32_t *where,
	uintptr_t value)
{
	unsigned i;
	uint32_t instruction[8];

	/* GNU SPARC64 PLT entries are eight instructions (32 bytes). */
	instruction[0] = 0x03000000U | (uint32_t)((value >> 42) & 0x3fffffU);
	instruction[1] = 0x09000000U | (uint32_t)((value >> 10) & 0x3fffffU);
	instruction[2] = 0x82106000U | (uint32_t)((value >> 32) & 0x3ffU);
	instruction[3] = 0x83287020U;

	/* Match the canonical gas "setx value, %g4, %g1" expansion. */
	/* Process each element required by the operation. */
	instruction[4] = 0x82004004U;
	instruction[5] = 0x82106000U | (uint32_t)(value & 0x3ffU);
	instruction[6] = 0x81c04000U;
	instruction[7] = 0x01000000U;
	for (i = 0; i < 8; i++)
		where[i] = instruction[i];
	__asm__ volatile("membar #Sync" : : : "memory");

	/* Process each element required by the operation. */
	for (i = 0; i < 8; i++)
		__asm__ volatile("flush %0" : : "r"(&where[i]) : "memory");
	__asm__ volatile("membar #Sync" : : : "memory");
}
#endif

/* Called with the recursive loader lock held; returns with it held. */
static void
unload_object_locked(
	struct rtld_object *object)
{
	intptr_t result;
	struct __rtld_tcb *tcb;
	struct rtld_object *dependencies[RTLD_NEEDED_MAX];
	struct rtld_tls_module *module;
	size_t tls_size;
	unsigned i, dependency_count;

	module = NULL;
	tls_size = 0;

	/* Handles the object availability. */
	if (object == NULL || !object->active || object->permanent ||
	    object->unloading || object->direct_refs != 0 ||
	    object->dependency_refs != 0)

		/* Returns the computed result. */
		return;
	object->unloading = 1;
	remove_initialization_record(object);

	/* Process each remaining element. */
	dependency_count = object->needed_count;
	for (i = 0; i < dependency_count; i++)
		dependencies[i] = object->needed[i];

	/* Application callbacks may recursively use the loader. */
	loader_unlock();
	finalize_object_unlocked(object);
	loader_lock();

	/* Checks the current object. */
	if (object->tls_module_id != 0) {
		/* Process each element required by the operation. */
		module = &tls_modules[object->tls_module_id];
		tls_size = module->memory_size;
		for (tcb = rtld_threads; tcb != NULL; tcb = tcb->rtld_next) {
			/* Handles the dtv availability. */
			if (tcb->dtv != NULL &&
			    object->tls_module_id < tcb->dtv_count &&
			    tcb->dtv[object->tls_module_id] != NULL) {
				tls_unmap(tcb->dtv[object->tls_module_id],
					  tls_size);
				tcb->dtv[object->tls_module_id] = NULL;
				tcb->dtv_generation = tls_generation + 1U;
			}
		}
		module->active = 0;
		module->owner = NULL;
		module->init_image = NULL;
		tls_generation++;
	}

	/* Process each remaining element. */
	for (i = object->mapping_count; i != 0; i--) {
		result = syscall6(ZEDBSD_SYS_munmap, object->mapping_start[i - 1U],
	     object->mapping_size[i - 1U], 0, 0, 0, 0);

		/* Handles an operation failure. */
		if (raw_error(result))
			rtld_fatal("cannot unmap shared object");
	}

	/* Process each remaining element. */
	for (i = 0; i < dependency_count; i++) {
		/* Handles the dependencies condition. */
		if (dependencies[i] == NULL ||
		    dependencies[i]->dependency_refs == 0) {
			rtld_fatal(
			    "invalid shared-object dependency reference");
		}
		dependencies[i]->dependency_refs--;
	}
	rtld_memset(object, 0, sizeof(*object));

	/* Process each remaining element. */
	for (i = 0; i < dependency_count; i++)
		unload_object_locked(dependencies[i]);
}

/* Supports the remove initialization record operation. */
static void
remove_initialization_record(
	struct rtld_object *object)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < initialization_count; i++) {
		/* Handles the initialization order condition. */
		if (initialization_order[i] == object) {
			/* Process each remaining element. */
			for (; i + 1U < initialization_count; i++) {
				initialization_order[i] =
				    initialization_order[i + 1U];
			}
			initialization_order[--initialization_count] = NULL;
			break;
		}
	}
}

/* Supports the finalize object unlocked operation. */
static void
finalize_object_unlocked(
	struct rtld_object *object)
{
	size_t i;

	/* Checks the current object. */
	if (!object->initialized)
		return;

	/* Continue while the operation condition remains true. */
	object->initialized = 0;
	i = object->fini_count;
	while (i != 0) {
		i--;

		/* Checks the current object. */
		if (object->fini_array[i] != 0)
			((void (*)(void))object->fini_array[i])();
	}

	/* Checks the current object. */
	if (object->fini != 0)
		((void (*)(void))object->fini)();
}

/* Supports the rtld dlsym common operation. */
static void *
rtld_dlsym_common(
	void *value,
	const char *name,
	const char *version)
{
	struct rtld_handle *handle;
	uintptr_t result;
	uint32_t visited;
	int found;

	result = 0;
	visited = 0;
	found = 0;

	clear_loader_error();

	/* Handles a failed reserved loader symbol operation. */
	if (name == NULL || name[0] == '\0' || reserved_loader_symbol(name)) {
		set_loader_error("invalid symbol name");

		/* Reports that no result is available. */
		return NULL;
	}
	loader_lock();
	handle = validate_handle(value);

	/* Handles the handle availability. */
	if (handle == NULL) {
		set_loader_error("invalid dynamic-loader handle");
	} else if (handle->main_scope) {
		result = lookup_global_optional(name, version, &found);
	} else {
		result = lookup_handle_graph(handle->object, name, version,
					     &visited, &found);
	}

	/* Handles the handle availability. */
	if (handle != NULL && !found)
		set_loader_error("symbol not found");
	loader_unlock();

	/* Returns the computed result. */
	return found ? (void *)result : NULL;
}

/* Supports the validate handle operation. */
static struct rtld_handle *
validate_handle(
	void *value)
{
	uintptr_t address;
	uintptr_t first;
	uintptr_t end;
	struct rtld_handle *handle;

	address = (uintptr_t)value;
	first = (uintptr_t)&handles[0];
	end = (uintptr_t)&handles[RTLD_HANDLE_MAX];

	/* Handles the address condition. */
	if (address < first || address >= end ||
	    (address - first) % sizeof(handles[0]) != 0)

		/* Reports that no result is available. */
		return NULL;
	handle = (struct rtld_handle *)value;

	/* Handles the object availability. */
	if (handle->magic != RTLD_HANDLE_MAGIC || !handle->active ||
	    handle->references == 0 || handle->generation == 0 ||
	    handle->object == NULL || !handle->object->active ||
	    handle->object->unloading)

		/* Reports that no result is available. */
		return NULL;

	/* Returns the computed result. */
	return handle;
}

/* Supports the lookup global optional operation. */
static uintptr_t
lookup_global_optional(
	const char *name,
	const char *version,
	int *found)
{
	uintptr_t function_result;
	struct rtld_object *object;
	Elf_Sym *symbol;
	unsigned i;

	*found = 0;
	symbol = lookup_in_object_version(main_object, name, version);

	/* Handles the symbol availability. */
	if (symbol != NULL) {
		*found = 1;
		/* Obtains the symbol value result. */
		function_result = symbol_value(main_object, symbol);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		object = &objects[i];

		/* Checks the current object. */
		if (!object->active || object->unloading ||
		    object == main_object || object == interpreter_object)
			continue;
		symbol = lookup_in_object_version(object, name, version);

		/* Handles the symbol availability. */
		if (symbol != NULL) {
			*found = 1;
			/* Obtains the symbol value result. */
			function_result = symbol_value(object, symbol);

			/* Returns the computed result. */
			return function_result;
		}
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the lookup handle graph operation. */
static uintptr_t
lookup_handle_graph(
	struct rtld_object *object,
	const char *name,
	const char *version,
	uint32_t *visited,
	int *found)
{
	uintptr_t function_result;
	uintptr_t value;
	uintptr_t index;
	Elf_Sym *symbol;
	unsigned i;

	/* Checks the current object. */
	if (object < &objects[0] || object >= &objects[RTLD_OBJECT_MAX] ||
	    !object->active || object->unloading)

		/* Reports successful completion. */
		return 0;
	index = (uintptr_t)(object - &objects[0]);

	/* Handles the visited condition. */
	if ((*visited & ((uint32_t)1U << index)) != 0)
		return 0;
	*visited |= (uint32_t)1U << index;
	symbol = lookup_in_object_version(object, name, version);

	/* Handles the symbol availability. */
	if (symbol != NULL) {
		*found = 1;
		/* Obtains the symbol value result. */
		function_result = symbol_value(object, symbol);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (i = 0; i < object->needed_count; i++) {
		value = lookup_handle_graph(object->needed[i], name,
				      version, visited, found);

		/* Handles the found condition. */
		if (*found)
			return value;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the bootstrap relative operation. */
static int
bootstrap_relative(
	uintptr_t base,
	const Elf_Phdr *phdr,
	unsigned phnum)
{
	Elf_Dyn *dynamic;
	size_t dynamic_count, i;
	Elf_Rel *rel;
	size_t relsz, relent;
	Elf_Rela *rela;
	size_t relasz, relaent;
	uintptr_t *where;

	dynamic = NULL;
	dynamic_count = 0;
	rel = NULL;
	relsz = 0;
	relent = sizeof(Elf_Rel);
	rela = NULL;
	relasz = 0;
	relaent = sizeof(Elf_Rela);

	/* Process each element required by the operation. */
	for (i = 0; i < phnum; i++) {
		/* Handles the phdr condition. */
		if (phdr[i].p_type == PT_DYNAMIC) {
			/* Handles the dynamic availability. */
			if (dynamic != NULL ||
			    phdr[i].p_memsz < sizeof(Elf_Dyn))

				/* Reports operation failure. */
				return -1;
			dynamic =
			    (Elf_Dyn *)(base + (uintptr_t)phdr[i].p_vaddr);
			dynamic_count =
			    (size_t)phdr[i].p_memsz / sizeof(Elf_Dyn);
		}
	}

	/* Handles the dynamic availability. */
	if (dynamic == NULL)
		return -1;

	/* Process each remaining element. */
	for (i = 0; i < dynamic_count; i++) {
		/* Handles the dynamic condition. */
		if (dynamic[i].d_tag == DT_NULL)
			break;

		/* Dispatch the selected operation case. */
		switch ((int)dynamic[i].d_tag) {
		case DT_REL:
			rel = (Elf_Rel *)(base +
					  (uintptr_t)dynamic[i].d_un.d_ptr);
			break;
		case DT_RELSZ:
			relsz = (size_t)dynamic[i].d_un.d_val;
			break;
		case DT_RELENT:
			relent = (size_t)dynamic[i].d_un.d_val;
			break;
		case DT_RELA:
			rela = (Elf_Rela *)(base +
					    (uintptr_t)dynamic[i].d_un.d_ptr);
			break;
		case DT_RELASZ:
			relasz = (size_t)dynamic[i].d_un.d_val;
			break;
		case DT_RELAENT:
			relaent = (size_t)dynamic[i].d_un.d_val;
			break;
		default:
			break;
		}
	}

	/* Handles the rel availability. */
	if (i == dynamic_count || (rel != NULL && relent != sizeof(Elf_Rel)) ||
	    (rela != NULL && relaent != sizeof(Elf_Rela)) ||
	    relsz % sizeof(Elf_Rel) != 0 || relasz % sizeof(Elf_Rela) != 0)

		/* Reports operation failure. */
		return -1;

	/* Process each remaining element. */
	for (i = 0; i < relsz / sizeof(Elf_Rel); i++) {
		/* Handles a failed ELF R TYPE operation. */
		if (ELF_R_TYPE(rel[i].r_info) != RTLD_RELATIVE ||
		    ELF_R_SYM(rel[i].r_info) != 0)

			/* Reports operation failure. */
			return -1;
		where = (uintptr_t *)(base + (uintptr_t)rel[i].r_offset);
		*where += base;
	}

	/* Process each remaining element. */
	for (i = 0; i < relasz / sizeof(Elf_Rela); i++) {
		/* Handles a failed ELF R TYPE operation. */
		if (ELF_R_TYPE(rela[i].r_info) != RTLD_RELATIVE ||
		    ELF_R_SYM(rela[i].r_info) != 0)

			/* Reports operation failure. */
			return -1;
		where = (uintptr_t *)(base + (uintptr_t)rela[i].r_offset);
		*where = base + (uintptr_t)rela[i].r_addend;
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the setup premapped object operation. */
static void
setup_premapped_object(
	struct rtld_object *object,
	uintptr_t base,
	const Elf_Phdr *phdr,
	unsigned phnum,
	int type)
{
	unsigned i;

	object->base = base;
	object->type = type;
	object->phnum = phnum;

	/* Handles the phnum condition. */
	if (phnum == 0 || phnum > 64)
		rtld_fatal("invalid pre-mapped program headers");

	/* Process each element required by the operation. */
	for (i = 0; i < phnum; i++)
		object->phdr[i] = phdr[i];
	parse_dynamic(object);
}
