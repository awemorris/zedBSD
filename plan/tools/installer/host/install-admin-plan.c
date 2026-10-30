/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/*
 * The host driver of install-admin's requests, plan and installed settings
 * (plan/tools/installer/host/install-admin-plan-test.sh):
 *   install-admin-plan read FILE          -> "RESULT errno consumed verb" and, when read, "CHECK errno" and a summary
 *   install-admin-plan prefixes FILE      -> "PREFIXES ok" when every prefix of the request is EAGAIN
 *   install-admin-plan two FILE           -> reads two requests from one buffer: "TWO verb verb"
 *   install-admin-plan cfg ROOT SWAP|- 0|1 -> the cfg text, or "RESULT errno"
 *   install-admin-plan accounts ROOT USER FULLNAME PASSWORD ROOT-PASSWORD yes|no -> "RESULT errno"
 *   install-admin-plan hostname ROOT NAME | sshd ROOT | fstab ROOT -> "RESULT errno"
 *   install-admin-plan crypt PASSWORD HASH -> "CRYPT same|other"
 */
#include "userland/desktop/installer/admin/plan.h"
#include "userland/desktop/installer/admin/target.h"

#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char text[PLAN_REQUEST_MAX * 2];
static struct plan_request request;

static size_t read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	size_t size;

	if (file == NULL) {
		perror(path);
		exit(2);
	}
	size = fread(text, 1, sizeof(text), file);
	fclose(file);
	return size;
}

static int open_root(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY);

	if (fd < 0) {
		perror(path);
		exit(2);
	}
	return fd;
}

static void summary(const struct plan_request *r)
{
	const struct install_plan *p = &r->install;
	unsigned i;

	printf("DISK %llu %s %u %llu\n", (unsigned long long)r->disk.registration, r->disk.name, r->disk.sector_bytes,
	       (unsigned long long)r->disk.sectors);
	if (r->verb == PLAN_EDIT_DELETE)
		printf("PART %llu:%llu:%llu:%s\n", (unsigned long long)r->part.disk, (unsigned long long)r->part.start,
		       (unsigned long long)r->part.sectors, r->part.partuuid);
	if (r->verb == PLAN_EDIT_CREATE || r->verb == PLAN_EDIT_CREATE_WITH_ESP)
		printf("NEW %llu %llu ESP %llu %llu\n", (unsigned long long)r->part_new.start,
		       (unsigned long long)r->part_new.sectors, (unsigned long long)r->esp_new.start,
		       (unsigned long long)r->esp_new.sectors);
	if (r->verb != PLAN_INSTALL)
		return;
	if (p->layout == PLAN_WHOLE)
		printf("WHOLE %llu %llu swap=%d\n", (unsigned long long)p->whole_esp_bytes,
		       (unsigned long long)p->whole_swap_bytes, p->has_swap_partition);
	else
		printf("CHOSEN esp=%s root=%s swap=%s\n", p->esp.partuuid, p->root.partuuid,
		       p->has_swap_partition ? p->swap.partuuid : "none");
	printf("UNITS");
	for (i = 0; i < p->unit_count; i++)
		printf(" %s", p->unit[i]);
	printf("\n");
	printf("USER [%s] [%s] [%s] [%s] %d [%s]\n", p->user, p->fullname, p->password, p->root_password, p->autologin,
	       p->hostname);
}

int main(int argc, char **argv)
{
	size_t length;
	size_t consumed;
	size_t prefix;
	int error;
	int check;

	if (argc < 3)
		return 2;
	if (strcmp(argv[1], "read") == 0) {
		length = read_file(argv[2]);
		error = plan_request_read(text, length, &request, &consumed);
		printf("RESULT %d %zu %d\n", error, error == 0 ? consumed : 0, error == 0 ? (int)request.verb : 0);
		if (error != 0)
			return 0;
		check = 0;
		if (request.verb == PLAN_INSTALL)
			check = plan_install_check(&request.disk, &request.install);
		else if (request.verb != PLAN_PROBE && request.verb != PLAN_CANCEL)
			check = plan_edit_check(&request);
		printf("CHECK %d\n", check);
		summary(&request);
		plan_request_forget(&request);
		if (request.install.password[0] != '\0' || request.install.root_password[0] != '\0')
			printf("FORGET failed\n");
		return 0;
	}
	if (strcmp(argv[1], "prefixes") == 0) {
		length = read_file(argv[2]);
		for (prefix = 0; prefix < length; prefix++) {
			error = plan_request_read(text, prefix, &request, &consumed);
			if (error != EAGAIN) {
				printf("PREFIXES %zu gave %d\n", prefix, error);
				return 0;
			}
		}
		printf("PREFIXES ok\n");
		return 0;
	}
	if (strcmp(argv[1], "two") == 0) {
		int first;

		length = read_file(argv[2]);
		error = plan_request_read(text, length, &request, &consumed);
		if (error != 0) {
			printf("TWO first %d\n", error);
			return 0;
		}
		first = (int)request.verb;
		error = plan_request_read(text + consumed, length - consumed, &request, &prefix);
		if (error != 0) {
			printf("TWO second %d\n", error);
			return 0;
		}
		printf("TWO %d %d %zu\n", first, (int)request.verb, consumed + prefix);
		return 0;
	}
	if (strcmp(argv[1], "cfg") == 0 && argc == 5) {
		struct plan_cfg cfg;
		char cfg_text[PLAN_CFG_MAX];

		cfg.root_partuuid = argv[2];
		cfg.swap_partuuid = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
		cfg.graphical = atoi(argv[4]);
		error = plan_cfg_text(&cfg, cfg_text, sizeof(cfg_text), &length);
		if (error != 0)
			printf("RESULT %d\n", error);
		else
			fwrite(cfg_text, 1, length, stdout);
		return 0;
	}
	if (strcmp(argv[1], "accounts") == 0 && argc == 8) {
		struct install_plan plan;

		memset(&plan, 0, sizeof(plan));
		snprintf(plan.user, sizeof(plan.user), "%s", argv[3]);
		snprintf(plan.fullname, sizeof(plan.fullname), "%s", argv[4]);
		snprintf(plan.password, sizeof(plan.password), "%s", argv[5]);
		snprintf(plan.root_password, sizeof(plan.root_password), "%s", argv[6]);
		plan.autologin = strcmp(argv[7], "yes") == 0;
		error = target_accounts(open_root(argv[2]), &plan);
		printf("RESULT %d\n", error);
		return 0;
	}
	if (strcmp(argv[1], "hostname") == 0 && argc == 4) {
		printf("RESULT %d\n", target_hostname_set(open_root(argv[2]), argv[3]));
		return 0;
	}
	if (strcmp(argv[1], "sshd") == 0) {
		printf("RESULT %d\n", target_sshd_set(open_root(argv[2])));
		return 0;
	}
	if (strcmp(argv[1], "fstab") == 0) {
		printf("RESULT %d\n", target_fstab_set(open_root(argv[2])));
		return 0;
	}
	if (strcmp(argv[1], "crypt") == 0 && argc == 4) {
		const char *hash = crypt(argv[2], argv[3]);

		printf("CRYPT %s\n", hash != NULL && strcmp(hash, argv[3]) == 0 ? "same" : "other");
		return 0;
	}
	return 2;
}
