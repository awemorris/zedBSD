/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The OpRegion service on a shadow mailbox, as a part of the display ktest.
 *
 * The part drives the Linux OpRegion lifecycle of the started display's
 * OpRegion world (setup, register, resume, notify and ASLE, unregister,
 * cleanup), the ACPI notifier chain with synthetic video events, and the
 * ASLE worker with the reference request handlers.  The Linux text, the
 * notifier chain and the worker queue are the production code; the test
 * owns the mailbox memory, the event source and a fake backlight.  The
 * firmware's region is never written: production runs VBT_ONLY and never
 * sets the service up.
 */

#include "display-ktest.h"
#include "../execution/ktest.h"
#include "../../i915.h"
#include "../../display/opregion.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/sched.h>

#include <uapi/errno.h>
#include <stdint.h>

/*
 * The address the shadow mailbox stands at.  It is an unaligned "physical"
 * token, like the target's 0x614e5018; the service resolves it through its
 * mapping table and never maps it.
 */
#define I915_TEST_ASLS_TOKEN		0x6f000018u

/* Where the shadow VBT stands, relative to the shadow mailbox (the 2.1 relative RVDA). */
#define I915_TEST_RVDA_OFFSET		0x2000u

/* The mailboxes the target announces: ACPI, ASLE, VBT and ASLE_EXT, no SWSCI. */
#define I915_TEST_MBOXES		0x1du

/* The offsets of the OpRegion header fields the shadow fills. */
#define I915_TEST_OVER_SIZE		0x10u
#define I915_TEST_OVER_MINOR		0x16u
#define I915_TEST_OVER_MAJOR		0x17u
#define I915_TEST_MBOXES_OFFSET		0x58u

/* The fields of struct opregion_acpi (mailbox 1, at 0x100). */
#define I915_TEST_DRDY			0x100u
#define I915_TEST_CSTS			0x104u
#define I915_TEST_CEVT			0x108u
#define I915_TEST_DIDL			0x120u
#define I915_TEST_CADL			0x160u
#define I915_TEST_CHPD			0x1a8u

/* The fields of struct opregion_asle (mailbox 3, at 0x300). */
#define I915_TEST_ASLE_ARDY		0x300u
#define I915_TEST_ASLE_ASLC		0x304u
#define I915_TEST_ASLE_TCHE		0x308u
#define I915_TEST_ASLE_ALSI		0x30cu
#define I915_TEST_ASLE_BCLP		0x310u
#define I915_TEST_ASLE_CBLV		0x318u
#define I915_TEST_ASLE_RVDA		0x3bau
#define I915_TEST_ASLE_RVDS		0x3c2u

/* The ASLE request bits and the answers the reference writes back into ASLC. */
#define I915_TEST_ASLC_SET_ALS_ILLUM	(1u << 0)
#define I915_TEST_ASLC_SET_BACKLIGHT	(1u << 1)
#define I915_TEST_ASLC_SET_PFIT		(1u << 2)
#define I915_TEST_ASLC_UNKNOWN_REQUEST	(1u << 9)
#define I915_TEST_ASLC_ALS_ILLUM_FAILED	(1u << 10)
#define I915_TEST_ASLC_BACKLIGHT_FAILED	(1u << 12)
#define I915_TEST_ASLC_PFIT_FAILED	(1u << 14)

/* The valid bits of the requested (BCLP) and the current (CBLV) backlight level. */
#define I915_TEST_BCLP_VALID		(1u << 31)
#define I915_TEST_CBLV_VALID		(1u << 31)

/* The DRM connector types the service is given (DRM_MODE_CONNECTOR_DisplayPort, _HDMIA). */
#define I915_TEST_CONNECTOR_DP		10
#define I915_TEST_CONNECTOR_HDMIA	11

/* The PCI power state the adapter notification is asked about (PCI_D0). */
#define I915_TEST_PCI_D0		0

/* The ACPI video event types: a display switch, and another notification. */
#define I915_TEST_ACPI_VIDEO_SWITCH	0x80u
#define I915_TEST_ACPI_VIDEO_OTHER	0x81u

/* How long, in ticks, an ASLE request, a hand-shake and a held callback may take. */
#define I915_TEST_ASLE_FLUSH_TICKS	200u
#define I915_TEST_WAIT_TICKS		100u
#define I915_TEST_HOLD_TICKS		30u

/*
 * The shadow OpRegion: driver-owned RAM in the OpRegion format.
 *
 * The test writes it as the firmware would, the service reads and answers
 * in it.  Each step of the run fills it anew; it is never handed to the
 * firmware.
 */
static uint8_t shadow_region[8192] __attribute__((aligned(4096)));

/*
 * The shadow VBT the relative RVDA of the shadow OpRegion points at.
 *
 * It is a minimal valid VBT (header and BDB header only), filled once per
 * run before the setup.
 */
static uint8_t shadow_vbt[96] __attribute__((aligned(64)));

/*
 * How often the second receiver on the ACPI notifier chain was called.
 *
 * The chain test clears it before registering the receiver.
 */
static int other_calls;

/*
 * The worker queue the ASLE service and the lifecycle works run on.
 *
 * It is created by the first run and lives for the rest of the boot: the
 * display's OpRegion world keeps pointing at it after the run.
 * test_wq_live is nonzero once it exists.
 */
static struct i915_workqueue test_wq;
static int test_wq_live;

/*
 * What the fake backlight was asked last: how often it was called, the
 * level and the maximum.  They only grow or are overwritten; each check
 * compares against the call count it saw before its request.
 */
static unsigned backlight_calls;
static uint32_t backlight_level;
static uint32_t backlight_max;

/*
 * The hand-shake of the lifecycle tests: entered is signalled by a callback
 * or work once it runs, release is what it waits on (nobody signals it, so
 * the wait lasts I915_TEST_HOLD_TICKS and keeps the callback busy).
 */
static struct i915_completion entered_completion;
static struct i915_completion release_completion;

/*
 * What the held callbacks report: whether the slow receiver has finished
 * and how often it was called, whether the backlight should hold, and
 * whether a held backlight request has finished.  They are written by the
 * worker and read by the test thread.
 */
static volatile int slow_finished;
static volatile int slow_calls;
static volatile int backlight_block;
static volatile int backlight_block_finished;

/*
 * The works of the lifecycle tests: one dispatches a video event from the
 * worker, the other keeps the single worker busy.  Each is prepared right
 * before it is queued.
 */
static struct i915_work dispatch_work;
static struct i915_work blocker_work;

static uint32_t i915_shadow_read(unsigned offset);
static void i915_shadow_write(unsigned offset, uint32_t value);
static void i915_vbt_write16(unsigned offset, uint16_t value);
static void i915_shadow_init(uint32_t mboxes);
static void i915_shadow_vbt_init(void);
static int i915_other_receiver(struct notifier_block *nb, unsigned long action, void *data);
static void i915_log_dispatch(struct i915_display *display, const char *id, const struct i915_acpi_dispatch *dispatch, uint32_t before, uint32_t after);
static void i915_fake_backlight(void *context, uint32_t level, uint32_t max);
static void i915_asle_request(struct i915_display *display, uint32_t aslc, uint32_t bclp);
static void i915_log_asle(struct i915_display *display, const char *id, uint32_t request, unsigned calls_before);
static void i915_setup_tests(struct i915_ktest *ktest, struct i915_display *display);
static void i915_register_tests(struct i915_ktest *ktest, struct i915_display *display);
static void i915_notify_tests(struct i915_ktest *ktest, struct i915_display *display);
static void i915_chain_tests(struct i915_ktest *ktest, struct i915_display *display);
static void i915_asle_tests(struct i915_ktest *ktest, struct i915_display *display);
static void i915_unregister_tests(struct i915_ktest *ktest, struct i915_display *display);
static int i915_slow_receiver(struct notifier_block *nb, unsigned long action, void *data);
static void i915_dispatch_work(void *context);
static void i915_blocking_backlight(void *context, uint32_t level, uint32_t max);
static void i915_blocker_work(void *context);
static int i915_start_service(struct i915_display *display, void (*backlight)(void *, uint32_t, uint32_t));
static void i915_lifecycle_tests(struct i915_ktest *ktest, struct i915_display *display);

/*
 * Runs the OpRegion service tests on the started display's OpRegion world.
 *
 * The world is the one the display start created; production leaves it
 * without a backend, and every step here ends with it cleaned up again.
 */
void
drv_i915_display_ktest_opregion(
	struct i915_ktest *ktest)
{
	struct i915_display *display;
	int error;

	/* The service lives in the display's OpRegion world; without one there is nothing to drive. */
	display = NULL;
	if (ktest->device != NULL)
		display = ktest->device->display;
	if (display == NULL) {
		drv_i915_ktest_skip(ktest, "opregion: the OpRegion service tests", "the device has no display");
		return;
	}
	if (display->opregion_world == NULL) {
		drv_i915_ktest_skip(ktest, "opregion: the OpRegion service tests", "the display has no OpRegion world");
		return;
	}

	/* Prepares the ACPI notifier chain of the display. */
	drv_i915_acpi_notifier_init(display);

	/* Creates the shared worker queue the first time. */
	if (!test_wq_live) {
		error = drv_i915_workqueue_create(&test_wq, "i915-opregion-test");
		drv_i915_ktest_check(ktest, error == 0, "opregion: OP-WQ the shared worker queue exists");
		if (error == 0)
			test_wq_live = 1;
	}

	/* intel_opregion_setup() on the shadow: the ASLS token to the shadow, the RVDA to the shadow VBT. */
	i915_setup_tests(ktest, display);

	/* Registration and resume_display: DIDL / CADL from the connectors, the ready words published. */
	i915_register_tests(ktest, display);

	/* The reference decision table of the registered video event callback. */
	i915_notify_tests(ktest, display);

	/* A second receiver on the chain: the stop mask and the priority order. */
	i915_chain_tests(ktest, display);

	/* The ASLE requests on the registered, ready service. */
	i915_asle_tests(ktest, display);

	/* Unregistration and cleanup: the reference order, then nothing reaches the service. */
	i915_unregister_tests(ktest, display);

	/* Stop while busy, stale requests, a failed setup and re-initialization. */
	i915_lifecycle_tests(ktest, display);
}

/* Reads a 32-bit word of the shadow OpRegion. */
static uint32_t
i915_shadow_read(
	unsigned offset)
{
	uint32_t value;

	/* Copies the word out; the offset need not be aligned. */
	kern_memcpy(&value, shadow_region + offset, 4);

	/* Succeeded: reports the word. */
	return value;
}

/* Writes a 32-bit word of the shadow OpRegion, as the firmware would. */
static void
i915_shadow_write(
	unsigned offset,
	uint32_t value)
{
	/* Copies the word in; the offset need not be aligned. */
	kern_memcpy(shadow_region + offset, &value, 4);
}

/* Writes a 16-bit word of the shadow VBT. */
static void
i915_vbt_write16(
	unsigned offset,
	uint16_t value)
{
	/* Copies the word in; the offset need not be aligned. */
	kern_memcpy(shadow_vbt + offset, &value, 2);
}

/* Fills the shadow OpRegion with an empty version 2.1 header announcing the given mailboxes. */
static void
i915_shadow_init(
	uint32_t mboxes)
{
	static const char signature[16] = {
		'I', 'n', 't', 'e', 'l', 'G', 'r', 'a',
		'p', 'h', 'i', 'c', 's', 'M', 'e', 'm'
	};

	/* Starts from an empty region with the OpRegion signature. */
	kern_memset(shadow_region, 0, sizeof(shadow_region));
	kern_memcpy(shadow_region, signature, 16);

	/* The header: 8 KiB, version 2.1, and the mailboxes. */
	shadow_region[I915_TEST_OVER_SIZE] = 8u;
	shadow_region[I915_TEST_OVER_MINOR] = 1u;
	shadow_region[I915_TEST_OVER_MAJOR] = 2u;
	kern_memcpy(shadow_region + I915_TEST_MBOXES_OFFSET, &mboxes, 4);
}

/* Fills the shadow VBT: a VBT header (48 bytes) followed by a BDB header (22 bytes). */
static void
i915_shadow_vbt_init(void)
{
	static const char vbt_signature[20] = {
		'$', 'V', 'B', 'T', ' ', 'S', 'H', 'A', 'D', 'O',
		'W', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
	};
	static const char bdb_signature[16] = {
		'B', 'I', 'O', 'S', '_', 'D', 'A', 'T',
		'A', '_', 'B', 'L', 'O', 'C', 'K', ' '
	};
	uint32_t bdb_offset;

	/* The VBT header: signature, version 100, header 48 bytes, VBT 96 bytes, BDB at 48. */
	kern_memset(shadow_vbt, 0, sizeof(shadow_vbt));
	kern_memcpy(shadow_vbt, vbt_signature, 20);
	i915_vbt_write16(20, 100);
	i915_vbt_write16(22, 48);
	i915_vbt_write16(24, 96);
	bdb_offset = 48u;
	kern_memcpy(shadow_vbt + 28, &bdb_offset, 4);

	/* The BDB header: signature, version 249, header 22 bytes, BDB 48 bytes. */
	kern_memcpy(shadow_vbt + 48, bdb_signature, 16);
	i915_vbt_write16(64, 249);
	i915_vbt_write16(66, 22);
	i915_vbt_write16(68, 48);
}

/* Counts a call of the second receiver on the chain. */
static int
i915_other_receiver(
	struct notifier_block *nb,
	unsigned long action,
	void *data)
{
	UNUSED_PARAMETER(nb);
	UNUSED_PARAMETER(action);
	UNUSED_PARAMETER(data);

	/* Counts the call and lets the walk go on. */
	other_calls++;

	/* Succeeded: the event was seen. */
	return NOTIFY_OK;
}

/* Logs what one synthetic video event did to the chain and to CSTS. */
static void
i915_log_dispatch(
	struct i915_display *display,
	const char *id,
	const struct i915_acpi_dispatch *dispatch,
	uint32_t before,
	uint32_t after)
{
	const char *backend;
	unsigned epoch;

	/* Reads what the instance is bound to. */
	backend = drv_i915_opregion_mailbox_backend(display);
	epoch = drv_i915_opregion_service_epoch(display);

	/* Logs the dispatch. */
	kern_logf("i915: OP-NOTIFY %s mailbox_backend=%s service_epoch=%u event_source=%s callback_result=0x%x dispatch_result=%d calls=%u request_before csts=0x%x response_after csts=0x%x real_opregion_write_count=0\n",
	    id,
	    backend,
	    epoch,
	    dispatch->event_source,
	    (unsigned)dispatch->callback_result,
	    dispatch->dispatch_result,
	    dispatch->calls,
	    before,
	    after);
}

/* Records a backlight request the ASLE worker handed over. */
static void
i915_fake_backlight(
	void *context,
	uint32_t level,
	uint32_t max)
{
	UNUSED_PARAMETER(context);

	/* Counts the request and keeps what it asked for. */
	backlight_calls++;
	backlight_level = level;
	backlight_max = max;
}

/*
 * Makes one ASLE request as the firmware does (the payload, then the
 * request word, then the GSE entry) and waits for the worker.
 */
static void
i915_asle_request(
	struct i915_display *display,
	uint32_t aslc,
	uint32_t bclp)
{
	uint64_t deadline;

	/* Writes the payload and the request word. */
	i915_shadow_write(I915_TEST_ASLE_BCLP, bclp);
	i915_shadow_write(I915_TEST_ASLE_ASLC, aslc);

	/* Raises the GSE entry, then waits for the worker to answer. */
	drv_i915_opregion_gse_entry(display);
	deadline = sched_ticks() + I915_TEST_ASLE_FLUSH_TICKS;
	(void)drv_i915_opregion_asle_flush(display, deadline);
}

/* Logs what one ASLE request did to the mailbox, the backlight and the worker. */
static void
i915_log_asle(
	struct i915_display *display,
	const char *id,
	uint32_t request,
	unsigned calls_before)
{
	const char *backend;
	unsigned epoch;
	unsigned started;
	unsigned finished;
	unsigned queued_new;
	unsigned queued_pending;
	uint32_t aslc;
	uint32_t cblv;

	/* Reads the instance, the worker and the answer. */
	backend = drv_i915_opregion_mailbox_backend(display);
	epoch = drv_i915_opregion_service_epoch(display);
	drv_i915_opregion_worker_stats_get(display, &started, &finished, &queued_new, &queued_pending);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	cblv = i915_shadow_read(I915_TEST_ASLE_CBLV);

	/* Logs the request and its answer. */
	kern_logf("i915: OP-ASLE %s mailbox_backend=%s service_epoch=%u event_source=SYNTHETIC(GSE entry) display_backend=MODEL request_before aslc=0x%x | response_after aslc=0x%x cblv=0x%x | backlight calls +%u level %u/%u | worker started %u finished %u queued new %u pending %u | real_opregion_write_count=0\n",
	    id,
	    backend,
	    epoch,
	    request,
	    aslc,
	    cblv,
	    backlight_calls - calls_before,
	    backlight_level,
	    backlight_max,
	    started,
	    finished,
	    queued_new,
	    queued_pending);
}

/* Runs intel_opregion_setup() on the shadow and checks what it found and wrote. */
static void
i915_setup_tests(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	const void *vbt;
	const char *backend;
	uint64_t rvda;
	uint32_t rvds;
	uint32_t vbt_size;
	uint32_t chpd;
	uint32_t ardy;
	int error;
	int shadow_bound;
	int passed;

	/* The target's mailboxes and a relative RVDA to the shadow VBT; CHPD clear, ARDY a stale value. */
	i915_shadow_init(I915_TEST_MBOXES);
	i915_shadow_vbt_init();
	rvda = I915_TEST_RVDA_OFFSET;
	rvds = sizeof(shadow_vbt);
	kern_memcpy(shadow_region + I915_TEST_ASLE_RVDA, &rvda, 8);
	kern_memcpy(shadow_region + I915_TEST_ASLE_RVDS, &rvds, 4);
	i915_shadow_write(I915_TEST_CHPD, 0u);
	i915_shadow_write(I915_TEST_ASLE_ARDY, 0x77u);

	/* Maps the shadow and its VBT, then runs the setup, stopping at the first refusal. */
	error = drv_i915_opregion_shadow_map(display, I915_TEST_ASLS_TOKEN, shadow_region, sizeof(shadow_region));
	if (error == 0)
		error = drv_i915_opregion_shadow_map(display, I915_TEST_ASLS_TOKEN + I915_TEST_RVDA_OFFSET, shadow_vbt, sizeof(shadow_vbt));
	if (error == 0)
		error = drv_i915_opregion_shadow_setup(display, I915_TEST_ASLS_TOKEN);

	/* The instance is bound to the shadow when the setup succeeded. */
	backend = drv_i915_opregion_mailbox_backend(display);
	shadow_bound = kern_strcmp(backend, "SHADOW");
	passed = 0;
	if (error == 0 && shadow_bound == 0)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-SETUP intel_opregion_setup() on the shadow: ASLS token, signature, mailboxes");

	/* The setup itself writes CHPD and ARDY, in the shadow only. */
	chpd = i915_shadow_read(I915_TEST_CHPD);
	ardy = i915_shadow_read(I915_TEST_ASLE_ARDY);
	passed = 0;
	if (chpd == 1u && ardy == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-SETUP-WRITES setup itself writes CHPD = 1 and ARDY = NOT_READY (in the shadow only)");

	/* The relative RVDA resolves through the mapping table to the shadow VBT. */
	vbt_size = 0u;
	vbt = drv_i915_opregion_vbt(display, &vbt_size);
	passed = 0;
	if (vbt == (const void *)shadow_vbt && vbt_size == sizeof(shadow_vbt))
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-SETUP-RVDA the 2.1 relative RVDA is resolved through the mapping table to the shadow VBT (valid)");
}

/* Starts the service with its connectors, registers it, and checks what resume_display published. */
static void
i915_register_tests(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	uint32_t didl[4];
	uint32_t cadl[4];
	uint32_t csts;
	uint32_t drdy;
	uint32_t tche;
	uint32_t ardy;
	unsigned unported;
	unsigned boundaries_before;
	unsigned boundaries;
	unsigned unmaps;
	unsigned count;
	unsigned i;
	int registered;
	int error;
	int passed;

	/* The worker queue, the video policy, and the connectors eDP (fake backlight), DP and HDMI. */
	error = drv_i915_opregion_service_start(display, &test_wq, I915_OPREGION_POLICY_VIDEO);
	if (error == 0)
		error = drv_i915_opregion_add_backlight(display, i915_fake_backlight, NULL);
	if (error == 0)
		error = drv_i915_opregion_add_connector(display, I915_TEST_CONNECTOR_DP, NULL, NULL);
	if (error == 0)
		error = drv_i915_opregion_add_connector(display, I915_TEST_CONNECTOR_HDMIA, NULL, NULL);
	drv_i915_ktest_check(ktest, error == 0, "opregion: OP-START worker queue, policy video, connectors eDP (fake backlight), DP, HDMI");

	/* Leaves stale values the registration must overwrite, then registers. */
	i915_shadow_write(I915_TEST_CSTS, 0x55u);
	i915_shadow_write(I915_TEST_DIDL + 12u, 0xdeadu);
	drv_i915_opregion_counters(display, &unported, &boundaries_before, &unmaps);
	drv_i915_opregion_register(display);
	drv_i915_opregion_counters(display, &unported, &boundaries, &unmaps);

	/* Reads the device lists and the ready words the registration published. */
	for (i = 0u; i < 4u; i++) {
		didl[i] = i915_shadow_read(I915_TEST_DIDL + 4u * i);
		cadl[i] = i915_shadow_read(I915_TEST_CADL + 4u * i);
	}
	csts = i915_shadow_read(I915_TEST_CSTS);
	drdy = i915_shadow_read(I915_TEST_DRDY);
	tche = i915_shadow_read(I915_TEST_ASLE_TCHE);
	ardy = i915_shadow_read(I915_TEST_ASLE_ARDY);
	kern_logf("i915: OP-LIFECYCLE register: DIDL %x %x %x %x | CADL %x %x %x %x | CSTS %x DRDY %x TCHE %x ARDY %x | boundaries +%u unported %u\n",
	    didl[0],
	    didl[1],
	    didl[2],
	    didl[3],
	    cadl[0],
	    cadl[1],
	    cadl[2],
	    cadl[3],
	    csts,
	    drdy,
	    tche,
	    ardy,
	    boundaries - boundaries_before,
	    unported);

	/*
	 * The ACPI _DOD ids (ACPI 5.0 Appendix B.3.2) are type << 8 | index
	 * per type: the internal eDP is 4, the external DP and HDMI are 3.
	 */
	registered = drv_i915_opregion_notifier_registered(display);
	count = drv_i915_acpi_notifier_count(display);
	passed = 0;
	if (registered &&
	    count == 1u &&
	    didl[0] == 0x400u &&
	    didl[1] == 0x300u &&
	    didl[2] == 0x301u &&
	    didl[3] == 0u &&
	    cadl[0] == 0x400u &&
	    cadl[1] == 0x300u &&
	    cadl[2] == 0x301u &&
	    cadl[3] == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-REGISTER notifier on the chain; DIDL / CADL = eDP 0x400, DP 0x300, HDMI 0x301, then 0 (terminated)");

	/* resume_display publishes the ready words; _DSM is a boundary and no SWSCI access happens. */
	passed = 0;
	if (csts == 0u &&
	    drdy == 1u &&
	    tche == 2u &&
	    ardy == 1u &&
	    boundaries == boundaries_before + 1u &&
	    unported == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-READY resume_display publishes CSTS 0, DRDY 1, TCHE BLC_EN, ARDY READY (shadow); _DSM recorded as a boundary; the SWSCI-absent adapter notification touches no PCI config");

	/* A second registration is refused by the chain, which the reference ignores. */
	drv_i915_opregion_register(display);
	count = drv_i915_acpi_notifier_count(display);
	drv_i915_ktest_check(ktest, count == 1u, "opregion: OP-REGISTER-TWICE the chain refuses the block again (EEXIST, as the reference ignores): still one entry");

	/* Without an SWSCI mailbox the adapter notification is refused. */
	error = drv_i915_opregion_notify_adapter(display, I915_TEST_PCI_D0);
	drv_i915_ktest_check(ktest, error == ENODEV, "opregion: OP-SWSCI no SWSCI mailbox: intel_opregion_notify_adapter -> swsci -> check_swsci_function -> ENODEV");
}

/* Delivers synthetic events to the registered callback and checks the reference decision table. */
static void
i915_notify_tests(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_acpi_dispatch dispatch;
	uint32_t before;
	uint32_t after;
	int passed;

	/* An event that is not of the video class is not the callback's. */
	i915_shadow_write(I915_TEST_CSTS, 0x55u);
	before = i915_shadow_read(I915_TEST_CSTS);
	(void)drv_i915_acpi_notifier_call_chain(display, "button", "LID0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
	after = i915_shadow_read(I915_TEST_CSTS);
	i915_log_dispatch(display, "not-video", &dispatch, before, after);
	passed = 0;
	if (dispatch.callback_result == NOTIFY_DONE &&
	    dispatch.dispatch_result == 0 &&
	    dispatch.calls == 1u &&
	    after == 0x55u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-NOTIFY-CLASS an event that is not of the video class: NOTIFY_DONE, CSTS untouched");

	/* A display switch (CEVT bit 0) is accepted and answered in CSTS. */
	i915_shadow_write(I915_TEST_CSTS, 0x55u);
	i915_shadow_write(I915_TEST_CEVT, 0x1u);
	before = i915_shadow_read(I915_TEST_CSTS);
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
	after = i915_shadow_read(I915_TEST_CSTS);
	i915_log_dispatch(display, "0x80-switch", &dispatch, before, after);
	passed = 0;
	if (dispatch.callback_result == NOTIFY_OK &&
	    dispatch.dispatch_result == 0 &&
	    after == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-NOTIFY-SWITCH video 0x80 with CEVT bit 0 (display switch): NOTIFY_OK, CSTS written to 0");

	/* A switch event caused by the lid is refused, and CSTS is still answered. */
	i915_shadow_write(I915_TEST_CSTS, 0x55u);
	i915_shadow_write(I915_TEST_CEVT, 0x2u);
	before = i915_shadow_read(I915_TEST_CSTS);
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
	after = i915_shadow_read(I915_TEST_CSTS);
	i915_log_dispatch(display, "0x80-not-switch", &dispatch, before, after);
	passed = 0;
	if (dispatch.callback_result == NOTIFY_BAD &&
	    dispatch.dispatch_result == EINVAL &&
	    after == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-NOTIFY-BAD video 0x80 without CEVT bit 0: NOTIFY_BAD, dispatch EINVAL, and CSTS is STILL written to 0");

	/* Another video notification is accepted in this version. */
	i915_shadow_write(I915_TEST_CSTS, 0x55u);
	i915_shadow_write(I915_TEST_CEVT, 0x0u);
	before = i915_shadow_read(I915_TEST_CSTS);
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_OTHER, 0u, "SYNTHETIC", &dispatch);
	after = i915_shadow_read(I915_TEST_CSTS);
	i915_log_dispatch(display, "0x81", &dispatch, before, after);
	passed = 0;
	if (dispatch.callback_result == NOTIFY_OK &&
	    dispatch.dispatch_result == 0 &&
	    after == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-NOTIFY-OTHER video type 0x81: NOTIFY_OK in this version, CSTS written to 0");
}

/* Puts a second receiver behind the service's callback and checks how the walk ends. */
static void
i915_chain_tests(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_acpi_dispatch dispatch;
	struct notifier_block other;
	int error;
	int again;
	int passed;

	/* The second receiver runs after the service's block. */
	kern_memset(&other, 0, sizeof(other));
	other.notifier_call = i915_other_receiver;
	other.priority = -1;
	other_calls = 0;
	error = drv_i915_register_acpi_notifier(display, &other);
	drv_i915_ktest_check(ktest, error == 0, "opregion: OP-CHAIN a second receiver registers after i915's");

	/* NOTIFY_BAD carries the stop mask: the walk ends at the service. */
	i915_shadow_write(I915_TEST_CEVT, 0x0u);
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
	passed = 0;
	if (dispatch.callback_result == NOTIFY_BAD &&
	    dispatch.calls == 1u &&
	    other_calls == 0)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-CHAIN-STOP NOTIFY_BAD (stop mask) ends the walk: the later receiver is not called");

	/* NOTIFY_OK lets the walk go on to the second receiver. */
	i915_shadow_write(I915_TEST_CEVT, 0x1u);
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
	passed = 0;
	if (dispatch.callback_result == NOTIFY_OK &&
	    dispatch.calls == 2u &&
	    other_calls == 1)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-CHAIN-WALK NOTIFY_OK continues: both receivers run, in priority order");

	/* The receiver comes off the chain once; a second unregistration finds nothing. */
	error = drv_i915_unregister_acpi_notifier(display, &other);
	again = 0;
	if (error == 0)
		again = drv_i915_unregister_acpi_notifier(display, &other);
	passed = 0;
	if (error == 0 && again == ENOENT)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-CHAIN-UNREGISTER removes the block; a second unregister is ENOENT");
}

/* Makes ASLE requests on the registered service and checks the worker's answers. */
static void
i915_asle_tests(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	unsigned calls_before;
	unsigned started;
	unsigned finished;
	unsigned queued_new;
	unsigned queued_pending;
	uint32_t aslc;
	uint32_t cblv;
	int passed;

	/* A valid backlight request: served once, CBLV answers the level in percent. */
	calls_before = backlight_calls;
	i915_asle_request(display, I915_TEST_ASLC_SET_BACKLIGHT, I915_TEST_BCLP_VALID | 128u);
	i915_log_asle(display, "backlight-128", I915_TEST_ASLC_SET_BACKLIGHT, calls_before);
	drv_i915_opregion_worker_stats_get(display, &started, &finished, &queued_new, &queued_pending);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	cblv = i915_shadow_read(I915_TEST_ASLE_CBLV);
	passed = 0;
	if (backlight_calls == calls_before + 1u &&
	    backlight_level == 128u &&
	    backlight_max == 255u &&
	    aslc == 0u &&
	    cblv == (51u | I915_TEST_CBLV_VALID) &&
	    started == 1u &&
	    finished == 1u &&
	    queued_new == 1u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-ASLE-BCLP a valid request: the real worker ran once, intel_backlight_set_acpi(128, 255), CBLV = DIV_ROUND_UP(128*100,255) | valid, ASLC = 0 (success)");

	/* A request without the valid bit changes nothing and fails. */
	calls_before = backlight_calls;
	i915_shadow_write(I915_TEST_ASLE_CBLV, 0x1234u);
	i915_asle_request(display, I915_TEST_ASLC_SET_BACKLIGHT, 128u);
	i915_log_asle(display, "backlight-no-valid", I915_TEST_ASLC_SET_BACKLIGHT, calls_before);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	cblv = i915_shadow_read(I915_TEST_ASLE_CBLV);
	passed = 0;
	if (backlight_calls == calls_before &&
	    aslc == I915_TEST_ASLC_BACKLIGHT_FAILED &&
	    cblv == 0x1234u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-ASLE-INVALID no valid bit: no backlight change, ASLC = BACKLIGHT_FAILED, CBLV untouched");

	/* A level above 255 changes nothing and fails. */
	calls_before = backlight_calls;
	i915_asle_request(display, I915_TEST_ASLC_SET_BACKLIGHT, I915_TEST_BCLP_VALID | 300u);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	passed = 0;
	if (backlight_calls == calls_before && aslc == I915_TEST_ASLC_BACKLIGHT_FAILED)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-ASLE-RANGE a level above 255: no backlight change, ASLC = BACKLIGHT_FAILED");

	/* Under the native policy the reference ignores the request and answers success. */
	calls_before = backlight_calls;
	drv_i915_opregion_set_policy(display, I915_OPREGION_POLICY_NATIVE);
	i915_asle_request(display, I915_TEST_ASLC_SET_BACKLIGHT, I915_TEST_BCLP_VALID | 200u);
	i915_log_asle(display, "backlight-native-policy", I915_TEST_ASLC_SET_BACKLIGHT, calls_before);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	passed = 0;
	if (backlight_calls == calls_before && aslc == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-ASLE-NATIVE policy native: the reference ignores the request and answers success (no PWM change)");
	drv_i915_opregion_set_policy(display, I915_OPREGION_POLICY_VIDEO);

	/* Backlight, ALS and panel fit together: the backlight is served, the others fail, the answer is the OR. */
	calls_before = backlight_calls;
	i915_shadow_write(I915_TEST_ASLE_ALSI, 500u);
	i915_asle_request(display, I915_TEST_ASLC_SET_BACKLIGHT | I915_TEST_ASLC_SET_ALS_ILLUM | I915_TEST_ASLC_SET_PFIT, I915_TEST_BCLP_VALID | 64u);
	i915_log_asle(display, "mixed", I915_TEST_ASLC_SET_BACKLIGHT | I915_TEST_ASLC_SET_ALS_ILLUM | I915_TEST_ASLC_SET_PFIT, calls_before);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	passed = 0;
	if (backlight_calls == calls_before + 1u &&
	    backlight_level == 64u &&
	    aslc == (I915_TEST_ASLC_ALS_ILLUM_FAILED | I915_TEST_ASLC_PFIT_FAILED))
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-ASLE-MIXED backlight served, ALS and pfit answered FAILED as the reference does: the status is the OR");

	/* A request with no known bit gets no answer at all. */
	calls_before = backlight_calls;
	i915_asle_request(display, I915_TEST_ASLC_UNKNOWN_REQUEST, I915_TEST_BCLP_VALID | 10u);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	passed = 0;
	if (backlight_calls == calls_before && aslc == I915_TEST_ASLC_UNKNOWN_REQUEST)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-ASLE-NOREQ no known request bit: the worker returns without an answer (ASLC unchanged)");
}

/* Unregisters and cleans the service up, and checks that nothing reaches it afterwards. */
static void
i915_unregister_tests(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_acpi_dispatch dispatch;
	const char *backend;
	uint32_t before;
	uint32_t after;
	uint32_t ardy;
	uint32_t drdy;
	unsigned count;
	unsigned unported;
	unsigned boundaries;
	unsigned unmaps;
	unsigned started;
	unsigned finished;
	unsigned queued_new;
	unsigned queued_pending;
	unsigned started_after;
	unsigned finished_after;
	unsigned queued_new_after;
	unsigned queued_pending_after;
	int registered;
	int unbound;
	int passed;

	/* suspend_display (ARDY NOT_READY, the worker synced, DRDY 0), then the notifier comes off. */
	drv_i915_opregion_unregister(display);
	registered = drv_i915_opregion_notifier_registered(display);
	count = drv_i915_acpi_notifier_count(display);
	ardy = i915_shadow_read(I915_TEST_ASLE_ARDY);
	drdy = i915_shadow_read(I915_TEST_DRDY);
	passed = 0;
	if (!registered &&
	    count == 0u &&
	    ardy == 0u &&
	    drdy == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-UNREGISTER ARDY NOT_READY, the worker synced, DRDY 0, the callback off the chain (reference order)");

	/* The same display switch as before now reaches nobody. */
	i915_shadow_write(I915_TEST_CSTS, 0x55u);
	i915_shadow_write(I915_TEST_CEVT, 0x1u);
	before = i915_shadow_read(I915_TEST_CSTS);
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
	after = i915_shadow_read(I915_TEST_CSTS);
	i915_log_dispatch(display, "after-unregister", &dispatch, before, after);
	passed = 0;
	if (dispatch.calls == 0u &&
	    dispatch.callback_result == NOTIFY_DONE &&
	    after == 0x55u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-AFTER-UNREGISTER the same event again reaches nobody: CSTS untouched");

	/* The reference's second unregistration does nothing. */
	drv_i915_opregion_unregister(display);
	count = drv_i915_acpi_notifier_count(display);
	drv_i915_ktest_check(ktest, count == 0u, "opregion: OP-UNREGISTER-AGAIN the reference's second unregister does nothing");

	/* The cleanup unmaps the header and the RVDA mapping, and no SWSCI access happened. */
	(void)drv_i915_opregion_cleanup(display);
	drv_i915_opregion_counters(display, &unported, &boundaries, &unmaps);
	backend = drv_i915_opregion_mailbox_backend(display);
	unbound = kern_strcmp(backend, "NONE");
	passed = 0;
	if (unbound == 0 &&
	    unmaps >= 2u &&
	    unported == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-CLEANUP the reference cleanup unmaps the header and the RVDA mapping; no unported access happened");

	/* After the cleanup a GSE entry queues nothing: the gate is closed. */
	drv_i915_opregion_worker_stats_get(display, &started, &finished, &queued_new, &queued_pending);
	drv_i915_opregion_gse_entry(display);
	drv_i915_opregion_worker_stats_get(display, &started_after, &finished_after, &queued_new_after, &queued_pending_after);
	passed = 0;
	if (queued_new_after == queued_new &&
	    queued_pending_after == queued_pending &&
	    started_after == started)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-ASLE-NOMBOX after cleanup the GSE entry queues nothing (the gate is closed; no ASLE mailbox)");
}

/* A receiver that signals its entry and then holds the chain for I915_TEST_HOLD_TICKS. */
static int
i915_slow_receiver(
	struct notifier_block *nb,
	unsigned long action,
	void *data)
{
	uint64_t deadline;

	UNUSED_PARAMETER(nb);
	UNUSED_PARAMETER(action);
	UNUSED_PARAMETER(data);

	/* Counts the call and tells the test it is inside. */
	slow_calls++;
	drv_i915_complete(&entered_completion);

	/* Holds the chain: the unregistration must wait for this callback. */
	deadline = sched_ticks() + I915_TEST_HOLD_TICKS;
	(void)drv_i915_wait_for_completion(&release_completion, deadline);

	/* Reports that the callback has finished. */
	slow_finished = 1;

	/* Succeeded: the event was seen. */
	return NOTIFY_OK;
}

/* Delivers one display switch event from the worker (context is the display). */
static void
i915_dispatch_work(
	void *context)
{
	struct i915_acpi_dispatch dispatch;
	struct i915_display *display;

	/* Delivers the event on the display's chain. */
	display = context;
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
}

/* Records a backlight request and, while asked to, holds the worker inside it. */
static void
i915_blocking_backlight(
	void *context,
	uint32_t level,
	uint32_t max)
{
	uint64_t deadline;

	UNUSED_PARAMETER(context);

	/* Counts the request and keeps what it asked for. */
	backlight_calls++;
	backlight_level = level;
	backlight_max = max;

	/* Holds the worker inside the request, then reports that it finished. */
	if (backlight_block) {
		drv_i915_complete(&entered_completion);
		deadline = sched_ticks() + I915_TEST_HOLD_TICKS;
		(void)drv_i915_wait_for_completion(&release_completion, deadline);
		backlight_block_finished = 1;
	}
}

/* Keeps the single worker busy for I915_TEST_HOLD_TICKS. */
static void
i915_blocker_work(
	void *context)
{
	uint64_t deadline;

	UNUSED_PARAMETER(context);

	/* Tells the test the worker is taken, then holds it. */
	drv_i915_complete(&entered_completion);
	deadline = sched_ticks() + I915_TEST_HOLD_TICKS;
	(void)drv_i915_wait_for_completion(&release_completion, deadline);
}

/*
 * Sets the service up on a fresh shadow with the given backlight and
 * registers it; returns 0, the first refusal, or EIO when the notifier did
 * not end up registered.
 */
static int
i915_start_service(
	struct i915_display *display,
	void (*backlight)(void *, uint32_t, uint32_t))
{
	int error;
	int registered;

	/* Maps a fresh shadow and sets the instance up on it. */
	i915_shadow_init(I915_TEST_MBOXES);
	error = drv_i915_opregion_shadow_map(display, I915_TEST_ASLS_TOKEN, shadow_region, sizeof(shadow_region));
	if (error != 0)
		return error;
	error = drv_i915_opregion_shadow_setup(display, I915_TEST_ASLS_TOKEN);
	if (error != 0)
		return error;

	/* Starts the ASLE service with the one backlight. */
	error = drv_i915_opregion_service_start(display, &test_wq, I915_OPREGION_POLICY_VIDEO);
	if (error != 0)
		return error;
	error = drv_i915_opregion_add_backlight(display, backlight, NULL);
	if (error != 0)
		return error;

	/* Registers the instance; its callback must be on the chain afterwards. */
	drv_i915_opregion_register(display);
	registered = drv_i915_opregion_notifier_registered(display);
	if (!registered)
		return EIO;

	/* Succeeded: the service runs on the fresh shadow. */
	return 0;
}

/* Stops the service while it is busy, feeds it stale requests, and sets it up again. */
static void
i915_lifecycle_tests(
	struct i915_ktest *ktest,
	struct i915_display *display)
{
	struct i915_acpi_dispatch dispatch;
	struct notifier_block slow;
	const char *backend;
	uint64_t deadline;
	unsigned started;
	unsigned finished;
	unsigned queued_new;
	unsigned queued_pending;
	unsigned started_after;
	unsigned finished_after;
	unsigned queued_new_after;
	unsigned queued_pending_after;
	unsigned dropped;
	unsigned refused;
	unsigned dropped_after;
	unsigned epoch_before;
	unsigned epoch;
	unsigned count;
	uint32_t aslc;
	uint32_t ardy;
	uint32_t drdy;
	int calls_before;
	int error;
	int cleaned;
	int registered;
	int unbound;
	int entered;
	int passed;

	/* Prepares the hand-shake. */
	drv_i915_completion_init(&entered_completion, "op-entered");
	drv_i915_completion_init(&release_completion, "op-release");

	/* A bad signature: the reference setup fails and leaves the instance unbound. */
	i915_shadow_init(I915_TEST_MBOXES);
	shadow_region[0] = 'X';
	epoch_before = drv_i915_opregion_service_epoch(display);
	error = drv_i915_opregion_shadow_map(display, I915_TEST_ASLS_TOKEN, shadow_region, sizeof(shadow_region));
	passed = 0;
	if (error == 0) {
		error = drv_i915_opregion_shadow_setup(display, I915_TEST_ASLS_TOKEN);
		backend = drv_i915_opregion_mailbox_backend(display);
		unbound = kern_strcmp(backend, "NONE");
		if (error == EINVAL && unbound == 0)
			passed = 1;
	}
	drv_i915_ktest_check(ktest, passed, "opregion: OP-L0 a bad signature: intel_opregion_setup fails (EINVAL), the instance stays unbound");

	/* A registration after the failed setup does nothing, and the cleanup has nothing to do. */
	drv_i915_opregion_register(display);
	registered = drv_i915_opregion_notifier_registered(display);
	count = drv_i915_acpi_notifier_count(display);
	cleaned = EINVAL;
	if (!registered && count == 0u)
		cleaned = drv_i915_opregion_cleanup(display);
	drv_i915_ktest_check(ktest, cleaned == 0, "opregion: OP-L0-REGISTER register after a failed setup does nothing (no header); cleanup is a no-op");

	/* Set up and registered again on a good shadow: a new service epoch. */
	error = i915_start_service(display, i915_blocking_backlight);
	epoch = drv_i915_opregion_service_epoch(display);
	passed = 0;
	if (error == 0 && epoch == epoch_before + 2u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-L-REINIT setup + register again on a fresh shadow: registered, service epoch advanced");

	/* A slow receiver registers; the worker delivers an event to it. */
	kern_memset(&slow, 0, sizeof(slow));
	slow.notifier_call = i915_slow_receiver;
	slow.priority = 10;
	slow_finished = 0;
	slow_calls = 0;
	drv_i915_reinit_completion(&entered_completion);
	drv_i915_reinit_completion(&release_completion);
	error = drv_i915_register_acpi_notifier(display, &slow);
	drv_i915_ktest_check(ktest, error == 0, "opregion: OP-L1 a slow receiver registers");
	drv_i915_work_init(&dispatch_work, i915_dispatch_work, display);
	(void)drv_i915_queue_work(&test_wq, &dispatch_work);
	deadline = sched_ticks() + I915_TEST_WAIT_TICKS;
	entered = drv_i915_wait_for_completion(&entered_completion, deadline);
	drv_i915_ktest_check(ktest, entered == 1, "opregion: OP-L1 the event is being delivered (callback entered)");

	/* The unregistration returns only after the running callback has finished. */
	error = drv_i915_unregister_acpi_notifier(display, &slow);
	passed = 0;
	if (error == 0 && slow_finished == 1)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-L1-SYNC unregister returned only after the running callback had finished");

	/* After the unregistration the receiver is never called again. */
	deadline = sched_ticks() + I915_TEST_WAIT_TICKS;
	(void)drv_i915_flush_work(&test_wq, &dispatch_work, deadline);
	calls_before = slow_calls;
	(void)drv_i915_acpi_notifier_call_chain(display, "video", "GFX0", I915_TEST_ACPI_VIDEO_SWITCH, 0u, "SYNTHETIC", &dispatch);
	drv_i915_ktest_check(ktest, slow_calls == calls_before, "opregion: OP-L1-AFTER after the unregister the receiver is never called again");

	/* The ASLE work is held inside a backlight request when the service stops. */
	backlight_block = 1;
	backlight_block_finished = 0;
	drv_i915_reinit_completion(&entered_completion);
	drv_i915_reinit_completion(&release_completion);
	i915_shadow_write(I915_TEST_ASLE_BCLP, I915_TEST_BCLP_VALID | 100u);
	i915_shadow_write(I915_TEST_ASLE_ASLC, I915_TEST_ASLC_SET_BACKLIGHT);
	drv_i915_opregion_gse_entry(display);
	deadline = sched_ticks() + I915_TEST_WAIT_TICKS;
	entered = drv_i915_wait_for_completion(&entered_completion, deadline);
	drv_i915_ktest_check(ktest, entered == 1, "opregion: OP-L2 the worker is inside the backlight request");

	/* The stop waits for the running work (its answer written), then withdraws the ready words. */
	drv_i915_opregion_unregister(display);
	drv_i915_opregion_worker_stats_get(display, &started, &finished, &queued_new, &queued_pending);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	ardy = i915_shadow_read(I915_TEST_ASLE_ARDY);
	drdy = i915_shadow_read(I915_TEST_DRDY);
	registered = drv_i915_opregion_notifier_registered(display);
	kern_logf("i915: OP-LIFECYCLE stop-while-running: worker started %u finished %u | aslc 0x%x ardy %u drdy %u | unregister_synced=%d\n",
	    started,
	    finished,
	    aslc,
	    ardy,
	    drdy,
	    backlight_block_finished);
	passed = 0;
	if (backlight_block_finished == 1 &&
	    finished == started &&
	    aslc == 0u &&
	    ardy == 0u &&
	    drdy == 0u &&
	    !registered)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-L2-SYNC the stop waited for the running work (its answer written), then ARDY / DRDY 0, notifier off");
	backlight_block = 0;

	/* A request after the stop is dropped at the gate: nothing queued, nothing answered. */
	drv_i915_opregion_gate_counters(display, &dropped, &refused);
	i915_shadow_write(I915_TEST_ASLE_ASLC, I915_TEST_ASLC_SET_BACKLIGHT);
	drv_i915_opregion_gse_entry(display);
	drv_i915_opregion_worker_stats_get(display, &started_after, &finished_after, &queued_new_after, &queued_pending_after);
	drv_i915_opregion_gate_counters(display, &dropped_after, &refused);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	passed = 0;
	if (dropped_after == dropped + 1u &&
	    queued_new_after == queued_new &&
	    queued_pending_after == queued_pending &&
	    aslc == I915_TEST_ASLC_SET_BACKLIGHT)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-L2-STALE a request after the stop is dropped at the gate: nothing queued, nothing answered");

	/* The work is idle, so the cleanup releases the instance. */
	cleaned = drv_i915_opregion_cleanup(display);
	backend = drv_i915_opregion_mailbox_backend(display);
	unbound = kern_strcmp(backend, "NONE");
	passed = 0;
	if (cleaned == 0 && unbound == 0)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-L2-CLEANUP the work is idle: cleanup releases the instance");

	/* The service runs again, and a blocker takes the single worker. */
	error = i915_start_service(display, i915_fake_backlight);
	drv_i915_ktest_check(ktest, error == 0, "opregion: OP-L3 the service runs again");
	drv_i915_reinit_completion(&entered_completion);
	drv_i915_reinit_completion(&release_completion);
	drv_i915_work_init(&blocker_work, i915_blocker_work, NULL);
	(void)drv_i915_queue_work(&test_wq, &blocker_work);
	deadline = sched_ticks() + I915_TEST_WAIT_TICKS;
	entered = drv_i915_wait_for_completion(&entered_completion, deadline);
	drv_i915_ktest_check(ktest, entered == 1, "opregion: OP-L3 the single worker is busy");

	/* A request queued behind the busy worker is cancelled by the stop: never run, never answered. */
	drv_i915_opregion_worker_stats_get(display, &started, &finished, &queued_new, &queued_pending);
	i915_shadow_write(I915_TEST_ASLE_BCLP, I915_TEST_BCLP_VALID | 90u);
	i915_shadow_write(I915_TEST_ASLE_ASLC, I915_TEST_ASLC_SET_BACKLIGHT);
	drv_i915_opregion_gse_entry(display);
	drv_i915_opregion_unregister(display);
	deadline = sched_ticks() + I915_TEST_WAIT_TICKS;
	(void)drv_i915_flush_work(&test_wq, &blocker_work, deadline);
	drv_i915_opregion_worker_stats_get(display, &started_after, &finished_after, &queued_new_after, &queued_pending_after);
	aslc = i915_shadow_read(I915_TEST_ASLE_ASLC);
	ardy = i915_shadow_read(I915_TEST_ASLE_ARDY);
	drdy = i915_shadow_read(I915_TEST_DRDY);
	passed = 0;
	if (queued_new_after == queued_new + 1u &&
	    started_after == started &&
	    aslc == I915_TEST_ASLC_SET_BACKLIGHT &&
	    ardy == 0u &&
	    drdy == 0u)
		passed = 1;
	drv_i915_ktest_check(ktest, passed, "opregion: OP-L3-CANCEL queued behind a busy worker, the request is cancelled by the stop: never run, never answered");

	/* The instance is released for the rest of the boot. */
	cleaned = drv_i915_opregion_cleanup(display);
	drv_i915_ktest_check(ktest, cleaned == 0, "opregion: OP-L3-CLEANUP the instance is released");
}
