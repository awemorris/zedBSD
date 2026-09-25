/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the production HD Audio driver on the host (ws035-p007).
 *
 * A register-file model answers the controller registers; codecs answer
 * the verbs written to the CORB by writing the RIRB.  Two codec layouts
 * are modelled: QEMU's hda-duplex, and a laptop-like codec whose internal
 * speaker and headphone jack share one DAC through a mixer, behind an
 * Intel display codec at a lower address.  PCI, DMA and the audio
 * framework are stubs that record what the driver asked for.
 */

#include <drivers/audio/audio.h>
#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-hda.h>
#include <kern/device-io.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REGISTER_BYTES	0x4000U
#define WIDGETS_MAX	48U
#define SETS_MAX	512U

/* One widget of a model codec. */
struct model_widget {
	uint8_t nid;
	uint32_t caps;
	uint32_t pin_caps;
	uint32_t config;
	uint32_t amp_out;
	uint32_t amp_in;
	uint32_t pcm;
	uint8_t connections[8];
	unsigned connection_count;
};

/* One model codec. */
struct model_codec {
	unsigned present;
	unsigned silent;
	uint32_t vendor;
	uint8_t function_group;
	uint8_t first_widget;
	unsigned widget_count;
	struct model_widget widgets[WIDGETS_MAX];
};

/* One verb that set something, as the codec saw it. */
struct model_set {
	unsigned codec;
	uint8_t nid;
	uint32_t verb;
	uint32_t payload;
};

static uint8_t registers[REGISTER_BYTES];
static struct model_codec codecs[15];
static struct model_set sets[SETS_MAX];
static unsigned set_count;
static unsigned corb_processed;
static unsigned rirb_count;
static uint8_t *corb_memory;
static uint8_t *rirb_memory;
static uint64_t ticks;
static unsigned live_allocations;
static unsigned live_dma;
static struct drv_pci_driver *registered_driver;
static const struct drv_pci_service_interface *service;
static void *service_argument;
static void *driver_data;
static drv_pci_irq_handler_t irq_handler;
static void *irq_argument;
static unsigned bus_master;
static unsigned bar_claimed;
static unsigned bar_mapped;
static unsigned irq_live;
static uint16_t pcie_control;
static const struct drv_audio_ops *audio_ops;
static void *audio_private;
static struct drv_dma_device *audio_dma;
static unsigned audio_registered;
static unsigned interrupts[2];
static uint8_t fake_dma[16];
static struct drv_pci_device *fake_device = (struct drv_pci_device *)(uintptr_t)0x1000;

/* ---------------------------------------------------------------- */
/* Kernel services.                                                 */

void
__libc_assert_fail(const char *expression, const char *file, int line)
{
	fprintf(stderr, "ASSERT %s:%d: %s\n", file, line, expression);
	abort();
}

void *
kern_calloc(size_t count, size_t size)
{
	void *memory;

	memory = calloc(count, size);
	if (memory != NULL)
		live_allocations++;
	return memory;
}

void
kern_free(void *memory)
{
	if (memory == NULL)
		return;
	live_allocations--;
	free(memory);
}

void
kern_logf(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	fflush(stdout);
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	memset(mutex, 0, sizeof(*mutex));
	(void)rank;
	(void)name;
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	assert(mutex->locked == 0);
	mutex->locked = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	assert(mutex->locked == 1);
	mutex->locked = 0;
}

uint64_t
sched_ticks(void)
{
	/* Time moves on every look, so a register that never changes times out. */
	return ticks++;
}

void
sched_yield(void)
{
}

void kern_io_barrier(void) {}
void kern_io_read_barrier(void) {}
void kern_io_write_barrier(void) {}
void kern_compiler_barrier(void) {}

int
drv_dma_alloc_coherent(struct drv_dma_device *device, size_t size,
    size_t alignment, struct drv_dma_buffer *buffer)
{
	void *raw;
	uintptr_t aligned;

	assert(device == (struct drv_dma_device *)fake_dma);
	memset(buffer, 0, sizeof(*buffer));
	raw = malloc(size + alignment + sizeof(void *));
	assert(raw != NULL);
	aligned = ((uintptr_t)raw + sizeof(void *) + alignment - 1) & ~((uintptr_t)alignment - 1);
	((void **)aligned)[-1] = raw;
	buffer->address = (void *)aligned;
	memset(buffer->address, 0x77, size);
	buffer->device_address = (uintptr_t)buffer->address;
	buffer->size = size;
	live_dma++;
	return 0;
}

void
drv_dma_free_coherent(struct drv_dma_device *device, struct drv_dma_buffer *buffer)
{
	assert(device == (struct drv_dma_device *)fake_dma);
	live_dma--;
	free(((void **)buffer->address)[-1]);
	buffer->address = NULL;
}

/* ---------------------------------------------------------------- */
/* Audio framework stubs.                                           */

int
drv_audio_register(const struct drv_audio_ops *ops, void *private_data,
    struct drv_dma_device *dma, struct drv_audio_device **device)
{
	audio_ops = ops;
	audio_private = private_data;
	audio_dma = dma;
	audio_registered = 1;
	*device = (struct drv_audio_device *)(uintptr_t)0x2000;
	return 0;
}

int
drv_audio_unregister(struct drv_audio_device *device)
{
	assert(device == (struct drv_audio_device *)(uintptr_t)0x2000);
	audio_registered = 0;
	return 0;
}

void
drv_audio_interrupt(struct drv_audio_device *device, int capture)
{
	assert(device == (struct drv_audio_device *)(uintptr_t)0x2000);
	interrupts[capture]++;
}

/* ---------------------------------------------------------------- */
/* PCI stubs.                                                       */

int
drv_pci_driver_register(struct drv_pci_driver *driver)
{
	registered_driver = driver;
	return 0;
}

struct drv_dma_device *
drv_pci_device_dma(struct drv_pci_device *d)
{
	assert(d == fake_device);
	return (struct drv_dma_device *)fake_dma;
}

int
drv_pci_device_save_enable_state(struct drv_pci_device *d, struct drv_pci_enable_state *s)
{
	(void)d;
	memset(s, 0, sizeof(*s));
	return 0;
}

int
drv_pci_device_restore_enable_state(struct drv_pci_device *d, struct drv_pci_enable_state *s)
{
	(void)d;
	(void)s;
	return 0;
}

int
drv_pci_device_enable_memory(struct drv_pci_device *d)
{
	(void)d;
	return 0;
}

int
drv_pci_device_set_bus_master(struct drv_pci_device *d, bool on)
{
	(void)d;
	bus_master = on ? 1U : 0U;
	return 0;
}

int
drv_pci_device_bar(const struct drv_pci_device *d, unsigned i, struct drv_pci_bar *b)
{
	(void)d;
	assert(i == 0);
	memset(b, 0, sizeof(*b));
	b->type = DRV_PCI_BAR_MEMORY64;
	b->size = REGISTER_BYTES;
	return 0;
}

int
drv_pci_device_claim_bar(struct drv_pci_device *d, unsigned i)
{
	(void)d;
	(void)i;
	bar_claimed++;
	return 0;
}

void
drv_pci_device_release_bar(struct drv_pci_device *d, unsigned i)
{
	(void)d;
	(void)i;
	bar_claimed--;
}

int
drv_pci_device_map_bar(struct drv_pci_device *d, unsigned i, unsigned f, struct drv_pci_mapping *m)
{
	(void)d;
	(void)i;
	assert((f & DRV_PCI_MAP_NOCACHE) != 0);
	memset(m, 0, sizeof(*m));
	m->address = registers;
	m->size = REGISTER_BYTES;
	bar_mapped++;
	return 0;
}

void
drv_pci_device_unmap_bar(struct drv_pci_device *d, struct drv_pci_mapping *m)
{
	(void)d;
	assert(m->address == registers);
	bar_mapped--;
}

int
drv_pci_device_find_capability(struct drv_pci_device *d, uint8_t id, unsigned *result)
{
	(void)d;
	if (id != 0x10)
		return ENOENT;
	*result = 0x70;
	return 0;
}

int
drv_pci_device_config_read16(struct drv_pci_device *d, unsigned o, uint16_t *v)
{
	(void)d;
	assert(o == 0x78);
	*v = pcie_control;
	return 0;
}

int
drv_pci_device_config_write16(struct drv_pci_device *d, unsigned o, uint16_t v)
{
	(void)d;
	assert(o == 0x78);
	pcie_control = v;
	return 0;
}

int
drv_pci_device_allocate_irqs(struct drv_pci_device *d, unsigned flags, unsigned min,
    unsigned max, struct drv_pci_irq *i, unsigned *n)
{
	(void)d;
	assert((flags & DRV_PCI_IRQ_ALLOW_MSI) != 0);
	assert(min == 1 && max == 1);
	memset(i, 0, sizeof(*i));
	i->type = DRV_PCI_IRQ_MSI;
	*n = 1;
	irq_live++;
	return 0;
}

void
drv_pci_device_free_irqs(struct drv_pci_device *d, struct drv_pci_irq *i, unsigned n)
{
	(void)d;
	(void)i;
	assert(n == 1);
	irq_live--;
}

int
drv_pci_device_establish_irq(struct drv_pci_device *d, const struct drv_pci_irq *i,
    drv_pci_irq_handler_t h, void *a, const char *n, void **result)
{
	(void)d;
	(void)i;
	(void)n;
	irq_handler = h;
	irq_argument = a;
	*result = (void *)(uintptr_t)0x3000;
	return 0;
}

int
drv_pci_device_disestablish_irq_checked(struct drv_pci_device *d, void *cookie)
{
	(void)d;
	assert(cookie == (void *)(uintptr_t)0x3000);
	irq_handler = NULL;
	return 0;
}

int
drv_pci_device_set_service(struct drv_pci_device *d, const struct drv_pci_service_interface *i, void *a)
{
	(void)d;
	service = i;
	service_argument = a;
	return 0;
}

int
drv_pci_device_set_driver_data(struct drv_pci_device *d, void *p)
{
	(void)d;
	driver_data = p;
	return 0;
}

void *
drv_pci_device_driver_data(const struct drv_pci_device *d)
{
	(void)d;
	return driver_data;
}

/* ---------------------------------------------------------------- */
/* Codec model.                                                     */

static struct model_widget *
model_widget(struct model_codec *codec, uint8_t nid)
{
	unsigned index;

	for (index = 0; index < codec->widget_count; index++) {
		if (codec->widgets[index].nid == nid)
			return &codec->widgets[index];
	}
	return NULL;
}

/* Answers one verb, recording the ones that set state. */
static uint32_t
model_answer(unsigned address, uint32_t command)
{
	struct model_codec *codec;
	struct model_widget *widget;
	uint8_t nid;
	uint32_t verb;
	uint32_t payload;
	uint32_t parameter;
	unsigned index;
	uint32_t response;

	codec = &codecs[address];
	nid = (uint8_t)((command >> 20) & 0x7f);
	verb = (command >> 8) & 0xfff;
	payload = command & 0xff;

	/* Four-bit verbs: format (2) and amplifier (3). */
	if ((verb >> 8) == 0x2 || (verb >> 8) == 0x3) {
		assert(set_count < SETS_MAX);
		sets[set_count].codec = address;
		sets[set_count].nid = nid;
		sets[set_count].verb = verb >> 8;
		sets[set_count].payload = command & 0xffff;
		set_count++;
		return 0;
	}

	widget = model_widget(codec, nid);
	switch (verb) {
	case 0xF00:
		parameter = payload;
		if (nid == 0) {
			if (parameter == 0x00)
				return codec->vendor;
			if (parameter == 0x04)
				return ((uint32_t)codec->function_group << 16) | 1U;
			return 0;
		}

		if (nid == codec->function_group) {
			if (parameter == 0x04) {
				/* The node range runs from the first widget to the highest one. */
				response = codec->first_widget;
				for (index = 0; index < codec->widget_count; index++) {
					if (codec->widgets[index].nid > response)
						response = codec->widgets[index].nid;
				}
				return ((uint32_t)codec->first_widget << 16) |
				    (response - codec->first_widget + 1U);
			}
			if (parameter == 0x05)
				return 1;
			if (parameter == 0x0A)
				return 0x000e0060U;
			return 0;
		}

		/* A node the layout does not list is a vendor-defined widget. */
		if (widget == NULL)
			return parameter == 0x09 ? 0x00f00000U : 0U;
		if (parameter == 0x09)
			return widget->caps;
		if (parameter == 0x0A)
			return widget->pcm;
		if (parameter == 0x0C)
			return widget->pin_caps;
		if (parameter == 0x0D)
			return widget->amp_in;
		if (parameter == 0x0E)
			return widget->connection_count;
		if (parameter == 0x12)
			return widget->amp_out;
		return 0;

	case 0xF02:
		assert(widget != NULL);
		response = 0;
		for (index = 0; index < 4 && payload + index < widget->connection_count; index++)
			response |= (uint32_t)widget->connections[payload + index] << (index * 8);
		return response;

	case 0xF1C:
		assert(widget != NULL);
		return widget->config;

	default:
		assert(set_count < SETS_MAX);
		sets[set_count].codec = address;
		sets[set_count].nid = nid;
		sets[set_count].verb = verb;
		sets[set_count].payload = payload;
		set_count++;
		return 0;
	}
}

/* Runs the CORB up to the write pointer, answering into the RIRB. */
static void
model_run_corb(uint16_t write_pointer)
{
	uint32_t command;
	uint32_t *entry;
	unsigned address;
	uint16_t rirb_write;

	while (corb_processed != write_pointer) {
		/*
		 * Like QEMU: after RINTCNT responses the CORB stalls, and only
		 * clearing a set response status (set only with RINTCTL) resets
		 * the count.
		 */
		if (rirb_count >= registers[0x5A])
			return;

		corb_processed = (corb_processed + 1) % 256;
		memcpy(&command, corb_memory + corb_processed * 4, 4);
		address = command >> 28;
		if (!codecs[address].present || codecs[address].silent)
			continue;

		memcpy(&rirb_write, registers + 0x58, 2);
		rirb_write = (uint16_t)((rirb_write + 1) % 256);
		entry = (uint32_t *)(rirb_memory + rirb_write * 8);
		entry[0] = model_answer(address, command);
		entry[1] = address;
		memcpy(registers + 0x58, &rirb_write, 2);
		rirb_count++;
		if (rirb_count >= registers[0x5A] && (registers[0x5C] & 1) != 0)
			registers[0x5D] |= 1;
	}
}

static int
set_seen(unsigned address, uint8_t nid, uint32_t verb, uint32_t payload)
{
	unsigned index;

	for (index = 0; index < set_count; index++) {
		if (sets[index].codec == address && sets[index].nid == nid &&
		    sets[index].verb == verb && sets[index].payload == payload)
			return 1;
	}
	return 0;
}

static int
codec_addressed(unsigned address)
{
	unsigned index;

	for (index = 0; index < set_count; index++) {
		if (sets[index].codec == address)
			return 1;
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* Register model.                                                  */

uint8_t
kern_mmio_read8(const volatile void *address)
{
	return *(const volatile uint8_t *)address;
}

uint16_t
kern_mmio_read16(const volatile void *address)
{
	uint16_t value;

	memcpy(&value, (const void *)address, 2);
	return value;
}

uint32_t
kern_mmio_read32(const volatile void *address)
{
	uint32_t value;

	memcpy(&value, (const void *)address, 4);
	return value;
}

static void
model_write(unsigned offset, uint32_t value, unsigned size)
{
	uint64_t base;

	/* Write-one-to-clear bytes. */
	if (offset == 0x0E && size == 2) {
		registers[0x0E] &= (uint8_t)~value;
		registers[0x0F] &= (uint8_t)~(value >> 8);
		return;
	}

	if (offset == 0x5D && size == 1) {
		/* Clearing a set status resets the count and lets the CORB run on. */
		if ((registers[0x5D] & 1) != 0 && (value & 1) != 0) {
			rirb_count = 0;
			registers[0x5D] &= (uint8_t)~value;
			if ((registers[0x4C] & 2) != 0 && (registers[0x5C] & 2) != 0)
				model_run_corb(kern_mmio_read16(registers + 0x48));
			return;
		}

		registers[0x5D] &= (uint8_t)~value;
		return;
	}

	if (offset >= 0x80 && (offset - 0x80) % 0x20 == 3 && size == 1) {
		registers[offset] &= (uint8_t)~value;
		return;
	}

	/* The CORB read pointer's reset bit reads back as written. */
	if (offset == 0x58 && size == 2) {
		if ((value & 0x8000) != 0)
			memset(registers + 0x58, 0, 2);
		return;
	}

	memcpy(registers + offset, &value, size);

	/* A new CORB write pointer makes the codecs answer. */
	if (offset == 0x48 && size == 2) {
		memcpy(&base, registers + 0x40, 4);
		base |= (uint64_t)kern_mmio_read32(registers + 0x44) << 32;
		corb_memory = (uint8_t *)(uintptr_t)base;
		memcpy(&base, registers + 0x50, 4);
		base |= (uint64_t)kern_mmio_read32(registers + 0x54) << 32;
		rirb_memory = (uint8_t *)(uintptr_t)base;
		if ((registers[0x4C] & 2) != 0 && (registers[0x5C] & 2) != 0)
			model_run_corb((uint16_t)value);
		else
			corb_processed = value;
	}

	/* Leaving controller reset makes the present codecs announce themselves. */
	if (offset == 0x08 && (value & 1) != 0) {
		uint16_t present = 0;
		unsigned index;

		for (index = 0; index < 15; index++) {
			if (codecs[index].present)
				present |= (uint16_t)(1U << index);
		}
		memcpy(registers + 0x0E, &present, 2);
	}
}

void
kern_mmio_write8(volatile void *address, uint8_t value)
{
	model_write((unsigned)((uint8_t *)(uintptr_t)address - registers), value, 1);
}

void
kern_mmio_write16(volatile void *address, uint16_t value)
{
	model_write((unsigned)((uint8_t *)(uintptr_t)address - registers), value, 2);
}

void
kern_mmio_write32(volatile void *address, uint32_t value)
{
	model_write((unsigned)((uint8_t *)(uintptr_t)address - registers), value, 4);
}

/* ---------------------------------------------------------------- */
/* Codec layouts.                                                   */

#define CAPS(type, extra)	(((uint32_t)(type) << 20) | (extra))
#define AMP_QEMU		(0x80000000U | (0x4aU << 8) | 0x4aU | (3U << 16))
#define AMP_LAPTOP		(0x80000000U | (0x57U << 8) | 0x57U)

static void
model_reset(void)
{
	memset(registers, 0, sizeof(registers));
	memset(codecs, 0, sizeof(codecs));
	set_count = 0;
	corb_processed = 0;
	rirb_count = 0;
	ticks = 0;
	/* GCAP: 4 output, 4 input streams, 64-bit; CORB and RIRB offer 256 entries. */
	registers[0x00] = 0x01;
	registers[0x01] = 0x44;
	registers[0x4E] = 0x42;
	registers[0x5E] = 0x42;
	pcie_control = 0x0810;
	memset(interrupts, 0, sizeof(interrupts));
}

static void
add_widget(struct model_codec *codec, uint8_t nid, uint32_t caps, uint32_t pin_caps,
    uint32_t config, uint32_t amp, uint32_t pcm, const uint8_t *connections, unsigned count)
{
	struct model_widget *widget;

	widget = &codec->widgets[codec->widget_count++];
	widget->nid = nid;
	widget->caps = caps;
	widget->pin_caps = pin_caps;
	widget->config = config;
	widget->amp_out = amp;
	widget->amp_in = amp;
	widget->pcm = pcm;
	if (count != 0)
		memcpy(widget->connections, connections, count);
	widget->connection_count = count;
}

/* QEMU hda-duplex: DAC 2 -> line-out pin 3; ADC 4 <- line-in pin 5. */
static void
model_qemu_duplex(unsigned address)
{
	static const uint8_t to_dac[] = { 2 };
	static const uint8_t to_in[] = { 5 };
	struct model_codec *codec = &codecs[address];

	codec->present = 1;
	codec->vendor = 0x1af40022U;
	codec->function_group = 1;
	codec->first_widget = 2;
	add_widget(codec, 2, CAPS(0, 0x0d), 0, 0, AMP_QEMU, 0x000e0060U, NULL, 0);
	add_widget(codec, 3, CAPS(4, 0x100), 0x10, 0x00010010U, 0, 0, to_dac, 1);
	add_widget(codec, 4, CAPS(1, 0x10b), 0, 0, AMP_QEMU, 0x00020040U, to_in, 1);
	add_widget(codec, 5, CAPS(4, 0), 0x20, 0x00a10020U, 0, 0, NULL, 0);
}

/*
 * A laptop: speaker 0x14 (internal, EAPD) and headphone 0x21 (jack) both
 * reach DAC 0x02 through mixer 0x0c; ADC 0x08 selects between the
 * internal mic 0x12 and the mic jack 0x19 through selector 0x23.
 */
static void
model_laptop(unsigned address)
{
	static const uint8_t mixer_c[] = { 0x02, 0x0b };
	static const uint8_t mixer_d[] = { 0x03, 0x0b };
	static const uint8_t pin_out[] = { 0x0c, 0x0d };
	static const uint8_t adc[] = { 0x23 };
	static const uint8_t selector[] = { 0x12, 0x19 };
	struct model_codec *codec = &codecs[address];

	codec->present = 1;
	codec->vendor = 0x10ec0236U;
	codec->function_group = 1;
	codec->first_widget = 2;
	add_widget(codec, 0x02, CAPS(0, 0x0d), 0, 0, AMP_LAPTOP, 0x000e0060U, NULL, 0);
	add_widget(codec, 0x03, CAPS(0, 0x0d), 0, 0, AMP_LAPTOP, 0x000e0060U, NULL, 0);
	add_widget(codec, 0x08, CAPS(1, 0x10b), 0, 0, AMP_LAPTOP, 0x000e0060U, adc, 1);
	add_widget(codec, 0x0b, CAPS(2, 0), 0, 0, 0, 0, NULL, 0);
	add_widget(codec, 0x0c, CAPS(2, 0x10a), 0, 0, 0x80000000U, 0, mixer_c, 2);
	add_widget(codec, 0x0d, CAPS(2, 0x10a), 0, 0, 0x80000000U, 0, mixer_d, 2);
	add_widget(codec, 0x12, CAPS(4, 0), 0x20, 0x90a60110U, 0, 0, NULL, 0);
	add_widget(codec, 0x14, CAPS(4, 0x104), 0x10010, 0x90170110U, 0x80000000U, 0, pin_out, 2);
	add_widget(codec, 0x19, CAPS(4, 0), 0x20, 0x03a11020U, 0, 0, NULL, 0);
	add_widget(codec, 0x21, CAPS(4, 0x104), 0x1001c, 0x0321101fU, 0x80000000U, 0, pin_out, 2);
	add_widget(codec, 0x23, CAPS(3, 0x100), 0, 0, 0, 0, selector, 2);
}

/* An Intel display codec, which the driver must skip. */
static void
model_intel_display(unsigned address)
{
	struct model_codec *codec = &codecs[address];

	codec->present = 1;
	codec->vendor = 0x8086280bU;
	codec->function_group = 1;
}

/* ---------------------------------------------------------------- */
/* Tests.                                                           */

static void *
attach(int expected)
{
	int error;

	registered_driver = NULL;
	assert(drv_pci_hda_driver_register() == 0);
	assert(registered_driver != NULL);
	assert(registered_driver->ids[0].class_code == 0x040300);
	service = NULL;
	driver_data = NULL;
	error = registered_driver->attach(fake_device, &registered_driver->ids[0]);
	assert(error == expected);
	return driver_data;
}

static void
detach(void)
{
	assert(registered_driver->detach(fake_device, 0) == 0);
	assert(driver_data == NULL);
	assert(bar_claimed == 0 && bar_mapped == 0 && irq_live == 0);
	assert(bus_master == 0);
	assert(live_dma == 0 && live_allocations == 0);
}

static void
test_qemu_duplex(void)
{
	struct drv_dma_buffer ring;
	uint32_t *bdl;
	uint32_t value;
	uint16_t value16;
	unsigned sd;
	unsigned index;
	struct audio_volume volume;

	model_reset();
	model_qemu_duplex(0);
	assert(attach(0) != NULL);

	/* Snoop forced, bus master on, out of reset, rings running, interrupts set. */
	assert((pcie_control & 0x0800) == 0);
	assert(bus_master == 1);
	assert((registers[0x08] & 1) == 1);
	assert((registers[0x4C] & 2) != 0 && (registers[0x5C] & 3) == 3);
	memcpy(&value, registers + 0x20, 4);
	assert(value == (0x80000000U | (1U << 4) | 1U));

	/* Publication registers both directions with the PCI DMA owner. */
	assert(service != NULL);
	assert(service->publish(fake_device, service_argument) == 0);
	assert(audio_registered && audio_dma == (struct drv_dma_device *)fake_dma);
	assert(audio_ops->playback == 1 && audio_ops->capture == 1);
	assert(audio_ops->format_count >= 1);
	assert(audio_ops->formats[0].rate == 48000 && audio_ops->formats[0].channels == 2);
	assert(audio_ops->formats[0].format == KERN_AUDIO_FORMAT_S16_LE);
	assert(audio_ops->set_volume != NULL);

	/* Paths: pin 3 out, pin 5 in, both converters powered. */
	assert(set_seen(0, 3, 0x707, 0x40));
	assert(set_seen(0, 5, 0x707, 0x20));
	assert(set_seen(0, 2, 0x705, 0));
	assert(set_seen(0, 4, 0x705, 0));

	/* Volume starts at full: DAC 2 amplifier at 0x4a, both sides. */
	assert(set_seen(0, 2, 3, 0x8000 | 0x2000 | 0x4a));
	assert(set_seen(0, 2, 3, 0x8000 | 0x1000 | 0x4a));

	/* Prepare playback: descriptor list, registers and converter verbs. */
	assert(drv_dma_alloc_coherent((struct drv_dma_device *)fake_dma, 32768, 4096, &ring) == 0);
	set_count = 0;
	assert(audio_ops->prepare(audio_private, 0, &audio_ops->formats[0], &ring, 4096, 8) == 0);
	sd = 0x80 + 4 * 0x20;
	memcpy(&value, registers + sd + 0x08, 4);
	assert(value == 32768);
	memcpy(&value16, registers + sd + 0x0C, 2);
	assert(value16 == 7);
	memcpy(&value16, registers + sd + 0x12, 2);
	assert(value16 == 0x0011);
	memcpy(&value, registers + sd + 0x00, 4);
	assert((value & 0x00f00000) == 0x00100000 && (value & 0x4) != 0 && (value & 2) == 0);
	memcpy(&value, registers + sd + 0x18, 4);
	bdl = (uint32_t *)(uintptr_t)((uint64_t)value | ((uint64_t)kern_mmio_read32(registers + sd + 0x1C) << 32));
	for (index = 0; index < 8; index++) {
		assert(bdl[index * 4] == (uint32_t)(ring.device_address + index * 4096));
		assert(bdl[index * 4 + 2] == 4096 && bdl[index * 4 + 3] == 1);
	}
	assert(set_seen(0, 2, 0x706, 0x10));
	assert(set_seen(0, 2, 2, 0x0011));

	/* Start and stop move the RUN bit. */
	assert(audio_ops->start(audio_private, 0) == 0);
	assert((registers[sd] & 2) != 0);
	audio_ops->stop(audio_private, 0);
	assert((registers[sd] & 2) == 0);

	/* A misaligned ring is refused. */
	ring.device_address += 64;
	assert(audio_ops->prepare(audio_private, 0, &audio_ops->formats[0], &ring, 4096, 8) == EINVAL);
	ring.device_address -= 64;

	/* The interrupt passes a completed fragment of each stream on and clears it. */
	value = (1U << 4) | 1U;
	memcpy(registers + 0x24, &value, 4);
	registers[sd + 3] = 0x04;
	registers[0x80 + 3] = 0x04 | 0x08;
	assert(irq_handler(irq_argument) == 1);
	assert(interrupts[0] == 1 && interrupts[1] == 1);
	assert(registers[sd + 3] == 0 && registers[0x80 + 3] == 0);
	memset(registers + 0x24, 0, 4);
	assert(irq_handler(irq_argument) == 0);

	/* Volume: half, then muted. */
	set_count = 0;
	volume.left = 50;
	volume.right = 100;
	volume.muted = 0;
	volume.reserved = 0;
	assert(audio_ops->set_volume(audio_private, &volume) == 0);
	assert(set_seen(0, 2, 3, 0x8000 | 0x2000 | (0x4a * 50 / 100)));
	assert(set_seen(0, 2, 3, 0x8000 | 0x1000 | 0x4a));
	volume.muted = 1;
	assert(audio_ops->set_volume(audio_private, &volume) == 0);
	assert(set_seen(0, 2, 3, 0x8000 | 0x2000 | 0x80 | (0x4a * 50 / 100)));
	memset(&volume, 0, sizeof(volume));
	assert(audio_ops->get_volume(audio_private, &volume) == 0);
	assert(volume.left == 50 && volume.muted == 1);

	drv_dma_free_coherent((struct drv_dma_device *)fake_dma, &ring);
	assert(service->unpublish(fake_device, service_argument) == 0);
	detach();
	printf("hda qemu duplex: reset, rings, paths, BDL, run, interrupt, volume, detach PASS\n");
}

static void
test_laptop(void)
{
	model_reset();
	model_intel_display(0);
	model_laptop(2);
	assert(attach(0) != NULL);
	assert(service->publish(fake_device, service_argument) == 0);

	/* The display codec is skipped and never configured. */
	assert(!codec_addressed(0));

	/* Speaker and headphone both reach DAC 2 through mixer 0x0c. */
	assert(set_seen(2, 0x14, 0x707, 0x40));
	assert(set_seen(2, 0x14, 0x70C, 0x02));
	assert(set_seen(2, 0x21, 0x707, 0xc0));
	assert(set_seen(2, 0x14, 0x701, 0));
	assert(set_seen(2, 0x21, 0x701, 0));
	assert(set_seen(2, 0x0c, 3, 0x4000 | 0x2000 | 0x1000 | (0 << 8) | 0));

	/* Capture takes the mic jack 0x19 through selector 0x23. */
	assert(set_seen(2, 0x23, 0x701, 1));
	assert(set_seen(2, 0x19, 0x707, 0x20));
	assert(!set_seen(2, 0x12, 0x707, 0x20));

	/* The volume target is DAC 2 with 0x57 steps. */
	assert(set_seen(2, 0x02, 3, 0x8000 | 0x2000 | 0x57));
	assert(audio_ops->set_volume != NULL);

	assert(service->unpublish(fake_device, service_argument) == 0);
	detach();
	printf("hda laptop: display codec skipped, speaker+HP on one DAC, EAPD, mic jack preferred PASS\n");
}

static void
test_failures(void)
{
	/* A codec that never answers: attach fails and releases everything. */
	model_reset();
	model_qemu_duplex(0);
	codecs[0].silent = 1;
	assert(attach(ENODEV) == NULL);
	assert(bar_claimed == 0 && bar_mapped == 0 && irq_live == 0);
	assert(live_dma == 0 && live_allocations == 0);

	/* Only a display codec: nothing to use. */
	model_reset();
	model_intel_display(0);
	assert(attach(ENODEV) == NULL);
	assert(live_dma == 0 && live_allocations == 0);
	printf("hda failures: silent codec, display-only controller, clean rollback PASS\n");
}

int
main(void)
{
	test_qemu_duplex();
	test_laptop();
	test_failures();
	printf("hda fixture: all PASS\n");
	return 0;
}
