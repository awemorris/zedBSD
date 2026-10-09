/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * ACPI events (ACPI 6.5 sections 4.8 and 5.6): the fixed events of the
 * PM1 registers (the power and sleep buttons), the general-purpose events
 * of the GPE blocks and the _Lxx and _Exx methods that handle them, the
 * switch into ACPI mode, and the S5 soft-off that turns the power off
 * (section 7.4 and 16.1).
 *
 * The work is split the way the SCI requires.  drv_acpi_sci_interrupt()
 * runs in the interrupt: it only reads the status registers, masks every
 * event that fired and records it.  drv_acpi_events_process() runs in a
 * thread: it runs the handlers and the AML, clears the status and unmasks
 * the events again.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * The FADT fields the event code reads (ACPI 6.5 table 5.9).
 */
#define FADT_SCI_INT		46U
#define FADT_SMI_CMD		48U
#define FADT_ACPI_ENABLE	52U
#define FADT_PM1A_EVT_BLK	56U
#define FADT_PM1B_EVT_BLK	60U
#define FADT_PM1A_CNT_BLK	64U
#define FADT_PM1B_CNT_BLK	68U
#define FADT_GPE0_BLK		80U
#define FADT_GPE1_BLK		84U
#define FADT_PM1_EVT_LEN	88U
#define FADT_PM1_CNT_LEN	89U
#define FADT_GPE0_BLK_LEN	92U
#define FADT_GPE1_BLK_LEN	93U
#define FADT_GPE1_BASE		94U
#define FADT_FLAGS		112U
#define FADT_RESET_REG		116U
#define FADT_RESET_VALUE	128U
#define FADT_X_PM1A_EVT_BLK	148U
#define FADT_X_PM1B_EVT_BLK	160U
#define FADT_X_PM1A_CNT_BLK	172U
#define FADT_X_PM1B_CNT_BLK	184U
#define FADT_X_GPE0_BLK		220U
#define FADT_X_GPE1_BLK		232U
#define FADT_SLEEP_CONTROL_REG	244U
#define FADT_V1_LENGTH		116U

/*
 * The length of a Generic Address Structure, where its register width in
 * bits and its address are.
 */
#define GAS_LENGTH		12U
#define GAS_WIDTH		1U
#define GAS_ADDRESS		4U

/*
 * The FADT flags the event code reads.
 */
#define FADT_FLAG_POWER_BUTTON	(1U << 4)
#define FADT_FLAG_SLEEP_BUTTON	(1U << 5)
#define FADT_FLAG_RESET_REG	(1U << 10)
#define FADT_FLAG_HW_REDUCED	(1U << 20)

/*
 * The SCI_EN bit of PM1_CNT: set, the platform raises SCIs instead of SMIs.
 */
#define PM1_CNT_SCI_EN		0x0001U

/*
 * The GBL_RLS bit of PM1_CNT: written as one, it tells firmware that the
 * operating system let the Global Lock go while firmware waited for it.
 * SLP_EN is written as one only to sleep, so writes keep it clear.
 */
#define PM1_CNT_GBL_RLS		0x0004U
#define PM1_CNT_SLP_EN		0x2000U

/*
 * The SLP_TYP field of PM1_CNT: the sleep state the platform enters when
 * SLP_EN is written, in the platform's own numbering that \_S5 gives.
 */
#define PM1_CNT_SLP_TYP_SHIFT	10U
#define PM1_CNT_SLP_TYP_MASK	0x1c00U

/*
 * The WAK_STS bit of PM1_STS: set by a wake, written as one to clear it
 * before a sleep so that the wake is seen.
 */
#define PM1_STS_WAK_STS		0x8000U

/*
 * The sleep control register of a hardware-reduced platform (ACPI 6.5
 * section 4.8.3.7): SLP_TYP in bits 2 to 4 and SLP_EN in bit 5.
 */
#define SLEEP_CONTROL_SLP_TYP_SHIFT	2U
#define SLEEP_CONTROL_SLP_EN		0x20U

/*
 * The system state number of the soft-off state, the argument of _PTS.
 */
#define SLEEP_STATE_S5		5U

/*
 * How long the power may take to go after SLP_EN was written, in the
 * 100-nanosecond units of the interpreter's timer, and how many polls
 * of the timer bound the wait when the timer does not run.
 */
#define POWEROFF_WAIT		30000000ULL
#define POWEROFF_POLLS		100000000U

/*
 * The address space of a Generic Address Structure that is I/O ports.
 */
#define GAS_SPACE_SYSTEM_IO	1U

/*
 * How many GPEs the interpreter handles: the ones _Lxx and _Exx can name.
 */
#define GPE_MAX			256U

/*
 * How many times each GPE's handling is logged (BUG-255: which GPEs a lid,
 * a button or the EC raise on a machine, from the boot log).
 */
#define GPE_LOGGED		8U

/*
 * How long the switch into ACPI mode may take, in 10-microsecond polls.
 */
#define ACPI_ENABLE_POLLS	30000U

/*
 * The fixed events: bit numbers of PM1_STS and PM1_EN.
 */
#define FIXED_EVENT_COUNT	16U

/*
 * What woken_gpe holds while no GPE has fired since the sleep began.
 */
#define GPE_WOKEN_NONE		0xffffU

/*
 * The most wake sources one GPE can be armed for.
 */
#define GPE_ARMED_MAX		0xffffU

/*
 * The kinds of GPE handling.
 */
enum gpe_kind {
	GPE_NONE = 0,
	GPE_METHOD = 1,
	GPE_HANDLER = 2
};

/*
 * One register block the FADT names: an I/O port address and its length.
 */
struct register_block {
	uint32_t port;
	unsigned length;
};

/*
 * How one GPE is handled: by its _Lxx or _Exx method or by a driver's C
 * handler.  edge says the event is edge-triggered (_Exx), which clears
 * its status before the handler runs instead of after.  enabled is what
 * the event should be at runtime once its handling ends; wake says a
 * device's _PRW names it, which keeps it masked at runtime when nothing
 * else enables it.  armed counts the wake sources that want the GPE to
 * wake the system from S0 idle: while the system sleeps, exactly the GPEs
 * with a nonzero count raise SCIs.
 */
struct gpe_entry {
	struct drv_acpi_node *method;
	drv_acpi_gpe_handler_t handler;
	void *argument;
	uint16_t armed;
	uint8_t kind;
	uint8_t edge;
	uint8_t enabled;
	uint8_t wake;
};

/*
 * The handler of one fixed event.
 */
struct fixed_entry {
	drv_acpi_fixed_handler_t handler;
	void *argument;
};

/*
 * The event hardware the FADT describes, and how each event is handled.
 *
 * drv_acpi_events_init() fills it; the interrupt and the processing
 * thread share it, and the pending masks are read and written only under
 * the event lock of aml-os.h.  ready is zero until the FADT was read.
 * sleeping is set between drv_acpi_events_sleep_begin() and
 * drv_acpi_events_sleep_end(), and is read and written under the event
 * lock: while it is set only the armed GPEs are unmasked, and woken_gpe
 * keeps the first GPE whose SCI came (GPE_WOKEN_NONE until one does).
 */
static struct {
	struct register_block pm1a_event;
	struct register_block pm1b_event;
	struct register_block pm1a_control;
	struct register_block pm1b_control;
	struct register_block gpe0;
	struct register_block gpe1;
	unsigned gpe1_base;
	unsigned gpe_count;
	uint32_t smi_command;
	uint32_t flags;
	uint16_t sci_interrupt;
	uint16_t fixed_enabled;
	uint16_t fixed_pending;
	uint16_t woken_gpe;
	uint8_t acpi_enable;
	uint8_t ready;
	uint8_t sleeping;
	uint8_t gpe_pending[GPE_MAX / 8U];
	uint8_t gpe_logged[GPE_MAX];
	struct gpe_entry gpes[GPE_MAX];
	struct fixed_entry fixed[FIXED_EVENT_COUNT];
} events;

/*
 * What the S5 soft-off needs: the SLP_TYP values \_S5 names for the PM1a
 * and PM1b control registers, and on a hardware-reduced platform the
 * sleep control register when it is in I/O space.  drv_acpi_events_init()
 * fills it from the namespace and the FADT, while the machine still runs
 * AML freely; drv_acpi_poweroff() only reads it.  known is zero until
 * \_S5 was read, and stays zero on a platform that cannot be turned off.
 */
static struct {
	struct register_block sleep_control;
	uint8_t type_a;
	uint8_t type_b;
	uint8_t reduced;
	uint8_t known;
} soft_off;

/*
 * The reset register the FADT names (ACPI 6.5 section 4.8.3.6): the I/O
 * port that RESET_VALUE is written to to reset the machine.
 * drv_acpi_events_init() fills it from the FADT, also on a
 * hardware-reduced platform; drv_acpi_reset_machine() only reads it.
 * known is zero until a supported register was read, and stays zero on a
 * platform whose FADT says it has none or names one this driver does not
 * write (in memory or in PCI configuration space).
 */
static struct {
	uint32_t port;
	uint8_t value;
	uint8_t known;
} reset_register;

static uint32_t load_u32(const uint8_t *bytes);
static void read_soft_off(const uint8_t *fadt, size_t length, uint32_t flags);
static void read_reset_register(const uint8_t *fadt, size_t length, uint32_t flags);
static struct register_block fadt_block(const uint8_t *fadt, size_t length, unsigned legacy, unsigned wide, unsigned block_length);
static int enable_acpi_mode(void);
static bool gpe_present(unsigned gpe);
static void gpe_register(unsigned gpe, uint32_t *port, uint8_t *bit);
static uint32_t gpe_enable_port(unsigned gpe, uint32_t status_port);
static void record_gpe_block(const struct register_block *block, unsigned base);
static uint8_t port_read8(uint32_t port);
static void port_write8(uint32_t port, uint8_t value);
static uint16_t pm1_read(unsigned offset);
static void pm1_write(unsigned offset, uint16_t value);
static void pm1_control_set(const struct register_block *block, uint16_t bits);
static void pm1_control_sleep(const struct register_block *block, uint8_t type, bool enable);
static void gpe_set_enable(unsigned gpe, bool enable);
static void gpe_clear(unsigned gpe);
static int gpe_method_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static int wake_visitor(struct drv_acpi_node *node, unsigned depth, void *argument);
static int hex_digit(uint8_t character);
static void process_gpe(unsigned gpe);
static void log_gpe_list(const char *what, bool wake);
static bool gpe_wanted(unsigned gpe);
static bool gpe_is_pending(unsigned gpe);
static void log_event_state(unsigned runtime);

/*
 * Reads the event hardware from the FADT, switches the platform into ACPI
 * mode, masks and clears every event, and enables the GPEs that have an
 * _Lxx or _Exx method and are not only for waking the system.
 */
int
drv_acpi_events_init(
	const uint8_t *fadt,
	size_t length)
{
	uint32_t flags;
	unsigned gpe;
	unsigned total;
	unsigned runtime;
	bool present;
	int error;

	/* Refuses a FADT too short to describe the hardware. */
	if (fadt == NULL || length < FADT_V1_LENGTH)
		return EINVAL;

	/* Starts from no event hardware known, so that a failure leaves the SCI unready. */
	kern_memset(&events, 0, sizeof(events));

	/* Learns how the power is turned off, while AML still runs freely, and how the machine is reset. */
	flags = load_u32(fadt + FADT_FLAGS);
	read_soft_off(fadt, length, flags);
	read_reset_register(fadt, length, flags);

	/* A hardware-reduced platform has no fixed hardware and no GPE blocks. */
	if ((flags & FADT_FLAG_HW_REDUCED) != 0) {
		drv_acpi_os_log("ACPI: hardware-reduced platform; no fixed events\n");
		return ENOTSUP;
	}

	/* Reads where the registers are. */
	events.sci_interrupt = (uint16_t)(fadt[FADT_SCI_INT] | fadt[FADT_SCI_INT + 1U] << 8);
	events.smi_command = load_u32(fadt + FADT_SMI_CMD);
	events.acpi_enable = fadt[FADT_ACPI_ENABLE];
	events.pm1a_event = fadt_block(fadt, length, FADT_PM1A_EVT_BLK, FADT_X_PM1A_EVT_BLK, fadt[FADT_PM1_EVT_LEN]);
	events.pm1b_event = fadt_block(fadt, length, FADT_PM1B_EVT_BLK, FADT_X_PM1B_EVT_BLK, fadt[FADT_PM1_EVT_LEN]);
	events.pm1a_control = fadt_block(fadt, length, FADT_PM1A_CNT_BLK, FADT_X_PM1A_CNT_BLK, fadt[FADT_PM1_CNT_LEN]);
	events.pm1b_control = fadt_block(fadt, length, FADT_PM1B_CNT_BLK, FADT_X_PM1B_CNT_BLK, fadt[FADT_PM1_CNT_LEN]);
	events.gpe0 = fadt_block(fadt, length, FADT_GPE0_BLK, FADT_X_GPE0_BLK, fadt[FADT_GPE0_BLK_LEN]);
	events.gpe1 = fadt_block(fadt, length, FADT_GPE1_BLK, FADT_X_GPE1_BLK, fadt[FADT_GPE1_BLK_LEN]);
	events.gpe1_base = fadt[FADT_GPE1_BASE];

	/* Counts the GPEs of the first block: each status byte carries eight. */
	total = events.gpe0.length / 2U * 8U;

	/* A second block ends the numbering at its base plus its own GPEs. */
	if (events.gpe1.length != 0)
		total = events.gpe1_base + events.gpe1.length / 2U * 8U;

	/* Keeps only the GPEs _Lxx and _Exx can name. */
	if (total > GPE_MAX)
		total = GPE_MAX;

	/* Publishes the count every GPE loop runs to. */
	events.gpe_count = total;

	/* Refuses a platform without PM1 event registers. */
	if (events.pm1a_event.port == 0 || events.pm1a_event.length < 4U)
		return ENODEV;

	/* Refuses a platform without a PM1a control block, whose SCI_EN the switch into ACPI mode reads. */
	if (events.pm1a_control.port == 0)
		return ENODEV;

	/* Switches into ACPI mode, so that events raise SCIs. */
	error = enable_acpi_mode();
	if (error != 0)
		return error;

	/* Masks and clears every fixed event. */
	pm1_write(events.pm1a_event.length / 2U, 0);
	pm1_write(0, 0xffffU);

	/* Masks and clears every GPE the blocks have. */
	for (gpe = 0; gpe < events.gpe_count; gpe++) {
		/* Skips a number between the two blocks, which no register carries. */
		present = gpe_present(gpe);
		if (!present)
			continue;

		/* Masks the GPE and clears its status. */
		gpe_set_enable(gpe, false);
		gpe_clear(gpe);
	}

	/*
	 * Enables the Global Lock event, which firmware raises when it lets
	 * the lock go while the operating system waits; the waiter polls the
	 * lock, so the event needs no handler beyond clearing it.
	 * fixed_enabled is the set of fixed events the interrupt may record.
	 */
	events.fixed_enabled = (uint16_t)(1U << DRV_ACPI_EVENT_GLOBAL_LOCK);
	pm1_write(events.pm1a_event.length / 2U, events.fixed_enabled);

	/* The registers are known and quiet: the SCI may be taken now. */
	events.ready = 1;

	/* Keeps the flags, which say whether the buttons are fixed hardware. */
	events.flags = flags;

	/* Finds the GPEs that only wake the system, then the GPE methods. */
	drv_acpi_walk(NULL, wake_visitor, NULL);
	drv_acpi_walk(NULL, gpe_method_visitor, NULL);

	/* Enables the runtime GPEs. */
	runtime = 0;
	for (gpe = 0; gpe < events.gpe_count; gpe++) {
		/* A GPE with a method that does not only wake is a runtime event. */
		if (events.gpes[gpe].kind != GPE_METHOD || events.gpes[gpe].wake)
			continue;

		/*
		 * enabled tells the thread to unmask the GPE again once its
		 * method ran; the count goes into the boot log.
		 */
		events.gpes[gpe].enabled = 1;
		gpe_set_enable(gpe, true);
		runtime++;
	}

	/* Logs the mode and the enabled events, so that a boot log shows the event hardware armed. */
	log_event_state(runtime);

	/*
	 * Names the GPEs with a method that run, and the ones a _PRW keeps for
	 * waking only (masked while the machine runs): a boot that counts other
	 * runtime GPEs than another shows which one changed (BUG-255).
	 */
	log_gpe_list("runtime GPEs", false);
	log_gpe_list("wake-only GPEs with a method", true);

	/* Succeeded: the platform raises SCIs for the enabled events. */
	return 0;
}

/*
 * Reports the interrupt the SCI arrives on, or zero before initialization.
 */
unsigned
drv_acpi_sci_irq(void)
{
	/* Reports the interrupt the FADT names. */
	return events.sci_interrupt;
}

/*
 * Installs the handler of a fixed event and enables the event.
 */
int
drv_acpi_fixed_event_install(
	enum drv_acpi_fixed_event event,
	drv_acpi_fixed_handler_t handler,
	void *argument)
{
	unsigned long state;
	uint16_t enable;

	/* Refuses an event before initialization or outside PM1. */
	if (!events.ready || (unsigned)event >= FIXED_EVENT_COUNT)
		return EINVAL;

	/* A button the platform has as a device, not as fixed hardware, has no fixed event. */
	if (event == DRV_ACPI_EVENT_POWER_BUTTON && (events.flags & FADT_FLAG_POWER_BUTTON) != 0)
		return ENODEV;
	if (event == DRV_ACPI_EVENT_SLEEP_BUTTON && (events.flags & FADT_FLAG_SLEEP_BUTTON) != 0)
		return ENODEV;

	/* Installs the handler. */
	events.fixed[event].handler = handler;
	events.fixed[event].argument = argument;

	/*
	 * Enables the event in PM1_EN.  The bit in fixed_enabled lets the
	 * interrupt record the event; the one in PM1_EN lets it raise the SCI.
	 */
	state = drv_acpi_os_event_lock();

	events.fixed_enabled |= (uint16_t)(1U << event);
	enable = pm1_read(events.pm1a_event.length / 2U);
	pm1_write(events.pm1a_event.length / 2U, (uint16_t)(enable | (1U << event)));

	drv_acpi_os_event_unlock(state);

	/* Succeeded: the event raises an SCI and reaches the handler. */
	return 0;
}

/*
 * Installs a driver's C handler for a GPE in place of any method.
 *
 * The GPE is enabled as well.
 */
int
drv_acpi_gpe_install(
	unsigned gpe,
	bool edge,
	drv_acpi_gpe_handler_t handler,
	void *argument)
{
	unsigned long state;
	bool present;

	/* Refuses a GPE before initialization or outside the blocks. */
	if (!events.ready || gpe >= events.gpe_count)
		return EINVAL;

	/* Refuses a number between the two blocks, which no register carries. */
	present = gpe_present(gpe);
	if (!present)
		return EINVAL;

	/*
	 * Installs the handler and enables the GPE.  The handler kind keeps
	 * the method walk from replacing it, and enabled tells the thread to
	 * unmask the GPE again after each event.
	 */
	state = drv_acpi_os_event_lock();

	events.gpes[gpe].kind = GPE_HANDLER;
	events.gpes[gpe].handler = handler;
	events.gpes[gpe].argument = argument;
	events.gpes[gpe].edge = (uint8_t)edge;
	events.gpes[gpe].enabled = 1;
	gpe_set_enable(gpe, true);

	drv_acpi_os_event_unlock(state);

	/* Succeeded: the GPE raises an SCI and reaches the handler. */
	return 0;
}

/*
 * Enables at runtime a GPE that a device's _PRW names, for a driver that
 * needs the device's events while the system runs.
 *
 * A GPE some _PRW names is kept masked at runtime, as a wake-only event.
 * Firmware often signals a lid or a button through that same GPE, whose
 * _Lxx or _Exx method then notifies the device (BUG-253: the Latitude
 * 5330's lid is GPE 0x18, which LID0's and PBTN's _PRW name); the lid and
 * button driver enables it, as the operating systems the firmware is
 * written for do.  During a sleep the GPE goes on following its wake
 * sources and becomes a runtime one when the sleep ends.  It reports
 * EINVAL for a GPE the blocks do not have and ENOENT for one that no
 * method or handler handles, which would fire once and stay masked.
 */
int
drv_acpi_gpe_runtime_enable(
	unsigned gpe)
{
	struct gpe_entry *entry;
	unsigned long state;
	bool present;
	bool pending;

	/* Refuses a GPE before initialization or outside the blocks. */
	if (!events.ready || gpe >= events.gpe_count)
		return EINVAL;

	/* Refuses a number between the two blocks, which no register carries. */
	present = gpe_present(gpe);
	if (!present)
		return EINVAL;

	/* Makes the GPE a runtime event, with the interrupt kept out. */
	entry = &events.gpes[gpe];
	state = drv_acpi_os_event_lock();

	/* Refuses a GPE nothing handles. */
	if (entry->kind == GPE_NONE) {
		drv_acpi_os_event_unlock(state);
		return ENOENT;
	}

	/*
	 * enabled tells the thread to unmask the GPE again after each event
	 * and the end of a sleep to leave it unmasked.  Outside a sleep it is
	 * unmasked now, unless the thread has still to handle it and unmasks
	 * it itself; a status that latched while it was masked is an event
	 * still to be handled (the method reads what changed).
	 */
	entry->enabled = 1;
	pending = gpe_is_pending(gpe);
	if (!events.sleeping && !pending)
		gpe_set_enable(gpe, true);

	drv_acpi_os_event_unlock(state);

	/* Succeeded: the GPE raises SCIs at runtime. */
	return 0;
}

/*
 * Arms or disarms a GPE as a wake source of S0 idle.
 *
 * Each wake source that wants the GPE to wake the system (a device whose
 * _PRW names it, or the EC whose queries carry the lid and the buttons)
 * arms it once and disarms it once; the GPE stays armed while any source
 * holds it.  During a sleep the change reaches the enable register at
 * once.  It reports EINVAL for a GPE the blocks do not have or one
 * disarmed more often than armed, and EOVERFLOW when the count is full.
 */
int
drv_acpi_gpe_wake_set(
	unsigned gpe,
	bool arm)
{
	struct gpe_entry *entry;
	unsigned long state;
	bool present;
	bool pending;
	bool wanted;

	/* Refuses a GPE before initialization or outside the blocks. */
	if (!events.ready || gpe >= events.gpe_count)
		return EINVAL;

	/* Refuses a number between the two blocks, which no register carries. */
	present = gpe_present(gpe);
	if (!present)
		return EINVAL;

	/* Counts the source in or out, and follows the count in hardware while the system sleeps. */
	entry = &events.gpes[gpe];
	state = drv_acpi_os_event_lock();

	/* Moves the count of the sources that want the GPE. */
	if (arm) {
		/* Refuses a count that cannot grow. */
		if (entry->armed == GPE_ARMED_MAX) {
			drv_acpi_os_event_unlock(state);
			return EOVERFLOW;
		}

		/* One more source wants the GPE to wake the system. */
		entry->armed++;
	} else {
		/* Refuses a disarm that no arm matches. */
		if (entry->armed == 0) {
			drv_acpi_os_event_unlock(state);
			return EINVAL;
		}

		/* One source fewer; zero means nothing wakes the system through the GPE. */
		entry->armed--;
	}

	/*
	 * During a sleep the enable bit follows the count at once, except for
	 * a GPE the thread has still to handle, which it unmasks itself.
	 */
	pending = gpe_is_pending(gpe);
	if (events.sleeping && !pending) {
		wanted = gpe_wanted(gpe);
		gpe_set_enable(gpe, wanted);
	}

	drv_acpi_os_event_unlock(state);

	/* Succeeded: the GPE is armed as often as its sources want. */
	return 0;
}

/*
 * Leaves only the armed GPEs able to raise SCIs, for S0 idle.
 *
 * Every other GPE is masked; the runtime ones are unmasked again by
 * drv_acpi_events_sleep_end().  A wake-only GPE (one that is masked at
 * runtime) has its status cleared before it is unmasked, because the
 * status may have latched while it was masked and would wake the system
 * at once.  A runtime GPE keeps its status, which is an event still to be
 * handled.  A GPE the interrupt recorded and the thread has not yet
 * handled is left to the thread, which unmasks it after its handler when
 * it is armed.  The PM1 fixed events are left as they are: the fixed
 * power button and the RTC alarm wake the system as they are.  It reports
 * ENODEV before the event hardware is known and EBUSY during a sleep.
 */
int
drv_acpi_events_sleep_begin(void)
{
	struct gpe_entry *entry;
	unsigned long state;
	unsigned gpe;
	unsigned armed;
	bool present;
	bool pending;

	/* Refuses before the event hardware is known. */
	if (!events.ready)
		return ENODEV;

	/* Switches the GPE blocks to the armed GPEs alone, with the interrupt kept out. */
	state = drv_acpi_os_event_lock();

	/* Refuses a sleep inside a sleep. */
	if (events.sleeping) {
		drv_acpi_os_event_unlock(state);
		return EBUSY;
	}

	/*
	 * sleeping tells the thread to unmask only armed GPEs after their
	 * handlers, and the interrupt to keep the first GPE that fires.
	 */
	events.sleeping = 1;
	events.woken_gpe = GPE_WOKEN_NONE;

	/* Masks every GPE but the armed ones, and counts those for the log. */
	armed = 0;
	for (gpe = 0; gpe < events.gpe_count; gpe++) {
		/* Skips a number between the two blocks, which no register carries. */
		present = gpe_present(gpe);
		if (!present)
			continue;

		/* Masks a GPE that wakes nothing. */
		entry = &events.gpes[gpe];
		if (entry->armed == 0) {
			gpe_set_enable(gpe, false);
			continue;
		}

		/* Leaves a recorded GPE to the thread, which unmasks it after its handler. */
		pending = gpe_is_pending(gpe);
		if (pending)
			continue;

		/* Clears the stale status of a GPE masked at runtime. */
		if (!entry->enabled)
			gpe_clear(gpe);

		/* Unmasks the wake GPE. */
		gpe_set_enable(gpe, true);
		armed++;
	}

	drv_acpi_os_event_unlock(state);

	/* Logs how many GPEs can wake the system, so that a sleep's log shows the wake sources armed. */
	drv_acpi_os_log("ACPI: sleeping with %u wake GPEs\n", armed);

	/* Succeeded: only the armed GPEs raise SCIs until the sleep ends. */
	return 0;
}

/*
 * Unmasks the runtime GPEs again after S0 idle, and reports the first GPE
 * that fired while the system slept.
 *
 * Each GPE is set back to what it is at runtime: unmasked when it is
 * enabled, masked otherwise (the wake-only GPEs).  A GPE the interrupt
 * recorded and the thread has not yet handled stays masked; the thread
 * unmasks it after its handler.  woken receives the GPE whose SCI came
 * first during the sleep, or DRV_ACPI_GPE_NONE when none did; it may be
 * NULL.  It reports EINVAL when no sleep began.
 */
int
drv_acpi_events_sleep_end(
	unsigned *woken)
{
	unsigned long state;
	unsigned gpe;
	unsigned first;
	bool present;
	bool pending;
	bool runtime;

	/* Refuses before the event hardware is known. */
	if (!events.ready)
		return EINVAL;

	/* Sets every GPE back to its runtime state, with the interrupt kept out. */
	state = drv_acpi_os_event_lock();

	/* Refuses an end without a sleep. */
	if (!events.sleeping) {
		drv_acpi_os_event_unlock(state);
		return EINVAL;
	}

	/* Ends the sleep for the thread and the interrupt, and takes the GPE that woke the system. */
	events.sleeping = 0;
	first = DRV_ACPI_GPE_NONE;
	if (events.woken_gpe != GPE_WOKEN_NONE)
		first = events.woken_gpe;

	/* Puts each GPE back to its runtime enable bit. */
	for (gpe = 0; gpe < events.gpe_count; gpe++) {
		/* Skips a number between the two blocks, which no register carries. */
		present = gpe_present(gpe);
		if (!present)
			continue;

		/* Leaves a recorded GPE to the thread, which unmasks it after its handler. */
		pending = gpe_is_pending(gpe);
		if (pending)
			continue;

		/* Unmasks a runtime GPE and masks a wake-only one. */
		runtime = false;
		if (events.gpes[gpe].enabled)
			runtime = true;

		/* Writes the bit. */
		gpe_set_enable(gpe, runtime);
	}

	drv_acpi_os_event_unlock(state);

	/* Reports the GPE that woke the system. */
	if (woken != NULL)
		*woken = first;

	/* Succeeded: the runtime GPEs raise SCIs again. */
	return 0;
}

/*
 * Takes the first GPE that fired since the sleep began or since the last
 * call, without ending the sleep (ws052-p006): the coordinator asks after
 * each wake whether a GPE woke the system, and a spurious wake is slept
 * through again.  woken receives the GPE or DRV_ACPI_GPE_NONE.  It
 * reports EINVAL when no sleep began.
 */
int
drv_acpi_events_sleep_woken(
	unsigned *woken)
{
	unsigned long state;
	unsigned first;

	/* Refuses before the event hardware is known. */
	if (!events.ready)
		return EINVAL;

	/* Takes the GPE and makes room for the next wake's, with the interrupt kept out. */
	state = drv_acpi_os_event_lock();

	/* Refuses outside a sleep. */
	if (!events.sleeping) {
		drv_acpi_os_event_unlock(state);
		return EINVAL;
	}

	/* The first GPE since the last call, or none. */
	first = DRV_ACPI_GPE_NONE;
	if (events.woken_gpe != GPE_WOKEN_NONE)
		first = events.woken_gpe;
	events.woken_gpe = GPE_WOKEN_NONE;

	drv_acpi_os_event_unlock(state);

	/* Reports the GPE. */
	if (woken != NULL)
		*woken = first;

	/* Succeeded: the sleep goes on. */
	return 0;
}

/*
 * Records and masks every event that fired, in the SCI's interrupt.
 *
 * It touches only hardware registers, and reports whether anything is
 * pending for drv_acpi_events_process().
 */
bool
drv_acpi_sci_interrupt(void)
{
	unsigned long state;
	uint16_t status;
	uint16_t enable;
	uint16_t fired;
	unsigned index;
	bool pending;

	/* Nothing is pending before initialization. */
	if (!events.ready)
		return false;

	/* Records the fired events under the lock the thread takes them with. */
	state = drv_acpi_os_event_lock();

	/* Records and masks the fixed events that fired. */
	status = pm1_read(0);
	enable = pm1_read(events.pm1a_event.length / 2U);
	fired = (uint16_t)(status & enable & events.fixed_enabled);
	if (fired != 0) {
		events.fixed_pending |= fired;
		pm1_write(events.pm1a_event.length / 2U, (uint16_t)(enable & ~fired));
	}

	/* Records and masks the GPEs that fired, block by block. */
	record_gpe_block(&events.gpe0, 0);
	if (events.gpe1.length != 0)
		record_gpe_block(&events.gpe1, events.gpe1_base);

	/* Anything is pending when a fixed event fired. */
	pending = false;
	if (events.fixed_pending != 0)
		pending = true;

	/* Otherwise a GPE is pending when any register of them has a bit set. */
	for (index = 0; index < GPE_MAX / 8U; index++) {
		/* Stops at the first pending register. */
		if (events.gpe_pending[index] != 0) {
			pending = true;
			break;
		}
	}

	drv_acpi_os_event_unlock(state);

	/* Reports whether the thread has work. */
	return pending;
}

/*
 * Runs the handler of every pending event, in the SCI's thread.
 *
 * Each event's status is cleared and the event is unmasked again.
 */
void
drv_acpi_events_process(void)
{
	struct fixed_entry *entry;
	unsigned long state;
	uint16_t pending;
	uint16_t enable;
	uint8_t gpe_pending[GPE_MAX / 8U];
	unsigned event;
	unsigned gpe;

	/* Takes the pending events, leaving none for the next interrupt to add to. */
	state = drv_acpi_os_event_lock();

	pending = events.fixed_pending;
	events.fixed_pending = 0;
	kern_memcpy(gpe_pending, events.gpe_pending, sizeof(gpe_pending));
	kern_memset(events.gpe_pending, 0, sizeof(events.gpe_pending));

	drv_acpi_os_event_unlock(state);

	/* Handles each pending fixed event, then clears and unmasks it. */
	for (event = 0; event < FIXED_EVENT_COUNT; event++) {
		/* Skips an event that did not fire. */
		if ((pending & (1U << event)) == 0)
			continue;

		/* Clears its status, which is written as one. */
		pm1_write(0, (uint16_t)(1U << event));

		/* Calls its handler. */
		entry = &events.fixed[event];
		if (entry->handler != NULL)
			entry->handler((enum drv_acpi_fixed_event)event, entry->argument);

		/* Unmasks it again in PM1_EN. */
		state = drv_acpi_os_event_lock();

		enable = pm1_read(events.pm1a_event.length / 2U);
		pm1_write(events.pm1a_event.length / 2U, (uint16_t)(enable | (1U << event)));

		drv_acpi_os_event_unlock(state);
	}

	/* Handles each pending GPE. */
	for (gpe = 0; gpe < events.gpe_count; gpe++) {
		/* Skips a GPE that did not fire. */
		if ((gpe_pending[gpe / 8U] & (1U << (gpe % 8U))) == 0)
			continue;

		/* Runs its handler or method, clears it and unmasks it. */
		process_gpe(gpe);
	}
}

/*
 * Tells firmware with GBL_RLS that the Global Lock was let go.
 *
 * Firmware waited for the lock while the operating system held it.
 */
void
drv_acpi_events_global_release(void)
{
	unsigned long state;

	/* Nothing can be signalled before the registers are known. */
	if (!events.ready)
		return;

	/* Sets GBL_RLS in each PM1 control block. */
	state = drv_acpi_os_event_lock();

	pm1_control_set(&events.pm1a_control, PM1_CNT_GBL_RLS);
	if (events.pm1b_control.length != 0)
		pm1_control_set(&events.pm1b_control, PM1_CNT_GBL_RLS);

	drv_acpi_os_event_unlock(state);
}

/*
 * Turns the power off: the S5 soft-off state.
 *
 * _PTS(5) tells firmware the transition is coming; then the SLP_TYP values
 * \_S5 named are written with SLP_EN into the PM1 control registers, or
 * into the sleep control register of a hardware-reduced platform.  The
 * write normally does not return.  It reports ENODEV when the platform gave
 * no way to turn itself off, and ETIMEDOUT when the power stayed on; the
 * caller halts then.
 */
int
drv_acpi_poweroff(void)
{
	struct drv_acpi_object *arguments[1];
	struct drv_acpi_object *state_number;
	struct drv_acpi_object *result;
	unsigned long state;
	uint64_t start;
	uint64_t now;
	unsigned poll;
	unsigned gpe;
	uint8_t control;
	bool present;
	int error;

	/* Refuses a platform whose soft-off state is unknown. */
	if (!soft_off.known)
		return ENODEV;

	/* Refuses a platform whose sleep register was not found. */
	if (soft_off.reduced) {
		if (soft_off.sleep_control.length == 0)
			return ENODEV;
	} else {
		if (!events.ready || events.pm1a_control.length == 0)
			return ENODEV;
	}

	/* Allocates the argument of _PTS, the number of the S5 state. */
	state_number = drv_acpi_object_integer_new(SLEEP_STATE_S5);
	if (state_number == NULL)
		return ENOMEM;

	/* Tells firmware with _PTS that S5 is coming. */
	arguments[0] = state_number;
	result = NULL;
	error = drv_acpi_evaluate(NULL, "\\_PTS", arguments, 1, &result);
	drv_acpi_object_release(result);
	drv_acpi_object_release(state_number);

	/* A firmware without _PTS needs no notice; a failed _PTS does not stop the power going. */
	if (error != 0 && error != ENOENT)
		drv_acpi_os_log("ACPI: _PTS(5) failed (error %d); turning off anyway\n", error);

	/*
	 * From here no event is taken: the event lock keeps the SCI's thread
	 * out and interrupts off on this CPU until the power goes.
	 */
	state = drv_acpi_os_event_lock();

	/* A hardware-reduced platform sleeps through one byte. */
	if (soft_off.reduced) {
		/* Writes the sleep type with SLP_EN in the same byte. */
		control = (uint8_t)(soft_off.type_a << SLEEP_CONTROL_SLP_TYP_SHIFT);
		control |= SLEEP_CONTROL_SLP_EN;
		(void)drv_acpi_os_port_write(soft_off.sleep_control.port, 8, control);
	} else {
		/* Masks every fixed event, so that nothing wakes the platform. */
		pm1_write(events.pm1a_event.length / 2U, 0);

		/* Masks every GPE the blocks have. */
		for (gpe = 0; gpe < events.gpe_count; gpe++) {
			/* Skips a number between the two blocks, which no register carries. */
			present = gpe_present(gpe);
			if (!present)
				continue;

			/* Masks the GPE. */
			gpe_set_enable(gpe, false);
		}

		/* Clears a stale wake, so that the next wake is seen. */
		pm1_write(0, PM1_STS_WAK_STS);

		/* Writes the sleep type into both blocks first, then the type with SLP_EN, as ACPICA does. */
		pm1_control_sleep(&events.pm1a_control, soft_off.type_a, false);
		if (events.pm1b_control.length != 0)
			pm1_control_sleep(&events.pm1b_control, soft_off.type_b, false);
		pm1_control_sleep(&events.pm1a_control, soft_off.type_a, true);
		if (events.pm1b_control.length != 0)
			pm1_control_sleep(&events.pm1b_control, soft_off.type_b, true);
	}

	/* Waits for the power to go: by the timer when it runs, by count otherwise. */
	start = drv_acpi_os_timer();
	for (poll = 0; poll < POWEROFF_POLLS; poll++) {
		/* A platform still running this long after SLP_EN did not turn off. */
		now = drv_acpi_os_timer();
		if (now - start >= POWEROFF_WAIT)
			break;
	}

	/*
	 * The function only reaches its end when the platform stayed on: a
	 * successful soft-off never returns, so this failure is the last
	 * statement rather than a success return.
	 */
	drv_acpi_os_event_unlock(state);
	drv_acpi_os_log("ACPI: the platform did not turn off\n");
	return ETIMEDOUT;
}

/*
 * Resets the machine through the FADT's reset register (ACPI 6.5 section
 * 4.8.3.6): RESET_VALUE written to the register's I/O port.
 *
 * The write normally resets the machine at once (on many machines it is a
 * request to firmware, a write to the SMI command port); the caller waits a
 * moment and tries the platform's other ways when it did not.  Nothing is
 * evaluated, so it may be called after the devices were shut down.  It
 * reports ENODEV when the FADT gave no register this driver can write, and
 * the port write's error.
 */
int
drv_acpi_reset_machine(void)
{
	int error;

	/* Refuses a platform without a usable reset register. */
	if (!reset_register.known)
		return ENODEV;

	/* Writes the reset value; the register is one byte wide. */
	error = drv_acpi_os_port_write(reset_register.port, 8, reset_register.value);
	if (error != 0)
		return error;

	/* Succeeded: the reset was asked for. */
	return 0;
}

/* Reads a little-endian 32-bit value. */
static uint32_t
load_u32(
	const uint8_t *bytes)
{
	uint32_t value;

	/* Assembles the bytes, lowest first. */
	value = (uint32_t)bytes[0];
	value |= (uint32_t)bytes[1] << 8;
	value |= (uint32_t)bytes[2] << 16;
	value |= (uint32_t)bytes[3] << 24;

	/* Reports the assembled value. */
	return value;
}

/*
 * Reads what the S5 soft-off needs: the SLP_TYP values \_S5 names, and on
 * a hardware-reduced platform the sleep control register of the FADT.
 */
static void
read_soft_off(
	const uint8_t *fadt,
	size_t length,
	uint32_t flags)
{
	struct drv_acpi_object *package;
	struct drv_acpi_object *element;
	uint8_t types[2];
	enum drv_acpi_type type;
	unsigned index;
	int error;

	/* A hardware-reduced platform sleeps through the sleep control register, when it is in I/O space. */
	if ((flags & FADT_FLAG_HW_REDUCED) != 0) {
		soft_off.reduced = 1;
		if (length >= FADT_SLEEP_CONTROL_REG + GAS_LENGTH && fadt[FADT_SLEEP_CONTROL_REG] == GAS_SPACE_SYSTEM_IO) {
			soft_off.sleep_control.port = load_u32(fadt + FADT_SLEEP_CONTROL_REG + GAS_ADDRESS);
			soft_off.sleep_control.length = 1U;
		}

		/* Without such a register the platform cannot turn itself off. */
		if (soft_off.sleep_control.port == 0) {
			soft_off.sleep_control.length = 0;
			drv_acpi_os_log("ACPI: no sleep control register in I/O space; no soft-off\n");
		}
	}

	/* \_S5 is a package whose first two integers are SLP_TYPa and SLP_TYPb; without it there is no soft-off. */
	package = NULL;
	error = drv_acpi_evaluate(NULL, "\\_S5", NULL, 0, &package);
	if (error != 0) {
		drv_acpi_os_log("ACPI: no _S5 (error %d); no soft-off\n", error);
		return;
	}

	/* Takes the two sleep types; a package with one integer serves both blocks with it. */
	types[0] = 0;
	types[1] = 0;
	for (index = 0; index < 2U; index++) {
		/* A missing or non-integer element leaves that type at zero, the value most firmware gives. */
		element = drv_acpi_object_package_element(package, index);
		if (element == NULL)
			continue;
		type = drv_acpi_object_type(element);
		if (type != DRV_ACPI_TYPE_INTEGER)
			continue;
		types[index] = (uint8_t)(drv_acpi_object_integer(element) & 7U);
	}

	/* The package is no longer needed. */
	drv_acpi_object_release(package);

	/* Remembers them; from now on the soft-off needs no AML but _PTS. */
	soft_off.type_a = types[0];
	soft_off.type_b = types[1];
	soft_off.known = 1;
	drv_acpi_os_log("ACPI: S5 is SLP_TYP %u/%u\n", types[0], types[1]);
}

/*
 * Reads the FADT's reset register: a register in I/O space, one byte wide,
 * with the value that resets the machine, when the FADT's flags say it is
 * supported.  The log tells what was found.
 */
static void
read_reset_register(
	const uint8_t *fadt,
	size_t length,
	uint32_t flags)
{
	const uint8_t *gas;
	uint32_t port;

	/* A FADT too short to have the register, or one whose flags say it has none. */
	if (length < FADT_RESET_VALUE + 1U || (flags & FADT_FLAG_RESET_REG) == 0) {
		drv_acpi_os_log("ACPI: no reset register\n");
		return;
	}

	/* Only a register in I/O space, which is how the PC platforms name it. */
	gas = fadt + FADT_RESET_REG;
	if (gas[0] != GAS_SPACE_SYSTEM_IO) {
		drv_acpi_os_log("ACPI: reset register in address space %u is not used\n", (unsigned)gas[0]);
		return;
	}

	/* A register at port zero, or past the 64 KiB of I/O space, is none. */
	port = load_u32(gas + GAS_ADDRESS);
	if (port == 0U || port > 0xffffU) {
		drv_acpi_os_log("ACPI: no reset register\n");
		return;
	}

	/*
	 * known tells drv_acpi_reset_machine() that the port and the value
	 * may be written (the register is one byte wide by the
	 * specification, whatever width the structure gives).
	 */
	reset_register.port = port;
	reset_register.value = fadt[FADT_RESET_VALUE];
	reset_register.known = 1;
	drv_acpi_os_log("ACPI: reset register port 0x%x value 0x%x width %u\n", (unsigned)port, (unsigned)reset_register.value, (unsigned)gas[GAS_WIDTH]);
}

/*
 * Reads one register block's port: the 64-bit Generic Address Structure
 * when the FADT has one in I/O space, the 32-bit field otherwise.
 */
static struct register_block
fadt_block(
	const uint8_t *fadt,
	size_t length,
	unsigned legacy,
	unsigned wide,
	unsigned block_length)
{
	struct register_block block;
	uint32_t address;

	/* Takes the 32-bit field every FADT has. */
	block.port = load_u32(fadt + legacy);
	block.length = block_length;

	/* A newer FADT's structure wins when it names I/O ports. */
	if (length >= wide + GAS_LENGTH && fadt[wide] == GAS_SPACE_SYSTEM_IO) {
		address = load_u32(fadt + wide + GAS_ADDRESS);
		if (address != 0)
			block.port = address;
	}

	/* A port of zero means the block is absent, which a length of zero tells every user. */
	if (block.port == 0)
		block.length = 0;

	/* Reports the block. */
	return block;
}

/* Switches the platform into ACPI mode through the SMI command port. */
static int
enable_acpi_mode(void)
{
	uint32_t value;
	unsigned poll;
	int error;

	/* Reads PM1_CNT, whose SCI_EN says whether the platform is in ACPI mode. */
	error = drv_acpi_os_port_read(events.pm1a_control.port, 16, &value);
	if (error != 0)
		return error;

	/* A platform already in ACPI mode needs nothing. */
	if ((value & PM1_CNT_SCI_EN) != 0)
		return 0;

	/* A platform without an SMI command port, or without a value to write to it, has no way to switch. */
	if (events.smi_command == 0)
		return 0;
	if (events.acpi_enable == 0)
		return 0;

	/* Asks the firmware to switch. */
	error = drv_acpi_os_port_write(events.smi_command, 8, events.acpi_enable);
	if (error != 0)
		return error;

	/* Waits for SCI_EN to come on. */
	for (poll = 0; poll < ACPI_ENABLE_POLLS; poll++) {
		/* Reads PM1_CNT again, to see whether the firmware has switched. */
		error = drv_acpi_os_port_read(events.pm1a_control.port, 16, &value);
		if (error != 0)
			return error;

		/* Stops waiting once the firmware has switched. */
		if ((value & PM1_CNT_SCI_EN) != 0)
			break;

		/* Gives the firmware 10 microseconds more. */
		drv_acpi_os_stall(10);
	}

	/* Reports a firmware that did not switch. */
	if ((value & PM1_CNT_SCI_EN) == 0) {
		drv_acpi_os_log("ACPI: the platform did not enter ACPI mode\n");
		return ETIMEDOUT;
	}

	/* Succeeded: the platform raises SCIs from now on. */
	return 0;
}

/*
 * Reports whether a GPE number below the count has a register.  When the
 * second block's base lies above the end of the first block, the numbers
 * between them name no register in either block.
 */
static bool
gpe_present(
	unsigned gpe)
{
	unsigned first_count;

	/* Without a second block every number below the count is in the first. */
	if (events.gpe1.length == 0)
		return true;

	/* Numbers in the first block, and from the second block's base on, have a register. */
	first_count = events.gpe0.length / 2U * 8U;
	if (gpe < first_count)
		return true;
	if (gpe >= events.gpe1_base)
		return true;

	/* Reports a number in the gap between the blocks. */
	return false;
}

/* Finds the status register port and bit of a GPE. */
static void
gpe_register(
	unsigned gpe,
	uint32_t *port,
	uint8_t *bit)
{
	/* GPEs below the second block's base are in the first. */
	if (events.gpe1.length == 0 || gpe < events.gpe1_base) {
		*port = events.gpe0.port + gpe / 8U;
	} else {
		*port = events.gpe1.port + (gpe - events.gpe1_base) / 8U;
	}

	/* Reports the bit inside the register. */
	*bit = (uint8_t)(1U << (gpe % 8U));
}

/* Finds the enable register that matches a GPE's status register. */
static uint32_t
gpe_enable_port(
	unsigned gpe,
	uint32_t status_port)
{
	/* The enable registers follow the status registers of the GPE's block, the second half of it. */
	if (events.gpe1.length == 0 || gpe < events.gpe1_base)
		return status_port + events.gpe0.length / 2U;

	/* Reports the enable register of the second block. */
	return status_port + events.gpe1.length / 2U;
}

/*
 * Records and masks the GPEs that fired in one block, whose first GPE has
 * the number base.
 */
static void
record_gpe_block(
	const struct register_block *block,
	unsigned base)
{
	uint32_t status_port;
	uint32_t enable_port;
	uint8_t status_byte;
	uint8_t enable_byte;
	uint8_t fired;
	unsigned registers;
	unsigned index;
	unsigned bit;
	unsigned gpe;

	/* Visits each status register with the enable register in the block's second half. */
	registers = block->length / 2U;
	for (index = 0; index < registers; index++) {
		status_port = block->port + index;
		enable_port = status_port + registers;

		/* Reads the register's status and enable bits. */
		status_byte = port_read8(status_port);
		enable_byte = port_read8(enable_port);
		fired = (uint8_t)(status_byte & enable_byte);
		if (fired == 0)
			continue;

		/* Masks the fired events; the thread unmasks each once it is handled. */
		port_write8(enable_port, (uint8_t)(enable_byte & ~fired));

		/* Records each fired event as pending for the thread. */
		for (bit = 0; bit < 8U; bit++) {
			/* Skips a bit that did not fire. */
			if ((fired & (1U << bit)) == 0)
				continue;

			/* Skips a GPE that the interpreter does not number; only those are ever enabled. */
			gpe = base + index * 8U + bit;
			if (gpe >= events.gpe_count)
				continue;

			/* Records it, and keeps the first GPE that fires during a sleep as what woke the system. */
			events.gpe_pending[gpe / 8U] |= (uint8_t)(1U << (gpe % 8U));
			if (events.sleeping && events.woken_gpe == GPE_WOKEN_NONE)
				events.woken_gpe = (uint16_t)gpe;
		}
	}
}

/* Reads one byte-wide event register. */
static uint8_t
port_read8(
	uint32_t port)
{
	uint32_t value;
	int error;

	/* A register that cannot be read reads as zero. */
	error = drv_acpi_os_port_read(port, 8, &value);
	if (error != 0)
		return 0;

	/* Succeeded: reports the register's byte. */
	return (uint8_t)value;
}

/* Writes one byte-wide event register. */
static void
port_write8(
	uint32_t port,
	uint8_t value)
{
	/* A register that cannot be written is left alone. */
	(void)drv_acpi_os_port_write(port, 8, value);
}

/* Reads a PM1 event register (status at 0, enable at half the length), a and b ORed. */
static uint16_t
pm1_read(
	unsigned offset)
{
	uint32_t a;
	uint32_t b;
	int error;

	/* Reads the a block. */
	a = 0;
	error = drv_acpi_os_port_read(events.pm1a_event.port + offset, 16, &a);
	if (error != 0)
		a = 0;

	/* Reads the b block when there is one. */
	b = 0;
	if (events.pm1b_event.length != 0) {
		error = drv_acpi_os_port_read(events.pm1b_event.port + offset, 16, &b);
		if (error != 0)
			b = 0;
	}

	/* Reports the bits of both blocks together, as one register. */
	return (uint16_t)(a | b);
}

/* Writes a PM1 event register to both blocks. */
static void
pm1_write(
	unsigned offset,
	uint16_t value)
{
	/* Writes the a block. */
	(void)drv_acpi_os_port_write(events.pm1a_event.port + offset, 16, value);

	/* Writes the b block when there is one. */
	if (events.pm1b_event.length != 0)
		(void)drv_acpi_os_port_write(events.pm1b_event.port + offset, 16, value);
}

/* Writes bits as one into a PM1 control register, keeping its other bits. */
static void
pm1_control_set(
	const struct register_block *block,
	uint16_t bits)
{
	uint32_t value;
	int error;

	/* Reads the register, so that SCI_EN and the rest stay as they are. */
	error = drv_acpi_os_port_read(block->port, 16, &value);
	if (error != 0)
		return;

	/* Writes it back with the bits, and without SLP_EN. */
	value = (value & ~(uint32_t)PM1_CNT_SLP_EN) | bits;
	(void)drv_acpi_os_port_write(block->port, 16, value);
}

/* Writes SLP_TYP, and SLP_EN when asked, into a PM1 control register, keeping its other bits. */
static void
pm1_control_sleep(
	const struct register_block *block,
	uint8_t type,
	bool enable)
{
	uint32_t value;
	int error;

	/* Reads the register, so that SCI_EN and the rest stay as they are. */
	error = drv_acpi_os_port_read(block->port, 16, &value);
	if (error != 0)
		return;

	/* Replaces the sleep fields. */
	value &= ~(uint32_t)(PM1_CNT_SLP_TYP_MASK | PM1_CNT_SLP_EN);
	value |= (uint32_t)type << PM1_CNT_SLP_TYP_SHIFT;
	if (enable)
		value |= PM1_CNT_SLP_EN;

	/* Writes it; with SLP_EN the platform sleeps on this write. */
	(void)drv_acpi_os_port_write(block->port, 16, value);
}

/* Sets or clears a GPE's enable bit. */
static void
gpe_set_enable(
	unsigned gpe,
	bool enable)
{
	uint32_t port;
	uint8_t bit;
	uint8_t value;

	/* Finds the enable register, which follows the status registers of the GPE's block. */
	gpe_register(gpe, &port, &bit);
	port = gpe_enable_port(gpe, port);

	/* Changes the one bit. */
	value = port_read8(port);
	if (enable) {
		value |= bit;
	} else {
		value &= (uint8_t)~bit;
	}

	/* Writes the register back. */
	port_write8(port, value);
}

/* Clears a GPE's status bit, which is written as one. */
static void
gpe_clear(
	unsigned gpe)
{
	uint32_t port;
	uint8_t bit;

	/* Writes the bit alone, so that the other GPEs of the register keep their status. */
	gpe_register(gpe, &port, &bit);
	port_write8(port, bit);
}

/* Records a GPE method, _Lxx or _Exx, found below \_GPE. */
static int
gpe_method_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct drv_acpi_node *root;
	uint32_t gpe_scope;
	uint8_t kind;
	int high;
	int low;
	unsigned gpe;
	bool present;

	UNUSED_PARAMETER(depth);
	UNUSED_PARAMETER(argument);

	/* Only the methods directly below \_GPE count. */
	root = drv_acpi_root();
	gpe_scope = drv_acpi_ns_segment((const uint8_t *)"_GPE");
	if (node->object == NULL || node->object->type != DRV_ACPI_TYPE_METHOD)
		return 0;
	if (node->parent == NULL || node->parent->parent != root)
		return 0;
	if (node->parent->name != gpe_scope)
		return 0;

	/* The name is _L or _E and two hexadecimal digits. */
	kind = (uint8_t)(node->name >> 8);
	if ((node->name & 0xffU) != '_')
		return 0;
	if (kind != 'L' && kind != 'E')
		return 0;
	high = hex_digit((uint8_t)(node->name >> 16));
	low = hex_digit((uint8_t)(node->name >> 24));
	if (high < 0 || low < 0)
		return 0;

	/* Refuses a GPE the blocks do not have. */
	gpe = (unsigned)(high * 16 + low);
	if (gpe >= events.gpe_count)
		return 0;

	/* Refuses a number between the two blocks, which no register carries. */
	present = gpe_present(gpe);
	if (!present)
		return 0;

	/* Records the method; a GPE a driver handles keeps its handler. */
	if (events.gpes[gpe].kind == GPE_NONE) {
		events.gpes[gpe].kind = GPE_METHOD;
		events.gpes[gpe].method = node;

		/* An _Exx method handles an edge-triggered event, an _Lxx method a level one. */
		events.gpes[gpe].edge = 0;
		if (kind == 'E')
			events.gpes[gpe].edge = 1;
	}

	/* Goes on with the walk. */
	return 0;
}

/* Marks the GPEs the devices' _PRW say they wake the system with. */
static int
wake_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct drv_acpi_object *result;
	struct drv_acpi_object *first;
	char path[64];
	uint32_t wake_name;
	uint64_t gpe;
	int path_error;
	int error;

	UNUSED_PARAMETER(depth);
	UNUSED_PARAMETER(argument);

	/* Only a _PRW object counts. */
	wake_name = drv_acpi_ns_segment((const uint8_t *)"_PRW");
	if (node->name != wake_name)
		return 0;

	/* Evaluates it: a package whose first element is the GPE number; a failure is logged with the object's path (BUG-255). */
	error = drv_acpi_evaluate(node, NULL, NULL, 0, &result);
	if (error != 0 || result == NULL) {
		path_error = drv_acpi_node_path(node, path, sizeof(path));
		if (path_error != 0)
			path[0] = '\0';
		drv_acpi_os_log("ACPI: %s failed (error %d); its GPE is not kept for waking\n", path, error);
		return 0;
	}

	/* Marks the GPE an integer first element names; wake keeps it masked at runtime. */
	first = drv_acpi_object_package_element(result, 0);
	if (first != NULL && first->type == DRV_ACPI_TYPE_INTEGER) {
		gpe = first->value.integer;
		if (gpe < events.gpe_count)
			events.gpes[gpe].wake = 1;
	}

	/* The package is no longer needed. */
	drv_acpi_object_release(result);

	/* Goes on with the walk. */
	return 0;
}

/* Decodes one upper-case hexadecimal digit, or reports -1. */
static int
hex_digit(
	uint8_t character)
{
	/* Decodes a decimal digit. */
	if (character >= '0' && character <= '9')
		return character - '0';

	/* Decodes an upper-case letter. */
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;

	/* Reports a character that is no digit of a GPE name. */
	return -1;
}

/*
 * Handles one GPE: clears an edge event before its handler and a level
 * event after it, then unmasks it when it is still enabled.
 */
static void
process_gpe(
	unsigned gpe)
{
	struct gpe_entry *entry;
	struct drv_acpi_object *result;
	const char *trigger;
	unsigned long state;
	bool wanted;
	int error;

	/* Logs the GPE's first few handlings, with how it triggers (BUG-255). */
	entry = &events.gpes[gpe];
	if (events.gpe_logged[gpe] < GPE_LOGGED) {
		events.gpe_logged[gpe]++;
		trigger = "level";
		if (entry->edge)
			trigger = "edge";
		drv_acpi_os_log("ACPI: GPE 0x%x (%s) #%u\n", gpe, trigger, (unsigned)events.gpe_logged[gpe]);
	}

	/* An edge event is cleared first, so that a new edge is not lost. */
	if (entry->edge)
		gpe_clear(gpe);

	/* Runs the driver's handler or the GPE method; a GPE with neither stays masked from now on. */
	if (entry->kind == GPE_HANDLER && entry->handler != NULL) {
		entry->handler(gpe, entry->argument);
	} else if (entry->kind == GPE_METHOD) {
		result = NULL;
		error = drv_acpi_evaluate(entry->method, NULL, NULL, 0, &result);
		drv_acpi_object_release(result);
		if (error != 0)
			drv_acpi_os_log("ACPI: GPE 0x%x method failed (error %d)\n", gpe, error);
	} else {
		drv_acpi_os_log("ACPI: GPE 0x%x has no handler; it stays masked\n", gpe);
		entry->enabled = 0;
	}

	/* A level event is cleared once its source is handled. */
	if (!entry->edge)
		gpe_clear(gpe);

	/* Unmasks it again while it is wanted: enabled at runtime, armed during a sleep. */
	state = drv_acpi_os_event_lock();

	wanted = gpe_wanted(gpe);
	if (wanted)
		gpe_set_enable(gpe, true);

	drv_acpi_os_event_unlock(state);
}

/*
 * Tells whether a GPE should be unmasked now, under the event lock: an
 * armed GPE with a handler or a method during a sleep, an enabled one
 * otherwise.  An armed GPE with neither fires once to wake the system and
 * stays masked, so that a level source nobody clears does not storm.
 */
static bool
gpe_wanted(
	unsigned gpe)
{
	struct gpe_entry *entry;

	/* During a sleep only the armed GPEs that something handles raise SCIs. */
	entry = &events.gpes[gpe];
	if (events.sleeping) {
		/* A GPE no wake source wants stays masked. */
		if (entry->armed == 0)
			return false;

		/* A GPE nothing handles fires once and stays masked. */
		if (entry->kind == GPE_NONE)
			return false;

		/* The armed GPE wakes the system. */
		return true;
	}

	/* At runtime a GPE raises SCIs while its handling keeps it enabled. */
	if (entry->enabled)
		return true;

	/* The GPE stays masked. */
	return false;
}

/* Tells whether the interrupt recorded a GPE that the thread has not yet handled, under the event lock. */
static bool
gpe_is_pending(
	unsigned gpe)
{
	/* A recorded GPE has its bit in the pending mask. */
	if ((events.gpe_pending[gpe / 8U] & (1U << (gpe % 8U))) != 0)
		return true;

	/* The thread has nothing of the GPE to handle. */
	return false;
}

/*
 * Logs the numbers of the GPEs with a method that run at runtime (wake
 * false), or of those a _PRW keeps masked for waking only (wake true).
 */
static void
log_gpe_list(
	const char *what,
	bool wake)
{
	char line[160];
	size_t used;
	unsigned gpe;
	int written;

	/* Gathers the numbers, as many as the line holds. */
	used = 0;
	line[0] = '\0';
	for (gpe = 0; gpe < events.gpe_count; gpe++) {
		if (events.gpes[gpe].kind != GPE_METHOD || (events.gpes[gpe].wake != 0) != wake)
			continue;
		written = kern_snprintf(line + used, sizeof(line) - used, " 0x%x", gpe);
		if (written < 0 || (size_t)written >= sizeof(line) - used)
			break;
		used += (size_t)written;
	}

	/* Writes the line; an empty list says so. */
	if (used == 0) {
		drv_acpi_os_log("ACPI: %s: none\n", what);
	} else {
		drv_acpi_os_log("ACPI: %s:%s\n", what, line);
	}
}

/* Logs whether SCI_EN is set, the PM1 enable bits and how many GPEs run. */
static void
log_event_state(
	unsigned runtime)
{
	uint32_t control;
	uint16_t enable;
	int error;

	/* Reads PM1_CNT, whose SCI_EN says the platform raises SCIs. */
	control = 0;
	error = drv_acpi_os_port_read(events.pm1a_control.port, 16, &control);
	if (error != 0)
		control = 0;

	/* Reads PM1_EN, the fixed events that raise an SCI. */
	enable = pm1_read(events.pm1a_event.length / 2U);

	/* Writes the line. */
	drv_acpi_os_log("ACPI: SCI_EN %u, PM1_EN 0x%x, %u runtime GPEs\n", (unsigned)(control & PM1_CNT_SCI_EN), (unsigned)enable, runtime);
}
