/* WS004-p019 terminating-zero-packet transfer model. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define MODEL_ZERO_PACKET (1U << 1)
#define MODEL_XHCI_USABLE_TRBS 255U
#define MODEL_EHCI_MAX_QTDS 124U
#define MODEL_UHCI_MAX_TDS 255U

enum model_transfer_type {
	MODEL_CONTROL,
	MODEL_ISOCHRONOUS,
	MODEL_BULK,
	MODEL_INTERRUPT
};

static int
zero_packet_required(enum model_transfer_type type, int input, size_t length,
	unsigned maximum_packet_size, unsigned flags)
{
	return type == MODEL_BULK && !input && length != 0 &&
	    maximum_packet_size != 0 && (flags & MODEL_ZERO_PACKET) != 0 &&
	    length % maximum_packet_size == 0;
}

static unsigned
xhci_normal_count(uint64_t address, size_t length)
{
	unsigned count = 0;

	if (length == 0)
		return 1;
	while (length != 0) {
		size_t chunk = 0x10000U - (size_t)(address & 0xffffU);

		if (chunk > length)
			chunk = length;
		address += chunk;
		length -= chunk;
		count++;
	}
	return count;
}

static int
xhci_plan(uint64_t address, size_t length, unsigned packet, unsigned flags,
	unsigned *payload_trbs, unsigned *total_trbs)
{
	unsigned payload = xhci_normal_count(address, length);
	unsigned zlp = (unsigned)zero_packet_required(MODEL_BULK, 0, length,
	    packet, flags);

	if (payload >= MODEL_XHCI_USABLE_TRBS ||
	    (zlp && payload >= MODEL_XHCI_USABLE_TRBS - 1U))
		return 0;
	*payload_trbs = payload;
	*total_trbs = payload + zlp;
	return 1;
}

static int
ehci_plan(size_t length, unsigned packet, unsigned flags,
	unsigned initial_toggle, unsigned *payload_qtds, unsigned *total_qtds,
	unsigned *zlp_toggle)
{
	size_t payload = length / 0x4000U + (length % 0x4000U != 0);
	unsigned zlp = (unsigned)zero_packet_required(MODEL_BULK, 0, length,
	    packet, flags);
	unsigned toggle = initial_toggle;
	size_t offset = 0;

	if (payload > MODEL_EHCI_MAX_QTDS - zlp)
		return 0;
	*payload_qtds = (unsigned)payload;
	*total_qtds = payload == 0 ? 1U : (unsigned)payload + zlp;
	while (offset < length) {
		size_t chunk = length - offset > 0x4000U ? 0x4000U :
		    length - offset;

		toggle ^= (unsigned)(((chunk + packet - 1U) / packet) & 1U);
		offset += chunk;
	}
	*zlp_toggle = zlp ? toggle : 2U;
	return 1;
}

static int
uhci_plan(size_t length, unsigned packet, unsigned flags,
	unsigned initial_toggle, unsigned *payload_tds, unsigned *total_tds,
	unsigned *zlp_toggle, unsigned *next_toggle)
{
	size_t payload = length / packet + (length % packet != 0);
	unsigned zlp = (unsigned)zero_packet_required(MODEL_BULK, 0, length,
	    packet, flags);
	size_t total = payload == 0 ? 1U : payload + zlp;

	if (total > MODEL_UHCI_MAX_TDS)
		return 0;
	*payload_tds = (unsigned)payload;
	*total_tds = (unsigned)total;
	*zlp_toggle = zlp ? initial_toggle ^ ((unsigned)payload & 1U) : 2U;
	*next_toggle = initial_toggle ^ ((unsigned)total & 1U);
	return 1;
}

static void
eligibility_test(void)
{
	assert(zero_packet_required(MODEL_BULK, 0, 64, 64,
	    MODEL_ZERO_PACKET));
	assert(zero_packet_required(MODEL_BULK, 0, 128, 64,
	    MODEL_ZERO_PACKET));
	assert(!zero_packet_required(MODEL_BULK, 0, 63, 64,
	    MODEL_ZERO_PACKET));
	assert(!zero_packet_required(MODEL_BULK, 0, 64, 64, 0));
	assert(!zero_packet_required(MODEL_BULK, 1, 64, 64,
	    MODEL_ZERO_PACKET));
	assert(!zero_packet_required(MODEL_CONTROL, 0, 64, 64,
	    MODEL_ZERO_PACKET));
	assert(!zero_packet_required(MODEL_INTERRUPT, 0, 64, 64,
	    MODEL_ZERO_PACKET));
	assert(!zero_packet_required(MODEL_ISOCHRONOUS, 0, 64, 64,
	    MODEL_ZERO_PACKET));
	assert(!zero_packet_required(MODEL_BULK, 0, 0, 64,
	    MODEL_ZERO_PACKET));
}

static void
xhci_test(void)
{
	unsigned payload, total;

	assert(xhci_plan(0x1000U, 64, 64, MODEL_ZERO_PACKET,
	    &payload, &total));
	assert(payload == 1U && total == 2U);
	assert(xhci_plan(0x1fff0U, 32, 64, MODEL_ZERO_PACKET,
	    &payload, &total));
	assert(payload == 2U && total == 2U);
	assert(xhci_plan(0x1000U, 64, 64, 0, &payload, &total));
	assert(payload == 1U && total == 1U);
	/* The usable 255-entry ring keeps one entry free to avoid a full-ring
	 * producer alias.  A ZLP consumes that same bounded TD capacity. */
	assert(253U + 1U < MODEL_XHCI_USABLE_TRBS);
	assert(254U + 1U == MODEL_XHCI_USABLE_TRBS);
}

static void
ehci_test(void)
{
	unsigned payload, total, zlp_toggle;

	assert(ehci_plan(64, 64, MODEL_ZERO_PACKET, 0, &payload, &total,
	    &zlp_toggle));
	assert(payload == 1U && total == 2U);
	assert(zlp_toggle == 1U);
	/* Two 512-byte packets leave DATA0 for the terminating ZLP.  Advancing
	 * once per qTD would incorrectly choose DATA1 here. */
	assert(ehci_plan(1024, 512, MODEL_ZERO_PACKET, 0, &payload, &total,
	    &zlp_toggle));
	assert(payload == 1U && total == 2U && zlp_toggle == 0U);
	assert(ehci_plan(0x4000U, 512, MODEL_ZERO_PACKET, 1,
	    &payload, &total, &zlp_toggle));
	assert(payload == 1U && total == 2U);
	assert(zlp_toggle == 1U);
	assert(ehci_plan((MODEL_EHCI_MAX_QTDS - 1U) * 0x4000U, 512,
	    MODEL_ZERO_PACKET, 0, &payload, &total, &zlp_toggle));
	assert(total == MODEL_EHCI_MAX_QTDS);
	assert(!ehci_plan(MODEL_EHCI_MAX_QTDS * 0x4000U, 512,
	    MODEL_ZERO_PACKET, 0, &payload, &total, &zlp_toggle));
}

static void
uhci_test(void)
{
	unsigned payload, total, zlp_toggle, next_toggle;

	assert(uhci_plan(64, 64, MODEL_ZERO_PACKET, 0, &payload, &total,
	    &zlp_toggle, &next_toggle));
	assert(payload == 1U && total == 2U);
	assert(zlp_toggle == 1U && next_toggle == 0U);
	assert(uhci_plan(128, 64, MODEL_ZERO_PACKET, 1, &payload, &total,
	    &zlp_toggle, &next_toggle));
	assert(payload == 2U && total == 3U);
	assert(zlp_toggle == 1U && next_toggle == 0U);
	assert(uhci_plan((MODEL_UHCI_MAX_TDS - 1U) * 64U, 64,
	    MODEL_ZERO_PACKET, 0, &payload, &total, &zlp_toggle,
	    &next_toggle));
	assert(total == MODEL_UHCI_MAX_TDS);
	assert(!uhci_plan(MODEL_UHCI_MAX_TDS * 64U, 64,
	    MODEL_ZERO_PACKET, 0, &payload, &total, &zlp_toggle,
	    &next_toggle));
}

int
main(void)
{
	eligibility_test();
	xhci_test();
	ehci_test();
	uhci_test();
	puts("USB HCD zero-packet transfer model: PASS");
	return 0;
}
