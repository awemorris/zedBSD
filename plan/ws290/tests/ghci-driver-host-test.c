/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The host test of the GHCI 1.0 driver's core (ghci.c, cp.c, recover.c,
 * memory.c) against the GPU model (ghci-model.c) and the kernel stubs
 * (ghci-stubs.c): the attach, the order of the registers, submissions,
 * fences, the interrupt and the poll, the ring's room, device faults and
 * the recovery, hangs, the loss of a device, the gate and the detach
 * (design section 14, the scenes of p004).
 *
 * The test runs on one thread.  A wait of the driver is one tick of the
 * fake clock, in which the model runs and the scene's action may run:
 * the poll thread's work on a polled device, a fault, or the service
 * thread's recovery while the driver waits at the gate.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/gpu/gpu.h"
#include "drivers/gpu/ghci/display-scan.h"
#include "drivers/gpu/ghci/ghci.h"
#include "drivers/gpu/ghci/ghci-cmd.h"
#include "drivers/gpu/ghci/ghci-regs.h"
#include "kern/clock.h"
#include "kern/lock.h"
#include "uapi/errno.h"
#include "uapi/gpu.h"
#include "uapi/gpu-allocation.h"
#include "uapi/gpu-display.h"
#include "uapi/gpu-scanout.h"
#include "ghci-model.h"
#include "ghci-stubs.h"

/* The NOP dwords of one ordinary submission of the scenes. */
#define TEST_SUBMIT_DWORDS		200U

/* The dwords of the submission larger than the ring (64 Ki dwords). */
#define TEST_LARGE_DWORDS		70000U

/* The submissions of the wrap scene. */
#define TEST_SUBMITS			1000U

/* The milliseconds the scenes wait for a ticket. */
#define TEST_WAIT_MS			100U

/* The mappings the replay scene makes: their CMD_VM_MAPs are more than a 4 KiB ring holds. */
#define REPLAY_MAPPINGS			120U

/* The PCI IDs the scenes give the device (the KC705 board). */
#define TEST_PCI_VENDOR			0x10eeU
#define TEST_PCI_DEVICE			0x7024U

/* The watchdog cycles the driver programs at the model's 100 MHz clock (2000 ms). */
#define TEST_WDT_CYCLES			200000000U

/*
 * The counts of the checks that held and that failed; main reports both
 * and fails when any check failed.
 */
static unsigned checks_passed;
static unsigned checks_failed;

/* The device of the scene that runs; start_scene fills it. */
static struct ghci_device device;

/*
 * How the fake bus behaves in the scene: polled leaves the device
 * without an interrupt, master_found says the firmware left bus
 * mastering on, master_broken has the bus turn it on without the model
 * seeing it (the GPU never fetches).
 */
static int test_polled;
static int test_master_found;
static int test_master_broken;
static unsigned test_release_busy;
static uint32_t test_ring_bytes;

/*
 * What the fake bus saw: the calls, and the state of the model at the
 * first interrupt and the first bus master: whether a reset had run,
 * whether RUN was off and IRQ_STATUS clear, whether the ring was set.
 */
static unsigned interrupt_takes;
static unsigned interrupt_releases;
static unsigned master_on_calls;
static unsigned master_off_calls;
static int irq_after_reset;
static int master_before_run;
static int ring_before_master;
static int release_after_quiet;
static int master_off_after_release;

/*
 * The scene's action at each sleep of the driver, the number of sleeps
 * it skips first, and the guard that keeps it from running inside its own
 * sleeps (a recovery sleeps too).
 */
/* The owner the scenes give their GHCI sessions, which the core hears of when one dies. */
static int owner_token;

static void (*scene_action)(void);
static unsigned scene_action_after;
static int scene_action_running;
static unsigned scene_action_runs;

static void expect(int held, const char *name);
static int start_scene(enum model_profile profile, const struct model_switches *switches);
static void prepare_scene(enum model_profile profile, const struct model_switches *switches);
static int start_device(void);
static void end_scene(const char *name);
static void check_locks(const char *name);
static void model_irq(void *argument);
static void sleep_hook(unsigned sleep, void *argument);
static int fake_interrupt_take(struct ghci_device *target);
static int fake_interrupt_release(struct ghci_device *target);
static int fake_bus_master(struct ghci_device *target, int on);
static int submit_nops(uint32_t dwords, struct ghci_ticket *ticket);
static void action_recover(void);
static void action_stall_then_fault(void);
static void action_session_fault(void);
static void action_session_fault_once(void);
static void action_replay_fault(void);
static uint32_t heap_bytes(void);
static uint32_t dc_read(uint32_t reg);
static void display_round(void);
static int display_take(void);
static struct ghci_resource *display_storage(uint64_t bytes, int blob);
static uint32_t display_routes(void);
static int display_present(struct ghci_resource *storage, uint32_t width, uint32_t height, uint32_t stride, uint64_t offset, uint32_t flags, int wait, uint64_t *sequence);
static void display_pattern(struct ghci_resource *storage, uint32_t seed);
static int display_frame_holds(uint64_t frame, uint32_t frame_stride, struct ghci_resource *storage, uint32_t width, uint32_t height, uint32_t stride);
static void display_give_up(void);
static void boot_display_host(void);
static void scene_display_modes(void);
static void scene_display_present(void);
static void scene_display_copied(void);
static void scene_display_cursor(void);
static void scene_display_hotplug(void);
static void scene_display_errors(void);
static void scene_display_reset(void);
static void scene_display_boot(void);
static void scene_display_boot_end(void);
static void boot_end_relight(void *argument);
static void boot_end_bars(void);
static void scene_display_panic(void);
static void scene_display_detach(void);
static void scene_display_none(void);
static void scene_display_service_nowait(void);
static void scene_display_gate_keeps(void);
static void scene_display_stop_records(void);
static void scene_display_host_unsure(void);
static void scene_display_detach_after_end(void);
static void scene_display_light_cut(void);
static void scene_display_frame_bytes(void);
static void scene_display_release_wait(void);
static void ring_fill(void);
static void action_unhold(void);
static void scene_versions(void);
static void scene_attach_checks(void);
static void scene_attach_policy(void);
static void scene_va30_chunks(void);
static void scene_submit(void);
static void scene_polled(void);
static void scene_device_fault(void);
static void scene_fence_before_fault(void);
static void scene_stale_record(void);
static void scene_hang(void);
static void scene_long_submit(void);
static void scene_room_wait(void);
static void scene_gate(void);
static void scene_lost(void);
static void scene_storm(void);
static void scene_check_fails(void);
static void scene_detach(void);
static struct ghci_allocation *host_memory(uint64_t bytes);
static void memory_done(struct ghci_allocation *allocation);
static void reclaim(void);
static int submit_vm(struct ghci_vm *vm, struct ghci_ticket *ticket);
static void scene_sessions(void);
static void scene_mappings(void);
static void scene_flat(void);
static void scene_heap_bound(void);
static void scene_close_order(void);
static void scene_replay(void);
static void scene_culprit(void);
static void scene_lost_work(void);
static void scene_vm_retry(void);
static void scene_resources(void);
static void scene_vm_edges(void);
static void scene_reset_mem_ready(void);
static void action_fault_then_recover(void);
static void scene_local(void);
static void scene_local_lost(void);
static void scene_boot_ranges(void);
static void scene_local_fallbacks(void);
static void scene_lost_culprit(void);
static void boot_display(uint32_t ctrl, uint64_t base, uint32_t stride, uint32_t height);
static void extcap_rename(uint32_t from, uint32_t to);
static struct ghci_allocation *local_memory(uint64_t bytes);
static int quarantined_local(void);

/*
 * The operations of the fake bus; they live as long as the test.
 */
static const struct ghci_bus_ops fake_bus_ops = {
	fake_interrupt_take,
	fake_interrupt_release,
	fake_bus_master
};

/*
 * Runs the scenes and reports.
 */
int
main(void)
{
	/* Keeps the driver's log out of the report. */
	stub.quiet = 1;

	/* Runs the scenes. */
	scene_versions();
	scene_attach_checks();
	scene_attach_policy();
	scene_submit();
	scene_polled();
	scene_device_fault();
	scene_fence_before_fault();
	scene_stale_record();
	scene_hang();
	scene_long_submit();
	scene_room_wait();
	scene_gate();
	scene_lost();
	scene_storm();
	scene_check_fails();
	scene_detach();
	scene_sessions();
	scene_mappings();
	scene_flat();
	scene_heap_bound();
	scene_close_order();
	scene_replay();
	scene_culprit();
	scene_lost_work();
	scene_vm_retry();
	scene_resources();
	scene_vm_edges();
	scene_reset_mem_ready();
	scene_local();
	scene_local_lost();
	scene_boot_ranges();
	scene_local_fallbacks();
	scene_lost_culprit();
	scene_display_modes();
	scene_display_present();
	scene_display_copied();
	scene_display_cursor();
	scene_display_hotplug();
	scene_display_errors();
	scene_display_reset();
	scene_display_boot();
	scene_display_boot_end();
	scene_display_panic();
	scene_display_detach();
	scene_display_none();
	scene_display_service_nowait();
	scene_display_gate_keeps();
	scene_display_stop_records();
	scene_display_host_unsure();
	scene_display_detach_after_end();
	scene_display_light_cut();
	scene_display_frame_bytes();
	scene_display_release_wait();
	model_fini();

	/* Reports the counts. */
	printf("ghci-driver-host-test: %u passed, %u failed\n", checks_passed, checks_failed);
	if (checks_failed != 0U) {
		printf("FAIL ghci-driver-host-test\n");
		return 1;
	}

	/* Succeeded: every check held. */
	printf("PASS ghci-driver-host-test\n");
	return 0;
}

/* The service thread is not started on the host: the scenes run its steps. */
int
ghci_service_start(
	struct ghci_device *target)
{
	(void)target;
	return 0;
}

/* The poll thread is not started on the host: the sleep hook does its work. */
int
ghci_poll_start(
	struct ghci_device *target)
{
	(void)target;
	return 0;
}

/* Counts a check, and reports one that failed by name. */
static void
expect(
	int held,
	const char *name)
{
	/* A check that held is only counted. */
	if (held) {
		checks_passed++;
		return;
	}

	/* A failed check is reported by name. */
	checks_failed++;
	printf("FAIL %s\n", name);
}

/*
 * Starts a scene: a model of a profile, the stubs with the driver's lock
 * order and forbidden waits, and the device started.  Returns what the
 * start returned.
 */
static int
start_scene(
	enum model_profile profile,
	const struct model_switches *switches)
{
	int error;

	/* The model and the stubs, then the device. */
	prepare_scene(profile, switches);
	error = start_device();
	return error;
}

/*
 * Prepares a scene without starting the device: the model of a profile
 * and the stubs with the driver's lock order and forbidden waits.  A
 * scene may change the model before start_device().
 */
static void
prepare_scene(
	enum model_profile profile,
	const struct model_switches *switches)
{
	static const char *const order[] = {
		"ghci device", "ghci display", "ghci ring", "ghci interrupt"
	};

	/* The model and the stubs, with the rules the driver must keep. */
	model_init(profile, switches);
	stub_reset();
	stub_lock_order(order, 4U);
	stub_forbid_wait("ghci fence", "ghci device");
	stub_forbid_wait("ghci fence", "ghci display");
	stub_forbid_wait("ghci fence", "ghci ring");
	stub_forbid_wait("ghci gate", "ghci device");
	stub_forbid_wait("ghci gate", "ghci display");
	stub_forbid_wait("ghci gate", "ghci ring");
	stub_set_sleep_hook(sleep_hook, NULL);

	/* The fake bus. */
	interrupt_takes = 0U;
	interrupt_releases = 0U;
	master_on_calls = 0U;
	master_off_calls = 0U;
	irq_after_reset = 0;
	master_before_run = 0;
	ring_before_master = 0;
	release_after_quiet = 1;
	master_off_after_release = 1;
	scene_action = NULL;
	scene_action_after = 0U;
	scene_action_running = 0;
	scene_action_runs = 0U;
	model_set_bus_master(test_master_found);
}

/* Starts the device of the prepared scene as the bus glue leaves it.  Returns what the start returned. */
static int
start_device(void)
{
	int error;

	/* The device as the bus glue leaves it. */
	memset(&device, 0, sizeof(device));
	device.registers = stub_bar0();
	device.register_bytes = 4096U;
	device.bus_ops = &fake_bus_ops;
	device.pci_vendor = TEST_PCI_VENDOR;
	device.pci_device = TEST_PCI_DEVICE;
	device.bus_master_found = test_master_found;
	device.ring_bytes = test_ring_bytes;

	/* Starts it. */
	error = ghci_device_start(&device);
	return error;
}

/* Ends a scene: stops the device and checks how it waited and locked. */
static void
end_scene(
	const char *name)
{
	int error;

	/* A device that started stops. */
	scene_action = NULL;
	if (device.state != GHCI_STATE_DETACHED && device.fence_page != NULL) {
		error = ghci_device_stop(&device);
		expect(error == 0, name);
	}

	/* The locks and the waits. */
	check_locks(name);

	/* The flags of the next scene start again. */
	test_polled = 0;
	test_master_found = 0;
	test_master_broken = 0;
	test_release_busy = 0U;
	test_ring_bytes = 0U;
}

/* Checks that the driver kept its lock order and its rules on waits, and holds nothing. */
static void
check_locks(
	const char *name)
{
	char label[128];

	/* Each count by name. */
	snprintf(label, sizeof(label), "%s: lock order", name);
	expect(stub.order_violations == 0U, label);
	snprintf(label, sizeof(label), "%s: waits", name);
	expect(stub.wait_violations == 0U, label);
	snprintf(label, sizeof(label), "%s: double locks", name);
	expect(stub.double_locks == 0U && stub.unlock_errors == 0U, label);
	snprintf(label, sizeof(label), "%s: nothing held", name);
	expect(stub.held_count == 0U, label);
	snprintf(label, sizeof(label), "%s: MMIO", name);
	expect(stub.mmio_errors == 0U, label);
}

/*
 * The model's interrupt line, wired to the driver's handler.  While the
 * driver holds its interrupt lock (it writes IRQ_ENABLE inside it) the
 * handler would spin on another processor; here the line stays high and
 * the next tick of the model calls the handler.
 */
static void
model_irq(
	void *argument)
{
	if (stub_holds("ghci interrupt"))
		return;
	(void)ghci_cp_interrupt(argument);
}

/* Each sleep of the driver: the poll of a polled device, then the scene's action. */
static void
sleep_hook(
	unsigned sleep,
	void *argument)
{
	(void)argument;

	/* The poll thread's work. */
	if (device.polled && device.fence_page != NULL && device.state != GHCI_STATE_DETACHED)
		(void)ghci_cp_interrupt(&device);

	/* The scene's action, never inside itself. */
	if (scene_action == NULL || scene_action_running || sleep < scene_action_after)
		return;
	scene_action_running = 1;
	scene_action_runs++;
	scene_action();
	scene_action_running = 0;
}

/* Takes the interrupt: the model's line, unless the scene polls; records the order. */
static int
fake_interrupt_take(
	struct ghci_device *target)
{
	/* What the model was when the interrupt came: reset, not running, nothing pending. */
	interrupt_takes++;
	irq_after_reset = model.resets != 0U && (model.ctrl & GHCI_CTRL_RUN) == 0U;

	/* A polled scene takes none. */
	target->polled = test_polled;
	if (!test_polled)
		model_set_irq_handler(model_irq, target);
	return 0;
}

/* Lets the interrupt go, or answers EBUSY as many times as the scene says. */
static int
fake_interrupt_release(
	struct ghci_device *target)
{
	(void)target;

	/* A handler still running elsewhere. */
	if (test_release_busy != 0U) {
		test_release_busy--;
		return EBUSY;
	}

	/* The device is reset and quiet when its interrupt goes. */
	interrupt_releases++;
	if (model.ctrl != 0U || model_read(GHCI_REG_IRQ_ENABLE) != 0U)
		release_after_quiet = 0;
	model_set_irq_handler(NULL, NULL);
	return 0;
}

/* Turns bus mastering on or off in the model; records the order. */
static int
fake_bus_master(
	struct ghci_device *target,
	int on)
{
	(void)target;

	/* Off, after the interrupt went. */
	if (!on) {
		master_off_calls++;
		if (model.irq_handler != NULL)
			master_off_after_release = 0;
		model_set_bus_master(0);
		return 0;
	}

	/* On: the ring is set and the processor does not run yet. */
	master_on_calls++;
	master_before_run = (model.ctrl & GHCI_CTRL_RUN) == 0U;
	ring_before_master = model_read(GHCI_REG_RING_SIZE) != 0U && model_read(GHCI_REG_CP_HEAP_BASE) != 0U;
	if (!test_master_broken)
		model_set_bus_master(1);
	return 0;
}

/* Submits dwords of NOPs as the driver's own work. */
static int
submit_nops(
	uint32_t dwords,
	struct ghci_ticket *ticket)
{
	static uint32_t packets[TEST_LARGE_DWORDS];
	uint32_t done;
	uint32_t length;
	int error;

	/* NOPs of up to 255 dwords. */
	done = 0U;
	while (done < dwords) {
		length = dwords - done;
		if (length > GHCI_PACKET_DWORDS_MAX)
			length = GHCI_PACKET_DWORDS_MAX;
		(void)ghci_cmd_nop(&packets[done], length);
		done += length;
	}

	/* The submission. */
	error = ghci_cp_submit(&device, NULL, packets, dwords, ticket);
	return error;
}

/* The scene's action: the service thread's recovery. */
static void
action_recover(void)
{
	ghci_service_step(&device, GHCI_WORK_RECOVER);
}

/*
 * The scene's action for the wait for room: the clock jumps far past the
 * hang time (a wait for room has no timeout of its own), then a fault.
 */
static void
action_stall_then_fault(void)
{
	if (scene_action_runs < 100U)
		stub.ticks += kern_ms_to_ticks(GHCI_HANG_MS);
	if (scene_action_runs == 100U)
		model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x2000U, 0);
}

/* The scene's action: a session's fault under PROT (the processor goes on), each time. */
static void
action_session_fault(void)
{
	model_inject(GHCI_FAULT_KIND_PAGE_FAULT, GHCI_UNIT_SU, 1U, 0x4000U, 0);
}

/* The scene's action: a session's fault under PROT, the first time only. */
static void
action_session_fault_once(void)
{
	if (scene_action_runs == 1U)
		action_session_fault();
}

/* Reports the bytes of CP_HEAP the driver gave the model. */
static uint32_t
heap_bytes(void)
{
	return model_read(GHCI_REG_CP_HEAP_LIMIT) - model_read(GHCI_REG_CP_HEAP_BASE);
}

/* The versions and the magic the attach takes and refuses. */
static void
scene_versions(void)
{
	static const uint32_t versions[] = { 0x00010000U, 0x00010003U, 0x00020000U, 0U };
	static const int taken[] = { 1, 1, 0, 0 };
	char label[96];
	unsigned index;
	int error;

	/* Each version: 1.x is taken, anything else is not GHCI 1.0. */
	for (index = 0U; index < 4U; index++) {
		prepare_scene(MODEL_KC705_UMA, NULL);
		model.window[GHCI_REG_VERSION / 4U] = versions[index];
		error = start_device();
		snprintf(label, sizeof(label), "versions: %08x", (unsigned)versions[index]);
		if (taken[index])
			expect(error == 0, label);
		else
			expect(error == ENODEV && stub.registrations == 0U && model.resets == 0U, label);
		end_scene(label);
	}

	/* A wrong magic is not GHCI. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	model.window[GHCI_REG_MAGIC / 4U] = 0x12345678U;
	error = start_device();
	expect(error == ENODEV && model.resets == 0U, "versions: magic");
	end_scene("versions: magic");
}

/*
 * The checks of the attach: SCRATCH, MEM_READY, the reset and its wait,
 * the first fence, a device a kernel before left running or faulted, and
 * the order of the registers.
 */
static void
scene_attach_checks(void)
{
	struct model_switches switches;
	int error;

	/* A window whose SCRATCH does not answer is refused before anything is set. */
	memset(&switches, 0, sizeof(switches));
	switches.scratch_stuck = 1;
	error = start_scene(MODEL_KC705_UMA, &switches);
	expect(error == EIO && stub.registrations == 0U && interrupt_takes == 0U, "attach: SCRATCH");
	end_scene("attach: SCRATCH");

	/* MEM_READY a little late is waited for; one that never comes is refused. */
	memset(&switches, 0, sizeof(switches));
	switches.mem_ready_reads = 20U;
	error = start_scene(MODEL_KC705_UMA, &switches);
	expect(error == 0, "attach: MEM_READY late");
	end_scene("attach: MEM_READY late");
	switches.mem_ready_reads = 1000000U;
	error = start_scene(MODEL_KC705_UMA, &switches);
	expect(error == EIO && interrupt_takes == 0U, "attach: MEM_READY never");
	end_scene("attach: MEM_READY never");

	/* A reset that ends late is waited for; one that never ends is refused. */
	memset(&switches, 0, sizeof(switches));
	switches.reset_reads = 50U;
	error = start_scene(MODEL_KC705_UMA, &switches);
	expect(error == 0, "attach: reset late");
	end_scene("attach: reset late");
	memset(&switches, 0, sizeof(switches));
	switches.reset_stuck = 1;
	error = start_scene(MODEL_KC705_UMA, &switches);
	expect(error == EIO && interrupt_takes == 0U, "attach: reset stuck");
	end_scene("attach: reset stuck");

	/*
	 * The first reset waits for the quiesce of the watchdog a kernel before
	 * left (2^28 cycles at 100 MHz: 4 x 2684 ms + 500 ms = 11237 ms), so a
	 * reset that takes 10.5 s is not refused.
	 */
	memset(&switches, 0, sizeof(switches));
	switches.reset_reads = 10500U;
	error = start_scene(MODEL_KC705_UMA, &switches);
	expect(error == 0, "attach: reset wait from the old WDT_CYCLES");
	end_scene("attach: reset wait from the old WDT_CYCLES");

	/* An unknown clock leaves WDT_CYCLES at its default and waits 10 s for a reset. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	model.window[GHCI_REG_GCLK_HZ / 4U] = 0U;
	error = start_device();
	expect(error == 0 && device.reset_wait_ms == 10000U && model_read(GHCI_REG_WDT_CYCLES) == 0x10000000U, "attach: unknown clock");
	end_scene("attach: unknown clock");

	/*
	 * The watchdog and the reset's wait: 2000 ms at 100 MHz, and four
	 * watchdog periods and 500 ms for the quiesce (8500 ms).
	 */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "attach: started");
	expect(model_read(GHCI_REG_WDT_CYCLES) == TEST_WDT_CYCLES, "attach: WDT_CYCLES");
	expect(device.reset_wait_ms == 8500U, "attach: reset wait from WDT_CYCLES and GCLK_HZ");

	/* The order: the interrupt after the reset, bus mastering after the ring and before RUN. */
	expect(interrupt_takes == 1U && irq_after_reset, "attach: interrupt after the reset");
	expect(master_on_calls == 1U && master_before_run && ring_before_master, "attach: bus master after the ring, before RUN");
	expect(model.dma_without_master == 0U, "attach: no fetch without bus master");
	expect((model.ctrl & (GHCI_CTRL_RUN | GHCI_CTRL_WDT_EN | GHCI_CTRL_IRQ_GLOBAL)) ==
	    (GHCI_CTRL_RUN | GHCI_CTRL_WDT_EN | GHCI_CTRL_IRQ_GLOBAL), "attach: CTRL");
	expect(model_read(GHCI_REG_IRQ_ENABLE) == (GHCI_IRQ_FENCE | GHCI_IRQ_FAULT | GHCI_IRQ_WATCHDOG | GHCI_IRQ_DISPLAY), "attach: IRQ_ENABLE");
	expect(model_count_commands(GHCI_OP_FENCE) == 1U && device.fence_interrupts == 1U, "attach: first fence and its interrupt");
	expect(stub.registrations == 1U && stub.register_errors == 0U && device.ops.capabilities == (GPU_CAP_RESOURCE | GPU_CAP_BLOB | GPU_CAP_TRANSFER | GPU_CAP_MAPPING | GPU_CAP_SHARE | GPU_CAP_ALLOCATION_SHARE | GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS), "attach: registered with the resource and display capabilities");
	expect(strcmp(device.name, "ghci:10ee:7024") == 0, "attach: name");
	end_scene("attach: started");

	/* A GPU that never fetches the ring: the first fence does not come, and the attach undoes itself. */
	test_master_broken = 1;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == EIO && stub.registrations == 0U, "attach: no first fence");
	expect(interrupt_releases == 1U && master_off_calls == 1U && (model.ctrl & GHCI_CTRL_RUN) == 0U, "attach: undone");
	expect(device.ring.memory == NULL && device.fence_page == NULL && device.heap == NULL, "attach: memory given back");
	end_scene("attach: no first fence");

	/* A device a kernel before this one left faulted, or running its ring, is reset and taken. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	model.state = MODEL_FAULTED;
	model.ctrl = GHCI_CTRL_RUN;
	error = start_device();
	expect(error == 0 && model.state == MODEL_RUNNING, "attach: left faulted");
	end_scene("attach: left faulted");
	prepare_scene(MODEL_KC705_UMA, NULL);
	model.state = MODEL_RUNNING;
	model.ctrl = GHCI_CTRL_RUN;
	model.ring_wptr = 17U;
	error = start_device();
	expect(error == 0 && model.ring_rptr == model.ring_wptr, "attach: left running");
	end_scene("attach: left running");
}

/* The executor and CP_HEAP by profile. */
static void
scene_attach_policy(void)
{
	int error;

	/* The KC705: the executor, and CP_HEAP for every session and chunk. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0 && device.executor == 1, "policy: kc705 executor");
	expect(heap_bytes() == ghci_heap_device_bytes(device.caps.va_bits), "policy: kc705 CP_HEAP");
	end_scene("policy: kc705");

	/* Without shader units: the display only. */
	error = start_scene(MODEL_NO_SU, NULL);
	expect(error == 0 && device.executor == 0, "policy: no-su display only");
	end_scene("policy: no-su");

	/* Flat: the executor all the same, and CP_HEAP of session 0's root only. */
	error = start_scene(MODEL_FLAT, NULL);
	expect(error == 0 && device.executor == 1, "policy: flat executor");
	expect(heap_bytes() == 16384U, "policy: flat CP_HEAP 16 KiB");
	expect(model_count_commands(GHCI_OP_BIND_SESSION) == 0U, "policy: flat binds no session");
	end_scene("policy: flat");

	/* PROT: the executor, as without PROT. */
	error = start_scene(MODEL_PROT, NULL);
	expect(error == 0 && device.executor == 1, "policy: prot executor");
	end_scene("policy: prot");

	/* A 30-bit VA: CP_HEAP for the 512 chunks of 1 GiB; the mappings stay below 1 GiB. */
	error = start_scene(MODEL_VA30, NULL);
	expect(error == 0 && device.caps.va_bits == 30U, "policy: va30 started");
	expect(heap_bytes() == 16U * 16384U + 512U * 4096U + 65536U, "policy: va30 CP_HEAP");
	end_scene("policy: va30");
	scene_va30_chunks();

	/* The KC705's minimal build: flat and discrete, its local memory not used, the executor all the same. */
	error = start_scene(MODEL_KC705_MIN, NULL);
	expect(error == 0 && device.executor == 1 && device.local_ready == 0, "policy: kc705-min executor, host memory");
	expect(heap_bytes() == 16384U, "policy: kc705-min CP_HEAP 16 KiB");
	end_scene("policy: kc705-min");
}

/*
 * The chunks of a 30-bit VA: with chunks 1 to 509 taken, a mapping of
 * two chunks finds no room, and one of 64 KiB gets chunk 510, the last
 * below 1 GiB but one.
 */
static void
scene_va30_chunks(void)
{
	struct ghci_allocation *memory;
	struct ghci_allocation *wide;
	struct ghci_mapping *mapping;
	struct ghci_vm *vm;
	struct ghci_vm *taken;
	uint32_t chunk;
	int error;

	/* A session, and chunks 1 to 509 marked as another's. */
	error = start_scene(MODEL_VA30, NULL);
	memory = host_memory(64U * 1024U);
	wide = host_memory(4U * 1024U * 1024U);
	if (error == 0 && memory != NULL && wide != NULL)
		error = ghci_vm_open(&device, &owner_token, &vm);
	expect(error == 0 && memory != NULL && wide != NULL, "va30: session");
	if (error != 0 || memory == NULL || wide == NULL) {
		end_scene("va30");
		return;
	}
	taken = (struct ghci_vm *)(void *)&owner_token;
	for (chunk = 1U; chunk < 510U; chunk++)
		device.chunk_owner[chunk] = taken;

	/* Two chunks do not fit in chunk 510 alone. */
	error = ghci_vm_map(&device, vm, wide, 0U, &mapping);
	expect(error == ENOMEM, "va30: no two chunks past 509");

	/* 64 KiB takes chunk 510. */
	error = ghci_vm_map(&device, vm, memory, 0U, &mapping);
	expect(error == 0 && (mapping->va >> 21) == 510U && mapping->va + mapping->bytes <= (1ULL << 30) && model.faults == 0U,
	       "va30: the mapping below 1 GiB");
	if (error == 0) {
		ghci_vm_unmap(&device, mapping);
		reclaim();
	}
	for (chunk = 1U; chunk < 510U; chunk++)
		device.chunk_owner[chunk] = NULL;

	/* The close and the end. */
	ghci_vm_close(&device, vm);
	reclaim();
	memory_done(memory);
	memory_done(wide);
	end_scene("va30");
}

/* Submissions, fences and the interrupt; the ring wraps, and a submission larger than the ring. */
static void
scene_submit(void)
{
	struct ghci_ticket ticket;
	unsigned index;
	unsigned wraps;
	uint32_t last;
	int failures;
	int error;

	/* A thousand submissions, each waited for, the ring wrapping several times. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "submit: started");
	failures = 0;
	wraps = 0U;
	last = device.ring.wptr;
	for (index = 0U; index < TEST_SUBMITS; index++) {
		error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
		if (error == 0)
			error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
		if (error != 0)
			failures++;
		if (device.ring.wptr < last)
			wraps++;
		last = device.ring.wptr;
	}
	expect(failures == 0, "submit: every ticket passed");
	expect(wraps >= 3U, "submit: the ring wrapped");
	expect(model.faults == 0U, "submit: no framing fault");
	expect(model_count_commands(GHCI_OP_BIND_SESSION) == 2U * TEST_SUBMITS, "submit: session bound around each");
	expect(model_count_commands(GHCI_OP_FENCE) == TEST_SUBMITS + 1U, "submit: one fence each");
	expect(device.completed == device.fence_placed, "submit: completed");

	/* A submission larger than the ring goes in pieces. */
	error = submit_nops(TEST_LARGE_DWORDS, &ticket);
	expect(error == 0, "submit: larger than the ring placed");
	error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == 0 && model.faults == 0U, "submit: larger than the ring passed");
	end_scene("submit");

	/* A 4 KiB ring: a thousand submissions meet its end, its full state and its one free slot. */
	test_ring_bytes = 4096U;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0 && model_read(GHCI_REG_RING_SIZE) == 4096U, "small ring: started");
	failures = 0;
	wraps = 0U;
	last = device.ring.wptr;
	for (index = 0U; index < TEST_SUBMITS; index++) {
		error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
		if (error == 0)
			error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
		if (error != 0)
			failures++;
		if (device.ring.wptr < last)
			wraps++;
		last = device.ring.wptr;
	}
	expect(failures == 0 && wraps >= 100U && model.faults == 0U, "small ring: every ticket passed");
	model_hold(1);
	error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	scene_action = NULL;
	expect(error == 0, "small ring: placed while held");
	model_hold(0);
	error = submit_nops(TEST_LARGE_DWORDS, &ticket);
	if (error == 0)
		error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == 0 && model.faults == 0U, "small ring: larger than the ring passed");
	end_scene("small ring");
}

/* A polled device: no interrupt is enabled, the poll moves the fences. */
static void
scene_polled(void)
{
	struct ghci_ticket ticket;
	int error;

	/* Started polled. */
	test_polled = 1;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0 && device.polled, "polled: started");
	expect(model_read(GHCI_REG_IRQ_ENABLE) == 0U && (model.ctrl & GHCI_CTRL_IRQ_GLOBAL) == 0U, "polled: no interrupt enabled");

	/* A submission passes on the poll alone. */
	error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	if (error == 0)
		error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == 0 && model.irq_calls == 0U, "polled: ticket passed");

	/* A fault is found by the poll too. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_sleep_ms(1U);
	expect(device.recovering == 1, "polled: fault found");
	ghci_service_step(&device, 0U);
	expect(device.recovering == 0 && device.state == GHCI_STATE_RUNNING && device.resets == 1U, "polled: recovered");
	end_scene("polled");
}

/*
 * A device fault: the gate closes, the processor is reset, the tickets
 * of the work in flight fail, and the next work runs.  A fault that
 * leaves the processor running (a session's fault under PROT) is halted
 * first.
 */
static void
scene_device_fault(void)
{
	struct ghci_ticket first;
	struct ghci_ticket second;
	struct ghci_ticket third;
	enum ghci_gate gate;
	int error;

	/* Two submissions the processor holds. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "fault: started");
	model_hold(1);
	error = submit_nops(TEST_SUBMIT_DWORDS, &first);
	expect(error == 0, "fault: first placed");
	error = submit_nops(TEST_SUBMIT_DWORDS, &second);
	expect(error == 0, "fault: second placed");

	/* The fault closes the gate. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	expect(device.recovering == 1 && device.fault_records == 1U, "fault: recorded");
	gate = ghci_cp_enter(&device, 0U, 0);
	expect(gate == GHCI_GATE_RETRY, "fault: gate closed");

	/* The recovery resets; the work in flight fails, and the device runs again. */
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.recovering == 0 && device.state == GHCI_STATE_RUNNING && device.resets == 1U, "fault: recovered");
	error = ghci_cp_wait(&device, &first, TEST_WAIT_MS);
	expect(error == EIO, "fault: first failed");
	error = ghci_cp_wait(&device, &second, TEST_WAIT_MS);
	expect(error == EIO, "fault: second failed");
	error = submit_nops(TEST_SUBMIT_DWORDS, &third);
	if (error == 0)
		error = ghci_cp_wait(&device, &third, TEST_WAIT_MS);
	expect(error == 0, "fault: next work runs");
	expect(device.storm_count == 1U && device.oom_count == 0U, "fault: counted once");

	/* The watchdog of session 0's packet is a device fault too. */
	model_inject_watchdog(0U);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 2U && device.storm_count == 2U, "fault: session 0 watchdog");
	end_scene("fault");

	/* Under PROT a session's fault leaves the processor running: it is halted before the reset. */
	error = start_scene(MODEL_PROT, NULL);
	expect(error == 0, "fault: prot started");
	model_inject(GHCI_FAULT_KIND_PAGE_FAULT, GHCI_UNIT_SU, 1U, 0x3000U, 0);
	expect(device.recovering == 1 && model.state == MODEL_RUNNING, "fault: prot session fault, processor running");
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(model.halts == 1U && device.resets == 1U && device.state == GHCI_STATE_RUNNING, "fault: prot halted and reset");
	end_scene("fault: prot");
}

/*
 * A fence the device wrote before a fault whose interrupt was not yet
 * taken: its ticket passes, the later one fails.
 */
static void
scene_fence_before_fault(void)
{
	struct ghci_ticket written;
	struct ghci_ticket lost;
	int error;

	/* The first submission runs with the interrupt unheard; the second is held. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "fence before fault: started");
	model_set_irq_handler(NULL, NULL);
	error = submit_nops(TEST_SUBMIT_DWORDS, &written);
	expect(error == 0, "fence before fault: first placed");
	model_hold(1);
	error = submit_nops(TEST_SUBMIT_DWORDS, &lost);
	expect(error == 0, "fence before fault: second placed");

	/* The fault, then the interrupt with both events. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	model_set_irq_handler(model_irq, &device);
	model_tick();
	expect(device.recovering == 1, "fence before fault: fault taken");
	expect(device.completed != written.seq, "fence before fault: no completion while recovering");

	/* The recovery: the written fence passed, the other failed. */
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	error = ghci_cp_wait(&device, &written, TEST_WAIT_MS);
	expect(error == 0, "fence before fault: written ticket passed");
	error = ghci_cp_wait(&device, &lost, TEST_WAIT_MS);
	expect(error == EIO, "fence before fault: held ticket failed");
	end_scene("fence before fault");
}

/*
 * A fault record of an older recovery generation is dropped: a stale
 * exhaustion of CP_HEAP does not make a fault of the processor an
 * exhaustion.
 */
static void
scene_stale_record(void)
{
	struct ghci_fault_record *record;
	int error;

	/* One recovery moves the generation on. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "stale: started");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.recovery_generation == 1U && device.storm_count == 1U, "stale: first recovery");

	/* A record of the older generation, as a late handler would have left it. */
	record = &device.faults[0];
	memset(record, 0, sizeof(*record));
	record->generation = 0U;
	record->fault_info = GHCI_FAULT_KIND_MMU_OOM | (GHCI_UNIT_CP << GHCI_FAULT_UNIT_SHIFT);
	device.fault_records = 1U;

	/* A new fault of the processor: counted as a fault, not as an exhaustion. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.oom_count == 0U && device.storm_count == 2U && device.state == GHCI_STATE_RUNNING, "stale: dropped");
	end_scene("stale");
}

/* A hang: work that does not move for the hang time is reset; a fence alone that does not come too. */
static void
scene_hang(void)
{
	struct ghci_ticket ticket;
	int error;

	/* The processor holds a submission. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "hang: started");
	model_hold(1);
	error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	expect(error == 0, "hang: placed");

	/* Before the hang time nothing happens; after it the recovery resets. */
	ghci_service_step(&device, 0U);
	stub.ticks += kern_ms_to_ticks(GHCI_HANG_MS) / 2U;
	ghci_service_step(&device, 0U);
	expect(device.recovering == 0 && device.resets == 0U, "hang: not before the hang time");
	stub.ticks += kern_ms_to_ticks(GHCI_HANG_MS);
	ghci_service_step(&device, 0U);
	expect(device.resets == 1U && device.state == GHCI_STATE_RUNNING, "hang: reset");
	error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == EIO, "hang: ticket failed");

	/* A fence alone (as a VM operation's) that is not written is a hang too. */
	model_hold(1);
	error = ghci_cp_submit(&device, NULL, NULL, 0U, &ticket);
	expect(error == 0, "hang: fence placed");
	ghci_service_step(&device, 0U);
	stub.ticks += kern_ms_to_ticks(GHCI_HANG_MS) + 1U;
	ghci_service_step(&device, 0U);
	expect(device.resets == 2U, "hang: fence alone reset");
	end_scene("hang");
}

/* A long submission whose processor keeps moving RING_RPTR is not a hang. */
static void
scene_long_submit(void)
{
	struct ghci_ticket ticket;
	uint32_t packets[5U * GHCI_DW_WAIT_FENCE];
	uint64_t word_bus;
	uint8_t *word;
	uint64_t value;
	unsigned index;
	int error;

	/* A word in host memory the submission waits on, five times. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "long: started");
	error = model_host_alloc(8U, 8U, 0xffffffffU, &word_bus);
	expect(error == 0, "long: word");
	word = model_dma(word_bus, 8U);
	for (index = 0U; index < 5U; index++)
		(void)ghci_cmd_wait_fence(&packets[index * GHCI_DW_WAIT_FENCE], word_bus, index + 1U);
	error = ghci_cp_submit(&device, NULL, packets, 5U * GHCI_DW_WAIT_FENCE, &ticket);
	expect(error == 0, "long: placed");

	/* Each half hang time the word moves on, and the processor with it. */
	for (index = 0U; index < 5U; index++) {
		ghci_service_step(&device, 0U);
		stub.ticks += kern_ms_to_ticks(GHCI_HANG_MS) * 3U / 4U;
		ghci_service_step(&device, 0U);
		value = index + 1U;
		memcpy(word, &value, 8U);
		model_tick();
	}
	error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == 0 && device.resets == 0U, "long: not a hang");
	end_scene("long");
}

/*
 * A wait for the ring's room ends only when RING_RPTR moves or a
 * recovery begins: the clock running far past the hang time does not end
 * it, a fault does, and the submission placed in part fails.
 */
static void
scene_room_wait(void)
{
	struct ghci_ticket ticket;
	unsigned sleeps;
	int error;

	/* The processor holds the ring; a submission larger than the ring waits for room. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "room: started");
	model_hold(1);
	scene_action = action_stall_then_fault;
	sleeps = stub.sleeps;
	error = submit_nops(TEST_LARGE_DWORDS, &ticket);
	scene_action = NULL;
	expect(error == EIO, "room: placed in part, failed");
	expect(scene_action_runs >= 100U && stub.sleeps - sleeps >= 100U, "room: no timeout of its own");
	expect(device.recovering == 1, "room: recovery pending");

	/* The recovery, then the ring takes work again. */
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	if (error == 0)
		error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == 0 && device.state == GHCI_STATE_RUNNING, "room: runs again");
	end_scene("room");
}

/*
 * The gate: a caller holding the device's and the display's locks lets
 * both go while a recovery runs, and starts again; nothing waits at the
 * gate holding a lock.
 */
static void
scene_gate(void)
{
	enum ghci_gate gate;
	int error;

	/* A fault the service has not taken yet. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "gate: started");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);

	/* The caller waits at the gate; the recovery runs meanwhile and takes every lock. */
	scene_action = action_recover;
	mutex_lock(&device.lock);
	mutex_lock(&device.display_lock);
	gate = ghci_cp_enter(&device, GHCI_HOLD_DEVICE | GHCI_HOLD_DISPLAY, 1);
	scene_action = NULL;
	expect(gate == GHCI_GATE_RETRY, "gate: retry after the recovery");
	expect(stub.held_count == 0U && stub.double_locks == 0U, "gate: every lock let go");
	expect(device.recovering == 0 && device.resets == 1U, "gate: recovery ran while waiting");

	/* The gate is open again. */
	gate = ghci_cp_enter(&device, 0U, 1);
	expect(gate == GHCI_GATE_OPEN && stub_holds("ghci ring"), "gate: open");
	ghci_cp_leave(&device);
	end_scene("gate");
}

/*
 * The loss of a device: checks that fail three times in a row; the gate
 * then answers LOST to whoever waits, submissions and opens fail, and the
 * core hears of it.
 */
static void
scene_lost(void)
{
	struct ghci_ticket ticket;
	enum ghci_gate gate;
	void *session;
	int error;

	/* A fault, and a GPU that fetches nothing after it: every check fails. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "lost: started");
	model_hold(1);
	error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	expect(error == 0, "lost: placed");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	model_set_bus_master(0);

	/* A caller waits at the gate while the recovery fails three times. */
	scene_action = action_recover;
	gate = ghci_cp_enter(&device, 0U, 1);
	scene_action = NULL;
	expect(gate == GHCI_GATE_LOST, "lost: the gate answers LOST");
	expect(device.state == GHCI_STATE_LOST && device.recovery_failures == GHCI_RECOVERY_TRIES, "lost: three checks failed");
	expect(device.resets == GHCI_RECOVERY_TRIES && stub.errors_reported == 1U, "lost: reported");

	/* Everything after fails. */
	error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == EIO, "lost: pending ticket failed");
	error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	expect(error == EIO, "lost: submission refused");
	error = device.ops.open(&device, &session);
	expect(error == EIO, "lost: open refused");

	/* The detach stops a lost device and gives its memory back once bus mastering is off. */
	error = ghci_device_stop(&device);
	expect(error == 0 && device.state == GHCI_STATE_DETACHED && device.ring.memory == NULL, "lost: detached");
	end_scene("lost");

	/* A reset that never ends loses the device at once. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "lost: started again");
	model.switches.reset_stuck = 1;
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_LOST && device.resets == 0U, "lost: reset stuck");
	model.switches.reset_stuck = 0;
	model.reset_hung = 0;
	end_scene("lost: reset stuck");
}

/*
 * Faults that come too often: three faults no session caused within a
 * minute, or two exhaustions of CP_HEAP, lose the device; faults spread
 * wider do not.
 */
static void
scene_storm(void)
{
	unsigned index;
	int error;

	/* Three device faults within a minute. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "storm: started");
	for (index = 0U; index < 3U; index++) {
		model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
		ghci_service_step(&device, GHCI_WORK_RECOVER);
	}
	expect(device.state == GHCI_STATE_LOST && device.resets == 2U, "storm: lost at the third");
	end_scene("storm");

	/* Two exhaustions of CP_HEAP within a minute. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "storm: oom started");
	for (index = 0U; index < 2U; index++) {
		model_inject(GHCI_FAULT_KIND_MMU_OOM, GHCI_UNIT_CP, 0U, 0U, 0);
		ghci_service_step(&device, GHCI_WORK_RECOVER);
	}
	expect(device.state == GHCI_STATE_LOST && device.resets == 1U && device.storm_count == 0U, "storm: lost at the second exhaustion");
	end_scene("storm: oom");

	/* Three device faults more than a minute apart. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "storm: spread started");
	for (index = 0U; index < 3U; index++) {
		model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
		ghci_service_step(&device, GHCI_WORK_RECOVER);
		stub.ticks += kern_ms_to_ticks(GHCI_FAULT_STORM_MS / 2U + 1U);
	}
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 3U, "storm: spread faults recovered");
	end_scene("storm: spread");
}

/*
 * The detach: refused while a session is open; it resets the GPU, lets
 * the interrupt go and turns bus mastering off; it keeps bus mastering
 * the firmware left on; and it waits for a recovery in progress.
 */
static void
scene_detach(void)
{
	void *session;
	int error;

	/* A session keeps the device. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "detach: started");
	error = device.ops.open(&device, &session);
	expect(error == 0, "detach: session opened");
	error = ghci_device_stop(&device);
	expect(error == EBUSY, "detach: refused with a session");
	device.ops.close(&device, session);

	/* The detach. */
	error = ghci_device_stop(&device);
	expect(error == 0 && device.state == GHCI_STATE_DETACHED, "detach: stopped");
	expect(model.ctrl == 0U && model_read(GHCI_REG_IRQ_ENABLE) == 0U, "detach: reset and quiet");
	expect(interrupt_releases == 1U && model.irq_handler == NULL, "detach: interrupt let go");
	expect(master_off_calls == 1U && model.bus_master == 0, "detach: bus master off");
	expect(release_after_quiet && master_off_after_release, "detach: reset, quiet, interrupt, then bus master");
	expect(device.ring.memory == NULL && device.fence_page == NULL && device.heap == NULL, "detach: memory given back");
	expect(stub.unregistrations == 1U, "detach: unregistered");
	end_scene("detach");

	/* An interrupt whose handler may still run keeps the device and its memory; a second try ends. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "detach: busy started");
	test_release_busy = 1U;
	error = ghci_device_stop(&device);
	expect(error == EBUSY && device.ring.memory != NULL && device.interrupt_taken, "detach: handler busy, kept");
	error = ghci_device_stop(&device);
	expect(error == 0 && device.ring.memory == NULL && interrupt_releases == 1U, "detach: second try");
	end_scene("detach: busy");

	/* Bus mastering the firmware left on stays on. */
	test_master_found = 1;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0 && master_on_calls == 0U, "detach: firmware's bus master kept at the start");
	error = ghci_device_stop(&device);
	expect(error == 0 && master_off_calls == 0U && model.bus_master == 1, "detach: firmware's bus master kept at the stop");
	end_scene("detach: firmware bus master");

	/* A recovery in progress ends before the detach drives the GPU. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "detach: recovery started");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	scene_action = action_recover;
	error = ghci_device_stop(&device);
	scene_action = NULL;
	expect(error == 0 && device.resets == 1U && device.state == GHCI_STATE_DETACHED, "detach: after the recovery");
	end_scene("detach: recovery");
}

/*
 * The last checks of a recovery: a session's fault that latches during
 * the reset leaves FAULT_INFO set after the fence comes back, so the
 * recovery goes round again; one at every pass loses the device.
 */
static void
scene_check_fails(void)
{
	struct model_switches switches;
	int error;

	/* A device fault, and a session's fault while the reset runs. */
	memset(&switches, 0, sizeof(switches));
	switches.reset_reads = 5U;
	error = start_scene(MODEL_PROT, &switches);
	expect(error == 0, "check: started");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	scene_action = action_session_fault_once;
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	scene_action = NULL;
	expect(device.resets == 2U && device.state == GHCI_STATE_RUNNING, "check: second pass recovered");
	expect(device.recovery_failures == 0U && device.recovering == 0, "check: failures cleared");
	end_scene("check");

	/* A session's fault at every pass: the device is lost. */
	error = start_scene(MODEL_PROT, &switches);
	expect(error == 0, "check: every pass started");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	scene_action = action_session_fault;
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	scene_action = NULL;
	expect(device.state == GHCI_STATE_LOST && stub.errors_reported == 1U, "check: lost");
	end_scene("check: every pass");
}

/* The scene's action: a permission fault of session 1's texture unit, the first time only. */
static void
action_replay_fault(void)
{
	if (scene_action_runs == 1U)
		model_inject(GHCI_FAULT_KIND_PERM_FAULT, GHCI_UNIT_TU, 1U, 0x8000U, 0);
}

/* Takes host memory as a resource would, holding it once. */
static struct ghci_allocation *
host_memory(
	uint64_t bytes)
{
	struct ghci_allocation *allocation;
	int error;

	/* The run, held by the test. */
	error = ghci_memory_alloc(&device, bytes, 0U, &allocation);
	if (error != 0)
		return NULL;
	allocation->holds = 1U;
	return allocation;
}

/* Lets the test's hold of host memory go. */
static void
memory_done(
	struct ghci_allocation *allocation)
{
	mutex_lock(&device.lock);
	ghci_memory_unhold(&device, allocation);
	mutex_unlock(&device.lock);
}

/* One round of the service thread: what the GPU is done with goes back. */
static void
reclaim(void)
{
	ghci_service_step(&device, GHCI_WORK_RECLAIM);
}

/* Submits a few NOPs as a session's work. */
static int
submit_vm(
	struct ghci_vm *vm,
	struct ghci_ticket *ticket)
{
	uint32_t packets[TEST_SUBMIT_DWORDS];

	(void)ghci_cmd_nop(packets, TEST_SUBMIT_DWORDS);
	return ghci_cp_submit(&device, vm, packets, TEST_SUBMIT_DWORDS, ticket);
}

/*
 * The session numbers: fifteen open and the sixteenth is refused, a
 * closing session keeps its number until its fence passes, and the
 * tables are counted and given back.
 */
static void
scene_sessions(void)
{
	struct ghci_vm *vms[GHCI_SESSIONS];
	struct ghci_vm *extra;
	uint64_t base;
	uint64_t root;
	unsigned opened;
	unsigned index;
	int error;

	/* Fifteen sessions open, each with its CMD_SESSION_OPEN. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "sessions: started");
	base = device.heap_used;
	root = ghci_heap_session_bytes(device.caps.va_bits);
	opened = 0U;
	for (index = 1U; index < GHCI_SESSIONS; index++) {
		error = ghci_vm_open(&device, &owner_token, &vms[index]);
		if (error == 0 && vms[index]->session == index)
			opened++;
	}
	expect(opened == 15U, "sessions: fifteen open");
	expect(model_count_commands(GHCI_OP_SESSION_OPEN) == 15U && model.session_active == 0xFFFFU, "sessions: open in the model");
	expect(device.heap_used == base + 15U * root, "sessions: tables counted");
	error = ghci_vm_open(&device, &owner_token, &extra);
	expect(error == EBUSY, "sessions: the sixteenth is refused");

	/* A closing session keeps its number until its fence passes. */
	model_hold(1);
	ghci_vm_close(&device, vms[3]);
	error = ghci_vm_open(&device, &owner_token, &extra);
	expect(error == EBUSY && device.vm[3] != NULL && vms[3]->state == GHCI_VM_CLOSING, "sessions: number kept while closing");
	model_hold(0);
	reclaim();
	expect(device.vm[3] == NULL && device.heap_used == base + 14U * root, "sessions: retired after the fence");
	error = ghci_vm_open(&device, &owner_token, &extra);
	expect(error == 0 && extra->slot == 3U, "sessions: number taken again once retired");
	vms[3] = extra;

	/* Every session closes and retires; CP_HEAP's count is session 0's again. */
	for (index = 1U; index < GHCI_SESSIONS; index++)
		ghci_vm_close(&device, vms[index]);
	reclaim();
	expect(device.heap_used == base && model.session_active == 1U, "sessions: all retired");
	for (index = 1U; index < GHCI_SESSIONS; index++) {
		if (device.vm[index] != NULL)
			break;
	}
	expect(index == GHCI_SESSIONS, "sessions: every number free");
	end_scene("sessions");
}

/*
 * The mappings: CMD_VM_MAP with the address, bus address and flags the
 * model records; two sessions' addresses never meet (without PROT and
 * with it); an unmap gives the memory back after its fence; with PROT the
 * level 0 tables go back at the close and not at the unmap.
 */
static void
scene_mappings(void)
{
	struct ghci_allocation *first_memory;
	struct ghci_allocation *second_memory;
	struct ghci_allocation *big_memory;
	struct ghci_mapping *first;
	struct ghci_mapping *second;
	struct ghci_mapping *big;
	struct ghci_vm *one;
	struct ghci_vm *two;
	const struct model_mapping *seen;
	enum model_profile profile;
	uint64_t big_va;
	uint64_t base;
	uint64_t root;
	unsigned round;
	int error;

	/* Without PROT (one shared table) and with it. */
	for (round = 0U; round < 2U; round++) {
		profile = MODEL_KC705_UMA;
		if (round == 1U)
			profile = MODEL_PROT;
		error = start_scene(profile, NULL);
		expect(error == 0, "mappings: started");
		base = device.heap_used;
		root = ghci_heap_session_bytes(device.caps.va_bits);

		/* Two sessions, each with a mapping, and a mapping of three chunks. */
		first_memory = host_memory(64U * 1024U);
		second_memory = host_memory(64U * 1024U);
		big_memory = host_memory(5U * 1024U * 1024U);
		error = ghci_vm_open(&device, &owner_token, &one);
		if (error == 0)
			error = ghci_vm_open(&device, &owner_token, &two);
		if (error == 0)
			error = ghci_vm_map(&device, one, first_memory, 0U, &first);
		if (error == 0)
			error = ghci_vm_map(&device, two, second_memory, GHCI_VM_FLAG_EXEC, &second);
		if (error == 0)
			error = ghci_vm_map(&device, one, big_memory, 0U, &big);
		expect(error == 0, "mappings: mapped");
		if (error != 0) {
			end_scene("mappings");
			continue;
		}

		/* The model's tables hold them as the driver meant. */
		seen = model_lookup(one->session, first->va);
		expect(seen != NULL && seen->phys == first_memory->bus && seen->bytes == first->bytes &&
		    seen->flags == (GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE | GHCI_VM_FLAG_COHERENT), "mappings: first in the model");
		seen = model_lookup(two->session, second->va);
		expect(seen != NULL && seen->phys == second_memory->bus &&
		    seen->flags == (GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE | GHCI_VM_FLAG_COHERENT | GHCI_VM_FLAG_EXEC), "mappings: second in the model");
		expect((first->va >> 21) != (second->va >> 21), "mappings: sessions on different chunks");
		expect(device.chunk_owner[first->va >> 21] == one && device.chunk_owner[second->va >> 21] == two, "mappings: chunk owners");
		expect((big->va >> 21) + 3U <= (first->va >> 21) || (first->va >> 21) + 1U <= (big->va >> 21), "mappings: big on chunks of its own");
		expect(device.heap_used == base + 2U * root + 5U * GHCI_TABLE_BYTES, "mappings: level 0 tables counted");
		expect(model.faults == 0U && first->va >= GHCI_L0_SPAN_BYTES && big->va + big->bytes <= 0xFFE00000ULL, "mappings: no fault, inside the range");

		/* The unmap gives the memory back after its fence; the tables stay counted. */
		big_va = big->va;
		ghci_vm_unmap(&device, big);
		expect(model_lookup(one->session, big_memory->bus) == NULL, "mappings: unmapped in the model");
		reclaim();
		expect(one->mappings == first && device.chunk_owner[big_va >> 21] == NULL, "mappings: unmap retired");
		expect(big_memory->holds == 1U, "mappings: the mapping let the memory go");
		expect(device.heap_used == base + 2U * root + 5U * GHCI_TABLE_BYTES, "mappings: unmap keeps the tables counted");

		/* The other session takes the chunks given back: with PROT its own level 0 tables are counted too. */
		error = ghci_vm_map(&device, two, big_memory, 0U, &big);
		expect(error == 0 && (big->va >> 21) == (big_va >> 21), "mappings: chunks taken by the other session");
		if (round == 1U)
			expect(device.heap_used == base + 2U * root + 8U * GHCI_TABLE_BYTES, "mappings: PROT counts each session's tables");
		else
			expect(device.heap_used == base + 2U * root + 5U * GHCI_TABLE_BYTES, "mappings: the shared tables count once");
		ghci_vm_unmap(&device, big);
		reclaim();

		/* One allocation in two sessions (an import): each mapping holds it. */
		error = ghci_vm_map(&device, two, first_memory, 0U, &big);
		expect(error == 0 && first_memory->holds == 3U && model_lookup(two->session, big->va) != NULL &&
		    model_lookup(two->session, big->va)->phys == first_memory->bus, "mappings: one allocation in two sessions");

		/* The closes give the tables back (with PROT); the shared level 0 tables stay until a reset. */
		ghci_vm_close(&device, one);
		ghci_vm_close(&device, two);
		reclaim();
		if (round == 1U)
			expect(device.heap_used == base, "mappings: PROT close returns every table");
		else
			expect(device.heap_used == base + 5U * GHCI_TABLE_BYTES, "mappings: shared level 0 tables stay");
		expect(first_memory->holds == 1U && second_memory->holds == 1U, "mappings: close let the memory go");
		memory_done(first_memory);
		memory_done(second_memory);
		memory_done(big_memory);
		end_scene("mappings");
	}
}

/*
 * A GPU without a walker: sessions and mappings place nothing, a mapping's
 * address is its bus address, and an unmap waits for the last fence
 * placed.
 */
static void
scene_flat(void)
{
	struct ghci_allocation *memory;
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	struct ghci_vm *vm;
	int error;

	/* A session and a mapping, with nothing placed. */
	error = start_scene(MODEL_FLAT, NULL);
	expect(error == 0, "flat: started");
	memory = host_memory(16U * 1024U);
	error = ghci_vm_open(&device, &owner_token, &vm);
	if (error == 0)
		error = ghci_vm_map(&device, vm, memory, 0U, &mapping);
	expect(error == 0 && vm->session == 0U && mapping->va == memory->bus, "flat: the address is the bus address");
	expect(model_count_commands(GHCI_OP_SESSION_OPEN) == 0U && model_count_commands(GHCI_OP_VM_MAP) == 0U && model.faults == 0U,
	    "flat: nothing placed");

	/* Work of the session, then the unmap waits for its fence. */
	model_hold(1);
	error = submit_vm(vm, &ticket);
	expect(error == 0 && model_count_commands(GHCI_OP_BIND_SESSION) == 0U, "flat: submitted without a bind");
	ghci_vm_unmap(&device, mapping);
	expect(mapping->state == GHCI_MAPPING_UNMAPPING && mapping->ticket.seq == ticket.seq, "flat: unmap waits for the last fence");
	reclaim();
	expect(vm->mappings == mapping, "flat: kept until the fence");
	model_hold(0);
	reclaim();
	expect(vm->mappings == NULL && memory->holds == 1U, "flat: retired after the fence");

	/* A reset replays nothing on a GPU without a walker. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && vm->state == GHCI_VM_OPEN && model_count_commands(GHCI_OP_SESSION_OPEN) == 0U &&
	    model_count_commands(GHCI_OP_VM_MAP) == 0U, "flat: nothing replayed");

	/* The close places nothing either. */
	ghci_vm_close(&device, vm);
	reclaim();
	expect(device.vm[1] == NULL && model_count_commands(GHCI_OP_SESSION_CLOSE) == 0U && model.faults == 1U, "flat: closed");
	memory_done(memory);
	end_scene("flat");
}

/*
 * CP_HEAP's bound: a session past it is refused with ENOSPC and a chunk
 * past it with ENOMEM before the GPU sees them, so the processor never
 * runs out.
 */
static void
scene_heap_bound(void)
{
	struct ghci_allocation *memory;
	struct ghci_allocation *more;
	struct ghci_mapping *mapping;
	struct ghci_mapping *refused;
	struct ghci_vm *vm;
	struct ghci_vm *other;
	uint64_t root;
	int error;

	/* A bound with room for one session and one level 0 table. */
	error = start_scene(MODEL_PROT, NULL);
	expect(error == 0, "heap: started");
	root = ghci_heap_session_bytes(device.caps.va_bits);
	device.heap_budget = device.heap_used + root + GHCI_TABLE_BYTES;
	memory = host_memory(16U * 1024U);
	more = host_memory(3U * 1024U * 1024U);
	error = ghci_vm_open(&device, &owner_token, &vm);
	expect(error == 0, "heap: one session fits");
	error = ghci_vm_open(&device, &owner_token, &other);
	expect(error == ENOSPC, "heap: a second session is refused");
	error = ghci_vm_map(&device, vm, memory, 0U, &mapping);
	expect(error == 0, "heap: one chunk fits");
	error = ghci_vm_map(&device, vm, more, 0U, &refused);
	expect(error == ENOMEM && model.faults == 0U && model_count_commands(GHCI_OP_VM_MAP) == 1U, "heap: more chunks refused, no MMU_OOM");

	/* The close gives the bound back, and a session fits again. */
	ghci_vm_close(&device, vm);
	reclaim();
	error = ghci_vm_open(&device, &owner_token, &other);
	expect(error == 0, "heap: room again after the close");
	ghci_vm_close(&device, other);
	reclaim();
	memory_done(memory);
	memory_done(more);
	end_scene("heap");
}

/*
 * The close's packets: every live mapping's CMD_VM_UNMAP, then
 * CMD_SESSION_CLOSE, then the fence; never the close before an unmap.
 */
static void
scene_close_order(void)
{
	struct ghci_allocation *memory[3];
	struct ghci_mapping *mapping[3];
	const struct model_command *command;
	struct ghci_vm *vm;
	unsigned before;
	unsigned index;
	int ordered;
	int error;

	/* A session with three mappings. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "close: started");
	error = ghci_vm_open(&device, &owner_token, &vm);
	for (index = 0U; index < 3U && error == 0; index++) {
		memory[index] = host_memory(8U * 1024U);
		error = ghci_vm_map(&device, vm, memory[index], 0U, &mapping[index]);
	}
	expect(error == 0, "close: mapped");

	/* The close. */
	before = model.command_count;
	ghci_vm_close(&device, vm);
	ordered = model.command_count == before + 5U;
	for (index = 0U; index < 5U && ordered; index++) {
		command = &model.commands[before + index];
		if (index < 3U && command->opcode != GHCI_OP_VM_UNMAP)
			ordered = 0;
		if (index == 3U && command->opcode != GHCI_OP_SESSION_CLOSE)
			ordered = 0;
		if (index == 4U && command->opcode != GHCI_OP_FENCE)
			ordered = 0;
	}
	expect(ordered, "close: unmaps, close, fence");
	reclaim();
	expect(device.vm[1] == NULL && model.faults == 0U, "close: retired");
	for (index = 0U; index < 3U; index++)
		memory_done(memory[index]);
	end_scene("close");
}

/*
 * A device fault with sessions open: the reset, then the replay opens
 * every session again and maps its live mappings again, larger than the
 * ring when it must; nobody died.
 */
static void
scene_replay(void)
{
	struct ghci_allocation *memory[REPLAY_MAPPINGS];
	struct ghci_mapping *mapping[REPLAY_MAPPINGS];
	struct ghci_vm *one;
	struct ghci_vm *two;
	unsigned opens;
	unsigned maps;
	unsigned index;
	int error;

	/* Two sessions with 60 mappings each, and a ring of 4 KiB (the replay is larger). */
	test_ring_bytes = 4096U;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "replay: started");
	error = ghci_vm_open(&device, &owner_token, &one);
	if (error == 0)
		error = ghci_vm_open(&device, &owner_token, &two);
	for (index = 0U; index < REPLAY_MAPPINGS && error == 0; index++) {
		memory[index] = host_memory(4096U);
		error = ghci_vm_map(&device, index < REPLAY_MAPPINGS / 2U ? one : two, memory[index], 0U, &mapping[index]);
	}
	expect(error == 0, "replay: mapped");

	/* A fault of the device's own: no culprit. */
	opens = model_count_commands(GHCI_OP_SESSION_OPEN);
	maps = model_count_commands(GHCI_OP_VM_MAP);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 1U, "replay: recovered");
	expect(one->state == GHCI_VM_OPEN && two->state == GHCI_VM_OPEN && stub.session_errors == 0U, "replay: both open again");
	expect(model_count_commands(GHCI_OP_SESSION_OPEN) == opens + 2U && model_count_commands(GHCI_OP_VM_MAP) == maps + REPLAY_MAPPINGS,
	    "replay: opens and maps placed again");
	expect(model.session_active == 0x7U, "replay: active in the model");
	for (index = 0U; index < REPLAY_MAPPINGS; index++) {
		if (model_lookup(mapping[index]->vm->session, mapping[index]->va) == NULL)
			break;
	}
	expect(index == REPLAY_MAPPINGS, "replay: every mapping in the model again");

	/* A session's fault in the middle of the next replay: the last check finds it, and the next pass kills the culprit only. */
	stub.ticks += kern_ms_to_ticks(GHCI_FAULT_STORM_MS + 1000U);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	scene_action = action_replay_fault;
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	scene_action = NULL;
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 3U && scene_action_runs != 0U, "replay: fault in the replay, recovered again");
	expect(one->state == GHCI_VM_DEAD && two->state == GHCI_VM_OPEN && stub.session_errors == 1U, "replay: the culprit of the replay died");
	expect(model.session_active == (1U | (1U << two->session)), "replay: the other replayed");

	/* The sessions go on. */
	ghci_vm_unmap(&device, mapping[0]);
	ghci_vm_close(&device, one);
	ghci_vm_close(&device, two);
	reclaim();
	expect(device.vm[1] == NULL && device.vm[2] == NULL && model.faults == 3U, "replay: closed after");
	for (index = 0U; index < REPLAY_MAPPINGS; index++)
		memory_done(memory[index]);
	end_scene("replay");
}

/*
 * A session's fault under PROT (the processor goes on and writes the
 * culprit's fence): the reset still comes; the culprit dies and its
 * tickets fail, a second culprit is found by SESSION_ACTIVE, a session
 * whose open the processor has not confirmed is not one, the others are
 * replayed, the fault is not counted towards the storm, and a dead
 * session closes without the GPU and its number is not reused before.
 */
static void
scene_culprit(void)
{
	struct ghci_allocation *memory;
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	struct ghci_vm *one;
	struct ghci_vm *two;
	struct ghci_vm *three;
	struct ghci_vm *again;
	uint64_t used;
	unsigned closes;
	int error;

	/* Three sessions; session 1 has a mapping and work whose fence is written. */
	error = start_scene(MODEL_PROT, NULL);
	expect(error == 0, "culprit: started");
	memory = host_memory(8192U);
	error = ghci_vm_open(&device, &owner_token, &one);
	if (error == 0)
		error = ghci_vm_open(&device, &owner_token, &two);
	if (error == 0)
		error = ghci_vm_open(&device, &owner_token, &three);
	if (error == 0)
		error = ghci_vm_map(&device, one, memory, 0U, &mapping);
	if (error == 0)
		error = submit_vm(one, &ticket);
	expect(error == 0, "culprit: set up");

	/* Session 1 faults, and session 2 is no longer active either. */
	model_inject(GHCI_FAULT_KIND_PAGE_FAULT, GHCI_UNIT_SU, one->session, 0x4000U, 0);
	model.session_active &= ~(1U << two->session);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 1U, "culprit: reset");
	expect(one->state == GHCI_VM_DEAD && two->state == GHCI_VM_DEAD && three->state == GHCI_VM_OPEN, "culprit: culprits dead, the other open");
	expect(stub.session_errors == 2U && device.storm_count == 0U, "culprit: reported, not a storm");
	error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == EIO, "culprit: its written fence failed");
	expect(model.session_active == (1U | (1U << three->session)), "culprit: only the survivor replayed");
	error = submit_vm(one, &ticket);
	expect(error == EIO, "culprit: a dead session submits nothing");

	/* The dead session keeps its number until its owner closes it. */
	error = ghci_vm_open(&device, &owner_token, &again);
	expect(error == 0 && again->slot != one->slot && again->slot != two->slot, "culprit: dead numbers not reused");
	closes = model_count_commands(GHCI_OP_SESSION_CLOSE);
	used = device.heap_used;
	ghci_vm_unmap(&device, mapping);
	ghci_vm_close(&device, one);
	ghci_vm_close(&device, two);
	expect(model_count_commands(GHCI_OP_SESSION_CLOSE) == closes && device.vm[1] == NULL && device.vm[2] == NULL,
	    "culprit: dead sessions close without the GPU");
	expect(memory->holds == 1U, "culprit: dead mapping let the memory go");
	expect(device.heap_used == used, "culprit: a dead close returns no table twice");

	/* A session the processor has not confirmed opening is no culprit. */
	model_hold(1);
	error = ghci_vm_open(&device, &owner_token, &one);
	expect(error == 0, "culprit: open not yet confirmed");
	model.session_active &= ~(1U << one->session);
	model_inject(GHCI_FAULT_KIND_PAGE_FAULT, GHCI_UNIT_SU, three->session, 0x4000U, 0);
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(three->state == GHCI_VM_DEAD && one->state == GHCI_VM_OPEN && again->state == GHCI_VM_OPEN, "culprit: unconfirmed open spared");
	ghci_vm_close(&device, one);
	ghci_vm_close(&device, three);
	ghci_vm_close(&device, again);
	reclaim();
	memory_done(memory);
	end_scene("culprit");
}

/*
 * A device fault while a session's work is in flight: that session lost
 * its work and dies; a session that lost only a VM operation does not.
 * Then a CP_HEAP exhaustion: no culprit, counted in its own window.
 */
static void
scene_lost_work(void)
{
	struct ghci_allocation *memory;
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	struct ghci_vm *busy;
	struct ghci_vm *mapper;
	int error;

	/* Work of one session and a map of another, both held. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "lost work: started");
	memory = host_memory(4096U);
	error = ghci_vm_open(&device, &owner_token, &busy);
	if (error == 0)
		error = ghci_vm_open(&device, &owner_token, &mapper);
	model_hold(1);
	if (error == 0)
		error = submit_vm(busy, &ticket);
	if (error == 0)
		error = ghci_vm_map(&device, mapper, memory, 0U, &mapping);
	expect(error == 0, "lost work: placed");

	/* The device faults; the busy session's work is lost, the mapper is replayed. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	model_hold(0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(busy->state == GHCI_VM_DEAD && mapper->state == GHCI_VM_OPEN && stub.session_errors == 1U, "lost work: only the busy one died");
	expect(model_lookup(mapper->session, mapping->va) != NULL, "lost work: the map replayed");
	error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == EIO, "lost work: its ticket failed");

	/* An exhaustion of CP_HEAP that drops a session: no culprit, its own window. */
	model_inject(GHCI_FAULT_KIND_MMU_OOM, GHCI_UNIT_CP, mapper->session, 0x2000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(mapper->state == GHCI_VM_OPEN && device.oom_count == 1U, "lost work: MMU_OOM has no culprit");
	ghci_vm_unmap(&device, mapping);
	ghci_vm_close(&device, mapper);
	ghci_vm_close(&device, busy);
	reclaim();
	memory_done(memory);
	end_scene("lost work");

	/* Under PROT too: the session MMU_OOM dropped from SESSION_ACTIVE is no culprit, and the exhaustion is counted. */
	error = start_scene(MODEL_PROT, NULL);
	expect(error == 0, "lost work: PROT started");
	error = ghci_vm_open(&device, &owner_token, &mapper);
	expect(error == 0, "lost work: PROT open");
	model_inject(GHCI_FAULT_KIND_MMU_OOM, GHCI_UNIT_CP, mapper->session, 0x2000U, 0);
	expect((model.session_active & (1U << mapper->session)) == 0U, "lost work: PROT session dropped by the model");
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(mapper->state == GHCI_VM_OPEN && device.oom_count == 1U && stub.session_errors == 0U, "lost work: PROT MMU_OOM has no culprit");

	/* A device fault that also drops a session from SESSION_ACTIVE counts towards the storm, and kills nobody. */
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	model.session_active &= ~(1U << mapper->session);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(mapper->state == GHCI_VM_OPEN && device.storm_count == 1U, "lost work: device fault counted, no culprit");
	ghci_vm_close(&device, mapper);
	reclaim();
	end_scene("lost work PROT");
}

/*
 * A map, an unmap, an open and a close that meet a recovery at the gate:
 * each starts again after it, places its packets once, and nothing of a
 * closed session is placed.
 */
static void
scene_vm_retry(void)
{
	struct ghci_allocation *memory;
	struct ghci_mapping *mapping;
	struct ghci_vm *vm;
	struct ghci_vm *other;
	unsigned opens;
	unsigned maps;
	int error;

	/* A session, and a fault the service has not taken yet. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "retry: started");
	memory = host_memory(4096U);
	error = ghci_vm_open(&device, &owner_token, &vm);
	expect(error == 0, "retry: open");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);

	/* The map waits at the gate while the recovery replays the session, then maps once. */
	scene_action = action_recover;
	opens = model_count_commands(GHCI_OP_SESSION_OPEN);
	maps = model_count_commands(GHCI_OP_VM_MAP);
	error = ghci_vm_map(&device, vm, memory, 0U, &mapping);
	scene_action = NULL;
	expect(error == 0 && device.resets == 1U, "retry: map after the recovery");
	expect(model_count_commands(GHCI_OP_SESSION_OPEN) == opens + 1U && model_count_commands(GHCI_OP_VM_MAP) == maps + 1U,
	    "retry: one replayed open, one map");
	expect(model_lookup(vm->session, mapping->va) != NULL && model.faults == 1U, "retry: mapped");

	/* An open across a recovery takes one number and places one open (each fault a minute apart, not a storm). */
	stub.ticks += kern_ms_to_ticks(GHCI_FAULT_STORM_MS + 1000U);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	scene_action = action_recover;
	error = ghci_vm_open(&device, &owner_token, &other);
	scene_action = NULL;
	expect(error == 0 && other->slot == 2U && device.resets == 2U && model.session_active == 0x7U, "retry: open after the recovery");

	/* An unmap and a close across a recovery. */
	stub.ticks += kern_ms_to_ticks(GHCI_FAULT_STORM_MS + 1000U);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	scene_action = action_recover;
	ghci_vm_unmap(&device, mapping);
	scene_action = NULL;
	expect(mapping->state == GHCI_MAPPING_UNMAPPING && device.resets == 3U, "retry: unmap after the recovery");
	stub.ticks += kern_ms_to_ticks(GHCI_FAULT_STORM_MS + 1000U);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	scene_action = action_recover;
	ghci_vm_close(&device, vm);
	scene_action = NULL;
	expect(device.resets == 4U && model_count_commands(GHCI_OP_SESSION_CLOSE) == 1U, "retry: close after the recovery");
	ghci_vm_close(&device, other);
	reclaim();
	expect(device.vm[1] == NULL && device.vm[2] == NULL && model.faults == 4U, "retry: no fault of the driver's");
	memory_done(memory);
	end_scene("retry");
}

/*
 * The resources through the core's operations: a storage resource and a
 * blob, copies in and out, the processor's view, an export imported into
 * another session, and the memory kept until the last owner lets it go.
 */
static void
scene_resources(void)
{
	struct gpu_resource_create create;
	struct gpu_blob_create blob;
	struct drv_gpu_mapping view;
	struct gpu_info info;
	struct ghci_resource *storage;
	struct ghci_resource *shown;
	struct ghci_resource *imported;
	void *first;
	void *second;
	void *object;
	void *shared;
	uint32_t resource_id;
	uint8_t bytes[16];
	int error;

	/* Two sessions of the core. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "resources: started");
	error = device.ops.open(&device, &first);
	if (error == 0)
		error = device.ops.open(&device, &second);
	error = device.ops.get_info(&device, first, &info);
	expect(error == 0 && info.max_resources != 0U && info.max_resource_bytes >= (64ULL << 20), "resources: limits");

	/* A storage resource, written and read back. */
	memset(&create, 0, sizeof(create));
	create.bytes = 10000U;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	error = device.ops.resource_create(&device, first, &create, &object);
	storage = object;
	expect(error == 0 && storage->allocation->bytes == 12288U && storage->allocation->bus < 0x100000000ULL, "resources: storage");
	memset(bytes, 0x5a, sizeof(bytes));
	error = device.ops.resource_write(&device, first, storage, 9990U, bytes, 10U);
	expect(error == 0, "resources: write");
	memset(bytes, 0, sizeof(bytes));
	error = device.ops.resource_read(&device, first, storage, 9990U, bytes, 10U);
	expect(error == 0 && bytes[0] == 0x5aU && bytes[9] == 0x5aU, "resources: read back");
	error = device.ops.resource_read(&device, first, storage, 12280U, bytes, 16U);
	expect(error == EINVAL, "resources: read past the end");

	/* A mappable blob, its view, and one of a memory object refused. */
	memset(&blob, 0, sizeof(blob));
	blob.bytes = 4096U;
	blob.flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE;
	error = device.ops.blob_create(&device, first, &blob, &object, &resource_id);
	shown = object;
	expect(error == 0 && resource_id != 0U && resource_id == shown->resource_id, "resources: blob");
	error = device.ops.resource_map(&device, first, shown, &view);
	expect(error == 0 && view.physical == shown->allocation->bus && view.address == shown->allocation->view && view.bytes == 4096U,
	    "resources: view");
	blob.blob_id = 7U;
	error = device.ops.blob_create(&device, first, &blob, &object, &resource_id);
	expect(error == ENOTSUP, "resources: memory object blob refused");
	blob.blob_id = 0U;
	blob.flags = GPU_BLOB_CROSS_DEVICE;
	error = device.ops.blob_create(&device, first, &blob, &object, &resource_id);
	expect(error == ENOTSUP, "resources: cross-device refused");

	/* An export imported into the other session shows the same memory and outlives the original. */
	error = device.ops.share->export_resource(&device, first, shown, NULL, &shared);
	expect(error == 0, "resources: exported");
	error = device.ops.share->import_resource(&device, second, shared, 0U, &object, &resource_id);
	imported = object;
	expect(error == 0 && imported->allocation == shown->allocation && resource_id != shown->resource_id, "resources: imported");
	error = device.ops.share->import_resource(&device, second, shared, GPU_IMPORT_SCANOUT, &object, &resource_id);
	expect(error == ENOTSUP, "resources: scanout import refused");
	device.ops.resource_destroy(&device, first, shown);
	device.ops.share->release(&device, shared);
	expect(imported->allocation->holds == 1U, "resources: import keeps the memory");
	device.ops.resource_destroy(&device, second, imported);
	device.ops.resource_destroy(&device, first, storage);

	/* The sessions close. */
	device.ops.close(&device, first);
	device.ops.close(&device, second);
	end_scene("resources");
}

/* The scene's action: a device fault the first time, the service's recovery after. */
static void
action_fault_then_recover(void)
{
	if (scene_action_runs == 1U) {
		model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
		return;
	}
	ghci_service_step(&device, GHCI_WORK_RECOVER);
}

/*
 * The edges of the VM operations: a map whose wait for the ring's room
 * ends in a recovery gives its new arena back and maps once after it;
 * on a lost device an unmap and a close keep the memory the GPU was
 * given in the quarantine until the detach turned bus mastering off.
 */
static void
scene_vm_edges(void)
{
	struct ghci_allocation *memory;
	struct ghci_allocation *other_memory;
	struct ghci_mapping *mapping;
	struct ghci_mapping *other;
	struct ghci_ticket ticket;
	struct ghci_vm *vm;
	enum ghci_gate gate;
	uint32_t free_dwords;
	uint32_t nop;
	unsigned chunks;
	unsigned index;
	unsigned maps;
	int error;

	/* A 4 KiB ring the held processor lets fill. */
	test_ring_bytes = 4096U;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "edges: started");
	memory = host_memory(4096U);
	other_memory = host_memory(4096U);
	error = ghci_vm_open(&device, &owner_token, &vm);
	expect(error == 0, "edges: open");
	model_hold(1);
	error = submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	gate = ghci_cp_enter(&device, 0U, 1);
	for (;;) {
		free_dwords = (model_read(GHCI_REG_RING_RPTR) - device.ring.wptr - 1U + device.ring.dwords) % device.ring.dwords;
		if (free_dwords < GHCI_DW_VM_MAP || error != 0)
			break;
		(void)ghci_cmd_nop(&nop, 1U);
		error = ghci_cp_place(&device, &nop, 1U);
	}
	ghci_cp_doorbell(&device);
	ghci_cp_leave(&device);
	expect(error == 0 && gate == GHCI_GATE_OPEN, "edges: ring filled");

	/* The map waits for room, a fault ends the wait, and the map goes round after the recovery. */
	maps = model_count_commands(GHCI_OP_VM_MAP);
	scene_action = action_fault_then_recover;
	error = ghci_vm_map(&device, vm, memory, 0U, &mapping);
	scene_action = NULL;
	model_hold(0);
	expect(error == 0 && device.resets == 1U && device.state == GHCI_STATE_RUNNING, "edges: map after the recovery");
	chunks = 0U;
	for (index = 0U; index < GHCI_CHUNKS; index++) {
		if (device.chunk_owner[index] != NULL)
			chunks++;
	}
	expect(chunks == 1U && vm->arenas != NULL && model_count_commands(GHCI_OP_VM_MAP) == maps + 1U, "edges: one arena, one map");
	expect(model_lookup(vm->session, mapping->va) != NULL, "edges: mapped in the model");

	/* The device is lost with the memory mapped: the unmap and the close quarantine what the GPU was given. */
	error = ghci_vm_map(&device, vm, other_memory, 0U, &other);
	expect(error == 0, "edges: second map");
	device.state = GHCI_STATE_LOST;
	ghci_vm_unmap(&device, mapping);
	memory_done(memory);
	ghci_vm_close(&device, vm);
	memory_done(other_memory);
	expect(device.vm[1] == NULL && device.quarantined != NULL && device.quarantined->next != NULL &&
	    device.quarantined->next->next == NULL, "edges: both runs in the quarantine");

	/* The detach gives the quarantine back once the GPU is stopped. */
	error = ghci_device_stop(&device);
	expect(error == 0 && device.quarantined == NULL, "edges: quarantine given back at the detach");
	end_scene("edges");
}

/* Takes a run of local memory, held by the test. */
static struct ghci_allocation *
local_memory(
	uint64_t bytes)
{
	struct ghci_allocation *allocation;
	int error;

	/* The run, held by the test. */
	error = ghci_memory_alloc_local(&device, bytes, &allocation);
	if (error != 0)
		return NULL;
	allocation->holds = 1U;
	return allocation;
}

/* Reports whether the quarantine holds a run of local memory: 1 when it does. */
static int
quarantined_local(void)
{
	struct ghci_allocation *allocation;

	/* Each run of the quarantine. */
	for (allocation = device.quarantined; allocation != NULL; allocation = allocation->next) {
		if (allocation->kind == GHCI_MEMORY_LOCAL)
			return 1;
	}

	/* None of local memory. */
	return 0;
}

/*
 * Local memory of a discrete device: the boot framebuffer kept out of the
 * allocator and the window kept open until the release, runs of local
 * memory at local addresses and mapped with VM_FLAG_LOCAL, a unified
 * device that hands out none, and a boot framebuffer in host memory that
 * the driver leaves alone.
 */
static void
scene_local(void)
{
	struct ghci_allocation *first;
	struct ghci_allocation *second;
	struct ghci_allocation *third;
	struct ghci_allocation *host;
	struct ghci_allocation *spare;
	struct ghci_mapping *mapping;
	struct ghci_mapping *host_mapping;
	const struct model_mapping *seen;
	struct ghci_vm *vm;
	uint32_t ctrl;
	int error;

	/* A boot framebuffer of 1 MiB at 1 MiB into local memory, scanned out (DISP2 CTRL.ENABLE). */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	model.window[(MODEL_DISP2_BLOCK + 0x10U) / 4U] = 1U;
	model.window[(MODEL_DISP2_BLOCK + 0x20U) / 4U] = (uint32_t)MODEL_LOCAL_BASE + 0x100000U;
	model.window[(MODEL_DISP2_BLOCK + 0x28U) / 4U] = 4096U;
	model.window[(MODEL_DISP2_BLOCK + 0x2CU) / 4U] = (256U << 16) | 1024U;
	error = start_device();
	expect(error == 0 && device.local_ready && ghci_memory_local_bytes(&device) == MODEL_LOCAL_BYTES, "local: discrete started");
	expect(device.boot_reserved && device.boot_count == 1U && device.boot_offset[0] == 0x100000U && device.boot_bytes[0] == 0x100000U,
	    "local: boot framebuffer found");
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(ctrl == 1U && device.window_open && device.window_users == 1U, "local: window kept open for the boot console");

	/* Runs of local memory go around the boot framebuffer, at local addresses, not seen by the processor without a window. */
	first = local_memory(0x100000U);
	second = local_memory(0x100000U);
	expect(first != NULL && second != NULL && first->kind == GHCI_MEMORY_LOCAL, "local: runs taken");
	expect(first != NULL && first->bus == MODEL_LOCAL_BASE && first->view == NULL, "local: first run below the framebuffer");
	expect(second != NULL && second->bus == MODEL_LOCAL_BASE + 0x200000U, "local: second run past the framebuffer");

	/* A run larger than what is left is refused. */
	spare = NULL;
	error = ghci_memory_alloc_local(&device, MODEL_LOCAL_BYTES, &spare);
	expect(error == ENOMEM && spare == NULL, "local: a run past local memory refused");

	/* The release after the first flip: the range is the allocator's again, and the unused window closes. */
	ghci_memory_boot_release(&device);
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(!device.boot_reserved && ctrl == 0U && !device.window_open && device.window_users == 0U,
	    "local: boot framebuffer released, window closed");
	ghci_memory_boot_release(&device);
	expect(device.window_users == 0U && device.local_map.count == 2U, "local: a second release changes nothing");
	third = local_memory(0x100000U);
	expect(third != NULL && third->bus == MODEL_LOCAL_BASE + 0x100000U, "local: the framebuffer's range handed out after the release");

	/* A mapping of local memory carries VM_FLAG_LOCAL and the local address; host memory does not. */
	host = host_memory(64U * 1024U);
	vm = NULL;
	mapping = NULL;
	host_mapping = NULL;
	error = ghci_vm_open(&device, &owner_token, &vm);
	if (error == 0 && first != NULL)
		error = ghci_vm_map(&device, vm, first, 0U, &mapping);
	if (error == 0 && host != NULL)
		error = ghci_vm_map(&device, vm, host, 0U, &host_mapping);
	expect(error == 0 && mapping != NULL && host_mapping != NULL, "local: mapped");
	seen = mapping != NULL ? model_lookup(vm->session, mapping->va) : NULL;
	expect(seen != NULL && seen->phys == MODEL_LOCAL_BASE &&
	    seen->flags == (GHCI_VM_FLAG_READ | GHCI_VM_FLAG_WRITE | GHCI_VM_FLAG_LOCAL), "local: VM_MAP with VM_FLAG_LOCAL");
	seen = host_mapping != NULL ? model_lookup(vm->session, host_mapping->va) : NULL;
	expect(seen != NULL && (seen->flags & GHCI_VM_FLAG_LOCAL) == 0U && (seen->flags & GHCI_VM_FLAG_COHERENT) != 0U,
	    "local: host memory mapped without VM_FLAG_LOCAL");
	expect(model.faults == 0U && device.state == GHCI_STATE_RUNNING, "local: no fault");

	/* The unmap gives the run back to the allocator once its fence passed and the test lets it go. */
	if (mapping != NULL)
		ghci_vm_unmap(&device, mapping);
	if (host_mapping != NULL)
		ghci_vm_unmap(&device, host_mapping);
	reclaim();
	if (first != NULL)
		memory_done(first);
	spare = local_memory(0x100000U);
	expect(spare != NULL && spare->bus == MODEL_LOCAL_BASE, "local: an unmapped run is handed out again");
	if (vm != NULL)
		ghci_vm_close(&device, vm);
	reclaim();
	if (spare != NULL)
		memory_done(spare);
	if (second != NULL)
		memory_done(second);
	if (third != NULL)
		memory_done(third);
	if (host != NULL)
		memory_done(host);
	expect(device.local_map.count == 0U, "local: every run back in the allocator");
	end_scene("local");
	expect(device.local_extents == NULL && !device.local_ready, "local: allocator gone at the detach");

	/*
	 * A discrete device with a window and no boot framebuffer: the window
	 * closes at once, and a run of local memory has no view of the
	 * processor, which never reads or writes local memory.
	 */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	error = start_device();
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(error == 0 && !device.boot_reserved && ctrl == 0U && !device.window_open && device.window_users == 0U,
	    "local: unused window closed at the start");
	first = local_memory(0x2000U);
	expect(first != NULL && first->kind == GHCI_MEMORY_LOCAL && first->view == NULL, "local: no view of a run");
	if (first != NULL)
		memory_done(first);
	end_scene("local: window");

	/* A unified device hands out no local memory, and leaves no window to manage. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	spare = NULL;
	expect(error == 0 && !device.local_ready && ghci_memory_local_bytes(&device) == 0U, "local: unified uses none");
	error = ghci_memory_alloc_local(&device, 4096U, &spare);
	expect(error == ENOTSUP && spare == NULL, "local: unified refuses a run");
	end_scene("local: unified");

	/* A boot framebuffer in host memory on a discrete device is not the driver's to keep; the unused window closes. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	model.window[(MODEL_DISP2_BLOCK + 0x10U) / 4U] = 1U;
	model.window[(MODEL_DISP2_BLOCK + 0x20U) / 4U] = (uint32_t)MODEL_HOST_BASE;
	model.window[(MODEL_DISP2_BLOCK + 0x28U) / 4U] = 4096U;
	model.window[(MODEL_DISP2_BLOCK + 0x2CU) / 4U] = (16U << 16) | 1024U;
	error = start_device();
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(error == 0 && device.local_ready && !device.boot_reserved && ctrl == 0U, "local: boot framebuffer in host memory left alone");
	end_scene("local: host framebuffer");
}

/*
 * Local memory of a lost device: a run the GPU was given waits in the
 * quarantine when it is let go, and goes back to the allocator only at
 * the detach, after bus mastering is off.
 */
static void
scene_local_lost(void)
{
	struct ghci_allocation *run;
	struct ghci_mapping *mapping;
	struct ghci_ticket ticket;
	enum ghci_gate gate;
	struct ghci_vm *vm;
	int error;

	/* A discrete device with a mapped run of local memory. */
	error = start_scene(MODEL_KC705_DISCRETE, NULL);
	run = local_memory(0x10000U);
	vm = NULL;
	mapping = NULL;
	if (error == 0 && run != NULL)
		error = ghci_vm_open(&device, &owner_token, &vm);
	if (error == 0)
		error = ghci_vm_map(&device, vm, run, 0U, &mapping);
	expect(error == 0 && run != NULL && mapping != NULL, "local lost: mapped");

	/* The device is lost: every recovery's check fails. */
	model_hold(1);
	(void)submit_nops(TEST_SUBMIT_DWORDS, &ticket);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	model_set_bus_master(0);
	scene_action = action_recover;
	gate = ghci_cp_enter(&device, 0U, 1);
	scene_action = NULL;
	expect(gate == GHCI_GATE_LOST && device.state == GHCI_STATE_LOST, "local lost: lost");

	/* The mapping and the test let the run go: it waits in the quarantine, still out of the allocator. */
	if (mapping != NULL)
		ghci_vm_unmap(&device, mapping);
	if (vm != NULL)
		ghci_vm_close(&device, vm);
	reclaim();
	if (run != NULL)
		memory_done(run);
	expect(quarantined_local() && device.local_map.count == 1U, "local lost: the run is quarantined");

	/* The detach gives it back once bus mastering is off, and the allocator goes. */
	error = ghci_device_stop(&device);
	expect(error == 0 && device.quarantined == NULL && device.local_extents == NULL, "local lost: given back at the detach");
	end_scene("local lost");
}

/* Sets the display controller's block as a boot display leaves it: CTRL, the image scanned out, its pitch and height. */
static void
boot_display(
	uint32_t ctrl,
	uint64_t base,
	uint32_t stride,
	uint32_t height)
{
	/* CTRL, FB_BASE, FB_STRIDE and FB_SIZE (1024 pixels wide). */
	model.window[(MODEL_DISP2_BLOCK + 0x10U) / 4U] = ctrl;
	model.window[(MODEL_DISP2_BLOCK + 0x20U) / 4U] = (uint32_t)base;
	model.window[(MODEL_DISP2_BLOCK + 0x28U) / 4U] = stride;
	model.window[(MODEL_DISP2_BLOCK + 0x2CU) / 4U] = (height << 16) | 1024U;
}

/* Renames a block of the model's extended capability list, before the start reads it. */
static void
extcap_rename(
	uint32_t from,
	uint32_t to)
{
	uint32_t entry;
	uint32_t word;
	unsigned index;

	/* Each entry of the list, two dwords apart. */
	entry = MODEL_EXTCAP_LIST;
	for (index = 0U; index < 16U; index++) {
		word = model.window[entry / 4U];
		if ((word & 0xffU) == from) {
			model.window[entry / 4U] = (word & ~0xffU) | to;
			return;
		}
		if ((word >> 16) == 0U)
			return;
		entry += 8U;
	}
}

/*
 * What a boot display reads of local memory: the image scanned out, the
 * image of a pending flip and the cursor's image on DISP2 (joined where
 * they touch), only the image scanned out on DISP, nothing on a disabled
 * controller, images that straddle the ends of local memory or start
 * inside a page, and a detach before the release that keeps the window
 * open for the boot console.
 */
static void
scene_boot_ranges(void)
{
	struct ghci_allocation *first;
	struct ghci_allocation *second;
	uint32_t ctrl;
	int error;

	/* DISP2: 1 MiB at 1 MiB, a pending flip to 4 MiB, and a cursor right after the image, inside a page. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(1U, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	model.window[(MODEL_DISP2_BLOCK + 0x14U) / 4U] = 1U << 29;
	model.window[(MODEL_DISP2_BLOCK + 0x24U) / 4U] = (uint32_t)MODEL_LOCAL_BASE + 0x400000U;
	model.window[(MODEL_DISP2_BLOCK + 0x08U) / 4U] = 1U << 8;
	model.window[(MODEL_DISP2_BLOCK + 0xA0U) / 4U] = (uint32_t)MODEL_LOCAL_BASE + 0x200800U;
	error = start_device();
	expect(error == 0 && device.local_ready && device.boot_count == 2U, "boot ranges: two ranges");
	expect(device.boot_offset[0] == 0x100000U && device.boot_bytes[0] == 0x105000U, "boot ranges: image and cursor joined");
	expect(device.boot_offset[1] == 0x400000U && device.boot_bytes[1] == 0x100000U, "boot ranges: the pending flip's image");
	expect(device.local_map.count == 2U, "boot ranges: both out of the allocator");

	/* Runs go into the gaps around them. */
	first = local_memory(0x100000U);
	second = local_memory(0x100000U);
	expect(first != NULL && first->bus == MODEL_LOCAL_BASE, "boot ranges: a run below the image");
	expect(second != NULL && second->bus == MODEL_LOCAL_BASE + 0x205000U, "boot ranges: a run between the ranges");
	if (first != NULL)
		memory_done(first);
	if (second != NULL)
		memory_done(second);

	/* The release gives both back. */
	ghci_memory_boot_release(&device);
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(device.local_map.count == 0U && device.boot_count == 0U && ctrl == 0U, "boot ranges: released");
	end_scene("boot ranges");

	/* DISP2 without a pending flip: the image only (FB_NEXT is not read). */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(1U, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	model.window[(MODEL_DISP2_BLOCK + 0x24U) / 4U] = (uint32_t)MODEL_LOCAL_BASE + 0x400000U;
	error = start_device();
	expect(error == 0 && device.boot_count == 1U && device.boot_offset[0] == 0x100000U && device.boot_bytes[0] == 0x100000U,
	    "boot ranges: no pending flip");
	end_scene("boot ranges: no flip");

	/* DISP (ID 1): the image only, whatever its STATUS and the other registers say. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	extcap_rename(GHCI_EXTCAP_ID_DISP2, GHCI_EXTCAP_ID_DISP);
	boot_display(1U, MODEL_LOCAL_BASE + 0x200000U, 4096U, 128U);
	model.window[(MODEL_DISP2_BLOCK + 0x14U) / 4U] = 1U << 29;
	model.window[(MODEL_DISP2_BLOCK + 0x24U) / 4U] = (uint32_t)MODEL_LOCAL_BASE + 0x400000U;
	model.window[(MODEL_DISP2_BLOCK + 0x08U) / 4U] = 1U << 8;
	model.window[(MODEL_DISP2_BLOCK + 0xA0U) / 4U] = (uint32_t)MODEL_LOCAL_BASE + 0x600000U;
	error = start_device();
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(error == 0 && device.caps.block_offset[GHCI_EXTCAP_ID_DISP2] == 0U && device.boot_count == 1U &&
	    device.boot_offset[0] == 0x200000U && device.boot_bytes[0] == 0x80000U && ctrl == 1U, "boot ranges: DISP");
	end_scene("boot ranges: DISP");

	/* A disabled DISP2 reads nothing: no range, and the window closes. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(0U, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(error == 0 && device.local_ready && !device.boot_reserved && device.local_map.count == 0U && ctrl == 0U,
	    "boot ranges: disabled controller");
	end_scene("boot ranges: disabled");

	/* An image that starts below local memory is cut to its first byte. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(1U, MODEL_LOCAL_BASE - 0x1000U, 4096U, 4U);
	error = start_device();
	expect(error == 0 && device.boot_count == 1U && device.boot_offset[0] == 0U && device.boot_bytes[0] == 0x3000U,
	    "boot ranges: straddles the start");
	first = local_memory(0x1000U);
	expect(first != NULL && first->bus == MODEL_LOCAL_BASE + 0x3000U, "boot ranges: a run after a range at offset 0");
	if (first != NULL)
		memory_done(first);
	end_scene("boot ranges: start");

	/* An image that runs past the end of local memory is cut to its last page. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(1U, MODEL_LOCAL_BASE + MODEL_LOCAL_BYTES - 0x1800U, 4096U, 3U);
	error = start_device();
	expect(error == 0 && device.boot_count == 1U && device.boot_offset[0] == MODEL_LOCAL_BYTES - 0x2000U &&
	    device.boot_bytes[0] == 0x2000U, "boot ranges: straddles the end");
	end_scene("boot ranges: end");

	/* An image that starts inside a page takes the whole pages it touches. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(1U, MODEL_LOCAL_BASE + 0x1800U, 4096U, 1U);
	error = start_device();
	expect(error == 0 && device.boot_count == 1U && device.boot_offset[0] == 0x1000U && device.boot_bytes[0] == 0x2000U,
	    "boot ranges: inside a page");
	end_scene("boot ranges: inside a page");

	/* A detach before the release: the allocator goes with only the range in it, and the window stays open. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(1U, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	first = local_memory(0x1000U);
	if (first != NULL)
		memory_done(first);
	if (error == 0)
		error = ghci_device_stop(&device);
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(error == 0 && device.local_extents == NULL && device.boot_reserved && ctrl == 1U,
	    "boot ranges: detach with the range kept, window open");
	end_scene("boot ranges: detach");
}

/*
 * The start keeps everything in host memory on a discrete device it
 * cannot hand local memory out of: without a page-table walker, and with
 * a LOCAL_BASE that is not on a page; the boot framebuffer still keeps
 * the window open for the boot console.
 */
static void
scene_local_fallbacks(void)
{
	struct ghci_allocation *run;
	uint32_t ctrl;
	int error;

	/* LOCAL_BASE not on a page, with a boot framebuffer. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	model.window[(MODEL_MEM_BLOCK + GHCI_MEM_LOCAL_BASE) / 4U] = (uint32_t)MODEL_LOCAL_BASE + 0x800U;
	boot_display(1U, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	run = NULL;
	expect(error == 0 && !device.local_ready && ghci_memory_local_bytes(&device) == 0U && device.local_extents == NULL,
	    "fallbacks: LOCAL_BASE off a page leaves the device unified");
	expect(device.boot_reserved && ctrl == 1U, "fallbacks: the boot console keeps the window");
	error = ghci_memory_alloc_local(&device, 4096U, &run);
	expect(error == ENOTSUP && run == NULL, "fallbacks: no local run");
	ghci_memory_boot_release(&device);
	ctrl = model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U];
	expect(!device.boot_reserved && ctrl == 0U, "fallbacks: the release closes the window");
	end_scene("fallbacks: base");

	/* No page-table walker. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	model.window[GHCI_REG_DEVICE_CAPS / 4U] &= ~GHCI_CAP_HW_WALKER;
	error = start_device();
	expect(error == 0 && !device.local_ready && ghci_memory_local_bytes(&device) == 0U, "fallbacks: no walker leaves the device unified");
	end_scene("fallbacks: walker");
}

/*
 * A device lost in a recovery after a session's fault under PROT: the
 * culprit's submission whose fence the device wrote fails as a reset
 * would fail it, though the device never ended the generation with a
 * reset.
 */
static void
scene_lost_culprit(void)
{
	struct ghci_ticket ticket;
	struct ghci_ticket other_ticket;
	struct ghci_vm *one;
	struct ghci_vm *other;
	int error;

	/* Two sessions with work whose fences are written, the culprit's last. */
	error = start_scene(MODEL_PROT, NULL);
	one = NULL;
	other = NULL;
	if (error == 0)
		error = ghci_vm_open(&device, &owner_token, &one);
	if (error == 0)
		error = ghci_vm_open(&device, &owner_token, &other);
	if (error == 0)
		error = submit_vm(other, &other_ticket);
	if (error == 0)
		error = submit_vm(one, &ticket);
	expect(error == 0, "lost culprit: set up");

	/* Session 1 faults, and the reset never ends: the device is lost. */
	model.switches.reset_stuck = 1;
	model_inject(GHCI_FAULT_KIND_PAGE_FAULT, GHCI_UNIT_SU, one != NULL ? one->session : 1U, 0x4000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_LOST, "lost culprit: lost");

	/* The culprit's written fence failed; the other session's passed. */
	error = ghci_cp_wait(&device, &ticket, TEST_WAIT_MS);
	expect(error == EIO, "lost culprit: the culprit's written fence failed");
	error = ghci_cp_wait(&device, &other_ticket, TEST_WAIT_MS);
	expect(error == 0, "lost culprit: the other session's written fence passed");
	model.switches.reset_stuck = 0;
	model.reset_hung = 0;
	if (one != NULL)
		ghci_vm_close(&device, one);
	if (other != NULL)
		ghci_vm_close(&device, other);
	reclaim();
	end_scene("lost culprit");
}

/*
 * The display scenes (DISP2): the open, its lease and the generation the
 * scenes present to.
 */
static void *display_open;
static uint64_t display_lease;
static uint64_t display_generation;

/* Reads a register of the display controller's block. */
static uint32_t
dc_read(
	uint32_t reg)
{
	return model_read(MODEL_DISP2_BLOCK + reg);
}

/* Runs one round of the service thread's display work, as the thread would after an event. */
static void
display_round(void)
{
	ghci_service_step(&device, GHCI_WORK_DISPLAY);
}

/* Opens a session of the core and takes the display's lease.  Returns what the claim returned. */
static int
display_take(void)
{
	struct gpu_display_info info;
	struct gpu_display_claim claim;
	int error;

	/* The open, and the output's generation. */
	error = device.ops.open(&device, &display_open);
	if (error != 0)
		return error;
	memset(&info, 0, sizeof(info));
	error = device.ops.display->query(&device, display_open, &info);
	if (error != 0)
		return error;
	display_generation = info.generation;

	/* The lease of plane 0. */
	memset(&claim, 0, sizeof(claim));
	claim.display_id = 1U;
	claim.generation = display_generation;
	error = device.ops.display->claim(&device, display_open, &claim);
	display_lease = claim.lease;
	return error;
}

/* Reports the routes the scan-out query offers for the output now (GPU_SCANOUT_*), or 0 when the query fails. */
static uint32_t
display_routes(void)
{
	struct gpu_scanout_constraints constraints;
	int error;

	/* The query of the one output's generation. */
	memset(&constraints, 0, sizeof(constraints));
	constraints.display_id = 1U;
	constraints.generation = display_generation;
	error = device.ops.scanout->constraints(&device, display_open, &constraints);
	if (error != 0)
		return 0U;
	return constraints.flags;
}

/* Makes storage (a blob when asked) of the scenes' open; NULL when it could not. */
static struct ghci_resource *
display_storage(
	uint64_t bytes,
	int blob)
{
	struct gpu_resource_create create;
	struct gpu_blob_create request;
	uint32_t resource_id;
	void *object;
	int error;

	/* A blob, mappable and shareable, or ordinary storage. */
	if (blob) {
		memset(&request, 0, sizeof(request));
		request.bytes = bytes;
		request.flags = GPU_BLOB_MAPPABLE | GPU_BLOB_SHAREABLE;
		error = device.ops.blob_create(&device, display_open, &request, &object, &resource_id);
	} else {
		memset(&create, 0, sizeof(create));
		create.bytes = bytes;
		create.usage = GPU_RESOURCE_USAGE_STORAGE;
		error = device.ops.resource_create(&device, display_open, &create, &object);
	}
	if (error != 0)
		return NULL;
	return object;
}

/* Records a present of storage and, when wait is set, waits for its adoption as the core does. */
static int
display_present(
	struct ghci_resource *storage,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	uint64_t offset,
	uint32_t flags,
	int wait,
	uint64_t *sequence)
{
	struct gpu_display_present request;
	uint64_t adopted;
	int error;

	/* The present. */
	memset(&request, 0, sizeof(request));
	request.lease = display_lease;
	request.generation = display_generation;
	request.width = width;
	request.height = height;
	request.stride = stride;
	request.offset = offset;
	request.format = GPU_PIXEL_BGRA8888;
	request.flags = flags;
	error = device.ops.display->present(&device, display_open, storage, &request);
	if (error != 0)
		return error;
	*sequence = request.sequence;

	/* Its adoption. */
	if (!wait)
		return 0;
	error = device.ops.display->adoption_wait(&device, display_open, display_lease, sequence, &adopted);
	return error;
}

/* Fills storage with a pattern: each 32-bit word its offset plus a seed. */
static void
display_pattern(
	struct ghci_resource *storage,
	uint32_t seed)
{
	uint32_t *words;
	uint64_t index;

	/* Each word of the host memory. */
	words = (uint32_t *)(void *)storage->allocation->view;
	for (index = 0U; index < storage->allocation->bytes / 4U; index++)
		words[index] = (uint32_t)index * 4U + seed;
}

/* Reports whether a frame at a bus address holds the rows of storage with the pattern: 1 when it does. */
static int
display_frame_holds(
	uint64_t frame,
	uint32_t frame_stride,
	struct ghci_resource *storage,
	uint32_t width,
	uint32_t height,
	uint32_t stride)
{
	uint8_t *target;
	uint32_t row;

	/* Each row. */
	target = model_dma(frame, (uint64_t)frame_stride * height);
	if (target == NULL)
		return 0;
	for (row = 0U; row < height; row++) {
		if (memcmp(target + (uint64_t)row * frame_stride, storage->allocation->view + (uint64_t)row * stride, width * 4U) != 0)
			return 0;
	}
	return 1;
}

/* Lets the display's lease go and closes the open. */
static void
display_give_up(void)
{
	struct gpu_display_release release;

	/* The release, then the close. */
	memset(&release, 0, sizeof(release));
	release.lease = display_lease;
	(void)device.ops.display->release(&device, display_open, &release);
	device.ops.close(&device, display_open);
	display_open = NULL;
}

/*
 * The capabilities and the modes: a DISP2 device offers the display, its
 * events, the cursor and the scan-out query; the modes from the
 * controller's table; the claim; the first present lights the controller
 * with DISP_SET and FLIP through the ring; the release turns it dark.
 */
static void
scene_display_modes(void)
{
	struct gpu_display_info info;
	struct gpu_display_mode mode;
	struct gpu_display_claim claim;
	struct ghci_resource *storage;
	void *other;
	uint64_t sequence;
	int error;

	/* The device and its capabilities. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0 && device.display.present && device.display.mode_count == 2U, "display modes: started");
	expect((device.ops.capabilities & (GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS)) == (GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS) &&
	    device.ops.display == &ghci_display_cursor_ops && device.ops.scanout == &ghci_scanout_ops &&
	    stub_gpu_ops_validate(&device.ops) == 0, "display modes: capabilities and complete callbacks");
	expect(model.irq_enable == (GHCI_IRQ_FENCE | GHCI_IRQ_FAULT | GHCI_IRQ_WATCHDOG | GHCI_IRQ_DISPLAY) &&
	    dc_read(GHCI_DC_IRQ_ENABLE) == (GHCI_DC_IRQ_FLIP_DONE | GHCI_DC_IRQ_CURSOR_DONE | GHCI_DC_IRQ_HOTPLUG | GHCI_DC_IRQ_ERROR) &&
	    model.display.resets == 1U && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U, "display modes: dark and its events");

	/* The output. */
	error = device.ops.open(&device, &display_open);
	memset(&info, 0, sizeof(info));
	error = device.ops.display->query(&device, display_open, &info);
	expect(error == 0 && info.count == 1U && (info.flags & (GPU_DISPLAY_CONNECTED | GPU_DISPLAY_HAS_CURSOR)) == (GPU_DISPLAY_CONNECTED | GPU_DISPLAY_HAS_CURSOR) &&
	    info.formats == (GPU_DISPLAY_FORMAT_BGRA8888 | GPU_DISPLAY_FORMAT_RGBA8888) && info.max_width == 1280U &&
	    info.max_height == 720U && info.max_frame_bytes == 5120ULL * 720U, "display modes: query");
	display_generation = info.generation;

	/* The modes, enumerated and checked. */
	memset(&mode, 0, sizeof(mode));
	mode.display_id = 1U;
	mode.generation = display_generation;
	mode.operation = GPU_DISPLAY_MODE_ENUMERATE;
	mode.index = 1U;
	error = device.ops.display->mode(&device, display_open, &mode);
	expect(error == 0 && mode.count == 2U && mode.width == 640U && mode.height == 480U && mode.refresh_millihz == 59940U,
	    "display modes: 640 x 480 at 59.94 Hz");
	mode.index = 0U;
	error = device.ops.display->mode(&device, display_open, &mode);
	expect(error == 0 && mode.width == 1280U && mode.height == 720U && mode.refresh_millihz == 60000U, "display modes: 1280 x 720 at 60 Hz");
	mode.operation = GPU_DISPLAY_MODE_VALIDATE;
	mode.width = 800U;
	mode.height = 600U;
	error = device.ops.display->mode(&device, display_open, &mode);
	expect(error == EINVAL, "display modes: a size of no mode");
	mode.generation = display_generation + 1U;
	error = device.ops.display->mode(&device, display_open, &mode);
	expect(error == ESTALE, "display modes: an old generation");

	/* The claim, and another open's refused. */
	memset(&claim, 0, sizeof(claim));
	claim.display_id = 1U;
	claim.generation = display_generation;
	error = device.ops.display->claim(&device, display_open, &claim);
	display_lease = claim.lease;
	expect(error == 0 && display_lease == 1U && device.display.frames[0] != NULL && device.display.surfaces[1] != NULL,
	    "display modes: claimed, with the frames and surfaces");
	error = device.ops.open(&device, &other);
	claim.lease = 0U;
	if (error == 0)
		error = device.ops.display->claim(&device, other, &claim);
	expect(error == EBUSY, "display modes: a second claim");
	device.ops.close(&device, other);

	/* The first present lights the controller through the ring. */
	storage = display_storage(2560U * 480U, 0);
	display_pattern(storage, 7U);
	error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0 && sequence == 1U && device.display.adopted == 1U, "display modes: first present adopted");
	expect((dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U && dc_read(GHCI_DC_H_ACTIVE) == 640U &&
	    dc_read(GHCI_DC_V_ACTIVE) == 480U && dc_read(GHCI_DC_FB_STRIDE) == 2560U &&
	    dc_read(GHCI_DC_FB_SIZE) == (640U | (480U << 16)) && dc_read(GHCI_DC_H_SYNC) == 96U &&
	    dc_read(GHCI_DC_FB_BASE) == (uint32_t)device.display.frames[0]->bus, "display modes: lit on the frame");
	expect(model_count_commands(GHCI_OP_DISP_SET) >= 3U && model_count_commands(GHCI_OP_FLIP) == 1U &&
	    model.display.bad_writes == 0U && model.display.idle_waits_lit == 0U && model.display.flip_waits == 0U,
	    "display modes: DISP_SET and FLIP, no refused write");
	expect(model_count_commands(GHCI_OP_COPY_BUFFER) == 1U && device.display.frame_maps[0] != NULL &&
	    display_frame_holds(device.display.frames[0]->bus, 2560U, storage, 640U, 480U, 2560U),
	    "display modes: copied by the blitter into the host frame");

	/* The release turns the controller dark. */
	device.ops.resource_destroy(&device, display_open, storage);
	display_give_up();
	expect((dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U && !device.display.ending && device.display.owner == NULL,
	    "display modes: dark after the release");
	expect(model.display.bad_writes == 0U, "display modes: no refused write at the end");
	end_scene("display modes");
}

/*
 * Presents: a shared blob scanned where it is and held until the next
 * image is on the screen, a mailbox present replacing the one waiting,
 * FIFO presents that wait for room (never two flips of a plane at once),
 * and the presents the controller cannot show.
 */
static void
scene_display_present(void)
{
	struct ghci_resource *first;
	struct ghci_resource *second;
	struct ghci_resource *third;
	uint64_t sequence;
	uint64_t mailbox;
	unsigned takes;
	int error;

	/* A lease, and three blobs of the large mode. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	expect(error == 0, "display present: claimed");
	first = display_storage(5120U * 720U, 1);
	second = display_storage(5120U * 720U, 1);
	third = display_storage(5120U * 720U, 1);
	expect(first != NULL && second != NULL && third != NULL, "display present: blobs");

	/* A shared present: the blob's own address, held by the display. */
	error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0 && dc_read(GHCI_DC_FB_BASE) == (uint32_t)first->allocation->bus && first->allocation->holds == 2U,
	    "display present: shared, held");

	/* The next shared image on the screen lets the first go. */
	error = display_present(second, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0 && dc_read(GHCI_DC_FB_BASE) == (uint32_t)second->allocation->bus && first->allocation->holds == 1U &&
	    second->allocation->holds == 2U, "display present: the first let go");

	/* Mailbox: the first placed, the second waiting is replaced by the third, which is adopted with it. */
	takes = model.display.takes[0];
	error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB, 0, &sequence);
	if (error == 0)
		error = display_present(second, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB, 0, &mailbox);
	expect(error == 0 && device.display.queue_count == 1U, "display present: one waiting");
	if (error == 0)
		error = display_present(third, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB, 1, &mailbox);
	ghci_scan_flush(&device);
	expect(error == 0 && device.display.adopted == mailbox && mailbox == sequence + 2U &&
	    model.display.takes[0] == takes + 2U && dc_read(GHCI_DC_FB_BASE) == (uint32_t)third->allocation->bus &&
	    second->allocation->holds == 1U, "display present: mailbox replaced, adopted with its replacement");

	/* FIFO: four presents in a row wait for room; the controller never sees two flips of a plane at once. */
	error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	if (error == 0)
		error = display_present(second, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	if (error == 0)
		error = display_present(third, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	if (error == 0)
		error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0 && device.display.adopted == sequence && model.display.flip_waits == 0U, "display present: FIFO waits for room");

	/* What the controller cannot show. */
	error = display_present(first, 1280U, 720U, 5120U, 4U, GPU_DISPLAY_PRESENT_BLOB, 0, &sequence);
	expect(error == EINVAL, "display present: a first row off the memory line");
	error = display_present(first, 800U, 600U, 3200U, 0U, GPU_DISPLAY_PRESENT_BLOB, 0, &sequence);
	expect(error == EINVAL, "display present: a size of no mode");
	error = display_present(first, 1280U, 720U, 5120U, 4096U, 0U, 0, &sequence);
	expect(error == EINVAL, "display present: past the storage");

	/* The lease goes and the blobs with it. */
	display_give_up();
	ghci_scan_flush(&device);
	expect(device.display.shown.memory == NULL && model.display.bad_writes == 0U, "display present: nothing held after the release");
	end_scene("display present");
}

/*
 * Copied presents of a discrete GPU: into frames of local memory by the
 * blitter through session 0's mappings; a GPU without a blitter and
 * without the copy program offers no copied route (the processor never
 * copies a frame), and its shared presents go on.
 */
static void
scene_display_copied(void)
{
	struct ghci_resource *storage;
	struct ghci_allocation *frame;
	uint64_t sequence;
	unsigned copies;
	int error;

	/* With the blitter: the frames are local memory, mapped into session 0. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	error = start_device();
	if (error == 0)
		error = display_take();
	expect(error == 0 && device.display.frames[0]->kind == GHCI_MEMORY_LOCAL && device.display.frame_maps[0] != NULL &&
	    device.display.frame_maps[1] != NULL, "display copied: local frames mapped");
	expect(display_routes() == (GPU_SCANOUT_SHARED | GPU_SCANOUT_COPY), "display copied: both routes offered");
	storage = display_storage(2560U * 480U, 0);
	display_pattern(storage, 11U);
	copies = model_count_commands(GHCI_OP_COPY_BUFFER);
	error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	frame = device.display.frames[0];
	expect(error == 0 && model_count_commands(GHCI_OP_COPY_BUFFER) == copies + 1U &&
	    dc_read(GHCI_DC_FB_BASE) == (uint32_t)frame->bus && display_frame_holds(frame->bus, 2560U, storage, 640U, 480U, 2560U),
	    "display copied: by the blitter into local memory");

	/* A storage of another stride is copied a row at a time; the next frame is the other one. */
	device.ops.resource_destroy(&device, display_open, storage);
	storage = display_storage(4096U * 480U, 0);
	display_pattern(storage, 13U);
	copies = model_count_commands(GHCI_OP_COPY_BUFFER);
	error = display_present(storage, 640U, 480U, 4096U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	frame = device.display.frames[1];
	expect(error == 0 && model_count_commands(GHCI_OP_COPY_BUFFER) == copies + 480U &&
	    dc_read(GHCI_DC_FB_BASE) == (uint32_t)frame->bus && display_frame_holds(frame->bus, 2560U, storage, 640U, 480U, 4096U),
	    "display copied: row by row into the other frame");
	device.ops.resource_destroy(&device, display_open, storage);
	display_give_up();
	expect(model.display.bad_writes == 0U, "display copied: no refused write");
	end_scene("display copied");

	/*
	 * Without a blitter and without the executor (this test links the
	 * driver's core alone, as on a GPU without a shader unit), so without
	 * the copy program: only the shared route is offered, there are no
	 * frames, a copied present is refused with nothing written or placed,
	 * and a shared present is shown.
	 */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	model.window[GHCI_REG_DEVICE_CAPS / 4U] &= ~GHCI_CAP_BLT;
	error = start_device();
	if (error == 0)
		error = display_take();
	expect(error == 0 && device.display.frames[0] == NULL && device.display.frame_maps[0] == NULL &&
	    device.display.surfaces[0] != NULL, "display copied: no blitter, no frames");
	expect(display_routes() == GPU_SCANOUT_SHARED, "display copied: no blitter, the shared route only");
	storage = display_storage(2560U * 480U, 0);
	display_pattern(storage, 17U);
	copies = model.command_count;
	error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == ENOTSUP && model.command_count == copies && model_count_commands(GHCI_OP_COPY_BUFFER) == 0U &&
	    device.display.sequence == 0U && device.window_users == 0U &&
	    model.window[(MODEL_MEM_BLOCK + GHCI_MEM_WINDOW_CTRL) / 4U] == 0U, "display copied: no blitter, a copied present refused");
	device.ops.resource_destroy(&device, display_open, storage);
	storage = display_storage(2560U * 480U, 1);
	error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0 && sequence == 1U && dc_read(GHCI_DC_FB_BASE) == (uint32_t)storage->allocation->bus,
	    "display copied: no blitter, a shared present shown");
	device.ops.resource_destroy(&device, display_open, storage);
	display_give_up();
	end_scene("display copied: no blitter, no executor");
}

/*
 * The cursor plane: an image set (converted from RGBA8 to BGRA8, the rest
 * cleared) and flipped to plane 1, moved and hidden by the host; on a
 * discrete GPU the surfaces are host memory and an image in local memory
 * is refused; a controller without the plane, or a GPU without
 * HW_CURSOR, offers none.
 */
static void
scene_display_cursor(void)
{
	struct gpu_display_cursor_image image;
	struct gpu_display_cursor_move move;
	struct gpu_display_cursor_hide hide;
	struct gpu_display_info info;
	struct ghci_resource *frame;
	struct ghci_resource *storage;
	struct ghci_resource local_storage;
	uint8_t *surface;
	uint8_t *pixels;
	uint64_t sequence;
	int error;

	/* A lit display, and a 32 x 16 image of RGBA8. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	frame = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(frame, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0, "display cursor: lit");
	storage = display_storage(4096U, 0);
	pixels = storage->allocation->view;
	pixels[0] = 0x11U;
	pixels[1] = 0x22U;
	pixels[2] = 0x33U;
	pixels[3] = 0x44U;

	/* A move before an image is refused. */
	memset(&move, 0, sizeof(move));
	move.lease = display_lease;
	move.generation = display_generation;
	move.x = -3;
	move.y = 10;
	error = device.ops.display->cursor_move(&device, display_open, &move);
	expect(error == EINVAL, "display cursor: move before an image");

	/* The image goes into a surface and is flipped to plane 1. */
	memset(&image, 0, sizeof(image));
	image.lease = display_lease;
	image.generation = display_generation;
	image.width = 32U;
	image.height = 16U;
	image.stride = 128U;
	image.format = GPU_PIXEL_RGBA8888;
	error = device.ops.display->cursor_set(&device, display_open, storage, &image);
	ghci_sleep_ms(30U);
	display_round();
	surface = model_dma(device.display.surfaces[0]->bus, 256U * 64U);
	expect(error == 0 && surface != NULL && surface[0] == 0x33U && surface[2] == 0x11U && surface[3] == 0x44U &&
	    surface[16U * 256U] == 0U && dc_read(GHCI_DC_CURSOR_BASE) == (uint32_t)device.display.surfaces[0]->bus &&
	    device.display.cursor_shown == 0 && model.display.takes[1] == 1U, "display cursor: set and flipped");

	/* The host moves and hides it. */
	error = device.ops.display->cursor_move(&device, display_open, &move);
	expect(error == 0 && dc_read(GHCI_DC_CURSOR_POS) == ((uint32_t)(-3) & 0xffffU) + (10U << 16) &&
	    dc_read(GHCI_DC_CURSOR_CTRL) == 1U, "display cursor: moved");

	/* A place past the 16 signed bits is held at their ends, not cut. */
	move.x = -40000;
	move.y = 70000;
	error = device.ops.display->cursor_move(&device, display_open, &move);
	expect(error == 0 && dc_read(GHCI_DC_CURSOR_POS) == (0x8000U | (0x7fffU << 16)), "display cursor: held to the 16 bits");
	move.x = -3;
	move.y = 10;
	memset(&hide, 0, sizeof(hide));
	hide.lease = display_lease;
	hide.generation = display_generation;
	error = device.ops.display->cursor_hide(&device, display_open, &hide);
	expect(error == 0 && dc_read(GHCI_DC_CURSOR_CTRL) == 0U, "display cursor: hidden");

	/* The next image goes into the other surface. */
	error = device.ops.display->cursor_set(&device, display_open, storage, &image);
	ghci_sleep_ms(30U);
	display_round();
	expect(error == 0 && device.display.cursor_shown == 1 && model.display.bad_writes == 0U, "display cursor: the other surface");
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.resource_destroy(&device, display_open, frame);
	display_give_up();
	end_scene("display cursor");

	/*
	 * A discrete GPU: the frames are local memory, the cursor's surfaces
	 * host memory the processor writes and the controller reads; an
	 * image in local memory is refused, as the processor never reads it.
	 */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	error = start_device();
	if (error == 0)
		error = display_take();
	frame = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(frame, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0 && device.display.frames[0]->kind == GHCI_MEMORY_LOCAL &&
	    device.display.surfaces[0]->kind == GHCI_MEMORY_HOST && device.display.surfaces[1]->kind == GHCI_MEMORY_HOST,
	    "display cursor: discrete, local frames and host surfaces");
	storage = display_storage(4096U, 0);
	pixels = storage->allocation->view;
	pixels[0] = 0x11U;
	pixels[1] = 0x22U;
	pixels[2] = 0x33U;
	pixels[3] = 0x44U;
	image.lease = display_lease;
	image.generation = display_generation;
	error = device.ops.display->cursor_set(&device, display_open, storage, &image);
	ghci_sleep_ms(30U);
	display_round();
	surface = model_dma(device.display.surfaces[0]->bus, 256U * 64U);
	expect(error == 0 && surface != NULL && surface[0] == 0x33U && surface[2] == 0x11U && surface[3] == 0x44U &&
	    dc_read(GHCI_DC_CURSOR_BASE) == (uint32_t)device.display.surfaces[0]->bus && device.window_users == 0U,
	    "display cursor: discrete, set in host memory and flipped");
	memset(&local_storage, 0, sizeof(local_storage));
	local_storage.allocation = local_memory(4096U);
	error = local_storage.allocation != NULL ? device.ops.display->cursor_set(&device, display_open, &local_storage, &image) : 0;
	expect(error == ENOTSUP && device.window_users == 0U, "display cursor: discrete, an image in local memory refused");
	if (local_storage.allocation != NULL)
		memory_done(local_storage.allocation);
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.resource_destroy(&device, display_open, frame);
	display_give_up();
	end_scene("display cursor: discrete");

	/* A controller without the plane. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_CAPS) / 4U] &= ~GHCI_DC_CAPS_CURSOR;
	error = start_device();
	memset(&info, 0, sizeof(info));
	if (error == 0)
		error = device.ops.open(&device, &display_open);
	if (error == 0)
		error = device.ops.display->query(&device, display_open, &info);
	expect(error == 0 && device.ops.display == &ghci_display_ops && (info.flags & GPU_DISPLAY_HAS_CURSOR) == 0U,
	    "display cursor: none without the plane");
	device.ops.close(&device, display_open);
	end_scene("display cursor: no plane");

	/* A GPU without HW_CURSOR. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	model.window[GHCI_REG_EXT_CAP_FEATURE / 4U] &= ~GHCI_FEATURE_HW_CURSOR;
	error = start_device();
	expect(error == 0 && device.ops.display == &ghci_display_ops && !device.display.cursor, "display cursor: none without HW_CURSOR");
	end_scene("display cursor: no HW_CURSOR");
}

/*
 * The connection: a sink that goes away is a topology event, and the
 * lease's presents fail until a sink comes back as a new generation.
 */
static void
scene_display_hotplug(void)
{
	struct ghci_resource *storage;
	uint64_t topology;
	uint64_t now;
	uint64_t sequence;
	int error;

	/* A lease. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	expect(error == 0 && storage != NULL, "display hotplug: claimed");
	(void)device.ops.display->events(&device, display_open, &topology);

	/* The sink goes: HOTPLUG, the service reads CONNECTED, the topology moves. */
	model_display_sense(0);
	expect((device.display.irq_events & GHCI_DC_IRQ_HOTPLUG) != 0U && (model.irq_status & GHCI_IRQ_DISPLAY) == 0U &&
	    model.display.irq_status == 0U, "display hotplug: the event acknowledged in order");
	display_round();
	(void)device.ops.display->events(&device, display_open, &now);
	expect(!device.display.connected && now == topology + 1U && device.display.generation == display_generation,
	    "display hotplug: disconnected");
	error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	expect(error == ENXIO, "display hotplug: no present without a sink");

	/* The sink comes back: a new generation; the old one is stale. */
	model_display_sense(1);
	display_round();
	(void)device.ops.display->events(&device, display_open, &now);
	expect(device.display.connected && now == topology + 2U && device.display.generation == display_generation + 1U,
	    "display hotplug: connected again, a new generation");
	error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	expect(error == ESTALE, "display hotplug: the old generation is stale");
	device.ops.resource_destroy(&device, display_open, storage);
	display_give_up();
	end_scene("display hotplug");
}

/*
 * The scan-out errors: counted and cleared, a read error three rounds in
 * a row turns the controller dark, and too many in a second mask the
 * event until the next second.
 */
static void
scene_display_errors(void)
{
	struct ghci_resource *storage;
	uint64_t sequence;
	unsigned index;
	int error;

	/* A lit display. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0, "display errors: lit");

	/* An underflow is counted and cleared. */
	model_display_error(GHCI_DC_ST_UNDERFLOW);
	display_round();
	expect(device.display.underflows == 1U && device.display.errors_total == 1U && (dc_read(GHCI_DC_STATUS) & GHCI_DC_ST_UNDERFLOW) == 0U,
	    "display errors: an underflow counted and cleared");

	/* A read error three rounds in a row turns the controller dark. */
	for (index = 0U; index < 3U; index++) {
		model_display_error(GHCI_DC_ST_READ_ERROR);
		display_round();
	}
	ghci_sleep_ms(10U);
	expect((dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U && !device.display.enabled, "display errors: dark after three read errors");

	/* Too many in a second mask the event; the next second unmasks it. */
	for (index = 0U; index < 32U; index++) {
		model_display_error(GHCI_DC_ST_RECORD_ERROR);
		display_round();
	}
	expect(device.display.error_masked && (dc_read(GHCI_DC_IRQ_ENABLE) & GHCI_DC_IRQ_ERROR) == 0U, "display errors: masked");
	ghci_sleep_ms(1100U);
	display_round();
	expect(!device.display.error_masked && (dc_read(GHCI_DC_IRQ_ENABLE) & GHCI_DC_IRQ_ERROR) != 0U, "display errors: unmasked");
	device.ops.resource_destroy(&device, display_open, storage);
	display_give_up();
	end_scene("display errors");
}

/*
 * A reset of the command processor: a flip lost with the ring is counted
 * again (TAKEN0 does not name it) and placed again; a flip the controller
 * took while the processor was faulted has its record written once the
 * reset leaves the processor idle, and is counted taken; a present made
 * while the gate is closed waits for the recovery.
 */
static void
scene_display_reset(void)
{
	struct ghci_resource *first;
	struct ghci_resource *second;
	uint64_t sequence;
	int error;

	/* A lease and a first image. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	first = display_storage(5120U * 720U, 1);
	second = display_storage(5120U * 720U, 1);
	if (error == 0)
		error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0, "display reset: first image");

	/* A flip the processor never read, lost with the ring, is placed again after the recovery. */
	model_hold(1);
	error = display_present(second, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	expect(error == 0 && device.display.flips[0].placed, "display reset: a flip placed, not read");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 1U, "display reset: recovered");
	error = device.ops.display->adoption_wait(&device, display_open, display_lease, &sequence, NULL);
	expect(error == 0 && dc_read(GHCI_DC_FB_BASE) == (uint32_t)second->allocation->bus, "display reset: the lost flip placed again");

	/* A flip taken while the processor was faulted: its record comes after the reset. */
	error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	expect(error == 0 && model.display.pending[0], "display reset: a flip pending");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	model_display_vsync();
	expect(model.display.due[0] && (dc_read(GHCI_DC_STATUS) & GHCI_DC_ST_RECORD) != 0U, "display reset: the record due while faulted");
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && !device.display.flips[0].placed && device.display.adopted == sequence &&
	    dc_read(GHCI_DC_FB_BASE) == (uint32_t)first->allocation->bus, "display reset: counted taken after the reset");

	expect(model.display.bad_writes == 0U && model.display.flip_waits == 0U, "display reset: no refused write");
	display_give_up();
	end_scene("display reset");

	/* A present while the gate is closed is placed once the recovery ran (a new device: three faults would lose it). */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	first = display_storage(5120U * 720U, 1);
	second = display_storage(5120U * 720U, 1);
	if (error == 0)
		error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	error = display_present(second, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	expect(error == 0 && !device.display.flips[0].placed && device.display.queue_count == 1U, "display reset: recorded, not placed, at the closed gate");
	scene_action = action_recover;
	error = device.ops.display->adoption_wait(&device, display_open, display_lease, &sequence, NULL);
	scene_action = NULL;
	expect(error == 0 && dc_read(GHCI_DC_FB_BASE) == (uint32_t)second->allocation->bus && device.resets == 1U,
	    "display reset: placed after the recovery");
	expect(model.display.bad_writes == 0U && model.display.flip_waits == 0U, "display reset: no refused write at the gate");
	display_give_up();
	end_scene("display reset: gate");
}

/* Sets the boot display a firmware leaves on a UMA device: 1280 x 720 at 8 MiB of host memory, rows of 5120 bytes. */
static void
boot_display_host(void)
{
	boot_display(GHCI_DC_CTRL_ENABLE | GHCI_DC_CTRL_OUTPUT_HDMI, MODEL_HOST_BASE + 0x800000U, 5120U, 720U);
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_FB_SIZE) / 4U] = 1280U | (720U << 16);

	/* The timing of the model's first mode. */
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_H_FP) / 4U] = 110U;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_H_SYNC) / 4U] = 40U;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_H_BP) / 4U] = 220U;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_V_FP) / 4U] = 5U;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_V_SYNC) / 4U] = 5U;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_V_BP) / 4U] = 20U;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_SYNC_POL) / 4U] = 3U;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_PCLK_SEL) / 4U] = 1U;
}

/*
 * The console's leaving of the boot display: after the driver's first
 * flip is taken the console is told once, with no lock held and the
 * device's BARs; the boot range goes back only when it left (0), and is
 * kept on any other result, without BARs, or when the boot image is lit
 * again meanwhile.
 */
static void
scene_display_boot_end(void)
{
	struct ghci_resource *storage;
	uint64_t sequence;
	int error;

	/* Told and left: the range goes back, once, and the detach turns the controller dark. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(GHCI_DC_CTRL_ENABLE, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	boot_end_bars();
	stub.text_end_result = 0;
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	ghci_scan_flush(&device);
	expect(error == 0 && stub.text_end_calls == 0U && device.boot_reserved, "display boot end: not told before the flip is taken");
	error = device.ops.display->adoption_wait(&device, display_open, display_lease, &sequence, NULL);
	ghci_scan_flush(&device);
	expect(error == 0 && stub.text_end_calls == 1U && stub.text_end_held == 0U && stub.text_end_count == 2U &&
	    stub.text_end_ranges[0].physical == 0x80000000ULL && stub.text_end_ranges[0].bytes == 0x1000ULL &&
	    stub.text_end_ranges[1].physical == 0x400000000ULL && stub.text_end_ranges[1].bytes == 0x200000000ULL,
	    "display boot end: told once, unlocked, with the whole BARs");
	expect(!device.display.boot_console && !device.display.boot_kept && !device.boot_reserved && device.boot_count == 0U &&
	    device.window_users == 0U, "display boot end: given back once the console left");
	ghci_scan_flush(&device);
	expect(stub.text_end_calls == 1U, "display boot end: told only once");
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	end_scene("display boot end: left");
	expect((dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U, "display boot end: dark at the detach after the release");

	/* Refused (the board cannot stop, or something stays mapped in the BARs): the range is kept. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(GHCI_DC_CTRL_ENABLE, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	boot_end_bars();
	stub.text_end_result = EBUSY;
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	ghci_scan_flush(&device);
	ghci_scan_flush(&device);
	expect(error == 0 && stub.text_end_calls == 1U && device.display.boot_console && device.display.boot_kept && device.boot_reserved &&
	    device.window_users == 1U, "display boot end: kept when the console stays (once)");
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	end_scene("display boot end: refused");
	expect(dc_read(GHCI_DC_FB_BASE) == (uint32_t)MODEL_LOCAL_BASE + 0x100000U && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U,
	    "display boot end: shown again at the detach when kept");

	/* No BARs: not told, kept. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(GHCI_DC_CTRL_ENABLE, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	device.bar_count = 0U;
	stub.text_end_result = 0;
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	ghci_scan_flush(&device);
	expect(error == 0 && stub.text_end_calls == 0U && device.display.boot_kept && device.boot_reserved, "display boot end: not told without BARs");
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	end_scene("display boot end: no BARs");

	/* The boot image lit again while the console was told: left, but kept until the next own image. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(GHCI_DC_CTRL_ENABLE, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	boot_end_bars();
	stub.text_end_result = 0;
	stub.text_end_hook = boot_end_relight;
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	ghci_scan_flush(&device);
	expect(error == 0 && stub.text_end_calls == 1U && !device.display.boot_console && device.display.boot_kept && device.boot_reserved,
	    "display boot end: kept while the boot image is lit again");
	stub.text_end_hook = NULL;
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	end_scene("display boot end: lit again");
}

/* Stands for the end of a lease that lights the boot image while the console is told. */
static void
boot_end_relight(
	void *argument)
{
	(void)argument;

	/* The controller scans the boot image again. */
	device.display.boot_scanning = 1;
}

/* Gives the device a BAR0 of a page and a BAR2 of 8 GiB, as the bus glue would. */
static void
boot_end_bars(void)
{
	/* The registers' BAR. */
	device.bars[0].physical = 0x80000000ULL;
	device.bars[0].bytes = 0x1000ULL;

	/* The local window's BAR, whole. */
	device.bars[1].physical = 0x400000000ULL;
	device.bars[1].bytes = 0x200000000ULL;
	device.bar_count = 2U;
}

/*
 * The boot display: kept scanning until the driver's first flip is taken,
 * its range of local memory kept while the console may still draw to it
 * and given back otherwise; the detach shows it again while it is kept,
 * and turns the controller dark once it was given back.
 */
static void
scene_display_boot(void)
{
	struct ghci_resource *storage;
	uint64_t sequence;
	int error;

	/* A boot display in local memory, which the console may still draw to. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(GHCI_DC_CTRL_ENABLE, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	expect(error == 0 && device.display.boot_scanning && device.display.boot_kept && device.boot_reserved &&
	    model.display.resets == 0U && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U, "display boot: kept scanning");

	/* The first flip placed but not taken (no frame passed yet): the boot image stays. */
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	expect(error == 0 && device.display.boot_scanning && device.boot_reserved, "display boot: kept until the flip is taken");

	/* Taken: the boot display ends, but the range stays for the console. */
	error = device.ops.display->adoption_wait(&device, display_open, display_lease, &sequence, NULL);
	ghci_scan_flush(&device);
	expect(error == 0 && !device.display.boot_scanning && device.display.boot_kept && device.boot_reserved &&
	    device.window_users == 1U, "display boot: the console's range kept");

	/* The detach shows the boot image again. */
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	end_scene("display boot: kept");
	expect(dc_read(GHCI_DC_FB_BASE) == (uint32_t)MODEL_LOCAL_BASE + 0x100000U && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U &&
	    model.display.bad_writes == 0U, "display boot: shown again at the detach");

	/* Nothing draws to it: the range goes back after the first flip, and the detach turns the controller dark. */
	prepare_scene(MODEL_KC705_DISCRETE, NULL);
	boot_display(GHCI_DC_CTRL_ENABLE, MODEL_LOCAL_BASE + 0x100000U, 4096U, 256U);
	error = start_device();
	device.display.boot_console = 0;
	if (error == 0)
		error = display_take();
	storage = display_storage(2560U * 480U, 0);
	if (error == 0)
		error = display_present(storage, 640U, 480U, 2560U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	ghci_scan_flush(&device);
	expect(error == 0 && !device.display.boot_kept && !device.boot_reserved && device.boot_count == 0U &&
	    device.window_users == 0U, "display boot: given back after the first flip");
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	end_scene("display boot: released");
	expect((dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U, "display boot: dark at the detach after the release");
}

/*
 * The panic console: the host halts the processor, waits for HALTED and
 * writes the controller to show the boot image the console draws on; a
 * device without a boot image has nothing to show.
 */
static void
scene_display_panic(void)
{
	struct ghci_resource *storage;
	uint64_t sequence;
	int error;

	/* A UMA boot display, then the driver's own image. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	boot_display_host();
	error = start_device();
	expect(error == 0 && device.display.boot_scanning && device.display.boot.mode == 0U, "display panic: boot display in the 1280 x 720 mode");
	if (error == 0)
		error = display_take();
	storage = display_storage(5120U * 720U, 0);
	if (error == 0)
		error = display_present(storage, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0 && model_count_commands(GHCI_OP_DISP_SET) == 0U && dc_read(GHCI_DC_FB_BASE) != MODEL_HOST_BASE + 0x800000U,
	    "display panic: the first flip without a new lighting");

	/* The panic: halted, then the boot image by the host. */
	error = ghci_display_panic(&device);
	expect(error == 0 && model.state == MODEL_HALTED && dc_read(GHCI_DC_FB_BASE) == MODEL_HOST_BASE + 0x800000U &&
	    (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U && model.display.bad_writes == 0U, "display panic: the boot image shown");
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	end_scene("display panic");

	/* No boot image, nothing to show. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0 && ghci_display_panic(&device) == ENOENT, "display panic: none without a boot image");
	end_scene("display panic: none");
}

/*
 * The detach of a device whose display shows the driver's image: the
 * controller goes dark, by the ring, with the session's image let go.
 */
static void
scene_display_detach(void)
{
	struct ghci_resource *storage;
	struct ghci_allocation *memory;
	uint64_t sequence;
	int error;

	/* A shared image on the screen; the session closes without a release. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	storage = display_storage(5120U * 720U, 1);
	if (error == 0)
		error = display_present(storage, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	memory = storage->allocation;
	device.ops.resource_destroy(&device, display_open, storage);
	expect(error == 0 && memory->holds == 1U, "display detach: the display holds the image");
	device.ops.close(&device, display_open);
	expect(device.display.ending && device.display.owner == NULL, "display detach: the lease ended with the session");

	/* The detach: dark by the ring, before the processor stops. */
	error = ghci_device_stop(&device);
	expect(error == 0 && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U && !device.display.present &&
	    model.display.bad_writes == 0U && model.display.idle_waits_lit == 0U, "display detach: dark");
	end_scene("display detach");
}

/*
 * A GPU without DISP2 offers no display: no block, DISP alone, and DISP2
 * without the capability bit.
 */
static void
scene_display_none(void)
{
	int error;

	/* No block. */
	error = start_scene(MODEL_MINIMAL, NULL);
	expect(error == 0 && !device.display.present && device.ops.display == NULL && device.ops.scanout == NULL &&
	    (device.ops.capabilities & (GPU_CAP_DISPLAY | GPU_CAP_DISPLAY_EVENTS)) == 0U &&
	    (model.irq_enable & GHCI_IRQ_DISPLAY) == 0U, "display none: no DISP2");
	end_scene("display none");

	/* DISP alone is not served. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	extcap_rename(GHCI_EXTCAP_ID_DISP2, GHCI_EXTCAP_ID_DISP);
	error = start_device();
	expect(error == 0 && !device.display.present && device.ops.display == NULL, "display none: DISP alone");
	end_scene("display none: DISP");

	/* DISP2 without CAP_DISP. */
	prepare_scene(MODEL_KC705_UMA, NULL);
	model.window[GHCI_REG_DEVICE_CAPS / 4U] &= ~GHCI_CAP_DISP;
	error = start_device();
	expect(error == 0 && !device.display.present && device.ops.display == NULL, "display none: no CAP_DISP");
	end_scene("display none: no CAP_DISP");
}

/*
 * Fills the ring of a held processor with the driver's own NOPs until it
 * has no room for even one dword and a fence: the submissions never wait,
 * as each is placed only while the plan says it fits.
 */
static void
ring_fill(void)
{
	static const uint32_t sizes[] = { 64U, 16U, 4U, 1U };
	struct ghci_ticket ticket;
	uint32_t padding;
	uint32_t rptr;
	unsigned index;
	int error;

	/* Each size, as long as it fits with its fence. */
	for (index = 0U; index < 4U; index++) {
		for (;;) {
			rptr = model_read(GHCI_REG_RING_RPTR);
			error = ghci_ring_plan(device.ring.wptr, rptr, device.ring.dwords, sizes[index] + GHCI_DW_FENCE, &padding);
			if (error != 0)
				break;
			error = submit_nops(sizes[index], &ticket);
			if (error != 0)
				return;
		}
	}
}

/* The scene's action: the held processor goes on. */
static void
action_unhold(void)
{
	model_hold(0);
}

/*
 * The service thread never waits for the display: with the ring full and
 * the processor stopped, its round does not place the idle frame's
 * lighting and does not sleep; the hang check then starts the recovery
 * after the hang time, after which the lighting is placed.  A round whose
 * display or device lock is held by another thread skips the display's
 * work instead of waiting.
 */
static void
scene_display_service_nowait(void)
{
	unsigned sleeps;
	int error;

	/* A small ring, filled while the processor is held; the idle frame is not lit yet. */
	test_ring_bytes = 4096U;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0 && !device.display.enabled && device.display.idle_frame != NULL, "display nowait: started dark");
	model_hold(1);
	ring_fill();

	/* The round returns at once: nothing placed, no sleep. */
	sleeps = stub.sleeps;
	display_round();
	expect(stub.sleeps == sleeps && !device.display.flips[0].placed && !device.display.enabled && device.resets == 0U,
	    "display nowait: a full ring is not waited for");

	/* The hang check resets the stopped processor once the hang time passed; the lighting follows. */
	stub.ticks += kern_ms_to_ticks(GHCI_HANG_MS) + 1U;
	display_round();
	expect(device.resets == 1U && device.state == GHCI_STATE_RUNNING, "display nowait: the hang check recovered");
	display_round();
	expect(device.display.enabled && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U &&
	    dc_read(GHCI_DC_FB_BASE) == (uint32_t)device.display.idle_frame->bus, "display nowait: the idle frame lit after the recovery");

	/* The display's lock held elsewhere: the round skips the display and keeps its events. */
	mutex_lock(&device.display_lock);
	device.display.irq_events = GHCI_DC_IRQ_HOTPLUG;
	ghci_display_service(&device, GHCI_WORK_DISPLAY);
	expect(stub.double_locks == 0U && device.display.irq_events == GHCI_DC_IRQ_HOTPLUG, "display nowait: a held display lock skipped");
	mutex_unlock(&device.display_lock);
	display_round();
	expect(device.display.irq_events == 0U, "display nowait: the events taken at the next round");

	/* The device's lock held elsewhere: the round's flush leaves the drops for later. */
	mutex_lock(&device.lock);
	ghci_display_service(&device, GHCI_WORK_DISPLAY);
	mutex_unlock(&device.lock);
	expect(stub.double_locks == 0U && model.display.bad_writes == 0U, "display nowait: a held device lock skipped");
	end_scene("display nowait");
}

/*
 * The gate that does not wait keeps the caller's own locks: a present at
 * a closed gate keeps the display's lock, so two presents during a
 * recovery take two numbers.
 */
static void
scene_display_gate_keeps(void)
{
	struct ghci_resource *first;
	struct ghci_resource *second;
	enum ghci_gate gate;
	uint64_t sequence;
	uint64_t one;
	uint64_t two;
	int error;

	/* A lease and a first image; then a fault the service has not taken. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	first = display_storage(5120U * 720U, 1);
	second = display_storage(5120U * 720U, 1);
	if (error == 0)
		error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	expect(error == 0, "display gate: first image");
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);

	/* The gate without a wait answers RETRY with the display's lock still held, and only the ring's let go. */
	mutex_lock(&device.display_lock);
	gate = ghci_cp_enter(&device, GHCI_HOLD_DISPLAY, 0);
	expect(gate == GHCI_GATE_RETRY && stub_holds("ghci display") && !stub_holds("ghci ring"), "display gate: the caller's lock kept");
	mutex_unlock(&device.display_lock);

	/* Two presents at the closed gate: two numbers, both waiting. */
	one = 0U;
	two = 0U;
	error = display_present(second, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &one);
	if (error == 0)
		error = display_present(first, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &two);
	expect(error == 0 && one == sequence + 1U && two == sequence + 2U && device.display.queue_count == 2U,
	    "display gate: two presents, two numbers");

	/* The recovery, then both shown in turn. */
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	error = device.ops.display->adoption_wait(&device, display_open, display_lease, &two, NULL);
	expect(error == 0 && device.display.adopted == two && dc_read(GHCI_DC_FB_BASE) == (uint32_t)first->allocation->bus,
	    "display gate: both placed after the recovery");
	display_give_up();
	end_scene("display gate");
}

/*
 * The stop gives the fence page back only once the controller's records
 * were written: a record that stays due while bus mastering stays on
 * keeps the page in the quarantine; one the reset lets out does not.
 */
static void
scene_display_stop_records(void)
{
	struct ghci_allocation *page;
	int error;

	/* Bus mastering found on (it stays at the stop); no record due: the page goes back. */
	test_master_found = 1;
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "display stop: started");
	error = ghci_device_stop(&device);
	expect(error == 0 && device.fence_page == NULL && device.quarantined == NULL, "display stop: the page given back");
	end_scene("display stop: settled");

	/* A record that stays due: the page is kept. */
	test_master_found = 1;
	error = start_scene(MODEL_KC705_UMA, NULL);
	page = device.fence_page;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_STATUS) / 4U] |= GHCI_DC_ST_RECORD;
	if (error == 0)
		error = ghci_device_stop(&device);
	expect(error == 0 && device.fence_page == NULL && device.quarantined == page && page != NULL, "display stop: the page kept for a record due");
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_STATUS) / 4U] &= ~GHCI_DC_ST_RECORD;
	ghci_memory_release_quarantine(&device);
	end_scene("display stop: kept");

	/* A record due but bus mastering turned off by the driver: nothing reaches the page, which goes back. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_STATUS) / 4U] |= GHCI_DC_ST_RECORD;
	if (error == 0)
		error = ghci_device_stop(&device);
	expect(error == 0 && device.fence_page == NULL && device.quarantined == NULL, "display stop: given back without bus mastering");
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_STATUS) / 4U] &= ~GHCI_DC_ST_RECORD;
	end_scene("display stop: master off");
}

/*
 * A turn by the host that did not see the processor halt lets the image
 * the controller scanned go through the quarantine, never back to the
 * allocator.
 */
static void
scene_display_host_unsure(void)
{
	struct model_switches switches;
	struct ghci_resource *storage;
	struct ghci_allocation *memory;
	uint64_t sequence;
	int error;

	/* A processor whose halt never shows; a shared image on the screen, held by the display alone. */
	memset(&switches, 0, sizeof(switches));
	switches.halt_reads = 100000000U;
	error = start_scene(MODEL_KC705_UMA, &switches);
	if (error == 0)
		error = display_take();
	storage = display_storage(5120U * 720U, 1);
	if (error == 0)
		error = display_present(storage, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	memory = storage->allocation;
	device.ops.resource_destroy(&device, display_open, storage);
	expect(error == 0 && memory->holds == 1U, "display unsure: held by the display");

	/* The processor stops moving; the lease ends; the ring's dark packet never runs. */
	model_hold(1);
	device.ops.close(&device, display_open);
	display_open = NULL;
	ghci_display_detach(&device);
	expect((dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U && device.display.shown.memory == NULL &&
	    device.quarantined == memory && memory->scan_unsure, "display unsure: the image kept in the quarantine");
	end_scene("display unsure");
}

/*
 * The detach after a lease's end whose dark packet the service placed:
 * the packet's ticket comes before the image it turned off is let go of.
 */
static void
scene_display_detach_after_end(void)
{
	struct ghci_resource *storage;
	uint64_t sequence;
	int error;

	/* A shared image on the screen; the session closes; the service places the dark packet, not yet run. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	storage = display_storage(5120U * 720U, 1);
	if (error == 0)
		error = display_present(storage, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	device.ops.resource_destroy(&device, display_open, storage);
	model_hold(1);
	device.ops.close(&device, display_open);
	display_open = NULL;
	display_round();
	expect(error == 0 && device.display.ending && device.display.dark_placed == 1 && device.display.shown.memory != NULL,
	    "display detach after end: the dark packet placed");

	/* The detach waits for its ticket, then lets the image go (not through the quarantine). */
	model_hold(0);
	ghci_display_detach(&device);
	expect(ghci_cp_ticket(&device, &device.display.dark_ticket) == GHCI_TICKET_DONE && device.display.shown.memory == NULL &&
	    device.quarantined == NULL && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U, "display detach after end: the image let go after the ticket");
	end_scene("display detach after end");
}

/*
 * A lighting cut by a reset after its flip was taken but before ENABLE:
 * the reset's count finds the controller dark, puts the image back in
 * the queue and the controller is lit again.
 */
static void
scene_display_light_cut(void)
{
	struct ghci_resource *storage;
	uint64_t sequence;
	uint32_t value;
	int error;

	/* The first present of a lease lights the controller; the held processor has not run the run. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	storage = display_storage(5120U * 720U, 1);
	model_hold(1);
	if (error == 0)
		error = display_present(storage, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 0, &sequence);
	expect(error == 0 && device.display.flips[0].placed && device.display.flips[0].light, "display light cut: the lighting placed");

	/* As if the run stopped after the flip: TAKEN0 names it, ENABLE stayed off; then a fault and the recovery's round. */
	value = device.display.flips[0].value;
	model.window[(MODEL_DISP2_BLOCK + GHCI_DC_TAKEN0) / 4U] = value;
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 1U, "display light cut: recovered");

	/* The image went back to the queue, and the service's round after the recovery lit the controller on it. */
	display_round();
	expect(device.display.enabled && (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) != 0U &&
	    dc_read(GHCI_DC_FB_BASE) == (uint32_t)storage->allocation->bus && storage->allocation->holds == 2U &&
	    device.display.adopted == sequence, "display light cut: lit again");
	display_give_up();
	end_scene("display light cut");
}

/* The frames the driver takes fit every mode: the most bytes over the modes, not the mode with the most pixels. */
static void
scene_display_frame_bytes(void)
{
	static struct ghci_display table;

	/* 1024 x 1000 has the most pixels; 1025 x 999 has rows of 4160 bytes and the most bytes. */
	memset(&table, 0, sizeof(table));
	table.mode_count = 2U;
	table.modes[0].width = 1024U;
	table.modes[0].height = 1000U;
	table.modes[1].width = 1025U;
	table.modes[1].height = 999U;
	expect(ghci_scan_largest_mode(&table) == 0U && ghci_scan_frame_bytes(&table) == 4160ULL * 999U, "display frame bytes: every mode fits");
}

/*
 * The release returns once the lease's memory is no longer scanned: a
 * processor that runs the dark packet late is waited for, past a second.
 */
static void
scene_display_release_wait(void)
{
	struct gpu_display_release release;
	struct ghci_resource *storage;
	struct ghci_allocation *memory;
	uint64_t sequence;
	int error;

	/* A shared image on the screen, held by the session and the display. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	if (error == 0)
		error = display_take();
	storage = display_storage(5120U * 720U, 1);
	if (error == 0)
		error = display_present(storage, 1280U, 720U, 5120U, 0U, GPU_DISPLAY_PRESENT_BLOB | GPU_DISPLAY_PRESENT_FIFO, 1, &sequence);
	memory = storage->allocation;
	expect(error == 0 && memory->holds == 2U, "display release wait: shown");

	/* The processor runs again only after more than a second of the release's sleeps. */
	model_hold(1);
	scene_action = action_unhold;
	scene_action_after = stub.sleeps + (unsigned)kern_ms_to_ticks(1500U);
	memset(&release, 0, sizeof(release));
	release.lease = display_lease;
	error = device.ops.display->release(&device, display_open, &release);
	scene_action = NULL;
	scene_action_after = 0U;
	expect(error == 0 && !device.display.ending && device.display.shown.memory == NULL && memory->holds == 1U &&
	    (dc_read(GHCI_DC_CTRL) & GHCI_DC_CTRL_ENABLE) == 0U, "display release wait: returned once the image was let go");
	device.ops.resource_destroy(&device, display_open, storage);
	device.ops.close(&device, display_open);
	display_open = NULL;
	end_scene("display release wait");
}

/*
 * The recovery waits for MEM_READY after the reset before it sets the
 * device up again: a memory late to be ready is waited for, one that
 * never is loses the device.
 */
static void
scene_reset_mem_ready(void)
{
	int error;

	/* A device fault with the memory ready only after 20 reads. */
	error = start_scene(MODEL_KC705_UMA, NULL);
	expect(error == 0, "reset memory: started");
	model.switches.mem_ready_reads = 20U;
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_RUNNING && device.resets == 1U && model.mem_ready_reads_left == 0U,
	    "reset memory: late memory waited for");

	/* A memory that never becomes ready is a reset that did not end. */
	stub.ticks += kern_ms_to_ticks(GHCI_FAULT_STORM_MS + 1000U);
	model.switches.mem_ready_reads = 1000000U;
	model_inject(GHCI_FAULT_KIND_AXI_ERROR, GHCI_UNIT_CP, 0U, 0x1000U, 0);
	ghci_service_step(&device, GHCI_WORK_RECOVER);
	expect(device.state == GHCI_STATE_LOST, "reset memory: never ready loses the device");
	model.switches.mem_ready_reads = 0U;
	end_scene("reset memory");
}
