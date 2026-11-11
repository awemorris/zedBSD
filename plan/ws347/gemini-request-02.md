<!-- awesome-plan project=zedbsd record=ws347-gemini-request-02 -->

# Gemini への作業依頼 02: B4〜B10 の修正方針（ユーザー承認済み）

- 依頼元: zedBSD の Q1（Claude、監査の役）。2026-11-11。
- 前提: あなたの 1 次回答（B4〜B10）に対して、ユーザーが下の修正方針を承認した。照合の記録は zedBSD `plan/ws347/gemini-answers-01.md`。
- 対象: `~/amaterrace-gpu` main（8c0f3a69 で確認）の RTL・`docs/ghci.md`・`spec/gpu/`。
- **依頼 01 との関係**: 依頼 01 で「対象外」にした B5・B6 は、この依頼で決まった。依頼 01 の T2 の「EXT_CAP_FMT0[15:12] は今の値のまま」と T3 の「FMT を載せるかは対象外」は、下の W2・W3 で置き換える。依頼 01 の他の作業はそのまま。

## 守ってほしい事

1. `docs/ghci.md` を書き換えるのは、下に**書き換えの文を指定した所だけ**。指定の文どおりにし、それ以外の仕様の文は変えない。
2. amaterrace-gpu の AGENTS.md の規則（Verilog-2005、算術の演算子の隔離、`git commit -m WIP`、Vivado 1 process・2 thread、`sync`）。
3. 値は構成と board・top の parameter から作る。固定値で合わせない。
4. 指定に無い判断が要る所に当たったら、推測で埋めずに止めて報告する。

## 作業

### W1（B4）CP_HEAP_BASE・CP_HEAP_LIMIT を HEAP の register へ — RTL だけ

- `atg_cp_regs.v` の host の書き込み・読みの decode で、0x050・0x054 を `atg_cp_ctl` の `I_HEAP_BASE`・`I_HEAP_LIMIT`（内部の index 0x2C・0x2D）に繋ぐ（今は `GHCI_APERTURE_BASE`・`LIMIT` へ、`atg_cp_regs.v:281-282`）。
- host から APERTURE_BASE・APERTURE_LIMIT への書き込みの decode を廃止する（ghci.md:186-188 が APERTURE・KICK の廃止を明記）。`atg_mc_aper` の境界の検査の回路をどう扱うか（残すなら何を入力にするか）は報告に書く。
- 仕様の変更: なし。

### W2（B5）EXT_CAP_FMT0[15:12] と FMT_LIMITS[3:0] に MRT の実装数 — RTL と仕様

- RTL: `EXT_CAP_FMT0[15:12]` と FMT の `FMT_LIMITS[3:0]` に `CONFIG_ROP_MRT_COUNT`（`ATG_DEF_MRT_COUNT`）を出す（今は [15:12] が 0 固定）。`EXT_CAP_FEATURE[19:16]` と同じ値。minimal は 1。
- 仕様の書き換え（4 か所、この文に置き換える）:
  - ghci.md:293 の括弧の中:
    `(reports 8 on standard Amaterrace GPU, matching ...)` → `(reports the implemented colour attachment count, e.g. 8 on the full configuration and 1 on the minimal configuration, matching `EXT_CAP_FMT0[15:12]` and `FMT_LIMITS[3:0]`)`
  - ghci.md:308:
    `- `[15:12]`: `MAX_COLOR_ATTACHMENTS` — Hardware color attachment count limit (reports 8, mirroring `EXT_CAP_FEATURE[19:16]`).` → `- `[15:12]`: `MAX_COLOR_ATTACHMENTS` — Hardware color attachment count limit (the implemented count, mirroring `EXT_CAP_FEATURE[19:16]`).`
  - ghci.md:505: `[3:0] colour attachments` = 8` → `[3:0] colour attachments` (the implemented count, mirroring `EXT_CAP_FEATURE[19:16]`)`
  - ghci.md:1596 の表の値の欄: `| 8 |` → `| 1..8 (implemented count) |`

### W3（B6）FMT capability を常に載せる — RTL だけ

- FMT（ID 10、窓 0x140）の有無を `CORE1`（`SU_VARY_WORDS >= 64`、`atg_top.v:222-223`）から切り離し、全構成で list に載せる（HAS_FMT=1）。KC705・minimal でも載る。
- 依頼 01 の T3 の配置（FMT の窓は 0x140）に合わせる。LUT の増分を報告に書く。
- 仕様の変更: なし。

### W4（B7）DEVICE_CAPS[3]・[4] は「実際に実行できるか」— 仕様と RTL、構成の整理は提案だけ

- 仕様の書き換え（ghci.md:253-254 の「Config Parameter」の欄）:
  - [3] の `` `CONFIG_CP_USE_RV32_CORE` `` → `Firmware / hardware support`（意味の欄の文は変えない）
  - [4] の `` `CONFIG_CP_USE_RV32_CORE` `` → `Firmware / hardware support`（同上）
- RTL: [3]・[4] は、その command を実際に実行できる時だけ 1。KC705・minimal の今の FW と HW では 0。今の `ATG_DEF_RV32` から作る式は、実装の有無を表す parameter（無ければ 0 固定でなく、実装の有無を表す名前の parameter を足してそれが 0）に置き換える。
- **構成の名前の整理は提案だけ（変えない）**: あなたの回答では HAS_CPU と `CONFIG_CP_USE_RV32_CORE` は同じ概念だが、`config/minimal.mk` は `CONFIG_CP_USE_RV32_CORE = 0`、KC705 の board top は `HAS_CPU = 1`（`atg_kc705_top.v:347`）で食い違う。どう揃えるか（名前・どちらを正にするか・preset の値）の案を報告に書く。ユーザーが決めてから別の依頼にする。

### W5（B8）EXT_CAP_FEATURE[14]・[15] — RTL だけ

- [14] CAP_COLOR_LUT: DC に LUT が無いので 0。
- [15] CAP_MRT_BLEND: 描画先が 2 以上で、描画先ごとに独立の blend の回路がある構成だけ 1。minimal は 0。
- MRT が 2 以上の構成（balanced・high-perf）で、独立の blend の回路が実際にあるかを報告に書く（無いなら 0）。
- 仕様の変更: なし（ghci.md:291・:859 と整合）。

### W6（B9）KC705 の clock は 200 MHz — RTL と仕様

- RTL: GCLK_HZ（0x064）と VEND の GCLK_HZ を board top の実際の値（KC705 は 200000000）から出す。
- 仕様の書き換え（5 か所、この文に置き換える）:
  - ghci.md:224 の `~2.68s @ 100 MHz GCLK` → `~1.34s @ 200 MHz GCLK`
  - ghci.md:318: `- Kintex-7 KC705 implementation: `100,000,000` (100 MHz, `0x05F5E100`).` → `- Kintex-7 KC705 implementation: `200,000,000` (200 MHz, `0x0BEBC200`).`
  - ghci.md:322: `At 100 MHz, `timestampPeriod = 10.0` ns.` → `At 200 MHz, `timestampPeriod = 5.0` ns.`
  - ghci.md:1602 の表: `| 10.0 ns nominal | Core clock 100 MHz (...` → `| 5.0 ns nominal | Core clock 200 MHz (...`（括弧の中はそのまま）
  - ghci.md:1655: `$\approx 2.68$ seconds at 100 MHz core clock` → `$\approx 1.34$ seconds at 200 MHz core clock`

### W7（B10）`spec/gpu/` を ghci.md に揃える — 文書だけ、依頼 01 の後

- `spec/gpu/cp/atg_cp.md`・`spec/gpu/cp/atg_cp_regs.md` ほか `spec/gpu/` の register の offset・名前・版の表記を ghci.md（§3.1、§4.1）に揃える。「GHCI 2.0」の表記は全部 GHCI 1.0 に。廃止の APERTURE・KICK の記述は外す。
- **注意**: 1 次回答の齟齬の一覧に誤りがある。「INTR_STATUS/ACK は ghci の 0x018/0x01C」は誤り。ghci.md の 0x018・0x01C は FAULT_INFO・FAULT_ADDR_LO、IRQ_STATUS・IRQ_ENABLE は 0x038・0x03C。一覧からでなく、ghci.md §3.1 の表と全行を照らして直す。
- 例: `atg_cp.md:79-80` は CTRL 0x010・STATUS 0x014・IRQ_STATUS 0x018・IRQ_ENABLE 0x01C・RING_BASE_LO 0x020…、`atg_cp_regs.md:27` は「GHCI 2.0」、`:79` は KICK 0xF00、`:81` は SCRATCH 0x028。
- W1〜W6 と依頼 01 の RTL が終わってから行う（文書を RTL の最後の形に合わせるため）。

## 受け入れ

- 依頼 01 の tb（atg_top を通した物、minimal と balanced）に足して:
  - 0x050・0x054 に書いて読める、APERTURE の旧 offset への書き込みが何も変えない（W1）。
  - minimal で EXT_CAP_FMT0[15:12] = 1、FMT_LIMITS[3:0] = 1、balanced で MRT 数と一致（W2）。
  - minimal で EXTCAP の list に FMT（ID 10、0x140）が載り、FMT_INDEX に書いて FMT_OPTIMAL などが読める（W3）。
  - minimal で DEVICE_CAPS[3]・[4] = 0（W4）。EXT_CAP_FEATURE[14]・[15] = 0（W5）。GCLK_HZ = 200000000（W6）。
- minimal の期待値（依頼 01 から更新）: DEVICE_CAPS = 0x40010480、EXT_CAP_FEATURE = 0x00110000、**EXT_CAP_FMT0 = 0x00001200**。
- 既存の tb・`tools/lint.sh`・`tools/arith-lint.py` が PASS・0 件。minimal の KC705 の合成で WNS ≥ 0。
- `git diff` で、ghci.md の変更が上で指定した 11 か所（W2 の 4・W4 の 2・W6 の 5）だけである事。

## 報告してほしい物

- commit の SHA、変えた file、試験のコマンドと結果（件数）、minimal と balanced の 3 つの register の 16 進の値、LUT の増分（W3）、WNS。
- W1 の `atg_mc_aper` の扱い、W4 の構成の名前の整理の案、W5 の MRT 2 以上の構成の独立 blend の有無。
- 指定に無い判断が要って止めた所。
