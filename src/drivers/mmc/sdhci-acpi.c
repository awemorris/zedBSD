/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Probes and attaches SDHCI controllers discovered through ACPI namespace
 * enumeration, such as standard SD host controllers (PNP0D40) on Intel Atom
 * SoCs (Moorefield / Merrifield / Bay Trail).
 */

#include "drivers/mmc/sdhci-acpi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <drivers/acpi/acpi.h>
#include <drivers/mmc/sdhci.h>
#include <kern/disk.h>
#include <uapi/errno.h>
#include <kern/klog.h>
#include <kern/kcrt.h>
#include <kern/pmem.h>

/* PNP0D40 as a string and as the EISA identifier _HID and _CID may give instead. */
#define SDHCI_ACPI_ID_STRING		"PNP0D40"
#define SDHCI_ACPI_ID_EISA		0x400dd041U

/* _STA present bit */
#define ACPI_STATUS_PRESENT		0x01U

/* Default window size if not specified in CRS */
#define SDHCI_ACPI_DEFAULT_WINDOW	0x1000U

/* State maintained across the ACPI namespace walk */
struct sdhci_acpi_walk_state {
	unsigned unit_count;
	unsigned attached;
};

/* Resource info extracted from _CRS */
struct sdhci_acpi_resource_info {
	uint64_t base;
	uint64_t length;
	bool found;
};

static bool sdhci_acpi_probed;

/* Tells whether an identifier object is PNP0D40 (or compatible Intel SDHCI). */
static bool
sdhci_acpi_id_matches(
	const struct drv_acpi_object *object)
{
	enum drv_acpi_type type;
	const char *text;
	uint64_t value;
	size_t length;

	if (object == NULL)
		return false;

	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_INTEGER) {
		value = drv_acpi_object_integer(object);
		if (value == SDHCI_ACPI_ID_EISA)
			return true;
		return false;
	}

	if (type != DRV_ACPI_TYPE_STRING)
		return false;

	text = drv_acpi_object_string(object, &length);
	if (text == NULL)
		return false;

	if (kern_strcmp(text, SDHCI_ACPI_ID_STRING) == 0)
		return true;
	if (kern_strcmp(text, "INT33BB") == 0)
		return true;
	if (kern_strcmp(text, "INT3436") == 0)
		return true;
	if (kern_strcmp(text, "INT344D") == 0)
		return true;
	if (kern_strcmp(text, "80860F14") == 0)
		return true;
	if (kern_strcmp(text, "80860F16") == 0)
		return true;

	return false;
}

/* Tells whether a device's _HID or _CID names an SDHCI controller. */
static bool
sdhci_acpi_matches(
	struct drv_acpi_node *node)
{
	struct drv_acpi_object *object;
	struct drv_acpi_object *element;
	enum drv_acpi_type type;
	unsigned count;
	unsigned index;
	bool matched;
	int error;

	/* The hardware identifier first. */
	object = NULL;
	error = drv_acpi_evaluate(node, "_HID", NULL, 0U, &object);
	if (error == 0 && object != NULL) {
		matched = sdhci_acpi_id_matches(object);
		drv_acpi_object_release(object);
		if (matched)
			return true;
	}

	/* Then the compatible identifiers: one, or a package of them. */
	object = NULL;
	error = drv_acpi_evaluate(node, "_CID", NULL, 0U, &object);
	if (error != 0 || object == NULL)
		return false;

	matched = false;
	type = drv_acpi_object_type(object);
	if (type == DRV_ACPI_TYPE_PACKAGE) {
		count = drv_acpi_object_package_count(object);
		for (index = 0; index < count; index++) {
			element = drv_acpi_object_package_element(object, index);
			matched = sdhci_acpi_id_matches(element);
			if (matched)
				break;
		}
	} else {
		matched = sdhci_acpi_id_matches(object);
	}

	drv_acpi_object_release(object);
	return matched;
}

/* Tells whether a device is present: its _STA says so, or it has none. */
static bool
sdhci_acpi_present(
	struct drv_acpi_node *node)
{
	uint64_t status;
	int error;

	error = drv_acpi_evaluate_integer(node, "_STA", &status);
	if (error != 0)
		return true;

	return (status & ACPI_STATUS_PRESENT) != 0U;
}

/* Resource visitor for _CRS */
static int
sdhci_acpi_resource_visitor(
	const struct drv_acpi_resource *resource,
	void *argument)
{
	struct sdhci_acpi_resource_info *info = argument;

	if (resource->kind != DRV_ACPI_RESOURCE_MEMORY)
		return 0;

	if (resource->length == 0)
		return 0;

	if (!info->found) {
		info->base = resource->base;
		info->length = resource->length;
		info->found = true;
	}

	return 0;
}

/* ACPI namespace walk visitor */
static int
sdhci_acpi_visitor(
	struct drv_acpi_node *node,
	unsigned depth,
	void *argument)
{
	struct sdhci_acpi_walk_state *state;
	struct sdhci_acpi_resource_info res_info;
	struct drv_sdhci_config config;
	struct disk *disk;
	char path[64];
	void *registers;
	int error;

	(void)depth;
	state = argument;

	if (drv_acpi_node_type(node) != DRV_ACPI_TYPE_DEVICE)
		return 0;

	if (!sdhci_acpi_matches(node))
		return 0;

	if (!sdhci_acpi_present(node))
		return 0;

	if (state->unit_count >= DRV_SDHCI_UNIT_MAX)
		return -1;

	error = drv_acpi_node_path(node, path, sizeof(path));
	if (error != 0)
		(void)kern_snprintf(path, sizeof(path), "(sdhci-acpi)");

	res_info.base = 0;
	res_info.length = 0;
	res_info.found = false;

	error = drv_acpi_resources_walk(node, NULL, sdhci_acpi_resource_visitor, &res_info);
	if (error != 0 || !res_info.found) {
		kern_logf("sdhci-acpi: %s has no memory resource in _CRS\n", path);
		return 1;
	}

	if (res_info.length == 0)
		res_info.length = SDHCI_ACPI_DEFAULT_WINDOW;

	error = kern_device_map(res_info.base, res_info.length, KERN_DEVICE_UNCACHED, &registers);
	if (error != 0) {
		kern_logf("sdhci-acpi: failed to map MMIO 0x%llx for %s (error %d)\n",
			  (unsigned long long)res_info.base, path, error);
		return 1;
	}

	config.registers = registers;
	config.physical_base = res_info.base;
	config.base_clock_hz = 0;
	config.max_clock_hz = 0;
	config.quirks = DRV_SDHCI_QUIRK_FORCE_CD;
	config.card_present = NULL;
	config.context = NULL;
	config.unit = state->unit_count;

	disk = NULL;
	error = drv_sdhci_attach(&config, &disk);
	if (error != 0) {
		kern_logf("sdhci-acpi: attach failed at 0x%llx for %s (error %d)\n",
			  (unsigned long long)res_info.base, path, error);
		(void)kern_device_unmap(registers, res_info.length);
		return 1;
	}

	state->unit_count++;
	state->attached++;

	kern_logf("sdhci-acpi: %s at 0x%llx attached as %s\n",
		  path, (unsigned long long)res_info.base, disk->d_name);

	/* Return 1 to skip children of this controller device */
	return 1;
}

/*
 * Probes the ACPI namespace for SD host controllers and attaches them.
 * Returns the number of successfully attached controllers.
 */
int
drv_sdhci_acpi_probe(
	void)
{
	struct sdhci_acpi_walk_state state;
	int error;

	if (sdhci_acpi_probed)
		return 0;
	sdhci_acpi_probed = true;

	state.unit_count = 0;
	state.attached = 0;

	error = drv_acpi_walk(NULL, sdhci_acpi_visitor, &state);
	if (error < 0 && state.attached == 0)
		return error;

	return (int)state.attached;
}
