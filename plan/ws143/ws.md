<!-- awesome-plan project=zedbsd record=ws143 -->

# WS143: Bluetooth（Settings の Bluetooth の頁の実体、ベータ2）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc §2）: p004 cleared。p003・p005 は i02 以降まで保留。次は q904（p005 i02、P1）。 2026-10-08 q902 P1 の照合: p001・p002 cleared、p003〜p006 は一部実装・試験済み）
Primary Milestone: MG006
Related Milestones: MG005
Parent: [Master](../master.md)
Queue: なし（2026-10-08 q902 の照合の時点。過去: q752・q860・q878・q883・q888（P2）、q896（P1））
Resume point: 2026-10-08 q902 P1 の照合: p003 i01・p004（T1-409・426）・p005 i01a〜c（T1-419・426・432・423）は Q1 の判定待ち。次: T1-446（p006 の 2 点の直しの再試験）、p005 i02（BR/EDR の HID host）→ i03（LE）→ i04（5330 の門）、p003 i02（intelbt と 5330）、p008 の UAT。p007 は 10/13 以降、p009 はベータ3。旧: p001 は cleared の提案。p002 に着手。
Target: **ベータ2**（2026-10-05 の朝の user の「ベータ4以降」の後、同日の再編「ベータ3とベータ3の内容を、ベータ2に移動します」で WS143 を含むベータ3・ベータ4 以降の項目をベータ2 に移した（master の記録、Q1 の確認）。前の指示: user「WS037, WS044,WS048,WS141, ... WS143, ... は、ベータ4以降としてください。」）
<!-- awesome-plan-current:end -->

## 単一目標

Settings で stub になっている Bluetooth の頁を実体にし、zedBSD で Bluetooth の device（まず 5330 の AX211 の Bluetooth の半分、USB 8087:0033）を見つけ、pairing・接続・切断ができるようにする。

## ユーザーの指示（2026-10-04 夜）

「SettingsでスタブになってるBluetoothは、ベータ2（時期未定）での実装項目として、WSだけ作っておいてください。」

## 範囲（p001 で設計して確定、2026-10-05 夕 ユーザーが §9 の D1〜D18 を全部推奨どおりに決定）

- kernel: USB の transport `bt-usb` と HCI の packet の char device `/dev/bluetoothN`、HID の入力の口 `/dev/input/bridge`、`/dev/system` の resume の
  class（D2・D3。D15 の決定で、firmware の load と HCI core は kernel から userland の daemon へ移した）。
- userland: Bluetooth の daemon `bluetoothd`（firmware の load、HCI・L2CAP・SMP・SDP・GATT・HID host、pairing・鍵の保存・接続の管理、
  特権の分離）と CLI `bt`。firmware の optional の package `intelbt`。
- desktop: libkeiland-backend の Bluetooth の口、compositor の拡張（kl_system_manager_v1）、Settings の Bluetooth の頁（今の stub を置き換え）、system bar の表示。Linux・FreeBSD の Keiland では各 OS の Bluetooth の仕組み（BlueZ など）を backend で包む。
- 最初の profile は HID（BR/EDR の HID と LE の HOGP のキーボード・マウス）。A2DP と PAN は要るが他の開発の後、別の WS（D1）。
- 外部の実装・license の境界（Guardrail・設計方針）。firmware は userland/firmware の規約。

## Phase

設計は [design.md](design.md)（第 3 版と §9 の決定）。各 Phase は着手の前に詳細設計を phase.md に書き、design-reviewer を通す（§10.1）。
HID の経路は p002〜p005、利用者に見える形は p006。

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws143-p001](phase001/phase.md) | 調査と設計（device・firmware・HCI・profile の範囲・desktop の経路・試験の方法） | cleared（2026-10-08 Q1） | | — |
| [ws143-p002](phase002/phase.md) | 5330 の descriptor と版を T1 で取る。kernel の `bt-usb`（普通と bootloader の経路、寿命、境界、backpressure）と `/dev/bluetoothN`（`include/uapi/bluetooth.h`、D2）。resume は `/dev/system` の POWER の `sleep.end` を使い、UAPI は足さない（詳細設計 §1）。試験の道具 `bt-probe`、試験の kernel の loopback の controller。host の試験（組み直しと境界、悪い device、取り外し） | cleared（2026-10-08 Q1、T1-384 QEMU PASS。5330 の passthrough は未、T1-378 は 5330 が Linux の時） | | p001 |
| [ws143-p003](phase003/phase.md) | firmware の package `intelbt`、bluetoothd の transport・firmware の load（§3）・HCI core・scan、CLI `bt show`・`bt scan`、Read Local Supported Commands の記録。T1 の passthrough で load と scan | in-progress（i01 は T1-402 PASS（2 回目、1 回目は FAIL）で Q1 の判定待ち。i02 の intelbt の package と 5330 の firmware の load・scan（D13）は未着手） | | p002 |
| [ws143-p004](phase004/phase.md) | L2CAP、SSP の event、LE の SMP（D10）、暗号（D5 b1、無ければ b2）、鍵の保存、特権の分離（D16 a）、`_bluetooth` の account（D17）、socket の口の権限（D8） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2）） | | | p003 |
| [ws143-p005](phase005/phase.md) | usb-hid の glue の共有の module への refactor と USB の回帰、`/dev/input/bridge`（D3）、hid-report.c の fuzz、SDP・GATT client、HID host（BR/EDR と HOGP）、再接続、切断で key を離す | in-progress（i01a T1-419 PASS、i01b T1-421 FAIL → T1-426 input-bridge-p005 PASS・T1-432 の順序依存の直し PASS、i01c T1-423 boot PASS（i2c-hid の touchpad の実機の回帰は 5330）。i02（BR/EDR の HID host）・i03（LE の HOGP）は未着手、i04（5330 の実機の門、Q20 で p006 の前の必須）も未） | | p004（i02 は p004 の cleared） |
| ws143-p006 | desktop: backend の口、zedBSD の backend、API と protocol の版、Settings の頁、system bar、pairing の確認の窓 | test-wait（q896 P1 実装、T1-438 一部 PASS（差 2 点）、直しの再試験 T1-446 は未実行） | | p005 |
| ws143-p007 | Linux の backend（D-Bus の拡張、BlueZ）、FreeBSD の未対応の表示 | planned（Linux・FreeBSD は 10/13 以降、2026-10-08 ユーザー） | | p006 |
| ws143-p008 | UAT（5330 の素の機械、D18。ユーザーの BR/EDR と LE のキーボード・マウス）、Wi-Fi との共存 | planned（5330 とユーザーの BR/EDR・LE の機器） | | p006、device の機種 |
| ws143-p009 | 規約の全文との照合と回帰 | planned（全文規約はベータ3、2026-10-08 ユーザー） | | p002〜p008 |
