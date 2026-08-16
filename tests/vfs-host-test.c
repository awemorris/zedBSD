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
	char owned_name[NAME_MAX + 1U];
	const char *contents;
	char link_target[64];
};

#define MEM_NODE_MAX 8U
struct mem_fs {
	struct mem_node nodes[MEM_NODE_MAX];
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
	for (i = 0; i < MEM_NODE_MAX; i++)
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
	for (i = 0; i < MEM_NODE_MAX; i++) {
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

static int mem_link(struct inode *directory,
		    const struct componentname *name, struct inode *target)
{
	struct mem_node *parent = directory->i_data;
	struct mem_fs *fs = directory->i_mount->m_data;
	unsigned i;
	for (i = 0; i < MEM_NODE_MAX; i++) {
		if (fs->nodes[i].name != NULL)
			continue;
		fs->nodes[i].parent = parent;
		fs->nodes[i].name = "hard";
		if (!component_equal(name, fs->nodes[i].name))
			return EINVAL;
		fs->nodes[i].inode = target;
		return 0;
	}
	return ENOSPC;
}

static int mem_symlink(struct inode *directory,
		       const struct componentname *name, const char *target,
		       struct inode **result)
{
	struct mem_node *parent = directory->i_data;
	struct mem_fs *fs = directory->i_mount->m_data;
	struct inode *inode;
	unsigned i;
	for (i = 0; i < MEM_NODE_MAX; i++)
		if (fs->nodes[i].name == NULL)
			break;
	if (i == MEM_NODE_MAX)
		return ENOSPC;
	inode = inode_alloc(directory->i_mount);
	if (inode == NULL)
		return ENOSPC;
	fs->nodes[i].parent = parent;
	fs->nodes[i].name = component_equal(name, "link") ? "link" :
		component_equal(name, "loop") ? "loop" : "relative";
	if (strlen(target) >= sizeof(fs->nodes[i].link_target)) {
		inode_release(inode);
		return ENAMETOOLONG;
	}
	strcpy(fs->nodes[i].link_target, target);
	fs->nodes[i].contents = fs->nodes[i].link_target;
	fs->nodes[i].inode = inode;
	inode->i_ino = i + 1U;
	inode->i_type = INODE_SYMLINK;
	inode->i_mode = S_IFLNK | 0777U;
	inode->i_linkcount = 1;
	inode->i_size = (off_t)strlen(target);
	inode->i_data = &fs->nodes[i];
	inode->i_op = directory->i_op;
	*result = inode;
	return 0;
}

static int
mem_rename(struct inode *old_directory, const struct componentname *old_name,
	   struct inode *new_directory, const struct componentname *new_name,
	   unsigned flags)
{
	struct mem_node *old_parent = old_directory->i_data;
	struct mem_node *new_parent = new_directory->i_data;
	struct mem_fs *fs = old_directory->i_mount->m_data;
	struct mem_node *source = NULL;
	unsigned i;

	if (flags != 0 || new_name->cn_namelen > NAME_MAX)
		return EINVAL;
	for (i = 0; i < MEM_NODE_MAX; i++) {
		if (fs->nodes[i].parent == old_parent &&
		    fs->nodes[i].name != NULL &&
		    component_equal(old_name, fs->nodes[i].name))
			source = &fs->nodes[i];
		if (fs->nodes[i].parent == new_parent &&
		    fs->nodes[i].name != NULL &&
		    component_equal(new_name, fs->nodes[i].name))
			return EEXIST;
	}
	if (source == NULL)
		return ENOENT;
	memcpy(source->owned_name, new_name->cn_nameptr,
	    new_name->cn_namelen);
	source->owned_name[new_name->cn_namelen] = '\0';
	source->name = source->owned_name;
	source->parent = new_parent;
	return 0;
}

static ssize_t mem_readlink(struct inode *inode, char *buffer, size_t capacity)
{
	struct mem_node *node = inode->i_data;
	size_t length = strlen(node->link_target);
	if (length > capacity)
		length = capacity;
	memcpy(buffer, node->link_target, length);
	return (ssize_t)length;
}

static const struct inode_ops mem_iops = {
	.lookup = mem_lookup,
	.rename = mem_rename,
	.link = mem_link,
	.symlink = mem_symlink,
	.readlink = mem_readlink,
};
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
	struct mount *root_mount_ref, *disk1_mount;
	struct inode *inode, *again;
	struct file *file;
	struct dirent entry;
	char data[32] = {0};
	char cwd[ZEDBSD_PATH_MAX];
	int eof;
	unsigned lookups;

	disk_registry_reset(); mount_reset(); file_pool_reset();
	add_disk("mem0", &stores[0]); add_disk("mem1", &stores[1]);
	CHECK(filesystem_register(&mem_type) == 0);
	CHECK(mount_rootfs() == 0);
	CHECK(mount("mem", "/disk1", 0, &a) == 0);
	CHECK(mount("auto", "/disk2", 0, &b) == 0);
	root_mount_ref = mount_root_get_ref();
	disk1_mount = mount_find_ref("/disk1");
	CHECK(root_mount_ref != NULL && disk1_mount != NULL);
	path_set(&root_path, root_mount_ref, mount_root_inode());
	mount_release(root_mount_ref);
	CHECK(cwdinfo_init(&context, &root_path) == 0);
	path_release(&root_path);

	CHECK(namei_at(&context, "/disk1//dir/./nested", &inode) == 0);
	CHECK(inode->i_type == INODE_REG);
	inode_release(inode);
	CHECK(namei_at(&context, "/disk1/hello/", &inode) == ENOTDIR);
	CHECK(namei_at(&context, "/disk1/missing", &inode) == ENOENT);
	CHECK(namei_at(&context, "/disk1/hello/x", &inode) == ENOTDIR);
	CHECK(namei_at(&context, "/disk1/dir/..", &inode) == 0);
	CHECK(inode == disk1_mount->m_root);
	inode_release(inode);

	/* Generic VFS hard links and relative/absolute symlink traversal. */
	CHECK(namei_at(&context, "/disk1/hello", &inode) == 0);
	{
		struct componentname hard = {
			.cn_nameptr = "hard", .cn_namelen = 4
		};
		CHECK(inode_link(disk1_mount->m_root, &hard, inode) == 0);
	}
	CHECK(namei_at(&context, "/disk1/hard", &again) == 0);
	CHECK(again == inode && inode->i_linkcount == 2);
	inode_release(again);
	inode_release(inode);
	{
		struct componentname link = {
			.cn_nameptr = "link", .cn_namelen = 4
		};
		struct componentname relative = {
			.cn_nameptr = "relative", .cn_namelen = 8
		};
		struct componentname loop = {
			.cn_nameptr = "loop", .cn_namelen = 4
		};
		struct inode *created;
		CHECK(inode_symlink(disk1_mount->m_root, &link,
		    "/disk1/dir/nested", &created) == 0);
		inode_release(created);
		CHECK(inode_symlink(stores[0].nodes[1].inode, &relative,
		    "../hello", &created) == 0);
		inode_release(created);
		CHECK(inode_symlink(disk1_mount->m_root, &loop,
		    "loop", &created) == 0);
		inode_release(created);
	}
	CHECK(namei_at(&context, "/disk1/link", &inode) == 0);
	CHECK(inode == stores[0].nodes[3].inode);
	inode_release(inode);
	CHECK(namei_at(&context, "/disk1/dir/relative", &inode) == 0);
	CHECK(inode == stores[0].nodes[2].inode);
	inode_release(inode);
	CHECK(namei_at(&context, "/disk1/loop", &inode) == ELOOP);
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
	CHECK(fs_getcwd(&context, cwd, sizeof(cwd)) == 0);
	CHECK(!strcmp(cwd, "/disk1/dir"));
	CHECK(namei_at(&context, "nested", &inode) == 0);
	inode_release(inode);
	{
		struct componentname old_name = {
			.cn_nameptr = "dir", .cn_namelen = 3
		};
		struct componentname new_name = {
			.cn_nameptr = "moved", .cn_namelen = 5
		};
		struct inode *root = disk1_mount->m_root;
		uint64_t before = root->i_dirseq;

		CHECK(inode_rename(root, &old_name, root, &new_name, 0) == 0);
		CHECK(root->i_dirseq == before + 1U);
		CHECK(fs_getcwd(&context, cwd, sizeof(cwd)) == 0);
		CHECK(!strcmp(cwd, "/disk1/moved"));
		CHECK(namei_at(&context, "nested", &inode) == 0);
		inode_release(inode);
		CHECK(namei_at(&context, "/disk1/dir", &inode) == ENOENT);
		CHECK(namei_at(&context, "/disk1/moved", &inode) == 0);
		inode_release(inode);
	}
	CHECK(fs_chdir(&context, "../..") == 0);
	CHECK(fs_getcwd(&context, cwd, sizeof(cwd)) == 0);
	CHECK(!strcmp(cwd, "/"));
	CHECK(fs_getcwd(&context, cwd, 1) == ERANGE);

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
	mount_release(disk1_mount);
	if (failures) {
		printf("VFS host tests: %d failure(s)\n", failures);
		return 1;
	}
	printf("zedBSD inode/namei/mount host tests: OK\n");
	return 0;
}
