/*
 * WS001 p022: production overlay create/materialize/copy-up fault matrix.
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include <kern/file.h>
#include <kern/inode.h>
#include <kern/kmem.h>
#include <kern/mount.h>
#include <kern/namecache.h>
#include <kern/namei.h>
#include <kern/overlayfs.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE_OVERLAY_IDENTITY_MAX 128U
#define FIXTURE_OVERLAY_METADATA_MAX 128U
#define FIXTURE_OVERLAY_META_WHITEOUT 0x01U

struct fixture_overlay_identity {
	ino_t ino;
	uint8_t state;
	char path[ZEDBSD_PATH_MAX];
};

struct fixture_overlay_metadata {
	uint8_t used;
	uint8_t flags;
	uint64_t sequence;
	char path[ZEDBSD_PATH_MAX];
};

struct fixture_overlay_mount_state {
	struct path upper_root;
	struct path lower_root;
	unsigned flags;
	ino_t next_ino;
	struct fixture_overlay_identity identities[FIXTURE_OVERLAY_IDENTITY_MAX];
	struct fixture_overlay_metadata metadata[FIXTURE_OVERLAY_METADATA_MAX];
	struct file *journal[2];
	unsigned active_slot;
	unsigned next_sector;
	uint64_t epoch;
	uint64_t sequence;
	uint32_t journal_generation;
	uint16_t temp_counter;
	struct mutex copy_up_lock;
};

struct fixture_overlay_inode_info {
	struct path upper;
	struct path lower;
	unsigned identity_index;
	char path[ZEDBSD_PATH_MAX];
};

static unsigned checks;
static struct mutex transaction_lock;
static unsigned transaction_entries;
static int hidden_lower;
static struct fixture_overlay_mount_state state;
static struct fixture_overlay_inode_info root_info;
static struct fixture_overlay_inode_info sub_info;
static struct fixture_overlay_inode_info file_info;
static struct mount overlay_mount;
static struct mount upper_mount;
static struct mount lower_mount;
static struct inode overlay_root;
static struct inode overlay_sub;
static struct inode overlay_file;
static struct inode upper_root;
static struct inode lower_root;
static struct inode lower_sub;
static struct inode lower_file;
static struct inode materialized_sub;
static struct inode new_inode;
static struct inode temp_inode;
static struct file journal_file;
static struct file source_file;
static struct file destination_file;

static int new_entry;
static int sub_entry;
static int temp_entry;
static int final_entry;
static int socket_entry;
static int new_allocation;
static int sub_allocation;
static int temp_allocation;
static int create_error;
static int mkdir_error;
static int rename_error;
static int unlink_error;
static int rmdir_error;
static int lookup_error;
static int file_sync_error;
static int mount_results[8];
static unsigned mount_result_count;
static unsigned mount_calls;
static unsigned cache_removes;
static unsigned create_calls;
static unsigned mkdir_calls;
static unsigned unlink_calls;
static unsigned rmdir_calls;
static unsigned rename_calls;
static unsigned mknod_calls;
static unsigned journal_writes;
static unsigned file_closes;
static struct inode *last_overlay_inode;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr,                                        \
			    "ws001-p022 overlay fault: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

#define CHECK_ERROR(expression, wanted)                                    \
	do {                                                                 \
		int result_ = (expression);                                  \
		int wanted_ = (wanted);                                      \
		checks++;                                                    \
		if (result_ != wanted_) {                                    \
			fprintf(stderr,                                        \
			    "ws001-p022 overlay fault: failed at %s:%d: "      \
			    "got %d wanted %d\n", __FILE__, __LINE__,         \
			    result_, wanted_);                                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

extern int ws001_overlay_prepare_mutation(struct inode *);
extern int ws001_overlay_rename(struct inode *, const struct componentname *, struct inode *, const struct componentname *, unsigned);
extern int ws001_overlay_remove(struct inode *, const struct componentname *, int);
extern int ws001_overlay_create(struct inode *, const struct componentname *,
    const struct inode_creation_request *, struct inode **);
extern int ws001_overlay_mknod(struct inode *, const struct componentname *,
    const struct inode_creation_request *, struct inode **);
extern int ws001_overlay_ensure_upper_dir(struct inode *);
extern int ws001_overlay_copy_up_regular(struct inode *);
extern struct inode *ws001_overlay_alloc_inode(struct mount *);

static int
component_is(const struct componentname *component, const char *text)
{
	return component != NULL && component->cn_namelen == strlen(text) &&
	    memcmp(component->cn_nameptr, text, component->cn_namelen) == 0;
}

void
mutex_lock(struct mutex *mutex)
{
	CHECK(mutex != NULL && !mutex->locked);
	mutex->locked = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(mutex != NULL && mutex->locked);
	mutex->locked = 0;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)mutex;
	(void)rank;
	(void)name;
	return 0;
}

int
mutex_owned(struct mutex *mutex)
{
	return mutex != NULL && mutex->locked;
}

void
path_init(struct path *path)
{
	memset(path, 0, sizeof(*path));
}

void
path_set(struct path *path, struct mount *mount, struct inode *inode)
{
	path->p_mount = mount;
	path->p_inode = inode;
}

void
path_release(struct path *path)
{
	path_init(path);
}

void
inode_ref(struct inode *inode)
{
	CHECK(inode != NULL);
}

void
inode_release(struct inode *inode)
{
	CHECK(inode != NULL);
}

struct inode *
inode_alloc(struct mount *mount)
{
	struct inode *inode = ws001_overlay_alloc_inode(mount);

	if (inode != NULL)
		inode->i_mount = mount;
	last_overlay_inode = inode;
	return inode;
}

int
inode_get(struct mount *mount, ino_t ino, struct inode **result)
{
	(void)mount;
	(void)ino;
	if (result != NULL)
		*result = NULL;
	return ENOENT;
}

int
inode_lookup(struct inode *directory, const struct componentname *component,
    struct inode **result)
{
	if (lookup_error != 0)
		return lookup_error;
	if (hidden_lower && directory == &lower_root && component_is(component, "new")) {
		*result = &lower_file;
		return 0;
	}
	if (directory == &overlay_root && component_is(component, "sub")) {
		*result = &overlay_sub;
		return 0;
	}
	if ((directory == &upper_root || directory == &materialized_sub) &&
	    component_is(component, "new") && new_entry) {
		*result = &new_inode;
		return 0;
	}
	if (directory == &upper_root && component_is(component, "sub") &&
	    sub_entry) {
		*result = &materialized_sub;
		return 0;
	}
	if (directory == &materialized_sub && component_is(component, "file") &&
	    final_entry) {
		*result = &temp_inode;
		return 0;
	}
	if ((directory == &upper_root || directory == &materialized_sub) &&
	    component_is(component, "sock") && socket_entry) {
		*result = &temp_inode;
		return 0;
	}
	return ENOENT;
}

int
inode_create(struct inode *directory, const struct componentname *component,
    const struct inode_creation_request *request, struct inode **result)
{
	(void)request;
	CHECK(directory == &upper_root || directory == &materialized_sub);
	create_calls++;
	if (create_error != 0)
		return create_error;
	if (component_is(component, "new")) {
		new_entry = 1;
		new_allocation = 1;
		new_inode.i_linkcount = 1U;
		*result = &new_inode;
		return 0;
	}
	CHECK(component->cn_namelen == 10U);
	temp_entry = 1;
	temp_allocation = 1;
	temp_inode.i_linkcount = 1U;
	*result = &temp_inode;
	return 0;
}

int
inode_mkdir(struct inode *directory, const struct componentname *component,
    const struct inode_creation_request *request, struct inode **result)
{
	(void)request;
	CHECK(directory == &upper_root);
	CHECK(component_is(component, "sub"));
	mkdir_calls++;
	if (mkdir_error != 0)
		return mkdir_error;
	sub_entry = 1;
	sub_allocation = 1;
	materialized_sub.i_linkcount = 2U;
	upper_root.i_linkcount++;
	*result = &materialized_sub;
	return 0;
}

int
inode_unlink(struct inode *directory, const struct componentname *component)
{
	CHECK(directory == &upper_root || directory == &materialized_sub);
	unlink_calls++;
	if (unlink_error != 0)
		return unlink_error;
	if (component_is(component, "new")) {
		new_entry = 0;
		new_allocation = 0;
		new_inode.i_linkcount = 0U;
	} else if (component_is(component, "file")) {
		final_entry = 0;
		temp_allocation = 0;
		temp_inode.i_linkcount = 0U;
	} else if (component_is(component, "sock")) {
		socket_entry = 0;
		temp_allocation = 0;
		temp_inode.i_linkcount = 0U;
	} else {
		CHECK(component->cn_namelen == 10U);
		temp_entry = 0;
		temp_allocation = 0;
		temp_inode.i_linkcount = 0U;
	}
	return 0;
}

int
inode_rmdir(struct inode *directory, const struct componentname *component)
{
	CHECK(directory == &upper_root);
	CHECK(component_is(component, "sub"));
	rmdir_calls++;
	if (rmdir_error != 0)
		return rmdir_error;
	if (temp_entry || final_entry || socket_entry)
		return ENOTEMPTY;
	sub_entry = 0;
	sub_allocation = 0;
	materialized_sub.i_linkcount = 0U;
	upper_root.i_linkcount--;
	return 0;
}

int
inode_rename(struct inode *old_directory,
    const struct componentname *old_name, struct inode *new_directory,
    const struct componentname *new_name, unsigned flags)
{
	CHECK(old_directory == &upper_root ||
	    old_directory == &materialized_sub);
	CHECK(new_directory == old_directory);
	if (component_is(old_name, "new") && component_is(new_name, "moved")) {
		CHECK(new_entry && flags == 0);
		new_entry = 0;
		rename_calls++;
		return 0;
	}
	CHECK(old_name->cn_namelen == 10U);
	CHECK(component_is(new_name, "file") ||
	    component_is(new_name, "sock"));
	CHECK(flags == 0U);
	rename_calls++;
	if (rename_error != 0)
		return rename_error;
	temp_entry = 0;
	if (component_is(new_name, "file"))
		final_entry = 1;
	else
		socket_entry = 1;
	return 0;
}

int
inode_creation_request_preserve(const struct inode *source,
    struct inode_creation_request *request)
{
	CHECK(source != NULL);
	CHECK(request != NULL);
	memset(request, 0, sizeof(*request));
	request->origin = INODE_CREATION_PRESERVE;
	request->type = source->i_type;
	request->mode = source->i_mode;
	request->uid = source->i_uid;
	request->gid = source->i_gid;
	request->source = source;
	return 0;
}

int
inode_creation_prepare(struct inode *parent, struct inode *inode,
    const struct inode_creation_request *request)
{
	(void)parent;
	CHECK(inode == &temp_inode);
	CHECK(request != NULL);
	inode->i_mode = request->mode;
	inode->i_uid = request->uid;
	inode->i_gid = request->gid;
	return 0;
}

int
inode_getattr(struct inode *inode, struct stat *status)
{
	(void)inode;
	(void)status;
	return EOPNOTSUPP;
}

int
inode_setattr(struct inode *inode, const struct stat *status, unsigned mask)
{
	(void)inode;
	(void)status;
	(void)mask;
	return EOPNOTSUPP;
}

int
inode_mknod(struct inode *directory, const struct componentname *component,
    const struct inode_creation_request *request, struct inode **result)
{
	CHECK(directory == &upper_root || directory == &materialized_sub);
	CHECK(component->cn_namelen == 10U);
	CHECK(request != NULL);
	CHECK(request->type == INODE_SOCKET);
	CHECK(result != NULL);
	mknod_calls++;
	temp_entry = 1;
	temp_allocation = 1;
	temp_inode.i_type = INODE_SOCKET;
	temp_inode.i_linkcount = 1U;
	temp_inode.i_mode = request->mode;
	temp_inode.i_uid = request->uid;
	temp_inode.i_gid = request->gid;
	temp_inode.i_special = request->special;
	*result = &temp_inode;
	return 0;
}

int
inode_symlink(struct inode *directory, const struct componentname *component,
    const char *target, const struct inode_creation_request *request,
    struct inode **result)
{
	(void)directory;
	(void)component;
	(void)target;
	(void)request;
	(void)result;
	return EOPNOTSUPP;
}

ssize_t
inode_readlink(struct inode *inode, char *buffer, size_t capacity)
{
	(void)inode;
	(void)buffer;
	(void)capacity;
	return -EOPNOTSUPP;
}

int
inode_truncate_transaction(struct inode *inode,
    const struct inode_truncate_request *request,
    struct inode_truncate_result *result)
{
	(void)inode;
	(void)request;
	(void)result;
	return EOPNOTSUPP;
}

void inode_dir_changed(struct inode *directory)
{ directory->i_dirseq++; }

void
namecache_remove(struct inode *directory,
    const struct componentname *component)
{
	CHECK(directory == &overlay_root || directory == &overlay_sub);
	CHECK(component != NULL);
	cache_removes++;
}

int
mount_sync(struct mount *mount)
{
	CHECK(mount == &upper_mount);
	CHECK(mount_calls < sizeof(mount_results) / sizeof(mount_results[0]));
	return mount_calls < mount_result_count ?
	    mount_results[mount_calls++] : (mount_calls++, 0);
}

void
mount_vfs_transaction_enter(struct mount *mount)
{
	CHECK(!overlay_file.i_io_lock.locked);
	transaction_entries++;
	mutex_lock(mount->m_vfs_transaction_lock);
}

int mount_vfs_transaction_join(struct mount *mount)
{
	if (mutex_owned(mount->m_vfs_transaction_lock)) return 0;
	mount_vfs_transaction_enter(mount);
	return 1;
}

void
mount_vfs_transaction_leave(struct mount *mount)
{
	mutex_unlock(mount->m_vfs_transaction_lock);
}

int
file_open_resolved(const struct path *path, int flags, struct file **result)
{
	CHECK(path != NULL);
	if (path->p_inode == &lower_file) {
		CHECK(flags == O_RDONLY);
		*result = &source_file;
	} else {
		CHECK(path->p_inode == &temp_inode);
		CHECK(flags == O_RDWR);
		*result = &destination_file;
	}
	return 0;
}

ssize_t
file_pread(struct file *file, void *buffer, size_t length, off_t offset)
{
	CHECK(file == &source_file);
	CHECK(offset == 0);
	memset(buffer, 0x5a, length);
	return (ssize_t)length;
}

ssize_t
file_pwrite(struct file *file, const void *buffer, size_t length, off_t offset)
{
	CHECK(buffer != NULL);
	if (file == &journal_file) {
		CHECK(length == 512U);
		journal_writes++;
		return (ssize_t)length;
	}
	CHECK(file == &destination_file);
	CHECK(offset == 0);
	return (ssize_t)length;
}

ssize_t
file_pread_internal(struct file *file, void *buffer, size_t length,
    off_t offset, unsigned internal_flags)
{
	(void)internal_flags;
	return file_pread(file, buffer, length, offset);
}

ssize_t
file_pwrite_internal(struct file *file, const void *buffer, size_t length,
    off_t offset, unsigned internal_flags)
{
	(void)internal_flags;
	return file_pwrite(file, buffer, length, offset);
}

ssize_t
file_pwrite_internal_cred(struct file *file, const void *buffer,
    size_t length, off_t offset, unsigned internal_flags,
    const struct ucred *credential)
{
	(void)credential;
	return file_pwrite_internal(file, buffer, length, offset, internal_flags);
}

int
file_readdir(struct file *file, struct dirent *entry, int *eof)
{
	(void)file;
	(void)entry;
	(void)eof;
	return EOPNOTSUPP;
}

struct inode *
file_vm_inode(struct file *file)
{
	(void)file;
	return NULL;
}

int
file_fsync(struct file *file)
{
	CHECK(file == &journal_file || file == &destination_file);
	return file_sync_error;
}

int
file_close(struct file *file)
{
	CHECK(file == &source_file || file == &destination_file);
	file_closes++;
	return 0;
}

void *
kern_malloc(size_t size)
{
	return malloc(size);
}

void *
kern_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

void
kern_free(void *pointer)
{
	free(pointer);
}

int
mount_statvfs(struct mount *mount, struct statvfs *status)
{
	(void)mount;
	(void)status;
	return EOPNOTSUPP;
}

static void
reset_fixture(void)
{
	memset(&state, 0, sizeof(state));
	memset(&root_info, 0, sizeof(root_info));
	memset(&sub_info, 0, sizeof(sub_info));
	memset(&file_info, 0, sizeof(file_info));
	memset(&overlay_mount, 0, sizeof(overlay_mount));
	memset(&upper_mount, 0, sizeof(upper_mount));
	memset(&lower_mount, 0, sizeof(lower_mount));
	memset(&overlay_root, 0, sizeof(overlay_root));
	memset(&overlay_sub, 0, sizeof(overlay_sub));
	memset(&overlay_file, 0, sizeof(overlay_file));
	memset(&upper_root, 0, sizeof(upper_root));
	memset(&lower_root, 0, sizeof(lower_root));
	memset(&lower_sub, 0, sizeof(lower_sub));
	memset(&lower_file, 0, sizeof(lower_file));
	memset(&materialized_sub, 0, sizeof(materialized_sub));
	memset(&new_inode, 0, sizeof(new_inode));
	memset(&temp_inode, 0, sizeof(temp_inode));
	memset(&journal_file, 0, sizeof(journal_file));
	memset(mount_results, 0, sizeof(mount_results));
	state.flags = OVERLAY_READ_WRITE;
	state.next_ino = 100U;
	state.journal[0] = &journal_file;
	state.next_sector = 1U;
	state.epoch = 1U;
	state.upper_root.p_mount = &upper_mount;
	state.upper_root.p_inode = &upper_root;
	state.lower_root.p_mount = &lower_mount;
	state.lower_root.p_inode = &lower_root;
	memset(&transaction_lock, 0, sizeof(transaction_lock));
	transaction_entries = 0;
	hidden_lower = 0;
	overlay_mount.m_vfs_transaction_lock = &transaction_lock;
	overlay_mount.m_data = &state;
	overlay_mount.m_root = &overlay_root;
	overlay_root.i_mount = &overlay_mount;
	overlay_root.i_type = INODE_DIR;
	overlay_root.i_data = &root_info;
	root_info.upper = state.upper_root;
	root_info.lower = state.lower_root;
	overlay_sub.i_mount = &overlay_mount;
	overlay_sub.i_type = INODE_DIR;
	overlay_sub.i_data = &sub_info;
	sub_info.lower.p_mount = &lower_mount;
	sub_info.lower.p_inode = &lower_sub;
	strcpy(sub_info.path, "sub");
	overlay_file.i_mount = &overlay_mount;
	overlay_file.i_type = INODE_REG;
	overlay_file.i_data = &file_info;
	file_info.lower.p_mount = &lower_mount;
	file_info.lower.p_inode = &lower_file;
	strcpy(file_info.path, "sub/file");
	upper_root.i_type = INODE_DIR;
	upper_root.i_linkcount = 2U;
	lower_root.i_type = INODE_DIR;
	lower_sub.i_type = INODE_DIR;
	lower_sub.i_mode = 0750U;
	lower_file.i_type = INODE_REG;
	lower_file.i_size = 8;
	lower_file.i_mode = 0640U;
	lower_file.i_uid = 41U;
	lower_file.i_gid = 42U;
	materialized_sub.i_type = INODE_DIR;
	materialized_sub.i_linkcount = 0U;
	new_inode.i_type = INODE_REG;
	new_inode.i_linkcount = 0U;
	temp_inode.i_type = INODE_REG;
	temp_inode.i_linkcount = 0U;
	new_entry = 0;
	sub_entry = 0;
	temp_entry = 0;
	final_entry = 0;
	socket_entry = 0;
	new_allocation = 0;
	sub_allocation = 0;
	temp_allocation = 0;
	create_error = 0;
	mkdir_error = 0;
	rename_error = 0;
	unlink_error = 0;
	rmdir_error = 0;
	lookup_error = 0;
	file_sync_error = 0;
	mount_result_count = 0;
	mount_calls = 0;
	cache_removes = 0;
	create_calls = 0;
	mkdir_calls = 0;
	unlink_calls = 0;
	rmdir_calls = 0;
	rename_calls = 0;
	mknod_calls = 0;
	journal_writes = 0;
	file_closes = 0;
	last_overlay_inode = NULL;
}

static struct inode_creation_request
regular_request(void)
{
	struct inode_creation_request request;

	memset(&request, 0, sizeof(request));
	request.origin = INODE_CREATION_USER;
	request.type = INODE_REG;
	request.mode = 0600U;
	request.uid = 51U;
	request.gid = 52U;
	return request;
}

static struct inode_creation_request
socket_request(void *endpoint)
{
	struct inode_creation_request request;

	memset(&request, 0, sizeof(request));
	request.origin = INODE_CREATION_USER;
	request.type = INODE_SOCKET;
	request.mode = 0770U;
	request.uid = 61U;
	request.gid = 62U;
	request.special = endpoint;
	return request;
}

static void
check_no_metadata(void)
{
	unsigned i;

	for (i = 0; i < FIXTURE_OVERLAY_METADATA_MAX; i++)
		CHECK(state.metadata[i].used == 0U);
}

static void
check_no_upper_allocations(void)
{
	CHECK(new_allocation == 0);
	CHECK(sub_allocation == 0);
	CHECK(temp_allocation == 0);
	CHECK(new_inode.i_linkcount == 0U);
	CHECK(materialized_sub.i_linkcount == 0U);
	CHECK(temp_inode.i_linkcount == 0U);
	CHECK(upper_root.i_linkcount == 2U);
}

static void
test_existing_upper_create_failures(void)
{
	struct componentname name = { "new", 3U, COMPONENT_LAST };
	struct inode_creation_request request = regular_request();
	struct inode *result;

	reset_fixture();
	create_error = EIO;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_create(&overlay_root, &name, &request,
	    &result), EIO);
	CHECK(result == NULL);
	CHECK(create_calls == 1U);
	CHECK(new_entry == 0);
	CHECK(cache_removes == 0U);
	check_no_upper_allocations();
	check_no_metadata();

	/* A namespace rollback that cannot unlink the created leaf is
	 * quarantined.  The cleanup errno, not the triggering sync error, is
	 * authoritative. */
	reset_fixture();
	mount_results[0] = EIO;
	mount_results[1] = 0;
	mount_result_count = 2U;
	unlink_error = ENOSPC;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_create(&overlay_root, &name, &request,
	    &result), ENOSPC);
	CHECK(result == NULL);
	CHECK(new_entry == 1);
	CHECK(unlink_calls == 1U);
	CHECK(mount_calls == 2U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(cache_removes == 1U);
	CHECK(new_allocation == 1);
	CHECK(new_inode.i_linkcount == 1U);

	reset_fixture();
	mount_results[0] = EIO;
	mount_results[1] = 0;
	mount_result_count = 2U;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_create(&overlay_root, &name, &request,
	    &result), EIO);
	CHECK(result == NULL);
	CHECK(new_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(mount_calls == 2U);
	CHECK(cache_removes == 1U);
	CHECK(state.flags == OVERLAY_READ_WRITE);
	check_no_upper_allocations();
	check_no_metadata();

	reset_fixture();
	state.metadata[0].used = 1U;
	state.metadata[0].flags = FIXTURE_OVERLAY_META_WHITEOUT;
	strcpy(state.metadata[0].path, "new");
	file_sync_error = ENOSPC;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_create(&overlay_root, &name, &request,
	    &result), ENOSPC);
	CHECK(result == NULL);
	CHECK(new_entry == 0);
	CHECK(journal_writes == 1U);
	CHECK(state.metadata[0].used == 1U);
	CHECK(state.metadata[0].flags == FIXTURE_OVERLAY_META_WHITEOUT);
	CHECK(state.sequence == 0U);
	CHECK(state.next_sector == 1U);
	CHECK(cache_removes == 1U);
	check_no_upper_allocations();

	reset_fixture();
	mount_results[0] = EIO;
	mount_results[1] = ENOSPC;
	mount_result_count = 2U;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_create(&overlay_root, &name, &request,
	    &result), ENOSPC);
	CHECK(result == NULL);
	CHECK(new_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(cache_removes == 1U);
	check_no_upper_allocations();
	check_no_metadata();
}

static void
test_lower_only_materialization_failures(void)
{
	reset_fixture();
	mkdir_error = EIO;
	CHECK_ERROR(ws001_overlay_ensure_upper_dir(&overlay_sub), EIO);
	CHECK(mkdir_calls == 1U);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 0U);
	check_no_upper_allocations();

	reset_fixture();
	mount_results[0] = EIO;
	mount_results[1] = 0;
	mount_result_count = 2U;
	CHECK_ERROR(ws001_overlay_ensure_upper_dir(&overlay_sub), EIO);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 2U);
	CHECK(state.flags == OVERLAY_READ_WRITE);
	check_no_upper_allocations();

	reset_fixture();
	mount_results[0] = EIO;
	mount_results[1] = ENOSPC;
	mount_result_count = 2U;
	CHECK_ERROR(ws001_overlay_ensure_upper_dir(&overlay_sub), ENOSPC);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 1U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	check_no_upper_allocations();
	check_no_metadata();

	reset_fixture();
	mount_results[0] = EIO;
	mount_results[1] = 0;
	mount_result_count = 2U;
	rmdir_error = ENOSPC;
	CHECK_ERROR(ws001_overlay_ensure_upper_dir(&overlay_sub), ENOSPC);
	CHECK(sub_entry == 1);
	CHECK(sub_info.upper.p_inode == &materialized_sub);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 2U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(sub_allocation == 1);
	CHECK(materialized_sub.i_linkcount == 2U);
	CHECK(upper_root.i_linkcount == 3U);
}

static void
test_copy_up_failures(void)
{
	reset_fixture();
	create_error = EIO;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), EIO);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(temp_entry == 0);
	CHECK(final_entry == 0);
	CHECK(file_info.upper.p_inode == NULL);
	CHECK(unlink_calls == 0U);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 2U);
	CHECK(cache_removes == 0U);
	check_no_upper_allocations();
	check_no_metadata();

	reset_fixture();
	file_sync_error = EIO;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), EIO);
	CHECK(temp_entry == 0);
	CHECK(final_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(file_info.upper.p_inode == NULL);
	CHECK(state.flags == OVERLAY_READ_WRITE);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 3U);
	CHECK(cache_removes == 0U);
	check_no_upper_allocations();
	check_no_metadata();

	reset_fixture();
	file_sync_error = EIO;
	unlink_error = ENOSPC;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), ENOSPC);
	CHECK(temp_entry == 1);
	CHECK(final_entry == 0);
	CHECK(sub_entry == 1);
	CHECK(sub_info.upper.p_inode == &materialized_sub);
	CHECK(file_info.upper.p_inode == NULL);
	CHECK(unlink_calls == 1U);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 3U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(cache_removes == 0U);
	CHECK(sub_allocation == 1);
	CHECK(temp_allocation == 1);
	CHECK(materialized_sub.i_linkcount == 2U);
	CHECK(temp_inode.i_linkcount == 1U);
	CHECK(upper_root.i_linkcount == 3U);
	check_no_metadata();

	reset_fixture();
	file_sync_error = EIO;
	mount_results[0] = 0;
	mount_results[1] = ENOSPC;
	mount_result_count = 2U;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), ENOSPC);
	CHECK(temp_entry == 0);
	CHECK(final_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(file_info.upper.p_inode == NULL);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 3U);
	CHECK(cache_removes == 0U);
	check_no_upper_allocations();
	check_no_metadata();

	reset_fixture();
	rename_error = EBUSY;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), EBUSY);
	CHECK(temp_entry == 0);
	CHECK(final_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(file_info.upper.p_inode == NULL);
	CHECK(state.flags == OVERLAY_READ_WRITE);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 3U);
	CHECK(cache_removes == 0U);
	check_no_upper_allocations();
	check_no_metadata();

	reset_fixture();
	mount_results[0] = 0;
	mount_results[1] = EIO;
	mount_results[2] = 0;
	mount_result_count = 3U;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), EIO);
	CHECK(temp_entry == 0);
	CHECK(final_entry == 0);
	CHECK(file_info.upper.p_inode == NULL);
	CHECK(state.flags == OVERLAY_READ_WRITE);
	CHECK(unlink_calls == 1U);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 4U);
	check_no_upper_allocations();
	CHECK(state.metadata[0].used == 0U);

	reset_fixture();
	mount_results[0] = 0;
	mount_results[1] = EIO;
	mount_results[2] = ENOSPC;
	mount_result_count = 3U;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), ENOSPC);
	CHECK(temp_entry == 0);
	CHECK(final_entry == 0);
	CHECK(file_info.upper.p_inode == NULL);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(unlink_calls == 1U);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 4U);
	check_no_upper_allocations();

	/* If rollback cannot remove the renamed object, it is already a complete
	 * copy.  Keep it authoritative while freezing the overlay. */
	reset_fixture();
	mount_results[0] = 0;
	mount_results[1] = EIO;
	mount_result_count = 2U;
	unlink_error = ENOSPC;
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), ENOSPC);
	CHECK(temp_entry == 0);
	CHECK(final_entry == 1);
	CHECK(file_info.upper.p_inode == &temp_inode);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(unlink_calls == 1U);
	CHECK(sub_entry == 1);
	CHECK(sub_info.upper.p_inode == &materialized_sub);
	CHECK(rmdir_calls == 0U);
	CHECK(mount_calls == 3U);
	CHECK(sub_allocation == 1);
	CHECK(temp_allocation == 1);
	CHECK(materialized_sub.i_linkcount == 2U);
	CHECK(temp_inode.i_linkcount == 1U);
	CHECK(upper_root.i_linkcount == 3U);
}

static void
test_socket_temporary_cleanup_failures(void)
{
	struct componentname name = { "sock", 4U, COMPONENT_LAST };
	struct inode_creation_request request;
	struct inode *result;
	struct inode *prepared;
	int endpoint;

	reset_fixture();
	request = socket_request(&endpoint);
	rename_error = EIO;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_mknod(&overlay_root, &name, &request,
	    &result), EIO);
	prepared = last_overlay_inode;
	CHECK(result == NULL);
	CHECK(mknod_calls == 1U);
	CHECK(temp_entry == 0);
	CHECK(socket_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(mount_calls == 1U);
	CHECK(cache_removes == 0U);
	CHECK(state.flags == OVERLAY_READ_WRITE);
	CHECK(temp_inode.i_special == NULL);
	CHECK(prepared != NULL);
	CHECK(prepared->i_special == NULL);
	CHECK((prepared->i_flags & INODE_DEAD) != 0U);
	check_no_upper_allocations();
	check_no_metadata();

	reset_fixture();
	request = socket_request(&endpoint);
	rename_error = EIO;
	unlink_error = ENOSPC;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_mknod(&overlay_root, &name, &request,
	    &result), ENOSPC);
	prepared = last_overlay_inode;
	CHECK(result == NULL);
	CHECK(temp_entry == 1);
	CHECK(socket_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(mount_calls == 1U);
	CHECK(cache_removes == 0U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(temp_inode.i_special == NULL);
	CHECK(prepared != NULL);
	CHECK(prepared->i_special == NULL);
	CHECK((prepared->i_flags & INODE_DEAD) != 0U);
	CHECK(temp_allocation == 1);
	CHECK(temp_inode.i_linkcount == 1U);
	check_no_metadata();

	reset_fixture();
	request = socket_request(&endpoint);
	rename_error = EIO;
	mount_results[0] = ENOSPC;
	mount_result_count = 1U;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_mknod(&overlay_root, &name, &request,
	    &result), ENOSPC);
	prepared = last_overlay_inode;
	CHECK(result == NULL);
	CHECK(temp_entry == 0);
	CHECK(socket_entry == 0);
	CHECK(unlink_calls == 1U);
	CHECK(mount_calls == 1U);
	CHECK(cache_removes == 0U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(temp_inode.i_special == NULL);
	CHECK(prepared != NULL);
	CHECK(prepared->i_special == NULL);
	CHECK((prepared->i_flags & INODE_DEAD) != 0U);
	check_no_upper_allocations();
	check_no_metadata();

	/* The final rename has published the upper namespace name, but the
	 * durability sync in overlay_finish_new fails.  A successful final-name
	 * rollback and its sync remove both the socket and the provisional parent. */
	reset_fixture();
	request = socket_request(&endpoint);
	mount_results[0] = 0;
	mount_results[1] = EIO;
	mount_results[2] = 0;
	mount_results[3] = 0;
	mount_result_count = 4U;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_mknod(&overlay_sub, &name, &request,
	    &result), EIO);
	prepared = last_overlay_inode;
	CHECK(result == NULL);
	CHECK(temp_entry == 0);
	CHECK(socket_entry == 0);
	CHECK(sub_entry == 0);
	CHECK(sub_info.upper.p_inode == NULL);
	CHECK(unlink_calls == 1U);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 4U);
	CHECK(cache_removes == 1U);
	CHECK(state.flags == OVERLAY_READ_WRITE);
	CHECK(temp_inode.i_special == NULL);
	CHECK(prepared != NULL);
	CHECK(prepared->i_special == NULL);
	CHECK((prepared->i_flags & INODE_DEAD) != 0U);
	check_no_upper_allocations();
	check_no_metadata();

	/* If final-name rollback itself fails, the complete but endpoint-free
	 * upper socket and its parent remain authoritative under a read-only
	 * quarantine.  The first cleanup errno is preserved. */
	reset_fixture();
	request = socket_request(&endpoint);
	mount_results[0] = 0;
	mount_results[1] = EIO;
	mount_results[2] = 0;
	mount_results[3] = 0;
	mount_result_count = 4U;
	unlink_error = ENOSPC;
	result = (struct inode *)(uintptr_t)1U;
	CHECK_ERROR(ws001_overlay_mknod(&overlay_sub, &name, &request,
	    &result), ENOSPC);
	prepared = last_overlay_inode;
	CHECK(result == NULL);
	CHECK(temp_entry == 0);
	CHECK(socket_entry == 1);
	CHECK(sub_entry == 1);
	CHECK(sub_info.upper.p_inode == &materialized_sub);
	CHECK(unlink_calls == 1U);
	CHECK(rmdir_calls == 1U);
	CHECK(mount_calls == 4U);
	CHECK(cache_removes == 1U);
	CHECK(state.flags == OVERLAY_READ_ONLY);
	CHECK(temp_inode.i_special == NULL);
	CHECK(prepared != NULL);
	CHECK(prepared->i_special == NULL);
	CHECK((prepared->i_flags & INODE_DEAD) != 0U);
	CHECK(sub_allocation == 1);
	CHECK(temp_allocation == 1);
	CHECK(materialized_sub.i_linkcount == 2U);
	CHECK(temp_inode.i_linkcount == 1U);
	CHECK(upper_root.i_linkcount == 3U);
	check_no_metadata();
}

static void
functional_regressions(void)
{
	struct componentname name = { .cn_nameptr = "new", .cn_namelen = 3 };
	unsigned before;
	reset_fixture();
	CHECK_ERROR(ws001_overlay_prepare_mutation(&overlay_file), 0);
	CHECK(file_info.upper.p_inode == &temp_inode);
	before = transaction_entries;
	mutex_lock(&overlay_file.i_io_lock);
	CHECK_ERROR(ws001_overlay_prepare_mutation(&overlay_file), 0);
	CHECK_ERROR(ws001_overlay_copy_up_regular(&overlay_file), 0);
	CHECK(transaction_entries == before);
	mutex_unlock(&overlay_file.i_io_lock);

	for (int hidden = 0; hidden != 2; hidden++) {
		struct componentname moved = { .cn_nameptr = "moved", .cn_namelen = 5 };
		reset_fixture();
		new_entry = new_allocation = 1;
		new_inode.i_linkcount = 1;
		hidden_lower = hidden;
		mount_result_count = 1;
		mount_results[0] = EIO;
		CHECK_ERROR(ws001_overlay_rename(&overlay_root, &name,
		    &overlay_root, &moved, 0), EIO);
		CHECK(!new_entry && rename_calls == 1);
		CHECK(overlay_root.i_dirseq != 0 && cache_removes == 2);
		if (hidden) CHECK(journal_writes != 0);
		CHECK_ERROR(ws001_overlay_remove(&overlay_root, &name, 0), ENOENT);
	}
	for (int hidden = 0; hidden != 2; hidden++) {
		reset_fixture();
		new_entry = new_allocation = 1;
		new_inode.i_linkcount = 1;
		hidden_lower = hidden;
		/* Whiteout persistence, when needed, precedes the upper unlink. */
		mount_result_count = 1;
		mount_results[mount_result_count - 1] = EIO;
		CHECK_ERROR(ws001_overlay_remove(&overlay_root, &name, 0), EIO);
		CHECK(!new_entry);
		CHECK(cache_removes == 1);
		CHECK(overlay_root.i_dirseq != 0);
		CHECK(last_overlay_inode != NULL);
		CHECK((last_overlay_inode->i_flags & INODE_DEAD) != 0);
		if (hidden) CHECK(journal_writes != 0);
		CHECK_ERROR(ws001_overlay_remove(&overlay_root, &name, 0), ENOENT);
		/* Production overlay inodes use a static pool in this fixture. */
	}
}

int
main(void)
{
	functional_regressions();
	test_existing_upper_create_failures();
	test_lower_only_materialization_failures();
	test_copy_up_failures();
	test_socket_temporary_cleanup_failures();
	printf("ws001-p022 overlay create/copy-up fault matrix: PASS "
	    "(%u checks)\n", checks);
	return EXIT_SUCCESS;
}
