/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Dell laptop's thermal provider over ACPI WMI.
 *
 * The embedded controller runs the fan by itself; the operating system
 * reads the fans' speeds, picks the BIOS thermal mode, and, in a kernel
 * built with CONFIG_DELL_SMM_BOOST, raises a fan's state through the SMM
 * fan call.  All of it goes through the firmware's AML: the provider makes
 * no port or memory access of its own, and only the mode call and the SMM
 * fan calls enter System Management Mode.
 *
 * The thermal core calls the receiver's apply and the fans' boost from its
 * strategy thread, which may sleep, and the receiver's poll and the fans'
 * read from its poll thread.  The provider lets one call that enters the
 * SMM start a second at most, sleeping in the strategy thread rather than
 * refusing.  A piece of the firmware interface whose probe failed is
 * probed again from the poll, once a second, three times at most.
 */

#include <drivers/acpi/acpi.h>
#include <drivers/acpi/dell.h>
#include <drivers/acpi/wmi.h>
#include <kern/kcrt.h>
#include <kern/thermal.h>
#include <uapi/errno.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aml-os.h"

/* Marks a parameter a callback's contract passes but this one does not use. */
#define UNUSED_PARAMETER(parameter)	((void)(parameter))

/* Whether the fans' boost through the SMM fan call is built in. */
#ifndef CONFIG_DELL_SMM_BOOST
#define CONFIG_DELL_SMM_BOOST	0
#endif

/*
 * The GUIDs of the blocks the provider uses: the Dell WMI descriptor (a
 * data block), the SMBIOS calling interface, the DDV interface and the
 * SMM calls (method blocks), and the mapping device whose _WDG is a method
 * that lists the SMM calls' block.
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1), the _WDG
 * of \_SB.AMW0, \_SB.AMWV and \_SB.DIAG.
 */
#define DELL_GUID_DESCRIPTOR	"8D9DDCBC-A997-11DA-B012-B622A1EF5492"
#define DELL_GUID_SMBIOS	"A80593CE-A997-11DA-B012-B622A1EF5492"
#define DELL_GUID_DDV		"8A42EA14-4F2A-FD45-6422-0087F7A7E608"
#define DELL_GUID_SMM		"F1DDEE52-063C-4784-A11E-8A06684B9B01"
#define DELL_SMM_DEVICE		"\\_SB_.DIAG"

/*
 * The Dell WMI descriptor: "DELL" and " WMI" in its first eight bytes,
 * and the largest buffer the WMI methods take as its fourth dword.
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.AMW0.WQAA.
 */
#define DESCRIPTOR_SIGNATURE		"DELL WMI"
#define DESCRIPTOR_SIGNATURE_LENGTH	8U
#define DESCRIPTOR_BUFFER_SIZE		0x0CU
#define DESCRIPTOR_LENGTH		0x14U

/*
 * The DDV methods: the interface's version (3) and the fan information,
 * each given four bytes of zeros.
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1),
 * \_SB.AMWV.WMDV (methods 0x12 and 0x20, and the three input bytes the
 * method reads).
 */
#define DDV_VERSION		0x12U
#define DDV_FANS		0x20U
#define DDV_VERSION_KNOWN	3U
#define DDV_INPUT_SIZE		4U

/*
 * The firmware's flag that its AML may read the embedded controller
 * directly (otherwise each byte goes through System Management Mode), and
 * its variable that mirrors the last thermal mode asked for.
 *
 * Values referenced: the Dell Latitude 5330's DSDT (BIOS 1.31.1), \ECRD
 * and \ODV0.
 */
#define DELL_EC_READY		"\\ECRD"
#define DELL_MODE_VARIABLE	"\\ODV0"

/*
 * The method IDs of the SMBIOS call and of the SMM call.  The 5330's AML
 * does not look at them.
 *
 * Values referenced: Linux 6.12 drivers/platform/x86/dell/dell-smbios-wmi.c
 * (the method ID of the SMBIOS call) and drivers/hwmon/dell-smm-hwmon.c
 * (the method ID of the legacy SMM call).
 */
#define SMBIOS_METHOD		1U
#define SMM_METHOD		1U

/* The shortest time between two calls that enter the SMM, in 100-ns units. */
#define SMI_SPACING		10000000ULL
#define SMI_SPACING_MS		1000ULL

/* How many polls probe a failed piece again. */
#define PROBE_RETRIES		3U

/*
 * How many fan reads in a row at or below the speed before a boost tell
 * that the firmware put the fan back.
 */
#define BOOST_SLOW_READS	3U

/* The pieces of the firmware interface, as bits. */
#define PIECE_DESCRIPTOR	0x1U
#define PIECE_MODE		0x2U
#define PIECE_FANS		0x4U
#define PIECE_SMM		0x8U

/*
 * One fan the provider registered with the thermal core.
 *
 * index is the fan's place in the DDV fan information and its number in
 * the SMM fan call.  raised and before belong to the strategy thread;
 * slow_reads and seen_generation to the poll thread.  last_rpm, boosted,
 * rpm_before, generation and boost_off pass between them: at each change
 * of the boost the strategy thread stores rpm_before and boosted, then
 * moves generation with a release store; the poll thread loads generation
 * with an acquire load before it reads the other two, and starts watching
 * anew when it changed.
 */
struct dell_fan {
	unsigned index;
	bool raised;
	uint32_t before;
	uint32_t last_rpm;
	uint32_t boosted;
	uint32_t rpm_before;
	uint32_t generation;
	uint32_t seen_generation;
	unsigned slow_reads;
	uint32_t boost_off;
};

/*
 * The firmware interface the provider found.
 *
 * The block pointers are filled before the bit of their piece is set in
 * found with a release store, and a reader loads found with an acquire
 * load before it uses one.  pending is the pieces a probe could not finish
 * and the poll probes again; it is written by the attach and then only by
 * the poll thread, and read by apply to tell "not yet" from "never".  mode
 * is the BIOS thermal mode last set (or read at boot), or
 * DRV_ACPI_DELL_MODE_NONE.  last_smi is the strategy thread's.
 */
struct dell_provider {
	const struct drv_acpi_wmi_block *descriptor;
	const struct drv_acpi_wmi_block *smbios;
	const struct drv_acpi_wmi_block *ddv;
	const struct drv_acpi_wmi_block *smm;
	uint32_t found;
	uint32_t pending;
	unsigned retries;
	uint32_t mode;
	uint64_t last_smi;
	struct dell_fan fans[DRV_ACPI_DELL_FANS_MAX];
	unsigned fan_count;
};

static int dell_apply(void *context, const struct kern_thermal_strategy *strategy, uint32_t *flags);
static void dell_poll(void *context);
static int dell_firmware_name(void *context, char *name, size_t size);
static int dell_fan_read(void *context, struct kern_thermal_fan_state *state);
#if CONFIG_DELL_SMM_BOOST
static int dell_fan_boost(void *context, uint32_t level);
#endif

/*
 * What the provider does for the thermal core as the receiver of the
 * strategy: it picks the BIOS thermal mode, and its poll probes again what
 * could not be probed at boot.
 */
static const struct kern_thermal_receiver_ops dell_receiver_ops = {
	.apply = dell_apply,
	.poll = dell_poll,
	.firmware_name = dell_firmware_name,
};

/* What the provider does for the thermal core for each fan. */
static const struct kern_thermal_fan_ops dell_fan_ops = {
	.read = dell_fan_read,
#if CONFIG_DELL_SMM_BOOST
	.boost = dell_fan_boost,
#endif
};

/* The fans' names in the thermal core, by their place in the DDV fan information. */
static const char *const dell_fan_names[DRV_ACPI_DELL_FANS_MAX] = {
	"dell-fan0",
	"dell-fan1",
	"dell-fan2",
	"dell-fan3",
};

/*
 * The provider, for the life of the system.  It is filled by
 * drv_acpi_dell_attach() and the poll's probes as struct dell_provider
 * says, and is all zeros (nothing found) on a machine that is not a Dell
 * laptop.
 */
static struct dell_provider dell;

static uint32_t dell_probe(uint32_t pieces);
static int dell_probe_descriptor(void);
static int dell_probe_mode(void);
static int dell_probe_fans(void);
#if CONFIG_DELL_SMM_BOOST
static int dell_probe_smm(void);
#endif
static int dell_fans_read(struct drv_acpi_dell_fan *fans, unsigned *count);
static int dell_ddv_call(uint32_t method, struct drv_acpi_object **result);
static uint32_t dell_mode_read(void);
static int dell_mode_set(uint32_t mode);
#if CONFIG_DELL_SMM_BOOST
static int dell_smm_call(const uint8_t request[DRV_ACPI_DELL_SMM_SIZE], uint32_t *eax);
static int dell_fan_state_get(struct dell_fan *fan, uint32_t *state);
static int dell_fan_state_set(struct dell_fan *fan, uint32_t state);
static void dell_boost_refuse(struct dell_fan *fan, const char *why, int error);
static void dell_boost_publish(struct dell_fan *fan, bool raised);
static void dell_boost_watch(struct dell_fan *fan, uint32_t rpm);
#endif
static void dell_smi_wait(void);
static void dell_smi_done(void);
static uint32_t dell_found(void);
static void dell_found_add(uint32_t pieces);
static void dell_found_remove(uint32_t pieces);

/*
 * Attaches the Dell provider when the WMI layer lists the Dell WMI
 * descriptor.
 *
 * Reads the descriptor, then probes the thermal mode call, the DDV fans
 * and (with CONFIG_DELL_SMM_BOOST) the SMM fan calls, registers the fans
 * found, and registers the receiver of the strategy.  Nothing here enters
 * System Management Mode.  Thread context, after drv_acpi_wmi_attach().
 * Returns 0, or ENODEV on a machine without a Dell WMI descriptor.
 */
int
drv_acpi_dell_attach(void)
{
	const struct drv_acpi_wmi_block *descriptor;
	uint32_t pieces;
	uint32_t found;
	int error;

	/* Finds the descriptor; a machine without one is not a Dell laptop. */
	error = drv_acpi_wmi_find(DELL_GUID_DESCRIPTOR, 0, &descriptor);
	if (error != 0)
		return ENODEV;
	dell.descriptor = descriptor;
	dell.mode = DRV_ACPI_DELL_MODE_NONE;

	/* Probes every piece; those that failed are probed again from the poll. */
	pieces = PIECE_DESCRIPTOR | PIECE_MODE | PIECE_FANS;
	if (CONFIG_DELL_SMM_BOOST)
		pieces |= PIECE_SMM;
	pieces = dell_probe(pieces);
	__atomic_store_n(&dell.pending, pieces, __ATOMIC_RELEASE);

	/* Leaves a descriptor that is not Dell's alone. */
	found = dell_found();
	if (pieces == 0U && found == 0U) {
		drv_acpi_os_log("dell: the WMI descriptor is not Dell's\n");
		return ENODEV;
	}

	/* Registers the receiver of the strategy. */
	error = kern_thermal_receiver_register("dell-wmi", &dell_receiver_ops, &dell);
	if (error != 0) {
		drv_acpi_os_log("dell: the thermal core refused the receiver (error %d)\n", error);
		return error;
	}

	/* Succeeded: the provider serves what it found. */
	return 0;
}

/* Picks the BIOS thermal mode for a strategy (the strategy thread). */
static int
dell_apply(
	void *context,
	const struct kern_thermal_strategy *strategy,
	uint32_t *flags)
{
	uint32_t pending;
	uint32_t current;
	uint32_t found;
	uint32_t mode;
	int error;

	UNUSED_PARAMETER(context);
	UNUSED_PARAMETER(flags);

	/* Answers "not yet" while the probe of the mode call is retried, and "nothing here" after. */
	found = dell_found();
	if ((found & PIECE_MODE) == 0U) {
		pending = __atomic_load_n(&dell.pending, __ATOMIC_ACQUIRE);
		if ((pending & (PIECE_DESCRIPTOR | PIECE_MODE)) != 0U)
			return EAGAIN;
		return EOPNOTSUPP;
	}

	/* FIRMWARE writes nothing. */
	mode = drv_acpi_dell_mode_for(strategy);
	if (mode == DRV_ACPI_DELL_MODE_NONE)
		return 0;

	/* Leaves the firmware alone when it already runs the mode. */
	current = __atomic_load_n(&dell.mode, __ATOMIC_RELAXED);
	if (current == mode)
		return 0;

	/* Sets the mode, at most one call into the SMM a second. */
	dell_smi_wait();
	error = dell_mode_set(mode);
	dell_smi_done();
	if (error != 0)
		return error;

	/* Succeeded: the firmware runs the mode. */
	return 0;
}

/* Probes again the pieces that failed, once a second and three times at most (the poll thread). */
static void
dell_poll(
	void *context)
{
	uint32_t pending;

	UNUSED_PARAMETER(context);

	/* Nothing to probe, or the retries are spent. */
	pending = __atomic_load_n(&dell.pending, __ATOMIC_ACQUIRE);
	if (pending == 0U)
		return;

	/* Lists the WMI blocks again, for a mapping device that failed, then probes the pieces. */
	dell.retries++;
	(void)drv_acpi_wmi_attach();
	pending = dell_probe(pending);

	/* Gives up on what still fails after the last retry. */
	if (pending != 0U && dell.retries >= PROBE_RETRIES) {
		drv_acpi_os_log("dell: gave up probing 0x%x after %u retries\n", (unsigned)pending, dell.retries);
		pending = 0;
	}

	/* Publishes what is still to probe. */
	__atomic_store_n(&dell.pending, pending, __ATOMIC_RELEASE);
}

/* Names the BIOS thermal mode in force (a GET of the strategy). */
static int
dell_firmware_name(
	void *context,
	char *name,
	size_t size)
{
	const char *text;
	uint32_t mode;
	size_t length;

	UNUSED_PARAMETER(context);

	/* Names the mode last set or read at boot; one not known has no name. */
	mode = __atomic_load_n(&dell.mode, __ATOMIC_RELAXED);
	text = drv_acpi_dell_mode_name(mode);
	if (text == NULL)
		return ENOENT;

	/* Refuses a room too small for the name. */
	length = kern_strlen(text);
	if (length + 1U > size)
		return E2BIG;

	/* Copies the name with its NUL. */
	kern_memcpy(name, text, length + 1U);

	/* Succeeded: name holds the mode's name. */
	return 0;
}

/* Reads one fan's speed from the DDV fan information (the poll thread). */
static int
dell_fan_read(
	void *context,
	struct kern_thermal_fan_state *state)
{
	struct drv_acpi_dell_fan fans[DRV_ACPI_DELL_FANS_MAX];
	struct dell_fan *fan;
	unsigned count;
	int error;
#if CONFIG_DELL_SMM_BOOST
	uint32_t found;
	uint32_t off;
#endif

	/* The fan the context names. */
	fan = context;

	/* Reads the fan information. */
	error = dell_fans_read(fans, &count);
	if (error != 0)
		return error;

	/* Refuses a fan the firmware no longer lists. */
	if (fan->index >= count)
		return ENOENT;

	/* Reports the speed; the firmware gives no level. */
	state->flags = KERN_THERMAL_FAN_HAS_RPM;
	state->rpm = fans[fan->index].rpm;
	__atomic_store_n(&fan->last_rpm, state->rpm, __ATOMIC_RELAXED);
#if CONFIG_DELL_SMM_BOOST

	/* Tells whether a boost still holds, and offers the SMM fan states while it can. */
	dell_boost_watch(fan, state->rpm);
	found = dell_found();
	off = __atomic_load_n(&fan->boost_off, __ATOMIC_RELAXED);
	if ((found & PIECE_SMM) != 0U && off == 0U)
		state->boost_count = DRV_ACPI_DELL_FAN_STATE_HIGH;
#endif

	/* Succeeded: state holds the fan's speed. */
	return 0;
}

#if CONFIG_DELL_SMM_BOOST
/*
 * Sets a fan's boost through the SMM fan call (the strategy thread).
 *
 * A boost is a state the fan does not run below: when the fan already runs
 * at or above it nothing is written; otherwise the state is set and read
 * back.  A boost of 0 sets back the state the fan ran at before the first
 * raise.  A fan whose state does not read back as set, or whose speed
 * shows the firmware put it back, is boosted no more.
 */
static int
dell_fan_boost(
	void *context,
	uint32_t level)
{
	struct dell_fan *fan;
	uint32_t target;
	uint32_t state;
	uint32_t found;
	uint32_t off;
	bool raised;
	int error;

	/* The fan the context names. */
	fan = context;

	/* Refuses a machine without the SMM fan calls, and a level past the highest state. */
	found = dell_found();
	if ((found & PIECE_SMM) == 0U)
		return EOPNOTSUPP;
	if (level > DRV_ACPI_DELL_FAN_STATE_HIGH)
		return EINVAL;

	/* A fan found not to hold a boost takes none. */
	off = __atomic_load_n(&fan->boost_off, __ATOMIC_RELAXED);
	if (off != 0U) {
		if (level == 0U)
			return 0;
		return EOPNOTSUPP;
	}

	/* A fan the provider did not raise has nothing to put back. */
	if (level == 0U && !fan->raised)
		return 0;

	/* Enters the SMM at most once a second for the whole change. */
	dell_smi_wait();

	/* Puts back the state the fan ran at before the provider raised it. */
	if (level == 0U) {
		error = dell_fan_state_set(fan, fan->before);
		dell_smi_done();
		if (error != 0) {
			dell_boost_refuse(fan, "could not be put back", error);
			return error;
		}

		/* Succeeded: the fan runs as the firmware chose before the raise. */
		dell_boost_publish(fan, false);
		return 0;
	}

	/* A fan not raised yet: reads the state the firmware chose. */
	if (!fan->raised) {
		error = dell_fan_state_get(fan, &state);
		if (error != 0) {
			dell_smi_done();
			dell_boost_refuse(fan, "state could not be read", error);
			return error;
		}

		/* Leaves a fan that already runs at or above the level alone. */
		if (state >= level) {
			dell_smi_done();
			return 0;
		}

		/* Keeps the state to put back and to stay above. */
		fan->before = state;
	}

	/* The state to set: the level, never below what the fan ran at before. */
	target = level;
	if (fan->before > target)
		target = fan->before;

	/* Sets it and reads it back. */
	error = dell_fan_state_set(fan, target);
	if (error == 0)
		error = dell_fan_state_get(fan, &state);
	dell_smi_done();
	if (error != 0) {
		dell_boost_refuse(fan, "state could not be set", error);
		return error;
	}

	/* Refuses a fan whose state did not take. */
	if (state != target) {
		dell_boost_refuse(fan, "state did not take", EIO);
		return EIO;
	}

	/* Tells the poll thread the new boost; a state no higher than before is no raise. */
	raised = false;
	if (target > fan->before)
		raised = true;
	dell_boost_publish(fan, raised);

	/* Succeeded: the fan runs at the state set. */
	return 0;
}
#endif

/*
 * Probes the pieces named and reports those whose probe failed for a
 * reason that may pass (an AML error, the embedded controller not ready).
 * A piece absent from the firmware, or one it answers in a way the
 * provider does not know, is neither found nor failed.
 */
static uint32_t
dell_probe(
	uint32_t pieces)
{
	uint32_t failed;
	int error;

	/* Reads the descriptor first; nothing else is probed before it reads as Dell's. */
	failed = 0;
	if ((pieces & PIECE_DESCRIPTOR) != 0U) {
		error = dell_probe_descriptor();
		if (error == EAGAIN)
			return pieces;
		if (error != 0)
			return 0;
	}

	/* Probes the thermal mode call. */
	if ((pieces & PIECE_MODE) != 0U) {
		error = dell_probe_mode();
		if (error == EAGAIN)
			failed |= PIECE_MODE;
	}

	/* Probes the DDV fans. */
	if ((pieces & PIECE_FANS) != 0U) {
		error = dell_probe_fans();
		if (error == EAGAIN)
			failed |= PIECE_FANS;
	}
#if CONFIG_DELL_SMM_BOOST

	/* Probes the SMM fan calls. */
	if ((pieces & PIECE_SMM) != 0U) {
		error = dell_probe_smm();
		if (error == EAGAIN)
			failed |= PIECE_SMM;
	}
#endif

	/* Reports the pieces to probe again. */
	return failed;
}

/*
 * Reads the Dell WMI descriptor.  Returns 0 for Dell's, ENODEV for
 * another, or EAGAIN when it could not be read.
 */
static int
dell_probe_descriptor(void)
{
	struct drv_acpi_object *result;
	const uint8_t *bytes;
	enum drv_acpi_type type;
	uint32_t room;
	size_t length;
	int compared;
	int error;

	/* Queries the descriptor. */
	result = NULL;
	error = drv_acpi_wmi_query(dell.descriptor, 0, &result);
	if (error != 0) {
		drv_acpi_os_log("dell: the WMI descriptor could not be read (error %d)\n", error);
		return EAGAIN;
	}

	/* Refuses an answer that is not a buffer. */
	if (result == NULL)
		return ENODEV;
	type = drv_acpi_object_type(result);
	if (type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_object_release(result);
		return ENODEV;
	}

	/* Refuses a buffer shorter than the descriptor. */
	bytes = drv_acpi_object_buffer(result, &length);
	if (length < DESCRIPTOR_LENGTH) {
		drv_acpi_object_release(result);
		return ENODEV;
	}

	/* Takes the signature and the largest buffer, then gives the answer back. */
	compared = kern_memcmp(bytes, DESCRIPTOR_SIGNATURE, DESCRIPTOR_SIGNATURE_LENGTH);
	room = (uint32_t)bytes[DESCRIPTOR_BUFFER_SIZE];
	room |= (uint32_t)bytes[DESCRIPTOR_BUFFER_SIZE + 1U] << 8;
	room |= (uint32_t)bytes[DESCRIPTOR_BUFFER_SIZE + 2U] << 16;
	room |= (uint32_t)bytes[DESCRIPTOR_BUFFER_SIZE + 3U] << 24;
	drv_acpi_object_release(result);

	/* Refuses another vendor's descriptor. */
	if (compared != 0)
		return ENODEV;

	/* Refuses methods that cannot take the SMBIOS call's buffer. */
	if (room < DRV_ACPI_DELL_SMBIOS_SIZE) {
		drv_acpi_os_log("dell: the WMI methods take %u bytes at most\n", (unsigned)room);
		return ENODEV;
	}

	/* Succeeded: the descriptor is Dell's. */
	dell_found_add(PIECE_DESCRIPTOR);
	return 0;
}

/*
 * Finds the SMBIOS calling interface and reads the thermal mode the
 * firmware mirrors.  Returns 0, or ENOENT when the firmware has no SMBIOS
 * block.
 */
static int
dell_probe_mode(void)
{
	const struct drv_acpi_wmi_block *block;
	uint32_t mode;
	int error;

	/* Finds the SMBIOS block. */
	error = drv_acpi_wmi_find(DELL_GUID_SMBIOS, 0, &block);
	if (error != 0)
		return ENOENT;

	/*
	 * Takes the mode the firmware's variable holds as the one in force, so
	 * that a strategy for the same mode writes nothing.  The firmware sets
	 * the variable before it runs the call, so this is not proof that the
	 * mode was taken, only the last one asked for.
	 */
	mode = dell_mode_read();
	__atomic_store_n(&dell.mode, mode, __ATOMIC_RELAXED);

	/* Publishes the block. */
	dell.smbios = block;
	dell_found_add(PIECE_MODE);

	/* Says what the firmware's variable holds. */
	if (mode == DRV_ACPI_DELL_MODE_NONE) {
		drv_acpi_os_log("dell: thermal mode not known\n");
	} else {
		drv_acpi_os_log("dell: thermal mode %s\n", drv_acpi_dell_mode_name(mode));
	}

	/* Succeeded: the mode call can be made. */
	return 0;
}

/*
 * Finds the DDV interface, checks its version, and registers each fan it
 * lists.  Returns 0, ENOENT when there is no fan to read, or EAGAIN when
 * the interface could not be read.
 */
static int
dell_probe_fans(void)
{
	struct drv_acpi_dell_fan fans[DRV_ACPI_DELL_FANS_MAX];
	const struct drv_acpi_wmi_block *block;
	struct drv_acpi_object *result;
	enum drv_acpi_type type;
	uint64_t version;
	unsigned count;
	unsigned index;
	int error;

	/* Finds the DDV block. */
	error = drv_acpi_wmi_find(DELL_GUID_DDV, 0, &block);
	if (error != 0)
		return ENOENT;
	dell.ddv = block;

	/* Reads the interface's version. */
	result = NULL;
	error = dell_ddv_call(DDV_VERSION, &result);
	if (error != 0) {
		drv_acpi_os_log("dell: the DDV version could not be read (error %d)\n", error);
		return EAGAIN;
	}

	/* Takes the version from the answer, then gives the answer back. */
	version = 0;
	if (result != NULL) {
		type = drv_acpi_object_type(result);
		if (type == DRV_ACPI_TYPE_INTEGER)
			version = drv_acpi_object_integer(result);
		drv_acpi_object_release(result);
	}

	/* Refuses a version the provider does not know. */
	if (version != DDV_VERSION_KNOWN) {
		drv_acpi_os_log("dell: DDV version %u is not known, the fans are not read\n", (unsigned)version);
		return ENOENT;
	}

	/* Reads the fans the firmware lists. */
	error = dell_fans_read(fans, &count);
	if (error == EAGAIN)
		return EAGAIN;
	if (error != 0) {
		drv_acpi_os_log("dell: the DDV fan information is refused (error %d)\n", error);
		return ENOENT;
	}

	/* Says so of a machine that lists none. */
	if (count == 0U) {
		drv_acpi_os_log("dell: the DDV lists no fan\n");
		return ENOENT;
	}

	/* Registers each fan with the thermal core. */
	for (index = 0; index < count; index++) {
		dell.fans[index].index = index;
		error = kern_thermal_fan_register(dell_fan_names[index], 0, &dell_fan_ops, &dell.fans[index]);
		if (error != 0) {
			drv_acpi_os_log("dell: the thermal core refused fan %u (error %d)\n", index, error);
			break;
		}

		/* Counts the fan the core now reads. */
		dell.fan_count++;
	}

	/* Succeeded: the fans are read from now on. */
	dell_found_add(PIECE_FANS);
	drv_acpi_os_log("dell: %u fans, fan 0 at %u rpm\n", dell.fan_count, (unsigned)fans[0].rpm);
	return 0;
}

#if CONFIG_DELL_SMM_BOOST
/*
 * Lists the SMM calls' block from the mapping device whose _WDG method
 * only returns data, and finds it.  Returns 0, ENOENT when the firmware
 * has no SMM calls, or EAGAIN when the device could not be read.
 */
static int
dell_probe_smm(void)
{
	const struct drv_acpi_wmi_block *block;
	int error;

	/* Lists the device's blocks; a machine without the device has no SMM calls. */
	error = drv_acpi_wmi_attach_path(DELL_SMM_DEVICE);
	if (error == ENOENT || error == ENODEV)
		return ENOENT;
	if (error != 0) {
		drv_acpi_os_log("dell: the SMM call device could not be read (error %d)\n", error);
		return EAGAIN;
	}

	/* Finds the block. */
	error = drv_acpi_wmi_find(DELL_GUID_SMM, 0, &block);
	if (error != 0)
		return ENOENT;

	/* Succeeded: the fans' boost is offered. */
	dell.smm = block;
	dell_found_add(PIECE_SMM);
	return 0;
}
#endif

/*
 * Reads the DDV fan information.  Returns 0, EAGAIN when the embedded
 * controller is not ready for the firmware's AML or the call failed, or
 * the decoder's error.
 */
static int
dell_fans_read(
	struct drv_acpi_dell_fan *fans,
	unsigned *count)
{
	struct drv_acpi_object *result;
	struct drv_acpi_object *length;
	struct drv_acpi_object *buffer;
	const uint8_t *bytes;
	enum drv_acpi_type type;
	enum drv_acpi_type length_type;
	enum drv_acpi_type buffer_type;
	uint64_t listed;
	uint64_t ready;
	unsigned elements;
	size_t size;
	int error;

	/* Starts with no fan. */
	*count = 0;

	/*
	 * Refuses to read while the firmware's AML would reach the embedded
	 * controller through System Management Mode, one call per byte.  A
	 * firmware without the flag is read as it is.
	 */
	ready = 1;
	error = drv_acpi_evaluate_integer(NULL, DELL_EC_READY, &ready);
	if (error != 0 && error != ENOENT)
		return EAGAIN;
	if (ready == 0U)
		return EAGAIN;

	/* Calls the fan information method. */
	result = NULL;
	error = dell_ddv_call(DDV_FANS, &result);
	if (error != 0)
		return EAGAIN;
	if (result == NULL)
		return EINVAL;

	/* Refuses an answer that is not a package. */
	type = drv_acpi_object_type(result);
	if (type != DRV_ACPI_TYPE_PACKAGE) {
		drv_acpi_object_release(result);
		return EINVAL;
	}

	/* Refuses a package of another size. */
	elements = drv_acpi_object_package_count(result);
	if (elements != 2U) {
		drv_acpi_object_release(result);
		return EINVAL;
	}

	/* Refuses a package without its two elements. */
	length = drv_acpi_object_package_element(result, 0);
	buffer = drv_acpi_object_package_element(result, 1);
	if (length == NULL || buffer == NULL) {
		drv_acpi_object_release(result);
		return EINVAL;
	}

	/* Refuses elements of other types. */
	length_type = drv_acpi_object_type(length);
	buffer_type = drv_acpi_object_type(buffer);
	if (length_type != DRV_ACPI_TYPE_INTEGER || buffer_type != DRV_ACPI_TYPE_BUFFER) {
		drv_acpi_object_release(result);
		return EINVAL;
	}

	/* Decodes the fans, then gives the answer back. */
	listed = drv_acpi_object_integer(length);
	bytes = drv_acpi_object_buffer(buffer, &size);
	error = drv_acpi_dell_fans_decode(listed, bytes, size, fans, DRV_ACPI_DELL_FANS_MAX, count);
	drv_acpi_object_release(result);

	/* Takes the first fans of a list longer than the provider keeps. */
	if (error == E2BIG)
		error = 0;
	if (error != 0)
		return error;

	/* Succeeded: fans holds count fans. */
	return 0;
}

/* Calls a DDV method with its four bytes of zeros. */
static int
dell_ddv_call(
	uint32_t method,
	struct drv_acpi_object **result)
{
	uint8_t input[DDV_INPUT_SIZE];
	int error;

	/* Calls the method. */
	kern_memset(input, 0, sizeof(input));
	error = drv_acpi_wmi_method(dell.ddv, 0, method, input, sizeof(input), result);
	if (error != 0)
		return error;

	/* Succeeded: the caller holds the answer. */
	return 0;
}

/*
 * Reads the firmware's variable that mirrors the last thermal mode asked
 * for, or DRV_ACPI_DELL_MODE_NONE when it cannot be read or holds no mode.
 */
static uint32_t
dell_mode_read(void)
{
	uint64_t value;
	int error;

	/* Reads the variable. */
	value = 0;
	error = drv_acpi_evaluate_integer(NULL, DELL_MODE_VARIABLE, &value);
	if (error != 0)
		return DRV_ACPI_DELL_MODE_NONE;

	/* Refuses a value that is no mode. */
	if (value >= DRV_ACPI_DELL_MODE_COUNT)
		return DRV_ACPI_DELL_MODE_NONE;

	/* Succeeded: reports the mode. */
	return (uint32_t)value;
}

/*
 * Sets a BIOS thermal mode through the SMBIOS calling interface (the
 * strategy thread).  The call succeeded when the firmware wrote the
 * outputs and output0 says it completed.  Firmware that leaves the outputs
 * untouched does not take the call, and the mode call is offered no more.
 */
static int
dell_mode_set(
	uint32_t mode)
{
	uint8_t request[DRV_ACPI_DELL_SMBIOS_SIZE];
	struct drv_acpi_object *result;
	const uint8_t *answer;
	enum drv_acpi_type type;
	uint32_t mirrored;
	uint32_t status;
	size_t size;
	int error;

	/* Forgets the mode until the firmware confirms the new one. */
	__atomic_store_n(&dell.mode, DRV_ACPI_DELL_MODE_NONE, __ATOMIC_RELAXED);

	/* Makes the call. */
	drv_acpi_dell_mode_request(mode, request);
	result = NULL;
	error = drv_acpi_wmi_method(dell.smbios, 0, SMBIOS_METHOD, request, sizeof(request), &result);
	if (error != 0) {
		drv_acpi_os_log("dell: the thermal mode call failed (error %d)\n", error);
		return EIO;
	}

	/* Checks the answer, then gives it back. */
	error = EINVAL;
	status = 0;
	if (result != NULL) {
		type = drv_acpi_object_type(result);
		if (type == DRV_ACPI_TYPE_BUFFER) {
			answer = drv_acpi_object_buffer(result, &size);
			error = drv_acpi_dell_mode_answer(answer, size, &status);
		}

		/* Gives the answer back. */
		drv_acpi_object_release(result);
	}

	/* Offers the mode call no more when the firmware does not take it. */
	if (error == ENXIO || error == EINVAL) {
		drv_acpi_os_log("dell: the firmware did not run the thermal mode call\n");
		dell_found_remove(PIECE_MODE);
		return EIO;
	}

	/* Reports a call the firmware refused. */
	if (error != 0) {
		drv_acpi_os_log("dell: the thermal mode call returned status 0x%x\n", (unsigned)status);
		return EIO;
	}

	/* Remembers the mode, and says so when the firmware's variable shows another. */
	__atomic_store_n(&dell.mode, mode, __ATOMIC_RELAXED);
	mirrored = dell_mode_read();
	if (mirrored != mode)
		drv_acpi_os_log("dell: thermal mode %s set, the firmware shows %u\n", drv_acpi_dell_mode_name(mode), (unsigned)mirrored);

	/* Succeeded: the firmware took the mode. */
	return 0;
}

#if CONFIG_DELL_SMM_BOOST
/* Makes one SMM call through the SMM calls' block and checks its answer. */
static int
dell_smm_call(
	const uint8_t request[DRV_ACPI_DELL_SMM_SIZE],
	uint32_t *eax)
{
	struct drv_acpi_object *result;
	const uint8_t *answer;
	enum drv_acpi_type type;
	size_t size;
	int error;

	/* Makes the call. */
	*eax = 0;
	result = NULL;
	error = drv_acpi_wmi_method(dell.smm, 0, SMM_METHOD, request, DRV_ACPI_DELL_SMM_SIZE, &result);
	if (error != 0)
		return EIO;
	if (result == NULL)
		return EIO;

	/* Checks the answer, then gives it back. */
	error = EIO;
	type = drv_acpi_object_type(result);
	if (type == DRV_ACPI_TYPE_BUFFER) {
		answer = drv_acpi_object_buffer(result, &size);
		error = drv_acpi_dell_smm_answer(answer, size, request, eax);
	}

	/* Gives the answer back, and reports a call that did not run or was refused. */
	drv_acpi_object_release(result);
	if (error != 0)
		return EIO;

	/* Succeeded: eax holds what the SMM returned. */
	return 0;
}

/* Reads a fan's state through the SMM fan call. */
static int
dell_fan_state_get(
	struct dell_fan *fan,
	uint32_t *state)
{
	uint8_t request[DRV_ACPI_DELL_SMM_SIZE];
	uint32_t eax;
	int error;

	/* Asks for the fan's state; it is in the low byte of EAX. */
	drv_acpi_dell_fan_get_request(fan->index, request);
	error = dell_smm_call(request, &eax);
	if (error != 0)
		return error;
	*state = eax & 0xffU;

	/* Succeeded: state holds the fan's state. */
	return 0;
}

/* Sets a fan's state through the SMM fan call. */
static int
dell_fan_state_set(
	struct dell_fan *fan,
	uint32_t state)
{
	uint8_t request[DRV_ACPI_DELL_SMM_SIZE];
	uint32_t eax;
	int error;

	/* Asks for the state. */
	drv_acpi_dell_fan_set_request(fan->index, state, request);
	error = dell_smm_call(request, &eax);
	if (error != 0)
		return error;

	/* Succeeded: the SMM took the call. */
	return 0;
}

/*
 * Records a change of a fan's boost (the strategy thread): whether the fan
 * is raised, and the speed it ran at before, which the poll thread watches
 * the raise against.
 */
static void
dell_boost_publish(
	struct dell_fan *fan,
	bool raised)
{
	uint32_t generation;
	uint32_t boosted;
	uint32_t rpm;

	/* Keeps the speed before the change and whether it is a raise. */
	fan->raised = raised;
	boosted = 0;
	if (raised)
		boosted = 1;
	rpm = __atomic_load_n(&fan->last_rpm, __ATOMIC_RELAXED);
	__atomic_store_n(&fan->rpm_before, rpm, __ATOMIC_RELAXED);
	__atomic_store_n(&fan->boosted, boosted, __ATOMIC_RELAXED);

	/* Moves the generation last, so the poll thread that sees it sees the two above. */
	generation = __atomic_load_n(&fan->generation, __ATOMIC_RELAXED);
	__atomic_store_n(&fan->generation, generation + 1U, __ATOMIC_RELEASE);
}

/* Boosts a fan no more, and says why. */
static void
dell_boost_refuse(
	struct dell_fan *fan,
	const char *why,
	int error)
{
	/* Marks the fan; its reads offer no boost from now on. */
	__atomic_store_n(&fan->boost_off, 1U, __ATOMIC_RELAXED);
	fan->raised = false;
	drv_acpi_os_log("dell: fan %u: the SMM fan %s (error %d), no boost\n", fan->index, why, error);
}

/*
 * Watches a raised fan's speed (the poll thread): a speed at or below the
 * one before the raise, read three times in a row, tells that the firmware
 * put the fan back, and the fan is boosted no more.
 */
static void
dell_boost_watch(
	struct dell_fan *fan,
	uint32_t rpm)
{
	uint32_t generation;
	uint32_t boosted;
	uint32_t before;
	uint32_t off;

	/* Starts counting anew for a new boost. */
	generation = __atomic_load_n(&fan->generation, __ATOMIC_ACQUIRE);
	if (generation != fan->seen_generation) {
		fan->seen_generation = generation;
		fan->slow_reads = 0;
	}

	/* Nothing to watch while the fan is not raised, or once it is boosted no more. */
	boosted = __atomic_load_n(&fan->boosted, __ATOMIC_RELAXED);
	off = __atomic_load_n(&fan->boost_off, __ATOMIC_RELAXED);
	if (boosted == 0U || off != 0U)
		return;

	/* Counts the reads in a row that find the fan no faster than before the raise. */
	before = __atomic_load_n(&fan->rpm_before, __ATOMIC_RELAXED);
	if (rpm > before) {
		fan->slow_reads = 0;
		return;
	}

	/* Waits for the third such read before it believes the fan was put back. */
	fan->slow_reads++;
	if (fan->slow_reads < BOOST_SLOW_READS)
		return;

	/* The firmware put the fan back: boosts it no more. */
	__atomic_store_n(&fan->boost_off, 1U, __ATOMIC_RELAXED);
	drv_acpi_os_log("dell: fan %u runs at %u rpm, not above %u before the boost: the firmware put it back, no boost\n", fan->index, (unsigned)rpm, (unsigned)before);
}
#endif

/* Sleeps in the strategy thread until a call into the SMM may start. */
static void
dell_smi_wait(void)
{
	uint64_t now;
	uint64_t passed;
	uint64_t left;

	/* The first call need not wait. */
	if (dell.last_smi == 0U)
		return;

	/* Sleeps out what is left of the second since the last call. */
	now = drv_acpi_os_timer();
	passed = now - dell.last_smi;
	if (passed >= SMI_SPACING)
		return;
	left = (SMI_SPACING - passed) / (SMI_SPACING / SMI_SPACING_MS) + 1U;
	drv_acpi_os_log("dell: waits %u ms before the next firmware call\n", (unsigned)left);
	drv_acpi_os_sleep(left);
}

/* Records the end of a call into the SMM, which the next one waits a second from. */
static void
dell_smi_done(void)
{
	uint64_t now;

	/* Stamps the time; a clock that reads 0 still counts as a call. */
	now = drv_acpi_os_timer();
	if (now == 0U)
		now = 1U;
	dell.last_smi = now;
}

/* Reports the pieces found, with an acquire load so their blocks are seen. */
static uint32_t
dell_found(void)
{
	uint32_t found;

	/* Loads the bits. */
	found = __atomic_load_n(&dell.found, __ATOMIC_ACQUIRE);

	/* Reports them. */
	return found;
}

/* Publishes pieces found, after their blocks. */
static void
dell_found_add(
	uint32_t pieces)
{
	/* Sets the bits with a release, so a reader that sees one sees its block. */
	(void)__atomic_fetch_or(&dell.found, pieces, __ATOMIC_RELEASE);
}

/* Withdraws pieces the firmware turned out not to take. */
static void
dell_found_remove(
	uint32_t pieces)
{
	/* Clears the bits. */
	(void)__atomic_fetch_and(&dell.found, ~pieces, __ATOMIC_RELEASE);
}
