/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/acpi: the ACPI namespace and evaluations as text for user programs
 * (the diagnostic interface proposed in the WS049 design, section 7).
 *
 * Reading an open file gives the namespace, one line per node.  Writing a
 * path (such as "\\_SB.PCI0._CRS") evaluates it, and the reads that follow
 * give "PATH = VALUE".  The lines are the ones the host tests print, so a
 * namespace read in a guest compares with the host's line for line.
 */

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include <drivers/acpi/acpi.h>

#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/lock.h"

#include "acpi-text.h"
#include "aml-internal.h"

/*
 * The device number of /dev/acpi.
 */
#define ACPI_DEVICE_NUMBER	0x000b0000U

/*
 * The longest path a write may name.
 */
#define PATH_LENGTH_MAX		512U

/*
 * What one open of /dev/acpi reads: the text, how far it has been read,
 * and whether it was made yet.  The lock keeps two reads of the same file
 * from making it twice.
 */
struct acpi_open {
	struct mutex lock;
	struct drv_acpi_text text;
	size_t position;
	bool made;
};

static int acpi_open(struct file *file);
static int acpi_close(struct file *file);
static ssize_t acpi_read(struct file *file, void *buffer, size_t size);
static ssize_t acpi_write(struct file *file, const void *buffer, size_t size);

/*
 * The operations of /dev/acpi.
 */
static const struct cdev_ops acpi_ops = {
	.open = acpi_open,
	.close = acpi_close,
	.read = acpi_read,
	.write = acpi_write,
};

/*
 * Publishes /dev/acpi.
 */
int
drv_acpi_device_register(void)
{
	int error;

	/* Registers the character device. */
	error = cdev_register("acpi", (dev_t)ACPI_DEVICE_NUMBER, &acpi_ops, NULL);
	if (error != 0)
		return error;

	/* Succeeded: user programs can open /dev/acpi. */
	return 0;
}

/* Starts an open with no text made yet. */
static int
acpi_open(
	struct file *file)
{
	struct acpi_open *state;
	int error;

	/* Allocates the open's state. */
	state = kern_malloc(sizeof(*state));
	if (state == NULL)
		return ENOMEM;

	/* Starts the state with nothing read and no text made. */
	kern_memset(state, 0, sizeof(*state));

	/* Prepares the lock that keeps two reads from making the text twice. */
	error = mutex_init(&state->lock, LOCK_RANK_DEVICE, "acpi device");
	if (error != 0) {
		kern_free(state);
		return error;
	}

	/* Starts with no text. */
	drv_acpi_text_init(&state->text);

	/* Succeeded: the file holds the state until it closes. */
	file->f_data = state;
	return 0;
}

/* Frees an open's text and state. */
static int
acpi_close(
	struct file *file)
{
	struct acpi_open *state;

	/* Frees the text and the state. */
	state = file->f_data;
	if (state != NULL) {
		drv_acpi_text_release(&state->text);
		kern_free(state);
	}

	/* Succeeded: the file holds nothing any more. */
	file->f_data = NULL;
	return 0;
}

/* Reads the open's text, making the namespace's first when nothing was written. */
static ssize_t
acpi_read(
	struct file *file,
	void *buffer,
	size_t size)
{
	struct acpi_open *state;
	size_t count;
	int error;

	/* Makes the namespace's text on the first read, and copies what is left of the text. */
	state = file->f_data;
	mutex_lock(&state->lock);

	if (!state->made) {
		error = drv_acpi_text_namespace(&state->text);
		if (error != 0) {
			/* Drops the part made, so that the next read starts the text afresh. */
			drv_acpi_text_release(&state->text);
			drv_acpi_text_init(&state->text);
			mutex_unlock(&state->lock);
			return -error;
		}

		/*
		 * made tells later reads that the text is there; they go on
		 * from position, which starts at the text's start.
		 */
		state->made = true;
		state->position = 0;
	}

	/* Takes what is left of the text, up to the caller's buffer. */
	count = state->text.length - state->position;
	if (count > size)
		count = size;

	/* Copies it and moves past it. */
	if (count != 0)
		kern_memcpy(buffer, state->text.data + state->position, count);
	state->position += count;

	mutex_unlock(&state->lock);

	/* Succeeded: zero bytes at the end of the text. */
	return (ssize_t)count;
}

/* Evaluates the path written; the reads that follow give the result. */
static ssize_t
acpi_write(
	struct file *file,
	const void *buffer,
	size_t size)
{
	struct acpi_open *state;
	char path[PATH_LENGTH_MAX];
	size_t length;

	/* Refuses a path too long to be one. */
	if (size == 0 || size >= sizeof(path))
		return -EINVAL;

	/* Copies the path. */
	kern_memcpy(path, buffer, size);

	/* Drops the line end and trailing blanks, and terminates the path. */
	length = size;
	while (length != 0 &&
	       (path[length - 1U] == '\n' ||
		path[length - 1U] == ' '))
		length--;
	path[length] = '\0';

	/*
	 * Replaces the open's text with the evaluation; made tells later reads
	 * that the text is there, and they start at its start.  A failed
	 * evaluation is itself the text the reads give.
	 */
	state = file->f_data;
	mutex_lock(&state->lock);

	drv_acpi_text_release(&state->text);
	drv_acpi_text_init(&state->text);
	(void)drv_acpi_text_evaluate(&state->text, NULL, path, path);
	state->made = true;
	state->position = 0;

	mutex_unlock(&state->lock);

	/* Succeeded: the whole write was taken. */
	return (ssize_t)size;
}
