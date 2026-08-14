/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/clock.h"
#include "hal/hal.h"
#include "kern/platform.h"
#include "kern/internal.h"
#include "kern/vfs.h"
#include "kern/exec.h"
#include "kern/init.h"
#include "kern/sched.h"
#include "kern/vm-commit.h"

#ifndef ZEDBSD_INIT_PATH
#define ZEDBSD_INIT_PATH "/bin/sh"
#endif


/*
 * Stage 2 runs without a C library or operating-system services.  The request
 * object is the sole mutable argument passed through the real-mode BIOS
 * gateway in Stage 1.
 */
const struct zedbsd_handoff *ho;
const struct zedbsd_device *devs;
unsigned device_count;

struct zedbsd_filesystem mounted_fs;
struct zedbsd_namespace mounted_namespace;
struct zedbsd_environment boot_environment;
struct part parts[MAX_PARTS];
int curdev = -1, curpart = -1;
char kernel_name[ZEDBSD_PATH_MAX], kernel_arg[256];

/* Minimal freestanding string and memory primitives. */
void memzero(void *p, uint32_t n)
{
	uint8_t *q = p;
	while (n--)
		*q++ = 0;
}
void memcopy(void *d, const void *s, uint32_t n)
{
	uint8_t *a = d;
	const uint8_t *b = s;
	while (n--)
		*a++ = *b++;
}
int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}
unsigned slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}
int strcopy(char *destination, const char *source, unsigned capacity)
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

void update_cursor(void)
{
	hal_cons_update_cursor();
}

void putc(char c)
{
	hal_cons_putc((uint8_t)c);
}
void puts(const char *s)
{
	hal_cons_write(s);
}
void hex8(uint8_t v)
{
	const char *h = "0123456789ABCDEF";
	putc(h[v >> 4]);
	putc(h[v & 15]);
}
void dec(unsigned v)
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

uint64_t zedbsd_kernel_ticks(void);

/* Validated Stage 1 handoff and top-level Stage 2 command loop. */
void kernel_main(const struct zedbsd_handoff *h,
		 const struct zedbsd_device *platform_devices,
		 unsigned platform_device_count)
{
	int error;

	ho = h;
	device_count = platform_device_count;
	devs = platform_devices;
	hal_printf("boot: graphics service initialization\n");
	if (!kern_platform_graphics_init(zedbsd_kernel_milliseconds, NULL, NULL))
		hal_printf("boot: graphics service unavailable\n");
	zedbsd_env_init(&boot_environment);
	hal_printf("boot: VFS initialization\n");
	error = kern_vfs_init(h, platform_devices, platform_device_count);
	if (error != 0)
		hal_printf("VFS initialization failed (%d); entering idle.\n",
		    error);
	else {
		error = vm_commit_init();
		if (error != 0)
			hal_printf("VM commit initialization failed (%d); "
			    "entering idle.\n", error);
		else {
			hal_printf("boot: starting init %s\n\n", ZEDBSD_INIT_PATH);
			{
				int init_error = kern_init_start();
				if (init_error != 0)
					hal_printf("init not started (%d); entering idle.\n",
					    init_error);
			}
		}
	}
	sched_idle();
}
