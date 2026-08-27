/* Boot-slot and root-mode production regression fixture (BR-T44). */
#include <kern/boot-source.h>
#include <kern/inode.h>
#include <kern/namei.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_DISKS 6U

static struct disk disks[FIXTURE_DISKS];
static struct mount mounts[KERN_BOOT_SOURCE_SLOT_COUNT];
static struct inode inodes[KERN_BOOT_SOURCE_SLOT_COUNT];
static enum bootfat_type disk_fat[FIXTURE_DISKS];
static int disk_refs[FIXTURE_DISKS];
static unsigned mount_attempt;
static unsigned fail_mount_attempt;
static unsigned private_mounts;
static unsigned promoted_mounts;
static char lookup_relative[ZEDBSD_PATH_MAX];

static unsigned
disk_index(const struct disk *disk)
{
	assert(disk >= disks && disk < disks + FIXTURE_DISKS);
	return (unsigned)(disk - disks);
}

static struct disk *
selector_disk(const char *selector)
{
	if (!strcmp(selector, "sda1") || !strcmp(selector, "/dev/sda1") ||
	    !strcmp(selector, "UUID=A0") || !strcmp(selector, "alias0"))
		return &disks[0];
	if (!strcmp(selector, "sdb1") || !strcmp(selector, "UUID=B1"))
		return &disks[1];
	if (!strcmp(selector, "sdc1"))
		return &disks[2];
	if (!strcmp(selector, "sdd1"))
		return &disks[3];
	if (!strcmp(selector, "whole"))
		return &disks[4];
	if (!strcmp(selector, "fat12"))
		return &disks[5];
	return NULL;
}

void
disk_ref(struct disk *disk)
{
	disk_refs[disk_index(disk)]++;
}

void
disk_release(struct disk *disk)
{
	if (disk == NULL)
		return;
	assert(disk_refs[disk_index(disk)] > 0);
	disk_refs[disk_index(disk)]--;
}

int
block_identity_resolve(const char *selector, struct disk **result)
{
	struct disk *disk;

	assert(result != NULL);
	*result = NULL;
	if (!strcmp(selector, "ambiguous"))
		return EEXIST;
	disk = selector_disk(selector);
	if (disk == NULL)
		return ENOENT;
	disk_ref(disk);
	*result = disk;
	return 0;
}

int
fat_probe_type(struct disk *disk, enum bootfat_type *type)
{
	unsigned index = disk_index(disk);

	if (disk_fat[index] == 0)
		return EIO;
	*type = disk_fat[index];
	return 0;
}

int
mount_private(const char *type_name, struct disk *disk, int flags, void *data,
	      struct mount **result)
{
	struct mount *mountp;
	unsigned slot;

	assert(!strcmp(type_name, "fat"));
	assert(flags == 0);
	assert(data == NULL);
	assert(result != NULL);
	*result = NULL;
	mount_attempt++;
	if (fail_mount_attempt != 0U &&
	    mount_attempt == fail_mount_attempt)
		return EIO;
	for (slot = 0; slot < KERN_BOOT_SOURCE_SLOT_COUNT; slot++)
		if (mounts[slot].m_state == MOUNT_STATE_FREE)
			break;
	assert(slot < KERN_BOOT_SOURCE_SLOT_COUNT);
	mountp = &mounts[slot];
	memset(mountp, 0, sizeof(*mountp));
	mountp->m_internal_flags = MOUNT_PRIVATE_INTERNAL;
	mountp->m_state = MOUNT_STATE_LIVE;
	mountp->m_disk = disk;
	mountp->m_root = &inodes[slot];
	inodes[slot].i_mount = mountp;
	disk_ref(disk);
	private_mounts++;
	*result = mountp;
	return 0;
}

int
unmount_private(struct mount *mountp)
{
	assert(mountp != NULL);
	assert((mountp->m_internal_flags & MOUNT_PRIVATE_INTERNAL) != 0);
	assert(mountp->m_state == MOUNT_STATE_LIVE);
	disk_release(mountp->m_disk);
	memset(mountp, 0, sizeof(*mountp));
	assert(private_mounts != 0U);
	private_mounts--;
	return 0;
}

int
mount_private_lookup(struct mount *mountp, const char *relative,
		     struct path *result)
{
	assert(mountp != NULL);
	assert(relative != NULL);
	assert(result != NULL);
	assert(strlen(relative) < sizeof(lookup_relative));
	strcpy(lookup_relative, relative);
	if (!strcmp(relative, "missing"))
		return ENOENT;
	result->p_mount = mountp;
	result->p_inode = mountp->m_root;
	return 0;
}

int
mount_private_promote_root(struct mount *mountp, struct mount **result)
{
	assert(mountp != NULL);
	assert((mountp->m_internal_flags & MOUNT_PRIVATE_INTERNAL) != 0);
	mountp->m_internal_flags &= ~MOUNT_PRIVATE_INTERNAL;
	assert(private_mounts != 0U);
	private_mounts--;
	promoted_mounts++;
	if (result != NULL)
		*result = mountp;
	return 0;
}

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
	assert(relative != NULL);
	path_set(result, context->root.p_mount, context->root.p_inode);
	return 0;
}

static void
fixture_reset(void)
{
	unsigned index;

	memset(disks, 0, sizeof(disks));
	memset(mounts, 0, sizeof(mounts));
	memset(inodes, 0, sizeof(inodes));
	memset(disk_refs, 0, sizeof(disk_refs));
	for (index = 0; index < FIXTURE_DISKS; index++) {
		disks[index].d_flags = DISK_PARTITION;
		disks[index].d_dev = (dev_t)(100U + index);
		disk_fat[index] = ZEDBSD_FAT16;
	}
	disks[4].d_flags = 0;
	disk_fat[5] = ZEDBSD_FAT12;
	mount_attempt = 0;
	fail_mount_attempt = 0;
	private_mounts = 0;
	promoted_mounts = 0;
	lookup_relative[0] = '\0';
}

static void
assert_no_owned_resources(void)
{
	unsigned index;

	assert(private_mounts == 0U);
	for (index = 0; index < FIXTURE_DISKS; index++)
		assert(disk_refs[index] == 0);
}

static void
parse_ok(struct kern_boot_parameters *parameters, const char *text)
{
	assert(kern_boot_parameters_parse(parameters, text,
	    strlen(text) + 1U) == 0);
}

static void
test_selector_grammar(void)
{
	char maximum[DISK_NAME_MAX];
	char too_long[DISK_NAME_MAX + 1U];

	assert(kern_boot_source_selector_validate("sda1") == 0);
	assert(kern_boot_source_selector_validate("/dev/sda1") == 0);
	assert(kern_boot_source_selector_validate("UUID=6740-911D") == 0);
	assert(kern_boot_source_selector_validate("LABEL=ZEDBOOT") == 0);
	assert(kern_boot_source_selector_validate("PARTUUID=0123-01") == 0);
	assert(kern_boot_source_selector_validate("PARTLABEL=boot") == 0);
	assert(kern_boot_source_selector_validate(NULL) == EINVAL);
	assert(kern_boot_source_selector_validate("") == EINVAL);
	assert(kern_boot_source_selector_validate("/dev/") == EINVAL);
	assert(kern_boot_source_selector_validate("/dev/a/b") == EINVAL);
	assert(kern_boot_source_selector_validate("/dev/UUID=x") == EINVAL);
	assert(kern_boot_source_selector_validate("TYPE=x") == EINVAL);
	assert(kern_boot_source_selector_validate("bad/name") == EINVAL);
	assert(kern_boot_source_selector_validate("bad name") == EINVAL);

	memset(maximum, 'a', sizeof(maximum) - 1U);
	maximum[sizeof(maximum) - 1U] = '\0';
	assert(kern_boot_source_selector_validate(maximum) == 0);
	memset(too_long, 'a', sizeof(too_long) - 1U);
	too_long[sizeof(too_long) - 1U] = '\0';
	assert(kern_boot_source_selector_validate(too_long) == ENAMETOOLONG);
}

static void
test_boot_references(void)
{
	struct kern_boot_source_reference reference;
	char text[6U + ZEDBSD_PATH_MAX + 1U];

	assert(kern_boot_source_reference_parse("boot0:rootfs.img",
	    &reference) == 0);
	assert(reference.slot == 0U);
	assert(!strcmp(reference.relative, "rootfs.img"));
	assert(kern_boot_source_reference_parse("boot3:/images/data.img",
	    &reference) == 0);
	assert(reference.slot == 3U);
	assert(!strcmp(reference.relative, "images/data.img"));
	assert(kern_boot_source_reference_parse("boot4:x", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot00:x", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:/", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0://x", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:a//b", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:a/", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:.", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:a/../b", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:a\\b", &reference) ==
	    EINVAL);
	assert(kern_boot_source_reference_parse("boot0:a\177b", &reference) ==
	    EINVAL);

	memcpy(text, "boot0:", 6U);
	memset(text + 6U, 'a', ZEDBSD_PATH_MAX - 1U);
	text[6U + ZEDBSD_PATH_MAX - 1U] = '\0';
	assert(kern_boot_source_reference_parse(text, &reference) == 0);
	text[6U + ZEDBSD_PATH_MAX - 1U] = 'a';
	text[6U + ZEDBSD_PATH_MAX] = '\0';
	assert(kern_boot_source_reference_parse(text, &reference) ==
	    ENAMETOOLONG);
}

static void
test_root_mode_contract(void)
{
	enum kern_boot_root_mode mode;

	assert(kern_boot_source_root_mode("sda1", NULL, NULL, &mode) == 0);
	assert(mode == KERN_BOOT_ROOT_NATIVE);
	assert(kern_boot_source_root_mode(NULL, "boot0:root", "boot0:data",
	    &mode) == 0);
	assert(mode == KERN_BOOT_ROOT_OVERLAY);
	assert(kern_boot_source_root_mode(NULL, NULL, NULL, &mode) == EINVAL);
	assert(kern_boot_source_root_mode(NULL, "boot0:root", NULL, &mode) ==
	    EINVAL);
	assert(kern_boot_source_root_mode(NULL, NULL, "boot0:data", &mode) ==
	    EINVAL);
	assert(kern_boot_source_root_mode("sda1", "boot0:root",
	    "boot0:data", &mode) == EINVAL);
	assert(kern_boot_source_root_mode("sda1", NULL, NULL, NULL) == EINVAL);
	assert(kern_boot_source_fat_type_supported(ZEDBSD_FAT16));
	assert(kern_boot_source_fat_type_supported(ZEDBSD_FAT32));
	assert(!kern_boot_source_fat_type_supported(ZEDBSD_FAT12));
}

static void
test_sparse_slots_lookup_and_release(void)
{
	struct kern_boot_parameters parameters;
	struct kern_boot_source_context context;
	struct path path;
	unsigned slot;

	fixture_reset();
	parse_ok(&parameters, "boot1=sdb1 boot3=sdd1 rootpart=sdc1");
	kern_boot_source_context_init(&context);
	assert(kern_boot_source_context_mount(&context, &parameters, &disks[0],
	    NULL) == 0);
	assert(context.slot[0].configured);
	assert(context.slot[1].configured);
	assert(context.slot[1].runtime_mount == context.slot[1].mount);
	assert(!context.slot[2].configured);
	assert(context.slot[3].configured);
	assert(private_mounts == 3U);
	assert(kern_boot_source_lookup(&context, "boot1:/images/root.img",
	    &slot, &path) == 0);
	assert(slot == 1U);
	assert(!strcmp(lookup_relative, "images/root.img"));
	assert(kern_boot_source_lookup(&context, "boot2:x", &slot, &path) ==
	    ENOENT);
	assert(kern_boot_source_lookup(&context, "boot1:../x", &slot, &path) ==
	    EINVAL);
	assert(kern_boot_source_retain_slot(&context, 1U) == 0);
	assert(kern_boot_source_release_unused(&context) == 0);
	assert(!context.slot[0].configured);
	assert(context.slot[1].configured);
	assert(!context.slot[3].configured);
	assert(private_mounts == 1U);
	assert(kern_boot_source_context_destroy(&context) == 0);
	assert_no_owned_resources();
}

static void
test_loader_selector_and_explicit_override(void)
{
	struct kern_boot_parameters parameters;
	struct kern_boot_source_context context;

	/* The firmware selector identifies boot0 even when BIOS drive order made
	 * the loader-origin heuristic point at a different partition. */
	fixture_reset();
	parse_ok(&parameters, "rootpart=sdc1");
	kern_boot_source_context_init(&context);
	assert(kern_boot_source_context_mount(&context, &parameters, &disks[0],
	    "UUID=B1") == 0);
	assert(context.slot[0].disk == &disks[1]);
	assert(disk_refs[0] == 0);
	assert(kern_boot_source_context_destroy(&context) == 0);
	assert_no_owned_resources();

	fixture_reset();
	parse_ok(&parameters, "rootpart=sdc1");
	kern_boot_source_context_init(&context);
	assert(kern_boot_source_context_mount(&context, &parameters, NULL,
	    "UUID=A0") == 0);
	assert(context.slot[0].disk == &disks[0]);
	assert(kern_boot_source_context_destroy(&context) == 0);
	assert_no_owned_resources();

	fixture_reset();
	parse_ok(&parameters, "boot0=sdb1 rootpart=sdc1");
	kern_boot_source_context_init(&context);
	assert(kern_boot_source_context_mount(&context, &parameters, &disks[0],
	    "UUID=A0") == 0);
	assert(context.slot[0].disk == &disks[1]);
	assert(disk_refs[0] == 0);
	assert(kern_boot_source_context_destroy(&context) == 0);
	assert_no_owned_resources();
}

static void
assert_mount_failure(const char *text,
		     enum kern_boot_source_failure_stage stage, int expected,
		     unsigned failed_slot)
{
	struct kern_boot_parameters parameters;
	struct kern_boot_source_context context;

	fixture_reset();
	parse_ok(&parameters, text);
	kern_boot_source_context_init(&context);
	assert(kern_boot_source_context_mount(&context, &parameters, &disks[0],
	    NULL) == expected);
	assert(context.failure_stage == stage);
	assert(context.failure_slot == failed_slot);
	assert(context.cleanup_error == 0);
	assert_no_owned_resources();
}

static void
test_validation_and_rollback(void)
{
	struct kern_boot_parameters parameters;
	struct kern_boot_source_context context;
	static const unsigned failed_slot[] = { 0U, 1U, 3U };
	unsigned attempt;

	assert_mount_failure("boot1=/bad rootpart=sdc1",
	    KERN_BOOT_SOURCE_FAILURE_SELECTOR, EINVAL, 1U);
	assert_mount_failure("boot1=missing rootpart=sdc1",
	    KERN_BOOT_SOURCE_FAILURE_RESOLVE, ENOENT, 1U);
	assert_mount_failure("boot1=ambiguous rootpart=sdc1",
	    KERN_BOOT_SOURCE_FAILURE_RESOLVE, EEXIST, 1U);
	assert_mount_failure("boot1=whole rootpart=sdc1",
	    KERN_BOOT_SOURCE_FAILURE_PARTITION, EINVAL, 1U);
	assert_mount_failure("boot1=alias0 rootpart=sdc1",
	    KERN_BOOT_SOURCE_FAILURE_DUPLICATE, EEXIST, 1U);
	assert_mount_failure("boot1=fat12 rootpart=sdc1",
	    KERN_BOOT_SOURCE_FAILURE_FILESYSTEM, EOPNOTSUPP, 1U);

	for (attempt = 1U; attempt <= 3U; attempt++) {
		fixture_reset();
		parse_ok(&parameters,
		    "boot1=sdb1 boot3=sdd1 rootpart=sdc1");
		kern_boot_source_context_init(&context);
		fail_mount_attempt = attempt;
		assert(kern_boot_source_context_mount(&context, &parameters,
		    &disks[0], NULL) == EIO);
		assert(context.failure_stage == KERN_BOOT_SOURCE_FAILURE_MOUNT);
		assert(context.failure_slot == failed_slot[attempt - 1U]);
		assert(context.cleanup_error == 0);
		assert_no_owned_resources();
	}

	fixture_reset();
	parse_ok(&parameters, "rootpart=sdc1");
	kern_boot_source_context_init(&context);
	assert(kern_boot_source_context_mount(&context, &parameters, NULL,
	    NULL) == ENXIO);
	assert(context.failure_stage == KERN_BOOT_SOURCE_FAILURE_RESOLVE);
	assert_no_owned_resources();
}

static void
test_same_fat_root_promotion(void)
{
	struct kern_boot_parameters parameters;
	struct kern_boot_source_context context;
	struct mount *root = NULL;
	unsigned slot;

	fixture_reset();
	parse_ok(&parameters, "rootpart=sda1");
	kern_boot_source_context_init(&context);
	assert(kern_boot_source_context_mount(&context, &parameters, &disks[0],
	    NULL) == 0);
	assert(kern_boot_source_find_disk(&context, &disks[0], &slot) == 0);
	assert(slot == 0U);
	assert(kern_boot_source_retain_slot(&context, slot) == 0);
	assert(kern_boot_source_promote_root(&context, slot, &root) == 0);
	assert(root != NULL);
	assert(context.slot[0].promoted);
	assert(context.slot[0].mount == NULL);
	assert(context.slot[0].runtime_mount == root);
	assert(context.slot[0].disk == &disks[0]);
	assert(private_mounts == 0U);
	assert(promoted_mounts == 1U);
	assert(kern_boot_source_context_destroy(&context) == 0);
	/* The promoted mount, like a real namespace root, owns the disk now. */
	assert(disk_refs[0] == 1);
}

int
main(void)
{
	test_selector_grammar();
	test_boot_references();
	test_root_mode_contract();
	test_sparse_slots_lookup_and_release();
	test_loader_selector_and_explicit_override();
	test_validation_and_rollback();
	test_same_fat_root_promotion();
	puts("boot source tests passed");
	return 0;
}
