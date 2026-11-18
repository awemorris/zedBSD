/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Amlogic SD/eMMC controller with an SD memory card.
 *
 * The driver runs the controller in its direct mode: one command at a
 * time is written to the command registers, the data moves by the
 * controller's DMA through one bounce buffer the glue provides below
 * 4 GiB, and the driver polls the status register; it takes no interrupt
 * and uses no descriptor chain.  It brings one SD memory card up from the
 * idle state (identification at 400 kHz, the bus widened to four lines,
 * high speed when the card and the slot allow it) and publishes it as one
 * disk.  A request is moved with one single-block or multiple-block
 * command per run, a multiple-block run being ended with its own stop
 * command.  A run that fails is stopped, the card is brought back to the
 * transfer state, and the run is tried again; a run that fails at high
 * speed with a CRC error moves the clock down to the default speed.
 *
 * The board's glue switches the controller's gate, pins and the card's
 * supplies on, selects the controller's clock source when asked, and
 * hands over the mapped registers and the bounce buffer
 * (struct drv_meson_mmc_config).
 */

#include "drivers/mmc/meson-mmc.h"

#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/disk.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <uapi/errno.h>
#include <uapi/sysctl.h>

/*
 * The controller's registers.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * include/linux/amlogic/aml_sd.h (SD_EMMC_CLOCK, SD_EMMC_START,
 * SD_EMMC_CFG, SD_EMMC_STATUS, SD_EMMC_IRQ_EN, SD_EMMC_CMD_CFG,
 * SD_EMMC_CMD_ARG, SD_EMMC_CMD_DAT, SD_EMMC_CMD_RSP .. SD_EMMC_CMD_RSP3,
 * SD_EMMC_ADDR64) and drivers/mmc/host/meson-gx-mmc.c
 * (meson_mmc_cfg_init: the response mask register at 0x150).
 */
#define MESON_MMC_CLOCK			0x000U
#define MESON_MMC_START			0x040U
#define MESON_MMC_CONFIG		0x044U
#define MESON_MMC_STATUS		0x048U
#define MESON_MMC_INTERRUPTS		0x04cU
#define MESON_MMC_COMMAND		0x050U
#define MESON_MMC_ARGUMENT		0x054U
#define MESON_MMC_DATA_ADDRESS		0x058U
#define MESON_MMC_RESPONSE0		0x05cU
#define MESON_MMC_RESPONSE1		0x060U
#define MESON_MMC_RESPONSE2		0x064U
#define MESON_MMC_RESPONSE3		0x068U
#define MESON_MMC_ADDRESS_HIGH		0x0a0U
#define MESON_MMC_RESPONSE_MASK		0x150U

/*
 * The fields of the clock register: the divider of the source clock, the
 * phases of the core, transmit and receive clocks, and the bit that keeps
 * the clock running.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * include/linux/amlogic/aml_sd.h (CLK_DIV_MASK, CLK_CORE_PHASE_MASK,
 * CLK_PHASE_180, CLK_V3_ALWAYS_ON) and drivers/mmc/host/meson-gx-mmc.c
 * (meson_mmc_clk_init: always on, the largest divider, core phase 180).
 */
#define MESON_MMC_CLOCK_DIVIDER_MASK	0x0000003fU
#define MESON_MMC_CLOCK_CORE_PHASE_180	0x00000200U
#define MESON_MMC_CLOCK_ALWAYS_ON	0x10000000U

/* The largest divider the clock register holds (one-based: 1 passes the source through). */
#define MESON_MMC_DIVIDER_MAX		63U

/*
 * The fields of the configuration register: the bus width, the block
 * length as a power of two, the response timeout and the gap between
 * commands as powers of two of card clocks, and the clock's stop and
 * automatic gating.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * include/linux/amlogic/aml_sd.h (CFG_BUS_WIDTH_MASK, CFG_BUS_WIDTH_4,
 * CFG_BLK_LEN_MASK, CFG_RESP_TIMEOUT_MASK, CFG_RC_CC_MASK, CFG_STOP_CLOCK,
 * CFG_AUTO_CLK, SD_EMMC_CFG_RESP_TIMEOUT 256, SD_EMMC_CFG_CMD_GAP 16,
 * SD_EMMC_CFG_BLK_SIZE 512).
 */
#define MESON_MMC_CONFIG_WIDTH_MASK	0x00000003U
#define MESON_MMC_CONFIG_WIDTH_1	0x00000000U
#define MESON_MMC_CONFIG_WIDTH_4	0x00000001U
#define MESON_MMC_CONFIG_BLOCK_SHIFT	4U
#define MESON_MMC_CONFIG_BLOCK_MASK	0x000000f0U
#define MESON_MMC_CONFIG_TIMEOUT_SHIFT	8U
#define MESON_MMC_CONFIG_GAP_SHIFT	12U
#define MESON_MMC_CONFIG_STOP_CLOCK	0x00400000U
#define MESON_MMC_CONFIG_AUTO_CLOCK	0x00800000U
#define MESON_MMC_RESPONSE_TIMEOUT_LOG2	8U
#define MESON_MMC_COMMAND_GAP_LOG2	4U
#define MESON_MMC_BLOCK_LOG2		9U

/*
 * The bits of the status register, which the driver clears by writing
 * them: the CRC errors of the data lines, the descriptor and the
 * response, the timeouts, the end of the command, the busy flags and the
 * levels of the data lines.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * include/linux/amlogic/aml_sd.h (IRQ_RXD_ERR_MASK, IRQ_TXD_ERR,
 * IRQ_DESC_ERR, IRQ_RESP_ERR, IRQ_RESP_TIMEOUT, IRQ_DESC_TIMEOUT,
 * IRQ_END_OF_CHAIN, STATUS_BUSY, STATUS_DESC_BUSY, STATUS_DATI) and
 * drivers/mmc/host/meson-gx-mmc.c (the 0x7fff acknowledge).
 */
#define MESON_MMC_STATUS_CRC_ERRORS	0x000007ffU
#define MESON_MMC_STATUS_RESPONSE_TIMEOUT 0x00000800U
#define MESON_MMC_STATUS_DESC_TIMEOUT	0x00001000U
#define MESON_MMC_STATUS_TIMEOUTS	0x00001800U
#define MESON_MMC_STATUS_END_OF_CHAIN	0x00002000U
#define MESON_MMC_STATUS_ACKNOWLEDGE	0x00007fffU
#define MESON_MMC_STATUS_DATA0		0x00010000U
#define MESON_MMC_STATUS_DESC_BUSY	0x40000000U
#define MESON_MMC_STATUS_BUSY		0x80000000U

/* The status bits that end a command: its end or any of its errors. */
#define MESON_MMC_STATUS_FINISHED	(MESON_MMC_STATUS_END_OF_CHAIN | \
					 MESON_MMC_STATUS_CRC_ERRORS | \
					 MESON_MMC_STATUS_TIMEOUTS)

/*
 * The fields of the command configuration register.
 *
 * Values referenced: Hardkernel common_drivers odroids7d-6.12.y @34daba9
 * include/linux/amlogic/aml_sd.h (CMD_CFG_LENGTH_MASK, CMD_CFG_BLOCK_MODE,
 * CMD_CFG_R1B, CMD_CFG_END_OF_CHAIN, CMD_CFG_TIMEOUT_MASK, CMD_CFG_NO_RESP,
 * CMD_CFG_DATA_IO, CMD_CFG_DATA_WR, CMD_CFG_RESP_NOCRC, CMD_CFG_RESP_128,
 * CMD_CFG_RESP_NUM, CMD_CFG_CMD_INDEX_MASK, CMD_CFG_OWNER, CMD_DATA_MASK)
 * and SD_EMMC_CMD_TIMEOUT 1024 ms, SD_EMMC_CMD_TIMEOUT_DATA 4096 ms.
 */
#define MESON_MMC_COMMAND_LENGTH_MASK	0x000001ffU
#define MESON_MMC_COMMAND_BLOCK_MODE	0x00000200U
#define MESON_MMC_COMMAND_R1B		0x00000400U
#define MESON_MMC_COMMAND_END_OF_CHAIN	0x00000800U
#define MESON_MMC_COMMAND_TIMEOUT_SHIFT	12U
#define MESON_MMC_COMMAND_NO_RESPONSE	0x00010000U
#define MESON_MMC_COMMAND_DATA		0x00040000U
#define MESON_MMC_COMMAND_WRITE		0x00080000U
#define MESON_MMC_COMMAND_NO_CRC	0x00100000U
#define MESON_MMC_COMMAND_LONG		0x00200000U
#define MESON_MMC_COMMAND_RESPONSE	0x00400000U
#define MESON_MMC_COMMAND_INDEX_SHIFT	24U
#define MESON_MMC_COMMAND_OWNER		0x80000000U
#define MESON_MMC_COMMAND_TIMEOUT_LOG2	10U
#define MESON_MMC_DATA_TIMEOUT_LOG2	12U
#define MESON_MMC_DATA_ADDRESS_MASK	0xfffffffcU

/* The upper DMA address bytes of the descriptor, data and response in the high address register. */
#define MESON_MMC_ADDRESS_HIGH_FIELDS	0x00ffffffU

/* The most blocks the length field of one block-mode command counts. */
#define MESON_MMC_COMMAND_BLOCKS_MAX	511U

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
 * MMC_OCR_HCS, MMC_OCR_3_2V_3_3V, MMC_OCR_3_3V_3_4V,
 * MMC_R1_READY_FOR_DATA, SD_ARG_BUS_WIDTH_4).
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
#define SD_INTERFACE_ECHO_MASK		0xfffU

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
#define SD_SWITCH_GROUP1_MASK		0x0000000fU

/* The bytes of one block, the only block length the driver uses. */
#define MESON_MMC_BLOCK_BYTES		512U

/*
 * The most blocks one run moves: 64 KiB, the largest request the disk
 * layer hands down, when the bounce buffer holds that much.
 */
#define MESON_MMC_RUN_BLOCKS		128U

/*
 * How many times one run is tried, with a recovery between the tries, and
 * how many times one recovery is tried before the card is taken as gone.
 * A run of several blocks that fails every try is then moved a block at a
 * time, each block with its own tries.
 *
 * Value referenced: Linux v7.3 drivers/mmc/core/block.c (MMC_MAX_RETRIES
 * 5, and a failed multiple-block read retried a sector at a time).
 */
#define MESON_MMC_RUN_TRIES		5U
#define MESON_MMC_RECOVER_TRIES		3U

/* The clocks the card runs at while identified, by default and at high speed. */
#define MESON_MMC_IDENTIFY_HZ		400000ULL
#define MESON_MMC_DEFAULT_HZ		25000000ULL
#define MESON_MMC_HIGH_SPEED_HZ		50000000ULL

/* How long the driver waits for the controller and the card, in microseconds. */
#define MESON_MMC_COMMAND_WAIT_US	1000000ULL
#define MESON_MMC_DATA_WAIT_US		4000000ULL
#define MESON_MMC_IDLE_WAIT_US		10000ULL
#define MESON_MMC_BUSY_WAIT_US		2000000ULL
#define MESON_MMC_READY_WAIT_US		1000000ULL
#define MESON_MMC_READY_RETRY_US	10000ULL
#define MESON_MMC_CLOCK_SETTLE_US	10ULL
#define MESON_MMC_SWITCH_SETTLE_US	25ULL

/* The capacity above which a version 2 card is an SDXC card. */
#define MESON_MMC_SDHC_MAX_BLOCKS	(32ULL * 1024ULL * 1024ULL * 2ULL)

/* Marks a parameter that the disk layer passes but this driver does not need. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/*
 * The kinds of response a command expects, which decide how the command
 * configuration asks for it and how it is read back.
 */
enum meson_mmc_response {
	/* No response (CMD0). */
	MESON_MMC_RESPONSE_NONE = 0,

	/* A 48-bit response with its CRC checked (R1, R6, R7). */
	MESON_MMC_RESPONSE_SHORT,

	/* A 48-bit response whose CRC field is not a CRC (R3). */
	MESON_MMC_RESPONSE_SHORT_NO_CRC,

	/* A 48-bit response after which the card holds the data line busy (R1b). */
	MESON_MMC_RESPONSE_BUSY,

	/* A 136-bit response (R2). */
	MESON_MMC_RESPONSE_LONG
};

/*
 * One command to the card, with the data it moves, as one call to
 * send_command() takes it and fills its response in.
 */
struct meson_mmc_command {
	uint32_t index;
	uint32_t argument;
	enum meson_mmc_response response_kind;

	/* The data the command moves; length zero moves none. */
	uint8_t *data;
	uint32_t length;
	uint32_t block_size;
	int write;

	/* The response, word 0 holding bits 31:0. */
	uint32_t response[4];
};

/*
 * One controller and the card in its slot.
 *
 * A unit is claimed by an attach and kept for the life of the system.
 * Its lock serializes the commands once the disk is published, since the
 * controller has one set of command registers and one bounce buffer;
 * before that only the attach touches it.
 */
struct meson_mmc_unit {
	/* Whether the unit is claimed by an attach. */
	int in_use;

	/* The mapped registers and the bounce buffer. */
	volatile uint8_t *registers;
	uint8_t *bounce;
	uint64_t bounce_bus_address;
	uint32_t bounce_bytes;

	/* The clock sources and the glue's hook that selects one. */
	uint64_t slow_rate;
	uint64_t fast_rate;
	int (*select_source)(void *context, enum drv_meson_mmc_source source);
	void *context;

	/* The limits of the slot. */
	uint64_t max_frequency;
	uint32_t slot_bus_width;
	int high_speed_allowed;

	/* The most blocks one run moves, which the bounce buffer bounds. */
	uint32_t run_blocks;

	/* What the attach found; card.rca is the card's address. */
	struct drv_meson_mmc_card card;

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

	/* The status register the last failed command left, for the failure report. */
	uint32_t last_status;

	/* Failed tries and runs that moved only a block at a time, since the attach, for the log. */
	uint64_t errors;
	uint64_t split_runs;
};

/*
 * The controllers the driver serves.
 *
 * A slot is claimed by drv_meson_mmc_attach() and never released; the
 * disk the slot publishes points back at it through d_data.
 */
static struct meson_mmc_unit meson_mmc_units[DRV_MESON_MMC_UNIT_MAX];

static uint32_t read_register(const struct meson_mmc_unit *unit, uint32_t offset);
static void write_register(struct meson_mmc_unit *unit, uint32_t offset, uint32_t value);
static int start_deadline(uint64_t microseconds, uint64_t *deadline);
static int deadline_passed(uint64_t deadline);
static void pause_microseconds(uint64_t microseconds);
static int setup_controller(struct meson_mmc_unit *unit);
static int set_card_clock(struct meson_mmc_unit *unit, uint64_t target);
static void set_bus_width(struct meson_mmc_unit *unit, uint32_t width);
static int stop_controller(struct meson_mmc_unit *unit);
static int wait_data_idle(struct meson_mmc_unit *unit);
static uint32_t command_value(const struct meson_mmc_command *command);
static int send_command(struct meson_mmc_unit *unit, struct meson_mmc_command *command);
static int send_app_command(struct meson_mmc_unit *unit, struct meson_mmc_command *command);
static int wait_command_done(struct meson_mmc_unit *unit, uint64_t microseconds, uint32_t *status);
static void read_response(const struct meson_mmc_unit *unit, struct meson_mmc_command *command);
static void prepare_command(struct meson_mmc_command *command, uint32_t index, uint32_t argument, enum meson_mmc_response response_kind);
static uint32_t response_bits(const uint32_t *response, uint32_t start, uint32_t width);
static int start_card(struct meson_mmc_unit *unit);
static int identify_card(struct meson_mmc_unit *unit);
static int wait_card_powered(struct meson_mmc_unit *unit, uint32_t request, uint32_t *ocr);
static int decode_csd(struct meson_mmc_unit *unit, const uint32_t *csd);
static void decode_cid(struct meson_mmc_unit *unit, const uint32_t *cid);
static int select_card(struct meson_mmc_unit *unit);
static int configure_bus(struct meson_mmc_unit *unit);
static int read_scr(struct meson_mmc_unit *unit, uint32_t *specification, uint32_t *bus_widths);
static int switch_high_speed(struct meson_mmc_unit *unit, int *switched);
static int wait_card_ready(struct meson_mmc_unit *unit);
static int transfer_run(struct meson_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int transfer_run_recovering(struct meson_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int transfer_run_tries(struct meson_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int transfer_blocks(struct meson_mmc_unit *unit, int write, uint64_t block, uint32_t count, uint8_t *data);
static int recover_card(struct meson_mmc_unit *unit);
static int recover_card_tries(struct meson_mmc_unit *unit, const char *during);
static int slow_bus(struct meson_mmc_unit *unit);
static int publish_disk(struct meson_mmc_unit *unit, unsigned unit_number);
static void log_card(const struct meson_mmc_unit *unit);
static const char *card_kind(const struct drv_meson_mmc_card *card);
static int disk_submit(struct disk *disk, struct bio *bio);
static int disk_control(struct disk *disk, unsigned long request, void *argument);

/* The operations of an SD card's disk. */
static const struct disk_ops meson_mmc_disk_ops = {
	.submit = disk_submit,
	.ioctl = disk_control
};

/*
 * Brings up the card in a controller's slot and publishes it as a disk.
 *
 * Returns 0 with the disk, ENODEV when the slot is empty, EBUSY when the
 * unit number is taken, EINVAL for a description the driver cannot use,
 * or the errno of the step that failed; a failed attach leaves no disk.
 */
int
drv_meson_mmc_attach(
	const struct drv_meson_mmc_config *config,
	struct disk **disk)
{
	struct meson_mmc_unit *unit;
	int present;
	int error;

	/* Refuses a missing description or nowhere to put the disk. */
	if (config == NULL)
		return EINVAL;
	if (disk == NULL)
		return EINVAL;

	/* Refuses a description without registers, a clock hook or a bounce buffer of one block. */
	if (config->registers == NULL)
		return EINVAL;
	if (config->select_source == NULL)
		return EINVAL;
	if (config->slow_rate == 0)
		return EINVAL;
	if (config->bounce == NULL)
		return EINVAL;
	if (config->bounce_bytes < MESON_MMC_BLOCK_BYTES)
		return EINVAL;

	/* Refuses a bounce buffer the controller's 32-bit data address cannot reach. */
	if (config->bounce_bus_address + config->bounce_bytes > 0x100000000ULL)
		return EINVAL;

	/* Refuses a unit number past the table. */
	if (config->unit >= DRV_MESON_MMC_UNIT_MAX)
		return EBUSY;

	/* Claims the unit the glue numbered. */
	unit = &meson_mmc_units[config->unit];
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
	kern_memset(unit, 0, sizeof(*unit));
	unit->in_use = 1;
	unit->registers = config->registers;
	unit->bounce = config->bounce;
	unit->bounce_bus_address = config->bounce_bus_address;
	unit->bounce_bytes = config->bounce_bytes;
	unit->slow_rate = config->slow_rate;
	unit->fast_rate = config->fast_rate;
	unit->select_source = config->select_source;
	unit->context = config->context;
	unit->max_frequency = config->max_frequency;
	unit->slot_bus_width = config->bus_width;
	unit->high_speed_allowed = config->high_speed;

	/* Bounds a run by what the bounce buffer holds. */
	unit->run_blocks = config->bounce_bytes / MESON_MMC_BLOCK_BYTES;
	if (unit->run_blocks > MESON_MMC_RUN_BLOCKS)
		unit->run_blocks = MESON_MMC_RUN_BLOCKS;

	/* Puts the controller in a known state with the clock at 400 kHz. */
	error = setup_controller(unit);
	if (error != 0) {
		kern_logf("meson-mmc: controller %u unusable (%d)\n", config->unit, error);
		unit->in_use = 0;
		return error;
	}

	/* Brings the card from the idle state to the transfer state. */
	error = start_card(unit);
	if (error != 0) {
		kern_logf("meson-mmc: card in slot %u unusable (%d, status %x)\n",
			  config->unit,
			  error,
			  unit->last_status);
		unit->in_use = 0;
		return error;
	}

	/* Publishes the card. */
	error = publish_disk(unit, config->unit);
	if (error != 0) {
		kern_logf("meson-mmc: disk for slot %u unavailable (%d)\n", config->unit, error);
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
drv_meson_mmc_card_info(
	unsigned unit_number,
	struct drv_meson_mmc_card *card)
{
	const struct meson_mmc_unit *unit;

	/* Refuses nowhere to copy to and a unit outside the table. */
	if (card == NULL)
		return EINVAL;
	if (unit_number >= DRV_MESON_MMC_UNIT_MAX)
		return ENODEV;

	/* Refuses a unit without a published card. */
	unit = &meson_mmc_units[unit_number];
	if (!unit->in_use)
		return ENODEV;
	if (unit->disk == NULL)
		return ENODEV;

	/* Succeeded: the card's description. */
	*card = unit->card;
	return 0;
}

/*
 * Reads blocks of a published card, for the glue's check of what the
 * card holds; the disk's own requests go through the disk layer.
 *
 * Returns 0, ENODEV for a unit without a published card, or the errno of
 * the read.
 */
int
drv_meson_mmc_read(
	unsigned unit_number,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	struct meson_mmc_unit *unit;
	int error;

	/* Refuses nowhere to read into and a unit outside the table. */
	if (data == NULL)
		return EINVAL;
	if (unit_number >= DRV_MESON_MMC_UNIT_MAX)
		return ENODEV;

	/* Refuses a unit without a published card. */
	unit = &meson_mmc_units[unit_number];
	if (!unit->in_use)
		return ENODEV;
	if (unit->disk == NULL)
		return ENODEV;

	/* Reads the blocks as a request of the disk would. */
	mutex_lock(&unit->lock);

	error = EIO;
	if (!unit->failed)
		error = transfer_blocks(unit, 0, block, count, data);

	mutex_unlock(&unit->lock);

	/* Reports why the blocks were not read. */
	if (error != 0)
		return error;

	/* Succeeded: the blocks are in data. */
	return 0;
}

/* Reads one register of the controller. */
static uint32_t
read_register(
	const struct meson_mmc_unit *unit,
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
	struct meson_mmc_unit *unit,
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
	if (!counted)
		return ENODEV;
	if (frequency == 0)
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
 * Stops whatever the bootloader left running and sets the controller up
 * for direct commands: the bus one line wide, 512-byte blocks, the
 * response timeout and the command gap, no interrupts, and the clock at
 * 400 kHz.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/mmc/host/meson-gx-mmc.c (meson_mmc_clk_init,
 * meson_mmc_cfg_init, meson_mmc_probe: the clock register's defaults, the
 * configuration, the response mask, the acknowledged status).
 */
static int
setup_controller(
	struct meson_mmc_unit *unit)
{
	uint32_t high;
	int error;

	/* Stops a descriptor chain the bootloader may have left. */
	write_register(unit, MESON_MMC_START, 0);

	/* Runs the clock at its slowest, with the core phase at 180 degrees. */
	write_register(unit,
		       MESON_MMC_CLOCK,
		       MESON_MMC_CLOCK_ALWAYS_ON |
		       MESON_MMC_CLOCK_CORE_PHASE_180 |
		       MESON_MMC_CLOCK_DIVIDER_MASK);

	/* One data line, 512-byte blocks, a 256-clock response timeout and a 16-clock gap. */
	write_register(unit,
		       MESON_MMC_CONFIG,
		       MESON_MMC_CONFIG_WIDTH_1 |
		       (MESON_MMC_BLOCK_LOG2 << MESON_MMC_CONFIG_BLOCK_SHIFT) |
		       (MESON_MMC_RESPONSE_TIMEOUT_LOG2 << MESON_MMC_CONFIG_TIMEOUT_SHIFT) |
		       (MESON_MMC_COMMAND_GAP_LOG2 << MESON_MMC_CONFIG_GAP_SHIFT));

	/* Checks every response bit, as the controller does by default after a reset. */
	write_register(unit, MESON_MMC_RESPONSE_MASK, 0);

	/*
	 * Clears the upper address bytes of the data, descriptor and response
	 * DMA, so the bounce buffer's 32-bit address is the whole address
	 * whether or not the 64-bit bus mode is on.
	 */
	high = read_register(unit, MESON_MMC_ADDRESS_HIGH);
	high &= ~MESON_MMC_ADDRESS_HIGH_FIELDS;
	write_register(unit, MESON_MMC_ADDRESS_HIGH, high);

	/* Takes no interrupt: the driver polls the status register. */
	write_register(unit, MESON_MMC_INTERRUPTS, 0);

	/* Clears what the bootloader's last command left in the status. */
	write_register(unit, MESON_MMC_STATUS, MESON_MMC_STATUS_ACKNOWLEDGE);

	/* Runs the card clock at the identification rate. */
	error = set_card_clock(unit, MESON_MMC_IDENTIFY_HZ);
	if (error != 0)
		return error;

	/* Gives the card the clocks it needs before its first command. */
	pause_microseconds(1000ULL);

	/* Succeeded: the controller takes commands. */
	unit->card.bus_width = 1U;
	return 0;
}

/*
 * Sets the card clock to the fastest rate at or below target: from the
 * slow source up to the default speed, from the fast source above it.
 *
 * The clock is stopped while the source and the divider change, so the
 * card never sees a short pulse.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/mmc/host/meson-gx-mmc.c (meson_mmc_clk_set: stop the
 * clock, clear the automatic gating, change the rate, start the clock;
 * no_pxp_clk_set: the legacy timing from clkin0, the crystal).
 */
static int
set_card_clock(
	struct meson_mmc_unit *unit,
	uint64_t target)
{
	enum drv_meson_mmc_source source;
	uint64_t rate;
	uint64_t divider;
	uint32_t clock;
	uint32_t config;
	int error;

	/* Refuses a rate of zero, which no divider gives. */
	if (target == 0)
		return EINVAL;

	/*
	 * Runs identification and the default speed from the slow source, as
	 * the vendor's driver runs its legacy timing from the crystal, and
	 * high speed from the fast one.
	 */
	source = DRV_MESON_MMC_SOURCE_SLOW;
	rate = unit->slow_rate;
	if (target > MESON_MMC_DEFAULT_HZ && unit->fast_rate != 0) {
		source = DRV_MESON_MMC_SOURCE_FAST;
		rate = unit->fast_rate;
	}

	/* Divides the source down to the target or just below it. */
	divider = (rate + target - 1ULL) / target;
	if (divider == 0)
		divider = 1;

	/* Refuses a target the divider cannot reach from the source. */
	if (divider > MESON_MMC_DIVIDER_MAX)
		return ERANGE;

	/* Stops the clock and keeps it from gating itself. */
	config = read_register(unit, MESON_MMC_CONFIG);
	config |= MESON_MMC_CONFIG_STOP_CLOCK;
	config &= ~MESON_MMC_CONFIG_AUTO_CLOCK;
	write_register(unit, MESON_MMC_CONFIG, config);

	/* Has the glue feed the controller from the chosen source. */
	error = unit->select_source(unit->context, source);
	if (error != 0)
		return error;

	/* Writes the divider, keeping the phases and the always-on bit. */
	clock = read_register(unit, MESON_MMC_CLOCK);
	clock &= ~MESON_MMC_CLOCK_DIVIDER_MASK;
	clock |= (uint32_t)divider;
	clock |= MESON_MMC_CLOCK_ALWAYS_ON;
	write_register(unit, MESON_MMC_CLOCK, clock);

	/* Starts the clock again. */
	config &= ~MESON_MMC_CONFIG_STOP_CLOCK;
	write_register(unit, MESON_MMC_CONFIG, config);

	/* Lets the new clock settle before the next command. */
	pause_microseconds(MESON_MMC_CLOCK_SETTLE_US);

	/* Succeeded: the card runs at the new rate. */
	unit->card.card_clock = rate / divider;
	return 0;
}

/* Sets how many data lines the controller drives: 1 or 4. */
static void
set_bus_width(
	struct meson_mmc_unit *unit,
	uint32_t width)
{
	uint32_t config;

	/* Writes the width field of the configuration. */
	config = read_register(unit, MESON_MMC_CONFIG);
	config &= ~MESON_MMC_CONFIG_WIDTH_MASK;
	if (width == 4U) {
		config |= MESON_MMC_CONFIG_WIDTH_4;
	} else {
		config |= MESON_MMC_CONFIG_WIDTH_1;
	}
	write_register(unit, MESON_MMC_CONFIG, config);

	/* Records the width for the boot log. */
	unit->card.bus_width = width;
}

/*
 * Stops a command the controller is still running after an error, and
 * waits until it is idle.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/mmc/host/meson-gx-mmc.c (meson_mmc_request: the start
 * register cleared to stop execution) and Linux v7.3
 * drivers/mmc/host/meson-gx-mmc.c (meson_mmc_wait_desc_stop: the busy and
 * descriptor busy bits polled until they clear).
 */
static int
stop_controller(
	struct meson_mmc_unit *unit)
{
	uint64_t deadline;
	uint32_t status;
	int passed;
	int error;

	/* Clears the start register, which ends a running chain. */
	write_register(unit, MESON_MMC_START, 0);

	/* Sets how long the controller has to go idle. */
	error = start_deadline(MESON_MMC_IDLE_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Waits until neither the command nor a descriptor is busy. */
	for (;;) {
		/* Reads the busy flags. */
		status = read_register(unit, MESON_MMC_STATUS);
		if ((status & (MESON_MMC_STATUS_BUSY | MESON_MMC_STATUS_DESC_BUSY)) == 0)
			break;

		/* A controller that stays busy is reported. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Clears the status the stopped command left. */
	write_register(unit, MESON_MMC_STATUS, MESON_MMC_STATUS_ACKNOWLEDGE);

	/* Succeeded: the controller is idle. */
	return 0;
}

/*
 * Waits until the card releases the first data line, which it holds low
 * while it is busy after an R1b command.
 *
 * Procedure referenced: Linux v7.3 drivers/mmc/host/meson-gx-mmc.c
 * (meson_mmc_card_busy: the data lines' levels in the status register).
 */
static int
wait_data_idle(
	struct meson_mmc_unit *unit)
{
	uint64_t deadline;
	uint32_t status;
	int passed;
	int error;

	/* Sets how long the card may stay busy. */
	error = start_deadline(MESON_MMC_BUSY_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Waits for the line to go high. */
	for (;;) {
		/* Reads the data lines' levels. */
		status = read_register(unit, MESON_MMC_STATUS);
		if ((status & MESON_MMC_STATUS_DATA0) != 0)
			break;

		/* A card that stays busy is reported. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the card is no longer busy. */
	return 0;
}

/*
 * Builds the command configuration of a command: its index, the response
 * it expects, its data and its timeout.
 *
 * A data phase longer than one block runs in block mode with the count of
 * blocks in the length field; a shorter one counts bytes, where a length
 * of 512 is written as zero, the value the field gives that length.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/mmc/host/meson-gx-mmc.c (meson_mmc_start_cmd,
 * meson_mmc_set_response_bits).
 */
static uint32_t
command_value(
	const struct meson_mmc_command *command)
{
	uint32_t value;

	/* Names the command; the owner bit gives the command to the controller. */
	value = command->index << MESON_MMC_COMMAND_INDEX_SHIFT;
	value |= MESON_MMC_COMMAND_OWNER;
	value |= MESON_MMC_COMMAND_END_OF_CHAIN;

	/* Asks for the response the command expects. */
	switch (command->response_kind) {
	case MESON_MMC_RESPONSE_NONE:
		value |= MESON_MMC_COMMAND_NO_RESPONSE;
		break;
	case MESON_MMC_RESPONSE_SHORT:
		value |= MESON_MMC_COMMAND_RESPONSE;
		break;
	case MESON_MMC_RESPONSE_SHORT_NO_CRC:
		value |= MESON_MMC_COMMAND_RESPONSE;
		value |= MESON_MMC_COMMAND_NO_CRC;
		break;
	case MESON_MMC_RESPONSE_BUSY:
		value |= MESON_MMC_COMMAND_RESPONSE;
		value |= MESON_MMC_COMMAND_R1B;
		break;
	case MESON_MMC_RESPONSE_LONG:
		value |= MESON_MMC_COMMAND_RESPONSE;
		value |= MESON_MMC_COMMAND_LONG;
		break;
	}

	/* A command without data waits for its response alone. */
	if (command->length == 0) {
		value |= MESON_MMC_COMMAND_TIMEOUT_LOG2 << MESON_MMC_COMMAND_TIMEOUT_SHIFT;
		return value;
	}

	/* A command with data waits longer, in the direction it moves. */
	value |= MESON_MMC_COMMAND_DATA;
	value |= MESON_MMC_DATA_TIMEOUT_LOG2 << MESON_MMC_COMMAND_TIMEOUT_SHIFT;
	if (command->write)
		value |= MESON_MMC_COMMAND_WRITE;

	/* Counts blocks for several blocks, bytes for one. */
	if (command->length > command->block_size) {
		value |= MESON_MMC_COMMAND_BLOCK_MODE;
		value |= (command->length / command->block_size) & MESON_MMC_COMMAND_LENGTH_MASK;
	} else {
		value |= command->length & MESON_MMC_COMMAND_LENGTH_MASK;
	}

	/* Reports the configuration. */
	return value;
}

/*
 * Sends one command and moves its data through the bounce buffer.
 *
 * Returns 0 with the response, ETIMEDOUT when the card did not answer or
 * the data did not come, EIO for a CRC error, or EINVAL for data the
 * bounce buffer cannot hold.  The status that ended a failed command is
 * kept in last_status.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/mmc/host/meson-gx-mmc.c (meson_mmc_start_cmd: the
 * configuration, the data address and the response address written before
 * the argument, whose write starts the command; meson_mmc_irq,
 * meson_mmc_read_resp).
 */
static int
send_command(
	struct meson_mmc_unit *unit,
	struct meson_mmc_command *command)
{
	uint64_t wait;
	uint32_t value;
	uint32_t status;
	uint32_t block_log2;
	uint32_t config;
	int error;

	/* Refuses data the bounce buffer cannot hold or a block length the controller cannot set. */
	if (command->length > unit->bounce_bytes)
		return EINVAL;
	if (command->length > command->block_size && command->length / command->block_size > MESON_MMC_COMMAND_BLOCKS_MAX)
		return EINVAL;

	/* Sets the block length of a block-mode transfer: a power of two up to 512 bytes. */
	if (command->length > command->block_size) {
		/* Finds the power of two the block size is. */
		block_log2 = 0;
		while ((1U << block_log2) < command->block_size)
			block_log2++;
		if ((1U << block_log2) != command->block_size)
			return EINVAL;
		if (block_log2 > MESON_MMC_BLOCK_LOG2)
			return EINVAL;

		/* Writes it when it differs from the one set. */
		config = read_register(unit, MESON_MMC_CONFIG);
		if (((config & MESON_MMC_CONFIG_BLOCK_MASK) >> MESON_MMC_CONFIG_BLOCK_SHIFT) != block_log2) {
			config &= ~MESON_MMC_CONFIG_BLOCK_MASK;
			config |= block_log2 << MESON_MMC_CONFIG_BLOCK_SHIFT;
			write_register(unit, MESON_MMC_CONFIG, config);
		}
	}

	/*
	 * Puts a write's data in the bounce buffer and cleans it out of the
	 * caches; for a read, drops the buffer's lines from the caches so none
	 * is written back over what the controller brings.
	 */
	if (command->length != 0) {
		if (command->write) {
			kern_memcpy(unit->bounce, command->data, command->length);
			kern_dcache_clean_range(unit->bounce, command->length);
		} else {
			kern_dcache_invalidate_range(unit->bounce, command->length);
		}
	}

	/* Clears what the last command left in the status. */
	write_register(unit, MESON_MMC_STATUS, MESON_MMC_STATUS_ACKNOWLEDGE);

	/* Writes the command's configuration, its data address and the response's place (the registers). */
	value = command_value(command);
	write_register(unit, MESON_MMC_COMMAND, value);
	write_register(unit, MESON_MMC_DATA_ADDRESS, (uint32_t)unit->bounce_bus_address & MESON_MMC_DATA_ADDRESS_MASK);
	write_register(unit, MESON_MMC_RESPONSE0, 0);

	/* Starts the command by writing its argument, after everything above. */
	kern_io_write_barrier();
	write_register(unit, MESON_MMC_ARGUMENT, command->argument);

	/* Waits for the command and its data to end. */
	wait = MESON_MMC_COMMAND_WAIT_US;
	if (command->length != 0)
		wait = MESON_MMC_DATA_WAIT_US;
	error = wait_command_done(unit, wait, &status);
	if (error != 0) {
		unit->last_status = status;
		(void)stop_controller(unit);
		return error;
	}

	/* A response or data CRC error fails the command. */
	if ((status & MESON_MMC_STATUS_CRC_ERRORS) != 0) {
		unit->last_status = status;
		(void)stop_controller(unit);
		return EIO;
	}

	/* A card that did not answer, or data that never came, fails the command. */
	if ((status & MESON_MMC_STATUS_TIMEOUTS) != 0) {
		unit->last_status = status;
		(void)stop_controller(unit);
		return ETIMEDOUT;
	}

	/* Reads the response the command brought. */
	read_response(unit, command);

	/* Clears the end of the command from the status. */
	write_register(unit, MESON_MMC_STATUS, MESON_MMC_STATUS_ACKNOWLEDGE);

	/* Waits out the busy signal of an R1b command. */
	if (command->response_kind == MESON_MMC_RESPONSE_BUSY) {
		error = wait_data_idle(unit);
		if (error != 0) {
			unit->last_status = read_register(unit, MESON_MMC_STATUS);
			return error;
		}
	}

	/* Takes a read's data out of the bounce buffer, dropping stale lines first. */
	if (command->length != 0 && !command->write) {
		kern_dcache_invalidate_range(unit->bounce, command->length);
		kern_memcpy(command->data, unit->bounce, command->length);
	}

	/* Succeeded: the command ran and its data moved. */
	return 0;
}

/* Sends an application command: CMD55 with the card's address, then the command. */
static int
send_app_command(
	struct meson_mmc_unit *unit,
	struct meson_mmc_command *command)
{
	struct meson_mmc_command application;
	int error;

	/* Tells the card the next command is an application command. */
	prepare_command(&application, SD_APPLICATION, unit->card.rca << 16, MESON_MMC_RESPONSE_SHORT);
	error = send_command(unit, &application);
	if (error != 0)
		return error;

	/* Sends the command itself. */
	error = send_command(unit, command);
	if (error != 0)
		return error;

	/* Succeeded: the application command ran. */
	return 0;
}

/*
 * Polls the status until the command ends or fails.
 *
 * Returns 0 with the status that ended it, or ETIMEDOUT with the last
 * status seen when the controller never reported an end.
 */
static int
wait_command_done(
	struct meson_mmc_unit *unit,
	uint64_t microseconds,
	uint32_t *status)
{
	uint64_t deadline;
	int passed;
	int error;

	/* Sets how long the command may take. */
	*status = 0;
	error = start_deadline(microseconds, &deadline);
	if (error != 0)
		return error;

	/* Reads the status until the end or an error appears. */
	for (;;) {
		/* Reads the status. */
		*status = read_register(unit, MESON_MMC_STATUS);
		if ((*status & MESON_MMC_STATUS_FINISHED) != 0)
			break;

		/* A command that never ends is reported. */
		passed = deadline_passed(deadline);
		if (passed)
			return ETIMEDOUT;
	}

	/* Succeeded: the status that ended the command. */
	return 0;
}

/*
 * Reads a command's response into word 0 (bits 31:0) upward.
 *
 * A long response's four registers hold bits 31:0 in the first one and
 * bits 127:96 in the last, the CRC byte at the bottom as the card sent it.
 *
 * Procedure referenced: Hardkernel common_drivers odroids7d-6.12.y
 * @34daba9 drivers/mmc/host/meson-gx-mmc.c (meson_mmc_read_resp: resp[0],
 * the highest word, from RSP3).
 */
static void
read_response(
	const struct meson_mmc_unit *unit,
	struct meson_mmc_command *command)
{
	/* A command without a response has nothing to read. */
	if (command->response_kind == MESON_MMC_RESPONSE_NONE)
		return;

	/* A short response is the first register. */
	command->response[0] = read_register(unit, MESON_MMC_RESPONSE0);
	if (command->response_kind != MESON_MMC_RESPONSE_LONG)
		return;

	/* A long response fills all four words, lowest first. */
	command->response[1] = read_register(unit, MESON_MMC_RESPONSE1);
	command->response[2] = read_register(unit, MESON_MMC_RESPONSE2);
	command->response[3] = read_register(unit, MESON_MMC_RESPONSE3);
}

/* Fills in a command that moves no data and clears its response. */
static void
prepare_command(
	struct meson_mmc_command *command,
	uint32_t index,
	uint32_t argument,
	enum meson_mmc_response response_kind)
{
	unsigned word;

	/* Names the command, its argument and the response it expects. */
	command->index = index;
	command->argument = argument;
	command->response_kind = response_kind;

	/* Moves no data until the caller says otherwise. */
	command->data = NULL;
	command->length = 0;
	command->block_size = MESON_MMC_BLOCK_BYTES;
	command->write = 0;

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
	struct meson_mmc_unit *unit)
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
	struct meson_mmc_unit *unit)
{
	struct meson_mmc_command command;
	uint32_t request;
	uint32_t ocr;
	unsigned word;
	int error;

	/* Resets the card to the idle state; the command has no response. */
	unit->card.rca = 0;
	prepare_command(&command, SD_GO_IDLE, 0, MESON_MMC_RESPONSE_NONE);
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
			MESON_MMC_RESPONSE_SHORT);
	error = send_command(unit, &command);
	if (error == 0) {
		/* Keeps the echo for the boot log. */
		unit->card.interface_echo = command.response[0] & SD_INTERFACE_ECHO_MASK;

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
	prepare_command(&command, SD_ALL_SEND_CID, 0, MESON_MMC_RESPONSE_LONG);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Keeps the identity for decoding once the capacity is read. */
	for (word = 0; word < 4U; word++)
		unit->card.cid[word] = command.response[word];

	/* Has the card publish its address, which takes it to the stand-by state. */
	prepare_command(&command, SD_SEND_RELATIVE_ADDRESS, 0, MESON_MMC_RESPONSE_SHORT);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Records the address, which every later command names the card by. */
	unit->card.rca = command.response[0] >> 16;

	/* Reads the card's capacity. */
	prepare_command(&command, SD_SEND_CSD, unit->card.rca << 16, MESON_MMC_RESPONSE_LONG);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Keeps the capacity's register for the boot log. */
	for (word = 0; word < 4U; word++)
		unit->card.csd[word] = command.response[word];

	/* Decodes the capacity. */
	error = decode_csd(unit, unit->card.csd);
	if (error != 0)
		return error;

	/* Decodes the identity. */
	decode_cid(unit, unit->card.cid);

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
	struct meson_mmc_unit *unit,
	uint32_t request,
	uint32_t *ocr)
{
	struct meson_mmc_command command;
	uint64_t deadline;
	int passed;
	int error;

	/* Sets how long the card has to power up. */
	error = start_deadline(MESON_MMC_READY_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Repeats the request until the card reports that it has powered up. */
	for (;;) {
		/* Sends the request; the card answers with its conditions. */
		prepare_command(&command, SD_APP_SEND_OPERATING, request, MESON_MMC_RESPONSE_SHORT_NO_CRC);
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
		pause_microseconds(MESON_MMC_READY_RETRY_US);
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
	struct meson_mmc_unit *unit,
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
		unit->card.block_count = bytes / MESON_MMC_BLOCK_BYTES;
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
 * number from its CID; a dot stands for a character that cannot be
 * printed.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/sdmmcreg.h, master
 * 859883f17cb8e20088fece5398547e4264ad30b3 (SD_CID_MID, SD_CID_OID,
 * SD_CID_PNM_CPY, SD_CID_REV, SD_CID_PSN).
 */
static void
decode_cid(
	struct meson_mmc_unit *unit,
	const uint32_t *cid)
{
	uint32_t character;
	uint32_t oem;
	unsigned index;

	/* Reads the manufacturer, the revision and the serial number. */
	unit->card.manufacturer = response_bits(cid, SD_CID_MANUFACTURER_BIT, 8U);
	unit->card.revision = response_bits(cid, SD_CID_REVISION_BIT, 8U);
	unit->card.serial = response_bits(cid, SD_CID_SERIAL_BIT, 32U);

	/* Reads the OEM's two characters, the first in the higher byte. */
	oem = response_bits(cid, SD_CID_OEM_BIT, 16U);
	for (index = 0; index < 2U; index++) {
		character = (oem >> ((1U - index) * 8U)) & 0xffU;
		if (character < 0x20U || character > 0x7eU)
			character = '.';
		unit->card.oem[index] = (char)character;
	}

	/* Terminates the OEM. */
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
	struct meson_mmc_unit *unit)
{
	struct meson_mmc_command command;
	int error;

	/* Selects the card by its address; the card may hold the line busy. */
	prepare_command(&command, SD_SELECT_CARD, unit->card.rca << 16, MESON_MMC_RESPONSE_BUSY);
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Sets the block length; a high-capacity card's is fixed at the same 512 bytes. */
	prepare_command(&command, SD_SET_BLOCK_LENGTH, MESON_MMC_BLOCK_BYTES, MESON_MMC_RESPONSE_SHORT);
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
	struct meson_mmc_unit *unit)
{
	struct meson_mmc_command command;
	uint32_t specification;
	uint32_t bus_widths;
	uint64_t target;
	int switched;
	int error;

	/* Every card runs at the default speed once identified. */
	error = set_card_clock(unit, MESON_MMC_DEFAULT_HZ);
	if (error != 0)
		return error;

	/* Reads which bus widths and which specification the card has. */
	error = read_scr(unit, &specification, &bus_widths);
	if (error != 0)
		return error;

	/* Widens the bus to four lines when the slot wires them and the card has them. */
	if (unit->slot_bus_width >= 4U && (bus_widths & SD_SCR_BUS_WIDTH_4) != 0) {
		/* Widens the card's side first. */
		prepare_command(&command, SD_APP_SET_BUS_WIDTH, SD_BUS_WIDTH_4_ARGUMENT, MESON_MMC_RESPONSE_SHORT);
		error = send_app_command(unit, &command);
		if (error != 0)
			return error;

		/* Then widens the controller's side. */
		set_bus_width(unit, 4U);
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
		/* Takes the lower of high speed's clock and the slot's limit. */
		target = MESON_MMC_HIGH_SPEED_HZ;
		if (unit->max_frequency != 0 && unit->max_frequency < target)
			target = unit->max_frequency;

		/* Gives the card eight clocks at the old rate to finish the switch. */
		pause_microseconds(MESON_MMC_SWITCH_SETTLE_US);

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
	struct meson_mmc_unit *unit,
	uint32_t *specification,
	uint32_t *bus_widths)
{
	struct meson_mmc_command command;
	uint8_t scr[SD_SCR_BYTES];
	int error;

	/* Reads the eight bytes of the SCR as one short block. */
	prepare_command(&command, SD_APP_SEND_SCR, 0, MESON_MMC_RESPONSE_SHORT);
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
	struct meson_mmc_unit *unit,
	int *switched)
{
	struct meson_mmc_command command;
	uint8_t status[SD_SWITCH_STATUS_BYTES];
	uint32_t supported;
	int error;

	/* Asks, without switching, which functions of group 1 the card has. */
	*switched = 0;
	prepare_command(&command, SD_SWITCH_FUNCTION, SD_SWITCH_KEEP, MESON_MMC_RESPONSE_SHORT);
	command.data = status;
	command.length = SD_SWITCH_STATUS_BYTES;
	command.block_size = SD_SWITCH_STATUS_BYTES;
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* A card without high speed stays at the default speed. */
	supported = (uint32_t)status[SD_SWITCH_GROUP1_SUPPORT_BYTE] << 8;
	supported |= (uint32_t)status[SD_SWITCH_GROUP1_SUPPORT_BYTE + 1U];
	if ((supported & (1U << SD_SWITCH_HIGH_SPEED)) == 0)
		return 0;

	/* Switches group 1 to high speed, keeping every other group. */
	prepare_command(&command,
			SD_SWITCH_FUNCTION,
			SD_SWITCH_SET | (SD_SWITCH_KEEP & ~SD_SWITCH_GROUP1_MASK) | SD_SWITCH_HIGH_SPEED,
			MESON_MMC_RESPONSE_SHORT);
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
	struct meson_mmc_unit *unit)
{
	struct meson_mmc_command command;
	uint64_t deadline;
	int passed;
	int error;

	/* Sets how long the card may take to program the blocks. */
	error = start_deadline(MESON_MMC_BUSY_WAIT_US, &deadline);
	if (error != 0)
		return error;

	/* Repeats the status query until the card is ready for data. */
	for (;;) {
		/* Asks the card for its status. */
		prepare_command(&command, SD_SEND_STATUS, unit->card.rca << 16, MESON_MMC_RESPONSE_SHORT);
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
 * block, CMD18 or CMD25 followed by its stop command for more.
 */
static int
transfer_run(
	struct meson_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	struct meson_mmc_command command;
	struct meson_mmc_command stop;
	uint64_t address;
	uint32_t index;
	int error;

	/* A high-capacity card is addressed by block, an older card by byte. */
	address = block;
	if (!unit->card.high_capacity)
		address = block * MESON_MMC_BLOCK_BYTES;

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

	/* Describes the blocks the command moves. */
	prepare_command(&command, index, (uint32_t)address, MESON_MMC_RESPONSE_SHORT);
	command.data = data;
	command.length = count * MESON_MMC_BLOCK_BYTES;
	command.block_size = MESON_MMC_BLOCK_BYTES;
	command.write = write;

	/* Sends the command and moves its data. */
	error = send_command(unit, &command);
	if (error != 0)
		return error;

	/* Ends a multiple-block command; the card may hold the line busy while it does. */
	if (count > 1U) {
		prepare_command(&stop, SD_STOP_TRANSMISSION, 0, MESON_MMC_RESPONSE_BUSY);
		error = send_command(unit, &stop);
		if (error != 0)
			return error;
	}

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
	struct meson_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	const char *direction;
	uint32_t index;
	int error;

	/* Moves the whole run, with its tries. */
	error = transfer_run_tries(unit, write, block, count, data);
	if (error == 0)
		return 0;

	/* A failed card, or a run of one block, has nothing left to try. */
	if (unit->failed)
		return error;
	if (count == 1U)
		return error;

	/* Names the direction for the log. */
	direction = "read";
	if (write)
		direction = "write";

	/* A run of several blocks that never moved goes a block at a time, each with its own tries. */
	unit->split_runs++;
	kern_logf("meson-mmc: %s of %u blocks at %llu moved a block at a time (errors %llu, split runs %llu)\n",
		  direction,
		  count,
		  (unsigned long long)block,
		  (unsigned long long)unit->errors,
		  (unsigned long long)unit->split_runs);
	for (index = 0; index < count; index++) {
		error = transfer_run_tries(unit, write, block + index, 1U, data + (size_t)index * MESON_MMC_BLOCK_BYTES);
		if (error != 0)
			return error;
	}

	/* Succeeded: every block has moved. */
	return 0;
}

/*
 * Tries one run up to MESON_MMC_RUN_TRIES times, recovering the card
 * between the tries; a card that a recovery cannot bring back marks the
 * unit failed.  A CRC error at high speed moves the clock down to the
 * default speed before the next try.
 */
static int
transfer_run_tries(
	struct meson_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	const char *direction;
	unsigned attempt;
	int recovered;
	int error;

	/* Names the direction for the log. */
	direction = "read";
	if (write)
		direction = "write";

	/* Tries the run, recovering the card between the tries. */
	error = EIO;
	for (attempt = 0; attempt < MESON_MMC_RUN_TRIES; attempt++) {
		/* Moves the run; a run that moved ends the tries. */
		error = transfer_run(unit, write, block, count, data);
		if (error == 0)
			break;

		/* Reports the failed try with the status that ended it, and the count so far. */
		unit->errors++;
		kern_logf("meson-mmc: %s of %u blocks at %llu failed (%d, status %x), try %u (errors %llu)\n",
			  direction,
			  count,
			  (unsigned long long)block,
			  error,
			  unit->last_status,
			  attempt + 1U,
			  (unsigned long long)unit->errors);

		/* Brings the card back to the transfer state; a card that never comes back is failed. */
		recovered = recover_card_tries(unit, "after a failed run");
		if (recovered != 0) {
			unit->failed = 1;
			break;
		}

		/* A CRC error at high speed moves the clock down before the next try. */
		if ((unit->last_status & MESON_MMC_STATUS_CRC_ERRORS) != 0) {
			recovered = slow_bus(unit);
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
 * Moves blocks run by run, stopping at the first run that fails; the
 * caller holds the unit's lock.
 */
static int
transfer_blocks(
	struct meson_mmc_unit *unit,
	int write,
	uint64_t block,
	uint32_t count,
	uint8_t *data)
{
	uint32_t done;
	uint32_t run;
	int error;

	/* Moves the blocks in order, a run per command. */
	for (done = 0; done < count; done += run) {
		/* The run, or what is left of the request when that is less. */
		run = count - done;
		if (run > unit->run_blocks)
			run = unit->run_blocks;

		/* Moves the run, recovering the card if it fails. */
		error = transfer_run_recovering(unit,
						write,
						block + done,
						run,
						data + (size_t)done * MESON_MMC_BLOCK_BYTES);
		if (error != 0)
			return error;
	}

	/* Succeeded: every block has moved. */
	return 0;
}

/*
 * Brings the card back to the transfer state after a failed command: the
 * controller is stopped, the transfer in progress is ended, and the card
 * is asked until it is ready for data.
 */
static int
recover_card(
	struct meson_mmc_unit *unit)
{
	struct meson_mmc_command command;
	int error;

	/* Stops whatever the controller still runs and clears its status. */
	error = stop_controller(unit);
	if (error != 0)
		return error;

	/* Ends a transfer still in progress; a card with none refuses it, which is harmless. */
	prepare_command(&command, SD_STOP_TRANSMISSION, 0, MESON_MMC_RESPONSE_BUSY);
	(void)send_command(unit, &command);

	/* Waits until the card is ready for data again. */
	error = wait_card_ready(unit);
	if (error != 0)
		return error;

	/* Succeeded: the card is back in the transfer state. */
	return 0;
}

/*
 * Brings the card back to the transfer state up to MESON_MMC_RECOVER_TRIES
 * times; reports a card that never comes back, naming when it was tried.
 *
 * Returns 0, or the errno of the last recovery.
 */
static int
recover_card_tries(
	struct meson_mmc_unit *unit,
	const char *during)
{
	unsigned again;
	int error;

	/* Tries the recovery until it succeeds or the tries run out. */
	error = EIO;
	for (again = 0; again < MESON_MMC_RECOVER_TRIES; again++) {
		/* Brings the card back; a recovery that worked ends the tries. */
		error = recover_card(unit);
		if (error == 0)
			break;
	}

	/* Reports a card that did not come back. */
	if (error != 0) {
		kern_logf("meson-mmc: the card did not come back %s (%d, status %x); it is taken as gone\n",
			  during,
			  error,
			  unit->last_status);
		return error;
	}

	/* Succeeded: the card is back in the transfer state. */
	return 0;
}

/*
 * Moves the card clock down to the default speed after a CRC error at
 * high speed, once; a bus at the default speed is left as it is.
 *
 * The card stays in its high speed mode at the lower clock, which the
 * mode allows: high speed only raises the highest clock to 50 MHz.
 */
static int
slow_bus(
	struct meson_mmc_unit *unit)
{
	int error;

	/* A bus at the default speed already has nothing left to adapt. */
	if (unit->card.card_clock <= MESON_MMC_DEFAULT_HZ)
		return 0;

	/* Moves the card clock down to the default speed. */
	error = set_card_clock(unit, MESON_MMC_DEFAULT_HZ);
	if (error != 0)
		return error;

	/* Records and reports the slower bus. */
	unit->card.slowed = 1;
	kern_logf("meson-mmc: CRC errors at high speed, card clock down to %llu Hz\n",
		  (unsigned long long)unit->card.card_clock);

	/* Succeeded: the next try runs at the default speed. */
	return 0;
}

/* Publishes the card as the disk mmcblk<unit>. */
static int
publish_disk(
	struct meson_mmc_unit *unit,
	unsigned unit_number)
{
	struct disk *disk;
	int error;

	/* Allocates the disk. */
	disk = disk_alloc();
	if (disk == NULL)
		return ENOMEM;

	/* Prepares the lock that serializes the commands. */
	error = mutex_init(&unit->lock, LOCK_RANK_DISK, "meson-mmc");
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

	/* Describes the card: removable, 512-byte blocks, its capacity, runs the bounce buffer holds. */
	disk->d_flags = DISK_REMOVABLE;
	disk->d_block_size = MESON_MMC_BLOCK_BYTES;
	disk->d_block_count = unit->card.block_count;
	disk->d_max_transfer_blocks = unit->run_blocks;
	disk->d_ops = &meson_mmc_disk_ops;
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
 * Writes the card to the boot log: the interface condition's echo, the
 * operating conditions, the raw CID and CSD, the card's kind, identity
 * and size, and how the bus runs.
 */
static void
log_card(
	const struct meson_mmc_unit *unit)
{
	const struct drv_meson_mmc_card *card;
	const char *speed;

	/* Writes what the card answered to the identification. */
	card = &unit->card;
	kern_logf("meson-mmc: %s CMD8 echo %03x, OCR %08x, RCA %04x\n",
		  unit->disk->d_name,
		  card->interface_echo,
		  card->ocr,
		  card->rca);
	kern_logf("meson-mmc: %s CID %08x%08x%08x%08x CSD %08x%08x%08x%08x\n",
		  unit->disk->d_name,
		  card->cid[3],
		  card->cid[2],
		  card->cid[1],
		  card->cid[0],
		  card->csd[3],
		  card->csd[2],
		  card->csd[1],
		  card->csd[0]);

	/* Writes the card's kind, identity and size. */
	kern_logf("meson-mmc: %s %s card %s, manufacturer %02x, OEM %s, revision %u.%u, serial %08x\n",
		  unit->disk->d_name,
		  card_kind(card),
		  card->product,
		  card->manufacturer,
		  card->oem,
		  card->revision >> 4,
		  card->revision & 0x0fU,
		  card->serial);
	kern_logf("meson-mmc: %s %llu blocks of 512 bytes (%llu MiB)\n",
		  unit->disk->d_name,
		  (unsigned long long)card->block_count,
		  (unsigned long long)(card->block_count / 2048ULL));

	/* Writes how the bus runs. */
	speed = "default speed";
	if (card->high_speed)
		speed = "high speed";
	kern_logf("meson-mmc: %s bus %u-bit, %llu Hz, %s\n",
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
	const struct drv_meson_mmc_card *card)
{
	/* A version 1 CSD describes a standard-capacity card. */
	if (card->csd_version == 1U)
		return "SDSC";

	/* A version 2 CSD above 32 GiB describes an extended-capacity card. */
	if (card->block_count > MESON_MMC_SDHC_MAX_BLOCKS)
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
	struct meson_mmc_unit *unit;
	size_t transferred;
	int write;
	int error;

	/* Picks the unit and the direction of a read or a write. */
	unit = disk->d_data;
	write = 0;
	if (bio->b_op == BIO_WRITE)
		write = 1;

	/* Serves the request by its kind. */
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

		error = EIO;
		if (!unit->failed)
			error = transfer_blocks(unit, write, bio->b_mapped_block, bio->b_block_count, bio->b_data);

		mutex_unlock(&unit->lock);
	}

	/* Completes the request with the bytes it moved. */
	transferred = 0;
	if (error == 0)
		transferred = (size_t)bio->b_block_count * MESON_MMC_BLOCK_BYTES;
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
