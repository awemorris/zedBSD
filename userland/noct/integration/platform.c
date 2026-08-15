/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * zedBSD Noct target adapter
 * This will be removed after Noct is moved to userspace.
 */

#include "hal/hal.h"
#include "kern/env.h"
#include "kern/fs.h"
#include "kern/noct.h"
#include "kern/file.h"
#include "kern/vfs.h"
#include <userland/noct/integration/memory.h>
#include <userland/noct/integration/napi.h>
#include <userland/noct/integration/noct-m6-script.h>
#include <userland/noct/integration/platform.h>
#include "kern/namespace.h"
#include "libc/stdio-fs.h"

#include <noct/noct.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SCRIPT_HEAP_MIN (2U * 1024U * 1024U)

static const char embedded_source[] = ZEDBSD_NOCT_M6_SOURCE;
static const struct noct_beui_hal *target_beui_hal;
/* Host tests do not construct the kernel VFS; the strong runtime definition
 * in src/kern/vfs.c replaces this zeroed compatibility object. */
struct cwdinfo kern_cwdinfo __attribute__((weak));

struct target_context {
	struct cwdinfo *fs_context;
	struct zedbsd_filesystem *filesystem;
	struct zedbsd_namespace *namespace;
	zedbsd_noct_key_fn key_read;
	zedbsd_noct_key_fn key_poll;
	zedbsd_noct_clock_fn clock_second;
	void *key_context;
};

void
zedbsd_noct_set_beui_hal(const struct noct_beui_hal *hal)
{
	target_beui_hal = hal;
}

static void
console_string(const char *string)
{
	hal_cons_write(string);
}

static void
console_decimal(size_t value)
{
	char digits[11];
	unsigned count = 0;

	if (value == 0) {
		hal_cons_putc('0');
		return;
	}
	while (value != 0 && count < sizeof(digits)) {
		digits[count++] = (char)('0' + value % 10U);
		value /= 10U;
	}
	while (count != 0)
		hal_cons_putc((uint8_t)digits[--count]);
}

static size_t
console_writer(void *context, const char *bytes, size_t length)
{
	(void)context;
	hal_cons_write_n(bytes, (unsigned)length);
	return length;
}

static enum zedbsd_noct_repl_input_result
repl_read_line(void *context, int continuation, char *line, size_t capacity)
{
	struct target_context *target = context;
	size_t length = 0;

	if (target == NULL || target->key_read == NULL || line == NULL ||
	    capacity < 2U)
		return ZEDBSD_NOCT_REPL_INPUT_ERROR;
	console_string(continuation ? ". " : "> ");
	hal_cons_show_cursor(1);
	for (;;) {
		int key = target->key_read(target->key_context);

		if (key < 0)
			return ZEDBSD_NOCT_REPL_INPUT_ERROR;
		/* Native PC-98 input carries a modifier snapshot above the
		 * normalized key code for the Term adapter.  The line editor
		 * consumes characters, including the Ctrl-C code, only. */
		key &= 0x1ff;
		if (key > 0xff)
			continue;
		if (key == 0x03) {
			console_string("^C\n");
			line[0] = '\0';
			hal_cons_update_cursor();
			return ZEDBSD_NOCT_REPL_INPUT_EXIT;
		}
		if (key == '\r' || key == '\n') {
			hal_cons_putc('\n');
			line[length] = '\0';
			hal_cons_update_cursor();
			return ZEDBSD_NOCT_REPL_INPUT_LINE;
		}
		if (key == '\b' || key == 0x7f) {
			if (length != 0) {
				length--;
				hal_cons_putc('\b');
				hal_cons_update_cursor();
			}
			continue;
		}
		if (key >= 32 && key < 127 && length + 1U < capacity) {
			line[length++] = (char)key;
			hal_cons_putc((uint8_t)key);
			hal_cons_update_cursor();
		}
	}
}

static int
target_screen_clear(void *context)
{
	(void)context;
	hal_cons_clear();
	return 1;
}

static int
target_screen_clear_row(void *context, unsigned row)
{
	(void)context;
	if (row >= HAL_CONS_ROWS)
		return 0;
	hal_cons_clear_row(row);
	return 1;
}

static int
target_screen_put(void *context, unsigned row, unsigned column,
		  const char *text, uint8_t attribute)
{
	(void)context;
	return hal_cons_write_at_attr(row, column, text, attribute);
}

static int
target_screen_put_utf8(void *context, unsigned row, unsigned column,
		       const char *text, unsigned length, uint8_t attribute)
{
	(void)context;
	return hal_cons_write_n_at(row, column, text, length, attribute);
}

static int
target_screen_clear_to_eol(void *context, unsigned row, unsigned column)
{
	(void)context;
	return hal_cons_clear_to_eol_at(row, column);
}

static int
target_screen_set_cursor(void *context, unsigned row, unsigned column)
{
	(void)context;
	return hal_cons_set_cursor(row, column);
}

static int
target_screen_show_cursor(void *context, int visible)
{
	(void)context;
	hal_cons_show_cursor(visible);
	return 1;
}

static int
target_keyboard_read(void *context)
{
	struct target_context *target = context;

	return target->key_read != NULL ? target->key_read(target->key_context) :
		-1;
}

static int
target_keyboard_poll(void *context)
{
	struct target_context *target = context;

	return target->key_poll != NULL ? target->key_poll(target->key_context) :
		-1;
}

static int
target_clock_second(void *context)
{
	struct target_context *target = context;

	return target->clock_second != NULL ?
		target->clock_second(target->key_context) : -1;
}

static int
target_file_size(void *context, const char *path, uint32_t *size)
{
	struct target_context *target = context;
	struct file *file;
	struct zedbsd_file legacy;

	if (size == NULL)
		return 0;
	if (target->fs_context == NULL) {
		if ((target->namespace != NULL ?
		     zedbsd_namespace_open_result(target->namespace, path, &legacy) :
		     target->filesystem != NULL ?
		     zedbsd_fs_open_result(target->filesystem, path, &legacy) :
		     ZEDBSD_FS_NOT_FOUND) != ZEDBSD_FS_OK || legacy.size > UINT32_MAX)
			return 0;
		*size = (uint32_t)legacy.size;
		return 1;
	}
	if (
	    file_openat(target->fs_context, path, O_RDONLY, 0, &file) != 0)
		return 0;
	if (file->f_inode->i_size < 0) {
		(void)file_close(file);
		return 0;
	}
	*size = (uint32_t)file->f_inode->i_size;
	(void)file_close(file);
	return 1;
}

static int
target_file_read(void *context, const char *path, uint32_t offset,
		 void *buffer, uint32_t length)
{
	struct target_context *target = context;
	struct file *file;
	struct zedbsd_file legacy;
	int ok;
	if (target->fs_context == NULL)
		return (target->namespace != NULL ?
			zedbsd_namespace_open_result(target->namespace, path, &legacy) ==
			ZEDBSD_FS_OK : target->filesystem != NULL &&
			zedbsd_fs_open(target->filesystem, path, &legacy)) &&
		       zedbsd_file_read(&legacy, offset, buffer, length);
	if (
	    file_openat(target->fs_context, path, O_RDONLY, 0, &file) != 0)
		return 0;
	if (file_seek(file, offset, 0) < 0) {
		(void)file_close(file); return 0;
	}
	ok = file_read(file, buffer, length) == (ssize_t)length;
	(void)file_close(file);
	return ok;
}

static int
target_directory_read(void *context, const char *path, unsigned index,
		      struct zedbsd_noct_dirent *entry)
{
	struct target_context *target = context;
	struct file *directory;
	struct dirent filesystem_entry;
	size_t length;
	int eof = 0, error = 0;

	if (entry == NULL || path == NULL)
		return -1;
	if (target->fs_context == NULL) {
		struct zedbsd_dirent old;
		enum zedbsd_fs_result r = target->namespace != NULL ?
			zedbsd_namespace_readdir_result(target->namespace, path, index,
						      &old) :
			target->filesystem != NULL ?
			zedbsd_fs_readdir_result(target->filesystem, path, index, &old) :
			ZEDBSD_FS_INVALID_ARGUMENT;
		if (r == ZEDBSD_FS_NOT_FOUND) return 0;
		if (r != ZEDBSD_FS_OK) return -1;
		length = strnlen(old.name, sizeof(old.name));
		if (length >= sizeof(entry->name)) return -1;
		memcpy(entry->name, old.name, length + 1U);
		entry->size = old.size; entry->attributes = old.attributes;
		return 1;
	}
	if (file_openat(target->fs_context, path, O_RDONLY | O_DIRECTORY, 0,
			&directory) != 0)
		return -1;
	for (unsigned i = 0; i <= index; i++) {
		error = file_readdir(directory, &filesystem_entry, &eof);
		if (error != 0 || eof)
			break;
	}
	(void)file_close(directory);
	if (error != 0)
		return -1;
	if (eof)
		return 0;
	length = strnlen(filesystem_entry.d_name,
			 sizeof(filesystem_entry.d_name));
	if (length >= sizeof(entry->name))
		return -1;
	memcpy(entry->name, filesystem_entry.d_name, length);
	entry->name[length] = '\0';
	entry->size = 0;
	entry->attributes = filesystem_entry.d_type == INODE_DIR ? 0x10U : 0;
	return 1;
}

static void
make_services(struct zedbsd_noct_services *services,
	      struct target_context *context)
{
	memset(services, 0, sizeof(*services));
	services->context = context;
	services->beui = target_beui_hal;
	services->screen_clear = target_screen_clear;
	services->screen_clear_row = target_screen_clear_row;
	services->screen_put = target_screen_put;
	services->screen_put_utf8 = target_screen_put_utf8;
	services->screen_clear_to_eol = target_screen_clear_to_eol;
	services->screen_set_cursor = target_screen_set_cursor;
	services->screen_show_cursor = target_screen_show_cursor;
	services->keyboard_poll = target_keyboard_poll;
	services->keyboard_read = target_keyboard_read;
	services->clock_second = target_clock_second;
	services->file_size = target_file_size;
	services->file_read = target_file_read;
	services->directory_read = target_directory_read;
}

/* End of the resident vmunix image, from the linker script. */
extern char __kernel_vma_end[];
static struct zedbsd_noct_memory_profile selected_memory;
static int selected_memory_ready;

static int
select_memory_raw(struct zedbsd_noct_memory_profile *profile)
{
	uint32_t low_extended;
	uint32_t high_mib;
	/* The kernel end is a higher-half VMA.  Noct's arena accounting uses the
	 * corresponding physical range beginning at 1 MiB. */
	uint32_t high_end_physical =
		(uint32_t)(uintptr_t)__kernel_vma_end & 0x7fffffffU;
	uint32_t low_reserved = high_end_physical - 0x100000U;

	{
		size_t total = hal_pmem_get_total_size();
		low_extended = total > 0x00100000U ?
		    (uint32_t)(total - 0x00100000U) : 0;
		if (low_extended > 14U * 1024U * 1024U)
			low_extended = 14U * 1024U * 1024U;
		high_mib = total > 16U * 1024U * 1024U ?
		    (uint32_t)((total - 16U * 1024U * 1024U) /
		    (1024U * 1024U)) : 0;
	}
	return zedbsd_noct_select_memory(low_extended, high_mib, low_reserved,
					 profile);
}

int
zedbsd_noct_prepare_memory(void)
{
	if (selected_memory_ready)
		return 1;
	if (!select_memory_raw(&selected_memory))
		return 0;
	selected_memory_ready = 1;
	return 1;
}

static int
select_memory(struct zedbsd_noct_memory_profile *profile)
{
	if (profile == NULL || !zedbsd_noct_prepare_memory())
		return 0;
	*profile = selected_memory;
	return 1;
}

int
zedbsd_noct_run_embedded(unsigned repeat_count)
{
	struct zedbsd_noct_options options;
	struct zedbsd_noct_services services;
	struct target_context target = { 0 };
	struct hal_cons_state console_state;
	struct zedbsd_noct_result result;
	struct zedbsd_noct_memory_profile memory;
	unsigned iteration;

	if (repeat_count == 0 || repeat_count > 100U)
		return 0;
	hal_pc98_enable_high_memory();
	make_services(&services, &target);
	if (!select_memory(&memory)) {
		console_string("Noct: insufficient script arena\n");
		hal_cons_update_cursor();
		return 0;
	}
	options.arena = (void *)memory.arena_base;
	options.arena_size = memory.arena_size;
	options.fail_after = ZEDBSD_NOCT_NO_FAILURE;
	options.jit_enable = 1;
	options.jit_threshold = 1;
	options.write = console_writer;
	options.write_context = NULL;
	options.observe_jit_code = NULL;
	options.jit_context = NULL;
	options.services = &services;
	options.filesystem = NULL;
	options.environment = NULL;
	options.memory = &memory;
	hal_cons_save_state(&console_state);
	for (iteration = 0; iteration < repeat_count; iteration++) {
		if (!zedbsd_noct_run("<embedded>", embedded_source, &options,
				     &result)) {
			console_string("Noct M6 failed: ");
			console_string(zedbsd_noct_status_string(result.status));
			console_string("\n");
			hal_cons_update_cursor();
			hal_cons_restore_terminal(&console_state);
			return 0;
		}
		if (result.current_after_reset != 0 ||
		    result.jit_code_size != memory.jit_code_size ||
		    !result.jit_region_released) {
			console_string("Noct M6 cleanup/JIT check failed\n");
			hal_cons_update_cursor();
			hal_cons_restore_terminal(&console_state);
			return 0;
		}
		console_string("\n");
	}
	console_string("Noct M6 JIT PASS: runs=");
	console_decimal(repeat_count);
	console_string(" peak=");
	console_decimal(result.heap_peak);
	console_string(" bytes\n");
	hal_cons_restore_terminal(&console_state);
	return 1;
}

int
zedbsd_noct_run_file(struct zedbsd_namespace *namespace,
		     struct zedbsd_filesystem *filesystem,
		     struct zedbsd_environment *environment, const char *path,
		     int argc, char *const argv[], zedbsd_noct_key_fn key_read,
		     zedbsd_noct_key_fn key_poll,
		     zedbsd_noct_clock_fn clock_second, void *key_context)
{
	struct zedbsd_noct_options options;
	struct zedbsd_noct_services services;
	struct target_context target;
	struct hal_cons_state console_state;
	struct zedbsd_noct_result result;
	struct file *file;
	struct zedbsd_file legacy_file;
	struct zedbsd_noct_memory_profile memory;
	size_t source_area;
	size_t file_size;
	char *source;
	int ok, use_vfs = kern_cwdinfo.root.p_inode != NULL;

	if (path == NULL || path[0] == '\0')
		return 0;
	if ((use_vfs && file_openat(&kern_cwdinfo, path, O_RDONLY, 0, &file) != 0) ||
	    (!use_vfs && (filesystem == NULL ||
			 !zedbsd_fs_open(filesystem, path, &legacy_file)))) {
		console_string("Noct: file not found: ");
		console_string(path);
		console_string("\n");
		hal_cons_update_cursor();
		return 0;
	}
	file_size = use_vfs ? (size_t)file->f_inode->i_size :
		(size_t)legacy_file.size;
	hal_pc98_enable_high_memory();
	target.filesystem = filesystem;
	target.namespace = namespace;
	target.fs_context = use_vfs ? &kern_cwdinfo : NULL;
	target.key_read = key_read;
	target.key_poll = key_poll;
	target.clock_second = clock_second;
	target.key_context = key_context;
	make_services(&services, &target);
	if (!select_memory(&memory)) {
		console_string("Noct: insufficient script arena\n");
		hal_cons_update_cursor();
		return 0;
	}
	if (file_size > memory.source_max) {
		console_string("Noct: source exceeds memory profile limit: ");
		console_string(path);
		console_string("\n");
		hal_cons_update_cursor();
		return 0;
	}
	source_area = (file_size + 1U + 15U) & ~(size_t)15U;
	if (memory.arena_size < source_area + SCRIPT_HEAP_MIN) {
		console_string("Noct: insufficient script arena\n");
		hal_cons_update_cursor();
		return 0;
	}
	source = (char *)(memory.arena_base + memory.arena_size - source_area);
	if (file_size != 0 &&
	    (use_vfs ? file_read(file, source, file_size) != (ssize_t)file_size :
	     !zedbsd_file_read(&legacy_file, 0, source, (uint32_t)file_size))) {
		console_string("Noct: cannot read source: ");
		console_string(path);
		console_string("\n");
		hal_cons_update_cursor();
		return 0;
	}
	source[file_size] = '\0';

	options.arena = (void *)memory.arena_base;
	options.arena_size = memory.arena_size - source_area;
	options.fail_after = ZEDBSD_NOCT_NO_FAILURE;
	options.jit_enable = 1;
	options.jit_threshold = 1;
	options.write = console_writer;
	options.write_context = NULL;
	options.observe_jit_code = NULL;
	options.jit_context = NULL;
	options.services = &services;
	options.filesystem = filesystem;
	options.environment = environment;
	options.memory = &memory;
	if (use_vfs) zedbsd_stdio_set_context(&kern_cwdinfo);
	else zedbsd_stdio_set_namespace(namespace);
	hal_cons_save_state(&console_state);
	if (file_size >= sizeof(NOCT_BYTECODE_HEADER) - 1U &&
	    memcmp(source, NOCT_BYTECODE_HEADER,
		   sizeof(NOCT_BYTECODE_HEADER) - 1U) == 0)
		ok = zedbsd_noct_run_bytecode_args(path, (uint8_t *)source,
						 (uint32_t)file_size, argc, argv,
						 &options, &result);
	else
		ok = zedbsd_noct_run_args(path, source, argc, argv, &options,
					  &result);
	if (!ok) {
		console_string("Noct: ");
		console_string(zedbsd_noct_status_string(result.status));
		console_string("\n");
	} else if (result.script_status != 0) {
		console_string("Noct: script returned nonzero status\n");
		ok = 0;
	}
	hal_cons_restore_terminal(&console_state);
	zedbsd_stdio_set_namespace(NULL);
	if (use_vfs) (void)file_close(file);
	return ok;
}

int
zedbsd_noct_run_repl(struct zedbsd_namespace *namespace,
		     struct zedbsd_filesystem *filesystem,
		     struct zedbsd_environment *environment,
		     zedbsd_noct_key_fn key_read, zedbsd_noct_key_fn key_poll,
		     zedbsd_noct_clock_fn clock_second, void *key_context)
{
	struct zedbsd_noct_options options;
	struct zedbsd_noct_services services;
	struct target_context target;
	struct hal_cons_state console_state;
	struct zedbsd_noct_result result;
	struct zedbsd_noct_memory_profile memory;
	int ok;

	if (key_read == NULL)
		return 0;
	hal_pc98_enable_high_memory();
	target.filesystem = filesystem;
	target.namespace = namespace;
	target.fs_context = kern_cwdinfo.root.p_inode != NULL ?
		&kern_cwdinfo : NULL;
	target.key_read = key_read;
	target.key_poll = key_poll;
	target.clock_second = clock_second;
	target.key_context = key_context;
	make_services(&services, &target);
	if (!select_memory(&memory)) {
		console_string("Noct: insufficient script arena\n");
		hal_cons_update_cursor();
		return 0;
	}

	options.arena = (void *)memory.arena_base;
	options.arena_size = memory.arena_size;
	options.fail_after = ZEDBSD_NOCT_NO_FAILURE;
	options.jit_enable = 1;
	options.jit_threshold = 1;
	options.write = console_writer;
	options.write_context = NULL;
	options.observe_jit_code = NULL;
	options.jit_context = NULL;
	options.services = &services;
	options.filesystem = filesystem;
	options.environment = environment;
	options.memory = &memory;
	if (target.fs_context != NULL) zedbsd_stdio_set_context(target.fs_context);
	else zedbsd_stdio_set_namespace(namespace);
	hal_cons_save_state(&console_state);
	ok = zedbsd_noct_repl(&options, repl_read_line, &target, &result);
	if (!ok) {
		console_string("Noct: ");
		console_string(zedbsd_noct_status_string(result.status));
		console_string("\n");
	}
	hal_cons_restore_terminal(&console_state);
	zedbsd_stdio_set_namespace(NULL);
	return ok;
}
