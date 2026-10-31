/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Mali command stream front end: loading the firmware into the
 * microcontroller's address space, starting the microcontroller, and the
 * global interface the host talks to it through.
 *
 * Every section of the firmware gets memory of its own, filled from the
 * file through the memory's uncached view and mapped at the section's
 * address with the access its flags give.  The microcontroller is then
 * let run; it announces that it has started by raising the job block's
 * global interface bit, which the driver polls for, so a wrong interrupt
 * line does not hide the start.  Every later exchange is a request the
 * host flips and the firmware acknowledges, also polled.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-private.h"
#include "drivers/gpu/mali/mali-regs.h"

/* How long a register poll sleeps before the next read, in milliseconds (a tick at least). */
#define MALI_CSF_POLL_MS 1U

/*
 * How long the microcontroller has to announce its start (1 s), and to
 * answer a request (100 ms).
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c (panthor_fw_start:
 * 1000 ms; panthor_fw_ping_work and the scheduler's acknowledgement
 * waits: 100 ms).
 */
#define START_WAIT_MS			1000U
#define ANSWER_WAIT_MS			100U

/*
 * The firmware's timers: cores power off after 10 ms idle, the firmware
 * counts as idle after 800 µs, and a stream that makes no progress for
 * this many units of 1024 cycles is stopped.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (PWROFF_HYSTERESIS_US, IDLE_HYSTERESIS_US, PROGRESS_TIMEOUT_CYCLES,
 * PROGRESS_TIMEOUT_SCALE_SHIFT).
 */
#define POWEROFF_US			10000U
#define IDLE_US				800U
#define PROGRESS_UNITS			((5ULL * 500U * 1024U * 1024U) >> 10)

/*
 * The interface version from which the global block reports its state
 * in the request and acknowledge words.
 *
 * Value referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (panthor_fw_has_glb_state: interface 4.1.0 on).
 */
#define IFACE_STATE_VERSION		0x04010000U

/* The global configuration requests made at start, acknowledged together. */
#define GLOBAL_CONFIGURATION		(MALI_GLB_ALLOCATE | MALI_GLB_POWEROFF_TIMER | MALI_GLB_PROGRESS_TIMER)

static int load_section(struct mali_device *device, const uint8_t *bytes, unsigned index);
static unsigned section_access(uint32_t flags);
static void place_driver_objects(struct mali_device *device);
static void configure_global(struct mali_device *device);
static int wait_acknowledged(const volatile uint8_t *input, uint32_t request_at, const volatile uint8_t *output, uint32_t acknowledge_at, uint32_t mask, unsigned milliseconds);
static void report_start_failure(struct mali_device *device);
static const char *mcu_status_name(uint32_t status);

/*
 * Loads every section of a firmware file into the microcontroller's
 * address space, and binds the space to its slot.
 *
 * The file's bytes are only read; the caller may free them afterwards.
 */
int
mali_csf_load(
	struct mali_device *device,
	const uint8_t *bytes,
	size_t size)
{
	unsigned index;
	int error;

	/* Reads and checks the file's header and entries. */
	error = mali_fw_image_parse(bytes, size, (uint32_t)MALI_PAGE_BYTES, &device->image);
	if (error != 0) {
		kern_logf("mali: firmware file not usable (%d)\n", error);
		return error;
	}

	/* The microcontroller's space, bound to slot 0. */
	error = mali_as_init(device, &device->mcu, MALI_MCU_ADDRESS_SPACE);
	if (error != 0) {
		kern_logf("mali: firmware space not made (%d)\n", error);
		return error;
	}

	/* Every section, at its own address. */
	for (index = 0U; index < device->image.section_count; index++) {
		error = load_section(device, bytes, index);
		if (error != 0) {
			kern_logf("mali: firmware section %u \"%s\" not loaded (%d)\n",
				  index,
				  device->image.sections[index].name,
				  error);
			return error;
		}
	}

	/* The shared section is the one the host talks through. */
	device->shared = &device->sections[device->image.shared];
	place_driver_objects(device);

	/* The microcontroller translates through the space from now on. */
	error = mali_as_enable(device, &device->mcu);
	if (error != 0)
		return error;

	/* Says what was loaded. */
	kern_logf("mali: firmware header %u.%u, version hash %08x, %u sections (%u protected skipped)\n",
		  (unsigned)device->image.major,
		  (unsigned)device->image.minor,
		  (unsigned)device->image.version_hash,
		  device->image.section_count,
		  device->image.skipped);

	/* Succeeded: the firmware is in place. */
	return 0;
}

/*
 * Starts the microcontroller, reads the interface it published, and
 * configures the global block.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c (panthor_fw_start,
 * panthor_fw_init_ifaces, panthor_fw_init_global_iface).
 */
int
mali_csf_start(
	struct mali_device *device)
{
	struct mali_interfaces *interfaces;
	uint32_t version;
	int error;

	/* Forgets an old announcement, then lets the microcontroller run, restarting it when it halts. */
	mali_write32(device, MALI_JOB_INT_BASE + MALI_INT_CLEAR, 0xffffffffU);
	mali_write32(device, MALI_REG_MCU_CONTROL, MALI_MCU_AUTO);

	/* Waits for the firmware to announce its start on the global interface bit. */
	error = mali_gpu_wait_raw(device, MALI_JOB_INT_BASE, MALI_JOB_IRQ_GLOBAL, START_WAIT_MS);
	if (error != 0) {
		report_start_failure(device);
		return error;
	}

	mali_write32(device, MALI_JOB_INT_BASE + MALI_INT_CLEAR, MALI_JOB_IRQ_GLOBAL);

	/* Reads the interface out of the shared section. */
	interfaces = &device->interfaces;
	error = mali_iface_read(device->shared->view, (uint32_t)device->shared->gpu_address, (uint32_t)device->shared->bytes, interfaces);
	if (error != 0) {
		kern_logf("mali: firmware interface not usable (%d, version %08x, groups %u)\n",
			  error,
			  (unsigned)mali_iface_read32(device->shared->view, MALI_GLB_CONTROL_VERSION),
			  (unsigned)mali_iface_read32(device->shared->view, MALI_GLB_CONTROL_GROUPS));
		return error;
	}

	/* Says what the firmware offers. */
	version = interfaces->version;
	kern_logf("mali: firmware interface v%u.%u.%u, features %08x, %u groups of %u streams (%u work registers), suspend %u bytes\n",
		  (unsigned)(version >> MALI_IFACE_MAJOR_SHIFT),
		  (unsigned)((version >> MALI_IFACE_MINOR_SHIFT) & MALI_IFACE_BYTE_MASK),
		  (unsigned)(version & MALI_IFACE_PATCH_MASK),
		  (unsigned)interfaces->features,
		  (unsigned)interfaces->group_count,
		  (unsigned)interfaces->groups[0].stream_count,
		  (unsigned)((interfaces->groups[0].streams[0].features & MALI_CS_FEATURES_REGISTERS_MASK) + 1U),
		  (unsigned)interfaces->groups[0].suspend_bytes);

	/* The cores, the timers and the acknowledgements the host wants. */
	configure_global(device);

	/* Waits for the firmware to take the configuration. */
	error = mali_csf_wait_global(device, GLOBAL_CONFIGURATION, ANSWER_WAIT_MS);
	if (error != 0) {
		kern_logf("mali: firmware did not take its configuration (request %08x, acknowledge %08x)\n",
			  (unsigned)mali_iface_read32(interfaces->input, MALI_GLB_INPUT_REQUEST),
			  (unsigned)mali_iface_read32(interfaces->output, MALI_GLB_OUTPUT_ACK));
		return error;
	}

	/* The job block's interrupts tell the host of later answers. */
	mali_write32(device, MALI_JOB_INT_BASE + MALI_INT_MASK, 0xffffffffU);
	device->firmware_running = 1;
	device->mcu_status = mali_read32(device, MALI_REG_MCU_STATUS);

	/* Succeeded: the firmware runs and is configured. */
	return 0;
}

/*
 * Asks the firmware to answer a ping, which also tells its watchdog the
 * host is alive.
 */
int
mali_csf_ping(
	struct mali_device *device)
{
	struct mali_interfaces *interfaces;
	uint32_t request;
	uint32_t acknowledge;
	int error;

	/* Only a running firmware answers. */
	interfaces = &device->interfaces;
	if (!device->firmware_running)
		return ENXIO;

	/* Flips the ping request away from the last answer and rings the global doorbell. */
	request = mali_iface_read32(interfaces->input, MALI_GLB_INPUT_REQUEST);
	acknowledge = mali_iface_read32(interfaces->output, MALI_GLB_OUTPUT_ACK);
	mali_iface_write32(interfaces->input, MALI_GLB_INPUT_REQUEST, mali_iface_toggle(request, acknowledge, MALI_GLB_PING));
	mali_csf_ring_doorbell(device, MALI_DOORBELL_GLOBAL);

	/* Waits for the answer. */
	error = mali_csf_wait_global(device, MALI_GLB_PING, ANSWER_WAIT_MS);
	if (error != 0) {
		kern_logf("mali: firmware did not answer a ping (MCU %s, halt status %08x)\n",
			  mcu_status_name(mali_read32(device, MALI_REG_MCU_STATUS)),
			  (unsigned)mali_iface_read32(interfaces->output, MALI_GLB_OUTPUT_HALT_STATUS));
		return error;
	}

	/* Succeeded: the firmware answered. */
	return 0;
}

/*
 * Rings one doorbell, after everything written to the shared memory
 * before it.
 */
void
mali_csf_ring_doorbell(
	struct mali_device *device,
	unsigned doorbell)
{
	/* The firmware reads the shared memory once the doorbell rings, so the writes go first. */
	kern_io_write_barrier();
	mali_write32(device, MALI_DOORBELL_BASE + doorbell * MALI_DOORBELL_STRIDE, 1U);
}

/*
 * Rings the doorbells of a set of group slots: their bits in the global
 * doorbell request are flipped, and the global doorbell rings.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (panthor_fw_ring_csg_doorbells).
 */
void
mali_csf_ring_groups(
	struct mali_device *device,
	uint32_t groups)
{
	struct mali_interfaces *interfaces;
	uint32_t request;
	uint32_t acknowledge;

	/* Flips the slots' bits away from the firmware's last answer. */
	interfaces = &device->interfaces;
	request = mali_iface_read32(interfaces->input, MALI_GLB_INPUT_DOORBELL);
	acknowledge = mali_iface_read32(interfaces->output, MALI_GLB_OUTPUT_DOORBELL_ACK);
	mali_iface_write32(interfaces->input, MALI_GLB_INPUT_DOORBELL, mali_iface_toggle(request, acknowledge, groups));

	/* The global doorbell makes the firmware look. */
	mali_csf_ring_doorbell(device, MALI_DOORBELL_GLOBAL);
}

/*
 * Waits for the firmware to acknowledge the global requests of a mask.
 */
int
mali_csf_wait_global(
	struct mali_device *device,
	uint32_t mask,
	unsigned milliseconds)
{
	struct mali_interfaces *interfaces;
	int error;

	/* The global request word against its acknowledge word. */
	interfaces = &device->interfaces;
	error = wait_acknowledged(interfaces->input, MALI_GLB_INPUT_REQUEST, interfaces->output, MALI_GLB_OUTPUT_ACK, mask, milliseconds);
	if (error != 0)
		return error;

	/* Succeeded: the requests are answered. */
	return 0;
}

/*
 * Waits for the firmware to acknowledge the requests of a mask on one
 * group slot.
 */
int
mali_csf_wait_group(
	struct mali_device *device,
	unsigned slot,
	uint32_t mask,
	unsigned milliseconds)
{
	struct mali_group_interface *group;
	int error;

	/* Refuses a slot the firmware does not offer. */
	if (slot >= device->interfaces.group_count)
		return EINVAL;

	/* The slot's request word against its acknowledge word. */
	group = &device->interfaces.groups[slot];
	error = wait_acknowledged(group->input, MALI_CSG_INPUT_REQUEST, group->output, MALI_CSG_OUTPUT_ACK, mask, milliseconds);
	if (error != 0)
		return error;

	/* Succeeded: the requests are answered. */
	return 0;
}

/*
 * Writes the microcontroller's status to the log when it changed since
 * the last report, with the firmware's halt status.
 */
void
mali_csf_report_status(
	struct mali_device *device)
{
	uint32_t status;
	uint32_t halt;

	/* Only a change is worth a line. */
	status = mali_read32(device, MALI_REG_MCU_STATUS);
	if (status == device->mcu_status)
		return;

	/* The new status, and why the firmware stopped when it did. */
	halt = 0U;
	if (device->firmware_running)
		halt = mali_iface_read32(device->interfaces.output, MALI_GLB_OUTPUT_HALT_STATUS);

	kern_logf("mali: MCU %s (was %s), halt status %08x\n",
		  mcu_status_name(status),
		  mcu_status_name(device->mcu_status),
		  (unsigned)halt);
	device->mcu_status = status;
}

/* Gives one section memory, fills it from the file and maps it at its address. */
static int
load_section(
	struct mali_device *device,
	const uint8_t *bytes,
	unsigned index)
{
	const struct mali_fw_section *section;
	struct mali_buffer *buffer;
	uint32_t length;
	int error;

	/* The section's whole span; the parser checked it is page aligned. */
	section = &device->image.sections[index];
	buffer = &device->sections[index];
	error = mali_buffer_alloc(buffer, (uint64_t)section->va_end - section->va_start);
	if (error != 0)
		return error;

	/* The file's bytes first; the rest of new memory is already zero, as a ZERO section asks. */
	length = section->data_end - section->data_start;
	kern_memcpy((void *)(uintptr_t)buffer->view, bytes + section->data_start, length);

	/* The section where the firmware was linked to find it. */
	error = mali_as_map(device, &device->mcu, section->va_start, buffer, section_access(section->flags));
	if (error != 0) {
		(void)mali_pt_unmap(&device->mcu.tables, section->va_start, buffer->bytes);
		mali_buffer_free(buffer);
		return error;
	}

	/* Succeeded: the section is in place. */
	return 0;
}

/*
 * Turns a section's flags into the access its mapping gives: written
 * when writable, run when executable, and cached only when the section
 * asks for the plain cached mode.  A coherent mode is mapped uncached,
 * since the GPU does not snoop the processor here.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (panthor_fw_load_section_entry: the mapping flags of a section).
 */
static unsigned
section_access(
	uint32_t flags)
{
	unsigned access;

	/* Write and execute follow the flags of the same names. */
	access = 0U;
	if ((flags & MALI_FW_SECTION_WRITE) != 0U)
		access |= MALI_PT_WRITE;
	if ((flags & MALI_FW_SECTION_EXECUTE) != 0U)
		access |= MALI_PT_EXECUTE;

	/* Only the plain cached mode is cached. */
	if ((flags & MALI_FW_SECTION_CACHE_MASK) == MALI_FW_SECTION_CACHED)
		access |= MALI_PT_CACHED;

	/* Succeeded: the mapping's access. */
	return access;
}

/*
 * Sets where the driver's own objects start in the microcontroller's
 * space: past every firmware section that lies in the shared region.
 */
static void
place_driver_objects(
	struct mali_device *device)
{
	const struct mali_fw_section *section;
	uint64_t next;
	unsigned index;

	/* The first page past the last section of the region. */
	next = MALI_FW_SHARED_START;
	for (index = 0U; index < device->image.section_count; index++) {
		section = &device->image.sections[index];
		if (section->va_end <= MALI_FW_SHARED_START || section->va_start >= MALI_MCU_KERNEL_END)
			continue;
		if (section->va_end > next)
			next = section->va_end;
	}

	device->mcu_next = next;
}

/*
 * Writes the global block's configuration and asks the firmware to take
 * it: every shader core, the three timers, the acknowledgements that
 * raise the job interrupt, idle reports and the cycle counter.
 *
 * Procedure referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c
 * (panthor_fw_init_global_iface).
 */
static void
configure_global(
	struct mali_device *device)
{
	struct mali_interfaces *interfaces;
	uint32_t acknowledgements;
	uint32_t request;
	uint32_t acknowledge;

	/* Every shader core the GPU has. */
	interfaces = &device->interfaces;
	mali_iface_write64(interfaces->input, MALI_GLB_INPUT_CORE_MASK, device->shader_present);

	/* The timers, counted on the GPU's own clock. */
	mali_iface_write32(interfaces->input, MALI_GLB_INPUT_POWEROFF_TIMER, mali_iface_timer(POWEROFF_US, device->core_rate));
	mali_iface_write32(interfaces->input, MALI_GLB_INPUT_PROGRESS_TIMER, (uint32_t)PROGRESS_UNITS);
	mali_iface_write32(interfaces->input, MALI_GLB_INPUT_IDLE_TIMER, mali_iface_timer(IDLE_US, device->core_rate));

	/* The answers that raise the job interrupt; the state ones only where the firmware reports them. */
	acknowledgements = MALI_GLB_ALLOCATE | MALI_GLB_PING | MALI_GLB_PROGRESS_TIMER;
	acknowledgements |= MALI_GLB_POWEROFF_TIMER | MALI_GLB_IDLE_ENABLE | MALI_GLB_IDLE;
	if (interfaces->version >= IFACE_STATE_VERSION)
		acknowledgements |= MALI_GLB_STATE_MASK;
	mali_iface_write32(interfaces->input, MALI_GLB_INPUT_ACK_IRQ_MASK, acknowledgements);

	/*
	 * Idle reports and the cycle counter are levels; the three
	 * configurations are requests flipped away from the last answer.
	 */
	request = mali_iface_read32(interfaces->input, MALI_GLB_INPUT_REQUEST);
	acknowledge = mali_iface_read32(interfaces->output, MALI_GLB_OUTPUT_ACK);
	request = mali_iface_update(request, MALI_GLB_IDLE_ENABLE | MALI_GLB_COUNTER, MALI_GLB_IDLE_ENABLE | MALI_GLB_COUNTER);
	request = mali_iface_toggle(request, acknowledge, GLOBAL_CONFIGURATION);
	mali_iface_write32(interfaces->input, MALI_GLB_INPUT_REQUEST, request);

	/* The global doorbell makes the firmware read it. */
	mali_csf_ring_doorbell(device, MALI_DOORBELL_GLOBAL);
}

/* Polls a request word against its acknowledge word until the bits of a mask agree. */
static int
wait_acknowledged(
	const volatile uint8_t *input,
	uint32_t request_at,
	const volatile uint8_t *output,
	uint32_t acknowledge_at,
	uint32_t mask,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	uint32_t request;
	uint32_t acknowledge;
	int acked;

	/* Reads, then sleeps a tick, until the answer is there or the time is spent. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (;;) {
		/* The firmware wrote the shared memory before it raised anything. */
		kern_io_read_barrier();
		request = mali_iface_read32(input, request_at);
		acknowledge = mali_iface_read32(output, acknowledge_at);
		acked = mali_iface_acked(request, acknowledge, mask);
		if (acked)
			return 0;

		/* The time is spent. */
		now = sched_ticks();
		if (now >= deadline)
			break;

		/* A millisecond before the next read. */
		sched_sleep(now + kern_ms_to_ticks(MALI_CSF_POLL_MS));
	}

	/* Succeeded in waiting, but the firmware did not answer. */
	return ETIMEDOUT;
}

/* Writes what the GPU shows when the microcontroller did not announce its start. */
static void
report_start_failure(
	struct mali_device *device)
{
	uint32_t base;

	/* The microcontroller's own status and what it published. */
	kern_logf("mali: MCU did not start (status %s, job raw %08x, interface version %08x)\n",
		  mcu_status_name(mali_read32(device, MALI_REG_MCU_STATUS)),
		  (unsigned)mali_read32(device, MALI_JOB_INT_BASE + MALI_INT_RAWSTAT),
		  (unsigned)mali_iface_read32(device->shared->view, MALI_GLB_CONTROL_VERSION));

	/* The GPU's own fault, and the microcontroller's space's fault. */
	base = MALI_AS_BASE + MALI_MCU_ADDRESS_SPACE * MALI_AS_STRIDE;
	kern_logf("mali: GPU raw %08x status %08x fault %08x at %016llx; MMU raw %08x, space 0 fault %08x at %016llx\n",
		  (unsigned)mali_read32(device, MALI_GPU_INT_BASE + MALI_INT_RAWSTAT),
		  (unsigned)mali_read32(device, MALI_REG_STATUS),
		  (unsigned)mali_read32(device, MALI_REG_FAULT_STATUS),
		  (unsigned long long)mali_read64(device, MALI_REG_FAULT_ADDRESS),
		  (unsigned)mali_read32(device, MALI_MMU_INT_BASE + MALI_INT_RAWSTAT),
		  (unsigned)mali_read32(device, base + MALI_AS_FAULT_STATUS),
		  (unsigned long long)mali_read64(device, base + MALI_AS_FAULT_ADDRESS));

	/* The cores' power, which the firmware turns on itself. */
	kern_logf("mali: power: shader ready %llx, tiler ready %llx, L2 ready %llx\n",
		  (unsigned long long)mali_read64(device, MALI_REG_SHADER_READY),
		  (unsigned long long)mali_read64(device, MALI_REG_TILER_READY),
		  (unsigned long long)mali_read64(device, MALI_REG_L2_READY));
}

/* Names one microcontroller status. */
static const char *
mcu_status_name(
	uint32_t status)
{
	/* The four statuses the register reports. */
	switch (status) {
	case MALI_MCU_STATUS_DISABLED:
		return "disabled";
	case MALI_MCU_STATUS_ENABLED:
		return "enabled";
	case MALI_MCU_STATUS_HALT:
		return "halted";
	case MALI_MCU_STATUS_FATAL:
		return "fatal";
	default:
		break;
	}

	/* A value the register is not known to report. */
	return "unknown";
}
