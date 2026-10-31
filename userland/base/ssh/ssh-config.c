/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The configuration files of sshd and ssh.
 *
 * Skeleton: every function answers ENOSYS (or does nothing) until it is
 * implemented.
 */

#include "ssh-config.h"

/*
 * Fills config with sshd's defaults (host_key_count 0: the default key
 * is added by sshd_config_finish when no HostKey line gave one).
 */
void
sshd_config_defaults(
	struct sshd_config *config)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
}

/*
 * Reads size bytes of sshd_config text into config (which holds the
 * defaults or earlier lines).
 */
int
sshd_config_parse(
	struct sshd_config *config,
	const char *text,
	size_t size,
	char *error)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
	(void)text;
	(void)size;
	(void)error;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Adds the default host key when none was given.
 */
int
sshd_config_finish(
	struct sshd_config *config)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Clears config (set_mask 0, every field unset).
 */
void
ssh_config_init(
	struct ssh_config *config)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
}

/*
 * Reads size bytes of ssh_config text for destination host (the name the
 * user gave) into config, setting only fields not yet set.
 */
int
ssh_config_parse(
	struct ssh_config *config,
	const char *text,
	size_t size,
	const char *host,
	char *error)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
	(void)text;
	(void)size;
	(void)host;
	(void)error;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Applies one -o option ("Keyword=value" or "Keyword value"), as a line
 * outside every Host block.
 */
int
ssh_config_option(
	struct ssh_config *config,
	const char *option,
	char *error)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
	(void)option;
	(void)error;

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Fills every field not set with its default.
 */
void
ssh_config_defaults(
	struct ssh_config *config)
{
	/* Marks the arguments used until the function is implemented. */
	(void)config;
}
