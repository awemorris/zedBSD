/* Real mount/namei/inode lifecycle, with the existing independent memory FS. */
#define main old_mount_suite_main
#include "../../ws018-kernel-architecture/tests/legacy-disk-mount-test.c"
#undef main

struct context_job { struct cwdinfo *cwd; int error; };
static void context_mount_job(void *argument)
{
	struct context_job *job = argument;
	job->error = mount_context(job->cwd, "memory", "target", 0, NULL);
}
static ssize_t context_readlink(struct inode *inode, char *buffer, size_t size)
{
	const char *text = inode->i_data;
	size_t length = strlen(text);
	if (length > size) length = size;
	memcpy(buffer, text, length);
	return (ssize_t)length;
}

int main(void)
{
	struct mount *root, *child;
	struct path root_path, resolved, jail_path;
	struct cwdinfo cwd, jail;
	struct inode *run, *point, *file, *link, *subdir, *replacement;
	struct inode_ops link_ops = ops;
	struct context_job job;
	void *thread;
	struct memory_control control = {0};
	struct componentname target_name = component("target");
	unsigned baseline;

	mount_reset();
	CHECK(filesystem_register(&memory_type) == 0);
	CHECK(mount_root_create("memory", 0, NULL, &root) == 0);
	path_set(&root_path, root, root->m_root);
	CHECK(cwdinfo_init(&cwd, &root_path) == 0);
	run = new_node(root, 101, INODE_DIR);
	point = new_node(root, 102, INODE_DIR);
	file = new_node(root, 103, INODE_REG);
	set_name(root->m_root, "run", run);
	set_name(run, "..", root->m_root);
	set_name(run, "target", point);
	set_name(run, "file", file);
	set_name(point, "..", run);
	link = new_node(root, 104, INODE_SYMLINK);
	link_ops.readlink = context_readlink;
	link->i_op = &link_ops;
	link->i_data = "target";
	set_name(run, "alias", link);
	baseline = mount_count();
	CHECK(mount_context(&cwd, "memory", "/run/missing", 0, NULL) == ENOENT);
	CHECK(mount_context(&cwd, "memory", "/run/file", 0, NULL) == ENOTDIR);
	CHECK(mount_context(&cwd, "memory", "/", 0, NULL) == EBUSY);
	control.mount_error = EIO;
	CHECK(mount_context(&cwd, "memory", "/run/target", 0, &control) == EIO);
	CHECK(mount_count() == baseline);
	CHECK(fs_chdir(&cwd, "/run") == 0);
	CHECK(mount_context(&cwd, "memory", "alias", 0, NULL) == 0);
	CHECK(namei_path_at(&cwd, "target", &resolved) == 0);
	child = resolved.p_mount;
	subdir = new_node(child, 105, INODE_DIR);
	set_name(child->m_root, "nested", subdir);
	set_name(subdir, "..", child->m_root);
	path_release(&resolved);
	CHECK(mount_context(&cwd, "memory", "target/nested", 0, NULL) == 0);
	CHECK(unmount_context(&cwd, "alias") == EBUSY);
	CHECK(unmount_context(&cwd, "alias/nested") == 0);
	set_name(child->m_root, "nested", NULL);
	set_name(subdir, "..", NULL);
	inode_release(subdir);
	CHECK(unmount_context(&cwd, "alias") == 0);

	/* A target replaced during lookup must not publish over the replacement. */
	replacement = new_node(root, 106, INODE_DIR);
	job.cwd = &cwd;
	job.error = 0;
	lookup_pause_inode = point;
	lookup_pause_once = 1;
	host_gate_reset(1);
	thread = host_thread_start(context_mount_job, &job);
	host_gate_wait(1);
	CHECK(inode_rmdir(run, &target_name) == 0);
	set_name(run, "target", replacement);
	set_name(replacement, "..", run);
	host_gate_release(1);
	host_thread_join(thread);
	CHECK(job.error == ENOENT || job.error == EAGAIN);
	CHECK(mount_count() == baseline);
	lookup_pause_inode = NULL;
	set_name(point, "..", NULL);
	inode_release(point);
	point = replacement;
	control.mount_error = 0;
	CHECK(mount_context(&cwd, "memory", "/run/target/", MOUNT_READ_ONLY, &control) == 0);
	CHECK(mount_count() == baseline + 1);
	CHECK(mount_context(&cwd, "memory", "/run/target", 0, NULL) == EBUSY);
	CHECK(inode_rmdir(run, &target_name) == EBUSY);
	CHECK(namei_path_at(&cwd, "/run/target", &resolved) == 0);
	CHECK(resolved.p_mount->m_flags & MOUNT_READ_ONLY);
	CHECK(unmount_context(&cwd, "/run/target") == EBUSY);
	path_release(&resolved);
	control.sync_error = EIO;
	CHECK(unmount_context(&cwd, "/run/target") == EIO);
	control.sync_error = 0;
	CHECK(fs_chdir(&cwd, "/run") == 0);
	CHECK(unmount_context(&cwd, "target") == 0);
	CHECK(unmount_context(&cwd, "target") == EINVAL);
	CHECK(unmount_context(&cwd, "missing") == ENOENT);
	CHECK(mount_count() == baseline);

	/* A root-confined caller's absolute and .. paths stay inside its root. */
	path_set(&jail_path, root, run);
	CHECK(cwdinfo_init(&jail, &jail_path) == 0);
	CHECK(mount_context(&jail, "memory", "/../target", 0, NULL) == 0);
	CHECK(namei_path_at(&cwd, "target", &resolved) == 0);
	child = resolved.p_mount;
	CHECK(!strcmp(child->m_path, "/run/target"));
	path_release(&resolved);
	CHECK(unmount_context(&jail, "/../target") == 0);
	CHECK(mount_context(&jail, "memory", "/run/target", 0, NULL) == ENOENT);
	CHECK(mount_context(&jail, "memory", "/", 0, NULL) == EBUSY);
	CHECK(unmount_context(&jail, "/") == EINVAL);
	cwdinfo_destroy(&jail);
	path_release(&jail_path);
	CHECK(fs_chdir(&cwd, "/") == 0);
	cwdinfo_destroy(&cwd);
	path_release(&root_path);
	clear_names();
	inode_release(point);
	inode_release(file);
	inode_release(link);
	inode_release(run);
	CHECK(mount_count() == baseline);
	mount_reset();
	printf("nested mount PASS %u checks\n", checks);
	return 0;
}
