/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Dell provider's pure parts: which BIOS thermal mode a strategy picks,
 * the decoding of the DDV fan information, and the buffers of the SMBIOS
 * calling interface and of the SMM fan calls with the checks of their
 * answers.
 *
 * Nothing here evaluates AML or touches the hardware.
 */

#include <drivers/acpi/dell.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The SMBIOS calling interface's buffer, as the firmware's WMBA method of
 * the SMBIOS block reads it: the class and the select as words, four input
 * dwords, four output dwords, an attribute dword, and the length of an
 * extension that follows (the whole buffer is that length plus 0x2C).
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.AMW0.WMBA (the fields at 0, 2, 4, 8 and 0x28, the length plus 0x2C).
 * Values referenced: Linux 6.12 include/uapi/linux/wmi.h (the calling
 * interface buffer: the outputs at 0x14, the attribute at 0x24).
 */
#define SMBIOS_CLASS		0x00U
#define SMBIOS_SELECT		0x02U
#define SMBIOS_INPUT0		0x04U
#define SMBIOS_INPUT1		0x08U
#define SMBIOS_OUTPUT		0x14U
#define SMBIOS_OUTPUT_SIZE	16U
#define SMBIOS_EXTENSION	0x28U

/*
 * The thermal mode call: class 0x11, select 0x13, and input0 1 to set the
 * mode named by one bit of input1.  The firmware turns the bits 1, 2, 4
 * and 8 into the modes 0 to 3 of its ACPI variable.
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.AMW0.WMBA (the class, select and input0 it checks, and the four
 * input1 cases).
 */
#define MODE_CLASS		0x11U
#define MODE_SELECT		0x13U
#define MODE_SET		0x01U

/*
 * What the SMBIOS call leaves in output0 when it completed.
 *
 * Value referenced: Linux 6.12
 * drivers/platform/x86/dell/dell-smbios-base.c (dell_smbios_error).
 */
#define SMBIOS_COMPLETED	0x00000000U

/*
 * What the provider fills the outputs with before the call: firmware that
 * ran the call writes over at least output0.
 */
#define SMBIOS_SENTINEL		0xffU

/*
 * The four-register SMM call's buffer: for each of EAX, EBX, ECX and EDX a
 * length dword (4) and the register's dword.  The firmware writes the
 * registers it returns over the same places.
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.DIAG.C000 and C001.
 */
#define SMM_REGISTER_SIZE	4U
#define SMM_EAX_LENGTH		0U
#define SMM_EAX			4U
#define SMM_EBX_LENGTH		8U
#define SMM_EBX			12U
#define SMM_ECX_LENGTH		16U
#define SMM_EDX_LENGTH		24U

/*
 * The SMM fan calls: read a fan's state (EBX the fan), and set it (EBX the
 * fan and the state times 256).
 *
 * Values referenced: Linux 6.12 drivers/hwmon/dell-smm-hwmon.c (the get
 * fan and set fan commands, the set fan argument).
 */
#define SMM_GET_FAN		0x00a3U
#define SMM_SET_FAN		0x01a3U
#define SMM_FAN_STATE_SHIFT	8U

/*
 * The SMM's answer to a call it does not carry out: the low 16 bits of EAX
 * all ones.
 *
 * Value referenced: Linux 6.12 drivers/hwmon/dell-smm-hwmon.c
 * (dell_smm_call, the check of the returned EAX).
 */
#define SMM_FAILED		0xffffU
#define SMM_FAILED_MASK		0xffffU

/*
 * The manual axes' thresholds: a third and two thirds of the axis.  A
 * manual strategy that allows little noise picks the quiet mode, one that
 * asks for speed and allows noise picks the performance mode, any other
 * the optimized one.
 */
#define AXIS_LOW		33U
#define AXIS_HIGH		67U

/* The end mark of the DDV fan information, and the bytes of one fan. */
#define FANS_END		0xffU
#define FAN_BYTES		3U

static void smm_request(uint32_t eax, uint32_t ebx, uint8_t request[DRV_ACPI_DELL_SMM_SIZE]);
static uint32_t read_le32(const uint8_t *bytes);
static void write_le16(uint8_t *bytes, uint16_t value);
static void write_le32(uint8_t *bytes, uint32_t value);

/*
 * Reports the BIOS thermal mode a strategy picks.
 *
 * Returns DRV_ACPI_DELL_MODE_NONE for FIRMWARE, which writes nothing, and
 * for a value the core does not define.
 */
uint32_t
drv_acpi_dell_mode_for(
	const struct kern_thermal_strategy *strategy)
{
	/* Picks the mode by the strategy. */
	switch (strategy->value) {
	case KERN_THERMAL_STRATEGY_PERFORMANCE:
		return DRV_ACPI_DELL_MODE_PERFORMANCE;
	case KERN_THERMAL_STRATEGY_BALANCED:
		return DRV_ACPI_DELL_MODE_OPTIMIZED;
	case KERN_THERMAL_STRATEGY_POWERSAVE:
		/* The cool mode lowers the processor; the quiet one would slow the fan. */
		return DRV_ACPI_DELL_MODE_COOL;
	case KERN_THERMAL_STRATEGY_MANUAL:
		break;
	default:
		return DRV_ACPI_DELL_MODE_NONE;
	}

	/* A manual strategy that allows little noise lets the firmware run the fan slower. */
	if (strategy->noise <= AXIS_LOW)
		return DRV_ACPI_DELL_MODE_QUIET;

	/* One that wants speed and allows noise takes the performance mode. */
	if (strategy->noise >= AXIS_HIGH && strategy->speed >= AXIS_HIGH)
		return DRV_ACPI_DELL_MODE_PERFORMANCE;

	/* Anything between takes the optimized mode. */
	return DRV_ACPI_DELL_MODE_OPTIMIZED;
}

/*
 * Reports the name of a BIOS thermal mode, or NULL for one not known.
 *
 * The names are the BIOS setup's, but "Performance" for the one the setup
 * calls "UltraPerformance", which would not fit the core's 15 characters.
 */
const char *
drv_acpi_dell_mode_name(
	uint32_t mode)
{
	/* Names the mode. */
	switch (mode) {
	case DRV_ACPI_DELL_MODE_OPTIMIZED:
		return "Optimized";
	case DRV_ACPI_DELL_MODE_COOL:
		return "Cool";
	case DRV_ACPI_DELL_MODE_QUIET:
		return "Quiet";
	case DRV_ACPI_DELL_MODE_PERFORMANCE:
		return "Performance";
	default:
		return NULL;
	}
}

/*
 * Decodes the DDV fan information: the length the firmware gave and a
 * buffer of three bytes for each fan (its kind, then its speed in rpm,
 * low byte first) closed by 0xFF.
 *
 * A length of 0 is a machine whose embedded controller reports no fan
 * (the buffer then holds one byte that is not a fan).  At most capacity
 * fans are taken.  Returns 0, EINVAL for information that does not hold
 * whole fans closed by the end mark, or E2BIG when more fans than capacity
 * were listed (the first capacity are taken).
 *
 * Value referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.AMWV.WMDV (method 0x20: the length 3n + 1, the three bytes of a
 * fan, the closing 0xFF, and the length 0 of no fan).
 */
int
drv_acpi_dell_fans_decode(
	uint64_t length,
	const uint8_t *bytes,
	size_t size,
	struct drv_acpi_dell_fan *fans,
	unsigned capacity,
	unsigned *count)
{
	uint64_t listed;
	unsigned index;
	size_t offset;

	/* Starts with no fan. */
	*count = 0;

	/* A length of 0 lists no fan. */
	if (length == 0)
		return 0;

	/* Refuses a length that is not whole fans and the end mark, or that runs past the buffer. */
	if (length % FAN_BYTES != 1U)
		return EINVAL;
	if (length > size)
		return EINVAL;

	/* Refuses information without its end mark. */
	if (bytes[length - 1U] != FANS_END)
		return EINVAL;

	/* Takes each fan, up to the capacity. */
	listed = length / FAN_BYTES;
	for (index = 0; index < listed && index < capacity; index++) {
		offset = (size_t)index * FAN_BYTES;
		fans[index].type = bytes[offset];
		fans[index].rpm = (uint16_t)(bytes[offset + 1U] | ((uint16_t)bytes[offset + 2U] << 8));
	}

	/* Reports the fans taken, and that some were left. */
	*count = index;
	if (listed > capacity)
		return E2BIG;

	/* Succeeded: every fan listed is taken. */
	return 0;
}

/*
 * Fills the SMBIOS calling interface's buffer that sets a BIOS thermal
 * mode, with no extension, the outputs filled with the sentinel, and the
 * acoustic setting of the call's second byte left 0.
 *
 * Values referenced: Linux 6.12 drivers/platform/x86/dell/dell-pc.c (the
 * thermal mode bits: balanced 0, cool bottom 1, quiet 2, performance 3).
 */
void
drv_acpi_dell_mode_request(
	uint32_t mode,
	uint8_t request[DRV_ACPI_DELL_SMBIOS_SIZE])
{
	/* Starts from a buffer of zeros: no extension, no attribute, inputs 2 and 3 zero. */
	kern_memset(request, 0, DRV_ACPI_DELL_SMBIOS_SIZE);

	/* Names the thermal mode call and asks it to set the mode's bit. */
	write_le16(&request[SMBIOS_CLASS], MODE_CLASS);
	write_le16(&request[SMBIOS_SELECT], MODE_SELECT);
	write_le32(&request[SMBIOS_INPUT0], MODE_SET);
	write_le32(&request[SMBIOS_INPUT1], (uint32_t)1U << mode);

	/* Fills the outputs with the sentinel that tells a call that never ran. */
	kern_memset(&request[SMBIOS_OUTPUT], SMBIOS_SENTINEL, SMBIOS_OUTPUT_SIZE);

	/* The extension's length stays 0, so the buffer is 0x2C bytes. */
	write_le32(&request[SMBIOS_EXTENSION], 0);
}

/*
 * Checks the buffer the SMBIOS call came back with.
 *
 * status gets output0.  Returns 0 when the call completed, ENXIO when the
 * outputs still hold the sentinel (the firmware did not run the call),
 * EIO when output0 reports a failure, or EINVAL for a buffer too short to
 * hold the outputs.
 */
int
drv_acpi_dell_mode_answer(
	const uint8_t *answer,
	size_t size,
	uint32_t *status)
{
	unsigned index;
	bool untouched;

	/* Starts with no status. */
	*status = 0;

	/* Refuses a buffer without its outputs. */
	if (size < SMBIOS_OUTPUT + SMBIOS_OUTPUT_SIZE)
		return EINVAL;

	/* Looks for a byte of the outputs the firmware wrote. */
	untouched = true;
	for (index = 0; index < SMBIOS_OUTPUT_SIZE; index++) {
		if (answer[SMBIOS_OUTPUT + index] != SMBIOS_SENTINEL) {
			untouched = false;
			break;
		}
	}

	/* Reports a call the firmware never ran. */
	if (untouched)
		return ENXIO;

	/* Reports a call that ran and failed. */
	*status = read_le32(&answer[SMBIOS_OUTPUT]);
	if (*status != SMBIOS_COMPLETED)
		return EIO;

	/* Succeeded: the firmware completed the call. */
	return 0;
}

/*
 * Fills the SMM call's buffer that reads the state of a fan.
 */
void
drv_acpi_dell_fan_get_request(
	uint32_t fan,
	uint8_t request[DRV_ACPI_DELL_SMM_SIZE])
{
	/* Asks for the fan's state. */
	smm_request(SMM_GET_FAN, fan, request);
}

/*
 * Fills the SMM call's buffer that sets the state of a fan.
 */
void
drv_acpi_dell_fan_set_request(
	uint32_t fan,
	uint32_t state,
	uint8_t request[DRV_ACPI_DELL_SMM_SIZE])
{
	/* Names the fan and the state it is to run at. */
	smm_request(SMM_SET_FAN, fan | (state << SMM_FAN_STATE_SHIFT), request);
}

/*
 * Checks the buffer an SMM call came back with, against the request it
 * was made with.
 *
 * eax gets the returned EAX.  Returns 0, ENXIO when EAX still holds the
 * command (the call never ran), EIO when the SMM refused the call, or
 * EINVAL for a buffer too short.
 */
int
drv_acpi_dell_smm_answer(
	const uint8_t *answer,
	size_t size,
	const uint8_t request[DRV_ACPI_DELL_SMM_SIZE],
	uint32_t *eax)
{
	uint32_t command;

	/* Starts with no answer. */
	*eax = 0;

	/* Refuses a buffer without the registers. */
	if (size < DRV_ACPI_DELL_SMM_SIZE)
		return EINVAL;

	/*
	 * Reports a call that left EAX as it was: the firmware's AML copies the
	 * registers back from the shared memory it handed to the SMM, so a call
	 * the SMM never ran returns the command itself.
	 */
	command = read_le32(&request[SMM_EAX]);
	*eax = read_le32(&answer[SMM_EAX]);
	if (*eax == command)
		return ENXIO;

	/* Reports a call the SMM refused. */
	if ((*eax & SMM_FAILED_MASK) == SMM_FAILED)
		return EIO;

	/* Succeeded: the SMM answered the call. */
	return 0;
}

/* Fills the four-register SMM call's buffer with EAX and EBX; ECX and EDX are 0. */
static void
smm_request(
	uint32_t eax,
	uint32_t ebx,
	uint8_t request[DRV_ACPI_DELL_SMM_SIZE])
{
	/* Starts from a buffer of zeros: ECX and EDX are 0. */
	kern_memset(request, 0, DRV_ACPI_DELL_SMM_SIZE);

	/* Gives each register its length. */
	write_le32(&request[SMM_EAX_LENGTH], SMM_REGISTER_SIZE);
	write_le32(&request[SMM_EBX_LENGTH], SMM_REGISTER_SIZE);
	write_le32(&request[SMM_ECX_LENGTH], SMM_REGISTER_SIZE);
	write_le32(&request[SMM_EDX_LENGTH], SMM_REGISTER_SIZE);

	/* Places the command and its argument. */
	write_le32(&request[SMM_EAX], eax);
	write_le32(&request[SMM_EBX], ebx);
}

/* Reads a little-endian dword. */
static uint32_t
read_le32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Assembles the four bytes, the lowest first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Reports the dword. */
	return value;
}

/* Writes a little-endian word. */
static void
write_le16(
	uint8_t *bytes,
	uint16_t value)
{
	/* Stores the two bytes, the lowest first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

/* Writes a little-endian dword. */
static void
write_le32(
	uint8_t *bytes,
	uint32_t value)
{
	/* Stores the four bytes, the lowest first. */
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}
