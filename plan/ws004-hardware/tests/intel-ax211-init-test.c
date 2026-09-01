/*
 * Intel AX211 private initialization protocol fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-init.h"

static uint32_t
get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static void
put_command_version(uint8_t *bytes, size_t index, uint8_t group,
	uint8_t opcode, uint8_t command_version, uint8_t notification_version)
{
	uint8_t *entry;

	entry = bytes +
	    index * INTEL_AX211_PROTOCOL_COMMAND_VERSION_ENTRY_SIZE;
	entry[0] = opcode;
	entry[1] = group;
	entry[2] = command_version;
	entry[3] = notification_version;
}

static void
make_api89_command_table(
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES])
{
	size_t index;

	for (index = 0U;
	    index < INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U; index++)
		put_command_version(bytes, index, 0x10U, (uint8_t)index,
		    1U, 1U);
	put_command_version(bytes, 0U, 0x00U, 0x01U, 99U, 6U);
	put_command_version(bytes, 1U, 0x01U, 0x0cU, 5U, 0U);
	put_command_version(bytes, 2U, 0x01U, 0x0dU, 17U, 0U);
	put_command_version(bytes, 3U, 0x0cU, 0x00U, 1U, 0U);
	put_command_version(bytes, 4U, 0x0cU, 0x02U, 1U, 4U);
	put_command_version(bytes, 5U, 0x0cU, 0xfeU, 99U, 1U);
	put_command_version(bytes,
	    INTEL_AX211_PROTOCOL_API89_COMMAND_COUNT - 1U,
	    0U, 0U, 0U, 0U);
}

static void
test_extended_cfg(void)
{
	uint8_t bytes[INTEL_AX211_INIT_EXTENDED_CFG_SIZE + 1U];
	enum intel_ax211_init_profile profile;

	memset(bytes, 0xa5, sizeof(bytes));
	assert(intel_ax211_init_extended_cfg_encode(
	    INTEL_AX211_INIT_PROFILE_RUNTIME, bytes) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(get_le32(bytes) == 0U);
	assert(bytes[4] == 0xa5U);
	profile = INTEL_AX211_INIT_PROFILE_READ_NVM;
	assert(intel_ax211_init_extended_cfg_decode(bytes, 4U, &profile) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(profile == INTEL_AX211_INIT_PROFILE_RUNTIME);

	assert(intel_ax211_init_extended_cfg_encode(
	    INTEL_AX211_INIT_PROFILE_READ_NVM, bytes) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(bytes[0] == 0x02U && bytes[1] == 0U && bytes[2] == 0U &&
	    bytes[3] == 0U && bytes[4] == 0xa5U);
	profile = INTEL_AX211_INIT_PROFILE_RUNTIME;
	assert(intel_ax211_init_extended_cfg_decode(bytes, 4U, &profile) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(profile == INTEL_AX211_INIT_PROFILE_READ_NVM);

	assert(intel_ax211_init_extended_cfg_encode(
	    (enum intel_ax211_init_profile)2, bytes) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	assert(intel_ax211_init_extended_cfg_encode(
	    INTEL_AX211_INIT_PROFILE_RUNTIME, NULL) ==
	    INTEL_AX211_PROTOCOL_INVALID);
	assert(intel_ax211_init_extended_cfg_decode(NULL, 4U, &profile) ==
	    INTEL_AX211_PROTOCOL_INVALID);
	assert(intel_ax211_init_extended_cfg_decode(bytes, 4U, NULL) ==
	    INTEL_AX211_PROTOCOL_INVALID);
	assert(intel_ax211_init_extended_cfg_decode(bytes, 3U, &profile) ==
	    INTEL_AX211_PROTOCOL_TRUNCATED);
	assert(intel_ax211_init_extended_cfg_decode(bytes, 5U, &profile) ==
	    INTEL_AX211_PROTOCOL_OVERSIZED);

	profile = INTEL_AX211_INIT_PROFILE_RUNTIME;
	put_le32(bytes, 1U);
	assert(intel_ax211_init_extended_cfg_decode(bytes, 4U, &profile) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	assert(profile == INTEL_AX211_INIT_PROFILE_RUNTIME);
	put_le32(bytes, 4U);
	assert(intel_ax211_init_extended_cfg_decode(bytes, 4U, &profile) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	put_le32(bytes, 0x02000000U);
	assert(intel_ax211_init_extended_cfg_decode(bytes, 4U, &profile) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
}

static void
test_command_table(void)
{
	uint8_t bytes[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	uint8_t malformed[INTEL_AX211_PROTOCOL_API89_COMMAND_BYTES];
	struct intel_ax211_protocol_command_table table;

	make_api89_command_table(bytes);
	assert(intel_ax211_protocol_command_table_parse(bytes, sizeof(bytes),
	    &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_init_api89_validate(&table) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_init_api89_validate(NULL) ==
	    INTEL_AX211_PROTOCOL_INVALID);

	memcpy(malformed, bytes, sizeof(malformed));
	put_command_version(malformed, 6U, INTEL_AX211_INIT_SYSTEM_GROUP,
	    INTEL_AX211_INIT_EXTENDED_CFG_OPCODE,
	    INTEL_AX211_INIT_EXTENDED_CFG_VERSION, 0U);
	assert(intel_ax211_protocol_command_table_parse(malformed,
	    sizeof(malformed), &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_init_api89_validate(&table) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);

	memcpy(malformed, bytes, sizeof(malformed));
	put_command_version(malformed, 6U,
	    INTEL_AX211_PROTOCOL_GROUP_LEGACY,
	    INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE, 99U, 0U);
	assert(intel_ax211_protocol_command_table_parse(malformed,
	    sizeof(malformed), &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_init_api89_validate(&table) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);

	memcpy(malformed, bytes, sizeof(malformed));
	put_command_version(malformed, 3U,
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE, 2U, 0U);
	assert(intel_ax211_protocol_command_table_parse(malformed,
	    sizeof(malformed), &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_init_api89_validate(&table) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);

	memcpy(malformed, bytes, sizeof(malformed));
	put_command_version(malformed, 4U,
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE, 1U, 3U);
	assert(intel_ax211_protocol_command_table_parse(malformed,
	    sizeof(malformed), &table) == INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_init_api89_validate(&table) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
}

static struct intel_ax211_protocol_message
make_init_complete(uint32_t generation)
{
	struct intel_ax211_protocol_message message;

	memset(&message, 0, sizeof(message));
	message.opcode = INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE;
	message.group = INTEL_AX211_PROTOCOL_GROUP_LEGACY;
	message.version = INTEL_AX211_PROTOCOL_UNKNOWN_VERSION;
	message.generation = generation;
	return message;
}

static void
test_init_complete(void)
{
	struct intel_ax211_protocol_message message;
	uint8_t payload = 0U;

	message = make_init_complete(42U);
	assert(intel_ax211_init_complete_validate(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_OK);
	assert(intel_ax211_init_complete_validate(NULL, 42U) ==
	    INTEL_AX211_PROTOCOL_INVALID);
	assert(intel_ax211_init_complete_validate(&message, 0U) ==
	    INTEL_AX211_PROTOCOL_INVALID);

	message.version = 0U;
	assert(intel_ax211_init_complete_validate(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	message = make_init_complete(42U);
	message.group = INTEL_AX211_INIT_SYSTEM_GROUP;
	assert(intel_ax211_init_complete_validate(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_UNSUPPORTED);
	message = make_init_complete(42U);
	message.flags = INTEL_AX211_PROTOCOL_COMMAND_FAILED_MASK;
	assert(intel_ax211_init_complete_validate(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_FAILED);
	message = make_init_complete(41U);
	assert(intel_ax211_init_complete_validate(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_STALE);
	message = make_init_complete(42U);
	message.payload = &payload;
	message.payload_length = 1U;
	assert(intel_ax211_init_complete_validate(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_OVERSIZED);
	message.payload = NULL;
	assert(intel_ax211_init_complete_validate(&message, 42U) ==
	    INTEL_AX211_PROTOCOL_INVALID);
}

static uint8_t *
read_file(const char *path, size_t *length)
{
	FILE *file;
	long end;
	uint8_t *bytes;

	file = fopen(path, "rb");
	assert(file != NULL);
	assert(fseek(file, 0L, SEEK_END) == 0);
	end = ftell(file);
	assert(end > 0L);
	assert(fseek(file, 0L, SEEK_SET) == 0);
	bytes = malloc((size_t)end);
	assert(bytes != NULL);
	assert(fread(bytes, 1U, (size_t)end, file) == (size_t)end);
	assert(fclose(file) == 0);
	*length = (size_t)end;
	return bytes;
}

static void
test_real_api89_table(const char *path)
{
	struct intel_ax211_protocol_command_table table;
	uint8_t *firmware;
	size_t length;
	size_t offset;
	int found;

	firmware = read_file(path, &length);
	assert(length == 1736748U);
	assert(length >= 88U);
	offset = 88U;
	found = 0;
	while (offset < length) {
		uint32_t type;
		uint32_t tlv_length;
		size_t padded;

		assert(length - offset >= 8U);
		type = get_le32(firmware + offset);
		tlv_length = get_le32(firmware + offset + 4U);
		offset += 8U;
		padded = (size_t)tlv_length;
		if ((padded & 3U) != 0U) {
			size_t padding = 4U - (padded & 3U);

			assert(padded <= SIZE_MAX - padding);
			padded += padding;
		}
		assert(padded <= length - offset);
		if (type == 48U) {
			assert(!found);
			assert(intel_ax211_protocol_command_table_parse(
			    firmware + offset, tlv_length, &table) ==
			    INTEL_AX211_PROTOCOL_OK);
			assert(intel_ax211_init_api89_validate(&table) ==
			    INTEL_AX211_PROTOCOL_OK);
			found = 1;
		}
		offset += padded;
	}
	assert(found);
	free(firmware);
}

int
main(int argc, char **argv)
{
	assert(argc == 1 || argc == 2);
	test_extended_cfg();
	test_command_table();
	test_init_complete();
	if (argc == 2)
		test_real_api89_table(argv[1]);
	puts("intel ax211 init protocol tests passed");
	return 0;
}
