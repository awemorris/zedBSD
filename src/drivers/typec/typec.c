/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB Type-C connector layer (ws050-p002): the connector records the
 * connector driver publishes, their generations, and the listeners told of
 * each change.  The records are copied in and out under the layer's lock;
 * the listeners are called outside it.
 */

#include <stdarg.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/typec/typec.h>

#include "typec-os.h"

/*
 * A _PLD buffer (ACPI 6.5 section 6.1.8): the size of revision 1, the
 * revision's bits, the user visible bit, and the first bits of the group
 * token and the group position, 8 bits each.
 */
#define TYPEC_PLD_SIZE		16U
#define TYPEC_PLD_REVISION_BIT	0U
#define TYPEC_PLD_REVISION_BITS	7U
#define TYPEC_PLD_VISIBLE_BIT	64U
#define TYPEC_PLD_TOKEN_BIT	79U
#define TYPEC_PLD_POSITION_BIT	87U
#define TYPEC_PLD_GROUP_BITS	8U

/*
 * One registered listener and its argument.
 */
struct typec_listener_entry {
	drv_typec_listener listener;
	void *argument;
};

/*
 * What the display driver last reported of one of its Type-C ports, and
 * the connector the port is wired to (bound is the connector's index plus
 * one; 0 is none, which is what a port starts as).
 */
struct typec_display_entry {
	uint64_t generation;
	struct drv_typec_dp_state state;
	unsigned bound;
};

/*
 * How a bound connector's two DisplayPort reports compare: whether they
 * differ, since when (milliseconds), and whether they have differed long
 * enough to be a disagreement, which is logged once when it begins.
 */
struct typec_dp_watch {
	bool differing;
	uint64_t since_ms;
	bool disagree;
};

/*
 * A text being written into a caller's buffer: what has been written, and
 * the room.  The text stays terminated; what does not fit is dropped.
 */
struct typec_text {
	char *buffer;
	size_t size;
	size_t length;
};

/*
 * The connector records, one per connector the driver found.
 *
 * Written by drv_typec_connector_publish() and read by
 * drv_typec_connector_get(), both under the layer's lock.  Records past
 * typec_count are zero.
 */
static struct drv_typec_connector typec_connectors[DRV_TYPEC_CONNECTOR_MAX];

/*
 * How many connectors the driver found (0 until it reports them).
 *
 * Protected by the layer's lock.
 */
static unsigned typec_count;

/*
 * The generation stamped on the record published next.
 *
 * It only ever increases (from 1), so a reader holding a record knows it is
 * stale when the connector's current generation is larger.  Protected by
 * the layer's lock.
 */
static uint64_t typec_generation;

/*
 * The registered listeners.
 *
 * Entries are only added (drivers register once and stay), under the
 * layer's lock; a change is told to the ones registered when it is
 * published.
 */
static struct typec_listener_entry typec_listeners[DRV_TYPEC_LISTENER_MAX];

/*
 * How many listeners are registered.
 *
 * Protected by the layer's lock.
 */
static unsigned typec_listener_count;

/*
 * The operations waiting for the connector driver, oldest first: a ring of
 * typec_request_count entries from typec_request_first.
 *
 * Filled by the drv_typec_connector_* operations and emptied by the
 * connector driver's thread, both under the layer's lock.
 */
static struct drv_typec_request typec_requests[DRV_TYPEC_REQUEST_MAX];
static unsigned typec_request_first;
static unsigned typec_request_count;

/*
 * The serial given to the operation asked next.
 *
 * It only increases (from 1; 0 in a record means none was carried out),
 * under the layer's lock.
 */
static uint32_t typec_request_serial;

/*
 * The connector driver's wake-up, called (outside the lock) after an
 * operation is queued, and its argument; NULL until the driver names it.
 */
static void (*typec_kick)(void *argument);
static void *typec_kick_argument;

/*
 * What the display driver reported of its Type-C ports, and their bindings.
 *
 * Written by drv_typec_display_report() and drv_typec_display_bind(),
 * read when a connector record is copied, all under the layer's lock.  The
 * zero entry is a port never reported and bound to nothing; the bindings
 * outlive a reset of the connectors (they are how the board is wired).
 */
static struct typec_display_entry typec_displays[DRV_TYPEC_DISPLAY_PORT_MAX];

/*
 * The comparison of the two DisplayPort reports of each connector.
 *
 * Moved on whenever either report or a binding changes, and when a record
 * is copied or drv_typec_display_check() runs, under the layer's lock.
 * Emptied with the connector records.
 */
static struct typec_dp_watch typec_dp_watches[DRV_TYPEC_CONNECTOR_MAX];

static int typec_request_put(struct drv_typec_request *request, uint32_t *serial);
static unsigned typec_display_port_of(unsigned connector);
static unsigned typec_location_bits(const uint8_t *buffer, unsigned first, unsigned count);
static bool typec_dp_same(const struct drv_typec_dp_state *left, const struct drv_typec_dp_state *right);
static bool typec_dp_differ(const struct drv_typec_dp_state *display, const struct drv_typec_dp_state *ucsi);
static bool typec_dp_compare(unsigned connector, uint64_t now, uint32_t *remaining);
static void typec_dp_fill(unsigned index, struct drv_typec_connector *connector);
static void typec_dp_log(unsigned connector, uint64_t generation, const struct drv_typec_dp_state *display, const struct drv_typec_dp_state *ucsi);
static void typec_listeners_tell(unsigned connector, uint64_t generation);
static void typec_text_dp(struct typec_text *text, const struct drv_typec_connector *connector);
static void typec_text_display(struct typec_text *text, unsigned port, const struct drv_typec_display *display);
static char typec_text_pin(enum drv_typec_dp_pin pin);
static void typec_text_line(struct typec_text *text, unsigned index, const struct drv_typec_connector *connector);
static void typec_text_modes(struct typec_text *text, const char *name, const struct drv_typec_alt_mode_list *list);
static const char *typec_text_partner(enum drv_typec_partner_type type);
static const char *typec_text_power(enum drv_typec_power_operation operation);
static void typec_text_append(struct typec_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/*
 * Registers a listener of connector changes.
 *
 * Returns 0, EINVAL for no listener, or ENOSPC when the table is full.
 */
int
drv_typec_listener_register(
	drv_typec_listener listener,
	void *argument)
{
	/* Refuses a missing listener. */
	if (listener == NULL)
		return EINVAL;

	/* Adds it to the table, which has a fixed room. */
	drv_typec_os_lock();

	if (typec_listener_count == DRV_TYPEC_LISTENER_MAX) {
		drv_typec_os_unlock();
		return ENOSPC;
	}

	/* The next free entry. */
	typec_listeners[typec_listener_count].listener = listener;
	typec_listeners[typec_listener_count].argument = argument;
	typec_listener_count++;

	drv_typec_os_unlock();

	/* Succeeded: it is told of every change published from now on. */
	return 0;
}

/*
 * Reports how many connectors there are.
 */
unsigned
drv_typec_connector_count(void)
{
	unsigned count;

	/* Reads the count the driver set. */
	drv_typec_os_lock();

	count = typec_count;

	drv_typec_os_unlock();

	/* Reports it. */
	return count;
}

/*
 * Copies the record of a connector (0-based index).
 *
 * Returns 0, or ENOENT for a connector that is not there.
 */
int
drv_typec_connector_get(
	unsigned index,
	struct drv_typec_connector *connector)
{
	uint32_t remaining;
	uint64_t now;
	bool began;

	/* The time the two DisplayPort reports are compared at. */
	now = drv_typec_os_now_ms();

	/*
	 * Copies the record of a connector that is there, with the display
	 * driver's report of its bound port and how the two reports compare.
	 */
	drv_typec_os_lock();

	if (index >= typec_count) {
		drv_typec_os_unlock();
		return ENOENT;
	}

	/* The copy, and the layer's part of it. */
	kern_memcpy(connector, &typec_connectors[index], sizeof(*connector));
	began = typec_dp_compare(index, now, &remaining);
	typec_dp_fill(index, connector);

	drv_typec_os_unlock();

	/* A disagreement this copy saw begin is logged once. */
	if (began)
		typec_dp_log(index, connector->generation, &connector->dp_display, &connector->dp_ucsi);

	/* Succeeded: the caller holds a copy at its generation. */
	return 0;
}

/*
 * Sets how many connectors the connector driver found, emptying every
 * record (at the driver's start and after its PPM is reset).
 */
void
drv_typec_connectors_reset(
	unsigned count)
{
	/* Keeps no more than the table holds. */
	if (count > DRV_TYPEC_CONNECTOR_MAX) {
		drv_typec_os_log("typec: %u connectors, keeping %u\n", count, DRV_TYPEC_CONNECTOR_MAX);
		count = DRV_TYPEC_CONNECTOR_MAX;
	}

	/* Empties the records and sets the count. */
	drv_typec_os_lock();

	kern_memset(typec_connectors, 0, sizeof(typec_connectors));
	kern_memset(typec_dp_watches, 0, sizeof(typec_dp_watches));
	typec_count = count;

	drv_typec_os_unlock();
}

/*
 * Publishes a new record of a connector (0-based index) and tells the
 * listeners which connector changed and at which generation.
 *
 * Returns 0, or ENOENT for a connector that is not there.
 */
int
drv_typec_connector_publish(
	unsigned index,
	const struct drv_typec_connector *connector)
{
	struct drv_typec_dp_state display;
	struct drv_typec_dp_state ucsi;
	uint32_t remaining;
	uint64_t generation;
	uint64_t now;
	unsigned port;
	bool began;

	/* The time the two DisplayPort reports are compared at. */
	now = drv_typec_os_now_ms();

	/*
	 * Stores the record under a new generation and compares its
	 * DisplayPort report with the display driver's; the listeners are
	 * told after the lock is released.
	 */
	drv_typec_os_lock();

	if (index >= typec_count) {
		drv_typec_os_unlock();
		return ENOENT;
	}

	/* The record under its generation. */
	typec_generation++;
	generation = typec_generation;
	kern_memcpy(&typec_connectors[index], connector, sizeof(typec_connectors[index]));
	typec_connectors[index].generation = generation;

	/* The comparison with the bound port, and what a disagreement that began is logged with. */
	began = typec_dp_compare(index, now, &remaining);
	kern_memset(&display, 0, sizeof(display));
	port = typec_display_port_of(index);
	if (port != DRV_TYPEC_DISPLAY_PORT_NONE)
		display = typec_displays[port].state;
	ucsi = typec_connectors[index].dp_ucsi;

	drv_typec_os_unlock();

	/* A disagreement this record began is logged once. */
	if (began)
		typec_dp_log(index, generation, &display, &ucsi);

	/* Tells each listener. */
	typec_listeners_tell(index, generation);

	/* Succeeded: the record is current at its new generation. */
	return 0;
}

/*
 * Asks a connector (0-based index) to swap to a data role, accepting the
 * partner's swaps from then on.
 *
 * Returns 0 with the operation's serial, ENOENT for a connector that is
 * not there, EINVAL for a role that is not one, or EBUSY when too many
 * operations wait.
 */
int
drv_typec_connector_set_data_role(
	unsigned index,
	enum drv_typec_data_role role,
	uint32_t *serial)
{
	struct drv_typec_request request;
	int error;

	/* Refuses a role that is not one. */
	if (role != DRV_TYPEC_DATA_DFP && role != DRV_TYPEC_DATA_UFP)
		return EINVAL;

	/* The operation, queued. */
	kern_memset(&request, 0, sizeof(request));
	request.kind = DRV_TYPEC_REQUEST_DATA_ROLE;
	request.connector = index;
	request.value = (unsigned)role;
	error = typec_request_put(&request, serial);
	if (error != 0)
		return error;

	/* Succeeded: the connector driver carries it out. */
	return 0;
}

/*
 * Asks a connector (0-based index) to swap to a power role, accepting the
 * partner's swaps from then on.
 *
 * Returns 0 with the operation's serial, ENOENT, EINVAL or EBUSY.
 */
int
drv_typec_connector_set_power_role(
	unsigned index,
	enum drv_typec_power_role role,
	uint32_t *serial)
{
	struct drv_typec_request request;
	int error;

	/* Refuses a role that is not one. */
	if (role != DRV_TYPEC_ROLE_SINK && role != DRV_TYPEC_ROLE_SOURCE)
		return EINVAL;

	/* The operation, queued. */
	kern_memset(&request, 0, sizeof(request));
	request.kind = DRV_TYPEC_REQUEST_POWER_ROLE;
	request.connector = index;
	request.value = (unsigned)role;
	error = typec_request_put(&request, serial);
	if (error != 0)
		return error;

	/* Succeeded: the connector driver carries it out. */
	return 0;
}

/*
 * Asks a connector (0-based index) to be reset.
 *
 * Returns 0 with the operation's serial, ENOENT, EINVAL or EBUSY.
 */
int
drv_typec_connector_reset(
	unsigned index,
	enum drv_typec_reset kind,
	uint32_t *serial)
{
	struct drv_typec_request request;
	int error;

	/* Refuses a kind that is not one. */
	if (kind != DRV_TYPEC_RESET_HARD && kind != DRV_TYPEC_RESET_DATA)
		return EINVAL;

	/* The operation, queued. */
	kern_memset(&request, 0, sizeof(request));
	request.kind = DRV_TYPEC_REQUEST_RESET;
	request.connector = index;
	request.value = (unsigned)kind;
	error = typec_request_put(&request, serial);
	if (error != 0)
		return error;

	/* Succeeded: the connector driver carries it out. */
	return 0;
}

/*
 * Asks a connector (0-based index) to enter one of its Alternate Modes
 * (an index into its connector_modes) with a mode-specific configuration.
 *
 * Returns 0 with the operation's serial, ENOENT, EINVAL for a mode the
 * connector does not list, or EBUSY.
 */
int
drv_typec_connector_enter_mode(
	unsigned index,
	unsigned mode,
	uint32_t configuration,
	uint32_t *serial)
{
	struct drv_typec_request request;
	int error;

	/* Refuses a mode beyond any list. */
	if (mode >= DRV_TYPEC_ALT_MODE_MAX)
		return EINVAL;

	/* The operation, queued. */
	kern_memset(&request, 0, sizeof(request));
	request.kind = DRV_TYPEC_REQUEST_ENTER_MODE;
	request.connector = index;
	request.mode = mode;
	request.configuration = configuration;
	error = typec_request_put(&request, serial);
	if (error != 0)
		return error;

	/* Succeeded: the connector driver carries it out. */
	return 0;
}

/*
 * Asks a connector (0-based index) to leave one of its Alternate Modes.
 *
 * Returns 0 with the operation's serial, ENOENT, EINVAL or EBUSY.
 */
int
drv_typec_connector_exit_mode(
	unsigned index,
	unsigned mode,
	uint32_t *serial)
{
	struct drv_typec_request request;
	int error;

	/* Refuses a mode beyond any list. */
	if (mode >= DRV_TYPEC_ALT_MODE_MAX)
		return EINVAL;

	/* The operation, queued. */
	kern_memset(&request, 0, sizeof(request));
	request.kind = DRV_TYPEC_REQUEST_EXIT_MODE;
	request.connector = index;
	request.mode = mode;
	error = typec_request_put(&request, serial);
	if (error != 0)
		return error;

	/* Succeeded: the connector driver carries it out. */
	return 0;
}

/*
 * Names the connector driver's wake-up, called after each operation is
 * queued.
 */
void
drv_typec_operator_set(
	void (*kick)(void *argument),
	void *argument)
{
	/* Kept for the operations asked from now on. */
	drv_typec_os_lock();

	typec_kick = kick;
	typec_kick_argument = argument;

	drv_typec_os_unlock();
}

/*
 * Takes the oldest waiting operation.  Returns true with it in *request,
 * false when none waits.
 */
bool
drv_typec_request_take(
	struct drv_typec_request *request)
{
	/* The oldest, out of the ring. */
	drv_typec_os_lock();

	if (typec_request_count == 0) {
		drv_typec_os_unlock();
		return false;
	}

	/* The oldest, out of the ring. */
	*request = typec_requests[typec_request_first];
	typec_request_first = (typec_request_first + 1U) % DRV_TYPEC_REQUEST_MAX;
	typec_request_count--;

	drv_typec_os_unlock();

	/* Succeeded: the connector driver carries it out. */
	return true;
}

/*
 * Notes an operation's outcome (its serial and errno value) in its
 * connector's record without a new generation: the connector driver reads
 * the connector again and publishes it, which tells the listeners.
 *
 * Returns 0, or ENOENT for a connector that is not there any more.
 */
int
drv_typec_request_finish(
	const struct drv_typec_request *request,
	int error)
{
	/* The outcome in the record. */
	drv_typec_os_lock();

	if (request->connector >= typec_count) {
		drv_typec_os_unlock();
		return ENOENT;
	}

	/* Its serial and outcome. */
	typec_connectors[request->connector].request_serial = request->serial;
	typec_connectors[request->connector].request_error = error;

	drv_typec_os_unlock();

	/* Succeeded: the next published record carries it. */
	return 0;
}

/*
 * Records what the display driver reads of DisplayPort on one of its
 * Type-C ports (0-based): the report replaces the last one.  When the port
 * is bound to a connector, that connector's record changes with it: it is
 * stamped with the report's generation, the two reports are compared, and
 * the listeners are told.  A difference that has to wait to become a
 * disagreement wakes the connector driver's thread, which calls
 * drv_typec_display_check() when the wait is over.
 *
 * Called from the display driver's thread (never from an interrupt), with
 * none of its Type-C ports' locks held.  Returns 0, or EINVAL for a port
 * beyond DRV_TYPEC_DISPLAY_PORT_MAX or no report.
 */
int
drv_typec_display_report(
	unsigned port,
	const struct drv_typec_dp_state *state)
{
	struct drv_typec_dp_state display;
	struct drv_typec_dp_state ucsi;
	struct typec_display_entry *entry;
	void (*kick)(void *argument);
	void *argument;
	uint32_t remaining;
	uint64_t generation;
	uint64_t now;
	unsigned connector;
	bool changed;
	bool same;
	bool began;

	/* Refuses a port the layer has no room for, or no report. */
	if (port >= DRV_TYPEC_DISPLAY_PORT_MAX)
		return EINVAL;
	if (state == NULL)
		return EINVAL;

	/* The report, as known, with a pin assignment that is not a letter read as none. */
	display = *state;
	display.known = true;
	if ((unsigned)display.pin > (unsigned)DRV_TYPEC_DP_PIN_F)
		display.pin = DRV_TYPEC_DP_PIN_NONE;

	/* The time the two DisplayPort reports are compared at. */
	now = drv_typec_os_now_ms();

	/*
	 * Stores a report that differs from the last one under a new
	 * generation, which the bound connector's record takes; the bound
	 * connector is compared with every report, the same one again too.
	 */
	drv_typec_os_lock();

	/* A new report, or the same again (the display's hotplug work repeats one). */
	entry = &typec_displays[port];
	same = typec_dp_same(&entry->state, &display);
	changed = true;
	if (entry->generation != 0 && same)
		changed = false;
	generation = entry->generation;
	if (changed) {
		typec_generation++;
		generation = typec_generation;
		entry->generation = generation;
		entry->state = display;
	}

	/* The bound connector, when the connector driver has it, changes with a new report. */
	connector = DRV_TYPEC_CONNECTOR_NONE;
	began = false;
	remaining = 0;
	kern_memset(&ucsi, 0, sizeof(ucsi));
	if (entry->bound != 0U && entry->bound - 1U < typec_count) {
		connector = entry->bound - 1U;
		if (changed)
			typec_connectors[connector].generation = generation;
		began = typec_dp_compare(connector, now, &remaining);
		ucsi = typec_connectors[connector].dp_ucsi;
	}

	/* The connector driver's wake-up, for a difference that waits. */
	kick = typec_kick;
	argument = typec_kick_argument;

	drv_typec_os_unlock();

	/* An unbound port's report is only kept. */
	if (connector == DRV_TYPEC_CONNECTOR_NONE)
		return 0;

	/* A disagreement this report began is logged once. */
	if (began)
		typec_dp_log(connector, generation, &display, &ucsi);

	/* A difference that waits to become a disagreement wakes the thread that checks it. */
	if (remaining != 0 && kick != NULL)
		kick(argument);

	/* Tells each listener of the bound connector's change, when the report is new. */
	if (changed)
		typec_listeners_tell(connector, generation);

	/* Succeeded: the report is the port's current one. */
	return 0;
}

/*
 * Copies what the display driver last reported of one of its Type-C ports
 * (0-based), and the connector it is bound to.
 *
 * Returns 0, or EINVAL for a port beyond DRV_TYPEC_DISPLAY_PORT_MAX.
 */
int
drv_typec_display_get(
	unsigned port,
	struct drv_typec_display *display)
{
	/* Refuses a port the layer has no room for. */
	if (port >= DRV_TYPEC_DISPLAY_PORT_MAX)
		return EINVAL;

	/* Copies the entry, with its binding as a connector index. */
	drv_typec_os_lock();

	display->generation = typec_displays[port].generation;
	display->state = typec_displays[port].state;
	display->connector = DRV_TYPEC_CONNECTOR_NONE;
	if (typec_displays[port].bound != 0U)
		display->connector = typec_displays[port].bound - 1U;

	drv_typec_os_unlock();

	/* Succeeded: the caller holds a copy (generation 0: never reported). */
	return 0;
}

/*
 * Binds a display port (0-based) to the connector (0-based) it is wired
 * to, or unbinds it with DRV_TYPEC_CONNECTOR_NONE.
 *
 * Until a port is bound its reports go into no connector's record: which
 * display port drives which USB-C connector depends on the board and is
 * only taken from where it has been found out.  A connector the connector
 * driver has not found yet may be named; it takes the port's report once
 * it is there.  The two connectors whose binding changed start their
 * comparison afresh and are told to the listeners.
 *
 * Returns 0, EINVAL for a port or a connector beyond the layer's room, or
 * EBUSY when another port is bound to the connector.
 */
int
drv_typec_display_bind(
	unsigned port,
	unsigned connector)
{
	unsigned old_connector;
	unsigned other;
	uint64_t old_generation;
	uint64_t new_generation;
	bool old_present;
	bool new_present;

	/* Refuses a port or a connector the layer has no room for. */
	if (port >= DRV_TYPEC_DISPLAY_PORT_MAX)
		return EINVAL;
	if (connector != DRV_TYPEC_CONNECTOR_NONE && connector >= DRV_TYPEC_CONNECTOR_MAX)
		return EINVAL;

	/* Changes the binding; each connector it moves off or onto changes. */
	drv_typec_os_lock();

	/* A connector takes the reports of one port only. */
	if (connector != DRV_TYPEC_CONNECTOR_NONE) {
		other = typec_display_port_of(connector);
		if (other != DRV_TYPEC_DISPLAY_PORT_NONE && other != port) {
			drv_typec_os_unlock();
			return EBUSY;
		}
	}

	/* The connector the port leaves, and the one it joins (a connector index plus one, 0 for none). */
	old_connector = DRV_TYPEC_CONNECTOR_NONE;
	if (typec_displays[port].bound != 0U)
		old_connector = typec_displays[port].bound - 1U;
	typec_displays[port].bound = 0U;
	if (connector != DRV_TYPEC_CONNECTOR_NONE)
		typec_displays[port].bound = connector + 1U;

	/* Each of the two that the connector driver has starts its comparison afresh under a new generation. */
	old_present = false;
	old_generation = 0;
	if (old_connector != DRV_TYPEC_CONNECTOR_NONE && old_connector != connector && old_connector < typec_count) {
		typec_generation++;
		old_generation = typec_generation;
		typec_connectors[old_connector].generation = old_generation;
		kern_memset(&typec_dp_watches[old_connector], 0, sizeof(typec_dp_watches[old_connector]));
		old_present = true;
	}

	/* The connector joined, likewise. */
	new_present = false;
	new_generation = 0;
	if (connector != DRV_TYPEC_CONNECTOR_NONE && connector < typec_count) {
		typec_generation++;
		new_generation = typec_generation;
		typec_connectors[connector].generation = new_generation;
		kern_memset(&typec_dp_watches[connector], 0, sizeof(typec_dp_watches[connector]));
		new_present = true;
	}

	drv_typec_os_unlock();

	/* Tells the listeners of each connector that changed. */
	if (old_present)
		typec_listeners_tell(old_connector, old_generation);
	if (new_present)
		typec_listeners_tell(connector, new_generation);

	/* Succeeded: the port's reports go to the connector from now on. */
	return 0;
}

/*
 * Reads a _PLD buffer into a location (ACPI 6.5 section 6.1.8): the
 * revision in bits 6:0, the user visible bit 64, the group token in bits
 * 86:79 and the group position in bits 94:87.  Returns 0, or EINVAL for a
 * buffer shorter than the 16 bytes of revision 1 or of revision 0.
 */
int
drv_typec_location_decode(
	const uint8_t *buffer,
	size_t length,
	struct drv_typec_location *location)
{
	unsigned revision;

	/* Nothing is known until the buffer is read. */
	kern_memset(location, 0, sizeof(*location));

	/* A buffer of revision 1 or later is 16 bytes at least. */
	if (buffer == NULL || length < TYPEC_PLD_SIZE)
		return EINVAL;
	revision = typec_location_bits(buffer, TYPEC_PLD_REVISION_BIT, TYPEC_PLD_REVISION_BITS);
	if (revision == 0U)
		return EINVAL;

	/* The fields a connector is matched by. */
	location->visible = typec_location_bits(buffer, TYPEC_PLD_VISIBLE_BIT, 1U) != 0U;
	location->group_token = typec_location_bits(buffer, TYPEC_PLD_TOKEN_BIT, TYPEC_PLD_GROUP_BITS);
	location->group_position = typec_location_bits(buffer, TYPEC_PLD_POSITION_BIT, TYPEC_PLD_GROUP_BITS);

	/* Succeeded: the location is known. */
	location->known = true;
	return 0;
}

/*
 * Finds the connector at a display port's location: the one visible
 * connector whose group token and position are the port's.  A port whose
 * location is unknown or not visible, no such connector, or two of them
 * give DRV_TYPEC_CONNECTOR_NONE: a binding is taken only from where it is
 * certain.
 */
unsigned
drv_typec_location_match(
	const struct drv_typec_location *connectors,
	unsigned count,
	const struct drv_typec_location *port)
{
	unsigned found;
	unsigned index;

	/* A port without a visible location matches nothing. */
	if (!port->known || !port->visible)
		return DRV_TYPEC_CONNECTOR_NONE;

	/* The connectors at the same place; a second one makes it uncertain. */
	found = DRV_TYPEC_CONNECTOR_NONE;
	for (index = 0U; index < count; index++) {
		/* A connector elsewhere, unknown or hidden. */
		if (!connectors[index].known || !connectors[index].visible)
			continue;
		if (connectors[index].group_token != port->group_token ||
		    connectors[index].group_position != port->group_position)
			continue;

		/* A second connector at the place. */
		if (found != DRV_TYPEC_CONNECTOR_NONE)
			return DRV_TYPEC_CONNECTOR_NONE;
		found = index;
	}

	/* The connector, or none. */
	return found;
}

/*
 * Compares the display driver's and UCSI's DisplayPort state of every
 * bound connector, logging each disagreement that begins, and reports how
 * many milliseconds until the youngest difference seen now is old enough
 * to be a disagreement (0: no difference waits).
 *
 * The connector driver's thread calls it each time it wakes and waits no
 * longer than it says.
 */
uint32_t
drv_typec_display_check(void)
{
	struct drv_typec_dp_state display;
	struct drv_typec_dp_state ucsi;
	uint32_t remaining;
	uint32_t earliest;
	uint64_t generation;
	uint64_t now;
	unsigned index;
	unsigned port;
	bool present;
	bool began;

	/* The time the reports are compared at. */
	now = drv_typec_os_now_ms();

	/* Compares each connector in its own critical section, logging outside it. */
	earliest = 0;
	for (index = 0; index < DRV_TYPEC_CONNECTOR_MAX; index++) {
		/* Moves the connector's comparison on, when the connector is there. */
		drv_typec_os_lock();

		present = false;
		began = false;
		remaining = 0;
		generation = 0;
		kern_memset(&display, 0, sizeof(display));
		kern_memset(&ucsi, 0, sizeof(ucsi));
		if (index < typec_count) {
			present = true;
			began = typec_dp_compare(index, now, &remaining);
			generation = typec_connectors[index].generation;
			port = typec_display_port_of(index);
			if (port != DRV_TYPEC_DISPLAY_PORT_NONE)
				display = typec_displays[port].state;
			ucsi = typec_connectors[index].dp_ucsi;
		}

		drv_typec_os_unlock();

		/* The connectors end at the first one the driver did not find. */
		if (!present)
			break;

		/* A disagreement that began now is logged once. */
		if (began)
			typec_dp_log(index, generation, &display, &ucsi);

		/* The soonest a waiting difference is due. */
		if (remaining != 0 &&
		    (earliest == 0 ||
		     remaining < earliest))
			earliest = remaining;
	}

	/* Succeeded: the milliseconds until the next comparison is due. */
	return earliest;
}

/*
 * Writes every connector record as text, one line each, for the diagnostic
 * /dev/typec: whether something is attached, the partner, the power, the
 * plug's orientation, the Alternate Modes and the partner's PDOs.
 *
 * Returns the length of the text (without its terminating NUL), which is
 * cut short when the buffer is too small.
 */
size_t
drv_typec_text(
	char *buffer,
	size_t size)
{
	struct drv_typec_connector connector;
	struct drv_typec_display display;
	struct typec_text text;
	unsigned count;
	unsigned index;
	unsigned port;
	int error;

	/* An empty text in the caller's buffer. */
	text.buffer = buffer;
	text.size = size;
	text.length = 0;
	if (size != 0)
		buffer[0] = '\0';

	/* One line for each connector, from a copy of its record. */
	count = drv_typec_connector_count();
	for (index = 0; index < count; index++) {
		/* A connector gone meanwhile (the PPM was reset) ends the text. */
		error = drv_typec_connector_get(index, &connector);
		if (error != 0)
			break;

		/* Its line. */
		typec_text_line(&text, index, &connector);
	}

	/* One line for each display port the display driver reported. */
	for (port = 0; port < DRV_TYPEC_DISPLAY_PORT_MAX; port++) {
		/* A port never reported has no line. */
		error = drv_typec_display_get(port, &display);
		if (error != 0)
			break;
		if (display.generation == 0)
			continue;

		/* Its line. */
		typec_text_display(&text, port, &display);
	}

	/* Succeeded: the length of what was written. */
	return text.length;
}

/*
 * Queues an operation under a new serial and wakes the connector driver.
 * Returns 0, ENOENT for a connector that is not there, or EBUSY when the
 * queue is full.
 */
static int
typec_request_put(
	struct drv_typec_request *request,
	uint32_t *serial)
{
	void (*kick)(void *argument);
	void *argument;
	unsigned slot;

	/* Into the ring, under a new serial. */
	drv_typec_os_lock();

	if (request->connector >= typec_count) {
		drv_typec_os_unlock();
		return ENOENT;
	}

	/* A full ring takes no more. */
	if (typec_request_count == DRV_TYPEC_REQUEST_MAX) {
		drv_typec_os_unlock();
		return EBUSY;
	}

	/* The operation under its serial, at the ring's end. */
	typec_request_serial++;
	request->serial = typec_request_serial;
	slot = (typec_request_first + typec_request_count) % DRV_TYPEC_REQUEST_MAX;
	typec_requests[slot] = *request;
	typec_request_count++;
	kick = typec_kick;
	argument = typec_kick_argument;

	drv_typec_os_unlock();

	/* The connector driver's thread wakes for it. */
	if (kick != NULL)
		kick(argument);

	/* Succeeded: the caller knows the operation by its serial. */
	if (serial != NULL)
		*serial = request->serial;
	return 0;
}

/*
 * Names the display port bound to a connector, or
 * DRV_TYPEC_DISPLAY_PORT_NONE.  The caller holds the layer's lock.
 */
static unsigned
typec_display_port_of(
	unsigned connector)
{
	unsigned port;

	/* The first port whose binding names the connector. */
	for (port = 0; port < DRV_TYPEC_DISPLAY_PORT_MAX; port++) {
		if (typec_displays[port].bound == connector + 1U)
			return port;
	}

	/* No port is bound to it. */
	return DRV_TYPEC_DISPLAY_PORT_NONE;
}

/* Tells whether two reports of one source say the same. */
static bool
typec_dp_same(
	const struct drv_typec_dp_state *left,
	const struct drv_typec_dp_state *right)
{
	/* Each field of the report. */
	if (left->known != right->known)
		return false;
	if (left->hpd != right->hpd)
		return false;
	if (left->pin != right->pin)
		return false;
	if (left->lanes != right->lanes)
		return false;
	if (left->orientation != right->orientation)
		return false;

	/* The two say the same. */
	return true;
}

/*
 * Tells whether the display driver's and UCSI's DisplayPort reports of one
 * connector differ: in the hot plug detect, in the pin assignment when
 * both name one, or in the orientation when both know it.  Two reports
 * differ only when both are known.
 */
static bool
typec_dp_differ(
	const struct drv_typec_dp_state *display,
	const struct drv_typec_dp_state *ucsi)
{
	/* Nothing to compare without both reports. */
	if (!display->known)
		return false;
	if (!ucsi->known)
		return false;

	/* The hot plug detect. */
	if (display->hpd != ucsi->hpd)
		return true;

	/* The pin assignment, when both name one. */
	if (display->pin != DRV_TYPEC_DP_PIN_NONE &&
	    ucsi->pin != DRV_TYPEC_DP_PIN_NONE &&
	    display->pin != ucsi->pin)
		return true;

	/* The orientation, when both know it. */
	if (display->orientation != DRV_TYPEC_ORIENTATION_UNKNOWN &&
	    ucsi->orientation != DRV_TYPEC_ORIENTATION_UNKNOWN &&
	    display->orientation != ucsi->orientation)
		return true;

	/* The reports agree. */
	return false;
}

/*
 * Moves a connector's comparison of its two DisplayPort reports on to the
 * time now: a difference starts a wait, and one that lasts
 * DRV_TYPEC_DISAGREE_MS is a disagreement; agreement ends either.  Sets
 * *remaining to the milliseconds a difference still waits (0: none).
 * Returns true when a disagreement begins now, which the caller logs after
 * the lock is released.  The caller holds the layer's lock.
 */
static bool
typec_dp_compare(
	unsigned connector,
	uint64_t now,
	uint32_t *remaining)
{
	struct typec_dp_watch *watch;
	uint64_t elapsed;
	unsigned port;
	bool differ;

	/* The connector's two reports; an unbound connector has only UCSI's. */
	watch = &typec_dp_watches[connector];
	*remaining = 0;
	differ = false;
	port = typec_display_port_of(connector);
	if (port != DRV_TYPEC_DISPLAY_PORT_NONE)
		differ = typec_dp_differ(&typec_displays[port].state, &typec_connectors[connector].dp_ucsi);

	/* Agreement ends a wait and a disagreement, so the next difference is logged again. */
	if (!differ) {
		watch->differing = false;
		watch->disagree = false;
		return false;
	}

	/* A new difference waits, since the two reports come in no fixed order. */
	if (!watch->differing) {
		watch->differing = true;
		watch->since_ms = now;
		*remaining = DRV_TYPEC_DISAGREE_MS;
		return false;
	}

	/* A disagreement already begun is not logged again. */
	if (watch->disagree)
		return false;

	/* A difference not old enough waits the rest of the time. */
	elapsed = now - watch->since_ms;
	if (elapsed < DRV_TYPEC_DISAGREE_MS) {
		*remaining = DRV_TYPEC_DISAGREE_MS - (uint32_t)elapsed;
		return false;
	}

	/*
	 * The difference has lasted: the record shows the disagreement until
	 * the reports agree again.
	 */
	watch->disagree = true;

	/* Succeeded: a disagreement begins. */
	return true;
}

/*
 * Fills the layer's part of a copy of a connector record: the bound
 * display port and its report, the DisplayPort state taken, its source
 * and the disagreement.  The caller holds the layer's lock.
 */
static void
typec_dp_fill(
	unsigned index,
	struct drv_typec_connector *connector)
{
	unsigned port;

	/* The bound port and what the display driver reports of it. */
	port = typec_display_port_of(index);
	connector->display_port = port;
	kern_memset(&connector->dp_display, 0, sizeof(connector->dp_display));
	if (port != DRV_TYPEC_DISPLAY_PORT_NONE)
		connector->dp_display = typec_displays[port].state;

	/* The display driver's report lights the display, so it is taken over UCSI's. */
	kern_memset(&connector->dp, 0, sizeof(connector->dp));
	connector->dp_source = DRV_TYPEC_DP_SOURCE_NONE;
	if (connector->dp_display.known) {
		connector->dp = connector->dp_display;
		connector->dp_source = DRV_TYPEC_DP_SOURCE_DISPLAY;
	} else if (connector->dp_ucsi.known) {
		connector->dp = connector->dp_ucsi;
		connector->dp_source = DRV_TYPEC_DP_SOURCE_UCSI;
	}

	/* An orientation the source taken does not know comes from the connector's status. */
	if (connector->dp_source != DRV_TYPEC_DP_SOURCE_NONE &&
	    connector->dp.orientation == DRV_TYPEC_ORIENTATION_UNKNOWN)
		connector->dp.orientation = connector->orientation;

	/* Whether the two reports disagree. */
	connector->dp_disagree = typec_dp_watches[index].disagree;
}

/* Logs a disagreement that began between a connector's two DisplayPort reports. */
static void
typec_dp_log(
	unsigned connector,
	uint64_t generation,
	const struct drv_typec_dp_state *display,
	const struct drv_typec_dp_state *ucsi)
{
	/* Both reports, and the generation they disagree at. */
	drv_typec_os_log("typec: connector %u: DisplayPort disagree: display hpd=%d pin=%c, ucsi hpd=%d pin=%c (generation %llu)\n", connector + 1U, (int)display->hpd, typec_text_pin(display->pin), (int)ucsi->hpd, typec_text_pin(ucsi->pin), (unsigned long long)generation);
}

/* Tells each registered listener that a connector changed at a generation. */
static void
typec_listeners_tell(
	unsigned connector,
	uint64_t generation)
{
	struct typec_listener_entry listeners[DRV_TYPEC_LISTENER_MAX];
	unsigned listener_count;
	unsigned listener;

	/* Takes a copy of the listeners, which are called without the lock. */
	drv_typec_os_lock();

	listener_count = typec_listener_count;
	kern_memcpy(listeners, typec_listeners, sizeof(listeners));

	drv_typec_os_unlock();

	/* Tells each listener. */
	for (listener = 0; listener < listener_count; listener++)
		listeners[listener].listener(listeners[listener].argument, connector, generation);
}

/* Writes the line of one connector. */
static void
typec_text_line(
	struct typec_text *text,
	unsigned index,
	const struct drv_typec_connector *connector)
{
	const char *separator;
	const char *cable;
	const char *role;
	unsigned mode;
	unsigned pdo;

	/* The connector's number as UCSI counts (from 1), and whether something is attached. */
	typec_text_append(text, "connector %u:", index + 1U);
	if (!connector->connected) {
		typec_text_append(text, " detached");
	} else {
		role = "sink";
		if (connector->power_role == DRV_TYPEC_ROLE_SOURCE)
			role = "source";
		typec_text_append(text, " attached partner=%s", typec_text_partner(connector->partner_type));
		typec_text_append(text, " power=%s role=%s", typec_text_power(connector->power_operation), role);
	}

	/* What is carried to the partner, each as a word of its own. */
	if ((connector->partner_flags & DRV_TYPEC_PARTNER_USB) != 0)
		typec_text_append(text, " usb");
	if ((connector->partner_flags & DRV_TYPEC_PARTNER_ALT_MODE) != 0)
		typec_text_append(text, " alt-mode");
	if ((connector->partner_flags & DRV_TYPEC_PARTNER_USB4) != 0)
		typec_text_append(text, " usb4");

	/* The plug's orientation, when the connector driver knows it. */
	if (connector->orientation == DRV_TYPEC_ORIENTATION_NORMAL) {
		typec_text_append(text, " orientation=normal");
	} else if (connector->orientation == DRV_TYPEC_ORIENTATION_FLIPPED) {
		typec_text_append(text, " orientation=flipped");
	}

	/* The contract's Request Data Object, when there is one. */
	if (connector->request_data_object != 0)
		typec_text_append(text, " rdo=0x%08x", (unsigned)connector->request_data_object);

	/* The Alternate Modes of the connector, the partner and the cable. */
	typec_text_modes(text, "modes", &connector->connector_modes);
	typec_text_modes(text, "partner-modes", &connector->partner_modes);
	typec_text_modes(text, "cable-modes", &connector->cable_modes);

	/* The connector modes it is in, by their SVIDs. */
	for (mode = 0; mode < connector->current_mode_count; mode++) {
		/* An index the list does not hold names no mode. */
		if (connector->current_modes[mode] >= connector->connector_modes.count)
			continue;

		/* The mode's SVID, the first after the word. */
		separator = ",";
		if (mode == 0)
			separator = " current=";
		typec_text_append(text, "%s%04x", separator, (unsigned)connector->connector_modes.modes[connector->current_modes[mode]].svid);
	}

	/* The partner's Power Data Objects. */
	for (pdo = 0; pdo < connector->partner_pdo_count; pdo++) {
		/* The PDO, the first after the word. */
		separator = ",";
		if (pdo == 0)
			separator = " pdos=";
		typec_text_append(text, "%s0x%08x", separator, (unsigned)connector->partner_pdos[pdo]);
	}

	/* What the cable reports of itself. */
	if (connector->cable.known) {
		cable = "passive";
		if (connector->cable.active)
			cable = "active";
		typec_text_append(text, " cable=%s speed=%llu current=%umA", cable, (unsigned long long)connector->cable.speed_bps, connector->cable.current_ma);
	}

	/* DisplayPort on the connector, from the display driver and from UCSI. */
	typec_text_dp(text, connector);

	/* The last operation carried out, and its outcome. */
	if (connector->request_serial != 0)
		typec_text_append(text, " request=%u error=%d", (unsigned)connector->request_serial, connector->request_error);

	/* The generation the record was published at, which ends the line. */
	typec_text_append(text, " generation=%llu\n", (unsigned long long)connector->generation);
}

/*
 * Writes the DisplayPort state of a connector, nothing when neither source
 * reports: the hot plug detect and the pin assignment as taken, each with
 * its source and with UCSI's beside the display driver's, the lanes, the
 * bound display port (from 1) and a disagreement.
 */
static void
typec_text_dp(
	struct typec_text *text,
	const struct drv_typec_connector *connector)
{
	const char *source;
	bool beside;

	/* The bound display port, even before it reports. */
	if (connector->display_port != DRV_TYPEC_DISPLAY_PORT_NONE)
		typec_text_append(text, " display-port=%u", connector->display_port + 1U);

	/* Neither source reports DisplayPort. */
	if (connector->dp_source == DRV_TYPEC_DP_SOURCE_NONE)
		return;

	/* The source taken, and whether UCSI's report stands beside the display driver's. */
	source = "ucsi";
	beside = false;
	if (connector->dp_source == DRV_TYPEC_DP_SOURCE_DISPLAY) {
		source = "display";
		if (connector->dp_ucsi.known)
			beside = true;
	}

	/* The hot plug detect. */
	typec_text_append(text, " hpd=%d(%s)", (int)connector->dp.hpd, source);
	if (beside)
		typec_text_append(text, " ucsi=%d", (int)connector->dp_ucsi.hpd);

	/* The pin assignment, when one is named. */
	if (connector->dp.pin != DRV_TYPEC_DP_PIN_NONE)
		typec_text_append(text, " pin=%c(%s)", typec_text_pin(connector->dp.pin), source);
	if (beside && connector->dp_ucsi.pin != DRV_TYPEC_DP_PIN_NONE)
		typec_text_append(text, " ucsi=%c", typec_text_pin(connector->dp_ucsi.pin));

	/* The lanes, when reported. */
	if (connector->dp.lanes != 0)
		typec_text_append(text, " lanes=%u", connector->dp.lanes);

	/* A disagreement between the two. */
	if (connector->dp_disagree)
		typec_text_append(text, " disagree");
}

/* Writes the line of one display port the display driver reported. */
static void
typec_text_display(
	struct typec_text *text,
	unsigned port,
	const struct drv_typec_display *display)
{
	/* The port as the display driver counts (from 1), and its report. */
	typec_text_append(text, "display-port %u: hpd=%d pin=%c lanes=%u", port + 1U, (int)display->state.hpd, typec_text_pin(display->state.pin), display->state.lanes);

	/* The orientation, when the display driver knows it. */
	if (display->state.orientation == DRV_TYPEC_ORIENTATION_NORMAL) {
		typec_text_append(text, " orientation=normal");
	} else if (display->state.orientation == DRV_TYPEC_ORIENTATION_FLIPPED) {
		typec_text_append(text, " orientation=flipped");
	}

	/* The connector it is bound to (from 1). */
	if (display->connector == DRV_TYPEC_CONNECTOR_NONE) {
		typec_text_append(text, " connector=none");
	} else {
		typec_text_append(text, " connector=%u", display->connector + 1U);
	}

	/* The generation of the report, which ends the line. */
	typec_text_append(text, " generation=%llu\n", (unsigned long long)display->generation);
}

/* Names a pin assignment by its letter, '-' for none. */
static char
typec_text_pin(
	enum drv_typec_dp_pin pin)
{
	/* No pin assignment, or one beyond F. */
	if (pin == DRV_TYPEC_DP_PIN_NONE)
		return '-';
	if ((unsigned)pin > (unsigned)DRV_TYPEC_DP_PIN_F)
		return '-';

	/* Succeeded: A for 1. */
	return (char)('A' + (int)pin - 1);
}

/* Writes a list of Alternate Modes as SVID/VDO pairs, nothing for an empty one. */
static void
typec_text_modes(
	struct typec_text *text,
	const char *name,
	const struct drv_typec_alt_mode_list *list)
{
	unsigned mode;

	/* Each mode, the first after the list's name. */
	for (mode = 0; mode < list->count; mode++) {
		/* The list's name before its first mode, a comma before the others. */
		if (mode == 0) {
			typec_text_append(text, " %s=", name);
		} else {
			typec_text_append(text, ",");
		}

		/* The mode. */
		typec_text_append(text, "%04x/%08x", (unsigned)list->modes[mode].svid, (unsigned)list->modes[mode].vdo);
	}
}

/* Names the kind of the attached partner. */
static const char *
typec_text_partner(
	enum drv_typec_partner_type type)
{
	/* The kinds of GET_CONNECTOR_STATUS. */
	switch (type) {
	case DRV_TYPEC_PARTNER_DFP:
		return "dfp";
	case DRV_TYPEC_PARTNER_UFP:
		return "ufp";
	case DRV_TYPEC_PARTNER_POWERED_CABLE:
		return "powered-cable";
	case DRV_TYPEC_PARTNER_POWERED_CABLE_UFP:
		return "powered-cable-ufp";
	case DRV_TYPEC_PARTNER_DEBUG_ACCESSORY:
		return "debug-accessory";
	case DRV_TYPEC_PARTNER_AUDIO_ACCESSORY:
		return "audio-accessory";
	default:
		break;
	}

	/* No partner, or a kind the layer does not name. */
	return "none";
}

/* Names how power is delivered. */
static const char *
typec_text_power(
	enum drv_typec_power_operation operation)
{
	/* The power operation modes of GET_CONNECTOR_STATUS. */
	switch (operation) {
	case DRV_TYPEC_POWER_USB_DEFAULT:
		return "usb-default";
	case DRV_TYPEC_POWER_BC:
		return "bc";
	case DRV_TYPEC_POWER_PD:
		return "pd";
	case DRV_TYPEC_POWER_TYPEC_1_5A:
		return "typec-1.5a";
	case DRV_TYPEC_POWER_TYPEC_3A:
		return "typec-3a";
	case DRV_TYPEC_POWER_TYPEC_5A:
		return "typec-5a";
	default:
		break;
	}

	/* A mode the PPM did not report. */
	return "unknown";
}

/* Appends formatted text, dropping what does not fit. */
static void
typec_text_append(
	struct typec_text *text,
	const char *format,
	...)
{
	va_list arguments;
	size_t room;
	int length;

	/* Nothing fits in a full buffer (the last byte is the terminator's). */
	if (text->length + 1U >= text->size)
		return;

	/* Formats after what is there; the terminator always fits. */
	room = text->size - text->length;
	va_start(arguments, format);
	length = kern_vsnprintf(text->buffer + text->length, room, format, arguments);
	va_end(arguments);
	if (length < 0)
		return;

	/* Moves past what fitted. */
	if ((size_t)length >= room) {
		text->length = text->size - 1U;
	} else {
		text->length += (size_t)length;
	}
}

/* Reads count bits of a little-endian bit string from bit first (bit 0 is byte 0's lowest). */
static unsigned
typec_location_bits(
	const uint8_t *buffer,
	unsigned first,
	unsigned count)
{
	unsigned value;
	unsigned bit;
	unsigned at;

	/* Each bit, lowest first. */
	value = 0U;
	for (bit = 0U; bit < count; bit++) {
		at = first + bit;
		if ((buffer[at / 8U] & (1U << (at % 8U))) != 0U)
			value |= 1U << bit;
	}

	/* The field. */
	return value;
}
