<!-- awesome-plan project=zedbsd record=ws003-p024 -->

<!-- awesome-plan-current:start -->

Status: cleared
Phase disposition: normal
Lifecycle: closed
Current Focus: No

既存の完了・受け入れを保持。2026-09-12のWS003終了にあたり現在状態を明示。今回新たな試験・受け入れを追加していない。

<!-- awesome-plan-current:end -->

<details>
<summary>終了前の計画・試行履歴（現在の再実行指示ではない）</summary>

<details>
<summary>2026-09-12完了判断以前の計画・試行履歴（再実行指示ではない）</summary>

# WS003 Phase 024: PC-9821V13 Stage-1 fixed-read compatibility

<!-- v13-focus-current:start -->

## 2026-09-11 fg006: 特定した原因を修正してV13の通常起動を確認する

Phase ID: `ws003-p024`
Status: uncleared
Phase disposition: normal
Selection: user-selected execution phase; not started in this task.

## 役割・手順

fg006の3番目の実行対象。[ws003-p022](../phase022/phase.md)の現行契約と[ws003-p023](../phase023/phase.md)の実機停止境界を消費する。旧q043のSENSE除去結果は履歴であり、同じ変更を再度実装することを目標にしない。
根拠から特定したIPL/BIOS read/次段転送の不整合に対して最小修正を設計・実施する。後段の別原因が判明した場合はその原因・影響範囲を具体化して扱い、無関係なHAL再設計・全disk形式変更へ広げない。
原因を捕捉するfocused回帰と維持対象qemu-pc98の通常起動を確認し、診断専用変更を外した同一hashの通常imageでPC-9821V13を確認する。

## 受け入れ・Goalとの関係

報告されたビープ停止が解消し、通常loaderからkernelへの起動処理が進むことを実機の観測で確認する。実際に到達したroot/init/login段階と未到達点を明記し、QEMUだけで実機成功としない。
今回のboot改善はfg004のインストール受け入れに成果を渡す。インストーラ本体・QEMU /sbin・PCI/USB設定はそれぞれ既存の別Phaseの担当であり、本Phaseへ無断で取り込まない。loader後に別課題で止まる場合はboot修復の成立と残課題を区別し、機種全体のインストール成功を主張しない。
[BUG-013](../../bugs/BUG-013.md)の解決判定は同一のビープ停止に対する実機結果に基づく。WS025-p032のcleared/closed判断は変更しない。source/config/hash、修正根拠、通常実機結果をWS003と後続のインストール計画へ引き継ぐ。

## 共通条件と実行準備

Parent: [ws003](../ws.md)。Primary Milestone: MG003。Related Milestone: MG008。Focused Goal: fg006。
対象はPC-9821V13 / 64MB / CF-IDE。LBA0のロード・実行はユーザー確認済みで、その後ビープ停止する。現行artifactのhash、正確な停止命令・次段到達はまだ特定していない。
2026-09-11ユーザーがp022/p023/p024を具体的な実行Phaseとして選択。今回は計画更新であり、Phase状態をin-progressにせず、新しい実行Queue・時間枠は未設定。旧q037/q039/q043の実績や古いartifactを現在の合格・実行承認へ読み替えない。
[guardrail](../../guardrail.md)、`plan/coding-style.md`の該当全文、loaderの既存assembly規約・サイズ/ABI/配置制約に従う。native PC98形式、IPL1、既存の後段geometry検証を維持し、旧SENSE復活案・無根拠なdelay/reset/retryを実装指示にしない。HAL責務/hal.h変更、aggregate `make check`、commit、pushは行わない。
実行時には現行設定を固定し、対応するfocused image/layout検証と `make -j16 ZEDBSD_CONFIG=config/ci/config-pc98.mk`、維持対象qemu-pc98を使う。実際のコマンドは選択構成で確認する。実機に渡す成果物は一つずつsize/hashと目的を記録し、同じ観測を重複要求しない。
結果にはsource/config/image hash、観測の最後の成功点、未実施項目、残原因、次Phaseへの成果物を記録する。新しいQueueを選ぶ際にこの3 Phaseの現在の設計・前提・有限時間枠を対応付ける。

<!-- v13-focus-current:end -->


## 旧設計・旧試行の履歴

以下は旧source/artifactに対する設計・結果。現行の範囲と判断は上記を優先する。


Last updated: 2026-08-31

Phase ID: `ws003-p024`

Status: Uncleared (`q043`); source/binary/QEMU milestone passes, one exact
PC-9821V13 boot remains and the independent Noct host-CLI regression blocks
the production Make-owned checker

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md), extending `BR-T54` and `BR-T55`

## Objective

Restore entry on the NEC PC-9821V13 by removing the unnecessary AH=`84h`
SENSE transaction from the LBA-0 Stage-1 IPL. Stage 1 reads one fixed physical
sector, CHS 0/0/2, and does not consume geometry; it should therefore issue
that bounded AH=`06h` read directly. Preserve every later geometry-dependent
SENSE path, the native PC-98 disk layout, and the already-correct `IPL1`
signature.

The implementation decision is closed and can enter a Queue without another
human design choice. The Phase still ends with one exact physical boot because
QEMU already accepts both the old and current Stage-1 sequences.

## Established boundary

- `IPL1` is present at LBA-0 offsets 4--7 in both the built Stage 1 and the
  installed image. Its positive and corrupted-negative checks pass.
- The ordinary and diagnostic images reach `login:` under the maintained
  qemu-pc98 configuration, while the PC-9821V13 still beeps and stops before
  loader text.
- Stage 1 currently owns a private stack, preserves the firmware entry
  registers and boot-device identity, performs AH=`84h` SENSE, and then reads
  the invariant CHS 0/0/2 sector. None of the returned geometry is used.
- The earlier zedBSD Stage-1 path and the independently inspected native
  PC-98 fixed-disk IPL pattern enter their fixed CHS read without this
  preliminary SENSE. The new SENSE call is therefore the narrowest remaining
  firmware-compatibility difference at the observed boundary.
- `partition-pbr.S` and `bootzbsd.S` use SENSE for geometry-dependent later
  work. Their validation is not implicated by this Stage-1 result and must
  remain intact.

This Phase deliberately supersedes only p022's Stage-1 SENSE requirement. It
does not rewrite p022/p023's historical evidence or relax any later disk
validation.

## Fixed decisions

1. Delete Stage 1's `bios_sense` call, helper, `1S` failure path, and dead
   diagnostic branch.
2. Keep the private `SS:SP`, saved `SI`/`DI`, normalized fixed-disk `boot_id`,
   explicit `ES:BP`, `BX=512`, AH=`06h`, CHS 0/0/2, carry check, and `1R`
   read-failure diagnostic.
3. Do not change `lba2.S`, `partition-pbr.S`, or `bootzbsd.S` merely to make
   them resemble Stage 1. In particular, strict SENSE remains in the PBR and
   BOOTZBSD paths which actually consume disk geometry or enumerate devices.
4. Retain the native PC-98 LBA-0/LBA-1/LBA-2 layout, `IPL1`, boot-menu bytes
   `09 00`, trailing `55 aa`, native partition entries, and `BOOT` selection.
   Do not add a PC/AT partition entry or GPT compatibility path.
5. Keep successful production boot silent. The diagnostic variant retains
   finite Stage-1-entry, Stage-1-read-failure, and Stage-2-entry checkpoints;
   it no longer advertises a SENSE-failure checkpoint that cannot occur.
6. Do not add a guessed reset, delay, geometry substitution, or retry loop.
   Any further physical stop is localized by the existing bounded diagnostic
   path before another repair is designed.

## Implementation plan

1. Simplify `bootloader/pc98/disk-ipl.S` to prepare the target and issue the
   fixed AH=`06h` read immediately after boot-device normalization.
2. Remove only the resulting dead Stage-1 SENSE code and update diagnostic
   comments/groups without changing the Stage-1 sector budget.
3. Update the focused source/binary contract so it rejects AH=`84h` in Stage
   1, requires the complete fixed AH=`06h` CHS 0/0/2 transaction, and proves
   that SENSE still exists in `partition-pbr.S` and `bootzbsd.S`.
4. Re-run exact `IPL1`, `09 00`, `55 aa`, native LBA-1/LBA-2, no-PC/AT-entry,
   installed-byte, and fixed-size checks. A separately compiled Stage 1 must
   still be exactly 512 bytes.
5. Build the ordinary PC-98 image with `make -j16`, run its production image
   checker, and boot a disposable copy under the maintained qemu-pc98 command
   through `init: system running` and `login:` with no IPL failure marker.
6. Rebuild the diagnostic variant, prove Stage-1 and Stage-2 entry markers in
   qemu-pc98, then copy it to a uniquely named immutable handoff path and
   record its size and SHA-256.
7. Ask for one PC-9821V13 boot of that exact artifact. A loader/kernel screen
   clears this compatibility Phase; a bounded failure group identifies the
   next Phase. Do not request repeated hardware boots here.

The focused Stage-1 object/layout checks may run independently of the current
WS008 host-CLI regression. If that unrelated regression prevents the ordinary
integrated `make -j16` or maintained Noct runners, record the exact blocked
gate and leave this Phase `uncleared`; do not patch Noct inside WS003.

## Completion conditions

- LBA-0 Stage 1 contains no AH=`84h` SENSE transaction or `1S` branch;
- its one AH=`06h` read still publishes the complete fixed CHS 0/0/2 and
  buffer/register contract, checks carry, and reports `1R` on return failure;
- PBR and BOOTZBSD SENSE/geometry validation remain source-checked;
- Stage 1 remains 512 bytes and the image retains `IPL1`, `09 00`, `55 aa`,
  the native partition table/selector, and zero PC/AT partition entries;
- focused positive/negative fixtures, the production image checker, and
  `git diff --check` pass;
- the ordinary disposable image reaches init/login under the maintained
  qemu-pc98 command and the diagnostic image reaches both entry markers;
- one uniquely named/hash-recorded diagnostic artifact advances beyond the
  original beep boundary on the PC-9821V13.

## Reconsideration boundary

Stop and extract a new Phase if the physical result reaches Stage 2 and then
fails in its partition-table/PBR path, if the fixed AH=`06h` call returns an
explicit error, or if the medium is not exposed as the expected fixed-disk
BIOS device. None of those outcomes authorizes weakening the PBR/BOOTZBSD
SENSE checks or changing the native partition format in this Phase.

## q043 automatic result (2026-08-31)

The bounded Stage-1 correction is implemented. LBA 0 no longer issues
AH=`84h`, owns no `bios_sense` helper or `1S` branch, and still performs one
ordered AH=`06h` transaction with `ES:BP=1fc0:0000`, `BX=512`, `CX=0`,
CHS 0/0/2, saved firmware `SI`/`DI`, and a carry-to-`1R` failure edge. The
independent review found no register, BIOS, or layout defect in that sequence.
PBR and BOOTZBSD still contain and validate their geometry-dependent SENSE
paths.

The ordinary and diagnostic Stage-1 binaries are exactly 512 bytes. The
ordinary installed image retains `IPL1`, `09 00 55 aa`, a zero PC/AT
partition-entry region, the native PC-98 partition table at LBA 1, and the
byte-identical selector at LBA 2--15. The direct fixed-read/source/binary/layout
contract and its corrupted-`IPL1` negative fixture pass. The strengthened Noct
contract also confines every required register assignment to the one fixed
read block and rejects an extra INT 1Bh transaction; it cannot currently run
because of the independently tracked `ws008-p010` host-tool regression.

QEMU evidence uses qemu-pc98 SHA-256
`9400ec81d8ce99e89fafa580a5bf6adfaeb9e8be15a8f0eed427710bfd7e12da`
and the maintained argv from this Phase. The ordinary image SHA-256
`b62c958face27ed31e74e5725117b0dd2cda57cd3f57c33044f984310d9a804e`
reaches `init: system running` and `login:`. The diagnostic image emits only
`P1E` then `P2E`, emits no fixed-IPL failure marker, and also reaches init and
login. Both disposable runs leave their source image unchanged.

| Gate | Result |
| --- | --- |
| Stage-1 512-byte build and `IPL1`/tail checks | PASS |
| fixed-read/source/binary/native-layout and corrupted-magic checks | PASS |
| PBR/BOOTZBSD geometry-SENSE source contract | PASS |
| ordinary qemu-pc98 continuation | PASS: init/login |
| diagnostic qemu-pc98 continuation | PASS: `P1E`, `P2E`, init/login |
| independent corrective review | PASS; test-strengthening recommendations applied |
| `make -j16` compile/link path | Kernel link PASS; post-link Noct invocation rejects `--path=tools/build` |
| Make-owned Noct contracts and p024 target | BLOCKED by `ws008-p010`; direct invocation also lacks required `zbReadFile`/`zbShellQuote` module symbols |
| `git diff --check` | PASS |

The p024 Make target deliberately reuses p023's diagnostic-only Stage-1 and
Stage-2 build directories, then copies and hashes the resulting bytes at the
p024 handoff path. The p024 contract compares those two images byte-for-byte;
the current manual equivalent passes. No production image uses diagnostic
code.

## Physical handoff: one boot

Purpose: check whether removing only the unused Stage-1 SENSE transaction
allows the PC-9821V13 firmware to reach the fixed CHS read and Stage 2. Boot
this exact diagnostic image once:

`/home/awe/zedBSD/build/handoff/ws003-p024-pc9821-v13-fixed-read-diagnostic.img`

| Property | Value |
| --- | --- |
| Size | 135,266,304 bytes |
| SHA-256 | `7d4e7d674e4acfc171133567f2969c2d29a09cbbcdde1566d9f05985450eee50` |

Report the screen reached and the audible groups in order:

- no beep: firmware did not enter zedBSD Stage 1;
- `1`, then `3`: Stage 1 entered but its fixed LBA-2 read failed;
- `1`, then `4`: Stage 2 entered; a later group `5`, `6`, or `7` respectively
  localizes partition-table read, `BOOT` lookup, or partition-PBR read;
- loader/kernel output after `1`, then `4`: the p024 compatibility boundary
  is cleared.

No second physical attempt is required in this Phase. Until this one result
arrives, p024 remains `uncleared`; q043 and later independent Queues may still
finish.

</details>

</details>
