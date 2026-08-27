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
#include "kern/boot-parameters.h"
#include "kern/klog.h"
#include "kern/sched.h"
#include "kern/vm-commit.h"
#include "kern/vm-reclaim.h"

#ifndef ZEDBSD_INIT_PATH
#define ZEDBSD_INIT_PATH "/sbin/init"
#endif

/*
 * Stage 2 runs without a C library or operating-system services.  The request
 * object is the sole mutable argument passed through the real-mode BIOS
 * gateway in Stage 1.
 */
const struct boot_handoff *ho;
static struct boot_handoff handoff_snapshot;
const struct boot_device *devs;
unsigned device_count;

struct bootfs mounted_fs;
struct bootfs_namespace mounted_namespace;
struct environment boot_environment;
struct part parts[MAX_PARTS];
int curdev = -1, curpart = -1;

/* Minimal freestanding string and memory primitives. */
void
memzero(void *p, uint32_t n)
{
	uint8_t *q = p;
	while (n--)
		*q++ = 0;
}
void
memcopy(void *d, const void *s, uint32_t n)
{
	uint8_t *a = d;
	const uint8_t *b = s;
	while (n--)
		*a++ = *b++;
}
int
streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}
unsigned
slen(const char *s)
{
	unsigned n = 0;
	while (s[n])
		n++;
	return n;
}
int
strcopy(char *destination, const char *source, unsigned capacity)
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

void
update_cursor(void)
{
	hal_cons_update_cursor();
}

void
putc(char c)
{
	hal_cons_putc((uint8_t)c);
}
void
puts(const char *s)
{
	hal_cons_write(s);
}
void
hex8(uint8_t v)
{
	const char *h = "0123456789ABCDEF";
	putc(h[v >> 4]);
	putc(h[v & 15]);
}
void
dec(unsigned v)
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

uint64_t clock_ticks(void);

/* Validated Stage 1 handoff and top-level Stage 2 command loop. */
void
kernel_main(const struct boot_handoff *h,
	    const struct boot_device *platform_devices,
	    unsigned platform_device_count)
{
	const struct kern_boot_parameters *boot_parameters;
	const char *boot_parameter_line;
	const char *init_path;
	int error;

	/* PC-98 Stage 1 places its handoff below 1 MiB.  User address spaces do
	 * not retain that identity mapping, so persistent kernel services must
	 * refer to a kernel-owned copy after init has started. */
	memcopy(&handoff_snapshot, h, sizeof(handoff_snapshot));
	ho = &handoff_snapshot;
	device_count = platform_device_count;
	devs = platform_devices;
	env_init(&boot_environment);
	vm_reclaim_init();
	boot_parameter_line = hal_get_arch_handoff("boot.command-line");
	hal_printf("boot: parameters: %s\n",
	    boot_parameter_line != NULL ? boot_parameter_line : "");
	kern_logf("boot: parameters: %s\n",
	    boot_parameter_line != NULL ? boot_parameter_line : "");
	error = kern_boot_parameters_initialize(boot_parameter_line,
	    boot_parameter_line != NULL ? KERN_BOOT_PARAMETERS_STORAGE_SIZE : 0U);
	if (error != 0) {
		hal_printf("boot: parameter parsing failed (%d); entering idle.\n",
			   error);
		kern_logf("boot: parameter parsing failed (%d); entering idle.\n",
			  error);
	} else {
		boot_parameters = kern_boot_parameters_current();
		if (kern_boot_parameters_unknown_count(boot_parameters) != 0U) {
			int truncated;
			const char *name = kern_boot_parameters_unknown_name(
			    boot_parameters, &truncated);
			unsigned count = kern_boot_parameters_unknown_count(
			    boot_parameters);

			hal_printf("boot: ignored %u unknown parameter%s; "
				   "first=%s%s\n",
			    count, count == 1U ? "" : "s", name,
			    truncated ? "..." : "");
			kern_logf("boot: ignored %u unknown parameter%s; "
				  "first=%s%s\n",
			    count, count == 1U ? "" : "s", name,
			    truncated ? "..." : "");
		}
		hal_printf("boot: VFS initialization\n");
		kern_logf("boot: VFS initialization\n");
		error = kern_vfs_init(h, platform_devices,
		    platform_device_count);
		if (error != 0) {
			hal_printf("VFS initialization failed (%d); "
				   "entering idle.\n",
				   error);
			kern_logf("VFS initialization failed (%d); "
				  "entering idle.\n",
				  error);
		} else {
			error = vm_commit_init();
			if (error != 0) {
				hal_printf("VM commit initialization failed (%d); "
					   "entering idle.\n",
					   error);
				kern_logf("VM commit initialization failed (%d); "
					  "entering idle.\n",
					  error);
			} else {
				int init_error;

				init_path = kern_boot_parameters_init_path(
				    boot_parameters);
#if !defined(HAL_ARCH_I386) && !defined(HAL_ARCH_AMD64)
				if (!kern_boot_parameters_source_present())
					init_path = ZEDBSD_INIT_PATH;
#endif
				hal_printf("boot: starting init %s\n", init_path);
				kern_logf("boot: starting init %s\n\n", init_path);
				init_error = kern_init_start(init_path);
				if (init_error != 0) {
					hal_printf("init %s not started (%d); "
						   "entering idle.\n",
					    init_path, init_error);
					kern_logf("init %s not started (%d); "
						  "entering idle.\n",
						  init_path, init_error);
				}
			}
		}
	}
	sched_idle();
}
