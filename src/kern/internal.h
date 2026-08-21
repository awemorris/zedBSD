/* Private runtime wiring shared while the Stage 2 services are split. */
#ifndef ZEDBSD_KERN_INTERNAL_H
#define ZEDBSD_KERN_INTERNAL_H

#include <kern/boot.h>
#include <kern/env.h>
#include <kern/fs.h>
#include <kern/namespace.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_PARTS 16
#define LINE_MAX 256
#define STARTUP_TIMEOUT_SECONDS 1
#define MAX_IDE_DEVICES 4
#define MAX_SCSI_TARGETS 7
#define MAX_FIXED_DEVICES (MAX_IDE_DEVICES + MAX_SCSI_TARGETS)

struct part {
	uint8_t valid, index, bootable;
	char name[17];
	uint32_t start, data, count;
};

enum startup_phase {
	STARTUP_DRAW, STARTUP_PROBE, STARTUP_TIMEOUT, STARTUP_SELECTED,
	STARTUP_SHELL,
};
enum startup_auto_kind {
	STARTUP_AUTO_NONE, STARTUP_AUTO_CONFIG, STARTUP_AUTO_PBR,
};
enum startup_config_kind {
	STARTUP_CONFIG_NONE, STARTUP_CONFIG_BOOTCFG,
};
struct startup_state {
	enum startup_phase phase;
	unsigned next_candidate, probe_total, probe_done, fixed_count;
	uint8_t ide_bitmap, scsi_bitmap;
	int auto_device, auto_partition, auto_priority;
	enum startup_auto_kind auto_kind;
	enum startup_config_kind auto_config_kind;
	int automatic_cancelled, timeout_start;
	unsigned timeout_budget;
};

extern const struct boot_handoff *kern_handoff;
extern const struct boot_device *kern_devices;
extern unsigned kern_device_count;
extern struct bootfs kern_mounted_fs;
extern struct bootfs_namespace kern_mounted_namespace;
extern struct environment kern_environment;
extern struct part kern_parts[MAX_PARTS];
extern int kern_current_device, kern_current_partition;

int kern_streq(const char *left, const char *right);
void kern_memzero(void *destination, uint32_t length);
void kern_memcopy(void *destination, const void *source, uint32_t length);
unsigned kern_slen(const char *text);
int kern_strcopy(char *destination, const char *source, unsigned capacity);
void kern_update_cursor(void);
void kern_putc(char character);
void kern_puts(const char *text);
void kern_dec(unsigned value);
void kern_hex8(uint8_t value);
void kern_devname(int device);
int kern_scanparts(int device);
int kern_mountpart(int device, int partition);
void kern_select_disk_home(int device);
void kern_register_scanned_disk(int device);
int kern_key(void);
int kern_poll(void);
int kern_line(char *buffer);
void kern_prompt(void);
int kern_noct_key_read(void *context);
int kern_noct_key_poll(void *context);
int kern_noct_key_is_down(void *context, int key);
void kern_noct_key_drain(void *context);
int kern_noct_clock_second(void *context);
int kern_clock_second(void);
uint8_t kern_ide_reported_drives(void);
uint8_t kern_scsi_reported_targets(void);
unsigned kern_bit_count(uint8_t value);
int kern_probe_fixed_device(uint8_t device_class, uint8_t bios_id);
void kern_probe_fixed_class(uint8_t device_class);
#ifdef ZEDBSD_M9_WRITE_TEST
int kern_m9_write_test(uint32_t lba);
#endif
int kern_command(char *line);

const char *startup_config_file(void);
int run_noct_user(const char *, int, char *const []);
extern int kern_noct_last_status;
int startup_menu(struct startup_state *state);

#define ho kern_handoff
#define devs kern_devices
#define device_count kern_device_count
#define mounted_fs kern_mounted_fs
#define mounted_namespace kern_mounted_namespace
#define boot_environment kern_environment
#define parts kern_parts
#define curdev kern_current_device
#define curpart kern_current_partition
#define streq kern_streq
#define memzero kern_memzero
#define memcopy kern_memcopy
#define slen kern_slen
#define strcopy kern_strcopy
#define update_cursor kern_update_cursor
#define putc kern_putc
#define puts kern_puts
#define dec kern_dec
#define hex8 kern_hex8
#define devname kern_devname
#define scanparts kern_scanparts
#define mountpart kern_mountpart
#define select_disk_home kern_select_disk_home
#define register_scanned_disk kern_register_scanned_disk
#define key kern_key
#define poll kern_poll
#define line kern_line
#define prompt kern_prompt
#define noct_key_read kern_noct_key_read
#define noct_key_poll kern_noct_key_poll
#define noct_key_is_down kern_noct_key_is_down
#define noct_key_drain kern_noct_key_drain
#define noct_clock_second kern_noct_clock_second
#define clock_second kern_clock_second
#define ide_reported_drives kern_ide_reported_drives
#define scsi_reported_targets kern_scsi_reported_targets
#define bit_count kern_bit_count
#define probe_fixed_device kern_probe_fixed_device
#define probe_fixed_class kern_probe_fixed_class
#define m9_write_test kern_m9_write_test
#define command kern_command

#endif
