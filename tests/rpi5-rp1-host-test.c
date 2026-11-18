/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs the Raspberry Pi 5's RP1 module (rp1.c, rp1-gpio.c, rp1-irq.c) on
 * the host against a Pi 5 device tree and a root complex kept in memory,
 * and checks what the RP1 drivers are given: their registers on the CPU's
 * bus, the DMA window, the inbound windows the module sets, a pin's
 * function, and the route of a USB controller's interrupt.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drivers/platform/rpi5/rp1.h>
#include <drivers/platform/rpi5/rp1-gpio.h>
#include <drivers/platform/rpi5/rp1-irq.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>

/* The largest blob the test reads. */
#define BLOB_LIMIT		(2U * 1024U * 1024U)

/* The most register blocks the module maps. */
#define MAPPING_LIMIT		16U

/* The root complex's registers in the trees, and the RP1 behind it. */
#define EXPECT_PCIE_REGISTERS	0x1000120000ULL
#define FAKE_RP1_BUS		1U

/*
 * Where the firmware leaves RP1's BARs and its own outbound window (seen
 * on a Pi 5 on 2026-10-22: CPU 0x1c_0000_0000 to PCI 0x8000_0000), the
 * BARs' sizes (16 KiB, 4 MiB, 64 KiB), and where the module moves the
 * BARs: the peripheral one to PCI 0 as the rp1 node's ranges say, the
 * shared memory and the MSI-X table after it.
 */
#define FIRMWARE_MSIX_BAR	0x80410000U
#define FIRMWARE_PERIPHERAL_BAR	0x80000000U
#define FIRMWARE_WINDOW_PCI	0x80000000U
#define FIRMWARE_WINDOW_MIB	0x3ff00000U
#define FIRMWARE_WINDOW_HIGH	0x1cU
#define FAKE_BAR_COUNT		3U
#define EXPECT_MSIX_BAR		0x00410000U
#define EXPECT_PERIPHERAL_BAR	0x00000000U
#define EXPECT_SHARED_BAR	0x00400000U

/* Where the drivers find their registers. */
#define EXPECT_USB0		0x1f00200000ULL
#define EXPECT_USB1		0x1f00300000ULL
#define EXPECT_USB_SIZE		0x100000ULL
#define EXPECT_GEM		0x1f00100000ULL
#define EXPECT_GPIO		0x1f000d0000ULL

/* The DMA window: the CPU's memory at 64 GiB on RP1's side. */
#define EXPECT_DMA_OFFSET	0x1000000000ULL
#define EXPECT_DMA_LIMIT	0xfffffffffULL

/* A pin and its VBUS function, and where its control and pad registers are. */
#define VBUS_PIN		42U
#define VBUS_FUNCTION		2U
#define VBUS_CONTROL		0x8044U
#define VBUS_PAD		0x8024U

/*
 * RP1's MSI-X capability in the fake configuration space, its table in
 * the first BAR, the MIP, and the GIC lines the USB vectors get.
 */
#define FAKE_CAPABILITY		0x40U
#define FAKE_MSIX_ENTRIES	61U
#define EXPECT_MSIX_TABLE	0x1f00410000ULL
#define EXPECT_MSIX_CONFIG	0x1f00108000ULL
#define EXPECT_MIP		0x1000130000ULL
#define EXPECT_DOORBELL		0xfffffff000ULL
#define EXPECT_USB0_VECTOR	31U
#define EXPECT_USB1_VECTOR	36U
#define EXPECT_FIRST_LINE	(32 + 128)

/* The most GIC lines the test records. */
#define LINE_LIMIT		8U

/* One block the module mapped, kept in host memory. */
struct mapping {
	uint64_t address;
	size_t size;
	uint8_t *bytes;
};

/* The size masks a BAR answers all ones with: the MSI-X table, the peripherals, the shared memory. */
static const uint32_t fake_bar_masks[FAKE_BAR_COUNT] = { 0xffffc000U, 0xffc00000U, 0xffff0000U };

static struct mapping mappings[MAPPING_LIMIT];
static unsigned mapping_count;
static uint8_t *fake_pcie;
static const void *tree;
static int lines[LINE_LIMIT];
static unsigned line_count;

static int check(int condition, const char *what);
static uint8_t *find_block(uint64_t address);
static uint32_t read_le32(const uint8_t *bytes, unsigned offset);
static void write_le32(uint8_t *bytes, unsigned offset, uint32_t value);
static int check_tree(const char *path);
static int check_interrupts(uint32_t usb0, uint32_t usb1);
static void ignore_interrupt(void *argument);

/* The tree is the only memory the module asks for through the direct map. */
void *
kern_pmem_to_kernel(
	hal_physaddr_t address)
{
	(void)address;

	/* Succeeded: the blob read from the file. */
	return (void *)tree;
}

/*
 * Maps a block as zeroed host memory, except the root complex's, which
 * shows a trained link and RP1 behind it.
 */
int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	uint8_t *bytes;

	(void)attributes;

	/* Refuses more blocks than the test keeps. */
	if (mapping_count == MAPPING_LIMIT)
		return 12;

	/* Allocates the block. */
	bytes = calloc(1, size);
	if (bytes == NULL)
		return 12;

	/*
	 * A root complex as the Pi 5's firmware leaves it with
	 * pciex4_reset=0: the link up, the root port a bridge without bus
	 * numbers, its own outbound window, and RP1 behind it with its BARs
	 * where the firmware put them.
	 */
	if (address == EXPECT_PCIE_REGISTERS) {
		write_le32(bytes, 0x4068U, 0x30U);
		write_le32(bytes, 0x0cU, 0x00010000U);
		write_le32(bytes, 0x18U, 0);
		write_le32(bytes, 0x400cU, FIRMWARE_WINDOW_PCI);
		write_le32(bytes, 0x4070U, FIRMWARE_WINDOW_MIB);
		write_le32(bytes, 0x4080U, FIRMWARE_WINDOW_HIGH);
		write_le32(bytes, 0x4084U, FIRMWARE_WINDOW_HIGH);
		write_le32(bytes, 0x8000U, 0x00011de4U);
		write_le32(bytes, 0x8010U, FIRMWARE_MSIX_BAR);
		write_le32(bytes, 0x8014U, FIRMWARE_PERIPHERAL_BAR);
		write_le32(bytes, 0x8018U, 0);
		fake_pcie = bytes;
	}

	/* Succeeded: the block. */
	mappings[mapping_count].address = address;
	mappings[mapping_count].size = size;
	mappings[mapping_count].bytes = bytes;
	mapping_count++;
	*mapped = bytes;
	return 0;
}

/* Unmapping keeps the memory, which later checks read. */
int
kern_device_unmap(
	void *mapped,
	size_t size)
{
	(void)mapped;
	(void)size;

	/* Succeeded. */
	return 0;
}

/* Reads a register from host memory. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uint32_t value;

	/* Copies the bytes. */
	memcpy(&value, (const void *)address, sizeof(value));

	/* Succeeded: the register. */
	return value;
}

/* Writes a register to host memory; RP1's BARs answer all ones with their size. */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uint8_t *bytes;
	unsigned bar;

	/* A BAR sized with all ones reads back its size mask. */
	bytes = (uint8_t *)address;
	if (fake_pcie != NULL && value == 0xffffffffU) {
		for (bar = 0; bar < FAKE_BAR_COUNT; bar++) {
			if (bytes == fake_pcie + 0x8010U + 4U * bar)
				value = fake_bar_masks[bar];
		}
	}

	/* Copies the bytes. */
	memcpy((void *)address, &value, sizeof(value));
}

/* Prints the module's log. */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	/* Prints the message with the test's prefix. */
	va_start(arguments, format);
	printf("  log: ");
	vprintf(format, arguments);
	va_end(arguments);
}

/* Any line can be edge triggered. */
int
kern_irq_set_mode(
	int irq,
	unsigned trigger,
	unsigned polarity)
{
	(void)irq;
	(void)trigger;
	(void)polarity;

	/* Succeeded. */
	return 0;
}

/* Records the lines the module takes. */
int
kern_irq_register(
	int irq,
	kern_irq_handler_t handler,
	void *argument)
{
	(void)handler;
	(void)argument;

	/* Refuses more lines than the test keeps. */
	if (line_count == LINE_LIMIT)
		return 12;

	/* Succeeded: the line is recorded. */
	lines[line_count] = irq;
	line_count++;
	return 0;
}

/* Ending an interrupt is not called on the host. */
void
kern_irq_send_eoi(
	kern_irq_ack_t acknowledge)
{
	(void)acknowledge;
}

/* The lock has no other user on the host. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	(void)lock;
	(void)rank;
	(void)name;
}

/* Takes the lock, which nothing contends on the host. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;

	/* Succeeded: interrupts were off. */
	return 0;
}

/* Releases the lock. */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	(void)enabled;
}

/*
 * Runs the test on the device tree blob named on the command line.
 */
int
main(
	int argc,
	char **argv)
{
	int failures;

	/* Requires the blob's path. */
	if (argc != 2) {
		fprintf(stderr, "usage: rpi5-rp1-host-test bcm2712-rpi-5-b.dtb\n");
		return 2;
	}

	/* Checks the tree. */
	failures = check_tree(argv[1]);
	if (failures != 0) {
		printf("rpi5-rp1-host-test: FAIL (%s, %d)\n", argv[1], failures);
		return 1;
	}

	/* Succeeded. */
	printf("rpi5-rp1-host-test: PASS (%s)\n", argv[1]);
	return 0;
}

/* Runs the module on one tree and counts the failed checks. */
static int
check_tree(
	const char *path)
{
	static unsigned char blob[BLOB_LIMIT];
	uint8_t *pcie;
	uint8_t *gpio;
	uint8_t *pads;
	uint64_t address;
	uint64_t size;
	uint64_t offset;
	uint64_t limit;
	uint32_t usb0;
	uint32_t usb1;
	uint32_t node;
	uint32_t pad;
	int coherent;
	int failures;
	int error;
	FILE *file;

	/* Reads the blob. */
	file = fopen(path, "rb");
	if (file == NULL) {
		perror(path);
		return 1;
	}
	(void)fread(blob, 1, sizeof(blob), file);
	fclose(file);
	tree = blob;

	/* Finds RP1 as the boot does. */
	failures = 0;
	error = drv_rp1_init(0x2000000U);
	failures += check(error == 0, "RP1 is found behind the root complex");
	if (error != 0)
		return failures;

	/* The two USB controllers, in the tree's order. */
	error = drv_rp1_find_child("snps,dwc3", DRV_FDT_NO_NODE, &usb0);
	failures += check(error == 0, "the first USB controller is found");
	error = drv_rp1_find_child("snps,dwc3", usb0, &usb1);
	failures += check(error == 0, "the second USB controller is found");
	error = drv_rp1_find_child("snps,dwc3", usb1, &node);
	failures += check(error != 0, "there is no third USB controller");
	error = drv_rp1_reg(usb0, 0, &address, &size);
	failures += check(error == 0 && address == EXPECT_USB0 && size == EXPECT_USB_SIZE, "the first controller's registers");
	error = drv_rp1_reg(usb1, 0, &address, &size);
	failures += check(error == 0 && address == EXPECT_USB1 && size == EXPECT_USB_SIZE, "the second controller's registers");

	/* The Ethernet controller. */
	error = drv_rp1_find_child("cdns,macb", DRV_FDT_NO_NODE, &node);
	failures += check(error == 0, "the Ethernet controller is found");
	error = drv_rp1_reg(node, 0, &address, &size);
	failures += check(error == 0 && address == EXPECT_GEM, "the Ethernet controller's registers");

	/* The DMA window. */
	error = drv_rp1_dma_window(&offset, &limit, &coherent);
	failures += check(error == 0 && offset == EXPECT_DMA_OFFSET, "RP1's DMA sees memory at 64 GiB");
	failures += check(limit == EXPECT_DMA_LIMIT, "RP1's DMA reaches 64 GiB of memory");
	failures += check(coherent == 0, "RP1's DMA does not snoop");

	/* The inbound windows: RP1's own window, memory, and the MSI receiver. */
	pcie = find_block(EXPECT_PCIE_REGISTERS);
	failures += check(pcie != NULL, "the root complex was mapped");
	if (pcie == NULL)
		return failures;
	failures += check(read_le32(pcie, 0x402cU) == 0x00000007U, "window 1 covers 4 MiB at PCI 0");
	failures += check(read_le32(pcie, 0x40acU) == 0x00000001U && read_le32(pcie, 0x40b0U) == 0x1fU, "window 1 lands on RP1's own window");
	failures += check(read_le32(pcie, 0x4034U) == 0x00000015U && read_le32(pcie, 0x4038U) == 0x10U, "window 2 covers 64 GiB at PCI 64 GiB");
	failures += check(read_le32(pcie, 0x40b4U) == 0x00000001U && read_le32(pcie, 0x40b8U) == 0, "window 2 lands on memory");
	failures += check(read_le32(pcie, 0x403cU) == 0xfffff01cU && read_le32(pcie, 0x4040U) == 0xffU, "window 3 covers the MSI page");
	failures += check(read_le32(pcie, 0x40bcU) == 0x00130001U && read_le32(pcie, 0x40c0U) == 0x10U, "window 3 lands on the MSI receiver");
	failures += check((read_le32(pcie, 0x8004U) & 0x6U) == 0x6U, "RP1 decodes memory and masters");

	/* The root port the firmware left unnumbered, and the outbound window it left unset. */
	failures += check(read_le32(pcie, 0x18U) == ((FAKE_RP1_BUS << 16) | (FAKE_RP1_BUS << 8)), "the root port's secondary bus is 1");
	failures += check((read_le32(pcie, 0x04U) & 0x6U) == 0x6U, "the root port forwards memory both ways");
	failures += check(read_le32(pcie, 0x20U) == 0xfff00000U, "the root port passes the 32-bit memory space");
	failures += check((read_le32(pcie, 0x4008U) & 0x2000U) != 0, "an unanswered configuration read returns ones");
	failures += check(read_le32(pcie, 0x400cU) == 0 && read_le32(pcie, 0x4010U) == 0, "the outbound window starts at PCI 0");
	failures += check(read_le32(pcie, 0x4070U) == 0xfff00000U, "the outbound window's low MiB bits");
	failures += check(read_le32(pcie, 0x4080U) == 0x1fU && read_le32(pcie, 0x4084U) == 0x1fU, "the outbound window is at 124 GiB");
	failures += check(read_le32(pcie, 0x4014U) == 0 && read_le32(pcie, 0x4018U) == 4U, "the second outbound window starts at PCI 16 GiB");
	failures += check(read_le32(pcie, 0x4074U) == 0xfff00000U, "the second outbound window's low MiB bits");
	failures += check(read_le32(pcie, 0x4088U) == 0x1cU && read_le32(pcie, 0x408cU) == 0x1eU, "the second outbound window is at 112 GiB");

	/* RP1's BARs, moved from the firmware's places to the tree's. */
	failures += check(read_le32(pcie, 0x8014U) == EXPECT_PERIPHERAL_BAR, "the peripheral BAR is at PCI 0");
	failures += check(read_le32(pcie, 0x8018U) == EXPECT_SHARED_BAR, "the shared memory BAR follows it");
	failures += check(read_le32(pcie, 0x8010U) == EXPECT_MSIX_BAR, "the MSI-X table's BAR comes last");

	/* A pin's function. */
	error = drv_rp1_gpio_init();
	failures += check(error == 0, "the GPIO block is mapped");
	error = drv_rp1_gpio_set_function(VBUS_PIN, VBUS_FUNCTION);
	failures += check(error == 0, "the VBUS pin is connected");
	gpio = find_block(EXPECT_GPIO);
	pads = find_block(EXPECT_GPIO + 0x20000U);
	failures += check(gpio != NULL && pads != NULL, "the GPIO blocks were mapped");
	if (gpio == NULL || pads == NULL)
		return failures;
	failures += check(read_le32(gpio, VBUS_CONTROL) == VBUS_FUNCTION, "the pin's selector is its VBUS function");
	pad = read_le32(pads, VBUS_PAD);
	failures += check((pad & 0x40U) != 0 && (pad & 0x80U) == 0, "the pin's pad is enabled");
	error = drv_rp1_gpio_set_function(54U, 0);
	failures += check(error != 0, "a pin RP1 does not have is refused");

	/* The USB controllers' interrupts. */
	write_le32(pcie, 0x8004U, read_le32(pcie, 0x8004U) | (1U << 20));
	write_le32(pcie, 0x8034U, FAKE_CAPABILITY);
	write_le32(pcie, 0x8000U + FAKE_CAPABILITY, ((FAKE_MSIX_ENTRIES - 1U) << 16) | 0x11U);
	write_le32(pcie, 0x8000U + FAKE_CAPABILITY + 4U, 0);
	failures += check_interrupts(usb0, usb1);

	/* Succeeded: the failures counted. */
	return failures;
}

/* Routes the two USB controllers' interrupts and checks the route. */
static int
check_interrupts(
	uint32_t usb0,
	uint32_t usb1)
{
	uint8_t *pcie;
	uint8_t *table;
	uint8_t *config;
	unsigned vector;
	unsigned trigger;
	unsigned entry;
	int failures;
	int error;

	/* The specifiers of the two controllers. */
	failures = 0;
	error = drv_rp1_irq_of(usb0, 0, &vector, &trigger);
	failures += check(error == 0 && vector == EXPECT_USB0_VECTOR && trigger == DRV_RP1_IRQ_EDGE, "the first controller's vector");
	error = drv_rp1_irq_of(usb1, 0, &vector, &trigger);
	failures += check(error == 0 && vector == EXPECT_USB1_VECTOR && trigger == DRV_RP1_IRQ_EDGE, "the second controller's vector");

	/* The path. */
	error = drv_rp1_irq_init();
	failures += check(error == 0, "the interrupt path is found");
	if (error != 0)
		return failures;
	pcie = find_block(EXPECT_PCIE_REGISTERS);
	failures += check((read_le32(pcie, 0x8000U + FAKE_CAPABILITY) & 0xc0000000U) == 0x80000000U, "MSI-X is on without the function mask");
	failures += check(find_block(EXPECT_MIP) != NULL, "the MIP is mapped");

	/* The two routes. */
	error = drv_rp1_irq_establish(EXPECT_USB0_VECTOR, DRV_RP1_IRQ_EDGE, ignore_interrupt, NULL);
	failures += check(error == 0, "the first vector is routed");
	error = drv_rp1_irq_establish(EXPECT_USB1_VECTOR, DRV_RP1_IRQ_EDGE, ignore_interrupt, NULL);
	failures += check(error == 0, "the second vector is routed");
	failures += check(line_count == 2U && lines[0] == EXPECT_FIRST_LINE && lines[1] == EXPECT_FIRST_LINE + 1, "the vectors take the MIP's first lines");
	error = drv_rp1_irq_establish(FAKE_MSIX_ENTRIES, DRV_RP1_IRQ_EDGE, ignore_interrupt, NULL);
	failures += check(error != 0, "a vector RP1 does not have is refused");

	/* The table entries and the vector configuration. */
	table = find_block(EXPECT_MSIX_TABLE);
	config = find_block(EXPECT_MSIX_CONFIG);
	failures += check(table != NULL && config != NULL, "the table and the vector configuration are mapped");
	if (table == NULL || config == NULL)
		return failures;
	entry = EXPECT_USB1_VECTOR * 16U;
	failures += check(read_le32(table, entry) == (uint32_t)EXPECT_DOORBELL, "the entry's address is the doorbell's low half");
	failures += check(read_le32(table, entry + 4U) == (uint32_t)(EXPECT_DOORBELL >> 32), "the entry's address is the doorbell's high half");
	failures += check(read_le32(table, entry + 8U) == 1U, "the entry's message is the second line");
	failures += check(read_le32(table, entry + 12U) == 0, "the entry is unmasked");
	failures += check(read_le32(table, 5U * 16U + 12U) == 1U, "an unrouted entry stays masked");
	failures += check(read_le32(config, 0x800U + 8U + EXPECT_USB1_VECTOR * 4U) == 1U, "the vector is enabled in RP1");

	/* Succeeded: the failures counted. */
	return failures;
}

/* The handler of the routed vectors, which the host never calls. */
static void
ignore_interrupt(
	void *argument)
{
	(void)argument;
}

/* Finds the host memory of a block mapped at an address. */
static uint8_t *
find_block(
	uint64_t address)
{
	unsigned index;

	/* Looks at each mapping. */
	for (index = 0; index < mapping_count; index++) {
		/* The block mapped at the address. */
		if (mappings[index].address == address)
			return mappings[index].bytes;
	}

	/* No such block. */
	return NULL;
}

/* Reads a little-endian word of a block. */
static uint32_t
read_le32(
	const uint8_t *bytes,
	unsigned offset)
{
	uint32_t value;

	/* Copies the word. */
	memcpy(&value, bytes + offset, sizeof(value));

	/* Succeeded: the word. */
	return value;
}

/* Writes a little-endian word of a block. */
static void
write_le32(
	uint8_t *bytes,
	unsigned offset,
	uint32_t value)
{
	/* Copies the word. */
	memcpy(bytes + offset, &value, sizeof(value));
}

/* Reports a failed check and counts it. */
static int
check(
	int condition,
	const char *what)
{
	/* A passed check counts nothing. */
	if (condition)
		return 0;

	/* Succeeded: one failure counted. */
	printf("  FAIL: %s\n", what);
	return 1;
}
