/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binding of the PowerPC Mac (the emulators' mac99 and the PowerBook
 * G4 that it stands for).
 *
 * The console is a grid of text over the whole framebuffer Open Firmware
 * left (128 by 48 cells on a 1024x768 screen), drawn by the boot
 * framebuffer driver: 32-bit pixels, or 8-bit indices into the colour
 * table the firmware loaded, which the hardware layer takes to be the
 * Macintosh system table and whose colours it hands over.  The hardware
 * layer's early console, 80 by 25 cells at the screen's corner, shows the
 * lines until then, and they are drawn again on the grid.  A screen the
 * driver cannot draw (15 or 16 bits) keeps the console a teletype on the
 * hardware layer's.  The USB and PCI drivers are
 * registered before the buses behind the host bridges are enumerated, so
 * the OHCI controllers and the built-in Ethernet (GMAC) bind there; the
 * ATA channels of the I/O controller
 * are driven by programmed I/O.  Disks are read by the Apple Partition
 * Map, or GPT or MBR on a PC disk.
 *
 * The board publishes one boot device.  With an initial ramdisk it is
 * the ramdisk (0x80), an MBR disk image whose UFS partition is the root.
 * Otherwise the firmware read the kernel from a disk (0x81): the ATA
 * drive the firmware's bootpath names, else the first disk with an
 * Apple Partition Map, waited for up to ten seconds once the USB ports
 * are scanned.  The keyboard is the power manager's ADB one or a USB one.
 * Halting, rebooting and powering off go through the hardware layer,
 * which asks the power manager.
 *
 * The machine has no serial port, so a boot that stops shows only the
 * screen.  A boot watch, a thread above the others, writes every five
 * seconds until init runs: how often each interrupt source was taken
 * (and which the hardware layer masked for a storm), the addresses the
 * tick found the processor at most often, the state of each OHCI
 * controller and of the last mass storage command.  "mac99.watch=off" on
 * the firmware's command line turns it off, and "mac99.pmu=off" leaves
 * the power manager's keyboard and mouse alone (a USB keyboard still types).
 *
 * A laptop's fan controller is set up first among the devices, so its
 * fans follow the temperatures for the rest of the boot.  "mac99.fan=off"
 * leaves it alone, and "mac99.fan=read" lets the driver read and log it
 * but write nothing.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>
#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <drivers/generic/boot-fb.h>
#include <drivers/generic/fb-text.h>
#include <drivers/generic/ramdisk.h>
#include <drivers/generic/serial-console.h>
#include <drivers/platform/mac99/mac99-ata.h>
#include <drivers/platform/mac99/mac99-ethernet.h>
#include <drivers/platform/mac99/mac99-fan.h>
#include <drivers/platform/mac99/mac99-pci.h>
#include <drivers/platform/mac99/mac99-pmu.h>
#include <kern/boot.h>
#include <kern/clock.h>
#include <kern/disk.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/partition.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/text-display.h>
#include "kern/platform/driver-startup.h"

#if CONFIG_DRIVER_USB_HID
#include <drivers/usb/usb-hid.h>
#endif

#if CONFIG_DRIVER_PCI_OHCI
#include <drivers/pci/pci-ohci.h>
#endif

#if CONFIG_DRIVER_USB_STORAGE
#include <drivers/usb/usb-storage.h>
#endif

#include <kern/process.h>
#include <kern/thread.h>
#include <kern/lock.h>

/* The block size the ramdisk is cut into; a disk image is laid out in it. */
#define MAC99_BLOCK_SIZE	512U

/* The BIOS-style ids of the boot devices: the ramdisk, and the disk the firmware read the kernel from. */
#define MAC99_BOOT_ID		0x80U
#define MAC99_BOOT_ID_DISK	0x81U

/* How long the boot waits for the disk the firmware read the kernel from. */
#define MAC99_BOOT_DISK_WAIT_MS	10000U

/* The longest early console line put in the kernel's log. */
#define MAC99_LOG_LINE		128U

/*
 * The words of the hardware layer's "mac99.framebuffer": the physical
 * address, the width, the height, the bytes a row and the bits a pixel;
 * and the entries of an 8-bit screen's colour table.
 */
#define MAC99_FB_ADDRESS	0U
#define MAC99_FB_WIDTH		1U
#define MAC99_FB_HEIGHT		2U
#define MAC99_FB_PITCH		3U
#define MAC99_FB_DEPTH		4U
#define MAC99_FB_TABLE_ENTRIES	256U

/* The Driver Descriptor Map's signature, at the start of an Apple-partitioned disk. */
#define MAC99_APM_SIGNATURE_0	0x45U
#define MAC99_APM_SIGNATURE_1	0x52U

/*
 * The boot watch: how often it writes, the most reports, its priority
 * (above the default, so a thread that spins does not silence it), the
 * addresses it names a report, and the word that turns it off.
 */
#define MAC99_WATCH_PERIOD_MS	5000U
#define MAC99_WATCH_REPORTS	120U
#define MAC99_WATCH_PRIORITY	4
#define MAC99_WATCH_TOP		4U
#define MAC99_WATCH_OFF		"mac99.watch=off"

/* The word that leaves the power manager's ADB keyboard and mouse alone. */
#define MAC99_PMU_OFF		"mac99.pmu=off"

/* The words that leave the fan controller alone, or let the driver read it but write nothing. */
#define MAC99_FAN_OFF		"mac99.fan=off"
#define MAC99_FAN_READ		"mac99.fan=read"

/* The longest line of interrupt counts, and the most sources it names. */
#define MAC99_WATCH_LINE	200U
#define MAC99_WATCH_SOURCES	128U

/* The tables the hardware layer hands over: the interrupt counts' and the samples' first words. */
#define MAC99_IRQ_WATCH_COUNTS	1U
#define MAC99_PC_SAMPLE_FIRST	2U
#define MAC99_PC_SAMPLES_MAX	512U

/* Marks a parameter that the kernel's platform interface passes but this board does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The ramdisk's disk, published by kern_platform_init() and kept for the
 * life of the system; NULL when the firmware was handed no ramdisk.
 */
static struct disk *ramdisk;

/* The boot device's id the handoff named; set by kern_platform_init(). */
static unsigned boot_id;

/*
 * The boot watch's own state, allocated when it starts and freed when it
 * ends, used by the watch thread alone: what it saw at its last report
 * (each source's count of takes and the samples taken, so each report
 * shows what happened since) and the samples a report counts, copied out
 * of the hardware layer's ring so they hold still while counted.
 */
struct watch_state {
	uint32_t last_counts[MAC99_WATCH_SOURCES];
	uint32_t last_samples;
	uint32_t samples_copy[MAC99_PC_SAMPLES_MAX];
};

/* The boot watch's sleep. */
static struct spinlock watch_lock;

static void start_console(void);
static void redraw_early_lines(void);
static void log_hardware_lines(void);
static void watch_start(void);
static void watch_thread(void *argument);
static void watch_report(struct watch_state *state, unsigned seconds);
static void watch_interrupts(struct watch_state *state, unsigned seconds);
static void watch_samples(struct watch_state *state);
static int init_runs(void);
static int command_line_has(const char *word);
static void log_lines(const char *text);
static int publish_ramdisk(void);
static struct disk *find_boot_disk(void);
static int disk_is_apple(struct disk *disk);

/*
 * Publishes the boot devices of the board: the ramdisk, if any.
 *
 * Returns the number of devices published.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	struct kern_boot_device *device;
	size_t index;
	bool may_write;
	int fan_off;
	int fan_read_only;
	int error;

	/* Publishes /dev/console's output on the screen, before anything can fail. */
	start_console();

	/* Keeps the hardware layer's lines, which scrolled away, in the kernel's log. */
	log_hardware_lines();

	/* Watches the rest of the boot, which may stop where only the screen can tell. */
	watch_start();

	/* Lets a laptop's fan controller cool it, unless the command line leaves it alone or keeps it to reading. */
	fan_off = command_line_has(MAC99_FAN_OFF);
	fan_read_only = command_line_has(MAC99_FAN_READ);
	may_write = true;
	if (fan_read_only)
		may_write = false;
	if (fan_off) {
		kern_logf("mac99: fan controller left alone by the command line\n");
	} else {
		error = drv_mac99_fan_init(may_write);
		if (error != 0 && error != ENODEV)
			kern_logf("mac99: fan controller unavailable (%d)\n", error);
	}

	/* Rejects a handoff that is not the common record. */
	if (handoff == NULL || devices == NULL || capacity == 0)
		return 0;
	if (handoff->magic != KERN_HANDOFF_MAGIC)
		return 0;
	if (handoff->version != KERN_HANDOFF_VERSION_MULTIBOOT)
		return 0;
	if (handoff->size < sizeof(*handoff))
		return 0;

	/* Reads Apple disks by their partition map and PC disks by GPT or MBR. */
	partition_set_scheme(&drv_partition_scheme_mac_auto);

	/* Starts the disk registry empty, before any disk is published. */
	disk_registry_reset();

	/* Registers the USB core and its class drivers, and the PCI drivers. */
	(void)kern_driver_startup_usb_devices();
	(void)kern_driver_startup_pci_devices();

	/* Registers the built-in Ethernet's driver with what the board knows of it. */
	error = drv_mac99_ethernet_init();
	if (error != 0)
		kern_logf("mac99: Ethernet driver unavailable (%d)\n", error);

	/* Enumerates the PCI bus, binding the drivers; a board without the host bridge goes on without it. */
	error = drv_mac99_pci_init();
	if (error != 0 && error != ENODEV)
		kern_logf("mac99: PCI unavailable (%d)\n", error);

	/* Publishes the drives of the I/O controller's ATA channels. */
	error = drv_mac99_ata_init();
	if (error != 0 && error != ENODEV)
		kern_logf("mac99: ATA unavailable (%d)\n", error);

	/* The boot device: the ramdisk, or the disk the firmware read the kernel from. */
	boot_id = handoff->boot_bios_id;
	device = &devices[0];
	if (boot_id == MAC99_BOOT_ID) {
		error = publish_ramdisk();
		if (error != 0)
			return 0;
		device->bios_id = MAC99_BOOT_ID;
	} else {
		device->bios_id = MAC99_BOOT_ID_DISK;
	}

	/* Describes the boot device the handoff names. */
	device->device_class = KERN_DEV_SD;
	device->display_index = 0;
	device->flags = KERN_DEV_PRESENT | KERN_DEV_BOOT_ORIGIN;
	device->sector_size = MAC99_BLOCK_SIZE;
	device->cylinders = 0;
	device->heads = 0;
	device->sectors = 0;
	device->controller_location = 0;

	/* Clears the record's reserved bytes. */
	for (index = 0; index < sizeof(device->reserved); index++)
		device->reserved[index] = 0;

	/* Succeeded: the single published device. */
	return 1;
}

/*
 * Finishes device discovery once interrupts are enabled: the USB ports
 * are scanned, and a boot from a disk waits a bounded time for it.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *devices,
	size_t count)
{
	struct disk *disk;
	uint64_t deadline;

	UNUSED_PARAMETER(devices);
	UNUSED_PARAMETER(count);

	/* Has the USB controllers find their devices. */
	kern_driver_probe_usb_roots_and_pci_wlan();

	/* A boot from the ramdisk waits for nothing. */
	if (boot_id != MAC99_BOOT_ID_DISK)
		return;

	/* Waits until the boot disk is found or the time is up. */
	deadline = sched_ticks() + kern_ms_to_ticks(MAC99_BOOT_DISK_WAIT_MS);
	for (;;) {
		disk = find_boot_disk();
		if (disk != NULL)
			return;
		if (sched_ticks() > deadline)
			break;
		sched_yield();
	}
	kern_logf("mac99: no boot disk found\n");
}

/*
 * Starts the board's input devices: the keyboard and mouse on the power
 * manager's ADB (a PowerBook's own), and the USB keyboards and mice.
 */
int
kern_platform_input_init(
	void)
{
	int error;

	/* The ADB keyboard and mouse; a board without the power manager has none, and the command line may refuse them. */
	if (command_line_has(MAC99_PMU_OFF)) {
		kern_logf("mac99: ADB keyboard and mouse off by the command line\n");
	} else {
		error = drv_mac99_pmu_init();
		if (error != 0 && error != ENODEV)
			kern_logf("mac99: ADB input unavailable (%d)\n", error);
	}

	/* Lets USB keyboards type into the console. */
#if CONFIG_DRIVER_USB_HID
	drv_usb_hid_input_ready();
#endif

	/* Succeeded: the input devices are started. */
	return 0;
}

/*
 * Resolves the published boot device to the ramdisk or to the disk the
 * firmware read the kernel from.
 */
struct disk *
kern_platform_block_device(
	const struct kern_boot_device *device)
{
	struct disk *disk;

	/* The ramdisk's device. */
	if (device == NULL)
		return NULL;
	if (device->bios_id == MAC99_BOOT_ID)
		return ramdisk;

	/* The boot disk's device. */
	if (device->bios_id != MAC99_BOOT_ID_DISK)
		return NULL;
	disk = find_boot_disk();

	/* Succeeded: the disk, or none when it was not found. */
	return disk;
}

/*
 * Writes a debug string to the early console.
 */
void
kern_platform_debug_write(
	const char *text)
{
	void (*console)(int character);

	/* Ignores a missing string. */
	if (text == NULL)
		return;

	/*
	 * Writes nothing once the kernel's console is published: the log
	 * already wrote the record there, and the hardware layer would only
	 * hand it to the same console again.
	 */
	console = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (console != NULL)
		return;

	/* Emits every byte through the HAL. */
	while (*text != '\0') {
		hal_putc((unsigned char)*text);
		text++;
	}
}

/*
 * Halts the machine permanently.
 */
void
kern_platform_halt(
	void)
{
	/* Halts with interrupts disabled. */
	(void)hal_irq_disable();
	for (;;)
		hal_halt();
}

/*
 * Turns the power off through the hardware layer, halting if that returns.
 */
int
kern_platform_poweroff(
	void)
{
	/* The HAL's power-off does not return when it can turn the power off. */
	hal_poweroff();
	for (;;)
		hal_halt();
}

/*
 * Reboots the machine through the hardware layer.
 */
void
kern_platform_reboot(
	void)
{
	/* Halts in case the reset request returns. */
	hal_reset();
	for (;;)
		hal_halt();
}

/*
 * Publishes the console: the text grid over the firmware's screen when
 * the boot framebuffer driver can draw it, else the teletype on the
 * hardware layer's early console.
 */
static void
start_console(
	void)
{
	struct kern_boot_framebuffer screen;
	const uint32_t *shape;
	const uint32_t *palette;
	int error;

	/* The screen the early console drew on; without one the teletype is all there is. */
	shape = hal_get_arch_handoff("mac99.framebuffer");
	if (shape == NULL) {
		drv_serial_console_init();
		return;
	}

	/*
	 * The screen in the kernel's form.  A 32-bit pixel is stored as the
	 * processor's word 0x00RRGGBB, high byte first, which is the unused
	 * byte, red, green and blue in the order of their addresses; an 8-bit
	 * screen's form is not looked at.
	 */
	screen.physical_base = shape[MAC99_FB_ADDRESS];
	screen.size = (uint64_t)shape[MAC99_FB_PITCH] * shape[MAC99_FB_HEIGHT];
	screen.width = shape[MAC99_FB_WIDTH];
	screen.height = shape[MAC99_FB_HEIGHT];
	screen.pitch = shape[MAC99_FB_PITCH];
	screen.format = KERN_BOOT_FRAMEBUFFER_XRGB8888;

	/* Maps it: 32-bit pixels, or 8-bit indices drawn with the table's colours the hardware layer hands over. */
	error = ENODEV;
	if (shape[MAC99_FB_DEPTH] == 32U) {
		error = drv_boot_fb_init(&screen, 0U);
	} else if (shape[MAC99_FB_DEPTH] == 8U) {
		palette = hal_get_arch_handoff("mac99.framebuffer.palette");
		if (palette != NULL)
			error = drv_boot_fb_init_indexed(&screen, palette, MAC99_FB_TABLE_ENTRIES);
	}

	/* A screen the driver cannot draw keeps the teletype on the early console. */
	if (error != 0) {
		kern_logf("mac99: %u-bit screen not mapped (%d); the console stays a teletype\n",
			  shape[MAC99_FB_DEPTH], error);
		drv_serial_console_init();
		return;
	}

	/* The grid over the whole screen; the machine has no serial line to add. */
	drv_fb_text_init(NULL);

	/* The grid started on a cleared screen: the early console's lines go on it again. */
	redraw_early_lines();

	/* Offers the screen to /dev/graphics; without one there is no device. */
	error = drv_boot_fb_register_backend();
	if (error != 0 && error != ENODEV)
		kern_logf("graphics: firmware framebuffer not offered (%d)\n", error);
}

/*
 * Writes what the early console printed, as far as its copy holds, on
 * the console that has just taken the screen over; a quiet console keeps
 * the screen as it is.  The characters are not logged again: the log has
 * the kernel's own, and log_hardware_lines() the hardware layer's.
 */
static void
redraw_early_lines(
	void)
{
	const char *text;
	int quiet;

	/* A quiet boot shows nothing of the log on the screen. */
	quiet = kern_log_quiet();
	if (quiet)
		return;

	/* The early console's copy, which stopped growing once the console was published. */
	text = hal_get_arch_handoff("mac99.console-log");
	if (text == NULL)
		return;

	/* Each character, on the screen only. */
	while (*text != '\0') {
		kern_text_putc((unsigned char)*text);
		text++;
	}
}

/*
 * Puts the lines of the early console that the hardware layer printed
 * about the machine (those starting "PPC32 ") in the kernel's log.
 */
static void
log_hardware_lines(
	void)
{
	/* The early console's copy, line by line, then the survey of the firmware's tree for the thermal parts. */
	log_lines(hal_get_arch_handoff("mac99.console-log"));
	log_lines(hal_get_arch_handoff("mac99.firmware-tree"));
}

/* Puts the hardware layer's lines of a text (those starting with "PPC32 ") in the kernel's log. */
static void
log_lines(
	const char *text)
{
	char line[MAC99_LOG_LINE];
	size_t length;
	size_t kept;
	int compared;

	/* A missing text has no lines. */
	if (text == NULL)
		return;

	/* Each line; a long one is cut at what a log line holds. */
	while (*text != '\0') {
		for (length = 0; text[length] != '\0' && text[length] != '\n'; length++)
			continue;
		kept = length;
		if (kept >= sizeof(line))
			kept = sizeof(line) - 1U;
		compared = 1;
		if (length >= 6U)
			compared = kern_strncmp(text, "PPC32 ", 6U);
		if (compared == 0) {
			kern_memcpy(line, text, kept);
			line[kept] = '\0';
			kern_logf("hal: %s\n", line);
		}
		text += length;
		if (*text == '\n')
			text++;
	}
}

/*
 * Publishes the initial ramdisk the firmware was handed as a disk.
 *
 * Returns 0, ENOENT when there is none, or another errno.
 */
static int
publish_ramdisk(
	void)
{
	const uint64_t *initrd_range;
	uint64_t physical;
	uint64_t size;
	uint8_t *first;
	uint8_t *last;
	int error;

	/* Finds the ramdisk's physical range; none means no boot device. */
	initrd_range = hal_get_arch_handoff("mac99.initrd");
	if (initrd_range == NULL)
		return ENOENT;
	physical = initrd_range[0];
	size = initrd_range[1];

	/* Uses whole blocks only; a tail shorter than a block holds no data of the image. */
	size -= size % MAC99_BLOCK_SIZE;
	if (size == 0)
		return ENOENT;

	/*
	 * Finds the ramdisk in the direct map.  RAM is mapped in one piece,
	 * so the range is usable when both its ends are in managed RAM and
	 * their aliases are as far apart as the addresses.
	 */
	first = kern_pmem_to_kernel((hal_physaddr_t)physical);
	if (first == NULL)
		return EFAULT;
	last = kern_pmem_to_kernel((hal_physaddr_t)(physical + size - 1));
	if (last == NULL)
		return EFAULT;
	if ((uint64_t)(last - first) != size - 1)
		return EFAULT;

	/* Publishes the memory as the ramdisk's disk. */
	error = drv_ramdisk_create("ram0", first, size, &ramdisk);
	if (error != 0) {
		kern_logf("mac99: ramdisk unavailable (%d)\n", error);
		return error;
	}

	/* Reports the ramdisk's size and place. */
	kern_logf("mac99: ramdisk ram0 %llu KiB at %llx\n",
		  (unsigned long long)(size / 1024U),
		  (unsigned long long)physical);

	/* Succeeded: the ramdisk is a disk. */
	return 0;
}

/*
 * Finds the disk the firmware read the kernel from: the ATA drive the
 * bootpath names, else the first whole disk with an Apple Partition Map.
 * The registry keeps the disk; no reference is held for the caller.
 */
static struct disk *
find_boot_disk(
	void)
{
	struct disk *disk;
	unsigned index;
	int apple;

	/* The ATA drive the firmware named. */
	disk = drv_mac99_ata_boot_disk();
	if (disk != NULL)
		return disk;

	/* The first whole disk, not the ramdisk, that is Apple-partitioned. */
	for (index = 0; ; index++) {
		disk = disk_at(index);
		if (disk == NULL)
			return NULL;
		apple = 0;
		if ((disk->d_flags & DISK_PARTITION) == 0 && disk != ramdisk)
			apple = disk_is_apple(disk);
		disk_release(disk);
		if (apple)
			return disk;
	}
}

/* Tells whether a disk starts with an Apple Driver Descriptor Map. */
static int
disk_is_apple(
	struct disk *disk)
{
	uint8_t sector[MAC99_BLOCK_SIZE];
	int error;

	/* Reads the first block of a disk of 512-byte blocks. */
	if (disk->d_block_size != MAC99_BLOCK_SIZE)
		return 0;
	error = disk_read(disk, 0, 1, sector);
	if (error != 0)
		return 0;

	/* Its signature is "ER". */
	if (sector[0] != MAC99_APM_SIGNATURE_0 || sector[1] != MAC99_APM_SIGNATURE_1)
		return 0;
	return 1;
}

/*
 * Starts the boot watch, unless the command line turns it off.
 */
static void
watch_start(
	void)
{
	struct watch_state *state;
	struct thread *thread;
	int error;

	/* The word that turns it off. */
	if (command_line_has(MAC99_WATCH_OFF))
		return;

	/* Its state, zeroed: nothing seen yet. */
	state = kern_calloc(1, sizeof(*state));
	if (state == NULL) {
		kern_logf("mac99: boot watch unavailable (no memory)\n");
		return;
	}

	/* The thread, above the default priority, which ends by itself and frees the state. */
	spin_init(&watch_lock, LOCK_RANK_DEVICE, "mac99 boot watch");
	error = kthread_create(watch_thread, state, MAC99_WATCH_PRIORITY, &thread);
	if (error != 0) {
		kern_free(state);
		kern_logf("mac99: boot watch unavailable (%d)\n", error);
		return;
	}
	thread->detached = 1;
	thread_start(thread);
}

/*
 * Writes a report every MAC99_WATCH_PERIOD_MS until init runs, at most
 * MAC99_WATCH_REPORTS times.
 */
static void
watch_thread(
	void *argument)
{
	struct watch_state *state;
	unsigned long irq;
	unsigned report;

	/* The state watch_start() made. */
	state = argument;

	/* Sleeps a period, then reports unless init runs. */
	for (report = 1; report <= MAC99_WATCH_REPORTS; report++) {
		irq = spin_lock_irqsave(&watch_lock);
		sched_sleep_locked(sched_ticks() + kern_ms_to_ticks(MAC99_WATCH_PERIOD_MS), &watch_lock);
		spin_unlock_irqrestore(&watch_lock, irq);
		if (init_runs())
			break;
		watch_report(state, report * (MAC99_WATCH_PERIOD_MS / 1000U));
	}
	if (report > MAC99_WATCH_REPORTS)
		kern_logf("mac99: watch: %u reports; the watch ends\n", MAC99_WATCH_REPORTS);
	kern_free(state);
}

/* Writes one report of the boot watch. */
static void
watch_report(
	struct watch_state *state,
	unsigned seconds)
{
	/* The interrupts, then where the processor was. */
	watch_interrupts(state, seconds);
	watch_samples(state);

	/* The USB host controllers and the last mass storage command. */
#if CONFIG_DRIVER_PCI_OHCI
	drv_pci_ohci_report();
#endif
#if CONFIG_DRIVER_USB_STORAGE
	drv_usb_storage_report();
#endif
}

/*
 * Writes how often each source was taken since the last report (and in
 * all), and the sources masked for a storm.
 */
static void
watch_interrupts(
	struct watch_state *state,
	unsigned seconds)
{
	char line[MAC99_WATCH_LINE];
	const uint32_t *table;
	uint32_t count;
	uint32_t storms;
	unsigned sources;
	unsigned source;
	size_t used;
	int written;

	/* The hardware layer's counts, a slot a source then the storm bitmap; a board without them says so. */
	table = hal_get_arch_handoff("mac99.irq-watch");
	if (table == NULL || table[0] == 0 || table[0] > MAC99_WATCH_SOURCES) {
		kern_logf("mac99: watch %us no interrupt counts\n", seconds);
		return;
	}
	sources = table[0];

	/* Each source taken at all: its takes since the last report, then in all. */
	used = 0;
	line[0] = '\0';
	for (source = 0; source < sources && used < sizeof(line); source++) {
		count = table[MAC99_IRQ_WATCH_COUNTS + source];
		if (count == 0)
			continue;
		written = kern_snprintf(line + used, sizeof(line) - used, " %u:+%u/%u",
		    source, count - state->last_counts[source], count);
		state->last_counts[source] = count;
		if (written < 0)
			break;
		used += (size_t)written;
	}
	kern_logf("mac99: watch %us irq%s\n", seconds, line[0] != '\0' ? line : " none");

	/* The sources the hardware layer masked for a storm. */
	for (source = 0; source < sources; source++) {
		storms = table[MAC99_IRQ_WATCH_COUNTS + table[0] + source / 32U];
		if ((storms & (1U << (source % 32U))) != 0)
			kern_logf("mac99: irq %u was masked for a storm\n", source);
	}
}

/*
 * Writes the addresses the tick found the processor at most often since
 * the last report, with how often.
 */
static void
watch_samples(
	struct watch_state *state)
{
	uint32_t top_address[MAC99_WATCH_TOP];
	uint32_t top_count[MAC99_WATCH_TOP];
	const uint32_t *ring;
	uint32_t taken;
	uint32_t fresh;
	uint32_t size;
	uint32_t count;
	uint32_t address;
	unsigned index;
	unsigned other;
	unsigned slot;

	/* The hardware layer's ring; a board without it says nothing. */
	ring = hal_get_arch_handoff("mac99.pc-samples");
	if (ring == NULL || ring[0] == 0 || ring[0] > MAC99_PC_SAMPLES_MAX)
		return;

	/* The samples taken since the last report, at most the ring. */
	size = ring[0];
	taken = ring[1];
	fresh = taken - state->last_samples;
	if (fresh > size)
		fresh = size;
	state->last_samples = taken;
	for (index = 0; index < fresh; index++)
		state->samples_copy[index] = ring[MAC99_PC_SAMPLE_FIRST + (taken - 1U - index) % size];

	/* Each address counted once, at its first sample; the most frequent kept. */
	kern_memset(top_address, 0, sizeof(top_address));
	kern_memset(top_count, 0, sizeof(top_count));
	for (index = 0; index < fresh; index++) {
		address = state->samples_copy[index];
		for (other = 0; other < index; other++) {
			if (state->samples_copy[other] == address)
				break;
		}
		if (other < index)
			continue;
		count = 0;
		for (other = index; other < fresh; other++) {
			if (state->samples_copy[other] == address)
				count++;
		}

		/* Takes the place of the least frequent kept, when it is more frequent. */
		slot = 0;
		for (other = 1; other < MAC99_WATCH_TOP; other++) {
			if (top_count[other] < top_count[slot])
				slot = other;
		}
		if (count > top_count[slot]) {
			top_count[slot] = count;
			top_address[slot] = address;
		}
	}

	/* The kept addresses, as found. */
	kern_logf("mac99: pc/%u %08x*%u %08x*%u %08x*%u %08x*%u\n",
	    fresh, top_address[0], top_count[0], top_address[1], top_count[1],
	    top_address[2], top_count[2], top_address[3], top_count[3]);
}

/* Reports whether init, process 1, runs. */
static int
init_runs(
	void)
{
	struct process *init;

	/* The first process, looked up and let go. */
	init = process_find_ref(1);
	if (init == NULL)
		return 0;
	process_release(init);

	/* Succeeded: init runs. */
	return 1;
}

/* Reports whether the firmware's command line holds a word, as a word of its own. */
static int
command_line_has(
	const char *word)
{
	const char *line;
	const char *found;
	size_t length;

	/* Without a command line it holds nothing. */
	line = hal_get_arch_handoff("boot.command-line");
	if (line == NULL)
		return 0;

	/* Each place a word starts, compared with the word to its end. */
	length = kern_strlen(word);
	for (found = line; *found != '\0'; found++) {
		if (found != line && found[-1] != ' ')
			continue;
		if (kern_strncmp(found, word, length) != 0)
			continue;
		if (found[length] == '\0' || found[length] == ' ')
			return 1;
	}

	/* The line does not hold it. */
	return 0;
}
