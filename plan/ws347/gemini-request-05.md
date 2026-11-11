<!-- awesome-plan project=zedbsd record=ws347-gemini-request-05 -->

# Gemini への作業依頼 05（まとめ版）: GHCI 1.0 の host の register の残り全部

- 依頼元: zedBSD の Q1（Claude、監査の役）。2026-11-12。対象: `~/amaterrace-gpu` main eeed36ed。
- **この文書は依頼 01〜04 を全部置き換える。読むのはこの文書だけでよい。** 作業の番号は S1〜S11 に振り直した。
- 監査の記録（zedBSD 側）: `plan/ws347/audit-eeed36ed.md`。

## 0. 守ってほしい事

1. `docs/ghci.md` が正。**仕様は書き換えない。** 仕様が黙っている所は推測で埋めず、報告に書く（下の「今回触らない物」は仕様の問いとしてユーザーに上げてある）。
2. 値は**実際に instance した回路と board・top の parameter**から作る。固定値・macro の既定で辻褄を合わせない。macro が無い時の既定は「無い」（0）。macro の名前は Makefile が `include/atg_config.vh` に実際に出す名前と一致させる。
3. Verilog-2005、`rtl/library/arith/` の外に算術の演算子を書かない、Vivado は host で 1 process・2 thread、実行の後に `sync`、commit message は `WIP` ちょうど（前回は守れていた）。
4. 下の「済んだ物」を壊さない。
5. tb は**仕様の文**を期待値にする。RTL の今の並びに合わせた期待値で PASS させない（前回、STATUS と FAULT_INFO の誤った位置を tb が期待値にしていた）。

## 済んだ物（壊さない）

- Slim の page 0 だけ（SLIM_MMIO の撤去）。page 0 の各 offset を `HCR_IDX_*` の index ただ 1 つに送る形、読みの混ざりの撤去。
- RING_SIZE を byte の数で受ける（4 KiB〜16 MiB の 2 の冪、他は前の値を保って無視）、WPTR・RPTR は 22 bit。走行中の RING_*・CP_HEAP の書き込みは LOCKED_REG（kind 7）。
- FAULT_INFO の W1C、64 bit の TIMESTAMP、RESIDENT_WARPS、NATIVE_ISA_ID。
- EXTCAP の見出しが 0x100〜0x13F に収まる事、MEM（0x1E0〜）・VEND の SUBSYSTEM・PERF_SRC の答え、見出しは RO。
- FMT_OPTIMAL・LINEAR の bit の並び（§4.1）と、FILTER_LINEAR を bilinear が有る時だけにした事。
- 前処理（`` `elsif ``）の直し、汎用 module の GCLK_HZ の既定 0（KC705 200 MHz、ZCU106 400 MHz を board が渡す）、config.mk.template の 3 つの key。
- minimal（KC705）の 3 register: DEVICE_CAPS 0x40010480、EXT_CAP_FEATURE 0x00110000、EXT_CAP_FMT0 0x00001200。
- AXI の ID（host の access で ID 1）と tb の AXI の model の ID の queue。

## 1. register の並び（最優先、driver が直に読む）

### S1. STATUS（0x010）を §3.3 の並びに（2026-11-12 改訂: 初版の「他の bit は 0」は Q1 の誤り、CP_STATE は残す）

- 仕様（ghci.md:340-358）: [0] RUNNING、[1] HALTED、[2] FAULTED、[8] MC_IDLE、[11] RING_EMPTY、[12] MEM_READY、[13] AXI_STUCK、[14] QUIESCE、**[19:16] CP_STATE**、[31:20] 予約（0）。[7:3]・[10:9]・[15] は 0。
- 今（`atg_cp_ctl.v:769`）: [0] CP の busy、[7:1] 各 unit の busy、[9] faulted、[10] halted、[14] halt_int。
- 直し方: [2:0] を RUNNING・HALTED・FAULTED に、[7:3] を 0 に、[8]・[11]〜[14] を仕様の意味に。RUNNING・HALTED・QUIESCE の意味は §3.3 の CTRL と状態（M10 の IDLE・RUNNING・HALTING・HALTED・FAULTED）どおり。
- **CP_STATE の値も仕様の列挙に**: 仕様は 0 IDLE（doorbell か RUN を待つ）、1 FETCH、2 DECODE、3 EXEC、4 WAIT_FENCE、5 WAIT_MEM、6 BLIT、7 FAULT。今の RTL（`atg_cp_ctl.v:186-188`、`ghci_regs.vh:332-339` の `GHCI_ST_*`）は lifecycle の値（1 RUNNING、2 HALTING、3 HALTED、4 FAULTED、5 RESETTING、6 BLIT、7 WAIT_FENCE）で、仕様と違う。M10 は「RUNNING でも doorbell を待つ間は CP_STATE == 0」と書くので、今の RTL（走行中は 1）だと driver の IDLE の判定が狂う。CP の実際の段（fetch・decode・exec・fence 待ち・memory 待ち・blit・fault）から仕様の値を出す。`GHCI_ST_*` の定義も仕様の列挙に合わせる。

### S2. FAULT_INFO（0x018）を §3.5 の並びに

- 仕様（ghci.md:417〜）: [3:0] KIND、[7:4] UNIT、[15:8] SESSION、[19:16] STAGE、[31:20] SYNDROME。KIND・UNIT・STAGE の値は §3.5 と `include/ghci_regs.vh` の `GHCI_FAULT_KIND_*`・`GHCI_FAULT_UNIT_*`・`GHCI_STAGE_*`。
- 今（`atg_cp_ctl.v:440-460`）: kind を [15:12]、block を [23:16]、client を [7:0]、tag を [31:24]。
- 直し方: fault を作る**全部の経路**（MC の memory fault、ring の framing・OOB、watchdog、LOCKED_REG、AXI、SU の ISA fault、KICK_ABSENT など）が仕様の並びで書く。FAULT_ADDR（§3.5）の中身も合わせる。

### S3. SESSION_ACTIVE（0x058）

- PROT が有る構成（balanced など）で、開いている session の bitmap を §11.6 どおりに返す。今は PROT=1 でも 1 固定。PROT が無い構成は session 0 だけ。

## 2. 能力の register の中身

### S4. 残りの bit を実装から

- **DEVICE_CAPS**: [7] GSI1 は仕様で **Fixed 1**（今は NSU≠0 で作る）。[25] TEX_FFILT は `CONFIG_TU_ENABLE_FFILT` だけ（今は SU の FP16 を掛けている）。[31] MSAA は MSAA の回路（RX・ROP の多 sample）が実際に在る時。[3] INDIRECT_DRAW・[4] SECONDARY_RING は、seq・fw が `CMD_DRAW_INDIRECT`・`CMD_DISPATCH_INDIRECT`・`CMD_EXECUTE_COMMANDS` を最後まで実行できるかを確かめ、できるなら 1、できないなら 0。どちらにしたか、根拠（file:line）を報告に書く。
- **EXT_CAP_FMT0**: [9] SRGB = `CONFIG_FMT_ENABLE_SRGB`（今 1 固定）。[10] FFILT_FP16 = TEX_FFILT かつ FMT の FP16（`CONFIG_FMT_ENABLE_FP16`。今は SU の FP16）。[11] FFILT_FP32 = TEX_FFILT かつ FMT の FP32（今 0 固定）。[7] ETC2 = ETC の decoder が在る時（今は atg_top が読む `CFG_TU_ENABLE_ETC2` を Makefile が作らない。decoder は FFILT の時に atg_tu_ffilt の中にある。Makefile の名前 `CFG_ENABLE_ETC2` と、実際の decoder の有無に合わせる）。FMT の窓も同じ条件で ETC2 の format を出す。
- **SUBGROUP**: atg_top などが読む `CFG_SU_ENABLE_SUBGROUP` を Makefile は作らない（Makefile は `CFG_ENABLE_SUBGROUP`）。名前を揃える。SU_PACKED・SUBGROUP・LDS は NSU が 0 なら 0。
- **注入の parameter を消す**: `atg_cp_regs.v:758` などの、DEVICE_CAPS・EXT_CAP_* を外から渡す parameter。
- **VEND の SUBSYSTEM の既定**: 汎用 module（`atg_cp_regs.v:304`）の既定は 0。各 board の top が渡す（KC705: 0x10EE・0x0007。ZCU106 は KC705 の値を答えない）。

### S5. FMT の窓の除外（§4.1）

- TEX_FFILT が無い時、FP16・FP32 の format に FILTER_LINEAR を出さない（今は出す）。
- ROP_PACK が無い時、UINT・SINT などの Path 2 の format に COLOR_ATTACHMENT・BLEND を出さない（今は出す）。

## 3. include・死んだ論理・文書

### S6. include

- `include/ghci_regs.vh`: 新しい map と重なる別名を消す（`GHCI_PERF0` = 0x080 は EXT_CAP_FMT0 と重なる、:242）。`GHCI_KICK = GHCI_LEGACY_KICK` を宣言の前に使う所（:268、Vivado の Synth 8-6901）を直す。
- `include/ghci_regs.h`: FENCE_LAST・PIPE_FSM・DBG_* などを ghci.md の名前か `GHCI_LEGACY_*` に。

### S7. 死んだ論理

- ring の queue 1・2・DMA・preempt・sema（0 を入れて unused_ok に入れただけ）、dbg の本体のうち GHCI 1.0 で使わない物、`kick_prot_ok` の入力、`I_PERF_CTRL`・`A_LOCAL_*`、ring の `sz_clamp_c`（:1276）、unused_ok の古い幅（:1889）を消す。
- ctl の comment の KICK・aperture（:10,12,33,51,52,82）。
- RING_SIZE の書き込みは sim の版と合成の版で同じ条件（今は合成の版だけ `wmask[0]` を要る）。

### S8. `spec/gpu/` の文書（RTL の直しの後、最後に）

- `atg_cp_regs.md` から RTL に無い物を消し、RTL と ghci.md に合わせる（PERF_CTRL 0x160、PERF_SRC 0x168..、CPFW_DOORBELL 0x1C0・RESET 0x1C4、WINDOW_OFFSET 0x1F4（RW）。0x1F4 は ghci.md で MEM_CACHE_CFG（RO））。CPFW の範囲の記述。
- STATUS・FAULT_INFO の並び（S1・S2 の後）。
- config.mk.template の VF の comment「4,8,16」を仕様の 2,4,8 に。

## 4. 試験と timing

### S9. tb

`sim/tb/atg_ghci_top_tb.v`:

1. **preset ごとの compile の仕組み**を作る（`sim/run.sh` か Makefile の target で、`make preset-minimal` と `make preset-balanced` の `include/atg_config.vh` をそれぞれ作り、その構成で compile して流す）。手で preset を切り替える前提にしない。
2. **DUT を KC705 の board と同じに**: `boards/kc705/atg_kc705_top.v` そのものか、同じ parameter の atg_top（S_ID_W、S_LEN_W、SU_GRP_W・SU_NREGS・SU_NWARPS、HAS_L2・L2_*、PROT_GRANTS などを board と同じに。minimal の SU_GRP_W は 3）。
3. **期待値は仕様の式で**: 16 進の定数（RESIDENT_WARPS 0x00200004、FMT の 0xEF・0xED、EXTCAP の指す先）を、構成の値と §3・§4.1 の式から計算する形に。RTL の macro の写しにしない。
4. **STATUS・FAULT_INFO**: S1・S2 の**仕様の位置**で照合（R2 の LOCKED_REG は FAULT_INFO[3:0] = 7、FAULTED は STATUS[2]、走行中に doorbell を待つ間は STATUS[19:16] = 0、packet の取り込み中は FETCH 以降の値）。
5. **EXTCAP の全部の窓**: 見出しを辿り、PERF_SRC・CPFW・DISP2・MEM（MEM_CACHE_CFG を含む）・FMT・VEND を照合。DBG の指す先は 0。
6. **ring**: IDLE で CTRL に RESET を書いても ring が動かない事。
7. **TIMESTAMP_HI**: 内部の counter を force せず、LO の桁上がりで HI が進む事を確かめる（短い時間で済むよう、試験の時だけ counter の初期値を parameter で与えるのはよい）。
8. **既存の tb と harness を GHCI 1.0 の map に直して PASS させる**: 消えた名前（`GHCI_CTRL`・`GHCI_RING_BASE`・`GHCI_CAPS`・`GHCI_KICK` など）を使う file。少なくとも `atg_top_tb.v`、`atg_top_fw_tb.v`、`atg_top_prot_tb.vh`、`atg_kc705_pcie_tb.v`、`atg_top_disp_tb.v`、`atg_top_cs_tb.v`、`atg_top_scene.vh`、`atg_kc705_top_tb.v`、`atg_top_prot_tb.v`、`atg_top_mesh_tb.v`、`atg_cp_multi_queue_tb.v`、`atg_cp_irq_tb.v`、`sim/verilator/harness.cpp`、`fw_harness.cpp`。**特に `atg_kc705_top_tb.v` は KC705 の board の試験なので必ず。** 旧い STATE・KICK に依る試験は、GHCI 1.0 の ring の packet で同じ事を試す形に書き直す。GHCI 1.0 に無い機能だけを試す tb は消してよい（一覧を報告に）。

### S10. lint

- `tools/lint.sh`・`tools/arith-lint.py` を流して 0 件。

### S11. 配置・配線まで

- 前回の build は `tools/vivado-build.sh` の既定の `VIVADO_TIMEOUT=1800`（30 分）で止まった（Gemini の 1 次回答。合成だけで約 20 分）。合成の dcp を `VIVADO_FROM_SYNTH` で使うか、`VIVADO_TIMEOUT=5400` 程度にして、minimal の KC705 で**route の後**の WNS ≥ 0・WHS ≥ 0 と bitstream まで通す。LUT は合成の後 98.34% で余裕が少ない。
- 既定の 1800 秒が KC705 の build に足りない事が分かったので、`tools/vivado-build.sh` の既定を KC705 の build が通る長さにするか、使い方の comment に書く（どちらにしたか報告）。

## 今回触らない物（仕様の問いとしてユーザーに上げた）

- CPFW の 11 個の register が §4.1 の CPFW の窓（PERF と共用の 0x160〜0x17F）に入らない件。今の配置（0x170・0x1A0〜0x1BC）のままにする。SA（id 7）の指す先 0x140 は SA が list に無いのでそのまま。
- RING_SIZE のリセット値、size を変えた時の WPTR・RPTR、RING_BASE_HI が fetch に効かない件、RING_BASE の整列の mask。
- TIMESTAMP の LO・HI の読みの原子性、VEND の VENDOR_ID の値、PERF0〜3 の counter の置き場所、MEM_CACHE_CFG の field の幅と minimal の値、FMT_BUFFER の並び、SESSION_ACTIVE のリセットの関係、DBG の窓。

## 受け入れ

1. S9 の tb が minimal と balanced で全部 PASS（仕様の式の期待値、注入なし、KC705 の parameter、preset ごとの compile の仕組みで）。
2. 既存の tb・harness が PASS（S9 の 8）、`tools/lint.sh`・`tools/arith-lint.py` 0 件。
3. minimal の KC705 で route の後の WNS ≥ 0・WHS ≥ 0、bitstream ができる。

## 報告の書き方

- offset は ghci.md の値、bit は ghci.md の名前で書く。
- S1〜S11 のそれぞれ: 済み・一部・未着手、変えた file、確かめた試験（tb が何をどう照合したか）。
- commit の SHA、試験のコマンドと件数、minimal・balanced の 3 register の 16 進、route の後の WNS・WHS、LUT、bitstream の path と SHA-256。
- S4 の INDIRECT・SECONDARY の判断と根拠、S9 の 8 で消した tb の一覧、決められなかった所。
- 区切りごとに commit してよい。順は S1・S2 → S9 の 4 → S3〜S5 → S6・S7 → S9 の残り → S10 → S11 → S8。
