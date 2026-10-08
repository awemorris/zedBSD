<!-- awesome-plan project=zedbsd record=ws182 -->

# WS182: 電源ボタンのメニュー（Log Out・Shut Down などを選ぶ）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p001 を cleared（D1 は 2026-10-08 ユーザー「現状ではオーケーです」）。2026-10-08 q910 P2 の照合: p001（設計、D1 はユーザーの確認）は Q1 の判定、p002 は T1-377 QEMU PASS で 5320・5330 の短押し・長押しの実機待ち）
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O1
Parent: [Master](../master.md)
Queue: q861 / q861-i01（P1）
Target: **ベータ2 の最後**（2026-10-07 ユーザー）
Resume point: p001 の設計を書いた（D1: greeter・lock でメニューを出さない → **2026-10-08 ユーザー「現状ではオーケーです」で確定**）。p002 を実装し build warning 0・host 試験 PASS、QEMU は T1 の試験待ち、実機は人の手（2026-10-08 P1）。
<!-- awesome-plan-current:end -->

## 目標（2026-10-07 ユーザー）

「電源ボタンのハンドリングは、あとで実装でいいです。ログオフ、電源オフ、などのメニューを表示できるようにしたいです。独立WSにして、ベータ2の最後に実装しましょう。」

- 電源ボタンを押すと、Keiland が Log Out・Shut Down など（Restart・Sleep を含めるかは設計で決めてユーザーに確かめる）のメニューを出す。
- 既知の事実（2026-10-07 Q1、5320）: 押下は `\_SB.PBTN` の Notify で届き、compositor の session.log に `KWL EVENT power button` が出る（1 回の押下で 2 行、押下と解放の数え方を確かめる）。App Home の Power Off の dialog（ws099-p037）が既にある。
- WS052 p012（電源ボタンの短押しで sleep、N5）との関係を設計で整理する。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: 電源ボタンの事象 → メニュー（App Home の Power Off の dialog の再利用）、greeter・lock の時、WS052 p012 との関係、2 行の事象（押下と解放の 2 つの Notify） | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: D1 は 2026-10-08 ユーザー「現状ではオーケーです」。旧: 設計済み（D1 はユーザーの確認待ち）） | — |
| [p002](phase002/phase.md) | 実装と QEMU・実機（5320・5330）の確認 | uncleared（2026-10-08 T1-377 QEMU PASS。残り: 実機の gap_ms、D1 の確認） | p001 |

## 決定 2026-10-08（ユーザー）

D1（greeter・lock では電源ボタンのメニューを出さない）:「現状ではオーケーです。」同じ返事の追加の要望は別の WS へ: lock の画面の大きな時計 → [WS187](../ws187/ws.md)、PIN・Password・Hardware Key の選択 → [ws172-p007](../ws172/phase007/phase.md)。
