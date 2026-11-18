/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The \_OSI method: what firmware learns about the operating system.
 *
 * PC firmware enables features by the Windows release it is told it runs
 * on, so zedBSD answers true for the Windows releases up to the one named
 * last in the table, and for the feature strings it implements.  Optional
 * features (Module Device, Processor Device, 3.0 Thermal Model, 3.0 _SCP
 * Extensions, Processor Aggregator Device) are claimed only once a driver
 * handles them.  The table is the one place to change that answer.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "aml-internal.h"
#include "aml-os.h"

/*
 * The strings \_OSI answers true for.
 */
static const char *const osi_supported[] = {
	"Windows 2000",
	"Windows 2001",
	"Windows 2001 SP1",
	"Windows 2001.1",
	"Windows 2001 SP2",
	"Windows 2001.1 SP1",
	"Windows 2006",
	"Windows 2006.1",
	"Windows 2006 SP1",
	"Windows 2006 SP2",
	"Windows 2009",
	"Windows 2012",
	"Windows 2013",
	"Windows 2015",
	"Windows 2016",
	"Windows 2017",
	"Windows 2017.2",
	"Windows 2018",
	"Windows 2018.2",
	"Windows 2019",
	"Windows 2020",
	"Windows 2021",
	"Windows 2022",
	"Extended Address Space Descriptor",
};

static int osi_method(struct drv_acpi_eval *eval, struct drv_acpi_object **arguments, unsigned argument_count, struct drv_acpi_object **result);

/*
 * Creates \_OSI as a native method of one argument.
 */
int
drv_acpi_osi_install(void)
{
	struct drv_acpi_object *method;
	struct drv_acpi_node *root;
	struct drv_acpi_node *node;
	uint32_t segment;
	int error;

	/* Creates the node below the root. */
	root = drv_acpi_root();
	segment = drv_acpi_ns_segment((const uint8_t *)"_OSI");
	error = drv_acpi_ns_create_child(root, segment, DRV_ACPI_OWNER_PREDEFINED, &node);
	if (error != 0)
		return error;

	/* Allocates the method. */
	method = drv_acpi_object_new(DRV_ACPI_TYPE_METHOD);
	if (method == NULL)
		return ENOMEM;

	/* Makes it a method of one argument that runs in C. */
	method->value.method.native = osi_method;
	method->value.method.argument_count = 1;

	/* The node takes its own reference to the method; this one goes. */
	drv_acpi_ns_attach(node, method);
	drv_acpi_object_release(method);

	/* Succeeded: firmware can call \_OSI. */
	return 0;
}

/* Answers whether the operating system supports the string it is given. */
static int
osi_method(
	struct drv_acpi_eval *eval,
	struct drv_acpi_object **arguments,
	unsigned argument_count,
	struct drv_acpi_object **result)
{
	struct drv_acpi_object *answer;
	const char *text;
	size_t index;
	int compared;
	bool supported;

	UNUSED_PARAMETER(eval);

	/* Refuses a call without a string. */
	if (argument_count != 1 || arguments[0] == NULL)
		return EINVAL;
	if (arguments[0]->type != DRV_ACPI_TYPE_STRING)
		return EINVAL;

	/* Takes the string firmware asks about. */
	text = arguments[0]->value.string.text;

	/* Looks the string up. */
	supported = false;
	for (index = 0; index < sizeof(osi_supported) / sizeof(osi_supported[0]); index++) {
		/* Stops at the matching entry. */
		compared = kern_strcmp(osi_supported[index], text);
		if (compared == 0) {
			supported = true;
			break;
		}
	}

	/* Makes the answer. */
	answer = drv_acpi_object_integer_new(0);
	if (answer == NULL)
		return ENOMEM;

	/* Answers Ones for a supported string and zero otherwise. */
	if (supported)
		answer->value.integer = drv_acpi_integer_mask();

	/* Hands over the answer. */
	*result = answer;

	/* Succeeded: result says whether the string is supported. */
	return 0;
}
