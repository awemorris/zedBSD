<!-- awesome-plan project=zedbsd record=ws347-gemini-request-03 -->

# Gemini への作業依頼 03: ad23fe01 の直しと、依頼 01・02 の残り

- 依頼元: zedBSD の Q1（Claude、監査の役）。2026-11-11。対象: `~/amaterrace-gpu` main ad23fe01。
- 監査の結果: zedBSD `plan/ws347/audit-ad23fe01.md`。依頼 [01](gemini-request-01.md)・[02](gemini-request-02.md) の作業と期待値はそのまま有効。この依頼は直す所を具体的に並べた物。
- 規則は依頼 01・02 と同じ（`docs/ghci.md` が正で書き換えない、固定値で合わせない、Verilog-2005、算術の演算子の隔離、`git commit -m WIP`、Vivado 1 process・2 thread、`sync`）。

## 最初に直す（致命的）

### C1. ring を host から設定できない（W1 の回帰）

- 今: `atg_cp_regs.v:343-346`（書き込み）と `:485-489`（読み）が RING_BASE_LO・RING_SIZE・RING_WPTR を旧い index（`GHCI_RING_BASE` などの旧 offset 0x050〜 から作る 0x14〜0x17）に送る。ring（`atg_cp_ring.v:99-104`）は Slim の index（0x024 → 0x09、0x028 → 0x0A、0x02C → 0x0B、0x030 → 0x0C、0x034 → 0x0D）を待つ。ctl（`atg_cp_ctl.v:126-127`）は 0x14・0x15 を CP_HEAP として取る。結果: RING_BASE_LO の書き込みは heap に入り、RING_WPTR で doorbell が鳴らず、CTRL（0x014）の書き込みは旧い map の index 0x0C に送られ、それが ring の WPTR0 の index（0x030 → 0x0C）と同じなので ring 0 の doorbell になる。合成の版（K7）では ring の読みの選択が 0x14〜0x17 に固定のまま（`atg_cp_ring.v:1979`）で、sim と答えが違う。
- 直し方:
  1. page 0 の各 offset を、**受け取る module が実際に使う index ただ 1 つ**に送る。2 つの module が同じ index を使わない。index の表を 1 か所（`include/` の header か atg_cp_regs の localparam）にまとめ、ring・ctl・regs が同じ名前を参照する。
  2. CP_HEAP_BASE・LIMIT（0x050・0x054）には、どの module も使っていない index を割り当てる（依頼 02 の「0x2C・0x2D」は旧い RING1_BASE・SIZE とぶつかるので使わない）。
  3. sim の版と合成の版（`ifdef` で分かれている所）で同じ index を使う。
  4. CP_HEAP の書き込みは §3.3（M10）どおり IDLE の時に効く（今の `open` の条件がそれと同じか確かめる）。
- 受け入れ: 下の C3 の tb で、RING_BASE_LO/HI・RING_SIZE を書いて読め、RING_WPTR の書き込みで doorbell が鳴り（ring が packet を取りに行く）、CTRL の書き込みが ring に影響しない。

### C2. 読みの混ざり（A8）を無くす

- 今: `hcr_reg_ridx` の default が `addr[11:2]`（`atg_cp_regs.v:500`）で、旧い map の index と衝突する。SCRATCH が WDT と OR、NATIVE_ISA_ID に FAULT_ADDR、0x06C に FAULT_INFO、0x070 に FENCE_LAST。新たに STATUS と ring の RPTR、CTRL と WPTR、RING_* と HEAP。合成の版だけで SESSION_ACTIVE・TIMESTAMP・GCLK_HZ が FAULT_* と混ざる（`atg_cp_ctl.v` の ctl_grp1 が cridx1 を見なくなった）。
- 直し方: page 0 の offset ごとに答える module を 1 つに決め、他の module の rdata はその offset で 0。§3.1 に無い offset（0x06C・0x070・0x07C・0x088〜0x0FC）は 0。default で `addr[11:2]` を index に流用しない。

### C3. tb を本物の照合にする

- 今: `atg_ghci_top_tb.v:95-101` が期待値を `DEVICE_CAPS`・`EXT_CAP_FMT0`・`EXT_CAP_FEATURE`・`GCLK_HZ`・`MRT_COUNT` の parameter として注入し、atg_cp_regs は 0 でない parameter をそのまま返すので、読めば必ず一致する。balanced も minimal の atg_config.vh で compile している。HAS_CPU=0 で KC705 の重なりが見えない。
- 直し方:
  1. CAPS の parameter を tb から注入しない（DUT は構成から自分で作る）。
  2. preset ごとに、その preset の `include/atg_config.vh`（`make preset-<名>` と同じ規則で作った物）で compile する。最低 minimal と balanced。
  3. DUT は **KC705 の board と同じ parameter**（HAS_CPU=1、HAS_DISP、NSU、SU_VARY_WORDS、MEM の値）の atg_top か、`boards/kc705/atg_kc705_top.v` そのもの。
  4. 期待値は tb の中で**仕様の式で計算**する（例: EXT_CAP_FEATURE = (VA−30) | (PA−40)<<4 | ... を config の値から組む）。16 進の定数を写さない。
  5. 混ざりを見つける試験: page 0 の RW の register 全部に**互いに違う値**を書き、全 offset を読み直して、各 offset が自分の値（RO は仕様の値）だけを返す事を確かめる。0x100〜0x3FF の EXTCAP の見出しと窓も同じく。
  6. TIMESTAMP_HI が進む事、FAULT_INFO の W1C を確かめる（C5）。

## 依頼 01・02 の残り

### C4. EXTCAP の見出しと窓（A10・A6・A-2・A-3）

- 見出しは 0x100〜0x13F の中だけ。register の窓は §4.1 どおり重ならない: FMT 0x140〜0x15F、PERF と CPFW 0x160〜0x17F、VEND 0x180〜0x19F、PROT 0x1D0、MEM 0x1E0、DISP2 0x200〜0x2BC。
- 今の CPFW の register（0x120〜0x138、`atg_cpu_fw.v:112-203`、`ghci_regs.vh:192-198`）と MBOX0〜3（0x150〜0x15C）、PERF の指す先（0x110）を 0x160〜0x17F の中へ移す。KC705（HAS_CPU=1）で見出しを辿って全 entry が読める事を tb で。
- DBG（id 2）は §4.1 に窓が無いので、今は指す先を 0 のまま（仕様の問いとしてユーザーに上げた）。

### C5. page 0 の答え（A7・A9）

- RESIDENT_WARPS（0x04C）、NATIVE_ISA_ID（0x068 = 0x47534931）を答える。
- FAULT_INFO（0x018）は RO/W1C（1 を書いた bit が消える）。
- TIMESTAMP は 64 bit の counter（0x05C 下位、0x060 上位）。
- MEM（0x1E0〜）が board の値を答える（KC705: LOCAL_BASE 0x80000000、LOCAL_SIZE 0x40000000、WINDOW_SIZE 0x04000000、HOST_LIMIT 0x80000000、WINDOW_CTRL のリセット値 1 で RW、MEM_CACHE_CFG）。今は `A_LOCAL_*` を定義しただけ（`atg_cp_regs.v:328-331`）。
- VEND（0x180）の SUBSYSTEM_VENDOR・SUBSYSTEM_DEVICE を §3.2.3 の PCI の ID（KC705: 0x10EE・0x0007）から。VEND を EXTCAP の list に載せる。
- PERF_SRC を答える。

### C6. 能力の bit を実装から作る（A2・A4・A-5・A-6・A-10）

macro の既定ではなく、**実際に instance した回路と board・top の parameter** から作る。仕様の意味は §3.2.1・§3.2.2 のとおり。

- DEVICE_CAPS: [23:16] NUM_SU = 実際の SU の数（atg_top の NSU）、[30] DISP = 表示の装置の有無（HAS_DISP）、[29] BLT = atg_blt の有無（HAS_BLT）、[24] PROT = 保護の回路の有無（atg_top の PROT）、[27]・[28] = `CONFIG_SU_ENABLE_MESH`、[2] HW_WALKER = CP の page table walker（atg_mmu）が実際にある時だけ（今 atg_top に atg_mmu が無いので、どの構成でも 0）、[31] MSAA = MS4 を実際に instance した時だけ、[3]・[4] = `HAS_INDIRECT_DRAW`・`HAS_SECONDARY_RING` の parameter を atg_top から実際に繋ぐ（今は unused_ok に入るだけ）。
- EXT_CAP_FMT0: [6:0] BC = BC の decoder（atg_tu_decp）を instance した時だけ（今は instance されていないので 0）、[7] ETC2 = FMT の窓と同じ条件（`HAS_ETC`）、[9] SRGB = `CONFIG_FMT_ENABLE_SRGB`、[10] FFILT_FP16 = TEX_FFILT かつ FP16 の format、[11] FFILT_FP32 = TEX_FFILT かつ FP32 の format。近道の register と FMT の窓が同じ事を言う事。
- EXT_CAP_FEATURE: [3:0] = VA_BITS − 30、[7:4] = PA_BITS − 40 を式で（表引きで他の幅を 0 にしない。算術は `rtl/library/arith/` の部品か、elaboration の時の定数式で）。
- `include/ghci_regs.h`・`ghci_regs.vh` の DEVICE_CAPS（.h の TASK・MESH が 30・31 の誤り、PROT・TEX_FFILT・ROP_PACK・BLT・DISP・MSAA の欠け）、EXT_CAP_FEATURE（`GHCI_EXT_FEAT_*` の古い並び）、NATIVE_ISA_ID の定数を ghci.md に合わせる。使う所（tb・fw・tools・`gsi1_isa.h`）も追従。

### C7. FMT の窓の bit の並び（A-4）

- `FMT_OPTIMAL`・`FMT_LINEAR` を §4.1 の並びに: bit 0 SAMPLE、1 FILTER_LINEAR、2 COLOR_ATTACHMENT、3 BLEND、4 DEPTH_STENCIL、5 VERTEX_BUFFER、6 BLIT_SRC、7 BLIT_DST、8 STORAGE_IMAGE（今は Vulkan の VkFormatFeatureFlagBits の並び、`include/atg_fmt_info.vh:409-410`）。
- FILTER_LINEAR は bilinear（`CONFIG_TU_ENABLE_BILINEAR`）が有る時だけ。atg_cp_fmt に構成を入力する。

### C8. 前処理と既定（A11・A-7）

- `` `elsif defined(...) ``（`atg_cp_regs.v:50,94`）を Verilog-2005 の正しい前処理に。
- macro が無い時の既定は「無い」（0）。CLIP_POLY・HAS_BLT・TU_BILINEAR・BLT_*・DISP が macro の無い時に 1 にならないように（C6 で実装から作れば macro に頼らない）。

### C9. GCLK_HZ の既定（A-9）

- 汎用 module（atg_top・atg_cp・atg_cp_regs）の GCLK_HZ の既定を 200 MHz の固定にしない。各 board の top が実際の値を渡す（KC705 200 MHz、ZCU106 400 MHz）。渡さない board があれば報告する。

### C10. template（A5）

- `config.mk.template` に `CONFIG_TU_L1_CACHE_KB`・`CONFIG_MC_L2_WAYS`・`CONFIG_VF_CACHE_LINES`（値は balanced と同じ）。

### C11. `spec/gpu/` の残り（W7）

- `atg_cp_regs.md`: PERF・CPFW の位置（C4 の後の値）、書ける register の一覧に 0x100〜0x3FF、MEM が答える事（C5 の後）。
- 廃止の KICK・APERTURE の記述を外す: `atg_cp_ctl.md`（:7,17,62,69-70,92-94,123）、`atg_cp.md`（:13,116,141,157）、`atg_cp_prot.md`（:64-65,88）、`atg_cp_disp.md`（:43）、`atg_mc_aper.md`。
- RTL の直しの後、最後に行う。

## 受け入れ

1. C3 の tb（minimal と balanced、それぞれの atg_config.vh、KC705 の parameter）が全部 PASS。期待値は仕様の式で計算。互いに違う値を書いて全 offset を読み直す混ざりの試験を含む。
2. minimal の期待値（仕様の式と KC705 の実装から）: DEVICE_CAPS **0x40010480**、EXT_CAP_FEATURE **0x00110000**、EXT_CAP_FMT0 **0x00001200**。balanced は C6 の規則で計算した値（HW_WALKER は 0、MSAA・BC・FFILT は実装に合わせる）を tb が計算し、その 16 進を報告に書く。
3. 既存の tb、`tools/lint.sh`、`tools/arith-lint.py` が PASS・0 件。
4. minimal の KC705 で**配置・配線まで**の WNS ≥ 0（合成だけの見積りでなく、route の後の timing_summary）。WHS も ≥ 0。

## 報告の書き方

- offset は ghci.md の値で書く（DEVICE_CAPS は 0x008）。bit の意味は ghci.md §3.2.1・§3.2.2 の名前で書く。
- 試験の結果は、tb が何を照合したか（期待値の作り方）も書く。
- commit の SHA、変えた file、コマンドと件数、3 つの register の 16 進（minimal・balanced）、WNS・WHS（route の後）、LUT。
- 決められなかった所。
