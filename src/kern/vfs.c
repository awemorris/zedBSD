/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/vfs.h"
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
#include "kern/mouse-device.h"
#include "kern/graphics-device.h"
#include "kern/system-device.h"
#include "kern/devfs.h"
#include "kern/tmpfs.h"
#include "kern/overlayfs.h"
#include "kern/loop.h"
#include "kern/klog.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <string.h>

#define PHYSICAL_DISK_MAX 4U
#define VFS_HIGH __attribute__((section(".hightext")))
#define VFS_LOG(...) do { \
	hal_printf(__VA_ARGS__); \
	kern_logf(__VA_ARGS__); \
} while (0)

#if defined(HAL_ARCH_I386)
#define ROOTFS_IMAGE_PRIMARY "/rootfs.img"
#define ROOTFS_IMAGE_UNIFIED_PC98 "/rootfs.98"
#define ROOTFS_IMAGE_UNIFIED_OTHER "/rootfs.x86"
#elif defined(HAL_ARCH_AMD64)
#define ROOTFS_IMAGE_PRIMARY "/rootfs.img"
#define ROOTFS_IMAGE_UNIFIED_OTHER "/rootfs.x64"
#elif defined(HAL_ARCH_ARM64)
#define ROOTFS_IMAGE_PRIMARY "/rootfs.img"
#define ROOTFS_IMAGE_UNIFIED_OTHER "/rootfs.rp4"
#endif

struct cwdinfo kern_cwdinfo __attribute__((section(".vfs_bss")));

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
boot_root_selector(char output[ZEDBSD_BLKID_TEXT_MAX], unsigned *mount_flags)
{
	const char *line = hal_get_arch_handoff("boot.command-line");
	if (line == NULL) return ENOENT;
	while (*line != '\0') {
		const char *start;
		size_t length;
		while (*line == ' ') line++;
		start = line;
		while (*line != '\0' && *line != ' ') line++;
		length = (size_t)(line - start);
		if (length > 5U && memcmp(start, "root=", 5U) == 0) {
			start += 5U; length -= 5U;
			/* ro/rw is a mount option, not part of the identity. */
			for (size_t i = 0; i < length; i++)
				if (start[i] == ',') {
					const char *option = start + i + 1U;
					size_t option_length = length - i - 1U;
					if (option_length == 2U &&
					    memcmp(option, "ro", 2U) == 0)
						*mount_flags = MOUNT_READ_ONLY;
					else if (!(option_length == 2U &&
					    memcmp(option, "rw", 2U) == 0))
						return EINVAL;
					length = i; break;
				}
			if (length == 0 || length >= ZEDBSD_BLKID_TEXT_MAX)
				return EINVAL;
			memcpy(output, start, length); output[length] = '\0';
			return 0;
		}
	}
	return ENOENT;
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

static VFS_HIGH int
ufs1_root_marker_matches(struct disk *disk,int *matches)
{
	static const char expected[]="zedBSD ufs1 root v1\n";
	struct mount *mountp=NULL;
	struct path marker;
	struct file *file=NULL;
	char value[sizeof(expected)];
	ssize_t count;
	int error;

	*matches=0;
	path_init(&marker);
	error=mount_private("ufs1",disk,MOUNT_READ_ONLY,NULL,&mountp);
	if(error==EOPNOTSUPP || error==EINVAL || error==EROFS)
		return 0;
	if(error!=0)
		return error;
	error=mount_private_lookup(mountp,"etc/zedbsd-root",&marker);
	if(error==ENOENT || error==ENOTDIR) {
		error=0;
		goto out;
	}
	if(error!=0)
		goto out;
	error=file_open_resolved(&marker,O_RDONLY,&file);
	if(error!=0)
		goto out;
	count=file_read(file,value,sizeof(value));
	if(count<0) {
		error=(int)-count;
		goto out;
	}
	*matches=count==(ssize_t)(sizeof(expected)-1U) &&
	    memcmp(value,expected,sizeof(expected)-1U)==0;
out:
	if(file!=NULL)
		(void)file_close(file);
	path_release(&marker);
	if(unmount_private(mountp)!=0 && error==0)
		error=EBUSY;
	return error;
}

int
kern_vfs_init(const struct boot_handoff *handoff,
	      const struct boot_device *devices, unsigned device_count)
{
	struct disk *physical[PHYSICAL_DISK_MAX];
	struct disk *boot_physical = NULL;
	struct disk *boot_partition = NULL;
	struct disk *root_partition = NULL;
	struct disk *root_loop = NULL;
	struct mount *root_mount;
#ifdef ROOTFS_IMAGE_PRIMARY
	struct mount *boot_private = NULL;
	struct disk *data_loop = NULL;
	struct mount *root_private = NULL;
	struct mount *data_private = NULL;
#endif
	struct path root_path;
	const char *failure_stage = "initialize root cwd";
	unsigned physical_count = 0, next_number = 1, i;
	int error;
	char root_selector[ZEDBSD_BLKID_TEXT_MAX];
	int explicit_root = 0;
	unsigned root_mount_flags = 0;

	if (handoff == NULL)
		return vfs_fail("handoff", EINVAL);
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
	for (i = 0; i < disk_count() && physical_count < PHYSICAL_DISK_MAX; i++) {
		struct disk *disk = disk_at(i);
		if (disk != NULL && !(disk->d_flags & DISK_PARTITION))
			physical[physical_count++] = disk;
	}
	VFS_LOG("vfs: native boot disk=%s physical disks=%u\n",
	    boot_physical != NULL ? boot_physical->d_name : "none",
	    physical_count);
	mount_reset();
	(void)loop_init();
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
	error = console_device_register();
	if (error != 0)
		return vfs_fail("register console", error);
	error = mouse_device_register();
	if (error != 0)
		return vfs_fail("register mouse", error);
	error = kern_platform_input_init();
	if (error != 0)
		return vfs_fail("initialize platform input", error);
	error = graphics_device_register();
	if (error != 0)
		return vfs_fail("register graphics", error);
	error = system_device_register();
	if (error != 0)
		return vfs_fail("register system", error);
	for (i = 0; i < physical_count; i++) {
		struct partition entries[PARTITION_MAX];
		struct disk_geometry geometry;
		int geometry_error = disk_ioctl(physical[i],
		    DISK_IOCTL_GET_GEOMETRY, &geometry);
		int count = partition_scan(physical[i], entries, PARTITION_MAX);
		int slot;
		if (geometry_error == 0)
			VFS_LOG("vfs: scan %s H/S=%u/%u blocks=%u: %d entries\n",
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
			VFS_LOG("vfs: %s partition %u start=%u data=%u blocks=%u\n",
			    physical[i]->d_name, (unsigned)slot + 1U,
			    (uint32_t)entries[slot].p_start_block,
			    (uint32_t)entries[slot].p_data_block,
			    (uint32_t)entries[slot].p_block_count);
			if (physical[i] == boot_physical &&
			    ((handoff->version ==
			      ZEDBSD_HANDOFF_VERSION_MULTIBOOT &&
			      handoff->boot_partition_scheme ==
			      ZEDBSD_PARTITION_SCHEME_MBR &&
			      entries[slot].p_index + 1U ==
			      handoff->boot_partition_index) ||
			     (handoff->version == ZEDBSD_HANDOFF_VERSION_SUN4U &&
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
			     (handoff->version ==
			      ZEDBSD_HANDOFF_VERSION_PC98 &&
			      entries[slot].p_start_block ==
			      handoff->boot_partition_lba)))
				boot_partition = entries[slot].p_disk;
		}
		disk_release(physical[i]);
	}
	if (boot_partition == NULL)
		return vfs_fail("find boot partition", ENXIO);
	error = boot_root_selector(root_selector, &root_mount_flags);
	if (error == 0) {
		error = block_identity_resolve(root_selector, &root_partition);
		if (error != 0) return vfs_fail("resolve root selector", error);
		explicit_root = 1;
		VFS_LOG("vfs: root selector %s resolved to /dev/%s\n",
		    root_selector, root_partition->d_name);
	} else if (error != ENOENT) return vfs_fail("parse root selector", error);
	for(i=0;i<partition_count();i++) {
		const struct partition *partition=partition_at(i);
		int matches=0;
		if(explicit_root || partition==NULL || partition->p_disk==NULL ||
		    partition->p_disk==boot_partition ||
		    partition->p_parent!=boot_physical)
			continue;
		error=ufs1_root_marker_matches(partition->p_disk,&matches);
		if(error!=0)
			return vfs_fail("inspect UFS1 root candidate",error);
		if(!matches)
			continue;
		if(root_partition!=NULL)
			return vfs_fail("ambiguous UFS1 root candidates",EINVAL);
		root_partition=partition->p_disk;
	}
#ifdef ROOTFS_IMAGE_PRIMARY
	if (root_partition == NULL) {
		struct overlay_mount_args overlay_args;
		struct path private_root;
		struct path lower;
		struct path upper;
		path_init(&private_root);
		path_init(&lower);
		path_init(&upper);
		error = mount_private("auto", boot_partition, 0, NULL,
		    &boot_private);
		if (error == 0) {
			path_set(&private_root, boot_private, boot_private->m_root);
			error = loop_attach_path(&private_root, ROOTFS_IMAGE_PRIMARY,
			    LOOP_READ_ONLY, &root_loop);
			if (error == ENOENT) {
				const char *unified = ROOTFS_IMAGE_UNIFIED_OTHER;
#ifdef ROOTFS_IMAGE_UNIFIED_PC98
				if (handoff->version == ZEDBSD_HANDOFF_VERSION_PC98)
					unified = ROOTFS_IMAGE_UNIFIED_PC98;
#endif
					error = loop_attach_path(&private_root,
				    unified, LOOP_READ_ONLY, &root_loop);
			}
			if (error == 0)
				error = loop_attach_path(&private_root, "/data.img",
				    LOOP_READ_WRITE, &data_loop);
		}
		path_release(&private_root);
		if (error == ENOENT && root_loop == NULL) {
			(void)unmount_private(boot_private);
			boot_private = NULL;
			error = 0;
		}
		if (error != 0)
			return vfs_fail(root_loop != NULL ? "attach data image" :
			    "attach rootfs image", error);
		if (root_loop != NULL && data_loop != NULL) {
			VFS_LOG("vfs: %s <- rootfs image (private, read-only)\n",
			    root_loop->d_name);
			VFS_LOG("vfs: %s <- data.img (private, read-write)\n",
			    data_loop->d_name);
			VFS_LOG("vfs: %s filesystem consistency check...\n",
			    root_loop->d_name);
			error = mount_private("auto", root_loop, MOUNT_READ_ONLY,
			    NULL, &root_private);
			if (error == 0) {
				VFS_LOG("vfs: %s UFS consistency check...\n",
				    data_loop->d_name);
				error = mount_private("auto", data_loop, 0, NULL,
				    &data_private);
			}
			if (error == 0)
				path_set(&lower, root_private, root_private->m_root);
			if (error == 0)
				path_set(&upper, data_private, data_private->m_root);
			memset(&overlay_args, 0, sizeof(overlay_args));
			overlay_args.upper = upper;
			overlay_args.lower = lower;
			overlay_args.flags = OVERLAY_READ_WRITE;
			if (error == 0) {
				VFS_LOG("vfs: mounting overlay filesystem at /...\n");
				error = mount_root_create("overlay", 0,
				    &overlay_args, &root_mount);
			}
			path_release(&upper);
			path_release(&lower);
			if (error != 0)
				return vfs_fail("mount root overlay", error);
			VFS_LOG("vfs: root=overlay lower=%s upper=%s\n",
			    root_loop->d_name, data_loop->d_name);
		}
	}
#endif
	if (root_partition != NULL) {
		struct fat_mount_args args = { root_partition->d_name };
		error = mount_root_create("auto", root_mount_flags, &args,
		    &root_mount);
	} else if (root_loop == NULL) {
		struct fat_mount_args args = { boot_partition->d_name };
		error = mount_root_create("auto", 0, &args, &root_mount);
	}
	if (error != 0)
		return vfs_fail(root_partition != NULL ? "mount UFS1 root" :
		    root_loop != NULL ? "mount rootfs image" :
		    "mount boot FAT root", error);
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
			error = mount_bind_at(&shm_path, &root_path, "shm", NULL);
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
		    partition->p_disk == boot_partition ||
		    partition->p_disk == root_partition ||
		    disk_name(next_number, name) != 0)
			continue;
		args.fspec = partition->p_disk->d_name;
		error = mount_at("auto", &root_path, name, 0, &args, NULL);
		if (error == 0)
			next_number++;
	}
	path_release(&root_path);
	return 0;

out_root:
	path_release(&root_path);
	return vfs_fail(failure_stage, error);
}
