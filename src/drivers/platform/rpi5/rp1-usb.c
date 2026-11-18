/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's USB ports: RP1's two DWC3 controllers.
 *
 * A DWC3 is an xHCI whose registers start at its base, followed by the
 * core's own global registers.  The module puts each core in host mode
 * and then shows the controllers to the PCI core as the functions of a
 * root bus that exists only here: each function's configuration space is
 * kept in memory, and its one BAR is the controller's registers.  The
 * xHCI driver binds to the functions by their class, as to any xHCI.
 *
 * The functions' interrupt is a pin of this host.  The host serves it by
 * calling the PCI core's line from the controller's RP1 interrupt, and
 * from a polling thread: often when the interrupt could not be routed,
 * rarely otherwise, so an edge that came while the pin was masked is not
 * lost.
 */

#include <drivers/platform/rpi5/rp1.h>
#include <drivers/platform/rpi5/rp1-gpio.h>
#include <drivers/platform/rpi5/rp1-irq.h>
#include <drivers/platform/rpi5/rp1-usb.h>
#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-xhci.h>
#include <drivers/usb/usb.h>
#include <drivers/usb/usb-hid.h>
#include <drivers/usb/usb-uvc.h>
#include <drivers/usb/usb-hub.h>
#include <drivers/usb/usb-storage.h>
#include <drivers/usb/usb-uas.h>
#include <drivers/usb/usb-cdc-ncm.h>
#include <drivers/usb/usb-cdc-ecm.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/irq.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/pmem.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <uapi/errno.h>

/* The most controllers RP1 has. */
#define RP1_USB_MAX			2U

/*
 * The DWC3 core's global registers the module sets: the global control
 * register and its port-capability direction, the user control register
 * 1, the USB 2.0 PHY configuration and its turnaround time and interface
 * width, and the core's identity.
 *
 * Values referenced: OpenBSD sys/dev/fdt/xhci_fdt.c rev 1.32 (USB3_GCTL,
 * USB3_GCTL_PRTCAPDIR_MASK, USB3_GCTL_PRTCAPDIR_HOST, USB3_GUCTL1,
 * USB3_GUSB2PHYCFG0, USB3_GUSB2PHYCFG0_USBTRDTIM, USB3_GUSB2PHYCFG0_PHYIF,
 * the turnaround time 9 for an 8-bit UTMI interface) and FreeBSD
 * sys/dev/usb/controller/dwc3/dwc3.h (DWC3_GSNPSID).
 */
#define DWC3_GCTL			0xc110U
#define DWC3_GCTL_PRTCAPDIR_MASK	(0x3U << 12)
#define DWC3_GCTL_PRTCAPDIR_HOST	(0x1U << 12)
#define DWC3_GSNPSID			0xc120U
#define DWC3_GUSB2PHYCFG0		0xc200U
#define DWC3_GUSB2PHYCFG0_USBTRDTIM_MASK	(0xfU << 10)
#define DWC3_GUSB2PHYCFG0_USBTRDTIM_8BIT	(0x9U << 10)
#define DWC3_GUSB2PHYCFG0_PHYIF		(1U << 3)

/* The bytes of a controller's registers the core initialization touches. */
#define DWC3_GLOBAL_BYTES		0xc300U

/*
 * The VBUS functions of RP1's pins, by name: the pin and its function
 * selector.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pinctrl/pinctrl-rp1.c (rp1_gpio_pin_funcs: vbus0 is the fifth
 * function of pins 28 and 29, vbus1 the third of pins 42 and 43).
 */
#define RP1_USB_VBUS_PIN_COUNT		4U

/*
 * The configuration space every function shows: RP1's vendor and product
 * (the controllers are part of it), the xHCI class, a 64-bit memory BAR,
 * and interrupt pin A.
 */
#define RP1_USB_IDENTITY		0x00011de4U
#define RP1_USB_CLASS_XHCI		0x0c0330U
#define RP1_USB_BAR_TYPE_64BIT		0x4U
#define RP1_USB_INTERRUPT_PIN_A		1U

/* The configuration registers the host keeps for a function. */
#define PCI_ID				0x00U
#define PCI_COMMAND			0x04U
#define PCI_CLASS_REVISION		0x08U
#define PCI_BAR0			0x10U
#define PCI_BAR0_HIGH			0x14U
#define PCI_SUBSYSTEM			0x2cU
#define PCI_INTERRUPT			0x3cU
#define PCI_INTERRUPT_PIN_SHIFT		8U
#define PCI_CLASS_SHIFT			8U

/*
 * The command bits a function keeps: memory decode, bus mastering and the
 * legacy interrupt's disable.  The rest read as zero.
 */
#define PCI_COMMAND_MEMORY		0x0002U
#define PCI_COMMAND_MASTER		0x0004U
#define PCI_COMMAND_INTX_DISABLE	0x0400U
#define PCI_COMMAND_KEPT		(PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INTX_DISABLE)

/* The host's root bus. */
#define RP1_USB_SEGMENT			0U
#define RP1_USB_BUS			0U

/* The DMA constraints of a controller behind RP1: no snooping is assumed unless the tree says so. */
#define RP1_USB_DMA_MAX_SEGMENT		(16U * 1024U * 1024U)
#define RP1_USB_DMA_ADDRESS_BITS	64U

/*
 * How often the polling thread serves the controllers, in milliseconds:
 * when a controller has no interrupt, and when every one has.
 */
#define RP1_USB_POLL_MS			2U
#define RP1_USB_SAFETY_POLL_MS		50U

/* How long a removal waits for a running service, in steps of a millisecond. */
#define RP1_USB_DRAIN_POLLS		1000U
#define RP1_USB_DRAIN_POLL_US		1000U

/*
 * One controller shown as a PCI function.
 *
 * Filled by drv_rp1_usb_init() before the root bus exists and constant
 * afterwards (present is set once the core is a host; a function without
 * it reads as absent) except for the configuration registers (command, BAR) the
 * PCI core and the xHCI driver write, and the pin's entry.  The host's
 * lock guards the configuration registers and the pin's entry.
 */
struct rp1_usb_function {
	int present;
	int interrupt;
	uint32_t node;
	uint64_t pci_base;
	uint64_t cpu_base;
	uint64_t size;
	uint32_t snpsid;
	uint16_t command;
	uint32_t bar_low;
	uint32_t bar_high;
	drv_pci_intx_run_t run;
	void *line;
	int pin_enabled;
	unsigned in_flight;
};

/*
 * The host of the controllers' root bus.
 *
 * It lives for as long as the kernel runs: the PCI core keeps it as the
 * host of the root bus.  started is set once the root bus is published,
 * polling once the polling thread exists, and poll_ms, the thread's
 * interval, before it starts.
 */
struct rp1_usb_host {
	struct spinlock lock;
	struct rp1_usb_function functions[RP1_USB_MAX];
	unsigned count;
	struct drv_dma_device *dma;
	struct drv_pci_bus *root;
	struct thread *poll_thread;
	int started;
	int polling;
	unsigned poll_ms;
};

/* One pin's VBUS function. */
struct rp1_usb_vbus_pin {
	unsigned pin;
	const char *function;
	unsigned selector;
};

static int host_config_read(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t *value);
static int host_config_write(void *context, const struct drv_pci_address *address, unsigned offset, unsigned width, uint32_t value);
static int host_map_bar(void *context, struct drv_pci_device *device, const struct drv_pci_bar *bar, unsigned flags, struct drv_pci_mapping *mapping);
static void host_unmap_bar(void *context, struct drv_pci_mapping *mapping);
static int host_allocate_irqs(void *context, struct drv_pci_device *device, enum drv_pci_irq_type type, unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs, unsigned *count);
static void host_free_irqs(void *context, struct drv_pci_device *device, struct drv_pci_irq *irqs, unsigned count);
static int host_intx_establish(void *context, unsigned vector, drv_pci_intx_run_t run, void *line);
static int host_intx_disestablish(void *context, unsigned vector, drv_pci_intx_run_t run, void *line);
static void host_intx_mask(void *context, unsigned vector);
static void host_intx_unmask(void *context, unsigned vector);

/*
 * The operations the PCI core calls on the host's root bus.
 *
 * The table is constant; the host arrives as the context of each call.
 */
static const struct drv_pci_bus_ops rp1_usb_bus_ops = {
	.config_space_size = 256,
	.config_read = host_config_read,
	.config_write = host_config_write,
	.map_bar = host_map_bar,
	.unmap_bar = host_unmap_bar,
	.allocate_irqs = host_allocate_irqs,
	.free_irqs = host_free_irqs,
	.intx_establish = host_intx_establish,
	.intx_disestablish = host_intx_disestablish,
	.intx_mask = host_intx_mask,
	.intx_unmask = host_intx_unmask
};

/*
 * The pins that can carry a controller's VBUS control.
 *
 * Values referenced: Raspberry Pi Linux rpi-6.12.y
 * drivers/pinctrl/pinctrl-rp1.c (rp1_gpio_pin_funcs, rows 28, 29, 42, 43).
 */
static const struct rp1_usb_vbus_pin rp1_usb_vbus_pins[RP1_USB_VBUS_PIN_COUNT] = {
	{ 28U, "vbus0", 4U },
	{ 29U, "vbus0", 4U },
	{ 42U, "vbus1", 2U },
	{ 43U, "vbus1", 2U },
};

/* The one host of RP1's controllers. */
static struct rp1_usb_host rp1_usb;

static int find_controllers(void);
static int is_host_controller(uint32_t node);
static int start_core(struct rp1_usb_function *function);
static void connect_vbus(uint32_t node);
static void connect_vbus_group(uint32_t group);
static int parse_gpio_name(const char *name, uint32_t length, unsigned *pin);
static int publish_bus(void);
static void register_usb_drivers(void);
static struct rp1_usb_function *function_at(const struct drv_pci_address *address);
static uint32_t function_dword(const struct rp1_usb_function *function, unsigned offset);
static void function_store(struct rp1_usb_function *function, unsigned offset, uint32_t dword);
static void establish_interrupts(void);
static void controller_interrupt(void *argument);
static void start_polling(void);
static void poll_worker(void *argument);
static void serve_function(struct rp1_usb_function *function);

/*
 * Starts the USB core and RP1's controllers.
 */
int
drv_rp1_usb_init(
	void)
{
	unsigned index;
	int error;

	/* Brings the PCI core into service. */
	error = drv_pci_init();
	if (error != 0 && error != EALREADY)
		return error;

	/* Registers the USB drivers, so publishing the bus binds the controllers. */
	register_usb_drivers();

	/* Prepares the lock of the configuration registers and the pins. */
	spin_init(&rp1_usb.lock, LOCK_RANK_DEVICE, "RP1 USB");

	/* Finds the controllers among RP1's peripherals. */
	error = find_controllers();
	if (error != 0)
		return error;

	/* Puts each controller in host mode; one that cannot be stays off the bus. */
	for (index = 0; index < rp1_usb.count; index++) {
		error = start_core(&rp1_usb.functions[index]);
		if (error != 0) {
			kern_logf("rp1-usb: controller %u unavailable (%d)\n", index, error);
			continue;
		}

		/* Shows the controller on the bus. */
		rp1_usb.functions[index].present = 1;
	}

	/* Shows the controllers to the PCI core, which binds the xHCI driver. */
	error = publish_bus();
	if (error != 0) {
		kern_logf("rp1-usb: bus not published (%d)\n", error);
		return error;
	}

	/* Succeeded: the controllers are bound or reported. */
	return 0;
}

/*
 * Starts serving the controllers' interrupts and has them look at their
 * root ports.
 */
void
drv_rp1_usb_refresh(
	void)
{
	/* Leaves a board without the bus alone. */
	if (!rp1_usb.started) {
		kern_logf("rp1-usb: no controllers to start\n");
		return;
	}

	/* Routes the controllers' RP1 interrupts, then starts the polling service. */
	establish_interrupts();
	start_polling();

	/* Has each xHCI controller look at its root ports. */
	drv_pci_xhci_probe_roots();
}

/*
 * Finds the DWC3 controllers under the rp1 node, in host mode, and where
 * their registers are on the PCI bus and the CPU's.
 */
static int
find_controllers(
	void)
{
	struct rp1_usb_function *function;
	const struct drv_fdt *fdt;
	const uint8_t *value;
	uint64_t rp1_address;
	uint64_t pci_address;
	uint64_t cpu_address;
	uint64_t size;
	uint32_t length;
	uint32_t node;
	bool enabled;
	int host;
	int error;

	/* Walks RP1's DWC3 nodes in the device tree's order. */
	fdt = drv_rp1_fdt();
	node = DRV_FDT_NO_NODE;
	for (;;) {
		error = drv_rp1_find_child("snps,dwc3", node, &node);
		if (error != 0)
			break;

		/* Skips a controller the tree turned off. */
		enabled = drv_fdt_node_enabled(fdt, node);
		if (!enabled)
			continue;

		/* Skips a controller not meant to be a host. */
		host = is_host_controller(node);
		if (!host)
			continue;

		/* Stops at the most the host can show. */
		if (rp1_usb.count == RP1_USB_MAX)
			break;

		/* Finds the controller's registers on the CPU's bus. */
		error = drv_rp1_reg(node, 0, &cpu_address, &size);
		if (error != 0) {
			kern_logf("rp1-usb: unusable reg (%d)\n", error);
			continue;
		}

		/* Finds them on the PCI bus too, which the BAR shows. */
		error = drv_fdt_property(fdt, node, "reg", &value, &length);
		if (error != 0 || length < 8U)
			continue;
		rp1_address = drv_fdt_cells_value(value, 0, 2U);
		error = drv_rp1_to_pci(rp1_address, &pci_address);
		if (error != 0)
			continue;

		/* Refuses registers too small for the core's globals or not a power of two. */
		if (size < DWC3_GLOBAL_BYTES || (size & (size - 1U)) != 0) {
			kern_logf("rp1-usb: registers of %llx bytes not usable\n", (unsigned long long)size);
			continue;
		}

		/* Records the controller; its BAR is where its registers are on the PCI bus. */
		function = &rp1_usb.functions[rp1_usb.count];
		kern_memset(function, 0, sizeof(*function));
		function->node = node;
		function->pci_base = pci_address;
		function->cpu_base = cpu_address;
		function->size = size;
		function->bar_low = (uint32_t)pci_address;
		function->bar_high = (uint32_t)(pci_address >> 32);
		rp1_usb.count++;
	}

	/* Refuses an RP1 without any controller. */
	if (rp1_usb.count == 0) {
		kern_logf("rp1-usb: no USB controller in the device tree\n");
		return ENODEV;
	}

	/* Succeeded: the controllers are known. */
	return 0;
}

/* Tells whether a controller's dr_mode lets it be a host; a node without one can be. */
static int
is_host_controller(
	uint32_t node)
{
	const struct drv_fdt *fdt;
	const uint8_t *value;
	uint32_t length;
	int differs;
	int error;

	/* A controller without a mode is a host by default. */
	fdt = drv_rp1_fdt();
	error = drv_fdt_property(fdt, node, "dr_mode", &value, &length);
	if (error != 0)
		return 1;

	/* Refuses a mode too short to be "host". */
	if (length < sizeof("host"))
		return 0;

	/* Refuses another mode. */
	differs = kern_memcmp(value, "host", sizeof("host"));
	if (differs != 0)
		return 0;

	/* Succeeded: the controller is a host. */
	return 1;
}

/*
 * Puts one DWC3 core in host mode and sets its USB 2.0 PHY interface,
 * after connecting its VBUS pins.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/xhci_fdt.c rev 1.32
 * (xhci_snps_init(): force the port capability to host, then set the
 * USB 2.0 PHY's turnaround time for its width).
 */
static int
start_core(
	struct rp1_usb_function *function)
{
	volatile uint8_t *registers;
	void *mapped;
	uint32_t bits;
	int error;

	/* Gives the controller its VBUS pins, when the tree names them. */
	connect_vbus(function->node);

	/* Maps the controller's registers. */
	error = kern_device_map(function->cpu_base, (size_t)function->size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;
	registers = mapped;

	/* Reads the core's identity, which a missing core reads as all ones. */
	function->snpsid = kern_mmio_read32(registers + DWC3_GSNPSID);
	if (function->snpsid == 0xffffffffU || function->snpsid == 0) {
		kern_logf("rp1-usb: no DWC3 at %llx (identity %08x)\n",
			  (unsigned long long)function->cpu_base,
			  function->snpsid);
		(void)kern_device_unmap(mapped, (size_t)function->size);
		return ENODEV;
	}

	/* Makes the core a host. */
	bits = kern_mmio_read32(registers + DWC3_GCTL);
	bits &= ~DWC3_GCTL_PRTCAPDIR_MASK;
	bits |= DWC3_GCTL_PRTCAPDIR_HOST;
	kern_mmio_write32(registers + DWC3_GCTL, bits);

	/* Sets the USB 2.0 PHY's 8-bit interface and its turnaround time. */
	bits = kern_mmio_read32(registers + DWC3_GUSB2PHYCFG0);
	bits &= ~DWC3_GUSB2PHYCFG0_USBTRDTIM_MASK;
	bits &= ~DWC3_GUSB2PHYCFG0_PHYIF;
	bits |= DWC3_GUSB2PHYCFG0_USBTRDTIM_8BIT;
	kern_mmio_write32(registers + DWC3_GUSB2PHYCFG0, bits);

	/* Reports the core. */
	kern_logf("rp1-usb: DWC3 %08x at %llx in host mode\n",
		  function->snpsid,
		  (unsigned long long)function->cpu_base);

	/* Releases the mapping; the xHCI driver maps the BAR itself. */
	(void)kern_device_unmap(mapped, (size_t)function->size);

	/* Succeeded: the xHCI registers work. */
	return 0;
}

/*
 * Connects the VBUS pins a controller's pinctrl-0 names, so the ports'
 * power switch follows the controller.
 */
static void
connect_vbus(
	uint32_t node)
{
	const struct drv_fdt *fdt;
	const uint8_t *value;
	uint32_t length;
	uint32_t phandle;
	uint32_t group;
	unsigned index;
	int error;

	/* Reads the controller's pin groups; a controller without them has nothing to connect. */
	fdt = drv_rp1_fdt();
	error = drv_fdt_property(fdt, node, "pinctrl-0", &value, &length);
	if (error != 0)
		return;

	/* Starts the GPIO block the pins are in. */
	error = drv_rp1_gpio_init();
	if (error != 0)
		return;

	/* Connects each group the property names. */
	for (index = 0; (index + 1U) * 4U <= length; index++) {
		phandle = (uint32_t)drv_fdt_cells_value(value, index, 1U);
		error = drv_fdt_find_phandle(fdt, phandle, &group);
		if (error != 0)
			continue;

		/* Connects the group's pins to its function. */
		connect_vbus_group(group);
	}
}

/* Connects the pins of one pin group whose function is a VBUS one. */
static void
connect_vbus_group(
	uint32_t group)
{
	const struct drv_fdt *fdt;
	const uint8_t *function;
	const uint8_t *pins;
	uint32_t function_length;
	uint32_t pins_length;
	uint32_t start;
	uint32_t end;
	unsigned pin;
	unsigned entry;
	int matches;
	int error;

	/* Reads the group's function and its pins. */
	fdt = drv_rp1_fdt();
	error = drv_fdt_property(fdt, group, "function", &function, &function_length);
	if (error != 0)
		return;
	error = drv_fdt_property(fdt, group, "pins", &pins, &pins_length);
	if (error != 0)
		return;

	/* Walks the pin names, each a string of the list. */
	start = 0;
	while (start < pins_length) {
		/* Finds the end of this name. */
		end = start;
		while (end < pins_length && pins[end] != '\0')
			end++;

		/* Reads the pin's number; a name that is not gpioN is skipped. */
		error = parse_gpio_name((const char *)pins + start, end - start, &pin);
		start = end + 1U;
		if (error != 0)
			continue;

		/* Looks for the pin's selector of the group's function. */
		for (entry = 0; entry < RP1_USB_VBUS_PIN_COUNT; entry++) {
			/* Skips another pin. */
			if (rp1_usb_vbus_pins[entry].pin != pin)
				continue;

			/* Skips another function of the pin. */
			matches = kern_strcmp((const char *)function, rp1_usb_vbus_pins[entry].function);
			if (matches != 0)
				continue;

			/* Connects the pin. */
			error = drv_rp1_gpio_set_function(pin, rp1_usb_vbus_pins[entry].selector);
			if (error != 0)
				kern_logf("rp1-usb: VBUS pin %u not connected (%d)\n", pin, error);
		}
	}
}

/* Reads a pin's number from a name of the form gpioN. */
static int
parse_gpio_name(
	const char *name,
	uint32_t length,
	unsigned *pin)
{
	unsigned number;
	uint32_t index;
	int differs;

	/* Refuses a name without the prefix or without a number. */
	if (length <= 4U)
		return EINVAL;
	differs = kern_memcmp(name, "gpio", 4U);
	if (differs != 0)
		return EINVAL;

	/* Reads the decimal digits. */
	number = 0;
	for (index = 4U; index < length; index++) {
		/* Refuses anything that is not a digit. */
		if (name[index] < '0' || name[index] > '9')
			return EINVAL;
		number = number * 10U + (unsigned)(name[index] - '0');
	}

	/* Succeeded: the pin's number. */
	*pin = number;
	return 0;
}

/*
 * Creates the root bus with the controllers as its functions and has the
 * PCI core enumerate it.
 */
static int
publish_bus(
	void)
{
	struct drv_dma_constraints constraints;
	uint64_t bus_offset;
	uint64_t physical_limit;
	int coherent;
	int error;

	/* Reads the addresses RP1's peripherals use for memory. */
	error = drv_rp1_dma_window(&bus_offset, &physical_limit, &coherent);
	if (error != 0)
		return error;

	/* Describes the controllers' DMA: the window, 64-bit addresses, coherence as the tree says. */
	kern_memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = RP1_USB_DMA_ADDRESS_BITS;
	constraints.max_segment_size = RP1_USB_DMA_MAX_SEGMENT;
	constraints.segment_boundary = 0;
	constraints.coherent = coherent;
	error = drv_dma_device_create_window(&constraints, bus_offset, physical_limit, &rp1_usb.dma);
	if (error != 0)
		return error;

	/* Creates the root bus. */
	error = drv_pci_bus_create_root(RP1_USB_SEGMENT, RP1_USB_BUS, &rp1_usb_bus_ops, &rp1_usb, rp1_usb.dma, &rp1_usb.root);
	if (error != 0) {
		(void)drv_dma_device_destroy(rp1_usb.dma);
		rp1_usb.dma = NULL;
		return error;
	}

	/* Marks the bus published: refresh serves it from now on. */
	rp1_usb.started = 1;

	/* Enumerates the functions, which attaches the xHCI driver. */
	error = drv_pci_bus_scan_tree(rp1_usb.root);
	if (error != 0)
		return error;

	/* Succeeded: the PCI core owns the bus. */
	return 0;
}

/*
 * Registers the USB core and the USB drivers the configuration selects.
 *
 * They must be known before the PCI core enumerates the bus, because
 * enumeration binds the xHCI controllers and they then find their devices.
 * A failure is recorded; the boot goes on without that driver.
 */
static void
register_usb_drivers(
	void)
{
#if CONFIG_DRIVER_PCI_XHCI
	int error;

	/* Brings up the USB core. */
	error = drv_usb_init();
	if (error != 0) {
		kern_logf("usb: core initialization failed (%d)\n", error);
		return;
	}

	/* Registers bulk-only USB disks. */
#if CONFIG_DRIVER_USB_STORAGE
	error = drv_usb_storage_driver_register();
	if (error != 0)
		kern_logf("usb: storage registration failed (%d)\n", error);
#endif

	/* Registers USB Attached SCSI disks. */
#if CONFIG_DRIVER_USB_STORAGE
	error = drv_usb_uas_driver_register();
	if (error != 0)
		kern_logf("usb: UAS registration failed (%d)\n", error);
#endif

	/* Registers USB NCM network interfaces. */
#if CONFIG_DRIVER_USB_CDC_NCM
	error = drv_usb_cdc_ncm_driver_register();
	if (error != 0)
		kern_logf("usb: CDC NCM registration failed (%d)\n", error);
#endif

	/* Registers USB ECM network interfaces. */
#if CONFIG_DRIVER_USB_CDC_ECM
	error = drv_usb_cdc_ecm_driver_register();
	if (error != 0)
		kern_logf("usb: CDC ECM registration failed (%d)\n", error);
#endif

	/* Registers the USB Video Class cameras. */
#if CONFIG_DRIVER_USB_UVC
	error = drv_usb_uvc_driver_register();
	if (error != 0)
		kern_logf("usb: video class registration failed (%d)\n", error);
#endif

	/* Registers the keyboard and mouse driver. */
#if CONFIG_DRIVER_USB_HID
	error = drv_usb_hid_driver_register();
	if (error != 0)
		kern_logf("usb: HID input driver registration failed (%d)\n", error);
#endif

	/* Registers the hub driver, for keyboards and disks behind a hub. */
#if CONFIG_DRIVER_USB_HUB
	error = drv_usb_hub_driver_register();
	if (error != 0)
		kern_logf("usb: hub driver registration failed (%d)\n", error);
#endif

	/* Registers the host controller driver. */
	error = drv_pci_xhci_driver_register();
	if (error != 0)
		kern_logf("usb: xHCI PCI driver registration failed (%d)\n", error);
#endif
}

/* Finds the function an address names, or NULL for one the bus does not have. */
static struct rp1_usb_function *
function_at(
	const struct drv_pci_address *address)
{
	/* Only device n, function 0, of the one root bus exists. */
	if (address == NULL)
		return NULL;
	if (address->segment != RP1_USB_SEGMENT || address->bus != RP1_USB_BUS)
		return NULL;
	if (address->function != 0 || address->device >= rp1_usb.count)
		return NULL;

	/* A controller that is not a host is not shown. */
	if (!rp1_usb.functions[address->device].present)
		return NULL;

	/* Succeeded: the controller shown at the address. */
	return &rp1_usb.functions[address->device];
}

/*
 * Reads one dword of a function's configuration space; the caller holds
 * the host's lock.
 */
static uint32_t
function_dword(
	const struct rp1_usb_function *function,
	unsigned offset)
{
	uint32_t size_mask;
	uint32_t dword;

	/* Answers each register the function has; the others read as zero. */
	switch (offset) {
	case PCI_ID:
	case PCI_SUBSYSTEM:
		dword = RP1_USB_IDENTITY;
		break;
	case PCI_COMMAND:
		dword = function->command;
		break;
	case PCI_CLASS_REVISION:
		dword = RP1_USB_CLASS_XHCI << PCI_CLASS_SHIFT;
		break;
	case PCI_BAR0:
		/* The BAR keeps only the bits above its size, as the sizing probe expects. */
		size_mask = ~((uint32_t)function->size - 1U);
		dword = (function->bar_low & size_mask) | RP1_USB_BAR_TYPE_64BIT;
		break;
	case PCI_BAR0_HIGH:
		dword = function->bar_high;
		break;
	case PCI_INTERRUPT:
		dword = RP1_USB_INTERRUPT_PIN_A << PCI_INTERRUPT_PIN_SHIFT;
		break;
	default:
		dword = 0;
		break;
	}

	/* Succeeded: the register's value. */
	return dword;
}

/*
 * Stores a written dword into a function's configuration space; the caller
 * holds the host's lock.  Only the command and the BAR are writable.
 */
static void
function_store(
	struct rp1_usb_function *function,
	unsigned offset,
	uint32_t dword)
{
	/* Keeps the writable registers; the others ignore writes. */
	switch (offset) {
	case PCI_COMMAND:
		function->command = (uint16_t)(dword & PCI_COMMAND_KEPT);
		break;
	case PCI_BAR0:
		function->bar_low = dword & ~0xfU;
		break;
	case PCI_BAR0_HIGH:
		function->bar_high = dword;
		break;
	default:
		break;
	}
}

/* Reads a function's configuration space at any width. */
static int
host_config_read(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t *value)
{
	struct rp1_usb_function *function;
	unsigned long enabled;
	uint32_t dword;
	unsigned shift;

	(void)context;

	/* Refuses a malformed access. */
	if (value == NULL || offset >= 256U)
		return EINVAL;
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;
	if ((offset & (width - 1U)) != 0)
		return EINVAL;

	/* An absent function reads as all ones. */
	function = function_at(address);
	if (function == NULL) {
		*value = 0xffffffffU;
		if (width < 4U)
			*value &= (1U << (width * 8U)) - 1U;
		return 0;
	}

	/* Reads the dword the access is in. */
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	dword = function_dword(function, offset & ~3U);

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);

	/* Narrows it to the access. */
	shift = (offset & 3U) * 8U;
	dword >>= shift;
	if (width < 4U)
		dword &= (1U << (width * 8U)) - 1U;

	/* Succeeded: the register's value. */
	*value = dword;
	return 0;
}

/* Writes a function's configuration space at any width. */
static int
host_config_write(
	void *context,
	const struct drv_pci_address *address,
	unsigned offset,
	unsigned width,
	uint32_t value)
{
	struct rp1_usb_function *function;
	unsigned long enabled;
	uint32_t dword;
	uint32_t mask;
	unsigned shift;

	(void)context;

	/* Refuses a malformed access. */
	if (offset >= 256U)
		return EINVAL;
	if (width != 1U && width != 2U && width != 4U)
		return EINVAL;
	if ((offset & (width - 1U)) != 0)
		return EINVAL;

	/* An absent function ignores writes. */
	function = function_at(address);
	if (function == NULL)
		return 0;

	/* Finds the bytes of the dword the access writes. */
	shift = (offset & 3U) * 8U;
	mask = 0xffffffffU;
	if (width < 4U)
		mask = ((1U << (width * 8U)) - 1U) << shift;

	/* Merges the access into its dword and stores the dword. */
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	dword = function_dword(function, offset & ~3U);
	dword &= ~mask;
	dword |= (value << shift) & mask;
	function_store(function, offset & ~3U, dword);

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);

	/* Succeeded: the register holds the value. */
	return 0;
}

/* Maps part of a function's BAR: the controller's registers. */
static int
host_map_bar(
	void *context,
	struct drv_pci_device *device,
	const struct drv_pci_bar *bar,
	unsigned flags,
	struct drv_pci_mapping *mapping)
{
	struct drv_pci_address address;
	struct rp1_usb_function *function;
	uint64_t offset;
	uint64_t cpu_address;
	void *mapped;
	int error;

	(void)context;
	(void)flags;

	/* Refuses missing arguments and anything but the function's memory BAR. */
	if (device == NULL || bar == NULL || mapping == NULL)
		return EINVAL;
	if (bar->index != 0 || bar->size == 0)
		return EINVAL;
	drv_pci_device_address(device, &address);
	function = function_at(&address);
	if (function == NULL)
		return EINVAL;

	/* Refuses a range outside the controller's registers. */
	if (bar->bus_address < function->pci_base)
		return EINVAL;
	offset = bar->bus_address - function->pci_base;
	if (offset >= function->size || bar->size > function->size - offset)
		return EINVAL;

	/* Maps the registers uncached. */
	cpu_address = function->cpu_base + offset;
	error = kern_device_map(cpu_address, (size_t)bar->size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Describes the mapping; the first private word marks it as owned here. */
	mapping->address = mapped;
	mapping->physical_address = cpu_address;
	mapping->size = (size_t)bar->size;
	mapping->type = bar->type;
	mapping->private_data[0] = 1U;
	mapping->private_data[1] = 0;

	/* Succeeded: the driver reaches the controller at address. */
	return 0;
}

/* Removes a mapping made by host_map_bar(). */
static void
host_unmap_bar(
	void *context,
	struct drv_pci_mapping *mapping)
{
	(void)context;

	/* Ignores storage that holds no mapping of ours. */
	if (mapping == NULL || mapping->private_data[0] != 1U)
		return;

	/* Releases the window; a refusal changes nothing. */
	(void)kern_device_unmap(mapping->address, mapping->size);
	kern_memset(mapping, 0, sizeof(*mapping));
}

/* Gives a function its interrupt pin, the host's pin of the same number plus one. */
static int
host_allocate_irqs(
	void *context,
	struct drv_pci_device *device,
	enum drv_pci_irq_type type,
	unsigned minimum,
	unsigned maximum,
	struct drv_pci_irq *irqs,
	unsigned *count)
{
	struct drv_pci_address address;
	struct rp1_usb_function *function;

	(void)context;

	/* Refuses missing arguments and a request for more than one line. */
	if (device == NULL || irqs == NULL || count == NULL)
		return EINVAL;
	if (minimum != 1U || maximum < minimum)
		return ENOTSUP;

	/* Offers only the pin. */
	if (type != DRV_PCI_IRQ_INTX)
		return ENOTSUP;

	/* Refuses a function the bus does not have. */
	drv_pci_device_address(device, &address);
	function = function_at(&address);
	if (function == NULL)
		return ENODEV;

	/* Succeeded: the function's pin. */
	irqs[0].type = DRV_PCI_IRQ_INTX;
	irqs[0].index = 0;
	irqs[0].vector = (unsigned)address.device + 1U;
	irqs[0].private_data[0] = 0;
	irqs[0].private_data[1] = 0;
	*count = 1U;
	return 0;
}

/* Releases interrupts from host_allocate_irqs(), which hold nothing. */
static void
host_free_irqs(
	void *context,
	struct drv_pci_device *device,
	struct drv_pci_irq *irqs,
	unsigned count)
{
	(void)context;
	(void)device;
	(void)irqs;
	(void)count;
}

/*
 * Puts a PCI core line in a pin's entry, the pin still masked.
 *
 * Called with the core's line lock held and interrupts off.  Reports EBUSY
 * when another line holds the pin.
 */
static int
host_intx_establish(
	void *context,
	unsigned vector,
	drv_pci_intx_run_t run,
	void *line)
{
	struct rp1_usb_function *function;
	unsigned long enabled;
	int error;

	(void)context;

	/* Refuses a pin the host does not have and an empty line. */
	if (vector < 1U || vector > rp1_usb.count)
		return EINVAL;
	if (run == NULL || line == NULL)
		return EINVAL;

	/* Fills the pin's entry unless another line holds it. */
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	function = &rp1_usb.functions[vector - 1U];
	error = 0;
	if (function->run != NULL) {
		error = EBUSY;
	} else {
		function->run = run;
		function->line = line;
		function->pin_enabled = 0;
	}

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);

	/* Failed: the pin belongs to another line. */
	if (error != 0)
		return error;

	/* Succeeded: the line is in the table, its pin still masked. */
	return 0;
}

/*
 * Empties a pin's entry and waits until no service still runs it.
 *
 * Called in thread context.  After one second of waiting the entry is put
 * back as it was and EBUSY is reported.
 */
static int
host_intx_disestablish(
	void *context,
	unsigned vector,
	drv_pci_intx_run_t run,
	void *line)
{
	struct rp1_usb_function *function;
	unsigned long enabled;
	unsigned in_flight;
	unsigned poll;
	int was_enabled;

	(void)context;

	/* Refuses a pin the host does not have. */
	if (vector < 1U || vector > rp1_usb.count)
		return EINVAL;

	/* Masks the pin and empties its entry, after checking the entry is this line's. */
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	function = &rp1_usb.functions[vector - 1U];
	if (function->run != run || function->line != line) {
		spin_unlock_irqrestore(&rp1_usb.lock, enabled);
		return EINVAL;
	}

	/* Keeps the pin's enable to put back, then empties the entry. */
	was_enabled = function->pin_enabled;
	function->pin_enabled = 0;
	function->run = NULL;
	function->line = NULL;

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);

	/* Waits, a millisecond at a time, for a service still inside run. */
	in_flight = 0;
	for (poll = 0; poll < RP1_USB_DRAIN_POLLS; poll++) {
		/* Reads how many services still hold the pin. */
		enabled = spin_lock_irqsave(&rp1_usb.lock);

		in_flight = function->in_flight;

		spin_unlock_irqrestore(&rp1_usb.lock, enabled);

		/* Drained: no service runs the line and none will. */
		if (in_flight == 0)
			break;

		/* Lets the other service finish. */
		kern_usleep_range(RP1_USB_DRAIN_POLL_US, RP1_USB_DRAIN_POLL_US);
	}

	/* Puts the entry back as it was when a service did not come back. */
	if (in_flight != 0) {
		enabled = spin_lock_irqsave(&rp1_usb.lock);

		function->run = run;
		function->line = line;
		function->pin_enabled = was_enabled;

		spin_unlock_irqrestore(&rp1_usb.lock, enabled);

		/* Reports the pin whose service did not come back. */
		kern_logf("rp1-usb: pin %u still running after a second\n", vector);
		return EBUSY;
	}

	/* Succeeded: the entry is empty and no service runs the line. */
	return 0;
}

/* Stops a pin from being served; called with interrupts off. */
static void
host_intx_mask(
	void *context,
	unsigned vector)
{
	unsigned long enabled;

	(void)context;

	/* Ignores a pin the host does not have. */
	if (vector < 1U || vector > rp1_usb.count)
		return;

	/* Clears the pin's enable. */
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	rp1_usb.functions[vector - 1U].pin_enabled = 0;

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);
}

/* Lets a pin be served; called with interrupts off. */
static void
host_intx_unmask(
	void *context,
	unsigned vector)
{
	unsigned long enabled;

	(void)context;

	/* Ignores a pin the host does not have. */
	if (vector < 1U || vector > rp1_usb.count)
		return;

	/* Sets the pin's enable. */
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	rp1_usb.functions[vector - 1U].pin_enabled = 1;

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);
}

/*
 * Routes each controller's RP1 interrupt to its pin's service and chooses
 * the polling interval: rare when every controller has its interrupt.
 */
static void
establish_interrupts(
	void)
{
	struct rp1_usb_function *function;
	unsigned vector;
	unsigned trigger;
	unsigned index;
	unsigned routed;
	unsigned present;
	int error;

	/* Polls often unless every controller gets its interrupt. */
	rp1_usb.poll_ms = RP1_USB_POLL_MS;

	/* Finds RP1's interrupt path; without it the polling serves alone. */
	error = drv_rp1_irq_init();
	if (error != 0)
		return;

	/* Routes the interrupt of each controller on the bus. */
	routed = 0;
	present = 0;
	for (index = 0; index < rp1_usb.count; index++) {
		/* Skips a controller that is not on the bus. */
		function = &rp1_usb.functions[index];
		if (!function->present)
			continue;

		/* Counts the controller on the bus. */
		present++;

		/* Reads the controller's vector. */
		error = drv_rp1_irq_of(function->node, 0, &vector, &trigger);
		if (error != 0) {
			kern_logf("rp1-usb: controller %u has no interrupt (%d)\n", index, error);
			continue;
		}

		/* Routes it to the pin's service. */
		error = drv_rp1_irq_establish(vector, trigger, controller_interrupt, function);
		if (error != 0) {
			kern_logf("rp1-usb: controller %u interrupt not routed (%d)\n", index, error);
			continue;
		}

		/* Counts the routed controller. */
		function->interrupt = 1;
		routed++;
	}

	/* Polls rarely once every controller interrupts. */
	if (present != 0 && routed == present)
		rp1_usb.poll_ms = RP1_USB_SAFETY_POLL_MS;
}

/* Serves a controller's pin from its RP1 interrupt. */
static void
controller_interrupt(
	void *argument)
{
	/* Runs the pin's line as the interrupt asks. */
	serve_function(argument);
}

/* Starts the thread that serves the controllers' pins. */
static void
start_polling(
	void)
{
	int error;

	/* Leaves a thread that already runs alone. */
	if (rp1_usb.polling)
		return;

	/* Creates the service thread. */
	error = kthread_create(poll_worker, &rp1_usb, SCHED_PRIORITY_DEFAULT, &rp1_usb.poll_thread);
	if (error != 0) {
		kern_logf("rp1-usb: polling unavailable (%d)\n", error);
		return;
	}

	/* Makes it runnable for as long as the kernel runs. */
	rp1_usb.polling = 1;
	thread_start(rp1_usb.poll_thread);
	kern_logf("rp1-usb: polling %u controllers every %u ms\n", rp1_usb.count, rp1_usb.poll_ms);
}

/* Serves every controller's pin, then sleeps, for as long as the kernel runs. */
static void
poll_worker(
	void *argument)
{
	uint64_t deadline;
	unsigned index;

	(void)argument;

	/* Serves the pins at the polling interval. */
	for (;;) {
		/* Serves each controller in turn. */
		for (index = 0; index < rp1_usb.count; index++)
			serve_function(&rp1_usb.functions[index]);

		/* Sleeps on scheduler ticks until the next round. */
		deadline = sched_ticks() + kern_ms_to_ticks(rp1_usb.poll_ms);
		sched_sleep(deadline);
	}
}

/*
 * Serves one controller's pin as its interrupt would: runs the PCI core's
 * line, with interrupts off, when the pin is established, unmasked and the
 * function has not disabled its interrupt.
 */
static void
serve_function(
	struct rp1_usb_function *function)
{
	drv_pci_intx_run_t run;
	unsigned long enabled;
	bool interrupts;
	void *line;

	/* Takes the pin's entry and counts this service on it. */
	run = NULL;
	line = NULL;
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	if (function->run != NULL &&
	    function->pin_enabled &&
	    (function->command & PCI_COMMAND_INTX_DISABLE) == 0) {
		run = function->run;
		line = function->line;
		function->in_flight++;
	}

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);

	/* Leaves a pin nobody serves. */
	if (run == NULL)
		return;

	/* Runs the line as interrupt context would, with interrupts off. */
	interrupts = kern_irq_disable();
	run(line);
	if (interrupts)
		kern_irq_enable();

	/* Drops this service from the pin, which lets a removal finish. */
	enabled = spin_lock_irqsave(&rp1_usb.lock);

	function->in_flight--;

	spin_unlock_irqrestore(&rp1_usb.lock, enabled);
}
