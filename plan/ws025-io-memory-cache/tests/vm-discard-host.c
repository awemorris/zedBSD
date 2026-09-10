/* Actual VM ownership preflight; collaborators only provide locks/media status. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef int tid_t;
#include "../../../src/kern/vm.c"

bool hal_irq_disable(void) { return false; }
void hal_irq_enable(void) {}
unsigned long spin_lock_irqsave(struct spinlock *lock) { (void)lock; return 0; }
void spin_unlock_irqrestore(struct spinlock *lock, unsigned long irq) { (void)lock; (void)irq; }
int disk_media_status(const struct disk *disk)
{ return disk == NULL || disk->d_media_revoked ? ENXIO : 0; }

static unsigned frames_freed, objects_freed, files_closed;
static size_t credits_released;
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
int hal_pmem_free(struct hal_pmem *memory)
{ (void)memory; assert(atomic_load_acquire(&object_registry_lock) == 0); frames_freed++; return HAL_OK; }
void kern_free(void *pointer)
{ assert(pointer && atomic_load_acquire(&object_registry_lock) == 0); objects_freed++; }
int file_close(struct file *file)
{ assert(file && atomic_load_acquire(&object_registry_lock) == 0); files_closed++; return 0; }
void writeback_budget_clean(struct writeback_budget *budget, size_t bytes)
{ assert(budget); credits_released += bytes; }
int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{ (void)rank; (void)name; memset(mutex, 0, sizeof(*mutex)); return 0; }
int mutex_owned(struct mutex *mutex) { return mutex->locked; }
void mutex_lock(struct mutex *mutex) { assert(!mutex->locked); mutex->locked = 1; }
void mutex_unlock(struct mutex *mutex) { assert(mutex->locked); mutex->locked = 0; }

int main(void)
{
    struct mount mount = {0}, other = {0};
    struct disk disk = {0};
    struct inode inode = {0}, unrelated = {0};
    struct vm_object object = {0}, foreign = {0}, before;
    struct vm_object_page page = {0}, orphan = {0};
    unsigned mode;
    mount.m_disk = &disk;
    inode.i_mount = &mount; unrelated.i_mount = &other;
    object.inode = &inode; foreign.inode = &unrelated;
    object.next = &foreign; shared_objects = &object;
    refcount_init(&object.refs, 1);
    object.flags = VM_OBJECT_CACHE_REFERENCE | VM_OBJECT_RETAINED_WRITEBACK;
    object.writeback_error = EIO;
    object.pages = &page; object.dirty_pages = &page;
    page.flags = VM_OBJECT_PAGE_DIRTY; page.dirty_linked = 1;
    assert(vm_object_discard_mount_check(NULL) == EINVAL);
    assert(vm_object_discard_mount_check(&other) == EINVAL);
    assert(vm_object_discard_mount_check(&mount) == EINVAL);
    disk.d_media_revoked = 1;
    before = object;
    assert(vm_object_discard_mount_check(&mount) == 0);
    assert(memcmp(&before, &object, sizeof(object)) == 0);
    mount.m_state = MOUNT_STATE_DYING;
    object.flags = VM_OBJECT_CACHE_REFERENCE;
    object.writeback_error = 0;
    object.dirty_pages = NULL;
    page.flags = 0;
    page.dirty_linked = 0;
    assert(object_mount_reserved(&object));
    assert(vm_object_reclaim_clean(PAGE_SIZE) == 0);
    assert(object_cache_evict_one(NULL) == 0);
    assert(shared_objects == &object && object.pages == &page && !frames_freed);
    mount.m_state = MOUNT_STATE_LIVE;
    assert(!object_mount_reserved(&object));
    object = before;
    page.flags = VM_OBJECT_PAGE_DIRTY;
    page.dirty_linked = 1;
    for (mode = 0; mode < 12; mode++) {
        object = before;
        page.hold_count = page.pin_count = page.mapping_count = 0;
        page.flags = VM_OBJECT_PAGE_DIRTY;
        if (mode == 0) object.mapping_count = 1;
        if (mode == 1) object.active_operations = 1;
        if (mode == 2) object.registry_waiters = 1;
        if (mode == 3) refcount_get(&object.refs);
        if (mode == 4) object.flags |= VM_OBJECT_DETACHING;
        if (mode == 5) object.flags |= VM_OBJECT_CONTENT;
        if (mode == 6) page.hold_count = 1;
        if (mode == 7) page.pin_count = 1;
        if (mode == 8) page.mapping_count = 1;
        if (mode == 9) page.flags |= VM_OBJECT_PAGE_WRITEBACK;
        if (mode == 10) { object.orphan_pages = &orphan; orphan.pin_count = 1; }
        if (mode == 11) object.flags |= VM_OBJECT_RESIZING;
        assert(vm_object_discard_mount_check(&mount) == EBUSY);
        assert(object.writeback_error == EIO && object.dirty_pages == &page);
        assert(page.flags & VM_OBJECT_PAGE_DIRTY);
    }
    object = before;
    foreign.inode = &inode; /* A busy second object must prevent every discard. */
    {
        uint64_t discarded = 123;
        assert(vm_object_discard_mount(&mount, &discarded) == EBUSY && discarded == 0);
        assert(object.dirty_pages == &page && object.writeback_error == EIO);
        assert(shared_objects == &object && !objects_freed && !frames_freed);
    }
    foreign.inode = &unrelated;
    {
        struct file reader = {0}, writer = {0};
        unsigned refs = 123;
        uint64_t discarded = 123;
        reader.f_inode = &inode;
        reader.f_path.p_inode = &inode;
        reader.f_path.p_mount = &mount;
        writer = reader;
        refcount_init(&reader.f_refs, 2);
        refcount_init(&writer.f_refs, 1);
        object.file = &reader;
        object.write_file = &reader;
        assert(vm_object_discard_mount_refs(&mount, NULL, &refs) == 0 && refs == 1);
        assert(vm_object_discard_mount_refs(&mount, &inode, &refs) == 0 && refs == 1);
        assert(vm_object_discard_mount_refs(&mount, &unrelated, &refs) == EINVAL && refs == 0);
        refcount_get(&reader.f_refs);
        assert(vm_object_discard_mount_refs(&mount, NULL, &refs) == EBUSY && refs == 0);
        assert(vm_object_discard_mount(&mount, &discarded) == EBUSY && discarded == 0);
        assert(object.dirty_pages == &page && object.writeback_error == EIO);
        assert(shared_objects == &object && !objects_freed && !frames_freed);
        refcount_init(&reader.f_refs, 1);
        object.write_file = &writer;
        assert(vm_object_discard_mount_refs(&mount, NULL, &refs) == 0 && refs == 2);
        assert(vm_object_discard_mount_refs(&mount, &inode, &refs) == 0 && refs == 2);
        writer.f_path.p_mount = &other;
        assert(vm_object_discard_mount_check(&mount) == EBUSY);
        writer.f_path.p_mount = &mount;
        writer.f_inode = &unrelated;
        assert(vm_object_discard_mount_check(&mount) == EBUSY);
        object = before;
    }
    {
        struct vm_page_slab slab = {0};
        struct file file = {0};
        uint64_t discarded;
        memset(&page, 0, sizeof(page)); memset(&orphan, 0, sizeof(orphan));
        page.owner = &object; page.flags = VM_OBJECT_PAGE_DIRTY;
        page.dirty_linked = 1; page.metadata_slab = &slab;
        orphan.owner = &object; orphan.flags = VM_OBJECT_PAGE_DIRTY | VM_OBJECT_PAGE_ORPHANED;
        orphan.metadata_slab = &slab;
        page.writeback_budget = (struct writeback_budget *)&disk;
        orphan.writeback_budget = page.writeback_budget;
        slab.used = 2; slab.memory.size = PAGE_SIZE;
        object.orphan_pages = &orphan;
        object.file = &file; object.write_file = &file;
        file.f_inode = &inode;
        file.f_path.p_inode = &inode;
        file.f_path.p_mount = &mount;
        refcount_init(&file.f_refs, 2);
        object_count = 2; cache_objects = 1; atomic_store_release(&object_pages, 2);
        assert(vm_object_discard_mount(&mount, &discarded) == 0);
        assert(discarded == 2 * PAGE_SIZE && credits_released == 2 * PAGE_SIZE);
        assert(frames_freed == 3 && objects_freed == 1 && files_closed == 2);
        assert(shared_objects == &foreign && object_count == 1 && cache_objects == 0);
        assert(atomic_load_acquire(&object_pages) == 0 && page_slabs == NULL);
        assert(vm_object_discard_mount(&mount, &discarded) == 0 && discarded == 0);
    }
    shared_objects = NULL;
    puts("VM revoked-mount discard: PASS preflight, refusal nonmutation, dirty credits, detached resource release");
    return 0;
}
