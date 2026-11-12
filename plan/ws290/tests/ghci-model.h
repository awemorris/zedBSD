/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A model of a GHCI 1.0 GPU for the driver's host tests.
 *
 * The model is the register window of BAR0 (the registers, the extended
 * capability list and its blocks), host memory and local memory that the
 * GPU reaches by bus address, and a command processor that reads the ring
 * when the doorbell rings.  The processor checks each packet's framing,
 * opcode and guard, runs CMD_FENCE, CMD_WAIT_FENCE and the session and VM
 * commands, and records every other command without running it: the tests
 * compare the recorded commands with what the driver meant to send.
 *
 * Where the specification leaves a reading open, the model has a switch
 * (struct model_switches) so that a test can run the driver against the
 * pessimistic reading as well.  The model is written from the same reading
 * of the specification as the driver, so a pass shows that the driver
 * sends what that reading asks for, not how the hardware behaves.
 */

#ifndef GHCI_MODEL_H
#define GHCI_MODEL_H

#include <stdint.h>

/* The model's host memory: 32 MiB at 256 MiB, below the GPU's host limit. */
#define MODEL_HOST_BASE			0x10000000ULL
#define MODEL_HOST_BYTES		(32U << 20)

/* The model's local memory (discrete profiles): 16 MiB at 2 GiB, all of it in the window. */
#define MODEL_LOCAL_BASE		0x80000000ULL
#define MODEL_LOCAL_BYTES		(16U << 20)

/* The register window the model keeps: the first 1 KiB of BAR0 (section 4.1). */
#define MODEL_WINDOW_BYTES		0x400U

/* The commands the model records; later ones are counted as dropped. */
#define MODEL_COMMANDS			4096U

/* The leading dwords of a command kept in its record. */
#define MODEL_COMMAND_WORDS		32U

/* The mappings one page table of the model holds. */
#define MODEL_MAPPINGS			512U

/* The page tables of the model: one per session, or one shared table without PROT. */
#define MODEL_TABLES			16U

/* The sessions of the GPU. */
#define MODEL_SESSIONS			16U

/* Where the blocks of the extended capability list are (section 4.1). */
#define MODEL_EXTCAP_LIST		0x100U
#define MODEL_FMT_BLOCK			0x140U
#define MODEL_VEND_BLOCK		0x180U
#define MODEL_PROT_BLOCK		0x1D0U
#define MODEL_MEM_BLOCK			0x1E0U
#define MODEL_DISP2_BLOCK		0x200U

/*
 * The capability profiles of the tests (design section 14).  Each is a
 * DEVICE_CAPS, EXT_CAP_FEATURE and EXT_CAP_FMT0 and a set of blocks.
 */
enum model_profile {
	MODEL_KC705_UMA = 0,
	MODEL_KC705_DISCRETE,
	MODEL_MINIMAL,
	MODEL_FULL,
	MODEL_FLAT,
	MODEL_PROT,
	MODEL_NO_BLT,
	MODEL_NO_SU,
	MODEL_DISCRETE,
	MODEL_MISMATCH,
	MODEL_KC705_MIN,
	MODEL_VA30,
	MODEL_BLT_MIN,
	MODEL_PROFILES
};

/* The driver-visible states of the command processor (section 5.2). */
enum model_state {
	MODEL_IDLE = 0,
	MODEL_RUNNING,
	MODEL_HALTED,
	MODEL_FAULTED
};

/*
 * The readings of the specification a test may switch to the pessimistic
 * side.  All zero is the reading of the specification as the driver's
 * design takes it.
 */
struct model_switches {
	int close_keeps_heap;		/* SESSION_CLOSE does not return its tables to CP_HEAP (N48) */
	int prot0_close_clears;		/* without PROT, SESSION_CLOSE clears the session's shared PTEs (N49) */
	int unmap_closed_faults;	/* VM_UNMAP of a closed session faults with kind 1 (N49) */
	unsigned reset_reads;		/* reads of CTRL that still see RESET after a reset */
	int reset_stuck;		/* a reset never ends and STATUS.AXI_STUCK reads 1 */
	unsigned halt_reads;		/* reads of STATUS before HALTED shows after HALT */
	unsigned mem_ready_reads;	/* reads of STATUS before MEM_READY shows after a reset */
	int scratch_stuck;		/* SCRATCH ignores writes (a window that does not answer) */
};

/*
 * One command the processor took, as the tests compare it: the opcode,
 * flags and length from the header, the session bound when it ran, its
 * first dwords, and whether the processor skipped it because its session
 * had faulted.
 */
struct model_command {
	uint32_t opcode;
	uint32_t flags;
	uint32_t dwords;
	uint32_t session;
	uint32_t skipped;
	uint32_t words[MODEL_COMMAND_WORDS];
};

/* One mapping of a model page table: a range of GPU VA, where it goes, its flags and its session. */
struct model_mapping {
	uint64_t va;
	uint64_t bytes;
	uint64_t phys;
	uint32_t flags;
	uint32_t session;
};

/*
 * A fault the test asks the model to raise: at once, or when the next
 * draw or dispatch of a session runs.  A watchdog raises IRQ_WATCHDOG with
 * IRQ_FAULT and kind 4.
 */
struct model_injection {
	int armed;
	int at_draw;
	uint32_t kind;
	uint32_t unit;
	uint32_t session;
	uint64_t address;
};

/*
 * The display controller of DISP2 (section 13.2) as the model keeps it
 * beside the registers it stores in the window: the sticky STATUS bits,
 * the display events and their mask, each plane's pending flip, the
 * records due while the processor cannot write them, and what the tests
 * check: refused writes (BAD_WRITE), the takes and cancellations of each
 * plane, the records written, FLIPs that had to wait for a pending one,
 * DISP_SETs that waited for idle while the controller was lit, and the
 * controller's own resets.
 */
struct model_display {
	uint32_t sticky;
	uint32_t irq_status;
	uint32_t irq_enable;
	int pending[2];
	uint32_t pending_base[2];
	uint32_t pending_value[2];
	uint32_t pending_record[2];
	int due[2];
	uint32_t due_value[2];
	uint32_t due_record[2];
	unsigned bad_writes;
	unsigned takes[2];
	unsigned cancels[2];
	unsigned records;
	unsigned flip_waits;
	unsigned idle_waits_lit;
	unsigned resets;
};

/* The interrupt handler the model calls while its line is high. */
typedef void (*model_irq_handler)(void *argument);

/*
 * The whole model.  The tests read it directly to check what the model
 * saw; they change it only through the functions below.
 */
struct model {
	enum model_profile profile;
	struct model_switches switches;
	uint32_t window[MODEL_WINDOW_BYTES / 4U];
	enum model_state state;
	uint32_t ctrl;
	uint32_t ring_wptr;
	uint32_t ring_rptr;
	uint32_t irq_status;
	uint32_t irq_enable;
	uint32_t fault_info;
	uint64_t fault_address;
	uint32_t session_active;
	uint32_t session_faulted;
	uint32_t bound_session;
	int waiting_fence;
	int hold;
	int bus_master;
	int heap_ready;
	uint64_t heap_used;
	uint64_t heap_leaked;
	int reset_hung;
	unsigned reset_reads_left;
	unsigned halt_reads_left;
	unsigned mem_ready_reads_left;
	uint64_t ticks;
	struct model_mapping tables[MODEL_TABLES][MODEL_MAPPINGS];
	uint32_t table_mappings[MODEL_TABLES];
	int table_root[MODEL_TABLES];
	struct model_command commands[MODEL_COMMANDS];
	unsigned command_count;
	unsigned commands_dropped;
	struct model_injection injection;
	model_irq_handler irq_handler;
	void *irq_argument;
	int in_handler;
	unsigned irq_calls;
	unsigned irq_left_high;
	unsigned doorbells;
	unsigned resets;
	unsigned halts;
	unsigned faults;
	unsigned dma_without_master;
	unsigned locked_writes;
	uint64_t host_next;
	uint8_t *host;
	uint8_t *local;
	struct model_display display;
};

/* The model the tests run; model_init fills it. */
extern struct model model;

/*
 * Nonzero makes FMT know R8_UNORM (every feature) and the two combined
 * depth and stencil formats (sampled, depth attachment) besides its own;
 * the transfer test sets it before its devices.  It lasts across models.
 */
extern int model_more_formats;

void model_init(enum model_profile profile, const struct model_switches *switches);
void model_fini(void);
uint32_t model_read(uint32_t offset);
void model_write(uint32_t offset, uint32_t value);
void model_run(void);
void model_tick(void);
void model_set_bus_master(int enabled);
void model_set_irq_handler(model_irq_handler handler, void *argument);
int model_irq_line(void);
void model_inject(uint32_t kind, uint32_t unit, uint32_t session, uint64_t address, int at_draw);
void model_inject_watchdog(uint32_t session);
void model_hold(int held);
int model_host_alloc(uint64_t bytes, uint64_t alignment, uint64_t max_address, uint64_t *bus);
uint8_t *model_dma(uint64_t bus, uint64_t bytes);
uint64_t model_heap_bytes(void);
const struct model_mapping *model_lookup(uint32_t session, uint64_t va);
unsigned model_count_commands(uint32_t opcode);
const struct model_command *model_last_command(uint32_t opcode);
void model_display_vsync(void);
void model_display_error(uint32_t sticky);
void model_display_sense(int connected);

#endif
