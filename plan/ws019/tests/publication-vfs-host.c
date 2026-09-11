/* Real VFS transactions with an in-memory namespace endpoint. */
#define main legacy_mount_main
#include "../../ws018/tests/legacy-disk-mount-test.c"
#undef main
#include <zedbsd/rename.h>

static unsigned publish_calls;
static int publish_error;
static int publish_rename(struct inode *old, const struct componentname *from,
    struct inode *new, const struct componentname *to, unsigned flags)
{
	struct inode *source;
	char a[40], b[40];
	CHECK(mutex_owned(old->i_mount->m_vfs_transaction_lock));
	CHECK(flags == 0);
	publish_calls++;
	if (publish_error) return publish_error;
	CHECK(lookup(old, from, &source) == 0);
	CHECK(from->cn_namelen < sizeof(a) && to->cn_namelen < sizeof(b));
	memcpy(a, from->cn_nameptr, from->cn_namelen); a[from->cn_namelen] = 0;
	memcpy(b, to->cn_nameptr, to->cn_namelen); b[to->cn_namelen] = 0;
	set_name(new, b, source); set_name(old, a, NULL);
	inode_release(source);
	return 0;
}
static const struct inode_ops publish_ops = { .lookup = lookup, .rename = publish_rename };
struct publisher { struct inode *parent; struct componentname from; int result; };
static const struct componentname final_name = { "final", 5, 0 };
static void publish(void *argument)
{
	struct publisher *job = argument;
	job->result = inode_rename(job->parent, &job->from, job->parent,
	    &final_name, RENAME_NOREPLACE);
}
int main(void)
{
	struct mount *root;
	struct mount foreign = {0};
	struct inode other = {0};
	struct inode *a, *b, *found;
	struct componentname left = { "left", 4, 0 }, right = { "right", 5, 0 };
	struct publisher jobs[2];
	void *threads[2];
	unsigned before, iteration;
	mount_reset(); CHECK(filesystem_register(&memory_type) == 0);
	CHECK(mount_root_create("memory", 0, NULL, &root) == 0);
	root->m_root->i_op = &publish_ops;
	other.i_mount = &foreign;
	CHECK(inode_rename(root->m_root, &left, &other, &right, RENAME_NOREPLACE) == EXDEV);
	a = new_node(root, 10, INODE_REG); b = new_node(root, 11, INODE_REG);
	set_name(root->m_root, "left", a); set_name(root->m_root, "right", b);
	CHECK(inode_rename(root->m_root, &left, root->m_root, &right, 2) == EINVAL);
	CHECK(inode_rename(root->m_root, &left, root->m_root, &right, RENAME_NOREPLACE) == EEXIST);
	CHECK(publish_calls == 0);
	CHECK(inode_rename(root->m_root, &left, root->m_root, &left, RENAME_NOREPLACE) == EEXIST);
	CHECK(inode_rename(root->m_root, &left, root->m_root, &left, 0) == 0);
	b->i_type = INODE_DIR;
	CHECK(inode_rename(root->m_root, &left, root->m_root, &right, RENAME_NOREPLACE) == EEXIST);
	b->i_type = INODE_REG;
	a->i_flags |= INODE_SWAPFILE;
	CHECK(inode_rename(root->m_root, &left, root->m_root, &final_name, RENAME_NOREPLACE) == EBUSY);
	a->i_flags &= ~INODE_SWAPFILE;
	publish_error = EIO;
	CHECK(inode_rename(root->m_root, &left, root->m_root, &final_name, RENAME_NOREPLACE) == EIO);
	CHECK(inode_lookup(root->m_root, &left, &found) == 0 && found == a); inode_release(found);
	publish_error = 0;
	CHECK(inode_rename(root->m_root, &left, root->m_root, &right, 0) == 0);
	CHECK(inode_lookup(root->m_root, &right, &found) == 0 && found == a); inode_release(found);
	for (iteration = 0; iteration < 100; iteration++) {
		clear_names();
		set_name(root->m_root, "left", a); set_name(root->m_root, "right", b);
		jobs[0].parent = jobs[1].parent = root->m_root;
		jobs[0].from = left; jobs[1].from = right;
		before = publish_calls;
		threads[0] = host_thread_start(publish, &jobs[0]);
		threads[1] = host_thread_start(publish, &jobs[1]);
		host_thread_join(threads[0]); host_thread_join(threads[1]);
		CHECK((jobs[0].result == 0 && jobs[1].result == EEXIST) ||
		    (jobs[1].result == 0 && jobs[0].result == EEXIST));
		CHECK(publish_calls == before + 1);
		CHECK(inode_lookup(root->m_root, &final_name, &found) == 0);
		CHECK(found == (jobs[0].result == 0 ? a : b)); inode_release(found);
	}
	clear_names(); inode_release(a); inode_release(b);
	printf("publication VFS: %u checks PASS, 100 competing publishers\n", checks);
	return 0;
}
