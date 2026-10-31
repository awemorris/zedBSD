/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The binding of the StarFive VisionFive 2 board.
 *
 * The console is the text console over the loader's screen and the UART
 * the firmware's console uses: what it writes goes to both, and what the
 * UART receives, by its interrupt, is typed into it beside USB keyboards.
 * The board publishes one boot device: the initial ramdisk the loader read,
 * when there is one, and otherwise the SD card.  The ramdisk holds a disk
 * image laid out like a Raspberry Pi 4's SD card, a FAT boot partition
 * first and the UFS root beside it, so the kernel's root search finds the
 * root as it does there.  An SD card booted without a ramdisk carries its
 * root as a UFS partition of its own, which the boot parameters name.
 * Halting, rebooting and powering off go through the hardware layer.
 *
 * The screen the firmware left on, when the loader handed one over, is
 * the hardware layer's: it draws the console there.  The binding only
 * reads the screen's named values and checks them, for the graphics
 * driver that will take the screen over.
 *
 * The SoC's system control blocks, clock and reset generators and pin
 * controllers are brought up from the device tree before any device, so
 * the devices' drivers can switch their own clocks, resets and pins.  The
 * boot log shows the PLLs' and the SD card clock's rates and the card
 * detect pin, for comparing with what the firmware set.
 *
 * The SD card slot's card is published as the disk mmcblk0 beside the
 * ramdisk, and is the boot device when there is no ramdisk.  Once
 * interrupts are on, the boot log lists the card's partitions.
 *
 * The first Ethernet port is published as the network interface en0.
 *
 * The GPU core is brought up as far as reading its identity, from the
 * board's own description of it unless the device tree enables a node.
 * The display controller the loader left on is then read, from the
 * board's description of it or the tree's node, to decide whether its
 * screen can be taken over; nothing of it is changed.
 *
 * The console is the serial teletype with one operation added: a fatal
 * stop puts the display controller's plane back on the loader's screen,
 * where the hardware layer draws the console's text, in case a graphics
 * program had the controller show a buffer of its own.
 *
 * Once a GPU has flipped its first image the console leaves the screen
 * for good: the kernel publishes its own character path, which writes the
 * firmware console's UART, so the hardware layer neither draws on the
 * screen nor calls the firmware again; the serial line carries on, and a
 * fatal stop takes no display back.  Without that UART the console stays
 * as it is.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hal/hal.h>
#include <drivers/disklabel/disklabel.h>
#include <drivers/generic/fdt.h>
#include <drivers/generic/ramdisk.h>
#include <drivers/generic/boot-fb.h>
#include <drivers/generic/fb-text.h>
#include <drivers/generic/serial-console.h>
#include <drivers/platform/visionfive2/vf2-crg.h>
#include <drivers/platform/visionfive2/vf2-ethernet.h>
#if CONFIG_DRIVER_POWERVR
#include <drivers/platform/visionfive2/vf2-gpu.h>
#include <drivers/platform/visionfive2/vf2-pmu.h>
#include "drivers/gpu/powervr/powervr.h"
#endif
#include <drivers/platform/visionfive2/vf2-mmc.h>
#include <drivers/platform/visionfive2/vf2-pcie.h>
#include <drivers/platform/visionfive2/vf2-pinctrl.h>
#include <drivers/platform/visionfive2/vf2-pwmdac.h>
#ifdef CONFIG_VF2_DMA_TEST
#include <drivers/platform/visionfive2/vf2-dmatest.h>
#endif
#include <drivers/platform/visionfive2/vf2-syscon.h>
#include <drivers/serial/ns16550.h>
#include <kern/boot.h>
#include <kern/disk.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/partition.h>
#include <kern/text-display.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

/* The block size the ramdisk is cut into; a disk image is laid out in it. */
#define VF2_BLOCK_SIZE		512U

/* The BIOS-style id the boot device has, the one the handoff names. */
#define VF2_BOOT_ID		0x80U

/* The bytes of one pixel of the screen the hardware layer names. */
#define VF2_PIXEL_BYTES		4U

/* Marks a parameter that the kernel's platform interface passes but this board does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * Whether the board's devices snoop the CPU caches when the device tree
 * does not say.
 *
 * The JH7110's device tree marks no device non-coherent, and its L2 cache
 * controller offers no maintenance for device memory, which points to
 * coherent DMA; the board's DMA glue passes this to drv_fdt_dma_coherent()
 * as the default.
 */
#define VF2_DMA_DEFAULT_COHERENT	1

/* The SD card slot's controller, whose clock and card detect are logged. */
#define VF2_SD_REGISTERS	0x16020000ULL

/* The AXI configuration bus clock's number in the SYS clock generator. */
#define VF2_AXI_CONFIG_CLOCK	7U

/* The SD card clock's number in the SYS clock generator. */
#define VF2_SD_CARD_CLOCK	94U

#if CONFIG_DRIVER_POWERVR
/*
 * Where the GPU core is: the board's device tree usually has no node for
 * it, so the board describes it itself.
 *
 * Values referenced:
 * StarFive JH7110 TRM Preliminary V2, Table 2-4 System Memory Map (the
 * GPU at 0x1800_0000, 1 MB) and Table 2-3 System Interrupt Connections
 * (gpu_os_irq is global interrupt 77, which the PLIC numbers 82, five
 * above, as for the board's other devices); StarFive Linux
 * JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (gpu@18000000, interrupts 82).
 */
static const struct drv_vf2_gpu_place vf2_gpu_builtin = {
	0x18000000ULL,
	0x100000ULL,
	82U
};

/*
 * Where the display controller is: the board's device tree usually has no
 * enabled node for it, so the board describes it itself.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (dc8200@29400000: the
 * identity window 0x2940_0000 of 0x100 bytes, the display window
 * 0x2940_0800 of 0x2000 bytes, interrupts 95).
 */
static const struct drv_powervr_dc_place vf2_dc_builtin = {
	0x29400000ULL,
	0x100ULL,
	0x29400800ULL,
	0x2000ULL,
	95U
};

/*
 * The clocks the display controller's registers need, in the order a bus
 * access passes them: the SYS generator's display bus gates, then the
 * VOUT generator's gates of the controller and of the HDMI panel's pixel
 * clock.
 *
 * Values referenced:
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_SYSCLK_NOC_BUS_DISP_AXI 60, JH7110_SYSCLK_VOUT_TOP_AHB 61,
 * JH7110_SYSCLK_VOUT_TOP_AXI 62, JH7110_VOUTCLK_DC8200_AXI 4,
 * JH7110_VOUTCLK_DC8200_CORE 5, JH7110_VOUTCLK_DC8200_AHB 6,
 * JH7110_VOUTCLK_DC8200_PIX0 7); StarFive Linux
 * JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (dc8200's clocks).
 */
struct vf2_dc_gate {
	struct drv_vf2_clock clock;
	const char *name;
};

static const struct vf2_dc_gate vf2_dc_gates[] = {
	{ { DRV_VF2_CRG_SYS, 60U }, "noc_bus_disp_axi" },
	{ { DRV_VF2_CRG_SYS, 61U }, "vout_top_ahb" },
	{ { DRV_VF2_CRG_SYS, 62U }, "vout_top_axi" },
	{ { DRV_VF2_CRG_VOUT, 4U }, "dc8200_axi" },
	{ { DRV_VF2_CRG_VOUT, 5U }, "dc8200_core" },
	{ { DRV_VF2_CRG_VOUT, 6U }, "dc8200_ahb" },
	{ { DRV_VF2_CRG_VOUT, 7U }, "dc8200_pix0" }
};

/*
 * Where the HDMI transmitter and the display subsystem's control block
 * are, for a boot whose display driver lights the output itself.
 *
 * Values referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (hdmi@29590000, 0x4000 bytes;
 * dssctrl@295b0000, 0x90 bytes).
 */
#define VF2_HDMI_BASE		0x29590000ULL
#define VF2_HDMI_SIZE		0x4000ULL
#define VF2_DSS_BASE		0x295b0000ULL
#define VF2_DSS_SIZE		0x90ULL

/*
 * The clocks the display controller and the HDMI transmitter run on when
 * the board powers the output up itself, in the order they are started:
 * the SYS generator's display source and bus gates and the HDMI audio
 * master clock, then the VOUT generator's controller, pixel, LCD and HDMI
 * gates.
 *
 * Values referenced:
 * Linux v6.12 include/dt-bindings/clock/starfive,jh7110-crg.h
 * (JH7110_SYSCLK_VOUT_SRC 58, JH7110_SYSCLK_NOC_BUS_DISP_AXI 60,
 * JH7110_SYSCLK_VOUT_TOP_AHB 61, JH7110_SYSCLK_VOUT_TOP_AXI 62,
 * JH7110_SYSCLK_VOUT_TOP_HDMITX0_MCLK 63, JH7110_VOUTCLK_DC8200_AXI 4 to
 * JH7110_VOUTCLK_DC8200_PIX1 8, JH7110_VOUTCLK_DOM_VOUT_TOP_LCD 9,
 * JH7110_VOUTCLK_HDMI_TX_MCLK 15 to JH7110_VOUTCLK_HDMI_TX_SYS 17);
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * arch/riscv/boot/dts/starfive/jh7110.dtsi (dc8200's and hdmi's clocks).
 */
static const struct vf2_dc_gate vf2_dc_start_gates[] = {
	{ { DRV_VF2_CRG_SYS, 58U }, "vout_src" },
	{ { DRV_VF2_CRG_SYS, 60U }, "noc_bus_disp_axi" },
	{ { DRV_VF2_CRG_SYS, 61U }, "vout_top_ahb" },
	{ { DRV_VF2_CRG_SYS, 62U }, "vout_top_axi" },
	{ { DRV_VF2_CRG_SYS, 63U }, "hdmitx0_mclk" },
	{ { DRV_VF2_CRG_VOUT, 4U }, "dc8200_axi" },
	{ { DRV_VF2_CRG_VOUT, 5U }, "dc8200_core" },
	{ { DRV_VF2_CRG_VOUT, 6U }, "dc8200_ahb" },
	{ { DRV_VF2_CRG_VOUT, 7U }, "dc8200_pix0" },
	{ { DRV_VF2_CRG_VOUT, 8U }, "dc8200_pix1" },
	{ { DRV_VF2_CRG_VOUT, 9U }, "vout_top_lcd" },
	{ { DRV_VF2_CRG_VOUT, 15U }, "hdmi_tx_mclk" },
	{ { DRV_VF2_CRG_VOUT, 16U }, "hdmi_tx_bclk" },
	{ { DRV_VF2_CRG_VOUT, 17U }, "hdmi_tx_sys" }
};

/*
 * The reset lines released when the board powers the output up itself:
 * the SYS generator's display source and bus, then the VOUT generator's
 * controller lines and the HDMI transmitter's.
 *
 * Values referenced:
 * Linux v6.12 include/dt-bindings/reset/starfive,jh7110-crg.h
 * (JH7110_SYSRST_VOUT_TOP_SRC 43, JH7110_SYSRST_NOC_BUS_DISP_AXI 26,
 * JH7110_VOUTRST_DC8200_AXI 0, _AHB 1, _CORE 2,
 * JH7110_VOUTRST_HDMI_TX_HDMI 9).
 */
static const struct drv_vf2_reset vf2_dc_start_resets[] = {
	{ DRV_VF2_CRG_SYS, 43U },
	{ DRV_VF2_CRG_SYS, 26U },
	{ DRV_VF2_CRG_VOUT, 0U },
	{ DRV_VF2_CRG_VOUT, 1U },
	{ DRV_VF2_CRG_VOUT, 2U },
	{ DRV_VF2_CRG_VOUT, 9U }
};

/*
 * What a fatal stop calls to put the display back on the loader's screen.
 *
 * Set once by probe_display_controller() on the boot CPU when the display
 * driver read the controller, and not changed afterwards; NULL while no
 * display is to be taken back.
 */
static void (*vf2_fatal_display)(void);
#endif

/*
 * What the board gives the console: the UART when it was found, and the
 * fatal stop's return of the display to the loader's screen.  Filled once
 * by start_console() before the console starts and not changed
 * afterwards.
 */
static struct drv_fb_text_board vf2_console_board;

/*
 * The ramdisk's disk, published by kern_platform_init() and kept for the
 * life of the system; NULL when the loader read no ramdisk.
 */
static struct disk *ramdisk;

/*
 * The device tree, kept once probe_soc() has brought the SoC's blocks up
 * from it.  The tree's blob is the hardware layer's and lives as long as
 * the system.
 */
static struct drv_fdt soc_tree;

/* Nonzero once probe_soc() has filled soc_tree; never cleared. */
static int soc_ready;

/*
 * The UART the firmware's console uses (/chosen/stdout-path), mapped by
 * probe_soc() on the boot CPU and kept for the life of the system;
 * console_uart_present says it was found, and neither changes afterwards.
 */
static struct drv_ns16550 console_uart;
static int console_uart_present;

/*
 * The SD card's disk, published by kern_platform_init() and kept for the
 * life of the system; NULL when the slot is empty or the card failed.
 */
static struct disk *sd_card;

/*
 * The SD card's partitions as the boot log lists them.  Used only by
 * describe_sd_partitions(), once, during the boot; it is file-scope to
 * keep the table off the stack.
 */
static struct partition sd_partitions[PARTITION_MAX];

static int publish_ramdisk(void);
static void describe_framebuffer(void);
static void probe_soc(void);
static void describe_clocks(void);
static void describe_card_detect(const struct drv_fdt *fdt);
static void attach_sd_card(void);
static void attach_ethernet(void);
static void describe_sd_partitions(void);
static void reveal_fatal(void);
static void probe_console_uart(const struct drv_fdt *fdt);
static void start_console(void);
static void console_uart_putc(int character);
#if CONFIG_DRIVER_POWERVR
static void probe_display_controller(const struct drv_fdt *fdt);
static int check_display_clocks(void);
static void light_display(const struct drv_powervr_dc_place *place);
static int start_display_gates(enum drv_vf2_crg crg);
static int release_display_resets(enum drv_vf2_crg crg);
static void describe_display_power(void);
static int power_display(void);
#endif

/*
 * Publishes the boot device of the VisionFive 2: the ramdisk, or the SD
 * card when the loader read no ramdisk.
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
	int error;

	/* Rejects a handoff that is not the common record. */
	if (handoff == NULL || devices == NULL || capacity == 0)
		return 0;
	if (handoff->magic != KERN_HANDOFF_MAGIC)
		return 0;
	if (handoff->version != KERN_HANDOFF_VERSION_MULTIBOOT)
		return 0;
	if (handoff->size < sizeof(*handoff))
		return 0;

	/* Reads and checks the screen the firmware left on, for the boot log. */
	describe_framebuffer();

	/* Brings up the SoC's clocks, resets and pins for the devices. */
	probe_soc();

	/* Publishes the console over the loader's screen and the UART probe_soc() found. */
	start_console();

	/*
	 * Selects the partition scheme that tells GPT from MBR: the ramdisk's
	 * image is an MBR disk, the SD card's UEFI image a GPT disk.
	 */
	partition_set_scheme(&drv_partition_scheme_pcat_auto);

	/* Starts the disk registry empty, before any disk is published. */
	disk_registry_reset();

	/* Publishes the ramdisk, the boot device. */
	error = publish_ramdisk();

	/* Publishes the SD card beside it. */
	attach_sd_card();

	/* Publishes the Ethernet port. */
	attach_ethernet();

	/* Without the ramdisk the SD card is the boot device, if there is one. */
	if (error != 0 && sd_card == NULL)
		return 0;

	/* Says which disk the boot device is when there is no ramdisk. */
	if (error != 0)
		kern_logf("visionfive2: no ramdisk; the boot device is %s\n", sd_card->d_name);

	/* Describes the boot device the handoff names. */
	device = &devices[0];
	device->device_class = KERN_DEV_SD;
	device->display_index = 0;
	device->bios_id = VF2_BOOT_ID;
	device->flags = KERN_DEV_PRESENT;
	if (handoff->boot_bios_id == VF2_BOOT_ID)
		device->flags |= KERN_DEV_BOOT_ORIGIN;
	device->sector_size = VF2_BLOCK_SIZE;
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
 * Finishes device discovery once interrupts are enabled: lists the SD
 * card's partitions and looks behind the PCIe root ports.
 */
void
kern_platform_refresh_devices(
	const struct kern_boot_device *devices,
	size_t count)
{
	UNUSED_PARAMETER(devices);
	UNUSED_PARAMETER(count);

	/* Lists the SD card's partitions. */
	describe_sd_partitions();

	/* Looks behind the USB root ports and publishes the NVMe namespaces. */
	drv_vf2_pcie_refresh();
}

/*
 * Starts the console's input: USB keyboards, and what the console's UART
 * receives.
 */
int
kern_platform_input_init(
	void)
{
	int error;

	/* Lets USB keyboards type into the console. */
	drv_vf2_pcie_input_ready();

	/* A tree without the console's UART leaves the keyboards alone. */
	if (!console_uart_present)
		return 0;

	/* Has the UART's receive interrupt feed the console. */
	error = drv_ns16550_start_input(&console_uart, drv_serial_console_receive);
	if (error != 0) {
		kern_logf("visionfive2: serial input unavailable (%d)\n", error);
		return error;
	}

	/* Succeeded: the serial line and USB keyboards type into the console. */
	return 0;
}

/*
 * Resolves the published boot device to the ramdisk, or to the SD card
 * when there is no ramdisk.
 */
struct disk *
kern_platform_block_device(
	const struct kern_boot_device *device)
{
	/* Only the boot device has a disk. */
	if (device == NULL || device->device_class != KERN_DEV_SD)
		return NULL;

	/* The ramdisk, when the loader read one. */
	if (ramdisk != NULL)
		return ramdisk;

	/* Succeeded: the SD card, or none when the slot is empty. */
	return sd_card;
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
	 * already wrote the record there, UART included.
	 */
	console = __atomic_load_n(&kernel_putc, __ATOMIC_ACQUIRE);
	if (console != NULL)
		return;

	/* Emits every byte through the hardware layer. */
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
 * Turns the power off through the firmware, halting if that returns.
 */
int
kern_platform_poweroff(
	void)
{
	/* The hardware layer's power-off does not return when the firmware can turn the power off. */
	hal_poweroff();
	for (;;)
		hal_halt();
}

/*
 * Reboots the machine through the firmware.
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
 * Publishes the initial ramdisk the loader read as a disk.
 *
 * Returns 0, ENOENT when the loader read none, or another errno.
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
	initrd_range = hal_get_arch_handoff("riscv64.initrd");
	if (initrd_range == NULL)
		return ENOENT;
	physical = initrd_range[0];
	size = initrd_range[1];

	/* Uses whole blocks only; a tail shorter than a block holds no data of the image. */
	size -= size % VF2_BLOCK_SIZE;
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
		kern_logf("visionfive2: ramdisk unavailable (%d)\n", error);
		return error;
	}

	/* Reports the ramdisk's size and place. */
	kern_logf("visionfive2: ramdisk ram0 %llu KiB at %llx\n",
		  (unsigned long long)(size / 1024U),
		  (unsigned long long)physical);

	/* Succeeded: the ramdisk is a disk. */
	return 0;
}

/*
 * Reads the screen the hardware layer names under "riscv64.framebuffer"
 * and writes it to the boot log, refusing values that do not describe a
 * screen: a format other than the two four-byte ones, rows shorter than
 * the width, or memory too small for every row.
 */
static void
describe_framebuffer(
	void)
{
	struct kern_boot_framebuffer framebuffer;
	uint64_t needed;
	int error;

	/* Reads the named values; a boot without the loader's screen has none. */
	error = kern_boot_framebuffer("riscv64.framebuffer", &framebuffer);
	if (error != 0) {
		kern_logf("visionfive2: no framebuffer (%d)\n", error);
		return;
	}

	/* Refuses a pixel format the hardware layer does not name. */
	if (framebuffer.format != KERN_BOOT_FRAMEBUFFER_RGBX8888 &&
	    framebuffer.format != KERN_BOOT_FRAMEBUFFER_BGRX8888) {
		kern_logf("visionfive2: framebuffer of unknown format %u\n", framebuffer.format);
		return;
	}

	/* Refuses rows shorter than the width, the pitch being in bytes. */
	if ((uint64_t)framebuffer.pitch < (uint64_t)framebuffer.width * VF2_PIXEL_BYTES) {
		kern_logf("visionfive2: framebuffer pitch %u shorter than its width %u\n",
			  framebuffer.pitch,
			  framebuffer.width);
		return;
	}

	/* Refuses memory too small for every row. */
	needed = (uint64_t)framebuffer.pitch * framebuffer.height;
	if (framebuffer.size < needed) {
		kern_logf("visionfive2: framebuffer of %llu bytes for %llu\n",
			  (unsigned long long)framebuffer.size,
			  (unsigned long long)needed);
		return;
	}

	/* Succeeded: the screen as the hardware layer named it. */
	kern_logf("visionfive2: framebuffer %ux%u pitch %u format %u at %llx size %llx\n",
		  framebuffer.width,
		  framebuffer.height,
		  framebuffer.pitch,
		  framebuffer.format,
		  (unsigned long long)framebuffer.physical_base,
		  (unsigned long long)framebuffer.size);
}

/*
 * Finds the system control blocks, clock and reset generators and pin
 * controllers in the device tree, and logs the rates and the card detect
 * pin.
 *
 * A tree that cannot be read, or one without the blocks, leaves the board
 * without its devices' clocks; the boot goes on with the ramdisk.
 */
static void
probe_soc(
	void)
{
	const uint64_t *fdt_range;
	const uint64_t *ccache;
	struct drv_fdt fdt;
	const void *blob;
	int error;

	/* Says whether the hardware layer found the cache controller that cache maintenance writes. */
	ccache = hal_get_arch_handoff("riscv64.ccache.base");
	if (ccache != NULL) {
		kern_logf("visionfive2: ccache at 0x%llx, cache maintenance on\n", (unsigned long long)*ccache);
	} else {
		kern_logf("visionfive2: ccache unavailable, no cache maintenance\n");
	}

	/* Finds the device tree's range the hardware layer kept. */
	fdt_range = hal_get_arch_handoff("riscv64.fdt");
	if (fdt_range == NULL) {
		kern_logf("visionfive2: no device tree\n");
		return;
	}

	/* Finds the tree's bytes in the direct map, refusing a tree outside RAM. */
	blob = kern_pmem_to_kernel((hal_physaddr_t)fdt_range[0]);
	if (blob == NULL) {
		kern_logf("visionfive2: device tree outside RAM\n");
		return;
	}

	/* Refuses a tree whose header does not check out. */
	error = drv_fdt_open(&fdt, blob, (size_t)fdt_range[1]);
	if (error != 0) {
		kern_logf("visionfive2: unreadable device tree (%d)\n", error);
		return;
	}

	/* Maps the firmware console's UART, which needs nothing else of the SoC. */
	probe_console_uart(&fdt);

	/* Maps the system control blocks, which hold the PLL settings. */
	error = drv_vf2_syscon_init(&fdt);
	if (error == ENODEV) {
		/*
		 * A tree without the blocks is not in the binding of Linux's
		 * mainline device tree, which the board's drivers read: StarFive's
		 * U-Boot hands over its own tree when bootefi is given
		 * ${fdtcontroladdr}.  The EFI partition carries the mainline trees.
		 */
		kern_logf("visionfive2: system control blocks unavailable (%d)\n", error);
		kern_logf("visionfive2: the device tree is not Linux's mainline binding; boot with the EFI partition's "
			  "dtb/jh7110-starfive-visionfive-2-v1.3b.dtb (or -v1.2a.dtb) loaded to ${fdt_addr_r} and passed to bootefi\n");
		return;
	} else if (error != 0) {
		kern_logf("visionfive2: system control blocks unavailable (%d)\n", error);
		return;
	}

	/* Maps the clock and reset generators. */
	error = drv_vf2_crg_init(&fdt);
	if (error != 0) {
		kern_logf("visionfive2: clock generators unavailable (%d)\n", error);
		return;
	}

	/* Maps the pin controllers. */
	error = drv_vf2_pinctrl_init(&fdt);
	if (error != 0) {
		kern_logf("visionfive2: pin controllers unavailable (%d)\n", error);
		return;
	}

	/* Logs what the firmware left the clocks and the card slot at. */
	describe_clocks();
	describe_card_detect(&fdt);

	/* Keeps the tree for the devices brought up after the disk registry is reset. */
	soc_tree = fdt;
	soc_ready = 1;

	/* Starts the PCIe root complexes and the USB and NVMe drivers behind them. */
	error = drv_vf2_pcie_init(&fdt, VF2_DMA_DEFAULT_COHERENT);
	if (error != 0 && error != ENODEV)
		kern_logf("visionfive2: PCIe unavailable (%d)\n", error);

	/* Publishes the headphone jack; the board boots without sound when it fails. */
	error = drv_vf2_pwmdac_attach(&fdt, VF2_DMA_DEFAULT_COHERENT);
	if (error != 0 && error != ENODEV)
		kern_logf("visionfive2: sound unavailable (%d)\n", error);
#if CONFIG_DRIVER_POWERVR

	/* Brings the GPU core up as far as its identity; the board boots without it when it fails. */
	error = drv_vf2_gpu_attach(&fdt, &vf2_gpu_builtin);
	if (error != 0)
		kern_logf("visionfive2: GPU unavailable (%d)\n", error);

	/* Reads the display controller the loader left on and decides on its takeover. */
	probe_display_controller(&fdt);
#endif
#ifdef CONFIG_VF2_DMA_TEST

	/* The test kernel's DMA coherency check. */
	error = drv_vf2_dmatest_publish(&soc_tree);
	if (error != 0)
		kern_logf("visionfive2: DMA test unavailable (%d)\n", error);
#endif
}

/*
 * Writes the PLLs' rates and the SD card clock's rate to the boot log.
 *
 * The rates are computed from the registers, for comparing with the
 * values the firmware's memory dump shows.  A rate that cannot be
 * computed is logged as its error.
 */
static void
describe_clocks(
	void)
{
	struct drv_vf2_clock clock;
	uint64_t pll_rates[3];
	uint64_t bus_rate;
	uint64_t card_rate;
	int pll_errors[3];
	int bus_error;
	int card_error;
	unsigned pll;

	/* Computes the three PLLs' rates. */
	for (pll = 0; pll < 3U; pll++) {
		pll_errors[pll] = drv_vf2_pll_get_rate(pll, &pll_rates[pll]);
		if (pll_errors[pll] != 0)
			pll_rates[pll] = 0;
	}

	/* Logs them, each with its error, zero when computed. */
	kern_logf("visionfive2: pll0 %llu Hz (%d), pll1 %llu Hz (%d), pll2 %llu Hz (%d)\n",
		  (unsigned long long)pll_rates[0],
		  pll_errors[0],
		  (unsigned long long)pll_rates[1],
		  pll_errors[1],
		  (unsigned long long)pll_rates[2],
		  pll_errors[2]);

	/* Computes the AXI configuration bus's rate, which the card clock divides. */
	clock.crg = DRV_VF2_CRG_SYS;
	clock.index = VF2_AXI_CONFIG_CLOCK;
	bus_error = drv_vf2_clock_get_rate(&clock, &bus_rate);
	if (bus_error != 0)
		bus_rate = 0;

	/* Computes the SD card clock's rate. */
	clock.index = VF2_SD_CARD_CLOCK;
	card_error = drv_vf2_clock_get_rate(&clock, &card_rate);
	if (card_error != 0)
		card_rate = 0;

	/* Logs both, each with its error, zero when computed. */
	kern_logf("visionfive2: axi_cfg0 %llu Hz (%d), sdcard %llu Hz (%d)\n",
		  (unsigned long long)bus_rate,
		  bus_error,
		  (unsigned long long)card_rate,
		  card_error);
}

/*
 * Writes whether a card is in the SD slot to the boot log.
 *
 * The slot's card detect pin is made an input and read once.
 */
static void
describe_card_detect(
	const struct drv_fdt *fdt)
{
	struct drv_vf2_gpio card_detect;
	uint64_t address;
	uint64_t size;
	uint32_t node;
	int present;
	int error;

	/* Finds the SD controller the slot is wired to among the controllers. */
	node = DRV_FDT_NO_NODE;
	for (;;) {
		/* Moves to the next controller; none left means no slot. */
		error = drv_fdt_find_compatible(fdt, "starfive,jh7110-mmc", node, &node);
		if (error != 0) {
			kern_logf("visionfive2: no SD card slot (%d)\n", error);
			return;
		}

		/* Stops at the controller whose registers are the slot's. */
		error = drv_fdt_reg(fdt, node, 0, &address, &size);
		if (error == 0 && address == VF2_SD_REGISTERS)
			break;
	}

	/* Finds the card detect pin. */
	error = drv_vf2_gpio_lookup(fdt, node, "cd-gpios", &card_detect);
	if (error != 0) {
		kern_logf("visionfive2: no card detect pin (%d)\n", error);
		return;
	}

	/* Makes the pin an input. */
	error = drv_vf2_gpio_configure_input(&card_detect);
	if (error != 0) {
		kern_logf("visionfive2: card detect pin unusable (%d)\n", error);
		return;
	}

	/* Reads whether a card is in. */
	error = drv_vf2_gpio_read(&card_detect, &present);
	if (error != 0) {
		kern_logf("visionfive2: card detect unreadable (%d)\n", error);
		return;
	}

	/* Succeeded: logs the slot's state. */
	if (present != 0) {
		kern_logf("visionfive2: SD card present (gpio %u)\n", card_detect.pin);
	} else {
		kern_logf("visionfive2: SD card absent (gpio %u)\n", card_detect.pin);
	}
}

/*
 * Brings up the SD card slot and publishes its card as mmcblk0.
 *
 * A board whose SoC blocks did not come up, an empty slot or a card that
 * fails leaves the board without the disk; the boot goes on.
 */
static void
attach_sd_card(
	void)
{
	int error;

	/* Needs the clocks, resets and pins the slot's glue switches. */
	if (!soc_ready) {
		kern_logf("visionfive2: SD card slot not brought up: SoC blocks unavailable\n");
		return;
	}

	/* Brings the slot and its card up. */
	error = drv_vf2_mmc_attach(&soc_tree, &sd_card);
	if (error != 0) {
		sd_card = NULL;
		kern_logf("visionfive2: no SD card disk (%d)\n", error);
		return;
	}

	/* Succeeded: the card is a disk. */
	kern_logf("visionfive2: SD card is %s\n", sd_card->d_name);
}

/*
 * Brings up the first Ethernet port and publishes it as en0.
 *
 * A board whose SoC blocks did not come up, or a port that fails, leaves
 * the board without the interface; the boot goes on.
 */
static void
attach_ethernet(
	void)
{
	int error;

	/* Needs the clocks, resets and pins the port's glue switches. */
	if (!soc_ready) {
		kern_logf("visionfive2: Ethernet not brought up: SoC blocks unavailable\n");
		return;
	}

	/* Brings the port and its PHY up. */
	error = drv_vf2_ethernet_attach(&soc_tree, VF2_DMA_DEFAULT_COHERENT);
	if (error != 0) {
		kern_logf("visionfive2: no Ethernet (%d)\n", error);
		return;
	}

	/* Succeeded: the port is a network interface. */
	kern_logf("visionfive2: Ethernet is en0\n");
}

/*
 * Writes the SD card's partitions to the boot log: each one's number,
 * first block and size, its label when the table gives one, and whether
 * it is the EFI system partition.
 */
static void
describe_sd_partitions(
	void)
{
	const struct partition *entry;
	const char *label;
	const char *kind;
	int count;
	int index;

	/* A board without the card has nothing to list. */
	if (sd_card == NULL)
		return;

	/* Reads the card's partition table. */
	count = partition_scan(sd_card, sd_partitions, PARTITION_MAX);
	if (count < 0) {
		kern_logf("visionfive2: %s partition table unreadable (%d)\n", sd_card->d_name, count);
		return;
	}

	/* Writes how many partitions there are. */
	kern_logf("visionfive2: %s has %d partitions\n", sd_card->d_name, count);

	/* Writes each partition. */
	for (index = 0; index < count; index++) {
		/* Takes the partition's label, empty when the table gives none. */
		entry = &sd_partitions[index];
		label = "";
		if ((entry->p_flags & PARTITION_HAS_LABEL) != 0)
			label = entry->p_label;

		/* Marks the EFI system partition. */
		kind = "";
		if ((entry->p_flags & PARTITION_EFI_SYSTEM) != 0)
			kind = ", EFI system";

		/* Writes the partition's line. */
		kern_logf("visionfive2: %s partition %u start %llu blocks %llu (%llu MiB) \"%s\"%s\n",
			  sd_card->d_name,
			  entry->p_index + 1U,
			  (unsigned long long)entry->p_start_block,
			  (unsigned long long)entry->p_block_count,
			  (unsigned long long)(entry->p_block_count / 2048ULL),
			  label,
			  kind);
	}
}
#if CONFIG_DRIVER_POWERVR

/*
 * Reads the display controller the loader left on, from the tree's
 * enabled node when there is one and the board's description otherwise,
 * and has the display driver decide whether its screen can be taken over.
 *
 * The controller is read only when its power domain is on; reading a
 * powered-down block would stop the bus.
 */
static void
probe_display_controller(
	const struct drv_fdt *fdt)
{
	struct drv_powervr_dc_place place;
	const uint8_t *cells;
	uint32_t length;
	uint32_t node;
	struct kern_boot_framebuffer framebuffer;
	bool enabled;
	int powered;
	int error;

	/* Starts from the board's own description of the controller. */
	place = vf2_dc_builtin;

	/* Takes the windows and the interrupt from a node the tree enables, when there is one. */
	error = drv_fdt_find_compatible(fdt, "starfive,jh7110-dc8200", DRV_FDT_NO_NODE, &node);
	enabled = false;
	if (error == 0)
		enabled = drv_fdt_node_enabled(fdt, node);
	if (enabled) {
		error = drv_fdt_reg(fdt, node, 0, &place.control, &place.control_size);
		if (error == 0)
			error = drv_fdt_reg(fdt, node, 1, &place.display, &place.display_size);
		if (error != 0) {
			kern_logf("dc8200: no-go (unreadable device tree node, %d)\n", error);
			return;
		}

		error = drv_fdt_property(fdt, node, "interrupts", &cells, &length);
		if (error == 0 && length >= 4U)
			place.interrupt = (uint32_t)drv_fdt_cells_value(cells, 0, 1U);
	}

	/* Lights the output itself when the loader left no screen or the boot line names a mode. */
	if (drv_powervr_dc_own_wanted()) {
		light_display(&place);
		return;
	}

	/* Leaves the controller alone when its power domain is off or unknown. */
	error = drv_vf2_pmu_powered(DRV_VF2_PD_VOUT, &powered);
	if (error != 0) {
		kern_logf("dc8200: no-go (power domain unknown, %d)\n", error);
		return;
	}

	/*
	 * An off domain under a loader's screen means the loader tore the
	 * output down when the kernel took the machine: StarFive's U-Boot
	 * removes its DC8200 video device on ExitBootServices, clearing the
	 * planes, stopping the clocks and turning PD_VOUT off, so the handed
	 * framebuffer is no longer shown.
	 *
	 * Procedure referenced:
	 * StarFive U-Boot JH7110_VisionFive2_devel drivers/video/starfive/
	 * sf_vop.c (DM_FLAG_OS_PREPARE, the remove hook ending in the VOUT
	 * domain's power-off) and lib/efi_loader/efi_boottime.c
	 * (dm_remove_devices_flags(DM_REMOVE_ACTIVE_ALL) on exit).
	 */
	if (!powered) {
		if (kern_boot_framebuffer("riscv64.framebuffer", &framebuffer) == 0) {
			kern_logf("dc8200: no-go (power domain off, loader removed its output at exit)\n");
			light_display(&place);
			return;
		}
		kern_logf("dc8200: no-go (power domain off)\n");
		return;
	}

	/*
	 * Leaves the controller alone when a clock its registers need is
	 * stopped; a loader's screen whose clocks were stopped is lit again
	 * by the board.
	 */
	error = check_display_clocks();
	if (error != 0) {
		if (kern_boot_framebuffer("riscv64.framebuffer", &framebuffer) == 0)
			light_display(&place);
		return;
	}

	/* Has the display driver read the controller and log its decision. */
	error = drv_powervr_dc_probe(&place);
	if (error != 0 && error != ENOENT) {
		kern_logf("visionfive2: display controller unread (%d)\n", error);
		return;
	}

	/* Lets a fatal stop take the screen back once the controller is read. */
	if (error == 0)
		vf2_fatal_display = drv_powervr_dc_restore_fatal;
}

/*
 * Has the display driver light the HDMI output at its own mode.
 *
 * When the loader left a screen, the output is already powered and
 * clocked and is only set to the mode; otherwise the board powers it up
 * first.  Either way the plane becomes the screen a fatal stop returns to.
 */
static void
light_display(
	const struct drv_powervr_dc_place *place)
{
	struct drv_powervr_dc_own_place own;
	struct kern_boot_framebuffer framebuffer;
	int loader_screen;
	int powered;
	int error;

	own.controller = *place;
	own.hdmi = VF2_HDMI_BASE;
	own.hdmi_size = VF2_HDMI_SIZE;
	own.subsystem = 0;
	own.subsystem_size = 0;

	/*
	 * Finds whether the loader's output is still lit: a framebuffer was
	 * handed, the power domain is on and the registers' clocks run.  A
	 * loader that tore its output down at exit leaves the framebuffer's
	 * memory, which the driver shows again after powering the output.
	 */
	error = kern_boot_framebuffer("riscv64.framebuffer", &framebuffer);
	loader_screen = 0;
	if (error == 0)
		loader_screen = 1;
	if (loader_screen) {
		error = drv_vf2_pmu_powered(DRV_VF2_PD_VOUT, &powered);
		if (error != 0 || !powered)
			loader_screen = 0;
	}
	if (loader_screen) {
		error = check_display_clocks();
		if (error != 0)
			loader_screen = 0;
	}

	/* An unlit one is powered, clocked and released from reset first. */
	if (!loader_screen) {
		error = power_display();
		if (error != 0)
			return;
		own.subsystem = VF2_DSS_BASE;
		own.subsystem_size = VF2_DSS_SIZE;
	}

	/* Has the display driver set the mode and show its framebuffer. */
	error = drv_powervr_dc_own_mode(&own);
	if (error == ENODEV)
		describe_display_power();
	if (error != 0) {
		kern_logf("visionfive2: display not lit (%d)\n", error);
		return;
	}

	/* Lets a fatal stop take the screen back to the driver's framebuffer. */
	vf2_fatal_display = drv_powervr_dc_restore_fatal;
}

/*
 * Starts, in table order, the display gates of one clock generator.
 */
static int
start_display_gates(
	enum drv_vf2_crg crg)
{
	const struct vf2_dc_gate *gate;
	unsigned index;
	int error;

	/* Starts each of the generator's gates in order. */
	for (index = 0; index < sizeof(vf2_dc_start_gates) / sizeof(vf2_dc_start_gates[0]); index++) {
		gate = &vf2_dc_start_gates[index];
		if (gate->clock.crg != crg)
			continue;
		error = drv_vf2_clock_enable(&gate->clock);
		if (error != 0) {
			kern_logf("dc8200: own mode: clock %s not started (%d)\n", gate->name, error);
			return error;
		}
	}

	/* Succeeded: every gate of the generator is open. */
	return 0;
}

/*
 * Releases, in table order, the display reset lines of one clock
 * generator.
 */
static int
release_display_resets(
	enum drv_vf2_crg crg)
{
	const struct drv_vf2_reset *reset;
	unsigned index;
	int error;

	/* Releases each of the generator's lines in order. */
	for (index = 0; index < sizeof(vf2_dc_start_resets) / sizeof(vf2_dc_start_resets[0]); index++) {
		reset = &vf2_dc_start_resets[index];
		if (reset->crg != crg)
			continue;
		error = drv_vf2_reset_deassert(reset);
		if (error != 0) {
			kern_logf("dc8200: own mode: reset %u/%u not released (%d)\n",
				  (unsigned)reset->crg,
				  reset->index,
				  error);
			return error;
		}
	}

	/* Succeeded: every line of the generator is released. */
	return 0;
}

/*
 * Logs, on one line, each display gate's and reset line's state, after the
 * controller read back an unknown model: '+' open or released, '-' stopped
 * or held, '?' unreadable.
 */
static void
describe_display_power(
	void)
{
	char line[256];
	size_t used;
	unsigned index;
	int state;
	int error;
	char mark;

	used = 0;
	line[0] = '\0';

	/* Lists each gate. */
	for (index = 0; index < sizeof(vf2_dc_start_gates) / sizeof(vf2_dc_start_gates[0]); index++) {
		error = drv_vf2_clock_running(&vf2_dc_start_gates[index].clock, &state);
		mark = error != 0 ? '?' : (state ? '+' : '-');
		used += (size_t)kern_snprintf(line + used, sizeof(line) - used, " %s%c",
					 vf2_dc_start_gates[index].name, mark);
		if (used >= sizeof(line))
			break;
	}

	/* Lists each reset line. */
	for (index = 0; index < sizeof(vf2_dc_start_resets) / sizeof(vf2_dc_start_resets[0]) && used < sizeof(line); index++) {
		error = drv_vf2_reset_released(&vf2_dc_start_resets[index], &state);
		mark = error != 0 ? '?' : (state ? '+' : '-');
		used += (size_t)kern_snprintf(line + used, sizeof(line) - used, " rst%u/%u%c",
					 (unsigned)vf2_dc_start_resets[index].crg,
					 vf2_dc_start_resets[index].index, mark);
	}

	kern_logf("dc8200: display power:%s\n", line);
}

/*
 * Powers the display output up: its power domain, its clocks, the LCD
 * clock's source and its reset lines.
 *
 * The HDMI panel's pixel clock runs from the HDMI PHY's output (the VOUT
 * generator's hdmitx0_pixelclk input), as for display 0 in the vendor
 * driver; the PHY drives it once the HDMI PLL locks.
 *
 * Procedure referenced:
 * StarFive Linux JH7110_VisionFive2_6.12.y_devel 4cecf169f38e
 * drivers/gpu/drm/verisilicon/vs_dc.c (dc_init and vs_dc_enable: the
 * clocks enabled, the controller's resets released, the LCD clock on
 * display 0's pixel clock, display 0's pixel clock on hdmitx0_pixelclk)
 * and inno_hdmi.c
 * (inno_hdmi_enable_clk_deassert_rst).
 */
static int
power_display(
	void)
{
	struct drv_vf2_clock lcd;
	struct drv_vf2_clock pixel;
	int powered;
	int error;

	/* Turns the display's power domain on when it is off. */
	error = drv_vf2_pmu_powered(DRV_VF2_PD_VOUT, &powered);
	if (error != 0) {
		kern_logf("dc8200: own mode: power domain unknown (%d)\n", error);
		return error;
	}

	if (!powered) {
		error = drv_vf2_pmu_set_power(DRV_VF2_PD_VOUT, 1);
		if (error != 0) {
			kern_logf("dc8200: own mode: power domain not on (%d)\n", error);
			return error;
		}
	}

	/*
	 * Starts the SYS generator's display gates, then releases its display
	 * resets: SYSRST_VOUT_TOP_SRC holds the VOUT generator itself in
	 * reset, so its gates are opened only after that line is released, as
	 * in the vendor's U-Boot (sf_vop.c vout_probe_resources_jh7110).
	 */
	error = start_display_gates(DRV_VF2_CRG_SYS);
	if (error != 0)
		return error;
	error = release_display_resets(DRV_VF2_CRG_SYS);
	if (error != 0)
		return error;

	/* Starts the VOUT generator's gates once it is out of reset. */
	error = start_display_gates(DRV_VF2_CRG_VOUT);
	if (error != 0)
		return error;

	/* Runs the LCD clock from the HDMI panel's pixel clock. */
	lcd.crg = DRV_VF2_CRG_VOUT;
	lcd.index = 9U;
	pixel.crg = DRV_VF2_CRG_VOUT;
	pixel.index = 7U;
	error = drv_vf2_clock_set_parent(&lcd, &pixel);
	if (error != 0) {
		kern_logf("dc8200: own mode: LCD clock source not set (%d)\n", error);
		return error;
	}

	/* Runs the HDMI panel's pixel clock from the HDMI PHY's output. */
	error = drv_vf2_clock_set_input_parent(&pixel, "hdmitx0_pixelclk");
	if (error != 0) {
		kern_logf("dc8200: own mode: pixel clock source not set (%d)\n", error);
		return error;
	}

	/* Releases the VOUT generator's controller and HDMI lines. */
	error = release_display_resets(DRV_VF2_CRG_VOUT);
	if (error != 0)
		return error;

	/* Succeeded: the output is powered, clocked and out of reset. */
	return 0;
}

/*
 * Checks that every clock the display controller's registers need is
 * running, logging the first stopped one as the no-go reason.
 *
 * The SYS gates are read first, since the VOUT generator's own registers
 * are behind them.  A tree without the VOUT generator leaves its gates
 * unchecked, as before the generator was known.  EBUSY means a clock is
 * stopped; another error that a gate could not be read.
 */
static int
check_display_clocks(
	void)
{
	const struct vf2_dc_gate *gate;
	unsigned index;
	int running;
	int error;

	/* Reads each gate in bus order. */
	for (index = 0; index < sizeof(vf2_dc_gates) / sizeof(vf2_dc_gates[0]); index++) {
		/* Reads the gate's state. */
		gate = &vf2_dc_gates[index];
		error = drv_vf2_clock_running(&gate->clock, &running);

		/* Skips the VOUT generator's gates when the tree does not describe it. */
		if (error == ENODEV && gate->clock.crg == DRV_VF2_CRG_VOUT)
			continue;

		/* Refuses a gate that cannot be read. */
		if (error != 0) {
			kern_logf("dc8200: no-go (clock %s unknown, %d)\n", gate->name, error);
			return error;
		}

		/* Refuses a stopped gate: the controller's registers would not answer. */
		if (!running) {
			kern_logf("dc8200: no-go (clock %s stopped)\n", gate->name);
			return EBUSY;
		}
	}

	/* Succeeded: every gate the registers need is open. */
	return 0;
}
#endif

/*
 * Puts the display controller's plane back on the loader's screen for a
 * fatal stop, before the console draws its text there.  The console calls
 * it only while it has not left the screen.  Runs with interrupts
 * disabled.
 */
static void
reveal_fatal(
	void)
{
#if CONFIG_DRIVER_POWERVR
	/* Takes the screen back when the display driver read the controller. */
	if (vf2_fatal_display != NULL)
		vf2_fatal_display();
#endif
}

/*
 * Maps the UART /chosen/stdout-path names; kern_platform_input_init()
 * starts its input.  A tree without one leaves the console on the screen
 * alone.
 */
static void
probe_console_uart(
	const struct drv_fdt *fdt)
{
	int error;

	/* Finds and maps the firmware console's UART. */
	error = drv_ns16550_probe_console(fdt, &console_uart);
	if (error != 0) {
		kern_logf("visionfive2: no console UART (%d)\n", error);
		return;
	}

	/* Keeps it for the console's output and input. */
	console_uart_present = 1;
	kern_logf("visionfive2: console UART at %llx\n", (unsigned long long)console_uart.physical);
}

/*
 * Writes one character of the console to its UART, ending each line with
 * a carriage return as the firmware's console did.  Never sleeps.
 */
static void
console_uart_putc(
	int character)
{
	/* A new line returns the carriage first. */
	if (character == '\n')
		drv_ns16550_putc_polled(&console_uart, (uint8_t)'\r');

	/* The character itself. */
	drv_ns16550_putc_polled(&console_uart, (uint8_t)character);
}

/*
 * Publishes the console: the loader's screen, the UART, the text console
 * over both, and the screen's /dev/graphics backend.
 */
static void
start_console(
	void)
{
	struct kern_boot_framebuffer screen;
	int error;

	/* Gives the console the UART, when the tree named one. */
	if (console_uart_present)
		vf2_console_board.serial_write = console_uart_putc;

	/* Lets a fatal stop take the display back to the loader's screen. */
	vf2_console_board.reveal_fatal = reveal_fatal;

	/* Maps the loader's screen; without one the console is the UART alone. */
	error = kern_boot_framebuffer("riscv64.framebuffer", &screen);
	if (error == 0 && screen.size != 0U) {
		error = drv_boot_fb_init(&screen, 0U);
		if (error != 0)
			kern_logf("visionfive2: framebuffer not mapped (%d)\n", error);
	}

	/* Starts the text console over the screen and the UART. */
	drv_fb_text_init(&vf2_console_board);

	/* Offers the screen to /dev/graphics; without one there is no device. */
	error = drv_boot_fb_register_backend();
	if (error != 0 && error != ENODEV)
		kern_logf("graphics: loader framebuffer not offered (%d)\n", error);
}
