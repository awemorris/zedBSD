/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * XKEYBOARD: the keyboard's map, state and indicators as the X Keyboard
 * Extension tells them.
 *
 * The server keeps one keyboard map, the core protocol's (the keysyms of
 * each keycode in two groups of two, and the keys of the eight
 * modifiers).  XKEYBOARD's map is made from it on each request, the way
 * the extension's protocol maps a core map: the first group is the core
 * map's first two keysyms and the second group the other two, each group
 * of a key is of one of the four canonical key types (ONE_LEVEL,
 * TWO_LEVEL, ALPHABETIC and KEYPAD), a Mode_switch key selects the second
 * group, and the modifier keys' actions come from a small table of symbol
 * interpretations that the compatibility map reports.  A change of the
 * core map is therefore a change of XKEYBOARD's, and its clients hear of
 * it with MapNotify.
 *
 * The keyboard's state is the core state's: the modifiers held and
 * locked, and the second group while a Mode_switch key is held (whose
 * modifier the core state carries instead).  A client that asked to use
 * the extension sees the group in its core events' state, as the
 * extension lays down.
 *
 * Requests: UseExtension, SelectEvents, Bell, GetState, LatchLockState,
 * GetControls, SetControls, GetMap, GetCompatMap, GetIndicatorState,
 * GetIndicatorMap, GetNamedIndicator, GetNames, GetGeometry (no
 * geometry), PerClientFlags, ListComponents (none), GetKbdByName (the
 * keyboard as it is), GetDeviceInfo and SetDebuggingFlags.  The requests
 * that would change the map, the names or the geometry are refused with
 * BadImplementation.  Events: NewKeyboardNotify is never needed (the
 * keycodes stay 8 to 255), MapNotify, StateNotify, IndicatorStateNotify
 * and BellNotify.
 *
 * Wire layouts follow the public X Keyboard Extension protocol
 * specification, version 1.0.
 */

#include "userland/desktop/xserver/internal.h"

#include <stdlib.h>
#include <string.h>

/* The extension's name and the version the server speaks. */
#define XKB_NAME			"XKEYBOARD"
#define XKB_MAJOR_VERSION		1U
#define XKB_MINOR_VERSION		0U

/* The requests (minor opcodes). */
#define XKB_USE_EXTENSION		0U
#define XKB_SELECT_EVENTS		1U
#define XKB_BELL			3U
#define XKB_GET_STATE			4U
#define XKB_LATCH_LOCK_STATE		5U
#define XKB_GET_CONTROLS		6U
#define XKB_SET_CONTROLS		7U
#define XKB_GET_MAP			8U
#define XKB_SET_MAP			9U
#define XKB_GET_COMPAT_MAP		10U
#define XKB_SET_COMPAT_MAP		11U
#define XKB_GET_INDICATOR_STATE		12U
#define XKB_GET_INDICATOR_MAP		13U
#define XKB_SET_INDICATOR_MAP		14U
#define XKB_GET_NAMED_INDICATOR		15U
#define XKB_SET_NAMED_INDICATOR		16U
#define XKB_GET_NAMES			17U
#define XKB_SET_NAMES			18U
#define XKB_GET_GEOMETRY		19U
#define XKB_SET_GEOMETRY		20U
#define XKB_PER_CLIENT_FLAGS		21U
#define XKB_LIST_COMPONENTS		22U
#define XKB_GET_KBD_BY_NAME		23U
#define XKB_GET_DEVICE_INFO		24U
#define XKB_SET_DEVICE_INFO		25U
#define XKB_SET_DEBUGGING_FLAGS		101U

/* The keyboard's device id (XInputExtension's master keyboard), and the specs that name it. */
#define XKB_DEVICE_ID			3U
#define XKB_USE_CORE_KEYBOARD		0x0100U

/* The keycodes the keyboard has. */
#define XKB_KEYCODE_MIN			8U
#define XKB_KEYCODE_MAX			255U
#define XKB_KEYS			(XKB_KEYCODE_MAX - XKB_KEYCODE_MIN + 1U)

/* The events' kinds (the xkbType byte, and the bit SelectEvents names each by). */
#define XKB_EVENT_MAP_NOTIFY		1U
#define XKB_EVENT_STATE_NOTIFY		2U
#define XKB_EVENT_INDICATOR_STATE	4U
#define XKB_EVENT_BELL_NOTIFY		8U

/* The parts of the map (GetMap's components, MapNotify's changes). */
#define XKB_MAP_KEY_TYPES		0x01U
#define XKB_MAP_KEY_SYMS		0x02U
#define XKB_MAP_MODIFIER_MAP		0x04U
#define XKB_MAP_EXPLICIT		0x08U
#define XKB_MAP_KEY_ACTIONS		0x10U
#define XKB_MAP_KEY_BEHAVIORS		0x20U
#define XKB_MAP_VIRTUAL_MODS		0x40U
#define XKB_MAP_VIRTUAL_MOD_MAP		0x80U
#define XKB_MAP_ALL			0xffU

/* The parts of the state StateNotify says changed. */
#define XKB_STATE_MODS			0x0001U
#define XKB_STATE_BASE_MODS		0x0002U
#define XKB_STATE_LOCKED_MODS		0x0008U
#define XKB_STATE_GROUP			0x0010U
#define XKB_STATE_BASE_GROUP		0x0020U
#define XKB_STATE_COMPAT		0x0100U
#define XKB_STATE_GRAB_MODS		0x0200U
#define XKB_STATE_COMPAT_GRAB		0x0400U
#define XKB_STATE_LOOKUP_MODS		0x0800U
#define XKB_STATE_COMPAT_LOOKUP		0x1000U
#define XKB_STATE_BUTTONS		0x2000U

/* The names GetNames knows (its which). */
#define XKB_NAMES_KEYCODES		0x0001U
#define XKB_NAMES_GEOMETRY		0x0002U
#define XKB_NAMES_SYMBOLS		0x0004U
#define XKB_NAMES_PHYS_SYMBOLS		0x0008U
#define XKB_NAMES_TYPES			0x0010U
#define XKB_NAMES_COMPAT		0x0020U
#define XKB_NAMES_TYPE_NAMES		0x0040U
#define XKB_NAMES_LEVEL_NAMES		0x0080U
#define XKB_NAMES_INDICATORS		0x0100U
#define XKB_NAMES_KEYS			0x0200U
#define XKB_NAMES_ALIASES		0x0400U
#define XKB_NAMES_VIRTUAL_MODS		0x0800U
#define XKB_NAMES_GROUPS		0x1000U
#define XKB_NAMES_RADIO_GROUPS		0x2000U
#define XKB_NAMES_ALL			0x3fffU

/* GetKbdByName's components. */
#define XKB_GBN_TYPES			0x01U
#define XKB_GBN_COMPAT			0x02U
#define XKB_GBN_CLIENT_SYMBOLS		0x04U
#define XKB_GBN_SERVER_SYMBOLS		0x08U
#define XKB_GBN_INDICATORS		0x10U
#define XKB_GBN_KEY_NAMES		0x20U
#define XKB_GBN_OTHER_NAMES		0x80U
#define XKB_GBN_MAP			(XKB_GBN_TYPES | XKB_GBN_CLIENT_SYMBOLS | XKB_GBN_SERVER_SYMBOLS)
#define XKB_GBN_NAMES			(XKB_GBN_KEY_NAMES | XKB_GBN_OTHER_NAMES)
#define XKB_GBN_REPORTED		(XKB_GBN_MAP | XKB_GBN_COMPAT | XKB_GBN_INDICATORS | XKB_GBN_NAMES)

/* The canonical key types, in the order the protocol fixes for them. */
#define XKB_TYPE_ONE_LEVEL		0U
#define XKB_TYPE_TWO_LEVEL		1U
#define XKB_TYPE_ALPHABETIC		2U
#define XKB_TYPE_KEYPAD			3U
#define XKB_TYPES			4U

/* The levels of all the key types together (GetNames' level names). */
#define XKB_TYPE_LEVELS			7U

/* The most map entries and levels one key type has. */
#define XKB_TYPE_ENTRIES_MAX		2U
#define XKB_TYPE_LEVELS_MAX		2U

/* The groups a key may have, and the most keysyms one group has. */
#define XKB_GROUPS			2U

/* The virtual modifiers (their bits) and how many there are. */
#define XKB_VMOD_NUM_LOCK		0U
#define XKB_VMOD_ALT			1U
#define XKB_VMOD_META			2U
#define XKB_VMOD_SUPER			3U
#define XKB_VMOD_HYPER			4U
#define XKB_VMOD_ALT_GR			5U
#define XKB_VMOD_SCROLL_LOCK		6U
#define XKB_VMOD_LEVEL_THREE		7U
#define XKB_VMODS			8U
#define XKB_VMODS_MASK			0x00ffU
#define XKB_NO_VMOD			0xffU

/* The real modifiers by bit. */
#define XKB_SHIFT			0x01U
#define XKB_LOCK			0x02U

/* The indicators: Caps Lock, Num Lock and Scroll Lock, all three physical. */
#define XKB_INDICATORS			3U
#define XKB_INDICATORS_MASK		0x00000007U
#define XKB_INDICATORS_ALL		32U

/* How an indicator follows the modifiers: by those locked. */
#define XKB_IM_USE_LOCKED		0x04U

/* The actions (their types and flags). */
#define XKB_SA_NO_ACTION		0x00U
#define XKB_SA_SET_MODS			0x01U
#define XKB_SA_LOCK_MODS		0x03U
#define XKB_SA_SET_GROUP		0x04U
#define XKB_SA_CLEAR_LOCKS		0x01U
#define XKB_SA_USE_MOD_MAP_MODS		0x04U
#define XKB_ACTION_SIZE			8U

/* The symbol interpretations' matches, and their flag that only a key's first level counts. */
#define XKB_SI_ANY_OF_OR_NONE		1U
#define XKB_SI_ANY_OF			2U
#define XKB_SI_LEVEL_ONE_ONLY		0x80U
#define XKB_SI_LOCKING_KEY		0x02U

/* The per-client flags the server keeps (all but DetectableAutoRepeat, which the backends cannot tell). */
#define XKB_PCF_SUPPORTED		0x0000001eU

/* The controls: RepeatKeys, and the per-key repeat bit of SetControls' changes. */
#define XKB_CONTROL_REPEAT_KEYS		0x00000001U
#define XKB_CONTROL_PER_KEY_REPEAT	0x40000000U

/* The keyboard's repeat as GetControls tells it, in milliseconds (the system repeats the keys). */
#define XKB_REPEAT_DELAY		500U
#define XKB_REPEAT_INTERVAL		33U

/* The keysyms the map treats apart. */
#define KEYSYM_MODE_SWITCH		0xff7eU
#define KEYSYM_NUM_LOCK			0xff7fU
#define KEYSYM_SCROLL_LOCK		0xff14U
#define KEYSYM_SHIFT_L			0xffe1U
#define KEYSYM_SHIFT_LOCK		0xffe6U
#define KEYSYM_CAPS_LOCK		0xffe5U
#define KEYSYM_META_L			0xffe7U
#define KEYSYM_META_R			0xffe8U
#define KEYSYM_ALT_L			0xffe9U
#define KEYSYM_ALT_R			0xffeaU
#define KEYSYM_SUPER_L			0xffebU
#define KEYSYM_SUPER_R			0xffecU
#define KEYSYM_HYPER_L			0xffedU
#define KEYSYM_HYPER_R			0xffeeU
#define KEYSYM_LEVEL_THREE		0xfe03U
#define KEYSYM_KEYPAD_FIRST		0xff80U
#define KEYSYM_KEYPAD_LAST		0xffbdU
#define KEYSYM_UNICODE			0x01000000U

/* The name the keyboard's components, its device and its groups go by. */
#define XKB_COMPONENT_NAME		"core"
#define XKB_DEVICE_NAME			"Virtual core keyboard"
#define XKB_DEVICE_TYPE			"KEYBOARD"

/* The bit of a core event's state that starts the group (13 and 14). */
#define XKB_GROUP_SHIFT			13U
#define XKB_GROUP_BITS			0x6000U

/*
 * One canonical key type: its name and its levels' names, the real and
 * virtual modifiers it looks at, its levels, and its map entries (the
 * level each combination of modifiers chooses; none is level 1).
 */
struct xkb_type {
	const char *name;
	const char *level_names[XKB_TYPE_LEVELS_MAX];
	uint8_t levels;
	uint8_t real_mods;
	uint16_t vmods;
	uint8_t entry_count;
	uint8_t entry_level[XKB_TYPE_ENTRIES_MAX];
	uint8_t entry_real[XKB_TYPE_ENTRIES_MAX];
	uint16_t entry_vmods[XKB_TYPE_ENTRIES_MAX];
};

/*
 * One symbol interpretation of the compatibility map: the keysym it
 * matches (0: any), how a key's modifiers must match (with the flag that
 * only its first level counts), the virtual modifier it binds the key to,
 * whether the key locks, and the action it gives the key.  The table is
 * both what GetCompatMap reports and where the keys' actions come from:
 * the first match wins.
 */
struct xkb_interpret {
	uint32_t keysym;
	uint8_t match;
	uint8_t vmod;
	uint8_t flags;
	uint8_t action;
};

/*
 * One indicator: its name, and the locked modifiers (real, or virtual)
 * that light it.
 */
struct xkb_indicator {
	const char *name;
	uint8_t real_mods;
	uint8_t vmod;
};

/*
 * One key as XKEYBOARD shows it, made from the core map: its groups and
 * each one's key type, the keysyms of its levels (width a group), each
 * level's action, and its real and virtual modifier maps.
 */
struct xkb_key {
	unsigned groups;
	unsigned width;
	unsigned types[XKB_GROUPS];
	uint32_t syms[XKB_GROUPS][XKB_TYPE_LEVELS_MAX];
	uint8_t actions[XKB_GROUPS][XKB_TYPE_LEVELS_MAX][XKB_ACTION_SIZE];
	int acted;
	uint8_t modmap;
	uint16_t vmodmap;
};

/*
 * A reply being written: its bytes in a client's byte order, growing as
 * they come.  failed is set when it could not grow, and the request is
 * then refused with BadAlloc.
 */
struct xkb_buffer {
	uint8_t *bytes;
	size_t used;
	size_t capacity;
	int msb;
	int failed;
};

/* The canonical key types: ONE_LEVEL, TWO_LEVEL, ALPHABETIC (Shift or Lock) and KEYPAD (Shift or NumLock). */
static const struct xkb_type xkb_types[XKB_TYPES] = {
	{ "ONE_LEVEL", { "Any", NULL }, 1U, 0U, 0U, 0U, { 0U, 0U }, { 0U, 0U }, { 0U, 0U } },
	{ "TWO_LEVEL", { "Base", "Shift" }, 2U, XKB_SHIFT, 0U, 1U, { 1U, 0U }, { XKB_SHIFT, 0U }, { 0U, 0U } },
	{ "ALPHABETIC", { "Base", "Caps" }, 2U, XKB_SHIFT | XKB_LOCK, 0U, 2U, { 1U, 1U }, { XKB_SHIFT, XKB_LOCK }, { 0U, 0U } },
	{ "KEYPAD", { "Base", "Number" }, 2U, XKB_SHIFT, 1U << XKB_VMOD_NUM_LOCK, 2U, { 1U, 1U }, { XKB_SHIFT, 0U }, { 0U, 1U << XKB_VMOD_NUM_LOCK } }
};

/* The virtual modifiers' names, by their bits. */
static const char *const xkb_vmod_names[XKB_VMODS] = {
	"NumLock", "Alt", "Meta", "Super", "Hyper", "AltGr", "ScrollLock", "LevelThree"
};

/*
 * The symbol interpretations: the locking keys lock their modifiers,
 * Mode_switch selects the second group, the named modifier keys bind
 * their virtual modifiers, and any key in a modifier sets it.
 */
static const struct xkb_interpret xkb_interprets[] = {
	{ KEYSYM_CAPS_LOCK, XKB_SI_ANY_OF_OR_NONE, XKB_NO_VMOD, XKB_SI_LOCKING_KEY, XKB_SA_LOCK_MODS },
	{ KEYSYM_SHIFT_LOCK, XKB_SI_ANY_OF_OR_NONE, XKB_NO_VMOD, XKB_SI_LOCKING_KEY, XKB_SA_LOCK_MODS },
	{ KEYSYM_NUM_LOCK, XKB_SI_ANY_OF_OR_NONE, XKB_VMOD_NUM_LOCK, XKB_SI_LOCKING_KEY, XKB_SA_LOCK_MODS },
	{ KEYSYM_MODE_SWITCH, XKB_SI_ANY_OF_OR_NONE, XKB_VMOD_ALT_GR, 0U, XKB_SA_SET_GROUP },
	{ KEYSYM_ALT_L, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_ALT, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_ALT_R, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_ALT, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_META_L, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_META, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_META_R, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_META, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_SUPER_L, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_SUPER, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_SUPER_R, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_SUPER, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_HYPER_L, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_HYPER, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_HYPER_R, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_HYPER, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_SCROLL_LOCK, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_SCROLL_LOCK, 0U, XKB_SA_SET_MODS },
	{ KEYSYM_LEVEL_THREE, XKB_SI_ANY_OF_OR_NONE | XKB_SI_LEVEL_ONE_ONLY, XKB_VMOD_LEVEL_THREE, 0U, XKB_SA_SET_MODS },
	{ 0U, XKB_SI_ANY_OF | XKB_SI_LEVEL_ONE_ONLY, XKB_NO_VMOD, 0U, XKB_SA_SET_MODS }
};

/* How many symbol interpretations the table has. */
#define XKB_INTERPRETS		(sizeof(xkb_interprets) / sizeof(xkb_interprets[0]))

/* The indicators: Caps Lock (Lock locked), Num Lock and Scroll Lock (their virtual modifiers locked). */
static const struct xkb_indicator xkb_indicators[XKB_INDICATORS] = {
	{ "Caps Lock", XKB_LOCK, XKB_NO_VMOD },
	{ "Num Lock", 0U, XKB_VMOD_NUM_LOCK },
	{ "Scroll Lock", 0U, XKB_VMOD_SCROLL_LOCK }
};

/* The bytes of each kind of event's details in SelectEvents (0: MapNotify, whose details are in the request's fixed part). */
static const uint8_t xkb_detail_sizes[X11_XKB_EVENT_KINDS] = {
	2U, 0U, 2U, 4U, 4U, 4U, 2U, 1U, 1U, 1U, 2U, 2U
};

static unsigned xkb_use_extension(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_select_events(struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_bell(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_state(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xkb_latch_lock_state(struct x11server *server, const uint8_t *request, size_t length);
static unsigned xkb_get_controls(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xkb_set_controls(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_map(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_compat_map(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_indicator_state(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xkb_get_indicator_map(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_named_indicator(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_names(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_geometry(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_per_client_flags(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_list_components(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xkb_get_kbd_by_name(struct x11server *server, struct x11_client *client, const uint8_t *request, size_t length);
static unsigned xkb_get_device_info(struct x11server *server, struct x11_client *client, size_t length);
static unsigned xkb_set_debugging_flags(struct x11server *server, struct x11_client *client, size_t length);
static void xkb_write_map(struct xkb_buffer *buffer, const struct x11server *server, unsigned parts, const unsigned *first, const unsigned *count, uint16_t vmods);
static void xkb_write_types(struct xkb_buffer *buffer, const struct x11server *server, unsigned first, unsigned count);
static void xkb_write_compat(struct xkb_buffer *buffer, const struct x11server *server, unsigned groups, unsigned first, unsigned count);
static void xkb_write_action(struct xkb_buffer *buffer, uint8_t type, uint8_t flags, uint8_t mods, int8_t group);
static void xkb_write_indicator_map(struct xkb_buffer *buffer, const struct x11server *server, uint32_t which);
static void xkb_write_names(struct xkb_buffer *buffer, struct x11server *server, uint32_t which);
static void xkb_write_atom(struct xkb_buffer *buffer, struct x11server *server, const char *name);
static void xkb_finish(struct xkb_buffer *buffer, size_t start, const struct x11_client *client);
static void xkb_send(struct x11server *server, struct x11_client *client, struct xkb_buffer *buffer);
static void xkb_put(struct xkb_buffer *buffer, const void *bytes, size_t length);
static void xkb_put8(struct xkb_buffer *buffer, unsigned value);
static void xkb_put16(struct xkb_buffer *buffer, unsigned value);
static void xkb_put32(struct xkb_buffer *buffer, uint32_t value);
static void xkb_pad(struct xkb_buffer *buffer);
static void xkb_key_describe(const struct x11server *server, unsigned keycode, struct xkb_key *key);
static unsigned xkb_group_describe(uint32_t first, uint32_t second, uint32_t *syms);
static void xkb_level_action(uint32_t keysym, uint8_t modmap, int first_level, uint8_t *action);
static void xkb_case(uint32_t keysym, uint32_t *lower, uint32_t *upper);
static uint8_t xkb_modmap(const struct x11server *server, unsigned keycode);
static unsigned xkb_vmod_of(uint32_t keysym);
static uint8_t xkb_vmod_real(const struct x11server *server, unsigned vmod);
static uint8_t xkb_vmods_real(const struct x11server *server, uint16_t vmods);
static uint8_t xkb_mode_switch_mods(const struct x11server *server);
static unsigned xkb_groups_used(const struct x11server *server);
static uint32_t xkb_indicator_state(const struct x11server *server, const struct x11_xkb_state *state);
static uint8_t xkb_indicator_mods(const struct x11server *server, unsigned index);
static int xkb_keyboard(uint16_t spec);
static void xkb_state_check(struct x11server *server, uint8_t keycode, uint8_t type, uint8_t major, uint8_t minor);
static void xkb_event_header(uint8_t *event, struct x11server *server, const struct x11_client *client, unsigned kind);
static int xkb_selected(const struct x11_client *client, unsigned kind, uint32_t changed);

/*
 * Answers QueryExtension's name XKEYBOARD: its major opcode, first event
 * and first error.  Returns nonzero when the name is the extension's.
 */
int
x11_xkb_query(
	const uint8_t *name,
	size_t length,
	uint8_t *major,
	uint8_t *event,
	uint8_t *error)
{
	int differs;

	/* Another name. */
	if (length != sizeof(XKB_NAME) - 1U)
		return 0;
	differs = memcmp(name, XKB_NAME, length);
	if (differs != 0)
		return 0;

	/* Succeeded: the extension's numbers. */
	*major = (uint8_t)X11_XKB_MAJOR;
	*event = (uint8_t)X11_XKB_EVENT;
	*error = (uint8_t)X11_XKB_ERROR;
	return 1;
}

/*
 * Answers an XKEYBOARD request.  A client must have asked to use the
 * extension first, and a request must name the core keyboard.  Returns
 * 0, or the error to send.
 */
unsigned
x11_xkb_request(
	struct x11server *server,
	unsigned index,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *client;
	uint16_t spec;
	unsigned code;
	int keyboard;

	/* UseExtension comes before the others, which name the keyboard. */
	client = &server->clients[index];
	if (length < 4U)
		return X11_BAD_LENGTH;
	if (request[1] == XKB_USE_EXTENSION) {
		code = xkb_use_extension(server, client, request, length);
		return code;
	}
	if (!client->xkb_used)
		return X11_BAD_ACCESS;

	/* The keyboard each request but SetDebuggingFlags names. */
	if (request[1] != XKB_SET_DEBUGGING_FLAGS) {
		if (length < 8U)
			return X11_BAD_LENGTH;
		spec = x11_read16(request + 4, client->order);
		keyboard = xkb_keyboard(spec);
		if (!keyboard)
			return X11_XKB_ERROR;
	}

	/* The request's own handler. */
	switch (request[1]) {
	case XKB_SELECT_EVENTS:
		code = xkb_select_events(client, request, length);
		break;
	case XKB_BELL:
		code = xkb_bell(server, client, request, length);
		break;
	case XKB_GET_STATE:
		code = xkb_get_state(server, client, length);
		break;
	case XKB_LATCH_LOCK_STATE:
		code = xkb_latch_lock_state(server, request, length);
		break;
	case XKB_GET_CONTROLS:
		code = xkb_get_controls(server, client, length);
		break;
	case XKB_SET_CONTROLS:
		code = xkb_set_controls(server, client, request, length);
		break;
	case XKB_GET_MAP:
		code = xkb_get_map(server, client, request, length);
		break;
	case XKB_GET_COMPAT_MAP:
		code = xkb_get_compat_map(server, client, request, length);
		break;
	case XKB_GET_INDICATOR_STATE:
		code = xkb_get_indicator_state(server, client, length);
		break;
	case XKB_GET_INDICATOR_MAP:
		code = xkb_get_indicator_map(server, client, request, length);
		break;
	case XKB_GET_NAMED_INDICATOR:
		code = xkb_get_named_indicator(server, client, request, length);
		break;
	case XKB_GET_NAMES:
		code = xkb_get_names(server, client, request, length);
		break;
	case XKB_GET_GEOMETRY:
		code = xkb_get_geometry(server, client, request, length);
		break;
	case XKB_PER_CLIENT_FLAGS:
		code = xkb_per_client_flags(server, client, request, length);
		break;
	case XKB_LIST_COMPONENTS:
		code = xkb_list_components(server, client, length);
		break;
	case XKB_GET_KBD_BY_NAME:
		code = xkb_get_kbd_by_name(server, client, request, length);
		break;
	case XKB_GET_DEVICE_INFO:
		code = xkb_get_device_info(server, client, length);
		break;
	case XKB_SET_DEBUGGING_FLAGS:
		code = xkb_set_debugging_flags(server, client, length);
		break;
	case XKB_SET_MAP:
	case XKB_SET_COMPAT_MAP:
	case XKB_SET_INDICATOR_MAP:
	case XKB_SET_NAMED_INDICATOR:
	case XKB_SET_NAMES:
	case XKB_SET_GEOMETRY:
	case XKB_SET_DEVICE_INFO:
		/* The map is the core map's: it changes through the core requests only. */
		code = X11_BAD_IMPLEMENTATION;
		break;
	default:
		code = X11_BAD_REQUEST;
		break;
	}

	/* Succeeded: answered, or the error to send. */
	return code;
}

/*
 * Splits a core state (modifiers, buttons) into the keyboard's state as
 * XKEYBOARD tells it: a Mode_switch key's modifier becomes the second
 * group, and the locked modifiers are the server's locks.
 */
void
x11_xkb_state_of(
	const struct x11server *server,
	uint16_t core,
	struct x11_xkb_state *state)
{
	uint8_t mode_switch;
	uint8_t mods;

	/* The modifiers that are Mode_switch's, which make the group. */
	mode_switch = xkb_mode_switch_mods(server);
	mods = (uint8_t)(core & 0xffU);

	/* The group, and the modifiers without Mode_switch's. */
	state->group = 0U;
	if ((mods & mode_switch) != 0U)
		state->group = 1U;
	state->mods = (uint8_t)(mods & ~mode_switch);
	state->locked = (uint8_t)(server->locks & state->mods);
	state->base = (uint8_t)(state->mods & ~state->locked);

	/* Succeeded: the core state a client without XKB sees, and the buttons. */
	state->compat = mods;
	state->buttons = (uint16_t)(core & 0x1f00U);
}

/*
 * Turns the state of a device or crossing event (least significant byte
 * first) into XKEYBOARD's for a client that uses the extension: the group
 * in bits 13 and 14 in place of a Mode_switch key's modifier.  An event
 * another client sent is left as it was sent.
 */
void
x11_xkb_event_state(
	const struct x11server *server,
	const struct x11_client *client,
	uint8_t *event)
{
	struct x11_xkb_state state;
	uint16_t core;
	uint16_t value;

	/* A client without XKB, a sent event, or not a device or crossing event. */
	if (!client->xkb_used)
		return;
	if ((event[0] & X11_EVENT_SENT) != 0U)
		return;
	if (event[0] < X11_EVENT_KEY_PRESS || event[0] > X11_EVENT_LEAVE_NOTIFY)
		return;

	/* The state split, and written back with the group. */
	core = x11_read16(event + 28, 0);
	x11_xkb_state_of(server, core, &state);
	value = (uint16_t)((core & 0x1f00U) | state.mods | ((unsigned)state.group << XKB_GROUP_SHIFT));

	/* Succeeded: the event's state. */
	x11_write16(event + 28, value, 0);
}

/*
 * Hears that a device event was handled: the clients that asked hear of
 * the change it made to the keyboard's state and to the indicators.
 */
void
x11_xkb_input_changed(
	struct x11server *server,
	const struct x11_device_event *event)
{
	uint8_t keycode;
	uint8_t type;

	/* The key or the button, and the core event it was. */
	keycode = 0U;
	type = 0U;
	if (event->kind == X11_DEVICE_KEY) {
		keycode = event->detail;
		type = X11_EVENT_KEY_RELEASE;
		if (event->pressed)
			type = X11_EVENT_KEY_PRESS;
	} else if (event->kind == X11_DEVICE_BUTTON) {
		type = X11_EVENT_BUTTON_RELEASE;
		if (event->pressed)
			type = X11_EVENT_BUTTON_PRESS;
	}

	/* Succeeded: StateNotify and IndicatorStateNotify when anything changed. */
	xkb_state_check(server, keycode, type, 0U, 0U);
}

/*
 * Hears that the core map changed (MappingNotify's request: the modifiers
 * or the keyboard; the pointer's map is not XKEYBOARD's): the clients
 * that asked hear MapNotify for the whole map, which is made anew from
 * the core map, then of a change of the state its modifiers make.
 */
void
x11_xkb_mapping_changed(
	struct x11server *server,
	uint8_t request)
{
	struct x11_client *client;
	uint8_t event[32];
	unsigned index;
	int selected;
	int msb;

	/* The pointer's map. */
	if (request > 1U)
		return;

	/* Each client that selected MapNotify of these parts. */
	for (index = 0U; index < server->clients_max; index++) {
		client = x11_client_of(server, index);
		if (client == NULL)
			continue;
		selected = xkb_selected(client, XKB_EVENT_MAP_NOTIFY, XKB_MAP_ALL & ~XKB_MAP_KEY_BEHAVIORS);
		if (!selected)
			continue;

		/* Every part, over every keycode and key type. */
		msb = client->order;
		xkb_event_header(event, server, client, XKB_EVENT_MAP_NOTIFY);
		x11_write16(event + 10, (uint16_t)(XKB_MAP_ALL & ~XKB_MAP_KEY_BEHAVIORS), msb);
		event[12] = (uint8_t)XKB_KEYCODE_MIN;
		event[13] = (uint8_t)XKB_KEYCODE_MAX;
		event[14] = 0U;
		event[15] = (uint8_t)XKB_TYPES;
		event[16] = (uint8_t)XKB_KEYCODE_MIN;
		event[17] = (uint8_t)XKB_KEYS;
		event[18] = (uint8_t)XKB_KEYCODE_MIN;
		event[19] = (uint8_t)XKB_KEYS;
		event[22] = (uint8_t)XKB_KEYCODE_MIN;
		event[23] = (uint8_t)XKB_KEYS;
		event[24] = (uint8_t)XKB_KEYCODE_MIN;
		event[25] = (uint8_t)XKB_KEYS;
		event[26] = (uint8_t)XKB_KEYCODE_MIN;
		event[27] = (uint8_t)XKB_KEYS;
		x11_write16(event + 28, (uint16_t)XKB_VMODS_MASK, msb);
		x11_client_send(server, client, event, sizeof(event));
	}

	/* Succeeded: the state the new modifiers make. */
	xkb_state_check(server, 0U, 0U, 0U, 0U);
}

/* UseExtension: the client uses XKEYBOARD from now on, when it speaks the server's version. */
static unsigned
xkb_use_extension(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	uint16_t wanted_major;

	/* The version the client speaks. */
	if (length != 8U)
		return X11_BAD_LENGTH;
	wanted_major = x11_read16(request + 4, client->order);

	/* The server's version; supported when the major versions are the same. */
	memset(reply, 0, sizeof(reply));
	if (wanted_major == XKB_MAJOR_VERSION) {
		reply[1] = 1U;
		client->xkb_used = 1;
	}
	x11_write16(reply + 8, (uint16_t)XKB_MAJOR_VERSION, client->order);
	x11_write16(reply + 10, (uint16_t)XKB_MINOR_VERSION, client->order);

	/* Succeeded: the reply. */
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * SelectEvents: the kinds of XKEYBOARD's events the client hears, each
 * with the details it names (cleared, all, or changed under a mask).
 * MapNotify's details are in the request's fixed part, the others' follow
 * it in the order of the kinds.
 */
static unsigned
xkb_select_events(
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint32_t affect_details;
	uint32_t values;
	uint32_t all;
	uint16_t affect_which;
	uint16_t clear;
	uint16_t select_all;
	uint16_t listed;
	size_t offset;
	size_t size;
	unsigned kind;
	int msb;

	/* The fixed part: the kinds affected, those cleared and those selected whole. */
	msb = client->order;
	if (length < 16U)
		return X11_BAD_LENGTH;
	affect_which = x11_read16(request + 6, msb);
	clear = x11_read16(request + 8, msb);
	select_all = x11_read16(request + 10, msb);
	if ((affect_which & ~((1U << X11_XKB_EVENT_KINDS) - 1U)) != 0U)
		return X11_BAD_VALUE;
	listed = (uint16_t)(affect_which & ~clear & ~select_all);

	/* The details listed must be there. */
	size = 16U;
	for (kind = 0U; kind < X11_XKB_EVENT_KINDS; kind++) {
		if ((listed & (1U << kind)) != 0U)
			size += 2U * xkb_detail_sizes[kind];
	}
	if (length < size)
		return X11_BAD_LENGTH;
	if (length > ((size + 3U) & ~(size_t)3U))
		return X11_BAD_LENGTH;

	/* Each kind affected: cleared, selected whole, or its details changed under the mask. */
	offset = 16U;
	for (kind = 0U; kind < X11_XKB_EVENT_KINDS; kind++) {
		if ((affect_which & (1U << kind)) == 0U)
			continue;

		/* Every detail a kind's field can carry. */
		all = 0xffffffffU;
		if (xkb_detail_sizes[kind] == 2U || kind == 1U)
			all = 0xffffU;
		if (xkb_detail_sizes[kind] == 1U)
			all = 0xffU;

		/* The mask of the details the request changes, and their values. */
		affect_details = 0U;
		values = 0U;
		if (kind == 1U) {
			affect_details = x11_read16(request + 12, msb);
			values = x11_read16(request + 14, msb);
		} else if ((listed & (1U << kind)) != 0U && xkb_detail_sizes[kind] == 1U) {
			affect_details = request[offset];
			values = request[offset + 1U];
			offset += 2U;
		} else if ((listed & (1U << kind)) != 0U && xkb_detail_sizes[kind] == 2U) {
			affect_details = x11_read16(request + offset, msb);
			values = x11_read16(request + offset + 2U, msb);
			offset += 4U;
		} else if ((listed & (1U << kind)) != 0U) {
			affect_details = x11_read32(request + offset, msb);
			values = x11_read32(request + offset + 4U, msb);
			offset += 8U;
		}

		/* The details from now on. */
		if ((clear & (1U << kind)) != 0U) {
			client->xkb_details[kind] = 0U;
		} else if ((select_all & (1U << kind)) != 0U) {
			client->xkb_details[kind] = all;
		} else {
			client->xkb_details[kind] = (client->xkb_details[kind] & ~affect_details) | (values & affect_details & all);
		}

		/* The kind is selected while any of its details is. */
		client->xkb_events &= ~(1U << kind);
		if (client->xkb_details[kind] != 0U)
			client->xkb_events |= 1U << kind;
	}

	/* Succeeded: selected. */
	return 0U;
}

/* Bell: the keyboard's bell rings (unless only the event is asked for), and the clients that asked hear BellNotify. */
static unsigned
xkb_bell(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct x11_client *listener;
	struct x11_window *window;
	uint8_t event[32];
	uint32_t window_id;
	uint32_t name;
	uint16_t pitch;
	uint16_t duration;
	unsigned index;
	int percent;
	int selected;
	int msb;

	/* The volume, whether only the event is asked for, the bell's name and its window. */
	msb = client->order;
	if (length != 28U)
		return X11_BAD_LENGTH;
	percent = (int8_t)request[10];
	if (percent < -100 || percent > 100)
		return X11_BAD_VALUE;
	pitch = x11_read16(request + 14, msb);
	duration = x11_read16(request + 16, msb);
	name = x11_read32(request + 20, msb);
	window_id = x11_read32(request + 24, msb);
	if (window_id != 0U) {
		window = x11_window_find(server, window_id);
		if (window == NULL)
			return X11_BAD_WINDOW;
	}

	/* Rung, by a backend that can, unless only the event is asked for. */
	if (request[12] == 0U && server->ops->bell != NULL)
		server->ops->bell(server->backend, percent);

	/* Succeeded: BellNotify to each client that asked. */
	for (index = 0U; index < server->clients_max; index++) {
		listener = x11_client_of(server, index);
		if (listener == NULL)
			continue;
		selected = xkb_selected(listener, XKB_EVENT_BELL_NOTIFY, 1U);
		if (!selected)
			continue;
		xkb_event_header(event, server, listener, XKB_EVENT_BELL_NOTIFY);
		event[9] = request[6];
		event[10] = request[8];
		event[11] = (uint8_t)percent;
		x11_write16(event + 12, pitch, listener->order);
		x11_write16(event + 14, duration, listener->order);
		x11_write32(event + 16, name, listener->order);
		x11_write32(event + 20, window_id, listener->order);
		event[24] = request[12];
		x11_client_send(server, listener, event, sizeof(event));
	}
	return 0U;
}

/* GetState: the keyboard's modifiers, group and the pointer's buttons. */
static unsigned
xkb_get_state(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	struct x11_xkb_state state;
	uint8_t reply[32];
	uint16_t core;

	/* The header alone. */
	if (length != 8U)
		return X11_BAD_LENGTH;

	/* The state now. */
	core = x11_input_state(server);
	x11_xkb_state_of(server, core, &state);

	/* Succeeded: the modifiers each way, the group, and the buttons. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XKB_DEVICE_ID;
	reply[8] = state.mods;
	reply[9] = state.base;
	reply[11] = state.locked;
	reply[12] = state.group;
	x11_write16(reply + 14, state.group, client->order);
	reply[18] = state.compat;
	reply[19] = state.mods;
	reply[20] = state.compat;
	reply[21] = state.mods;
	reply[22] = state.compat;
	x11_write16(reply + 24, state.buttons, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * LatchLockState: the locked modifiers the request names change.  The
 * server latches nothing and has no locked group, so those parts change
 * nothing.
 */
static unsigned
xkb_latch_lock_state(
	struct x11server *server,
	const uint8_t *request,
	size_t length)
{
	uint8_t affect;
	uint8_t locks;

	/* The modifiers whose locks change, and the locks. */
	if (length != 16U)
		return X11_BAD_LENGTH;
	affect = request[6];
	locks = request[7];

	/* Succeeded: locked or unlocked, and the clients that asked hear of it. */
	server->locks = (uint16_t)((server->locks & ~affect) | (locks & affect));
	xkb_state_check(server, 0U, 0U, (uint8_t)X11_XKB_MAJOR, (uint8_t)XKB_LATCH_LOCK_STATE);
	return 0U;
}

/* GetControls: the keyboard's controls: the groups, the repeat and each key's repeat. */
static unsigned
xkb_get_controls(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[92];
	uint32_t enabled;
	int msb;

	/* The header alone. */
	msb = client->order;
	if (length != 8U)
		return X11_BAD_LENGTH;

	/* The groups the map has, wrapped into range; the repeat's delay and interval. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XKB_DEVICE_ID;
	reply[8] = 1U;
	reply[9] = (uint8_t)xkb_groups_used(server);
	x11_write16(reply + 20, (uint16_t)XKB_REPEAT_DELAY, msb);
	x11_write16(reply + 22, (uint16_t)XKB_REPEAT_INTERVAL, msb);

	/* RepeatKeys while the keys repeat, and each key's repeat. */
	enabled = 0U;
	if (server->global_auto_repeat)
		enabled = XKB_CONTROL_REPEAT_KEYS;
	x11_write32(reply + 56, enabled, msb);
	memcpy(reply + 60, server->auto_repeats, 32U);

	/* Succeeded: the reply. */
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * SetControls: the controls the server keeps change (whether the keys
 * repeat, and each key's repeat); the others are taken and not kept.
 */
static unsigned
xkb_set_controls(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint32_t affect;
	uint32_t enabled;
	uint32_t change;
	int msb;

	/* The controls enabled and those changed. */
	msb = client->order;
	if (length != 100U)
		return X11_BAD_LENGTH;
	affect = x11_read32(request + 24, msb);
	enabled = x11_read32(request + 28, msb);
	change = x11_read32(request + 32, msb);

	/* RepeatKeys turns the keys' repeat on or off. */
	if ((affect & XKB_CONTROL_REPEAT_KEYS) != 0U) {
		server->global_auto_repeat = 0U;
		if ((enabled & XKB_CONTROL_REPEAT_KEYS) != 0U)
			server->global_auto_repeat = 1U;
	}

	/* Succeeded: each key's repeat. */
	if ((change & XKB_CONTROL_PER_KEY_REPEAT) != 0U)
		memcpy(server->auto_repeats, request + 68, 32U);
	return 0U;
}

/*
 * GetMap: the parts of the map the request names, each whole (full) or
 * over the range the request gives (partial).
 */
static unsigned
xkb_get_map(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct xkb_buffer buffer;
	unsigned first[8];
	unsigned count[8];
	unsigned full;
	unsigned partial;
	unsigned part;
	unsigned bit;
	uint16_t vmods;
	int msb;

	/* The parts asked for whole and in part. */
	msb = client->order;
	if (length != 28U)
		return X11_BAD_LENGTH;
	full = x11_read16(request + 6, msb) & XKB_MAP_ALL;
	partial = x11_read16(request + 8, msb) & XKB_MAP_ALL & ~full;

	/* Each part's range: the key types, then the keys of each per-key part (keysyms, actions, behaviours, explicit, modifiers, virtual modifiers). */
	for (part = 0U; part < 8U; part++) {
		first[part] = XKB_KEYCODE_MIN;
		count[part] = XKB_KEYS;
	}
	first[0] = 0U;
	count[0] = XKB_TYPES;
	vmods = XKB_VMODS_MASK;
	if ((partial & XKB_MAP_KEY_TYPES) != 0U) {
		first[0] = request[10];
		count[0] = request[11];
		if (first[0] + count[0] > XKB_TYPES)
			return X11_BAD_VALUE;
	}
	for (part = 1U; part < 8U; part++) {
		bit = 1U << part;
		if (part == 6U)
			continue;
		if ((partial & bit) == 0U)
			continue;

		/* The range's place in the request (the parts' pairs, VirtualMods taking the place of one). */
		if (part == 1U) {
			first[part] = request[12];
			count[part] = request[13];
		} else if (part == 4U) {
			first[part] = request[14];
			count[part] = request[15];
		} else if (part == 5U) {
			first[part] = request[16];
			count[part] = request[17];
		} else if (part == 3U) {
			first[part] = request[20];
			count[part] = request[21];
		} else if (part == 2U) {
			first[part] = request[22];
			count[part] = request[23];
		} else {
			first[part] = request[24];
			count[part] = request[25];
		}

		/* A range must be inside the keycodes. */
		if (count[part] != 0U && first[part] < XKB_KEYCODE_MIN)
			return X11_BAD_VALUE;
		if (first[part] + count[part] > XKB_KEYCODE_MAX + 1U)
			return X11_BAD_VALUE;
	}
	if ((partial & XKB_MAP_VIRTUAL_MODS) != 0U)
		vmods = (uint16_t)(x11_read16(request + 18, msb) & XKB_VMODS_MASK);

	/* The reply. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.msb = msb;
	xkb_write_map(&buffer, server, full | partial, first, count, vmods);
	if (buffer.failed) {
		free(buffer.bytes);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: sent. */
	xkb_finish(&buffer, 0U, client);
	xkb_send(server, client, &buffer);
	return 0U;
}

/* GetCompatMap: the symbol interpretations (all, or a range of them) and the groups' modifiers asked for. */
static unsigned
xkb_get_compat_map(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct xkb_buffer buffer;
	unsigned groups;
	unsigned first;
	unsigned count;
	int msb;

	/* The groups, and all the interpretations or a range of them. */
	msb = client->order;
	if (length != 12U)
		return X11_BAD_LENGTH;
	groups = request[6] & 0x0fU;
	first = 0U;
	count = XKB_INTERPRETS;
	if (request[7] == 0U) {
		first = x11_read16(request + 8, msb);
		count = x11_read16(request + 10, msb);
		if (first + count > XKB_INTERPRETS)
			return X11_BAD_VALUE;
	}

	/* The reply. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.msb = msb;
	xkb_write_compat(&buffer, server, groups, first, count);
	if (buffer.failed) {
		free(buffer.bytes);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: sent. */
	xkb_finish(&buffer, 0U, client);
	xkb_send(server, client, &buffer);
	return 0U;
}

/* GetIndicatorState: the indicators lit. */
static unsigned
xkb_get_indicator_state(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	struct x11_xkb_state state;
	uint8_t reply[32];
	uint32_t lit;
	uint16_t core;

	/* The header alone. */
	if (length != 8U)
		return X11_BAD_LENGTH;

	/* The state, and the indicators it lights. */
	core = x11_input_state(server);
	x11_xkb_state_of(server, core, &state);
	lit = xkb_indicator_state(server, &state);

	/* Succeeded: the reply. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XKB_DEVICE_ID;
	x11_write32(reply + 8, lit, client->order);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetIndicatorMap: how each indicator asked for follows the state. */
static unsigned
xkb_get_indicator_map(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct xkb_buffer buffer;
	uint32_t which;

	/* The indicators. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	which = x11_read32(request + 8, client->order);

	/* The reply. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.msb = client->order;
	xkb_write_indicator_map(&buffer, server, which);
	if (buffer.failed) {
		free(buffer.bytes);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: sent. */
	xkb_finish(&buffer, 0U, client);
	xkb_send(server, client, &buffer);
	return 0U;
}

/* GetNamedIndicator: the indicator of a name, whether it is lit, and how it follows the state. */
static unsigned
xkb_get_named_indicator(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct x11_xkb_state state;
	uint8_t reply[32];
	uint32_t atom;
	uint32_t name;
	uint32_t lit;
	uint16_t core;
	size_t name_length;
	unsigned index;
	int msb;

	/* The indicator's name. */
	msb = client->order;
	if (length != 16U)
		return X11_BAD_LENGTH;
	atom = x11_read32(request + 12, msb);
	if (atom == 0U)
		return X11_BAD_ATOM;

	/* The reply: supported, the name, and not found unless an indicator has it. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XKB_DEVICE_ID;
	x11_write32(reply + 8, atom, msb);
	reply[28] = 1U;

	/* The indicator of the name, if any. */
	core = x11_input_state(server);
	x11_xkb_state_of(server, core, &state);
	lit = xkb_indicator_state(server, &state);
	for (index = 0U; index < XKB_INDICATORS; index++) {
		name_length = strlen(xkb_indicators[index].name);
		name = x11_atom_intern(server, xkb_indicators[index].name, name_length, 1);
		if (name != atom)
			continue;

		/* Found: whether it is lit, physical, its index and its map. */
		reply[12] = 1U;
		reply[13] = (uint8_t)((lit >> index) & 1U);
		reply[14] = 1U;
		reply[15] = (uint8_t)index;
		reply[19] = (uint8_t)XKB_IM_USE_LOCKED;
		reply[20] = xkb_indicator_mods(server, index);
		reply[21] = xkb_indicators[index].real_mods;
		if (xkb_indicators[index].vmod != XKB_NO_VMOD)
			x11_write16(reply + 22, (uint16_t)(1U << xkb_indicators[index].vmod), msb);
		break;
	}

	/* Succeeded: the reply. */
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* GetNames: the names asked for. */
static unsigned
xkb_get_names(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct xkb_buffer buffer;
	uint32_t which;

	/* The names. */
	if (length != 12U)
		return X11_BAD_LENGTH;
	which = x11_read32(request + 8, client->order) & XKB_NAMES_ALL;

	/* The reply. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.msb = client->order;
	xkb_write_names(&buffer, server, which);
	if (buffer.failed) {
		free(buffer.bytes);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: sent. */
	xkb_finish(&buffer, 0U, client);
	xkb_send(server, client, &buffer);
	return 0U;
}

/* GetGeometry: the keyboard has no geometry, so none of any name is found. */
static unsigned
xkb_get_geometry(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];

	/* The name. */
	if (length != 12U)
		return X11_BAD_LENGTH;

	/* Succeeded: the name, not found. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XKB_DEVICE_ID;
	memcpy(reply + 8, request + 8, 4U);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * PerClientFlags: the client's flags change under a mask.  The server
 * keeps the flags it supports; DetectableAutoRepeat it cannot keep, since
 * the backends report a held key's repeat as a release and a press.
 */
static unsigned
xkb_per_client_flags(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	uint8_t reply[32];
	uint32_t change;
	uint32_t value;
	int msb;

	/* The flags changed, and their values. */
	msb = client->order;
	if (length != 28U)
		return X11_BAD_LENGTH;
	change = x11_read32(request + 8, msb) & XKB_PCF_SUPPORTED;
	value = x11_read32(request + 12, msb);
	client->xkb_flags = (client->xkb_flags & ~change) | (value & change);

	/* Succeeded: those supported, and the client's. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XKB_DEVICE_ID;
	x11_write32(reply + 8, XKB_PCF_SUPPORTED, msb);
	x11_write32(reply + 12, client->xkb_flags, msb);
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/* ListComponents: the server keeps no database of keyboard components, so none are listed. */
static unsigned
xkb_list_components(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[32];

	/* The fixed part, and the patterns after it. */
	if (length < 12U)
		return X11_BAD_LENGTH;

	/* Succeeded: none. */
	memset(reply, 0, sizeof(reply));
	reply[1] = (uint8_t)XKB_DEVICE_ID;
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * GetKbdByName: with no names, the keyboard as it is (its map, its
 * compatibility map, its indicators and its names, each as the request
 * for it answers); with names, nothing, since the server keeps no
 * database of components to load them from.
 */
static unsigned
xkb_get_kbd_by_name(
	struct x11server *server,
	struct x11_client *client,
	const uint8_t *request,
	size_t length)
{
	struct xkb_buffer buffer;
	unsigned first[8];
	unsigned count[8];
	unsigned reported;
	unsigned part;
	size_t offset;
	size_t start;
	unsigned name;
	int named;
	int msb;

	/* The six names after the fixed part, each counted. */
	msb = client->order;
	if (length < 12U)
		return X11_BAD_LENGTH;
	offset = 12U;
	named = 0;
	for (name = 0U; name < 6U; name++) {
		if (offset >= length)
			return X11_BAD_LENGTH;
		if (request[offset] != 0U)
			named = 1;
		offset += 1U + request[offset];
	}
	if (offset > length)
		return X11_BAD_LENGTH;

	/* The components asked for that the server reports. */
	reported = (x11_read16(request + 6, msb) | x11_read16(request + 8, msb)) & XKB_GBN_REPORTED;
	if (named)
		reported = 0U;

	/* The reply's own part: the keycodes, and what is found and reported. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.msb = msb;
	xkb_put(&buffer, NULL, 8U);
	xkb_put8(&buffer, XKB_KEYCODE_MIN);
	xkb_put8(&buffer, XKB_KEYCODE_MAX);
	xkb_put8(&buffer, 0U);
	xkb_put8(&buffer, 0U);
	xkb_put16(&buffer, reported);
	xkb_put16(&buffer, reported);
	xkb_put(&buffer, NULL, 16U);

	/* The map, whole, as GetMap answers. */
	if ((reported & XKB_GBN_MAP) != 0U) {
		for (part = 0U; part < 8U; part++) {
			first[part] = XKB_KEYCODE_MIN;
			count[part] = XKB_KEYS;
		}
		first[0] = 0U;
		count[0] = XKB_TYPES;
		start = buffer.used;
		xkb_write_map(&buffer, server, XKB_MAP_ALL, first, count, XKB_VMODS_MASK);
		xkb_finish(&buffer, start, client);
	}

	/* The compatibility map, whole, as GetCompatMap answers. */
	if ((reported & XKB_GBN_COMPAT) != 0U) {
		start = buffer.used;
		xkb_write_compat(&buffer, server, 0x0fU, 0U, XKB_INTERPRETS);
		xkb_finish(&buffer, start, client);
	}

	/* The indicators' maps, as GetIndicatorMap answers. */
	if ((reported & XKB_GBN_INDICATORS) != 0U) {
		start = buffer.used;
		xkb_write_indicator_map(&buffer, server, XKB_INDICATORS_MASK);
		xkb_finish(&buffer, start, client);
	}

	/* The names, as GetNames answers. */
	if ((reported & XKB_GBN_NAMES) != 0U) {
		start = buffer.used;
		xkb_write_names(&buffer, server, XKB_NAMES_ALL);
		xkb_finish(&buffer, start, client);
	}

	/* A reply that could not be written. */
	if (buffer.failed) {
		free(buffer.bytes);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: sent. */
	xkb_finish(&buffer, 0U, client);
	xkb_send(server, client, &buffer);
	return 0U;
}

/* GetDeviceInfo: the keyboard's id, name and type; it has no buttons or indicator feedbacks of its own to report. */
static unsigned
xkb_get_device_info(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	struct xkb_buffer buffer;

	/* The fixed part. */
	if (length != 16U)
		return X11_BAD_LENGTH;

	/* The keyboard: nothing more than its own state, its type, its name. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.msb = client->order;
	xkb_put(&buffer, NULL, 8U);
	xkb_put(&buffer, NULL, 13U);
	xkb_put8(&buffer, 1U);
	xkb_put(&buffer, NULL, 6U);
	xkb_write_atom(&buffer, server, XKB_DEVICE_TYPE);
	xkb_put16(&buffer, (unsigned)(sizeof(XKB_DEVICE_NAME) - 1U));
	xkb_put(&buffer, XKB_DEVICE_NAME, sizeof(XKB_DEVICE_NAME) - 1U);
	xkb_pad(&buffer);
	if (buffer.failed) {
		free(buffer.bytes);
		return X11_BAD_ALLOC;
	}

	/* Succeeded: sent. */
	xkb_finish(&buffer, 0U, client);
	xkb_send(server, client, &buffer);
	return 0U;
}

/* SetDebuggingFlags: the server has no debugging flags, and says so. */
static unsigned
xkb_set_debugging_flags(
	struct x11server *server,
	struct x11_client *client,
	size_t length)
{
	uint8_t reply[32];

	/* The fixed part, and the message after it. */
	if (length < 24U)
		return X11_BAD_LENGTH;

	/* Succeeded: no flags set or supported. */
	memset(reply, 0, sizeof(reply));
	x11_reply(server, client, reply, sizeof(reply));
	return 0U;
}

/*
 * Writes a GetMap reply (its header to be finished): the parts named,
 * each over its range (first and count by part, the key types' first),
 * and the virtual modifiers named.
 */
static void
xkb_write_map(
	struct xkb_buffer *buffer,
	const struct x11server *server,
	unsigned parts,
	const unsigned *first,
	const unsigned *count,
	uint16_t vmods)
{
	struct xkb_key key;
	size_t header;
	unsigned keycode;
	unsigned group;
	unsigned level;
	unsigned vmod;
	unsigned total_syms;
	unsigned total_actions;
	unsigned total_modmap;
	unsigned total_vmodmap;

	/* The header, filled in at the end; its fixed part. */
	header = buffer->used;
	xkb_put(buffer, NULL, 40U);

	/* The key types. */
	if ((parts & XKB_MAP_KEY_TYPES) != 0U)
		xkb_write_types(buffer, server, first[0], count[0]);

	/* The keysyms: each key's types, groups, width and keysyms. */
	total_syms = 0U;
	if ((parts & XKB_MAP_KEY_SYMS) != 0U) {
		for (keycode = first[1]; keycode < first[1] + count[1]; keycode++) {
			xkb_key_describe(server, keycode, &key);
			xkb_put8(buffer, key.types[0]);
			xkb_put8(buffer, key.types[1]);
			xkb_put8(buffer, 0U);
			xkb_put8(buffer, 0U);
			xkb_put8(buffer, key.groups);
			xkb_put8(buffer, key.width);
			xkb_put16(buffer, key.groups * key.width);
			for (group = 0U; group < key.groups; group++) {
				for (level = 0U; level < key.width; level++)
					xkb_put32(buffer, key.syms[group][level]);
			}
			total_syms += key.groups * key.width;
		}
	}

	/* The actions: each key's count (padded), then the actions. */
	total_actions = 0U;
	if ((parts & XKB_MAP_KEY_ACTIONS) != 0U) {
		for (keycode = first[4]; keycode < first[4] + count[4]; keycode++) {
			xkb_key_describe(server, keycode, &key);
			if (!key.acted) {
				xkb_put8(buffer, 0U);
				continue;
			}
			xkb_put8(buffer, key.groups * key.width);
			total_actions += key.groups * key.width;
		}
		xkb_pad(buffer);
		for (keycode = first[4]; keycode < first[4] + count[4]; keycode++) {
			xkb_key_describe(server, keycode, &key);
			if (!key.acted)
				continue;
			for (group = 0U; group < key.groups; group++) {
				for (level = 0U; level < key.width; level++)
					xkb_put(buffer, key.actions[group][level], XKB_ACTION_SIZE);
			}
		}
	}

	/* The virtual modifiers' real modifiers (padded). */
	if ((parts & XKB_MAP_VIRTUAL_MODS) != 0U) {
		for (vmod = 0U; vmod < XKB_VMODS; vmod++) {
			if ((vmods & (1U << vmod)) != 0U)
				xkb_put8(buffer, xkb_vmod_real(server, vmod));
		}
		xkb_pad(buffer);
	}

	/* The modifier map: each key in range that is a modifier's (padded). */
	total_modmap = 0U;
	if ((parts & XKB_MAP_MODIFIER_MAP) != 0U) {
		for (keycode = first[2]; keycode < first[2] + count[2]; keycode++) {
			xkb_key_describe(server, keycode, &key);
			if (key.modmap == 0U)
				continue;
			xkb_put8(buffer, keycode);
			xkb_put8(buffer, key.modmap);
			total_modmap++;
		}
		xkb_pad(buffer);
	}

	/* The virtual modifier map: each key in range that binds a virtual modifier. */
	total_vmodmap = 0U;
	if ((parts & XKB_MAP_VIRTUAL_MOD_MAP) != 0U) {
		for (keycode = first[7]; keycode < first[7] + count[7]; keycode++) {
			xkb_key_describe(server, keycode, &key);
			if (key.vmodmap == 0U)
				continue;
			xkb_put8(buffer, keycode);
			xkb_put8(buffer, 0U);
			xkb_put16(buffer, key.vmodmap);
			total_vmodmap++;
		}
	}

	/* A buffer that failed has no header to fill. */
	if (buffer->failed)
		return;

	/* The keycodes, the parts present, and the key types' range. */
	buffer->bytes[header + 10U] = (uint8_t)XKB_KEYCODE_MIN;
	buffer->bytes[header + 11U] = (uint8_t)XKB_KEYCODE_MAX;
	x11_write16(buffer->bytes + header + 12U, (uint16_t)parts, buffer->msb);
	if ((parts & XKB_MAP_KEY_TYPES) != 0U) {
		buffer->bytes[header + 14U] = (uint8_t)first[0];
		buffer->bytes[header + 15U] = (uint8_t)count[0];
	}
	buffer->bytes[header + 16U] = (uint8_t)XKB_TYPES;

	/* The keysyms' and the actions' ranges and totals. */
	if ((parts & XKB_MAP_KEY_SYMS) != 0U) {
		buffer->bytes[header + 17U] = (uint8_t)first[1];
		x11_write16(buffer->bytes + header + 18U, (uint16_t)total_syms, buffer->msb);
		buffer->bytes[header + 20U] = (uint8_t)count[1];
	}
	if ((parts & XKB_MAP_KEY_ACTIONS) != 0U) {
		buffer->bytes[header + 21U] = (uint8_t)first[4];
		x11_write16(buffer->bytes + header + 22U, (uint16_t)total_actions, buffer->msb);
		buffer->bytes[header + 24U] = (uint8_t)count[4];
	}

	/* The behaviours' and the explicit components' ranges (the keys have none of either). */
	if ((parts & XKB_MAP_KEY_BEHAVIORS) != 0U) {
		buffer->bytes[header + 25U] = (uint8_t)first[5];
		buffer->bytes[header + 26U] = (uint8_t)count[5];
	}
	if ((parts & XKB_MAP_EXPLICIT) != 0U) {
		buffer->bytes[header + 28U] = (uint8_t)first[3];
		buffer->bytes[header + 29U] = (uint8_t)count[3];
	}

	/* The modifier maps' ranges and totals, and the virtual modifiers. */
	if ((parts & XKB_MAP_MODIFIER_MAP) != 0U) {
		buffer->bytes[header + 31U] = (uint8_t)first[2];
		buffer->bytes[header + 32U] = (uint8_t)count[2];
		buffer->bytes[header + 33U] = (uint8_t)total_modmap;
	}
	if ((parts & XKB_MAP_VIRTUAL_MOD_MAP) != 0U) {
		buffer->bytes[header + 34U] = (uint8_t)first[7];
		buffer->bytes[header + 35U] = (uint8_t)count[7];
		buffer->bytes[header + 36U] = (uint8_t)total_vmodmap;
	}
	if ((parts & XKB_MAP_VIRTUAL_MODS) != 0U)
		x11_write16(buffer->bytes + header + 38U, vmods, buffer->msb);
}

/* Writes a range of the key types: each one's modifiers, levels and map entries. */
static void
xkb_write_types(
	struct xkb_buffer *buffer,
	const struct x11server *server,
	unsigned first,
	unsigned count)
{
	const struct xkb_type *type;
	unsigned index;
	unsigned entry;
	uint8_t resolved;
	uint8_t active;

	/* Each key type of the range. */
	for (index = first; index < first + count; index++) {
		type = &xkb_types[index];

		/* Its modifiers (the real ones its virtual ones stand for included), levels and entries. */
		resolved = xkb_vmods_real(server, type->vmods);
		xkb_put8(buffer, (unsigned)(type->real_mods | resolved));
		xkb_put8(buffer, type->real_mods);
		xkb_put16(buffer, type->vmods);
		xkb_put8(buffer, type->levels);
		xkb_put8(buffer, type->entry_count);
		xkb_put8(buffer, 0U);
		xkb_put8(buffer, 0U);

		/* Each entry, active unless its virtual modifiers stand for no real one. */
		for (entry = 0U; entry < type->entry_count; entry++) {
			resolved = xkb_vmods_real(server, type->entry_vmods[entry]);
			active = 1U;
			if (type->entry_vmods[entry] != 0U && resolved == 0U)
				active = 0U;
			xkb_put8(buffer, active);
			xkb_put8(buffer, (unsigned)(type->entry_real[entry] | resolved));
			xkb_put8(buffer, type->entry_level[entry]);
			xkb_put8(buffer, type->entry_real[entry]);
			xkb_put16(buffer, type->entry_vmods[entry]);
			xkb_put16(buffer, 0U);
		}
	}
}

/*
 * Writes a GetCompatMap reply (its header to be finished): a range of the
 * symbol interpretations, then the modifiers of each group asked for
 * (the second to fourth: AltGr, the virtual modifier of Mode_switch).
 */
static void
xkb_write_compat(
	struct xkb_buffer *buffer,
	const struct x11server *server,
	unsigned groups,
	unsigned first,
	unsigned count)
{
	const struct xkb_interpret *interpret;
	unsigned index;
	unsigned group;
	uint8_t alt_gr;

	/* The header: the groups, the range and the total. */
	xkb_put(buffer, NULL, 8U);
	xkb_put8(buffer, groups);
	xkb_put8(buffer, 0U);
	xkb_put16(buffer, first);
	xkb_put16(buffer, count);
	xkb_put16(buffer, (unsigned)XKB_INTERPRETS);
	xkb_put(buffer, NULL, 16U);

	/* Each interpretation of the range: its keysym, its match, its virtual modifier, its flags and its action. */
	for (index = first; index < first + count; index++) {
		interpret = &xkb_interprets[index];
		xkb_put32(buffer, interpret->keysym);
		xkb_put8(buffer, 0xffU);
		xkb_put8(buffer, interpret->match);
		xkb_put8(buffer, interpret->vmod);
		xkb_put8(buffer, interpret->flags);
		xkb_write_action(buffer, interpret->action, 0U, 0U, 1);
	}

	/* Succeeded: each group's modifiers asked for. */
	alt_gr = xkb_vmod_real(server, XKB_VMOD_ALT_GR);
	for (group = 0U; group < 4U; group++) {
		if ((groups & (1U << group)) == 0U)
			continue;
		if (group == 0U) {
			xkb_put32(buffer, 0U);
			continue;
		}
		xkb_put8(buffer, alt_gr);
		xkb_put8(buffer, 0U);
		xkb_put16(buffer, 1U << XKB_VMOD_ALT_GR);
	}
}

/*
 * Writes one action: setting or locking a key's modifiers (by its
 * modifier map), or moving the group by one; anything else is no action.
 */
static void
xkb_write_action(
	struct xkb_buffer *buffer,
	uint8_t type,
	uint8_t flags,
	uint8_t mods,
	int8_t group)
{
	uint8_t action[XKB_ACTION_SIZE];

	/* The type. */
	memset(action, 0, sizeof(action));
	action[0] = type;

	/* Its fields: the modifiers by the key's map, or the group's step. */
	if (type == XKB_SA_SET_MODS) {
		action[1] = (uint8_t)(XKB_SA_USE_MOD_MAP_MODS | XKB_SA_CLEAR_LOCKS | flags);
		action[2] = mods;
		action[3] = mods;
	} else if (type == XKB_SA_LOCK_MODS) {
		action[1] = (uint8_t)(XKB_SA_USE_MOD_MAP_MODS | flags);
		action[2] = mods;
		action[3] = mods;
	} else if (type == XKB_SA_SET_GROUP) {
		action[1] = flags;
		action[2] = (uint8_t)group;
	}

	/* Succeeded: written. */
	xkb_put(buffer, action, sizeof(action));
}

/* Writes a GetIndicatorMap reply (its header to be finished): the maps of the indicators named. */
static void
xkb_write_indicator_map(
	struct xkb_buffer *buffer,
	const struct x11server *server,
	uint32_t which)
{
	unsigned index;

	/* The header: the indicators named, the physical ones, how many there are. */
	xkb_put(buffer, NULL, 8U);
	xkb_put32(buffer, which);
	xkb_put32(buffer, XKB_INDICATORS_MASK);
	xkb_put8(buffer, XKB_INDICATORS_ALL);
	xkb_put(buffer, NULL, 15U);

	/* Succeeded: each indicator named, those with no map empty. */
	for (index = 0U; index < XKB_INDICATORS_ALL; index++) {
		if ((which & (1U << index)) == 0U)
			continue;
		if (index >= XKB_INDICATORS) {
			xkb_put(buffer, NULL, 12U);
			continue;
		}
		xkb_put8(buffer, 0U);
		xkb_put8(buffer, 0U);
		xkb_put8(buffer, 0U);
		xkb_put8(buffer, XKB_IM_USE_LOCKED);
		xkb_put8(buffer, xkb_indicator_mods(server, index));
		xkb_put8(buffer, xkb_indicators[index].real_mods);
		if (xkb_indicators[index].vmod == XKB_NO_VMOD) {
			xkb_put16(buffer, 0U);
		} else {
			xkb_put16(buffer, 1U << xkb_indicators[index].vmod);
		}
		xkb_put32(buffer, 0U);
	}
}

/*
 * Writes a GetNames reply (its header to be finished): the names named,
 * in the order the reply lays down.  The keys are named by their
 * keycodes ("K" and three digits).
 */
static void
xkb_write_names(
	struct xkb_buffer *buffer,
	struct x11server *server,
	uint32_t which)
{
	char key_name[4];
	unsigned groups;
	unsigned index;
	unsigned level;
	unsigned keycode;

	/* The header: what is named, the keycodes, the key types, the groups, the virtual modifiers, the keys and the indicators. */
	groups = (1U << xkb_groups_used(server)) - 1U;
	xkb_put(buffer, NULL, 8U);
	xkb_put32(buffer, which);
	xkb_put8(buffer, XKB_KEYCODE_MIN);
	xkb_put8(buffer, XKB_KEYCODE_MAX);
	xkb_put8(buffer, XKB_TYPES);
	xkb_put8(buffer, groups);
	xkb_put16(buffer, XKB_VMODS_MASK);
	xkb_put8(buffer, XKB_KEYCODE_MIN);
	xkb_put8(buffer, XKB_KEYS);
	xkb_put32(buffer, XKB_INDICATORS_MASK);
	xkb_put8(buffer, 0U);
	xkb_put8(buffer, 0U);
	xkb_put16(buffer, XKB_TYPE_LEVELS);
	xkb_put32(buffer, 0U);

	/* The components' names, each the same. */
	if ((which & XKB_NAMES_KEYCODES) != 0U)
		xkb_write_atom(buffer, server, XKB_COMPONENT_NAME);
	if ((which & XKB_NAMES_GEOMETRY) != 0U)
		xkb_write_atom(buffer, server, XKB_COMPONENT_NAME);
	if ((which & XKB_NAMES_SYMBOLS) != 0U)
		xkb_write_atom(buffer, server, XKB_COMPONENT_NAME);
	if ((which & XKB_NAMES_PHYS_SYMBOLS) != 0U)
		xkb_write_atom(buffer, server, XKB_COMPONENT_NAME);
	if ((which & XKB_NAMES_TYPES) != 0U)
		xkb_write_atom(buffer, server, XKB_COMPONENT_NAME);
	if ((which & XKB_NAMES_COMPAT) != 0U)
		xkb_write_atom(buffer, server, XKB_COMPONENT_NAME);

	/* The key types' names. */
	if ((which & XKB_NAMES_TYPE_NAMES) != 0U) {
		for (index = 0U; index < XKB_TYPES; index++)
			xkb_write_atom(buffer, server, xkb_types[index].name);
	}

	/* The key types' levels (padded), then their levels' names. */
	if ((which & XKB_NAMES_LEVEL_NAMES) != 0U) {
		for (index = 0U; index < XKB_TYPES; index++)
			xkb_put8(buffer, xkb_types[index].levels);
		xkb_pad(buffer);
		for (index = 0U; index < XKB_TYPES; index++) {
			for (level = 0U; level < xkb_types[index].levels; level++)
				xkb_write_atom(buffer, server, xkb_types[index].level_names[level]);
		}
	}

	/* The indicators' names. */
	if ((which & XKB_NAMES_INDICATORS) != 0U) {
		for (index = 0U; index < XKB_INDICATORS; index++)
			xkb_write_atom(buffer, server, xkb_indicators[index].name);
	}

	/* The virtual modifiers' names. */
	if ((which & XKB_NAMES_VIRTUAL_MODS) != 0U) {
		for (index = 0U; index < XKB_VMODS; index++)
			xkb_write_atom(buffer, server, xkb_vmod_names[index]);
	}

	/* The groups' names. */
	if ((which & XKB_NAMES_GROUPS) != 0U) {
		xkb_write_atom(buffer, server, "Group 1");
		if ((groups & 2U) != 0U)
			xkb_write_atom(buffer, server, "Group 2");
	}

	/* Succeeded: the keys' names (no aliases or radio groups follow). */
	if ((which & XKB_NAMES_KEYS) != 0U) {
		for (keycode = XKB_KEYCODE_MIN; keycode <= XKB_KEYCODE_MAX; keycode++) {
			key_name[0] = 'K';
			key_name[1] = (char)('0' + keycode / 100U);
			key_name[2] = (char)('0' + keycode / 10U % 10U);
			key_name[3] = (char)('0' + keycode % 10U);
			xkb_put(buffer, key_name, sizeof(key_name));
		}
	}
}

/* Writes the atom of a name (None when it cannot be made). */
static void
xkb_write_atom(
	struct xkb_buffer *buffer,
	struct x11server *server,
	const char *name)
{
	uint32_t atom;
	size_t length;

	/* Succeeded: interned, and written. */
	length = strlen(name);
	atom = x11_atom_intern(server, name, length, 0);
	xkb_put32(buffer, atom);
}

/*
 * Finishes a reply written from start: its type, the keyboard's id, the
 * client's sequence number and its length in words after 32 bytes.
 */
static void
xkb_finish(
	struct xkb_buffer *buffer,
	size_t start,
	const struct x11_client *client)
{
	size_t length;

	/* A buffer that failed, or a reply shorter than a header. */
	if (buffer->failed)
		return;
	length = buffer->used - start;
	if (length < 32U)
		return;

	/* Succeeded: the header. */
	buffer->bytes[start] = 1U;
	buffer->bytes[start + 1U] = (uint8_t)XKB_DEVICE_ID;
	x11_write16(buffer->bytes + start + 2U, client->sequence, client->order);
	x11_write32(buffer->bytes + start + 4U, (uint32_t)((length - 32U) / 4U), client->order);
}

/* Sends a finished reply, and frees its buffer. */
static void
xkb_send(
	struct x11server *server,
	struct x11_client *client,
	struct xkb_buffer *buffer)
{
	/* Succeeded: queued, and freed. */
	x11_client_send(server, client, buffer->bytes, buffer->used);
	free(buffer->bytes);
	buffer->bytes = NULL;
}

/* Appends bytes (zeros when bytes is NULL) to a reply, growing it. */
static void
xkb_put(
	struct xkb_buffer *buffer,
	const void *bytes,
	size_t length)
{
	uint8_t *grown;
	size_t capacity;

	/* A buffer that failed takes nothing more. */
	if (buffer->failed)
		return;

	/* More room, doubling. */
	if (buffer->used + length > buffer->capacity) {
		capacity = buffer->capacity * 2U;
		if (capacity < 256U)
			capacity = 256U;
		while (capacity < buffer->used + length)
			capacity *= 2U;
		grown = realloc(buffer->bytes, capacity);
		if (grown == NULL) {
			buffer->failed = 1;
			return;
		}
		buffer->bytes = grown;
		buffer->capacity = capacity;
	}

	/* Succeeded: the bytes, or zeros. */
	if (bytes == NULL) {
		memset(buffer->bytes + buffer->used, 0, length);
	} else {
		memcpy(buffer->bytes + buffer->used, bytes, length);
	}
	buffer->used += length;
}

/* Appends one byte. */
static void
xkb_put8(
	struct xkb_buffer *buffer,
	unsigned value)
{
	uint8_t byte;

	/* Succeeded: appended. */
	byte = (uint8_t)value;
	xkb_put(buffer, &byte, 1U);
}

/* Appends a 16-bit value in the client's byte order. */
static void
xkb_put16(
	struct xkb_buffer *buffer,
	unsigned value)
{
	uint8_t bytes[2];

	/* Succeeded: appended. */
	x11_write16(bytes, (uint16_t)value, buffer->msb);
	xkb_put(buffer, bytes, sizeof(bytes));
}

/* Appends a 32-bit value in the client's byte order. */
static void
xkb_put32(
	struct xkb_buffer *buffer,
	uint32_t value)
{
	uint8_t bytes[4];

	/* Succeeded: appended. */
	x11_write32(bytes, value, buffer->msb);
	xkb_put(buffer, bytes, sizeof(bytes));
}

/* Appends zeros to the next whole word. */
static void
xkb_pad(
	struct xkb_buffer *buffer)
{
	size_t pad;

	/* Succeeded: padded. */
	pad = (4U - (buffer->used & 3U)) & 3U;
	xkb_put(buffer, NULL, pad);
}

/*
 * Describes a key from the core map: its two groups (the core map's first
 * two keysyms and the other two), each one's key type and levels, the
 * width (the most levels of its groups), each level's action, and its
 * modifier maps.
 */
static void
xkb_key_describe(
	const struct x11server *server,
	unsigned keycode,
	struct xkb_key *key)
{
	const uint32_t *core;
	unsigned group;
	unsigned level;
	unsigned levels;
	int first_level;

	/* Nothing, until the core map says. */
	memset(key, 0, sizeof(*key));
	if (keycode < XKB_KEYCODE_MIN || keycode > XKB_KEYCODE_MAX)
		return;
	core = server->keysyms[keycode];
	key->modmap = xkb_modmap(server, keycode);
	key->vmodmap = (uint16_t)xkb_vmod_of(core[0]);

	/* The groups: the second when its keysyms are there, the first when any is. */
	if (core[2] != 0U || core[3] != 0U) {
		key->groups = 2U;
	} else if (core[0] != 0U || core[1] != 0U) {
		key->groups = 1U;
	}

	/* Each group's key type and keysyms, and the width. */
	for (group = 0U; group < key->groups; group++) {
		key->types[group] = xkb_group_describe(core[group * 2U], core[group * 2U + 1U], key->syms[group]);
		levels = xkb_types[key->types[group]].levels;
		if (levels > key->width)
			key->width = levels;
	}

	/* Succeeded: each level's action, the key acting when any level has one. */
	for (group = 0U; group < key->groups; group++) {
		for (level = 0U; level < key->width; level++) {
			first_level = 0;
			if (group == 0U && level == 0U)
				first_level = 1;
			xkb_level_action(key->syms[group][level], key->modmap, first_level, key->actions[group][level]);
			if (key->actions[group][level][0] != XKB_SA_NO_ACTION)
				key->acted = 1;
		}
	}
}

/*
 * Chooses a group's key type from its two core keysyms, as the core
 * protocol reads them: a letter alone stands for its lower and upper
 * cases (ALPHABETIC, as is a lower case letter with its upper case), a
 * keypad keysym second makes KEYPAD, one keysym makes ONE_LEVEL and two
 * TWO_LEVEL.  Writes the group's keysyms and returns the key type.
 */
static unsigned
xkb_group_describe(
	uint32_t first,
	uint32_t second,
	uint32_t *syms)
{
	uint32_t lower;
	uint32_t upper;

	/* The keysyms as they are. */
	syms[0] = first;
	syms[1] = second;
	xkb_case(first, &lower, &upper);

	/* One keysym: a letter's two cases, or one level. */
	if (second == 0U) {
		if (lower == upper)
			return XKB_TYPE_ONE_LEVEL;
		syms[0] = lower;
		syms[1] = upper;
		return XKB_TYPE_ALPHABETIC;
	}

	/* A lower case letter and its upper case. */
	if (lower != upper && first == lower && second == upper)
		return XKB_TYPE_ALPHABETIC;

	/* A keypad keysym second. */
	if (second >= KEYSYM_KEYPAD_FIRST && second <= KEYSYM_KEYPAD_LAST)
		return XKB_TYPE_KEYPAD;

	/* Succeeded: two levels. */
	return XKB_TYPE_TWO_LEVEL;
}

/*
 * Makes a level's action from the first symbol interpretation that
 * matches its keysym and its key's modifier map (an interpretation for
 * the first level only sees no modifiers on the others).
 */
static void
xkb_level_action(
	uint32_t keysym,
	uint8_t modmap,
	int first_level,
	uint8_t *action)
{
	const struct xkb_interpret *interpret;
	struct xkb_buffer buffer;
	unsigned index;
	uint8_t seen;
	uint8_t match;

	/* No action, until an interpretation matches. */
	memset(action, 0, XKB_ACTION_SIZE);
	if (keysym == 0U)
		return;

	/* The first interpretation of the keysym, or of any keysym, whose match holds. */
	for (index = 0U; index < XKB_INTERPRETS; index++) {
		interpret = &xkb_interprets[index];
		if (interpret->keysym != 0U && interpret->keysym != keysym)
			continue;

		/* The modifiers the match sees, and the match. */
		seen = modmap;
		if ((interpret->match & XKB_SI_LEVEL_ONE_ONLY) != 0U && !first_level)
			seen = 0U;
		match = (uint8_t)(interpret->match & ~XKB_SI_LEVEL_ONE_ONLY);
		if (match == XKB_SI_ANY_OF && seen == 0U)
			continue;

		/* A modifier action of a key in no modifier does nothing. */
		if (interpret->action != XKB_SA_SET_GROUP && seen == 0U)
			return;

		/* Succeeded: the action, by the modifiers seen. */
		memset(&buffer, 0, sizeof(buffer));
		buffer.bytes = action;
		buffer.capacity = XKB_ACTION_SIZE;
		xkb_write_action(&buffer, interpret->action, 0U, seen, 1);
		return;
	}
}

/*
 * Finds a keysym's lower and upper case (the same keysym twice when it has
 * no case): Latin-1, and the Latin, Greek and Cyrillic letters of the
 * Unicode keysyms.
 */
static void
xkb_case(
	uint32_t keysym,
	uint32_t *lower,
	uint32_t *upper)
{
	uint32_t code;
	uint32_t base;

	/* No case, until a range says. */
	*lower = keysym;
	*upper = keysym;

	/* Latin-1: A to Z, and the accented letters but the signs of multiplication and division. */
	if (keysym >= 'A' && keysym <= 'Z') {
		*lower = keysym + 0x20U;
		return;
	}
	if (keysym >= 'a' && keysym <= 'z') {
		*upper = keysym - 0x20U;
		return;
	}
	if (keysym >= 0xc0U && keysym <= 0xdeU && keysym != 0xd7U) {
		*lower = keysym + 0x20U;
		return;
	}
	if (keysym >= 0xe0U && keysym <= 0xfeU && keysym != 0xf7U) {
		*upper = keysym - 0x20U;
		return;
	}

	/* Not a Unicode keysym. */
	if (keysym < KEYSYM_UNICODE || keysym > KEYSYM_UNICODE + 0x10ffffU)
		return;
	code = keysym - KEYSYM_UNICODE;
	base = KEYSYM_UNICODE;

	/* Latin Extended-A's pairs (even upper, odd lower in the two runs that keep that order). */
	if ((code >= 0x100U && code <= 0x137U) || (code >= 0x14aU && code <= 0x177U)) {
		*upper = base + (code & ~1U);
		*lower = base + (code | 1U);
		return;
	}

	/* Greek capitals and smalls. */
	if (code >= 0x391U && code <= 0x3a9U && code != 0x3a2U) {
		*lower = base + code + 0x20U;
		return;
	}
	if (code >= 0x3b1U && code <= 0x3c9U && code != 0x3c2U) {
		*upper = base + code - 0x20U;
		return;
	}

	/* Cyrillic capitals and smalls. */
	if (code >= 0x400U && code <= 0x40fU) {
		*lower = base + code + 0x50U;
		return;
	}
	if (code >= 0x410U && code <= 0x42fU) {
		*lower = base + code + 0x20U;
		return;
	}
	if (code >= 0x430U && code <= 0x44fU) {
		*upper = base + code - 0x20U;
		return;
	}
	if (code >= 0x450U && code <= 0x45fU)
		*upper = base + code - 0x50U;
}

/* Returns the modifiers a keycode is a key of (its modifier map). */
static uint8_t
xkb_modmap(
	const struct x11server *server,
	unsigned keycode)
{
	unsigned modifier;
	unsigned slot;
	uint8_t mods;

	/* Each modifier that has the key. */
	mods = 0U;
	for (modifier = 0U; modifier < X11_MODIFIERS; modifier++) {
		for (slot = 0U; slot < server->keys_per_modifier; slot++) {
			if (server->modifier_keys[modifier][slot] == keycode)
				mods = (uint8_t)(mods | (1U << modifier));
		}
	}

	/* Succeeded: the modifiers. */
	return mods;
}

/* Returns the virtual modifiers (their bits) a keysym binds its key to. */
static unsigned
xkb_vmod_of(
	uint32_t keysym)
{
	unsigned index;

	/* The interpretation of the keysym, when it binds one. */
	for (index = 0U; index < XKB_INTERPRETS; index++) {
		if (xkb_interprets[index].keysym != keysym || keysym == 0U)
			continue;
		if (xkb_interprets[index].vmod == XKB_NO_VMOD)
			return 0U;
		return 1U << xkb_interprets[index].vmod;
	}

	/* None. */
	return 0U;
}

/* Returns the real modifiers a virtual modifier stands for: those of the keys that bind it. */
static uint8_t
xkb_vmod_real(
	const struct x11server *server,
	unsigned vmod)
{
	unsigned keycode;
	unsigned bound;
	uint8_t mods;

	/* Each key that binds it gives its modifier map. */
	mods = 0U;
	for (keycode = XKB_KEYCODE_MIN; keycode <= XKB_KEYCODE_MAX; keycode++) {
		bound = xkb_vmod_of(server->keysyms[keycode][0]);
		if ((bound & (1U << vmod)) == 0U)
			continue;
		mods = (uint8_t)(mods | xkb_modmap(server, keycode));
	}

	/* Succeeded: the modifiers. */
	return mods;
}

/* Returns the real modifiers a set of virtual modifiers stands for. */
static uint8_t
xkb_vmods_real(
	const struct x11server *server,
	uint16_t vmods)
{
	unsigned vmod;
	uint8_t mods;

	/* Each virtual modifier of the set. */
	mods = 0U;
	for (vmod = 0U; vmod < XKB_VMODS; vmod++) {
		if ((vmods & (1U << vmod)) != 0U)
			mods = (uint8_t)(mods | xkb_vmod_real(server, vmod));
	}

	/* Succeeded: the modifiers. */
	return mods;
}

/* Returns the modifiers whose keys are Mode_switch keys (the core state's group). */
static uint8_t
xkb_mode_switch_mods(
	const struct x11server *server)
{
	unsigned modifier;
	unsigned slot;
	unsigned keycode;
	uint8_t mods;

	/* Each modifier with a key whose first keysym is Mode_switch. */
	mods = 0U;
	for (modifier = 0U; modifier < X11_MODIFIERS; modifier++) {
		for (slot = 0U; slot < server->keys_per_modifier; slot++) {
			keycode = server->modifier_keys[modifier][slot];
			if (keycode == 0U)
				continue;
			if (server->keysyms[keycode][0] == KEYSYM_MODE_SWITCH)
				mods = (uint8_t)(mods | (1U << modifier));
		}
	}

	/* Succeeded: the modifiers. */
	return mods;
}

/* Returns how many groups the map has (2 when any key has the second, else 1). */
static unsigned
xkb_groups_used(
	const struct x11server *server)
{
	unsigned keycode;

	/* Any key with a keysym in the second group. */
	for (keycode = XKB_KEYCODE_MIN; keycode <= XKB_KEYCODE_MAX; keycode++) {
		if (server->keysyms[keycode][2] != 0U || server->keysyms[keycode][3] != 0U)
			return 2U;
	}

	/* One. */
	return 1U;
}

/* Returns the indicators a state lights: each whose modifiers are locked. */
static uint32_t
xkb_indicator_state(
	const struct x11server *server,
	const struct x11_xkb_state *state)
{
	unsigned index;
	uint32_t lit;
	uint8_t mods;

	/* Each indicator with modifiers, all of which (any one) locked. */
	lit = 0U;
	for (index = 0U; index < XKB_INDICATORS; index++) {
		mods = xkb_indicator_mods(server, index);
		if (mods != 0U && (state->locked & mods) != 0U)
			lit |= 1U << index;
	}

	/* Succeeded: the indicators. */
	return lit;
}

/* Returns the real modifiers an indicator follows. */
static uint8_t
xkb_indicator_mods(
	const struct x11server *server,
	unsigned index)
{
	uint8_t mods;

	/* Its real modifiers, and those its virtual modifier stands for. */
	mods = xkb_indicators[index].real_mods;
	if (xkb_indicators[index].vmod != XKB_NO_VMOD)
		mods = (uint8_t)(mods | xkb_vmod_real(server, xkb_indicators[index].vmod));

	/* Succeeded: the modifiers. */
	return mods;
}

/* Tells whether a device spec names the keyboard (the core keyboard, or its id). */
static int
xkb_keyboard(
	uint16_t spec)
{
	/* The core keyboard, by its spec or its id. */
	if (spec == XKB_USE_CORE_KEYBOARD)
		return 1;
	if (spec == XKB_DEVICE_ID)
		return 1;

	/* Another device. */
	return 0;
}

/*
 * Compares the keyboard's state and indicators with those last told:
 * StateNotify (with the key or button, the core event and the request
 * that changed it) and IndicatorStateNotify to the clients that asked
 * for the parts that changed.
 */
static void
xkb_state_check(
	struct x11server *server,
	uint8_t keycode,
	uint8_t type,
	uint8_t major,
	uint8_t minor)
{
	struct x11_xkb_state state;
	struct x11_client *client;
	uint8_t event[32];
	uint32_t lit;
	uint32_t lit_changed;
	unsigned changed;
	unsigned index;
	uint16_t core;
	int selected;
	int msb;

	/* The state now, and the parts that changed. */
	core = x11_input_state(server);
	x11_xkb_state_of(server, core, &state);
	changed = 0U;
	if (state.mods != server->xkb_last.mods)
		changed |= XKB_STATE_MODS | XKB_STATE_GRAB_MODS | XKB_STATE_LOOKUP_MODS;
	if (state.base != server->xkb_last.base)
		changed |= XKB_STATE_BASE_MODS;
	if (state.locked != server->xkb_last.locked)
		changed |= XKB_STATE_LOCKED_MODS;
	if (state.group != server->xkb_last.group)
		changed |= XKB_STATE_GROUP | XKB_STATE_BASE_GROUP;
	if (state.compat != server->xkb_last.compat)
		changed |= XKB_STATE_COMPAT | XKB_STATE_COMPAT_GRAB | XKB_STATE_COMPAT_LOOKUP;
	if (state.buttons != server->xkb_last.buttons)
		changed |= XKB_STATE_BUTTONS;
	server->xkb_last = state;
	lit = xkb_indicator_state(server, &state);
	lit_changed = lit ^ server->xkb_indicators;
	server->xkb_indicators = lit;

	/* Each client that asked for the parts that changed. */
	for (index = 0U; index < server->clients_max; index++) {
		client = x11_client_of(server, index);
		if (client == NULL)
			continue;
		msb = client->order;

		/* StateNotify: the state each way, what changed, and what changed it. */
		selected = xkb_selected(client, XKB_EVENT_STATE_NOTIFY, changed);
		if (selected) {
			xkb_event_header(event, server, client, XKB_EVENT_STATE_NOTIFY);
			event[9] = state.mods;
			event[10] = state.base;
			event[12] = state.locked;
			event[13] = state.group;
			x11_write16(event + 14, state.group, msb);
			event[19] = state.compat;
			event[20] = state.mods;
			event[21] = state.compat;
			event[22] = state.mods;
			event[23] = state.compat;
			x11_write16(event + 24, state.buttons, msb);
			x11_write16(event + 26, (uint16_t)changed, msb);
			event[28] = keycode;
			event[29] = type;
			event[30] = major;
			event[31] = minor;
			x11_client_send(server, client, event, sizeof(event));
		}

		/* IndicatorStateNotify: the indicators lit, and those that changed. */
		selected = xkb_selected(client, XKB_EVENT_INDICATOR_STATE, lit_changed);
		if (selected) {
			xkb_event_header(event, server, client, XKB_EVENT_INDICATOR_STATE);
			x11_write32(event + 12, lit, msb);
			x11_write32(event + 16, lit_changed, msb);
			x11_client_send(server, client, event, sizeof(event));
		}
	}
}

/* Starts an XKEYBOARD event for a client: its code, its kind, the sequence number, the time and the keyboard. */
static void
xkb_event_header(
	uint8_t *event,
	struct x11server *server,
	const struct x11_client *client,
	unsigned kind)
{
	/* Succeeded: the header, the rest zero. */
	memset(event, 0, 32U);
	event[0] = (uint8_t)X11_XKB_EVENT;
	event[1] = (uint8_t)kind;
	x11_write16(event + 2, client->sequence, client->order);
	x11_write32(event + 4, server->now, client->order);
	event[8] = (uint8_t)XKB_DEVICE_ID;
}

/* Tells whether a client uses XKEYBOARD and selected a kind of event with any of these details. */
static int
xkb_selected(
	const struct x11_client *client,
	unsigned kind,
	uint32_t details)
{
	/* A client without XKB, or one set up only in part. */
	if (!client->xkb_used || !client->setup)
		return 0;

	/* The kind not selected, or none of the details. */
	if ((client->xkb_events & (1U << kind)) == 0U)
		return 0;
	if ((client->xkb_details[kind] & details) == 0U)
		return 0;

	/* Succeeded: selected. */
	return 1;
}
