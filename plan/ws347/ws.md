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

- 2026-11-13 ユーザーから、GHCI の仕様の問いの決定を受けた: B33 は session の fault（今の RTL どおり、SESSION ≠ 0 なら watchdog・AXI_ERROR も session だけを止める）、B34 は後の fault の記録（FAULT_INFO・FAULT_ADDR）だけを捨て、SESSION_ACTIVE・FAULTED・IRQ_FAULT は行う（依頼 08 の X5 (a) の仮の形のまま）。
- 2026-11-13 ユーザーから、次の趣旨の説明を受けた: discrete の GPU でも scan-out は host memory を読める。GOP の framebuffer が host memory か板の VRAM かは、host（または板の UEFI）の firmware による。→ host が GPU の local memory に書くのは、GOP が板の VRAM にある時の GOP の framebuffer だけ。仕様の §13.2・§11.3 の比べの表を直す（agy の依頼 08 の後の訂正に入れる）。
- 2026-11-13 ユーザーから、次の趣旨の訂正を受けた: host が GPU の local memory に書いてよいのは GOP の VRAM だけ（B35 の前提の誤解の訂正）。GOP を残して使う事はまず無いので、blitter の無い discrete の GPU で CPU が local の窓を通して frame に書く予備の道（`display.c` の `copy_by_processor`）は削除する。
- 2026-11-13 ユーザーから、依頼 08（X1〜X9）は全部実装し、B35（host の BAR の local の窓と L2 の一貫性）は driver が command で守る、という趣旨の決定を受けた。
- 2026-11-13 ユーザーから、次の趣旨の決定を受けた: (1) 割り込みの有無で polling にする事と、DMA が coherent でない時に uncached で使う事は driver の基本の機能なので build の option にしない（`CONFIG_GHCI_POLL`・`CONFIG_GHCI_HOST_UNCACHED` を外し、検出だけで決める。mk の案は取り下げ）。(2) clipper の無い GPU（CLIP_POLY 0）は何も言わずに欠けてよい（minimal は実用ではない。software の clip は作らない）。両方 [p008](phase008/phase.md) で P4 が直す。
- 2026-11-14 ユーザーから、次の趣旨の決定を受けた: RTL が reset で register を初期化しないのは誤りなので、全ての module で直す。memory（RAM の配列）は初期化しない。reset の種類は、DSP48E1・BRAM・SRL を包む am_* の wrapper だけ同期 reset、それ以外の全ての register は非同期 reset（今の docs/style.md の「同期 reset」を改める）。clock が複数ある module（atg_dc・atg_dc_fifo・am_afifo・am_afifo_bram・atg_pcie_msi・am_jtag_regs・atg_kc705_mig・atg_rop_blend・atg_rop_wpix、別名の clock だけの atg_tlb_l2）はユーザーが直し方を考える。Claude の担当が同期 reset で始めた作業（p010）は破棄し、agy に方針を伝えて 1 file（atg_tlb_l1.v）を頼み、その差分をユーザーに見せる。
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

- 2026-11-15 02:0x Q1: 11 回目（blt の 5 file、amaterrace-gpu 4fa36117）をレビューして OK とした（消えた論理の行は無く、reset 以外の変更は論理が同じ）。atg_blt の単体の testbench の case 3 の FAIL（byte 0x40f4〜）は reset の作業の前の 41b9ed95 でも同じで、前からある不具合（reset とは別に調べる）。12 回目（atg_blt_cfg・favg・port・rsv・rsv_px、[q-rst12.txt](agy-reset/q-rst12.txt)）を送った。reset は 185 file のうち 50 file まで済み。

- 2026-11-14 17:5x Q1（host のコールドブートの前）: agy の reset は 10 回目まで済み（amaterrace-gpu main 38593b0e。1 回目 b7693ca6 から 2 cbd4048c・3 9fec3f9a・4 8eb3947a・5 825b2929・6 cf306b07・7 f6c39eb1・8 1cbdc20a・9 f7db141f・10 38593b0e）。依頼の文の生成と一覧は [agy-reset/](agy-reset/)（rst-round.sh N が rst-files.txt の 5 file と方針 1〜10、q-extra.txt があれば前の回への指摘を足す。log と依頼の文は AGY_DIR に置く）。次は 11 回目（rst-files.txt の 46〜50 行目）。ユーザーの判断待ち: atg_dc_regs の単体の testbench の 3 つの FAIL（9 回目の前から。hotplug の初めの検出・ENABLE 中の TIMING の書き込みの棄却・RESET の flip の取り消しの 1 cycle、案は agy-reset/a-rst10.md）。10 回目の atg_dc_tmds の qm_xor の書き換え（reset 以外の変更、UNOPTFLAT の解消）は、累積 XOR の連鎖を `^d_b[i:0]` に開いただけで論理は同じなので Q1 が受け入れた（2026-11-14 21:3x、lint の warning 0 の受け入れの条件のため）。2026-11-14 21:5x Q1: 10 回目をレビューして OK とした（9 回目への指摘の 3 件は 1cbdc20a の振る舞いに戻り、atg_dc_tgen・atg_dc_plane2 は普通の段と制御の bit に reset が入り、SRL になる 2 段の列だけ残した。今回の 5 file は消えた論理の行が無く、足したのは reset と字下げだけ、と diff を機械的に比べて確かめた）。atg_fmt_cvt の全 400k cycle（PASS 4858 s）・atg_dc_tgen の full（PASS 80 s）・atg_dc（FAIL は既知の cancelled by RESET の 1 件だけ）を amaterrace の worktree `q1-rst10`（38593b0e、main の tree と同じ config.mk）で流した。11 回目（blt の 5 file、[q-rst11.txt](agy-reset/q-rst11.txt)）を agy に送った（log は Q1 の scratchpad）。pts/2 の agy は Web UI のための remote control の server で、rst-round.sh の busy の判定はこの会話の headless の agy だけを見る形に直した。P1 の p016 は host の書き込みの遅さ（NVMe が 22 MB/s）で止めた。再起動の後に書き込みを測り直して再開する。
- 2026-11-14 Q1: reset の方針（am_* の wrapper は同期、他は非同期）を agy に伝え、1 file（atg_tlb_l1.v）を頼んだ。agy は b7693ca6 で非同期にしたが、reset の源（atg_mmu の LUT1 の反転、atg_cp_ctl の LUT4 の core_rst_n）は glitch・解除の同期が無く非同期 reset に安全でないと返した。ユーザーは atg_tlb_l1.v の差分を OK とし、reset の論理（極性・各 module の反転・core_rst_n）はそのままでよいとした。残りの 185 file（clock が複数の 10 file を除く。atg_tlb_l2 は clk_l2 が clk の別名だけなのでユーザーの了承で最後に足した。一覧は scratchpad の agy/rst-files.txt）を 5 file ずつ agy に頼み、届いた順にユーザーがレビューする（2 回目から。依頼の文は rst-round.sh が作る）。 2・3 回目（cbd4048c・9fec3f9a）はユーザーが OK、この調子で続ける（2026-11-14）。 同日ユーザーの決定: valid などの制御の bit が守る data の register（skid の data、256・512 bit の buffer）は、reset を配ると fanout の規則（docs/style.md、ASIC で妥当）を超える時は reset しない例外とする（FPGA でも実用上問題ない）。制御の register は必ず reset する。それまで依頼 08 の X5〜X9 は送らない。4 件の修正の後の上位の testbench（41b9ed95）: atg_top_shade は DRAW 中の pipe_busy_mask 0、atg_top_fault 11 errors、atg_ghci_top 5 FAIL（X4 の Scenario 4 で FAULTED が立たない）、atg_top_tex 10 FAIL（画素が一色 ffe62828、model は階調）、atg_top_fault 12 FAIL（Scenario 1 で FAULTED が立たない）。reset を入れた後に流し直して解析する。
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
| [p008](phase008/phase.md) | blitter の無い GPU で表示の copy の present を shader で写す（今は CPU が写す）。2026-11-13 の決定で CONFIG_GHCI_POLL・HOST_UNCACHED を外し、CLIP_POLY の shortfall を外す事を足した | cleared（2026-11-13 Q1、P4 q2030。main に merge） | p003 |
| [p010](phase010/phase.md) | RTL の全ての register に reset を入れる（Claude の担当で同期 reset） | canceled（2026-11-14 ユーザーの決定で方針が変わり破棄。非同期 reset で agy に頼み直す） | — |
| [p009](phase009/phase.md) | host が local memory に GOP の VRAM 以外を書かない形に: `copy_by_processor` の削除と、driver の local の窓の使い方の点検 | cleared（2026-11-13 Q1、P4 q2043。main に merge） | p008 |
