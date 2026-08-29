/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/vfs.h"
#include "kern/boot.h"
#include "kern/disk.h"
#include "kern/block-identity.h"
#include "kern/fat-vfs.h"
#include "kern/file.h"
#include "kern/ufs1.h"
#include "kern/ufs2.h"
#include "kern/mount.h"
#include "kern/partition.h"
#include "kern/platform.h"
#include "kern/process.h"
#include "kern/cdev.h"
#include "kern/console-device.h"
#include "kern/input-device.h"
#if CONFIG_DRIVER_GRAPHICS_DEVICE
#include "kern/graphics-device.h"
#endif
#include "kern/system-device.h"
#include "kern/devfs.h"
#include "kern/tmpfs.h"
#include "kern/overlayfs.h"
#include "kern/loop.h"
#include "kern/klog.h"
#include "kern/inode.h"
#include "kern/swap-boot.h"
#include "kern/swap-control.h"
#include "kern/swap-source.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <string.h>

#define PHYSICAL_DISK_MAX 4U
#define VFS_HIGH __attribute__((section(".hightext")))
#define VFS_LOG(...)                                                           \
	do {                                                                   \
		hal_printf(__VA_ARGS__);                                       \
		kern_logf(__VA_ARGS__);                                        \
	} while (0)

#if !defined(HAL_ARCH_I386) && !defined(HAL_ARCH_AMD64)
#define VFS_LEGACY_NULL_AUTOROOT 1
#endif

#if defined(HAL_ARCH_ARM64)
#define LEGACY_ROOTFS_IMAGE_PRIMARY "/rootfs.img"
#define LEGACY_ROOTFS_IMAGE_UNIFIED "/rootfs.rp4"
#define LEGACY_DATA_IMAGE "/data.img"
#endif

struct cwdinfo kern_cwdinfo __attribute__((section(".vfs_bss")));
static struct kern_boot_source_context boot_sources
	__attribute__((section(".vfs_bss")));
static struct kern_swap_source_set swap_sources
	__attribute__((section(".vfs_bss")));

struct vfs_swap_control_context {
	struct kern_boot_source_context *boot_sources;
	/* Owned system-lifetime reference; NULL for an overlay root. */
	struct disk *native_root;
};

static struct vfs_swap_control_context swap_control_context
	__attribute__((section(".vfs_bss")));

static int
vfs_swap_resolve_path(void *opaque, const char *selector,
		      struct path *result)
{
	struct vfs_swap_control_context *context = opaque;

	if (context == NULL || selector == NULL || result == NULL)
		return EINVAL;
	if (selector[0] == '/')
		return namei_path_at(&kern_cwdinfo, selector, result);
	return kern_boot_source_runtime_lookup(context->boot_sources, selector,
	    result);
}

static int
vfs_swap_resolve_disk(void *opaque, const char *selector,
		      struct disk **result)
{
	int error;

	(void)opaque;
	if (result == NULL)
		return EINVAL;
	*result = NULL;
	error = kern_boot_source_selector_validate(selector);
	if (error != 0)
		return error;
	return block_identity_resolve(selector, result);
}

struct vfs_disk_range {
	struct disk *leaf;
	uint64_t first;
	uint64_t last;
};

static int
vfs_disk_range_resolve(struct disk *disk, struct vfs_disk_range *range)
{
	struct disk *first_leaf, *last_leaf;
	uint64_t first, last;
	int error;

	if (disk == NULL || range == NULL || disk->d_block_count == 0)
		return EINVAL;
	error = disk_resolve_range(disk, 0, 1, &first_leaf, &first);
	if (error != 0)
		return error;
	error = disk_resolve_range(disk, disk->d_block_count - 1U, 1,
	    &last_leaf, &last);
	if (error != 0)
		return error;
	if (first_leaf != last_leaf || last < first)
		return EIO;
	range->leaf = first_leaf;
	range->first = first;
	range->last = last;
	return 0;
}

static int
vfs_swap_validate_raw(void *opaque, struct disk *candidate)
{
	struct vfs_swap_control_context *context = opaque;
	struct vfs_disk_range root, source;
	int error;

	if (context == NULL || candidate == NULL)
		return EINVAL;
	if (context->native_root == NULL)
		return 0;
	error = vfs_disk_range_resolve(context->native_root, &root);
	if (error != 0)
		return error;
	error = vfs_disk_range_resolve(candidate, &source);
	if (error != 0)
		return error;
	if ((root.leaf == source.leaf || root.leaf->d_dev == source.leaf->d_dev) &&
	    root.first <= source.last && source.first <= root.last)
		return EEXIST;
	return 0;
}

static const struct kern_swap_control_resolver_ops vfs_swap_resolver = {
	.resolve_path = vfs_swap_resolve_path,
	.resolve_disk = vfs_swap_resolve_disk,
	.validate_raw = vfs_swap_validate_raw,
};

static int
vfs_fail(const char *stage, int error)
{
	VFS_LOG("vfs: %s failed (error %d)\n", stage, error);
	return error;
}

static int
disk_name(unsigned number, char name[NAME_MAX + 1U])
{
	unsigned at = 4;
	if (number == 0 || number > 99)
		return EINVAL;
	memcpy(name, "disk", 4);
	if (number >= 10)
		name[at++] = (char)('0' + number / 10);
	name[at++] = (char)('0' + number % 10);
	name[at] = '\0';
	return 0;
}

static int
vfs_ensure_root_directory(const struct path *root, const char *name,
			  mode_t mode)
{
	struct componentname component;
	struct inode *inode = NULL;
	int error;
	component.cn_nameptr = name;
	component.cn_namelen = strlen(name);
	component.cn_flags = COMPONENT_LAST;
	error = inode_lookup(root->p_inode, &component, &inode);
	if (error == 0) {
		error = inode->i_type == INODE_DIR ? 0 : ENOTDIR;
		inode_release(inode);
		return error;
	}
	if (error != ENOENT)
		return error;
	error = inode_mkdir(root->p_inode, &component, mode, &inode);
	if (inode != NULL)
		inode_release(inode);
	return error;
}

#if defined(VFS_LEGACY_NULL_AUTOROOT)
static VFS_HIGH int
ufs1_root_marker_matches(struct disk *disk, int *matches)
{
	static const char expected[] = "zedBSD ufs1 root v1\n";
	struct mount *mountp = NULL;
	struct path marker;
	struct file *file = NULL;
	char value[sizeof(expected)];
	ssize_t count;
	int error;

	*matches = 0;
	path_init(&marker);
	error = mount_private("ufs1", disk, MOUNT_READ_ONLY, NULL, &mountp);
	if (error == EOPNOTSUPP || error == EINVAL || error == EROFS)
		return 0;
	if (error != 0)
		return error;
	error = mount_private_lookup(mountp, "etc/zedbsd-root", &marker);
	if (error == ENOENT || error == ENOTDIR) {
		error = 0;
		goto out;
	}
	if (error != 0)
		goto out;
	error = file_open_resolved(&marker, O_RDONLY, &file);
	if (error != 0)
		goto out;
	count = file_read(file, value, sizeof(value));
	if (count < 0) {
		error = (int)-count;
		goto out;
	}
	*matches = count == (ssize_t)(sizeof(expected) - 1U) &&
		   memcmp(value, expected, sizeof(expected) - 1U) == 0;
out:
	if (file != NULL)
		(void)file_close(file);
	path_release(&marker);
	if (unmount_private(mountp) != 0 && error == 0)
		error = EBUSY;
	return error;
}

#if defined(HAL_ARCH_ARM64)
struct vfs_legacy_overlay_setup {
	struct path lower_root;
	struct path upper_root;
	struct disk *lower_loop;
	struct disk *upper_loop;
	struct mount *boot_mount;
	struct mount *lower_mount;
	struct mount *upper_mount;
};

static void
vfs_legacy_overlay_setup_init(struct vfs_legacy_overlay_setup *setup)
{
	memset(setup, 0, sizeof(*setup));
	path_init(&setup->lower_root);
	path_init(&setup->upper_root);
}

static int
vfs_legacy_overlay_setup_cleanup(struct vfs_legacy_overlay_setup *setup)
{
	int error, first_error = 0;

	path_release(&setup->upper_root);
	path_release(&setup->lower_root);
	if (setup->upper_mount != NULL) {
		error = unmount_private(setup->upper_mount);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->upper_mount = NULL;
	}
	if (setup->lower_mount != NULL) {
		error = unmount_private(setup->lower_mount);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->lower_mount = NULL;
	}
	if (setup->upper_loop != NULL) {
		error = loop_detach(setup->upper_loop);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->upper_loop = NULL;
	}
	if (setup->lower_loop != NULL) {
		error = loop_detach(setup->lower_loop);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->lower_loop = NULL;
	}
	if (setup->boot_mount != NULL) {
		error = unmount_private(setup->boot_mount);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->boot_mount = NULL;
	}
	return first_error;
}

static VFS_HIGH int
vfs_mount_legacy_arm_overlay(struct disk *boot_partition,
			     struct mount **root_out)
{
	struct vfs_legacy_overlay_setup setup;
	struct overlay_mount_args args;
	struct path boot_root;
	const char *stage = "mount legacy boot partition";
	int cleanup_error, error;

	vfs_legacy_overlay_setup_init(&setup);
	path_init(&boot_root);
	error = mount_private("auto", boot_partition, 0, NULL,
	    &setup.boot_mount);
	if (error != 0)
		goto fail;
	path_set(&boot_root, setup.boot_mount, setup.boot_mount->m_root);
	stage = "attach legacy rootfs image";
	error = loop_attach_path(&boot_root, LEGACY_ROOTFS_IMAGE_PRIMARY,
	    LOOP_READ_ONLY, &setup.lower_loop);
	if (error == ENOENT)
		error = loop_attach_path(&boot_root, LEGACY_ROOTFS_IMAGE_UNIFIED,
		    LOOP_READ_ONLY, &setup.lower_loop);
	if (error == ENOENT && setup.lower_loop == NULL) {
		path_release(&boot_root);
		cleanup_error = vfs_legacy_overlay_setup_cleanup(&setup);
		if (cleanup_error != 0)
			return vfs_fail("release legacy boot mount", cleanup_error);
		return ENOENT;
	}
	if (error != 0)
		goto fail;
	stage = "attach legacy data image";
	error = loop_attach_path(&boot_root, LEGACY_DATA_IMAGE, LOOP_READ_WRITE,
	    &setup.upper_loop);
	path_release(&boot_root);
	if (error != 0)
		goto fail;

	VFS_LOG("vfs: %s <- legacy rootfs image (private, read-only)\n",
	    setup.lower_loop->d_name);
	VFS_LOG("vfs: %s <- legacy data image (private, read-write)\n",
	    setup.upper_loop->d_name);
	stage = "mount legacy rootfs image";
	error = mount_private("auto", setup.lower_loop, MOUNT_READ_ONLY, NULL,
	    &setup.lower_mount);
	if (error != 0)
		goto fail;
	stage = "mount legacy data image";
	error = mount_private("auto", setup.upper_loop, 0, NULL,
	    &setup.upper_mount);
	if (error != 0)
		goto fail;
	path_set(&setup.lower_root, setup.lower_mount,
	    setup.lower_mount->m_root);
	path_set(&setup.upper_root, setup.upper_mount,
	    setup.upper_mount->m_root);
	memset(&args, 0, sizeof(args));
	args.upper = setup.upper_root;
	args.lower = setup.lower_root;
	args.flags = OVERLAY_READ_WRITE;
	stage = "mount legacy root overlay";
	error = mount_root_create("overlay", 0, &args, root_out);
	if (error != 0)
		goto fail;
	VFS_LOG("vfs: root=legacy-overlay lower=%s upper=%s\n",
	    setup.lower_loop->d_name, setup.upper_loop->d_name);
	path_release(&setup.upper_root);
	path_release(&setup.lower_root);
	return 0;

fail:
	path_release(&boot_root);
	cleanup_error = vfs_legacy_overlay_setup_cleanup(&setup);
	if (cleanup_error != 0)
		VFS_LOG("vfs: %s cleanup failed (error %d)\n", stage,
		    cleanup_error);
	return vfs_fail(stage, error);
}
#endif

static VFS_HIGH int
vfs_mount_legacy_root(struct disk *boot_partition,
		      struct disk *boot_physical, struct disk **root_disk_out,
		      struct mount **root_out)
{
	struct disk *root_partition = NULL;
	struct fat_mount_args args;
	unsigned index;
	int error;

	*root_disk_out = NULL;
	*root_out = NULL;
	if (boot_partition == NULL)
		return vfs_fail("find loader boot partition", ENXIO);
	for (index = 0; index < partition_count(); index++) {
		const struct partition *partition = partition_at(index);
		int matches = 0;

		if (partition == NULL || partition->p_disk == NULL ||
		    partition->p_disk == boot_partition ||
		    partition->p_parent != boot_physical)
			continue;
		error = ufs1_root_marker_matches(partition->p_disk, &matches);
		if (error != 0)
			return vfs_fail("inspect legacy UFS1 root candidate", error);
		if (!matches)
			continue;
		if (root_partition != NULL)
			return vfs_fail("ambiguous legacy UFS1 root candidates",
			    EINVAL);
		root_partition = partition->p_disk;
	}

#if defined(HAL_ARCH_ARM64)
	if (root_partition == NULL) {
		error = vfs_mount_legacy_arm_overlay(boot_partition, root_out);
		if (error == 0)
			return 0;
		if (error != ENOENT)
			return error;
	}
#endif

	if (root_partition != NULL) {
		disk_ref(root_partition);
		args.fspec = root_partition->d_name;
		error = mount_root_create("auto", 0, &args, root_out);
		if (error != 0) {
			disk_release(root_partition);
			return vfs_fail("mount legacy UFS1 root", error);
		}
		*root_disk_out = root_partition;
		return 0;
	}

	args.fspec = boot_partition->d_name;
	error = mount_root_create("auto", 0, &args, root_out);
	if (error != 0)
		return vfs_fail("mount legacy boot partition root", error);
	disk_ref(boot_partition);
	*root_disk_out = boot_partition;
	return 0;
}
#endif

struct vfs_overlay_setup {
	struct path lower_file_path;
	struct path upper_file_path;
	struct path lower_root;
	struct path upper_root;
	struct file *lower_file;
	struct file *upper_file;
	struct disk *lower_loop;
	struct disk *upper_loop;
	struct mount *lower_mount;
	struct mount *upper_mount;
};

static void
vfs_overlay_setup_init(struct vfs_overlay_setup *setup)
{
	memset(setup, 0, sizeof(*setup));
	path_init(&setup->lower_file_path);
	path_init(&setup->upper_file_path);
	path_init(&setup->lower_root);
	path_init(&setup->upper_root);
}

static int
vfs_overlay_setup_cleanup(struct vfs_overlay_setup *setup)
{
	int error, first_error = 0;

	path_release(&setup->upper_root);
	path_release(&setup->lower_root);
	if (setup->upper_mount != NULL) {
		error = unmount_private(setup->upper_mount);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->upper_mount = NULL;
	}
	if (setup->lower_mount != NULL) {
		error = unmount_private(setup->lower_mount);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->lower_mount = NULL;
	}
	if (setup->upper_loop != NULL) {
		error = loop_detach(setup->upper_loop);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->upper_loop = NULL;
	}
	if (setup->lower_loop != NULL) {
		error = loop_detach(setup->lower_loop);
		if (first_error == 0 && error != 0)
			first_error = error;
		if (error == 0)
			setup->lower_loop = NULL;
	}
	if (setup->upper_file != NULL) {
		error = file_close(setup->upper_file);
		if (first_error == 0 && error != 0)
			first_error = error;
		setup->upper_file = NULL;
	}
	if (setup->lower_file != NULL) {
		error = file_close(setup->lower_file);
		if (first_error == 0 && error != 0)
			first_error = error;
		setup->lower_file = NULL;
	}
	path_release(&setup->upper_file_path);
	path_release(&setup->lower_file_path);
	return first_error;
}

static void
vfs_overlay_setup_release_transient(struct vfs_overlay_setup *setup)
{
	path_release(&setup->upper_root);
	path_release(&setup->lower_root);
	if (setup->upper_file != NULL) {
		(void)file_close(setup->upper_file);
		setup->upper_file = NULL;
	}
	if (setup->lower_file != NULL) {
		(void)file_close(setup->lower_file);
		setup->lower_file = NULL;
	}
	path_release(&setup->upper_file_path);
	path_release(&setup->lower_file_path);
}

static VFS_HIGH int
vfs_mount_overlay_root(const struct kern_boot_parameters *parameters,
			       struct mount **root_out)
{
	struct vfs_overlay_setup setup;
	struct overlay_mount_args args;
	const char *lower_text = kern_boot_parameters_overlay_root(parameters);
	const char *upper_text = kern_boot_parameters_overlay_data(parameters);
	const char *stage = "resolve overlay-root";
	unsigned lower_slot = 0, upper_slot = 0;
	int cleanup_error, error;

	vfs_overlay_setup_init(&setup);
	error = kern_boot_source_lookup(&boot_sources, lower_text, &lower_slot,
	    &setup.lower_file_path);
	if (error != 0)
		goto fail;
	stage = "resolve overlay-data";
	error = kern_boot_source_lookup(&boot_sources, upper_text, &upper_slot,
	    &setup.upper_file_path);
	if (error != 0)
		goto fail;
	stage = "validate overlay image files";
	if (setup.lower_file_path.p_inode->i_type != INODE_REG ||
	    setup.upper_file_path.p_inode->i_type != INODE_REG) {
		error = EINVAL;
		goto fail;
	}
	if (path_equal(&setup.lower_file_path, &setup.upper_file_path) ||
	    (setup.lower_file_path.p_mount->m_disk != NULL &&
	     setup.upper_file_path.p_mount->m_disk != NULL &&
	     setup.lower_file_path.p_mount->m_disk->d_dev ==
		 setup.upper_file_path.p_mount->m_disk->d_dev &&
	     setup.lower_file_path.p_inode->i_ino ==
		 setup.upper_file_path.p_inode->i_ino)) {
		error = EEXIST;
		goto fail;
	}
	if ((setup.upper_file_path.p_mount->m_flags & MOUNT_READ_ONLY) != 0 ||
	    setup.upper_file_path.p_mount->m_disk == NULL ||
	    (setup.upper_file_path.p_mount->m_disk->d_flags &
	     DISK_READ_ONLY) != 0 ||
	    (setup.upper_file_path.p_inode->i_mode & 0222U) == 0) {
		error = EROFS;
		goto fail;
	}
	stage = "open overlay-root";
	error = file_open_resolved(&setup.lower_file_path, O_RDONLY,
	    &setup.lower_file);
	if (error != 0)
		goto fail;
	stage = "open overlay-data";
	error = file_open_resolved(&setup.upper_file_path, O_RDWR,
	    &setup.upper_file);
	if (error != 0)
		goto fail;
	stage = "attach overlay-root loop";
	error = loop_attach_file(setup.lower_file, LOOP_READ_ONLY,
	    &setup.lower_loop);
	if (error != 0)
		goto fail;
	stage = "attach overlay-data loop";
	error = loop_attach_file(setup.upper_file, LOOP_READ_WRITE,
	    &setup.upper_loop);
	if (error != 0)
		goto fail;
	VFS_LOG("vfs: %s <- %s (private, read-only)\n",
	    setup.lower_loop->d_name, lower_text);
	VFS_LOG("vfs: %s <- %s (private, read-write)\n",
	    setup.upper_loop->d_name, upper_text);
	stage = "mount overlay-root image";
	error = mount_private("auto", setup.lower_loop, MOUNT_READ_ONLY, NULL,
	    &setup.lower_mount);
	if (error != 0)
		goto fail;
	stage = "mount overlay-data image";
	error = mount_private("auto", setup.upper_loop, 0, NULL,
	    &setup.upper_mount);
	if (error != 0)
		goto fail;
	path_set(&setup.lower_root, setup.lower_mount,
	    setup.lower_mount->m_root);
	path_set(&setup.upper_root, setup.upper_mount,
	    setup.upper_mount->m_root);
	/* A successful loop attachment makes its boot slot a system-lifetime
	 * backing owner.  Mark both before the sole root namespace commit. */
	error = kern_boot_source_retain_slot(&boot_sources, lower_slot);
	if (error == 0)
		error = kern_boot_source_retain_slot(&boot_sources, upper_slot);
	if (error != 0) {
		stage = "retain overlay boot slot";
		goto fail;
	}
	stage = "release unused boot slots";
	error = kern_boot_source_release_unused(&boot_sources);
	if (error != 0)
		goto fail;
	memset(&args, 0, sizeof(args));
	args.upper = setup.upper_root;
	args.lower = setup.lower_root;
	args.flags = OVERLAY_READ_WRITE;
	stage = "mount root overlay";
	error = mount_root_create("overlay", 0, &args, root_out);
	if (error != 0)
		goto fail;
	VFS_LOG("vfs: root=overlay lower=%s upper=%s\n", lower_text,
	    upper_text);
	vfs_overlay_setup_release_transient(&setup);
	return 0;

fail:
	cleanup_error = vfs_overlay_setup_cleanup(&setup);
	if (cleanup_error != 0)
		VFS_LOG("vfs: %s cleanup failed (error %d)\n", stage,
		    cleanup_error);
	return vfs_fail(stage, error);
}

static VFS_HIGH int
vfs_resolve_native_root(const char *selector, struct disk **root_disk_out)
{
	struct disk *disk = NULL;
	int error;

	*root_disk_out = NULL;
	error = kern_boot_source_selector_validate(selector);
	if (error != 0)
		return vfs_fail("validate rootpart selector", error);
	error = block_identity_resolve(selector, &disk);
	if (error != 0)
		return vfs_fail("resolve rootpart selector", error);
	if ((disk->d_flags & DISK_PARTITION) == 0) {
		disk_release(disk);
		return vfs_fail("validate rootpart partition", EINVAL);
	}
	VFS_LOG("vfs: rootpart selector %s resolved to /dev/%s\n", selector,
	    disk->d_name);
	*root_disk_out = disk;
	return 0;
}

static VFS_HIGH int
vfs_mount_native_root(struct disk *disk, struct mount **root_out)
{
	struct fat_mount_args args;
	unsigned boot_slot;
	int error;

	if (disk == NULL || root_out == NULL)
		return vfs_fail("mount resolved rootpart", EINVAL);
	if (kern_boot_source_find_disk(&boot_sources, disk, &boot_slot) == 0) {
		error = kern_boot_source_retain_slot(&boot_sources, boot_slot);
		if (error != 0)
			return vfs_fail("retain rootpart boot slot", error);
		error = kern_boot_source_release_unused(&boot_sources);
		if (error != 0)
			return vfs_fail("release unused boot slots", error);
		error = kern_boot_source_promote_root(&boot_sources, boot_slot,
		    root_out);
		if (error != 0)
			return vfs_fail("promote rootpart boot slot", error);
		VFS_LOG("vfs: rootpart reuses boot%u FAT mount\n", boot_slot);
	} else {
		error = kern_boot_source_release_unused(&boot_sources);
		if (error != 0)
			return vfs_fail("release unused boot slots", error);
		args.fspec = disk->d_name;
		error = mount_root_create("auto", 0, &args, root_out);
		if (error != 0)
			return vfs_fail("mount rootpart", error);
	}
	return 0;
}

static int
vfs_boot_source_contains_disk(const struct disk *disk)
{
	unsigned slot;

	return kern_boot_source_find_disk(&boot_sources, disk, &slot) == 0;
}

int
kern_vfs_init(const struct boot_handoff *handoff,
	      const struct boot_device *devices, unsigned device_count)
{
	struct disk *physical[PHYSICAL_DISK_MAX];
	struct disk *boot_physical = NULL;
	struct disk *loader_boot_partition = NULL;
	struct disk *root_partition = NULL;
	struct mount *root_mount = NULL;
	struct path root_path;
	const struct kern_boot_parameters *parameters;
	enum kern_boot_root_mode root_mode = KERN_BOOT_ROOT_NATIVE;
	const char *failure_stage = "initialize root cwd";
	unsigned physical_count = 0, next_number = 1, failed_swap, i;
	int error, legacy_autoroot = 0;

	if (handoff == NULL)
		return vfs_fail("handoff", EINVAL);
	parameters = kern_boot_parameters_current();
	if (parameters == NULL)
		return vfs_fail("boot parameter state", EINVAL);
#if defined(VFS_LEGACY_NULL_AUTOROOT)
	legacy_autoroot = !kern_boot_parameters_source_present();
#endif
	if (!legacy_autoroot) {
		error = kern_boot_source_root_mode(
		    kern_boot_parameters_rootpart(parameters),
		    kern_boot_parameters_overlay_root(parameters),
		    kern_boot_parameters_overlay_data(parameters), &root_mode);
		if (error != 0)
			return vfs_fail("select root mode", error);
	} else {
		VFS_LOG("vfs: absent parameter source; using legacy autoroot\n");
	}
	if (handoff->version == ZEDBSD_HANDOFF_VERSION_SUN4U)
		VFS_LOG("vfs: boot BIOS=%02x Sun slice=%u devices=%u\n",
			handoff->boot_bios_id, handoff->boot_partition_index,
			device_count);
	else if (handoff->version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT)
		VFS_LOG("vfs: boot BIOS=%02x MBR partition=%u devices=%u\n",
			handoff->boot_bios_id, handoff->boot_partition_index,
			device_count);
	else if (handoff->version == ZEDBSD_HANDOFF_VERSION_X68K)
		VFS_LOG("vfs: boot SCSI=%u X68k partition=%u devices=%u\n",
			handoff->boot_bios_id, handoff->boot_partition_index,
			device_count);
	else
		VFS_LOG("vfs: boot BIOS=%02X partition LBA=%u devices=%u\n",
			handoff->boot_bios_id, handoff->boot_partition_lba,
			device_count);
	for (i = 0; i < device_count; i++)
		if (devices[i].bios_id == handoff->boot_bios_id) {
			boot_physical = kern_platform_block_device(&devices[i]);
			break;
		}
	mount_reset();
	(void)loop_init();
	kern_boot_source_context_init(&boot_sources);
	cdev_reset();
	partition_reset();
	error = filesystem_register(&fat_filesystem_type);
	if (error != 0)
		return vfs_fail("register FAT", error);
	error = filesystem_register(&ufs1_filesystem_type);
	if (error != 0)
		return vfs_fail("register UFS1", error);
	error = filesystem_register(&ufs2_filesystem_type);
	if (error != 0)
		return vfs_fail("register UFS2", error);
	error = filesystem_register(&devfs_type);
	if (error != 0)
		return vfs_fail("register devfs", error);
	error = filesystem_register(&tmpfs_type);
	if (error != 0)
		return vfs_fail("register tmpfs", error);
	error = overlayfs_init();
	if (error != 0)
		return vfs_fail("register overlayfs", error);
	input_core_init();
	error = console_device_register();
	if (error != 0)
		return vfs_fail("register console", error);
	error = kern_platform_input_init();
	if (error != 0)
		return vfs_fail("initialize platform input", error);
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	error = graphics_device_register();
	if (error != 0)
		return vfs_fail("register graphics", error);
#endif
	error = system_device_register();
	if (error != 0)
		return vfs_fail("register system", error);
	for (i = 0; i < disk_count() && physical_count < PHYSICAL_DISK_MAX;
	     i++) {
		struct disk *disk = disk_at(i);
		if (disk != NULL && !(disk->d_flags & DISK_PARTITION))
			physical[physical_count++] = disk;
		else if (disk != NULL)
			disk_release(disk);
	}
	VFS_LOG("vfs: native boot disk=%s physical disks=%u\n",
		boot_physical != NULL ? boot_physical->d_name : "none",
		physical_count);
	for (i = 0; i < physical_count; i++) {
		struct partition entries[PARTITION_MAX];
		struct disk_geometry geometry;
		int geometry_error =
		    disk_ioctl(physical[i], DISK_IOCTL_GET_GEOMETRY, &geometry);
		int count = partition_scan(physical[i], entries, PARTITION_MAX);
		int slot;
		if (geometry_error == 0)
			VFS_LOG(
			    "vfs: scan %s H/S=%u/%u blocks=%u: %d entries\n",
			    physical[i]->d_name, geometry.heads,
			    geometry.sectors_per_track,
			    (uint32_t)physical[i]->d_block_count, count);
		else
			VFS_LOG("vfs: scan %s geometry error=%d: %d entries\n",
				physical[i]->d_name, geometry_error, count);
		if (count < 0) {
			disk_release(physical[i]);
			continue;
		}
		for (slot = 0; slot < count; slot++) {
			if (entries[slot].p_block_count == 0 ||
			    partition_create_disk(&entries[slot]) != 0)
				continue;
			VFS_LOG(
			    "vfs: %s partition %u start=%08X:%08X "
			    "data=%08X:%08X blocks=%08X:%08X\n",
			    physical[i]->d_name, entries[slot].p_index + 1U,
			    (uint32_t)(entries[slot].p_start_block >> 32),
			    (uint32_t)entries[slot].p_start_block,
			    (uint32_t)(entries[slot].p_data_block >> 32),
			    (uint32_t)entries[slot].p_data_block,
			    (uint32_t)(entries[slot].p_block_count >> 32),
			    (uint32_t)entries[slot].p_block_count);
			if (physical[i] == boot_physical &&
			    ((handoff->version ==
				  ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
			      handoff->boot_partition_scheme ==
				  ZEDBSD_PARTITION_SCHEME_MBR &&
			      entries[slot].p_index + 1U ==
				  handoff->boot_partition_index) ||
			     (handoff->version ==
				  ZEDBSD_HANDOFF_VERSION_SUN4U &&
			      handoff->boot_partition_scheme ==
				  ZEDBSD_PARTITION_SCHEME_SUN &&
			      entries[slot].p_index ==
				  handoff->boot_partition_index) ||
			     (handoff->version == ZEDBSD_HANDOFF_VERSION_X68K &&
			      handoff->boot_partition_scheme ==
				  ZEDBSD_PARTITION_SCHEME_X68K &&
			      entries[slot].p_index + 1U ==
				  handoff->boot_partition_index &&
			      entries[slot].p_start_block ==
				  handoff->boot_partition_lba) ||
			     (handoff->version == ZEDBSD_HANDOFF_VERSION_PC98 &&
			      entries[slot].p_start_block ==
				  handoff->boot_partition_lba)))
				loader_boot_partition = entries[slot].p_disk;
		}
		disk_release(physical[i]);
	}
#if defined(VFS_LEGACY_NULL_AUTOROOT)
	if (legacy_autoroot) {
		error = vfs_mount_legacy_root(loader_boot_partition, boot_physical,
		    &root_partition, &root_mount);
		if (error != 0)
			return error;
		/* Legacy root discovery has no swapN parameters, but the runtime UAPI
		 * still owns the same active (initially empty) four-source manager. */
		kern_swap_source_set_init(&swap_sources);
		error = kern_swap_source_set_activate(&swap_sources);
		if (error != 0) {
			disk_release(root_partition);
			return vfs_fail("activate legacy runtime swap manager", error);
		}
		error = kern_boot_source_retain_configured(&boot_sources);
		if (error != 0) {
			(void)kern_swap_source_set_abort(&swap_sources);
			disk_release(root_partition);
			return vfs_fail("retain legacy runtime boot slots", error);
		}
		goto root_ready;
	}
#endif
	error = kern_boot_source_context_mount(&boot_sources, parameters,
	    loader_boot_partition, hal_get_arch_handoff("boot.selector"));
	if (error != 0) {
		VFS_LOG("vfs: boot%u %s failed (error %d)\n",
		    boot_sources.failure_slot,
		    kern_boot_source_failure_stage_name(
			boot_sources.failure_stage), error);
		if (boot_sources.cleanup_error != 0)
			VFS_LOG("vfs: boot-slot rollback failed (error %d)\n",
			    boot_sources.cleanup_error);
		return error;
	}
	for (i = 0; i < KERN_BOOT_SOURCE_SLOT_COUNT; i++)
		if (boot_sources.slot[i].configured)
			VFS_LOG("vfs: boot%u %s -> /dev/%s (private FAT)\n", i,
			    kern_boot_parameters_boot(parameters, i) != NULL ?
				kern_boot_parameters_boot(parameters, i) :
				"<loader-origin>",
				    boot_sources.slot[i].disk->d_name);
	if (root_mode == KERN_BOOT_ROOT_NATIVE) {
		error = vfs_resolve_native_root(
		    kern_boot_parameters_rootpart(parameters), &root_partition);
		if (error != 0) {
			int cleanup_error =
			    kern_boot_source_context_destroy(&boot_sources);

			if (cleanup_error != 0)
				VFS_LOG("vfs: rootpart resolution rollback failed "
				    "(error %d)\n", cleanup_error);
			return error;
		}
	}

	/*
	 * Prepare every selected source as one transaction before root selection.
	 * File-backed sources retain their private boot slot; root selection may
	 * then release every unrelated slot without invalidating swap extents.
	 */
	error = kern_swap_boot_prepare(parameters, &boot_sources, &swap_sources,
	    &failed_swap);
	if (error != 0) {
		int cleanup_error;

		VFS_LOG("vfs: swap%u prepare failed (error %d)\n", failed_swap,
		    error);
		cleanup_error = kern_boot_source_context_destroy(&boot_sources);
		if (cleanup_error != 0)
			VFS_LOG("vfs: swap rollback failed (error %d)\n",
			    cleanup_error);
		disk_release(root_partition);
		return error;
	}
	if (root_mode == KERN_BOOT_ROOT_NATIVE) {
		error = kern_swap_source_set_validate_native_root(&swap_sources,
		    root_partition);
		if (error != 0) {
			int swap_error = kern_swap_source_set_abort(&swap_sources);
			int cleanup_error =
			    kern_boot_source_context_destroy(&boot_sources);

			if (swap_error != 0)
				VFS_LOG("vfs: swap alias rollback failed (error %d)\n",
				    swap_error);
			if (cleanup_error != 0)
				VFS_LOG("vfs: swap alias boot-slot rollback failed "
				    "(error %d)\n", cleanup_error);
			disk_release(root_partition);
			return vfs_fail("validate rootpart swap alias", error);
		}
	}
	/*
	 * Runtime `bootN:PATH` is a stable selector, not merely a boot-time
	 * convenience.  Earlier code released every configured slot which was
	 * unrelated to root/boot swap at root selection.  Retain all configured
	 * slots now, while context_destroy can still unwind every private mount;
	 * publication is deferred until the complete VFS namespace is ready.
	 */
	error = kern_boot_source_retain_configured(&boot_sources);
	if (error != 0) {
		int swap_error = kern_swap_source_set_abort(&swap_sources);
		int cleanup_error =
		    kern_boot_source_context_destroy(&boot_sources);

		if (swap_error != 0)
			VFS_LOG("vfs: swap retain rollback failed (error %d)\n",
			    swap_error);
		if (cleanup_error != 0)
			VFS_LOG("vfs: boot-slot retain rollback failed (error %d)\n",
			    cleanup_error);
		disk_release(root_partition);
		return vfs_fail("retain runtime boot slots", error);
	}
	/*
	 * Publish the fully prepared aggregate only after a native root has been
	 * resolved and checked against every raw source.  Root mounting may then
	 * release unused boot slots or commit the namespace.  If publication
	 * fails, every retained file source and private FAT mount can still be
	 * unwound by destroying the untouched boot-source context.
	 */
	error = kern_swap_source_set_activate(&swap_sources);
	if (error != 0) {
		int swap_error = kern_swap_source_set_abort(&swap_sources);
		int cleanup_error;

		if (swap_error != 0)
			VFS_LOG("vfs: swap activation rollback failed (error %d)\n",
			    swap_error);
		cleanup_error = kern_boot_source_context_destroy(&boot_sources);
		if (cleanup_error != 0)
			VFS_LOG("vfs: swap boot-slot rollback failed (error %d)\n",
			    cleanup_error);
		disk_release(root_partition);
		return vfs_fail("activate swap sources", error);
	}
	if (root_mode == KERN_BOOT_ROOT_NATIVE)
		error = vfs_mount_native_root(root_partition, &root_mount);
	else
		error = vfs_mount_overlay_root(parameters, &root_mount);
	if (error != 0) {
		int swap_error = kern_swap_source_set_abort(&swap_sources);
		int cleanup_error;

		if (swap_error != 0)
			VFS_LOG("vfs: swap rollback failed (error %d)\n",
			    swap_error);
		cleanup_error = kern_boot_source_context_destroy(&boot_sources);
		if (cleanup_error != 0)
			VFS_LOG("vfs: root-selection rollback failed (error %d)\n",
			    cleanup_error);
		disk_release(root_partition);
		return error;
	}
	if (swap_sources.count != 0) {
		uint32_t total, free_slots;
		unsigned source_index;

		for (source_index = 0;
		     source_index < KERN_SWAP_SOURCE_COUNT; source_index++) {
			const struct kern_swap_source *source =
			    &swap_sources.range[source_index].source;

			if (source->ops == NULL)
				continue;
			VFS_LOG("swap: swap%u source=%s slots=%u\n",
			    source->parameter_index,
			    kern_boot_parameters_swap(parameters,
				source->parameter_index), source->slot_count);
		}
		if (swap_get_stats(&swap_sources.backend, &total, &free_slots) == 0)
			VFS_LOG("swap: active sources=%u total=%u free=%u\n",
			    swap_sources.count, total, free_slots);
	}
#if defined(VFS_LEGACY_NULL_AUTOROOT)
root_ready:
#endif
	path_set(&root_path, root_mount, root_mount->m_root);
	error = cwdinfo_init(&kern_cwdinfo, &root_path);
	if (error != 0)
		goto out_root;
	process_attach_boot_cwd(&kern_cwdinfo);
	VFS_LOG("vfs: mounting runtime filesystems...\n");
	error = vfs_ensure_root_directory(&root_path, "shm", 01777U);
	if (error != 0 && error != EROFS && error != EOPNOTSUPP)
		goto out_root;
	error = vfs_ensure_root_directory(&root_path, "tmp", 01777U);
	if (error != 0 && error != EROFS && error != EOPNOTSUPP)
		goto out_root;
	failure_stage = "mount /tmp";
	error = mount_at("tmpfs", &root_path, "tmp", 0, NULL, NULL);
	if (error != 0)
		goto out_root;
	error = vfs_ensure_root_directory(&root_path, "run", 0755U);
	if (error != 0 && error != EROFS && error != EOPNOTSUPP)
		goto out_root;
	failure_stage = "mount /run";
	error = mount_at("tmpfs", &root_path, "run", 0, NULL, NULL);
	if (error != 0)
		goto out_root;
	failure_stage = "mount /dev";
	error = mount_at("devfs", &root_path, "dev", 0, NULL, NULL);
	if (error != 0)
		goto out_root;
	{
		struct path dev_path, shm_path;
		struct mount *shm_mount = NULL;
		path_init(&dev_path);
		path_init(&shm_path);
		failure_stage = "resolve /dev";
		error = namei_path_at(&kern_cwdinfo, "/dev", &dev_path);
		if (error == 0) {
			failure_stage = "mount /dev/shm";
			error = mount_at("tmpfs", &dev_path, "shm", 0, NULL,
					 &shm_mount);
		}
		if (error == 0) {
			path_set(&shm_path, shm_mount, shm_mount->m_root);
			failure_stage = "bind /dev/shm at /shm";
			error =
			    mount_bind_at(&shm_path, &root_path, "shm", NULL);
		}
		path_release(&shm_path);
		path_release(&dev_path);
		if (error != 0)
			goto out_root;
	}
	VFS_LOG("vfs: runtime filesystems mounted\n");
	for (i = 0; i < partition_count(); i++) {
		const struct partition *partition = partition_at(i);
		struct fat_mount_args args;
		char name[NAME_MAX + 1U];
		if (partition == NULL || partition->p_disk == NULL ||
		    (legacy_autoroot ?
			 partition->p_disk == loader_boot_partition :
			 vfs_boot_source_contains_disk(partition->p_disk)) ||
		    partition->p_disk == root_partition ||
		    disk_name(next_number, name) != 0)
			continue;
		args.fspec = partition->p_disk->d_name;
		error = mount_at("auto", &root_path, name, 0, &args, NULL);
		if (error == 0)
			next_number++;
	}
	{
		struct kern_swap_control_registration registration;

		failure_stage = "publish runtime boot selectors";
		error = kern_boot_source_publish_runtime(&boot_sources);
		if (error != 0)
			goto out_root;
		memset(&swap_control_context, 0,
		    sizeof(swap_control_context));
		swap_control_context.boot_sources = &boot_sources;
		if (root_partition != NULL) {
			disk_ref(root_partition);
			swap_control_context.native_root = root_partition;
		}
		memset(&registration, 0, sizeof(registration));
		registration.sources = &swap_sources;
		registration.resolver = &vfs_swap_resolver;
		registration.resolver_context = &swap_control_context;
		failure_stage = "register runtime swap control";
		error = kern_swap_control_register(&registration);
		if (error != 0) {
			disk_release(swap_control_context.native_root);
			swap_control_context.native_root = NULL;
			goto out_root;
		}
	}
	path_release(&root_path);
	disk_release(root_partition);
	return 0;

out_root:
	path_release(&root_path);
	disk_release(root_partition);
	return vfs_fail(failure_stage, error);
}
