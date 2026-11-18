/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Finding the Mali GPU in the device tree and bringing it up.
 *
 * The early stage runs on the platform's thread: it registers the
 * display node, opens the GPU's clocks and resets through the SoC's
 * glue, maps the registers, reads the identity, takes the interrupt
 * lines, resets the GPU and powers its L2 cache.  The later stage runs
 * on a thread of its own, since the firmware lives on the root file
 * system that is mounted after the devices come up: it loads the
 * firmware, starts the microcontroller, configures the global
 * interface, pings the firmware, starts the driver's own command stream,
 * and from then on writes what the interrupt handlers recorded and pings
 * the firmware now and then.  Every failure is one line of the log, and
 * the boot goes on without the GPU.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/fdt.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/sysctl.h>
#include <kern/thread.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/mali-attach.h"
#include "drivers/gpu/mali/mali-private.h"
#include "drivers/gpu/mali/mali-regs.h"

/* The most of the device tree the driver reads (1 MiB). */
#define FDT_READ_LIMIT		(1024U * 1024U)

/* The first GIC interrupt number of the shared peripheral lines, and the cell that says a line is one. */
#define GIC_SPI_BASE		32U
#define GIC_SPI_TYPE		0U
#define INTERRUPT_CELLS		3U

/*
 * The smallest register window the driver uses (up to the last doorbell
 * it rings), and the most of the window it maps: the control blocks and
 * the first eight doorbells.  The rest of the window holds doorbells the
 * driver never rings.
 */
#define REGISTERS_MIN_BYTES	(MALI_DOORBELL_BASE + 2U * MALI_DOORBELL_STRIDE)
#define REGISTERS_MAP_BYTES	(MALI_DOORBELL_BASE + 8U * MALI_DOORBELL_STRIDE)

/* The largest firmware file the driver reads (16 MiB), the longest path it builds, and where the files are. */
#define FIRMWARE_LIMIT		(16U * 1024U * 1024U)
#define FIRMWARE_PATH_BYTES	64U
#define FIRMWARE_DIRECTORY	"/lib/firmware/arm/mali"
#define FIRMWARE_NAME		"mali_csffw.bin"

/*
 * The minor architectures of v10 whose firmware is published, tried in
 * turn when the GPU's own has no file.  The three published files are
 * one and the same image.
 *
 * Values referenced:
 * Linux v7.3 drivers/gpu/drm/panthor/panthor_fw.c (the MODULE_FIRMWARE
 * list of arch10.8, arch10.10 and arch10.12).
 */
static const unsigned firmware_minors[] = {
	12U,
	10U,
	8U,
};

/* How often the later stage writes what it saw (1 s), and how often it pings the firmware (10 s). */
#define REPORT_MS		1000U
#define PING_EVERY_REPORTS	10U

/* How many pings in a row may go unanswered before the later stage stops pinging. */
#define PINGS_UNANSWERED_MAX	3U

/*
 * The board's one GPU.
 *
 * Filled by drv_mali_gpu_attach() on the platform's thread, then owned by
 * the later stage's thread; the interrupt handlers write only its event
 * words.  Nonzero attached means the early stage has run.
 */
static struct mali_device board_gpu;
static int board_gpu_attached;

static int find_gpu(const struct drv_fdt *fdt, uint32_t *node);
static int describe_gpu(const struct drv_fdt *fdt, uint32_t node, struct mali_device *device);
static int read_interrupt(const struct drv_fdt *fdt, uint32_t node, const char *name, unsigned fallback, int *irq);
static int bring_up(struct mali_device *device);
static void report_identity(const struct mali_device *device);
static void late_thread(void *argument);
static int late_bring_up(struct mali_device *device);
static int read_other_minor(const struct mali_device *device, char *path, uint8_t **bytes, size_t *size);
static void watch(struct mali_device *device);

/*
 * Finds the GPU and the display, registers the display node, brings the
 * GPU up to its L2 cache and starts the later stage.
 */
int
drv_mali_gpu_attach(
	uint64_t fdt_phys)
{
	struct mali_device *device;
	struct thread *thread;
	struct drv_fdt fdt;
	const void *blob;
	uint32_t node;
	int error;

	/* Refuses a second attach. */
	device = &board_gpu;
	if (board_gpu_attached)
		return -EALREADY;
	board_gpu_attached = 1;

	/* Opens the device tree through the direct map. */
	blob = kern_pmem_to_kernel((kern_physaddr_t)fdt_phys);
	if (blob == NULL) {
		kern_logf("mali: no device tree at %llx\n", (unsigned long long)fdt_phys);
		return -ENODEV;
	}

	error = drv_fdt_open(&fdt, blob, FDT_READ_LIMIT);
	if (error != 0) {
		kern_logf("mali: device tree not readable (%d)\n", error);
		return -error;
	}

	/* The display node first; it stays whatever becomes of the GPU. */
	(void)mali_display_attach(&fdt);

	/* The GPU's node, its registers and its lines. */
	error = find_gpu(&fdt, &node);
	if (error != 0) {
		kern_logf("mali: no GPU in the device tree (%d)\n", error);
		return -error;
	}

	error = describe_gpu(&fdt, node, device);
	if (error != 0) {
		kern_logf("mali: GPU node not usable (%d)\n", error);
		return -error;
	}

	/* The SoC's clocks and resets around the GPU. */
	error = mali_s7d_glue(&fdt, node, &device->glue);
	if (error != 0) {
		kern_logf("mali: no clock and reset controller for the GPU (%d)\n", error);
		return -error;
	}

	/* Up to the L2 cache, on this thread. */
	error = bring_up(device);
	if (error != 0)
		return -error;

	/* The lock the later stage's thread and the render node share the firmware under. */
	error = mutex_init(&device->lock, LOCK_RANK_DEVICE, "mali-gpu");
	if (error != 0)
		return -error;

	/*
	 * The later stage on a thread of its own; nobody joins it.  Until it
	 * has published the render node or given up, the GPU counts as
	 * attaching (hw.gpu.attaching), so the graphical login waits for the
	 * render node as well as the display; the later stage ends the count.
	 */
	kern_gpu_attach_begin();
	error = kthread_create(late_thread, device, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_gpu_attach_end();
		kern_logf("mali: no thread for the firmware (%d)\n", error);
		return -error;
	}

	thread_start(thread);

	/* Succeeded: the later stage runs on its own from here. */
	return 0;
}

/* Finds the first enabled node of a Mali GPU of the Valhall kind, by its upstream or its vendor name. */
static int
find_gpu(
	const struct drv_fdt *fdt,
	uint32_t *node)
{
	static const char *const names[] = {
		"arm,mali-valhall-csf",
		"arm,mali-midgard",
	};
	unsigned index;
	uint32_t found;
	bool enabled;
	int error;

	/* Each name in turn, each node of the name in turn. */
	for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
		found = DRV_FDT_NO_NODE;
		for (;;) {
			/* The next node of the name. */
			error = drv_fdt_find_compatible(fdt, names[index], found, &found);
			if (error != 0)
				break;

			/* A disabled node is not this board's GPU. */
			enabled = drv_fdt_node_enabled(fdt, found);
			if (enabled) {
				*node = found;
				return 0;
			}
		}
	}

	/* No node names the GPU. */
	return ENODEV;
}

/* Reads the GPU's register window and its three interrupt lines. */
static int
describe_gpu(
	const struct drv_fdt *fdt,
	uint32_t node,
	struct mali_device *device)
{
	int error;

	/* The register window, large enough for the doorbells the driver rings. */
	error = drv_fdt_reg(fdt, node, 0U, &device->register_base, &device->register_bytes);
	if (error != 0)
		return error;
	if (device->register_bytes < REGISTERS_MIN_BYTES)
		return ERANGE;

	/* The three lines, by name, else in the order the binding gives them. */
	error = read_interrupt(fdt, node, "GPU", 0U, &device->irq_gpu);
	if (error != 0)
		return error;

	error = read_interrupt(fdt, node, "MMU", 1U, &device->irq_mmu);
	if (error != 0)
		return error;

	error = read_interrupt(fdt, node, "JOB", 2U, &device->irq_job);
	if (error != 0)
		return error;

	/* Succeeded: the node says where the GPU is. */
	return 0;
}

/*
 * Reads one interrupt line of the node by its name in interrupt-names
 * (the vendor's tree uses capitals, the upstream binding small letters),
 * or at fallback when the node names none; the line must be a GIC
 * shared peripheral one.
 */
static int
read_interrupt(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	unsigned fallback,
	int *irq)
{
	char lower[8];
	const uint8_t *value;
	uint32_t length;
	uint32_t type;
	uint32_t line;
	unsigned position;
	unsigned index;
	int error;

	/* The name as given, then in small letters. */
	position = fallback;
	error = drv_fdt_string_index(fdt, node, "interrupt-names", name, &index);
	if (error != 0) {
		for (index = 0U; name[index] != '\0' && index + 1U < sizeof(lower); index++) {
			lower[index] = name[index];
			if (name[index] >= 'A' && name[index] <= 'Z')
				lower[index] = (char)(name[index] - 'A' + 'a');
		}
		lower[index] = '\0';
		error = drv_fdt_string_index(fdt, node, "interrupt-names", lower, &index);
	}

	if (error == 0)
		position = index;

	/* The line's three cells. */
	error = drv_fdt_property(fdt, node, "interrupts", &value, &length);
	if (error != 0)
		return error;
	if (length < (position + 1U) * INTERRUPT_CELLS * 4U)
		return ERANGE;

	/* Only a shared peripheral line has a number the driver can take. */
	type = (uint32_t)drv_fdt_cells_value(value, position * INTERRUPT_CELLS, 1U);
	line = (uint32_t)drv_fdt_cells_value(value, position * INTERRUPT_CELLS + 1U, 1U);
	if (type != GIC_SPI_TYPE)
		return ENOTSUP;

	/* Succeeded: the kernel's number of the line. */
	*irq = (int)(line + GIC_SPI_BASE);
	return 0;
}

/*
 * Powers the GPU through the glue, maps its registers, reads its
 * identity, takes its lines, resets it and powers its L2 cache.
 */
static int
bring_up(
	struct mali_device *device)
{
	void *mapped;
	int error;

	/* The clocks and the resets. */
	error = device->glue.power_up(device->glue.context, &device->core_rate);
	if (error != 0) {
		kern_logf("mali: GPU not powered (%d)\n", error);
		return error;
	}

	/* The part of the register window the driver uses. */
	if (device->register_bytes > REGISTERS_MAP_BYTES)
		device->register_bytes = REGISTERS_MAP_BYTES;

	error = kern_device_map(device->register_base, (size_t)device->register_bytes, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("mali: registers at %llx not mapped (%d)\n", (unsigned long long)device->register_base, error);
		device->glue.power_down(device->glue.context);
		return error;
	}

	device->registers = mapped;

	/* What the SoC writes in the GPU's registers before the GPU's own reset. */
	if (device->glue.registers_ready != NULL)
		device->glue.registers_ready(device->glue.context, device);

	/* The identity, and one line that says it. */
	mali_gpu_read_identity(device);
	report_identity(device);

	/* Only the command stream architecture the driver knows. */
	if (device->arch_major != MALI_ARCH_VALHALL_CSF) {
		kern_logf("mali: architecture %u is not the one the driver knows (%u)\n", device->arch_major, MALI_ARCH_VALHALL_CSF);
		return ENODEV;
	}

	/* The three lines, the faults opened. */
	error = mali_gpu_irq_start(device);
	if (error != 0)
		return error;

	/* A known state, then the cache the firmware runs through. */
	error = mali_gpu_soft_reset(device);
	if (error != 0)
		return error;

	error = mali_gpu_l2_power_on(device);
	if (error != 0)
		return error;

	/* Succeeded: the GPU waits for its firmware. */
	return 0;
}

/* Writes the GPU's identity, features and present cores on one line. */
static void
report_identity(
	const struct mali_device *device)
{
	/* What a reader needs to tell the GPU and to pick its firmware. */
	kern_logf("mali: %s r%up%u arch %u.%u.%u GPU_ID %08x at %llx irq %d/%d/%d, %u shader cores (%llx), tiler %llx, L2 %llx (features %08x), spaces %08x, VA %u PA %u bits, CSF %08x, coherency %08x, clock %llu Hz\n",
		  mali_gpu_model(device),
		  device->version_major,
		  device->version_minor,
		  device->arch_major,
		  device->arch_minor,
		  device->arch_revision,
		  (unsigned)device->gpu_id,
		  (unsigned long long)device->register_base,
		  device->irq_gpu,
		  device->irq_mmu,
		  device->irq_job,
		  (unsigned)__builtin_popcountll(device->shader_present),
		  (unsigned long long)device->shader_present,
		  (unsigned long long)device->tiler_present,
		  (unsigned long long)device->l2_present,
		  (unsigned)device->l2_features,
		  (unsigned)device->address_spaces,
		  device->virtual_bits,
		  device->physical_bits,
		  (unsigned)device->csf_id,
		  (unsigned)device->coherency_features,
		  (unsigned long long)device->core_rate);
}

/* Runs the later stage, then watches the GPU for the life of the system. */
static void
late_thread(
	void *argument)
{
	struct mali_device *device;
	int error;

	/* Brings the firmware up; a failure stops the microcontroller and ends the stage. */
	device = argument;
	error = late_bring_up(device);

	/* The render node is published or never will be; the graphical login stops waiting. */
	kern_gpu_attach_end();
	if (error != 0) {
		mali_write32(device, MALI_REG_MCU_CONTROL, MALI_MCU_DISABLE);
		mali_gpu_report_events(device);
		return;
	}

	/* Writes what happens from now on. */
	watch(device);
}

/*
 * Loads the firmware file of the GPU's architecture, starts the
 * microcontroller, pings it and starts the driver's own stream.
 */
static int
late_bring_up(
	struct mali_device *device)
{
	char path[FIRMWARE_PATH_BYTES];
	uint8_t *bytes;
	size_t size;
	int error;

	/* The file is named by the architecture's major and minor numbers. */
	kern_snprintf(path, sizeof(path), FIRMWARE_DIRECTORY "/arch%u.%u/" FIRMWARE_NAME, device->arch_major, device->arch_minor);
	error = mali_firmware_read(path, FIRMWARE_LIMIT, &bytes, &size);
	if (error == ENOENT)
		error = read_other_minor(device, path, &bytes, &size);
	if (error != 0) {
		kern_logf("mali: firmware %s not read (%d)\n", path, error);
		return error;
	}

	/* The sections into memory; the file's copy is not needed after. */
	kern_logf("mali: firmware %s, %llu bytes\n", path, (unsigned long long)size);
	error = mali_csf_load(device, bytes, size);
	kern_free(bytes);
	if (error != 0)
		return error;

	/* The microcontroller and the global interface. */
	error = mali_csf_start(device);
	if (error != 0)
		return error;

	/* A first ping proves the round trip. */
	error = mali_csf_ping(device);
	if (error != 0)
		return error;

	kern_logf("mali: firmware answers; MCU enabled\n");

	/* The driver's own stream and its self-test; its failure leaves the firmware running. */
	error = mali_queue_start(device);
	if (error != 0) {
		kern_logf("mali: queue not started (%d)\n", error);
		return 0;
	}

	/* The render node over a stream that runs work; its failure leaves the rest running. */
	error = mali_render_start(device);
	if (error != 0)
		kern_logf("mali: render node not started (%d)\n", error);

	/* Succeeded: the firmware runs. */
	return 0;
}

/*
 * Reads the firmware of another published minor architecture of the
 * GPU's major one, leaving the path it read (or tried last) in path.
 */
static int
read_other_minor(
	const struct mali_device *device,
	char *path,
	uint8_t **bytes,
	size_t *size)
{
	unsigned index;
	int error;

	/* Only the major architecture whose published files are one image. */
	if (device->arch_major != MALI_ARCH_VALHALL_CSF)
		return ENOENT;

	/* Each published minor in turn, skipping the GPU's own, tried already. */
	error = ENOENT;
	for (index = 0U; index < sizeof(firmware_minors) / sizeof(firmware_minors[0]); index++) {
		if (firmware_minors[index] == device->arch_minor)
			continue;

		/* The file of that minor; anything but an absent file ends the search. */
		kern_snprintf(path, FIRMWARE_PATH_BYTES, FIRMWARE_DIRECTORY "/arch%u.%u/" FIRMWARE_NAME, device->arch_major, firmware_minors[index]);
		error = mali_firmware_read(path, FIRMWARE_LIMIT, bytes, size);
		if (error != ENOENT)
			break;
	}

	/* No published file was read either. */
	if (error != 0)
		return error;

	/* Says which file stands in for the GPU's own. */
	kern_logf("mali: no firmware of arch %u.%u; using %s\n", device->arch_major, device->arch_minor, path);

	/* Succeeded: the caller owns the bytes. */
	return 0;
}

/*
 * Writes, once a second, what the interrupt handlers recorded and a
 * change of the microcontroller's status, and pings the firmware every
 * ten seconds so its watchdog knows the host is there.
 */
static void
watch(
	struct mali_device *device)
{
	uint32_t jobs;
	unsigned reports;
	unsigned unanswered;
	int jobs_seen;
	int error;

	/* Forever, one report a second. */
	reports = 0U;
	unanswered = 0U;
	jobs_seen = 0;
	for (;;) {
		sched_sleep(sched_ticks() + kern_ms_to_ticks(REPORT_MS));

		/* Faults, and the microcontroller's status, while no submission uses the firmware. */
		mutex_lock(&device->lock);

		mali_gpu_report_events(device);
		mali_csf_report_status(device);

		mutex_unlock(&device->lock);

		/* The first job interrupt says the line works; later ones are routine. */
		jobs = __atomic_exchange_n(&device->job_events, 0U, __ATOMIC_ACQ_REL);
		if (jobs != 0U && !jobs_seen) {
			kern_logf("mali: job interrupt %d works (status %08x)\n", device->irq_job, (unsigned)jobs);
			jobs_seen = 1;
		}

		/* The watchdog's ping, until the firmware has left several unanswered in a row. */
		reports++;
		if (reports % PING_EVERY_REPORTS != 0U || unanswered >= PINGS_UNANSWERED_MAX)
			continue;

		/* The ping, likewise between submissions. */
		mutex_lock(&device->lock);

		error = mali_csf_ping(device);

		mutex_unlock(&device->lock);

		if (error == 0) {
			unanswered = 0U;
			continue;
		}

		/* Says once that the pings stop, so a dead firmware does not fill the log. */
		unanswered++;
		if (unanswered == PINGS_UNANSWERED_MAX)
			kern_logf("mali: firmware left %u pings unanswered; no more pings\n", unanswered);
	}
}
