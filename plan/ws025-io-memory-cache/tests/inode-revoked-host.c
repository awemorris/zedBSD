/* Actual inode-cache preflight with a controlled VM ownership boundary. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef int tid_t;
#include "../../../src/kern/inode.c"

static unsigned cache_locked;
static struct inode *vm_inode;
static unsigned vm_owned;
static int vm_error;
static int global_error;

void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
int disk_media_status(const struct disk *disk)
{ return disk->d_media_revoked ? ENXIO : 0; }

unsigned long spin_lock_irqsave(struct spinlock *lock)
{ assert(lock == &inode_cache_lock && !cache_locked); cache_locked = 1; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq)
{ (void)irq; assert(lock == &inode_cache_lock && cache_locked); cache_locked = 0; }
int vm_object_discard_mount_refs(struct mount *mount, struct inode *inode, unsigned *refs)
{
    assert(mount && !cache_locked);
    *refs = 0;
    if (!inode) return global_error;
    assert(refcount_load(&inode->i_refs) >= 2);
    if (vm_error && inode == vm_inode) return vm_error;
    if (inode == vm_inode) *refs = vm_owned;
    return 0;
}

int main(void)
{
    struct mount mount = {0}, other = {0};
    struct disk disk = {0};
    struct inode root = {0}, file = {0}, foreign = {0};
    struct inode before;
    root.i_mount = file.i_mount = &mount;
    foreign.i_mount = &other;
    mount.m_root = &root;
    assert(inode_cache_mount_revoked_check(NULL) == EINVAL);
    assert(inode_cache_mount_revoked_check(&mount) == EBUSY);
    mount.m_state = MOUNT_STATE_DYING;
    global_error = EINVAL;
    assert(inode_cache_mount_revoked_check(&mount) == EINVAL);
    global_error = 0;
    inode_cache[0] = &root;
    inode_cache[1] = INODE_CACHE_RESERVED;
    inode_cache[2] = &foreign;
    inode_cache[3] = &file;
    refcount_init(&root.i_refs, 2);
    refcount_init(&file.i_refs, 4); /* cache + namespace + two VM descriptions */
    atomic_store_release(&file.i_namespace_refs, 1);
    file.i_flags = INODE_DIRTY;
    vm_inode = &file;
    vm_owned = 2;
    before = file;
    assert(inode_cache_mount_revoked_check(&mount) == 0);
    assert(memcmp(&file, &before, sizeof(file)) == 0);
    refcount_get(&file.i_refs); /* external FD/cwd-equivalent owner */
    before = file;
    assert(inode_cache_mount_revoked_check(&mount) == EBUSY);
    assert(memcmp(&file, &before, sizeof(file)) == 0);
    refcount_init(&file.i_refs, 4);
    vm_error = EBUSY;
    assert(inode_cache_mount_revoked_check(&mount) == EBUSY);
    assert(refcount_load(&root.i_refs) == 2 && refcount_load(&file.i_refs) == 4);
    vm_error = 0;
    vm_owned = UINT_MAX;
    assert(inode_cache_mount_revoked_check(&mount) == EBUSY);
    assert(refcount_load(&file.i_refs) == 4);
    vm_owned = 2;
    atomic_store_release(&file.i_namespace_refs, UINT_MAX);
    assert(inode_cache_mount_revoked_check(&mount) == EBUSY);
    assert(refcount_load(&file.i_refs) == 4);
    atomic_store_release(&file.i_namespace_refs, 1);
    refcount_init(&file.i_refs, UINT_MAX);
    assert(inode_cache_mount_revoked_check(&mount) == EBUSY);
    assert(refcount_load(&file.i_refs) == UINT_MAX);
    assert(!cache_locked && inode_cache[3] == &file && file.i_flags == INODE_DIRTY);
    refcount_init(&file.i_refs, 4);
    mount.m_disk = &disk;
    disk.d_media_revoked = 1;
    file.i_flags |= INODE_DEAD;
    foreign.i_flags = INODE_DIRTY;
    before = file;
    assert(inode_cache_discard_mount_dirty(&mount) == 1);
    before.i_flags &= ~INODE_DIRTY;
    assert(memcmp(&file, &before, sizeof(file)) == 0);
    assert(inode_cache[3] == &file && foreign.i_flags == INODE_DIRTY);
    assert(inode_cache_discard_mount_dirty(&mount) == 0);
    assert(!cache_locked);
    puts("Inode revoked teardown: PASS ownership, pin cleanup, local dirty discard");
    return 0;
}
