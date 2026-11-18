/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The falcon microcontrollers the GSP boot drives: the GSP's own falcon,
 * which runs FWSEC, and the secure processor (SEC2), which runs the booters.
 *
 * A falcon is reset, its instruction memory (IMEM, in 256-byte blocks that
 * each carry a tag) and data memory (DMEM) are written word by word through
 * their port registers, a start address is set, and the CPU is started;
 * the driver then waits for it to halt and reads its two mailboxes.
 *
 * From Ampere on (a profile's NVRTX_FALCON_BOOT_ROM) the falcon is reset
 * with a few more steps (its register block is selected, the reset waits
 * for the engine to be ready, and the scrub is reported in another
 * register), it copies a signed program from system memory by its own DMA,
 * and its boot ROM is told where the signature is and which engine and
 * ucode it is before the start.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/clock.h>
#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gpu.h"

/*
 * The falcon registers, relative to the falcon's base.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/gm200.c
 * (gm200_flcn_disable, gm200_flcn_enable, gm200_flcn_reset_wait_mem_scrubbing,
 * gm200_flcn_fw_load, gm200_flcn_fw_boot), nvkm/falcon/v1.c
 * (nvkm_falcon_v1_load_imem, nvkm_falcon_v1_load_dmem, nvkm_falcon_v1_start),
 * nvkm/falcon/base.c (nvkm_falcon_oneinit), nvkm/falcon/gp102.c
 * (gp102_flcn_reset_eng), nvkm/subdev/gsp/tu102.c (tu102_gsp_fwsec_load_bld)
 * and nvkm/subdev/gsp/rm/r535/gsp.c (r535_gsp_init).
 */
#define FALCON_IRQ_CLEAR		0x004U
#define FALCON_IRQ_MASK_CLEAR		0x014U
#define FALCON_SCRUB_TOUCH		0x040U
#define FALCON_MAILBOX0			0x040U
#define FALCON_MAILBOX1			0x044U
#define FALCON_IRQ_DESTINATION		0x048U
#define FALCON_OS_VERSION		0x080U
#define FALCON_IDENTITY_COPY		0x084U
#define FALCON_CPU_CONTROL		0x100U
#define FALCON_CPU_ALIAS		0x130U
#define FALCON_BOOT_VECTOR		0x104U
#define FALCON_HARDWARE_CONFIG		0x108U
#define FALCON_DMA_CONTROL		0x10cU
#define FALCON_IMEM_CONTROL		0x180U
#define FALCON_IMEM_DATA		0x184U
#define FALCON_IMEM_TAG			0x188U
#define FALCON_DMEM_CONTROL		0x1c0U
#define FALCON_DMEM_DATA		0x1c4U
#define FALCON_ENGINE_RESET		0x3c0U
#define FALCON_DMA_CONTEXT		0x600U
#define FALCON_FBIF_CONTROL		0x624U

/*
 * Bits of those registers.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/gm200.c
 * (gm200_flcn_reset_wait_mem_scrubbing, gm200_flcn_fw_load,
 * gm200_flcn_fw_boot), nvkm/falcon/v1.c (nvkm_falcon_v1_load_imem,
 * nvkm_falcon_v1_start), nvkm/falcon/base.c (nvkm_falcon_oneinit) and
 * nvkm/subdev/gsp/tu102.c (tu102_gsp_fwsec_load_bld).
 */
#define FALCON_CPU_START		0x2U
#define FALCON_CPU_HALTED		0x10U
#define FALCON_CPU_ALIAS_ENABLED	0x40U
#define FALCON_SCRUBBING		0x6U
#define FALCON_PORT_AUTO_INCREMENT	(1U << 24)
#define FALCON_PORT_SECURE		(1U << 28)
#define FALCON_FBIF_ALLOW_PHYSICAL	0x80U
#define FALCON_DMA_CONTEXT_MASK		0x7U
#define FALCON_DMA_CONTEXT_SYSTEM	0x5U
#define FALCON_CODE_LIMIT_MASK		0x1ffU
#define FALCON_IMEM_BLOCK		0x100U

/*
 * The PMC enable register, where SEC2 has a reset bit.
 *
 * Value referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/subdev/mc/nv04.c
 * (nv04_mc_device_disable).
 */
#define FALCON_PMC_ENABLE		0x000200U

/*
 * The registers of an Ampere or later falcon, relative to its base: the
 * reset-ready and scrub status, the DMA's base, target, source, command and
 * upper base, the second register block (base + 0x1000) with the boot ROM's
 * parameters, the RISC-V core's status, the register-block selection and
 * the RISC-V mode bits.
 *
 * Values referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c
 * (ga102_flcn_reset_prep, ga102_flcn_reset_wait_mem_scrubbing,
 * ga102_flcn_dma_init, ga102_flcn_dma_xfer, ga102_flcn_dma_done,
 * ga102_flcn_fw_load, ga102_flcn_fw_boot, ga102_flcn_select,
 * ga102_flcn_riscv_active), nvkm/falcon/tu102.c (tu102_flcn_riscv_active) and
 * nvkm/subdev/gsp/ga102.c (ga102_gsp_reset).
 */
#define FALCON_AMPERE_STATUS		0x0f4U
#define FALCON_AMPERE_RESET_READY	0x80000000U
#define FALCON_AMPERE_SCRUBBING		0x00001000U
#define FALCON_DMA_BASE			0x110U
#define FALCON_DMA_TARGET		0x114U
#define FALCON_DMA_COMMAND		0x118U
#define FALCON_DMA_SOURCE		0x11cU
#define FALCON_DMA_BASE_UPPER		0x128U
#define FALCON_DMA_DONE			0x2U
#define FALCON_DMA_CODE			0x10U
#define FALCON_DMA_SECURE		0x4U
#define FALCON_DMA_BLOCK		0x100U
#define FALCON_DMA_BLOCK_SIZE		(6U << 8)
#define FALCON_DMA_CONTEXT_FIELDS	0x00010007U
#define FALCON_ROM_BOOT			0x1180U
#define FALCON_ROM_UCODE		0x1198U
#define FALCON_ROM_ENGINE		0x119cU
#define FALCON_ROM_SIGNATURE		0x1210U
#define FALCON_RISCV_STATUS_TURING	0x1240U
#define FALCON_RISCV_RUNNING_TURING	0x1U
#define FALCON_RISCV_STATUS_AMPERE	0x1388U
#define FALCON_RISCV_RUNNING_AMPERE	0x80U
#define FALCON_SELECT			0x1668U
#define FALCON_SELECT_PENDING		0x10U
#define FALCON_SELECT_DONE		0x1U
#define FALCON_RISCV_MODE_BITS		0x111U

/* How long the waits last, in microseconds, and the step of the poll. */
#define FALCON_SCRUB_TIMEOUT_US		10000U
#define FALCON_AMPERE_SCRUB_TIMEOUT_US	20000U
#define FALCON_READY_TIMEOUT_US		150U
#define FALCON_SELECT_TIMEOUT_US	10000U
#define FALCON_DMA_TIMEOUT_US		2000000U
#define FALCON_HALT_TIMEOUT_US		2000000U
#define FALCON_POLL_STEP_US		10U

static void falcon_mask(struct nvrtx_gpu *gpu, uint32_t offset, uint32_t mask, uint32_t value);
static int falcon_scrub_wait(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
static int falcon_select(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);
static void falcon_reset_ready(struct nvrtx_gpu *gpu, const struct nvrtx_falcon *falcon);

/*
 * Waits until a register's masked value equals the expected one.
 *
 * Returns 0, or ETIMEDOUT when the time ran out first.
 */
int
nvrtx_poll(
	struct nvrtx_gpu *gpu,
	uint32_t offset,
	uint32_t mask,
	uint32_t value,
	uint32_t timeout_us)
{
	uint64_t start;
	uint64_t now;
	uint32_t current;

	/* Reads the register until it matches or the time is spent. */
	start = nvrtx_now_us();
	for (;;) {
		current = nvrtx_rd32(gpu, offset);
		if ((current & mask) == value)
			return 0;

		/* The time is measured, not counted in steps: a short sleep lasts a whole tick. */
		now = nvrtx_now_us();
		if (now - start > timeout_us)
			break;

		kern_usleep_range(FALCON_POLL_STEP_US, FALCON_POLL_STEP_US * 2U);
	}

	/* The register never matched. */
	return ETIMEDOUT;
}

/*
 * Reads a monotonic time in microseconds, for the timeouts of the waits.
 *
 * A short sleep wakes on the next scheduler tick, so a wait that counted
 * its steps would last many times its timeout; the waits compare this time
 * instead.  Without a counter the tick clock is used.
 */
uint64_t
nvrtx_now_us(void)
{
	uint64_t counter;
	uint64_t frequency;
	uint64_t microseconds;
	bool have;

	/* Reads the counter and its rate. */
	have = kern_rtc_read_counter(&counter, &frequency);
	if (!have || frequency == 0U) {
		/* No counter: the tick clock, in whole milliseconds. */
		microseconds = clock_milliseconds(NULL) * 1000U;
		return microseconds;
	}

	/* Scales the counter without overflowing the product. */
	microseconds = (counter / frequency) * 1000000U + ((counter % frequency) * 1000000U) / frequency;

	/* Succeeded: the time in microseconds. */
	return microseconds;
}

/*
 * Resets a falcon: disables it (and its PMC bit), then enables it again.
 *
 * Returns ETIMEDOUT when its memories are not scrubbed in time.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/base.c
 * (nvkm_falcon_reset) and nvkm/falcon/gm200.c (gm200_flcn_disable,
 * gm200_flcn_enable).
 */
int
nvrtx_falcon_reset(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	uint32_t identity;
	int error;

	/* Selects the falcon's register block where its generation needs it. */
	error = falcon_select(gpu, falcon);
	if (error != 0)
		return error;

	/* Stops its interrupts from reaching the host and clears their masks. */
	falcon_mask(gpu, falcon->base + FALCON_IRQ_DESTINATION, 0x3U, 0U);
	nvrtx_wr32(gpu, falcon->base + FALCON_IRQ_MASK_CLEAR, 0xffffffffU);

	/* Takes a falcon with a PMC bit out of the enable register, once it is ready for the reset. */
	if (falcon->pmc_mask != 0U) {
		falcon_reset_ready(gpu, falcon);
		falcon_mask(gpu, FALCON_PMC_ENABLE, falcon->pmc_mask, 0U);
	}

	/* Pulses the engine reset. */
	error = nvrtx_falcon_reset_engine(gpu, falcon);
	if (error != 0)
		return error;

	/* Pulses it again for the enable, selects it again, puts the PMC bit back and waits for the scrub. */
	error = nvrtx_falcon_reset_engine(gpu, falcon);
	if (error != 0)
		return error;

	error = falcon_select(gpu, falcon);
	if (error != 0)
		return error;

	if (falcon->pmc_mask != 0U) {
		falcon_mask(gpu, FALCON_PMC_ENABLE, falcon->pmc_mask, falcon->pmc_mask);
		(void)nvrtx_rd32(gpu, FALCON_PMC_ENABLE);
	}

	error = falcon_scrub_wait(gpu, falcon);
	if (error != 0)
		return error;

	/* Gives the falcon a copy of the chip identity, as it expects. */
	identity = nvrtx_rd32(gpu, NVRTX_REG_MC_IDENT_0);
	nvrtx_wr32(gpu, falcon->base + FALCON_IDENTITY_COPY, identity);

	/* Succeeded: the falcon is reset with clean memories. */
	return 0;
}

/*
 * Prepares a reset falcon to load without an instance block: physical DMA allowed, DMA control cleared.
 */
void
nvrtx_falcon_load_physical(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	/* Lets the falcon's DMA use physical addresses, with no context bound. */
	falcon_mask(gpu, falcon->base + FALCON_FBIF_CONTROL, FALCON_FBIF_ALLOW_PHYSICAL, FALCON_FBIF_ALLOW_PHYSICAL);
	nvrtx_wr32(gpu, falcon->base + FALCON_DMA_CONTROL, 0U);
}

/*
 * Points one DMA context of a falcon at non-coherent system memory.
 */
void
nvrtx_falcon_dma_system(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon,
	uint32_t context)
{
	/* Sets the context's target to physical system memory, non-coherent. */
	falcon_mask(gpu, falcon->base + FALCON_DMA_CONTEXT + context * 4U, FALCON_DMA_CONTEXT_MASK, FALCON_DMA_CONTEXT_SYSTEM);
}

/*
 * Reports the size of a falcon's instruction memory.
 */
uint32_t
nvrtx_falcon_code_limit(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	uint32_t config;

	/* The hardware configuration holds the size in 256-byte blocks. */
	config = nvrtx_rd32(gpu, falcon->base + FALCON_HARDWARE_CONFIG);

	/* Succeeded: the size in bytes. */
	return (config & FALCON_CODE_LIMIT_MASK) << 8;
}

/*
 * Writes code into a falcon's instruction memory.
 *
 * The length is a whole number of 256-byte blocks; each block carries the
 * next tag, starting from the given one.  Secure code is marked so.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/v1.c
 * (nvkm_falcon_v1_load_imem).
 */
int
nvrtx_falcon_imem(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon,
	const uint8_t *code,
	uint32_t length,
	uint32_t address,
	uint32_t tag,
	int secure)
{
	uint32_t control;
	uint32_t at;

	/* Refuses a length that is not whole blocks. */
	if (length == 0U || (length % FALCON_IMEM_BLOCK) != 0U)
		return EINVAL;

	/* Opens the port at the address, incrementing, marked secure when asked. */
	control = FALCON_PORT_AUTO_INCREMENT | address;
	if (secure != 0)
		control |= FALCON_PORT_SECURE;
	nvrtx_wr32(gpu, falcon->base + FALCON_IMEM_CONTROL, control);

	/* Writes each block's tag, then its words. */
	for (at = 0U; at < length; at += 4U) {
		if ((at % FALCON_IMEM_BLOCK) == 0U) {
			nvrtx_wr32(gpu, falcon->base + FALCON_IMEM_TAG, tag);
			tag++;
		}

		nvrtx_wr32(gpu, falcon->base + FALCON_IMEM_DATA, nvrtx_get32(code, at));
	}

	/* Succeeded: the code is in the instruction memory. */
	return 0;
}

/*
 * Writes data into a falcon's data memory.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/v1.c
 * (nvkm_falcon_v1_load_dmem).
 */
int
nvrtx_falcon_dmem(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon,
	const uint8_t *data,
	uint32_t length,
	uint32_t address)
{
	uint32_t at;

	/* Refuses a length that is not whole words. */
	if (length == 0U || (length % 4U) != 0U)
		return EINVAL;

	/* Opens the port at the address, incrementing, and writes the words. */
	nvrtx_wr32(gpu, falcon->base + FALCON_DMEM_CONTROL, FALCON_PORT_AUTO_INCREMENT | address);
	for (at = 0U; at < length; at += 4U)
		nvrtx_wr32(gpu, falcon->base + FALCON_DMEM_DATA, nvrtx_get32(data, at));

	/* Succeeded: the data is in the data memory. */
	return 0;
}

/*
 * Starts a loaded falcon and waits for it to halt.
 *
 * The mailboxes are written before the start and read after the halt.
 * Returns ETIMEDOUT when the falcon did not halt within 2 s; the mailboxes
 * are read either way.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/gm200.c
 * (gm200_flcn_fw_boot).
 */
int
nvrtx_falcon_boot(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon,
	uint32_t boot_address,
	uint32_t *mailbox0,
	uint32_t *mailbox1)
{
	int error;

	/* Hands the program its two arguments. */
	nvrtx_wr32(gpu, falcon->base + FALCON_MAILBOX0, *mailbox0);
	nvrtx_wr32(gpu, falcon->base + FALCON_MAILBOX1, *mailbox1);

	/* Sets the start address and starts the CPU. */
	nvrtx_wr32(gpu, falcon->base + FALCON_BOOT_VECTOR, boot_address);
	nvrtx_wr32(gpu, falcon->base + FALCON_CPU_CONTROL, FALCON_CPU_START);

	/* Waits for the CPU to halt. */
	error = nvrtx_poll(gpu, falcon->base + FALCON_CPU_CONTROL, FALCON_CPU_HALTED, FALCON_CPU_HALTED, FALCON_HALT_TIMEOUT_US);

	/* Reads what the program left in the mailboxes. */
	*mailbox0 = nvrtx_rd32(gpu, falcon->base + FALCON_MAILBOX0);
	*mailbox1 = nvrtx_rd32(gpu, falcon->base + FALCON_MAILBOX1);

	/* Reports a falcon that did not halt. */
	if (error != 0)
		return error;

	/* Succeeded: the program ran to its halt. */
	return 0;
}

/*
 * Starts a falcon's CPU where it stands, through the alias register when the
 * CPU control says the alias is in use.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/v1.c
 * (nvkm_falcon_v1_start).
 */
void
nvrtx_falcon_start(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	uint32_t control;

	/* Reads whether the CPU control is reached through its alias. */
	control = nvrtx_rd32(gpu, falcon->base + FALCON_CPU_CONTROL);
	if ((control & FALCON_CPU_ALIAS_ENABLED) != 0U) {
		/* The alias starts the CPU. */
		nvrtx_wr32(gpu, falcon->base + FALCON_CPU_ALIAS, FALCON_CPU_START);
		return;
	}

	/* The CPU control itself starts the CPU. */
	nvrtx_wr32(gpu, falcon->base + FALCON_CPU_CONTROL, FALCON_CPU_START);
}

/*
 * Pulses a falcon's engine reset and waits for the scrub.
 *
 * This alone is how the GSP's falcon is put into its RISC-V mode before the booter starts it.
 */
int
nvrtx_falcon_reset_engine(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	int error;

	/* Waits, briefly, for an Ampere falcon to be ready for the reset. */
	falcon_reset_ready(gpu, falcon);

	/* Holds the engine in reset for 10 us, then lets it go. */
	falcon_mask(gpu, falcon->base + FALCON_ENGINE_RESET, 0x1U, 0x1U);
	kern_usleep_range(10U, 20U);
	falcon_mask(gpu, falcon->base + FALCON_ENGINE_RESET, 0x1U, 0U);

	/* Waits for the memories to be scrubbed. */
	error = falcon_scrub_wait(gpu, falcon);
	if (error != 0)
		return error;

	/* Succeeded: the engine is out of reset. */
	return 0;
}

/*
 * Puts the GSP's falcon into its RISC-V mode: an engine reset, and on Ampere
 * and later the RISC-V mode bits of its second register block.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/ga102.c (ga102_gsp_reset).
 */
int
nvrtx_falcon_riscv_mode(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	int error;

	/* Pulses the engine reset. */
	error = nvrtx_falcon_reset_engine(gpu, falcon);
	if (error != 0)
		return error;

	/* An Ampere falcon is switched to the RISC-V core explicitly. */
	if (falcon->style == NVRTX_FALCON_BOOT_ROM)
		falcon_mask(gpu, falcon->base + FALCON_SELECT, FALCON_RISCV_MODE_BITS, FALCON_RISCV_MODE_BITS);

	/* Succeeded: the falcon starts its RISC-V core when the booter hands it over. */
	return 0;
}

/*
 * Reports whether the GSP's RISC-V core runs, with the status register read.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/tu102.c
 * (tu102_flcn_riscv_active) and nvkm/falcon/ga102.c (ga102_flcn_riscv_active).
 */
bool
nvrtx_falcon_riscv_running(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon,
	uint32_t *status)
{
	/* An Ampere falcon reports the core in another register and bit than Turing's. */
	if (falcon->style == NVRTX_FALCON_BOOT_ROM) {
		*status = nvrtx_rd32(gpu, falcon->base + FALCON_RISCV_STATUS_AMPERE);
		if ((*status & FALCON_RISCV_RUNNING_AMPERE) != 0U)
			return true;

		/* The core is stopped. */
		return false;
	}

	/* Turing's status register: bit 0 while the core runs. */
	*status = nvrtx_rd32(gpu, falcon->base + FALCON_RISCV_STATUS_TURING);
	if ((*status & FALCON_RISCV_RUNNING_TURING) != 0U)
		return true;

	/* The core is stopped. */
	return false;
}

/*
 * Has an Ampere or later falcon copy part of a program into its memory by DMA.
 *
 * The program is in system memory at bus; length bytes from source (an
 * offset into it) go to target in the instruction memory (code) or the data
 * memory, 256 bytes at a time.  The DMA's base holds the address in
 * 256-byte units, so bus + source of data must be a multiple of 256 (code
 * is addressed from bus itself).  Returns EINVAL for a length or an address
 * the DMA cannot take, ETIMEDOUT when a block does not finish.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c (ga102_flcn_dma_init,
 * ga102_flcn_dma_xfer, ga102_flcn_fw_load).
 */
int
nvrtx_falcon_dma_load(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon,
	uint64_t bus,
	uint32_t source,
	uint32_t length,
	uint32_t target,
	int code,
	int secure)
{
	uint64_t base;
	uint32_t command;
	uint32_t offset;
	uint32_t done;
	int error;

	/* Code is copied from offsets into the program, data from a base at its own start. */
	base = bus;
	offset = source;
	if (code == 0) {
		base = bus + source;
		offset = 0U;
	}

	/* The DMA moves whole blocks from a block-aligned base below 2^40. */
	if (length == 0U || (length % FALCON_DMA_BLOCK) != 0U)
		return EINVAL;
	if ((base % FALCON_DMA_BLOCK) != 0U || (base >> 40) != 0U)
		return EINVAL;

	/* Lets the falcon reach physical memory and points DMA context 0 at system memory. */
	falcon_mask(gpu, falcon->base + FALCON_FBIF_CONTROL, FALCON_FBIF_ALLOW_PHYSICAL, FALCON_FBIF_ALLOW_PHYSICAL);
	nvrtx_wr32(gpu, falcon->base + FALCON_DMA_CONTROL, 0U);
	falcon_mask(gpu, falcon->base + FALCON_DMA_CONTEXT, FALCON_DMA_CONTEXT_FIELDS, FALCON_DMA_CONTEXT_SYSTEM);

	/* Builds the command: 256-byte blocks, to the code or the data, marked secure when asked. */
	command = FALCON_DMA_BLOCK_SIZE;
	if (code != 0)
		command |= FALCON_DMA_CODE;
	if (secure != 0)
		command |= FALCON_DMA_SECURE;

	/* Sets the DMA's base. */
	nvrtx_wr32(gpu, falcon->base + FALCON_DMA_BASE, (uint32_t)(base >> 8));
	nvrtx_wr32(gpu, falcon->base + FALCON_DMA_BASE_UPPER, 0U);

	/* Copies each block and waits for it. */
	for (done = 0U; done < length; done += FALCON_DMA_BLOCK) {
		nvrtx_wr32(gpu, falcon->base + FALCON_DMA_TARGET, target + done);
		nvrtx_wr32(gpu, falcon->base + FALCON_DMA_SOURCE, offset + done);
		nvrtx_wr32(gpu, falcon->base + FALCON_DMA_COMMAND, command);

		/* Waits for the block to arrive. */
		error = nvrtx_poll(gpu, falcon->base + FALCON_DMA_COMMAND, FALCON_DMA_DONE, FALCON_DMA_DONE, FALCON_DMA_TIMEOUT_US);
		if (error != 0)
			return error;
	}

	/* Succeeded: the part is in the falcon's memory. */
	return 0;
}

/*
 * Tells an Ampere or later falcon's boot ROM about the program it starts:
 * where its signature is in the data memory, its engine and its ucode
 * number.  The start itself is nvrtx_falcon_boot's.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c (ga102_flcn_fw_boot).
 */
void
nvrtx_falcon_boot_rom(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon,
	uint32_t signature_at,
	uint32_t engine_id,
	uint32_t ucode_id)
{
	/* The boot ROM's parameters, then its go-ahead. */
	nvrtx_wr32(gpu, falcon->base + FALCON_ROM_SIGNATURE, signature_at);
	nvrtx_wr32(gpu, falcon->base + FALCON_ROM_ENGINE, engine_id);
	nvrtx_wr32(gpu, falcon->base + FALCON_ROM_UCODE, ucode_id);
	nvrtx_wr32(gpu, falcon->base + FALCON_ROM_BOOT, 0x1U);
}

/* Changes the masked bits of a register to a value. */
static void
falcon_mask(
	struct nvrtx_gpu *gpu,
	uint32_t offset,
	uint32_t mask,
	uint32_t value)
{
	uint32_t current;

	/* Reads, replaces the masked bits, and writes back. */
	current = nvrtx_rd32(gpu, offset);
	nvrtx_wr32(gpu, offset, (current & ~mask) | value);
}

/*
 * Waits for a falcon's memories to finish scrubbing after a reset.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c
 * (ga102_flcn_reset_wait_mem_scrubbing) and nvkm/falcon/gm200.c
 * (gm200_flcn_reset_wait_mem_scrubbing).
 */
static int
falcon_scrub_wait(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	int error;

	/* Touches the scrub control. */
	falcon_mask(gpu, falcon->base + FALCON_SCRUB_TOUCH, 0U, 0U);

	/* An Ampere falcon reports the scrub in its status register. */
	if (falcon->style == NVRTX_FALCON_BOOT_ROM) {
		error = nvrtx_poll(gpu,
		    falcon->base + FALCON_AMPERE_STATUS,
		    FALCON_AMPERE_SCRUBBING,
		    0U,
		    FALCON_AMPERE_SCRUB_TIMEOUT_US);
		if (error != 0)
			return error;

		/* Succeeded: the memories are clean. */
		return 0;
	}

	/* A Turing falcon clears both scrubbing bits of its DMA control. */
	error = nvrtx_poll(gpu, falcon->base + FALCON_DMA_CONTROL, FALCON_SCRUBBING, 0U, FALCON_SCRUB_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the memories are clean. */
	return 0;
}

/*
 * Selects an Ampere falcon's register block when it is not selected yet.
 *
 * A Turing falcon has nothing to select.  Returns ETIMEDOUT when the
 * selection does not complete.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c (ga102_flcn_select).
 */
static int
falcon_select(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	uint32_t value;
	int error;

	/* Only an Ampere falcon has the selection. */
	if (falcon->style != NVRTX_FALCON_BOOT_ROM)
		return 0;

	/* Nothing is pending: the block is selected already. */
	value = nvrtx_rd32(gpu, falcon->base + FALCON_SELECT);
	if ((value & FALCON_SELECT_PENDING) == 0U)
		return 0;

	/* Asks for the selection and waits for it. */
	nvrtx_wr32(gpu, falcon->base + FALCON_SELECT, 0U);
	error = nvrtx_poll(gpu, falcon->base + FALCON_SELECT, FALCON_SELECT_DONE, FALCON_SELECT_DONE, FALCON_SELECT_TIMEOUT_US);
	if (error != 0)
		return error;

	/* Succeeded: the block is selected. */
	return 0;
}

/*
 * Waits up to 150 us for an Ampere falcon to say it is ready for a reset.
 *
 * The wait is advice: a falcon that does not say so is reset anyway.  A
 * Turing falcon has no such bit.
 *
 * Procedure referenced: Linux v6.19 drivers/gpu/drm/nouveau/nvkm/falcon/ga102.c
 * (ga102_flcn_reset_prep).
 */
static void
falcon_reset_ready(
	struct nvrtx_gpu *gpu,
	const struct nvrtx_falcon *falcon)
{
	/* Only an Ampere falcon reports readiness. */
	if (falcon->style != NVRTX_FALCON_BOOT_ROM)
		return;

	/* Reads the status once, then waits for the ready bit. */
	(void)nvrtx_rd32(gpu, falcon->base + FALCON_AMPERE_STATUS);
	(void)nvrtx_poll(gpu,
	    falcon->base + FALCON_AMPERE_STATUS,
	    FALCON_AMPERE_RESET_READY,
	    FALCON_AMPERE_RESET_READY,
	    FALCON_READY_TIMEOUT_US);
}
