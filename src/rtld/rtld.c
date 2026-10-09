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

#include "src/rtld/rtld.h"

#include <link.h>
#include <uapi/auxv.h>
#include <rtld-abi.h>
#include <uapi/syscall.h>
#include <uapi/thread.h>
#include <uapi/usync.h>
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

/*
 * A page of TLSDESC arguments an object needed beyond the ones it holds
 * itself (WS140): each argument's address is written into a descriptor,
 * so a page is never moved, only linked after the last.  The pages go
 * with the object.
 */
#define RTLD_TLSDESC_CHUNK \
	((RTLD_PAGE_SIZE - 2U * sizeof(void *)) / sizeof(struct __tls_index))
struct rtld_tlsdesc_chunk {
	struct rtld_tlsdesc_chunk *next;
	uintptr_t used;
	struct __tls_index index[RTLD_TLSDESC_CHUNK];
};

/*
 * One loaded object.
 *
 * Its program headers, and the mappings its segments were given (two a
 * load segment at most: the file and its zero fill), are kept in the
 * object when there are at most RTLD_PROGRAM_INLINE headers, and in a
 * program table otherwise (program_table).
 * Its dependencies likewise: RTLD_NEEDED_INLINE inside, else a mapping
 * (needed_mapping).  Its TLSDESC arguments: RTLD_TLSDESC_INLINE inside,
 * then pages linked from tlsdesc_chunks.  init_prev and init_next place it
 * in the list of initialized objects, and lookup_mark records the last
 * dlsym() walk that visited it; the loader lock guards both.
 */
struct rtld_object {
	char path[RTLD_PATH_MAX];
	uintptr_t base;
	int type;
	Elf_Phdr phdr_inline[RTLD_PROGRAM_INLINE];
	Elf_Phdr *phdr;
	unsigned phnum;
	struct rtld_program_table *program_table;
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
	uint32_t needed_offset_inline[RTLD_NEEDED_INLINE];
	struct rtld_object *needed_inline[RTLD_NEEDED_INLINE];
	uint32_t *needed_offset;
	struct rtld_object **needed;
	void *needed_mapping;
	size_t needed_mapping_size;
	unsigned needed_count;
	const char *rpath;
	const char *runpath;
	struct rtld_object *loader_parent;
	unsigned relative_done;
	/* The writable load segment the last relocation target fell in. */
	uintptr_t relocation_window_start;
	uintptr_t relocation_window_end;
	unsigned relocating;
	unsigned relocated;
	unsigned initializing;
	unsigned initialized;
	dev_t device;
	ino_t inode;
	unsigned has_identity;

	/* This object's place in the list a debugger reads. */
	struct link_map map;
	uintptr_t mapping_start_inline[2U * RTLD_PROGRAM_INLINE];
	size_t mapping_size_inline[2U * RTLD_PROGRAM_INLINE];
	uintptr_t *mapping_start;
	size_t *mapping_size;
	unsigned mapping_capacity;
	unsigned mapping_count;
	uintptr_t tls_module_id;
	unsigned active;
	unsigned unloading;
	unsigned permanent;
	unsigned direct_refs;
	unsigned dependency_refs;
	uint32_t generation;
	struct __tls_index tlsdesc_inline[RTLD_TLSDESC_INLINE];
	struct rtld_tlsdesc_chunk *tlsdesc_chunks;
	struct rtld_tlsdesc_chunk *tlsdesc_last;
	unsigned tlsdesc_count;
	struct rtld_object *init_prev;
	struct rtld_object *init_next;
	uint32_t lookup_mark;
};

/*
 * A chunk of the object table (WS140): the first is static, the others
 * are mapped when the objects before them are all in use.  A chunk is
 * never released before the process ends, so an object's address never
 * changes, and the readers that take no lock (dl_iterate_phdr) may walk
 * the chain while it grows.
 */
struct rtld_object_chunk {
	struct rtld_object_chunk *next;
	struct rtld_object slots[RTLD_OBJECT_CHUNK];
};

/*
 * What an object holds outside itself, taken before its slot is cleared
 * and released afterwards (unload_object_locked): its dependency table,
 * its program table and its TLSDESC pages.
 */
struct rtld_object_tables {
	void *needed_mapping;
	size_t needed_mapping_size;
	struct rtld_program_table *program_table;
	struct rtld_tlsdesc_chunk *tlsdesc_chunks;
};

/*
 * The head of an object's program table (object_set_programs), before its
 * headers and mapping records.  dl_iterate_phdr reads the headers without
 * the loader lock, so a table is never unmapped: once its object goes it
 * is kept on a list for a later object to use again.
 */
struct rtld_program_table {
	struct rtld_program_table *next;
	size_t size;
};

struct rtld_tlsdesc {
	uintptr_t resolver;
	uintptr_t argument;
};

/*
 * A name being looked up, with its two hashes computed once rather than
 * once for every object the search visits.
 */
struct rtld_symbol_name {
	const char *name;
	uint32_t gnu_hash;
	uint32_t elf_hash;
	unsigned hashed;
};

#define RTLD_HANDLE_CHUNK 64U
#define RTLD_HANDLE_MAGIC 0x5a444c48U

struct rtld_handle {
	uint32_t magic;
	uint32_t generation;
	struct rtld_object *object;
	unsigned references;
	unsigned active;
	unsigned main_scope;
};

/*
 * A chunk of the handle table (WS140): the first is static, the others are
 * mapped when every handle before them is in use.  A chunk is never
 * released, so a closed handle stays readable for validate_handle to
 * refuse.  Handles are used under the loader lock only.
 */
struct rtld_handle_chunk {
	struct rtld_handle_chunk *next;
	struct rtld_handle slots[RTLD_HANDLE_CHUNK];
};

/* The TLS modules of a chunk; the first chunk's slot 0 is never used (id 0). */
#define RTLD_TLS_CHUNK 33U

/* The entries a thread's first TLS vector has room for. */
#define RTLD_DTV_INITIAL RTLD_TLS_CHUNK

struct rtld_tls_module {
	uintptr_t id;
	const void *init_image;
	size_t file_size;
	size_t memory_size;
	size_t alignment;
	struct rtld_object *owner;
	unsigned active;
	/* Distance below the thread pointer, zero for a dynamic module. */
	size_t static_offset;
	unsigned is_static;
};

/*
 * A chunk of the TLS module table (WS140): module id N is slot N % 33 of
 * chunk N / 33.  The first is static, the others are mapped as ids grow,
 * and none is released, so __tls_get_addr, which takes no lock, may walk
 * the chain while it grows.
 */
struct rtld_tls_chunk {
	struct rtld_tls_chunk *next;
	struct rtld_tls_module slots[RTLD_TLS_CHUNK];
};

/*
 * The object table: the static first chunk and the last one linked.
 * object_count is how many slots have ever been used.  The chain and the
 * count grow at startup or under the loader lock; the count is published
 * with release order after its chunk is linked, and a reader without the
 * lock reads it with acquire order before it walks the chain.
 */
static struct rtld_object_chunk object_chunk_first;
static struct rtld_object_chunk *object_chunk_last = &object_chunk_first;
static unsigned object_count;
/* How many objects have been added and removed over the process's life. */
static unsigned long long rtld_object_generation;
static unsigned long long rtld_object_removals;
static struct rtld_object *main_object;
static struct rtld_object *interpreter_object;
/*
 * The initialized objects, oldest first: the order their finalizers run
 * in backwards.  Changed under the loader lock only.
 */
static struct rtld_object *initialization_head;
static struct rtld_object *initialization_tail;
/* The number of the last dlsym() walk (lookup_mark); under the loader lock only. */
static uint32_t lookup_generation;
/* Program tables of objects gone, for reuse; under the loader lock only. */
static struct rtld_program_table *program_tables_free;
static unsigned startup_initialized;
static unsigned process_finalized;
/* The handle table: the static first chunk and the last one linked. */
static struct rtld_handle_chunk handle_chunk_first;
static struct rtld_handle_chunk *handle_chunk_last = &handle_chunk_first;
static uint32_t next_handle_generation = 1;
static volatile uint32_t loader_lock_word;
static uintptr_t loader_lock_owner;
static unsigned loader_lock_depth;
static char loader_error[KERN_RTLD_DLERROR_SIZE];
static unsigned loader_error_pending;
/*
 * The TLS module table: the static first chunk and the last one linked.
 * tls_module_count is the highest id ever given; it grows under the loader
 * lock, with release order after the module is written, and
 * __tls_get_addr reads it with acquire order.
 */
static struct rtld_tls_chunk tls_chunk_first;
static struct rtld_tls_chunk *tls_chunk_last = &tls_chunk_first;
static uintptr_t tls_module_count;
static uint64_t tls_generation;

/*
 * The static TLS area.
 *
 * A linker turns a main executable's access to its own thread-local storage
 * into a fixed displacement below the thread pointer, and it does so whatever
 * model the compiler asked for, because an executable is never loaded at a
 * second address.  Nothing relocates such an access, so the program only
 * works if its TLS block really is there.  The executable and the objects
 * loaded alongside it are therefore given one contiguous block under the
 * thread pointer, laid out once before any relocation is processed.  An
 * object that arrives later through dlopen() cannot join that block and keeps
 * a separately allocated one reached through __tls_get_addr.
 */
static uintptr_t static_tls_distance;
static void *static_tls_template;
static size_t static_tls_template_size;
static size_t static_tls_alignment = 1;
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_I386)
static unsigned static_tls_sealed;
#endif
static struct __rtld_tcb *rtld_threads;
static uint32_t next_object_generation = 1;

/*
 * What a debugger reads to find the loaded objects.
 *
 * A debugger stops a program it did not load, so it has no list of its
 * own.  It finds this structure through the DT_DEBUG entry of the
 * executable, which is filled in below, and walks the objects from it.
 *
 * The list is published only while r_state says it is consistent, and the
 * loader calls the function r_brk names before and after it changes
 * anything.  A debugger plants a breakpoint there to be told.
 */
__attribute__((visibility("default")))
struct r_debug _r_debug = { 1, NULL, 0, RT_CONSISTENT, 0 };

static struct link_map *debug_map_tail;

/* The object table's helpers (WS140), used from the debugger interface on. */
static struct rtld_object *object_at(unsigned index);
static void object_chunk_grow(void);
static struct rtld_program_table *program_table_take(size_t size);
static void object_set_programs(struct rtld_object *object, const Elf_Phdr *phdr, unsigned phnum);
static Elf_Phdr *read_program_headers(int fd, const Elf_Ehdr *header, Elf_Phdr *room, unsigned room_count, size_t *mapping_size);
static struct __tls_index *tlsdesc_slot(struct rtld_object *object);
static void object_tables_take(const struct rtld_object *object, struct rtld_object_tables *tables);
static void object_tables_release(const struct rtld_object_tables *tables);
static void object_reserve_needed(struct rtld_object *object);
static void object_clear(struct rtld_object *object);

/*
 * The function a debugger stops on.  It does nothing: being called is the
 * whole of what it says.  It is kept from being optimised away or folded
 * with another empty function, because its address is the interface.
 */
__attribute__((visibility("default"), noinline))
void
_rtld_debug_state(void)
{
	__asm__ volatile("" ::: "memory");
}

/* Announces that the list is about to change, and then that it has. */
static void
debug_state_change(
	int state)
{
	_r_debug.r_state = state;
	_rtld_debug_state();
}

/* Adds one object to the end of the list a debugger walks. */
static void
debug_map_add(
	struct rtld_object *object)
{
	struct link_map *map;

	map = &object->map;
	map->l_addr = (ElfW_Addr)object->base;
	map->l_name = object->path;
	map->l_ld = (ElfW_Dyn *)object->dynamic;
	map->l_next = NULL;
	map->l_prev = debug_map_tail;

	/* Handles the first object, which the structure itself names. */
	if (debug_map_tail == NULL)
		_r_debug.r_map = map;
	else
		debug_map_tail->l_next = map;
	debug_map_tail = map;
}

/*
 * Adds every object a load brought in that is not in the list yet.  One
 * dlopen() may bring in a whole closure of dependencies, and a debugger
 * is told about all of them at once rather than one at a time.
 */
static void
debug_map_publish(
	void)
{
	struct rtld_object *object;
	unsigned index;
	int added;

	added = 0;

	/* Process each remaining element. */
	for (index = 0; index < object_count; index++) {
		/* Only an object in the process now that is not published yet. */
		object = object_at(index);
		if (!object->active || object->unloading || object->map.l_name != NULL)
			continue;

		/* Announces the change once, before the first addition. */
		if (!added) {
			debug_state_change(RT_ADD);
			added = 1;
		}
		debug_map_add(object);
	}

	/* Handles the addition condition. */
	if (added)
		debug_state_change(RT_CONSISTENT);
}

/* Removes one object from the list a debugger walks. */
static void
debug_map_remove(
	struct rtld_object *object)
{
	struct link_map *map;

	map = &object->map;

	/* Handles an object that was never published. */
	if (map->l_prev == NULL && _r_debug.r_map != map)
		return;
	if (map->l_prev != NULL)
		map->l_prev->l_next = map->l_next;
	else
		_r_debug.r_map = map->l_next;
	if (map->l_next != NULL)
		map->l_next->l_prev = map->l_prev;
	else
		debug_map_tail = map->l_prev;
	map->l_next = NULL;
	map->l_prev = NULL;
}

__attribute__((visibility("default")))
const struct __rtld_exports __rtld_exports = {
    .abi_version = KERN_RTLD_ABI_VERSION,
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
    .dl_iterate_phdr = __rtld_dl_iterate_phdr,
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
static uintptr_t thread_pointer(void);
static void layout_static_tls(void);
#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_I386)
static uintptr_t static_tls_place(struct rtld_tls_module *module, uintptr_t offset);
#endif
static struct rtld_tls_module *tls_module_at(uintptr_t id);
static uintptr_t tls_module_new_id(void);
static void dtv_grow(struct __rtld_tcb *tcb, uintptr_t module);
#if defined(HAL_ARCH_AMD64)
static uintptr_t static_tls_displacement(const struct rtld_object *owner);
#endif
static void initialize_object(struct rtld_object *object);
static void clear_loader_error(void);
static void set_loader_error(const char *message);
static struct rtld_handle *allocate_handle(struct rtld_object *object, int main_scope);
static struct rtld_handle *handle_free_slot(void);
static const char *dlopen_bare_name(const char *path);
static int dlopen_names_file(const char *path);
static const char *object_basename(const char *path);
static int preflight_dlopen_file(int fd);
static int valid_elf_header(const Elf_Ehdr *header, int expected_type);
static int validate_file_programs(const Elf_Ehdr *header, const Elf_Phdr *phdr, off_t file_size);
static int temporary_writable_plt(const Elf_Phdr *program);
static uintptr_t page_floor(uintptr_t value);
static struct rtld_object *load_object(const char *name, struct rtld_object *requester);
static struct rtld_object *load_object_path(const char *file_path);
static struct rtld_object *load_object_file(intptr_t fd, const char path[RTLD_PATH_MAX], struct rtld_object *requester);
static intptr_t open_dependency(const char *name, size_t name_length, const struct rtld_object *requester, char path[RTLD_PATH_MAX]);
static intptr_t open_search_list(const char *list, const struct rtld_object *owner, const char *name, size_t name_length, char path[RTLD_PATH_MAX]);
static intptr_t open_search_candidate(const char *directory, size_t directory_length, const char *name, size_t name_length, char path[RTLD_PATH_MAX]);
static struct rtld_object *find_identity(const struct stat *status);
static struct rtld_object *new_object(const char *path);
static void copy_path(char destination[RTLD_PATH_MAX], const char *source);
static void map_one_segment(struct rtld_object *object, int fd, const Elf_Phdr *program);
static void object_reserve_span(struct rtld_object *object, uintptr_t low);
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
/*
 * Supports the environment lookup operation.
 *
 * Returns the value of one variable, or NULL when it is absent or empty.
 * The loader runs before the C library, so it reads the vector itself.
 */
static const char *
find_environment(
	char **environment,
	const char *name)
{
	size_t length;
	char **entry;

	/* Handles the environment availability. */
	if (environment == NULL)
		return NULL;
	length = rtld_strlen(name);

	/* Process each remaining element. */
	for (entry = environment; *entry != NULL; entry++) {
		const char *text = *entry;
		size_t index;

		/* Compares the name up to its separator. */
		for (index = 0; index < length; index++) {
			if (text[index] != name[index])
				break;
		}
		if (index != length || text[length] != '=')
			continue;

		/* An empty value is the same as an absent one. */
		if (text[length + 1U] == '\0')
			return NULL;

		/* Returns the computed result. */
		return &text[length + 1U];
	}

	/* Reports that no result is available. */
	return NULL;
}

/*
 * The directories LD_LIBRARY_PATH named, or NULL when the variable was
 * absent, empty, or withheld because the program is running with privileges
 * its invoker does not have.
 */
static const char *library_path;

static void relocate_object(struct rtld_object *object);
static void apply_value(struct rtld_object *object, uintptr_t offset, uint32_t type, uint32_t symbol_index, uintptr_t addend, int is_rela);
static uintptr_t resolve_relocation_symbol(struct rtld_object *object, uint32_t index);
static uintptr_t symbol_value(struct rtld_object *object, const Elf_Sym *symbol);
static uintptr_t lookup_symbol_version(const char *name, const char *required_version, int weak);
static int reserved_loader_symbol(const char *name);
static Elf_Sym *lookup_in_object_version(struct rtld_object *object, const char *name, const char *required_version);
static Elf_Sym *lookup_in_object_hashed(struct rtld_object *object, struct rtld_symbol_name *symbol_name, const char *required_version);
static Elf_Sym *lookup_gnu_hash(struct rtld_object *object, struct rtld_symbol_name *symbol_name, const char *required_version);
static int relocation_target_writable(struct rtld_object *object, uintptr_t address, size_t size);
static void hash_symbol_name(struct rtld_symbol_name *symbol_name);
static Elf_Sym *match_symbol(struct rtld_object *object, uint32_t index, const char *name, const char *required_version);
static int symbol_version_matches(struct rtld_object *object, uint32_t symbol_index, const char *required_version);
static const char *defined_version_name(struct rtld_object *object, uint16_t version_index);
static const char *relocation_version_name(struct rtld_object *object, uint32_t symbol_index);
static const char *required_version_name(struct rtld_object *object, uint16_t version_index);
static Elf_Sym *resolve_tls_symbol(struct rtld_object *object, uint32_t index, struct rtld_object **owner);
static void unload_object_locked(struct rtld_object *object);
static void remove_initialization_record(struct rtld_object *object);
static void finalize_object_unlocked(struct rtld_object *object);
static void lookup_generation_next(void);
static void *rtld_dlsym_common(void *value, const char *name, const char *version);
static struct rtld_handle *validate_handle(void *value);
static uintptr_t lookup_global_optional(const char *name, const char *version, int *found);
static uintptr_t lookup_handle_graph(struct rtld_object *object, const char *name, const char *version, int *found);
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
		(void)syscall6(KERN_SYS_write, 2, (uintptr_t)message,
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
	(void)syscall6(KERN_SYS_exit, 127, 0, 0, 0, 0, 0);

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
	return KERN_RTLD_ABI_VERSION;
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
	unsigned char *mapping;
	void **dtv;
	size_t dtv_count;
	size_t payload;
	size_t size;

	/* Handles the out availability. */
	if (out == NULL)
		return -1;
	*out = NULL;

	/*
	 * One mapping holds the static area and the control block, so that
	 * the thread pointer stays a page boundary and the whole thread is
	 * released in a single unmap.
	 */
	payload = (size_t)page_ceil(static_tls_distance);
	size = payload + KERN_TLS_TCB_RESERVE;
	mapping = tls_map(size);

	/* Handles the mapping availability. */
	if (mapping == NULL)
		return -1;
	tcb = (struct __rtld_tcb *)(mapping + payload);

	/*
	 * The TLS vector: room for every module there is now, at least
	 * RTLD_DTV_INITIAL, and two entries past the end for the chain of the
	 * vectors it replaces (WS140 D6), which starts empty.  A module
	 * registered after this is read makes __tls_get_addr grow it.
	 */
	dtv_count = __atomic_load_n(&tls_module_count, __ATOMIC_ACQUIRE) + 1U;
	if (dtv_count < RTLD_DTV_INITIAL)
		dtv_count = RTLD_DTV_INITIAL;

	/* Maps it (zeroed, so the chain's two entries start empty). */
	dtv = tls_map((dtv_count + 2U) * sizeof(*dtv));

	/* Handles the dtv availability. */
	if (dtv == NULL) {
		tls_unmap(mapping, size);

		/* Reports operation failure. */
		return -1;
	}

	/* Anonymous memory leaves .tbss zeroed; only the template is copied. */
	if (static_tls_template_size != 0) {
		rtld_memcpy((unsigned char *)tcb - static_tls_distance,
			    static_tls_template, static_tls_template_size);
	}

	/* The control block: the static area, the TLS vector and the thread's record. */
	rtld_memset(tcb, 0, sizeof(*tcb));
	tcb->tls.self = (uintptr_t)tcb;
	tcb->tls.mapping_base = (uintptr_t)mapping;
	tcb->tls.mapping_size = size;
	tcb->tls.template_address = (uintptr_t)static_tls_template;
	tcb->tls.template_size = static_tls_template_size;
	tcb->tls.memory_size = static_tls_distance;
	tcb->tls.alignment = static_tls_alignment;
	tcb->tls.distance = static_tls_distance;
	rtld_memset(dtv, 0, (dtv_count + 2U) * sizeof(*dtv));
	tcb->dtv = dtv;
	tcb->dtv_count = dtv_count;
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
	uintptr_t count;
	struct __rtld_tcb **link;
	struct rtld_tls_module *module;
	void **dtv;
	void **retired;
	size_t dtv_count;
	size_t retired_count;

	/* Handles the tcb availability. */
	if (tcb == NULL)
		return;
	current = syscall6(KERN_SYS_thread_self, KERN_THREAD_SELF_GET_TLS,
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

	/* Frees the thread's dynamic blocks, which its current vector holds. */
	count = __atomic_load_n(&tls_module_count, __ATOMIC_ACQUIRE);
	if (tcb->dtv != NULL) {
		for (id = 1; id < tcb->dtv_count && id <= count; id++) {
			/* A block this thread allocated. */
			if (tcb->dtv[id] != NULL) {
				module = tls_module_at(id);
				tls_unmap(tcb->dtv[id], module->memory_size);
			}
		}
	}

	/* Frees the vector, then each one it replaced, along their chain. */
	dtv = tcb->dtv;
	dtv_count = tcb->dtv_count;
	while (dtv != NULL) {
		/* The link to the one before, read before this one goes. */
		retired = (void **)dtv[dtv_count];
		retired_count = (size_t)(uintptr_t)dtv[dtv_count + 1U];
		tls_unmap(dtv, (dtv_count + 2U) * sizeof(*dtv));
		dtv = retired;
		dtv_count = retired_count;
	}
	tcb->dtv = NULL;

	/* The control block sits inside the mapping it records, not at it. */
	tls_unmap((void *)tcb->tls.mapping_base, tcb->tls.mapping_size);
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

	value = syscall6(KERN_SYS_thread_self,
				  KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(value))
		return -1;

	/* Validates the current value. */
	if (value == 0) {
		/* Handles a failed rtld thread alloc operation. */
		if (__rtld_thread_alloc(pthread_private, &tcb) != 0)
			return -1;
		value =
		    syscall6(KERN_SYS_thread_self, KERN_THREAD_SELF_SET_TLS,
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
	uintptr_t value;

	/* The calling thread's control block; a thread without one has no pthread record. */
	value = thread_pointer();
	if (value == 0)
		return NULL;

	/* Returns the computed result. */
	return ((struct __rtld_tcb *)value)->pthread_private;
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
	uintptr_t count;
	unsigned active;
	void *block;

	/* Refuses an id no module was given; the count is read before the chain. */
	count = __atomic_load_n(&tls_module_count, __ATOMIC_ACQUIRE);
	if (index == NULL || index->module == 0 || index->module > count)
		rtld_fatal("invalid TLS index");

	/* The calling thread's control block, read without a system call where the processor holds it (BUG-110). */
	value = (intptr_t)thread_pointer();
	if (value == 0)
		rtld_fatal("thread has no TLS control block");
	tcb = (struct __rtld_tcb *)(uintptr_t)value;

	/* The module, written before it was marked active. */
	module = tls_module_at(index->module);
	active = __atomic_load_n(&module->active, __ATOMIC_ACQUIRE);
	if (!active || index->offset >= module->memory_size)
		rtld_fatal("invalid TLS module access");

	/* A module in the static area is already present in every thread. */
	if (module->is_static) {
		/* Returns the computed result. */
		return (unsigned char *)tcb - module->static_offset +
		       index->offset;
	}

	/* A thread without a vector cannot reach a dynamic module. */
	if (tcb->dtv == NULL)
		rtld_fatal("invalid TLS module access");

	/* A module registered after the vector was sized: it grows (WS140 D6). */
	if (index->module >= tcb->dtv_count)
		dtv_grow(tcb, index->module);

	/* The thread's block of the module. */
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

/*
 * Replaces the calling thread's TLS vector with one large enough for
 * module, at least twice the old size.  The old vector is not freed and
 * not changed: an access that a signal interrupted after reading it still
 * reads right values from it.  It is linked from the two entries past the
 * new one's end (its address, then its size) and freed with the thread
 * (__rtld_thread_free).  Only the thread itself grows its vector; the
 * loader lock orders it against unload_object_locked, which clears
 * entries of every thread's vector.
 */
static void
dtv_grow(
	struct __rtld_tcb *tcb,
	uintptr_t module)
{
	void **dtv;
	size_t count;
	size_t i;

	/* Copies the vector into a larger one and publishes it. */
	loader_lock();

	/* Grown already on another path: nothing to do. */
	if (module < tcb->dtv_count) {
		loader_unlock();
		return;
	}

	/* Twice as large, or large enough for the module. */
	count = tcb->dtv_count * 2U;
	if (count < module + 1U)
		count = module + 1U;

	/* A size whose bytes, with the chain's two entries, fit a size_t. */
	if (count > SIZE_MAX / sizeof(*dtv) - 2U)
		rtld_fatal("TLS vector is too large");

	/* The new vector; without memory the process cannot go on (WS140 U2). */
	dtv = tls_map((count + 2U) * sizeof(*dtv));
	if (dtv == NULL)
		rtld_fatal("cannot allocate TLS vector");

	/* The thread's blocks, then the link to the old vector. */
	for (i = 1; i < tcb->dtv_count; i++)
		dtv[i] = tcb->dtv[i];

	/* The link to the old vector, past the new one's entries. */
	dtv[count] = (void *)tcb->dtv;
	dtv[count + 1U] = (void *)(uintptr_t)tcb->dtv_count;

	/* The vector first, then its size, so the size never outgrows it. */
	__atomic_store_n(&tcb->dtv, dtv, __ATOMIC_RELEASE);
	__atomic_store_n(&tcb->dtv_count, count, __ATOMIC_RELEASE);

	loader_unlock();
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
	intptr_t base;
	void *address;

	/* Handles the descriptor availability. */
	if (descriptor == NULL || descriptor->argument == 0)
		rtld_fatal("invalid TLSDESC argument");
	index = (const struct __tls_index *)descriptor->argument;
	address = __tls_get_addr(index);

	/* The base the descriptor's offset is from: the calling thread's pointer. */
	base = (intptr_t)thread_pointer();
	if (base == 0)
		rtld_fatal("thread has no TLSDESC base");

	/* Returns the computed result. */
	return (uintptr_t)address - (uintptr_t)base;
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

	/* Handles the process finalized condition. */
	if (process_finalized)
		return;

	/*
	 * Finalizes in the reverse order of initialization: the last object
	 * initialized is taken off the list under the lock, then its
	 * destructors run without it, since they may call the loader (WS140
	 * D4).
	 */
	process_finalized = 1;
	for (;;) {
		/* Takes the object initialized last off the list, if any is left. */
		loader_lock();

		object = initialization_tail;
		if (object == NULL) {
			loader_unlock();
			return;
		}

		/* Off the list, so a destructor's dlclose does not find it there. */
		remove_initialization_record(object);

		loader_unlock();

		/* Runs its destructors, once. */
		finalize_object_unlocked(object);
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
	const char *loaded_name;
	char full_path[RTLD_PATH_MAX];
	intptr_t fd;
	size_t length;
	unsigned i;
	int same_path;
	int same_name;
	int names_file;

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
			set_loader_error("cannot allocate dynamic-loader handle");
		loader_unlock();

		/* Returns the computed result. */
		return handle;
	}
	/*
	 * A bare name (or /lib/ and a name) is looked for where a DT_NEEDED name
	 * is; any other path with a slash names its file, opened as it is, as
	 * POSIX has it (T1-495: Python loads its extension modules from
	 * /usr/lib/python3.14/lib-dynload by their whole paths).
	 */
	names_file = 0;
	name = dlopen_bare_name(path);
	if (name == NULL) {
		names_file = dlopen_names_file(path);
		if (names_file)
			name = path;
	}

	/* Refuses an empty path, or one too long to keep. */
	if (name == NULL || (length = rtld_strlen(name)) == 0 ||
	    length >= RTLD_PATH_MAX) {
		set_loader_error("invalid shared-object path");
		loader_unlock();

		/* Reports that no result is available. */
		return NULL;
	}

	/*
	 * An object already loaded under the path, or for a bare name under the
	 * name (from /lib or /usr/lib), is shared.  A path is not matched by its
	 * file name alone: two directories may hold different files of one name
	 * (the same file under another path is found by its identity on load).
	 */
	for (i = 0; i < object_count; i++) {
		object = object_at(i);
		loaded_name = object_basename(object->path);
		same_path = rtld_strcmp(object->path, path) == 0;
		same_name = 0;
		if (!names_file)
			same_name = rtld_strcmp(loaded_name, name) == 0;
		if (object->active && !object->unloading && (same_path || same_name))
			goto loaded;
	}

	/* The file: at its path, or found where a DT_NEEDED name is (/lib, then /usr/lib for packages; BUG-083). */
	if (names_file) {
		copy_path(full_path, path);
		fd = syscall6(KERN_SYS_open, (uintptr_t)full_path, O_RDONLY, 0, 0, 0, 0);
	} else {
		fd = open_dependency(name, length, NULL, full_path);
	}

	/* Handles an operation failure. */
	if (raw_error(fd)) {
		set_loader_error("shared object not found");
		loader_unlock();

		/* Reports that no result is available. */
		return NULL;
	}

	/* Handles a failed preflight dlopen file operation. */
	if (preflight_dlopen_file((int)fd) != 0) {
		(void)syscall6(KERN_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
		set_loader_error("invalid shared object");
		loader_unlock();

		/* Reports that no result is available. */
		return NULL;
	}
	(void)syscall6(KERN_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);

	/* Maps the object: the file at the path, or the one the name finds. */
	if (names_file) {
		object = load_object_path(full_path);
	} else {
		object = load_object(name, NULL);
	}
	relocate_object(object);
	debug_map_publish();

	/* Constructors may call dlopen recursively, so do not hold the lock. */
	loader_unlock();
	initialize_object(object);
	loader_lock();
loaded:
	handle = allocate_handle(object, 0);

	/* Handles the handle availability. */
	if (handle == NULL) {
		set_loader_error("cannot allocate dynamic-loader handle");
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
 * Implements the dl iterate phdr operation.
 *
 * Reports every loaded object to the callback, oldest first, stopping at the
 * first non-zero return and passing it back.  An unwinder uses this to find
 * the exception tables of whichever object a frame belongs to.
 */
int
__rtld_dl_iterate_phdr(
	int (*callback)(struct dl_phdr_info *, size_t, void *),
	void *argument)
{
	struct dl_phdr_info information;
	struct rtld_object *object;
	const Elf_Phdr *phdr;
	unsigned index;
	unsigned count;
	int result;

	/* Handles the callback availability. */
	if (callback == NULL)
		return 0;

	/* The objects counted now; their chunks are linked before the count is (WS140). */
	count = __atomic_load_n(&object_count, __ATOMIC_ACQUIRE);
	for (index = 0; index < count; index++) {
		object = object_at(index);

		/* Skips an object that is not part of the process now. */
		if (!object->active || object->unloading)
			continue;

		/* The headers' pointer, read once; NULL only while the slot clears. */
		phdr = __atomic_load_n(&object->phdr, __ATOMIC_RELAXED);
		if (phdr == NULL)
			continue;

		rtld_memset(&information, 0, sizeof(information));
		information.dlpi_addr = (ElfW_Addr)object->base;
		information.dlpi_name = object->path;
		information.dlpi_phdr = (const ElfW_Phdr *)(const void *)phdr;
		information.dlpi_phnum = (ElfW_Half)object->phnum;
		information.dlpi_adds = rtld_object_generation;
		information.dlpi_subs = rtld_object_removals;

		/* Stops at the first callback that asks to. */
		result = callback(&information, sizeof(information), argument);
		if (result != 0)
			return result;
	}

	/* Reports that every object was visited. */
	return 0;
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
	struct rtld_object *candidate;
	int contains;
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
		/* Skips an object that is not part of the process now. */
		candidate = object_at(i);
		if (!candidate->active || candidate->unloading)
			continue;

		/* Takes the object whose segments hold the address. */
		contains = object_contains(candidate, address, 1, 0);
		if (contains) {
			object = candidate;
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

	value = syscall6(KERN_SYS_thread_self,
				  KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
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
	char **environment;
	uintptr_t at_base, at_phdr, at_phnum, at_phent;
	uintptr_t at_entry;
	uintptr_t at_secure;
	uintptr_t at_execfn;
	const char *main_name;
	uintptr_t main_base;
	int main_type;
	Elf_Ehdr *self_header;
	Elf_Phdr *self_phdr;
	struct rtld_object *object;
	unsigned i;
	struct __rtld_tcb *initial_tcb;
	intptr_t tls_result;

	at_base = 0;
	at_phdr = 0;
	at_phnum = 0;
	at_phent = 0;
	at_entry = 0;
	at_secure = 0;
	at_execfn = 0;
	main_base = 0;
	main_type = ET_EXEC;

	/* Handles the initial stack availability. */
	if (initial_stack == NULL)
		rtld_fatal("missing initial stack");

	/* The environment follows the argument vector, ending with a zero. */
	environment = (char **)(void *)(initial_stack + 1U +
	    initial_stack[0] + 1U);
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
		case AT_SECURE:
			at_secure = auxv[1];
			break;
		case AT_EXECFN:
			at_execfn = auxv[1];
			break;
		default:
			break;
		}
	}

	/*
	 * LD_LIBRARY_PATH lets a caller name directories to look in before
	 * the system ones.  It is ignored for a program that gained
	 * privileges through its mode bits, because the caller would
	 * otherwise choose the code that runs with them.
	 */
	if (at_secure == 0)
		library_path = find_environment(environment, "LD_LIBRARY_PATH");

	/* Checks the current index. */
	if (i == 64 || at_base == 0 || at_phdr == 0 || at_phnum == 0 ||
	    at_phnum >= PN_XNUM || at_phent != sizeof(Elf_Phdr) || at_entry == 0)
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

	/*
	 * dladdr() names the file an address came from, and a program asking
	 * where its own code lives wants its own path back rather than a
	 * placeholder.  The kernel passes that path in AT_EXECFN; a program
	 * started without one keeps the placeholder, which is a name no path
	 * search can return.
	 */
	main_name = "<main>";

	/* Handles the exec file name condition. */
	if (at_execfn != 0) {
		const char *execfn = (const char *)at_execfn;

		/* Handles the exec file name length condition. */
		if (execfn[0] != '\0' && rtld_strlen(execfn) < RTLD_PATH_MAX)
			main_name = execfn;
	}
	main_object = new_object(main_name);
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
	layout_static_tls();

	/*
	 * Publishes the loaded objects for a debugger, before anything is
	 * relocated.
	 *
	 * The executable's dynamic section carries a DT_DEBUG entry, empty
	 * as the linker wrote it; filling it in with the address of the
	 * structure is how a debugger that stopped this process finds the
	 * list at all.  That entry sits inside the range relocation then
	 * seals against writing, so it is written while it still can be.
	 * The executable comes first, which is the order a debugger
	 * expects to read.
	 */
	_r_debug.r_ldbase = (ElfW_Addr)at_base;
	_r_debug.r_brk = (ElfW_Addr)(uintptr_t)_rtld_debug_state;
	debug_state_change(RT_ADD);
	debug_map_add(main_object);

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Every other object loaded at startup. */
		object = object_at(i);
		if (object->active && object != main_object)
			debug_map_add(object);
	}

	/* Process each element required by the operation. */
	for (i = 0; main_object->dynamic != NULL &&
	     i < main_object->dynamic_count; i++) {
		/* Handles the dynamic entry condition. */
		if (main_object->dynamic[i].d_tag == DT_DEBUG) {
			main_object->dynamic[i].d_un.d_ptr =
			    (Elf_Addr)(uintptr_t)&_r_debug;
			break;
		}
	}
	debug_state_change(RT_CONSISTENT);

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Relocates each object loaded at startup. */
		object = object_at(i);
		if (object->active)
			relocate_object(object);
	}

	/*
 * The initial executable, interpreter, and DT_NEEDED closure remain
	 * mapped until process termination.  Only later dlopen() objects are
	 * candidates for physical unload. */
	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* Keeps each object loaded at startup. */
		object = object_at(i);
		if (object->active)
			object->permanent = 1;
	}

	/* Handles a failed rtld thread alloc operation. */
	if (__rtld_thread_alloc(NULL, &initial_tcb) != 0)
		rtld_fatal("cannot allocate initial TLS");
	tls_result =
	    syscall6(KERN_SYS_thread_self, KERN_THREAD_SELF_SET_TLS,
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
	function_result = syscall6(KERN_SYS_mmap, address, size, (uintptr_t)prot,
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
		(void)syscall6(KERN_SYS_munmap, (uintptr_t)address,
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
		(void)syscall6(KERN_SYS_usync, (uintptr_t)&loader_lock_word,
			       KERN_USYNC_WAIT, 1, 0, 0,
			       KERN_USYNC_PRIVATE);
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

	value = syscall6(KERN_SYS_thread_self,
				  KERN_THREAD_SELF_TID, 0, 0, 0, 0, 0);

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
	(void)syscall6(KERN_SYS_usync, (uintptr_t)&loader_lock_word,
		       KERN_USYNC_WAKE, 0, 0, 1, KERN_USYNC_PRIVATE);
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

/*
 * Finds the module of an id (below or at tls_module_count): in the first
 * chunk directly, which is the fast path of __tls_get_addr, else along the
 * chain.
 */
static struct rtld_tls_module *
tls_module_at(
	uintptr_t id)
{
	struct rtld_tls_chunk *chunk;
	uintptr_t step;

	/* Most processes have no more modules than the first chunk holds. */
	if (id < RTLD_TLS_CHUNK)
		return &tls_chunk_first.slots[id];

	/* Along the chain to the id's chunk. */
	chunk = &tls_chunk_first;
	for (step = id / RTLD_TLS_CHUNK; step != 0; step--)
		chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE);

	/* Reports the module's slot within its chunk. */
	return &chunk->slots[id % RTLD_TLS_CHUNK];
}

/*
 * Gives a TLS module id: one a module gone left free, else the next one,
 * linking a chunk first when that id starts one.  The loader lock is held;
 * the caller writes the module and counts the id (register_tls_module).
 */
static uintptr_t
tls_module_new_id(
	void)
{
	struct rtld_tls_module *module;
	struct rtld_tls_chunk *chunk;
	uintptr_t id;

	/* A free id among those given before. */
	for (id = 1; id <= tls_module_count; id++) {
		/* The first whose module is gone. */
		module = tls_module_at(id);
		if (!module->active)
			return id;
	}

	/* The next id, in a new chunk when it starts one. */
	id = tls_module_count + 1U;
	if (id % RTLD_TLS_CHUNK == 0) {
		chunk = tls_map(sizeof(*chunk));
		if (chunk == NULL)
			rtld_fatal("cannot allocate TLS module table");

		/* Published after the last, before the id is counted. */
		__atomic_store_n(&tls_chunk_last->next, chunk, __ATOMIC_RELEASE);
		tls_chunk_last = chunk;
	}

	/* Succeeded: a new id. */
	return id;
}

/*
 * Supports the layout static tls operation.
 *
 * Places the main executable first, because its displacement below the
 * thread pointer is the one a linker has already committed to, and then
 * every other module loaded at startup.  The result is a template image of
 * the whole area, which each thread copies into place as it is created.
 */
#if !defined(HAL_ARCH_AMD64) && !defined(HAL_ARCH_I386)
static void
layout_static_tls(
	void)
{
	/*
	 * A variant I architecture counts upwards from the thread pointer and
	 * reserves the control block at its base, which the kern_tls_prefix
	 * contract cannot describe.  The kernel declines static TLS there for
	 * the same reason, so every module stays dynamic.
	 */
}
#else
static void
layout_static_tls(
	void)
{
	struct rtld_tls_module *module;
	struct rtld_tls_module *main_module;
	unsigned char *image;
	uintptr_t offset;
	uintptr_t id;

	/* Handles a second pass, which would move blocks already in use. */
	if (static_tls_sealed)
		return;

	/*
	 * The main program's module first, where the displacements its linker
	 * fixed expect it; then every other module loaded at startup, by id
	 * (WS140 D7: two passes instead of a list of the modules).
	 */
	offset = 0;
	main_module = NULL;
	if (main_object != NULL && main_object->tls_module_id != 0) {
		main_module = tls_module_at(main_object->tls_module_id);
		offset = static_tls_place(main_module, offset);
	}

	/* Places every other module loaded at startup, by id. */
	for (id = 1; id <= tls_module_count; id++) {
		/* Skips a slot that holds no module, and the main one. */
		module = tls_module_at(id);
		if (!module->active || module == main_module)
			continue;

		/* Places it below the modules placed so far. */
		offset = static_tls_place(module, offset);
	}

	/* No module is placed after this pass. */
	static_tls_sealed = 1;

	/* Handles the offset condition. */
	if (offset == 0)
		return;

	/* The template is mapped once and never written again. */
	image = tls_map((size_t)offset);

	/* Handles the image availability. */
	if (image == NULL)
		rtld_fatal("cannot allocate static TLS template");

	/* Copies each placed module's image to its place; the order does not matter. */
	for (id = 1; id <= tls_module_count; id++) {
		/* Anonymous memory leaves .tbss and the padding zeroed. */
		module = tls_module_at(id);
		if (!module->active || !module->is_static || module->file_size == 0)
			continue;

		/* Copies its initialized data to its place in the template. */
		rtld_memcpy(image + (offset - module->static_offset),
			    module->init_image,
			    module->file_size);
	}

	/* Publishes the area every new thread gets. */
	static_tls_distance = offset;
	static_tls_template = image;
	static_tls_template_size = (size_t)offset;
}
#endif

#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_I386)
/*
 * Places one module in the static TLS area below the modules placed
 * before it, whose blocks end offset bytes below the thread pointer, and
 * returns where the next one starts.
 */
static uintptr_t
static_tls_place(
	struct rtld_tls_module *module,
	uintptr_t offset)
{
	/* Refuses a total the thread pointer cannot reach. */
	if (module->memory_size > KERN_TLS_MEMORY_MAX - offset)
		rtld_fatal("static TLS area is too large");

	/*
	 * Variant II counts downwards from the thread pointer, so a
	 * block ends at its own offset and the rounding that aligns
	 * it belongs below, not above.
	 */
	offset += module->memory_size;
	offset = (offset + module->alignment - 1U) &
		 ~(uintptr_t)(module->alignment - 1U);
	module->static_offset = (size_t)offset;
	module->is_static = 1;

	/* The area is aligned for its most demanding module. */
	if (module->alignment > static_tls_alignment)
		static_tls_alignment = module->alignment;

	/* Where the next module is placed from. */
	return offset;
}
#endif

#if defined(HAL_ARCH_AMD64)

/*
 * Supports the static tls displacement operation.
 *
 * Reports how far below the thread pointer a module's block sits.  Only a
 * module present in every thread has such a place, so an object that
 * dlopen() added later cannot satisfy an initial-exec access.
 */
static uintptr_t
static_tls_displacement(
	const struct rtld_object *owner)
{
	const struct rtld_tls_module *module;

	/* Handles the owner availability. */
	if (owner == NULL || owner->tls_module_id == 0)
		rtld_fatal("TLS module is unavailable");
	module = tls_module_at(owner->tls_module_id);

	/* Handles a module that is not part of the static area. */
	if (!module->is_static)
		rtld_fatal("initial-exec TLS needs a startup-loaded module");

	/* Returns the computed result. */
	return (uintptr_t)0 - (uintptr_t)module->static_offset;
}

#endif

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

	/* Appends the object to the initialization order (WS140 D4). */
	loader_lock();

	object->init_prev = initialization_tail;
	object->init_next = NULL;

	/* Links it after the old tail, or makes it the head of an empty list. */
	if (initialization_tail != NULL)
		initialization_tail->init_next = object;
	else
		initialization_head = object;

	/* Makes it the tail, which process_fini finalizes first. */
	initialization_tail = object;

	loader_unlock();

	/* Done: a later call finds it initialized. */
	object->initializing = 0;
}

/* Supports the clear loader error operation. */
static void
clear_loader_error(
	void)
{
	intptr_t value;
	struct __rtld_tcb *tcb;

	value = syscall6(KERN_SYS_thread_self,
				  KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
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

	value = syscall6(KERN_SYS_thread_self,
				  KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
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
	struct rtld_handle_chunk *chunk;
	struct rtld_handle *handle;

	/* A slot no handle uses, in the chunks there are. */
	handle = handle_free_slot();

	/* None: one more chunk; without memory dlopen fails (WS140 D8). */
	if (handle == NULL) {
		chunk = tls_map(sizeof(*chunk));
		if (chunk == NULL)
			return NULL;

		/* Linked after the last; its first slot is the handle. */
		handle_chunk_last->next = chunk;
		handle_chunk_last = chunk;
		handle = &chunk->slots[0];
	}

	/* A generation that is never 0, which validate_handle refuses. */
	handle->magic = RTLD_HANDLE_MAGIC;
	handle->generation = next_handle_generation++;
	if (next_handle_generation == 0)
		next_handle_generation = 1;

	/* The handle's object and scope. */
	handle->object = object;
	handle->references = 1;
	handle->active = 1;
	handle->main_scope = (unsigned)main_scope;

	/* A handle of its own keeps the object loaded. */
	if (!main_scope)
		object->direct_refs++;

	/* Succeeded: the new handle. */
	return handle;
}

/*
 * Finds a handle slot no handle uses, or NULL when every chunk is full.
 */
static struct rtld_handle *
handle_free_slot(
	void)
{
	struct rtld_handle_chunk *chunk;
	unsigned i;

	/* Searches every chunk in order for an unused slot. */
	for (chunk = &handle_chunk_first; chunk != NULL; chunk = chunk->next) {
		for (i = 0; i < RTLD_HANDLE_CHUNK; i++) {
			/* Reports the first slot no handle holds. */
			if (!chunk->slots[i].active)
				return &chunk->slots[i];
		}
	}

	/* Reports that every slot is in use. */
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

/* Tells whether a dlopen path names its file: it is not empty and holds a slash (other than /lib/ and a bare name). */
static int
dlopen_names_file(
	const char *path)
{
	const char *cursor;

	/* No path. */
	if (path == NULL || path[0] == '\0')
		return 0;

	/* A slash anywhere makes it a path to open as it is. */
	for (cursor = path; *cursor != '\0'; cursor++) {
		if (*cursor == '/')
			return 1;
	}

	/* Succeeded: a bare name, looked for in the library directories. */
	return 0;
}

/* Returns the file name of an object's path: what follows its last slash, or the whole path. */
static const char *
object_basename(
	const char *path)
{
	const char *cursor;
	const char *name;

	/* The part after the last slash. */
	name = path;
	for (cursor = path; *cursor != '\0'; cursor++) {
		if (*cursor == '/')
			name = cursor + 1;
	}

	/* The name found. */
	return name;
}

/* Supports the preflight dlopen file operation. */
static int
preflight_dlopen_file(
	int fd)
{
	struct stat status;
	Elf_Ehdr header;
	Elf_Phdr room[RTLD_PROGRAM_INLINE];
	Elf_Phdr *phdr;
	intptr_t result;
	size_t phdr_mapping;
	int valid;

	result = syscall6(KERN_SYS_fstat, (uintptr_t)fd, (uintptr_t)&status,
			  0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(result) || status.st_size < (off_t)sizeof(header))
		return -1;
	result = syscall6(KERN_SYS_pread, (uintptr_t)fd, (uintptr_t)&header,
			  sizeof(header), 0, 0, 0);

	/* Handles a failed valid elf header operation. */
	if (result != (intptr_t)sizeof(header) ||
	    !valid_elf_header(&header, ET_DYN) ||
	    header.e_phoff > (Elf_Off)status.st_size ||
	    header.e_phnum >
		((Elf_Off)status.st_size - header.e_phoff) / sizeof(Elf_Phdr))

		/* Reports operation failure. */
		return -1;

	/* The program headers, in a mapping of their own when they are many. */
	phdr = read_program_headers(fd,
				    &header,
				    room,
				    RTLD_PROGRAM_INLINE,
				    &phdr_mapping);
	if (phdr == NULL)
		return -1;

	/* Checks them, then lets a mapping of their own go. */
	valid = validate_file_programs(&header, phdr, status.st_size) == 0;
	if (phdr_mapping != 0)
		tls_unmap(phdr, phdr_mapping);

	/* Refuses a file whose program headers do not check. */
	if (!valid)
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
	    header->e_phnum >= PN_XNUM)

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
	struct rtld_object *object;
	intptr_t fd;
	size_t length;
	unsigned i;

	/* Handles the name availability. */
	if (name == NULL || name[0] == '\0')
		rtld_fatal("empty dependency name");
	length = rtld_strlen(name);

	/* Checks the current data length. */
	if (length >= RTLD_PATH_MAX)
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

	/* Maps the file found and its dependencies. */
	object = load_object_file(fd, path, requester);

	/* Succeeded: the object of the file. */
	return object;
}

/*
 * Loads the shared object at a path that names its file (dlopen of a path
 * with a slash, opened as it is rather than searched for, T1-495).
 */
static struct rtld_object *
load_object_path(
	const char *file_path)
{
	char path[RTLD_PATH_MAX];
	struct rtld_object *object;
	intptr_t fd;

	/* The path as the object's own, which the caller measured against RTLD_PATH_MAX. */
	copy_path(path, file_path);

	/* Opens the file itself. */
	fd = syscall6(KERN_SYS_open, (uintptr_t)path, O_RDONLY, 0, 0, 0, 0);
	if (raw_error(fd))
		rtld_fatal("cannot open shared object");

	/* Maps the file and its dependencies. */
	object = load_object_file(fd, path, NULL);

	/* Succeeded: the object of the file. */
	return object;
}

/* Maps an opened shared object (closing its descriptor), or finds it loaded already; then its dependencies. */
static struct rtld_object *
load_object_file(
	intptr_t fd,
	const char path[RTLD_PATH_MAX],
	struct rtld_object *requester)
{
	struct stat status;
	Elf_Ehdr header;
	Elf_Phdr room[RTLD_PROGRAM_INLINE];
	Elf_Phdr *phdr;
	struct rtld_object *object, *existing;
	intptr_t result;
	size_t phdr_mapping;
	unsigned i;
	uintptr_t minimum;

	minimum = UINTPTR_MAX;

	/* The file's identity and size. */
	result = syscall6(KERN_SYS_fstat, (uintptr_t)fd, (uintptr_t)&status,
			  0, 0, 0, 0);

	/* Handles an operation failure. */
	if (raw_error(result) || status.st_size < (off_t)sizeof(header))
		rtld_fatal("cannot stat dependency");
	existing = find_identity(&status);

	/* Handles the existing availability. */
	if (existing != NULL) {
		(void)syscall6(KERN_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);

		/* Returns the computed result. */
		return existing;
	}
	result = syscall6(KERN_SYS_pread, (uintptr_t)fd, (uintptr_t)&header,
			  sizeof(header), 0, 0, 0);

	/* Handles a failed valid elf header operation. */
	if (result != (intptr_t)sizeof(header) ||
	    !valid_elf_header(&header, ET_DYN) ||
	    header.e_phoff > (Elf_Off)status.st_size ||
	    header.e_phnum >
		((Elf_Off)status.st_size - header.e_phoff) / sizeof(Elf_Phdr))
		rtld_fatal("invalid dependency ELF header");

	/* The program headers, in a mapping of their own when they are many. */
	phdr = read_program_headers((int)fd,
				    &header,
				    room,
				    RTLD_PROGRAM_INLINE,
				    &phdr_mapping);
	if (phdr == NULL)
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
	object_set_programs(object, phdr, header.e_phnum);
	if (phdr_mapping != 0)
		tls_unmap(phdr, phdr_mapping);

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Handles a failed page floor operation. */
		if (object->phdr[i].p_type == PT_LOAD &&
		    object->phdr[i].p_memsz != 0 &&
		    page_floor((uintptr_t)object->phdr[i].p_vaddr) < minimum) {
			minimum =
			    page_floor((uintptr_t)object->phdr[i].p_vaddr);
		}
	}
	/*
	 * The whole span of the load segments, reserved at once (WS140,
	 * T1-163): mapping the first segment alone let the kernel place it in
	 * a hole too small for the segments after it, which then could not be
	 * mapped.
	 */
	object_reserve_span(object, minimum);

	/* Each load segment, into its place in the span. */
	for (i = 0; i < object->phnum; i++) {
		/* A load segment with memory. */
		if (object->phdr[i].p_type == PT_LOAD &&
		    object->phdr[i].p_memsz != 0)
			map_one_segment(object, (int)fd, &object->phdr[i]);
	}
	(void)syscall6(KERN_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
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

	/* A caller's own list is consulted before the system directories. */
	if (library_path != NULL && *library_path != '\0') {
		fd = open_search_list(library_path, NULL, name, name_length,
				      path);

		/* Handles an operation failure. */
		if (!raw_error(fd))
			return fd;
	}

	/*
	 * The default search: /lib holds the libraries the base system itself
	 * needs, /usr/lib the ones that arrive with packages.  A name is
	 * looked for in that order, so a base library is never shadowed.
	 */
	function_result = open_search_candidate("/lib", 4U, name, name_length, path);

	/* Handles an operation failure. */
	if (raw_error(function_result))
		function_result = open_search_candidate("/usr/lib", 8U, name,
						       name_length, path);

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

			/* Both parts fit, a long suffix too (it cannot wrap the sum). */
			if (suffix_length < sizeof(directory) &&
			    origin_length <=
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
	    name_length == 0 || name_length > RTLD_PATH_MAX - 2U ||
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
	function_result = syscall6(KERN_SYS_open, (uintptr_t)path, O_RDONLY, 0, 0, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the find identity operation. */
static struct rtld_object *
find_identity(
	const struct stat *status)
{
	struct rtld_object *object;
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		/* An object in the process now with the same file identity. */
		object = object_at(i);
		if (object->active && !object->unloading &&
		    object->has_identity &&
		    object->device == status->st_dev &&
		    object->inode == status->st_ino)

			/* Returns the computed result. */
			return object;
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

	/* A slot no object uses now. */
	object = NULL;
	for (i = 0; i < object_count; i++) {
		/* Stops at the first slot that is free. */
		object = object_at(i);
		if (!object->active)
			break;
	}

	/* None: the next one, in a new chunk when the last is full (the first is static). */
	if (i == object_count) {
		/* Links a chunk when every slot of the last is counted. */
		if (object_count != 0 && object_count % RTLD_OBJECT_CHUNK == 0)
			object_chunk_grow();

		/* Counts the slot after its chunk is linked. */
		object = &object_chunk_last->slots[object_count % RTLD_OBJECT_CHUNK];
		__atomic_store_n(&object_count, object_count + 1U, __ATOMIC_RELEASE);
	}
	object_clear(object);
	object->active = 1;
	object->generation = next_object_generation++;

	/* A walk of the loaded objects reports this as one more addition. */
	rtld_object_generation++;

	/* Handles the next object generation condition. */
	if (next_object_generation == 0)
		next_object_generation = 1;
	copy_path(object->path, path);

	/* Returns the computed result. */
	return object;
}

/*
 * Finds the slot of an object by its number (below object_count): its
 * chunk, along the chain, and its place there.
 */
static struct rtld_object *
object_at(
	unsigned index)
{
	struct rtld_object_chunk *chunk;
	unsigned step;

	/* Along the chain to the slot's chunk. */
	chunk = &object_chunk_first;
	for (step = index / RTLD_OBJECT_CHUNK; step != 0; step--)
		chunk = __atomic_load_n(&chunk->next, __ATOMIC_ACQUIRE);

	/* Reports the slot of the index within its chunk. */
	return &chunk->slots[index % RTLD_OBJECT_CHUNK];
}

/*
 * Links one more chunk to the object table: mapped zeroed and published
 * before any of its slots is counted.
 */
static void
object_chunk_grow(
	void)
{
	struct rtld_object_chunk *chunk;

	/* The chunk; without memory the process cannot go on (WS140 U2). */
	chunk = tls_map(sizeof(*chunk));
	if (chunk == NULL)
		rtld_fatal("cannot allocate shared-object table");

	/* Links it after the last chunk before any of its slots is counted. */
	__atomic_store_n(&object_chunk_last->next, chunk, __ATOMIC_RELEASE);
	object_chunk_last = chunk;
}

/*
 * Gives a program table of at least size bytes: one a gone object left
 * when it is large enough, otherwise a new mapping.
 */
static struct rtld_program_table *
program_table_take(
	size_t size)
{
	struct rtld_program_table **link;
	struct rtld_program_table *table;

	/* Reuses a table an unloaded object left, when it is large enough. */
	for (link = &program_tables_free; *link != NULL; link = &(*link)->next) {
		/* Takes the first that is large enough off the list. */
		table = *link;
		if (table->size >= size) {
			*link = table->next;
			table->next = NULL;

			/* Succeeded: a table used before. */
			return table;
		}
	}

	/* A new one; without memory the process cannot go on (WS140 U2). */
	table = tls_map(size);
	if (table == NULL)
		rtld_fatal("cannot allocate program-header table");

	/* Records the size mapped, for reuse. */
	table->size = page_ceil(size);

	/* Succeeded: a new table. */
	return table;
}

/*
 * Gives an object its program headers: kept inside it when they fit,
 * otherwise in one mapping that also holds the records of the mappings
 * its segments get (two a header at most).
 */
static void
object_set_programs(
	struct rtld_object *object,
	const Elf_Phdr *phdr,
	unsigned phnum)
{
	struct rtld_program_table *table;
	unsigned char *data;
	size_t headers_size;
	size_t starts_size;
	size_t size;

	/* Few enough: the room inside the object, which object_clear set. */
	object->phnum = phnum;
	if (phnum > RTLD_PROGRAM_INLINE) {
		/* The head, the headers, then the mappings' starts and sizes. */
		headers_size = (size_t)phnum * sizeof(Elf_Phdr);
		starts_size = 2U * (size_t)phnum * sizeof(uintptr_t);
		size = sizeof(*table) + headers_size + starts_size +
		    2U * (size_t)phnum * sizeof(size_t);
		table = program_table_take(size);

		/* The object's tables are in the table from now on. */
		data = (unsigned char *)(void *)(table + 1);
		object->program_table = table;
		object->phdr = (Elf_Phdr *)(void *)data;
		object->mapping_start = (uintptr_t *)(void *)(data + headers_size);
		object->mapping_size =
		    (size_t *)(void *)(data + headers_size + starts_size);
		object->mapping_capacity = 2U * phnum;
	}

	/* Copies the headers into the object's room or its table. */
	rtld_memcpy(object->phdr, phdr, (size_t)phnum * sizeof(Elf_Phdr));
}

/*
 * Reads an ELF file's program headers (the header checked their place
 * against the file): into the caller's room when they fit, otherwise into
 * a mapping of their own, whose size is put in *mapping_size (0 for the
 * caller's room).  Returns the headers, or NULL when the file is short.
 */
static Elf_Phdr *
read_program_headers(
	int fd,
	const Elf_Ehdr *header,
	Elf_Phdr *room,
	unsigned room_count,
	size_t *mapping_size)
{
	Elf_Phdr *headers;
	size_t size;
	intptr_t result;

	/* The caller's room, or a mapping. */
	size = (size_t)header->e_phnum * sizeof(Elf_Phdr);
	headers = room;
	*mapping_size = 0;
	if (header->e_phnum > room_count) {
		headers = tls_map(size);
		if (headers == NULL)
			rtld_fatal("cannot allocate program headers");
		*mapping_size = size;
	}

	/* Reads the headers from the file. */
	result = syscall6(KERN_SYS_pread,
			  (uintptr_t)fd,
			  (uintptr_t)headers,
			  size,
			  (uintptr_t)header->e_phoff,
			  0,
			  0);
	if (result != (intptr_t)size) {
		/* A short file: lets a mapping of their own go. */
		if (*mapping_size != 0)
			tls_unmap(headers, *mapping_size);

		/* Nothing is left mapped for the caller to let go. */
		*mapping_size = 0;

		/* Reports the short file. */
		return NULL;
	}

	/* Succeeded: the headers. */
	return headers;
}

/*
 * Gives an object room for one more TLSDESC argument: inside it, then in
 * the last page, then in a new page linked after it.
 */
static struct __tls_index *
tlsdesc_slot(
	struct rtld_object *object)
{
	struct rtld_tlsdesc_chunk *chunk;
	struct __tls_index *slot;

	/* Uses the room inside the object while it lasts. */
	if (object->tlsdesc_count < RTLD_TLSDESC_INLINE) {
		slot = &object->tlsdesc_inline[object->tlsdesc_count];
		object->tlsdesc_count++;

		/* Succeeded: room inside the object. */
		return slot;
	}

	/* A new page when there is none or the last is full. */
	chunk = object->tlsdesc_last;
	if (chunk == NULL || chunk->used == RTLD_TLSDESC_CHUNK) {
		chunk = tls_map(sizeof(*chunk));
		if (chunk == NULL)
			rtld_fatal("cannot allocate TLSDESC arguments");

		/* Links it first, or after the last. */
		if (object->tlsdesc_last == NULL)
			object->tlsdesc_chunks = chunk;
		else
			object->tlsdesc_last->next = chunk;

		/* Makes it the page the next argument goes in. */
		object->tlsdesc_last = chunk;
	}

	/* The next argument of the page. */
	slot = &chunk->index[chunk->used];
	chunk->used++;
	object->tlsdesc_count++;

	/* Succeeded: room in a page. */
	return slot;
}

/*
 * Takes what an object holds outside itself, before its slot is cleared;
 * object_tables_release lets it go once nothing reads it.
 */
static void
object_tables_take(
	const struct rtld_object *object,
	struct rtld_object_tables *tables)
{
	/* The dependency table, the program table and the TLSDESC pages. */
	tables->needed_mapping = object->needed_mapping;
	tables->needed_mapping_size = object->needed_mapping_size;
	tables->program_table = object->program_table;
	tables->tlsdesc_chunks = object->tlsdesc_chunks;
}

/*
 * Lets go of what object_tables_take took.
 */
static void
object_tables_release(
	const struct rtld_object_tables *tables)
{
	struct rtld_tlsdesc_chunk *chunk;
	struct rtld_tlsdesc_chunk *next;

	/* The dependency table (nothing when the object held it inside). */
	tls_unmap(tables->needed_mapping, tables->needed_mapping_size);

	/* The program table is kept for reuse, never unmapped. */
	if (tables->program_table != NULL) {
		tables->program_table->next = program_tables_free;
		program_tables_free = tables->program_table;
	}

	/* Each TLSDESC page, its link read before it goes. */
	for (chunk = tables->tlsdesc_chunks; chunk != NULL; chunk = next) {
		next = chunk->next;
		tls_unmap(chunk, sizeof(*chunk));
	}
}

/*
 * Makes room for an object's dependencies: the inline room when they fit,
 * otherwise one mapping holding the objects' pointers, then their names'
 * offsets.  The entries are counted as parse_dynamic reads them, up to
 * the first DT_NULL.
 */
static void
object_reserve_needed(
	struct rtld_object *object)
{
	unsigned char *table;
	size_t count;
	size_t size;
	size_t i;

	/* Counts the DT_NEEDED entries. */
	count = 0;
	for (i = 0; i < object->dynamic_count; i++) {
		/* The end of the section, as parse_dynamic stops there. */
		if (object->dynamic[i].d_tag == DT_NULL)
			break;

		/* Counts a dependency. */
		if (object->dynamic[i].d_tag == DT_NEEDED)
			count++;
	}

	/* Few enough for the room inside the object. */
	if (count <= RTLD_NEEDED_INLINE)
		return;

	/* A table; without memory the process cannot go on (WS140 U2). */
	size = count * (sizeof(struct rtld_object *) + sizeof(uint32_t));
	table = tls_map(size);
	if (table == NULL)
		rtld_fatal("cannot allocate dependency table");

	/* The objects' pointers first, for their alignment, then the offsets. */
	object->needed_mapping = table;
	object->needed_mapping_size = size;
	object->needed = (struct rtld_object **)(void *)table;
	object->needed_offset = (uint32_t *)(void *)
	    (table + count * sizeof(struct rtld_object *));
}

/*
 * Clears an object's slot and points its tables at the room inside it.
 * The headers' pointer is never left NULL, since dl_iterate_phdr may read
 * the slot without the loader lock.
 */
static void
object_clear(
	struct rtld_object *object)
{
	/* Everything, then the inline tables. */
	rtld_memset(object, 0, sizeof(*object));
	object->phdr = object->phdr_inline;
	object->mapping_start = object->mapping_start_inline;
	object->mapping_size = object->mapping_size_inline;
	object->mapping_capacity = 2U * RTLD_PROGRAM_INLINE;
	object->needed = object->needed_inline;
	object->needed_offset = object->needed_offset_inline;
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

/*
 * Reserves the address range of an object's load segments, from the page
 * of the lowest (low) to the end of the highest, as one inaccessible
 * mapping, which costs no commit, and sets the object's base from where it
 * landed.  The segments are then mapped over it; the reservation is the
 * one mapping the object records, so unloading it unmaps them all.
 */
static void
object_reserve_span(
	struct rtld_object *object,
	uintptr_t low)
{
	uintptr_t high;
	uintptr_t end;
	intptr_t mapped;
	unsigned i;
	int failed;

	/* The end of the highest load segment (validate_file_programs checked the sums). */
	high = low;
	for (i = 0; i < object->phnum; i++) {
		/* A load segment with memory. */
		if (object->phdr[i].p_type != PT_LOAD || object->phdr[i].p_memsz == 0)
			continue;

		/* Its end, rounded to a page. */
		end = page_ceil((uintptr_t)(object->phdr[i].p_vaddr + object->phdr[i].p_memsz));
		if (end > high)
			high = end;
	}

	/* The range, anywhere the kernel finds room. */
	mapped = map_call(0,
			  (size_t)(high - low),
			  PROT_NONE,
			  MAP_PRIVATE | MAP_ANONYMOUS,
			  -1,
			  0);
	failed = raw_error(mapped);
	if (failed)
		rtld_fatal("cannot reserve shared object address range");

	/* Refuses a place below the lowest segment's address, which no base reaches. */
	if ((uintptr_t)mapped < low)
		rtld_fatal("invalid shared object load bias");

	/* The base, and the reservation as the object's one mapping. */
	object->base = (uintptr_t)mapped - low;
	remember_mapping(object, (uintptr_t)mapped, (size_t)(high - low));
}

/* Supports the map one segment operation. */
static void
map_one_segment(
	struct rtld_object *object,
	int fd,
	const Elf_Phdr *program)
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
	requested = object->base + virtual_page;
	flags = MAP_PRIVATE | MAP_FIXED;
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

	/* The segment lands where the span has room for it (MAP_FIXED within the reservation). */
	if ((uintptr_t)mapped != requested)
		rtld_fatal("cannot map shared object segment");

	/* File-backed segments may need additional anonymous BSS pages.
	 * A pure BSS segment was already mapped in full above. */
	if (file_map_size != 0 && file_map_size < memory_map_size) {
		anonymous = object->base + virtual_page + file_map_size;
		anonymous_size = memory_map_size - file_map_size;
		mapped = map_call(
		    anonymous, anonymous_size, map_prot,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

		/* Handles an operation failure. */
		if (raw_error(mapped) || (uintptr_t)mapped != anonymous)
			rtld_fatal("cannot map shared object zero fill");
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
		result = syscall6(KERN_SYS_mprotect, object->base + virtual_page,
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
	if (object->mapping_count == object->mapping_capacity)
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

	/* Room for every dependency, counted before the entries are read. */
	object_reserve_needed(object);

	/* Process each remaining element. */
	for (i = 0; i < object->dynamic_count; i++) {
		dynamic = &object->dynamic[i];

		/* Handles the dynamic condition. */
		if (dynamic->d_tag == DT_NULL)
			break;

		/* Dispatch the selected operation case. */
		switch ((int)dynamic->d_tag) {
		case DT_NEEDED:
			/* object_reserve_needed made room for each. */
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

/*
 * Reports whether a relocation may write the bytes at address.  Targets
 * come in address order, so the writable segment the last one fell in
 * answers nearly every check without the search over the headers.
 */
static int
relocation_target_writable(
	struct rtld_object *object,
	uintptr_t address,
	size_t size)
{
	uintptr_t start, end;
	unsigned i;

	/* Handles the address condition. */
	if (address > UINTPTR_MAX - size)
		return 0;

	/* Handles the address inside the last segment. */
	if (address >= object->relocation_window_start &&
	    address + size <= object->relocation_window_end)
		return 1;

	/* Process each element required by the operation. */
	for (i = 0; i < object->phnum; i++) {
		/* Checks the current object. */
		if (object->phdr[i].p_type != PT_LOAD ||
		    (object->phdr[i].p_flags & PF_W) == 0)
			continue;
		start = object->base + (uintptr_t)object->phdr[i].p_vaddr;

		/* Handles the uintptr t condition. */
		if ((uintptr_t)object->phdr[i].p_memsz > UINTPTR_MAX - start)
			continue;
		end = start + (uintptr_t)object->phdr[i].p_memsz;

		/* Handles the address condition. */
		if (address >= start && address + size <= end) {
			object->relocation_window_start = start;
			object->relocation_window_end = end;
			return 1;
		}
	}

	/* Reports that no result is available. */
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
	uintptr_t id;
	unsigned i;

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
			    object->phdr[i].p_memsz > KERN_TLS_MEMORY_MAX)
				rtld_fatal("unsupported TLS alignment or size");

			/* A free id, or the next one (WS140). */
			id = tls_module_new_id();
			object->tls_module_id = id;
			module = tls_module_at(id);
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

			/*
			 * Marks it active after it is written, then counts a
			 * new id, so that __tls_get_addr, which reads the
			 * count and then the flag, never sees half a module.
			 */
			__atomic_store_n(&module->active, 1U, __ATOMIC_RELEASE);
			if (id > tls_module_count)
				__atomic_store_n(&tls_module_count, id, __ATOMIC_RELEASE);

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
		result = syscall6(KERN_SYS_mprotect, start, RTLD_PAGE_SIZE,
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
		result = syscall6(KERN_SYS_mprotect, start, end - start,
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

	/* Handles a failed relocation target writable operation. */
	if (!relocation_target_writable(object, address,
#if defined(HAL_ARCH_SPARCV9)
			     type == R_SPARC_JMP_SLOT ? 8U * sizeof(uint32_t) :
#endif
						      sizeof(uintptr_t)))
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
	case R_X86_64_TPOFF64:
		/* Handles the symbol index condition. */
		if (symbol_index == 0) {
			tls_owner = object;
			value = 0;
		} else {
			tls_symbol = resolve_tls_symbol(object, symbol_index,
							&tls_owner);

			/* Handles the tls symbol availability. */
			if (tls_symbol == NULL)
				rtld_fatal("TLS symbol is unavailable");
			value = (uintptr_t)tls_symbol->st_value;
		}
		value += static_tls_displacement(tls_owner) + addend;
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
	struct rtld_symbol_name symbol_name;
	struct rtld_object *object;
	Elf_Sym *symbol;
	unsigned i;

	symbol_name.name = name;
	symbol_name.hashed = 0;

	/* Handles the reserved loader symbol condition. */
	if (reserved_loader_symbol(name)) {
		symbol = lookup_in_object_hashed(interpreter_object, &symbol_name,
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
	symbol = lookup_in_object_hashed(main_object, &symbol_name,
					 required_version);

	/* Handles the symbol availability. */
	if (symbol != NULL) {
		/* Obtains the symbol value result. */
		function_result = symbol_value(main_object, symbol);

		/* Returns the computed result. */
		return function_result;
	}

	/* Process each remaining element. */
	for (i = 0; i < object_count; i++) {
		object = object_at(i);

		/* Checks the current object. */
		if (!object->active || object->unloading ||
		    object == main_object || object == interpreter_object)
			continue;
		symbol = lookup_in_object_hashed(object, &symbol_name,
						 required_version);

		/* Handles the symbol availability. */
		if (symbol != NULL) {
			/* Obtains the symbol value result. */
			function_result = symbol_value(object, symbol);

			/* Returns the computed result. */
			return function_result;
		}
	}
	symbol = lookup_in_object_hashed(interpreter_object, &symbol_name,
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

	/* Every reserved name begins with two underscores. */
	if (name[0] != '_' || name[1] != '_')
		return 0;

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
	struct rtld_symbol_name symbol_name;

	symbol_name.name = name;
	symbol_name.hashed = 0;

	return lookup_in_object_hashed(object, &symbol_name, required_version);
}

/*
 * Looks a name up in one object, computing each hash the first time an
 * object needs it and keeping it for the objects searched after.
 */
static Elf_Sym *
lookup_in_object_hashed(
	struct rtld_object *object,
	struct rtld_symbol_name *symbol_name,
	const char *required_version)
{
	Elf_Sym *symbol;
	uint32_t buckets, index, *bucket, *chain;
	unsigned traversed;

	traversed = 0;

	/* Handles the object availability. */
	if (object == NULL || !object->active || object->unloading ||
	    (object->hash == NULL && object->gnu_bloom == NULL))
		return NULL;

	/* Handles the gnu bloom availability. */
	if (object->gnu_bloom != NULL)
		return lookup_gnu_hash(object, symbol_name, required_version);

	/* Computes the hashes the first time an object needs one. */
	if (!symbol_name->hashed)
		hash_symbol_name(symbol_name);
	buckets = object->hash[0];
	bucket = object->hash + 2;
	chain = bucket + buckets;
	index = bucket[symbol_name->elf_hash % buckets];
	while (index != STN_UNDEF && traversed++ < object->symbol_count) {
		/* Checks the current index. */
		if (index >= object->symbol_count)
			rtld_fatal("corrupt symbol hash chain");
		symbol = match_symbol(object, index, symbol_name->name,
				      required_version);

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
	struct rtld_symbol_name *symbol_name,
	const char *required_version)
{
	uint32_t chain_hash;
	Elf_Sym *symbol;
	const unsigned word_bits = sizeof(Elf_Addr) * 8U;
	uint32_t hash;
	Elf_Addr mask, bloom;
	uint32_t index;

	/* Computes the hashes the first time an object needs one. */
	if (!symbol_name->hashed)
		hash_symbol_name(symbol_name);
	hash = symbol_name->gnu_hash;

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
			symbol = match_symbol(object, index, symbol_name->name,
					      required_version);

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

/*
 * Computes both hashes of a name in one pass: a search usually meets
 * objects with each kind of table, and the names are long.
 */
static void
hash_symbol_name(
	struct rtld_symbol_name *symbol_name)
{
	const unsigned char *cursor;
	uint32_t gnu, elf, high;

	gnu = 5381U;
	elf = 0;
	for (cursor = (const unsigned char *)symbol_name->name; *cursor != '\0';
	     cursor++) {
		gnu = gnu * 33U + *cursor;
		elf = (elf << 4) + *cursor;
		high = elf & 0xf0000000U;
		elf ^= high >> 24;
		elf &= ~high;
	}
	symbol_name->gnu_hash = gnu;
	symbol_name->elf_hash = elf;
	symbol_name->hashed = 1;
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
		candidate = object_at(i);

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
	struct rtld_tls_module *module;
	struct __tls_index *index;
	Elf_Sym *symbol;
	uintptr_t offset;

	owner = object;
	symbol = NULL;
	offset = addend;

	/* Handles a failed object contains operation. */
	if (!object_contains(object, address, sizeof(*descriptor), PF_W))
		rtld_fatal("TLSDESC target is not writable");

	/* Handles the symbol index condition. */
	if (symbol_index != 0) {
		symbol = resolve_tls_symbol(object, symbol_index, &owner);

		/* Handles the symbol availability. */
		if (symbol == NULL)
			rtld_fatal("weak TLSDESC symbol is unavailable");
		offset += (uintptr_t)symbol->st_value;
	}

	/* The owner must have a TLS module that holds the offset. */
	if (owner == NULL || owner->tls_module_id == 0)
		rtld_fatal("invalid TLSDESC module or offset");

	/* Refuses an offset past the module's block. */
	module = tls_module_at(owner->tls_module_id);
	if (offset >= module->memory_size)
		rtld_fatal("invalid TLSDESC module or offset");

	/* The argument, in room that grows as the object needs (WS140 U3). */
	index = tlsdesc_slot(object);
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
	struct rtld_object *inline_copy[RTLD_NEEDED_INLINE];
	struct rtld_object **dependencies;
	struct rtld_object_tables tables;
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

	/*
	 * A debugger is told before the object goes, while its name and
	 * its mapping are still there to be read.
	 */
	debug_state_change(RT_DELETE);
	debug_map_remove(object);
	debug_state_change(RT_CONSISTENT);
	remove_initialization_record(object);

	/*
	 * Keeps the dependency list past the clearing of the object below:
	 * the inline list is copied, an external table is kept as it is and
	 * unmapped at the end (WS140 D3).
	 */
	dependency_count = object->needed_count;
	dependencies = object->needed;
	if (object->needed_mapping == NULL) {
		/* Copies the inline list, which the clearing overwrites. */
		for (i = 0; i < dependency_count; i++)
			inline_copy[i] = object->needed[i];

		/* Walks the copy instead of the object's list. */
		dependencies = inline_copy;
	}

	/* Application callbacks may recursively use the loader. */
	loader_unlock();
	finalize_object_unlocked(object);
	loader_lock();

	/* Checks the current object. */
	if (object->tls_module_id != 0) {
		/* Process each element required by the operation. */
		module = tls_module_at(object->tls_module_id);
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

	/* Every removed object counts, with TLS or without (WS140 U5). */
	rtld_object_removals++;

	/* Process each remaining element. */
	for (i = object->mapping_count; i != 0; i--) {
		result = syscall6(KERN_SYS_munmap, object->mapping_start[i - 1U],
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

	/* Clears the slot, keeping its tables to release after the walk. */
	object_tables_take(object, &tables);
	object_clear(object);

	/* Process each remaining element. */
	for (i = 0; i < dependency_count; i++)
		unload_object_locked(dependencies[i]);

	/* Releases the tables, the dependency list among them, now unused. */
	object_tables_release(&tables);
}

/* Supports the remove initialization record operation. */
static void
remove_initialization_record(
	struct rtld_object *object)
{
	/* An object not on the list has nothing to remove. */
	if (object != initialization_head && object->init_prev == NULL)
		return;

	/* Unlinks the object from the one before it, or from the head. */
	if (object->init_prev != NULL)
		object->init_prev->init_next = object->init_next;
	else
		initialization_head = object->init_next;

	/* Unlinks it from the one after it, or from the tail. */
	if (object->init_next != NULL)
		object->init_next->init_prev = object->init_prev;
	else
		initialization_tail = object->init_prev;

	/* Marks it off the list, so a second removal does nothing. */
	object->init_prev = NULL;
	object->init_next = NULL;
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

/*
 * Starts a new dlsym() walk: the marks left by the walks before no longer
 * equal the generation.  When the counter wraps, every mark is cleared so
 * that a mark left long ago cannot pass for the new walk.  The loader lock
 * is held.
 */
static void
lookup_generation_next(
	void)
{
	unsigned i;

	/* Advances to the next walk's number. */
	lookup_generation++;

	/* Clears the old marks once the counter wraps to zero. */
	if (lookup_generation == 0) {
		for (i = 0; i < object_count; i++)
			object_at(i)->lookup_mark = 0;
		lookup_generation = 1;
	}
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
	int found;

	result = 0;
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
		lookup_generation_next();
		result = lookup_handle_graph(handle->object, name, version,
					     &found);
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
	struct rtld_handle_chunk *chunk;
	uintptr_t address;
	uintptr_t first;
	uintptr_t end;
	struct rtld_handle *handle;

	/* The chunk whose slots the address falls on, at a slot's start. */
	address = (uintptr_t)value;
	handle = NULL;
	for (chunk = &handle_chunk_first; chunk != NULL; chunk = chunk->next) {
		/* Takes the address when it is one of this chunk's slots. */
		first = (uintptr_t)&chunk->slots[0];
		end = (uintptr_t)&chunk->slots[RTLD_HANDLE_CHUNK];
		if (address >= first && address < end &&
		    (address - first) % sizeof(chunk->slots[0]) == 0) {
			handle = (struct rtld_handle *)value;
			break;
		}
	}

	/* Not a handle this loader gave. */
	if (handle == NULL)
		return NULL;


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
		object = object_at(i);

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
	int *found)
{
	uintptr_t function_result;
	uintptr_t value;
	Elf_Sym *symbol;
	unsigned i;

	/* Checks the current object. */
	if (object == NULL || !object->active || object->unloading)

		/* Reports successful completion. */
		return 0;

	/* Visits each object once in this walk (WS140 D5). */
	if (object->lookup_mark == lookup_generation)
		return 0;
	object->lookup_mark = lookup_generation;
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
				      version, found);

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
	object->base = base;
	object->type = type;

	/* Handles the phnum condition. */
	if (phnum == 0 || phnum >= PN_XNUM)
		rtld_fatal("invalid pre-mapped program headers");

	/* The headers, then the dynamic section they name. */
	object_set_programs(object, phdr, phnum);
	parse_dynamic(object);
}

/*
 * Reads the calling thread's thread pointer, the address of its control
 * block (struct __rtld_tcb), or 0 when it has none (BUG-110; on amd64 a
 * thread without one faults instead, as FS then has no base to read at).
 *
 * The kernel keeps the pointer a thread installs (thread_self's SET_TLS, a
 * new thread's thread_create, exec's initial one) in the register the
 * processor gives user TLS, and restores it at every switch, in signal
 * handlers too.  On amd64 that is FS's base, which user code cannot read
 * directly, but every control block begins with its own address
 * (kern_tls_prefix.self, set by exec, by this loader and by the static C
 * library), so %fs:0 is the pointer; on arm64 TPIDR_EL0 is the pointer.
 * Elsewhere thread_self is asked.  Reading the register costs nothing,
 * where the system call cost every global-dynamic TLS access about 200 ns.
 * The dynamic process always has a control block before its first code
 * runs: this loader installs one before the entry point, and a new thread
 * gets one at thread_create.
 */
static uintptr_t
thread_pointer(
	void)
{
#if defined(HAL_ARCH_AMD64)
	uintptr_t value;

	/* The control block's first word, its own address. */
	__asm__ volatile("movq %%fs:0, %0" : "=r"(value));

	/* Succeeded: the thread pointer. */
	return value;
#elif defined(HAL_ARCH_ARM64)
	uintptr_t value;

	/* The thread pointer register. */
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));

	/* Succeeded: the thread pointer. */
	return value;
#else
	intptr_t value;

	/* The kernel's record of the thread pointer. */
	value = syscall6(KERN_SYS_thread_self, KERN_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	if (raw_error(value))
		return 0;

	/* Succeeded: the thread pointer, 0 when the thread has none. */
	return (uintptr_t)value;
#endif
}
