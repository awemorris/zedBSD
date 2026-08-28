/* WS016 SWAP-T007 bootN runtime-lifetime regression fixture. */
#include <kern/boot.h>
#include <kern/inode.h>
#include <kern/namei.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct mount mounts[KERN_BOOT_SOURCE_SLOT_COUNT];
static struct inode inodes[KERN_BOOT_SOURCE_SLOT_COUNT];
static struct disk disks[KERN_BOOT_SOURCE_SLOT_COUNT];
static unsigned unmounts;
static unsigned private_lookups;
static unsigned root_lookups;

void
path_init(struct path *path)
{
	memset(path, 0, sizeof(*path));
}

void
path_set(struct path *path, struct mount *mountp, struct inode *inode)
{
	path->p_mount = mountp;
	path->p_inode = inode;
}

void
path_release(struct path *path)
{
	path_init(path);
}

int
unmount_private(struct mount *mountp)
{
	assert(mountp != NULL);
	mountp->m_state = MOUNT_STATE_DEAD;
	unmounts++;
	return 0;
}

int
mount_private_lookup(struct mount *mountp, const char *relative,
		     struct path *result)
{
	assert(mountp != NULL);
	assert(!strcmp(relative, "swapfile"));
	private_lookups++;
	path_set(result, mountp, mountp->m_root);
	return 0;
}

int
mount_private_promote_root(struct mount *mountp, struct mount **result)
{
	assert(mountp != NULL);
	assert((mountp->m_internal_flags & MOUNT_PRIVATE_INTERNAL) != 0);
	mountp->m_internal_flags &= ~MOUNT_PRIVATE_INTERNAL;
	if (result != NULL)
		*result = mountp;
	return 0;
}

int
cwdinfo_init(struct cwdinfo *context, const struct path *root)
{
	memset(context, 0, sizeof(*context));
	context->root = *root;
	context->cwd = *root;
	return 0;
}

void
cwdinfo_destroy(struct cwdinfo *context)
{
	(void)context;
}

int
namei_path_at(struct cwdinfo *context, const char *relative,
	      struct path *result)
{
	assert(context != NULL);
	assert(!strcmp(relative, "swapfile"));
	root_lookups++;
	path_set(result, context->root.p_mount, context->root.p_inode);
	return 0;
}

static void
slot_init(struct kern_boot_source_context *context, unsigned slot)
{
	struct kern_boot_source_slot *source = &context->slot[slot];

	memset(&mounts[slot], 0, sizeof(mounts[slot]));
	memset(&inodes[slot], 0, sizeof(inodes[slot]));
	memset(&disks[slot], 0, sizeof(disks[slot]));
	mounts[slot].m_state = MOUNT_STATE_LIVE;
	mounts[slot].m_internal_flags = MOUNT_PRIVATE_INTERNAL;
	mounts[slot].m_root = &inodes[slot];
	mounts[slot].m_disk = &disks[slot];
	inodes[slot].i_mount = &mounts[slot];
	source->disk = &disks[slot];
	source->mount = &mounts[slot];
	source->runtime_mount = &mounts[slot];
	source->configured = 1U;
}

static void
test_retention_and_private_lookup(void)
{
	struct kern_boot_source_context context;
	struct path path;

	kern_boot_source_context_init(&context);
	slot_init(&context, 0U);
	slot_init(&context, 3U);
	assert(kern_boot_source_runtime_lookup(&context, "boot0:swapfile",
	    &path) == ENXIO);
	assert(kern_boot_source_publish_runtime(&context) == EINVAL);
	assert(kern_boot_source_retain_configured(&context) == 0);
	assert(context.slot[0].retained);
	assert(context.slot[3].retained);
	assert(kern_boot_source_release_unused(&context) == 0);
	assert(unmounts == 0U);
	assert(kern_boot_source_publish_runtime(&context) == 0);
	assert(kern_boot_source_runtime_lookup(&context, "boot3:swapfile",
	    &path) == 0);
	assert(path.p_mount == &mounts[3]);
	assert(private_lookups == 1U);
	assert(kern_boot_source_runtime_lookup(&context, "boot2:swapfile",
	    &path) == ENOENT);
	assert(kern_boot_source_runtime_lookup(&context, "relative-swapfile",
	    &path) == EINVAL);
	/* Publication transfers the context to system lifetime. */
	assert(kern_boot_source_context_destroy(&context) == EBUSY);
}

static void
test_promoted_root_lookup(void)
{
	struct kern_boot_source_context context;
	struct mount *root;
	struct path path;

	kern_boot_source_context_init(&context);
	slot_init(&context, 1U);
	assert(kern_boot_source_retain_configured(&context) == 0);
	assert(kern_boot_source_promote_root(&context, 1U, &root) == 0);
	assert(root != NULL);
	assert(context.slot[1].mount == NULL);
	assert(context.slot[1].runtime_mount == root);
	assert(context.slot[1].disk == &disks[1]);
	assert(kern_boot_source_publish_runtime(&context) == 0);
	assert(kern_boot_source_runtime_lookup(&context, "boot1:swapfile",
	    &path) == 0);
	assert(path.p_mount == root);
	assert(root_lookups == 1U);
}

int
main(void)
{
	test_retention_and_private_lookup();
	test_promoted_root_lookup();
	puts("boot-source runtime tests passed");
	return 0;
}
