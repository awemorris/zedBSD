/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The USB Type-C connector layer (kernel-internal).
 *
 * A connector driver (the UCSI driver) keeps one record per USB-C
 * connector: whether something is attached, the power role and contract,
 * the partner's kind and the Alternate Modes of the connector, the
 * partner and the cable.  Other drivers (DisplayPort Alternate Mode,
 * power management) read a copy of a record and register a listener that
 * is told which connector changed.  Every change stamps the record with a
 * new generation, so a reader can tell whether what it holds is current.
 */

#ifndef KERN_DRIVERS_TYPEC_TYPEC_H
#define KERN_DRIVERS_TYPEC_TYPEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The most connectors kept (UCSI numbers up to 127; a PC has a few).
 */
#define DRV_TYPEC_CONNECTOR_MAX 16U

/*
 * The most Alternate Modes kept for each of the connector, the partner and
 * the cable.
 */
#define DRV_TYPEC_ALT_MODE_MAX 16U

/*
 * The most Power Data Objects a USB PD port advertises.
 */
#define DRV_TYPEC_PDO_MAX 7U

/*
 * The most Alternate Modes a connector is in at once that are kept.
 */
#define DRV_TYPEC_CURRENT_MODE_MAX 4U

/*
 * The most listeners registered at once.
 */
#define DRV_TYPEC_LISTENER_MAX 4U

/*
 * The most operations waiting for the connector driver at once.
 */
#define DRV_TYPEC_REQUEST_MAX 8U

/*
 * The Standard or Vendor ID of the DisplayPort Alternate Mode.
 */
#define DRV_TYPEC_SVID_DISPLAYPORT 0xFF01U

/*
 * The most Type-C ports a display driver reports of (Alder Lake-P has
 * TC1 to TC4).
 */
#define DRV_TYPEC_DISPLAY_PORT_MAX 4U

/*
 * The connector of a display port that is bound to none, and the display
 * port of a connector that has none bound.
 */
#define DRV_TYPEC_CONNECTOR_NONE 0xFFFFFFFFU
#define DRV_TYPEC_DISPLAY_PORT_NONE 0xFFFFFFFFU

/*
 * How long the display driver's and UCSI's DisplayPort state may differ
 * before the layer calls it a disagreement: the PPM's notification and the
 * display's hotplug interrupt come in no fixed order.
 */
#define DRV_TYPEC_DISAGREE_MS 200U

/*
 * What a connector can do (from its capability), as bits.
 */
enum drv_typec_capability {
	DRV_TYPEC_CAPABILITY_SOURCE_ONLY = 1U << 0,
	DRV_TYPEC_CAPABILITY_SINK_ONLY = 1U << 1,
	DRV_TYPEC_CAPABILITY_DUAL_ROLE = 1U << 2,
	DRV_TYPEC_CAPABILITY_AUDIO_ACCESSORY = 1U << 3,
	DRV_TYPEC_CAPABILITY_DEBUG_ACCESSORY = 1U << 4,
	DRV_TYPEC_CAPABILITY_USB2 = 1U << 5,
	DRV_TYPEC_CAPABILITY_USB3 = 1U << 6,
	DRV_TYPEC_CAPABILITY_ALT_MODE = 1U << 7,
	DRV_TYPEC_CAPABILITY_PROVIDER = 1U << 8,
	DRV_TYPEC_CAPABILITY_CONSUMER = 1U << 9,
	DRV_TYPEC_CAPABILITY_SWAP_TO_DFP = 1U << 10,
	DRV_TYPEC_CAPABILITY_SWAP_TO_UFP = 1U << 11,
	DRV_TYPEC_CAPABILITY_SWAP_TO_SOURCE = 1U << 12,
	DRV_TYPEC_CAPABILITY_SWAP_TO_SINK = 1U << 13,
};

/*
 * How power is delivered on an attached connector.
 */
enum drv_typec_power_operation {
	DRV_TYPEC_POWER_UNKNOWN = 0,
	DRV_TYPEC_POWER_USB_DEFAULT = 1,
	DRV_TYPEC_POWER_BC = 2,
	DRV_TYPEC_POWER_PD = 3,
	DRV_TYPEC_POWER_TYPEC_1_5A = 4,
	DRV_TYPEC_POWER_TYPEC_3A = 5,
	DRV_TYPEC_POWER_TYPEC_5A = 6,
};

/*
 * The role a connector plays in the power it carries.
 */
enum drv_typec_power_role {
	DRV_TYPEC_ROLE_SINK = 0,
	DRV_TYPEC_ROLE_SOURCE = 1,
};

/*
 * What is attached to a connector.
 */
enum drv_typec_partner_type {
	DRV_TYPEC_PARTNER_NONE = 0,
	DRV_TYPEC_PARTNER_DFP = 1,
	DRV_TYPEC_PARTNER_UFP = 2,
	DRV_TYPEC_PARTNER_POWERED_CABLE = 3,
	DRV_TYPEC_PARTNER_POWERED_CABLE_UFP = 4,
	DRV_TYPEC_PARTNER_DEBUG_ACCESSORY = 5,
	DRV_TYPEC_PARTNER_AUDIO_ACCESSORY = 6,
};

/*
 * What a connector is carrying to its partner, as bits.
 */
enum drv_typec_partner_flag {
	DRV_TYPEC_PARTNER_USB = 1U << 0,
	DRV_TYPEC_PARTNER_ALT_MODE = 1U << 1,
	DRV_TYPEC_PARTNER_USB4 = 1U << 2,
};

/*
 * Which way the plug is turned, when the connector driver knows.
 */
enum drv_typec_orientation {
	DRV_TYPEC_ORIENTATION_UNKNOWN = 0,
	DRV_TYPEC_ORIENTATION_NORMAL = 1,
	DRV_TYPEC_ORIENTATION_FLIPPED = 2,
};

/*
 * The role a connector plays in the data it carries (USB host or device).
 */
enum drv_typec_data_role {
	DRV_TYPEC_DATA_DFP = 0,
	DRV_TYPEC_DATA_UFP = 1,
};

/*
 * The DisplayPort pin assignment a partner chose (VESA DisplayPort Alt
 * Mode, A to F), numbered as the letters are: 1 is A.  Intel's FIA records
 * it with the same numbers (3 is C, 4 is D, 5 is E).
 */
enum drv_typec_dp_pin {
	DRV_TYPEC_DP_PIN_NONE = 0,
	DRV_TYPEC_DP_PIN_A = 1,
	DRV_TYPEC_DP_PIN_B = 2,
	DRV_TYPEC_DP_PIN_C = 3,
	DRV_TYPEC_DP_PIN_D = 4,
	DRV_TYPEC_DP_PIN_E = 5,
	DRV_TYPEC_DP_PIN_F = 6,
};

/*
 * Which report a connector's DisplayPort state was taken from.
 */
enum drv_typec_dp_source {
	DRV_TYPEC_DP_SOURCE_NONE = 0,
	DRV_TYPEC_DP_SOURCE_DISPLAY = 1,
	DRV_TYPEC_DP_SOURCE_UCSI = 2,
};

/*
 * The kinds of connector reset: a USB PD Hard Reset, or a Data Reset
 * (which UCSI 2.0 and later offer).
 */
enum drv_typec_reset {
	DRV_TYPEC_RESET_HARD = 0,
	DRV_TYPEC_RESET_DATA = 1,
};

/*
 * What is at the far end of a cable.
 */
enum drv_typec_plug_end {
	DRV_TYPEC_PLUG_TYPE_A = 0,
	DRV_TYPEC_PLUG_TYPE_B = 1,
	DRV_TYPEC_PLUG_TYPE_C = 2,
	DRV_TYPEC_PLUG_OTHER = 3,
};

/*
 * The operations another driver asks of a connector.
 */
enum drv_typec_request_kind {
	DRV_TYPEC_REQUEST_DATA_ROLE = 1,
	DRV_TYPEC_REQUEST_POWER_ROLE = 2,
	DRV_TYPEC_REQUEST_RESET = 3,
	DRV_TYPEC_REQUEST_ENTER_MODE = 4,
	DRV_TYPEC_REQUEST_EXIT_MODE = 5,
};

/*
 * One operation waiting for the connector driver: its kind, the connector
 * (0-based), the role or the reset kind (value), the index into the
 * connector's modes and the mode-specific configuration (for DisplayPort,
 * its Configure VDO), and the serial it was given.
 */
struct drv_typec_request {
	enum drv_typec_request_kind kind;
	unsigned connector;
	unsigned value;
	unsigned mode;
	uint32_t configuration;
	uint32_t serial;
};

/*
 * What the attached cable reports of itself, when the connector driver
 * can ask (known is false otherwise).
 */
struct drv_typec_cable {
	bool known;
	uint64_t speed_bps;
	unsigned current_ma;
	bool vbus;
	bool active;
	bool directional;
	enum drv_typec_plug_end plug_end;
	bool modes;
};

/*
 * What one source reports of DisplayPort on a USB-C port: whether it
 * reported at all (known), the hot plug detect, the pin assignment, the
 * lanes DisplayPort has (0: not reported) and the plug's orientation.
 */
struct drv_typec_dp_state {
	bool known;
	bool hpd;
	enum drv_typec_dp_pin pin;
	unsigned lanes;
	enum drv_typec_orientation orientation;
};

/*
 * What the display driver last reported of one of its Type-C ports, and
 * the connector the port is bound to.
 *
 * A copy is what drv_typec_display_get() hands out.  generation is the
 * layer's generation at the report (0: never reported).
 */
struct drv_typec_display {
	uint64_t generation;
	struct drv_typec_dp_state state;
	unsigned connector;
};

/*
 * One Alternate Mode: its Standard or Vendor ID and the mode's VDO.
 */
struct drv_typec_alt_mode {
	uint16_t svid;
	uint32_t vdo;
};

/*
 * A list of Alternate Modes, in the order the connector driver reported
 * them (an index into the connector's list names a mode it can enter).
 */
struct drv_typec_alt_mode_list {
	struct drv_typec_alt_mode modes[DRV_TYPEC_ALT_MODE_MAX];
	unsigned count;
};

/*
 * Everything known about one connector at one generation.
 *
 * A copy is what drv_typec_connector_get() hands out; the connector
 * driver publishes a whole new record with drv_typec_connector_publish().
 */
struct drv_typec_connector {
	/* The generation this record was published at (0: never). */
	uint64_t generation;

	/* What the connector can do (enum drv_typec_capability bits). */
	uint32_t capability;

	/* Whether something is attached; the fields below need it. */
	bool connected;

	/* How power is delivered, and the role this side plays. */
	enum drv_typec_power_operation power_operation;
	enum drv_typec_power_role power_role;

	/* The attached partner's kind and what is carried to it (enum drv_typec_partner_flag bits). */
	enum drv_typec_partner_type partner_type;
	uint32_t partner_flags;

	/* The plug's orientation (unknown unless the connector driver reports it). */
	enum drv_typec_orientation orientation;

	/* The USB PD Request Data Object of the contract (0: none known). */
	uint32_t request_data_object;

	/* The Alternate Modes of the connector, the partner (SOP) and the cable (SOP'). */
	struct drv_typec_alt_mode_list connector_modes;
	struct drv_typec_alt_mode_list partner_modes;
	struct drv_typec_alt_mode_list cable_modes;

	/* Which connector modes can be entered now, one bit per index of connector_modes. */
	uint8_t supported_modes[(DRV_TYPEC_ALT_MODE_MAX + 7U) / 8U];

	/* The indexes into connector_modes of the modes the connector is in. */
	uint8_t current_modes[DRV_TYPEC_CURRENT_MODE_MAX];
	unsigned current_mode_count;

	/* The partner's Power Data Objects (its source ones when this side sinks, else its sink ones). */
	uint32_t partner_pdos[DRV_TYPEC_PDO_MAX];
	unsigned partner_pdo_count;

	/* The attached cable's properties. */
	struct drv_typec_cable cable;

	/*
	 * The last operation carried out on the connector (serial 0: none
	 * yet), its errno value, and for one the PPM failed the reason it gave
	 * (UCSI's GET_ERROR_STATUS Error Information bits; 0: none given).
	 */
	uint32_t request_serial;
	int request_error;
	uint32_t request_error_information;

	/*
	 * What UCSI reports of the DisplayPort mode the connector is in
	 * (GET_CAM_CS, UCSI 3.0 and later; not known otherwise).  The
	 * connector driver fills it.
	 */
	struct drv_typec_dp_state dp_ucsi;

	/*
	 * The layer fills the rest in each copy it hands out, and ignores what
	 * a published record holds there: the display port bound to the
	 * connector (DRV_TYPEC_DISPLAY_PORT_NONE when none), what the display
	 * driver reports of it, the state taken (the display driver's when it
	 * reports, which is what lights the display, else UCSI's) and its
	 * source, and whether the two have differed for DRV_TYPEC_DISAGREE_MS.
	 */
	unsigned display_port;
	struct drv_typec_dp_state dp_display;
	struct drv_typec_dp_state dp;
	enum drv_typec_dp_source dp_source;
	bool dp_disagree;
};

/*
 * A listener: told the connector index (0-based) and the generation of
 * each published change.  It runs on the thread that published the change
 * (the connector driver's, or the display driver's for a report of a bound
 * port) and must not block or wait for either.
 */
typedef void (*drv_typec_listener)(void *argument, unsigned connector, uint64_t generation);

/*
 * Registers a listener of connector changes.
 */
int
drv_typec_listener_register(
	drv_typec_listener listener,
	void *argument);

/*
 * Reports how many connectors there are.
 */
unsigned
drv_typec_connector_count(void);

/*
 * Copies the record of a connector.
 */
int
drv_typec_connector_get(
	unsigned index,
	struct drv_typec_connector *connector);

/*
 * Sets how many connectors the connector driver found (records reset).
 */
void
drv_typec_connectors_reset(
	unsigned count);

/*
 * Publishes a new record of a connector and tells the listeners.
 */
int
drv_typec_connector_publish(
	unsigned index,
	const struct drv_typec_connector *connector);

/*
 * Asks a connector to swap to a data role (and to accept the partner's
 * swaps); the outcome comes with a later published record.
 */
int
drv_typec_connector_set_data_role(
	unsigned index,
	enum drv_typec_data_role role,
	uint32_t *serial);

/*
 * Asks a connector to swap to a power role (and to accept the partner's
 * swaps); the outcome comes with a later published record.
 */
int
drv_typec_connector_set_power_role(
	unsigned index,
	enum drv_typec_power_role role,
	uint32_t *serial);

/*
 * Asks a connector to be reset; the outcome comes with a later published
 * record.
 */
int
drv_typec_connector_reset(
	unsigned index,
	enum drv_typec_reset kind,
	uint32_t *serial);

/*
 * Asks a connector to enter one of its Alternate Modes (an index into its
 * connector_modes) with a mode-specific configuration; the outcome comes
 * with a later published record.
 */
int
drv_typec_connector_enter_mode(
	unsigned index,
	unsigned mode,
	uint32_t configuration,
	uint32_t *serial);

/*
 * Asks a connector to leave one of its Alternate Modes; the outcome comes
 * with a later published record.
 */
int
drv_typec_connector_exit_mode(
	unsigned index,
	unsigned mode,
	uint32_t *serial);

/*
 * Names the function the layer calls when an operation waits, so the
 * connector driver's thread wakes (the connector driver calls it once).
 */
void
drv_typec_operator_set(
	void (*kick)(void *argument),
	void *argument);

/*
 * Takes the oldest waiting operation, for the connector driver's thread.
 */
bool
drv_typec_request_take(
	struct drv_typec_request *request);

/*
 * Cancels an operation that waits; its outcome ECANCELED is published.
 */
int
drv_typec_request_cancel(
	uint32_t serial);

/*
 * Tells the layer the connector driver stopped: every operation that waits
 * ends with an errno value, none is taken any more, and the records keep
 * the last state.
 */
unsigned
drv_typec_driver_stop(
	int error);

/*
 * Tells whether the connector driver stopped.
 */
bool
drv_typec_driver_stopped(void);

/*
 * Notes an operation's outcome in its connector's record, which the
 * connector driver publishes next.
 */
int
drv_typec_request_finish(
	const struct drv_typec_request *request,
	int error,
	uint32_t information);

/*
 * Records what the display driver reads of DisplayPort on one of its
 * Type-C ports (0-based, TC1 is 0).
 */
int
drv_typec_display_report(
	unsigned port,
	const struct drv_typec_dp_state *state);

/*
 * Forgets what the display driver reported of one of its Type-C ports
 * (the display driver stopped).
 */
int
drv_typec_display_forget(
	unsigned port);

/*
 * Copies what the display driver last reported of one of its Type-C ports.
 */
int
drv_typec_display_get(
	unsigned port,
	struct drv_typec_display *display);

/*
 * Binds a display port to the connector (0-based) it is wired to, or
 * unbinds it with DRV_TYPEC_CONNECTOR_NONE.
 */
int
drv_typec_display_bind(
	unsigned port,
	unsigned connector);

/*
 * A device's physical location as ACPI's _PLD buffer gives it (ACPI 6.5
 * section 6.1.8): whether it was read, whether it is visible to the user,
 * and its group token and group position, which name one physical
 * connector across the devices that sit on it (the USB-C connector, the
 * USB ports of its lanes).  The board's way to tell which display port
 * drives which connector (ws050-p005).
 */
struct drv_typec_location {
	bool known;
	bool visible;
	unsigned group_token;
	unsigned group_position;
};

/*
 * Reads a _PLD buffer into a location: 0, or EINVAL for a buffer too
 * short or of revision 0.
 */
int
drv_typec_location_decode(
	const uint8_t *buffer,
	size_t length,
	struct drv_typec_location *location);

/*
 * Finds the connector (0-based) at a display port's location: the one
 * visible connector of the same group token and position, or
 * DRV_TYPEC_CONNECTOR_NONE when none or more than one is.
 */
unsigned
drv_typec_location_match(
	const struct drv_typec_location *connectors,
	unsigned count,
	const struct drv_typec_location *port);

/*
 * Compares the display driver's and UCSI's DisplayPort state of every
 * bound connector, and reports how many milliseconds until a difference
 * seen now is old enough to be a disagreement (0: none waits).
 */
uint32_t
drv_typec_display_check(void);

/*
 * Writes every connector record as text, one line each (the diagnostic
 * /dev/typec).
 */
size_t
drv_typec_text(
	char *buffer,
	size_t size);

#endif
