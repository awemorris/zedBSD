# i915 再構築 S5 — 試験の移設

[実施計画](i915-rebuild-plan.md)、[共通規則](i915-rebuild-rules.md)。試験は `src/drivers/gpu/i915/tests/` と
`plan/ws031/tests/` に置く。本番コードは試験を呼ばない。試験 build（`I915_TESTS=y`）だけが tests/ を link し、
本番の weak checkpoint を実装する（coding-style §2 の weak 宣言、§12: 試験専用の環境変数・分岐を本番に置かない）。

## 1. kernel 試験の入口（固定）

- `device.c` は node を公開して serving に入る直前に、weak な
  `void drv_i915_test_after_start(struct i915_device *device)` を呼ぶ（NULL なら何もしない）。
  定義は `tests/execution/runner.c`（T4a の所有）。runner は試験 build の compile 時定数
  `I915_TEST_SCENARIO`（`ZEDBSD_TEST_CPPFLAGS` で与える。tests/ の中だけが読む）で試験を選ぶ。
- 表示の試験（T4b）は `tests/display/*.c` に `void drv_i915_test_display_<name>(struct i915_device *device)` を定義し、
  runner の表から呼ばれる。名前: `lcdb`, `lcdc`, `lcdd`, `lcdg`, `lcdo`, `lcdr`, `hdmib`, `dual`, `dual_share`, `n1`,
  `aux`, `hdmi_edid`, `hdmi_hpd`, `ktest`（表示の ktest 群をまとめて走らせる）。
- 実機を使う試験は同時に一つだけ。`flock /tmp/i915-hw.lock plan/ws031/tests/vkloop-hw.sh ...` で排他する。

## 2. 分担

| 担当 | 旧 | 新 | 受入 |
| --- | --- | --- | --- |
| T1 host render/legacy | `plan/ws031/tests/i915-vk-{res,resdispatch,pipe,cmdbuf,sync}-test.c`、`i915-vk-e127-stubs.inc`、`run-vk-host-tests.sh`、`run-vk-analyzer.sh`、`plan/ws029/tests/*`（git の履歴、2026-10-08 の WS の完了で削除） | 新 render / session / command / ppgtt に向けた fixture。旧 module（res/pipe/cmdbuf/sync/wsi）専用の検査は、同じ意味の新関数（objects/memory/command/fence）へ移すか、廃止理由を書く。ws029: stream parse と session PPGTT は新ファイルへ、uncore/gtt/irq/lrc/backend は廃止（本番から到達しないコードの試験） | `run-vk-host-tests.sh` 既定一覧 PASS（通常＋ASan/UBSan）、analyzer PASS |
| T2 contract | `parity/tests/*`（mock mmio/dma/pci、contract 6 本、run.sh） | `tests/contracts/`（mock と contract、run.sh） | 全 contract PASS |
| T3 host display | `run-dp-host-test.sh`、`run-lcd-host-test.sh`、`run-lcd-modeset-host-test.sh`、`run-opregion-host-test.sh`、`run-native-decide-host-test.sh` と fixture、`lcd_fake_hw.c`、`dp_fake_hw.c` | `tests/display/`（fake HW）＋ `plan/ws031/tests/` の script を新ファイルへ | 5 script PASS |
| T4a kernel 実行試験 | `parity/ktest.c`、`eu_test.c`、`runner.c` の試験部、`selftest.c`（本番から到達しない legacy HW 試験は廃止）、`draw_fixture.h`、`tex_fixture*_gen.inc`、`vk/vkref-generated.inc` | `tests/execution/`（runner、ktest、eu_test）、`tests/fixtures/`、`device.c` の weak 呼出し 1 行 | 実機: ktest PASS 件数、EU test PASS |
| T4b kernel 表示試験 | `parity_lcd_kernel.c` のシナリオ、`lcd_show_ktest.c`、`lcdg_ktest.c`、`lcd_modeset_ktest.c`、`scanout_ktest.c`、`opregion_ktest.c`、`opregion_fwtest.c`、`hpd_ktest.c`、`parity_hpd_test.c`、`edp_ktest.c`、`edp_sync_ktest.c`、`lcd_hw_check.c`、`lcd_pattern.c` の試験部 | `tests/display/` | 実機: 表示 ktest PASS、LCD-B シナリオ PASS |
