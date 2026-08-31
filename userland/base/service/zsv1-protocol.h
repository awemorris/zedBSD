/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland zsv1 protocol interface.
 */

#ifndef ZEDBSD_ZSV1_PROTOCOL_H
#define ZEDBSD_ZSV1_PROTOCOL_H

#include <stddef.h>
#include <sys/types.h>

#define ZSV1_REQUEST_MAX 256U
#define ZSV1_RESPONSE_LINE_MAX 512U
#define ZSV1_SERVICE_MAX 32U
#define ZSV1_DEPENDENCY_MAX 32U
#define ZSV1_NAME_CAPACITY 64U
#define ZSV1_TOKEN_CAPACITY 64U

enum zsv1_command {
	ZSV1_COMMAND_LIST,
	ZSV1_COMMAND_SHOW,
	ZSV1_COMMAND_START,
	ZSV1_COMMAND_STOP,
	ZSV1_COMMAND_RESTART,
	ZSV1_COMMAND_RELOAD,
	ZSV1_COMMAND_HALT,
	ZSV1_COMMAND_POWEROFF,
	ZSV1_COMMAND_REBOOT,
};

enum zsv1_service_state {
	ZSV1_STATE_STOPPED,
	ZSV1_STATE_STARTING,
	ZSV1_STATE_RUNNING,
	ZSV1_STATE_COMPLETED,
	ZSV1_STATE_FAILED,
	ZSV1_STATE_SKIPPED,
};

enum zsv1_record_type {
	ZSV1_RECORD_SERVICE,
	ZSV1_RECORD_AFTER,
	ZSV1_RECORD_REQUIRES,
	ZSV1_RECORD_OK,
	ZSV1_RECORD_ERROR,
	ZSV1_RECORD_END,
};

enum zsv1_dependency_type {
	ZSV1_DEPENDENCY_AFTER,
	ZSV1_DEPENDENCY_REQUIRES,
};

struct zsv1_request {
	enum zsv1_command command;
	char service[ZSV1_NAME_CAPACITY];
};

struct zsv1_service_record {
	char name[ZSV1_NAME_CAPACITY];
	enum zsv1_service_state state;
	int enabled;
	pid_t pid;
};

struct zsv1_dependency_record {
	enum zsv1_dependency_type type;
	char name[ZSV1_NAME_CAPACITY];
};

struct zsv1_record {
	enum zsv1_record_type type;
	struct zsv1_service_record service;
	char name[ZSV1_NAME_CAPACITY];
	char token[ZSV1_TOKEN_CAPACITY];
	int error_number;
};

struct zsv1_response {
	struct zsv1_service_record services[ZSV1_SERVICE_MAX];
	size_t service_count;
	struct zsv1_dependency_record dependencies[ZSV1_DEPENDENCY_MAX];
	size_t dependency_count;
	int ok_present;
	char ok_token[ZSV1_TOKEN_CAPACITY];
	int error_present;
	int error_number;
	char error_reason[ZSV1_TOKEN_CAPACITY];
	int ended;
};

struct zsv1_decoder {
	struct zsv1_response response;
	char line[ZSV1_RESPONSE_LINE_MAX + 1U];
	size_t line_length;
	int failed;
};

int zsv1_name_valid(const char *);
const char *zsv1_command_name(enum zsv1_command);
const char *zsv1_state_name(enum zsv1_service_state);
int zsv1_request_parse(const void *, size_t, struct zsv1_request *);
int zsv1_request_format(const struct zsv1_request *, char *, size_t, size_t *);
int zsv1_record_parse(const void *, size_t, struct zsv1_record *);
int zsv1_record_format(const struct zsv1_record *, char *, size_t, size_t *);

void zsv1_decoder_init(struct zsv1_decoder *);
int zsv1_decoder_feed(struct zsv1_decoder *, const void *, size_t);
int zsv1_decoder_finish(struct zsv1_decoder *);
const struct zsv1_response *zsv1_decoder_response(const struct zsv1_decoder *);

#endif
