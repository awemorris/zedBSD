<!-- awesome-plan project=zedbsd record=ws005 -->

# WS005: networking and WLAN

<!-- awesome-plan-current:start -->
Status: completed（2026-10-08 ユーザーの判断「WS005は記録のミスだと思います、とっくに完了してます。」）
Primary Milestone: MG005
Related Milestones: MG006
Objectives: O1, O2, O3
Parent: [Master](../master.md)
Queue: なし
<!-- awesome-plan-current:end -->

## 結果

有線（USB の RTL8156 NCM）と WLAN（RTL8822BU・AX211）を networkd が管理し、`net wifi`・`/sbin/wifi`・`net lan` の操作、network group の利用者への WiFi の制御の許可（2026-10-02）、起動時と login の自動再接続、system bar の network の menu と Settings の Wi-Fi の頁（Connecting… の表示、鍵の欄、Disconnect）まで実装した。p001〜p012 は 2026-09 に実機で受け入れ、ベータ1 の p018〜p032 の大半は QEMU（T1）で cleared、実機の確認は 5330・5320 の日常の使用でユーザーが完了と判断した。

## 閉じ方（2026-10-08）

- ユーザーの判断で WS を completed にした。記録上 planning・in-progress・uncleared のまま残っていた Phase（p013〜p015・p017 の取消し提案、p019、p021〜p023、p028、p031、p032）は、実装が main に入っており、ユーザーの使用で確かめられたものとして閉じた（個別の証拠の追記はしない）。
- 全文規約の見直し（p022）は、ベータ3 の規約の整形（2026-10-08 ユーザー「コーディング規約による整形はベータ3でやります。」）に移す。
- 残る WiFi の UI の Bug（BUG-183〜BUG-189 など）は [Bug Board](../known-bugs.md) で追う。

## 残した物

- `tests/`: [WS129 p011](../ws129/phase011/phase.md) の release の試験の一覧から `bug149-check.sh` などが参照されるので残す。次に変更へ追従させる時に試験の整理の基準（AGENTS.md）を当てる。
- `network-improvements-2026-09-11.md`（設計の記録）。
- Phase の directory は削除した（git の履歴にある）。
