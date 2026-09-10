/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * NEC PC-98 internal IDE driver (PIO, polled)
 *
 * - The PC-98 built-in IDE multiplexes two ATA channels.
 * - Each with a master/slave pair onto one register block.
 * - Port 0x432 bit 0 selects the active bank.
 * - The command block lives at 0x640-0x64e with a 2-byte stride
 *   (16-bit data at 0x640), and the control block at 0x74c/0x74e.
 * - Reference model: qemu-pc98 hw/ide/pc98-ide.c
 * - Reference driver: linux-pc98 drivers/ata/pata_pc9800.c.
 * - Interrupts stay disabled (nIEN), everything is polled.
 */

#include "drivers/platform/pc98/pc98-ide.h"
#include "kern/boot.h"
#include "kern/lock.h"
#include <errno.h>
#include <hal/hal.h>

#define IDE_BANK_SELECT 0x432U
#define IDE_DATA 0x640U
#define IDE_ERROR 0x642U
#define IDE_NSECT 0x644U
#define IDE_LBA_LOW 0x646U
#define IDE_LBA_MID 0x648U
#define IDE_LBA_HIGH 0x64aU
#define IDE_DRIVE_HEAD 0x64cU
#define IDE_STATUS 0x64eU     /* read: status, write: command */
#define IDE_ALT_STATUS 0x74cU /* read: alt status, write: devctl */

#define IDE_STATUS_BSY 0x80U
#define IDE_STATUS_DRDY 0x40U
#define IDE_STATUS_DF 0x20U
#define IDE_STATUS_DRQ 0x08U
#define IDE_STATUS_ERR 0x01U

#define IDE_CMD_READ 0x20U
#define IDE_CMD_WRITE 0x30U
#define IDE_CMD_IDENTIFY 0xecU
#define IDE_CMD_FLUSH_CACHE 0xe7U

#define IDE_DEVCTL_NIEN 0x02U
#define IDE_DEVCTL_SRST 0x04U

#define IDE_ID_CONFIG 0U
#define IDE_ID_COMMAND_SET_2 83U
#define IDE_ID_CFA_FEATURE 0x4004U

#define IDE_UNIT_MAX 4U

/*
 * Match the independently proven Linux pc98ide driver's five-second
 * ceiling: every unsuccessful poll is followed by about 10
 * microseconds of ISA I/O delay.
 */
#define IDE_TIMEOUT_POLLS 500000U

struct ide_unit {
	struct disk *disk;
	uint8_t present;
	uint8_t bank;
	uint8_t drive;
	uint8_t use_lba;
	/* Native geometry from IDENTIFY, for the CHS command fallback. */
	uint16_t native_cylinders;
	uint16_t native_heads;
	uint16_t native_sectors;
	uint16_t firmware_heads;
	uint16_t firmware_sectors;
	uint8_t flush_supported;
	uint8_t write_cache_known;
	uint8_t write_cache_enabled;
};

static struct ide_unit units[IDE_UNIT_MAX];
static struct ide_unit *unit_order[IDE_UNIT_MAX];
static unsigned unit_count;
static const char *failure_stage;
/* Both ATA banks share one register window and must serialize commands. */
static struct mutex controller_lock;


static const struct boot_device * bios_device_for_slot(const struct boot_device *devices, unsigned count, unsigned slot);
static int reset_bank(uint8_t bank);
static void select_bank(uint8_t bank);
static void outb(uint16_t port, uint8_t value);
static void delay_10us(void);
static void select_delay(void);
static uint8_t inb(uint16_t port);
static void delay_2ms(void);
static int wait_clear(uint8_t mask);
static void report_probe_failure(unsigned slot);
static int identify(uint8_t bank, uint8_t drive, uint16_t data[256]);
static int select_unit(const struct ide_unit *unit, uint8_t head_bits, int lba);
static int wait_selectable(void);
static int wait_drq(void);
static uint16_t inw(uint16_t port);
static int identify_is_cfa(const uint16_t data[256]);
static void outw(uint16_t port, uint16_t value);
static int setup_transfer(const struct ide_unit *unit, uint64_t lba, uint32_t count);
static int pio_read(struct disk *dev, uint64_t lba, uint32_t count, void *buffer);
static int pio_write(struct disk *dev, uint64_t lba, uint32_t count, const void *buffer);
static int pio_flush(struct disk *dev);
static int pc98_ide_submit(struct disk *dev, struct bio *bio);
static int pc98_ide_ioctl(struct disk *dev, unsigned long request, void *argument);

static const struct disk_ops pc98_ide_disk_ops = {
	.submit = pc98_ide_submit,
	.ioctl = pc98_ide_ioctl,
};

/*
 * Implements the drv pc98 ide init operation.
 */
unsigned
drv_pc98_ide_init(
	const struct boot_device *bios_devices,
	unsigned bios_device_count)
{
	struct ide_unit *unit;
	const struct boot_device *bios_dev;
	uint64_t sector_count;
	uint8_t bank;
	uint8_t drive;
	static uint16_t data[256];
	uint8_t reset_done[2] = {0, 0};
	unsigned pass;
	unsigned slot;

	(void)mutex_init(&controller_lock, LOCK_RANK_DISK, "pc98-ide");
	unit_count = 0;
	/* Process each element required by the operation. */
	for (slot = 0; slot < IDE_UNIT_MAX; slot++) {
		units[slot].present = 0;
		units[slot].disk = NULL;
		unit_order[slot] = NULL;
	}

	/*
	 * Probe only BIOS-advertised units, with the boot-origin unit first.
	 * Besides avoiding hangs on floating secondary channels, this preserves
	 * the BIOS-to-native mapping used by VFS.
	 */
	for (pass = 0; pass < 2; pass++) {
		/* Process each element required by the operation. */
		for (slot = 0; slot < IDE_UNIT_MAX; slot++) {
			bank = (uint8_t)(slot / 2U);
			drive = (uint8_t)(slot & 1U);

			/* Handles the bios dev availability. */
			bios_dev = bios_device_for_slot(
				bios_devices, bios_device_count, slot);
			if (bios_dev == NULL ||
			    (((bios_dev->flags & ZEDBSD_DEV_BOOT_ORIGIN) !=
			      0) != (pass == 0)))
				continue;

			/* Handles the reset done condition. */
			if (!reset_done[bank]) {
				reset_done[bank] = 1;

				/* Checks the reset bank result. */
				if (!reset_bank(bank)) {
					report_probe_failure(slot);
					continue;
				}
			}

			unit = &units[slot];

			/* Checks the identify result. */
			if (!identify(bank, drive, data)) {
				report_probe_failure(slot);
				continue;
			}

			unit->bank = bank;
			unit->drive = drive;
			unit->native_cylinders = data[1];
			unit->native_heads = data[3];
			unit->native_sectors = data[6];
			unit->use_lba = (data[49] & 0x0200U) != 0;
			unit->flush_supported =
				(data[83] & 0xc000U) == 0x4000U &&
				(data[83] & 0x1000U) != 0;
			unit->write_cache_known =
				(data[87] & 0xc000U) == 0x4000U;
			unit->write_cache_enabled = unit->write_cache_known &&
						    (data[85] & 0x0020U) != 0;

			/* Handles the unit condition. */
			if (unit->use_lba) {
				sector_count = (uint32_t)data[60] |
					       ((uint32_t)data[61] << 16);
			} else {
				sector_count =
					(uint64_t)data[1] * data[3] * data[6];
			}

			/* Handles the sector count condition. */
			if (sector_count == 0 ||
			    (!unit->use_lba && (unit->native_heads == 0 ||
						unit->native_sectors == 0)))
				continue;
			unit->disk = disk_alloc();

			/* Handles the disk availability. */
			if (unit->disk == NULL)
				continue;

			/* Checks the disk alloc sd name result. */
			if (disk_alloc_sd_name(unit->disk) != 0) {
				(void)disk_destroy(unit->disk);
				unit->disk = NULL;
				continue;
			}

			unit->disk->d_block_count = sector_count;
			unit->disk->d_block_size = 512;
			unit->disk->d_max_transfer_blocks = 255;
			unit->disk->d_ops = &pc98_ide_disk_ops;
			unit->disk->d_data = unit;

			/*
			 * Partition tables are written in the firmware-sensed
			 * geometry, so prefer it over IDENTIFY.  The firmware
			 * enumerates IDE disks in the same bank-major order
			 * this probe uses, so BIOS ID 80h+slot is an exact
			 * pairing.
			 */
			if (bios_dev->heads != 0 && bios_dev->sectors != 0) {
				unit->firmware_heads = bios_dev->heads;
				unit->firmware_sectors = bios_dev->sectors;
			} else {
				unit->firmware_heads = unit->native_heads;
				unit->firmware_sectors = unit->native_sectors;
			}

			/* Checks the disk create result. */
			if (disk_create(unit->disk) == 0) {
				unit->present = 1;
				unit_order[unit_count] = unit;
				unit_count++;
			}
		}
	}

	/* Returns the computed result. */
	return unit_count;
}

/*
 * Implements the drv pc98 ide unit operation.
 */
struct disk *
drv_pc98_ide_unit(
	unsigned ordinal)
{
	/* Returns the computed result. */
	return ordinal < unit_count ? unit_order[ordinal]->disk : NULL;
}

/*
 * Implements the drv pc98 ide bios unit operation.
 */
struct disk *
drv_pc98_ide_bios_unit(
	uint8_t bios_id)
{
	unsigned slot;

	/* Handles the bios id condition. */
	if (bios_id < 0x80U || bios_id >= 0x80U + IDE_UNIT_MAX)
		return NULL;
	slot = bios_id - 0x80U;

	/* Returns the computed result. */
	return units[slot].present ? units[slot].disk : NULL;
}

/* Supports the bios device for slot operation. */
static const struct boot_device *
bios_device_for_slot(
	const struct boot_device *devices,
	unsigned count,
	unsigned slot)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; devices != NULL && i < count; i++) {
		/* Handles the devices condition. */
		if (devices[i].device_class == ZEDBSD_DEV_IDE &&
		    devices[i].bios_id == 0x80U + slot) {
			/* Returns the computed result. */
			return &devices[i];
		}
	}

	/* Reports that no result is available. */
	return NULL;
}

/* Reset one channel before IDENTIFY.  This is the sequence used by the working minimal Linux PC-98 IDE driver: assert SRST for at least 10 us, release it with interrupts disabled, then allow 2 ms for settling. */
static int
reset_bank(
	uint8_t bank)
{
	int error;

	/* Pulses the software reset with interrupts held off. */
	select_bank(bank);
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN | IDE_DEVCTL_SRST);
	delay_10us();
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN);
	delay_2ms();
	failure_stage = "wait after software reset";

	/* Obtains the wait clear result. */
	error = wait_clear(IDE_STATUS_BSY);

	/* Returns the computed result. */
	return error;
}

/* Supports the select bank operation. */
static void
select_bank(
	uint8_t bank)
{
	/*
	 * Select the channel and deliberately clear every other writable bit.
	 * In particular, bit 3 selects DWORD data transfers on later PC-9821
	 * controllers, while this driver always transfers with 16-bit INW/OUTW.
	 *
	 * The original Linux/98 IDE frontend and the small pc98ide block driver
	 * both write exactly 0 or 1 here.  Reusing the value read from 0x432 is
	 * unsafe: it contains capability/status bits as well as mode state, and
	 * firmware is allowed to leave DWORD mode enabled.
	 */
	outb(IDE_BANK_SELECT, (uint8_t)(bank & 1U));
}

/* Supports the outb operation. */
static void
outb(
	uint16_t port,
	uint8_t value)
{
	__asm__ volatile("outb %0, %w1" : : "a"(value), "Nd"(port));
}

/* Four alternate-status reads are the ATA-mandated 400 ns delay.  Using that hardware-timed delay also avoids depending on a calibrated CPU loop this early in boot. */
static void
delay_10us(
	void)
{
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < 25U; i++)
		select_delay();
}

/* Reading the alternate status four times gives the 400ns settle time the ATA specification requires after a drive-select. */
static void
select_delay(
	void)
{
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
}

/* Supports the inb operation. */
static uint8_t
inb(
	uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %w1, %0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the delay 2ms operation. */
static void
delay_2ms(
	void)
{
	unsigned i;

	/* Process each element required by the operation. */
	for (i = 0; i < 200U; i++)
		delay_10us();
}

/* Supports the wait clear operation. */
static int
wait_clear(
	uint8_t mask)
{
	uint32_t spins;

	/* Process each element required by the operation. */
	for (spins = 0; spins < IDE_TIMEOUT_POLLS; spins++) {
		/* Checks the inb result. */
		if (!(inb(IDE_ALT_STATUS) & mask))
			return 1;
		delay_10us();
	}

	/* Succeeded. */
	return 0;
}

/* Supports the report probe failure operation. */
static void
report_probe_failure(
	unsigned slot)
{
	uint8_t status;
	uint8_t error;

	/* Reports the registers as they stood when the probe failed. */
	select_bank((uint8_t)(slot / 2U));
	status = inb(IDE_ALT_STATUS);
	error = (status & IDE_STATUS_ERR) ? inb(IDE_ERROR) : 0;
	hal_printf("ide: probe BIOS=%02X bank=%u drive=%u stage=%s "
		   "status=%02X error=%02X bankctl=%02X\n",
		   0x80U + slot, slot / 2U, slot & 1U, failure_stage, status,
		   error, inb(IDE_BANK_SELECT));
}

/* Supports the identify operation. */
static int
identify(
	uint8_t bank,
	uint8_t drive,
	uint16_t data[256])
{
	struct ide_unit probe;
	unsigned word;
	uint8_t status;

	probe.bank = bank;
	probe.drive = drive;
	select_bank(bank);
	failure_stage = "read initial status";

	/* A floating bus reads 0xff on both units: nothing on this bank. */
	status = inb(IDE_ALT_STATUS);
	if (status == 0xffU)
		return 0;

	/* Checks the select unit result. */
	if (!select_unit(&probe, 0, 0))
		return 0;

	/*
	 * Selecting an absent device makes its sibling answer (or the bus
	 * float); the signature check below rejects both cases.
	 */
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN);
	failure_stage = "issue IDENTIFY";
	outb(IDE_STATUS, IDE_CMD_IDENTIFY);

	/* Checks the operation status. */
	status = inb(IDE_STATUS);
	if (status == 0 || status == 0xffU)
		return 0;

	/* Checks the wait drq result. */
	failure_stage = "wait IDENTIFY DRQ";
	if (!wait_drq())
		return 0;
	failure_stage = "read IDENTIFY data";
	/* Process each element required by the operation. */
	for (word = 0; word < 256; word++)
		data[word] = inw(IDE_DATA);

	/*
	 * ATA disks clear word 0 bit 15.  CFA devices are the intentional
	 * exception; IDENTIFY PACKET devices are not block disks here.
	 */
	failure_stage = "validate IDENTIFY device type";

	/* Checks the identify is cfa result. */
	if ((data[IDE_ID_CONFIG] & 0x8000U) && !identify_is_cfa(data))
		return 0;

	/* Reports operation failure. */
	return 1;
}

/* Supports the select unit operation. */
static int
select_unit(
	const struct ide_unit *unit,
	uint8_t head_bits,
	int lba)
{
	int error;

	select_bank(unit->bank);
	failure_stage = "wait before select";

	/* Checks the wait selectable result. */
	if (!wait_selectable())
		return 0;
	outb(IDE_DRIVE_HEAD,
	     (uint8_t)(0xa0U | (lba ? 0x40U : 0U) | ((unit->drive & 1U) << 4) |
		       (head_bits & 0x0fU)));
	select_delay();
	failure_stage = "wait after select";

	/* Obtains the wait clear result. */
	error = wait_clear(IDE_STATUS_BSY);

	/* Returns the computed result. */
	return error;
}

/* An absent device can leave the shared task-file bus floating at 0xff. That is not a busy device: it must be possible to write DRIVE/HEAD and select a known-present sibling.  Linux libata's ata_sff_busy_wait() uses the same rule. */
static int
wait_selectable(
	void)
{
	uint8_t status;
	uint32_t spins;

	/* Process each element required by the operation. */
	for (spins = 0; spins < IDE_TIMEOUT_POLLS; spins++) {
		/* Checks the operation status. */
		status = inb(IDE_ALT_STATUS);
		if (status == 0xffU || !(status & IDE_STATUS_BSY))
			return 1;
		delay_10us();
	}

	/* Succeeded. */
	return 0;
}

/* Wait for BSY to drop and DRQ to rise; fails on ERR or timeout. */
static int
wait_drq(
	void)
{
	uint8_t status;
	uint32_t spins;

	/* Process each element required by the operation. */
	for (spins = 0; spins < IDE_TIMEOUT_POLLS; spins++) {
		/* Checks the operation status. */
		status = inb(IDE_ALT_STATUS);
		if (status & IDE_STATUS_BSY) {
			delay_10us();
			continue;
		}

		/* Checks the operation status. */
		if (status & (IDE_STATUS_DF | IDE_STATUS_ERR))
			return 0;

		/* Checks the operation status. */
		if (status & IDE_STATUS_DRQ)
			return 1;
		delay_10us();
	}

	/* Succeeded. */
	return 0;
}

/* Supports the inw operation. */
static uint16_t
inw(
	uint16_t port)
{
	uint16_t value;

	__asm__ volatile("inw %w1, %0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* CompactFlash in True IDE mode executes ATA IDENTIFY DEVICE, but its general-configuration word is allowed to have bit 15 set.  Linux libata recognizes the two traditional values as well as newer cards which report the CFA feature set in word 83. */
static int
identify_is_cfa(
	const uint16_t data[256])
{
	/* Returns the computed result. */
	return data[IDE_ID_CONFIG] == 0x848aU ||
	       data[IDE_ID_CONFIG] == 0x844aU ||
	       (data[IDE_ID_COMMAND_SET_2] & 0xc004U) == IDE_ID_CFA_FEATURE;
}

/* Supports the outw operation. */
static void
outw(
	uint16_t port,
	uint16_t value)
{
	__asm__ volatile("outw %0, %w1" : : "a"(value), "Nd"(port));
}

/* Program the address registers for one chunk.  LBA28 when the drive supports it, CHS in the drive's native geometry otherwise. */
static int
setup_transfer(
	const struct ide_unit *unit,
	uint64_t lba,
	uint32_t count)
{
	uint32_t spt;
	uint32_t heads;
	uint32_t sect;
	uint32_t head;
	uint32_t cyl;

	/* Handles the unit condition. */
	if (unit->use_lba) {
		/* Checks the select unit result. */
		if (!select_unit(unit, (uint8_t)((lba >> 24) & 0x0fU), 1))
			return 0;
		outb(IDE_NSECT, (uint8_t)count);
		outb(IDE_LBA_LOW, (uint8_t)lba);
		outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
		outb(IDE_LBA_HIGH, (uint8_t)(lba >> 16));
	} else {
		spt = unit->native_sectors;
		heads = unit->native_heads;
		sect = (uint32_t)(lba % spt) + 1U;
		head = (uint32_t)((lba / spt) % heads);
		cyl = (uint32_t)(lba / ((uint64_t)spt * heads));

		/* Checks the select unit result. */
		if (!select_unit(unit, (uint8_t)head, 0))
			return 0;
		outb(IDE_NSECT, (uint8_t)count);
		outb(IDE_LBA_LOW, (uint8_t)sect);
		outb(IDE_LBA_MID, (uint8_t)cyl);
		outb(IDE_LBA_HIGH, (uint8_t)(cyl >> 8));
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the pio read operation. */
static int
pio_read(
	struct disk *dev,
	uint64_t lba,
	uint32_t count,
	void *buffer)
{
	unsigned word;
	uint32_t chunk;
	uint32_t sector;
	struct ide_unit *unit = dev->d_data;
	uint16_t *out = buffer;

	/* Process each remaining element. */
	while (count > 0) {
		/* nsect is 8-bit; 0 would mean 256, keep chunks explicit. */

		/* Checks the setup transfer result. */
		chunk = count > 255U ? 255U : count;
		if (!setup_transfer(unit, lba, chunk))
			return EIO;
		outb(IDE_STATUS, IDE_CMD_READ);
		/* Process each element required by the operation. */
		for (sector = 0; sector < chunk; sector++) {
			failure_stage = "wait read DRQ";

			/* Checks the wait drq result. */
			if (!wait_drq())
				return EIO;
			/* Process each element required by the operation. */
			for (word = 0; word < 256; word++)
				*out++ = inw(IDE_DATA);
		}

		lba += chunk;
		count -= chunk;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the pio write operation. */
static int
pio_write(
	struct disk *dev,
	uint64_t lba,
	uint32_t count,
	const void *buffer)
{
	unsigned word;
	uint32_t chunk;
	uint32_t sector;
	struct ide_unit *unit = dev->d_data;
	const uint16_t *in = buffer;

	/* Process each remaining element. */
	while (count > 0) {
		/* Checks the setup transfer result. */
		chunk = count > 255U ? 255U : count;
		if (!setup_transfer(unit, lba, chunk))
			return EIO;
		outb(IDE_STATUS, IDE_CMD_WRITE);
		/* Process each element required by the operation. */
		for (sector = 0; sector < chunk; sector++) {
			failure_stage = "wait write DRQ";

			/* Checks the wait drq result. */
			if (!wait_drq())
				return EIO;
			/* Process each element required by the operation. */
			for (word = 0; word < 256; word++)
				outw(IDE_DATA, *in++);
		}

		failure_stage = "wait write completion";

		/* Checks the wait clear result. */
		if (!wait_clear(IDE_STATUS_BSY) ||
		    (inb(IDE_ALT_STATUS) & (IDE_STATUS_DF | IDE_STATUS_ERR))) {
			/* Failed. */
			return EIO;
		}
		lba += chunk;
		count -= chunk;
	}

	/* Succeeded. */
	return 0;
}

/* Supports the pio flush operation. */
static int
pio_flush(
	struct disk *dev)
{
	struct ide_unit *unit = dev->d_data;
	uint8_t status;

	/* Handles the unit condition. */
	if (!unit->flush_supported) {
		/*
		 * A valid disabled-cache report needs no media flush command.
		 */
		return unit->write_cache_known && !unit->write_cache_enabled
			       ? 0
			       : EOPNOTSUPP;
	}

	/* Checks the select unit result. */
	if (!select_unit(unit, 0, unit->use_lba))
		return EIO;
	failure_stage = "issue FLUSH CACHE";
	outb(IDE_STATUS, IDE_CMD_FLUSH_CACHE);
	failure_stage = "wait FLUSH CACHE completion";

	/* Checks the wait clear result. */
	if (!wait_clear(IDE_STATUS_BSY))
		return EIO;
	status = inb(IDE_ALT_STATUS);

	/* Returns the computed result. */
	return (status & (IDE_STATUS_DF | IDE_STATUS_ERR)) != 0 ? EIO : 0;
}

/* Supports the pc98 ide submit operation. */
static int
pc98_ide_submit(
	struct disk *dev,
	struct bio *bio)
{
	uint8_t status;
	uint8_t ata_error;
	struct ide_unit *unit = dev->d_data;
	int error;
	size_t transferred = 0;

	/* Keep bank selection, PIO data and status sampling in one command. */
	mutex_lock(&controller_lock);
	failure_stage = "request";

	/* Handles the bio condition. */
	if (bio->b_op == BIO_READ) {
		error = pio_read(dev, bio->b_mapped_block, bio->b_block_count,
				 bio->b_data);
	} else if (bio->b_op == BIO_WRITE) {
		error = pio_write(dev, bio->b_mapped_block, bio->b_block_count,
				  bio->b_data);
	} else if (bio->b_op == BIO_FLUSH)
		error = pio_flush(dev);
	else {
		/* Failed. */
		mutex_unlock(&controller_lock);
		return EOPNOTSUPP;
	}

	/* Checks the operation status. */
	if (error != 0) {
		select_bank(unit->bank);
		status = inb(IDE_ALT_STATUS);
		ata_error = (status & IDE_STATUS_ERR) ? inb(IDE_ERROR) : 0;
		hal_printf("ide: %s %s LBA=%u count=%u bank=%u drive=%u "
			   "stage=%s status=%02X error=%02X bankctl=%02X\n",
			   dev->d_name,
			   bio->b_op == BIO_READ    ? "read"
			   : bio->b_op == BIO_WRITE ? "write"
						    : "flush",
			   (uint32_t)bio->b_mapped_block, bio->b_block_count,
			   unit->bank, unit->drive, failure_stage, status,
			   ata_error, inb(IDE_BANK_SELECT));
	}

	/* Checks the operation status. */
	if (error == 0)
		transferred = (size_t)bio->b_block_count * dev->d_block_size;
	mutex_unlock(&controller_lock);

	/* Completion may issue another request; do it after releasing the bus. */
	bio_complete(bio, error, transferred);

	/* Succeeded. */
	return 0;
}

/* Supports the pc98 ide ioctl operation. */
static int
pc98_ide_ioctl(
	struct disk *dev,
	unsigned long request,
	void *argument)
{
	struct ide_unit *unit = dev->d_data;
	struct disk_geometry *geometry = argument;

	/* Handles the request condition. */
	if (request != DISK_IOCTL_GET_GEOMETRY)
		return EOPNOTSUPP;

	/* Handles the geometry availability. */
	if (geometry == NULL)
		return EINVAL;
	geometry->cylinders = unit->native_cylinders;
	geometry->heads = unit->native_heads;
	geometry->sectors_per_track = unit->native_sectors;

	/* Firmware geometry, when present, is stored separately below. */
	if (unit->firmware_heads != 0) {
		geometry->heads = unit->firmware_heads;
		geometry->sectors_per_track = unit->firmware_sectors;
	}

	/* Succeeded. */
	return 0;
}
