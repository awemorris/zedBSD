<!-- awesome-plan project=zedbsd record=ws050 -->

# WS050: USB-C の UCSI driver

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG003
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: 2026-10-08 Q1 の判定（sweep-beta2-rc §2）: p001・p005 cleared。 2026-10-08 q902 P1 の照合: p002〜p004 cleared（2026-10-07 Q1）。p005 は 5330 の実機の値が出た: 2026-10-08 の UAT の log（../bugs/BUG-256/kernel-5330-uat-20261008.log）に `typec: display port TC1: connector 0 (group 0 position 1), bound (error 0)`・`TC2: connector 1 (group 0 position 2), bound`・TC3・TC4 は not bound、`ucsi: 2 connectors, 4 Alternate Modes`、ucsi の timed out・did not start は 0 → Q1 の判定待ち。p006 の実機の確認（/dev/typec、role・Alt Mode の操作、HPD の二つの出所の統合）は 5330、全文規約の分はベータ3。旧 resume: 2026-10-07 q834 P2: p005 の正常系（i915 の DP の report と UCSI 3.x の GET_CAM_CS の統合、host 94、kernel・I915_TESTS warning 0）。次は p006（実機）。それ以前: p004 の正常系（操作の口と 5 つの command、host 75/75）。次は p005（i915 との連携、WS051 の後）。それ以前: p003 の正常系（ACPI の transport・kernel の組み込み・/dev/typec、host 57/57、kernel warning 0、QEMU は T1 へ）。次は p004。それ以前: p002 の正常系（核・Type-C の層・host 31/31、仕様 1.2・3.1 と照合）。次は p003（ACPI の transport と kernel への組み込み）。それ以前: 2026-10-04 p001 の design.md 第 3 版
<!-- awesome-plan-current:end -->

## 目標

USB Type-C Connector System Software Interface（UCSI）の driver で、USB-C の connector の状態（接続・向き・電源の役割・データの役割・
Alternate Mode・USB PD の contract）を読み、変化を受け取り、必要な操作（role の切り替え、Alternate Mode の選択）ができる。

## きっかけ

2026-09-24 ユーザー指示: 「USB-C UCSIドライバの実装。」

## 前提と今あるもの

- PC の UCSI は ACPI の device（`_HID` の `USBC000`、`_CID` の `PNP0CA0`）で、共有の memory（OperationRegion）と `_DSM` で EC/PD controller と話す。
  → **WS049 の AML interpreter が前提。**
- USB の xHCI・hub・HID・storage の driver はある（`src/drivers/usb`、`src/drivers/pci/pci-xhci.c`）。Type-C の概念（connector・partner・Alternate Mode）の層は無い。
- 対象機は WS049 と同じ（仮定）。

## 範囲

- UCSI の command（`PPM_RESET`、`SET_NOTIFICATION_ENABLE`、`GET_CAPABILITY`、`GET_CONNECTOR_CAPABILITY`、`GET_CONNECTOR_STATUS`、`GET_ALTERNATE_MODES`、
  `GET_CAM_SUPPORTED`、`GET_CURRENT_CAM`、`SET_NEW_CAM`、`GET_PDOS`、`SET_UOR`・`SET_PDR`・`CONNECTOR_RESET`・`GET_CABLE_PROPERTY` ほか）と
  通知の割り込み（ACPI の `Notify`）。UCSI 1.x と 2.x の両方の配置と field（2026-10-04 ユーザーの決定 3、2.x は host の試験で）。
- Type-C の connector の層（状態、partner の Alternate Mode の一覧）。診断の text の `/dev/typec`（UAPI を足さない）、利用者への正式な通知は
  WS132 の `/dev/system`（決定 1）。
- DP Alt Mode の入口（WS051 が使う: mode に入る・出る（SET_NEW_CAM））。DP の HPD と pin の割り当ては、UCSI 2.0 以上で取れる時は UCSI から、
  i915 の TCSS・FIA から取れる時は i915 からも取り、両方を統合する（1.x は i915 だけ。決定 5 とその補足）。plug の向きは 2.0 以上は UCSI、
  1.x は i915 の TCSS から取る（決定 4、取れるかは WS051 と確かめる）。設計は [design.md](design.md) §13。

## 受け入れ

- 対象機で各 USB-C port の抜き差し、向き、電源の役割、partner の Alternate Mode が読め、抜き差しの通知が届く。
- 規約の全文、build、boot test。実機の証拠と QEMU の証拠を分ける（QEMU には UCSI が無いので、試験は実機が中心）。

## Phase 一覧

| Phase | 内容 | Status | 依存 | 対象 |
| --- | --- | --- | --- | --- |
| [ws050-p001](phase001/phase.md) | 調査と設計: UCSI の仕様（1.2/2.x）、対象機の `USBC000` の AML（共有 memory の配置、`_DSM` の function）、Type-C の層の設計、公開の形 | in-progress → Q1 の判定待ち（第 3 版、§10 の判断は 2026-10-04 ユーザーが決定、p002〜p005 がこの設計で実装済み） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）） | | 設計文書 |
| [ws050-p002](phase002/phase.md) | UCSI の核・Type-C の層の状態と kernel 内の口、1.x・2.x の配置、疑似の PPM と記録の再生の host の試験 | cleared（2026-10-07 Q1） | p001、仕様書での確認、実機の VERSION と mailbox の記録（UAT） | `src/drivers/typec/` |
| [ws050-p003](phase003/phase.md) | ACPI の transport と kernel への組み込み（`ucsi-acpi.c`、attach、Notify、thread、`/dev/typec`） | cleared（2026-10-07 Q1、T1-330） | p002、ws049-p017（q696、WS049 の公開の口）、ws049-p007・p008 | 同上 |
| [ws050-p004](phase004/phase.md) | 操作の command（CONNECTOR_RESET・SET_UOR・SET_PDR・SET_NEW_CAM・GET_CABLE_PROPERTY）と操作の口 | cleared（2026-10-07 Q1） | p003 | 同上 |
| [ws050-p005](phase005/phase.md) | i915 との連携（HPD・pin・向きの二つの出所の統合、TC の port と connector の対応付け） | in-progress → Q1 の判定待ち（host 94・build warning 0。5330 の実機 2026-10-08 の log で TC1・TC2 の connector の対応が bound（error 0）、ucsi の初期化に timeout 無し） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）。5330 の log） | | 同上 |
| ws050-p006 | 実機の確認と規約の全文の確認 | planned（実機は 5330、全文規約の分はベータ3、2026-10-08 ユーザー） | p003〜p005、実機 | WS の全 source |

## 2026-10-04 予定（Q1）

ユーザー「次の新規実装項目は、USB-C の DisplayPort Alternate Modeの実現を目標にします。その次が電源管理です。これらは併走できると思います。共通のpredecessorがAMLですね。」→ [queue.md](../queue.md) の q679（WS050 p001）・q680（WS051 p001）・q681（WS052 p001）。設計は WS049 の q677（BUG-165、DSDT）と並走、実装は q677・q678（ws049-p007）の後。

## 2026-10-04 ユーザーの決定（§10 の 5 項目、Q1 経由、design.md の末尾に記録）

公開は `/dev/typec`（診断）→ 正式は `/dev/system`。role の切替・CONNECTOR_RESET・SET_NEW_CAM は**範囲に入れる**（目標は今のまま、Phase を足した）。
UCSI 1.x と 2.x の両方。向きは 1.x でも i915 から。HPD・pin は UCSI 2.0 以上と i915 の両方の経路（1.x は i915 だけ）。

## 2026-10-07 P1: 5320 の UCSI の timeout（q847、Q1 の依頼）

- 症状（5320、Tiger Lake、Dell subsys 1028:0a1f、zedBSD b35bf04、毎回の起動）: `ucsi: 2 connectors, 4 Alternate Modes, features 0x000014` の後に
  `ucsi: command 0x12 timed out (CCI 0x00000000)`・`ucsi: the PPM did not start (error 42)`（[log](../ws118/tests/k5320-20261007-greeter-fail.log)、
  2026-10-07 の 5320 の dmesg でも同じ。読むだけ）。`/dev/typec` は connector 1（PD の sink、generation 7）・connector 2（detached、generation 6）まで読めていた。
- 原因（source と実機の log からの推定）: `ucsi_pending_handle` が connector の変化を ACK_CC_CI の CONNECTOR_CHANGE だけで acknowledge していた。
  この PPM は completion の無い change だけの ACK の後に次の command に答えなくなる（CCI が 0 のまま、次の GET_CONNECTOR_STATUS が 5 秒で timeout）。
  Linux v6.8 の ucsi_acpi は同じ不具合を Dell の全機で `ucsi_dell_sync_write()` で避ける（GET_CAPABILITY の dummy を先に送り、その completion と change を
  1 回の ACK で）。AML の確認は未実施（ACK の振る舞いは EC の firmware の中で、AML からは見えない）。
- 修正: `src/drivers/typec/ucsi.c` の `ucsi_acknowledge_change`（GET_CAPABILITY を流し、CONNECTOR_CHANGE と COMMAND_COMPLETED を 1 回の ACK_CC_CI で）。
  仕様（§4.5.4）が許す形なので全 PPM に適用（kernel に DMI が無く Dell を判別できない、5320・5330 はともに Dell）。
- 確かめ: `make -C plan/ws050/tests` 94 PASS（疑似の PPM に「change だけの ACK の後は答えない」5320 の規則を足した。旧の ucsi.c ではこの規則で 10 件 FAIL、
  `two-changes` が error 110 で 5320 と同じ形になることを確かめた）、ACPI の host（5330 の table）PASS、`kernel-check` warning 0（stack の最大は
  `ucsi_pending_handle` 584 byte、旧 312）、kernel の build warning 0。
- 未実施: 5320・5330 の実機（kernel の入れ替えは Q1 経由。合否: dmesg に timeout と「did not start」が無く、`/dev/typec` の 2 connector が読めること）。

## 5320 の実機（2026-10-07 Q1）

UCSI の ACK の直し（9f836b951）入りの main 173fa2115 の kernel を 5320 に入れ（ユーザーの承認・再起動）: kernel.log に `ucsi: version 1.0.0, 1.x mailbox`、`ucsi: 2 connectors, 4 Alternate Modes`、`command 0x12 timed out`・`the PPM did not start` は無い、/dev/typec あり。5330 の実機は未実施。
