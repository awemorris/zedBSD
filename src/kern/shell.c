/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * Interactive shell and applet/Noct command dispatch.
 * This will be removed.
 */
#include "kern/internal.h"
#include "kern/clock.h"
#include "kern/console-device.h"
#include "kern/platform.h"
#include "kern/file.h"
#include "kern/vfs.h"
#include "kern/exec.h"
#include "kern/process.h"
#include "kern/kmem.h"
#include "kern/swap.h"
#include "kern/swap-fat.h"
#include "kern/vm-reclaim.h"
#include "hal/hal.h"
#include <errno.h>
#include <string.h>

#define CFG_MAX 8192
static uint8_t sec[512], cfg[CFG_MAX];
int kern_noct_last_status;

static void
print_stat(const char *name, size_t value)
{
	puts(name);
	putc('=');
	dec((unsigned)value);
	putc('\n');
}

static void
print_vmstat(void)
{
	struct hal_memory_stats hal_stats;
	struct kern_memory_stats kern_stats;
	struct vm_reclaim_stats vm_stats;
	struct swap_backend *swap = swap_system_backend();
	uint32_t swap_total = 0, swap_free = 0;

	hal_memory_get_stats(&hal_stats);
	kern_memory_get_stats(&kern_stats);
	vm_reclaim_get_stats(&vm_stats);
	if (swap != NULL)
		(void)swap_get_stats(swap, &swap_total, &swap_free);
	print_stat("physical.total", hal_stats.physical_total);
	print_stat("physical.reserved", hal_stats.physical_reserved);
	print_stat("physical.allocated", hal_stats.physical_allocated);
	print_stat("physical.free", hal_stats.physical_free);
	print_stat("image", kern_stats.image_bytes);
	print_stat("heap.fixed", kern_stats.heap_fixed);
	print_stat("heap.current", kern_stats.heap_current);
	print_stat("heap.peak", kern_stats.heap_peak);
	print_stat("heap.largest-free", kern_stats.heap_largest_free);
	print_stat("heap.largest-failed", kern_stats.heap_largest_failed);
	print_stat("hal.tasks", hal_stats.task_count);
	print_stat("hal.task-stack-bytes", hal_stats.task_stack_bytes);
	print_stat("hal.spaces", hal_stats.space_count);
	print_stat("hal.page-tables", hal_stats.page_table_count);
	print_stat("vm.resident", vm_stats.resident);
	print_stat("vm.anonymous", vm_stats.anonymous_resident);
	print_stat("vm.file", vm_stats.file_resident);
	print_stat("vm.wired", vm_stats.wired);
	print_stat("vm.busy", vm_stats.busy);
	print_stat("vm.dirty", vm_stats.dirty);
	print_stat("vm.clean", vm_stats.clean);
	print_stat("vm.swapped", vm_stats.swapped);
	print_stat("vm.faults", vm_stats.faults);
	print_stat("vm.page-in", vm_stats.page_ins);
	print_stat("vm.page-out", vm_stats.page_outs);
	print_stat("vm.reclaims", vm_stats.reclaims);
	print_stat("vm.io-errors", vm_stats.io_errors);
	print_stat("swap.total", swap_total);
	print_stat("swap.free", swap_free);
	print_stat("swap.extents", swap_fat_extent_count());
}

void prompt(void)
{
	char cwd[ZEDBSD_PATH_MAX];
	if (fs_getcwd(&kern_cwdinfo, cwd, sizeof(cwd)) != 0)
		strcpy(cwd, "/");
	puts(cwd);
	puts(" $ ");
	update_cursor();
}
static uint32_t raw_key(void)
{
	/* A blocking read must never leave the hardware cursor stale or hidden. */
	update_cursor();
	return (uint32_t)console_input_read_event() & HAL_KEY_EVENT_KEY_MASK;
}
int key(void)
{
	return (int)raw_key();
}
static uint32_t applet_key(void)
{
	return (uint32_t)key();
}
int poll(void)
{
	{
		int event = console_input_poll_event();
		return event < 0 ? -1 : event & (int)HAL_KEY_EVENT_KEY_MASK;
	}
}

int noct_key_read(void *context)
{
	(void)context;
	return console_input_read_event();
}

int noct_key_poll(void *context)
{
	(void)context;
	return console_input_poll_event();
}

int noct_key_is_down(void *context, int key)
{
	(void)context;
	return hal_cons_key_state(key);
}

/* A BeUI-only program never reads the buffered Keyboard module, so the
 * type-ahead a held key fills has to be emptied as a BeUI side effect or
 * keys pressed during a game leak to the next buffered reader.  The BIOS
 * queue holds at most 16 entries; the bound keeps the drain finite
 * against a BIOS whose poll never reports an empty queue. */
void noct_key_drain(void *context)
{
	(void)context;
	hal_cons_drain_input();
}

/* Return seconds since the start of the current minute, or -1 for an
 * invalid BIOS result.  Stage 1 converts the BIOS BCD byte to binary before
 * returning it through the gateway.  INT 1Ch/AH=00h is available on the
 * early PC-9801 models for which a CPU-speed-dependent delay loop would be
 * least useful. */
int clock_second(void)
{
	return (int)((zedbsd_kernel_ticks() / 100U) % 60U);
}

int noct_clock_second(void *context)
{
	(void)context;
	return clock_second();
}

int line(char *b)
{
	unsigned n = 0;
	for (;;) {
		int k = key();
		if (k == 0x1b) {
			b[0] = 0;
			return -1;
		}
		if (k == '\r' || k == '\n') {
			putc('\n');
			update_cursor();
			b[n] = 0;
			return n;
		}
		if ((k == 8 || k == 0x7f) && n) {
			n--;
			putc('\b');
			update_cursor();
			continue;
		}
		if (k >= 32 && k < 127 && n < LINE_MAX - 1) {
			b[n++] = k;
			putc(k);
			update_cursor();
		}
	}
}
static int split(char *s, char **v, int max)
{
	int n = 0;
	while (*s) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || *s == '#' || *s == ';')
			break;
		if (n == max)
			break;
		v[n++] = s;
		if (*s == '\"') {
			v[n - 1] = ++s;
			while (*s && *s != '\"')
				s++;
		} else
			while (*s && *s != ' ' && *s != '\t')
				s++;
		if (*s)
			*s++ = 0;
	}
	return n;
}
static int number(const char *s)
{
	int n = 0;
	if (!*s)
		return -1;
	while (*s >= '0' && *s <= '9')
		n = n * 10 + *s++ - '0';
	return *s ? -1 : n;
}

static void listdev(uint8_t cls)
{
	for (unsigned i = 0; i < device_count; i++) {
		if (cls && devs[i].device_class != cls)
			continue;
		devname(i);
		puts(" BIOS ");
		hex8(devs[i].bios_id);
		if (devs[i].flags & ZEDBSD_DEV_HAS_GEOMETRY) {
			puts(" H/S ");
			dec(devs[i].heads);
			putc('/');
			dec(devs[i].sectors);
		}
		if (devs[i].bios_id == ho->boot_bios_id)
			puts(" boot");
		putc('\n');
	}
}
static int selectdisk(const char *c, const char *n)
{
	int ix = number(n);
	uint8_t cls = streq(c, "fd")     ? 1
	              : streq(c, "ide")  ? 2
	              : streq(c, "scsi") ? 3
	                                 : 0;
	if (!cls || ix < 0)
		return 0;
	for (unsigned i = 0; i < device_count; i++)
		if (devs[i].device_class == cls &&
		    devs[i].display_index == ix) {
			curdev = i;
			curpart = -1;
			scanparts(i);
			return 1;
		}
	return 0;
}
static int selectpart(const char *s)
{
	if (curdev < 0)
		return 0;
	int n = number(s);
	for (int i = 0; i < MAX_PARTS; i++)
		if (parts[i].valid && ((n >= 0 && i == n) ||
		                       (n < 0 && streq(parts[i].name, s)))) {
			if (!mountpart(curdev, i))
				return 0;
			curpart = i;
			select_disk_home(curdev);
			return 1;
		}
	return 0;
}

/* Filesystem-facing shell commands and extension-module loaders. */
static int vfs_ls(const char *path)
{
	struct file *directory;
	int error = file_openat(&kern_cwdinfo, path,
				O_RDONLY | O_DIRECTORY, 0, &directory);
	if (error != 0)
		return 0;
	for (;;) {
		struct dirent entry;
		int eof;
		error = file_readdir(directory, &entry, &eof);
		if (error != 0 || eof)
			break;
		puts(entry.d_name);
		putc('\n');
	}
	(void)file_close(directory);
	return error == 0;
}
static int catfile(const char *n)
{
	struct file *file;
	ssize_t count;
	if (file_openat(&kern_cwdinfo, n, O_RDONLY, 0, &file) != 0)
		return 0;
	while ((count = file_read(file, sec, sizeof(sec))) > 0)
		for (ssize_t i = 0; i < count; i++)
			putc(sec[i]);
	(void)file_close(file);
	return count == 0;
}
static uint32_t crc32_image(const uint8_t *p, uint32_t n)
{
	uint32_t c = 0xffffffff;
	for (uint32_t i = 0; i < n; i++) {
		uint8_t b = (i >= 16 && i < 20) ? 0 : p[i];
		c ^= b;
		for (int j = 0; j < 8; j++)
			c = (c >> 1) ^ ((0 - (c & 1)) & 0xedb88320);
	}
	return ~c;
}
static int run_applet(const char *n, int argc, char **argv)
{
	struct file *file;
	uint8_t *image = (uint8_t *)0x50000;
	off_t size;
	if (file_openat(&kern_cwdinfo, n, O_RDONLY, 0, &file) != 0)
		return 0;
	size = file->f_inode->i_size;
	if (size < (off_t)sizeof(struct zedbsd_applet_header) ||
	    size > 0x10000 || file_read(file, image, (size_t)size) != size) {
		(void)file_close(file);
		return 0;
	}
	(void)file_close(file);
	struct zedbsd_applet_header *h = (struct zedbsd_applet_header *)image;
	if (h->magic != ZEDBSD_APPLET_MAGIC || h->abi_version != 1 ||
	    h->header_size != sizeof(*h) || h->image_size != (uint32_t)size ||
	    h->entry_offset < h->header_size || h->entry_offset >= (uint32_t)size ||
	    crc32_image(image, (uint32_t)size) != h->crc32)
		return 0;
	struct zedbsd_applet_services s = {1, sizeof(s), putc, puts,
	                                   applet_key};
	zedbsd_applet_entry_t entry =
	        (zedbsd_applet_entry_t)(image + h->entry_offset);
	uint32_t r = entry(&s, (uint32_t)argc, (const char *const *)argv);
	if (r) {
		puts("applet status ");
		dec(r);
		putc('\n');
		return 0;
	}
	return 1;
}
static int open_noct_application(const char *prefix, const char *name,
				 const char *extension,
				 char path[ZEDBSD_PATH_MAX])
{
	struct zedbsd_file file;
	unsigned source = 0;
	unsigned position = 0;
	unsigned base_length = 0;
	unsigned extension_length = 0;

	while (prefix[source] != '\0') {
		if (position + 1U >= ZEDBSD_PATH_MAX)
			return 0;
		path[position++] = prefix[source++];
	}
	while (name[base_length] != '\0') {
		char ch = name[base_length++];

		if (ch == '.' || ch == '/' || ch == '\\' ||
		    position + 1U >= ZEDBSD_PATH_MAX)
			return 0;
		path[position++] = ch >= 'a' && ch <= 'z' ?
			(char)(ch - 'a' + 'A') : ch;
	}
	if (!base_length)
		return 0;
	while (extension[extension_length] != '\0') {
		if (position + 1U >= ZEDBSD_PATH_MAX)
			return 0;
		path[position++] = extension[extension_length++];
	}
	path[position] = '\0';
	return zedbsd_fs_open(&mounted_fs, path, &file) ? 1 : 0;
}

/* Execute one already-tokenized shell command against the current state.
 * Source commands resolve in CMD/ with a root fallback for pre-CMD BOOT
 * volumes.  Unqualified names then fall back to precompiled NAME.NAP
 * bytecode in APPS/ (then CMD/ and the root), so applications too large
 * for the small-memory source compiler still launch as shell commands. */
int run_noct_user(const char *path, int argc, char *const argv[],
		  unsigned flags, char *result, size_t result_capacity)
{
	char executable[ZEDBSD_PATH_MAX];
	char home[ZEDBSD_PATH_MAX + 6U];
	char dictionary[ZEDBSD_PATH_MAX + 17U];
	char *child_argv[23];
	char *child_env[4];
	struct process *child;
	int status, error;
	unsigned disk;
	const char *value;

	if (path == NULL || argc < 0 || argc > 20 ||
	    (argc != 0 && argv == NULL)) {
		kern_noct_last_status = -EINVAL;
		return 0;
	}
	executable[0] = '\0';
	for (disk = 1; disk <= 99U; disk++) {
		struct inode *inode;
		unsigned at = 5U;
		memcpy(executable, "/disk", 5);
		if (disk >= 10U)
			executable[at++] = (char)('0' + disk / 10U);
		executable[at++] = (char)('0' + disk % 10U);
		memcpy(executable + at, "/NOCT.ELF", 10);
		if (namei_at(&kern_cwdinfo, executable, &inode) == 0) {
			inode_release(inode);
			break;
		}
		executable[0] = '\0';
	}
	if (executable[0] == '\0') {
		kern_noct_last_status = -ENOENT;
		return 0;
	}
	child_argv[0] = executable;
	child_argv[1] = (char *)path;
	for (int i = 0; i < argc; i++)
		child_argv[i + 2] = argv[i];
	child_argv[argc + 2] = NULL;
	memset(child_env, 0, sizeof(child_env));
	value = zedbsd_env_get(&boot_environment, "HOME");
	if (value != NULL && strlen(value) + 6U <= sizeof(home)) {
		memcpy(home, "HOME=", 5);
		strcpy(home + 5, value);
		child_env[0] = home;
	}
	value = zedbsd_env_get(&boot_environment, "REMACS_SKK_DICT");
	if (value != NULL && strlen(value) + 17U <= sizeof(dictionary)) {
		unsigned slot = child_env[0] != NULL ? 1U : 0U;
		memcpy(dictionary, "REMACS_SKK_DICT=", 16);
		strcpy(dictionary + 16, value);
		child_env[slot] = dictionary;
	}
	if ((flags & PROCESS_SPAWN_RESULT) != 0) {
		unsigned slot = child_env[0] == NULL ? 0U :
			(child_env[1] == NULL ? 1U : 2U);
		child_env[slot] = "ZEDBSD_RESULT_FD=3";
	}
	error = process_spawn(executable, child_argv, child_env, flags, &child);
	if (error != 0) {
		kern_noct_last_status = -error;
		return 0;
	}
	error = process_wait(child, &status, result, result_capacity);
	kern_noct_last_status = error != 0 ? -error : status;
	return error == 0 && status == 0;
}

static int run_noct_application(const char *name, const char *extension,
				int nap_fallback, int argc,
				char *const argv[])
{
	char path[ZEDBSD_PATH_MAX];

	if (!open_noct_application("CMD/", name, extension, path) &&
	    !open_noct_application("", name, extension, path) &&
	    (!nap_fallback ||
	     (!open_noct_application("APPS/", name, ".NAP", path) &&
	      !open_noct_application("CMD/", name, ".NAP", path) &&
	      !open_noct_application("", name, ".NAP", path))))
		return 0;
	return run_noct_user(path, argc, argv, 0, NULL, 0);
}

int command(char *s)
{
	char *v[20];
	int n = split(s, v, 20);
	if (!n)
		return 1;
#ifdef ZEDBSD_M9_WRITE_TEST
	if (streq(v[0], "m9-write-test")) {
		int lba = n == 2 ? number(v[1]) : -1;

		return lba >= 0 && m9_write_test((uint32_t)lba);
	}
#endif
	if (streq(v[0], "help")) {
		puts("help echo env set unset pause wait device probe-ide probe-scsi "
		     "disk part pwd cd ls cat source "
		     "run noct emacs vmstat reboot halt\n");
		return 1;
	}
	if (streq(v[0], "vmstat")) {
		if (n != 1)
			return 0;
		print_vmstat();
		return 1;
	}
	if (streq(v[0], "env")) {
		if (n != 1)
			return 0;
		for (size_t index = 0;
		     index < zedbsd_env_count(&boot_environment); index++) {
			const char *name;
			const char *value;

			if (!zedbsd_env_at(&boot_environment, index, &name, &value))
				return 0;
			puts(name);
			putc('=');
			puts(value);
			putc('\n');
		}
		return 1;
	}
	if (streq(v[0], "set"))
		return n == 3 &&
		       zedbsd_env_set(&boot_environment, v[1], v[2]);
	if (streq(v[0], "unset")) {
		if (n != 2 || !zedbsd_env_name_valid(v[1]))
			return 0;
		(void)zedbsd_env_unset(&boot_environment, v[1]);
		return 1;
	}
	if (streq(v[0], "echo")) {
		for (int i = 1; i < n; i++) {
			if (i > 1)
				putc(' ');
			puts(v[i]);
		}
		putc('\n');
		return 1;
	}
	if (streq(v[0], "pause")) {
		for (int i = 1; i < n; i++) {
			puts(v[i]);
			putc(' ');
		}
		key();
		return 1;
	}
	if (streq(v[0], "wait")) {
		unsigned loops = n > 1 ? (unsigned)number(v[1]) * 50000 : 50000;
		while (loops-- && poll() < 0)
			;
		return 1;
	}
	if (streq(v[0], "device")) {
		listdev(0);
		puts("boot -> BIOS ");
		hex8(ho->boot_bios_id);
		putc('\n');
		return 1;
	}
	if (streq(v[0], "probe-ide")) {
		probe_fixed_class(ZEDBSD_DEV_IDE);
		listdev(ZEDBSD_DEV_IDE);
		return 1;
	}
	if (streq(v[0], "probe-scsi")) {
		probe_fixed_class(ZEDBSD_DEV_SCSI);
		listdev(ZEDBSD_DEV_SCSI);
		return 1;
	}
	if (streq(v[0], "disk")) {
		if (n == 1) {
			if (curdev >= 0)
				devname(curdev);
			putc('\n');
			return 1;
		}
		return n == 3 && selectdisk(v[1], v[2]);
	}
	if (streq(v[0], "part")) {
		if (n == 1) {
			for (int i = 0; i < MAX_PARTS; i++)
				if (parts[i].valid) {
					dec(i); putc(' '); puts(parts[i].name);
					puts(" LBA "); dec(parts[i].start); putc('\n');
				}
			return 1;
		}
		return selectpart(v[1]);
	}
	if (streq(v[0], "pwd")) {
		char cwd[ZEDBSD_PATH_MAX];
		if (n != 1) return 0;
		if (fs_getcwd(&kern_cwdinfo, cwd, sizeof(cwd)) != 0)
			return 0;
		puts(cwd); putc('\n'); return 1;
	}
	if (streq(v[0], "cd")) {
		const char *path = n == 1 ? zedbsd_env_get(&boot_environment, "HOME") :
			(n == 2 ? v[1] : NULL);
		return path != NULL && fs_chdir(&kern_cwdinfo, path) == 0;
	}
	if (streq(v[0], "ls"))
		return n <= 2 && vfs_ls(n == 2 ? v[1] : ".");
	if (streq(v[0], "cat"))
		return n == 2 && catfile(v[1]);
	if (streq(v[0], "source")) {
		struct file *file;
		off_t size;

		if (n != 2 || file_openat(&kern_cwdinfo, v[1], O_RDONLY, 0,
						 &file) != 0)
			return 0;
		size = file->f_inode->i_size;
		if (size < 0 || size >= CFG_MAX ||
		    file_read(file, cfg, (size_t)size) != size) {
			(void)file_close(file);
			return 0;
		}
		(void)file_close(file);
		cfg[(uint32_t)size] = 0;
		char *p = (char *)cfg;
		unsigned ln = 1;
		while (*p) {
			char *q = p;
			while (*q && *q != '\n' && *q != '\r')
				q++;
			char save = *q;
			*q = 0;
			if (!command(p)) {
				puts("source error line ");
				dec(ln);
				putc('\n');
				return 0;
			}
			*q = save;
			while (*q == '\n' || *q == '\r')
				q++, ln++;
			p = q;
		}
		return 1;
	}
	if (streq(v[0], "halt")) {
		kern_platform_halt();
	}
	if (streq(v[0], "reboot")) {
		kern_platform_reboot();
	}
	if (streq(v[0], "run"))
		return n >= 2 && run_applet(v[1], n - 2, &v[2]);
	if (streq(v[0], "noct")) {
		if (n == 1)
			return run_noct_user("--repl", 0, NULL, 0, NULL, 0);
		return run_noct_application(v[1], "", 0, n - 2, &v[2]);
	}
	if (streq(v[0], "emacs")) {
		const char *dictionary = zedbsd_env_get(&boot_environment,
						      "REMACS_SKK_DICT");

		/* The 8.3 path is present in linux-pc98 product image with EMACS.NAP. */
		if (dictionary == NULL || dictionary[0] == '\0')
			(void)zedbsd_env_set(&boot_environment, "REMACS_SKK_DICT",
					     "HOME/SKKJISYO.DIC");
		return run_noct_application("EMACS", ".NAP", 0, n - 1, &v[1]);
	}
	/* Unknown unqualified names resolve to NAME.NCT on the selected BOOT
	 * filesystem.  C built-ins above always retain precedence, including
	 * their argument-validation failures. */
	return run_noct_application(v[0], ".NCT", 1, n - 1, &v[1]);
}
