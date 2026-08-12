/* zedBSD inode/namei/mount host tests. SPDX-License-Identifier: Zlib */
#include "kern/disk.h"
#include "kern/file.h"
#include "kern/mount.h"
#include "kern/namecache.h"
#include "kern/namei.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%d: %s\n", __FILE__, \
	__LINE__, #x); failures++; } } while (0)

struct mem_node {
	struct inode *inode;
	struct mem_node *parent;
	const char *name;
	const char *contents;
};

struct mem_fs {
	struct mem_node nodes[4];
	unsigned lookup_calls;
	char marker;
};

static int component_equal(const struct componentname *name, const char *s)
{
	return strlen(s) == name->cn_namelen &&
	       memcmp(name->cn_nameptr, s, name->cn_namelen) == 0;
}

static int mem_lookup(struct inode *directory, const struct componentname *name,
		      struct inode **result)
{
	struct mem_node *node = directory->i_data;
	struct mem_fs *fs = directory->i_mount->m_data;
	unsigned i;
	fs->lookup_calls++;
	if (component_equal(name, ".")) {
		inode_ref(directory); *result = directory; return 0;
	}
	if (component_equal(name, "..")) {
		inode_ref(node->parent->inode); *result = node->parent->inode; return 0;
	}
	for (i = 0; i < 4; i++)
		if (fs->nodes[i].parent == node && fs->nodes[i].name != NULL &&
		    component_equal(name, fs->nodes[i].name)) {
			inode_ref(fs->nodes[i].inode);
			*result = fs->nodes[i].inode;
			return 0;
		}
	return ENOENT;
}

static ssize_t mem_read(struct file *file, void *buffer, size_t length)
{
	struct mem_node *node = file->f_inode->i_data;
	size_t available, count;
	if ((uint64_t)file->f_offset >= (uint64_t)file->f_inode->i_size)
		return 0;
	available = (size_t)(file->f_inode->i_size - file->f_offset);
	count = length < available ? length : available;
	memcpy(buffer, node->contents + (size_t)file->f_offset, count);
	file->f_offset += (off_t)count;
	return (ssize_t)count;
}

static int mem_readdir(struct file *file, struct dirent *entry, int *eof)
{
	struct mem_node *directory = file->f_inode->i_data;
	struct mem_fs *fs = file->f_inode->i_mount->m_data;
	unsigned wanted = (unsigned)file->f_offset, seen = 0, i;
	for (i = 0; i < 4; i++) {
		if (fs->nodes[i].parent != directory || fs->nodes[i].name == NULL)
			continue;
		if (seen++ != wanted)
			continue;
		memset(entry, 0, sizeof(*entry));
		entry->d_ino = fs->nodes[i].inode->i_ino;
		entry->d_type = fs->nodes[i].inode->i_type;
		strcpy(entry->d_name, fs->nodes[i].name);
		file->f_offset++;
		*eof = 0;
		return 0;
	}
	*eof = 1;
	return 0;
}

static const struct inode_ops mem_iops = { .lookup = mem_lookup };
static const struct file_ops mem_dir_fops = { .readdir = mem_readdir };
static const struct file_ops mem_file_fops = { .read = mem_read };

static int mem_probe(struct disk *disk)
{
	return disk->d_data != NULL ? 0 : EOPNOTSUPP;
}

static int mem_mount(struct mount *mountp)
{
	struct mem_fs *fs = mountp->m_disk->d_data;
	static const char hello[] = "hello from ";
	unsigned i;
	memset(fs, 0, sizeof(*fs));
	fs->marker = mountp->m_disk->d_name[3];
	fs->nodes[0].name = "";
	fs->nodes[0].parent = &fs->nodes[0];
	fs->nodes[1].name = "dir";
	fs->nodes[1].parent = &fs->nodes[0];
	fs->nodes[2].name = "hello";
	fs->nodes[2].parent = &fs->nodes[0];
	fs->nodes[2].contents = hello;
	fs->nodes[3].name = "nested";
	fs->nodes[3].parent = &fs->nodes[1];
	fs->nodes[3].contents = "nested";
	for (i = 0; i < 4; i++) {
		struct inode *inode = inode_alloc(mountp);
		if (inode == NULL) return ENOSPC;
		fs->nodes[i].inode = inode;
		inode->i_ino = i + 1U;
		inode->i_data = &fs->nodes[i];
		inode->i_linkcount = 1;
		if (i < 2) {
			inode->i_type = INODE_DIR;
			inode->i_mode = S_IFDIR | 0555U;
			inode->i_op = &mem_iops;
			inode->i_fop = &mem_dir_fops;
		} else {
			inode->i_type = INODE_REG;
			inode->i_mode = S_IFREG | 0444U;
			inode->i_fop = &mem_file_fops;
			inode->i_size = (off_t)strlen(inode->i_data == &fs->nodes[2] ?
				hello : "nested");
		}
		if (i != 0) inode_release(inode); /* leave only cache ownership */
	}
	fs->nodes[0].inode->i_flags = INODE_ROOT;
	mountp->m_data = fs;
	mountp->m_root = fs->nodes[0].inode;
	return 0;
}

static const struct filesystem_type mem_type = {
	.fs_name = "mem",
	.probe = mem_probe,
	.mount = mem_mount,
};

static int leaf_submit(struct disk *disk, struct bio *bio)
{
	(void)disk;
	bio_complete(bio, 0, (size_t)bio->b_block_count * 512U);
	return 0;
}
static const struct disk_ops leaf_ops = { .submit = leaf_submit };
static struct mem_fs stores[2];

static struct disk *add_disk(const char *name, struct mem_fs *store)
{
	struct disk *disk = disk_alloc();
	CHECK(disk != NULL);
	if (disk == NULL) return NULL;
	strcpy(disk->d_name, name);
	disk->d_block_size = 512;
	disk->d_block_count = 16;
	disk->d_ops = &leaf_ops;
	disk->d_data = store;
	CHECK(disk_create(disk) == 0);
	return disk;
}

int main(void)
{
	struct fat_mount_args a = { "mem0" }, b = { "mem1" };
	struct cwdinfo context;
	struct path root_path;
	struct inode *inode, *again;
	struct file *file;
	struct dirent entry;
	char data[32] = {0};
	int eof;
	unsigned lookups;

	disk_registry_reset(); mount_reset(); file_pool_reset();
	add_disk("mem0", &stores[0]); add_disk("mem1", &stores[1]);
	CHECK(filesystem_register(&mem_type) == 0);
	CHECK(mount_rootfs() == 0);
	CHECK(mount("mem", "/disk1", MOUNT_READ_ONLY, &a) == 0);
	CHECK(mount("auto", "/disk2", MOUNT_READ_ONLY, &b) == 0);
	path_set(&root_path, mount_root_get(), mount_root_inode());
	CHECK(cwdinfo_init(&context, &root_path) == 0);
	path_release(&root_path);

	CHECK(namei_at(&context, "/disk1//dir/./nested", &inode) == 0);
	CHECK(inode->i_type == INODE_REG);
	inode_release(inode);
	CHECK(namei_at(&context, "/disk1/hello/", &inode) == ENOTDIR);
	CHECK(namei_at(&context, "/disk1/missing", &inode) == ENOENT);
	CHECK(namei_at(&context, "/disk1/hello/x", &inode) == ENOTDIR);
	CHECK(namei_at(&context, "/disk1/dir/..", &inode) == 0);
	CHECK(inode == mount_find("/disk1")->m_root);
	inode_release(inode);
	CHECK(namei_at(&context, "/disk1/..", &inode) == 0);
	CHECK(inode == mount_root_inode());
	inode_release(inode);

	lookups = stores[0].lookup_calls;
	CHECK(namei_at(&context, "/disk1/hello", &inode) == 0);
	CHECK(namei_at(&context, "/disk1/hello", &again) == 0);
	CHECK(stores[0].lookup_calls == lookups);
	CHECK(inode == again);
	inode_release(inode); inode_release(again);

	CHECK(fs_chdir(&context, "/disk1/dir") == 0);
	CHECK(!strcmp(fs_getcwd(&context), "/disk1/dir"));
	CHECK(namei_at(&context, "nested", &inode) == 0);
	inode_release(inode);
	CHECK(fs_chdir(&context, "../..") == 0);
	CHECK(!strcmp(fs_getcwd(&context), "/"));

	CHECK(file_openat(&context, "/disk2/hello", O_RDONLY, 0, &file) == 0);
	CHECK(file_read(file, data, sizeof(data)) == 11);
	CHECK(!memcmp(data, "hello from ", 11));
	CHECK(file_close(file) == 0);
	CHECK(file_openat(&context, "/", O_RDONLY | O_DIRECTORY, 0, &file) == 0);
	CHECK(file_readdir(file, &entry, &eof) == 0 && !eof);
	CHECK(!strcmp(entry.d_name, "disk1"));
	CHECK(file_readdir(file, &entry, &eof) == 0 && !eof);
	CHECK(!strcmp(entry.d_name, "disk2"));
	CHECK(file_readdir(file, &entry, &eof) == 0 && eof);
	CHECK(file_close(file) == 0);

	cwdinfo_destroy(&context);
	if (failures) {
		printf("VFS host tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("zedBSD inode/namei/mount host tests: OK\n");
	return 0;
}
