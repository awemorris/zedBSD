/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The OpRegion environment: what the Linux OpRegion and ACPI text needs.
 *
 * The mailbox layout, struct intel_opregion and the functions are the Linux
 * v6.8.12 text of intel_opregion.c and intel_acpi.c; this header maps the
 * Linux types and the ACPI notifier interface onto zedBSD (the world-neutral
 * notifier types are in internal.h).  The OpRegion service's backend is
 * chosen when the instance is bound: SHADOW (driver-owned RAM in the OpRegion
 * format, for synthetic tests), or FIRMWARE (the real shared region), which
 * production does not enable: it runs VBT_ONLY.
 *
 * Every piece of state this environment keeps is in struct
 * i915_opregion_world.  The device the Linux text passes around, struct
 * drm_i915_private, is the world's i915_opregion_dev member, so a helper
 * that is handed the device, its ASLE work or its connection lock finds the
 * world without a global.
 *
 * Errors: functions return zedBSD positive errno at their boundaries.  The
 * Linux text returns zedBSD errno numbers here, except the Linux-internal
 * ENOTSUPP, which intel_opregion_setup() returns for a device without an
 * OpRegion; I915_OPREGION_ENOTSUPP carries that Linux number.
 *
 * A translation unit includes this header or another Linux environment,
 * never two (see internal.h).
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_OPREGION_INTERNAL_H
#define DRIVERS_GPU_I915_DISPLAY_OPREGION_INTERNAL_H

#include "internal.h"

#ifdef I915_DISPLAY_LINUX_WORLD
#error "opregion-internal.h cannot be included together with another Linux display environment"
#endif
#define I915_DISPLAY_LINUX_WORLD "opregion"

/* This translation unit is compiled in the OpRegion environment. */
#define I915_DISPLAY_WORLD_OPREGION 1

#include <uapi/errno.h>
#include <kern/klog.h>
#include <kern/lock.h>

/* The Linux-internal errno the OpRegion setup returns when there is no OpRegion. */
#define I915_OPREGION_ENOTSUPP 524

/* Marks a parameter a helper does not read (as i915.h spells it). */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(parameter) ((void)(parameter))
#endif

/* A structure laid out without padding, as the firmware mailboxes are. */
#ifndef __packed
#define __packed __attribute__((packed))
#endif

/* The ACPI device class of the video events (acpi/video.h). */
#define ACPI_VIDEO_CLASS "video"

/*
 * Diagnostics of the Linux text.
 *
 * Debug output is kept silent (the service logs its own records); errors
 * and information are logged.  The device argument is not evaluated.
 */
#define I915_OPREGION_DRM_DBG(dev, ...) ((void)(dev))
#define I915_OPREGION_DRM_DBG_KMS(dev, ...) ((void)(dev))
#define I915_OPREGION_DRM_DEBUG_KMS(fmt, ...) ((void)0)
#define I915_OPREGION_DRM_ERR(dev, fmt, ...) kern_logf("i915: opregion (drm_err) " fmt, ##__VA_ARGS__)
#define DRM_INFO(fmt, ...) kern_logf("i915: opregion (DRM_INFO) " fmt, ##__VA_ARGS__)
#define DRM_INFO_ONCE(fmt, ...) kern_logf("i915: opregion (DRM_INFO_ONCE) " fmt, ##__VA_ARGS__)

/* Linux WARN_ON() and drm_WARN_ON(): logs the condition text and reports the condition. */
#define I915_OPREGION_WARN_ON(x) drv_i915_opregion_warn_on(!!(x), #x)
#define I915_OPREGION_DRM_WARN_ON(dev, x) drv_i915_opregion_warn_on(!!(x), #x)

/* Fails the build when the condition holds (the Linux BUILD_BUG_ON()). */
#define BUILD_BUG_ON(c) _Static_assert(!(c), "BUILD_BUG_ON")

/* The backlight policy (the Linux acpi_video_get_backlight_type()), an input of the service. */
#define acpi_video_get_backlight_type() ((enum acpi_backlight_type)drv_i915_opregion_backlight_policy())

/* Releases the connection mutex taken by i915_opregion_drm_modeset_lock(). */
#define drm_modeset_unlock(l) drv_i915_opregion_connection_unlock(i915_opregion_world_of_lock(l))

/* Hands an ASLE backlight request to the connector's registered target. */
#define intel_backlight_set_acpi(st, level, max) drv_i915_opregion_backlight_set_acpi((st), (level), (max))

/* The ACPI notifier chain of the OpRegion service (internal.h). */
#define register_acpi_notifier(nb) i915_register_acpi_notifier(nb)
#define unregister_acpi_notifier(nb) i915_unregister_acpi_notifier(nb)

/* The platform: the display is present. */
#define HAS_DISPLAY(i915) (1)

/* PCI power states (include/linux/pci.h). */
#define PCI_D0 0
#define PCI_D1 1
#define PCI_D2 2
#define PCI_D3hot 3
#define PCI_D3cold 4

/* DRM_MODE_CONNECTOR_* (include/uapi/drm/drm_mode.h, the UAPI values). */
#define DRM_MODE_CONNECTOR_Unknown 0
#define DRM_MODE_CONNECTOR_VGA 1
#define DRM_MODE_CONNECTOR_DVII 2
#define DRM_MODE_CONNECTOR_DVID 3
#define DRM_MODE_CONNECTOR_DVIA 4
#define DRM_MODE_CONNECTOR_Composite 5
#define DRM_MODE_CONNECTOR_SVIDEO 6
#define DRM_MODE_CONNECTOR_LVDS 7
#define DRM_MODE_CONNECTOR_Component 8
#define DRM_MODE_CONNECTOR_9PinDIN 9
#define DRM_MODE_CONNECTOR_DisplayPort 10
#define DRM_MODE_CONNECTOR_HDMIA 11
#define DRM_MODE_CONNECTOR_HDMIB 12
#define DRM_MODE_CONNECTOR_TV 13
#define DRM_MODE_CONNECTOR_eDP 14
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_CONNECTOR_DSI 16

/*
 * The PCI configuration space of the service instance.
 *
 * ASLS reads the instance's token; SWSCI configuration access is not
 * provided (the target has no SWSCI mailbox), and any access is recorded as
 * an error, never faked.
 */
#define to_pci_dev(d) ((struct pci_dev *)(d))
#define pci_read_config_dword(pdev, where, val) drv_i915_opregion_pci_read32((pdev), (where), (val))
#define pci_read_config_word(pdev, where, val) (*(val) = 0, drv_i915_opregion_pci_access_unported("pci_read_config_word"))
#define pci_write_config_word(pdev, where, val) drv_i915_opregion_pci_access_unported("pci_write_config_word")
#define pci_write_config_dword(pdev, where, val) drv_i915_opregion_pci_access_unported("pci_write_config_dword")

/*
 * Linux msleep() in the SWSCI polling: not reachable without the SWSCI
 * mailbox; reaching it is recorded.  The duration is not evaluated.
 */
#define I915_OPREGION_MSLEEP(ms) ((void)drv_i915_opregion_pci_access_unported("msleep (SWSCI polling)"))

/*
 * Linux wait_for() in the SWSCI polling: recorded, and a timeout.  Neither
 * the condition nor the duration is evaluated.
 */
#define I915_OPREGION_WAIT_FOR(COND, MS) \
	(drv_i915_opregion_pci_access_unported("wait_for (SWSCI completion polling)"), -ETIMEDOUT)

/*
 * memremap() and memunmap() through the instance's mapping table (SHADOW:
 * driver-owned RAM; FIRMWARE: the HAL mappings of the real region).
 */
#define MEMREMAP_WB 1
#define memremap(p, s, f) drv_i915_opregion_memremap((p), (s), (f))
#define memunmap(p) drv_i915_opregion_memunmap(p)

/*
 * VBT firmware.
 *
 * display.params.vbt_firmware is NULL here, so Linux returns before these;
 * request_firmware() would answer -ENOENT.
 */
#define request_firmware(fw, name, dev) (-ENOENT)
#define release_firmware(fw) ((void)(fw))
#define GFP_KERNEL 0

/*
 * Linux kmemdup(): never reached (see request_firmware()); reports NULL
 * without evaluating its arguments.
 */
#define I915_OPREGION_KMEMDUP(p, n, gfp) ((void *)0)

/* Validates a VBT (the Linux intel_bios_is_valid_vbt()) with the VBT parser. */
#define intel_bios_is_valid_vbt(b, s) (drv_i915_vbt_validate((b), (s)) != 0)

/* DMI: zedBSD has no DMI data source, so dmi_check_system() matches nothing (the table is kept). */
#define DMI_MATCH(a, b) { .slot = a, .substr = b }
#define dmi_check_system(list) drv_i915_opregion_dmi_check_system(list)

/* ACPI _DSM and the SWSCI mailbox setup: no AML in zedBSD, no SWSCI mailbox on the target; recorded boundaries. */
#define intel_dsm_get_bios_data_funcs_supported(i915) \
	drv_i915_opregion_boundary("intel_dsm_get_bios_data_funcs_supported: ACPI _DSM (no AML)")
#define swsci_setup(i915) \
	drv_i915_opregion_boundary("swsci_setup: the SWSCI mailbox (v2.x) is not ported -- absent on the target")

/*
 * The backlight types acpi_video_get_backlight_type() reports
 * (acpi/video.h).
 */
enum acpi_backlight_type {
	acpi_backlight_undef = -1,
	acpi_backlight_none = 0,
	acpi_backlight_video,
	acpi_backlight_vendor,
	acpi_backlight_native
};

/*
 * The DMI fields a match may name (linux/mod_devicetable.h).
 */
enum dmi_field {
	DMI_NONE,
	DMI_SYS_VENDOR,
	DMI_PRODUCT_NAME,
	DMI_PRODUCT_VERSION,
	DMI_BOARD_VENDOR,
	DMI_BOARD_NAME
};

/* A PCI power state (include/linux/pci.h). */
typedef int pci_power_t;

/* A physical address or size of a resource. */
typedef uint64_t resource_size_t;

struct drm_modeset_acquire_ctx;
struct pci_dev;
struct opregion_header;
struct opregion_acpi;
struct opregion_swsci;
struct opregion_asle;
struct opregion_asle_ext;

/*
 * How the ASLE work has been queued and run, for the service's records.
 *
 * It is a member of struct i915_opregion_world.
 */
struct i915_opregion_worker_stats {
	unsigned started;
	unsigned finished;
	unsigned queued_new;
	unsigned queued_pending;
};

/*
 * A Linux work item.
 *
 * The only one of this environment is the ASLE work of the world's device;
 * the work queue calls i915_opregion_work_trampoline() with it, which runs
 * func.
 */
struct work_struct {
	struct i915_work kwork;
	void (*func)(struct work_struct *work);
};

/*
 * A modeset lock; the connection mutex of the world stands behind it.
 */
struct drm_modeset_lock {
	int unused;
};

/*
 * The drm device: the connection mutex asle_set_backlight() takes, and the
 * device pointer the PCI accessors are handed.
 */
struct drm_device {
	struct {
		struct drm_modeset_lock connection_mutex;
	} mode_config;
	void *dev;
};

/*
 * A connector's state: the index of its registered backlight target.
 */
struct drm_connector_state {
	unsigned target;
};

/*
 * A drm connector: its state and its type.
 */
struct drm_connector {
	const struct drm_connector_state *state;
	int connector_type;
};

/*
 * An i915 connector: the drm connector and the ACPI device id intel_acpi.c
 * gives it.
 *
 * The world keeps one per registered backlight target.
 */
struct intel_connector {
	struct drm_connector base;
	u32 acpi_device_id;
};

/*
 * The connector walk of the Linux text: the next index into the world's
 * backlight targets.
 */
struct drm_connector_list_iter {
	unsigned idx;
};

#include "../intel/opregion.h"

/*
 * The device the OpRegion text works on: only the OpRegion state, the VBT
 * firmware parameter and the work queue of the ASLE work.
 *
 * It is the i915_opregion_dev member of struct i915_opregion_world.
 */
struct drm_i915_private {
	struct drm_device drm;
	struct {
		struct intel_opregion opregion;
		struct {
			const char *vbt_firmware;
		} params;
	} display;
	struct i915_workqueue *unordered_wq;
};

/*
 * A loaded firmware image (include/linux/firmware.h).
 */
struct firmware {
	size_t size;
	const u8 *data;
};

/*
 * One DMI match of a system id (linux/mod_devicetable.h).
 */
struct dmi_strmatch {
	unsigned char slot;
	char substr[79];
};

/*
 * One system a DMI quirk applies to (linux/mod_devicetable.h).
 */
struct dmi_system_id {
	int (*callback)(const struct dmi_system_id *);
	const char *ident;
	struct dmi_strmatch matches[4];
	void *driver_data;
};

/*
 * One region the mapping table resolves memremap() into.
 *
 * SHADOW: memory the test owns; FIRMWARE: a HAL mapping of the real region.
 */
struct i915_opregion_map {
	resource_size_t phys;
	void *ptr;
	size_t size;
};

/*
 * One HAL device mapping the FIRMWARE backend made, which the cleanup unmaps.
 */
struct i915_opregion_hal_map {
	void *va;
	size_t size;
};

/*
 * One registered backlight target: the connector asle_set_backlight() walks
 * and the hook it hands the request to.
 */
struct i915_opregion_backlight_target {
	void (*set_acpi)(void *ctx, uint32_t level, uint32_t max);
	void *ctx;
	struct intel_connector conn;
	struct drm_connector_state state;
};

/*
 * Everything the OpRegion environment keeps.
 *
 * The display root holds a pointer to it; its owner allocates it zeroed and
 * then sets the two fields that start nonzero:
 *
 *	world->i915_opregion_backend = "NONE";
 *	world->i915_opregion_policy = acpi_backlight_vendor;
 *
 * Unless a field says otherwise it is written by the service's lifecycle
 * calls (setup, register, unregister, cleanup), which the probe thread makes
 * one at a time.
 */
struct i915_opregion_world {
	/*
	 * The device the Linux text works on.  Its OpRegion state is cleared
	 * by every setup; unordered_wq is set by the service start.  The ASLE
	 * work reads it on the worker.
	 */
	struct drm_i915_private i915_opregion_dev;

	/* The backend name: "NONE", "SHADOW" or "FIRMWARE"; "NONE" until a setup succeeds and after cleanup. */
	const char *i915_opregion_backend;

	/* What pci_read_config_dword(ASLS) reports; set by every setup. */
	uint32_t i915_opregion_asls;

	/* How many setups have been attempted; it only increases. */
	unsigned i915_opregion_epoch;

	/* The mapping table memremap() resolves into; emptied by cleanup and by a FIRMWARE setup. */
	struct i915_opregion_map i915_opregion_maps[4];
	unsigned i915_opregion_nmaps;

	/* SWSCI accesses reached (recorded errors), boundaries reached, unmaps done. */
	unsigned i915_opregion_unported;
	unsigned i915_opregion_boundaries;
	unsigned i915_opregion_unmaps;

	/*
	 * The producer gate: GSE requests are taken only while nonzero.  Set
	 * by register once an ASLE mailbox exists, cleared by unregister; read
	 * by the GSE entry.
	 */
	int i915_opregion_accepting;

	/* GSE requests dropped by the gate, and cleanups refused because the ASLE work was not idle. */
	unsigned i915_opregion_dropped;
	unsigned i915_opregion_cleanup_refused;

	/* The FIRMWARE backend's HAL mappings, unmapped by cleanup or a failed setup. */
	struct i915_opregion_hal_map i915_opregion_hal[2];
	unsigned i915_opregion_nhal;

	/*
	 * How the ASLE work was queued and run.  Cleared by the service
	 * start; moved by the queueing thread and by the worker without a
	 * lock.
	 */
	struct i915_opregion_worker_stats i915_opregion_wstats;

	/* The backlight policy acpi_video_get_backlight_type() reports; set by the service start and on request. */
	int i915_opregion_policy;

	/*
	 * The connection mutex of the Linux text: asle_set_backlight() holds it
	 * while it walks the backlight targets.  Initialised once, by the first
	 * service start.
	 */
	struct mutex i915_opregion_conn_lock;
	int i915_opregion_conn_lock_live;

	/*
	 * The registered backlight targets; the first i915_opregion_nbl are
	 * in use.  Emptied by the service start, added to before register.
	 */
	struct i915_opregion_backlight_target i915_opregion_bl[4];
	unsigned i915_opregion_nbl;
};

/*
 * The world's functions the Linux text reaches (the owner defines them).
 */
int drv_i915_opregion_warn_on(int cond, const char *what);
int drv_i915_opregion_backlight_policy(void);
void drv_i915_opregion_connection_lock(struct i915_opregion_world *world);
void drv_i915_opregion_connection_unlock(struct i915_opregion_world *world);
struct intel_connector *drv_i915_opregion_connector_next(struct i915_opregion_world *world, struct drm_connector_list_iter *it);
void drv_i915_opregion_backlight_set_acpi(const struct drm_connector_state *st, u32 level, u32 max);
int drv_i915_opregion_pci_read32(struct pci_dev *pdev, int where, u32 *val);
int drv_i915_opregion_pci_access_unported(const char *what);
void *drv_i915_opregion_memremap(resource_size_t phys, size_t size, unsigned long flags);
void drv_i915_opregion_memunmap(void *p);
int drv_i915_opregion_dmi_check_system(const struct dmi_system_id *list);
void drv_i915_opregion_boundary(const char *what);
int drv_i915_opregion_cancel_work_sync(struct work_struct *w);

/* Validates a VBT with the VBT parser; nonzero when valid. */
int drv_i915_vbt_validate(const void *bytes, size_t size);

/* The two functions of intel_opregion.c and intel_acpi.c other files call, under the names opregion.c exports them by. */
int drv_i915_opregion_setup(struct drm_i915_private *dev_priv);
void drv_i915_acpi_device_id_update(struct drm_i915_private *dev_priv);

/* The world the device of the Linux text belongs to. */
static __inline struct i915_opregion_world *
i915_opregion_world_of(
	struct drm_i915_private *i915)
{
	/* The device is the world's i915_opregion_dev member. */
	return container_of(i915, struct i915_opregion_world, i915_opregion_dev);
}

/* The world a connection mutex of the Linux text belongs to. */
static __inline struct i915_opregion_world *
i915_opregion_world_of_lock(
	struct drm_modeset_lock *lock)
{
	struct drm_i915_private *i915;

	/* The lock is the connection mutex of the device's drm device. */
	i915 = container_of(lock, struct drm_i915_private, drm.mode_config.connection_mutex);

	/* Succeeded: reports the world of that device. */
	return i915_opregion_world_of(i915);
}

/*
 * The world a work item belongs to.
 *
 * The ASLE work of the world's device is the only work of this environment
 * (intel_opregion_setup() prepares it), so the item is that member.
 */
static __inline struct i915_opregion_world *
i915_opregion_world_of_work(
	struct work_struct *work)
{
	struct drm_i915_private *i915;

	/* The item is the ASLE work of the device's OpRegion state. */
	i915 = container_of(work, struct drm_i915_private, display.opregion.asle_work);

	/* Succeeded: reports the world of that device. */
	return i915_opregion_world_of(i915);
}

/* Runs a work item's Linux callback on the worker, counting the run. */
static __inline void
i915_opregion_work_trampoline(
	void *context)
{
	struct work_struct *work;
	struct i915_opregion_world *world;

	/* Resolves the item and the world whose counters it moves. */
	work = context;
	world = i915_opregion_world_of_work(work);

	/* Counts the start, runs the callback, counts the finish. */
	world->i915_opregion_wstats.started++;
	work->func(work);
	world->i915_opregion_wstats.finished++;
}

/* Prepares a work item on the trampoline (the Linux INIT_WORK()). */
static __inline void
i915_opregion_init_work(
	struct work_struct *work,
	void (*function)(struct work_struct *work))
{
	/* Records the Linux callback. */
	work->func = function;

	/* Prepares the driver work, whose trampoline runs the callback. */
	drv_i915_work_init(&work->kwork, i915_opregion_work_trampoline, work);
}

/*
 * Queues a work item (the Linux queue_work()).
 *
 * Without a queue nothing is queued.  Reports 1 when the item was newly
 * queued and 0 otherwise; an item already pending is counted, not a failure.
 */
static __inline int
i915_opregion_queue_work(
	struct i915_workqueue *queue,
	struct work_struct *work)
{
	struct i915_opregion_world *world;
	int queued;

	/* Resolves the world whose counters the request moves. */
	world = i915_opregion_world_of_work(work);

	/* Queues the driver work when the service has a queue. */
	queued = 0;
	if (queue != NULL)
		queued = drv_i915_queue_work(queue, &work->kwork);

	/* Counts a new queueing apart from a request for an item already pending. */
	if (queued == 1) {
		world->i915_opregion_wstats.queued_new++;
	} else {
		world->i915_opregion_wstats.queued_pending++;
	}

	/* Succeeded: reports whether the item was newly queued. */
	return queued;
}

/*
 * Takes the connection mutex (the Linux drm_modeset_lock()).
 *
 * The world's connection mutex stands behind the lock; never -EDEADLK.
 */
static __inline int
i915_opregion_drm_modeset_lock(
	struct drm_modeset_lock *lock,
	struct drm_modeset_acquire_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Takes the connection mutex of the world the lock belongs to. */
	drv_i915_opregion_connection_lock(i915_opregion_world_of_lock(lock));

	/* Succeeded: the lock is held. */
	return 0;
}

/* Starts a connector walk (the Linux drm_connector_list_iter_begin()). */
static __inline void
i915_opregion_drm_connector_list_iter_begin(
	struct drm_device *dev,
	struct drm_connector_list_iter *iter)
{
	UNUSED_PARAMETER(dev);

	/* The walk starts at the first registered backlight target. */
	iter->idx = 0u;
}

/* Ends a connector walk (the Linux drm_connector_list_iter_end()); nothing is held. */
static __inline void
i915_opregion_drm_connector_list_iter_end(
	struct drm_connector_list_iter *iter)
{
	UNUSED_PARAMETER(iter);
}

/*
 * Walks the world's backlight targets from where the iterator stands (the
 * Linux for_each_intel_connector_iter()).
 */
#define I915_OPREGION_FOR_EACH_INTEL_CONNECTOR_ITER(world, connector, iter) \
	while (((connector) = drv_i915_opregion_connector_next((world), (iter))) != NULL)

/* Tells whether the platform has DDI outputs (the Linux HAS_DDI()); it has. */
static __inline bool
i915_opregion_has_ddi(
	struct drm_i915_private *i915)
{
	UNUSED_PARAMETER(i915);

	/* Succeeded: every supported platform has DDI outputs. */
	return true;
}

/* Logs a switch value the Linux text did not expect (the Linux MISSING_CASE()). */
static __inline void
i915_opregion_missing_case(
	long value)
{
	/* Names the unexpected value. */
	kern_logf("i915: opregion MISSING_CASE %ld\n", value);
}

/* Frees memory the Linux text allocated (the Linux kfree()); nothing here is allocated. */
static __inline void
i915_opregion_kfree(
	const void *pointer)
{
	UNUSED_PARAMETER(pointer);
}

#endif
