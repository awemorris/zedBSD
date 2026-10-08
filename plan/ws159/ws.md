<!-- awesome-plan project=zedbsd record=ws159 -->

# WS159: native のタッチパッド（Intel LPSS の I2C・ACPI の I2C-HID・HID の touchpad）と compositor のタッチパッドの層

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p003・p004 cleared、p001（設計）は Q1 の判定、p002・p006 は実装・host・T1-113 の boot まで（実機の I2C・GPIO の割り込みは WS183 と一緒に 5330）、p005 は実機の UAT。旧: planning）
Primary Milestone: MG006
Related Milestones: MG003
Parent: [Master](../master.md)
Queue: q713（**ベータ1**、2026-10-05 ユーザーのクリックの回答。P1・P2 が空いたら優先して着手）
Resume point: p001（設計）から。WS049（ACPI AML）の後（ユーザー）。WS049 は p007〜p009・p016・p017 まで済み（実機の UAT 待ち）。

**UAT の image の条件（2026-10-05 Q1）**: main 37d2a16（P1 1a41db3、i2c-hid の probe で `thread_start` を呼ぶ修正）以降で作る。これより前の build では実機の touchpad の thread が動かない（host の試験は thread を自分で走らせていたので見逃した。host-i2c-hid に thread_start の check を追加）。
<!-- awesome-plan-current:end -->

## 単一目標

5330 などのタッチパッドを PS/2 の互換の mouse ではなく I2C-HID の native の device として扱い、多点（MT）と clickpad の押し込みを kernel の evdev に出し、compositor のタッチパッドの層で tap・tap-drag・押し込み・2 本指のスクロール・gesture を作る。

## 経緯とユーザーの指示（2026-10-05）

- P1（q700）の調べ: 5330 のタッチパッドは今 "PC/AT PS/2 mouse"・"i8042: mouse id=0 packet=3"（3 byte、wheel なし、`plan/uat/2026-10-04/dmesg.txt:52,360`）。I2C HID の driver は tree に無い。compositor が受けるのは相対の motion と BTN_LEFT/RIGHT だけで、tap は touchpad の firmware が合成している。
- ユーザー（原文）:「タップダウンを取れない？それは違うと思います。PS/2でもタップダウンもイベントとして取れているように見えました。でも、2本指スクロールは取れないですね。ACPI AMLを実装したあと、I2C-HIDを実装しましょう。compositorのtouchpad層も作りましょう。WSを立ててください。」
- → タップダウン（BUG-190）は PS/2 の経路のまま P1 が調べ直す（q700）。2 本指のスクロール（[BUG-156](../bugs/BUG-156.md)）と押し込み（[BUG-167](../bugs/BUG-167.md)）、タッチパッドの tap-drag の仕様（[BUG-166](../bugs/BUG-166.md) の native の部分）はこの WS で扱う。

## 許可（2026-10-05 ユーザー）

「5330を起動しました。操作はすべて許可します。」→ 5330 の Linux（`ssh awe@10.0.30.3`、Debian 6.19.13、sudo は password 無し）で、タッチパッドの調べに要る操作を全部してよい（I2C-HID の device `i2c-VEN_06CB:00`（Synaptics）の情報、HID の report descriptor、evdev の capability と記録、ACPI の table、driver の bind・unbind など）。採った物は `plan/ws159/tests/` に置く。iGPU と AX211 の同時の passthrough はしない（Guardrail）。host の設定を変えたら記録し、終わったら戻すか、残すなら記録する。

## 範囲（p001 で設計して確定）

1. kernel: Intel LPSS の DesignWare I2C の controller の driver（PCI の device、ACPI の `_CRS` の解析は WS049 p017 の `drv_acpi_resources_walk`）、ACPI の PNP0C50（I2C-HID）の発見と HID の descriptor の取得、HID の report の解析（digitizer: contact・x・y・tip・button・contact count）、evdev への MT（`ABS_MT_*`・`BTN_TOUCH`・`BTN_TOOL_*`）と BTN_LEFT（clickpad）の出力。割り込みは GPIO（ACPI の GpioInt）か APIC。PS/2 の互換の mouse との二重の入力を止める（I2C-HID が動く時は PS/2 の aux を使わない）。
2. compositor（または libkeiland-backend の入力）: タッチパッドの層。絶対座標から相対の motion（加速は ws089-p024 の曲線）、tap で click、tap-drag（タップの直後約 300ms 以内に触れて動かすとドラッグ、BUG-166 の仕様）、clickpad の押し込み（物理の押し込みのまま動かすとドラッグ）、2 本指のスクロール（自然な方向は Touchpad の設定、既定 ON）、端からの gesture（WS142 の 2・3 本指）、手のひらの除外。Linux・FreeBSD の Keiland は libinput 相当の層を backend で使う。
3. 設定: Settings の Touchpad の頁（ws089-p024）。
4. 試験: host の試験（HID の report の解析、touchpad の層の状態機械）、QEMU（I2C-HID の device は QEMU に無い見込み → host の試験と実機）、実機（5330）の UAT。

## Phase（案）

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [ws159-p001](phase001/phase.md) | 設計（LPSS I2C・I2C-HID・HID の digitizer・evdev の MT・PS/2 との切替・compositor の touchpad の層・試験） | in-progress（2026-10-05 q713-i01 P1: 5330 の Linux の採取と設計を書いた、Q1 の確認待ち） | WS049 |
| [ws159-p002](phase002/phase.md) | LPSS の DesignWare I2C の driver と ACPI の I2cSerialBus・GpioInt | in-progress（2026-10-05 q713-i01 P1: 実装・host・build、QEMU と実機の待ち） | p001 |
| [ws159-p003](phase003/phase.md) | I2C-HID と Precision Touchpad、EVIOCGPROP、input-inject の touchpad | cleared（2026-10-05 Q1、T1-100） | p002 |
| [ws159-p004](phase004/phase.md) | compositor の touchpad の層（tap・tap-drag・押し込み・2 本指のスクロール） | cleared（2026-10-05 Q1） | p003（host の試験は先に） |
| [ws159-p005](phase005/phase.md) | 実機の UAT と全文の規約 | planned（2026-10-05 P1: UAT の手順書（touchpad・電池と事象・Windows キー・network の詳細・su/sudo/passwd と Users・About）と image の config `tests/config-amd64-uat.mk`・回収の script `tests/uat-collect.sh`。build と UAT は Q1 がユーザーと） | p004、p006 |
| [ws159-p006](phase006/phase.md) | GPIO の割り込み（INTC1055 の pinctrl の GpioInt、level・active low）で sampling（6/25 ms の MMIO）をやめる（電池のため、Q1 の追加、2026-10-05） | in-progress（2026-10-05 q720-i01 P1: ユーザーの `hal_irq_set_mode` で割り込みを実装（amd64 の IOAPIC、kernel の wrapper、Intel GPIO の pad の割り込み、I2C-HID の割り込み駆動、失敗時は 4 ms の監視）。build・host の試験まで、5330 は未実施） | p003 |

## 2026-10-06

- BUG-210 resolved: firmware が LPSS I2C の BAR0 を割り当てないので PCI の root の窓（_CRS）から割り当てる（67b70410）。実機で touchpad と 2 本指の scroll が動いた（ユーザー）
