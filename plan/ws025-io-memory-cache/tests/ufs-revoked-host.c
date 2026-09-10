/* Actual UFS revoked-unmount preflight; unrelated driver sections are discarded. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef int tid_t;
#include "../../../src/drivers/fs/ufs.c"

int
disk_media_status(const struct disk *disk)
{
    return disk->d_media_revoked ? ENXIO : 0;
}

void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }

int
main(void)
{
    struct mount mount = {0};
    struct disk disk = {0};
    struct disk snapshot = {0};
    struct ufs_mount_state volume = {0};
    struct ufs_mount_state before;

    assert(ufs_prepare_unmount_revoked(NULL) == EINVAL);
    assert(ufs_prepare_unmount_revoked(&mount) == EINVAL);
    mount.m_disk = &disk;
    mount.m_data = &volume;
    assert(ufs_prepare_unmount_revoked(&mount) == EINVAL);
    disk.d_media_revoked = 1;
    volume.writable = 1;
    before = volume;
    assert(ufs_prepare_unmount_revoked(&mount) == 0);
    assert(memcmp(&volume, &before, sizeof(volume)) == 0);
    volume.snapshot_disk = &snapshot;
    before = volume;
    assert(ufs_prepare_unmount_revoked(&mount) == EBUSY);
    assert(memcmp(&volume, &before, sizeof(volume)) == 0);
    volume.snapshot_disk = NULL;
    volume.journal.image_readers = 1;
    before = volume;
    assert(ufs_prepare_unmount_revoked(&mount) == EBUSY);
    assert(memcmp(&volume, &before, sizeof(volume)) == 0);
    volume.journal.image_readers = IMAGE_READERS_CLOSED | 1;
    assert(ufs_prepare_unmount_revoked(&mount) == EBUSY);
    volume.journal.image_readers = IMAGE_READERS_CLOSED;
    assert(ufs_prepare_unmount_revoked(&mount) == 0);
    volume.writable = 0;
    assert(ufs_prepare_unmount_revoked(&mount) == 0);
    volume.writable = 1;
    volume.journal.image_readers = 0;
    mount.m_state = MOUNT_STATE_DYING;
    before = volume;
    ufs_commit_unmount_revoked(&mount);
    before.writable = 0;
    before.journal.image_readers = IMAGE_READERS_CLOSED;
    assert(memcmp(&volume, &before, sizeof(volume)) == 0);
    ufs_commit_unmount_revoked(&mount);
    assert(memcmp(&volume, &before, sizeof(volume)) == 0);
    mount.m_data = NULL;
    assert(ufs_prepare_unmount_revoked(&mount) == EINVAL);
    puts("UFS revoked teardown: PASS preflight, refusal, local no-I/O commit");
    return 0;
}
