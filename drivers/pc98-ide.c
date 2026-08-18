/*
 * NEC PC-98 internal IDE driver (PIO, polled)
 * Copyright (C) 2026 Awe Morris
 *
 * - The PC-98 built-in IDE multiplexes two ATA channels.
 * - Each with a master/slave pair onto one register block.
 * - Port 0x432 bit 0 selects the active bank.
 * - The command block lives at 0x640-0x64e with a 2-byte stride
 *   (16-bit data at 0x640), and the control block at 0x74c/0x74e.
 * - Reference model: qemu-pc98 hw/ide/pc98-ide.c
 * - Reference driver: linux-pc98 drivers/ata/pata_pc9800.c.
 * - Interrupts stay disabled (nIEN), everything is polled.
 *
 * SPDX-License-Identifier: Zlib
 */

#include "drivers/pc98-ide.h"
#include "kern/boot.h"
#include <errno.h>
#include <hal/hal.h>

#define IDE_BANK_SELECT   0x432U
#define IDE_DATA          0x640U
#define IDE_ERROR         0x642U
#define IDE_NSECT         0x644U
#define IDE_LBA_LOW       0x646U
#define IDE_LBA_MID       0x648U
#define IDE_LBA_HIGH      0x64aU
#define IDE_DRIVE_HEAD    0x64cU
#define IDE_STATUS        0x64eU   /* read: status, write: command */
#define IDE_ALT_STATUS    0x74cU   /* read: alt status, write: devctl */

#define IDE_STATUS_BSY    0x80U
#define IDE_STATUS_DRDY   0x40U
#define IDE_STATUS_DF     0x20U
#define IDE_STATUS_DRQ    0x08U
#define IDE_STATUS_ERR    0x01U

#define IDE_CMD_READ      0x20U
#define IDE_CMD_WRITE     0x30U
#define IDE_CMD_IDENTIFY  0xecU
#define IDE_CMD_FLUSH_CACHE 0xe7U

#define IDE_DEVCTL_NIEN   0x02U
#define IDE_DEVCTL_SRST   0x04U

#define IDE_ID_CONFIG        0U
#define IDE_ID_COMMAND_SET_2 83U
#define IDE_ID_CFA_FEATURE   0x4004U

#define IDE_UNIT_MAX      4U

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

static uint8_t
inb(uint16_t port)
{
	uint8_t value;

	__asm__ volatile ("inb %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
outb(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0, %w1" : : "a"(value), "Nd"(port));
}

static uint16_t
inw(uint16_t port)
{
	uint16_t value;

	__asm__ volatile ("inw %w1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
outw(uint16_t port, uint16_t value)
{
	__asm__ volatile ("outw %0, %w1" : : "a"(value), "Nd"(port));
}

static void
select_bank(uint8_t bank)
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

/* Reading the alternate status four times gives the 400ns settle time
 * the ATA specification requires after a drive-select. */
static void
select_delay(void)
{
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
	(void)inb(IDE_ALT_STATUS);
}

/* Four alternate-status reads are the ATA-mandated 400 ns delay.  Using
 * that hardware-timed delay also avoids depending on a calibrated CPU loop
 * this early in boot. */
static void
delay_10us(void)
{
	unsigned i;

	for (i = 0; i < 25U; i++)
		select_delay();
}

static void
delay_2ms(void)
{
	unsigned i;

	for (i = 0; i < 200U; i++)
		delay_10us();
}

static int
wait_clear(uint8_t mask)
{
	uint32_t spins;

	for (spins = 0; spins < IDE_TIMEOUT_POLLS; spins++) {
		if (!(inb(IDE_ALT_STATUS) & mask))
			return 1;
		delay_10us();
	}
	return 0;
}

/*
 * An absent device can leave the shared task-file bus floating at 0xff.
 * That is not a busy device: it must be possible to write DRIVE/HEAD and
 * select a known-present sibling.  Linux libata's ata_sff_busy_wait() uses
 * the same rule.
 */
static int
wait_selectable(void)
{
	uint32_t spins;

	for (spins = 0; spins < IDE_TIMEOUT_POLLS; spins++) {
		uint8_t status = inb(IDE_ALT_STATUS);

		if (status == 0xffU || !(status & IDE_STATUS_BSY))
			return 1;
		delay_10us();
	}
	return 0;
}

/* Wait for BSY to drop and DRQ to rise; fails on ERR or timeout. */
static int
wait_drq(void)
{
	uint32_t spins;

	for (spins = 0; spins < IDE_TIMEOUT_POLLS; spins++) {
		uint8_t status = inb(IDE_ALT_STATUS);

		if (status & IDE_STATUS_BSY) {
			delay_10us();
			continue;
		}
		if (status & (IDE_STATUS_DF | IDE_STATUS_ERR))
			return 0;
		if (status & IDE_STATUS_DRQ)
			return 1;
		delay_10us();
	}
	return 0;
}

/* Reset one channel before IDENTIFY.  This is the sequence used by the
 * working minimal Linux PC-98 IDE driver: assert SRST for at least 10 us,
 * release it with interrupts disabled, then allow 2 ms for settling. */
static int
reset_bank(uint8_t bank)
{
	select_bank(bank);
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN | IDE_DEVCTL_SRST);
	delay_10us();
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN);
	delay_2ms();
	failure_stage = "wait after software reset";
	return wait_clear(IDE_STATUS_BSY);
}

static int
select_unit(const struct ide_unit *unit, uint8_t head_bits, int lba)
{
	select_bank(unit->bank);
	failure_stage = "wait before select";
	if (!wait_selectable())
		return 0;
	outb(IDE_DRIVE_HEAD, (uint8_t)(0xa0U | (lba ? 0x40U : 0U) |
				       ((unit->drive & 1U) << 4) |
				       (head_bits & 0x0fU)));
	select_delay();
	failure_stage = "wait after select";
	return wait_clear(IDE_STATUS_BSY);
}

/*
 * Program the address registers for one chunk.  LBA28 when the drive
 * supports it, CHS in the drive's native geometry otherwise.
 */
static int
setup_transfer(const struct ide_unit *unit, uint64_t lba, uint32_t count)
{
	if (unit->use_lba) {
		if (!select_unit(unit, (uint8_t)((lba >> 24) & 0x0fU), 1))
			return 0;
		outb(IDE_NSECT, (uint8_t)count);
		outb(IDE_LBA_LOW, (uint8_t)lba);
		outb(IDE_LBA_MID, (uint8_t)(lba >> 8));
		outb(IDE_LBA_HIGH, (uint8_t)(lba >> 16));
	} else {
		uint32_t spt = unit->native_sectors;
		uint32_t heads = unit->native_heads;
		uint32_t sect = (uint32_t)(lba % spt) + 1U;
		uint32_t head = (uint32_t)((lba / spt) % heads);
		uint32_t cyl = (uint32_t)(lba / ((uint64_t)spt * heads));

		if (!select_unit(unit, (uint8_t)head, 0))
			return 0;
		outb(IDE_NSECT, (uint8_t)count);
		outb(IDE_LBA_LOW, (uint8_t)sect);
		outb(IDE_LBA_MID, (uint8_t)cyl);
		outb(IDE_LBA_HIGH, (uint8_t)(cyl >> 8));
	}
	return 1;
}

static int
pio_read(struct disk *dev, uint64_t lba, uint32_t count, void *buffer)
{
	struct ide_unit *unit = dev->d_data;
	uint16_t *out = buffer;

	while (count > 0) {
		/* nsect is 8-bit; 0 would mean 256, keep chunks explicit. */
		uint32_t chunk = count > 255U ? 255U : count;
		uint32_t sector;

		if (!setup_transfer(unit, lba, chunk))
			return EIO;
		outb(IDE_STATUS, IDE_CMD_READ);
		for (sector = 0; sector < chunk; sector++) {
			unsigned word;

			failure_stage = "wait read DRQ";
			if (!wait_drq())
				return EIO;
			for (word = 0; word < 256; word++)
				*out++ = inw(IDE_DATA);
		}
		lba += chunk;
		count -= chunk;
	}
	return 0;
}

static int
pio_write(struct disk *dev, uint64_t lba, uint32_t count,
	  const void *buffer)
{
	struct ide_unit *unit = dev->d_data;
	const uint16_t *in = buffer;

	while (count > 0) {
		uint32_t chunk = count > 255U ? 255U : count;
		uint32_t sector;

		if (!setup_transfer(unit, lba, chunk))
			return EIO;
		outb(IDE_STATUS, IDE_CMD_WRITE);
		for (sector = 0; sector < chunk; sector++) {
			unsigned word;

			failure_stage = "wait write DRQ";
			if (!wait_drq())
				return EIO;
			for (word = 0; word < 256; word++)
				outw(IDE_DATA, *in++);
		}
		failure_stage = "wait write completion";
		if (!wait_clear(IDE_STATUS_BSY) ||
		    (inb(IDE_ALT_STATUS) & (IDE_STATUS_DF | IDE_STATUS_ERR)))
			return EIO;
		lba += chunk;
		count -= chunk;
	}
	return 0;
}

static int
pio_flush(struct disk *dev)
{
	struct ide_unit *unit = dev->d_data;
	uint8_t status;

	if (!unit->flush_supported) {
		/* A valid disabled-cache report needs no media flush command. */
		return unit->write_cache_known && !unit->write_cache_enabled ?
			0 : EOPNOTSUPP;
	}
	if (!select_unit(unit, 0, unit->use_lba))
		return EIO;
	failure_stage = "issue FLUSH CACHE";
	outb(IDE_STATUS, IDE_CMD_FLUSH_CACHE);
	failure_stage = "wait FLUSH CACHE completion";
	if (!wait_clear(IDE_STATUS_BSY))
		return EIO;
	status = inb(IDE_ALT_STATUS);
	return (status & (IDE_STATUS_DF | IDE_STATUS_ERR)) != 0 ? EIO : 0;
}

static int
pc98_ide_submit(struct disk *dev, struct bio *bio)
{
	struct ide_unit *unit = dev->d_data;
	int error;
	size_t transferred = 0;

	failure_stage = "request";

	if (bio->b_op == BIO_READ)
		error = pio_read(dev, bio->b_mapped_block,
				 bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_WRITE)
		error = pio_write(dev, bio->b_mapped_block,
				  bio->b_block_count, bio->b_data);
	else if (bio->b_op == BIO_FLUSH)
		error = pio_flush(dev);
	else
		return EOPNOTSUPP;
	if (error != 0) {
		uint8_t status;
		uint8_t ata_error;

		select_bank(unit->bank);
		status = inb(IDE_ALT_STATUS);
		ata_error = (status & IDE_STATUS_ERR) ? inb(IDE_ERROR) : 0;
		hal_printf("ide: %s %s LBA=%u count=%u bank=%u drive=%u "
		    "stage=%s status=%02X error=%02X bankctl=%02X\n",
		    dev->d_name, bio->b_op == BIO_READ ? "read" :
		    bio->b_op == BIO_WRITE ? "write" : "flush",
		    (uint32_t)bio->b_mapped_block, bio->b_block_count,
		    unit->bank, unit->drive, failure_stage, status, ata_error,
		    inb(IDE_BANK_SELECT));
	}
	if (error == 0)
		transferred = (size_t)bio->b_block_count * dev->d_block_size;
	bio_complete(bio, error, transferred);
	return 0;
}

static int
pc98_ide_ioctl(struct disk *dev, unsigned long request, void *argument)
{
	struct ide_unit *unit = dev->d_data;
	struct disk_geometry *geometry = argument;

	if (request != DISK_IOCTL_GET_GEOMETRY)
		return EOPNOTSUPP;
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
	return 0;
}

static const struct disk_ops pc98_ide_disk_ops = {
	.submit = pc98_ide_submit,
	.ioctl = pc98_ide_ioctl,
};

/* CompactFlash in True IDE mode executes ATA IDENTIFY DEVICE, but its
 * general-configuration word is allowed to have bit 15 set.  Linux libata
 * recognizes the two traditional values as well as newer cards which report
 * the CFA feature set in word 83. */
static int
identify_is_cfa(const uint16_t data[256])
{
	return data[IDE_ID_CONFIG] == 0x848aU ||
	    data[IDE_ID_CONFIG] == 0x844aU ||
	    (data[IDE_ID_COMMAND_SET_2] & 0xc004U) == IDE_ID_CFA_FEATURE;
}

static int
identify(uint8_t bank, uint8_t drive, uint16_t data[256])
{
	struct ide_unit probe;
	unsigned word;
	uint8_t status;

	probe.bank = bank;
	probe.drive = drive;
	select_bank(bank);
	failure_stage = "read initial status";
	status = inb(IDE_ALT_STATUS);
	/* A floating bus reads 0xff on both units: nothing on this bank. */
	if (status == 0xffU)
		return 0;
	if (!select_unit(&probe, 0, 0))
		return 0;
	/* Selecting an absent device makes its sibling answer (or the bus
	 * float); the signature check below rejects both cases. */
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN);
	failure_stage = "issue IDENTIFY";
	outb(IDE_STATUS, IDE_CMD_IDENTIFY);
	status = inb(IDE_STATUS);
	if (status == 0 || status == 0xffU)
		return 0;
	failure_stage = "wait IDENTIFY DRQ";
	if (!wait_drq())
		return 0;
	failure_stage = "read IDENTIFY data";
	for (word = 0; word < 256; word++)
		data[word] = inw(IDE_DATA);
	/* ATA disks clear word 0 bit 15.  CFA devices are the intentional
	 * exception; IDENTIFY PACKET devices are not block disks here. */
	failure_stage = "validate IDENTIFY device type";
	if ((data[IDE_ID_CONFIG] & 0x8000U) && !identify_is_cfa(data))
		return 0;
	return 1;
}

static const struct zedbsd_device *
bios_device_for_slot(const struct zedbsd_device *devices, unsigned count,
		     unsigned slot)
{
	unsigned i;

	for (i = 0; devices != NULL && i < count; i++)
		if (devices[i].device_class == ZEDBSD_DEV_IDE &&
		    devices[i].bios_id == 0x80U + slot)
			return &devices[i];
	return NULL;
}

static void
report_probe_failure(unsigned slot)
{
	uint8_t status;
	uint8_t error;

	select_bank((uint8_t)(slot / 2U));
	status = inb(IDE_ALT_STATUS);
	error = (status & IDE_STATUS_ERR) ? inb(IDE_ERROR) : 0;
	hal_printf("ide: probe BIOS=%02X bank=%u drive=%u stage=%s "
	    "status=%02X error=%02X bankctl=%02X\n", 0x80U + slot,
	    slot / 2U, slot & 1U, failure_stage, status, error,
	    inb(IDE_BANK_SELECT));
}

static void
set_name(struct ide_unit *unit, unsigned ordinal)
{
	unit->disk->d_name[0] = 'i';
	unit->disk->d_name[1] = 'd';
	unit->disk->d_name[2] = 'e';
	unit->disk->d_name[3] = (char)('0' + ordinal);
	unit->disk->d_name[4] = '\0';
	unit->disk->d_name[DISK_NAME_MAX - 1U] = '\0';
}

unsigned
zedbsd_ide_pc98_init(const struct zedbsd_device *bios_devices,
		     unsigned bios_device_count)
{
	static uint16_t data[256];
	uint8_t reset_done[2] = { 0, 0 };
	unsigned pass;
	unsigned slot;

	unit_count = 0;
	for (slot = 0; slot < IDE_UNIT_MAX; slot++) {
		units[slot].present = 0;
		units[slot].disk = NULL;
		unit_order[slot] = NULL;
	}
	/* Probe only BIOS-advertised units, with the boot-origin unit first.
	 * Besides avoiding hangs on floating secondary channels, this preserves
	 * the BIOS-to-native mapping used by VFS. */
	for (pass = 0; pass < 2; pass++) {
		for (slot = 0; slot < IDE_UNIT_MAX; slot++) {
			struct ide_unit *unit;
			const struct zedbsd_device *bios_dev;
			uint64_t sector_count;
			uint8_t bank = (uint8_t)(slot / 2U);
			uint8_t drive = (uint8_t)(slot & 1U);

			bios_dev = bios_device_for_slot(bios_devices,
			    bios_device_count, slot);
			if (bios_dev == NULL ||
			    (((bios_dev->flags & ZEDBSD_DEV_BOOT_ORIGIN) != 0) !=
			     (pass == 0)))
				continue;
			if (!reset_done[bank]) {
				reset_done[bank] = 1;
				if (!reset_bank(bank)) {
					report_probe_failure(slot);
					continue;
				}
			}
			unit = &units[slot];
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
			if (unit->use_lba) {
				sector_count =
					(uint32_t)data[60] |
					((uint32_t)data[61] << 16);
			} else {
				sector_count =
					(uint64_t)data[1] * data[3] * data[6];
			}
			if (sector_count == 0 ||
			    (!unit->use_lba &&
			     (unit->native_heads == 0 ||
			      unit->native_sectors == 0)))
				continue;
			unit->disk = disk_alloc();
			if (unit->disk == NULL)
				continue;
			unit->disk->d_block_count = sector_count;
			unit->disk->d_block_size = 512;
			unit->disk->d_max_transfer_blocks = 255;
			unit->disk->d_ops = &pc98_ide_disk_ops;
			unit->disk->d_data = unit;
			set_name(unit, unit_count);
			/*
			 * Partition tables are written in the firmware-sensed
			 * geometry, so prefer it over IDENTIFY.  The firmware
			 * enumerates IDE disks in the same bank-major order
			 * this probe uses, so BIOS ID 80h+slot is an exact pairing.
			 */
			if (bios_dev->heads != 0 &&
			    bios_dev->sectors != 0) {
				unit->firmware_heads = bios_dev->heads;
				unit->firmware_sectors = bios_dev->sectors;
			} else {
				unit->firmware_heads = unit->native_heads;
				unit->firmware_sectors = unit->native_sectors;
			}
			if (disk_create(unit->disk) == 0) {
				unit->present = 1;
				unit_order[unit_count] = unit;
				unit_count++;
			}
		}
	}
	return unit_count;
}

struct disk *
zedbsd_ide_pc98_unit(unsigned ordinal)
{
	return ordinal < unit_count ? unit_order[ordinal]->disk : NULL;
}

struct disk *
zedbsd_ide_pc98_bios_unit(uint8_t bios_id)
{
	unsigned slot;

	if (bios_id < 0x80U || bios_id >= 0x80U + IDE_UNIT_MAX)
		return NULL;
	slot = bios_id - 0x80U;
	return units[slot].present ? units[slot].disk : NULL;
}
