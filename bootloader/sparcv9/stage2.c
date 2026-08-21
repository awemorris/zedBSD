/*
 * zedBSD SPARC V9 second-stage FAT16/ELF64 loader.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "ofw.h"
#include "handoff.h"

#define SECTOR_SIZE 512U
#define KERNEL_PHYS_BASE 0x00400000ULL
#define KERNEL_WINDOW_SIZE 0x00400000UL
#define KERNEL_DIRECT_BASE 0xfffff80000000000ULL
#define KERNEL_WINDOW_VA (KERNEL_DIRECT_BASE + KERNEL_PHYS_BASE)
#define KERNEL_HANDOFF_VA (KERNEL_DIRECT_BASE + 0x007fe000ULL)
#define ELF_HEADER_SIZE 64U
#define ELF_PROGRAM_HEADER_SIZE 56U
#define ELF_PROGRAM_HEADER_MAX 16U
#define ELF_PT_LOAD 1U
#define ELF_PF_X 1U
#define FAT_CLUSTER_END 0xfff8U
#define KERNEL_FILE_LIMIT (2U * 1024U * 1024U)

struct fat_volume {
	ofw_cell_t disk;
	unsigned int sectors_per_cluster;
	unsigned int fat_start;
	unsigned int root_start;
	unsigned int root_sectors;
	unsigned int data_start;
	unsigned int sectors_per_fat;
	unsigned int total_sectors;
};

struct fat_file {
	const struct fat_volume *volume;
	unsigned int first_cluster;
	unsigned long size;
};

struct load_segment {
	unsigned long long start;
	unsigned long long end;
	unsigned long long offset;
	unsigned long long file_size;
	unsigned long long memory_size;
	unsigned int flags;
};

static unsigned char sector_buffer[SECTOR_SIZE];
static unsigned char kernel_image[KERNEL_FILE_LIMIT];
static struct sun4u_boot_handoff handoff_image;

void sparcv9_loader_enable_mmu(void);

static unsigned int
get16le(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8;
}

static unsigned long
get32le(const unsigned char *bytes)
{
	return (unsigned long)get16le(bytes) |
	    (unsigned long)get16le(bytes + 2) << 16;
}

static unsigned int
get16be(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] << 8 | bytes[1];
}

static unsigned long
get32be(const unsigned char *bytes)
{
	return (unsigned long)get16be(bytes) << 16 | get16be(bytes + 2);
}

static unsigned long long
get64be(const unsigned char *bytes)
{
	return (unsigned long long)get32be(bytes) << 32 | get32be(bytes + 4);
}

static void
copy_bytes(void *destination, const void *source, unsigned long size)
{
	unsigned char *out = destination;
	const unsigned char *in = source;

	while (size-- != 0)
		*out++ = *in++;
}

static void
zero_bytes(void *destination, unsigned long size)
{
	unsigned char *out = destination;

	while (size-- != 0)
		*out++ = 0;
}

static int
read_exact(ofw_cell_t disk, unsigned long long offset, void *buffer,
	    unsigned long size)
{
	unsigned char *bytes = buffer;

	if (ofw_seek(disk, offset) != 0)
		return -1;
	while (size != 0) {
		long count = ofw_read(disk, bytes, size);

		if (count <= 0 || (unsigned long)count > size)
			return -1;
		bytes += count;
		size -= (unsigned long)count;
	}
	return 0;
}

static int
read_sector(const struct fat_volume *volume, unsigned int sector,
	    unsigned char *buffer)
{
	if (sector >= volume->total_sectors)
		return -1;
	return read_exact(volume->disk,
	    (unsigned long long)sector * SECTOR_SIZE, buffer, SECTOR_SIZE);
}

static int
fat_mount(struct fat_volume *volume, ofw_cell_t disk)
{
	unsigned int reserved;
	unsigned int fats;
	unsigned int root_entries;
	unsigned int total;

	volume->disk = disk;
	volume->total_sectors = ~0U;
	if (read_sector(volume, 0, sector_buffer) != 0)
		return -1;
	if (get16le(sector_buffer + 11) != SECTOR_SIZE)
		return -1;
	volume->sectors_per_cluster = sector_buffer[13];
	reserved = get16le(sector_buffer + 14);
	fats = sector_buffer[16];
	root_entries = get16le(sector_buffer + 17);
	total = get16le(sector_buffer + 19);
	if (total == 0)
		total = (unsigned int)get32le(sector_buffer + 32);
	volume->sectors_per_fat = get16le(sector_buffer + 22);
	if (volume->sectors_per_cluster == 0 ||
	    (volume->sectors_per_cluster &
	    (volume->sectors_per_cluster - 1U)) != 0 ||
	    reserved == 0 || fats == 0 || root_entries == 0 ||
	    volume->sectors_per_fat == 0 || total == 0)
		return -1;
	volume->total_sectors = total;
	volume->fat_start = reserved;
	volume->root_start = reserved + fats * volume->sectors_per_fat;
	volume->root_sectors = (root_entries * 32U + SECTOR_SIZE - 1U) /
	    SECTOR_SIZE;
	volume->data_start = volume->root_start + volume->root_sectors;
	if (volume->data_start >= total)
		return -1;
	return 0;
}

static int
fat_find_kernel(const struct fat_volume *volume, struct fat_file *file)
{
	static const unsigned char name[11] = {
		'V', 'M', 'U', 'N', 'I', 'X', ' ', ' ', 'S', '9', ' '
	};
	unsigned int sector;

	for (sector = 0; sector < volume->root_sectors; sector++) {
		unsigned int offset;

		if (read_sector(volume, volume->root_start + sector,
		    sector_buffer) != 0)
			return -1;
		for (offset = 0; offset < SECTOR_SIZE; offset += 32U) {
			const unsigned char *entry = sector_buffer + offset;
			unsigned int index;

			if (entry[0] == 0)
				return -1;
			if (entry[0] == 0xe5 || entry[11] == 0x0f ||
			    (entry[11] & 0x18U) != 0)
				continue;
			for (index = 0; index < sizeof(name); index++)
				if (entry[index] != name[index])
					break;
			if (index != sizeof(name))
				continue;
			file->volume = volume;
			file->first_cluster = get16le(entry + 26);
			file->size = get32le(entry + 28);
			if (file->first_cluster < 2 || file->size < ELF_HEADER_SIZE)
				return -1;
			return 0;
		}
	}
	return -1;
}

static int
fat_next_cluster(const struct fat_volume *volume, unsigned int cluster,
	    unsigned int *next)
{
	unsigned long offset = (unsigned long)cluster * 2U;
	unsigned int sector = volume->fat_start +
	    (unsigned int)(offset / SECTOR_SIZE);
	unsigned int within = (unsigned int)(offset % SECTOR_SIZE);

	if (read_sector(volume, sector, sector_buffer) != 0)
		return -1;
	*next = get16le(sector_buffer + within);
	return 0;
}

static int
fat_read_file(const struct fat_file *file, unsigned long offset,
	    void *destination, unsigned long size)
{
	const struct fat_volume *volume = file->volume;
	unsigned long cluster_size =
	    (unsigned long)volume->sectors_per_cluster * SECTOR_SIZE;
	unsigned int cluster = file->first_cluster;
	unsigned long skip;
	unsigned long steps = 0;
	unsigned char *out = destination;

	if (offset > file->size || size > file->size - offset)
		return -1;
	for (skip = offset; skip >= cluster_size; skip -= cluster_size) {
		if (fat_next_cluster(volume, cluster, &cluster) != 0 ||
		    cluster < 2 || cluster >= FAT_CLUSTER_END)
			return -1;
		if (++steps > volume->total_sectors)
			return -1;
	}
	while (size != 0) {
		unsigned long available = cluster_size - skip;
		unsigned long amount = size < available ? size : available;

		while (amount != 0) {
			unsigned int cluster_sector = (unsigned int)(skip /
			    SECTOR_SIZE);
			unsigned int within = (unsigned int)(skip % SECTOR_SIZE);
			unsigned long chunk = SECTOR_SIZE - within;
			unsigned int data_sector = volume->data_start +
			    (cluster - 2U) * volume->sectors_per_cluster +
			    cluster_sector;

			if (chunk > amount)
				chunk = amount;
			if (read_sector(volume, data_sector, sector_buffer) != 0)
				return -1;
			copy_bytes(out, sector_buffer + within, chunk);
			out += chunk;
			skip += chunk;
			amount -= chunk;
			size -= chunk;
		}
		if (size == 0)
			break;
		if (fat_next_cluster(volume, cluster, &cluster) != 0 ||
		    cluster < 2 || cluster >= FAT_CLUSTER_END)
			return -1;
		if (++steps > volume->total_sectors)
			return -1;
		skip = 0;
	}
	return 0;
}

static int
make_fat_path(char *path, unsigned long capacity)
{
	int length = ofw_bootpath(path, capacity);
	int colon = -1;
	int index;

	if (length <= 0)
		return -1;
	for (index = 0; index < length; index++)
		if (path[index] == ':')
			colon = index;
	if (colon >= 0) {
		if ((unsigned long)colon + 3U > capacity)
			return -1;
		path[colon + 1] = 'b';
		path[colon + 2] = '\0';
	} else {
		if ((unsigned long)length + 3U > capacity)
			return -1;
		path[length++] = ':';
		path[length++] = 'b';
		path[length] = '\0';
	}
	return 0;
}

static int
power_of_two(unsigned long long value)
{
	return value != 0 && (value & (value - 1ULL)) == 0;
}

static void
flush_instruction_cache(unsigned long long start, unsigned long long end)
{
	start &= ~31ULL;
	while (start < end) {
		__asm__ volatile("flush %0" : : "r"((void *)start) : "memory");
		start += 32U;
	}
	__asm__ volatile("membar #Sync" : : : "memory");
}

static int
load_kernel(const unsigned char *image, unsigned long image_size,
	    const struct sun4u_boot_handoff *handoff,
	    unsigned long long *entry_out)
{
	const unsigned char *header = image;
	struct load_segment segments[ELF_PROGRAM_HEADER_MAX];
	unsigned long long entry;
	unsigned long long phoff;
	unsigned int phnum;
	unsigned int loaded = 0;
	unsigned int index;
	int entry_is_executable = 0;

	if (image_size < ELF_HEADER_SIZE)
		return -1;
	ofw_puts("SPARCV9 ELF HEADER\n");
	if (header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' ||
	    header[3] != 'F' || header[4] != 2 || header[5] != 2 ||
	    header[6] != 1 || get16be(header + 16) != 2 ||
	    get16be(header + 18) != 43 || get32be(header + 20) != 1 ||
	    get16be(header + 52) != ELF_HEADER_SIZE ||
	    get16be(header + 54) != ELF_PROGRAM_HEADER_SIZE)
		return -1;
	entry = get64be(header + 24);
	phoff = get64be(header + 32);
	phnum = get16be(header + 56);
	if (phnum == 0 || phnum > ELF_PROGRAM_HEADER_MAX ||
	    phoff > image_size ||
	    (unsigned long long)phnum * ELF_PROGRAM_HEADER_SIZE >
	    image_size - phoff)
		return -1;
	for (index = 0; index < phnum; index++) {
		const unsigned char *program = image + phoff +
		    (unsigned long long)index * ELF_PROGRAM_HEADER_SIZE;
		unsigned long long offset;
		unsigned long long virtual_address;
		unsigned long long physical_address;
		unsigned long long file_size;
		unsigned long long memory_size;
		unsigned long long alignment;
		unsigned int flags;
		unsigned int other;

		if (get32be(program) != ELF_PT_LOAD)
			continue;
		flags = (unsigned int)get32be(program + 4);
		offset = get64be(program + 8);
		virtual_address = get64be(program + 16);
		physical_address = get64be(program + 24);
		file_size = get64be(program + 32);
		memory_size = get64be(program + 40);
		alignment = get64be(program + 48);
		if (memory_size == 0 || file_size > memory_size ||
		    physical_address < KERNEL_PHYS_BASE ||
		    memory_size > KERNEL_WINDOW_SIZE ||
		    physical_address - KERNEL_PHYS_BASE >
		    KERNEL_WINDOW_SIZE - memory_size ||
		    virtual_address != KERNEL_DIRECT_BASE + physical_address ||
		    offset > image_size || file_size > image_size - offset ||
		    !power_of_two(alignment) || alignment > KERNEL_WINDOW_SIZE ||
		    (virtual_address & (alignment - 1ULL)) !=
		    (offset & (alignment - 1ULL)))
			return -1;
		for (other = 0; other < loaded; other++)
			if (virtual_address < segments[other].end &&
			    virtual_address + memory_size > segments[other].start)
				return -1;
		segments[loaded].start = virtual_address;
		segments[loaded].end = virtual_address + memory_size;
		segments[loaded].offset = offset;
		segments[loaded].file_size = file_size;
		segments[loaded].memory_size = memory_size;
		segments[loaded].flags = flags;
		loaded++;
		if ((flags & ELF_PF_X) != 0) {
			if (entry >= virtual_address &&
			    entry < virtual_address + memory_size)
				entry_is_executable = 1;
		}
	}
	if (loaded == 0 || !entry_is_executable)
		return -1;
	if (ofw_claim_fixed((void *)KERNEL_WINDOW_VA,
	    KERNEL_PHYS_BASE, KERNEL_WINDOW_SIZE) !=
	    (void *)KERNEL_WINDOW_VA)
		return -1;
	ofw_puts("SPARCV9 ELF PASS\n");
	sparcv9_loader_enable_mmu();
	for (index = 0; index < loaded; index++) {
		const struct load_segment *segment = &segments[index];

		if (segment->file_size != 0)
			copy_bytes((void *)segment->start,
			    image + segment->offset,
			    (unsigned long)segment->file_size);
		zero_bytes((void *)(segment->start + segment->file_size),
		    (unsigned long)(segment->memory_size -
		    segment->file_size));
		if ((segment->flags & ELF_PF_X) != 0)
			flush_instruction_cache(segment->start, segment->end);
	}
	zero_bytes((void *)KERNEL_HANDOFF_VA, 8192U);
	copy_bytes((void *)KERNEL_HANDOFF_VA, handoff, sizeof(*handoff));
	*entry_out = entry;
	return 0;
}

void
sparcv9_stage2_main(ofw_client_t client, ofw_cell_t boot_disk)
{
	typedef void (*kernel_entry_t)(ofw_client_t, ofw_cell_t, void *);
	struct fat_volume volume;
	struct fat_file file;
	unsigned long long entry;
	char path[256];
	char bootpath[256];
	ofw_scell_t fat_disk;

	(void)boot_disk;
	ofw_init(client);
	ofw_puts("SPARCV9 STAGE2\n");
	if (ofw_bootpath(bootpath, sizeof(bootpath)) <= 0)
		goto failed;
	if (make_fat_path(path, sizeof(path)) != 0)
		goto failed;
	fat_disk = ofw_open(path);
	if (fat_disk <= 0)
		goto failed;
	if (fat_mount(&volume, (ofw_cell_t)fat_disk) != 0 ||
	    fat_find_kernel(&volume, &file) != 0 ||
	    file.size > sizeof(kernel_image) ||
	    fat_read_file(&file, 0, kernel_image, file.size) != 0 ||
	    sparcv9_handoff_build(&handoff_image, bootpath) != 0) {
		(void)ofw_close((ofw_cell_t)fat_disk);
		goto failed;
	}
	(void)ofw_close((ofw_cell_t)fat_disk);
	if (load_kernel(kernel_image, file.size, &handoff_image, &entry) != 0)
		goto failed;
	((kernel_entry_t)entry)(client, ofw_stdout_handle(),
	    (void *)KERNEL_HANDOFF_VA);
failed:
	ofw_puts("SPARCV9 STAGE2 FAIL\n");
	ofw_exit();
}
