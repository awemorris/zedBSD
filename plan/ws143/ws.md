<!-- awesome-plan project=zedbsd record=ws143 -->

# WS143: Bluetooth（Settings の Bluetooth の頁の実体、ベータ2）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-05 P1 が p001 に着手、q752）
Primary Milestone: MG006
Related Milestones: MG005
Parent: [Master](../master.md)
Queue: q752（P1、2026-10-05、p001）、q860（P2、2026-10-08、p001 の締め → p002 から HID の経路）
Resume point: p001 は cleared の提案（Q1 の判定待ち）。p002（bt-usb と `/dev/btN`）に着手。
Target: **ベータ2**（2026-10-05 の朝の user の「ベータ4以降」の後、同日の再編「ベータ3とベータ3の内容を、ベータ2に移動します」で WS143 を含むベータ3・ベータ4 以降の項目をベータ2 に移した（master の記録、Q1 の確認）。前の指示: user「WS037, WS044,WS048,WS141, ... WS143, ... は、ベータ4以降としてください。」）
<!-- awesome-plan-current:end -->

## 単一目標

Settings で stub になっている Bluetooth の頁を実体にし、zedBSD で Bluetooth の device（まず 5330 の AX211 の Bluetooth の半分、USB 8087:0033）を見つけ、pairing・接続・切断ができるようにする。

## ユーザーの指示（2026-10-04 夜）

「SettingsでスタブになってるBluetoothは、ベータ2（時期未定）での実装項目として、WSだけ作っておいてください。」

## 範囲（p001 で設計して確定、2026-10-05 夕 ユーザーが §9 の D1〜D18 を全部推奨どおりに決定）

- kernel: USB の transport `bt-usb` と HCI の packet の char device `/dev/btN`、HID の入力の口 `/dev/hid-host`、`/dev/system` の resume の
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
| [ws143-p001](phase001/phase.md) | 調査と設計（device・firmware・HCI・profile の範囲・desktop の経路・試験の方法） | in-progress（cleared の提案、q860） | — |
| [ws143-p002](phase002/phase.md) | 5330 の descriptor と版を T1 で取る。kernel の `bt-usb`（普通と bootloader の経路、寿命、境界、backpressure）と `/dev/btN`（`include/uapi/bluetooth.h`、D2）、`/dev/system` の resume の class。Read Version と HCI_Reset だけの小さな道具。host の試験（組み直しと境界、悪い device、取り外し） | in-progress（q860、P2 2026-10-08: kernel・bt-probe・host 試験まで、design-reviewer の review の反映中） | p001 |
| ws143-p003 | firmware の package `intelbt`、bluetoothd の transport・firmware の load（§3）・HCI core・scan、CLI `bt show`・`bt scan`、Read Local Supported Commands の記録。T1 の passthrough で load と scan | planned | p002 |
| ws143-p004 | L2CAP、SSP の event、LE の SMP（D10）、暗号（D5 b1、無ければ b2）、鍵の保存、特権の分離（D16 a）、`_bluetooth` の account（D17）、socket の口の権限（D8） | planned | p003 |
| ws143-p005 | usb-hid の glue の共有の module への refactor と USB の回帰、`/dev/hid-host`（D3）、hid-report.c の fuzz、SDP・GATT client、HID host（BR/EDR と HOGP）、再接続、切断で key を離す | planned | p004 |
| ws143-p006 | desktop: backend の口、zedBSD の backend、API と protocol の版、Settings の頁、system bar、pairing の確認の窓 | planned | p005 |
| ws143-p007 | Linux の backend（D-Bus の拡張、BlueZ）、FreeBSD の未対応の表示 | planned | p006 |
| ws143-p008 | UAT（5330 の素の機械、D18。ユーザーの BR/EDR と LE のキーボード・マウス）、Wi-Fi との共存 | planned | p006、device の機種 |
| ws143-p009 | 規約の全文との照合と回帰 | planned | p002〜p008 |
