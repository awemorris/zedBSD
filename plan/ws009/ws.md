<!-- awesome-plan project=zedbsd record=ws009 -->

# WS009: documentation

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-27 ユーザーの回答（クリック）「今書く」: DOC-54（/dev/gpu の UAPI の文書）を今書く。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」: p009 を標準廉価実装手順で p010〜p024 に分けた）
Primary Milestone: MG001
Related Milestones: MG006, MG008, MG009
Objectives: O2, O4, O5
Parent: [Master](../master.md)
Queue: なし
Resume point: p001〜p008・p010〜p024 cleared（p009 canceled）。DOC-54 は完了（p024 main d2c8a5969）。2026-10-30 P1（q1705-i01）が WS の完了の条件を確かめ、満ちない条件が残った（下の「WS の受け入れの確認（2026-10-30）」）。満たすために p025〜p028 を立てた（2026-10-30 Q1 の判断「案 A〜D を Phase にする」）: p025（Opus、P1）と p026（Sonnet）は並べてよい、p027（Sonnet）は p025・p026 の後、p028（Opus、P1）は p026 の後。完了の形への書き直しと Phase の directory の片付けは p025〜p028 の後に受け入れを確かめ直してから。照合の script は plan/ws009/tests/check-gpu-uapi-doc.py（基点 4dfcce008）、共通の規則は [gpu-doc-rules.md](gpu-doc-rules.md)。docs の link の試験の既知の失敗: `docs/agent/awesome-plan/README.md` の `img/1.png` ほか 6 件（固定版の欠け、触らない、2026-10-29 Q1）
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
| [`ws009-p013`](phase013/phase.md) | cleared（2026-10-30 Q1、main 920060ea3。照合 --final PASS、link PASS） | gpu-sharing.md: export・import（resource・allocation・scanout）。依存: なし |
| [`ws009-p014`](phase014/phase.md) | cleared（2026-10-30 Q1、main 30ba69459。照合 --final PASS、link PASS） | gpu-submission.md: COMMAND・SUBMIT・WAIT・PRESENT・失敗と回復。依存: なし |
| [`ws009-p015`](phase015/phase.md) | cleared（2026-10-30 Q1、main adae0d7b6。照合 --final PASS、link PASS） | gpu-fences.md: fence と *_SYNC。依存: なし |
| [`ws009-p016`](phase016/phase.md) | cleared（2026-10-30 Q1、main a1fa5f50c。照合 --final PASS、link PASS） | gpu-jobs.md: job。依存: なし |
| [`ws009-p017`](phase017/phase.md) | cleared（2026-10-30 Q1、main 805196240。照合 --final PASS、link PASS） | gpu-display.md: output・lease・present・wait・events・constraints・副作用。依存: なし |
| [`ws009-p018`](phase018/phase.md) | cleared（2026-10-30 Q1、main b28d32c23。照合 --final PASS、link PASS） | gpu-display-control.md: power・refresh・cursor・commit・overlay。依存: なし |
| [`ws009-p019`](phase019/phase.md) | cleared（2026-10-30 Q1、main 59ccf6412。照合 --final PASS、link PASS） | gpu-drivers.md: node の種類の表。依存: なし |
| [`ws009-p020`](phase020/phase.md) | cleared（2026-10-30 Q1、main fff25f65e。照合 --final PASS（2 file）、link PASS） | gpu-command-stream.md・gpu-sysctl.md。依存: なし |
| [`ws009-p021`](phase021/phase.md) | cleared（2026-10-30 Q1、main 9e41d8918。照合 --final --examples PASS（x86_64・aarch64・riscv64 × 4 例、compile のみ）、link PASS） | gpu-examples.md: compile できる例。依存: なし |
| [`ws009-p022`](phase022/phase.md) | cleared（2026-10-30 Q1、main 4374dde38。照合 --final PASS（placeholder 0）、link PASS） | gpu-requests.md: 索引の 2 列と共通の errno の表。依存: p010〜p021 |
| [`ws009-p023`](phase023/phase.md) | cleared（2026-10-30 Q1、main 4a3b4e0e4。全 14 file の照合 --final --examples PASS（12 compile）、link PASS 826。Bug の候補 23 は phase023 の結果） | 統合: 全 file の --final・二重の主張の照合・既存の文書の直し。依存: p022 |
| [`ws009-p024`](phase024/phase.md) | cleared（2026-10-30 Q1、main d2c8a5969。14 file を code と照らし 12 file を直した、照合 --final --examples PASS、link PASS 676。新しい Bug の候補 15 は BUG-365・BUG-366） | 正確さの敵対的レビュー（Opus を推す）。依存: p023 |
| [`ws009-p025`](phase025/phase.md) | cleared（2026-10-30 Q1、main 581085a8c。managed-wlan.md 全面書き直し・network-console.md、usage の 9 形が一致、link PASS 1037。guest の実行は未確かめ） | 案 A: DOC-55 managed-wlan.md を今の `net wifi`（add・modify・delete・session）と `net lan`・`net startup` に合わせる。依存: なし |
| [`ws009-p026`](phase026/phase.md) | cleared（2026-10-30 Q1、main 2dd8d03b0。Noct 2.0.3 の値、menuconfig の label、索引から全て辿れる・Status 行の欠け 0、link PASS 1036） | 案 B: build-from-source.md の Noct 2.0.3 と variant の label、evdev.md の Noct の版、索引（media-playback・release の beta2）、状態の行 11 file。依存: なし |
| [`ws009-p027`](phase027/phase.md) | in-progress（書いた・PASS、2026-10-30 Sonnet 5.5。cleared は Q1） | 案 C: DOC-T70〜T72 の照合を source から値を読む `check-docs-current.py` に作り直す、plan/tools/docs-check/ へ移す案。依存: p025・p026 |
| [`ws009-p028`](phase028/phase.md) | planned（2026-10-30 P1 の設計、Opus = P1 が実行） | 案 D: clean な tree からの build と amd64 QEMU の boot の再現（DOC-T20・T30・T72）、互換の主張と WS001 の照合（DOC-T60）。依存: p026（DOC-T60 は依存なし） |

## WS の受け入れの確認（2026-10-30 P1、q1705-i01、基点 main aaf2bc280）

WS の完了の条件（下の「WS completion conditions」）を一つずつ確かめた。**満ちない条件が残る**ので、ws.md は完了の形にしない。

| 条件 | 確かめ | 結果 |
| --- | --- | --- |
| 全ての work item の公開 | §2 の表の DOC-00〜DOC-55 | 全部 Complete（DOC-54 は p024 で完了、この確認で表を直した）。ただし公開済みの DOC-55 と DOC-20 の一部が今の source と食い違う（下の 3・4） |
| 相互の link（docs の索引から辿れる） | docs/README.md から相対 link を辿る（scratchpad の script） | 56 file のうち 50 に届く。届かない product の文書: `docs/reference/media-playback.md`、`docs/release/zedbsd-1.0.0-beta2.md`（`docs/agent/` の 4 file は agent 向けで対象外と見る） |
| header・source・試験への辿り（DOC-54） | `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final` | PASS（14 files、46 index rows、46 request sections、98 constant rows、47 structures、20 capset rows、0 placeholders、12 example compiles） |
| 辿り（DOC-T70〜T72、`check-completed-producer-docs.noct`） | `noct plan/ws009/tests/check-completed-producer-docs.noct .` | **FAIL**（最初の失敗で止まる: `userland/base/net/main.c: missing required text: net wifi set-key SSID PASSPHRASE [auto]`）。失敗を全部数えた写し（scratchpad）では、文の照合 9 件と状態の行 29 件が落ちる（下の 3〜6） |
| link の検査 | `noct plan/ws009/tests/check-relative-links.noct <dir>` | docs/reference 614・docs/architecture 62・docs/howto 349・docs/release 9 links PASS。`docs` 全体は既知の `docs/agent/awesome-plan/README.md` の `img/1.png` で止まる。`plan` は `plan/agents/P1/queue.md -> ../../ws005/phase018/phase.md` で止まる（数えた写しで plan の切れは約 4,770 件、消した Phase の directory への link が大半。plan の記録は Q1 の record-audit の範囲で、WS009 の product の文書の条件ではないと見る） |
| 構造（状態の行） | docs/style.md「Begin each document with a status line that says what kind of document it is」 | `Status:` の行の無い docs の file が 16（product の物: docs/README.md、architecture/README.md、reference/README.md、reference/agent-acceptance-test.md・atomic-publication.md・block-administration.md・media-playback.md・security-keys.md・tls.md・vulkan-video.md、release/zedbsd-1.0.0-beta2.md、style.md） |
| clean な環境での build と boot の再現（DOC-T20・T30・T72） | — | **未実施**（最後の再現は p002・q190。その後 amd64 の既定の profile が `native` になり、Noct が 2.0.3 になった） |
| 互換の主張と WS001 の一致（DOC-T60） | — | **未実施**（WS001 は incomplete・保留で、POSIX の header の Phase が残る） |

満ちない点の詳細:

1. `docs/reference/media-playback.md`・`docs/release/zedbsd-1.0.0-beta2.md` が索引から辿れない。
2. 状態の行の無い product の文書（上の表）。
3. **DOC-55 `docs/reference/managed-wlan.md` が古い**: 「Status: current」で `net wifi set-key SSID PASSPHRASE [auto]` を唯一の文法とするが、今の `userland/base/net/main.c` の usage は `net wifi add SSID [--password PASSWORD] [--auto yes|no]`・`modify`・`delete`・`enable`・`disable`・`list`・`connect`・`disconnect`・`session open|close [account]`（利用者ごとの store、login session の間だけ join）と `net lan ...`・`net startup`。`set-key` は main.c に 0 件。
4. **DOC-20 `docs/howto/build-from-source.md` の Noct が古い**: 文書は「post-2.0.1 commit `bb239816e…`」、`userland/base/noct/version.mk` は `ZEDBSD_NOCT_VERSION := 2.0.3`・tag `f6efa83a8c…`。
5. `check-completed-producer-docs.noct` 自体が古い（文書が正しく試験が古い物）: evdev の「remain implemented and are deprecated」（legacy の console の event は削除済みと文書が正しく書く）、Makefile の `ZEDBSD_VARIANTS_amd64_pcat := hybrid uefi bios`（今は `native hybrid uefi bios`）、variant の label `UEFI + BIOS (for PC/AT)`・`BIOS (for PC/AT)`・「correctly built `uefi` image\nis expected not to boot」の改行の位置、Noct 2.0.1 の 3 件。状態の行の規則（current/experimental/deprecated/planned）は style.md の今の規則（文書の種類を書く）と合わない。
6. 上の 5 の通り、DOC-T70〜T72 の試験は直すか、AGENTS.md §9 の試験の整理の基準で置き換える必要がある。

満たすための案（2026-10-30 Q1 の判断で p025〜p028 にした）:

| 案 | 内容 | 担当の目安 |
| --- | --- | --- |
| A | managed-wlan.md を今の `net wifi`・`net lan`・`net startup`・session の振る舞いに書き直す（source と networkd を照らす） | Opus（振る舞いの読み取りが要る） |
| B | build-from-source.md の Noct の版と variant の記述を今の Makefile・version.mk に合わせる。索引から辿れない 2 file を索引に足す。状態の行を足す | Sonnet 5.5 high で 1 回 |
| C | `check-completed-producer-docs.noct` を今の source と style.md の状態の行の規則に合わせて直す（失敗を全部出す形）。今後も使うなら `plan/tools/` へ移す | Sonnet 5.5 high で 1 回（A・B の後） |
| D | clean な環境での build と amd64 QEMU の boot の再現（DOC-T20・T30・T72）、WS001 の互換の主張の照合（DOC-T60） | Q1 が QEMU を割り当てて Opus |

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
| DOC-54 | Complete through `ws009-p024`（2026-10-30、main d2c8a5969） | `/dev/gpu` UAPI and capability reference (`docs/reference/gpu.md` and `gpu-*.md`) | GFX-10 | Versioned object model and supported profiles match headers/tests; `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final` passes |
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
