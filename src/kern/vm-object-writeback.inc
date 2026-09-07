/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* Establishes an independent writer before taking any file/content lease. */
int
vm_object_writeback_prepare(struct file *file, struct vm_object **result)
{
	struct vm_object *object;
	struct file *writer;
	struct backing_mutation_guard guard;
	struct inode *inode;
	bool enabled;
	int error;

	if (result == NULL)
		return EINVAL;
	*result = NULL;
	if (file == NULL || file->f_inode == NULL || file->f_path.p_inode == NULL ||
	    file->f_inode->i_type != INODE_REG || file->f_backing_claim != NULL ||
	    file->f_format_claim != NULL || (file_status_flags_get(file) & O_ACCMODE) == O_RDONLY)
		return EOPNOTSUPP;
	inode = file_vm_inode(file);
	if (inode != file->f_inode ||
	    (inode->i_flags & (INODE_LOOPFILE | INODE_SWAPFILE)) != 0)
		return EOPNOTSUPP;
	error = backing_mutation_begin_inode(inode, &guard);
	if (error != 0)
		return error;
	error = vm_object_get_shared_internal(file, &object, 1);
	backing_mutation_end(&guard);
	if (error != 0)
		return error;
	writer = NULL;
	enabled = registry_lock();
	if (object->write_file != NULL) {
		registry_unlock(enabled);
		*result = object;
		return 0;
	}
	registry_unlock(enabled);

	/* The caller already owns write access; the clone never reaches userland. */
	error = file_open_resolved(&file->f_path, O_RDWR | O_NOFOLLOW, &writer);
	if (error == 0 && file_vm_inode(writer) != inode)
		error = EAGAIN;
	if (error == 0) {
		enabled = registry_lock();
		if (object->write_file == NULL) {
			object->write_file = writer;
			writer = NULL;
		}
		registry_unlock(enabled);
	}
	if (writer != NULL)
		(void)file_close(writer);
	if (error != 0) {
		vm_object_writeback_release(object);
		return error;
	}
	*result = object;
	return 0;
}

void
vm_object_writeback_release(struct vm_object *object)
{
	if (object == NULL)
		return;
	object_operation_end(object);
	if (refcount_put(&object->refs))
		destroy_object(object);
}

/*
 * Takes the existing content transaction without flushing its old dirty image.
 * New pages remain private until all old bytes and mapping revocations succeed.
 * The caller drops inode I/O locks while this preparation performs backend reads.
 */
int
vm_object_content_prepare_delayed(struct vm_object_content *content,
    struct vm_object *object, struct writeback_ticket *ticket)
{
	struct vm_object_page *pages[16];
	unsigned fresh[16];
	uint64_t base;
	uint64_t end;
	uint64_t current;
	unsigned count;
	unsigned index;
	unsigned long irq;
	uint32_t observed;
	size_t wanted;
	ssize_t received;
	int error;

	if (content == NULL || !content->active || content->prepared ||
	    object == NULL || object->inode != content->inode || ticket == NULL ||
	    ticket->budget == NULL || object->write_file == NULL || content->length == 0)
		return EINVAL;
	if (content->offset < 0 || content->offset > object->logical_size ||
	    content->length > (uint64_t)(object->logical_size - content->offset))
		return EAGAIN;
	base = (uint64_t)content->offset & ~(uint64_t)(PAGE_SIZE - 1U);
	end = (uint64_t)content->offset + content->length;
	count = (unsigned)((end - base + PAGE_SIZE - 1U) / PAGE_SIZE);
	if (count > 16 || ticket->reserved < (uint64_t)count * PAGE_SIZE)
		return EAGAIN;
	memset(pages, 0, sizeof(pages));
	memset(fresh, 0, sizeof(fresh));
	error = 0;

	/* Existing readers/pins remain authoritative; no partial taking on refusal. */
	irq = spin_lock_irqsave(&object->lock);
	if ((object->flags & VM_OBJECT_CONTENT) == 0 ||
	    object->content_generation != content->generation)
		HAL_FATAL("delayed write lost content gate");
	for (index = 0; index < count; index++) {
		pages[index] = find_page(object, (off_t)(base + index * PAGE_SIZE));
		if (pages[index] != NULL &&
		    ((pages[index]->flags & (VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK |
		    VM_OBJECT_PAGE_ERROR | VM_OBJECT_PAGE_ORPHANED)) != 0 ||
		    pages[index]->hold_count != pages[index]->pin_count)) {
			spin_unlock_irqrestore(&object->lock, irq);
			return EAGAIN;
		}
	}
	for (index = 0; index < count; index++) {
		if (pages[index] != NULL) {
			pages[index]->flags |= VM_OBJECT_PAGE_BUSY | VM_OBJECT_PAGE_WRITEBACK;
			pages[index]->content_generation = content->generation;
		}
	}
	spin_unlock_irqrestore(&object->lock, irq);

	for (index = 0; index < count; index++) {
		current = base + (uint64_t)index * PAGE_SIZE;
		if (pages[index] == NULL) {
			pages[index] = alloc_object_page(object, (off_t)current, 1);
			if (pages[index] == NULL) {
				error = EAGAIN;
				goto failed;
			}
			fresh[index] = 1;
			memset(pages[index]->pmem.vaddr, 0, PAGE_SIZE);
			wanted = (size_t)(object->logical_size - (off_t)current);
			if (wanted > PAGE_SIZE)
				wanted = PAGE_SIZE;
			/* Preserve old bytes even for a full overwrite: commit may still abort. */
			received = file_pread_internal(object->file, pages[index]->pmem.vaddr,
			    wanted, (off_t)current, FILE_IO_VM_OBJECT);
			if (received != (ssize_t)wanted) {
				error = received < 0 ? (int)-received : EIO;
				goto failed;
			}
			pages[index]->content_generation = content->generation;
			continue;
		}
		observed = 0;
		if (pages[index]->mapping_count != 0) {
			if (vmspace_object_page_revoke == NULL)
				error = EOPNOTSUPP;
			else
				error = vmspace_object_page_revoke(pages[index], &observed);
		}
		irq = spin_lock_irqsave(&object->lock);
		if ((observed & HAL_PAGE_DIRTY) != 0)
			object_page_dirty_mark(pages[index]);
		spin_unlock_irqrestore(&object->lock, irq);
		if (error != 0)
			goto failed;
	}

	/* Recheck all private identities before publishing any of them. */
	irq = spin_lock_irqsave(&object->lock);
	for (index = 0; index < count; index++) {
		if (fresh[index] && find_page(object, pages[index]->offset) != NULL) {
			spin_unlock_irqrestore(&object->lock, irq);
			error = EAGAIN;
			goto failed;
		}
	}
	for (index = 0; index < count; index++) {
		if (fresh[index]) {
			pages[index]->next = object->pages;
			object->pages = pages[index];
			object_page_index_insert(object, pages[index]);
			(void)atomic_fetch_add_relaxed(&object_pages, 1);
		}
	}
	content->prepared = 1;
	content->writeback_ticket = ticket;
	spin_unlock_irqrestore(&object->lock, irq);
	return 0;

failed:
	irq = spin_lock_irqsave(&object->lock);
	content_release_pages_locked(object, content->generation);
	spin_unlock_irqrestore(&object->lock, irq);
	for (index = 0; index < count; index++) {
		if (fresh[index])
			release_object_page_storage(pages[index]);
	}
	return error;
}
