/*
 * KA-T110: kernel-owned boot metadata lifetime
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <kern/boot.h>
#include <kern/kernel.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static unsigned reclaim_initializations;
static unsigned parser_initializations;
static unsigned vfs_initializations;
static unsigned commit_initializations;
static unsigned init_starts;
static unsigned idle_entries;
static const struct boot_handoff *vfs_handoff;
static const struct boot_device *vfs_devices;
static unsigned vfs_device_count;
static char selected_init[64];
static jmp_buf idle_return;
static struct kern_boot_parameters parameters;

#define CHECK(expression)                                                     \
	do {                                                                   \
		checks++;                                                       \
		if (!(expression)) {                                           \
			fprintf(stderr, "KA-T110: failed at %s:%d: %s\n",      \
			    __FILE__, __LINE__, #expression);                    \
			exit(1);                                               \
		}                                                              \
	} while (0)

void *
hal_get_arch_handoff(const char *name)
{
	CHECK(name != NULL);
	CHECK(strcmp(name, "boot.command-line") == 0);
	return (void *)"";
}

int
hal_printf(const char *format, ...)
{
	CHECK(format != NULL);
	return 0;
}

void
kern_logf(const char *format, ...)
{
	CHECK(format != NULL);
}

void
vm_reclaim_init(void)
{
	reclaim_initializations++;
}

int
kern_boot_parameters_initialize(const char *input, size_t capacity)
{
	CHECK(input != NULL);
	CHECK(capacity == KERN_BOOT_PARAMETERS_STORAGE_SIZE);
	parser_initializations++;
	return 0;
}

const struct kern_boot_parameters *
kern_boot_parameters_current(void)
{
	return &parameters;
}

unsigned
kern_boot_parameters_unknown_count(const struct kern_boot_parameters *input)
{
	CHECK(input == &parameters);
	return 0;
}

const char *
kern_boot_parameters_unknown_name(const struct kern_boot_parameters *input,
	int *truncated)
{
	CHECK(input == &parameters);
	CHECK(truncated != NULL);
	*truncated = 0;
	return "";
}

const char *
kern_boot_parameters_init_path(const struct kern_boot_parameters *input)
{
	CHECK(input == &parameters);
	return "/sbin/init";
}

int
kern_boot_parameters_source_present(void)
{
	return 1;
}

int
kern_vfs_init(const struct boot_handoff *handoff,
	const struct boot_device *devices, unsigned device_count)
{
	vfs_initializations++;
	vfs_handoff = handoff;
	vfs_devices = devices;
	vfs_device_count = device_count;
	return 0;
}

int
vm_commit_init(void)
{
	commit_initializations++;
	return 0;
}

int
kern_init_start(const char *path)
{
	init_starts++;
	CHECK(path != NULL);
	CHECK(strlen(path) < sizeof(selected_init));
	strcpy(selected_init, path);
	return 0;
}

void
sched_idle(void)
{
	idle_entries++;
	longjmp(idle_return, 1);
}

static void
run_kernel_main(const struct boot_handoff *handoff,
	const struct boot_device *devices, unsigned device_count)
{
	if (setjmp(idle_return) == 0)
		kernel_main(handoff, devices, device_count);
}

static void
fill_device(struct boot_device *device, unsigned seed)
{
	memset(device, 0, sizeof(*device));
	device->device_class = (uint8_t)(1U + seed);
	device->display_index = (uint8_t)(2U + seed);
	device->bios_id = (uint8_t)(0x80U + seed);
	device->flags = (uint8_t)(3U + seed);
	device->sector_size = (uint16_t)(512U << seed);
	device->cylinders = (uint16_t)(1024U + seed);
	device->heads = (uint8_t)(16U + seed);
	device->sectors = (uint8_t)(32U + seed);
	device->controller_location = (uint8_t)(4U + seed);
}

static void
check_device(const struct boot_device *actual,
	const struct boot_device *expected)
{
	CHECK(actual != NULL);
	CHECK(actual != expected);
	CHECK(memcmp(actual, expected, sizeof(*actual)) == 0);
}

int
main(void)
{
	struct boot_handoff handoff;
	struct boot_handoff second_handoff;
	struct boot_device devices[3];
	struct boot_device expected[3];
	struct boot_device second_device;
	struct boot_device second_expected;
	unsigned index;

	CHECK(kern_boot_bios_id() == 0);
	CHECK(kern_boot_device_count() == 0);
	CHECK(kern_boot_device_at(0) == NULL);
	CHECK(kern_boot_device_at(UINT32_MAX) == NULL);

	memset(&handoff, 0, sizeof(handoff));
	handoff.magic = ZEDBSD_HANDOFF_MAGIC;
	handoff.version = ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	handoff.size = sizeof(handoff);
	handoff.boot_bios_id = 0x82U;
	for (index = 0; index < 3; index++) {
		fill_device(&devices[index], index);
		expected[index] = devices[index];
	}
	run_kernel_main(&handoff, devices, 3);

	CHECK(kern_boot_bios_id() == 0x82U);
	CHECK(kern_boot_device_count() == 3);
	CHECK(kern_boot_device_at(0) == &devices[0]);
	for (index = 0; index < 3; index++)
		check_device(kern_boot_device_at(index), &expected[index]);
	CHECK(kern_boot_device_at(3) == NULL);
	CHECK(kern_boot_device_at(UINT32_MAX) == NULL);
	CHECK(vfs_handoff == &handoff);
	CHECK(vfs_devices == devices);
	CHECK(vfs_device_count == 3);
	CHECK(strcmp(selected_init, "/sbin/init") == 0);

	/* The handoff is loader-owned on entry and must be snapshotted.  The
	 * device table is already the static kernel-owned table in kernel_entry;
	 * this host caller likewise keeps its table alive for the test. */
	memset(&handoff, 0xa5, sizeof(handoff));
	CHECK(kern_boot_bios_id() == 0x82U);
	for (index = 0; index < 3; index++)
		check_device(kern_boot_device_at(index), &expected[index]);

	memset(&second_handoff, 0, sizeof(second_handoff));
	second_handoff.magic = ZEDBSD_HANDOFF_MAGIC;
	second_handoff.version = ZEDBSD_HANDOFF_VERSION_MULTIBOOT;
	second_handoff.size = sizeof(second_handoff);
	second_handoff.boot_bios_id = 0x90U;
	fill_device(&second_device, 5);
	second_expected = second_device;
	run_kernel_main(&second_handoff, &second_device, 1);
	memset(&second_handoff, 0, sizeof(second_handoff));
	CHECK(kern_boot_bios_id() == 0x90U);
	CHECK(kern_boot_device_count() == 1);
	CHECK(kern_boot_device_at(0) == &second_device);
	check_device(kern_boot_device_at(0), &second_expected);
	CHECK(kern_boot_device_at(1) == NULL);

	CHECK(reclaim_initializations == 2);
	CHECK(parser_initializations == 2);
	CHECK(vfs_initializations == 2);
	CHECK(commit_initializations == 2);
	CHECK(init_starts == 2);
	CHECK(idle_entries == 2);

	printf("KA-T110: PASS (%u boot-metadata checks)\n", checks);
	return 0;
}
