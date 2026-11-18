/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 second-stage FAT16/ELF64 loader.
 *
 * It reads VMUNIX.S9 from the FAT16 partition of the boot disk, builds the
 * handoff from the device tree, loads the kernel into its 12 MiB window and
 * enters it.
 */

#include "ofw.h"
#include "handoff.h"

/* The disk's sector size. */
#define SECTOR_SIZE 512U

/* The kernel's physical window and where it is mapped in the direct map. */
#define KERNEL_PHYS_BASE 0x00400000ULL
#define KERNEL_WINDOW_SIZE 0x00c00000UL
#define KERNEL_DIRECT_BASE 0xfffff80000000000ULL
#define KERNEL_WINDOW_VA (KERNEL_DIRECT_BASE + KERNEL_PHYS_BASE)

/* Where the handoff is copied for the kernel: the window's last 8 KiB. */
#define KERNEL_HANDOFF_VA (KERNEL_DIRECT_BASE + 0x00ffe000ULL)

/* The ELF64 layout and the program header values the loader checks. */
#define ELF_HEADER_SIZE 64U
#define ELF_PROGRAM_HEADER_SIZE 56U
#define ELF_PROGRAM_HEADER_MAX 16U
#define ELF_PT_LOAD 1U
#define ELF_PF_X 1U

/* The first FAT16 entry value that ends a cluster chain. */
#define FAT_CLUSTER_END 0xfff8U

/* The largest kernel file the loader reads. */
#define KERNEL_FILE_LIMIT (2U * 1024U * 1024U)

/* Marks a parameter that this function does not use. */
#define UNUSED_PARAMETER(parameter) ((void)(parameter))

/* The FAT16 volume the kernel is read from, as its boot sector describes it. */
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

/* A file of the volume's root directory: its first cluster and its size. */
struct fat_file {
	const struct fat_volume *volume;
	unsigned int first_cluster;
	unsigned long size;
};

/* One checked PT_LOAD segment of the kernel, kept until it is copied into place. */
struct load_segment {
	unsigned long long start;
	unsigned long long end;
	unsigned long long offset;
	unsigned long long file_size;
	unsigned long long memory_size;
	unsigned int flags;
};

/* The kernel's entry: the client interface, the console handle and the handoff. */
typedef void (*kernel_entry_t)(ofw_client_t, ofw_cell_t, void *);

/* The buffer every FAT sector is read through. */
static unsigned char sector_buffer[SECTOR_SIZE];

/* The whole kernel file, read before it is checked and copied into its window. */
static unsigned char kernel_image[KERNEL_FILE_LIMIT];

/* The handoff built from the device tree before it is copied for the kernel. */
static struct sun4u_boot_handoff handoff_image;

/* Turns on the MMU's instruction and data translation (in stage2-entry.S). */
void sparcv9_loader_enable_mmu(void);

static unsigned int get16le(const unsigned char *bytes);
static unsigned long get32le(const unsigned char *bytes);
static unsigned int get16be(const unsigned char *bytes);
static unsigned long get32be(const unsigned char *bytes);
static unsigned long long get64be(const unsigned char *bytes);
static void copy_bytes(void *destination, const void *source, unsigned long size);
static void zero_bytes(void *destination, unsigned long size);
static int read_exact(ofw_cell_t disk, unsigned long long offset, void *buffer, unsigned long size);
static int read_sector(const struct fat_volume *volume, unsigned int sector, unsigned char *buffer);
static int fat_mount(struct fat_volume *volume, ofw_cell_t disk);
static int fat_entry_is_kernel(const unsigned char *entry);
static int fat_find_kernel(const struct fat_volume *volume, struct fat_file *file);
static int fat_next_cluster(const struct fat_volume *volume, unsigned int cluster, unsigned int *next);
static int fat_follow_chain(const struct fat_volume *volume, unsigned int *cluster, unsigned long *steps);
static int fat_read_file(const struct fat_file *file, unsigned long offset, void *destination, unsigned long size);
static int make_fat_path(char *path, unsigned long capacity);
static int power_of_two(unsigned long long value);
static void flush_instruction_cache(unsigned long long start, unsigned long long end);
static int elf_header_acceptable(const unsigned char *header);
static int segment_acceptable(const unsigned char *program, unsigned long image_size);
static int load_kernel(const unsigned char *image, unsigned long image_size, const struct sun4u_boot_handoff *handoff, unsigned long long *entry_out);
static int read_kernel(struct fat_volume *volume, struct fat_file *file, ofw_cell_t fat_disk, const char *bootpath);
static int stage2_boot(ofw_client_t client);

/*
 * Loads the kernel and enters it.
 *
 * Any failure, or a kernel that returns, is reported and the loader exits
 * to the firmware.
 */
void
sparcv9_stage2_main(
	ofw_client_t client,
	ofw_cell_t boot_disk)
{
	UNUSED_PARAMETER(boot_disk);

	/* Loads and enters the kernel; coming back means it failed. */
	(void)stage2_boot(client);

	/* Reports the failure and exits to the firmware. */
	ofw_puts("SPARCV9 STAGE2 FAIL\n");
	ofw_exit();
}

/* Reads a little-endian 16-bit field. */
static unsigned int
get16le(
	const unsigned char *bytes)
{
	/* Combines the low byte and the high byte. */
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8;
}

/* Reads a little-endian 32-bit field. */
static unsigned long
get32le(
	const unsigned char *bytes)
{
	unsigned long low;
	unsigned long high;

	/* Reads the two halves. */
	low = get16le(bytes);
	high = get16le(bytes + 2);

	/* Reports the combined value. */
	return low | high << 16;
}

/* Reads a big-endian 16-bit field. */
static unsigned int
get16be(
	const unsigned char *bytes)
{
	/* Combines the high byte and the low byte. */
	return (unsigned int)bytes[0] << 8 | bytes[1];
}

/* Reads a big-endian 32-bit field. */
static unsigned long
get32be(
	const unsigned char *bytes)
{
	unsigned long high;
	unsigned long low;

	/* Reads the two halves. */
	high = get16be(bytes);
	low = get16be(bytes + 2);

	/* Reports the combined value. */
	return high << 16 | low;
}

/* Reads a big-endian 64-bit field. */
static unsigned long long
get64be(
	const unsigned char *bytes)
{
	unsigned long long high;
	unsigned long long low;

	/* Reads the two halves. */
	high = get32be(bytes);
	low = get32be(bytes + 4);

	/* Reports the combined value. */
	return high << 32 | low;
}

/* Copies bytes without the C library. */
static void
copy_bytes(
	void *destination,
	const void *source,
	unsigned long size)
{
	unsigned char *out;
	const unsigned char *in;

	/* Copies the bytes one at a time, front to back. */
	out = destination;
	in = source;
	while (size != 0) {
		*out = *in;
		out++;
		in++;
		size--;
	}
}

/* Clears bytes without the C library. */
static void
zero_bytes(
	void *destination,
	unsigned long size)
{
	unsigned char *out;

	/* Clears the bytes one at a time. */
	out = destination;
	while (size != 0) {
		*out = 0;
		out++;
		size--;
	}
}

/* Reads exactly size bytes of the disk from a byte offset. */
static int
read_exact(
	ofw_cell_t disk,
	unsigned long long offset,
	void *buffer,
	unsigned long size)
{
	unsigned char *bytes;
	long count;
	int status;

	/* Moves to the offset. */
	bytes = buffer;
	status = ofw_seek(disk, offset);
	if (status != 0)
		return -1;

	/* Reads until every byte is in, refusing a short or a long read. */
	while (size != 0) {
		count = ofw_read(disk, bytes, size);
		if (count <= 0 || (unsigned long)count > size)
			return -1;

		bytes += count;
		size -= (unsigned long)count;
	}

	/* Succeeded: the buffer holds the bytes. */
	return 0;
}

/* Reads one sector of the volume, refusing one past its end. */
static int
read_sector(
	const struct fat_volume *volume,
	unsigned int sector,
	unsigned char *buffer)
{
	int status;

	/* Refuses a sector outside the volume. */
	if (sector >= volume->total_sectors)
		return -1;

	/* Reads the sector. */
	status = read_exact(volume->disk, (unsigned long long)sector * SECTOR_SIZE, buffer, SECTOR_SIZE);
	if (status != 0)
		return -1;

	/* Succeeded: the buffer holds the sector. */
	return 0;
}

/* Reads the boot sector and describes the FAT16 volume. */
static int
fat_mount(
	struct fat_volume *volume,
	ofw_cell_t disk)
{
	unsigned int reserved;
	unsigned int fats;
	unsigned int root_entries;
	unsigned int total;
	unsigned int bytes_per_sector;
	int status;

	/* Reads the boot sector before the volume's size is known. */
	volume->disk = disk;
	volume->total_sectors = ~0U;
	status = read_sector(volume, 0, sector_buffer);
	if (status != 0)
		return -1;

	/* Refuses a sector size other than the disk's. */
	bytes_per_sector = get16le(sector_buffer + 11);
	if (bytes_per_sector != SECTOR_SIZE)
		return -1;

	/* Reads the BPB's geometry; a zero 16-bit total gives way to the 32-bit one. */
	volume->sectors_per_cluster = sector_buffer[13];
	reserved = get16le(sector_buffer + 14);
	fats = sector_buffer[16];
	root_entries = get16le(sector_buffer + 17);
	total = get16le(sector_buffer + 19);
	if (total == 0)
		total = (unsigned int)get32le(sector_buffer + 32);

	/* Refuses impossible cluster, reserved, FAT, root and total counts. */
	volume->sectors_per_fat = get16le(sector_buffer + 22);
	if (volume->sectors_per_cluster == 0 ||
	    (volume->sectors_per_cluster &
	     (volume->sectors_per_cluster - 1U)) != 0 ||
	    reserved == 0 ||
	    fats == 0 ||
	    root_entries == 0 ||
	    volume->sectors_per_fat == 0 ||
	    total == 0)
		return -1;

	/* Works out where the FAT, the root directory and the data are. */
	volume->total_sectors = total;
	volume->fat_start = reserved;
	volume->root_start = reserved + fats * volume->sectors_per_fat;
	volume->root_sectors = (root_entries * 32U + SECTOR_SIZE - 1U) / SECTOR_SIZE;
	volume->data_start = volume->root_start + volume->root_sectors;

	/* Refuses a volume with no room for data. */
	if (volume->data_start >= total)
		return -1;

	/* Succeeded: the volume is described. */
	return 0;
}

/* Reports whether a root directory entry is the file VMUNIX.S9. */
static int
fat_entry_is_kernel(
	const unsigned char *entry)
{
	/* The kernel's 8.3 name. */
	static const unsigned char name[11] = {
		'V', 'M', 'U', 'N', 'I', 'X', ' ', ' ', 'S', '9', ' '
	};
	unsigned int index;

	/* A deleted entry, a long-name entry, a directory and a volume label are not the file. */
	if (entry[0] == 0xe5 ||
	    entry[11] == 0x0f ||
	    (entry[11] & 0x18U) != 0)
		return 0;

	/* Compares the name. */
	for (index = 0; index < sizeof(name); index++) {
		if (entry[index] != name[index])
			return 0;
	}

	/* Succeeded: the entry is the kernel. */
	return 1;
}

/* Finds VMUNIX.S9 in the root directory. */
static int
fat_find_kernel(
	const struct fat_volume *volume,
	struct fat_file *file)
{
	const unsigned char *entry;
	unsigned int sector;
	unsigned int offset;
	int status;
	int is_kernel;

	/* Reads each sector of the root directory. */
	for (sector = 0; sector < volume->root_sectors; sector++) {
		status = read_sector(volume, volume->root_start + sector, sector_buffer);
		if (status != 0)
			return -1;

		/* Examines each entry of the sector. */
		for (offset = 0; offset < SECTOR_SIZE; offset += 32U) {
			/* The end of the directory: there is no kernel. */
			entry = sector_buffer + offset;
			if (entry[0] == 0)
				return -1;

			/* Passes over any other entry. */
			is_kernel = fat_entry_is_kernel(entry);
			if (!is_kernel)
				continue;

			/* Records the file and refuses one without data or too short for an ELF header. */
			file->volume = volume;
			file->first_cluster = get16le(entry + 26);
			file->size = get32le(entry + 28);
			if (file->first_cluster < 2 || file->size < ELF_HEADER_SIZE)
				return -1;

			/* Succeeded: the file is found. */
			return 0;
		}
	}

	/* Reports that the root directory has no kernel. */
	return -1;
}

/* Reads the FAT entry that follows a cluster. */
static int
fat_next_cluster(
	const struct fat_volume *volume,
	unsigned int cluster,
	unsigned int *next)
{
	unsigned long offset;
	unsigned int sector;
	unsigned int within;
	int status;

	/* Finds the FAT sector and the entry's place in it. */
	offset = (unsigned long)cluster * 2U;
	sector = volume->fat_start + (unsigned int)(offset / SECTOR_SIZE);
	within = (unsigned int)(offset % SECTOR_SIZE);

	/* Reads the sector. */
	status = read_sector(volume, sector, sector_buffer);
	if (status != 0)
		return -1;

	/* Decodes the entry. */
	*next = get16le(sector_buffer + within);

	/* Succeeded: the next cluster is read. */
	return 0;
}

/* Moves to the next cluster of a chain, refusing an end, a bad entry and a loop. */
static int
fat_follow_chain(
	const struct fat_volume *volume,
	unsigned int *cluster,
	unsigned long *steps)
{
	int status;

	/* Refuses an unreadable FAT, a free or reserved entry and the end of the chain. */
	status = fat_next_cluster(volume, *cluster, cluster);
	if (status != 0 ||
	    *cluster < 2 ||
	    *cluster >= FAT_CLUSTER_END)
		return -1;

	/* Refuses a chain longer than the volume, which can only be a loop. */
	(*steps)++;
	if (*steps > volume->total_sectors)
		return -1;

	/* Succeeded: the cluster is the next one. */
	return 0;
}

/* Reads a byte range of a file through its cluster chain. */
static int
fat_read_file(
	const struct fat_file *file,
	unsigned long offset,
	void *destination,
	unsigned long size)
{
	const struct fat_volume *volume;
	unsigned long cluster_size;
	unsigned int cluster;
	unsigned long skip;
	unsigned long steps;
	unsigned char *out;
	unsigned long available;
	unsigned long amount;
	unsigned int cluster_sector;
	unsigned int within;
	unsigned long chunk;
	unsigned int data_sector;
	int status;

	/* Starts at the file's first cluster. */
	volume = file->volume;
	cluster_size = (unsigned long)volume->sectors_per_cluster * SECTOR_SIZE;
	cluster = file->first_cluster;
	steps = 0;
	out = destination;

	/* Refuses a range outside the file. */
	if (offset > file->size || size > file->size - offset)
		return -1;

	/* Follows the chain to the cluster that holds the offset. */
	for (skip = offset; skip >= cluster_size; skip -= cluster_size) {
		status = fat_follow_chain(volume, &cluster, &steps);
		if (status != 0)
			return -1;
	}

	/* Reads cluster after cluster until the range is in. */
	while (size != 0) {
		/* Reads at most the rest of this cluster. */
		available = cluster_size - skip;
		amount = available;
		if (size < available)
			amount = size;

		/* Reads the cluster's part sector by sector. */
		while (amount != 0) {
			/* Finds the sector and the part of it the range covers. */
			cluster_sector = (unsigned int)(skip / SECTOR_SIZE);
			within = (unsigned int)(skip % SECTOR_SIZE);
			chunk = SECTOR_SIZE - within;
			data_sector = volume->data_start + (cluster - 2U) * volume->sectors_per_cluster + cluster_sector;
			if (chunk > amount)
				chunk = amount;

			/* Reads the sector and copies the part. */
			status = read_sector(volume, data_sector, sector_buffer);
			if (status != 0)
				return -1;

			copy_bytes(out, sector_buffer + within, chunk);
			out += chunk;
			skip += chunk;
			amount -= chunk;
			size -= chunk;
		}

		/* The range is in. */
		if (size == 0)
			break;

		/* Moves to the next cluster from its start. */
		status = fat_follow_chain(volume, &cluster, &steps);
		if (status != 0)
			return -1;

		skip = 0;
	}

	/* Succeeded: the destination holds the range. */
	return 0;
}

/* Builds the path of the boot disk's FAT partition, slice b of the boot path. */
static int
make_fat_path(
	char *path,
	unsigned long capacity)
{
	int length;
	int colon;
	int index;

	/* Reads the boot path. */
	colon = -1;
	length = ofw_bootpath(path, capacity);
	if (length <= 0)
		return -1;

	/* Finds the last colon, which starts the slice. */
	for (index = 0; index < length; index++) {
		if (path[index] == ':')
			colon = index;
	}

	/* Replaces the slice with b, or appends :b when the path names none. */
	if (colon >= 0) {
		/* Refuses a path whose slice cannot be replaced in the buffer. */
		if ((unsigned long)colon + 3U > capacity)
			return -1;

		path[colon + 1] = 'b';
		path[colon + 2] = '\0';
	} else {
		/* Refuses a path with no room for :b. */
		if ((unsigned long)length + 3U > capacity)
			return -1;

		path[length] = ':';
		length++;
		path[length] = 'b';
		length++;
		path[length] = '\0';
	}

	/* Succeeded: the path names the FAT partition. */
	return 0;
}

/* Reports whether a value is a power of two. */
static int
power_of_two(
	unsigned long long value)
{
	/* Zero is not a power of two. */
	if (value == 0)
		return 0;

	/* A value with more than one bit set is not one either. */
	if ((value & (value - 1ULL)) != 0)
		return 0;

	/* Succeeded: the value is a power of two. */
	return 1;
}

/* Flushes the instruction cache over a range of copied code. */
static void
flush_instruction_cache(
	unsigned long long start,
	unsigned long long end)
{
	/* Flushes each 32-byte line. */
	start &= ~31ULL;
	while (start < end) {
		__asm__ volatile("flush %0" : : "r"((void *)start) : "memory");
		start += 32U;
	}

	/* Waits for the flushes to complete. */
	__asm__ volatile("membar #Sync" : : : "memory");
}

/* Reports whether an ELF header is a big-endian SPARC V9 executable's. */
static int
elf_header_acceptable(
	const unsigned char *header)
{
	unsigned int type;
	unsigned int machine;
	unsigned long version;
	unsigned int header_size;
	unsigned int program_header_size;

	/* The file is not a big-endian ELF64 file of the current version. */
	if (header[0] != 0x7f ||
	    header[1] != 'E' ||
	    header[2] != 'L' ||
	    header[3] != 'F' ||
	    header[4] != 2 ||
	    header[5] != 2 ||
	    header[6] != 1)
		return 0;

	/* The file is not an executable. */
	type = get16be(header + 16);
	if (type != 2)
		return 0;

	/* The executable is not for SPARC V9. */
	machine = get16be(header + 18);
	if (machine != 43)
		return 0;

	/* The executable is not of the current version. */
	version = get32be(header + 20);
	if (version != 1)
		return 0;

	/* The header is of another size. */
	header_size = get16be(header + 52);
	if (header_size != ELF_HEADER_SIZE)
		return 0;

	/* The program headers are of another size. */
	program_header_size = get16be(header + 54);
	if (program_header_size != ELF_PROGRAM_HEADER_SIZE)
		return 0;

	/* Succeeded: the header can be loaded from. */
	return 1;
}

/* Reports whether a PT_LOAD segment fits the kernel window and the file. */
static int
segment_acceptable(
	const unsigned char *program,
	unsigned long image_size)
{
	unsigned long long offset;
	unsigned long long virtual_address;
	unsigned long long physical_address;
	unsigned long long file_size;
	unsigned long long memory_size;
	unsigned long long alignment;
	int aligned;

	/* Reads the segment's fields. */
	offset = get64be(program + 8);
	virtual_address = get64be(program + 16);
	physical_address = get64be(program + 24);
	file_size = get64be(program + 32);
	memory_size = get64be(program + 40);
	alignment = get64be(program + 48);

	/* The segment is empty, or its file part is larger than it. */
	if (memory_size == 0 || file_size > memory_size)
		return 0;

	/* The segment lies outside the kernel's physical window. */
	if (physical_address < KERNEL_PHYS_BASE ||
	    memory_size > KERNEL_WINDOW_SIZE ||
	    physical_address - KERNEL_PHYS_BASE > KERNEL_WINDOW_SIZE - memory_size)
		return 0;

	/* The segment's virtual address is not its physical one in the direct map. */
	if (virtual_address != KERNEL_DIRECT_BASE + physical_address)
		return 0;

	/* The segment's file part lies outside the file. */
	if (offset > image_size || file_size > image_size - offset)
		return 0;

	/* The alignment is not a power of two. */
	aligned = power_of_two(alignment);
	if (!aligned)
		return 0;

	/* The alignment is larger than the window, or the address and the offset disagree within it. */
	if (alignment > KERNEL_WINDOW_SIZE ||
	    (virtual_address & (alignment - 1ULL)) != (offset & (alignment - 1ULL)))
		return 0;

	/* Succeeded: the segment can be loaded. */
	return 1;
}

/* Checks the kernel, copies its segments into the window and the handoff after them. */
static int
load_kernel(
	const unsigned char *image,
	unsigned long image_size,
	const struct sun4u_boot_handoff *handoff,
	unsigned long long *entry_out)
{
	const unsigned char *header;
	const unsigned char *program;
	const struct load_segment *segment;
	struct load_segment segments[ELF_PROGRAM_HEADER_MAX];
	unsigned long long entry;
	unsigned long long phoff;
	unsigned long long virtual_address;
	unsigned long long memory_size;
	unsigned long type;
	unsigned int phnum;
	unsigned int loaded;
	unsigned int index;
	unsigned int flags;
	unsigned int other;
	void *window;
	int entry_is_executable;
	int acceptable;

	/* Nothing is loaded yet. */
	header = image;
	loaded = 0;
	entry_is_executable = 0;

	/* Refuses a file shorter than the ELF header. */
	if (image_size < ELF_HEADER_SIZE)
		return -1;

	/* Refuses a file that is not a SPARC V9 executable. */
	ofw_puts("SPARCV9 ELF HEADER\n");
	acceptable = elf_header_acceptable(header);
	if (!acceptable)
		return -1;

	/* Reads the entry point and where the program headers are. */
	entry = get64be(header + 24);
	phoff = get64be(header + 32);
	phnum = get16be(header + 56);

	/* Refuses program headers that are not all within the file. */
	if (phnum == 0 ||
	    phnum > ELF_PROGRAM_HEADER_MAX ||
	    phoff > image_size ||
	    (unsigned long long)phnum * ELF_PROGRAM_HEADER_SIZE > image_size - phoff)
		return -1;

	/* Takes each PT_LOAD segment. */
	for (index = 0; index < phnum; index++) {
		/* Only PT_LOAD segments are loaded. */
		program = image + phoff + (unsigned long long)index * ELF_PROGRAM_HEADER_SIZE;
		type = get32be(program);
		if (type != ELF_PT_LOAD)
			continue;

		/* Refuses a segment that does not fit the window or the file. */
		acceptable = segment_acceptable(program, image_size);
		if (!acceptable)
			return -1;

		/* Refuses a segment that overlaps one taken before it. */
		flags = (unsigned int)get32be(program + 4);
		virtual_address = get64be(program + 16);
		memory_size = get64be(program + 40);
		for (other = 0; other < loaded; other++) {
			if (virtual_address < segments[other].end &&
			    virtual_address + memory_size > segments[other].start)
				return -1;
		}

		/* Records the segment. */
		segments[loaded].start = virtual_address;
		segments[loaded].end = virtual_address + memory_size;
		segments[loaded].offset = get64be(program + 8);
		segments[loaded].file_size = get64be(program + 32);
		segments[loaded].memory_size = memory_size;
		segments[loaded].flags = flags;
		loaded++;

		/* The entry point counts only when it lies in an executable segment. */
		if ((flags & ELF_PF_X) != 0) {
			if (entry >= virtual_address &&
			    entry < virtual_address + memory_size)
				entry_is_executable = 1;
		}
	}

	/* Refuses a kernel with nothing to load or entered outside its code. */
	if (loaded == 0 || !entry_is_executable)
		return -1;

	/* Claims the kernel's window at its direct-map address. */
	window = ofw_claim_fixed((void *)KERNEL_WINDOW_VA, KERNEL_PHYS_BASE, KERNEL_WINDOW_SIZE);
	if (window != (void *)KERNEL_WINDOW_VA)
		return -1;

	/* Turns on the MMU, so the window is reached through its mapping. */
	ofw_puts("SPARCV9 ELF PASS\n");
	sparcv9_loader_enable_mmu();

	/* Copies each segment, clears its tail and flushes the instruction cache over its code. */
	for (index = 0; index < loaded; index++) {
		/* Copies the file part. */
		segment = &segments[index];
		if (segment->file_size != 0) {
			copy_bytes((void *)segment->start,
				   image + segment->offset,
				   (unsigned long)segment->file_size);
		}

		/* Clears the rest. */
		zero_bytes((void *)(segment->start + segment->file_size),
			   (unsigned long)(segment->memory_size - segment->file_size));

		/* Makes copied code visible to instruction fetch. */
		if ((segment->flags & ELF_PF_X) != 0)
			flush_instruction_cache(segment->start, segment->end);
	}

	/* Places the handoff in the window's last 8 KiB. */
	zero_bytes((void *)KERNEL_HANDOFF_VA, 8192U);
	copy_bytes((void *)KERNEL_HANDOFF_VA, handoff, sizeof(*handoff));

	/* Publishes the entry point. */
	*entry_out = entry;

	/* Succeeded: the kernel is in place. */
	return 0;
}

/* Reads VMUNIX.S9 from the FAT partition and builds the handoff. */
static int
read_kernel(
	struct fat_volume *volume,
	struct fat_file *file,
	ofw_cell_t fat_disk,
	const char *bootpath)
{
	int status;

	/* Describes the FAT volume. */
	status = fat_mount(volume, fat_disk);
	if (status != 0)
		return -1;

	/* Finds the kernel file. */
	status = fat_find_kernel(volume, file);
	if (status != 0)
		return -1;

	/* Refuses a kernel larger than the buffer. */
	if (file->size > sizeof(kernel_image))
		return -1;

	/* Reads the whole kernel. */
	status = fat_read_file(file, 0, kernel_image, file->size);
	if (status != 0)
		return -1;

	/* Builds the handoff from the device tree. */
	status = sparcv9_handoff_build(&handoff_image, bootpath);
	if (status != 0)
		return -1;

	/* Succeeded: the kernel is read and the handoff built. */
	return 0;
}

/* Loads the kernel and enters it; returning means a failure. */
static int
stage2_boot(
	ofw_client_t client)
{
	struct fat_volume volume;
	struct fat_file file;
	unsigned long long entry;
	char path[256];
	char bootpath[256];
	ofw_scell_t fat_disk;
	kernel_entry_t kernel_entry;
	int length;
	int status;

	/* No kernel has been read yet. */
	file.size = 0;

	/* Starts the client interface and says the second stage runs. */
	ofw_init(client);
	ofw_puts("SPARCV9 STAGE2\n");

	/* Reads the boot path for the handoff. */
	length = ofw_bootpath(bootpath, sizeof(bootpath));
	if (length <= 0)
		return -1;

	/* Builds the path of the FAT partition. */
	status = make_fat_path(path, sizeof(path));
	if (status != 0)
		return -1;

	/* Opens the FAT partition. */
	fat_disk = ofw_open(path);
	if (fat_disk <= 0)
		return -1;

	/* Reads the kernel and builds the handoff, then closes the partition either way. */
	status = read_kernel(&volume, &file, (ofw_cell_t)fat_disk, bootpath);
	(void)ofw_close((ofw_cell_t)fat_disk);
	if (status != 0)
		return -1;

	/* Places the kernel in its window. */
	status = load_kernel(kernel_image, file.size, &handoff_image, &entry);
	if (status != 0)
		return -1;

	/* Enters the kernel with the client interface, the console and the handoff. */
	kernel_entry = (kernel_entry_t)entry;
	kernel_entry(client, ofw_stdout_handle(), (void *)KERNEL_HANDOFF_VA);

	/* Reports a kernel that came back as a failure. */
	return -1;
}
