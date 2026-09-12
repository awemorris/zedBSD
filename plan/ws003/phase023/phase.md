<!-- awesome-plan-current:start -->

## 現在状態 — 2026-09-12

Status: cleared
Phase disposition: normal

2026-09-12、ユーザーがWS003 p022・p023・p024の完了を報告し、MarkdownとGitHubの更新を指示した。3 Phaseをcleared（disposition: normal）として受け入れ、fg006を完了とする。WS003はインストーラ等の残件があるためincompleteを維持する。新しいQueueは作成せず、保留中の実行は再開しない。

対象: [p022](https://github.com/awemorris/zedBSD/issues/88)、[p023](https://github.com/awemorris/zedBSD/issues/89)、[p024](https://github.com/awemorris/zedBSD/issues/90)。

根拠は今回のユーザー完了報告。既存Markdownの自動検証・旧試行の結果は履歴として保持する。今回エージェントがbuild・QEMU・実機検証を再実施したものではなく、新しいartifact hashやroot/init/login到達点は報告されていないため追加しない。旧試行のunclearedや未実施項目を過去に遡ってPASSへ変更しない。

<!-- awesome-plan-current:end -->

<details>
<summary>2026-09-12完了判断以前の計画・試行履歴（再実行指示ではない）</summary>

# WS003 Phase 023: PC-9821V13 IPL entry localization

<!-- v13-focus-current:start -->

## 2026-09-11 fg006: LBA0実行後の停止位置を特定する

Phase ID: `ws003-p023`
Status: uncleared
Phase disposition: normal
Selection: user-selected execution phase; not started in this task.

## 役割・手順

旧来の履歴専用扱いを解除し、fg006の2番目の実行対象とする。前提は[ws003-p022](../phase022/phase.md)の現行entry/stack/read契約と診断位置。
LBA0が実行されたこと自体を再び未確認に戻さず、LBA0内の読込み前後、BIOS read結果、LBA2 entry、partition-table/BOOT選択、PBR/BOOTZBSDへの転送のどこで停止するかを絞る。
既存のdiagnostic variantと音/debug出力を現行sourceに照合し、安全な位置で有限の識別点を使う。古いSENSE失敗経路を現在存在すると仮定しない。通常imageと診断imageを分離し、QEMUで識別点と正常継続を確認してから、名前・size・hashを固定して必要な実機観測を行う。

## 受け入れ・引き継ぎ

実機観測を特定artifactに対応付け、最後の成功点と最初の失敗段階、BIOSの結果または次段到達を、次の修正を選べる粒度で記録する。既存の実機証拠が十分なら同じ試験を再要求しない。診断が観測できない場合は識別不能を明記し、Phaseをclearedにしない。
根拠付きの停止境界と再現/観測方法を[ws003-p024](../phase024/phase.md)へ渡す。ここでは問題が解決したことと原因が絞れたことを区別する。必要な追加観測は得られる新しい情報を明示する。

## 共通条件と実行準備

Parent: [ws003](../ws.md)。Primary Milestone: MG003。Related Milestone: MG008。Focused Goal: fg006。
対象はPC-9821V13 / 64MB / CF-IDE。LBA0のロード・実行はユーザー確認済みで、その後ビープ停止する。現行artifactのhash、正確な停止命令・次段到達はまだ特定していない。
2026-09-11ユーザーがp022/p023/p024を具体的な実行Phaseとして選択。今回は計画更新であり、Phase状態をin-progressにせず、新しい実行Queue・時間枠は未設定。旧q037/q039/q043の実績や古いartifactを現在の合格・実行承認へ読み替えない。
[guardrail](../../guardrail.md)、`plan/coding-style.md`の該当全文、loaderの既存assembly規約・サイズ/ABI/配置制約に従う。native PC98形式、IPL1、既存の後段geometry検証を維持し、旧SENSE復活案・無根拠なdelay/reset/retryを実装指示にしない。HAL責務/hal.h変更、aggregate `make check`、commit、pushは行わない。
実行時には現行設定を固定し、対応するfocused image/layout検証と `make -j16 ZEDBSD_CONFIG=config/ci/config-pc98.mk`、維持対象qemu-pc98を使う。実際のコマンドは選択構成で確認する。実機に渡す成果物は一つずつsize/hashと目的を記録し、同じ観測を重複要求しない。
結果にはsource/config/image hash、観測の最後の成功点、未実施項目、残原因、次Phaseへの成果物を記録する。新しいQueueを選ぶ際にこの3 Phaseの現在の設計・前提・有限時間枠を対応付ける。

<!-- v13-focus-current:end -->

<details>
<summary>以前の設計・試行履歴（現行の役割は上記）</summary>


Last updated: 2026-08-31

Phase ID: `ws003-p023`

Status: Uncleared historical evidence (`q039`); every automatic gate passes,
physical handoff superseded by `ws003-p024`

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md), especially `BR-T55`

## Objective

Remove ambiguity from the still-failing PC-9821V13 boot without inventing a
different disk signature. Prove that the normal Stage 1 and installed LBA 0
contain ASCII `IPL1` at offsets 4--7, then produce one separately named,
immutable diagnostic image whose audible checkpoints distinguish firmware
handoff, Stage-1 SENSE/read failure, and Stage-2 entry in a single physical
boot.

The p023 physical request is retired: q043 p024 consumes this localization
result, removes the unnecessary Stage-1 SENSE, and owns the single current V13
artifact. Keep this Phase as historical evidence; do not boot its older image
first.

## Established facts

- The PC-9800 hardware reference places `IPL1` at address `0004h` in a
  fixed-disk IPL record.
- FreeBSD/pc98 places `.ascii "IPL1"` at offset 4.
- The maintained qemu-pc98 IDE and SCSI ROMs compare only offsets 4--7 before
  entering the IPL.
- The current zedBSD Stage 1 and disk image contain bytes
  `49 50 4c 31` at those exact offsets. The signature itself is therefore
  correct and must not be changed as a guessed repair.
- q037's fixture checked the tail and installed bytes but did not independently
  assert or negatively corrupt the four-byte `IPL1` field.
- The mutable `build/pc98/hdd-image.img` no longer has q037's recorded hash, so
  a physical result cannot be attributed safely unless this Phase publishes a
  new uniquely named artifact and hash.

## Fixed decisions

1. Retain the native PC-98 layout, `IPL1` at offsets 4--7, and `55 aa` at
   offsets 510--511.
2. Describe offsets 508 and 509 accurately as boot-menu version and reserved
   bytes. They are not a count of Stage-1 sectors.
3. Add an exact positive check for `IPL1` to both Stage 1 and installed LBA 0,
   plus a one-byte-corruption negative fixture which the checker rejects.
4. Keep tracing outside the normal production path. A build-time diagnostic
   variant may share maintained assembly, but ordinary `disk-image` output
   must remain silent and byte-stable apart from intentional unrelated source
   changes.
5. The diagnostic variant emits a bounded, countable audio group only after
   it owns a safe stack. It distinguishes at least Stage-1 entry, SENSE
   failure, LBA-2 read failure, and Stage-2 entry. It also writes the same
   checkpoint to QEMU debug port E9 so automatic execution can validate the
   selected path without interpreting host audio.
6. The physical handoff names exactly one diagnostic image and SHA-256. One
   V13 boot is requested; repeated human boots are deferred until a new fact
   justifies them.

## Implementation plan

1. Extend `BR-T54`/the production BIOS checker with the exact `IPL1` contract
   and a corrupted-copy rejection case; correct stale metadata terminology.
2. Add conditional Stage-1/Stage-2 audio/debug checkpoints which compile out
   of the normal image. Preserve the no-inherited-stack, register, size, and
   INT 1Bh contracts from p022.
3. Add a bounded PC-98 diagnostic-image target which starts from the checked
   normal image and replaces only the fixed IPL sectors with their diagnostic
   variants. Never publish it as the ordinary image.
4. Run `make -j16`, the production checker, `BR-T54`, and qemu-pc98 through
   login for the normal image. Run the diagnostic image far enough to prove
   its entry checkpoints and continuation automatically.
5. Copy the finished diagnostic image to a stable, explicit handoff pathname,
   record its size and SHA-256, and stop at one physical observation.

## Completion conditions

- exact positive and corrupted-negative `IPL1` checks pass for Stage 1 and the
  installed image;
- normal Stage 1 remains 512 bytes, retains its native layout and signatures,
  and the production PC-98 image reaches login under qemu-pc98 without an
  audio checkpoint;
- the diagnostic image has countable Stage-1/Stage-2 checkpoints, continues
  under qemu-pc98, and has a stable name, size, and SHA-256;
- `make -j16`, `make check-disk-image`, focused fixtures, and
  `git diff --check` pass;
- one boot of that exact diagnostic artifact on the PC-9821V13 reports the
  audible group or advances into the loader/kernel.

The Phase remains `uncleared` at the last condition until the user supplies
that one observation. An audio result defines the next disk/firmware boundary
without authorizing a partition-layout rewrite.

## Automatic result (2026-08-31)

The signature hypothesis is closed: both the ordinary Stage 1 and installed
LBA 0 contain `IPL1` at offsets 4--7. The production checker rejects a copy in
which the same one byte is corrupted in both places, so byte equality alone
cannot hide a bad magic field. The normal image retains `09 00 55 aa`, passes
its production checker, and reaches `login:` in the maintained qemu-pc98.

`ZEDBSD_PC98_IPL_DIAGNOSTIC` compiles entirely out of the normal fixed IPLs.
The Phase-owned Make fragment builds separate Stage-1/Stage-2 binaries and
patches only LBA 0 and LBA 2--15 of a copied normal image. Its finite audio
groups use the PC-98 BIOS timed-beep service after each stage owns its stack;
the same checkpoints appear as `P1E` and `P2E` on debug port E9. A disposable
qemu-pc98 run emitted exactly those two entry markers, no failure marker, and
continued through `init: system running` and `login:`. The immutable source
hash was unchanged by that run.

Verification evidence:

| Gate | Result |
| --- | --- |
| `make -j16` | PASS |
| production `make check-disk-image` | PASS |
| `BR-T54` exact/corrupted `IPL1` fixture | PASS |
| `BR-T55` diagnostic byte-range contract | PASS |
| maintained BR-T46 `pc98/default` | PASS 1/1 to `login:` |
| `BR-T55` diagnostic qemu-pc98 runner | PASS: `P1E`, `P2E`, init, login |
| `git diff --check` | PASS |

## Physical handoff: one boot only

Purpose: determine which earliest fixed-disk IPL boundary the PC-9821V13
actually reaches. Boot only this file once:

`/home/awe/zedBSD/build/handoff/ws003-p023-pc9821-v13-ad49c654.img`

| Property | Value |
| --- | --- |
| Size | 135,266,304 bytes |
| SHA-256 | `ad49c6542234d521f654be110e47703f27cdf73d3766209da413edd459888f9c` |

Report the audible groups in order, or a photo if the loader/kernel advances:

- no beep: firmware did not transfer control to zedBSD Stage 1;
- `1`, then `2`: Stage 1 entered, then SENSE failed;
- `1`, then `3`: Stage 1 entered, then the LBA-2 read failed;
- `1`, then `4`: Stage 2 entered; if the machine then advances, the fixed IPL
  path is clear;
- after `1`, `4`, a group of `5`, `6`, or `7`: respectively partition-table
  read, BOOT-name lookup, or partition-PBR read failed.

Do not run a second physical trial for this Phase. The first result is the
resume fact for a later bounded repair.

</details>

</details>
