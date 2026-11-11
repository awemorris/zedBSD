<!-- awesome-plan project=zedbsd record=ws347-gemini-answers-01 -->

# Gemini の 1 次回答（B4〜B10）と Q1 の照合（2026-11-11）

- 質問: [gemini-questions-01.md](gemini-questions-01.md)。回答はユーザーが受け取って Q1 に渡した（修正方針の打診）。
- Q1 の照合: amaterrace-gpu main 8c0f3a69 で、回答の根拠の file:line を読んで確かめた（sim・実機ではない）。決定はユーザー。

| # | Gemini の答え（要点） | Gemini の修正方針 | Q1 の照合 | 仕様の文の変更 |
| --- | --- | --- | --- | --- |
| B4 | 0x050・0x054 は HEAP に繋ぐ意図。APERTURE は GHCI 1.0 で廃止。minimal（MMU 無し）では HEAP は機能上使わない | 0x050・0x054 を atg_cp_ctl の I_HEAP_BASE・LIMIT（内部の index 0x2C・0x2D）へ。APERTURE への host の書き込みの decode を廃止 | 確かめた: ghci.md:186-188 が APERTURE・KICK の廃止を明記。`atg_cp_regs.v:281-282` が HEAP を APERTURE に送る。`atg_cp_ctl.v:126-127` の I_HEAP（旧 offset 0x0B0・0x0B4 の index）。仕様どおりの直し | 不要 |
| B5 | minimal の描画先は物理的に 1。FMT0[15:12] は MRT の実装数を写すのが意図、0 固定は bit の詰めのバグ | RTL: [15:12] に MRT 数。仕様の「reports 8」を「configured MRT count」に | 意図は仕様の mirroring の文と合う。**仕様の 8 を書く所は 4 か所**: ghci.md:293（EXT_CAP_FEATURE の説明）、:505（FMT_LIMITS[3:0] = 8）、:1596（maxColorAttachments の表 8）、§3.2.2 の FMT0。Gemini の方針は FMT0 の 1 か所だけで、FMT_LIMITS[3:0] の RTL も同じ扱いが要る | 要（ユーザーの判断） |
| B6 | CORE1 に結んだのは初期の名残。minimal に載せられる、約 30〜50 LUT。FMT 無しの時は driver の静的な表を想定 | HAS_FMT=1 を常に | `atg_cp_fmt.v` は 70 行・組み合わせの decoder で、LUT の見積りは妥当。driver の静的な表は zedBSD の方針（driver は仕様と CAPS だけに従う）と合わないので、FMT を常に載せる方が driver に良い | 不要 |
| B7 | minimal の bitstream で INDIRECT・SECONDARY は動かない（FW・HW とも未実装）。HAS_CPU と CONFIG_CP_USE_RV32_CORE は同じ概念。KC705 は host の連携に RV32 の FW が要るので HAS_CPU=1 | [3]・[4] は「実際に実行できるか」を表す flag とし、minimal は 0 | minimal で 0 は正しい。ただし仕様 §3.2.1 は [3]・[4] を `CONFIG_CP_USE_RV32_CORE` に結んでいるので、意味を「実行できるか」に変えるのは仕様の文の変更。また「同じ概念」なのに minimal.mk は 0、KC705 の board top は HAS_CPU=1 で食い違う（構成の名前と実際の装置が一致しない）。整理が要る | 要（ユーザーの判断） |
| B8 | DC に gamma・LUT は無い。minimal の ROP は描画先 1 で独立の blend は無い | [14]=0、[15]=0（MRT>1 かつ独立 blend の構成だけ 1） | 仕様 :291 は [15] を「描画先ごとの独立 blend」と書き、:859 は 0 の時の fault を書く。minimal で 0 は仕様と整合。MRT>1 の構成で独立 blend の回路が有るかは答えに無い | 不要 |
| B9 | KC705 の core は 200 MHz で確定 | GCLK_HZ と VEND は board top の実際の値。ghci.md §3.2.3 の 100 MHz を 200 MHz に | 方針は妥当。**仕様の 100 MHz は 5 か所**: :224（WDT_CYCLES の ~2.68s @ 100 MHz）、:318（KC705 100,000,000）、:322（timestampPeriod 10.0 ns）、:1602（表 10.0 ns）、:1655（~2.68 s）。Gemini は §3.2.3 だけを挙げた | 要（ユーザーの判断、例の数値の直し） |
| B10 | spec/gpu/ は ghci.md に合わせる対象 | 依頼 01 の後に文書の同期の別 task | 方針は妥当。**Gemini の齟齬の一覧に誤り**: 「INTR_STATUS/ACK（spec 0x028/0x02C vs ghci 0x018/0x01C）」とあるが、ghci.md の 0x018・0x01C は FAULT_INFO・FAULT_ADDR_LO で、IRQ_STATUS・IRQ_ENABLE は 0x038・0x03C。atg_cp.md:79-80 は IRQ_STATUS 0x018・RING_BASE_LO 0x020 などと書く。atg_cp_regs.md:81 は SCRATCH を 0x028 とも書く。同期の task は全行の照合（Claude が一覧を作れる）から始めるのがよい | 不要（spec/gpu/ の側を直す） |
