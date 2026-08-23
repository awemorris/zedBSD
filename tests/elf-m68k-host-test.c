#include "kern/elf.h"
#include "kern/exec.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/vmspace.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_CAPACITY 8192U
#define LOAD_OFFSET 0x1000U
#define LOAD_ADDRESS 0x00400000U

static uint8_t image[IMAGE_CAPACITY];
static size_t image_size;
static struct inode image_inode;
static struct file image_file;
static struct vm_region mapped_region;
static uint8_t mapped_bytes[4096];
static unsigned map_count;
struct vm_layout vm_layout;

void
vmspace_layout_init(void)
{
	vm_layout.user_minimum = 4096U;
	vm_layout.user_limit = 0x80000000U;
}

void *kern_malloc(size_t size) { return malloc(size); }
void *kern_calloc(size_t count, size_t size) { return calloc(count, size); }
void kern_free(void *pointer) { free(pointer); }

off_t
file_seek(struct file *file, off_t offset, int whence)
{
	off_t base = whence == 0 ? 0 : whence == 1 ? file->f_offset :
	    whence == 2 ? file->f_inode->i_size : -1;
	if (base < 0 || offset < -base || base + offset < 0)
		return -EINVAL;
	file->f_offset = base + offset;
	return file->f_offset;
}

ssize_t
file_read(struct file *file, void *buffer, size_t length)
{
	size_t offset = (size_t)file->f_offset;
	if (offset >= image_size)
		return 0;
	if (length > image_size - offset)
		length = image_size - offset;
	memcpy(buffer, image + offset, length);
	file->f_offset += (off_t)length;
	return (ssize_t)length;
}

int
file_content_lease_begin(struct file *file, struct file_content_lease *lease)
{
	memset(lease, 0, sizeof(*lease));
	lease->file = file;
	lease->size = (off_t)image_size;
	lease->active = 1;
	return 0;
}

ssize_t
file_content_lease_pread(struct file_content_lease *lease, void *buffer,
	size_t length, off_t offset)
{
	(void)lease;
	if (offset < 0 || (size_t)offset >= image_size)
		return 0;
	if (length > image_size - (size_t)offset)
		length = image_size - (size_t)offset;
	memcpy(buffer, image + offset, length);
	return (ssize_t)length;
}

void
file_content_lease_end(struct file_content_lease *lease)
{
	memset(lease, 0, sizeof(*lease));
}

int
vmspace_map_anon_fixed_noreplace(struct vmspace *vm, uintptr_t start,
	size_t size, uint32_t prot, struct vm_region **result)
{
	(void)vm;
	if (map_count != 0)
		return EINVAL;
	memset(&mapped_region, 0, sizeof(mapped_region));
	mapped_region.start = start;
	mapped_region.size = size;
	mapped_region.prot = prot;
	mapped_region.backing = VM_BACKING_ANON;
	memset(mapped_bytes, 0, sizeof(mapped_bytes));
	map_count++;
	if (result != NULL)
		*result = &mapped_region;
	return 0;
}

int
vmspace_copy_to(struct vmspace *vm, uintptr_t destination,
	const void *source, size_t size)
{
	(void)vm;
	if (destination < mapped_region.start ||
	    destination - mapped_region.start > mapped_region.size ||
	    size > mapped_region.size - (destination - mapped_region.start))
		return EFAULT;
	memcpy(mapped_bytes + destination - mapped_region.start, source, size);
	return 0;
}

int
vmspace_protect(struct vmspace *vm, uintptr_t start, size_t size,
	uint32_t prot)
{
	(void)vm;
	if (mapped_region.start != start || mapped_region.size != size)
		return EINVAL;
	mapped_region.prot = prot;
	return 0;
}

int
vmspace_find_free_range(struct vmspace *vm, uintptr_t hint, size_t size,
	size_t alignment, uintptr_t *result)
{
	(void)vm;
	(void)size;
	if (result == NULL || alignment == 0)
		return EINVAL;
	*result = (hint + alignment - 1U) & ~(uintptr_t)(alignment - 1U);
	return 0;
}

int
vmspace_find_free_range_bounded(struct vmspace *vm, uintptr_t minimum,
	uintptr_t maximum, size_t size, size_t alignment, uintptr_t *result)
{
	int error = vmspace_find_free_range(vm, minimum, size, alignment, result);
	if (error == 0 && (*result >= maximum || size > maximum - *result))
		return ENOMEM;
	return error;
}

int
vmspace_unmap(struct vmspace *vm, uintptr_t start, size_t size)
{
	(void)vm;
	(void)start;
	(void)size;
	if (map_count != 0)
		map_count--;
	return 0;
}

static void
be16(size_t offset, uint16_t value)
{
	image[offset] = (uint8_t)(value >> 8);
	image[offset + 1] = (uint8_t)value;
}

static void
be32(size_t offset, uint32_t value)
{
	image[offset] = (uint8_t)(value >> 24);
	image[offset + 1] = (uint8_t)(value >> 16);
	image[offset + 2] = (uint8_t)(value >> 8);
	image[offset + 3] = (uint8_t)value;
}

static void
make_valid_image(void)
{
	const size_t ph = sizeof(struct elf32_ehdr);

	memset(image, 0, sizeof(image));
	image[EI_MAG0] = ELFMAG0;
	image[EI_MAG1] = ELFMAG1;
	image[EI_MAG2] = ELFMAG2;
	image[EI_MAG3] = ELFMAG3;
	image[EI_CLASS] = ELFCLASS32;
	image[EI_DATA] = ELFDATA2MSB;
	image[EI_VERSION] = EV_CURRENT;
	be16(16, ET_EXEC);
	be16(18, EM_68K);
	be32(20, EV_CURRENT);
	be32(24, LOAD_ADDRESS);
	be32(28, (uint32_t)ph);
	be16(40, sizeof(struct elf32_ehdr));
	be16(42, sizeof(struct elf32_phdr));
	be16(44, 1);
	be32(ph + 0, PT_LOAD);
	be32(ph + 4, LOAD_OFFSET);
	be32(ph + 8, LOAD_ADDRESS);
	be32(ph + 12, LOAD_ADDRESS);
	be32(ph + 16, 4);
	be32(ph + 20, 8);
	be32(ph + 24, PF_R | PF_X);
	be32(ph + 28, 4096);
	memcpy(image + LOAD_OFFSET, "68K!", 4);
	image_size = LOAD_OFFSET + 4;
	memset(&image_inode, 0, sizeof(image_inode));
	image_inode.i_size = (off_t)image_size;
	memset(&image_file, 0, sizeof(image_file));
	image_file.f_inode = &image_inode;
	map_count = 0;
}

static int
load(struct vmspace *vm, struct elf32_image_info *info)
{
	image_file.f_offset = 0;
	return elf32_load(&image_file, vm, info);
}

int
main(void)
{
	struct vmspace vm;
	struct elf32_image_info info;

	memset(&vm, 0, sizeof(vm));
	make_valid_image();
	assert(load(&vm, &info) == 0);
	assert(info.entry == LOAD_ADDRESS && vm.entry == LOAD_ADDRESS);
	assert(mapped_region.start == LOAD_ADDRESS && mapped_region.size == 4096);
	assert(mapped_region.prot == (HAL_SPACE_READ | HAL_SPACE_EXEC));
	assert(mapped_region.backing == VM_BACKING_ANON);
	assert(memcmp(mapped_bytes, "68K!", 4) == 0);
	assert(mapped_bytes[4] == 0);

	make_valid_image();
	image[EI_DATA] = ELFDATA2LSB;
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	make_valid_image();
	be16(18, EM_386);
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	make_valid_image();
	be32(28, (uint32_t)image_size - 8U);
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	make_valid_image();
	image_size = sizeof(struct elf32_ehdr) - 1U;
	image_inode.i_size = (off_t)image_size;
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	puts("zedBSD m68k big-endian ELF32 loader host tests: PASS");
	return 0;
}
