/*
 * Boots
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "platform/pc98/abi.h"
#include "core/console.h"
#include "core/env.h"
#include "core/fat16.h"
#include "core/fs.h"
#include "core/image.h"
#include "core/messages.h"
#include "core/namespace.h"
#include "core/blkdev.h"
#include "core/noct-napi.h"
#include "core/noct-platform.h"
#include "core/partition.h"
#include "drivers/ide-pc98.h"
#include "drivers/kbd-pc98.h"
#include "platform/pc98/partition-pc98.h"
#include "platform/pc98/display-pc98.h"
#include <sys/hal/fb.h>

/* Upstream Noct owns BeUI and its PC-98 display backends. */
#include "beui-pc98-auto.h"

#define MAX_PARTS 16
/* Physical linear aperture of the Core-Graph / Cirrus board. */
#define CIRRUS_APERTURE 0xf0000000U
#define CFG_MAX 8192
#define LINE_MAX 256
#define BP_ADDR 0x80000U
#define CMD_ADDR 0x81000U
#define PC98_ADDR 0x82000U
#define PC98_SETUP_NODE_SIZE 32U
#define STARTUP_TIMEOUT_SECONDS 1
#define MAX_IDE_DEVICES 4
#define MAX_SCSI_TARGETS 7
#define MAX_FIXED_DEVICES (MAX_IDE_DEVICES + MAX_SCSI_TARGETS)

/*
 * Stage 2 runs without a C library or operating-system services.  The request
 * object is the sole mutable argument passed through the real-mode BIOS
 * gateway in Stage 1.
 */
static const struct boots_handoff *ho;
static const struct boots_device *devs;
static struct boots_device discovered_devices[MAX_FIXED_DEVICES];
static unsigned device_count;
static uint8_t sec[512], cfg[CFG_MAX];
#ifdef BOOTS_M9_WRITE_TEST
static uint8_t m9_original[512], m9_pattern[512], m9_observed[512];
#endif
static uint32_t load_text_done, load_text_total;
static uint32_t load_data_done, load_data_total;
static int load_progress_class = -1;

struct part {
	uint8_t valid, index, bootable;
	char name[17];
	uint32_t start, data;
};

enum startup_phase {
	STARTUP_DRAW,
	STARTUP_PROBE,
	STARTUP_TIMEOUT,
	STARTUP_SELECTED,
	STARTUP_SHELL,
};

enum startup_auto_kind {
	STARTUP_AUTO_NONE,
	STARTUP_AUTO_CONFIG,
	STARTUP_AUTO_PBR,
};

enum startup_config_kind {
	STARTUP_CONFIG_NONE,
	STARTUP_CONFIG_AUTOEXEC,
	STARTUP_CONFIG_BOOTCFG,
};

struct startup_state {
	enum startup_phase phase;
	unsigned next_candidate;
	unsigned probe_total;
	unsigned probe_done;
	unsigned fixed_count;
	uint8_t ide_bitmap;
	uint8_t scsi_bitmap;
	int auto_device;
	int auto_partition;
	int auto_priority;
	enum startup_auto_kind auto_kind;
	enum startup_config_kind auto_config_kind;
	int automatic_cancelled;
	int timeout_start;
	unsigned timeout_budget;
};
static struct boots_filesystem mounted_fs;
static struct boots_namespace mounted_namespace;
static struct boots_environment boot_environment;
static struct noct_beui_pc98_auto beui_display;
static struct noct_beui_hal beui_hal;
static struct noct_beui_display_hal native_display;

static int display_proxy_enter(void *context, struct noct_beui_display_info *info)
{
	int ok;
	(void)context;
	fb_set_active(1);
	ok = native_display.enter(native_display.context, info);
	if (!ok)
		fb_set_active(0);
	return ok;
}
static void display_proxy_leave(void *context)
{
	(void)context;
	native_display.leave(native_display.context);
	fb_set_active(0);
}
static int display_proxy_poll(void *context)
{
	(void)context;
	return native_display.poll_events == NULL ? 1 :
		native_display.poll_events(native_display.context);
}
static int display_proxy_fill(void *context, const struct noct_beui_rect *rect,
	uint32_t color)
{
	(void)context;
	return native_display.fill(native_display.context, rect, color);
}
static int display_proxy_line(void *context, unsigned x0, unsigned y0,
	unsigned x1, unsigned y1, uint32_t color)
{
	(void)context;
	return native_display.line(native_display.context, x0, y0, x1, y1, color);
}
static int display_proxy_pattern_fill(void *context,
	const struct noct_beui_rect *rect, uint32_t color, uint64_t pattern)
{
	(void)context;
	return native_display.pattern_fill(native_display.context, rect, color,
		pattern);
}
static int display_proxy_draw_image(void *context, unsigned x, unsigned y,
	const struct noct_beui_image *image)
{
	(void)context;
	return native_display.draw_image(native_display.context, x, y, image);
}
static int display_proxy_draw_image_pattern(void *context, unsigned x,
	unsigned y, const struct noct_beui_image *image, uint64_t pattern)
{
	(void)context;
	return native_display.draw_image_pattern(native_display.context, x, y,
		image, pattern);
}
static int display_proxy_flush(void *context,
	const struct noct_beui_rect *rectangles, size_t count)
{
	(void)context;
	return native_display.flush(native_display.context, rectangles, count);
}

static void install_display_proxy(void)
{
	native_display = beui_hal.display;
	beui_hal.display.context = NULL;
	beui_hal.display.enter = display_proxy_enter;
	beui_hal.display.leave = display_proxy_leave;
	beui_hal.display.poll_events = display_proxy_poll;
	beui_hal.display.fill = display_proxy_fill;
	beui_hal.display.line = display_proxy_line;
	beui_hal.display.pattern_fill = display_proxy_pattern_fill;
	beui_hal.display.draw_image = display_proxy_draw_image;
	beui_hal.display.draw_image_pattern = display_proxy_draw_image_pattern;
	beui_hal.display.flush = display_proxy_flush;
}
static struct part parts[MAX_PARTS];
static int curdev = -1, curpart = -1;
static char kernel_name[BOOTS_PATH_MAX], kernel_arg[256];

/* Minimal freestanding string and memory primitives. */
static void memzero(void *p, uint32_t n)
{
	uint8_t *q = p;
	while (n--)
		*q++ = 0;
}
static void memcopy(void *d, const void *s, uint32_t n)
{
	uint8_t *a = d;
	const uint8_t *b = s;
	while (n--)
		*a++ = *b++;
}
static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}
static unsigned slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}
static int strcopy(char *destination, const char *source, unsigned capacity)
{
	unsigned i = 0;

	if (!capacity)
		return 0;
	while (source[i] && i + 1 < capacity) {
		destination[i] = source[i];
		i++;
	}
	destination[i] = 0;
	if (source[i]) {
		destination[0] = 0;
		return 0;
	}
	return 1;
}

static void update_cursor(void)
{
	boots_console_update_cursor();
}

static void putc(char c)
{
	boots_console_putc((uint8_t)c);
}
static void puts(const char *s)
{
	boots_console_puts_sjis((const uint8_t *)s);
}
static void hex8(uint8_t v)
{
	const char *h = "0123456789ABCDEF";
	putc(h[v >> 4]);
	putc(h[v & 15]);
}
static void dec(unsigned v)
{
	char b[11];
	unsigned n = 0;
	if (!v) {
		putc('0');
		return;
	}
	while (v) {
		b[n++] = '0' + v % 10;
		v /= 10;
	}
	while (n)
		putc(b[--n]);
}

static unsigned kib(uint32_t bytes)
{
	return (bytes >> 10) + !!(bytes & 1023);
}

/* Rewrite the current terminal line without scrolling for each disk read. */
static void show_load_progress(int load_class)
{
	uint32_t done = load_class ? load_data_done : load_text_done;
	uint32_t total = load_class ? load_data_total : load_text_total;

	if (load_progress_class >= 0 && load_progress_class != load_class)
		putc('\n');
	load_progress_class = load_class;
	putc('\r');
	boots_console_clear_to_eol();
	putc('\r');
	puts((const char *)(load_class ? boots_msg_data : boots_msg_code));
	dec(kib(done));
	puts(" / ");
	dec(kib(total));
	puts(" KB");
	if (done >= total) {
		putc('\n');
		load_progress_class = -1;
	}
}

static void begin_load_progress(uint32_t kernel_size)
{
	puts((const char *)boots_msg_kernel_size);
	dec(kib(kernel_size));
	puts(" KB\n");
	load_progress_class = -1;
}

uint64_t boots_kernel_ticks(void);
uint64_t boots_kernel_milliseconds(void *context);
void boots_pc98_jump_linux(uint32_t entry, uint32_t boot_params)
	__attribute__((noreturn));

static uint8_t
beui_port_in8(void *context, uint16_t port)
{
	uint8_t value;

	(void)context;
	__asm__ volatile ("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
beui_port_out8(void *context, uint16_t port, uint8_t value)
{
	(void)context;
	__asm__ volatile ("outb %0,%w1" : : "a"(value), "Nd"(port));
}

static int
beui_display_reset(void *context)
{
	(void)context;
	return boots_pc98_display_graphics_start();
}

static int
beui_display_stop(void *context)
{
	(void)context;
	return boots_pc98_display_graphics_stop();
}
/*
 * Native block device lookup.  The firmware enumerates IDE disks in the
 * same bank-major order the driver probes them, so the pairing between
 * a BIOS device descriptor and a registered unit is ordinal.  Non-IDE
 * classes have no native driver yet (SCSI later; floppies need a future
 * FDC driver) and read as absent.
 */
static struct boots_blkdev *blk_for_dev(const struct boots_device *d)
{
	unsigned ordinal = 0;

	for (unsigned i = 0; i < device_count; i++) {
		if (&devs[i] == d) {
			if (d->device_class != BOOTS_DEV_IDE)
				return 0;
			return boots_ide_pc98_unit(ordinal);
		}
		if (devs[i].device_class == BOOTS_DEV_IDE)
			ordinal++;
	}
	return 0;
}
/* Nonzero on failure, matching the old gateway convention. */
static int readsec(const struct boots_device *d, uint32_t lba, void *buf)
{
	struct boots_blkdev *blk = blk_for_dev(d);

	return blk == 0 ||
	       boots_blkdev_read(blk, lba, 1, buf) != BOOTS_BLKDEV_OK;
}
static int writesec(const struct boots_device *d, uint32_t lba,
		    const void *buf)
{
	struct boots_blkdev *blk = blk_for_dev(d);

	return blk == 0 ||
	       boots_blkdev_write(blk, lba, 1, buf) != BOOTS_BLKDEV_OK;
}
static uint16_t w16(const uint8_t *p)
{
	return p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t w32(const uint8_t *p)
{
	return w16(p) | ((uint32_t)w16(p + 2) << 16);
}
/* PC-98 partition-table discovery using per-device BIOS logical geometry. */
static void devname(int i)
{
	switch (devs[i].device_class) {
	case BOOTS_DEV_FDD:
		puts("fd");
		break;
	case BOOTS_DEV_IDE:
		puts("ide");
		break;
	default:
		puts("scsi");
	}
	dec(devs[i].display_index);
}
static int scanparts(int di)
{
	struct boots_partition entries[MAX_PARTS];
	struct boots_blkdev *blk;
	int count;

	memzero(parts, sizeof(parts));
	if (di < 0 || !(devs[di].flags & BOOTS_DEV_HAS_GEOMETRY))
		return 0;
	blk = blk_for_dev(&devs[di]);
	if (blk == 0)
		return 0;
	count = boots_partition_scan(blk, entries, MAX_PARTS);
	if (count < 0)
		return 0;
	for (int i = 0; i < count; i++) {
		const struct boots_partition *e = &entries[i];

		if (e->start_lba == 0 && e->data_lba == 0 &&
		    e->name[0] == 0 && !e->bootable)
			continue;
		parts[i].valid = 1;
		parts[i].index = i;
		parts[i].bootable = e->bootable;
		parts[i].start = (uint32_t)e->start_lba;
		parts[i].data = (uint32_t)e->data_lba;
		for (int j = 0; j < 16; j++) {
			parts[i].name[j] = e->name[j];
			if (!parts[i].name[j])
				break;
		}
		parts[i].name[16] = 0;
	}
	return 1;
}
static int disk_volume_read(const void *context, uint32_t lba, void *buffer)
{
	return !readsec(context, lba, buffer);
}
static int disk_volume_write(void *context, uint32_t lba,
			     const void *buffer)
{
	return !writesec(context, lba, buffer);
}

static int mountpart_into(struct boots_filesystem *filesystem,
			  int device_index, int partition_index)
{
	const struct boots_filesystem_driver *const drivers[] = {
		&boots_fat16_driver,
		&boots_fat12_driver,
	};
	struct boots_volume volume;

	if (!parts[partition_index].valid)
		return 0;
	/* The shared volume ABI uses mutable context for write callbacks.  The
	 * device descriptor itself remains logically read-only. */
	volume.context = (void *)&devs[device_index];
	volume.start_lba = parts[partition_index].data;
	volume.sector_size = 512;
	volume.read = disk_volume_read;
	volume.write = disk_volume_write;
	return boots_fs_mount(filesystem, &volume, drivers,
			       sizeof(drivers) / sizeof(drivers[0]));
}

static int mountpart(int device_index, int partition_index)
{
	return mountpart_into(&mounted_fs, device_index, partition_index);
}

static int disk_mount_name(int device_index, char name[8])
{
	unsigned ordinal;

	if (device_index < 0 || (unsigned)device_index >= device_count)
		return 0;
	ordinal = (unsigned)device_index + 1U;
	name[0] = 'd';
	name[1] = 'i';
	name[2] = 's';
	name[3] = 'k';
	if (ordinal < 10U) {
		name[4] = (char)('0' + ordinal);
		name[5] = 0;
	} else {
		name[4] = (char)('0' + ordinal / 10U);
		name[5] = (char)('0' + ordinal % 10U);
		name[6] = 0;
	}
	return 1;
}

static void select_disk_home(int device_index)
{
	char name[8];
	char home[24];
	char dictionary[48];
	unsigned name_length;

	if (!disk_mount_name(device_index, name) ||
	    !boots_namespace_set_default(&mounted_namespace, name))
		return;
	home[0] = '/';
	name_length = slen(name);
	memcopy(home + 1, name, name_length);
	memcopy(home + 1 + name_length, "/home", 6);
	(void)boots_env_set(&boot_environment, "HOME", home);
	memcopy(dictionary, home, slen(home));
	memcopy(dictionary + slen(home), "/skkjisyo.dic", 14);
	(void)boots_env_set(&boot_environment, "REMACS_SKK_DICT", dictionary);
}

/* Mount one user-visible FAT volume per physical disk.  BOOT is preferred;
 * otherwise the first readable FAT16 partition becomes /diskN.  The
 * namespace is intentionally above the filesystem drivers so future ext4
 * and UFS drivers can use the same UNIX path contract. */
static void register_scanned_disk(int device_index)
{
	struct boots_filesystem filesystem;
	char name[8];
	int preferred = -1;
	int fallback = -1;
	int partition;

	if (!disk_mount_name(device_index, name))
		return;
	for (partition = 0; partition < MAX_PARTS; partition++) {
		if (!parts[partition].valid)
			continue;
		if (streq(parts[partition].name, "BOOT"))
			preferred = partition;
		else if (fallback < 0)
			fallback = partition;
	}
	if (preferred >= 0 &&
	    mountpart_into(&filesystem, device_index, preferred)) {
		(void)boots_namespace_mount(&mounted_namespace, name, &filesystem);
		return;
	}
	for (partition = fallback; partition >= 0 && partition < MAX_PARTS;
	     partition++)
		if (parts[partition].valid &&
		    mountpart_into(&filesystem, device_index, partition)) {
			(void)boots_namespace_mount(&mounted_namespace, name,
						&filesystem);
			return;
		}
}

/* Keyboard input, parser, and stateful shell selection helpers. */
static void prompt(void)
{
	if (curdev >= 0)
		devname(curdev);
	else
		puts("none");
	if (curpart >= 0) {
		putc(':');
		puts(parts[curpart].name);
	}
	puts(" ok ");
	update_cursor();
}
static uint32_t raw_key(void)
{
	/* A blocking read must never leave the hardware cursor stale or hidden. */
	update_cursor();
	return (uint32_t)boots_kbd_pc98_read();
}
static int key(void)
{
	return (int)raw_key();
}
static uint32_t applet_key(void)
{
	return (uint32_t)key();
}
static int poll(void)
{
	return boots_kbd_pc98_poll();
}

static int noct_key_read(void *context)
{
	(void)context;
	return boots_kbd_pc98_read_event();
}

static int noct_key_poll(void *context)
{
	(void)context;
	return boots_kbd_pc98_poll_event();
}

static int noct_key_is_down(void *context, int key)
{
	(void)context;
	return boots_kbd_pc98_state(key);
}

/* A BeUI-only program never reads the buffered Keyboard module, so the
 * type-ahead a held key fills has to be emptied as a BeUI side effect or
 * keys pressed during a game leak to the next buffered reader.  The BIOS
 * queue holds at most 16 entries; the bound keeps the drain finite
 * against a BIOS whose poll never reports an empty queue. */
static void noct_key_drain(void *context)
{
	(void)context;
	boots_kbd_pc98_drain();
}

/* Return seconds since the start of the current minute, or -1 for an
 * invalid BIOS result.  Stage 1 converts the BIOS BCD byte to binary before
 * returning it through the gateway.  INT 1Ch/AH=00h is available on the
 * early PC-9801 models for which a CPU-speed-dependent delay loop would be
 * least useful. */
static int clock_second(void)
{
	return (int)((boots_kernel_ticks() / 100U) % 60U);
}

static int noct_clock_second(void *context)
{
	(void)context;
	return clock_second();
}

static int line(char *b)
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
		if (devs[i].flags & BOOTS_DEV_HAS_GEOMETRY) {
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
			kernel_name[0] = kernel_arg[0] = 0;
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
			kernel_name[0] = kernel_arg[0] = 0;
			return 1;
		}
	return 0;
}

/* Filesystem-facing shell commands and extension-module loaders. */
static void ls(void)
{
	if (curpart < 0) {
		for (int i = 0; i < MAX_PARTS; i++)
			if (parts[i].valid) {
				dec(i);
				putc(' ');
				puts(parts[i].name);
				puts(" LBA ");
				dec(parts[i].start);
				putc('\n');
		}
		return;
	}
	for (unsigned index = 0;; index++) {
		struct boots_dirent entry;

		if (!boots_fs_readdir(&mounted_fs, "", index, &entry))
			return;
		puts(entry.name);
		putc('\n');
	}
}
static int catfile(const char *n)
{
	struct boots_file file;
	uint64_t offset = 0;

	if (curpart < 0 || !boots_fs_open(&mounted_fs, n, &file))
		return 0;
	while (offset < file.size) {
		uint32_t k = file.size - offset > 512 ?
		             512 : (uint32_t)(file.size - offset);
		if (!boots_file_read(&file, offset, sec, k))
			return 0;
		for (uint32_t i = 0; i < k; i++)
			putc(sec[i]);
		offset += k;
	}
	return 1;
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
	struct boots_file file;
	uint8_t *image = (uint8_t *)0x50000;
	if (curpart < 0 || !boots_fs_open(&mounted_fs, n, &file) ||
	    file.size < sizeof(struct boots_applet_header) ||
	    file.size > 0x10000 ||
	    !boots_file_read(&file, 0, image, (uint32_t)file.size))
		return 0;
	struct boots_applet_header *h = (struct boots_applet_header *)image;
	if (h->magic != BOOTS_APPLET_MAGIC || h->abi_version != 1 ||
	    h->header_size != sizeof(*h) || h->image_size != file.size ||
	    h->entry_offset < h->header_size || h->entry_offset >= file.size ||
	    crc32_image(image, (uint32_t)file.size) != h->crc32)
		return 0;
	struct boots_applet_services s = {1, sizeof(s), putc, puts,
	                                   applet_key};
	boots_applet_entry_t entry =
	        (boots_applet_entry_t)(image + h->entry_offset);
	uint32_t r = entry(&s, (uint32_t)argc, (const char *const *)argv);
	if (r) {
		puts("applet status ");
		dec(r);
		putc('\n');
		return 0;
	}
	return 1;
}
/*
 * 0:055Dh contains the dense BIOS IDE-drive map in its low nibble.  Unlike
 * physical-slot bitmap 0:05BAh, bit N corresponds directly to BIOS unit
 * 80h+N.  Stock ROMs may not return from SENSE for an absent unit, so this
 * map is authoritative for enumeration.  Preserve a boot unit already
 * handed to Stage 2 even if unusual firmware failed to publish the bit.
 */
static uint8_t ide_reported_drives(void)
{
	uint8_t bitmap = 0;

	for (unsigned i = 0; i < device_count; i++)
		if (devs[i].device_class == BOOTS_DEV_IDE &&
		    devs[i].bios_id >= 0x80 &&
		    devs[i].bios_id < 0x80 + MAX_IDE_DEVICES)
			bitmap |= 1U << (devs[i].bios_id - 0x80);
	return bitmap;
}

/*
 * A PC-9801-55/92 host adapter normally owns SCSI ID 7.  Some firmware sets
 * bit 7 in 0:0482h for the adapter itself; treating it as an eighth disk and
 * issuing SENSE to A7h can enter firmware that never returns.  Enumerate only
 * the seven target IDs which the registered SCSI BIOS reports as disks.
 */
static uint8_t scsi_reported_targets(void)
{
	uint8_t bitmap = 0;
	for (unsigned i = 0; i < device_count; i++)
		if (devs[i].device_class == BOOTS_DEV_SCSI &&
		    devs[i].bios_id >= 0xa0 &&
		    devs[i].bios_id < 0xa0 + MAX_SCSI_TARGETS)
			bitmap |= 1U << (devs[i].bios_id - 0xa0);
	return bitmap;
}

static unsigned bit_count(uint8_t value)
{
	unsigned count = 0;

	while (value) {
		count += value & 1U;
		value >>= 1;
	}
	return count;
}

/* Probe exactly one BIOS unit.  A nonnegative result is the new list index. */
static int probe_fixed_device(uint8_t device_class, uint8_t bios_id)
{
	for (unsigned i = 0; i < device_count; i++)
		if (devs[i].device_class == device_class &&
		    devs[i].bios_id == bios_id)
			return (int)i;
	return -1;
}

/* Shell probes remain exhaustive; startup uses probe_fixed_device directly. */
static void probe_fixed_class(uint8_t device_class)
{
	uint8_t first = device_class == BOOTS_DEV_IDE ? 0x80 : 0xa0;
	unsigned count = device_class == BOOTS_DEV_IDE ? MAX_IDE_DEVICES :
							MAX_SCSI_TARGETS;

	if (device_class == BOOTS_DEV_IDE) {
		boots_blkdev_reset();
		(void)boots_ide_pc98_init(devs, device_count);
	}
	for (unsigned index = 0; index < count; index++)
		probe_fixed_device(device_class, first + index);
}

/* ELF32 structures used by the uncompressed Linux kernel loader. */
struct eh {
	uint8_t id[16];
	uint16_t type, machine;
	uint32_t ver, entry, phoff, shoff, flags;
	uint16_t ehsize, phsize, phnum;
};
struct ph {
	uint32_t type, off, vaddr, paddr, filesz, memsz, flags, align;
};
static __attribute__((noreturn)) void jump_linux(uint32_t entry)
{
	boots_pc98_jump_linux(entry, BP_ADDR);
	__builtin_unreachable();
}

/*
 * Enable the PC-98 memory mapping required before writing kernel segments
 * above 1 MiB.  The port sequence mirrors the existing Linux loader and must
 * be completed while interrupts are still under Stage 2 control.
 */
static void enable_highmem(void)
{
	asm volatile(
	        "xorb %%al,%%al; outb %%al,$0xf2; movb $2,%%al; outb "
	        "%%al,$0xf6; movw $0x439,%%dx; inb %%dx,%%al; andb $0xfb,%%al; "
	        "outb %%al,%%dx; xorb %%al,%%al; outb %%al,$0xf8; movw "
	        "$0x43b,%%dx; movb $4,%%al; outb %%al,%%dx" ::
	                : "eax", "edx");
}
static uint8_t low8(uint32_t a)
{
	uint8_t v;
	asm volatile("movb (%1),%0" : "=q"(v) : "r"(a));
	return v;
}

static uint16_t low16(uint32_t a)
{
	uint16_t v;
	asm volatile("movw (%1),%0" : "=r"(v) : "r"(a));
	return v;
}

/*
 * Publish one SETUP_PC98_DISK node for every BIOS-visible fixed disk.  Linux
 * needs each disk's own logical geometry to decode its NEC98 partition table;
 * passing only the boot disk is insufficient when IDE and SCSI disks coexist.
 * The device descriptors already contain the Stage 1 SENSE results, so this
 * does not issue more BIOS calls or lengthen the handoff path.
 */
static void build_pc98_disk_setup(uint8_t *bp)
{
	uint8_t *previous = 0;
	unsigned count = 0;

	*(uint32_t *)(bp + 0x250) = 0;
	*(uint32_t *)(bp + 0x254) = 0;
	for (unsigned i = 0; i < device_count; i++) {
		const struct boots_device *d = &devs[i];
		uint8_t *x;

		if ((d->device_class != BOOTS_DEV_IDE &&
		     d->device_class != BOOTS_DEV_SCSI) ||
		    !(d->flags & BOOTS_DEV_HAS_GEOMETRY) ||
		    !d->heads || !d->sectors || count >= 12)
			continue;

		x = (uint8_t *)(PC98_ADDR + count * PC98_SETUP_NODE_SIZE);
		memzero(x, PC98_SETUP_NODE_SIZE);
		if (!count)
			*(uint32_t *)(bp + 0x250) = (uint32_t)x;
		if (previous)
			*(uint32_t *)(previous + 0) = (uint32_t)x;
		*(uint32_t *)(x + 8) = 11;
		*(uint32_t *)(x + 12) = 12;
		*(uint32_t *)(x + 16) = 0x44383950;
		*(uint16_t *)(x + 20) = 1;
		*(uint16_t *)(x + 22) = 12;
		x[24] = d->bios_id;
		x[25] = d->heads;
		x[26] = d->sectors;
		if ((int)i == curdev)
			x[27] = BOOTS_LINUX_DISK_F_BOOT;
		previous = x;
		count++;
	}
}

/*
 * Load every PT_LOAD segment, construct Linux boot_params and the PC-98
 * extension block, then enter the ELF entry point.  BIOS logical H/S and the
 * original BIOS drive number are preserved for the kernel partition parser.
 */
static int vmlinux_probe(struct boots_file *file)
{
	struct eh e;

	return file->size <= 0xffffffffU &&
	       boots_file_read(file, 0, &e, sizeof(e)) &&
	       w32(e.id) == 0x464c457f && e.id[4] == 1 && e.id[5] == 1 &&
	       e.machine == 3;
}

static void load_progress(void *context, uint32_t bytes)
{
	int load_class = *(const int *)context;

	if (load_class)
		load_data_done += bytes;
	else
		load_text_done += bytes;
	show_load_progress(load_class);
}

static int vmlinux_load(struct boots_file *file, const char *arguments)
{
	struct eh e;
	struct ph p;
	unsigned load_segments = 0;

	if (!boots_file_read(file, 0, &e, sizeof(e)))
		return 0;
	if (w32(e.id) != 0x464c457f || e.id[4] != 1 || e.id[5] != 1 ||
	    e.machine != 3 || e.phsize != sizeof(p) || e.phnum > 16)
		return 0;
	load_text_done = load_text_total = 0;
	load_data_done = load_data_total = 0;
	for (unsigned i = 0; i < e.phnum; i++) {
		if (!boots_file_read(file, e.phoff + i * sizeof(p), &p,
				      sizeof(p)))
			return 0;
		if (p.type != 1)
			continue;
		if (p.filesz > p.memsz || p.paddr < 0x100000 ||
		    p.off + p.filesz < p.off || p.off + p.filesz > file->size)
			return 0;
		if (p.flags & 2) {
			if (load_data_total + p.filesz < load_data_total)
				return 0;
			load_data_total += p.filesz;
		} else {
			if (load_text_total + p.filesz < load_text_total)
				return 0;
			load_text_total += p.filesz;
		}
		load_segments++;
	}
	if (!load_segments)
		return 0;
	begin_load_progress((uint32_t)file->size);
	enable_highmem();
	for (unsigned i = 0; i < e.phnum; i++) {
		int load_class;

		if (!boots_file_read(file, e.phoff + i * sizeof(p), &p,
				      sizeof(p)))
			return 0;
		if (p.type != 1)
			continue;
		if (p.filesz > p.memsz || p.paddr < 0x100000 ||
		    p.off + p.filesz > file->size)
			return 0;
		load_class = !!(p.flags & 2);
		if (!boots_file_read_progress(file, p.off, (void *)p.paddr,
					       p.filesz, load_progress,
					       &load_class))
			return 0;
		memzero((void *)(p.paddr + p.filesz), p.memsz - p.filesz);
	}
	memzero((void *)BP_ADDR, 4096);
	memcopy((void *)CMD_ADDR, arguments, slen(arguments) + 1);
	uint8_t *bp = (uint8_t *)BP_ADDR;
	*(uint32_t *)(bp + 0x228) = CMD_ADDR;
	bp[0x210] = 0xff;
	bp[0x1e8] = 2;
	build_pc98_disk_setup(bp);
	uint32_t conv = ((low8(0x501) & 7) + 1) << 17;
	uint32_t ext = low8(0x401) << 17;
	uint8_t *em = bp + 0x2d0;
	*(uint64_t *)(em + 0) = 0;
	*(uint64_t *)(em + 8) = conv;
	*(uint32_t *)(em + 16) = 1;
	*(uint64_t *)(em + 20) = 0x100000;
	*(uint64_t *)(em + 28) = ext;
	*(uint32_t *)(em + 36) = 1;
	/*
	 * 0:0594h reports the number of MiB above the PC-98 16 MiB boundary.
	 * The first C implementation accidentally omitted this third E820 entry,
	 * limiting a 64 MiB machine to the memory described by 0:0401 (about
	 * 16 MiB) and causing severe swap thrashing during Debian login.
	 */
	uint16_t high_mib = low16(0x594);
	if (high_mib) {
		*(uint64_t *)(em + 40) = 0x1000000;
		*(uint64_t *)(em + 48) = (uint64_t)high_mib << 20;
		*(uint32_t *)(em + 56) = 1;
		bp[0x1e8] = 3;
	} else {
		bp[0x1e8] = 2;
	}
	jump_linux(e.entry);
}

static const struct boots_image_loader vmlinux_loader = {
	"vmlinux", vmlinux_probe, vmlinux_load
};

static int linuxboot(void)
{
	if (curpart < 0 || !kernel_name[0])
		return 0;
	return boots_image_boot(&vmlinux_loader, &mounted_fs, kernel_name,
				 kernel_arg);
}

#ifdef BOOTS_M9_WRITE_TEST
static void m9_debug_puts(const char *text)
{
	while (*text) {
		uint8_t character = (uint8_t)*text++;

		asm volatile("outb %0,$0xe9" : : "a"(character));
	}
}

static void m9_report(const char *text)
{
	puts(text);
	m9_debug_puts(text);
}

static int m9_same_sector(const uint8_t *left, const uint8_t *right)
{
	for (unsigned i = 0; i < 512; i++)
		if (left[i] != right[i])
			return 0;
	return 1;
}

/* Destructive raw-sector test, compiled only into BOOT-M9.SYS.  The caller
 * must select an expendable sector in a temporary image.  Once the first
 * write succeeds, every exit path attempts to restore the original sector. */
static int m9_write_test(uint32_t lba)
{
	int result = 1;

	if (curdev < 0) {
		m9_report("M9 disk write test: no selected disk\n");
		return 0;
	}
	if (readsec(&devs[curdev], lba, m9_original)) {
		m9_report("M9 disk write test: initial read failed\n");
		return 0;
	}
	for (unsigned i = 0; i < sizeof(m9_pattern); i++)
		m9_pattern[i] = (uint8_t)(0xa5U ^ i ^ lba ^ (lba >> 8));
	if (writesec(&devs[curdev], lba, m9_pattern)) {
		m9_report("M9 disk write test: pattern write failed\n");
		return 0;
	}
	if (readsec(&devs[curdev], lba, m9_observed) ||
	    !m9_same_sector(m9_observed, m9_pattern)) {
		m9_report("M9 disk write test: pattern read-back failed\n");
		result = 0;
	}
	if (writesec(&devs[curdev], lba, m9_original)) {
		m9_report("M9 disk write test: RESTORE FAILED\n");
		return 0;
	}
	if (readsec(&devs[curdev], lba, m9_observed) ||
	    !m9_same_sector(m9_observed, m9_original)) {
		m9_report("M9 disk write test: restore read-back failed\n");
		return 0;
	}
	if (result)
		m9_report("M9 disk write/read/restore: PASS\n");
	return result;
}
#endif

/* Resolve PREFIX + NAME.EXT with the name upper-cased. */
static int open_noct_application(const char *prefix, const char *name,
				 const char *extension,
				 char path[BOOTS_PATH_MAX])
{
	struct boots_file file;
	unsigned source = 0;
	unsigned position = 0;
	unsigned base_length = 0;
	unsigned extension_length = 0;

	while (prefix[source] != '\0') {
		if (position + 1U >= BOOTS_PATH_MAX)
			return 0;
		path[position++] = prefix[source++];
	}
	while (name[base_length] != '\0') {
		char ch = name[base_length++];

		if (ch == '.' || ch == '/' || ch == '\\' ||
		    position + 1U >= BOOTS_PATH_MAX)
			return 0;
		path[position++] = ch >= 'a' && ch <= 'z' ?
			(char)(ch - 'a' + 'A') : ch;
	}
	if (!base_length)
		return 0;
	while (extension[extension_length] != '\0') {
		if (position + 1U >= BOOTS_PATH_MAX)
			return 0;
		path[position++] = extension[extension_length++];
	}
	path[position] = '\0';
	return boots_fs_open(&mounted_fs, path, &file) ? 1 : 0;
}

/* Execute one already-tokenized shell command against the current state.
 * Source commands resolve in CMD/ with a root fallback for pre-CMD BOOT
 * volumes.  Unqualified names then fall back to precompiled NAME.NAP
 * bytecode in APPS/ (then CMD/ and the root), so applications too large
 * for the small-memory source compiler still launch as shell commands. */
static int run_noct_application(const char *name, const char *extension,
				int nap_fallback, int argc,
				char *const argv[])
{
	char path[BOOTS_PATH_MAX];

	if (!open_noct_application("CMD/", name, extension, path) &&
	    !open_noct_application("", name, extension, path) &&
	    (!nap_fallback ||
	     (!open_noct_application("APPS/", name, ".NAP", path) &&
	      !open_noct_application("CMD/", name, ".NAP", path) &&
	      !open_noct_application("", name, ".NAP", path))))
		return 0;
	return boots_noct_run_file(&mounted_namespace, &mounted_fs,
				    &boot_environment, path,
				    argc, argv, noct_key_read, noct_key_poll,
				    noct_clock_second, 0);
}

static int command(char *s)
{
	char *v[20];
	int n = split(s, v, 20);
	if (!n)
		return 1;
#ifdef BOOTS_M9_WRITE_TEST
	if (streq(v[0], "m9-write-test")) {
		int lba = n == 2 ? number(v[1]) : -1;

		return lba >= 0 && m9_write_test((uint32_t)lba);
	}
#endif
	if (streq(v[0], "help")) {
		puts("help echo env set unset pause wait devalias probe-ide probe-scsi "
		     "disk part ls cat source kernel arg boot linux "
		     "run noct emacs noct-test reboot halt\n");
		return 1;
	}
	if (streq(v[0], "env")) {
		if (n != 1)
			return 0;
		for (size_t index = 0;
		     index < boots_env_count(&boot_environment); index++) {
			const char *name;
			const char *value;

			if (!boots_env_at(&boot_environment, index, &name, &value))
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
		       boots_env_set(&boot_environment, v[1], v[2]);
	if (streq(v[0], "unset")) {
		if (n != 2 || !boots_env_name_valid(v[1]))
			return 0;
		(void)boots_env_unset(&boot_environment, v[1]);
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
	if (streq(v[0], "devalias")) {
		listdev(0);
		puts("boot -> BIOS ");
		hex8(ho->boot_bios_id);
		putc('\n');
		return 1;
	}
	if (streq(v[0], "probe-ide")) {
		probe_fixed_class(BOOTS_DEV_IDE);
		listdev(BOOTS_DEV_IDE);
		return 1;
	}
	if (streq(v[0], "probe-scsi")) {
		probe_fixed_class(BOOTS_DEV_SCSI);
		listdev(BOOTS_DEV_SCSI);
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
			ls();
			return 1;
		}
		return selectpart(v[1]);
	}
	if (streq(v[0], "ls")) {
		ls();
		return 1;
	}
	if (streq(v[0], "cat"))
		return n == 2 && catfile(v[1]);
	if (streq(v[0], "kernel")) {
		if (n == 1) {
			puts(kernel_name);
			putc('\n');
			return 1;
		}
		if (!strcopy(kernel_name, v[1], sizeof(kernel_name)))
			return 0;
		kernel_arg[0] = 0;
		return 1;
	}
	if (streq(v[0], "arg")) {
		kernel_arg[0] = 0;
		unsigned z = 0;
		for (int i = 1; i < n; i++) {
			if (i > 1 && z < 255)
				kernel_arg[z++] = ' ';
			for (char *p = v[i]; *p && z < 255;)
				kernel_arg[z++] = *p++;
		}
		kernel_arg[z] = 0;
		return 1;
	}
	if (streq(v[0], "linux")) {
		if (n < 2)
			return 0;
		if (!strcopy(kernel_name, v[1], sizeof(kernel_name)))
			return 0;
		kernel_arg[0] = 0;
		unsigned z = 0;
		for (int i = 2; i < n; i++) {
			if (i > 2)
				kernel_arg[z++] = ' ';
			for (char *p = v[i]; *p && z < 255;)
				kernel_arg[z++] = *p++;
		}
		kernel_arg[z] = 0;
		return linuxboot();
	}
	if (streq(v[0], "boot")) {
		if (kernel_name[0])
			return linuxboot();
		if (curdev < 0)
			return 0;
		puts("Chain boot is not available on the HAL yet.\n");
		return 0;
	}
	if (streq(v[0], "source")) {
		struct boots_file file;

		if (n != 2 || !boots_fs_open(&mounted_fs, v[1], &file) ||
		    file.size >= CFG_MAX ||
		    !boots_file_read(&file, 0, cfg, (uint32_t)file.size))
			return 0;
		cfg[(uint32_t)file.size] = 0;
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
		for (;;)
			asm volatile("cli; hlt");
	}
	if (streq(v[0], "reboot")) {
		asm volatile("movb $0x0f,%%al; outb %%al,$0x37" ::: "eax");
		for (;;)
			;
	}
	if (streq(v[0], "run"))
		return n >= 2 && run_applet(v[1], n - 2, &v[2]);
	if (streq(v[0], "noct")) {
		if (n == 1)
			return boots_noct_run_repl(&mounted_namespace, &mounted_fs,
						    &boot_environment, noct_key_read,
						    noct_key_poll,
						    noct_clock_second, 0);
		return boots_noct_run_file(&mounted_namespace, &mounted_fs,
					    &boot_environment,
					    v[1], n - 2, &v[2],
					    noct_key_read, noct_key_poll,
					    noct_clock_second, 0);
	}
	if (streq(v[0], "emacs")) {
		const char *dictionary = boots_env_get(&boot_environment,
						      "REMACS_SKK_DICT");

		/* The 8.3 path is present in every Boots image with REMACS.NAP. */
		if (dictionary == NULL || dictionary[0] == '\0')
			(void)boots_env_set(&boot_environment, "REMACS_SKK_DICT",
					     "HOME/SKKJISYO.DIC");
		return run_noct_application("REMACS", ".NAP", 0, n - 1, &v[1]);
	}
	if (streq(v[0], "noct-test")) {
		int repeat;

		if (n > 2)
			return 0;
		repeat = n == 2 ? number(v[1]) : 1;
		if (repeat < 1 || repeat > 100)
			return 0;
		return boots_noct_run_embedded((unsigned)repeat);
	}
	/* Unknown unqualified names resolve to NAME.NCT on the selected BOOT
	 * filesystem.  C built-ins above always retain precedence, including
	 * their argument-validation failures. */
	return run_noct_application(v[0], ".NCT", 1, n - 1, &v[1]);
}

/* The startup menu exposes only the first four fixed disks. The full stable
 * discovery order remains addressable through devalias and disk. */
static int menu_device(unsigned ordinal)
{
	unsigned found = 0;
	for (unsigned i = 0; i < device_count; i++) {
		if (++found == ordinal)
			return (int)i;
	}
	return -1;
}

/* Resolve the startup configuration file on the mounted BOOT volume.
 * BOOT.CFG is the pre-1.0 name; keep reading it for one release. */
static const char *startup_config_file(void)
{
	struct boots_file file;

	if (boots_fs_open(&mounted_fs, "BOOTS.CFG", &file))
		return "BOOTS.CFG";
	if (boots_fs_open(&mounted_fs, "BOOT.CFG", &file))
		return "BOOT.CFG";
	return NULL;
}

static enum startup_config_kind boot_volume_startup_kind(void)
{
	struct boots_file file;

	if (boots_fs_open(&mounted_fs, "AUTOEXEC.NCT", &file))
		return STARTUP_CONFIG_AUTOEXEC;
	if (startup_config_file())
		return STARTUP_CONFIG_BOOTCFG;
	return STARTUP_CONFIG_NONE;
}

static unsigned fixed_device_ordinal(int device)
{
	unsigned ordinal = 0;

	for (int i = 0; i <= device; i++)
		ordinal++;
	return ordinal;
}

static void consider_automatic_device(struct startup_state *state, int device)
{
	int first_bootable = -1;
	int config_partition = -1;
	enum startup_config_kind config_kind = STARTUP_CONFIG_NONE;

	if (device < 0 || !(devs[device].flags & BOOTS_DEV_HAS_GEOMETRY) ||
	    !scanparts(device))
		return;
	register_scanned_disk(device);
	for (int partition = 0; partition < MAX_PARTS; partition++) {
		if (!parts[partition].valid)
			continue;
		/* The BOOT volume's PBR reloads this loader.  It must not be the
		 * fallback target when BOOTS.CFG is absent, or Auto loops forever. */
		if (first_bootable < 0 && parts[partition].bootable &&
		    !streq(parts[partition].name, "BOOT"))
			first_bootable = partition;
		if (config_partition < 0 && streq(parts[partition].name, "BOOT") &&
		    mountpart(device, partition) &&
		    (config_kind = boot_volume_startup_kind()) !=
			    STARTUP_CONFIG_NONE)
			config_partition = partition;
	}
	if (config_partition >= 0 && state->auto_priority > 1) {
		/* Discovery order is stable: keep the first BOOT volume found. */
		state->auto_priority = 1;
		state->auto_kind = STARTUP_AUTO_CONFIG;
		state->auto_config_kind = config_kind;
		state->auto_device = device;
		state->auto_partition = config_partition;
	}
	if (first_bootable >= 0 && 3 < state->auto_priority) {
		state->auto_priority = 3;
		state->auto_kind = STARTUP_AUTO_PBR;
		state->auto_device = device;
		state->auto_partition = first_bootable;
	}
}

static int activate_automatic_target(const struct startup_state *state)
{
	if (state->auto_kind == STARTUP_AUTO_NONE ||
	    !scanparts(state->auto_device) ||
	    !parts[state->auto_partition].valid)
		return 0;
	if (state->auto_kind == STARTUP_AUTO_CONFIG &&
	    (!mountpart(state->auto_device, state->auto_partition) ||
	     boot_volume_startup_kind() == STARTUP_CONFIG_NONE))
		return 0;
	curdev = state->auto_device;
	curpart = state->auto_partition;
	select_disk_home(curdev);
	kernel_name[0] = kernel_arg[0] = 0;
	return 1;
}

/* AUTOEXEC.NCT may select one action, but it cannot inject a second shell
 * line or leave a stale action behind for a later VM invocation. */
static int valid_boot_action(const char *action)
{
	unsigned length = 0;
	int non_space = 0;

	if (action == 0)
		return 0;
	while (action[length] != 0) {
		unsigned char ch = (unsigned char)action[length++];

		if (length >= LINE_MAX || ch < 0x20U || ch == 0x7fU)
			return 0;
		if (ch != ' ' && ch != '\t')
			non_space = 1;
	}
	return non_space;
}

/* Return zero when no graphical startup script exists, one after executing
 * its selected action, and -1 when the script/action failed validation. */
static int run_autoexec(void)
{
	struct boots_file file;
	const char *selected;
	char action[LINE_MAX];
	int script_ok;

	if (!boots_fs_open(&mounted_fs, "AUTOEXEC.NCT", &file))
		return 0;
	(void)boots_env_unset(&boot_environment, "BOOT_ACTION");
	script_ok = boots_noct_run_file(&mounted_namespace, &mounted_fs,
					 &boot_environment,
					 "AUTOEXEC.NCT", 0, 0, noct_key_read,
					 noct_key_poll, noct_clock_second, 0);
	/* A graphical script may have owned Cirrus or GDC graphics.  Restore the
	 * firmware text display and erase every GDC graphics plane before its
	 * selected Boots command runs.  Real Cirrus-equipped machines retain the
	 * old graphics VRAM contents when the display is switched back to GDC. */
	(void)boots_pc98_display_text_restore();
	(void)noct_beui_pc98_gdc_clear_graphics(&beui_display.gdc);
	boots_console_reset();
	boots_console_set_mode(BOOTS_CONSOLE_TERMINAL);
	if (!script_ok) {
		puts("AUTOEXEC.NCT failed; returning to the text shell.\n");
		return -1;
	}
	selected = boots_env_get(&boot_environment, "BOOT_ACTION");
	if (!valid_boot_action(selected) ||
	    !strcopy(action, selected, sizeof(action))) {
		(void)boots_env_unset(&boot_environment, "BOOT_ACTION");
		puts("AUTOEXEC.NCT did not select a valid BOOT_ACTION.\n");
		return -1;
	}
	(void)boots_env_unset(&boot_environment, "BOOT_ACTION");
	if (!command(action)) {
		puts("BOOT_ACTION failed: ");
		puts(action);
		putc('\n');
		return -1;
	}
	return 1;
}

static void draw_startup_header(void)
{
	boots_console_write_at(0, 0, boots_msg_machine);
	boots_console_write_at(2, 0, boots_msg_loader);
	boots_console_write_at(3, 0, boots_msg_copyright);
	boots_console_write_at(5, 0, boots_msg_probing);
}

static void draw_probe_bar(unsigned current, unsigned total)
{
	char filled[BOOTS_CONSOLE_COLUMNS + 1U];
	char empty[BOOTS_CONSOLE_COLUMNS + 1U];
	unsigned columns = total ? current * BOOTS_CONSOLE_COLUMNS / total : 0;
	unsigned index;

	if (columns > BOOTS_CONSOLE_COLUMNS)
		columns = BOOTS_CONSOLE_COLUMNS;
	for (index = 0; index < columns; index++)
		filled[index] = ' ';
	filled[index] = 0;
	for (index = 0; index < BOOTS_CONSOLE_COLUMNS - columns; index++)
		empty[index] = ' ';
	empty[index] = 0;
	boots_console_put_sjis_at(BOOTS_CONSOLE_ROWS - 1U, 0,
				   (const uint8_t *)filled,
				   BOOTS_CONSOLE_NORMAL_ATTRIBUTE | 0x04U);
	if (columns < BOOTS_CONSOLE_COLUMNS)
		boots_console_put_sjis_at(BOOTS_CONSOLE_ROWS - 1U, columns,
					   (const uint8_t *)empty,
					   BOOTS_CONSOLE_NORMAL_ATTRIBUTE);
}

static void draw_probe_progress(unsigned current, unsigned total,
				uint8_t device_class, uint8_t bios_id)
{

	boots_console_clear_row(5);
	boots_console_write_at(5, 0, boots_msg_probing);
	putc(' ');
	puts(device_class == BOOTS_DEV_IDE ? "IDE " : "SCSI ");
	dec((unsigned)bios_id -
	    (device_class == BOOTS_DEV_IDE ? 0x80U : 0xa0U) + 1U);
	puts(" (");
	dec(current);
	putc('/');
	dec(total);
	putc(')');
	draw_probe_bar(current, total);
}

static void draw_automatic_status(const struct startup_state *state)
{
	draw_probe_bar(state->probe_total, state->probe_total);
	boots_console_clear_row(5);
	boots_console_write_at(5, 0, boots_msg_automatic_run);
	if (state->auto_config_kind == STARTUP_CONFIG_AUTOEXEC)
		puts(" AUTOEXEC.NCT");
	else if (state->auto_config_kind == STARTUP_CONFIG_BOOTCFG)
		puts(" BOOTS.CFG");
}

static void draw_startup_menu(const struct startup_state *state)
{
	for (unsigned menu_row = 6; menu_row <= 17; menu_row++)
		boots_console_clear_row(menu_row);
	boots_console_write_at(6, 0, (const uint8_t *)"");
	dec(state->fixed_count);
	puts((const char *)boots_msg_found_suffix);
	boots_console_write_at(8, 0, boots_msg_boot_from);
	boots_console_write_at(9, 0, boots_msg_auto_prefix);
	if (state->phase == STARTUP_DRAW || state->phase == STARTUP_PROBE) {
		puts((const char *)boots_msg_searching);
	} else if (state->auto_kind != STARTUP_AUTO_NONE) {
		puts(devs[state->auto_device].device_class == BOOTS_DEV_FDD ?
		     "FDD " : "HDD ");
		dec(fixed_device_ordinal(state->auto_device));
		puts((const char *)boots_msg_partition);
		dec((unsigned)state->auto_partition + 1);
		if (state->auto_kind == STARTUP_AUTO_CONFIG) {
			if (state->auto_config_kind == STARTUP_CONFIG_AUTOEXEC)
				puts((const char *)boots_msg_run_autoexec);
			else
				puts((const char *)boots_msg_run_cfg);
		}
	} else {
		puts((const char *)boots_msg_unavailable);
	}
	putc(')');

	for (unsigned ordinal = 1; ordinal <= 4; ordinal++) {
		if (menu_device(ordinal) < 0)
			continue;
		boots_console_write_at(9 + ordinal, 0,
					(const uint8_t *)"  ");
		putc((char)('1' + ordinal));
		puts((const char *)boots_msg_fixed_disk_prefix);
		dec(ordinal);
	}
	boots_console_write_at(15, 0, boots_msg_esc_shell);
	boots_console_write_at(17, 0, boots_msg_select);
	update_cursor();
}

static void accept_startup_selection(int key_code)
{
	boots_console_clear_row(BOOTS_CONSOLE_ROWS - 1U);
	if (key_code == 0x1b)
		puts("ESC");
	else if (key_code >= 0)
		putc((char)key_code);
	putc('\n');
	boots_console_set_mode(BOOTS_CONSOLE_TERMINAL);
}

static void chain_menu_device(unsigned ordinal)
{
	int di = menu_device(ordinal);
	if (di < 0) {
		puts("Device is not present.\n");
		return;
	}
	curdev = di;
	curpart = -1;
	kernel_name[0] = kernel_arg[0] = 0;
	puts("Chain boot is not available on the HAL yet.\n");
}

static void chain_automatic_partition(const struct startup_state *state)
{
	if (!activate_automatic_target(state)) {
		puts("Automatic target is no longer readable.\n");
		return;
	}
	puts("Chain boot is not available on the HAL yet.\n");
}

/* Return -1 for an ignored key, zero for Shell, and one for BOOTS.CFG. */
static int handle_startup_key(struct startup_state *state, int key_code)
{
	if (key_code == 0x1b) {
		accept_startup_selection(key_code);
		state->phase = STARTUP_SHELL;
		if (state->auto_kind == STARTUP_AUTO_CONFIG)
			activate_automatic_target(state);
		return 0;
	}
	if (key_code == '1') {
		if (state->auto_kind == STARTUP_AUTO_NONE)
			return -1;
		accept_startup_selection(key_code);
		state->phase = STARTUP_SELECTED;
		if (state->auto_kind == STARTUP_AUTO_CONFIG)
			return activate_automatic_target(state) ? 1 : 0;
		chain_automatic_partition(state);
		state->phase = STARTUP_SHELL;
		return 0;
	}
	if (key_code >= '2' && key_code <= '5') {
		unsigned ordinal = (unsigned)(key_code - '1');

		if (menu_device(ordinal) < 0)
			return -1;
		accept_startup_selection(key_code);
		state->phase = STARTUP_SELECTED;
		chain_menu_device(ordinal);
		state->phase = STARTUP_SHELL;
		return 0;
	}
	return -1;
}

static int pending_startup_key(void)
{
	return poll() >= 0 ? key() : -1;
}

/* Process one stable candidate; at most one invocation reaches INT 1Bh. */
static void probe_next_startup_device(struct startup_state *state)
{
	unsigned candidate;
	uint8_t device_class;
	uint8_t bios_id;
	int new_device;

	for (;;) {
		if (state->next_candidate >= MAX_FIXED_DEVICES)
			return;
		candidate = state->next_candidate++;
		if (candidate < MAX_IDE_DEVICES) {
			device_class = BOOTS_DEV_IDE;
			bios_id = 0x80 + candidate;
			if (state->ide_bitmap & (1U << candidate))
				break;
			continue;
		}
		device_class = BOOTS_DEV_SCSI;
		bios_id = 0xa0 + candidate - MAX_IDE_DEVICES;
		if (state->scsi_bitmap &
		    (1U << (candidate - MAX_IDE_DEVICES)))
			break;
	}
	state->probe_done++;
	draw_probe_progress(state->probe_done, state->probe_total,
			    device_class, bios_id);
	new_device = probe_fixed_device(device_class, bios_id);
	state->fixed_count = device_count;
	if (new_device >= 0)
		consider_automatic_device(state, new_device);
}

/* Explicit cooperative startup state machine.  BIOS SENSE itself may block,
 * but keyboard input is checked immediately before and after every candidate. */
static int startup_menu(struct startup_state *state)
{
	curdev = curpart = -1;
	boots_namespace_init(&mounted_namespace);
	state->phase = STARTUP_DRAW;
	state->next_candidate = 0;
	state->ide_bitmap = ide_reported_drives();
	state->scsi_bitmap = scsi_reported_targets();
	state->probe_total = bit_count(state->ide_bitmap) +
			     bit_count(state->scsi_bitmap);
	state->probe_done = 0;
	state->fixed_count = device_count;
	state->auto_device = state->auto_partition = -1;
	state->auto_priority = 4;
	state->auto_kind = STARTUP_AUTO_NONE;
	state->auto_config_kind = STARTUP_CONFIG_NONE;
	state->automatic_cancelled = 0;
	state->timeout_start = -1;
	state->timeout_budget = 0x20000;

	boots_console_reset();
	draw_startup_header();
	for (unsigned device = 0; device < device_count; device++)
		consider_automatic_device(state, device);
	draw_startup_menu(state);
	state->phase = STARTUP_PROBE;
	for (;;) {
		int key_code;
		int result;

		if (state->phase == STARTUP_PROBE) {
			key_code = pending_startup_key();
			if (key_code >= 0) {
				state->automatic_cancelled = 1;
				if ((result = handle_startup_key(state, key_code)) >= 0)
					return result;
			}
			if (state->next_candidate < MAX_FIXED_DEVICES) {
				probe_next_startup_device(state);
				draw_startup_menu(state);
				key_code = pending_startup_key();
				if (key_code >= 0) {
					state->automatic_cancelled = 1;
					if ((result = handle_startup_key(state,
								 key_code)) >= 0)
						return result;
				}
				continue;
			}
			state->phase = STARTUP_TIMEOUT;
			state->timeout_start = clock_second();
			if (!state->automatic_cancelled &&
			    state->auto_kind != STARTUP_AUTO_NONE)
				draw_automatic_status(state);
			draw_startup_menu(state);
			continue;
		}

		if (state->auto_kind == STARTUP_AUTO_NONE ||
		    state->automatic_cancelled) {
			key_code = key();
			result = handle_startup_key(state, key_code);
			if (result >= 0)
				return result;
			continue;
		}

		key_code = pending_startup_key();
		if (key_code >= 0) {
			state->automatic_cancelled = 1;
			if ((result = handle_startup_key(state, key_code)) >= 0)
				return result;
			continue;
		}
		int now = clock_second();
		if ((state->timeout_start >= 0 && now >= 0 &&
		     (now - state->timeout_start + 60) % 60 >=
		     STARTUP_TIMEOUT_SECONDS) || !--state->timeout_budget) {
			accept_startup_selection(-1);
			state->phase = STARTUP_SELECTED;
			if (state->auto_kind == STARTUP_AUTO_CONFIG)
				return activate_automatic_target(state) ? 1 : 0;
			chain_automatic_partition(state);
			state->phase = STARTUP_SHELL;
			return 0;
		}
	}
}

/* Validated Stage 1 handoff and top-level Stage 2 command loop. */
void boots_main(const struct boots_handoff *h)
{
	char b[LINE_MAX];
	ho = h;
	if (!h || h->magic != BOOTS_HANDOFF_MAGIC || h->version != 1 ||
	    h->size < sizeof(*h) || !h->device_count || !h->device_table)
		for (;;)
			asm volatile("cli; hlt");
	device_count = 0;
	const struct boots_device *initial =
		(const struct boots_device *)h->device_table;
	for (unsigned i = 0; i < h->device_count &&
	     device_count < MAX_FIXED_DEVICES; i++) {
		if ((initial[i].device_class != BOOTS_DEV_FDD &&
		     initial[i].device_class != BOOTS_DEV_IDE &&
		     initial[i].device_class != BOOTS_DEV_SCSI) ||
		    !(initial[i].flags & BOOTS_DEV_PRESENT))
			continue;
		discovered_devices[device_count++] = initial[i];
	}
	devs = discovered_devices;
	/* Native disk path: probe the IDE banks and adopt the PC-98
	 * partition-table scheme.  The firmware-sensed geometry rides in
	 * from the device table for partition interpretation. */
	boots_partition_set_scheme(&boots_partition_scheme_pc98);
	boots_blkdev_reset();
	(void)boots_ide_pc98_init(devs, device_count);
	/* Stage 2 owns the machine one-to-one, so the Core-Graph aperture is
	 * addressed directly and needs no memory manager. */
	noct_beui_pc98_auto_default(&beui_display, beui_display_reset,
				     beui_display_stop, NULL, beui_port_in8,
				     beui_port_out8, NULL,
				     (volatile uint8_t *)CIRRUS_APERTURE);
	if (noct_beui_pc98_auto_make_hal(&beui_hal, &beui_display)) {
		install_display_proxy();
		beui_hal.clock.context = NULL;
		beui_hal.clock.milliseconds = boots_kernel_milliseconds;
		beui_hal.input.context = NULL;
		beui_hal.input.is_key_down = noct_key_is_down;
		beui_hal.input.drain = noct_key_drain;
		boots_noct_set_beui_hal(&beui_hal);
	}
	boots_env_init(&boot_environment);
	(void)boots_env_set(&boot_environment, "HOME", "/");
	(void)boots_env_set(&boot_environment, "REMACS_SKK_DICT",
			     "/skkjisyo.dic");
	for (;;) {
		struct startup_state startup;

		(void)boots_pc98_display_text_restore();
		int automatic = startup_menu(&startup);
		if (curpart >= 0) {
			puts("source: ");
			devname(curdev);
			putc(':');
			puts(parts[curpart].name);
			putc('\n');
		}
		if (automatic) {
			int autoexec = curpart >= 0 ? run_autoexec() : -1;

			if (autoexec == 0) {
				const char *cfg_name = startup_config_file();
				char source_cfg[24] = "source ";
				unsigned at = 7;

				if (!cfg_name)
					cfg_name = "BOOTS.CFG";
				for (unsigned i = 0; cfg_name[i] &&
				     at + 1 < sizeof(source_cfg); i++)
					source_cfg[at++] = cfg_name[i];
				source_cfg[at] = 0;
				if (!command(source_cfg)) {
					puts(cfg_name);
					puts(" automatic boot failed.\n");
				}
			}
		}
		for (;;) {
			prompt();
			if (line(b) < 0)
				break;
			if (!command(b))
				puts("error\n");
		}
	}
}
