/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * WS131 p011: libkeiland's kl_system_* for Settings' host tests
 * (host-build.sh), without Wayland: no compositor offers the system, so
 * kl_system_open gives none and Settings' sound page says it is not
 * available (host-network.c fills the network pages by hand).  Test code
 * only; the program never has it.
 */

#include <keiland/keiland.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kl_system *
kl_system_open(struct wl_display *display)
{
	(void)display;
	errno = ENOTSUP;
	return NULL;
}

void
kl_system_close(struct kl_system *system)
{
	(void)system;
}

int
kl_system_dispatch(struct kl_system *system, unsigned *changed)
{
	(void)system;
	if (changed != NULL)
		*changed = 0U;
	return 0;
}

/*
 * The account (ws160-p002): with HOST_ACCOUNT_RESULT=ERRNO in the
 * environment the desktop offers it, and a password change is answered
 * with that errno (0 changed) at the next take_result; the passwords asked
 * are printed as their lengths only.
 */
static int host_account_pending;
static uint32_t host_account_request;

unsigned
kl_system_capabilities(const struct kl_system *system)
{
	(void)system;
	if (getenv("HOST_KEYS") != NULL && getenv("HOST_KEY_OPS") != NULL)
		return KL_SYSTEM_HAS_ACCOUNT | KL_SYSTEM_HAS_PIN | KL_SYSTEM_HAS_KEYS | KL_SYSTEM_HAS_KEY_OPS;
	if (getenv("HOST_KEYS") != NULL)
		return KL_SYSTEM_HAS_ACCOUNT | KL_SYSTEM_HAS_PIN | KL_SYSTEM_HAS_KEYS;
	if (getenv("HOST_ACCOUNT_RESULT") != NULL)
		return KL_SYSTEM_HAS_ACCOUNT;
	return 0U;
}

int
kl_system_take_result(struct kl_system *system, uint32_t *request, int *error)
{
	const char *answer;

	(void)system;
	if (!host_account_pending)
		return 0;
	host_account_pending = 0;
	answer = getenv("HOST_ACCOUNT_RESULT");
	*request = host_account_request;
	*error = answer != NULL ? atoi(answer) : ENOTSUP;
	return 1;
}

void
kl_system_audio_get_state(const struct kl_system *system, struct kl_audio_state *state)
{
	(void)system;
	memset(state, 0, sizeof(*state));
}

int
kl_system_audio_set_volume(struct kl_system *system, unsigned left, unsigned right, unsigned muted, uint32_t *request)
{
	(void)system;
	(void)left;
	(void)right;
	(void)muted;
	(void)request;
	return ENOTSUP;
}

int
kl_system_audio_feedback(struct kl_system *system, uint32_t *request)
{
	(void)system;
	(void)request;
	return ENOTSUP;
}

/* The account (ws160-p002): offered with HOST_ACCOUNT_RESULT, answered at the next take_result. */
int
kl_system_account_set_password(struct kl_system *system, const char *current, const char *fresh, uint32_t *request)
{
	(void)system;
	if (getenv("HOST_ACCOUNT_RESULT") == NULL)
		return ENOTSUP;
	host_account_request++;
	host_account_pending = 1;
	if (request != NULL)
		*request = host_account_request;
	printf("HOSTACCOUNT set-password request=%u current_length=%zu new_length=%zu\n", host_account_request, strlen(current), strlen(fresh));
	return 0;
}

/* The administration of the accounts (ws089-p026): not offered by the stand-in. */
int
kl_system_account_administer(struct kl_system *system, const char *password, const char *operation, uint32_t *request)
{
	(void)system;
	(void)password;
	(void)operation;
	(void)request;
	return ENOTSUP;
}

/* The lock screen's PIN (ws163-p003): not offered by the stand-in. */
int
kl_system_account_set_pin(struct kl_system *system, const char *current, const char *pin, uint32_t *request)
{
	(void)system;
	(void)current;
	(void)pin;
	(void)request;
	return ENOTSUP;
}

/* What the user has enrolled (ws172-p002): never told by the stand-in. */
int
kl_system_account_enrolled(const struct kl_system *system, unsigned *pin, unsigned *keys)
{
	(void)system;
	*pin = 0U;
	*keys = 0U;
	return 0;
}

/*
 * The security keys (ws172-p003): with HOST_KEYS in the environment two
 * keys are listed and an addition or a removal is asked (its answer never
 * comes); without it there are none.
 */
size_t
kl_system_account_keys(const struct kl_system *system, struct kl_system_key *keys, size_t capacity)
{
	(void)system;
	if (getenv("HOST_KEYS") == NULL || capacity < 2U)
		return 0U;
	snprintf(keys[0].ref, sizeof(keys[0].ref), "0123456789abcdef");
	snprintf(keys[0].label, sizeof(keys[0].label), "YubiKey 5 NFC");
	snprintf(keys[1].ref, sizeof(keys[1].ref), "fedcba9876543210");
	snprintf(keys[1].label, sizeof(keys[1].label), "Spare key");
	return 2U;
}

int
kl_system_account_add_key(struct kl_system *system, const char *password, const char *label, const char *pin, uint32_t *request)
{
	(void)system;
	printf("HOST key add label=%s password=%zu pin=%zu\n", label, strlen(password), strlen(pin));
	*request = 77U;
	return 0;
}

int
kl_system_account_remove_key(struct kl_system *system, const char *password, const char *ref, uint32_t *request)
{
	(void)system;
	printf("HOST key remove ref=%s password=%zu\n", ref, strlen(password));
	*request = 78U;
	return 0;
}

/*
 * The keys' own operations (ws199-p001): offered with HOST_KEYS and
 * HOST_KEY_OPS; what is there is HOST_KEY_COUNT keys (default 1), the
 * one with a PIN unless HOST_KEY_NO_PIN; each request is answered with
 * HOST_ACCOUNT_RESULT at the next take_result.
 */
int
kl_system_account_key_info(struct kl_system *system, uint32_t *request)
{
	(void)system;
	if (getenv("HOST_KEY_OPS") == NULL)
		return ENOTSUP;
	printf("HOST key info\n");
	*request = 80U;
	host_account_request = 80U;
	host_account_pending = 1;
	return 0;
}

int
kl_system_account_key_info_get(const struct kl_system *system, struct kl_system_key_info *info)
{
	const char *count;

	(void)system;
	memset(info, 0, sizeof(*info));
	count = getenv("HOST_KEY_COUNT");
	info->count = 1U;
	if (count != NULL)
		info->count = (unsigned)atoi(count);
	snprintf(info->name, sizeof(info->name), "YubiKey 5 NFC");
	info->pin = getenv("HOST_KEY_NO_PIN") == NULL;
	info->retries = 8U;
	info->min = 4U;
	return 1;
}

int
kl_system_account_key_pin(struct kl_system *system, const char *current, const char *pin, uint32_t *request)
{
	(void)system;
	printf("HOST key pin current=%d pin=%zu\n", current != NULL, strlen(pin));
	*request = 81U;
	host_account_request = 81U;
	host_account_pending = 1;
	return 0;
}

int
kl_system_account_key_reset(struct kl_system *system, const char *password, uint32_t *request)
{
	(void)system;
	printf("HOST key reset password=%zu\n", strlen(password));
	*request = 82U;
	host_account_request = 82U;
	host_account_pending = 1;
	return 0;
}

int
kl_system_account_key_cancel(struct kl_system *system)
{
	(void)system;
	printf("HOST key cancel\n");
	return 0;
}

int
kl_system_account_replugged(struct kl_system *system, uint32_t *request)
{
	(void)system;
	(void)request;
	return 0;
}

unsigned
kl_system_account_key_removed(const struct kl_system *system)
{
	(void)system;
	return 1U;
}

int
kl_system_account_touched(struct kl_system *system, uint32_t *request)
{
	(void)system;
	(void)request;
	return 0;
}

/* No refusal's word without the administration. */
int
kl_system_account_refusal(const struct kl_system *system, uint32_t request, char *reason, size_t size)
{
	(void)system;
	(void)request;
	(void)reason;
	(void)size;
	return 0;
}

/* Remote Login (ws089-p025): not offered by the stand-in. */
void
kl_system_sharing_get_state(const struct kl_system *system, struct kl_sharing_state *state)
{
	(void)system;
	memset(state, 0, sizeof(*state));
}

int
kl_system_sharing_set_ssh(struct kl_system *system, unsigned on, uint32_t *request)
{
	(void)system;
	(void)on;
	(void)request;
	return ENOTSUP;
}

int
kl_system_sharing_query(struct kl_system *system, uint32_t *request)
{
	(void)system;
	(void)request;
	return ENOTSUP;
}

/*
 * ws089-p013: the machine's monitor for About.  With HOST_MEMORY in the
 * environment a monitor opens and its frames say 16 GB of memory, 9.5 GB
 * free; without it there is none (as on a compositor without the monitor).
 */
static int host_monitor;

struct kl_system_monitor *
kl_system_monitor_open(struct kl_system *system, unsigned period_ms)
{
	(void)system;
	printf("HOST monitor open period=%u\n", period_ms);
	if (getenv("HOST_MEMORY") == NULL) {
		errno = ENOTSUP;
		return NULL;
	}
	return (struct kl_system_monitor *)&host_monitor;
}

int
kl_system_monitor_take(struct kl_system_monitor *monitor, struct kl_monitor_frame *frame)
{
	(void)monitor;
	memset(frame, 0, sizeof(*frame));
	frame->valid = KL_MONITOR_FRAME_MEMORY;
	frame->memory_total = 16ULL * 1024ULL * 1024ULL * 1024ULL;
	frame->memory_free = 9ULL * 1024ULL * 1024ULL * 1024ULL + 512ULL * 1024ULL * 1024ULL;
	return 1;
}

void
kl_system_monitor_close(struct kl_system_monitor *monitor)
{
	(void)monitor;
}

/*
 * ws164 (q875): the calls Settings gained since (the printers, KL_VERSION
 * 56; the displays, 58 and 59; the power state), answered as a compositor
 * without them would: none listed, nothing done.
 */
size_t
kl_system_printers_get(const struct kl_system *system, struct kl_printer *printers, size_t capacity)
{
	(void)system;
	(void)printers;
	(void)capacity;
	return 0;
}

size_t
kl_system_print_jobs_get(const struct kl_system *system, struct kl_print_job *jobs, size_t capacity)
{
	(void)system;
	(void)jobs;
	(void)capacity;
	return 0;
}

int
kl_system_printers_add(struct kl_system *system, unsigned protocol, const char *host, unsigned port, const char *path, uint32_t *request)
{
	(void)system;
	(void)protocol;
	(void)host;
	(void)port;
	(void)path;
	(void)request;
	return ENOTSUP;
}

int
kl_system_printers_remove(struct kl_system *system, uint32_t printer, uint32_t *request)
{
	(void)system;
	(void)printer;
	(void)request;
	return ENOTSUP;
}

int
kl_system_printers_set_default(struct kl_system *system, uint32_t printer, uint32_t *request)
{
	(void)system;
	(void)printer;
	(void)request;
	return ENOTSUP;
}

/* A printer's name, IPP path or LPD queue changed (KL_VERSION 75): not offered by the stand-in. */
int
kl_system_printers_edit(struct kl_system *system, uint32_t printer, const char *name, const char *path, uint32_t *request)
{
	(void)system;
	(void)printer;
	(void)name;
	(void)path;
	(void)request;
	return ENOTSUP;
}

int
kl_system_print_cancel(struct kl_system *system, uint32_t job, uint32_t *request)
{
	(void)system;
	(void)job;
	(void)request;
	return ENOTSUP;
}

size_t
kl_system_displays_get(const struct kl_system *system, struct kl_display *displays, size_t capacity)
{
	(void)system;
	(void)displays;
	(void)capacity;
	return 0;
}

unsigned
kl_system_displays_mode(const struct kl_system *system)
{
	(void)system;
	return 0;
}

int
kl_system_displays_apply(struct kl_system *system, unsigned mode, const struct kl_display_place *places, size_t count, uint32_t *request)
{
	(void)system;
	(void)mode;
	(void)places;
	(void)count;
	(void)request;
	return ENOTSUP;
}

int
kl_system_displays_set_brightness(struct kl_system *system, const char *key, unsigned percent, uint32_t *request)
{
	(void)system;
	(void)key;
	(void)percent;
	(void)request;
	return ENOTSUP;
}

int
kl_system_displays_set_shown(struct kl_system *system, const char *key, unsigned shown, uint32_t *request)
{
	(void)system;
	(void)key;
	(void)shown;
	(void)request;
	return ENOTSUP;
}

void
kl_system_power_get_state(const struct kl_system *system, struct kl_power_state *state)
{
	(void)system;
	memset(state, 0, sizeof(*state));
}

/* The computer's readings (ws188-p002): not offered without a compositor, so the pages show nothing read. */
int
kl_system_machine_query(struct kl_system *system, unsigned what, uint32_t *request)
{
	(void)system;
	(void)what;
	(void)request;
	return ENOTSUP;
}

unsigned
kl_system_machine_known(const struct kl_system *system)
{
	(void)system;
	return 0U;
}

uint32_t
kl_system_machine_serial(const struct kl_system *system, unsigned part)
{
	(void)system;
	(void)part;
	return 0U;
}

int
kl_system_machine_about(const struct kl_system *system, struct kl_machine_about *about)
{
	(void)system;
	(void)about;
	return ENOENT;
}

size_t
kl_system_machine_filesystems(const struct kl_system *system, struct kl_machine_filesystem *list, size_t capacity)
{
	(void)system;
	(void)list;
	(void)capacity;
	return 0U;
}

size_t
kl_system_machine_users(const struct kl_system *system, struct kl_machine_user *list, size_t capacity)
{
	(void)system;
	(void)list;
	(void)capacity;
	return 0U;
}

int
kl_system_machine_login_language(const struct kl_system *system, char *code, size_t size)
{
	(void)system;
	(void)code;
	(void)size;
	return ENOENT;
}

/* Bluetooth (ws143-p006): not offered without a compositor, so the page says it is not available. */
int
kl_system_bluetooth_state(const struct kl_system *system, struct kl_bluetooth_state *state)
{
	(void)system;
	memset(state, 0, sizeof(*state));
	return ENOTSUP;
}

size_t
kl_system_bluetooth_devices(const struct kl_system *system, struct kl_bluetooth_device *devices, size_t capacity)
{
	(void)system;
	(void)devices;
	(void)capacity;
	return 0U;
}

int
kl_system_bluetooth_watch(struct kl_system *system, unsigned on)
{
	(void)system;
	(void)on;
	return ENOTSUP;
}

int
kl_system_bluetooth_scan(struct kl_system *system, unsigned on)
{
	(void)system;
	(void)on;
	return ENOTSUP;
}

int
kl_system_bluetooth_power(struct kl_system *system, unsigned on, uint32_t *request)
{
	(void)system;
	(void)on;
	(void)request;
	return ENOTSUP;
}

int
kl_system_bluetooth_device(struct kl_system *system, unsigned action, const char *address, unsigned type, uint32_t *request)
{
	(void)system;
	(void)action;
	(void)address;
	(void)type;
	(void)request;
	return ENOTSUP;
}
