/* SWAP-T008: production control facade with a deterministic VFS resolver. */
#include <kern/disk.h>
#include <kern/inode.h>
#include <kern/lock.h>
#include <kern/mount.h>
#include <kern/swap-control.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct kern_swap_source_set sources;
static struct disk file_disk;
static struct disk raw_disk;
static struct disk root_disk;
static struct mount file_mount;
static struct inode file_inode;
static struct inode rebound_inode;
static unsigned char test_thread_storage;
static int pending_signal;
enum path_race_mode {
	PATH_RACE_NONE = 0,
	PATH_RACE_MISSING,
	PATH_RACE_REBOUND,
};
static enum path_race_mode path_race;
static unsigned path_race_resolution;
static unsigned file_prepare_count;
static unsigned raw_prepare_count;
static unsigned source_destroy_count;
static unsigned runtime_add_count;
static unsigned path_release_count;
static unsigned disk_release_count;

unsigned long
spin_lock_irqsave(struct spinlock *lock)
{
	(void)lock;
	return 0;
}

void
spin_unlock_irqrestore(struct spinlock *lock, unsigned long state)
{
	(void)lock;
	(void)state;
}

struct thread *
thread_current(void)
{
	return (struct thread *)&test_thread_storage;
}

int
signal_pending_unblocked(const struct thread *thread)
{
	assert(thread == (struct thread *)&test_thread_storage);
	return pending_signal;
}

void
path_init(struct path *path)
{
	memset(path, 0, sizeof(*path));
}

void
path_release(struct path *path)
{
	assert(path != NULL && path->p_inode != NULL);
	path_release_count++;
	path_init(path);
}

void
disk_release(struct disk *disk)
{
	assert(disk == &raw_disk || disk == &root_disk);
	disk_release_count++;
}

static int
fake_read(void *data, uint32_t slot, void *page)
{
	(void)data;
	(void)slot;
	(void)page;
	return 0;
}

static int
fake_write(void *data, uint32_t slot, const void *page)
{
	(void)data;
	(void)slot;
	(void)page;
	return 0;
}

static void
fake_destroy(void *data)
{
	(void)data;
}

static const struct swap_backend_ops fake_ops = {
	.read_page = fake_read,
	.write_page = fake_write,
	.destroy = fake_destroy,
};

void
kern_swap_source_init(struct kern_swap_source *source)
{
	if (source != NULL)
		memset(source, 0, sizeof(*source));
}

void
kern_swap_source_destroy(struct kern_swap_source *source)
{
	if (source != NULL && source->ops != NULL)
		source_destroy_count++;
	kern_swap_source_init(source);
}

int
kern_swap_source_set_diagnostic(struct kern_swap_source *source,
	const char *diagnostic)
{
	size_t length = strlen(diagnostic);

	if (length == 0 || length > KERN_SWAP_SOURCE_TEXT_MAX)
		return EINVAL;
	memcpy(source->diagnostic, diagnostic, length + 1U);
	return 0;
}

int
kern_swap_source_prepare_file(const struct path *path, unsigned parameter,
	struct kern_swap_source *source)
{
	static int file_cookie;

	assert(path->p_inode == &file_inode);
	file_prepare_count++;
	kern_swap_source_init(source);
	source->ops = &fake_ops;
	source->data = &file_cookie;
	source->identity_disk = path->p_mount->m_disk;
	source->identity_inode = path->p_inode;
	source->slot_count = 31;
	source->header_version = 2;
	source->uuid[0] = 0x12;
	memcpy(source->label, "fixture", 8U);
	source->parameter_index = parameter;
	return 0;
}

int
kern_swap_source_prepare_raw(struct disk *disk, unsigned parameter,
	struct kern_swap_source *source)
{
	static int raw_cookie;

	assert(disk == &raw_disk);
	raw_prepare_count++;
	kern_swap_source_init(source);
	source->ops = &fake_ops;
	source->data = &raw_cookie;
	source->identity_disk = disk;
	source->slot_count = 63;
	source->header_version = 1;
	source->parameter_index = parameter;
	return 0;
}

int
kern_swap_source_set_find_identity(const struct kern_swap_source_set *set,
	struct disk *disk, struct inode *inode, unsigned *source_id)
{
	unsigned id;

	for (id = 0; id < KERN_SWAP_SOURCE_COUNT; id++) {
		const struct kern_swap_source *source = &set->range[id].source;

		if (source->ops != NULL && source->identity_disk == disk &&
		    source->identity_inode == inode) {
			*source_id = id;
			return 0;
		}
	}
	return ENOENT;
}

int
kern_swap_source_set_runtime_add(struct kern_swap_source_set *set,
	struct kern_swap_source *source, unsigned *source_id)
{
	unsigned id;

	runtime_add_count++;
	for (id = 0; id < KERN_SWAP_SOURCE_COUNT; id++)
		if (set->range[id].source.ops == NULL)
			break;
	if (id == KERN_SWAP_SOURCE_COUNT)
		return ENOSPC;
	set->range[id].source = *source;
	set->range[id].source.parameter_index = id;
	set->backend.source[id].state = SWAP_SOURCE_STATE_ACTIVE;
	set->count++;
	kern_swap_source_init(source);
	if (source_id != NULL)
		*source_id = id;
	return 0;
}

int
kern_swap_source_set_runtime_remove_cancelable(struct kern_swap_source_set *set,
	unsigned source_id, kern_swap_source_cancel_fn cancel, void *argument)
{
	int error;

	if (source_id >= KERN_SWAP_SOURCE_COUNT ||
	    set->range[source_id].source.ops == NULL)
		return ENOENT;
	set->backend.source[source_id].state = SWAP_SOURCE_STATE_DRAINING;
	error = cancel != NULL ? cancel(argument) : 0;
	if (error != 0) {
		set->backend.source[source_id].state = SWAP_SOURCE_STATE_ACTIVE;
		return error;
	}
	kern_swap_source_init(&set->range[source_id].source);
	memset(&set->backend.source[source_id], 0,
	    sizeof(set->backend.source[source_id]));
	set->count--;
	return 0;
}

int
kern_swap_source_set_snapshot(struct kern_swap_source_set *set,
	unsigned source_id, struct kern_swap_source_snapshot *snapshot)
{
	const struct kern_swap_source *source;

	if (source_id >= KERN_SWAP_SOURCE_COUNT)
		return EINVAL;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->source_id = source_id;
	source = &set->range[source_id].source;
	if (source->ops == NULL) {
		snapshot->state = SWAP_SOURCE_STATE_INACTIVE;
		return 0;
	}
	snapshot->state = set->backend.source[source_id].state;
	snapshot->header_version = source->header_version;
	snapshot->total_pages = source->slot_count;
	snapshot->used_pages = 3;
	memcpy(snapshot->uuid, source->uuid, sizeof(snapshot->uuid));
	memcpy(snapshot->label, source->label, sizeof(snapshot->label));
	memcpy(snapshot->diagnostic, source->diagnostic,
	    sizeof(snapshot->diagnostic));
	return 0;
}

static int
resolve_path(void *context, const char *selector, struct path *result)
{
	(void)context;
	if (strcmp(selector, "/swap") != 0 &&
	    strcmp(selector, "boot0:swap") != 0)
		return ENOENT;
	if (path_race != PATH_RACE_NONE && ++path_race_resolution == 2U) {
		if (path_race == PATH_RACE_MISSING)
			return ENOENT;
		result->p_mount = &file_mount;
		result->p_inode = &rebound_inode;
		return 0;
	}
	result->p_mount = &file_mount;
	result->p_inode = &file_inode;
	return 0;
}

static void
test_add_lookup_races(void)
{
	unsigned prepare_baseline = file_prepare_count;
	unsigned destroy_baseline = source_destroy_count;
	unsigned add_baseline = runtime_add_count;
	unsigned release_baseline = path_release_count;

	path_race = PATH_RACE_MISSING;
	path_race_resolution = 0;
	assert(kern_swap_control_add("/swap") == EAGAIN);
	assert(file_prepare_count == prepare_baseline + 1U);
	assert(source_destroy_count == destroy_baseline + 1U);
	assert(path_release_count == release_baseline + 1U);
	assert(runtime_add_count == add_baseline && sources.count == 0U);

	path_race = PATH_RACE_REBOUND;
	path_race_resolution = 0;
	assert(kern_swap_control_add("/swap") == EAGAIN);
	assert(file_prepare_count == prepare_baseline + 2U);
	assert(source_destroy_count == destroy_baseline + 2U);
	assert(path_release_count == release_baseline + 3U);
	assert(runtime_add_count == add_baseline && sources.count == 0U);

	path_race = PATH_RACE_NONE;
	path_race_resolution = 0;
}

static int
resolve_disk(void *context, const char *selector, struct disk **result)
{
	(void)context;
	if (strcmp(selector, "/dev/sda2") == 0 ||
	    strcmp(selector, "UUID=RAW") == 0)
		*result = &raw_disk;
	else if (strcmp(selector, "/dev/root") == 0)
		*result = &root_disk;
	else
		return ENOENT;
	return 0;
}

static int
validate_raw(void *context, struct disk *disk)
{
	(void)context;
	return disk == &root_disk ? EBUSY : 0;
}

int
main(void)
{
	static const struct kern_swap_control_resolver_ops resolver = {
		.resolve_path = resolve_path,
		.resolve_disk = resolve_disk,
		.validate_raw = validate_raw,
	};
	struct kern_swap_control_registration registration = {
		.sources = &sources,
		.resolver = &resolver,
	};
	struct kern_swap_control_source_info info;
	char too_long[KERN_SWAP_SOURCE_TEXT_MAX + 2U];

	memset(&sources, 0, sizeof(sources));
	memset(&file_disk, 0, sizeof(file_disk));
	memset(&raw_disk, 0, sizeof(raw_disk));
	memset(&root_disk, 0, sizeof(root_disk));
	memset(&file_mount, 0, sizeof(file_mount));
	memset(&file_inode, 0, sizeof(file_inode));
	memset(&rebound_inode, 0, sizeof(rebound_inode));
	file_disk.d_dev = 1;
	raw_disk.d_dev = 2;
	root_disk.d_dev = 3;
	file_mount.m_disk = &file_disk;
	file_inode.i_mount = &file_mount;
	file_inode.i_ino = 100;
	rebound_inode.i_mount = &file_mount;
	rebound_inode.i_ino = 101;
	sources.active = 1;

	assert(kern_swap_control_register(&registration) == 0);
	assert(kern_swap_control_register(&registration) == EBUSY);
	assert(kern_swap_control_add("") == EINVAL);
	memset(too_long, 'x', sizeof(too_long));
	too_long[sizeof(too_long) - 1U] = '\0';
	assert(kern_swap_control_add(too_long) == EINVAL);
	test_add_lookup_races();

	assert(kern_swap_control_add("/swap") == 0);
	assert(file_prepare_count == 3 && path_release_count == 5);
	assert(kern_swap_control_get(0, &info) == 0);
	assert(info.state == SWAP_SOURCE_STATE_ACTIVE);
	assert(info.header_version == 2 && info.total_pages == 31 &&
	    info.used_pages == 3 && info.uuid[0] == 0x12);
	assert(strcmp(info.label, "fixture") == 0);
	assert(strcmp(info.source, "/swap") == 0);
	assert(kern_swap_control_add("boot0:swap") == EEXIST);
	assert(file_prepare_count == 3 && path_release_count == 6);

	pending_signal = 1;
	assert(kern_swap_control_remove("boot0:swap") == EINTR);
	assert(kern_swap_control_get(0, &info) == 0 &&
	    info.state == SWAP_SOURCE_STATE_ACTIVE);
	pending_signal = 0;
	assert(kern_swap_control_remove("boot0:swap") == 0);
	assert(kern_swap_control_get(0, &info) == 0 &&
	    info.state == SWAP_SOURCE_STATE_INACTIVE);

	assert(kern_swap_control_add("/dev/root") == EBUSY);
	assert(raw_prepare_count == 0);
	assert(disk_release_count == 1);
	assert(kern_swap_control_add("/dev/sda2") == 0);
	assert(raw_prepare_count == 1);
	/* Initial and post-claim raw-selector resolutions are both released. */
	assert(disk_release_count == 3);
	assert(kern_swap_control_add("UUID=RAW") == EEXIST);
	assert(raw_prepare_count == 1);
	assert(disk_release_count == 4);
	assert(kern_swap_control_get(0, &info) == 0 &&
	    info.header_version == 1 && info.total_pages == 63 &&
	    strcmp(info.source, "/dev/sda2") == 0);
	assert(kern_swap_control_remove("UUID=RAW") == 0);
	assert(disk_release_count == 5);
	assert(kern_swap_control_remove("UUID=RAW") == ENOENT);
	assert(disk_release_count == 6);
	assert(kern_swap_control_get(KERN_SWAP_SOURCE_COUNT, &info) == EINVAL);

	puts("SWAP-T008: runtime swap control facade: PASS");
	return 0;
}
