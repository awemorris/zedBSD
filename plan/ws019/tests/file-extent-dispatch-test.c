/* Filesystem capability refusal and callback/error propagation. */
#include <kern/file-backing.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned provider_calls;
static unsigned callback_calls;

static int
consume(uint64_t logical, uint64_t physical, uint32_t count, void *context)
{
	assert(logical == 0 && physical == 37 && count == 8);
	callback_calls++;
	return *(int *)context;
}

static int
provide(struct file *file, file_extent_cb callback, void *context)
{
	assert(file->f_inode->i_type == INODE_REG);
	provider_calls++;
	return callback(0, 37, 8, context);
}

static int
metadata(uint64_t physical, uint32_t count, void *context)
{
	assert(physical == 71 && count == 8);
	return *(int *)context;
}

static int
provide_metadata(struct file *file, file_metadata_extent_cb callback, void *context)
{
	assert(file->f_inode->i_type == INODE_REG);
	return callback(71, 8, context);
}

int
main(void)
{
	struct file file;
	struct inode inode;
	struct mount mountp;
	struct disk disk;
	struct filesystem_type type;
	int error;

	memset(&file, 0, sizeof(file));
	memset(&inode, 0, sizeof(inode));
	memset(&mountp, 0, sizeof(mountp));
	memset(&disk, 0, sizeof(disk));
	memset(&type, 0, sizeof(type));
	error = 0;
	assert(file_backing_extents(NULL, consume, &error) == EINVAL);
	assert(file_backing_extents(&file, consume, &error) == EINVAL);
	file.f_inode = &inode;
	inode.i_type = INODE_DIR;
	assert(file_backing_extents(&file, consume, &error) == EINVAL);
	inode.i_type = INODE_REG;
	assert(file_backing_extents(&file, consume, &error) == EOPNOTSUPP);
	inode.i_mount = &mountp;
	assert(file_backing_extents(&file, consume, &error) == EOPNOTSUPP);
	mountp.m_disk = &disk;
	assert(file_backing_extents(&file, consume, &error) == EOPNOTSUPP);
	mountp.m_type = &type;
	assert(file_backing_extents(&file, consume, &error) == EOPNOTSUPP);
	type.file_extents = provide;
	assert(file_backing_extents(&file, NULL, &error) == EINVAL);
	assert(provider_calls == 0 && callback_calls == 0);
	assert(file_backing_extents(&file, consume, &error) == 0);
	error = EIO;
	assert(file_backing_extents(&file, consume, &error) == EIO);
	assert(provider_calls == 2 && callback_calls == 2);
	assert(file_backing_metadata_extents(NULL, metadata, &error) == EINVAL);
	assert(file_backing_metadata_extents(&file, NULL, &error) == EINVAL);
	assert(file_backing_metadata_extents(&file, metadata, &error) == 0);
	type.file_metadata_extents = provide_metadata;
	assert(file_backing_metadata_extents(&file, metadata, &error) == EIO);
	error = 0;
	assert(file_backing_metadata_extents(&file, metadata, &error) == 0);
	type.file_extents = NULL;
	assert(file_backing_metadata_extents(&file, metadata, &error) == EOPNOTSUPP);
	puts("file extent dispatch: PASS");
	return 0;
}
