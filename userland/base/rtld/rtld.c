/*
 * zedBSD ELF runtime linker
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
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
		struct zedbsd_tls_index index;
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
static struct zedbsd_rtld_tcb *rtld_threads;
static uint32_t next_object_generation = 1;

static uintptr_t
page_floor(uintptr_t value)
{
	return value & ~(uintptr_t)(RTLD_PAGE_SIZE - 1U);
}

static uintptr_t
page_ceil(uintptr_t value)
{
	if (value > UINTPTR_MAX - (RTLD_PAGE_SIZE - 1U))
		rtld_fatal("address overflow");
	return (value + RTLD_PAGE_SIZE - 1U) &
	    ~(uintptr_t)(RTLD_PAGE_SIZE - 1U);
}

static int
raw_error(intptr_t value)
{
	return value < 0 && value >= -4095;
}

static intptr_t
syscall6(uint32_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2,
	uintptr_t a3, uintptr_t a4, uintptr_t a5)
{
	return rtld_syscall6(number, a0, a1, a2, a3, a4, a5);
}

static uintptr_t
current_tid(void)
{
	intptr_t value = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_TID, 0, 0, 0, 0, 0);
	return raw_error(value) ? 0 : (uintptr_t)value;
}

static void
loader_lock(void)
{
	uintptr_t tid = current_tid();

	if (tid != 0 && loader_lock_owner == tid) {
		loader_lock_depth++;
		return;
	}
	for (;;) {
		if (__atomic_exchange_n(&loader_lock_word, 1,
		    __ATOMIC_ACQUIRE) == 0)
			break;
		(void)syscall6(ZEDBSD_SYS_usync, (uintptr_t)&loader_lock_word,
		    ZEDBSD_USYNC_WAIT, 1, 0, 0, ZEDBSD_USYNC_PRIVATE);
	}
	loader_lock_owner = tid;
	loader_lock_depth = 1;
}

static void
loader_unlock(void)
{
	if (loader_lock_depth == 0 || loader_lock_owner != current_tid())
		rtld_fatal("loader lock ownership failure");
	if (--loader_lock_depth != 0)
		return;
	loader_lock_owner = 0;
	__atomic_store_n(&loader_lock_word, 0, __ATOMIC_RELEASE);
	(void)syscall6(ZEDBSD_SYS_usync, (uintptr_t)&loader_lock_word,
	    ZEDBSD_USYNC_WAKE, 0, 0, 1, ZEDBSD_USYNC_PRIVATE);
}

static void
clear_loader_error(void)
{
	intptr_t value = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	struct zedbsd_rtld_tcb *tcb = raw_error(value) || value == 0 ? NULL :
	    (struct zedbsd_rtld_tcb *)(uintptr_t)value;

	if (tcb != NULL) {
		tcb->dlerror_pending = 0;
		tcb->dlerror_buf[0] = '\0';
	} else {
		loader_error_pending = 0;
		loader_error[0] = '\0';
	}
}

static void
set_loader_error(const char *message)
{
	size_t length = 0;
	intptr_t value = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	struct zedbsd_rtld_tcb *tcb = raw_error(value) || value == 0 ? NULL :
	    (struct zedbsd_rtld_tcb *)(uintptr_t)value;
	char *buffer = tcb != NULL ? tcb->dlerror_buf : loader_error;
	size_t capacity = tcb != NULL ? sizeof(tcb->dlerror_buf) :
	    sizeof(loader_error);

	if (message == NULL)
		message = "runtime linker error";
	while (message[length] != '\0' &&
	    length + 1U < capacity) {
		buffer[length] = message[length];
		length++;
	}
	buffer[length] = '\0';
	if (tcb != NULL)
		tcb->dlerror_pending = 1;
	else
		loader_error_pending = 1;
}

void
rtld_debug(const char *message)
{
	if (message != NULL)
		(void)syscall6(ZEDBSD_SYS_write, 2, (uintptr_t)message,
		    rtld_strlen(message), 0, 0, 0);
}

void
rtld_fatal(const char *message)
{
	rtld_debug("ld.so: ");
	rtld_debug(message != NULL ? message : "runtime linker failure");
	rtld_debug("\n");
	(void)syscall6(ZEDBSD_SYS_exit, 127, 0, 0, 0, 0, 0);
	for (;;) { }
}

static int
valid_elf_header(const Elf_Ehdr *header, int expected_type)
{
	if (header == NULL || header->e_ident[EI_MAG0] != ELFMAG0 ||
	    header->e_ident[EI_MAG1] != ELFMAG1 ||
	    header->e_ident[EI_MAG2] != ELFMAG2 ||
	    header->e_ident[EI_MAG3] != ELFMAG3 ||
	    header->e_ident[EI_CLASS] != ELF_CLASS ||
	    header->e_ident[EI_DATA] != RTLD_DATA ||
	    header->e_ident[EI_VERSION] != EV_CURRENT ||
	    header->e_type != expected_type || header->e_machine != RTLD_MACHINE ||
	    header->e_version != EV_CURRENT || header->e_ehsize != sizeof(*header) ||
	    header->e_phentsize != sizeof(Elf_Phdr) || header->e_phnum == 0 ||
	    header->e_phnum > 64)
		return 0;
	return 1;
}

static int
bootstrap_relative(uintptr_t base, const Elf_Phdr *phdr, unsigned phnum)
{
	Elf_Dyn *dynamic = NULL;
	size_t dynamic_count = 0, i;
	Elf_Rel *rel = NULL;
	size_t relsz = 0, relent = sizeof(Elf_Rel);
	Elf_Rela *rela = NULL;
	size_t relasz = 0, relaent = sizeof(Elf_Rela);

	for (i = 0; i < phnum; i++)
		if (phdr[i].p_type == PT_DYNAMIC) {
			if (dynamic != NULL || phdr[i].p_memsz < sizeof(Elf_Dyn))
				return -1;
			dynamic = (Elf_Dyn *)(base + (uintptr_t)phdr[i].p_vaddr);
			dynamic_count = (size_t)phdr[i].p_memsz / sizeof(Elf_Dyn);
		}
	if (dynamic == NULL)
		return -1;
	for (i = 0; i < dynamic_count; i++) {
		if (dynamic[i].d_tag == DT_NULL)
			break;
		switch ((int)dynamic[i].d_tag) {
		case DT_REL: rel = (Elf_Rel *)(base +
			(uintptr_t)dynamic[i].d_un.d_ptr); break;
		case DT_RELSZ: relsz = (size_t)dynamic[i].d_un.d_val; break;
		case DT_RELENT: relent = (size_t)dynamic[i].d_un.d_val; break;
		case DT_RELA: rela = (Elf_Rela *)(base +
			(uintptr_t)dynamic[i].d_un.d_ptr); break;
		case DT_RELASZ: relasz = (size_t)dynamic[i].d_un.d_val; break;
		case DT_RELAENT: relaent = (size_t)dynamic[i].d_un.d_val; break;
		default: break;
		}
	}
	if (i == dynamic_count || (rel != NULL && relent != sizeof(Elf_Rel)) ||
	    (rela != NULL && relaent != sizeof(Elf_Rela)) ||
	    relsz % sizeof(Elf_Rel) != 0 || relasz % sizeof(Elf_Rela) != 0)
		return -1;
	for (i = 0; i < relsz / sizeof(Elf_Rel); i++) {
		uintptr_t *where;
		if (ELF_R_TYPE(rel[i].r_info) != RTLD_RELATIVE ||
		    ELF_R_SYM(rel[i].r_info) != 0)
			return -1;
		where = (uintptr_t *)(base + (uintptr_t)rel[i].r_offset);
		*where += base;
	}
	for (i = 0; i < relasz / sizeof(Elf_Rela); i++) {
		uintptr_t *where;
		if (ELF_R_TYPE(rela[i].r_info) != RTLD_RELATIVE ||
		    ELF_R_SYM(rela[i].r_info) != 0)
			return -1;
		where = (uintptr_t *)(base + (uintptr_t)rela[i].r_offset);
		*where = base + (uintptr_t)rela[i].r_addend;
	}
	return 0;
}

static int
object_contains(const struct rtld_object *object, uintptr_t address,
	size_t size, uint32_t required)
{
	unsigned i;
	if (object == NULL || size == 0 || address > UINTPTR_MAX - size)
		return 0;
	for (i = 0; i < object->phnum; i++) {
		uintptr_t start, end;
		if (object->phdr[i].p_type != PT_LOAD ||
		    (object->phdr[i].p_flags & required) != required)
			continue;
		start = object->base + (uintptr_t)object->phdr[i].p_vaddr;
		if ((uintptr_t)object->phdr[i].p_memsz > UINTPTR_MAX - start)
			continue;
		end = start + (uintptr_t)object->phdr[i].p_memsz;
		if (address >= start && address + size <= end)
			return 1;
	}
	return 0;
}

static uintptr_t
object_pointer(const struct rtld_object *object, Elf_Addr value, size_t size,
	uint32_t required)
{
	uintptr_t address;
	if ((uintptr_t)value > UINTPTR_MAX - object->base)
		rtld_fatal("dynamic pointer overflow");
	address = object->base + (uintptr_t)value;
	if (!object_contains(object, address, size, required))
		rtld_fatal("dynamic pointer outside object");
	return address;
}

static size_t
object_readable_bytes(const struct rtld_object *object, Elf_Addr value)
{
	unsigned i;
	for (i = 0; i < object->phnum; i++) {
		Elf_Addr start, end;
		if (object->phdr[i].p_type != PT_LOAD ||
		    (object->phdr[i].p_flags & PF_R) == 0)
			continue;
		start = object->phdr[i].p_vaddr;
		if ((uint64_t)object->phdr[i].p_memsz >
		    (uint64_t)(~(Elf_Addr)0 - start))
			continue;
		end = start + (Elf_Addr)object->phdr[i].p_memsz;
		if (value >= start && value < end)
			return (size_t)(end - value);
	}
	return 0;
}

static void
copy_path(char destination[RTLD_PATH_MAX], const char *source)
{
	size_t length = rtld_strlen(source);
	if (length == 0 || length >= RTLD_PATH_MAX)
		rtld_fatal("invalid object path");
	rtld_memcpy(destination, source, length + 1U);
}

static struct rtld_object *
new_object(const char *path)
{
	struct rtld_object *object;
	unsigned i;

	for (i = 0; i < object_count; i++)
		if (!objects[i].active)
			break;
	if (i == object_count) {
		if (object_count == RTLD_OBJECT_MAX)
			rtld_fatal("too many shared objects");
		object_count++;
	}
	object = &objects[i];
	rtld_memset(object, 0, sizeof(*object));
	object->active = 1;
	object->generation = next_object_generation++;
	if (next_object_generation == 0)
		next_object_generation = 1;
	copy_path(object->path, path);
	return object;
}

static int
bounded_string(const char *string, size_t capacity, size_t *length_out)
{
	size_t length;
	for (length = 0; length < capacity; length++)
		if (string[length] == '\0') {
			if (length_out != NULL)
				*length_out = length;
			return 1;
		}
	return 0;
}

static const char *
dynamic_string(struct rtld_object *object, uint32_t offset)
{
	if (offset >= object->strsz ||
	    !bounded_string(object->strtab + offset,
	    object->strsz - offset, NULL))
		rtld_fatal("invalid version string");
	return object->strtab + offset;
}

static Elf_Addr
version_offset(Elf_Addr value, uint32_t offset)
{
	if ((Elf_Addr)offset > (Elf_Addr)~(Elf_Addr)0 - value)
		rtld_fatal("symbol version table overflow");
	return value + (Elf_Addr)offset;
}

static void
validate_verdef(struct rtld_object *object)
{
	Elf_Addr cursor = object->verdef_value;
	uint32_t record;

	for (record = 0; record < object->verdef_count; record++) {
		Elf_Verdef *definition = (Elf_Verdef *)object_pointer(object,
		    cursor, sizeof(*definition), PF_R);
		Elf_Addr auxiliary;
		uint16_t item;

		if (definition->vd_version != VER_DEF_CURRENT ||
		    definition->vd_ndx == VER_NDX_LOCAL ||
		    (definition->vd_ndx & VER_NDX_HIDDEN) != 0 ||
		    ((definition->vd_ndx == VER_NDX_GLOBAL) !=
		    ((definition->vd_flags & VER_FLG_BASE) != 0)) ||
		    definition->vd_cnt == 0 || definition->vd_aux == 0)
			rtld_fatal("invalid symbol version definition");
		auxiliary = version_offset(cursor, definition->vd_aux);
		for (item = 0; item < definition->vd_cnt; item++) {
			Elf_Verdaux *name = (Elf_Verdaux *)object_pointer(object,
			    auxiliary, sizeof(*name), PF_R);
			(void)dynamic_string(object, name->vda_name);
			if (item + 1U < definition->vd_cnt) {
				if (name->vda_next == 0)
					rtld_fatal("truncated symbol version definition");
				auxiliary = version_offset(auxiliary, name->vda_next);
			}
		}
		if (record + 1U < object->verdef_count) {
			if (definition->vd_next == 0)
				rtld_fatal("truncated symbol version definitions");
			cursor = version_offset(cursor, definition->vd_next);
		} else if (definition->vd_next != 0) {
			rtld_fatal("extra symbol version definitions");
		}
	}
}

static void
validate_verneed(struct rtld_object *object)
{
	Elf_Addr cursor = object->verneed_value;
	uint32_t record;

	for (record = 0; record < object->verneed_count; record++) {
		Elf_Verneed *need = (Elf_Verneed *)object_pointer(object,
		    cursor, sizeof(*need), PF_R);
		Elf_Addr auxiliary;
		uint16_t item;

		if (need->vn_version != VER_NEED_CURRENT || need->vn_cnt == 0 ||
		    need->vn_aux == 0)
			rtld_fatal("invalid symbol version requirement");
		(void)dynamic_string(object, need->vn_file);
		auxiliary = version_offset(cursor, need->vn_aux);
		for (item = 0; item < need->vn_cnt; item++) {
			Elf_Vernaux *name = (Elf_Vernaux *)object_pointer(object,
			    auxiliary, sizeof(*name), PF_R);
			if ((name->vna_other & VER_NDX_MASK) <= VER_NDX_GLOBAL)
				rtld_fatal("invalid required symbol version index");
			(void)dynamic_string(object, name->vna_name);
			if (item + 1U < need->vn_cnt) {
				if (name->vna_next == 0)
					rtld_fatal("truncated symbol version requirement");
				auxiliary = version_offset(auxiliary, name->vna_next);
			}
		}
		if (record + 1U < object->verneed_count) {
			if (need->vn_next == 0)
				rtld_fatal("truncated symbol version requirements");
			cursor = version_offset(cursor, need->vn_next);
		} else if (need->vn_next != 0) {
			rtld_fatal("extra symbol version requirements");
		}
	}
}

static void
register_tls_module(struct rtld_object *object)
{
	unsigned i, id;

	for (i = 0; i < object->phnum; i++)
		if (object->phdr[i].p_type == PT_TLS) {
			struct rtld_tls_module *module;
			size_t alignment = (size_t)object->phdr[i].p_align;

			if (object->tls_module_id != 0 ||
			    object->phdr[i].p_filesz > object->phdr[i].p_memsz ||
			    object->phdr[i].p_memsz == 0)
				rtld_fatal("invalid TLS segment");
			if (alignment == 0)
				alignment = 1;
			if ((alignment & (alignment - 1U)) != 0 ||
			    alignment > RTLD_PAGE_SIZE ||
			    object->phdr[i].p_memsz > RTLD_PAGE_SIZE)
				rtld_fatal("unsupported TLS alignment or size");
			for (id = 1; id <= tls_module_count; id++)
				if (!tls_modules[id].active)
					break;
			if (id > tls_module_count) {
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
			if (module->file_size != 0)
				module->init_image = (const void *)object_pointer(object,
				    object->phdr[i].p_vaddr, module->file_size, PF_R);
			module->active = 1;
			tls_generation++;
		}
}

static void
parse_dynamic(struct rtld_object *object)
{
	size_t i;
	size_t relsz = 0, relasz = 0, relent = sizeof(Elf_Rel);
	size_t relaent = sizeof(Elf_Rela), syment = sizeof(Elf_Sym);
	size_t init_array_size = 0, fini_array_size = 0;
	size_t preinit_array_size = 0;
	uintptr_t strtab_value = 0, symtab_value = 0, hash_value = 0;
	uintptr_t gnu_hash_value = 0;
	uintptr_t versym_value = 0, verdef_value = 0, verneed_value = 0;
	uintptr_t rpath_offset = UINTPTR_MAX, runpath_offset = UINTPTR_MAX;
	uintptr_t rel_value = 0, rela_value = 0, jmprel_value = 0;

	for (i = 0; i < object->phnum; i++)
		if (object->phdr[i].p_type == PT_DYNAMIC) {
			if (object->dynamic != NULL ||
			    object->phdr[i].p_memsz < sizeof(Elf_Dyn))
				rtld_fatal("invalid dynamic segment");
			object->dynamic = (Elf_Dyn *)object_pointer(object,
			    object->phdr[i].p_vaddr,
			    (size_t)object->phdr[i].p_memsz, PF_R);
			object->dynamic_count = (size_t)object->phdr[i].p_memsz /
			    sizeof(Elf_Dyn);
		}
	if (object->dynamic == NULL)
		rtld_fatal("missing dynamic segment");
	for (i = 0; i < object->dynamic_count; i++) {
		Elf_Dyn *dynamic = &object->dynamic[i];
		if (dynamic->d_tag == DT_NULL)
			break;
		switch ((int)dynamic->d_tag) {
		case DT_NEEDED:
			if (object->needed_count == RTLD_NEEDED_MAX)
				rtld_fatal("too many dependencies");
			object->needed_offset[object->needed_count++] =
			    (uint32_t)dynamic->d_un.d_val;
			break;
		case DT_HASH: hash_value = (uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_GNU_HASH:
			gnu_hash_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_STRTAB: strtab_value = (uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_STRSZ: object->strsz = (size_t)dynamic->d_un.d_val; break;
		case DT_SYMTAB: symtab_value = (uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_SYMENT: syment = (size_t)dynamic->d_un.d_val; break;
		case DT_VERSYM:
			versym_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_VERDEF:
			verdef_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_VERDEFNUM:
			if (dynamic->d_un.d_val > UINT32_MAX)
				rtld_fatal("too many symbol version definitions");
			object->verdef_count = (uint32_t)dynamic->d_un.d_val;
			break;
		case DT_VERNEED:
			verneed_value = (uintptr_t)dynamic->d_un.d_ptr;
			break;
		case DT_VERNEEDNUM:
			if (dynamic->d_un.d_val > UINT32_MAX)
				rtld_fatal("too many symbol version requirements");
			object->verneed_count = (uint32_t)dynamic->d_un.d_val;
			break;
		case DT_REL: rel_value = (uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_RELSZ: relsz = (size_t)dynamic->d_un.d_val; break;
		case DT_RELENT: relent = (size_t)dynamic->d_un.d_val; break;
		case DT_RELA: rela_value = (uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_RELASZ: relasz = (size_t)dynamic->d_un.d_val; break;
		case DT_RELAENT: relaent = (size_t)dynamic->d_un.d_val; break;
		case DT_JMPREL: jmprel_value = (uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_PLTRELSZ: object->jmprel_size =
			(size_t)dynamic->d_un.d_val; break;
		case DT_PLTREL: object->pltrel = (int)dynamic->d_un.d_val; break;
		case DT_INIT: object->init = object->base +
			(uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_FINI: object->fini = object->base +
			(uintptr_t)dynamic->d_un.d_ptr; break;
		case DT_INIT_ARRAY: object->init_array = (uintptr_t *)(object->base +
			(uintptr_t)dynamic->d_un.d_ptr); break;
		case DT_INIT_ARRAYSZ: init_array_size =
			(size_t)dynamic->d_un.d_val; break;
		case DT_FINI_ARRAY: object->fini_array = (uintptr_t *)(object->base +
			(uintptr_t)dynamic->d_un.d_ptr); break;
		case DT_FINI_ARRAYSZ: fini_array_size =
			(size_t)dynamic->d_un.d_val; break;
		case DT_PREINIT_ARRAY: object->preinit_array =
			(uintptr_t *)(object->base +
			(uintptr_t)dynamic->d_un.d_ptr); break;
		case DT_PREINIT_ARRAYSZ: preinit_array_size =
			(size_t)dynamic->d_un.d_val; break;
		case DT_RPATH:
			rpath_offset = (uintptr_t)dynamic->d_un.d_val;
			break;
		case DT_RUNPATH:
			runpath_offset = (uintptr_t)dynamic->d_un.d_val;
			break;
		case DT_TEXTREL:
			rtld_fatal("unsupported dynamic feature");
		default: break;
		}
	}
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
	object->strtab = (const char *)object_pointer(object,
	    (Elf_Addr)strtab_value, object->strsz, PF_R);
	object->symtab = (Elf_Sym *)object_pointer(object, (Elf_Addr)symtab_value,
	    sizeof(Elf_Sym), PF_R);
	if (hash_value != 0) {
		size_t hash_words;
		object->hash = (uint32_t *)object_pointer(object,
		    (Elf_Addr)hash_value, 2U * sizeof(uint32_t), PF_R);
		if (object->hash[0] == 0 || object->hash[1] == 0)
			rtld_fatal("invalid SysV hash");
		hash_words = 2U;
		if ((size_t)object->hash[0] > SIZE_MAX - hash_words)
			rtld_fatal("invalid SysV hash size");
		hash_words += object->hash[0];
		if ((size_t)object->hash[1] > SIZE_MAX - hash_words ||
		    sizeof(uint32_t) > SIZE_MAX / (hash_words + object->hash[1]))
			rtld_fatal("invalid SysV hash size");
		hash_words += object->hash[1];
		object->symbol_count = object->hash[1];
		(void)object_pointer(object, (Elf_Addr)hash_value,
		    hash_words * sizeof(uint32_t), PF_R);
	}
	if (gnu_hash_value != 0) {
		uint32_t *header;
		size_t bloom_bytes, bucket_bytes, chain_capacity, chain_bytes;
		uintptr_t offset;
		unsigned bucket_index;
		uint32_t gnu_symbol_count;

		header = (uint32_t *)object_pointer(object,
		    (Elf_Addr)gnu_hash_value, 4U * sizeof(uint32_t), PF_R);
		object->gnu_bucket_count = header[0];
		object->gnu_symbol_offset = header[1];
		object->gnu_bloom_count = header[2];
		object->gnu_bloom_shift = header[3];
		if (object->gnu_bucket_count == 0 || object->gnu_bloom_count == 0 ||
		    (object->gnu_bloom_count & (object->gnu_bloom_count - 1U)) != 0 ||
		    object->gnu_bloom_shift >= sizeof(Elf_Addr) * 8U ||
		    sizeof(Elf_Addr) > SIZE_MAX / object->gnu_bloom_count ||
		    sizeof(uint32_t) > SIZE_MAX / object->gnu_bucket_count)
			rtld_fatal("invalid GNU hash header");
		bloom_bytes = (size_t)object->gnu_bloom_count * sizeof(Elf_Addr);
		bucket_bytes = (size_t)object->gnu_bucket_count * sizeof(uint32_t);
		if (gnu_hash_value > UINTPTR_MAX - 4U * sizeof(uint32_t))
			rtld_fatal("GNU hash address overflow");
		offset = gnu_hash_value + 4U * sizeof(uint32_t);
		object->gnu_bloom = (Elf_Addr *)object_pointer(object,
		    (Elf_Addr)offset, bloom_bytes, PF_R);
		if (offset > UINTPTR_MAX - bloom_bytes)
			rtld_fatal("GNU hash address overflow");
		offset += bloom_bytes;
		object->gnu_bucket = (uint32_t *)object_pointer(object,
		    (Elf_Addr)offset, bucket_bytes, PF_R);
		if (offset > UINTPTR_MAX - bucket_bytes)
			rtld_fatal("GNU hash address overflow");
		offset += bucket_bytes;
		chain_bytes = object_readable_bytes(object, (Elf_Addr)offset);
		chain_capacity = chain_bytes / sizeof(uint32_t);
		if (chain_capacity != 0)
			object->gnu_chain = (uint32_t *)object_pointer(object,
			    (Elf_Addr)offset, sizeof(uint32_t), PF_R);
		gnu_symbol_count = object->gnu_symbol_offset;
		for (bucket_index = 0;
		    bucket_index < object->gnu_bucket_count; bucket_index++) {
			uint32_t symbol = object->gnu_bucket[bucket_index];
			size_t chain_index;
			if (symbol == 0)
				continue;
			if (symbol < object->gnu_symbol_offset)
				rtld_fatal("invalid GNU hash bucket");
			chain_index = (size_t)symbol - object->gnu_symbol_offset;
			for (;;) {
				if (chain_index >= chain_capacity)
					rtld_fatal("unterminated GNU hash chain");
				if ((object->gnu_chain[chain_index] & 1U) != 0)
					break;
				if (symbol == UINT32_MAX)
					rtld_fatal("unterminated GNU hash chain");
				symbol++;
				chain_index++;
			}
			if (symbol == UINT32_MAX)
				rtld_fatal("GNU hash symbol count overflow");
			if (symbol + 1U > gnu_symbol_count)
				gnu_symbol_count = symbol + 1U;
		}
		if (object->hash != NULL) {
			if (object->gnu_symbol_offset > object->symbol_count ||
			    gnu_symbol_count > object->symbol_count)
				rtld_fatal("GNU and SysV hash disagree");
		} else {
			if (gnu_symbol_count == 0)
				rtld_fatal("empty GNU symbol table");
			object->symbol_count = gnu_symbol_count;
		}
		chain_capacity = (size_t)object->symbol_count -
		    object->gnu_symbol_offset;
		if (chain_capacity > SIZE_MAX / sizeof(uint32_t))
			rtld_fatal("invalid GNU hash chain size");
		if (chain_capacity != 0)
			(void)object_pointer(object, (Elf_Addr)offset,
			    chain_capacity * sizeof(uint32_t), PF_R);
	}
	if (object->symbol_count == 0 ||
	    sizeof(Elf_Sym) > SIZE_MAX / object->symbol_count)
		rtld_fatal("invalid dynamic symbol count");
	(void)object_pointer(object, (Elf_Addr)symtab_value,
	    (size_t)object->symbol_count * sizeof(Elf_Sym), PF_R);
	if ((verdef_value == 0) != (object->verdef_count == 0) ||
	    (verneed_value == 0) != (object->verneed_count == 0) ||
	    ((verdef_value != 0 || verneed_value != 0) && versym_value == 0))
		rtld_fatal("incomplete symbol version tables");
	if (versym_value != 0) {
		if (sizeof(Elf_Versym) > SIZE_MAX / object->symbol_count)
			rtld_fatal("invalid symbol version table size");
		object->versym = (Elf_Versym *)object_pointer(object,
		    (Elf_Addr)versym_value,
		    (size_t)object->symbol_count * sizeof(Elf_Versym), PF_R);
	}
	if (verdef_value != 0) {
		object->verdef_value = (Elf_Addr)verdef_value;
		validate_verdef(object);
	}
	if (verneed_value != 0) {
		object->verneed_value = (Elf_Addr)verneed_value;
		validate_verneed(object);
	}
	if (rel_value != 0) {
		object->rel = (Elf_Rel *)object_pointer(object, (Elf_Addr)rel_value,
		    relsz, PF_R);
		object->rel_count = relsz / sizeof(Elf_Rel);
	}
	if (rela_value != 0) {
		object->rela = (Elf_Rela *)object_pointer(object,
		    (Elf_Addr)rela_value, relasz, PF_R);
		object->rela_count = relasz / sizeof(Elf_Rela);
	}
	if (jmprel_value != 0) {
		object->jmprel = (void *)object_pointer(object,
		    (Elf_Addr)jmprel_value, object->jmprel_size, PF_R);
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
	if (object->init_array != NULL)
		(void)object_pointer(object, (Elf_Addr)((uintptr_t)object->init_array -
		    object->base), init_array_size, PF_R);
	if (object->fini_array != NULL)
		(void)object_pointer(object, (Elf_Addr)((uintptr_t)object->fini_array -
		    object->base), fini_array_size, PF_R);
	if (object->preinit_array != NULL)
		(void)object_pointer(object,
		    (Elf_Addr)((uintptr_t)object->preinit_array - object->base),
		    preinit_array_size, PF_R);
	for (i = 0; i < object->needed_count; i++) {
		uint32_t offset = object->needed_offset[i];
		if (offset >= object->strsz || !bounded_string(object->strtab + offset,
		    object->strsz - offset, NULL))
			rtld_fatal("invalid dependency name");
	}
	if (rpath_offset != UINTPTR_MAX) {
		if (rpath_offset >= object->strsz ||
		    !bounded_string(object->strtab + rpath_offset,
		    object->strsz - rpath_offset, NULL))
			rtld_fatal("invalid RPATH");
		object->rpath = object->strtab + rpath_offset;
	}
	if (runpath_offset != UINTPTR_MAX) {
		if (runpath_offset >= object->strsz ||
		    !bounded_string(object->strtab + runpath_offset,
		    object->strsz - runpath_offset, NULL))
			rtld_fatal("invalid RUNPATH");
		object->runpath = object->strtab + runpath_offset;
	}
	register_tls_module(object);
}

static int
segment_prot(uint32_t flags)
{
	int prot = 0;
#if !defined(HAL_ARCH_SPARCV9)
	if ((flags & (PF_W | PF_X)) == (PF_W | PF_X))
		rtld_fatal("writable executable segment");
#endif
	if (flags & PF_R) prot |= PROT_READ;
	if (flags & PF_W) prot |= PROT_WRITE;
	if (flags & PF_X) prot |= PROT_EXEC;
	return prot;
}

static int
temporary_writable_plt(const Elf_Phdr *program)
{
#if defined(HAL_ARCH_SPARCV9)
	return program->p_flags == (PF_R | PF_W | PF_X) &&
	    program->p_filesz == program->p_memsz && program->p_memsz != 0 &&
	    program->p_memsz <= RTLD_PAGE_SIZE &&
	    ((uintptr_t)program->p_vaddr & (RTLD_PAGE_SIZE - 1U)) == 0 &&
	    ((uintptr_t)program->p_offset & (RTLD_PAGE_SIZE - 1U)) == 0;
#else
	(void)program;
	return 0;
#endif
}

static intptr_t
map_call(uintptr_t address, size_t size, int prot, int flags, int fd,
	uintptr_t offset)
{
	return syscall6(ZEDBSD_SYS_mmap, address, size, (uintptr_t)prot,
	    (uintptr_t)flags, (uintptr_t)fd, offset);
}

static void
remember_mapping(struct rtld_object *object, uintptr_t start, size_t size)
{
	if (object->mapping_count == 64)
		rtld_fatal("too many object mappings");
	object->mapping_start[object->mapping_count] = start;
	object->mapping_size[object->mapping_count++] = size;
}

static int
validate_file_programs(const Elf_Ehdr *header, const Elf_Phdr *phdr,
	off_t file_size)
{
	unsigned i, j, loads = 0, dynamics = 0;
	for (i = 0; i < header->e_phnum; i++) {
		uintptr_t start, end;
		if (phdr[i].p_type == PT_DYNAMIC)
			dynamics++;
		if (phdr[i].p_type != PT_LOAD)
			continue;
		if (phdr[i].p_memsz == 0) {
			if (phdr[i].p_filesz != 0)
				return -1;
			continue;
		}
		loads++;
		if (phdr[i].p_filesz > phdr[i].p_memsz ||
		    phdr[i].p_offset > (Elf_Off)file_size ||
		    phdr[i].p_filesz > (Elf_Off)file_size - phdr[i].p_offset ||
		    ((phdr[i].p_offset ^ phdr[i].p_vaddr) &
		    (RTLD_PAGE_SIZE - 1U)) != 0 ||
		    phdr[i].p_vaddr > (Elf_Addr)UINTPTR_MAX - phdr[i].p_memsz ||
		    (((phdr[i].p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) &&
		    !temporary_writable_plt(&phdr[i])))
			return -1;
		start = page_floor((uintptr_t)phdr[i].p_vaddr);
		if ((uintptr_t)(phdr[i].p_vaddr + phdr[i].p_memsz) >
		    UINTPTR_MAX - (RTLD_PAGE_SIZE - 1U))
			return -1;
		end = page_ceil((uintptr_t)(phdr[i].p_vaddr + phdr[i].p_memsz));
		for (j = 0; j < i; j++) {
			uintptr_t other_start, other_end;
			if (phdr[j].p_type != PT_LOAD)
				continue;
			other_start = page_floor((uintptr_t)phdr[j].p_vaddr);
			other_end = page_ceil((uintptr_t)(phdr[j].p_vaddr +
			    phdr[j].p_memsz));
			if (start < other_end && other_start < end)
				return -1;
		}
	}
	if (loads == 0 || dynamics != 1)
		return -1;
	return 0;
}

static int
preflight_dlopen_file(int fd)
{
	struct stat status;
	Elf_Ehdr header;
	Elf_Phdr phdr[64];
	intptr_t result;
	size_t phdr_size;

	result = syscall6(ZEDBSD_SYS_fstat, (uintptr_t)fd,
	    (uintptr_t)&status, 0, 0, 0, 0);
	if (raw_error(result) || status.st_size < (off_t)sizeof(header))
		return -1;
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd,
	    (uintptr_t)&header, sizeof(header), 0, 0, 0);
	if (result != (intptr_t)sizeof(header) ||
	    !valid_elf_header(&header, ET_DYN) ||
	    header.e_phoff > (Elf_Off)status.st_size ||
	    header.e_phnum > ((Elf_Off)status.st_size - header.e_phoff) /
	    sizeof(Elf_Phdr))
		return -1;
	phdr_size = (size_t)header.e_phnum * sizeof(Elf_Phdr);
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd, (uintptr_t)phdr,
	    phdr_size, (uintptr_t)header.e_phoff, 0, 0);
	if (result != (intptr_t)phdr_size ||
	    validate_file_programs(&header, phdr, status.st_size) != 0)
		return -1;
	return 0;
}

static void
map_one_segment(struct rtld_object *object, int fd, const Elf_Phdr *program,
	int choose_base)
{
	uintptr_t virtual_page = page_floor((uintptr_t)program->p_vaddr);
	uintptr_t page_delta = (uintptr_t)program->p_vaddr - virtual_page;
	uintptr_t file_bytes = page_delta + (uintptr_t)program->p_filesz;
	uintptr_t memory_bytes = page_delta + (uintptr_t)program->p_memsz;
	size_t file_map_size = program->p_filesz != 0 ?
	    (size_t)page_ceil(file_bytes) : 0;
	size_t memory_map_size = (size_t)page_ceil(memory_bytes);
	uintptr_t file_offset = page_floor((uintptr_t)program->p_offset);
	uintptr_t requested = choose_base ? 0 : object->base + virtual_page;
	int flags = MAP_PRIVATE | (choose_base ? 0 : MAP_FIXED_NOREPLACE);
	int final_prot = segment_prot(program->p_flags);
	int map_prot = final_prot;
	int need_zero = program->p_memsz > program->p_filesz;
	intptr_t mapped;
	if (temporary_writable_plt(program)) {
		final_prot = PROT_READ | PROT_WRITE;
		map_prot = final_prot;
	}

	if (need_zero && (map_prot & PROT_WRITE) == 0) {
		if (map_prot & PROT_EXEC)
			rtld_fatal("executable BSS segment is unsupported");
		map_prot |= PROT_WRITE;
	}
	if (file_map_size != 0) {
		mapped = map_call(requested, file_map_size, map_prot, flags, fd,
		    file_offset);
		if (raw_error(mapped))
			rtld_fatal("cannot map shared object segment");
	} else {
		mapped = map_call(requested, memory_map_size, map_prot,
		    flags | MAP_ANONYMOUS, -1, 0);
		if (raw_error(mapped))
			rtld_fatal("cannot map shared object BSS");
	}
	if (choose_base) {
		if ((uintptr_t)mapped < virtual_page)
			rtld_fatal("invalid shared object load bias");
		object->base = (uintptr_t)mapped - virtual_page;
	}
	remember_mapping(object, (uintptr_t)mapped,
	    file_map_size != 0 ? file_map_size : memory_map_size);
	if (file_map_size < memory_map_size) {
		uintptr_t anonymous = object->base + virtual_page + file_map_size;
		size_t anonymous_size = memory_map_size - file_map_size;
		mapped = map_call(anonymous, anonymous_size, map_prot,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (raw_error(mapped) || (uintptr_t)mapped != anonymous)
			rtld_fatal("cannot map shared object zero fill");
		remember_mapping(object, anonymous, anonymous_size);
	}
	if (need_zero && program->p_filesz != 0) {
		uintptr_t zero_start = object->base +
		    (uintptr_t)program->p_vaddr + (uintptr_t)program->p_filesz;
		uintptr_t zero_end = object->base +
		    (uintptr_t)program->p_vaddr + (uintptr_t)program->p_memsz;
		uintptr_t file_page_end = object->base + virtual_page + file_map_size;
		if (zero_end > file_page_end)
			zero_end = file_page_end;
		if (zero_end > zero_start)
			rtld_memset((void *)zero_start, 0, zero_end - zero_start);
	}
	if (map_prot != final_prot) {
		intptr_t result = syscall6(ZEDBSD_SYS_mprotect,
		    object->base + virtual_page, memory_map_size,
		    (uintptr_t)final_prot, 0, 0, 0);
		if (raw_error(result))
			rtld_fatal("cannot protect shared object segment");
	}
}

static struct rtld_object *
find_identity(const struct stat *status)
{
	unsigned i;
	for (i = 0; i < object_count; i++)
		if (objects[i].active && !objects[i].unloading &&
		    objects[i].has_identity && objects[i].device == status->st_dev &&
		    objects[i].inode == status->st_ino)
			return &objects[i];
	return NULL;
}

static intptr_t
open_search_candidate(const char *directory, size_t directory_length,
	const char *name, size_t name_length, char path[RTLD_PATH_MAX])
{
	if (directory_length == 0 || directory_length >= RTLD_PATH_MAX ||
	    name_length == 0 || directory_length > RTLD_PATH_MAX - name_length - 2U)
		return -1;
	rtld_memcpy(path, directory, directory_length);
	if (path[directory_length - 1U] != '/')
		path[directory_length++] = '/';
	rtld_memcpy(path + directory_length, name, name_length);
	path[directory_length + name_length] = '\0';
	return syscall6(ZEDBSD_SYS_open, (uintptr_t)path, O_RDONLY, 0, 0, 0, 0);
}

static intptr_t
open_search_list(const char *list, const struct rtld_object *owner,
	const char *name, size_t name_length, char path[RTLD_PATH_MAX])
{
	const char *component;

	if (list == NULL)
		return -1;
	component = list;
	for (;;) {
		const char *end = component;
		char directory[RTLD_PATH_MAX];
		size_t length;
		intptr_t fd = -1;

		while (*end != '\0' && *end != ':')
			end++;
		length = (size_t)(end - component);
		if (length != 0 && component[0] == '/') {
			if (length < sizeof(directory)) {
				rtld_memcpy(directory, component, length);
				directory[length] = '\0';
				fd = open_search_candidate(directory, length, name,
				    name_length, path);
			}
		} else if (length >= 7U && component[0] == '$' &&
		    component[1] == 'O' && component[2] == 'R' &&
		    component[3] == 'I' && component[4] == 'G' &&
		    component[5] == 'I' && component[6] == 'N' &&
		    (length == 7U || component[7] == '/') && owner != NULL &&
		    owner->path[0] == '/') {
			const char *slash = owner->path;
			const char *cursor;
			size_t origin_length, suffix_length = length - 7U;

			for (cursor = owner->path; *cursor != '\0'; cursor++)
				if (*cursor == '/')
					slash = cursor;
			origin_length = (size_t)(slash - owner->path);
			if (origin_length == 0)
				origin_length = 1;
			if (origin_length <= sizeof(directory) - suffix_length - 1U) {
				rtld_memcpy(directory, owner->path, origin_length);
				if (suffix_length != 0)
					rtld_memcpy(directory + origin_length,
					    component + 7U, suffix_length);
				length = origin_length + suffix_length;
				directory[length] = '\0';
				fd = open_search_candidate(directory, length, name,
				    name_length, path);
			}
		}
		if (!raw_error(fd))
			return fd;
		if (*end == '\0')
			break;
		component = end + 1;
	}
	return -1;
}

static intptr_t
open_dependency(const char *name, size_t name_length,
	const struct rtld_object *requester, char path[RTLD_PATH_MAX])
{
	const struct rtld_object *owner;
	intptr_t fd;

	if (requester != NULL && requester->runpath != NULL) {
		fd = open_search_list(requester->runpath, requester, name,
		    name_length, path);
		if (!raw_error(fd))
			return fd;
	} else {
		for (owner = requester; owner != NULL; owner = owner->loader_parent)
			if (owner->rpath != NULL) {
				fd = open_search_list(owner->rpath, owner, name,
				    name_length, path);
				if (!raw_error(fd))
					return fd;
			}
	}
	return open_search_candidate("/lib", 4U, name, name_length, path);
}

static struct rtld_object *load_object(const char *name,
	struct rtld_object *requester);

static void
load_dependencies(struct rtld_object *object)
{
	unsigned i;
	for (i = 0; i < object->needed_count; i++) {
		object->needed[i] = load_object(object->strtab +
		    object->needed_offset[i], object);
		object->needed[i]->dependency_refs++;
	}
}

static struct rtld_object *
load_object(const char *name, struct rtld_object *requester)
{
	char path[RTLD_PATH_MAX];
	struct stat status;
	Elf_Ehdr header;
	Elf_Phdr phdr[64];
	struct rtld_object *object, *existing;
	intptr_t fd, result;
	size_t length;
	unsigned i, first = 0;
	uintptr_t minimum = UINTPTR_MAX;

	if (name == NULL || name[0] == '\0')
		rtld_fatal("empty dependency name");
	length = rtld_strlen(name);
	if (length >= RTLD_NAME_MAX)
		rtld_fatal("dependency name too long");
	for (i = 0; i < length; i++)
		if (name[i] == '/')
			rtld_fatal("dependency path must be a bare name");
	fd = open_dependency(name, length, requester, path);
	if (raw_error(fd))
		rtld_fatal("cannot open dependency");
	result = syscall6(ZEDBSD_SYS_fstat, (uintptr_t)fd, (uintptr_t)&status,
	    0, 0, 0, 0);
	if (raw_error(result) || status.st_size < (off_t)sizeof(header))
		rtld_fatal("cannot stat dependency");
	existing = find_identity(&status);
	if (existing != NULL) {
		(void)syscall6(ZEDBSD_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
		return existing;
	}
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd, (uintptr_t)&header,
	    sizeof(header), 0, 0, 0);
	if (result != (intptr_t)sizeof(header) || !valid_elf_header(&header, ET_DYN) ||
	    header.e_phoff > (Elf_Off)status.st_size ||
	    header.e_phnum > ((Elf_Off)status.st_size - header.e_phoff) /
	    sizeof(Elf_Phdr))
		rtld_fatal("invalid dependency ELF header");
	result = syscall6(ZEDBSD_SYS_pread, (uintptr_t)fd, (uintptr_t)phdr,
	    (size_t)header.e_phnum * sizeof(Elf_Phdr),
	    (uintptr_t)header.e_phoff, 0, 0);
	if (result != (intptr_t)((size_t)header.e_phnum * sizeof(Elf_Phdr)))
		rtld_fatal("cannot read dependency headers");
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
	for (i = 0; i < object->phnum; i++)
		if (object->phdr[i].p_type == PT_LOAD &&
		    object->phdr[i].p_memsz != 0 &&
		    page_floor((uintptr_t)object->phdr[i].p_vaddr) < minimum) {
			minimum = page_floor((uintptr_t)object->phdr[i].p_vaddr);
			first = i;
		}
	map_one_segment(object, (int)fd, &object->phdr[first], 1);
	for (i = 0; i < object->phnum; i++)
		if (i != first && object->phdr[i].p_type == PT_LOAD &&
		    object->phdr[i].p_memsz != 0)
			map_one_segment(object, (int)fd, &object->phdr[i], 0);
	(void)syscall6(ZEDBSD_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
	parse_dynamic(object);
	load_dependencies(object);
	return object;
}

static uint32_t
elf_hash(const char *name)
{
	uint32_t hash = 0, high;
	while (*name != '\0') {
		hash = (hash << 4) + (unsigned char)*name++;
		high = hash & 0xf0000000U;
		if (high != 0) hash ^= high >> 24;
		hash &= ~high;
	}
	return hash;
}

static uint32_t
gnu_hash_name(const char *name)
{
	uint32_t hash = 5381U;
	while (*name != '\0')
		hash = hash * 33U + (unsigned char)*name++;
	return hash;
}

static const char *
defined_version_name(struct rtld_object *object, uint16_t version_index)
{
	Elf_Addr cursor = object->verdef_value;
	uint32_t record;

	for (record = 0; record < object->verdef_count; record++) {
		Elf_Verdef *definition = (Elf_Verdef *)object_pointer(object,
		    cursor, sizeof(*definition), PF_R);
		if ((definition->vd_ndx & VER_NDX_MASK) == version_index) {
			Elf_Addr auxiliary = version_offset(cursor,
			    definition->vd_aux);
			Elf_Verdaux *name = (Elf_Verdaux *)object_pointer(object,
			    auxiliary, sizeof(*name), PF_R);
			return dynamic_string(object, name->vda_name);
		}
		if (definition->vd_next == 0)
			break;
		cursor = version_offset(cursor, definition->vd_next);
	}
	return NULL;
}

static const char *
required_version_name(struct rtld_object *object, uint16_t version_index)
{
	Elf_Addr cursor = object->verneed_value;
	uint32_t record;

	for (record = 0; record < object->verneed_count; record++) {
		Elf_Verneed *need = (Elf_Verneed *)object_pointer(object,
		    cursor, sizeof(*need), PF_R);
		Elf_Addr auxiliary = version_offset(cursor, need->vn_aux);
		uint16_t item;

		for (item = 0; item < need->vn_cnt; item++) {
			Elf_Vernaux *name = (Elf_Vernaux *)object_pointer(object,
			    auxiliary, sizeof(*name), PF_R);
			if ((name->vna_other & VER_NDX_MASK) == version_index)
				return dynamic_string(object, name->vna_name);
			if (name->vna_next == 0)
				break;
			auxiliary = version_offset(auxiliary, name->vna_next);
		}
		if (need->vn_next == 0)
			break;
		cursor = version_offset(cursor, need->vn_next);
	}
	return NULL;
}

static const char *
relocation_version_name(struct rtld_object *object, uint32_t symbol_index)
{
	Elf_Sym *symbol;
	uint16_t index;
	const char *name;

	if (object->versym == NULL)
		return NULL;
	index = object->versym[symbol_index] & VER_NDX_MASK;
	if (index <= VER_NDX_GLOBAL)
		return NULL;
	symbol = &object->symtab[symbol_index];
	name = symbol->st_shndx == SHN_UNDEF ?
	    required_version_name(object, index) :
	    defined_version_name(object, index);
	if (name == NULL)
		rtld_fatal("unknown relocation symbol version");
	return name;
}

static int
symbol_version_matches(struct rtld_object *object, uint32_t symbol_index,
	const char *required_version)
{
	uint16_t raw, index;
	const char *provided;

	if (object->versym == NULL)
		return required_version == NULL;
	raw = object->versym[symbol_index];
	index = raw & VER_NDX_MASK;
	if (index <= VER_NDX_GLOBAL)
		return required_version == NULL;
	provided = defined_version_name(object, index);
	if (provided == NULL)
		rtld_fatal("unknown defined symbol version");
	if (required_version != NULL)
		return rtld_strcmp(provided, required_version) == 0;
	return (raw & VER_NDX_HIDDEN) == 0;
}

static Elf_Sym *
match_symbol(struct rtld_object *object, uint32_t index, const char *name,
	const char *required_version)
{
	Elf_Sym *symbol;
	const char *symbol_name;
	unsigned binding, visibility;

	if (index >= object->symbol_count)
		rtld_fatal("symbol index outside table");
	symbol = &object->symtab[index];
	if (symbol->st_name >= object->strsz)
		rtld_fatal("invalid symbol name");
	symbol_name = object->strtab + symbol->st_name;
	binding = ELF_ST_BIND(symbol->st_info);
	visibility = ELF_ST_VISIBILITY(symbol->st_other);
	if (symbol->st_shndx != SHN_UNDEF &&
	    (binding == STB_GLOBAL || binding == STB_WEAK) &&
	    visibility != STV_HIDDEN && rtld_strcmp(symbol_name, name) == 0 &&
	    symbol_version_matches(object, index, required_version))
		return symbol;
	return NULL;
}

static Elf_Sym *
lookup_gnu_hash(struct rtld_object *object, const char *name,
	const char *required_version)
{
	const unsigned word_bits = sizeof(Elf_Addr) * 8U;
	uint32_t hash = gnu_hash_name(name);
	Elf_Addr mask, bloom;
	uint32_t index;

	bloom = object->gnu_bloom[(hash / word_bits) &
	    (object->gnu_bloom_count - 1U)];
	mask = ((Elf_Addr)1U << (hash % word_bits)) |
	    ((Elf_Addr)1U << ((hash >> object->gnu_bloom_shift) % word_bits));
	if ((bloom & mask) != mask)
		return NULL;
	index = object->gnu_bucket[hash % object->gnu_bucket_count];
	if (index == 0)
		return NULL;
	for (;;) {
		uint32_t chain_hash;
		Elf_Sym *symbol;

		if (index < object->gnu_symbol_offset ||
		    index >= object->symbol_count)
			rtld_fatal("corrupt GNU hash chain");
		chain_hash = object->gnu_chain[index - object->gnu_symbol_offset];
		if ((chain_hash | 1U) == (hash | 1U)) {
			symbol = match_symbol(object, index, name, required_version);
			if (symbol != NULL)
				return symbol;
		}
		if ((chain_hash & 1U) != 0)
			return NULL;
		index++;
	}
}

static Elf_Sym *
lookup_in_object_version(struct rtld_object *object, const char *name,
	const char *required_version)
{
	uint32_t buckets, index, *bucket, *chain;
	unsigned traversed = 0;
	if (object == NULL || !object->active || object->unloading ||
	    (object->hash == NULL && object->gnu_bloom == NULL))
		return NULL;
	if (object->gnu_bloom != NULL)
		return lookup_gnu_hash(object, name, required_version);
	buckets = object->hash[0];
	bucket = object->hash + 2;
	chain = bucket + buckets;
	index = bucket[elf_hash(name) % buckets];
	while (index != STN_UNDEF && traversed++ < object->symbol_count) {
		Elf_Sym *symbol;
		if (index >= object->symbol_count)
			rtld_fatal("corrupt symbol hash chain");
		symbol = match_symbol(object, index, name, required_version);
		if (symbol != NULL)
			return symbol;
		index = chain[index];
	}
	if (traversed > object->symbol_count)
		rtld_fatal("cyclic symbol hash chain");
	return NULL;
}

static uintptr_t
symbol_value(struct rtld_object *object, const Elf_Sym *symbol)
{
	if (symbol->st_shndx == SHN_ABS)
		return (uintptr_t)symbol->st_value;
	if ((uintptr_t)symbol->st_value > UINTPTR_MAX - object->base)
		rtld_fatal("symbol address overflow");
	return object->base + (uintptr_t)symbol->st_value;
}

static int
reserved_loader_symbol(const char *name)
{
	static const char *const names[] = {
		"__tls_get_addr",
		"___tls_get_addr",
		"__zedbsd_rtld_abi_version",
		"__zedbsd_rtld_thread_alloc",
		"__zedbsd_rtld_thread_free",
		"__zedbsd_rtld_thread_attach",
		"__zedbsd_rtld_pthread_private",
		"__zedbsd_rtld_startup_init",
		"__zedbsd_rtld_fork_prepare",
		"__zedbsd_rtld_fork_parent",
		"__zedbsd_rtld_fork_child",
		"__zedbsd_rtld_dlopen",
		"__zedbsd_rtld_dlsym",
		"__zedbsd_rtld_dlvsym",
		"__zedbsd_rtld_dlclose",
		"__zedbsd_rtld_dlerror",
		"__zedbsd_rtld_process_fini"
	};
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (rtld_strcmp(name, names[i]) == 0)
			return 1;
	return 0;
}

static uintptr_t
lookup_symbol_version(const char *name, const char *required_version, int weak)
{
	Elf_Sym *symbol;
	unsigned i;
	if (reserved_loader_symbol(name)) {
		symbol = lookup_in_object_version(interpreter_object, name,
		    required_version);
		if (symbol != NULL)
			return symbol_value(interpreter_object, symbol);
		if (weak)
			return 0;
		rtld_fatal("missing private loader symbol");
	}
	symbol = lookup_in_object_version(main_object, name, required_version);
	if (symbol != NULL)
		return symbol_value(main_object, symbol);
	for (i = 0; i < object_count; i++) {
		struct rtld_object *object = &objects[i];
		if (!object->active || object->unloading ||
		    object == main_object || object == interpreter_object)
			continue;
		symbol = lookup_in_object_version(object, name, required_version);
		if (symbol != NULL)
			return symbol_value(object, symbol);
	}
	symbol = lookup_in_object_version(interpreter_object, name,
	    required_version);
	if (symbol != NULL)
		return symbol_value(interpreter_object, symbol);
	if (weak)
		return 0;
	rtld_fatal("undefined symbol");
}

static uintptr_t
resolve_relocation_symbol(struct rtld_object *object, uint32_t index)
{
	Elf_Sym *symbol;
	const char *name;
	unsigned binding;
	if (index == 0)
		return 0;
	if (index >= object->symbol_count)
		rtld_fatal("invalid relocation symbol");
	symbol = &object->symtab[index];
	if (symbol->st_name >= object->strsz)
		rtld_fatal("invalid relocation symbol name");
	binding = ELF_ST_BIND(symbol->st_info);
	if (symbol->st_shndx != SHN_UNDEF && binding == STB_LOCAL)
		return symbol_value(object, symbol);
	name = object->strtab + symbol->st_name;
	return lookup_symbol_version(name,
	    relocation_version_name(object, index), binding == STB_WEAK);
}

static Elf_Sym *
resolve_tls_symbol(struct rtld_object *object, uint32_t index,
	struct rtld_object **owner)
{
	Elf_Sym *symbol;
	const char *name, *version;
	unsigned binding, i;

	if (index == 0 || index >= object->symbol_count)
		rtld_fatal("invalid TLS relocation symbol");
	symbol = &object->symtab[index];
	if (symbol->st_name >= object->strsz)
		rtld_fatal("invalid TLS symbol name");
	binding = ELF_ST_BIND(symbol->st_info);
	if (symbol->st_shndx != SHN_UNDEF && binding == STB_LOCAL) {
		*owner = object;
		return symbol;
	}
	name = object->strtab + symbol->st_name;
	version = relocation_version_name(object, index);
	symbol = lookup_in_object_version(main_object, name, version);
	if (symbol != NULL) {
		*owner = main_object;
		return symbol;
	}
	for (i = 0; i < object_count; i++) {
		struct rtld_object *candidate = &objects[i];
		if (!candidate->active || candidate->unloading ||
		    candidate == main_object || candidate == interpreter_object)
			continue;
		symbol = lookup_in_object_version(candidate, name, version);
		if (symbol != NULL) {
			*owner = candidate;
			return symbol;
		}
	}
	if (binding == STB_WEAK)
		return NULL;
	rtld_fatal("undefined TLS symbol");
}

#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64)
extern uintptr_t __zedbsd_tlsdesc_resolver(void);

static void
install_tlsdesc(struct rtld_object *object, uintptr_t address,
	uint32_t symbol_index, uintptr_t addend)
{
	struct rtld_object *owner = object;
	struct rtld_tlsdesc *descriptor;
	struct zedbsd_tls_index *index;
	Elf_Sym *symbol = NULL;
	uintptr_t offset = addend;

	if (!object_contains(object, address, sizeof(*descriptor), PF_W))
		rtld_fatal("TLSDESC target is not writable");
	if (object->tlsdesc_count ==
	    sizeof(object->tlsdesc_argument) /
	    sizeof(object->tlsdesc_argument[0]))
		rtld_fatal("too many TLSDESC relocations");
	if (symbol_index != 0) {
		symbol = resolve_tls_symbol(object, symbol_index, &owner);
		if (symbol == NULL)
			rtld_fatal("weak TLSDESC symbol is unavailable");
		offset += (uintptr_t)symbol->st_value;
	}
	if (owner == NULL || owner->tls_module_id == 0 ||
	    offset >= tls_modules[owner->tls_module_id].memory_size)
		rtld_fatal("invalid TLSDESC module or offset");
	index = &object->tlsdesc_argument[object->tlsdesc_count++].index;
	index->module = owner->tls_module_id;
	index->offset = offset;
	descriptor = (struct rtld_tlsdesc *)address;
	descriptor->resolver = (uintptr_t)__zedbsd_tlsdesc_resolver;
	descriptor->argument = (uintptr_t)index;
}
#endif

#if defined(HAL_ARCH_SPARCV9)
static void
sparcv9_patch_jmp_slot(uint32_t *where, uintptr_t value)
{
	unsigned i;
	uint32_t instruction[8];

	/* GNU SPARC64 PLT entries are eight instructions (32 bytes). */
	instruction[0] = 0x03000000U | (uint32_t)((value >> 42) & 0x3fffffU);
	instruction[1] = 0x09000000U | (uint32_t)((value >> 10) & 0x3fffffU);
	instruction[2] = 0x82106000U | (uint32_t)((value >> 32) & 0x3ffU);
	instruction[3] = 0x83287020U;
	/* Match the canonical gas "setx value, %g4, %g1" expansion. */
	instruction[4] = 0x82004004U;
	instruction[5] = 0x82106000U | (uint32_t)(value & 0x3ffU);
	instruction[6] = 0x81c04000U;
	instruction[7] = 0x01000000U;
	for (i = 0; i < 8; i++)
		where[i] = instruction[i];
	__asm__ volatile("membar #Sync" : : : "memory");
	for (i = 0; i < 8; i++)
		__asm__ volatile("flush %0" : : "r"(&where[i]) : "memory");
	__asm__ volatile("membar #Sync" : : : "memory");
}
#endif

static void
apply_value(struct rtld_object *object, uintptr_t offset, uint32_t type,
	uint32_t symbol_index, uintptr_t addend, int is_rela)
{
	uintptr_t address, symbol = 0, value;
	uintptr_t *where;
	struct rtld_object *tls_owner = NULL;
	Elf_Sym *tls_symbol;

	/* NONE relocations have neither a target nor a symbol to validate. */
#if defined(HAL_ARCH_I386)
	if (type == R_386_NONE)
		return;
#elif defined(HAL_ARCH_AMD64)
	if (type == R_X86_64_NONE)
		return;
#elif defined(HAL_ARCH_ARM64)
	if (type == R_AARCH64_NONE)
		return;
#elif defined(HAL_ARCH_SPARCV9)
	if (type == R_SPARC_NONE)
		return;
#endif
	if (offset > UINTPTR_MAX - object->base)
		rtld_fatal("relocation target overflow");
	address = object->base + offset;
	if (!object_contains(object, address,
#if defined(HAL_ARCH_SPARCV9)
	    type == R_SPARC_JMP_SLOT ? 8U * sizeof(uint32_t) :
#endif
	    sizeof(uintptr_t), PF_W))
		rtld_fatal("relocation target is not writable");
	where = (uintptr_t *)address;
	if (!is_rela)
		addend = *where;

#if defined(HAL_ARCH_I386)
	switch (type) {
	case R_386_NONE: return;
	case R_386_RELATIVE:
		if (symbol_index != 0) rtld_fatal("invalid relative relocation");
		value = object->base + addend; break;
	case R_386_32:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend; break;
	case R_386_PC32:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend - address; break;
	case R_386_GLOB_DAT:
	case R_386_JMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index); break;
	case R_386_TLS_DTPMOD32:
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
			    &tls_owner);
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id; break;
	case R_386_TLS_DTPOFF32:
		tls_symbol = resolve_tls_symbol(object, symbol_index, &tls_owner);
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend; break;
	default: rtld_fatal("unsupported i386 relocation");
	}
#elif defined(HAL_ARCH_AMD64)
	switch (type) {
	case R_X86_64_NONE: return;
	case R_X86_64_RELATIVE:
		if (symbol_index != 0) rtld_fatal("invalid relative relocation");
		value = object->base + addend; break;
	case R_X86_64_64:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend; break;
	case R_X86_64_GLOB_DAT:
	case R_X86_64_JUMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index); break;
	case R_X86_64_DTPMOD64:
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
			    &tls_owner);
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id; break;
	case R_X86_64_DTPOFF64:
		tls_symbol = resolve_tls_symbol(object, symbol_index, &tls_owner);
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend; break;
	case R_X86_64_TLSDESC:
		install_tlsdesc(object, address, symbol_index, addend);
		return;
	default: rtld_fatal("unsupported amd64 relocation");
	}
#elif defined(HAL_ARCH_ARM64)
	switch (type) {
	case R_AARCH64_NONE: return;
	case R_AARCH64_RELATIVE:
		if (symbol_index != 0) rtld_fatal("invalid relative relocation");
		value = object->base + addend; break;
	case R_AARCH64_ABS64:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend; break;
	case R_AARCH64_GLOB_DAT:
	case R_AARCH64_JUMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index); break;
	case R_AARCH64_TLS_DTPMOD64:
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
			    &tls_owner);
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id; break;
	case R_AARCH64_TLS_DTPREL64:
		tls_symbol = resolve_tls_symbol(object, symbol_index, &tls_owner);
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend; break;
	case R_AARCH64_TLSDESC:
		install_tlsdesc(object, address, symbol_index, addend);
		return;
	default: rtld_fatal("unsupported aarch64 relocation");
	}
#elif defined(HAL_ARCH_SPARCV9)
	switch (type) {
	case R_SPARC_NONE: return;
	case R_SPARC_RELATIVE:
		if (symbol_index != 0) rtld_fatal("invalid relative relocation");
		value = object->base + addend; break;
	case R_SPARC_64:
		symbol = resolve_relocation_symbol(object, symbol_index);
		value = symbol + addend; break;
	case R_SPARC_GLOB_DAT:
		value = resolve_relocation_symbol(object, symbol_index); break;
	case R_SPARC_JMP_SLOT:
		value = resolve_relocation_symbol(object, symbol_index);
		sparcv9_patch_jmp_slot((uint32_t *)where, value);
		return;
	case R_SPARC_TLS_DTPMOD64:
		if (symbol_index == 0)
			tls_owner = object;
		else
			tls_symbol = resolve_tls_symbol(object, symbol_index,
			    &tls_owner);
		if (tls_owner == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS module is unavailable");
		value = tls_owner->tls_module_id; break;
	case R_SPARC_TLS_DTPOFF64:
		tls_symbol = resolve_tls_symbol(object, symbol_index, &tls_owner);
		if (tls_symbol == NULL || tls_owner->tls_module_id == 0)
			rtld_fatal("TLS symbol is unavailable");
		value = (uintptr_t)tls_symbol->st_value + addend; break;
	default: rtld_fatal("unsupported sparcv9 relocation");
	}
#endif
	*where = value;
}

static void
relocate_object(struct rtld_object *object)
{
	size_t i;
	if (object->relocated)
		return;
	if (object->relocating)
		return;
	object->relocating = 1;
	for (i = 0; i < object->needed_count; i++)
		relocate_object(object->needed[i]);
	for (i = 0; i < object->rel_count; i++) {
		uint32_t type = ELF_R_TYPE(object->rel[i].r_info);
		if (object->relative_done && type == RTLD_RELATIVE)
			continue;
		apply_value(object, (uintptr_t)object->rel[i].r_offset, type,
		    ELF_R_SYM(object->rel[i].r_info), 0, 0);
	}
	for (i = 0; i < object->rela_count; i++) {
		uint32_t type = ELF_R_TYPE(object->rela[i].r_info);
		if (object->relative_done && type == RTLD_RELATIVE)
			continue;
		apply_value(object, (uintptr_t)object->rela[i].r_offset, type,
		    ELF_R_SYM(object->rela[i].r_info),
		    (uintptr_t)object->rela[i].r_addend, 1);
	}
	if (object->jmprel != NULL && object->pltrel == DT_REL) {
		Elf_Rel *rel = object->jmprel;
		for (i = 0; i < object->jmprel_size / sizeof(*rel); i++)
			apply_value(object, (uintptr_t)rel[i].r_offset,
			    ELF_R_TYPE(rel[i].r_info), ELF_R_SYM(rel[i].r_info), 0, 0);
	} else if (object->jmprel != NULL && object->pltrel == DT_RELA) {
		Elf_Rela *rela = object->jmprel;
		for (i = 0; i < object->jmprel_size / sizeof(*rela); i++)
			apply_value(object, (uintptr_t)rela[i].r_offset,
			    ELF_R_TYPE(rela[i].r_info), ELF_R_SYM(rela[i].r_info),
			    (uintptr_t)rela[i].r_addend, 1);
	}
#if defined(HAL_ARCH_SPARCV9)
	/* SPARC PLT instructions are writable only while JMP_SLOT is applied. */
	for (i = 0; i < object->phnum; i++) {
		uintptr_t start;
		intptr_t result;

		if (!temporary_writable_plt(&object->phdr[i]))
			continue;
		start = object->base + (uintptr_t)object->phdr[i].p_vaddr;
		result = syscall6(ZEDBSD_SYS_mprotect, start, RTLD_PAGE_SIZE,
		    PROT_READ | PROT_EXEC, 0, 0, 0);
		if (raw_error(result))
			rtld_fatal("cannot seal SPARC PLT");
	}
#endif
	for (i = 0; i < object->phnum; i++) {
		uintptr_t start, end;
		intptr_t result;

		if (object->phdr[i].p_type != PT_GNU_RELRO ||
		    object->phdr[i].p_memsz == 0)
			continue;
		if (object->phdr[i].p_vaddr > (Elf_Addr)UINTPTR_MAX -
		    object->phdr[i].p_memsz)
			rtld_fatal("invalid GNU_RELRO range");
		(void)object_pointer(object, object->phdr[i].p_vaddr,
		    (size_t)object->phdr[i].p_memsz, PF_R);
		start = page_floor(object->base +
		    (uintptr_t)object->phdr[i].p_vaddr);
		end = page_ceil(object->base + (uintptr_t)object->phdr[i].p_vaddr +
		    (uintptr_t)object->phdr[i].p_memsz);
		result = syscall6(ZEDBSD_SYS_mprotect, start, end - start,
		    PROT_READ, 0, 0, 0);
		if (raw_error(result)) {
			rtld_debug("ld.so: GNU_RELRO mprotect failed for ");
			rtld_debug(object->path);
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

static void
initialize_object(struct rtld_object *object)
{
	size_t i;
	if (object == NULL || object->initialized || object == interpreter_object)
		return;
	if (object->initializing)
		return;
	object->initializing = 1;
	for (i = 0; i < object->needed_count; i++)
		initialize_object(object->needed[i]);
	object->initialized = 1;
	if (object->init != 0)
		((void (*)(void))object->init)();
	for (i = 0; i < object->init_count; i++)
		if (object->init_array[i] != 0)
			((void (*)(void))object->init_array[i])();
	if (initialization_count == RTLD_OBJECT_MAX)
		rtld_fatal("initialization order overflow");
	initialization_order[initialization_count++] = object;
	object->initializing = 0;
}

__attribute__((visibility("default"))) unsigned
__zedbsd_rtld_abi_version(void)
{
	return ZEDBSD_RTLD_ABI_VERSION;
}

static void *
tls_map(size_t size)
{
	intptr_t result;

	size = (size_t)page_ceil(size);
	result = map_call(0, size, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return raw_error(result) ? NULL : (void *)(uintptr_t)result;
}

static void
tls_unmap(void *address, size_t size)
{
	if (address != NULL)
		(void)syscall6(ZEDBSD_SYS_munmap, (uintptr_t)address,
		    page_ceil(size), 0, 0, 0, 0);
}

static void *
allocate_tls_block(const struct rtld_tls_module *module)
{
	void *block;

	if (module == NULL || !module->active || module->memory_size == 0)
		return NULL;
	block = tls_map(module->memory_size);
	if (block == NULL)
		return NULL;
	if (module->file_size != 0)
		rtld_memcpy(block, module->init_image, module->file_size);
	if (module->memory_size > module->file_size)
		rtld_memset((unsigned char *)block + module->file_size, 0,
		    module->memory_size - module->file_size);
	return block;
}

__attribute__((visibility("default"))) int
__zedbsd_rtld_thread_alloc(void *pthread_private,
	struct zedbsd_rtld_tcb **out)
{
	struct zedbsd_rtld_tcb *tcb;
	void **dtv;

	if (out == NULL)
		return -1;
	*out = NULL;
	tcb = tls_map(sizeof(*tcb));
	if (tcb == NULL)
		return -1;
	dtv = tls_map((RTLD_OBJECT_MAX + 1U) * sizeof(*dtv));
	if (dtv == NULL) {
		tls_unmap(tcb, sizeof(*tcb));
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
	return 0;
}

__attribute__((visibility("default"))) void
__zedbsd_rtld_thread_free(struct zedbsd_rtld_tcb *tcb)
{
	intptr_t current;
	uintptr_t id;
	struct zedbsd_rtld_tcb **link;

	if (tcb == NULL)
		return;
	current = syscall6(ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_GET_TLS,
	    0, 0, 0, 0, 0);
	if (!raw_error(current) && (uintptr_t)current == (uintptr_t)tcb)
		rtld_fatal("attempt to free current thread TLS");
	loader_lock();
	for (link = &rtld_threads; *link != NULL; link = &(*link)->rtld_next)
		if (*link == tcb) {
			*link = tcb->rtld_next;
			break;
		}
	loader_unlock();
	if (tcb->dtv != NULL)
		for (id = 1; id < tcb->dtv_count && id <= tls_module_count; id++)
			if (tcb->dtv[id] != NULL)
				tls_unmap(tcb->dtv[id], tls_modules[id].memory_size);
	tls_unmap(tcb->dtv,
	    (RTLD_OBJECT_MAX + 1U) * sizeof(*tcb->dtv));
	tcb->dtv = NULL;
	tls_unmap(tcb, sizeof(*tcb));
}

__attribute__((visibility("default"))) int
__zedbsd_rtld_thread_attach(void *pthread_private)
{
	intptr_t value = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	struct zedbsd_rtld_tcb *tcb;

	if (raw_error(value))
		return -1;
	if (value == 0) {
		if (__zedbsd_rtld_thread_alloc(pthread_private, &tcb) != 0)
			return -1;
		value = syscall6(ZEDBSD_SYS_thread_self,
		    ZEDBSD_THREAD_SELF_SET_TLS, (uintptr_t)tcb, 0, 0, 0, 0);
		if (raw_error(value)) {
			__zedbsd_rtld_thread_free(tcb);
			return -1;
		}
		return 0;
	}
	tcb = (struct zedbsd_rtld_tcb *)(uintptr_t)value;
	if (tcb->dtv == NULL || tcb->pthread_private != NULL)
		return -1;
	tcb->pthread_private = pthread_private;
	return 0;
}

__attribute__((visibility("default"))) void *
__zedbsd_rtld_pthread_private(void)
{
	intptr_t value = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	if (raw_error(value) || value == 0)
		return NULL;
	return ((struct zedbsd_rtld_tcb *)(uintptr_t)value)->pthread_private;
}

__attribute__((visibility("default"))) void *
__tls_get_addr(const struct zedbsd_tls_index *index)
{
	intptr_t value;
	struct zedbsd_rtld_tcb *tcb;
	struct rtld_tls_module *module;
	void *block;

	if (index == NULL || index->module == 0 ||
	    index->module > tls_module_count)
		rtld_fatal("invalid TLS index");
	value = syscall6(ZEDBSD_SYS_thread_self, ZEDBSD_THREAD_SELF_GET_TLS,
	    0, 0, 0, 0, 0);
	if (raw_error(value) || value == 0)
		rtld_fatal("thread has no TLS control block");
	tcb = (struct zedbsd_rtld_tcb *)(uintptr_t)value;
	module = &tls_modules[index->module];
	if (!module->active || index->offset >= module->memory_size ||
	    tcb->dtv == NULL || index->module >= tcb->dtv_count)
		rtld_fatal("invalid TLS module access");
	block = tcb->dtv[index->module];
	if (block == NULL) {
		block = allocate_tls_block(module);
		if (block == NULL)
			rtld_fatal("cannot allocate TLS block");
		tcb->dtv[index->module] = block;
	}
	tcb->dtv_generation = tls_generation;
	return (unsigned char *)block + index->offset;
}

#if defined(HAL_ARCH_AMD64) || defined(HAL_ARCH_ARM64)
__attribute__((visibility("hidden"))) uintptr_t
__zedbsd_tlsdesc_resolve(const struct rtld_tlsdesc *descriptor)
{
	const struct zedbsd_tls_index *index;
	intptr_t thread_pointer;
	void *address;

	if (descriptor == NULL || descriptor->argument == 0)
		rtld_fatal("invalid TLSDESC argument");
	index = (const struct zedbsd_tls_index *)descriptor->argument;
	address = __tls_get_addr(index);
	thread_pointer = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	if (raw_error(thread_pointer) || thread_pointer == 0)
		rtld_fatal("thread has no TLSDESC base");
	return (uintptr_t)address - (uintptr_t)thread_pointer;
}
#endif

#if defined(HAL_ARCH_I386)
__attribute__((visibility("default"), regparm(1))) void *
___tls_get_addr(const struct zedbsd_tls_index *index)
{
	return __tls_get_addr(index);
}
#endif

__attribute__((visibility("default"))) void
__zedbsd_rtld_fork_prepare(void)
{
	loader_lock();
}

__attribute__((visibility("default"))) void
__zedbsd_rtld_fork_parent(void)
{
	loader_unlock();
}

__attribute__((visibility("default"))) void
__zedbsd_rtld_fork_child(void)
{
	loader_lock_owner = current_tid();
	loader_lock_depth = 1;
	loader_lock_word = 1;
	loader_unlock();
}

__attribute__((visibility("default"))) void
__zedbsd_rtld_startup_init(void)
{
	size_t i;
	if (startup_initialized)
		return;
	startup_initialized = 1;
	for (i = 0; i < main_object->preinit_count; i++)
		if (main_object->preinit_array[i] != 0)
			((void (*)(void))main_object->preinit_array[i])();
	initialize_object(main_object);
}

__attribute__((visibility("default"))) void
__zedbsd_rtld_process_fini(void)
{
	if (process_finalized)
		return;
	process_finalized = 1;
	while (initialization_count != 0) {
		struct rtld_object *object = initialization_order[--initialization_count];
		size_t i = object->fini_count;
		while (i != 0) {
			i--;
			if (object->fini_array[i] != 0)
				((void (*)(void))object->fini_array[i])();
		}
		if (object->fini != 0)
			((void (*)(void))object->fini)();
	}
}

static struct rtld_handle *
allocate_handle(struct rtld_object *object, int main_scope)
{
	unsigned i;

	for (i = 0; i < RTLD_HANDLE_MAX; i++)
		if (!handles[i].active) {
			handles[i].magic = RTLD_HANDLE_MAGIC;
			handles[i].generation = next_handle_generation++;
			if (next_handle_generation == 0)
				next_handle_generation = 1;
			handles[i].object = object;
			handles[i].references = 1;
			handles[i].active = 1;
			handles[i].main_scope = (unsigned)main_scope;
			if (!main_scope)
				object->direct_refs++;
			return &handles[i];
		}
	return NULL;
}

static struct rtld_handle *
validate_handle(void *value)
{
	uintptr_t address = (uintptr_t)value;
	uintptr_t first = (uintptr_t)&handles[0];
	uintptr_t end = (uintptr_t)&handles[RTLD_HANDLE_MAX];
	struct rtld_handle *handle;

	if (address < first || address >= end ||
	    (address - first) % sizeof(handles[0]) != 0)
		return NULL;
	handle = (struct rtld_handle *)value;
	if (handle->magic != RTLD_HANDLE_MAGIC || !handle->active ||
	    handle->references == 0 || handle->generation == 0 ||
	    handle->object == NULL || !handle->object->active ||
	    handle->object->unloading)
		return NULL;
	return handle;
}

static uintptr_t
lookup_global_optional(const char *name, const char *version, int *found)
{
	Elf_Sym *symbol;
	unsigned i;

	*found = 0;
	symbol = lookup_in_object_version(main_object, name, version);
	if (symbol != NULL) {
		*found = 1;
		return symbol_value(main_object, symbol);
	}
	for (i = 0; i < object_count; i++) {
		struct rtld_object *object = &objects[i];
		if (!object->active || object->unloading ||
		    object == main_object || object == interpreter_object)
			continue;
		symbol = lookup_in_object_version(object, name, version);
		if (symbol != NULL) {
			*found = 1;
			return symbol_value(object, symbol);
		}
	}
	return 0;
}

static uintptr_t
lookup_handle_graph(struct rtld_object *object, const char *name,
	const char *version, uint32_t *visited, int *found)
{
	uintptr_t index;
	Elf_Sym *symbol;
	unsigned i;

	if (object < &objects[0] || object >= &objects[RTLD_OBJECT_MAX] ||
	    !object->active || object->unloading)
		return 0;
	index = (uintptr_t)(object - &objects[0]);
	if ((*visited & ((uint32_t)1U << index)) != 0)
		return 0;
	*visited |= (uint32_t)1U << index;
	symbol = lookup_in_object_version(object, name, version);
	if (symbol != NULL) {
		*found = 1;
		return symbol_value(object, symbol);
	}
	for (i = 0; i < object->needed_count; i++) {
		uintptr_t value = lookup_handle_graph(object->needed[i], name,
		    version, visited, found);
		if (*found)
			return value;
	}
	return 0;
}

static const char *
dlopen_bare_name(const char *path)
{
	const char *cursor;

	if (path == NULL || path[0] == '\0')
		return NULL;
	if (path[0] == '/') {
		if (path[1] != 'l' || path[2] != 'i' || path[3] != 'b' ||
		    path[4] != '/')
			return NULL;
		path += 5;
	}
	for (cursor = path; *cursor != '\0'; cursor++)
		if (*cursor == '/')
			return NULL;
	return path;
}

static void
remove_initialization_record(struct rtld_object *object)
{
	unsigned i;

	for (i = 0; i < initialization_count; i++)
		if (initialization_order[i] == object) {
			for (; i + 1U < initialization_count; i++)
				initialization_order[i] = initialization_order[i + 1U];
			initialization_order[--initialization_count] = NULL;
			break;
		}
}

static void
finalize_object_unlocked(struct rtld_object *object)
{
	size_t i;

	if (!object->initialized)
		return;
	object->initialized = 0;
	i = object->fini_count;
	while (i != 0) {
		i--;
		if (object->fini_array[i] != 0)
			((void (*)(void))object->fini_array[i])();
	}
	if (object->fini != 0)
		((void (*)(void))object->fini)();
}

/* Called with the recursive loader lock held; returns with it held. */
static void
unload_object_locked(struct rtld_object *object)
{
	struct zedbsd_rtld_tcb *tcb;
	struct rtld_object *dependencies[RTLD_NEEDED_MAX];
	struct rtld_tls_module *module = NULL;
	size_t tls_size = 0;
	unsigned i, dependency_count;

	if (object == NULL || !object->active || object->permanent ||
	    object->unloading || object->direct_refs != 0 ||
	    object->dependency_refs != 0)
		return;
	object->unloading = 1;
	remove_initialization_record(object);
	dependency_count = object->needed_count;
	for (i = 0; i < dependency_count; i++)
		dependencies[i] = object->needed[i];

	/* Application callbacks may recursively use the loader. */
	loader_unlock();
	finalize_object_unlocked(object);
	loader_lock();

	if (object->tls_module_id != 0) {
		module = &tls_modules[object->tls_module_id];
		tls_size = module->memory_size;
		for (tcb = rtld_threads; tcb != NULL; tcb = tcb->rtld_next)
			if (tcb->dtv != NULL &&
			    object->tls_module_id < tcb->dtv_count &&
			    tcb->dtv[object->tls_module_id] != NULL) {
				tls_unmap(tcb->dtv[object->tls_module_id], tls_size);
				tcb->dtv[object->tls_module_id] = NULL;
				tcb->dtv_generation = tls_generation + 1U;
			}
		module->active = 0;
		module->owner = NULL;
		module->init_image = NULL;
		tls_generation++;
	}
	for (i = object->mapping_count; i != 0; i--) {
		intptr_t result = syscall6(ZEDBSD_SYS_munmap,
		    object->mapping_start[i - 1U], object->mapping_size[i - 1U],
		    0, 0, 0, 0);
		if (raw_error(result))
			rtld_fatal("cannot unmap shared object");
	}
	for (i = 0; i < dependency_count; i++) {
		if (dependencies[i] == NULL ||
		    dependencies[i]->dependency_refs == 0)
			rtld_fatal("invalid shared-object dependency reference");
		dependencies[i]->dependency_refs--;
	}
	rtld_memset(object, 0, sizeof(*object));
	for (i = 0; i < dependency_count; i++)
		unload_object_locked(dependencies[i]);
}

__attribute__((visibility("default"))) void *
__zedbsd_rtld_dlopen(const char *path, int flags)
{
	struct rtld_object *object;
	struct rtld_handle *handle;
	const char *name;
	char full_path[RTLD_PATH_MAX];
	intptr_t fd;
	size_t length;
	unsigned i;

	clear_loader_error();
	if ((flags & ~(RTLD_LAZY | RTLD_NOW | RTLD_GLOBAL)) != 0 ||
	    ((flags & (RTLD_LAZY | RTLD_NOW)) != RTLD_LAZY &&
	    (flags & (RTLD_LAZY | RTLD_NOW)) != RTLD_NOW)) {
		set_loader_error("invalid dlopen flags");
		return NULL;
	}
	loader_lock();
	if (path == NULL) {
		handle = allocate_handle(main_object, 1);
		if (handle == NULL)
			set_loader_error("too many dynamic-loader handles");
		loader_unlock();
		return handle;
	}
	name = dlopen_bare_name(path);
	if (name == NULL || (length = rtld_strlen(name)) == 0 ||
	    length >= RTLD_NAME_MAX) {
		set_loader_error("invalid shared-object path");
		loader_unlock();
		return NULL;
	}
	for (i = 0; i < object_count; i++)
		if (objects[i].active && !objects[i].unloading &&
		    (rtld_strcmp(objects[i].path, path) == 0 ||
		    (objects[i].path[0] == '/' &&
		    rtld_strcmp(objects[i].path + 5, name) == 0))) {
			object = &objects[i];
			goto loaded;
		}
	rtld_memcpy(full_path, "/lib/", 5);
	rtld_memcpy(full_path + 5, name, length + 1U);
	fd = syscall6(ZEDBSD_SYS_open, (uintptr_t)full_path, O_RDONLY,
	    0, 0, 0, 0);
	if (raw_error(fd)) {
		set_loader_error("shared object not found");
		loader_unlock();
		return NULL;
	}
	if (preflight_dlopen_file((int)fd) != 0) {
		(void)syscall6(ZEDBSD_SYS_close, (uintptr_t)fd, 0, 0, 0, 0, 0);
		set_loader_error("invalid shared object");
		loader_unlock();
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
	if (handle == NULL) {
		set_loader_error("too many dynamic-loader handles");
		unload_object_locked(object);
	}
	loader_unlock();
	return handle;
}

static void *
rtld_dlsym_common(void *value, const char *name, const char *version)
{
	struct rtld_handle *handle;
	uintptr_t result = 0;
	uint32_t visited = 0;
	int found = 0;

	clear_loader_error();
	if (name == NULL || name[0] == '\0' || reserved_loader_symbol(name)) {
		set_loader_error("invalid symbol name");
		return NULL;
	}
	loader_lock();
	handle = validate_handle(value);
	if (handle == NULL) {
		set_loader_error("invalid dynamic-loader handle");
	} else if (handle->main_scope) {
		result = lookup_global_optional(name, version, &found);
	} else {
		result = lookup_handle_graph(handle->object, name, version, &visited,
		    &found);
	}
	if (handle != NULL && !found)
		set_loader_error("symbol not found");
	loader_unlock();
	return found ? (void *)result : NULL;
}

__attribute__((visibility("default"))) void *
__zedbsd_rtld_dlsym(void *value, const char *name)
{
	return rtld_dlsym_common(value, name, NULL);
}

__attribute__((visibility("default"))) void *
__zedbsd_rtld_dlvsym(void *value, const char *name, const char *version)
{
	if (version == NULL || version[0] == '\0') {
		clear_loader_error();
		set_loader_error("invalid symbol version");
		return NULL;
	}
	return rtld_dlsym_common(value, name, version);
}

__attribute__((visibility("default"))) int
__zedbsd_rtld_dlclose(void *value)
{
	struct rtld_handle *handle;
	struct rtld_object *object;
	unsigned main_scope;

	clear_loader_error();
	loader_lock();
	handle = validate_handle(value);
	if (handle == NULL) {
		set_loader_error("invalid dynamic-loader handle");
		loader_unlock();
		return -1;
	}
	object = handle->object;
	main_scope = handle->main_scope;
	if (--handle->references == 0) {
		handle->active = 0;
		handle->object = NULL;
		handle->main_scope = 0;
		if (!main_scope) {
			if (object->direct_refs == 0)
				rtld_fatal("invalid shared-object direct reference");
			object->direct_refs--;
			unload_object_locked(object);
		}
	}
	loader_unlock();
	return 0;
}

__attribute__((visibility("default"))) char *
__zedbsd_rtld_dlerror(void)
{
	intptr_t value = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_GET_TLS, 0, 0, 0, 0, 0);
	struct zedbsd_rtld_tcb *tcb = raw_error(value) || value == 0 ? NULL :
	    (struct zedbsd_rtld_tcb *)(uintptr_t)value;

	if (tcb != NULL) {
		if (!tcb->dlerror_pending)
			return NULL;
		tcb->dlerror_pending = 0;
		return tcb->dlerror_buf;
	}
	if (!loader_error_pending)
		return NULL;
	loader_error_pending = 0;
	return loader_error;
}

__attribute__((visibility("default")))
const struct zedbsd_rtld_exports __zedbsd_rtld_exports = {
	.abi_version = ZEDBSD_RTLD_ABI_VERSION,
	.struct_size = sizeof(struct zedbsd_rtld_exports),
	.startup_init = __zedbsd_rtld_startup_init,
	.process_fini = __zedbsd_rtld_process_fini,
	.dlopen = __zedbsd_rtld_dlopen,
	.dlsym = __zedbsd_rtld_dlsym,
	.dlvsym = __zedbsd_rtld_dlvsym,
	.dlclose = __zedbsd_rtld_dlclose,
	.dlerror = __zedbsd_rtld_dlerror,
	.thread_alloc = __zedbsd_rtld_thread_alloc,
	.thread_free = __zedbsd_rtld_thread_free,
	.thread_attach = __zedbsd_rtld_thread_attach,
	.pthread_private = __zedbsd_rtld_pthread_private,
	.fork_prepare = __zedbsd_rtld_fork_prepare,
	.fork_parent = __zedbsd_rtld_fork_parent,
	.fork_child = __zedbsd_rtld_fork_child,
	.tls_get_addr = __tls_get_addr,
};

static void
setup_premapped_object(struct rtld_object *object, uintptr_t base,
	const Elf_Phdr *phdr, unsigned phnum, int type)
{
	unsigned i;
	object->base = base;
	object->type = type;
	object->phnum = phnum;
	if (phnum == 0 || phnum > 64)
		rtld_fatal("invalid pre-mapped program headers");
	for (i = 0; i < phnum; i++)
		object->phdr[i] = phdr[i];
	parse_dynamic(object);
}

uintptr_t
rtld_main(uintptr_t *initial_stack)
{
	uintptr_t *cursor, *auxv;
	uintptr_t at_base = 0, at_phdr = 0, at_phnum = 0, at_phent = 0;
	uintptr_t at_entry = 0;
	uintptr_t main_base = 0;
	int main_type = ET_EXEC;
	Elf_Ehdr *self_header;
	Elf_Phdr *self_phdr;
	unsigned i;
	struct zedbsd_rtld_tcb *initial_tcb;
	intptr_t tls_result;
	if (initial_stack == NULL)
		rtld_fatal("missing initial stack");
	cursor = initial_stack + 1U + initial_stack[0] + 1U;
	while (*cursor++ != 0) { }
	auxv = cursor;
	for (i = 0; i < 64; i++, auxv += 2) {
		if (auxv[0] == AT_NULL)
			break;
		switch (auxv[0]) {
		case AT_BASE: at_base = auxv[1]; break;
		case AT_PHDR: at_phdr = auxv[1]; break;
		case AT_PHNUM: at_phnum = auxv[1]; break;
		case AT_PHENT: at_phent = auxv[1]; break;
		case AT_ENTRY: at_entry = auxv[1]; break;
		default: break;
		}
	}
	if (i == 64 || at_base == 0 || at_phdr == 0 || at_phnum == 0 ||
	    at_phnum > 64 || at_phent != sizeof(Elf_Phdr) || at_entry == 0)
		rtld_fatal("invalid ELF auxiliary vector");
	self_header = (Elf_Ehdr *)at_base;
	if (!valid_elf_header(self_header, ET_DYN))
		rtld_fatal("invalid interpreter ELF header");
	self_phdr = (Elf_Phdr *)(at_base + (uintptr_t)self_header->e_phoff);
	if (bootstrap_relative(at_base, self_phdr, self_header->e_phnum) != 0)
		rtld_fatal("interpreter bootstrap relocation failed");
	for (i = 0; i < at_phnum; i++)
		if (((Elf_Phdr *)at_phdr)[i].p_type == PT_PHDR) {
			uintptr_t phdr_vaddr =
			    (uintptr_t)((Elf_Phdr *)at_phdr)[i].p_vaddr;
			if (phdr_vaddr > at_phdr)
				rtld_fatal("invalid main program-header address");
			main_base = at_phdr - phdr_vaddr;
			break;
		}
	if (main_base != 0) {
		Elf_Ehdr *main_header = (Elf_Ehdr *)main_base;
		if (!valid_elf_header(main_header, ET_DYN))
			rtld_fatal("invalid PIE executable header");
		main_type = ET_DYN;
	}

	main_object = new_object("<main>");
	interpreter_object = new_object(RTLD_INTERP_PATH);
	setup_premapped_object(main_object, main_base, (Elf_Phdr *)at_phdr,
	    (unsigned)at_phnum, main_type);
	setup_premapped_object(interpreter_object, at_base,
	    self_phdr, self_header->e_phnum, ET_DYN);
	interpreter_object->relative_done = 1;
	load_dependencies(main_object);
	if (interpreter_object->needed_count != 0)
		rtld_fatal("interpreter must not have dependencies");
	for (i = 0; i < object_count; i++)
		if (objects[i].active)
			relocate_object(&objects[i]);
	/* The initial executable, interpreter, and DT_NEEDED closure remain
	 * mapped until process termination.  Only later dlopen() objects are
	 * candidates for physical unload. */
	for (i = 0; i < object_count; i++)
		if (objects[i].active)
			objects[i].permanent = 1;
	if (__zedbsd_rtld_thread_alloc(NULL, &initial_tcb) != 0)
		rtld_fatal("cannot allocate initial TLS");
	tls_result = syscall6(ZEDBSD_SYS_thread_self,
	    ZEDBSD_THREAD_SELF_SET_TLS, (uintptr_t)initial_tcb, 0, 0, 0, 0);
	if (raw_error(tls_result))
		rtld_fatal("cannot install initial TLS");
	return at_entry;
}
