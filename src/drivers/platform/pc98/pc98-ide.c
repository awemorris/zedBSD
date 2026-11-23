/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * NEC PC-98 internal IDE driver (PIO)
 *
 * - The PC-98 built-in IDE multiplexes two ATA channels.
 * - Each with a master/slave pair onto one register block.
 * - Port 0x432 bit 0 selects the active bank.
 * - The command block lives at 0x640-0x64e with a 2-byte stride
 *   (16-bit data at 0x640), and the control block at 0x74c/0x74e.
 * - The reset and IDENTIFY of the probe are polled with interrupts off
 *   (nIEN).  Afterwards a wait the drive does not end at once sleeps until
 *   the drive interrupts: ATA devices hold an interrupt when a PIO sector
 *   is ready to move and when a command ends, assert INTRQ for it while
 *   nIEN is clear, and drop it when the status register is read.  Both
 *   channels drive IRQ 9 on PC-98 boards.
 * - A drive that finishes a command without an interrupt reaching this
 *   driver (a board wired otherwise) is reported once, and every command
 *   is polled from then on.
 */

#include "drivers/platform/pc98/pc98-ide.h"
#include "kern/boot.h"
#include "kern/clock.h"
#include "kern/irq.h"
#include "kern/lock.h"
#include "kern/sched.h"
#include "kern/waitq.h"
#include <uapi/errno.h>
#include <uapi/sysctl.h>
#include "kern/klog.h"

/*
 * The register block.
 *
 * Values referenced: qemu-pc98 hw/ide/pc98-ide.c and linux-pc98
 * drivers/ata/pata_pc9800.c (the bank select port and the register
 * addresses).
 */
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

/*
 * The interrupt both banks of the built-in IDE raise.
 *
 * Values referenced: Linux 2.6.7 PC-9800 include/asm-i386/ide.h
 * (ide_default_irq() gives 9 for the 0x640 command block), linux-pc98
 * drivers/ata/pata_pc9800.c (PC98_ATA_IRQ), and qemu-pc98
 * hw/i386/pc98.c and hw/ide/pc98-ide.c (both channels on IRQ 9).
 */
#define IDE_IRQ 9

/*
 * How long a command waits asleep before it looks at the drive without an
 * interrupt, and how long it may take in all, as long as the polled wait.
 */
#define IDE_IRQ_CHECK_MS 250U
#define IDE_IRQ_LIMIT_MS 5000U

/*
 * How many times a wait looks at the drive, about 10 microseconds apart,
 * before it sleeps for the interrupt: about the time a sector of a transfer
 * under way takes to come, and less than a sleep and a wakeup cost a
 * 386SX.
 */
#define IDE_SPIN_POLLS 16U

/* What an interrupt-driven wait found. */
#define IDE_WAIT_READY 0
#define IDE_WAIT_FAILED 1
#define IDE_WAIT_UNANSWERED 2

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
	/* FLUSH CACHE is in the command set (IDENTIFY word 83 bit 12). */
	uint8_t flush_supported;
};

static struct ide_unit units[IDE_UNIT_MAX];
static struct ide_unit *unit_order[IDE_UNIT_MAX];
static unsigned unit_count;
static const char *failure_stage;
/* Both ATA banks share one register window and must serialize commands. */
static struct mutex controller_lock;

/*
 * The interrupts of the controller: interrupt_count counts the ones the
 * handler took, and the command that waits sleeps on interrupt_wait until
 * it changes.  Both are under interrupt_lock.
 */
static struct spinlock interrupt_lock;
static struct wait_queue interrupt_wait;
static uint32_t interrupt_count;

/*
 * The count a command has consumed.  Read and written by the command that
 * holds controller_lock.
 */
static uint32_t interrupt_seen;

/*
 * Set while commands wait for the interrupt; cleared for good when one
 * finished without it.  Read and written under controller_lock.
 */
static int interrupts_used;


static const struct kern_boot_device * bios_device_for_slot(const struct kern_boot_device *devices, unsigned count, unsigned slot);
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
static void read_sector_data(uint16_t *buffer);
static void write_sector_data(const uint16_t *buffer);
static int setup_transfer(const struct ide_unit *unit, uint64_t lba, uint32_t count);
static int pio_read(struct disk *dev, uint64_t lba, uint32_t count, void *buffer);
static int pio_write(struct disk *dev, uint64_t lba, uint32_t count, const void *buffer);
static int pio_flush(struct disk *dev);
static void start_interrupts(void);
static void ide_interrupt(int irq, kern_irq_ack_t acknowledge, void *argument);
static int wait_command(int want_drq);
static int wait_interrupt(int want_drq);
static void allow_interrupt(int allowed);
static int take_interrupts(uint64_t deadline);
static void stop_interrupts(void);
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
	const struct kern_boot_device *bios_devices,
	unsigned bios_device_count)
{
	struct ide_unit *unit;
	const struct kern_boot_device *bios_dev;
	uint64_t sector_count;
	uint8_t bank;
	uint8_t drive;
	static uint16_t data[256];
	uint8_t reset_done[2] = {0, 0};
	unsigned pass;
	unsigned slot;

	(void)mutex_init(&controller_lock, LOCK_RANK_DISK, "pc98-ide");
	spin_init(&interrupt_lock, LOCK_RANK_DISK, "pc98-ide interrupt");
	waitq_init(&interrupt_wait, "pc98-ide interrupt");
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
			    (((bios_dev->flags & KERN_DEV_BOOT_ORIGIN) !=
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

			/* hw.diskstats lists it as an IDE disk (ws134-p006). */
			disk_set_stats_kind(unit->disk, DISK_STATS_KIND_IDE);

			/* Checks the disk create result. */
			if (disk_create(unit->disk) == 0) {
				unit->present = 1;
				unit_order[unit_count] = unit;
				unit_count++;
				if (!unit->flush_supported)
					kern_logf("ide: %s has no FLUSH CACHE; "
						   "flushes complete with the "
						   "writes\n",
						   unit->disk->d_name);
			}
		}
	}

	/* The units found wait for their interrupt from now on. */
	if (unit_count != 0)
		start_interrupts();

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
static const struct kern_boot_device *
bios_device_for_slot(
	const struct kern_boot_device *devices,
	unsigned count,
	unsigned slot)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; devices != NULL && i < count; i++) {
		/* Handles the devices condition. */
		if (devices[i].device_class == KERN_DEV_IDE &&
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
	kern_logf("ide: probe BIOS=%02X bank=%u drive=%u stage=%s "
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

/*
 * Reads one sector's 256 data words with a single string instruction.
 *
 * A word loop in C spends several instructions per word besides the port
 * read, which on a 386SX without a cache costs more than the ISA cycle
 * itself; REP INSW leaves only the bus cycle.  Linux libata moves PIO data
 * the same way (ata_sff_data_xfer(): ioread16_rep/iowrite16_rep on the data
 * port).
 */
static void
read_sector_data(
	uint16_t *buffer)
{
	uint32_t words;

	/* Moves the sector from the data port into the buffer. */
	words = 256U;
	__asm__ volatile("rep insw"
			 : "+D"(buffer), "+c"(words)
			 : "d"((uint16_t)IDE_DATA)
			 : "memory");
}

/*
 * Writes one sector's 256 data words with a single string instruction, for
 * the same reason as read_sector_data().
 */
static void
write_sector_data(
	const uint16_t *buffer)
{
	uint32_t words;

	/* Moves the sector from the buffer to the data port. */
	words = 256U;
	__asm__ volatile("rep outsw"
			 : "+S"(buffer), "+c"(words)
			 : "d"((uint16_t)IDE_DATA)
			 : "memory");
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

		/* Issues the read; the drive interrupts as each sector is ready. */
		outb(IDE_STATUS, IDE_CMD_READ);
		/* Process each element required by the operation. */
		for (sector = 0; sector < chunk; sector++) {
			failure_stage = "wait read DRQ";

			/* Checks the wait drq result. */
			if (!wait_command(1))
				return EIO;

			/* Takes the sector the drive has ready. */
			read_sector_data(out);
			out += 256;
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

		/*
		 * Issues the write.  The drive asks for the first sector
		 * without an interrupt, and interrupts after each sector it
		 * took: for the next one, or at the end of the command.
		 */
		outb(IDE_STATUS, IDE_CMD_WRITE);
		/* Process each element required by the operation. */
		for (sector = 0; sector < chunk; sector++) {
			failure_stage = "wait write DRQ";

			/* The first sector is polled for, the others are waited for. */
			if (sector == 0) {
				if (!wait_drq())
					return EIO;
			} else if (!wait_command(1)) {
				return EIO;
			}

			/* Hands the drive the sector it asked for. */
			write_sector_data(in);
			in += 256;
		}

		failure_stage = "wait write completion";

		/* Checks the wait clear result. */
		if (!wait_command(0) ||
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

	/*
	 * A drive without FLUSH CACHE offers nothing the host can ask it to
	 * commit: a write is done when the drive says it is. The caller has
	 * already waited for every earlier write, so the flush is complete.
	 * Drives before ATA-4, which is every drive a real PC-98 shipped
	 * with, report no command set at all and land here.
	 */
	if (!unit->flush_supported)
		return 0;

	/* Checks the select unit result. */
	if (!select_unit(unit, 0, unit->use_lba))
		return EIO;
	failure_stage = "issue FLUSH CACHE";
	outb(IDE_STATUS, IDE_CMD_FLUSH_CACHE);
	failure_stage = "wait FLUSH CACHE completion";

	/* The drive interrupts when the cache is written. */
	if (!wait_command(0))
		return EIO;
	status = inb(IDE_ALT_STATUS);

	/* Returns the computed result. */
	return (status & (IDE_STATUS_DF | IDE_STATUS_ERR)) != 0 ? EIO : 0;
}

/*
 * Installs the interrupt handler and lets the commands that follow wait
 * for the drive's interrupt.  Without the handler they stay polled.
 */
static void
start_interrupts(
	void)
{
	int error;

	/* Installs the handler while the line is still masked. */
	kern_irq_mask(IDE_IRQ);
	error = kern_irq_register(IDE_IRQ, ide_interrupt, NULL);
	if (error != 0) {
		kern_logf("ide: IRQ %d unavailable (%d); commands are polled\n",
			  IDE_IRQ, error);
		return;
	}

	/* Commands wait for the interrupt, which the line now delivers. */
	mutex_lock(&controller_lock);
	interrupts_used = 1;
	mutex_unlock(&controller_lock);
	kern_irq_unmask(IDE_IRQ);
}

/*
 * Counts an interrupt of the controller and wakes the command waiting for
 * it.  The command reads the status register itself, which releases the
 * drive's INTRQ; the 8259 takes the line by its edge.
 */
static void
ide_interrupt(
	int irq,
	kern_irq_ack_t acknowledge,
	void *argument)
{
	(void)irq;
	(void)argument;

	/* One more interrupt for the waiting command. */
	spin_lock(&interrupt_lock);
	interrupt_count++;
	waitq_wake_all(&interrupt_wait);
	spin_unlock(&interrupt_lock);

	/* Releases the line. */
	kern_irq_send_eoi(acknowledge);
}

/*
 * Waits for the drive to finish its part of a command: to ask for or offer
 * a sector (want_drq), or to end it.  Fails on an error, a fault, or a
 * drive that does not answer in time.
 */
static int
wait_command(
	int want_drq)
{
	int outcome;

	/* Waits asleep for the interrupt while the controller delivers it. */
	if (interrupts_used) {
		outcome = wait_interrupt(want_drq);
		if (outcome == IDE_WAIT_READY)
			return 1;
		if (outcome == IDE_WAIT_FAILED)
			return 0;
	}

	/* A polled controller, or one whose interrupt did not come. */
	if (want_drq) {
		outcome = wait_drq();
		return outcome;
	}

	/* Polls for the end of the command. */
	outcome = wait_clear(IDE_STATUS_BSY);
	return outcome;
}

/*
 * Waits for the drive and judges the status it leaves.  A drive that is
 * ready within IDE_SPIN_POLLS looks is taken at once, as a sector of a
 * transfer under way usually is: on a slow CPU a sleep and a wakeup cost
 * more than that wait.  Otherwise the drive's interrupt is allowed and the
 * command sleeps until it comes.  Every IDE_IRQ_CHECK_MS without one the
 * status is looked at anyway: a drive that became ready without an
 * interrupt shows that its interrupt does not reach this driver, which
 * then polls every command.  Reports IDE_WAIT_READY, IDE_WAIT_FAILED (an
 * error, a fault, or no answer within IDE_IRQ_LIMIT_MS), or
 * IDE_WAIT_UNANSWERED (the interrupt is missing; the caller polls).
 */
static int
wait_interrupt(
	int want_drq)
{
	uint64_t limit;
	uint64_t check;
	uint64_t now;
	uint8_t status;
	unsigned spin;
	int arrived;
	int ready;

	/* The status is valid 400 ns after a command is issued. */
	select_delay();

	/* A drive ready soon is taken without sleeping. */
	for (spin = 0; spin < IDE_SPIN_POLLS; spin++) {
		status = inb(IDE_ALT_STATUS);
		if ((status & IDE_STATUS_BSY) == 0) {
			ready = 1;
			if (want_drq && (status & IDE_STATUS_DRQ) == 0)
				ready = 0;
			if ((status & (IDE_STATUS_DF | IDE_STATUS_ERR)) != 0)
				ready = 1;
			if (ready)
				break;
		}
		delay_10us();
	}

	/* Takes a ready drive, releasing the interrupt it holds. */
	if (spin < IDE_SPIN_POLLS) {
		status = inb(IDE_STATUS);
		if ((status & (IDE_STATUS_DF | IDE_STATUS_ERR)) != 0)
			return IDE_WAIT_FAILED;
		return IDE_WAIT_READY;
	}

	/*
	 * Allows the interrupt.  A drive that finished meanwhile raises it at
	 * once, as an interrupt it holds is passed on when nIEN clears.
	 */
	allow_interrupt(1);

	/* The command may take as long as a polled one. */
	limit = sched_ticks() + kern_ms_to_ticks(IDE_IRQ_LIMIT_MS);
	for (;;) {
		/* Sleeps until an interrupt, or until the next look. */
		check = sched_ticks() + kern_ms_to_ticks(IDE_IRQ_CHECK_MS);
		if (check > limit)
			check = limit;
		arrived = take_interrupts(check);

		/* Reading the status releases the drive's INTRQ. */
		status = inb(IDE_STATUS);
		now = sched_ticks();

		/* A busy drive is not done yet, unless it is out of time. */
		if ((status & IDE_STATUS_BSY) != 0) {
			if (now < limit)
				continue;
			allow_interrupt(0);
			return IDE_WAIT_FAILED;
		}

		/* An error or a fault ends the command. */
		if ((status & (IDE_STATUS_DF | IDE_STATUS_ERR)) != 0) {
			allow_interrupt(0);
			return IDE_WAIT_FAILED;
		}

		/* A drive that should offer data and does not is waited for, like the polled wait. */
		if (want_drq && (status & IDE_STATUS_DRQ) == 0) {
			if (now < limit)
				continue;
			allow_interrupt(0);
			return IDE_WAIT_FAILED;
		}

		/* The drive is done, and its interrupt came. */
		if (arrived) {
			allow_interrupt(0);
			return IDE_WAIT_READY;
		}

		/* Gives an interrupt on its way a moment to arrive. */
		for (spin = 0; spin < 10U; spin++)
			delay_10us();
		arrived = take_interrupts(0);
		if (arrived) {
			allow_interrupt(0);
			return IDE_WAIT_READY;
		}

		/* The drive finished without an interrupt: polls from now on. */
		stop_interrupts();
		return IDE_WAIT_UNANSWERED;
	}
}

/*
 * Allows the selected bank's drives to raise their interrupt, forgetting
 * the interrupts taken before, or stops them from raising it again.
 */
static void
allow_interrupt(
	int allowed)
{
	unsigned long irq;

	/* Stops the drives from interrupting. */
	if (!allowed) {
		outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN);
		return;
	}

	/* The interrupts so far belong to earlier waits. */
	irq = spin_lock_irqsave(&interrupt_lock);
	interrupt_seen = interrupt_count;
	spin_unlock_irqrestore(&interrupt_lock, irq);

	/* Lets the drives interrupt. */
	outb(IDE_ALT_STATUS, 0);
}

/*
 * Consumes the interrupts taken since the last look, sleeping until one
 * comes or until the deadline (a tick count; 0 does not sleep).  Reports
 * whether there was one.
 */
static int
take_interrupts(
	uint64_t deadline)
{
	unsigned long irq;
	uint64_t observed;
	uint64_t now;
	int arrived;

	/* Sleeps while no interrupt is new and the deadline is ahead. */
	irq = spin_lock_irqsave(&interrupt_lock);
	while (interrupt_count == interrupt_seen && deadline != 0) {
		now = sched_ticks();
		if (now >= deadline)
			break;
		observed = waitq_sequence(&interrupt_wait);
		(void)waitq_sleep(&interrupt_wait, &interrupt_lock, observed, deadline, 0);
	}

	/* Takes every new interrupt at once. */
	arrived = 0;
	if (interrupt_count != interrupt_seen)
		arrived = 1;
	interrupt_seen = interrupt_count;
	spin_unlock_irqrestore(&interrupt_lock, irq);

	/* Succeeded: whether an interrupt came. */
	return arrived;
}

/*
 * Turns the interrupts off for good after a command finished without one,
 * and says so once: the board does not deliver the drive's INTRQ on IRQ 9.
 */
static void
stop_interrupts(
	void)
{
	/* The line and the drive stay quiet from now on. */
	interrupts_used = 0;
	kern_irq_mask(IDE_IRQ);
	outb(IDE_ALT_STATUS, IDE_DEVCTL_NIEN);

	/* Reports the change once; it is never undone. */
	kern_logf("ide: no interrupt on IRQ %d from bank %u; commands are polled\n",
		  IDE_IRQ, (unsigned)(inb(IDE_BANK_SELECT) & 1U));
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
		kern_logf("ide: %s %s LBA=%u count=%u bank=%u drive=%u "
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
