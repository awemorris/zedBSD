<!-- awesome-plan project=zedbsd record=ws347 -->

# WS347: VF2＋KC705 で Amaterrace GPU を試験・デバッグする（GHCI の driver を最新の仕様に追従）

Status: incomplete
Primary Milestone: MG006
Related Milestones: MG001
Objectives: O2, O4
Parent: [Master](../master.md)
Owner: Q1
Queue: [q2009](../queue.md)（p001、Q1 が直に）

## 目標

VisionFive 2（VF2、riscv64）に PCIe でつないだ KC705（Xilinx Kintex-7 の FPGA の板）に、Amaterrace GPU（am-ws001 系、
`~/amaterrace-gpu`）の minimal の構成の bitstream を Vivado で JTAG（USB）から書き、zedBSD の GHCI の driver
（`src/drivers/gpu/ghci/`）で実機の GPU を動かす。driver は最新の GHCI 1.0（amaterrace-gpu の `docs/ghci.md`）に追従させる。

## ユーザーの決定（要点）

- 2026-11-11 ユーザーから、次の趣旨の指示を受けた:
  - Amaterrace GPU の試験とデバッグを VF2 で行う WS を立てる。VF2 には PCIe で KC705 がつながっている。
  - Amaterrace GPU は minimal の構成で 200 MHz でタイミングが閉じ、bitstream が置かれている。USB 経由で書き込んで試験する。
  - 書き込みには Vivado を使う（以前の「担当は KC705 に書き込まない」は、この WS では置き換わる）。
  - GHCI の仕様は最近少し直った。KC705 に LUT が収まるよう多くの機能を option にして削り、CAPS（capability の bit）が増えた。
    例: Blitter が option になって外れ、CAP が無い時は shader で blit する。
  - 順: (1) bitstream の書き込みができることを確かめる → (2) GHCI の driver を最新の仕様に追従 → (3) FPGA の動作確認。
- amaterrace-gpu は読み取り専用のまま（bitstream と ghci.md を読むだけ。git は log・show・diff だけ）。

## WS290 との関係（Q1 の判断）

- [WS290](../ws290/ws.md)（GHCI 1.0 の driver の書き直し、incomplete・保留）は ghci.md 89529de〜cb937e6（2026-10-28）までを実装済み。
  この WS はその code の上で、それ以後の仕様の差（ghci.md a7f5ca47、2026-11-06 まで）への追従と実機の試験を行う。
- WS290 の p014（実機の確かめ）はこの WS の p004 に引き継ぐ（WS290 の側は WS290 を再開する時に canceled にする）。

## 実機

- KC705: host（centris）の USB の JTAG（Digilent、FT2232、target `localhost:3121/xilinx_tcf/Digilent/210203368704A`、device `xc7k325t_0`）。
  2026-11-11 11:5x に Vivado 2026.1 の hw_manager で見え、未設定（DONE=0）。
- VF2: 10.0.30.6（DHCP、2026-11-11）、SwitchBot の plug 3 `AC276E432AE6`、serial は host の /dev/ttyUSB0。
- Vivado: `~/.local/bin/vivado`（amaterrace-gpu の tools/vivado-cmd.sh の wrapper: 同時 1 process・2 thread・終わりに sync）。
  作業の directory は amaterrace-gpu の外（その tree に journal・log を書かない）。

## Resume point

- 2026-11-11 Q1: 立てた。p001 の JTAG の確認まで済み。書き込む bitstream の所在と、KC705 と VF2 の電源の関係をユーザーに確かめ中。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | KC705 への bitstream の書き込み（Vivado・JTAG）と VF2 からの PCIe の見え方 | in-progress | — |
| [p002](phase002/phase.md) | 仕様の差の読み（WS290 の基準 → 最新の ghci.md）と driver の追従の設計 | planned | — |
| [p003](phase003/phase.md) | driver の追従の実装と host 試験（CAPS、Blitter 無しの shader blit ほか） | planned | p002 |
| [p004](phase004/phase.md) | VF2＋KC705 での段ごとの動作確認とデバッグ（attach → ring・fence → 描画 → 表示） | planned | p001・p003 |
| [p005](phase005/phase.md) | 規約の全文の見直し | planned | p003・p004 |
