/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GitHub Codespaces on the launch screen (codespace.c): the arguments
 * that ask GitHub's gh for the codespaces and for one's ssh settings, the
 * list gh writes read into its codespaces, the Host its settings name,
 * and the ProxyCommand that reaches a codespace through gh.
 *
 * It does not include a header of Windows, so the tests run it on the
 * host; ssh-thread.c runs gh and shows the list.
 */

#ifndef MIKUTERM_CODESPACE_H
#define MIKUTERM_CODESPACE_H

#include <stddef.h>

/*
 * gh's arguments for the list: one line a codespace, its name, its state,
 * its repository and its display name separated by tabs (jq's @tsv).
 */
#define CODESPACE_LIST_ARGUMENTS	"codespace list --json name,state,repository,displayName --jq \".[] | [.name, .state, .repository, .displayName] | @tsv\""

/* The state of a codespace that runs: the others start when they are connected to. */
#define CODESPACE_AVAILABLE		"Available"

/* The longest name, state, repository and display name of a codespace kept, with their NULs. */
#define CODESPACE_NAME_MAX		96U
#define CODESPACE_STATE_MAX		32U
#define CODESPACE_TEXT_MAX		128U

/* The longest arguments of gh for one codespace's settings, with the NUL. */
#define CODESPACE_ARGUMENTS_MAX		(CODESPACE_NAME_MAX + 48U)

/* One codespace of gh's list. */
struct codespace_entry {
	char name[CODESPACE_NAME_MAX];
	char state[CODESPACE_STATE_MAX];
	char repository[CODESPACE_TEXT_MAX];
	char display[CODESPACE_TEXT_MAX];
};

unsigned codespace_list_read(const char *text, struct codespace_entry *entries, unsigned most);
int codespace_config_arguments(const char *name, char *arguments, size_t size);
int codespace_config_host(const char *text, size_t size, char *host, size_t host_size);
int codespace_proxy_command(const char *gh, const char *name, const char *identity, char *command, size_t size);

#endif
