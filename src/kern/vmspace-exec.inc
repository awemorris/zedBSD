/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* Publishes captured cache pages as a private mapping, never MAP_SHARED. */
int
vmspace_map_exec_snapshot(
	struct vmspace *vm,
	uintptr_t start,
	uint32_t prot,
	struct file_exec_snapshot *snapshot)
{
	struct vm_region *region;
	int error;

	if (vm == NULL || vm == &kernel_vmspace || snapshot == NULL ||
	    !snapshot->input.active || !snapshot->input.shared_read ||
	    snapshot->input.read_object == NULL || snapshot->length == 0 ||
	    snapshot->page_count != snapshot->length / PAGE_SIZE ||
	    (prot & HAL_SPACE_WRITE) != 0)
		return EINVAL;
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	error = map_region(vm, start, snapshot->length, prot, VM_BACKING_FILE,
	    snapshot->input.file, snapshot->offset, start, snapshot->length,
	    0, 0, &region);
	if (error == 0) {
		file_exec_snapshot_ref(snapshot);
		region->snapshot = snapshot;
		vmspace_generation_advance_locked(vm);
	}
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	return error;
}

/* Caller reserved BUSY page/region ownership and released both VM locks. */
static int
vmspace_exec_cache_fault(
	struct vmspace *vm,
	struct vm_region *region,
	struct vm_page *page,
	uint32_t required)
{
	struct vm_object_page *source;
	struct vm_private_page *fresh;
	uintptr_t address;
	uint32_t prot;
	unsigned long irq;
	int error, writing, unmapped;

	/* The region's snapshot pins the immutable source during allocation and copy. */
	source = page->object_page;
	address = page->address;
	prot = region->prot;
	writing = required == HAL_SPACE_WRITE;
	fresh = NULL;
	error = 0;
	unmapped = 0;
	if (writing) {
		fresh = private_page_alloc();
		if (fresh == NULL)
			error = ENOMEM;
		if (error == 0)
			error = allocate_page_frame(fresh, page);
		if (error == 0) {
			irq = spin_lock_irqsave(&fresh->state_lock);
			fresh->flags = VM_PAGE_RESIDENT;
			fresh->generation++;
			if (fresh->generation == 0)
				fresh->generation++;
			spin_unlock_irqrestore(&fresh->state_lock, irq);
			error = vm_object_page_pin_read(source, 0, fresh->pmem.vaddr, PAGE_SIZE);
			if (error == 0)
				vm_private_page_mark_dirty(fresh);
		}
	}

	/* Replaces a PTE only after the complete private copy is ready. */
	if (error == 0 && writing && (page->flags & VM_MAPPING_MAPPED) != 0) {
		if (hal_page_unmap(vm->space, (void *)address, PAGE_SIZE) != HAL_OK)
			error = EIO;
		else
			unmapped = 1;
	}
	vm_metadata_enter();
	mutex_lock(&vm->lock);
	if ((page->flags & VM_MAPPING_BUSY) == 0 || region->hold_count == 0)
		HAL_FATAL("exec cache fault lost reservation");
	if (unmapped)
		page->flags &= ~VM_MAPPING_MAPPED;
	if (error == 0 && (find_region_locked(vm, address, 1) != region ||
	    find_page(region, address) != page || page->object_page != source ||
	    region->snapshot == NULL || region->prot != prot || (prot & required) == 0))
		error = EAGAIN;
	if (error == 0 && hal_page_map(vm->space, (void *)address,
	    writing ? fresh->pmem.paddr : source->pmem.paddr, PAGE_SIZE,
	    writing ? prot : prot & ~HAL_SPACE_WRITE) != HAL_OK)
		error = ENOMEM;
	if (error == 0) {
		if (writing) {
			vm_object_mapping_remove(source, page);
			page->object_page = NULL;
			page->object_next = NULL;
			private_page_attach_new(page, fresh);
			fresh = NULL;
			vm_page_track(page);
			page->flags &= ~VM_MAPPING_COW;
		}
		page->flags |= VM_MAPPING_MAPPED;
		vmspace_generation_advance_locked(vm);
	}
	page->flags &= ~VM_MAPPING_BUSY;
	region->hold_count--;
	vmspace_fault_wake_locked(vm);
	mutex_unlock(&vm->lock);
	vm_metadata_leave();
	if (fresh != NULL)
		vm_private_page_put(fresh);
	return error;
}
