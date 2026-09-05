/* KA-T121: actual mount/namei/inode/cache code, memory filesystem and HAL.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/mount.h>
#include <kern/namei.h>
#include <kern/namecache.h>
#include <kern/file.h>
#include <kern/cred.h>
#include <kern/block-identity.h>
#include <kern/clock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zedbsd/mountinfo.h>
#include "mount-thread-host.h"

static unsigned checks, mutations, syncs;
static struct inode *mutation_source, *mutation_target;
static unsigned watch_contention, mutation_gate;
static unsigned lookup_pause_once;
static struct inode *lookup_pause_inode;
static _Thread_local unsigned thread_identity;
#define CHECK(x) do { __atomic_fetch_add(&checks, 1U, __ATOMIC_RELAXED); if (!(x)) { \
	fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); \
} } while (0)

void spin_init(struct spinlock *s, enum lock_rank r, const char *n)
{ memset(s, 0, sizeof(*s)); s->rank = r; s->name = n; }
unsigned long spin_lock_irqsave(struct spinlock *s)
{
	while (__atomic_exchange_n(&s->held.value, 1U, __ATOMIC_ACQUIRE))
		host_thread_yield();
	return 0;
}
void spin_unlock_irqrestore(struct spinlock *s, unsigned long f)
{ (void)f; __atomic_store_n(&s->held.value, 0U, __ATOMIC_RELEASE); }
int mutex_init(struct mutex *m, enum lock_rank r, const char *n)
{ memset(m, 0, sizeof(*m)); spin_init(&m->guard, r, n); return 0; }
int mutex_owned(struct mutex *m)
{
	return __atomic_load_n(&m->owner, __ATOMIC_RELAXED) ==
	    (struct thread *)&thread_identity;
}
void mutex_lock(struct mutex *m)
{
	CHECK(!mutex_owned(m));
	while (__atomic_exchange_n(&m->locked, 1U, __ATOMIC_ACQUIRE)) {
		if (__atomic_load_n(&watch_contention, __ATOMIC_RELAXED))
			host_gate_signal(2);
		host_thread_yield();
	}
	__atomic_store_n(&m->owner, (struct thread *)&thread_identity,
	    __ATOMIC_RELAXED);
}
void mutex_unlock(struct mutex *m)
{
	CHECK(mutex_owned(m));
	__atomic_store_n(&m->owner, NULL, __ATOMIC_RELAXED);
	__atomic_store_n(&m->locked, 0U, __ATOMIC_RELEASE);
}
void waitq_init(struct wait_queue *q, const char *n)
{ memset(q, 0, sizeof(*q)); q->name = n; }
void *kern_malloc(size_t n) { return malloc(n); }
void kern_free(void *p) { free(p); }
void clock_realtime(int64_t *s, long *n) { *s = 1; *n = 0; }
struct ucred *cred_current_ref(void) { return NULL; }
void cred_release(struct ucred *c) { (void)c; }
int vfs_access(const struct inode *i, const struct ucred *c, int f)
{ (void)i; (void)c; (void)f; return 0; }
int vfs_may_create(const struct inode *i, const struct ucred *c)
{ (void)c; return i->i_type == INODE_DIR ? 0 : ENOTDIR; }
int vfs_clear_setid_on_write(struct inode *i, const struct ucred *c)
{ (void)c; i->i_mode &= ~(S_ISUID | S_ISGID); return 0; }
int vfs_clear_setid_on_content_change(struct inode *i)
{ i->i_mode &= ~(S_ISUID | S_ISGID); return 0; }
int backing_mutation_begin_inode(struct inode *i, struct backing_mutation_guard *g)
{ (void)i; memset(g, 0, sizeof(*g)); return 0; }
int backing_mutation_begin_disk(struct disk *d, uint64_t s, uint64_t n,
	const struct backing_claim *c, struct backing_mutation_guard *g)
{ (void)d; (void)s; (void)n; (void)c; memset(g, 0, sizeof(*g)); return 0; }
void backing_mutation_end(struct backing_mutation_guard *g) { (void)g; }
int backing_claim_check_mount(struct disk *d, unsigned f)
{ (void)d; (void)f; return 0; }
int disk_open(struct disk *d) { (void)d; abort(); }
void disk_close(struct disk *d) { (void)d; abort(); }
void disk_release(struct disk *d) { (void)d; abort(); }
struct disk *disk_find(const char *name) { (void)name; abort(); }
void record_lock_inode_destroy(struct inode *i) { (void)i; }
int block_identity_resolve(const char *s, struct disk **d)
{ (void)s; (void)d; abort(); }
/* Borrowed inode pointers model directory entries independently of mount
 * objects. Matching by inode identity also exercises reconstructed aliases. */
static struct {
	struct inode *parent, *inode;
	char name[40];
} names[64];
static unsigned name_count;
static int same_inode(const struct inode *a, const struct inode *b)
{ return a == b || (a && b && a->i_mount == b->i_mount && a->i_ino == b->i_ino); }
/* Only directory I/O is modeled: production getcwd reconstructs non-root
 * attachment parents through this directory stream and actual namei. */
int file_open_resolved(const struct path *path, int flags, struct file **out)
{
	struct file *file;
	CHECK((flags & O_DIRECTORY) != 0 && path->p_inode->i_type == INODE_DIR);
	file = calloc(1, sizeof(*file)); CHECK(file != NULL);
	path_set(&file->f_path, path->p_mount, path->p_inode);
	file->f_inode = path->p_inode; *out = file; return 0;
}
int file_readdir(struct file *file, struct dirent *entry, int *eof)
{
	while ((unsigned)file->f_offset < name_count) {
		unsigned i = (unsigned)file->f_offset++;
		if (names[i].inode == NULL || !same_inode(names[i].parent, file->f_inode))
			continue;
		memset(entry, 0, sizeof(*entry));
		entry->d_ino = names[i].inode->i_ino;
		entry->d_type = names[i].inode->i_type;
		strcpy(entry->d_name, names[i].name); *eof = 0; return 0;
	}
	*eof = 1; return 0;
}
int file_close(struct file *file)
{ path_release(&file->f_path); free(file); return 0; }
static void set_name(struct inode *parent, const char *name, struct inode *inode)
{
	unsigned i;
	for (i = 0; i < name_count; i++)
		if (same_inode(names[i].parent, parent) && !strcmp(names[i].name, name))
			break;
	CHECK(i < sizeof(names) / sizeof(names[0]));
	if (i == name_count) name_count++;
	names[i].parent = parent; names[i].inode = inode;
	CHECK(strlen(name) < sizeof(names[i].name));
	strcpy(names[i].name, name);
}
static int name_matches(const char *name, const struct componentname *component)
{
	return strlen(name) == component->cn_namelen &&
	    !memcmp(name, component->cn_nameptr, component->cn_namelen);
}
static void clear_names(void)
{ memset(names, 0, sizeof(names)); name_count = 0; namecache_reset(); }
static int found_node(struct inode *node, struct inode **out)
{
	inode_ref(node);
	if (node == lookup_pause_inode &&
	    __atomic_exchange_n(&lookup_pause_once, 0U, __ATOMIC_RELAXED))
		host_gate_pause(1);
	*out = node;
	return 0;
}

static int lookup(struct inode *dir, const struct componentname *name,
	struct inode **out)
{
	struct inode *n = NULL;
	for (unsigned i = 0; i < name_count; i++)
		if (same_inode(names[i].parent, dir) && name_matches(names[i].name, name)) {
			n = names[i].inode;
			if (!n) return ENOENT;
			return found_node(n, out);
		}
	if (name->cn_namelen == 1 && name->cn_nameptr[0] == '.') n = dir;
	else if (name->cn_namelen == 2 && !memcmp(name->cn_nameptr, "..", 2)) n = dir;
	else if (name->cn_namelen == 6 && !memcmp(name->cn_nameptr, "source", 6)) n = mutation_source;
	else if (name->cn_namelen == 6 && !memcmp(name->cn_nameptr, "target", 6)) n = mutation_target;
	if (!n) return ENOENT;
	return found_node(n, out);
}
static int remove_node(struct inode *i, const struct componentname *n)
{
	__atomic_fetch_add(&mutations, 1U, __ATOMIC_RELAXED);
	host_gate_pause(__atomic_load_n(&mutation_gate, __ATOMIC_RELAXED));
	for (unsigned j = 0; j < name_count; j++)
		if (same_inode(names[j].parent, i) && name_matches(names[j].name, n))
			names[j].inode = NULL;
	return 0;
}
static int rename_node(struct inode *a, const struct componentname *an,
	struct inode *b, const struct componentname *bn, unsigned f)
{ (void)a; (void)an; (void)b; (void)bn; (void)f;
  __atomic_fetch_add(&mutations, 1U, __ATOMIC_RELAXED);
  host_gate_pause(__atomic_load_n(&mutation_gate, __ATOMIC_RELAXED)); return 0; }
static int create_node(struct inode *parent, const struct componentname *name,
	const struct inode_creation_request *request, struct inode **out)
{
	struct inode *node = inode_alloc(parent->i_mount);
	char text[40];
	CHECK(node != NULL && name->cn_namelen < sizeof(text));
	node->i_type = request->type; node->i_mode = inode_type_mode(request->type) | request->mode;
	node->i_ino = 1000 + __atomic_fetch_add(&mutations, 1U, __ATOMIC_RELAXED);
	memcpy(text, name->cn_nameptr, name->cn_namelen); text[name->cn_namelen] = 0;
	set_name(parent, text, node); *out = node; return 0;
}
static int symlink_node(struct inode *parent, const struct componentname *name,
	const char *target, const struct inode_creation_request *request, struct inode **out)
{ (void)target; return create_node(parent, name, request, out); }
static int link_node(struct inode *parent, const struct componentname *name,
	struct inode *target)
{
	char text[40];
	CHECK(name->cn_namelen < sizeof(text));
	memcpy(text, name->cn_nameptr, name->cn_namelen); text[name->cn_namelen] = 0;
	__atomic_fetch_add(&mutations, 1U, __ATOMIC_RELAXED);
	set_name(parent, text, target); return 0;
}
static const struct inode_ops ops = {
	.lookup = lookup, .unlink = remove_node, .rmdir = remove_node, .rename = rename_node,
	.create = create_node, .mkdir = create_node, .mknod = create_node,
	.symlink = symlink_node, .link = link_node,
};
struct memory_control {
	unsigned mount_gate, sync_gate, prepare_gate, mounts, destroys;
	int mount_error, sync_error, prepare_error;
};
static int mount_memory(struct mount *m)
{
	struct memory_control *control = m->m_data;
	if (control) {
		__atomic_fetch_add(&control->mounts, 1U, __ATOMIC_RELAXED);
		host_gate_pause(control->mount_gate);
		if (control->mount_error) return control->mount_error;
	}
	struct inode *n = inode_alloc(m);
	if (!n) return ENOSPC;
	n->i_type = INODE_DIR; n->i_mode = S_IFDIR | 0755;
	n->i_ino = 1; n->i_op = &ops;
	n->i_flags = INODE_ROOT | INODE_NOCACHE_CHILDREN;
	m->m_root = n; return 0;
}
static int sync_memory(struct mount *m)
{
	struct memory_control *control = m->m_data;
	__atomic_fetch_add(&syncs, 1U, __ATOMIC_RELAXED);
	if (control) { host_gate_pause(control->sync_gate); return control->sync_error; }
	return 0;
}
static int prepare_memory(struct mount *m)
{
	struct memory_control *control = m->m_data;
	if (control) { host_gate_pause(control->prepare_gate); return control->prepare_error; }
	return 0;
}
static void destroy_memory(struct mount *m)
{
	struct memory_control *control = m->m_data;
	if (control) __atomic_fetch_add(&control->destroys, 1U, __ATOMIC_RELAXED);
}
static const struct filesystem_type memory_type = {
	.fs_name = "memory", .fs_flags = FILESYSTEM_NODEV,
	.mount = mount_memory, .sync = sync_memory,
	.prepare_unmount = prepare_memory, .unmount = destroy_memory,
};

static struct inode *new_node(struct mount *mountp, ino_t ino, enum inode_type type)
{
	struct inode *node = inode_alloc(mountp);
	CHECK(node != NULL);
	node->i_ino = ino; node->i_type = type;
	node->i_mode = inode_type_mode(type) | 0755;
	node->i_flags = INODE_NOCACHE_CHILDREN; node->i_op = &ops;
	return node;
}

static void guards(struct mount *root)
{
	struct componentname source = { "source", 6, 0 }, target = { "target", 6, 0 };
	struct inode *a = inode_alloc(root), *b = inode_alloc(root);
	const unsigned flags[] = { INODE_ROOT, INODE_SWAPFILE, INODE_LOOPFILE };
	CHECK(a && b);
	a->i_type = b->i_type = INODE_DIR; a->i_ino = 2; b->i_ino = 3;
	mutation_source = a; mutation_target = b;
	for (unsigned i = 0; i < 3; i++) {
		a->i_flags = flags[i]; b->i_flags = 0;
		CHECK(inode_rmdir(root->m_root, &source) == EBUSY);
		CHECK(inode_rename(root->m_root, &source, root->m_root, &target, 0) == EBUSY);
		a->i_flags = 0; b->i_flags = flags[i];
		CHECK(inode_rename(root->m_root, &source, root->m_root, &target, 0) == EBUSY);
		a->i_type = INODE_REG; a->i_flags = flags[i];
		CHECK(inode_unlink(root->m_root, &source) == EBUSY);
		a->i_type = INODE_DIR;
	}
	CHECK(mutations == 0);
	a->i_flags = b->i_flags = 0;
	CHECK(inode_rmdir(root->m_root, &source) == 0 && mutations == 1);
	root->m_flags = MOUNT_READ_ONLY;
	CHECK(inode_rmdir(root->m_root, &source) == EROFS);
	root->m_flags = 0;
	mutation_source = mutation_target = NULL;
	inode_release(a); inode_release(b);
}

static int covered_directory_probe(struct mount *root, const struct path *root_path,
	struct cwdinfo *cwd)
{
	struct componentname source = { "source", 6, 0 };
	struct inode *covered = inode_alloc(root);
	struct path visible;
	int error;
	CHECK(covered != NULL);
	covered->i_type = INODE_DIR; covered->i_ino = 9;
	mutation_source = covered;
	CHECK(mount_at("memory", root_path, "source", 0, NULL, NULL) == 0);
	CHECK(namei_path_at(cwd, "/source", &visible) == 0);
	CHECK(visible.p_inode != covered && (visible.p_inode->i_flags & INODE_ROOT));
	path_release(&visible);
	error = inode_rmdir(root->m_root, &source);
	printf("KA-T121 covered-directory protection: rmdir=%d expected=%d callbacks=%u\n",
	    error, EBUSY, mutations);
	CHECK(unmount("/source", 0) == 0);
	mutation_source = NULL; inode_release(covered);
	return error == EBUSY && mutations == 0 ? 0 : 1;
}

static struct componentname component(const char *name)
{
	struct componentname result = { name, strlen(name), 0 };
	return result;
}

static void expect_busy_names(struct inode *parent, const char *held_name)
{
	struct componentname held = component(held_name), ordinary = component("ordinary");
	unsigned before = mutations;
	CHECK(inode_rmdir(parent, &held) == EBUSY);
	CHECK(inode_unlink(parent, &held) == EBUSY);
	CHECK(inode_rename(parent, &held, parent, &ordinary, 0) == EBUSY);
	CHECK(inode_rename(parent, &ordinary, parent, &held, 0) == EBUSY);
	CHECK(mutations == before);
}

static void creation_collisions(struct mount *root)
{
	struct componentname held = component("virtual"), free_name = component("created");
	struct inode_creation_request request = {
		.origin = INODE_CREATION_SYSTEM, .type = INODE_REG, .mode = 0644,
	};
	struct inode *node = NULL, *file = new_node(root, 41, INODE_REG);
	unsigned before = mutations;
	CHECK(inode_create(root->m_root, &held, &request, &node) == EEXIST);
	request.type = INODE_DIR;
	CHECK(inode_mkdir(root->m_root, &held, &request, &node) == EEXIST);
	request.type = INODE_FIFO;
	CHECK(inode_mknod(root->m_root, &held, &request, &node) == EEXIST);
	request.type = INODE_SYMLINK;
	CHECK(inode_symlink(root->m_root, &held, "ordinary", &request, &node) == EEXIST);
	CHECK(inode_link(root->m_root, &held, file) == EEXIST);
	CHECK(mutations == before && node == NULL);
	request.type = INODE_REG;
	CHECK(inode_create(root->m_root, &free_name, &request, &node) == 0);
	CHECK(node != NULL && mutations == before + 1);
	inode_release(node); inode_release(file);
}

static void mounted_names(struct mount *root, const struct path *root_path,
	struct cwdinfo *cwd)
{
	struct inode *covered = new_node(root, 20, INODE_DIR);
	struct inode *ordinary = new_node(root, 21, INODE_DIR);
	struct inode *other_parent = new_node(root, 22, INODE_DIR);
	struct inode *other = new_node(root, 23, INODE_DIR);
	struct inode *alias = new_node(root, root->m_root->i_ino, INODE_DIR);
	struct componentname held = component("covered"), free_name = component("other");
	struct path visible, view_path;
	struct mount *view, *child;
	unsigned before;
	set_name(root->m_root, "covered", covered);
	set_name(root->m_root, "alternate", covered);
	set_name(root->m_root, "ordinary", ordinary);
	set_name(root->m_root, "other", other);
	set_name(other_parent, "covered", other);
	CHECK(mount_at("memory", root_path, "covered", 0, NULL, NULL) == 0);
	CHECK(mount_at("memory", root_path, "virtual", 0, NULL, NULL) == 0);
	expect_busy_names(root->m_root, "covered");
	expect_busy_names(root->m_root, "alternate"); /* another spelling of covered inode */
	expect_busy_names(root->m_root, "virtual");
	expect_busy_names(alias, "covered"); /* same inode identity, distinct pointer */
	{
		struct path alias_path;
		struct dirent entry;
		unsigned cursor = 0;
		path_set(&alias_path, root, alias);
		CHECK(path_equal(&alias_path, root_path));
		CHECK(mount_lookup_child(&alias_path, &held, &visible) == 0);
		path_release(&visible);
		CHECK(mount_at("memory", &alias_path, "covered", 0, NULL, NULL) == EBUSY);
		CHECK(mount_bind_at(root_path, &alias_path, "covered", NULL) == EBUSY);
		CHECK(mount_readdir_child(&alias_path, &cursor, &entry) == 0);
		CHECK(!strcmp(entry.d_name, "covered"));
		path_release(&alias_path);
	}
	creation_collisions(root);
	CHECK(namei_path_at(cwd, "/covered", &visible) == 0);
	CHECK(visible.p_inode != covered); path_release(&visible);
	before = mutations;
	CHECK(inode_rmdir(other_parent, &held) == 0);
	CHECK(inode_rmdir(root->m_root, &free_name) == 0);
	CHECK(mutations == before + 2);
	{
		struct mount *other_mount;
		struct inode *same_number;
		CHECK(mount_at("memory", root_path, "otherfs", 0, NULL, &other_mount) == 0);
		same_number = new_node(other_mount, covered->i_ino, INODE_DIR);
		set_name(other_mount->m_root, "covered", same_number);
		CHECK(inode_rmdir(other_mount->m_root, &held) == 0);
		inode_release(same_number);
		CHECK(unmount("/otherfs", 0) == 0);
	}
	/* Bind views have separate attachment locations but share backing inodes. */
	CHECK(mount_bind_at(root_path, root_path, "view", &view) == 0);
	path_set(&view_path, view, view->m_root);
	CHECK(mount_at("memory", &view_path, "bound", 0, NULL, &child) == 0);
	expect_busy_names(root->m_root, "bound");
	CHECK(namei_path_at(cwd, "/view/bound", &visible) == 0);
	CHECK(visible.p_mount == child); path_release(&visible);
	CHECK(mount_at("memory", root_path, "bound", 0, NULL, NULL) == 0);
	CHECK(namei_path_at(cwd, "/bound", &visible) == 0);
	CHECK(visible.p_mount != child); path_release(&visible);
	CHECK(unmount("/bound", 0) == 0);
	expect_busy_names(root->m_root, "bound"); /* the other view still reserves it */
	CHECK(unmount("/view/bound", 0) == 0);
	path_release(&view_path);
	CHECK(unmount("/view", 0) == 0);
	CHECK(unmount("/covered", 0) == 0);
	CHECK(unmount("/virtual", 0) == 0);
	before = mutations;
	CHECK(inode_rmdir(root->m_root, &held) == 0 && mutations == before + 1);
	clear_names();
	inode_release(covered); inode_release(ordinary); inode_release(other_parent);
	inode_release(other); inode_release(alias);
}

static void ancestors_and_bind_sources(struct mount *root, const struct path *root_path)
{
	struct inode *ancestor = new_node(root, 50, INODE_DIR);
	struct inode *parent = new_node(root, 51, INODE_DIR);
	struct inode *ordinary = new_node(root, 52, INODE_DIR);
	struct inode *source = new_node(root, 53, INODE_DIR);
	struct path parent_path, source_path;
	struct mount *child;
	struct componentname anc = component("ancestor"), par = component("parent");
	struct componentname ord = component("ordinary"), src = component("bindsource");
	unsigned before = mutations;
	set_name(root->m_root, "ancestor", ancestor);
	set_name(root->m_root, "ordinary", ordinary);
	set_name(root->m_root, "bindsource", source);
	set_name(ancestor, "parent", parent);
	set_name(parent, "..", ancestor); set_name(ancestor, "..", root->m_root);
	set_name(source, "..", root->m_root);
	path_set(&parent_path, root, parent);
	CHECK(mount_at("memory", &parent_path, "leaf", 0, NULL, &child) == 0);
	CHECK(inode_rmdir(root->m_root, &anc) == EBUSY);
	CHECK(inode_rename(root->m_root, &anc, root->m_root, &ord, 0) == EBUSY);
	CHECK(inode_rename(root->m_root, &ord, root->m_root, &anc, 0) == EBUSY);
	CHECK(inode_rmdir(ancestor, &par) == EBUSY);
	CHECK(mutations == before);
	CHECK(!strcmp(child->m_path, "/ancestor/parent/leaf"));
	CHECK(unmount("/ancestor/parent/leaf", 0) == 0);
	path_release(&parent_path);
	path_set(&source_path, root, source);
	CHECK(mount_bind_at(&source_path, root_path, "bind", NULL) == 0);
	CHECK(inode_rmdir(root->m_root, &src) == EBUSY);
	CHECK(inode_rename(root->m_root, &src, root->m_root, &ord, 0) == EBUSY);
	CHECK(inode_rename(root->m_root, &ord, root->m_root, &src, 0) == EBUSY);
	CHECK(mutations == before);
	CHECK(unmount("/bind", 0) == 0);
	path_release(&source_path);
	CHECK(inode_rmdir(root->m_root, &src) == 0);
	CHECK(inode_rmdir(ancestor, &par) == 0);
	CHECK(inode_rmdir(root->m_root, &anc) == 0);
	CHECK(mutations == before + 3);
	clear_names();
	inode_release(ancestor); inode_release(parent); inode_release(ordinary); inode_release(source);
}

static void publication_and_rollback(struct mount *root, const struct path *root_path)
{
	struct memory_control control = { 0 };
	struct componentname held = component("held");
	struct inode *covered = new_node(root, 60, INODE_DIR);
	struct path found;
	struct mount *mounted;
	unsigned before = mount_count(), refs = refcount_load(&root->m_refs);
	unsigned covered_refs = refcount_load(&covered->i_refs);
	set_name(root->m_root, "held", covered);
	control.mount_error = EIO;
	CHECK(mount_at("memory", root_path, "held", 0, &control, NULL) == EIO);
	CHECK(mount_count() == before && refcount_load(&root->m_refs) == refs);
	CHECK(refcount_load(&covered->i_refs) == covered_refs);
	CHECK(mount_lookup_child(root_path, &held, &found) == ENOENT);
	control.mount_error = 0;
	CHECK(mount_at("memory", root_path, "held", 0, &control, &mounted) == 0);
	CHECK(mount_at("memory", root_path, "held", 0, NULL, NULL) == EBUSY);
	CHECK(mount_bind_at(root_path, root_path, "held", NULL) == EBUSY);
	CHECK(mount_count() == before + 1);
	control.sync_error = EIO;
	CHECK(unmount("/held", 0) == EIO);
	CHECK(mounted->m_state == MOUNT_STATE_LIVE && control.destroys == 0);
	CHECK(inode_rmdir(root->m_root, &held) == EBUSY);
	control.sync_error = 0; control.prepare_error = EIO;
	CHECK(unmount("/held", 0) == EIO);
	CHECK(mounted->m_state == MOUNT_STATE_LIVE && control.destroys == 0);
	CHECK(mount_lookup_child(root_path, &held, &found) == 0);
	CHECK(found.p_mount == mounted); path_release(&found);
	CHECK(mount_at("memory", root_path, "held", 0, NULL, NULL) == EBUSY);
	control.prepare_error = 0;
	CHECK(unmount("/held", 0) == 0 && control.destroys == 1);
	CHECK(mount_count() == before && refcount_load(&root->m_refs) == refs);
	CHECK(refcount_load(&covered->i_refs) == covered_refs);
	CHECK(mount_bind_at(root_path, root_path, "held", NULL) == 0);
	CHECK(mount_bind_at(root_path, root_path, "held", NULL) == EBUSY);
	CHECK(mount_at("memory", root_path, "held", 0, NULL, NULL) == EBUSY);
	CHECK(unmount("/held", 0) == 0);
	CHECK(refcount_load(&covered->i_refs) == covered_refs);
	CHECK(inode_rmdir(root->m_root, &held) == 0);
	clear_names(); inode_release(covered);
}

enum job_kind { JOB_MOUNT, JOB_BIND, JOB_RMDIR, JOB_UNLINK,
	JOB_RENAME_SOURCE, JOB_RENAME_TARGET, JOB_CREATE, JOB_UNMOUNT, JOB_LOOKUP,
	JOB_ROOT };
struct job {
	enum job_kind kind;
	const struct path *parent;
	struct memory_control *control;
	struct inode *found;
	struct mount *mounted;
	int error;
	unsigned done;
};

static void run_job(void *argument)
{
	struct job *job = argument;
	struct inode *parent = job->parent ? job->parent->p_inode : NULL;
	struct componentname held = component("held"), ordinary = component("ordinary");
	struct inode_creation_request request = {
		.origin = INODE_CREATION_SYSTEM, .type = INODE_REG, .mode = 0644,
	};
	switch (job->kind) {
	case JOB_MOUNT:
		job->error = mount_at("memory", job->parent, "held", 0, job->control, NULL); break;
	case JOB_BIND:
		job->error = mount_bind_at(job->parent, job->parent, "held", NULL); break;
	case JOB_RMDIR: job->error = inode_rmdir(parent, &held); break;
	case JOB_UNLINK: job->error = inode_unlink(parent, &held); break;
	case JOB_RENAME_SOURCE:
		job->error = inode_rename(parent, &held, parent, &ordinary, 0); break;
	case JOB_RENAME_TARGET:
		job->error = inode_rename(parent, &ordinary, parent, &held, 0); break;
	case JOB_CREATE:
		job->error = inode_create(parent, &held, &request, &job->found); break;
	case JOB_UNMOUNT: job->error = unmount("/held", 0); break;
	case JOB_LOOKUP: job->error = inode_lookup(parent, &held, &job->found); break;
	case JOB_ROOT:
		job->error = mount_root_create("memory", 0, job->control, &job->mounted); break;
	}
	__atomic_store_n(&job->done, 1U, __ATOMIC_RELEASE);
	host_gate_signal(2);
}

static void root_preparation_races(void)
{
	for (unsigned failure = 0; failure < 2; failure++) {
		struct memory_control control = { .mount_gate = 1, .mount_error = failure ? EIO : 0 };
		struct memory_control competitor = { 0 };
		struct job first = { .kind = JOB_ROOT, .control = &control };
		struct job second = { .kind = JOB_ROOT, .control = &competitor };
		struct mount *root;
		void *a, *b;
		mount_reset(); CHECK(filesystem_register(&memory_type) == 0);
		host_gate_reset(1); host_gate_reset(2);
		a = host_thread_start(run_job, &first); host_gate_wait(1);
		CHECK(mount_root_get_ref() == NULL);
		b = host_thread_start(run_job, &second); host_gate_wait(2);
		host_thread_join(b);
		CHECK(second.error == EBUSY && competitor.mounts == 0 && second.mounted == NULL);
		CHECK(mount_count() == 1);
		host_gate_release(1); host_thread_join(a);
		CHECK(first.error == (failure ? EIO : 0));
		if (failure) {
			CHECK(mount_root_get_ref() == NULL && mount_count() == 0);
			CHECK(mount_root_create("memory", 0, NULL, &root) == 0);
		} else root = first.mounted;
		CHECK(mount_root_get_ref() == root); mount_release(root);
		CHECK(mount_count() == 1);
		mount_reset();
	}
	puts("KA-T121 concurrent root preparation/rollback PASS");
}

static void threaded_admission(struct mount *root, const struct path *root_path,
	struct cwdinfo *cwd)
{
	unsigned races = 0;
	/* Each competitor is observed either waiting on a real mutex or already
	 * returning the required error while the first backend remains paused.
	 * No scheduler sleeps or timing-based 'probably blocked' assertions. */
	for (unsigned phase = 0; phase < 3; phase++) {
		for (enum job_kind kind = JOB_MOUNT; kind <= JOB_CREATE; kind++) {
			struct memory_control first_control = { 0 }, second_control = { 0 };
			struct inode *covered = new_node(root, 70, INODE_DIR);
			struct inode *ordinary = new_node(root, 71, INODE_DIR);
			struct job first = { .kind = phase ? JOB_UNMOUNT : JOB_MOUNT,
				.parent = root_path, .control = &first_control };
			struct job second = { .kind = kind, .parent = root_path,
				.control = &second_control };
			struct componentname held = component("held");
			struct path found;
			void *a, *b;
			unsigned before = mutations, mounts_before = mount_count();
			int expected = kind == JOB_CREATE ? EEXIST : EBUSY;
			set_name(root->m_root, "held", covered);
			set_name(root->m_root, "ordinary", ordinary);
			host_gate_reset(1); host_gate_reset(2);
			if (phase) {
				CHECK(mount_at("memory", root_path, "held", 0, &first_control, NULL) == 0);
				if (phase == 1) {
					first_control.sync_gate = 1; first_control.sync_error = EIO;
				} else {
					first_control.prepare_gate = 1; first_control.prepare_error = EIO;
				}
			} else first_control.mount_gate = 1;
			a = host_thread_start(run_job, &first);
			host_gate_wait(1);
			CHECK(mount_lookup_child(root_path, &held, &found) == EBUSY);
			CHECK(namei_path_at(cwd, "/held", &found) == EBUSY);
			__atomic_store_n(&watch_contention, 1U, __ATOMIC_RELAXED);
			b = host_thread_start(run_job, &second);
			host_gate_wait(2);
			CHECK(!__atomic_load_n(&second.done, __ATOMIC_ACQUIRE) || second.error == expected);
			CHECK(__atomic_load_n(&mutations, __ATOMIC_RELAXED) == before);
			CHECK(__atomic_load_n(&second_control.mounts, __ATOMIC_RELAXED) == 0);
			host_gate_release(1);
			host_thread_join(a); host_thread_join(b);
			__atomic_store_n(&watch_contention, 0U, __ATOMIC_RELAXED);
			CHECK(first.error == (phase ? EIO : 0));
			CHECK(second.error == expected && mutations == before);
			CHECK(first_control.destroys == 0);
			first_control.mount_gate = first_control.sync_gate = first_control.prepare_gate = 0;
			first_control.sync_error = first_control.prepare_error = 0;
			CHECK(unmount("/held", 0) == 0);
			CHECK(first_control.destroys == 1 && mount_count() == mounts_before);
			clear_names(); inode_release(covered); inode_release(ordinary); races++;
		}
	}
	printf("KA-T121 threaded admission: %u mount/sync-rollback/prepare-rollback races PASS\n", races);
}

static void mutation_before_mount(struct mount *root, const struct path *root_path)
{
	struct inode *covered = new_node(root, 80, INODE_DIR);
	struct memory_control control = { 0 };
	struct job first = { .kind = JOB_RMDIR, .parent = root_path };
	struct job second = { .kind = JOB_MOUNT, .parent = root_path, .control = &control };
	struct inode *missing;
	struct componentname held = component("held");
	void *a, *b;
	unsigned before = mutations;
	set_name(root->m_root, "held", covered);
	host_gate_reset(1); host_gate_reset(2);
	mutation_gate = 1;
	a = host_thread_start(run_job, &first); host_gate_wait(1);
	__atomic_store_n(&watch_contention, 1U, __ATOMIC_RELAXED);
	b = host_thread_start(run_job, &second); host_gate_wait(2);
	CHECK(!__atomic_load_n(&second.done, __ATOMIC_ACQUIRE));
	CHECK(__atomic_load_n(&control.mounts, __ATOMIC_RELAXED) == 0);
	host_gate_release(1); host_thread_join(a); host_thread_join(b);
	watch_contention = mutation_gate = 0;
	CHECK(first.error == 0 && second.error == 0 && mutations == before + 1);
	CHECK(inode_lookup(root->m_root, &held, &missing) == ENOENT);
	CHECK(unmount("/held", 0) == 0 && control.destroys == 1);
	clear_names(); inode_release(covered);
}

static void failed_preparation_releases_reservation(struct mount *root,
	const struct path *root_path)
{
	for (unsigned mutation = 0; mutation < 2; mutation++) {
		struct inode *covered = new_node(root, 85, INODE_DIR);
		struct memory_control failed = { .mount_gate = 1, .mount_error = EIO };
		struct memory_control succeeding = { 0 };
		struct job first = { .kind = JOB_MOUNT, .parent = root_path, .control = &failed };
		struct job second = { .kind = mutation ? JOB_RMDIR : JOB_MOUNT,
			.parent = root_path, .control = &succeeding };
		unsigned before = mutations, mounts_before = mount_count();
		unsigned refs = refcount_load(&root->m_refs);
		void *a, *b;
		set_name(root->m_root, "held", covered);
		host_gate_reset(1); host_gate_reset(2);
		a = host_thread_start(run_job, &first); host_gate_wait(1);
		__atomic_store_n(&watch_contention, 1U, __ATOMIC_RELAXED);
		b = host_thread_start(run_job, &second); host_gate_wait(2);
		CHECK(!__atomic_load_n(&second.done, __ATOMIC_ACQUIRE) || second.error == EBUSY);
		CHECK(__atomic_load_n(&mutations, __ATOMIC_RELAXED) == before);
		CHECK(__atomic_load_n(&succeeding.mounts, __ATOMIC_RELAXED) == 0);
		host_gate_release(1); host_thread_join(a); host_thread_join(b);
		watch_contention = 0;
		CHECK(first.error == EIO && second.error == EBUSY);
		/* PREPARING rejects this attempt immediately. A fresh attempt after
		 * the failed preparation must see the reservation fully released. */
		run_job(&second);
		CHECK(second.error == 0);
		CHECK(mutations == before + mutation);
		if (!mutation) CHECK(unmount("/held", 0) == 0 && succeeding.destroys == 1);
		CHECK(mount_count() == mounts_before && refcount_load(&root->m_refs) == refs);
		clear_names(); inode_release(covered);
	}
}

static void admitted_ordinary_mutations(struct mount *root)
{
	struct inode *directory = new_node(root, 86, INODE_DIR);
	struct inode *file = new_node(root, 87, INODE_REG);
	struct componentname a = component("ordinary"), b = component("destination");
	unsigned before = mutations;
	set_name(root->m_root, "ordinary", directory);
	/* Syscalls already own the namespace transaction; generic wrappers must
	 * join it and retain caller ownership, including error exits. */
	mount_vfs_transaction_enter(root);
	CHECK(inode_rename(root->m_root, &a, root->m_root, &b, 0) == 0);
	CHECK(mutex_owned(root->m_vfs_transaction_lock));
	CHECK(inode_rmdir(root->m_root, &a) == 0);
	CHECK(mutex_owned(root->m_vfs_transaction_lock));
	CHECK(inode_rmdir(root->m_root, &a) == ENOENT);
	CHECK(mutex_owned(root->m_vfs_transaction_lock));
	set_name(root->m_root, "ordinary", file);
	CHECK(inode_unlink(root->m_root, &a) == 0);
	CHECK(mutex_owned(root->m_vfs_transaction_lock));
	mount_vfs_transaction_leave(root);
	CHECK(mutations == before + 3);
	clear_names(); inode_release(directory); inode_release(file);
}

static void stale_lookup_publication(struct mount *root)
{
	struct inode *parent = new_node(root, 90, INODE_DIR);
	struct inode *old = new_node(root, 91, INODE_REG);
	struct inode *replacement = new_node(root, 92, INODE_REG), *found;
	struct componentname held = component("held");
	struct path parent_path;
	struct job reader = { .kind = JOB_LOOKUP };
	void *thread;
	parent->i_flags &= ~INODE_NOCACHE_CHILDREN;
	set_name(parent, "held", old);
	path_set(&parent_path, root, parent); reader.parent = &parent_path;
	lookup_pause_inode = old; lookup_pause_once = 1;
	host_gate_reset(1);
	thread = host_thread_start(run_job, &reader); host_gate_wait(1);
	CHECK(inode_unlink(parent, &held) == 0);
	set_name(parent, "held", replacement);
	host_gate_release(1); host_thread_join(thread);
	CHECK(reader.error == 0 && reader.found != NULL);
	inode_release(reader.found);
	lookup_pause_inode = NULL;
	CHECK(inode_lookup(parent, &held, &found) == 0);
	CHECK(found == replacement); inode_release(found);
	path_release(&parent_path); clear_names();
	inode_release(parent); inode_release(old); inode_release(replacement);
	puts("KA-T121 stale lookup publication race PASS");
}

struct metadata_control {
	struct inode *upper_parent;
	unsigned gate, prepared, calls, backend_calls;
	int error;
};

static int prepare_metadata(struct inode *inode)
{
	struct metadata_control *control = inode->i_data;
	struct componentname name = component("materialized");
	struct inode_creation_request request = {
		.origin = INODE_CREATION_SYSTEM, .type = INODE_DIR, .mode = 0755,
	};
	struct inode *created;
	control->calls++;
	if (control->prepared) return 0;
	CHECK(!mutex_owned(&inode->i_io_lock));
	host_gate_pause(control->gate);
	if (control->error) return control->error;
	/* Model a stacking backend which must create its upper object, invoking
	 * the real generic namespace wrapper while preparation owns no I/O lock. */
	CHECK(inode_mkdir(control->upper_parent, &name, &request, &created) == 0);
	inode_release(created); control->prepared = 1; return 0;
}

static int setattr_metadata(struct inode *inode, const struct stat *status, unsigned mask)
{
	struct metadata_control *control = inode->i_data;
	(void)status; (void)mask;
	CHECK(control->prepared && mutex_owned(&inode->i_io_lock));
	control->backend_calls++; return 0;
}

static int truncate_metadata(struct inode *inode, off_t size)
{
	struct metadata_control *control = inode->i_data;
	CHECK(control->prepared && mutex_owned(&inode->i_io_lock));
	control->backend_calls++; inode->i_size = size; return 0;
}

static const struct inode_ops metadata_ops = {
	.prepare_mutation = prepare_metadata, .setattr = setattr_metadata,
	.truncate = truncate_metadata,
};

struct metadata_job { struct inode *inode; int create, error; };
static void run_metadata(void *argument)
{
	struct metadata_job *job = argument;
	if (job->create) {
		struct ucred cred = { .euid = 1000, .egid = 1000 };
		struct inode_creation_request request;
		mount_vfs_transaction_enter(job->inode->i_mount);
		job->error = inode_creation_request_user(job->inode, &cred,
		    INODE_REG, 0644, 0, NULL, &request);
		mount_vfs_transaction_leave(job->inode->i_mount);
	} else {
		struct stat status = { .st_mode = S_IFDIR | 0700 };
		job->error = inode_setattr(job->inode, &status, INODE_ATTR_MODE);
	}
	host_gate_signal(2);
}

static void metadata_preparation_order(struct mount *root)
{
	struct inode *directory = new_node(root, 95, INODE_DIR);
	struct metadata_control control = { .upper_parent = root->m_root, .gate = 1 };
	struct metadata_job metadata = { .inode = directory }, creation = { .inode = directory, .create = 1 };
	struct stat status = { .st_mode = S_IFDIR | 0755 };
	void *a, *b;
	directory->i_data = &control; directory->i_op = &metadata_ops;
	host_gate_reset(1); host_gate_reset(2);
	a = host_thread_start(run_metadata, &metadata); host_gate_wait(1);
	b = host_thread_start(run_metadata, &creation); host_gate_wait(2);
	/* The child-creation attribute snapshot takes this same directory's I/O
	 * mutex while holding the namespace gate. It must finish before upper
	 * materialization resumes, proving there is no inverse lock ownership. */
	host_thread_join(b); CHECK(creation.error == 0);
	host_gate_release(1); host_thread_join(a);
	CHECK(metadata.error == 0 && control.prepared && control.backend_calls == 1);
	control.gate = control.prepared = 0; control.error = EIO;
	CHECK(inode_setattr(directory, &status, INODE_ATTR_MODE) == EIO);
	CHECK(control.backend_calls == 1 && (directory->i_mode & 0777) == 0700);
	CHECK(!mutex_owned(&directory->i_io_lock));
	directory->i_type = INODE_REG; directory->i_mode = S_IFREG | 0644;
	CHECK(inode_truncate(directory, 16) == EIO);
	CHECK(control.backend_calls == 1 && directory->i_size == 0);
	control.error = 0;
	CHECK(inode_truncate(directory, 16) == 0);
	CHECK(control.backend_calls == 2 && directory->i_size == 16);
	CHECK(!mutex_owned(&directory->i_io_lock));
	clear_names(); inode_release(directory);
	puts("KA-T121 metadata preparation/I/O-lock ordering PASS");
}

int main(int argc, char **argv)
{
	struct mount *root, *a, *nested;
	struct path root_path, a_path, found;
	struct cwdinfo cwd;
	struct zedbsd_mount_info entries[8];
	struct dirent entry;
	unsigned count, cursor = 0;
	char path[80];
	CHECK(INODE_ROOT == 1 && INODE_SWAPFILE == 0x10 && INODE_LOOPFILE == 0x20);
	if (argc == 1) root_preparation_races();
	mount_reset(); CHECK(filesystem_register(&memory_type) == 0);
	CHECK(mount_root_create("memory", 0, NULL, &root) == 0);
	path_set(&root_path, root, root->m_root);
	CHECK(cwdinfo_init(&cwd, &root_path) == 0);
	if (argc == 2 && !strcmp(argv[1], "--covered-directory"))
		return covered_directory_probe(root, &root_path, &cwd);
	CHECK(mount_at("memory", &root_path, "a", MOUNT_READ_ONLY, NULL, &a) == 0);
	path_set(&a_path, a, a->m_root);
	CHECK(mount_at("memory", &a_path, "nested", 0, NULL, &nested) == 0);
	CHECK(namei_path_at(&cwd, "/a/nested/..", &found) == 0);
	CHECK(path_equal(&found, &a_path)); path_release(&found);
	CHECK(namei_path_at(&cwd, "/a/..", &found) == 0);
	CHECK(path_equal(&found, &root_path)); path_release(&found);
	CHECK(namei_path_at(&cwd, "/a/nested", &found) == 0);
	CHECK(found.p_mount == nested); path_release(&found);
	CHECK(mount_bind_at(&a_path, &root_path, "alias", NULL) == 0);
	CHECK(namei_path_at(&cwd, "/alias/..", &found) == 0);
	CHECK(path_equal(&found, &root_path)); path_release(&found);
	CHECK(mount_info_snapshot(entries, 8, &count) == 0 && count == 4);
	CHECK(!strcmp(entries[2].target, "/a/nested"));
	CHECK(!strcmp(entries[3].source, "/a") && entries[3].kind == ZEDBSD_MOUNT_INFO_BIND);
	CHECK(entries[1].flags == MOUNT_READ_ONLY && entries[2].flags == 0);
	CHECK(mount_readdir_child(&root_path, &cursor, &entry) == 0 && !strcmp(entry.d_name, "a"));
	CHECK(mount_readdir_child(&root_path, &cursor, &entry) == 0 && !strcmp(entry.d_name, "alias"));
	CHECK(unmount("/a", 0) == EBUSY);
	CHECK(unmount("/a/nested", 0) == 0);
	CHECK(unmount("/alias", 0) == 0);
	path_release(&a_path);
	CHECK(unmount("/a", 0) == 0);
	CHECK(namei_path_at(&cwd, "/a", &found) == ENOENT);
	CHECK(mount("memory", "/disk1", 0, NULL) == 0); /* explicit name not blacklisted */
	CHECK(unmount("/disk1", 0) == 0);
	CHECK(fs_getcwd(&cwd, path, sizeof(path)) == 0 && !strcmp(path, "/"));
	guards(root);
	mounted_names(root, &root_path, &cwd);
	ancestors_and_bind_sources(root, &root_path);
	publication_and_rollback(root, &root_path);
	threaded_admission(root, &root_path, &cwd);
	mutation_before_mount(root, &root_path);
	failed_preparation_releases_reservation(root, &root_path);
	admitted_ordinary_mutations(root);
	stale_lookup_publication(root);
	metadata_preparation_order(root);
	CHECK(syncs >= 3);
	cwdinfo_destroy(&cwd); path_release(&root_path);
	namecache_reset();
	printf("KA-T121: %u checks PASS (production mount/namei/inode/cache)\n", checks);
	return 0;
}
