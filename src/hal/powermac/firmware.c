/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board's firmware: the Open Firmware client interface, asked once,
 * before the HAL takes the machine, for the RAM, the time base's rate, the
 * display and the command line.  The answers make the kernel's handoff
 * and the named objects of hal_get_arch_handoff(), among them the initial
 * ramdisk the firmware was handed.  The firmware is asked nothing but
 * where things are; it is told nothing, save two lines written through
 * its output, as the asking starts and as it ends, which a kernel that
 * stops before the HAL draws leaves on the screen.
 *
 * Values referenced: IEEE Std 1275-1994 (Open Firmware), the client
 * interface's argument array and the finddevice, getprop, child,
 * instance-to-package, package-to-path and write services; the PowerPC
 * binding (the display's width, height, linebytes, depth and address
 * properties, and /chosen's stdout, the instance the firmware writes its
 * text to).  The initial ramdisk
 * comes the way a Linux PowerPC kernel takes it: its address and size in
 * r3 and r4 at entry (QEMU's OpenBIOS passes the -initrd image so; a
 * size of 0 or 0xdeadbeef means none), or the linux,initrd-start and
 * linux,initrd-end properties of /chosen.
 *
 * The interrupt controller is the PCI host's interrupt-parent (else the
 * node under the I/O controller), at its reg offset into the I/O
 * controller's first assigned address.  A device's line is either its
 * own two-cell interrupts property (source and Apple's sense, as Apple's
 * firmware writes it) or its one-cell pin looked up in the host's
 * interrupt-map, whose entries are the child's three address cells, the
 * pin, the controller's phandle and the controller's cells (IEEE Std
 * 1275 PCI binding and the Open Firmware interrupt mapping
 * recommended practice).  Without a ramdisk the kernel came from a disk:
 * the partition is the number after the colon in /chosen's bootpath, an
 * entry of the disk's Apple Partition Map.
 *
 * The internal PCI host (pci@f4000000, where a PowerBook's Ethernet
 * controller and the ATA-6 controller sit) is read the same way as the
 * expansion bus's host: its reg, its first bus number (bus-range) and its
 * devices' lines.  The system controller (uni-n), whose register block
 * holds the clock gates of the devices on the internal host, is its reg.
 * A PCI node with a six-byte local-mac-address under either host is an
 * Ethernet controller whose address the firmware knows (Apple's trees
 * name the built-in GMAC so, and QEMU's OpenBIOS gives its sungem one).
 *
 * The RAM is the memory node's reg, one cell of address and one of size
 * a bank on these machines (the root's #address-cells and #size-cells);
 * a PowerBook lists its soldered memory and its slot as banks, and the
 * HAL takes the run of banks that follow each other from the first one's
 * start.  The device windows are the 256 MiB blocks that hold what the
 * kernel drives: the interrupt controller and the I/O controller, the
 * PCI host's registers, the display, and the PCI host's memory ranges
 * (its ranges entries whose phys.hi space code is 32-bit or 64-bit
 * memory, the PCI bus binding to IEEE Std 1275), whose BARs the
 * firmware assigned.  The power manager is the I/O controller's
 * via-pmu child (reg and interrupts) and the extint-gpio1 child of its
 * gpio node, the line the PMU pulls low while it has something to
 * report (Apple's device trees name them so; QEMU's OpenBIOS mirrors
 * them).
 *
 * The fan controller of a PowerBook or an iBook is a child named fan
 * (compatible adt7460, adt7467 or adm1030) of the system controller's I2C
 * controller (/uni-n's i2c child, compatible keywest-i2c): either directly
 * under it, its reg the controller's channel in bits 8 to 11 and the
 * device's 8-bit bus address in the low byte, or under one of its i2c-bus
 * children, whose reg is the channel.  The controller's registers are its
 * AAPL,address (else the first cell of its reg), AAPL,address-step bytes
 * apart (16 when it names none), and AAPL,i2c-rate is its bus's rate in
 * kHz.  The fan node's hwsensor-params-version and hwsensor-location (the
 * sensors' places, NUL-separated) are handed over as they are.
 */

#include <hal/hal.h>
#include <kern/boot.h>
#include "mac-arch.h"
#include "mac99.h"
#include "cons.h"
#include "framebuffer.h"

/* A missing node or property. */
#define FIRMWARE_FAILED		0xffffffffU

/* The most cells one call passes and returns. */
#define FIRMWARE_CELLS		8U

/* The most interrupt routes the firmware's tree is read for, and the interrupt-map cells read. */
#define ROUTES_MAX		16U
#define INTERRUPT_MAP_CELLS	(ROUTES_MAX * 7U)

/* The words of the handoff's route table: the count, the two masks, then four a route. */
#define ROUTE_TABLE_WORDS	(3U + ROUTES_MAX * 4U)

/* The PCI address bits of a device tree phys.hi cell: bus, device and function. */
#define PCI_ADDRESS_MASK	0x00ffff00U

/* The hosts an Ethernet record names: the expansion bus's and the internal one. */
#define PCI_HOST_EXPANSION	0U
#define PCI_HOST_INTERNAL	1U

/* The most Ethernet controllers described, and the words of each in the handoff. */
#define ETHERNET_MAX		2U
#define ETHERNET_RECORD_WORDS	4U

/* The length of an Ethernet address. */
#define ETHERNET_ADDRESS_BYTES	6U

/* The partition a disk boot assumes when the bootpath names none: the image tool's boot partition. */
#define BOOT_PARTITION_DEFAULT	2U

/* The memory node's reg cells read: eight banks of one address and one size cell. */
#define MEMORY_CELLS		16U

/* The PCI host's ranges cells read: eight entries of three child, one parent and two size cells. */
#define RANGES_CELLS		48U

/* A ranges entry's phys.hi space code: bits 24 and 25; 2 is 32-bit memory, 3 64-bit memory. */
#define PCI_SPACE_SHIFT		24U
#define PCI_SPACE_MASK		0x3U
#define PCI_SPACE_MEMORY32	0x2U
#define PCI_SPACE_MEMORY64	0x3U

/* The longest node name compared. */
#define NAME_SIZE		32U

/* The power manager's handoff: its registers, its interrupt, its GPIO line's register and interrupt. */
#define PMU_TABLE_WORDS		4U

/*
 * The fan controller's handoff: the words before the sensors' places, the
 * most bytes of those places kept, and the words of the whole table.
 */
#define FAN_TABLE_HEAD_WORDS	8U
#define FAN_LOCATION_BYTES	64U
#define FAN_TABLE_WORDS		(FAN_TABLE_HEAD_WORDS + FAN_LOCATION_BYTES / 4U)

/* The fan controllers the handoff names. */
#define FAN_CHIP_ADT7460	1U
#define FAN_CHIP_ADT7467	2U
#define FAN_CHIP_ADM1030	3U

/* The register step of an I2C controller whose node names none. */
#define I2C_STEP_DEFAULT	0x10U

/* A fan child's reg: the controller's channel in bits 8 to 11, the 8-bit bus address in the low byte. */
#define I2C_REG_CHANNEL_SHIFT	8U
#define I2C_REG_CHANNEL_MASK	0xfU
#define I2C_REG_ADDRESS_MASK	0xffU

/* The longest compatible property compared. */
#define COMPATIBLE_SIZE		64U

/*
 * The survey of the firmware's tree for the thermal parts: the room for
 * its lines, the deepest nesting walked, the most nodes looked at, the
 * levels under an I2C controller listed whole, and the longest path and
 * property read for a line.
 */
#define SURVEY_SIZE		3584U
#define SURVEY_DEPTH		12U
#define SURVEY_NODES		800U
#define SURVEY_I2C_LEVELS	3U
#define SURVEY_PATH_SIZE	96U
#define SURVEY_PROPERTY_SIZE	48U

/* The bootpath's longest form the firmware is asked for. */
#define BOOTPATH_SIZE		256U

/* The most ATA channels described, and the words of each in the handoff. */
#define ATA_CHANNELS_MAX	4U
#define ATA_RECORD_WORDS	4U

/* An ATA channel's boot drive word when the firmware did not boot from it. */
#define ATA_NOT_BOOT		0xffffffffU

/* One call's argument array: the service's name, the counts, then the cells. */
struct firmware_arguments {
	uint32_t service;
	uint32_t argument_count;
	uint32_t return_count;
	uint32_t cells[FIRMWARE_CELLS];
};

/* What the firmware reported. */
static struct mac99_board_info board;

/* The kernel's handoff and the command line, handed over as "boot.command-line". */
static struct kern_boot_handoff kernel_handoff;
static char boot_command_line[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

/* The initial ramdisk's physical address and size, handed over as "mac99.initrd". */
static uint64_t initrd_range[2];

/*
 * The PCI host bridge's register block (the first cell of its reg), handed
 * over as "mac99.pci.base"; zero when the firmware has no such node.
 */
static uint64_t pci_base;

/*
 * The devices' interrupt lines, from the firmware's tree, and how many;
 * the controller is set from them and they are handed over as
 * "mac99.pci.interrupts".
 */
static struct mac99_interrupt_route routes[ROUTES_MAX];
static unsigned route_count;

/*
 * The handoff's form of the routes: the count, the address mask and the
 * pin mask a device is matched under, then each route's address, pin,
 * source and sense.
 */
static uint32_t route_table[ROUTE_TABLE_WORDS];

/*
 * The internal PCI host, handed over as "mac99.pci.internal": its
 * register block and the number of its own bus (the first of its
 * bus-range); zero when the firmware has no such node.  Its devices'
 * lines, in the same form as route_table, are handed over as
 * "mac99.pci.internal.interrupts".
 */
static uint64_t pci_internal[2];
static uint32_t internal_route_table[ROUTE_TABLE_WORDS];

/* The internal PCI host's node; zero when there is none. */
static uint32_t pci_internal_host;

/*
 * How many cells name one of the interrupt controller's lines, found
 * with the controller from the expansion bus's host; 0 when there is no
 * controller.
 */
static uint32_t controller_cells;

/*
 * The system controller's register block (uni-n's reg), handed over as
 * "mac99.uni-n.base"; zero when the firmware has no such node.
 */
static uint64_t uni_n_base;

/*
 * The I/O controller's first assigned address, handed over as
 * "mac99.macio.base"; zero when there is none.
 */
static uint64_t macio_handoff;

/*
 * The Ethernet controllers whose address the firmware knows, handed over
 * as "mac99.ethernet": the count, then for each the host it is under
 * (PCI_HOST_EXPANSION or PCI_HOST_INTERNAL), its configuration address
 * (bus << 16 | device << 11 | function << 8), its address's first four
 * octets (the first in the top byte) and its last two (in the top half).
 */
static uint32_t ethernet_table[1U + ETHERNET_MAX * ETHERNET_RECORD_WORDS];

/*
 * The I/O controller's node and its first assigned address, found with
 * the interrupt controller under it; zero when there is none.
 */
static uint32_t macio_node;
static uint32_t macio_base;

/*
 * The ATA channels under the I/O controller, handed over as "mac99.ata":
 * the count, then for each its task file's and its DBDMA channel's
 * physical addresses, its interrupt source and the drive the firmware
 * booted from on it (ATA_NOT_BOOT when none).
 */
static uint32_t ata_table[1U + ATA_CHANNELS_MAX * ATA_RECORD_WORDS];

/* The firmware's bootpath, NUL-terminated; empty when it gave none. */
static char bootpath[BOOTPATH_SIZE];

/* The PCI host bridge's node the expansion bus hangs from; zero when there is none. */
static uint32_t pci_host;

/*
 * The power manager, handed over as "mac99.pmu": its VIA's registers,
 * the VIA's interrupt source, the GPIO register of the line it pulls low
 * to report (0 when none) and that line's interrupt source (0xffffffff
 * when none).
 */
static uint32_t pmu_table[PMU_TABLE_WORDS];

/*
 * The fan controller, handed over as "mac99.fan": its I2C controller's
 * registers, their step, the channel the controller drives it on, its
 * 7-bit bus address, which chip it is (FAN_CHIP_*), the bus's rate in kHz
 * (0 when the firmware names none), the node's hwsensor-params-version
 * (0 when missing) and the length of the sensors' places; then those
 * places, NUL-separated and NUL-terminated.  Filled once at boot; the
 * first word is 0 when the firmware's tree has no such fan controller.
 */
static uint32_t fan_table[FAN_TABLE_WORDS];

/*
 * The survey of the firmware's tree, handed over as "mac99.firmware-tree":
 * one line a node ("PPC32 OF" and the node's path, compatible, reg,
 * device_type and AAPL,address) for every node whose name speaks of I2C,
 * fans, temperatures, sensors or the power manager, and every node up to
 * SURVEY_I2C_LEVELS below an I2C node.  Filled once at boot and
 * NUL-terminated; empty when the firmware gave no tree.
 */
static char survey[SURVEY_SIZE];
static uint32_t survey_length;

/* Whether the survey ran out of room and ends with its mark. */
static bool survey_full;

static void firmware(const char *service, unsigned arguments, unsigned returns, const uint32_t *in, uint32_t *out);
static uint32_t finddevice(const char *path);
static uint32_t getprop(uint32_t node, const char *name, void *buffer, uint32_t length);
static uint32_t getprop_word(uint32_t node, const char *name);
static uint32_t getprop_first(uint32_t node, const char *name);
static void read_display(void);
static void keep_initrd(uint32_t start, uint32_t size);
static void read_initrd(uint32_t chosen);
static int name_is(const char *name, const char *wanted);
static void read_interrupts(uint32_t host, uint32_t *table);
static uint32_t read_openpic(uint32_t host, uint32_t *cells);
static void add_route(uint32_t *table, uint32_t address, uint32_t pin, uint32_t source, uint32_t sense);
static void read_internal_host(void);
static void read_ethernet(uint32_t host, uint32_t host_index);
static uint32_t read_boot_partition(void);
static void read_ata(void);
static uint32_t boot_drive_on(uint32_t channel);
static void read_memory(void);
static void read_windows(void);
static void add_window(uint32_t address);
static void add_host_windows(uint32_t host);
static void read_pmu(void);
static void read_fan(void);
static void record_fan(uint32_t controller, uint32_t channel, uint32_t node, uint32_t chip);
static uint32_t fan_chip(uint32_t node);
static int compatible_has(uint32_t node, const char *wanted);
static void survey_tree(void);
static int survey_name_matches(const char *name);
static int survey_text_has(const char *text, const char *word);
static void survey_node(uint32_t node);
static void survey_append(const char *text, uint32_t length);
static void survey_append_string(const char *text);
static void survey_append_hex(uint32_t value);
static uint32_t child_named(uint32_t parent, const char *wanted);
static uint32_t find_expansion_host(void);
static void firmware_mark(uint32_t chosen, const char *text);

/*
 * Asks the firmware what the HAL needs, and prepares the kernel's handoff.
 */
void
mac99_firmware_read(
	uint32_t initrd_address,
	uint32_t initrd_size)
{
	uint32_t node;
	uint32_t cells[2];
	uint32_t length;

	/* The command line first, which may turn the marks off; then the first mark. */
	node = finddevice("/chosen");
	if (node != FIRMWARE_FAILED) {
		length = getprop(node, "bootargs", boot_command_line, sizeof(boot_command_line) - 1U);
		if (length == FIRMWARE_FAILED)
			length = 0;
		if (length >= sizeof(boot_command_line))
			length = sizeof(boot_command_line) - 1U;
		boot_command_line[length] = '\0';
	}
	firmware_mark(node, "zedBSD: kernel entered, asking the firmware\r\n");

	/* The RAM banks, and the run of them from the first one's start. */
	read_memory();

	/* The time base's rate, from the first processor. */
	node = finddevice("/cpus");
	if (node != FIRMWARE_FAILED) {
		firmware("child", 1, 1, &node, cells);
		node = cells[0];
		if (node != FIRMWARE_FAILED && node != 0)
			board.timebase_frequency = getprop_word(node, "timebase-frequency");
	}

	/* The PCI host bridge the expansion bus hangs from, and its devices' interrupt lines. */
	node = find_expansion_host();
	if (node != FIRMWARE_FAILED && node != 0) {
		pci_host = node;
		length = getprop(node, "reg", cells, sizeof(cells));
		if (length != FIRMWARE_FAILED && length >= sizeof(cells))
			pci_base = cells[0];
		controller_cells = 2;
		board.openpic_base = read_openpic(node, &controller_cells);
		read_interrupts(node, route_table);
		read_ethernet(node, PCI_HOST_EXPANSION);
	}

	/* The internal PCI host, its devices' lines, and the system controller that gates their clocks. */
	read_internal_host();
	node = finddevice("/uni-n");
	if (node != FIRMWARE_FAILED && node != 0)
		uni_n_base = getprop_first(node, "reg");

	/* The display and the path the firmware booted from. */
	read_display();
	node = finddevice("/chosen");
	if (node != FIRMWARE_FAILED) {
		length = getprop(node, "bootpath", bootpath, sizeof(bootpath) - 1U);
		if (length == FIRMWARE_FAILED)
			length = 0;
		if (length >= sizeof(bootpath))
			length = sizeof(bootpath) - 1U;
		bootpath[length] = '\0';
	}

	/* The ATA channels under the I/O controller, and which drive the firmware booted from. */
	macio_handoff = macio_base;
	read_ata();

	/* The power manager under the I/O controller, the fan controller, and the device windows the kernel needs. */
	read_pmu();
	read_fan();
	survey_tree();
	read_windows();

	/* The initial ramdisk: the loader's registers, else /chosen's properties. */
	if (initrd_address != 0 && initrd_size != 0 && initrd_size != 0xdeadbeefU)
		keep_initrd(initrd_address, initrd_size);
	if (board.ramdisk_size == 0 && node != FIRMWARE_FAILED)
		read_initrd(node);

	/*
	 * The common handoff: one boot device.  A ramdisk is the device 0x80
	 * and its first MBR partition; otherwise the firmware read the kernel
	 * from a disk, the device 0x81, and the bootpath's Apple partition.
	 */
	hal_memset(&kernel_handoff, 0, sizeof(kernel_handoff));
	kernel_handoff.magic = KERN_HANDOFF_MAGIC;
	kernel_handoff.version = KERN_HANDOFF_VERSION_MULTIBOOT;
	kernel_handoff.size = sizeof(kernel_handoff);
	kernel_handoff.device_count = 1;
	kernel_handoff.boot_partition_lba = KERN_BOOT_PARTITION_LBA_UNKNOWN;
	if (board.ramdisk_size != 0) {
		kernel_handoff.boot_bios_id = MAC99_BOOT_ID_RAMDISK;
		kernel_handoff.boot_partition_scheme = KERN_PARTITION_SCHEME_MBR;
		kernel_handoff.boot_partition_index = 1;
	} else {
		kernel_handoff.boot_bios_id = MAC99_BOOT_ID_DISK;
		kernel_handoff.boot_partition_scheme = KERN_PARTITION_SCHEME_APM;
		kernel_handoff.boot_partition_index = read_boot_partition();
	}

	/* The last mark: the firmware is not asked again. */
	firmware_mark(node, "zedBSD: firmware read, taking the machine\r\n");
}

/*
 * Reports what the firmware said about the board.
 */
const struct mac99_board_info *
mac99_board(
	void)
{
	/* Succeeded: the record filled at boot. */
	return &board;
}

/*
 * Reports the devices' interrupt lines the firmware described.
 */
const struct mac99_interrupt_route *
mac99_interrupt_routes(
	unsigned *count)
{
	/* The routes read at boot, and how many. */
	*count = route_count;
	return routes;
}

/*
 * Reports the handoff the kernel's entry reads.
 */
const struct kern_boot_handoff *
mac99_kernel_handoff(
	void)
{
	/* Succeeded: the handoff filled at boot. */
	return &kernel_handoff;
}

/*
 * Returns one named architecture handoff object: "boot.command-line" is
 * the firmware's command line, "mac99.initrd" the initial ramdisk's
 * address and size, "mac99.pci.base" the PCI host bridge's register
 * block and "mac99.pci.interrupts" the devices' interrupt lines, each
 * when there is one, and "mac99.ata" the ATA channels under the I/O
 * controller (32-bit words: the count, then four a channel: the task
 * file's and the DBDMA channel's addresses, the interrupt source and the
 * drive booted from on it, or 0xffffffff), "mac99.pmu" the power manager
 * (32-bit words: its VIA's registers, the VIA's interrupt source, the
 * register of the GPIO line it pulls low to report, or 0, and that line's
 * interrupt source, or 0xffffffff), "mac99.console-log" what the early
 * console printed (a NUL-terminated string), "mac99.irq-watch" the
 * interrupt controller's counts (32-bit words: the number of count
 * slots, a count of takes each, then a bitmap of the sources masked for
 * a storm), "mac99.pc-samples" where the ticks found the processor
 * (32-bit words: the ring's size, the samples taken, then the ring; the
 * last two are live and change while they are read), "mac99.framebuffer"
 * the screen the early console drew on (32-bit words: the physical
 * address, the width, the height, the bytes a row and the bits a pixel)
 * and, for an 8-bit screen, "mac99.framebuffer.palette" the colours its
 * table is taken to show (256 words of 0xRRGGBB, index 0 first).  The
 * lines are 32-bit words: the count of routes, the mask a device's
 * address (bus << 16 | device << 11 | function << 8)
 * and the mask its pin is compared under, then each route's address,
 * pin (0 matches any), the interrupt controller's source (the HAL's
 * interrupt number) and Apple's sense cell.  "mac99.pci.internal" is the
 * internal PCI host (two 64-bit words: its register block and its bus's
 * number) and "mac99.pci.internal.interrupts" its devices' lines in the
 * same form, "mac99.uni-n.base" the system controller's register block
 * and "mac99.macio.base" the I/O controller's (each a 64-bit word), each
 * when there is one, and "mac99.ethernet" the Ethernet controllers whose
 * address the firmware knows (32-bit words: the count, then four a
 * controller: its host, 0 the expansion bus's and 1 the internal one, its
 * configuration address in the routes' form, its address's first four
 * octets with the first in the top byte, and its last two in the top
 * half).  "mac99.fan" is the fan controller on the system controller's
 * I2C bus, when the firmware's tree names one (32-bit words: the I2C
 * controller's registers, their step, the channel, the chip's 7-bit bus
 * address, the chip, 1 the ADT7460, 2 the ADT7467 and 3 the ADM1030, the bus's rate in
 * kHz or 0, the fan node's hwsensor-params-version or 0, and the length of
 * the sensors' places; then those places as bytes, NUL-separated and
 * NUL-terminated).  "mac99.firmware-tree" is the survey of the firmware's
 * tree for the thermal parts: NUL-terminated text, one line a node.
 */
void *
hal_get_arch_handoff(
	const char *name)
{
	int match;

	/* A missing name names nothing. */
	if (name == NULL)
		return NULL;

	/* The command line, when the firmware passed one. */
	match = name_is(name, "boot.command-line");
	if (match && boot_command_line[0] != '\0')
		return boot_command_line;

	/* The initial ramdisk's address and size, when there is one. */
	match = name_is(name, "mac99.initrd");
	if (match && board.ramdisk_size != 0)
		return initrd_range;

	/* The PCI host bridge's register block, when the firmware named one. */
	match = name_is(name, "mac99.pci.base");
	if (match && pci_base != 0)
		return &pci_base;

	/* The devices' interrupt lines, when the firmware described any. */
	match = name_is(name, "mac99.pci.interrupts");
	if (match && route_table[0] != 0)
		return route_table;

	/* The internal PCI host, when the firmware named one. */
	match = name_is(name, "mac99.pci.internal");
	if (match && pci_internal[0] != 0)
		return pci_internal;

	/* The internal host's devices' lines, when the firmware described any. */
	match = name_is(name, "mac99.pci.internal.interrupts");
	if (match && internal_route_table[0] != 0)
		return internal_route_table;

	/* The system controller's registers, when the firmware named it. */
	match = name_is(name, "mac99.uni-n.base");
	if (match && uni_n_base != 0)
		return &uni_n_base;

	/* The I/O controller's registers, when the firmware named it. */
	match = name_is(name, "mac99.macio.base");
	if (match && macio_handoff != 0)
		return &macio_handoff;

	/* The Ethernet controllers whose address the firmware knows, when there are any. */
	match = name_is(name, "mac99.ethernet");
	if (match && ethernet_table[0] != 0)
		return ethernet_table;

	/* The ATA channels, when the firmware described any. */
	match = name_is(name, "mac99.ata");
	if (match && ata_table[0] != 0)
		return ata_table;

	/* The power manager, when the firmware described one. */
	match = name_is(name, "mac99.pmu");
	if (match && pmu_table[0] != 0)
		return pmu_table;

	/* The fan controller, when the firmware described one. */
	match = name_is(name, "mac99.fan");
	if (match && fan_table[0] != 0)
		return fan_table;

	/* The survey of the thermal parts of the firmware's tree, when it found any. */
	match = name_is(name, "mac99.firmware-tree");
	if (match && survey_length != 0)
		return survey;

	/* What the early console printed, the hardware layer's lines among it. */
	match = name_is(name, "mac99.console-log");
	if (match)
		return (void *)(uintptr_t)virt_cons_log();

	/* The interrupt counts and the storm bitmap. */
	match = name_is(name, "mac99.irq-watch");
	if (match)
		return (void *)(uintptr_t)mac99_irq_watch();

	/* The ticks' ring of interrupted addresses. */
	match = name_is(name, "mac99.pc-samples");
	if (match)
		return (void *)(uintptr_t)mac99_clock_samples();

	/* The screen the early console drew on, when it took one. */
	match = name_is(name, "mac99.framebuffer");
	if (match)
		return (void *)(uintptr_t)virt_framebuffer_handoff();

	/* The colours of an 8-bit screen's table. */
	match = name_is(name, "mac99.framebuffer.palette");
	if (match)
		return (void *)(uintptr_t)virt_framebuffer_palette();

	/* An unknown handoff. */
	return NULL;
}

/*
 * Tells whether the firmware's command line holds a word, between spaces
 * or at either end.
 */
bool
mac99_boot_option(
	const char *word)
{
	size_t start;
	size_t index;

	/* Each word of the line, compared with the one asked for. */
	start = 0;
	while (boot_command_line[start] != '\0') {
		while (boot_command_line[start] == ' ')
			start++;
		for (index = 0; word[index] != '\0' && boot_command_line[start + index] == word[index]; index++)
			continue;
		if (word[index] == '\0' && (boot_command_line[start + index] == ' ' || boot_command_line[start + index] == '\0'))
			return true;
		while (boot_command_line[start] != '\0' && boot_command_line[start] != ' ')
			start++;
	}

	/* The line does not hold it. */
	return false;
}

/* Calls a client interface service. */
static void
firmware(
	const char *service,
	unsigned arguments,
	unsigned returns,
	const uint32_t *in,
	uint32_t *out)
{
	struct firmware_arguments call;
	unsigned index;

	/* The array: the name, the counts, the arguments, room for the returns. */
	hal_memset(&call, 0, sizeof(call));
	call.service = (uint32_t)(uintptr_t)service;
	call.argument_count = arguments;
	call.return_count = returns;
	for (index = 0; index < arguments; index++)
		call.cells[index] = in[index];

	/* The firmware answers in place. */
	mac_firmware_call(&call);
	for (index = 0; index < returns; index++)
		out[index] = call.cells[arguments + index];
}

/*
 * Writes a line through the firmware's output (/chosen's stdout), unless
 * the command line holds mac99.ofmark=off: on a machine whose kernel stops
 * before the HAL draws, the line shows how far the boot came.
 */
static void
firmware_mark(
	uint32_t chosen,
	const char *text)
{
	uint32_t in[3];
	uint32_t written;

	/* Nothing without /chosen or its output, or when turned off. */
	if (chosen == FIRMWARE_FAILED || chosen == 0)
		return;
	if (mac99_boot_option("mac99.ofmark=off"))
		return;
	in[0] = getprop_word(chosen, "stdout");
	if (in[0] == 0)
		return;

	/* The output instance, the text and its length. */
	in[1] = (uint32_t)(uintptr_t)text;
	in[2] = (uint32_t)hal_strlen(text);
	written = 0;
	firmware("write", 3, 1, in, &written);
}

/* Finds a node by its path or alias. */
static uint32_t
finddevice(
	const char *path)
{
	uint32_t in;
	uint32_t out;

	/* The node's handle, or the failure value. */
	in = (uint32_t)(uintptr_t)path;
	firmware("finddevice", 1, 1, &in, &out);
	return out;
}

/* Reads a property into a buffer; reports its size, or the failure value. */
static uint32_t
getprop(
	uint32_t node,
	const char *name,
	void *buffer,
	uint32_t length)
{
	uint32_t in[4];
	uint32_t out;

	/* The node, the name, the buffer and its length. */
	in[0] = node;
	in[1] = (uint32_t)(uintptr_t)name;
	in[2] = (uint32_t)(uintptr_t)buffer;
	in[3] = length;
	firmware("getprop", 4, 1, in, &out);
	return out;
}

/* Reads a one-cell property, zero when it is missing. */
static uint32_t
getprop_word(
	uint32_t node,
	const char *name)
{
	uint32_t value;
	uint32_t length;

	/* A property shorter than a cell counts as missing. */
	value = 0;
	length = getprop(node, name, &value, sizeof(value));
	if (length != sizeof(value))
		return 0;

	/* Succeeded: the cell. */
	return value;
}

/* Reads the first cell of a property of one cell or more, zero when it is missing. */
static uint32_t
getprop_first(
	uint32_t node,
	const char *name)
{
	uint32_t cells[8];
	uint32_t length;

	/* A property shorter than a cell counts as missing. */
	length = getprop(node, name, cells, sizeof(cells));
	if (length == FIRMWARE_FAILED || length < sizeof(cells[0]))
		return 0;

	/* Succeeded: the first cell. */
	return cells[0];
}

/*
 * Reads the display's address and shape: the screen alias's node, or the
 * node behind the firmware's output.
 */
static void
read_display(
	void)
{
	uint32_t node;
	uint32_t chosen;
	uint32_t instance;

	/* The screen alias first. */
	node = finddevice("screen");
	if (node == FIRMWARE_FAILED || node == 0) {
		chosen = finddevice("/chosen");
		if (chosen == FIRMWARE_FAILED)
			return;
		instance = getprop_word(chosen, "stdout");
		if (instance == 0)
			return;
		firmware("instance-to-package", 1, 1, &instance, &node);
		if (node == FIRMWARE_FAILED || node == 0)
			return;
	}

	/* The properties the display's driver published. */
	board.framebuffer_address = getprop_word(node, "address");
	board.framebuffer_width = getprop_word(node, "width");
	board.framebuffer_height = getprop_word(node, "height");
	board.framebuffer_linebytes = getprop_word(node, "linebytes");
	board.framebuffer_depth = getprop_word(node, "depth");
}

/*
 * Keeps the initial ramdisk's range.  A range outside the RAM bank is
 * ignored, so no page of it is kept.
 */
static void
keep_initrd(
	uint32_t start,
	uint32_t size)
{
	uint64_t end;
	uint64_t memory_end;

	/* Keeps only a range inside the RAM bank. */
	end = (uint64_t)start + size;
	memory_end = (uint64_t)board.memory_base + board.memory_size;
	if (start < board.memory_base || end > memory_end)
		return;

	/* Succeeded: the range, for the page allocator and the platform. */
	board.ramdisk_base = start;
	board.ramdisk_size = size;
	initrd_range[0] = start;
	initrd_range[1] = size;
}

/*
 * Reads the initial ramdisk's range from /chosen; an empty or reversed
 * range is ignored.
 */
static void
read_initrd(
	uint32_t chosen)
{
	uint32_t start;
	uint32_t end;

	/* Both ends, one cell each. */
	start = getprop_word(chosen, "linux,initrd-start");
	end = getprop_word(chosen, "linux,initrd-end");
	if (start == 0 || end <= start)
		return;

	/* Succeeded: the range, when it lies in the RAM bank. */
	keep_initrd(start, end - start);
}

/* Tells whether a handoff name is the one wanted. */
static int
name_is(
	const char *name,
	const char *wanted)
{
	size_t index;

	/* Compares character by character, then requires the same end. */
	for (index = 0; wanted[index] != '\0'; index++) {
		if (name[index] != wanted[index])
			return 0;
	}

	/* The names are the same only when the name ends there too. */
	if (name[index] != '\0')
		return 0;
	return 1;
}

/*
 * Reads a PCI host's devices' lines into its handoff table: the host's
 * interrupt-map, and each child's own two-cell interrupts.  The
 * controller must have been found first (read_openpic()).
 */
static void
read_interrupts(
	uint32_t host,
	uint32_t *table)
{
	uint32_t map[INTERRUPT_MAP_CELLS];
	uint32_t mask[4];
	uint32_t cells[4];
	uint32_t entry_cells;
	uint32_t length;
	uint32_t child;
	uint32_t sense;
	unsigned index;

	/* Without a controller whose lines the firmware names in one or two cells there is nothing to route to. */
	if (controller_cells == 0 || controller_cells > 2U)
		return;

	/* The masks a device's address and pin are matched under; without a map, the whole address. */
	table[1] = PCI_ADDRESS_MASK;
	table[2] = 0;
	length = getprop(host, "interrupt-map-mask", mask, sizeof(mask));
	if (length != FIRMWARE_FAILED && length >= sizeof(mask)) {
		table[1] = mask[0] & PCI_ADDRESS_MASK;
		table[2] = mask[3];
	}

	/* The map's entries: three address cells, the pin, the controller's phandle and its cells. */
	entry_cells = 5U + controller_cells;
	length = getprop(host, "interrupt-map", map, sizeof(map));
	if (length != FIRMWARE_FAILED && length <= sizeof(map)) {
		for (index = 0; (index + 1U) * entry_cells * 4U <= length; index++) {
			sense = 0;
			if (controller_cells > 1U)
				sense = map[index * entry_cells + 6U];
			add_route(table,
				  map[index * entry_cells] & table[1],
				  map[index * entry_cells + 3U] & table[2],
				  map[index * entry_cells + 5U],
				  sense);
		}
	}

	/* Each device with a line of its own: two cells, the source and the sense. */
	firmware("child", 1, 1, &host, &child);
	while (child != 0 && child != FIRMWARE_FAILED) {
		length = getprop(child, "interrupts", cells, sizeof(cells));
		if (length != FIRMWARE_FAILED && length >= 8U) {
			length = getprop(child, "reg", mask, sizeof(mask));
			if (length != FIRMWARE_FAILED && length >= 4U)
				add_route(table, mask[0] & table[1], 0, cells[0], cells[1]);
		}
		firmware("peer", 1, 1, &child, &child);
	}
}

/*
 * Finds the interrupt controller: its node's reg offset into the I/O
 * controller's first assigned address.  Reports the address, 0 when
 * there is none, and how many cells name one of its lines.
 */
static uint32_t
read_openpic(
	uint32_t host,
	uint32_t *cells)
{
	uint32_t assigned[5];
	uint32_t offset;
	uint32_t controller;
	uint32_t parent;
	uint32_t length;

	/* The host's interrupt-parent, else the node under the I/O controller. */
	controller = getprop_word(host, "interrupt-parent");
	if (controller == 0) {
		controller = finddevice("/pci@f2000000/mac-io/interrupt-controller");
		if (controller == FIRMWARE_FAILED)
			controller = finddevice("mac-io/interrupt-controller");
		if (controller == FIRMWARE_FAILED)
			return 0;
	}

	/* How many cells name one of its lines. */
	*cells = getprop_word(controller, "#interrupt-cells");

	/* Its offset into the I/O controller: the first cell of its reg. */
	offset = getprop_first(controller, "reg");

	/* The I/O controller's first assigned address: phys.hi, phys.mid, phys.lo, then the size. */
	firmware("parent", 1, 1, &controller, &parent);
	if (parent == 0 || parent == FIRMWARE_FAILED)
		return 0;
	length = getprop(parent, "assigned-addresses", assigned, sizeof(assigned));
	if (length == FIRMWARE_FAILED || length < sizeof(assigned))
		return 0;
	macio_node = parent;
	macio_base = assigned[2];

	/* Succeeded: the controller's address. */
	return assigned[2] + offset;
}

/*
 * Records one route of a host, while there is room: in the routes the
 * controller's triggers are set from, and in the host's handoff table,
 * whose first word counts them.
 */
static void
add_route(
	uint32_t *table,
	uint32_t address,
	uint32_t pin,
	uint32_t source,
	uint32_t sense)
{
	uint32_t count;

	/* A full table keeps the routes it has. */
	if (route_count == ROUTES_MAX)
		return;

	/* The route for the controller's triggers. */
	routes[route_count].address = address;
	routes[route_count].pin = pin;
	routes[route_count].source = source;
	routes[route_count].sense = sense;
	route_count++;

	/* Its copy in the host's handoff table. */
	count = table[0];
	table[3U + count * 4U] = address;
	table[4U + count * 4U] = pin;
	table[5U + count * 4U] = source;
	table[6U + count * 4U] = sense;
	table[0] = count + 1U;
}

/*
 * Reads the internal PCI host: its register block, its bus's number,
 * its devices' lines and its Ethernet controller's address.
 */
static void
read_internal_host(
	void)
{
	uint32_t cells[2];
	uint32_t node;
	uint32_t length;

	/* A board without the internal host has nothing more to read. */
	node = finddevice("/pci@f4000000");
	if (node == FIRMWARE_FAILED || node == 0)
		return;
	length = getprop(node, "reg", cells, sizeof(cells));
	if (length == FIRMWARE_FAILED || length < sizeof(cells))
		return;
	pci_internal_host = node;

	/* The register block, and the first number of the host's bus-range (0 when it names none). */
	pci_internal[0] = cells[0];
	pci_internal[1] = 0;
	length = getprop(node, "bus-range", cells, sizeof(cells));
	if (length != FIRMWARE_FAILED && length >= 4U)
		pci_internal[1] = cells[0];

	/* Its devices' lines, and the address of an Ethernet controller among them. */
	read_interrupts(node, internal_route_table);
	read_ethernet(node, PCI_HOST_INTERNAL);
}

/*
 * Records the children of a PCI host that carry a six-byte
 * local-mac-address: an Ethernet controller and the address the
 * firmware gives it.
 */
static void
read_ethernet(
	uint32_t host,
	uint32_t host_index)
{
	uint8_t address[8];
	uint32_t reg[5];
	uint32_t child;
	uint32_t length;
	uint32_t *record;

	/* Each child with an address and a configuration address, while there is room. */
	firmware("child", 1, 1, &host, &child);
	while (child != 0 && child != FIRMWARE_FAILED && ethernet_table[0] < ETHERNET_MAX) {
		length = getprop(child, "local-mac-address", address, sizeof(address));
		if (length == ETHERNET_ADDRESS_BYTES) {
			length = getprop(child, "reg", reg, sizeof(reg));
			if (length != FIRMWARE_FAILED && length >= 4U) {
				/* The host, the configuration address, then the octets, the first in the top byte. */
				record = &ethernet_table[1U + ethernet_table[0] * ETHERNET_RECORD_WORDS];
				record[0] = host_index;
				record[1] = reg[0] & PCI_ADDRESS_MASK;
				record[2] = ((uint32_t)address[0] << 24) |
					    ((uint32_t)address[1] << 16) |
					    ((uint32_t)address[2] << 8) |
					    (uint32_t)address[3];
				record[3] = ((uint32_t)address[4] << 24) |
					    ((uint32_t)address[5] << 16);
				ethernet_table[0]++;
			}
		}
		firmware("peer", 1, 1, &child, &child);
	}
}

/*
 * Reads the partition the firmware booted from: the number after the
 * last colon of the bootpath ("...disk@0:2,\\vmunix" is 2), or
 * BOOT_PARTITION_DEFAULT when it names none.
 */
static uint32_t
read_boot_partition(
	void)
{
	uint32_t index;
	uint32_t colon;
	uint32_t number;

	/* The last colon. */
	colon = BOOTPATH_SIZE;
	for (index = 0; index < BOOTPATH_SIZE && bootpath[index] != '\0'; index++) {
		if (bootpath[index] == ':')
			colon = index;
	}
	if (colon == BOOTPATH_SIZE)
		return BOOT_PARTITION_DEFAULT;

	/* The decimal number after it; none is the default. */
	number = 0;
	for (index = colon + 1U; index < BOOTPATH_SIZE && bootpath[index] >= '0' && bootpath[index] <= '9'; index++)
		number = number * 10U + (uint32_t)(bootpath[index] - '0');
	if (number == 0 || number > 64U)
		return BOOT_PARTITION_DEFAULT;

	/* Succeeded: the partition's number. */
	return number;
}

/*
 * Reads the ATA channels under the I/O controller: each child whose
 * device_type is "ata", its reg's two ranges (the task file and the
 * DBDMA channel) into the controller, and its first interrupt.
 */
static void
read_ata(
	void)
{
	uint32_t cells[4];
	char type[8];
	uint32_t child;
	uint32_t length;
	uint32_t *record;

	/* Without the I/O controller there are no channels. */
	if (macio_node == 0)
		return;

	/* Each child of the I/O controller that is an ATA channel. */
	firmware("child", 1, 1, &macio_node, &child);
	while (child != 0 && child != FIRMWARE_FAILED && ata_table[0] < ATA_CHANNELS_MAX) {
		length = getprop(child, "device_type", type, sizeof(type));
		if (length == 4U &&
		    type[0] == 'a' &&
		    type[1] == 't' &&
		    type[2] == 'a' &&
		    type[3] == '\0') {
			length = getprop(child, "reg", cells, sizeof(cells));
			if (length != FIRMWARE_FAILED && length >= 8U) {
				record = &ata_table[1U + ata_table[0] * ATA_RECORD_WORDS];
				record[0] = macio_base + cells[0];
				record[1] = 0;
				if (length >= sizeof(cells))
					record[1] = macio_base + cells[2];
				record[2] = getprop_first(child, "interrupts");
				record[3] = boot_drive_on(child);
				ata_table[0]++;
			}
		}
		firmware("peer", 1, 1, &child, &child);
	}
}

/*
 * Tells which drive of an ATA channel the bootpath names: the unit after
 * the channel's own path ("<channel>/disk@1:..." is 1), or ATA_NOT_BOOT
 * when the bootpath is not under the channel.
 */
static uint32_t
boot_drive_on(
	uint32_t channel)
{
	char path[BOOTPATH_SIZE];
	uint32_t in[3];
	uint32_t length;
	uint32_t index;
	uint32_t drive;

	/* The channel's full path. */
	in[0] = channel;
	in[1] = (uint32_t)(uintptr_t)path;
	in[2] = sizeof(path) - 1U;
	firmware("package-to-path", 3, 1, in, &length);
	if (length == FIRMWARE_FAILED || length == 0 || length >= sizeof(path))
		return ATA_NOT_BOOT;
	path[length] = '\0';

	/* The bootpath must start with it and go on to a child. */
	for (index = 0; index < length; index++) {
		if (bootpath[index] != path[index])
			return ATA_NOT_BOOT;
	}
	if (bootpath[length] != '/')
		return ATA_NOT_BOOT;

	/* The child's unit, after its '@'. */
	for (index = length + 1U; index < BOOTPATH_SIZE && bootpath[index] != '\0' && bootpath[index] != '@'; index++)
		continue;
	if (index >= BOOTPATH_SIZE || bootpath[index] != '@')
		return 0;
	drive = 0;
	for (index++; index < BOOTPATH_SIZE && bootpath[index] >= '0' && bootpath[index] <= '9'; index++)
		drive = drive * 10U + (uint32_t)(bootpath[index] - '0');

	/* Succeeded: the drive. */
	return drive;
}

/*
 * Reads the RAM banks of the memory node, and takes the run of them that
 * follow each other from the first bank's start; a bank after a gap is
 * counted in the total but not used.
 */
static void
read_memory(
	void)
{
	uint32_t cells[MEMORY_CELLS];
	uint32_t node;
	uint32_t length;
	uint32_t address_cells;
	uint32_t size_cells;
	uint32_t entry;
	uint32_t count;
	uint32_t index;
	uint32_t value;
	uint64_t end;
	bool grew;

	/* The root's cells: one of address and one of size unless it says otherwise. */
	node = finddevice("/");
	address_cells = 1;
	size_cells = 1;
	if (node != FIRMWARE_FAILED) {
		value = getprop_word(node, "#address-cells");
		if (value != 0)
			address_cells = value;
		value = getprop_word(node, "#size-cells");
		if (value != 0)
			size_cells = value;
	}
	entry = address_cells + size_cells;
	if (address_cells > 2U || size_cells > 2U)
		return;

	/* The banks; a 32-bit machine's addresses and sizes are the last cell of each. */
	node = finddevice("/memory");
	if (node == FIRMWARE_FAILED)
		return;
	length = getprop(node, "reg", cells, sizeof(cells));
	if (length == FIRMWARE_FAILED || length < entry * 4U)
		return;
	if (length > sizeof(cells))
		length = sizeof(cells);
	count = length / (entry * 4U);
	board.memory_base = cells[address_cells - 1U];
	end = board.memory_base;
	for (index = 0; index < count; index++)
		board.memory_total += cells[index * entry + entry - 1U];

	/* The run from the first bank: each bank that starts where the run ends lengthens it. */
	do {
		grew = false;
		for (index = 0; index < count; index++) {
			if (cells[index * entry + entry - 1U] == 0)
				continue;
			if (cells[index * entry + address_cells - 1U] != end)
				continue;
			end += cells[index * entry + entry - 1U];
			grew = true;
		}
	} while (grew && end < 0x100000000ULL);
	if (end > 0xffffffffULL)
		end = 0xfffff000ULL;
	board.memory_size = (uint32_t)(end - board.memory_base);
}

/*
 * Lists the device windows the kernel needs, most needed first: the
 * interrupt controller's (with the I/O controller's devices), the PCI
 * host's registers', the display's, the PCI host's memory ranges', then
 * the internal PCI host's, the system controller's and the fan
 * controller's I2C controller's.
 */
static void
read_windows(
	void)
{
	/* The controllers and the display. */
	add_window(board.openpic_base);
	add_window((uint32_t)pci_base);
	add_window(board.framebuffer_address);

	/* The expansion bus's host's memory ranges, then the internal host's block and ranges, and the system controller. */
	add_host_windows(pci_host);
	add_window((uint32_t)pci_internal[0]);
	add_host_windows(pci_internal_host);
	add_window((uint32_t)uni_n_base);

	/* The fan controller's I2C controller, which sits beside the system controller. */
	add_window(fan_table[0]);
}

/*
 * Adds the blocks a PCI host's memory ranges reach to the device windows;
 * a missing host (0) adds nothing.
 */
static void
add_host_windows(
	uint32_t host)
{
	uint32_t ranges[RANGES_CELLS];
	uint32_t length;
	uint32_t index;
	uint32_t space;
	uint64_t address;
	uint64_t size;
	uint64_t end;

	/* Nothing without a host. */
	if (host == 0)
		return;

	/* The host's memory ranges: three child cells, one parent cell, two size cells an entry. */
	length = getprop(host, "ranges", ranges, sizeof(ranges));
	if (length == FIRMWARE_FAILED)
		return;
	if (length > sizeof(ranges))
		length = sizeof(ranges);
	for (index = 0; (index + 1U) * 6U * 4U <= length; index++) {
		space = (ranges[index * 6U] >> PCI_SPACE_SHIFT) & PCI_SPACE_MASK;
		if (space != PCI_SPACE_MEMORY32 && space != PCI_SPACE_MEMORY64)
			continue;
		address = ranges[index * 6U + 3U];
		size = ((uint64_t)ranges[index * 6U + 4U] << 32) | ranges[index * 6U + 5U];
		if (size == 0)
			continue;
		end = address + size;
		if (end > 0x100000000ULL)
			end = 0x100000000ULL;

		/* Each block the range reaches. */
		for (; address < end; address = (address & MAC99_WINDOW_MASK) + 0x10000000ULL)
			add_window((uint32_t)address);
	}
}

/* Adds the block that holds an address to the device windows, once, while there is room. */
static void
add_window(
	uint32_t address)
{
	uint32_t block;
	uint32_t index;

	/* Nothing at 0 is a device the firmware named. */
	if (address == 0)
		return;
	block = address & MAC99_WINDOW_MASK;

	/* A block listed already, or no room, adds nothing. */
	for (index = 0; index < board.window_count; index++) {
		if (board.windows[index] == block)
			return;
	}
	if (board.window_count == MAC99_WINDOWS_MAX)
		return;

	/* Succeeded: the block is wanted after those before it. */
	board.windows[board.window_count] = block;
	board.window_count++;
}

/*
 * Reads the power manager under the I/O controller: the via-pmu node's
 * registers and interrupt, and the gpio node's extint-gpio1 line.
 */
static void
read_pmu(
	void)
{
	uint32_t cells[2];
	uint32_t node;
	uint32_t gpio;
	uint32_t length;
	uint32_t offset;
	uint32_t gpio_offset;

	/* Without the I/O controller there is no power manager to reach. */
	pmu_table[3] = 0xffffffffU;
	if (macio_node == 0)
		return;

	/* The VIA the power manager talks through: its offset into the I/O controller and its interrupt. */
	node = child_named(macio_node, "via-pmu");
	if (node == 0)
		return;
	offset = getprop_first(node, "reg");
	if (offset == 0)
		return;
	board.pmu_base = macio_base + offset;
	board.pmu_interrupt = getprop_first(node, "interrupts");

	/*
	 * The line it pulls low to report: extint-gpio1, whose reg is an
	 * offset into the gpio node's (an offset below the gpio node's own
	 * is taken as relative to it).
	 */
	board.pmu_gpio_interrupt = 0xffffffffU;
	gpio = child_named(macio_node, "gpio");
	node = 0;
	if (gpio != 0)
		node = child_named(gpio, "extint-gpio1");
	if (node != 0) {
		offset = getprop_first(node, "reg");
		gpio_offset = getprop_first(gpio, "reg");
		if (offset < gpio_offset)
			offset += gpio_offset;
		length = getprop(node, "interrupts", cells, sizeof(cells));
		if (offset != 0 && length != FIRMWARE_FAILED && length >= 4U) {
			board.pmu_gpio = macio_base + offset;
			board.pmu_gpio_interrupt = cells[0];
			if (length >= sizeof(cells))
				board.pmu_gpio_sense = cells[1];
		}
	}

	/* The handoff's copy. */
	pmu_table[0] = board.pmu_base;
	pmu_table[1] = board.pmu_interrupt;
	pmu_table[2] = board.pmu_gpio;
	pmu_table[3] = board.pmu_gpio_interrupt;
}

/*
 * Reads the fan controller on the system controller's I2C controller: the
 * controller's registers, step and rate, the channel and bus address the
 * fan node names, and the node's sensor parameters.  Only an I2C
 * controller the firmware calls keywest-i2c, and only a fan node whose
 * compatible names a chip the kernel knows, are handed over.
 */
static void
read_fan(
	void)
{
	char name[NAME_SIZE];
	uint32_t uni_n;
	uint32_t controller;
	uint32_t child;
	uint32_t grandchild;
	uint32_t channel;
	uint32_t length;
	uint32_t chip;
	int match;

	/* The system controller's I2C controller; a board without one has no fan controller to hand over. */
	uni_n = finddevice("/uni-n");
	if (uni_n == FIRMWARE_FAILED || uni_n == 0)
		return;
	controller = child_named(uni_n, "i2c");
	if (controller == 0)
		return;
	match = compatible_has(controller, "keywest-i2c");
	if (!match)
		return;

	/* Each child: a fan node itself, or a bus node whose children may hold one. */
	firmware("child", 1, 1, &controller, &child);
	while (child != 0 && child != FIRMWARE_FAILED) {
		length = getprop(child, "name", name, sizeof(name) - 1U);
		if (length == FIRMWARE_FAILED || length >= sizeof(name))
			length = 0;
		name[length] = '\0';

		/* A fan node right under the controller: its reg holds the channel too. */
		match = name_is(name, "fan");
		if (match) {
			chip = fan_chip(child);
			if (chip != 0) {
				channel = (getprop_first(child, "reg") >> I2C_REG_CHANNEL_SHIFT) & I2C_REG_CHANNEL_MASK;
				record_fan(controller, channel, child, chip);
				return;
			}
		}

		/* A bus node: its reg is the channel, and the fan node is its child. */
		match = name_is(name, "i2c-bus");
		if (match) {
			grandchild = child_named(child, "fan");
			if (grandchild != 0) {
				chip = fan_chip(grandchild);
				if (chip != 0) {
					channel = getprop_first(child, "reg");
					record_fan(controller, channel, grandchild, chip);
					return;
				}
			}
		}

		firmware("peer", 1, 1, &child, &child);
	}
}

/*
 * Fills the fan controller's handoff from its I2C controller's node, the
 * channel, and the fan node.
 */
static void
record_fan(
	uint32_t controller,
	uint32_t channel,
	uint32_t node,
	uint32_t chip)
{
	char *locations;
	uint32_t registers;
	uint32_t step;
	uint32_t length;

	/* The controller's registers: AAPL,address, else its reg; nothing without either. */
	registers = getprop_word(controller, "AAPL,address");
	if (registers == 0)
		registers = getprop_first(controller, "reg");
	if (registers == 0)
		return;

	/* The registers' step, 16 bytes when the node names none. */
	step = getprop_word(controller, "AAPL,address-step");
	if (step == 0)
		step = I2C_STEP_DEFAULT;

	/* The sensors' places, as the firmware wrote them, cut at the room kept and ended with a NUL. */
	locations = (char *)&fan_table[FAN_TABLE_HEAD_WORDS];
	length = getprop(node, "hwsensor-location", locations, FAN_LOCATION_BYTES - 1U);
	if (length == FIRMWARE_FAILED || length >= FAN_LOCATION_BYTES)
		length = 0;
	locations[length] = '\0';

	/* The rest of the record; the registers last, which make the record count. */
	fan_table[1] = step;
	fan_table[2] = channel;
	fan_table[3] = (getprop_first(node, "reg") & I2C_REG_ADDRESS_MASK) >> 1;
	fan_table[4] = chip;
	fan_table[5] = getprop_word(controller, "AAPL,i2c-rate");
	fan_table[6] = getprop_word(node, "hwsensor-params-version");
	fan_table[7] = length;
	fan_table[0] = registers;
}

/*
 * Reports which fan controller a fan node is by its compatible: an
 * ADT7460, an ADT7467, an ADM1030, or 0 for a chip the kernel does not
 * know.
 */
static uint32_t
fan_chip(
	uint32_t node)
{
	int match;

	/* The ADT7460. */
	match = compatible_has(node, "adt7460");
	if (match)
		return FAN_CHIP_ADT7460;

	/* The ADT7467. */
	match = compatible_has(node, "adt7467");
	if (match)
		return FAN_CHIP_ADT7467;

	/* The ADM1030 (the 12-inch PowerBook G4's). */
	match = compatible_has(node, "adm1030");
	if (match)
		return FAN_CHIP_ADM1030;

	/* Another chip, or none named. */
	return 0;
}

/*
 * Tells whether one of the NUL-separated names of a node's compatible
 * property is the one wanted.
 */
static int
compatible_has(
	uint32_t node,
	const char *wanted)
{
	char compatible[COMPATIBLE_SIZE];
	uint32_t length;
	uint32_t start;
	int match;

	/* The property, cut at the room kept and ended with a NUL. */
	length = getprop(node, "compatible", compatible, sizeof(compatible) - 1U);
	if (length == FIRMWARE_FAILED || length >= sizeof(compatible))
		return 0;
	compatible[length] = '\0';

	/* Each name in turn, compared whole. */
	start = 0;
	while (start < length) {
		match = name_is(&compatible[start], wanted);
		if (match)
			return 1;
		while (start < length && compatible[start] != '\0')
			start++;
		start++;
	}

	/* No name is the one wanted. */
	return 0;
}

/*
 * Surveys the firmware's tree for the parts that measure and cool: walks
 * every node, depth first, and lists the ones whose names speak of them
 * and everything just below an I2C node.  Reads only; the tree is not
 * changed and no device is touched.
 */
static void
survey_tree(
	void)
{
	char name[NAME_SIZE];
	uint32_t nodes[SURVEY_DEPTH];
	uint32_t below_i2c[SURVEY_DEPTH];
	uint32_t depth;
	uint32_t visited;
	uint32_t node;
	uint32_t next;
	uint32_t length;
	uint32_t distance;
	int listed;

	/* The root's first child starts the walk. */
	node = finddevice("/");
	if (node == FIRMWARE_FAILED || node == 0)
		return;
	firmware("child", 1, 1, &node, &next);
	depth = 0;
	nodes[0] = next;
	below_i2c[0] = 0;
	visited = 0;

	/* Each node, its children before its next sibling, while the walk has room. */
	while (visited < SURVEY_NODES) {
		node = nodes[depth];
		if (node == 0 || node == FIRMWARE_FAILED) {
			/* No more siblings here: back to the parent's next sibling. */
			if (depth == 0)
				break;
			depth--;
			firmware("peer", 1, 1, &nodes[depth], &nodes[depth]);
			continue;
		}
		visited++;

		/* The node's name, and whether it is listed. */
		length = getprop(node, "name", name, sizeof(name) - 1U);
		if (length == FIRMWARE_FAILED || length >= sizeof(name))
			length = 0;
		name[length] = '\0';
		distance = below_i2c[depth];
		listed = survey_name_matches(name);
		if (distance != 0 && distance <= SURVEY_I2C_LEVELS)
			listed = 1;
		if (listed)
			survey_node(node);

		/* Its children, one level deeper, counted from the nearest I2C node above. */
		firmware("child", 1, 1, &node, &next);
		if (next != 0 && next != FIRMWARE_FAILED && depth + 1U < SURVEY_DEPTH) {
			depth++;
			nodes[depth] = next;
			below_i2c[depth] = 0;
			if (distance != 0)
				below_i2c[depth] = distance + 1U;
			if (survey_text_has(name, "i2c"))
				below_i2c[depth] = 1;
			continue;
		}

		/* No children: the next sibling. */
		firmware("peer", 1, 1, &node, &nodes[depth]);
	}
}

/* Tells whether a node's name speaks of I2C, fans, temperatures, sensors or the power manager. */
static int
survey_name_matches(
	const char *name)
{
	static const char *const words[] = {
		"i2c", "fan", "therm", "adt", "temp", "sensor", "pmu", "smu"
	};
	uint32_t index;
	int found;

	/* Each word, anywhere in the name. */
	for (index = 0; index < sizeof(words) / sizeof(words[0]); index++) {
		found = survey_text_has(name, words[index]);
		if (found)
			return 1;
	}

	/* No word is in the name. */
	return 0;
}

/* Tells whether a text holds a word, anywhere. */
static int
survey_text_has(
	const char *text,
	const char *word)
{
	uint32_t start;
	uint32_t index;

	/* Each place the word could start. */
	for (start = 0; text[start] != '\0'; start++) {
		for (index = 0; word[index] != '\0' && text[start + index] == word[index]; index++)
			continue;
		if (word[index] == '\0')
			return 1;
	}

	/* The word is not there. */
	return 0;
}

/* Adds one node's line: its path, compatible, reg, device_type and AAPL,address. */
static void
survey_node(
	uint32_t node)
{
	char path[SURVEY_PATH_SIZE];
	char property[SURVEY_PROPERTY_SIZE];
	uint32_t cells[2];
	uint32_t in[3];
	uint32_t length;
	uint32_t index;

	/* The path. */
	in[0] = node;
	in[1] = (uint32_t)(uintptr_t)path;
	in[2] = sizeof(path) - 1U;
	firmware("package-to-path", 3, 1, in, &length);
	if (length == FIRMWARE_FAILED || length >= sizeof(path))
		length = 0;
	path[length] = '\0';
	survey_append_string("PPC32 OF ");
	survey_append_string(path);

	/* The compatible names, separated by '|'. */
	length = getprop(node, "compatible", property, sizeof(property) - 1U);
	if (length != FIRMWARE_FAILED && length < sizeof(property) && length != 0) {
		for (index = 0; index < length; index++) {
			if (property[index] == '\0')
				property[index] = '|';
		}
		while (length != 0 && property[length - 1U] == '|')
			length--;
		property[length] = '\0';
		survey_append_string(" c=");
		survey_append_string(property);
	}

	/* The first two cells of reg. */
	length = getprop(node, "reg", cells, sizeof(cells));
	if (length != FIRMWARE_FAILED && length >= 4U) {
		survey_append_string(" r=");
		survey_append_hex(cells[0]);
		if (length >= sizeof(cells)) {
			survey_append_string(",");
			survey_append_hex(cells[1]);
		}
	}

	/* The device type. */
	length = getprop(node, "device_type", property, sizeof(property) - 1U);
	if (length != FIRMWARE_FAILED && length < sizeof(property) && length != 0) {
		property[length] = '\0';
		survey_append_string(" t=");
		survey_append_string(property);
	}

	/* Apple's absolute address of a controller's registers. */
	length = getprop(node, "AAPL,address", cells, sizeof(cells));
	if (length != FIRMWARE_FAILED && length >= 4U) {
		survey_append_string(" a=");
		survey_append_hex(cells[0]);
	}
	survey_append_string("\n");
}

/* Adds bytes to the survey; what does not fit ends the survey with a mark. */
static void
survey_append(
	const char *text,
	uint32_t length)
{
	static const char full[] = "\nPPC32 OF (more)\n";
	uint32_t index;

	/* A survey that ran out of room takes nothing more. */
	if (survey_full)
		return;

	/* Bytes that leave no room for the mark end the survey with it. */
	if (survey_length + length + sizeof(full) >= SURVEY_SIZE) {
		for (index = 0; full[index] != '\0'; index++)
			survey[survey_length + index] = full[index];
		survey_length += index;
		survey[survey_length] = '\0';
		survey_full = true;
		return;
	}

	/* The bytes, still NUL-terminated. */
	for (index = 0; index < length; index++)
		survey[survey_length + index] = text[index];
	survey_length += length;
	survey[survey_length] = '\0';
}

/* Adds a NUL-terminated text to the survey. */
static void
survey_append_string(
	const char *text)
{
	/* The text up to its NUL. */
	survey_append(text, (uint32_t)hal_strlen(text));
}

/* Adds a number in hexadecimal, without leading zeros. */
static void
survey_append_hex(
	uint32_t value)
{
	static const char digits[] = "0123456789abcdef";
	char text[8];
	uint32_t count;
	uint32_t index;
	char swap;

	/* The digits, lowest first, then turned around. */
	count = 0;
	do {
		text[count] = digits[value & 0xfU];
		value >>= 4;
		count++;
	} while (value != 0);
	for (index = 0; index < count / 2U; index++) {
		swap = text[index];
		text[index] = text[count - 1U - index];
		text[count - 1U - index] = swap;
	}
	survey_append(text, count);
}

/* Finds a child by its name property; 0 when none has it. */
static uint32_t
child_named(
	uint32_t parent,
	const char *wanted)
{
	char name[NAME_SIZE];
	uint32_t child;
	uint32_t length;
	int match;

	/* Each child, its name compared whole. */
	firmware("child", 1, 1, &parent, &child);
	while (child != 0 && child != FIRMWARE_FAILED) {
		length = getprop(child, "name", name, sizeof(name) - 1U);
		if (length != FIRMWARE_FAILED && length < sizeof(name)) {
			name[length] = '\0';
			match = name_is(name, wanted);
			if (match)
				return child;
		}
		firmware("peer", 1, 1, &child, &child);
	}

	/* No child has the name. */
	return 0;
}

/*
 * Finds the PCI host bridge the I/O controller hangs from: the 32-bit
 * Mac's expansion bus at its fixed place, else the parent of the node
 * the mac-io alias names (the 970 machines' AGP or HyperTransport host).
 */
static uint32_t
find_expansion_host(
	void)
{
	uint32_t node;
	uint32_t parent;

	/* The 32-bit Mac's expansion bus. */
	node = finddevice("/pci@f2000000");
	if (node != FIRMWARE_FAILED && node != 0)
		return node;

	/* Otherwise the I/O controller's parent. */
	node = finddevice("mac-io");
	if (node == FIRMWARE_FAILED || node == 0)
		return FIRMWARE_FAILED;
	firmware("parent", 1, 1, &node, &parent);
	if (parent == 0)
		return FIRMWARE_FAILED;

	/* Succeeded: the host, or the firmware's failure value. */
	return parent;
}
