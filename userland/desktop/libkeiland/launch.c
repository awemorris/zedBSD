/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The programs an application starts apart from itself (kl_launch): what
 * is asked is checked, the compositor is asked for an activation token
 * from the window the user works in, and the platform's file starts the
 * program with the token in its environment.
 */

#include "process.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* The variable, with its '=', a started program finds its activation token in. */
#define LAUNCH_TOKEN_VARIABLE	"XDG_ACTIVATION_TOKEN="

static int launch_options_valid(const struct kl_launch_options *options);
static int launch_environment(const char *const *changes, const char *token, const char ***made);

/*
 * Starts a program apart from the application.
 */
int
kl_launch(
	struct kl_app *app,
	const struct kl_launch_options *options)
{
	char variable[sizeof(LAUNCH_TOKEN_VARIABLE) + KL_ACTIVATION_TOKEN_MAX];
	char token[KL_ACTIVATION_TOKEN_MAX];
	const char **environment;
	struct wl_display *display;
	struct wl_surface *surface;
	int valid;
	int error;

	/* What is asked must make sense. */
	if (app == NULL || options == NULL)
		return EINVAL;
	valid = launch_options_valid(options);
	if (!valid)
		return EINVAL;

	/*
	 * Leave for the program's window to come to the front, asked from the
	 * window the user works in; a compositor that gives none leaves the
	 * program without.
	 */
	variable[0] = '\0';
	if (options->from != NULL) {
		display = kl_app_display(app);
		surface = kl_window_surface(options->from);
		error = kl_activation_token(display, surface, NULL, token, sizeof(token));
		if (error == 0)
			(void)snprintf(variable, sizeof(variable), "%s%s", LAUNCH_TOKEN_VARIABLE, token);
	}

	/* The program's environment's changes, the token's after the program's. */
	error = launch_environment(options->environment, variable, &environment);
	if (error != 0)
		return error;

	/* The program, started by the platform's file. */
	error = keiui_launch_start(options, environment);
	free(environment);
	if (error != 0)
		return error;

	/* Succeeded: the program is on its way. */
	return 0;
}

/*
 * Tells whether a launch's options make sense: exactly one of the
 * arguments (with a program) and the command line (not empty).
 */
static int
launch_options_valid(
	const struct kl_launch_options *options)
{
	/* Neither, or both. */
	if (options->argv == NULL && options->command == NULL)
		return 0;
	if (options->argv != NULL && options->command != NULL)
		return 0;

	/* Arguments without a program. */
	if (options->argv != NULL) {
		if (options->argv[0] == NULL || options->argv[0][0] == '\0')
			return 0;
	}

	/* An empty command line. */
	if (options->command != NULL && options->command[0] == '\0')
		return 0;

	/* They make sense. */
	return 1;
}

/*
 * Makes the list of a launch's environment's changes: the program's, then
 * the token's variable when there is one.  Returns 0 with the list
 * allocated (the strings are not copied), or ENOMEM.
 */
static int
launch_environment(
	const char *const *changes,
	const char *token,
	const char ***made)
{
	const char **list;
	size_t count;
	size_t index;

	/* How many the program asks for. */
	count = 0;
	while (changes != NULL && changes[count] != NULL)
		count++;

	/* The list, with room for the token's and the end. */
	list = calloc(count + 2U, sizeof(*list));
	if (list == NULL)
		return ENOMEM;

	/* The program's changes, then the token's. */
	for (index = 0; index < count; index++)
		list[index] = changes[index];
	if (token[0] != '\0')
		list[count] = token;

	/* Succeeded: the list ends with NULL. */
	*made = list;
	return 0;
}
