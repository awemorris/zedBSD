/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/vfs.h"
#include "kern/disk.h"
#include "kern/fat-vfs.h"
#include "kern/file.h"
#include "kern/ufs1.h"
#include "kern/mount.h"
#include "kern/partition.h"
#include "kern/platform.h"
#include "kern/process.h"
#include "kern/cdev.h"
#include "kern/console-device.h"
#include "kern/graphics-device.h"
#include "kern/system-device.h"
#include "kern/devfs.h"
#include "kern/overlayfs.h"
#include "kern/loop.h"
#include "kern/swap-fat.h"

#include <errno.h>
#include <fcntl.h>
#include <hal/hal.h>
#include <string.h>

#define PHYSICAL_DISK_MAX 4U
#define VFS_HIGH __attribute__((section(".hightext")))

struct userland_profile_desc {
	const char *name;
	const char *fat_image_path;
	const char *ufs_image_path;
	const char *init_path;
};

struct arch_userland_state {
	struct disk *loop_disk;
	struct mount *private_mount;
	struct mount *bin_overlay;
	struct mount *lib_overlay;
	struct path lower_bin;
	struct path lower_lib;
	struct path upper_bin;
	struct path upper_lib;
	struct path metadata_root;
	int active;
};

#if defined(HAL_ARCH_I386)
static const struct userland_profile_desc userland_profile = {
	"i386", "/arch/i386.img", "/arch/i386.ufs", "/bin/sh"
};
#define ZEDBSD_HAS_ARCH_USERLAND 1
#elif defined(HAL_ARCH_AMD64)
static const struct userland_profile_desc userland_profile = {
	"amd64", "/arch/amd64.img", "/arch/amd64.ufs", "/bin/sh"
};
#define ZEDBSD_HAS_ARCH_USERLAND 1
#elif defined(HAL_ARCH_ARM64)
static const struct userland_profile_desc userland_profile = {
	"aarch64", "/arch/aarch64.img", "/arch/aarch64.ufs", "/bin/sh"
};
#define ZEDBSD_HAS_ARCH_USERLAND 1
#endif

#ifdef ZEDBSD_HAS_ARCH_USERLAND
static struct arch_userland_state arch_userland
	__attribute__((section(".vfs_bss")));
#endif

struct cwdinfo kern_cwdinfo __attribute__((section(".vfs_bss")));

static int
vfs_fail(const char *stage, int error)
{
	hal_printf("vfs: %s failed (error %d)\n", stage, error);
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

#ifdef ZEDBSD_HAS_ARCH_USERLAND
static VFS_HIGH void
vfs_arch_userland_cleanup(void)
{
	if (arch_userland.lib_overlay != NULL) {
		(void)unmount("/lib", 0);
		arch_userland.lib_overlay = NULL;
	}
	if (arch_userland.bin_overlay != NULL) {
		(void)unmount("/bin", 0);
		arch_userland.bin_overlay = NULL;
	}
	path_release(&arch_userland.metadata_root);
	path_release(&arch_userland.upper_lib);
	path_release(&arch_userland.upper_bin);
	if (arch_userland.private_mount != NULL) {
		(void)unmount_private(arch_userland.private_mount);
		arch_userland.private_mount = NULL;
	}
	if (arch_userland.loop_disk != NULL) {
		(void)loop_detach(arch_userland.loop_disk);
		arch_userland.loop_disk = NULL;
	}
	path_release(&arch_userland.lower_lib);
	path_release(&arch_userland.lower_bin);
	arch_userland.active = 0;
}

static VFS_HIGH int
vfs_check_profile_marker(const struct userland_profile_desc *profile)
{
	struct path marker;
	struct file *file;
	char value[32];
	ssize_t count;
	size_t expected = strlen(profile->name);
	int error;
	path_init(&marker);
	error = mount_private_lookup(arch_userland.private_mount,
		"lib/arch.id", &marker);
	if (error != 0)
		return error;
	error = file_open_resolved(&marker, O_RDONLY, &file);
	path_release(&marker);
	if (error != 0)
		return error;
	count = file_read(file, value, sizeof(value));
	(void)file_close(file);
	if (count < 0)
		return (int)-count;
	if ((size_t)count != expected + 1U || value[expected] != '\n' ||
	    memcmp(value, profile->name, expected))
		return EINVAL;
	return 0;
}

static VFS_HIGH int
vfs_mount_arch_userland(const struct userland_profile_desc *profile,
			const struct path *root, int ufs_profile)
{
	struct overlay_mount_args args;
	struct path shell;
	const char *image_path = ufs_profile ? profile->ufs_image_path :
		profile->fat_image_path;
	const char *filesystem = ufs_profile ? "ufs1" : "fat";
	const char *stage = "capture lower /bin";
	int error;
	memset(&arch_userland, 0, sizeof(arch_userland));
	path_init(&arch_userland.lower_bin);
	path_init(&arch_userland.lower_lib);
	path_init(&arch_userland.upper_bin);
	path_init(&arch_userland.upper_lib);
	path_init(&arch_userland.metadata_root);
	error = namei_path_at(&kern_cwdinfo, "/bin", &arch_userland.lower_bin);
	if (error != 0) goto fail;
	stage = "capture lower /lib";
	error = namei_path_at(&kern_cwdinfo, "/lib", &arch_userland.lower_lib);
	if (error != 0) goto fail;
	stage = "attach profile loop";
	error = loop_attach_path(root, image_path, LOOP_READ_WRITE,
		&arch_userland.loop_disk);
	if (error != 0) goto fail;
	stage = "mount private profile filesystem";
	error = mount_private(filesystem, arch_userland.loop_disk, 0, NULL,
		&arch_userland.private_mount);
	if (error != 0) goto fail;
	stage = "lookup private /bin";
	error = mount_private_lookup(arch_userland.private_mount, "bin",
		&arch_userland.upper_bin);
	if (error != 0) goto fail;
	stage = "lookup private /lib";
	error = mount_private_lookup(arch_userland.private_mount, "lib",
		&arch_userland.upper_lib);
	if (error != 0) goto fail;
	stage = "lookup private metadata";
	error = mount_private_lookup(arch_userland.private_mount, "zedovl",
		&arch_userland.metadata_root);
	if (error != 0) goto fail;
	if (arch_userland.upper_bin.p_inode->i_type != INODE_DIR ||
	    arch_userland.upper_lib.p_inode->i_type != INODE_DIR ||
	    arch_userland.metadata_root.p_inode->i_type != INODE_DIR) {
		error = ENOTDIR;
		goto fail;
	}
	stage = "validate profile marker";
	error = vfs_check_profile_marker(profile);
	if (error != 0) goto fail;
	memset(&args, 0, sizeof(args));
	args.upper = arch_userland.upper_bin;
	args.lower = arch_userland.lower_bin;
	args.metadata_root = arch_userland.metadata_root;
	args.journal_base = "bin";
	args.flags = OVERLAY_READ_WRITE;
	stage = "mount /bin overlay";
	error = overlay_mount_at(root->p_mount, "bin", &args,
		&arch_userland.bin_overlay);
	if (error != 0) goto fail;
	args.upper = arch_userland.upper_lib;
	args.lower = arch_userland.lower_lib;
	args.journal_base = "lib";
	stage = "mount /lib overlay";
	error = overlay_mount_at(root->p_mount, "lib", &args,
		&arch_userland.lib_overlay);
	if (error != 0) goto fail;
	path_init(&shell);
	stage = "resolve /bin/sh";
	error = namei_path_at(&kern_cwdinfo, profile->init_path, &shell);
	if (error == 0 && shell.p_inode->i_type != INODE_REG)
		error = ENOEXEC;
	path_release(&shell);
	if (error != 0) goto fail;
	arch_userland.active = 1;
	hal_printf("vfs: userland profile=%s image=%s loop=%s "
	    "source=private fs=%s overlay=rw\n", profile->name, image_path,
	    arch_userland.loop_disk->d_name, filesystem);
	return 0;
fail:
	hal_printf("vfs: userland stage=%s profile=%s image=%s "
	    "failed (error %d)\n", stage, profile->name, image_path,
	    error);
	vfs_arch_userland_cleanup();
	return error;
}
#endif

int
kern_vfs_init(const struct zedbsd_handoff *handoff,
	      const struct zedbsd_device *devices, unsigned device_count)
{
	struct disk *physical[PHYSICAL_DISK_MAX];
	struct disk *boot_physical = NULL;
	struct disk *boot_partition = NULL;
	struct disk *root_partition = NULL;
	struct mount *boot_mount,*root_mount;
	struct path boot_path;
	struct path root_path;
	const char *failure_stage = "initialize root cwd";
	unsigned physical_count = 0, next_number = 2, i;
	int error;

	if (handoff == NULL)
		return vfs_fail("handoff", EINVAL);
	if (handoff->version == ZEDBSD_HANDOFF_VERSION_SUN4U)
		hal_printf("vfs: boot BIOS=%02x Sun slice=%u devices=%u\n",
		    handoff->boot_bios_id, handoff->boot_partition_index,
		    device_count);
	else if (handoff->version == ZEDBSD_HANDOFF_VERSION_MULTIBOOT)
		hal_printf("vfs: boot BIOS=%02x MBR partition=%u devices=%u\n",
		    handoff->boot_bios_id, handoff->boot_partition_index,
		    device_count);
	else
		hal_printf("vfs: boot BIOS=%02X partition LBA=%u devices=%u\n",
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
	hal_printf("vfs: native boot disk=%s physical disks=%u\n",
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
	error = filesystem_register(&devfs_type);
	if (error != 0)
		return vfs_fail("register devfs", error);
	error = overlayfs_init();
	if (error != 0)
		return vfs_fail("register overlayfs", error);
	error = console_device_register();
	if (error != 0)
		return vfs_fail("register console", error);
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
			hal_printf("vfs: scan %s H/S=%u/%u blocks=%u: %d entries\n",
			    physical[i]->d_name, geometry.heads,
			    geometry.sectors_per_track,
			    (uint32_t)physical[i]->d_block_count, count);
		else
			hal_printf("vfs: scan %s geometry error=%d: %d entries\n",
			    physical[i]->d_name, geometry_error, count);
		if (count < 0) {
			disk_release(physical[i]);
			continue;
		}
		for (slot = 0; slot < count; slot++) {
			if (entries[slot].p_block_count == 0 ||
			    partition_create_disk(&entries[slot]) != 0)
				continue;
			hal_printf("vfs: %s partition %u start=%u data=%u blocks=%u\n",
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
	for(i=0;i<partition_count();i++) {
		const struct partition *partition=partition_at(i);
		int matches=0;
		if(partition==NULL || partition->p_disk==NULL ||
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
	path_init(&boot_path);
	{
		struct fat_mount_args args = {
			root_partition!=NULL?root_partition->d_name:
			boot_partition->d_name
		};
		error = mount_root_create(root_partition!=NULL?"ufs1":"auto", 0,
			&args, &root_mount);
	}
	if (error != 0)
		return vfs_fail(root_partition!=NULL?"mount UFS1 root":
			"mount boot FAT root", error);
	path_set(&root_path, root_mount, root_mount->m_root);
	error = cwdinfo_init(&kern_cwdinfo, &root_path);
	if (error != 0)
		goto out_root;
	process_attach_boot_cwd(&kern_cwdinfo);
	if(root_partition!=NULL) {
		struct fat_mount_args args={boot_partition->d_name};
		failure_stage="mount boot FAT at /boot";
		error=mount_at("auto",&root_path,"boot",0,&args,&boot_mount);
		if(error!=0)
			goto out_root;
		path_set(&boot_path,boot_mount,boot_mount->m_root);
		failure_stage="bind boot FAT at /disk1";
		error=mount_bind_at(&boot_path,&root_path,"disk1",NULL);
		if(error!=0)
			goto out_root;
		failure_stage="bind UFS1 root at /disk2";
		error=mount_bind_at(&root_path,&root_path,"disk2",NULL);
		if(error!=0)
			goto out_root;
		next_number=3;
		hal_printf("vfs: root=ufs1 disk=%s boot=%s\n",
			root_partition->d_name,boot_partition->d_name);
	} else {
		boot_mount=root_mount;
		path_set(&boot_path,boot_mount,boot_mount->m_root);
		failure_stage = "bind /disk1";
		error = mount_bind_at(&root_path, &root_path, "disk1", NULL);
		if (error != 0)
			goto out_root;
		hal_printf("vfs: root=legacy-fat disk=%s\n",boot_partition->d_name);
	}
	failure_stage = "mount /dev";
	error = mount_at("devfs", &root_path, "dev", 0, NULL, NULL);
	if (error != 0)
		goto out_root;
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
#ifdef ZEDBSD_HAS_ARCH_USERLAND
	failure_stage = "mount architecture userland";
	error = vfs_mount_arch_userland(&userland_profile, &root_path,
		root_partition != NULL);
	if (error != 0)
		goto out_root;
#endif
	error = swap_fat_activate(&kern_cwdinfo,
		root_partition!=NULL?"/boot":"/");
	if (error != 0 && error != ENOENT)
		hal_printf("swap: /swapfile disabled (%d)\n", error);
	path_release(&boot_path);
	path_release(&root_path);
	return 0;

out_root:
	path_release(&boot_path);
	path_release(&root_path);
	return vfs_fail(failure_stage, error);
}
