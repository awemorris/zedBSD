/*
 * zedBSD Intel AX211 private initialization protocol codecs
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c.
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * OpenBSD source provenance:
 *   sys/dev/pci/if_iwx.c, if_iwxreg.h at
 *   0f464d413c50396e4e6cd70948f15613d6a73081
 */

#include "intel-ax211-init.h"

static uint32_t
ax211_init_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	    ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void
ax211_init_put_le32(uint8_t *bytes, uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static int
ax211_init_version_require(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode,
	uint8_t command_version,
	uint8_t notification_version)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	result = intel_ax211_protocol_command_version_lookup(table, group,
	    opcode, &version);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	if (version.command_version != command_version ||
	    version.notification_version != notification_version)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	return INTEL_AX211_PROTOCOL_OK;
}

static int
ax211_init_version_require_absent(
	const struct intel_ax211_protocol_command_table *table,
	uint8_t group,
	uint8_t opcode)
{
	struct intel_ax211_protocol_command_version version;
	int result;

	result = intel_ax211_protocol_command_version_lookup(table, group,
	    opcode, &version);
	if (result == INTEL_AX211_PROTOCOL_MISSING)
		return INTEL_AX211_PROTOCOL_OK;
	if (result == INTEL_AX211_PROTOCOL_OK)
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	return result;
}

/*
 * The digest-pinned API89 image does not advertise g2/c03 or legacy g0/c04.
 * OpenBSD supplies the former's fixed v1 wire layout and treats the latter as
 * a legacy, versionless notification.  Their absence is part of the exact
 * artifact identity rather than permission to infer a version.
 */
int
intel_ax211_init_api89_validate(
	const struct intel_ax211_protocol_command_table *table)
{
	int result;

	result = intel_ax211_protocol_command_table_validate_api89(table);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	result = ax211_init_version_require_absent(table,
	    INTEL_AX211_INIT_SYSTEM_GROUP,
	    INTEL_AX211_INIT_EXTENDED_CFG_OPCODE);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	result = ax211_init_version_require_absent(table,
	    INTEL_AX211_PROTOCOL_GROUP_LEGACY,
	    INTEL_AX211_PROTOCOL_INIT_COMPLETE_OPCODE);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	result = ax211_init_version_require(table,
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_ACCESS_COMPLETE_OPCODE,
	    INTEL_AX211_INIT_NVM_ACCESS_COMMAND_VERSION,
	    INTEL_AX211_INIT_NVM_ACCESS_NOTIFICATION_VERSION);
	if (result != INTEL_AX211_PROTOCOL_OK)
		return result;
	return ax211_init_version_require(table,
	    INTEL_AX211_PROTOCOL_GROUP_REGULATORY_NVM,
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_OPCODE,
	    INTEL_AX211_INIT_NVM_GET_INFO_COMMAND_VERSION,
	    INTEL_AX211_PROTOCOL_NVM_GET_INFO_VERSION);
}

int
intel_ax211_init_extended_cfg_encode(
	enum intel_ax211_init_profile profile,
	uint8_t output[INTEL_AX211_INIT_EXTENDED_CFG_SIZE])
{
	uint32_t flags;

	if (output == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (profile == INTEL_AX211_INIT_PROFILE_RUNTIME)
		flags = 0U;
	else if (profile == INTEL_AX211_INIT_PROFILE_READ_NVM)
		flags = INTEL_AX211_INIT_EXTENDED_CFG_NVM_FLAG;
	else
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	ax211_init_put_le32(output, flags);
	return INTEL_AX211_PROTOCOL_OK;
}

int
intel_ax211_init_extended_cfg_decode(
	const uint8_t *bytes,
	size_t length,
	enum intel_ax211_init_profile *profile)
{
	enum intel_ax211_init_profile decoded;
	uint32_t flags;

	if (bytes == NULL || profile == NULL)
		return INTEL_AX211_PROTOCOL_INVALID;
	if (length < INTEL_AX211_INIT_EXTENDED_CFG_SIZE)
		return INTEL_AX211_PROTOCOL_TRUNCATED;
	if (length > INTEL_AX211_INIT_EXTENDED_CFG_SIZE)
		return INTEL_AX211_PROTOCOL_OVERSIZED;
	flags = ax211_init_get_le32(bytes);
	if (flags == 0U)
		decoded = INTEL_AX211_INIT_PROFILE_RUNTIME;
	else if (flags == INTEL_AX211_INIT_EXTENDED_CFG_NVM_FLAG)
		decoded = INTEL_AX211_INIT_PROFILE_READ_NVM;
	else
		return INTEL_AX211_PROTOCOL_UNSUPPORTED;
	*profile = decoded;
	return INTEL_AX211_PROTOCOL_OK;
}

int
intel_ax211_init_complete_validate(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation)
{
	if (generation == 0U)
		return INTEL_AX211_PROTOCOL_INVALID;
	return intel_ax211_protocol_init_complete(message, generation);
}
