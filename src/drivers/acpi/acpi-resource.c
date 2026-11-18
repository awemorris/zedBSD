/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Resource templates (ACPI 6.5 section 6.4): the buffer a device's _CRS
 * returns, decoded into the I/O ranges, memory ranges and interrupts the
 * device uses, for the drivers that would otherwise each decode it.
 *
 * Small descriptors carry their length in their tag byte; large ones carry
 * a 16-bit length after it.  Descriptors that hold no I/O, memory,
 * interrupt or connection (DMA, vendor, start and end of dependent
 * functions, a GPIO I/O connection, a serial bus other than I2C) are
 * stepped over.  An address space descriptor of the bus number type is
 * stepped over too.  An I2C serial bus connection and a GPIO interrupt
 * connection (ws159-p002, an I2C-HID touchpad's) are decoded with the
 * path of the controller they name.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>

#include "aml-internal.h"

/*
 * The tag bytes of the small descriptors decoded, with their length bits.
 */
#define SMALL_TAG_NAME_MASK	0x78U
#define SMALL_TAG_LENGTH_MASK	0x07U
#define SMALL_IRQ		0x20U
#define SMALL_IO		0x40U
#define SMALL_FIXED_IO		0x48U
#define SMALL_END		0x78U

/*
 * The tag bit that marks a large descriptor, and the large tags decoded.
 */
#define LARGE_TAG		0x80U
#define LARGE_MEMORY24		0x81U
#define LARGE_MEMORY32		0x85U
#define LARGE_FIXED_MEMORY32	0x86U
#define LARGE_DWORD_ADDRESS	0x87U
#define LARGE_WORD_ADDRESS	0x88U
#define LARGE_EXTENDED_IRQ	0x89U
#define LARGE_QWORD_ADDRESS	0x8aU
#define LARGE_EXTENDED_ADDRESS	0x8bU
#define LARGE_GPIO		0x8cU
#define LARGE_SERIAL_BUS	0x8eU

/*
 * A GPIO connection descriptor (ACPI 6.5 section 6.4.3.8.1): its connection
 * type (0 an interrupt), its flags, the offsets of its pin table and of its
 * resource source's name, all counted from the tag byte, and the bits of
 * its interrupt flags.
 */
#define GPIO_TYPE		4U
#define GPIO_TYPE_INTERRUPT	0U
#define GPIO_GENERAL_FLAGS	5U
#define GPIO_INTERRUPT_FLAGS	7U
#define GPIO_PIN_TABLE		14U
#define GPIO_SOURCE_NAME	17U
#define GPIO_VENDOR_DATA	19U
#define GPIO_MINIMUM		23U
#define GPIO_CONSUMER		0x01U
#define GPIO_EDGE		0x01U
#define GPIO_POLARITY_MASK	0x06U
#define GPIO_POLARITY_LOW	0x02U
#define GPIO_SHARED		0x08U
#define GPIO_WAKE		0x10U

/*
 * A serial bus connection descriptor (section 6.4.3.8.2): its bus type (1
 * I2C), its flags, the length of its type-specific data (counted from
 * SERIAL_TYPE_DATA), and an I2C connection's speed and address.  The
 * resource source's name follows the type-specific data.
 */
#define SERIAL_BUS_TYPE		5U
#define SERIAL_BUS_I2C		1U
#define SERIAL_GENERAL_FLAGS	6U
#define SERIAL_TYPE_FLAGS	7U
#define SERIAL_TYPE_LENGTH	10U
#define SERIAL_TYPE_DATA	12U
#define SERIAL_I2C_SPEED	12U
#define SERIAL_I2C_ADDRESS	16U
#define SERIAL_I2C_DATA		6U
#define SERIAL_CONSUMER		0x02U
#define SERIAL_I2C_TEN_BIT	0x01U

/*
 * The resource types of an address space descriptor.
 */
#define ADDRESS_MEMORY		0U
#define ADDRESS_IO		1U

/*
 * The flags of the descriptors: an interrupt that is edge-triggered,
 * active low or shared; a resource the device consumes rather than
 * produces; a memory range that may be written.
 */
#define IRQ_FLAG_EDGE		0x01U
#define IRQ_FLAG_ACTIVE_LOW	0x08U
#define IRQ_FLAG_SHARED		0x10U
#define EXTENDED_IRQ_CONSUMER	0x01U
#define EXTENDED_IRQ_EDGE	0x02U
#define EXTENDED_IRQ_ACTIVE_LOW	0x04U
#define EXTENDED_IRQ_SHARED	0x08U
#define ADDRESS_CONSUMER	0x01U
#define MEMORY_WRITABLE		0x01U

/*
 * The offsets of the fields of an address space descriptor, counted from
 * its tag byte, by the width of its numbers: the granularity, minimum and
 * length of the Word, DWord and QWord forms and of the Extended form.
 */
#define WORD_MINIMUM		8U
#define WORD_LENGTH		14U
#define DWORD_MINIMUM		10U
#define DWORD_LENGTH		22U
#define QWORD_MINIMUM		14U
#define QWORD_LENGTH		38U
#define EXTENDED_MINIMUM	16U
#define EXTENDED_LENGTH		40U

static int resources_parse(const uint8_t *bytes, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static int small_descriptor(const uint8_t *descriptor, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static int large_descriptor(const uint8_t *descriptor, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static int irq_descriptor(const uint8_t *descriptor, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static int extended_irq_descriptor(const uint8_t *descriptor, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static int address_descriptor(const uint8_t *descriptor, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static int gpio_descriptor(const uint8_t *descriptor, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static int serial_bus_descriptor(const uint8_t *descriptor, size_t length, drv_acpi_resource_visitor_t visitor, void *argument);
static void copy_source(const uint8_t *descriptor, size_t length, size_t offset, size_t end, char *source);
static uint64_t load_little(const uint8_t *bytes, unsigned width);

/*
 * Walks the resources a device's resource template describes.
 *
 * method names the object that gives the template, "_CRS" when NULL (a
 * driver may ask for _PRS instead).  The visitor sees each I/O range,
 * memory range and interrupt in the template's order.  The walk reports
 * zero after the last resource, the visitor's value when it stops the
 * walk (a negative one by convention), EINVAL when the object is not a
 * buffer, and EIO for a template whose descriptors run past its end.
 */
int
drv_acpi_resources_walk(
	struct drv_acpi_node *device,
	const char *method,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_object *template;
	const uint8_t *bytes;
	size_t length;
	int error;

	/* Refuses a missing device or visitor. */
	if (device == NULL || visitor == NULL)
		return EINVAL;

	/* Asks for _CRS unless the driver names another object. */
	if (method == NULL)
		method = "_CRS";

	/* Evaluates the template. */
	template = NULL;
	error = drv_acpi_evaluate(device, method, NULL, 0, &template);
	if (error != 0)
		return error;

	/* Refuses anything but a buffer. */
	if (template == NULL || template->type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_object_release(template);
		return EINVAL;
	}

	/* Decodes the descriptors. */
	bytes = template->value.buffer.bytes;
	length = template->value.buffer.length;
	error = resources_parse(bytes, length, visitor, argument);

	/* The template is no longer needed. */
	drv_acpi_object_release(template);

	/* Reports a template that could not be decoded, or the visitor's stop. */
	if (error != 0)
		return error;

	/* Succeeded: the visitor saw every resource. */
	return 0;
}

/* Decodes each descriptor of a template, up to its end tag or its end. */
static int
resources_parse(
	const uint8_t *bytes,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	size_t offset;
	size_t size;
	uint8_t tag;
	int error;

	/* Visits each descriptor in order. */
	offset = 0;
	while (offset < length) {
		/* Stops at the end tag. */
		tag = bytes[offset];
		if ((tag & LARGE_TAG) == 0 && (tag & SMALL_TAG_NAME_MASK) == SMALL_END)
			break;

		/* Measures the descriptor: a small one's length is in its tag, a large one's in the two bytes after it. */
		if ((tag & LARGE_TAG) == 0) {
			size = 1U + (tag & SMALL_TAG_LENGTH_MASK);
		} else {
			/* Refuses a large descriptor whose length runs past the template. */
			if (length - offset < 3U)
				return EIO;
			size = 3U + (size_t)load_little(bytes + offset + 1U, 2);
		}

		/* Refuses a descriptor that runs past the template. */
		if (size > length - offset)
			return EIO;

		/* Decodes it by its form. */
		if ((tag & LARGE_TAG) == 0) {
			error = small_descriptor(bytes + offset, size, visitor, argument);
		} else {
			error = large_descriptor(bytes + offset, size, visitor, argument);
		}

		/* Stops at a descriptor the visitor stopped at, or one that could not be decoded. */
		if (error != 0)
			return error;

		/* Moves to the next descriptor. */
		offset += size;
	}

	/* Succeeded: every descriptor was decoded. */
	return 0;
}

/* Decodes a small descriptor: an IRQ, an I/O range or a fixed I/O range. */
static int
small_descriptor(
	const uint8_t *descriptor,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	int stop;

	/* Starts the resource empty, with the tag it came from. */
	kern_memset(&resource, 0, sizeof(resource));
	resource.descriptor = descriptor[0];

	/* Decodes by the descriptor's name. */
	switch (descriptor[0] & SMALL_TAG_NAME_MASK) {
	case SMALL_IRQ:
		/* An IRQ descriptor may name several interrupts. */
		stop = irq_descriptor(descriptor, length, visitor, argument);
		break;
	case SMALL_IO:
		/* An I/O range: its minimum base and its length (ACPI 6.5 section 6.4.2.5). */
		if (length < 8U)
			return EIO;
		resource.kind = DRV_ACPI_RESOURCE_IO;
		resource.base = load_little(descriptor + 2U, 2);
		resource.length = descriptor[7];
		stop = visitor(&resource, argument);
		break;
	case SMALL_FIXED_IO:
		/* A fixed I/O range: its 10-bit base and its length (section 6.4.2.6). */
		if (length < 4U)
			return EIO;
		resource.kind = DRV_ACPI_RESOURCE_IO;
		resource.base = load_little(descriptor + 1U, 2) & 0x3ffU;
		resource.length = descriptor[3];
		stop = visitor(&resource, argument);
		break;
	default:
		/* Any other small descriptor holds no range or interrupt. */
		stop = 0;
		break;
	}

	/* Reports the visitor's stop. */
	if (stop != 0)
		return stop;

	/* Succeeded: the descriptor's resources were visited. */
	return 0;
}

/* Decodes a large descriptor: a memory range, an extended IRQ or an address space. */
static int
large_descriptor(
	const uint8_t *descriptor,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	int stop;

	/* Starts the resource empty, with the tag it came from. */
	kern_memset(&resource, 0, sizeof(resource));
	resource.descriptor = descriptor[0];

	/* Decodes by the descriptor's tag. */
	switch (descriptor[0]) {
	case LARGE_MEMORY24:
		/* A 24-bit memory range, whose numbers are in units of 256 bytes (section 6.4.3.1). */
		if (length < 12U)
			return EIO;
		resource.kind = DRV_ACPI_RESOURCE_MEMORY;
		resource.writable = (uint8_t)(descriptor[3] & MEMORY_WRITABLE);
		resource.base = load_little(descriptor + 4U, 2) << 8;
		resource.length = load_little(descriptor + 10U, 2) << 8;
		stop = visitor(&resource, argument);
		break;
	case LARGE_MEMORY32:
		/* A 32-bit memory range: its minimum and its length (section 6.4.3.3). */
		if (length < 20U)
			return EIO;
		resource.kind = DRV_ACPI_RESOURCE_MEMORY;
		resource.writable = (uint8_t)(descriptor[3] & MEMORY_WRITABLE);
		resource.base = load_little(descriptor + 4U, 4);
		resource.length = load_little(descriptor + 16U, 4);
		stop = visitor(&resource, argument);
		break;
	case LARGE_FIXED_MEMORY32:
		/* A fixed 32-bit memory range: its base and its length (section 6.4.3.4). */
		if (length < 12U)
			return EIO;
		resource.kind = DRV_ACPI_RESOURCE_MEMORY;
		resource.writable = (uint8_t)(descriptor[3] & MEMORY_WRITABLE);
		resource.base = load_little(descriptor + 4U, 4);
		resource.length = load_little(descriptor + 8U, 4);
		stop = visitor(&resource, argument);
		break;
	case LARGE_EXTENDED_IRQ:
		/* An extended IRQ descriptor may name several interrupts. */
		stop = extended_irq_descriptor(descriptor, length, visitor, argument);
		break;
	case LARGE_WORD_ADDRESS:
	case LARGE_DWORD_ADDRESS:
	case LARGE_QWORD_ADDRESS:
	case LARGE_EXTENDED_ADDRESS:
		/* An address space descriptor of memory or I/O. */
		stop = address_descriptor(descriptor, length, visitor, argument);
		break;
	case LARGE_GPIO:
		/* A GPIO connection: an interrupt is visited, an I/O connection is not. */
		stop = gpio_descriptor(descriptor, length, visitor, argument);
		break;
	case LARGE_SERIAL_BUS:
		/* A serial bus connection: an I2C one is visited, the others are not. */
		stop = serial_bus_descriptor(descriptor, length, visitor, argument);
		break;
	default:
		/* Any other large descriptor holds no range or interrupt. */
		stop = 0;
		break;
	}

	/* Reports the visitor's stop, or a descriptor too short for its form. */
	if (stop != 0)
		return stop;

	/* Succeeded: the descriptor's resources were visited. */
	return 0;
}

/* Visits each interrupt of a small IRQ descriptor's mask (section 6.4.2.1). */
static int
irq_descriptor(
	const uint8_t *descriptor,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	uint64_t mask;
	uint8_t flags;
	unsigned irq;
	int stop;

	/* Refuses a descriptor too short for its mask. */
	if (length < 3U)
		return EIO;

	/* Takes the flags; without the flags byte the interrupt is edge-triggered, active high and exclusive. */
	mask = load_little(descriptor + 1U, 2);
	flags = IRQ_FLAG_EDGE;
	if (length >= 4U)
		flags = descriptor[3];

	/* Describes the interrupts the flags give. */
	kern_memset(&resource, 0, sizeof(resource));
	resource.kind = DRV_ACPI_RESOURCE_IRQ;
	resource.descriptor = descriptor[0];
	resource.length = 1;
	if ((flags & IRQ_FLAG_EDGE) == 0)
		resource.level = 1;
	if ((flags & IRQ_FLAG_ACTIVE_LOW) != 0)
		resource.active_low = 1;
	if ((flags & IRQ_FLAG_SHARED) != 0)
		resource.shared = 1;

	/* Visits each interrupt the mask names. */
	for (irq = 0; irq < 16U; irq++) {
		/* Skips an interrupt the mask does not name. */
		if ((mask & ((uint64_t)1 << irq)) == 0)
			continue;

		/* Visits the interrupt; the visitor may stop the walk. */
		resource.base = irq;
		stop = visitor(&resource, argument);
		if (stop != 0)
			return stop;
	}

	/* Succeeded: every interrupt of the mask was visited. */
	return 0;
}

/* Visits each interrupt of an extended IRQ descriptor (section 6.4.3.6). */
static int
extended_irq_descriptor(
	const uint8_t *descriptor,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	uint8_t flags;
	unsigned count;
	unsigned index;
	int stop;

	/* Refuses a descriptor too short for its flags and count. */
	if (length < 5U)
		return EIO;

	/* Refuses a count of interrupts that runs past the descriptor. */
	flags = descriptor[3];
	count = descriptor[4];
	if ((size_t)count * 4U > length - 5U)
		return EIO;

	/* Describes the interrupts the flags give. */
	kern_memset(&resource, 0, sizeof(resource));
	resource.kind = DRV_ACPI_RESOURCE_IRQ;
	resource.descriptor = descriptor[0];
	resource.length = 1;
	if ((flags & EXTENDED_IRQ_CONSUMER) == 0)
		resource.producer = 1;
	if ((flags & EXTENDED_IRQ_EDGE) == 0)
		resource.level = 1;
	if ((flags & EXTENDED_IRQ_ACTIVE_LOW) != 0)
		resource.active_low = 1;
	if ((flags & EXTENDED_IRQ_SHARED) != 0)
		resource.shared = 1;

	/* Visits each interrupt number. */
	for (index = 0; index < count; index++) {
		/* Visits the interrupt; the visitor may stop the walk. */
		resource.base = load_little(descriptor + 5U + index * 4U, 4);
		stop = visitor(&resource, argument);
		if (stop != 0)
			return stop;
	}

	/* Succeeded: every interrupt of the descriptor was visited. */
	return 0;
}

/*
 * Visits the range of a Word, DWord, QWord or Extended address space
 * descriptor of memory or I/O (sections 6.4.3.5.1 to 6.4.3.5.4).
 */
static int
address_descriptor(
	const uint8_t *descriptor,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	unsigned width;
	unsigned minimum;
	unsigned range_length;
	int stop;

	/* Finds where the form keeps its minimum and its length, and how wide they are. */
	switch (descriptor[0]) {
	case LARGE_WORD_ADDRESS:
		width = 2;
		minimum = WORD_MINIMUM;
		range_length = WORD_LENGTH;
		break;
	case LARGE_DWORD_ADDRESS:
		width = 4;
		minimum = DWORD_MINIMUM;
		range_length = DWORD_LENGTH;
		break;
	case LARGE_QWORD_ADDRESS:
		width = 8;
		minimum = QWORD_MINIMUM;
		range_length = QWORD_LENGTH;
		break;
	default:
		width = 8;
		minimum = EXTENDED_MINIMUM;
		range_length = EXTENDED_LENGTH;
		break;
	}

	/* Refuses a descriptor too short for its length field. */
	if (length < range_length + width)
		return EIO;

	/* A bus number range, or a vendor-defined type, holds no I/O or memory. */
	if (descriptor[3] != ADDRESS_MEMORY && descriptor[3] != ADDRESS_IO)
		return 0;

	/* Describes the range. */
	kern_memset(&resource, 0, sizeof(resource));
	resource.descriptor = descriptor[0];
	resource.kind = DRV_ACPI_RESOURCE_IO;
	if (descriptor[3] == ADDRESS_MEMORY)
		resource.kind = DRV_ACPI_RESOURCE_MEMORY;
	resource.base = load_little(descriptor + minimum, width);
	resource.length = load_little(descriptor + range_length, width);

	/* A device that does not consume the range produces it for its children. */
	if ((descriptor[4] & ADDRESS_CONSUMER) == 0)
		resource.producer = 1;

	/* A memory range says whether it may be written. */
	if (descriptor[3] == ADDRESS_MEMORY && (descriptor[5] & MEMORY_WRITABLE) != 0)
		resource.writable = 1;

	/* Visits the range. */
	stop = visitor(&resource, argument);
	if (stop != 0)
		return stop;

	/* Succeeded: the range was visited. */
	return 0;
}

/*
 * Visits a GPIO interrupt connection (section 6.4.3.8.1): its first pin,
 * the number of pins, how the interrupt is signalled and the controller.
 */
static int
gpio_descriptor(
	const uint8_t *descriptor,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	uint64_t pin_table;
	uint64_t source_name;
	uint64_t vendor_data;
	uint64_t flags;
	uint8_t polarity;
	int stop;

	/* Refuses a descriptor too short for its fixed fields. */
	if (length < GPIO_MINIMUM)
		return EIO;

	/* A GPIO I/O connection holds no interrupt. */
	if (descriptor[GPIO_TYPE] != GPIO_TYPE_INTERRUPT)
		return 0;

	/* Finds the pin table and the controller's name, which end where the next part begins. */
	pin_table = load_little(descriptor + GPIO_PIN_TABLE, 2);
	source_name = load_little(descriptor + GPIO_SOURCE_NAME, 2);
	vendor_data = load_little(descriptor + GPIO_VENDOR_DATA, 2);

	/* Refuses a pin table that is empty, out of order or past the descriptor. */
	if (pin_table < GPIO_MINIMUM)
		return EIO;
	if (source_name < pin_table + 2U)
		return EIO;
	if (source_name > length)
		return EIO;

	/* The controller's name ends at the vendor data, or at the descriptor's end without any. */
	if (vendor_data == 0U || vendor_data > length || vendor_data < source_name)
		vendor_data = length;

	/* Describes the interrupt: its first pin and how many pins the table holds. */
	kern_memset(&resource, 0, sizeof(resource));
	resource.kind = DRV_ACPI_RESOURCE_GPIO_INT;
	resource.descriptor = descriptor[0];
	resource.base = load_little(descriptor + pin_table, 2);
	resource.length = (source_name - pin_table) / 2U;

	/* A device that does not consume the connection produces it. */
	if ((descriptor[GPIO_GENERAL_FLAGS] & GPIO_CONSUMER) == 0)
		resource.producer = 1;

	/* The trigger: edge or level. */
	flags = load_little(descriptor + GPIO_INTERRUPT_FLAGS, 2);
	if ((flags & GPIO_EDGE) == 0)
		resource.level = 1;

	/* The polarity: active low (an interrupt on both edges is not active low). */
	polarity = (uint8_t)(flags & GPIO_POLARITY_MASK);
	if (polarity == GPIO_POLARITY_LOW)
		resource.active_low = 1;

	/* Whether the interrupt is shared, and whether it can wake the machine. */
	if ((flags & GPIO_SHARED) != 0)
		resource.shared = 1;
	if ((flags & GPIO_WAKE) != 0)
		resource.wake = 1;

	/* The GPIO controller's path. */
	copy_source(descriptor, length, (size_t)source_name, (size_t)vendor_data, resource.source);

	/* Visits the interrupt. */
	stop = visitor(&resource, argument);
	if (stop != 0)
		return stop;

	/* Succeeded: the interrupt was visited. */
	return 0;
}

/*
 * Visits an I2C serial bus connection (section 6.4.3.8.2.1): the device's
 * address, its speed and the bus controller.
 */
static int
serial_bus_descriptor(
	const uint8_t *descriptor,
	size_t length,
	drv_acpi_resource_visitor_t visitor,
	void *argument)
{
	struct drv_acpi_resource resource;
	uint64_t type_length;
	uint64_t flags;
	size_t source_name;
	int stop;

	/* Refuses a descriptor too short for its fixed fields. */
	if (length < SERIAL_TYPE_DATA)
		return EIO;

	/* Only an I2C connection is decoded; SPI and UART are stepped over. */
	if (descriptor[SERIAL_BUS_TYPE] != SERIAL_BUS_I2C)
		return 0;

	/* Refuses I2C data too short for the speed and address, or past the descriptor. */
	type_length = load_little(descriptor + SERIAL_TYPE_LENGTH, 2);
	if (type_length < SERIAL_I2C_DATA)
		return EIO;
	if (type_length > length - SERIAL_TYPE_DATA)
		return EIO;

	/* Describes the connection: the address on the bus and the speed. */
	kern_memset(&resource, 0, sizeof(resource));
	resource.kind = DRV_ACPI_RESOURCE_I2C;
	resource.descriptor = descriptor[0];
	resource.base = load_little(descriptor + SERIAL_I2C_ADDRESS, 2);
	resource.length = 1;
	resource.speed = (uint32_t)load_little(descriptor + SERIAL_I2C_SPEED, 4);

	/* A device that does not consume the connection produces it. */
	if ((descriptor[SERIAL_GENERAL_FLAGS] & SERIAL_CONSUMER) == 0)
		resource.producer = 1;

	/* A 10-bit address. */
	flags = load_little(descriptor + SERIAL_TYPE_FLAGS, 2);
	if ((flags & SERIAL_I2C_TEN_BIT) != 0)
		resource.ten_bit = 1;

	/* The bus controller's path follows the type-specific data. */
	source_name = SERIAL_TYPE_DATA + (size_t)type_length;
	copy_source(descriptor, length, source_name, length, resource.source);

	/* Visits the connection. */
	stop = visitor(&resource, argument);
	if (stop != 0)
		return stop;

	/* Succeeded: the connection was visited. */
	return 0;
}

/*
 * Copies a resource source's name (a NUL-terminated path between offset
 * and end of the descriptor) into source, cut short to fit and always
 * terminated.
 */
static void
copy_source(
	const uint8_t *descriptor,
	size_t length,
	size_t offset,
	size_t end,
	char *source)
{
	size_t index;

	/* The name may not run past the descriptor. */
	if (end > length)
		end = length;

	/* Copies the characters up to the name's NUL, the end or the room. */
	index = 0;
	while (offset + index < end && index + 1U < DRV_ACPI_RESOURCE_SOURCE_MAX) {
		/* The name's own NUL ends it. */
		if (descriptor[offset + index] == 0U)
			break;

		/* Copies one character. */
		source[index] = (char)descriptor[offset + index];
		index++;
	}

	/* Succeeded: the name is terminated. */
	source[index] = '\0';
}

/* Reads a little-endian number of 2, 4 or 8 bytes. */
static uint64_t
load_little(
	const uint8_t *bytes,
	unsigned width)
{
	uint64_t value;
	unsigned index;

	/* Assembles the number from its highest byte down. */
	value = 0;
	for (index = width; index != 0; index--)
		value = value << 8 | bytes[index - 1U];

	/* Reports the assembled number. */
	return value;
}
