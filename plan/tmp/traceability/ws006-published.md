<!-- awesome-plan project=zedbsd record=ws006 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws006/ws.md`

親: [master](https://github.com/awemorris/zedBSD/issues/1)

# WS006: input and evdev

<!-- traceability:start -->

## Goal traceability

- Primary Milestone: **MG006 — グラフィカルな操作環境を利用できる**
- Related Milestones: MG003
- Objectives: O2, O4
- 貢献する成果: 操作に必要な入力デバイスとイベント体系を提供する。
- 上位定義: [MasterのObjectives / Milestone Goals](https://github.com/awemorris/zedBSD/issues/1)

既存Phaseは本WSを親として上位成果に接続する。Primaryは分類と責任の所在であり、
各PhaseがRelatedすべてを満たすという意味ではない。成果・検証・限界は各Phaseの
現行記録を根拠とする。今回の対応付けは状態変更・未定義作業の追加・実行許可ではない。

<!-- traceability:end -->


q147 closure: libc terminal identity is corrected. Ordinary xHCI and paired
EHCI/UHCI campaigns pass USB-root I/O, HID/hotplug, native PTY checks and actual
Xzed input/TTY restoration. [Results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase011-terminal-identity/results.md).

Last updated: 2026-09-09

WSID: `ws006`

Status: completed (q147), including the user's 2026-09-05 physical USB HID
confirmation and q126 Noct/BeUI consumer evidence.

Parent: [master plan](https://github.com/awemorris/zedBSD/issues/1)

Last verified Phases: `ws006-p009`, `ws006-p010`, `ws006-p011` complete (`q147`).

Resume point: none in this WS. Legacy event UAPI and `/dev/mouse` are absent;
ordinary TTY remains and Xzed opens capability-discovered evdev nodes.

Shared tests: [WS006 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/tests/README.md)

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws006-p001`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase001-evdev-profile/phase.md) | Complete | Experimental UAPI/profile and dual-ABI layout tests pass |
| [`ws006-p002`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase002-input-core/phase.md) | Complete milestone | Core/devfs/queue build and focused evidence pass; real producer runtime remains IN-02 |
| [`ws006-p003`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase003-producer-bridge/phase.md) | Complete milestone | Production event nodes register in QEMU; physical-key broker/consumer evidence remains p004 |
| [`ws006-p004`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase004-console-broker/phase.md) | Complete PC/AT software milestone | Fixed string event, single broker, focused/build evidence, and production QEMU event-node/console coexistence pass; PC-98/X68000 physical detail remains |
| [`ws006-p005`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase005-evdev-capability-state/phase.md) | Complete PC/AT milestone (`q020`) | Registered native-word capabilities, state queries, boundary fixtures, and capability-only amd64 QEMU discovery pass; character-only HAL and multi-source pointer residuals retained |
| [`ws006-p006`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase006-input-truthfulness-ownership/phase.md) | Complete automatic/source milestone (`q044`) | Per-source physical/momentary input, bounded console subscription, atomic overflow resync, and terminal callback ownership pass; fresh QEMU image acceptance remains behind WS008 MB-008 |
| [`ws006-p007`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase007-usb-hid-parser/phase.md) | Complete parser milestone (`q044`) | Bounded report/boot layouts and malformed/unsupported descriptor handling pass strict, sanitizer, and analyzer gates at 791 checks without live USB claims |
| [`ws006-p008`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase008-usb-hid-evdev/phase.md) | Complete (`q048` plus user physical confirmation, 2026-09-05) | Production Report-Protocol keyboard/mouse/tablet, generation-safe lifecycle, console coexistence, xHCI/EHCI/UHCI automatic gates, and physical USB HID pass |
| [`ws006-p009`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase009-consumer-legacy-removal/phase.md) | Complete (q147) | Legacy removal, Noct/BeUI and actual Xzed acceptance pass |
| [`ws006-p010`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase010-legacy-usb-root-recovery/phase.md) | Complete (q147) | Paired USB-root/HID replay after boot and UHCI ownership repairs |
| [`ws006-p011`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase011-terminal-identity/phase.md) | Complete (q147) | libc PTY identity, native error/bounds cases and GUI restoration |

## Historical Phase order (superseded by q147 closure)

```text
ws006-p005 complete
       |
       +-- ws006-p006 truthfulness and multi-source ownership --+ complete
       |                                                        |
       +-- ws006-p007 USB HID parser/report core ---------------+ complete
                                                               p008 automatic complete
                                                               IN-T42 physical pending
                                                                     |
ws018-p007 Xzed evdev complete --------------------------------------+--> p009
ws008-p009 latest Noct/BeUI evdev integration -----------------------+
```

P006 and p007 completed their independent implementation bodies in q044. P008
consumed both without reopening their private contracts; q048 completed its
automatic source, lifecycle, xHCI, and paired EHCI/UHCI runtime boundary.
One IN-T42 physical observation remains. P009 cannot enter
a Queue until `ws008-p009` replaces the
rejected/disabled historical target integration with the latest official Noct
target and passes its evdev consumer gate.

## Goals

- Provide a documented `/dev/input/eventN` evdev interface for keyboard and
  pointer events.
- Make existing console producers and USB HID devices use one kernel input
  core.
- Migrate consumers before removing the legacy console event/key-state API.

## WS completion conditions

WS006 is complete when the published evdev profile and ABI tests pass, console
input and multiple evdev readers coexist correctly, USB HID keyboard/mouse work
in QEMU and on supported hardware, all in-tree consumers are migrated, and the
obsolete console event/key-state interfaces are removed without regression.

## 1. Objective

Introduce `/dev/input/eventN` devices with an explicitly selected
Linux/FreeBSD-compatible evdev profile, migrate event consumers away from
zedBSD-specific `/dev/console` event/key-state interfaces, and support USB HID
keyboards and mice. The console continues to receive normal character input
through a kernel-internal input broker.

## 2. Migration rule

Legacy `/dev/console` input interfaces are removed only after equivalent evdev
producers and consumers are verified:

1. freeze the evdev UAPI and event semantics;
2. implement the input core and expose existing console keyboard/mouse producers
   as `/dev/input/eventN`;
3. feed console character processing from the same internal input core;
4. migrate Xzed and the Noct/BeUI zedBSD backend to evdev;
5. implement USB HID producers and verify hotplug;
6. delete continuous event acquisition and key-state ioctls from
   `/dev/console`, then remove dead compatibility code.

The console driver should not open its own `/dev/input/eventN` device. Producers
publish into a kernel input core, which fans out to evdev readers and to the
console's character/key translation path.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| IN-00 | Complete | evdev compatibility profile and public UAPI | Existing console UAPI audit | Header/layout tests pass and difference table is published |
| IN-01 | Complete milestone | Kernel input core, registration, event fan-out, buffering, poll/read, and lifecycle | IN-00, VFS/device primitives | Queue/ABI/native build pass; guest producer lifecycle evidence is handed to IN-02 |
| IN-02 | Complete PC/AT milestone | Existing console input producers also register evdev devices | IN-01 | Production keyboard/mouse nodes register and QEMU reads keyboard records without breaking console text input |
| IN-03 | Complete PC/AT milestone | Console consumes the internal input stream | IN-01/02 | One worker fans out PC/AT physical events; keymap/queue/QEMU coexistence evidence passes |
| IN-04 | Complete through `ws018-p007` | Xzed evdev migration | IN-02, GFX X11 repair | Keyboard and absolute/relative mouse behavior pass without a console-event or `/dev/mouse` fallback |
| IN-05 | Historical milestone complete; latest WS008 revalidation required | Noct/BeUI evdev migration | IN-02, latest NOCT upstream/backend work | Selected latest BeUI target passes without console event ioctls |
| IN-06 | q126実装済み / p009 uncleared | Remove console continuous-event and key-state UAPI | IN-03–05, IN-11 | No in-tree consumer remains; compatibility audit and regression tests pass |
| IN-07 | Complete through `ws006-p006` | Truthful logical/physical producers, internal console subscription, and per-source ownership | IN-01–03 | Character-only, multiple-source, detach, and console/evdev coexistence fixtures pass |
| IN-10 | Complete through `ws006-p007` | USB HID descriptor/report core | HW-01 xHCI, USB core | Descriptor parser corpus, malformed reports, boot/report protocol tests |
| IN-11 | Automatic/software milestone complete through `ws006-p008`; IN-T42 pending | USB HID keyboard and mouse evdev devices | IN-01, IN-07, IN-10 | QEMU USB keyboard/tablet/mouse and physical hotplug tests pass |

## 4. UAPI design gate

“Compatible with Linux/FreeBSD” is not sufficient as a binary contract. IN-00
must publish the selected definitions and differences, including:

- `struct input_event` field types, timestamp clock, alignment, and 32/64-bit
  behavior;
- event type/code/value constants required by keyboard and mouse consumers;
- device identity/capability/name queries and the supported `EVIOC*` subset;
- relative, absolute, synchronization, repeat, and device-removal semantics;
- grab/exclusive access policy, permissions, and event injection policy;
- stable numbering versus dynamic `/dev/input/eventN` discovery.

If exact source compatibility requires aliases while binary layouts differ,
that fact is documented rather than called transparent ABI compatibility.

## 5. Buffering and failure behavior

Each reader needs an independent bounded queue or cursor. Slow readers may not
stall input producers or the console. Overflow emits the selected synchronization
loss indication and requires consumers to resynchronize according to the
published profile. Detach wakes blocked readers and returns a stable error/end
condition.

USB HID parsing treats report descriptors as untrusted device input: all
lengths, counts, usages, and bit ranges are bounded before access.

## q126 Priority実行結果

旧console event UAPI撤去とTTY/evdev・Noct/BeUI・xHCI受け入れは通過。paired EHCI/UHCI USB root列挙timeoutとXzed GUI再検証を残し、WSは未完了。[p009結果・再開条件](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase009-consumer-legacy-removal/results.md)を参照。

## Current goal ordering (2026-09-09)

q126 EHCI/UHCI USB-root failure remains a required QEMU investigation and fix
within the Priority goal. The user requests ready implementation work first,
then USB analysis; this is an ordering change, not cancellation. Coordinate
with WS002-p023 USB-boot halt investigation, keeping boot and shutdown outcomes
separate unless evidence establishes a common cause.

## q137 legacy USB prerequisite

[p010](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase010-legacy-usb-root-recovery/phase.md) selected in q137 to repair
paired EHCI/UHCI USB-root enumeration before resuming p009 GUI/input acceptance.

q137 repairs boot-context ownership and reaches login, keyboard/relative input
and hotplug. xHCI full acceptance and paired dirty USB halt/reboot pass; paired
64 MiB concurrent I/O stalls in process-reaper heap traversal. p010 remains
uncleared; [progress and resume path](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase010-legacy-usb-root-recovery/progress.md)
point to WS002-p024 capture. WS006 and p009 remain incomplete.
User authorization includes QEMU analysis and correction within the Priority
goal; do not treat unavailable hardware as an implementation blocker.
