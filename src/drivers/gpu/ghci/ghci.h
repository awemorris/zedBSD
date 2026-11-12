/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The generic driver of a GPU with the GPU Host Controller Interface
 * (GHCI 1.0): the device, the ring, the fences, the interrupt, the
 * recovery and the parts the driver's files share.
 *
 * One register window is one device.  The driver knows a GPU only by its
 * registers: GHCI_MAGIC and the major version say it is GHCI 1.0, and the
 * capability registers and the extended capability list say what it has.
 * Nothing in the driver names a vendor or branches on a minor version.
 * The bus glue (pci-attach.c) maps the register window and the optional
 * local window and fills bus_ops, which the start calls to take the
 * interrupt and to turn on bus mastering once the GPU has been reset; it
 * then hands the device to ghci_device_start().  On a bus that delivers
 * no interrupt of the GPU's the device is polled: a thread does the
 * interrupt handler's work every GHCI_POLL_MS, and IRQ_ENABLE stays zero.
 *
 * The ring.  The driver alone writes the ring (host memory, 256 KiB).
 * Every path that places packets passes the gate (ghci_cp_enter): while a
 * recovery runs, the gate lets go of every lock the caller holds and
 * waits for the recovery to end, and the caller starts again.  Every
 * submission ends with a CMD_FENCE that writes a 64-bit sequence into the
 * fence page and raises the fence interrupt; a ticket is that sequence
 * and the reset generation it was placed in.
 *
 * Faults.  Every fault, every watchdog expiry, a hang and a failed check
 * of the device are recovered by a reset of the device, whatever
 * STATUS.FAULTED says; only the service thread recovers.  A reset ends
 * every ticket placed before it that had not passed with an error.  A
 * device that does not come back from a reset, or faults too often, is
 * lost: every later submission fails, and the memory the GPU may still
 * reach is never given back while bus mastering is on.
 *
 * Locks, in the order they are taken: lock (a mutex: the sessions and
 * the device's life), display_lock (a mutex: the display), cp_lock (a
 * mutex: the ring, the doorbell, the reset and the submission log) and
 * irq_lock (a spinlock: the completed fence, the generations, the
 * recovery's mark and records, and the service thread's work).  Nothing
 * waits for a ticket while it holds lock or display_lock.
 */

#ifndef DRIVERS_GPU_GHCI_H
#define DRIVERS_GPU_GHCI_H

#include <stddef.h>
#include <stdint.h>

#include <drivers/gpu/gpu.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/waitq.h>

#include "ghci-regs.h"
#include "ghci-words.h"

/*
 * The ring the driver takes unless the bus glue asks for another size
 * (ring_bytes): 256 KiB of host memory.  Any size is a power of two
 * between 4 KiB and 16 MiB.
 */
#define GHCI_RING_BYTES			(256U * 1024U)

/* The CP_HEAP of a GPU without a page-table walker: session 0's 16 KiB root only. */
#define GHCI_FLAT_HEAP_BYTES		GHCI_ROOT_BYTES_VA32

/* The smallest CP_HEAP the start settles for when the full one cannot be had. */
#define GHCI_HEAP_MIN_BYTES		(64U * 1024U)

/* The bytes of the fence page; the device's fence is its first 64-bit word. */
#define GHCI_FENCE_PAGE_BYTES		GHCI_PAGE_BYTES

/*
 * How long the driver waits for the hardware: the memory to become ready
 * after a reset, the processor to halt, and a fence the driver itself
 * places to see that the processor runs (the liveness).
 */
#define GHCI_MEMORY_WAIT_MS		1000U
#define GHCI_HALT_WAIT_MS		1000U
#define GHCI_LIVENESS_MS		1000U

/* The watchdog time the driver programs into WDT_CYCLES. */
#define GHCI_WATCHDOG_MS		2000U

/*
 * The service thread wakes at least this often, and a fence that is
 * behind the last one placed while neither it nor RING_RPTR moves for
 * GHCI_HANG_MS counts as a hang.
 */
#define GHCI_SERVICE_PERIOD_MS		100U
#define GHCI_HANG_MS			10000U

/* How often a polled device, and a wait for the ring's room, look at the device. */
#define GHCI_POLL_MS			1U

/*
 * The faults that make a device lost: three faults no session caused,
 * or two CP_HEAP exhaustions, within GHCI_FAULT_STORM_MS; and recoveries
 * whose checks fail GHCI_RECOVERY_TRIES times in a row.
 */
#define GHCI_FAULT_STORM_MS		60000U
#define GHCI_FAULT_STORM		3U
#define GHCI_OOM_STORM			2U
#define GHCI_RECOVERY_TRIES		3U

/* The submissions the driver remembers until their fences pass. */
#define GHCI_SUBMIT_LOG			1024U

/* The fault records the interrupt keeps for the service thread. */
#define GHCI_FAULT_RECORDS		8U

/*
 * The fences of culprits' submissions one reset generation remembers as
 * failed although the device wrote them; past them, the generation's end
 * is moved back to before the first such fence.
 */
#define GHCI_CULPRIT_FENCES		32U

/* The ends of the reset generations the driver remembers, to answer an old ticket. */
#define GHCI_RESET_HISTORY		16U

/*
 * The ranges of local memory a boot display may still read, kept out of
 * the allocator until the release: the image being scanned out, the one
 * a pending flip will take, and the cursor's image.
 */
#define GHCI_BOOT_RANGES		3U

/* The values the start writes to SCRATCH and reads back. */
#define GHCI_SCRATCH_FIRST		0x5a5a5a5aU
#define GHCI_SCRATCH_SECOND		0xa5a5a5a5U

/* The work the service thread is woken for. */
#define GHCI_WORK_RECLAIM		1U
#define GHCI_WORK_RECOVER		2U
#define GHCI_WORK_STOP			4U
#define GHCI_WORK_DISPLAY		8U
#define GHCI_WORK_RESET			16U

/*
 * How long the GPU core's reset waits for the service thread to have
 * reset the device and seen a fence of the driver's come back.
 */
#define GHCI_CORE_RESET_WAIT_MS		30000U

/*
 * The display: the most modes the driver keeps of a controller (DC_CAPS
 * names up to seven), and the slot after them that holds the timing a
 * boot display was found running with.
 */
#define GHCI_DISPLAY_MODES_MAX		8U
#define GHCI_DISPLAY_BOOT_MODE		GHCI_DISPLAY_MODES_MAX

/*
 * The presents the display keeps waiting for the controller's one
 * pending flip of plane 0: two, and one more for a flip a reset gave
 * back.
 */
#define GHCI_DISPLAY_QUEUE		3U
#define GHCI_DISPLAY_QUEUE_DEPTH	2U

/* The held memory the display lets go of once its lock is let go. */
#define GHCI_DISPLAY_DROPS		16U

/* The outer locks a caller of the gate holds, which the gate lets go of when it waits. */
#define GHCI_HOLD_DEVICE		1U
#define GHCI_HOLD_DISPLAY		2U

/*
 * A request of ghci_vm_map's flags beside GHCI_VM_FLAG_EXEC: a mapping
 * the GPU may only read (and run, with EXEC), without GHCI_VM_FLAG_WRITE.
 * It is above the bits of CMD_VM_MAP's flags and never reaches a packet.
 */
#define GHCI_VM_MAP_NO_WRITE		(1U << 31)

/*
 * The life of a device: the start, the ring running, a recovery
 * resetting the device and replaying what it lost, a device that did not
 * come back, and a device whose detach has finished.
 */
enum ghci_device_state {
	GHCI_STATE_ATTACHING = 0,
	GHCI_STATE_RUNNING,
	GHCI_STATE_RECOVERING,
	GHCI_STATE_REPLAYING,
	GHCI_STATE_LOST,
	GHCI_STATE_DETACHED
};

/* What the gate answers: the ring is the caller's, a recovery ran, or the device is lost. */
enum ghci_gate {
	GHCI_GATE_OPEN = 0,
	GHCI_GATE_RETRY,
	GHCI_GATE_LOST
};

/* The state of a ticket: its fence has not passed, it has, or a reset ended it. */
enum ghci_ticket_state {
	GHCI_TICKET_PENDING = 0,
	GHCI_TICKET_DONE,
	GHCI_TICKET_FAILED
};

/* The kinds of memory an allocation is in: host RAM, or the GPU's local memory. */
enum ghci_memory_kind {
	GHCI_MEMORY_HOST = 1,
	GHCI_MEMORY_LOCAL
};

/*
 * The life of a GHCI session: open, the culprit of a fault until the
 * reset, waiting to be opened again after a reset, dead after a reset
 * (until its owner closes it), closing until its last fence passes,
 * retired (its number free again), and isolated by the GPU core because
 * its stop could not be confirmed.
 *
 * DEAD and ISOLATED differ in what the GPU may still do: a DEAD session
 * died in a reset that ended, so the GPU reaches none of its memory and
 * its unmaps and close give everything back at once.  An ISOLATED
 * session may still run in the GPU (no reset happened), so its unmaps
 * and close place nothing and give nothing back: its mappings, number
 * and chunks stay with the device until the next reset that ends.
 */
enum ghci_vm_state {
	GHCI_VM_OPEN = 1,
	GHCI_VM_FAULTED,
	GHCI_VM_REPLAY_WAIT,
	GHCI_VM_DEAD,
	GHCI_VM_CLOSING,
	GHCI_VM_RETIRED,
	GHCI_VM_ISOLATED
};

/*
 * The life of a mapping: in the page table, its CMD_VM_UNMAP placed and
 * its fence not yet passed, or given up by its owner in an isolated
 * session and held until the next reset that ends.
 */
enum ghci_mapping_state {
	GHCI_MAPPING_LIVE = 1,
	GHCI_MAPPING_UNMAPPING,
	GHCI_MAPPING_HELD
};

struct ghci_device;
struct ghci_arena;

/*
 * One run of physically contiguous memory the GPU reaches: host RAM, or
 * a run of the GPU's local memory.
 *
 * bus is the address the GPU uses (on the platforms the driver knows,
 * the processor's physical address; for local memory, its local address),
 * bytes its size in whole pages, view the processor's kernel view (the
 * direct map, or an uncached mapping on a bus that does not snoop the
 * caches; for local memory, its place in the local window, or NULL when
 * the window does not show it, which only the driver's own accesses use
 * while they hold the window open) and run the RAM it is (unused for
 * local memory).  The allocation lives until its owner gives it back
 * after the GPU is done with it; on a lost device it waits in the
 * quarantine instead.  holds counts what keeps a shared allocation (a
 * resource, an export, a mapping until its unmap's fence passed), and
 * reached says the GPU was ever given its address (lock).  scan_unsure
 * says the display controller may still read it, as the host could not
 * see the controller stop (lock): its last owner puts it in the
 * quarantine instead of giving it back.
 */
struct ghci_allocation {
	struct ghci_allocation *next;
	enum ghci_memory_kind kind;
	uint64_t bus;
	uint64_t bytes;
	uint8_t *view;
	struct kern_pmem run;
	unsigned holds;
	int reached;
	int scan_unsure;
};

/*
 * A submission's place in the fence order: the 64-bit sequence of the
 * CMD_FENCE placed after it and the reset generation it was placed in.
 * A reset ends every ticket of an older generation whose fence had not
 * passed with an error.
 */
struct ghci_ticket {
	uint64_t seq;
	uint64_t generation;
};

/*
 * The end of one reset generation: the fence the device had written when
 * the reset ended it, and the fences of the culprits' submissions the
 * device wrote although it skipped their work.  A ticket of that
 * generation passed if its sequence is at most completed and not one of
 * failed, and failed otherwise.  The driver remembers the last
 * GHCI_RESET_HISTORY generations; a ticket older than those is answered
 * as failed.
 */
struct ghci_reset_record {
	uint64_t generation;
	uint64_t completed;
	uint64_t failed[GHCI_CULPRIT_FENCES];
	unsigned failed_count;
};

/*
 * One submission whose fence has not been seen passed: the dwords of the
 * ring it took, its fence's sequence and the session whose work it was
 * (NULL for the driver's own, which includes every VM operation).  The
 * recovery reads the log to find which submission a fault of a GPU
 * without a page-table walker belongs to, and which sessions lost work
 * to a reset.
 */
struct ghci_submit_entry {
	uint32_t start;
	uint32_t end;
	uint64_t seq;
	struct ghci_vm *owner;
};

/*
 * What the interrupt saw of one fault or watchdog expiry, or what the
 * service saw of a hang, kept for the service thread with the recovery
 * generation it happened in: a record of an older generation is of a
 * fault a finished recovery has already reset.
 */
struct ghci_fault_record {
	uint64_t generation;
	uint32_t irq_status;
	uint32_t status;
	uint32_t fault_info;
	uint64_t fault_address;
	uint32_t session_active;
	uint32_t rptr;
	uint32_t cp_pc;
	uint32_t cp_status;
	uint64_t fence;
	int hang;
};

/*
 * The ring: its memory, its size in dwords and the next dword the driver
 * writes (cp_lock).
 */
struct ghci_ring {
	struct ghci_allocation *memory;
	uint32_t dwords;
	uint32_t wptr;
};

/*
 * The operations the bus glue gives the start: taking the interrupt (or
 * leaving the device polled), letting it go (EBUSY while a handler may
 * still run on another processor, after which the device's memory is
 * kept), and turning bus mastering on or off.  The start calls them only after the GPU has been reset, so no
 * interrupt or DMA a kernel before this one left behind reaches this one.
 */
struct ghci_bus_ops {
	int (*interrupt_take)(struct ghci_device *device);
	int (*interrupt_release)(struct ghci_device *device);
	int (*bus_master)(struct ghci_device *device, int on);
};

/*
 * One mapping of an allocation into a session's GPU addresses.
 *
 * va is the GPU address (the bus address on a GPU without a page-table
 * walker), arena the run of chunks it lies in (NULL without a walker),
 * flags its GHCI_VM_FLAG_* and replayed the recovery generation it was
 * last placed in.  An UNMAPPING mapping's ticket is the fence after its
 * CMD_VM_UNMAP; the mapping holds its allocation until that passes
 * (lock).
 */
struct ghci_mapping {
	struct ghci_mapping *next;
	struct ghci_vm *vm;
	struct ghci_allocation *allocation;
	struct ghci_arena *arena;
	uint64_t va;
	uint64_t bytes;
	uint32_t flags;
	enum ghci_mapping_state state;
	struct ghci_ticket ticket;
	uint64_t replayed;
};

/*
 * One GHCI session (a number 1 to 15, or session 0 for the driver) and
 * the GPU addresses it has.
 *
 * owner is the GPU core's session to report a loss to.  slot is the
 * number it holds among the fifteen; session is the number its packets
 * name (0 on a GPU without a page-table walker, where every session runs
 * as session 0).  state changes under the device's lock and cp_lock
 * together, so either one reads it.  The arenas are the runs of 2 MiB
 * chunks it owns, l0_marks the level 0 tables counted in its own page
 * table (with PROT), and counted whether its tables are in CP_HEAP's
 * count.  closed says that the owner of an isolated session gave it up:
 * the record stays with the device until a reset retires it.
 */
struct ghci_vm {
	void *owner;
	uint32_t slot;
	uint32_t session;
	int flat;
	enum ghci_vm_state state;
	int counted;
	int lost_work;
	int reported;
	int closed;
	struct ghci_ticket open_ticket;
	struct ghci_ticket close_ticket;
	struct ghci_mapping *mappings;
	struct ghci_arena *arenas;
	uint8_t l0_marks[GHCI_CHUNKS / 8U];
};

/*
 * One storage resource or blob of a session: the allocation it shows,
 * its renderer identity and its blob flags.  It holds the allocation
 * until it is destroyed (lock).
 */
struct ghci_resource {
	struct ghci_resource *next;
	struct ghci_allocation *allocation;
	uint32_t resource_id;
	uint32_t flags;
	int blob;
};

/*
 * One open of the device by the GPU core.  The device's lock protects
 * the list it is on and its resources.  vk is the Vulkan executor's side
 * of the open, from the open to the close (NULL on a device without the
 * executor).
 */
struct ghci_session {
	struct ghci_device *device;
	struct ghci_session *next;
	struct ghci_resource *resources;
	void *vk;
};

/*
 * One copy of rows the core of the driver asks the executor to make in
 * session 0 (a copied present's rows into a frame of the display): from
 * source to destination, both GPU addresses of session 0, rows rows of
 * bytes bytes (a multiple of 4), the rows of each side stride bytes
 * apart.  It lives on the caller's stack for the call.
 */
struct ghci_rows {
	uint64_t source;
	uint32_t source_stride;
	uint64_t destination;
	uint32_t destination_stride;
	uint32_t bytes;
	uint32_t rows;
};

/*
 * What the core of the driver asks of the Vulkan executor, which binds
 * the table at the start and keeps it until the detach: the executor's
 * side of a session's open and close, the memory a blob of an executor's
 * VkDeviceMemory shows, the completions the service thread retires, the
 * executor's own end at the detach, and a copy of rows in session 0 by
 * the driver's copy program.
 *
 *  - session_open fails only without memory and leaves nothing behind.
 *  - session_close cannot fail; it runs before the session's resources go.
 *  - memory_blob runs under the device's lock and returns the allocation
 *    of the session's VkDeviceMemory named by blob_id, or NULL.
 *  - service runs on the service thread and holds no lock of the device.
 *  - copy_rows places the copy as the driver's own work and gives its
 *    ticket, which the caller waits for before it lets go of either side;
 *    the caller makes one copy at a time and holds no lock of the device.
 *    It returns ENOTSUP, with nothing placed, when the copy program cannot
 *    make the copy (the compiler refuses it, or a side lies at or above
 *    4 GiB), ENOMEM, or EIO.
 */
struct ghci_executor_ops {
	int (*session_open)(struct ghci_device *device, struct ghci_session *session);
	void (*session_close)(struct ghci_device *device, struct ghci_session *session);
	struct ghci_allocation *(*memory_blob)(struct ghci_device *device, struct ghci_session *session, uint64_t blob_id);
	void (*service)(struct ghci_device *device);
	void (*detach)(struct ghci_device *device);
	int (*copy_rows)(struct ghci_device *device, const struct ghci_rows *rows, struct ghci_ticket *ticket);
};

/*
 * One mode of the display controller, as its mode table (MODE_INDEX and
 * the MODE registers) names it: the size, the refresh the clock and the
 * totals give, the porches and syncs, the sync polarity (SYNC_POL) and the
 * PCLK_SEL value that picks its pixel clock.  Read once at the attach.
 */
struct ghci_display_mode {
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	uint32_t h_front;
	uint32_t h_sync;
	uint32_t h_back;
	uint32_t v_front;
	uint32_t v_sync;
	uint32_t v_back;
	uint32_t sync_polarity;
	uint32_t clock_select;
};

/*
 * One image of plane 0 the display has recorded: a present's frame.
 *
 * sequence is the present's number in the lease (0 for none).  base is
 * the 32-bit physical address the controller scans, with the rows stride
 * bytes apart in the controller's format; mode is the mode the image
 * fills.  A shared image (a blob the session presents) is memory the
 * display holds until the controller no longer scans it; a copied one is
 * one of the driver's two frames (copy_frame 0 or 1, memory NULL).
 */
struct ghci_display_image {
	uint64_t sequence;
	uint32_t base;
	uint32_t stride;
	uint32_t format;
	unsigned mode;
	struct ghci_allocation *memory;
	int copy_frame;
};

/*
 * The one flip of a plane that the driver has placed in the ring and
 * whose record it has not seen yet.  The driver places a plane's next
 * flip only after this one's record, so the controller's one pending
 * flip per plane never makes the ring wait.
 *
 * value is the 31-bit value the record carries; light says the flip is
 * part of a run that lights the controller.  image is plane 0's,
 * surface the cursor surface of plane 1.
 */
struct ghci_display_flip {
	int placed;
	uint32_t value;
	int light;
	struct ghci_display_image image;
	int surface;
};

/*
 * The display controller of a device (display.c and display-scan.c, the
 * DISP2 capability): one output with a primary plane and, when the
 * controller has one, a cursor plane.  The ring changes what it shows
 * while the command processor runs: a present is a FLIP of a shared
 * image or of one of the driver's two frames into which it copied the
 * session's storage.
 *
 * The device's display_lock serializes everything here but topology and
 * irq_events, which irq_lock protects.  The registers, the identity and
 * the mode table are read at the attach and never change.
 *
 * The lease: owner and lease name the holder; ending says a lease ended
 * and the controller is still going dark (no claim until it is), and
 * dark_ticket is the fence of the packet that turns it off.  sequence is
 * the lease's last present number and adopted the highest one shown with
 * every lower one (replaced ones count); uncertain says an adoption did
 * not come in time, after which the lease's presents fail.
 *
 * What is lit: enabled, the mode, the format and the stride of plane 0;
 * shown is the image it scans.  flips[] are the planes' placed flips,
 * queue[] the presents still waiting for plane 0's flip, oldest first.
 * copy_frame_busy is the driver's frame a copied present is filling (-1
 * for none): no present is recorded until it is.  drops[] is held memory
 * let go of once the lock is let go (the device's lock frees it), and
 * unsure[] memory the controller may still read, which then goes to the
 * quarantine; host_unsure says the host turned the controller without
 * seeing the processor halt or the controller go idle.  The frames are
 * mapped into session 0 (frame_maps) where the blitter copies into them.
 * service_placing says the service thread places: the ring is then only
 * entered when it is free and has room at once.
 *
 * The cursor: surfaces[] are the driver's two images of plane 1;
 * cursor_shown is the surface the controller has (-1 for none) and
 * cursor_pending one to flip once the controller is lit (-1 for none).
 *
 * The boot display: boot_scanning says the controller still scans what
 * the firmware left (boot, in the mode slot GHCI_DISPLAY_BOOT_MODE),
 * until the driver's own first flip is taken.  boot_kept says the boot
 * image is still kept out of the allocators, boot_console that the
 * kernel's console may still draw to it (until kern_text_end() returns 0
 * for the device's ranges; the image is never given back before),
 * boot_release that the release is due once every lock is let go,
 * boot_end_due that the console is to be told it leaves the display once
 * every lock is let go, and boot_end_tried that it was (once only).
 */
struct ghci_display {
	int present;
	int cursor;
	uint32_t registers;
	uint32_t controller_id;
	uint32_t controller_version;
	uint32_t controller_caps;
	struct ghci_display_mode modes[GHCI_DISPLAY_MODES_MAX + 1U];
	unsigned mode_count;
	struct wait_queue flip_wake;
	uint64_t topology;
	uint32_t irq_events;
	uint32_t irq_enable;

	/* The connection and the output's generation. */
	int connected;
	uint64_t generation;
	uint64_t hotplug_tick;

	/* The lease. */
	struct ghci_session *owner;
	uint64_t lease;
	uint64_t next_lease;
	int ending;
	int dark_placed;
	struct ghci_ticket dark_ticket;
	uint64_t sequence;
	uint64_t adopted;
	uint64_t shown_time_ns;
	int uncertain;

	/* What the controller shows and what waits for it. */
	int enabled;
	unsigned mode;
	uint32_t format;
	uint32_t stride;
	struct ghci_display_image shown;
	struct ghci_display_flip flips[2];
	struct ghci_display_image queue[GHCI_DISPLAY_QUEUE];
	unsigned queue_count;
	uint32_t next_value;
	struct ghci_allocation *frames[2];
	struct ghci_mapping *frame_maps[2];
	int copy_frame_busy;
	struct ghci_allocation *drops[GHCI_DISPLAY_DROPS];
	unsigned drop_count;
	struct ghci_allocation *unsure[GHCI_DISPLAY_DROPS];
	unsigned unsure_count;
	int host_unsure;
	int service_placing;

	/* The cursor plane. */
	struct ghci_allocation *surfaces[2];
	int cursor_shown;
	int cursor_pending;
	int cursor_image;

	/*
	 * The driver's idle frame (the largest mode), which is scanned out
	 * when a sink is connected and no lease holds the display;
	 * idle_generation is the output's generation it was last lit for.
	 */
	struct ghci_allocation *idle_frame;
	uint64_t idle_generation;

	/* The boot display. */
	int boot_scanning;
	int boot_kept;
	int boot_console;
	int boot_release;
	int boot_end_due;
	int boot_end_tried;
	struct ghci_display_image boot;

	/* The scan-out errors: the count of this second, the mask, the reads that kept failing, and the totals. */
	unsigned error_count;
	uint64_t error_tick;
	int error_masked;
	unsigned read_errors;
	unsigned underflows;
	unsigned errors_total;
};

/* The most memory ranges the bus decodes for a device: BAR0 and BAR2. */
#define GHCI_BUS_RANGES 2U

/* One range of processor physical addresses the device decodes: a whole BAR. */
struct ghci_bus_range {
	uint64_t physical;
	uint64_t bytes;
};

/*
 * One GHCI device.
 *
 * The bus glue fills the bus fields before ghci_device_start(), and keeps
 * the structure until ghci_device_stop() has returned 0.  The capability
 * fields are read once at the start and never change.  The comment of
 * each group says which lock protects it.
 */
struct ghci_device {
	/*
	 * The bus, filled by the glue: its private record, its operations,
	 * the register window, the local window, the function's IDs, whether
	 * the device is polled, whether host memory is reached uncached (the
	 * bus does not snoop the caches), whether bus mastering was on when
	 * the driver found the function (a boot display may be using it), the
	 * ring's size in bytes (0: GHCI_RING_BYTES), and the processor physical
	 * ranges of the BARs mapped (bar_count of bars, each a whole BAR).
	 */
	void *bus;
	const struct ghci_bus_ops *bus_ops;
	volatile uint8_t *registers;
	uint32_t register_bytes;
	volatile uint8_t *window;
	uint64_t window_physical;
	uint64_t window_bytes;
	uint16_t pci_vendor;
	uint16_t pci_device;
	int polled;
	int host_uncached;
	int bus_master_found;
	uint32_t ring_bytes;
	struct ghci_bus_range bars[GHCI_BUS_RANGES];
	unsigned bar_count;

	/*
	 * What the start turned on, so that the detach and a failed start
	 * undo only that: the interrupt, and bus mastering.
	 */
	int interrupt_taken;
	int bus_master_set;

	/* The locks, the wait queues and the threads. */
	struct mutex lock;
	struct mutex display_lock;
	struct mutex cp_lock;
	struct spinlock irq_lock;
	struct wait_queue fence_wake;
	struct wait_queue ring_wake;
	struct wait_queue gate_wake;
	struct wait_queue service_wake;
	struct wait_queue poll_wake;
	unsigned service_work;
	int service_running;
	int poll_stop;
	int poll_running;

	/* The identification and the capabilities, read at the start. */
	uint32_t version;
	struct ghci_caps caps;
	uint32_t wdt_cycles;
	uint32_t reset_wait_ms;
	int executor;
	char name[32];

	/* The registration with the GPU core and the sessions (lock). */
	struct drv_gpu_ops ops;
	struct drv_gpu_device *gpu;
	struct ghci_session *sessions;
	unsigned session_count;
	int withdrawing;
	uint32_t resource_ids;

	/*
	 * The GHCI sessions and their GPU addresses (lock; a session's state
	 * also under cp_lock): session 0's record, the record of each number
	 * in use (vm[0] is vm0), the owner of each 2 MiB chunk of the 32-bit
	 * GPU addresses, what CP_HEAP may hold and holds by the driver's
	 * bound, and the level 0 tables counted in the page table every
	 * session shares (without PROT; they stay until a reset).
	 */
	struct ghci_vm vm0;
	struct ghci_vm *vm[GHCI_SESSIONS];
	struct ghci_vm *chunk_owner[GHCI_CHUNKS];
	uint64_t heap_budget;
	uint64_t heap_used;
	uint8_t l0_marks[GHCI_CHUNKS / 8U];

	/*
	 * The device's own memory: the ring, the fence page and CP_HEAP,
	 * from the start to the detach.  quarantined is memory the GPU may
	 * still reach on a lost device, given back only once bus mastering
	 * is off.
	 */
	struct ghci_ring ring;
	struct ghci_allocation *fence_page;
	struct ghci_allocation *heap;
	struct ghci_allocation *quarantined;

	/*
	 * The GPU's local memory on a discrete device (lock), from the start
	 * to the detach: local_ready says that the driver hands it out,
	 * local_map is the allocator of its offsets from LOCAL_BASE and
	 * local_extents the allocator's table.  What the boot display the
	 * firmware left running may still read of local memory is kept out
	 * of it (boot_reserved, boot_count page-rounded ranges in address
	 * order, each at boot_offset for boot_bytes) until the release after
	 * the driver's own first flip.
	 */
	int local_ready;
	struct ghci_range_map local_map;
	struct ghci_range_extent *local_extents;
	int boot_reserved;
	unsigned boot_count;
	uint32_t boot_offset[GHCI_BOOT_RANGES];
	uint32_t boot_bytes[GHCI_BOOT_RANGES];

	/*
	 * The local window (irq_lock): window_users counts what of the driver
	 * uses it (the kept boot framebuffer, which the boot console may
	 * write through it, and each hold of the driver's own accesses), and
	 * window_open is what the driver last wrote to WINDOW_CTRL.ENABLE:
	 * open while window_users is not 0, closed otherwise.
	 */
	unsigned window_users;
	int window_open;

	/*
	 * The ring's bookkeeping (cp_lock): the sequence of the last fence
	 * placed, and the submissions whose fences have not been seen passed,
	 * oldest first, in a circle of GHCI_SUBMIT_LOG.
	 */
	uint64_t fence_placed;
	struct ghci_submit_entry submit_log[GHCI_SUBMIT_LOG];
	unsigned submit_first;
	unsigned submit_count;

	/*
	 * The recovery placing packets of its own (cp_lock): while
	 * recovery_placing is set, a wait for the ring's room does not end
	 * for the recovery that is running, but for a new fault (the fault
	 * count moved from recovery_faults) or at recovery_deadline.
	 */
	int recovery_placing;
	unsigned recovery_faults;
	uint64_t recovery_deadline;

	/*
	 * What the interrupt, the gate and the service share (irq_lock).
	 *
	 * state is the device's life; it changes under cp_lock as well.
	 * irq_mask is the IRQ_ENABLE the handler restores after masking.
	 * completed is the fence the device has written, as the driver has
	 * accepted it; it does not move while recovering, so that no ticket
	 * passes on a fence a fault may have made meaningless.  issued is the
	 * last fence placed, which the hang check compares.  generation is
	 * the tickets' reset generation.  recovering is the mark that closes
	 * the gate; recovery_generation stamps the fault records and
	 * gate_generation wakes the gate when a recovery ends.  fault_count
	 * counts every fault and watchdog the handler saw, fence_interrupts
	 * every fence interrupt.  The records are the faults the service
	 * thread has not taken yet.
	 */
	enum ghci_device_state state;
	uint32_t irq_mask;
	uint64_t completed;
	uint64_t issued;
	uint64_t generation;
	int recovering;
	uint64_t recovery_generation;
	uint64_t gate_generation;
	unsigned fault_count;
	unsigned fence_interrupts;
	struct ghci_fault_record faults[GHCI_FAULT_RECORDS];
	unsigned fault_records;
	unsigned faults_dropped;
	struct ghci_reset_record history[GHCI_RESET_HISTORY];

	/*
	 * The hang check (irq_lock): the fence and RING_RPTR it last saw,
	 * and the tick it last saw either move.
	 */
	uint64_t progress_fence;
	uint32_t progress_rptr;
	uint64_t progress_tick;

	/*
	 * The service thread's own counts: the checks that failed in a row,
	 * the ticks of the recent faults no session caused and of the recent
	 * CP_HEAP exhaustions, and the resets done.
	 */
	unsigned recovery_failures;
	uint64_t storm_ticks[GHCI_FAULT_STORM];
	unsigned storm_count;
	uint64_t oom_ticks[GHCI_OOM_STORM];
	unsigned oom_count;
	unsigned resets;

	/*
	 * The Vulkan executor (executor_ops NULL without it) and its record,
	 * from the start to the detach.
	 */
	const struct ghci_executor_ops *executor_ops;
	void *vk;

	/*
	 * The GPU core's reset (irq_lock): reset_asked tells the service
	 * thread to run it, reset_answer is its outcome (0 or an error), and
	 * reset_rounds counts the resets the service ran for the core, so
	 * that the caller knows its own one ended.
	 */
	int reset_asked;
	int reset_answer;
	uint64_t reset_rounds;

	/* The display controller (display_lock), from the start to the detach. */
	struct ghci_display display;
};

int ghci_device_start(struct ghci_device *device);
int ghci_device_stop(struct ghci_device *device);
uint32_t ghci_read(struct ghci_device *device, uint32_t offset);
void ghci_write(struct ghci_device *device, uint32_t offset, uint32_t value);
void ghci_sleep_ms(unsigned milliseconds);
int ghci_wait_register(struct ghci_device *device, uint32_t offset, uint32_t mask, uint32_t value, unsigned milliseconds);
uint64_t ghci_fence_read(struct ghci_device *device);
void ghci_fence_write(struct ghci_device *device, uint64_t value);

int ghci_cp_start(struct ghci_device *device);
int ghci_cp_stop(struct ghci_device *device);
int ghci_cp_check_packets(const uint32_t *packets, uint32_t dwords);
enum ghci_gate ghci_cp_enter(struct ghci_device *device, unsigned held, int wait);
enum ghci_gate ghci_cp_try_enter(struct ghci_device *device, uint32_t need);
void ghci_cp_leave(struct ghci_device *device);
int ghci_cp_place(struct ghci_device *device, const uint32_t *packets, uint32_t dwords);
void ghci_cp_doorbell(struct ghci_device *device);
int ghci_cp_place_fence(struct ghci_device *device, struct ghci_vm *owner, uint32_t start, struct ghci_ticket *ticket);
int ghci_cp_submit(struct ghci_device *device, struct ghci_vm *vm, const uint32_t *packets, uint32_t dwords, struct ghci_ticket *ticket);
int ghci_cp_submit_placed(struct ghci_device *device, struct ghci_vm *vm, const uint32_t *packets, uint32_t dwords, struct ghci_ticket *ticket, int *placed);
int ghci_cp_wait(struct ghci_device *device, const struct ghci_ticket *ticket, unsigned milliseconds);
enum ghci_ticket_state ghci_cp_ticket(struct ghci_device *device, const struct ghci_ticket *ticket);
enum ghci_ticket_state ghci_cp_ticket_locked(struct ghci_device *device, const struct ghci_ticket *ticket);
int ghci_cp_await_fence(struct ghci_device *device, uint64_t seq, unsigned milliseconds);
void ghci_cp_ring_reset(struct ghci_device *device);
void ghci_cp_run(struct ghci_device *device);
void ghci_cp_irq_enable(struct ghci_device *device);
int ghci_cp_interrupt(void *argument);

int ghci_service_start(struct ghci_device *device);
void ghci_service_main(void *argument);
void ghci_service_step(struct ghci_device *device, unsigned work);
void ghci_service_stop(struct ghci_device *device);
void ghci_service_wake(struct ghci_device *device, unsigned work);
void ghci_recover_wait(struct ghci_device *device);
void ghci_recover_fault(struct ghci_device *device);
int ghci_recover_reset(struct ghci_device *device);
int ghci_poll_start(struct ghci_device *device);
void ghci_poll_main(void *argument);
void ghci_poll_stop(struct ghci_device *device);

int ghci_memory_alloc(struct ghci_device *device, uint64_t bytes, uint64_t limit, struct ghci_allocation **result);
int ghci_memory_alloc_local(struct ghci_device *device, uint64_t bytes, struct ghci_allocation **result);
int ghci_memory_local_start(struct ghci_device *device);
void ghci_memory_local_stop(struct ghci_device *device);
uint64_t ghci_memory_local_bytes(const struct ghci_device *device);
void ghci_memory_boot_release(struct ghci_device *device);
int ghci_memory_window_hold(struct ghci_device *device);
void ghci_memory_window_unhold(struct ghci_device *device);
int ghci_memory_local_read(struct ghci_device *device, const struct ghci_allocation *allocation, uint64_t offset, void *buffer, uint64_t bytes);
int ghci_memory_local_write(struct ghci_device *device, const struct ghci_allocation *allocation, uint64_t offset, const void *buffer, uint64_t bytes);
int ghci_memory_free(struct ghci_device *device, struct ghci_allocation *allocation);
void ghci_memory_quarantine(struct ghci_device *device, struct ghci_allocation *allocation);
void ghci_memory_release_quarantine(struct ghci_device *device);
uint64_t ghci_memory_limit(const struct ghci_device *device);
void ghci_memory_hold(struct ghci_allocation *allocation);
void ghci_memory_unhold(struct ghci_device *device, struct ghci_allocation *allocation);

int ghci_resource_add(struct ghci_device *device, struct ghci_session *session, struct ghci_allocation *allocation, int blob, uint32_t flags, struct ghci_resource **result);

void ghci_vm_init(struct ghci_device *device);
void ghci_vm_fini(struct ghci_device *device);
int ghci_vm_open(struct ghci_device *device, void *owner, struct ghci_vm **result);
void ghci_vm_close(struct ghci_device *device, struct ghci_vm *vm);
int ghci_vm_map(struct ghci_device *device, struct ghci_vm *vm, struct ghci_allocation *allocation, uint32_t flags, struct ghci_mapping **result);
void ghci_vm_unmap(struct ghci_device *device, struct ghci_mapping *mapping);
int ghci_vm_reclaim(struct ghci_device *device);
int ghci_vm_find_culprits(struct ghci_device *device, uint32_t fault_info, uint32_t active, int halted, uint64_t fence);
void ghci_vm_mark_lost(struct ghci_device *device, uint64_t fence);
void ghci_vm_after_reset(struct ghci_device *device);
int ghci_vm_replay(struct ghci_device *device);
uint32_t ghci_vm_active_mask(struct ghci_device *device);
void ghci_vm_settle(struct ghci_device *device);
unsigned ghci_vm_take_dead(struct ghci_device *device, void **owners, unsigned capacity);
void ghci_vm_isolate(struct ghci_device *device, struct ghci_vm *vm);

extern const struct drv_gpu_share_ops ghci_share_ops;

extern const struct drv_gpu_display_ops ghci_display_ops;
extern const struct drv_gpu_display_ops ghci_display_cursor_ops;
extern const struct drv_gpu_scanout_ops ghci_scanout_ops;

int ghci_display_attach(struct ghci_device *device);
void ghci_display_detach(struct ghci_device *device);
void ghci_display_session_close(struct ghci_device *device, struct ghci_session *session);
void ghci_display_service(struct ghci_device *device, unsigned work);
void ghci_display_interrupt(struct ghci_device *device);
void ghci_display_recover_count(struct ghci_device *device);
int ghci_display_panic(struct ghci_device *device);
int ghci_display_records_settle(struct ghci_device *device);
int ghci_adv7511_start(struct ghci_device *device, uint32_t registers);
int ghci_adv7511_sensed(struct ghci_device *device, uint32_t registers, int *connected);

#endif
