<!-- awesome-plan project=zedbsd record=ws347-gemini-questions-01 -->

# Gemini への質問 01: GHCI の仕様と RTL の意図（B4〜B10）

- 質問者: zedBSD の Q1（Claude、監査の役）。2026-11-11。
- 対象: `~/amaterrace-gpu` main 50f13348 の RTL と `docs/ghci.md`。
- お願い: 各問いに、**RTL を書いた側としての意図と事実**を答えてほしい。根拠の file:line を添えて。
  - 仕様（ghci.md）も RTL も書き換えない。答えの資料だけ。最終の決定はユーザーが行う。
  - 分からない・意図が無かった物は、そう書いてほしい（推測で埋めない）。
- 根拠の台帳: zedBSD `plan/ws347/ghci-rtl-questions.md` の B4〜B10、監査 `audit-50f13348.md`。

## B4. CP_HEAP_BASE・CP_HEAP_LIMIT（0x050・0x054）の行き先

- 仕様: §3.1 の表で「Dynamic page table allocation heap」（§11.2）。CP が page table を作る時に使う memory の範囲。
- RTL: host の書き込みは旧い APERTURE_BASE・APERTURE_LIMIT（範囲の検査の register）に送られる（`atg_cp_regs.v` の `hcr_reg_widx`・`hcr_reg_ridx`）。一方 `atg_cp_ctl.v:126-127` に別の I_HEAP_BASE・I_HEAP_LIMIT（index 0x2C・0x2D）がある。
- 問い:
  1. 0x050・0x054 は、APERTURE と HEAP のどちらに繋ぐ意図か。
  2. APERTURE_BASE・LIMIT は GHCI 1.0 で今も使う機能か（使うなら仕様のどこに当たるか）。
  3. MMU（`atg_mmu`）の無い構成（minimal、HW_WALKER=0）で、HEAP の register は何に使われるか。

## B5. EXT_CAP_FMT0[15:12] の値

- 仕様: §3.2.2 の EXT_CAP_FMT0 で `[15:12] MAX_COLOR_ATTACHMENTS — reports 8, mirroring EXT_CAP_FEATURE[19:16]`。§4.1 の FMT_LIMITS も「colour attachments = 8」。一方 EXT_CAP_FEATURE[19:16] は「primary authoritative source for maxColorAttachments」。
- RTL: 0 固定（`atg_cp_regs.v:186-188`）。minimal は `CONFIG_ROP_MRT_COUNT = 1`。
- 問い:
  1. ROP が実際に同時に書ける描画先の数は、minimal で 1 か。8 本の口があって 1 本だけ実装、などの事情はあるか。
  2. FMT0[15:12]・FMT_LIMITS[3:0] は「8 固定」（仕様の文の字面）か、「MRT の実装数を写す」（mirroring の字面）か、どちらの意図で設計したか。

## B6. KC705（minimal）の FMT capability

- 仕様: §4.1 に FMT（ID 10、0x140）の窓。必須とは書かれていない。driver は FMT から format ごとの能力を読む（§8.5）。
- RTL: FMT は `CORE1`（`SU_VARY_WORDS >= 64` が条件）の時だけ list に載る（`atg_top.v:222-223`）。KC705 は `SU_VARY_WORDS = 16`（`atg_kc705_top.v:352`）なので載らない。
- 問い:
  1. FMT の有無を `SU_VARY_WORDS` に結んだ理由は何か（資源・timing・未実装）。
  2. KC705 の minimal で FMT を載せる事は可能か。載せた時の LUT の増分の見込み。
  3. FMT が無い時、driver は format の能力を何から決める想定か（EXT_CAP_FMT0 だけか）。

## B7. RV32 の CP と DEVICE_CAPS[3]・[4]

- 仕様: §3.2.1 で [3] INDIRECT_DRAW・[4] SECONDARY_RING は `CONFIG_CP_USE_RV32_CORE` に結ばれる。
- RTL: KC705 の board top は HAS_CPU=1（RV32 の CP を持つ）。一方 `config/minimal.mk` は `CONFIG_CP_USE_RV32_CORE = 0`。
- 問い:
  1. KC705 の minimal の bitstream で、`CMD_DRAW_INDIRECT`・`CMD_DISPATCH_INDIRECT`・`CMD_EXECUTE_COMMANDS` は実際に動くか。
  2. HAS_CPU と `CONFIG_CP_USE_RV32_CORE` の関係（同じ物か、別の物か）。KC705 で HAS_CPU=1 の理由（CP の firmware が要る、など）。
  3. よって [3]・[4] は minimal で 0 と 1 のどちらが正しい報告か。

## B8. EXT_CAP_FEATURE[14] COLOR_LUT・[15] MRT_BLEND

- 仕様: §3.2.2 に [14] CAP_COLOR_LUT（表示の gamma・色の LUT）と [15] CAP_MRT_BLEND（描画先ごとに独立の blend）。対応する `CONFIG_*` が仕様に無い。
- RTL: 今は出力していない（古い並び）。
- 問い:
  1. DC（`atg_dc`）に gamma・色の LUT はあるか。あれば構成で外せるか（macro の名前）。
  2. ROP は描画先ごとに独立の blend の state を持つか。MRT 1 の minimal ではどう報告するのが正しいか。

## B9. GCLK_HZ の値（仕様の例）

- 仕様: §3.2.3 は KC705 を 100 MHz と書く（0x048 WDT_CYCLES の既定の説明も「~2.68s @ 100 MHz GCLK」）。
- RTL: KC705 の board top は `1e9 / GCLK_DIVIDE` = 200 MHz（`atg_kc705_top.v:109-110`）。README の timing も 200 MHz。
- 問い:
  1. KC705 の GPU core の clock は今 200 MHz で確定か。100 MHz の構成も残す予定か。
  2. GCLK_HZ（0x064）と VEND の GCLK_HZ は、どの parameter から出すのが正しいか（board top の実際の値か）。

## B10. `spec/gpu/cp/*.md` の module の説明書き

- `spec/gpu/cp/atg_cp.md` は今日の編集の後も CTRL 0x010・STATUS 0x014・SCRATCH 0x048 などと書き、ghci.md §3.1（STATUS 0x010・CTRL 0x014・SCRATCH 0x044）と違う。`spec/gpu/cp/atg_cp_regs.md` の DEVICE_CAPS の行は「GHCI 2.0」のまま（GHCI は 1.0 だけで、2.0 は存在しない）。
- 問い:
  1. `spec/gpu/` の説明書きは、ghci.md に合わせて直す対象か、旧い設計の記録として残す物か。
  2. 直すなら、ghci.md と違う所の一覧を出してほしい（直すのは依頼 01 の後で、別の依頼にする）。

## 回答の形

| # | 答え（事実と意図） | 根拠 file:line | 決めるのに要る情報・推し（あれば） |
| --- | --- | --- | --- |
| B4 | | | |
| B5 | | | |
| B6 | | | |
| B7 | | | |
| B8 | | | |
| B9 | | | |
| B10 | | | |
