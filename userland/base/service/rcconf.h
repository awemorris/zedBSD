/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_RCCONF_H
#define ZEDBSD_RCCONF_H

#include <stddef.h>
#include <stdio.h>

#define RCCONF_PATH "/etc/rc.conf"
#define RCCONF_VERSION 1U

#define RCCONF_LINE_CAPACITY 1024
#define RCCONF_HOSTNAME_CAPACITY 256
#define RCCONF_SERVICE_MAX 32
#define RCCONF_SERVICE_NAME_CAPACITY 64
#define RCCONF_SETTING_MAX 4
#define RCCONF_SETTING_NAME_CAPACITY 64
#define RCCONF_SETTING_VALUE_CAPACITY 512

struct rcconf_setting {
	char name[RCCONF_SETTING_NAME_CAPACITY];
	char value[RCCONF_SETTING_VALUE_CAPACITY];
};

struct rcconf_service {
	char name[RCCONF_SERVICE_NAME_CAPACITY];
	int enabled;
	size_t setting_count;
	struct rcconf_setting settings[RCCONF_SETTING_MAX];
};

struct rcconf_model {
	unsigned int version;
	char hostname[RCCONF_HOSTNAME_CAPACITY];
	size_t service_count;
	struct rcconf_service services[RCCONF_SERVICE_MAX];
};

typedef int (*rcconf_mutator_t)(struct rcconf_model *, void *);

void rcconf_model_init(struct rcconf_model *);
int rcconf_model_validate(const struct rcconf_model *);
int rcconf_load(const char *, struct rcconf_model *);
int rcconf_write(FILE *, const struct rcconf_model *);

int rcconf_service_enabled(const struct rcconf_model *, const char *, int *);
int rcconf_setting_get(const struct rcconf_model *, const char *, const char *,
		       char *, size_t);
int rcconf_model_set_enabled(struct rcconf_model *, const char *, int);
int rcconf_model_set_setting(struct rcconf_model *, const char *, const char *,
			     const char *);

int rcconf_update(const char *, rcconf_mutator_t, void *);
int rcconf_set_enabled(const char *, const char *, int);

#endif
