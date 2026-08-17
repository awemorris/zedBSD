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
	off_t base;
	if (whence == 0)
		base = 0;
	else if (whence == 1)
		base = file->f_offset;
	else if (whence == 2)
		base = file->f_inode->i_size;
	else
		return -EINVAL;
	if (offset < -base || base + offset < 0)
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
vmspace_map_file(struct vmspace *vm, uintptr_t start, size_t size,
		 uint32_t prot, struct file *file, off_t file_offset,
		 uintptr_t data_start, size_t data_size,
		 struct vm_region **result)
{
	(void)vm;
	if (map_count != 0)
		return EINVAL;
	memset(&mapped_region, 0, sizeof(mapped_region));
	mapped_region.start = start;
	mapped_region.size = size;
	mapped_region.prot = prot;
	mapped_region.backing = VM_BACKING_FILE;
	mapped_region.file = file;
	mapped_region.file_offset = file_offset;
	mapped_region.data_start = data_start;
	mapped_region.data_size = data_size;
	map_count++;
	if (result != NULL)
		*result = &mapped_region;
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
reset_mapping(void)
{
	memset(&mapped_region, 0, sizeof(mapped_region));
	map_count = 0;
}

static struct elf32_ehdr *
make_valid_image(void)
{
	struct elf32_ehdr *header;
	struct elf32_phdr *program;
	memset(image, 0, sizeof(image));
	header = (struct elf32_ehdr *)image;
	header->e_ident[EI_MAG0] = ELFMAG0;
	header->e_ident[EI_MAG1] = ELFMAG1;
	header->e_ident[EI_MAG2] = ELFMAG2;
	header->e_ident[EI_MAG3] = ELFMAG3;
	header->e_ident[EI_CLASS] = ELFCLASS32;
	header->e_ident[EI_DATA] = ELFDATA2LSB;
	header->e_ident[EI_VERSION] = EV_CURRENT;
	header->e_type = ET_EXEC;
	header->e_machine = EM_386;
	header->e_version = EV_CURRENT;
	header->e_entry = LOAD_ADDRESS;
	header->e_phoff = sizeof(*header);
	header->e_ehsize = sizeof(*header);
	header->e_phentsize = sizeof(*program);
	header->e_phnum = 1;
	program = (struct elf32_phdr *)(image + header->e_phoff);
	program->p_type = PT_LOAD;
	program->p_offset = LOAD_OFFSET;
	program->p_vaddr = LOAD_ADDRESS;
	program->p_paddr = LOAD_ADDRESS;
	program->p_filesz = 4;
	program->p_memsz = 8;
	program->p_flags = PF_R | PF_X;
	program->p_align = 4096;
	memcpy(image + LOAD_OFFSET, "ELF!", 4);
	image_size = LOAD_OFFSET + 4;
	memset(&image_inode, 0, sizeof(image_inode));
	image_inode.i_size = (off_t)image_size;
	memset(&image_file, 0, sizeof(image_file));
	image_file.f_inode = &image_inode;
	return header;
}

static int
load(struct vmspace *vm, struct elf32_image_info *info)
{
	image_file.f_offset = 0;
	return elf32_load(&image_file, vm, info);
}

static struct elf32_phdr *
add_stack_header(struct elf32_ehdr *header, uint32_t size, uint32_t flags)
{
	struct elf32_phdr *program;
	assert(header->e_phnum < 4);
	program = (struct elf32_phdr *)(image + header->e_phoff) +
		header->e_phnum++;
	memset(program, 0, sizeof(*program));
	program->p_type = PT_GNU_STACK;
	program->p_memsz = size;
	program->p_flags = flags;
	return program;
}

int
main(void)
{
	struct vmspace vm;
	struct elf32_ehdr *header;
	struct elf32_phdr *program;
	struct elf32_image_info info;

	memset(&vm, 0, sizeof(vm));
	header = make_valid_image();
	assert(load(&vm, &info) == 0);
	assert(info.entry == LOAD_ADDRESS && vm.entry == LOAD_ADDRESS);
	assert(info.stack_size == EXEC_STACK_DEFAULT_SIZE);
	assert(mapped_region.start == LOAD_ADDRESS);
	assert(mapped_region.size == 4096);
	assert(mapped_region.prot == (HAL_SPACE_READ | HAL_SPACE_EXEC));
	assert(mapped_region.backing == VM_BACKING_FILE);
	assert(mapped_region.file == &image_file);
	assert(mapped_region.file_offset == LOAD_OFFSET);
	assert(mapped_region.data_start == LOAD_ADDRESS);
	assert(mapped_region.data_size == 4);
	assert(mapped_region.pages == NULL);
	reset_mapping();

	header = make_valid_image();
	add_stack_header(header, 0, PF_R | PF_W);
	assert(load(&vm, &info) == 0);
	assert(info.stack_size == EXEC_STACK_DEFAULT_SIZE && map_count == 1);
	reset_mapping();

	header = make_valid_image();
	add_stack_header(header, 64U * 1024U, PF_R | PF_W);
	assert(load(&vm, &info) == 0);
	assert(info.stack_size == 64U * 1024U && map_count == 1);
	reset_mapping();

	header = make_valid_image();
	add_stack_header(header, 64U * 1024U + 1U, PF_R | PF_W);
	assert(load(&vm, &info) == 0);
	assert(info.stack_size == 68U * 1024U && map_count == 1);
	reset_mapping();

	header = make_valid_image();
	add_stack_header(header, EXEC_STACK_HARD_MAX, PF_R | PF_W);
	assert(load(&vm, &info) == 0);
	assert(info.stack_size == EXEC_STACK_HARD_MAX && map_count == 1);
	reset_mapping();

	header = make_valid_image();
	add_stack_header(header, EXEC_STACK_HARD_MAX + 1U, PF_R | PF_W);
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	header = make_valid_image();
	add_stack_header(header, 65536, PF_R | PF_W);
	add_stack_header(header, 65536, PF_R | PF_W);
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	header = make_valid_image();
	add_stack_header(header, 65536, PF_R | PF_W | PF_X);
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	header = make_valid_image();
	add_stack_header(header, 65536, PF_R);
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	header = make_valid_image();
	add_stack_header(header, 65536, PF_W);
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	header = make_valid_image();
	program = add_stack_header(header, 65536, PF_R | PF_W);
	program->p_filesz = 1;
	assert(load(&vm, &info) == ENOEXEC && map_count == 0);

	header = make_valid_image();
	header->e_ident[EI_CLASS] = 2;
	assert(load(&vm, &info) == ENOEXEC);

	header = make_valid_image();
	program = (struct elf32_phdr *)(image + header->e_phoff);
	program->p_type = PT_INTERP;
	assert(load(&vm, &info) == ENOEXEC);

	header = make_valid_image();
	program = (struct elf32_phdr *)(image + header->e_phoff);
	program->p_filesz = program->p_memsz + 1;
	assert(load(&vm, &info) == ENOEXEC);

	header = make_valid_image();
	image_size = sizeof(*header) - 1;
	image_inode.i_size = (off_t)image_size;
	assert(load(&vm, &info) == ENOEXEC);

	puts("zedBSD ELF32 loader host tests: PASS");
	return 0;
}
