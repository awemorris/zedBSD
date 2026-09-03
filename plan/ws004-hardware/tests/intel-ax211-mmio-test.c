/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * Intel AX211 private MMIO fixture
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/drivers/intel-ax211-mmio.h"

#define TEST_EVENT_CAPACITY                              30000U
#define TEST_CSR_CAPACITY                                  256U
#define TEST_NEVER_READY                  UINT64_C(0xffffffffffffffff)

#define TEST_CSR_HW_IF_CONFIG_REG                         0x000U
#define TEST_CSR_RESET                                    0x020U
#define TEST_CSR_GP_CNTRL                                 0x024U
#define TEST_CSR_GIO_REG                                  0x03cU
#define TEST_CSR_MBOX_SET_REG                             0x088U
#define TEST_CSR_LTR_LONG_VAL_AD                          0x0d4U
#define TEST_CSR_GIO_CHICKEN_BITS                         0x100U
#define TEST_CSR_CTXT_INFO_ADDR                           0x118U
#define TEST_CSR_IML_DATA_ADDR                            0x120U
#define TEST_CSR_IML_SIZE_ADDR                            0x128U
#define TEST_CSR_DBG_HPET_MEM_REG                         0x240U
#define TEST_CSR_DBG_LINK_PWR_MGMT_REG                    0x250U
#define TEST_CSR_MAC_ADDRESS0_OTP                         0x380U
#define TEST_CSR_MAC_ADDRESS1_OTP                         0x384U
#define TEST_CSR_MAC_ADDRESS0_STRAP                       0x388U
#define TEST_CSR_MAC_ADDRESS1_STRAP                       0x38cU

#define TEST_HW_IF_HAP_WAKE_L1A                       0x00080000U
#define TEST_HW_IF_NIC_READY                          0x00400000U
#define TEST_HW_IF_PREPARE                            0x08000000U
#define TEST_HW_IF_ENABLE_PME                         0x10000000U
#define TEST_MBOX_OS_ALIVE                            0x00000020U
#define TEST_RESET_SW                                 0x00000080U
#define TEST_RESET_MASTER_DISABLED                    0x00000100U
#define TEST_RESET_STOP_MASTER                        0x00000200U
#define TEST_LINK_POWER_MANAGEMENT_DISABLED           0x80000000U
#define TEST_GP_MAC_CLOCK_READY                       0x00000001U
#define TEST_GP_INIT_DONE                             0x00000004U
#define TEST_GP_MAC_ACCESS_REQ                        0x00000008U
#define TEST_GP_GOING_TO_SLEEP                       0x00000010U
#define TEST_GIO_L0S_DISABLED                         0x00000002U
#define TEST_GIO_L1A_NO_L0S_RX                       0x00800000U
#define TEST_HPET_WAIT_THRESHOLD                      0xffff0000U
#define TEST_AUTO_FUNC_BOOT                           0x00000002U
#define TEST_LTR_BOOTSTRAP                            0x82fa88faU
#define TEST_UMAC_CPU_INIT_RUN                        0x00d05c44U

enum test_event_type {
	TEST_EVENT_CSR_READ = 1,
	TEST_EVENT_CSR_WRITE = 2,
	TEST_EVENT_PRPH_READ = 3,
	TEST_EVENT_PRPH_WRITE = 4,
	TEST_EVENT_DELAY = 5,
	TEST_EVENT_DEADLINE = 6
};

struct test_event {
	enum test_event_type type;
	uint32_t address;
	uint32_t value;
	uint64_t start;
	uint64_t end;
};

struct test_backend {
	uint32_t csr[TEST_CSR_CAPACITY];
	struct test_event event[TEST_EVENT_CAPACITY];
	size_t event_count;
	uint64_t now;
	uint64_t nic_ready_at;
	uint64_t clock_ready_at;
	uint64_t master_disabled_at;
	uint32_t prph_value;
	uint32_t prph_writes;
	int nic_requested;
	int stop_master_requested;
	int going_to_sleep;
	int clock_stuck;
	int fail_csr_read;
	uint32_t fail_csr_read_offset;
	int fail_csr_write;
	uint32_t fail_csr_write_offset;
	int fail_access_clear;
};

static int test_csr_read32(void *argument, uint32_t offset, uint32_t *value);
static int test_csr_write32(void *argument, uint32_t offset, uint32_t value);
static int test_prph_read32(void *argument, uint32_t address, uint32_t *value);
static int test_prph_write32(void *argument, uint32_t address, uint32_t value);
static int test_delay_us(void *argument, uint32_t duration_us);
static int test_clock_us(void *argument, uint64_t *time_us);
static void test_trace_deadline(void *argument, enum intel_ax211_mmio_wait wait, uint64_t start_us, uint64_t deadline_us);
static void test_record(struct test_backend *backend, enum test_event_type type, uint32_t address, uint32_t value, uint64_t start, uint64_t end);
static void test_backend_init(struct test_backend *backend);
static struct intel_ax211_mmio_profile test_profile(uint16_t mac_type);
static int test_state_init(struct intel_ax211_mmio *mmio, struct test_backend *backend, uint16_t mac_type);
static void test_reach_apm(struct intel_ax211_mmio *mmio, struct test_backend *backend);
static size_t test_find_event(const struct test_backend *backend, size_t start, enum test_event_type type, uint32_t address, uint32_t value);
static size_t test_count_event(const struct test_backend *backend, enum test_event_type type);
static void test_exact_profiles(void);
static void test_prepare_immediate_and_fallback(void);
static void test_prepare_timeout_and_clock_failure(void);
static void test_reset_and_apm_order(void);
static void test_nic_ownership(void);
static void test_stop_sequence(void);
static void test_stop_timeout_and_failure(void);
static void test_mac_strap_and_otp(void);
static void test_mac_transaction_failures(void);
static void test_gen3_publication(void);
static void test_gen3_failed_ownership(void);

static const struct intel_ax211_mmio_ops test_ops = {
	test_csr_read32,
	test_csr_write32,
	test_prph_read32,
	test_prph_write32,
	test_delay_us,
	test_clock_us,
	test_trace_deadline
};

int
main(void)
{
	test_exact_profiles();
	test_prepare_immediate_and_fallback();
	test_prepare_timeout_and_clock_failure();
	test_reset_and_apm_order();
	test_nic_ownership();
	test_stop_sequence();
	test_stop_timeout_and_failure();
	test_mac_strap_and_otp();
	test_mac_transaction_failures();
	test_gen3_publication();
	test_gen3_failed_ownership();
	puts("intel ax211 mmio tests: PASS");
	return 0;
}

/* Models one checked CSR read and its hardware-owned status bits. */
static int
test_csr_read32(
	void *argument,
	uint32_t offset,
	uint32_t *value)
{
	struct test_backend *backend;
	uint32_t result;

	backend = argument;
	assert(offset / 4U < TEST_CSR_CAPACITY);
	result = backend->csr[offset / 4U];

	/* Supplies NIC_READY only after hardware accepts the request. */
	if (offset == TEST_CSR_HW_IF_CONFIG_REG) {
		result &= ~TEST_HW_IF_NIC_READY;
		if (backend->nic_requested &&
		    backend->now >= backend->nic_ready_at)
			result |= TEST_HW_IF_NIC_READY;
	}

	/* Supplies clock and sleep status independently of host writes. */
	if (offset == TEST_CSR_GP_CNTRL) {
		result &= ~(TEST_GP_MAC_CLOCK_READY |
		    TEST_GP_GOING_TO_SLEEP);
		if (backend->now >= backend->clock_ready_at)
			result |= TEST_GP_MAC_CLOCK_READY;
		if (backend->going_to_sleep)
			result |= TEST_GP_GOING_TO_SLEEP;
	}
	if (offset == TEST_CSR_RESET) {
		result &= ~TEST_RESET_MASTER_DISABLED;
		if (backend->stop_master_requested &&
		    backend->now >= backend->master_disabled_at)
			result |= TEST_RESET_MASTER_DISABLED;
	}

	*value = result;
	test_record(backend, TEST_EVENT_CSR_READ, offset, result, 0U, 0U);
	if (backend->fail_csr_read &&
	    offset == backend->fail_csr_read_offset)
		return -1;
	return 0;
}

/* Models one checked CSR write. */
static int
test_csr_write32(
	void *argument,
	uint32_t offset,
	uint32_t value)
{
	struct test_backend *backend;

	backend = argument;
	assert(offset / 4U < TEST_CSR_CAPACITY);
	test_record(backend, TEST_EVENT_CSR_WRITE, offset, value, 0U, 0U);
	if (backend->fail_csr_write &&
	    offset == backend->fail_csr_write_offset)
		return -1;
	if (backend->fail_access_clear && offset == TEST_CSR_GP_CNTRL &&
	    (value & TEST_GP_MAC_ACCESS_REQ) == 0U)
		return -1;

	/* Remembers the ownership request but leaves acceptance hardware-owned. */
	if (offset == TEST_CSR_HW_IF_CONFIG_REG &&
	    (value & TEST_HW_IF_NIC_READY) != 0U)
		backend->nic_requested = 1;
	if (offset == TEST_CSR_HW_IF_CONFIG_REG)
		value &= ~TEST_HW_IF_NIC_READY;
	if (offset == TEST_CSR_GP_CNTRL)
		value &= ~(TEST_GP_MAC_CLOCK_READY |
		    TEST_GP_GOING_TO_SLEEP);
	if (offset == TEST_CSR_RESET) {
		value &= ~TEST_RESET_MASTER_DISABLED;
		if ((value & TEST_RESET_STOP_MASTER) != 0U)
			backend->stop_master_requested = 1;
	}
	backend->csr[offset / 4U] = value;
	return 0;
}

/* Models one ownership-protected PRPH read. */
static int
test_prph_read32(
	void *argument,
	uint32_t address,
	uint32_t *value)
{
	struct test_backend *backend;

	backend = argument;
	*value = backend->prph_value;
	test_record(backend, TEST_EVENT_PRPH_READ, address, *value, 0U, 0U);
	return 0;
}

/* Models one ownership-protected PRPH write. */
static int
test_prph_write32(
	void *argument,
	uint32_t address,
	uint32_t value)
{
	struct test_backend *backend;

	backend = argument;
	backend->prph_value = value;
	backend->prph_writes++;
	test_record(backend, TEST_EVENT_PRPH_WRITE, address, value, 0U, 0U);
	return 0;
}

/* Advances the deterministic mock clock unless it is deliberately stuck. */
static int
test_delay_us(
	void *argument,
	uint32_t duration_us)
{
	struct test_backend *backend;

	backend = argument;
	if (duration_us >= 1000U)
		test_record(backend, TEST_EVENT_DELAY, duration_us, 0U, 0U, 0U);
	if (!backend->clock_stuck)
		backend->now += duration_us;
	return 0;
}

/* Returns the deterministic monotonic mock clock. */
static int
test_clock_us(
	void *argument,
	uint64_t *time_us)
{
	struct test_backend *backend;

	backend = argument;
	*time_us = backend->now;
	return 0;
}

/* Records each finite deadline exposed by the private MMIO core. */
static void
test_trace_deadline(
	void *argument,
	enum intel_ax211_mmio_wait wait,
	uint64_t start_us,
	uint64_t deadline_us)
{
	struct test_backend *backend;

	backend = argument;
	test_record(backend, TEST_EVENT_DEADLINE, (uint32_t)wait, 0U,
	    start_us, deadline_us);
}

/* Appends one bounded mock event. */
static void
test_record(
	struct test_backend *backend,
	enum test_event_type type,
	uint32_t address,
	uint32_t value,
	uint64_t start,
	uint64_t end)
{
	struct test_event *event;

	assert(backend->event_count < TEST_EVENT_CAPACITY);
	event = &backend->event[backend->event_count++];
	event->type = type;
	event->address = address;
	event->value = value;
	event->start = start;
	event->end = end;
}

/* Initializes one deterministic backend. */
static void
test_backend_init(
	struct test_backend *backend)
{
	memset(backend, 0, sizeof(*backend));
	backend->nic_ready_at = TEST_NEVER_READY;
	backend->clock_ready_at = TEST_NEVER_READY;
	backend->master_disabled_at = TEST_NEVER_READY;
}

/* Returns the exact P038 transport profile for one admitted MAC type. */
static struct intel_ax211_mmio_profile
test_profile(
	uint16_t mac_type)
{
	struct intel_ax211_mmio_profile profile;

	memset(&profile, 0, sizeof(profile));
	profile.mac_type = mac_type;
	profile.rf_type = INTEL_AX211_MMIO_RF_GF;
	profile.umac_prph_offset = INTEL_AX211_MMIO_UMAC_PRPH_OFFSET;
	return profile;
}

/* Initializes one state against the deterministic backend. */
static int
test_state_init(
	struct intel_ax211_mmio *mmio,
	struct test_backend *backend,
	uint16_t mac_type)
{
	struct intel_ax211_mmio_profile profile;

	profile = test_profile(mac_type);
	return intel_ax211_mmio_init(mmio, &test_ops, backend, &profile);
}

/* Advances one exact state through prepare, reset, and APM. */
static void
test_reach_apm(
	struct intel_ax211_mmio *mmio,
	struct test_backend *backend)
{
	backend->nic_ready_at = backend->now;
	assert(test_state_init(mmio, backend, INTEL_AX211_MMIO_MAC_SO) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_prepare_card_hw(mmio) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_sw_reset(mmio) == INTEL_AX211_MMIO_OK);
	backend->clock_ready_at = backend->now + 30U;
	assert(intel_ax211_mmio_apm_init(mmio) == INTEL_AX211_MMIO_OK);
}

/* Finds one exact event at or after the supplied trace position. */
static size_t
test_find_event(
	const struct test_backend *backend,
	size_t start,
	enum test_event_type type,
	uint32_t address,
	uint32_t value)
{
	size_t index;

	/* Searches the bounded event trace in production order. */
	for (index = start; index < backend->event_count; index++) {
		if (backend->event[index].type != type)
			continue;
		if (backend->event[index].address != address)
			continue;
		if (backend->event[index].value != value)
			continue;
		return index;
	}
	return backend->event_count;
}

/* Counts one event class in the bounded trace. */
static size_t
test_count_event(
	const struct test_backend *backend,
	enum test_event_type type)
{
	size_t index;
	size_t count;

	/* Counts every matching event without interpreting its payload. */
	count = 0U;
	for (index = 0U; index < backend->event_count; index++) {
		if (backend->event[index].type == type)
			count++;
	}
	return count;
}

/* Proves that the caller must supply exact SO-or-SOF plus GF assumptions. */
static void
test_exact_profiles(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_mmio_profile profile;

	test_backend_init(&backend);
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SO) ==
	    INTEL_AX211_MMIO_OK);
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SOF) ==
	    INTEL_AX211_MMIO_OK);

	profile = test_profile(0x42U);
	assert(intel_ax211_mmio_init(&mmio, &test_ops, &backend, &profile) ==
	    INTEL_AX211_MMIO_INVALID);
	profile = test_profile(INTEL_AX211_MMIO_MAC_SO);
	profile.rf_type++;
	assert(intel_ax211_mmio_init(&mmio, &test_ops, &backend, &profile) ==
	    INTEL_AX211_MMIO_INVALID);
	profile = test_profile(INTEL_AX211_MMIO_MAC_SO);
	profile.cdb = 1U;
	assert(intel_ax211_mmio_init(&mmio, &test_ops, &backend, &profile) ==
	    INTEL_AX211_MMIO_INVALID);
	profile = test_profile(INTEL_AX211_MMIO_MAC_SO);
	profile.integrated = 1U;
	assert(intel_ax211_mmio_init(&mmio, &test_ops, &backend, &profile) ==
	    INTEL_AX211_MMIO_INVALID);
	profile = test_profile(INTEL_AX211_MMIO_MAC_SO);
	profile.umac_prph_offset += 4U;
	assert(intel_ax211_mmio_init(&mmio, &test_ops, &backend, &profile) ==
	    INTEL_AX211_MMIO_INVALID);
	assert(backend.event_count == 0U);
}

/* Proves ordinary and fallback NIC_READY/MBOX ordering. */
static void
test_prepare_immediate_and_fallback(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	size_t ready_write;
	size_t mailbox_write;
	size_t link_write;
	size_t prepare_write;

	test_backend_init(&backend);
	backend.nic_ready_at = 0U;
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SO) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_prepare_card_hw(&mmio) ==
	    INTEL_AX211_MMIO_OK);
	ready_write = test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_HW_IF_CONFIG_REG, TEST_HW_IF_NIC_READY);
	mailbox_write = test_find_event(&backend, ready_write + 1U,
	    TEST_EVENT_CSR_WRITE, TEST_CSR_MBOX_SET_REG,
	    TEST_MBOX_OS_ALIVE);
	assert(ready_write < mailbox_write);
	assert(mmio.prepared);

	test_backend_init(&backend);
	backend.nic_ready_at = 1000U;
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SOF) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_prepare_card_hw(&mmio) ==
	    INTEL_AX211_MMIO_OK);
	link_write = test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_DBG_LINK_PWR_MGMT_REG,
	    TEST_LINK_POWER_MANAGEMENT_DISABLED);
	prepare_write = test_find_event(&backend, link_write + 1U,
	    TEST_EVENT_CSR_WRITE, TEST_CSR_HW_IF_CONFIG_REG,
	    TEST_HW_IF_PREPARE | TEST_HW_IF_NIC_READY);
	mailbox_write = test_find_event(&backend, prepare_write + 1U,
	    TEST_EVENT_CSR_WRITE, TEST_CSR_MBOX_SET_REG,
	    TEST_MBOX_OS_ALIVE);
	assert(link_write < prepare_write);
	assert(prepare_write < mailbox_write);
	assert(backend.now >= 1050U);
}

/* Proves finite prepare failure and rejection of a stopped clock. */
static void
test_prepare_timeout_and_clock_failure(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;

	test_backend_init(&backend);
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SO) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_prepare_card_hw(&mmio) ==
	    INTEL_AX211_MMIO_TIMEOUT);
	assert(!mmio.prepared);
	assert(test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_MBOX_SET_REG, TEST_MBOX_OS_ALIVE) == backend.event_count);
	assert(backend.now < 500000U);
	assert(test_count_event(&backend, TEST_EVENT_DEADLINE) > 1U);

	test_backend_init(&backend);
	backend.clock_stuck = 1;
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SO) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_prepare_card_hw(&mmio) ==
	    INTEL_AX211_MMIO_CLOCK);
	assert(!mmio.prepared);
}

/* Proves reset/APM sequencing, workarounds, and exact deadlines. */
static void
test_reset_and_apm_order(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	size_t position;
	size_t deadline;

	test_backend_init(&backend);
	backend.nic_ready_at = 0U;
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SO) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_sw_reset(&mmio) == INTEL_AX211_MMIO_ORDER);
	assert(intel_ax211_mmio_apm_init(&mmio) == INTEL_AX211_MMIO_ORDER);
	assert(intel_ax211_mmio_prepare_card_hw(&mmio) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_apm_init(&mmio) == INTEL_AX211_MMIO_ORDER);
	assert(intel_ax211_mmio_sw_reset(&mmio) == INTEL_AX211_MMIO_OK);
	assert((backend.csr[TEST_CSR_RESET / 4U] & TEST_RESET_SW) != 0U);
	assert(backend.now == 5000U);

	backend.clock_ready_at = backend.now + 30U;
	position = backend.event_count;
	assert(intel_ax211_mmio_apm_init(&mmio) == INTEL_AX211_MMIO_OK);
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_GIO_CHICKEN_BITS, TEST_GIO_L1A_NO_L0S_RX) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_DBG_HPET_MEM_REG, TEST_HPET_WAIT_THRESHOLD) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_HW_IF_CONFIG_REG, TEST_HW_IF_HAP_WAKE_L1A |
	    TEST_HW_IF_NIC_READY) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_GIO_REG, TEST_GIO_L0S_DISABLED) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_GP_CNTRL, TEST_GP_INIT_DONE) + 1U;
	assert(position <= backend.event_count);
	deadline = test_find_event(&backend, 0U, TEST_EVENT_DEADLINE,
	    INTEL_AX211_MMIO_WAIT_APM_CLOCK, 0U);
	assert(deadline < backend.event_count);
	assert(backend.event[deadline].end - backend.event[deadline].start ==
	    25000U);
	assert(mmio.apm_ready);
}

/* Proves ownership bounds, nesting, and PRPH exclusion after failure. */
static void
test_nic_ownership(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	uint32_t value;
	size_t prph_before;
	size_t deadline;

	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	backend.going_to_sleep = 1;
	prph_before = test_count_event(&backend, TEST_EVENT_PRPH_WRITE);
	assert(intel_ax211_mmio_nic_lock(&mmio) ==
	    INTEL_AX211_MMIO_TIMEOUT);
	assert(mmio.nic_lock_depth == 0U);
	assert((backend.csr[TEST_CSR_GP_CNTRL / 4U] &
	    TEST_GP_MAC_ACCESS_REQ) == 0U);
	assert(intel_ax211_mmio_prph_write32(&mmio, 0x1234U, 7U) ==
	    INTEL_AX211_MMIO_NOT_OWNER);
	assert(test_count_event(&backend, TEST_EVENT_PRPH_WRITE) == prph_before);
	deadline = test_find_event(&backend, 0U, TEST_EVENT_DEADLINE,
	    INTEL_AX211_MMIO_WAIT_NIC_OWNERSHIP, 0U);
	assert(deadline < backend.event_count);
	assert(backend.event[deadline].end - backend.event[deadline].start ==
	    150000U);

	backend.going_to_sleep = 0;
	assert(intel_ax211_mmio_nic_lock(&mmio) == INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_nic_lock(&mmio) == INTEL_AX211_MMIO_OK);
	assert(mmio.nic_lock_depth == 2U);
	assert(intel_ax211_mmio_prph_write32(&mmio, 0x1234U, 7U) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_prph_read32(&mmio, 0x1234U, &value) ==
	    INTEL_AX211_MMIO_OK);
	assert(value == 7U);
	assert(intel_ax211_mmio_nic_unlock(&mmio) == INTEL_AX211_MMIO_OK);
	assert(mmio.nic_lock_depth == 1U);
	assert(intel_ax211_mmio_nic_unlock(&mmio) == INTEL_AX211_MMIO_OK);
	assert(mmio.nic_lock_depth == 0U);
	assert(intel_ax211_mmio_nic_unlock(&mmio) ==
	    INTEL_AX211_MMIO_NOT_OWNER);
}

/* Proves exact finite stop ordering and a required fresh lifecycle. */
static void
test_stop_sequence(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	size_t position;
	size_t deadline;

	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	assert(intel_ax211_mmio_nic_lock(&mmio) == INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_nic_lock(&mmio) == INTEL_AX211_MMIO_OK);
	backend.master_disabled_at = backend.now + 6030U;
	backend.event_count = 0U;
	assert(intel_ax211_mmio_stop(&mmio) == INTEL_AX211_MMIO_OK);

	position = test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_GP_CNTRL, TEST_GP_INIT_DONE |
	    TEST_GP_MAC_CLOCK_READY) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_DBG_LINK_PWR_MGMT_REG,
	    TEST_LINK_POWER_MANAGEMENT_DISABLED) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_HW_IF_CONFIG_REG, TEST_HW_IF_HAP_WAKE_L1A |
	    TEST_HW_IF_NIC_READY | TEST_HW_IF_PREPARE |
	    TEST_HW_IF_ENABLE_PME) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_DELAY,
	    1000U, 0U) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_DBG_LINK_PWR_MGMT_REG, 0U) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_DELAY,
	    5000U, 0U) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_RESET, TEST_RESET_SW | TEST_RESET_STOP_MASTER) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_GP_CNTRL, TEST_GP_MAC_CLOCK_READY) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_RESET, TEST_RESET_SW | TEST_RESET_STOP_MASTER |
	    TEST_RESET_MASTER_DISABLED) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_DELAY,
	    5000U, 0U) + 1U;
	assert(position <= backend.event_count);
	deadline = test_find_event(&backend, 0U, TEST_EVENT_DEADLINE,
	    INTEL_AX211_MMIO_WAIT_MASTER_DISABLED, 0U);
	assert(deadline < backend.event_count);
	assert(backend.event[deadline].end - backend.event[deadline].start ==
	    100U);
	assert(mmio.nic_lock_depth == 0U);
	assert(!mmio.prepared);
	assert(!mmio.reset_done);
	assert(!mmio.apm_ready);
	assert(intel_ax211_mmio_sw_reset(&mmio) == INTEL_AX211_MMIO_ORDER);

	/* A stopped object becomes usable only through a fresh full sequence. */
	assert(intel_ax211_mmio_prepare_card_hw(&mmio) ==
	    INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_sw_reset(&mmio) == INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_apm_init(&mmio) == INTEL_AX211_MMIO_OK);
}

/* Proves timeout/failure bounds and fail-closed state publication. */
static void
test_stop_timeout_and_failure(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	uint64_t start;

	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	start = backend.now;
	backend.event_count = 0U;
	assert(intel_ax211_mmio_stop(&mmio) == INTEL_AX211_MMIO_OK);
	assert(mmio.master_disable_timed_out);
	assert(backend.now == start + 11100U);
	assert(test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_GP_CNTRL, TEST_GP_MAC_CLOCK_READY) <
	    backend.event_count);
	assert(test_find_event(&backend, 0U, TEST_EVENT_DELAY, 5000U, 0U) <
	    backend.event_count);
	assert(!mmio.prepared && !mmio.reset_done && !mmio.apm_ready);
	assert(mmio.nic_lock_depth == 0U);

	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	assert(intel_ax211_mmio_nic_lock(&mmio) == INTEL_AX211_MMIO_OK);
	backend.fail_access_clear = 1;
	backend.master_disabled_at = backend.now + 6000U;
	backend.event_count = 0U;
	assert(intel_ax211_mmio_stop(&mmio) == INTEL_AX211_MMIO_IO);
	assert(!mmio.master_disable_timed_out);
	assert(test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_RESET, TEST_RESET_SW | TEST_RESET_STOP_MASTER) <
	    backend.event_count);
	assert(!mmio.prepared && !mmio.reset_done && !mmio.apm_ready);
	assert(mmio.nic_lock_depth == 0U);
	assert(intel_ax211_mmio_stop(NULL) == INTEL_AX211_MMIO_INVALID);
}

/* Proves strap preference, OTP fallback, byte order, and nested ownership. */
static void
test_mac_strap_and_otp(void)
{
	static const uint8_t first_expected[6] = {
		0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U
	};
	static const uint8_t second_expected[6] = {
		0x06U, 0x12U, 0x34U, 0x56U, 0x78U, 0x9aU
	};
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	uint8_t address[6];

	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	backend.csr[TEST_CSR_MAC_ADDRESS0_STRAP / 4U] = 0x02112233U;
	backend.csr[TEST_CSR_MAC_ADDRESS1_STRAP / 4U] = 0x00004455U;
	backend.csr[TEST_CSR_MAC_ADDRESS0_OTP / 4U] = 0x06123456U;
	backend.csr[TEST_CSR_MAC_ADDRESS1_OTP / 4U] = 0x0000789aU;
	memset(address, 0xa5, sizeof(address));
	backend.event_count = 0U;
	assert(intel_ax211_mmio_read_mac(&mmio, address) ==
	    INTEL_AX211_MMIO_OK);
	assert(memcmp(address, first_expected, sizeof(address)) == 0);
	assert(test_find_event(&backend, 0U, TEST_EVENT_CSR_READ,
	    TEST_CSR_MAC_ADDRESS0_STRAP, 0x02112233U) <
	    backend.event_count);
	assert(test_find_event(&backend, 0U, TEST_EVENT_CSR_READ,
	    TEST_CSR_MAC_ADDRESS0_OTP, 0x06123456U) ==
	    backend.event_count);
	assert(mmio.nic_lock_depth == 0U);

	/* The reserved OEM sentinel must fall back to the OTP words. */
	backend.csr[TEST_CSR_MAC_ADDRESS0_STRAP / 4U] = 0x02ccaaffU;
	backend.csr[TEST_CSR_MAC_ADDRESS1_STRAP / 4U] = 0x0000ee00U;
	memset(address, 0xa5, sizeof(address));
	backend.event_count = 0U;
	assert(intel_ax211_mmio_read_mac(&mmio, address) ==
	    INTEL_AX211_MMIO_OK);
	assert(memcmp(address, second_expected, sizeof(address)) == 0);
	assert(test_find_event(&backend, 0U, TEST_EVENT_CSR_READ,
	    TEST_CSR_MAC_ADDRESS0_OTP, 0x06123456U) <
	    backend.event_count);

	/* One nested reference owned by the caller remains owned afterwards. */
	assert(intel_ax211_mmio_nic_lock(&mmio) == INTEL_AX211_MMIO_OK);
	assert(intel_ax211_mmio_read_mac(&mmio, address) ==
	    INTEL_AX211_MMIO_OK);
	assert(mmio.nic_lock_depth == 1U);
	assert(intel_ax211_mmio_nic_unlock(&mmio) == INTEL_AX211_MMIO_OK);
}

/* Proves invalid/read/unlock failures never replace caller identity bytes. */
static void
test_mac_transaction_failures(void)
{
	static const uint8_t unchanged[6] = {
		0xa5U, 0xa5U, 0xa5U, 0xa5U, 0xa5U, 0xa5U
	};
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	uint8_t address[6];

	test_backend_init(&backend);
	assert(test_state_init(&mmio, &backend, INTEL_AX211_MMIO_MAC_SO) ==
	    INTEL_AX211_MMIO_OK);
	memcpy(address, unchanged, sizeof(address));
	assert(intel_ax211_mmio_read_mac(&mmio, address) ==
	    INTEL_AX211_MMIO_ORDER);
	assert(memcmp(address, unchanged, sizeof(address)) == 0);

	/* Zero strap plus broadcast OTP is terminal and transactional. */
	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	backend.csr[TEST_CSR_MAC_ADDRESS0_OTP / 4U] = 0xffffffffU;
	backend.csr[TEST_CSR_MAC_ADDRESS1_OTP / 4U] = 0x0000ffffU;
	memcpy(address, unchanged, sizeof(address));
	assert(intel_ax211_mmio_read_mac(&mmio, address) ==
	    INTEL_AX211_MMIO_INVALID);
	assert(memcmp(address, unchanged, sizeof(address)) == 0);
	assert(mmio.nic_lock_depth == 0U);

	/* A CSR read fault is not mistaken for an invalid-address fallback. */
	backend.fail_csr_read = 1;
	backend.fail_csr_read_offset = TEST_CSR_MAC_ADDRESS0_STRAP;
	memcpy(address, unchanged, sizeof(address));
	assert(intel_ax211_mmio_read_mac(&mmio, address) ==
	    INTEL_AX211_MMIO_IO);
	assert(memcmp(address, unchanged, sizeof(address)) == 0);
	assert(mmio.nic_lock_depth == 0U);

	/* A failed outermost unlock invalidates readiness and output together. */
	backend.fail_csr_read = 0;
	backend.csr[TEST_CSR_MAC_ADDRESS0_STRAP / 4U] = 0x02112233U;
	backend.csr[TEST_CSR_MAC_ADDRESS1_STRAP / 4U] = 0x00004455U;
	backend.fail_access_clear = 1;
	memcpy(address, unchanged, sizeof(address));
	assert(intel_ax211_mmio_read_mac(&mmio, address) ==
	    INTEL_AX211_MMIO_IO);
	assert(memcmp(address, unchanged, sizeof(address)) == 0);
	assert(mmio.nic_lock_depth == 0U);
	assert(!mmio.apm_ready);
}

/* Proves the exact Gen3 CSR/ownership/LTR/UMAC publication order. */
static void
test_gen3_publication(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_mmio_boot boot;
	size_t position;

	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	backend.event_count = 0U;
	boot.context_address = UINT64_C(0x1122334455667788);
	boot.iml_address = UINT64_C(0x8877665544332211);
	boot.iml_size = INTEL_AX211_MMIO_IML_SIZE;
	assert(intel_ax211_mmio_publish_gen3(&mmio, &boot) ==
	    INTEL_AX211_MMIO_OK);

	position = test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_CTXT_INFO_ADDR, 0x55667788U) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_CTXT_INFO_ADDR + 4U, 0x11223344U) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_IML_DATA_ADDR, 0x44332211U) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_IML_DATA_ADDR + 4U, 0x88776655U) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_IML_SIZE_ADDR, INTEL_AX211_MMIO_IML_SIZE) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_HW_IF_CONFIG_REG, TEST_HW_IF_HAP_WAKE_L1A |
	    TEST_HW_IF_NIC_READY | TEST_AUTO_FUNC_BOOT) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_GP_CNTRL, TEST_GP_INIT_DONE |
	    TEST_GP_MAC_CLOCK_READY | TEST_GP_MAC_ACCESS_REQ) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_LTR_LONG_VAL_AD, TEST_LTR_BOOTSTRAP) + 1U;
	position = test_find_event(&backend, position, TEST_EVENT_PRPH_WRITE,
	    TEST_UMAC_CPU_INIT_RUN, 1U) + 1U;
	assert(position <= backend.event_count);
	assert(backend.prph_writes == 1U);
	assert(mmio.nic_lock_depth == 0U);

	backend.event_count = 0U;
	boot.iml_size--;
	assert(intel_ax211_mmio_publish_gen3(&mmio, &boot) ==
	    INTEL_AX211_MMIO_INVALID);
	assert(backend.event_count == 0U);
}

/* Proves that failed ownership cannot reach LTR or UMAC PRPH access. */
static void
test_gen3_failed_ownership(void)
{
	struct test_backend backend;
	struct intel_ax211_mmio mmio;
	struct intel_ax211_mmio_boot boot;

	test_backend_init(&backend);
	test_reach_apm(&mmio, &backend);
	backend.going_to_sleep = 1;
	backend.event_count = 0U;
	boot.context_address = 0x1000U;
	boot.iml_address = 0x2000U;
	boot.iml_size = INTEL_AX211_MMIO_IML_SIZE;
	assert(intel_ax211_mmio_publish_gen3(&mmio, &boot) ==
	    INTEL_AX211_MMIO_TIMEOUT);
	assert(test_find_event(&backend, 0U, TEST_EVENT_CSR_WRITE,
	    TEST_CSR_LTR_LONG_VAL_AD, TEST_LTR_BOOTSTRAP) ==
	    backend.event_count);
	assert(test_count_event(&backend, TEST_EVENT_PRPH_WRITE) == 0U);
	assert(mmio.nic_lock_depth == 0U);

	backend.going_to_sleep = 0;
	backend.fail_csr_write = 1;
	backend.fail_csr_write_offset = TEST_CSR_CTXT_INFO_ADDR;
	backend.event_count = 0U;
	assert(intel_ax211_mmio_publish_gen3(&mmio, &boot) ==
	    INTEL_AX211_MMIO_IO);
	assert(backend.event_count == 1U);
}
