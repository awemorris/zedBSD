/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The timing check of sshd's monitor for fuzz-host.sh: the time from an
 * authentication request to its answer, for an account that exists with
 * the right password, the same account with a wrong one and an empty one,
 * a locked account, and a name that is no account; and a key query for
 * an account that exists and for none.  sshd.h's Timing rule makes every
 * answer wait until SSHD_FAIL_DELAY_MS after the request; the check is that
 * the answers of all the cases lie within 50 ms of each other.
 *
 * The real monitor and account checks (sshd-monitor.c, sshd-login.c,
 * login/verify.c, the host's crypt) run, as root would run them: geteuid,
 * getpwnam_r and getspnam_r are replaced here, so that the accounts are
 * the test's own ("alice" with a SHA-512 crypt hash of "right", "locked"
 * with "!" in front) and nothing of the host's accounts is read, and
 * syslog is silent.  (The child that reads authorized_keys runs as this
 * process: its change of ids fails without root, which refuses the key,
 * in the same time for both accounts.)
 *
 *   timing-host <rounds>      prints one line per case and "timing: PASS" or "timing: FAIL"
 */

#define _GNU_SOURCE

#include <crypt.h>
#include <errno.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ssh-auth.h"
#include "sshd-monitor.h"

static char timing_hash[256];
static char timing_locked[260];

uid_t
geteuid(void)
{
	return 0;
}

int
getpwnam_r(const char *name, struct passwd *account, char *buffer, size_t size, struct passwd **result)
{
	*result = NULL;
	if (strcmp(name, "alice") != 0 && strcmp(name, "locked") != 0)
		return 0;
	if (size < 64)
		return ERANGE;
	memset(account, 0, sizeof(*account));
	strcpy(buffer, name);
	account->pw_name = buffer;
	account->pw_passwd = buffer + strlen(name);
	account->pw_dir = "/nonexistent-home";
	account->pw_shell = "/bin/sh";
	account->pw_uid = 4243;
	account->pw_gid = 4243;
	*result = account;
	return 0;
}

int
getspnam_r(const char *name, struct spwd *entry, char *buffer, size_t size, struct spwd **result)
{
	const char *hash;

	*result = NULL;
	if (strcmp(name, "alice") == 0)
		hash = timing_hash;
	else if (strcmp(name, "locked") == 0)
		hash = timing_locked;
	else
		return 0;
	if (strlen(hash) + 1U > size)
		return ERANGE;
	memset(entry, 0, sizeof(*entry));
	strcpy(buffer, hash);
	entry->sp_namp = (char *)name;
	entry->sp_pwdp = buffer;
	*result = entry;
	return 0;
}

void
syslog(int priority, const char *format, ...)
{
	(void)priority;
	(void)format;
}

static long
timing_request(const struct sshd_config *config, const struct ssh_key *host_key, unsigned type, const char *user, const char *password, uint32_t *result)
{
	struct sshd_monitor monitor;
	struct ssh_buf body;
	struct ssh_buf reply;
	struct ssh_buf blob;
	struct timespec arrived;
	struct timespec done;
	enum sshd_monitor_next next;
	uint8_t h[SSH_SHA256_SIZE];
	int status;

	sshd_monitor_init(&monitor, config, host_key, 1);
	monitor.sign_interval_ms = 0;
	ssh_buf_init(&body, 4096, 1);
	ssh_buf_init(&reply, 4096, 0);
	memset(h, 0x33, sizeof(h));
	ssh_buf_put_string(&body, h, sizeof(h));
	(void)clock_gettime(CLOCK_MONOTONIC, &arrived);
	(void)sshd_monitor_handle(&monitor, SSH_MON_SIGN, body.data, body.size, &arrived, &reply, &next);
	ssh_buf_reset(&body);
	ssh_buf_reset(&reply);
	ssh_buf_put_cstring(&body, user);
	if (type == SSH_MON_PASSWORD_LOGIN) {
		ssh_buf_put_cstring(&body, password);
	} else {
		ssh_buf_init(&blob, 128, 0);
		ssh_key_put_public_blob(host_key->public_key, &blob);
		ssh_buf_put_string(&body, blob.data, blob.size);
		ssh_buf_free(&blob);
	}
	(void)clock_gettime(CLOCK_MONOTONIC, &arrived);
	status = sshd_monitor_handle(&monitor, (enum ssh_mon_type)type, body.data, body.size, &arrived, &reply, &next);
	(void)clock_gettime(CLOCK_MONOTONIC, &done);
	*result = 0xffffffffU;
	if (status == 0 && reply.size == 9)
		*result = ssh_load_be32(reply.data + 5);
	ssh_buf_free(&body);
	ssh_buf_free(&reply);
	sshd_monitor_clear(&monitor);
	return (long)(done.tv_sec - arrived.tv_sec) * 1000L + (done.tv_nsec - arrived.tv_nsec) / 1000000L;
}

static int
timing_compare(const void *left, const void *right)
{
	long a = *(const long *)left;
	long b = *(const long *)right;
	return (a > b) - (a < b);
}

int
main(int argc, char **argv)
{
	static const char *names[] = { "alice, right password", "alice, wrong password", "alice, empty password",
		"locked, right password", "no such user", "key query, alice", "key query, no such user" };
	static const unsigned types[] = { SSH_MON_PASSWORD_LOGIN, SSH_MON_PASSWORD_LOGIN, SSH_MON_PASSWORD_LOGIN,
		SSH_MON_PASSWORD_LOGIN, SSH_MON_PASSWORD_LOGIN, SSH_MON_PUBKEY_QUERY, SSH_MON_PUBKEY_QUERY };
	static const char *users[] = { "alice", "alice", "alice", "locked", "ghost", "alice", "ghost" };
	static const char *passwords[] = { "right", "wrong", "", "right", "right", "", "" };
	static const uint32_t expected[] = { 0, EACCES, EACCES, EACCES, EACCES, EACCES, EACCES };
	struct sshd_config config;
	struct ssh_key host_key;
	long times[16];
	long medians[7];
	long low;
	long high;
	uint32_t result;
	unsigned rounds;
	unsigned round;
	unsigned index;
	int failed;
	char *hash;

	rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 5;
	if (rounds < 1 || rounds > 16)
		rounds = 5;
	hash = crypt("right", "$6$timingsaltvalue$");
	if (hash == NULL)
		return 2;
	strcpy(timing_hash, hash);
	sprintf(timing_locked, "!%s", timing_hash);
	sshd_config_defaults(&config);
	config.password = 1;
	config.pubkey = 1;
	(void)sshd_config_finish(&config);
	if (ssh_key_generate(&host_key, NULL) != 0)
		return 2;
	failed = 0;
	low = 1000000;
	high = 0;
	for (index = 0; index < 7; index++) {
		for (round = 0; round < rounds; round++) {
			times[round] = timing_request(&config, &host_key, types[index], users[index], passwords[index], &result);
			if (result != expected[index]) {
				printf("FAIL %s: answer %u, expected %u\n", names[index], (unsigned)result, (unsigned)expected[index]);
				failed = 1;
			}
		}
		qsort(times, rounds, sizeof(times[0]), timing_compare);
		medians[index] = times[rounds / 2];
		printf("%-28s median %ld ms (min %ld, max %ld)\n", names[index], medians[index], times[0], times[rounds - 1]);
		if (medians[index] < low)
			low = medians[index];
		if (medians[index] > high)
			high = medians[index];
		if (times[0] < (long)SSHD_FAIL_DELAY_MS) {
			printf("FAIL %s: an answer came before SSHD_FAIL_DELAY_MS\n", names[index]);
			failed = 1;
		}
	}
	printf("spread of the medians: %ld ms\n", high - low);
	if (high - low >= 50)
		failed = 1;
	ssh_key_clear(&host_key);
	printf("timing: %s\n", failed ? "FAIL" : "PASS");
	return failed;
}
