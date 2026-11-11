<!-- awesome-plan project=zedbsd record=ws347-gemini-request-04 -->

# Gemini への作業依頼 04（まとめ版）: GHCI 1.0 の host の register の残り全部

- 依頼元: zedBSD の Q1（Claude、監査の役）。2026-11-11。対象: `~/amaterrace-gpu` main 8ebb2e2f。
- **この文書は依頼 01〜03 と前の版の依頼 04 を全部置き換える。読むのはこの文書だけでよい。** 作業の番号は R1〜R14 に振り直した。
- 監査の記録（zedBSD 側）: `plan/ws347/audit-50f13348.md`・`audit-ad23fe01.md`・`audit-8ebb2e2f.md`。

## 0. 守ってほしい事

1. `docs/ghci.md` が正。**仕様は書き換えない。** 仕様が黙っている所に当たったら、推測で埋めずに止めて報告に書く。
2. 値は**実際に instance した回路と board・top の parameter**から作る。固定値や macro の既定で辻褄を合わせない。macro が無い時の既定は「無い」（0）。
3. Verilog-2005、`rtl/library/arith/` の外に算術の演算子を書かない、Vivado は host で 1 process・2 thread、実行の後に `sync`。
4. **commit message は `WIP` ちょうど**（`git commit -m WIP`）。8ebb2e2f の `WIP: fix C1 ...` は repo の AGENTS.md の違反。
5. 終わった物を壊さない（下の「済んだ物」）。

## 済んだ物（壊さない）

- SLIM_MMIO の撤去（KC705 も Slim の page 0 だけ）。
- page 0 の各 offset を `include/ghci_regs.vh:65-96` の `HCR_IDX_*` の index ただ 1 つに送る形（2 つの module が同じ index を受けない）。RING_* は ring、CP_HEAP は ctl、CTRL は ctl。sim と合成で同じ経路。
- 読みの混ざりの撤去（`hcr_reg_ridx` の default は `HCR_IDX_INVALID`、§3.1 に無い offset は 0）。
- DEVICE_CAPS・EXT_CAP_FEATURE・EXT_CAP_FMT0 の bit の**並び**（中身の出所は R7 で直す）。EXT_CAP_FMT0[15:12]・FMT_LIMITS[3:0] に MRT 数。EXT_CAP_FEATURE[14] = 0・[15] = MRT ≥ 2。ghci.md の 11 か所の文。

## 1. ring（最優先）

### R1. RING_SIZE を byte の数で受ける

- 仕様（§3.1 の 0x02C、§6.1）: ring の大きさの **byte の数**（2 の冪、4 KiB〜16 MiB、例 64 KiB = `0x00010000`）。容量 n = RING_SIZE / 4 dword、RING_WPTR・RING_RPTR は [0, n−1] の dword の index。
- 今: `atg_cp_ring.v:1368-1370` が `hcr_wdata[4:0]` を log2 として受け 10〜20 に clamp（1 KiB〜1 MiB）。0x10000 を書くと size は 10。16 MiB を表せない（WPTR は 18 bit）。
- 直し方: byte の数を受け（内部で log2 に変えて持つのはよい）、読み返しは書いた byte の数。WPTR・RPTR は 22 bit（4M dword）まで。4 KiB〜16 MiB の 2 の冪でない値の扱い（INVALID_ARG の fault か無視か）を決めて報告に書く。

### R2. 走行中の RING_* の書き込みを LOCKED_REG にする

- 仕様: §3.3 M10 は RING_BASE_LO/HI・RING_SIZE・CP_HEAP_BASE/LIMIT を IDLE の時だけ書ける register とし、§3.5 は走行中の書き込みを kind 7 `LOCKED_REG` とする。
- 今: `atg_cp_ctl.v:205` の `wr_locked` が CP_HEAP だけ。RING_* は黙って無視。
- 直し方: `wr_locked` に RING_BASE_LO・RING_BASE_HI・RING_SIZE を足す（`HCR_IDX_*` の名前で）。

## 2. page 0（0x000〜0x0FC）

### R3. 答えの足りない register

| offset | 名前 | 期待（KC705・minimal） |
| --- | --- | --- |
| 0x018 | FAULT_INFO | RO/W1C: 1 を書いた bit が消える（今は書き込みを受け取る module が無い） |
| 0x04C | RESIDENT_WARPS | [15:0] 常駐の warp 数、[23:16] lane あたりの register 数（今は 0） |
| 0x058 | SESSION_ACTIVE | §11.6 の値（PROT が無ければ session 0 だけ。今は 0 固定） |
| 0x05C・0x060 | TIMESTAMP_LO・HI | **64 bit** の GPU の cycle の counter（今は 32 bit で HI が 0） |
| 0x064 | GCLK_HZ | board の実際の値（KC705 200000000、R11） |
| 0x068 | NATIVE_ISA_ID | 0x47534931（済み、壊さない） |

## 3. EXTCAP（0x100〜0x3FF）

### R4. 見出しと窓の配置（§4.1）

- 見出しは 0x100〜0x13F の中だけ（各 8 byte: word 0 = [7:0] ID・[15:8] version・[31:16] 次までの dword 数（最後は 0）、word 1 = 窓の byte offset）。
- 窓は重ならない: FMT 0x140〜0x15F、PERF と CPFW 0x160〜0x17F、VEND 0x180〜0x19F、PROT 0x1D0〜0x1DF、MEM 0x1E0〜0x1F7、DISP2 0x200〜0x2BC。
- 今: PERF の指す先が 0x110、CPFW の register（0x120〜0x138、`atg_cpu_fw.v:112-203`、`ghci_regs.vh:192-198`）が見出しの領域に重なり、KC705（HAS_CPU=1）で list を辿れない。CPFW の MBOX0〜3（0x150〜0x15C）が FMT の窓に重なる。→ PERF・CPFW・MBOX を 0x160〜0x17F に移す。
- DBG（id 2）は §4.1 に窓が無いので、指す先は 0 のまま（仕様の問いとしてユーザーに上げてある）。

### R5. 窓の答えと書き込み

- MEM（0x1E0〜）が board の値を答える（今は `A_LOCAL_*` を定義しただけ、`atg_cp_regs.v:328-331`）。KC705: LOCAL_BASE 0x80000000、LOCAL_SIZE 0x40000000、WINDOW_SIZE 0x04000000、HOST_LIMIT 0x80000000、WINDOW_CTRL（RW、リセット値 1）、MEM_CACHE_CFG（§11 の式）。
- VEND（0x180）を list に載せ、SUBSYSTEM_VENDOR・SUBSYSTEM_DEVICE を §3.2.3 の PCI の ID（KC705: 0x10EE・0x0007）から。PERF_SRC を答える。
- 書き込み: 窓の RW の register（DISP2、MEM の WINDOW_CTRL、FMT_INDEX、PROT、CPFW）に書ける。RO の見出し（0x100〜0x13F）は `hcr_we_ok` から外す。

### R6. FMT の窓の bit の並び

- `FMT_OPTIMAL`・`FMT_LINEAR` を §4.1 の並びに: bit 0 SAMPLE、1 FILTER_LINEAR、2 COLOR_ATTACHMENT、3 BLEND、4 DEPTH_STENCIL、5 VERTEX_BUFFER、6 BLIT_SRC、7 BLIT_DST、8 STORAGE_IMAGE（今は Vulkan の VkFormatFeatureFlagBits の並び、`include/atg_fmt_info.vh:409-410`）。
- FILTER_LINEAR は bilinear（`CONFIG_TU_ENABLE_BILINEAR`）が有る時だけ。atg_cp_fmt に構成を入力する。

## 4. 能力の register の中身

### R7. 実装から作る

仕様の意味は §3.2.1・§3.2.2。

- **DEVICE_CAPS**: [23:16] NUM_SU = atg_top の NSU（今 1 固定）。[30] DISP = HAS_DISP（今は Makefile が作らない macro の既定 1）。[29] BLT = atg_blt の有無。[24] PROT = atg_top の PROT。[27]・[28] = `CONFIG_SU_ENABLE_MESH`（今 [27] は存在しない `CFG_ENABLE_TASK`）。[2] HW_WALKER = atg_mmu が実際にある時だけ（今 atg_top に無いので全構成で 0）。[31] MSAA = MS4 を実際に instance した時だけ。[3]・[4] = `HAS_INDIRECT_DRAW`・`HAS_SECONDARY_RING` を atg_top から実際に繋ぐ（今は unused_ok に入るだけ）。
- **EXT_CAP_FMT0**: [6:0] BC = BC の decoder（atg_tu_decp）を instance した時だけ（今は instance されていないので 0）。[7] ETC2 = FMT の窓と同じ条件（`HAS_ETC`）。[9] SRGB = `CONFIG_FMT_ENABLE_SRGB`（今 1 固定）。[10] FFILT_FP16 = TEX_FFILT かつ FP16 の format、[11] FFILT_FP32 = TEX_FFILT かつ FP32 の format（今 0 固定）。近道の register と FMT の窓が同じ事を言う事。
- **EXT_CAP_FEATURE**: [3:0] = VA_BITS − 30、[7:4] = PA_BITS − 40 を式で（今は 30・32・40／40・48 以外を 0 にする表引き）。
- **期待値**: minimal（KC705）は DEVICE_CAPS **0x40010480**、EXT_CAP_FEATURE **0x00110000**、EXT_CAP_FMT0 **0x00001200**。balanced は上の規則で計算した値（HW_WALKER 0、MSAA・BC・FFILT は実装どおり）を tb が計算し、報告に 16 進で書く。

### R8. include の定義

- `include/ghci_regs.h`・`ghci_regs.vh` を ghci.md に合わせる: DEVICE_CAPS の bit（.h の TASK・MESH が 30・31 の誤り、PROT・TEX_FFILT・ROP_PACK・BLT・DISP・MSAA の欠け）、EXT_CAP_FEATURE の `GHCI_EXT_FEAT_*`（古い並び）、NATIVE_ISA_ID（0x068）の定数。
- 旧い map の名前（`ghci_regs.vh:168-239`、`ghci_regs.h:210` の `GHCI_CTRL` = 0x030、`GHCI_RING_BASE` = 0x050、`GHCI_KICK` など）が新しい map と重なる値のまま公開されている。消すか `GHCI_LEGACY_*` に変える。
- 使う所（tb・fw・tools・`gsi1_isa.h`・`sim/verilator/harness.cpp`）を追従させる。

## 5. 構成・前処理・clock

### R9. 前処理と既定

- `` `elsif defined(...) ``（`atg_cp_regs.v:50,94`）を Verilog-2005 の正しい前処理に。
- macro が無い時に CLIP_POLY・HAS_BLT・TU_BILINEAR・BLT_*・DISP が 1 にならないように（R7 で実装から作れば macro に頼らない）。

### R10. 死んだ論理

- page 0 から届かなくなった旧い index の論理（`atg_cp_ring.v:106-128`、`atg_cp_fence.v:87`、`atg_cp_dbg.v:59-61`、`atg_cp_prot.v:123-124`、`atg_cp_regs.v:325-326`）のうち、GHCI 1.0 で使わない物を消す。
- `atg_cp_ctl.v:198-199` の `kick_present`・`kick_busy`・入力 `kick_prot_ok` の未使用、`atg_cp_ctl.v:36,40` の古い comment（KICK・APERTURE）も直す。

### R11. GCLK_HZ の既定

- 汎用 module（atg_top・atg_cp・atg_cp_regs）の GCLK_HZ の既定を 200 MHz の固定にしない。各 board の top が実際の値を渡す（KC705 200 MHz、ZCU106 400 MHz）。渡さない board は報告に書く。

### R12. config.mk.template

- `CONFIG_TU_L1_CACHE_KB`・`CONFIG_MC_L2_WAYS`・`CONFIG_VF_CACHE_LINES` を足す（値は `config/balanced.mk` と同じ）。

## 6. 試験と文書

### R13. tb を本物の照合にする

`sim/tb/atg_ghci_top_tb.v` と既存の tb:

1. **注入しない**: CAPS・GCLK_HZ・MRT_COUNT を tb から parameter で注入しない（今 `:95-101`・`:115-121`）。DUT が構成から作る。
2. **preset ごとの構成**: minimal と balanced を、それぞれの `include/atg_config.vh`（`make preset-<名>` と同じ規則で作った物）で compile する。
3. **KC705 と同じ DUT**: HAS_CPU=1・HAS_DISP・NSU・SU_VARY_WORDS・MEM の値（`boards/kc705/atg_kc705_top.v` そのものか、同じ parameter の atg_top）。
4. **期待値は仕様の式で計算**（16 進の定数を写さない）。
5. **混ざりの試験**: page 0 の RW の register の全部（CTRL・RING_BASE_LO/HI・RING_SIZE・RING_WPTR・IRQ_ENABLE・SCRATCH・WDT_CYCLES・CP_HEAP_BASE/LIMIT）に互いに違う値を書き、0x000〜0x0FC の**全 offset**を読み直し、各 offset が自分の値（RO は仕様の値、§3.1 に無い offset は 0）だけを返す事。EXTCAP の見出しと窓も、KC705 で見出しを辿って全 entry を読む。
6. **ring**: CTRL.RUN = 1 で ring に 1 つの packet（例: `CMD_FENCE` で host の memory に値を書く物）を置いて RING_WPTR を書き、RING_RPTR が進み fence の値が書かれる事。IDLE で CTRL に RUN 以外の bit や RESET を書いても ring が動かない事。RUN の前の WPTR では packet を取りに行かない事。
7. **R1**: 0x00010000 を書いて読み返しが 0x00010000、WPTR が 16383 の次に 0 に戻る事。**R2**: RUNNING で RING_SIZE を書くと FAULT_INFO の kind が 7 になり値が変わらない事。**R3**: FAULT_INFO の W1C、TIMESTAMP_HI が進む事。
8. 既存の tb・harness（`atg_top_tb.v`、`atg_cp_ring_tb.v`、`atg_kc705_top_tb.v`、`sim/verilator/harness.cpp` など、旧い map の名前を使う約 15 file）を §3.1 の offset に直して PASS させる。直せない物は一覧を報告に書く。
9. 「RING_BASE_LO preserved after HEAP write」の照合は書き込みの順を直す（RING を先に書き、HEAP の後に RING を読む）。

### R14. `spec/gpu/` の文書（RTL の直しの後、最後に）

- `atg_cp_regs.md`: PERF・CPFW の位置（R4 の後の値）、書ける register の一覧に 0x100〜0x3FF、MEM が答える事（R5 の後）。
- 廃止の KICK・APERTURE の記述を外す: `atg_cp_ctl.md`（:7,17,62,69-70,92-94,123）、`atg_cp.md`（:13,116,141,157）、`atg_cp_prot.md`（:64-65,88）、`atg_cp_disp.md`（:43）、`atg_mc_aper.md`。

## 受け入れ

1. R13 の tb が minimal と balanced で全部 PASS（期待値は仕様の式、注入なし、KC705 の parameter）。
2. 既存の tb、`tools/lint.sh`、`tools/arith-lint.py` が PASS・0 件。
3. minimal の KC705 で**配置・配線の後**の WNS ≥ 0 と WHS ≥ 0（合成だけの見積りでなく、route の後の timing_summary）。読みの経路を組合せにしたので、足りなければ読みの経路を flop で切る（読みの latency の増加は仕様上かまわない）。

## 報告の書き方

- offset は ghci.md の値（DEVICE_CAPS は 0x008）、bit は ghci.md §3.2.1・§3.2.2 の名前で書く。
- R1〜R14 のそれぞれに: 済み・一部・未着手、変えた file、確かめた試験。
- commit の SHA（message は `WIP`）、試験のコマンドと件数（tb が何をどう照合したかも）、3 つの register の 16 進（minimal・balanced）、route の後の WNS・WHS、LUT。
- 決められなかった所（R1 の不正な値の扱いなど）。
- 量が多いので、R1〜R3 → R13 の 6・7 → R4〜R8 → R9〜R12 → R13 の残り → R14 の順に、区切りごとに commit してよい。
