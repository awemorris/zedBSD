/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <kern/io-scratch.h>
#include <string.h>

extern unsigned hal_vmap_capabilities(void) __attribute__((weak));
extern int hal_vmap_reserve(size_t, struct hal_vmap **) __attribute__((weak));
extern int hal_vmap_populate(struct hal_vmap *, uint64_t, uint64_t) __attribute__((weak));
extern int hal_vmap_pin(struct hal_vmap *, void **) __attribute__((weak));
extern void hal_vmap_unpin(struct hal_vmap *) __attribute__((weak));
extern int hal_vmap_release(struct hal_vmap *) __attribute__((weak));

/* The caller charges its own pool/worker class once, after successful admission. */
int
io_scratch_alloc(
	size_t size,
	int allow_vmap,
	struct io_scratch *result)
{
	struct hal_pmem_request request;
	struct io_scratch scratch;
	size_t page, rounded;
	int error;

	if (result == NULL)
		return HAL_ERR_INVALID;
	memset(result, 0, sizeof(*result));
	page = hal_page_get_page_size(1);
	if (size == 0 || page == 0 || size > SIZE_MAX - (page - 1U))
		return HAL_ERR_INVALID;
	rounded = (size + page - 1U) / page * page;
	memset(&scratch, 0, sizeof(scratch));
	memset(&request, 0, sizeof(request));
	request.paddr = HAL_PMEM_PADDR_ANY;
	request.size = rounded;
	request.alignment = page;
	request.type = HAL_PMEM_TYPE_RAM;
	error = hal_pmem_alloc(&request, &scratch.physical);
	if (error == HAL_OK && scratch.physical.vaddr != NULL &&
	    scratch.physical.size >= rounded) {
		scratch.vaddr = scratch.physical.vaddr;
		scratch.size = scratch.physical.size;
		*result = scratch;
		return HAL_OK;
	}
	if (scratch.physical.size != 0 && hal_pmem_free(&scratch.physical) != HAL_OK)
		HAL_FATAL("scratch physical rollback failed");
	memset(&scratch.physical, 0, sizeof(scratch.physical));
	if (!allow_vmap || rounded > HAL_VMAP_MAX_SIZE ||
	    hal_vmap_capabilities == NULL || hal_vmap_reserve == NULL ||
	    hal_vmap_populate == NULL || hal_vmap_pin == NULL ||
	    hal_vmap_unpin == NULL || hal_vmap_release == NULL ||
	    hal_vmap_capabilities() == 0)
		return HAL_ERR_NOMEM;
	error = hal_vmap_reserve(rounded, &scratch.mapping);
	if (error != HAL_OK)
		return error;
	error = hal_vmap_populate(scratch.mapping, 0, UINT64_MAX);
	if (error == HAL_OK)
		error = hal_vmap_pin(scratch.mapping, &scratch.vaddr);
	if (error != HAL_OK) {
		if (hal_vmap_release(scratch.mapping) != HAL_OK)
			HAL_FATAL("scratch vmap rollback failed");
		return error;
	}
	scratch.size = rounded;
	*result = scratch;
	return HAL_OK;
}

/* The scratch owner's pin spans every borrower until its existing drain gate. */
int
io_scratch_free(
	struct io_scratch *scratch)
{
	int error;

	if (scratch == NULL || scratch->vaddr == NULL || scratch->size == 0)
		return HAL_ERR_INVALID;
	if (scratch->mapping != NULL) {
		hal_vmap_unpin(scratch->mapping);
		error = hal_vmap_release(scratch->mapping);
		if (error != HAL_OK && hal_vmap_pin(scratch->mapping, &scratch->vaddr) != HAL_OK)
			HAL_FATAL("scratch vmap release lost ownership");
	} else {
		error = hal_pmem_free(&scratch->physical);
	}
	if (error == HAL_OK)
		memset(scratch, 0, sizeof(*scratch));
	return error;
}
