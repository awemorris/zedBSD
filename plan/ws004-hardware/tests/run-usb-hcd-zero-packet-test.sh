#!/bin/sh
# ws004-p019 terminating-zero-packet transfer gate.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
temporary_root=${TMPDIR:-"$root/build/q049-tmp"}
mkdir -p "$temporary_root"
work=$(mktemp -d "$temporary_root/usb-hcd-zero-packet.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc=${CC:-cc}
common='-std=c11 -Wall -Wextra -Werror'
fixture="$root/plan/ws004-hardware/tests/usb-hcd-zero-packet-test.c"
xhci="$root/src/drivers/pci-xhci.c"
ehci="$root/src/drivers/pci-ehci.c"
uhci="$root/src/drivers/pci-uhci.c"

# shellcheck disable=SC2086
$cc $common "$fixture" -o "$work/usb-hcd-zero-packet"
"$work/usb-hcd-zero-packet"

# shellcheck disable=SC2086
$cc $common -fsanitize=address,undefined -fno-omit-frame-pointer \
	"$fixture" -o "$work/usb-hcd-zero-packet-sanitize"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
	"$work/usb-hcd-zero-packet-sanitize"

# shellcheck disable=SC2086
$cc $common -fanalyzer -c "$fixture" \
	-o "$work/usb-hcd-zero-packet-analyzer.o"

xhci_enqueue=$(sed -n '/xhci_urb_enqueue(/,/^}/p' "$xhci")
xhci_normal=$(sed -n '/^enqueue_normal(/,/^}/p' "$xhci")
ehci_build=$(sed -n '/^ehci_build_request(/,/^}/p' "$ehci")
uhci_count=$(sed -n '/^uhci_required_td_count(/,/^}/p' "$uhci")
uhci_build=$(sed -n '/uhci_build_request(/,/^}/p' "$uhci")

for contract in DRV_USB_TRANSFER_BULK DRV_USB_URB_ZERO_PACKET \
    '!input' 'length != 0' 'length % maximum_packet_size == 0' \
    'normal_count + (zero_packet ? 1U : 0U)' \
    'normal_count >= XHCI_RING_TRBS - 2U'; do
	printf '%s\n' "$xhci_enqueue" | grep -Fq -- "$contract"
done
for contract in XHCI_TRB_CHAIN XHCI_TRB_IOC \
    'if (zero_packet && td_size < 31U)' \
    'ring_push(ring, address, 0'; do
	printf '%s\n' "$xhci_normal" | grep -Fq -- "$contract"
done

for contract in DRV_USB_TRANSFER_BULK DRV_USB_URB_ZERO_PACKET \
    '!drv_usb_endpoint_is_input(endpoint)' 'length != 0' \
    'length % packet == 0' 'EHCI_MAX_QTDS - zero_packet' \
    'length == 0 || zero_packet' \
	'((chunk + packet - 1U) / packet) & 1U' \
    'request->qtd_count != required_qtds' EHCI_QTD_IOC; do
	printf '%s\n' "$ehci_build" | grep -Fq -- "$contract"
done

for contract in 'data_count + zero_packet' UHCI_MAX_TDS; do
	printf '%s\n' "$uhci_count" | grep -Fq -- "$contract"
done
for contract in DRV_USB_TRANSFER_BULK DRV_USB_URB_ZERO_PACKET \
    '!drv_usb_endpoint_is_input(ep)' 'length != 0' \
    'length % packet == 0' 'length == 0 || zero_packet' \
    'r->td_count != required_tds' UHCI_TD_IOC; do
	printf '%s\n' "$uhci_build" | grep -Fq -- "$contract"
done

echo 'USB HCD zero-packet production-source gates: PASS'
