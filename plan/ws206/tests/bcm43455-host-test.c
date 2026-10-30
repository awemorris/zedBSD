/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the BCM43455 driver (ws206-p004).
 *
 * The production files of src/drivers/wifi/bcm43455/ (all but
 * bcm43455-files.c, which reads the file system) are compiled unchanged
 * against a model of the chip behind the bus operations: function 0 and
 * function 1 registers, the backplane with its cores, wrappers,
 * enumeration ROM and RAM, the SDIO core's interrupt status and mailbox,
 * and a firmware that answers BCDC control requests and sends events and
 * data on function 2.  The network device and WLAN station calls are
 * stand-ins that record what the driver asked.
 *
 * Usage: bcm43455-host-test unit|smp [FILES-DIRECTORY]
 *
 * unit:  the codecs (SDPCM, BCDC, events, NVRAM, CLM, the firmware's
 *        version text), the chip's bring-up order, the settings every
 *        start makes, the probe's address checks, publication, open and
 *        close with the bus thread, data both ways, a bad and an
 *        oversized frame, a halted firmware, missing credit, and that
 *        every buffer a secret passed through is cleared afterwards.
 *        With FILES-DIRECTORY (the package's verified cache), the real
 *        firmware, CLM and NVRAM are checked and converted too.
 *        The radio messages (channel specifications, escan requests and
 *        results, the network description as a probe response, the join
 *        and key messages) and the table of connection errors.
 * smp:   requesters on four threads, a transmitter and the firmware's
 *        events race the bus thread, then the bus is stopped under them.
 * radio: the FullMAC radio operations against the model's firmware (ws206-
 *        p005): the scan's order of reports (ready, networks, end) also
 *        when results come before the escan's answer, stale results, the
 *        abort; the connection's commands, its events and errors, data
 *        both ways, link loss, disconnect, cancel, a connect that does not
 *        wait for a slow bus, the PMK erased on every path; a bus failure,
 *        its restart and the restarts' bound.
 * radio-smp: a connector, a scanner, a poller, a transmitter and the
 *        firmware race the bus thread (ThreadSanitizer).
 */

#include "drivers/wifi/bcm43455/bcm43455.h"
#include "drivers/wifi/bcm43455/bcm43455-bcdc.h"
#include "drivers/wifi/bcm43455/bcm43455-bus.h"
#include "drivers/wifi/bcm43455/bcm43455-bytes.h"
#include "drivers/wifi/bcm43455/bcm43455-chip.h"
#include "drivers/wifi/bcm43455/bcm43455-device.h"
#include "drivers/wifi/bcm43455/bcm43455-event.h"
#include "drivers/wifi/bcm43455/bcm43455-firmware.h"
#include "drivers/wifi/bcm43455/bcm43455-hw.h"
#include "drivers/wifi/bcm43455/bcm43455-radio.h"
#include "drivers/wifi/bcm43455/bcm43455-sdpcm.h"
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <kern/net/net-device.h>
#include <kern/net/packet-buf.h>
#include <kern/net/wlan.h>
#include <kern/thread.h>
#include <kern/waitq.h>
#include <uapi/errno.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Stops the test at the first expectation that does not hold. */
#define CHECK(expression)                                                     \
	do {                                                                  \
		if (!(expression)) {                                          \
			fprintf(stderr, "bcm43455-host-test: failed at %s:%d: %s\n", \
			    __FILE__, __LINE__, #expression);                   \
			abort();                                              \
		}                                                             \
	} while (0)

uint64_t sched_ticks(void);

/* The model's backplane map. */
#define CC_BASE			0x18000000U
#define D11_BASE		0x18001000U
#define SDIO_BASE		0x18002000U
#define CR4_BASE		0x18003000U
#define CC_WRAP			0x18100000U
#define D11_WRAP		0x18101000U
#define SDIO_WRAP		0x18102000U
#define CR4_WRAP		0x18103000U
#define PMU_BASE		0x18012000U
#define EROM_BASE		0x18109000U
#define RAM_BASE		0x198000U
#define RAM_SIZE		0xc8000U

/* The model's frame queue. */
#define QUEUE_MAX		64U
#define FRAME_MAX		6144U

/* The model chip's address and the board's placeholder. */
static const uint8_t chip_mac[6] = {0xdc, 0xa6, 0x32, 0x01, 0x02, 0x03};
static const char placeholder_text[] = "b8:27:eb:74:f2:6c";

/* One frame the firmware sends, as the host reads it from function 2. */
struct model_frame {
	size_t length;
	uint8_t bytes[FRAME_MAX];
};

/* One control request the firmware received, kept for the order checks. */
struct model_request {
	uint32_t command;
	unsigned set;
	char name[32];
	size_t length;
	uint8_t payload[2048];
};

/*
 * The chip.  Every field is changed under model_lock, which the bus
 * operations take; the smp scenario's threads take it too.
 */
static struct {
	int powered;
	unsigned power_ups;
	unsigned power_downs;
	uint8_t cccr_enable;
	uint8_t ien;
	uint8_t clock;
	unsigned fast_requests_before_boot;
	uint8_t window[3];
	uint8_t watermark;
	uint8_t sleep_control;
	uint8_t wakeup_control;
	uint8_t card_capability;
	uint8_t card_control;
	unsigned io_aborts;
	unsigned f2_disables;
	unsigned save_restore;
	uint32_t pmu_control;
	uint32_t chip_control_address;
	unsigned read_terminates;
	uint32_t erom[64];
	unsigned erom_words;
	uint32_t d11_io;
	uint32_t d11_reset;
	uint32_t d11_io_history[16];
	unsigned d11_io_count;
	uint32_t cr4_io;
	uint32_t cr4_reset;
	uint32_t cr4_bank;
	unsigned ram_written_running;
	uint8_t *ram;
	uint32_t vector;
	unsigned booted;
	unsigned boot_count;
	unsigned boot_image_ok;
	uint32_t int_status;
	uint32_t host_int_mask;
	uint32_t to_chip_mailbox;
	uint32_t to_chip_data;
	uint32_t to_host_data;
	unsigned naks;
	unsigned f2_enabled;
	uint8_t host_sequence;
	unsigned sequence_errors;
	uint8_t firmware_sequence;
	unsigned credit;
	struct model_frame queue[QUEUE_MAX];
	unsigned queue_head;
	unsigned queue_count;
	size_t read_position;
	struct model_request requests[64];
	unsigned request_count;
	uint8_t clm[4096];
	size_t clm_received;
	unsigned clm_messages;
	unsigned clm_flags_ok;
	uint8_t country[12];
	int refuse_country;
	int silent;
	int halt_on_down;
	uint8_t data_sent[FRAME_MAX];
	size_t data_sent_length;
	unsigned data_sent_count;
	uint8_t secret_seen[32];
	unsigned scan_mode;
	unsigned escan_starts;
	unsigned escan_aborts;
	uint16_t escan_sync;
	uint16_t escan_chanspec;
	uint8_t escan_scan_type;
	unsigned join_script;
	unsigned joins;
	unsigned leaves;
	uint8_t key_seen[BCM43455_KEY_MESSAGE_LENGTH];
	unsigned keys;
	uint8_t join_seen[BCM43455_JOIN_REQUEST_LENGTH];
	uint32_t refuse_command;
	uint32_t ignore_command;
	uint8_t held_answer[16 + 2048];
	size_t held_length;
	unsigned armed;
	unsigned pending;
	unsigned kicked;
	unsigned irq_enabled;
} chip;

/* The firmware image the model expects to boot. */
static uint8_t *firmware_image;

/* The lock and the condition the model and the interrupt wait share. */
static pthread_mutex_t model_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t model_wakeup = PTHREAD_COND_INITIALIZER;

/* When the test began, for the scheduler's clock. */
static struct timespec started;

/* What each thread holds, to check that every spinlock of the driver is a leaf. */
static __thread unsigned spin_depth;

/* One kernel lock or queue, as a host mutex and condition. */
struct host_object {
	const void *key;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
};

/* The kernel locks and queues, each a host mutex or condition, found by address. */
static struct host_object host_objects[256];
static unsigned host_object_count;
static pthread_mutex_t host_object_lock = PTHREAD_MUTEX_INITIALIZER;

/* The network and WLAN stand-ins' records. */
static struct net_device fake_net;
static unsigned net_allocs;
static unsigned net_creates;
static unsigned net_gones;
static unsigned net_destroys;
static unsigned polls_scheduled;
static unsigned attaches;
static unsigned opens;
static unsigned closes;
static unsigned quiesces;
static unsigned quiesce_ends;
static unsigned stop_requests;
static int stop_completed;
static unsigned packets_freed;
static int attach_answer;
static int fake_station_token;
static const struct wlan_radio_ops *attached_ops;
static struct wlan_scan_profile attached_profile;

/* The smp scenario's threads' stop flag. */
static int smp_done;

/* How the model's firmware answers an escan start. */
#define SCAN_NORMAL		0U
#define SCAN_RESULTS_FIRST	1U
#define SCAN_HOLD		2U
#define SCAN_REFUSE		3U
#define SCAN_BUSY		4U
#define SCAN_STALE		5U

/* What the model's firmware does after a join command. */
#define JOIN_NOTHING		0U
#define JOIN_SUCCESS		1U
#define JOIN_WRONG_KEY		2U
#define JOIN_NO_NETWORK		3U
#define JOIN_M1_TIMEOUT		4U
#define JOIN_M3_TIMEOUT		5U

/* Milliseconds the model's bus takes for each frame the host writes, to slow the bus thread. */
static unsigned model_delay_ms;

/* What the WLAN stand-ins were told, in order. */
#define REPORT_READY		1U
#define REPORT_FRAME		2U
#define REPORT_COMPLETE		3U
#define REPORT_SCAN_ERROR	4U
#define REPORT_EVENT		5U
#define REPORT_LOSS		6U
#define REPORT_ETHERNET		7U
#define REPORT_RESTART		8U

/* One report the driver made to the WLAN core. */
struct report {
	unsigned kind;
	uint64_t generation;
	uint32_t value;
	int error;
	char ssid[33];
	uint32_t channel;
	uint32_t security;
	int32_t rssi;
	size_t length;
};

/* The reports, the lock the poll and the checks share, and the port the station keeps open. */
#define REPORT_MAX		512U
static struct report reports[REPORT_MAX];
static unsigned report_count;
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t open_port_generation;

static void run_unit(const char *files);
static void run_smp(void);
static void run_radio(void);
static void run_radio_smp(void);
static void check_radio_codecs(void);
static void check_connect_table(void);
static void check_sdpcm(void);
static void check_bcdc(void);
static void check_events(void);
static void check_nvram(void);
static void check_firmware_files(void);
static void check_real_files(const char *directory);
static void check_profile(void);
static void check_chip(void);
static void check_probe(void);
static void check_refusals(void);
static void check_runtime(void);
static void check_failures(void);
static void make_files(struct bcm43455_files *files, const char *mac);
static struct bcm43455_device *make_device(const char *mac);
static void model_reset(void);
static void model_build_erom(void);
static void model_push_locked(unsigned channel, const uint8_t *payload, size_t length);
static void model_push_event(uint32_t type, uint32_t status, uint32_t reason);
static void model_push_data(const uint8_t *frame, size_t length);
static void model_push_event_locked(uint32_t type, uint32_t status, uint32_t reason, uint16_t flags, const uint8_t *data, size_t data_length);
static void model_push_event_with(uint32_t type, uint32_t status, uint32_t reason, uint16_t flags);
static size_t model_bss(uint8_t *bss, const char *ssid, uint8_t last, uint16_t chanspec);
static void model_scan_events_locked(uint16_t sync_id, uint16_t chanspec);
static void model_join_events_locked(void);
static void record_report(unsigned kind, uint64_t generation, uint32_t value, int error, const struct wlan_bss_record *bss, size_t length);
static void model_raise_locked(uint32_t bits);
static void model_frame_from_host(const uint8_t *bytes, size_t length);
static void model_control(const uint8_t *message, size_t length);
static void model_boot_locked(void);
static uint32_t model_read32_locked(uint32_t address);
static void model_write32_locked(uint32_t address, uint32_t value);
static int all_zero(const void *bytes, size_t length);
static unsigned count_requests(const char *name);
static const struct model_request *find_request(const char *name);
static struct host_object *find_object(const void *key);
static void sleep_ms(unsigned milliseconds);
static unsigned char *read_whole(const char *path, size_t *size);

/* The bus operations the model answers. */
static int op_read_byte(void *context, unsigned function, uint32_t address, uint8_t *value);
static int op_write_byte(void *context, unsigned function, uint32_t address, uint8_t value);
static int op_read(void *context, unsigned function, uint32_t address, int increment, uint8_t *buffer, size_t length);
static int op_write(void *context, unsigned function, uint32_t address, int increment, uint8_t *buffer, size_t length);
static unsigned op_block_size(void *context, unsigned function);
static int op_enable_function(void *context, unsigned function, unsigned timeout_ms);
static int op_disable_function(void *context, unsigned function);
static int op_sleep(void *context, unsigned milliseconds);
static int op_interrupt_wait(void *context, uint64_t deadline);
static void op_interrupt_enable(void *context, int enable);
static void op_interrupt_rearm(void *context);
static void op_interrupt_kick(void *context);
static int op_power_up(void *context);
static void op_power_down(void *context);

/* The model's bus. */
static const struct bcm43455_bus_ops model_ops = {
	op_read_byte,
	op_write_byte,
	op_read,
	op_write,
	op_block_size,
	op_enable_function,
	op_disable_function,
	op_sleep,
	op_interrupt_wait,
	op_interrupt_enable,
	op_interrupt_rearm,
	op_interrupt_kick,
	op_power_up,
	op_power_down
};

/*
 * Runs one scenario.
 */
int
main(
	int argc,
	char **argv)
{
	/* Requires the scenario. */
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: bcm43455-host-test unit|smp|radio|radio-smp [FILES-DIRECTORY]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &started);

	/* The model's RAM and the firmware image it expects. */
	chip.ram = calloc(1, RAM_SIZE);
	firmware_image = malloc(BCM43455_FIRMWARE_SIZE);
	CHECK(chip.ram != NULL && firmware_image != NULL);

	if (strcmp(argv[1], "unit") == 0) {
		run_unit(argc == 3 ? argv[2] : NULL);
	} else if (strcmp(argv[1], "smp") == 0) {
		run_smp();
	} else if (strcmp(argv[1], "radio") == 0) {
		run_radio();
	} else if (strcmp(argv[1], "radio-smp") == 0) {
		run_radio_smp();
	} else {
		fprintf(stderr, "bcm43455-host-test: unknown scenario %s\n", argv[1]);
		return 2;
	}

	printf("bcm43455-host-test %s: PASS\n", argv[1]);
	return 0;
}

/* The single-threaded checks, then the runtime with its bus thread. */
static void
run_unit(
	const char *files)
{
	check_sdpcm();
	check_bcdc();
	check_events();
	check_nvram();
	check_firmware_files();
	if (files != NULL)
		check_real_files(files);
	check_profile();
	check_radio_codecs();
	check_connect_table();
	check_chip();
	check_probe();
	check_refusals();
	check_runtime();
	check_failures();
}

/* SDPCM headers, the credit window and the padding. */
static void
check_sdpcm(void)
{
	struct bcm43455_sdpcm_fields header;
	uint8_t frame[64];

	/* A frame written is read back with its fields. */
	memset(frame, 0, sizeof(frame));
	bcm43455_sdpcm_encode(frame, 40, 7, BCM43455_SDPCM_CHANNEL_DATA);
	frame[9] = 12;
	frame[8] = 0x5a;
	CHECK(bcm43455_get_le16(frame) == 40 && bcm43455_get_le16(frame + 2) == (uint16_t)~40U);
	CHECK(bcm43455_sdpcm_decode(frame, sizeof(frame), &header) == 0);
	CHECK(header.length == 40 && header.sequence == 7 && header.channel == 2);
	CHECK(header.payload_start == 12 && header.max_sequence == 12 && header.flow_control == 0x5a);

	/* An all-zero tag is no frame; a wrong complement or a short length is refused. */
	memset(frame, 0, sizeof(frame));
	CHECK(bcm43455_sdpcm_decode(frame, sizeof(frame), &header) == ENOENT);
	bcm43455_sdpcm_encode(frame, 40, 0, 0);
	frame[2] ^= 1;
	CHECK(bcm43455_sdpcm_decode(frame, sizeof(frame), &header) == EIO);
	bcm43455_sdpcm_encode(frame, 8, 0, 0);
	CHECK(bcm43455_sdpcm_decode(frame, sizeof(frame), &header) == EPROTO);
	bcm43455_sdpcm_encode(frame, 40, 0, 0);
	frame[7] = 41;
	CHECK(bcm43455_sdpcm_decode(frame, sizeof(frame), &header) == EPROTO);
	frame[7] = 11;
	CHECK(bcm43455_sdpcm_decode(frame, sizeof(frame), &header) == EPROTO);
	bcm43455_sdpcm_encode(frame, 12, 3, 0);
	frame[7] = 0;
	frame[9] = 20;
	CHECK(bcm43455_sdpcm_decode(frame, sizeof(frame), &header) == 0);
	CHECK(header.length == 12 && header.max_sequence == 20);
	CHECK(bcm43455_sdpcm_decode(frame, 11, &header) == EINVAL);

	/* The window wraps modulo 256 and a backwards or garbled grant is none. */
	CHECK(bcm43455_sdpcm_window(12, 7) == 5);
	CHECK(bcm43455_sdpcm_window(2, 250) == 8);
	CHECK(bcm43455_sdpcm_window(7, 7) == 0);
	CHECK(bcm43455_sdpcm_window(6, 7) == 0);
	CHECK(bcm43455_sdpcm_window(0x87, 7) == 0);
	CHECK(bcm43455_sdpcm_window(0x86, 7) == 0x7f);

	/* Up to a block is padded to four bytes, beyond it to whole blocks. */
	CHECK(bcm43455_sdpcm_padded_length(13, 512) == 16);
	CHECK(bcm43455_sdpcm_padded_length(512, 512) == 512);
	CHECK(bcm43455_sdpcm_padded_length(513, 512) == 1024);
	CHECK(bcm43455_sdpcm_padded_length(1536, 512) == 1536);
}

/* BCDC control and data headers and iovar payloads. */
static void
check_bcdc(void)
{
	struct bcm43455_bcdc_answer answer;
	struct bcm43455_bcdc_data data;
	uint8_t message[64];
	size_t length;

	/* A control header carries the command, the length, the set flag and the id. */
	memset(message, 0, sizeof(message));
	bcm43455_bcdc_encode_control(message, 263, 20, 1, 0x1234);
	CHECK(bcm43455_get_le32(message) == 263 && bcm43455_get_le32(message + 4) == 20);
	CHECK(bcm43455_get_le32(message + 8) == 0x12340002U);
	CHECK(bcm43455_bcdc_decode_control(message, 36, &answer) == 0);
	CHECK(answer.command == 263 && answer.id == 0x1234 && answer.set == 1 && answer.failed == 0);
	CHECK(answer.payload_offset == 16 && answer.payload_length == 20);

	/* The payload never runs past what was received, and the error flag and status come through. */
	bcm43455_put_le32(message + 8, 0x00070001U);
	bcm43455_put_le32(message + 12, (uint32_t)-23);
	CHECK(bcm43455_bcdc_decode_control(message, 24, &answer) == 0);
	CHECK(answer.failed == 1 && answer.status == -23 && answer.id == 7 && answer.payload_length == 8);
	CHECK(bcm43455_bcdc_decode_control(message, 15, &answer) == EPROTO);

	/* An iovar is its name, a NUL and its value; one that does not fit is refused. */
	CHECK(bcm43455_bcdc_iovar(message, sizeof(message), "ver", NULL, 0, &length) == 0);
	CHECK(length == 4 && memcmp(message, "ver", 4) == 0);
	CHECK(bcm43455_bcdc_iovar(message, sizeof(message), "roam_off", "\1\0\0\0", 4, &length) == 0);
	CHECK(length == 13 && message[8] == 0 && message[9] == 1);
	CHECK(bcm43455_bcdc_iovar(message, 12, "roam_off", "\1\0\0\0", 4, &length) == ENOSPC);
	CHECK(bcm43455_bcdc_iovar(message, sizeof(message), "", NULL, 0, &length) == EINVAL);

	/* A data header of version 2 says where the frame starts; another version is refused. */
	bcm43455_bcdc_encode_data(message, 5, 0);
	CHECK(message[0] == 0x20 && message[1] == 5 && message[3] == 0);
	message[3] = 2;
	CHECK(bcm43455_bcdc_decode_data(message, 30, &data) == 0);
	CHECK(data.frame_offset == 12 && data.frame_length == 18 && data.priority == 5);
	CHECK(bcm43455_bcdc_decode_data(message, 11, &data) == EPROTO);
	message[0] = 0x10;
	CHECK(bcm43455_bcdc_decode_data(message, 30, &data) == EPROTO);
}

/* Builds an event frame: Ethernet, vendor header, big-endian message, data. */
static size_t
build_event(
	uint8_t *frame,
	uint32_t type,
	uint32_t status,
	uint32_t reason,
	size_t data_length)
{
	memset(frame, 0, 72 + data_length);
	memset(frame, 0xff, 6);
	memcpy(frame + 6, chip_mac, 6);
	bcm43455_put_be16(frame + 12, 0x886c);
	bcm43455_put_be16(frame + 14, 0x8001);
	bcm43455_put_be16(frame + 16, (uint16_t)(58 + data_length));
	frame[18] = 0;
	frame[19] = 0x00;
	frame[20] = 0x10;
	frame[21] = 0x18;
	bcm43455_put_be16(frame + 22, 1);
	bcm43455_put_be16(frame + 24, 2);
	bcm43455_put_be32(frame + 28, type);
	bcm43455_put_be32(frame + 32, status);
	bcm43455_put_be32(frame + 36, reason);
	bcm43455_put_be32(frame + 44, (uint32_t)data_length);
	memcpy(frame + 48, chip_mac, 6);
	memcpy(frame + 54, "wlan0", 5);
	return 72 + data_length;
}

/* Event frames and the event mask. */
static void
check_events(void)
{
	struct bcm43455_event event;
	uint8_t frame[128];
	uint8_t mask[BCM43455_EVENT_MASK_LENGTH];
	size_t length;

	/* An event decodes with its big-endian fields and its data's place. */
	length = build_event(frame, BCM43455_EVENT_LINK, 0, 1, 6);
	CHECK(bcm43455_event_decode(frame, length, &event) == 0);
	CHECK(event.type == 16 && event.status == 0 && event.reason == 1 && event.version == 2);
	CHECK(event.vendor_subtype == 0x8001);
	CHECK(event.data_start == 72 && event.data_length == 6);
	CHECK(memcmp(event.address, chip_mac, 6) == 0);

	/* Another ethertype is not an event; a wrong vendor or an overlong data length is refused. */
	bcm43455_put_be16(frame + 12, 0x0800);
	CHECK(bcm43455_event_decode(frame, length, &event) == ENOENT);
	length = build_event(frame, 69, 8, 0, 0);
	frame[21] = 0x19;
	CHECK(bcm43455_event_decode(frame, length, &event) == EPROTO);
	length = build_event(frame, 69, 8, 0, 4);
	CHECK(bcm43455_event_decode(frame, length - 1U, &event) == EPROTO);
	CHECK(bcm43455_event_decode(frame, 40, &event) == EPROTO);

	/* Event n is bit n % 8 of byte n / 8. */
	memset(mask, 0, sizeof(mask));
	CHECK(bcm43455_event_mask_add(mask, 0) == 0);
	CHECK(bcm43455_event_mask_add(mask, 46) == 0);
	CHECK(bcm43455_event_mask_add(mask, 69) == 0);
	CHECK(mask[0] == 1 && mask[5] == 0x40 && mask[8] == 0x20);
	CHECK(bcm43455_event_mask_add(mask, 143) == 0 && mask[17] == 0x80);
	CHECK(bcm43455_event_mask_add(mask, 144) == EINVAL);
}

/* NVRAM conversion, its address and its place in RAM; CLM messages. */
static void
check_nvram(void)
{
	static const char text[] =
	    "# Raspberry Pi 4 test settings\n"
	    "\n"
	    "  boardtype=0x6e4  \r\n"
	    "#macaddr=00:90:4c:c5:12:38\n"
	    "macaddr=b8:27:eb:74:f2:6c\n"
	    "not a setting\n"
	    "\tbtc_mode=1\n"
	    "=empty-key\n"
	    "ccode=";
	static const char expected[] = "boardtype=0x6e4\0macaddr=b8:27:eb:74:f2:6c\0btc_mode=1\0ccode=\0";
	uint8_t blob[256];
	uint8_t clm[3000];
	uint8_t message[BCM43455_CLM_HEADER_LENGTH + BCM43455_CLM_CHUNK_MAX];
	uint8_t mac[6];
	size_t length;
	size_t message_length;
	size_t taken;
	uint32_t address;
	uint32_t token;
	uint32_t words;

	/* Settings are kept, each with its NUL; comments, blanks and junk are dropped; a NUL and zeros pad it. */
	memset(blob, 0xaa, sizeof(blob));
	CHECK(bcm43455_nvram_pack((const uint8_t *)text, sizeof(text) - 1U, blob, sizeof(blob), &length) == 0);
	CHECK(memcmp(blob, expected, sizeof(expected)) == 0);
	CHECK((length % 4U) == 0);
	words = (uint32_t)((length - 4U) / 4U);
	token = bcm43455_get_le32(blob + length - 4U);
	CHECK((token & 0xffffU) == words && (token >> 16) == ((~words) & 0xffffU));
	CHECK(all_zero(blob + sizeof(expected) - 1U, length - 4U - (sizeof(expected) - 1U)));

	/* A list that does not fit, or holds no setting, is refused. */
	CHECK(bcm43455_nvram_pack((const uint8_t *)text, sizeof(text) - 1U, blob, 40, &length) == ENOSPC);
	CHECK(bcm43455_nvram_pack((const uint8_t *)"# none\n\n", 8, blob, sizeof(blob), &length) == EINVAL);

	/* The placeholder address is the macaddr line, not the commented one. */
	CHECK(bcm43455_nvram_mac((const uint8_t *)text, sizeof(text) - 1U, mac) == 0);
	CHECK(mac[0] == 0xb8 && mac[1] == 0x27 && mac[5] == 0x6c);
	CHECK(bcm43455_nvram_mac((const uint8_t *)"a=1\n", 4, mac) == ENOENT);
	CHECK(bcm43455_nvram_mac((const uint8_t *)"macaddr=b8:27:eb:74:f2\n", 23, mac) == EINVAL);
	CHECK(bcm43455_nvram_mac((const uint8_t *)"macaddr=b8:27:eb:74:f2:6g\n", 26, mac) == EINVAL);

	/* The blob ends at the top of RAM: its length word is RAM's last four bytes. */
	CHECK(bcm43455_nvram_place(RAM_BASE, RAM_SIZE, 1000, length, &address) == 0);
	CHECK(address + length == RAM_BASE + RAM_SIZE);
	CHECK(bcm43455_nvram_place(RAM_BASE, RAM_SIZE, RAM_SIZE - 8U, 12, &address) == EINVAL);
	CHECK(bcm43455_nvram_place(RAM_BASE, RAM_SIZE, 1000, 10, &address) == EINVAL);

	/* The CLM goes in pieces of 1400 bytes, the first marked as the beginning and the last as the end. */
	memset(clm, 0x5c, sizeof(clm));
	CHECK(bcm43455_clm_message(message, sizeof(message), clm, sizeof(clm), 0, &message_length, &taken) == 0);
	CHECK(taken == 1400 && message_length == 1412);
	CHECK(bcm43455_get_le16(message) == 0x1002 && bcm43455_get_le16(message + 2) == 2);
	CHECK(bcm43455_get_le32(message + 4) == 1400 && bcm43455_get_le32(message + 8) == 0);
	CHECK(bcm43455_clm_message(message, sizeof(message), clm, sizeof(clm), 1400, &message_length, &taken) == 0);
	CHECK(taken == 1400 && bcm43455_get_le16(message) == 0x1000);
	CHECK(bcm43455_clm_message(message, sizeof(message), clm, sizeof(clm), 2800, &message_length, &taken) == 0);
	CHECK(taken == 200 && bcm43455_get_le16(message) == 0x1004 && message_length == 212);
	CHECK(bcm43455_clm_message(message, sizeof(message), clm, 100, 0, &message_length, &taken) == 0);
	CHECK(bcm43455_get_le16(message) == 0x1006);
	CHECK(bcm43455_clm_message(message, sizeof(message), clm, sizeof(clm), 3000, &message_length, &taken) == EINVAL);
	CHECK(bcm43455_clm_message(message, 100, clm, sizeof(clm), 0, &message_length, &taken) == ENOSPC);
}

/* The size and form checks of the three files, and the firmware's features. */
static void
check_firmware_files(void)
{
	static const char version[] = "43455c0-roml/43455_sdio-pno-aoe-wfds-mfp-idsup-idauth-clm_min Version: 7.45.241 (x CY)";
	uint8_t *image;
	uint8_t clm[BCM43455_CLM_SIZE];
	uint8_t nvram[BCM43455_NVRAM_SIZE];
	char text[BCM43455_VERSION_TEXT_MAX];
	char short_text[20];

	/* The firmware's version text is found anywhere in an image of the package's size. */
	image = calloc(1, BCM43455_FIRMWARE_SIZE);
	CHECK(image != NULL);
	memcpy(image + 500000, version, sizeof(version));
	CHECK(bcm43455_firmware_check(image, BCM43455_FIRMWARE_SIZE, text, sizeof(text)) == 0);
	CHECK(strcmp(text, version) == 0);
	CHECK(bcm43455_firmware_check(image, BCM43455_FIRMWARE_SIZE, short_text, sizeof(short_text)) == 0);
	CHECK(strlen(short_text) == 19 && memcmp(short_text, version, 19) == 0);
	CHECK(bcm43455_firmware_check(image, BCM43455_FIRMWARE_SIZE - 4U, text, sizeof(text)) == EINVAL);
	image[500000] = 'x';
	CHECK(bcm43455_firmware_check(image, BCM43455_FIRMWARE_SIZE, text, sizeof(text)) == EINVAL);
	free(image);

	/* The build's features and the running capability list are read word by word. */
	CHECK(bcm43455_device_has_feature(version, "idsup") == 1);
	CHECK(bcm43455_device_has_feature(version, "clm_min") == 1);
	CHECK(bcm43455_device_has_feature(version, "idsu") == 0);
	CHECK(bcm43455_device_has_feature(version, "Version:") == 0);
	CHECK(bcm43455_device_has_feature("no slash", "idsup") == 0);
	CHECK(bcm43455_device_has_capability("ap sta wme idsup idauth\n", "idsup") == 1);
	CHECK(bcm43455_device_has_capability("ap sta wme idsupx idauth", "idsup") == 0);
	CHECK(bcm43455_device_has_capability("", "idsup") == 0);

	/* The CLM starts with BLOB and the NVRAM is text, each of the package's size. */
	memset(clm, 0, sizeof(clm));
	memcpy(clm, "BLOB", 4);
	CHECK(bcm43455_clm_check(clm, sizeof(clm)) == 0);
	CHECK(bcm43455_clm_check(clm, sizeof(clm) - 1U) == EINVAL);
	clm[0] = 'b';
	CHECK(bcm43455_clm_check(clm, sizeof(clm)) == EINVAL);
	memset(nvram, 'a', sizeof(nvram));
	nvram[10] = '\n';
	nvram[11] = '\r';
	nvram[12] = '\t';
	CHECK(bcm43455_nvram_check(nvram, sizeof(nvram)) == 0);
	nvram[13] = 0;
	CHECK(bcm43455_nvram_check(nvram, sizeof(nvram)) == EINVAL);
}

/* The package's real files, from its verified cache. */
static void
check_real_files(
	const char *directory)
{
	char path[1024];
	char text[BCM43455_VERSION_TEXT_MAX];
	uint8_t blob[BCM43455_NVRAM_BLOB_MAX];
	uint8_t mac[6];
	unsigned char *firmware;
	unsigned char *clm;
	unsigned char *nvram;
	size_t firmware_size;
	size_t clm_size;
	size_t nvram_size;
	size_t length;

	/* Reads the three files. */
	snprintf(path, sizeof(path), "%s/cyfmac43455-sdio-minimal.bin", directory);
	firmware = read_whole(path, &firmware_size);
	snprintf(path, sizeof(path), "%s/cyfmac43455-sdio.clm_blob", directory);
	clm = read_whole(path, &clm_size);
	snprintf(path, sizeof(path), "%s/brcmfmac43455-sdio.txt", directory);
	nvram = read_whole(path, &nvram_size);

	/* Each passes the driver's checks; the firmware names the supplicant. */
	CHECK(bcm43455_firmware_check(firmware, firmware_size, text, sizeof(text)) == 0);
	printf("  real firmware: %s\n", text);
	CHECK(bcm43455_device_has_feature(text, "idsup") == 1);
	CHECK(bcm43455_clm_check(clm, clm_size) == 0);
	CHECK(bcm43455_nvram_check(nvram, nvram_size) == 0);

	/* The NVRAM converts to whole words with a length word, and has no country of its own (E6). */
	CHECK(bcm43455_nvram_pack(nvram, nvram_size, blob, sizeof(blob), &length) == 0);
	CHECK((length % 4U) == 0 && length < nvram_size);
	CHECK(memmem(blob, length, "ccode=", 6) == NULL);
	CHECK(memmem(blob, length, "boardflags3=0x44200100", 23) != NULL);
	printf("  real NVRAM: %zu bytes of text, %zu in the chip's form\n", nvram_size, length);

	/* Its placeholder address is the Raspberry Pi one. */
	CHECK(bcm43455_nvram_mac(nvram, nvram_size, mac) == 0);
	CHECK(mac[0] == 0xb8 && mac[1] == 0x27 && mac[2] == 0xeb);

	free(firmware);
	free(clm);
	free(nvram);
}

/* The scan profile from the firmware's channel list. */
static void
check_profile(void)
{
	static const uint32_t channels[] = {1, 6, 6, 11, 13, 14, 15, 36, 40, 52, 100, 144, 145, 149, 165, 169, 200};
	struct wlan_scan_profile profile;
	unsigned i;

	/* Known channels are kept once each, all scanned by the firmware without host probes. */
	CHECK(bcm43455_device_scan_profile(channels, sizeof(channels) / sizeof(channels[0]), &profile) == 0);
	CHECK(profile.channel_count == 13);
	CHECK(profile.channels[0].channel == 1 && profile.channels[0].center_frequency_mhz == 2412);
	CHECK(profile.channels[4].channel == 14 && profile.channels[4].center_frequency_mhz == 2484);
	CHECK(profile.channels[5].channel == 36 && profile.channels[5].center_frequency_mhz == 5180);
	CHECK(profile.channels[12].channel == 169 && profile.channels[12].center_frequency_mhz == 5845);
	for (i = 0; i < profile.channel_count; i++)
		CHECK(profile.channels[i].flags == WLAN_SCAN_CHANNEL_OFFLOADED_DWELL && profile.channels[i].reserved == 0);
	CHECK(all_zero(&profile.channels[profile.channel_count], sizeof(profile.channels[0]) * (WLAN_SCAN_CHANNEL_MAX - profile.channel_count)));

	/* A list without one known channel is refused. */
	CHECK(bcm43455_device_scan_profile(channels + 16, 1, &profile) == EINVAL);
}

/* The chip's bring-up: identity, cores, RAM, download and start, in order. */
static void
check_chip(void)
{
	struct bcm43455_chip *chip_state;
	uint8_t nvram[64];
	unsigned i;

	chip_state = calloc(1, sizeof(*chip_state));
	CHECK(chip_state != NULL);

	/* A powered chip is identified and its cores are found in the enumeration ROM. */
	model_reset();
	CHECK(op_power_up(NULL) == 0);
	CHECK(bcm43455_chip_identify(chip_state, &model_ops, NULL) == 0);
	CHECK(chip_state->id == 0x4345 && chip_state->revision == 6);
	CHECK(chip_state->chipcommon.base == CC_BASE && chip_state->chipcommon.wrapper == CC_WRAP);
	CHECK(chip_state->chipcommon.revision == 43);
	CHECK(chip_state->sdio.base == SDIO_BASE && chip_state->sdio.wrapper == SDIO_WRAP);
	CHECK(chip_state->cr4.base == CR4_BASE && chip_state->cr4.wrapper == CR4_WRAP);
	CHECK(chip_state->d11.base == D11_BASE && chip_state->d11.wrapper == D11_WRAP);
	CHECK(chip_state->pmu.base == PMU_BASE && chip_state->pmu_base == PMU_BASE);
	CHECK(chip_state->ram_base == RAM_BASE && chip_state->ram_size == RAM_SIZE);

	/* The ARM core is halted and the radio held in reset with its PHY reset first. */
	CHECK((chip.cr4_io & BCM43455_CR4_IO_CPU_HALT) != 0 && chip.cr4_reset == 0);
	CHECK(chip.d11_io_count == 2);
	CHECK(chip.d11_io_history[0] == (BCM43455_D11_IO_PHY_RESET | BCM43455_D11_IO_PHY_CLOCK | 3U));
	CHECK(chip.d11_io == (BCM43455_D11_IO_PHY_CLOCK | 3U) && chip.d11_reset == 1);

	/* The PMU says save-restore; SDIO stays on, a card reset resets WLAN, the PMU reloads, F2 is off, the clock is the chip's. */
	CHECK(chip_state->pmu_revision == 24 && chip_state->save_restore == 1);
	CHECK((chip.sleep_control & BCM43455_SLEEP_KEEP_SDIO_ON) != 0);
	CHECK((chip.card_control & BCM43455_CARD_CONTROL_WLAN_RESET) != 0);
	CHECK((chip.pmu_control & BCM43455_PMU_CONTROL_RESOURCE_RELOAD) != 0);
	CHECK(chip.f2_disables == 1 && chip.clock == 0);

	/* The firmware and the NVRAM go to the bottom and the top of RAM while the ARM core is halted. */
	for (i = 0; i < BCM43455_FIRMWARE_SIZE; i++)
		firmware_image[i] = (uint8_t)(i * 7U + 3U);
	for (i = 0; i < sizeof(nvram); i++)
		nvram[i] = (uint8_t)(0x40U + i);
	CHECK(bcm43455_chip_download(chip_state, firmware_image, BCM43455_FIRMWARE_SIZE, nvram, sizeof(nvram)) == 0);
	CHECK(chip.ram_written_running == 0);
	CHECK(memcmp(chip.ram, firmware_image, BCM43455_FIRMWARE_SIZE) == 0);
	CHECK(memcmp(chip.ram + RAM_SIZE - sizeof(nvram), nvram, sizeof(nvram)) == 0);
	CHECK(chip.vector == bcm43455_get_le32(firmware_image));
	CHECK(chip.booted == 1 && chip.boot_count == 1 && chip.boot_image_ok == 1);
	CHECK(all_zero(chip_state->bounce, sizeof(chip_state->bounce)));
	CHECK(chip.clock == 0);

	/* The download ran on the slow clock: the fast one was never asked for before the firmware ran (BUG-311). */
	CHECK(chip.fast_requests_before_boot == 0);

	/* With save-restore the frame function comes up with the HT wait, CMD14 and only the force bit. */
	CHECK(bcm43455_chip_start(chip_state) == 0);
	CHECK(chip.to_chip_data == (4U << 16));
	CHECK(chip.f2_enabled == 1);
	CHECK(chip.host_int_mask == (BCM43455_INT_MAILBOX_MASK | BCM43455_INT_CHIP_ACTIVE));
	CHECK(chip.watermark == 8);
	CHECK((chip.wakeup_control & BCM43455_WAKEUP_HT_WAIT) != 0);
	CHECK(chip.card_capability == (BCM43455_CARD_CAPABILITY_CMD14 | BCM43455_CARD_CAPABILITY_CMD14_EXT));
	CHECK(chip.clock == BCM43455_CLOCK_FORCE_HT);
	CHECK(chip.ien == 0x07);
	CHECK(chip.window[2] == 0x18 && chip.window[1] == 0x00 && chip.window[0] == 0x00);

	/* Without save-restore the host's request is put back without the force. */
	model_reset();
	chip.save_restore = 0;
	CHECK(op_power_up(NULL) == 0);
	CHECK(bcm43455_chip_identify(chip_state, &model_ops, NULL) == 0);
	CHECK(chip_state->save_restore == 0);
	CHECK(bcm43455_chip_download(chip_state, firmware_image, BCM43455_FIRMWARE_SIZE, nvram, sizeof(nvram)) == 0);
	CHECK(bcm43455_chip_start(chip_state) == 0);
	CHECK((chip.clock & 0x3fU) == BCM43455_CLOCK_HT_REQUEST);
	CHECK(chip.wakeup_control == 0 && chip.card_capability == 0);

	/* A ROM without the ARM core's registers is refused. */
	model_reset();
	CHECK(op_power_up(NULL) == 0);
	chip.erom[chip.erom_words - 3U] = 0x0000000fU;
	CHECK(bcm43455_chip_identify(chip_state, &model_ops, NULL) == EIO);
	free(chip_state);
}

/* The probe: every setting of a start, in order, and the identity. */
static void
check_probe(void)
{
	struct bcm43455_device *device;
	const struct model_request *request;
	uint32_t count;
	unsigned i;

	/* A probe powers the chip up, sets it up, reads its identity and powers it down. */
	model_reset();
	device = make_device(placeholder_text);
	CHECK(bcm43455_device_probe(device) == 0);
	CHECK(chip.power_ups == 1 && chip.power_downs == 1 && chip.powered == 0);
	CHECK(memcmp(device->identity.mac, chip_mac, 6) == 0);
	CHECK(device->identity.supplicant == 1);
	CHECK(strstr(device->identity.version, "7.45.241") != NULL);
	CHECK(device->identity.profile.channel_count == 13);

	/* The order: glomming off, the CLM, the country, the identity, events, roaming, power saving, channels, then down. */
	CHECK(chip.request_count == 16);
	CHECK(strcmp(chip.requests[0].name, "bus:rxglom") == 0 && chip.requests[0].set == 1);
	CHECK(strcmp(chip.requests[1].name, "clmload") == 0 && strcmp(chip.requests[2].name, "clmload") == 0);
	CHECK(strcmp(chip.requests[3].name, "clmload_status") == 0 && chip.requests[3].set == 0);
	CHECK(strcmp(chip.requests[4].name, "country") == 0 && chip.requests[4].set == 1);
	CHECK(strcmp(chip.requests[5].name, "country") == 0 && chip.requests[5].set == 0);
	CHECK(strcmp(chip.requests[6].name, "cur_etheraddr") == 0);
	CHECK(strcmp(chip.requests[7].name, "ver") == 0);
	CHECK(strcmp(chip.requests[8].name, "cap") == 0);
	CHECK(strcmp(chip.requests[9].name, "event_msgs") == 0);
	CHECK(strcmp(chip.requests[10].name, "roam_off") == 0);
	CHECK(chip.requests[11].command == BCM43455_COMMAND_POWER_SAVE && bcm43455_get_le32(chip.requests[11].payload) == 0);
	CHECK(chip.requests[12].command == BCM43455_COMMAND_CHANNEL_LIST);
	count = bcm43455_get_le32(chip.requests[12].payload);
	CHECK(count == BCM43455_CHANNELS_MAX);
	CHECK(chip.requests[13].command == BCM43455_COMMAND_IO_VERSION && chip.requests[13].set == 0);
	CHECK(strcmp(chip.requests[14].name, "scan_ver") == 0 && chip.requests[14].set == 0);
	CHECK(chip.requests[15].command == BCM43455_COMMAND_RADIO_DOWN);
	CHECK(device->identity.io_type == BCM43455_CHANSPEC_LAYOUT_AC && device->identity.scan_layout == BCM43455_SCAN_LAYOUT_V0);

	/* The CLM arrived whole, in two messages marked begin and end. */
	CHECK(chip.clm_received == BCM43455_CLM_SIZE && chip.clm_messages == 2 && chip.clm_flags_ok == 1);
	CHECK(memcmp(chip.clm, device->files.clm, BCM43455_CLM_SIZE) == 0);

	/* The country is XX, revision 0, the world-wide locale. */
	request = find_request("country");
	CHECK(request != NULL);
	CHECK(memcmp(request->payload, "XX\0\0\0\0\0\0XX\0\0", 12) == 0);

	/* The event mask, 18 bytes, names the nine events the driver uses, and only them. */
	request = find_request("event_msgs");
	CHECK(request != NULL && request->length == 18);
	CHECK(request->payload[0] == 0xe9 && request->payload[1] == 0x10 && request->payload[2] == 0x01);
	CHECK(request->payload[5] == 0x40 && request->payload[8] == 0x20);
	for (i = 3; i < 18; i++) {
		if (i != 5 && i != 8)
			CHECK(request->payload[i] == 0);
	}

	/* The radio was never brought up at a probe, and the firmware never failed. */
	CHECK(count_requests("up") == 0);
	CHECK(chip.sequence_errors == 0);

	/* Nothing the bus moved is left in its buffers. */
	CHECK(all_zero(device->bus.tx_frame, sizeof(device->bus.tx_frame)));
	CHECK(all_zero(device->bus.rx_frame, sizeof(device->bus.rx_frame)));
	CHECK(all_zero(device->bus.chip.bounce, sizeof(device->bus.chip.bounce)));
	bcm43455_device_destroy(device);
}

/* The probe refuses addresses that are not the chip's own, and a firmware without its country. */
static void
check_refusals(void)
{
	struct bcm43455_device *device;
	char text[32];

	/* The NVRAM's placeholder reported by the chip is refused. */
	model_reset();
	snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x",
	    chip_mac[0], chip_mac[1], chip_mac[2], chip_mac[3], chip_mac[4], chip_mac[5]);
	device = make_device(text);
	CHECK(bcm43455_device_probe(device) == EADDRNOTAVAIL);
	CHECK(chip.powered == 0);
	bcm43455_device_destroy(device);

	/* A firmware that will not take XX is not brought up, and the chip is powered down. */
	model_reset();
	chip.refuse_country = 1;
	device = make_device(placeholder_text);
	CHECK(bcm43455_device_probe(device) == EIO);
	CHECK(chip.powered == 0 && chip.power_downs == 1);
	bcm43455_device_destroy(device);

	/* A firmware that refuses revision 0 is asked to choose the revision itself. */
	model_reset();
	chip.refuse_country = 2;
	device = make_device(placeholder_text);
	CHECK(bcm43455_device_probe(device) == 0);
	CHECK(chip.request_count == 17);
	CHECK(memcmp(chip.country, "XX\0\0\xff\xff\xff\xffXX\0\0", 12) == 0);
	bcm43455_device_destroy(device);

	/* A firmware that never answers times out, and the chip is powered down. */
	model_reset();
	chip.silent = 1;
	device = make_device(placeholder_text);
	CHECK(bcm43455_device_probe(device) == ETIMEDOUT);
	CHECK(chip.powered == 0);
	CHECK(all_zero(device->bus.tx_frame, sizeof(device->bus.tx_frame)));
	bcm43455_device_destroy(device);
}

/* Publication, open with the bus thread, data, events, control from another thread, close. */
static void
check_runtime(void)
{
	static const uint8_t secret[32] = {
		0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xf0, 0x01,
		0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78, 0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf1, 0x02
	};
	struct bcm43455_device *device;
	uint8_t frame[1514];
	uint8_t answer[64];
	size_t answered;
	unsigned taken;
	unsigned i;

	/* The probed chip is published as wlan0 with a station of the FullMAC method set. */
	model_reset();
	device = make_device(placeholder_text);
	CHECK(bcm43455_device_probe(device) == 0);
	CHECK(bcm43455_device_publish(device) == 0);
	CHECK(net_creates == 1 && strcmp(fake_net.name, "wlan0") == 0);
	CHECK(memcmp(fake_net.hwaddr, chip_mac, 6) == 0 && fake_net.mtu == 1500);
	CHECK(attaches == 1 && attached_ops != NULL);
	CHECK(attached_ops->quiesce != NULL && attached_ops->stop_retry != NULL && attached_ops->restart != NULL);
	CHECK(attached_ops->scan_channel_start != NULL && attached_ops->scan_stop != NULL && attached_ops->disconnect != NULL);
	CHECK(attached_ops->fullmac_connect != NULL && attached_ops->ethernet_transmit != NULL);
	CHECK(attached_ops->connect_start == NULL && attached_ops->management_transmit == NULL);
	CHECK(attached_ops->association_set == NULL && attached_ops->association_clear == NULL);
	CHECK(attached_ops->frame_transmit == NULL && attached_ops->key_install == NULL);
	CHECK(attached_ops->key_delete == NULL && attached_ops->keys_activate == NULL);
	CHECK(attached_profile.channel_count == 13);
	CHECK(bcm43455_device_publish(device) == EALREADY);

	/* Opening starts the chip with its radio up and the bus thread serving it. */
	model_reset();
	CHECK(fake_net.ops->open(&fake_net) == 0);
	CHECK(opens == 1 && device->runtime_active == 1 && device->bus.thread_running == 1);
	CHECK(count_requests("up") == 1);
	CHECK(chip.irq_enabled == 1);

	/* A request from another thread goes through the slot and comes back. */
	answered = 0;
	CHECK(bcm43455_bus_iovar_get(&device->bus, "cur_etheraddr", answer, 6, &answered) == 0);
	CHECK(answered == 6 && memcmp(answer, chip_mac, 6) == 0);

	/* A secret set through the slot reaches the firmware and stays in no buffer. */
	CHECK(bcm43455_bus_iovar_set(&device->bus, "test_secret", secret, sizeof(secret)) == 0);
	pthread_mutex_lock(&model_lock);
	CHECK(memcmp(chip.secret_seen, secret, sizeof(secret)) == 0);
	pthread_mutex_unlock(&model_lock);
	CHECK(all_zero(device->bus.tx_frame, sizeof(device->bus.tx_frame)));
	CHECK(all_zero(device->bus.rx_frame, sizeof(device->bus.rx_frame)));
	CHECK(all_zero(device->bus.slot.request, sizeof(device->bus.slot.request)));
	CHECK(all_zero(device->bus.slot.answer, sizeof(device->bus.slot.answer)));
	CHECK(device->bus.slot.state == BCM43455_SLOT_EMPTY);

	/* An answer carrying the secret back is cleared from the slot once copied out. */
	answered = 0;
	CHECK(bcm43455_bus_iovar_get(&device->bus, "test_secret", answer, 32, &answered) == 0);
	CHECK(answered == 32 && memcmp(answer, secret, 32) == 0);
	CHECK(all_zero(device->bus.slot.answer, sizeof(device->bus.slot.answer)));

	/* Without a connection a frame is refused, and a received one is dropped before the ring. */
	for (i = 0; i < sizeof(frame); i++)
		frame[i] = (uint8_t)i;
	CHECK(bcm43455_device_ethernet_transmit(device, 1, frame, sizeof(frame)) == ESTALE);
	CHECK(bcm43455_device_ethernet_transmit(device, 1, frame, 1601) == EMSGSIZE);
	model_push_data(frame, 100);
	for (i = 0; i < 200; i++) {
		if (device->bus.counters.data_in != 0)
			break;
		sleep_ms(5);
	}
	CHECK(device->bus.counters.data_in == 1 && device->rx_ring.count == 0 && device->rx_ring.dropped == 1);
	(void)taken;

	/* An event is counted. */
	model_push_event(BCM43455_EVENT_LINK, 0, 0);
	for (i = 0; i < 200; i++) {
		if (device->events != 0)
			break;
		sleep_ms(5);
	}
	CHECK(device->events == 1);

	/* Closing stops the thread, takes the radio down and cuts the power under the station's barrier. */
	fake_net.ops->close(&fake_net);
	CHECK(device->bus.thread_running == 0 && device->runtime_active == 0);
	CHECK(count_requests("down") == 1);
	CHECK(chip.powered == 0 && chip.irq_enabled == 0);
	CHECK(closes == 2 && quiesces == 1 && quiesce_ends == 1);
	CHECK(stop_requests == 1 && stop_completed == 0);
	CHECK(all_zero(device->bus.tx_frame, sizeof(device->bus.tx_frame)));
	CHECK(all_zero(device->bus.rx_frame, sizeof(device->bus.rx_frame)));

	/* A request while closed is refused at once. */
	CHECK(bcm43455_bus_iovar_get(&device->bus, "ver", answer, 8, &answered) == ENETDOWN);
	CHECK(bcm43455_device_ethernet_transmit(device, 1, frame, 60) == ENETDOWN);

	/* The station's quiesce after the close has nothing left to stop. */
	CHECK(attached_ops->quiesce(device) == 0);
	CHECK(chip.power_downs == 1);
}

/* A bad frame, an oversized frame, missing credit and a halted firmware. */
static void
check_failures(void)
{
	struct bcm43455_device *device;
	uint8_t big[5000];
	uint8_t answer[64];
	size_t answered;
	unsigned i;

	/* Opens a fresh chip; the device of check_runtime stays published, so this one is not. */
	model_reset();
	device = make_device(placeholder_text);
	mutex_lock(&device->lifecycle_lock);
	CHECK(bcm43455_device_start(device, 1) == 0);
	mutex_unlock(&device->lifecycle_lock);

	/* A garbled frame tag is terminated and refused, and the next frame still arrives. */
	pthread_mutex_lock(&model_lock);
	model_push_locked(BCM43455_SDPCM_CHANNEL_DATA, (const uint8_t *)"\x20\0\0\0", 4);
	chip.queue[(chip.queue_head + chip.queue_count - 1U) % QUEUE_MAX].bytes[2] ^= 0x01;
	model_raise_locked(BCM43455_INT_FRAME);
	pthread_mutex_unlock(&model_lock);
	answered = 0;
	CHECK(bcm43455_bus_iovar_get(&device->bus, "cur_etheraddr", answer, 6, &answered) == 0);
	CHECK(chip.io_aborts == 1 && chip.read_terminates == 1 && chip.naks == 1);
	CHECK(device->bus.counters.bad_frames == 1);

	/* A frame longer than the receive buffer is read through and dropped. */
	memset(big, 0x77, sizeof(big));
	big[0] = 0x20;
	big[3] = 0;
	pthread_mutex_lock(&model_lock);
	model_push_locked(BCM43455_SDPCM_CHANNEL_DATA, big, sizeof(big));
	model_raise_locked(BCM43455_INT_FRAME);
	pthread_mutex_unlock(&model_lock);
	CHECK(bcm43455_bus_iovar_get(&device->bus, "cur_etheraddr", answer, 6, &answered) == 0);
	CHECK(device->bus.counters.dropped == 1);
	CHECK(all_zero(device->bus.rx_frame, sizeof(device->bus.rx_frame)));

	/* Without credit a request waits and times out; the slot is left clean. */
	pthread_mutex_lock(&model_lock);
	chip.credit = 0;
	pthread_mutex_unlock(&model_lock);
	CHECK(bcm43455_bus_iovar_get(&device->bus, "ver", answer, 8, &answered) == 0);
	CHECK(bcm43455_bus_iovar_get(&device->bus, "ver", answer, 8, &answered) == ETIMEDOUT);
	CHECK(device->bus.slot.state == BCM43455_SLOT_EMPTY);
	CHECK(all_zero(device->bus.slot.request, sizeof(device->bus.slot.request)));
	pthread_mutex_lock(&model_lock);
	chip.credit = 8;
	pthread_mutex_unlock(&model_lock);
	model_push_event(BCM43455_EVENT_LINK, 0, 0);
	for (i = 0; i < 100; i++) {
		if (bcm43455_sdpcm_window(device->bus.tx_credit_limit, device->bus.tx_sequence) != 0)
			break;
		sleep_ms(5);
	}
	CHECK(bcm43455_bus_iovar_get(&device->bus, "ver", answer, 8, &answered) == 0);

	/* A firmware that stops answering leaves an abandoned request, which a late answer clears. */
	pthread_mutex_lock(&model_lock);
	chip.silent = 1;
	pthread_mutex_unlock(&model_lock);
	CHECK(bcm43455_bus_iovar_get(&device->bus, "ver", answer, 8, &answered) == ETIMEDOUT);
	CHECK(device->bus.slot.state == BCM43455_SLOT_ABANDONED);
	CHECK(all_zero(device->bus.slot.request, sizeof(device->bus.slot.request)));
	pthread_mutex_lock(&model_lock);
	chip.silent = 0;
	pthread_mutex_unlock(&model_lock);

	/* A halted firmware ends the bus thread with EIO, which the device records. */
	pthread_mutex_lock(&model_lock);
	chip.to_host_data = BCM43455_TO_HOST_FIRMWARE_HALT;
	model_raise_locked(BCM43455_INT_HOST_MAILBOX);
	pthread_mutex_unlock(&model_lock);
	for (i = 0; i < 200; i++) {
		if (device->bus_error != 0)
			break;
		sleep_ms(5);
	}
	CHECK(device->bus_error == EIO);
	CHECK(bcm43455_bus_iovar_get(&device->bus, "ver", answer, 8, &answered) == ENETDOWN);

	/* The stop after the failure reports it and still powers the chip down. */
	mutex_lock(&device->lifecycle_lock);
	bcm43455_device_stop(device);
	mutex_unlock(&device->lifecycle_lock);
	CHECK(chip.powered == 0);
	bcm43455_device_destroy(device);
}

/* The arguments of one smp requester. */
struct requester {
	struct bcm43455_device *device;
	unsigned index;
	unsigned done;
	unsigned refused;
};

/* Requests the chip's address again and again. */
static void *
smp_requester(
	void *argument)
{
	struct requester *requester;
	uint8_t answer[8];
	size_t answered;
	int error;

	requester = argument;
	while (!__atomic_load_n(&smp_done, __ATOMIC_SEQ_CST)) {
		error = bcm43455_bus_iovar_get(&requester->device->bus, "cur_etheraddr", answer, 6, &answered);
		if (error == 0) {
			CHECK(answered == 6 && memcmp(answer, chip_mac, 6) == 0);
			requester->done++;
		} else {
			CHECK(error == ENETDOWN || error == ETIMEDOUT);
			requester->refused++;
		}
	}
	return NULL;
}

/* Queues frames to send again and again. */
static void *
smp_transmitter(
	void *argument)
{
	struct requester *requester;
	uint8_t frame[200];
	int error;

	requester = argument;
	memset(frame, 0x42, sizeof(frame));
	while (!__atomic_load_n(&smp_done, __ATOMIC_SEQ_CST)) {
		error = bcm43455_device_ethernet_transmit(requester->device, 1, frame, sizeof(frame));
		if (error == 0)
			requester->done++;
		else
			requester->refused++;
		sleep_ms(1);
	}
	return NULL;
}

/* Sends events and data frames again and again. */
static void *
smp_firmware(
	void *argument)
{
	uint8_t frame[100];

	(void)argument;
	memset(frame, 0x24, sizeof(frame));
	while (!__atomic_load_n(&smp_done, __ATOMIC_SEQ_CST)) {
		model_push_event(BCM43455_EVENT_LINK, 0, 0);
		model_push_data(frame, sizeof(frame));
		sleep_ms(1);
	}
	return NULL;
}

/* Requesters, a transmitter and the firmware race the bus thread; then it is stopped under them. */
static void
run_smp(void)
{
	struct bcm43455_device *device;
	struct requester requesters[5];
	pthread_t threads[6];
	unsigned total;
	unsigned long enabled;
	unsigned i;

	/* A running chip, joined to a network as connection 1 so data flows both ways. */
	model_reset();
	device = make_device(placeholder_text);
	mutex_lock(&device->lifecycle_lock);
	CHECK(bcm43455_device_start(device, 1) == 0);
	mutex_unlock(&device->lifecycle_lock);
	enabled = spin_lock_irqsave(&device->ring_lock);
	device->connection.state = BCM43455_CONNECT_JOINING;
	device->connection.generation = 1;
	spin_unlock_irqrestore(&device->ring_lock, enabled);

	/* Starts four requesters, a transmitter and the firmware's sender. */
	__atomic_store_n(&smp_done, 0, __ATOMIC_SEQ_CST);
	for (i = 0; i < 5; i++) {
		requesters[i].device = device;
		requesters[i].index = i;
		requesters[i].done = 0;
		requesters[i].refused = 0;
	}
	for (i = 0; i < 4; i++)
		CHECK(pthread_create(&threads[i], NULL, smp_requester, &requesters[i]) == 0);
	CHECK(pthread_create(&threads[4], NULL, smp_transmitter, &requesters[4]) == 0);
	CHECK(pthread_create(&threads[5], NULL, smp_firmware, NULL) == 0);

	/* Lets them run, then stops the bus under them. */
	sleep_ms(1500);
	mutex_lock(&device->lifecycle_lock);
	bcm43455_device_stop(device);
	mutex_unlock(&device->lifecycle_lock);
	sleep_ms(50);
	__atomic_store_n(&smp_done, 1, __ATOMIC_SEQ_CST);
	for (i = 0; i < 6; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);

	/* Every requester was answered many times and refused after the stop. */
	total = 0;
	for (i = 0; i < 4; i++) {
		CHECK(requesters[i].done > 0);
		total += requesters[i].done;
	}
	printf("  smp: %u answers, %u frames sent, %llu events, %llu data frames in\n",
	    total, chip.data_sent_count,
	    (unsigned long long)device->events,
	    (unsigned long long)device->bus.counters.data_in);
	CHECK(chip.data_sent_count > 0 && device->events > 0);
	CHECK(chip.sequence_errors == 0);
	CHECK(device->bus.thread_running == 0 && chip.powered == 0);
	CHECK(device->bus.slot.state == BCM43455_SLOT_EMPTY || device->bus.slot.state == BCM43455_SLOT_ANSWERED);
	CHECK(all_zero(device->bus.slot.request, sizeof(device->bus.slot.request)));
	bcm43455_device_destroy(device);
}

/* Channel specifications, escan requests and results, the network description, the join and key messages. */
static void
check_radio_codecs(void)
{
	static const uint8_t pmk[32] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0xff
	};
	static const char hex[] = "000102030405060708090a0b0c0d0e0ff0e1d2c3b4a5968778695a4b3c2d1eff";
	struct wlan_bss_record parsed;
	uint8_t buffer[256];
	uint8_t bss[512];
	uint8_t frame[BCM43455_SCAN_FRAME_MAX];
	uint8_t data[600];
	uint8_t key[BCM43455_KEY_MESSAGE_LENGTH];
	size_t length;
	size_t bss_length;
	size_t first;
	uint32_t channel;
	uint16_t sync_id;
	unsigned count;
	int32_t rssi;

	/* 20 MHz specifications in both layouts; numbers outside the bands are none. */
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_AC, 6) == 0x1006);
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_AC, 36) == 0xd024);
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_AC, 165) == 0xd0a5);
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_N, 6) == 0x2b06);
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_N, 36) == 0x1b24);
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0) == 0);
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_AC, 15) == 0);
	CHECK(bcm43455_chanspec_from_channel(BCM43455_CHANSPEC_LAYOUT_AC, 200) == 0);

	/* The primary channel of 20, 40 and 80 MHz specifications (802.11ac). */
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0x1006, &channel) == 0 && channel == 6);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xd826, &channel) == 0 && channel == 36);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xd926, &channel) == 0 && channel == 40);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0x1803, &channel) == 0 && channel == 1);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xe02a, &channel) == 0 && channel == 36);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xe12a, &channel) == 0 && channel == 40);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xe22a, &channel) == 0 && channel == 44);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xe32a, &channel) == 0 && channel == 48);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xe432, &channel) == EINVAL);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xda26, &channel) == EINVAL);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_AC, 0xe832, &channel) == EINVAL);

	/* The same in the 802.11n layout. */
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_N, 0x2b06, &channel) == 0 && channel == 6);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_N, 0x1d26, &channel) == 0 && channel == 36);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_N, 0x1e26, &channel) == 0 && channel == 40);
	CHECK(bcm43455_chanspec_control_channel(BCM43455_CHANSPEC_LAYOUT_N, 0x1f26, &channel) == EINVAL);

	/* An escan start of layout 0: one channel, active, any network, the firmware's timing. */
	CHECK(bcm43455_escan_encode(buffer, sizeof(buffer), BCM43455_SCAN_LAYOUT_V0, BCM43455_ESCAN_START, 0x0102, 0x1006, 0, &length) == 0);
	CHECK(length == 76);
	CHECK(bcm43455_get_le32(buffer) == 1 && bcm43455_get_le16(buffer + 4) == 1 && bcm43455_get_le16(buffer + 6) == 0x0102);
	CHECK(bcm43455_get_le32(buffer + 8) == 0 && buffer[8 + 36] == 0xff && buffer[8 + 41] == 0xff);
	CHECK(buffer[8 + 42] == 2 && buffer[8 + 43] == 0);
	CHECK(bcm43455_get_le32(buffer + 8 + 44) == 0xffffffffU && bcm43455_get_le32(buffer + 8 + 56) == 0xffffffffU);
	CHECK(bcm43455_get_le32(buffer + 8 + 60) == 1 && bcm43455_get_le16(buffer + 8 + 64) == 0x1006);
	CHECK(bcm43455_escan_encode(buffer, sizeof(buffer), BCM43455_SCAN_LAYOUT_V0, BCM43455_ESCAN_START, 1, 0x1006, 1, &length) == 0);
	CHECK(buffer[8 + 43] == 1);

	/* Its abort lists no channel; layout 2 carries its own version and length. */
	CHECK(bcm43455_escan_encode(buffer, sizeof(buffer), BCM43455_SCAN_LAYOUT_V0, BCM43455_ESCAN_ABORT, 0, 0, 0, &length) == 0);
	CHECK(length == 72 && bcm43455_get_le16(buffer + 4) == 3 && bcm43455_get_le32(buffer + 8 + 60) == 0);
	CHECK(bcm43455_escan_encode(buffer, sizeof(buffer), BCM43455_SCAN_LAYOUT_V2, BCM43455_ESCAN_START, 7, 0xd024, 1, &length) == 0);
	CHECK(length == 84 && length <= BCM43455_ESCAN_REQUEST_MAX);
	CHECK(bcm43455_get_le32(buffer) == 2 && bcm43455_get_le16(buffer + 8) == 2 && bcm43455_get_le16(buffer + 10) == 84);
	CHECK(bcm43455_get_le32(buffer + 8 + 48) == 1 && bcm43455_get_le32(buffer + 8 + 68) == 1);
	CHECK(bcm43455_get_le16(buffer + 8 + 72) == 0xd024);
	CHECK(bcm43455_escan_encode(buffer, 75, BCM43455_SCAN_LAYOUT_V0, BCM43455_ESCAN_START, 1, 0x1006, 0, &length) == ENOSPC);
	CHECK(bcm43455_escan_encode(buffer, sizeof(buffer), 1, BCM43455_ESCAN_START, 1, 0x1006, 0, &length) == EINVAL);
	CHECK(bcm43455_escan_encode(buffer, sizeof(buffer), 0, 2, 1, 0x1006, 0, &length) == EINVAL);

	/* A network description becomes a probe response the core's parser reads as WPA2-PSK CCMP. */
	bss_length = model_bss(bss, "zed-test", 0x0c, 0x1006);
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == 0);
	CHECK(channel == 6 && rssi == -42 && frame[0] == 0x50);
	CHECK(wlan_frame_parse_bss(frame, length, rssi, (uint8_t)channel, &parsed) == 0);
	CHECK(parsed.ssid_length == 8 && memcmp(parsed.ssid, "zed-test", 8) == 0);
	CHECK(parsed.bssid[0] == 0x02 && parsed.bssid[5] == 0x0c && parsed.channel == 6);
	CHECK(parsed.beacon_interval_tu == 100 && parsed.capability == 0x0411 && parsed.rssi_dbm == -42);
	CHECK((parsed.security & WLAN_SECURITY_WPA2) != 0 && (parsed.security & WLAN_SECURITY_PSK) != 0);
	CHECK((parsed.security & WLAN_SECURITY_CCMP) != 0 && (parsed.security & WLAN_SECURITY_PRIVACY) != 0);

	/* The description's own primary channel wins over the specification's; a 40 MHz one is decoded. */
	bss_length = model_bss(bss, "zed-wide", 0x0d, 0xd926);
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == 0 && channel == 40);
	bss[88] = 36;
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == 0 && channel == 36);
	CHECK(wlan_frame_parse_bss(frame, length, rssi, (uint8_t)channel, &parsed) == 0 && parsed.channel == 36);

	/* Malformed descriptions are refused: elements past the end, a long SSID, a short length, no room. */
	bss_length = model_bss(bss, "zed-test", 0x0c, 0x1006);
	bcm43455_put_le32(bss + 120, (uint32_t)bss_length);
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == EPROTO);
	bss_length = model_bss(bss, "zed-test", 0x0c, 0x1006);
	bss[18] = 33;
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == EPROTO);
	bss_length = model_bss(bss, "zed-test", 0x0c, 0x1006);
	bss[128 + 1] = 200;
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == EPROTO);
	bss_length = model_bss(bss, "zed-test", 0x0c, 0x1006);
	CHECK(bcm43455_bss_to_frame(bss, 127, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == EPROTO);
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, 60, &length, &channel, &rssi) == EMSGSIZE);
	bcm43455_put_le16(bss + 72, 0xe432);
	CHECK(bcm43455_bss_to_frame(bss, bss_length, BCM43455_CHANSPEC_LAYOUT_AC, frame, sizeof(frame), &length, &channel, &rssi) == EPROTO);

	/* An escan result's header and the walk over its descriptions. */
	memset(data, 0, sizeof(data));
	bss_length = model_bss(data + 12, "zed-a", 1, 0x1001);
	bcm43455_put_le16(data + 8, 0x4242);
	bcm43455_put_le16(data + 10, 1);
	CHECK(bcm43455_escan_result_decode(data, 12 + bss_length, &sync_id, &count, &first) == 0);
	CHECK(sync_id == 0x4242 && count == 1 && first == 12);
	CHECK(bcm43455_escan_result_decode(data, 11, &sync_id, &count, &first) == EPROTO);
	CHECK(bcm43455_bss_length(data, 12 + bss_length, 12, &length) == 0 && length == bss_length);
	CHECK(bcm43455_bss_length(data, 12 + bss_length - 1, 12, &length) == EPROTO);
	CHECK(bcm43455_bss_length(data, 12 + 100, 12, &length) == EPROTO);

	/* The join request: the SSID, the BSSID, one channel. */
	CHECK(bcm43455_join_encode(buffer, sizeof(buffer), (const uint8_t *)"zed-net", 7, chip_mac, 0x1006, &length) == 0);
	CHECK(length == 52 && bcm43455_get_le32(buffer) == 7 && memcmp(buffer + 4, "zed-net", 7) == 0);
	CHECK(memcmp(buffer + 36, chip_mac, 6) == 0 && bcm43455_get_le16(buffer + 42) == 0);
	CHECK(bcm43455_get_le32(buffer + 44) == 1 && bcm43455_get_le16(buffer + 48) == 0x1006);
	CHECK(bcm43455_join_encode(buffer, sizeof(buffer), (const uint8_t *)"x", 33, chip_mac, 0x1006, &length) == EINVAL);
	CHECK(bcm43455_join_encode(buffer, 51, (const uint8_t *)"x", 1, chip_mac, 0x1006, &length) == ENOSPC);

	/* The key message: 64 lower-case hex digits marked as text, then zeros. */
	bcm43455_key_encode(key, pmk);
	CHECK(bcm43455_get_le16(key) == 64 && bcm43455_get_le16(key + 2) == 1);
	CHECK(memcmp(key + 4, hex, 64) == 0 && key[68] == 0 && key[69] == 0);

	/* The escan statuses. */
	CHECK(bcm43455_scan_classify(8) == BCM43455_SCAN_STATUS_RESULTS);
	CHECK(bcm43455_scan_classify(0) == BCM43455_SCAN_STATUS_COMPLETE);
	CHECK(bcm43455_scan_classify(4) == BCM43455_SCAN_STATUS_ABORTED);
	CHECK(bcm43455_scan_classify(9) == BCM43455_SCAN_STATUS_BUSY);
	CHECK(bcm43455_scan_classify(10) == BCM43455_SCAN_STATUS_BUSY);
	CHECK(bcm43455_scan_classify(1) == BCM43455_SCAN_STATUS_FAILED);
	CHECK(bcm43455_scan_classify(13) == BCM43455_SCAN_STATUS_FAILED);
}

/* Classifies one event at a connection's progress. */
static void
classify(
	uint32_t type,
	uint32_t status,
	uint32_t reason,
	uint16_t flags,
	unsigned associated,
	unsigned authorized,
	enum bcm43455_connect_step step,
	int error)
{
	struct bcm43455_event event;
	struct bcm43455_connect_outcome outcome;

	memset(&event, 0, sizeof(event));
	event.type = type;
	event.status = status;
	event.reason = reason;
	event.flags = flags;
	bcm43455_connect_classify(&event, associated, authorized, &outcome);
	if (outcome.step != step || outcome.error != error) {
		fprintf(stderr, "bcm43455-host-test: event %u status %u reason %u flags %u at %u/%u: step %d error %d, expected %d %d\n",
		    type, status, reason, flags, associated, authorized, outcome.step, outcome.error, step, error);
		CHECK(0);
	}
}

/* The table of what each event means to a connection, and the errors networkd relies on. */
static void
check_connect_table(void)
{
	/* Joining: the join result's success or the link coming up. */
	classify(BCM43455_EVENT_JOIN_RESULT, 0, 0, 0, 0, 0, BCM43455_CONNECT_ASSOCIATED, 0);
	classify(BCM43455_EVENT_LINK, 0, 0, 1, 0, 0, BCM43455_CONNECT_ASSOCIATED, 0);
	classify(BCM43455_EVENT_JOIN_RESULT, 0, 0, 0, 1, 0, BCM43455_CONNECT_NONE, 0);
	classify(BCM43455_EVENT_LINK, 0, 0, 1, 1, 0, BCM43455_CONNECT_NONE, 0);

	/* The join's failures: no network, a timeout or no answer, a refusal. */
	classify(BCM43455_EVENT_JOIN_RESULT, 3, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ETIMEDOUT);
	classify(BCM43455_EVENT_JOIN_RESULT, 2, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ETIMEDOUT);
	classify(BCM43455_EVENT_JOIN_RESULT, 5, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ETIMEDOUT);
	classify(BCM43455_EVENT_JOIN_RESULT, 1, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ECONNREFUSED);
	classify(BCM43455_EVENT_JOIN_RESULT, 6, 0, 0, 0, 0, BCM43455_CONNECT_NONE, 0);
	classify(BCM43455_EVENT_AUTHENTICATION, 1, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ECONNREFUSED);
	classify(BCM43455_EVENT_AUTHENTICATION, 2, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ETIMEDOUT);
	classify(BCM43455_EVENT_ASSOCIATION, 5, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ETIMEDOUT);
	classify(BCM43455_EVENT_ASSOCIATION, 1, 0, 0, 0, 0, BCM43455_CONNECT_LOST, ECONNREFUSED);
	classify(BCM43455_EVENT_AUTHENTICATION, 0, 0, 0, 0, 0, BCM43455_CONNECT_NONE, 0);
	classify(BCM43455_EVENT_AUTHENTICATION, 6, 0, 0, 0, 0, BCM43455_CONNECT_NONE, 0);
	classify(BCM43455_EVENT_ASSOCIATION, 1, 0, 0, 1, 0, BCM43455_CONNECT_NONE, 0);

	/* The supplicant: keyed, and its failures; only after the join, only before the key. */
	classify(BCM43455_EVENT_SUPPLICANT, 6, 0, 0, 1, 0, BCM43455_CONNECT_AUTHORIZED, 0);
	classify(BCM43455_EVENT_SUPPLICANT, 6, 0, 0, 0, 0, BCM43455_CONNECT_NONE, 0);
	classify(BCM43455_EVENT_SUPPLICANT, 6, 0, 0, 1, 1, BCM43455_CONNECT_NONE, 0);
	classify(BCM43455_EVENT_SUPPLICANT, 4, 15, 0, 1, 0, BCM43455_CONNECT_LOST, ETIMEDOUT);
	classify(BCM43455_EVENT_SUPPLICANT, 10, 15, 0, 1, 0, BCM43455_CONNECT_LOST, ETIMEDOUT);
	classify(BCM43455_EVENT_SUPPLICANT, 8, 15, 0, 1, 0, BCM43455_CONNECT_LOST, EACCES);
	classify(BCM43455_EVENT_SUPPLICANT, 8, 14, 0, 1, 0, BCM43455_CONNECT_LOST, EACCES);
	classify(BCM43455_EVENT_SUPPLICANT, 4, 14, 0, 1, 0, BCM43455_CONNECT_LOST, EPROTO);
	classify(BCM43455_EVENT_SUPPLICANT, 9, 7, 0, 1, 0, BCM43455_CONNECT_LOST, EPROTO);
	classify(BCM43455_EVENT_SUPPLICANT, 5, 1, 0, 1, 0, BCM43455_CONNECT_LOST, EPROTO);
	classify(BCM43455_EVENT_SUPPLICANT, 11, 10, 0, 1, 0, BCM43455_CONNECT_LOST, EPROTO);

	/* Leaving: after the key a plain reset; before it, the access point's four-way timeout is the key's refusal. */
	classify(BCM43455_EVENT_PEER_DEAUTHENTICATED, 0, 3, 0, 1, 1, BCM43455_CONNECT_LOST, ECONNRESET);
	classify(BCM43455_EVENT_PEER_DISASSOCIATED, 0, 8, 0, 1, 1, BCM43455_CONNECT_LOST, ECONNRESET);
	classify(BCM43455_EVENT_PEER_DEAUTHENTICATED, 0, 15, 0, 1, 1, BCM43455_CONNECT_LOST, ECONNRESET);
	classify(BCM43455_EVENT_PEER_DEAUTHENTICATED, 0, 15, 0, 1, 0, BCM43455_CONNECT_LOST, EACCES);
	classify(BCM43455_EVENT_PEER_DISASSOCIATED, 0, 15, 0, 1, 0, BCM43455_CONNECT_LOST, EACCES);
	classify(BCM43455_EVENT_DEAUTHENTICATED, 0, 15, 0, 1, 0, BCM43455_CONNECT_LOST, ECONNRESET);
	classify(BCM43455_EVENT_PEER_DEAUTHENTICATED, 0, 2, 0, 1, 0, BCM43455_CONNECT_LOST, ECONNRESET);
	classify(BCM43455_EVENT_PEER_DEAUTHENTICATED, 0, 15, 0, 0, 0, BCM43455_CONNECT_NONE, 0);
	classify(BCM43455_EVENT_LINK, 0, 0, 0, 1, 1, BCM43455_CONNECT_LOST, ECONNRESET);
	classify(BCM43455_EVENT_LINK, 0, 0, 0, 1, 0, BCM43455_CONNECT_LOST, ECONNRESET);
	classify(BCM43455_EVENT_LINK, 0, 0, 0, 0, 0, BCM43455_CONNECT_NONE, 0);

	/* An escan result says nothing about the connection. */
	classify(BCM43455_EVENT_SCAN_RESULT, 0, 0, 0, 1, 1, BCM43455_CONNECT_NONE, 0);
}

/* Forgets the reports the stand-ins recorded. */
static void
reports_clear(void)
{
	pthread_mutex_lock(&report_lock);
	report_count = 0;
	memset(reports, 0, sizeof(reports));
	pthread_mutex_unlock(&report_lock);
}

/* Takes a copy of report index. */
static struct report
report_at(
	unsigned index)
{
	struct report copy;

	pthread_mutex_lock(&report_lock);
	CHECK(index < report_count && report_count <= REPORT_MAX);
	copy = reports[index];
	pthread_mutex_unlock(&report_lock);
	return copy;
}

/* How many reports there are. */
static unsigned
reports_made(void)
{
	unsigned count;

	pthread_mutex_lock(&report_lock);
	count = report_count;
	pthread_mutex_unlock(&report_lock);
	return count;
}

/* Runs the poll until count reports were made, failing after two seconds. */
static void
poll_until(
	unsigned count)
{
	unsigned i;

	for (i = 0; i < 1000; i++) {
		(void)fake_net.ops->poll_receive(&fake_net, 64);
		if (reports_made() >= count)
			return;
		sleep_ms(2);
	}
	fprintf(stderr, "bcm43455-host-test: %u reports, expected %u\n", reports_made(), count);
	CHECK(0);
}

/* Runs the poll for a while and checks that no report came beyond count. */
static void
poll_quiet(
	unsigned count,
	unsigned milliseconds)
{
	unsigned i;

	for (i = 0; i < milliseconds / 5U; i++) {
		(void)fake_net.ops->poll_receive(&fake_net, 64);
		sleep_ms(5);
	}
	CHECK(reports_made() == count);
}

/* Waits until the bus thread has taken events events in total. */
static void
wait_events(
	struct bcm43455_device *device,
	uint64_t events)
{
	unsigned i;

	for (i = 0; i < 400; i++) {
		if (__atomic_load_n(&device->bus.counters.events_in, __ATOMIC_SEQ_CST) >= events)
			return;
		sleep_ms(5);
	}
	CHECK(0);
}

/* Checks that no buffer a PMK passes through still holds a byte of it. */
static void
check_no_secret(
	struct bcm43455_device *device)
{
	unsigned long enabled;

	enabled = spin_lock_irqsave(&device->ring_lock);
	CHECK(all_zero(device->connection.pmk, sizeof(device->connection.pmk)));
	spin_unlock_irqrestore(&device->ring_lock, enabled);
	CHECK(all_zero(device->work_key, sizeof(device->work_key)));
	CHECK(all_zero(device->work_buffer, sizeof(device->work_buffer)));
	CHECK(all_zero(device->bus.tx_frame, sizeof(device->bus.tx_frame)));
	CHECK(all_zero(device->bus.slot.request, sizeof(device->bus.slot.request)));
	CHECK(all_zero(device->bus.slot.answer, sizeof(device->bus.slot.answer)));
	CHECK(device->bus.direct.answer == NULL);
}

/* Makes a connection request for generation on channel 6 with the test PMK. */
static void
make_request(
	struct wlan_fullmac_connect_request *request,
	uint64_t generation,
	const uint8_t pmk[32])
{
	memset(request, 0, sizeof(*request));
	request->generation = generation;
	request->deadline_ticks = 30000;
	memcpy(request->bss.ssid, "zed-alpha", 9);
	request->bss.ssid_length = 9;
	request->bss.bssid[0] = 0x02;
	request->bss.bssid[5] = 0x0a;
	request->bss.channel = 6;
	request->bss.center_frequency_mhz = 2437;
	memcpy(request->pmk, pmk, 32);
}

/* A published and opened device on a fresh model, as a user's "wifi up" leaves it. */
static struct bcm43455_device *
radio_device(void)
{
	struct bcm43455_device *device;

	model_reset();
	device = make_device(placeholder_text);
	CHECK(bcm43455_device_probe(device) == 0);
	CHECK(bcm43455_device_publish(device) == 0);
	CHECK(fake_net.ops->open(&fake_net) == 0);
	CHECK(device->runtime_active == 1 && device->bus.thread_running == 1 && device->opened == 1);
	return device;
}

/* The scan: the order of reports, results before the answer, stale results, refusal, busy, abort. */
static void
check_scan(
	struct bcm43455_device *device)
{
	const struct wlan_radio_ops *ops;
	struct report report;
	uint64_t events;
	unsigned base;

	ops = attached_ops;

	/* A channel: ready, both networks as WPA2-PSK CCMP probe responses, then its end. */
	reports_clear();
	CHECK(ops->scan_channel_start(device, 5, 0, 1, 0) == 0);
	poll_until(4);
	report = report_at(0);
	CHECK(report.kind == REPORT_READY && report.generation == 5 && report.value == 0);
	report = report_at(1);
	CHECK(report.kind == REPORT_FRAME && report.generation == 5 && strcmp(report.ssid, "zed-alpha") == 0);
	CHECK(report.channel == 1 && report.rssi == -42);
	CHECK((report.security & (WLAN_SECURITY_WPA2 | WLAN_SECURITY_PSK | WLAN_SECURITY_CCMP)) == (WLAN_SECURITY_WPA2 | WLAN_SECURITY_PSK | WLAN_SECURITY_CCMP));
	report = report_at(2);
	CHECK(report.kind == REPORT_FRAME && strcmp(report.ssid, "zed-beta") == 0);
	report = report_at(3);
	CHECK(report.kind == REPORT_COMPLETE && report.generation == 5 && report.value == 0);
	CHECK(chip.escan_starts == 1 && chip.escan_chanspec == 0x1001 && chip.escan_scan_type == 0);

	/*
	 * The next channel, whose networks and end the firmware sends before
	 * it answers the request: nothing is reported until the answer, then
	 * the order holds.
	 */
	chip.scan_mode = SCAN_RESULTS_FIRST;
	events = device->bus.counters.events_in;
	CHECK(ops->scan_channel_start(device, 5, 1, 13, 0) == 0);
	wait_events(device, events + 2U);
	poll_quiet(4, 60);
	CHECK(device->scan_ring.count == 2 && device->scan.complete_pending == 1);
	pthread_mutex_lock(&model_lock);
	CHECK(chip.held_length != 0);
	model_push_locked(BCM43455_SDPCM_CHANNEL_CONTROL, chip.held_answer, chip.held_length);
	model_raise_locked(BCM43455_INT_FRAME);
	chip.held_length = 0;
	pthread_mutex_unlock(&model_lock);
	poll_until(8);
	CHECK(report_at(4).kind == REPORT_READY && report_at(4).value == 1);
	CHECK(report_at(5).kind == REPORT_FRAME && report_at(5).channel == 13);
	CHECK(report_at(6).kind == REPORT_FRAME);
	CHECK(report_at(7).kind == REPORT_COMPLETE && report_at(7).value == 1);
	CHECK(chip.escan_scan_type == 1 && chip.escan_chanspec == 0x100d);

	/* A finished channel needs no abort; the scan is forgotten. */
	CHECK(ops->scan_stop(device, 5) == 0);
	CHECK(chip.escan_aborts == 0 && device->scan.state == BCM43455_SCAN_IDLE);

	/* Results of another escan are dropped; the channel hears its ready, then is aborted. */
	reports_clear();
	chip.scan_mode = SCAN_STALE;
	CHECK(ops->scan_channel_start(device, 6, 0, 2, 0) == 0);
	poll_until(1);
	CHECK(report_at(0).kind == REPORT_READY && report_at(0).generation == 6);
	poll_quiet(1, 100);
	CHECK(device->scan_ring.count == 0);

	/* A scan of another generation, or the next channel before the end, is refused. */
	CHECK(ops->scan_channel_start(device, 7, 0, 2, 0) == EBUSY);
	CHECK(ops->scan_channel_start(device, 6, 1, 3, 0) == EBUSY);
	CHECK(ops->scan_channel_start(device, 6, 1, 15, 0) == EINVAL);
	CHECK(ops->scan_stop(device, 6) == 0);
	CHECK(chip.escan_aborts == 1 && device->scan.state == BCM43455_SCAN_IDLE);

	/* A firmware that refuses the escan: an error, and no ready. */
	reports_clear();
	chip.scan_mode = SCAN_REFUSE;
	CHECK(ops->scan_channel_start(device, 8, 0, 1, 0) == 0);
	poll_until(1);
	CHECK(report_at(0).kind == REPORT_SCAN_ERROR && report_at(0).generation == 8 && report_at(0).error == EIO);
	CHECK(ops->scan_stop(device, 8) == 0);
	CHECK(chip.escan_aborts == 1);

	/* A firmware busy with another scan: ready, then EBUSY. */
	reports_clear();
	chip.scan_mode = SCAN_BUSY;
	CHECK(ops->scan_channel_start(device, 9, 0, 1, 0) == 0);
	poll_until(2);
	CHECK(report_at(0).kind == REPORT_READY && report_at(1).kind == REPORT_SCAN_ERROR && report_at(1).error == EBUSY);
	CHECK(ops->scan_stop(device, 9) == 0);

	/* A held scan is aborted, and its late results are not reported. */
	reports_clear();
	chip.scan_mode = SCAN_HOLD;
	CHECK(ops->scan_channel_start(device, 10, 0, 1, 0) == 0);
	poll_until(1);
	base = chip.escan_aborts;
	CHECK(ops->scan_stop(device, 10) == 0);
	CHECK(chip.escan_aborts == base + 1U);
	pthread_mutex_lock(&model_lock);
	model_scan_events_locked(chip.escan_sync, chip.escan_chanspec);
	pthread_mutex_unlock(&model_lock);
	poll_quiet(1, 100);

	/* A stop of a scan never started, or of another generation, does nothing. */
	CHECK(ops->scan_stop(device, 10) == 0);
	CHECK(ops->scan_stop(device, 99) == 0);
	CHECK(chip.escan_aborts == base + 1U);
	chip.scan_mode = SCAN_NORMAL;
}

/* A connection: its commands, its events, data, loss, disconnect, cancel and failures. */
static void
check_connect(
	struct bcm43455_device *device)
{
	static const uint8_t pmk[32] = {
		0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
		0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f
	};
	static const char hex[] = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf505152535455565758595a5b5c5d5e5f";
	const struct wlan_radio_ops *ops;
	struct wlan_fullmac_connect_request request;
	const struct model_request *recorded;
	struct report report;
	uint8_t frame[200];
	uint64_t started_ms;
	uint64_t events;
	unsigned joins_before;
	unsigned i;

	ops = attached_ops;

	/* A join the firmware completes: the commands in WHD's order, then joined and authorized. */
	reports_clear();
	pthread_mutex_lock(&model_lock);
	chip.request_count = 0;
	chip.join_script = JOIN_SUCCESS;
	pthread_mutex_unlock(&model_lock);
	make_request(&request, 9, pmk);
	CHECK(ops->fullmac_connect(device, &request) == 0);
	CHECK(ops->fullmac_connect(device, &request) == EBUSY);
	poll_until(2);
	report = report_at(0);
	CHECK(report.kind == REPORT_EVENT && report.generation == 9 && report.value == WLAN_FULLMAC_EVENT_ASSOCIATED);
	report = report_at(1);
	CHECK(report.kind == REPORT_EVENT && report.generation == 9 && report.value == WLAN_FULLMAC_EVENT_AUTHORIZED);
	pthread_mutex_lock(&model_lock);
	CHECK(chip.request_count == 9);
	CHECK(chip.requests[0].command == BCM43455_COMMAND_INFRASTRUCTURE && bcm43455_get_le32(chip.requests[0].payload) == 1);
	CHECK(chip.requests[1].command == BCM43455_COMMAND_AUTHENTICATION && bcm43455_get_le32(chip.requests[1].payload) == 0);
	CHECK(chip.requests[2].command == BCM43455_COMMAND_CIPHERS && bcm43455_get_le32(chip.requests[2].payload) == 4);
	CHECK(chip.requests[3].command == BCM43455_COMMAND_KEY_MANAGEMENT && bcm43455_get_le32(chip.requests[3].payload) == 0x80);
	CHECK(strcmp(chip.requests[4].name, "bsscfg:sup_wpa") == 0 && chip.requests[4].length == 8);
	CHECK(bcm43455_get_le32(chip.requests[4].payload) == 0 && bcm43455_get_le32(chip.requests[4].payload + 4) == 1);
	CHECK(strcmp(chip.requests[5].name, "bsscfg:sup_wpa2_eapver") == 0 && bcm43455_get_le32(chip.requests[5].payload + 4) == 0xffffffffU);
	CHECK(strcmp(chip.requests[6].name, "bsscfg:sup_wpa_tmo") == 0 && bcm43455_get_le32(chip.requests[6].payload + 4) == 3000);
	CHECK(chip.requests[7].command == BCM43455_COMMAND_SUPPLICANT_KEY);
	CHECK(chip.requests[8].command == BCM43455_COMMAND_JOIN);
	CHECK(bcm43455_get_le16(chip.key_seen) == 64 && bcm43455_get_le16(chip.key_seen + 2) == 1);
	CHECK(memcmp(chip.key_seen + 4, hex, 64) == 0);
	CHECK(bcm43455_get_le32(chip.join_seen) == 9 && memcmp(chip.join_seen + 4, "zed-alpha", 9) == 0);
	CHECK(chip.join_seen[36] == 0x02 && chip.join_seen[41] == 0x0a);
	CHECK(bcm43455_get_le32(chip.join_seen + 44) == 1 && bcm43455_get_le16(chip.join_seen + 48) == 0x1006);
	memset(chip.key_seen, 0, sizeof(chip.key_seen));
	pthread_mutex_unlock(&model_lock);
	check_no_secret(device);

	/* Data both ways once the port is open; an older generation's frame is refused. */
	__atomic_store_n(&open_port_generation, 9, __ATOMIC_SEQ_CST);
	for (i = 0; i < sizeof(frame); i++)
		frame[i] = (uint8_t)(i + 3U);
	CHECK(ops->ethernet_transmit(device, 9, frame, sizeof(frame)) == 0);
	CHECK(ops->ethernet_transmit(device, 8, frame, sizeof(frame)) == ESTALE);
	for (i = 0; i < 200; i++) {
		if (__atomic_load_n(&chip.data_sent_count, __ATOMIC_SEQ_CST) != 0)
			break;
		sleep_ms(5);
	}
	pthread_mutex_lock(&model_lock);
	CHECK(chip.data_sent_count == 1 && chip.data_sent_length == sizeof(frame) && memcmp(chip.data_sent, frame, sizeof(frame)) == 0);
	pthread_mutex_unlock(&model_lock);
	model_push_data(frame, 120);
	poll_until(3);
	report = report_at(2);
	CHECK(report.kind == REPORT_ETHERNET && report.generation == 9 && report.length == 120);

	/* The access point leaves: a plain reset, after which data is no longer taken. */
	events = device->bus.counters.events_in;
	model_push_event_with(BCM43455_EVENT_PEER_DEAUTHENTICATED, 0, 3, 0);
	poll_until(4);
	report = report_at(3);
	CHECK(report.kind == REPORT_LOSS && report.generation == 9 && report.error == ECONNRESET);
	model_push_data(frame, 120);
	poll_quiet(4, 60);
	CHECK(ops->ethernet_transmit(device, 9, frame, sizeof(frame)) == 0);
	sleep_ms(50);
	CHECK(__atomic_load_n(&chip.data_sent_count, __ATOMIC_SEQ_CST) == 1);
	(void)events;

	/* Disconnect leaves the access point once, and forgets the connection. */
	CHECK(ops->disconnect(device, 9) == 0);
	CHECK(chip.leaves == 1 && device->connection.state == BCM43455_CONNECT_IDLE);
	CHECK(ops->disconnect(device, 9) == 0);
	CHECK(chip.leaves == 1);
	CHECK(ops->ethernet_transmit(device, 9, frame, sizeof(frame)) == ESTALE);
	__atomic_store_n(&open_port_generation, 0, __ATOMIC_SEQ_CST);
	check_no_secret(device);

	/* A wrong key: the access point leaves, then the supplicant says it waited for message 3: EACCES. */
	reports_clear();
	chip.join_script = JOIN_WRONG_KEY;
	make_request(&request, 10, pmk);
	events = device->bus.counters.events_in;
	CHECK(ops->fullmac_connect(device, &request) == 0);
	wait_events(device, events + 4U);
	poll_until(2);
	CHECK(report_at(0).kind == REPORT_EVENT && report_at(0).value == WLAN_FULLMAC_EVENT_ASSOCIATED);
	report = report_at(1);
	CHECK(report.kind == REPORT_LOSS && report.generation == 10 && report.error == EACCES);
	poll_quiet(2, 50);
	CHECK(ops->disconnect(device, 10) == 0);

	/* No network: ETIMEDOUT.  Message 1 never came: ETIMEDOUT.  Message 3 never came: EACCES. */
	reports_clear();
	chip.join_script = JOIN_NO_NETWORK;
	make_request(&request, 11, pmk);
	CHECK(ops->fullmac_connect(device, &request) == 0);
	poll_until(1);
	CHECK(report_at(0).kind == REPORT_LOSS && report_at(0).error == ETIMEDOUT);
	CHECK(ops->disconnect(device, 11) == 0);
	reports_clear();
	chip.join_script = JOIN_M1_TIMEOUT;
	make_request(&request, 12, pmk);
	CHECK(ops->fullmac_connect(device, &request) == 0);
	poll_until(2);
	CHECK(report_at(1).kind == REPORT_LOSS && report_at(1).error == ETIMEDOUT);
	CHECK(ops->disconnect(device, 12) == 0);
	reports_clear();
	chip.join_script = JOIN_M3_TIMEOUT;
	make_request(&request, 13, pmk);
	CHECK(ops->fullmac_connect(device, &request) == 0);
	poll_until(2);
	CHECK(report_at(1).kind == REPORT_LOSS && report_at(1).error == EACCES);
	CHECK(ops->disconnect(device, 13) == 0);
	check_no_secret(device);

	/* The firmware refuses the key: EIO, the half-made settings undone, the PMK gone. */
	reports_clear();
	chip.join_script = JOIN_SUCCESS;
	chip.refuse_command = BCM43455_COMMAND_SUPPLICANT_KEY;
	make_request(&request, 14, pmk);
	i = chip.leaves;
	joins_before = chip.joins;
	CHECK(ops->fullmac_connect(device, &request) == 0);
	poll_until(1);
	CHECK(report_at(0).kind == REPORT_LOSS && report_at(0).generation == 14 && report_at(0).error == EIO);
	CHECK(chip.leaves == i + 1U && chip.joins == joins_before);
	check_no_secret(device);
	CHECK(ops->disconnect(device, 14) == 0);
	chip.refuse_command = 0;

	/*
	 * The firmware never answers the join: the key message is already gone
	 * while the join waits, and ETIMEDOUT follows the command's timeout.
	 */
	reports_clear();
	chip.ignore_command = BCM43455_COMMAND_JOIN;
	make_request(&request, 15, pmk);
	joins_before = chip.joins;
	CHECK(ops->fullmac_connect(device, &request) == 0);
	for (i = 0; i < 200; i++) {
		if (__atomic_load_n(&chip.joins, __ATOMIC_SEQ_CST) != joins_before)
			break;
		sleep_ms(2);
	}
	CHECK(__atomic_load_n(&chip.joins, __ATOMIC_SEQ_CST) == joins_before + 1U);
	CHECK(all_zero(device->work_key, sizeof(device->work_key)));
	CHECK(reports_made() == 0);
	poll_until(1);
	CHECK(report_at(0).kind == REPORT_LOSS && report_at(0).error == ETIMEDOUT);
	check_no_secret(device);
	chip.ignore_command = 0;
	CHECK(ops->disconnect(device, 15) == 0);

	/*
	 * A slow bus: connect still returns at once, and a disconnect while the
	 * commands go out stops them before the key and the join; nothing of
	 * the connection is reported.
	 */
	reports_clear();
	pthread_mutex_lock(&model_lock);
	chip.request_count = 0;
	chip.keys = 0;
	i = chip.joins;
	pthread_mutex_unlock(&model_lock);
	__atomic_store_n(&model_delay_ms, 40U, __ATOMIC_SEQ_CST);
	make_request(&request, 16, pmk);
	started_ms = sched_ticks();
	CHECK(ops->fullmac_connect(device, &request) == 0);
	CHECK(sched_ticks() - started_ms < 20U);
	sleep_ms(60);
	CHECK(ops->disconnect(device, 16) == 0);
	__atomic_store_n(&model_delay_ms, 0U, __ATOMIC_SEQ_CST);
	pthread_mutex_lock(&model_lock);
	CHECK(chip.keys == 0 && chip.joins == i);
	CHECK(chip.request_count >= 2 && chip.requests[chip.request_count - 1U].command == BCM43455_COMMAND_LEAVE);
	pthread_mutex_unlock(&model_lock);
	poll_quiet(0, 50);
	check_no_secret(device);

	/* A disconnect before the bus thread took the request only forgets it. */
	reports_clear();
	__atomic_store_n(&model_delay_ms, 40U, __ATOMIC_SEQ_CST);
	CHECK(ops->scan_channel_start(device, 20, 0, 1, 0) == 0);
	make_request(&request, 17, pmk);
	CHECK(ops->fullmac_connect(device, &request) == 0);
	i = chip.leaves;
	CHECK(ops->disconnect(device, 17) == 0);
	CHECK(device->connection.state == BCM43455_CONNECT_IDLE);
	__atomic_store_n(&model_delay_ms, 0U, __ATOMIC_SEQ_CST);
	poll_until(4);
	CHECK(chip.leaves == i);
	CHECK(ops->scan_stop(device, 20) == 0);
	check_no_secret(device);

	/* Without the firmware's supplicant a connect is refused; a closed device refuses too. */
	device->identity.supplicant = 0;
	make_request(&request, 18, pmk);
	CHECK(ops->fullmac_connect(device, &request) == EOPNOTSUPP);
	device->identity.supplicant = 1;
	request.bss.channel = 15;
	CHECK(ops->fullmac_connect(device, &request) == EINVAL);
}

/* A bus failure: the lost link, a restart, and the restarts' bound. */
static void
check_restart(
	struct bcm43455_device *device)
{
	static const uint8_t pmk[32] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
	const struct wlan_radio_ops *ops;
	struct wlan_fullmac_connect_request request;
	unsigned boots;
	unsigned round;
	unsigned i;

	ops = attached_ops;

	/* A joined connection, then the firmware halts. */
	reports_clear();
	chip.join_script = JOIN_SUCCESS;
	make_request(&request, 30, pmk);
	CHECK(ops->fullmac_connect(device, &request) == 0);
	poll_until(2);
	CHECK(report_at(1).value == WLAN_FULLMAC_EVENT_AUTHORIZED);

	/* Each halt is reported as a lost link and a restart, which brings the chip back; the fourth in a row is not restarted. */
	for (round = 0; round < 4; round++) {
		reports_clear();
		pthread_mutex_lock(&model_lock);
		chip.to_host_data = BCM43455_TO_HOST_FIRMWARE_HALT;
		model_raise_locked(BCM43455_INT_HOST_MAILBOX);
		pthread_mutex_unlock(&model_lock);
		if (round == 0) {
			poll_until(2);
			CHECK(report_at(0).kind == REPORT_LOSS && report_at(0).generation == 30 && report_at(0).error == EIO);
			CHECK(report_at(1).kind == REPORT_RESTART);
			check_no_secret(device);
		} else if (round < 3) {
			poll_until(1);
			CHECK(report_at(0).kind == REPORT_RESTART);
		} else {
			for (i = 0; i < 200; i++) {
				if (__atomic_load_n(&device->failure_pending, __ATOMIC_SEQ_CST))
					break;
				sleep_ms(5);
			}
			poll_quiet(0, 100);
			CHECK(device->restarts == BCM43455_RESTARTS_MAX);
			break;
		}

		/* The retirement thread's call: the chip is loaded again and the station reopened. */
		CHECK(bcm43455_device_ethernet_transmit(device, 30, request.pmk, 20) == ENETDOWN);
		boots = chip.boot_count;
		i = opens;
		CHECK(ops->restart(device) == 0);
		CHECK(chip.boot_count == boots + 1U && opens == i + 1U);
		CHECK(device->runtime_active == 1 && device->bus.thread_running == 1 && device->failure_pending == 0);
		CHECK(ops->restart(device) == 0 && chip.boot_count == boots + 1U);
	}

	/* A user's close and open start the count afresh. */
	fake_net.ops->close(&fake_net);
	CHECK(ops->restart(device) == 0);
	CHECK(fake_net.ops->open(&fake_net) == 0);
	CHECK(device->restarts == 0 && device->failure_pending == 0);
}

/* The radio operations against the model's firmware. */
static void
run_radio(void)
{
	struct bcm43455_device *device;

	device = radio_device();
	check_scan(device);
	check_connect(device);
	check_restart(device);

	/* Closing erases what is left and powers the chip down. */
	fake_net.ops->close(&fake_net);
	CHECK(device->runtime_active == 0 && chip.powered == 0);
	check_no_secret(device);
	CHECK(device->scan.state == BCM43455_SCAN_IDLE && device->connection.state == BCM43455_CONNECT_IDLE);
}

/* Connects and disconnects again and again. */
static void *
radio_connector(
	void *argument)
{
	static const uint8_t pmk[32] = {0x77};
	struct bcm43455_device *device;
	struct wlan_fullmac_connect_request request;
	uint64_t generation;
	unsigned tries;
	int error;

	device = argument;
	generation = 100;
	while (!__atomic_load_n(&smp_done, __ATOMIC_SEQ_CST)) {
		generation++;
		make_request(&request, generation, pmk);
		error = attached_ops->fullmac_connect(device, &request);
		CHECK(error == 0 || error == EBUSY || error == ENETDOWN);
		__atomic_store_n(&open_port_generation, generation, __ATOMIC_SEQ_CST);
		sleep_ms(3);
		for (tries = 0; tries < 20; tries++) {
			error = attached_ops->disconnect(device, generation);
			if (error == 0)
				break;
		}
		CHECK(error == 0);
	}
	return NULL;
}

/* Scans a channel and stops again and again. */
static void *
radio_scanner(
	void *argument)
{
	struct bcm43455_device *device;
	uint64_t generation;
	unsigned tries;
	int error;

	device = argument;
	generation = 1000;
	while (!__atomic_load_n(&smp_done, __ATOMIC_SEQ_CST)) {
		generation++;

		/* Every other scan is held by the firmware, so its stop aborts it. */
		pthread_mutex_lock(&model_lock);
		chip.scan_mode = SCAN_NORMAL;
		if ((generation % 2U) == 0)
			chip.scan_mode = SCAN_HOLD;
		pthread_mutex_unlock(&model_lock);
		error = attached_ops->scan_channel_start(device, generation, 0, 1 + (unsigned)(generation % 13U), 0);
		CHECK(error == 0 || error == EBUSY || error == ENETDOWN);
		sleep_ms(2);
		for (tries = 0; tries < 20; tries++) {
			error = attached_ops->scan_stop(device, generation);
			if (error == 0)
				break;
		}
		CHECK(error == 0);
	}
	return NULL;
}

/* Polls again and again, as the network worker. */
static void *
radio_poller(
	void *argument)
{
	(void)argument;
	while (!__atomic_load_n(&smp_done, __ATOMIC_SEQ_CST)) {
		(void)fake_net.ops->poll_receive(&fake_net, 16);
		sleep_ms(1);
	}
	return NULL;
}

/* Sends frames of the latest connection again and again. */
static void *
radio_transmitter(
	void *argument)
{
	struct bcm43455_device *device;
	uint8_t frame[100];

	device = argument;
	memset(frame, 0x5a, sizeof(frame));
	while (!__atomic_load_n(&smp_done, __ATOMIC_SEQ_CST)) {
		(void)attached_ops->ethernet_transmit(device, __atomic_load_n(&open_port_generation, __ATOMIC_SEQ_CST), frame, sizeof(frame));
		sleep_ms(1);
	}
	return NULL;
}

/* The radio operations raced by a connector, a scanner, a poller, a transmitter and the firmware. */
static void
run_radio_smp(void)
{
	struct bcm43455_device *device;
	pthread_t threads[5];
	unsigned i;

	device = radio_device();
	chip.join_script = JOIN_SUCCESS;
	__atomic_store_n(&smp_done, 0, __ATOMIC_SEQ_CST);
	CHECK(pthread_create(&threads[0], NULL, radio_connector, device) == 0);
	CHECK(pthread_create(&threads[1], NULL, radio_scanner, device) == 0);
	CHECK(pthread_create(&threads[2], NULL, radio_poller, NULL) == 0);
	CHECK(pthread_create(&threads[3], NULL, radio_transmitter, device) == 0);
	CHECK(pthread_create(&threads[4], NULL, smp_firmware, NULL) == 0);
	sleep_ms(1500);
	__atomic_store_n(&smp_done, 1, __ATOMIC_SEQ_CST);
	for (i = 0; i < 5; i++)
		CHECK(pthread_join(threads[i], NULL) == 0);

	/* The connections and scans came through, and closing leaves no secret. */
	printf("  radio-smp: %u joins, %u escans, %u aborts, %u leaves, %u reports, %u frames sent\n",
	    chip.joins, chip.escan_starts, chip.escan_aborts, chip.leaves, report_count, chip.data_sent_count);
	CHECK(chip.joins > 0 && chip.escan_starts > 0 && chip.leaves > 0);
	CHECK(chip.sequence_errors == 0);
	fake_net.ops->close(&fake_net);
	CHECK(device->runtime_active == 0 && chip.powered == 0);
	check_no_secret(device);
}

/* Builds the three files: a firmware with its version text, a CLM, and NVRAM text with a placeholder. */
static void
make_files(
	struct bcm43455_files *files,
	const char *mac)
{
	static const char version[] = "43455c0-roml/43455_sdio-pno-aoe-wfds-mfp-idsup-idauth-clm_min Version: 7.45.241 (x CY)";
	char nvram[256];
	unsigned i;
	int length;

	/* The firmware image the model checks at boot. */
	for (i = 0; i < BCM43455_FIRMWARE_SIZE; i++)
		firmware_image[i] = (uint8_t)(i * 13U + 1U);
	memcpy(firmware_image + 400000, version, sizeof(version));
	files->firmware = malloc(BCM43455_FIRMWARE_SIZE);
	CHECK(files->firmware != NULL);
	memcpy(files->firmware, firmware_image, BCM43455_FIRMWARE_SIZE);
	files->firmware_length = BCM43455_FIRMWARE_SIZE;

	/* A CLM container. */
	files->clm = malloc(BCM43455_CLM_SIZE);
	CHECK(files->clm != NULL);
	for (i = 0; i < BCM43455_CLM_SIZE; i++)
		files->clm[i] = (uint8_t)(i ^ 0x5aU);
	memcpy(files->clm, "BLOB", 4);
	files->clm_length = BCM43455_CLM_SIZE;

	/* The NVRAM text. */
	length = snprintf(nvram, sizeof(nvram), "# test board\nboardtype=0x6e4\nmacaddr=%s\nbtc_mode=1\n", mac);
	files->nvram = malloc((size_t)length);
	CHECK(files->nvram != NULL);
	memcpy(files->nvram, nvram, (size_t)length);
	files->nvram_length = (size_t)length;
}

/* Makes a device on the model with fresh files. */
static struct bcm43455_device *
make_device(
	const char *mac)
{
	struct bcm43455_files files;
	struct bcm43455_device *device;

	memset(&files, 0, sizeof(files));
	make_files(&files, mac);
	CHECK(bcm43455_device_create(&model_ops, NULL, &files, &device) == 0);
	CHECK(files.firmware == NULL && files.clm == NULL && files.nvram == NULL);
	return device;
}

/*
 * The model.
 */

/* Puts the model back to a chip that was never powered. */
static void
model_reset(void)
{
	uint8_t *ram;

	pthread_mutex_lock(&model_lock);
	ram = chip.ram;
	memset(&chip, 0, sizeof(chip));
	chip.ram = ram;
	memset(chip.ram, 0, RAM_SIZE);
	chip.credit = 8;
	chip.save_restore = 1;
	chip.d11_reset = 0;
	chip.cr4_reset = 1;
	model_build_erom();
	pthread_mutex_unlock(&model_lock);
}

/* Appends one enumeration ROM word. */
static void
erom_add(
	uint32_t word)
{
	CHECK(chip.erom_words < sizeof(chip.erom) / sizeof(chip.erom[0]));
	chip.erom[chip.erom_words] = word;
	chip.erom_words++;
}

/* Appends a component's two words. */
static void
erom_component(
	uint32_t designer,
	uint32_t part,
	unsigned revision,
	unsigned slave_wraps,
	unsigned master_wraps,
	unsigned master_ports)
{
	erom_add((designer << 20) | (part << 8) | (1U << 4) | 0x1U);
	erom_add(((uint32_t)revision << 24) | (slave_wraps << 19) | (master_wraps << 14) | (1U << 9) | (master_ports << 4) | 0x1U);
}

/* Appends an address descriptor: base, port, kind (0 slave, 2 slave wrap, 3 master wrap). */
static void
erom_address(
	uint32_t base,
	unsigned port,
	unsigned kind,
	unsigned size)
{
	erom_add(base | (port << 8) | (kind << 6) | (size << 4) | 0x5U);
}

/*
 * Builds an enumeration ROM with the chip's cores, a few descriptors the
 * driver must step over (master ports, a described size, a wide address,
 * a second port), and the end mark.
 */
static void
model_build_erom(void)
{
	/* The chip common core, revision 43: one slave wrapper. */
	erom_component(0x4bf, 0x800, 43, 1, 0, 0);
	erom_address(CC_BASE, 0, 0, 0);
	erom_address(CC_WRAP, 0, 2, 0);

	/*
	 * The radio core: a master port, a memory region of a described size
	 * that is not its registers, its registers, a slave wrapper that a
	 * core with master ports does not use, and its master wrapper.
	 */
	erom_component(0x4bf, 0x812, 2, 1, 1, 1);
	erom_add(0x00000103U);
	erom_address(0x18500000U, 0, 0, 3);
	erom_add(0x00010000U);
	erom_address(D11_BASE, 0, 0, 0);
	erom_address(0x18201000U, 0, 2, 0);
	erom_address(D11_WRAP, 0, 3, 0);

	/* A core the driver does not need, with a wide address. */
	erom_component(0x4bf, 0x80e, 3, 1, 0, 0);
	erom_add(0x18004000U | 0x5U | 0x8U);
	erom_add(0x00000001U);
	erom_address(0x18104000U, 0, 2, 0);

	/* The PMU, a core of its own without wrappers. */
	erom_component(0x4bf, 0x827, 24, 0, 0, 0);
	erom_address(PMU_BASE, 0, 0, 0);

	/* The SDIO device core, revision 21. */
	erom_component(0x4bf, 0x829, 21, 0, 1, 1);
	erom_add(0x00000203U);
	erom_address(SDIO_BASE, 0, 0, 0);
	erom_address(SDIO_WRAP, 0, 3, 0);

	/* The ARM CR4: a second slave port that is not its registers. */
	erom_component(0x4bf, 0x83e, 5, 0, 1, 1);
	erom_add(0x00000303U);
	erom_address(0x18300000U, 1, 0, 0);
	erom_address(CR4_BASE, 0, 0, 0);
	erom_address(CR4_WRAP, 0, 3, 0);

	/* The end mark. */
	erom_add(0x0000000fU);
}

/* Queues a frame for the host on a channel, with the firmware's credit; the caller holds model_lock. */
static void
model_push_locked(
	unsigned channel,
	const uint8_t *payload,
	size_t length)
{
	struct model_frame *frame;

	CHECK(chip.queue_count < QUEUE_MAX);
	CHECK(12U + length <= FRAME_MAX);
	frame = &chip.queue[(chip.queue_head + chip.queue_count) % QUEUE_MAX];
	memset(frame->bytes, 0, sizeof(frame->bytes));
	bcm43455_sdpcm_encode(frame->bytes, (uint16_t)(12U + length), chip.firmware_sequence, (uint8_t)channel);
	frame->bytes[9] = (uint8_t)(chip.host_sequence + chip.credit);
	memcpy(frame->bytes + 12, payload, length);
	frame->length = 12U + length;
	chip.firmware_sequence++;
	chip.queue_count++;
}

/* Sends the host an event. */
static void
model_push_event(
	uint32_t type,
	uint32_t status,
	uint32_t reason)
{
	uint8_t payload[4 + 72];

	bcm43455_bcdc_encode_data(payload, 0, 0);
	(void)build_event(payload + 4, type, status, reason, 0);
	pthread_mutex_lock(&model_lock);
	if (chip.queue_count < QUEUE_MAX - 8U) {
		model_push_locked(BCM43455_SDPCM_CHANNEL_EVENT, payload, sizeof(payload));
		model_raise_locked(BCM43455_INT_FRAME);
	}
	pthread_mutex_unlock(&model_lock);
}

/* Sends the host a data frame. */
static void
model_push_data(
	const uint8_t *frame,
	size_t length)
{
	uint8_t payload[4 + 1600];

	CHECK(length <= 1600);
	bcm43455_bcdc_encode_data(payload, 0, 0);
	memcpy(payload + 4, frame, length);
	pthread_mutex_lock(&model_lock);
	if (chip.queue_count < QUEUE_MAX - 8U) {
		model_push_locked(BCM43455_SDPCM_CHANNEL_DATA, payload, 4U + length);
		model_raise_locked(BCM43455_INT_FRAME);
	}
	pthread_mutex_unlock(&model_lock);
}

/* Sends the host an event with flags and data; the caller holds model_lock.  A nearly full queue drops it. */
static void
model_push_event_locked(
	uint32_t type,
	uint32_t status,
	uint32_t reason,
	uint16_t flags,
	const uint8_t *data,
	size_t data_length)
{
	uint8_t payload[4 + 72 + 1024];

	CHECK(data_length <= 1024);
	if (chip.queue_count >= QUEUE_MAX - 8U)
		return;
	bcm43455_bcdc_encode_data(payload, 0, 0);
	(void)build_event(payload + 4, type, status, reason, data_length);
	bcm43455_put_be16(payload + 4 + 26, flags);
	if (data_length != 0)
		memcpy(payload + 4 + 72, data, data_length);
	model_push_locked(BCM43455_SDPCM_CHANNEL_EVENT, payload, 4U + 72U + data_length);
	model_raise_locked(BCM43455_INT_FRAME);
}

/* Sends the host an event with flags. */
static void
model_push_event_with(
	uint32_t type,
	uint32_t status,
	uint32_t reason,
	uint16_t flags)
{
	pthread_mutex_lock(&model_lock);
	model_push_event_locked(type, status, reason, flags, NULL, 0);
	pthread_mutex_unlock(&model_lock);
}

/*
 * Writes the firmware's description of a network: a WPA2-PSK CCMP access
 * point named ssid with the BSSID 02:00:00:00:00:last on chanspec, whose
 * elements repeat the SSID and the channel the driver makes itself.
 */
static size_t
model_bss(
	uint8_t *bss,
	const char *ssid,
	uint8_t last,
	uint16_t chanspec)
{
	static const uint8_t rsn[22] = {
		0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f,
		0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
	};
	size_t ssid_length;
	size_t elements;
	size_t length;

	ssid_length = strlen(ssid);
	memset(bss, 0, 256);
	bcm43455_put_le32(bss, 109);
	bss[8] = 0x02;
	bss[13] = last;
	bcm43455_put_le16(bss + 14, 100);
	bcm43455_put_le16(bss + 16, 0x0411);
	bss[18] = (uint8_t)ssid_length;
	memcpy(bss + 19, ssid, ssid_length);
	bcm43455_put_le16(bss + 72, chanspec);
	bcm43455_put_le16(bss + 78, (uint16_t)(int16_t)-42);
	bss[88] = 0;

	/* The elements: the SSID and the channel again, then the RSN element. */
	elements = 128;
	bss[elements++] = 0;
	bss[elements++] = (uint8_t)ssid_length;
	memcpy(bss + elements, ssid, ssid_length);
	elements += ssid_length;
	bss[elements++] = 3;
	bss[elements++] = 1;
	bss[elements++] = (uint8_t)(chanspec & 0xffU);
	memcpy(bss + elements, rsn, sizeof(rsn));
	elements += sizeof(rsn);
	bcm43455_put_le16(bss + 116, 128);
	bcm43455_put_le32(bss + 120, (uint32_t)(elements - 128U));
	length = (elements + 3U) & ~(size_t)3U;
	bcm43455_put_le32(bss + 4, (uint32_t)length);
	return length;
}

/* Reports an escan's two networks, then its end; the caller holds model_lock. */
static void
model_scan_events_locked(
	uint16_t sync_id,
	uint16_t chanspec)
{
	uint8_t data[1024];
	size_t length;

	memset(data, 0, sizeof(data));
	length = 12;
	length += model_bss(data + length, "zed-alpha", 0x0a, chanspec);
	length += model_bss(data + length, "zed-beta", 0x0b, chanspec);
	bcm43455_put_le32(data, (uint32_t)length);
	bcm43455_put_le32(data + 4, 109);
	bcm43455_put_le16(data + 8, sync_id);
	bcm43455_put_le16(data + 10, 2);
	model_push_event_locked(BCM43455_EVENT_SCAN_RESULT, 8, 0, 0, data, length);

	memset(data, 0, 12);
	bcm43455_put_le32(data, 12);
	bcm43455_put_le16(data + 8, sync_id);
	model_push_event_locked(BCM43455_EVENT_SCAN_RESULT, 0, 0, 0, data, 12);
}

/* Reports what the firmware does after a join command; the caller holds model_lock. */
static void
model_join_events_locked(void)
{
	/* Tells the scripts apart. */
	switch (chip.join_script) {
	case JOIN_SUCCESS:
		model_push_event_locked(BCM43455_EVENT_AUTHENTICATION, 0, 0, 0, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_ASSOCIATION, 0, 0, 0, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_LINK, 0, 0, 1, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_JOIN_RESULT, 0, 0, 0, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_SUPPLICANT, 6, 0, 0, NULL, 0);
		break;

	case JOIN_WRONG_KEY:
		model_push_event_locked(BCM43455_EVENT_LINK, 0, 0, 1, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_JOIN_RESULT, 0, 0, 0, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_PEER_DEAUTHENTICATED, 0, 2, 0, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_SUPPLICANT, 8, 14, 0, NULL, 0);
		break;

	case JOIN_NO_NETWORK:
		model_push_event_locked(BCM43455_EVENT_JOIN_RESULT, 3, 0, 0, NULL, 0);
		break;

	case JOIN_M1_TIMEOUT:
		model_push_event_locked(BCM43455_EVENT_LINK, 0, 0, 1, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_JOIN_RESULT, 0, 0, 0, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_SUPPLICANT, 4, 15, 0, NULL, 0);
		break;

	case JOIN_M3_TIMEOUT:
		model_push_event_locked(BCM43455_EVENT_LINK, 0, 0, 1, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_JOIN_RESULT, 0, 0, 0, NULL, 0);
		model_push_event_locked(BCM43455_EVENT_SUPPLICANT, 8, 15, 0, NULL, 0);
		break;

	default:
		break;
	}
}

/* Raises SDIO core interrupt bits and, when the host has it armed, the card's interrupt. */
static void
model_raise_locked(
	uint32_t bits)
{
	chip.int_status |= bits;
	if (chip.irq_enabled && chip.armed && (chip.ien & 0x01U) != 0) {
		chip.armed = 0;
		chip.pending = 1;
		pthread_cond_broadcast(&model_wakeup);
	}
}

/* The firmware boots once the ARM core runs: it checks what it was given and says it is ready. */
static void
model_boot_locked(void)
{
	uint32_t token;
	uint32_t words;

	chip.boot_count++;
	chip.booted = 1;
	token = 0;
	memcpy(&token, chip.ram + RAM_SIZE - 4U, 4);
	words = token & 0xffffU;
	chip.boot_image_ok = 0;
	if (memcmp(chip.ram, firmware_image, BCM43455_FIRMWARE_SIZE) == 0 &&
	    chip.vector == bcm43455_get_le32(firmware_image))
		chip.boot_image_ok = 1;
	(void)words;
	chip.to_host_data = BCM43455_TO_HOST_FIRMWARE_READY | (4U << 16);
	model_raise_locked(BCM43455_INT_HOST_MAILBOX);
}

/* Reads a 32-bit backplane register or RAM word. */
static uint32_t
model_read32_locked(
	uint32_t address)
{
	uint32_t value;

	/* RAM. */
	if (address >= RAM_BASE && address + 4U <= RAM_BASE + RAM_SIZE) {
		memcpy(&value, chip.ram + (address - RAM_BASE), 4);
		return value;
	}

	/* The enumeration ROM. */
	if (address >= EROM_BASE && address < EROM_BASE + 4U * chip.erom_words)
		return chip.erom[(address - EROM_BASE) / 4U];
	if (address >= EROM_BASE && address < EROM_BASE + 0x1000U)
		return 0x0000000fU;

	switch (address) {
	case CC_BASE + 0x00:
		return 0x4345U | (6U << 16) | (1U << 28);
	case CC_BASE + 0x04:
		return 0x58800000U;
	case CC_BASE + 0xac:
		return 0x00000041U;
	case CC_BASE + 0xfc:
		return EROM_BASE;
	case CR4_BASE + 0x04:
		return 3U | (1U << 4);
	case CR4_BASE + 0x40:
		return chip.cr4_bank;
	case CR4_BASE + 0x44:
		if (chip.cr4_bank < 3U)
			return 0x1fU;
		return 0x03U;
	case D11_WRAP + 0x408:
		return chip.d11_io;
	case D11_WRAP + 0x800:
		return chip.d11_reset;
	case CR4_WRAP + 0x408:
		return chip.cr4_io;
	case CR4_WRAP + 0x800:
		return chip.cr4_reset;
	case PMU_BASE + 0x600:
		return chip.pmu_control;
	case PMU_BASE + 0x604:
		return 24U;
	case PMU_BASE + 0x654:
		if (chip.chip_control_address == 3U && chip.save_restore)
			return 0x04U;
		return 0;
	case SDIO_BASE + 0x20:
		return chip.int_status;
	case SDIO_BASE + 0x24:
		return chip.host_int_mask;
	case SDIO_BASE + 0x4c:
		return chip.to_host_data;
	default:
		return 0;
	}
}

/* Writes a 32-bit backplane register or RAM word. */
static void
model_write32_locked(
	uint32_t address,
	uint32_t value)
{
	/* RAM, which must not change while the ARM core runs. */
	if (address >= RAM_BASE && address + 4U <= RAM_BASE + RAM_SIZE) {
		if (chip.booted)
			chip.ram_written_running++;
		memcpy(chip.ram + (address - RAM_BASE), &value, 4);
		return;
	}

	switch (address) {
	case 0:
		chip.vector = value;
		break;
	case CR4_BASE + 0x40:
		chip.cr4_bank = value;
		break;
	case PMU_BASE + 0x600:
		chip.pmu_control = value;
		break;
	case PMU_BASE + 0x650:
		chip.chip_control_address = value;
		break;
	case D11_WRAP + 0x408:
		chip.d11_io = value;
		if (chip.d11_io_count < 16U)
			chip.d11_io_history[chip.d11_io_count++] = value;
		break;
	case D11_WRAP + 0x800:
		chip.d11_reset = value;
		break;
	case CR4_WRAP + 0x408:
		chip.cr4_io = value;
		/* The core runs once out of reset with its clock on and neither the forced gate nor the halt. */
		if (chip.cr4_reset == 0 && value == BCM43455_IO_CONTROL_CLOCK && !chip.booted)
			model_boot_locked();
		if ((value & BCM43455_CR4_IO_CPU_HALT) != 0)
			chip.booted = 0;
		break;
	case CR4_WRAP + 0x800:
		chip.cr4_reset = value;
		break;
	case SDIO_BASE + 0x20:
		chip.int_status &= ~value;
		break;
	case SDIO_BASE + 0x24:
		chip.host_int_mask = value;
		break;
	case SDIO_BASE + 0x40:
		chip.to_chip_mailbox = value;
		if ((value & BCM43455_TO_CHIP_NAK) != 0)
			chip.naks++;
		break;
	case SDIO_BASE + 0x48:
		chip.to_chip_data = value;
		break;
	default:
		break;
	}
}

/* Answers one control message from the host. */
static void
model_control(
	const uint8_t *message,
	size_t length)
{
	struct model_request *request;
	uint8_t answer[16 + 2048];
	uint32_t command;
	uint32_t request_length;
	uint32_t flags;
	const uint8_t *payload;
	const char *name;
	size_t name_length;
	size_t value_offset;
	int32_t status;
	unsigned set;

	CHECK(length >= 16);
	command = bcm43455_get_le32(message);
	request_length = bcm43455_get_le32(message + 4);
	flags = bcm43455_get_le32(message + 8);
	set = (flags & 2U) != 0;
	payload = message + 16;
	CHECK(16U + request_length <= length);

	/* Records the request; past 64 the log wraps (the smp scenario sends thousands). */
	request = &chip.requests[chip.request_count % 64U];
	chip.request_count++;
	memset(request, 0, sizeof(*request));
	request->command = command;
	request->set = set;
	name = "";
	value_offset = 0;
	if (command == BCM43455_COMMAND_READ_VARIABLE || command == BCM43455_COMMAND_WRITE_VARIABLE) {
		name = (const char *)payload;
		name_length = strnlen(name, request_length);
		CHECK(name_length < request_length && name_length < sizeof(request->name));
		memcpy(request->name, name, name_length);
		value_offset = name_length + 1U;
	} else if (command == BCM43455_COMMAND_RADIO_UP) {
		strcpy(request->name, "up");
	} else if (command == BCM43455_COMMAND_RADIO_DOWN) {
		strcpy(request->name, "down");
	} else if (command == BCM43455_COMMAND_LEAVE) {
		strcpy(request->name, "leave");
		chip.leaves++;
	}
	request->length = request_length - value_offset;
	memcpy(request->payload, payload + value_offset, request->length);
	if (command == BCM43455_COMMAND_JOIN) {
		CHECK(request->length == BCM43455_JOIN_REQUEST_LENGTH);
		memcpy(chip.join_seen, request->payload, BCM43455_JOIN_REQUEST_LENGTH);
		chip.joins++;
	}

	/* A silent firmware, or one that ignores this command, answers nothing. */
	if (chip.silent)
		return;
	if (chip.ignore_command != 0 && command == chip.ignore_command)
		return;

	/* Builds the answer: the same header with the result, and the payload. */
	memset(answer, 0, sizeof(answer));
	memcpy(answer, message, 16);
	status = 0;
	if (command == BCM43455_COMMAND_READ_VARIABLE) {
		if (strcmp(request->name, "cur_etheraddr") == 0) {
			memcpy(answer + 16, chip_mac, 6);
		} else if (strcmp(request->name, "ver") == 0) {
			strcpy((char *)answer + 16, "wl0: Nov  1 2021 00:40:07 version 7.45.241 (1a2f2fa CY) FWID 01-5209c09");
		} else if (strcmp(request->name, "cap") == 0) {
			strcpy((char *)answer + 16, "ap sta wme 802.11d 802.11h rm cqa cac dualband ampdu ampdu_tx ampdu_rx amsdurx tdls radio_pwrsave p2p proptxstatus mchan wds dwds p2po anqpo vht-prop-rates dfrts txpwrcache stbc-tx stbc-rx-1ss epno pfnx wnm bsstrans mfp ndoe rssi_mon cptlv-4000 idsup idauth");
		} else if (strcmp(request->name, "clmload_status") == 0) {
			bcm43455_put_le32(answer + 16, chip.clm_received == BCM43455_CLM_SIZE ? 0 : 1);
		} else if (strcmp(request->name, "country") == 0) {
			memcpy(answer + 16, chip.country, 12);
		} else if (strcmp(request->name, "test_secret") == 0) {
			memcpy(answer + 16, chip.secret_seen, 32);
		} else {
			status = -23;
		}
	} else if (command == BCM43455_COMMAND_WRITE_VARIABLE) {
		if (strcmp(request->name, "escan") == 0) {
			uint16_t action;

			action = bcm43455_get_le16(request->payload + 4);
			if (action == BCM43455_ESCAN_START) {
				chip.escan_starts++;
				chip.escan_sync = bcm43455_get_le16(request->payload + 6);
				chip.escan_scan_type = request->payload[8 + 43];
				chip.escan_chanspec = bcm43455_get_le16(request->payload + 8 + 64);
				if (chip.scan_mode == SCAN_REFUSE)
					status = -1;
			} else {
				CHECK(action == BCM43455_ESCAN_ABORT);
				chip.escan_aborts++;
			}
		} else if (strcmp(request->name, "clmload") == 0) {
			uint16_t clm_flags;
			uint32_t clm_length;

			clm_flags = bcm43455_get_le16(request->payload);
			clm_length = bcm43455_get_le32(request->payload + 4);
			CHECK(bcm43455_get_le16(request->payload + 2) == 2);
			if (chip.clm_messages == 0 && clm_flags == 0x1002)
				chip.clm_flags_ok = 1;
			else if (chip.clm_messages == 0)
				chip.clm_flags_ok = 0;
			memcpy(chip.clm + chip.clm_received, request->payload + 12, clm_length);
			chip.clm_received += clm_length;
			chip.clm_messages++;
			if (chip.clm_received == BCM43455_CLM_SIZE && clm_flags != 0x1004)
				chip.clm_flags_ok = 0;
		} else if (strcmp(request->name, "country") == 0) {
			if (chip.refuse_country == 1)
				status = -1;
			else if (chip.refuse_country == 2 && bcm43455_get_le32(request->payload + 4) == 0)
				status = -1;
			else
				memcpy(chip.country, request->payload, 12);
		} else if (strcmp(request->name, "test_secret") == 0) {
			memcpy(chip.secret_seen, request->payload, 32);
		}
	} else if (command == BCM43455_COMMAND_CHANNEL_LIST) {
		static const uint32_t list[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
		unsigned i;

		bcm43455_put_le32(answer + 16, 13);
		for (i = 0; i < 13; i++)
			bcm43455_put_le32(answer + 20 + 4 * i, list[i]);
	} else if (command == BCM43455_COMMAND_RADIO_DOWN && chip.halt_on_down) {
		status = -1;
	} else if (command == BCM43455_COMMAND_IO_VERSION) {
		bcm43455_put_le32(answer + 16, BCM43455_CHANSPEC_LAYOUT_AC);
	} else if (command == BCM43455_COMMAND_SUPPLICANT_KEY) {
		CHECK(request->length == BCM43455_KEY_MESSAGE_LENGTH);
		memcpy(chip.key_seen, request->payload, BCM43455_KEY_MESSAGE_LENGTH);
		chip.keys++;
	}

	/* A command the test makes the firmware refuse. */
	if (chip.refuse_command != 0 && command == chip.refuse_command)
		status = -1;

	/* A firmware that reports an escan's results before it answers the request. */
	if (command == BCM43455_COMMAND_WRITE_VARIABLE &&
	    strcmp(request->name, "escan") == 0 &&
	    status == 0 &&
	    chip.scan_mode == SCAN_RESULTS_FIRST &&
	    bcm43455_get_le16(request->payload + 4) == BCM43455_ESCAN_START)
		model_scan_events_locked(chip.escan_sync, chip.escan_chanspec);

	/* The answer's flags keep the id and the set bit, plus the error flag. */
	if (status != 0) {
		bcm43455_put_le32(answer + 8, flags | 1U);
		bcm43455_put_le32(answer + 12, (uint32_t)status);
	}
	/* A firmware that reports an escan's results first holds its answer until the test lets it go. */
	if (command == BCM43455_COMMAND_WRITE_VARIABLE &&
	    strcmp(request->name, "escan") == 0 &&
	    status == 0 &&
	    chip.scan_mode == SCAN_RESULTS_FIRST &&
	    bcm43455_get_le16(request->payload + 4) == BCM43455_ESCAN_START) {
		memcpy(chip.held_answer, answer, 16U + request_length);
		chip.held_length = 16U + request_length;
		return;
	}
	model_push_locked(BCM43455_SDPCM_CHANNEL_CONTROL, answer, 16U + request_length);
	model_raise_locked(BCM43455_INT_FRAME);

	/* What the firmware does after it answered: scan the channel, or join. */
	if (status == 0 &&
	    command == BCM43455_COMMAND_WRITE_VARIABLE &&
	    strcmp(request->name, "escan") == 0 &&
	    bcm43455_get_le16(request->payload + 4) == BCM43455_ESCAN_START) {
		if (chip.scan_mode == SCAN_NORMAL)
			model_scan_events_locked(chip.escan_sync, chip.escan_chanspec);
		else if (chip.scan_mode == SCAN_STALE)
			model_scan_events_locked((uint16_t)(chip.escan_sync + 100U), chip.escan_chanspec);
		else if (chip.scan_mode == SCAN_BUSY)
			model_push_event_locked(BCM43455_EVENT_SCAN_RESULT, 9, 0, 0, NULL, 0);
	}
	if (status == 0 && command == BCM43455_COMMAND_JOIN)
		model_join_events_locked();
}

/* Takes one frame the host wrote to function 2. */
static void
model_frame_from_host(
	const uint8_t *bytes,
	size_t length)
{
	struct bcm43455_sdpcm_fields header;
	struct bcm43455_bcdc_data data;

	CHECK(bcm43455_sdpcm_decode(bytes, length, &header) == 0);
	CHECK(header.length <= length);
	CHECK(header.payload_start == 12);

	/* The host's sequence goes up by one, and it never sends beyond the credit. */
	if (header.sequence != chip.host_sequence)
		chip.sequence_errors++;
	chip.host_sequence = (uint8_t)(header.sequence + 1U);

	if (header.channel == BCM43455_SDPCM_CHANNEL_CONTROL) {
		model_control(bytes + 12, (size_t)header.length - 12U);
	} else if (header.channel == BCM43455_SDPCM_CHANNEL_DATA) {
		CHECK(bcm43455_bcdc_decode_data(bytes + 12, (size_t)header.length - 12U, &data) == 0);
		memcpy(chip.data_sent, bytes + 12 + data.frame_offset, data.frame_length);
		chip.data_sent_length = data.frame_length;
		chip.data_sent_count++;
	} else {
		CHECK(0);
	}
}

/* CMD52 read. */
static int
op_read_byte(
	void *context,
	unsigned function,
	uint32_t address,
	uint8_t *value)
{
	(void)context;
	pthread_mutex_lock(&model_lock);
	CHECK(chip.powered);
	*value = 0;
	if (function == 0) {
		if (address == 0x00)
			*value = 0x43;
		else if (address == 0x04)
			*value = chip.ien;
		else if (address == BCM43455_CCCR_CARD_CONTROL)
			*value = chip.card_control;
	} else if (function == 1) {
		if (address == BCM43455_F1_CHIP_CLOCK) {
			*value = (uint8_t)(chip.clock & 0x3fU);
			if ((chip.clock & (BCM43455_CLOCK_ALP_REQUEST | BCM43455_CLOCK_FORCE_ALP)) != 0)
				*value |= BCM43455_CLOCK_ALP_AVAILABLE;
			/* The fast clock needs the PMU resources the running firmware sets up (BUG-311). */
			if (chip.booted && (chip.clock & (BCM43455_CLOCK_HT_REQUEST | BCM43455_CLOCK_FORCE_HT)) != 0)
				*value |= BCM43455_CLOCK_HT_AVAILABLE;
		} else if (address == BCM43455_F1_SLEEP_CONTROL) {
			*value = chip.sleep_control;
		} else if (address == BCM43455_F1_WAKEUP_CONTROL) {
			*value = chip.wakeup_control;
		}
	}
	pthread_mutex_unlock(&model_lock);
	return 0;
}

/* CMD52 write. */
static int
op_write_byte(
	void *context,
	unsigned function,
	uint32_t address,
	uint8_t value)
{
	(void)context;
	pthread_mutex_lock(&model_lock);
	CHECK(chip.powered);
	if (function == 0 && address == 0x04) {
		chip.ien = value;
	} else if (function == 0 && address == BCM43455_CCCR_IO_ABORT) {
		CHECK(value == 2);
		chip.io_aborts++;
	} else if (function == 0 && address == BCM43455_CCCR_CARD_CAPABILITY) {
		chip.card_capability = value;
	} else if (function == 0 && address == BCM43455_CCCR_CARD_CONTROL) {
		chip.card_control = value;
	} else if (function == 1) {
		switch (address) {
		case BCM43455_F1_WINDOW_LOW:
			chip.window[0] = value;
			break;
		case BCM43455_F1_WINDOW_MID:
			chip.window[1] = value;
			break;
		case BCM43455_F1_WINDOW_HIGH:
			chip.window[2] = value;
			break;
		case BCM43455_F1_CHIP_CLOCK:
			chip.clock = value;
			if (!chip.booted && (value & (BCM43455_CLOCK_HT_REQUEST | BCM43455_CLOCK_FORCE_HT)) != 0)
				chip.fast_requests_before_boot++;
			break;
		case BCM43455_F1_WATERMARK:
			chip.watermark = value;
			break;
		case BCM43455_F1_SLEEP_CONTROL:
			chip.sleep_control = value;
			break;
		case BCM43455_F1_WAKEUP_CONTROL:
			chip.wakeup_control = value;
			break;
		case BCM43455_F1_FRAME_CONTROL:
			/* Terminating drops the frame being read, if one was started. */
			if ((value & BCM43455_FRAME_CONTROL_READ_TERMINATE) != 0) {
				chip.read_terminates++;
				if (chip.queue_count != 0 && chip.read_position != 0) {
					chip.queue_head = (chip.queue_head + 1U) % QUEUE_MAX;
					chip.queue_count--;
					chip.read_position = 0;
				}
			}
			break;
		default:
			break;
		}
	}
	pthread_mutex_unlock(&model_lock);
	return 0;
}

/* The backplane address a function 1 access reaches. */
static uint32_t
backplane_address(
	uint32_t address)
{
	uint32_t window;

	CHECK((address & 0x8000U) != 0);
	window = ((uint32_t)chip.window[0] << 8) | ((uint32_t)chip.window[1] << 16) | ((uint32_t)chip.window[2] << 24);
	return window | (address & 0x7fffU);
}

/* CMD53 read: the backplane, or the next frame on function 2. */
static int
op_read(
	void *context,
	unsigned function,
	uint32_t address,
	int increment,
	uint8_t *buffer,
	size_t length)
{
	struct model_frame *frame;
	uint32_t base;
	uint32_t value;
	size_t i;

	(void)context;
	pthread_mutex_lock(&model_lock);
	CHECK(chip.powered);
	CHECK(length != 0);
	if (length > op_block_size(NULL, function))
		CHECK((length % op_block_size(NULL, function)) == 0);
	if (function == 1) {
		CHECK(increment == 1);
		base = backplane_address(address);
		CHECK((base & 3U) == 0 && (length % 4U) == 0);
		CHECK((base & 0x7fffU) + length <= 0x8000U);
		for (i = 0; i < length; i += 4) {
			value = model_read32_locked(base + (uint32_t)i);
			memcpy(buffer + i, &value, 4);
		}
	} else {
		CHECK(function == 2 && address == BCM43455_FRAME_ADDRESS && increment == 0 && chip.f2_enabled);
		CHECK((length % 4U) == 0);
		memset(buffer, 0, length);
		if (chip.queue_count != 0) {
			frame = &chip.queue[chip.queue_head];
			for (i = 0; i < length && chip.read_position + i < frame->length; i++)
				buffer[i] = frame->bytes[chip.read_position + i];
			chip.read_position += length;
			if (chip.read_position >= frame->length) {
				chip.queue_head = (chip.queue_head + 1U) % QUEUE_MAX;
				chip.queue_count--;
				chip.read_position = 0;
			}
		}
	}
	pthread_mutex_unlock(&model_lock);
	return 0;
}

/* CMD53 write: the backplane, or a frame on function 2. */
static int
op_write(
	void *context,
	unsigned function,
	uint32_t address,
	int increment,
	uint8_t *buffer,
	size_t length)
{
	uint32_t base;
	uint32_t value;
	size_t i;

	(void)context;
	if (function == 2 && __atomic_load_n(&model_delay_ms, __ATOMIC_SEQ_CST) != 0)
		sleep_ms(__atomic_load_n(&model_delay_ms, __ATOMIC_SEQ_CST));
	pthread_mutex_lock(&model_lock);
	CHECK(chip.powered);
	CHECK(length != 0);
	if (length > op_block_size(NULL, function))
		CHECK((length % op_block_size(NULL, function)) == 0);
	if (function == 1) {
		CHECK(increment == 1);
		base = backplane_address(address);
		CHECK((base & 3U) == 0 && (length % 4U) == 0);
		CHECK((base & 0x7fffU) + length <= 0x8000U);
		for (i = 0; i < length; i += 4) {
			memcpy(&value, buffer + i, 4);
			model_write32_locked(base + (uint32_t)i, value);
		}
	} else {
		CHECK(function == 2 && address == BCM43455_FRAME_ADDRESS && increment == 0 && chip.f2_enabled);
		CHECK((length % 4U) == 0);
		model_frame_from_host(buffer, length);
	}
	pthread_mutex_unlock(&model_lock);
	return 0;
}

/* The block sizes power_up set. */
static unsigned
op_block_size(
	void *context,
	unsigned function)
{
	(void)context;
	if (function == 1)
		return 64;
	if (function == 2)
		return 512;
	return 0;
}

/* Function 2 comes ready once the firmware runs on the fast clock. */
static int
op_enable_function(
	void *context,
	unsigned function,
	unsigned timeout_ms)
{
	int error;

	(void)context;
	(void)timeout_ms;
	CHECK(function == 2);
	pthread_mutex_lock(&model_lock);
	error = ETIMEDOUT;
	if (chip.booted && (chip.clock & (BCM43455_CLOCK_HT_REQUEST | BCM43455_CLOCK_FORCE_HT)) != 0) {
		chip.f2_enabled = 1;
		error = 0;
	}
	pthread_mutex_unlock(&model_lock);
	return error;
}

/* The frame function is disabled at attach, until the firmware runs. */
static int
op_disable_function(
	void *context,
	unsigned function)
{
	(void)context;
	CHECK(function == 2);
	pthread_mutex_lock(&model_lock);
	chip.f2_disables++;
	chip.f2_enabled = 0;
	pthread_mutex_unlock(&model_lock);
	return 0;
}

/* Sleeps for real, a little. */
static int
op_sleep(
	void *context,
	unsigned milliseconds)
{
	(void)context;
	sleep_ms(milliseconds);
	return 0;
}

/* Waits for the card's interrupt, a kick or the deadline. */
static int
op_interrupt_wait(
	void *context,
	uint64_t deadline)
{
	struct timespec until;
	uint64_t nanoseconds;
	int error;

	(void)context;
	pthread_mutex_lock(&model_lock);
	error = ETIMEDOUT;
	for (;;) {
		if (chip.pending) {
			chip.pending = 0;
			error = 0;
			break;
		}
		if (chip.kicked) {
			chip.kicked = 0;
			error = EAGAIN;
			break;
		}
		if (sched_ticks() >= deadline)
			break;
		nanoseconds = (uint64_t)started.tv_nsec + deadline * 1000000ULL;
		until.tv_sec = started.tv_sec + (time_t)(nanoseconds / 1000000000ULL);
		until.tv_nsec = (long)(nanoseconds % 1000000000ULL);
		pthread_cond_timedwait(&model_wakeup, &model_lock, &until);
	}
	pthread_mutex_unlock(&model_lock);
	return error;
}

/* Lets the card's interrupt in, or keeps it away. */
static void
op_interrupt_enable(
	void *context,
	int enable)
{
	(void)context;
	pthread_mutex_lock(&model_lock);
	chip.irq_enabled = enable ? 1U : 0U;
	chip.armed = chip.irq_enabled;
	chip.pending = 0;
	pthread_mutex_unlock(&model_lock);
}

/* Arms the card's interrupt again; one raised meanwhile fires at once. */
static void
op_interrupt_rearm(
	void *context)
{
	(void)context;
	pthread_mutex_lock(&model_lock);
	if (chip.irq_enabled) {
		chip.armed = 1;
		if (chip.int_status != 0)
			model_raise_locked(0);
	}
	pthread_mutex_unlock(&model_lock);
}

/* Ends a wait without an interrupt. */
static void
op_interrupt_kick(
	void *context)
{
	(void)context;
	pthread_mutex_lock(&model_lock);
	chip.kicked = 1;
	pthread_cond_broadcast(&model_wakeup);
	pthread_mutex_unlock(&model_lock);
}

/* Powers the chip from reset: the firmware is gone, F1 enabled, nothing queued. */
static int
op_power_up(
	void *context)
{
	(void)context;
	pthread_mutex_lock(&model_lock);
	chip.powered = 1;
	chip.power_ups++;
	chip.booted = 0;
	chip.f2_enabled = 0;
	chip.clock = 0;
	chip.ien = 0;
	chip.queue_count = 0;
	chip.read_position = 0;
	chip.int_status = 0;
	chip.host_sequence = 0xff;
	chip.firmware_sequence = 0;
	chip.sleep_control = 0;
	chip.wakeup_control = 0;
	chip.card_control = 0;
	chip.d11_reset = 0;
	chip.cr4_reset = 1;
	chip.cr4_io = 0;
	chip.clm_received = 0;
	chip.clm_messages = 0;
	memset(chip.ram, 0, RAM_SIZE);
	pthread_mutex_unlock(&model_lock);
	return 0;
}

/* Cuts the chip's power. */
static void
op_power_down(
	void *context)
{
	(void)context;
	pthread_mutex_lock(&model_lock);
	CHECK(!chip.irq_enabled);
	chip.powered = 0;
	chip.power_downs++;
	chip.booted = 0;
	pthread_mutex_unlock(&model_lock);
}

/* Reports whether every byte is zero. */
static int
all_zero(
	const void *bytes,
	size_t length)
{
	const uint8_t *byte;
	size_t i;

	byte = bytes;
	for (i = 0; i < length; i++) {
		if (byte[i] != 0)
			return 0;
	}
	return 1;
}

/* Counts the recorded requests of a name. */
static unsigned
count_requests(
	const char *name)
{
	unsigned count;
	unsigned i;

	count = 0;
	pthread_mutex_lock(&model_lock);
	CHECK(chip.request_count <= 64U);
	for (i = 0; i < chip.request_count; i++) {
		if (strcmp(chip.requests[i].name, name) == 0)
			count++;
	}
	pthread_mutex_unlock(&model_lock);
	return count;
}

/* Finds the first recorded set of a name. */
static const struct model_request *
find_request(
	const char *name)
{
	unsigned i;

	CHECK(chip.request_count <= 64U);
	for (i = 0; i < chip.request_count; i++) {
		if (strcmp(chip.requests[i].name, name) == 0 && chip.requests[i].set)
			return &chip.requests[i];
	}
	return NULL;
}

/* Sleeps for real. */
static void
sleep_ms(
	unsigned milliseconds)
{
	struct timespec pause;

	pause.tv_sec = milliseconds / 1000U;
	pause.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	nanosleep(&pause, NULL);
}

/* Reads a whole file. */
static unsigned char *
read_whole(
	const char *path,
	size_t *size)
{
	FILE *file;
	unsigned char *bytes;
	long length;

	file = fopen(path, "rb");
	CHECK(file != NULL);
	CHECK(fseek(file, 0, SEEK_END) == 0);
	length = ftell(file);
	CHECK(length > 0);
	CHECK(fseek(file, 0, SEEK_SET) == 0);
	bytes = malloc((size_t)length);
	CHECK(bytes != NULL);
	CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
	fclose(file);
	*size = (size_t)length;
	return bytes;
}

/*
 * The kernel services the driver calls.
 */

/* Finds, or makes, the host object of a kernel lock or queue. */
static struct host_object *
find_object(
	const void *key)
{
	unsigned i;

	pthread_mutex_lock(&host_object_lock);
	for (i = 0; i < host_object_count; i++) {
		if (host_objects[i].key == key) {
			pthread_mutex_unlock(&host_object_lock);
			return &host_objects[i];
		}
	}
	CHECK(host_object_count < sizeof(host_objects) / sizeof(host_objects[0]));
	host_objects[host_object_count].key = key;
	pthread_mutex_init(&host_objects[host_object_count].mutex, NULL);
	pthread_cond_init(&host_objects[host_object_count].cond, NULL);
	host_object_count++;
	pthread_mutex_unlock(&host_object_lock);
	return &host_objects[host_object_count - 1U];
}

/* Prints the driver's log lines. */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("  log: ");
	vprintf(format, arguments);
	va_end(arguments);
}

void *
kern_malloc(
	size_t size)
{
	return malloc(size);
}

void *
kern_calloc(
	size_t count,
	size_t size)
{
	return calloc(count, size);
}

void
kern_free(
	void *pointer)
{
	free(pointer);
}

/* Every spinlock of the driver is of the device rank. */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	CHECK(rank == LOCK_RANK_DEVICE && name != NULL);
	lock->rank = rank;
	lock->name = name;
	(void)find_object(lock);
}

/* Takes a spinlock; the driver's spinlocks are leaves, so none is held already. */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	CHECK(spin_depth == 0);
	pthread_mutex_lock(&find_object(lock)->mutex);
	spin_depth++;
	return 1UL;
}

void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	CHECK(enabled == 1UL && spin_depth == 1);
	spin_depth--;
	pthread_mutex_unlock(&find_object(lock)->mutex);
}

int
mutex_init(
	struct mutex *mutex,
	enum lock_rank rank,
	const char *name)
{
	CHECK(rank == LOCK_RANK_DEVICE && name != NULL);
	(void)find_object(mutex);
	return 0;
}

/* Takes a mutex: never under a spinlock. */
void
mutex_lock(
	struct mutex *mutex)
{
	CHECK(spin_depth == 0);
	pthread_mutex_lock(&find_object(mutex)->mutex);
}

void
mutex_unlock(
	struct mutex *mutex)
{
	pthread_mutex_unlock(&find_object(mutex)->mutex);
}

void
waitq_init(
	struct wait_queue *queue,
	const char *name)
{
	queue->head = NULL;
	queue->tail = NULL;
	queue->sequence = 1;
	queue->name = name;
	(void)find_object(queue);
}

uint64_t
waitq_sequence(
	const struct wait_queue *queue)
{
	return __atomic_load_n(&queue->sequence, __ATOMIC_SEQ_CST);
}

/* Sleeps on the queue's condition with the condition lock's mutex, the one spinlock held. */
int
waitq_sleep(
	struct wait_queue *queue,
	struct spinlock *condition_lock,
	uint64_t observed,
	uint64_t deadline,
	unsigned flags)
{
	struct timespec until;
	uint64_t nanoseconds;
	struct host_object *lock;
	struct host_object *wait;

	CHECK(flags == 0 && spin_depth == 1);
	if (waitq_sequence(queue) != observed)
		return EAGAIN;
	lock = find_object(condition_lock);
	wait = find_object(queue);
	if (deadline == 0) {
		pthread_cond_wait(&wait->cond, &lock->mutex);
	} else {
		nanoseconds = (uint64_t)started.tv_nsec + deadline * 1000000ULL;
		until.tv_sec = started.tv_sec + (time_t)(nanoseconds / 1000000000ULL);
		until.tv_nsec = (long)(nanoseconds % 1000000000ULL);
		pthread_cond_timedwait(&wait->cond, &lock->mutex, &until);
	}
	if (waitq_sequence(queue) != observed)
		return 0;
	if (deadline != 0 && sched_ticks() >= deadline)
		return ETIMEDOUT;
	return 0;
}

/* Wakes every sleeper; the waker holds the condition lock. */
void
waitq_wake_all(
	struct wait_queue *queue)
{
	CHECK(spin_depth == 1);
	__atomic_fetch_add(&queue->sequence, 1U, __ATOMIC_SEQ_CST);
	pthread_cond_broadcast(&find_object(queue)->cond);
}

/* The scheduler's clock: milliseconds since the test began. */
uint64_t
sched_ticks(void)
{
	struct timespec now;
	int64_t nanoseconds;

	clock_gettime(CLOCK_MONOTONIC, &now);
	nanoseconds = (int64_t)(now.tv_sec - started.tv_sec) * 1000000000LL + (now.tv_nsec - started.tv_nsec);
	return (uint64_t)nanoseconds / 1000000ULL;
}

/* The one bus thread at a time. */
static struct thread bus_thread_record;
static void (*bus_thread_entry)(void *);
static void *bus_thread_argument;

/* Runs the recorded thread's entry. */
static void *
thread_main(
	void *argument)
{
	void (*entry)(void *);
	void *entry_argument;

	(void)argument;
	entry = bus_thread_entry;
	entry_argument = bus_thread_argument;
	entry(entry_argument);
	return NULL;
}

int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	(void)priority;
	bus_thread_entry = entry;
	bus_thread_argument = arg;
	bus_thread_record.detached = 0;
	*result = &bus_thread_record;
	return 0;
}

/* Starts the recorded thread detached, as the kernel's would be. */
void
thread_start(
	struct thread *thread)
{
	pthread_t host;

	CHECK(thread == &bus_thread_record && thread->detached == 1U);
	CHECK(pthread_create(&host, NULL, thread_main, NULL) == 0);
	CHECK(pthread_detach(host) == 0);
}

/* The files of a device are freed with it. */
void
bcm43455_files_release(
	struct bcm43455_files *files)
{
	free(files->firmware);
	free(files->clm);
	free(files->nvram);
	memset(files, 0, sizeof(*files));
}

/*
 * The network device and WLAN station stand-ins.
 */

struct net_device *
net_device_alloc(void)
{
	net_allocs++;
	memset(&fake_net, 0, sizeof(fake_net));
	return &fake_net;
}

int
net_device_create(
	struct net_device *device)
{
	CHECK(device == &fake_net && device->ops != NULL && device->ops->transmit != NULL);
	net_creates++;
	return 0;
}

int
net_device_gone(
	struct net_device *device)
{
	(void)device;
	net_gones++;
	return 0;
}

void
net_device_destroy(
	struct net_device *device)
{
	(void)device;
	net_destroys++;
}

int
net_device_set_carrier(
	struct net_device *device,
	int carrier)
{
	device->carrier = (unsigned)carrier;
	return 0;
}

void
net_device_schedule_poll(
	struct net_device *device)
{
	(void)device;
	__atomic_fetch_add(&polls_scheduled, 1U, __ATOMIC_SEQ_CST);
}

void
packet_buf_free(
	struct packet_buf *packet)
{
	(void)packet;
	packets_freed++;
}

/* Checks what the WLAN layer checks of a FullMAC-less radio: an offloaded profile, the lifecycle methods. */
int
wlan_station_attach(
	struct net_device *device,
	const struct wlan_radio_ops *ops,
	void *radio_context,
	const struct wlan_scan_profile *scan_profile,
	struct wlan_station **result)
{
	unsigned i;

	(void)radio_context;
	CHECK(device == &fake_net && ops != NULL && scan_profile != NULL);
	for (i = 0; i < scan_profile->channel_count; i++)
		CHECK((scan_profile->channels[i].flags & WLAN_SCAN_CHANNEL_ACTIVE_ALLOWED) == 0);
	attaches++;
	attached_ops = ops;
	attached_profile = *scan_profile;
	if (attach_answer != 0)
		return attach_answer;
	*result = (struct wlan_station *)&fake_station_token;
	return 0;
}

int
wlan_station_open(
	struct wlan_station *station)
{
	CHECK(station == (struct wlan_station *)&fake_station_token);
	opens++;
	return 0;
}

int
wlan_station_close(
	struct wlan_station *station)
{
	CHECK(station == (struct wlan_station *)&fake_station_token);
	closes++;
	return 0;
}

void
wlan_station_stop_request(
	struct wlan_station *station)
{
	(void)station;
	stop_requests++;
}

void
wlan_station_stop_complete(
	struct wlan_station *station,
	int error)
{
	(void)station;
	stop_completed = error;
}

int
wlan_station_stop_busy(
	struct wlan_station *station)
{
	(void)station;
	return 0;
}

int
wlan_station_quiesce_begin(
	struct wlan_station *station)
{
	(void)station;
	quiesces++;
	return 0;
}

void
wlan_station_quiesce_end(
	struct wlan_station *station)
{
	(void)station;
	quiesce_ends++;
}

int
wlan_station_transmit(
	struct wlan_station *station,
	struct packet_buf *packet)
{
	(void)station;
	packet_buf_free(packet);
	return ENOTCONN;
}

int
wlan_station_ioctl(
	struct net_device *device,
	unsigned long request,
	void *argument)
{
	(void)device;
	(void)request;
	(void)argument;
	return 0;
}

/* Records one report the driver made. */
static void
record_report(
	unsigned kind,
	uint64_t generation,
	uint32_t value,
	int error,
	const struct wlan_bss_record *bss,
	size_t length)
{
	struct report *report;

	pthread_mutex_lock(&report_lock);
	report = &reports[report_count % REPORT_MAX];
	memset(report, 0, sizeof(*report));
	report->kind = kind;
	report->generation = generation;
	report->value = value;
	report->error = error;
	report->length = length;
	if (bss != NULL) {
		memcpy(report->ssid, bss->ssid, bss->ssid_length);
		report->channel = bss->channel;
		report->security = bss->security;
		report->rssi = bss->rssi_dbm;
	}
	report_count++;
	pthread_mutex_unlock(&report_lock);
}

int
wlan_station_report_scan_channel_ready(
	struct wlan_station *station,
	uint64_t generation,
	uint32_t step_index)
{
	CHECK(station == (struct wlan_station *)&fake_station_token);
	record_report(REPORT_READY, generation, step_index, 0, NULL, 0);
	return 0;
}

int
wlan_station_report_scan_channel_complete(
	struct wlan_station *station,
	uint64_t generation,
	uint32_t step_index)
{
	CHECK(station == (struct wlan_station *)&fake_station_token);
	record_report(REPORT_COMPLETE, generation, step_index, 0, NULL, 0);
	return 0;
}

int
wlan_station_report_scan_error(
	struct wlan_station *station,
	uint64_t generation,
	int error)
{
	CHECK(station == (struct wlan_station *)&fake_station_token && error > 0);
	record_report(REPORT_SCAN_ERROR, generation, 0, error, NULL, 0);
	return 0;
}

/* Parses the probe response as the core does, and records the network. */
int
wlan_station_report_scan_frame(
	struct wlan_station *station,
	uint64_t generation,
	const uint8_t *frame,
	size_t length,
	int32_t rssi_dbm,
	uint8_t channel_hint)
{
	struct wlan_bss_record bss;

	CHECK(station == (struct wlan_station *)&fake_station_token);
	CHECK(wlan_frame_parse_bss(frame, length, rssi_dbm, channel_hint, &bss) == 0);
	CHECK(bss.channel == channel_hint);
	record_report(REPORT_FRAME, generation, 0, 0, &bss, length);
	return 0;
}

int
wlan_station_report_fullmac_event(
	struct wlan_station *station,
	uint64_t generation,
	enum wlan_fullmac_event event)
{
	CHECK(station == (struct wlan_station *)&fake_station_token);
	record_report(REPORT_EVENT, generation, (uint32_t)event, 0, NULL, 0);
	return 0;
}

int
wlan_station_report_link_loss(
	struct wlan_station *station,
	uint64_t generation,
	int error)
{
	CHECK(station == (struct wlan_station *)&fake_station_token && error > 0);
	record_report(REPORT_LOSS, generation, 0, error, NULL, 0);
	return 0;
}

/* Takes a frame only for the generation whose port the test keeps open. */
int
wlan_station_report_ethernet(
	struct wlan_station *station,
	uint64_t generation,
	const uint8_t *frame,
	size_t length)
{
	CHECK(station == (struct wlan_station *)&fake_station_token && frame != NULL);
	if (generation != __atomic_load_n(&open_port_generation, __ATOMIC_SEQ_CST))
		return EACCES;
	record_report(REPORT_ETHERNET, generation, 0, 0, NULL, length);
	return 0;
}

void
wlan_station_restart_request(
	struct wlan_station *station)
{
	CHECK(station == (struct wlan_station *)&fake_station_token);
	record_report(REPORT_RESTART, 0, 0, 0, NULL, 0);
}

/* A tick of the test's clock is one millisecond. */
uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	return milliseconds;
}
