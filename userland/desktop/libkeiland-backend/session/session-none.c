/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The session where no session manager speaks to the compositor (Linux and
 * FreeBSD; libkeiland-backend since ws131-p006).
 *
 * A native system session takes the display at once, has no login screen
 * and no lock through a manager, and ends with Log Out through the
 * compositor's ordinary shutdown: every request answers ENOTSUP and the
 * tick has nothing to read.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/*
 * Has no hand-over to wait for.
 */
int
kl_backend_session_ready(
	struct kl_backend *backend)
{
	/* The display may be taken at once. */
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has no manager to ask for a Log Out.
 */
int
kl_backend_session_logout(
	struct kl_backend *backend)
{
	/* The compositor ends by itself. */
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has no login screen to log in from.
 */
int
kl_backend_session_authenticate(
	struct kl_backend *backend,
	const char *user,
	unsigned style,
	const char *secret)
{
	/* Nothing is sent, and nothing is kept. */
	(void)user;
	(void)style;
	(void)secret;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has no manager to unlock through.
 */
int
kl_backend_session_unlock(
	struct kl_backend *backend,
	unsigned style,
	const char *secret)
{
	/* Nothing is sent, and nothing is kept. */
	(void)style;
	(void)secret;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has no manager to ask for the styles.
 */
int
kl_backend_session_styles(
	struct kl_backend *backend,
	const char *user)
{
	/* Nothing is asked. */
	(void)user;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Gives the password alone.
 */
unsigned
kl_backend_session_styles_get(
	const struct kl_backend *backend)
{
	/* No other style without a manager. */
	(void)backend;
	return KL_BACKEND_STYLE_PASSWORD;
}

/*
 * Has no manager to set a PIN through.
 */
int
kl_backend_session_set_pin(
	struct kl_backend *backend,
	const char *password,
	const char *pin)
{
	/* Nothing is sent, and nothing is kept. */
	(void)password;
	(void)pin;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has no manager to ask what is enrolled.
 */
int
kl_backend_session_enrolled(
	struct kl_backend *backend)
{
	/* Nothing is asked. */
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Gives no PIN and no key.
 */
void
kl_backend_session_enrolled_get(
	const struct kl_backend *backend,
	unsigned *pin,
	unsigned *keys)
{
	/* Nothing is enrolled without a manager. */
	(void)backend;
	*pin = 0U;
	*keys = 0U;
}

/*
 * Gives no key.
 */
size_t
kl_backend_session_keys_get(
	const struct kl_backend *backend,
	struct kl_backend_key *keys,
	size_t capacity)
{
	/* Nothing is enrolled without a manager. */
	(void)backend;
	(void)keys;
	(void)capacity;
	return 0U;
}

/*
 * Has no manager to register a key through.
 */
int
kl_backend_session_add_key(
	struct kl_backend *backend,
	const char *password,
	const char *label,
	const char *pin)
{
	/* Nothing is sent, and nothing is kept. */
	(void)password;
	(void)label;
	(void)pin;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/* Gives the key options' defaults (ws199-p001). */
void
kl_backend_session_options_get(
	const struct kl_backend *backend,
	unsigned *key_pin,
	unsigned *key_touch)
{
	/* Both asked. */
	(void)backend;
	*key_pin = 1U;
	*key_touch = 1U;
}

/* Sets no key options. */
int
kl_backend_session_set_options(
	struct kl_backend *backend,
	const char *password,
	unsigned key_pin,
	unsigned key_touch)
{
	/* Nothing is sent, and nothing is kept. */
	(void)password;
	(void)key_pin;
	(void)key_touch;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/* Asks nothing about the keys (ws199-p001). */
int
kl_backend_session_key_info(
	struct kl_backend *backend)
{
	/* Not here. */
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/* Gives no key. */
void
kl_backend_session_key_info_get(
	const struct kl_backend *backend,
	struct kl_backend_key_info *info)
{
	/* None. */
	(void)backend;
	memset(info, 0, sizeof(*info));
}

/* Asks nobody whose a key is (ws199-p001). */
int
kl_backend_session_key_owner(
	struct kl_backend *backend)
{
	/* Not here. */
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/* Gives no owner. */
void
kl_backend_session_key_owner_get(
	const struct kl_backend *backend,
	struct kl_backend_key_owner *owner)
{
	/* None. */
	(void)backend;
	memset(owner, 0, sizeof(*owner));
}

/* Sets no key's PIN. */
int
kl_backend_session_key_pin(
	struct kl_backend *backend,
	const char *current,
	const char *pin)
{
	/* Nothing is sent, and nothing is kept. */
	(void)current;
	(void)pin;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/* Resets no key. */
int
kl_backend_session_key_reset(
	struct kl_backend *backend,
	const char *password)
{
	/* Nothing is sent, and nothing is kept. */
	(void)password;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/* No registration removed. */
unsigned
kl_backend_session_key_removed(
	const struct kl_backend *backend)
{
	/* None. */
	(void)backend;
	return 0U;
}

/*
 * Has no manager to remove a key through.
 */
int
kl_backend_session_remove_key(
	struct kl_backend *backend,
	const char *password,
	const char *ref)
{
	/* Nothing is sent. */
	(void)password;
	(void)ref;
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has no attempt to stop.
 */
int
kl_backend_session_cancel(
	struct kl_backend *backend)
{
	/* Nothing is sent. */
	if (backend == NULL)
		return EINVAL;
	return ENOTSUP;
}

/*
 * Has no refusal to tell.
 */
const char *
kl_backend_session_reason(
	const struct kl_backend *backend)
{
	/* No manager refused anything. */
	(void)backend;
	return "";
}

/*
 * Tells that no manager started the session.
 */
int
kl_backend_session_managed(
	const struct kl_backend *backend)
{
	/* Neither lock nor Log Out goes through a manager. */
	(void)backend;
	return 0;
}

/*
 * Has nothing to read.
 */
void
kl_backend_session_tick(
	struct kl_backend *backend,
	uint64_t now_ms)
{
	/* No manager sends anything. */
	(void)backend;
	(void)now_ms;
}
