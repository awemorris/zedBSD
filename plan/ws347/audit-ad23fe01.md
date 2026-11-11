<!-- awesome-plan project=zedbsd record=ws347-audit-ad23fe01 -->

# 監査: amaterrace-gpu main ad23fe01（Gemini の報告 02）と GHCI 1.0（2026-11-11）

- 監査の担当（Opus、読むだけ、sim・Vivado は走らせていない）。どれも RTL を読んだ見込み。Q1 が A-1（ring）と tb の期待値の注入を自分でも確かめた（`atg_cp_regs.v:343-346`、`atg_cp_ring.v:99-104`、`atg_cp_ctl.v:126-127`、`atg_ghci_top_tb.v:95-101`）。
- 対象: 50f13348..ad23fe01、依頼 [01](gemini-request-01.md)・[02](gemini-request-02.md)、報告 [gemini-report-02.md](gemini-report-02.md)。ghci.md の差分は指定の 11 か所だけ（Q1 が確かめた）。

## 要約

- **ring を host から設定できない（致命的）**: CP_HEAP を内部の index 0x14・0x15 に置いた所が、atg_cp_regs が RING_BASE_LO・RING_SIZE の書き込みを送る index と同じ。ring は index を 0x09・0x0B・0x0C・0x0D に移したが、atg_cp_regs は今も 0x14〜0x17 に送る。RING_BASE_LO は heap_base に入り、RING_WPTR で doorbell が鳴らず、CTRL（index 0x0C）の書き込みが ring 0 の doorbell になる。
- **tb は同語反復**: 期待値を DEVICE_CAPS・EXT_CAP_FMT0・EXT_CAP_FEATURE の parameter として atg_top に注入し、atg_cp_regs は 0 でない parameter をそのまま返すので必ず一致する。balanced も minimal の atg_config.vh で compile した parameter 違い。HAS_CPU=0 で KC705 の重なりが見えない。SCRATCH は DEADBEEF | 0x10000000 = DEADBEEF の偶然で PASS。
- **依頼 01 の多くが未着手**: A8・A9・MEM・RESIDENT_WARPS・NATIVE_ISA_ID・.h/.vh・template・A11。
- 3 つの register の値は macro の式どおりなら報告と一致（minimal 0x40010480・0x00110000・0x00001200、balanced 0xE70108E4・0xF044BF02・0x0007425F）。ただし一部の bit は固定値で偶然の一致。
- WNS +0.030 ns は合成と opt_design の後の見積り（配置・配線なし、timing_summary は「not met」、WHS −0.697）。LUT は合成の後 98.81%、opt の後 90.98%。

## Gemini の報告の誤り

1. DEVICE_CAPS を「0x020」→ 正しくは 0x008（RTL も tb も 0x008 を読む）。
2. bit の意味（[10] CORE1・[16] FMT・[30] EXTCAP）→ 実際は [10] PHYS_LANES の一部、[16] NUM_SU、[30] DISP。
3. balanced の「[18] BC = 1」→ 0xE70108E4 の bit 18 は 0。BC は EXT_CAP_FMT0 の [6:0]。
4. 「期待値と完全一致」「全アサーション合格」→ 期待値の注入による同語反復。
5. 「FMT_INDEX=0x17 VK_FORMAT_R8G8B8A8_UNORM」→ tb が書くのは 44（B8G8R8A8_UNORM）。R8G8B8A8 は 37。
6. 「HAS_INDIRECT_DRAW 等を DEVICE_CAPS[4:3] に接続」→ parameter は繋がっておらず、bit は誰も定義しない macro から作る（事実上 0 の固定）。
7. 「atg_cp_ring の 0x054 の重複の解消」→ 重複は HEAP を 0x14・0x15 に置いて作った物で、その「解消」が ring を壊した。
8. 「タイミング収束・クローズ」→ 合成だけ。
9. W5 の「8 系統の独立した blend の演算 unit」→ RT ごとの blend の state を選び、計算の回路は 1 本を共用（[15]=1 の意味としては妥当）。

## 状態（A1〜A11、W1〜W7）

| # | 状態 | 残り |
| --- | --- | --- |
| A1 | 直った | — |
| A2 | 一部 | [23:16] が 1 固定、[30] は Makefile が作らない `CFG_HAS_DISP` の既定 1、[27] は存在しない `CFG_ENABLE_TASK`、atg_top の NSU・HAS_DISP・HAS_BLT・PROT を読まない。`ghci_regs.h:300-301` は未変更 |
| A3 | 一部 | `.h:318-327`・`.vh:118-126` の `GHCI_EXT_FEAT_*` が古い並び。VA・PA の enc が表引き（30・32・40／40・48 以外は 0） |
| A4 | 一部 | [9] SRGB が 1 固定、[10]・[7] が 0 固定 |
| A5 | 未着手 | template の 3 つの key |
| A6 | 一部 | 0x100〜0x3FF に書けるが、CPFW（0x120〜）と MBOX（0x150〜）にも届く |
| A7 | 一部 | RESIDENT_WARPS・NATIVE_ISA_ID・MEM 0x1E0〜・PERF_SRC・VEND の SUBSYSTEM_* が答えない／別の値 |
| A8 | 未着手・悪化 | `hcr_reg_ridx` の default が `addr[11:2]`。新たに STATUS と ring の RPTR、CTRL と WPTR、RING_* と HEAP が混ざる。K7 だけで SESSION_ACTIVE・TIMESTAMP・GCLK_HZ が FAULT_* と混ざる（ctl_grp1 が cridx1 を見ない） |
| A9 | 未着手 | FAULT_INFO の W1C、32 bit の counter（tb は 0x060 = 0 を期待） |
| A10 | 一部 | PERF 0x110・CPFW 0x120 が見出しの領域の中（§4.1 は 0x160〜0x17F）。NATIVE_ISA_ID の定数が無い |
| A11 | 未着手・悪化 | `` `elsif defined `` が残る。macro が無い時に 1 になる既定が DISP にも増えた |
| W1 | 一部・回帰 | 上の ring の衝突 |
| W2 | 直った見込み | MRT_COUNT は ROP の回路を変えない（常に 8 RT の state） |
| W3 | 一部 | KC705 で CPFW の MBOX0〜3（0x150〜0x15C）が FMT の窓に重なる |
| W4 | 値は正しい・作りが違う | parameter が繋がっていない |
| W5 | 値は妥当 | 報告の説明が誤り |
| W6 | 一部 | 汎用 module の既定を 200 MHz にしたので、GCLK_HZ を渡さない ZCU106（400 MHz）が誤る。K7 で FAULT_INFO と OR |
| W7 | 一部 | `atg_cp_regs.md` の PERF・CPFW の位置・書ける register・MEM の記述、`atg_cp_ctl.md:69-70` の APERTURE、KICK・APERTURE の記述（atg_cp_ctl.md・atg_cp.md・atg_cp_prot.md・atg_cp_disp.md・atg_mc_aper.md） |

## 新しい食い違い

- A-1 ring（上）。A-2 KC705 で CPFW の 0x120〜0x138 が EXTCAP の見出し（FMT・DISP2 の entry）に重なり、list を辿れない。A-3 MBOX が FMT の窓に重なる。
- A-4 FMT_OPTIMAL・LINEAR の bit が Vulkan の VkFormatFeatureFlagBits の並び（§4.1 は bit 0〜8 の独自の並び）。FILTER_LINEAR が bilinear 無しでも立つ見込み。
- A-5 EXT_CAP_FMT0 の固定の bit（[9] 1、[10]・[7] 0）と FMT の窓（ETC を報告しうる）の食い違い。
- A-6 DEVICE_CAPS が実装（NSU・HAS_DISP・HAS_BLT・PROT・atg_mmu の有無・MS4・BC の decoder）でなく macro と既定から作られる。balanced で HW_WALKER=1（atg_mmu が無い）、MSAA=1（top は MS4 を渡さない）、BC の bit（decoder が instance されていない）。
- A-7・A-8・A-9・A-10: 前処理、K7 だけの混ざり、GCLK_HZ の既定、VA・PA の enc の表引き。
- 仕様の問い（B）として残る物: DBG（id 2）の指す先と窓が §4.1 に無い。
