# Queue: RTL8822BU lifecycle automatic hardening

Last updated: 2026-09-02

QID: `q060`

Queue status: completed

Queue finished: **Yes**

Authorization: the user authorized continuous Queue execution, prioritized
WS004 before WS005, and directed the implementation to establish a simple
working communication path before perfecting abnormal and semi-normal cases.
Q059 completed that normal path. The user then explicitly selected complete
RTL8822BU abnormal/semi-normal hardening as the next priority.

Timebox: none. Execute only the finite automatic milestone of p030. Do not
request another physical adapter run or start WS005 p008 final acceptance in
this Queue.

Parent: [master plan](master.md)

Previous Queue: [q059](queue-q059.md)

## Purpose

Harden the now-working RTL8822BU WPA2-Personal/CCMP station across rekey,
bounded same-network reconnect, interface close/reopen, USB and firmware
failure, unplug/reinsert, shutdown, and concurrent USB-storage activity using
production-linked deterministic fixtures. Preserve the q059 normal path and
leave all physical lifecycle and five-run repeatability evidence to the later
shared WS005 p008 checkpoint.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws004-p030` | [WLAN lifecycle hardening](ws004-hardware/phase030-wlan-lifecycle-hardware-hardening/phase.md) | automatic milestone completed (`q060`); shared physical closure pending | P030's rekey/reconnect/lifecycle/fault/race/storage automatic milestone passes without claiming its later shared physical closure |

## Accepted decisions

- Retain only the already supported station-mode 2.4-GHz/20-MHz
  WPA2-Personal/CCMP profile. Do not add 5 GHz, DFS, HT/VHT, roaming, WPA3,
  DHCP policy, or another WLAN device in q060.
- The common kernel WLAN layer owns long-lived controlled-port, rekey, and
  same-network reconnect state. The RTL8822BU driver owns hardware/firmware,
  USB transport, radio, descriptors, and key slots.
- Reconnect attempts use delays 0/1/2/4/8 seconds, at most five failures and
  at most 30 seconds per generation. Explicit disconnect/down/removal cancels
  the generation.
- Removal never retains a passphrase or PMK. Reinsert creates a fresh device
  instance; later WS005 policy may submit a new credential.
- Implement the ordinary lifecycle transition first inside this Queue, then
  complete the bounded error/race matrix. Do not broaden into unrelated
  hardening.
- Do not use aggregate `make check`. Repository `.internal/` material is not
  an input to automatic tests; if runtime credentials are ever needed later,
  they may exist only under ignored `.internal/` and must not enter evidence or
  a commit.

## Implementation checkpoints

1. Inventory the q059 production state machine and ownership ledgers; extend
   focused fixtures before changing public or long-lived state.
2. Implement GTK and pairwise rekey with atomic key-generation replacement,
   replay/reinstall protection, and controlled-port ordering.
3. Implement finite same-network reconnect after link loss, with fresh
   authentication/nonces/keys and cancellation by explicit lifecycle events.
4. Harden open/down, firmware restart, endpoint recovery, unplug/reinsert, and
   terminal shutdown so all work is retired exactly once or quarantined.
5. Exercise success and injected failure across rekey/reconnect/lifecycle,
   including at least 100 synthetic iterations and concurrent USB-storage
   progress.
6. Rerun directly affected WLAN/RTL8822BU/USB/net-device regressions,
   sanitizer/analyzer variants, configured x86 builds, `make -j16`, bounded
   IDE and xHCI USB-root boots, and `git diff --check`.

## Completion definition

Q060 completes when p030's automatic milestone passes: rekey and reconnect
preserve strict key/PN/controlled-port semantics; close/recovery/removal/
reinsert/shutdown retire every owned object exactly once or retain a complete
checked quarantine; and the declared fault/race/storage regression matrix
passes. The Phase remains physically open until its one shared WS005 p008
lifecycle checkpoint and p008-owned five consecutive cold boots are later
accepted. No human action is requested by q060.

## Execution result

Completed on 2026-09-02. The production-linked WPA2/CCMP lifecycle aggregate
passed its ordinary, ASan/UBSan, compiler-analyzer, and amd64/i386 ABI gates.
The eight declared USB-wide regressions passed: checked recovery, concurrent
xHCI URBs, xHCI SuperSpeed interrupt context, net-device hotplug, CDC NCM wire
and driver, zero-packet HCD handling, and CDC ECM. The NCM driver recorded
2,013 checks and ECM recorded 1,464 checks in each ordinary and sanitizer run.

All five NVMe focused runners passed their ordinary, sanitizer, and analyzer
variants. The amd64 disk image and configured i386 kernel built after explicit
initialization of the slot/replacement declarations. The final amd64 candidate
was `build/q060-final-001/amd64/hdd-image.img`, 202392064 bytes, SHA-256
`e1b05f714af810bb1cb89b6badb4c2c694b33c20b7abb8406c86c9ca76c5c707`.
One four-CPU/4-GiB OVMF q35/xHCI USB-root boot passed through exact `login:`.
After the NVMe admin control was updated to require the current truthful
`writable max-transfer=8` publication, its disposable OVMF/q35 IDE-root rerun
returned `HW-T20 QEMU NVMe admin: PASS`; both the source image and the
discovery-only namespace remained byte-identical.

This completes only p030's finite automatic milestone. It does not claim or
consume the one shared WS005 p008 physical lifecycle checkpoint or p008-owned
five-consecutive-cold-boot campaign; those remain pending.

Next Queue: [q061](queue-q061.md)
