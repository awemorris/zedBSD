/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Derived from the Linux kernel v6.8.12 (drivers/gpu/drm/i915/display/intel_opregion.c),
 * which carries the following notice.
 *
 * Copyright 2008 Intel Corporation <hong.liu@intel.com>
 * Copyright 2008 Red Hat <mjg@redhat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT.  IN NO EVENT SHALL INTEL AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * The OpRegion service and the ACPI notifier chain (see opregion.h).
 *
 * The OpRegion lifecycle is the Linux v6.8.12 intel_opregion.c text,
 * rewritten function by function; its Linux notice is in
 * intel/opregion.h.  The ACPI display ids are this driver's
 * own code: the id layout is the ACPI _DOD device id, and its values and the
 * connector classification agree with Linux intel_acpi.c.  Around them the instance provides the
 * mailbox mapping table memremap() resolves into, the PCI view (ASLS), the
 * backlight targets, the worker queue, and the recorded boundaries.
 *
 *   SHADOW    driver-owned RAM in the OpRegion format (tests); the
 *             firmware's region is never mapped writable here.
 *   FIRMWARE  the real shared region, mapped read / write; production does
 *             not enable it and runs VBT_ONLY.
 *
 * The ACPI notifier chain follows the Linux contract (drivers/acpi/event.c
 * and kernel/notifier.c, v6.8.12, checked against the public sources; no
 * text is copied): registration in priority order, higher first and equal
 * priority after the existing ones, the same block twice refused; removal of
 * a block not on the chain refused; the call chain walks the callbacks in
 * order until one returns NOTIFY_STOP_MASK, and reports EINVAL for
 * NOTIFY_BAD.  One kernel mutex serializes everything (Linux uses an rwsem,
 * where dispatches may overlap), so after an unregistration returns the
 * callback is not running and is not called again; a callback must not
 * register or unregister on the chain.
 */

#include "opregion-internal.h"
#include "opregion.h"
#include <kern/kcrt.h>

#include <hal/hal.h>
#include <kern/clock.h>
#include <kern/kmem.h>
#include <kern/sched.h>

/* The mailbox layouts, offsets and request bits of the Linux text. */
#include "../intel/opregion.h"

/*
 * The ACPI display device id (the _DOD id scheme of the ACPI specification):
 * an index within the display type in bits 3:0 and the display type in bits
 * 11:8.  The values agree with those Linux intel_acpi.c uses.
 */
#define I915_ACPI_DISPLAY_INDEX_SHIFT		0
#define I915_ACPI_DISPLAY_TYPE_SHIFT		8
#define I915_ACPI_DISPLAY_TYPE_MASK		(0xfU << 8)
#define I915_ACPI_DISPLAY_TYPE_OTHER		(0U << 8)
#define I915_ACPI_DISPLAY_TYPE_VGA		(1U << 8)
#define I915_ACPI_DISPLAY_TYPE_TV		(2U << 8)
#define I915_ACPI_DISPLAY_TYPE_EXTERNAL_DIGITAL	(3U << 8)
#define I915_ACPI_DISPLAY_TYPE_INTERNAL_DIGITAL	(4U << 8)

/* How many display types the type field can name. */
#define I915_ACPI_DISPLAY_TYPES			16U

/* How long the ASLE work cancellation waits for a running callback, in scheduler ticks. */
#define I915_OPREGION_CANCEL_TICKS (5u * KERN_CLOCK_HZ)

/* How many regions the memremap() table holds, and how many HAL mappings the FIRMWARE backend makes. */
#define I915_OPREGION_MAPS 4u
#define I915_OPREGION_HAL_MAPS 2u

/* How many backlight targets the service registers. */
#define I915_OPREGION_BACKLIGHT_TARGETS 4u

/* The display types a sanitized encoder is reported as (intel_opregion_notify_encoder()). */
#define I915_OPREGION_DISPLAY_TYPE_CRT			0
#define I915_OPREGION_DISPLAY_TYPE_TV			1
#define I915_OPREGION_DISPLAY_TYPE_EXTERNAL_FLAT_PANEL	2
#define I915_OPREGION_DISPLAY_TYPE_INTERNAL_FLAT_PANEL	3

/*
 * The display whose OpRegion world exists.
 *
 * The environment header declares the hooks the Linux text reaches through
 * its macros (memremap(), the SWSCI configuration access, the recorded
 * boundaries, the backlight policy and the backlight hand-over), the ACPI
 * notifier registration and the encoder notification without a display or
 * a world; they reach the world through this binding.
 * drv_i915_opregion_world_create() sets it and
 * drv_i915_opregion_world_destroy() clears it, both on the probe thread; the
 * driver drives one display.  NULL means no world exists.
 *
 * XXX: this is the one file-scope state left; it goes when those hooks take
 * the world as an argument.
 */
static struct i915_display *i915_opregion_bound_display;

static struct i915_opregion_world *i915_opregion_bound_world(void);
static int i915_register_acpi_notifier(struct notifier_block *nb);
static int i915_unregister_acpi_notifier(struct notifier_block *nb);
static void i915_opregion_unmap_hal(struct i915_opregion_world *world);
static u32 i915_asle_set_als_illum(struct drm_i915_private *dev_priv, u32 alsi);
static u32 i915_asle_set_backlight(struct drm_i915_private *dev_priv, u32 bclp);
static u32 i915_asle_set_pwm_freq(struct drm_i915_private *dev_priv, u32 pfmb);
static u32 i915_asle_set_pfit(struct drm_i915_private *dev_priv, u32 pfit);
static u32 i915_asle_set_supported_rotation_angles(struct drm_i915_private *dev_priv, u32 srot);
static u32 i915_asle_set_button_array(struct drm_i915_private *dev_priv, u32 iuer);
static u32 i915_asle_set_convertible(struct drm_i915_private *dev_priv, u32 iuer);
static u32 i915_asle_set_docking(struct drm_i915_private *dev_priv, u32 iuer);
static u32 i915_asle_isct_state(struct drm_i915_private *dev_priv);
static void i915_asle_work(struct work_struct *work);
static void i915_opregion_asle_intr(struct drm_i915_private *dev_priv);
static int i915_opregion_video_event(struct notifier_block *nb, unsigned long val, void *data);
static int i915_check_swsci_function(struct drm_i915_private *i915, u32 function);
static int i915_swsci(struct drm_i915_private *dev_priv, u32 function, u32 parm, u32 *parm_out);
static int i915_opregion_notify_adapter(struct drm_i915_private *dev_priv, pci_power_t state);
static void i915_set_did(struct intel_opregion *opregion, int i, u32 val);
static void i915_didl_outputs(struct drm_i915_private *dev_priv);
static void i915_setup_cadls(struct drm_i915_private *dev_priv);
static int i915_no_opregion_vbt_callback(const struct dmi_system_id *id);
static int i915_load_vbt_firmware(struct drm_i915_private *dev_priv);
static void i915_opregion_setup_mailboxes(struct drm_i915_private *dev_priv, void *base, u32 mboxes);
static void i915_opregion_find_vbt(struct drm_i915_private *dev_priv, u32 asls, void *base, u32 mboxes);
static void i915_opregion_register(struct drm_i915_private *i915);
static void i915_opregion_resume_display(struct drm_i915_private *i915);
static void i915_opregion_resume(struct drm_i915_private *i915);
static void i915_opregion_suspend_display(struct drm_i915_private *i915);
static void i915_opregion_suspend(struct drm_i915_private *i915, pci_power_t state);
static void i915_opregion_unregister(struct drm_i915_private *i915);
static void i915_opregion_cleanup(struct drm_i915_private *i915);
static u32 i915_acpi_display_type(struct intel_connector *connector);

/*
 * Creates the OpRegion environment's world of a display.
 *
 * The world starts zeroed, with no backend and the vendor backlight policy,
 * which is what the old file-scope state was at boot.  Returns 0, EBUSY
 * when a world already exists, or ENOMEM.
 */
int
drv_i915_opregion_world_create(
	struct i915_display *display)
{
	struct i915_opregion_world *world;

	/* The service works for one display; a second world would steal the binding. */
	if (i915_opregion_bound_display != NULL)
		return EBUSY;

	/* Allocates the world zeroed. */
	world = kern_calloc(1, sizeof(*world));
	if (world == NULL)
		return ENOMEM;

	/* Sets the two fields that start nonzero. */
	world->i915_opregion_backend = "NONE";
	world->i915_opregion_policy = acpi_backlight_vendor;

	/* Hands the world to the display and to the hooks that are given no world. */
	display->opregion_world = world;
	i915_opregion_bound_display = display;

	/* Succeeded: the service has its world. */
	return 0;
}

/*
 * Destroys the OpRegion environment's world of a display.
 *
 * The service must have been unregistered and cleaned up; a display without
 * a world is left alone.
 */
void
drv_i915_opregion_world_destroy(
	struct i915_display *display)
{
	struct i915_opregion_world *world;

	/* Nothing to destroy without a world. */
	world = display->opregion_world;
	if (world == NULL)
		return;

	/* Withdraws the binding the hooks use. */
	if (i915_opregion_bound_display == display)
		i915_opregion_bound_display = NULL;

	/* Frees the world and forgets it. */
	display->opregion_world = NULL;
	kern_free(world);
}

/*
 * Prepares the ACPI notifier chain of a display, once.
 */
void
drv_i915_acpi_notifier_init(
	struct i915_display *display)
{
	/* The chain lock is initialized on first use. */
	if (!display->chain_lock_live) {
		(void)mutex_init(&display->chain_lock, LOCK_RANK_DEVICE, "i915-acpi-notifier");
		display->chain_lock_live = 1;
	}
}

/*
 * Puts a notifier block on the ACPI notifier chain, in priority order.
 *
 * A block of higher priority goes before, one of equal priority after the
 * blocks already there.  Returns 0, EINVAL for a block without a callback,
 * or EEXIST when the block is already registered.
 */
int
drv_i915_register_acpi_notifier(
	struct i915_display *display,
	struct notifier_block *nb)
{
	struct notifier_block **link;
	int error;

	/* Refuses a block without a callback. */
	if (nb == NULL)
		return EINVAL;
	if (nb->notifier_call == NULL)
		return EINVAL;

	/* Finds the place of the block, refusing it when it is already there. */
	drv_i915_acpi_notifier_init(display);
	error = 0;
	mutex_lock(&display->chain_lock);

	for (link = &display->chain_head;
	     *link != NULL;
	     link = &(*link)->next) {
		if (*link == nb) {
			kern_logf("i915: WARN acpi notifier callback already registered\n");
			error = EEXIST;
			break;
		}

		/* A block of higher priority goes before this one. */
		if (nb->priority > (*link)->priority)
			break;
	}

	/* Links the block in. */
	if (error == 0) {
		nb->next = *link;
		*link = nb;
	}

	mutex_unlock(&display->chain_lock);

	/* Reports a block that was already registered. */
	if (error != 0)
		return error;

	/* Succeeded: the block is on the chain. */
	return 0;
}

/*
 * Takes a notifier block off the ACPI notifier chain.
 *
 * Taking the chain lock waits for a running call chain, so afterwards the
 * callback neither runs nor is called again.  Returns 0, or ENOENT when the
 * block is not on the chain.
 */
int
drv_i915_unregister_acpi_notifier(
	struct i915_display *display,
	struct notifier_block *nb)
{
	struct notifier_block **link;
	int error;

	/* Looks for the block and unlinks it. */
	drv_i915_acpi_notifier_init(display);
	error = ENOENT;
	mutex_lock(&display->chain_lock);

	for (link = &display->chain_head;
	     *link != NULL;
	     link = &(*link)->next) {
		if (*link == nb) {
			*link = nb->next;
			nb->next = NULL;
			error = 0;
			break;
		}
	}

	mutex_unlock(&display->chain_lock);

	/* Reports a block that was not on the chain. */
	if (error != 0)
		return error;

	/* Succeeded: the block is off the chain. */
	return 0;
}

/*
 * Delivers one ACPI bus event to the notifier chain
 * (acpi_notifier_call_chain()).
 *
 * The callbacks run in order until one returns a result with
 * NOTIFY_STOP_MASK; the chain's result is the last callback's (NOTIFY_DONE
 * when none ran).  out, when given, receives the event source (SYNTHETIC
 * unless named), the chain's result, the dispatch's result and how many
 * callbacks ran.  Returns 0, or EINVAL when the chain's result is
 * NOTIFY_BAD.
 *
 * XXX: out->dispatch_result was the Linux -EINVAL; it is the positive
 * EINVAL this function returns now.
 */
int
drv_i915_acpi_notifier_call_chain(
	struct i915_display *display,
	const char *device_class,
	const char *bus_id,
	uint32_t type,
	uint32_t data,
	const char *event_source,
	struct i915_acpi_dispatch *out)
{
	struct acpi_bus_event event;
	struct notifier_block *nb;
	int result;
	int dispatch;
	unsigned calls;

	/* Builds the event. */
	kern_memset(&event, 0, sizeof(event));
	if (device_class != NULL)
		kern_strncpy(event.device_class, device_class, sizeof(event.device_class) - 1u);
	if (bus_id != NULL)
		kern_strncpy(event.bus_id, bus_id, sizeof(event.bus_id) - 1u);
	event.type = type;
	event.data = data;

	/* Runs the callbacks in order until one stops the walk. */
	drv_i915_acpi_notifier_init(display);
	result = NOTIFY_DONE;
	calls = 0u;
	mutex_lock(&display->chain_lock);

	for (nb = display->chain_head;
	     nb != NULL;
	     nb = nb->next) {
		result = nb->notifier_call(nb, 0ul, &event);
		calls++;
		if ((result & NOTIFY_STOP_MASK) != 0)
			break;
	}

	mutex_unlock(&display->chain_lock);

	/* A NOTIFY_BAD chain fails the dispatch. */
	dispatch = 0;
	if (result == NOTIFY_BAD)
		dispatch = EINVAL;

	/* Records what the dispatch did. */
	if (out != NULL) {
		if (event_source != NULL) {
			out->event_source = event_source;
		} else {
			out->event_source = "SYNTHETIC";
		}

		out->callback_result = result;
		out->dispatch_result = dispatch;
		out->calls = calls;
	}

	/* Reports a NOTIFY_BAD chain. */
	if (dispatch != 0)
		return dispatch;

	/* Succeeded: the event was delivered. */
	return 0;
}

/*
 * Counts the notifier blocks on the ACPI notifier chain.
 */
unsigned
drv_i915_acpi_notifier_count(
	struct i915_display *display)
{
	struct notifier_block *nb;
	unsigned count;

	/* Walks the chain under its lock. */
	drv_i915_acpi_notifier_init(display);
	count = 0u;
	mutex_lock(&display->chain_lock);

	for (nb = display->chain_head;
	     nb != NULL;
	     nb = nb->next)
		count++;

	mutex_unlock(&display->chain_lock);

	/* Succeeded: reports the number of blocks. */
	return count;
}

/*
 * Returns the backend the instance is bound to: "NONE", "SHADOW" or
 * "FIRMWARE".
 */
const char *
drv_i915_opregion_mailbox_backend(
	struct i915_display *display)
{
	/* Succeeded: reports the backend name. */
	return display->opregion_world->i915_opregion_backend;
}

/*
 * Returns how many setups have been attempted.
 */
unsigned
drv_i915_opregion_service_epoch(
	struct i915_display *display)
{
	/* Succeeded: reports the setup count. */
	return display->opregion_world->i915_opregion_epoch;
}

/*
 * Adds a region to the table memremap() resolves into.
 *
 * Returns 0, or ENOSPC when the table is full.
 */
int
drv_i915_opregion_shadow_map(
	struct i915_display *display,
	uint64_t phys,
	void *ptr,
	uint32_t size)
{
	struct i915_opregion_world *world;
	struct i915_opregion_map *map;

	/* Refuses a region the table has no room for. */
	world = display->opregion_world;
	if (world->i915_opregion_nmaps >= I915_OPREGION_MAPS)
		return ENOSPC;

	/* Records the region. */
	map = &world->i915_opregion_maps[world->i915_opregion_nmaps];
	map->phys = phys;
	map->ptr = ptr;
	map->size = size;
	world->i915_opregion_nmaps++;

	/* Succeeded: memremap() resolves the region. */
	return 0;
}

/*
 * Runs intel_opregion_setup() on the SHADOW backend, whose ASLS reads
 * asls_token (the address the shadow is mapped at).
 *
 * Returns 0, EBUSY while the instance is still registered, or the setup's
 * error (I915_OPREGION_ENOTSUPP for a zero token).
 */
int
drv_i915_opregion_shadow_setup(
	struct i915_display *display,
	uint32_t asls_token)
{
	struct i915_opregion_world *world;
	int error;

	/* Refuses to set up an instance that is still registered. */
	world = display->opregion_world;
	if (world->i915_opregion_dev.display.opregion.acpi_notifier.notifier_call != NULL)
		return EBUSY;

	/* Starts a new instance on the shadow. */
	kern_memset(&world->i915_opregion_dev.display.opregion, 0, sizeof(world->i915_opregion_dev.display.opregion));
	world->i915_opregion_dev.display.params.vbt_firmware = NULL;
	world->i915_opregion_asls = asls_token;
	world->i915_opregion_backend = "SHADOW";
	world->i915_opregion_epoch++;

	/* Runs the Linux setup; a failed one leaves no backend. */
	error = drv_i915_opregion_setup(&world->i915_opregion_dev);
	if (error != 0) {
		world->i915_opregion_backend = "NONE";
		return error;
	}

	/* Succeeded: the instance is set up on the shadow. */
	return 0;
}

/*
 * Runs intel_opregion_setup() on the FIRMWARE backend: the real OpRegion at
 * asls, mapped read / write (uncached device view), and the RVDA VBT it
 * names, mapped read-only.
 *
 * From here the driver writes the shared mailboxes.  Returns 0, ENODEV for
 * no OpRegion, EBUSY while registered or still mapped, ENOMEM when the
 * OpRegion cannot be mapped, or the setup's error (the mappings are undone
 * then).
 */
int
drv_i915_opregion_firmware_setup(
	struct i915_display *display,
	uint32_t asls)
{
	struct i915_opregion_world *world;
	void *op;
	void *vbt;
	uint64_t rvda;
	uint64_t phys;
	uint32_t rvds;
	uint32_t major;
	uint32_t minor;
	int map_error;
	int error;

	/* No ASLS is no OpRegion. */
	world = display->opregion_world;
	if (asls == 0u)
		return ENODEV;

	/* Refuses an instance that is still registered or mapped. */
	if (world->i915_opregion_dev.display.opregion.acpi_notifier.notifier_call != NULL)
		return EBUSY;
	if (world->i915_opregion_nhal != 0u)
		return EBUSY;

	/* Maps the OpRegion read / write and enters it in the table. */
	world->i915_opregion_nmaps = 0u;
	op = NULL;
	map_error = hal_space_map_device((hal_physaddr_t)asls, OPREGION_SIZE, HAL_SPACE_READ | HAL_SPACE_WRITE, &op);
	if (map_error != HAL_OK)
		return ENOMEM;
	if (op == NULL)
		return ENOMEM;
	world->i915_opregion_hal[world->i915_opregion_nhal].va = op;
	world->i915_opregion_hal[world->i915_opregion_nhal].size = OPREGION_SIZE;
	world->i915_opregion_nhal++;
	(void)drv_i915_opregion_shadow_map(display, asls, op, OPREGION_SIZE);

	/* Reads the version and the RVDA the Linux setup will memremap. */
	major = ((const volatile u8 *)op)[0x17];
	minor = ((const volatile u8 *)op)[0x16];
	rvda = 0u;
	rvds = 0u;
	kern_memcpy(&rvda, (const u8 *)op + OPREGION_ASLE_OFFSET + 186, 8);
	kern_memcpy(&rvds, (const u8 *)op + OPREGION_ASLE_OFFSET + 194, 4);

	/* Maps the RVDA VBT read-only at the address the Linux setup computes. */
	if (major >= 2u &&
	    rvda != 0u &&
	    rvds != 0u) {
		/* From version 2.1 the RVDA is an offset from the OpRegion. */
		if (major > 2u || minor >= 1u) {
			phys = (uint64_t)asls + rvda;
		} else {
			phys = rvda;
		}

		/* Enters the VBT mapping in the table when it succeeds. */
		vbt = NULL;
		map_error = hal_space_map_device((hal_physaddr_t)phys, rvds, HAL_SPACE_READ, &vbt);
		if (map_error == HAL_OK && vbt != NULL) {
			world->i915_opregion_hal[world->i915_opregion_nhal].va = vbt;
			world->i915_opregion_hal[world->i915_opregion_nhal].size = rvds;
			world->i915_opregion_nhal++;
			(void)drv_i915_opregion_shadow_map(display, phys, vbt, rvds);
		}
	}

	/* Starts a new instance on the firmware's region. */
	kern_memset(&world->i915_opregion_dev.display.opregion, 0, sizeof(world->i915_opregion_dev.display.opregion));
	world->i915_opregion_dev.display.params.vbt_firmware = NULL;
	world->i915_opregion_asls = asls;
	world->i915_opregion_backend = "FIRMWARE";
	world->i915_opregion_epoch++;

	/* Runs the Linux setup; a failed one leaves no backend and no mapping. */
	error = drv_i915_opregion_setup(&world->i915_opregion_dev);
	if (error != 0) {
		world->i915_opregion_backend = "NONE";
		i915_opregion_unmap_hal(world);
		return error;
	}

	/* Succeeded: the instance is set up on the firmware's region. */
	return 0;
}

/*
 * Registers the instance (intel_opregion_register()) and opens the GSE gate
 * once an ASLE mailbox exists.
 */
void
drv_i915_opregion_register(
	struct i915_display *display)
{
	struct i915_opregion_world *world;

	/* Runs the Linux registration. */
	world = display->opregion_world;
	i915_opregion_register(&world->i915_opregion_dev);

	/* Requests are accepted once the Linux text published ARDY READY, which needs an ASLE mailbox. */
	world->i915_opregion_accepting = 0;
	if (world->i915_opregion_dev.display.opregion.asle != NULL)
		world->i915_opregion_accepting = 1;
}

/*
 * Unregisters the instance: closes the GSE gate first, then runs
 * intel_opregion_unregister(), which waits for the ASLE work.
 */
void
drv_i915_opregion_unregister(
	struct i915_display *display)
{
	struct i915_opregion_world *world;

	/* Stops the producer before the worker is synchronized. */
	world = display->opregion_world;
	world->i915_opregion_accepting = 0;

	/* Runs the Linux unregistration. */
	i915_opregion_unregister(&world->i915_opregion_dev);
}

/*
 * Cleans the instance up (intel_opregion_cleanup()) once the ASLE work is
 * shown idle, and undoes the FIRMWARE backend's mappings.
 *
 * Returns 0, or EBUSY while still registered or while the ASLE work is not
 * shown idle (the mapping and the instance stay, and the refusal is
 * counted).
 */
int
drv_i915_opregion_cleanup(
	struct i915_display *display)
{
	struct i915_opregion_world *world;
	struct work_struct *work;
	int busy;
	int pending;

	/* A registered instance must be unregistered first. */
	world = display->opregion_world;
	work = &world->i915_opregion_dev.display.opregion.asle_work;
	if (world->i915_opregion_accepting)
		return EBUSY;

	/* The ASLE work of a set-up instance with a queue must be neither pending nor running. */
	busy = 0;
	if (world->i915_opregion_dev.unordered_wq != NULL &&
	    world->i915_opregion_dev.display.opregion.header != NULL) {
		pending = drv_i915_work_pending(world->i915_opregion_dev.unordered_wq, &work->kwork);
		if (pending != 0) {
			busy = 1;
		} else if (work->kwork.state == I915_WORK_RUNNING) {
			busy = 1;
		}
	}
	if (busy) {
		world->i915_opregion_cleanup_refused++;
		kern_logf("i915: opregion cleanup REFUSED: the ASLE work is not shown idle -- mapping kept\n");
		return EBUSY;
	}

	/* Runs the Linux cleanup and undoes the FIRMWARE mappings. */
	i915_opregion_cleanup(&world->i915_opregion_dev);
	i915_opregion_unmap_hal(world);

	/* Leaves the instance without a backend or a table. */
	world->i915_opregion_backend = "NONE";
	world->i915_opregion_nmaps = 0u;

	/* Succeeded: the instance is cleaned up. */
	return 0;
}

/*
 * Tells the firmware the adapter's PCI power state
 * (intel_opregion_notify_adapter()).
 *
 * Returns 0, or the SWSCI refusal (ENODEV without the SWSCI mailbox).
 */
int
drv_i915_opregion_notify_adapter(
	struct i915_display *display,
	int pci_state)
{
	int error;

	/* Runs the Linux notification. */
	error = i915_opregion_notify_adapter(&display->opregion_world->i915_opregion_dev, pci_state);
	if (error != 0)
		return error;

	/* Succeeded: the firmware was told. */
	return 0;
}

/*
 * Tells the firmware that a sanitized encoder was enabled or disabled
 * (intel_opregion_notify_encoder()).
 *
 * The caller hands the port (enum port) and the output type (enum
 * intel_output_type) because the encoder objects belong to the modeset
 * environment; the port mapping, the parameter layout and the SWSCI
 * request are the Linux text's.  Port E is reported as port 0 and every
 * other port as port + 1 with BIT(port) set; a disable adds 4 << 8; the
 * display type goes to bits 16 + port * 3.  Returns 0, ENODEV without the
 * SWSCI mailbox (or without a world), EINVAL for a port or type out of
 * range, or the SWSCI refusal.
 */
int
drv_i915_opregion_notify_encoder(
	int port_in,
	int output_type,
	int enable)
{
	struct i915_opregion_world *world;
	u32 parm;
	u32 type;
	u32 port;
	int error;

	/* Without a world there is no SWSCI mailbox. */
	world = i915_opregion_bound_world();
	if (world == NULL)
		return ENODEV;

	/* HAS_DDI() holds on this platform; Linux returns 0 on the older ones. */
	error = i915_check_swsci_function(&world->i915_opregion_dev, SWSCI_SBCB_DISPLAY_POWER_STATE);
	if (error != 0)
		return error;

	/* Port E is port 0; every other port is its index plus one, with its bit set. */
	parm = 0u;
	port = (u32)port_in;
	if (port == 4u) {
		port = 0u;
	} else {
		parm |= 1u << port;
		port++;
	}
	if (port > 4u) {
		kern_logf("i915: opregion notify_encoder: port index %u out of bounds\n", (unsigned)port);
		return EINVAL;
	}

	/* A disable is flagged. */
	if (!enable)
		parm |= 4u << 8;

	/* Classifies the output. */
	switch (output_type) {
	case I915_OUTPUT_ANALOG:
		type = I915_OPREGION_DISPLAY_TYPE_CRT;
		break;
	case I915_OUTPUT_DDI:
	case I915_OUTPUT_DP:
	case I915_OUTPUT_HDMI:
	case I915_OUTPUT_DP_MST:
		type = I915_OPREGION_DISPLAY_TYPE_EXTERNAL_FLAT_PANEL;
		break;
	case I915_OUTPUT_EDP:
	case I915_OUTPUT_DSI:
		type = I915_OPREGION_DISPLAY_TYPE_INTERNAL_FLAT_PANEL;
		break;
	default:
		kern_logf("i915: opregion notify_encoder: unsupported encoder type %d\n", output_type);
		return EINVAL;
	}

	/* Places the display type of the port and asks the firmware. */
	parm |= type << (16u + port * 3u);
	error = i915_swsci(&world->i915_opregion_dev, SWSCI_SBCB_DISPLAY_POWER_STATE, parm, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the firmware was told. */
	return 0;
}

/*
 * Reads a 32-bit mailbox word of the set-up instance (either backend), or
 * 0 when there is none or the offset is out of the OpRegion.
 */
uint32_t
drv_i915_opregion_mbox_read(
	struct i915_display *display,
	unsigned off)
{
	struct i915_opregion_world *world;
	u32 value;

	/* Reads the word when the instance is set up and the offset fits. */
	world = display->opregion_world;
	value = 0u;
	if (world->i915_opregion_dev.display.opregion.header != NULL && off + 4u <= OPREGION_SIZE)
		kern_memcpy(&value, (const u8 *)world->i915_opregion_dev.display.opregion.header + off, 4);

	/* Succeeded: reports the word, or 0. */
	return value;
}

/*
 * Writes a 32-bit mailbox word of the set-up instance (either backend);
 * nothing is written when there is none or the offset is out of the
 * OpRegion.
 */
void
drv_i915_opregion_mbox_write(
	struct i915_display *display,
	unsigned off,
	uint32_t v)
{
	struct i915_opregion_world *world;

	/* Writes the word when the instance is set up and the offset fits. */
	world = display->opregion_world;
	if (world->i915_opregion_dev.display.opregion.header != NULL && off + 4u <= OPREGION_SIZE)
		kern_memcpy((u8 *)world->i915_opregion_dev.display.opregion.header + off, &v, 4);
}

/*
 * Returns the VBT the setup found and its size, or NULL.
 */
const void *
drv_i915_opregion_vbt(
	struct i915_display *display,
	uint32_t *size)
{
	struct intel_opregion *opregion;

	/* Reports the size alongside the VBT. */
	opregion = &display->opregion_world->i915_opregion_dev.display.opregion;
	*size = opregion->vbt_size;

	/* Succeeded: reports the VBT, or NULL. */
	return opregion->vbt;
}

/*
 * Tells whether the instance's video event callback is registered.
 */
int
drv_i915_opregion_notifier_registered(
	struct i915_display *display)
{
	/* The registration sets the callback and the unregistration clears it. */
	if (display->opregion_world->i915_opregion_dev.display.opregion.acpi_notifier.notifier_call == NULL)
		return 0;

	/* Succeeded: the callback is registered. */
	return 1;
}

/*
 * Reports the SWSCI accesses reached, the boundaries reached and the
 * unmaps done.
 */
void
drv_i915_opregion_counters(
	struct i915_display *display,
	unsigned *unported,
	unsigned *boundaries,
	unsigned *unmaps)
{
	struct i915_opregion_world *world;

	/* Copies the three counters. */
	world = display->opregion_world;
	*unported = world->i915_opregion_unported;
	*boundaries = world->i915_opregion_boundaries;
	*unmaps = world->i915_opregion_unmaps;
}

/*
 * Reports the GSE requests the gate dropped and the cleanups refused.
 */
void
drv_i915_opregion_gate_counters(
	struct i915_display *display,
	unsigned *dropped,
	unsigned *cleanup_refused)
{
	struct i915_opregion_world *world;

	/* Copies the two counters. */
	world = display->opregion_world;
	*dropped = world->i915_opregion_dropped;
	*cleanup_refused = world->i915_opregion_cleanup_refused;
}

/*
 * Reports how the ASLE work was queued and run.
 */
void
drv_i915_opregion_worker_stats_get(
	struct i915_display *display,
	unsigned *started,
	unsigned *finished,
	unsigned *queued_new,
	unsigned *queued_pending)
{
	struct i915_opregion_world *world;

	/* Copies the four counters. */
	world = display->opregion_world;
	*started = world->i915_opregion_wstats.started;
	*finished = world->i915_opregion_wstats.finished;
	*queued_new = world->i915_opregion_wstats.queued_new;
	*queued_pending = world->i915_opregion_wstats.queued_pending;
}

/*
 * Starts the ASLE service: its worker queue, its backlight policy (the
 * Linux acpi_video_get_backlight_type()), no backlight targets and clean
 * worker counters.
 */
int
drv_i915_opregion_service_start(
	struct i915_display *display,
	struct i915_workqueue *wq,
	int policy)
{
	struct i915_opregion_world *world;

	/* The connection mutex is initialized by the first start. */
	world = display->opregion_world;
	if (!world->i915_opregion_conn_lock_live) {
		(void)mutex_init(&world->i915_opregion_conn_lock, LOCK_RANK_DEVICE, "i915-opregion-connection");
		world->i915_opregion_conn_lock_live = 1;
	}

	/* Binds the queue and the policy, and empties the targets and the counters. */
	world->i915_opregion_dev.unordered_wq = wq;
	world->i915_opregion_policy = policy;
	world->i915_opregion_nbl = 0u;
	kern_memset(&world->i915_opregion_wstats, 0, sizeof(world->i915_opregion_wstats));

	/* Succeeded: the service is started. */
	return 0;
}

/*
 * Changes the backlight policy the ASLE service follows.
 */
void
drv_i915_opregion_set_policy(
	struct i915_display *display,
	int policy)
{
	/* Records the policy. */
	display->opregion_world->i915_opregion_policy = policy;
}

/*
 * Registers a connector the ASLE backlight requests are handed to.
 *
 * set_acpi (called with ctx, the level and the maximum) receives each
 * request; NULL registers a connector without a backlight.  Returns 0, or
 * ENOSPC when the targets are full.
 */
int
drv_i915_opregion_add_connector(
	struct i915_display *display,
	int drm_connector_type,
	void (*set_acpi)(void *ctx, uint32_t level, uint32_t max),
	void *ctx)
{
	struct i915_opregion_world *world;
	struct i915_opregion_backlight_target *target;
	unsigned index;

	/* Refuses a target the table has no room for. */
	world = display->opregion_world;
	index = world->i915_opregion_nbl;
	if (index >= I915_OPREGION_BACKLIGHT_TARGETS)
		return ENOSPC;

	/* Fills the target: its hook, its state's index and its connector. */
	target = &world->i915_opregion_bl[index];
	kern_memset(target, 0, sizeof(*target));
	target->set_acpi = set_acpi;
	target->ctx = ctx;
	target->state.target = index;
	target->conn.base.state = &target->state;
	target->conn.base.connector_type = drm_connector_type;

	/* Publishes it to the connector walk. */
	world->i915_opregion_nbl = index + 1u;

	/* Succeeded: the connector receives the requests. */
	return 0;
}

/*
 * Registers an eDP connector the ASLE backlight requests are handed to.
 */
int
drv_i915_opregion_add_backlight(
	struct i915_display *display,
	void (*set_acpi)(void *ctx, uint32_t level, uint32_t max),
	void *ctx)
{
	int error;

	/* Registers it as an eDP connector. */
	error = drv_i915_opregion_add_connector(display, DRM_MODE_CONNECTOR_eDP, set_acpi, ctx);
	if (error != 0)
		return error;

	/* Succeeded: the backlight receives the requests. */
	return 0;
}

/*
 * Receives a GSE request (what the GU_MISC GSE decode calls:
 * gen11_gu_misc_irq_handler() -> intel_opregion_asle_intr()).
 *
 * A request while the gate is closed (stopped or not started) queues
 * nothing and is counted.
 */
void
drv_i915_opregion_gse_entry(
	struct i915_display *display)
{
	struct i915_opregion_world *world;

	/* A late or early request is dropped. */
	world = display->opregion_world;
	if (!world->i915_opregion_accepting) {
		world->i915_opregion_dropped++;
		return;
	}

	/* Queues the ASLE work. */
	i915_opregion_asle_intr(&world->i915_opregion_dev);
}

/*
 * Waits until the queued or running ASLE work has finished.
 *
 * Returns 1 when it was pending or running, 0 when it was idle or there is
 * no queue.
 */
int
drv_i915_opregion_asle_flush(
	struct i915_display *display,
	uint64_t deadline)
{
	struct i915_opregion_world *world;
	int flushed;

	/* Without a queue nothing can be pending. */
	world = display->opregion_world;
	if (world->i915_opregion_dev.unordered_wq == NULL)
		return 0;

	/* Waits for the work. */
	flushed = drv_i915_flush_work(world->i915_opregion_dev.unordered_wq,
				      &world->i915_opregion_dev.display.opregion.asle_work.kwork,
				      deadline);

	/* Succeeded: reports whether there was work to wait for. */
	return flushed;
}

/*
 * Logs a Linux WARN_ON() condition that holds, and reports it.
 */
int
drv_i915_opregion_warn_on(
	int cond,
	const char *what)
{
	/* Names the condition that held. */
	if (cond)
		kern_logf("i915: opregion WARN_ON(%s)\n", what);

	/* Succeeded: reports the condition. */
	return cond;
}

/*
 * Returns the backlight policy (the Linux acpi_video_get_backlight_type()).
 */
int
drv_i915_opregion_backlight_policy(void)
{
	struct i915_opregion_world *world;

	/* Without a world the policy is the initial one. */
	world = i915_opregion_bound_world();
	if (world == NULL)
		return acpi_backlight_vendor;

	/* Succeeded: reports the service's policy. */
	return world->i915_opregion_policy;
}

/*
 * Takes the connection mutex of the Linux text.
 */
void
drv_i915_opregion_connection_lock(
	struct i915_opregion_world *world)
{
	/* The mutex the service start initialized. */
	mutex_lock(&world->i915_opregion_conn_lock);
}

/*
 * Releases the connection mutex of the Linux text.
 */
void
drv_i915_opregion_connection_unlock(
	struct i915_opregion_world *world)
{
	/* The mutex drv_i915_opregion_connection_lock() took. */
	mutex_unlock(&world->i915_opregion_conn_lock);
}

/*
 * Returns the next registered connector of a connector walk, or NULL at the
 * end.
 */
struct intel_connector *
drv_i915_opregion_connector_next(
	struct i915_opregion_world *world,
	struct drm_connector_list_iter *it)
{
	struct intel_connector *connector;

	/* The walk ends after the last registered target. */
	if (it->idx >= world->i915_opregion_nbl)
		return NULL;

	/* Hands out the next target's connector. */
	connector = &world->i915_opregion_bl[it->idx].conn;
	it->idx++;

	/* Succeeded: reports the connector. */
	return connector;
}

/*
 * Hands an ASLE backlight request to the connector's registered target (the
 * Linux intel_backlight_set_acpi()).
 *
 * A connector without a backlight has no hook, and Linux returns at once
 * for it.
 */
void
drv_i915_opregion_backlight_set_acpi(
	const struct drm_connector_state *st,
	u32 level,
	u32 max)
{
	struct i915_opregion_world *world;
	struct i915_opregion_backlight_target *target;

	/* Without a world or a state there is no target. */
	world = i915_opregion_bound_world();
	if (world == NULL)
		return;
	if (st == NULL)
		return;

	/* The state names a registered target. */
	if (st->target >= world->i915_opregion_nbl)
		return;

	/* Calls the target's hook, when it has one. */
	target = &world->i915_opregion_bl[st->target];
	if (target->set_acpi != NULL)
		target->set_acpi(target->ctx, level, max);
}

/*
 * Reads a dword of the instance's PCI configuration space: ASLS reads the
 * setup's token, and any other offset reads 0 and is recorded as an
 * unported access.
 */
int
drv_i915_opregion_pci_read32(
	struct pci_dev *pdev,
	int where,
	u32 *val)
{
	struct i915_opregion_world *world;

	UNUSED_PARAMETER(pdev);

	/* ASLS reads the token; anything else reads 0. */
	world = i915_opregion_bound_world();
	*val = 0u;
	if (where == ASLS && world != NULL)
		*val = world->i915_opregion_asls;

	/* Any other offset is an access the target does not provide. */
	if (where != ASLS)
		(void)drv_i915_opregion_pci_access_unported("pci_read_config_dword (not ASLS)");

	/* Succeeded: the dword was read. */
	return 0;
}

/*
 * Records an SWSCI configuration access or poll the target does not
 * provide (it has no SWSCI mailbox), and refuses it with ENODEV.
 */
int
drv_i915_opregion_pci_access_unported(
	const char *what)
{
	struct i915_opregion_world *world;

	/* Counts the access. */
	world = i915_opregion_bound_world();
	if (world != NULL)
		world->i915_opregion_unported++;

	/* Logs it as an error, never as a faked success. */
	kern_logf("i915: opregion ERROR %s reached: SWSCI config access is not provided (no SWSCI mailbox on the target)\n",
		what);

	/* The access is refused. */
	return ENODEV;
}

/*
 * Resolves a physical range through the instance's mapping table (the
 * Linux memremap()), or returns NULL when no region holds it.
 */
void *
drv_i915_opregion_memremap(
	resource_size_t phys,
	size_t size,
	unsigned long flags)
{
	struct i915_opregion_world *world;
	struct i915_opregion_map *map;
	unsigned i;

	UNUSED_PARAMETER(flags);

	/* Without a world there is no table. */
	world = i915_opregion_bound_world();
	if (world == NULL)
		return NULL;

	/* Looks for a region that holds the whole range. */
	for (i = 0u; i < world->i915_opregion_nmaps; i++) {
		map = &world->i915_opregion_maps[i];
		if (phys >= map->phys && phys + size <= map->phys + map->size)
			return (char *)map->ptr + (phys - map->phys);
	}

	/* No region holds it. */
	kern_logf("i915: opregion memremap(0x%llx, %u): not in the %s mapping table\n", (unsigned long long)phys,
		(unsigned)size, world->i915_opregion_backend);
	return NULL;
}

/*
 * Counts a Linux memunmap(); the memory stays with its owner (the test for
 * SHADOW, the cleanup for FIRMWARE).
 */
void
drv_i915_opregion_memunmap(
	void *p)
{
	struct i915_opregion_world *world;

	UNUSED_PARAMETER(p);

	/* Counts the unmap. */
	world = i915_opregion_bound_world();
	if (world != NULL)
		world->i915_opregion_unmaps++;
}

/*
 * Matches the system against a DMI quirk list (the Linux
 * dmi_check_system()).
 *
 * zedBSD has no DMI data source, so nothing matches.
 */
int
drv_i915_opregion_dmi_check_system(
	const struct dmi_system_id *list)
{
	UNUSED_PARAMETER(list);

	/* Reports that no entry matched. */
	return 0;
}

/*
 * Records a boundary the Linux text reached and that is not evaluated here
 * (ACPI _DSM, the SWSCI mailbox setup).
 */
void
drv_i915_opregion_boundary(
	const char *what)
{
	struct i915_opregion_world *world;

	/* Counts the boundary. */
	world = i915_opregion_bound_world();
	if (world != NULL)
		world->i915_opregion_boundaries++;

	/* Logs it. */
	kern_logf("i915: opregion boundary: %s -- not evaluated\n", what);
}

/*
 * Cancels the ASLE work and waits until it is not running (the Linux
 * cancel_work_sync()).
 *
 * Returns 1 when it was pending, else 0; without a queue nothing can be
 * pending or running.
 */
int
drv_i915_opregion_cancel_work_sync(
	struct work_struct *w)
{
	struct i915_opregion_world *world;
	uint64_t deadline;
	int cancelled;

	/* Without a queue nothing can be pending or running. */
	world = i915_opregion_world_of_work(w);
	if (world->i915_opregion_dev.unordered_wq == NULL)
		return 0;

	/* Cancels and waits, bounded by the deadline. */
	deadline = sched_ticks() + I915_OPREGION_CANCEL_TICKS;
	cancelled = drv_i915_cancel_work_sync(world->i915_opregion_dev.unordered_wq, &w->kwork, deadline);

	/* Succeeded: reports whether the work was pending. */
	return cancelled;
}

/*
 * Sets up the OpRegion the device's ASLS names (intel_opregion_setup()).
 *
 * The mailboxes found are recorded, the ASLE driver readiness is cleared,
 * hotplug notifications are declined, and the VBT is looked for in the
 * firmware file (never named here), the RVDA and mailbox #4.  Returns 0,
 * I915_OPREGION_ENOTSUPP without an OpRegion, ENOMEM when it cannot be
 * mapped, or EINVAL for a wrong signature.
 */
int
drv_i915_opregion_setup(
	struct drm_i915_private *dev_priv)
{
	struct intel_opregion *opregion;
	u32 asls;
	u32 mboxes;
	char buf[sizeof(OPREGION_SIGNATURE)];
	void *base;
	int compared;

	BUILD_BUG_ON(sizeof(struct opregion_header) != 0x100);
	BUILD_BUG_ON(sizeof(struct opregion_acpi) != 0x100);
	BUILD_BUG_ON(sizeof(struct opregion_swsci) != 0x100);
	BUILD_BUG_ON(sizeof(struct opregion_asle) != 0x100);
	BUILD_BUG_ON(sizeof(struct opregion_asle_ext) != 0x400);

	/* No ASLS is no OpRegion. */
	opregion = &dev_priv->display.opregion;
	(void)pci_read_config_dword(to_pci_dev(dev_priv->drm.dev), ASLS, &asls);
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "graphic opregion physical addr: 0x%x\n", asls);
	if (asls == 0) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "ACPI OpRegion not supported!\n");
		return I915_OPREGION_ENOTSUPP;
	}

	/* Prepares the ASLE work. */
	i915_opregion_init_work(&opregion->asle_work, i915_asle_work);

	/* Maps the OpRegion. */
	base = memremap(asls, OPREGION_SIZE, MEMREMAP_WB);
	if (!base)
		return ENOMEM;

	/* Refuses a region without the OpRegion signature. */
	kern_memcpy(buf, base, sizeof(buf));
	compared = kern_memcmp(buf, OPREGION_SIGNATURE, 16);
	if (compared != 0) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "opregion signature mismatch\n");
		memunmap(base);
		return EINVAL;
	}

	/* Records the header and the lid state. */
	opregion->header = base;
	opregion->lid_state = (u32 *)((u8 *)base + ACPI_CLID);
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "ACPI OpRegion version %u.%u.%u\n",
		opregion->header->over.major,
		opregion->header->over.minor,
		opregion->header->over.revision);

	/* Records the mailboxes. */
	mboxes = opregion->header->mboxes;
	i915_opregion_setup_mailboxes(dev_priv, base, mboxes);

	/* Looks for the VBT. */
	i915_opregion_find_vbt(dev_priv, asls, base, mboxes);

	/* Succeeded: the OpRegion is set up. */
	return 0;
}

/*
 * Gives every connector its ACPI device id.
 *
 * The id is the connector's display type with, in the index field, how many
 * connectors of the same type came before it on the connector list.
 */
void
drv_i915_acpi_device_id_update(
	struct drm_i915_private *dev_priv)
{
	struct i915_opregion_world *world;
	struct intel_connector *connector;
	struct drm_connector_list_iter conn_iter;
	unsigned seen[I915_ACPI_DISPLAY_TYPES];
	unsigned type;
	u32 display_type;

	/* No connector of any type has been numbered yet. */
	kern_memset(seen, 0, sizeof(seen));

	/* Numbers the connectors within their display type, in list order. */
	world = i915_opregion_world_of(dev_priv);
	i915_opregion_drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	I915_OPREGION_FOR_EACH_INTEL_CONNECTOR_ITER(world, connector, &conn_iter) {
		display_type = i915_acpi_display_type(connector);
		type = (display_type & I915_ACPI_DISPLAY_TYPE_MASK) >> I915_ACPI_DISPLAY_TYPE_SHIFT;
		connector->acpi_device_id = display_type | ((u32)seen[type] << I915_ACPI_DISPLAY_INDEX_SHIFT);
		seen[type]++;
	}
	i915_opregion_drm_connector_list_iter_end(&conn_iter);
}

/* Returns the world of the bound display, or NULL. */
static struct i915_opregion_world *
i915_opregion_bound_world(void)
{
	/* Without a bound display there is no world. */
	if (i915_opregion_bound_display == NULL)
		return NULL;

	/* Succeeded: reports the display's world. */
	return i915_opregion_bound_display->opregion_world;
}

/* Puts the instance's notifier block on the bound display's chain (the Linux register_acpi_notifier()). */
static int
i915_register_acpi_notifier(
	struct notifier_block *nb)
{
	int error;

	/* Without a display there is no chain. */
	if (i915_opregion_bound_display == NULL)
		return ENODEV;

	/* Registers the block. */
	error = drv_i915_register_acpi_notifier(i915_opregion_bound_display, nb);
	if (error != 0)
		return error;

	/* Succeeded: the block is on the chain. */
	return 0;
}

/* Takes the instance's notifier block off the bound display's chain (the Linux unregister_acpi_notifier()). */
static int
i915_unregister_acpi_notifier(
	struct notifier_block *nb)
{
	int error;

	/* Without a display there is no chain. */
	if (i915_opregion_bound_display == NULL)
		return ENODEV;

	/* Unregisters the block. */
	error = drv_i915_unregister_acpi_notifier(i915_opregion_bound_display, nb);
	if (error != 0)
		return error;

	/* Succeeded: the block is off the chain. */
	return 0;
}

/* Undoes every HAL mapping the FIRMWARE backend made, the last first. */
static void
i915_opregion_unmap_hal(
	struct i915_opregion_world *world)
{
	struct i915_opregion_hal_map *map;

	/* Unmaps in reverse order. */
	while (world->i915_opregion_nhal > 0u) {
		world->i915_opregion_nhal--;
		map = &world->i915_opregion_hal[world->i915_opregion_nhal];
		(void)hal_space_unmap_device(map->va, map->size);
	}
}

/* Refuses an ambient light reading: not supported. */
static u32
i915_asle_set_als_illum(
	struct drm_i915_private *dev_priv,
	u32 alsi)
{
	UNUSED_PARAMETER(alsi);

	/*
	 * alsi is the current ALS reading in lux. 0 indicates below sensor
	 * range, 0xffff indicates above sensor range. 1-0xfffe are valid
	 */
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "Illum is not supported\n");
	return ASLC_ALS_ILLUM_FAILED;
}

/* Hands a backlight request to every connector with a backlight, and records the level set. */
static u32
i915_asle_set_backlight(
	struct drm_i915_private *dev_priv,
	u32 bclp)
{
	struct i915_opregion_world *world;
	struct intel_connector *connector;
	struct drm_connector_list_iter conn_iter;
	struct opregion_asle *asle;
	enum acpi_backlight_type policy;

	/* The native backlight policy ignores OpRegion requests. */
	asle = dev_priv->display.opregion.asle;
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "bclp = 0x%08x\n", bclp);
	policy = acpi_video_get_backlight_type();
	if (policy == acpi_backlight_native) {
		I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm,
			"opregion backlight request ignored\n");
		return 0;
	}

	/* Refuses a request that is not marked valid, or is out of range. */
	if (!(bclp & ASLE_BCLP_VALID))
		return ASLC_BACKLIGHT_FAILED;
	bclp &= ASLE_BCLP_MSK;
	if (bclp > 255)
		return ASLC_BACKLIGHT_FAILED;

	/*
	 * Update backlight on all connectors that support backlight (usually
	 * only one).
	 */
	world = i915_opregion_world_of(dev_priv);
	(void)i915_opregion_drm_modeset_lock(&dev_priv->drm.mode_config.connection_mutex, NULL);

	I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm, "updating opregion backlight %d/255\n",
		bclp);
	i915_opregion_drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	I915_OPREGION_FOR_EACH_INTEL_CONNECTOR_ITER(world, connector, &conn_iter) {
		intel_backlight_set_acpi(connector->base.state, bclp, 255);
	}
	i915_opregion_drm_connector_list_iter_end(&conn_iter);
	asle->cblv = DIV_ROUND_UP(bclp * 100, 255) | ASLE_CBLV_VALID;

	drm_modeset_unlock(&dev_priv->drm.mode_config.connection_mutex);

	/* Succeeded: the request was served. */
	return 0;
}

/* Refuses a PWM frequency request: not supported. */
static u32
i915_asle_set_pwm_freq(
	struct drm_i915_private *dev_priv,
	u32 pfmb)
{
	UNUSED_PARAMETER(pfmb);

	/* Reports the request as failed. */
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "PWM freq is not supported\n");
	return ASLC_PWM_FREQ_FAILED;
}

/* Refuses a panel fitting request: not supported. */
static u32
i915_asle_set_pfit(
	struct drm_i915_private *dev_priv,
	u32 pfit)
{
	UNUSED_PARAMETER(pfit);

	/*
	 * Panel fitting is currently controlled by the X code, so this is a
	 * noop until modesetting support works fully
	 */
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "Pfit is not supported\n");
	return ASLC_PFIT_FAILED;
}

/* Refuses a supported rotation angles request: not supported. */
static u32
i915_asle_set_supported_rotation_angles(
	struct drm_i915_private *dev_priv,
	u32 srot)
{
	UNUSED_PARAMETER(srot);

	/* Reports the request as failed. */
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "SROT is not supported\n");
	return ASLC_ROTATION_ANGLES_FAILED;
}

/* Refuses a button array event: not supported. */
static u32
i915_asle_set_button_array(
	struct drm_i915_private *dev_priv,
	u32 iuer)
{
	/* Names the buttons of the event. */
	if (!iuer) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Button array event is not supported (nothing)\n");
	}
	if (iuer & ASLE_IUER_ROTATION_LOCK_BTN) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Button array event is not supported (rotation lock)\n");
	}
	if (iuer & ASLE_IUER_VOLUME_DOWN_BTN) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Button array event is not supported (volume down)\n");
	}
	if (iuer & ASLE_IUER_VOLUME_UP_BTN) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Button array event is not supported (volume up)\n");
	}
	if (iuer & ASLE_IUER_WINDOWS_BTN) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Button array event is not supported (windows)\n");
	}
	if (iuer & ASLE_IUER_POWER_BTN) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Button array event is not supported (power)\n");
	}

	/* Reports the event as failed. */
	return ASLC_BUTTON_ARRAY_FAILED;
}

/* Refuses a convertible indicator: not supported. */
static u32
i915_asle_set_convertible(
	struct drm_i915_private *dev_priv,
	u32 iuer)
{
	/* Names the mode. */
	if (iuer & ASLE_IUER_CONVERTIBLE) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Convertible is not supported (clamshell)\n");
	} else {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Convertible is not supported (slate)\n");
	}

	/* Reports the indicator as failed. */
	return ASLC_CONVERTIBLE_FAILED;
}

/* Refuses a docking indicator: not supported. */
static u32
i915_asle_set_docking(
	struct drm_i915_private *dev_priv,
	u32 iuer)
{
	/* Names the state. */
	if (iuer & ASLE_IUER_DOCKING) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "Docking is not supported (docked)\n");
	} else {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"Docking is not supported (undocked)\n");
	}

	/* Reports the indicator as failed. */
	return ASLC_DOCKING_FAILED;
}

/* Refuses an ISCT state change: not supported. */
static u32
i915_asle_isct_state(
	struct drm_i915_private *dev_priv)
{
	/* Reports the change as failed. */
	I915_OPREGION_DRM_DBG(&dev_priv->drm, "ISCT is not supported\n");
	return ASLC_ISCT_STATE_FAILED;
}

/* Serves the requests of an ASLE interrupt and writes their status back (the ASLE work). */
static void
i915_asle_work(
	struct work_struct *work)
{
	struct intel_opregion *opregion;
	struct drm_i915_private *dev_priv;
	struct opregion_asle *asle;
	u32 aslc_stat;
	u32 aslc_req;

	/* Without an ASLE mailbox there is nothing to serve. */
	opregion = container_of(work, struct intel_opregion, asle_work);
	dev_priv = container_of(opregion, struct drm_i915_private, display.opregion);
	asle = dev_priv->display.opregion.asle;
	aslc_stat = 0;
	if (!asle)
		return;

	/* An interrupt without a request is ignored. */
	aslc_req = asle->aslc;
	if (!(aslc_req & ASLC_REQ_MSK)) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm,
			"No request on ASLC interrupt 0x%08x\n", aslc_req);
		return;
	}

	/* Serves each request, collecting the failure bits. */
	if (aslc_req & ASLC_SET_ALS_ILLUM)
		aslc_stat |= i915_asle_set_als_illum(dev_priv, asle->alsi);
	if (aslc_req & ASLC_SET_BACKLIGHT)
		aslc_stat |= i915_asle_set_backlight(dev_priv, asle->bclp);
	if (aslc_req & ASLC_SET_PFIT)
		aslc_stat |= i915_asle_set_pfit(dev_priv, asle->pfit);
	if (aslc_req & ASLC_SET_PWM_FREQ)
		aslc_stat |= i915_asle_set_pwm_freq(dev_priv, asle->pfmb);
	if (aslc_req & ASLC_SUPPORTED_ROTATION_ANGLES) {
		aslc_stat |= i915_asle_set_supported_rotation_angles(dev_priv,
								      asle->srot);
	}
	if (aslc_req & ASLC_BUTTON_ARRAY)
		aslc_stat |= i915_asle_set_button_array(dev_priv, asle->iuer);
	if (aslc_req & ASLC_CONVERTIBLE_INDICATOR)
		aslc_stat |= i915_asle_set_convertible(dev_priv, asle->iuer);
	if (aslc_req & ASLC_DOCKING_INDICATOR)
		aslc_stat |= i915_asle_set_docking(dev_priv, asle->iuer);
	if (aslc_req & ASLC_ISCT_STATE_CHANGE)
		aslc_stat |= i915_asle_isct_state(dev_priv);

	/* Writes the status back. */
	asle->aslc = aslc_stat;
}

/* Queues the ASLE work when there is an ASLE mailbox (intel_opregion_asle_intr()). */
static void
i915_opregion_asle_intr(
	struct drm_i915_private *dev_priv)
{
	/* Queues the work; an already pending one is counted, not refused. */
	if (dev_priv->display.opregion.asle) {
		(void)i915_opregion_queue_work(dev_priv->unordered_wq,
					       &dev_priv->display.opregion.asle_work);
	}
}

/*
 * Answers an ACPI video event (intel_opregion_video_event()).
 *
 * The only video events relevant to opregion are 0x80. These indicate either a
 * docking event, lid switch or display switch request. In Linux, these are
 * handled by the dock, button and video drivers.
 */
static int
i915_opregion_video_event(
	struct notifier_block *nb,
	unsigned long val,
	void *data)
{
	struct intel_opregion *opregion;
	struct acpi_bus_event *event;
	struct opregion_acpi *acpi;
	int compared;
	int ret;

	UNUSED_PARAMETER(val);

	/* Events of other device classes are not for the OpRegion. */
	opregion = container_of(nb, struct intel_opregion, acpi_notifier);
	event = data;
	ret = NOTIFY_OK;
	compared = kern_strcmp(event->device_class, ACPI_VIDEO_CLASS);
	if (compared != 0)
		return NOTIFY_DONE;

	/* A 0x80 event the firmware did not mark in CEVT is bad. */
	acpi = opregion->acpi;
	if (event->type == 0x80 && ((acpi->cevt & 1) == 0))
		ret = NOTIFY_BAD;

	/* Clears the notification status. */
	acpi->csts = 0;

	/* Succeeded: reports the answer. */
	return ret;
}

/* Tells whether an SWSCI function may be called (check_swsci_function()): 0, ENODEV or EINVAL. */
static int
i915_check_swsci_function(
	struct drm_i915_private *i915,
	u32 function)
{
	struct opregion_swsci *swsci;
	u32 main_function;
	u32 sub_function;

	/* Without the SWSCI mailbox no function may be called. */
	swsci = i915->display.opregion.swsci;
	if (!swsci)
		return ENODEV;

	/* Splits the function code. */
	main_function = (function & SWSCI_SCIC_MAIN_FUNCTION_MASK) >>
		SWSCI_SCIC_MAIN_FUNCTION_SHIFT;
	sub_function = (function & SWSCI_SCIC_SUB_FUNCTION_MASK) >>
		SWSCI_SCIC_SUB_FUNCTION_SHIFT;

	/* Check if we can call the function. See swsci_setup for details. */
	if (main_function == SWSCI_SBCB) {
		if ((i915->display.opregion.swsci_sbcb_sub_functions &
		     (1 << sub_function)) == 0)
			return EINVAL;
	} else if (main_function == SWSCI_GBDA) {
		if ((i915->display.opregion.swsci_gbda_sub_functions &
		     (1 << sub_function)) == 0)
			return EINVAL;
	}

	/* Succeeded: the function may be called. */
	return 0;
}

/*
 * Makes one SWSCI request and waits for the firmware's answer (swsci()).
 *
 * Returns 0 with *parm_out set when given, the check's refusal, EBUSY when
 * a request is in progress, ETIMEDOUT, or EIO for a failed request.
 */
static int
i915_swsci(
	struct drm_i915_private *dev_priv,
	u32 function,
	u32 parm,
	u32 *parm_out)
{
	struct opregion_swsci *swsci;
	u32 scic;
	u32 dslp;
	u16 swsci_val;
	int error;
	int timed_out;

	/* Refuses a function that may not be called. */
	swsci = dev_priv->display.opregion.swsci;
	error = i915_check_swsci_function(dev_priv, function);
	if (error != 0)
		return error;

	/* Driver sleep timeout in ms. */
	dslp = swsci->dslp;
	if (!dslp) {
		/*
		 * The spec says 2ms should be the default, but it's too small
		 * for some machines.
		 */
		dslp = 50;
	} else if (dslp > MAX_DSLP) {
		/* Hey bios, trust must be earned. */
		DRM_INFO_ONCE("ACPI BIOS requests an excessive sleep of %u ms, "
			      "using %u ms instead\n", dslp, MAX_DSLP);
		dslp = MAX_DSLP;
	}

	/* The spec tells us to do this, but we are the only user... */
	scic = swsci->scic;
	if (scic & SWSCI_SCIC_INDICATOR) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "SWSCI request already in progress\n");
		return EBUSY;
	}

	/* Writes the request. */
	scic = function | SWSCI_SCIC_INDICATOR;
	swsci->parm = parm;
	swsci->scic = scic;

	/* Ensure SCI event is selected and event trigger is cleared. */
	(void)pci_read_config_word(to_pci_dev(dev_priv->drm.dev), SWSCI, &swsci_val);
	if (!(swsci_val & SWSCI_SCISEL) || (swsci_val & SWSCI_GSSCIE)) {
		swsci_val |= SWSCI_SCISEL;
		swsci_val &= ~SWSCI_GSSCIE;
		(void)pci_write_config_word(to_pci_dev(dev_priv->drm.dev), SWSCI, swsci_val);
	}

	/* Use event trigger to tell bios to check the mail. */
	swsci_val |= SWSCI_GSSCIE;
	(void)pci_write_config_word(to_pci_dev(dev_priv->drm.dev), SWSCI, swsci_val);

	/* Poll for the result. */
	timed_out = I915_OPREGION_WAIT_FOR(((scic = swsci->scic) & SWSCI_SCIC_INDICATOR) == 0, dslp);
	if (timed_out) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "SWSCI request timed out\n");
		return ETIMEDOUT;
	}

	/* Note: scic == 0 is an error! */
	scic = (scic & SWSCI_SCIC_EXIT_STATUS_MASK) >>
		SWSCI_SCIC_EXIT_STATUS_SHIFT;
	if (scic != SWSCI_SCIC_EXIT_STATUS_SUCCESS) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "SWSCI request error %u\n", scic);
		return EIO;
	}

	/* Hands the answer out. */
	if (parm_out)
		*parm_out = swsci->parm;

	/* Succeeded: the firmware served the request. */
	return 0;
}

/*
 * Tells the firmware the adapter's PCI power state
 * (intel_opregion_notify_adapter()): 0, the SWSCI refusal, or EINVAL for
 * an unknown state.
 */
static int
i915_opregion_notify_adapter(
	struct drm_i915_private *dev_priv,
	pci_power_t state)
{
	size_t i;
	bool has_ddi;
	int error;

	/* Only DDI platforms are told. */
	has_ddi = i915_opregion_has_ddi(dev_priv);
	if (!has_ddi)
		return 0;

	/* Sends the parameter of the state. */
	for (i = 0; i < ARRAY_SIZE(power_state_map); i++) {
		if (state != power_state_map[i].pci_power_state)
			continue;

		error = i915_swsci(dev_priv, SWSCI_SBCB_ADAPTER_POWER_STATE,
				   power_state_map[i].parm, NULL);
		if (error != 0)
			return error;

		/* Succeeded: the firmware was told. */
		return 0;
	}

	/* The state has no parameter. */
	return EINVAL;
}

/*
 * Sets one entry of the DIDL list, or of the extended did2 list after it
 * (set_did()).
 *
 * The list passes the devices to the firmware; the values are defined by
 * section B.4.2 of the ACPI specification (version 3).
 */
static void
i915_set_did(
	struct intel_opregion *opregion,
	int i,
	u32 val)
{
	int warned;

	/* An index within DIDL sets DIDL. */
	if ((size_t)i < ARRAY_SIZE(opregion->acpi->didl)) {
		opregion->acpi->didl[i] = val;
	} else {
		i -= ARRAY_SIZE(opregion->acpi->didl);

		/* An index beyond did2 is warned about and dropped. */
		warned = I915_OPREGION_WARN_ON((size_t)i >= ARRAY_SIZE(opregion->acpi->did2));
		if (warned)
			return;

		opregion->acpi->did2[i] = val;
	}
}

/* Passes the connectors' ACPI device ids to the firmware in DIDL (intel_didl_outputs()). */
static void
i915_didl_outputs(
	struct drm_i915_private *dev_priv)
{
	struct i915_opregion_world *world;
	struct intel_opregion *opregion;
	struct intel_connector *connector;
	struct drm_connector_list_iter conn_iter;
	int i;
	int max_outputs;

	/*
	 * In theory, did2, the extended didl, gets added at opregion version
	 * 3.0. In practice, however, we're supposed to set it for earlier
	 * versions as well, since a BIOS that doesn't understand did2 should
	 * not look at it anyway. Use a variable so we can tweak this if a need
	 * arises later.
	 */
	opregion = &dev_priv->display.opregion;
	world = i915_opregion_world_of(dev_priv);
	i = 0;
	max_outputs = ARRAY_SIZE(opregion->acpi->didl) +
		ARRAY_SIZE(opregion->acpi->did2);

	/* Gives every connector its ACPI device id. */
	drv_i915_acpi_device_id_update(dev_priv);

	/* Lists the ids, counting every connector. */
	i915_opregion_drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	I915_OPREGION_FOR_EACH_INTEL_CONNECTOR_ITER(world, connector, &conn_iter) {
		if (i < max_outputs)
			i915_set_did(opregion, i, connector->acpi_device_id);
		i++;
	}
	i915_opregion_drm_connector_list_iter_end(&conn_iter);
	I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm, "%d outputs detected\n", i);

	/* More connectors than entries are reported. */
	if (i > max_outputs) {
		I915_OPREGION_DRM_ERR(&dev_priv->drm,
			"More than %d outputs in connector list\n",
			max_outputs);
	}

	/* If fewer than max outputs, the list must be null terminated */
	if (i < max_outputs)
		i915_set_did(opregion, i, 0);
}

/* Fills CADL, the currently active display list, from the connectors' ids (intel_setup_cadls()). */
static void
i915_setup_cadls(
	struct drm_i915_private *dev_priv)
{
	struct i915_opregion_world *world;
	struct intel_opregion *opregion;
	struct intel_connector *connector;
	struct drm_connector_list_iter conn_iter;
	int i;

	/*
	 * Initialize the CADL field from the connector device ids. This is
	 * essentially the same as copying from the DIDL. Technically, this is
	 * not always correct as display outputs may exist, but not active. This
	 * initialization is necessary for some Clevo laptops that check this
	 * field before processing the brightness and display switching hotkeys.
	 *
	 * Note that internal panels should be at the front of the connector
	 * list already, ensuring they're not left out.
	 */
	opregion = &dev_priv->display.opregion;
	world = i915_opregion_world_of(dev_priv);
	i = 0;
	i915_opregion_drm_connector_list_iter_begin(&dev_priv->drm, &conn_iter);
	I915_OPREGION_FOR_EACH_INTEL_CONNECTOR_ITER(world, connector, &conn_iter) {
		if ((size_t)i >= ARRAY_SIZE(opregion->acpi->cadl))
			break;
		opregion->acpi->cadl[i++] = connector->acpi_device_id;
	}
	i915_opregion_drm_connector_list_iter_end(&conn_iter);

	/* If fewer than 8 active devices, the list must be null terminated */
	if ((size_t)i < ARRAY_SIZE(opregion->acpi->cadl))
		opregion->acpi->cadl[i] = 0;
}

/* Marks a system whose OpRegion VBT must not be used (the DMI quirk callback). */
static int
i915_no_opregion_vbt_callback(
	const struct dmi_system_id *id)
{
	UNUSED_PARAMETER(id);

	/* Reports the quirk as matched. */
	I915_OPREGION_DRM_DEBUG_KMS("Falling back to manually reading VBT from "
		"VBIOS ROM for %s\n", id->ident);
	return 1;
}

/*
 * Loads the VBT from the firmware file display.params.vbt_firmware names
 * (intel_load_vbt_firmware()).
 *
 * The parameter is never set here, so this answers ENOENT at once.  Returns
 * 0 with the VBT recorded, ENOENT, the request's error, ENOMEM, or EINVAL
 * for an invalid VBT.
 */
static int
i915_load_vbt_firmware(
	struct drm_i915_private *dev_priv)
{
	struct intel_opregion *opregion;
	const struct firmware *fw;
	const char *name;
	int requested;
	int valid;
	int error;

	/* Without a name there is no firmware file. */
	opregion = &dev_priv->display.opregion;
	fw = NULL;
	name = dev_priv->display.params.vbt_firmware;
	if (!name || !*name)
		return ENOENT;

	/*
	 * Requests the file.  request_firmware() reports the Linux negative
	 * errno, which is logged as Linux logs it.
	 */
	requested = request_firmware(&fw, name, dev_priv->drm.dev);
	if (requested != 0) {
		I915_OPREGION_DRM_ERR(&dev_priv->drm,
			"Requesting VBT firmware \"%s\" failed (%d)\n",
			name, requested);
		return -requested;
	}

	/* Keeps a copy of a valid VBT. */
	valid = intel_bios_is_valid_vbt(fw->data, fw->size);
	if (valid) {
		opregion->vbt_firmware = I915_OPREGION_KMEMDUP(fw->data, fw->size, GFP_KERNEL);
		if (opregion->vbt_firmware) {
			I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm,
				"Found valid VBT firmware \"%s\"\n", name);
			opregion->vbt = opregion->vbt_firmware;
			opregion->vbt_size = fw->size;
			error = 0;
		} else {
			error = ENOMEM;
		}
	} else {
		I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm, "Invalid VBT firmware \"%s\"\n",
			name);
		error = EINVAL;
	}

	/* Releases the file. */
	release_firmware(fw);

	/* Reports a VBT that could not be kept. */
	if (error != 0)
		return error;

	/* Succeeded: the file's VBT is recorded. */
	return 0;
}

/* Records the mailboxes an OpRegion has (the mailbox part of intel_opregion_setup()). */
static void
i915_opregion_setup_mailboxes(
	struct drm_i915_private *dev_priv,
	void *base,
	u32 mboxes)
{
	struct intel_opregion *opregion;
	u8 major;

	/* Mailbox #1: the public ACPI methods. */
	opregion = &dev_priv->display.opregion;
	if (mboxes & MBOX_ACPI) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "Public ACPI methods supported\n");
		opregion->acpi = (struct opregion_acpi *)((u8 *)base + OPREGION_ACPI_OFFSET);
		/*
		 * Indicate we handle monitor hotplug events ourselves so we do
		 * not need ACPI notifications for them. Disabling these avoids
		 * triggering the AML code doing the notifation, which may be
		 * broken as Windows also seems to disable these.
		 */
		opregion->acpi->chpd = 1;
	}

	/* Mailbox #2: SWSCI, obsolete from version 3. */
	if (mboxes & MBOX_SWSCI) {
		major = opregion->header->over.major;
		if (major >= 3) {
			I915_OPREGION_DRM_ERR(&dev_priv->drm, "SWSCI Mailbox #2 present for opregion v3.x, ignoring\n");
		} else {
			if (major >= 2)
				I915_OPREGION_DRM_DBG(&dev_priv->drm, "SWSCI Mailbox #2 present for opregion v2.x\n");
			I915_OPREGION_DRM_DBG(&dev_priv->drm, "SWSCI supported\n");
			opregion->swsci = (struct opregion_swsci *)((u8 *)base + OPREGION_SWSCI_OFFSET);
			swsci_setup(dev_priv);
		}
	}

	/* Mailbox #3: ASLE, with the driver not ready yet. */
	if (mboxes & MBOX_ASLE) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "ASLE supported\n");
		opregion->asle = (struct opregion_asle *)((u8 *)base + OPREGION_ASLE_OFFSET);
		opregion->asle->ardy = ASLE_ARDY_NOT_READY;
	}

	/* Mailbox #5: the ASLE extension. */
	if (mboxes & MBOX_ASLE_EXT) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "ASLE extension supported\n");
		opregion->asle_ext = (struct opregion_asle_ext *)((u8 *)base + OPREGION_ASLE_EXT_OFFSET);
	}

	/* Mailbox #2 for the backlight, from version 3. */
	if (mboxes & MBOX_BACKLIGHT) {
		I915_OPREGION_DRM_DBG(&dev_priv->drm, "Mailbox #2 for backlight present\n");
	}
}

/*
 * Looks for the VBT: the firmware file, then (unless a DMI quirk says no)
 * the RVDA, then mailbox #4 (the VBT part of intel_opregion_setup()).
 */
static void
i915_opregion_find_vbt(
	struct drm_i915_private *dev_priv,
	u32 asls,
	void *base,
	u32 mboxes)
{
	static const struct dmi_system_id intel_no_opregion_vbt[] = {
		{
			.callback = i915_no_opregion_vbt_callback,
			.ident = "ThinkCentre A57",
			.matches = {
				DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
				DMI_MATCH(DMI_PRODUCT_NAME, "97027RG"),
			},
		},
		{ }
	};
	struct intel_opregion *opregion;
	resource_size_t rvda;
	const void *vbt;
	u32 vbt_size;
	int loaded;
	int quirk;
	int valid;

	/* The firmware file wins. */
	opregion = &dev_priv->display.opregion;
	loaded = i915_load_vbt_firmware(dev_priv);
	if (loaded == 0)
		return;

	/* A system the quirk list names does not use the OpRegion VBT. */
	quirk = dmi_check_system(intel_no_opregion_vbt);
	if (quirk)
		return;

	/* The RVDA, from version 2, when the ASLE mailbox names one. */
	if (opregion->header->over.major >= 2 && opregion->asle &&
	    opregion->asle->rvda && opregion->asle->rvds) {
		rvda = opregion->asle->rvda;

		/*
		 * opregion 2.0: rvda is the physical VBT address.
		 *
		 * opregion 2.1+: rvda is unsigned, relative offset from
		 * opregion base, and should never point within opregion.
		 */
		if (opregion->header->over.major > 2 ||
		    opregion->header->over.minor >= 1) {
			(void)I915_OPREGION_DRM_WARN_ON(&dev_priv->drm, rvda < OPREGION_SIZE);
			rvda += asls;
		}

		/* Maps the RVDA and keeps it when it holds a valid VBT. */
		opregion->rvda = memremap(rvda, opregion->asle->rvds,
					  MEMREMAP_WB);
		vbt = opregion->rvda;
		vbt_size = opregion->asle->rvds;
		valid = intel_bios_is_valid_vbt(vbt, vbt_size);
		if (valid) {
			I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm,
				"Found valid VBT in ACPI OpRegion (RVDA)\n");
			opregion->vbt = vbt;
			opregion->vbt_size = vbt_size;
			return;
		}

		/* An invalid one is unmapped again. */
		I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm,
			"Invalid VBT in ACPI OpRegion (RVDA)\n");
		memunmap(opregion->rvda);
		opregion->rvda = NULL;
	}

	/*
	 * The VBT specification says that if the ASLE ext mailbox is not used
	 * its area is reserved, but on some CHT boards the VBT extends into the
	 * ASLE ext area. Allow this even though it is against the spec, so we
	 * do not end up rejecting the VBT on those boards (and end up not
	 * finding the LCD panel because of this).
	 */
	vbt = (const u8 *)base + OPREGION_VBT_OFFSET;
	if (mboxes & MBOX_ASLE_EXT) {
		vbt_size = OPREGION_ASLE_EXT_OFFSET;
	} else {
		vbt_size = OPREGION_SIZE;
	}
	vbt_size -= OPREGION_VBT_OFFSET;

	/* Keeps mailbox #4 when it holds a valid VBT. */
	valid = intel_bios_is_valid_vbt(vbt, vbt_size);
	if (valid) {
		I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm,
			"Found valid VBT in ACPI OpRegion (Mailbox #4)\n");
		opregion->vbt = vbt;
		opregion->vbt_size = vbt_size;
	} else {
		I915_OPREGION_DRM_DBG_KMS(&dev_priv->drm,
			"Invalid VBT in ACPI OpRegion (Mailbox #4)\n");
	}
}

/* Registers the video event callback and resumes the OpRegion (intel_opregion_register()). */
static void
i915_opregion_register(
	struct drm_i915_private *i915)
{
	struct intel_opregion *opregion;

	/* Nothing without a set-up OpRegion. */
	opregion = &i915->display.opregion;
	if (!opregion->header)
		return;

	/* Registers the video event callback when there are public ACPI methods. */
	if (opregion->acpi) {
		opregion->acpi_notifier.notifier_call =
			i915_opregion_video_event;
		(void)register_acpi_notifier(&opregion->acpi_notifier);
	}

	/* Tells the firmware the driver is ready. */
	i915_opregion_resume(i915);
}

/* Publishes the device lists and the driver readiness to the firmware (intel_opregion_resume_display()). */
static void
i915_opregion_resume_display(
	struct drm_i915_private *i915)
{
	struct intel_opregion *opregion;

	/* The display lists and the ACPI readiness. */
	opregion = &i915->display.opregion;
	if (opregion->acpi) {
		i915_didl_outputs(i915);
		i915_setup_cadls(i915);

		/*
		 * Notify BIOS we are ready to handle ACPI video ext notifs.
		 * Right now, all the events are handled by the ACPI video
		 * module. We don't actually need to do anything with them.
		 */
		opregion->acpi->csts = 0;
		opregion->acpi->drdy = 1;
	}

	/* The backlight control and the ASLE readiness. */
	if (opregion->asle) {
		opregion->asle->tche = ASLE_TCHE_BLC_EN;
		opregion->asle->ardy = ASLE_ARDY_READY;
	}

	/* Some platforms abuse the _DSM to enable MUX */
	intel_dsm_get_bios_data_funcs_supported(i915);
}

/* Resumes the OpRegion: the display part and the adapter power state D0 (intel_opregion_resume()). */
static void
i915_opregion_resume(
	struct drm_i915_private *i915)
{
	struct intel_opregion *opregion;

	/* Nothing without a set-up OpRegion. */
	opregion = &i915->display.opregion;
	if (!opregion->header)
		return;

	/* The display part. */
	if (HAS_DISPLAY(i915))
		i915_opregion_resume_display(i915);

	/* Tells the firmware the adapter is in D0; a refusal is not an error here. */
	(void)i915_opregion_notify_adapter(i915, PCI_D0);
}

/* Withdraws the driver readiness and stops the ASLE work (intel_opregion_suspend_display()). */
static void
i915_opregion_suspend_display(
	struct drm_i915_private *i915)
{
	struct intel_opregion *opregion;

	/* The driver is no longer ready for ASLE requests. */
	opregion = &i915->display.opregion;
	if (opregion->asle)
		opregion->asle->ardy = ASLE_ARDY_NOT_READY;

	/* Waits until the ASLE work is not running. */
	(void)drv_i915_opregion_cancel_work_sync(&i915->display.opregion.asle_work);

	/* The driver is no longer ready for ACPI notifications. */
	if (opregion->acpi)
		opregion->acpi->drdy = 0;
}

/* Suspends the OpRegion: the adapter power state, then the display part (intel_opregion_suspend()). */
static void
i915_opregion_suspend(
	struct drm_i915_private *i915,
	pci_power_t state)
{
	struct intel_opregion *opregion;

	/* Nothing without a set-up OpRegion. */
	opregion = &i915->display.opregion;
	if (!opregion->header)
		return;

	/* Tells the firmware the adapter's state; a refusal is not an error here. */
	(void)i915_opregion_notify_adapter(i915, state);

	/* The display part. */
	if (HAS_DISPLAY(i915))
		i915_opregion_suspend_display(i915);
}

/* Suspends the OpRegion to D1 and unregisters the video event callback (intel_opregion_unregister()). */
static void
i915_opregion_unregister(
	struct drm_i915_private *i915)
{
	struct intel_opregion *opregion;

	/* Suspends first. */
	opregion = &i915->display.opregion;
	i915_opregion_suspend(i915, PCI_D1);

	/* Nothing more without a set-up OpRegion. */
	if (!opregion->header)
		return;

	/* Unregisters the callback, and forgets it. */
	if (opregion->acpi_notifier.notifier_call) {
		(void)unregister_acpi_notifier(&opregion->acpi_notifier);
		opregion->acpi_notifier.notifier_call = NULL;
	}
}

/* Unmaps the OpRegion and clears every pointer into it (intel_opregion_cleanup()). */
static void
i915_opregion_cleanup(
	struct drm_i915_private *i915)
{
	struct intel_opregion *opregion;

	/* Nothing without a set-up OpRegion. */
	opregion = &i915->display.opregion;
	if (!opregion->header)
		return;

	/* just clear all opregion memory pointers now */
	memunmap(opregion->header);
	if (opregion->rvda) {
		memunmap(opregion->rvda);
		opregion->rvda = NULL;
	}
	if (opregion->vbt_firmware) {
		i915_opregion_kfree(opregion->vbt_firmware);
		opregion->vbt_firmware = NULL;
	}

	/* Forgets every mailbox. */
	opregion->header = NULL;
	opregion->acpi = NULL;
	opregion->swsci = NULL;
	opregion->asle = NULL;
	opregion->asle_ext = NULL;
	opregion->vbt = NULL;
	opregion->lid_state = NULL;
}

/* Returns the ACPI display type a connector type belongs to. */
static u32
i915_acpi_display_type(
	struct intel_connector *connector)
{
	/*
	 * Which ACPI display type each DRM connector type reports.  The
	 * classification agrees with Linux intel_acpi.c; the table never changes.
	 */
	static const struct {
		int connector_type;
		u32 display_type;
	} classes[] = {
		{ DRM_MODE_CONNECTOR_VGA, I915_ACPI_DISPLAY_TYPE_VGA },
		{ DRM_MODE_CONNECTOR_DVIA, I915_ACPI_DISPLAY_TYPE_VGA },
		{ DRM_MODE_CONNECTOR_Composite, I915_ACPI_DISPLAY_TYPE_TV },
		{ DRM_MODE_CONNECTOR_SVIDEO, I915_ACPI_DISPLAY_TYPE_TV },
		{ DRM_MODE_CONNECTOR_Component, I915_ACPI_DISPLAY_TYPE_TV },
		{ DRM_MODE_CONNECTOR_9PinDIN, I915_ACPI_DISPLAY_TYPE_TV },
		{ DRM_MODE_CONNECTOR_TV, I915_ACPI_DISPLAY_TYPE_TV },
		{ DRM_MODE_CONNECTOR_DVII, I915_ACPI_DISPLAY_TYPE_EXTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_DVID, I915_ACPI_DISPLAY_TYPE_EXTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_DisplayPort, I915_ACPI_DISPLAY_TYPE_EXTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_HDMIA, I915_ACPI_DISPLAY_TYPE_EXTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_HDMIB, I915_ACPI_DISPLAY_TYPE_EXTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_LVDS, I915_ACPI_DISPLAY_TYPE_INTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_eDP, I915_ACPI_DISPLAY_TYPE_INTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_DSI, I915_ACPI_DISPLAY_TYPE_INTERNAL_DIGITAL },
		{ DRM_MODE_CONNECTOR_Unknown, I915_ACPI_DISPLAY_TYPE_OTHER },
		{ DRM_MODE_CONNECTOR_VIRTUAL, I915_ACPI_DISPLAY_TYPE_OTHER }
	};
	unsigned index;

	/* Looks the connector type up in the classification. */
	for (index = 0U; index < sizeof(classes) / sizeof(classes[0]); index++) {
		if (classes[index].connector_type == connector->base.connector_type)
			return classes[index].display_type;
	}

	/* A connector type the classification does not know is reported and counted as other. */
	i915_opregion_missing_case(connector->base.connector_type);
	return I915_ACPI_DISPLAY_TYPE_OTHER;
}
