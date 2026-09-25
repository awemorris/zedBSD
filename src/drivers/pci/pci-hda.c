/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * HD Audio controller driver: one analog codec, one output and one input
 * path, published through the audio framework as /dev/dspN.
 */

#include <drivers/audio/audio.h>
#include <drivers/generic/dma.h>
#include <drivers/pci/pci.h>
#include <drivers/pci/pci-hda.h>
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <uapi/errno.h>

/* Controller registers. */
#define HDA_GCAP		0x00U
#define HDA_GCTL		0x08U
#define HDA_STATESTS		0x0EU
#define HDA_INTCTL		0x20U
#define HDA_INTSTS		0x24U
#define HDA_CORBLBASE		0x40U
#define HDA_CORBUBASE		0x44U
#define HDA_CORBWP		0x48U
#define HDA_CORBRP		0x4AU
#define HDA_CORBCTL		0x4CU
#define HDA_CORBSIZE		0x4EU
#define HDA_RIRBLBASE		0x50U
#define HDA_RIRBUBASE		0x54U
#define HDA_RIRBWP		0x58U
#define HDA_RINTCNT		0x5AU
#define HDA_RIRBCTL		0x5CU
#define HDA_RIRBSTS		0x5DU
#define HDA_RIRBSIZE		0x5EU

/* Stream descriptor registers, relative to the descriptor. */
#define HDA_SD_BASE		0x80U
#define HDA_SD_SIZE		0x20U
#define HDA_SD_CTL		0x00U
#define HDA_SD_STS		0x03U
#define HDA_SD_LPIB		0x04U
#define HDA_SD_CBL		0x08U
#define HDA_SD_LVI		0x0CU
#define HDA_SD_FMT		0x12U
#define HDA_SD_BDPL		0x18U
#define HDA_SD_BDPU		0x1CU

/* Register bits. */
#define HDA_GCAP_64OK		0x0001U
#define HDA_GCTL_CRST		0x00000001U
#define HDA_INTCTL_GIE		0x80000000U
#define HDA_CORBCTL_RUN		0x02U
#define HDA_CORBRP_RESET	0x8000U
#define HDA_RIRBWP_RESET	0x8000U
#define HDA_RIRBCTL_DMAEN	0x02U
#define HDA_RIRBCTL_RINTCTL	0x01U
#define HDA_SD_CTL_SRST		0x00000001U
#define HDA_SD_CTL_RUN		0x00000002U
#define HDA_SD_CTL_IOCE		0x00000004U
#define HDA_SD_CTL_FEIE		0x00000008U
#define HDA_SD_CTL_DEIE		0x00000010U
#define HDA_SD_CTL_TAG_SHIFT	20U
#define HDA_SD_STS_BCIS		0x04U
#define HDA_SD_STS_FIFOE	0x08U
#define HDA_SD_STS_DESE		0x10U
#define HDA_SD_STS_ALL		0x1CU

/* The unsolicited-response flag in the upper word of a RIRB entry. */
#define HDA_RIRB_UNSOLICITED	0x10U

/* Codec verbs. */
#define HDA_VERB_GET_PARAMETER		0xF00U
#define HDA_VERB_GET_CONNECTION_LIST	0xF02U
#define HDA_VERB_GET_CONFIG_DEFAULT	0xF1CU
#define HDA_VERB_SET_CONNECTION_SELECT	0x701U
#define HDA_VERB_SET_POWER_STATE	0x705U
#define HDA_VERB_SET_CHANNEL_STREAM	0x706U
#define HDA_VERB_SET_PIN_CONTROL	0x707U
#define HDA_VERB_SET_EAPD		0x70CU
#define HDA_VERB4_SET_FORMAT		0x2U
#define HDA_VERB4_SET_AMP		0x3U

/* Codec parameters. */
#define HDA_PARAM_VENDOR		0x00U
#define HDA_PARAM_NODE_COUNT		0x04U
#define HDA_PARAM_FUNCTION_TYPE		0x05U
#define HDA_PARAM_WIDGET_CAPS		0x09U
#define HDA_PARAM_PCM			0x0AU
#define HDA_PARAM_PIN_CAPS		0x0CU
#define HDA_PARAM_AMP_IN_CAPS		0x0DU
#define HDA_PARAM_CONNECTION_LENGTH	0x0EU
#define HDA_PARAM_AMP_OUT_CAPS		0x12U

/* Widget capability bits and types. */
#define HDA_WIDGET_STEREO		0x00000001U
#define HDA_WIDGET_IN_AMP		0x00000002U
#define HDA_WIDGET_OUT_AMP		0x00000004U
#define HDA_WIDGET_AMP_OVERRIDE		0x00000008U
#define HDA_WIDGET_CONNECTIONS		0x00000100U
#define HDA_WIDGET_OUTPUT		0U
#define HDA_WIDGET_INPUT		1U
#define HDA_WIDGET_MIXER		2U
#define HDA_WIDGET_SELECTOR		3U
#define HDA_WIDGET_PIN			4U

/* Pin capability, pin control and configuration bits. */
#define HDA_PIN_CAP_HEADPHONE		0x00000008U
#define HDA_PIN_CAP_OUTPUT		0x00000010U
#define HDA_PIN_CAP_INPUT		0x00000020U
#define HDA_PIN_CAP_EAPD		0x00010000U
#define HDA_PIN_CONTROL_HEADPHONE	0x80U
#define HDA_PIN_CONTROL_OUTPUT		0x40U
#define HDA_PIN_CONTROL_INPUT		0x20U
#define HDA_EAPD_ENABLE			0x02U
#define HDA_CONFIG_CONNECTIVITY_SHIFT	30U
#define HDA_CONNECTIVITY_JACK		0U
#define HDA_CONNECTIVITY_NONE		1U
#define HDA_CONNECTIVITY_INTERNAL	2U

/* Amplifier capability and SET_AMP payload bits. */
#define HDA_AMP_CAP_MUTE		0x80000000U
#define HDA_AMP_SET_OUTPUT		0x8000U
#define HDA_AMP_SET_INPUT		0x4000U
#define HDA_AMP_SET_LEFT		0x2000U
#define HDA_AMP_SET_RIGHT		0x1000U
#define HDA_AMP_SET_INDEX_SHIFT		8U
#define HDA_AMP_SET_MUTE		0x0080U

/* PCM capability bits. */
#define HDA_PCM_RATE_44100		0x00000020U
#define HDA_PCM_RATE_48000		0x00000040U
#define HDA_PCM_BITS_16			0x00020000U
#define HDA_PCM_BITS_32			0x00100000U

/* The PCI Express capability and its Enable No Snoop bit. */
#define HDA_PCI_CAP_EXPRESS		0x10U
#define HDA_PCIE_DEVICE_CONTROL		0x08U
#define HDA_PCIE_NO_SNOOP		0x0800U

/* Limits. */
#define HDA_NID_MAX			128U
#define HDA_CONNECTIONS_MAX		16U
#define HDA_PATH_MAX			6U
#define HDA_OUTPUT_PINS_MAX		4U
#define HDA_BDL_ENTRIES			16U
#define HDA_FORMATS_MAX			3U
#define HDA_STREAM_TAG			1U
#define HDA_CODECS_MAX			15U

/*
 * Register reads that take at least a millisecond.  An MMIO read crosses
 * PCI Express or exits a virtual machine, which takes at least 0.2 us, so
 * this many reads bound a wait even before the scheduler's ticks start,
 * which is when PCI attach runs.
 */
#define HDA_READS_PER_MS		5000U

/* Deadlines in milliseconds. */
#define HDA_RESET_MS			100U
#define HDA_CODEC_SETTLE_MS		1U
#define HDA_COMMAND_MS			10U
#define HDA_STREAM_MS			10U

/* The stream index of each direction, matching the audio framework. */
#define HDA_PLAYBACK			0
#define HDA_CAPTURE			1

/* One widget of the chosen codec as read at attach. */
struct hda_widget {
	uint32_t caps;
	uint32_t pin_caps;
	uint32_t config;
	uint32_t amp_out_caps;
	uint32_t amp_in_caps;
	uint8_t connections[HDA_CONNECTIONS_MAX];
	unsigned connection_count;
	unsigned type;
	unsigned present;
};

/*
 * One route through the codec.
 *
 * nids[0] is where the search started (an output pin or an input
 * converter) and nids[length - 1] where it ended.  selects[i] is the
 * index in nids[i]'s connection list that leads to nids[i + 1].
 */
struct hda_path {
	uint8_t nids[HDA_PATH_MAX];
	uint8_t selects[HDA_PATH_MAX];
	unsigned length;
};

/* One stream descriptor and the descriptor list that covers the ring. */
struct hda_stream {
	unsigned present;
	unsigned index;
	uint8_t converter;
	struct drv_dma_buffer bdl;
};

/* One supported format and whether the input converter takes it too. */
struct hda_format {
	struct audio_format format;
	uint16_t code;
	unsigned capture;
};

/*
 * One HD Audio controller bound by PCI.
 *
 * Attach fills everything before publication; after that the command
 * mutex serializes codec verbs, the audio framework serializes each
 * stream, and the interrupt handler reads only the register window and
 * the published audio handle.
 */
struct hda_controller {
	struct drv_pci_device *pci;
	struct drv_dma_device *dma;
	const char *stage;
	struct drv_pci_enable_state enable_state;
	unsigned saved;
	unsigned bar_claimed;
	struct drv_pci_mapping registers;
	struct drv_pci_irq irq;
	unsigned irq_count;
	void *irq_cookie;
	struct drv_dma_buffer command_rings;
	unsigned corb_entries;
	unsigned rirb_entries;
	unsigned corb_write;
	unsigned rirb_read;
	unsigned rings_running;
	struct mutex command_lock;
	unsigned input_streams;
	unsigned output_streams;
	unsigned dma64;
	unsigned codec;
	uint8_t function_group;
	struct hda_widget *widgets;
	struct hda_path outputs[HDA_OUTPUT_PINS_MAX];
	unsigned output_count;
	struct hda_path input;
	unsigned has_input;
	uint8_t volume_nid;
	unsigned volume_steps;
	unsigned volume_mute;
	struct audio_volume volume;
	struct hda_format formats[HDA_FORMATS_MAX];
	struct audio_format format_list[HDA_FORMATS_MAX];
	unsigned format_count;
	struct drv_audio_ops ops;
	struct hda_stream streams[2];
	struct drv_audio_device *audio;
	unsigned stream_errors;
};

/*
 * Forward declaration.
 */
static int hda_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int hda_detach(struct drv_pci_device *device, unsigned flags);
static int hda_publish(struct drv_pci_device *device, void *argument);
static int hda_unpublish(struct drv_pci_device *device, void *argument);
static int hda_start(struct hda_controller *controller);
static int hda_stop(struct hda_controller *controller);
static uint8_t hda_read8(struct hda_controller *controller, unsigned offset);
static uint16_t hda_read16(struct hda_controller *controller, unsigned offset);
static uint32_t hda_read32(struct hda_controller *controller, unsigned offset);
static void hda_write8(struct hda_controller *controller, unsigned offset, uint8_t value);
static void hda_write16(struct hda_controller *controller, unsigned offset, uint16_t value);
static void hda_write32(struct hda_controller *controller, unsigned offset, uint32_t value);
static void hda_delay_ms(struct hda_controller *controller, unsigned milliseconds);
static int hda_wait32(struct hda_controller *controller, unsigned offset, uint32_t mask, uint32_t value, unsigned milliseconds);
static int hda_wait8(struct hda_controller *controller, unsigned offset, uint8_t mask, uint8_t value, unsigned milliseconds);
static int hda_wait16(struct hda_controller *controller, unsigned offset, uint16_t mask, uint16_t value, unsigned milliseconds);
static void hda_force_snoop(struct hda_controller *controller);
static int hda_controller_reset(struct hda_controller *controller);
static int hda_command_rings_start(struct hda_controller *controller);
static void hda_command_rings_stop(struct hda_controller *controller);
static unsigned hda_ring_entries(uint8_t size_register, uint8_t *select);
static int hda_command(struct hda_controller *controller, uint32_t command, uint32_t *response);
static int hda_verb(struct hda_controller *controller, uint8_t nid, uint32_t verb, uint32_t payload, uint32_t *response);
static int hda_verb4(struct hda_controller *controller, uint8_t nid, uint32_t verb, uint32_t payload);
static int hda_parameter(struct hda_controller *controller, uint8_t nid, uint32_t parameter, uint32_t *value);
static int hda_codecs_probe(struct hda_controller *controller);
static int hda_codec_probe(struct hda_controller *controller, unsigned codec);
static int hda_widgets_read(struct hda_controller *controller, uint8_t start, unsigned count);
static int hda_connections_read(struct hda_controller *controller, uint8_t nid, struct hda_widget *widget);
static unsigned hda_connectivity(const struct hda_widget *widget);
static int hda_path_search(struct hda_controller *controller, uint8_t nid, unsigned target, unsigned pass, struct hda_path *path);
static int hda_path_accepts(const struct hda_widget *widget, unsigned target, unsigned pass);
static int hda_outputs_find(struct hda_controller *controller);
static int hda_input_find(struct hda_controller *controller);
static int hda_path_program(struct hda_controller *controller, const struct hda_path *path, int capture);
static void hda_volume_target_find(struct hda_controller *controller);
static int hda_volume_write(struct hda_controller *controller, const struct audio_volume *volume);
static int hda_formats_build(struct hda_controller *controller);
static int hda_streams_start(struct hda_controller *controller);
static int hda_stream_reset(struct hda_controller *controller, const struct hda_stream *stream);
static int hda_interrupt_start(struct hda_controller *controller);
static int hda_interrupt(void *argument);
static int hda_ops_prepare(void *private_data, int capture, const struct audio_format *format, const struct drv_dma_buffer *buffer, uint32_t fragment_bytes, uint32_t fragment_count);
static int hda_ops_start(void *private_data, int capture);
static void hda_ops_stop(void *private_data, int capture);
static uint32_t hda_ops_position(void *private_data, int capture);
static int hda_ops_get_volume(void *private_data, struct audio_volume *volume);
static int hda_ops_set_volume(void *private_data, const struct audio_volume *volume);

/*
 * Registers the driver for every PCI HD Audio controller.
 */
int
drv_pci_hda_driver_register(void)
{
	static const struct drv_pci_id identifiers[] = {
		{ DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
		  0x040300U, 0xffff00U, 0U }
	};
	static struct drv_pci_driver driver = {
		"hda", identifiers, 1U, NULL, hda_attach, hda_detach,
		NULL, NULL, NULL, { 0U, 0U, 0U, 0U }
	};
	int error;

	/* Lets PCI bind every multimedia controller of the HD Audio subclass. */
	error = drv_pci_driver_register(&driver);
	if (error != 0)
		return error;

	/* Succeeded: later PCI probing attaches each controller. */
	return 0;
}

/* Acquires one controller and stages its audio registration for PCI. */
static int
hda_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct hda_controller *controller;
	int error;
	int cleanup;

	/* Matching has already consumed the class record. */
	(void)id;

	/* Allocates the controller state. */
	controller = kern_calloc(1U, sizeof(*controller));
	if (controller == NULL)
		return ENOMEM;

	/* Allocates the widget table of the chosen codec. */
	controller->widgets = kern_calloc(HDA_NID_MAX, sizeof(*controller->widgets));
	if (controller->widgets == NULL) {
		kern_free(controller);
		return ENOMEM;
	}

	/* Serializes codec verbs from attach, prepare and volume requests. */
	controller->pci = device;
	error = mutex_init(&controller->command_lock, LOCK_RANK_DEVICE, "hda command");
	if (error != 0) {
		kern_free(controller->widgets);
		kern_free(controller);
		return error;
	}

	/* Gives PCI an owner before the first hardware acquisition. */
	error = drv_pci_device_set_driver_data(device, controller);
	if (error != 0) {
		kern_free(controller->widgets);
		kern_free(controller);
		return error;
	}

	/* Brings the controller up and stages publication. */
	error = hda_start(controller);
	if (error != 0) {
		kern_logf("hda: attach stopped at %s: %d\n", controller->stage, error);

		/* A controller that would not stop keeps its owner for a detach retry. */
		cleanup = hda_stop(controller);
		if (cleanup != 0) {
			kern_logf("hda: attach cleanup retained %d\n", cleanup);
			return 0;
		}

		/* Removes PCI's pointer once no hardware lease remains. */
		cleanup = drv_pci_device_set_driver_data(device, NULL);
		if (cleanup != 0)
			return 0;

		kern_free(controller->widgets);
		kern_free(controller);
		return error;
	}

	/* Succeeded: PCI publishes the device after binding it. */
	return 0;
}

/* Stops an unpublished controller and releases it. */
static int
hda_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	struct hda_controller *controller;
	int error;

	/* PCI has already applied its detach policy. */
	(void)flags;

	/* A failed attach may already have released everything. */
	controller = drv_pci_device_driver_data(device);
	if (controller == NULL)
		return 0;

	/* The audio device must be withdrawn before its hardware goes. */
	if (controller->audio != NULL)
		return EBUSY;

	/* Stops every stream, the interrupt and the command rings. */
	error = hda_stop(controller);
	if (error != 0)
		return error;

	/* Removes PCI's pointer before freeing the controller. */
	error = drv_pci_device_set_driver_data(device, NULL);
	if (error != 0)
		return error;

	kern_free(controller->widgets);
	kern_free(controller);

	/* Succeeded: PCI may clear the binding. */
	return 0;
}

/* Publishes /dev/dspN and /dev/mixerN for a started controller. */
static int
hda_publish(
	struct drv_pci_device *device,
	void *argument)
{
	struct hda_controller *controller;
	int error;

	/* Registers the controller's own ops table with the PCI DMA owner. */
	(void)device;
	controller = argument;
	error = drv_audio_register(&controller->ops,
	    controller,
	    controller->dma,
	    &controller->audio);
	if (error != 0)
		return error;

	/* Records which codec and paths the device uses. */
	kern_logf("hda: codec %u, %u output pin(s), input %s, volume %s, %u format(s), %s\n",
	    controller->codec,
	    controller->output_count,
	    controller->has_input != 0U ? "yes" : "no",
	    controller->volume_steps != 0U ? "yes" : "no",
	    controller->format_count,
	    controller->irq.type == DRV_PCI_IRQ_MSI ? "MSI" : "INTx");

	/* Succeeded: the audio nodes are visible. */
	return 0;
}

/* Withdraws the audio nodes unless a stream is open. */
static int
hda_unpublish(
	struct drv_pci_device *device,
	void *argument)
{
	struct hda_controller *controller;
	int error;

	/* Unregisters; EBUSY keeps the device published for a retry. */
	(void)device;
	controller = argument;
	error = drv_audio_unregister(controller->audio);
	if (error != 0)
		return error;

	controller->audio = NULL;

	/* Succeeded: no audio node names the controller. */
	return 0;
}

/* Brings the controller from reset to a staged audio publication. */
static int
hda_start(
	struct hda_controller *controller)
{
	static const struct drv_pci_service_interface service = {
		hda_publish, hda_unpublish
	};
	struct drv_pci_bar bar;
	uint16_t capabilities;
	int error;

	/* Uses the PCI DMA owner so ring addresses are bus addresses. */
	controller->stage = "dma-provider";
	controller->dma = drv_pci_device_dma(controller->pci);
	if (controller->dma == NULL)
		return ENODEV;

	/* Saves the command bits restored at detach. */
	controller->stage = "save-pci-state";
	error = drv_pci_device_save_enable_state(controller->pci, &controller->enable_state);
	if (error != 0)
		return error;

	controller->saved = 1U;

	/* Enables register decoding. */
	controller->stage = "enable-memory";
	error = drv_pci_device_enable_memory(controller->pci);
	if (error != 0)
		return error;

	/* Checks that BAR0 is a memory window large enough for the registers. */
	controller->stage = "bar0";
	error = drv_pci_device_bar(controller->pci, 0U, &bar);
	if (error != 0)
		return error;
	if (bar.type != DRV_PCI_BAR_MEMORY32 && bar.type != DRV_PCI_BAR_MEMORY64)
		return ENODEV;
	if (bar.size < 0x4000U)
		return ENODEV;

	/* Claims and maps the register window. */
	controller->stage = "map-registers";
	error = drv_pci_device_claim_bar(controller->pci, 0U);
	if (error != 0)
		return error;

	controller->bar_claimed = 1U;
	error = drv_pci_device_map_bar(controller->pci,
	    0U,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &controller->registers);
	if (error != 0)
		return error;

	/* Makes the controller's DMA snoop the CPU caches. */
	controller->stage = "snoop";
	hda_force_snoop(controller);

	/* Lets the controller reach the command rings and stream memory. */
	controller->stage = "enable-bus-master";
	error = drv_pci_device_set_bus_master(controller->pci, true);
	if (error != 0)
		return error;

	/* Resets the controller and lets the codecs announce themselves. */
	controller->stage = "controller-reset";
	error = hda_controller_reset(controller);
	if (error != 0)
		return error;

	/* Reads the stream counts and the DMA width. */
	controller->stage = "capabilities";
	capabilities = hda_read16(controller, HDA_GCAP);
	controller->input_streams = (capabilities >> 8) & 0xfU;
	controller->output_streams = (capabilities >> 12) & 0xfU;
	if ((capabilities & HDA_GCAP_64OK) != 0U)
		controller->dma64 = 1U;
	if (controller->input_streams == 0U && controller->output_streams == 0U)
		return ENODEV;

	/* Starts the command rings through which codecs are asked. */
	controller->stage = "command-rings";
	error = hda_command_rings_start(controller);
	if (error != 0)
		return error;

	/* Finds an analog codec with an output path. */
	controller->stage = "codecs";
	error = hda_codecs_probe(controller);
	if (error != 0)
		return error;

	/* Resets the stream descriptors and allocates their descriptor lists. */
	controller->stage = "streams";
	error = hda_streams_start(controller);
	if (error != 0)
		return error;

	/* Establishes the interrupt before any stream can run. */
	controller->stage = "interrupt";
	error = hda_interrupt_start(controller);
	if (error != 0)
		return error;

	/* Fills the per-instance ops table the framework borrows. */
	controller->ops.playback = controller->streams[HDA_PLAYBACK].present;
	controller->ops.capture = controller->streams[HDA_CAPTURE].present;
	controller->ops.formats = controller->format_list;
	controller->ops.format_count = controller->format_count;
	controller->ops.prepare = hda_ops_prepare;
	controller->ops.start = hda_ops_start;
	controller->ops.stop = hda_ops_stop;
	controller->ops.position = hda_ops_position;
	controller->ops.get_volume = hda_ops_get_volume;
	if (controller->volume_steps != 0U)
		controller->ops.set_volume = hda_ops_set_volume;

	/* Stages publication; PCI calls hda_publish after binding. */
	controller->stage = "service";
	error = drv_pci_device_set_service(controller->pci, &service, controller);
	if (error != 0)
		return error;

	/* Succeeded: the controller is ready for publication. */
	return 0;
}

/*
 * Stops whatever hda_start acquired, in reverse order.
 *
 * Each step checks whether it was reached, so this serves both a failed
 * attach and detach.
 */
static int
hda_stop(
	struct hda_controller *controller)
{
	uint32_t control;
	unsigned index;
	int error;

	/* Stops both streams and masks every controller interrupt. */
	if (controller->registers.address != NULL) {
		for (index = 0; index < 2U; index++) {
			if (controller->streams[index].present != 0U)
				(void)hda_stream_reset(controller, &controller->streams[index]);
		}

		hda_write32(controller, HDA_INTCTL, 0U);
	}

	/* Removes the interrupt; EBUSY keeps it for a retry while dispatch drains. */
	if (controller->irq_cookie != NULL) {
		error = drv_pci_device_disestablish_irq_checked(controller->pci, controller->irq_cookie);
		if (error != 0)
			return error;

		controller->irq_cookie = NULL;
	}

	if (controller->irq_count != 0U) {
		drv_pci_device_free_irqs(controller->pci, &controller->irq, controller->irq_count);
		controller->irq_count = 0U;
	}

	/* Stops the command rings and holds the controller in reset. */
	if (controller->registers.address != NULL) {
		hda_command_rings_stop(controller);
		control = hda_read32(controller, HDA_GCTL);
		hda_write32(controller, HDA_GCTL, control & ~HDA_GCTL_CRST);
	}

	/* Ends DMA before the memory it used is returned. */
	if (controller->saved != 0U)
		(void)drv_pci_device_set_bus_master(controller->pci, false);

	/* Returns the descriptor lists and the command rings. */
	for (index = 0; index < 2U; index++) {
		if (controller->streams[index].bdl.address != NULL)
			drv_dma_free_coherent(controller->dma, &controller->streams[index].bdl);
		controller->streams[index].present = 0U;
	}

	if (controller->command_rings.address != NULL)
		drv_dma_free_coherent(controller->dma, &controller->command_rings);

	/* Unmaps and releases the register window. */
	if (controller->registers.address != NULL) {
		drv_pci_device_unmap_bar(controller->pci, &controller->registers);
		controller->registers.address = NULL;
	}

	if (controller->bar_claimed != 0U) {
		drv_pci_device_release_bar(controller->pci, 0U);
		controller->bar_claimed = 0U;
	}

	/* Restores the command bits the controller had before attach. */
	if (controller->saved != 0U) {
		(void)drv_pci_device_restore_enable_state(controller->pci, &controller->enable_state);
		controller->saved = 0U;
	}

	/* Succeeded: the controller holds no hardware resource. */
	return 0;
}

/* Reads one byte register. */
static uint8_t
hda_read8(
	struct hda_controller *controller,
	unsigned offset)
{
	/* Reports the register value. */
	return kern_mmio_read8((uint8_t *)controller->registers.address + offset);
}

/* Reads one 16-bit register. */
static uint16_t
hda_read16(
	struct hda_controller *controller,
	unsigned offset)
{
	/* Reports the register value. */
	return kern_mmio_read16((uint8_t *)controller->registers.address + offset);
}

/* Reads one 32-bit register. */
static uint32_t
hda_read32(
	struct hda_controller *controller,
	unsigned offset)
{
	/* Reports the register value. */
	return kern_mmio_read32((uint8_t *)controller->registers.address + offset);
}

/* Writes one byte register. */
static void
hda_write8(
	struct hda_controller *controller,
	unsigned offset,
	uint8_t value)
{
	/* Stores the value. */
	kern_mmio_write8((uint8_t *)controller->registers.address + offset, value);
}

/* Writes one 16-bit register. */
static void
hda_write16(
	struct hda_controller *controller,
	unsigned offset,
	uint16_t value)
{
	/* Stores the value. */
	kern_mmio_write16((uint8_t *)controller->registers.address + offset, value);
}

/* Writes one 32-bit register. */
static void
hda_write32(
	struct hda_controller *controller,
	unsigned offset,
	uint32_t value)
{
	/* Stores the value. */
	kern_mmio_write32((uint8_t *)controller->registers.address + offset, value);
}

/*
 * Waits at least the given time without sleeping.
 *
 * Attach runs before the scheduler's ticks start, so the wait ends when
 * either the tick deadline has passed or enough register reads have
 * taken at least that long.  One extra tick covers the partial tick
 * already under way.
 */
static void
hda_delay_ms(
	struct hda_controller *controller,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	unsigned reads;

	/* Reads a harmless register until either measure says the time is up. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (reads = 0; reads < milliseconds * HDA_READS_PER_MS; reads++) {
		now = sched_ticks();
		if (now >= deadline)
			break;

		(void)hda_read16(controller, HDA_GCAP);
	}
}

/* Waits for a 32-bit register to show a value under a mask. */
static int
hda_wait32(
	struct hda_controller *controller,
	unsigned offset,
	uint32_t mask,
	uint32_t value,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	unsigned reads;
	uint32_t current;

	/* Polls until the value appears or either measure of the deadline passes. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (reads = 0; reads < milliseconds * HDA_READS_PER_MS; reads++) {
		current = hda_read32(controller, offset);
		if ((current & mask) == value)
			return 0;

		now = sched_ticks();
		if (now >= deadline)
			break;
	}

	/* Reports a register that never showed the value. */
	return ETIMEDOUT;
}

/* Waits for a byte register to show a value under a mask. */
static int
hda_wait8(
	struct hda_controller *controller,
	unsigned offset,
	uint8_t mask,
	uint8_t value,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	unsigned reads;
	uint8_t current;

	/* Polls until the value appears or either measure of the deadline passes. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (reads = 0; reads < milliseconds * HDA_READS_PER_MS; reads++) {
		current = hda_read8(controller, offset);
		if ((current & mask) == value)
			return 0;

		now = sched_ticks();
		if (now >= deadline)
			break;
	}

	/* Reports a register that never showed the value. */
	return ETIMEDOUT;
}

/* Waits for a 16-bit register to show a value under a mask. */
static int
hda_wait16(
	struct hda_controller *controller,
	unsigned offset,
	uint16_t mask,
	uint16_t value,
	unsigned milliseconds)
{
	uint64_t deadline;
	uint64_t now;
	unsigned reads;
	uint16_t current;

	/* Polls until the value appears or either measure of the deadline passes. */
	deadline = sched_ticks() + kern_ms_to_ticks(milliseconds) + 1U;
	for (reads = 0; reads < milliseconds * HDA_READS_PER_MS; reads++) {
		current = hda_read16(controller, offset);
		if ((current & mask) == value)
			return 0;

		now = sched_ticks();
		if (now >= deadline)
			break;
	}

	/* Reports a register that never showed the value. */
	return ETIMEDOUT;
}

/*
 * Stops the controller from marking its DMA as no-snoop.
 *
 * The rings are cacheable memory, so a no-snoop read could see stale
 * data.  Intel's PCH HD Audio sets Enable No Snoop in its PCI Express
 * Device Control register; a controller without that capability is left
 * alone.
 */
static void
hda_force_snoop(
	struct hda_controller *controller)
{
	unsigned capability;
	uint16_t control;
	int error;

	/* Finds the PCI Express capability. */
	error = drv_pci_device_find_capability(controller->pci, HDA_PCI_CAP_EXPRESS, &capability);
	if (error != 0)
		return;

	/* Clears Enable No Snoop if it is set. */
	error = drv_pci_device_config_read16(controller->pci,
	    capability + HDA_PCIE_DEVICE_CONTROL,
	    &control);
	if (error != 0)
		return;
	if ((control & HDA_PCIE_NO_SNOOP) == 0U)
		return;

	control &= (uint16_t)~HDA_PCIE_NO_SNOOP;
	(void)drv_pci_device_config_write16(controller->pci,
	    capability + HDA_PCIE_DEVICE_CONTROL,
	    control);
}

/*
 * Puts the controller through reset and lets the codecs announce
 * themselves.
 *
 * DMA left running by firmware is stopped first, so that no engine walks
 * memory the kernel is about to reuse.
 */
static int
hda_controller_reset(
	struct hda_controller *controller)
{
	uint32_t control;
	int error;

	/* Stops firmware's command rings and masks interrupts. */
	hda_command_rings_stop(controller);
	hda_write32(controller, HDA_INTCTL, 0U);

	/* Enters reset and waits until the controller is in it. */
	control = hda_read32(controller, HDA_GCTL);
	hda_write32(controller, HDA_GCTL, control & ~HDA_GCTL_CRST);
	error = hda_wait32(controller, HDA_GCTL, HDA_GCTL_CRST, 0U, HDA_RESET_MS);
	if (error != 0)
		return error;

	/* Holds reset briefly, then leaves it and waits until the controller is out. */
	hda_delay_ms(controller, HDA_CODEC_SETTLE_MS);
	control = hda_read32(controller, HDA_GCTL);
	hda_write32(controller, HDA_GCTL, control | HDA_GCTL_CRST);
	error = hda_wait32(controller, HDA_GCTL, HDA_GCTL_CRST, HDA_GCTL_CRST, HDA_RESET_MS);
	if (error != 0)
		return error;

	/* Gives the codecs time to request their addresses. */
	hda_delay_ms(controller, HDA_CODEC_SETTLE_MS);

	/* Succeeded: the controller is out of reset. */
	return 0;
}

/* Reports the entries of a CORB or RIRB size register and the selector for them. */
static unsigned
hda_ring_entries(
	uint8_t size_register,
	uint8_t *select)
{
	/* Takes the largest size the controller offers. */
	if ((size_register & 0x40U) != 0U) {
		*select = 2U;
		return 256U;
	}

	if ((size_register & 0x20U) != 0U) {
		*select = 1U;
		return 16U;
	}

	/* The smallest ring every controller has. */
	*select = 0U;
	return 2U;
}

/*
 * Starts the command output ring (CORB) and the response input ring
 * (RIRB) in one coherent page: CORB at the start, RIRB from 2 KiB.
 */
static int
hda_command_rings_start(
	struct hda_controller *controller)
{
	uint64_t address;
	uint8_t select;
	int error;

	/* Allocates the page both rings live in. */
	error = drv_dma_alloc_coherent(controller->dma, 4096U, 4096U, &controller->command_rings);
	if (error != 0)
		return error;

	kern_memset(controller->command_rings.address, 0, 4096U);
	address = controller->command_rings.device_address;
	if (controller->dma64 == 0U && (address >> 32) != 0U)
		return EINVAL;

	/* Sizes the CORB and points the controller at it. */
	controller->corb_entries = hda_ring_entries(hda_read8(controller, HDA_CORBSIZE), &select);
	hda_write8(controller, HDA_CORBSIZE, select);
	hda_write32(controller, HDA_CORBLBASE, (uint32_t)address);
	hda_write32(controller, HDA_CORBUBASE, (uint32_t)(address >> 32));

	/*
	 * Resets the CORB read pointer.  Some controllers never show the reset
	 * bit as set, so a missing acknowledgement is not an error; the bit
	 * must read back clear, though.
	 */
	hda_write16(controller, HDA_CORBRP, HDA_CORBRP_RESET);
	(void)hda_wait16(controller, HDA_CORBRP, HDA_CORBRP_RESET, HDA_CORBRP_RESET, HDA_COMMAND_MS);
	hda_write16(controller, HDA_CORBRP, 0U);
	error = hda_wait16(controller, HDA_CORBRP, HDA_CORBRP_RESET, 0U, HDA_COMMAND_MS);
	if (error != 0)
		return error;

	/* Empties the CORB and starts it. */
	hda_write16(controller, HDA_CORBWP, 0U);
	controller->corb_write = 0U;
	hda_write8(controller, HDA_CORBCTL, HDA_CORBCTL_RUN);

	/* Sizes the RIRB and points the controller at it. */
	address += 2048U;
	controller->rirb_entries = hda_ring_entries(hda_read8(controller, HDA_RIRBSIZE), &select);
	hda_write8(controller, HDA_RIRBSIZE, select);
	hda_write32(controller, HDA_RIRBLBASE, (uint32_t)address);
	hda_write32(controller, HDA_RIRBUBASE, (uint32_t)(address >> 32));

	/*
	 * Resets the RIRB write pointer and starts the RIRB.  The response
	 * status is enabled even though responses are polled: some controllers,
	 * QEMU's among them, stop taking commands after RINTCNT responses until
	 * software clears that status, which each command does.  The controller
	 * interrupt (INTCTL.CIE) stays off, so no interrupt is raised.
	 */
	hda_write16(controller, HDA_RIRBWP, HDA_RIRBWP_RESET);
	controller->rirb_read = 0U;
	hda_write16(controller, HDA_RINTCNT, 1U);
	hda_write8(controller, HDA_RIRBCTL, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_RINTCTL);
	controller->rings_running = 1U;

	/* Succeeded: codec verbs can be sent. */
	return 0;
}

/* Stops both command rings and waits until their DMA has stopped. */
static void
hda_command_rings_stop(
	struct hda_controller *controller)
{
	/* Stops the CORB engine. */
	hda_write8(controller, HDA_CORBCTL, 0U);
	(void)hda_wait8(controller, HDA_CORBCTL, HDA_CORBCTL_RUN, 0U, HDA_COMMAND_MS);

	/* Stops the RIRB engine. */
	hda_write8(controller, HDA_RIRBCTL, 0U);
	(void)hda_wait8(controller, HDA_RIRBCTL, HDA_RIRBCTL_DMAEN, 0U, HDA_COMMAND_MS);
	controller->rings_running = 0U;
}

/*
 * Sends one codec command and waits for its response.
 *
 * Responses arrive in order; an unsolicited response in between is
 * skipped.  Runs in thread context only.
 */
static int
hda_command(
	struct hda_controller *controller,
	uint32_t command,
	uint32_t *response)
{
	volatile uint32_t *corb;
	volatile uint32_t *rirb;
	uint64_t deadline;
	uint64_t now;
	uint32_t extended;
	unsigned hardware;
	unsigned reads;
	int error;

	/* Refuses a command while the rings are down. */
	if (controller->rings_running == 0U)
		return EIO;

	/* Queues the command in the next CORB entry. */
	corb = controller->command_rings.address;
	rirb = (volatile uint32_t *)((uint8_t *)controller->command_rings.address + 2048U);
	mutex_lock(&controller->command_lock);

	/* A response that came after an earlier command timed out belongs to no one. */
	controller->rirb_read = hda_read16(controller, HDA_RIRBWP) & 0xffU;

	controller->corb_write = (controller->corb_write + 1U) % controller->corb_entries;
	corb[controller->corb_write] = command;
	kern_io_write_barrier();
	hda_write16(controller, HDA_CORBWP, (uint16_t)controller->corb_write);

	/* Waits for a solicited response in the RIRB, bounded as hda_wait32 is. */
	error = ETIMEDOUT;
	deadline = sched_ticks() + kern_ms_to_ticks(HDA_COMMAND_MS) + 1U;
	for (reads = 0; reads < HDA_COMMAND_MS * HDA_READS_PER_MS; reads++) {
		hardware = hda_read16(controller, HDA_RIRBWP) & 0xffU;
		kern_io_read_barrier();
		while (controller->rirb_read != hardware) {
			controller->rirb_read = (controller->rirb_read + 1U) % controller->rirb_entries;
			extended = rirb[controller->rirb_read * 2U + 1U];
			if ((extended & HDA_RIRB_UNSOLICITED) != 0U)
				continue;

			*response = rirb[controller->rirb_read * 2U];
			error = 0;
			break;
		}

		if (error == 0)
			break;

		now = sched_ticks();
		if (now >= deadline)
			break;
	}

	/* Acknowledges the response flag the controller raised. */
	hda_write8(controller, HDA_RIRBSTS, 0x05U);

	mutex_unlock(&controller->command_lock);

	/* Reports a codec that did not answer. */
	if (error != 0)
		return error;

	/* Succeeded: the response is the codec's answer. */
	return 0;
}

/* Sends a 12-bit verb with an 8-bit payload to a node of the chosen codec. */
static int
hda_verb(
	struct hda_controller *controller,
	uint8_t nid,
	uint32_t verb,
	uint32_t payload,
	uint32_t *response)
{
	uint32_t command;
	uint32_t discarded;
	int error;

	/* Encodes codec, node, verb and payload. */
	command = ((uint32_t)controller->codec << 28) |
	    ((uint32_t)nid << 20) |
	    (verb << 8) |
	    (payload & 0xffU);
	if (response == NULL)
		response = &discarded;

	error = hda_command(controller, command, response);
	if (error != 0)
		return error;

	/* Succeeded: the codec answered. */
	return 0;
}

/* Sends a 4-bit verb with a 16-bit payload to a node of the chosen codec. */
static int
hda_verb4(
	struct hda_controller *controller,
	uint8_t nid,
	uint32_t verb,
	uint32_t payload)
{
	uint32_t command;
	uint32_t response;
	int error;

	/* Encodes codec, node, verb and payload. */
	command = ((uint32_t)controller->codec << 28) |
	    ((uint32_t)nid << 20) |
	    (verb << 16) |
	    (payload & 0xffffU);
	error = hda_command(controller, command, &response);
	if (error != 0)
		return error;

	/* Succeeded: the codec accepted the verb. */
	return 0;
}

/* Reads one codec parameter. */
static int
hda_parameter(
	struct hda_controller *controller,
	uint8_t nid,
	uint32_t parameter,
	uint32_t *value)
{
	int error;

	/* Asks the node for the parameter. */
	error = hda_verb(controller, nid, HDA_VERB_GET_PARAMETER, parameter, value);
	if (error != 0)
		return error;

	/* Succeeded: value holds the parameter. */
	return 0;
}

/* Picks the first analog codec that has an output path. */
static int
hda_codecs_probe(
	struct hda_controller *controller)
{
	uint16_t present;
	unsigned codec;
	int error;

	/* Reads and acknowledges which addresses have a codec. */
	present = hda_read16(controller, HDA_STATESTS);
	hda_write16(controller, HDA_STATESTS, present);

	/* Tries each codec in address order. */
	for (codec = 0; codec < HDA_CODECS_MAX; codec++) {
		if ((present & (1U << codec)) == 0U)
			continue;

		error = hda_codec_probe(controller, codec);
		if (error == 0)
			return 0;

		kern_logf("hda: codec %u not used: %d\n", codec, error);
	}

	/* Reports a controller with no usable codec. */
	return ENODEV;
}

/*
 * Reads one codec and chooses its paths.
 *
 * Intel's display codecs carry only HDMI and DisplayPort sound and are
 * skipped.  ENODEV means this codec cannot be used.
 */
static int
hda_codec_probe(
	struct hda_controller *controller,
	unsigned codec)
{
	uint32_t vendor;
	uint32_t nodes;
	uint32_t type;
	uint8_t start;
	unsigned count;
	unsigned index;
	int error;

	/* Addresses every following verb to this codec. */
	controller->codec = codec;
	controller->function_group = 0U;
	kern_memset(controller->widgets, 0, HDA_NID_MAX * sizeof(*controller->widgets));

	/* Skips Intel's display codecs. */
	error = hda_parameter(controller, 0U, HDA_PARAM_VENDOR, &vendor);
	if (error != 0)
		return error;
	if ((vendor >> 16) == 0x8086U)
		return ENODEV;

	/* Finds the audio function group among the root's nodes. */
	error = hda_parameter(controller, 0U, HDA_PARAM_NODE_COUNT, &nodes);
	if (error != 0)
		return error;

	start = (uint8_t)(nodes >> 16);
	count = nodes & 0xffU;
	for (index = 0; index < count; index++) {
		error = hda_parameter(controller, (uint8_t)(start + index), HDA_PARAM_FUNCTION_TYPE, &type);
		if (error != 0)
			return error;

		if ((type & 0xffU) == 1U) {
			controller->function_group = (uint8_t)(start + index);
			break;
		}
	}

	if (controller->function_group == 0U)
		return ENODEV;

	/* Powers the function group up. */
	error = hda_verb(controller, controller->function_group, HDA_VERB_SET_POWER_STATE, 0U, NULL);
	if (error != 0)
		return error;

	/* Reads every widget of the function group. */
	error = hda_parameter(controller, controller->function_group, HDA_PARAM_NODE_COUNT, &nodes);
	if (error != 0)
		return error;

	error = hda_widgets_read(controller, (uint8_t)(nodes >> 16), nodes & 0xffU);
	if (error != 0)
		return error;

	/* Chooses the output pins; a codec without one is not used. */
	error = hda_outputs_find(controller);
	if (error != 0)
		return error;

	/* Chooses one input path if the codec has one. */
	(void)hda_input_find(controller);

	/* Lists the formats the converters take. */
	error = hda_formats_build(controller);
	if (error != 0)
		return error;

	/* Programs every chosen path. */
	for (index = 0; index < controller->output_count; index++) {
		error = hda_path_program(controller, &controller->outputs[index], 0);
		if (error != 0)
			return error;
	}

	if (controller->has_input != 0U) {
		error = hda_path_program(controller, &controller->input, 1);
		if (error != 0)
			controller->has_input = 0U;
	}

	/* Sets the volume to full so the device starts audible. */
	hda_volume_target_find(controller);
	controller->volume.left = 100U;
	controller->volume.right = 100U;
	controller->volume.muted = 0U;
	if (controller->volume_steps != 0U) {
		error = hda_volume_write(controller, &controller->volume);
		if (error != 0)
			return error;
	}

	/* Succeeded: the codec and its paths are ready. */
	return 0;
}

/* Reads the capabilities and connections of every widget in a range. */
static int
hda_widgets_read(
	struct hda_controller *controller,
	uint8_t start,
	unsigned count)
{
	struct hda_widget *widget;
	uint32_t function_amp_out;
	uint32_t function_amp_in;
	unsigned index;
	uint8_t nid;
	int error;

	/* Reads the function group's default amplifier capabilities. */
	error = hda_parameter(controller, controller->function_group, HDA_PARAM_AMP_OUT_CAPS, &function_amp_out);
	if (error != 0)
		return error;

	error = hda_parameter(controller, controller->function_group, HDA_PARAM_AMP_IN_CAPS, &function_amp_in);
	if (error != 0)
		return error;

	/* Reads each widget the table can hold. */
	for (index = 0; index < count; index++) {
		nid = (uint8_t)(start + index);
		if (nid >= HDA_NID_MAX)
			break;

		widget = &controller->widgets[nid];
		error = hda_parameter(controller, nid, HDA_PARAM_WIDGET_CAPS, &widget->caps);
		if (error != 0)
			return error;

		widget->type = (widget->caps >> 20) & 0xfU;
		widget->present = 1U;

		/* Amplifier capabilities come from the widget only when it overrides them. */
		widget->amp_out_caps = function_amp_out;
		widget->amp_in_caps = function_amp_in;
		if ((widget->caps & HDA_WIDGET_AMP_OVERRIDE) != 0U) {
			error = hda_parameter(controller, nid, HDA_PARAM_AMP_OUT_CAPS, &widget->amp_out_caps);
			if (error != 0)
				return error;

			error = hda_parameter(controller, nid, HDA_PARAM_AMP_IN_CAPS, &widget->amp_in_caps);
			if (error != 0)
				return error;
		}

		/* A pin also has its own capabilities and its configuration default. */
		if (widget->type == HDA_WIDGET_PIN) {
			error = hda_parameter(controller, nid, HDA_PARAM_PIN_CAPS, &widget->pin_caps);
			if (error != 0)
				return error;

			error = hda_verb(controller, nid, HDA_VERB_GET_CONFIG_DEFAULT, 0U, &widget->config);
			if (error != 0)
				return error;
		}

		/* Reads the connection list of a widget that has one. */
		if ((widget->caps & HDA_WIDGET_CONNECTIONS) != 0U) {
			error = hda_connections_read(controller, nid, widget);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the widget table describes the function group. */
	return 0;
}

/*
 * Reads a widget's connection list, expanding ranges.
 *
 * A short-form response holds four 8-bit entries and a long-form one two
 * 16-bit entries; an entry with its top bit set ends a range that starts
 * at the previous entry.  Entries past the table's capacity are dropped.
 */
static int
hda_connections_read(
	struct hda_controller *controller,
	uint8_t nid,
	struct hda_widget *widget)
{
	uint32_t length;
	uint32_t response;
	uint32_t entry;
	uint32_t previous;
	uint32_t value;
	unsigned total;
	unsigned per_response;
	unsigned entry_bits;
	unsigned index;
	unsigned slot;
	int error;

	/* Reads how many entries there are and their width. */
	error = hda_parameter(controller, nid, HDA_PARAM_CONNECTION_LENGTH, &length);
	if (error != 0)
		return error;

	total = length & 0x7fU;
	per_response = 4U;
	entry_bits = 8U;
	if ((length & 0x80U) != 0U) {
		per_response = 2U;
		entry_bits = 16U;
	}

	/* Reads the entries a response at a time. */
	previous = 0U;
	response = 0U;
	for (index = 0; index < total; index++) {
		slot = index % per_response;
		if (slot == 0U) {
			error = hda_verb(controller, nid, HDA_VERB_GET_CONNECTION_LIST, index, &response);
			if (error != 0)
				return error;
		}

		entry = (response >> (slot * entry_bits)) & ((1U << entry_bits) - 1U);
		value = entry & ((1U << (entry_bits - 1U)) - 1U);

		/* A range entry adds every node after the previous one up to this one. */
		if ((entry & (1U << (entry_bits - 1U))) != 0U && previous != 0U) {
			for (previous++; previous < value; previous++) {
				if (widget->connection_count >= HDA_CONNECTIONS_MAX)
					break;
				widget->connections[widget->connection_count++] = (uint8_t)previous;
			}
		}

		if (widget->connection_count < HDA_CONNECTIONS_MAX)
			widget->connections[widget->connection_count++] = (uint8_t)value;
		previous = value;
	}

	/* Succeeded: the widget's connections are known. */
	return 0;
}

/* Reports a pin's port connectivity from its configuration default. */
static unsigned
hda_connectivity(
	const struct hda_widget *widget)
{
	/* The top two bits: jack, none, internal or both. */
	return (widget->config >> HDA_CONFIG_CONNECTIVITY_SHIFT) & 3U;
}

/*
 * Reports whether a widget ends a search.
 *
 * An output search ends at an output converter; an input search ends at
 * an input-capable pin that is connected, a jack in the first pass and
 * any connected pin in the second.
 */
static int
hda_path_accepts(
	const struct hda_widget *widget,
	unsigned target,
	unsigned pass)
{
	unsigned connectivity;

	/* An output search wants a DAC. */
	if (target == HDA_WIDGET_OUTPUT) {
		if (widget->type != HDA_WIDGET_OUTPUT)
			return 0;

		return 1;
	}

	/* An input search wants a connected input pin. */
	if (widget->type != HDA_WIDGET_PIN)
		return 0;
	if ((widget->pin_caps & HDA_PIN_CAP_INPUT) == 0U)
		return 0;

	connectivity = hda_connectivity(widget);
	if (connectivity == HDA_CONNECTIVITY_NONE)
		return 0;
	if (pass == 0U && connectivity == HDA_CONNECTIVITY_INTERNAL)
		return 0;

	/* Succeeded: the pin ends the input search. */
	return 1;
}

/*
 * Searches depth first from a node through mixers and selectors.
 *
 * On success path holds the route from nid to the accepting widget.
 */
static int
hda_path_search(
	struct hda_controller *controller,
	uint8_t nid,
	unsigned target,
	unsigned pass,
	struct hda_path *path)
{
	struct hda_widget *widget;
	struct hda_widget *next;
	unsigned depth;
	unsigned index;
	int found;

	/* Refuses an unknown node or a path that is already too long. */
	if (nid >= HDA_NID_MAX)
		return 0;

	widget = &controller->widgets[nid];
	if (widget->present == 0U)
		return 0;
	if (path->length >= HDA_PATH_MAX)
		return 0;

	/* Appends the node. */
	depth = path->length;
	path->nids[depth] = nid;
	path->selects[depth] = 0U;
	path->length++;

	/* Stops at an accepting widget other than the start. */
	if (depth > 0U) {
		found = hda_path_accepts(widget, target, pass);
		if (found != 0)
			return 1;
	}

	/* Continues through the connections of the start or of a mixer or selector. */
	if (depth == 0U ||
	    widget->type == HDA_WIDGET_MIXER ||
	    widget->type == HDA_WIDGET_SELECTOR) {
		for (index = 0; index < widget->connection_count; index++) {
			next = &controller->widgets[widget->connections[index]];
			if (next->present == 0U)
				continue;

			found = hda_path_search(controller, widget->connections[index], target, pass, path);
			if (found != 0) {
				path->selects[depth] = (uint8_t)index;
				return 1;
			}
		}
	}

	/* Takes the node back off the path. */
	path->length--;
	return 0;
}

/*
 * Chooses the output pins.
 *
 * The first pin to reach a DAC, internal pins before jacks, picks the
 * DAC; every other connected output pin that reaches the same DAC is
 * enabled too, since jacks are not sensed.
 */
static int
hda_outputs_find(
	struct hda_controller *controller)
{
	struct hda_widget *widget;
	struct hda_path path;
	unsigned connectivity;
	unsigned pass;
	unsigned nid;
	uint8_t converter;
	int found;

	/* Finds the first pin that reaches a DAC, internal pins first. */
	converter = 0U;
	controller->output_count = 0U;
	for (pass = 0; pass < 2U; pass++) {
		for (nid = 1; nid < HDA_NID_MAX; nid++) {
			widget = &controller->widgets[nid];
			if (widget->present == 0U || widget->type != HDA_WIDGET_PIN)
				continue;
			if ((widget->pin_caps & HDA_PIN_CAP_OUTPUT) == 0U)
				continue;

			connectivity = hda_connectivity(widget);
			if (connectivity == HDA_CONNECTIVITY_NONE)
				continue;
			if (pass == 0U && connectivity != HDA_CONNECTIVITY_INTERNAL)
				continue;
			if (pass == 1U && connectivity == HDA_CONNECTIVITY_INTERNAL)
				continue;

			/* Keeps the pin if it reaches the chosen DAC, or picks its DAC first. */
			kern_memset(&path, 0, sizeof(path));
			found = hda_path_search(controller, (uint8_t)nid, HDA_WIDGET_OUTPUT, 0U, &path);
			if (found == 0)
				continue;
			if (converter == 0U)
				converter = path.nids[path.length - 1U];
			if (path.nids[path.length - 1U] != converter)
				continue;
			if (controller->output_count >= HDA_OUTPUT_PINS_MAX)
				continue;

			controller->outputs[controller->output_count++] = path;
		}
	}

	/* Reports a codec with no output path. */
	if (controller->output_count == 0U)
		return ENODEV;

	/* Succeeded: the output pins and their DAC are chosen. */
	return 0;
}

/* Chooses the first input converter that reaches a connected input pin, jacks first. */
static int
hda_input_find(
	struct hda_controller *controller)
{
	struct hda_widget *widget;
	unsigned pass;
	unsigned nid;
	int found;

	/* Tries every ADC, first for a jack and then for any connected pin. */
	controller->has_input = 0U;
	for (pass = 0; pass < 2U; pass++) {
		for (nid = 1; nid < HDA_NID_MAX; nid++) {
			widget = &controller->widgets[nid];
			if (widget->present == 0U || widget->type != HDA_WIDGET_INPUT)
				continue;

			kern_memset(&controller->input, 0, sizeof(controller->input));
			found = hda_path_search(controller, (uint8_t)nid, HDA_WIDGET_PIN, pass, &controller->input);
			if (found != 0) {
				controller->has_input = 1U;
				return 0;
			}
		}
	}

	/* Reports a codec without a usable input. */
	return ENODEV;
}

/*
 * Programs one path: power, connection selects, amplifiers and the pin.
 *
 * Every amplifier on the path is unmuted at 0 dB; the volume target is
 * set separately afterwards.
 */
static int
hda_path_program(
	struct hda_controller *controller,
	const struct hda_path *path,
	int capture)
{
	struct hda_widget *widget;
	struct hda_widget *next;
	uint32_t gain;
	uint32_t control;
	unsigned index;
	uint8_t nid;
	int error;

	/* Walks every widget of the path. */
	for (index = 0; index < path->length; index++) {
		nid = path->nids[index];
		widget = &controller->widgets[nid];

		/* Powers the widget up. */
		error = hda_verb(controller, nid, HDA_VERB_SET_POWER_STATE, 0U, NULL);
		if (error != 0)
			return error;

		/* A widget with a choice of inputs takes the one the path goes through. */
		if (index + 1U < path->length && widget->connection_count > 1U &&
		    widget->type != HDA_WIDGET_MIXER) {
			error = hda_verb(controller, nid, HDA_VERB_SET_CONNECTION_SELECT, path->selects[index], NULL);
			if (error != 0)
				return error;
		}

		/* Unmutes the output amplifier at 0 dB. */
		if ((widget->caps & HDA_WIDGET_OUT_AMP) != 0U) {
			gain = widget->amp_out_caps & 0x7fU;
			error = hda_verb4(controller, nid, HDA_VERB4_SET_AMP,
			    HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | gain);
			if (error != 0)
				return error;
		}

		/* Unmutes the input amplifier of the connection the path uses. */
		if ((widget->caps & HDA_WIDGET_IN_AMP) != 0U && index + 1U < path->length) {
			gain = widget->amp_in_caps & 0x7fU;
			error = hda_verb4(controller, nid, HDA_VERB4_SET_AMP,
			    HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
			    ((uint32_t)path->selects[index] << HDA_AMP_SET_INDEX_SHIFT) | gain);
			if (error != 0)
				return error;
		}

		/* An input converter's own amplifier is on its input side. */
		if (capture != 0 && index == 0U && (widget->caps & HDA_WIDGET_IN_AMP) != 0U &&
		    path->length == 1U) {
			gain = widget->amp_in_caps & 0x7fU;
			error = hda_verb4(controller, nid, HDA_VERB4_SET_AMP,
			    HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | gain);
			if (error != 0)
				return error;
		}

		/* Enables the pin in the path's direction. */
		if (widget->type == HDA_WIDGET_PIN) {
			control = HDA_PIN_CONTROL_INPUT;
			if (capture == 0) {
				control = HDA_PIN_CONTROL_OUTPUT;
				if ((widget->pin_caps & HDA_PIN_CAP_HEADPHONE) != 0U)
					control |= HDA_PIN_CONTROL_HEADPHONE;
			}

			error = hda_verb(controller, nid, HDA_VERB_SET_PIN_CONTROL, control, NULL);
			if (error != 0)
				return error;

			/* Turns an external amplifier on, which internal speakers need. */
			if (capture == 0 && (widget->pin_caps & HDA_PIN_CAP_EAPD) != 0U) {
				error = hda_verb(controller, nid, HDA_VERB_SET_EAPD, HDA_EAPD_ENABLE, NULL);
				if (error != 0)
					return error;
			}
		}

		/* Checks that the next hop exists in the table. */
		if (index + 1U < path->length) {
			next = &controller->widgets[path->nids[index + 1U]];
			if (next->present == 0U)
				return EINVAL;
		}
	}

	/* Succeeded: the path carries sound. */
	return 0;
}

/* Picks the output amplifier nearest the DAC that has gain steps. */
static void
hda_volume_target_find(
	struct hda_controller *controller)
{
	const struct hda_path *path;
	struct hda_widget *widget;
	unsigned index;
	unsigned steps;

	/* Walks the first output path from the DAC back to the pin. */
	controller->volume_steps = 0U;
	path = &controller->outputs[0];
	for (index = path->length; index > 0U; index--) {
		widget = &controller->widgets[path->nids[index - 1U]];
		if ((widget->caps & HDA_WIDGET_OUT_AMP) == 0U)
			continue;

		steps = (widget->amp_out_caps >> 8) & 0x7fU;
		if (steps == 0U)
			continue;

		controller->volume_nid = path->nids[index - 1U];
		controller->volume_steps = steps;
		if ((widget->amp_out_caps & HDA_AMP_CAP_MUTE) != 0U)
			controller->volume_mute = 1U;
		return;
	}
}

/* Writes a volume to the target amplifier, left and right separately. */
static int
hda_volume_write(
	struct hda_controller *controller,
	const struct audio_volume *volume)
{
	uint32_t left;
	uint32_t right;
	uint32_t mute;
	int error;

	/* Maps 0..100 onto 0..steps. */
	left = volume->left * controller->volume_steps / 100U;
	right = volume->right * controller->volume_steps / 100U;

	/* Mutes with the mute bit, or with the lowest gain when there is none. */
	mute = 0U;
	if (volume->muted != 0U) {
		if (controller->volume_mute != 0U) {
			mute = HDA_AMP_SET_MUTE;
		} else {
			left = 0U;
			right = 0U;
		}
	}

	/* Writes the left channel. */
	error = hda_verb4(controller, controller->volume_nid, HDA_VERB4_SET_AMP,
	    HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | mute | left);
	if (error != 0)
		return error;

	/* Writes the right channel. */
	error = hda_verb4(controller, controller->volume_nid, HDA_VERB4_SET_AMP,
	    HDA_AMP_SET_OUTPUT | HDA_AMP_SET_RIGHT | mute | right);
	if (error != 0)
		return error;

	/* Succeeded: the amplifier holds the volume. */
	return 0;
}

/*
 * Lists the formats the DAC takes, those the ADC also takes first.
 *
 * The audio framework's format is shared by both directions, so a format
 * only the DAC takes is still offered but refused for capture.
 */
static int
hda_formats_build(
	struct hda_controller *controller)
{
	static const struct hda_format candidates[HDA_FORMATS_MAX] = {
		{ { KERN_AUDIO_FORMAT_S16_LE, 2U, 48000U, 0U }, 0x0011U, 0U },
		{ { KERN_AUDIO_FORMAT_S16_LE, 2U, 44100U, 0U }, 0x4011U, 0U },
		{ { KERN_AUDIO_FORMAT_S32_LE, 2U, 48000U, 0U }, 0x0041U, 0U },
	};
	static const uint32_t needs[HDA_FORMATS_MAX] = {
		HDA_PCM_RATE_48000 | HDA_PCM_BITS_16,
		HDA_PCM_RATE_44100 | HDA_PCM_BITS_16,
		HDA_PCM_RATE_48000 | HDA_PCM_BITS_32,
	};
	struct hda_format taken[HDA_FORMATS_MAX];
	uint32_t output_pcm;
	uint32_t input_pcm;
	unsigned count;
	unsigned index;
	unsigned pass;
	uint8_t converter;
	int error;

	/* Reads the DAC's PCM capabilities, or the function group's. */
	converter = controller->outputs[0].nids[controller->outputs[0].length - 1U];
	if ((controller->widgets[converter].caps & HDA_WIDGET_STEREO) == 0U)
		return ENODEV;

	error = hda_parameter(controller, converter, HDA_PARAM_PCM, &output_pcm);
	if (error != 0)
		return error;
	if (output_pcm == 0U) {
		error = hda_parameter(controller, controller->function_group, HDA_PARAM_PCM, &output_pcm);
		if (error != 0)
			return error;
	}

	/* Reads the ADC's PCM capabilities when there is an input. */
	input_pcm = 0U;
	if (controller->has_input != 0U) {
		error = hda_parameter(controller, controller->input.nids[0], HDA_PARAM_PCM, &input_pcm);
		if (error != 0)
			return error;
		if (input_pcm == 0U) {
			error = hda_parameter(controller, controller->function_group, HDA_PARAM_PCM, &input_pcm);
			if (error != 0)
				return error;
		}
	}

	/* Takes the formats both converters accept first, then those only the DAC does. */
	count = 0U;
	for (pass = 0; pass < 2U; pass++) {
		for (index = 0; index < HDA_FORMATS_MAX; index++) {
			if ((output_pcm & needs[index]) != needs[index])
				continue;
			if (pass == 0U && (input_pcm & needs[index]) != needs[index])
				continue;
			if (pass == 1U && (input_pcm & needs[index]) == needs[index])
				continue;

			taken[count] = candidates[index];
			if (pass == 0U)
				taken[count].capture = 1U;
			count++;
		}
	}

	if (count == 0U)
		return ENODEV;

	/* Publishes the list the framework borrows. */
	for (index = 0; index < count; index++) {
		controller->formats[index] = taken[index];
		controller->format_list[index] = taken[index].format;
	}

	controller->format_count = count;

	/* Succeeded: the framework's default is the first entry. */
	return 0;
}

/* Chooses, resets and gives a descriptor list to each stream the codec can use. */
static int
hda_streams_start(
	struct hda_controller *controller)
{
	struct hda_stream *stream;
	unsigned index;
	int error;

	/* Playback uses the first output descriptor, which follows the input ones. */
	if (controller->output_streams != 0U) {
		stream = &controller->streams[HDA_PLAYBACK];
		stream->index = controller->input_streams;
		stream->converter = controller->outputs[0].nids[controller->outputs[0].length - 1U];
		stream->present = 1U;
	}

	/* Capture uses the first input descriptor when the codec has an input path. */
	if (controller->input_streams != 0U && controller->has_input != 0U) {
		stream = &controller->streams[HDA_CAPTURE];
		stream->index = 0U;
		stream->converter = controller->input.nids[0];
		stream->present = 1U;
	}

	if (controller->streams[HDA_PLAYBACK].present == 0U)
		return ENODEV;

	/* Resets each stream and allocates its descriptor list. */
	for (index = 0; index < 2U; index++) {
		stream = &controller->streams[index];
		if (stream->present == 0U)
			continue;

		error = hda_stream_reset(controller, stream);
		if (error != 0)
			return error;

		error = drv_dma_alloc_coherent(controller->dma, HDA_BDL_ENTRIES * 16U, 128U, &stream->bdl);
		if (error != 0)
			return error;

		kern_memset(stream->bdl.address, 0, HDA_BDL_ENTRIES * 16U);
	}

	/* Succeeded: both streams are idle and have descriptor lists. */
	return 0;
}

/*
 * Stops a stream and puts its descriptor through reset.
 *
 * Reset also stops a stream whose RUN bit would not clear.
 */
static int
hda_stream_reset(
	struct hda_controller *controller,
	const struct hda_stream *stream)
{
	unsigned base;
	uint32_t control;
	int error;

	/* Clears RUN and waits for the engine to stop. */
	base = HDA_SD_BASE + stream->index * HDA_SD_SIZE;
	control = hda_read32(controller, base + HDA_SD_CTL) & 0x00ffffffU;
	hda_write32(controller, base + HDA_SD_CTL, control & ~HDA_SD_CTL_RUN);
	(void)hda_wait32(controller, base + HDA_SD_CTL, HDA_SD_CTL_RUN, 0U, HDA_STREAM_MS);

	/* Enters stream reset and waits until the descriptor is in it. */
	hda_write32(controller, base + HDA_SD_CTL, HDA_SD_CTL_SRST);
	error = hda_wait32(controller, base + HDA_SD_CTL, HDA_SD_CTL_SRST, HDA_SD_CTL_SRST, HDA_STREAM_MS);
	if (error != 0)
		return error;

	/* Leaves stream reset and waits until the descriptor is out. */
	hda_write32(controller, base + HDA_SD_CTL, 0U);
	error = hda_wait32(controller, base + HDA_SD_CTL, HDA_SD_CTL_SRST, 0U, HDA_STREAM_MS);
	if (error != 0)
		return error;

	/* Clears any status the stream had raised. */
	hda_write8(controller, base + HDA_SD_STS, HDA_SD_STS_ALL);

	/* Succeeded: the stream is idle. */
	return 0;
}

/* Establishes the controller interrupt and enables it for the two streams. */
static int
hda_interrupt_start(
	struct hda_controller *controller)
{
	uint32_t enable;
	unsigned count;
	int error;

	/* Allocates one MSI, or the shared INTx line. */
	count = 0U;
	error = drv_pci_device_allocate_irqs(controller->pci,
	    DRV_PCI_IRQ_ALLOW_MSI | DRV_PCI_IRQ_ALLOW_INTX,
	    1U,
	    1U,
	    &controller->irq,
	    &count);
	if (error != 0)
		return error;

	controller->irq_count = count;
	if (count != 1U)
		return EIO;

	/* Installs the handler before the controller can raise anything. */
	error = drv_pci_device_establish_irq(controller->pci,
	    &controller->irq,
	    hda_interrupt,
	    controller,
	    "hda",
	    &controller->irq_cookie);
	if (error != 0)
		return error;

	/* Enables the global interrupt and those of the streams in use. */
	enable = HDA_INTCTL_GIE;
	if (controller->streams[HDA_PLAYBACK].present != 0U)
		enable |= 1U << controller->streams[HDA_PLAYBACK].index;
	if (controller->streams[HDA_CAPTURE].present != 0U)
		enable |= 1U << controller->streams[HDA_CAPTURE].index;

	hda_write32(controller, HDA_INTCTL, enable);

	/* Succeeded: stream interrupts reach the handler. */
	return 0;
}

/*
 * Handles the controller interrupt.
 *
 * Returns zero when the controller raised nothing, so a shared INTx line
 * can be offered to its other owners.
 */
static int
hda_interrupt(
	void *argument)
{
	struct hda_controller *controller;
	struct hda_stream *stream;
	uint32_t status;
	unsigned base;
	unsigned index;
	uint8_t stream_status;

	/* Reads which sources are raised; all ones means the device is gone. */
	controller = argument;
	status = hda_read32(controller, HDA_INTSTS);
	if (status == 0U || status == 0xffffffffU)
		return 0;

	/* Acknowledges each stream and passes a completed fragment on. */
	for (index = 0; index < 2U; index++) {
		stream = &controller->streams[index];
		if (stream->present == 0U)
			continue;
		if ((status & (1U << stream->index)) == 0U)
			continue;

		base = HDA_SD_BASE + stream->index * HDA_SD_SIZE;
		stream_status = hda_read8(controller, base + HDA_SD_STS);
		hda_write8(controller, base + HDA_SD_STS, stream_status & HDA_SD_STS_ALL);

		/* A FIFO or descriptor error is counted; the framework sees the gap as an underrun. */
		if ((stream_status & (HDA_SD_STS_FIFOE | HDA_SD_STS_DESE)) != 0U)
			controller->stream_errors++;

		if ((stream_status & HDA_SD_STS_BCIS) != 0U && controller->audio != NULL)
			drv_audio_interrupt(controller->audio, (int)index);
	}

	/* Succeeded: the interrupt was this controller's. */
	return 1;
}

/*
 * Binds the framework's ring to a stream and the stream to its converter.
 *
 * Each fragment is one descriptor with its completion interrupt, which is
 * the framework's one interrupt per fragment.
 */
static int
hda_ops_prepare(
	void *private_data,
	int capture,
	const struct audio_format *format,
	const struct drv_dma_buffer *buffer,
	uint32_t fragment_bytes,
	uint32_t fragment_count)
{
	struct hda_controller *controller;
	struct hda_stream *stream;
	const struct hda_format *chosen;
	volatile uint32_t *entry;
	uint64_t address;
	unsigned base;
	unsigned index;
	int error;

	/* Finds the stream and the hardware code of the format. */
	controller = private_data;
	stream = &controller->streams[capture];
	if (stream->present == 0U)
		return ENODEV;

	chosen = NULL;
	for (index = 0; index < controller->format_count; index++) {
		if (controller->formats[index].format.format == format->format &&
		    controller->formats[index].format.channels == format->channels &&
		    controller->formats[index].format.rate == format->rate) {
			chosen = &controller->formats[index];
			break;
		}
	}

	if (chosen == NULL)
		return EINVAL;
	if (capture != 0 && chosen->capture == 0U)
		return EINVAL;

	/* The controller needs 128-byte aligned buffers and at most one list's entries. */
	if ((buffer->device_address & 127U) != 0U || (fragment_bytes & 127U) != 0U)
		return EINVAL;
	if (fragment_count < 2U || fragment_count > HDA_BDL_ENTRIES)
		return EINVAL;
	if (controller->dma64 == 0U && ((buffer->device_address + buffer->size) >> 32) != 0U)
		return EINVAL;

	/* Stops and resets the descriptor before reprogramming it. */
	error = hda_stream_reset(controller, stream);
	if (error != 0)
		return error;

	/* Fills one descriptor per fragment, each raising its completion interrupt. */
	for (index = 0; index < fragment_count; index++) {
		entry = (volatile uint32_t *)((uint8_t *)stream->bdl.address + index * 16U);
		address = buffer->device_address + (uint64_t)index * fragment_bytes;
		entry[0] = (uint32_t)address;
		entry[1] = (uint32_t)(address >> 32);
		entry[2] = fragment_bytes;
		entry[3] = 1U;
	}

	kern_io_write_barrier();

	/* Programs the ring length, last descriptor, format and list address. */
	base = HDA_SD_BASE + stream->index * HDA_SD_SIZE;
	hda_write32(controller, base + HDA_SD_CBL, fragment_bytes * fragment_count);
	hda_write16(controller, base + HDA_SD_LVI, (uint16_t)(fragment_count - 1U));
	hda_write16(controller, base + HDA_SD_FMT, chosen->code);
	hda_write32(controller, base + HDA_SD_BDPL, (uint32_t)stream->bdl.device_address);
	hda_write32(controller, base + HDA_SD_BDPU, (uint32_t)(stream->bdl.device_address >> 32));
	hda_write32(controller, base + HDA_SD_CTL,
	    (HDA_STREAM_TAG << HDA_SD_CTL_TAG_SHIFT) |
	    HDA_SD_CTL_IOCE | HDA_SD_CTL_FEIE | HDA_SD_CTL_DEIE);

	/* Binds the converter to the stream tag and gives it the same format. */
	error = hda_verb(controller, stream->converter, HDA_VERB_SET_CHANNEL_STREAM, HDA_STREAM_TAG << 4, NULL);
	if (error != 0)
		return error;

	error = hda_verb4(controller, stream->converter, HDA_VERB4_SET_FORMAT, chosen->code);
	if (error != 0)
		return error;

	/* Succeeded: the stream is ready to run. */
	return 0;
}

/* Starts a prepared stream. */
static int
hda_ops_start(
	void *private_data,
	int capture)
{
	struct hda_controller *controller;
	struct hda_stream *stream;
	uint32_t control;
	unsigned base;

	/* Sets RUN on the descriptor. */
	controller = private_data;
	stream = &controller->streams[capture];
	if (stream->present == 0U)
		return ENODEV;

	base = HDA_SD_BASE + stream->index * HDA_SD_SIZE;
	control = hda_read32(controller, base + HDA_SD_CTL) & 0x00ffffffU;
	hda_write32(controller, base + HDA_SD_CTL, control | HDA_SD_CTL_RUN);

	/* Succeeded: the stream walks the ring. */
	return 0;
}

/*
 * Stops a stream; on return the hardware no longer touches the ring.
 *
 * A stream whose RUN bit does not clear is reset, which also stops it.
 */
static void
hda_ops_stop(
	void *private_data,
	int capture)
{
	struct hda_controller *controller;
	struct hda_stream *stream;
	uint32_t control;
	unsigned base;
	int error;

	/* Clears RUN and waits for the engine to stop. */
	controller = private_data;
	stream = &controller->streams[capture];
	if (stream->present == 0U)
		return;

	base = HDA_SD_BASE + stream->index * HDA_SD_SIZE;
	control = hda_read32(controller, base + HDA_SD_CTL) & 0x00ffffffU;
	hda_write32(controller, base + HDA_SD_CTL, control & ~HDA_SD_CTL_RUN);
	error = hda_wait32(controller, base + HDA_SD_CTL, HDA_SD_CTL_RUN, 0U, HDA_STREAM_MS);
	if (error != 0)
		(void)hda_stream_reset(controller, stream);

	/* Clears the status the last fragments raised. */
	hda_write8(controller, base + HDA_SD_STS, HDA_SD_STS_ALL);
}

/* Reports the stream's position in the ring from its link position register. */
static uint32_t
hda_ops_position(
	void *private_data,
	int capture)
{
	struct hda_controller *controller;
	struct hda_stream *stream;
	unsigned base;

	/* Reads LPIB; one MMIO read, since the framework's spinlock is held. */
	controller = private_data;
	stream = &controller->streams[capture];
	base = HDA_SD_BASE + stream->index * HDA_SD_SIZE;
	return hda_read32(controller, base + HDA_SD_LPIB);
}

/* Reports the last volume written. */
static int
hda_ops_get_volume(
	void *private_data,
	struct audio_volume *volume)
{
	struct hda_controller *controller;

	/* Copies the cached volume. */
	controller = private_data;
	*volume = controller->volume;

	/* Succeeded: volume holds the current setting. */
	return 0;
}

/* Writes a new volume to the target amplifier. */
static int
hda_ops_set_volume(
	void *private_data,
	const struct audio_volume *volume)
{
	struct hda_controller *controller;
	int error;

	/* Writes the amplifier, then remembers the value. */
	controller = private_data;
	error = hda_volume_write(controller, volume);
	if (error != 0)
		return error;

	controller->volume = *volume;

	/* Succeeded: the amplifier holds the new volume. */
	return 0;
}
