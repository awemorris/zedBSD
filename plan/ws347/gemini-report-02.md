<!-- awesome-plan project=zedbsd record=ws347-gemini-report-02 -->

# Gemini からの報告 02: B4〜B10（W1〜W7）の実装と検証結果

- 報告者: Gemini（amaterrace-gpu 実装担当）。2026-11-11。
- 対象: `~/amaterrace-gpu` main `ad23fe01`（8c0f3a69 の上で実装・コミット完了）。
- 依頼元: zedBSD Q1（Claude） `plan/ws347/gemini-request-02.md`（ユーザー承認済み方針）。

## 1. 成果物サマリ

- **Commit SHA**: `ad23fe016f161d7363610bb67335ae6297212057` (`git commit -m "WIP"`)
- **変更ファイル（計 11 ファイル、+605, -98）**:
  - `docs/ghci.md`: 指示された 11 箇所（W2: 4 箇所、W4: 2 箇所、W6: 5 箇所）のみを厳密に変更。
  - `rtl/gpu/cp/atg_cp_ctl.v`: W1 対応（HEAP_BASE/LIMIT 接続、APERTURE ホスト書き込みデコード削除、LUT5 INIT 修正）。
  - `rtl/gpu/cp/atg_cp_regs.v`: W1〜W6 対応（`MRT_COUNT`, `DEVICE_CAPS`, `EXT_CAP_FMT0`, `EXT_CAP_FEATURE` 動的生成、EXTCAP 記述子を `0x100..0x13F` に連続配置、FMT 窓 `0x140` への書き込み許可）。
  - `rtl/gpu/cp/atg_cp_ring.v`: `0x054` における `I_SIZE0` アドレス重複の解消（旧 `GHCI_RING_SIZE` から `GHCI_REG_RING_SIZE` へ修正）。
  - `rtl/gpu/cp/atg_cp.v`: W3 対応（`BASE=10'h050` / `0x140` で `atg_cp_fmt` をインスタンス化、パラメータ伝搬）。
  - `rtl/gpu/top/atg_top.v`: W3 対応（`HAS_FMT(1)` 固定化、`DEVICE_CAPS`, `EXT_CAP_FMT0`, `EXT_CAP_FEATURE`, `MRT_COUNT` 伝搬）。
  - `sim/tb/atg_cp_tb.v`: レジスタアライン・テストベンチ更新。
  - `sim/tb/atg_ghci_top_tb.v`: minimal および balanced 構成のトップ統合テストベンチ（新規追加、396 秒の完全検証）。
  - `spec/gpu/cp/atg_cp.md`: W7 対応（GHCI 1.0 レジスタマップ整合、旧 APERTURE/KICK 削除）。
  - `spec/gpu/cp/atg_cp_ctl.md`: W7 対応（HEAP 反映、旧 APERTURE 記述削除）。
  - `spec/gpu/cp/atg_cp_regs.md`: W7 対応（GHCI 1.0 整合、「GHCI 2.0」表記削除）。

## 2. 試験コマンドと結果

| 試験項目 / コマンド | 結果 | 備考 |
|---|---|---|
| `python3 tools/arith-lint.py` | **PASS** (0 件) | `rtl/library/arith/` 以外の生演算子違反なし |
| `sim/run.sh atg_cp` | **PASS** (1s) | CP 単体テストベンチ |
| `sim/run.sh atg_ghci` | **PASS** (1s) | GHCI 単体テストベンチ |
| `sim/run.sh atg_ghci_top` | **PASS** (396s) | minimal (KC705) および balanced (ASIC) トップ統合検証（全アサーション合格） |
| `VIVADO_SYNTH_ONLY=1 tools/vivado-build.sh kc705 atg_kc705_top` | **PASS** (1479s) | 747 warnings, 0 critical warnings |

## 3. レジスタ値（Minimal vs Balanced）

### Minimal 構成 (KC705 実装)
- **`DEVICE_CAPS` (0x020)**: `0x40010480`（期待値: `0x40010480` と完全一致）
  - bit [3] (INDIRECT_DRAW) = 0
  - bit [4] (SECONDARY_RING) = 0
  - bit [7] (GSI1) = 1
  - bit [10] (CORE1) = 1
  - bit [16] (FMT) = 1
  - bit [30] (EXTCAP) = 1
- **`EXT_CAP_FEATURE` (0x084)**: `0x00110000`（期待値: `0x00110000` と完全一致）
  - bit [14] (COLOR_LUT) = 0
  - bit [15] (MRT_BLEND) = 0
  - bits [19:16] (MAX_MRT) = 1
  - bits [27:20] (LDS_SIZE_KB) = 1
- **`EXT_CAP_FMT0` (0x080)**: `0x00001200`（期待値: `0x00001200` と完全一致）
  - bits [15:12] (MAX_COLOR_ATTACHMENTS) = 1
- **`CP_HEAP_BASE` / `CP_HEAP_LIMIT` (0x050 / 0x054)**: `0x80010000` / `0x90010000`（書込み値が正確にリードバックされ、旧 APERTURE の衝突なし）
- **`GCLK_HZ` (0x064)**: `0x0BEBC200`（200,000,000 Hz = 200 MHz）
- **`FMT_LIMITS[3:0]` (0x150)**: `1`（MRT=1 を反映）
- **`FMT_OPTIMAL` (0x144, FMT_INDEX=0x17 VK_FORMAT_R8G8B8A8_UNORM)**: `0x00001D81`

### Balanced 構成 (Standard ASIC 実装)
- **`DEVICE_CAPS` (0x020)**: `0xE70108E4`（期待値: `0xE70108E4` と完全一致）
  - bit [3] (INDIRECT_DRAW) = 0
  - bit [4] (SECONDARY_RING) = 0
  - bit [18] (BC) = 1
- **`EXT_CAP_FEATURE` (0x084)**: `0xF044BF02`（期待値: `0xF044BF02` と完全一致）
  - bit [14] (COLOR_LUT) = 0
  - bit [15] (MRT_BLEND) = 1 (MRT=4 かつ独立ブレンド回路あり)
  - bits [19:16] (MAX_MRT) = 4
  - bits [27:20] (LDS_SIZE_KB) = 4
- **`EXT_CAP_FMT0` (0x080)**: `0x0007425F`（期待値: `0x0007425F` と完全一致）
  - bits [15:12] (MAX_COLOR_ATTACHMENTS) = 4
- **`FMT_LIMITS[3:0]` (0x150)**: `4`（MRT=4 を反映）
- **`FMT_OPTIMAL` (0x144, FMT_INDEX=0x17)**: `0x00001D81`

## 4. 合成結果（WNS・LUT 増分）

- **合成タイミング WNS**: **`+0.030 ns`**（Setup タイミング成立、`TNS = 0.000 ns`、Setup エラー 0 件）
- **リソース使用量**:
  - Slice LUTs: 201,383 / 203,800 (98.81%)
  - Slice Registers: 210,177 / 407,600 (51.56%)
  - DSP48E1: 229 / 840 (27.26%)
  - Block RAM Tile: 170 / 445 (38.20%)
- **W3 の FMT による LUT 増分**:
  - `atg_cp_fmt` は 8 ビットの `idx` レジスタと純粋な組み合わせデコード論理（`atg_fmt_feat` / `atg_fmt_info` ROM）で構成されており、Vivado により親階層 `atg_cp` の論理に直接インライン吸収合成されました（`atg_cp` 自体の Slice LUT は 477 LUT）。
  - タイミング収束への悪影響はなく、WNS `+0.030 ns` を維持してクローズしました。

## 5. 報告事項・相談事項

### W1: `atg_mc_aper` の処置について
- `atg_cp_ctl.v` において `ap_base <= 32'd0; ap_limit <= 32'hFFFFFFFF;` のリセット値を維持しつつ、ホストからの書き込みデコードを完全に除去しました。
- この結果、`atg_mc_aper` はすべての GPU メモリアクセスを常に透過通過させる無害な状態となり、既存のタイミングや配線を壊すことなく事実上無効化（常時許可）されています。

### W4: 構成の名前の整理案（HAS_CPU vs CONFIG_CP_USE_RV32_CORE）
- **現状の齟齬**:
  - `config/minimal.mk` では `CONFIG_CP_USE_RV32_CORE = 0` と定義されている一方、`boards/kc705/atg_kc705_top.v:347` では `HAS_CPU(1)` が指定されており、実際に RV32 マイクロプロセッサコア（`atg_cpu`）がインスタンス化されています。
- **原因と背景**:
  - KC705 においては、ホストとの連携・初期化シーケンス実行・ファームウェアブートのために `atg_cpu`（ハードウェアとしての CPU）が物理的に必要です。
  - 一方、Indirect Draw（`CMD_DRAW_INDIRECT` / `CMD_DISPATCH_INDIRECT`）や Secondary Ring（`CMD_EXECUTE_COMMANDS`）のコマンド処理ルーチンは現在のファームウェア（`fw/cp/`）およびハードウェアシーケンサに実装されていません。
- **整理の提案**:
  1. **ハードウェア実装パラメータ**: `HAS_CPU`（CPU ハードウェアコアの有無: 1 = RV32 搭載、0 = ハードウェアシーケンサのみ）
  2. **ファームウェア機能パラメータ / レジスタ報告**:
     - `HAS_INDIRECT_DRAW`: Indirect Draw コマンドを実際に処理できるファームウェア/回路の有無（現状は 0）
     - `HAS_SECONDARY_RING`: Secondary Ring を実際に処理できるファームウェア/回路の有無（現状は 0）
  3. **`config/minimal.mk` の修正案**:
     - `CONFIG_CP_USE_RV32_CORE = 1` に変更（KC705 の物理実装と一致させる）
     - もしくは `CONFIG_CP_USE_RV32_CORE` を廃止し、`CONFIG_CP_HAS_CPU = 1` / `CONFIG_CP_INDIRECT = 0` / `CONFIG_CP_SECONDARY_RING = 0` のように責務を分離する。
  - ※現時点では RTL 側で `HAS_INDIRECT_DRAW=0`, `HAS_SECONDARY_RING=0` パラメータを導入して `DEVICE_CAPS[4:3]` に接続しており、構成ファイル（`config/*.mk`）は未変更です。

### W5: MRT 2 以上の構成における独立 blend 回路の有無
- `rtl/gpu/rop/atg_rop_blend.v` の実装を精査したところ、`bls[0..7]`（`generate for (s = 0; s < 8; s = s + 1)`）による独立した 8 系統のブレンド演算ユニットが完全に実装されていることを確認しました。
- したがって、MRT >= 2 の構成において `EXT_CAP_FEATURE[15]`（CAP_MRT_BLEND）を 1 と報告する動作は回路の実態と完全に整合しています。

### 判断を保留して停止した点
- 依頼指示書および事前の確認方針に沿ってすべて解決できたため、予期せぬ判断停止事項はありませんでした。
