<!-- awesome-plan project=zedbsd record=ws033 -->

# WS033: networking サービスと有線インタフェースの管理

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p001 は uncleared（q598-i01 で中断、表を phase.md に合わせた）。残り: p001 の QEMU の USB の LAN の後挿し、p002 は実機、p003 は規約（ベータ3））
Primary Milestone: MG005
Related Milestones: MG003
Objectives: O1, O2, O3
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1、2026-10-02 user: ネットワークが最上位）
Queue: なし
Resume point: [p001](phase001/phase.md)（USB の LAN の後挿し・抜去・carrier の変化を QEMU で通す、planned）→ [p003](phase003/phase.md)（全文規約）→ [p002](phase002/phase.md)（実機、ユーザーの時期）
<!-- awesome-plan-current:end -->

## 目標

有線インタフェースを networkd が常駐して管理し（ケーブルの抜き差しに追従して DHCP・static を構成する）、
起動時の `networking` サービスが「どれか一つのインタフェースが到達可能なアドレスを持つ」まで待てるようにする。

## 結果

この WS は計画の記録（ws.md）を持たないまま実行された。経緯と決定は [notes.md](notes.md) にある。

- 判断と実行を分けた `managed-lan`（IDLE・PENDING・CONFIGURED・UNCONFIGURED の状態）。装置を開かず、次にすべきことを答えるだけなので host で試せる。
- carrier の変化は既存の route socket（`RTM_IFINFO`）で受ける。新しい kernel interface は要らなかった。
- `net lan enable`・`net lan disable`・`net startup`、`/etc/service.d/networking`（oneshot）。待つのは `net startup` で、`rc.conf` の `networking.wait` を読む。
- DHCP が取れなければ MAC から 169.254.x.y を導く（有効なアドレスとは扱わない）。
- 試験: `make managed-lan-host-test`（31 項目、[tests/managed-lan-host-test.c](tests/managed-lan-host-test.c)）。

## 残り

- QEMU の試験機に有線 NIC が無く、ケーブルの抜き差しは実機で試せていない（判断の側は host で試した）。
- dp8390 はリンクを検出しないので、抜線を検出できない。
- 旧 `networking-target.sh` は console log を読む harness（`run-target-console.py`）に頼っていたため、2026-09-24 の plan 整理で削除した。

## ベータ1（fg019）の到達目標と受け入れ（2026-10-02 計画）

対象の Latitude 5330・5320 は RJ45 を持たず、有線は USB の LAN（RTL8156 の CDC NCM、ECM）だけ。ベータ1 では次を測れる形で確かめる。

| # | 条件 | 証拠 |
| --- | --- | --- |
| L1 | 起動の後に USB の LAN を挿すと、`net lan enable` の管理で DHCP の address を得て `fetch` が通る（device の再列挙） | QEMU の QMP `device_add`/`device_del`（usb-net）と guest の SSH |
| L2 | carrier の down/up（ケーブルの抜き差しに相当）で address を外し／取り直す。carrier を出さない装置ではそう記録する | QEMU（usb-net の link の切替えが guest の carrier に届くかを先に調べる）、実機は L4 |
| L3 | `networking.wait` を true にした起動が、address を得たら抜け、得られなければ設定の時間で抜けて起動が続く | QEMU の SSH と `service status networking` |
| L4 | 実機: 5330/5320 で RTL8156 の抜き差しに追従する | ユーザーの報告（時期はユーザーに聞く） |

## Phase 一覧

この WS は ws.md を持たずに実行された（上の「結果」）。2026-10-02 に残りを Phase にした。

| Phase | 目的 | Status | 依存 | 目安 |
| --- | --- | --- | --- | --- |
| [p001](phase001/phase.md) | USB の LAN の後挿し・抜去・carrier の変化と `networking.wait` を QEMU で通す（L1〜L3）。見つかった不具合を直す | in-progress（q912 P1 2026-10-08: L1 の fetch と L3 の試験を作り T1 へ。L1・抜去は T1-145・T1-221 で PASS、L2 は QEMU で carrier が動かず p002 へ） | なし（ws005-p019 と `userland/base/networkd/`・`net/` が重なるので同時に走らせない） | 2〜3h |
| [p003](phase003/phase.md) | WS033 で書いた source（`managed-lan.c`、`net lan`・`net startup`、init の setting の表）の全文規約の確認 | planned | p001 の修正の後（独立に先に走らせてもよいが、p001 の修正を含めて 1 回にする） | 2h |
| [p002](phase002/phase.md) | 実機の確認 L4（ユーザーと一緒に、ws005-p023 と同じ日にまとめる） | planning | p001、ユーザーの時期 | 1h（立会い） |

## 2026-10-04 UAT の結果（Q1）

起動の時から挿した USB LAN（RTL8156、ue0）は up して DHCP。**起動の後に挿すと up しない**（[BUG-168](../bugs/BUG-168.md)）。抜くと Ethernet のメニューに wlan0 が出る（[BUG-169](../bugs/BUG-169.md)）。WiFi の address への SSH ができない（[BUG-174](../bugs/BUG-174.md)）。

## 2026-10-06 UAT のフィードバック

- BUG-222 ue0 の SCP が約 950 KB/s（link の速度を Settings に出し 10 Mbps の mode か確かめる。ue0 の chip はユーザーに確かめる）
- BUG-212 有線の link down で WiFi が切れ再接続できない（WS005 と）
- BUG-213 link down の後の Settings の「No address」と IP の表示の矛盾（WS089 と）
