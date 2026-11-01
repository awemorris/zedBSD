/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the nvrtx GSP parts that touch no hardware.
 *
 * usage: nvrtx-gsp-host-test [FIRMWARE-DIR]
 * FIRMWARE-DIR is a verified nvidia-gsp-firmware cache of the Turing,
 * Ampere and Ada set (the file names of userland/firmware/nvidia-gsp/Makefile);
 * without it, or for a file missing from it, the checks of the real files'
 * containers are skipped and counted as skipped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/display/nvrtx-disp.h"
#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/nvrtx-mem.h"
#include "drivers/gpu/nvrtx/nvrtx-private.h"
#include "drivers/gpu/nvrtx/nvrtx-work.h"

static unsigned failures;
static unsigned checks;
/* Checks of the real files not run because a file (or the whole cache) is missing. */
static unsigned skipped;

static void
check(int ok, const char *what)
{
	checks++;
	if (!ok) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static uint8_t *
read_file(const char *dir, const char *name, size_t *size)
{
	char path[1024];
	FILE *f;
	uint8_t *data;
	long length;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "rb");
	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	length = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc((size_t)length);
	if (data == NULL || fread(data, 1, (size_t)length, f) != (size_t)length) {
		fclose(f);
		free(data);
		return NULL;
	}
	fclose(f);
	*size = (size_t)length;
	return data;
}

/* Chunked pages with made-up bus addresses: chunk c at 0x40000000 + c * 2 * chunk_bytes. */
static void
make_pages(struct nvrtx_pages *pages, size_t page_count, size_t chunk_bytes)
{
	size_t c;

	pages->chunk_bytes = chunk_bytes;
	pages->page_count = page_count;
	pages->chunk_count = (page_count * NVRTX_GSP_PAGE_SIZE + chunk_bytes - 1) / chunk_bytes;
	pages->chunk_cpu = calloc(pages->chunk_count, sizeof(uint8_t *));
	pages->chunk_bus = calloc(pages->chunk_count, sizeof(uint64_t));
	for (c = 0; c < pages->chunk_count; c++) {
		pages->chunk_cpu[c] = aligned_alloc(NVRTX_GSP_PAGE_SIZE, chunk_bytes);
		memset(pages->chunk_cpu[c], 0xa5, chunk_bytes);
		pages->chunk_bus[c] = 0x40000000ULL + (uint64_t)c * 2U * chunk_bytes;
	}
}

static void
test_wire(void)
{
	uint8_t b[16];
	uint8_t element[2 * NVRTX_GSP_PAGE_SIZE];
	unsigned i;

	nvrtx_put64(b, 0, 0x1122334455667788ULL);
	check(b[0] == 0x88 && b[7] == 0x11, "wire: put64 little-endian");
	check(nvrtx_get64(b, 0) == 0x1122334455667788ULL, "wire: get64");
	nvrtx_put32(b, 8, 0xdeadbeefU);
	check(nvrtx_get32(b, 8) == 0xdeadbeefU, "wire: get32");

	for (i = 0; i < sizeof(element); i++)
		element[i] = (uint8_t)(i * 7U + 3U);
	nvrtx_put32(element, NVRTX_ELEMENT_CHECKSUM_AT, 0);
	nvrtx_put32(element, NVRTX_ELEMENT_CHECKSUM_AT, nvrtx_fold_checksum(element, sizeof(element)));
	check(nvrtx_fold_checksum(element, sizeof(element)) == 0, "wire: finished element folds to zero");
}

static void
test_layout(void)
{
	struct nvrtx_wpr_layout l;
	struct nvrtx_bootloader bl;
	struct nvrtx_wpr_sources src;
	uint8_t record[NVRTX_WPRM_BYTES];
	uint8_t page[NVRTX_GSP_PAGE_SIZE];
	uint8_t args[NVRTX_RMARGS_BYTES];
	uint8_t log[NVRTX_GSP_LOG_BYTES];
	uint64_t vram;
	int error;

	vram = nvrtx_vram_size((8U << 4) | 10U);
	check(vram == 8ULL << 30, "layout: VRAM size 8 GiB decoded");
	check(nvrtx_vram_size(0x40000000U | (8U << 4) | 10U) == (8ULL << 30) / 16 * 15, "layout: fifteen sixteenths");
	check(nvrtx_vga_workspace(vram, 0) == vram - 0x100000, "layout: invalid VGA register gives the last MiB");
	check(nvrtx_vga_workspace(vram, 0x01fff008U) == 0x1fff00000ULL, "layout: valid VGA register address");
	check(nvrtx_vga_workspace(vram, 0x00100008U) == vram - 0x20000, "layout: low VGA address gives the last 128 KiB");

	error = nvrtx_wpr_layout(vram, vram - 0x100000, 0x1000, 0x1b34ea0, 0, 64ULL << 20, &l);
	check(error == 0, "layout: 8 GiB fits");
	check(l.vga == 0x1fff00000ULL, "layout: VGA workspace");
	check(l.frts == 0x1ffe00000ULL && l.frts_size == 0x100000, "layout: FRTS");
	check(l.boot == 0x1ffdff000ULL, "layout: bootloader");
	check(l.image == 0x1fe2c0000ULL, "layout: image");
	check(l.heap == 0x1f7900000ULL && l.heap_size == 0x6900000ULL, "layout: heap 105 MiB");
	check(l.region == 0x1f7800000ULL && l.region_size == 0x8700000ULL, "layout: region 135 MiB");
	check(l.outer_heap == 0x1f7700000ULL && l.outer_heap_size == 0x100000, "layout: outer heap");
	check(nvrtx_wpr_layout(vram, vram - 0x100000, 0x1000, 0x40000000ULL, 0, 64ULL << 20, &l) == 0, "layout: (large image still computes)");
	check(nvrtx_wpr_layout(64ULL << 20, (64ULL << 20) - 0x100000, 0x1000, 0x1b34ea0, 0, 64ULL << 20, &l) == EINVAL, "layout: 64 MiB VRAM refused");

	nvrtx_wpr_layout(vram, vram - 0x100000, 0x1000, 0x1b34ea0, 0, 64ULL << 20, &l);
	memset(&bl, 0, sizeof(bl));
	bl.monitor_code_at = 0x11;
	bl.monitor_data_at = 0x22;
	bl.manifest_at = 0x33;
	src.image_table = 0x1000;
	src.boot_address = 0x2000;
	src.boot_size = 0x1000;
	src.signature = 0x3000;
	src.signature_size = 0x1000;
	src.bootloader = &bl;
	nvrtx_wpr_record(record, &l, &src);
	check(nvrtx_get64(record, 0) == 0xdc3aae21371a60b3ULL, "record: magic");
	check(nvrtx_get64(record, 8) == 1, "record: revision");
	check(nvrtx_get64(record, 24) == 0x1b34ea0, "record: image size");
	check(nvrtx_get64(record, 48) == 0x11 && nvrtx_get64(record, 56) == 0x22 && nvrtx_get64(record, 64) == 0x33, "record: bootloader offsets");
	check(nvrtx_get64(record, 88) == 0x1f7700000ULL && nvrtx_get64(record, 96) == 0x1f7700000ULL, "record: reserved start and outer heap");
	check(nvrtx_get64(record, 112) == 0x1f7800000ULL, "record: region start");
	check(nvrtx_get64(record, 168) == 0x1fff00000ULL, "record: region end");
	check(nvrtx_get64(record, 176) == vram, "record: VRAM size");
	check(nvrtx_get64(record, 192) == 0x100000, "record: VGA size");
	check(nvrtx_get64(record, 248) == 0, "record: tail zero");

	memset(page, 0, sizeof(page));
	nvrtx_libos_region(page, 0, "LOGINIT", 0x5000, 0x10000);
	nvrtx_libos_region(page, 3, "RMARGS", 0x6000, NVRTX_RMARGS_REGION_BYTES);
	check(nvrtx_get64(page, 0) == 0x4c4f47494e4954ULL, "libos: name packed first character highest");
	check(nvrtx_get64(page, 8) == 0x5000 && nvrtx_get64(page, 16) == 0x10000, "libos: address and size");
	check(page[24] == 1 && page[25] == 1, "libos: contiguous in system memory");
	check(nvrtx_get64(page, 96) == 0x524d41524753ULL, "libos: fourth entry at 96");
	check(nvrtx_get64(page, 112) == 0x1000, "libos: the arguments' region is one whole page (R3)");
	check(NVRTX_RMARGS_BYTES <= NVRTX_RMARGS_REGION_BYTES, "libos: the arguments fit their region");

	nvrtx_rmargs(args, 0x7000, 129);
	check(nvrtx_get64(args, 0) == 0x7000 && nvrtx_get32(args, 8) == 129, "rmargs: queue pages");
	check(nvrtx_get64(args, 16) == 0x1000 && nvrtx_get64(args, 24) == 0x41000, "rmargs: queue offsets");
	check(args[48] == 1, "rmargs: stack in DMEM");
	check(nvrtx_get32(args, 32) == 0 && args[40] == 0, "rmargs: cold start");

	nvrtx_log_buffer(log, 0x80000, sizeof(log));
	check(nvrtx_get64(log, 0) == 0 && nvrtx_get64(log, 8) == 0x80000 && nvrtx_get64(log, 8 + 15 * 8) == 0x8f000, "log: put pointer and page list");
}

static void
test_radix3(void)
{
	struct nvrtx_pages image;
	struct nvrtx_pages table;
	size_t pages;
	int error;

	make_pages(&image, 6965, 0x100000);
	pages = nvrtx_radix3_pages(6965);
	check(pages == 16, "radix3: 6965 image pages need 16 table pages");
	make_pages(&table, pages, pages * NVRTX_GSP_PAGE_SIZE);
	error = nvrtx_radix3_fill(&image, &table);
	check(error == 0, "radix3: fill");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 0), 0) == nvrtx_pages_bus(&table, 1), "radix3: top names middle");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 0), 8) == 0, "radix3: top rest zero");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 1), 13 * 8) == nvrtx_pages_bus(&table, 15), "radix3: middle names last bottom");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 1), 14 * 8) == 0, "radix3: middle rest zero");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 2), 0) == 0x40000000ULL, "radix3: first image page");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 2), 256 * 8) == 0x40000000ULL + 2 * 0x100000, "radix3: second chunk");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 15), (6964 % 512) * 8) == nvrtx_pages_bus(&image, 6964), "radix3: last image page");
	check(nvrtx_get64(nvrtx_pages_cpu(&table, 15), (6965 % 512) * 8) == 0, "radix3: tail zero");
	table.page_count = 15;
	check(nvrtx_radix3_fill(&image, &table) == EINVAL, "radix3: wrong table size refused");
}

static void
test_queue(void)
{
	struct nvrtx_pages shared;
	struct nvrtx_channel driver;
	struct nvrtx_channel gsp;
	uint8_t *staging_driver;
	uint8_t *staging_gsp;
	uint8_t payload[20000];
	uint8_t received[20000];
	uint8_t *slot;
	uint32_t function;
	uint32_t sequence;
	uint32_t result;
	size_t length;
	unsigned i;
	unsigned round;
	int error;
	int ok;

	make_pages(&shared, NVRTX_QUEUE_SHARED_BYTES / NVRTX_GSP_PAGE_SIZE, NVRTX_QUEUE_SHARED_BYTES);
	check(shared.page_count == 129, "queue: shared region is 129 pages");
	nvrtx_queue_shared_init(&shared);
	check(nvrtx_get64(nvrtx_pages_cpu(&shared, 0), 128 * 8) == nvrtx_pages_bus(&shared, 128), "queue: page list");
	slot = nvrtx_pages_cpu(&shared, 1);
	check(nvrtx_get32(slot, 4) == 0x40000 && nvrtx_get32(slot, 8) == 0x1000 && nvrtx_get32(slot, 12) == 63, "queue: command header size, slot, count");
	check(nvrtx_get32(slot, 20) == 1 && nvrtx_get32(slot, 24) == 32 && nvrtx_get32(slot, 28) == 0x1000, "queue: command header flags, reader, slots");

	staging_driver = malloc(NVRTX_STAGING_BYTES);
	staging_gsp = malloc(NVRTX_STAGING_BYTES);
	nvrtx_channel_init(&driver, &shared, staging_driver);
	/* The GSP's mirror: it sends in the status queue and receives from the command queue. */
	nvrtx_channel_init(&gsp, &shared, staging_gsp);
	gsp.command.at = NVRTX_QUEUE_STATUS_AT;
	gsp.command.peer_at = NVRTX_QUEUE_COMMAND_AT;
	gsp.status.at = NVRTX_QUEUE_COMMAND_AT;
	gsp.status.peer_at = NVRTX_QUEUE_STATUS_AT;

	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(i * 13U + 1U);

	error = nvrtx_channel_send(&driver, 72, 0, payload, 100);
	check(error == 0, "queue: send one slot");
	slot = nvrtx_pages_cpu(&shared, 2);
	check(nvrtx_fold_checksum(slot, NVRTX_GSP_PAGE_SIZE) == 0, "queue: element checksum folds to zero");
	check(nvrtx_get32(slot, 40) == 1 && nvrtx_get32(slot, 36) == 0, "queue: element slots and number");
	check(nvrtx_get32(slot, 48) == 0x03000000 && memcmp(slot + 52, "VRPC", 4) == 0, "queue: call version and signature");
	check(nvrtx_get32(slot, 56) == 132 && nvrtx_get32(slot, 60) == 72, "queue: call length and function");
	check(nvrtx_get32(slot, 64) == 0xffffffffU, "queue: result pending");
	check(nvrtx_get32(nvrtx_pages_cpu(&shared, 1), 16) == 1, "queue: write pointer moved");

	error = nvrtx_channel_receive(&gsp, received, sizeof(received), &length);
	check(error == 0 && length == 100 && memcmp(received, payload, 100) == 0, "queue: GSP side receives");
	check(nvrtx_get32(nvrtx_pages_cpu(&shared, 65), 32) == 1, "queue: GSP read pointer in the status page");

	check(nvrtx_channel_send(&driver, 1, 1, payload, NVRTX_CALL_PAYLOAD_MAX + 1) == E2BIG, "queue: too large refused");

	/* Fills the command queue without the GSP reading: 62 slots usable. */
	ok = 1;
	for (i = 0; i < 62; i++) {
		if (nvrtx_channel_send(&driver, 76, i, payload, 10) != 0)
			ok = 0;
	}
	check(ok, "queue: 62 one-slot messages fit");
	check(nvrtx_channel_send(&driver, 76, 99, payload, 10) == EAGAIN, "queue: full queue says EAGAIN");
	ok = 1;
	for (i = 0; i < 62; i++) {
		error = nvrtx_channel_peek(&gsp, &function, &sequence, &length, &result);
		if (error != 0 || sequence != i || function != 76)
			ok = 0;
		nvrtx_channel_receive(&gsp, received, sizeof(received), &length);
	}
	check(ok, "queue: GSP drains in order across the wrap");

	/* Multi-slot replies that wrap, answered by the GSP into the status queue. */
	ok = 1;
	for (round = 0; round < 20; round++) {
		error = nvrtx_channel_send(&gsp, 0x1000 + round, round, payload, 9000 + round * 400);
		if (error != 0)
			ok = 0;
		error = nvrtx_channel_peek(&driver, &function, &sequence, &length, &result);
		if (error != 0 || function != 0x1000 + round || sequence != round || length != 9000 + round * 400)
			ok = 0;
		memset(received, 0, sizeof(received));
		error = nvrtx_channel_receive(&driver, received, sizeof(received), &length);
		if (error != 0 || memcmp(received, payload, length) != 0)
			ok = 0;
	}
	check(ok, "queue: multi-slot messages wrap and arrive whole");
	check(nvrtx_channel_peek(&driver, &function, &sequence, &length, &result) == EAGAIN, "queue: empty status queue says EAGAIN");

	error = nvrtx_channel_send(&gsp, 5, 5, payload, 5000);
	error = nvrtx_channel_receive(&driver, received, 100, &length);
	check(error == 0 && length == 5000 && memcmp(received, payload, 100) == 0, "queue: a short buffer takes the head and the message");
}

/* Names a firmware file of a family record as the cache holds it: tu102's files bare, the others after their directory. */
static void
cache_name(const char *path, char *name, size_t capacity)
{
	const char *slash;
	const char *base;
	size_t directory;

	slash = strchr(path, '/');
	base = strrchr(path, '/');
	directory = (size_t)(slash - path);
	if (directory == 5 && strncmp(path, "tu102", 5) == 0)
		snprintf(name, capacity, "%s", base + 1);
	else
		snprintf(name, capacity, "%.*s-%s", (int)directory, path, base + 1);
}

/* Reads one firmware file of a family record from the cache; a missing file is counted as skipped. */
static uint8_t *
read_profile_file(const char *dir, const char *path, size_t *size)
{
	char name[256];
	uint8_t *data;

	cache_name(path, name, sizeof(name));
	data = read_file(dir, name, size);
	if (data == NULL) {
		printf("skip files: %s (%s) not in the cache\n", path, name);
		skipped++;
	}
	return data;
}

/* Checks one booter of a family: it parses, and a boot-ROM family's booter carries the meta and a fuse bank. */
static void
check_booter(const char *dir, const struct nvrtx_profile *profile, const char *path)
{
	struct nvrtx_booter booter;
	uint8_t *data;
	size_t size;
	size_t at;
	unsigned index;
	char what[160];
	int error;

	data = read_profile_file(dir, path, &size);
	if (data == NULL)
		return;
	error = nvrtx_booter_parse(data, size, &booter);
	snprintf(what, sizeof(what), "files %s: %s parses", profile->name, path);
	check(error == 0, what);
	if (error != 0) {
		free(data);
		return;
	}
	snprintf(what, sizeof(what), "files %s: %s signatures in the file", profile->name, path);
	check(booter.signature_count >= 1 && booter.signature_size > 0 &&
	    nvrtx_booter_signature(&booter, booter.signature_count - 1U, &at) == 0 && at + booter.signature_size <= size, what);
	if (profile->falcon_style == NVRTX_FALCON_BOOT_ROM) {
		snprintf(what, sizeof(what), "files %s: %s carries the boot ROM's meta with a fuse bank", profile->name, path);
		check(booter.has_meta != 0 && nvrtx_booter_fuse_register(booter.engine_id, booter.ucode_id) != 0U, what);
		snprintf(what, sizeof(what), "files %s: %s unfused GPU takes the last signature", profile->name, path);
		check(nvrtx_booter_fuse_index(&booter, 0U, &index) == 0 && index == booter.signature_count - 1U, what);
		snprintf(what, sizeof(what), "files %s: %s fused beyond its version refused", profile->name, path);
		check(nvrtx_booter_fuse_index(&booter, 1U << (booter.fuse_version + 1U), &index) == EINVAL, what);
		printf("info %s %s: meta fuse %u engine 0x%x ucode %u, fuse register 0x%06x, %u signatures\n",
		    profile->name, path, booter.fuse_version, booter.engine_id, booter.ucode_id,
		    nvrtx_booter_fuse_register(booter.engine_id, booter.ucode_id), booter.signature_count);
	}
	free(data);
}

/* Checks a family's firmware set: the image's sections, the booters, the bootloaders. */
static void
check_profile_files(const char *dir, unsigned ident0)
{
	const struct nvrtx_profile *profile;
	struct nvrtx_bootloader bl;
	struct nvrtx_falcon_bl fbl;
	struct nvrtx_section section;
	struct nvrtx_chip chip;
	uint8_t *data;
	size_t size;
	char what[160];
	int error;

	nvrtx_identify(ident0, 0U, &chip);
	profile = chip.profile;
	check(profile != NULL, "files: the chip has a family record");
	if (profile == NULL)
		return;

	data = read_profile_file(dir, profile->image_file, &size);
	if (data != NULL) {
		error = nvrtx_elf_section(data, size, NVRTX_GSP_IMAGE_SECTION, &section);
		snprintf(what, sizeof(what), "files %s: %s has .fwimage", profile->name, profile->image_file);
		check(error == 0 && section.size > 0 && section.at + section.size <= size, what);
		printf("info %s: .fwimage of %zu bytes\n", profile->name, section.size);
		error = nvrtx_elf_section(data, size, profile->signature_section, &section);
		snprintf(what, sizeof(what), "files %s: %s has %s", profile->name, profile->image_file, profile->signature_section);
		check(error == 0 && section.size > 0, what);
		check(nvrtx_elf_section(data, size, ".nosuch", &section) == ENOENT, "files: missing section");
		check(nvrtx_elf_section(data, 63, NVRTX_GSP_IMAGE_SECTION, &section) == EINVAL, "files: truncated ELF refused");
		free(data);
	}

	check_booter(dir, profile, profile->booter_load_file);
	check_booter(dir, profile, profile->booter_unload_file);

	data = read_profile_file(dir, profile->bootloader_file, &size);
	if (data != NULL) {
		snprintf(what, sizeof(what), "files %s: %s parses", profile->name, profile->bootloader_file);
		check(nvrtx_bootloader_parse(data, size, &bl) == 0 && bl.payload_size > 0 && bl.payload_at + bl.payload_size <= size, what);
		free(data);
	}

	if (profile->falcon_bootloader_file != NULL) {
		data = read_profile_file(dir, profile->falcon_bootloader_file, &size);
		if (data != NULL) {
			snprintf(what, sizeof(what), "files %s: %s parses", profile->name, profile->falcon_bootloader_file);
			check(nvrtx_falcon_bl_parse(data, size, &fbl) == 0 && fbl.code_size > 0, what);
			free(data);
		}
	}
}

/* The real files: the exact containers of the first target (TU106), then every family's set. */
static void
test_files(const char *dir)
{
	struct nvrtx_booter booter;
	struct nvrtx_bootloader bl;
	struct nvrtx_falcon_bl fbl;
	struct nvrtx_section section;
	uint8_t *data;
	size_t size;
	size_t at;
	int error;

	data = read_file(dir, "booter_load-570.144.bin", &size);
	check(data != NULL, "files: booter_load present");
	if (data != NULL) {
		error = nvrtx_booter_parse(data, size, &booter);
		check(error == 0, "files: booter_load parses");
		check(booter.payload_at == 0x88 && booter.payload_size == 0xe700, "files: booter_load payload");
		check(booter.os_code_at == 0 && booter.os_code_size == 0x100, "files: booter_load code");
		check(booter.os_data_at == 0x8500 && booter.os_data_size == 0x6200, "files: booter_load data");
		check(booter.app_at == 0x100 && booter.app_size == 0x8400, "files: booter_load application");
		check(booter.signature_count >= 1 && booter.signature_size > 0, "files: booter_load signatures");
		check(nvrtx_booter_signature(&booter, 0, &at) == 0 && at + booter.signature_size <= size, "files: production signature in the file");
		printf("info booter_load: %u signatures of %zu bytes, patch at payload +0x%x\n",
		    booter.signature_count, booter.signature_size, booter.patch_location);
		check(nvrtx_booter_parse(data, 100, &booter) == EINVAL, "files: truncated booter refused");
		free(data);
	} else {
		skipped++;
	}

	data = read_file(dir, "bootloader-570.144.bin", &size);
	check(data != NULL && nvrtx_bootloader_parse(data, size, &bl) == 0, "files: bootloader parses");
	check(data != NULL && bl.payload_at == 0x64 && bl.payload_size == 0x1000, "files: bootloader payload");
	free(data);

	data = read_file(dir, "bl.bin", &size);
	check(data != NULL && nvrtx_falcon_bl_parse(data, size, &fbl) == 0, "files: falcon bootloader parses");
	check(data != NULL && fbl.start_tag == 0xfd && fbl.code_size == 0x200 && fbl.code == data + 0x200, "files: falcon bootloader start tag and code");
	free(data);

	data = read_file(dir, "gsp-570.144.bin", &size);
	check(data != NULL, "files: gsp present");
	if (data != NULL) {
		error = nvrtx_elf_section(data, size, NVRTX_GSP_IMAGE_SECTION, &section);
		check(error == 0 && section.size == 0x1b34ea0, "files: .fwimage");
		error = nvrtx_elf_section(data, size, ".fwsignature_tu10x", &section);
		check(error == 0 && section.size == 0x1000, "files: .fwsignature_tu10x");
		free(data);
	} else {
		skipped++;
	}

	/* Every family: TU106, TU116, GA104, AD104. */
	check_profile_files(dir, 0x166000a1U);
	check_profile_files(dir, 0x168000a1U);
	check_profile_files(dir, 0x174000a1U);
	check_profile_files(dir, 0x194000a1U);
}

static void
put_image(uint8_t *rom, size_t base, unsigned units, unsigned type, int last)
{
	rom[base] = 0x55;
	rom[base + 1] = 0xaa;
	rom[base + 0x18] = 0x40;
	memcpy(rom + base + 0x40, "PCIR", 4);
	rom[base + 0x40 + 0x0a] = 0x18;
	rom[base + 0x40 + 0x10] = (uint8_t)units;
	rom[base + 0x40 + 0x14] = (uint8_t)type;
	memcpy(rom + base + 0x60, "NPDE", 4);
	rom[base + 0x60 + 8] = (uint8_t)units;
	rom[base + 0x60 + 0x0a] = last ? 0x80 : 0;
}

static void
test_vbios(void)
{
	static uint8_t rom[0x1600];
	struct nvrtx_vbios vbios;
	struct nvrtx_fwsec fwsec;
	uint8_t program[0x200];
	unsigned sum;
	unsigned i;
	int error;

	memset(rom, 0, sizeof(rom));
	put_image(rom, 0, 2, 0x00, 0);
	memcpy(rom + 0x100, "\xff\xb8" "BIT", 5);
	rom[0x109] = 6;
	rom[0x10a] = 2;
	rom[0x10c] = 'x';
	rom[0x112] = 'p';
	rom[0x113] = 2;
	rom[0x114] = 4;
	rom[0x116] = 0x00;
	rom[0x117] = 0x02;
	nvrtx_put32(rom, 0x200, 0x500);
	sum = 0;
	for (i = 0; i < 0x400; i++)
		sum += rom[i];
	rom[0x3ff] = (uint8_t)(0x100 - (sum & 0xff));
	put_image(rom, 0x400, 1, 0x03, 0);
	put_image(rom, 0x600, 8, 0xe0, 1);
	rom[0x700] = 1;
	rom[0x701] = 4;
	rom[0x702] = 6;
	rom[0x703] = 2;
	rom[0x704] = 0x10;
	rom[0x70a] = 0x85;
	nvrtx_put32(rom, 0x70c, 0x600);
	nvrtx_put32(rom, 0x800, 1U | (2U << 8) | (0x40U << 16));
	nvrtx_put32(rom, 0x800 + 16, 0x10);
	nvrtx_put32(rom, 0x800 + 20, 0x0);
	nvrtx_put32(rom, 0x800 + 24, 0x100);
	nvrtx_put32(rom, 0x800 + 32, 0x80);
	nvrtx_put32(rom, 0x800 + 36, 0x80);
	nvrtx_put32(rom, 0x800 + 40, 0x100);
	nvrtx_put32(rom, 0x800 + 44, 0x0);
	nvrtx_put32(rom, 0x800 + 48, 0x100);
	rom[0x940 + 0x10] = 1;
	rom[0x940 + 0x11] = 4;
	rom[0x940 + 0x12] = 8;
	rom[0x940 + 0x13] = 2;
	nvrtx_put32(rom, 0x940 + 0x14, 3);
	nvrtx_put32(rom, 0x940 + 0x1c, 4);
	nvrtx_put32(rom, 0x940 + 0x20, 0x40);
	nvrtx_put32(rom, 0x940 + 0x40 + 8, 0x80);

	error = nvrtx_vbios_walk(rom, sizeof(rom), &vbios);
	check(error == 0, "vbios: chain walks");
	check(vbios.image0_size == 0x400 && vbios.falcon_image == 0x600 && vbios.end == 0x1600, "vbios: first image, falcon image, end");
	error = nvrtx_vbios_fwsec(&vbios, &fwsec);
	check(error == 0, "vbios: FWSEC found through BIT and the falcon table");
	check(fwsec.version == 2 && fwsec.program_at == 0x840 && fwsec.program_size == 0x200, "vbios: FWSEC program place");
	check(fwsec.imem_size == 0x100 && fwsec.imem_secure_size == 0x80 && fwsec.dmem_at == 0x100 && fwsec.dmem_size == 0x100, "vbios: FWSEC descriptor");

	memcpy(program, rom + fwsec.program_at, sizeof(program));
	error = nvrtx_fwsec_command(program, &fwsec, NVRTX_FWSEC_FRTS, 0x1ffe00000ULL, 0x100000);
	check(error == 0, "fwsec: FRTS command written");
	check(nvrtx_get32(program, 0x100 + 0x40 + 44) == 0x15, "fwsec: command word");
	check(nvrtx_get32(program, 0x180) == 1 && nvrtx_get32(program, 0x184) == 24 && nvrtx_get32(program, 0x194) == 2, "fwsec: read request");
	check(nvrtx_get32(program, 0x180 + 24) == 1 && nvrtx_get32(program, 0x180 + 28) == 20, "fwsec: region header");
	check(nvrtx_get32(program, 0x180 + 32) == 0x1ffe00 && nvrtx_get32(program, 0x180 + 36) == 0x100 && nvrtx_get32(program, 0x180 + 40) == 2, "fwsec: region in 4 KiB units of the VRAM");

	rom[0x3ff] ^= 1;
	check(nvrtx_vbios_walk(rom, sizeof(rom), &vbios) == EINVAL, "vbios: bad checksum refused");
	rom[0x3ff] ^= 1;
	nvrtx_put32(rom, 0x800, 1U | (4U << 8) | (0x40U << 16));
	nvrtx_vbios_walk(rom, sizeof(rom), &vbios);
	check(nvrtx_vbios_fwsec(&vbios, &fwsec) == ENOTSUP, "vbios: descriptor version 4 refused");

	/*
	 * Version 3 (Ampere and later): the 44-byte fixed part, one signature,
	 * then the code (one 256-byte block) and the data (two blocks).
	 */
	memset(rom + 0x800, 0, sizeof(rom) - 0x800);
	nvrtx_put32(rom, 0x800, 1U | (3U << 8) | ((44U + NVRTX_FWSEC_SIGNATURE_BYTES) << 16));
	nvrtx_put32(rom, 0x800 + 8, 0x10);
	nvrtx_put32(rom, 0x800 + 12, 0x40);
	nvrtx_put32(rom, 0x800 + 16, 0x0);
	nvrtx_put32(rom, 0x800 + 20, 0x100);
	nvrtx_put32(rom, 0x800 + 28, 0x0);
	nvrtx_put32(rom, 0x800 + 32, 0x200);
	rom[0x800 + 36] = 0x00;
	rom[0x800 + 37] = 0x04;
	rom[0x800 + 38] = 3;
	rom[0x800 + 39] = 1;
	rom[0x800 + 40] = 0x04;
	rom[0x800 + 41] = 0x00;
	for (i = 0; i < NVRTX_FWSEC_SIGNATURE_BYTES; i++)
		rom[0x800 + 44 + i] = (uint8_t)(0x5a ^ i);
	error = nvrtx_vbios_walk(rom, sizeof(rom), &vbios);
	check(error == 0, "vbios v3: chain walks");
	error = nvrtx_vbios_fwsec(&vbios, &fwsec);
	check(error == 0 && fwsec.version == 3, "vbios v3: FWSEC found");
	check(fwsec.signatures_at == 0x800 + 44 && fwsec.signature_count == 1 && fwsec.signature_versions == 0x4, "vbios v3: signatures");
	check(fwsec.program_at == 0x800 + 44 + NVRTX_FWSEC_SIGNATURE_BYTES && fwsec.program_size == 0x300, "vbios v3: program follows the descriptor");
	check(fwsec.imem_size == 0x100 && fwsec.imem_secure_size == 0x100 && fwsec.dmem_at == 0x100 && fwsec.dmem_load_size == 0x200, "vbios v3: code and data");
	check(fwsec.engine_id == 0x400 && fwsec.ucode_id == 3, "vbios v3: engine and ucode");
	check(nvrtx_fwsec_fuse_register(&fwsec) == 0x8241c8U, "vbios v3: the GSP fuse bank's word of ucode 3");
	{
		static uint8_t program3[0x300];
		unsigned index;

		check(nvrtx_fwsec_fuse_index(&fwsec, 0x3U, &index) == 0 && index == 0, "vbios v3: fuse revoked to 2 takes the version-2 signature");
		check(nvrtx_fwsec_fuse_index(&fwsec, 0x1U, &index) == EINVAL, "vbios v3: no signature for version 1");
		memset(program3, 0, sizeof(program3));
		check(nvrtx_fwsec_sign(program3, &fwsec, &vbios, 0) == 0, "vbios v3: signed");
		check(program3[0x100 + 0x10] == 0x5a && program3[0x100 + 0x10 + 383] == (uint8_t)(0x5a ^ 383), "vbios v3: signature at its place in the data");
		check(nvrtx_fwsec_sign(program3, &fwsec, &vbios, 1) == EINVAL, "vbios v3: no second signature");
	}
	nvrtx_put32(rom, 0x800 + 20, 0x80);
	nvrtx_vbios_walk(rom, sizeof(rom), &vbios);
	check(nvrtx_vbios_fwsec(&vbios, &fwsec) == EINVAL, "vbios v3: code not whole DMA blocks refused");
}

/* The falcons the boot stops at each point it is left at with a program that may still run. */
static void
test_hold(void)
{
	check(nvrtx_gsp_hold_falcons(NVRTX_GSP_HOLD_FWSEC) == NVRTX_GSP_HOLD_GSP_FALCON, "hold: FWSEC-FRTS stops the GSP's falcon (FWSEC runs there)");
	check(nvrtx_gsp_hold_falcons(NVRTX_GSP_HOLD_BOOTER) == (NVRTX_GSP_HOLD_GSP_FALCON | NVRTX_GSP_HOLD_SEC2_FALCON),
	    "hold: booter_load stops the secure processor and the GSP it may have started");
	check(nvrtx_gsp_hold_falcons(NVRTX_GSP_HOLD_INIT) == NVRTX_GSP_HOLD_GSP_FALCON, "hold: gsp-init stops the GSP");
	check(nvrtx_gsp_hold_falcons(NVRTX_GSP_HOLD_UNLOAD) == NVRTX_GSP_HOLD_GSP_FALCON, "hold: unload stops the GSP");
	check(nvrtx_gsp_hold_falcons(NVRTX_GSP_HOLD_TEAR_DOWN) == (NVRTX_GSP_HOLD_GSP_FALCON | NVRTX_GSP_HOLD_SEC2_FALCON),
	    "hold: the teardown stops both (FWSEC-SB on the GSP's falcon, booter_unload on the secure processor's)");
	check(nvrtx_gsp_hold_falcons((enum nvrtx_gsp_hold)99) == (NVRTX_GSP_HOLD_GSP_FALCON | NVRTX_GSP_HOLD_SEC2_FALCON), "hold: an unknown point stops both");
}

/* The layouts of Ampere and Ada: the larger heap of libos 3, and the VRAM size register in MiB. */
static void
test_layout_ampere(void)
{
	struct nvrtx_wpr_layout l;
	uint64_t vram;
	int error;

	vram = nvrtx_vram_size_mib(12288U);
	check(vram == 12ULL << 30, "layout ampere: 12288 MiB decoded");
	check(nvrtx_vram_size_mib(8192U) == 8ULL << 30, "layout ampere: 8192 MiB decoded");

	/* 12 GiB with a 58 MiB image: the heap takes its 170 MiB least. */
	error = nvrtx_wpr_layout(vram, vram - 0x100000, 0x1000, 0x3a00000ULL, 22ULL << 20, 170ULL << 20, &l);
	check(error == 0, "layout ampere: 12 GiB fits");
	check(l.frts == 0x2ffe00000ULL && l.boot == 0x2ffdff000ULL, "layout ampere: FRTS and bootloader");
	check(l.image == 0x2fc3f0000ULL, "layout ampere: image");
	check(l.heap == 0x2f1900000ULL && l.heap_size == 170ULL << 20, "layout ampere: heap 170 MiB");
	check(l.region == 0x2f1800000ULL && l.outer_heap == 0x2f1700000ULL, "layout ampere: region and outer heap");
	printf("info layout ampere 12 GiB: %llu MiB from the outer heap to the end of the VRAM\n",
	    (unsigned long long)((vram - l.outer_heap) >> 20));

	/* The same VRAM with Turing's heap: the heap is smaller, so the libos 3 numbers are the ones used. */
	error = nvrtx_wpr_layout(vram, vram - 0x100000, 0x1000, 0x3a00000ULL, 0, 64ULL << 20, &l);
	check(error == 0 && l.heap_size < 170ULL << 20, "layout ampere: Turing's heap would be smaller");

	/* The reserved memory of the usual layouts lies in the scrubbed 256 MiB. */
	error = nvrtx_wpr_layout(vram, vram - 0x100000, 0x1000, 0x3a00000ULL, 22ULL << 20, 170ULL << 20, &l);
	check(error == 0 && nvrtx_wpr_scrubbed(&l) == 0, "scrubbed: Ampere 12 GiB (233 MiB reserved) inside");
	error = nvrtx_wpr_layout(24ULL << 30, (24ULL << 30) - 0x100000, 0x6000, 0x3c99000ULL, 22ULL << 20, 170ULL << 20, &l);
	check(error == 0 && nvrtx_wpr_scrubbed(&l) == 0, "scrubbed: Ada 24 GiB with the real image size inside");
	printf("info scrubbed: 24 GiB Ada reserves %llu MiB\n", (unsigned long long)(((24ULL << 30) - l.outer_heap) >> 20));
	error = nvrtx_wpr_layout(8ULL << 30, (8ULL << 30) - 0x100000, 0x1000, 0x1b34ea0, 0, 64ULL << 20, &l);
	check(error == 0 && nvrtx_wpr_scrubbed(&l) == 0, "scrubbed: RTX 2070 (137 MiB reserved) inside");

	/*
	 * The boundary, by the size measured as open-gpu-kernel-modules does
	 * (the frame buffer's size less the start of the non-WPR heap):
	 * 255 and 256 MiB pass, 257 MiB does not.
	 */
	memset(&l, 0, sizeof(l));
	l.vram_size = 12ULL << 30;
	l.outer_heap = l.vram_size - (255ULL << 20);
	check(nvrtx_wpr_scrubbed(&l) == 0, "scrubbed: 255 MiB reserved inside");
	l.outer_heap = l.vram_size - (256ULL << 20);
	check(nvrtx_wpr_scrubbed(&l) == 0, "scrubbed: 256 MiB reserved inside");
	l.outer_heap = l.vram_size - (257ULL << 20);
	check(nvrtx_wpr_scrubbed(&l) == ENOSPC, "scrubbed: 257 MiB reserved refused");
	l.outer_heap = l.vram_size - (256ULL << 20) - 0x1000;
	check(nvrtx_wpr_scrubbed(&l) == ENOSPC, "scrubbed: 256 MiB and a page refused");

	/* A larger heap than the scrubbed end holds is refused by the check, not the layout. */
	error = nvrtx_wpr_layout(vram, vram - 0x100000, 0x1000, 0x3a00000ULL, 22ULL << 20, 200ULL << 20, &l);
	check(error == 0 && nvrtx_wpr_scrubbed(&l) == ENOSPC, "scrubbed: a 200 MiB heap with a 58 MiB image refused");

	/* A VRAM too small for a 170 MiB heap is refused. */
	error = nvrtx_wpr_layout(256ULL << 20, (256ULL << 20) - 0x100000, 0x1000, 0x3a00000ULL, 22ULL << 20, 170ULL << 20, &l);
	check(error == EINVAL, "layout ampere: 256 MiB VRAM refused");
}

static void
test_records(void)
{
	struct nvrtx_sysinfo_values v;
	struct nvrtx_seq_command c;
	uint8_t record[NVRTX_SYSINFO_BYTES];
	uint8_t registry[512];
	uint8_t seq[40 + 13 * 4];
	uint32_t cursor;
	size_t length;
	int error;

	memset(&v, 0, sizeof(v));
	v.registers = 0xf6000000ULL;
	v.vram = 0xe0000000ULL;
	v.instance = 0xf0000000ULL;
	v.bus_device_function = 0x0100;
	v.user_address_limit = 0x00007ffffffff000ULL;
	v.console_size = 0x7f0000ULL;
	v.vendor = 0x10de;
	v.device = 0x1f02;
	v.subvendor = 0x1458;
	v.subdevice = 0x37c2;
	v.revision = 0xa1;
	v.primary = 1;
	nvrtx_sysinfo(record, &v);
	check(nvrtx_get64(record, 0) == 0xf6000000ULL && nvrtx_get64(record, 8) == 0xe0000000ULL && nvrtx_get64(record, 16) == 0xf0000000ULL, "sysinfo: windows");
	check(nvrtx_get64(record, 32) == 0x100 && nvrtx_get64(record, 72) == 0x00007ffffffff000ULL, "sysinfo: BDF and user limit");
	check(nvrtx_get32(record, 80) == 0x88000 && nvrtx_get32(record, 84) == 0x1000, "sysinfo: config mirror");
	check(nvrtx_get64(record, 64) == 0x7f0000ULL && nvrtx_get64(record, 56) == 0U, "sysinfo: the console's size at GspSystemInfo.consoleMemSize");
	check(nvrtx_get32(record, 88) == 0x1f0210de && nvrtx_get32(record, 92) == 0x37c21458 && nvrtx_get32(record, 96) == 0xa1, "sysinfo: PCI identity");
	check(record[896] == 1 && record[908] == 0 && nvrtx_get64(record, 920) == 0, "sysinfo: primary, no preserved memory");

	error = nvrtx_registry(registry, sizeof(registry), &length);
	check(error == 0 && length == 8 + 3 * 16 + 20 + 22 + 19, "registry: length");
	check(nvrtx_get32(registry, 0) == length && nvrtx_get32(registry, 4) == 3, "registry: header");
	check(nvrtx_get32(registry, 8) == 56 && registry[12] == 1 && nvrtx_get32(registry, 16) == 1 && nvrtx_get32(registry, 20) == 4, "registry: first entry");
	check(memcmp(registry + 56, "RMSecBusResetEnable", 20) == 0, "registry: first name");
	check(nvrtx_get32(registry, 40) == 56 + 20 + 22 && memcmp(registry + 98, "RMDevidCheckIgnore", 19) == 0, "registry: third entry");
	check(nvrtx_registry(registry, 60, &length) == E2BIG, "registry: small buffer refused");

	memset(seq, 0, sizeof(seq));
	nvrtx_put32(seq, 4, 11);
	nvrtx_put32(seq, 40, NVRTX_SEQ_WRITE);
	nvrtx_put32(seq, 44, 0x1234);
	nvrtx_put32(seq, 48, 0x5678);
	nvrtx_put32(seq, 52, NVRTX_SEQ_STORE);
	nvrtx_put32(seq, 56, 0x9abc);
	nvrtx_put32(seq, 60, 3);
	nvrtx_put32(seq, 64, NVRTX_SEQ_CORE_RESET);
	nvrtx_put32(seq, 68, NVRTX_SEQ_DELAY);
	nvrtx_put32(seq, 72, 50);
	nvrtx_put32(seq, 76, NVRTX_SEQ_CORE_START);
	nvrtx_put32(seq, 80, NVRTX_SEQ_CORE_WAIT_HALT);
	cursor = 0;
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == 0 && c.opcode == 0 && c.argument[0] == 0x1234 && c.argument[1] == 0x5678 && cursor == 3, "sequencer: write");
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == 0 && c.opcode == 4 && c.argument[1] == 3 && cursor == 6, "sequencer: store");
	check(nvrtx_sequencer_save(seq, sizeof(seq), 3, 0xfeed) == 0 && nvrtx_get32(seq, 8 + 12) == 0xfeed, "sequencer: save slot");
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == 0 && c.opcode == 5 && cursor == 7, "sequencer: core reset");
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == 0 && c.opcode == 3 && c.argument[0] == 50 && cursor == 9, "sequencer: delay");
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == 0 && cursor == 10, "sequencer: core start");
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == 0 && cursor == 11, "sequencer: wait halt");
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == ENOENT, "sequencer: end");
	{
		struct nvrtx_object o = { 0xc0de0001U, 0xc0de0001U, 0xc0de0080U, 0x80U };
		uint8_t params[56];
		uint8_t call[200];
		size_t n;

		memset(params, 0x5a, sizeof(params));
		n = nvrtx_alloc_call(call, sizeof(call), &o, params, sizeof(params));
		check(n == 88 && nvrtx_get32(call, 0) == 0xc0de0001U && nvrtx_get32(call, 8) == 0xc0de0080U && nvrtx_get32(call, 12) == 0x80, "alloc: header handles and class");
		check(nvrtx_get32(call, 16) == 0 && nvrtx_get32(call, 20) == 56 && nvrtx_get32(call, 24) == 0 && nvrtx_get32(call, 28) == 0, "alloc: status, length, flags, reserved");
		check(call[31] == 0 && call[32] == 0x5a && call[87] == 0x5a, "alloc: parameters right after the 32-byte header (R5)");
		check(nvrtx_alloc_call(call, 60, &o, params, sizeof(params)) == 0, "alloc: small buffer refused");
	}
	nvrtx_put32(seq, 40, 99);
	cursor = 0;
	check(nvrtx_sequencer_next(seq, sizeof(seq), &cursor, &c) == EINVAL, "sequencer: unknown opcode");
	check(nvrtx_sequencer_save(seq, sizeof(seq), 8, 0) == EINVAL, "sequencer: save slot range");
}

static void
test_mmu(void)
{
	static uint8_t image[NVRTX_MMU_TABLES_BYTES];
	static uint8_t inst[NVRTX_INSTANCE_BYTES];
	static uint8_t info[NVRTX_STATIC_INFO_BYTES];
	uint64_t pages[3] = { 0x10000000ULL, 0x10001000ULL, 0x2345f000ULL };
	uint64_t block;
	uint64_t va;
	uint32_t region;
	size_t r;

	block = 0x1f6f00000ULL;
	va = 0x20000000ULL;
	check(nvrtx_mmu_tables(image, block, va, pages, 3) == 0, "mmu: tables built");
	check(nvrtx_get64(image, 0) == (((block + 0x1000) >> 4) | 2) && nvrtx_get64(image, 8) == 0, "mmu: root entry 0 to dir2 in VRAM");
	check(nvrtx_get64(image, 0x1000) == (((block + 0x2000) >> 4) | 2), "mmu: dir2 entry 0");
	check(nvrtx_get64(image, 0x2000 + 8) == (((block + 0x3000) >> 4) | 2) && nvrtx_get64(image, 0x2000) == 0, "mmu: dir1 entry 1 (VA 512 MiB)");
	check(nvrtx_get64(image, 0x3000) == 0 && nvrtx_get64(image, 0x3008) == (((block + 0x4000) >> 4) | 2), "mmu: dual entry, small half");
	check(nvrtx_get64(image, 0x4000) == ((0x10000000ULL >> 4) | 0xd) && nvrtx_get64(image, 0x4010) == ((0x2345f000ULL >> 4) | 0xd), "mmu: page entries valid, system coherent, volatile");
	check(nvrtx_get64(image, 0x4018) == 0, "mmu: unused page entry invalid");
	check(nvrtx_mmu_tables(image, block, va + 0x1000, pages, 3) == EINVAL, "mmu: unaligned range refused");

	memset(inst, 0, sizeof(inst));
	nvrtx_mmu_instance(inst, block, 1ULL << 47);
	check(nvrtx_get64(inst, 0x200) == (block | 0xc00) && nvrtx_get64(inst, 0x208) == (1ULL << 47) - 1, "inst: root and limit");
	check(nvrtx_get32(inst, 0x2a0) == (uint32_t)(block | 0xc00) && nvrtx_get32(inst, 0x2a4) == (uint32_t)((block | 0xc00) >> 32), "inst: subcontext 0");
	check(nvrtx_get32(inst, 0x2b0) == 1 && nvrtx_get32(inst, 0x2b4) == 1 && nvrtx_get32(inst, 0x298) == 1, "inst: other subcontexts invalid, mask");

	memset(info, 0, sizeof(info));
	nvrtx_put32(info, 344, 3);
	r = 344 + 8;
	nvrtx_put64(info, r, 0);
	nvrtx_put64(info, r + 8, 0x0fffffffULL);
	info[r + 28] = 1;
	info[r + 29] = 1;
	r += 48;
	nvrtx_put64(info, r, 0x10000000ULL);
	nvrtx_put64(info, r + 8, 0x1f76fffffULL);
	info[r + 28] = 1;
	info[r + 29] = 1;
	r += 48;
	nvrtx_put64(info, r, 0x1f7700000ULL);
	nvrtx_put64(info, r + 8, 0x1ffffffffULL);
	nvrtx_put64(info, r + 16, 1);
	check(nvrtx_mmu_vram_block(info, 0x10000, &block, &region) == 0 && region == 1 && block == 0x1f7600000ULL, "vram: top of the last usable region, 1 MiB aligned");
	info[344 + 8 + 48 + 30] = 1;
	check(nvrtx_mmu_vram_block(info, 0x10000, &block, &region) == 0 && region == 0 && block == 0x0ff00000ULL, "vram: protected region passed over");
	nvrtx_put32(info, 344, 0);
	check(nvrtx_mmu_vram_block(info, 0x10000, &block, &region) == ENOSPC, "vram: no region");
}

/* D1: the work channel's page entries, fences and first push buffer. */
static void
test_work(void)
{
	uint8_t entries[3 * 8];
	uint64_t pages[2] = { 0x12340000ULL, 0x56781000ULL };
	uint8_t push[64];
	size_t length;

	check(nvrtx_mmu_gr_pte_offset(NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, NVRTX_WORK_VA_BASE) == 0x4000, "work: first page entry after the directories");
	check(nvrtx_mmu_gr_pte_offset(NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, NVRTX_WORK_VA_USER) == 0x4000 + (0x08100000 / 0x1000) * 8, "work: user range entry in page order across tables");
	check(nvrtx_mmu_gr_pte_offset(NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, NVRTX_WORK_VA_BASE + NVRTX_WORK_VA_BYTES - 0x1000) == 0x4000 + (0xdffff * 8), "work: last page entry (3.5 GiB of pages)");
	check(nvrtx_mmu_gr_pte_offset(NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, NVRTX_WORK_VA_BASE + NVRTX_WORK_VA_BYTES) == 0, "work: past the range refused");
	check(nvrtx_mmu_gr_pte_offset(NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, NVRTX_WORK_VA_USER + 8) == 0, "work: unaligned address refused");
	check(nvrtx_mmu_gr_pte_offset(NVRTX_WORK_VA_BASE, NVRTX_WORK_VA_BYTES, NVRTX_WORK_VA_BASE - 0x1000) == 0, "work: below the range refused");
	check(nvrtx_mmu_gr_pte_offset(0x20000000ULL, 0x20000000ULL + 0x200000ULL, 0x20000000ULL) == 0, "work: a range past one directory entry refused");
	check(nvrtx_mmu_gr_tables_bytes(NVRTX_WORK_VA_BYTES) == 0x4000 + (1792 + 6) * 0x1000, "work: 1792 page tables (3.5 GiB of 2 MiB) and the six further 512 MiB directories");

	nvrtx_mmu_gr_entries_vram(entries, 0x1f0000000ULL, 3);
	check(nvrtx_get64(entries, 0) == ((0x1f0000000ULL >> 4) | 1) && nvrtx_get64(entries, 16) == ((0x1f0002000ULL >> 4) | 1), "work: VRAM entries valid, not privileged, consecutive");
	nvrtx_mmu_gr_entries_system(entries, pages, 2);
	check(nvrtx_get64(entries, 0) == ((0x12340000ULL >> 4) | 0xd) && nvrtx_get64(entries, 8) == ((0x56781000ULL >> 4) | 0xd), "work: system entries valid, coherent, volatile");

	check(nvrtx_work_fence_passed(5, 5) && nvrtx_work_fence_passed(6, 5), "work: fence at or behind the semaphore passed");
	check(!nvrtx_work_fence_passed(4, 5), "work: fence ahead not passed");
	check(nvrtx_work_fence_passed(2, 0xfffffffeU) && !nvrtx_work_fence_passed(0xfffffffeU, 2), "work: fence wraps");

	memset(push, 0, sizeof(push));
	length = nvrtx_work_push_objects(push, 0xc597, 0xc5c0);
	check(length == 16, "work: objects push 4 words");
	check(nvrtx_get32(push, 0) == 0x20010000 && nvrtx_get32(push, 4) == 0xc597, "work: SET_OBJECT 3D on subchannel 0");
	check(nvrtx_get32(push, 8) == 0x20012000 && nvrtx_get32(push, 12) == 0xc5c0, "work: SET_OBJECT compute on subchannel 1");
	check(nvrtx_push_fence(push, 0x28001000ULL, 7) <= NVRTX_WORK_FENCE_BYTES, "work: a fence fits its slot");
	check(NVRTX_WORK_RING_ENTRIES * 8 == 0x1000 && (NVRTX_WORK_RING_ENTRIES & (NVRTX_WORK_RING_ENTRIES - 1)) == 0, "work: ring is one page, a power of two");
}

/* Compares a push buffer's words with the expected ones, printing the first difference. */
static int
push_is(const struct nvrtx_push *push, const uint32_t *want, uint32_t count)
{
	uint32_t i;

	if (push->overflow || push->count != count) {
		printf("  push has %u words (overflow %d), want %u\n", (unsigned)push->count, push->overflow, (unsigned)count);
		return 0;
	}
	for (i = 0; i < count; i++) {
		if (nvrtx_get32(push->bytes, i * 4U) != want[i]) {
			printf("  push word %u is %08x, want %08x\n", (unsigned)i, (unsigned)nvrtx_get32(push->bytes, i * 4U), (unsigned)want[i]);
			return 0;
		}
	}
	return 1;
}

/* P8b as nouveau: the first commit's words compared with nouveau's (dumped from its class headers). */
static void
test_disp_first(void)
{
	static uint8_t bytes[4096];
	static uint8_t lut[NVRTX_DISP_LUT_BYTES];
	static uint8_t edid[256];
	static const uint32_t start[42] = {
		0x00040208, 0xf0000000,
		0x00081004, 0x0000000f, 0x00000000, 0x00041010, 0x00117fff,
		0x00081084, 0x0000000f, 0x00000000, 0x00041090, 0x00117fff,
		0x00081104, 0x0000000f, 0x00000000, 0x00041110, 0x00117fff,
		0x00081184, 0x0000000f, 0x00000000, 0x00041190, 0x00117fff,
		0x00081204, 0x0000000f, 0x00000000, 0x00041210, 0x00117fff,
		0x00081284, 0x0000000f, 0x00000000, 0x00041290, 0x00117fff,
		0x00081304, 0x0000000f, 0x00000000, 0x00041310, 0x00117fff,
		0x00081384, 0x0000000f, 0x00000000, 0x00041390, 0x00117fff,
	};
	static const uint32_t first[54] = {
		0x00042020, 0x00000000, 0x00040300, 0x00000000, 0x00042020, 0x00000001, 0x00040300, 0x00000101,
		0x0004204c, 0x04000500, 0x00042058, 0x04000500,
		0x00102064, 0x042a0698, 0x0002006f, 0x00280167, 0x04280667,
		0x00042074, 0x00000001, 0x00042008, 0x00000000, 0x0004200c, 0x066ff300, 0x00042028, 0x066ff300,
		0x00042030, 0x00001114, 0x00042004, 0xfc000040,
		0x00041000, 0x00000000, 0x00041080, 0x00000000, 0x00041100, 0x00000001, 0x00041180, 0x00000001,
		0x00041200, 0x00000002, 0x00041280, 0x00000002, 0x00041300, 0x00000003, 0x00041380, 0x00000003,
		0x0004020c, 0x00001000, 0x00080218, 0x00000000, 0x00000000, 0x00040200, 0x00000001, 0x0004020c, 0x00000000,
	};
	static const uint32_t second[14] = {
		0x00102280, 0x00040509, 0xffffffff, 0xf0000001, 0x00123456,
		0x0004020c, 0x00001000, 0x00080218, 0x00000000, 0x00000001, 0x00040200, 0x00000001, 0x0004020c, 0x00000000,
	};
	static const uint32_t window[37] = {
		0x0008021c, 0xf0000000, 0x00000060,
		0x00040308, 0x00000001,
		0x00100224, 0x04000500, 0x00000010, 0x000000cf, 0x00000050,
		0x00040240, 0xfb000000, 0x00040260, 0x00045678, 0x00040290, 0x00000000, 0x00040298, 0x04000500, 0x000402a4, 0x04000500,
		0x000c0440, 0x00040508, 0xf0000001, 0x0012abcd,
		0x001c02ec, 0x00000ff0, 0x000000ff, 0x00007722, 0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000,
		0x00080370, 0x00000001, 0x00000001,
		0x00040200, 0x00000001,
	};
	static const uint32_t flip[25] = {
		0x0008021c, 0xf0000000, 0x00000070,
		0x00040308, 0x00000001,
		0x00100224, 0x04000500, 0x00000010, 0x000000cf, 0x00000050,
		0x00040240, 0xfb000000, 0x00040260, 0x00045678, 0x00040290, 0x00000000, 0x00040298, 0x04000500, 0x000402a4, 0x04000500,
		0x00080370, 0x00000000, 0x00000001,
		0x00040200, 0x00000001,
	};
	static const uint32_t view[13] = {
		0x0004244c, 0x04000500, 0x00042458, 0x04000500,
		0x0004020c, 0x00001000, 0x00080218, 0x00000000, 0x00000004, 0x00040200, 0x00000001, 0x0004020c, 0x00000000,
	};
	uint32_t scaled[54];
	uint32_t flip_core[25];
	struct nvrtx_push push;
	struct nvrtx_disp_timing timing;
	struct nvrtx_disp_timing back;
	struct nvrtx_disp_first commit;
	struct nvrtx_disp_surface surface;
	struct nvrtx_disp_sink sink;
	uint32_t armed[NVRTX_DISP_HEAD_COPIED];
	uint32_t avi[5];

	/* The core's start: 42 words, no release. */
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_core_start(&push);
	check(push_is(&push, start, 42), "first: core start is nouveau's 42 words");

	/* The 1280x1024@60 DMT timing gives nouveau's raster words. */
	memset(&timing, 0, sizeof(timing));
	timing.hdisplay = 1280; timing.hsync_start = 1328; timing.hsync_end = 1440; timing.htotal = 1688;
	timing.vdisplay = 1024; timing.vsync_start = 1025; timing.vsync_end = 1028; timing.vtotal = 1066;
	timing.clock_khz = 108000;
	memset(&commit, 0, sizeof(commit));
	nvrtx_disp_first_mode(&timing, &commit.mode);
	check(commit.mode.raster_size == 0x042a0698 && commit.mode.sync_end == 0x0002006f && commit.mode.blank_end == 0x00280167 && commit.mode.blank_start == 0x04280667, "first: raster words from the timing");
	check(commit.mode.output_resource == 0xfc000040 && commit.mode.clock_hz == 108000000, "first: output resource and clock");

	/* The armed words give the timing back. */
	memset(armed, 0, sizeof(armed));
	armed[NVRTX_DISP_ARMED_RASTER_SIZE] = commit.mode.raster_size;
	armed[NVRTX_DISP_ARMED_SYNC_END] = commit.mode.sync_end;
	armed[NVRTX_DISP_ARMED_BLANK_END] = commit.mode.blank_end;
	armed[NVRTX_DISP_ARMED_BLANK_START] = commit.mode.blank_start;
	armed[NVRTX_DISP_ARMED_PIXEL_CLOCK] = commit.mode.clock_hz;
	armed[NVRTX_DISP_ARMED_OUTPUT_RESOURCE] = 0xfc00004c;
	nvrtx_disp_first_timing_from_armed(armed, &back);
	check(back.hdisplay == 1280 && back.hsync_start == 1328 && back.hsync_end == 1440 && back.htotal == 1688, "first: armed horizontal timing");
	check(back.vdisplay == 1024 && back.vsync_start == 1025 && back.vsync_end == 1028 && back.vtotal == 1066 && back.clock_khz == 108000, "first: armed vertical timing and clock");
	check(back.nhsync == 1 && back.nvsync == 1, "first: armed polarities");

	/* The first core release: S 0, display 1, TMDS-A, head 0, then the release. */
	commit.head = 0; commit.sor = 0; commit.display_id = 1; commit.protocol = 1;
	commit.view_in = commit.mode.active;
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_core_mode(&push, &commit);
	nvrtx_disp_first_core_release(&push, 0);
	check(push_is(&push, first, 54), "first: core release #1 is nouveau's 54 words");

	/* A 640x480 console on the 1280x1024 mode: the view in is the console's, the view out the mode's (the firmware's 01e00280 / 04000500). */
	memcpy(scaled, first, sizeof(scaled));
	scaled[9] = 0x01e00280;
	commit.view_in = 0x01e00280;
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_core_mode(&push, &commit);
	nvrtx_disp_first_core_release(&push, 0);
	check(push_is(&push, scaled, 54), "first: a smaller console is enlarged by the head's view");

	/* A change of the view on head 1 alone, and its release interlocked with window 2. */
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_core_view(&push, 1, 0x04000500, 0x04000500);
	nvrtx_disp_first_core_release(&push, 4);
	check(push_is(&push, view, 13), "first: a view change is nouveau's view and a core release (13 words)");

	/* The output LUT and the second release, window 0 interlocked. */
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_core_olut(&push, 0, 0x12345600ULL);
	nvrtx_disp_first_core_release(&push, 1);
	check(push_is(&push, second, 14), "first: OLUT and core release #2 are nouveau's 14 words");

	/* The window's image, LUT, blend and release. */
	memset(&surface, 0, sizeof(surface));
	surface.offset = 0x4567800ULL; surface.width = 1280; surface.height = 1024; surface.pitch = 5120;
	surface.handle = NVRTX_DISP_HANDLE_FB; surface.format = NVRTX_DISP_FORMAT_A8R8G8B8;
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_window(&push, 0, &surface, 0x10, 0x60, 0x12abcd00ULL);
	check(push_is(&push, window, 37), "first: window 0 is nouveau's 37 words");
	{
		uint32_t pitch_window[37];

		/* Ampere's window has no layout field: the storage word alone changes. */
		memcpy(pitch_window, window, sizeof(pitch_window));
		pitch_window[7] = 0x00000000;
		nvrtx_push_init(&push, bytes, 1024);
		nvrtx_disp_first_window(&push, 0, &surface, 0x00, 0x60, 0x12abcd00ULL);
		check(window[7] == 0x10 && push_is(&push, pitch_window, 37), "first: Ampere's window storage word is 0");
	}

	/* A later flip: notifier (the other half), the image, no core interlock. */
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_window_flip(&push, 0, &surface, 0x10, 0x70, 0);
	check(push_is(&push, flip, 25), "first: a flip is nouveau's commit of the plane alone (25 words)");

	/* A flip with a change of the view: the same words, interlocked with the core too. */
	memcpy(flip_core, flip, sizeof(flip_core));
	flip_core[21] = 0x00000001;
	nvrtx_push_init(&push, bytes, 1024);
	nvrtx_disp_first_window_flip(&push, 0, &surface, 0x10, 0x70, 1);
	check(push_is(&push, flip_core, 25), "first: a flip with the head's view is interlocked with the core");

	/* The LUTs: identity, the last repeated. */
	nvrtx_disp_first_olut_fill(lut);
	check(nvrtx_get32(lut, 0) == 0 && nvrtx_get32(lut, 0x1c) == 0, "first: OLUT header clear");
	check(lut[0x20 + 8] == 0x40 && lut[0x20 + 8 + 1] == 0 && lut[0x20 + 8 + 2] == 0x40 && lut[0x20 + 8 + 4] == 0x40, "first: OLUT entry 1 is 64");
	check(lut[0x20 + 1023 * 8] == 0xc0 && lut[0x20 + 1023 * 8 + 1] == 0xff && lut[0x20 + 1024 * 8] == 0xc0 && lut[0x20 + 1024 * 8 + 1] == 0xff, "first: OLUT entry 1023 is 0xffc0, repeated");
	nvrtx_disp_first_ilut_fill(lut);
	check(lut[0x20] == 0 && lut[0x21] == 0, "first: ILUT entry 0 is 0");
	check(lut[0x20 + 8] == 0x00 && lut[0x20 + 9] == 0x14, "first: ILUT entry 1 is half 2^-10 (0x1400)");
	check(lut[0x20 + 512 * 8] == 0x00 && lut[0x20 + 512 * 8 + 1] == 0x38, "first: ILUT entry 512 is half 0.5 (0x3800)");
	check(lut[0x20 + 1023 * 8] == 0xfe && lut[0x20 + 1023 * 8 + 1] == 0x3b && lut[0x20 + 1024 * 8] == 0xfe && lut[0x20 + 1024 * 8 + 1] == 0x3b, "first: ILUT entry 1023 is 0x3bfe, repeated");

	/* The AVI infoframe of an RGB full-range non-CEA mode. */
	nvrtx_disp_first_avi(avi);
	check(avi[0] == 0x000d0282 && avi[1] == 0x0808104f && avi[2] == 0 && avi[3] == 0 && avi[4] == 0, "first: AVI words");

	/* The EDID's preferred timing: a 1280x1024@60 descriptor, +H +V. */
	memset(edid, 0, sizeof(edid));
	edid[54] = 0x30; edid[55] = 0x2a; edid[56] = 0x00; edid[57] = 0x98; edid[58] = 0x51; edid[59] = 0x00; edid[60] = 0x2a; edid[61] = 0x40;
	edid[62] = 0x30; edid[63] = 0x70; edid[64] = 0x13; edid[65] = 0x00; edid[71] = 0x1e;
	check(nvrtx_disp_first_edid_timing(edid, 128, &back) == 0, "first: EDID timing read");
	check(back.hdisplay == 1280 && back.hsync_start == 1328 && back.hsync_end == 1440 && back.htotal == 1688, "first: EDID horizontal");
	check(back.vdisplay == 1024 && back.vsync_start == 1025 && back.vsync_end == 1028 && back.vtotal == 1066 && back.clock_khz == 108000 && back.nhsync == 0 && back.nvsync == 0, "first: EDID vertical, clock, polarities");

	/* The sink: a CEA extension with the HDMI block and basic audio. */
	edid[126] = 1;
	edid[128] = 0x02; edid[129] = 0x03; edid[130] = 0x0c; edid[131] = 0x40;
	edid[132] = 0x67; edid[133] = 0x03; edid[134] = 0x0c; edid[135] = 0x00; edid[136] = 0x10; edid[137] = 0x00; edid[138] = 0x00; edid[139] = 0x00;
	nvrtx_disp_first_edid_sink(edid, 256, &sink);
	check(sink.hdmi == 1 && sink.audio == 1 && sink.caps == 0, "first: HDMI sink with audio, no forum block");
	edid[126] = 0;
	nvrtx_disp_first_edid_sink(edid, 256, &sink);
	check(sink.hdmi == 0 && sink.audio == 0, "first: no extension, a DVI sink");
}

/* P4m: the range allocator, the usable region's bounds and the VRAM page entry. */
static void
test_heap(void)
{
	static uint8_t info[NVRTX_STATIC_INFO_BYTES];
	static uint8_t bar1_tables[0x4000 + 128 * 0x1000];
	static struct nvrtx_heap heap;
	uint64_t a;
	uint64_t b;
	uint64_t c;
	uint64_t base;
	uint64_t top;
	uint32_t region;
	unsigned i;
	int ok;
	size_t r;

	nvrtx_heap_init(&heap, 0x100000ULL, 0x1100000ULL);
	check(heap.count == 1 && heap.free_bytes == 0x1000000ULL, "heap: one free run");
	check(nvrtx_heap_alloc(&heap, 0x1000, 0x1000, &a) == 0 && a == 0x10ff000ULL, "heap: first run from the top");
	check(nvrtx_heap_alloc(&heap, 0x3000, 0x10000, &b) == 0 && b == 0x10f0000ULL, "heap: aligned below the first");
	check(heap.count == 2 && heap.free[1].base == 0x10f3000ULL && heap.free[1].size == 0xc000ULL, "heap: alignment leaves a hole");
	check(nvrtx_heap_alloc(&heap, 0x2000, 0x1000, &c) == 0 && c == 0x10fd000ULL, "heap: the hole above is used first");
	check(nvrtx_heap_free(&heap, b, 0x3000) == 0, "heap: free a middle run");
	check(nvrtx_heap_free(&heap, b, 0x3000) == EINVAL, "heap: double free refused");
	check(nvrtx_heap_free(&heap, a, 0x1000) == 0 && nvrtx_heap_free(&heap, c, 0x2000) == 0, "heap: free the rest");
	check(heap.count == 1 && heap.free[0].base == 0x100000ULL && heap.free_bytes == 0x1000000ULL, "heap: runs merge back into one");
	check(nvrtx_heap_alloc(&heap, 0x2000000, 1, &a) == ENOSPC, "heap: too large refused");
	check(nvrtx_heap_alloc(&heap, 0, 1, &a) == EINVAL && nvrtx_heap_alloc(&heap, 0x1000, 3, &a) == EINVAL, "heap: empty size and odd align refused");
	check(nvrtx_heap_reserve(&heap, 0x200000ULL, 0x100000ULL) == 0 && heap.count == 2, "heap: reserve splits");
	check(nvrtx_heap_reserve(&heap, 0x280000ULL, 0x1000ULL) == EBUSY, "heap: reserve of a taken run refused");
	check(nvrtx_heap_free(&heap, 0x80000ULL, 0x1000ULL) == EINVAL, "heap: free outside the range refused");
	check(nvrtx_heap_free(&heap, 0x200000ULL, 0x100000ULL) == 0 && heap.count == 1, "heap: reserved run freed and merged");

	nvrtx_heap_init(&heap, 0, 0x10000000ULL);
	ok = 1;
	for (i = 0; i < 300; i++) {
		if (nvrtx_heap_alloc(&heap, 0x1000, 0x1000, &a) != 0)
			ok = 0;
	}
	for (i = 0; i < 300; i += 2) {
		if (nvrtx_heap_free(&heap, 0x10000000ULL - (uint64_t)(i + 1) * 0x1000, 0x1000) != 0 && i / 2 < NVRTX_HEAP_EXTENTS_MAX - 1)
			ok = 0;
	}
	check(ok && heap.count <= NVRTX_HEAP_EXTENTS_MAX, "heap: many holes stay bounded");

	nvrtx_heap_init(&heap, 5, 5);
	check(heap.count == 0 && nvrtx_heap_alloc(&heap, 1, 1, &a) == ENOSPC, "heap: empty range");

	memset(info, 0, sizeof(info));
	nvrtx_put32(info, 344, 2);
	r = 344 + 8;
	nvrtx_put64(info, r, 0x10000000ULL);
	nvrtx_put64(info, r + 8, 0x1f76fffffULL);
	info[r + 28] = 1;
	info[r + 29] = 1;
	r += 48;
	nvrtx_put64(info, r, 0x1f7700000ULL);
	nvrtx_put64(info, r + 8, 0x1ffffffffULL);
	nvrtx_put64(info, r + 16, 1);
	check(nvrtx_mmu_vram_region(info, &base, &top, &region) == 0 && region == 0 && base == 0x10000000ULL && top == 0x1f7700000ULL, "vram: usable region bounds");
	nvrtx_put32(info, 344, 0);
	check(nvrtx_mmu_vram_region(info, &base, &top, &region) == ENOSPC, "vram: no usable region");
	check(nvrtx_mmu_vram_pte(0x1f3f05000ULL) == ((0x1f3f05000ULL >> 4) | 1), "vram: page entry valid, VRAM, not volatile");

	/* P4m's BAR1 tables with the kept console (640x480, pitch 4096) mapped before the root switches. */
	check(nvrtx_mmu_gr_tables_bytes(0x10000000ULL) == sizeof(bar1_tables), "bar1: 128 page tables for 256 MiB");
	check(nvrtx_mmu_gr_tables(bar1_tables, 0x1e9370000ULL, 0, 0x10000000ULL) == 0, "bar1: tables built");
	check(nvrtx_mmu_gr_map_vram(bar1_tables, 0, 0x10000000ULL, 0, 0, 0x1e0000ULL) == 0, "bar1: console pages mapped in the image");
	check(nvrtx_get64(bar1_tables, 0) == (((0x1e9370000ULL + 0x1000) >> 4) | 2), "bar1: root entry at the second directory");
	check(nvrtx_get64(bar1_tables, 0x4000) == nvrtx_mmu_vram_pte(0), "bar1: console's first page on the VRAM's start");
	check(nvrtx_get64(bar1_tables, 0x4000 + 0x1df * 8) == nvrtx_mmu_vram_pte(0x1df000ULL), "bar1: console's last page");
	check(nvrtx_get64(bar1_tables, 0x4000 + 0x1e0 * 8) == 0, "bar1: first page after the console unmapped");
}

static void
test_objects(void)
{
	static uint8_t params[NVRTX_DEVINFO_PARAMS_BYTES];
	uint8_t call[64];
	uint8_t chan[NVRTX_CHANNEL_PARAMS_BYTES];
	uint8_t push[128];
	uint8_t entry[8];
	struct nvrtx_engine_pick pick;
	struct nvrtx_channel_setup setup;
	size_t n;

	nvrtx_pdir_params(params, 0x1f6f00000ULL, 0xc0de90f1U);
	n = nvrtx_control_call(call, sizeof(call), 0xc0de0001U, 0xc0de0080U, NVRTX_CTL_SET_PAGE_DIRECTORY, params, NVRTX_PDIR_PARAMS_BYTES);
	check(n == 56 && nvrtx_get32(call, 0) == 0xc0de0001U && nvrtx_get32(call, 4) == 0xc0de0080U && nvrtx_get32(call, 8) == 0x801813 && nvrtx_get32(call, 16) == 32, "control: header");
	check(nvrtx_get64(call, 24) == 0x1f6f00000ULL && nvrtx_get32(call, 32) == 4 && nvrtx_get32(call, 36) == 0 && nvrtx_get32(call, 40) == 0xc0de90f1U, "control: page directory");
	check(nvrtx_control_call(call, 40, 0, 0, 0, params, 32) == 0, "control: small buffer refused");
	nvrtx_space_params(params);
	check(nvrtx_get32(params, 0) == 0 && nvrtx_get32(params, 4) == 8 && nvrtx_get32(params, 32) == 0, "space: externally owned");

	memset(params, 0, sizeof(params));
	nvrtx_put32(params, 4, 4);
	nvrtx_put32(params, 12 + 0 * 100 + 8, 0x01);
	nvrtx_put32(params, 12 + 0 * 100 + 12, 0);
	nvrtx_put32(params, 12 + 1 * 100 + 8, 0x09);
	nvrtx_put32(params, 12 + 1 * 100 + 12, 0);
	nvrtx_put32(params, 12 + 2 * 100 + 8, 0x13);
	nvrtx_put32(params, 12 + 2 * 100 + 12, 2);
	nvrtx_put32(params, 12 + 3 * 100 + 8, 0x0a);
	nvrtx_put32(params, 12 + 3 * 100 + 12, 3);
	check(nvrtx_devinfo_pick(params, sizeof(params), &pick) == 0 && pick.engine == 0x0a && pick.runlist == 3 && pick.graphics_runlist == 0, "devinfo: copy engine of its own runlist");
	nvrtx_put32(params, 4, 3);
	check(nvrtx_devinfo_pick(params, sizeof(params), &pick) == ENOENT, "devinfo: none");
	nvrtx_put32(params, 4, 40);
	check(nvrtx_devinfo_pick(params, sizeof(params), &pick) == EINVAL, "devinfo: count too large");

	setup.ring_va = 0x20000000ULL;
	setup.ring_entries = 512;
	setup.chid = 8;
	setup.space = 0xc0de90f1U;
	setup.engine = 0x0a;
	setup.instance = 0x1f6f05000ULL;
	setup.userd = 0x1f6f06000ULL;
	setup.methods = 0x30000000ULL;
	setup.methods_size = 0x8000;
	setup.privileged = 0;
	nvrtx_channel_params(chan, &setup);
	check(nvrtx_get64(chan, 8) == 0x20000000ULL && nvrtx_get32(chan, 16) == 512 && nvrtx_get32(chan, 28) == 0xc0de90f1U && nvrtx_get32(chan, 128) == 0x0a, "channel: ring, space, engine");
	check(nvrtx_get32(chan, 20) == ((1u << 12) | (1u << 21)), "channel: flags (USERD index 0, page 1 fixed)");
	check(nvrtx_get64(chan, 144) == 0x1f6f05000ULL && nvrtx_get64(chan, 152) == 0x1000 && nvrtx_get32(chan, 160) == 2 && nvrtx_get32(chan, 164) == 1, "channel: instance in VRAM");
	check(nvrtx_get64(chan, 168) == 0x1f6f06000ULL && nvrtx_get64(chan, 176) == 0x200 && nvrtx_get64(chan, 200) == 0x200, "channel: USERD and RAMFC");
	check(nvrtx_get64(chan, 216) == 0x30000000ULL && nvrtx_get64(chan, 224) == 0x8000 && nvrtx_get32(chan, 232) == 1 && nvrtx_get32(chan, 236) == 0, "channel: method buffer in system memory");
	check(nvrtx_get32(chan, 244) == 0x14 && nvrtx_get32(chan, 360) == 0, "channel: internal flags, tail zero");

	n = nvrtx_push_fence(push, 0x20003000ULL, 0x1234);
	check(n == 52, "push: fence length");
	check(nvrtx_get32(push, 0) == ((1u << 29) | (3u << 16) | (0x5c >> 2)) && nvrtx_get32(push, 4) == 0x20003000U && nvrtx_get32(push, 8) == 0 && nvrtx_get32(push, 12) == 0x1234, "push: semaphore address and value");
	check(nvrtx_get32(push, 16) == ((1u << 29) | (1u << 16) | (0x6c >> 2)) && nvrtx_get32(push, 20) == 0x100001, "push: release with WFI");
	check(nvrtx_get32(push, 24) == ((1u << 29) | (4u << 16) | (0x28 >> 2)) && nvrtx_get32(push, 40) == (5u << 27), "push: membar");
	check(nvrtx_get32(push, 44) == ((1u << 29) | (1u << 16) | (0x20 >> 2)), "push: non-stall");
	nvrtx_ring_entry(entry, 0x120004000ULL, 13);
	check(nvrtx_get32(entry, 0) == 0x20004000U && nvrtx_get32(entry, 4) == (1u | (13u << 10)), "ring: entry");

	n = nvrtx_push_copy_fill(push, 4, 0xc5b5U, 0x20005100ULL, 64, 0xc0ffee01U);
	check(n == 56, "copy: fill length");
	check(nvrtx_get32(push, 0) == ((1u << 29) | (1u << 16) | (4u << 13)) && nvrtx_get32(push, 4) == 0xc5b5, "copy: class on subchannel 4");
	check(nvrtx_get32(push, 8) == ((1u << 29) | (1u << 16) | (4u << 13) | (0x700 >> 2)) && nvrtx_get32(push, 12) == 0xc0ffee01U, "copy: constant");
	check(nvrtx_get32(push, 20) == (4u | (3u << 16)), "copy: remap constant A, 4-byte components");
	check(nvrtx_get32(push, 24) == ((1u << 29) | (2u << 16) | (4u << 13) | (0x408 >> 2)) && nvrtx_get32(push, 28) == 0 && nvrtx_get32(push, 32) == 0x20005100U, "copy: destination");
	check(nvrtx_get32(push, 40) == 64 && nvrtx_get32(push, 44) == 1, "copy: one line of 64");
	n = nvrtx_push_copy_fill(push, 4, 0xc7b5U, 0x20005100ULL, 64, 0xc0ffee01U);
	check(n == 56 && nvrtx_get32(push, 4) == 0xc7b5, "copy: Ampere's class on subchannel 4");
	check(nvrtx_get32(push, 48) == ((1u << 29) | (1u << 16) | (4u << 13) | (0x300 >> 2)) && nvrtx_get32(push, 52) == (2u | 4u | 0x80u | 0x100u | 0x400u), "copy: launch");

	nvrtx_free_params(params, 0xc0de0001U, 0xc0dec46fU);
	check(nvrtx_get32(params, 0) == 0xc0de0001U && nvrtx_get32(params, 4) == 0 && nvrtx_get32(params, 8) == 0xc0dec46fU && nvrtx_get32(params, 12) == 0, "free: params");

	setup.privileged = 1;
	setup.chid = 1;
	nvrtx_channel_params(chan, &setup);
	check(nvrtx_get32(chan, 20) == ((1u << 8) | (1u << 21) | (1u << 5)), "channel: privileged flag, USERD index 1 page 0");
	check(nvrtx_get32(chan, 244) == 0x15, "channel: admin privilege");
}

/* The graphics golden context's parameters and tables. */
static void
test_gr(void)
{
	static uint8_t info[1664];
	static uint8_t promote[560];
	static uint8_t pdes[184];
	static uint8_t space[48];
	struct nvrtx_gr_buffer buffers[NVRTX_GR_BUFFER_MAX];
	struct nvrtx_promote_entry entries[2];
	uint8_t *image;
	size_t count;
	size_t length;
	uint64_t pte;

	/* Engine 0's sizes: main, pagepool, patch, bundle, attribute, rtv, fecs event, access map. */
	memset(info, 0, sizeof(info));
	nvrtx_put32(info, 0x00 * 8, 0x48123);
	nvrtx_put32(info, 0x0d * 8, 0x80000);
	nvrtx_put32(info, 0x10 * 8, 0x8000);
	nvrtx_put32(info, 0x11 * 8, 0x300000);
	nvrtx_put32(info, 0x13 * 8, 0x2c0000);
	nvrtx_put32(info, 0x14 * 8, 0x1000);
	nvrtx_put32(info, 0x17 * 8, 0x2000);
	nvrtx_put32(info, 0x18 * 8, 0x3000);
	count = nvrtx_gr_buffers(info, sizeof(info), buffers, NVRTX_GR_BUFFER_MAX);
	check(count == 9, "gr: nine buffers with the unrestricted map");
	check(buffers[0].buffer_id == 0 && buffers[0].size == 0x49000 + 0x40000 && buffers[0].page_shift == 16 && buffers[0].initialize && !buffers[0].global, "gr: main with subcontext headers, 64 KiB pages");
	check(buffers[1].buffer_id == 4 && buffers[1].global && !buffers[1].initialize && buffers[1].page_shift == 16, "gr: pagepool");
	check(buffers[2].buffer_id == 2 && !buffers[2].global && buffers[2].initialize && buffers[2].page_shift == 12, "gr: patch");
	check(buffers[3].buffer_id == 3 && buffers[3].page_shift == 21 && buffers[3].align_shift == 21, "gr: bundle, 2 MiB pages");
	check(buffers[4].buffer_id == 5 && buffers[4].page_shift == 21 && buffers[4].align_shift == 22, "gr: attribute aligned to its size");
	check(buffers[6].buffer_id == 9 && buffers[6].global && buffers[6].initialize, "gr: fecs event");
	check(buffers[7].buffer_id == 10 && buffers[7].read_only && buffers[8].buffer_id == 11 && buffers[8].read_only && buffers[8].size == 0x3000, "gr: the access map and its unrestricted twin");
	nvrtx_put32(info, 0x14 * 8, 0);
	check(nvrtx_gr_buffers(info, sizeof(info), buffers, NVRTX_GR_BUFFER_MAX) == 0, "gr: a missing buffer refuses the list");
	check(nvrtx_gr_buffers(info, 100, buffers, NVRTX_GR_BUFFER_MAX) == 0, "gr: short information refused");

	/* The promote: header and two entries. */
	entries[0].buffer_id = 0;
	entries[0].initialize = 1;
	entries[0].nonmapped = 0;
	entries[0].virtual_address = 0x20000000ULL;
	entries[0].physical = 0x1e8400000ULL;
	entries[0].size = 0x89000;
	entries[1].buffer_id = 10;
	entries[1].initialize = 1;
	entries[1].nonmapped = 1;
	entries[1].virtual_address = 0x20400000ULL;
	entries[1].physical = 0x1e8800000ULL;
	entries[1].size = 0x3000;
	nvrtx_promote_params(promote, 0xc1d00000U, 0xf1f00001U, entries, 2);
	check(nvrtx_get32(promote, 0) == 1 && nvrtx_get32(promote, 4) == 0 && nvrtx_get32(promote, 12) == 0xc1d00000U && nvrtx_get32(promote, 16) == 0xf1f00001U && nvrtx_get32(promote, 40) == 2, "promote: engine, client, channel, count");
	check(nvrtx_get64(promote, 48) == 0x1e8400000ULL && nvrtx_get64(promote, 56) == 0x20000000ULL && nvrtx_get64(promote, 64) == 0x89000 && nvrtx_get32(promote, 72) == 4 && promote[76] == 0 && promote[77] == 0 && promote[78] == 1 && promote[79] == 0, "promote: main entry");
	check(nvrtx_get64(promote, 80) == 0x1e8800000ULL && nvrtx_get64(promote, 88) == 0 && promote[108] == 10 && promote[110] == 1 && promote[111] == 1, "promote: access map entry, not mapped");

	/* The copy of the server's directories. */
	nvrtx_copy_pdes_params(pdes, 0x1e8000000ULL, 0x1e8001000ULL, 0x1e8002000ULL);
	check(nvrtx_get64(pdes, 8) == 0x20000000ULL && nvrtx_get64(pdes, 16) == 0x100000000ULL && nvrtx_get64(pdes, 24) == 0x11fffffffULL && nvrtx_get32(pdes, 32) == 3, "pdes: 512 MiB at 4 GiB, three levels");
	check(nvrtx_get64(pdes, 40) == 0x1e8000000ULL && nvrtx_get64(pdes, 48) == 32 && nvrtx_get32(pdes, 56) == 1 && nvrtx_get32(pdes, 60) == 47, "pdes: root");
	check(nvrtx_get64(pdes, 64) == 0x1e8001000ULL && nvrtx_get64(pdes, 72) == 4096 && nvrtx_get32(pdes, 84) == 38, "pdes: second level");
	check(nvrtx_get64(pdes, 88) == 0x1e8002000ULL && nvrtx_get64(pdes, 96) == 4096 && nvrtx_get32(pdes, 108) == 29 && nvrtx_get32(pdes, 0) == 0 && nvrtx_get32(pdes, 4) == 0, "pdes: first level, subdevice 0");

	nvrtx_space_params_shared(space);
	check(nvrtx_get32(space, 0) == 0 && nvrtx_get32(space, 4) == 0, "space: shared, no flags");

	/* The tables: 8 MiB at 0x20000000, then a privileged read-only page. */
	length = nvrtx_mmu_gr_tables_bytes(0x800000ULL);
	check(length == 0x4000 + 4 * 0x1000, "gr tables: four page tables");
	image = calloc(1, length);
	check(nvrtx_mmu_gr_tables(image, 0x1e8000000ULL, 0x20000000ULL, 0x800000ULL) == 0, "gr tables: built");
	check(nvrtx_get64(image, 0) == ((0x1e8001000ULL >> 4) | 2) && nvrtx_get64(image, 0x1000) == ((0x1e8002000ULL >> 4) | 2), "gr tables: root and second level on index 0");
	check(nvrtx_get64(image, 0x2000 + 1 * 8) == ((0x1e8003000ULL >> 4) | 2) && nvrtx_get64(image, 0x2000 + 8 * 8) == 0, "gr tables: first level entry 1, the server's entry 8 left");
	check(nvrtx_get64(image, 0x3000 + 0 * 16 + 8) == ((0x1e8004000ULL >> 4) | 2) && nvrtx_get64(image, 0x3000 + 3 * 16 + 8) == ((0x1e8007000ULL >> 4) | 2) && nvrtx_get64(image, 0x3000 + 4 * 16 + 8) == 0, "gr tables: one page table per 2 MiB");
	check(nvrtx_mmu_gr_map(image, 0x20000000ULL, 0x800000ULL, 0x20201000ULL, 0x1e8400000ULL, 0x2000, 1) == 0, "gr map: two pages");
	pte = nvrtx_get64(image, 0x4000 + (0x201000 >> 12) * 8);
	check(pte == ((0x1e8400000ULL >> 4) | 1 | (1u << 5) | (1u << 6)), "gr map: valid, VRAM, privileged, read-only");
	check(nvrtx_get64(image, 0x4000 + (0x202000 >> 12) * 8) == ((0x1e8401000ULL >> 4) | 1 | (1u << 5) | (1u << 6)), "gr map: second page");
	check(nvrtx_mmu_gr_map(image, 0x20000000ULL, 0x800000ULL, 0x207ff000ULL, 0x1e8400000ULL, 0x2000, 0) == EINVAL, "gr map: run past the range refused");
	check(nvrtx_mmu_gr_tables(image, 0x1e8000000ULL, 0xf0000000ULL, 0x20000000ULL) == EINVAL, "gr tables: range into the server's refused");
	check(nvrtx_mmu_gr_tables(image, 0x1e8000000ULL, 0x20100000ULL, 0x200000ULL) == EINVAL, "gr tables: unaligned range refused");
	free(image);
}

/* The display records: push words, hash-table entries, context DMA, parameters. */
static void
test_disp(void)
{
	uint8_t memory[64 * 4];
	uint8_t small[3 * 4];
	uint8_t entry[NVRTX_DISP_RAMHT_ENTRY_BYTES];
	uint8_t object[NVRTX_DISP_CONTEXT_DMA_BYTES];
	uint8_t pb[NVRTX_DISP_PUSHBUFFER_BYTES];
	uint8_t chan[NVRTX_DISP_CHANNEL_BYTES];
	uint32_t armed[NVRTX_DISP_HEAD_COPIED];
	uint8_t assign[NVRTX_DISP_ASSIGN_SOR_BYTES];
	struct nvrtx_disp_mode mode;
	struct nvrtx_push push;
	struct nvrtx_disp_surface surface;
	uint32_t i;

	/* One method: header (count 1 at bit 18, method offset), then the word. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_push_one(&push, NVRTX_CORE_SET_CONTEXT_DMA_NOTIFIER, NVRTX_DISP_HANDLE_SYNC);
	check(push.count == 2 && nvrtx_get32(memory, 0) == 0x00040208U && nvrtx_get32(memory, 4) == 0xf0000000U, "disp push one method");

	/* A run that does not fit writes nothing and marks the overflow. */
	nvrtx_push_init(&push, small, 3);
	nvrtx_push_method(&push, 0x1004, armed, 3);
	check(push.count == 0 && push.overflow == 1, "disp push overflow");

	/* The hash folds the handle by 10 bits and moves the channel up by 6. */
	check(nvrtx_disp_ramht_hash(0, 0xf0000000U) == 0x303U, "disp ramht hash core sync");
	check(nvrtx_disp_ramht_hash(0, 0xf0000001U) == 0x302U, "disp ramht hash core vram");
	check(nvrtx_disp_ramht_hash(1, 0xf0000000U) == 0x343U, "disp ramht hash window 0 sync");

	/* The entry: the handle, then chid 25+, object offset 9+, client's low 14 bits. */
	nvrtx_disp_ramht_entry(entry, 0xf0000001U, 1, 0xc1d00000U, 0x2040);
	check(nvrtx_get32(entry, 0) == 0xf0000001U && nvrtx_get32(entry, 4) == 0x02408000U, "disp ramht entry");

	/* The context DMA object over the VRAM: flags 0x45, start and limit in 256-byte units. */
	nvrtx_disp_context_dma(object, 0x1f3e10000ULL, 0x1f3e10fffULL);
	check(nvrtx_get32(object, 0) == 0x45U && nvrtx_get32(object, 4) == 0x01f3e100U && nvrtx_get32(object, 8) == 0 &&
	    nvrtx_get32(object, 12) == 0x01f3e10fU && nvrtx_get32(object, 16) == 0 && nvrtx_get32(object, 20) == 0, "disp context dma");

	/* The push buffer's registration (r570 layout: 56 bytes). */
	nvrtx_disp_pushbuffer_params(pb, 0x123456000ULL, 0x1000, NVRTX_DISP_CLASS_WINDOW, 2);
	check(nvrtx_get32(pb, 0) == 1 && nvrtx_get32(pb, 8) == 0x23456000U && nvrtx_get32(pb, 12) == 1 &&
	    nvrtx_get32(pb, 16) == 0xfffU && nvrtx_get32(pb, 24) == 1 && nvrtx_get32(pb, 28) == 0xc57eU &&
	    nvrtx_get32(pb, 32) == 2 && pb[36] == 1 && nvrtx_get32(pb, 40) == 3 && nvrtx_get32(pb, 44) == 0 &&
	    nvrtx_get32(pb, 48) == 1, "disp pushbuffer params");

	/* The channel's allocation (40 bytes): instance at 0, put 0, subdevice mask at 32. */
	nvrtx_disp_channel_params(chan, 2);
	check(nvrtx_get32(chan, 0) == 2 && nvrtx_get32(chan, 12) == 0 && nvrtx_get32(chan, 32) == 1, "disp channel params");

	/* The mode from the armed state: the visible size from the blank's start less its end. */
	for (i = 0; i < NVRTX_DISP_HEAD_COPIED; i++)
		armed[i] = 0x1000U + i;
	armed[NVRTX_DISP_ARMED_RASTER_SIZE] = 0x042a0698U;
	armed[NVRTX_DISP_ARMED_BLANK_END] = 0x00290168U;
	armed[NVRTX_DISP_ARMED_BLANK_START] = 0x04290668U;
	armed[NVRTX_DISP_ARMED_PIXEL_CLOCK] = 0x066ff300U;
	nvrtx_disp_mode_from_armed(armed, &mode);
	check(mode.active == 0x04000500U && mode.raster_size == 0x042a0698U && mode.clock_hz == 108000000U &&
	    mode.output_resource == 0x100eU && mode.dither == 0x100bU, "disp mode from armed");

	/* The head's mode on head 1 (0x2400 on): view in and out, the raster run, then nouveau's fixed words. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_core_head_mode(&push, 1, &mode);
	check(push.overflow == 0 && nvrtx_get32(memory, 0) == 0x0004244cU && nvrtx_get32(memory, 4) == 0x04000500U &&
	    nvrtx_get32(memory, 8) == 0x00042458U && nvrtx_get32(memory, 12) == 0x04000500U &&
	    nvrtx_get32(memory, 16) == 0x00102464U && nvrtx_get32(memory, 20) == 0x042a0698U &&
	    nvrtx_get32(memory, 32) == 0x04290668U, "disp head mode view and raster");
	check(nvrtx_get32(memory, 36) == 0x00042474U && nvrtx_get32(memory, 44) == 0x00042408U && nvrtx_get32(memory, 48) == 0 &&
	    nvrtx_get32(memory, 52) == 0x0004240cU && nvrtx_get32(memory, 56) == 108000000U &&
	    nvrtx_get32(memory, 60) == 0x00042428U && nvrtx_get32(memory, 68) == 0x00042430U && nvrtx_get32(memory, 72) == 0x1114U,
	    "disp head mode control, clock and usage");
	check(nvrtx_get32(memory, 76) == 0x00042418U && nvrtx_get32(memory, 80) == 0x100bU && nvrtx_get32(memory, 84) == 0x00042400U &&
	    nvrtx_get32(memory, 88) == 0 && nvrtx_get32(memory, 92) == 0x00042404U && nvrtx_get32(memory, 96) == 0x100eU &&
	    push.count == 25, "disp head mode dither, procamp, output resource");

	/* The display id of head 1 and the SOR control. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_core_display_id(&push, 1, 0x200);
	nvrtx_disp_core_sor(&push, 1, (1U << NVRTX_DISP_SOR_PROTOCOL_SHIFT) | 1U);
	check(nvrtx_get32(memory, 0) == 0x00042420U && nvrtx_get32(memory, 4) == 0x200U &&
	    nvrtx_get32(memory, 8) == 0x00040320U && nvrtx_get32(memory, 12) == 0x101U, "disp display id and SOR control");

	/* The SOR assignment: 80 bytes, the display at 4, the exclusion at 8; the reply's tagged list at 40. */
	nvrtx_disp_assign_sor_params(assign, 0x200, 0x2);
	check(nvrtx_get32(assign, 0) == 0 && nvrtx_get32(assign, 4) == 0x200U && assign[8] == 2 && nvrtx_get32(assign, 76) == 0,
	    "disp assign SOR params");
	check(nvrtx_disp_assign_sor_result(assign, 0x200) == NVRTX_DISP_SOR_NONE, "disp assign SOR none");
	nvrtx_put32(assign, 40 + 8, 0x200);
	check(nvrtx_disp_assign_sor_result(assign, 0x200) == 1, "disp assign SOR 1");

	/* The jump back to the start of a push buffer. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_push_jump(&push, 0);
	check(push.count == 1 && nvrtx_get32(memory, 0) == 0x20000000U, "disp push jump");

	/* The core update: notifier on, interlocks (none, window 0), update, notifier off. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_core_update(&push, 0, 1);
	check(push.count == 9 && nvrtx_get32(memory, 4) == 0x1000U && nvrtx_get32(memory, 8) == 0x00080218U &&
	    nvrtx_get32(memory, 12) == 0 && nvrtx_get32(memory, 16) == 1 && nvrtx_get32(memory, 20) == 0x00040200U &&
	    nvrtx_get32(memory, 24) == 1 && nvrtx_get32(memory, 32) == 0, "disp core update");

	/* The window's image: the pitch in 64-byte units, offset in 256-byte units. */
	surface.offset = 0x1e0000000ULL;
	surface.width = 1280;
	surface.height = 1024;
	surface.pitch = 5120;
	surface.handle = NVRTX_DISP_HANDLE_VRAM;
	surface.format = NVRTX_DISP_FORMAT_A8R8G8B8;
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_window_image(&push, &surface);
	check(nvrtx_get32(memory, 0) == 0x00040308U && nvrtx_get32(memory, 4) == 1 && nvrtx_get32(memory, 8) == 0x00100224U &&
	    nvrtx_get32(memory, 12) == 0x04000500U && nvrtx_get32(memory, 16) == 0x10 && nvrtx_get32(memory, 20) == 0xcfU &&
	    nvrtx_get32(memory, 24) == 80 && nvrtx_get32(memory, 32) == 0xf0000001U && nvrtx_get32(memory, 40) == 0x01e00000U,
	    "disp window image");

	/* The window update with the core: interlock bit 0, the window's own bit. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_window_update(&push, 1, 2);
	check(nvrtx_get32(memory, 0) == 0x00080370U && nvrtx_get32(memory, 4) == 1 && nvrtx_get32(memory, 8) == 4 &&
	    nvrtx_get32(memory, 12) == 0x00040200U, "disp window update");

	/* The bands wrap after eight. */
	check(nvrtx_disp_band_color(0) == 0xffffffffU && nvrtx_disp_band_color(8) == 0xffffffffU &&
	    nvrtx_disp_band_color(7) == 0xff000000U, "disp band colours");
}

/* The cursor and display-control records: the PIO channel, the core's cursor methods, positions, pixels, frames. */
static void
test_disp_cursor(void)
{
	uint8_t memory[64 * 4];
	uint8_t pb[NVRTX_DISP_PUSHBUFFER_BYTES];
	uint8_t pio[NVRTX_DISP_PIO_CHANNEL_BYTES];
	struct nvrtx_push push;
	uint32_t i;
	int zero;

	/* The PIO channel's registration: class and instance, not valid, subdevice 0, no memory (r570 layout). */
	memset(pb, 0xa5, sizeof(pb));
	nvrtx_disp_pio_pushbuffer_params(pb, NVRTX_DISP_CLASS_CURSOR, 1);
	check(nvrtx_get32(pb, 0) == 0 && nvrtx_get32(pb, 8) == 0 && nvrtx_get32(pb, 12) == 0 && nvrtx_get32(pb, 16) == 0 &&
	    nvrtx_get32(pb, 24) == 0 && nvrtx_get32(pb, 28) == 0xc57aU && nvrtx_get32(pb, 32) == 1 && pb[36] == 0 &&
	    nvrtx_get32(pb, 40) == 0 && nvrtx_get32(pb, 48) == 1, "disp cursor pushbuffer params");

	/* The PIO channel's allocation (16 bytes): the instance, nothing else. */
	memset(pio, 0xa5, sizeof(pio));
	nvrtx_disp_pio_channel_params(pio, 1);
	zero = 1;
	for (i = 4; i < NVRTX_DISP_PIO_CHANNEL_BYTES; i++) {
		if (pio[i] != 0)
			zero = 0;
	}
	check(nvrtx_get32(pio, 0) == 1 && zero, "disp cursor channel params");

	/* The cursor on head 0: control and composition in one run, the colour scale 255/256, the object, the offset in 256-byte units. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_core_cursor_set(&push, 0, NVRTX_DISP_HANDLE_VRAM, 0x1e0040000ULL);
	check(push.count == 9 && nvrtx_get32(memory, 0) == 0x0008209cU && nvrtx_get32(memory, 4) == 0x800001cfU &&
	    nvrtx_get32(memory, 8) == 0x000072ffU && nvrtx_get32(memory, 12) == 0x00042228U &&
	    nvrtx_get32(memory, 16) == 0x00000ff0U && nvrtx_get32(memory, 20) == 0x00042088U &&
	    nvrtx_get32(memory, 24) == 0xf0000001U && nvrtx_get32(memory, 28) == 0x00042090U &&
	    nvrtx_get32(memory, 32) == 0x01e00400U, "disp core cursor set");

	/* The cursor off on head 1: control A8R8G8B8 disabled, no object. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_core_cursor_clear(&push, 1);
	check(push.count == 4 && nvrtx_get32(memory, 0) == 0x0004249cU && nvrtx_get32(memory, 4) == 0xcfU &&
	    nvrtx_get32(memory, 8) == 0x00042488U && nvrtx_get32(memory, 12) == 0, "disp core cursor clear");

	/* The core's update for a cursor: notifier on, no interlock (cursor or window), update, notifier off. */
	nvrtx_push_init(&push, memory, 64);
	nvrtx_disp_core_update_cursor(&push, 0);
	check(push.count == 9 && nvrtx_get32(memory, 0) == 0x0004020cU && nvrtx_get32(memory, 4) == 0x1000U &&
	    nvrtx_get32(memory, 8) == 0x00080218U && nvrtx_get32(memory, 12) == 0 && nvrtx_get32(memory, 16) == 0 &&
	    nvrtx_get32(memory, 20) == 0x00040200U && nvrtx_get32(memory, 24) == 1 && nvrtx_get32(memory, 32) == 0,
	    "disp core update with the cursor");

	/* Positions: x low, y high, 16-bit two's complement, held inside 16 bits. */
	check(nvrtx_disp_cursor_point(10, 20) == 0x0014000aU, "disp cursor point");
	check(nvrtx_disp_cursor_point(-5, -1) == 0xfffffffbU, "disp cursor point negative");
	check(nvrtx_disp_cursor_point(40000, -40000) == 0x80007fffU, "disp cursor point held");

	/* Pixels: BGRA8888 is A8R8G8B8 already, RGBA8888 swaps red and blue. */
	check(nvrtx_disp_cursor_pixel(0x80112233U, 0) == 0x80112233U, "disp cursor pixel BGRA");
	check(nvrtx_disp_cursor_pixel(0x80112233U, 1) == 0x80332211U, "disp cursor pixel RGBA");

	/* The 16-bit frame counter's movement, across its wrap. */
	check(nvrtx_disp_frame_advance(10, 12) == 2 && nvrtx_disp_frame_advance(5, 5) == 0 &&
	    nvrtx_disp_frame_advance(0xfffe, 0x0001) == 3, "disp frame advance");
}

int
main(int argc, char **argv)
{
	test_wire();
	test_layout();
	test_layout_ampere();
	test_hold();
	test_radix3();
	test_queue();
	test_vbios();
	test_records();
	test_mmu();
	test_work();
	test_disp_first();
	test_heap();
	test_objects();
	test_gr();
	test_disp();
	test_disp_cursor();
	if (argc > 1) {
		test_files(argv[1]);
	} else {
		printf("skip files: no firmware directory given\n");
		skipped++;
	}

	if (failures != 0) {
		printf("FAIL nvrtx gsp: %u of %u checks, SKIPPED %u\n", failures, checks, skipped);
		return 1;
	}
	printf("PASS nvrtx gsp: %u checks, SKIPPED %u\n", checks, skipped);
	return 0;
}
