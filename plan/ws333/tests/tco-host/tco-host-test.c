/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host test of the Intel TCO watchdog (src/drivers/platform/pcat/intel-tco.c).
 *
 * The driver's file is included, so that its static state and steps are
 * reachable, and the kernel calls it makes are stubs over a model of the
 * hardware: the TCO registers (a down counter of 600 ms counts that
 * resets the machine on the first run out on a hub, on the second on the
 * ICH9, unless the reset is forbidden), the hub's ACPI timer switch, the
 * ICH9's general control register, the PCI configuration of the SMBus
 * function and the LPC bridge, and a millisecond clock that sched_sleep()
 * moves on.  The scenarios check the order of the register writes, the
 * parameters, the refusals, the reloads of every CPU's stamps, the reset
 * when one CPU stops, and the halt around a sleep.
 *
 * The last line is "tco-host: PASS" or "tco-host: FAIL".
 */

#include "drivers/platform/pcat/intel-tco.c"

#include <stdarg.h>

/*
 * The C library calls the test makes, declared by hand: the kernel's
 * headers, which the driver's file brings, define types that the host's
 * library headers define differently.
 */
int printf(const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);
char *getenv(const char *name);
void *memset(void *destination, int value, size_t size);
int strcmp(const char *left, const char *right);

/* The I/O port the model's hub decodes the TCO registers at. */
#define MODEL_HUB_BASE		0x400U

/* The ICH9 model's ACPI I/O base, and so its TCO base 60h above. */
#define MODEL_ICH9_ACPI		0x600U

/* The physical bases the model's memory pages stand for. */
#define MODEL_PMC_BASE		0xfe000000ULL
#define MODEL_RCBA_BASE		0xfed1c000ULL

/* The scenario's failures. */
static unsigned failures;

/* The model's clock, in milliseconds, which is also the kernel's tick. */
static uint64_t model_now;

/* The milliseconds since the counter's last count. */
static uint64_t model_phase;

/* The kind of hub the model is. */
static enum tco_kind model_kind;

/* The TCO registers. */
static uint16_t model_count;
static uint16_t model_status1;
static uint16_t model_status2;
static uint16_t model_control1;
static uint16_t model_timer;
static unsigned model_runs;

/* Nonzero when the board's strap holds the reset forbidden. */
static unsigned model_strap;

/* Nonzero when a write ever carried NMI_NOW. */
static unsigned model_nmi_written;

/* The time of the reset, or 0 while there was none. */
static uint64_t model_reset_at;

/* How many reloads the counter has had, and the time of the last. */
static unsigned model_reloads;
static uint64_t model_reload_at;

/* The memory pages: the hub's controller page and the ICH9's configuration page. */
static uint32_t model_pmc_page[TCO_PAGE_SIZE / 4U];
static uint32_t model_rcba_page[TCO_PAGE_SIZE / 4U];

/* Nonzero when the hub's controller answers on PCI; else ACPI publishes its base. */
static unsigned model_pmc_visible;

/* Nonzero when the ACPI name of the controller's base is missing. */
static unsigned model_pmc_unpublished;

/* The function the driver binds, and the controller's. */
static uint32_t model_config[64];
static uint32_t model_pmc_config[64];
static int model_device_token;
static int model_pmc_token;

/* The boot line the stubs hand over. */
static const char *model_line;

/* The CPUs, and the threads the driver made. */
static unsigned model_cpu_count;
static unsigned model_threads_made;
static unsigned model_threads_started;
static unsigned model_threads_bound;
static int model_thread_priority;
static struct thread model_thread_pool[KERN_CPU_MAX];

/* The last line the driver logged. */
static char model_log[256];

/* Records a failed check. */
static void
check(
	int condition,
	const char *what)
{
	/* Counts and names a failure. */
	if (!condition) {
		failures++;
		printf("FAIL: %s\n", what);
	}
}

/* Reports whether the reset is forbidden in the model. */
static int
model_reset_forbidden(void)
{
	/* The hub's bit is its control bit, held by the strap. */
	if (model_kind == TCO_KIND_HUB) {
		if (model_strap)
			return 1;
		if ((model_control1 & TCO_CONTROL1_NO_REBOOT) != 0U)
			return 1;
		return 0;
	}

	/* The ICH9's is in the general control register. */
	if ((model_rcba_page[(TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK) / 4U] & TCO_ICH9_GENERAL_NO_REBOOT) != 0U)
		return 1;
	return 0;
}

/* Reports whether the model's counter counts. */
static int
model_counting(void)
{
	/* A halted counter stands. */
	if ((model_control1 & TCO_CONTROL1_HALT) != 0U)
		return 0;

	/* The hub's counter stands while the ACPI timer is stopped. */
	if (model_kind == TCO_KIND_HUB &&
	    (model_pmc_page[(TCO_PMC_TIMER_CONTROL & TCO_PAGE_MASK) / 4U] & TCO_PMC_TIMER_DISABLE) != 0U)
		return 0;

	return 1;
}

/* Moves the model's clock on to a time, counting the counter down. */
static void
model_advance(
	uint64_t until)
{
	/* One millisecond at a time. */
	while (model_now < until) {
		model_now++;
		if (model_reset_at != 0U || !model_counting())
			continue;
		model_phase++;
		if (model_phase < 600U)
			continue;
		model_phase = 0U;
		if (model_count > 0U)
			model_count--;
		if (model_count != 0U)
			continue;

		/* A run out. */
		model_runs++;
		model_status1 |= TCO_STATUS1_TIMEOUT;
		if (model_kind == TCO_KIND_HUB || model_runs == 2U) {
			model_status2 |= TCO_STATUS2_SECOND_TIMEOUT;
			if (!model_reset_forbidden()) {
				model_reset_at = model_now;
				continue;
			}
			model_runs = 0U;
		}
		model_count = (uint16_t)(model_timer & TCO_COUNT_MASK);
	}
}

/* The kernel's I/O stubs over the TCO model. */
uint16_t
kern_io_in16(
	uint16_t port)
{
	unsigned offset;

	offset = (unsigned)port - (unsigned)tco.base;
	switch (offset) {
	case TCO_RELOAD:
		return model_count;
	case TCO_STATUS1:
		return model_status1;
	case TCO_STATUS2:
		if (model_strap)
			return (uint16_t)(model_status2 | TCO_STATUS2_STRAP_NO_REBOOT);
		return model_status2;
	case TCO_CONTROL1:
		if (model_strap)
			return (uint16_t)(model_control1 | TCO_CONTROL1_NO_REBOOT);
		return model_control1;
	case TCO_TIMER:
		return model_timer;
	default:
		break;
	}
	check(0, "a read of an unknown TCO register");
	return 0xffffU;
}

void
kern_io_out16(
	uint16_t port,
	uint16_t value)
{
	unsigned offset;

	offset = (unsigned)port - (unsigned)tco.base;
	switch (offset) {
	case TCO_RELOAD:
		model_count = (uint16_t)(model_timer & TCO_COUNT_MASK);
		model_phase = 0U;
		model_runs = 0U;
		model_reloads++;
		model_reload_at = model_now;
		return;
	case TCO_STATUS1:
		model_status1 = (uint16_t)(model_status1 & ~value);
		return;
	case TCO_STATUS2:
		model_status2 = (uint16_t)(model_status2 & ~(value & TCO_STATUS2_SECOND_TIMEOUT));
		return;
	case TCO_CONTROL1:
		if ((value & TCO_CONTROL1_NMI_NOW) != 0U)
			model_nmi_written = 1U;
		model_control1 = value;
		return;
	case TCO_TIMER:
		model_timer = value;
		return;
	default:
		break;
	}
	check(0, "a write of an unknown TCO register");
}

/* Port widths the driver does not use. */
uint8_t kern_io_in8(uint16_t port) { (void)port; check(0, "an 8-bit port read"); return 0; }
uint32_t kern_io_in32(uint16_t port) { (void)port; check(0, "a 32-bit port read"); return 0; }
void kern_io_out8(uint16_t port, uint8_t value) { (void)port; (void)value; check(0, "an 8-bit port write"); }
void kern_io_out32(uint16_t port, uint32_t value) { (void)port; (void)value; check(0, "a 32-bit port write"); }

/* The memory stubs over the model's pages. */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	return *(const volatile uint32_t *)address;
}

void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

int
kern_device_map(
	uint64_t address,
	size_t size,
	unsigned attributes,
	void **mapped)
{
	check(size == TCO_PAGE_SIZE, "a map of one page");
	check(attributes == KERN_DEVICE_UNCACHED, "an uncached map");
	if (address == MODEL_PMC_BASE + 0x1000U) {
		*mapped = model_pmc_page;
		return 0;
	}
	if (address == MODEL_RCBA_BASE + 0x3000U) {
		*mapped = model_rcba_page;
		return 0;
	}
	check(0, "a map of an unknown page");
	return EINVAL;
}

/* The PCI stubs over the model's configuration. */
int
drv_pci_driver_register(
	struct drv_pci_driver *driver)
{
	check(driver->attach == tco_attach, "the driver's attach");
	check(driver->suspend == tco_suspend, "the driver's suspend");
	check(driver->resume == tco_resume, "the driver's resume");
	return 0;
}

int
drv_pci_device_config_read32(
	struct drv_pci_device *device,
	unsigned offset,
	uint32_t *value)
{
	if ((void *)device == (void *)&model_pmc_token)
		*value = model_pmc_config[offset / 4U];
	else
		*value = model_config[offset / 4U];
	return 0;
}

int
drv_pci_device_config_read8(
	struct drv_pci_device *device,
	unsigned offset,
	uint8_t *value)
{
	uint32_t word;

	(void)drv_pci_device_config_read32(device, offset & ~3U, &word);
	*value = (uint8_t)(word >> ((offset & 3U) * 8U));
	return 0;
}

struct drv_pci_device *
drv_pci_find_device(
	const struct drv_pci_address *address)
{
	check(address->device == 31U && address->function == 2U, "the controller's place");
	if (model_pmc_visible)
		return (struct drv_pci_device *)(void *)&model_pmc_token;
	return NULL;
}

uint16_t
drv_pci_device_vendor(
	const struct drv_pci_device *device)
{
	(void)device;
	return TCO_VENDOR_INTEL;
}

/* The ACPI stub: the firmware's published base of the controller. */
int
drv_acpi_evaluate_integer(
	struct drv_acpi_node *scope,
	const char *path,
	uint64_t *value)
{
	(void)scope;
	check(strcmp(path, TCO_PMC_ACPI_NAME) == 0, "the controller's ACPI name");
	if (model_pmc_unpublished)
		return ENOENT;
	*value = MODEL_PMC_BASE;
	return 0;
}

/* The kernel's boot line and log. */
void *
kern_boot_handoff(
	const char *name)
{
	check(strcmp(name, "boot.command-line") == 0, "the boot line's name");
	return (void *)(uintptr_t)model_line;
}

void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(model_log, sizeof(model_log), format, arguments);
	va_end(arguments);
	if (getenv("TCO_HOST_VERBOSE") != NULL)
		printf("  log: %s", model_log);
}

/* The scheduler's stubs over the model's clock and threads. */
uint64_t
sched_ticks(void)
{
	return model_now;
}

uint64_t
kern_ms_to_ticks(
	uint64_t milliseconds)
{
	return milliseconds;
}

void
sched_sleep(
	uint64_t timeout_tick)
{
	model_advance(timeout_tick);
}

unsigned
kern_cpu_count(void)
{
	return model_cpu_count;
}

int
kthread_create(
	void (*entry)(void *),
	void *argument,
	int priority,
	struct thread **result)
{
	check(entry == tco_thread_main, "the thread's entry");
	check((uintptr_t)argument == model_threads_made, "the thread's CPU");
	model_thread_priority = priority;
	*result = &model_thread_pool[model_threads_made];
	model_threads_made++;
	return 0;
}

int
sched_set_cpu(
	struct thread *thread,
	kern_cpu_id_t cpu,
	enum sched_cpu_affinity affinity)
{
	check(thread == &model_thread_pool[cpu], "the bound thread's CPU");
	check(affinity == SCHED_CPU_BOUND, "a bound thread");
	model_threads_bound++;
	return 0;
}

int
thread_abort_new(
	struct thread *thread)
{
	(void)thread;
	return 0;
}

void
thread_start(
	struct thread *thread)
{
	check(thread->detached == 1U, "a detached thread");
	model_threads_started++;
}

bool
kern_irq_disable(void)
{
	check(0, "an interrupt disable");
	return false;
}

void
kern_compiler_barrier(void)
{
}

/* Puts the model and the driver back to the firmware's state. */
static void
model_reset(
	enum tco_kind kind,
	const char *line)
{
	memset(&tco, 0, sizeof(tco));
	tco_generation = 1U;
	memset(tco_stamps, 0, sizeof(tco_stamps));
	tco_stop_said = 0U;
	model_now = 1000U;
	model_phase = 0U;
	model_kind = kind;
	model_count = 0x32U;
	model_status1 = 0U;
	model_status2 = 0U;
	model_control1 = (uint16_t)(TCO_CONTROL1_HALT | TCO_CONTROL1_NO_REBOOT | (1U << 12));
	model_timer = 0x4U;
	model_runs = 0U;
	model_strap = 0U;
	model_nmi_written = 0U;
	model_reset_at = 0U;
	model_reloads = 0U;
	model_reload_at = 0U;
	memset(model_pmc_page, 0, sizeof(model_pmc_page));
	memset(model_rcba_page, 0, sizeof(model_rcba_page));
	model_rcba_page[(TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK) / 4U] = TCO_ICH9_GENERAL_NO_REBOOT;
	model_pmc_visible = 0U;
	model_pmc_unpublished = 0U;
	memset(model_config, 0, sizeof(model_config));
	memset(model_pmc_config, 0, sizeof(model_pmc_config));
	if (kind == TCO_KIND_HUB) {
		model_config[TCO_SMBUS_BASE / 4U] = MODEL_HUB_BASE | 1U;
		model_config[TCO_SMBUS_CONTROL / 4U] = TCO_SMBUS_CONTROL_DECODE | 1U;
	} else {
		model_config[TCO_ICH9_ACPI_BASE / 4U] = MODEL_ICH9_ACPI | 1U;
		model_config[TCO_ICH9_ACPI_CONTROL / 4U] = TCO_ICH9_ACPI_CONTROL_DECODE;
		model_config[TCO_ICH9_CHIPSET_BASE / 4U] = (uint32_t)MODEL_RCBA_BASE | TCO_ICH9_CHIPSET_BASE_ENABLE;
	}
	model_pmc_config[TCO_PMC_BAR_LOW / 4U] = (uint32_t)MODEL_PMC_BASE | 4U;
	model_line = line;
	model_cpu_count = 12U;
	model_threads_made = 0U;
	model_threads_started = 0U;
	model_threads_bound = 0U;
	model_thread_priority = -1;
	memset(model_thread_pool, 0, sizeof(model_thread_pool));
	model_log[0] = '\0';
}

/* Binds the model's function as PCI would. */
static int
model_attach(
	enum tco_kind kind)
{
	struct drv_pci_id id;

	memset(&id, 0, sizeof(id));
	id.driver_data = (uintptr_t)kind;
	return tco_attach((struct drv_pci_device *)(void *)&model_device_token, &id);
}

/* Runs every CPU's stamp once a period for the given time, leaving one CPU out if asked. */
static void
model_run_threads(
	uint64_t milliseconds,
	unsigned silent_cpu)
{
	uint64_t end;
	unsigned cpu;

	/* A watchdog that was not armed has no period to run. */
	if (tco.period_ticks == 0U) {
		check(0, "the threads run without a period");
		return;
	}

	end = model_now + milliseconds;
	while (model_now < end && model_reset_at == 0U) {
		for (cpu = 0U; cpu < tco.cpu_count; cpu++) {
			if (cpu != silent_cpu)
				tco_stamp(cpu);
		}
		model_advance(model_now + tco.period_ticks);
	}
}

/* The boot parameter's reading. */
static void
test_parameters(void)
{
	uint32_t seconds;
	int error;

	seconds = 0U;
	error = tco_parameter_seconds("watchdog=60", TCO_PARAMETER_SECONDS, &seconds);
	check(error == 0 && seconds == 60U, "watchdog=60");
	error = tco_parameter_seconds("boot0=x  watchdog=45\tkmsg=quiet", TCO_PARAMETER_SECONDS, &seconds);
	check(error == 0 && seconds == 45U, "watchdog=45 among others");
	error = tco_parameter_seconds("watchdog=off", TCO_PARAMETER_SECONDS, &seconds);
	check(error == ENOENT, "watchdog=off");
	error = tco_parameter_seconds("watchdog=offer", TCO_PARAMETER_SECONDS, &seconds);
	check(error == EINVAL, "watchdog=offer");
	error = tco_parameter_seconds("watchdog=6a", TCO_PARAMETER_SECONDS, &seconds);
	check(error == EINVAL, "watchdog=6a");
	error = tco_parameter_seconds("watchdog=", TCO_PARAMETER_SECONDS, &seconds);
	check(error == EINVAL, "watchdog= empty");
	error = tco_parameter_seconds("watchdog=1234567", TCO_PARAMETER_SECONDS, &seconds);
	check(error == EINVAL, "seven digits");
	error = tco_parameter_seconds("watchdog.stop=30", TCO_PARAMETER_SECONDS, &seconds);
	check(error == ENOENT, "watchdog.stop= is not watchdog=");
	error = tco_parameter_seconds("nowatchdog=30", TCO_PARAMETER_SECONDS, &seconds);
	check(error == ENOENT, "a name inside a word");
	error = tco_parameter_seconds("watchdog=60 watchdog.stop=90", TCO_PARAMETER_STOP, &seconds);
	check(error == 0 && seconds == 90U, "watchdog.stop=90");
	error = tco_parameter_seconds(NULL, TCO_PARAMETER_SECONDS, &seconds);
	check(error == ENOENT, "no line");
}

/* The counts each kind loads. */
static void
test_counts(void)
{
	check(tco_count_for(TCO_KIND_HUB, 30U) == 50U, "hub 30 s is 50 counts");
	check(tco_count_for(TCO_KIND_HUB, 60U) == 100U, "hub 60 s is 100 counts");
	check(tco_count_for(TCO_KIND_HUB, 4U) == 7U, "hub 4 s is 7 counts");
	check(tco_count_for(TCO_KIND_HUB, 600U) == 1000U, "hub 600 s is 1000 counts");
	check(tco_count_for(TCO_KIND_ICH9, 60U) == 50U, "ICH9 60 s is 50 counts a run");
	check(tco_count_for(TCO_KIND_ICH9, 4U) == 4U, "ICH9 4 s is 4 counts a run");
}

/* No parameter: nothing is bound or armed. */
static void
test_off(void)
{
	int error;

	model_reset(TCO_KIND_HUB, "kmsg=quiet");
	error = model_attach(TCO_KIND_HUB);
	check(error == ENODEV, "no parameter leaves the function unbound");
	error = drv_intel_tco_start();
	check(error == ENODEV, "no parameter arms nothing");
	check(model_control1 == (TCO_CONTROL1_HALT | TCO_CONTROL1_NO_REBOOT | (1U << 12)), "no parameter leaves the control alone");

	model_reset(TCO_KIND_HUB, "watchdog=3");
	error = model_attach(TCO_KIND_HUB);
	check(error == ENODEV, "3 s is refused");
	model_reset(TCO_KIND_HUB, "watchdog=601");
	error = model_attach(TCO_KIND_HUB);
	check(error == ENODEV, "601 s is refused");
}

/* The hub: armed with the controller hidden, kept alive, reset when one CPU stops. */
static void
test_hub(void)
{
	uint64_t stopped;
	int error;

	model_reset(TCO_KIND_HUB, "watchdog=30");
	model_status2 = TCO_STATUS2_SECOND_TIMEOUT;
	model_control1 |= TCO_CONTROL1_NMI_NOW;
	model_pmc_page[(TCO_PMC_TIMER_CONTROL & TCO_PAGE_MASK) / 4U] = TCO_PMC_TIMER_DISABLE;
	error = model_attach(TCO_KIND_HUB);
	check(error == 0, "hub attach");
	check(tco.base == MODEL_HUB_BASE, "hub TCO base from the SMBus function");
	error = drv_intel_tco_start();
	check(error == 0, "hub start");
	check((model_control1 & TCO_CONTROL1_HALT) == 0U, "hub counter runs");
	check((model_control1 & TCO_CONTROL1_NO_REBOOT) == 0U, "hub reset allowed");
	check((model_control1 & (1U << 12)) != 0U, "hub keeps TCO_LOCK");
	check((model_timer & TCO_COUNT_MASK) == 50U, "hub loads 50 counts");
	check((model_status2 & TCO_STATUS2_SECOND_TIMEOUT) == 0U, "hub clears the last timeout");
	check((model_pmc_page[(TCO_PMC_TIMER_CONTROL & TCO_PAGE_MASK) / 4U] & TCO_PMC_TIMER_DISABLE) == 0U,
	      "hub starts the ACPI timer through the published base");
	check(model_nmi_written == 0U, "hub never writes NMI_NOW");
	check(model_threads_made == 12U && model_threads_bound == 12U && model_threads_started == 12U,
	      "hub one bound thread per CPU");
	check(model_thread_priority == SCHED_PRIOR_HIGH, "hub threads at the high priority");
	check(tco.period_ticks == 7500U, "hub stamps every 7.5 s");
	check(tco.stop_tick == 0U, "hub no stop");
	check(model_reset_at == 0U, "hub no reset while starting");

	/* Ten minutes of every CPU stamping: no reset. */
	model_run_threads(600000U, KERN_CPU_MAX);
	check(model_reset_at == 0U, "hub ten minutes without a reset");
	check(model_reloads > 70U, "hub reloads while every CPU stamps");

	/* CPU 7 stops: the reset follows within the timeout and a period. */
	stopped = model_now;
	model_run_threads(120000U, 7U);
	check(model_reset_at != 0U, "hub resets when one CPU stops");
	check(model_reset_at - stopped <= 30000U, "hub resets within the timeout of the stop");
	check(model_reset_at - model_reload_at >= 29400U, "hub does not reset early");
	check(model_reset_at - model_reload_at <= 30600U, "hub resets 30 s after the last reload");
}

/* The hub with the controller on PCI, and the sleep's halt and arm. */
static void
test_hub_sleep(void)
{
	uint64_t before;
	int error;

	model_reset(TCO_KIND_HUB, "watchdog=60 watchdog.stop=120");
	model_pmc_visible = 1U;
	error = model_attach(TCO_KIND_HUB);
	check(error == 0, "sleep attach");
	before = model_now;
	error = drv_intel_tco_start();
	check(error == 0, "sleep start");
	check(tco.stop_tick == before + TCO_COUNT_CHECK_MS + 120000U, "the stop's tick");

	/* The sleep halts the counter; an hour passes without a reset. */
	error = tco_suspend(NULL);
	check(error == 0 && (model_control1 & TCO_CONTROL1_HALT) != 0U, "suspend halts");
	model_pmc_page[(TCO_PMC_TIMER_CONTROL & TCO_PAGE_MASK) / 4U] = TCO_PMC_TIMER_DISABLE;
	model_advance(model_now + 3600000U);
	check(model_reset_at == 0U, "no reset while asleep");

	/* The wake arms it again with the timer running. */
	error = tco_resume(NULL);
	check(error == 0 && (model_control1 & TCO_CONTROL1_HALT) == 0U, "resume runs the counter");
	check((model_pmc_page[(TCO_PMC_TIMER_CONTROL & TCO_PAGE_MASK) / 4U] & TCO_PMC_TIMER_DISABLE) == 0U,
	      "resume starts the ACPI timer");
	check(model_count == 100U, "resume reloads");
	model_run_threads(300000U, KERN_CPU_MAX);
	check(model_reset_at == 0U, "after the sleep the stamps keep it");
	model_advance(model_now + 70000U);
	check(model_reset_at != 0U, "after the sleep a stop resets");
}

/* The hub's refusals: the strap, and a counter whose clock cannot be started. */
static void
test_hub_refusals(void)
{
	int error;

	model_reset(TCO_KIND_HUB, "watchdog=30");
	model_strap = 1U;
	error = model_attach(TCO_KIND_HUB);
	check(error == 0, "strap attach");
	error = drv_intel_tco_start();
	check(error == ENODEV, "strap refuses");
	check((model_control1 & TCO_CONTROL1_HALT) != 0U, "strap leaves the counter halted");
	check(model_threads_made == 0U, "strap makes no thread");

	model_reset(TCO_KIND_HUB, "watchdog=30");
	model_pmc_unpublished = 1U;
	model_pmc_page[(TCO_PMC_TIMER_CONTROL & TCO_PAGE_MASK) / 4U] = TCO_PMC_TIMER_DISABLE;
	error = model_attach(TCO_KIND_HUB);
	check(error == 0, "stopped clock attach");
	error = drv_intel_tco_start();
	check(error == EIO, "a counter that does not count is refused");
	check((model_control1 & TCO_CONTROL1_HALT) != 0U, "the refused counter is halted");
	check((model_control1 & TCO_CONTROL1_NO_REBOOT) != 0U, "the refused counter's reset is forbidden again");
	check(model_threads_made == 0U, "the refused counter has no thread");

	model_reset(TCO_KIND_HUB, "watchdog=30");
	model_config[TCO_SMBUS_CONTROL / 4U] = 0U;
	error = model_attach(TCO_KIND_HUB);
	check(error == ENODEV, "an undecoded TCO base is refused");
}

/* The ICH9: the general control's bit, the TCO base 60h above the ACPI base, the reset on the second run out. */
static void
test_ich9(void)
{
	uint64_t stopped;
	int error;

	model_reset(TCO_KIND_ICH9, "watchdog=60");
	model_cpu_count = 2U;
	error = model_attach(TCO_KIND_ICH9);
	check(error == 0, "ICH9 attach");
	error = drv_intel_tco_start();
	check(error == 0, "ICH9 start");
	check(tco.base == MODEL_ICH9_ACPI + 0x60U, "ICH9 TCO base");
	check((model_rcba_page[(TCO_ICH9_GENERAL_CONTROL & TCO_PAGE_MASK) / 4U] & TCO_ICH9_GENERAL_NO_REBOOT) == 0U,
	      "ICH9 reset allowed");
	check((model_timer & TCO_COUNT_MASK) == 50U, "ICH9 loads 50 counts a run");
	model_run_threads(300000U, KERN_CPU_MAX);
	check(model_reset_at == 0U, "ICH9 kept alive");
	stopped = model_now;
	model_advance(model_now + 120000U);
	check(model_reset_at != 0U, "ICH9 resets without reloads");
	check(model_reset_at - stopped <= 60000U, "ICH9 resets within the timeout of the stop");
	check(model_reset_at - model_reload_at >= 59400U, "ICH9 does not reset early");
	check(model_reset_at - model_reload_at <= 61200U, "ICH9 resets 60 s after the last reload");

	model_reset(TCO_KIND_ICH9, "watchdog=60");
	model_config[TCO_ICH9_CHIPSET_BASE / 4U] = 0U;
	error = model_attach(TCO_KIND_ICH9);
	check(error == 0, "ICH9 without RCBA attach");
	error = drv_intel_tco_start();
	check(error == ENODEV, "ICH9 without its configuration space is refused");
}

int
main(void)
{
	test_parameters();
	test_counts();
	test_off();
	test_hub();
	test_hub_sleep();
	test_hub_refusals();
	test_ich9();
	if (failures == 0U) {
		printf("tco-host: PASS\n");
		return 0;
	}
	printf("tco-host: FAIL (%u)\n", failures);
	return 1;
}
