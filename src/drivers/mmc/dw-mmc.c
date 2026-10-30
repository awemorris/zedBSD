/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The DesignWare Mobile Storage Host controller with an SD memory card.
 *
 * The driver moves data through the controller's FIFO by programmed I/O
 * and polls the controller's raw interrupt status; it takes no interrupt
 * and uses no DMA.  The FIFO of a run is served with preemption off: a
 * FIFO left full stops the card clock in the middle of a block, and the
 * reads that met it failed their data CRC.  It brings one SD memory card up from the idle state
 * (identification at 400 kHz, the bus widened to four lines, high speed
 * when the card and the slot allow it) and publishes it as one disk.  A
 * request is moved with one single-block or multiple-block command per run
 * of up to DW_MMC_RUN_BLOCKS blocks, the controller ending a multiple-block
 * run with its automatic stop.  A run that fails is stopped, the card is
 * brought back to the transfer state, and the run is tried again.  A run
 * that fails at high speed with an error of the data lines' signal (a bad
 * CRC, a missing start or end bit) first moves the controller's sample
 * phase to the middle of the phases that read the run, when the glue can
 * set the phase, and then the clock down to the default speed.
 *
 * The board's glue switches the controller's clocks, reset and pins on and
 * hands over the mapped registers (struct drv_dw_mmc_config).
 */

#include <drivers/mmc/dw-mmc.h>

#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/disk.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

/*
 * The controller's registers.
 *
 * Values referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (the SDMMC_* register
 * offsets).
 */
#define DW_MMC_CONTROL			0x000U
#define DW_MMC_POWER_ENABLE		0x004U
#define DW_MMC_CLOCK_DIVIDER		0x008U
#define DW_MMC_CLOCK_SOURCE		0x00cU
#define DW_MMC_CLOCK_ENABLE		0x010U
#define DW_MMC_TIMEOUT			0x014U
#define DW_MMC_CARD_TYPE		0x018U
#define DW_MMC_BLOCK_SIZE		0x01cU
#define DW_MMC_BYTE_COUNT		0x020U
#define DW_MMC_INTERRUPT_MASK		0x024U
#define DW_MMC_ARGUMENT			0x028U
#define DW_MMC_COMMAND			0x02cU
#define DW_MMC_RESPONSE0		0x030U
#define DW_MMC_RAW_STATUS		0x044U
#define DW_MMC_STATUS			0x048U
#define DW_MMC_FIFO_THRESHOLD		0x04cU
#define DW_MMC_VERSION			0x06cU
#define DW_MMC_HARDWARE			0x070U

/*
 * The bits of the control register: the three self-clearing resets, and
 * the global interrupt enable.
 *
 * Values referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SDMMC_CTRL_*).
 */
#define DW_MMC_CONTROL_RESET		0x00000001U
#define DW_MMC_CONTROL_FIFO_RESET	0x00000002U
#define DW_MMC_CONTROL_DMA_RESET	0x00000004U
#define DW_MMC_CONTROL_INTERRUPTS	0x00000010U
#define DW_MMC_CONTROL_DMA_ENABLE	0x00000020U
#define DW_MMC_CONTROL_INTERNAL_DMA	0x02000000U

/*
 * The bits of the clock enable register and the card type register, for
 * card 0 of the controller.
 *
 * Values referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SDMMC_CLKENA_*,
 * SDMMC_CTYPE_*).
 */
#define DW_MMC_CLOCK_ON			0x00000001U
#define DW_MMC_CLOCK_LOW_POWER		0x00010000U
#define DW_MMC_CARD_TYPE_4BIT		0x00000001U
#define DW_MMC_CARD_TYPE_8BIT		0x00010000U

/*
 * The bits of the command register.
 *
 * Values referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SDMMC_CMD_*).
 */
#define DW_MMC_COMMAND_RESPONSE		0x00000040U
#define DW_MMC_COMMAND_LONG		0x00000080U
#define DW_MMC_COMMAND_CHECK_CRC	0x00000100U
#define DW_MMC_COMMAND_DATA		0x00000200U
#define DW_MMC_COMMAND_WRITE		0x00000400U
#define DW_MMC_COMMAND_AUTO_STOP	0x00001000U
#define DW_MMC_COMMAND_WAIT_DATA	0x00002000U
#define DW_MMC_COMMAND_STOP_ABORT	0x00004000U
#define DW_MMC_COMMAND_INITIALIZE	0x00008000U
#define DW_MMC_COMMAND_CLOCK_ONLY	0x00200000U
#define DW_MMC_COMMAND_HOLD		0x20000000U
#define DW_MMC_COMMAND_START		0x80000000U

/*
 * The bits of the raw interrupt status register.
 *
 * Values referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SDMMC_RINTSTS_*).
 */
#define DW_MMC_STATUS_RESPONSE_ERROR	0x00000002U
#define DW_MMC_STATUS_COMMAND_DONE	0x00000004U
#define DW_MMC_STATUS_DATA_OVER		0x00000008U
#define DW_MMC_STATUS_TX_READY		0x00000010U
#define DW_MMC_STATUS_RX_READY		0x00000020U
#define DW_MMC_STATUS_RESPONSE_CRC	0x00000040U
#define DW_MMC_STATUS_DATA_CRC		0x00000080U
#define DW_MMC_STATUS_RESPONSE_TIMEOUT	0x00000100U
#define DW_MMC_STATUS_DATA_TIMEOUT	0x00000200U
#define DW_MMC_STATUS_HOST_TIMEOUT	0x00000400U
#define DW_MMC_STATUS_FIFO_OVERRUN	0x00000800U
#define DW_MMC_STATUS_HARDWARE_LOCKED	0x00001000U
#define DW_MMC_STATUS_START_BIT		0x00002000U
#define DW_MMC_STATUS_AUTO_STOP_DONE	0x00004000U
#define DW_MMC_STATUS_END_BIT		0x00008000U
#define DW_MMC_STATUS_ALL		0xffffffffU

/*
 * The data errors of the raw interrupt status: a bad CRC, a missing start
 * or end bit, a FIFO overrun and a command the controller refused.
 *
 * Values referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SDMMC_RINTSTS_DATA_ERR).
 */
#define DW_MMC_STATUS_DATA_ERRORS	(DW_MMC_STATUS_END_BIT | DW_MMC_STATUS_START_BIT | \
					 DW_MMC_STATUS_HARDWARE_LOCKED | DW_MMC_STATUS_FIFO_OVERRUN | \
					 DW_MMC_STATUS_DATA_CRC)

/*
 * The data errors that say the controller sampled the data lines at the
 * wrong time: a bad CRC, a missing start bit or a missing end bit.
 */
#define DW_MMC_STATUS_SIGNAL_ERRORS	(DW_MMC_STATUS_END_BIT | DW_MMC_STATUS_START_BIT | \
					 DW_MMC_STATUS_DATA_CRC)

/*
 * The fields of the status, hardware configuration and FIFO threshold
 * registers.
 *
 * Values referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SDMMC_STATUS_*,
 * SDMMC_HCON_*, SDMMC_FIFOTH_*).
 */
#define DW_MMC_STATUS_DATA_BUSY		0x00000200U
#define DW_MMC_STATUS_FIFO_COUNT_SHIFT	17U
#define DW_MMC_STATUS_FIFO_COUNT_MASK	0x1fffU
#define DW_MMC_HARDWARE_WIDTH_SHIFT	7U
#define DW_MMC_HARDWARE_WIDTH_MASK	0x7U
#define DW_MMC_HARDWARE_WIDTH_32	1U
#define DW_MMC_HARDWARE_WIDTH_64	2U
#define DW_MMC_THRESHOLD_BURST_SHIFT	28U
#define DW_MMC_THRESHOLD_RX_SHIFT	16U
#define DW_MMC_THRESHOLD_RX_MASK	0xfffU
#define DW_MMC_THRESHOLD_TX_SHIFT	0U

/*
 * Where the FIFO's data port is: controllers before version 2.40a have it
 * at 0x100, later ones at 0x200.
 *
 * Values referenced: Linux v6.12 drivers/mmc/host/dw_mmc.h (DW_MMC_240A,
 * DATA_OFFSET, DATA_240A_OFFSET, SDMMC_GET_VERID).
 */
#define DW_MMC_VERSION_MASK		0xffffU
#define DW_MMC_VERSION_FIFO_MOVED	0x240aU
#define DW_MMC_FIFO_OLD			0x100U
#define DW_MMC_FIFO_NEW			0x200U

/*
 * The SD commands the driver sends, by number.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/sdmmcreg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (MMC_*, SD_* commands).
 */
#define SD_GO_IDLE			0U
#define SD_ALL_SEND_CID			2U
#define SD_SEND_RELATIVE_ADDRESS	3U
#define SD_SWITCH_FUNCTION		6U
#define SD_SELECT_CARD			7U
#define SD_SEND_INTERFACE_CONDITION	8U
#define SD_SEND_CSD			9U
#define SD_STOP_TRANSMISSION		12U
#define SD_SEND_STATUS			13U
#define SD_SET_BLOCK_LENGTH		16U
#define SD_READ_SINGLE			17U
#define SD_READ_MULTIPLE		18U
#define SD_WRITE_SINGLE			24U
#define SD_WRITE_MULTIPLE		25U
#define SD_APPLICATION			55U
#define SD_APP_SET_BUS_WIDTH		6U
#define SD_APP_SEND_OPERATING		41U
#define SD_APP_SEND_SCR			51U

/*
 * The fields of the operating conditions (OCR), the card status and the
 * bus width argument.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/sdmmcreg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (MMC_OCR_MEM_READY,
 * MMC_OCR_HCS, MMC_OCR_3_2V_3_3V, MMC_OCR_3_3V_3_4V, SD_OCR_VOL_MASK,
 * MMC_R1_READY_FOR_DATA, SD_ARG_BUS_WIDTH_4) and
 * sys/dev/fdt/dwmmc.c (dwmmc_host_ocr, the 3.3 V window).
 */
#define SD_OCR_READY			0x80000000U
#define SD_OCR_HIGH_CAPACITY		0x40000000U
#define SD_OCR_3V3			0x00300000U
#define SD_CARD_READY_FOR_DATA		0x00000100U
#define SD_BUS_WIDTH_4_ARGUMENT		2U

/*
 * The interface condition the driver asks: the supply voltage is in the
 * range bit 8 stands for, and the pattern the card echoes back.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/sdmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_send_if_cond).
 */
#define SD_INTERFACE_VOLTAGE		0x00000100U
#define SD_INTERFACE_PATTERN		0x23U
#define SD_INTERFACE_PATTERN_MASK	0xffU

/*
 * The fields of the CSD (bit positions of the 128-bit register, CRC
 * included), the CID and the SCR, and the switch function's group 1.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/sdmmcreg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SD_CSD_*, SD_CID_*, SCR_*,
 * SFUNC_STATUS_GROUP, SD_ACCESS_MODE_SDR25); its MMC_RSP_BITS() counts
 * from a response without the CRC byte, so its positions are 8 below
 * these.
 */
#define SD_CSD_VERSION_BIT		126U
#define SD_CSD_CCC_BIT			84U
#define SD_CSD_CCC_SWITCH		0x400U
#define SD_CSD_READ_LENGTH_BIT		80U
#define SD_CSD_V1_SIZE_BIT		62U
#define SD_CSD_V1_SIZE_WIDTH		12U
#define SD_CSD_V1_MULTIPLIER_BIT	47U
#define SD_CSD_V2_SIZE_BIT		48U
#define SD_CSD_V2_SIZE_WIDTH		22U
#define SD_CID_MANUFACTURER_BIT		120U
#define SD_CID_OEM_BIT			104U
#define SD_CID_PRODUCT_BIT		64U
#define SD_CID_REVISION_BIT		56U
#define SD_CID_SERIAL_BIT		24U
#define SD_SCR_BYTES			8U
#define SD_SCR_SPEC_1_10		1U
#define SD_SCR_BUS_WIDTH_4		0x4U
#define SD_SWITCH_STATUS_BYTES		64U
#define SD_SWITCH_GROUP1_SUPPORT_BYTE	12U
#define SD_SWITCH_HIGH_SPEED		1U
#define SD_SWITCH_SET			0x80000000U
#define SD_SWITCH_KEEP			0x00ffffffU

/* The bytes of one block, the only block length the driver uses. */
#define DW_MMC_BLOCK_BYTES		512U

/*
 * The most blocks one multiple-block command moves: 64 KiB, the largest
 * request the disk layer hands down.
 */
#define DW_MMC_RUN_BLOCKS		128U

/*
 * How many times one run is tried, with a recovery between the tries, and
 * how many times one recovery is tried before the card is taken as gone.
 * A run of several blocks that fails every try is then moved a block at a
 * time, each block with its own tries (BUG-343: a data CRC error now and
 * then must not reach the file system).
 *
 * Value referenced: Linux v6.12 drivers/mmc/core/block.c (MMC_MAX_RETRIES
 * 5, and a failed multiple-block read retried a sector at a time).
 */
#define DW_MMC_RUN_TRIES		5U
#define DW_MMC_RECOVER_TRIES		3U

/* The clocks the card runs at while identified, by default and at high speed. */
#define DW_MMC_IDENTIFY_HZ		400000ULL
#define DW_MMC_DEFAULT_HZ		25000000ULL
#define DW_MMC_HIGH_SPEED_HZ		50000000ULL

/*
 * How long a new sample phase takes to settle, in microseconds.
 *
 * Value referenced: Linux v6.12 drivers/mmc/host/dw_mmc-starfive.c
 * (dw_mci_starfive_set_sample_phase, mdelay(1)).
 */
#define DW_MMC_PHASE_SETTLE_US		1000ULL

/*
 * How far the bus has been adapted to data errors: not at all, the sample
 * phase tuned, or the clock down to the default speed.
 */
enum dw_mmc_bus_step {
	DW_MMC_BUS_AS_SET = 0,
	DW_MMC_BUS_PHASE_TUNED,
	DW_MMC_BUS_SLOWED
};

/* The largest divider of the card clock (the register's eight bits). */
#define DW_MMC_DIVIDER_MAX		255U

/* How long the driver waits for the controller and the card, in microseconds. */
#define DW_MMC_RESET_WAIT_US		500000ULL
#define DW_MMC_COMMAND_WAIT_US		1000000ULL
#define DW_MMC_FIFO_WAIT_US		1000000ULL
#define DW_MMC_BUSY_WAIT_US		2000000ULL
#define DW_MMC_READY_WAIT_US		1000000ULL
#define DW_MMC_READY_RETRY_US		10000ULL

/* The capacity above which a version 2 card is an SDXC card. */
#define DW_MMC_SDHC_MAX_BLOCKS		(32ULL * 1024ULL * 1024ULL * 2ULL)

/* Marks a parameter that the disk layer passes but this driver does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The kinds of response a command expects, which decide how the command
 * register asks for it and how it is read back.
 */
enum dw_mmc_response {
	/* No response (CMD0). */
	DW_MMC_RESPONSE_NONE = 0,

	/* A 48-bit response with its CRC checked (R1, R6, R7). */
	DW_MMC_RESPONSE_SHORT,

	/* A 48-bit response whose CRC field is not a CRC (R3). */
	DW_MMC_RESPONSE_SHORT_NO_CRC,

	/* A 48-bit response after which the card holds the data line busy (R1b). */
	DW_MMC_RESPONSE_BUSY,

	/* A 136-bit response (R2). */
	DW_MMC_RESPONSE_LONG
};

/*
 * One command to the card, with the data it moves, as one call to
 * send_command() takes it and fills its response in.
 */
struct dw_mmc_command {
	uint32_t index;
	uint32_t argument;
	enum dw_mmc_response response_kind;

	/* The data the command moves; length zero moves none. */
	uint8_t *data;
	uint32_t length;
	uint32_t block_size;
	int write;

	/* Whether the controller sends the stop itself after the last block. */
	int auto_stop;

	/* The response, word 0 holding bits 31:0. */
	uint32_t response[4];
};

/*
 * One controller and the card in its slot.
 *
 * A unit is claimed by an attach and kept for the life of the system.
 * Its lock serializes the commands once the disk is published, since the
 * controller has one set of command registers; before that only the
 * attach touches it.
 */
struct dw_mmc_unit {
	/* Whether the unit is claimed by an attach. */
	int in_use;

	/* The mapped registers and the FIFO's data port in them. */
	volatile uint8_t *registers;
	uint32_t fifo_offset;

	/* The FIFO's entries, each fifo_width bytes. */
	uint32_t fifo_width;
	uint32_t fifo_depth;

	/* The card interface clock and the limits of the slot. */
	uint64_t clock_rate;
	uint64_t max_frequency;
	uint32_t slot_bus_width;
	int high_speed_allowed;

	/* The glue's hook for the sample phase, how many phases it has, and its context. */
	void (*set_sample_phase)(void *context, uint32_t phase);
	uint32_t sample_phase_count;
	void *phase_context;

	/* How far data errors have adapted the bus.  Under the lock. */
	enum dw_mmc_bus_step bus_step;

	/* What the attach found; card.rca is the card's address. */
	struct drv_dw_mmc_card card;

	/* The command classes the card's CSD lists. */
	uint32_t command_classes;

	/* The published disk, or NULL before the attach finished. */
	struct disk *disk;

	/* One command at a time. */
	struct mutex lock;

	/*
	 * Set once a failed run could not bring the card back to the
	 * transfer state.  Every later request fails at once instead of
	 * waiting out its deadlines.  Under the lock.
	 */
	int failed;

	/* The raw interrupt status the last failed wait saw, for the failure report. */
	uint32_t last_status;

	/* Failed tries and runs that moved only a block at a time, since the attach, for the log. */
	uint64_t errors;
	uint64_t split_runs;
};

/*
 * The controllers the driver serves.
 *
 * A slot is claimed by drv_dw_mmc_attach() and never released; the disk
 * the slot publishes points back at it through d_data.
 */
static struct dw_mmc_unit dw_mmc_units[DRV_DW_MMC_UNIT_MAX];

static uint32_t read_register(const struct dw_mmc_unit *unit, uint32_t offset);
static void write_register(struct dw_mmc_unit *unit, uint32_t offset, uint32_t value);
static int start_deadline(uint64_t microseconds, uint64_t *deadline);
static int deadline_passed(uint64_t deadline);
static void pause_microseconds(uint64_t microseconds);
static int setup_controller(struct dw_mmc_unit *unit, const struct drv_dw_mmc_config *config);
static int reset_controller(struct dw_mmc_unit *unit, uint32_t resets);
static int update_clock_registers(struct dw_mmc_unit *unit);
static int set_card_clock(struct dw_mmc_unit *unit, uint64_t target);
static int set_bus_width(struct dw_mmc_unit *unit, uint32_t width);
static int wait_data_idle(struct dw_mmc_unit *unit, uint64_t microseconds);
static uint32_t command_value(const struct dw_mmc_command *command);
static int send_command(struct dw_mmc_unit *unit, struct dw_mmc_command *command);
static int send_app_command(struct dw_mmc_unit *unit, struct dw_mmc_command *command);
static int wait_command_done(struct dw_mmc_unit *unit);
static int move_data(struct dw_mmc_unit *unit, struct dw_mmc_command *command);
static int serve_fifo(struct dw_mmc_unit *unit, struct dw_mmc_command *command);
static int wait_raw_status(struct dw_mmc_unit *unit, uint32_t wanted);
static void read_fifo(struct dw_mmc_unit *unit, uint8_t *data, uint32_t bytes);
static void write_fifo(struct dw_mmc_unit *unit, const uint8_t *data, uint32_t bytes);
static void prepare_command(struct dw_mmc_command *command, uint32_t index, uint32_t argument, enum dw_mmc_response response_kind);
static uint32_t response_bits(const uint32_t *response, uint32_t start, uint32_t width);
static int identify_card(struct dw_mmc_unit *unit);
static int start_card(struct dw_mmc_unit *unit);
static int wait_card_powered(struct dw_mmc_unit *unit, uint32_t request, uint32_t *ocr);
static int decode_csd(struct dw_mmc_unit *unit, const uint32_t *csd);
static void decode_cid(struct dw_mmc_unit *unit, const uint32_t *cid);
static int select_card(struct dw_mmc_unit *unit);
static int configure_bus(struct dw_mmc_unit *unit);
static int read_scr(struct dw_mmc_unit *unit, uint32_t *specification, uint32_t *bus_widths);
static int switch_high_speed(struct dw_mmc_unit *unit, int *switched);
static int wait_card_ready(struct dw_mmc_unit *unit);
static int transfer_run(struct dw_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int transfer_run_recovering(struct dw_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int transfer_run_tries(struct dw_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int recover_card(struct dw_mmc_unit *unit);
static int recover_card_tries(struct dw_mmc_unit *unit, const char *during);
static int adapt_bus(struct dw_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int tune_sample_phase(struct dw_mmc_unit *unit, uint64_t block, uint32_t count, uint8_t *data);
static void set_sample_phase(struct dw_mmc_unit *unit, uint32_t phase);
static int publish_disk(struct dw_mmc_unit *unit, unsigned unit_number);
static void log_card(const struct dw_mmc_unit *unit);
static const char *card_kind(const struct drv_dw_mmc_card *card);
static int disk_submit(struct disk *disk, struct bio *bio);
static int disk_control(struct disk *disk, unsigned long request, void *argument);

/* The operations of an SD card's disk. */
static const struct disk_ops dw_mmc_disk_ops = {
	.submit = disk_submit,
	.ioctl = disk_control
};

/*
 * Brings up the card in a controller's slot and publishes it as a disk.
 *
 * Returns 0 with the disk, ENODEV when the slot is empty, EBUSY when every
 * unit is taken or the unit number is in use, or the errno of the step
 * that failed; a failed attach leaves no disk.
 */
int
drv_dw_mmc_attach(
	const struct drv_dw_mmc_config *config,
	struct disk **disk)
{
	struct dw_mmc_unit *unit;
	int present;
	int error;

	/* Refuses a description without registers or a clock. */
	if (config == NULL || disk == NULL)
		return EINVAL;
	if (config->registers == NULL || config->clock_rate == 0)
		return EINVAL;
	if (config->unit >= DRV_DW_MMC_UNIT_MAX)
		return EBUSY;

	/* Claims the unit the glue numbered. */
	unit = &dw_mmc_units[config->unit];
	if (unit->in_use)
		return EBUSY;

	/* Starts without a disk, which every failure below leaves. */
	*disk = NULL;

	/* Asks the slot whether a card is in; a slot without a detect pin has one. */
	present = 1;
	if (config->card_present != NULL) {
		error = config->card_present(config->context, &present);
		if (error != 0)
			return error;
	}

	/* An empty slot has nothing to publish. */
	if (present == 0)
		return ENODEV;

	/* Takes the unit and clears what an earlier failed attach left. */
	unit->in_use = 1;
	unit->disk = NULL;
	unit->failed = 0;
	unit->last_status = 0;
	unit->command_classes = 0;
	unit->bus_step = DW_MMC_BUS_AS_SET;
	memset(&unit->card, 0, sizeof(unit->card));

	/* Resets the controller and powers the card with its clock at 400 kHz. */
	error = setup_controller(unit, config);
	if (error != 0) {
		kern_logf("dw-mmc: controller %u unusable (%d)\n", config->unit, error);
		unit->in_use = 0;
		return error;
	}

	/* Brings the card from the idle state to the transfer state. */
	error = start_card(unit);
	if (error != 0) {
		kern_logf("dw-mmc: card in slot %u unusable (%d, status %x)\n",
			  config->unit,
			  error,
			  unit->last_status);
		unit->in_use = 0;
		return error;
	}

	/* Publishes the card. */
	error = publish_disk(unit, config->unit);
	if (error != 0) {
		kern_logf("dw-mmc: disk for slot %u unavailable (%d)\n", config->unit, error);
		unit->in_use = 0;
		return error;
	}

	/* Writes what the card is to the boot log. */
	log_card(unit);

	/* Succeeded: the card is a disk. */
	*disk = unit->disk;
	return 0;
}

/*
 * Copies what the attach of a unit found in its slot.
 *
 * Returns 0, or ENODEV for a unit without a published card.
 */
int
drv_dw_mmc_card_info(
	unsigned unit_number,
	struct drv_dw_mmc_card *card)
{
	const struct dw_mmc_unit *unit;

	/* Refuses a unit outside the table or without a card. */
	if (card == NULL)
		return EINVAL;
	if (unit_number >= DRV_DW_MMC_UNIT_MAX)
		return ENODEV;
	unit = &dw_mmc_units[unit_number];
	if (!unit->in_use || unit->disk == NULL)
		return ENODEV;

	/* Succeeded: the card's description. */
	*card = unit->card;
	return 0;
}

/* Reads one register of the controller. */
static uint32_t
read_register(
	const struct dw_mmc_unit *unit,
	uint32_t offset)
{
	uint32_t value;

	/* Reads the 32-bit register at the offset. */
	value = kern_mmio_read32(unit->registers + offset);

	/* Reports the register's value. */
	return value;
}

/* Writes one register of the controller. */
static void
write_register(
	struct dw_mmc_unit *unit,
	uint32_t offset,
	uint32_t value)
{
	/* Writes the 32-bit register at the offset. */
	kern_mmio_write32(unit->registers + offset, value);
}

/* Computes the counter value a wait of the given length ends at. */
static int
start_deadline(
	uint64_t microseconds,
	uint64_t *deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* Reads the monotonic counter; without one no wait can end. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted || frequency == 0)
		return ENODEV;

	/* Succeeded: the counter value the wait ends at. */
	*deadline = now + frequency * microseconds / 1000000ULL;
	return 0;
}

/* Reports whether the monotonic counter has passed a deadline. */
static int
deadline_passed(
	uint64_t deadline)
{
	uint64_t frequency;
	uint64_t now;
	bool counted;

	/* A counter that cannot be read ends the wait rather than spinning forever. */
	counted = kern_rtc_read_counter(&now, &frequency);
	if (!counted)
		return 1;

	/* The deadline has passed once the counter reaches it. */
	if (now >= deadline)
		return 1;

	/* The wait goes on. */
	return 0;
}

/* Spins for at least the given number of microseconds. */
static void
pause_microseconds(
	uint64_t microseconds)
{
	uint64_t deadline;
	int error;
	int passed;

	/* Sets the end of the pause; without a counter the pause is skipped. */
	error = start_deadline(microseconds, &deadline);
	if (error != 0)
		return;

	/* Spins until the counter passes the end of the pause. */
	for (;;) {
		passed = deadline_passed(deadline);
		if (passed)
			break;
	}
}

/*
 * Resets the controller, chooses programmed I/O through its FIFO, and
 * powers the card with the bus one line wide and the clock at 400 kHz.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_attach,
 * dwmmc_pio_mode, dwmmc_bus_power) and sys/dev/sdmmc/sdmmc.c
 * (sdmmc_enable).
 */
static int
setup_controller(
	struct dw_mmc_unit *unit,
	const struct drv_dw_mmc_config *config)
{
	uint32_t hardware;
	uint32_t version;
	uint32_t width;
	uint32_t threshold;
	uint32_t control;
	int error;

	/* Records the registers and the slot's limits. */
	unit->registers = config->registers;
	unit->clock_rate = config->clock_rate;
	unit->max_frequency = config->max_frequency;
	unit->slot_bus_width = config->bus_width;
	unit->high_speed_allowed = config->high_speed;

	/* Records the glue's sample phase hook; without a hook there is no phase to tune. */
	unit->set_sample_phase = config->set_sample_phase;
	unit->sample_phase_count = config->sample_phase_count;
	unit->phase_context = config->phase_context;
	if (unit->set_sample_phase == NULL)
		unit->sample_phase_count = 0;
	unit->card.sample_phase = config->sample_phase;

	/* Learns the FIFO's width from the controller; only 32 and 64 bits are served. */
	hardware = read_register(unit, DW_MMC_HARDWARE);
	width = (hardware >> DW_MMC_HARDWARE_WIDTH_SHIFT) & DW_MMC_HARDWARE_WIDTH_MASK;
	if (width == DW_MMC_HARDWARE_WIDTH_32) {
		unit->fifo_width = 4U;
	} else if (width == DW_MMC_HARDWARE_WIDTH_64) {
		unit->fifo_width = 8U;
	} else {
		return ENOTSUP;
	}

	/* Takes the FIFO's depth from the glue, or from the receive threshold's reset value. */
	unit->fifo_depth = config->fifo_depth;
	if (unit->fifo_depth == 0) {
		threshold = read_register(unit, DW_MMC_FIFO_THRESHOLD);
		unit->fifo_depth = ((threshold >> DW_MMC_THRESHOLD_RX_SHIFT) & DW_MMC_THRESHOLD_RX_MASK) + 1U;
	}

	/* Refuses a FIFO too shallow to split into halves. */
	if (unit->fifo_depth < 2U)
		return ENOTSUP;

	/* Finds the FIFO's data port, which moved in version 2.40a. */
	version = read_register(unit, DW_MMC_VERSION);
	unit->fifo_offset = DW_MMC_FIFO_NEW;
	if ((version & DW_MMC_VERSION_MASK) < DW_MMC_VERSION_FIFO_MOVED)
		unit->fifo_offset = DW_MMC_FIFO_OLD;

	/* Records the controller for the boot log. */
	unit->card.controller_version = version;
	unit->card.fifo_width = unit->fifo_width;
	unit->card.fifo_depth = unit->fifo_depth;

	/* Resets the controller, its FIFO and its DMA engine. */
	error = reset_controller(unit,
				 DW_MMC_CONTROL_RESET |
				 DW_MMC_CONTROL_FIFO_RESET |
				 DW_MMC_CONTROL_DMA_RESET);
	if (error != 0)
		return error;

	/*
	 * Masks every interrupt and clears the raw status.  The raw status
	 * still records every event, which is what the driver polls; the
	 * global enable is on, but with every source masked the interrupt
	 * line stays quiet.
	 */
	write_register(unit, DW_MMC_INTERRUPT_MASK, 0);
	write_register(unit, DW_MMC_RAW_STATUS, DW_MMC_STATUS_ALL);
	control = read_register(unit, DW_MMC_CONTROL);
	control |= DW_MMC_CONTROL_INTERRUPTS;

	/* Turns DMA off: the data goes through the FIFO by programmed I/O. */
	control &= ~(DW_MMC_CONTROL_INTERNAL_DMA | DW_MMC_CONTROL_DMA_ENABLE);
	write_register(unit, DW_MMC_CONTROL, control);

	/* Sets the FIFO's thresholds to its halves, and the longest response and data timeouts. */
	threshold = (2U << DW_MMC_THRESHOLD_BURST_SHIFT) |
		    ((unit->fifo_depth / 2U - 1U) << DW_MMC_THRESHOLD_RX_SHIFT) |
		    ((unit->fifo_depth / 2U) << DW_MMC_THRESHOLD_TX_SHIFT);
	write_register(unit, DW_MMC_FIFO_THRESHOLD, threshold);
	write_register(unit, DW_MMC_TIMEOUT, 0xffffffffU);

	/* Starts the bus one line wide, as every card is after its reset. */
	error = set_bus_width(unit, 1U);
	if (error != 0)
		return error;

	/* Switches the card's power on and waits the time the card needs after it. */
	write_register(unit, DW_MMC_POWER_ENABLE, 1U);
	pause_microseconds((uint64_t)config->power_on_delay_ms * 1000ULL);

	/* Runs the card clock at the identification rate. */
	error = set_card_clock(unit, DW_MMC_IDENTIFY_HZ);
	if (error != 0)
		return error;

	/* Succeeded: the controller is ready for the card's identification. */
	return 0;
}

/* Starts the given self-clearing resets of the control register and waits for them. */
static int
reset_controller(
	struct dw_mmc_unit *unit,
	uint32_t resets)
{
	uint64_t deadline;
	uint32_t control;
	int passed;
	int error;

	/* Starts the resets. */
	control = read_register(unit, DW_MMC_CONTROL);
	write_register(unit, DW_MMC_CONTROL, control | resets);

	/* Sets how long the controller has to finish them. */
	error = start_deadline(DW_MMC_RESET_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Waits until the controller clears every reset bit. */
	for (;;) {
		/* The resets are done once their bits read back clear. */
		control = read_register(unit, DW_MMC_CONTROL);
		if ((control & resets) == 0)
			break;

		/* A controller that never finishes is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the resets are done. */
	return 0;
}

/*
 * Has the controller take the clock registers over to the card side.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_bus_clock).
 */
static int
update_clock_registers(
	struct dw_mmc_unit *unit)
{
	uint64_t deadline;
	uint32_t command;
	uint32_t status;
	int passed;
	int error;

	/* Sends the clock-only command, which reaches no card. */
	write_register(unit,
		       DW_MMC_COMMAND,
		       DW_MMC_COMMAND_START |
		       DW_MMC_COMMAND_WAIT_DATA |
		       DW_MMC_COMMAND_CLOCK_ONLY);

	/* Sets how long the controller has to take the command. */
	error = start_deadline(DW_MMC_COMMAND_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Waits until the controller takes the command, which clears its start bit. */
	for (;;) {
		/* The command is taken once its start bit reads back clear. */
		command = read_register(unit, DW_MMC_COMMAND);
		if ((command & DW_MMC_COMMAND_START) == 0)
			break;

		/* A controller that never takes the command is given up on. */
		passed = deadline_passed(deadline);
		if (passed) {
			unit->last_status = read_register(unit, DW_MMC_RAW_STATUS);
			return ETIMEDOUT;
		}
	}

	/* A command the controller could not accept is reported as locked. */
	status = read_register(unit, DW_MMC_RAW_STATUS);
	if ((status & DW_MMC_STATUS_HARDWARE_LOCKED) != 0) {
		unit->last_status = status;
		write_register(unit, DW_MMC_RAW_STATUS, DW_MMC_STATUS_HARDWARE_LOCKED);
		return EIO;
	}

	/* Succeeded: the card side runs on the new clock settings. */
	return 0;
}

/*
 * Runs the card clock at the fastest rate not above the target.
 *
 * The card clock is the interface clock divided by twice the divider, or
 * the interface clock itself with divider zero.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_bus_clock).
 */
static int
set_card_clock(
	struct dw_mmc_unit *unit,
	uint64_t target)
{
	uint64_t divider;
	uint64_t card_clock;
	int error;

	/* Picks the smallest divider that does not run the card faster than the target. */
	divider = 0;
	if (unit->clock_rate > target)
		divider = (unit->clock_rate + 2ULL * target - 1ULL) / (2ULL * target);

	/* Refuses a target the divider cannot reach. */
	if (divider > DW_MMC_DIVIDER_MAX)
		return ERANGE;

	/* The rate the card will run at. */
	card_clock = unit->clock_rate;
	if (divider != 0)
		card_clock = unit->clock_rate / (2ULL * divider);

	/* Stops the card clock and selects divider 0 as its source. */
	write_register(unit, DW_MMC_CLOCK_ENABLE, 0);
	write_register(unit, DW_MMC_CLOCK_SOURCE, 0);

	/* Sets the divider and has the card side take it. */
	write_register(unit, DW_MMC_CLOCK_DIVIDER, (uint32_t)divider);
	error = update_clock_registers(unit);
	if (error != 0)
		return error;

	/* Starts the clock again, stopping it between commands to save power. */
	write_register(unit, DW_MMC_CLOCK_ENABLE, DW_MMC_CLOCK_ON | DW_MMC_CLOCK_LOW_POWER);
	error = update_clock_registers(unit);
	if (error != 0)
		return error;

	/* Succeeded: records the rate the card runs at. */
	unit->card.card_clock = card_clock;
	return 0;
}

/*
 * Sets how many data lines the controller drives: one or four.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_bus_width).
 */
static int
set_bus_width(
	struct dw_mmc_unit *unit,
	uint32_t width)
{
	uint32_t card_type;

	/* Reads the card type, keeping the other cards' bits. */
	card_type = read_register(unit, DW_MMC_CARD_TYPE);
	card_type &= ~(DW_MMC_CARD_TYPE_4BIT | DW_MMC_CARD_TYPE_8BIT);

	/* Selects four lines, or leaves the one-line setting. */
	if (width == 4U) {
		card_type |= DW_MMC_CARD_TYPE_4BIT;
	} else if (width != 1U) {
		return EINVAL;
	}

	/* Succeeded: the controller drives that many lines. */
	write_register(unit, DW_MMC_CARD_TYPE, card_type);
	unit->card.bus_width = width;
	return 0;
}

/* Waits until the card stops holding the data line busy. */
static int
wait_data_idle(
	struct dw_mmc_unit *unit,
	uint64_t microseconds)
{
	uint64_t deadline;
	uint32_t status;
	int passed;
	int error;

	/* Sets how long the card may stay busy. */
	error = start_deadline(microseconds, &deadline);
	if (error != 0)
		return error;

	/* Polls the status until the busy bit clears. */
	for (;;) {
		/* The card is idle once the controller no longer sees it busy. */
		status = read_register(unit, DW_MMC_STATUS);
		if ((status & DW_MMC_STATUS_DATA_BUSY) == 0)
			break;

		/* A card that stays busy is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the data line is free. */
	return 0;
}

/*
 * Builds the command register's value for a command.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_exec_command).
 */
static uint32_t
command_value(
	const struct dw_mmc_command *command)
{
	uint32_t value;

	/* Every command starts through the hold register with its index. */
	value = DW_MMC_COMMAND_START | DW_MMC_COMMAND_HOLD | command->index;

	/*
	 * A stop aborts the transfer in progress; a status query may go while
	 * data moves; every other command waits for the previous data.
	 */
	if (command->index == SD_STOP_TRANSMISSION) {
		value |= DW_MMC_COMMAND_STOP_ABORT;
	} else if (command->index != SD_SEND_STATUS) {
		value |= DW_MMC_COMMAND_WAIT_DATA;
	}

	/* The reset command is preceded by the card's initialization clocks. */
	if (command->index == SD_GO_IDLE)
		value |= DW_MMC_COMMAND_INITIALIZE;

	/* Asks for the response the command expects, with its CRC checked where it has one. */
	switch (command->response_kind) {
	case DW_MMC_RESPONSE_SHORT:
	case DW_MMC_RESPONSE_BUSY:
		value |= DW_MMC_COMMAND_RESPONSE | DW_MMC_COMMAND_CHECK_CRC;
		break;
	case DW_MMC_RESPONSE_SHORT_NO_CRC:
		value |= DW_MMC_COMMAND_RESPONSE;
		break;
	case DW_MMC_RESPONSE_LONG:
		value |= DW_MMC_COMMAND_RESPONSE | DW_MMC_COMMAND_LONG | DW_MMC_COMMAND_CHECK_CRC;
		break;
	case DW_MMC_RESPONSE_NONE:
		break;
	}

	/* Marks a command that moves data, its direction, and the automatic stop. */
	if (command->length != 0) {
		value |= DW_MMC_COMMAND_DATA;
		if (command->write)
			value |= DW_MMC_COMMAND_WRITE;
		if (command->auto_stop)
			value |= DW_MMC_COMMAND_AUTO_STOP;
	}

	/* Reports the register's value. */
	return value;
}

/*
 * Sends one command, moves its data through the FIFO, and reads its
 * response.
 *
 * Returns 0, ETIMEDOUT when the card does not answer, or EIO for a
 * response or data error; the raw status of the failure is kept in
 * last_status.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_exec_command).
 */
static int
send_command(
	struct dw_mmc_unit *unit,
	struct dw_mmc_command *command)
{
	uint32_t value;
	uint32_t index;
	int error;

	/* Refuses data that does not fill the FIFO's entries whole. */
	if (command->length != 0) {
		if (command->data == NULL || command->block_size == 0)
			return EINVAL;
		if (command->length % unit->fifo_width != 0)
			return EINVAL;
	}

	/* Waits until the card stops holding the data line, except for a stop or a status query. */
	if (command->index != SD_STOP_TRANSMISSION && command->index != SD_SEND_STATUS) {
		error = wait_data_idle(unit, DW_MMC_BUSY_WAIT_US);
		if (error != 0)
			return error;
	}

	/* Describes the data of a command that moves any. */
	if (command->length != 0) {
		write_register(unit, DW_MMC_BYTE_COUNT, command->length);
		write_register(unit, DW_MMC_BLOCK_SIZE, command->block_size);

		/* Empties the FIFO of whatever an earlier transfer left. */
		error = reset_controller(unit, DW_MMC_CONTROL_FIFO_RESET);
		if (error != 0)
			return error;
	}

	/* Clears the raw status, then starts the command with its argument. */
	value = command_value(command);
	write_register(unit, DW_MMC_RAW_STATUS, DW_MMC_STATUS_ALL);
	write_register(unit, DW_MMC_ARGUMENT, command->argument);
	write_register(unit, DW_MMC_COMMAND, value);

	/* Waits for the response, or for the card to fail to give one. */
	error = wait_command_done(unit);
	if (error != 0)
		return error;

	/* Reads the response: four words for a long one, word 0 holding bits 31:0. */
	if (command->response_kind == DW_MMC_RESPONSE_LONG) {
		for (index = 0; index < 4U; index++)
			command->response[index] = read_register(unit, DW_MMC_RESPONSE0 + index * 4U);
	} else if (command->response_kind != DW_MMC_RESPONSE_NONE) {
		command->response[0] = read_register(unit, DW_MMC_RESPONSE0);
	}

	/* Moves the data, and waits for the controller's own stop. */
	if (command->length != 0) {
		error = move_data(unit, command);
		if (error != 0)
			return error;
	}

	/* A command that leaves the card busy is done once the card lets the line go. */
	if (command->response_kind == DW_MMC_RESPONSE_BUSY) {
		error = wait_data_idle(unit, DW_MMC_BUSY_WAIT_US);
		if (error != 0)
			return error;
	}

	/* Succeeded: the command and its data are done. */
	return 0;
}

/* Sends an application command: CMD55 to the card, then the command itself. */
static int
send_app_command(
	struct dw_mmc_unit *unit,
	struct dw_mmc_command *command)
{
	struct dw_mmc_command prefix;
	int error;

	/* Tells the card the next command is an application command. */
	prepare_command(&prefix, SD_APPLICATION, unit->card.rca << 16, DW_MMC_RESPONSE_SHORT);
	error = send_command(unit, &prefix);
	if (error != 0)
		return error;

	/* Sends the application command. */
	error = send_command(unit, command);
	if (error != 0)
		return error;

	/* Succeeded: the card answered the application command. */
	return 0;
}

/* Waits until the controller has the command's response, or reports why not. */
static int
wait_command_done(
	struct dw_mmc_unit *unit)
{
	uint64_t deadline;
	uint32_t status;
	int passed;
	int error;

	/* Sets how long the card has to answer. */
	error = start_deadline(DW_MMC_COMMAND_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Polls the raw status until the command is done. */
	for (;;) {
		/* A command the controller could not accept is given up on. */
		status = read_register(unit, DW_MMC_RAW_STATUS);
		if ((status & DW_MMC_STATUS_HARDWARE_LOCKED) != 0) {
			unit->last_status = status;
			return EIO;
		}

		/* The command is done once the controller says so. */
		if ((status & DW_MMC_STATUS_COMMAND_DONE) != 0)
			break;

		/* A controller that never finishes the command is given up on. */
		passed = deadline_passed(deadline);
		if (passed) {
			unit->last_status = status;
			return ETIMEDOUT;
		}
	}

	/* A card that gave no response did not take the command. */
	if ((status & DW_MMC_STATUS_RESPONSE_TIMEOUT) != 0) {
		unit->last_status = status;
		return ETIMEDOUT;
	}

	/* A response with a bad CRC or a bad form is an error. */
	if ((status & (DW_MMC_STATUS_RESPONSE_CRC | DW_MMC_STATUS_RESPONSE_ERROR)) != 0) {
		unit->last_status = status;
		return EIO;
	}

	/* Succeeded: the response is in the response registers. */
	return 0;
}

/*
 * Moves a command's data through the FIFO with preemption off.
 *
 * A thread switch in the middle of a run leaves the FIFO full (or, for a
 * write, empty) for as long as another thread runs, and the controller
 * stops the card clock until the FIFO is served.  On the VisionFive 2 the
 * reads that met a full FIFO were the ones that failed their data CRC
 * (status a4), at 49.5 MHz and at 24.75 MHz; with preemption off no read
 * failed at 24.75 MHz.  Interrupts stay on: a handler is short next to a
 * thread's quantum.  A run of DW_MMC_RUN_BLOCKS blocks keeps preemption off
 * for about 5 ms at 24.75 MHz.
 */
static int
move_data(
	struct dw_mmc_unit *unit,
	struct dw_mmc_command *command)
{
	int error;

	/* Serves the FIFO until the run has moved, without giving the CPU away. */
	kern_preempt_disable();
	error = serve_fifo(unit, command);
	kern_preempt_enable();

	/* Reports why the data did not move. */
	if (error != 0)
		return error;

	/* Succeeded: the data has moved. */
	return 0;
}

/*
 * Moves a command's data through the FIFO, a threshold's worth at a time,
 * then waits for the end of the data and for the automatic stop.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_transfer_data,
 * dwmmc_exec_command).
 */
static int
serve_fifo(
	struct dw_mmc_unit *unit,
	struct dw_mmc_command *command)
{
	uint64_t deadline;
	uint32_t remaining;
	uint32_t ready;
	uint32_t entries;
	uint32_t bytes;
	uint32_t status;
	uint8_t *data;
	int passed;
	int error;

	/*
	 * The event that says the FIFO can be served: the threshold reached
	 * in the direction of the transfer, the end of the data, or the host
	 * timeout, which the controller raises while it waits for the FIFO.
	 */
	ready = DW_MMC_STATUS_DATA_OVER | DW_MMC_STATUS_HOST_TIMEOUT;
	if (command->write) {
		ready |= DW_MMC_STATUS_TX_READY;
	} else {
		ready |= DW_MMC_STATUS_RX_READY;
	}

	/* Sets how long the FIFO may go without any data moving. */
	error = start_deadline(DW_MMC_FIFO_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Serves the FIFO until every byte has moved. */
	data = command->data;
	remaining = command->length;
	while (remaining > 0) {
		/* Waits until the FIFO can be served, or for an error. */
		error = wait_raw_status(unit, ready);
		if (error != 0)
			return error;

		/* Counts the entries that can move: those filled for a read, those free for a write. */
		status = read_register(unit, DW_MMC_STATUS);
		entries = (status >> DW_MMC_STATUS_FIFO_COUNT_SHIFT) & DW_MMC_STATUS_FIFO_COUNT_MASK;
		if (command->write) {
			if (entries > unit->fifo_depth)
				entries = unit->fifo_depth;
			entries = unit->fifo_depth - entries;
		}

		/* Moves those entries, or what is left of the data when that is less. */
		bytes = entries * unit->fifo_width;
		if (bytes > remaining)
			bytes = remaining;
		if (command->write) {
			write_fifo(unit, data, bytes);
		} else {
			read_fifo(unit, data, bytes);
		}

		/* Moves past the bytes the FIFO took or gave. */
		data += bytes;
		remaining -= bytes;

		/*
		 * Acknowledges the threshold and the host timeout, which the
		 * controller raises again while they hold.
		 */
		write_register(unit,
			       DW_MMC_RAW_STATUS,
			       DW_MMC_STATUS_TX_READY |
			       DW_MMC_STATUS_RX_READY |
			       DW_MMC_STATUS_HOST_TIMEOUT);

		/* Data that moved gives the FIFO its time again. */
		if (bytes != 0) {
			error = start_deadline(DW_MMC_FIFO_WAIT_US, &deadline);
			if (error != 0)
				return error;
			continue;
		}

		/* A FIFO that stays without anything to move, the end of the data included, is given up on. */
		passed = deadline_passed(deadline);
		if (passed) {
			unit->last_status = read_register(unit, DW_MMC_RAW_STATUS);
			return ETIMEDOUT;
		}
	}

	/* Waits for the end of the data. */
	error = wait_raw_status(unit, DW_MMC_STATUS_DATA_OVER);
	if (error != 0)
		return error;

	/* Waits for the controller's own stop after the last block. */
	if (command->auto_stop) {
		error = wait_raw_status(unit, DW_MMC_STATUS_AUTO_STOP_DONE);
		if (error != 0)
			return error;
	}

	/* Succeeded: the data has moved. */
	return 0;
}

/* Waits until any of the wanted raw status bits is set, or for a data error. */
static int
wait_raw_status(
	struct dw_mmc_unit *unit,
	uint32_t wanted)
{
	uint64_t deadline;
	uint32_t status;
	int passed;
	int error;

	/* Sets how long the controller has to raise the event. */
	error = start_deadline(DW_MMC_FIFO_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Polls the raw status until the event or an error. */
	for (;;) {
		/* A data error ends the transfer. */
		status = read_register(unit, DW_MMC_RAW_STATUS);
		if ((status & DW_MMC_STATUS_DATA_ERRORS) != 0) {
			unit->last_status = status;
			return EIO;
		}

		/* A card that sends no data in time ends the transfer. */
		if ((status & DW_MMC_STATUS_DATA_TIMEOUT) != 0) {
			unit->last_status = status;
			return ETIMEDOUT;
		}

		/* The wanted event ends the wait. */
		if ((status & wanted) != 0)
			break;

		/* A controller that never raises it is given up on. */
		passed = deadline_passed(deadline);
		if (passed) {
			unit->last_status = status;
			return ETIMEDOUT;
		}
	}

	/* Succeeded: the event happened. */
	return 0;
}

/*
 * Reads bytes from the FIFO's data port, whole entries only, storing the
 * bytes of each 32-bit word lowest first.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_read_data32,
 * dwmmc_read_data64).
 */
static void
read_fifo(
	struct dw_mmc_unit *unit,
	uint8_t *data,
	uint32_t bytes)
{
	uint32_t offset;
	uint32_t word;
	uint32_t done;

	/*
	 * Reads one 32-bit word per four bytes; a 64-bit entry is its two
	 * halves at the port and the word after it.
	 */
	for (done = 0; done < bytes; done += 4U) {
		offset = unit->fifo_offset;
		if (unit->fifo_width == 8U && (done & 4U) != 0)
			offset += 4U;
		word = read_register(unit, offset);
		data[done + 0U] = (uint8_t)(word & 0xffU);
		data[done + 1U] = (uint8_t)((word >> 8) & 0xffU);
		data[done + 2U] = (uint8_t)((word >> 16) & 0xffU);
		data[done + 3U] = (uint8_t)((word >> 24) & 0xffU);
	}
}

/*
 * Writes bytes to the FIFO's data port, whole entries only, the lowest
 * byte of each 32-bit word first.
 *
 * Procedure referenced: OpenBSD sys/dev/fdt/dwmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (dwmmc_write_data32,
 * dwmmc_write_data64).
 */
static void
write_fifo(
	struct dw_mmc_unit *unit,
	const uint8_t *data,
	uint32_t bytes)
{
	uint32_t offset;
	uint32_t word;
	uint32_t done;

	/*
	 * Writes one 32-bit word per four bytes; a 64-bit entry is its two
	 * halves at the port and the word after it.
	 */
	for (done = 0; done < bytes; done += 4U) {
		offset = unit->fifo_offset;
		if (unit->fifo_width == 8U && (done & 4U) != 0)
			offset += 4U;
		word = (uint32_t)data[done + 0U];
		word |= (uint32_t)data[done + 1U] << 8;
		word |= (uint32_t)data[done + 2U] << 16;
		word |= (uint32_t)data[done + 3U] << 24;
		write_register(unit, offset, word);
	}
}

/* Fills in a command without data. */
static void
prepare_command(
	struct dw_mmc_command *command,
	uint32_t index,
	uint32_t argument,
	enum dw_mmc_response response_kind)
{
	unsigned word;

	/* Names the command, its argument and the response it expects. */
	command->index = index;
	command->argument = argument;
	command->response_kind = response_kind;

	/* Moves no data until the caller says otherwise. */
	command->data = NULL;
	command->length = 0;
	command->block_size = 0;
	command->write = 0;
	command->auto_stop = 0;

	/* Clears the response. */
	for (word = 0; word < 4U; word++)
		command->response[word] = 0;
}

/*
 * Extracts a field of up to 32 bits from a response, counting bit 0 as
 * the lowest bit of word 0.
 */
static uint32_t
response_bits(
	const uint32_t *response,
	uint32_t start,
	uint32_t width)
{
	uint32_t position;
	uint32_t value;
	uint32_t bit;

	/* Gathers the field one bit at a time, lowest first. */
	value = 0;
	for (bit = 0; bit < width; bit++) {
		position = start + bit;
		if (((response[position / 32U] >> (position % 32U)) & 1U) != 0) {
			value |= 1U << bit;
		}
	}

	/* Reports the field. */
	return value;
}

/*
 * Brings the card from the idle state to the transfer state, then widens
 * and speeds up the bus.
 */
static int
start_card(
	struct dw_mmc_unit *unit)
{
	int error;

	/* Identifies the card: its capacity, its identity and its address. */
	error = identify_card(unit);
	if (error != 0)
		return error;

	/* Selects the card, which takes it to the transfer state. */
	error = select_card(unit);
	if (error != 0)
		return error;

	/* Widens the bus to four lines and moves to high speed where both ends allow it. */
	error = configure_bus(unit);
	if (error != 0)
		return error;

	/* Succeeded: the card is ready for data. */
	return 0;
}

/*
 * Resets the card, learns its operating conditions and capacity class,
 * reads its identity and capacity, and has it publish its address.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/sdmmc.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_go_idle_state,
 * sdmmc_send_if_cond, sdmmc_set_relative_addr) and
 * sys/dev/sdmmc/sdmmc_mem.c (sdmmc_mem_enable, sdmmc_mem_scan).
 */
static int
identify_card(
	struct dw_mmc_unit *unit)
{
	struct dw_mmc_command command;
	uint32_t cid[4];
	uint32_t request;
	uint32_t ocr;
	unsigned word;
	int error;

	/* Resets the card to the idle state; the command has no response. */
	unit->card.rca = 0;
	prepare_command(&command, SD_GO_IDLE, 0, DW_MMC_RESPONSE_NONE);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/*
	 * Asks whether the card takes the supply voltage.  A version 2 card
	 * echoes the pattern and may be a high-capacity card; an older card
	 * does not answer, and is a standard-capacity card.
	 */
	request = SD_OCR_3V3;
	prepare_command(&command,
			SD_SEND_INTERFACE_CONDITION,
			SD_INTERFACE_VOLTAGE | SD_INTERFACE_PATTERN,
			DW_MMC_RESPONSE_SHORT);
	error = send_command(unit, &command);
	if (error == 0) {
		/* A version 2 card that echoes another pattern did not understand the voltage. */
		if ((command.response[0] & SD_INTERFACE_PATTERN_MASK) != SD_INTERFACE_PATTERN)
			return EIO;
		request |= SD_OCR_HIGH_CAPACITY;
	} else if (error != ETIMEDOUT) {
		/* Anything but silence is a failure of the controller or the card. */
		return error;
	}

	/* Has the card power up at 3.3 V, waiting until it says it is ready. */
	error = wait_card_powered(unit, request, &ocr);
	if (error != 0)
		return error;

	/* Records the conditions and whether the card is a high-capacity one. */
	unit->card.ocr = ocr;
	unit->card.high_capacity = 0;
	if ((ocr & SD_OCR_HIGH_CAPACITY) != 0)
		unit->card.high_capacity = 1;

	/* Reads the card's identity, which takes it to the identification state. */
	prepare_command(&command, SD_ALL_SEND_CID, 0, DW_MMC_RESPONSE_LONG);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Keeps the identity for decoding once the capacity is read. */
	for (word = 0; word < 4U; word++)
		cid[word] = command.response[word];

	/* Has the card publish its address, which takes it to the stand-by state. */
	prepare_command(&command, SD_SEND_RELATIVE_ADDRESS, 0, DW_MMC_RESPONSE_SHORT);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Records the address, which every later command names the card by. */
	unit->card.rca = command.response[0] >> 16;

	/* Reads the card's capacity. */
	prepare_command(&command, SD_SEND_CSD, unit->card.rca << 16, DW_MMC_RESPONSE_LONG);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Decodes the capacity. */
	error = decode_csd(unit, command.response);
	if (error != 0)
		return error;

	/* Decodes the identity. */
	decode_cid(unit, cid);

	/* Succeeded: the card is identified and waits to be selected. */
	return 0;
}

/*
 * Sends the operating conditions until the card has powered up.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/sdmmc_mem.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_mem_send_op_cond).
 */
static int
wait_card_powered(
	struct dw_mmc_unit *unit,
	uint32_t request,
	uint32_t *ocr)
{
	struct dw_mmc_command command;
	uint64_t deadline;
	int passed;
	int error;

	/* Sets how long the card has to power up. */
	error = start_deadline(DW_MMC_READY_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Repeats the request until the card reports that it has powered up. */
	for (;;) {
		/* Sends the request; the card answers with its conditions. */
		prepare_command(&command, SD_APP_SEND_OPERATING, request, DW_MMC_RESPONSE_SHORT_NO_CRC);
		error = send_app_command(unit, &command);
		if (error != 0)
			return error;

		/* A card that has powered up says so in its conditions. */
		if ((command.response[0] & SD_OCR_READY) != 0)
			break;

		/* A card that never powers up is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;

		/* Gives the card time before the next request. */
		pause_microseconds(DW_MMC_READY_RETRY_US);
	}

	/* Succeeded: the conditions of the powered card. */
	*ocr = command.response[0];
	return 0;
}

/*
 * Decodes the card's capacity in 512-byte blocks from its CSD.
 *
 * A version 1 CSD gives the capacity as a size, a multiplier and a block
 * length; a version 2 CSD gives it in units of 512 KiB.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/sdmmcreg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SD_CSD_CSDVER,
 * SD_CSD_CAPACITY, SD_CSD_V2_CAPACITY, SD_CSD_READ_BL_LEN).
 */
static int
decode_csd(
	struct dw_mmc_unit *unit,
	const uint32_t *csd)
{
	uint32_t structure;
	uint64_t size;
	uint32_t multiplier;
	uint32_t length_shift;
	uint64_t bytes;

	/* Reads the CSD's version and the card's command classes. */
	structure = response_bits(csd, SD_CSD_VERSION_BIT, 2U);
	unit->card.csd_version = structure + 1U;
	unit->command_classes = response_bits(csd, SD_CSD_CCC_BIT, 12U);

	/* Computes the capacity the way the version describes it. */
	if (structure == 0) {
		/* Version 1: (size + 1) blocks of 2^(multiplier + 2) of 2^length bytes. */
		size = response_bits(csd, SD_CSD_V1_SIZE_BIT, SD_CSD_V1_SIZE_WIDTH);
		multiplier = response_bits(csd, SD_CSD_V1_MULTIPLIER_BIT, 3U);
		length_shift = response_bits(csd, SD_CSD_READ_LENGTH_BIT, 4U);
		bytes = (size + 1ULL) << (multiplier + 2U + length_shift);
		unit->card.block_count = bytes / DW_MMC_BLOCK_BYTES;
	} else if (structure == 1U) {
		/* Version 2: (size + 1) units of 512 KiB, which is 1024 blocks. */
		size = response_bits(csd, SD_CSD_V2_SIZE_BIT, SD_CSD_V2_SIZE_WIDTH);
		unit->card.block_count = (size + 1ULL) * 1024ULL;
	} else {
		/* Later versions describe cards larger than the driver serves. */
		return ENOTSUP;
	}

	/* Refuses a card without capacity. */
	if (unit->card.block_count == 0)
		return EIO;

	/* Succeeded: the capacity is known. */
	return 0;
}

/*
 * Decodes the card's manufacturer, OEM, product name, revision and serial
 * number from its CID.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/sdmmcreg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SD_CID_MID, SD_CID_OID,
 * SD_CID_PNM_CPY, SD_CID_REV, SD_CID_PSN).
 */
static void
decode_cid(
	struct dw_mmc_unit *unit,
	const uint32_t *cid)
{
	uint32_t character;
	uint32_t oem;
	unsigned index;

	/* Reads the manufacturer, the revision and the serial number. */
	unit->card.manufacturer = response_bits(cid, SD_CID_MANUFACTURER_BIT, 8U);
	unit->card.revision = response_bits(cid, SD_CID_REVISION_BIT, 8U);
	unit->card.serial = response_bits(cid, SD_CID_SERIAL_BIT, 32U);

	/* Reads the OEM's two characters, a dot standing for one that cannot be printed. */
	oem = response_bits(cid, SD_CID_OEM_BIT, 16U);
	unit->card.oem[0] = (char)((oem >> 8) & 0xffU);
	unit->card.oem[1] = (char)(oem & 0xffU);
	unit->card.oem[2] = '\0';

	/* Reads the product name's five characters, the first in the highest bits. */
	for (index = 0; index < 5U; index++) {
		character = response_bits(cid, SD_CID_PRODUCT_BIT + (4U - index) * 8U, 8U);
		if (character < 0x20U || character > 0x7eU)
			character = '.';
		unit->card.product[index] = (char)character;
	}

	/* Terminates the name. */
	unit->card.product[5] = '\0';

	/* Replaces an OEM character that cannot be printed. */
	for (index = 0; index < 2U; index++) {
		character = (uint32_t)(uint8_t)unit->card.oem[index];
		if (character < 0x20U || character > 0x7eU)
			unit->card.oem[index] = '.';
	}
}

/*
 * Selects the card, which takes it to the transfer state, and sets the
 * block length.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/sdmmc_mem.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_mem_init,
 * sdmmc_mem_set_blocklen).
 */
static int
select_card(
	struct dw_mmc_unit *unit)
{
	struct dw_mmc_command command;
	int error;

	/* Selects the card by its address; the card may hold the line busy. */
	prepare_command(&command, SD_SELECT_CARD, unit->card.rca << 16, DW_MMC_RESPONSE_BUSY);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Sets the block length; a high-capacity card's is fixed at the same 512 bytes. */
	prepare_command(&command, SD_SET_BLOCK_LENGTH, DW_MMC_BLOCK_BYTES, DW_MMC_RESPONSE_SHORT);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Succeeded: the card is in the transfer state. */
	return 0;
}

/*
 * Runs the bus at the default speed, widens it to four lines when the
 * slot and the card have them, and moves to high speed when both allow
 * it.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/sdmmc_mem.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_mem_sd_init,
 * sdmmc_mem_set_bus_width).
 */
static int
configure_bus(
	struct dw_mmc_unit *unit)
{
	struct dw_mmc_command command;
	uint32_t specification;
	uint32_t bus_widths;
	uint64_t target;
	int switched;
	int error;

	/* Every card runs at the default speed once identified. */
	error = set_card_clock(unit, DW_MMC_DEFAULT_HZ);
	if (error != 0)
		return error;

	/* Reads which bus widths and which specification the card has. */
	error = read_scr(unit, &specification, &bus_widths);
	if (error != 0)
		return error;

	/* Widens the bus to four lines, the card first and then the controller. */
	if (unit->slot_bus_width >= 4U && (bus_widths & SD_SCR_BUS_WIDTH_4) != 0) {
		prepare_command(&command, SD_APP_SET_BUS_WIDTH, SD_BUS_WIDTH_4_ARGUMENT, DW_MMC_RESPONSE_SHORT);
		error = send_app_command(unit, &command);
		if (error != 0)
			return error;

		/* Then widens the controller's side. */
		error = set_bus_width(unit, 4U);
		if (error != 0)
			return error;
	}

	/*
	 * Moves to high speed only when the slot allows it and the card has
	 * the switch function: a card of specification 1.10 or later that
	 * lists the switch class.
	 */
	switched = 0;
	if (unit->high_speed_allowed &&
	    specification >= SD_SCR_SPEC_1_10 &&
	    (unit->command_classes & SD_CSD_CCC_SWITCH) != 0) {
		error = switch_high_speed(unit, &switched);
		if (error != 0)
			return error;
	}

	/* Runs the clock at high speed, kept under the slot's limit. */
	if (switched) {
		target = DW_MMC_HIGH_SPEED_HZ;
		if (unit->max_frequency != 0 && unit->max_frequency < target)
			target = unit->max_frequency;

		/* Gives the card eight clocks at the old rate to finish the switch. */
		pause_microseconds(25ULL);

		/* Moves the card clock to the higher rate. */
		error = set_card_clock(unit, target);
		if (error != 0)
			return error;

		/* Records the speed for the boot log. */
		unit->card.high_speed = 1;
	}

	/* Succeeded: the bus runs as fast and as wide as both ends allow. */
	return 0;
}

/*
 * Reads the card's SCR and reports its specification version and bus
 * widths.
 *
 * The SCR arrives as eight bytes, the highest bits first: bits 59:56 in
 * byte 0 are the specification, bits 51:48 in byte 1 the bus widths.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/sdmmc_mem.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_mem_send_scr,
 * sdmmc_mem_decode_scr).  Values referenced: OpenBSD
 * sys/dev/sdmmc/sdmmcreg.h (SCR_SD_SPEC, SCR_SD_BUS_WIDTHS).
 */
static int
read_scr(
	struct dw_mmc_unit *unit,
	uint32_t *specification,
	uint32_t *bus_widths)
{
	struct dw_mmc_command command;
	uint8_t scr[SD_SCR_BYTES];
	int error;

	/* Reads the eight bytes of the SCR as one block. */
	prepare_command(&command, SD_APP_SEND_SCR, 0, DW_MMC_RESPONSE_SHORT);
	command.data = scr;
	command.length = SD_SCR_BYTES;
	command.block_size = SD_SCR_BYTES;
	error = send_app_command(unit, &command);
	if (error != 0)
		return error;

	/* Succeeded: the specification version and the bus widths. */
	*specification = scr[0] & 0x0fU;
	*bus_widths = scr[1] & 0x0fU;
	return 0;
}

/*
 * Asks the card which bus speeds it has, and switches it to high speed
 * when it has that.
 *
 * The switch function's 64-byte status arrives highest bits first; the
 * functions group 1 supports are bits 415:400, in bytes 12 and 13.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/sdmmc_mem.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_mem_sd_switch,
 * sdmmc_mem_sd_init).  Values referenced: OpenBSD
 * sys/dev/sdmmc/sdmmcreg.h (SFUNC_STATUS_GROUP, SD_ACCESS_MODE_SDR25)
 * and sys/dev/sdmmc/sdmmc_mem.c (the switch argument).
 */
static int
switch_high_speed(
	struct dw_mmc_unit *unit,
	int *switched)
{
	struct dw_mmc_command command;
	uint8_t status[SD_SWITCH_STATUS_BYTES];
	uint32_t supported;
	int error;

	/* Asks, without switching, which functions of group 1 the card has. */
	*switched = 0;
	prepare_command(&command, SD_SWITCH_FUNCTION, SD_SWITCH_KEEP, DW_MMC_RESPONSE_SHORT);
	command.data = status;
	command.length = SD_SWITCH_STATUS_BYTES;
	command.block_size = SD_SWITCH_STATUS_BYTES;
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* A card without high speed stays at the default speed. */
	supported = ((uint32_t)status[SD_SWITCH_GROUP1_SUPPORT_BYTE] << 8) |
		    (uint32_t)status[SD_SWITCH_GROUP1_SUPPORT_BYTE + 1U];
	if ((supported & (1U << SD_SWITCH_HIGH_SPEED)) == 0)
		return 0;

	/* Switches group 1 to high speed, keeping every other group. */
	prepare_command(&command,
			SD_SWITCH_FUNCTION,
			SD_SWITCH_SET | (SD_SWITCH_KEEP & ~0xfU) | SD_SWITCH_HIGH_SPEED,
			DW_MMC_RESPONSE_SHORT);
	command.data = status;
	command.length = SD_SWITCH_STATUS_BYTES;
	command.block_size = SD_SWITCH_STATUS_BYTES;
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Succeeded: the card runs at high speed once the clock follows. */
	*switched = 1;
	return 0;
}

/*
 * Asks the card for its status until it is ready for data, which a write
 * reaches once the card has programmed the blocks.
 *
 * Procedure referenced: OpenBSD sys/dev/sdmmc/sdmmc_mem.c, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (sdmmc_mem_read_block_subr,
 * the status loop).
 */
static int
wait_card_ready(
	struct dw_mmc_unit *unit)
{
	struct dw_mmc_command command;
	uint64_t deadline;
	int passed;
	int error;

	/* Sets how long the card may take to program the blocks. */
	error = start_deadline(DW_MMC_BUSY_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Repeats the status query until the card is ready for data. */
	for (;;) {
		/* Asks the card for its status. */
		prepare_command(&command, SD_SEND_STATUS, unit->card.rca << 16, DW_MMC_RESPONSE_SHORT);
		error = send_command(unit, &command);
		if (error != 0)
			return error;

		/* A card ready for data has finished the last command's work. */
		if ((command.response[0] & SD_CARD_READY_FOR_DATA) != 0)
			break;

		/* A card that stays busy is given up on. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the card is ready for the next command. */
	return 0;
}

/*
 * Moves one run of blocks with one command: CMD17 or CMD24 for a single
 * block, CMD18 or CMD25 with the controller's automatic stop for more.
 */
static int
transfer_run(
	struct dw_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	struct dw_mmc_command command;
	uint64_t address;
	uint32_t index;
	int error;

	/* A high-capacity card is addressed by block, an older card by byte. */
	address = block;
	if (!unit->card.high_capacity)
		address = block * DW_MMC_BLOCK_BYTES;

	/* Refuses an address the 32-bit argument cannot carry. */
	if (address > 0xffffffffULL)
		return EINVAL;

	/* Picks the single-block or multiple-block command of the direction. */
	if (write) {
		index = SD_WRITE_SINGLE;
		if (count > 1U)
			index = SD_WRITE_MULTIPLE;
	} else {
		index = SD_READ_SINGLE;
		if (count > 1U)
			index = SD_READ_MULTIPLE;
	}

	/* Moves the blocks; the controller stops a multiple-block command itself. */
	prepare_command(&command, index, (uint32_t)address, DW_MMC_RESPONSE_SHORT);
	command.data = data;
	command.length = count * DW_MMC_BLOCK_BYTES;
	command.block_size = DW_MMC_BLOCK_BYTES;
	command.write = write;
	if (count > 1U)
		command.auto_stop = 1;

	/* Sends the command and moves its data. */
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* A write is done once the card has programmed the blocks. */
	if (write) {
		error = wait_card_ready(unit);
		if (error != 0)
			return error;
	}

	/* Succeeded: the run has moved. */
	return 0;
}

/*
 * Moves one run, recovering the card and trying again when it fails, and
 * a block at a time when the whole run never moves.
 *
 * A card that cannot be recovered marks the unit failed.
 */
static int
transfer_run_recovering(
	struct dw_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	uint32_t index;
	int error;

	/* The whole run, with its tries. */
	error = transfer_run_tries(unit, write, block, count, data);
	if (error == 0 || unit->failed || count == 1U)
		return error;

	/* A run of several blocks that never moved goes a block at a time, each with its own tries. */
	unit->split_runs++;
	kern_logf("dw-mmc: %s of %u blocks at %llu moved a block at a time (errors %llu, split runs %llu)\n",
		  write ? "write" : "read", count, (unsigned long long)block,
		  (unsigned long long)unit->errors, (unsigned long long)unit->split_runs);
	for (index = 0; index < count; index++) {
		error = transfer_run_tries(unit, write, block + index, 1U, data + (size_t)index * DW_MMC_BLOCK_BYTES);
		if (error != 0)
			return error;
	}

	/* Succeeded: every block has moved. */
	return 0;
}

/*
 * Tries one run up to DW_MMC_RUN_TRIES times, recovering the card between
 * the tries; a card that a recovery cannot bring back after
 * DW_MMC_RECOVER_TRIES attempts marks the unit failed.
 */
static int
transfer_run_tries(
	struct dw_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	unsigned attempt;
	int recovered;
	int error;

	/* Tries the run, recovering the card between the tries. */
	error = EIO;
	for (attempt = 0; attempt < DW_MMC_RUN_TRIES; attempt++) {
		/* Moves the run; a run that moved ends the tries. */
		error = transfer_run(unit, write, block, count, data);
		if (error == 0)
			break;

		/* Reports the failed try with the status that ended it, and the count so far. */
		unit->errors++;
		kern_logf("dw-mmc: %s of %u blocks at %llu failed (%d, status %x), try %u (errors %llu)\n",
			  write ? "write" : "read",
			  count,
			  (unsigned long long)block,
			  error,
			  unit->last_status,
			  attempt + 1U,
			  (unsigned long long)unit->errors);

		/* Brings the card back to the transfer state, a few times; a card that never comes back is failed. */
		recovered = recover_card_tries(unit, "after a failed run");
		if (recovered != 0) {
			unit->failed = 1;
			break;
		}

		/* An error of the data lines' signal adapts the bus before the next try. */
		if ((unit->last_status & DW_MMC_STATUS_SIGNAL_ERRORS) != 0) {
			recovered = adapt_bus(unit, write, block, count, data);
			if (recovered != 0) {
				unit->failed = 1;
				break;
			}
		}
	}

	/* Reports why the run did not move. */
	if (error != 0)
		return error;

	/* Succeeded: the run has moved. */
	return 0;
}

/*
 * Brings the card back to the transfer state after a failed command: the
 * controller and its FIFO are reset, the transfer in progress is stopped,
 * and the card is asked until it is ready for data.
 */
static int
recover_card(
	struct dw_mmc_unit *unit)
{
	struct dw_mmc_command command;
	int error;

	/*
	 * Resets the controller's state machines, its FIFO and its DMA
	 * interface, has the card side take the clock registers again, and
	 * clears the raw status the failed transfer left.
	 *
	 * Procedure referenced: Linux v6.12 drivers/mmc/host/dw_mmc.c
	 * (dw_mci_reset: SDMMC_CTRL_ALL_RESET_FLAGS, then the clock update).
	 */
	error = reset_controller(unit, DW_MMC_CONTROL_RESET | DW_MMC_CONTROL_FIFO_RESET | DW_MMC_CONTROL_DMA_RESET);
	if (error != 0)
		return error;
	error = update_clock_registers(unit);
	if (error != 0)
		return error;
	write_register(unit, DW_MMC_RAW_STATUS, DW_MMC_STATUS_ALL);

	/* Stops a transfer still in progress; a card with none refuses it, which is harmless. */
	prepare_command(&command, SD_STOP_TRANSMISSION, 0, DW_MMC_RESPONSE_BUSY);
	(void)send_command(unit, &command);

	/* Waits until the card is ready for data again. */
	error = wait_card_ready(unit);
	if (error != 0)
		return error;

	/* Succeeded: the card is back in the transfer state. */
	return 0;
}

/*
 * Brings the card back to the transfer state up to DW_MMC_RECOVER_TRIES
 * times; reports a card that never comes back, naming when it was tried.
 *
 * Returns 0, or the errno of the last recovery.
 */
static int
recover_card_tries(
	struct dw_mmc_unit *unit,
	const char *during)
{
	unsigned again;
	int error;

	/* Tries the recovery until it succeeds or the tries run out. */
	error = EIO;
	for (again = 0; again < DW_MMC_RECOVER_TRIES && error != 0; again++)
		error = recover_card(unit);
	if (error != 0) {
		kern_logf("dw-mmc: the card did not come back %s (%d, status %x); it is taken as gone\n",
			  during,
			  error,
			  unit->last_status);
		return error;
	}

	/* Succeeded: the card is back in the transfer state. */
	return 0;
}

/*
 * Adapts the bus to a run that failed with an error of the data lines'
 * signal, one step per call: first the sample phase is tuned on the
 * failed run when it is a read and the glue can set the phase, then the
 * card clock goes down to the default speed.  A bus that has taken every
 * step, or runs at the default speed already, is left as it is.
 *
 * The card stays in its high speed mode at the lower clock, which the
 * mode allows: high speed only raises the highest clock to 50 MHz.
 *
 * Returns 0, or the errno of a card that could not be brought back to the
 * transfer state.
 *
 * Procedure referenced: Linux v6.12 drivers/mmc/host/dw_mmc-starfive.c
 * (dw_mci_starfive_execute_tuning: every phase tried, the middle of the
 * phases that pass taken) and drivers/mmc/core/core.c
 * (mmc_set_clock lowering the clock is what the core does for a card
 * whose tuning fails).
 */
static int
adapt_bus(
	struct dw_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	int tuned;
	int error;

	/* A bus at the default speed already has nothing left to adapt. */
	if (unit->card.card_clock <= DW_MMC_DEFAULT_HZ)
		return 0;

	/* Tunes the sample phase on the failed read, once. */
	if (unit->bus_step == DW_MMC_BUS_AS_SET) {
		unit->bus_step = DW_MMC_BUS_PHASE_TUNED;
		if (!write && unit->sample_phase_count != 0) {
			tuned = tune_sample_phase(unit, block, count, data);
			if (tuned < 0)
				return EIO;
			if (tuned > 0)
				return 0;
		}
	}

	/* Moves the card clock down to the default speed, once. */
	if (unit->bus_step == DW_MMC_BUS_SLOWED)
		return 0;
	unit->bus_step = DW_MMC_BUS_SLOWED;
	error = set_card_clock(unit, DW_MMC_DEFAULT_HZ);
	if (error != 0)
		return error;
	unit->card.slowed = 1;
	kern_logf("dw-mmc: data errors at high speed, card clock down to %llu Hz\n",
		  (unsigned long long)unit->card.card_clock);

	/* Succeeded: the next try runs at the default speed. */
	return 0;
}

/*
 * Reads the failed run at every sample phase and keeps the middle of the
 * longest stretch of phases that read it without an error.
 *
 * Returns 1 when a phase read the run, 0 when none did (the phase is put
 * back as it was), or -1 when the card could not be brought back to the
 * transfer state after a failed read.
 */
static int
tune_sample_phase(
	struct dw_mmc_unit *unit,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	uint32_t phase;
	uint32_t start;
	uint32_t length;
	uint32_t best_start;
	uint32_t best_length;
	int error;

	/* Tries every phase, measuring the stretches of phases that pass. */
	start = 0;
	length = 0;
	best_start = 0;
	best_length = 0;
	for (phase = 0; phase < unit->sample_phase_count; phase++) {
		/* Reads the run at this phase. */
		set_sample_phase(unit, phase);
		error = transfer_run(unit, 0, block, count, data);

		/* A phase that passes extends the stretch it is in. */
		if (error == 0) {
			if (length == 0)
				start = phase;
			length++;
			if (length > best_length) {
				best_start = start;
				best_length = length;
			}
			continue;
		}

		/*
		 * A phase that fails ends the stretch, and the card is brought
		 * back at the phase it had before the tuning, so that the
		 * recovery's own commands are not sampled at the failing phase,
		 * with the same tries as after a failed run: one recovery that a
		 * noisy bus fails must not take the card as gone.
		 */
		length = 0;
		set_sample_phase(unit, unit->card.sample_phase);
		error = recover_card_tries(unit, "while the sample phase was tuned");
		if (error != 0)
			return -1;
	}

	/* No phase read the run: the phase goes back as it was. */
	if (best_length == 0) {
		set_sample_phase(unit, unit->card.sample_phase);
		kern_logf("dw-mmc: no sample phase reads at %llu Hz\n",
			  (unsigned long long)unit->card.card_clock);
		return 0;
	}

	/* Takes the middle of the longest stretch. */
	phase = best_start + (best_length - 1U) / 2U;
	set_sample_phase(unit, phase);
	unit->card.sample_phase = phase;
	kern_logf("dw-mmc: sample phase %u (phases %u-%u read)\n",
		  phase,
		  best_start,
		  best_start + best_length - 1U);

	/* Succeeded: the run reads at the new phase. */
	return 1;
}

/* Has the glue set the controller's sample phase and waits for it to settle. */
static void
set_sample_phase(
	struct dw_mmc_unit *unit,
	uint32_t phase)
{
	unit->set_sample_phase(unit->phase_context, phase);
	pause_microseconds(DW_MMC_PHASE_SETTLE_US);
}

/* Publishes the card as the disk mmcblk<unit>. */
static int
publish_disk(
	struct dw_mmc_unit *unit,
	unsigned unit_number)
{
	struct disk *disk;
	int error;

	/* Allocates the disk. */
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;

	/* Prepares the lock that serializes the commands. */
	error = mutex_init(&unit->lock, LOCK_RANK_DISK, "dw-mmc");
	if (error != 0)
		return error;

	/* Names the disk mmcblk<unit>; the unit number is one digit. */
	disk->d_name[0] = 'm';
	disk->d_name[1] = 'm';
	disk->d_name[2] = 'c';
	disk->d_name[3] = 'b';
	disk->d_name[4] = 'l';
	disk->d_name[5] = 'k';
	disk->d_name[6] = (char)('0' + unit_number);
	disk->d_name[7] = '\0';

	/* Describes the card: removable, 512-byte blocks, its capacity, runs of up to 64 KiB. */
	disk->d_flags = DISK_REMOVABLE;
	disk->d_block_size = DW_MMC_BLOCK_BYTES;
	disk->d_block_count = unit->card.block_count;
	disk->d_max_transfer_blocks = DW_MMC_RUN_BLOCKS;
	disk->d_ops = &dw_mmc_disk_ops;
	disk->d_data = unit;

	/* hw.diskstats lists the card as an SD/MMC disk. */
	disk_set_stats_kind(disk, DISK_STATS_KIND_SDMMC);

	/* Publishes the disk. */
	error = disk_create(disk);
	if (error != 0)
		return error;

	/* Succeeded: the card is a published disk. */
	unit->disk = disk;
	return 0;
}

/*
 * Writes the controller and the card to the boot log: the controller's
 * version and FIFO, the card's kind, identity and size, and how the bus
 * runs.
 */
static void
log_card(
	const struct dw_mmc_unit *unit)
{
	const struct drv_dw_mmc_card *card;
	const char *speed;

	/* Writes the controller's version, its FIFO and its interface clock. */
	card = &unit->card;
	kern_logf("dw-mmc: %s controller version %x, FIFO %u x %u bytes, ciu %llu Hz\n",
		  unit->disk->d_name,
		  card->controller_version,
		  card->fifo_depth,
		  card->fifo_width,
		  (unsigned long long)unit->clock_rate);

	/* Writes the card's kind, identity and size. */
	kern_logf("dw-mmc: %s %s card %s, manufacturer %02x, OEM %s, revision %u.%u, serial %08x\n",
		  unit->disk->d_name,
		  card_kind(card),
		  card->product,
		  card->manufacturer,
		  card->oem,
		  card->revision >> 4,
		  card->revision & 0x0fU,
		  card->serial);
	kern_logf("dw-mmc: %s %llu blocks of 512 bytes (%llu MiB)\n",
		  unit->disk->d_name,
		  (unsigned long long)card->block_count,
		  (unsigned long long)(card->block_count / 2048ULL));

	/* Writes how the bus runs. */
	speed = "default speed";
	if (card->high_speed)
		speed = "high speed";
	kern_logf("dw-mmc: %s bus %u-bit, %llu Hz, %s\n",
		  unit->disk->d_name,
		  card->bus_width,
		  (unsigned long long)card->card_clock,
		  speed);
}

/*
 * Names the card's kind: SDSC for a version 1 CSD, SDHC for a version 2
 * CSD of up to 32 GiB, and SDXC above that.
 */
static const char *
card_kind(
	const struct drv_dw_mmc_card *card)
{
	/* A version 1 CSD describes a standard-capacity card. */
	if (card->csd_version == 1U)
		return "SDSC";

	/* A version 2 CSD above 32 GiB describes an extended-capacity card. */
	if (card->block_count > DW_MMC_SDHC_MAX_BLOCKS)
		return "SDXC";

	/* The rest are high-capacity cards. */
	return "SDHC";
}

/*
 * Serves one request to the card's disk: reads and writes are moved run
 * by run, and a flush has nothing to wait for, since every write has been
 * programmed before it completed.
 */
static int
disk_submit(
	struct disk *disk,
	struct bio *bio)
{
	struct dw_mmc_unit *unit;
	uint8_t *data;
	uint32_t done;
	uint32_t run;
	size_t transferred;
	int write;
	int error;

	/* Picks the requests the card serves, and the direction of a read or a write. */
	unit = disk->d_data;
	data = bio->b_data;
	write = 0;
	if (bio->b_op == BIO_WRITE)
		write = 1;
	error = 0;
	if (bio->b_op == BIO_FLUSH) {
		/* A flush has nothing left to write. */
		error = 0;
	} else if (bio->b_op != BIO_READ && bio->b_op != BIO_WRITE) {
		/* Discards and the rest are not served. */
		error = EOPNOTSUPP;
	} else {
		/* Moves every block of the request, stopping at the first failure. */
		mutex_lock(&unit->lock);

		/* A card marked failed is not tried again. */
		if (unit->failed)
			error = EIO;

		/*
		 * Moves the blocks in order, a run per command.  The disk
		 * layer splits at d_max_transfer_blocks, so a request is
		 * normally one run; a longer one is still served in runs.
		 */
		run = 0;
		for (done = 0;
		     done < bio->b_block_count && error == 0;
		     done += run) {
			/* The run, or what is left of the request when that is less. */
			run = bio->b_block_count - done;
			if (run > DW_MMC_RUN_BLOCKS)
				run = DW_MMC_RUN_BLOCKS;

			/* Moves the run, recovering the card if it fails. */
			error = transfer_run_recovering(unit,
							write,
							bio->b_mapped_block + done,
							run,
							data + (size_t)done * DW_MMC_BLOCK_BYTES);
		}

		mutex_unlock(&unit->lock);
	}

	/* Completes the request with the bytes it moved. */
	transferred = 0;
	if (error == 0)
		transferred = (size_t)bio->b_block_count * DW_MMC_BLOCK_BYTES;
	bio_complete(bio, error, transferred);

	/* Succeeded: the request was completed, with its own error if any. */
	return 0;
}

/* Answers a control request to the card's disk; the card has none. */
static int
disk_control(
	struct disk *disk,
	unsigned long request,
	void *argument)
{
	UNUSED_PARAMETER(disk);
	UNUSED_PARAMETER(request);
	UNUSED_PARAMETER(argument);

	/* Refuses every request. */
	return EOPNOTSUPP;
}
