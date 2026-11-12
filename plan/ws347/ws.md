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

- 2026-11-13 ユーザーから、次の趣旨の決定を受けた: (1) 割り込みの有無で polling にする事と、DMA が coherent でない時に uncached で使う事は driver の基本の機能なので build の option にしない（`CONFIG_GHCI_POLL`・`CONFIG_GHCI_HOST_UNCACHED` を外し、検出だけで決める。mk の案は取り下げ）。(2) clipper の無い GPU（CLIP_POLY 0）は何も言わずに欠けてよい（minimal は実用ではない。software の clip は作らない）。両方 [p008](phase008/phase.md) で P4 が直す。
- 2026-11-13 ユーザーから、次の趣旨の決定を受けた: blitter（CAP_BLT）が無い時は、Vulkan の転送だけでなく表示の写しも shader で行う（[p008](phase008/phase.md)）。
- 2026-11-11 ユーザーから、次の趣旨の指示を受けた:
  - Amaterrace GPU の試験とデバッグを VF2 で行う WS を立てる。VF2 には PCIe で KC705 がつながっている。
  - Amaterrace GPU は minimal の構成で 200 MHz でタイミングが閉じ、bitstream が置かれている。USB 経由で書き込んで試験する。
  - 書き込みには Vivado を使う（以前の「担当は KC705 に書き込まない」は、この WS では置き換わる）。
  - GHCI の仕様は最近少し直った。KC705 に LUT が収まるよう多くの機能を option にして削り、CAPS（capability の bit）が増えた。
    例: Blitter が option になって外れ、CAP が無い時は shader で blit する。
  - 順: (1) bitstream の書き込みができることを確かめる → (2) GHCI の driver を最新の仕様に追従 → (3) FPGA の動作確認。
- 2026-11-11 ユーザーから、amaterrace-gpu は他のセッションでの開発が終わったので Q1 に全権を委ね書き込みもよい、という趣旨の指示を受けた。→ 下の役割分担の決定で置き換え（RTL は Gemini が実装）。
- 2026-11-11 ユーザーから、仕様（ghci.md）を正とし、仕様に瑕疵が無さそうなら RTL を直す、driver は仕様に合わせて直す、という趣旨の指示を受けた
  （食い違いの一覧は [spec-diff.md](spec-diff.md)）。→ 同日の下の決定で置き換え。
- 2026-11-11 12:3x ユーザーの指示で全作業を止めた。その後ユーザーから役割分担の決定を受けた（要点は AGENTS.md §7）: Claude は仕様と RTL を
  読んで監査し driver を書く、Gemini は Claude の指揮の下で RTL を実装する（パイプライン化とタイミングの収束）。ghci.md が正、食い違いは
  A（RTL の誤り → Gemini への依頼）と B（仕様の問い → ユーザー）に分け、台帳は [ghci-rtl-questions.md](ghci-rtl-questions.md)。

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

- 2026-11-11 12:37 Q1: ユーザーの指示で全作業を止めた（P1・P2・Vivado の build）。再開はユーザーの指示で。
  - P1 の worktree `/home/awe/amaterrace-worktrees/p1-ws347rtl`（branch ws347-rtl）に RTL の未 commit の変更 3 file（atg_cp.v・atg_cp_regs.v・atg_top.v）。RTL は Gemini の担当なので、扱いはユーザーの判断。
  - P2 の worktree `/home/awe/zedBSD-worktrees/p2-ws347`（branch agent/p2-ws347）に driver の未 commit の変更（中身は未確認）。
  - bitstream の build（`build/ws347/out/`、bace7d66 の minimal）は placement の途中で止めた。bitstream は無い。
  - 仕様は amaterrace-gpu main 6949b248（VA_BITS_ENC = VA − 30、ユーザーの承認）。
  - 疑問は [ghci-rtl-questions.md](ghci-rtl-questions.md)。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | KC705 への bitstream の書き込み（Vivado・JTAG）と VF2 からの PCIe の見え方 | in-progress・止め（build を placement の途中で止めた） | — |
| [p002](phase002/phase.md) | 仕様の差の読み（WS290 の基準 → 最新の ghci.md）と driver の追従の設計 | cleared（2026-11-11、[spec-diff.md](spec-diff.md)） | — |
| [p003](phase003/phase.md) | driver の追従の実装と host 試験（VA_BITS_ENC = VA − 30、新しい CAP の bit と 0 の時の扱い、試験の道具の直し） | cleared（2026-11-13 Q1、P4 q2025。main に merge） | p002 |
| [p004](phase004/phase.md) | VF2＋KC705 での段ごとの動作確認とデバッグ（attach → ring・fence → 描画 → 表示） | planned | p001・p003・p007 |
| [p006](phase006/phase.md) | Amaterrace GPU の RTL の CAPS を ghci.md に合わせる | canceled（2026-11-11、RTL は Gemini が実装する役割分担） | p002 |
| [p007](phase007/phase.md) | 直した RTL で bitstream を作り直し、CAPS を実機で照合 | canceled（2026-11-11、RTL はユーザーが直す） | — |
| [p005](phase005/phase.md) | 規約の全文の見直し | planned | p003・p004・p006 |
| [p008](phase008/phase.md) | blitter の無い GPU で表示の copy の present を shader で写す（今は CPU が PCIe 越しに写す） | in-progress（2026-11-13 Q1、q2030-i01 P4） | p003 |
