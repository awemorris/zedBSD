<!-- awesome-plan project=zedbsd record=ws003 -->

<!-- awesome-plan-current:start -->

Status: incomplete
Lifecycle: closed (not planned), retired; no reuse
Current Focused Goals: none

未完了成果は保留に移し、WSを終了。元の全目標を達成したという意味ではない。

## 2026-09-12 WS003終了・WS027新設

ユーザーがPPCを新規移植として独立WSへ移すよう指示し、WS003を閉じて再利用しないことを指定した。その他の未完了は「未完了のまま保留事項へ移し、WS003内のPhaseは終了する」と明示。目標達成や試験PASSを追加する判断ではない。

PPC移植は[ws027](https://github.com/awemorris/zedBSD/issues/374)のp001-p007（旧WS003 p033-p039）へ移管。初期到達点はp003まで。その他の未完了は[Future Work F-004](https://github.com/awemorris/zedBSD/issues/364)へ保留移管。fg009はWS027、fg004は保留。WS003は終了・再利用禁止。実行Queueは作らない。

<!-- awesome-plan-current:end -->

<details>
<summary>WS003の終了前の履歴</summary>

# WS003: real-hardware bring-up

<!-- installer-bringup-current:start -->

## Current focus — 2026-09-11 installer bring-up

Status: incomplete。親: [master](../master.md)。Primary Milestone: MG003。Focused Goals: fg004（fg006は2026-09-12完了）。
現在のPriorityリストは削除済みのまま。今回の機種一覧は順位ではない。Queue: none。今回のユーザー指示は計画更新であり、開発・実機操作は開始しない。

### ゴールと担当

| 機種 | 到達点 | 担当Phase |
| --- | --- | --- |
| PC-9821V13 / 64MB / CF-IDE | 実機でインストールが行える | [ws003-p024](phase024/phase.md)、[ws003-p026](phase026/phase.md)、必要な[ws003-p027](phase027/phase.md) → [ws003-p029](phase029/phase.md) |
| Dell Latitude 5320 | 実機でインストールが行える | [ws003-p018](phase018/phase.md)（既存FAT経路）または[ws003-p019](phase019/phase.md)（native経路）。実行する方式を具体化して選択 |
| Let's Note SV7 | 実機でインストールが行える | [ws003-p030](phase030/phase.md) |
| Let's Note LX6 | 実機でインストールが行える | [ws003-p028](phase028/phase.md) → [ws003-p031](phase031/phase.md) |

共通の受け入れ案は通常インストールの完了と、インストール先からloader→kernel→root→init/login・基本操作を確認すること。text/graphic、FAT/native、具体的な書込み先は機種ごとの実行計画で確定する。両モード全組合せや新しい反復実機試験を自動的な必須条件にしない。既存の具体的な媒体・データ保持の制約は維持する。
この4機種の成果はfg004を担う。WS003の既存の別ゴール・未完了事項を今回の範囲に無断で加えたり、4機種だけでWS全体がcompletedになったと判断したりしない。

### 今回の観測と課題

- [BUG-013](../bugs/BUG-013.md): PC98のLBA0がロード・実行されるがビープ停止。以前の到達推定を今回のユーザー観測で更新。PC98構成は従来と同じことをユーザーが確認。
- [BUG-023](../bugs/BUG-023.md): `/sbin` が空なのはQEMUで起動したPC98環境。実機IPL停止とは別経路。
- [BUG-024](../bugs/BUG-024.md): PC98でPCI/USBをmenuconfigから選べない。platform除外とnormalize経路を静的確認。実動作対応は別途確認。
- [BUG-025](../bugs/BUG-025.md): LX6 USB起動でbootパーティションを判別できずinitを起動できない。正確なログ・起動モードは未取得。

閉鎖済みWS019/WS025およびWS025-p032の受け入れは維持。インストーラ実装は[ws019](../ws019/ws.md)を利用し、PC98ブート問題はBUG-013の下でp022→p023→p024が契約確認・停止位置特定・修正/実機確認を分担する。BUG-017とLX6問題の同一原因は未証明。

### 依存と実行準備

p022 → p023 → p024 → p029、p026 → p029、p028 → p031。p027 → p029はPCI/USBを実際に使う場合の成果依存であり、IDE単独の経路を不要に止めない。SV7およびLatitudeの準備はPC98/LX6修復から独立。
Latitude p018は既存の読取り・loader・installer成果を現行artifactで確認する。p019はnative方式を選んだ場合だけ具体化する。閉鎖されたWS019への古い「実装待ち」は現在の前提にしない。
[ws003-p032](phase032/phase.md)は今回変更した最終sourceに対する規約確認を担う。変更が出揃う終盤に機種別結果と合わせる。前提未充足や実機入力待ちは対応Phaseで明示し、独立作業を待たせない。
新しい実行Queueは未選択。調査順の候補はQEMU `/sbin`・menuconfigの独立切り分け、PC98 LBA0後の境界確定、LX6のboot媒体解決の切り分け。これは承認済み実行順ではない。

## 2026-09-11 fg006: PC-9821V13での起動改善

ユーザーがCurrent Focused Goalsへの追加と、WS003 p022/p023/p024の具体的実行Phase化を明示指示した。fg004（4機種インストーラ）・fg005（ネットワーク）を保持する。

| 順序 | 実行Phase | 成果 |
| --- | --- | --- |
| 1 | [ws003-p022](phase022/phase.md) | 現行IPLのstack・BIOS read契約、artifact対応、診断の前提 |
| 2 | [ws003-p023](phase023/phase.md) | LBA0実行後の実機停止境界と原因を絞る観測 |
| 3 | [ws003-p024](phase024/phase.md) | 根拠に対応した修正と通常imageでのV13起動確認 |

依存: p022 → p023 → p024。p022/p023は旧履歴専用の扱いを解除し、現行の役割・受け入れを上記と各Phaseに更新する。過去の試行・証拠は保持する。3 Phaseはunclearedのまま次の試行を待ち、今回in-progressやclearedにしない。
この順序は選択した3 Phaseの依存順であり、削除済みの全WS Priorityリストを復活させない。今回の依頼は計画更新。新しいactive Queue・実行時間枠は未設定。

<!-- installer-bringup-current:end -->


<!-- traceability:start -->

## Goal traceability

- Primary Milestone: **MG003 — 対象機へ導入して単独起動できる**
- Related Milestones: MG008
- Objectives: O2, O4
- 貢献する成果: 対象機の起動と移植上の問題を解消する。
- 上位定義: [MasterのObjectives / Milestone Goals](https://github.com/awemorris/zedBSD/issues/1)

既存Phaseは本WSを親として上位成果に接続する。Primaryは分類と責任の所在であり、
各PhaseがRelatedすべてを満たすという意味ではない。成果・検証・限界は各Phaseの
現行記録を根拠とする。今回の対応付けは状態変更・未定義作業の追加・実行許可ではない。

<!-- traceability:end -->

<details>
<summary>2026-09-11より前の引き継ぎ履歴（現行指示は上記）</summary>



Last updated: 2026-09-03

WSID: `ws003`

Status: active; `ws003-p004` through `ws003-p009` complete in q013; q014
`ws003-p010` and physical U3 complete through BR-T41; q015 completed `p011`
through `p015`; q023 completed `ws003-p016`; `ws003-p017` is superseded by
the WS013 required-`zedbsd.cfg` path; `ws003-p018` is the dependency-gated
final Latitude NVMe install/boot milestone; `ws003-p020` completed in q033 on
the Panasonic CF-SV7, and `ws003-p021` completed its raw-image GPT/root
continuation in q034 with a successful physical CF-SV7 boot. q043
`ws003-p024` now removes only Stage 1's unused SENSE transaction before its
invariant CHS 0/0/2 read while retaining the geometry-dependent PBR/BOOTZBSD
SENSE paths. Its source/binary/QEMU milestone passes and one exact
PC-9821V13 artifact boot remains; p022/p023 are retained as historical
automatic evidence rather than competing physical requests

Q066 completed `ws003-p025`'s automatic milestone: the approved HAL
wall-clock/counter split and complete-CPU-set amd64 TSC publication contract
now pass their focused, positive/negative SMP QEMU, and configured-build gates.
It does not move timing policy into the AX211 driver. One physical multicore
observation remains and is shared with p038's final direct boot.

Parent: [master plan](../master.md)

Last completed Phase: `ws003-p021` in q034. q015 `ws003-p011`--`p015` also
completed BR-T46 with 31/31
production-loader cells across i386 PC/AT, i386 PC-98, amd64 BIOS, and amd64
UEFI in the post-review `q015-br-t46-final-007` run. Earlier BR-T41 resolved
the intended UUID to `/dev/sda1`, mounted the
read-write data loop and root overlay, started init, and reached a root shell,
proving physical tier U3.

Resume point: boot the single p024 diagnostic artifact named in its Phase once
on the PC-9821V13 and report its screen/audio boundary. Do not run the older
p022/p023 physical artifacts first. The fixed-read automatic milestone and
independent review already pass; the production Make-owned Noct gate resumes
separately through `ws008-p010`. The CF-SV7 USB-root issue is closed.
Finish the automatic WS013/WS019 prerequisites before `ws003-p018`. Latitude follow-up still
includes BR-T31
sustained root I/O and, after U4 is otherwise frozen, BR-T30 five-boot
repeatability. Do not request an additional intermediate hardware boot now.
Hardware inventory remains incomplete for both declared laptops.

Shared tests: [WS003 test index](tests/README.md)


</details>

## Phase registry

| Combined ID | Work item | Status | Result |
| --- | --- | --- | --- |
| `ws003-p001` | [BR-00 hardware inventory](phase001/phase.md) | Carried forward; WLAN ID captured | RTL8822CE is `10ec:c822`, subsystem `10ec:c130`; remaining target DMI/PCI/USB inventory is incomplete |
| `ws003-p002` | [BR-05 Latitude UEFI memory map](phase002/phase.md) | Complete | Four physical markers proved U1 and `RSDP=0x64ffe014`; the corrected image reaches ACPI/IRQ/HAL on hardware 3/3, while BR-T24 4/8/16-GiB OVMF and legacy BIOS remain passing |
| `ws003-p003` | [Latitude xHCI capability/MMIO bring-up](phase003/phase.md) | Partial (`q012` uncleared) | Both physical xHCI 1.2 controllers pass capability validation and attach; BR-T33 then fails during EP0 enumeration before mass storage |
| `ws003-p004` | [Latitude xHCI device enumeration](phase004/phase.md) | Complete (`q013`) | BR-T34 reached `usb-storage: sda`; Control/EP0/reset and U2 are physically accepted |
| `ws003-p005` | [xHCI command and cancellation lifecycle](phase005/phase.md) | Complete (`q013`) | Fault fixtures and BR-T34 show safe command/cancel/DMA/slot ownership |
| `ws003-p006` | [xHCI halted-endpoint recovery](phase006/phase.md) | Complete (`q013`) | BR-T35 and physical BOT I/O clear EP0/bulk recovery and Normal-IN TD behavior |
| `ws003-p007` | [Shared DMA allocation synchronization](phase007/phase.md) | Complete (`q013`) | BR-T36 and both physical controllers complete without allocation-registry corruption |
| `ws003-p008` | [xHCI device association lifetime](phase008/phase.md) | Complete (`q013`) | BR-T37/hotplug and multi-device physical configuration retain the correct object association |
| `ws003-p009` | [xHCI SuperSpeed endpoint context](phase009/phase.md) | Complete (`q013`) | BR-T38 and the physical SuperSpeed storage configuration clear Slot/Endpoint Context |
| `ws003-p010` | [USB-storage flush capability](phase010/phase.md) | Complete (`q014`) | BR-T41 mounted the USB-backed writable overlay and reached init/login/root shell; the opcode-35 failure did not recur |
| `ws003-p011` | [common boot-parameter core and init selection](phase011/phase.md) | Completed (`q015`, 2026-08-27) | BR-T42 passes; the bounded common parser and architecture-independent `init=` semantics are implemented |
| `ws003-p012` | [x86 boot-parameter handoff](phase012/phase.md) | Completed (`q015`, 2026-08-27) | BR-T43 and all four production-loader runtime paths publish the same kernel-owned parameter string |
| `ws003-p013` | [boot slots and root-source selection](phase013/phase.md) | Completed (`q015`, 2026-08-27) | BR-T44 and BR-T46 pass native/overlay selection on all four platforms plus UUID/PARTUUID discovery-order regressions on both amd64 firmware paths |
| `ws003-p014` | [multi-source swap activation](phase014/phase.md) | Completed (`q015`, 2026-08-27) | BR-T45 and every BR-T46 file/raw/mixed swap cell pass actual page-out, page-in, and content restoration |
| `ws003-p015` | [four-platform boot-parameter acceptance](phase015/phase.md) | Completed (`q015`, 2026-08-27) | BR-T46 passes 31/31 production-loader cells: PC/AT 7, PC-98 6, amd64 BIOS 9, and amd64 UEFI 9 |
| `ws003-p016` | [static image boot parameters and Python-regression removal](phase016/phase.md) | Completed (`q023`, 2026-08-28) | BR-T47 and a fresh BR-T46 pass: one maintained definition feeds all x86 loaders and the kernel fallback; generated inputs, Python, and stale cross-build state are absent |
| `ws003-p017` | [UEFI LoadOptions firmware compatibility](phase017/phase.md) | Superseded by WS013 p003 | Historical BR-T48 converter policy is removed; CT-T016 proves LoadOptions is ignored by the required `/zedbsd.cfg` path |
| `ws003-p018` | [Latitude existing-FAT NVMe overlay installation and boot](phase018/phase.md) | planned | fg004のLatitude既存FAT候補。方式・媒体と現行前提を具体化。 |
| `ws003-p019` | [Latitude NVMe native installation and boot](phase019/phase.md) | planning | fg004のLatitude native候補。方式選択後に詳細化。 |
| `ws003-p020` | [Panasonic CF-SV7 early ACPI/interrupt bring-up](phase020/phase.md) | Completed (`q033`, 2026-08-30) | The single physical boot passed IRQ/XMM/HAL and continued through xHCI, USB storage, and VFS; early-init automated gates remain passing |
| `ws003-p021` | [Portable GPT image extent on larger USB media](phase021/phase.md) | Completed (`q034`, 2026-08-30) | Generic bounded-GPT host/QEMU gates pass and the frozen image boots successfully on the CF-SV7 through USB-root overlay/init/login |
| `ws003-p022` | [PC-9821V13 IPL stack and disk-read contract](phase022/phase.md) | cleared | 2026-09-12ユーザー完了報告で受け入れ。旧試行は履歴として保持。 |
| `ws003-p023` | [PC-9821V13 IPL entry localization](phase023/phase.md) | cleared | 2026-09-12ユーザー完了報告で受け入れ。旧試行は履歴として保持。 |
| `ws003-p024` | [PC-9821V13 Stage-1 fixed-read compatibility](phase024/phase.md) | cleared | 2026-09-12ユーザー完了報告で受け入れ。旧試行は履歴として保持。 |
| `ws003-p025` | [HAL clock-source split and amd64 SMP monotonic counter](phase025/phase.md) | Automatic milestone complete (`q066`); shared physical observation pending | The approved epoch/counter API pair, private calibration, complete admitted-CPU validation, same-build fault-injection rebuild gate, and positive/negative SMP KVM evidence pass; close it with p038's one final direct boot |
| `ws003-p026` | [ws003-p026](phase026/phase.md) | planned | 2026-09-11インストーラ実機bring-up計画。 |
| `ws003-p027` | [ws003-p027](phase027/phase.md) | planned | 2026-09-11インストーラ実機bring-up計画。 |
| `ws003-p028` | [ws003-p028](phase028/phase.md) | planned | 2026-09-11インストーラ実機bring-up計画。 |
| `ws003-p029` | [ws003-p029](phase029/phase.md) | planning | 2026-09-11インストーラ実機bring-up計画。 |
| `ws003-p030` | [ws003-p030](phase030/phase.md) | planning | 2026-09-11インストーラ実機bring-up計画。 |
| `ws003-p031` | [ws003-p031](phase031/phase.md) | planning | 2026-09-11インストーラ実機bring-up計画。 |
| `ws003-p032` | [ws003-p032](phase032/phase.md) | planning | 2026-09-11インストーラ実機bring-up計画。 |

`ws003-p003` was the sole authorized item in q012. Its physical result closes
the PCI/BAR/capability boundary and extracts the first device-enumeration stop
to `ws003-p004`. q013 further separates the independently testable P1
command/cancel lifecycle into p005. q013 review added p006 endpoint recovery
and p007 shared-DMA synchronization. Continued review added p008 direct device
association and p009 SuperSpeed context; all six consumed the same passing
BR-T34 U2 observation. The independent U3 stop was isolated in p010 and is
cleared by BR-T41.

The public implemented parameter contract is
[documented separately](../../docs/reference/kernel-boot-parameters.md). It
uses four boot filesystem slots (`boot0`--`boot3`), mutually exclusive native
`rootpart` and explicit overlay modes, four ordered swap sources
(`swap0`--`swap3`), and architecture-independent `init`. The old `boot=` and
`root=` spellings and the provisional `loop0=`/`loop1=` names are not retained.

The boot-parameter implementation added a Python-generated header after WS010
had removed Python from the supported x86 image paths. It also exposed stale
cross-build state when `ZEDBSD_BOOT_PARAMETERS_FILE` changed. `ws003-p016`
removes that mechanism, makes the image default maintained source, and adapts
the affected regressions without reopening the p011--p015 public contract.

## Current xHCI handoff decisions

- The former `capabilities (13)` compound `ENODEV` failure is cleared on both
  physical functions; both report HCIVERSION 1.2 and `reject=00000000:ok`.
- Enable PCI Memory Space before every BAR MMIO read, but keep bus mastering
  deferred until DMA/controller startup.
- Diagnose the original and reassigned BAR and raw capability registers before
  choosing between a local ordering fix and bounded high-address MMIO support.
- Keep U2 controller/storage enumeration separate from U3/U4 root and login
  acceptance. U2 and U3 are complete. BR-T41 also provides one init/login/root
  shell and X/`zterm` smoke result, but sustained I/O and recovery evidence are
  still required before full U4/U5 acceptance.

## Goals

- Boot zedBSD from USB on the Dell Latitude 5320.
- Boot zedBSD from USB on the Panasonic CF-SV7. Its post-RSDP early
  ACPI/interrupt stop and the fixed-size GPT image's larger-media root
  continuity are both cleared by p020/p021.
- Restore the native PC-98 disk image on the NEC PC-9821V13 without replacing
  its IPL/partition format with a PC/AT-compatible MBR.
- Reach a stable init/login shell while continuing to use the intended USB
  mass-storage root on each declared laptop target.
- Establish usable diagnostics and at least one project physical network path.
  The existing Latitude USB-Ethernet result satisfies the current network
  milestone; CF-SV7 networking is not part of p020 or M12 and requires a later
  explicit Phase if selected.
- Install from the ordinary USB system into existing NVMe FAT32 partitions and
  boot the overlay through UEFI without formatting; retain native-root
  installation as later p019 work.

## WS completion conditions

WS003 is complete when USB boot reaches tier U5 in the declared QEMU matrix and
on both declared laptop targets, the frozen integrated image reaches a usable
shell on five consecutive final-acceptance cold boots per target, the root
filesystem passes safe I/O tests, one documented physical network path passes
configuration and transfer tests, and p018 installs then boots the Latitude's
internal-NVMe overlay. The later native p019 is a separate milestone rather
than a p018 condition. Intermediate Phases use one consolidated physical
acceptance boot after their automated batch; they do not demand repeated human
boots for every internal change.

Targets:

- Dell Latitude 5320, Intel 11th-generation platform;
- Panasonic CF-SV7, exact DMI/CPU/device inventory pending;
- NEC PC-9821V13, native PC-98 fixed-disk BIOS path.

## 1. Objective

Boot a reproducible zedBSD USB image on each target laptop, retain the USB mass
storage device as the root backing store, reach a stable login shell, and
establish a diagnostic path. The project also retains at least one physical
network interface, currently the accepted Latitude USB-Ethernet path; CF-SV7
networking is a later, separately scoped target.
After that non-destructive base is stable, use the ordinary USB system to
install the non-formatting overlay path to existing internal-NVMe FAT32
storage on the Latitude. Native-root UEFI installation follows separately.

The target name alone is insufficient to select drivers. The exact machine
configuration and PCI/USB IDs are part of the first deliverable.

## 2. Definition of “USB boot works”

USB bring-up is divided into observable tiers so firmware success is not
confused with operating-system support:

| Tier | Required behavior |
| --- | --- |
| U0 — Firmware load | BIOS/UEFI discovers the USB device and transfers control to the zedBSD loader |
| U1 — Kernel entry | The loader loads the intended kernel and passes valid boot parameters |
| U2 — Kernel enumeration | The kernel enumerates the active USB host controller and mass-storage device |
| U3 — Root continuity | The kernel selects and mounts the intended USB-backed root, independent of discovery order |
| U4 — Stable system | init/login/shell work and sustained reads/writes complete without reset or corruption |
| U5 — Recovery | Timeouts, missing media, and controller errors fail visibly without hanging silently |

M1 requires U0–U5 in QEMU. M2 requires U0–U5 on the Latitude 5320, and M12
requires the same staged result on the CF-SV7 after p020 clears its first
early-HAL boundary.

## 3. Work items

| ID | Status | Deliverable | Dependencies | Acceptance gate |
| --- | --- | --- | --- | --- |
| BR-00 | In progress; Latitude WLAN/NVMe IDs captured, CF-SV7 inventory pending | Exact per-target inventory: BIOS, UEFI mode, Secure Boot state, CPU, GPU, xHCI, NVMe, WLAN, Ethernet/USB adapters, and IDs | Physical laptops | RTL8822CE `10ec:c822`/subsystem `10ec:c130` and SanDisk SN740 NVMe `15b7:5015` are recorded for the Latitude; remaining Latitude and CF-SV7 inventory is stored with commands and output summary |
| BR-01 | Planned | Reproducible USB image layout and safe write/verify procedure | Current build/image pipeline | A disposable image is generated twice consistently and its partitions/files are inspected |
| BR-02 | Planned | QEMU boot through `qemu-xhci` and `usb-storage` | BR-01, xHCI work in HW track | U0–U5 pass in the declared BIOS/UEFI matrix |
| BR-03 | Complete (`q015`) | Stable boot/root device selection rather than enumeration-order assumptions | Bootloader/kernel parameter review | BR-T46 UUID and PARTUUID cells pass on both amd64 firmware paths with the auxiliary disk enumerated first; the PC/AT root/swap alias is rejected before publication |
| BR-04 | Planned | Kernel xHCI and USB-storage continuity sufficient for USB root | xHCI, block layer, USB storage | Repeated QEMU I/O and reset/error tests pass |
| BR-05 | Complete | Latitude firmware-to-kernel USB boot | BR-00–BR-04 | U1 passes: corrected high-RSDP path reaches ACPI/IRQ/HAL on three cold boots |
| BR-06 | In progress; U3 complete, one shell/X smoke boot passed | Latitude USB root through init/login/shell | BR-05, `ws003-p003`--`p010` | BR-T41 provisionally confirms U3 and a basic U4 path; BR-T31 sustained I/O and, after U4 is frozen, BR-T30 five consecutive shell boots remain |
| BR-07 | Planned after device identification | The user's Realtek USB LAN adapter works as a host-mode physical network path | Exact USB VID:PID/controller family, WS004 driver, WS005 integration | The adapter reconnects and passes DHCP/static and transfer tests on the Latitude |
| BR-08 | Planned | At least one working physical network path | BR-00, BR-06, relevant NET/HW item | DHCP or static configuration, ping, and data transfer pass on hardware |
| BR-09 | Planned as `ws003-p018` | Install to and boot an overlay from existing Latitude NVMe FAT32 partitions | WS004 p025, WS013 p002/p003, WS019 p005 | No-format/no-GPT/no-NVRAM install followed by fallback/manual UEFI boot; final frozen image passes the declared repeatability gate |
| BR-10 | Future as `ws003-p019` | Install and boot a native Latitude NVMe root | WS019 p006/p007 and explicit later design | Native `rootpart=` boot is accepted without weakening BR-09 |
| BR-11 | Complete as `ws003-p020` (`q033`) | Clear the CF-SV7 post-RSDP early ACPI/interrupt boundary | Current amd64 UEFI loader/kernel, one consolidated physical acceptance boot | CF-SV7 passed IRQ/XMM/HAL and continued into USB/VFS without QEMU/Latitude regression |
| BR-12 | Complete as `ws003-p021` (`q034`) | Accept a coherent GPT-declared extent on larger USB media and continue CF-SV7 root/local-shell bring-up | BR-11 and the captured GPT extent mismatch | Strict corruption checks, sparse larger-media BIOS/UEFI login, and the final CF-SV7 USB-root overlay/init/login observation pass |

## 4. QEMU USB matrix

The first Phase extracted from BR-01–BR-04 must state the precise matrix. The
intended minimum is:

- UEFI and the currently supported legacy/BIOS path, unless the loader supports
  only one and the limitation is explicitly recorded;
- an xHCI controller with USB mass storage, because an 11th-generation laptop
  cannot be assumed to expose UHCI/EHCI to the operating system;
- the USB device as the only boot disk, followed by a case with an additional
  disk to expose device-order assumptions;
- root mount, sustained file I/O, sync, and clean reboot;
- missing or delayed root device and an injected/observable timeout path.

An EHCI case may remain as regression coverage, but it is not a substitute for
the xHCI path.

## 5. USB LAN target decision

The selected first network target is one of the user's Realtek USB Ethernet
adapters connected to the Latitude's host-mode xHCI controller. This is not
CDC ACM, which is a serial communication profile. A standards-compliant CDC
ECM or CDC NCM interface can bind by interface class/subclass/protocol without
requiring a product-specific ID. That does not make all USB Ethernet devices a
single class: Realtek RTL8152/RTL8153 devices commonly expose vendor-specific
interfaces and need a Realtek-family backend plus a VID:PID/quirk table.

The implementation direction is therefore a common `usbnet` data/lifecycle
core, CDC ECM/NCM class frontends, and a separate Realtek-family frontend when
the target descriptors require it. Supporting several adapters means adding
their IDs to that one family driver, not creating one driver per product.
Descriptors decide which frontend is needed; marketing brand alone does not.

On FreeBSD, collect:

```sh
usbconfig list
usbconfig -d ugenBUS.ADDRESS dump_device_desc
usbconfig -d ugenBUS.ADDRESS dump_curr_config_desc
```

Record `idVendor`, `idProduct`, interface class/subclass/protocol, and the
FreeBSD attached driver. CDC device/gadget mode is no longer a dependency of
the Latitude network milestone and may be reconsidered separately later.

## 6. Secure Boot policy

NVMe and Secure Boot are independent. The initial Latitude policy is UEFI boot
with Secure Boot disabled; zedBSD image signing and key enrollment are deferred.
This does not prevent either USB or NVMe storage from being used as the UEFI
boot source. BR-00 still records the firmware setting for reproducibility.

References:

- UEFI Secure Boot authenticates UEFI images rather than selecting the storage
  protocol: <https://uefi.org/specs/UEFI/2.10/32_Secure_Boot_and_Driver_Signing.html>
- NetBSD's UEFI installation procedure explicitly uses Secure Boot disabled
  with either NVMe or other disks:
  <https://wiki.netbsd.org/Installation_on_UEFI_systems/>
- The Latitude 5320 firmware documents Secure Boot as a boot-configuration
  option and does not support legacy boot mode:
  <https://www.dell.com/support/manuals/en-us/latitude-13-5320-2-in-1-laptop/latitude_5320_sm/boot-configuration>

## 7. Safety and handoff

- Use a dedicated, disposable USB device for image writes.
- Resolve the exact block-device path and verify its size/identity before every
  destructive host operation.
- Do not write the internal NVMe device during initial USB bring-up.
- Preserve framebuffer/console diagnostics until an independent diagnostic
  channel is proven.
- Record partial success at the highest U-tier reached, along with the earliest
  failing transition and its logs.

## 2026-09-12 fg009: PPC Open Firmware / APM+FAT

PowerBook G4 A1010 / 867MHzを移植先とし、まずQEMU mac99上で、Open Firmware → APM+FATの独自ローダ → zedboot.cfg → 同じFATのvmunix → PPCカーネル初期化を成立させる。後続でamd64上のUSB OHCI、PPCユーザーABI、USB root、rootfs.img/data.imgのループバック利用へ進む。今回は計画のみ。

最初の到達点はp033→p034→p035。rootfs.img/data.imgは後続p038。設定名は今回指定のzedboot.cfg（現行UEFIはzedbsd.cfg）、kernel=vmunix。独自ローダはXCOFFを第一候補とし、OFによるELF直接ロードに依存しない。

- [ws003-p033](https://github.com/awemorris/zedBSD/issues/367): OF起動契約・APM/FAT imageとXCOFFローダ入口 (planned)
- [ws003-p034](https://github.com/awemorris/zedBSD/issues/368): zedboot.cfg・FAT読み取り・PPC ELF handoff (planned)
- [ws003-p035](https://github.com/awemorris/zedBSD/issues/369): PPC HAL・mac99基板対応とカーネル初期起動 (planned)
- [ws003-p036](https://github.com/awemorris/zedBSD/issues/370): amd64でUSB OHCI・USBストレージを検証 (planning)
- [ws003-p037](https://github.com/awemorris/zedBSD/issues/371): PPCユーザーABI・libcとinit到達 (planning)
- [ws003-p038](https://github.com/awemorris/zedBSD/issues/372): PPC USB boot・rootfs.img/data.img統合 (planning)
- [ws003-p039](https://github.com/awemorris/zedBSD/issues/373): PPC/OHCI変更の最終規約・統合確認 (planning)

全体の共通契約は各Phase本文に記載。実行Queueは作成せず、既存の実行保留、fg006完了、他WSの判断を保持する。

</details>
