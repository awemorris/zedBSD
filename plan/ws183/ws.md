<!-- awesome-plan project=zedbsd record=ws183 -->

# WS183: I2C HID の割り込み（Extended Interrupt）と Tiger Lake の GPIO の group

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG003
Related Milestones: —
Objectives: O2
Parent: [Master](../master.md)
Queue: P1 の列（2026-10-08）
Target: ベータ2（他のベータ2 の WS と同じ優先度、後回し。2026-10-07 ユーザー）
Resume point: 2026-10-08 q902 P1 の照合: p001 は 5320 の実機待ち（5320 は電源オフ、ユーザーの時期）。p002 は 5330・5320 の実機の tap の UAT 待ち（QEMU に touchpad は無い）。関連: 5330 の touchpad の GPIO の割り込みが止まる [BUG-261](../bugs/BUG-261.md)（P2 q876 修正済み、5330 の確認待ち）。旧: p001 test-wait（2026-10-08 P1: 実装・build・host 試験まで。5320 の実機の確認がユーザーの時期）。
<!-- awesome-plan-current:end -->

## 目標（2026-10-07 ユーザー）

「タッチパッドは独立WSにして、ほかベータ2WSと同じ優先度で後回しにします。」

- 5320 の touchpad（TPD0、0488:1024）の `_CRS` は GpioInt でなく Extended Interrupt（APIC IRQ 51、level、active low）。i2c-hid は GpioInt だけを扱うので sampling（6・25 ms）で読んでいる（[ws118-p007](../ws118/phase007/phase.md)）。i2c-hid に Extended Interrupt の経路（`hal_irq_set_mode` で level・active low）を足し、割り込みで読む。
- Tiger Lake の `\_SB.GPCL` の group の package は 7 要素で、`intel-gpio.c` の GROUP_FIELDS 9・GROUP_FIRST_NUMBER 8（ADL の形）と合わない。TGL で GpioInt の機器の pad が見つかるようにする。
- BUG-247（bar の 2 回 tap）の原因が sampling なら、ここで直る。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | i2c-hid の Extended Interrupt の経路と TGL の GPIO の group、5320 の実機で確認 | test-wait（2026-10-08 P1） | — |
| p002 | 1 本指のタップのクリックの遅れ（2026-10-08 ユーザーの UAT「タップ判定が150msくらいかかってる気がします。これは調整可能なんですか？」）: 今は指を離した時に左を押し、離すのは TAP_DRAG_MS（300 ms）の後（`touchpad.c` touch_end）。離した時に押す・離すを続けて送り、TAP_DRAG_MS の内の次の touch で押し直してドラッグにする。ダブルクリック・タップのドラッグは保つ。host 試験 | test-wait（q863、P2: 実装・host 試験済み、実機の確認を Q1 へ依頼） | — |
| [p003](phase003/phase.md) | 規約の全文の見直し（WS183 の C の変更） | cleared（2026-10-09 Q1、P4 Sonnet 5.5 low、fbd1da9e7） | p001・p002 |
