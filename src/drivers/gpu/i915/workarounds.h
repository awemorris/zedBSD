/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GT, engine and context workarounds, the register whitelist, MOCS and PAT.
 *
 * Every workaround is kept as an explicit list of (register, clear, set,
 * kind) entries with its workaround name attached.  A list can be dumped
 * and compared with another dump, applied to the hardware, and read back.
 */

#ifndef DRIVERS_GPU_I915_WORKAROUNDS_H
#define DRIVERS_GPU_I915_WORKAROUNDS_H

#include <stdint.h>

#include "gt-power.h"

struct i915_mmio;
struct i915_gt_info;
struct i915_engine_info;
struct mutex;

/* How many entries one workaround list holds. */
#define I915_WA_MAX		48

/* How many MOCS entries the table has (I915_GT_GEN9_NUM_MOCS_ENTRIES). */
#define I915_MOCS_ENTRIES	64

/* How many engines the per-engine lists are kept for. */
#define I915_WA_ENGINES		6

/*
 * How the apply and verify passes treat one workaround entry.
 *
 * The kind is explicit rather than inferred from the masks because the two
 * passes behave differently for each.
 */
enum i915_wa_kind {
	/*
	 * The clear and set masks are real bits: the apply pass reads,
	 * modifies and writes, and the readback must match the set bits
	 * within the read mask.
	 */
	I915_WA_PLAIN = 0,

	/*
	 * A masked register: the upper 16 bits select which of the lower 16
	 * change, so the value carries its own mask and nothing is read.
	 */
	I915_WA_MASKED,

	/*
	 * Written like a plain entry, but the readback is not trustworthy
	 * (FF_MODE2 under Wa_1608008084, MISCCPCTL under Wa_14015795083).
	 */
	I915_WA_NO_VERIFY
};

/*
 * One register a workaround programs.
 *
 * Entries live inside their list and are never freed.  In a whitelist the
 * register field holds the whitelisted address with its access flags, which
 * is the word written into a non-privileged slot.
 */
struct i915_wa {
	/* The register, or in a whitelist the address and access flags. */
	uint32_t reg;

	/* The bits the apply pass clears before it sets. */
	uint32_t clr;

	/* The value the apply pass sets. */
	uint32_t set;

	/* The bits the verify pass compares; zero means nothing is compared. */
	uint32_t read_mask;

	/* One of enum i915_wa_kind. */
	int kind;

	/* Nonzero for a multicast (MCR) register. */
	int is_mcr;

	/* The workaround name, a static string, for the manifest. */
	const char *name;
};

/*
 * One workaround list: the GT list, or the engine, context or whitelist list
 * of one engine.
 *
 * It is embedded in the GT initialization state and rebuilt from nothing by
 * the table construction.
 */
struct i915_wa_list {
	struct i915_wa list[I915_WA_MAX];

	/* How many entries are in use. */
	unsigned count;

	/* How many entries were refused because the list was full. */
	unsigned overflow;

	/* The list name, a static string, for the manifest. */
	const char *name;
};

/*
 * What applying one list to the hardware did.
 */
struct i915_wa_apply_result {
	/* Entries written, and plain entries left alone because nothing changed. */
	unsigned written;
	unsigned skipped_unchanged;

	/* Entries read back and found right, and found wrong. */
	unsigned verified;
	unsigned mismatched;

	/* Entries whose readback is not trusted and was not compared. */
	unsigned not_verifiable;
};

/*
 * The MOCS table the GT is programmed with.
 *
 * It is built once by the table construction; every entry the platform
 * table leaves undefined takes the values of the unused-entries index.
 */
struct i915_mocs {
	/* The control value and the L3 cache control of every entry. */
	uint32_t control[I915_MOCS_ENTRIES];
	uint16_t l3cc[I915_MOCS_ENTRIES];

	/* How many entries are programmed. */
	unsigned n_entries;

	/* The uncached entry, and the entry undefined indices copy. */
	unsigned uc_index;
	unsigned unused_entries_index;

	/* Nonzero once a table has been built for this platform. */
	int valid;

	/*
	 * Nonzero when every engine has a MOCS table of its own (Gen9) rather
	 * than one global table (Gen12); the engine resume programs it.
	 */
	int engine_mocs;
};

/*
 * Everything the GT start builds before the first submission, and what
 * programming it into the hardware did.
 *
 * The device start owns it from the table construction to the device stop;
 * the context and verification code reads the per-engine lists.
 */
struct i915_gt_init {
	/* The GT list, and the engine, context and whitelist lists per engine. */
	struct i915_wa_list gt_wa;
	struct i915_wa_list engine_wa[I915_WA_ENGINES];
	struct i915_wa_list ctx_wa[I915_WA_ENGINES];
	struct i915_wa_list whitelist[I915_WA_ENGINES];

	/* The MOCS table, and the RC6 and RPS state. */
	struct i915_mocs mocs;
	struct i915_rc6 rc6;
	struct i915_rps rps;

	/* Nonzero once all eight PAT entries have been written. */
	int pat_programmed;

	/* What applying the GT and engine lists did. */
	struct i915_wa_apply_result gt_wa_applied;
	struct i915_wa_apply_result engine_wa_applied[I915_WA_ENGINES];

	/* How many MOCS, L3 cache control and whitelist registers were written. */
	unsigned mocs_global_writes;
	unsigned mocs_l3cc_writes;
	unsigned whitelist_writes;

	/* How many engines had their workarounds applied on resume. */
	unsigned engines_resumed;

	/* Nonzero once the tables are built, and once the GT was programmed. */
	int tables_built;
	int hw_programmed;
};

void drv_i915_wa_add_or(struct i915_wa_list *wal, uint32_t reg, uint32_t set, int is_mcr, const char *name);
void drv_i915_wa_add_field(struct i915_wa_list *wal, uint32_t reg, uint32_t clr, uint32_t set, int is_mcr, const char *name);
void drv_i915_wa_add_masked_bits(struct i915_wa_list *wal, uint32_t reg, uint32_t bits, int is_mcr, const char *name);
void drv_i915_wa_add_masked_field(struct i915_wa_list *wal, uint32_t reg, uint32_t mask, uint32_t value, int is_mcr, const char *name);
void drv_i915_wa_add_no_verify(struct i915_wa_list *wal, uint32_t reg, uint32_t clr, uint32_t set, int is_mcr, const char *name);

void drv_i915_wa_build_gt_gen12(struct i915_wa_list *wal, const struct i915_gt_info *gt);
void drv_i915_wa_build_gt_gen11(struct i915_wa_list *wal, const struct i915_gt_info *gt);
void drv_i915_wa_build_gt_gen9(struct i915_wa_list *wal, const struct i915_gt_info *gt);
void drv_i915_wa_build_engine(struct i915_wa_list *wal, const struct i915_engine_info *engine, const struct i915_gt_info *gt, unsigned mocs_uc_index);
void drv_i915_wa_build_context(struct i915_wa_list *wal, const struct i915_engine_info *engine, int graphics_ver, unsigned mocs_uc_index);
void drv_i915_wa_build_whitelist(struct i915_wa_list *wal, const struct i915_engine_info *engine, int graphics_ver);

void drv_i915_wa_list_apply(const struct i915_wa_list *wal, struct i915_mmio *mmio, int verify, struct i915_wa_apply_result *result);
void drv_i915_engine_apply_whitelist(const struct i915_wa_list *wal, const struct i915_engine_info *engine, struct i915_mmio *mmio);
void drv_i915_wa_list_dump(const struct i915_wa_list *wal, const char *what);

void drv_i915_mocs_build(struct i915_mocs *table, int graphics_ver);
void drv_i915_mocs_program(const struct i915_mocs *table, struct i915_mmio *mmio, unsigned *global_writes, unsigned *l3cc_writes);
void drv_i915_mocs_program_l3cc(const struct i915_mocs *table, struct i915_mmio *mmio, unsigned *writes);
void drv_i915_mocs_program_engine(const struct i915_mocs *table, const struct i915_engine_info *engine, struct i915_mmio *mmio, unsigned *writes);

void drv_i915_pat_program_gen12(struct i915_mmio *mmio, unsigned *writes);
void drv_i915_pat_program_gen11(struct i915_mmio *mmio, unsigned *writes);
void drv_i915_pat_program_gen9(struct i915_mmio *mmio, unsigned *writes);

void drv_i915_gt_init_tables(struct i915_gt_init *gt_init, const struct i915_gt_info *gt, int graphics_ver, struct mutex *sb_lock, struct i915_mmio *mmio);
void drv_i915_gt_program(struct i915_gt_init *gt_init, const struct i915_gt_info *gt, struct i915_mmio *mmio);
void drv_i915_engine_apply_resume_wa(struct i915_gt_init *gt_init, const struct i915_gt_info *gt, unsigned index, struct i915_mmio *mmio);

#endif
