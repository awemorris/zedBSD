<!-- awesome-plan project=zedbsd record=ws009 -->

# WS009: documentation

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-27 ユーザーの回答（クリック）「今書く」: DOC-54（/dev/gpu の UAPI の文書）を今書く。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」: p009 を標準廉価実装手順で p010〜p024 に分けた）
Primary Milestone: MG001
Related Milestones: MG006, MG008, MG009
Objectives: O2, O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: p001〜p008 cleared。DOC-54 は p009 を分けた p010〜p024（2026-10-30 P1 の設計、q1701。review と反映は [gpu-doc-review.md](gpu-doc-review.md)）。骨格 docs/reference/gpu.md・gpu-*.md の基点は 4dfcce008、照合の script は plan/ws009/tests/check-gpu-uapi-doc.py、共通の規則は [gpu-doc-rules.md](gpu-doc-rules.md)。p010〜p021 は互いに独立（Sonnet 5.5 high、並べてよい。ws.md の表の行の merge の競合は Q1 が merge で解く）、p022 は p010〜p021 の後、p023 は p022 の後、p024（Opus を推す）は p023 の後。docs の link の試験の既知の失敗: `docs/agent/awesome-plan/README.md` の `img/1.png` が無い（固定版の欠け、触らない、2026-10-29 Q1）。docs/reference・docs/architecture の既存の link 切れ 4 件は 2026-10-30 P1 が直した
Target: **ベータ2**（2026-10-13 ユーザー「WS196、WS172、WS009はベータ2に入れます。」。旧: ベータ3（2026-10-08 ユーザー））
<!-- awesome-plan-current:end -->

Shared tests: [WS009 test index](tests/README.md)

## User decisions（DOC-54）

- 2026-10-27 ユーザーの回答（クリック）「今書く」: DOC-54 を保留から外し、今書く。
- 2026-10-29 ユーザーの回答（クリック、Q1 の中継）Q-D54-1「capset の形も書く」。解釈: 範囲は `/dev/gpuN` の ABI 1、sysctl の GPU の項、capset id 4・version 0 の payload の形。`/dev/amdrdna0` と設計中の UAPI は書かない。
- 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」: p009 を p010〜p024 に分け、実行してよい。

## Phase registry

| Phase | Status | Result / resume point |
| --- | --- | --- |
| [`ws009-p001`](phase001/phase.md) | Complete | Product hierarchy/rules and Noct relative-link validation established |
| [`ws009-p002`](phase002/phase.md) | Complete | Toolchain, parallel image build, amd64 QEMU marker, diagnostics, and links pass |
| [`ws009-p003`](phase003/phase.md) | Complete | Current init/service configuration, readiness, supervision, and shutdown contract published |
| [`ws009-p004`](phase004/phase.md) | Complete (`q046`) | Current multi-source, momentary, resync, console-subscriber, and detach behavior is documented without claiming live USB HID |
| [`ws009-p005`](phase005/phase.md) | Complete (`q046`) | Common parameters and all four required configured x86 loader paths are reconciled with production source and retained q015/q031/q032 evidence |
| [`ws009-p006`](phase006/phase.md) | Complete (`q072`) | Current WLAN, physical USB HID, Intel Mac Variant, Noct 2.0.1, and project-toolchain behavior is published without claiming planned removals or new UAPIs |
| [`ws009-p007`](phase007/phase.md) | Complete (`q150`) | Architecture, compatibility, console/graphics/system and multi-radio WLAN reconciled; 283 product links pass |
| [`ws009-p008`](phase008/phase.md) | Complete (`q190`) | Current amd64/PC98 installer and native-root workflows, recovery/limits, producer evidence and 293 product links verified |
| [`ws009-p009`](phase009/phase.md) | canceled（2026-10-30、p010〜p024 に分けて置き換えた） | DOC-54 の第 2 版の設計（p009a・p009b、22 LW）。review の指摘（[design-review.md](phase009/design-review.md)）は p010〜p024 の骨格の spec と phase.md に移した |
| [`ws009-p010`](phase010/phase.md) | cleared（2026-10-30 Q1、main fa61e266b。照合 --final PASS、link PASS） | gpu.md: node・権限・呼び方・並行と待ち・互換。依存: なし |
| [`ws009-p011`](phase011/phase.md) | cleared（2026-10-30 Q1、main e75adfd29。照合 --final PASS、link PASS） | gpu-capabilities.md: bit・GET_INFO・GET_CAPSET・capset 4。依存: なし |
| [`ws009-p012`](phase012/phase.md) | cleared（2026-10-30 Q1、main 23a127662。照合 --final PASS、link PASS） | gpu-memory.md: resource・blob・placed・transfer・map・device。依存: なし |
| [`ws009-p013`](phase013/phase.md) | planned（2026-10-30 P1 の設計、q1701） | gpu-sharing.md: export・import（resource・allocation・scanout）。依存: なし |
| [`ws009-p014`](phase014/phase.md) | cleared（2026-10-30 Q1、main 30ba69459。照合 --final PASS、link PASS） | gpu-submission.md: COMMAND・SUBMIT・WAIT・PRESENT・失敗と回復。依存: なし |
| [`ws009-p015`](phase015/phase.md) | planned（2026-10-30 P1 の設計、q1701） | gpu-fences.md: fence と *_SYNC。依存: なし |
| [`ws009-p016`](phase016/phase.md) | planned（2026-10-30 P1 の設計、q1701） | gpu-jobs.md: job。依存: なし |
| [`ws009-p017`](phase017/phase.md) | cleared（2026-10-30 Q1、main 805196240。照合 --final PASS、link PASS） | gpu-display.md: output・lease・present・wait・events・constraints・副作用。依存: なし |
| [`ws009-p018`](phase018/phase.md) | planned（2026-10-30 P1 の設計、q1701） | gpu-display-control.md: power・refresh・cursor・commit・overlay。依存: なし |
| [`ws009-p019`](phase019/phase.md) | cleared（2026-10-30 Q1、main 59ccf6412。照合 --final PASS、link PASS） | gpu-drivers.md: node の種類の表。依存: なし |
| [`ws009-p020`](phase020/phase.md) | 書いた・照合 PASS（2026-10-30、cleared は Q1 が決める） | gpu-command-stream.md・gpu-sysctl.md。依存: なし |
| [`ws009-p021`](phase021/phase.md) | planned（2026-10-30 P1 の設計、q1701） | gpu-examples.md: compile できる例。依存: なし |
| [`ws009-p022`](phase022/phase.md) | planned（2026-10-30 P1 の設計、q1701） | gpu-requests.md: 索引の 2 列と共通の errno の表。依存: p010〜p021 |
| [`ws009-p023`](phase023/phase.md) | planned（2026-10-30 P1 の設計、q1701） | 統合: 全 file の --final・二重の主張の照合・既存の文書の直し。依存: p022 |
| [`ws009-p024`](phase024/phase.md) | planned（2026-10-30 P1 の設計、q1701） | 正確さの敵対的レビュー（Opus を推す）。依存: p023 |

Documentation work may be extracted alongside a producer WS when it describes
a newly frozen public interface, but it retains a WS009 Phase ID and acceptance
record.

## Goals

- Explain the zedBSD architecture, independent specifications, HAL boundaries,
  public UAPIs, build system, boot flow, init/services, and supported devices.
- State current behavior, compatibility, limitations, and planned behavior
  without presenting one as another.
- Make a clean build, QEMU boot, and supported-system administration
  reproducible from the documentation.

## WS completion conditions

WS009 is complete when every listed documentation work item is published,
cross-linked, and traceable to current headers/source/tests; build and boot
instructions are reproduced in a clean environment; compatibility claims agree
with WS001; and the documentation link/structure checks pass.

## 1. Objective

Create maintainable documentation for zedBSD's independent specifications,
architecture, public interfaces, build/boot process, init system, and HAL
bridges. Documentation is developed alongside stable behavior and is required
evidence for new public UAPIs.

Plans remain under `plan/`. Product documentation may later be organized
under directories such as `docs/architecture/`, `docs/reference/`, and
`docs/howto/`; that layout is chosen in the first documentation Phase rather
than assumed here.

## 2. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| DOC-00 | Complete | Documentation information architecture, style, version/status banners, and link checks | Current docs inventory | Navigation and automated relative-link validation pass |
| DOC-10 | Complete (`q150`) | zedBSD independent-specification overview | Architecture inventory | Each intentional divergence has rationale, stable contract, and implementation references |
| DOC-11 | Complete (`q150`) | HAL overview and architecture-independent kernel structure | Kernel/platform audit | Subsystem boundaries, ownership, and amd64-specific examples are traceable to source |
| DOC-12 | Complete (`q150`) | UAPI POSIX/SUS compliance and `_XOPEN_SOURCE` profile | PX-02 | Claims match headers, implementations, and compliance ledger |
| DOC-20 | Complete | Complete build-from-source guide | Supported toolchain/build audit | Toolchain/image commands reproduced; amd64 QEMU and links pass |
| DOC-30 | Complete (`q190`; retained boot evidence) | Bootloader and boot-flow guide | BR-01–BR-04 design | BIOS/UEFI flow, failure points, and diagnostic paths are documented |
| DOC-31 | Complete through `ws009-p005` | Kernel parameter reference | Parser/default audit | Every documented parameter cites parser/default and unknown-key behavior |
| DOC-32 | Complete (`q190`; retained boot evidence) | Boot filesystem plus loopback-root guide | Boot/root implementation | QEMU procedure reaches the intended root reproducibly |
| DOC-33 | Complete (`q190`) | Native UFS-root guide | Stable native storage drivers | QEMU and supported hardware procedures are explicit and safe |
| DOC-34 | Complete (`q190`) | USB trial and NVMe installation guide | WS019 p005, WS003 p018 | Public text/graphic source/mode/confirmation/copy workflows, GPT/FAT coexistence, dedicated EFI+UFS, native file swap, PC98 FAT/bootstrap limits and source-free boot match accepted WS019/q187 behavior |
| DOC-40 | Complete | zedBSD init, rc.conf, service.d, fd 3 readiness, and shutdown model | Phase 11–20 implementation | Boot/service/shutdown examples match tested behavior |
| DOC-50 | Complete (`q150`) | `/dev/console` HAL-bridge reference | IN migration state | Text/input/framebuffer roles and deprecated interfaces are accurate |
| DOC-51 | Complete (`q150`) | `/dev/graphics` HAL-bridge reference | GFX takeover design | Framebuffer, mmap/ioctl, ownership, and GPU takeover are specified |
| DOC-52 | Complete (`q150`) | `/dev/system` HAL-bridge reference | Device/UAPI audit | Operations, permissions, data structures, and architecture hooks are specified |
| DOC-53 | Complete through `ws009-p004` | evdev and `/dev/input` UAPI reference | IN-00 through the q044 p006/p007 boundary | ABI profile, event semantics, examples, current ownership, and compatibility differences are published without claiming live USB HID |
| DOC-54 | Planned as `ws009-p010`〜`p024`（p009 を分けた、2026-10-30） | `/dev/gpu` UAPI and capability reference (`docs/reference/gpu.md` and `gpu-*.md`) | GFX-10 | Versioned object model and supported profiles match headers/tests; `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final` passes |
| DOC-55 | Complete (`q150`) | `networkd`, `net`, `dhcpc`, WPA backend, and rc.conf networking reference | NET-20+ as applicable | Command/config/protocol/error examples pass tests |

## 3. Required document qualities

Public-interface documentation includes:

- stability/version status and supported architectures;
- exact headers, structures, constants, calls/ioctls, error behavior, and
  permissions;
- ownership, lifetime, concurrency, blocking, and cancellation rules;
- normal example plus timeout/failure example;
- implementation and test references;
- compatibility statement for POSIX, SUS, Linux, FreeBSD, or zedBSD-specific
  behavior as applicable.

Architecture documents distinguish current implementation from intended design.
Planned behavior is not presented as already available.

## 4. Delivery sequence

1. DOC-00 establishes navigation and validation.
2. The build and minimum boot/diagnostic guide is produced early enough to
   support external reproduction of hardware bring-up.
3. Init/network documentation consolidates the already implemented Phase 11–20
   behavior.
4. evdev and GPU references are written as part of their UAPI design Phases,
   before legacy input removal or accelerated consumers are declared stable.
5. The POSIX/SUS profile is synchronized with the compliance master on every
   relevant Phase completion.
