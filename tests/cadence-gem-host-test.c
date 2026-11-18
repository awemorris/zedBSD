/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Runs the Cadence GEM driver on the host against a fake controller, fake
 * PHYs and fake DMA memory.
 *
 * "-t" runs the Raspberry Pi 5's Ethernet glue through attachment, open,
 * transmit, receive, interrupts, link changes, a DMA error and close, and
 * prints a trace of everything the drivers did that the hardware or the
 * rest of the kernel could observe: every register access, the GPIO, the
 * delays, the interrupt routing, the DMA allocations, the locks and
 * barriers, the log, the descriptors and the interface.  The build compares
 * that trace with the one the driver gave before its generic part was
 * split from the glue, so the Pi 5 sees the same accesses in the same
 * order.  It runs each case in a child process, as the glue attaches once.
 *
 * Without "-t" it checks the driver's other shapes: 32-bit descriptors with
 * no access to the upper-address registers, the receive restart and its
 * interrupt mask, the RGMII selection, the PHY scan and the MDIO frames,
 * a configuration it must refuse, and the unit limit.
 *
 * The build copies each driver with its include lines removed and compiles
 * the copies with cadence-gem-host-test.h in front of them.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cadence-gem-host-test.h"

/*
 * The controller's registers and bits the fake models or the checks look
 * at.  The values are the ones the driver uses.
 */
#define FAKE_NETCTL			0x0000U
#define FAKE_NETCTL_RXEN		(1U << 2)
#define FAKE_NETCFG			0x0004U
#define FAKE_NETCFG_FDEN		(1U << 1)
#define FAKE_NETCFG_100			(1U << 0)
#define FAKE_NETCFG_1000		(1U << 10)
#define FAKE_NETSR			0x0008U
#define FAKE_NETSR_IDLE			(1U << 2)
#define FAKE_USRIO			0x000cU
#define FAKE_DMACR			0x0010U
#define FAKE_DMACR_DMA64		(1U << 30)
#define FAKE_ISR			0x0024U
#define FAKE_IER			0x0028U
#define FAKE_IDR			0x002cU
#define FAKE_IXR_HRESP			(1U << 11)
#define FAKE_IXR_RXOVR			(1U << 10)
#define FAKE_IXR_TXDONE			(1U << 7)
#define FAKE_IXR_RXUBR			(1U << 2)
#define FAKE_IXR_RXDONE			(1U << 1)
#define FAKE_PHYMNTNC			0x0034U
#define FAKE_LADDRL			0x0088U
#define FAKE_LADDRH			0x008cU
#define FAKE_MID			0x00fcU
#define FAKE_CFG6			0x0294U
#define FAKE_CFG8			0x029cU
#define FAKE_TXQBASEHI			0x04c8U
#define FAKE_RXQBASEHI			0x04d4U

/* The descriptor bits the fake controller sets and the checks expect. */
#define FAKE_RXD_USED			(1U << 0)
#define FAKE_RXD_WRAP			(1U << 1)
#define FAKE_RXD_SOF			(1U << 14)
#define FAKE_RXD_EOF			(1U << 15)
#define FAKE_TXD_USED			(1U << 31)
#define FAKE_TXD_WRAP			(1U << 30)
#define FAKE_TXD_RLIMIT			(1U << 29)
#define FAKE_TXD_LAST			(1U << 15)

/* The rings' geometry the driver uses. */
#define FAKE_RING_COUNT			256U
#define FAKE_PACKET_BYTES		2048U

/* RP1's clock controller registers the glue reads and writes. */
#define FAKE_CLK_ETH_CTRL		0x0064U
#define FAKE_CLK_ETH_TSU_CTRL		0x0134U
#define FAKE_CLK_ENABLE			(1U << 11)

/* The fake device tree's nodes and the PHY's phandle. */
#define NODE_GEM			0x100U
#define NODE_PHY			0x200U
#define NODE_CLOCKS			0x300U
#define PHANDLE_PHY			1U

/* The PHY models the fake bus answers with. */
#define PHY_ABSENT			0
#define PHY_BCM54213PE			1
#define PHY_GENERIC			2

/* The most register accesses one case records for the checks. */
#define ACCESS_MAX			65536U

/* The most DMA allocations one case makes. */
#define ALLOCATION_MAX			8U

/* The bytes after each DMA allocation that the driver must never write, and their pattern. */
#define GUARD_BYTES			64U
#define GUARD_PATTERN			0xa5U

/* A word the driver must leave as the test put it. */
#define SENTINEL			0xdeadbee0U

/* The PHY registers a reset leaves, and the reset bit. */
#define PHY_CONTROL_RESET		0x8000U
#define PHY_CONTROL_DEFAULT		0x1140U

/* One register access, for the checks. */
struct access {
	int write;
	unsigned offset;
	uint32_t value;
};

/* One fake PHY: which model answers at its address, and its registers. */
struct fake_phy {
	int model;
	uint16_t registers[32];
};

/* One device tree property of the fake tree. */
struct fake_property {
	uint32_t node;
	const char *name;
	const uint8_t *value;
	uint32_t length;
};

/* What the test's own PHY hook saw. */
struct hook_record {
	unsigned setup_calls;
	unsigned setup_phy;
	uint32_t setup_identity;
	uint32_t replace_identity;
	unsigned tx_clock_calls;
	unsigned tx_clock_speed;
	void (*handler)(void *argument);
	void *argument;
};

/* The controller's registers, the word at each offset. */
static uint32_t gem_registers[0x1000U / 4U];

/* RP1's clock controller's registers. */
static uint32_t clock_registers[0x200U / 4U];

/* The interrupt causes the controller has raised and the driver not cleared. */
static uint32_t pending_interrupts;

/* The PHYs on the controller's MDIO bus, by address. */
static struct fake_phy phys[32];

/* The fake monotonic counter and the scheduler's tick. */
static uint64_t fake_counter;
static uint64_t fake_ticks;

/* Whether the tree carries the board's address, and which. */
static int tree_has_mac;
static uint8_t tree_mac[6];

/* The interrupt handler the driver routed. */
static drv_rp1_irq_handler_t routed_handler;
static void *routed_argument;

/* The one interface the fake hands out, and whether it was taken. */
static struct net_device fake_net;
static int fake_net_taken;

/* The one service thread the fake hands out. */
static struct thread fake_thread;

/* The DMA devices the fake created, and the allocations in order. */
static struct drv_dma_device fake_dma_devices[4];
static unsigned fake_dma_device_count;
static struct drv_dma_buffer allocations[ALLOCATION_MAX];
static unsigned allocation_count;

/* Whether the trace is printed, and the register accesses recorded. */
static int trace_printing;
static struct access accesses[ACCESS_MAX];
static unsigned access_count;

/* The last log line, for the checks. */
static char log_lines[64][160];
static unsigned log_count;

/* How many checks failed. */
static unsigned failures;

/* The fake tree's property values, big-endian cells. */
static const uint8_t property_phy_mode[] = "rgmii-id";
static const uint8_t property_phy_handle[] = { 0, 0, 0, PHANDLE_PHY };
static const uint8_t property_phy_reg[] = { 0, 0, 0, 1 };
static const uint8_t property_reset_gpios[] = { 0, 0, 0, 9, 0, 0, 0, 32, 0, 0, 0, 1 };
static const uint8_t property_reset_duration[] = { 0, 0, 0, 5 };
static const uint8_t property_assigned_clocks[] = { 0, 0, 0, 5, 0, 0, 0, 6, 0, 0, 0, 5, 0, 0, 0, 12 };
static const uint8_t property_assigned_rates[] = { 0x02, 0xfa, 0xf0, 0x80, 0x0b, 0xeb, 0xc2, 0x00 };

/* The fake tree, apart from the board's address. */
static const struct fake_property fake_tree[] = {
	{ NODE_GEM, "phy-mode", property_phy_mode, sizeof(property_phy_mode) },
	{ NODE_GEM, "phy-handle", property_phy_handle, sizeof(property_phy_handle) },
	{ NODE_GEM, "phy-reset-gpios", property_reset_gpios, sizeof(property_reset_gpios) },
	{ NODE_GEM, "phy-reset-duration", property_reset_duration, sizeof(property_reset_duration) },
	{ NODE_PHY, "reg", property_phy_reg, sizeof(property_phy_reg) },
	{ NODE_CLOCKS, "assigned-clocks", property_assigned_clocks, sizeof(property_assigned_clocks) },
	{ NODE_CLOCKS, "assigned-clock-rates", property_assigned_rates, sizeof(property_assigned_rates) }
};

/* The test's PHY hook's record. */
static struct hook_record hooks;

static void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void record_access(int write, unsigned offset, uint32_t value);
static uint16_t phy_read(unsigned address, unsigned reg);
static void phy_write(unsigned address, unsigned reg, uint16_t data);
static void reset_fakes(void);
static void prepare_rp1(int with_tree_mac, int with_filter_mac);
static void run_rp1_case(const char *name, int with_tree_mac, int with_filter_mac);
static void exercise_rp1(void);
static void dump_descriptors(const char *label, unsigned allocation, unsigned words, unsigned first, unsigned count);
static void dump_digest(const char *label, unsigned allocation);
static struct packet_buf *make_frame(size_t length, uint8_t seed);
static void fill_received(unsigned words, unsigned slot, uint32_t status, size_t length, uint8_t seed);
static void check(int condition, const char *what);
static int log_contains(const char *text);
static unsigned count_accesses(unsigned offset);
static int find_write(unsigned from, unsigned offset, uint32_t value);
static int last_write(unsigned offset, uint32_t *value);
static int guard_intact(unsigned allocation);
static int test_setup_phy(void *context, const struct drv_mii_bus *bus, unsigned phy, uint32_t *identity);
static void test_set_tx_clock(void *context, unsigned speed_mbps);
static int test_establish_irq(void *context, void (*handler)(void *argument), void *argument);
static void check_32_bit(void);
static void check_64_bit_and_limits(void);

/*
 * Prints the Pi 5's trace with "-t", or runs the checks of the driver's
 * other shapes.
 */
int
main(
	int argc,
	char **argv)
{
	int comparison;

	/* Asks whether the trace is wanted. */
	comparison = 1;
	if (argc > 1)
		comparison = strcmp(argv[1], "-t");

	/* Prints the Pi 5's trace, one case per child. */
	if (comparison == 0) {
		trace_printing = 1;
		run_rp1_case("tree address", 1, 0);
		run_rp1_case("filter address", 0, 1);
		run_rp1_case("no address", 0, 0);
		return 0;
	}

	/* Checks the 32-bit shape, then the 64-bit one and the limits. */
	check_32_bit();
	check_64_bit_and_limits();

	/* Reports the outcome. */
	if (failures != 0) {
		printf("cadence-gem-host-test: FAIL (%u)\n", failures);
		return 1;
	}

	/* Succeeded: every check held. */
	printf("cadence-gem-host-test: PASS\n");
	return 0;
}

/* Prints one trace line when the trace is wanted. */
static void
trace(
	const char *format,
	...)
{
	va_list arguments;

	/* Leaves the checks' runs quiet. */
	if (!trace_printing)
		return;

	/* Prints the line. */
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	putchar('\n');
}

/* Records one controller register access for the checks. */
static void
record_access(
	int write,
	unsigned offset,
	uint32_t value)
{
	/* Drops what does not fit; no case comes near the bound. */
	if (access_count == ACCESS_MAX)
		return;

	/* Appends the access. */
	accesses[access_count].write = write;
	accesses[access_count].offset = offset;
	accesses[access_count].value = value;
	access_count++;
}

/* Answers an MDIO read as the PHY at the address would. */
static uint16_t
phy_read(
	unsigned address,
	unsigned reg)
{
	struct fake_phy *phy;
	uint16_t data;

	/* An address with no PHY reads as all ones. */
	phy = &phys[address];
	if (phy->model == PHY_ABSENT)
		return 0xffffU;

	/* A reset in progress reads once as still running, then as done. */
	data = phy->registers[reg];
	if (reg == 0 && (data & PHY_CONTROL_RESET) != 0)
		phy->registers[0] = PHY_CONTROL_DEFAULT;

	/* Reports the register. */
	return data;
}

/* Takes an MDIO write as the PHY at the address would. */
static void
phy_write(
	unsigned address,
	unsigned reg,
	uint16_t data)
{
	/* An address with no PHY drops the write. */
	if (phys[address].model == PHY_ABSENT)
		return;

	/* Stores the register. */
	phys[address].registers[reg] = data;
}

/* Starts a case from a quiet controller, bus, tree and kernel. */
static void
reset_fakes(
	void)
{
	unsigned index;

	/* Empties the controller, the clocks and the PHYs. */
	memset(gem_registers, 0, sizeof(gem_registers));
	memset(clock_registers, 0, sizeof(clock_registers));
	memset(phys, 0, sizeof(phys));
	pending_interrupts = 0;
	gem_registers[FAKE_NETSR / 4U] = FAKE_NETSR_IDLE;

	/* Empties the kernel's side. */
	fake_counter = 0;
	fake_ticks = 1000U;
	tree_has_mac = 0;
	routed_handler = NULL;
	routed_argument = NULL;
	memset(&fake_net, 0, sizeof(fake_net));
	fake_net_taken = 0;
	fake_dma_device_count = 0;
	allocation_count = 0;
	access_count = 0;
	log_count = 0;
	memset(&hooks, 0, sizeof(hooks));

	/* Gives back the last case's DMA memory. */
	for (index = 0; index < ALLOCATION_MAX; index++) {
		free(allocations[index].address);
		allocations[index].address = NULL;
	}
}

/*
 * Prepares the Pi 5's controller as the firmware leaves it: version 7 with
 * three priority queues and screeners, the Ethernet clock off, a BCM54213PE
 * at address 1 with a gigabit link, and the board's address in the tree,
 * in a filter, or nowhere.
 */
static void
prepare_rp1(
	int with_tree_mac,
	int with_filter_mac)
{
	/* Describes the controller's version, queues, screeners and DMA width. */
	reset_fakes();
	gem_registers[FAKE_MID / 4U] = 0x00070109U;
	gem_registers[FAKE_CFG6 / 4U] = (1U << 23) | 0x0206U;
	gem_registers[FAKE_CFG8 / 4U] = (2U << 24) | (1U << 16);
	gem_registers[FAKE_NETCFG / 4U] = 0x00080000U;
	gem_registers[FAKE_DMACR / 4U] = 0x00020784U;

	/* Leaves the board's address in the third filter, behind an empty and a multicast one. */
	if (with_filter_mac) {
		gem_registers[(FAKE_LADDRL + 8U) / 4U] = 0x00005e01U;
		gem_registers[(FAKE_LADDRH + 8U) / 4U] = 0x00000001U;
		gem_registers[(FAKE_LADDRL + 16U) / 4U] = 0x01dd3ad8U;
		gem_registers[(FAKE_LADDRH + 16U) / 4U] = 0x00000302U;
	}

	/* Leaves the Ethernet clock off and the timestamp clock on. */
	clock_registers[FAKE_CLK_ETH_CTRL / 4U] = 0x00000000U;
	clock_registers[FAKE_CLK_ETH_TSU_CTRL / 4U] = FAKE_CLK_ENABLE;

	/* Puts the BCM54213PE at address 1 with a gigabit link. */
	phys[1].model = PHY_BCM54213PE;
	phys[1].registers[0] = PHY_CONTROL_DEFAULT;
	phys[1].registers[1] = 0x796dU;
	phys[1].registers[2] = 0x600dU;
	phys[1].registers[3] = 0x84a2U;
	phys[1].registers[0x19] = 0x0700U;

	/* Writes the board's address into the tree. */
	tree_has_mac = with_tree_mac;
	tree_mac[0] = 0xd8U;
	tree_mac[1] = 0x3aU;
	tree_mac[2] = 0xddU;
	tree_mac[3] = 0x11U;
	tree_mac[4] = 0x22U;
	tree_mac[5] = 0x33U;
}

/* Runs one Pi 5 case in a child, so the glue attaches from the start. */
static void
run_rp1_case(
	const char *name,
	int with_tree_mac,
	int with_filter_mac)
{
	pid_t child;
	int status;

	/* Separates the case's trace from the one before. */
	fflush(stdout);
	printf("== case: %s\n", name);
	fflush(stdout);

	/* Runs the case in a fresh copy of the process. */
	child = fork();
	if (child == 0) {
		prepare_rp1(with_tree_mac, with_filter_mac);
		exercise_rp1();
		fflush(stdout);
		_exit(0);
	}

	/* Waits for the case to end before the next one starts. */
	if (child > 0)
		(void)waitpid(child, &status, 0);
}

/*
 * Drives the Pi 5's interface through its life: attachment, open,
 * transmit, the controller's completions, an interrupt, a receive batch,
 * link changes, a DMA error, close and a second open.
 */
static void
exercise_rp1(
	void)
{
	struct net_device *net;
	struct packet_buf *packet;
	uint32_t *descriptors;
	unsigned count;
	int error;

	/* Attaches the interface, then asks again. */
	error = drv_rp1_ethernet_init();
	trace("init %d", error);
	if (error != 0)
		return;
	error = drv_rp1_ethernet_init();
	trace("init again %d", error);

	/* Starts the periodic service twice; the second start does nothing. */
	drv_rp1_ethernet_refresh();
	drv_rp1_ethernet_refresh();

	/* Opens the interface and shows the rings it handed the controller. */
	net = &fake_net;
	error = net->ops->open(net);
	trace("open %d", error);
	dump_digest("rings", 0);
	dump_descriptors("rx", 0, 4U, 0, 2U);
	dump_descriptors("rx", 0, 4U, FAKE_RING_COUNT - 1U, 2U);
	dump_descriptors("tx", 0, 4U, FAKE_RING_COUNT + 1U, 2U);
	dump_descriptors("tx", 0, 4U, 2U * FAKE_RING_COUNT, 2U);

	/* Sends a short frame, a full one, and two the driver must refuse. */
	packet = make_frame(42U, 1U);
	error = net->ops->transmit(net, packet);
	trace("transmit 42: %d", error);
	packet = make_frame(1514U, 2U);
	error = net->ops->transmit(net, packet);
	trace("transmit 1514: %d", error);
	packet = make_frame(1515U, 3U);
	error = net->ops->transmit(net, packet);
	trace("transmit 1515: %d", error);
	packet = make_frame(10U, 4U);
	error = net->ops->transmit(net, packet);
	trace("transmit 10: %d", error);
	error = net->ops->transmit(net, NULL);
	trace("transmit none: %d", error);
	dump_descriptors("tx", 0, 4U, FAKE_RING_COUNT + 1U, 3U);
	dump_digest("tx slots", 2);

	/* Has the controller send both frames, the second given up on. */
	descriptors = allocations[0].address;
	descriptors[(FAKE_RING_COUNT + 1U) * 4U + 1U] |= FAKE_TXD_USED;
	descriptors[(FAKE_RING_COUNT + 2U) * 4U + 1U] |= FAKE_TXD_USED | FAKE_TXD_RLIMIT;

	/* Has the controller receive a whole frame and a broken one. */
	fill_received(4U, 0, FAKE_RXD_SOF | FAKE_RXD_EOF, 64U, 5U);
	fill_received(4U, 1U, FAKE_RXD_SOF, 100U, 6U);

	/* Raises receive, transmit and overrun, and serves the interrupt. */
	pending_interrupts = FAKE_IXR_RXDONE | FAKE_IXR_TXDONE | FAKE_IXR_RXOVR;
	routed_handler(routed_argument);

	/* Runs the network worker's pass. */
	count = net->ops->poll_receive(net, 8U);
	trace("poll %u rx_errors %llu rx_dropped %llu tx_errors %llu",
	      count,
	      (unsigned long long)net->rx_errors,
	      (unsigned long long)net->rx_dropped,
	      (unsigned long long)net->tx_errors);
	dump_descriptors("rx", 0, 4U, 0, 3U);

	/* Drops the link to 100 Mbit/s and lets the next pass see it. */
	phys[1].registers[0x19] = 0x0500U;
	fake_ticks += 1000U;
	count = net->ops->poll_receive(net, 8U);
	trace("poll %u link %u", count, net->link_mbps);

	/* Samples again too soon: the PHY is not read. */
	count = net->ops->poll_receive(net, 8U);
	trace("poll %u link %u", count, net->link_mbps);

	/* Pulls the cable. */
	phys[1].registers[1] = 0x7949U;
	fake_ticks += 1000U;
	count = net->ops->poll_receive(net, 8U);
	trace("poll %u link %u", count, net->link_mbps);

	/* Raises a DMA error. */
	pending_interrupts = FAKE_IXR_HRESP;
	routed_handler(routed_argument);

	/* Closes, reopens with the link back, and closes again. */
	net->ops->close(net);
	phys[1].registers[1] = 0x796dU;
	fake_ticks += 1000U;
	error = net->ops->open(net);
	trace("reopen %d", error);
	dump_digest("rings", 0);
	net->ops->close(net);
}

/* Prints a run of descriptors' words. */
static void
dump_descriptors(
	const char *label,
	unsigned allocation,
	unsigned words,
	unsigned first,
	unsigned count)
{
	const uint32_t *descriptors;
	unsigned index;
	unsigned word;
	char line[160];
	size_t used;

	/* Prints each descriptor on its own line. */
	descriptors = allocations[allocation].address;
	for (index = first; index < first + count; index++) {
		used = (size_t)snprintf(line, sizeof(line), "desc %s %u:", label, index);
		for (word = 0; word < words; word++)
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %08x", descriptors[index * words + word]);
		trace("%s", line);
	}
}

/* Prints an FNV-1a digest of one allocation's bytes. */
static void
dump_digest(
	const char *label,
	unsigned allocation)
{
	const uint8_t *bytes;
	uint64_t digest;
	size_t index;

	/* Folds every byte in. */
	bytes = allocations[allocation].address;
	digest = 0xcbf29ce484222325ULL;
	for (index = 0; index < allocations[allocation].size; index++) {
		digest ^= bytes[index];
		digest *= 0x100000001b3ULL;
	}

	/* Prints it. */
	trace("digest %s %016llx", label, (unsigned long long)digest);
}

/* Builds a frame of a length, its bytes counting up from a seed. */
static struct packet_buf *
make_frame(
	size_t length,
	uint8_t seed)
{
	struct packet_buf *packet;
	uint8_t *payload;
	size_t index;

	/* Takes a packet and fills it. */
	packet = packet_buf_alloc(PACKET_BUF_DEFAULT_HEADROOM);
	payload = packet_buf_append(packet, length);
	for (index = 0; index < length; index++)
		payload[index] = (uint8_t)(seed + index);

	/* Succeeded: the frame. */
	return packet;
}

/* Has the fake controller write a frame into a receive slot and hand it back. */
static void
fill_received(
	unsigned words,
	unsigned slot,
	uint32_t status,
	size_t length,
	uint8_t seed)
{
	uint32_t *descriptors;
	uint8_t *payload;
	size_t index;

	/* Writes the frame into the slot. */
	payload = (uint8_t *)allocations[1].address + (size_t)slot * FAKE_PACKET_BYTES;
	for (index = 0; index < length; index++)
		payload[index] = (uint8_t)(seed + index);

	/* Writes the status and gives the descriptor back to the driver. */
	descriptors = allocations[0].address;
	descriptors[slot * words + 1U] = status | (uint32_t)length;
	descriptors[slot * words] |= FAKE_RXD_USED;
}

/* Counts a failed check and says which. */
static void
check(
	int condition,
	const char *what)
{
	/* A check that held says nothing. */
	if (condition)
		return;

	/* Reports the failure. */
	printf("FAIL: %s\n", what);
	failures++;
}

/* Says whether a log line of the case contains a text. */
static int
log_contains(
	const char *text)
{
	const char *match;
	unsigned index;

	/* Looks through the lines. */
	for (index = 0; index < log_count; index++) {
		match = strstr(log_lines[index], text);
		if (match != NULL)
			return 1;
	}

	/* No line has it. */
	return 0;
}

/* Counts the accesses of a case to one register. */
static unsigned
count_accesses(
	unsigned offset)
{
	unsigned index;
	unsigned count;

	/* Counts reads and writes alike. */
	count = 0;
	for (index = 0; index < access_count; index++) {
		if (accesses[index].offset == offset)
			count++;
	}

	/* Reports the count. */
	return count;
}

/* Finds a write of a value to a register from an access on, or -1. */
static int
find_write(
	unsigned from,
	unsigned offset,
	uint32_t value)
{
	unsigned index;

	/* Looks forward from the access. */
	for (index = from; index < access_count; index++) {
		if (accesses[index].write &&
		    accesses[index].offset == offset &&
		    accesses[index].value == value)
			return (int)index;
	}

	/* No such write. */
	return -1;
}

/* Finds the value last written to a register; zero when none was. */
static int
last_write(
	unsigned offset,
	uint32_t *value)
{
	unsigned index;

	/* Looks backward from the last access. */
	for (index = access_count; index > 0; index--) {
		if (accesses[index - 1U].write && accesses[index - 1U].offset == offset) {
			*value = accesses[index - 1U].value;
			return 1;
		}
	}

	/* The register was never written. */
	return 0;
}

/* Says whether the bytes after an allocation still hold the guard pattern. */
static int
guard_intact(
	unsigned allocation)
{
	const uint8_t *guard;
	unsigned index;

	/* Looks at every guard byte. */
	guard = (const uint8_t *)allocations[allocation].address + allocations[allocation].size;
	for (index = 0; index < GUARD_BYTES; index++) {
		if (guard[index] != GUARD_PATTERN)
			return 0;
	}

	/* The guard is as the allocation left it. */
	return 1;
}

/* Records the PHY the driver found and hands back the identity it was given. */
static int
test_setup_phy(
	void *context,
	const struct drv_mii_bus *bus,
	unsigned phy,
	uint32_t *identity)
{
	struct hook_record *record;

	/* Records the call. */
	record = context;
	(void)bus;
	record->setup_calls++;
	record->setup_phy = phy;
	record->setup_identity = *identity;

	/* Replaces the identity when the case asks for it. */
	if (record->replace_identity != 0)
		*identity = record->replace_identity;

	/* Succeeded: the PHY is up. */
	return 0;
}

/* Records the speed the transmit clock was set for. */
static void
test_set_tx_clock(
	void *context,
	unsigned speed_mbps)
{
	struct hook_record *record;

	/* Records the call. */
	record = context;
	record->tx_clock_calls++;
	record->tx_clock_speed = speed_mbps;
}

/* Records the interrupt handler the driver routes. */
static int
test_establish_irq(
	void *context,
	void (*handler)(void *argument),
	void *argument)
{
	struct hook_record *record;

	/* Records the handler. */
	record = context;
	record->handler = handler;
	record->argument = argument;

	/* Succeeded: the handler is routed. */
	return 0;
}

/*
 * Checks a version-2 controller with 32-bit descriptors, the receive
 * restart and RGMII: no access to the upper-address registers, no 64-bit
 * DMA bit, two-word descriptors, the PHY found by a scan, the generic link
 * read, the transmit clock, and the receive interrupt mask.
 */
static void
check_32_bit(
	void)
{
	struct drv_cadence_gem_config config;
	struct drv_dma_constraints constraints;
	struct drv_dma_device *dma;
	struct drv_cadence_gem *gem;
	struct net_device *net;
	struct packet_buf *packet;
	const uint32_t *descriptors;
	uint64_t address;
	uint32_t expected;
	uint32_t value;
	unsigned index;
	unsigned count;
	unsigned mark;
	int written;
	int found;
	int error;

	/* Prepares a version-2 controller and a generic PHY at address 7 only. */
	reset_fakes();
	gem_registers[FAKE_MID / 4U] = 0x00020118U;
	phys[7].model = PHY_GENERIC;
	phys[7].registers[0] = PHY_CONTROL_DEFAULT;
	phys[7].registers[1] = 0x796dU;
	phys[7].registers[2] = 0x0141U;
	phys[7].registers[3] = 0x0cc2U;
	phys[7].registers[4] = 0x01e1U;
	phys[7].registers[5] = 0x45e1U;
	phys[7].registers[9] = 0x0200U;
	phys[7].registers[10] = 0x0800U;
	phys[7].registers[15] = 0x3000U;

	/* Creates the controller's 32-bit DMA device. */
	memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 32U;
	constraints.max_segment_size = FAKE_RING_COUNT * FAKE_PACKET_BYTES;
	error = drv_dma_device_create(&constraints, &dma);
	check(error == 0, "32-bit DMA device");

	/* Describes the controller. */
	memset(&config, 0, sizeof(config));
	config.name = "en0";
	config.registers = (volatile uint8_t *)gem_registers;
	config.descriptor_width = DRV_CADENCE_GEM_DESCRIPTOR_32;
	config.dma = dma;
	config.pclk_hz = 108333333U;
	config.phy_address = 0;
	config.phy_scan = 1;
	config.mac[0] = 0x02U;
	config.mac[1] = 0x5aU;
	config.mac[2] = 0x59U;
	config.mac[3] = 0x42U;
	config.mac[4] = 0x4fU;
	config.mac[5] = 0x30U;
	config.flags = DRV_CADENCE_GEM_FLAG_RESTART_RX_ON_USED_BIT | DRV_CADENCE_GEM_FLAG_USRIO_RGMII;
	config.context = &hooks;
	config.setup_phy = test_setup_phy;
	config.set_tx_clock = test_set_tx_clock;
	config.establish_irq = test_establish_irq;

	/* Attaches it. */
	error = drv_cadence_gem_attach(&config, &gem);
	check(error == 0, "32-bit attach");
	if (error != 0)
		return;

	/* The scan asked address 0 first, skipped the silent ones and found 7. */
	found = find_write(0, FAKE_PHYMNTNC, 0x600a0000U);
	check(found >= 0, "MDIO read of address 0 register 2");
	found = find_write(0, FAKE_PHYMNTNC, 0x638a0000U);
	check(found >= 0, "MDIO read of address 7 register 2");
	found = find_write(0, FAKE_PHYMNTNC, 0x640a0000U);
	check(found < 0, "no MDIO read past address 7");
	check(hooks.setup_calls == 1U, "PHY setup called once");
	check(hooks.setup_phy == 7U, "PHY setup at address 7");
	check(hooks.setup_identity == 0x01410cc2U, "PHY setup given the identity read");
	found = log_contains("gem: PHY 7 id 01410cc2");
	check(found, "PHY log line");
	found = log_contains("MDIO divider 48");
	check(found, "MDIO divider for 108 MHz");

	/* The rings are two-word descriptors. */
	check(allocation_count == 3U, "three DMA allocations");
	check(allocations[0].size == 2U * (FAKE_RING_COUNT + 1U) * 8U, "rings of two-word descriptors");
	check(allocations[1].device_address < 0x100000000ULL, "32-bit receive slots");

	/* Opens the interface. */
	net = &fake_net;
	mark = access_count;
	error = net->ops->open(net);
	check(error == 0, "32-bit open");

	/* Every receive descriptor points at its slot, the last one wrapping; the spare closes the ring. */
	descriptors = allocations[0].address;
	found = 1;
	for (index = 0; index < FAKE_RING_COUNT; index++) {
		address = allocations[1].device_address + (uint64_t)index * FAKE_PACKET_BYTES;
		expected = (uint32_t)address;
		if (index == FAKE_RING_COUNT - 1U)
			expected |= FAKE_RXD_WRAP;
		if (descriptors[index * 2U] != expected || descriptors[index * 2U + 1U] != 0)
			found = 0;
	}

	/* Compares the receive ring and its spare descriptor with what was expected. */
	check(found, "receive descriptors");
	check(descriptors[FAKE_RING_COUNT * 2U] == (FAKE_RXD_USED | FAKE_RXD_WRAP), "receive spare descriptor");
	check(descriptors[FAKE_RING_COUNT * 2U + 1U] == 0, "receive spare status");

	/* Every transmit descriptor is the driver's, the last one wrapping; the spare closes the ring. */
	descriptors = (const uint32_t *)allocations[0].address + (FAKE_RING_COUNT + 1U) * 2U;
	found = 1;
	for (index = 0; index < FAKE_RING_COUNT; index++) {
		expected = FAKE_TXD_USED;
		if (index == FAKE_RING_COUNT - 1U)
			expected |= FAKE_TXD_WRAP;
		if (descriptors[index * 2U] != 0 || descriptors[index * 2U + 1U] != expected)
			found = 0;
	}

	/* Compares the transmit ring and its spare descriptor with what was expected. */
	check(found, "transmit descriptors");
	check(descriptors[FAKE_RING_COUNT * 2U + 1U] == (FAKE_TXD_USED | FAKE_TXD_WRAP), "transmit spare descriptor");
	found = guard_intact(0);
	check(found, "nothing written past the rings");

	/* The controller got 32-bit DMA, RGMII and the restart's interrupt. */
	written = last_write(FAKE_DMACR, &value);
	check(written && (value & FAKE_DMACR_DMA64) == 0, "no 64-bit DMA bit");
	written = last_write(FAKE_USRIO, &value);
	check(written && value == 1U, "RGMII selected");
	expected = FAKE_IXR_HRESP | FAKE_IXR_RXOVR | FAKE_IXR_TXDONE | FAKE_IXR_RXDONE | FAKE_IXR_RXUBR;
	found = find_write(mark, FAKE_IER, expected);
	check(found >= 0, "interrupts with the used-bit read");

	/* The generic link read found a full-duplex gigabit link and set the clock. */
	check(net->link_mbps == 1000U, "gigabit link");
	check(hooks.tx_clock_calls == 1U && hooks.tx_clock_speed == 1000U, "transmit clock for gigabit");
	written = last_write(FAKE_NETCFG, &value);
	expected = FAKE_NETCFG_1000 | FAKE_NETCFG_100 | FAKE_NETCFG_FDEN;
	check(written && (value & expected) == expected, "gigabit full duplex in NETCFG");
	found = log_contains("gem: en0 link up 1000 Mbps full duplex");
	check(found, "link log line");

	/* A transmitted frame takes two words and leaves the next descriptor alone. */
	((uint32_t *)descriptors)[2] = SENTINEL;
	packet = make_frame(60U, 7U);
	error = net->ops->transmit(net, packet);
	check(error == 0, "32-bit transmit");
	check(descriptors[0] == (uint32_t)allocations[2].device_address, "transmit address");
	check(descriptors[1] == (60U | FAKE_TXD_LAST), "transmit status");
	check(descriptors[2] == SENTINEL && descriptors[3] == FAKE_TXD_USED, "next transmit descriptor untouched");

	/* A used-bit read restarts receive and holds both receive interrupts. */
	mark = access_count;
	fill_received(2U, 0, FAKE_RXD_SOF | FAKE_RXD_EOF, 60U, 8U);
	pending_interrupts = FAKE_IXR_RXUBR | FAKE_IXR_RXDONE;
	hooks.handler(hooks.argument);
	found = find_write(mark, FAKE_IDR, FAKE_IXR_RXDONE | FAKE_IXR_RXUBR);
	check(found >= 0, "both receive interrupts held");
	found = 0;
	for (index = mark; index + 1U < access_count; index++) {
		if (accesses[index].write &&
		    accesses[index].offset == FAKE_NETCTL &&
		    (accesses[index].value & FAKE_NETCTL_RXEN) == 0 &&
		    accesses[index + 1U].write &&
		    accesses[index + 1U].offset == FAKE_NETCTL &&
		    (accesses[index + 1U].value & FAKE_NETCTL_RXEN) != 0)
			found = 1;
	}

	/* The restart wrote receive off and then on again. */
	check(found, "receive turned off and on");

	/* The worker's pass delivers the frame and unmasks both receive interrupts. */
	mark = access_count;
	count = net->ops->poll_receive(net, 8U);
	check(count == 1U, "one frame received");
	found = find_write(mark, FAKE_IER, FAKE_IXR_RXDONE | FAKE_IXR_RXUBR);
	check(found >= 0, "both receive interrupts unmasked");
	descriptors = allocations[0].address;
	check(descriptors[0] == (uint32_t)allocations[1].device_address, "receive descriptor handed back");

	/* Nothing in the whole case reached the upper-address registers. */
	count = count_accesses(FAKE_RXQBASEHI);
	check(count == 0, "no RXQBASEHI access");
	count = count_accesses(FAKE_TXQBASEHI);
	check(count == 0, "no TXQBASEHI access");
	count = count_accesses(FAKE_CFG6);
	check(count == 0, "no CFG6 access on version 2");
}

/*
 * Checks the 64-bit shape without the Pi 5's glue (the upper-address
 * registers, the 64-bit DMA bit, an identity the PHY hook supplies, a
 * fixed PHY address), then the configurations the driver refuses and the
 * unit limit.
 */
static void
check_64_bit_and_limits(
	void)
{
	struct drv_cadence_gem_config config;
	struct drv_dma_constraints constraints;
	struct drv_dma_device *dma;
	struct drv_cadence_gem *gem;
	struct net_device *net;
	uint32_t value;
	unsigned count;
	int written;
	int found;
	int error;

	/* Prepares a version-7 controller with no PHY on the bus. */
	reset_fakes();
	gem_registers[FAKE_MID / 4U] = 0x00070109U;

	/* Creates a 64-bit DMA device. */
	memset(&constraints, 0, sizeof(constraints));
	constraints.address_bits = 64U;
	error = drv_dma_device_create(&constraints, &dma);
	check(error == 0, "64-bit DMA device");

	/* Describes a controller whose hook knows its PHY. */
	memset(&config, 0, sizeof(config));
	config.name = "en1";
	config.registers = (volatile uint8_t *)gem_registers;
	config.descriptor_width = DRV_CADENCE_GEM_DESCRIPTOR_64;
	config.dma = dma;
	config.pclk_hz = 200000000U;
	config.phy_address = 3U;
	config.mac[0] = 0x02U;
	config.mac[5] = 0x01U;
	config.context = &hooks;
	config.setup_phy = test_setup_phy;
	config.establish_irq = test_establish_irq;
	hooks.replace_identity = 0x12345678U;

	/* Refuses a descriptor width, no registers and no MDIO clock without taking a unit. */
	config.descriptor_width = 48U;
	error = drv_cadence_gem_attach(&config, &gem);
	check(error == EINVAL, "refuses a 48-bit descriptor");
	config.descriptor_width = DRV_CADENCE_GEM_DESCRIPTOR_64;
	config.registers = NULL;
	error = drv_cadence_gem_attach(&config, &gem);
	check(error == EINVAL, "refuses no registers");
	config.registers = (volatile uint8_t *)gem_registers;
	config.pclk_hz = 0;
	error = drv_cadence_gem_attach(&config, &gem);
	check(error == EINVAL, "refuses no MDIO clock");
	config.pclk_hz = 200000000U;
	check(access_count == 0, "a refused configuration touches nothing");

	/* Attaches it: the hook's identity is the one logged, and no MDIO frame was sent. */
	error = drv_cadence_gem_attach(&config, &gem);
	check(error == 0, "64-bit attach");
	check(hooks.setup_phy == 3U && hooks.setup_identity == 0, "PHY setup at the fixed address, nothing read");
	count = count_accesses(FAKE_PHYMNTNC);
	check(count == 0, "no MDIO frame without a scan");
	found = log_contains("gem: PHY 3 id 12345678");
	check(found, "PHY log line of the hook's identity");
	check(allocations[0].size == 2U * (FAKE_RING_COUNT + 1U) * 16U, "rings of four-word descriptors");

	/* Opens it: the upper-address registers and the 64-bit DMA bit are written. */
	net = &fake_net;
	error = net->ops->open(net);
	check(error == 0, "64-bit open");
	written = last_write(FAKE_RXQBASEHI, &value);
	check(written && value == (uint32_t)(allocations[0].device_address >> 32), "RXQBASEHI");
	written = last_write(FAKE_DMACR, &value);
	check(written && (value & FAKE_DMACR_DMA64) != 0, "64-bit DMA bit");
	count = count_accesses(FAKE_USRIO);
	check(count == 0, "no user I/O write without the flag");

	/* The second unit is the last one. */
	error = drv_cadence_gem_attach(&config, &gem);
	check(error == EBUSY, "no third unit");
}

/*
 * Starts a lock free.
 */
void
spin_init(
	struct spinlock *lock,
	enum lock_rank rank,
	const char *name)
{
	/* Starts the lock free; the name is the driver's own business. */
	(void)rank;
	(void)name;
	lock->held = 0;
}

/*
 * Takes a lock and records the critical section's start.
 */
unsigned long
spin_lock_irqsave(
	struct spinlock *lock)
{
	/* Records the critical section's start. */
	lock->held = 1;
	trace("lock");
	return 0;
}

/*
 * Releases a lock and records the critical section's end.
 */
void
spin_unlock_irqrestore(
	struct spinlock *lock,
	unsigned long enabled)
{
	/* Records the critical section's end. */
	(void)enabled;
	lock->held = 0;
	trace("unlock");
}

/*
 * Reads a fake register of the controller or of RP1's clock controller.
 */
uint32_t
kern_mmio_read32(
	const volatile void *address)
{
	uintptr_t where;
	uint32_t value;
	unsigned offset;

	/* Reads RP1's clock controller. */
	where = (uintptr_t)address;
	if (where >= (uintptr_t)clock_registers && where < (uintptr_t)clock_registers + sizeof(clock_registers)) {
		offset = (unsigned)(where - (uintptr_t)clock_registers);
		value = clock_registers[offset / 4U];
		trace("R clk %04x %08x", offset, value);
		return value;
	}

	/* Reads the controller; the interrupt status is the raised causes. */
	offset = (unsigned)(where - (uintptr_t)gem_registers);
	value = gem_registers[offset / 4U];
	if (offset == FAKE_ISR)
		value = pending_interrupts;
	record_access(0, offset, value);
	trace("R gem %04x %08x", offset, value);
	return value;
}

/*
 * Writes a fake register, running an MDIO frame or clearing interrupt causes.
 */
void
kern_mmio_write32(
	volatile void *address,
	uint32_t value)
{
	uintptr_t where;
	unsigned offset;
	unsigned operation;
	unsigned phy;
	unsigned reg;

	/* Writes RP1's clock controller. */
	where = (uintptr_t)address;
	if (where >= (uintptr_t)clock_registers && where < (uintptr_t)clock_registers + sizeof(clock_registers)) {
		offset = (unsigned)(where - (uintptr_t)clock_registers);
		clock_registers[offset / 4U] = value;
		trace("W clk %04x %08x", offset, value);
		return;
	}

	/* Writes the controller. */
	offset = (unsigned)(where - (uintptr_t)gem_registers);
	record_access(1, offset, value);
	trace("W gem %04x %08x", offset, value);

	/* Clears the interrupt causes written. */
	if (offset == FAKE_ISR) {
		pending_interrupts &= ~value;
		return;
	}

	/* Runs an MDIO frame on the fake bus: a read leaves the data in the register. */
	if (offset == FAKE_PHYMNTNC) {
		operation = (value >> 28) & 0x3U;
		phy = (value >> 23) & 0x1fU;
		reg = (value >> 18) & 0x1fU;
		if (operation == 2U) {
			value = (value & ~0xffffU) | phy_read(phy, reg);
		} else if (operation == 1U) {
			phy_write(phy, reg, (uint16_t)value);
		}
	}

	/* Keeps the value; the status register stays idle. */
	gem_registers[offset / 4U] = value;
	gem_registers[FAKE_NETSR / 4U] = FAKE_NETSR_IDLE;
}

/*
 * Records a full I/O barrier.
 */
void
kern_io_barrier(
	void)
{
	/* Records the full barrier. */
	trace("barrier");
}

/*
 * Records a read barrier.
 */
void
kern_io_read_barrier(
	void)
{
	/* Records the read barrier. */
	trace("barrier r");
}

/*
 * Records a write barrier.
 */
void
kern_io_write_barrier(
	void)
{
	/* Records the write barrier. */
	trace("barrier w");
}

/*
 * Keeps a kernel log line for the checks and the trace.
 */
void
kern_logf(
	const char *format,
	...)
{
	va_list arguments;
	char line[160];
	size_t length;

	/* Formats the line without its newline. */
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	length = strlen(line);
	if (length != 0 && line[length - 1U] == '\n')
		line[length - 1U] = 0;

	/* Keeps it for the checks and the trace. */
	if (log_count < 64U) {
		snprintf(log_lines[log_count], sizeof(log_lines[log_count]), "%s", line);
		log_count++;
	}

	/* Prints it in the trace. */
	trace("log %s", line);
}

/*
 * Copies memory with the C library.
 */
void *
kern_memcpy(
	void *destination,
	const void *source,
	size_t length)
{
	/* Copies with the C library. */
	memcpy(destination, source, length);

	/* Succeeded: the destination, as memcpy() reports it. */
	return destination;
}

/*
 * Fills memory with the C library.
 */
void *
kern_memset(
	void *destination,
	int byte,
	size_t length)
{
	/* Fills with the C library. */
	memset(destination, byte, length);

	/* Succeeded: the destination, as memset() reports it. */
	return destination;
}

/*
 * Compares strings with the C library.
 */
int
kern_strcmp(
	const char *left,
	const char *right)
{
	int comparison;

	/* Compares with the C library. */
	comparison = strcmp(left, right);

	/* Reports the order of the two strings. */
	return comparison;
}

/*
 * Measures a string with the C library.
 */
size_t
kern_strlen(
	const char *string)
{
	size_t length;

	/* Measures with the C library. */
	length = strlen(string);

	/* Reports the length. */
	return length;
}

/*
 * Reports the scheduler tick the case set.
 */
uint64_t
clock_ticks(
	void)
{
	/* Reports the tick the case set. */
	return fake_ticks;
}

/*
 * Reads the fake monotonic counter, a microsecond further on each read.
 */
bool
kern_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	/* Moves the counter a microsecond on each read. */
	fake_counter++;
	*counter = fake_counter;
	*frequency_hz = 1000000U;
	return true;
}

/*
 * Records a delay without waiting.
 */
void
kern_usleep_range(
	unsigned min_us,
	unsigned max_us)
{
	/* Records the delay. */
	trace("usleep %u %u", min_us, max_us);
}

/*
 * Reports the scheduler tick the case set.
 */
uint64_t
sched_ticks(
	void)
{
	/* Reports the tick the case set. */
	return fake_ticks;
}

/*
 * Returns at once; the service thread never runs on the host.
 */
void
sched_sleep(
	uint64_t timeout_tick)
{
	/* The service thread never runs on the host. */
	(void)timeout_tick;
}

/*
 * Hands out the one service thread without running it.
 */
int
kthread_create(
	void (*entry)(void *),
	void *arg,
	int priority,
	struct thread **result)
{
	/* Hands out the one thread without running it. */
	fake_thread.entry = entry;
	fake_thread.argument = arg;
	*result = &fake_thread;
	trace("kthread %d", priority);
	return 0;
}

/*
 * Records a thread's start.
 */
void
thread_start(
	struct thread *thread)
{
	/* Records the start. */
	(void)thread;
	trace("thread start");
}

/*
 * Creates a fake DMA device.
 */
int
drv_dma_device_create(
	const struct drv_dma_constraints *constraints,
	struct drv_dma_device **result)
{
	struct drv_dma_device *device;

	/* Places a 32-bit device's memory low and another's above 4 GiB. */
	device = &fake_dma_devices[fake_dma_device_count++];
	device->address_bits = constraints->address_bits;
	device->coherent = constraints->coherent;
	device->next_address = 0x10000000ULL;
	if (constraints->address_bits > 32U)
		device->next_address = 0x1080000000ULL;
	*result = device;
	return 0;
}

/*
 * Creates a fake DMA device behind a window onto memory.
 */
int
drv_dma_device_create_window(
	const struct drv_dma_constraints *constraints,
	uint64_t bus_offset,
	uint64_t physical_limit,
	struct drv_dma_device **result)
{
	int error;

	/* Creates the device; the window only moves where its memory appears. */
	(void)physical_limit;
	error = drv_dma_device_create(constraints, result);
	if (error != 0)
		return error;
	(*result)->next_address += bus_offset;
	return 0;
}

/*
 * Allocates fake DMA memory with a guard after it.
 */
int
drv_dma_alloc_coherent(
	struct drv_dma_device *device,
	size_t size,
	size_t alignment,
	struct drv_dma_buffer *buffer)
{
	void *memory;

	/* Takes zeroed host memory at the alignment, with the guard after it. */
	memory = aligned_alloc(alignment, (size + GUARD_BYTES + alignment - 1U) / alignment * alignment);
	if (memory == NULL)
		return ENOMEM;
	memset(memory, 0, size);
	memset((uint8_t *)memory + size, GUARD_PATTERN, GUARD_BYTES);

	/* Gives it the device address next in the device's window. */
	buffer->address = memory;
	buffer->device_address = device->next_address;
	buffer->size = size;
	device->next_address += (size + 0xfffffU) & ~(uint64_t)0xfffffU;

	/* Keeps it for the checks. */
	if (allocation_count < ALLOCATION_MAX)
		allocations[allocation_count++] = *buffer;
	trace("dma %zu align %zu at %llx", size, alignment, (unsigned long long)buffer->device_address);
	return 0;
}

/*
 * Allocates a packet with room for a full frame.
 */
struct packet_buf *
packet_buf_alloc(
	size_t headroom)
{
	struct packet_buf *packet;

	/* Takes a packet with room for a full frame after the headroom. */
	packet = calloc(1U, sizeof(*packet));
	packet->capacity = headroom + 2048U;
	packet->storage = calloc(1U, packet->capacity);
	packet->data = packet->storage + headroom;
	packet->length = 0;
	trace("packet alloc");
	return packet;
}

/*
 * Frees a packet.
 */
void
packet_buf_free(
	struct packet_buf *packet)
{
	/* Accepts no packet, as the kernel does. */
	if (packet == NULL)
		return;

	/* Gives the packet back. */
	free(packet->storage);
	free(packet);
	trace("packet free");
}

/*
 * Grows a packet's frame at its end.
 */
void *
packet_buf_append(
	struct packet_buf *packet,
	size_t length)
{
	void *tail;

	/* Grows the frame at its end. */
	tail = packet->data + packet->length;
	packet->length += length;
	return tail;
}

/*
 * Hands out the one interface.
 */
struct net_device *
net_device_alloc(
	void)
{
	/* Hands out the one interface. */
	fake_net_taken = 1;
	trace("net alloc");
	return &fake_net;
}

/*
 * Records what the driver published.
 */
int
net_device_create(
	struct net_device *device)
{
	/* Records what the driver published. */
	trace("net create %s mtu %u hwaddr %02x:%02x:%02x:%02x:%02x:%02x len %u flags %x",
	      device->name,
	      device->mtu,
	      device->hwaddr[0],
	      device->hwaddr[1],
	      device->hwaddr[2],
	      device->hwaddr[3],
	      device->hwaddr[4],
	      device->hwaddr[5],
	      device->hwaddr_len,
	      device->flags);
	return 0;
}

/*
 * Records the reference the service thread holds.
 */
void
net_device_ref(
	struct net_device *device)
{
	/* Records the reference the service thread holds. */
	(void)device;
	trace("net ref");
}

/*
 * Records the carrier.
 */
int
net_device_set_carrier(
	struct net_device *device,
	int carrier)
{
	/* Records the carrier. */
	(void)device;
	trace("carrier %d", carrier);
	return 0;
}

/*
 * Records a received frame and drops it.
 */
void
net_device_receive(
	struct net_device *device,
	struct packet_buf *packet)
{
	const uint8_t *bytes;
	uint32_t sum;
	size_t index;

	/* Records the frame's length and a sum of its bytes, then drops it. */
	(void)device;
	bytes = packet->data;
	sum = 0;
	for (index = 0; index < packet->length; index++)
		sum = sum * 31U + bytes[index];
	trace("receive %zu sum %08x", packet->length, sum);
	packet_buf_free(packet);
}

/*
 * Records a wake of the network worker.
 */
void
net_device_schedule_poll(
	struct net_device *device)
{
	/* Records the wake of the network worker. */
	(void)device;
	trace("schedule poll");
}

/*
 * Finds the PHY's node by its phandle.
 */
int
drv_fdt_find_phandle(
	const struct drv_fdt *fdt,
	uint32_t phandle,
	uint32_t *node)
{
	/* Knows the PHY's phandle only. */
	(void)fdt;
	if (phandle != PHANDLE_PHY)
		return ENOENT;
	*node = NODE_PHY;
	return 0;
}

/*
 * Looks a property up in the fake tree.
 */
int
drv_fdt_property(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	const uint8_t **value,
	uint32_t *length)
{
	unsigned index;
	int comparison;

	/* Answers the board's address when the case put it in the tree. */
	(void)fdt;
	comparison = strcmp(name, "local-mac-address");
	if (node == NODE_GEM && comparison == 0) {
		if (!tree_has_mac)
			return ENOENT;
		*value = tree_mac;
		*length = sizeof(tree_mac);
		return 0;
	}

	/* Looks the property up in the fixed tree. */
	for (index = 0; index < sizeof(fake_tree) / sizeof(fake_tree[0]); index++) {
		/* Skips another node's property and another name. */
		if (fake_tree[index].node != node)
			continue;
		comparison = strcmp(fake_tree[index].name, name);
		if (comparison != 0)
			continue;

		/* Succeeded: the property. */
		*value = fake_tree[index].value;
		*length = fake_tree[index].length;
		return 0;
	}

	/* The tree has no such property. */
	return ENOENT;
}

/*
 * Reports every node of the fake tree as enabled.
 */
bool
drv_fdt_node_enabled(
	const struct drv_fdt *fdt,
	uint32_t node)
{
	/* Every node of the fake tree is enabled. */
	(void)fdt;
	(void)node;
	return true;
}

/*
 * Reads a one-cell property of the fake tree, or the fallback.
 */
uint32_t
drv_fdt_node_cells(
	const struct drv_fdt *fdt,
	uint32_t node,
	const char *name,
	uint32_t fallback)
{
	const uint8_t *value;
	uint32_t length;
	uint32_t cells;
	int error;

	/* Reads a one-cell property, or the fallback. */
	error = drv_fdt_property(fdt, node, name, &value, &length);
	if (error != 0 || length != 4U)
		return fallback;

	/* Succeeded: the property's cell. */
	cells = (uint32_t)drv_fdt_cells_value(value, 0, 1U);
	return cells;
}

/*
 * Joins big-endian cells.
 */
uint64_t
drv_fdt_cells_value(
	const uint8_t *value,
	uint32_t cell_index,
	uint32_t cell_count)
{
	uint64_t result;
	uint32_t index;

	/* Joins the big-endian cells. */
	result = 0;
	for (index = 0; index < cell_count * 4U; index++)
		result = (result << 8) | value[cell_index * 4U + index];
	return result;
}

/*
 * Hands out the fake tree.
 */
const struct drv_fdt *
drv_rp1_fdt(
	void)
{
	static const struct drv_fdt tree;

	/* Hands out the fake tree. */
	return &tree;
}

/*
 * Finds the controller or the clock controller among RP1's peripherals.
 */
int
drv_rp1_find_child(
	const char *compatible,
	uint32_t after,
	uint32_t *node)
{
	int comparison;

	/* Knows the controller. */
	(void)after;
	comparison = strcmp(compatible, "cdns,macb");
	if (comparison == 0) {
		*node = NODE_GEM;
		return 0;
	}

	/* And the clock controller. */
	comparison = strcmp(compatible, "raspberrypi,rp1-clocks");
	if (comparison == 0) {
		*node = NODE_CLOCKS;
		return 0;
	}

	/* RP1 has no such peripheral. */
	return ENODEV;
}

/*
 * Maps the fake controller or clock controller.
 */
int
drv_rp1_map(
	uint32_t node,
	unsigned index,
	volatile uint8_t **mapped,
	uint64_t *size)
{
	/* Maps the controller's registers. */
	(void)index;
	trace("map %x", node);
	if (node == NODE_GEM) {
		*mapped = (volatile uint8_t *)gem_registers;
		*size = 0x4000U;
		return 0;
	}

	/* Maps the clock controller's registers. */
	if (node == NODE_CLOCKS) {
		*mapped = (volatile uint8_t *)clock_registers;
		*size = sizeof(clock_registers);
		return 0;
	}

	/* The node has no registers. */
	return ENODEV;
}

/*
 * Reports RP1's fake window onto memory.
 */
int
drv_rp1_dma_window(
	uint64_t *bus_offset,
	uint64_t *physical_limit,
	int *coherent)
{
	/* RP1 reaches memory at an offset, coherently. */
	*bus_offset = 0x1000000000ULL;
	*physical_limit = 0x400000000ULL;
	*coherent = 1;
	return 0;
}

/*
 * Records the GPIO block's start.
 */
int
drv_rp1_gpio_init(
	void)
{
	/* Records the GPIO block's start. */
	trace("gpio init");
	return 0;
}

/*
 * Records a GPIO line's level.
 */
int
drv_rp1_gpio_set_output(
	unsigned pin,
	int level)
{
	/* Records the line's level. */
	trace("gpio %u %d", pin, level);
	return 0;
}

/*
 * Records the interrupt path's start.
 */
int
drv_rp1_irq_init(
	void)
{
	/* Records the interrupt path's start. */
	trace("irq init");
	return 0;
}

/*
 * Reports the controller's vector and trigger.
 */
int
drv_rp1_irq_of(
	uint32_t node,
	unsigned index,
	unsigned *vector,
	unsigned *trigger)
{
	/* The controller's vector and level trigger. */
	trace("irq of %x %u", node, index);
	*vector = 6U;
	*trigger = 4U;
	return 0;
}

/*
 * Keeps the routed interrupt handler for the case to call.
 */
int
drv_rp1_irq_establish(
	unsigned vector,
	unsigned trigger,
	drv_rp1_irq_handler_t handler,
	void *argument)
{
	/* Keeps the handler for the case to call. */
	trace("irq establish %u %u", vector, trigger);
	routed_handler = handler;
	routed_argument = argument;
	return 0;
}
