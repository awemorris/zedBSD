/* KA-T050 aggregate boot-header compile contract. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

/* This must be the first project header in every consumer class. */
#include <kern/boot.h>

#if (defined(KA_T050_KERNEL) + defined(KA_T050_HAL) + \
    defined(KA_T050_PC98) + defined(KA_T050_X68K)) != 1
#error "select exactly one KA-T050 consumer class"
#endif

/* Loader handoff ABI shared by every consumer class. */
_Static_assert(sizeof(struct boot_stage2_header) == 20,
    "Stage 2 header ABI changed");
_Static_assert(sizeof(struct boot_handoff) == 24,
    "boot handoff ABI changed");
_Static_assert(sizeof(struct bios_request) == 16,
    "BIOS request ABI changed");
_Static_assert(sizeof(struct boot_device) == 16,
    "boot device ABI changed");
_Static_assert(__builtin_offsetof(struct boot_handoff,
    boot_partition_lba) == 20, "boot partition LBA offset changed");

#if defined(KA_T050_KERNEL)

/* The parsed contract is deliberately pointer-free and ABI-neutral. */
_Static_assert(KERN_BOOT_PARAMETER_BOOT0 == 0,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_BOOT3 == 3,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_ROOTPART == 4,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_OVERLAY_ROOT == 5,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_OVERLAY_DATA == 6,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_SWAP0 == 7,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_SWAP3 == 10,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_INIT == 11,
    "boot parameter key numbering changed");
_Static_assert(KERN_BOOT_PARAMETER_COUNT == 12,
    "boot parameter key count changed");
_Static_assert(sizeof(struct kern_boot_parameters) == 3136,
    "parsed boot-parameter ABI changed");

_Static_assert(KERN_BOOT_ROOT_INVALID == 0,
    "boot root-mode numbering changed");
_Static_assert(KERN_BOOT_ROOT_NATIVE == 1,
    "boot root-mode numbering changed");
_Static_assert(KERN_BOOT_ROOT_OVERLAY == 2,
    "boot root-mode numbering changed");
_Static_assert(KERN_BOOT_SOURCE_FAILURE_NONE == 0,
    "boot failure-stage numbering changed");
_Static_assert(KERN_BOOT_SOURCE_FAILURE_MOUNT == 6,
    "boot failure-stage numbering changed");
_Static_assert(KERN_BOOT_SOURCE_SLOT_COUNT == 4,
    "boot source slot count changed");

int
ka_t050_kernel_contract(void)
{
	struct kern_boot_parameters parameters;
	struct kern_boot_source_context context;
	struct kern_boot_source_reference reference;
	enum kern_boot_root_mode mode;

	(void)parameters;
	(void)context;
	(void)reference;
	(void)mode;
	(void)kern_boot_parameters_parse;
	(void)kern_boot_parameters_initialize;
	(void)kern_boot_parameters_current;
	(void)kern_boot_source_selector_validate;
	(void)kern_boot_source_reference_parse;
	(void)kern_boot_source_root_mode;
	(void)kern_boot_source_fat_type_supported;
	(void)kern_boot_source_context_mount;
	(void)kern_boot_source_publish_runtime;
	(void)kern_boot_source_context_destroy;
	return 0;
}

#elif defined(KA_T050_HAL)

int
ka_t050_hal_contract(const struct boot_handoff *handoff)
{
	return handoff != (const struct boot_handoff *)0 &&
	    handoff->magic == ZEDBSD_HANDOFF_MAGIC;
}

#elif defined(KA_T050_PC98)

#include <boot/pc98-handoff.h>

_Static_assert(sizeof(struct zedbsd_pc98_parameter_handoff) ==
    ZEDBSD_PC98_PARAMETER_HANDOFF_SIZE,
    "PC-98 parameter handoff ABI changed");
_Static_assert(__builtin_offsetof(struct zedbsd_pc98_parameter_handoff,
    parameters) == ZEDBSD_PC98_PARAMETER_RECORD_OFFSET,
    "PC-98 parameter record offset changed");

int
ka_t050_pc98_contract(const struct zedbsd_pc98_parameter_handoff *handoff)
{
	return handoff != (const struct zedbsd_pc98_parameter_handoff *)0 &&
	    handoff->common.version == ZEDBSD_HANDOFF_VERSION_PC98;
}

#elif defined(KA_T050_X68K)

_Static_assert(sizeof(struct boot_memory_region32) == 12,
    "X68k memory-region ABI changed");
_Static_assert(sizeof(struct x68k_boot_handoff) == 104,
    "X68k handoff ABI changed");
_Static_assert(__builtin_offsetof(struct x68k_boot_handoff,
    memory_regions) == 56, "X68k memory-region offset changed");

int
ka_t050_x68k_contract(const struct x68k_boot_handoff *handoff)
{
	return handoff != (const struct x68k_boot_handoff *)0 &&
	    handoff->extension_magic == ZEDBSD_X68K_HANDOFF_MAGIC;
}

#endif
