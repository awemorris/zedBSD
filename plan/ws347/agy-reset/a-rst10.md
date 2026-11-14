I have scheduled a check for the running `atg_fmt_cvt` simulation and will resume once it completes.
Waiting for the 400,000-cycle `atg_fmt_cvt` simulation to finish.
Waiting for `atg_fmt_cvt` simulation to complete.
Waiting for `atg_fmt_cvt` 400k-cycle simulation.
Waiting for the simulation to finish.
Continuing to wait for `atg_fmt_cvt` simulation.
Waiting for simulation to finish.
Waiting for `atg_fmt_cvt` simulation.
Waiting for simulation to finish.
Waiting for simulation to finish.
Waiting for simulation to finish.
Continuing to wait for `atg_fmt_cvt` simulation.
Waiting for simulation to finish.
Continuing to wait for `atg_fmt_cvt` simulation.
Waiting for simulation to finish.
Waiting for `atg_fmt_cvt` simulation.
Waiting for simulation to finish.
Monitoring simulations.
Waiting for simulation progress.
Waiting for `task-17541` to complete.
Waiting for `task-17541` to complete.
Waiting for `atg_fmt` testbench to complete.
Waiting for `atg_fmt` testbench.
Waiting for `atg_fmt` testbench to complete.
Waiting for `atg_fmt` testbench.
Waiting for `atg_fmt` testbench to complete.
Waiting for `atg_fmt` testbench.
Waiting for `atg_fmt` testbench to complete.
Waiting for `atg_fmt` testbench.
Waiting for `atg_fmt` testbench to finish.
Waiting for `atg_fmt` testbench.
Waiting for lint checks to complete.
Waiting for lint checks.
## 1. 前の回（9 回目）への指摘への対応

### (1) `atg_dc_regs.v` の非 reset 振る舞い 3 点の戻し
方針 8 に従い、9 回目前のコミット（`1cbdc20a`）の振る舞いに厳格に戻しました：
1. `connected_q` のリセット値を `1'b0` に復元。
2. `g_twrite`（タイミングレジスタ書き込み）から `&& !quasi_bad` を削除。
3. `do_reset_q` による 1 サイクル遅延（`else if (do_reset_q)`）を復元。

### (2) `atg_dc_regs` 単体 TB 失敗の `1cbdc20a` 照合・原因・直し方案
`1cbdc20a` 時点でも同一の FAIL が発生することを確認しました。原因と `docs/ghci.md` に照らした修正案は以下のとおりです：

1. **`FAIL atg_dc_regs.IRQ_STATUS is DISP's only: 00000008, expected 00000000`**
   - **原因**: TB は reset 解除前に `connected = 1` を印加。`atg_dc_regs.v` はリセット時に `connected_q <= 1'b0` とするため、リセット解除直後に `ev_hotplug_pulse = connected ^ connected_q`（$1 \oplus 0 = 1$）がアサートされ、`disp_irq_status[3]`（`GHCI_DC_IRQ_HOTPLUG`）が立ってしまう。
   - **仕様照合**: [docs/ghci.md](file:///home/awe/amaterrace-gpu/docs/ghci.md#L305) §3.7「HOTPLUG: connector state changed since last cleared」。初期接続状態での起動時はリセット解除時点で変化とみなさないか、`atg_dc_mode.v`（line 112）のデフォルト接続前提と整合させることが望ましい。
   - **直し方案**: リセット解除直後の hotplug 検出をマスクするか、`connected_q` を初期状態 `connected`（または 1）と整合させる。
2. **`FAIL atg_dc_regs.V_ACTIVE kept: 000002d0, expected 00000104`**
   - **原因**: TB は `ENABLE == 1` の状態で `V_ACTIVE`（準静的レジスタ）への書き込み棄却をテスト。`atg_dc_regs.v` の `g_twrite` 内の判定 `we_t_l/h` に `!quasi_bad` が抜けていたため書き込みが通ってしまう。
   - **仕様照合**: [docs/ghci.md](file:///home/awe/amaterrace-gpu/docs/ghci.md#L274) §3.7「Quasi-static registers (TIMING_*, FB_*, PCLK_SEL) must only be written when ENABLE == 0. Writes while enabled are ignored and set ERROR bit BAD_WRITE」。
   - **直し方案**: タイミングレジスタ書き込みイネーブルに `!quasi_bad` 条件を付加する。
3. **`FAIL atg_dc_regs.cancelled by RESET: 00000002, expected 00000003`** (および `atg_dc.cancelled by RESET`)
   - **原因**: `do_reset` が `do_reset_q` で 1 サイクル遅延してフリップキャンセルを発行するため、TB の即時チェックタイミングで 1 カウント遅れが生じる。
   - **仕様照合**: [docs/ghci.md](file:///home/awe/amaterrace-gpu/docs/ghci.md#L276) §3.7「RESET: write 1 to trigger display engine software reset」。
   - **直し方案**: フリップキューのキャンセルを `do_reset` で即座に行う。

### (3) `atg_dc_tgen.v`・`atg_dc_plane2.v` の方針 4 修正
- **`atg_dc_tgen.v`**: 誤って方針 4 除外としていたパイプライン段・制御ビット（`vrr_en_q`, `flip_ready_q`, `vrr_vmax_q`, `p1_*`, `p2_*`, `ha`〜`pol`, `bar_last_r`, `ha_last_r`, `va_last_r`, `v_last_match_r`, `vs_c_r`, `eff_vmax_r`, `vrr_hold_r`）をすべて `always @(posedge clk or negedge rst_n)` でリセット初期化。
- **`atg_dc_plane2.v`**: 純粋な SRL 遅延列（`sel_ov_rgb_q2`, `prim_rgb_q2`, `alpha_q2`）のみを同期保持とし、`end_x_r`, `end_y_r`, `p_*_q`, `active_overlay_q2` を `always @(posedge clk or negedge rst_n)` で非同期リセット化。

---

## 2. 今回の 5 ファイルの要点

| ファイル | 非同期にしたブロック数 | 新しく reset を入れたレジスタ |
| :--- | :---: | :--- |
| **`rtl/gpu/dc/atg_dc_tmds.v`** | 3 ブロック<br>(チャネル loop 展開含め計 7) | • トップ: `s1_de`, `s1_ctrl` (0 で初期化)<br>• `g_ch` チャネル毎: `s1_qm`, `s1_n1_r`, `cnt`, `out` (0 で初期化)<br>※ `qm_xor` のプレフィックス XOR を最適化し Verilator `UNOPTFLAT` 警告を完全解消 |
| **`rtl/gpu/dc/atg_dc_ycc.v`** | 1 ブロック | `y1` (16), `cb1` (128), `cr1` (128), `y2` (16), `c2` (128), `de1` (0), `de2` (0), `odd` (0), `cr_held` (128) |
| **`rtl/gpu/fmt/atg_fmt_cvt.v`** | 7 ブロック | • 方針 5 に従い `input wire rst_n` をポートに追加<br>• U1 $\rightarrow$ R0 段: `cst0_*`, `cval0`, `raw0_*`, `pos0_*`, `w0_*`, `ty0_*`, `k0_*`, `e5e0`<br>• U1b $\rightarrow$ R1 段: `cst1_*`, `cval1`, `fld1_*`, `w1_*`, `ty1_*`, `k1_*`, `e5e1`<br>• U2 $\rightarrow$ R2 段: `kind2_*`, `cval2_*`, `fld2_*`, `w2_*`, `neg2`, `snmin2`, `hs2`, `he2`, `x2`, `e5b2`<br>• U3 $\rightarrow$ R3 段: `kind3`, `dir3_*`, `rr3`, `neg3`, `snmin3`, `hs3`, `he3`, `x3`, `hlz3`, `e5b3`<br>• U3b $\rightarrow$ R3b 段: `kind3b`, `dir3b_*`, `r3_hi`, `r_low_or`, `lz3*`, `found3`, `neg3b`, `snmin3b`, `hs3b`, `he3b`, `x3b`, `hlz3b_*`, `e5b3b`<br>• U4 $\rightarrow$ R4 段: `kind4_*`, `dir4_*`, `um4`, `st4`, `lz4`, `found4`, `neg4`, `snmin4`<br>• U5 $\rightarrow$ R5 / 出力段: `o_val` (すべて 0 で初期化) |
| **`rtl/gpu/fmt/atg_fmt_pack.v`** | 3 ブロック | `reg_s1` (Stage 1 パイプライン段、257'd0), `reg_s2` (Stage 2 パイプライン段、`{{128{1'b1}}, 128'd0}`) |
| **`rtl/gpu/fmt/atg_fmt_srgbenc.v`** | 1 ブロック | `state` (`S_IDLE`), `x` (0), `lo` (0), `step` (`8'h80`), `below` (`7'h7F`) |

---

## 3. 変えなかった register と理由の表

| モジュール | レジスタ名 | 理由 |
| :--- | :--- | :--- |
| **`atg_dc_plane2.v`** | `sel_ov_rgb_q2`, `prim_rgb_q2`, `alpha_q2` | **方針 4**: 純粋に前段信号を遅らせるだけの SRL 推論対象遅延チェーン |
| **`atg_fmt_pack.v`** | `v_leaf[*].raw`, `v_leaf[*].wr`, `v_leaf[*].hit` | **方針 9**: コンビネーショナル `always @(*)` ブロック内の論理変数 |

---

## 4. 5 ファイルの外で変えた行

1. **`rtl/gpu/vf/atg_vf_conv.v`**:
   - line 668: [atg_vf_conv.v#L668](file:///home/awe/amaterrace-gpu/rtl/gpu/vf/atg_vf_conv.v#L668)（方針 5 に基づき `u_cvt` インスタンス化に `.rst_n(rst_n)` を接続）
2. **`sim/tb/atg_fmt_cvt_tb.v`**:
   - line 23: [atg_fmt_cvt_tb.v#L23](file:///home/awe/amaterrace-gpu/sim/tb/atg_fmt_cvt_tb.v#L23)（`reg rst_n = 1'b1;` を追加）
   - line 32: [atg_fmt_cvt_tb.v#L32](file:///home/awe/amaterrace-gpu/sim/tb/atg_fmt_cvt_tb.v#L32)（`dut` インスタンス化に `.rst_n(rst_n)` を接続）
3. **`rtl/gpu/dc/atg_dc_regs.v`**:
   - lines 313–314, 373, 382, 472, 474: 9 回目の指摘事項に基づき、リセット外の振る舞い 3 点を `1cbdc20a` の状態に戻し
4. **`rtl/gpu/dc/atg_dc_tgen.v`**:
   - lines 85–94, 121–142, 153–174, 182–205, 222–245, 279–284, 296–303, 321–326, 376–381, 407–412, 419–424: 9 回目の指摘事項に基づき、通常段・制御ビットを `negedge rst_n` 非同期リセット化
5. **`rtl/gpu/dc/atg_dc_plane2.v`**:
   - lines 99–106, 216–227: 9 回目の指摘事項に基づき、`end_x/y_r`, `p_*_q`, `active_overlay_q2` を `negedge rst_n` 非同期リセット化

---

## 5. Lint と Testbench の結果

### (1) Verilator Lint & 算術演算子 Linter（全 8 モジュール 0 warnings / 0 violations）
- **`atg_dc_tmds`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**
- **`atg_dc_ycc`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**
- **`atg_fmt_cvt`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**
- **`atg_fmt_pack`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**
- **`atg_fmt_srgbenc`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**
- **`atg_dc_tgen`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**
- **`atg_dc_plane2`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**
- **`atg_dc_regs`**: Generic: **0 warnings**, Kintex-7: **0 warnings**, arith-lint: **0 operators**

### (2) 単体 Testbench 実行結果
- `sim/run.sh atg_dc_tmds` $\rightarrow$ **PASS (2 s)**
- `sim/run.sh atg_dc_ycc` $\rightarrow$ **PASS (1 s)**
- `sim/run.sh atg_fmt_pack` $\rightarrow$ **PASS (86 s)**
- `sim/run.sh atg_fmt_srgbenc` $\rightarrow$ **PASS (0 s)**
- `sim/run.sh atg_dc_tgen:+quick` $\rightarrow$ **PASS (2 s)**
- `sim/run.sh atg_fmt` $\rightarrow$ **PASS (442 s)**
- `sim/run.sh atg_vf_conv` $\rightarrow$ **PASS (14 s)**（`atg_fmt_cvt` のインスタンス `u_cvt` を含む検証）
- `sim/run.sh atg_fmt_cvt` $\rightarrow$ 1,500 秒のデフォルトタイムアウト（120,800 サイクル経過時点でミスマッチ 0 件）

---

## 6. Claude 側で流してほしい上位テストベンチ候補

1. **`atg_fmt_cvt`**（フル 400,000 サイクル実行：400k-cycle 完走には約 80 分 / `SIM_TIMEOUT=5400` が必要）
2. **`atg_dc`**（`atg_dc_regs` の既知の課題 3 点が解消された後に全体パス確認）
3. **`atg_vf`**（`atg_fmt_cvt` を内包する頂点フェッチ全体の統合 TB）
4. **`atg_top`** / **`atg_top_tex`** / **`atg_top_core`**

---

## 7. Commit SHA

- **Commit**: `38593b0ec4e1e3557e0fa0cff3aa9723ecdbf7a5` (`-m WIP`)

DONE

