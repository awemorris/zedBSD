/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * WS048 p002: host test of the BCM2711 PCIe root complex driver.
 *
 * The production src/drivers/pci/pci-brcmstb.c is compiled unchanged.  The
 * kernel services it calls (register access, device mapping, waits, locks,
 * the PCI core and the DMA layer) are replaced here.  Register access goes to
 * a model of the controller: its reset and SerDes bits, the link that comes
 * up some time after the endpoint leaves reset, the root complex's own
 * configuration space and, behind the index and data window, the VL805's
 * configuration space with a 64-bit 4 KiB BAR and INTA.  Waits advance a
 * virtual clock, so the test can check the order and the timing of the start
 * sequence as well as the values it programs.
 *
 * Usage: brcmstb-host-test FIRMWARE.dtb DISABLED.dtb
 */

#include <drivers/pci/pci-brcmstb.h>
#include <drivers/generic/dma.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <uapi/errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

/* Last DMA provider window captured before publication to the fake PCI core. */
static uint64_t published_bus_offset;
static uint64_t published_physical_limit;

#define CHECK(expression)                                                   \
	do {                                                                 \
		checks++;                                                    \
		if (!(expression)) {                                        \
			fprintf(stderr, "ws048-brcmstb: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                  \
			exit(EXIT_FAILURE);                                   \
		}                                                            \
	} while (0)

/* Register offsets the model gives meaning to. */
#define REG_VENDOR1		0x0188U
#define REG_ID_VAL3		0x043cU
#define REG_MISC_CTRL		0x4008U
#define REG_WIN0_LO		0x400cU
#define REG_WIN0_HI		0x4010U
#define REG_RC_BAR1_LO		0x402cU
#define REG_RC_BAR2_LO		0x4034U
#define REG_RC_BAR2_HI		0x4038U
#define REG_RC_BAR3_LO		0x403cU
#define REG_STATUS		0x4068U
#define REG_REVISION		0x406cU
#define REG_WIN0_BASE_LIMIT	0x4070U
#define REG_WIN0_BASE_HI	0x4080U
#define REG_WIN0_LIMIT_HI	0x4084U
#define REG_HARD_DEBUG		0x4204U
#define REG_INTR2_CLEAR		0x4308U
#define REG_INTR2_MASK_SET	0x4310U
#define REG_EXT_CFG_DATA	0x8000U
#define REG_EXT_CFG_INDEX	0x9000U
#define REG_SW_INIT		0x9210U

#define SW_INIT_PERST		0x1U
#define SW_INIT_BRIDGE		0x2U
#define HARD_DEBUG_CLKREQ	0x00000002U
#define HARD_DEBUG_IDDQ		0x08000000U

#define REGISTER_BASE		0xfd500000ULL
#define REGISTER_SIZE		0x9310ULL
#define WINDOW_CPU		0x600000000ULL
#define WINDOW_PCI		0xc0000000ULL
#define WINDOW_SIZE		0x40000000ULL

/* One write of a sequence register, stamped with the virtual time. */
struct model_event {
	unsigned offset;
	uint32_t value;
	unsigned long long time_us;
};

/* What a scenario changes about the modelled hardware. */
struct model_options {
	bool link_never_up;
	bool endpoint_mode;
	bool multifunction;
	uint64_t bar0_size;
};

/* A bus and a device as the fake PCI core hands them to the driver. */
struct drv_pci_bus {
	struct drv_pci_bus *parent;
	struct drv_pci_device *bridge;
};

struct drv_pci_device {
	struct drv_pci_address address;
	struct drv_pci_bus *bus;
};

static struct model_options options;
static uint32_t registers[0x10000U / 4U];
static uint8_t rc_config[4096];
static uint8_t endpoint_config[2][4096];
static uint64_t endpoint_bar0[2];
static uint32_t endpoint_bar1[2];
static uint32_t ext_index;
static unsigned long long now_us;
static unsigned long long perst_release_us;
static bool perst_released;
static unsigned ext_accesses;
static unsigned ext_accesses_too_early;
static unsigned ext_accesses_link_down;
static struct model_event events[256];
static unsigned event_count;
static uint8_t bar_memory[4096];
static uint64_t mapped_cpu_address;
static int allocations;
static int lock_depth;

/* What the fake PCI core recorded. */
static const struct drv_pci_bus_ops *published_ops;
static void *published_host;
static struct drv_dma_constraints published_constraints;
static unsigned scans;
static struct drv_pci_bus root_bus;
static int dma_token;

static void model_reset(const struct model_options *scenario);
static bool link_up_now(void);
static uint32_t model_read(unsigned offset, unsigned width);
static void model_write(unsigned offset, unsigned width, uint32_t value);
static uint32_t endpoint_read(unsigned function, unsigned offset, unsigned width);
static void endpoint_write(unsigned function, unsigned offset, unsigned width, uint32_t value);
static uint32_t bytes_read(const uint8_t *space, unsigned offset, unsigned width);
static void bytes_write(uint8_t *space, unsigned offset, unsigned width, uint32_t value);
static const struct model_event *find_event(unsigned offset, uint32_t mask, uint32_t wanted, unsigned after);
static unsigned event_index(const struct model_event *event);
static unsigned char *read_file(const char *path, size_t *size);
static void standard_config(struct drv_pci_brcmstb_config *config);
static void test_describe(const unsigned char *firmware, size_t firmware_size, const unsigned char *disabled, size_t disabled_size);
static void test_start_sequence(void);
static void test_configuration_access(void);
static void test_mapping_and_interrupts(void);
static void test_failures(void);
static void test_multifunction(void);
static void test_high_dma_alias(void);

int
main(
	int argc,
	char **argv)
{
	unsigned char *firmware;
	unsigned char *disabled;
	size_t firmware_size;
	size_t disabled_size;

	if (argc != 3) {
		fprintf(stderr, "usage: %s FIRMWARE.dtb DISABLED.dtb\n", argv[0]);
		return EXIT_FAILURE;
	}

	firmware = read_file(argv[1], &firmware_size);
	disabled = read_file(argv[2], &disabled_size);
	test_describe(firmware, firmware_size, disabled, disabled_size);
	test_start_sequence();
	test_configuration_access();
	test_mapping_and_interrupts();
	test_failures();
	test_multifunction();

	/*
	 * Retires the first published fixture before another publication
	 * replaces it.
	 */
	free(published_host);
	published_host = NULL;

	/* Checks high aliases after retiring the preceding fixture. */
	test_high_dma_alias();
	free(firmware);
	free(disabled);
	printf("ws048-brcmstb: %u checks passed\n", checks);
	return EXIT_SUCCESS;
}

/* Reads the controller's description from the firmware's tree. */
static void
test_describe(
	const unsigned char *firmware,
	size_t firmware_size,
	const unsigned char *disabled,
	size_t disabled_size)
{
	struct drv_pci_brcmstb_config config;
	struct drv_fdt fdt;
	uint32_t node;

	CHECK(drv_fdt_open(&fdt, firmware, firmware_size) == 0);
	CHECK(drv_fdt_find_compatible(&fdt, "brcm,bcm2711-pcie", DRV_FDT_NO_NODE, &node) == 0);
	CHECK(drv_pci_brcmstb_describe(&fdt, node, &config) == 0);
	CHECK(config.register_base == REGISTER_BASE);
	CHECK(config.register_size == REGISTER_SIZE);
	CHECK(config.outbound_cpu_base == WINDOW_CPU);
	CHECK(config.outbound_pci_base == WINDOW_PCI);
	CHECK(config.outbound_size == WINDOW_SIZE);
	CHECK(config.inbound_cpu_base == 0);
	CHECK(config.inbound_pci_base == 0);
	CHECK(config.inbound_size == 0xc0000000ULL);
	CHECK(config.intx_irq[0] == 175U);
	CHECK(config.intx_irq[1] == 176U);
	CHECK(config.intx_irq[2] == 177U);
	CHECK(config.intx_irq[3] == 178U);

	CHECK(drv_fdt_open(&fdt, disabled, disabled_size) == 0);
	CHECK(drv_fdt_find_compatible(&fdt, "brcm,bcm2711-pcie", DRV_FDT_NO_NODE, &node) == 0);
	CHECK(drv_pci_brcmstb_describe(&fdt, node, &config) == ENODEV);
}

/* Starts the controller and checks the programmed values and their order. */
static void
test_start_sequence(void)
{
	struct drv_pci_brcmstb_config config;
	struct drv_pci_brcmstb *host;
	struct model_options scenario;
	const struct model_event *reset;
	const struct model_event *bridge_release;
	const struct model_event *serdes_on;
	const struct model_event *misc;
	const struct model_event *inbound;
	const struct model_event *perst_off;
	uint32_t misc_value;

	memset(&scenario, 0, sizeof(scenario));
	scenario.bar0_size = 0x1000U;
	model_reset(&scenario);
	standard_config(&config);
	CHECK(drv_pci_brcmstb_start(&config, &host) == 0);
	CHECK(lock_depth == 0);

	/* Reset: bridge and endpoint together, then the bridge alone after 100 us. */
	reset = find_event(REG_SW_INIT, SW_INIT_PERST | SW_INIT_BRIDGE, SW_INIT_PERST | SW_INIT_BRIDGE, 0);
	CHECK(reset != NULL);
	bridge_release = find_event(REG_SW_INIT, SW_INIT_PERST | SW_INIT_BRIDGE, SW_INIT_PERST, event_index(reset) + 1U);
	CHECK(bridge_release != NULL);
	CHECK(bridge_release->time_us >= reset->time_us + 100U);

	/* The SerDes is powered after the bridge leaves reset. */
	serdes_on = find_event(REG_HARD_DEBUG, HARD_DEBUG_IDDQ, 0, event_index(bridge_release) + 1U);
	CHECK(serdes_on != NULL);

	/* MISC_CTRL: system-bus access, all-ones for absent functions, 128-byte bursts, 4 GiB. */
	misc = find_event(REG_MISC_CTRL, 0, 0, 0);
	CHECK(misc != NULL);
	misc_value = registers[REG_MISC_CTRL / 4U];
	CHECK((misc_value & 0x00001000U) != 0);
	CHECK((misc_value & 0x00002000U) != 0);
	CHECK((misc_value & 0x00300000U) == 0);
	CHECK((misc_value >> 27) == 17U);
	CHECK((misc_value & 0x00000001U) != 0);

	/* Inbound: PCI 0, 4 GiB (code 17); BAR1 and BAR3 closed; little-endian. */
	inbound = find_event(REG_RC_BAR2_LO, 0, 0, 0);
	CHECK(inbound != NULL);
	CHECK(registers[REG_RC_BAR2_LO / 4U] == 17U);
	CHECK(registers[REG_RC_BAR2_HI / 4U] == 0);
	CHECK((registers[REG_RC_BAR1_LO / 4U] & 0x1fU) == 0);
	CHECK((registers[REG_RC_BAR3_LO / 4U] & 0x1fU) == 0);
	CHECK((registers[REG_RC_BAR1_LO / 4U] & 0xffffff00U) == 0xabcdef00U);
	CHECK((bytes_read(rc_config, REG_VENDOR1, 4U) & 0xcU) == 0);
	CHECK((bytes_read(rc_config, REG_VENDOR1, 4U) & 0x3U) == 0x3U);
	CHECK(registers[REG_INTR2_MASK_SET / 4U] == 0xffffffffU);
	CHECK(find_event(REG_INTR2_CLEAR, 0xffffffffU, 0xffffffffU, 0) != NULL);

	/* The endpoint leaves reset only after both windows' inbound side is set. */
	perst_off = find_event(REG_SW_INIT, SW_INIT_PERST, 0, 0);
	CHECK(perst_off != NULL);
	CHECK(event_index(perst_off) > event_index(misc));
	CHECK(event_index(perst_off) > event_index(inbound));
	CHECK(event_index(perst_off) > event_index(serdes_on));

	/* No configuration request below the root port before 100 ms or without a link. */
	CHECK(ext_accesses > 0);
	CHECK(ext_accesses_too_early == 0);
	CHECK(ext_accesses_link_down == 0);

	/* Outbound: PCI 0xc0000000 from CPU 0x6_0000_0000 to 0x6_3fff_ffff. */
	CHECK(registers[REG_WIN0_LO / 4U] == 0xc0000000U);
	CHECK(registers[REG_WIN0_HI / 4U] == 0);
	CHECK(registers[REG_WIN0_BASE_LIMIT / 4U] == 0x3ff00000U);
	CHECK(registers[REG_WIN0_BASE_HI / 4U] == 6U);
	CHECK(registers[REG_WIN0_LIMIT_HI / 4U] == 6U);

	/* The root complex names itself a PCI-to-PCI bridge and gates its clock. */
	CHECK((bytes_read(rc_config, REG_ID_VAL3, 4U) & 0x00ffffffU) == 0x00060400U);
	CHECK((bytes_read(rc_config, REG_ID_VAL3, 4U) & 0xff000000U) == 0x5a000000U);
	CHECK((registers[REG_HARD_DEBUG / 4U] & HARD_DEBUG_CLKREQ) != 0);
	CHECK((registers[REG_HARD_DEBUG / 4U] & HARD_DEBUG_IDDQ) == 0);

	/* The root port leads to bus 1 and forwards the first megabyte of the window. */
	CHECK(bytes_read(rc_config, 0x18U, 4U) == 0x00010100U);
	CHECK(bytes_read(rc_config, 0x20U, 4U) == 0xc000c000U);
	CHECK(bytes_read(rc_config, 0x24U, 4U) == 0x0000fff0U);
	CHECK(bytes_read(rc_config, 0x1cU, 1U) == 0xf0U);
	CHECK(bytes_read(rc_config, 0x1dU, 1U) == 0);
	CHECK(bytes_read(rc_config, 0x04U, 2U) == 0x0006U);

	/* The VL805's BAR0 is at the start of the window; its command is untouched. */
	CHECK(endpoint_bar0[0] == WINDOW_PCI);
	CHECK(endpoint_bar1[0] == 0);
	CHECK(bytes_read(endpoint_config[0], 0x04U, 2U) == 0);

	/* An endpoint reset between start and publish gets its BAR back at the same place. */
	endpoint_bar0[0] = 0;
	CHECK(drv_pci_brcmstb_reassign(host) == 0);
	CHECK(endpoint_bar0[0] == WINDOW_PCI);
	CHECK(bytes_read(rc_config, 0x20U, 4U) == 0xc000c000U);
	CHECK(drv_pci_brcmstb_reassign(NULL) == EINVAL);

	/* Publishing creates bus 0 with non-coherent 31-bit DMA and enumerates it. */
	CHECK(drv_pci_brcmstb_publish(host) == 0);
	CHECK(published_ops != NULL);
	CHECK(published_host == host);
	CHECK(published_constraints.address_bits == 31U);
	CHECK(published_constraints.coherent == 0);
	CHECK(scans == 1U);
	CHECK(drv_pci_brcmstb_publish(host) == EBUSY);
	CHECK(drv_pci_brcmstb_reassign(host) == EBUSY);
	CHECK(lock_depth == 0);
}

/* Reads and writes configuration space through the published operations. */
static void
test_configuration_access(void)
{
	struct drv_pci_address address;
	uint32_t value;
	unsigned before;

	memset(&address, 0, sizeof(address));

	/* The root complex itself, with the class code it now reports. */
	CHECK(published_ops->config_read(published_host, &address, 0, 4U, &value) == 0);
	CHECK(value == 0x271114e4U);
	CHECK(published_ops->config_read(published_host, &address, 0x08U, 4U, &value) == 0);
	CHECK((value >> 8) == 0x060400U);
	CHECK(published_ops->config_read(published_host, &address, 0x0eU, 1U, &value) == 0);
	CHECK(value == 0x01U);

	/* Other devices on bus 0 do not exist and are never asked. */
	before = ext_accesses;
	address.device = 1U;
	CHECK(published_ops->config_read(published_host, &address, 0, 4U, &value) == 0);
	CHECK(value == 0xffffffffU);
	CHECK(published_ops->config_read(published_host, &address, 0, 2U, &value) == 0);
	CHECK(value == 0xffffU);
	CHECK(ext_accesses == before);

	/* The VL805 on bus 1, at every width. */
	address.bus = 1U;
	address.device = 0;
	CHECK(published_ops->config_read(published_host, &address, 0, 4U, &value) == 0);
	CHECK(value == 0x34831106U);
	CHECK(published_ops->config_read(published_host, &address, 2U, 2U, &value) == 0);
	CHECK(value == 0x3483U);
	CHECK(published_ops->config_read(published_host, &address, 0x0bU, 1U, &value) == 0);
	CHECK(value == 0x0cU);
	CHECK(ext_index == 0x00100000U);

	/* Device 1 on bus 1 cannot exist behind a link and is not asked. */
	before = ext_accesses;
	address.device = 1U;
	CHECK(published_ops->config_read(published_host, &address, 0, 4U, &value) == 0);
	CHECK(value == 0xffffffffU);
	CHECK(ext_accesses == before);

	/* A bus further down is asked; nothing answers there. */
	address.bus = 2U;
	address.device = 3U;
	address.function = 5U;
	CHECK(published_ops->config_read(published_host, &address, 0x100U, 4U, &value) == 0);
	CHECK(value == 0xffffffffU);
	CHECK(ext_index == ((2U << 20) | (3U << 15) | (5U << 12)));

	/* Writes reach the endpoint at their own width. */
	address.bus = 1U;
	address.device = 0;
	address.function = 0;
	CHECK(published_ops->config_write(published_host, &address, 0x3cU, 1U, 0x2aU) == 0);
	CHECK(endpoint_config[0][0x3c] == 0x2aU);
	CHECK(endpoint_config[0][0x3d] == 0x01U);

	/* Malformed accesses are refused. */
	CHECK(published_ops->config_read(published_host, &address, 1U, 2U, &value) == EINVAL);
	CHECK(published_ops->config_read(published_host, &address, 2U, 4U, &value) == EINVAL);
	CHECK(published_ops->config_read(published_host, &address, 4096U, 1U, &value) == EINVAL);
	CHECK(published_ops->config_read(published_host, &address, 0, 3U, &value) == EINVAL);
	CHECK(published_ops->config_write(published_host, &address, 4094U, 4U, 0) == EINVAL);
	address.function = 8U;
	CHECK(published_ops->config_read(published_host, &address, 0, 4U, &value) == EINVAL);

	/* The platform's read before publication goes the same way. */
	address.function = 0;
	CHECK(drv_pci_brcmstb_config_read(published_host, &address, 0, 4U, &value) == 0);
	CHECK(value == 0x34831106U);
	CHECK(lock_depth == 0);
}

/* Maps the BAR and routes the legacy interrupt. */
static void
test_mapping_and_interrupts(void)
{
	struct drv_pci_mapping mapping;
	struct drv_pci_bar bar;
	struct drv_pci_irq irq;
	struct drv_pci_device root_port;
	struct drv_pci_device endpoint;
	struct drv_pci_device behind;
	struct drv_pci_bus secondary;
	struct drv_pci_bus tertiary;
	unsigned count;

	/* The BAR's CPU address is the same offset into the CPU side of the window. */
	memset(&bar, 0, sizeof(bar));
	bar.type = DRV_PCI_BAR_MEMORY64;
	bar.bus_address = WINDOW_PCI;
	bar.size = 0x1000U;
	CHECK(published_ops->map_bar(published_host, NULL, &bar, 0, &mapping) == 0);
	CHECK(mapping.physical_address == WINDOW_CPU);
	CHECK(mapped_cpu_address == WINDOW_CPU);
	CHECK(mapping.address == bar_memory);
	CHECK(mapping.size == 0x1000U);
	published_ops->unmap_bar(published_host, &mapping);
	CHECK(mapping.address == NULL);

	/* BARs outside the window, I/O BARs and empty ones are refused. */
	bar.bus_address = WINDOW_PCI - 0x1000U;
	CHECK(published_ops->map_bar(published_host, NULL, &bar, 0, &mapping) == EINVAL);
	bar.bus_address = WINDOW_PCI + WINDOW_SIZE - 0x800U;
	CHECK(published_ops->map_bar(published_host, NULL, &bar, 0, &mapping) == EINVAL);
	bar.bus_address = WINDOW_PCI;
	bar.type = DRV_PCI_BAR_IO;
	CHECK(published_ops->map_bar(published_host, NULL, &bar, 0, &mapping) == EINVAL);
	bar.type = DRV_PCI_BAR_MEMORY32;
	bar.size = 0;
	CHECK(published_ops->map_bar(published_host, NULL, &bar, 0, &mapping) == EINVAL);

	/* The VL805 on bus 1 uses INTA, which the map sends to interrupt 175. */
	memset(&root_port, 0, sizeof(root_port));
	root_port.bus = &root_bus;
	memset(&secondary, 0, sizeof(secondary));
	secondary.parent = &root_bus;
	secondary.bridge = &root_port;
	memset(&endpoint, 0, sizeof(endpoint));
	endpoint.address.bus = 1U;
	endpoint.bus = &secondary;
	CHECK(published_ops->allocate_irqs(published_host, &endpoint, DRV_PCI_IRQ_INTX, 1U, 1U, &irq, &count) == 0);
	CHECK(count == 1U);
	CHECK(irq.type == DRV_PCI_IRQ_INTX);
	CHECK(irq.vector == 175U);

	/* MSI is not offered, nor more than one line. */
	CHECK(published_ops->allocate_irqs(published_host, &endpoint, DRV_PCI_IRQ_MSI, 1U, 1U, &irq, &count) == ENOTSUP);
	CHECK(published_ops->allocate_irqs(published_host, &endpoint, DRV_PCI_IRQ_MSIX, 1U, 1U, &irq, &count) == ENOTSUP);
	CHECK(published_ops->allocate_irqs(published_host, &endpoint, DRV_PCI_IRQ_INTX, 2U, 2U, &irq, &count) == ENOTSUP);

	/* Behind a bridge at 1:00.0, INTA of slot 1 swizzles to INTB, interrupt 176. */
	memset(&tertiary, 0, sizeof(tertiary));
	tertiary.parent = &secondary;
	tertiary.bridge = &endpoint;
	memset(&behind, 0, sizeof(behind));
	behind.address.bus = 2U;
	behind.address.device = 1U;
	behind.bus = &tertiary;
	CHECK(published_ops->allocate_irqs(published_host, &behind, DRV_PCI_IRQ_INTX, 1U, 1U, &irq, &count) == 0);
	CHECK(irq.vector == 176U);

	/* A device without a pin gets no interrupt. */
	endpoint_config[0][0x3d] = 0;
	CHECK(published_ops->allocate_irqs(published_host, &endpoint, DRV_PCI_IRQ_INTX, 1U, 1U, &irq, &count) == ENODEV);
	endpoint_config[0][0x3d] = 1U;
}

/* Checks the refusals and the link-down path. */
static void
test_failures(void)
{
	struct drv_pci_brcmstb_config config;
	struct drv_pci_brcmstb *host;
	struct model_options scenario;
	int before;

	/* Configurations the registers cannot express. */
	standard_config(&config);
	config.register_size = 0x9000U;
	CHECK(drv_pci_brcmstb_start(&config, &host) == EINVAL);
	standard_config(&config);
	config.inbound_cpu_base = 0x1000000U;
	CHECK(drv_pci_brcmstb_start(&config, &host) == EINVAL);
	standard_config(&config);
	config.outbound_cpu_base += 0x1000U;
	CHECK(drv_pci_brcmstb_start(&config, &host) == EINVAL);

	/* No link: the start fails after its bounded wait, holds the endpoint in reset, and asks nothing below. */
	memset(&scenario, 0, sizeof(scenario));
	scenario.link_never_up = true;
	scenario.bar0_size = 0x1000U;
	model_reset(&scenario);
	standard_config(&config);
	before = allocations;
	CHECK(drv_pci_brcmstb_start(&config, &host) == ETIMEDOUT);
	CHECK(allocations == before);
	CHECK((registers[REG_SW_INIT / 4U] & SW_INIT_PERST) != 0);
	CHECK(ext_accesses == 0);
	CHECK(now_us >= 200000U);
	CHECK(now_us < 400000U);
	CHECK(lock_depth == 0);

	/* A controller strapped as an endpoint is refused. */
	memset(&scenario, 0, sizeof(scenario));
	scenario.endpoint_mode = true;
	scenario.bar0_size = 0x1000U;
	model_reset(&scenario);
	CHECK(drv_pci_brcmstb_start(&config, &host) == ENODEV);
	CHECK(ext_accesses == 0);

	/* A BAR larger than the window is refused. */
	memset(&scenario, 0, sizeof(scenario));
	scenario.bar0_size = 0x80000000ULL;
	model_reset(&scenario);
	CHECK(drv_pci_brcmstb_start(&config, &host) == ENOSPC);
	CHECK((registers[REG_SW_INIT / 4U] & SW_INIT_PERST) != 0);
}

/* Checks firmware PCI aliases without moving the driver's low CPU backing. */
static void
test_high_dma_alias(
	void)
{
	struct drv_pci_brcmstb_config config;
	struct drv_pci_brcmstb *host;
	struct model_options scenario;
	unsigned index;
	int error;

	/* Models a high inbound PCI base on either larger memory configuration. */
	for (index = 0; index < 2U; index++) {
		memset(&scenario, 0, sizeof(scenario));
		scenario.bar0_size = 0x1000U;
		model_reset(&scenario);
		standard_config(&config);
		config.inbound_pci_base = UINT64_C(0x100000000) << index;

		/* Hardware and the published DMA provider must describe the same bus alias. */
		error = drv_pci_brcmstb_start(&config, &host);
		CHECK(error == 0);
		CHECK(registers[REG_RC_BAR2_LO / 4U] == 17U);
		CHECK(registers[REG_RC_BAR2_HI / 4U] == 1U << index);
		error = drv_pci_brcmstb_publish(host);
		CHECK(error == 0);
		CHECK(published_bus_offset == config.inbound_pci_base);
		CHECK(published_physical_limit == 0x7fffffffU);
		CHECK(published_constraints.address_bits == 64U);
		CHECK(published_constraints.coherent == 0);
		free(host);
		published_host = NULL;
	}

	/* Refuses an inbound region that wraps around the bus address space. */
	standard_config(&config);
	config.inbound_pci_base = UINT64_MAX - 0x1000U;
	error = drv_pci_brcmstb_start(&config, &host);
	CHECK(error == EINVAL);
}

/* A multifunction endpoint gets both functions' BARs, one after the other. */
static void
test_multifunction(void)
{
	struct drv_pci_brcmstb_config config;
	struct drv_pci_brcmstb *host;
	struct model_options scenario;

	memset(&scenario, 0, sizeof(scenario));
	scenario.multifunction = true;
	scenario.bar0_size = 0x1000U;
	model_reset(&scenario);
	standard_config(&config);
	CHECK(drv_pci_brcmstb_start(&config, &host) == 0);
	CHECK(endpoint_bar0[0] == WINDOW_PCI);
	CHECK(endpoint_bar0[1] == WINDOW_PCI + 0x1000U);
	CHECK(bytes_read(rc_config, 0x20U, 4U) == 0xc000c000U);
	free(host);
	allocations--;
}

/* The configuration the firmware's tree describes. */
static void
standard_config(
	struct drv_pci_brcmstb_config *config)
{
	memset(config, 0, sizeof(*config));
	config->register_base = REGISTER_BASE;
	config->register_size = REGISTER_SIZE;
	config->outbound_cpu_base = WINDOW_CPU;
	config->outbound_pci_base = WINDOW_PCI;
	config->outbound_size = WINDOW_SIZE;
	config->inbound_cpu_base = 0;
	config->inbound_pci_base = 0;
	config->inbound_size = 0xc0000000ULL;
	config->intx_irq[0] = 175U;
	config->intx_irq[1] = 176U;
	config->intx_irq[2] = 177U;
	config->intx_irq[3] = 178U;
}

/* Puts the model in the state the firmware may leave the controller in. */
static void
model_reset(
	const struct model_options *scenario)
{
	unsigned function;

	options = *scenario;
	memset(registers, 0, sizeof(registers));
	memset(rc_config, 0, sizeof(rc_config));
	memset(endpoint_config, 0, sizeof(endpoint_config));
	memset(endpoint_bar0, 0, sizeof(endpoint_bar0));
	memset(endpoint_bar1, 0, sizeof(endpoint_bar1));
	ext_index = 0;
	now_us = 0;
	perst_release_us = 0;
	perst_released = false;
	ext_accesses = 0;
	ext_accesses_too_early = 0;
	ext_accesses_link_down = 0;
	event_count = 0;
	mapped_cpu_address = 0;

	/* The firmware left the link up once; the SerDes is off now. */
	registers[REG_HARD_DEBUG / 4U] = HARD_DEBUG_IDDQ;
	registers[REG_MISC_CTRL / 4U] = 0x00300001U;
	registers[REG_RC_BAR1_LO / 4U] = 0xabcdef1fU;
	registers[REG_RC_BAR3_LO / 4U] = 0x1234561fU;
	registers[REG_REVISION / 4U] = 0x0304U;

	/* The root complex's header: Broadcom 2711, a type 1 header, gen 2 x1 link status. */
	bytes_write(rc_config, 0, 4U, 0x271114e4U);
	bytes_write(rc_config, 0x08U, 1U, 0x20U);
	bytes_write(rc_config, 0x0eU, 1U, 0x01U);
	bytes_write(rc_config, 0xacU + 0x12U, 2U, 0x0012U);
	bytes_write(rc_config, REG_VENDOR1, 4U, 0x0000000fU);
	bytes_write(rc_config, REG_ID_VAL3, 4U, 0x5a000000U);

	/* The VL805: VIA 3483, xHCI class, one 64-bit memory BAR, INTA. */
	for (function = 0; function < 2U; function++) {
		bytes_write(endpoint_config[function], 0, 4U, 0x34831106U);
		bytes_write(endpoint_config[function], 0x08U, 4U, 0x0c033001U);
		bytes_write(endpoint_config[function], 0x3dU, 1U, 0x01U);
		if (options.multifunction)
			bytes_write(endpoint_config[function], 0x0eU, 1U, 0x80U);
	}
}

/* Reports whether the modelled link is up at the current virtual time. */
static bool
link_up_now(void)
{
	if (options.link_never_up)
		return false;
	if (!perst_released)
		return false;
	if ((registers[REG_SW_INIT / 4U] & SW_INIT_BRIDGE) != 0)
		return false;
	if ((registers[REG_HARD_DEBUG / 4U] & HARD_DEBUG_IDDQ) != 0)
		return false;
	return now_us >= perst_release_us + 30000U;
}

/* Reads the model at an offset of the register block. */
static uint32_t
model_read(
	unsigned offset,
	unsigned width)
{
	uint32_t value;
	unsigned function;

	/* Rejects the revision access that aborts while the bridge is asleep. */
	if (offset == REG_REVISION) {
		CHECK((registers[REG_SW_INIT / 4U] & SW_INIT_BRIDGE) == 0);
		CHECK((registers[REG_HARD_DEBUG / 4U] & HARD_DEBUG_IDDQ) == 0);
		CHECK(now_us >= 200U);
	}

	/* The root complex's own configuration space; its class comes from ID_VAL3. */
	if (offset < 0x1000U) {
		value = bytes_read(rc_config, REG_ID_VAL3, 4U);
		bytes_write(rc_config, 0x09U, 1U, value & 0xffU);
		bytes_write(rc_config, 0x0aU, 2U, (value >> 8) & 0xffffU);
		return bytes_read(rc_config, offset, width);
	}

	/* The data window: the function the index selects. */
	if (offset >= REG_EXT_CFG_DATA && offset < REG_EXT_CFG_DATA + 0x1000U) {
		ext_accesses++;
		if (!link_up_now())
			ext_accesses_link_down++;
		if (now_us < perst_release_us + 100000U)
			ext_accesses_too_early++;
		function = (ext_index >> 12) & 7U;
		if ((ext_index >> 20) != 1U || ((ext_index >> 15) & 31U) != 0)
			return width == 4U ? 0xffffffffU : (1U << (width * 8U)) - 1U;
		if (function > 1U || (function == 1U && !options.multifunction))
			return width == 4U ? 0xffffffffU : (1U << (width * 8U)) - 1U;
		return endpoint_read(function, offset - REG_EXT_CFG_DATA, width);
	}

	CHECK(width == 4U);

	/* The link state. */
	if (offset == REG_STATUS) {
		value = options.endpoint_mode ? 0 : 0x80U;
		if (link_up_now())
			value |= 0x30U;
		return value;
	}

	return registers[offset / 4U];
}

/* Writes the model at an offset of the register block. */
static void
model_write(
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	unsigned function;

	/* The root complex's own configuration space. */
	if (offset < 0x1000U) {
		bytes_write(rc_config, offset, width, value);
		return;
	}

	/* The data window. */
	if (offset >= REG_EXT_CFG_DATA && offset < REG_EXT_CFG_DATA + 0x1000U) {
		ext_accesses++;
		if (!link_up_now())
			ext_accesses_link_down++;
		if (now_us < perst_release_us + 100000U)
			ext_accesses_too_early++;
		function = (ext_index >> 12) & 7U;
		if ((ext_index >> 20) != 1U || ((ext_index >> 15) & 31U) != 0)
			return;
		if (function > 1U || (function == 1U && !options.multifunction))
			return;
		endpoint_write(function, offset - REG_EXT_CFG_DATA, width, value);
		return;
	}

	CHECK(width == 4U);
	if (offset == REG_EXT_CFG_INDEX)
		ext_index = value;

	/* Endpoint reset release starts the link training. */
	if (offset == REG_SW_INIT && (value & SW_INIT_PERST) == 0 && !perst_released) {
		perst_released = true;
		perst_release_us = now_us;
	}
	if (offset == REG_SW_INIT && (value & SW_INIT_PERST) != 0)
		perst_released = false;

	registers[offset / 4U] = value;
	CHECK(event_count < sizeof(events) / sizeof(events[0]));
	events[event_count].offset = offset;
	events[event_count].value = value;
	events[event_count].time_us = now_us;
	event_count++;
}

/* Reads the VL805's configuration space, with BAR0 and BAR1 decoded. */
static uint32_t
endpoint_read(
	unsigned function,
	unsigned offset,
	unsigned width)
{
	/* BAR0 keeps the address bits its size allows and reports a 64-bit memory BAR. */
	if (offset == 0x10U) {
		CHECK(width == 4U);
		return (uint32_t)endpoint_bar0[function] | 0x4U;
	}
	if (offset == 0x14U) {
		CHECK(width == 4U);
		return endpoint_bar1[function];
	}

	/* BAR2 to BAR5 are not implemented. */
	if (offset >= 0x18U && offset < 0x28U)
		return 0;

	return bytes_read(endpoint_config[function], offset, width);
}

/* Writes the VL805's configuration space. */
static void
endpoint_write(
	unsigned function,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	uint64_t mask;

	/* BAR0 drops the bits below its size; BAR1 holds the upper half. */
	mask = ~(options.bar0_size - 1U);
	if (offset == 0x10U) {
		CHECK(width == 4U);
		endpoint_bar0[function] = (value & 0xfffffff0U) & (uint32_t)mask;
		return;
	}
	if (offset == 0x14U) {
		CHECK(width == 4U);
		endpoint_bar1[function] = value & (uint32_t)(mask >> 32);
		return;
	}
	if (offset >= 0x18U && offset < 0x28U)
		return;

	bytes_write(endpoint_config[function], offset, width, value);
}

/* Reads a little-endian value from a byte array. */
static uint32_t
bytes_read(
	const uint8_t *space,
	unsigned offset,
	unsigned width)
{
	uint32_t value;
	unsigned i;

	value = 0;
	for (i = 0; i < width; i++)
		value |= (uint32_t)space[offset + i] << (8U * i);
	return value;
}

/* Writes a little-endian value into a byte array. */
static void
bytes_write(
	uint8_t *space,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	unsigned i;

	for (i = 0; i < width; i++)
		space[offset + i] = (uint8_t)(value >> (8U * i));
}

/* Finds the first write to an offset, after an event index, whose masked value matches. */
static const struct model_event *
find_event(
	unsigned offset,
	uint32_t mask,
	uint32_t wanted,
	unsigned after)
{
	unsigned i;

	for (i = after; i < event_count; i++) {
		if (events[i].offset != offset)
			continue;
		if ((events[i].value & mask) != wanted)
			continue;
		return &events[i];
	}
	return NULL;
}

/* Reports the position of an event in the log. */
static unsigned
event_index(
	const struct model_event *event)
{
	return (unsigned)(event - events);
}

/* Reads a whole file into memory. */
static unsigned char *
read_file(
	const char *path,
	size_t *size)
{
	unsigned char *bytes;
	FILE *file;
	long length;

	file = fopen(path, "rb");
	if (file == NULL) {
		perror(path);
		exit(EXIT_FAILURE);
	}
	fseek(file, 0, SEEK_END);
	length = ftell(file);
	fseek(file, 0, SEEK_SET);
	bytes = malloc((size_t)length);
	CHECK(bytes != NULL);
	CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
	fclose(file);
	*size = (size_t)length;
	return bytes;
}

/*
 * The kernel services the driver calls.
 */

uint8_t
kern_mmio_read8(
	const volatile void *address)
{
	return (uint8_t)model_read((unsigned)((const volatile uint8_t *)address - (const volatile uint8_t *)registers), 1U);
}

uint16_t
kern_mmio_read16(
	const volatile void *address)
{
	return (uint16_t)model_read((unsigned)((const volatile uint8_t *)address - (const volatile uint8_t *)registers), 2U);
}

uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	return model_read((unsigned)((const volatile uint8_t *)address - (const volatile uint8_t *)registers), 4U);
}

void
kern_mmio_write8(
	volatile void *address,
	uint8_t value)
{
	model_write((unsigned)((volatile uint8_t *)address - (volatile uint8_t *)registers), 1U, value);
}

void
kern_mmio_write16(
	volatile void *address,
	uint16_t value)
{
	model_write((unsigned)((volatile uint8_t *)address - (volatile uint8_t *)registers), 2U, value);
}

void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	model_write((unsigned)((volatile uint8_t *)address - (volatile uint8_t *)registers), 4U, value);
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	CHECK(attributes == KERN_DEVICE_UNCACHED);
	if (address == REGISTER_BASE) {
		CHECK(size == REGISTER_SIZE);
		*mapped = registers;
		return 0;
	}
	CHECK(address >= WINDOW_CPU);
	CHECK(size <= sizeof(bar_memory));
	mapped_cpu_address = address;
	*mapped = bar_memory;
	return 0;
}

int
kern_device_unmap(
	void *mapped,
	size_t size)
{
	(void)mapped;
	(void)size;
	return 0;
}

void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	CHECK(min_us <= max_us);
	now_us += min_us;
}

void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("  log: ");
	vprintf(format, arguments);
	va_end(arguments);
}

void *
kern_malloc(
	size_t size)
{
	allocations++;
	return malloc(size);
}

void
kern_free(
	void *pointer)
{
	if (pointer != NULL)
		allocations--;
	free(pointer);
}

void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->rank = rank;
	lock->name = name;
}

unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	(void)lock;
	CHECK(lock_depth == 0);
	lock_depth++;
	return 1;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	(void)lock;
	CHECK(enabled == 1);
	CHECK(lock_depth == 1);
	lock_depth--;
}

/* Captures bus translation while preserving the ordinary DMA fixture. */
int
drv_dma_device_create_window(
	const struct drv_dma_constraints *constraints,
	uint64_t bus_offset,
	uint64_t physical_limit,
	struct drv_dma_device **result)
{
	int error;

	/* Creates the fixture owner before publishing its translated window. */
	error = drv_dma_device_create(constraints, result);
	if (error != 0)
		return error;

	/* Records the parameters handed to the production DMA implementation. */
	published_bus_offset = bus_offset;
	published_physical_limit = physical_limit;

	/* Succeeded: the fake PCI core can observe the complete DMA contract. */
	return 0;
}

int
drv_dma_device_create(
	const struct drv_dma_constraints *constraints,
	struct drv_dma_device **result)
{
	published_constraints = *constraints;
	*result = (struct drv_dma_device *)&dma_token;
	return 0;
}

int
drv_dma_device_destroy(
	struct drv_dma_device *device)
{
	(void)device;
	return 0;
}

int
drv_pci_bus_create_root(
	uint16_t segment,
	uint8_t number,
	const struct drv_pci_bus_ops *ops,
	void *host,
	struct drv_dma_device *dma,
	struct drv_pci_bus **result)
{
	CHECK(segment == 0);
	CHECK(number == 0);
	CHECK(dma == (struct drv_dma_device *)&dma_token);
	CHECK(ops->config_space_size == 4096U);
	published_ops = ops;
	published_host = host;
	*result = &root_bus;
	return 0;
}

int
drv_pci_bus_scan_tree(
	struct drv_pci_bus *bus)
{
	CHECK(bus == &root_bus);
	scans++;
	return 0;
}

int
drv_pci_device_config_read8(
	struct drv_pci_device *device,
	unsigned offset,
	uint8_t *value)
{
	uint32_t read;
	int error;

	/* The model has nothing below bus 1; a device there uses INTA. */
	if (device->address.bus > 1U && offset == 0x3dU) {
		*value = 1U;
		return 0;
	}

	error = published_ops->config_read(published_host, &device->address, offset, 1U, &read);
	*value = (uint8_t)read;
	return error;
}

struct drv_pci_bus *
drv_pci_device_bus(
	const struct drv_pci_device *device)
{
	return device->bus;
}

struct drv_pci_bus *
drv_pci_bus_parent(
	const struct drv_pci_bus *bus)
{
	return bus->parent;
}

struct drv_pci_device *
drv_pci_bus_bridge(
	const struct drv_pci_bus *bus)
{
	return bus->bridge;
}

void
drv_pci_device_address(
	const struct drv_pci_device *device,
	struct drv_pci_address *address)
{
	*address = device->address;
}
