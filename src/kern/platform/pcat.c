/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT platform.
 *
 * Boot storage comes from the BIOS IDE units; PCI, USB, and the configured
 * network and graphics drivers are registered before the host bridge is
 * probed.  The debug console is the Bochs/QEMU port 0xe9 when enabled.
 */

#include "kern/platform.h"
#include "kern/disk.h"
#include "kern/clock.h"
#include "kern/sched.h"
#include "kern/partition.h"
#include <drivers/disklabel/disklabel.h>
#include "drivers/platform/pcat/pcat-ide.h"
#include "drivers/pci/pci-pcat.h"
#include "drivers/platform/pcat/ps2-8042.h"
#include "drivers/platform/pcat/serial-mirror.h"
#if CONFIG_DRIVER_PCI_UHCI
#include "drivers/pci/pci-uhci.h"
#endif
#if CONFIG_DRIVER_PCI_EHCI
#include "drivers/pci/pci-ehci.h"
#endif
#if CONFIG_DRIVER_PCI_XHCI
#include "drivers/pci/pci-xhci.h"
#endif
#if CONFIG_DRIVER_PCI_NVME
#include <drivers/pci/pci-nvme.h>
#endif
#if CONFIG_DRIVER_PCI_INTEL_AX211
#include <drivers/wifi/intel-ax211/pci-intel-ax211.h>
#endif
#if CONFIG_DRIVER_PCI_VENUS
#include <drivers/pci/pci-venus.h>
#endif
#if CONFIG_DRIVER_PCI_I915
#include <drivers/pci/pci-i915.h>
#endif
#if CONFIG_DRIVER_PCI_HDA
#include <drivers/pci/pci-hda.h>
#endif
#if CONFIG_DRIVER_PCI_LPSS_I2C
#include <drivers/i2c/lpss-i2c.h>
#endif
#if CONFIG_DRIVER_PCI_LPSS_I2C && CONFIG_DRIVER_ACPI
#include <drivers/i2c/i2c-hid.h>
#endif
#if CONFIG_DRIVER_USB_STORAGE
#include "drivers/usb/usb-storage.h"
#include <drivers/usb/usb-uas.h>
#endif
#if CONFIG_DRIVER_USB_CDC_NCM
#include <drivers/usb/usb-cdc-ncm.h>
#endif
#if CONFIG_DRIVER_USB_CDC_ECM
#include <drivers/usb/usb-cdc-ecm.h>
#endif
#if CONFIG_DRIVER_USB_HID
#include <drivers/usb/usb-hid.h>
#include <drivers/usb/usb-hub.h>
#endif
#if CONFIG_DRIVER_USB_RTL8822BU
#include <drivers/usb/usb-rtl8822bu.h>
#endif
#if CONFIG_DRIVER_USB_CCID
#include <drivers/usb/usb-ccid.h>
#endif
#if CONFIG_DRIVER_USB_BT
#include <drivers/usb/usb-bt.h>
#endif
#include <drivers/pci/pci.h>
#if CONFIG_DRIVER_ACPI
#include <drivers/acpi/acpi.h>
#endif
#if CONFIG_DRIVER_ACPI && CONFIG_DRIVER_TYPEC
#include <drivers/typec/ucsi-acpi.h>
#endif
#include <drivers/usb/usb.h>
#if CONFIG_DRIVER_NE2000
#include "drivers/isa/pcat-ne2000.h"
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
#include "drivers/platform/pcat/graphics/pcat.h"
#endif
#include <uapi/errno.h>
#include <hal/hal.h>
#include "kern/klog.h"

/*
 * How long each way of resetting the machine is given before the next one
 * is tried, in microseconds: a reset that works takes effect at once.
 */
#define REBOOT_WAIT_US		100000U

/*
 * The reset control register of the PC chipsets (Intel's PCH, and the
 * AMD and QEMU chipsets alike): SYS_RST (bit 1) chooses a system reset
 * over an INIT, RST_CPU (bit 2) starts the reset on its rising edge, and
 * FULL_RST (bit 3) also cycles the power, a cold reset.
 */
#define RESET_CONTROL_PORT	0xcf9U
#define RESET_CONTROL_SYS_RST	0x02U
#define RESET_CONTROL_RST_CPU	0x04U
#define RESET_CONTROL_FULL_RST	0x08U

/*
 * The keyboard controller's status and command port, its input buffer
 * full bit, and the command that pulses the CPU's reset line.
 */
#define KBC_PORT		0x64U
#define KBC_STATUS_INPUT_FULL	0x02U
#define KBC_PULSE_RESET		0xfeU

/* How many reads of the status bound the wait for the controller's input buffer. */
#define KBC_DRAIN_SPINS		1000000U

static void reboot_wait(uint64_t microseconds);
static void reset_control_reset(void);
static void keyboard_reset(void);
static uint8_t port_read(uint16_t port);
static void port_write(uint16_t port, uint8_t value);

#if CONFIG_KERNEL_USB_HID_CHECKPOINT
int drv_usb_hid_checkpoint_driver_register(void);
#endif
#ifdef KERN_TEST_CHECKPOINTS
int ws004_pci_msi_qemu_register(void);
void ws004_pci_msi_qemu_raise(void);
#endif

/*
 * Initializes the PC/AT platform and enumerates its boot devices.
 *
 * Every configured driver is registered with the PCI and USB cores before
 * the host bridge is probed, so that the devices found there bind at once.
 * The BIOS IDE units become the boot device table.
 */
size_t
kern_platform_init(
	const struct kern_boot_handoff *handoff,
	struct kern_boot_device *devices,
	size_t capacity)
{
	struct disk *disk;
	struct kern_boot_device *device;
	size_t count;
	unsigned slot;
	unsigned i;
#if CONFIG_DRIVER_NE2000
	int network_error;
#endif
#if CONFIG_DRIVER_PCI_VENUS
	int venus_error;
#endif
#if CONFIG_DRIVER_PCI_I915
	int i915_error;
#endif
#if CONFIG_DRIVER_PCI_HDA
	int hda_error;
#endif
#if CONFIG_DRIVER_PCI_LPSS_I2C
	int lpss_error;
#endif
#if CONFIG_DRIVER_ACPI
	int acpi_error;
#endif
#if CONFIG_DRIVER_ACPI && CONFIG_DRIVER_TYPEC
	int typec_error;
#endif

	count = 0;

	/* Rejects a missing table or a handoff that is not a multiboot one. */
	if (handoff == 0 ||
	    devices == 0 ||
	    capacity == 0 ||
	    handoff->magic != KERN_HANDOFF_MAGIC ||
	    handoff->version != KERN_HANDOFF_VERSION_MULTIBOOT)
		return 0;

	/* Selects the partition scheme and starts with no disks. */
	partition_set_scheme(&drv_partition_scheme_pcat_auto);
	disk_registry_reset();

	/* Brings up the PCI core and, under test, the MSI fixture. */
	if (drv_pci_init() != 0)
		kern_logf("pci: core initialization failed\n");
#ifdef KERN_TEST_CHECKPOINTS
	if (ws004_pci_msi_qemu_register() != 0)
		kern_logf("WS004 MSI fixture registration failed\n");
#endif

	/* Brings up the USB core and registers the USB device drivers. */
	if (drv_usb_init() != 0)
		kern_logf("usb: core initialization failed\n");
#if CONFIG_DRIVER_USB_STORAGE
	if (drv_usb_storage_driver_register() != 0)
		kern_logf("usb: mass-storage driver registration failed\n");
	if (drv_usb_uas_driver_register() != 0)
		kern_logf("usb: UAS driver registration failed\n");
#endif
#if CONFIG_DRIVER_USB_CDC_NCM
	if (drv_usb_cdc_ncm_driver_register() != 0)
		kern_logf("usb: CDC NCM driver registration failed\n");
#endif
#if CONFIG_DRIVER_USB_CDC_ECM
	if (drv_usb_cdc_ecm_driver_register() != 0)
		kern_logf("usb: CDC ECM driver registration failed\n");
#endif
#if CONFIG_DRIVER_USB_RTL8822BU
	if (drv_usb_rtl8822bu_driver_register() != 0)
		kern_logf("usb: RTL8822BU WLAN driver registration failed\n");
#endif
#if CONFIG_KERNEL_USB_HID_CHECKPOINT
	if (drv_usb_hid_checkpoint_driver_register() != 0)
		kern_logf("usb: HID checkpoint driver registration failed\n");
#elif CONFIG_DRIVER_USB_HID
	if (drv_usb_hid_driver_register() != 0)
		kern_logf("usb: HID input driver registration failed\n");
#endif
#if CONFIG_DRIVER_USB_HUB
	if (drv_usb_hub_driver_register() != 0)
		kern_logf("usb: hub driver registration failed\n");
#endif
#if CONFIG_DRIVER_USB_CCID
	if (drv_usb_ccid_driver_register() != 0)
		kern_logf("usb: CCID smart card driver registration failed\n");
#endif
#if CONFIG_DRIVER_USB_BT
	if (drv_usb_bt_driver_register() != 0)
		kern_logf("usb: Bluetooth driver registration failed\n");
#endif

	/* Registers the PCI drivers: host controllers, NVMe, WLAN, graphics. */
#if CONFIG_DRIVER_PCI_UHCI
	if (drv_pci_uhci_driver_register() != 0)
		kern_logf("usb: UHCI PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_PCI_EHCI
	if (drv_pci_ehci_driver_register() != 0)
		kern_logf("usb: EHCI PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_PCI_XHCI
	if (drv_pci_xhci_driver_register() != 0)
		kern_logf("usb: xHCI PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_PCI_NVME
	if (drv_pci_nvme_driver_register() != 0)
		kern_logf("nvme: PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_PCI_VENUS
	/* Binds Venus through the same PCI lifecycle as other devices. */
	venus_error = drv_pci_venus_driver_register();
	if (venus_error != 0)
		kern_logf("pci: Venus driver registration failed (%d)\n", venus_error);

#endif
#if CONFIG_DRIVER_PCI_I915
	/* Binds the native Intel GPU through the same PCI lifecycle. */
	i915_error = drv_pci_i915_driver_register();
	if (i915_error != 0)
		kern_logf("pci: i915 driver registration failed (%d)\n", i915_error);

#endif
#if CONFIG_DRIVER_PCI_HDA
	/* Binds every HD Audio controller as an audio device. */
	hda_error = drv_pci_hda_driver_register();
	if (hda_error != 0)
		kern_logf("pci: HD Audio driver registration failed (%d)\n", hda_error);

#endif
#if CONFIG_DRIVER_PCI_LPSS_I2C
	/* Binds the PCH's I2C controllers, whose buses the I2C-HID touchpad uses (WS159). */
	lpss_error = drv_pci_lpss_i2c_driver_register();
	if (lpss_error != 0)
		kern_logf("pci: LPSS I2C driver registration failed (%d)\n", lpss_error);

#endif
#if CONFIG_DRIVER_PCI_INTEL_AX211
	if (drv_pci_intel_ax211_driver_register() != 0)
		kern_logf("wlan: Intel AX211 PCI driver registration failed\n");
#endif
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	if (drv_pcat_graphics_pci_register() != 0)
		kern_logf("graphics: PCI driver registration failed\n");
#endif

	/* Probes the host bridge, which binds the registered drivers. */
	if (drv_pci_pcat_init() != 0)
		kern_logf("pci: PC/AT host initialization failed\n");
#ifdef KERN_TEST_CHECKPOINTS
	else
		drv_pci_dump();
#endif
#if CONFIG_DRIVER_ACPI

	/*
	 * Loads the firmware's ACPI tables once the PCI functions are known.
	 * A platform without ACPI (ENODEV) was logged by the driver already.
	 */
	acpi_error = drv_acpi_attach();
	if (acpi_error != 0 && acpi_error != ENODEV)
		kern_logf("acpi: attachment failed (error %d)\n", acpi_error);

	/*
	 * Gives the memory BARs the firmware left unassigned an address in the
	 * host bridge's _CRS windows, and attaches the functions that waited
	 * for it (BUG-210: the 5330's LPSS I2C controllers, whose buses the
	 * touch pad's I2C-HID probe below needs).  Without a namespace there
	 * are no windows, and the functions are attached as they are.
	 */
	if (acpi_error == 0) {
		drv_pci_pcat_assign_deferred();
	} else {
		(void)drv_pci_probe_deferred();
	}
#if CONFIG_DRIVER_TYPEC

	/*
	 * Starts the USB-C connectors' UCSI driver (ws050-p003) on the
	 * namespace, once the Embedded Controller its _STA and _DSM read is
	 * attached.  A platform without a UCSI device (ENODEV) has none.
	 */
	if (acpi_error == 0) {
		typec_error = drv_ucsi_acpi_attach();
		if (typec_error != 0 && typec_error != ENODEV)
			kern_logf("ucsi: attachment failed (error %d)\n", typec_error);
	}
#endif
#endif

	/* Lists every BIOS IDE unit as a boot device. */
	(void)drv_pcat_ide_init();
	for (slot = 0; slot < 4U && count < capacity; slot++) {
		disk = drv_pcat_ide_bios_unit((uint8_t)(0x80U + slot));
		if (disk == 0)
			continue;
		device = &devices[count];
		device->device_class = KERN_DEV_IDE;
		device->display_index = (uint8_t)count;
		device->bios_id = (uint8_t)(0x80U + slot);
		device->flags = KERN_DEV_PRESENT;
		if (device->bios_id == handoff->boot_bios_id)
			device->flags |= KERN_DEV_BOOT_ORIGIN;
		device->sector_size = 512;
		device->cylinders = 0;
		device->heads = 0;
		device->sectors = 0;
		device->controller_location = (uint8_t)slot;
		for (i = 0; i < sizeof(device->reserved); i++)
			device->reserved[i] = 0;
		count++;
	}

	/* Attaches the ISA NE2000 when one is configured and present. */
#if CONFIG_DRIVER_NE2000
	network_error = drv_pcat_ne2000_init();
	if (network_error == 0)
		kern_logf("net: ISA NE2000 at 0x300 irq 10 registered "
		    "as ne0\n");
	else if (network_error != ENODEV)
		kern_logf("net: ISA NE2000 initialization failed (%d)\n",
		    network_error);
#endif

	/* Prepares the graphics driver. */
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	if (!drv_pcat_graphics_prepare())
		kern_logf("graphics: PC/AT driver unavailable\n");
#endif

	/* Reports the number of boot devices. */
	return count;
}

/*
 * Finishes device discovery once interrupts are enabled.
 *
 * The USB roots are probed and the platform waits a bounded time for boot
 * storage to appear before NVMe namespaces are probed, so that a PCI
 * namespace cannot make removable boot media look absent.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *d,
	size_t n)
{
	uint64_t deadline;

	(void)d;
	(void)n;

	/* Raises the MSI fixture interrupt under test. */
#ifdef KERN_TEST_CHECKPOINTS
	ws004_pci_msi_qemu_raise();
#endif

	/* Probes the USB roots and readies the PCI WLAN devices. */
#if CONFIG_DRIVER_PCI_UHCI
	drv_pci_uhci_probe_roots();
#endif
#if CONFIG_DRIVER_PCI_EHCI
	drv_pci_ehci_probe_roots();
#endif
#if CONFIG_DRIVER_PCI_XHCI
	drv_pci_xhci_probe_roots();
#endif
#if CONFIG_DRIVER_PCI_INTEL_AX211
	drv_pci_intel_ax211_devices_ready();
#endif

	/* Waits up to five seconds for the first disk to arrive. */
	if (disk_count() != 0)
		goto nvme;
	deadline = clock_ticks() + 5U * KERN_CLOCK_HZ;
	kern_logf("boot: waiting up to 5 seconds for boot storage\n");
	while (disk_count() == 0 && clock_ticks() < deadline)
		sched_yield();
	if (disk_count() == 0)
		kern_logf("boot: boot-storage wait expired\n");
nvme:
	(void)0;

	/*
	 * Probes NVMe namespaces after removable boot media has had its bounded
	 * discovery window.  A present PCI namespace must not make USB-root
	 * discovery look complete before the removable device arrives.
	 */
#if CONFIG_DRIVER_PCI_NVME
	drv_pci_nvme_probe_namespaces();
#endif
}

/*
 * Initializes the platform input devices.
 */
int
kern_platform_input_init(
	void)
{
	int error;

	/*
	 * USB enumeration precedes VFS input construction.  The platform-input
	 * boundary runs after input core and console registration, so dynamic
	 * HID publication preserves console event0 and its subscriber route.
	 */
#if CONFIG_DRIVER_USB_HID && !CONFIG_KERNEL_USB_HID_CHECKPOINT
	drv_usb_hid_input_ready();
#endif

	/* Attaches the PS/2 mouse. */

	/* Reports why the mouse attachment failed. */
	error = drv_pcat_ps2_8042_init();
	if (error != 0)
		return error;

	/*
	 * Lets the serial console be typed at, now that the terminal above it
	 * can take what arrives.  A build without the serial console leaves
	 * this doing nothing.
	 */
	drv_pcat_serial_mirror_start_input();

#if CONFIG_DRIVER_PCI_LPSS_I2C && CONFIG_DRIVER_ACPI
	/*
	 * Starts the HID over I2C devices the ACPI tables name (a laptop's
	 * touch pad, WS159); each publishes its input device from its own
	 * thread once it has come up, and one that does not come up leaves
	 * the PS/2 mouse as the pointer.
	 */
	(void)drv_i2c_hid_probe();
#endif

	/* Succeeded. */
	return 0;
}

/*
 * Finds the disk behind a boot device.
 */
struct disk *
kern_platform_block_device(
	const struct kern_boot_device *device)
{
	struct disk *disk;

	/* Only BIOS IDE units are boot devices on this platform. */
	if (device == 0 || device->device_class != KERN_DEV_IDE)
		return 0;

	/* Looks up the unit by its BIOS identifier. */
	disk = drv_pcat_ide_bios_unit(device->bios_id);

	/* Reports the disk, or none. */
	return disk;
}

/*
 * Writes text to the debug console port.
 */
void
kern_platform_debug_write(
	const char *text)
{
	uint8_t c;

	/* Emits every byte to port 0xe9 when the debug console is enabled. */
	while (text != 0 && *text != '\0') {
		c = (uint8_t)*text++;
#ifdef HAL_PCAT_DEBUGCON
		__asm__ volatile("outb %0,$0xe9" : : "a"(c));
#else
		(void)c;
#endif
	}
}

/*
 * Halts the CPU forever.
 */
void
kern_platform_halt(
	void)
{
	/* Shutdown barriers have finished. Use the existing terminal CPU-stop
	 * broadcast: a local CLI/HLT leaves other CPUs scheduling workers. */
	hal_cpu_panic_all();
}

/*
 * Turns the power off through ACPI's S5 soft-off.
 *
 * It returns only the reason the power stayed on: ENODEV from a firmware
 * without ACPI or without _S5, ETIMEDOUT when the write took no effect.
 */
int
kern_platform_poweroff(
	void)
{
	int error;

#if CONFIG_DRIVER_ACPI
	/* Asks the ACPI driver for S5, which normally does not return. */
	error = drv_acpi_poweroff();
#else
	/* A kernel built without the ACPI driver has no way to turn the power off. */
	error = ENODEV;
#endif
	if (error != 0)
		return error;

	/* The driver returned without an error, which it never does. */
	return EOPNOTSUPP;
}

/*
 * Reboots the machine: through the FADT's reset register, then the
 * chipset's reset control register, then the keyboard controller, and
 * halts when none of them reset it.
 *
 * Machines differ in which of them works (BUG-249: the Latitude 5320 kept
 * running after the keyboard controller's pulse, which was the only way
 * tried).  The FADT's register comes first, as the operating systems the
 * firmware is written for use it; on the Latitude 5330 it is a request to
 * firmware (0x73 written to the SMI command port 0xb2).
 */
void
kern_platform_reboot(
	void)
{
#if CONFIG_DRIVER_ACPI
	int error;

	/* The FADT's reset register, when ACPI names one; a reset that works takes effect within the wait. */
	error = drv_acpi_reset_machine();
	if (error == 0) {
		reboot_wait(REBOOT_WAIT_US);
		kern_logf("platform: reboot: the ACPI reset register did not reset the machine\n");
	}
#endif

	/* The chipset's reset control register. */
	reset_control_reset();
	kern_logf("platform: reboot: the reset control register did not reset the machine\n");

	/* The keyboard controller's reset line, then a halt in case that fails too. */
	keyboard_reset();
	kern_platform_halt();
}

/* Spins for a time, by the monotonic counter, or by a bounded count of port reads when there is none. */
static void
reboot_wait(
	uint64_t microseconds)
{
	uint64_t start;
	uint64_t now;
	uint64_t frequency;
	uint64_t rate;
	uint64_t span;
	uint64_t spin;
	bool available;

	/* The counter and its rate. */
	available = kern_rtc_read_counter(&start, &frequency);
	if (!available || frequency == 0U) {
		/* Without them, one read of an unused port takes about a microsecond on the PC buses. */
		for (spin = 0U; spin < microseconds; spin++)
			(void)port_read(0x80U);
		return;
	}

	/* The counts the wait lasts, rounded up. */
	span = (microseconds * frequency + 999999U) / 1000000U;

	/* Spins until they have passed; a counter that fails or changes rate ends the wait. */
	for (;;) {
		available = kern_rtc_read_counter(&now, &rate);
		if (!available || rate != frequency)
			return;
		if (now - start >= span)
			return;
	}
}

/*
 * Resets the machine through the chipset's reset control register: a
 * system reset with the power cycled, started by the rising edge of
 * RST_CPU, then waits for it.
 */
static void
reset_control_reset(void)
{
	uint8_t control;

	/* The register's other bits are kept; the reset's own bits start clear. */
	control = port_read(RESET_CONTROL_PORT);
	control &= (uint8_t)~(RESET_CONTROL_SYS_RST | RESET_CONTROL_RST_CPU | RESET_CONTROL_FULL_RST);

	/* A system reset with the power cycled is chosen first, then started by RST_CPU's rising edge. */
	port_write(RESET_CONTROL_PORT, (uint8_t)(control | RESET_CONTROL_SYS_RST | RESET_CONTROL_FULL_RST));
	reboot_wait(50U);
	port_write(RESET_CONTROL_PORT, (uint8_t)(control | RESET_CONTROL_SYS_RST | RESET_CONTROL_RST_CPU | RESET_CONTROL_FULL_RST));

	/* A reset that works takes effect within the wait. */
	reboot_wait(REBOOT_WAIT_US);
}

/* Pulses the CPU's reset line through the keyboard controller, then waits for it. */
static void
keyboard_reset(void)
{
	unsigned spin;
	uint8_t status;

	/* Waits a bounded time for the controller's input buffer to drain. */
	for (spin = 0U; spin < KBC_DRAIN_SPINS; spin++) {
		status = port_read(KBC_PORT);
		if ((status & KBC_STATUS_INPUT_FULL) == 0U)
			break;
	}

	/* The pulse; a reset that works takes effect within the wait. */
	port_write(KBC_PORT, KBC_PULSE_RESET);
	reboot_wait(REBOOT_WAIT_US);
}

/* Reads one byte from an I/O port. */
static uint8_t
port_read(
	uint16_t port)
{
	uint8_t value;

	/* The byte the port gives. */
	__asm__ volatile("inb %1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

/* Writes one byte to an I/O port. */
static void
port_write(
	uint16_t port,
	uint8_t value)
{
	/* The byte goes to the port. */
	__asm__ volatile("outb %0,%1" : : "a"(value), "Nd"(port));
}
