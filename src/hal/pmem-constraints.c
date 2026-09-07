/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>

/*
 * Supplies checked compatibility for HALs without constrained pool searches.
 * An out-of-range allocation is released, never truncated or returned as a
 * success. HALs with extent search provide a strong implementation.
 */
int __attribute__((weak))
hal_pmem_alloc_range(
	const struct hal_pmem_request *request,
	uint64_t minimum,
	uint64_t maximum,
	uint64_t boundary,
	struct hal_pmem *desc)
{
	struct hal_pmem_request adjusted;
	struct hal_pmem memory = {0};
	struct hal_pmem empty = {0};
	int error;

	if (request == NULL || desc == NULL || request->type != HAL_PMEM_TYPE_RAM ||
	    request->paddr != HAL_PMEM_PADDR_ANY || request->size == 0 || request->attr != 0 ||
	    (request->alignment != 0 && (request->alignment & (request->alignment - 1U)) != 0) ||
	    minimum > maximum ||
	    (boundary != 0 && (boundary & (boundary - 1U)) != 0))
		return HAL_ERR_INVALID;
	*desc = empty;
	adjusted = *request;
	if (boundary != 0 && boundary <= SIZE_MAX && adjusted.alignment < boundary)
		adjusted.alignment = (size_t)boundary;
	error = hal_pmem_alloc(&adjusted, &memory);
	if (error != HAL_OK)
		return error;
	if (memory.vaddr != NULL && memory.size >= request->size && memory.type == HAL_PMEM_TYPE_RAM &&
	    memory.attr == 0 && (adjusted.alignment == 0 || (memory.paddr & (adjusted.alignment - 1U)) == 0) &&
	    memory.paddr >= minimum && memory.paddr <= maximum &&
	    (uint64_t)memory.size - 1U <= maximum - memory.paddr &&
	    (boundary == 0 || memory.size <= boundary - memory.paddr % boundary)) {
		*desc = memory;
		return HAL_OK;
	}
	error = hal_pmem_free(&memory);
	if (error != HAL_OK) {
		*desc = memory;
		return HAL_ERR_STATE;
	}
	return HAL_ERR_NOMEM;
}
