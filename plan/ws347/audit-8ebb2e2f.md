<!-- awesome-plan project=zedbsd record=ws347-audit-8ebb2e2f -->

# 監査: amaterrace-gpu 8ebb2e2f（依頼 03 の C1・C2）（2026-11-11）

- 監査の担当（Opus、読むだけ、sim・lint・合成は走らせていない）。RTL を読んだ見込み。Q1 が RING_SIZE（`atg_cp_ring.v:1368-1370` と ghci.md:217・:666）と `wr_locked`（`atg_cp_ctl.v:205`）を自分でも確かめた。
- 対象: `git diff ad23fe01 8ebb2e2f`（7 file）。依頼 [03](gemini-request-03.md) の C1・C2 だけ（ユーザーが絞った）。

## 結論

- **C1 はほぼ完了**: page 0 の各 offset が `include/ghci_regs.vh:65-96` の `HCR_IDX_*`（offset[11:2]）の index ただ 1 つに送られ、ring・ctl・irq・regs が同じ名前を参照する。sim と K7 で同じ経路。RING_* は ring、CP_HEAP は ctl、CTRL は ctl だけ。RING・CP_HEAP の書き込みは IDLE の時だけ効く。page 0 から作れる index を 2 つの module が使う所は無い。
- **C2 は完了**: `hcr_reg_ridx` の default は `HCR_IDX_INVALID`（0x3FF）。§3.1 の各 offset に答えるのは 1 つの module だけ。0x06C・0x070・0x07C・0x088〜0x0FC は 0。前回の混ざりは全部無くなった。
- 消えた行は K7 専用の読みの LUT・mux、ctl の KICK（0 に固定）、旧い index の localparam。GHCI 1.0 で要る機能（ring の取り込み、doorbell、fence、IRQ、FAULT_*、watchdog、RESET の順、DISP2、CPFW）を壊していない見込み。消えた port は無い。

## 残り（A）

1. **RING_SIZE の符号化が仕様と違う**: 仕様（ghci.md:217・:666）は byte の数（2 の冪、4 KiB〜16 MiB、例 0x00010000）。RTL は `hcr_wdata[4:0]` を log2 として受け 10〜20 に clamp（`atg_cp_ring.v:1368-1370`、1 KiB〜1 MiB）。仕様どおりの driver が 0x10000 を書くと size は 10 になる。16 MiB も表せない（WPTR は 18 bit）。tb は 16 を書いて 16 を期待し、この違いを隠している。
2. **RUNNING 中の RING_* の書き込みが LOCKED_REG の fault にならない**: `atg_cp_ctl.v:205` の `wr_locked` が CP_HEAP だけで、RING_BASE_LO/HI・RING_SIZE が外れた。§3.5（ghci.md:426）は保護の register を走行中に書くと kind 7 LOCKED_REG、M10（:362）は RING_* を IDLE だけで書ける register とする。今は黙って無視。
3. **FAULT_INFO（0x018）の書き込みを受け取る module が無い**（W1C、C5）。RESIDENT_WARPS・TIMESTAMP_HI・SESSION_ACTIVE は 0（C5）。
4. **旧い名前が新しい map と重なる値のまま公開**: `include/ghci_regs.vh:168-239`・`ghci_regs.h:210` の `GHCI_CTRL`（0x030、今は RING_WPTR）・`GHCI_RING_BASE`（0x050、今は CP_HEAP）・`GHCI_KICK` など。これを使う既存の tb・harness（`atg_top_tb.v`、`atg_cp_ring_tb.v`、`atg_kc705_top_tb.v`、`sim/verilator/harness.cpp` など約 15 file）は PASS しない見込み。旧い map の index は 5 つの file に死んだ論理として残る（`atg_cp_ring.v:106-128`、`atg_cp_fence.v:87`、`atg_cp_dbg.v:59-61`、`atg_cp_prot.v:123-124`、`atg_cp_regs.v:325-326`）。
5. **lint の見込み**: `atg_cp_ctl.v:198-199` の `kick_present`・`kick_busy` と入力 `kick_prot_ok` が使われない。comment（`atg_cp_ctl.v:36,40`）に KICK・APERTURE・RING の locked の古い記述。
6. **EXTCAP の見出し（RO）にも `hcr_we` が出る**（`hcr_we_ok` が 0x100〜0x3FF の全部を通す）。受け取る module が無いので今は害が無い。
7. **commit message** が `WIP: fix C1 ...` で、repo の AGENTS.md の「`WIP` ちょうど」に違反。

## tb（`atg_ghci_top_tb.v:227-284`）

- 確かめている: RING_BASE_LO・HI・SIZE・WPTR の読み返し、CTRL に 0 を書いた後に WPTR が変わらない、NATIVE_ISA_ID、§3.1 に無い 9 つの offset が 0、SCRATCH と HEAP の読み直し。
- 確かめていない: doorbell で ring が packet を取りに行く事、CTRL の RUN・RESET で ring が動かない事、全 RW register に互いに違う値を書いて全 offset を読み直す事（WDT・IRQ_ENABLE・CTRL・STATUS・RPTR を照合していない、0x088〜0x0FC は 9 点だけ）。
- 「RING_BASE_LO preserved after HEAP write」は書き込みの順が逆で照合になっていない。RING_SIZE の 16 は仕様の値ではない。
- C3 の範囲（CAPS の注入、HAS_CPU=0、両 preset が同じ atg_config.vh）はそのまま。

## 仕様の問い（B）

- RING_BASE の整列: RTL は 32 byte で mask する（`& 32'hFFFFFFE0`）。仕様は 64 byte 整列を求めるが、hardware が下位 bit を mask するか・fault にするかは書いていない。

## 危険（未確認）

- K7 の DONT_TOUCH の LUT の木を消し、ring・ctl・irq・fence の読みが組合せの比較になった。route の後の WNS が悪くなりうる。
