/* Host-side tests for Boots filesystem-backed stdio. */

#include "kern/fs.h"
#include "kern/env.h"
#include "kern/namespace.h"
#include "libc/heap.h"
#include "libc/stdio-fs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char arena[128 * 1024];
static unsigned char contents[4096];
static uint64_t content_size;
static int exists;
static unsigned flushes;

static enum boots_fs_result probe(const struct boots_volume *volume)
{
	(void)volume;
	return BOOTS_FS_OK;
}

static enum boots_fs_result mount(struct boots_filesystem *filesystem)
{
	(void)filesystem;
	return BOOTS_FS_OK;
}

static enum boots_fs_result populate(const char *path, struct boots_file *file)
{
	if (strcmp(path, "/TEST.TXT") && strcmp(path, "TEST.TXT"))
		return BOOTS_FS_NOT_FOUND;
	if (!exists)
		return BOOTS_FS_NOT_FOUND;
	file->size = content_size;
	return BOOTS_FS_OK;
}

static enum boots_fs_result create(struct boots_filesystem *filesystem,
				    const char *path,
				    struct boots_file *file)
{
	(void)filesystem;
	if (strcmp(path, "/TEST.TXT") && strcmp(path, "TEST.TXT"))
		return BOOTS_FS_INVALID_PATH;
	exists = 1;
	content_size = 0;
	file->size = 0;
	return BOOTS_FS_OK;
}

static enum boots_fs_result open_file(struct boots_filesystem *filesystem,
				       const char *path,
				       struct boots_file *file)
{
	(void)filesystem;
	return populate(path, file);
}

static enum boots_fs_result read_file(struct boots_file *file,
		uint64_t offset, void *buffer, uint32_t length,
		boots_read_progress_t progress, void *progress_context)
{
	(void)file;
	(void)progress;
	(void)progress_context;
	memcpy(buffer, contents + offset, length);
	return BOOTS_FS_OK;
}

static enum boots_fs_result write_file(struct boots_file *file,
		uint64_t offset, const void *buffer, uint32_t length)
{
	uint64_t end = offset + length;

	if (end > sizeof(contents))
		return BOOTS_FS_NO_SPACE;
	if (offset > content_size)
		memset(contents + content_size, 0, (size_t)(offset - content_size));
	memcpy(contents + offset, buffer, length);
	if (end > content_size)
		content_size = end;
	file->size = content_size;
	return BOOTS_FS_OK;
}

static enum boots_fs_result truncate_file(struct boots_file *file,
					   uint64_t size)
{
	if (size > sizeof(contents))
		return BOOTS_FS_NO_SPACE;
	if (size > content_size)
		memset(contents + content_size, 0, (size_t)(size - content_size));
	content_size = size;
	file->size = size;
	return BOOTS_FS_OK;
}

static enum boots_fs_result flush_file(struct boots_file *file)
{
	(void)file;
	flushes++;
	return BOOTS_FS_OK;
}

static enum boots_fs_result readdir(struct boots_filesystem *filesystem,
		const char *path, unsigned index, struct boots_dirent *entry)
{
	(void)filesystem;
	if (index || !exists)
		return BOOTS_FS_NOT_FOUND;
	if (!strcmp(path, "HOME") || !strcmp(path, "home"))
		strcpy(entry->name, "COMPLETE.TXT");
	else if (!*path || !strcmp(path, "/"))
		strcpy(entry->name, "TEST.TXT");
	else
		return BOOTS_FS_NOT_FOUND;
	entry->size = content_size;
	return BOOTS_FS_OK;
}

static enum boots_fs_result stat_file(struct boots_filesystem *filesystem,
		const char *path, struct boots_dirent *entry)
{
	struct boots_file file;
	enum boots_fs_result result;

	(void)filesystem;
	if (!strcmp(path, "HOME") || !strcmp(path, "home")) {
		strcpy(entry->name, "HOME");
		entry->size = 0;
		entry->attributes = 0x10;
		return BOOTS_FS_OK;
	}
	result = populate(path, &file);
	if (result != BOOTS_FS_OK)
		return result;
	strcpy(entry->name, "TEST.TXT");
	entry->size = file.size;
	return BOOTS_FS_OK;
}

static int dummy_read(const void *context, uint32_t lba, void *buffer)
{
	(void)context;
	(void)lba;
	memset(buffer, 0, 512);
	return 1;
}

static const struct boots_filesystem_driver driver = {
	.name = "memory",
	.probe = probe,
	.mount = mount,
	.create = create,
	.open = open_file,
	.read = read_file,
	.write = write_file,
	.truncate = truncate_file,
	.flush = flush_file,
	.readdir = readdir,
	.stat = stat_file,
};

int main(void)
{
	const struct boots_filesystem_driver *drivers[] = { &driver };
	struct boots_volume volume = {
		.sector_size = 512,
		.read = dummy_read,
	};
	struct boots_filesystem filesystem;
	struct boots_namespace namespace;
	struct boots_dirent entry;
	struct boots_environment environment;
	FILE *file;
	char line[32];
	char bytes[4];

	boots_heap_init(arena, sizeof(arena));
	boots_env_init(&environment);
	assert(boots_env_set(&environment, "HOME", "HOME"));
	assert(boots_fs_mount(&filesystem, &volume, drivers, 1));
	boots_stdio_set_filesystem(&filesystem);
	boots_stdio_set_environment(&environment);
	assert(!strcmp(getenv("HOME"), "HOME"));
	assert(getenv("MISSING") == NULL);
	assert(access("/TEST.TXT", F_OK) == -1);
	file = fopen("/TEST.TXT", "wb");
	assert(file != NULL);
	assert(fprintf(file, "line %d\n", 42) == 8);
	assert(ftell(file) == 8);
	assert(fseek(file, 10, SEEK_SET) == 0);
	assert(fwrite("xy", 1, 2, file) == 2);
	assert(fclose(file) == 0 && flushes == 1);
	assert(content_size == 12 && contents[8] == 0 && contents[9] == 0);
	assert(access("/TEST.TXT", F_OK) == 0);

	file = fopen("/TEST.TXT", "rb");
	assert(file != NULL);
	assert(fgets(line, sizeof(line), file) == line);
	assert(!strcmp(line, "line 42\n"));
	assert(fseek(file, -4, SEEK_END) == 0);
	assert(ftell(file) == 8);
	assert(fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
	assert(bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 'x' &&
	       bytes[3] == 'y');
	assert(getc(file) == EOF);
	assert(fclose(file) == 0);

	assert(fopen("/TEST.TXT", "rb") != NULL);
	assert(boots_stdio_close_all() == 0);

	boots_namespace_init(&namespace);
	assert(boots_namespace_mount(&namespace, "disk1", &filesystem));
	assert(boots_namespace_set_default(&namespace, "disk1"));
	assert(boots_namespace_readdir_result(&namespace, "/disk1", 0,
					       &entry) == BOOTS_FS_OK);
	assert(!strcmp(entry.name, "test.txt"));
	assert(boots_namespace_readdir_result(&namespace, "/disk1/home/", 0,
					       &entry) == BOOTS_FS_OK);
	assert(!strcmp(entry.name, "complete.txt"));
	boots_stdio_set_namespace(&namespace);
	assert(getcwd(line, sizeof(line)) == line);
	assert(!strcmp(line, "/disk1"));
	assert(chdir("/disk1/home") == 0);
	assert(getcwd(line, sizeof(line)) == line);
	assert(!strcmp(line, "/disk1/home"));
	assert(chdir("/disk1") == 0);
	file = fopen("TEST.TXT", "rb");
	assert(file != NULL);
	assert(fclose(file) == 0);
	boots_stdio_set_namespace(NULL);
	boots_stdio_set_filesystem(NULL);
	boots_stdio_set_environment(NULL);
	assert(boots_heap_current() == 0 && boots_heap_validate());
	puts("Boots filesystem stdio host tests: OK");
	return 0;
}
