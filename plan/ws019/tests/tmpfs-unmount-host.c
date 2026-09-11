/* Actual tmpfs/inode/mount teardown, reusing independent HAL and mount fixtures. */
#define main old_mount_suite_main
#define file_readdir memory_fixture_readdir
#include "../../ws018/tests/legacy-disk-mount-test.c"
#undef main
#undef file_readdir
#include <kern/tmpfs.h>

int file_readdir(struct file *file, struct dirent *entry, int *eof)
{
	if (file->f_inode->i_fop != NULL && file->f_inode->i_fop->readdir != NULL)
		return file->f_inode->i_fop->readdir(file, entry, eof);
	return memory_fixture_readdir(file, entry, eof);
}

static size_t committed;
static int sync_error, prepare_error;
static struct mount *pressure_mount;
static void (*retire_original)(struct inode *);
static unsigned pressure_runs;
static int test_sync(struct mount *mountp) { (void)mountp; return sync_error; }
static int test_prepare(struct mount *mountp) { (void)mountp; return prepare_error; }
static void pressure_retire(struct inode *inode)
{
	struct inode *allocated[4096];
	unsigned count = 0, before = inode_cache_mount_count(inode->i_mount);
	retire_original(inode);
	while (count < 4096) {
		allocated[count] = inode_alloc(pressure_mount);
		if (allocated[count] == NULL) break;
		count++;
	}
	CHECK(count < 4096);
	CHECK(inode_cache_mount_count(inode->i_mount) == before);
	while (count != 0) {
		count--;
		allocated[count]->i_flags |= INODE_DEAD;
		inode_release(allocated[count]);
	}
	pressure_runs++;
}
const struct file_ops fifo_file_ops = {0};
void *kern_calloc(size_t n, size_t size) { return calloc(n, size); }
int vm_commit_reserve(size_t bytes) { committed += bytes; return 0; }
void vm_commit_release(size_t bytes) { CHECK(committed >= bytes); committed -= bytes; }

int main(void)
{
	struct mount *root, *child;
	struct path root_path, target;
	struct cwdinfo cwd;
	struct inode *point, *file, *directory, *next, *replaced, *symbol;
	struct filesystem_type tested_type = tmpfs_type;
	struct inode_ops root_ops;
	struct file io = {0};
	char content[8193], output[8193];
	struct inode_creation_request request;
	struct componentname file_name = component("file");
	struct componentname dir_name = component("directory");
	struct componentname alias = component("alias");
	struct componentname replacement_name = component("replacement");
	struct componentname symbol_name = component("symbol");
	unsigned baseline, cycle, depth;
	int error;

	mount_reset();
	CHECK(filesystem_register(&memory_type) == 0);
	tested_type.sync = test_sync;
	tested_type.prepare_unmount = test_prepare;
	CHECK(filesystem_register(&tested_type) == 0);
	CHECK(mount_root_create("memory", 0, NULL, &root) == 0);
	path_set(&root_path, root, root->m_root);
	CHECK(cwdinfo_init(&cwd, &root_path) == 0);
	point = new_node(root, 101, INODE_DIR);
	set_name(root->m_root, "target", point);
	set_name(point, "..", root->m_root);
	baseline = inode_cache_count();
	pressure_mount = root;
	memset(content, 0x5a, sizeof(content));
	for (cycle = 0; cycle < 32; cycle++) {
		CHECK(mount_context(&cwd, "tmpfs", "/target", 0, NULL) == 0);
		CHECK(namei_path_at(&cwd, "/target", &target) == 0);
		child = target.p_mount;
		root_ops = *child->m_root->i_op;
		retire_original = root_ops.retire_namespace;
		root_ops.retire_namespace = pressure_retire;
		child->m_root->i_op = &root_ops;
		CHECK(inode_creation_request_system(INODE_REG, 0600, 0, 0, 0, &request) == 0);
		CHECK(inode_create(child->m_root, &file_name, &request, &file) == 0);
		CHECK(inode_link(child->m_root, &alias, file) == 0);
		CHECK(atomic_load_acquire(&file->i_namespace_refs) == 2);
		CHECK(inode_create(child->m_root, &replacement_name, &request, &replaced) == 0);
		CHECK(inode_rename(child->m_root, &alias, child->m_root, &replacement_name, 0) == 0);
		CHECK(atomic_load_acquire(&replaced->i_namespace_refs) == 0);
		inode_release(replaced);
		CHECK(atomic_load_acquire(&file->i_namespace_refs) == 2);
		io.f_path.p_inode = file;
		io.f_inode = file;
		io.f_path.p_mount = child;
		CHECK(file->i_fop->pwrite(&io, content, sizeof(content), 17) == sizeof(content));
		CHECK(file->i_fop->pread(&io, output, sizeof(output), 17) == sizeof(output));
		CHECK(memcmp(content, output, sizeof(content)) == 0);
		CHECK(inode_setxattr(file, "user.test", content, 100, 0) == 0);
		CHECK(inode_creation_request_system(INODE_SYMLINK, 0777, 0, 0, 0, &request) == 0);
		CHECK(inode_symlink(child->m_root, &symbol_name, "file", &request, &symbol) == 0);
		inode_release(symbol);
		CHECK(inode_creation_request_system(INODE_DIR, 0700, 0, 0, 0, &request) == 0);
		CHECK(inode_mkdir(child->m_root, &dir_name, &request, &directory) == 0);
		for (depth = 0; depth < 24; depth++) {
			CHECK(inode_mkdir(directory, &dir_name, &request, &next) == 0);
			inode_release(directory);
			directory = next;
		}
		inode_release(directory);
		path_release(&target);
		error = mount_context(&cwd, "tmpfs", "/target/directory", 0, NULL);
		if (error != 0) fprintf(stderr, "child mount error %d cycle %u\n", error, cycle);
		CHECK(error == 0);
		CHECK(unmount_context(&cwd, "/target") == EBUSY);
		CHECK(unmount_context(&cwd, "/target/directory") == 0);
		CHECK(unmount_context(&cwd, "/target") == EBUSY);
		CHECK(file->i_linkcount == 2);
		inode_release(file);
		CHECK(fs_chdir(&cwd, "/target") == 0);
		CHECK(unmount_context(&cwd, "/target") == EBUSY);
		CHECK(fs_chdir(&cwd, "/") == 0);
		sync_error = EIO;
		CHECK(unmount_context(&cwd, "/target") == EIO);
		sync_error = 0;
		prepare_error = EIO;
		CHECK(unmount_context(&cwd, "/target") == EIO);
		prepare_error = 0;
		CHECK(namei_path_at(&cwd, "/target/file", &target) == 0);
		io.f_path = target;
		io.f_inode = target.p_inode;
		CHECK(target.p_inode->i_fop->pread(&io, output, sizeof(output), 17) == sizeof(output));
		CHECK(memcmp(content, output, sizeof(content)) == 0);
		CHECK(inode_getxattr(target.p_inode, "user.test", output, sizeof(output)) == 100);
		CHECK(memcmp(content, output, 100) == 0);
		path_release(&target);
		CHECK(unmount_context(&cwd, "/target") == 0);
		CHECK(inode_cache_count() == baseline);
		CHECK(committed == 0);
	}
	CHECK(pressure_runs == 32);
	cwdinfo_destroy(&cwd);
	path_release(&root_path);
	inode_release(point);
	mount_reset();
	printf("tmpfs unmount PASS %u checks\n", checks);
	return 0;
}
