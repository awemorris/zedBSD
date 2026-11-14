<!-- awesome-plan project=zedbsd record=ws347-p010 -->

# ws347-p010: Amaterrace GPU の RTL の全ての register に reset を入れる（memory と複数の clock の module を除く）

Status: in-progress（2026-11-14 Q1 起票。P2 q2080・P4 q2081・P5 q2082）
Parent: [WS347](../ws.md)

## 目的

2026-11-14 ユーザーの決定（要点）: RTL が reset で register を初期化しないのは誤りなので、全ての module で直す。memory（RAM の配列）は初期化しない。clock が複数ある module はユーザーが直す。この件に限り、Amaterrace の RTL を Claude（Q1 と担当）が書いてよい（ユーザーが Q1 の試し書き `atg_tlb_l1.v` の差分を見て、残りも Claude が入れるよう指示した）。

きっかけ: 台帳 B36。CTRL.RESET の後に STATE を配り直す時、書かれていない値が X のまま回路へ流れ、上位の testbench が止まった。X が流れる所を無くす。

## 範囲（担当ごと）

| Queue | 担当 | directory |
| --- | --- | --- |
| q2080 | P2 | `rtl/gpu/cp`・`rtl/gpu/mc`・`rtl/gpu/mmu`・`rtl/gpu/dc`・`rtl/gpu/fmt`・`rtl/gpu/blt`・`rtl/gpu/sa`・`rtl/gpu/top`・`rtl/library/*`・`rtl/bsp/*`・`boards/kc705`・`boards/zcu106` |
| q2081 | P4 | `rtl/gpu/su`・`rtl/gpu/vf`・`rtl/gpu/tu` |
| q2082 | P5 | `rtl/gpu/rx`・`rtl/gpu/rop` |

**触らない file（ユーザーが直す。複数の clock）**: `boards/kc705/lib/atg_kc705_mig.v`、`rtl/bsp/generic/am_jtag_regs.v`、`rtl/bsp/pcie/atg_pcie_msi.v`、`rtl/gpu/dc/atg_dc.v`、`rtl/gpu/dc/atg_dc_fifo.v`、`rtl/gpu/mmu/atg_tlb_l2.v`、`rtl/gpu/rop/atg_rop_blend.v`、`rtl/gpu/rop/atg_rop_wpix.v`、`rtl/library/bus/am_afifo.v`、`rtl/library/bus/am_afifo_bram.v`。

`rtl/gpu/mmu/atg_tlb_l1.v` は Q1 が試しに直した（差分は下の「手本」）。P2 の branch には入っている。

## 規則

1. **対象**: clock の edge で動く `always` の中で `<=` で書く全ての register。reset の分岐が無い block には足し、ある block では漏れている register を足す。
2. **reset の信号**: その module で同じ clock の他の block が使っている reset をそのまま使う（`rst_n`・`rst_sync`・`rst_high`・`fence_rst_high`・`rst_l1_int` など、fanout のために複製した net を含む）。`ifdef SYNTH_KINTEX7` で reset の net が分かれている時は、**両方の枝**に入れる。新しい reset の複製（keep の net・LUT1）は作らない。
3. **reset の値**: 0。状態機械は今ある idle の定数。他の枝・同じ register の別の reset に値があればそれに揃える。GHCI の register の電源を入れた時の値が `docs/ghci.md` にあればその値。
4. **CTRL.RESET で残す register**: `docs/ghci.md` が CTRL.RESET で残すと書く物（IRQ_ENABLE・WDT_CYCLES・CP_HEAP_BASE/LIMIT・RING_BASE・RING_SIZE・SCRATCH など、ghci.md の「Preserved Across Reset」）に、core の reset を足してはいけない。電源を入れた時の reset がその module に無ければ、足さずに Q1 へ報告する。
5. **memory は初期化しない**（ユーザーの決定）: 変数の番地で書き、番地で読む配列で、深さが 8 を超える物（RAM・LUTRAM・BRAM になる物、cache の tag・data、register file、FIFO の data）は reset しない。それ以外の配列（定数の添字・for 文で全部を書く・段をずらすパイプライン・深さ 8 以下）は register として for 文で reset する。reset しなかった配列は報告に全部書く（file:line、名前、深さ、理由）。
6. **reset の port が無い module**（計算だけのパイプラインなど）: clk の次に `input wire rst_n`（同期・active low、docs/style.md）を足し、その module を使う全ての所で、使う側の同じ clock の reset を繋ぐ（極性は使う側に合わせて変換）。使う所が他の担当の directory にあっても、port を足した担当が繋ぐ（merge の衝突は Q1 が解く）。library（`rtl/library/`）の module は P2 だけが変える。
7. **振る舞いを変えない**: reset の分岐を足す以外の変更（パイプラインの段・fanout の複製・keep の属性・ifdef の構造・名前）はしない。lint のために要る最小の変更だけ。疑わしい所（reset を足すと意味が変わりそうな所、例えば reset の中で意図的に保っている値）は足さずに報告する。
8. **ゲートした clock**（`clk & en`）・`negedge` の block を見つけたら、触らずに報告する。

## 手本（Q1、`rtl/gpu/mmu/atg_tlb_l1.v`）

書き込み許可で書く tag・PFN・flags に、同じ block の他の register が使う `rst_l1_int` で 0 を入れた。Q1 が P2 の branch（`p2-reset`）に最初の commit として入れた。

## 確かめ（担当）

- 変えた module ごとに Verilator の lint（`tools/lint.sh` と同じ option で `--top-module <module>`）が warning 0。最後に `tools/lint.sh` の全体を 1 回（warning 0）。
- 変えた module の単体の testbench（`sim/tb/<module>_tb.v` があるもの、`sim/run.sh <name>`、並列 4 まで、各 `timeout`）が PASS。**上位の物（`atg_top*`・`atg_ghci_top`・`atg_kc705_*`・`atg_su` の全体・`atg_tu`・`atg_rx_pipeline` など 10 分を超える物）は流さず、名前を Q1 に渡す**（Q1 がまとめて流す）。
- Vivado（合成・`tools/asic-lint.sh`）は流さない（host で同時に 1 つ。Q1 が agy かユーザーと調整する）。
- 自分で数え直した「直した file・register の数」と「reset しなかった配列」の表を報告に付ける。Q1 の解析の一覧（`ifdef` の reset を読み落とすので多めに出る）は手がかりにだけ使う: `/tmp/claude-1000/-home-awe-zedBSD-claude1--claude-worktrees-bridge-cse-01JKZGr9VeEnz96f7A6qkRro/791268f1-1d48-50fa-8542-d1de6d65cf67/scratchpad/rst/reset-list.tsv`。

## 作業の場所

- amaterrace-gpu の worktree `/home/awe/amaterrace-worktrees/<担当>-reset`（branch `<担当>-reset`、基点 amaterrace-gpu の main 41b9ed95）。amaterrace-gpu の main の tree・他の worktree・`plan/`・`docs/`・`spec/` は変えない。
- commit は `git commit -m WIP`（amaterrace-gpu の AGENTS.md）。push しない。merge は Q1。
- 記録は Q1 が書く。担当は結果を Q1 への報告（SendMessage と最後の報告）で返す。

## 結果

（担当が書く）
