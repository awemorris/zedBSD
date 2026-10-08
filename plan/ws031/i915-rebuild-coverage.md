# WS031 i915 再構築 — 旧ツリー被覆監査（レビュー用対応地図）

作成: 2026-09-22 03:24、centris `~/zedBSD-gpu` HEAD `7e7ff337`＋作業ツリー（未 commit）。読むだけの監査で、ソースは変更していない。
用途は二つ: (1) `src/drivers/gpu/i915-old/` の各ファイル・各関数が新ツリーのどこへ行ったかの対応地図（専門家レビューで旧と新を並べて読むため）、
(2) レビュー後に旧ツリーを削除する前に残っている作業の一覧。ユーザー決定により旧ツリーはレビューが終わるまで残す。

入力: [保全台帳](i915-refactoring-assets.md)（378 ファイル）、[関数台帳](i915-refactoring-functions.md) §E（2,988 行）、[規則](i915-rebuild-rules.md)、
[計画](i915-rebuild-plan.md)、[S1](i915-rebuild-s1.md)／[S1 報告](i915-rebuild-s1-reports.md)、[S3](i915-rebuild-s3.md)／[S3 報告](i915-rebuild-s3-reports.md)、
[S4](i915-rebuild-s4.md)／[S4 報告](i915-rebuild-s4-reports.md)、[S5](i915-rebuild-s5.md)、[レビュー §4](i915-refactoring-review.md)。

## 0. 方法と判定の意味

- 新ツリーの関数定義は `src/drivers/gpu/i915/`（`tests/` を除く＝本番）と、試験の置き場 `src/drivers/gpu/i915/tests/`、`plan/ws031/tests/`、`plan/ws029/tests/`（git の履歴、2026-10-08 の WS の完了で削除） から、字句解析（行頭の宣言子＋対応する括弧の後の `{`）で抽出した。
- 旧関数ごとに次の順で探した: 台帳の提案名（H 欄）→ 規則 §2／§6 と S3 §1・S4 §6 の機械改名（`parity_`/`osdep_`/`parity_intel_`/`intel_`→`drv_i915_`/`i915_`、環境接頭辞 `lcd`/`dp`/`hpd`/`vbt`/`opregion`/`wm`/`takeover` など）→ 接頭辞を除いた語幹の一致 → 同じ旧ファイルの他の関数が行った新ファイルの中での部分一致・語の一致 → 旧本体の特徴的な文字列リテラル。機械照合で残った本番側の約 180 件は一件ずつ grep と本体比較で解決した（注に inline 化・置換の根拠を書いた）。
- 判定: **found**＝本番ツリーに移植先がある（`file:function`、inline 化は括弧で注記）。**test-moved**＝試験ツリーに既にある。**test**＝試験専用で S5 担当（T1〜T4b）がまだ移していない（未移動を欠落とは判定しない）。**retired**＝計画・報告に廃止理由の記録がある。**unported**＝旧 vk module のうち S3a が「vkdemo から到達しない」と報告し移植しなかったもの（正式な廃止記録は未作成）。**MISSING**＝行き先を確認できないもの。
- 「match by tokens/substr/stem/string」は機械照合の根拠を示す。string は旧本体の文字列が新ファイルにあることだけを示し、関数の一対一対応は保証しない。レビューで疑わしければ該当行を本体比較すること。
- 試験の移設は他の担当が並行して進めている。本書の試験側の状態は作成時点のもの（`src/drivers/gpu/i915/tests/` には `render/`、`execution/`、`contracts/`、`display/` の一部があった。§2 の test 行に置き場の有無を書いた）。

## 1. 集計

| 判定 | 関数数 |
| --- | ---: |
| found | 2403 |
| test-moved | 115 |
| test | 254 |
| retired | 108 |
| unported | 103 |
| **MISSING** | 5 |
| 計 | 2988 |

MISSING 5 件の内訳と判断は §3.1。いずれも本番（resident）構成から到達しない旧 legacy 経路で、本番に効くものは **0 件**。ファイル単位の MISSING は §2（`port_lcd_calc.manifest.json`）。

## 2. 旧ファイル別（378 本）

「台帳の受入先」は [保全台帳](i915-refactoring-assets.md) の予定。「実際の行き先」は本監査で確認した場所。

| 旧ファイル（`i915-old/` 相対） | 判定 | 実際の行き先・根拠 | 台帳の受入先 |
| --- | --- | --- | --- |
| `draw_fixture.h` | test (S5 T4a) | destination `tests/fixtures/draw-fixture.h` | `tests/fixtures/draw-fixture.h` |
| `engine.c` | retired | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 | `engine.c`, `reset.c` |
| `gem.c` | moved | functions: found 7, retired 2. New homes: `memory.c` (7) | `memory.c` |
| `ggtt.c` | retired | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 | `ggtt.c` |
| `i915.c` | moved | functions: found 44. New homes: `command.c` (9), `resource.c` (9), `i915.c` (7), `session.c` (6), `job.c` (5), `reset.c` (5) | `command.c`, `device.c`, `engine.c`, `i915.c`, `job.c`, `reset.c`, `resource.c`, `session.c`, `ops.h`、`device-info.c`、capability profile |
| `internal.h` | moved | shared types/constants now in `i915.h`, `device.h`, `session.h`, `request-queue.h`, `command.c`, `job.c`. The 19 absent `I915_*` constants (e.g. `I915_BCS0_BASE`, `I915_CSB_ENTRIES`, `I915_FORCEWAKE_ALL`, `I915_RING_BYTES`) are used only by the retired legacy engine.c/uncore.c/ggtt.c/lrc.c/irq.c; `I915_REQUEST_MAX_DWORDS` only by the unreachable legacy request.c emit (§3.1) | `i915.h` / `device.h` / `session.h` / `memory.h` / `ggtt.h` / `ppgtt.h` / `context.h` / `request.h` |
| `irq.c` | retired | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 | `irq.c` |
| `linux/i915-commands.inc` | moved | definitions present in new tree: 70/70 (100%); mostly in `data/i915-commands.inc`, `request.c`, `tests/execution/eu-test.c` | `data/i915-commands.inc` |
| `linux/i915-ids.inc` | moved | definitions present in new tree: 7/7 (100%); mostly in `data/i915-ids.inc`, `i915.c`, `device.c` | `data/i915-ids.inc` |
| `linux/i915-lrc-offsets.inc` | moved | definitions present in new tree: 11/11 (100%); mostly in `data/i915-lrc-offsets.inc`, `data/i915-ids.inc`, `data/i915-mocs.inc` | `data/i915-lrc-offsets.inc` |
| `linux/i915-mocs.inc` | moved | definitions present in new tree: 37/37 (100%); mostly in `data/i915-mocs.inc`, `data/i915-gt-workarounds.inc`, `data/i915-gt-mocs-table.inc` | `data/i915-mocs.inc` |
| `linux/i915-regs.inc` | moved | definitions present in new tree: 173/173 (100%); mostly in `data/i915-regs.inc`, `irq.c`, `submit.c` | `data/i915-regs.inc` |
| `linux/i915-workarounds.inc` | moved | definitions present in new tree: 50/50 (100%); mostly in `data/i915-workarounds.inc`, `workarounds.c`, `data/i915-gt-workarounds.inc` | `data/i915-workarounds.inc` |
| `lrc.c` | retired | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 | `context.c` |
| `parity/backend.h` | moved (renamed) | `parity_mmio_priv`/`parity_pci_priv` became the context structs of `pci.h` (`struct i915_pci_context`) and `gt.h`/`device.c` | `device.h` / `mmio.h` / `memory.h` / `power.h` |
| `parity/backend_delayed.c` | moved | functions: found 10, test 1. New homes: `workqueue.c` (10) | `sync.c` |
| `parity/backend_delayed.h` | moved (renamed) | `workqueue.h`: `parity_kdelayed`/`parity_ktimerq` → `struct i915_delayed_work`/`i915_timer_queue`, `PARITY_KDELAYED_MAX` → `I915_TIMER_QUEUE_SLOTS` | `sync.h` |
| `parity/backend_dma.c` | moved | functions: found 2. New homes: `dma.c` (2) | `memory.c` |
| `parity/backend_mmio.c` | moved | functions: found 13. New homes: `mmio.c` (7), `runtime-pm.c` (6) | `mmio.c` |
| `parity/backend_pci.c` | moved | functions: found 10. New homes: `pci.c` (10) | `device.c` |
| `parity/backend_sync.c` | moved | functions: found 14, test 1. New homes: `workqueue.c` (10), `sync.c` (4) | `sync.c` |
| `parity/backend_sync.h` | moved (renamed) | `sync.h`/`workqueue.h`: `parity_kcompletion` → `struct i915_completion`, `parity_kwork(queue)` → `struct i915_work`/`i915_workqueue`, `PARITY_KWQ_DEPTH` → `I915_WORKQUEUE_DEPTH` | `sync.h` |
| `parity/bios.c` | moved | functions: found 17. New homes: `display/vbt.c` (17) | `display/vbt.c` |
| `parity/bios.h` | moved (test knobs → S5) | VBT/pin definitions in `display/internal.h`, `display/vbt.c`, `display/takeover.c`; the absent `PARITY_*_TEST` are test-build knobs deliberately not brought into production ([s4 §2.1](i915-rebuild-s4.md) L89; test build selects scenarios by `I915_TEST_SCENARIO`, [s5 §1](i915-rebuild-s5.md)) | `display/vbt.h` |
| `parity/cdclk.c` | moved | functions: found 29. New homes: `display/clock.c` (28), `device-info.c` (1) | `display/clock.c` |
| `parity/cdclk.h` | moved | definitions present in new tree: 5/5 (100%); mostly in `display/internal.h`, `display/clock.c`, `display/clock.h` | `display/clock.h` |
| `parity/combo_phy.c` | moved | functions: found 19. New homes: `display/phy.c` (18), `display/hotplug.c` (1) | `display/phy.c` |
| `parity/combo_phy.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `display/internal.h`, `display/phy.c` | `display/phy.h` |
| `parity/display_core.c` | moved | functions: found 17. New homes: `display/power.c` (15), `display/hotplug.c` (1), `reset.c` (1) | `display/power.c` |
| `parity/display_core.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `display/power.c`, `display/display.c`, `display/internal.h` | `display/power.h` |
| `parity/display_nogem.c` | moved | functions: found 46. New homes: `display/takeover.c` (40), `display/clock.c` (3), `display/pipe.c` (2), `display/hotplug.c` (1) | `display/takeover.c` |
| `parity/display_nogem.h` | moved | definitions present in new tree: 33/33 (100%); mostly in `display/internal.h`, `display/takeover.c`, `data/display-vbt-ref-types.inc` | `display/takeover.h` |
| `parity/display_state.c` | moved | functions: found 33. New homes: `display/watermark.c` (17), `display/state.c` (15), `display/hotplug.c` (1) | `display/state.c`, `display/watermark.c` |
| `parity/display_state.h` | moved | definitions present in new tree: 15/15 (100%); mostly in `display/internal.h`, `display/state.c`, `display/state.h` | `display/state.h` / `display/watermark.h` |
| `parity/dmc.c` | moved | functions: found 26. New homes: `display/dmc.c` (25), `mmio.c` (1) | `display/dmc.c` |
| `parity/dmc.h` | moved | definitions present in new tree: 5/5 (100%); mostly in `display/internal.h`, `display/dmc.c`, `display/dmc.h` | `display/dmc.h` |
| `parity/dp/dp_compat.h` | moved | functions: found 12. New homes: `display/dp-internal.h` (5), `display/modeset-internal.h` (3), `display/internal.h` (2), `display/dp.c` (1), `display/pipe.c` (1) | `display/dp-internal.h`（macroの意味を保存し明示loopへ置換） |
| `parity/dp/dp_fake_hw.c` | test (S5 T3) | destination present: `src/drivers/gpu/i915/tests/display/dp-fake-hw.c`; functions located in a test tree: 22/24 | `tests/display/dp-fake-hw.c` |
| `parity/dp/dp_fake_hw.h` | test (S5 T3) | destination present: `src/drivers/gpu/i915/tests/display/dp-fake-hw.h`; functions located in a test tree: 0/0 | `tests/display/dp-fake-hw.h` |
| `parity/dp/dp_fixture_latitude5330.h` | test (S5 T3) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/dp-fixture-latitude5330.h` |
| `parity/dp/dp_ref_types.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-dp-ref-types.inc`, `data/display-intel-pps.inc`, `display/panel.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/dp/drm_dp.h` | moved | definitions present in new tree: 1198/1198 (100%); mostly in `data/display-drm-dp.inc`, `data/display-mreg-drm-dp.inc`, `display/dp.c` | `data/display-drm-dp.inc` |
| `parity/dp/drm_dp_helper_port.c` | moved | functions: found 17. New homes: `display/dp-sink.c` (17) | `display/dp.c` |
| `parity/dp/drm_edid_port.c` | moved | functions: found 4. New homes: `display/edid-read.c` (4) | `display/edid.c` |
| `parity/dp/edp_ktest.c` | test (S5 T4b) | destination present: `src/drivers/gpu/i915/tests/display/edp-ktest.c`; functions located in a test tree: 4/4 | `tests/display/edp-ktest.c` |
| `parity/dp/edp_ktest.h` | test (S5 T4b) | destination present: `src/drivers/gpu/i915/tests/display/edp-ktest.h`; functions located in a test tree: 0/0 | `tests/display/edp-ktest.h` |
| `parity/dp/edp_sync_ktest.c` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/16 | `tests/display/edp-sync-ktest.c` |
| `parity/dp/intel_dp_aux.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-intel-dp-aux.inc` | `display/aux.h` |
| `parity/dp/intel_dp_aux_port.c` | moved | functions: found 13. New homes: `display/aux.c` (13) | `display/aux.c` |
| `parity/dp/intel_dp_aux_regs.h` | moved | definitions present in new tree: 52/52 (100%); mostly in `data/display-intel-dp-aux-regs.inc`, `display/aux.c` | `data/display-intel-dp-aux-regs.inc` |
| `parity/dp/intel_pps.h` | moved | definitions present in new tree: 2/2 (100%); mostly in `data/display-intel-pps.inc`, `display/panel.c`, `display/dp-internal.h` | `display/panel.h` |
| `parity/dp/intel_pps_port.c` | moved | functions: found 54. New homes: `display/panel.c` (54) | `display/panel.c` |
| `parity/dp/intel_pps_regs.h` | moved | definitions present in new tree: 51/51 (100%); mostly in `data/display-intel-pps-regs.inc`, `display/panel.c`, `display/dp-sink.c` | `data/display-intel-pps-regs.inc` |
| `parity/dp/parity_dp_aux_glue.inc` | moved | functions: found 1. New homes: `display/aux.c` (1) | `display/aux.c` |
| `parity/dp/parity_dp_kernel.c` | moved | functions: found 31, test 3. New homes: `display/dp-sink.c` (30), `display/hotplug.c` (1) | `display/dp.c` |
| `parity/dp/parity_dp_kernel.h` | moved | definitions present in new tree: 2/2 (100%); mostly in `display/dp-sink.h`, `display/dp-sink.c`, `display/internal.h` | `display/dp.h` |
| `parity/dp/parity_drm_dp_glue.inc` | moved | functions: found 1. New homes: `display/dp-sink.c` (1) | `display/dp.c` |
| `parity/dp/parity_drm_edid_glue.inc` | moved | functions: found 1. New homes: `display/edid-read.c` (1) | `display/edid.c` |
| `parity/dp/parity_edp.c` | moved | functions: found 26. New homes: `display/dp-sink.c` (26) | `display/dp.c` |
| `parity/dp/parity_edp.h` | moved | definitions present in new tree: 17/17 (100%); mostly in `display/internal.h`, `display/dp-sink.c`, `display/dp-internal.h` | `display/dp.h` |
| `parity/dram_bw.c` | moved | functions: found 6. New homes: `display/watermark.c` (6) | `display/watermark.c` |
| `parity/dram_bw.h` | moved | definitions present in new tree: 8/8 (100%); mostly in `display/internal.h`, `display/watermark.c`, `tests/execution/ktest-sync.c` | `display/watermark.h` |
| `parity/driver_probe.c` | moved | functions: found 27. New homes: `display/hotplug.c` (19), `display/display.c` (4), `display/power.c` (4) | `device.c`, `display/display.c`, `display/hotplug.c`, `display/power.c`, `display/watermark.c` |
| `parity/driver_probe.h` | moved | definitions present in new tree: 25/25 (100%); mostly in `display/internal.h`, `display/hotplug.c`, `data/display-hpd-pin-enum.inc` | `device.h` / `display/display.h` / `display/hotplug.h` / `display/power.h` / `display/watermark.h` |
| `parity/drm_device.c` | moved | functions: found 5, test 2. New homes: `display/display.c` (5) | `device.c` |
| `parity/drm_device.h` | moved | definitions present in new tree: 5/5 (100%); mostly in `display/internal.h`, `display/display.c`, `display/display.h` | `device.h` |
| `parity/eu_test.c` | test (S5 T4a) | destination present: `src/drivers/gpu/i915/tests/execution/eu-test.c`; functions located in a test tree: 12/45 | `tests/execution/eu-test.c` |
| `parity/eu_test.h` | test (S5 T4a) | destination present: `src/drivers/gpu/i915/tests/execution/eu-test.h`; functions located in a test tree: 0/0 | `tests/fixtures/eu-test.h` |
| `parity/firmware_adlp_dmc.c` | moved | `data/firmware/adlp-dmc.c`; byte array identical (79088 bytes) | `data/firmware/firmware-adlp-dmc.c` |
| `parity/firmware_tgl_dmc.c` | moved | `data/firmware/tgl-dmc.c`; byte array identical (19760 bytes) | `data/firmware/firmware-tgl-dmc.c` |
| `parity/firmware_vbt_dell_latitude_5320.c` | moved | `data/firmware/vbt-dell-latitude-5320.c`; byte array identical (8704 bytes) | `data/firmware/firmware-vbt-dell-latitude-5320.c` |
| `parity/firmware_vbt_dell_latitude_5330.c` | moved | `data/firmware/vbt-dell-latitude-5330.c`; byte array identical (8704 bytes) | `data/firmware/firmware-vbt-dell-latitude-5330.c` |
| `parity/gt_defaults.c` | moved | functions: found 9. New homes: `defaults.c` (7), `display/dp-sink.c` (1), `engine.c` (1) | `context.c` |
| `parity/gt_defaults.h` | moved | definitions present in new tree: 5/5 (100%); mostly in `defaults.h`, `defaults.c`, `gt.h` | `context.h` |
| `parity/gt_engine.c` | moved | functions: found 12. New homes: `submit.c` (7), `engine.c` (5) | `engine.c` |
| `parity/gt_engine.h` | moved | definitions present in new tree: 32/35 (91%); mostly in `data/i915-regs.inc`, `submit.c`, `engine.c`; absent e.g. `PARITY_GEN11_CSB_PTR_MASK`, `PARITY_RING_HEAD_REG`, `PARITY_RING_TAIL_REG` | `engine.h` |
| `parity/gt_fw_ranges.inc` | moved | `data/forcewake-ranges.inc` (mmio.c:54); all 86 numeric tokens identical. Generator `handover/tools/gen_fw_ranges.py` still writes the old path | `data/gt-fw-ranges.inc` |
| `parity/gt_init.h` | moved | definitions present in new tree: 10/11 (91%); mostly in `workarounds.h`, `workarounds.c`, `verify-workarounds.c`; absent e.g. `PARITY_WL_MAX` | `device.h` / `power.h` / `workarounds.h` / `ppgtt.h` |
| `parity/gt_init_base.c` | moved | functions: found 23, retired 1. New homes: `workarounds.c` (17), `gt-power.c` (6) | `device.c`, `power.c`, `ppgtt.c`, `workarounds.c` |
| `parity/gt_lrc.c` | moved | functions: found 25, test 3. New homes: `context.c` (25) | `context.c` |
| `parity/gt_lrc.h` | moved | definitions present in new tree: 61/65 (94%); mostly in `context.c`, `data/i915-regs.inc`, `data/i915-execution.inc`; absent e.g. `PARITY_GEN12_INDIRECT_CTX_OFFSET_DEFAULT`, `PARITY_LRC_STOP_RING`, `PARITY_RING_CMD_BUF_CCTL_REG`, `PARITY_RING_CTX_TIMESTAMP_REG` | `context.h` |
| `parity/gt_lrc_offsets.inc` | moved (merged) | production context.c now uses `gen12_rcs_offsets`/`gen12_xcs_offsets` of `data/i915-lrc-offsets.inc` (the former legacy linux/ transcription). Compiled and compared by this audit: rcs 141 bytes and xcs 38 bytes are byte-identical to the old parity tables. Generator `handover/tools/gen_lrc_offsets.py` still writes the old path | `data/gt-lrc-offsets.inc` |
| `parity/gt_mem.c` | moved | functions: found 37, test 2. New homes: `ggtt.c` (16), `ppgtt.c` (15), `memory.c` (6) | `ggtt.c`, `memory.c`, `ppgtt.c` |
| `parity/gt_mem.h` | moved | definitions present in new tree: 21/22 (95%); mostly in `ppgtt.c`, `ppgtt.h`, `memory.h`; absent e.g. `parity_gt_ppgtt_walk` | `ggtt.h` / `memory.h` / `ppgtt.h` |
| `parity/gt_migrate.c` | moved | functions: found 5. New homes: `migrate.c` (4), `display/dp-sink.c` (1) | `engine.c` |
| `parity/gt_migrate.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `migrate.h`, `migrate.c`, `gt.h` | `engine.h` |
| `parity/gt_mmio.c` | moved | functions: found 13. New homes: `device-info.c` (13) | `mmio.c` |
| `parity/gt_mmio.h` | moved | definitions present in new tree: 16/17 (94%); mostly in `device-info.h`, `device-info.c`, `data/engine-table.inc`; absent e.g. `parity_gt_mmio` | `mmio.h` |
| `parity/gt_request.c` | moved | functions: found 12. New homes: `request.c` (12) | `request.c` |
| `parity/gt_request.h` | moved | definitions present in new tree: 39/39 (100%); mostly in `request.c`, `data/i915-commands.inc`, `tests/execution/eu-test.c` | `request.h` |
| `parity/gt_resume.c` | moved | functions: found 4. New homes: `engine.c` (4) | `engine.c` |
| `parity/gt_resume.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `engine.c`, `migrate.h`, `defaults.c` | `engine.h` |
| `parity/gt_submit.c` | moved | functions: found 10. New homes: `submit.c` (9), `request.c` (1) | `request.c` |
| `parity/gt_submit.h` | moved | `submit.h`/`submit.c`, `data/i915-execution.inc` (Linux names, [s1-reports](i915-rebuild-s1-reports.md) L65) | `request.h` |
| `parity/gt_tlb.c` | moved | functions: found 2. New homes: `tlb.c` (2) | `ppgtt.c` |
| `parity/gt_tlb.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `tlb.h`, `tlb.c`, `tests/display/lcdg-ktest.c` | `ppgtt.h` |
| `parity/gt_verify_wa.c` | moved | functions: found 11. New homes: `verify-workarounds.c` (10), `display/dp-sink.c` (1) | `workarounds.c` |
| `parity/gt_verify_wa.h` | moved | definitions present in new tree: 9/9 (100%); mostly in `verify-workarounds.c`, `verify-workarounds.h`, `data/i915-commands.inc` | `workarounds.h` |
| `parity/gt_wa_adlp.c` | moved | functions: found 8. New homes: `workarounds.c` (8) | `workarounds.c` |
| `parity/irq.c` | moved | functions: found 46. New homes: `display/interrupts.c` (25), `irq.c` (21) | `irq.c` |
| `parity/irq.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `display/internal.h`, `display/interrupts.c`, `irq.h` | `irq.h` |
| `parity/ktest.c` | test (S5 T4a) | destination present: `src/drivers/gpu/i915/tests/execution/ktest.c`; functions located in a test tree: 10/56 | `tests/execution/ktest.c` |
| `parity/ktest.h` | test (S5 T4a) | destination present: `src/drivers/gpu/i915/tests/execution/ktest.h`; functions located in a test tree: 0/0 | `tests/fixtures/ktest.h` |
| `parity/lcd/drm_connector_status_port.c` | moved | functions: found 1. New homes: `display/hotplug.c` (1) | `display/hotplug.c` |
| `parity/lcd/drm_dp_bw_port.c` | moved | functions: found 2. New homes: `display/dp.c` (2) | `display/dp.c` |
| `parity/lcd/drm_dp_link_port.c` | moved | functions: found 24. New homes: `display/dp.c` (24) | `display/dp.c` |
| `parity/lcd/drm_edid_mode_port.c` | moved | functions: found 2. New homes: `display/edid.c` (2) | `display/edid.c` |
| `parity/lcd/drm_modes_hv_port.c` | moved | functions: found 3. New homes: `display/edid.c` (3) | `display/edid.c` |
| `parity/lcd/drm_modes_port.c` | moved | functions: found 1. New homes: `display/edid.c` (1) | `display/edid.c` |
| `parity/lcd/drm_probe_detect_port.c` | moved | functions: found 2. New homes: `display/hotplug.c` (2) | `display/hotplug.c` |
| `parity/lcd/edid_ref_types.h` | moved | definitions present in new tree: 89/89 (100%); mostly in `data/display-edid-ref-types.inc`, `display/edid.c`, `display/state.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/hpd_compat.h` | moved | functions: found 8. New homes: `display/hotplug-internal.h` (8) | `display/hotplug-internal.h`（macroの意味を保存し明示loopへ置換） |
| `parity/lcd/hpd_drm_connector_status.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `data/display-hpd-drm-connector-status.inc`, `display/hotplug.c`, `tests/display/hdmi-hotplug.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/hpd_for_each_pin.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-hpd-for-each-pin.inc` | `data/display-hpd-for-each-pin.inc` |
| `parity/lcd/hpd_hotplug_state.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-hpd-hotplug-state.inc`, `tests/display/hpd-ktest.c`, `display/internal.h` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/hpd_hotplug_types.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-hpd-hotplug-types.inc`, `display/hotplug-internal.h`, `display/hotplug.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/hpd_ktest.c` | test (S5 T4b) | destination present: `src/drivers/gpu/i915/tests/display/hpd-ktest.c`; functions located in a test tree: 4/5 | `tests/display/hpd-ktest.c` |
| `parity/lcd/hpd_mreg_drm_dp.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-drm-dp.inc`, `data/display-hpd-mreg-drm-dp.inc`, `display/hotplug.c` | `data/display-hpd-mreg-drm-dp.inc` |
| `parity/lcd/hpd_mreg_gmbus.h` | moved | definitions present in new tree: 29/29 (100%); mostly in `data/display-hpd-mreg-gmbus.inc`, `display/gmbus.c`, `display/hotplug.c` | `data/display-hpd-mreg-gmbus.inc` |
| `parity/lcd/hpd_mreg_gmbus_pins.h` | moved | definitions present in new tree: 2/2 (100%); mostly in `data/display-hpd-mreg-gmbus-pins.inc`, `data/display-vbt-tables.inc`, `display/vbt.h` | `data/display-hpd-mreg-gmbus-pins.inc` |
| `parity/lcd/hpd_mreg_i915_reg.h` | moved | definitions present in new tree: 12/12 (100%); mostly in `data/display-hpd-mreg-i915-reg.inc`, `display/hotplug.c`, `tests/display/hdmi-hotplug.c` | `data/display-hpd-mreg-i915-reg.inc` |
| `parity/lcd/hpd_pin_enum.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-hpd-pin-enum.inc`, `data/display-hpd-mreg-i915-reg.inc`, `display/internal.h` | `data/display-hpd-pin-enum.inc` |
| `parity/lcd/intel_acpi_port.c` | moved | functions: found 2. New homes: `display/opregion.c` (2) | `display/panel.c` |
| `parity/lcd/intel_atomic_plane_port.c` | moved | functions: found 5. New homes: `display/plane.c` (5) | `display/plane.c` |
| `parity/lcd/intel_backlight_port.c` | moved | functions: found 30. New homes: `display/panel-backlight.c` (30) | `display/panel.c` |
| `parity/lcd/intel_bw_port.c` | moved | functions: found 4. New homes: `display/watermark.c` (4) | `display/watermark.c` |
| `parity/lcd/intel_cdclk_port.c` | moved | functions: found 7. New homes: `display/clock.c` (7) | `display/clock.c` |
| `parity/lcd/intel_color_port.c` | moved | functions: found 10. New homes: `display/color.c` (10) | `display/color.c` |
| `parity/lcd/intel_combo_phy_port.c` | moved | functions: found 1. New homes: `display/phy.c` (1) | `display/phy.c` |
| `parity/lcd/intel_crtc_port.c` | moved | functions: found 9. New homes: `display/pipe.c` (9) | `display/pipe.c` |
| `parity/lcd/intel_ddi_buf_trans_port.c` | moved | functions: found 10. New homes: `display/phy.c` (10) | `display/phy.c` |
| `parity/lcd/intel_ddi_hotplug_port.c` | moved | functions: found 2. New homes: `display/hotplug.c` (2) | `display/hotplug.c` |
| `parity/lcd/intel_ddi_port.c` | moved | functions: found 77. New homes: `display/ddi.c` (75), `display/takeover.c` (2) | `display/ddi.c` |
| `parity/lcd/intel_display_port.c` | moved | functions: found 56. New homes: `display/pipe.c` (54), `display/takeover.c` (2) | `display/pipe.c` |
| `parity/lcd/intel_display_power_set_port.c` | moved | functions: found 2. New homes: `display/power.c` (2) | `display/power.c` |
| `parity/lcd/intel_dmc_port.c` | moved | functions: found 3. New homes: `display/dmc.c` (3) | `display/dmc.c` |
| `parity/lcd/intel_dp_connected_port.c` | moved | functions: found 1. New homes: `display/modeset-internal.h` (1) | `display/dp.c` |
| `parity/lcd/intel_dp_link_training_port.c` | moved | functions: found 45. New homes: `display/dp.c` (45) | `display/dp.c` |
| `parity/lcd/intel_dpll_port.c` | moved | functions: found 35. New homes: `display/clock.c` (35) | `display/clock.c` |
| `parity/lcd/intel_gmbus_port.c` | moved | functions: found 17. New homes: `display/gmbus.c` (17) | `display/gmbus.c` |
| `parity/lcd/intel_hdmi_detect_port.c` | moved | functions: found 3. New homes: `display/hdmi.c` (3) | `display/hdmi.c` |
| `parity/lcd/intel_hdmi_mode_port.c` | moved | functions: found 4. New homes: `display/hdmi-mode.c` (4) | `display/hdmi.c` |
| `parity/lcd/intel_hotplug_irq_port.c` | moved | functions: found 4. New homes: `display/hotplug.c` (4) | `display/hotplug.c` |
| `parity/lcd/intel_hotplug_port.c` | moved | functions: found 12. New homes: `display/hotplug.c` (12) | `display/hotplug.c` |
| `parity/lcd/intel_link_port.c` | moved | functions: found 20. New homes: `display/dp.c` (20) | `display/dp.c` |
| `parity/lcd/intel_modeset_setup_port.c` | moved | functions: found 25. New homes: `display/takeover.c` (25) | `display/takeover.c` |
| `parity/lcd/intel_opregion_port.c` | moved | functions: found 28. New homes: `display/opregion.c` (28) | `display/opregion.c` |
| `parity/lcd/intel_vblank_port.c` | moved | functions: found 9. New homes: `display/vblank.c` (9) | `display/vblank.c` |
| `parity/lcd/intel_vrr_port.c` | moved | functions: found 2. New homes: `display/pipe.c` (2) | `display/pipe.c` |
| `parity/lcd/intel_wm_port.c` | moved | functions: found 1. New homes: `display/watermark.c` (1) | `display/watermark.c` |
| `parity/lcd/lcd_buf_trans_types.h` | moved | definitions present in new tree: 8/8 (100%); mostly in `data/display-buf-trans-types.inc`, `data/display-phy-buf-trans.inc`, `display/ddi.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_compat.h` | moved | functions: found 14. New homes: `display/modeset-internal.h` (14) | `display/modeset-internal.h`（macroの意味を保存し明示loopへ置換） |
| `parity/lcd/lcd_dbuf_slice_enum.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-dbuf-slice-enum.inc`, `data/display-wm-dbuf-slices.inc`, `display/watermark.c` | `data/display-lcd-dbuf-slice-enum.inc` |
| `parity/lcd/lcd_dbuf_types.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-dbuf-types.inc`, `display/watermark.h`, `display/watermark-internal.h` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_ddi_regs.h` | moved | definitions present in new tree: 230/230 (100%); mostly in `data/display-ddi-regs.inc`, `display/ddi.c`, `display/pipe.c` | `data/display-lcd-ddi-regs.inc` |
| `parity/lcd/lcd_ddi_types.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `data/display-ddi-types.inc`, `display/modeset-internal.h`, `display/internal.h` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_dp_compat.h` | moved | functions: found 5. New homes: `display/modeset-internal.h` (5) | `display/dp.c` |
| `parity/lcd/lcd_dp_helper_inlines.h` | moved | functions: found 3. New homes: `data/display-dp-helper-inlines.inc` (3) | `display/dp.c` |
| `parity/lcd/lcd_dp_msa.h` | moved | definitions present in new tree: 36/36 (100%); mostly in `data/display-drm-dp.inc`, `data/display-dp-msa.inc`, `display/ddi.c` | `data/display-lcd-dp-msa.inc` |
| `parity/lcd/lcd_dp_phy_enum.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-drm-dp.inc`, `data/display-dp-phy-enum.inc`, `display/dp.h` | `data/display-lcd-dp-phy-enum.inc` |
| `parity/lcd/lcd_dpll_id_enum.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-dpll-id-enum.inc`, `display/internal.h`, `display/clock.c` | `data/display-lcd-dpll-id-enum.inc` |
| `parity/lcd/lcd_drm_colorspace.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-drm-colorspace.inc`, `display/modeset-internal.h` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_drm_fourcc.h` | moved | functions: found 1. New homes: `data/display-drm-fourcc.inc` (1) | `display/state.c` |
| `parity/lcd/lcd_drm_plane_defs.h` | moved | functions: found 1. New homes: `data/display-drm-plane-defs.inc` (1) | `display/plane.c` |
| `parity/lcd/lcd_fake_hw.c` | test (S5 T3) | destination not yet present (as of this audit); functions located in a test tree: 0/36 | `tests/display/lcd-fake-hw.c` |
| `parity/lcd/lcd_fake_hw.h` | test (S5 T3) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/lcd-fake-hw.h` |
| `parity/lcd/lcd_flip_compat.h` | moved | definitions present in new tree: 51/52 (98%); mostly in `display/modeset-internal.h`, `display/pipe.c`, `display/vblank.c`; absent e.g. `FLIP_OPS` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_hw_check.c` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 1/1 | `tests/display/scanout-hw-check.c` |
| `parity/lcd/lcd_hw_check.h` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/scanout-hw-check.h` |
| `parity/lcd/lcd_i915_colorkey.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `data/display-i915-colorkey.inc`, `display/plane.c`, `display/modeset-internal.h` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_i915_fixed.h` | moved | functions: found 15. New homes: `data/display-i915-fixed.inc` (15) | `display/internal.h` |
| `parity/lcd/lcd_link_training_inlines.h` | moved | functions: found 1. New homes: `data/display-link-training-inlines.inc` (1) | `display/dp.c` |
| `parity/lcd/lcd_modeset_compat.h` | moved | functions: found 2. New homes: `display/modeset-internal.h` (2) | `display/modeset-internal.h`（macroの意味を保存し明示loopへ置換） |
| `parity/lcd/lcd_modeset_ktest.c` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 2/3 | `tests/display/lcd-modeset-ktest.c` |
| `parity/lcd/lcd_modeset_ktest.h` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/lcd-modeset-ktest.h` |
| `parity/lcd/lcd_mreg_backlight.h` | moved | definitions present in new tree: 11/11 (100%); mostly in `data/display-mreg-backlight.inc`, `display/panel-backlight.c`, `tests/display/lcd-run.c` | `data/display-lcd-mreg-backlight.inc` |
| `parity/lcd/lcd_mreg_color.h` | moved | definitions present in new tree: 17/17 (100%); mostly in `data/display-mreg-color.inc`, `display/color.c`, `display/takeover-internal.h` | `data/display-lcd-mreg-color.inc` |
| `parity/lcd/lcd_mreg_combo_phy.h` | moved | definitions present in new tree: 59/59 (100%); mostly in `data/display-mreg-combo-phy.inc`, `display/ddi.c`, `display/phy.c` | `data/display-lcd-mreg-combo-phy.inc` |
| `parity/lcd/lcd_mreg_cx0.h` | moved | definitions present in new tree: 7/7 (100%); mostly in `data/display-mreg-cx0.inc`, `display/ddi.c` | `data/display-lcd-mreg-cx0.inc` |
| `parity/lcd/lcd_mreg_display.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `data/display-mreg-display.inc`, `display/watermark.c`, `display/takeover.c` | `data/display-lcd-mreg-display.inc` |
| `parity/lcd/lcd_mreg_display_device.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `data/display-mreg-display-device.inc`, `display/watermark.c`, `display/takeover.c` | `data/display-lcd-mreg-display-device.inc` |
| `parity/lcd/lcd_mreg_display_reg_defs.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `data/display-mreg-display-reg-defs.inc`, `data/display-mreg-wm.inc`, `display/modeset-internal.h` | `data/display-lcd-mreg-display-reg-defs.inc` |
| `parity/lcd/lcd_mreg_display_types.h` | moved | definitions present in new tree: 2/2 (100%); mostly in `data/display-mreg-display-types.inc`, `display/vblank.c`, `display/pipe.c` | `data/display-lcd-mreg-display-types.inc` |
| `parity/lcd/lcd_mreg_dmc.h` | moved | definitions present in new tree: 6/6 (100%); mostly in `data/display-mreg-dmc.inc`, `display/dmc.c`, `display/takeover.c` | `data/display-lcd-mreg-dmc.inc` |
| `parity/lcd/lcd_mreg_dmc_c.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-mreg-dmc-c.inc`, `display/dmc.c` | `data/display-lcd-mreg-dmc-c.inc` |
| `parity/lcd/lcd_mreg_drm_dp.h` | moved | definitions present in new tree: 102/102 (100%); mostly in `data/display-mreg-drm-dp.inc`, `data/display-drm-dp.inc`, `display/dp.c` | `data/display-lcd-mreg-drm-dp.inc` |
| `parity/lcd/lcd_mreg_hdmi_dip.h` | moved | definitions present in new tree: 7/7 (100%); mostly in `data/display-mreg-i915-reg.inc`, `data/display-mreg-hdmi-dip.inc`, `display/dp.c` | `data/display-lcd-mreg-hdmi-dip.inc` |
| `parity/lcd/lcd_mreg_i915_reg.h` | moved | definitions present in new tree: 168/168 (100%); mostly in `data/display-mreg-i915-reg.inc`, `display/clock.c`, `display/pipe.c` | `data/display-lcd-mreg-i915-reg.inc` |
| `parity/lcd/lcd_mreg_link_training.h` | moved | definitions present in new tree: 14/14 (100%); mostly in `data/display-mreg-link-training.inc`, `display/dp.c` | `data/display-lcd-mreg-link-training.inc` |
| `parity/lcd/lcd_mreg_power.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `data/display-mreg-power.inc`, `display/pipe.c`, `display/watermark.c` | `data/display-lcd-mreg-power.inc` |
| `parity/lcd/lcd_mreg_reg_defs.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-intel-dp-aux-regs.inc`, `data/display-mreg-i915-reg.inc`, `data/display-mreg-reg-defs.inc` | `data/display-lcd-mreg-reg-defs.inc` |
| `parity/lcd/lcd_mreg_vdsc.h` | moved | definitions present in new tree: 9/9 (100%); mostly in `data/display-mreg-vdsc.inc`, `display/ddi.c` | `data/display-lcd-mreg-vdsc.inc` |
| `parity/lcd/lcd_mreg_wm.h` | moved | definitions present in new tree: 92/92 (100%); mostly in `data/display-mreg-wm.inc`, `display/watermark.c`, `display/diagnostics.c` | `data/display-lcd-mreg-wm.inc` |
| `parity/lcd/lcd_pattern.c` | moved | functions: found 3, test 2. New homes: `display/diagnostics.c` (3) | `tests/display/lcd-pattern.c` |
| `parity/lcd/lcd_pattern.h` | moved (declarations) | declarations only (3 prototypes); the functions are mapped in §3 | `tests/display/lcd-pattern.h` |
| `parity/lcd/lcd_pch_enum.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-pch-enum.inc`, `data/display-vbt-ref-types.inc`, `display/display.c` | `data/display-lcd-pch-enum.inc` |
| `parity/lcd/lcd_plane_compat.h` | moved | definitions present in new tree: 27/27 (100%); mostly in `display/modeset-internal.h`, `display/plane.c`, `display/watermark.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_plane_regs.h` | moved | definitions present in new tree: 272/272 (100%); mostly in `data/display-plane-regs.inc`, `display/plane.c`, `tests/display/host-lcd-test.c` | `data/display-lcd-plane-regs.inc` |
| `parity/lcd/lcd_plane_types.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-i915-colorkey.inc`, `data/display-plane-types.inc`, `display/state.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_power_domain_enum.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `data/display-power-domain-enum.inc`, `display/power.c`, `display/pipe.h` | `data/display-lcd-power-domain-enum.inc` |
| `parity/lcd/lcd_power_domain_set_types.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `data/display-power-domain-set-types.inc`, `display/power.c`, `display/pipe.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_psr_selfetch_regs.h` | moved | definitions present in new tree: 21/21 (100%); mostly in `data/display-psr-selfetch-regs.inc`, `display/plane.c` | `data/display-lcd-psr-selfetch-regs.inc` |
| `parity/lcd/lcd_ref_inlines.h` | moved | functions: found 4. New homes: `data/display-ref-inlines.inc` (4) | `display/internal.h` |
| `parity/lcd/lcd_ref_types.h` | moved | definitions present in new tree: 15/15 (100%); mostly in `data/display-ref-types.inc`, `display/clock.c`, `display/pipe.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_seq_compat.h` | moved | definitions present in new tree: 83/85 (98%); mostly in `display/modeset-internal.h`, `display/ddi.c`, `display/pipe.c`; absent e.g. `SEQ_I915_CRTC_STATE`, `SEQ_I915_ENCODER` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcd_show_ktest.c` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 1/7 | `tests/display/lcd-show-ktest.c` |
| `parity/lcd/lcd_show_ktest.h` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/lcd-show-ktest.h` |
| `parity/lcd/lcd_trans_regs.h` | moved | definitions present in new tree: 101/101 (100%); mostly in `data/display-trans-regs.inc`, `display/pipe.c`, `display/takeover.c` | `data/display-lcd-trans-regs.inc` |
| `parity/lcd/lcd_wm_compat.h` | moved | functions: found 1. New homes: `display/watermark-internal.h` (1) | `display/watermark-internal.h`（macroの意味を保存し明示loopへ置換） |
| `parity/lcd/lcd_wm_ddb_types.h` | moved | functions: found 2. New homes: `data/display-wm-ddb-types.inc` (2) | `display/watermark.c` |
| `parity/lcd/lcd_wm_types.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `data/display-wm-types.inc`, `display/watermark.c`, `display/modeset-internal.h` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/lcdg_ktest.c` | test (S5 T4b) | destination present: `src/drivers/gpu/i915/tests/display/lcdg-ktest.c`; functions located in a test tree: 0/8 | `tests/display/lcdg-ktest.c` |
| `parity/lcd/lcdg_ktest.h` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/lcdg-ktest.h` |
| `parity/lcd/n1_compat.h` | moved | functions: found 4. New homes: `display/takeover-internal.h` (4) | `display/takeover-internal.h`（macroの意味を保存し明示loopへ置換） |
| `parity/lcd/opreg_pci_config.h` | moved | definitions present in new tree: 5/5 (100%); mostly in `data/display-opreg-pci-config.inc`, `display/opregion.c`, `tests/display/opregion-ktest.c` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/opreg_struct.h` | moved | definitions present in new tree: 2/2 (100%); mostly in `data/display-opreg-struct.inc`, `display/opregion.c`, `display/internal.h` | `display/internal.h（型/inlineを所有headerへ再配分）` |
| `parity/lcd/opregion_compat.h` | moved | functions: found 2. New homes: `display/opregion-internal.h` (2) | `display/opregion-internal.h`（macroの意味を保存し明示loopへ置換） |
| `parity/lcd/opregion_fwtest.c` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/4 | `tests/display/opregion-fwtest.c` |
| `parity/lcd/opregion_fwtest.h` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/opregion-fwtest.h` |
| `parity/lcd/opregion_ktest.c` | test (S5 T4b) | destination present: `src/drivers/gpu/i915/tests/display/opregion-ktest.c`; functions located in a test tree: 11/18 | `tests/display/opregion-ktest.c` |
| `parity/lcd/opregion_ktest.h` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/opregion-ktest.h` |
| `parity/lcd/parity_acpi_glue.inc` | moved (empty) | the file holds only a comment (no glue needed); intel_acpi_port.c went to `display/opregion.c` | `data/parity-acpi-glue.inc` |
| `parity/lcd/parity_atomic_plane_glue.inc` | moved | functions: found 1. New homes: `display/plane.c` (1) | `display/plane.c` |
| `parity/lcd/parity_backlight_glue.inc` | moved | functions: found 5. New homes: `display/panel-backlight.c` (5) | `display/panel.c` |
| `parity/lcd/parity_buf_trans_glue.inc` | moved | functions: found 1. New homes: `display/phy.c` (1) | `display/phy.c` |
| `parity/lcd/parity_bw_glue.inc` | moved | functions: found 2. New homes: `display/watermark.c` (2) | `display/watermark.c` |
| `parity/lcd/parity_cdclk_glue.inc` | moved | functions: found 1. New homes: `display/clock.c` (1) | `display/clock.c` |
| `parity/lcd/parity_color_glue.inc` | moved | functions: found 2. New homes: `display/color.c` (2) | `display/color.c` |
| `parity/lcd/parity_ddi_emit_glue.inc` | moved | functions: found 13. New homes: `display/ddi.c` (13) | `display/ddi.c` |
| `parity/lcd/parity_ddi_hotplug_glue.inc` | moved | functions: found 6. New homes: `display/hotplug.c` (4), `display/takeover.c` (2) | `display/hotplug.c` |
| `parity/lcd/parity_display_emit_glue.inc` | moved | functions: found 5. New homes: `display/pipe.c` (5) | `display/pipe.c` |
| `parity/lcd/parity_dpll_glue.inc` | moved | functions: found 10. New homes: `display/clock.c` (10) | `display/clock.c` |
| `parity/lcd/parity_edid_mode_glue.inc` | moved | functions: found 3. New homes: `display/edid.c` (3) | `display/edid.c` |
| `parity/lcd/parity_flip_glue.inc` | moved | functions: found 8. New homes: `display/vblank.c` (8) | `display/vblank.c` |
| `parity/lcd/parity_gmbus_glue.inc` | moved | functions: found 4. New homes: `display/gmbus.c` (4) | `display/gmbus.c` |
| `parity/lcd/parity_hdmi_detect_glue.inc` | moved | functions: found 10. New homes: `display/hdmi.c` (10) | `display/hdmi.c` |
| `parity/lcd/parity_hdmi_mode_glue.inc` | moved | functions: found 1. New homes: `display/hdmi-mode.c` (1) | `display/hdmi.c` |
| `parity/lcd/parity_hotplug.h` | moved | definitions present in new tree: 8/8 (100%); mostly in `display/internal.h`, `display/hotplug.c`, `display/hotplug-internal.h` | `display/hotplug.h` |
| `parity/lcd/parity_hotplug_glue.inc` | moved | functions: found 42. New homes: `display/hotplug.c` (39), `display/power.c` (2), `display/interrupts.c` (1) | `display/hotplug.c` |
| `parity/lcd/parity_hpd_test.c` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 2/3 | `tests/display/parity-hpd-test.c` |
| `parity/lcd/parity_lcd_calc.c` | moved | functions: found 17. New homes: `display/state.c` (17) | `display/state.c` |
| `parity/lcd/parity_lcd_calc.h` | moved | definitions present in new tree: 7/7 (100%); mostly in `display/internal.h`, `display/state.c`, `tests/display/host-lcd-test.c` | `display/state.h` |
| `parity/lcd/parity_lcd_kernel.c` | moved | functions: found 52, test 38, test-moved 8. New homes: `display/diagnostics.c` (15), `display/modeset.c` (14), `display/vblank.c` (7), `display/power.c` (4), `display/takeover.c` (4), `mmio.c` (3) | `display/aux.c`, `display/diagnostics.c`, `display/modeset.c`, `display/panel.c`, `display/power.c`, `display/present.c`, `display/scanout.c`, `display/state.c`, `display/takeover.c`, `display/vblank.c`, `display/watermark.c`, `irq.c`, `mmio.c`, `sync.c`, `tests/display/kernel-scenarios.c` |
| `parity/lcd/parity_lcd_kernel.h` | moved (test knobs → S5) | types in `display/internal.h` (`struct i915_lcd_kernel`); absent `PARITY_LCDB_*`, `*_WINDOW_MS` are test knobs ([s4 §2.1](i915-rebuild-s4.md) L89) for T4b `tests/display/lcd-run.c` | `display/modeset.h` / `display/scanout.h` / `display/state.h` / `tests/display/kernel-scenarios.h` |
| `parity/lcd/parity_lcd_modeset.c` | moved | functions: found 31. New homes: `display/modeset.c` (26), `display/panel-backlight.c` (3), `display/vblank.c` (2) | `display/modeset.c`, `display/panel.c`, `display/present.c`, `display/vblank.c`, `tests/display/modeset.c` |
| `parity/lcd/parity_lcd_modeset.h` | moved | definitions present in new tree: 12/12 (100%); mostly in `display/modeset.c`, `display/internal.h`, `tests/display/lcd-run.c` | `display/modeset.h` / `display/panel.h` / `display/present.h` / `display/vblank.h` / `tests/display/modeset.h` |
| `parity/lcd/parity_lcd_modeset_int.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `display/watermark.h`, `display/modeset.c`, `display/clock.c` | `display/internal.h` |
| `parity/lcd/parity_lcd_observe.c` | moved | functions: found 9. New homes: `display/diagnostics.c` (9) | `display/diagnostics.c` |
| `parity/lcd/parity_lcd_observe.h` | moved | definitions present in new tree: 5/5 (100%); mostly in `display/internal.h`, `display/diagnostics.c`, `display/diagnostics.h` | `display/diagnostics.h` |
| `parity/lcd/parity_lcd_ops.h` | moved | definitions present in new tree: 7/7 (100%); mostly in `display/internal.h`, `display/modeset-internal.h`, `display/modeset.c` | `display/internal.h` / `tests/fixtures/display-io.h（実HW操作とfakeの境界を照合）` |
| `parity/lcd/parity_lcd_regs.c` | moved | functions: found 4. New homes: `display/diagnostics.c` (4) | `display/diagnostics.c` |
| `parity/lcd/parity_lcd_show.c` | moved | functions: found 15. New homes: `display/modeset.c` (14), `display/diagnostics.c` (1) | `display/modeset.c` |
| `parity/lcd/parity_lcd_show.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `display/internal.h`, `tests/display/lcd-opregion.c`, `tests/display/lcd-run.c` | `display/modeset.h` |
| `parity/lcd/parity_lcd_trace.c` | moved | functions: found 34. New homes: `display/diagnostics.c` (33), `display/internal.h` (1) | `display/diagnostics.c` |
| `parity/lcd/parity_lcd_trace.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `display/internal.h`, `display/diagnostics.c`, `display/diagnostics.h` | `display/diagnostics.h` |
| `parity/lcd/parity_modeset_setup_glue.inc` | moved | functions: found 18. New homes: `display/takeover.c` (18) | `display/takeover.c` |
| `parity/lcd/parity_n1.h` | moved | definitions present in new tree: 1/1 (100%); mostly in `display/takeover.h`, `display/internal.h`, `display/takeover.c` | `display/takeover.h` |
| `parity/lcd/parity_opregion.h` | moved | definitions present in new tree: 10/10 (100%); mostly in `display/internal.h`, `display/opregion.c`, `tests/display/opregion-ktest.c` | `display/opregion.h` |
| `parity/lcd/parity_opregion_glue.inc` | moved | functions: found 36. New homes: `display/opregion.c` (36) | `display/opregion.c` |
| `parity/lcd/parity_plane_emit_glue.inc` | moved | functions: found 7. New homes: `display/plane.c` (7) | `display/plane.c` |
| `parity/lcd/parity_wm_glue.inc` | moved | functions: found 6. New homes: `display/watermark.c` (6) | `display/watermark.c` |
| `parity/lcd/port_lcd_calc.manifest.json` | MISSING (later kept in `intel/provenance/`, deleted 2026-09-22 by user decision) | provenance manifest of the old generated LCD port. [s4 §9-6](i915-rebuild-s4.md) L763 says the manifest stays in `data/provenance`; no `data/provenance` exists in the new tree. | `data/provenance/port_lcd_calc.manifest.json` |
| `parity/lcd/scanout.c` | moved | functions: found 8. New homes: `display/scanout.c` (8) | `display/scanout.c` |
| `parity/lcd/scanout.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `display/internal.h`, `tests/display/lcd-opregion.c`, `tests/display/aux.c` | `display/scanout.h` |
| `parity/lcd/scanout_ktest.c` | test (S5 T4b) | destination present: `src/drivers/gpu/i915/tests/display/scanout-ktest.c`; functions located in a test tree: 5/5 | `tests/display/scanout-ktest.c` |
| `parity/lcd/scanout_ktest.h` | test (S5 T4b) | destination not yet present (as of this audit); functions located in a test tree: 0/0 | `tests/display/scanout-ktest.h` |
| `parity/lcd/skl_plane_port.c` | moved | functions: found 30. New homes: `display/plane.c` (30) | `display/plane.c` |
| `parity/lcd/skl_watermark_port.c` | moved | functions: found 69. New homes: `display/watermark.c` (69) | `display/watermark.c` |
| `parity/legacy_shim.c` | moved | functions: found 24. New homes: `worker.c` (15), `display/present.c` (6), `display/scanout.c` (2), `display/display.c` (1) | `context.c`, `device.c`, `display/display.c`, `display/present.c`, `display/scanout.c`, `request.c`, `reset.c`, `sync.c` |
| `parity/legacy_shim.h` | retired (obsolete bridge) | the six `#define drv_i915_x parity_shim_x` redirects (PARITY_SHIM_REDIRECT) bridged the legacy ops to the parity shim; new ops call `worker.c` directly ([plan §5](i915-rebuild-plan.md) L65) | `context.h` / `request.h` / `reset.h（名前差替えを解消）` |
| `parity/native_decide.c` | moved | functions: found 1. New homes: `display/takeover.c` (1) | `display/takeover.c` |
| `parity/native_precheck.c` | moved | functions: found 10. New homes: `display/takeover.c` (10) | `display/takeover.c` |
| `parity/native_precheck.h` | moved | definitions present in new tree: 14/14 (100%); mostly in `display/internal.h`, `display/takeover.c`, `tests/display/host-native-decide-test.c` | `display/takeover.h` |
| `parity/opregion_service.c` | moved | functions: found 5. New homes: `display/opregion.c` (5) | `display/opregion.c` |
| `parity/opregion_service.h` | moved | definitions present in new tree: 7/7 (100%); mostly in `display/opregion.c`, `display/internal.h`, `tests/display/opregion-ktest.c` | `display/opregion.h` |
| `parity/opregion_vbt.c` | moved | functions: found 3. New homes: `display/vbt.c` (3) | `display/vbt.c` |
| `parity/opregion_vbt.h` | moved | definitions present in new tree: 6/6 (100%); mostly in `display/internal.h`, `display/vbt.c`, `display/opregion.c` | `display/vbt.h` |
| `parity/osdep/address_types.h` | moved | functions: found 7. New homes: `dma.h` (7) | `memory.h` |
| `parity/osdep/dma.c` | moved | functions: found 18. New homes: `dma.c` (18) | `memory.c` |
| `parity/osdep/dma.h` | moved (renamed) | `dma.h`: `osdep_dma_backend` → `struct i915_dma_ops` (dma.h:179), `osdep_dma_device` → `struct i915_dma`, `osdep_dma_dir` → `enum i915_dma_direction` | `memory.h` |
| `parity/osdep/firmware.c` | moved | functions: found 4. New homes: `firmware.c` (4) | `firmware.c` |
| `parity/osdep/firmware.h` | moved | `firmware.h`; `osdep_firmware_test_ops` → `drv_i915_firmware_set_override` (test hook, still in production `firmware.c`) | `firmware.h` |
| `parity/osdep/mmio.c` | moved | functions: found 18. New homes: `mmio.c` (18) | `mmio.c` |
| `parity/osdep/mmio.h` | moved (renamed) | `mmio.h`: `osdep_mmio_backend` → `struct i915_mmio_ops` (mmio.h:69), `osdep_fw_domain` → `I915_FORCEWAKE_*` (rules §2), `OSDEP_FW_ACK_POLLS` → ack-poll constant in mmio.h | `mmio.h` |
| `parity/osdep/pci.c` | moved | functions: found 22. New homes: `pci.c` (22) | `device.c` |
| `parity/osdep/pci.h` | moved | definitions present in new tree: 23/25 (92%); mostly in `pci.h`, `pci.c`, `tests/contracts/pci_contract_test.c`; absent e.g. `osdep_pci_backend`, `osdep_pci_res` | `device.h` |
| `parity/osdep/runtime_pm.c` | moved | functions: found 11. New homes: `runtime-pm.c` (11) | `power.c` |
| `parity/osdep/runtime_pm.h` | moved (renamed) | `runtime-pm.h`: `osdep_rpm_backend` → `struct i915_rpm_ops` (runtime-pm.h:40) | `power.h` |
| `parity/osdep/sync.c` | moved | functions: found 7, test 9, test-moved 1. New homes: `workqueue.c` (4), `sync.c` (3) | `sync.c` |
| `parity/osdep/sync.h` | moved / test (S5 T2) | completion/work types → `sync.h`/`workqueue.h` (`OSDEP_WQ_DEPTH` → `I915_WORKQUEUE_DEPTH`); the osdep sync model is test-only ([s1-reports](i915-rebuild-s1-reports.md) L22) | `sync.h` |
| `parity/osdep/trace.c` | moved | functions: found 5. New homes: `trace.c` (5) | `trace.c` |
| `parity/osdep/trace.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `trace.h`, `trace.c`, `tests/contracts/dma_contract_test.c` | `trace.h` |
| `parity/parity.h` | moved (replaced) | `parity_stage`/`parity_outcome`/`parity_result` replaced by `device->stage` and the start log in `device.c` (L432-438); the runner result part goes to T4a `tests/execution/` | `device.h` / `tests/execution/runner.h` |
| `parity/pch.c` | moved | functions: found 5, test 1. New homes: `display/display.c` (5) | `device-info.c` |
| `parity/pch.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `display/internal.h`, `tests/execution/ktest-display-probe.c`, `display/display.c` | `device-info.h` |
| `parity/pcode.c` | moved | functions: found 8. New homes: `power.c` (8) | `power.c` |
| `parity/pcode.h` | moved (declarations) | declarations only (4 prototypes); the functions are mapped in §3 | `power.h` |
| `parity/power_domains.c` | moved | functions: found 46. New homes: `display/power.c` (46) | `display/power.c` |
| `parity/power_domains.h` | moved | definitions present in new tree: 19/19 (100%); mostly in `display/internal.h`, `display/power.c`, `display/power.h` | `display/power.h` |
| `parity/probe.c` | moved | functions: found 7. New homes: `device.c` (6), `display/takeover.c` (1) | `device-info.c`, `device.c`, `display/takeover.c`, `trace.c` |
| `parity/pte.c` | moved | functions: found 3, test 1. New homes: `ggtt.c` (3) | `ppgtt.c` |
| `parity/pte.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `ggtt.h`, `data/i915-regs.inc`, `ggtt.c` | `ppgtt.h` |
| `parity/pxp.c` | moved | functions: found 3. New homes: `pxp.c` (2), `display/dp-sink.c` (1) | `device.c` |
| `parity/pxp.h` | moved | definitions present in new tree: 3/3 (100%); mostly in `data/i915-execution.inc`, `pxp.c`, `pxp.h` | `device.h` |
| `parity/reset.c` | moved | functions: found 1. New homes: `reset.c` (1) | `reset.c` |
| `parity/reset.h` | moved (declarations) | declarations only (1 prototypes); the functions are mapped in §3 | `reset.h` |
| `parity/resident.h` | moved (knob dropped) | `PARITY_RESIDENT`/`PARITY_RESIDENT_DISPLAY` are treated as always true ([rules §4](i915-rebuild-rules.md) L127); types in `display/internal.h` | `device.h` / `request.h` / `display/display.h` |
| `parity/resident_display.c` | moved | functions: found 14, test 1. New homes: `display/display.c` (7), `display/present.c` (4), `display/scanout.c` (2), `display/hotplug.c` (1) | `display/display.c`, `display/hotplug.c`, `display/present.c`, `display/scanout.c`, `tests/render/readback.c` |
| `parity/resident_display.h` | moved (declarations) | declarations only (1 prototypes); the functions are mapped in §3 | `display/display.h` / `display/scanout.h` / `display/present.h` |
| `parity/runner.c` | moved | functions: found 5, test 1. New homes: `device.c` (5) | `device.c`, `tests/execution/runner.c`, readiness/device保持。test結果部分はtests |
| `parity/runner.h` | moved (declarations) | declarations only (1 prototypes); the functions are mapped in §3 | `device.h`（登録/readinessの本番責務） |
| `parity/tests/dma_contract_test.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/dma_contract_test.c`; functions located in a test tree: 2/2 | `tests/fixtures/dma-contract-test.c` |
| `parity/tests/mmio_contract_test.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/mmio_contract_test.c`; functions located in a test tree: 1/1 | `tests/fixtures/mmio-contract-test.c` |
| `parity/tests/mock_dma.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/mock_dma.c`; functions located in a test tree: 10/12 | `tests/fixtures/mock-dma.c` |
| `parity/tests/mock_dma.h` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/mock_dma.h`; functions located in a test tree: 0/0 | `tests/fixtures/mock-dma.h` |
| `parity/tests/mock_mmio.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/mock_mmio.c`; functions located in a test tree: 5/10 | `tests/fixtures/mock-mmio.c` |
| `parity/tests/mock_mmio.h` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/mock_mmio.h`; functions located in a test tree: 0/0 | `tests/fixtures/mock-mmio.h` |
| `parity/tests/mock_pci.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/mock_pci.c`; functions located in a test tree: 10/19 | `tests/fixtures/mock-pci.c` |
| `parity/tests/mock_pci.h` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/mock_pci.h`; functions located in a test tree: 0/0 | `tests/fixtures/mock-pci.h` |
| `parity/tests/pci_contract_test.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/pci_contract_test.c`; functions located in a test tree: 1/1 | `tests/fixtures/pci-contract-test.c` |
| `parity/tests/pte_contract_test.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/pte_contract_test.c`; functions located in a test tree: 1/1 | `tests/fixtures/pte-contract-test.c` |
| `parity/tests/rpm_contract_test.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/rpm_contract_test.c`; functions located in a test tree: 1/3 | `tests/fixtures/rpm-contract-test.c` |
| `parity/tests/run.sh` | test (S5 T2) | contract runner; destination `tests/contracts/run.sh` | `tests/contracts/run.sh` |
| `parity/tests/sync_contract_test.c` | test (S5 T2) | destination present: `src/drivers/gpu/i915/tests/contracts/sync_contract_test.c`; functions located in a test tree: 1/5 | `tests/fixtures/sync-contract-test.c` |
| `parity/timer_calc.c` | moved | functions: test 5. New homes: — | `sync.c` |
| `parity/timer_calc.h` | test (S5 T4a) | timer model, test-only ([s1-reports](i915-rebuild-s1-reports.md) L22) | `sync.h` |
| `parity/vbt/intel_bios.h` | moved | definitions present in new tree: 31/31 (100%); mostly in `data/display-intel-bios.inc`, `data/display-intel-vbt-defs.inc`, `data/display-vbt-ref-types.inc` | `display/vbt.h` |
| `parity/vbt/intel_bios_port.c` | moved | functions: found 91. New homes: `display/vbt.c` (91) | `display/vbt.c` |
| `parity/vbt/intel_vbt_defs.h` | moved | definitions present in new tree: 209/209 (100%); mostly in `data/display-intel-vbt-defs.inc`, `display/vbt.c`, `data/display-vbt-tables.inc` | `data/display-intel-vbt-defs.inc` |
| `parity/vbt/parity_vbt.h` | moved | definitions present in new tree: 5/5 (100%); mostly in `display/internal.h`, `display/vbt.c`, `display/vbt-parse.h` | `display/vbt.h` |
| `parity/vbt/parity_vbt_glue.inc` | moved | functions: found 12. New homes: `display/vbt.c` (11), `display/edid.c` (1) | `display/vbt.c` |
| `parity/vbt/vbt_compat.h` | moved | functions: found 11. New homes: `display/vbt.h` (8), `display/takeover.c` (2), `display/vbt.c` (1) | `display/vbt.h` |
| `parity/vbt/vbt_ref_types.h` | moved | definitions present in new tree: 8/8 (100%); mostly in `data/display-vbt-ref-types.inc`, `display/internal.h`, `display/vbt.c` | `display/vbt.h` |
| `parity/vga.c` | moved | functions: found 12. New homes: `display/takeover.c` (12) | `display/takeover.c` |
| `parity/vga.h` | moved | definitions present in new tree: 6/6 (100%); mostly in `display/internal.h`, `display/takeover.c`, `display/takeover.h` | `display/takeover.h` |
| `parity/wait.c` | moved | functions: found 9, test 2. New homes: `sync.c` (9) | `sync.c` |
| `parity/wait.h` | moved / test (S5 T4a) | wait API → `sync.h`; `parity_time_test_ops` is the test hook, test-only ([s1-reports](i915-rebuild-s1-reports.md) L22) | `sync.h` |
| `ppgtt.c` | moved | functions: found 11, retired 1. New homes: `ppgtt.c` (11) | `ppgtt.c` |
| `request.c` | partial (5 MISSING) | functions: found 6, missing 5. New homes: `request-queue.c` (6) | `request.c` |
| `selftest.c` | retired / test (S5 T4a) | [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) | `tests/execution/selftest.c` |
| `tex_fixture_fhd_gen.inc` | test (S5 T4a) | destination `tests/fixtures/` | `tests/fixtures/tex-fixture-fhd-gen.inc` |
| `tex_fixture_gen.inc` | test (S5 T4a) | destination `tests/fixtures/` | `tests/fixtures/tex-fixture-gen.inc` |
| `uncore.c` | retired | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 | `mmio.c`, `reset.c` |
| `vk/cmd.c` | moved | functions: found 17. New homes: `render/codec.c` (7), `render/object.c` (5), `render/dispatch.c` (2), `render/transport.c` (2), `render/fence.c` (1) | `render/codec.c`, `render/dispatch.c`, `render/object.c`, `render/transport.c` |
| `vk/cmd.h` | moved (declarations) | declarations of vk/cmd.c; the functions are mapped in §3 (`render/codec.h`, `render/object.h`, `render/dispatch.h`, `render/transport.h`) | `render/codec.h` / `render/object.h` / `render/dispatch.h` |
| `vk/cmdbuf.c` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/command.c` |
| `vk/cmdbuf.h` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/command.h` |
| `vk/codec-generated.inc` | moved | `data/vulkan-codec.inc` (147 generated codec functions; generator gen_vk_server_codec.py updated, [s3-reports](i915-rebuild-s3-reports.md) S3a) | `data/vulkan-codec.inc` |
| `vk/compile.c` | moved | functions: found 12. New homes: `compiler/compile.c` (12) | `compiler/compile.c` |
| `vk/compile.h` | moved | `compiler/compiler.h` (`struct i915_shader_binary` with the same metadata fields); `i915_vk_urb_slot` had no user in the old tree | `compiler/compiler.h` |
| `vk/display.c` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/wsi.c` |
| `vk/display.h` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/wsi.h` |
| `vk/eu.c` | moved | functions: found 25. New homes: `compiler/eu.c` (25) | `compiler/eu.c` |
| `vk/eu.h` | moved | definitions present in new tree: 4/4 (100%); mostly in `compiler/eu.c`, `compiler/compile.c`, `compiler/eu.h` | `compiler/eu.h` |
| `vk/gfx-draw.c` | moved | functions: found 36, test 2. New homes: `render/state.c` (13), `render/batch.c` (5), `render/math.c` (5), `render/blit.c` (5), `render/draw.c` (4), `render/pipeline-prepare.c` (4) | `render/batch.c`, `render/blit.c`, `render/draw.c`, `render/math.c`, `render/memory.c`, `render/pipeline.c`, `render/state.c`, `tests/render/readback.c`, `tests/render/reference-shaders.c`, `data/render-eot.inc`、render内部layout型、device-info |
| `vk/gfx-obj.c` | moved | functions: found 32. New homes: `render/memory.c` (9), `render/pipeline.c` (6), `render/image.c` (5), `render/descriptor.c` (4), `render/reply.c` (3), `render/objects.c` (2) | `render/codec.c`, `render/descriptor.c`, `render/dispatch.c`, `render/image.c`, `render/memory.c`, `render/object.c`, `render/pipeline.c`, `render/render-pass.c`, `render/sync.c` |
| `vk/gfx-rec.c` | moved | functions: found 26. New homes: `render/command.c` (25), `render/reply.c` (1) | `render/blit.c`, `render/codec.c`, `render/command.c` |
| `vk/gfx.h` | moved | definitions present in new tree: 21/21 (100%); mostly in `render/gfx.h`, `render/command.c`, `render/state.c` | `render/internal.h` / `render/object.h` / `render/image.h` / `render/pipeline.h` / `render/blit.h` |
| `vk/inst.c` | moved | functions: found 16. New homes: `render/instance.c` (15), `render/transport.c` (1) | `render/instance.c`, `render/transport.c` |
| `vk/linux/3dstate-gen12.inc` | moved | definitions present in new tree: 166/166 (100%); mostly in `data/i915-3dstate-gen12.inc`, `tests/fixtures/draw-fixture.c`, `render/state.c` | `data/3dstate-gen12.inc` |
| `vk/linux/eu-encoding-gen12.inc` | moved | definitions present in new tree: 92/92 (100%); mostly in `data/eu-encoding-gen12.inc`, `compiler/eu.c` | `data/eu-encoding-gen12.inc` |
| `vk/linux/surface-state-gen12.inc` | unported (with vk/res.c) | only user was the unported `vk/res.c` (GEN12_SURFACE_*_DWORD/FORMAT_*); the one macro production used, `GEN12_SURFACE_ALIGN_4`, is in `data/i915-3dstate-gen12.inc:167` | `data/surface-state-gen12.inc` |
| `vk/pipe.c` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/batch.c`, `render/pipeline.c`, `render/state.c` |
| `vk/pipe.h` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/batch.h` / `render/pipeline.h` / `render/state.h` |
| `vk/res.c` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/descriptor.c`, `render/dispatch.c`, `render/image.c`, `render/memory.c`, `render/state.c` |
| `vk/res.h` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/memory.h` / `render/image.h` / `render/descriptor.h` |
| `vk/spirv.c` | moved | functions: found 14. New homes: `compiler/spirv.c` (14) | `compiler/spirv.c` |
| `vk/spirv.h` | moved (renamed) | `compiler/ir.h`/`compiler.h`: `i915_vk_inst/io/uniform` → `i915_shader_ir_inst/_io/_uniform`, `i915_vk_spirv_diag` → `i915_compile_diagnostic` ([s3-reports](i915-rebuild-s3-reports.md) L10) | `compiler/compiler.h` / `compiler/ir.h` |
| `vk/sync.c` | partial (fence part moved) | fence opcodes 35-38 → `render/fence.c`; rest not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 | `render/sync.c` |
| `vk/sync.h` | partial (fence part moved) | fence opcodes 35-38 → `render/fence.c`; rest not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 | `render/sync.h` |
| `vk/vk-internal.h` | moved (renamed) | `render/internal.h`, `render/gfx.h`, `compiler/ir.h`; `I915_VK_ARENA_BYTES`/`CAPSET_WORDS` → `render/internal.h` (ARENA_BYTES/CAPSET_WORDS) | `render/internal.h` / `compiler/ir.h` |
| `vk/vk.c` | moved | functions: found 7. New homes: `render/vulkan.c` (7) | `render/vulkan.c` |
| `vk/vk.h` | moved (declarations) | declarations of vk/vk.c → `render/render.h` | `render/render.h` |
| `vk/vkc.c` | moved | functions: found 7. New homes: `render/codec.c` (7) | `render/codec.c` |
| `vk/vkc.h` | moved (declarations) | declarations of the generated codec → `render/codec.h` | `render/codec.h` |
| `vk/vkref-generated.inc` | test (S5 T4a) | reference kernels, test-only [s3-reports](i915-rebuild-s3-reports.md) L25 | `tests/fixtures/vkref-generated.inc` |
| `vk/wsi.c` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/wsi.c` |
| `vk/wsi.h` | unported (unreachable) | old module, not ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51. Formal retirement decision not yet recorded (S5 T1 must record the reason for its fixtures) | `render/wsi.h` |

ファイル判定の集計: MISSING 1、moved 310、partial 3、retired 7、test 46、unported 11。

## 3. 旧関数別（関数台帳 §E の 2,988 行）

### 3.1 MISSING（行き先を確認できない関数）

| 旧ファイル:行 | 関数 | 本番への影響の判断 |
| --- | --- | --- |
| `request.c:139` | `drv_i915_request_kick` | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| `request.c:193` | `drv_i915_request_retire` | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| `request.c:337` | `i915_request_emit` | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| `request.c:371` | `i915_request_emit_prologue` | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| `request.c:424` | `i915_request_emit_breadcrumb` | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |

### 3.2 対応表（旧 → 新、全行）

ファイルごとに旧の行番号順。「新」は `新ファイル:新関数`（`src/drivers/gpu/i915/` 相対、試験は repo 相対）。

#### `engine.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 62 | `drv_i915_engines_start` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 142 | `drv_i915_engines_stop` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 162 | `drv_i915_engine_reset` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 206 | `drv_i915_engine_recover` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 264 | `drv_i915_engine_interrupt` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 311 | `drv_i915_engine_idle` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 325 | `i915_engine_init` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 364 | `i915_engine_fini` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 387 | `i915_engine_program` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 430 | `i915_engine_stop_cs` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (engine.c:drv_i915_engine_stop_cs), which is the parity-side port, not this code |
| 454 | `i915_engine_reset_prepare` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 492 | `i915_engine_reset_cancel` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 500 | `i915_mocs_init` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (workarounds.c:drv_i915_mocs_init), which is the parity-side port, not this code |
| 522 | `i915_mocs_control` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 537 | `i915_mocs_l3cc` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |

#### `gem.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 32 | `drv_i915_gem_create` | found | memory.c:drv_i915_gem_create |  |
| 87 | `drv_i915_gem_destroy` | found | memory.c:drv_i915_gem_destroy |  |
| 136 | `drv_i915_gem_share_put` | found | memory.c:drv_i915_gem_share_put |  |
| 154 | `drv_i915_gem_bind_ggtt` | retired |  | [s1-reports](i915-rebuild-s1-reports.md) L54 (callers only retired engine.c/lrc.c) |
| 188 | `drv_i915_gem_unbind_ggtt` | retired |  | [s1-reports](i915-rebuild-s1-reports.md) L54 |
| 206 | `drv_i915_gem_bind_vm` | found | memory.c:drv_i915_gem_bind_vm |  |
| 238 | `drv_i915_gem_unbind_vm` | found | memory.c:drv_i915_gem_unbind_vm |  |
| 255 | `drv_i915_gem_read` | found | memory.c:drv_i915_gem_read |  |
| 280 | `drv_i915_gem_write` | found | memory.c:drv_i915_gem_write |  |

#### `ggtt.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 43 | `drv_i915_ggtt_start` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 111 | `drv_i915_ggtt_stop` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 138 | `drv_i915_ggtt_alloc` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 191 | `drv_i915_ggtt_free` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 224 | `drv_i915_ggtt_insert` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 263 | `drv_i915_ggtt_clear` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 287 | `i915_ggtt_probe` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 350 | `i915_ggtt_scratch_start` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 379 | `i915_ggtt_boot_scanout` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 443 | `i915_ggtt_write_pte` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (ggtt.c:i915_ggtt_write_pte), which is the parity-side port, not this code |
| 457 | `i915_ggtt_flush` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 472 | `i915_ggtt_bit_test` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 488 | `i915_ggtt_bit_set` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |

#### `i915.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 115 | `drv_i915_pci_driver_register` | found | i915.c:drv_i915_pci_driver_register |  |
| 143 | `drv_i915_stream_parse` | found | command.c:drv_i915_stream_parse |  |
| 217 | `i915_attach` | found | i915.c:i915_attach |  |
| 283 | `i915_start` | found | device.c:drv_i915_device_start |  |
| 443 | `i915_stop` | found | device.c:drv_i915_device_stop |  |
| 533 | `i915_detach` | found | i915.c:i915_detach |  |
| 570 | `i915_publish` | found | i915.c:drv_i915_publish |  |
| 650 | `i915_unpublish` | found | i915.c:drv_i915_unpublish |  |
| 683 | `drv_i915_resident_publish` | found | i915.c:drv_i915_publish |  |
| 713 | `drv_i915_resident_unpublish` | found | i915.c:drv_i915_unpublish |  |
| 730 | `i915_open` | found | session.c:i915_open |  |
| 825 | `i915_close` | found | session.c:i915_close |  |
| 880 | `i915_get_info` | found | session.c:i915_get_info |  |
| 902 | `i915_resource_create` | found | resource.c:i915_resource_create |  |
| 972 | `i915_resource_destroy` | found | resource.c:i915_resource_destroy |  |
| 1018 | `i915_resource_read` | found | resource.c:i915_resource_read |  |
| 1052 | `i915_resource_write` | found | resource.c:i915_resource_write |  |
| 1086 | `i915_blob_create` | found | resource.c:i915_blob_create |  |
| 1168 | `i915_share_export` | found | resource.c:i915_share_export |  |
| 1183 | `i915_share_release` | found | resource.c:i915_share_release |  |
| 1193 | `i915_share_import` | found | resource.c:i915_share_import |  |
| 1233 | `i915_resource_map` | found | resource.c:i915_resource_map |  |
| 1279 | `i915_vk_command_reply` | found | render/transport.c:drv_i915_render_transport_reply (called from command.c:375) |  |
| 1316 | `i915_get_capset` | found | command.c:i915_get_capset |  |
| 1345 | `i915_command` | found | command.c:i915_command |  |
| 1391 | `i915_command_submit` | found | command.c:i915_command_submit |  |
| 1454 | `i915_command_drain` | found | command.c:i915_command_drain |  |
| 1479 | `i915_job_reserve` | found | job.c:i915_job_reserve |  |
| 1537 | `i915_job_commit` | found | job.c:i915_job_commit |  |
| 1591 | `i915_job_cancel` | found | job.c:i915_job_cancel |  |
| 1654 | `i915_job_capacity` | found | job.c:i915_job_capacity |  |
| 1699 | `i915_stop_begin` | found | reset.c:i915_stop_begin |  |
| 1725 | `i915_stop_poll` | found | reset.c:i915_stop_poll |  |
| 1758 | `i915_fault` | found | reset.c:i915_fault |  |
| 1793 | `i915_reset_device` | found | reset.c:i915_reset_device |  |
| 1860 | `i915_isolate` | found | reset.c:i915_isolate |  |
| 1895 | `i915_session_contexts_destroy` | found | session.c:i915_session_contexts_destroy |  |
| 1910 | `i915_session_batches_destroy` | found | session.c:i915_session_batches_destroy |  |
| 1934 | `i915_engine_for_timeline` | found | session.c:drv_i915_engine_for_timeline |  |
| 1965 | `i915_submit_stream` | found | command.c:i915_submit_stream |  |
| 2046 | `i915_submit_marker` | found | command.c:i915_submit_marker |  |
| 2085 | `i915_batch_acquire` | found | command.c:i915_batch_acquire |  |
| 2144 | `i915_session_object` | found | command.c:i915_session_object |  |
| 2160 | `i915_reservation` | found | job.c:i915_reservation |  |

#### `irq.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 44 | `drv_i915_irq_start` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 94 | `drv_i915_irq_stop` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 128 | `drv_i915_irq_reset` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (irq.c:drv_i915_irq_reset), which is the parity-side port, not this code |
| 161 | `drv_i915_irq_handler` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (irq.c:i915_irq_handler), which is the parity-side port, not this code |
| 202 | `i915_irq_enable` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 225 | `i915_irq_bank` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 256 | `i915_irq_identity` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 296 | `i915_irq_dispatch` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 325 | `i915_irq_engine` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |

#### `lrc.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 54 | `drv_i915_lrc_create` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 125 | `drv_i915_lrc_destroy` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 155 | `drv_i915_lrc_submit` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 184 | `drv_i915_lrc_reset_csb` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 222 | `drv_i915_lrc_csb_consume` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 276 | `drv_i915_lrc_ring_space` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 312 | `drv_i915_lrc_ring_emit` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 341 | `i915_lrc_set_offsets` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (context.c:i915_lrc_set_offsets), which is the parity-side port, not this code |
| 395 | `i915_lrc_init_regs` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (context.c:i915_lrc_init_regs), which is the parity-side port, not this code |
| 445 | `i915_lrc_render_power_state` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 466 | `i915_lrc_csb_read` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 511 | `i915_lrc_csb_parse` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |

#### `parity/backend_delayed.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 14 | `ktimer_thread` | found | workqueue.c:i915_timer_thread |  |
| 64 | `parity_ktimerq_create` | found | workqueue.c:drv_i915_timer_queue_create |  |
| 91 | `parity_ktimerq_destroy` | found | workqueue.c:drv_i915_timer_queue_destroy |  |
| 105 | `parity_kdelayed_init` | found | workqueue.c:drv_i915_delayed_work_init |  |
| 117 | `wait_not_firing` | found | workqueue.c:i915_delayed_wait_not_firing |  |
| 128 | `disarm` | found | workqueue.c:i915_delayed_disarm |  |
| 142 | `parity_kdelayed_queue` | found | workqueue.c:drv_i915_delayed_queue |  |
| 178 | `parity_kdelayed_cancel` | found | workqueue.c:drv_i915_delayed_cancel |  |
| 196 | `parity_kdelayed_cancel_sync` | found | workqueue.c:drv_i915_delayed_cancel_sync |  |
| 214 | `parity_kdelayed_flush` | test | T4b | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only callers edp_sync_ktest.c and the hotplug model hook |
| 228 | `parity_kdelayed_pending` | found | workqueue.c:drv_i915_delayed_pending |  |

#### `parity/backend_dma.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `b_set_info` | found | dma.c:i915_device_dma_set_info |  |
| 43 | `parity_dma_backend` | found | dma.c:drv_i915_dma_device_ops |  |

#### `parity/backend_mmio.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 42 | `b_raw_read32` | found | mmio.c:drv_i915_raw_read32 | match by tokens |
| 52 | `b_raw_write32` | found | mmio.c:drv_i915_raw_write32 | match by tokens |
| 62 | `fw_req_reg` | found | mmio.c:i915_forcewake_request_register |  |
| 74 | `fw_ack_reg` | found | mmio.c:i915_forcewake_ack_register |  |
| 86 | `b_fw_request` | found | mmio.c:i915_window_forcewake_request |  |
| 96 | `b_fw_ack` | found | mmio.c:i915_window_forcewake_ack |  |
| 113 | `parity_mmio_backend` | found | mmio.c:drv_i915_mmio_window_ops |  |
| 119 | `rpm_resume` | found | runtime-pm.c:i915_rpm_resume |  |
| 120 | `rpm_suspend` | found | runtime-pm.c:i915_rpm_device_suspend | match by tokens |
| 127 | `parity_rpm_backend` | found | runtime-pm.c:drv_i915_rpm_device_ops |  |
| 134 | `pci_probe_pm_resume` | found | runtime-pm.c:i915_rpm_pci_probe_resume |  |
| 145 | `pci_probe_pm_suspend` | found | runtime-pm.c:i915_rpm_pci_probe_suspend |  |
| 155 | `parity_pci_probe_pm_backend` | found | runtime-pm.c:drv_i915_rpm_pci_probe_ops |  |

#### `parity/backend_pci.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 14 | `b_read8` | found | pci.c:i915_pci_config_read8 |  |
| 23 | `b_read16` | found | pci.c:i915_pci_config_read16 |  |
| 32 | `b_read32` | found | pci.c:i915_pci_config_read32 |  |
| 41 | `b_write8` | found | pci.c:i915_pci_config_write8 |  |
| 48 | `b_write16` | found | pci.c:i915_pci_config_write16 |  |
| 55 | `b_write32` | found | pci.c:i915_pci_config_write32 |  |
| 67 | `format_msi_source` | found | pci.c:i915_pci_hex_digit (function containing the matched text) | match by string |
| 88 | `b_alloc_msi_vector` | found | pci.c:i915_pci_alloc_msi_vector |  |
| 109 | `b_free_msi_vector` | found | pci.c:i915_pci_free_msi_vector |  |
| 135 | `parity_pci_backend` | found | pci.c:drv_i915_pci_device_ops |  |

#### `parity/backend_sync.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 14 | `parity_kcompletion_init` | found | sync.c:drv_i915_completion_init |  |
| 23 | `parity_kcomplete` | found | sync.c:drv_i915_complete |  |
| 33 | `parity_kcomplete_all` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |
| 42 | `parity_kreinit_completion` | found | sync.c:drv_i915_reinit_completion |  |
| 51 | `parity_kwait` | found | sync.c:drv_i915_wait_for_completion |  |
| 73 | `parity_kwork_init` | found | workqueue.c:drv_i915_work_init |  |
| 87 | `kwq_remove_pending` | found | workqueue.c:i915_workqueue_remove |  |
| 111 | `kworker_thread` | found | workqueue.c:i915_workqueue_worker |  |
| 149 | `parity_kworkqueue_create` | found | workqueue.c:drv_i915_workqueue_create |  |
| 178 | `parity_kqueue_work` | found | workqueue.c:drv_i915_queue_work |  |
| 206 | `parity_kcancel_work` | found | workqueue.c:drv_i915_cancel_work |  |
| 218 | `parity_kwork_is_pending` | found | workqueue.c:drv_i915_work_pending |  |
| 230 | `parity_kcancel_work_sync` | found | workqueue.c:drv_i915_cancel_work_sync |  |
| 258 | `parity_kflush_work` | found | workqueue.c:drv_i915_flush_work |  |
| 281 | `parity_kworkqueue_destroy` | found | workqueue.c:drv_i915_workqueue_destroy |  |

#### `parity/bios.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 49 | `rd16` | found | display/vbt.c:i915_bios_rd16 | match by substr |
| 55 | `rd32` | found | display/vbt.c:i915_bios_rd32 | match by substr |
| 62 | `parity_bios_is_valid_vbt` | found | display/vbt.c:drv_i915_bios_is_valid_vbt |  |
| 93 | `parity_bios_process_vbt` | found | display/vbt.c:drv_i915_bios_process_vbt |  |
| 132 | `parity_bios_init_vbt_missing_defaults` | found | display/vbt.c:drv_i915_bios_init_vbt_missing_defaults |  |
| 181 | `oprom_get_vbt` | found | display/vbt.c:i915_bios_oprom_get_vbt | match by substr |
| 250 | `sha_rotr` | found | display/vbt.c:i915_sha_rotr |  |
| 253 | `parity_sha256` | found | display/vbt.c:drv_i915_sha256 |  |
| 324 | `parity_vbt_emit` | found | display/vbt.c:drv_i915_vbt_emit |  |
| 331 | `parity_vbt_fmtcheck` | found | display/vbt.c:drv_i915_vbt_fmtcheck |  |
| 367 | `explicit_blob_get` | found | display/vbt.c:i915_bios_explicit_blob_get | match by substr |
| 415 | `parity_bios_set_opregion_vbt` | found | display/vbt.c:drv_i915_bios_set_opregion_vbt |  |
| 422 | `parity_intel_bios_init_ex` | found | display/vbt.c:drv_i915_bios_init_ex |  |
| 544 | `parity_intel_bios_init` | found | display/vbt.c:drv_i915_bios_init |  |
| 551 | `parity_intel_bios_driver_remove` | found | display/vbt.c:drv_i915_bios_driver_remove |  |
| 563 | `explicit_pin_for` | found | display/vbt.c:i915_bios_explicit_pin_for | match by substr |
| 573 | `parity_vbt_explicit_pin` | found | display/vbt.c:drv_i915_vbt_explicit_pin |  |

#### `parity/cdclk.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 95 | `parity_adlp_cdclk_table` | found | display/clock.c:drv_i915_adlp_cdclk_table |  |
| 99 | `parity_icl_cdclk_table` | found | display/clock.c:drv_i915_icl_cdclk_table |  |
| 102 | `divrc` | found | display/clock.c:i915_divrc |  |
| 104 | `hweight16` | found | {device-info.c,display/clock.c}:i915_hweight16 |  |
| 114 | `parity_adlp_display_step` | found | display/clock.c:drv_i915_adlp_display_step |  |
| 126 | `parity_intel_init_cdclk_hooks` | found | display/clock.c:drv_i915_init_cdclk_hooks |  |
| 161 | `calc_voltage_level` | found | display/clock.c:i915_calc_voltage_level |  |
| 173 | `parity_tgl_calc_voltage_level` | found | display/clock.c:drv_i915_tgl_calc_voltage_level |  |
| 181 | `cdclk_calc_voltage_level` | found | display/clock.c:i915_cdclk_calc_voltage_level |  |
| 189 | `skl_cdclk_decimal` | found | display/clock.c:i915_skl_cdclk_decimal |  |
| 193 | `parity_bxt_calc_cdclk` | found | display/clock.c:drv_i915_bxt_calc_cdclk |  |
| 208 | `parity_bxt_calc_cdclk_pll_vco` | found | display/clock.c:drv_i915_bxt_calc_cdclk_pll_vco |  |
| 225 | `bxt_cdclk_cd2x_div_sel` | found | display/clock.c:i915_bxt_cdclk_cd2x_div_sel |  |
| 241 | `cdclk_squash_waveform` | found | display/clock.c:i915_cdclk_squash_waveform |  |
| 254 | `cdclk_pll_is_unknown` | found | display/clock.c:i915_cdclk_pll_is_unknown |  |
| 259 | `icl_readout_refclk` | found | display/clock.c:i915_icl_readout_refclk |  |
| 274 | `bxt_de_pll_readout` | found | display/clock.c:i915_bxt_de_pll_readout |  |
| 295 | `parity_bxt_get_cdclk` | found | display/clock.c:drv_i915_bxt_get_cdclk |  |
| 330 | `parity_intel_update_cdclk` | found | display/clock.c:drv_i915_update_cdclk |  |
| 340 | `icl_cdclk_pll_disable` | found | display/clock.c:i915_icl_cdclk_pll_disable |  |
| 351 | `icl_cdclk_pll_enable` | found | display/clock.c:i915_icl_cdclk_pll_enable |  |
| 366 | `icl_cdclk_pll_update` | found | display/clock.c:i915_icl_cdclk_pll_update |  |
| 375 | `adlp_cdclk_pll_crawl` | found | display/clock.c:i915_adlp_cdclk_pll_crawl |  |
| 401 | `_bxt_set_cdclk` | found | display/clock.c:drv_i915_bxt_set_cdclk | match by substr |
| 439 | `parity_bxt_set_cdclk` | found | display/clock.c:drv_i915_bxt_set_cdclk |  |
| 485 | `parity_bxt_sanitize_cdclk` | found | display/clock.c:drv_i915_bxt_sanitize_cdclk |  |
| 529 | `bxt_cdclk_init_hw` | found | display/clock.c:i915_bxt_cdclk_init_hw |  |
| 550 | `parity_intel_cdclk_init_hw` | found | display/clock.c:drv_i915_cdclk_init_hw |  |
| 558 | `parity_intel_max_cdclk_freq` | found | display/clock.c:drv_i915_max_cdclk_freq |  |

#### `parity/combo_phy.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 47 | `combophy_base` | found | display/phy.c:i915_combophy_base |  |
| 48 | `comp_dw` | found | display/phy.c:i915_comp_dw |  |
| 49 | `cl_dw` | found | display/phy.c:i915_cl_dw |  |
| 50 | `phy_misc` | found | display/phy.c:i915_phy_misc |  |
| 51 | `tx_dw8_ln0` | found | display/phy.c:i915_tx_dw8_ln0 |  |
| 52 | `tx_dw8_grp` | found | display/phy.c:i915_tx_dw8_grp |  |
| 53 | `pcs_dw1_ln0` | found | display/phy.c:i915_pcs_dw1_ln0 |  |
| 54 | `pcs_dw1_grp` | found | display/phy.c:i915_pcs_dw1_grp |  |
| 57 | `has_phy_misc` | found | display/phy.c:i915_has_phy_misc |  |
| 58 | `phy_is_master` | found | display/phy.c:i915_phy_is_master |  |
| 61 | `rmw` | found | display/hotplug.c:drv_i915_hpd_rmw |  |
| 70 | `get_procmon` | found | display/phy.c:i915_get_procmon |  |
| 87 | `check_phy_reg` | found | display/phy.c:i915_check_phy_reg |  |
| 93 | `verify_procmon` | found | display/phy.c:i915_verify_procmon |  |
| 105 | `set_procmon` | found | display/phy.c:i915_set_procmon |  |
| 116 | `combo_phy_enabled` | found | display/phy.c:i915_combo_phy_enabled |  |
| 125 | `parity_combo_phy_verify_state` | found | display/phy.c:drv_i915_combo_phy_verify_state |  |
| 148 | `parity_combo_phy_init_one` | found | display/phy.c:drv_i915_combo_phy_init_one |  |
| 180 | `parity_intel_combo_phy_init` | found | display/phy.c:drv_i915_combo_phy_init |  |

#### `parity/display_core.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 77 | `rmw` | found | display/hotplug.c:drv_i915_hpd_rmw |  |
| 86 | `gen9_set_dc_state_disable` | found | display/power.c:i915_gen9_set_dc_state_disable |  |
| 99 | `pch_reset_handshake` | found | display/power.c:i915_pch_reset_handshake |  |
| 107 | `parity_enabled_dbuf_slices_mask` | found | display/power.c:drv_i915_enabled_dbuf_slices_mask |  |
| 120 | `gen9_dbuf_slice_set` | found | display/power.c:i915_gen9_dbuf_slice_set |  |
| 135 | `gen9_dbuf_slices_update` | found | display/power.c:i915_gen9_dbuf_slices_update |  |
| 148 | `parity_dbuf_ctl_reg` | found | display/power.c:drv_i915_dbuf_ctl_reg |  |
| 155 | `parity_gen9_dbuf_slices_update` | found | display/power.c:drv_i915_gen9_dbuf_slices_update |  |
| 165 | `gen12_dbuf_slices_config` | found | display/power.c:i915_gen12_dbuf_slices_config |  |
| 184 | `gen9_dbuf_enable` | found | display/power.c:i915_gen9_dbuf_enable |  |
| 214 | `icl_mbus_init` | found | display/power.c:i915_icl_mbus_init |  |
| 239 | `tgl_bw_buddy_init` | found | display/power.c:i915_tgl_bw_buddy_init |  |
| 291 | `fault` | found | reset.c:i915_fault |  |
| 301 | `icl_display_core_init` | found | display/power.c:i915_icl_display_core_init |  |
| 392 | `parity_dc_off_enable` | found | display/power.c:drv_i915_dc_off_enable |  |
| 441 | `parity_intel_power_domains_init_hw` | found | display/power.c:drv_i915_power_domains_init_hw |  |
| 492 | `parity_intel_power_domains_driver_remove` | found | display/power.c:drv_i915_power_domains_driver_remove |  |

#### `parity/display_nogem.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 91 | `zero_mem` | found | display/takeover.c (inlined as memset) |  |
| 101 | `popcount4` | found | display/takeover.c:i915_popcount |  |
| 110 | `wr` | found | display/takeover.c (inlined: write + mmio_writes++) |  |
| 117 | `rmw` | found | display/hotplug.c:drv_i915_hpd_rmw |  |
| 132 | `parity_adjust_wm_latency` | found | display/takeover.c:drv_i915_adjust_wm_latency |  |
| 165 | `parity_skl_setup_wm_latency` | found | display/takeover.c:drv_i915_skl_setup_wm_latency |  |
| 209 | `intel_sagv_block_time` | found | display/takeover.c:i915_sagv_block_time |  |
| 232 | `intel_sagv_init` | found | display/takeover.c:i915_sagv_init |  |
| 266 | `parity_intel_shared_dpll_init` | found | display/takeover.c:drv_i915_shared_dpll_init |  |
| 315 | `parity_intel_crtc_init` | found | display/takeover.c:drv_i915_crtc_init |  |
| 377 | `parity_intel_display_wa_apply` | found | display/takeover.c:drv_i915_display_wa_apply |  |
| 416 | `parity_intel_update_max_cdclk` | found | display/takeover.c:drv_i915_update_max_cdclk |  |
| 428 | `gmbus_setup` | found | display/takeover.c:i915_gmbus_setup |  |
| 479 | `parity_intel_display_nogem_front` | found | display/takeover.c:drv_i915_display_nogem_front |  |
| 615 | `icl_dpclka_ddi_clk_off` | found | display/takeover.c:i915_icl_dpclka_ddi_clk_off |  |
| 630 | `icl_dpclka_tc_clk_off` | found | display/takeover.c:i915_icl_dpclka_tc_clk_off |  |
| 638 | `parity_intel_ddi_crt_present` | found | display/takeover.c:drv_i915_ddi_crt_present |  |
| 652 | `parity_dvo_port_to_port` | found | display/takeover.c:drv_i915_dvo_port_to_port |  |
| 684 | `parity_intel_port_to_phy` | found | display/takeover.c:drv_i915_port_to_phy |  |
| 692 | `parity_intel_phy_is_tc` | found | display/takeover.c:drv_i915_phy_is_tc |  |
| 700 | `parity_intel_ddi_is_tc` | found | display/takeover.c:drv_i915_ddi_is_tc |  |
| 711 | `ddi_lanes_domain` | found | display/takeover.c:i915_ddi_lanes_domain |  |
| 724 | `port_in_use` | found | display/takeover.c:i915_port_in_use |  |
| 735 | `ddi_skip` | found | display/takeover.c:i915_ddi_skip |  |
| 751 | `intel_ddi_init` | found | display/takeover.c:i915_ddi_init |  |
| 861 | `parity_intel_setup_outputs` | found | display/takeover.c:drv_i915_setup_outputs |  |
| 895 | `parity_intel_ddi_get_hw_state` | found | display/takeover.c:drv_i915_ddi_get_hw_state |  |
| 966 | `parity_intel_ddi_is_clock_enabled` | found | display/takeover.c:drv_i915_ddi_is_clock_enabled |  |
| 987 | `parity_intel_ddi_disable_clock` | found | display/takeover.c:drv_i915_ddi_disable_clock |  |
| 1029 | `trans_reg` | found | display/takeover.c:i915_trans_reg |  |
| 1043 | `trans_power_on` | found | display/takeover.c:i915_trans_power_on |  |
| 1058 | `parity_hsw_enabled_transcoders` | found | display/takeover.c:drv_i915_hsw_enabled_transcoders |  |
| 1112 | `get_transcoder_timings` | found | {display/pipe.c,display/takeover.c}:i915_get_transcoder_timings |  |
| 1128 | `hsw_get_pipe_config` | found | display/pipe.c:i915_hsw_get_pipe_config |  |
| 1175 | `readout_plane_state` | found | display/takeover.c:i915_readout_plane_state |  |
| 1214 | `parity_intel_dpll_readout` | found | display/takeover.c:drv_i915_dpll_readout |  |
| 1271 | `parity_intel_modeset_readout_hw_state` | found | display/takeover.c:drv_i915_modeset_readout_hw_state |  |
| 1375 | `intel_early_display_was` | found | display/takeover.c:i915_early_display_was |  |
| 1387 | `intel_fbc_sanitize` | found | display/takeover.c:i915_nogem_fbc_sanitize | match by substr |
| 1417 | `sanitize_encoder_pll_mapping` | found | display/takeover.c:i915_nogem_sanitize_encoder_pll_mapping (function containing the matched text) | match by string |
| 1439 | `intel_sanitize_crtc` | found | display/takeover.c:i915_sanitize_crtc |  |
| 1469 | `adlp_cmtg_clock_gating_wa` | found | display/clock.c:i915_adlp_cmtg_clock_gating_wa |  |
| 1490 | `parity_intel_dpll_sanitize_state` | found | display/clock.c:drv_i915_dpll_sanitize_state |  |
| 1526 | `intel_power_domains_sanitize_state` | found | display/clock.c:i915_icl_cdclk_pll_update (function containing the matched text) | match by string |
| 1546 | `parity_intel_modeset_sanitize_hw_state` | found | display/takeover.c:drv_i915_modeset_sanitize_hw_state |  |
| 1634 | `parity_intel_display_nogem_fini` | found | display/takeover.c:drv_i915_display_nogem_fini |  |

#### `parity/display_state.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 48 | `zero_mem` | found | display/state.c (inlined as memset) |  |
| 59 | `genmask_low` | found | display/watermark.c:i915_genmask_low |  |
| 70 | `is_power_of_2` | found | display/watermark.c:i915_is_power_of_2 |  |
| 77 | `rmw` | found | display/hotplug.c:drv_i915_hpd_rmw |  |
| 93 | `parity_atomic_global_obj_init` | found | display/state.c:drv_i915_atomic_global_obj_init |  |
| 123 | `alloc_global_state` | found | display/state.c:drv_i915_alloc_global_state |  |
| 152 | `parity_intel_mode_config_init` | found | display/state.c:drv_i915_mode_config_init |  |
| 220 | `parity_intel_cdclk_init` | found | display/state.c:drv_i915_cdclk_init |  |
| 242 | `parity_intel_color_init` | found | display/state.c:drv_i915_color_init |  |
| 263 | `parity_intel_dbuf_init` | found | display/state.c:drv_i915_dbuf_init |  |
| 286 | `parity_intel_has_sagv` | found | display/watermark.c:drv_i915_has_sagv |  |
| 299 | `parity_icl_qgv_points_mask` | found | display/watermark.c:drv_i915_icl_qgv_points_mask |  |
| 326 | `icl_max_bw_index` | found | display/watermark.c:i915_icl_max_bw_index |  |
| 352 | `tgl_max_bw_index` | found | display/watermark.c:i915_tgl_max_bw_index |  |
| 378 | `icl_qgv_bw` | found | display/watermark.c:i915_icl_qgv_bw |  |
| 395 | `adl_psf_bw` | found | display/watermark.c:i915_adl_psf_bw |  |
| 404 | `parity_icl_qgv_bw` | found | display/watermark.c:drv_i915_icl_qgv_bw |  |
| 410 | `parity_icl_max_bw_qgv_point_mask` | found | display/watermark.c:drv_i915_icl_max_bw_qgv_point_mask |  |
| 438 | `parity_icl_max_bw_psf_gv_point_mask` | found | display/watermark.c:drv_i915_icl_max_bw_psf_gv_point_mask |  |
| 460 | `icl_prepare_qgv_points_mask` | found | display/watermark.c:i915_icl_prepare_qgv_points_mask |  |
| 469 | `is_sagv_enabled` | found | display/watermark.c:i915_is_sagv_enabled |  |
| 477 | `parity_icl_pcode_restrict_qgv_points` | found | display/watermark.c:drv_i915_icl_pcode_restrict_qgv_points |  |
| 505 | `icl_force_disable_sagv` | found | display/watermark.c:i915_icl_force_disable_sagv |  |
| 527 | `parity_intel_bw_init` | found | display/watermark.c:drv_i915_bw_init |  |
| 558 | `parity_intel_pmdemand_init` | found | display/watermark.c:drv_i915_pmdemand_init |  |
| 681 | `intel_set_quirk` | found | display/state.c:i915_intel_set_quirk |  |
| 689 | `parity_intel_init_quirks` | found | display/state.c:drv_i915_init_quirks |  |
| 724 | `fbc_underrun_work_fn` | found | display/state.c:i915_fbc_underrun_work_fn |  |
| 737 | `need_fbc_vtd_wa` | found | display/state.c:i915_need_fbc_vtd_wa |  |
| 751 | `parity_intel_sanitize_fbc_option` | found | display/state.c:drv_i915_sanitize_fbc_option |  |
| 768 | `intel_fbc_create` | found | display/state.c:i915_intel_fbc_create |  |
| 799 | `parity_intel_fbc_init` | found | display/state.c:drv_i915_fbc_init |  |
| 831 | `parity_intel_display_state_fini` | found | display/state.c:drv_i915_display_state_fini |  |

#### `parity/dmc.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 95 | `arena_copy` | found | display/dmc.c:i915_arena_copy |  |
| 109 | `is_valid_dmc_id` | found | display/dmc.c:i915_is_valid_dmc_id |  |
| 113 | `fw_info_matches_stepping` | found | display/dmc.c:i915_fw_info_matches_stepping |  |
| 125 | `dmc_set_fw_offset` | found | display/dmc.c:i915_dmc_set_fw_offset |  |
| 146 | `mmio_addr_ok` | found | display/dmc.c:i915_mmio_addr_ok |  |
| 174 | `parse_css` | found | display/dmc.c:i915_parse_css |  |
| 192 | `parse_package` | found | display/dmc.c:i915_parse_package |  |
| 230 | `parse_header` | found | display/dmc.c:i915_parse_header |  |
| 316 | `parity_dmc_prepare` | found | display/dmc.c:drv_i915_dmc_prepare |  |
| 332 | `parity_parse_dmc_fw` | found | display/dmc.c:drv_i915_parse_dmc_fw |  |
| 365 | `parity_dmc_parse_reset` | found | display/dmc.c:drv_i915_dmc_parse_reset |  |
| 378 | `parity_dmc_has_payload` | found | display/dmc.c:drv_i915_dmc_has_payload |  |
| 399 | `dmc_reg_base` | found | display/dmc.c:i915_dmc_reg_base |  |
| 405 | `dmc_reg` | found | display/dmc.c:i915_dmc_reg |  |
| 406 | `dmc_evt_ctl` | found | display/dmc.c:i915_dmc_evt_ctl |  |
| 407 | `dmc_evt_htp` | found | display/dmc.c:i915_dmc_evt_htp |  |
| 410 | `is_evt_ctl` | found | display/dmc.c:i915_is_evt_ctl |  |
| 421 | `dmc_mmiodata` | found | display/dmc.c:i915_dmc_mmiodata |  |
| 429 | `payload_dword` | found | display/dmc.c:i915_payload_dword |  |
| 438 | `rmw32` | found | mmio.c:drv_i915_rmw32 |  |
| 446 | `parity_intel_dmc_load_program` | found | display/dmc.c:drv_i915_dmc_load_program |  |
| 538 | `dmc_get_ref` | found | display/dmc.c:i915_dmc_get_ref |  |
| 546 | `dmc_put_ref` | found | display/dmc.c:i915_dmc_put_ref |  |
| 556 | `dmc_load_work_fn` | found | display/dmc.c:i915_dmc_load_work_fn |  |
| 609 | `parity_intel_dmc_init` | found | display/dmc.c:drv_i915_dmc_init |  |
| 643 | `parity_intel_dmc_fini` | found | display/dmc.c:drv_i915_dmc_fini |  |

#### `parity/dp/dp_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 58 | `parity_dp_memcpy` | found | display/dp-internal.h:i915_dp_memcpy |  |
| 97 | `i915_mmio_reg_offset` | found | display/internal.h:i915_mmio_reg_offset |  |
| 98 | `i915_mmio_reg_valid` | found | display/internal.h:i915_mmio_reg_valid |  |
| 132 | `parity_dp_env_of` | found | display/dp-internal.h:i915_dp_env_of |  |
| 137 | `intel_de_rmw` | found | display/modeset-internal.h:i915_lcd_intel_de_rmw |  |
| 163 | `wait_remaining_ms_from_jiffies` | found | display/dp-internal.h:i915_wait_remaining_ms_from_jiffies |  |
| 230 | `i2c_transfer` | found | display/dp-internal.h:i915_i2c_transfer |  |
| 289 | `dp_to_dig_port` | found | display/modeset-internal.h:i915_lcd_dp_to_dig_port |  |
| 293 | `dp_to_i915` | found | display/dp-internal.h:i915_dp_to_i915 |  |
| 295 | `to_i915` | found | display/modeset-internal.h:i915_lcd_to_i915 |  |
| 299 | `intel_dp_is_edp` | found | display/dp.c:drv_i915_dp_is_edp |  |
| 301 | `intel_aux_power_domain` | found | display/pipe.c:drv_i915_aux_power_domain |  |

#### `parity/dp/dp_fake_hw.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 39 | `bytes_clear` | test | T3 | S5 T3 (destination not present yet) |
| 47 | `dp_fake_init` | test-moved | tests/display/dp-fake-hw.c:drv_i915_dp_fake_init | S5 T3 |
| 69 | `dp_fake_script` | test-moved | tests/display/dp-fake-hw.c:drv_i915_dp_fake_script | S5 T3 |
| 86 | `aux_finish` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_aux_finish | S5 T3 |
| 91 | `aux_reply` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_aux_reply | S5 T3 |
| 102 | `aux_transaction` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_aux_transaction | S5 T3 |
| 224 | `pp_status` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_pp_status | S5 T3 |
| 230 | `fake_read` | found | display/hotplug.c:i915_hpd_fake_read | ledger said test; kept in production (e.g. [s4 §4](i915-rebuild-s4.md) L278) |
| 258 | `fake_write` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_write | S5 T3 |
| 299 | `fake_wait_reg` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_wait_reg | S5 T3 |
| 320 | `fake_sleep_us` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_sleep_us | S5 T3 |
| 325 | `fake_now_ms` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_now_ms | S5 T3 |
| 330 | `fake_power_get` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_power_get | S5 T3 |
| 349 | `fake_power_put_async` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_power_put_async | S5 T3 |
| 364 | `fake_lock` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_lock | S5 T3 |
| 374 | `fake_unlock` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_unlock | S5 T3 |
| 383 | `fake_delayed_queue` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_delayed_queue | S5 T3 |
| 396 | `fake_delayed_cancel` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_delayed_cancel | S5 T3 |
| 413 | `fake_delayed_pending` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_delayed_pending | S5 T3 |
| 419 | `release_parked` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_release_parked | S5 T3 |
| 429 | `dp_fake_run_due` | test-moved | tests/display/dp-fake-hw.c:drv_i915_dp_fake_run_due | S5 T3 |
| 445 | `dp_fake_flush_async` | test-moved | tests/display/dp-fake-hw.c:drv_i915_dp_fake_flush_async | S5 T3 |
| 454 | `fake_power_put` | test-moved | tests/display/dp-fake-hw.c:i915_dp_fake_power_put | S5 T3 |
| 464 | `dp_fake_bind_env` | test-moved | tests/display/dp-fake-hw.c:drv_i915_dp_fake_bind_env | S5 T3 |

#### `parity/dp/drm_dp_helper_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 45 | `drm_dp_dump_access` | found | display/dp-sink.c:i915_drm_dp_dump_access |  |
| 70 | `drm_dp_dpcd_access` | found | display/dp-sink.c:i915_drm_dp_dpcd_access |  |
| 147 | `drm_dp_dpcd_probe` | found | display/dp-sink.c:i915_drm_dp_dpcd_probe |  |
| 174 | `drm_dp_dpcd_read` | found | display/dp-sink.c:i915_drm_dp_dpcd_read |  |
| 221 | `drm_dp_dpcd_write` | found | display/dp-sink.c:i915_drm_dp_dpcd_write |  |
| 236 | `drm_dp_read_extended_dpcd_caps` | found | display/dp-sink.c:i915_drm_dp_read_extended_dpcd_caps |  |
| 290 | `drm_dp_read_dpcd_caps` | found | display/dp-sink.c:i915_drm_dp_read_dpcd_caps |  |
| 310 | `drm_dp_i2c_functionality` | found | display/dp-sink.c:i915_drm_dp_i2c_functionality |  |
| 318 | `drm_dp_i2c_msg_write_status_update` | found | display/dp-sink.c:i915_drm_dp_i2c_msg_write_status_update |  |
| 343 | `drm_dp_aux_req_duration` | found | display/dp-sink.c:i915_drm_dp_aux_req_duration |  |
| 354 | `drm_dp_aux_reply_duration` | found | display/dp-sink.c:i915_drm_dp_aux_reply_duration |  |
| 382 | `drm_dp_i2c_msg_duration` | found | display/dp-sink.c:i915_drm_dp_i2c_msg_duration |  |
| 396 | `drm_dp_i2c_retry_count` | found | display/dp-sink.c:i915_drm_dp_i2c_retry_count |  |
| 420 | `drm_dp_i2c_do_msg` | found | display/dp-sink.c:i915_drm_dp_i2c_do_msg |  |
| 529 | `drm_dp_i2c_msg_set_request` | found | display/dp-sink.c:i915_drm_dp_i2c_msg_set_request |  |
| 543 | `drm_dp_i2c_drain_msg` | found | display/dp-sink.c:i915_drm_dp_i2c_drain_msg |  |
| 574 | `drm_dp_i2c_xfer` | found | display/dp-sink.c:i915_drm_dp_i2c_xfer |  |

#### `parity/dp/drm_edid_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 59 | `drm_edid_header_is_valid` | found | display/edid-read.c:i915_drm_edid_header_is_valid |  |
| 72 | `edid_block_compute_checksum` | found | display/edid-read.c:i915_edid_block_compute_checksum |  |
| 86 | `edid_block_get_checksum` | found | display/edid-read.c:i915_edid_block_get_checksum |  |
| 106 | `drm_do_probe_ddc_edid` | found | display/edid-read.c:i915_drm_do_probe_ddc_edid |  |

#### `parity/dp/edp_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 21 | `target_cfg` | test-moved | tests/display/edp-ktest.c:i915_edp_ktest_target_cfg | S5 T4b |
| 32 | `fresh` | test-moved | plan/ws031/tests/dp-host-test.c:fresh | S5 T4b |
| 39 | `released` | test-moved | plan/ws031/tests/dp-host-test.c:released | S5 T4b |
| 49 | `parity_edp_ktest` | test-moved | src/drivers/gpu/i915/tests/display/edp-ktest.c:i915_edp_ktest_acquire (function containing the matched text) | S5 T4b |

#### `parity/dp/edp_sync_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 23 | `ticks_after_ms` | test | T4b | S5 T4b (destination not present yet) |
| 39 | `body_fn` | test | T4b | S5 T4b (destination not present yet) |
| 50 | `delayed_work_checks` | test | T4b | S5 T4b (destination not present yet) |
| 137 | `hy_read32` | test | T4b | S5 T4b (destination not present yet) |
| 143 | `hy_write32` | test | T4b | S5 T4b (destination not present yet) |
| 154 | `hy_wait_reg` | test | T4b | S5 T4b (destination not present yet) |
| 161 | `hy_sleep_us` | test | T4b | S5 T4b (destination not present yet) |
| 162 | `hy_now_ms` | test | T4b | S5 T4b (destination not present yet) |
| 164 | `hy_power_get` | test | T4b | S5 T4b (destination not present yet) |
| 175 | `hy_power_put` | test | T4b | S5 T4b (destination not present yet) |
| 183 | `hy_power_put_async` | test | T4b | S5 T4b (destination not present yet) |
| 191 | `hybrid_fresh` | test | T4b | S5 T4b (destination not present yet) |
| 210 | `hybrid_released` | test | T4b | S5 T4b (destination not present yet) |
| 220 | `wait_vdd` | test | T4b | S5 T4b (destination not present yet) |
| 231 | `edp_concurrency_checks` | test | T4b | S5 T4b (destination not present yet) |
| 307 | `parity_edp_sync_ktest` | test | T4b | S5 T4b (destination not present yet) |

#### `parity/dp/intel_dp_aux_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 29 | `intel_dp_aux_pack` | found | display/aux.c:i915_dp_aux_pack |  |
| 41 | `intel_dp_aux_unpack` | found | display/aux.c:i915_dp_aux_unpack |  |
| 52 | `intel_dp_aux_wait_done` | found | display/aux.c:i915_dp_aux_wait_done |  |
| 75 | `skl_get_aux_clock_divider` | found | display/aux.c:i915_skl_get_aux_clock_divider |  |
| 85 | `intel_dp_aux_sync_len` | found | display/aux.c:i915_dp_aux_sync_len |  |
| 93 | `intel_dp_aux_fw_sync_len` | found | display/aux.c:i915_dp_aux_fw_sync_len |  |
| 114 | `skl_get_aux_send_ctl` | found | display/aux.c:i915_skl_get_aux_send_ctl |  |
| 151 | `intel_dp_aux_xfer` | found | display/aux.c:i915_dp_aux_xfer |  |
| 362 | `intel_dp_aux_header` | found | display/aux.c:i915_dp_aux_header |  |
| 371 | `intel_dp_aux_xfer_flags` | found | display/aux.c:i915_dp_aux_xfer_flags |  |
| 386 | `intel_dp_aux_transfer` | found | display/aux.c:i915_dp_aux_transfer |  |
| 466 | `tgl_aux_ctl_reg` | found | display/aux.c:i915_tgl_aux_ctl_reg |  |
| 488 | `tgl_aux_data_reg` | found | display/aux.c:i915_tgl_aux_data_reg |  |

#### `parity/dp/intel_pps_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 29 | `pps_name` | found | display/panel.c:i915_pps_name |  |
| 63 | `intel_pps_lock` | found | display/panel.c:drv_i915_pps_lock |  |
| 77 | `intel_pps_unlock` | found | display/panel.c:drv_i915_pps_unlock |  |
| 92 | `bxt_power_sequencer_idx` | found | display/panel.c:i915_bxt_power_sequencer_idx |  |
| 118 | `pps_has_pp_on` | found | display/panel.c:i915_pps_has_pp_on |  |
| 123 | `pps_has_vdd_on` | found | display/panel.c:i915_pps_has_vdd_on |  |
| 128 | `pps_any` | found | display/panel.c:i915_pps_any |  |
| 135 | `intel_num_pps` | found | display/panel.c:i915_num_pps |  |
| 155 | `intel_pps_is_valid` | found | display/panel.c:i915_pps_is_valid |  |
| 168 | `bxt_initial_pps_idx` | found | display/panel.c:i915_bxt_initial_pps_idx |  |
| 181 | `pps_initial_setup` | found | display/panel.c:i915_pps_initial_setup |  |
| 236 | `intel_pps_get_registers` | found | display/panel.c:i915_pps_get_registers |  |
| 265 | `_pp_ctrl_reg` | found | display/panel.c:i915_pp_ctrl_reg | match by substr |
| 275 | `_pp_stat_reg` | found | display/panel.c:i915_pp_stat_reg | match by substr |
| 284 | `edp_have_panel_power` | found | display/panel.c:i915_edp_have_panel_power |  |
| 297 | `edp_have_panel_vdd` | found | display/panel.c:i915_edp_have_panel_vdd |  |
| 310 | `intel_pps_check_power_unlocked` | found | display/panel.c:drv_i915_pps_check_power_unlocked |  |
| 343 | `wait_panel_status` | found | display/panel.c:i915_wait_panel_status |  |
| 377 | `wait_panel_on` | found | display/panel.c:i915_wait_panel_on |  |
| 388 | `wait_panel_off` | found | display/panel.c:i915_wait_panel_off |  |
| 399 | `wait_panel_power_cycle` | found | display/panel.c:i915_wait_panel_power_cycle |  |
| 424 | `intel_pps_wait_power_cycle` | found | display/panel.c:drv_i915_pps_wait_power_cycle |  |
| 435 | `wait_backlight_on` | found | display/panel.c:i915_wait_backlight_on |  |
| 441 | `edp_wait_backlight_off` | found | display/panel.c:i915_edp_wait_backlight_off |  |
| 451 | `ilk_get_pp_control` | found | display/panel.c:i915_ilk_get_pp_control |  |
| 472 | `intel_pps_vdd_on_unlocked` | found | display/panel.c:drv_i915_pps_vdd_on_unlocked |  |
| 535 | `intel_pps_vdd_on` | found | display/panel.c:drv_i915_pps_vdd_on |  |
| 553 | `intel_pps_vdd_off_sync_unlocked` | found | display/panel.c:i915_pps_vdd_off_sync_unlocked |  |
| 596 | `intel_pps_vdd_off_sync` | found | display/panel.c:drv_i915_pps_vdd_off_sync |  |
| 612 | `edp_panel_vdd_work` | found | display/panel.c:i915_edp_panel_vdd_work |  |
| 625 | `edp_panel_vdd_schedule_off` | found | display/panel.c:i915_edp_panel_vdd_schedule_off |  |
| 652 | `intel_pps_vdd_off_unlocked` | found | display/panel.c:drv_i915_pps_vdd_off_unlocked |  |
| 675 | `intel_pps_on_unlocked` | found | display/panel.c:drv_i915_pps_on_unlocked |  |
| 738 | `intel_pps_on` | found | display/panel.c:drv_i915_pps_on |  |
| 749 | `intel_pps_off_unlocked` | found | display/panel.c:drv_i915_pps_off_unlocked |  |
| 792 | `intel_pps_off` | found | display/panel.c:drv_i915_pps_off |  |
| 804 | `intel_pps_backlight_on` | found | display/panel.c:drv_i915_pps_backlight_on |  |
| 830 | `intel_pps_backlight_off` | found | display/panel.c:drv_i915_pps_backlight_off |  |
| 857 | `pps_vdd_init` | found | display/panel.c:i915_pps_vdd_init |  |
| 882 | `intel_pps_have_panel_power_or_vdd` | found | display/panel.c:drv_i915_pps_have_panel_power_or_vdd |  |
| 895 | `pps_init_timestamps` | found | display/panel.c:i915_pps_init_timestamps |  |
| 909 | `intel_pps_readout_hw_state` | found | display/panel.c:i915_pps_readout_hw_state |  |
| 944 | `intel_pps_dump_state` | found | display/panel.c:i915_pps_dump_state |  |
| 955 | `intel_pps_verify_state` | found | display/panel.c:i915_pps_verify_state |  |
| 971 | `pps_delays_valid` | found | display/panel.c:i915_pps_delays_valid |  |
| 977 | `pps_init_delays_bios` | found | display/panel.c:i915_pps_init_delays_bios |  |
| 992 | `pps_init_delays_vbt` | found | display/panel.c:i915_pps_init_delays_vbt |  |
| 1024 | `pps_init_delays_spec` | found | display/panel.c:i915_pps_init_delays_spec |  |
| 1046 | `pps_init_delays` | found | display/panel.c:i915_pps_init_delays |  |
| 1109 | `pps_init_registers` | found | display/panel.c:i915_pps_init_registers |  |
| 1200 | `intel_pps_encoder_reset` | found | display/panel.c:drv_i915_pps_encoder_reset |  |
| 1225 | `intel_pps_init` | found | display/panel.c:drv_i915_pps_init |  |
| 1246 | `pps_init_late` | found | display/panel.c:i915_pps_init_late |  |
| 1268 | `intel_pps_init_late` | found | display/panel.c:drv_i915_pps_init_late |  |

#### `parity/dp/parity_dp_aux_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 12 | `parity_intel_dp_aux_init` | found | display/aux.c:drv_i915_dp_aux_init |  |

#### `parity/dp/parity_dp_kernel.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 29 | `k_read32` | found | display/dp-sink.c:i915_dp_kernel_read32 | match by tokens |
| 34 | `k_write32` | found | display/dp-sink.c:i915_dp_kernel_write32 | match by tokens |
| 39 | `k_wait_reg` | found | display/dp-sink.c:i915_dp_kernel_wait_reg | match by tokens |
| 55 | `now_us` | found | display/dp-sink.c:i915_dp_kernel_now_us | match by substr |
| 77 | `parity_dp_kernel_sleep_us` | found | display/dp-sink.c:drv_i915_dp_kernel_sleep_us |  |
| 93 | `k_sleep_us` | found | display/dp-sink.c:drv_i915_dp_sleep_us | match by tokens |
| 98 | `k_now_ms` | found | display/dp-sink.c:drv_i915_dp_now_ms | match by tokens |
| 108 | `k_power_get` | found | display/dp-sink.c:drv_i915_dp_power_get | match by tokens |
| 115 | `k_power_put` | found | display/dp-sink.c:drv_i915_dp_power_put | match by tokens |
| 122 | `k_power_put_async` | found | display/dp-sink.c:drv_i915_dp_power_put_async | match by tokens |
| 130 | `k_lock` | found | display/dp-sink.c:drv_i915_dp_mutex_lock | match by tokens |
| 135 | `k_unlock` | found | display/dp-sink.c:drv_i915_dp_mutex_unlock | match by tokens |
| 140 | `sync_deadline` | found | display/hotplug.c:drv_i915_hpd_sync_deadline |  |
| 149 | `k_delayed_queue` | found | display/dp-sink.c:drv_i915_dp_delayed_queue | match by tokens |
| 157 | `k_delayed_cancel` | found | display/dp-sink.c:drv_i915_dp_delayed_cancel | match by tokens |
| 166 | `k_delayed_pending` | found | display/dp-sink.c:i915_dp_kernel_delayed_pending | match by tokens |
| 174 | `vdd_off_body` | found | display/dp-sink.c:i915_dp_kernel_vdd_off_body | match by substr |
| 180 | `async_put_body` | found | display/dp-sink.c:i915_dp_kernel_async_put_body | match by substr |
| 185 | `pd_async_queue` | found | display/dp-sink.c:i915_dp_kernel_pd_async_queue | match by substr |
| 192 | `pd_async_cancel` | found | display/dp-sink.c:i915_dp_kernel_pd_async_cancel | match by substr |
| 202 | `parity_dp_kernel_sync_start` | found | display/dp-sink.c:drv_i915_dp_kernel_sync_start |  |
| 222 | `parity_dp_kernel_sync_stop` | found | display/dp-sink.c:drv_i915_dp_kernel_sync_stop |  |
| 233 | `parity_dp_kernel_bind_sync` | found | display/dp-sink.c:drv_i915_dp_kernel_bind_sync |  |
| 243 | `parity_dp_kernel_bind` | found | display/dp-sink.c:drv_i915_dp_kernel_bind |  |
| 258 | `log_bytes` | found | display/dp-sink.c:i915_edp_log_bytes |  |
| 269 | `log_pps` | found | display/dp-sink.c:i915_edp_log_pps |  |
| 275 | `cfg_from_panel` | found | display/dp-sink.c:i915_edp_cfg_from_panel |  |
| 296 | `parity_edp_device_prepare` | found | display/dp-sink.c:drv_i915_edp_device_prepare |  |
| 306 | `well_refs` | found | display/dp-sink.c:i915_edp_well_refs |  |
| 315 | `parity_edp_device_init_connector` | found | display/dp-sink.c:drv_i915_edp_device_init_connector |  |
| 462 | `parity_edp_device_fini` | found | display/dp-sink.c:drv_i915_edp_device_fini |  |
| 493 | `vdd_is_on` | test | T4b | helper of parity_edp_aux_test_run (the `aux` scenario, probe.c:1868 under PARITY_AUX_TEST) |
| 500 | `wait_vdd_off` | test | T4b | helper of parity_edp_aux_test_run (the `aux` scenario) |
| 513 | `parity_edp_aux_test_run` | test | T4b | the `aux` scenario (probe.c:1868 under PARITY_AUX_TEST); weak string hit in dp-sink.c is shared log text, not the test |

#### `parity/dp/parity_drm_dp_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 8 | `parity_drm_dp_aux_init` | found | display/dp-sink.c:drv_i915_drm_dp_aux_init |  |

#### `parity/dp/parity_drm_edid_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `parity_drm_edid_read` | found | display/edid-read.c:drv_i915_drm_edid_read |  |

#### `parity/dp/parity_edp.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 43 | `parity_dp_log_enabled` | found | display/dp-sink.c:drv_i915_dp_log_enabled |  |
| 50 | `parity_dp_note` | found | display/dp-sink.c:drv_i915_dp_note |  |
| 58 | `parity_dp_env_current` | found | display/dp-sink.c:drv_i915_dp_env_current |  |
| 63 | `parity_dp_sleep_us` | found | display/dp-sink.c:drv_i915_dp_sleep_us |  |
| 74 | `parity_dp_now_ms` | found | display/dp-sink.c:drv_i915_dp_now_ms |  |
| 81 | `power_slot` | found | display/dp-sink.c:i915_dp_power_slot |  |
| 86 | `parity_dp_power_get` | found | display/dp-sink.c:drv_i915_dp_power_get |  |
| 100 | `parity_dp_power_put` | found | display/dp-sink.c:drv_i915_dp_power_put |  |
| 115 | `parity_dp_power_put_async` | found | display/dp-sink.c:drv_i915_dp_power_put_async |  |
| 132 | `parity_dp_mutex_lock` | found | display/dp-sink.c:drv_i915_dp_mutex_lock |  |
| 145 | `parity_dp_mutex_unlock` | found | display/dp-sink.c:drv_i915_dp_mutex_unlock |  |
| 157 | `parity_dp_delayed_queue` | found | display/dp-sink.c:drv_i915_dp_delayed_queue |  |
| 164 | `parity_dp_delayed_cancel` | found | display/dp-sink.c:drv_i915_dp_delayed_cancel |  |
| 172 | `read_pps_regs` | found | display/dp-sink.c:i915_edp_read_pps_regs |  |
| 185 | `snapshot_ownership` | found | display/dp-sink.c:i915_edp_snapshot_ownership |  |
| 207 | `apply_panel_vbt` | found | display/dp-sink.c:i915_edp_apply_panel_vbt |  |
| 219 | `fail` | found | display/dp-sink.c:i915_edp_fail |  |
| 229 | `parity_edp_begin` | found | display/dp-sink.c:drv_i915_edp_begin |  |
| 326 | `parity_edp_init_late` | found | display/dp-sink.c:drv_i915_edp_init_late |  |
| 346 | `parity_edp_work_run` | found | display/dp-sink.c:drv_i915_edp_work_run |  |
| 356 | `parity_edp_snapshot` | found | display/dp-sink.c:drv_i915_edp_snapshot |  |
| 362 | `parity_edp_end` | found | display/dp-sink.c:drv_i915_edp_end |  |
| 388 | `parity_edp_dpcd_read` | found | display/dp-sink.c:drv_i915_edp_dpcd_read |  |
| 395 | `parity_edp_dpcd_write` | found | display/dp-sink.c:drv_i915_edp_dpcd_write |  |
| 402 | `parity_edp_read_dpcd_caps` | found | display/dp-sink.c:drv_i915_edp_read_dpcd_caps |  |
| 410 | `parity_edp_panel_op` | found | display/dp-sink.c:drv_i915_edp_panel_op |  |

#### `parity/dram_bw.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 47 | `zero_bytes` | found | display/watermark.c (inlined as memset) |  |
| 57 | `parity_dram_decode` | found | display/watermark.c:drv_i915_dram_decode |  |
| 85 | `parity_dram_detect` | found | display/watermark.c:drv_i915_dram_detect |  |
| 108 | `parity_icl_get_qgv_points` | found | display/watermark.c:i915_icl_get_qgv_points |  |
| 184 | `parity_sagv_max_dclk` | found | display/watermark.c:i915_sagv_max_dclk |  |
| 195 | `parity_bw_init_hw` | found | display/watermark.c:drv_i915_bw_init_hw |  |

#### `parity/driver_probe.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 26 | `rmw` | found | display/hotplug.c:drv_i915_hpd_rmw |  |
| 37 | `parity_intel_ddi_hpd_pin` | found | display/hotplug.c:drv_i915_ddi_hpd_pin |  |
| 48 | `gen11_tc_hotplug` | found | display/hotplug.c:i915_gen11_tc_hotplug |  |
| 49 | `gen11_tbt_hotplug` | found | display/hotplug.c:i915_gen11_tbt_hotplug |  |
| 50 | `gen11_hotplug_ctl_enable` | found | display/hotplug.c:i915_gen11_hotplug_ctl_enable |  |
| 51 | `sde_ddi_hotplug_icp` | found | display/hotplug.c:i915_sde_ddi_hotplug_icp |  |
| 52 | `sde_tc_hotplug_icp` | found | display/hotplug.c:i915_sde_tc_hotplug_icp |  |
| 53 | `shotplug_ctl_ddi_hpd_enable` | found | display/hotplug.c:i915_shotplug_ctl_ddi_hpd_enable |  |
| 54 | `icp_tc_hpd_enable` | found | display/hotplug.c:i915_icp_tc_hpd_enable |  |
| 56 | `is_tc_pin` | found | display/hotplug.c:i915_is_tc_pin |  |
| 57 | `is_ddi_pin` | found | display/hotplug.c:i915_is_ddi_pin |  |
| 60 | `parity_intel_hpd_init_pins` | found | display/hotplug.c:drv_i915_hpd_init_pins |  |
| 82 | `hpd_irqs` | found | display/hotplug.c:i915_hpd_irqs |  |
| 102 | `hotplug_mask` | found | display/hotplug.c:i915_hotplug_mask |  |
| 114 | `hotplug_enables` | found | display/hotplug.c:i915_hotplug_enables |  |
| 127 | `gen11_hpd_irq_setup` | found | display/hotplug.c:i915_gen11_hpd_irq_setup |  |
| 185 | `parity_intel_hpd_irq_setup` | found | display/hotplug.c:drv_i915_hpd_irq_setup |  |
| 193 | `parity_intel_hpd_init` | found | display/hotplug.c:drv_i915_hpd_init |  |
| 214 | `parity_intel_hpd_poll_disable` | found | display/hotplug.c:drv_i915_hpd_poll_disable |  |
| 232 | `parity_skl_watermark_ipc_init` | found | display/display.c:i915_skl_watermark_ipc_init |  |
| 247 | `parity_intel_display_driver_probe` | found | display/display.c:i915_display_driver_probe |  |
| 297 | `parity_intel_power_domains_verify_state` | found | display/power.c:drv_i915_power_domains_verify_state |  |
| 318 | `wells_on` | found | display/power.c:i915_wells_on |  |
| 329 | `parity_intel_power_domains_enable` | found | display/power.c:drv_i915_power_domains_enable |  |
| 345 | `parity_intel_power_domains_disable` | found | display/power.c:drv_i915_power_domains_disable |  |
| 357 | `parity_i915_driver_register` | found | display/display.c:i915_driver_register | match by stem |
| 403 | `parity_i915_driver_unregister` | found | display/display.c:i915_driver_unregister | match by stem |

#### `parity/drm_device.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 27 | `parity_drm_vblank_test_fail_worker_at` | test | T4a | test hook [s4 §3/§8](i915-rebuild-s4.md) L247-249, L377-379 (g_vblank_worker_fail_pipe); caller ktest.c |
| 33 | `vblank_worker_should_fail` | test | T4a | test hook [s4 §3/§8](i915-rebuild-s4.md) L247-249, L377-379 |
| 44 | `parity_drm_dev_init` | found | display/display.c:drv_i915_drm_dev_init |  |
| 67 | `parity_drmm_add_action_or_reset` | found | display/display.c:drv_i915_drmm_add_action_or_reset |  |
| 85 | `parity_drm_dev_fini` | found | display/display.c:drv_i915_drm_dev_fini |  |
| 107 | `parity_drm_vblank_crtc_cleanup` | found | display/display.c:i915_drm_vblank_crtc_cleanup |  |
| 125 | `parity_drm_vblank_init` | found | display/display.c:drv_i915_drm_vblank_init |  |

#### `parity/eu_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 50 | `emit` | found | display/vbt.c:drv_i915_vbt_emit | ledger said test; kept in production (e.g. [s4 §4](i915-rebuild-s4.md) L278) |
| 61 | `emit_pc` | test | T4a | S5 T4a (destination not present yet) |
| 74 | `emit_marker` | test-moved | tests/execution/eu-test.c:i915_eu_emit_marker | S5 T4a |
| 83 | `emit_copy` | test-moved | tests/execution/eu-test.c:i915_eu_emit_copy | S5 T4a |
| 93 | `emit_sba` | test | T4a | S5 T4a (destination not present yet) |
| 121 | `parity_eu_test_build_batch` | test-moved | tests/execution/eu-test.c:drv_i915_test_eu_build_batch | S5 T4a |
| 218 | `parity_eu_batch_check_pipeline_select` | test | T4a | S5 T4a (destination not present yet) |
| 243 | `fnv1a64` | test-moved | tests/execution/eu-test.c:drv_i915_test_fnv1a64 | S5 T4a |
| 256 | `fail` | found | display/dp-sink.c:i915_edp_fail | ledger said test; kept in production (e.g. [s4 §4](i915-rebuild-s4.md) L278) |
| 268 | `retired` | test-moved | tests/execution/eu-test.c:i915_eu_retired | S5 T4a |
| 274 | `wait_retired` | test-moved | tests/execution/eu-test.c:drv_i915_test_eu_wait_retired | S5 T4a |
| 299 | `eu_build_request` | test | T4a | S5 T4a (destination not present yet) |
| 343 | `eu_park` | test | T4a | S5 T4a (destination not present yet) |
| 374 | `eu_log_record` | test-moved | src/drivers/gpu/i915/tests/execution/eu-test.c:drv_i915_test_eu_log_record (function containing the matched text) | S5 T4a |
| 396 | `eu_hang_dump_reset` | test | T4a | S5 T4a (destination not present yet) |
| 420 | `parity_eu_test_run` | test | T4a | S5 T4a (destination not present yet) |
| 594 | `eu_reset_markers` | test-moved | tests/execution/eu-test.c:i915_eu_reset_markers | S5 T4a |
| 610 | `parity_eu_test_repeat` | test-moved | src/drivers/gpu/i915/tests/execution/eu-test.c:drv_i915_test_eu_repeat (function containing the matched text) | S5 T4a |
| 742 | `eu_scrub_fixture_ptes` | test | T4a | S5 T4a (destination not present yet) |
| 754 | `parity_eu_test_release` | test-moved | tests/execution/eu-test.c:drv_i915_test_eu_release | S5 T4a |
| 788 | `parity_draw_batch_check_pipeline_select` | test | T4a | S5 T4a (destination not present yet) |
| 800 | `parity_draw_test_run` | test | T4a | S5 T4a (destination not present yet) |
| 943 | `parity_draw_test_release` | test | T4a | S5 T4a (destination not present yet) |
| 960 | `parity_tex_test_run` | test | T4a | S5 T4a (destination not present yet) |
| 1134 | `parity_tex_test_release` | test | T4a | S5 T4a (destination not present yet) |
| 1163 | `parity_fhd_va_layout` | test | T4a | S5 T4a (destination not present yet) |
| 1182 | `parity_fhd_render_verify` | test | T4a | S5 T4a (destination not present yet) |
| 1203 | `parity_fhd_render_run` | test | T4a | S5 T4a (destination not present yet) |
| 1211 | `parity_fhd_render_run_ex` | test | T4a | S5 T4a (destination not present yet) |
| 1414 | `fhd_maps` | test | T4a | S5 T4a (destination not present yet) |
| 1427 | `parity_fhd_render_release` | test | T4a | S5 T4a (destination not present yet) |
| 1473 | `parity_fhd_render_keep` | test | T4a | S5 T4a (destination not present yet) |
| 1486 | `parity_fhd_rt_map` | test | T4a | S5 T4a (destination not present yet) |
| 1524 | `parity_fhd_rt_unmap` | test | T4a | S5 T4a (destination not present yet) |
| 1560 | `t3_upload` | test | T4a | S5 T4a (destination not present yet) |
| 1570 | `t3_tex_diff` | test | T4a | S5 T4a (destination not present yet) |
| 1591 | `t3_run_plan` | test | T4a | S5 T4a (destination not present yet) |
| 1808 | `parity_t3_test_run` | test | T4a | S5 T4a (destination not present yet) |
| 1833 | `parity_bl_test_run` | test | T4a | S5 T4a (destination not present yet) |
| 1851 | `parity_t3_test_release` | test | T4a | S5 T4a (destination not present yet) |
| 1878 | `r1_write_c1_state` | test | T4a | S5 T4a (destination not present yet) |
| 1895 | `parity_r1_test_run` | test | T4a | S5 T4a (destination not present yet) |
| 2109 | `parity_r1_test_release` | test | T4a | S5 T4a (destination not present yet) |
| 2154 | `mcr_read_steered` | test-moved | tests/execution/eu-test.c:i915_eu_mcr_read_steered | S5 T4a |
| 2171 | `parity_mcr_probe_wa` | test-moved | tests/execution/eu-test.c:drv_i915_test_mcr_probe_wa | S5 T4a |

#### `parity/gt_defaults.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 17 | `fail` | found | display/dp-sink.c:i915_edp_fail |  |
| 27 | `parity_engines_record_defaults_submit` | found | defaults.c:i915_defaults_submit_engine (function containing the matched text) | match by string |
| 95 | `retired` | found | defaults.c:i915_defaults_retired | match by substr |
| 102 | `parity_engines_record_defaults_poll` | found | defaults.c:i915_defaults_poll (function containing the matched text) | match by string |
| 169 | `parity_engines_record_defaults_finish` | found | defaults.c:i915_defaults_finish (function containing the matched text) | match by string |
| 196 | `release_contexts` | found | defaults.c:i915_defaults_release_contexts | match by substr |
| 210 | `parity_engines_defaults_release` | found | defaults.c:drv_i915_engines_defaults_release |  |
| 227 | `parity_engine_dump` | found | engine.c:drv_i915_engine_dump |  |
| 265 | `parity_engines_record_defaults` | found | defaults.c:drv_i915_engines_record_defaults |  |

#### `parity/gt_engine.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 23 | `parity_engine_setup_common` | found | engine.c:drv_i915_engine_setup_common |  |
| 69 | `parity_execlists_submission_setup` | found | submit.c:drv_i915_execlists_submission_setup |  |
| 103 | `wr` | found | submit.c (inlined: write + ge->enable_writes++) |  |
| 111 | `enable_error_interrupt` | found | submit.c:i915_enable_error_interrupt |  |
| 137 | `parity_execlists_enable` | found | submit.c:drv_i915_execlists_enable |  |
| 164 | `parity_execlists_reset_csb_pointers` | found | submit.c:drv_i915_execlists_reset_csb_pointers |  |
| 213 | `parity_engine_release` | found | engine.c:drv_i915_engine_release |  |
| 231 | `parity_ring_set_paused` | found | submit.c:i915_ring_set_paused |  |
| 241 | `parity_engine_stop_cs` | found | engine.c:drv_i915_engine_stop_cs |  |
| 286 | `msg_idle_reg` | found | engine.c:i915_msg_idle_reg |  |
| 299 | `parity_engine_wait_for_pending_mi_fw` | found | engine.c:drv_i915_engine_wait_for_pending_mi_fw |  |
| 327 | `parity_execlists_reset_prepare` | found | submit.c:drv_i915_execlists_reset_prepare |  |

#### `parity/gt_init_base.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 70 | `wa_add_entry` | found | workarounds.c:i915_wa_add_entry |  |
| 103 | `parity_wa_write_or` | found | workarounds.c:drv_i915_wa_write_or |  |
| 111 | `parity_wa_write_clr_set` | found | workarounds.c:drv_i915_wa_write_clr_set |  |
| 119 | `parity_wa_write` | found | workarounds.c:drv_i915_wa_write_or | match by substr |
| 133 | `parity_wa_masked_en` | found | workarounds.c:drv_i915_wa_masked_en |  |
| 141 | `parity_wa_masked_dis` | retired |  | unused; [s1-reports](i915-rebuild-s1-reports.md) L45 |
| 149 | `parity_wa_masked_field_set` | found | workarounds.c:drv_i915_wa_masked_field_set |  |
| 158 | `parity_wa_add_no_verify` | found | workarounds.c:drv_i915_wa_add_no_verify |  |
| 165 | `parity_wa_list_dump` | found | workarounds.c:drv_i915_wa_list_dump |  |
| 188 | `parity_wa_list_apply` | found | workarounds.c:drv_i915_wa_list_apply |  |
| 244 | `parity_engine_apply_whitelist` | found | workarounds.c:drv_i915_engine_apply_whitelist |  |
| 320 | `parity_get_mocs_settings` | found | workarounds.c:drv_i915_get_mocs_settings |  |
| 376 | `parity_intel_mocs_init` | found | workarounds.c:drv_i915_mocs_init |  |
| 398 | `parity_init_l3cc_table` | found | workarounds.c:drv_i915_init_l3cc_table |  |
| 420 | `parity_tgl_setup_private_ppat` | found | workarounds.c:drv_i915_tgl_setup_private_ppat |  |
| 438 | `parity_intel_rc6_init` | found | gt-power.c:drv_i915_rc6_init |  |
| 455 | `parity_gen11_rc6_enable` | found | gt-power.c:drv_i915_gen11_rc6_enable |  |
| 523 | `parity_intel_rps_init` | found | gt-power.c:drv_i915_rps_init |  |
| 558 | `parity_intel_rps_enable` | found | gt-power.c:drv_i915_rps_enable |  |
| 577 | `parity_gt_init_tables` | found | workarounds.c:drv_i915_gt_init_tables |  |
| 607 | `parity_gt_init_hw_core` | found | workarounds.c:drv_i915_gt_init_hw_core |  |
| 643 | `parity_engine_apply_resume_wa` | found | workarounds.c:drv_i915_engine_apply_resume_wa |  |
| 656 | `parity_intel_rc6_sanitize` | found | gt-power.c:drv_i915_rc6_sanitize |  |
| 669 | `parity_intel_rps_sanitize` | found | gt-power.c:drv_i915_rps_sanitize |  |

#### `parity/gt_lrc.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 21 | `hweight8v` | found | context.c:i915_hweight8 |  |
| 30 | `parity_gen12_rcs_offsets_ref` | test | T4a | test accessor; only caller ktest.c:4615 |
| 36 | `parity_gen12_xcs_offsets_ref` | test | T4a | test accessor; only caller ktest.c |
| 44 | `parity_lrc_state_size` | found | context.c:i915_lrc_state_size |  |
| 62 | `parity_lrc_set_offsets` | found | context.c:i915_lrc_set_offsets |  |
| 117 | `parity_sseu_make_rpcs` | found | context.c:i915_sseu_make_rpcs |  |
| 142 | `parity_lrc_alloc` | found | context.c:drv_i915_lrc_alloc |  |
| 202 | `init_common_regs` | found | context.c:i915_lrc_init_common_regs | match by substr |
| 223 | `init_ppgtt_regs` | found | context.c:i915_lrc_init_ppgtt_regs | match by substr |
| 238 | `reset_stop_ring` | found | context.c:i915_lrc_reset_stop_ring | match by substr |
| 248 | `parity_lrc_init_regs` | found | context.c:i915_lrc_init_regs |  |
| 275 | `parity_lrc_init_state` | found | context.c:drv_i915_lrc_init_state |  |
| 305 | `parity_lrc_reset` | found | context.c:drv_i915_lrc_reset |  |
| 322 | `parity_lrc_aux_inv_reg` | found | context.c:drv_i915_lrc_aux_inv_reg |  |
| 336 | `context_wabb` | found | context.c:i915_lrc_wa_bb |  |
| 347 | `lrc_indirect_bb` | found | context.c:i915_lrc_indirect_bb |  |
| 358 | `emit_timestamp_wa` | found | context.c:i915_lrc_emit_timestamp_wa | match by substr |
| 384 | `emit_cmd_buf_wa` | found | context.c:i915_lrc_emit_cmd_buf_wa | match by substr |
| 404 | `emit_restore_scratch` | found | context.c:i915_lrc_emit_restore_scratch | match by substr |
| 423 | `emit_aux_table_inv` | found | context.c (inlined call to request.c:drv_i915_gen12_emit_aux_table_inv, context.c:806) |  |
| 434 | `emit_invalidate_state_cache` | found | context.c:i915_lrc_emit_invalidate_state_cache | match by substr |
| 450 | `setup_predicate_disable_wa` | found | context.c:i915_lrc_setup_predicate_disable_wa | match by substr |
| 469 | `setup_indirect_ctx_bb` | found | context.c:i915_lrc_setup_indirect_ctx_bb | match by substr |
| 505 | `setup_per_ctx_bb` | found | context.c:i915_lrc_setup_per_ctx_bb | match by substr |
| 527 | `parity_lrc_descriptor` | found | context.c:i915_lrc_descriptor |  |
| 541 | `parity_lrc_update_regs` | found | context.c:drv_i915_lrc_update_regs |  |
| 584 | `parity_lrc_keep` | test | T4a | only caller eu_test.c:1482 |
| 595 | `parity_lrc_release` | found | context.c:drv_i915_lrc_release |  |

#### `parity/gt_mem.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 31 | `parity_gen12_ppgtt_pte_encode` | found | ppgtt.c:drv_i915_gen12_ppgtt_pte_encode |  |
| 47 | `parity_gen8_pde_encode` | found | ppgtt.c:drv_i915_gen8_pde_encode |  |
| 54 | `parity_gen8_pde_encode_cached` | found | ppgtt.c:drv_i915_gen8_pde_encode_cached |  |
| 63 | `parity_gt_mem_init` | found | memory.c:drv_i915_gt_mem_init |  |
| 93 | `parity_gt_mem_fini` | found | memory.c:drv_i915_gt_mem_fini |  |
| 118 | `parity_gt_object_create` | found | memory.c:drv_i915_gt_object_create |  |
| 196 | `parity_gt_object_destroy` | found | memory.c:drv_i915_gt_object_destroy |  |
| 223 | `parity_gt_object_page_dma` | found | memory.c:drv_i915_gt_object_page_dma |  |
| 265 | `window_bit` | found | ggtt.c:i915_ggtt_window_bit | match by substr |
| 271 | `window_set` | found | ggtt.c:i915_ggtt_window_set | match by substr |
| 281 | `window_alloc` | found | ggtt.c:i915_ggtt_window_alloc | match by substr |
| 311 | `ggtt_write_pte` | found | ggtt.c:i915_ggtt_write_pte |  |
| 318 | `parity_gt_ggtt_flush` | found | ggtt.c:drv_i915_gt_ggtt_flush |  |
| 332 | `parity_gt_ggtt_bind` | found | ggtt.c:drv_i915_gt_ggtt_bind |  |
| 381 | `parity_gt_ggtt_unbind` | found | ggtt.c:drv_i915_gt_ggtt_unbind |  |
| 407 | `parity_gt_ggtt_read_pte` | found | ggtt.c:drv_i915_gt_ggtt_read_pte |  |
| 416 | `parity_gt_display_window_init` | found | ggtt.c:drv_i915_gt_display_window_init |  |
| 437 | `display_bit` | found | ggtt.c:i915_ggtt_display_bit | match by substr |
| 443 | `display_set` | found | ggtt.c:i915_ggtt_display_set | match by substr |
| 456 | `parity_gt_display_bind` | found | ggtt.c:drv_i915_gt_display_bind |  |
| 547 | `parity_gt_display_bind_foreign` | found | ggtt.c:drv_i915_gt_display_bind_foreign |  |
| 601 | `parity_gt_display_unbind_foreign` | found | ggtt.c:drv_i915_gt_display_unbind_foreign |  |
| 620 | `parity_gt_display_unbind` | found | ggtt.c:drv_i915_gt_display_unbind |  |
| 649 | `parity_gt_init_scratch` | found | ggtt.c:drv_i915_gt_init_scratch |  |
| 677 | `parity_gt_clflush` | found | memory.c:drv_i915_gt_clflush |  |
| 694 | `fill_px` | found | ppgtt.c:i915_gt_fill_px |  |
| 705 | `parity_gt_ppgtt_create` | found | ppgtt.c:drv_i915_gt_ppgtt_create |  |
| 787 | `parity_gt_ppgtt_destroy` | found | ppgtt.c:drv_i915_gt_ppgtt_destroy |  |
| 820 | `pd_range` | found | ppgtt.c:i915_gt_ppgtt_pd_range | match by substr |
| 833 | `pt_count` | found | ppgtt.c:i915_gt_ppgtt_pt_count | match by substr |
| 841 | `child_of` | found | ppgtt.c:i915_gt_ppgtt_child_of | match by substr |
| 853 | `alloc_level` | found | ppgtt.c:i915_gt_ppgtt_alloc_level | match by substr |
| 904 | `parity_gt_ppgtt_alloc_range` | found | ppgtt.c:drv_i915_gt_ppgtt_alloc_range |  |
| 918 | `foreach_level` | found | ppgtt.c:i915_gt_ppgtt_foreach_level | match by substr |
| 943 | `parity_gt_ppgtt_foreach_pt` | found | ppgtt.c:drv_i915_gt_ppgtt_foreach_pt |  |
| 955 | `parity_gt_ppgtt_insert_page` | found | ppgtt.c:drv_i915_gt_ppgtt_insert_page |  |
| 981 | `parity_gt_ppgtt_insert_scratch` | found | ppgtt.c:drv_i915_gt_ppgtt_insert_scratch |  |
| 1006 | `table_by_dma` | test | T4a | helper of parity_gt_ppgtt_walk, moved to tests [s1-reports](i915-rebuild-s1-reports.md) L54 |
| 1017 | `parity_gt_ppgtt_walk` | test | T4a | tests [s1-reports](i915-rebuild-s1-reports.md) L54; callers probe.c test blocks, ktest.c, eu_test.c, lcdg_ktest.c |

#### `parity/gt_migrate.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 14 | `fail` | found | display/dp-sink.c:i915_edp_fail |  |
| 25 | `insert_pte` | found | migrate.c:i915_migrate_fail (function containing the matched text) | match by string |
| 42 | `first_copy_engine` | found | migrate.c:i915_migrate_first_copy_engine | match by substr |
| 56 | `parity_intel_migrate_init` | found | migrate.c:drv_i915_migrate_init |  |
| 130 | `parity_intel_migrate_fini` | found | migrate.c:drv_i915_migrate_fini |  |

#### `parity/gt_mmio.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 69 | `hweight16v` | found | device-info.c:i915_hweight16 |  |
| 78 | `hweight32v` | found | device-info.c:i915_hweight32 |  |
| 89 | `gen11_get_crystal_clock_freq` | found | device-info.c:i915_crystal_clock_frequency |  |
| 107 | `read_reference_ts_freq` | found | device-info.c:i915_reference_timestamp_frequency |  |
| 122 | `parity_gen11_read_clock_frequency` | found | device-info.c:i915_read_clock_frequency | match by tokens |
| 155 | `parity_gen11_compute_sseu_info` | found | device-info.c:i915_compute_sseu_info | match by tokens |
| 183 | `parity_gen12_sseu_info_init` | found | device-info.c:i915_sseu_info_init | match by tokens |
| 234 | `parity_intel_engine_context_size` | found | device-info.c:i915_engine_context_size |  |
| 256 | `parity_engine_mask_apply_media_fuses` | found | device-info.c:i915_engine_mask_apply_media_fuses |  |
| 329 | `get_reset_domain` | found | device-info.c:i915_engine_reset_domain |  |
| 342 | `setup_engine_capabilities` | found | device-info.c:i915_engine_setup_capabilities | match by tokens |
| 358 | `parity_intel_gt_check_and_clear_faults` | found | device-info.c:i915_check_and_clear_faults | match by stem |
| 412 | `parity_intel_gt_init_mmio` | found | device-info.c:drv_i915_gt_init_mmio |  |

#### `parity/gt_request.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 19 | `parity_ring_begin` | found | request.c:drv_i915_ring_begin |  |
| 54 | `parity_ring_advance` | found | request.c:drv_i915_ring_advance |  |
| 66 | `parity_gen12_emit_aux_table_inv` | found | request.c:drv_i915_gen12_emit_aux_table_inv |  |
| 88 | `preparser_disable` | found | request.c:i915_preparser_disable |  |
| 95 | `emit_pipe_control` | found | request.c:i915_emit_pipe_control |  |
| 110 | `emit_flush_rcs` | found | request.c:i915_emit_flush_rcs |  |
| 179 | `emit_flush_xcs` | found | request.c:i915_emit_flush_xcs |  |
| 235 | `parity_emit_flush` | found | request.c:i915_emit_flush |  |
| 247 | `parity_emit_ctx_wa` | found | request.c:drv_i915_emit_ctx_wa |  |
| 297 | `parity_request_create` | found | request.c:drv_i915_request_create |  |
| 326 | `emit_fini_breadcrumb_tail` | found | request.c:i915_emit_fini_breadcrumb_tail |  |
| 355 | `parity_request_add` | found | request.c:drv_i915_request_add |  |

#### `parity/gt_resume.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `parity_intel_engines_init` | found | engine.c:drv_i915_engines_init |  |
| 66 | `parity_intel_engines_release` | found | engine.c:drv_i915_engines_release |  |
| 82 | `execlists_sanitize` | found | engine.c:i915_execlists_sanitize |  |
| 100 | `parity_intel_gt_resume` | found | engine.c:drv_i915_gt_resume |  |

#### `parity/gt_submit.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 21 | `parity_execlists_init` | found | submit.c:drv_i915_execlists_init |  |
| 34 | `parity_gen12_csb_parse` | found | submit.c:i915_gen12_csb_parse |  |
| 56 | `parity_request_completed` | found | request.c:drv_i915_request_completed |  |
| 67 | `schedule_in` | found | submit.c:i915_schedule_in |  |
| 90 | `schedule_out` | found | submit.c:i915_schedule_out |  |
| 103 | `update_context` | found | submit.c:i915_update_context |  |
| 141 | `write_desc` | found | submit.c:i915_write_desc |  |
| 150 | `parity_execlists_submit` | found | submit.c:drv_i915_execlists_submit |  |
| 190 | `csb_read` | found | submit.c:i915_csb_read |  |
| 237 | `parity_execlists_process_csb` | found | submit.c:drv_i915_execlists_process_csb |  |

#### `parity/gt_tlb.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 27 | `parity_gt_tlb_engine_reg` | found | tlb.c:i915_tlb_engine_register | match by substr |
| 51 | `parity_gt_invalidate_tlb_full` | found | tlb.c:drv_i915_gt_invalidate_tlb_full |  |

#### `parity/gt_verify_wa.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 25 | `parity_gen12_mcr_range` | found | verify-workarounds.c:drv_i915_gen12_mcr_range |  |
| 36 | `fail` | found | display/dp-sink.c:i915_edp_fail |  |
| 46 | `parity_wa_list_srm` | found | verify-workarounds.c:drv_i915_wa_list_srm |  |
| 81 | `parity_wa_list_check` | found | verify-workarounds.c:drv_i915_wa_list_check |  |
| 123 | `parity_engine_verify_wa_submit` | found | verify-workarounds.c:drv_i915_engine_verify_wa_submit |  |
| 183 | `retired` | found | verify-workarounds.c:i915_verify_wa_retired | match by substr |
| 190 | `parity_engine_verify_wa_poll` | found | verify-workarounds.c:drv_i915_engine_verify_wa_poll |  |
| 219 | `parity_engine_verify_wa_park` | found | verify-workarounds.c:drv_i915_engine_verify_wa_park |  |
| 258 | `wait_engine` | found | verify-workarounds.c:i915_verify_wa_wait_engine | match by substr |
| 280 | `parity_engines_verify_workarounds` | found | verify-workarounds.c:drv_i915_engines_verify_workarounds |  |
| 352 | `parity_engines_verify_wa_release` | found | verify-workarounds.c:drv_i915_engines_verify_wa_release |  |

#### `parity/gt_wa_adlp.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 90 | `icl_wa_init_mcr` | found | workarounds.c:i915_icl_wa_init_mcr |  |
| 106 | `wa_14011060649` | found | workarounds.c:i915_wa_14011060649 |  |
| 124 | `parity_gt_init_workarounds_adlp` | found | workarounds.c:drv_i915_gt_init_workarounds_adlp |  |
| 153 | `parity_gt_init_workarounds` | found | workarounds.c:drv_i915_engine_init_workarounds | match by substr |
| 162 | `parity_engine_init_workarounds` | found | workarounds.c:drv_i915_engine_init_workarounds |  |
| 232 | `parity_engine_init_ctx_wa` | found | workarounds.c:drv_i915_engine_init_ctx_wa |  |
| 291 | `whitelist_reg_ext` | found | workarounds.c:i915_whitelist_reg_ext |  |
| 306 | `parity_engine_init_whitelist` | found | workarounds.c:drv_i915_engine_init_whitelist |  |

#### `parity/irq.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 163 | `wr` | found | irq.c:i915_irq_write, display/interrupts.c:i915_display_irq_write |  |
| 171 | `rd` | found | irq.c / display/interrupts.c (inlined as drv_i915_read32) |  |
| 178 | `gen3_irq_reset` | found | irq.c:drv_i915_gen3_irq_reset |  |
| 194 | `gen3_assert_iir_is_zero` | found | irq.c:drv_i915_gen3_assert_iir_is_zero |  |
| 211 | `gen3_irq_init` | found | irq.c:drv_i915_gen3_irq_init |  |
| 222 | `gen11_master_intr_disable` | found | irq.c:i915_master_intr_disable | match by tokens |
| 233 | `gen11_master_intr_enable` | found | irq.c:i915_master_intr_enable | match by tokens |
| 239 | `pipe_power_on` | found | display/interrupts.c:i915_pipe_power_on |  |
| 246 | `transcoder_power_on` | found | display/interrupts.c:i915_transcoder_power_on |  |
| 255 | `parity_gen8_de_pipe_fault_mask` | found | display/interrupts.c:drv_i915_gen8_de_pipe_fault_mask |  |
| 269 | `parity_gen8_de_port_aux_mask` | found | display/interrupts.c:drv_i915_gen8_de_port_aux_mask |  |
| 300 | `parity_gen8_de_pipe_underrun_mask` | found | display/interrupts.c:drv_i915_gen8_de_pipe_underrun_mask |  |
| 311 | `parity_gen8_de_pipe_flip_done_mask` | found | display/interrupts.c:drv_i915_gen8_de_pipe_flip_done_mask |  |
| 322 | `parity_gen11_gt_irq_reset` | found | irq.c:drv_i915_gen11_gt_irq_reset |  |
| 349 | `parity_gen11_gt_irq_postinstall` | found | irq.c:drv_i915_gen11_gt_irq_postinstall |  |
| 398 | `parity_gen11_display_irq_reset` | found | display/interrupts.c:drv_i915_gen11_display_irq_reset |  |
| 439 | `icp_irq_postinstall` | found | display/interrupts.c:i915_icp_irq_postinstall |  |
| 447 | `gen8_de_irq_postinstall` | found | display/interrupts.c:i915_gen8_de_irq_postinstall |  |
| 527 | `parity_gen11_de_irq_postinstall` | found | display/interrupts.c:drv_i915_gen11_de_irq_postinstall |  |
| 545 | `gen11_gt_engine_identity` | found | irq.c:i915_gt_engine_identity | match by tokens |
| 584 | `gt_engine_irq` | found | irq.c:i915_gt_engine_irq |  |
| 597 | `gen11_gt_identity_handler` | found | irq.c:i915_gt_identity_handler | match by tokens |
| 640 | `gen11_gt_bank_handler` | found | irq.c:i915_gt_bank_handler | match by tokens |
| 661 | `parity_gen11_gt_irq_handler` | found | irq.c:drv_i915_gen11_gt_irq_handler |  |
| 680 | `gen8_de_irq_handler` | found | display/interrupts.c:i915_gen8_de_irq_handler |  |
| 793 | `parity_gen11_display_irq_handler` | found | display/interrupts.c:drv_i915_gen11_display_irq_handler |  |
| 807 | `parity_intel_irq_reset` | found | irq.c:drv_i915_irq_reset |  |
| 819 | `parity_intel_irq_postinstall` | found | irq.c:drv_i915_irq_postinstall |  |
| 842 | `gen11_irq_handler_body` | found | irq.c:i915_irq_handler_body | match by tokens |
| 906 | `gen11_irq_handler` | found | irq.c:i915_irq_handler | match by tokens |
| 918 | `parity_irq_vblank_init` | found | display/interrupts.c:drv_i915_irq_vblank_init |  |
| 932 | `parity_intel_synchronize_irq` | found | irq.c:drv_i915_synchronize_irq |  |
| 958 | `parity_gen8_irq_power_well_post_enable` | found | display/interrupts.c:drv_i915_gen8_irq_power_well_post_enable |  |
| 988 | `parity_irq_drain_pipes` | found | display/interrupts.c:drv_i915_irq_drain_pipes |  |
| 1021 | `parity_gen8_irq_power_well_pre_disable` | found | display/interrupts.c:drv_i915_gen8_irq_power_well_pre_disable |  |
| 1052 | `bdw_update_pipe_irq` | found | display/interrupts.c:i915_bdw_update_pipe_irq |  |
| 1081 | `bdw_enable_vblank_locked` | found | display/interrupts.c (inlined: i915_bdw_update_pipe_irq(.., GEN8_PIPE_VBLANK, GEN8_PIPE_VBLANK), L707) |  |
| 1088 | `bdw_disable_vblank_locked` | found | display/interrupts.c (inlined: i915_bdw_update_pipe_irq(.., GEN8_PIPE_VBLANK, 0), L748) |  |
| 1100 | `parity_drm_vblank_get` | found | display/interrupts.c:drv_i915_drm_vblank_get |  |
| 1121 | `parity_drm_vblank_put` | found | display/interrupts.c:drv_i915_drm_vblank_put |  |
| 1138 | `vbl_snapshot` | found | display/interrupts.c:i915_vblank_snapshot |  |
| 1152 | `parity_wait_vblank` | found | display/interrupts.c:drv_i915_wait_vblank |  |
| 1205 | `pw_irq_post_enable` | found | display/interrupts.c:i915_pw_irq_post_enable |  |
| 1211 | `pw_irq_pre_disable` | found | display/interrupts.c:i915_pw_irq_pre_disable |  |
| 1219 | `parity_intel_irq_install` | found | irq.c:drv_i915_irq_install |  |
| 1259 | `parity_intel_irq_uninstall` | found | irq.c:drv_i915_irq_uninstall |  |

#### `parity/ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 87 | `edp_ktest_check` | test | T4a | S5 T4a (destination not present yet) |
| 100 | `pd_async_test_queue` | test | T4a | S5 T4a (destination not present yet) |
| 112 | `pd_async_test_cancel` | test | T4a | S5 T4a (destination not present yet) |
| 127 | `deadline_ms` | test-moved | tests/execution/ktest.c:drv_i915_ktest_deadline_ms | S5 T4a |
| 136 | `msi_probe_handler` | test | T4a | S5 T4a (destination not present yet) |
| 188 | `ktest_vga_get` | test | T4a | S5 T4a (destination not present yet) |
| 189 | `ktest_vga_in8` | test | T4a | S5 T4a (destination not present yet) |
| 190 | `ktest_vga_out8` | test | T4a | S5 T4a (destination not present yet) |
| 191 | `ktest_vga_put` | test | T4a | S5 T4a (destination not present yet) |
| 201 | `pt_oneshot` | test | T4a | S5 T4a (destination not present yet) |
| 207 | `pt_thread_b` | test | T4a | S5 T4a (destination not present yet) |
| 219 | `pt_spin_until` | test | T4a | S5 T4a (destination not present yet) |
| 229 | `pt_thread_a` | test | T4a | S5 T4a (destination not present yet) |
| 251 | `sleep_corunner` | test-moved | tests/execution/ktest-sync.c:i915_ktest_sleep_corunner | S5 T4a |
| 261 | `pcode_time_ok` | test | T4a | S5 T4a (destination not present yet) |
| 271 | `pcode_time_fault` | test | T4a | S5 T4a (destination not present yet) |
| 293 | `dmc_bad_request` | test | T4a | S5 T4a (destination not present yet) |
| 304 | `dmc_fini_thread` | test | T4a | S5 T4a (destination not present yet) |
| 317 | `ktest_bridge_next` | test | T4a | S5 T4a (destination not present yet) |
| 330 | `ktest_fail_read` | test | T4a | S5 T4a (destination not present yet) |
| 343 | `fake_wt_record` | test | T4a | S5 T4a (destination not present yet) |
| 350 | `fake_wt_find` | test | T4a | S5 T4a (destination not present yet) |
| 361 | `fake_gen_get` | test-moved | tests/execution/ktest-sync.c:i915_fake_gen_get | S5 T4a |
| 372 | `fake_gen_set` | test-moved | tests/execution/ktest-sync.c:i915_fake_gen_set | S5 T4a |
| 382 | `fake_raw_read32` | test | T4a | S5 T4a (destination not present yet) |
| 423 | `fake_raw_write32` | test | T4a | S5 T4a (destination not present yet) |
| 508 | `fake_fw_request` | test | T4a | S5 T4a (destination not present yet) |
| 510 | `fake_fw_ack` | test | T4a | S5 T4a (destination not present yet) |
| 518 | `fake_mmio_open` | test | T4a | S5 T4a (destination not present yet) |
| 532 | `drm_test_action` | test | T4a | S5 T4a (destination not present yet) |
| 539 | `drm_zero_fixture` | test | T4a | S5 T4a (destination not present yet) |
| 555 | `oneshot_timer_cb` | test | T4a | S5 T4a (destination not present yet) |
| 568 | `completer_worker` | test-moved | tests/execution/ktest-sync.c:i915_ktest_completer_worker | S5 T4a |
| 575 | `spawn_detached` | test-moved | tests/execution/ktest-sync.c:i915_ktest_spawn_detached | S5 T4a |
| 589 | `spawn_on_cpu` | test | T4a | S5 T4a (destination not present yet) |
| 605 | `wq_requeue_fn` | test | T4a | S5 T4a (destination not present yet) |
| 622 | `cancel_worker_fn` | test-moved | tests/execution/ktest-sync.c:i915_ktest_cancel_worker_fn | S5 T4a |
| 632 | `canceller_thread` | test-moved | tests/execution/ktest-sync.c:i915_ktest_canceller_thread | S5 T4a |
| 653 | `xcpu_fn` | test-moved | tests/execution/ktest-sync.c:i915_ktest_xcpu_fn | S5 T4a |
| 665 | `bios_fpci_r8` | test | T4a | S5 T4a (destination not present yet) |
| 668 | `bios_fpci_r16` | test | T4a | S5 T4a (destination not present yet) |
| 673 | `fpci_subsys` | test | T4a | S5 T4a (destination not present yet) |
| 679 | `bios_fpci_r32` | test | T4a | S5 T4a (destination not present yet) |
| 680 | `bios_fpci_w8` | test | T4a | S5 T4a (destination not present yet) |
| 681 | `bios_fpci_w16` | test | T4a | S5 T4a (destination not present yet) |
| 682 | `bios_fpci_w32` | test | T4a | S5 T4a (destination not present yet) |
| 683 | `bios_fpci_alloc_msi` | test | T4a | S5 T4a (destination not present yet) |
| 684 | `bios_fpci_free_msi` | test | T4a | S5 T4a (destination not present yet) |
| 696 | `bios_make_vbt` | test | T4a | S5 T4a (destination not present yet) |
| 722 | `bios_zero` | test | T4a | S5 T4a (destination not present yet) |
| 732 | `rpm_test_resume` | test | T4a | S5 T4a (destination not present yet) |
| 733 | `rpm_test_suspend` | test | T4a | S5 T4a (destination not present yet) |
| 748 | `time_test_read` | test | T4a | S5 T4a (destination not present yet) |
| 762 | `pd_async_checks` | test | T4a | S5 T4a (destination not present yet) |
| 839 | `kvb_read_frame` | test | T4a | S5 T4a (destination not present yet) |
| 854 | `parity_sync_ktest` | test-moved | src/drivers/gpu/i915/tests/execution/ktest-sync.c:i915_ktest_workqueue (function containing the matched text) | S5 T4a |

#### `parity/lcd/drm_connector_status_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 42 | `drm_get_connector_status_name` | found | display/hotplug.c:i915_hpd_drm_get_connector_status_name |  |

#### `parity/lcd/drm_dp_bw_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 44 | `drm_dp_is_uhbr_rate` | found | display/dp.c:i915_drm_dp_is_uhbr_rate |  |
| 64 | `drm_dp_bw_channel_coding_efficiency` | found | display/dp.c:i915_drm_dp_bw_channel_coding_efficiency |  |

#### `parity/lcd/drm_dp_link_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 56 | `dp_lttpr_common_cap` | found | display/dp.c:i915_dp_lttpr_common_cap |  |
| 61 | `dp_lttpr_phy_cap` | found | display/dp.c:i915_dp_lttpr_phy_cap |  |
| 66 | `drm_dp_read_lttpr_regs` | found | display/dp.c:i915_drm_dp_read_lttpr_regs |  |
| 93 | `dp_link_status` | found | display/dp.c:i915_dp_link_status |  |
| 98 | `dp_get_lane_status` | found | display/dp.c:i915_dp_get_lane_status |  |
| 108 | `drm_dp_channel_eq_ok` | found | display/dp.c:drv_i915_drm_dp_channel_eq_ok |  |
| 127 | `drm_dp_clock_recovery_ok` | found | display/dp.c:drv_i915_drm_dp_clock_recovery_ok |  |
| 141 | `drm_dp_get_adjust_request_voltage` | found | display/dp.c:i915_drm_dp_get_adjust_request_voltage |  |
| 153 | `drm_dp_get_adjust_request_pre_emphasis` | found | display/dp.c:i915_drm_dp_get_adjust_request_pre_emphasis |  |
| 166 | `drm_dp_get_adjust_tx_ffe_preset` | found | display/dp.c:i915_drm_dp_get_adjust_tx_ffe_preset |  |
| 178 | `__8b10b_clock_recovery_delay_us` | found | display/dp.c:i915_8b10b_clock_recovery_delay_us (function containing the matched text) | match by string |
| 190 | `__8b10b_channel_eq_delay_us` | found | display/dp.c:i915_8b10b_clock_recovery_delay_us (function containing the matched text) | match by string |
| 202 | `__128b132b_channel_eq_delay_us` | found | display/dp.c:i915_8b10b_clock_recovery_delay_us (function containing the matched text) | match by string |
| 236 | `__read_delay` | found | display/dp.c:i915_read_delay (function containing the matched text) | match by string |
| 291 | `drm_dp_read_clock_recovery_delay` | found | display/dp.c:i915_drm_dp_read_clock_recovery_delay |  |
| 297 | `drm_dp_read_channel_eq_delay` | found | display/dp.c:i915_drm_dp_read_channel_eq_delay |  |
| 316 | `drm_dp_dpcd_read_phy_link_status` | found | display/dp.c:drv_i915_drm_dp_dpcd_read_phy_link_status |  |
| 366 | `drm_dp_lttpr_count` | found | display/dp.c:i915_drm_dp_lttpr_count |  |
| 390 | `drm_dp_lttpr_voltage_swing_level_3_supported` | found | display/dp.c:i915_drm_dp_lttpr_voltage_swing_level_3_supported |  |
| 405 | `drm_dp_lttpr_pre_emphasis_level_3_supported` | found | display/dp.c:i915_drm_dp_lttpr_pre_emphasis_level_3_supported |  |
| 422 | `drm_dp_read_lttpr_common_caps` | found | display/dp.c:i915_drm_dp_read_lttpr_common_caps |  |
| 442 | `drm_dp_read_lttpr_phy_caps` | found | display/dp.c:i915_drm_dp_read_lttpr_phy_caps |  |
| 462 | `drm_dp_phy_name` | found | display/dp.c:i915_drm_dp_phy_name |  |
| 483 | `drm_dp_link_rate_to_bw_code` | found | display/dp.c:i915_drm_dp_link_rate_to_bw_code |  |

#### `parity/lcd/drm_edid_mode_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 66 | `drm_mode_do_interlace_quirk` | found | display/edid.c:i915_drm_mode_do_interlace_quirk |  |
| 104 | `drm_mode_detailed` | found | display/edid.c:i915_drm_mode_detailed |  |

#### `parity/lcd/drm_modes_hv_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 51 | `drm_mode_copy` | found | display/edid.c:drv_i915_drm_mode_copy |  |
| 69 | `drm_mode_init` | found | display/edid.c:drv_i915_drm_mode_init |  |
| 84 | `drm_mode_get_hv_timing` | found | display/edid.c:drv_i915_drm_mode_get_hv_timing |  |

#### `parity/lcd/drm_modes_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 58 | `drm_mode_set_crtcinfo` | found | display/edid.c:drv_i915_drm_mode_set_crtcinfo |  |

#### `parity/lcd/drm_probe_detect_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 47 | `drm_helper_probe_detect_ctx` | found | display/hotplug.c:i915_drm_helper_probe_detect_ctx |  |
| 94 | `drm_helper_probe_detect` | found | display/hotplug.c:i915_hpd_drm_helper_probe_detect |  |

#### `parity/lcd/hpd_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 141 | `parity_hpd_queue_work` | found | display/hotplug-internal.h:i915_hpd_queue_work |  |
| 146 | `parity_hpd_queue_delayed_work` | found | display/hotplug-internal.h:i915_hpd_queue_delayed_work |  |
| 152 | `parity_hpd_mod_delayed_work` | found | display/hotplug-internal.h:i915_hpd_mod_delayed_work |  |
| 159 | `parity_hpd_cancel_work_sync` | found | display/hotplug-internal.h:i915_hpd_cancel_work_sync |  |
| 163 | `parity_hpd_cancel_delayed_work_sync` | found | display/hotplug-internal.h:i915_hpd_cancel_delayed_work_sync |  |
| 209 | `bxt_gmbus_clock_gating` | found | display/hotplug-internal.h:i915_bxt_gmbus_clock_gating |  |
| 210 | `pch_gmbus_clock_gating` | found | display/hotplug-internal.h:i915_pch_gmbus_clock_gating |  |
| 288 | `intel_encoder_is_dig_port` | found | display/hotplug-internal.h:i915_intel_encoder_is_dig_port |  |

#### `parity/lcd/hpd_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 34 | `make_edid` | test-moved | tests/display/hpd-ktest.c:i915_make_edid | S5 T4b |
| 54 | `ksleep_ticks` | test | T4b | S5 T4b (destination not present yet) |
| 60 | `wait_records` | test-moved | tests/display/hpd-ktest.c:i915_wait_records | S5 T4b |
| 76 | `set_encoder` | test-moved | tests/display/hpd-ktest.c:i915_set_encoder | S5 T4b |
| 86 | `parity_hpd_ktest` | test-moved | src/drivers/gpu/i915/tests/display/hpd-ktest.c:i915_start_tests (function containing the matched text) | S5 T4b |

#### `parity/lcd/intel_acpi_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 41 | `acpi_display_type` | found | display/opregion.c:i915_acpi_display_type |  |
| 82 | `intel_acpi_device_id_update` | found | display/opregion.c:drv_i915_acpi_device_id_update |  |

#### `parity/lcd/intel_atomic_plane_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 49 | `intel_adjusted_rate` | found | display/plane.c:drv_i915_adjusted_rate |  |
| 68 | `intel_plane_pixel_rate` | found | display/plane.c:drv_i915_plane_pixel_rate |  |
| 89 | `use_min_ddb` | found | display/plane.c:i915_use_min_ddb |  |
| 100 | `intel_plane_relative_data_rate` | found | display/plane.c:i915_plane_relative_data_rate |  |
| 144 | `intel_plane_data_rate` | found | display/plane.c:drv_i915_plane_data_rate |  |

#### `parity/lcd/intel_backlight_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 74 | `scale` | found | display/panel-backlight.c:i915_scale |  |
| 100 | `clamp_user_to_hw` | found | display/panel-backlight.c:i915_clamp_user_to_hw |  |
| 113 | `scale_hw_to_user` | found | display/panel-backlight.c:i915_scale_hw_to_user |  |
| 122 | `intel_backlight_invert_pwm_level` | found | display/panel-backlight.c:i915_backlight_invert_pwm_level |  |
| 140 | `intel_backlight_set_pwm_level` | found | display/panel-backlight.c:i915_backlight_set_pwm_level |  |
| 151 | `intel_backlight_level_to_pwm` | found | display/panel-backlight.c:drv_i915_backlight_level_to_pwm |  |
| 165 | `intel_backlight_level_from_pwm` | found | display/panel-backlight.c:drv_i915_backlight_level_from_pwm |  |
| 182 | `bxt_get_backlight` | found | display/panel-backlight.c:i915_bxt_get_backlight |  |
| 190 | `bxt_set_backlight` | found | display/panel-backlight.c:i915_bxt_set_backlight |  |
| 199 | `cnp_disable_backlight` | found | display/panel-backlight.c:i915_cnp_disable_backlight |  |
| 211 | `cnp_enable_backlight` | found | display/panel-backlight.c:i915_cnp_enable_backlight |  |
| 242 | `cnp_num_backlight_controllers` | found | display/panel-backlight.c:i915_cnp_num_backlight_controllers |  |
| 256 | `cnp_backlight_controller_is_valid` | found | display/panel-backlight.c:i915_cnp_backlight_controller_is_valid |  |
| 273 | `cnp_hz_to_pwm` | found | display/panel-backlight.c:i915_cnp_hz_to_pwm |  |
| 281 | `get_vbt_pwm_freq` | found | display/panel-backlight.c:i915_get_vbt_pwm_freq |  |
| 300 | `get_backlight_max_vbt` | found | display/panel-backlight.c:i915_get_backlight_max_vbt |  |
| 326 | `get_backlight_min_vbt` | found | display/panel-backlight.c:i915_get_backlight_min_vbt |  |
| 353 | `cnp_setup_backlight` | found | display/panel-backlight.c:i915_cnp_setup_backlight |  |
| 396 | `intel_pwm_get_backlight` | found | display/panel-backlight.c:i915_pwm_get_backlight |  |
| 404 | `intel_pwm_set_backlight` | found | display/panel-backlight.c:i915_pwm_set_backlight |  |
| 413 | `intel_pwm_enable_backlight` | found | display/panel-backlight.c:i915_pwm_enable_backlight |  |
| 423 | `intel_pwm_disable_backlight` | found | display/panel-backlight.c:i915_pwm_disable_backlight |  |
| 432 | `intel_pwm_setup_backlight` | found | display/panel-backlight.c:i915_pwm_setup_backlight |  |
| 449 | `__intel_backlight_enable` | found | display/panel-backlight.c:drv_i915_backlight_enable | match by tokens |
| 472 | `intel_backlight_enable` | found | display/panel-backlight.c:drv_i915_backlight_enable |  |
| 492 | `intel_backlight_disable` | found | display/panel-backlight.c:drv_i915_backlight_disable |  |
| 524 | `intel_panel_actually_set_backlight` | found | display/panel-backlight.c:i915_panel_actually_set_backlight |  |
| 537 | `scale_user_to_hw` | found | display/panel-backlight.c:i915_scale_user_to_hw |  |
| 547 | `intel_panel_set_backlight` | found | display/panel-backlight.c:i915_panel_set_backlight |  |
| 574 | `intel_backlight_set_acpi` | found | display/panel-backlight.c:drv_i915_backlight_set_acpi |  |

#### `parity/lcd/intel_bw_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 26 | `intel_bw_crtc_data_rate` | found | display/watermark.c:i915_intel_bw_crtc_data_rate |  |
| 51 | `intel_bw_crtc_min_cdclk` | found | display/watermark.c:i915_intel_bw_crtc_min_cdclk |  |
| 62 | `intel_bw_crtc_num_active_planes` | found | display/watermark.c:i915_intel_bw_crtc_num_active_planes |  |
| 71 | `intel_bw_crtc_update` | found | display/watermark.c:drv_i915_bw_crtc_update |  |

#### `parity/lcd/intel_cdclk_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 102 | `bxt_calc_cdclk` | found | display/clock.c:i915_bxt_calc_cdclk |  |
| 118 | `bxt_calc_cdclk_pll_vco` | found | display/clock.c:i915_bxt_calc_cdclk_pll_vco |  |
| 136 | `calc_voltage_level` | found | display/clock.c:i915_calc_voltage_level |  |
| 150 | `tgl_calc_voltage_level` | found | display/clock.c:i915_tgl_calc_voltage_level |  |
| 164 | `intel_pixel_rate_to_cdclk` | found | display/clock.c:i915_pixel_rate_to_cdclk |  |
| 182 | `intel_planes_min_cdclk` | found | display/clock.c:i915_planes_min_cdclk |  |
| 195 | `intel_crtc_compute_min_cdclk` | found | display/clock.c:drv_i915_crtc_compute_min_cdclk |  |

#### `parity/lcd/intel_color_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 50 | `lut_is_legacy` | found | display/color.c:i915_lut_is_legacy |  |
| 55 | `icl_gamma_mode` | found | display/color.c:i915_icl_gamma_mode |  |
| 84 | `icl_csc_mode` | found | display/color.c:i915_icl_csc_mode |  |
| 98 | `icl_load_csc_matrix` | found | display/color.c:i915_icl_load_csc_matrix |  |
| 109 | `icl_load_luts` | found | display/color.c:i915_icl_load_luts |  |
| 138 | `icl_color_commit_noarm` | found | display/color.c:i915_icl_color_commit_noarm |  |
| 151 | `icl_color_commit_arm` | found | display/color.c:i915_icl_color_commit_arm |  |
| 170 | `intel_color_load_luts` | found | display/color.c:drv_i915_color_load_luts |  |
| 180 | `intel_color_commit_noarm` | found | display/color.c:drv_i915_color_commit_noarm |  |
| 188 | `intel_color_commit_arm` | found | display/color.c:drv_i915_color_commit_arm |  |

#### `parity/lcd/intel_combo_phy_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 17 | `intel_combo_phy_power_up_lanes` | found | display/phy.c:drv_i915_combo_phy_power_up_lanes |  |

#### `parity/lcd/intel_crtc_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 29 | `intel_crtc_state_reset` | found | display/pipe.c:drv_i915_crtc_state_reset |  |
| 44 | `intel_usecs_to_scanlines` | found | display/pipe.c:drv_i915_usecs_to_scanlines |  |
| 55 | `intel_crtc_get_vblank_counter` | found | display/pipe.c:i915_crtc_get_vblank_counter |  |
| 69 | `intel_crtc_needs_vblank_work` | found | display/pipe.c:i915_crtc_needs_vblank_work |  |
| 78 | `intel_mode_vblank_start` | found | display/pipe.c:i915_mode_vblank_start |  |
| 88 | `intel_crtc_vblank_evade_scanlines` | found | display/pipe.c:drv_i915_crtc_vblank_evade_scanlines |  |
| 155 | `intel_pipe_update_start` | found | display/pipe.c:drv_i915_pipe_update_start |  |
| 271 | `intel_pipe_update_end` | found | display/pipe.c:drv_i915_pipe_update_end |  |
| 354 | `intel_crtc_wait_for_next_vblank` | found | display/pipe.c:drv_i915_crtc_wait_for_next_vblank |  |

#### `parity/lcd/intel_ddi_buf_trans_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 256 | `is_hobl_buf_trans` | found | display/phy.c:drv_i915_is_hobl_buf_trans |  |
| 261 | `use_edp_hobl` | found | display/phy.c:i915_use_edp_hobl |  |
| 269 | `use_edp_low_vswing` | found | display/phy.c:i915_use_edp_low_vswing |  |
| 278 | `intel_get_buf_trans` | found | display/phy.c:i915_intel_get_buf_trans |  |
| 285 | `tgl_get_combo_buf_trans_dp` | found | display/phy.c:i915_tgl_get_combo_buf_trans_dp |  |
| 306 | `tgl_get_combo_buf_trans_edp` | found | display/phy.c:i915_tgl_get_combo_buf_trans_edp |  |
| 325 | `tgl_get_combo_buf_trans` | found | display/phy.c:i915_tgl_get_combo_buf_trans |  |
| 338 | `adlp_get_combo_buf_trans_dp` | found | display/phy.c:i915_adlp_get_combo_buf_trans_dp |  |
| 349 | `adlp_get_combo_buf_trans_edp` | found | display/phy.c:i915_adlp_get_combo_buf_trans_edp |  |
| 368 | `adlp_get_combo_buf_trans` | found | display/phy.c:i915_adlp_get_combo_buf_trans |  |

#### `parity/lcd/intel_ddi_hotplug_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 46 | `intel_ddi_hotplug` | found | display/hotplug.c:i915_ddi_hotplug |  |
| 108 | `lpt_digital_port_connected` | found | display/hotplug.c:i915_lpt_digital_port_connected |  |

#### `parity/lcd/intel_ddi_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 200 | `ddi_buf_phy_link_rate` | found | display/ddi.c:i915_ddi_buf_phy_link_rate |  |
| 225 | `intel_ddi_init_dp_buf_reg` | found | display/ddi.c:i915_ddi_init_dp_buf_reg |  |
| 252 | `intel_ddi_set_dp_msa` | found | display/ddi.c:i915_ddi_set_dp_msa |  |
| 312 | `bdw_trans_port_sync_master_select` | found | display/ddi.c:i915_bdw_trans_port_sync_master_select |  |
| 327 | `intel_ddi_transcoder_func_reg_val_get` | found | display/ddi.c:i915_ddi_transcoder_func_reg_val_get |  |
| 438 | `intel_ddi_enable_transcoder_func` | found | display/ddi.c:i915_ddi_enable_transcoder_func |  |
| 471 | `intel_ddi_config_transcoder_func` | found | display/ddi.c:i915_ddi_config_transcoder_func |  |
| 485 | `hsw_chicken_trans_reg` | found | display/ddi.c:drv_i915_hsw_chicken_trans_reg |  |
| 493 | `tgl_ddi_pre_enable_dp` | found | display/ddi.c:i915_tgl_ddi_pre_enable_dp |  |
| 635 | `intel_ddi_pre_enable_dp` | found | display/ddi.c:i915_ddi_pre_enable_dp |  |
| 665 | `intel_ddi_pre_enable` | found | display/ddi.c:i915_ddi_pre_enable |  |
| 709 | `intel_enable_ddi_dp` | found | display/ddi.c:i915_enable_ddi_dp |  |
| 731 | `intel_enable_ddi` | found | display/ddi.c:i915_enable_ddi |  |
| 760 | `intel_ddi_pre_pll_enable` | found | display/ddi.c:i915_ddi_pre_pll_enable |  |
| 804 | `intel_ddi_dp_voltage_max` | found | display/ddi.c:i915_ddi_dp_voltage_max |  |
| 828 | `intel_ddi_dp_preemph_max` | found | display/ddi.c:i915_ddi_dp_preemph_max |  |
| 833 | `intel_ddi_enable_clock` | found | display/ddi.c:i915_ddi_enable_clock |  |
| 840 | `intel_ddi_disable_clock` | found | display/takeover.c:drv_i915_ddi_disable_clock |  |
| 846 | `_icl_ddi_enable_clock` | found | display/ddi.c:i915_icl_ddi_enable_clock_reg (function containing the matched text) | match by string |
| 862 | `_icl_ddi_disable_clock` | found | display/ddi.c:i915_icl_ddi_disable_clock_reg | match by substr |
| 872 | `icl_ddi_combo_enable_clock` | found | display/ddi.c:i915_icl_ddi_combo_enable_clock |  |
| 888 | `icl_ddi_combo_disable_clock` | found | display/ddi.c:i915_icl_ddi_combo_disable_clock |  |
| 898 | `intel_ddi_main_link_aux_domain` | found | display/ddi.c:i915_ddi_main_link_aux_domain |  |
| 928 | `main_link_aux_power_domain_get` | found | display/ddi.c:i915_main_link_aux_power_domain_get |  |
| 944 | `main_link_aux_power_domain_put` | found | display/ddi.c:i915_main_link_aux_power_domain_put |  |
| 959 | `intel_ddi_enable_transcoder_clock` | found | display/ddi.c:i915_ddi_enable_transcoder_clock |  |
| 981 | `intel_ddi_disable_transcoder_clock` | found | display/ddi.c:i915_ddi_disable_transcoder_clock |  |
| 998 | `intel_ddi_dp_level` | found | display/ddi.c:i915_ddi_dp_level |  |
| 1014 | `intel_ddi_level` | found | display/ddi.c:i915_ddi_level |  |
| 1038 | `icl_combo_phy_loadgen_select` | found | display/ddi.c:i915_icl_combo_phy_loadgen_select |  |
| 1050 | `icl_ddi_combo_vswing_program` | found | display/ddi.c:i915_icl_ddi_combo_vswing_program |  |
| 1114 | `icl_combo_phy_set_signal_levels` | found | display/ddi.c:i915_icl_combo_phy_set_signal_levels |  |
| 1165 | `translate_signal_level` | found | display/ddi.c:i915_translate_signal_level |  |
| 1183 | `intel_ddi_power_up_lanes` | found | display/ddi.c:i915_ddi_power_up_lanes |  |
| 1200 | `intel_ddi_mso_configure` | found | display/ddi.c:i915_ddi_mso_configure |  |
| 1225 | `tgl_dp_tp_transcoder` | found | display/ddi.c:i915_tgl_dp_tp_transcoder |  |
| 1233 | `dp_tp_ctl_reg` | found | display/ddi.c:i915_dp_tp_ctl_reg |  |
| 1244 | `dp_tp_status_reg` | found | display/ddi.c:i915_dp_tp_status_reg |  |
| 1255 | `intel_wait_ddi_buf_idle` | found | display/ddi.c:i915_wait_ddi_buf_idle |  |
| 1269 | `intel_wait_ddi_buf_active` | found | display/ddi.c:i915_wait_ddi_buf_active |  |
| 1307 | `intel_ddi_prepare_link_retrain` | found | display/ddi.c:i915_ddi_prepare_link_retrain |  |
| 1357 | `intel_ddi_set_link_train` | found | display/ddi.c:i915_ddi_set_link_train |  |
| 1389 | `intel_ddi_set_idle_link_train` | found | display/ddi.c:i915_ddi_set_idle_link_train |  |
| 1416 | `intel_ddi_disable_fec` | found | display/ddi.c:i915_ddi_disable_fec |  |
| 1429 | `disable_ddi_buf` | found | display/ddi.c:i915_disable_ddi_buf |  |
| 1454 | `intel_disable_ddi_buf` | found | display/ddi.c:i915_disable_ddi_buf |  |
| 1471 | `intel_ddi_disable_transcoder_func` | found | display/ddi.c:i915_ddi_disable_transcoder_func |  |
| 1512 | `intel_dp_sink_set_msa_timing_par_ignore_state` | found | display/ddi.c:i915_dp_sink_set_msa_timing_par_ignore_state |  |
| 1528 | `intel_disable_ddi_dp` | found | display/ddi.c:i915_disable_ddi_dp |  |
| 1549 | `intel_disable_ddi` | found | display/ddi.c:i915_disable_ddi |  |
| 1566 | `intel_ddi_post_disable_dp` | found | display/ddi.c:i915_ddi_post_disable_dp |  |
| 1631 | `intel_ddi_post_disable` | found | display/ddi.c:i915_ddi_post_disable |  |
| 1686 | `intel_ddi_post_pll_disable` | found | display/ddi.c:i915_ddi_post_pll_disable |  |
| 1702 | `icl_ddi_min_voltage_level` | found | display/ddi.c:i915_icl_ddi_min_voltage_level |  |
| 1710 | `jsl_ddi_min_voltage_level` | found | display/ddi.c:i915_jsl_ddi_min_voltage_level |  |
| 1718 | `tgl_ddi_min_voltage_level` | found | display/ddi.c:i915_tgl_ddi_min_voltage_level |  |
| 1726 | `intel_ddi_compute_min_voltage_level` | found | display/ddi.c:drv_i915_ddi_compute_min_voltage_level |  |
| 1740 | `intel_ddi_hdmi_level` | found | display/ddi.c:i915_ddi_hdmi_level |  |
| 1752 | `intel_ddi_pre_enable_hdmi` | found | display/ddi.c:i915_ddi_pre_enable_hdmi |  |
| 1777 | `intel_enable_ddi_hdmi` | found | display/ddi.c:i915_enable_ddi_hdmi |  |
| 1880 | `intel_disable_ddi_hdmi` | found | display/ddi.c:i915_disable_ddi_hdmi |  |
| 1895 | `intel_ddi_post_disable_hdmi` | found | display/ddi.c:i915_ddi_post_disable_hdmi |  |
| 1927 | `intel_ddi_get_encoder_pipes` | found | display/ddi.c:i915_ddi_get_encoder_pipes |  |
| 2043 | `intel_ddi_get_hw_state` | found | display/takeover.c:drv_i915_ddi_get_hw_state |  |
| 2059 | `intel_ddi_read_func_ctl` | found | display/ddi.c:i915_ddi_read_func_ctl |  |
| 2184 | `ddi_dotclock_get` | found | display/ddi.c:i915_ddi_dotclock_get |  |
| 2194 | `intel_ddi_get_config` | found | display/ddi.c:i915_ddi_get_config |  |
| 2248 | `intel_ddi_get_clock` | found | display/ddi.c:i915_ddi_get_clock |  |
| 2271 | `_icl_ddi_get_pll` | found | display/ddi.c:i915_icl_ddi_get_pll_reg | match by substr |
| 2281 | `icl_ddi_combo_get_pll` | found | display/ddi.c:i915_icl_ddi_combo_get_pll |  |
| 2291 | `icl_ddi_combo_get_config` | found | display/ddi.c:i915_icl_ddi_combo_get_config |  |
| 2298 | `intel_ddi_sync_state` | found | display/ddi.c:i915_ddi_sync_state |  |
| 2312 | `intel_ddi_get_power_domains` | found | display/ddi.c:i915_ddi_get_power_domains |  |
| 2338 | `_icl_ddi_is_clock_enabled` | found | display/ddi.c:i915_icl_ddi_is_clock_enabled_reg | match by substr |
| 2344 | `icl_ddi_combo_is_clock_enabled` | found | display/ddi.c:i915_icl_ddi_combo_is_clock_enabled |  |
| 2353 | `intel_ddi_connector_get_hw_state` | found | display/ddi.c:drv_i915_ddi_connector_get_hw_state |  |
| 2420 | `intel_ddi_sanitize_encoder_pll_mapping` | found | display/ddi.c:drv_i915_ddi_sanitize_encoder_pll_mapping |  |

#### `parity/lcd/intel_display_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 100 | `intel_phy_is_tc` | found | display/takeover.c:drv_i915_phy_is_tc |  |
| 121 | `intel_port_to_phy` | found | display/takeover.c:drv_i915_port_to_phy |  |
| 139 | `intel_reduce_m_n_ratio` | found | display/pipe.c:i915_reduce_m_n_ratio |  |
| 148 | `compute_m_n` | found | display/pipe.c:i915_compute_m_n |  |
| 161 | `intel_link_compute_m_n` | found | display/pipe.c:drv_i915_link_compute_m_n |  |
| 188 | `intel_set_m_n` | found | display/pipe.c:i915_set_m_n |  |
| 203 | `intel_cpu_transcoder_has_m2_n2` | found | display/pipe.c:i915_cpu_transcoder_has_m2_n2 |  |
| 212 | `intel_cpu_transcoder_set_m1_n1` | found | display/pipe.c:i915_cpu_transcoder_set_m1_n1 |  |
| 229 | `intel_cpu_transcoder_set_m2_n2` | found | display/pipe.c:i915_cpu_transcoder_set_m2_n2 |  |
| 243 | `intel_set_transcoder_timings` | found | display/pipe.c:i915_set_transcoder_timings |  |
| 324 | `intel_set_pipe_src_size` | found | display/pipe.c:i915_set_pipe_src_size |  |
| 339 | `hsw_set_frame_start_delay` | found | display/pipe.c:i915_hsw_set_frame_start_delay |  |
| 349 | `hsw_set_transconf` | found | display/pipe.c:i915_hsw_set_transconf |  |
| 379 | `hsw_configure_cpu_transcoder` | found | display/pipe.c:i915_hsw_configure_cpu_transcoder |  |
| 408 | `hsw_crtc_enable` | found | display/pipe.c:i915_hsw_crtc_enable |  |
| 501 | `intel_dotclock_calculate` | found | display/pipe.c:i915_dotclock_calculate |  |
| 524 | `ilk_pipe_pixel_rate` | found | display/pipe.c:i915_ilk_pipe_pixel_rate |  |
| 545 | `intel_get_m_n` | found | display/pipe.c:i915_get_m_n |  |
| 557 | `has_dsi_transcoders` | found | display/pipe.c:i915_has_dsi_transcoders |  |
| 563 | `has_pipe_transcoders` | found | display/pipe.c:i915_has_pipe_transcoders |  |
| 570 | `has_edp_transcoders` | found | display/pipe.c:i915_has_edp_transcoders |  |
| 575 | `is_hdr_mode` | found | display/pipe.c:i915_is_hdr_mode |  |
| 582 | `intel_phy_is_combo` | found | display/pipe.c:drv_i915_phy_is_combo |  |
| 604 | `intel_aux_power_domain` | found | display/pipe.c:drv_i915_aux_power_domain |  |
| 615 | `intel_wait_for_pipe_off` | found | display/pipe.c:i915_wait_for_pipe_off |  |
| 632 | `intel_enable_transcoder` | found | display/pipe.c:drv_i915_enable_transcoder |  |
| 692 | `intel_disable_transcoder` | found | display/pipe.c:drv_i915_disable_transcoder |  |
| 733 | `bdw_set_pipe_misc` | found | display/pipe.c:i915_bdw_set_pipe_misc |  |
| 783 | `icl_set_pipe_chicken` | found | display/pipe.c:i915_icl_set_pipe_chicken |  |
| 822 | `hsw_set_linetime_wm` | found | display/pipe.c:i915_hsw_set_linetime_wm |  |
| 832 | `hsw_crtc_disable` | found | display/pipe.c:i915_hsw_crtc_disable |  |
| 863 | `get_crtc_power_domains` | found | display/pipe.c:i915_get_crtc_power_domains |  |
| 900 | `intel_modeset_get_crtc_power_domains` | found | display/pipe.c:drv_i915_modeset_get_crtc_power_domains |  |
| 925 | `intel_modeset_put_crtc_power_domains` | found | display/pipe.c:drv_i915_modeset_put_crtc_power_domains |  |
| 933 | `hsw_panel_transcoders` | found | display/pipe.c:i915_hsw_panel_transcoders |  |
| 943 | `hsw_enabled_transcoders` | found | display/pipe.c:i915_hsw_enabled_transcoders |  |
| 1012 | `hsw_get_transcoder_state` | found | display/pipe.c:i915_hsw_get_transcoder_state |  |
| 1050 | `intel_get_transcoder_timings` | found | {display/pipe.c,display/takeover.c}:i915_get_transcoder_timings |  |
| 1099 | `intel_get_pipe_src_size` | found | display/pipe.c:i915_get_pipe_src_size |  |
| 1116 | `bdw_get_pipe_misc_output_format` | found | display/pipe.c:i915_bdw_get_pipe_misc_output_format |  |
| 1136 | `hsw_get_pipe_config` | found | display/pipe.c:i915_hsw_get_pipe_config |  |
| 1229 | `intel_crtc_get_pipe_config` | found | display/pipe.c:drv_i915_crtc_get_pipe_config |  |
| 1244 | `intel_crtc_readout_derived_state` | found | display/pipe.c:i915_crtc_readout_derived_state |  |
| 1280 | `intel_encoder_get_config` | found | display/pipe.c:drv_i915_encoder_get_config |  |
| 1288 | `intel_set_plane_visible` | found | display/pipe.c:drv_i915_set_plane_visible |  |
| 1302 | `intel_plane_fixup_bitmasks` | found | display/pipe.c:drv_i915_plane_fixup_bitmasks |  |
| 1322 | `intel_plane_disable_noatomic` | found | display/pipe.c:drv_i915_plane_disable_noatomic |  |
| 1374 | `transcoder_ddi_func_is_enabled` | found | display/pipe.c:i915_transcoder_ddi_func_is_enabled |  |
| 1389 | `intel_crtc_dotclock` | found | display/pipe.c:drv_i915_crtc_dotclock |  |
| 1412 | `assert_enabled_transcoders` | found | display/pipe.c:i915_assert_enabled_transcoders |  |
| 1427 | `intel_pipe_is_interlaced` | found | display/pipe.c:i915_pipe_is_interlaced |  |
| 1442 | `intel_cpu_transcoder_get_m1_n1` | found | display/pipe.c:drv_i915_cpu_transcoder_get_m1_n1 |  |
| 1459 | `intel_cpu_transcoder_get_m2_n2` | found | display/pipe.c:drv_i915_cpu_transcoder_get_m2_n2 |  |
| 1473 | `intel_mode_from_crtc_timings` | found | display/pipe.c:i915_mode_from_crtc_timings |  |
| 1494 | `intel_splitter_adjust_timings` | found | display/pipe.c:i915_splitter_adjust_timings |  |
| 1518 | `intel_crtc_compute_pixel_rate` | found | display/pipe.c:i915_crtc_compute_pixel_rate |  |

#### `parity/lcd/intel_display_power_set_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 19 | `intel_display_power_get_in_set` | found | display/power.c:drv_i915_display_power_get_in_set |  |
| 35 | `intel_display_power_put_mask_in_set` | found | display/power.c:drv_i915_display_power_put_mask_in_set |  |

#### `parity/lcd/intel_dmc_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 51 | `is_valid_dmc_id` | found | display/dmc.c:i915_is_valid_dmc_id |  |
| 56 | `intel_dmc_enable_pipe` | found | display/dmc.c:drv_i915_dmc_enable_pipe |  |
| 69 | `intel_dmc_disable_pipe` | found | display/dmc.c:drv_i915_dmc_disable_pipe |  |

#### `parity/lcd/intel_dp_connected_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 49 | `intel_digital_port_connected` | found | display/modeset-internal.h:i915_lcd_intel_digital_port_connected |  |

#### `parity/lcd/intel_dp_link_training_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 151 | `intel_dp_reset_lttpr_common_caps` | found | display/dp.c:i915_dp_reset_lttpr_common_caps |  |
| 156 | `intel_dp_reset_lttpr_count` | found | display/dp.c:i915_dp_reset_lttpr_count |  |
| 162 | `intel_dp_lttpr_phy_caps` | found | display/dp.c:i915_dp_lttpr_phy_caps |  |
| 168 | `intel_dp_read_lttpr_phy_caps` | found | display/dp.c:i915_dp_read_lttpr_phy_caps |  |
| 184 | `intel_dp_read_lttpr_common_caps` | found | display/dp.c:i915_dp_read_lttpr_common_caps |  |
| 210 | `intel_dp_set_lttpr_transparent_mode` | found | display/dp.c:i915_dp_set_lttpr_transparent_mode |  |
| 218 | `intel_dp_lttpr_transparent_mode_enabled` | found | display/dp.c:i915_dp_lttpr_transparent_mode_enabled |  |
| 233 | `intel_dp_init_lttpr_phys` | found | display/dp.c:i915_dp_init_lttpr_phys |  |
| 294 | `intel_dp_init_lttpr` | found | display/dp.c:i915_dp_init_lttpr |  |
| 325 | `intel_dp_init_lttpr_and_dprx_caps` | found | display/dp.c:i915_dp_init_lttpr_and_dprx_caps |  |
| 359 | `dp_voltage_max` | found | display/dp.c:i915_dp_voltage_max |  |
| 374 | `intel_dp_lttpr_voltage_max` | found | display/dp.c:i915_dp_lttpr_voltage_max |  |
| 385 | `intel_dp_lttpr_preemph_max` | found | display/dp.c:i915_dp_lttpr_preemph_max |  |
| 397 | `intel_dp_phy_is_downstream_of_source` | found | display/dp.c:i915_dp_phy_is_downstream_of_source |  |
| 408 | `intel_dp_phy_voltage_max` | found | display/dp.c:i915_dp_phy_voltage_max |  |
| 431 | `intel_dp_phy_preemph_max` | found | display/dp.c:i915_dp_phy_preemph_max |  |
| 453 | `has_per_lane_signal_levels` | found | display/dp.c:i915_has_per_lane_signal_levels |  |
| 463 | `intel_dp_get_lane_adjust_tx_ffe_preset` | found | display/dp.c:i915_dp_get_lane_adjust_tx_ffe_preset |  |
| 483 | `intel_dp_get_lane_adjust_vswing_preemph` | found | display/dp.c:i915_dp_get_lane_adjust_vswing_preemph |  |
| 519 | `intel_dp_get_lane_adjust_train` | found | display/dp.c:i915_dp_get_lane_adjust_train |  |
| 534 | `intel_dp_get_adjust_train` | found | display/dp.c:i915_dp_get_adjust_train |  |
| 563 | `intel_dp_training_pattern_set_reg` | found | display/dp.c:i915_dp_training_pattern_set_reg |  |
| 572 | `intel_dp_set_link_train` | found | display/dp.c:i915_dp_set_link_train |  |
| 592 | `dp_training_pattern_name` | found | display/dp.c:i915_dp_training_pattern_name |  |
| 608 | `intel_dp_program_link_training_pattern` | found | display/dp.c:i915_dp_program_link_training_pattern |  |
| 622 | `intel_dp_set_signal_levels` | found | display/dp.c:i915_dp_set_signal_levels |  |
| 649 | `intel_dp_reset_link_train` | found | display/dp.c:i915_dp_reset_link_train |  |
| 660 | `intel_dp_update_link_train` | found | display/dp.c:i915_dp_update_link_train |  |
| 678 | `intel_dp_lane_max_tx_ffe_reached` | found | display/dp.c:i915_dp_lane_max_tx_ffe_reached |  |
| 694 | `intel_dp_lane_max_vswing_reached` | found | display/dp.c:i915_dp_lane_max_vswing_reached |  |
| 710 | `intel_dp_link_max_vswing_reached` | found | display/dp.c:i915_dp_link_max_vswing_reached |  |
| 731 | `intel_dp_update_downspread_ctrl` | found | display/dp.c:i915_dp_update_downspread_ctrl |  |
| 743 | `intel_dp_update_link_bw_set` | found | display/dp.c:i915_dp_update_link_bw_set |  |
| 778 | `intel_dp_prepare_link_train` | found | display/dp.c:i915_dp_prepare_link_train |  |
| 827 | `intel_dp_adjust_request_changed` | found | display/dp.c:i915_dp_adjust_request_changed |  |
| 854 | `intel_dp_dump_link_status` | found | display/dp.c:i915_dp_dump_link_status |  |
| 868 | `intel_dp_link_training_clock_recovery` | found | display/dp.c:i915_dp_link_training_clock_recovery |  |
| 961 | `intel_dp_training_pattern` | found | display/dp.c:i915_dp_training_pattern |  |
| 1019 | `intel_dp_link_training_channel_equalization` | found | display/dp.c:i915_dp_link_training_channel_equalization |  |
| 1088 | `intel_dp_disable_dpcd_training_pattern` | found | display/dp.c:i915_dp_disable_dpcd_training_pattern |  |
| 1113 | `intel_dp_stop_link_train` | found | display/dp.c:drv_i915_dp_stop_link_train |  |
| 1129 | `intel_dp_link_train_phy` | found | display/dp.c:i915_dp_link_train_phy |  |
| 1152 | `intel_dp_schedule_fallback_link_training` | found | display/dp.c:i915_dp_schedule_fallback_link_training |  |
| 1179 | `intel_dp_link_train_all_phys` | found | display/dp.c:i915_dp_link_train_all_phys |  |
| 1215 | `intel_dp_start_link_train` | found | display/dp.c:drv_i915_dp_start_link_train |  |

#### `parity/lcd/intel_dpll_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 127 | `ehl_combo_pll_div_frac_wa_needed` | found | display/clock.c:i915_ehl_combo_pll_div_frac_wa_needed |  |
| 200 | `icl_calc_dp_combo_pll` | found | display/clock.c:i915_icl_calc_dp_combo_pll |  |
| 222 | `icl_calc_dpll_state` | found | display/clock.c:i915_icl_calc_dpll_state |  |
| 249 | `intel_combo_pll_enable_reg` | found | display/clock.c:i915_combo_pll_enable_reg |  |
| 261 | `icl_pll_power_enable` | found | display/clock.c:i915_icl_pll_power_enable |  |
| 276 | `icl_dpll_write` | found | display/clock.c:i915_icl_dpll_write |  |
| 318 | `icl_pll_enable` | found | display/clock.c:i915_icl_pll_enable |  |
| 329 | `adlp_cmtg_clock_gating_wa` | found | display/clock.c:i915_adlp_cmtg_clock_gating_wa |  |
| 353 | `combo_pll_enable` | found | display/clock.c:i915_combo_pll_enable |  |
| 375 | `icl_pll_disable` | found | display/clock.c:i915_icl_pll_disable |  |
| 406 | `combo_pll_disable` | found | display/clock.c:i915_combo_pll_disable |  |
| 414 | `_intel_enable_shared_dpll` | found | display/clock.c:i915_intel_enable_shared_dpll | match by substr |
| 430 | `intel_enable_shared_dpll` | found | display/clock.c:drv_i915_enable_shared_dpll |  |
| 470 | `_intel_disable_shared_dpll` | found | display/clock.c:i915_intel_disable_shared_dpll | match by substr |
| 486 | `intel_disable_shared_dpll` | found | display/clock.c:drv_i915_disable_shared_dpll |  |
| 526 | `icl_wrpll_ref_clock` | found | display/clock.c:i915_icl_wrpll_ref_clock |  |
| 540 | `icl_wrpll_get_multipliers` | found | display/clock.c:i915_icl_wrpll_get_multipliers |  |
| 579 | `icl_wrpll_params_populate` | found | display/clock.c:i915_icl_wrpll_params_populate |  |
| 628 | `icl_calc_wrpll` | found | display/clock.c:i915_icl_calc_wrpll |  |
| 680 | `intel_get_shared_dpll_by_id` | found | display/clock.c:drv_i915_get_shared_dpll_by_id |  |
| 696 | `intel_dpll_mask_all` | found | display/clock.c:i915_dpll_mask_all |  |
| 712 | `intel_find_shared_dpll` | found | display/clock.c:i915_find_shared_dpll |  |
| 774 | `intel_reference_shared_dpll_crtc` | found | display/clock.c:i915_reference_shared_dpll_crtc |  |
| 789 | `intel_reference_shared_dpll` | found | display/clock.c:i915_reference_shared_dpll |  |
| 813 | `intel_unreference_shared_dpll_crtc` | found | display/clock.c:drv_i915_unreference_shared_dpll_crtc |  |
| 827 | `intel_unreference_shared_dpll` | found | display/clock.c:i915_unreference_shared_dpll |  |
| 838 | `icl_ddi_combo_pll_get_freq` | found | display/clock.c:i915_icl_ddi_combo_pll_get_freq |  |
| 907 | `intel_dpll_get_freq` | found | display/clock.c:drv_i915_dpll_get_freq |  |
| 925 | `intel_dpll_get_hw_state` | found | display/clock.c:drv_i915_dpll_get_hw_state |  |
| 932 | `combo_pll_get_hw_state` | found | display/clock.c:i915_combo_pll_get_hw_state |  |
| 941 | `icl_pll_get_hw_state` | found | display/clock.c:i915_icl_pll_get_hw_state |  |
| 1001 | `readout_dpll_hw_state` | found | display/clock.c:i915_readout_dpll_hw_state |  |
| 1026 | `intel_dpll_readout_hw_state` | found | display/clock.c:drv_i915_n1_dpll_readout_hw_state |  |
| 1035 | `sanitize_dpll_state` | found | display/clock.c:i915_sanitize_dpll_state |  |
| 1053 | `intel_dpll_sanitize_state` | found | display/clock.c:drv_i915_dpll_sanitize_state |  |

#### `parity/lcd/intel_gmbus_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 89 | `to_intel_gmbus` | found | display/gmbus.c:i915_to_intel_gmbus |  |
| 95 | `intel_gmbus_reset` | found | display/gmbus.c:i915_hpd_intel_gmbus_reset |  |
| 101 | `has_gmbus_irq` | found | display/gmbus.c:i915_has_gmbus_irq |  |
| 110 | `gmbus_wait` | found | display/gmbus.c:i915_gmbus_wait |  |
| 143 | `gmbus_wait_idle` | found | display/gmbus.c:i915_gmbus_wait_idle |  |
| 165 | `gmbus_max_xfer_size` | found | display/gmbus.c:i915_gmbus_max_xfer_size |  |
| 172 | `gmbus_xfer_read_chunk` | found | display/gmbus.c:i915_gmbus_xfer_read_chunk |  |
| 224 | `gmbus_xfer_read` | found | display/gmbus.c:i915_gmbus_xfer_read |  |
| 251 | `gmbus_xfer_write_chunk` | found | display/gmbus.c:i915_gmbus_xfer_write_chunk |  |
| 286 | `gmbus_xfer_write` | found | display/gmbus.c:i915_gmbus_xfer_write |  |
| 314 | `gmbus_is_index_xfer` | found | display/gmbus.c:i915_gmbus_is_index_xfer |  |
| 324 | `gmbus_index_xfer` | found | display/gmbus.c:i915_gmbus_index_xfer |  |
| 356 | `do_gmbus_xfer` | found | display/gmbus.c:i915_do_gmbus_xfer |  |
| 487 | `gmbus_xfer` | found | display/gmbus.c:i915_gmbus_xfer |  |
| 511 | `intel_gmbus_force_bit` | found | display/gmbus.c:i915_hpd_intel_gmbus_force_bit |  |
| 527 | `intel_gmbus_is_forced_bit` | found | display/gmbus.c:i915_hpd_intel_gmbus_is_forced_bit |  |
| 534 | `intel_gmbus_irq_handler` | found | display/gmbus.c:i915_hpd_intel_gmbus_irq_handler |  |

#### `parity/lcd/intel_hdmi_detect_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 49 | `intel_hdmi_unset_edid` | found | display/hdmi.c:i915_hdmi_unset_edid |  |
| 61 | `intel_hdmi_set_edid` | found | display/hdmi.c:i915_hdmi_set_edid |  |
| 102 | `intel_hdmi_detect` | found | display/hdmi.c:i915_hdmi_detect |  |

#### `parity/lcd/intel_hdmi_mode_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 54 | `assert_hdmi_transcoder_func_disabled` | found | display/hdmi-mode.c:i915_assert_hdmi_transcoder_func_disabled |  |
| 63 | `hsw_set_infoframes` | found | display/hdmi-mode.c:i915_hsw_set_infoframes |  |
| 106 | `intel_dp_dual_mode_set_tmds_output` | found | display/hdmi-mode.c:drv_i915_dp_dual_mode_set_tmds_output |  |
| 139 | `intel_hdmi_handle_sink_scrambling` | found | display/hdmi-mode.c:drv_i915_hdmi_handle_sink_scrambling |  |

#### `parity/lcd/intel_hotplug_irq_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 25 | `icp_ddi_port_hotplug_long_detect` | found | display/hotplug.c:i915_icp_ddi_port_hotplug_long_detect |  |
| 38 | `icp_tc_port_hotplug_long_detect` | found | display/hotplug.c:i915_icp_tc_port_hotplug_long_detect |  |
| 60 | `intel_get_hpd_pins` | found | display/hotplug.c:i915_get_hpd_pins |  |
| 85 | `icp_irq_handler` | found | display/hotplug.c:i915_hpd_icp_irq_handler |  |

#### `parity/lcd/intel_hotplug_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 58 | `intel_connector_hpd_pin` | found | display/hotplug.c:i915_connector_hpd_pin |  |
| 100 | `intel_hpd_irq_storm_detect` | found | display/hotplug.c:i915_hpd_irq_storm_detect |  |
| 136 | `intel_hpd_irq_storm_switch_to_polling` | found | display/hotplug.c:i915_hpd_irq_storm_switch_to_polling |  |
| 177 | `intel_hpd_irq_storm_reenable_work` | found | display/hotplug.c:i915_hpd_irq_storm_reenable_work |  |
| 219 | `intel_hotplug_detect_connector` | found | display/hotplug.c:i915_hotplug_detect_connector |  |
| 252 | `intel_encoder_hotplug` | found | display/hotplug.c:i915_hpd_intel_encoder_hotplug |  |
| 258 | `intel_encoder_has_hpd_pulse` | found | display/hotplug.c:i915_encoder_has_hpd_pulse |  |
| 264 | `i915_digport_work_func` | found | display/hotplug.c:i915_digport_work_func |  |
| 315 | `i915_hotplug_work_func` | found | display/hotplug.c:i915_hotplug_work_func |  |
| 431 | `intel_hpd_irq_handler` | found | display/hotplug.c:i915_hpd_intel_hpd_irq_handler |  |
| 544 | `intel_hpd_init_early` | found | display/hotplug.c:i915_hpd_intel_hpd_init_early |  |
| 563 | `intel_hpd_cancel_work` | found | display/hotplug.c:i915_hpd_intel_hpd_cancel_work |  |

#### `parity/lcd/intel_link_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 49 | `intel_dp_is_uhbr` | found | display/dp.c:drv_i915_dp_is_uhbr |  |
| 61 | `intel_dp_link_symbol_size` | found | display/dp.c:drv_i915_dp_link_symbol_size |  |
| 73 | `intel_dp_link_symbol_clock` | found | display/dp.c:drv_i915_dp_link_symbol_clock |  |
| 86 | `intel_dp_link_required` | found | display/dp.c:drv_i915_dp_link_required |  |
| 101 | `intel_dp_effective_data_rate` | found | display/dp.c:drv_i915_dp_effective_data_rate |  |
| 129 | `intel_dp_max_data_rate` | found | display/dp.c:drv_i915_dp_max_data_rate |  |
| 155 | `intel_dp_needs_vsc_sdp` | found | display/dp.c:drv_i915_dp_needs_vsc_sdp |  |
| 181 | `intel_dp_rate_index` | found | display/dp.c:i915_dp_rate_index |  |
| 201 | `intel_dp_is_edp` | found | display/dp.c:drv_i915_dp_is_edp |  |
| 208 | `intel_dp_source_supports_tps3` | found | display/dp.c:i915_dp_source_supports_tps3 |  |
| 213 | `intel_dp_source_supports_tps4` | found | display/dp.c:i915_dp_source_supports_tps4 |  |
| 218 | `intel_dp_rate_select` | found | display/dp.c:i915_dp_rate_select |  |
| 230 | `intel_dp_compute_rate` | found | display/dp.c:i915_dp_compute_rate |  |
| 250 | `intel_dp_set_link_params` | found | display/dp.c:drv_i915_dp_set_link_params |  |
| 259 | `downstream_hpd_needs_d0` | found | display/dp.c:i915_downstream_hpd_needs_d0 |  |
| 275 | `intel_edp_init_source_oui` | found | display/dp.c:i915_edp_init_source_oui |  |
| 303 | `intel_dp_set_power` | found | display/dp.c:drv_i915_dp_set_power |  |
| 349 | `intel_edp_backlight_on` | found | display/dp.c:drv_i915_edp_backlight_on |  |
| 365 | `intel_edp_backlight_off` | found | display/dp.c:drv_i915_edp_backlight_off |  |
| 379 | `intel_dp_set_infoframes` | found | display/dp.c:drv_i915_dp_set_infoframes |  |

#### `parity/lcd/intel_modeset_setup_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 68 | `intel_crtc_needs_link_reset` | found | display/takeover.c:i915_crtc_needs_link_reset |  |
| 83 | `get_bigjoiner_slave_pipes` | found | display/takeover.c:i915_get_bigjoiner_slave_pipes |  |
| 102 | `get_portsync_pipes` | found | display/takeover.c:i915_get_portsync_pipes |  |
| 136 | `get_transcoder_pipes` | found | display/takeover.c:i915_get_transcoder_pipes |  |
| 159 | `intel_crtc_disable_noatomic_begin` | found | display/takeover.c:i915_crtc_disable_noatomic_begin |  |
| 222 | `intel_crtc_disable_noatomic_complete` | found | display/takeover.c:i915_crtc_disable_noatomic_complete |  |
| 260 | `intel_crtc_disable_noatomic` | found | display/takeover.c:i915_crtc_disable_noatomic |  |
| 296 | `set_encoder_for_connector` | found | display/takeover.c:i915_set_encoder_for_connector |  |
| 314 | `reset_encoder_connector_state` | found | display/takeover.c:i915_reset_encoder_connector_state |  |
| 339 | `reset_crtc_encoder_state` | found | display/takeover.c:i915_reset_crtc_encoder_state |  |
| 350 | `intel_modeset_update_connector_atomic_state` | found | display/takeover.c:i915_modeset_update_connector_atomic_state |  |
| 375 | `intel_crtc_copy_hw_to_uapi_state` | found | display/takeover.c:i915_crtc_copy_hw_to_uapi_state |  |
| 403 | `intel_sanitize_plane_mapping` | found | display/takeover.c:i915_sanitize_plane_mapping |  |
| 431 | `intel_crtc_has_encoders` | found | display/takeover.c:i915_crtc_has_encoders |  |
| 442 | `intel_encoder_find_connector` | found | display/takeover.c:i915_encoder_find_connector |  |
| 461 | `intel_sanitize_fifo_underrun_reporting` | found | display/takeover.c:i915_sanitize_fifo_underrun_reporting |  |
| 484 | `has_bogus_dpll_config` | found | display/takeover.c:i915_has_bogus_dpll_config |  |
| 504 | `intel_sanitize_crtc` | found | display/takeover.c:i915_sanitize_crtc |  |
| 556 | `intel_sanitize_all_crtcs` | found | display/takeover.c:i915_sanitize_all_crtcs |  |
| 592 | `intel_sanitize_encoder` | found | display/takeover.c:i915_sanitize_encoder |  |
| 675 | `readout_plane_state` | found | display/takeover.c:i915_readout_plane_state |  |
| 708 | `intel_modeset_readout_hw_state` | found | display/takeover.c:drv_i915_modeset_readout_hw_state |  |
| 919 | `get_encoder_power_domains` | found | display/takeover.c:i915_get_encoder_power_domains |  |
| 941 | `intel_early_display_was` | found | display/takeover.c:i915_early_display_was |  |
| 967 | `intel_modeset_setup_hw_state` | found | display/takeover.c:i915_modeset_setup_hw_state |  |

#### `parity/lcd/intel_opregion_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 307 | `asle_set_als_illum` | found | display/opregion.c:i915_asle_set_als_illum |  |
| 315 | `asle_set_backlight` | found | display/opregion.c:i915_asle_set_backlight |  |
| 356 | `asle_set_pwm_freq` | found | display/opregion.c:i915_asle_set_pwm_freq |  |
| 362 | `asle_set_pfit` | found | display/opregion.c:i915_asle_set_pfit |  |
| 370 | `asle_set_supported_rotation_angles` | found | display/opregion.c:i915_asle_set_supported_rotation_angles |  |
| 376 | `asle_set_button_array` | found | display/opregion.c:i915_asle_set_button_array |  |
| 400 | `asle_set_convertible` | found | display/opregion.c:i915_asle_set_convertible |  |
| 412 | `asle_set_docking` | found | display/opregion.c:i915_asle_set_docking |  |
| 423 | `asle_isct_state` | found | display/opregion.c:i915_asle_isct_state |  |
| 429 | `asle_work` | found | display/opregion.c:i915_asle_work |  |
| 481 | `intel_opregion_asle_intr` | found | display/opregion.c:i915_opregion_asle_intr |  |
| 493 | `intel_opregion_video_event` | found | display/opregion.c:i915_opregion_video_event |  |
| 515 | `check_swsci_function` | found | display/opregion.c:i915_check_swsci_function |  |
| 542 | `swsci` | found | display/opregion.c:i915_swsci |  |
| 616 | `intel_opregion_notify_adapter` | found | display/opregion.c:drv_i915_opregion_notify_adapter |  |
| 639 | `set_did` | found | display/opregion.c:i915_set_did |  |
| 653 | `intel_didl_outputs` | found | display/opregion.c:i915_didl_outputs |  |
| 692 | `intel_setup_cadls` | found | display/opregion.c:i915_setup_cadls |  |
| 722 | `intel_no_opregion_vbt_callback` | found | display/opregion.c:i915_no_opregion_vbt_callback |  |
| 729 | `intel_load_vbt_firmware` | found | display/opregion.c:i915_load_vbt_firmware |  |
| 769 | `intel_opregion_setup` | found | display/opregion.c:drv_i915_opregion_setup |  |
| 929 | `intel_opregion_register` | found | display/opregion.c:drv_i915_opregion_register |  |
| 945 | `intel_opregion_resume_display` | found | display/opregion.c:i915_opregion_resume_display |  |
| 971 | `intel_opregion_resume` | found | display/opregion.c:i915_opregion_resume |  |
| 984 | `intel_opregion_suspend_display` | found | display/opregion.c:i915_opregion_suspend_display |  |
| 997 | `intel_opregion_suspend` | found | display/opregion.c:i915_opregion_suspend |  |
| 1010 | `intel_opregion_unregister` | found | display/opregion.c:drv_i915_opregion_unregister |  |
| 1025 | `intel_opregion_cleanup` | found | display/opregion.c:drv_i915_opregion_cleanup |  |

#### `parity/lcd/intel_vblank_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 27 | `pipe_scanline_is_moving` | found | display/vblank.c:i915_pipe_scanline_is_moving |  |
| 40 | `wait_for_pipe_scanline_moving` | found | display/vblank.c:i915_wait_for_pipe_scanline_moving |  |
| 52 | `intel_wait_for_pipe_scanline_stopped` | found | display/vblank.c:drv_i915_wait_for_pipe_scanline_stopped |  |
| 57 | `intel_wait_for_pipe_scanline_moving` | found | display/vblank.c:drv_i915_wait_for_pipe_scanline_moving |  |
| 62 | `g4x_get_vblank_counter` | found | display/vblank.c:i915_g4x_get_vblank_counter |  |
| 78 | `__intel_get_crtc_scanline` | found | display/vblank.c:drv_i915_get_crtc_scanline | match by tokens |
| 134 | `intel_get_crtc_scanline` | found | display/vblank.c:drv_i915_get_crtc_scanline |  |
| 151 | `intel_crtc_scanline_offset` | found | display/vblank.c:i915_crtc_scanline_offset |  |
| 198 | `intel_crtc_update_active_timings` | found | display/vblank.c:drv_i915_crtc_update_active_timings |  |

#### `parity/lcd/intel_vrr_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 19 | `trans_vrr_ctl` | found | display/pipe.c:i915_trans_vrr_ctl |  |
| 32 | `intel_vrr_set_transcoder_timings` | found | display/pipe.c:i915_vrr_set_transcoder_timings |  |

#### `parity/lcd/intel_wm_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 20 | `intel_wm_plane_visible` | found | display/watermark.c:drv_i915_wm_plane_visible |  |

#### `parity/lcd/lcd_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 52 | `mul_u32_u32` | found | display/modeset-internal.h:mul_u32_u32 |  |
| 53 | `div_u64` | found | display/modeset-internal.h:i915_div_u64 |  |
| 54 | `roundup_pow_of_two` | found | display/modeset-internal.h:i915_roundup_pow_of_two |  |
| 220 | `drm_atomic_crtc_needs_modeset` | found | display/modeset-internal.h:drm_atomic_crtc_needs_modeset |  |
| 228 | `set_bit` | found | display/modeset-internal.h:i915_set_bit |  |
| 229 | `clear_bit` | found | display/modeset-internal.h:i915_clear_bit |  |
| 230 | `test_bit` | found | display/modeset-internal.h:test_bit |  |
| 231 | `bitmap_zero` | found | display/modeset-internal.h:i915_bitmap_zero |  |
| 233 | `bitmap_andnot` | found | display/modeset-internal.h:i915_bitmap_andnot |  |
| 235 | `bitmap_subset` | found | display/modeset-internal.h:i915_bitmap_subset |  |
| 254 | `drm_rect_width` | found | display/modeset-internal.h:i915_drm_rect_width |  |
| 255 | `drm_rect_height` | found | display/modeset-internal.h:i915_drm_rect_height |  |
| 441 | `parity_lcd_trans_offset` | found | display/modeset-internal.h:i915_lcd_trans_offset |  |
| 459 | `to_i915` | found | display/modeset-internal.h:i915_lcd_to_i915 |  |

#### `parity/lcd/lcd_dp_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 31 | `parity_lcd_dpcd_read` | found | display/modeset-internal.h:i915_lcd_dpcd_read |  |
| 36 | `parity_lcd_dpcd_write` | found | display/modeset-internal.h:i915_lcd_dpcd_write |  |
| 43 | `parity_lcd_dpcd_readb` | found | display/modeset-internal.h:i915_lcd_dpcd_readb |  |
| 44 | `parity_lcd_dpcd_writeb` | found | display/modeset-internal.h:i915_lcd_dpcd_writeb |  |
| 48 | `parity_lcd_dpcd_probe` | found | display/modeset-internal.h:i915_lcd_dpcd_probe |  |

#### `parity/lcd/lcd_dp_helper_inlines.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 33 | `drm_dp_tps3_supported` | found | data/display-dp-helper-inlines.inc:drm_dp_tps3_supported |  |
| 40 | `drm_dp_tps4_supported` | found | data/display-dp-helper-inlines.inc:drm_dp_tps4_supported |  |
| 47 | `drm_dp_is_branch` | found | data/display-dp-helper-inlines.inc:drm_dp_is_branch |  |

#### `parity/lcd/lcd_drm_fourcc.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 940 | `drm_fourcc_canonicalize_nvidia_format_mod` | found | data/display-drm-fourcc.inc:drm_fourcc_canonicalize_nvidia_format_mod |  |

#### `parity/lcd/lcd_drm_plane_defs.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 32 | `drm_rotation_90_or_270` | found | data/display-drm-plane-defs.inc:drm_rotation_90_or_270 |  |

#### `parity/lcd/lcd_fake_hw.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 49 | `slices_of_range` | test | T3 | S5 T3 (destination not present yet) |
| 63 | `dc_off_held` | test | T3 | S5 T3 (destination not present yet) |
| 68 | `slot` | test | T3 | S5 T3 (destination not present yet) |
| 86 | `lcd_fake_reg` | test | T3 | S5 T3 (destination not present yet) |
| 96 | `pll_locked` | test | T3 | S5 T3 (destination not present yet) |
| 101 | `ddi_clock_on` | test | T3 | S5 T3 (destination not present yet) |
| 107 | `frame_of` | test | T3 | S5 T3 (destination not present yet) |
| 113 | `to_next_frame` | test | T3 | S5 T3 (destination not present yet) |
| 121 | `f_read32` | test | T3 | S5 T3 (destination not present yet) |
| 156 | `f_write32` | test | T3 | S5 T3 (destination not present yet) |
| 260 | `f_rmw32` | test | T3 | S5 T3 (destination not present yet) |
| 268 | `f_wait_reg` | test | T3 | S5 T3 (destination not present yet) |
| 283 | `f_sleep` | test | T3 | S5 T3 (destination not present yet) |
| 290 | `f_dpcd_read` | test | T3 | S5 T3 (destination not present yet) |
| 296 | `f_dpcd_write` | test | T3 | S5 T3 (destination not present yet) |
| 302 | `f_read_dpcd_caps` | test | T3 | S5 T3 (destination not present yet) |
| 308 | `f_panel` | test | T3 | S5 T3 (destination not present yet) |
| 314 | `f_power_get` | test | T3 | S5 T3 (destination not present yet) |
| 325 | `f_power_put` | test | T3 | S5 T3 (destination not present yet) |
| 345 | `f_power_put_async` | test | T3 | S5 T3 (destination not present yet) |
| 355 | `f_dbuf_slices_update` | test | T3 | S5 T3 (destination not present yet) |
| 375 | `f_vblank_get` | test | T3 | S5 T3 (destination not present yet) |
| 384 | `f_vblank_put` | test | T3 | S5 T3 (destination not present yet) |
| 395 | `f_vblank_sleep` | test | T3 | S5 T3 (destination not present yet) |
| 413 | `f_irq_off` | test | T3 | S5 T3 (destination not present yet) |
| 423 | `f_irq_on` | test | T3 | S5 T3 (destination not present yet) |
| 432 | `f_arm_event` | test | T3 | S5 T3 (destination not present yet) |
| 441 | `f_wait_event` | test | T3 | S5 T3 (destination not present yet) |
| 461 | `f_cancel_event` | test | T3 | S5 T3 (destination not present yet) |
| 471 | `f_observe` | test | T3 | S5 T3 (destination not present yet) |
| 481 | `f_lock` | test | T3 | S5 T3 (destination not present yet) |
| 492 | `f_step` | test | T3 | S5 T3 (destination not present yet) |
| 495 | `sink_on_dpcd_write` | test | T3 | S5 T3 (destination not present yet) |
| 545 | `lcd_fake_init` | test | T3 | S5 T3 (destination not present yet) |
| 594 | `lcd_fake_power_refs_total` | test | T3 | S5 T3 (destination not present yet) |
| 603 | `lcd_fake_violations` | test | T3 | S5 T3 (destination not present yet) |

#### `parity/lcd/lcd_hw_check.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 21 | `parity_lcd_scanout_hw_check` | test-moved | src/drivers/gpu/i915/tests/display/aux.c:i915_aux_scanout_hw_check (function containing the matched text) | S5 T4b |

#### `parity/lcd/lcd_i915_fixed.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 21 | `is_fixed16_zero` | found | data/display-i915-fixed.inc:is_fixed16_zero |  |
| 26 | `u32_to_fixed16` | found | data/display-i915-fixed.inc:u32_to_fixed16 |  |
| 35 | `fixed16_to_u32_round_up` | found | data/display-i915-fixed.inc:fixed16_to_u32_round_up |  |
| 40 | `fixed16_to_u32` | found | data/display-i915-fixed.inc:fixed16_to_u32 |  |
| 45 | `min_fixed16` | found | data/display-i915-fixed.inc:min_fixed16 |  |
| 53 | `max_fixed16` | found | data/display-i915-fixed.inc:max_fixed16 |  |
| 61 | `clamp_u64_to_fixed16` | found | data/display-i915-fixed.inc:clamp_u64_to_fixed16 |  |
| 70 | `div_round_up_fixed16` | found | data/display-i915-fixed.inc:div_round_up_fixed16 |  |
| 76 | `mul_round_up_u32_fixed16` | found | data/display-i915-fixed.inc:mul_round_up_u32_fixed16 |  |
| 87 | `mul_fixed16` | found | data/display-i915-fixed.inc:mul_fixed16 |  |
| 98 | `div_fixed16` | found | data/display-i915-fixed.inc:div_fixed16 |  |
| 108 | `div_round_up_u32_fixed16` | found | data/display-i915-fixed.inc:div_round_up_u32_fixed16 |  |
| 119 | `mul_u32_fixed16` | found | data/display-i915-fixed.inc:mul_u32_fixed16 |  |
| 128 | `add_fixed16` | found | data/display-i915-fixed.inc:add_fixed16 |  |
| 138 | `add_fixed16_u32` | found | data/display-i915-fixed.inc:add_fixed16_u32 |  |

#### `parity/lcd/lcd_link_training_inlines.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 15 | `intel_dp_training_pattern_symbol` | found | data/display-link-training-inlines.inc:intel_dp_training_pattern_symbol |  |

#### `parity/lcd/lcd_modeset_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 87 | `str_on_off` | found | display/modeset-internal.h:i915_lcd_str_on_off |  |
| 88 | `str_enable_disable` | found | display/modeset-internal.h:i915_str_enable_disable |  |

#### `parity/lcd/lcd_modeset_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 30 | `bring_up` | test-moved | plan/ws031/tests/lcd-modeset-host-test.c:bring_up | S5 T4b |
| 73 | `released` | test-moved | plan/ws031/tests/dp-host-test.c:released | S5 T4b |
| 82 | `parity_lcd_modeset_ktest` | test | T4b | S5 T4b (destination not present yet) |

#### `parity/lcd/lcd_pattern.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 15 | `in_rect` | test | T4b | S5 T4b (destination not present yet) |
| 24 | `in_digit` | test | T4b | S5 T4b (destination not present yet) |
| 41 | `parity_lcd_pattern_pixel` | found | display/diagnostics.c:drv_i915_lcd_pattern_pixel | ledger said test; kept in production (e.g. [s4 §4](i915-rebuild-s4.md) L278) |
| 99 | `parity_lcd_pattern_fill` | found | display/diagnostics.c:drv_i915_lcd_pattern_fill | ledger said test; kept in production (e.g. [s4 §4](i915-rebuild-s4.md) L278) |
| 121 | `parity_lcd_pattern_verify` | found | display/diagnostics.c:drv_i915_lcd_pattern_verify | ledger said test; kept in production (e.g. [s4 §4](i915-rebuild-s4.md) L278) |

#### `parity/lcd/lcd_ref_inlines.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 10 | `transcoder_is_dsi` | found | data/display-ref-inlines.inc:transcoder_is_dsi |  |
| 17 | `intel_crtc_has_type` | found | data/display-ref-inlines.inc:intel_crtc_has_type |  |
| 24 | `intel_crtc_has_dp_encoder` | found | data/display-ref-inlines.inc:intel_crtc_has_dp_encoder |  |
| 33 | `intel_crtc_needs_modeset` | found | data/display-ref-inlines.inc:intel_crtc_needs_modeset |  |

#### `parity/lcd/lcd_show_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 47 | `locked` | test | T4b | S5 T4b (destination not present yet) |
| 59 | `bring_up` | test-moved | plan/ws031/tests/lcd-modeset-host-test.c:bring_up | S5 T4b |
| 112 | `edp_released` | test | T4b | S5 T4b (destination not present yet) |
| 121 | `live_ptes` | test | T4b | S5 T4b (destination not present yet) |
| 131 | `fail_in_window` | test | T4b | S5 T4b (destination not present yet) |
| 138 | `stick_the_pipe` | test | T4b | S5 T4b (destination not present yet) |
| 145 | `parity_lcd_show_ktest` | test | T4b | S5 T4b (destination not present yet) |

#### `parity/lcd/lcd_wm_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 34 | `drm_format_info` | found | display/watermark-internal.h:i915_drm_format_info |  |

#### `parity/lcd/lcd_wm_ddb_types.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 19 | `skl_ddb_entry_size` | found | data/display-wm-ddb-types.inc:skl_ddb_entry_size |  |
| 24 | `skl_ddb_entry_equal` | found | data/display-wm-ddb-types.inc:skl_ddb_entry_equal |  |

#### `parity/lcd/lcdg_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 36 | `tf_read` | test | T4b | S5 T4b (destination not present yet) |
| 58 | `tf_write` | test | T4b | S5 T4b (destination not present yet) |
| 71 | `tf_fw_request` | test | T4b | S5 T4b (destination not present yet) |
| 72 | `tf_fw_ack` | test | T4b | S5 T4b (destination not present yet) |
| 88 | `map_draw` | test | T4b | S5 T4b (destination not present yet) |
| 119 | `present_ptes` | test | T4b | S5 T4b (destination not present yet) |
| 134 | `present_at` | test | T4b | S5 T4b (destination not present yet) |
| 147 | `parity_lcdg_ktest` | test | T4b | S5 T4b (destination not present yet) |

#### `parity/lcd/n1_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 61 | `parity_n1_power_put` | found | display/takeover-internal.h:i915_n1_power_put |  |
| 90 | `drm_rect_init` | found | display/takeover-internal.h:i915_drm_rect_init |  |
| 286 | `drm_atomic_set_mode_for_crtc` | found | display/takeover-internal.h:i915_drm_atomic_set_mode_for_crtc |  |
| 344 | `parity_n1_bitmap_empty` | found | display/takeover-internal.h:i915_n1_bitmap_empty |  |

#### `parity/lcd/opregion_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 43 | `parity_opregion_work_trampoline` | found | display/opregion-internal.h:i915_opregion_work_trampoline |  |
| 52 | `parity_opregion_queue_work` | found | display/opregion-internal.h:i915_opregion_queue_work |  |

#### `parity/lcd/opregion_fwtest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 35 | `fw_backlight` | test | T4b | S5 T4b (destination not present yet) |
| 42 | `log_mbox` | test | T4b | S5 T4b (destination not present yet) |
| 54 | `parity_opregion_fw_test` | test | T4b | S5 T4b (destination not present yet) |
| 151 | `parity_opregion_fw_log_again` | test | T4b | S5 T4b (destination not present yet) |

#### `parity/lcd/opregion_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 23 | `rd32` | test | T4b | S5 T4b (destination not present yet) |
| 24 | `wr32` | test | T4b | S5 T4b (destination not present yet) |
| 25 | `wr16v` | test | T4b | S5 T4b (destination not present yet) |
| 43 | `shadow_init` | test-moved | tests/display/opregion-ktest.c:i915_shadow_init | S5 T4b |
| 56 | `shadow_vbt_init` | test-moved | tests/display/opregion-ktest.c:i915_shadow_vbt_init | S5 T4b |
| 75 | `other_cb` | test | T4b | S5 T4b (destination not present yet) |
| 82 | `log_dispatch` | test-moved | tests/display/opregion-ktest.c:i915_log_dispatch | S5 T4b |
| 105 | `fake_backlight` | test-moved | tests/display/opregion-ktest.c:i915_fake_backlight | S5 T4b |
| 114 | `asle_request` | test-moved | tests/display/opregion-ktest.c:i915_asle_request | S5 T4b |
| 122 | `log_asle` | test-moved | tests/display/opregion-ktest.c:i915_log_asle | S5 T4b |
| 134 | `asle_tests` | test-moved | tests/display/opregion-ktest.c:i915_asle_tests | S5 T4b |
| 185 | `slow_cb` | test | T4b | S5 T4b (destination not present yet) |
| 195 | `dispatch_fn` | test | T4b | S5 T4b (destination not present yet) |
| 203 | `blocking_backlight` | test-moved | tests/display/opregion-ktest.c:i915_blocking_backlight | S5 T4b |
| 216 | `blocker_fn` | test | T4b | S5 T4b (destination not present yet) |
| 223 | `start_service` | test-moved | tests/display/opregion-ktest.c:i915_start_service | S5 T4b |
| 233 | `lifecycle_tests` | test-moved | tests/display/opregion-ktest.c:i915_lifecycle_tests | S5 T4b |
| 323 | `parity_opregion_ktest` | test-moved | src/drivers/gpu/i915/tests/display/opregion-ktest.c:drv_i915_display_ktest_opregion (function containing the matched text) | S5 T4b |

#### `parity/lcd/parity_atomic_plane_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 9 | `parity_lcd_ms_plane_data_rates` | found | display/plane.c:drv_i915_lcd_ms_plane_data_rates |  |

#### `parity/lcd/parity_backlight_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 31 | `parity_lcd_ms_backlight_setup` | found | display/panel-backlight.c:drv_i915_lcd_ms_backlight_setup |  |
| 55 | `parity_lcd_ms_set_brightness` | found | display/panel-backlight.c:drv_i915_lcd_ms_set_brightness |  |
| 62 | `parity_lcd_ms_set_acpi` | found | display/panel-backlight.c:drv_i915_lcd_ms_set_acpi |  |
| 69 | `parity_lcd_ms_backlight_power` | found | display/panel-backlight.c:drv_i915_lcd_ms_backlight_power |  |
| 79 | `parity_lcd_ms_user_level` | found | display/panel-backlight.c:drv_i915_lcd_ms_user_level |  |

#### `parity/lcd/parity_buf_trans_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 9 | `parity_lcd_ms_bind_buf_trans` | found | display/phy.c:drv_i915_lcd_ms_bind_buf_trans |  |

#### `parity/lcd/parity_bw_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 8 | `parity_lcd_ms_bw_min_cdclk` | found | display/watermark.c:drv_i915_lcd_ms_bw_min_cdclk |  |
| 13 | `parity_lcd_ms_bw_data_rate` | found | display/watermark.c:drv_i915_lcd_ms_bw_data_rate |  |

#### `parity/lcd/parity_cdclk_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 12 | `parity_lcd_ms_cdclk_check` | found | display/clock.c:drv_i915_lcd_ms_cdclk_check |  |

#### `parity/lcd/parity_color_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 19 | `parity_lcd_ms_color_funcs` | found | display/color.c:drv_i915_lcd_ms_color_funcs |  |
| 24 | `parity_lcd_ms_color_check` | found | display/color.c:drv_i915_lcd_ms_color_check |  |

#### `parity/lcd/parity_ddi_emit_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 11 | `parity_ddi_emit` | found | display/ddi.c:drv_i915_ddi_emit |  |
| 69 | `parity_lcd_hdmi_level_shift` | found | display/ddi.c:drv_i915_lcd_hdmi_level_shift |  |
| 75 | `parity_lcd_ms_bound_port` | found | display/ddi.c:drv_i915_lcd_ms_bound_port |  |
| 86 | `parity_lcd_ms_bind_readout` | found | display/ddi.c:drv_i915_lcd_ms_bind_readout |  |
| 94 | `parity_lcd_ms_bound_encoder` | found | display/ddi.c:drv_i915_lcd_ms_bound_encoder |  |
| 99 | `parity_lcd_ms_bound_connector` | found | display/ddi.c:drv_i915_lcd_ms_bound_connector |  |
| 104 | `parity_lcd_ms_bind_encoder` | found | display/ddi.c:drv_i915_lcd_ms_bind_encoder |  |
| 143 | `intel_encoders_pre_pll_enable` | found | display/ddi.c:drv_i915_encoders_pre_pll_enable |  |
| 148 | `intel_encoders_pre_enable` | found | display/ddi.c:drv_i915_encoders_pre_enable |  |
| 153 | `intel_encoders_enable` | found | display/ddi.c:drv_i915_encoders_enable |  |
| 158 | `intel_encoders_disable` | found | display/ddi.c:drv_i915_encoders_disable |  |
| 163 | `intel_encoders_post_disable` | found | display/ddi.c:drv_i915_encoders_post_disable |  |
| 168 | `intel_encoders_post_pll_disable` | found | display/ddi.c:drv_i915_encoders_post_pll_disable |  |

#### `parity/lcd/parity_ddi_hotplug_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 17 | `intel_port_to_phy` | found | display/takeover.c:drv_i915_port_to_phy |  |
| 24 | `intel_phy_is_tc` | found | display/takeover.c:drv_i915_phy_is_tc |  |
| 30 | `intel_tc_port_link_reset` | found | display/hotplug.c:i915_hpd_intel_tc_port_link_reset |  |
| 38 | `intel_dp_phy_test` | found | display/hotplug.c:i915_hpd_intel_dp_phy_test |  |
| 43 | `intel_dp_retrain_link` | found | display/hotplug.c:i915_hpd_intel_dp_retrain_link |  |
| 54 | `intel_hdmi_reset_link` | found | display/hotplug.c:i915_hdmi_reset_link |  |

#### `parity/lcd/parity_display_emit_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 12 | `parity_display_emit_transcoder` | found | display/pipe.c:drv_i915_display_emit_transcoder |  |
| 47 | `parity_display_emit_cpu_transcoder` | found | display/pipe.c:drv_i915_display_emit_cpu_transcoder |  |
| 105 | `parity_lcd_ms_display_funcs` | found | display/pipe.c:drv_i915_lcd_ms_display_funcs |  |
| 110 | `parity_lcd_ms_crtc_enable` | found | display/pipe.c:drv_i915_lcd_ms_crtc_enable |  |
| 119 | `parity_lcd_ms_crtc_disable` | found | display/pipe.c:drv_i915_lcd_ms_crtc_disable |  |

#### `parity/lcd/parity_dpll_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 12 | `parity_icl_hdmi_wrpll` | found | display/clock.c:drv_i915_icl_hdmi_wrpll |  |
| 38 | `parity_icl_dp_combo_pll` | found | display/clock.c:drv_i915_icl_dp_combo_pll |  |
| 96 | `parity_lcd_shared_dpll_state` | found | display/clock.c:drv_i915_lcd_shared_dpll_state |  |
| 102 | `parity_lcd_dpll_pool_bind` | found | display/clock.c:drv_i915_lcd_dpll_pool_bind |  |
| 123 | `parity_lcd_dpll_pool_init` | found | display/clock.c:i915_lcd_dpll_pool_init |  |
| 134 | `parity_lcd_ms_release_pipe` | found | display/clock.c:drv_i915_lcd_ms_release_pipe |  |
| 145 | `parity_lcd_dplls_reset` | found | display/clock.c:drv_i915_lcd_dplls_reset |  |
| 156 | `parity_lcd_ms_alloc_pll` | found | display/clock.c:drv_i915_lcd_ms_alloc_pll |  |
| 173 | `parity_lcd_ms_release_pll` | found | display/clock.c:drv_i915_lcd_ms_release_pll |  |
| 183 | `parity_lcd_ms_bind_pll` | found | display/clock.c:drv_i915_lcd_ms_bind_pll |  |

#### `parity/lcd/parity_edid_mode_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 15 | `drm_mode_create` | found | display/edid.c:i915_edid_mode_create | match by tokens |
| 25 | `drm_mode_set_name` | found | display/edid.c:drv_i915_lcd_drm_mode_set_name |  |
| 30 | `parity_edid_preferred_mode` | found | display/edid.c:drv_i915_edid_preferred_mode |  |

#### `parity/lcd/parity_flip_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `parity_lcd_ms_crtc_funcs` | found | display/vblank.c:drv_i915_lcd_ms_crtc_funcs |  |
| 23 | `parity_lcd_ms_active_timings` | found | display/vblank.c:drv_i915_lcd_ms_active_timings |  |
| 35 | `parity_lcd_irq_disable` | found | display/vblank.c:drv_i915_lcd_irq_disable |  |
| 42 | `parity_lcd_irq_enable` | found | display/vblank.c:drv_i915_lcd_irq_enable |  |
| 49 | `parity_lcd_irq_save` | found | display/vblank.c:drv_i915_lcd_irq_save |  |
| 57 | `parity_lcd_irq_restore` | found | display/vblank.c:drv_i915_lcd_irq_restore |  |
| 64 | `parity_lcd_ms_evade_window` | found | display/vblank.c:drv_i915_lcd_ms_evade_window |  |
| 75 | `parity_crtc_state_size_crtc_unit` | found | display/vblank.c:drv_i915_crtc_state_size_crtc_unit |  |

#### `parity/lcd/parity_gmbus_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `hpd_gmbus_xfer_locked` | found | display/gmbus.c:i915_hpd_gmbus_xfer_locked |  |
| 29 | `hpd_bit_xfer_step` | found | display/gmbus.c:i915_hpd_bit_xfer_step |  |
| 36 | `parity_hpd_gmbus_adapter` | found | display/gmbus.c:drv_i915_hpd_gmbus_adapter |  |
| 68 | `parity_hpd_gmbus_forget` | found | display/gmbus.c:drv_i915_hpd_gmbus_forget |  |

#### `parity/lcd/parity_hdmi_detect_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 28 | `hpd_conn_index` | found | display/hdmi.c:i915_hpd_conn_index |  |
| 33 | `drm_edid_read_ddc` | found | display/hdmi.c:i915_hpd_drm_edid_read_ddc |  |
| 76 | `drm_edid_connector_update` | found | display/hdmi.c:i915_hpd_drm_edid_connector_update |  |
| 94 | `drm_edid_is_digital` | found | display/hdmi.c:i915_hpd_drm_edid_is_digital |  |
| 99 | `drm_edid_free` | found | display/hdmi.c:i915_hpd_drm_edid_free |  |
| 104 | `intel_hdmi_dp_dual_mode_detect` | found | display/hdmi.c:i915_hdmi_dp_dual_mode_detect |  |
| 112 | `parity_hpd_hdmi_connector_funcs` | found | display/hdmi.c:drv_i915_hpd_hdmi_connector_funcs |  |
| 117 | `parity_hpd_edid_info` | found | display/hdmi.c:drv_i915_hpd_edid_info |  |
| 124 | `parity_hpd_edid_bytes` | found | display/hdmi.c:drv_i915_hpd_edid_bytes |  |
| 134 | `parity_hpd_edid_forget` | found | display/hdmi.c:drv_i915_hpd_edid_forget |  |

#### `parity/lcd/parity_hdmi_mode_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 11 | `parity_lcd_hdmi_tmds_output` | found | display/hdmi-mode.c:drv_i915_lcd_hdmi_tmds_output |  |

#### `parity/lcd/parity_hotplug_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 25 | `parity_hpd_model_allowed` | found | display/hotplug.c:drv_i915_hpd_model_allowed |  |
| 62 | `hpd_status_name` | found | display/hotplug.c:i915_hpd_status_name |  |
| 67 | `parity_hpd_warn` | found | display/hotplug.c:drv_i915_hpd_warn |  |
| 76 | `parity_hpd_sync_deadline` | found | display/hotplug.c:drv_i915_hpd_sync_deadline |  |
| 85 | `parity_hpd_read` | found | display/hotplug.c:drv_i915_hpd_read |  |
| 115 | `parity_hpd_rmw` | found | display/hotplug.c:drv_i915_hpd_rmw |  |
| 136 | `hpd_fake_gmbus_write` | found | display/hotplug.c:i915_hpd_fake_gmbus_write |  |
| 170 | `parity_hpd_write` | found | display/hotplug.c:drv_i915_hpd_write |  |
| 181 | `parity_hpd_encoder_at` | found | display/hotplug.c:drv_i915_hpd_encoder_at |  |
| 186 | `parity_hpd_connector_next` | found | display/hotplug.c:drv_i915_hpd_connector_next |  |
| 192 | `intel_display_power_get` | found | display/power.c:drv_i915_display_power_get |  |
| 200 | `intel_display_power_put` | found | display/power.c:drv_i915_display_power_put |  |
| 209 | `intel_hpd_irq_setup` | found | display/hotplug.c:drv_i915_hpd_irq_setup |  |
| 222 | `parity_hpd_gmbus_woken` | found | display/hotplug.c:drv_i915_hpd_gmbus_woken |  |
| 227 | `intel_irqs_enabled` | found | display/interrupts.c:i915_irqs_enabled |  |
| 232 | `drm_kms_helper_poll_reschedule` | found | display/hotplug.c:i915_hpd_drm_kms_helper_poll_reschedule |  |
| 238 | `drm_kms_helper_connector_hotplug_event` | found | display/hotplug.c:i915_hpd_drm_kms_helper_connector_hotplug_event |  |
| 243 | `drm_kms_helper_hotplug_event` | found | display/hotplug.c:i915_hpd_drm_kms_helper_hotplug_event |  |
| 248 | `i915_hpd_poll_init_work` | found | display/hotplug.c:i915_hpd_poll_init_work |  |
| 253 | `hpd_dp_pulse_step` | found | display/hotplug.c:i915_hpd_dp_pulse_step |  |
| 262 | `hpd_dp_detect_step` | found | display/hotplug.c:i915_hpd_dp_detect_step |  |
| 272 | `hpd_tc_connected_step` | found | display/hotplug.c:i915_hpd_tc_connected_step |  |
| 279 | `hpd_hotplug_recorded` | found | display/hotplug.c:i915_hpd_hotplug_recorded |  |
| 315 | `parity_hpd_work_trampoline` | found | display/hotplug.c:drv_i915_hpd_work_trampoline |  |
| 329 | `hpd_make_objects` | found | display/hotplug.c:i915_hpd_make_objects |  |
| 403 | `parity_hpd_start` | found | display/hotplug.c:drv_i915_hpd_start |  |
| 470 | `parity_hpd_pch_irq` | found | display/hotplug.c:drv_i915_hpd_pch_irq |  |
| 484 | `parity_hpd_model_irq` | found | display/hotplug.c:drv_i915_hpd_model_irq |  |
| 499 | `hpd_icp_entry` | found | display/hotplug.c:i915_hpd_icp_entry |  |
| 525 | `parity_hpd_stop` | found | display/hotplug.c:drv_i915_hpd_stop |  |
| 547 | `parity_hpd_probe_connector` | found | display/hotplug.c:drv_i915_hpd_probe_connector |  |
| 563 | `parity_hpd_connector_name` | found | display/hotplug.c:drv_i915_hpd_connector_name |  |
| 568 | `parity_hpd_summary` | found | display/hotplug.c:drv_i915_hpd_summary |  |
| 584 | `parity_hpd_irq_record` | found | display/hotplug.c:drv_i915_hpd_irq_record |  |
| 589 | `parity_hpd_hotplug_record` | found | display/hotplug.c:drv_i915_hpd_hotplug_record |  |
| 595 | `parity_hpd_flush_reenable` | found | display/hotplug.c:drv_i915_hpd_flush_reenable |  |
| 602 | `parity_hpd_event_bits` | found | display/hotplug.c:drv_i915_hpd_event_bits |  |
| 603 | `parity_hpd_retry_bits` | found | display/hotplug.c:drv_i915_hpd_retry_bits |  |
| 604 | `parity_hpd_pin_state` | found | display/hotplug.c:drv_i915_hpd_pin_state |  |
| 605 | `parity_hpd_pin_count` | found | display/hotplug.c:drv_i915_hpd_pin_count |  |
| 606 | `parity_hpd_connector_polled` | found | display/hotplug.c:drv_i915_hpd_connector_polled |  |
| 607 | `parity_hpd_connector_status` | found | display/hotplug.c:drv_i915_hpd_connector_status |  |

#### `parity/lcd/parity_hpd_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 23 | `st_name` | test | T4b | S5 T4b (destination not present yet) |
| 28 | `parity_hpd_test_run` | test-moved | tests/display/hdmi-hotplug.c:i915_hpd_test_run | S5 T4b |
| 102 | `parity_hdmi_edid_test_run` | test-moved | src/drivers/gpu/i915/tests/display/hdmi-hotplug.c:i915_hdmi_edid_run (function containing the matched text) | S5 T4b |

#### `parity/lcd/parity_lcd_calc.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 29 | `parity_lcd_error` | found | display/state.c:drv_i915_lcd_error |  |
| 36 | `parity_lcd_errors` | found | display/state.c:drv_i915_lcd_errors |  |
| 41 | `parity_lcd_error_bind` | found | display/state.c:drv_i915_lcd_error_bind |  |
| 52 | `parity_lcd_note` | found | display/state.c:drv_i915_lcd_note |  |
| 60 | `rate_from_bw_code` | found | display/state.c:i915_state_rate_from_bw_code | match by substr |
| 71 | `parity_lcd_compute` | found | display/state.c:drv_i915_lcd_compute |  |
| 148 | `parity_lcd_compute_hdmi` | found | display/state.c:drv_i915_lcd_compute_hdmi |  |
| 166 | `record_write` | found | display/state.c:i915_state_record_write | match by substr |
| 182 | `record_rmw` | found | display/state.c:i915_state_record_rmw | match by substr |
| 199 | `record_step` | found | display/state.c:i915_state_record_step | match by substr |
| 215 | `parity_lcd_emit_plane` | found | display/state.c:drv_i915_lcd_emit_plane |  |
| 235 | `parity_lcd_words_step` | found | display/state.c:drv_i915_lcd_words_step |  |
| 245 | `parity_lcd_words_find` | found | display/state.c:drv_i915_lcd_words_find |  |
| 264 | `state_to_mode` | found | display/state.c:i915_state_to_mode |  |
| 280 | `parity_lcd_emit_cpu_transcoder` | found | display/state.c:drv_i915_lcd_emit_cpu_transcoder |  |
| 304 | `parity_lcd_emit_ddi` | found | display/state.c:drv_i915_lcd_emit_ddi |  |
| 331 | `parity_lcd_emit_transcoder` | found | display/state.c:drv_i915_lcd_emit_transcoder |  |

#### `parity/lcd/parity_lcd_kernel.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 134 | `k_note_sink` | found | display/diagnostics.c:drv_i915_lcd_kernel_note_sink |  |
| 139 | `k_read32` | found | mmio.c:drv_i915_read32 | match by tokens |
| 155 | `k_write32` | found | mmio.c:drv_i915_write32 | match by tokens |
| 162 | `k_rmw32` | found | mmio.c:drv_i915_rmw32 | match by tokens |
| 171 | `k_posting_read` | found | display/modeset.c:i915_kernel_posting_read | match by tokens |
| 177 | `k_wait_reg` | found | display/modeset.c:i915_kernel_wait_reg (function containing the matched text) | match by string |
| 195 | `k_usleep` | found | display/modeset.c:i915_kernel_usleep (function containing the matched text) | match by string |
| 207 | `k_udelay` | found | display/modeset.c:i915_kernel_udelay (function containing the matched text) | match by string |
| 217 | `k_dpcd_read` | found | display/diagnostics.c:i915_trace_dpcd_read | match by tokens |
| 223 | `k_dpcd_write` | found | display/diagnostics.c:i915_trace_dpcd_write | match by tokens |
| 229 | `k_read_dpcd_caps` | found | display/diagnostics.c:i915_trace_read_dpcd_caps | match by tokens |
| 235 | `k_panel` | found | display/diagnostics.c:i915_trace_panel | match by tokens |
| 241 | `k_power_get` | found | display/power.c:drv_i915_lcd_power_get | match by tokens |
| 264 | `k_power_get_if_enabled` | found | display/power.c:drv_i915_lcd_power_get_if_enabled | match by tokens |
| 275 | `k_power_put` | found | display/power.c:drv_i915_lcd_power_put (function containing the matched text) | match by string |
| 296 | `k_power_put_async` | found | display/power.c:drv_i915_lcd_power_put (function containing the matched text) | match by string |
| 310 | `k_dbuf_slices_update` | found | display/modeset.c:i915_kernel_dbuf_slices_update | match by tokens |
| 317 | `k_lock` | found | display/modeset.c:i915_kernel_lock | match by tokens |
| 331 | `k_step` | found | display/diagnostics.c:drv_i915_lcd_kernel_step |  |
| 345 | `k_error` | found | display/diagnostics.c:drv_i915_lcd_kernel_error |  |
| 357 | `k_debug` | found | display/diagnostics.c:drv_i915_lcd_kernel_debug |  |
| 364 | `k_frame` | found | display/vblank.c:i915_lcd_kernel_frame |  |
| 371 | `k_vblank_get` | found | display/vblank.c:drv_i915_lcd_kernel_vblank_get |  |
| 378 | `k_vblank_put` | found | display/vblank.c:drv_i915_lcd_kernel_vblank_put |  |
| 386 | `k_vblank_sleep` | found | display/vblank.c:drv_i915_lcd_kernel_vblank_sleep |  |
| 404 | `k_irq_off` | found | display/modeset.c:i915_kernel_irq_off | match by tokens |
| 411 | `k_irq_on` | found | display/modeset.c:i915_kernel_irq_on | match by tokens |
| 420 | `k_arm_event` | found | display/vblank.c:drv_i915_lcd_kernel_arm_event |  |
| 429 | `k_wait_event` | found | display/vblank.c:drv_i915_lcd_kernel_wait_event |  |
| 447 | `k_cancel_event` | found | display/vblank.c:drv_i915_lcd_kernel_cancel_event |  |
| 457 | `bind_ops` | found | display/modeset.c:drv_i915_lcd_kernel_bind_ops | match by substr |
| 493 | `stage_name` | test | T4b | S5 T4b (destination not present yet) |
| 501 | `log_regs` | found | display/diagnostics.c:drv_i915_lcd_log_regs |  |
| 534 | `probe_scanout` | test-moved | src/drivers/gpu/i915/tests/display/lcd-run.c:i915_test_lcd_probe_scanout (function containing the matched text) | S5 T4b |
| 628 | `at_stage` | test | T4b | S5 T4b (destination not present yet) |
| 649 | `kind_name` | found | display/diagnostics.c:i915_trace_kind_name | match by substr |
| 657 | `log_trace` | found | display/diagnostics.c:drv_i915_lcd_log_trace |  |
| 678 | `log_observer` | found | display/diagnostics.c:drv_i915_lcd_log_observer |  |
| 694 | `log_status` | found | display/diagnostics.c:drv_i915_lcd_log_status |  |
| 708 | `fill_cfg` | found | display/modeset.c:drv_i915_lcd_kernel_fill_cfg (function containing the matched text) | match by string |
| 785 | `preflight` | found | display/modeset.c:drv_i915_lcd_kernel_preflight (function containing the matched text) | match by string |
| 869 | `lcd_run_one` | test | T4b | S5 T4b (destination not present yet) |
| 1013 | `parity_lcd_kernel_lcdb_run` | test | T4b | S5 T4b (destination not present yet) |
| 1032 | `hdmib_window` | test-moved | src/drivers/gpu/i915/tests/display/hdmi-output.c:i915_test_hdmib_window (function containing the matched text) | S5 T4b |
| 1052 | `parity_lcd_kernel_hdmib_run` | test | T4b | S5 T4b (destination not present yet) |
| 1083 | `dual_frame` | test | T4b | S5 T4b (destination not present yet) |
| 1102 | `dual_bring_up` | test-moved | src/drivers/gpu/i915/tests/display/hdmi-output.c:i915_test_dual_bring_up (function containing the matched text) | S5 T4b |
| 1146 | `dual_log_state` | test-moved | src/drivers/gpu/i915/tests/display/hdmi-output.c:i915_test_dual_log_state (function containing the matched text) | S5 T4b |
| 1159 | `dual_stop` | test | T4b | S5 T4b (destination not present yet) |
| 1186 | `dual_release` | test-moved | src/drivers/gpu/i915/tests/display/hdmi-output.c:drv_i915_test_display_dual_share (function containing the matched text) | S5 T4b |
| 1200 | `parity_lcd_kernel_dual_run` | test | T4b | S5 T4b (destination not present yet) |
| 1351 | `parity_lcd_kernel_dual_share_run` | test | T4b | S5 T4b (destination not present yet) |
| 1515 | `read_frame` | test | T4b | S5 T4b (destination not present yet) |
| 1522 | `step_sleep` | test | T4b | S5 T4b (destination not present yet) |
| 1535 | `irq_check` | test-moved | src/drivers/gpu/i915/tests/display/lcd-run.c:i915_test_lcdr_irq_check (function containing the matched text) | S5 T4b |
| 1585 | `expected_duty` | test | T4b | S5 T4b (destination not present yet) |
| 1592 | `brightness_step` | test | T4b | S5 T4b (destination not present yet) |
| 1627 | `d_vbt_min` | test | T4b | S5 T4b (destination not present yet) |
| 1632 | `window_first` | test | T4b | S5 T4b (destination not present yet) |
| 1663 | `window_again` | test | T4b | S5 T4b (destination not present yet) |
| 1669 | `parity_lcd_kernel_lcdr_run` | test-moved | src/drivers/gpu/i915/tests/display/lcd-run.c:drv_i915_test_display_lcdr (function containing the matched text) | S5 T4b |
| 1697 | `parity_lcd_kernel_abandoned` | found | display/diagnostics.c:drv_i915_lcd_kernel_abandoned |  |
| 1702 | `parity_lcd_kernel_gpu_retained` | found | display/diagnostics.c:drv_i915_lcd_kernel_gpu_retained |  |
| 1707 | `parity_lcd_kernel_summary` | test | T4b | test-only [s4-reports P4](i915-rebuild-s4-reports.md) L37; callers probe.c test block, lcdg_ktest.c |
| 1725 | `parity_lcdg_finish` | test | T4b | S5 T4b (destination not present yet) |
| 1757 | `lcdg_verify` | test | T4b | S5 T4b (destination not present yet) |
| 1764 | `parity_lcd_kernel_lcdg_run` | test | T4b | S5 T4b (destination not present yet) |
| 1916 | `lcdc_verify_a` | test | T4b | S5 T4b (destination not present yet) |
| 1923 | `lcdc_flips` | test-moved | src/drivers/gpu/i915/tests/display/lcd-flip.c:i915_test_lcdc_flips (function containing the matched text) | S5 T4b |
| 1958 | `parity_lcd_kernel_lcdc_run` | test | T4b | S5 T4b (destination not present yet) |
| 2073 | `lcdd_verify` | test | T4b | S5 T4b (destination not present yet) |
| 2087 | `lcdd_draw` | test | T4b | S5 T4b (destination not present yet) |
| 2135 | `lcdd_evasion_probe` | test | T4b | S5 T4b (destination not present yet) |
| 2205 | `lcdd_rounds` | test | T4b | S5 T4b (destination not present yet) |
| 2242 | `parity_lcd_kernel_lcdd_run` | test | T4b | S5 T4b (destination not present yet) |
| 2402 | `lcdo_rd` | test | T4b | S5 T4b (destination not present yet) |
| 2403 | `lcdo_wr` | test | T4b | S5 T4b (destination not present yet) |
| 2405 | `lcdo_backlight` | test | T4b | S5 T4b (destination not present yet) |
| 2411 | `lcdo_verify` | test | T4b | S5 T4b (destination not present yet) |
| 2419 | `lcdo_expected_duty` | test | T4b | S5 T4b (destination not present yet) |
| 2426 | `lcdo_steps` | test | T4b | S5 T4b (destination not present yet) |
| 2500 | `parity_lcd_kernel_lcdo_run` | test | T4b | S5 T4b (destination not present yet) |
| 2596 | `n1_hold` | test | T4b | S5 T4b (destination not present yet) |
| 2610 | `n1_read_plane` | found | display/takeover.c:drv_i915_n1_read_plane |  |
| 2655 | `n1_console_fb` | found | display/takeover.c:drv_i915_n1_console_fb |  |
| 2667 | `n1_check_ggtt` | found | display/takeover.c:drv_i915_n1_check_ggtt |  |
| 2704 | `n1_mirror_console` | found | display/takeover.c:drv_i915_n1_mirror_console |  |
| 2735 | `n1_window_mirror` | test | T4b | S5 T4b (destination not present yet) |
| 2811 | `n1_window` | test | T4b | S5 T4b (destination not present yet) |
| 2854 | `parity_lcd_kernel_n1_run` | test | T4b | S5 T4b (destination not present yet) |
| 3158 | `resident_window` | found | display/modeset.c:i915_resident_window |  |
| 3175 | `resident_verify` | found | display/modeset.c:i915_resident_verify |  |
| 3182 | `parity_lcd_kernel_panel_mode` | found | display/state.c:drv_i915_display_panel_mode |  |
| 3202 | `parity_lcd_resident_buffer` | found | display/scanout.c:drv_i915_lcd_resident_buffer |  |
| 3207 | `parity_lcd_resident_back` | found | display/scanout.c:drv_i915_lcd_resident_back |  |
| 3212 | `parity_lcd_resident_flip` | found | display/present.c:drv_i915_lcd_resident_flip |  |
| 3238 | `parity_lcd_kernel_resident_run` | found | display/modeset.c:drv_i915_lcd_kernel_resident_run |  |
| 3342 | `parity_lcd_kernel_panel_size_mm` | found | display/state.c:drv_i915_display_panel_size_mm |  |

#### `parity/lcd/parity_lcd_modeset.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 46 | `bind_current` | found | display/modeset.c:i915_modeset_bind_current | match by substr |
| 61 | `parity_lcd_set_display_ver` | found | display/modeset.c:drv_i915_lcd_set_display_ver |  |
| 62 | `parity_lcd_display_ver` | found | display/modeset.c:drv_i915_lcd_display_ver |  |
| 65 | `parity_lcd_modeset_select` | found | display/modeset.c:drv_i915_lcd_modeset_select |  |
| 74 | `parity_lcd_modeset_selected` | found | display/modeset.c:drv_i915_lcd_modeset_selected |  |
| 80 | `on_error` | found | display/modeset.c:i915_modeset_on_error | match by substr |
| 89 | `parity_lcd_debug` | found | display/modeset.c:drv_i915_lcd_debug |  |
| 95 | `parity_lcd_modeset_prepare` | found | display/modeset.c:drv_i915_lcd_modeset_prepare |  |
| 302 | `read_link_status` | found | display/modeset.c:i915_modeset_read_link_status | match by substr |
| 313 | `parity_lcd_modeset_status` | found | display/modeset.c:drv_i915_lcd_modeset_status |  |
| 383 | `parity_lcd_modeset_enable` | found | display/modeset.c:drv_i915_lcd_modeset_enable |  |
| 415 | `parity_lcd_modeset_plane_update` | found | display/modeset.c:drv_i915_lcd_modeset_plane_update |  |
| 425 | `parity_lcd_modeset_plane_disable` | found | display/modeset.c:drv_i915_lcd_modeset_plane_disable |  |
| 437 | `parity_lcd_modeset_disable` | found | display/modeset.c:drv_i915_lcd_modeset_disable |  |
| 459 | `parity_lcd_ms_vblank_off` | found | display/vblank.c:drv_i915_lcd_ms_vblank_off |  |
| 470 | `parity_lcd_modeset_evade_window` | found | display/vblank.c:drv_i915_lcd_modeset_evade_window |  |
| 479 | `parity_lcd_modeset_plane_released` | found | display/modeset.c:drv_i915_lcd_modeset_plane_released |  |
| 487 | `parity_lcd_modeset_link_status` | found | display/modeset.c:drv_i915_lcd_modeset_link_status |  |
| 496 | `observe` | found | display/modeset.c:i915_modeset_observe | match by substr |
| 502 | `parity_lcd_modeset_commit_enable` | found | display/modeset.c:drv_i915_lcd_modeset_commit_enable |  |
| 551 | `parity_lcd_modeset_commit_disable` | found | display/modeset.c:drv_i915_lcd_modeset_commit_disable |  |
| 615 | `parity_lcd_modeset_abandoned` | found | display/modeset.c:drv_i915_lcd_modeset_abandoned |  |
| 622 | `parity_lcd_modeset_retained` | found | display/modeset.c:drv_i915_lcd_modeset_retained |  |
| 627 | `parity_lcd_modeset_discard_model` | found | display/modeset.c:drv_i915_lcd_modeset_discard_model | ledger said test; kept in production (e.g. [s4 §4](i915-rebuild-s4.md) L278) |
| 639 | `parity_lcd_backend_fault` | found | display/modeset.c:drv_i915_lcd_backend_fault |  |
| 644 | `parity_lcd_modeset_brightness` | found | display/panel-backlight.c:drv_i915_lcd_modeset_brightness |  |
| 659 | `parity_lcd_modeset_backlight_acpi` | found | display/panel-backlight.c:drv_i915_lcd_modeset_backlight_acpi |  |
| 670 | `parity_lcd_modeset_backlight` | found | display/panel-backlight.c:drv_i915_lcd_modeset_backlight |  |
| 687 | `live_surf` | found | display/modeset.c:i915_modeset_live_surf | match by substr |
| 692 | `frame_now` | found | display/modeset.c:i915_modeset_frame_now | match by substr |
| 697 | `parity_lcd_modeset_flip` | found | display/modeset.c:drv_i915_lcd_modeset_flip |  |

#### `parity/lcd/parity_lcd_observe.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 11 | `parity_lcd_observer_init` | found | display/diagnostics.c:drv_i915_lcd_observer_init |  |
| 25 | `sample` | found | display/diagnostics.c:i915_observer_sample | match by substr |
| 74 | `parity_lcd_observer_point` | found | display/diagnostics.c:drv_i915_lcd_observer_point |  |
| 83 | `parity_lcd_observer_steady_begin` | found | display/diagnostics.c:drv_i915_lcd_observer_steady_begin |  |
| 89 | `parity_lcd_observer_steady_sample` | found | display/diagnostics.c:drv_i915_lcd_observer_steady_sample |  |
| 94 | `parity_lcd_observer_steady_end` | found | display/diagnostics.c:drv_i915_lcd_observer_steady_end |  |
| 102 | `parity_lcd_observer_frames` | found | display/diagnostics.c:drv_i915_lcd_observer_frames |  |
| 118 | `parity_lcd_observer_stopped` | found | display/diagnostics.c:drv_i915_lcd_observer_stopped |  |
| 130 | `parity_lcd_observer_pipe_active` | found | display/diagnostics.c:drv_i915_lcd_observer_pipe_active |  |

#### `parity/lcd/parity_lcd_regs.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 26 | `parity_lcd_reg_table` | found | display/diagnostics.c:drv_i915_lcd_reg_table |  |
| 68 | `parity_lcd_reg_by_name` | found | display/diagnostics.c:drv_i915_lcd_reg_by_name |  |
| 82 | `parity_lcd_ref_dbuf_ctl` | found | display/diagnostics.c:drv_i915_lcd_ref_dbuf_ctl |  |
| 93 | `parity_lcd_last_resort_stop` | found | display/diagnostics.c:drv_i915_lcd_last_resort_stop |  |

#### `parity/lcd/parity_lcd_show.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 21 | `parity_lcd_show_retained` | found | display/modeset.c:drv_i915_lcd_show_retained |  |
| 26 | `parity_lcd_show_retain_gpu` | found | display/modeset.c:drv_i915_lcd_show_retain_gpu |  |
| 35 | `parity_lcd_show_gpu_retained` | found | display/modeset.c:drv_i915_lcd_show_gpu_retained |  |
| 40 | `parity_lcd_show_discard_gpu_model` | found | display/modeset.c:drv_i915_lcd_show_discard_gpu_model |  |
| 52 | `parity_lcd_show_discard_model` | found | display/modeset.c:drv_i915_lcd_show_discard_model |  |
| 65 | `anomaly` | found | display/modeset.c:i915_show_anomaly | match by substr |
| 74 | `reached` | found | display/modeset.c:i915_show_reached | match by substr |
| 81 | `observe_tap` | found | display/modeset.c (inlined: trace->tap = drv_i915_lcd_observer_point, L2571) |  |
| 86 | `first_error_after` | found | display/modeset.c:i915_show_first_error_after | match by substr |
| 101 | `show_display` | found | display/modeset.c:i915_show_display |  |
| 219 | `show_passed` | found | display/modeset.c:i915_show_passed |  |
| 227 | `show_begin` | found | display/modeset.c:i915_show_begin |  |
| 240 | `parity_lcd_show_prepared` | found | display/modeset.c:drv_i915_lcd_show_prepared |  |
| 258 | `pattern_verify` | found | display/diagnostics.c:drv_i915_lcd_pattern_verify |  |
| 266 | `parity_lcd_show_run` | found | display/modeset.c:drv_i915_lcd_show_run |  |

#### `parity/lcd/parity_lcd_trace.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 7 | `add` | found | display/diagnostics.c:i915_trace_add | match by tokens |
| 22 | `t_write32` | found | display/diagnostics.c:i915_trace_write32 | match by tokens |
| 32 | `t_rmw32` | found | display/diagnostics.c:i915_trace_rmw32 | match by tokens |
| 43 | `t_posting_read` | found | display/diagnostics.c:i915_trace_posting_read | match by tokens |
| 51 | `t_read32` | found | display/diagnostics.c:i915_trace_read32 | match by tokens |
| 68 | `t_wait_reg` | found | display/diagnostics.c:i915_trace_wait_reg | match by tokens |
| 81 | `t_usleep` | found | display/diagnostics.c:i915_trace_usleep | match by tokens |
| 90 | `t_udelay` | found | display/diagnostics.c:i915_trace_udelay | match by tokens |
| 98 | `first_bytes` | found | display/diagnostics.c:i915_trace_first_bytes | match by substr |
| 108 | `t_dpcd_read` | found | display/diagnostics.c:i915_trace_dpcd_read | match by tokens |
| 118 | `t_dpcd_write` | found | display/diagnostics.c:i915_trace_dpcd_write | match by tokens |
| 128 | `t_read_dpcd_caps` | found | display/internal.h:i915_mmio_reg_equal (function containing the matched text) | match by string |
| 138 | `t_panel` | found | display/diagnostics.c:i915_trace_panel | match by tokens |
| 148 | `t_power_get` | found | display/diagnostics.c:i915_trace_power_get | match by tokens |
| 158 | `t_power_put` | found | display/diagnostics.c:i915_trace_power_put | match by tokens |
| 167 | `t_power_put_async` | found | display/diagnostics.c:i915_trace_power_put_async | match by tokens |
| 176 | `t_dbuf_slices_update` | found | display/diagnostics.c:i915_trace_dbuf_slices_update | match by tokens |
| 185 | `t_observe` | found | display/diagnostics.c:i915_trace_observe | match by tokens |
| 198 | `t_vblank_get` | found | display/diagnostics.c:i915_trace_vblank_get | match by tokens |
| 199 | `t_vblank_put` | found | display/diagnostics.c:i915_trace_vblank_put | match by tokens |
| 200 | `t_vblank_sleep` | found | display/diagnostics.c:i915_trace_vblank_sleep | match by tokens |
| 201 | `t_irq_off` | found | display/diagnostics.c:i915_trace_irq_off | match by tokens |
| 202 | `t_irq_on` | found | display/diagnostics.c:i915_trace_irq_on | match by tokens |
| 203 | `t_arm_event` | found | display/diagnostics.c:i915_trace_arm_event | match by tokens |
| 204 | `t_wait_event` | found | display/diagnostics.c:i915_trace_wait_event | match by tokens |
| 205 | `t_cancel_event` | found | display/diagnostics.c:i915_trace_cancel_event | match by tokens |
| 207 | `t_lock` | found | display/diagnostics.c:i915_trace_lock | match by tokens |
| 214 | `named` | found | display/diagnostics.c:i915_trace_named | match by substr |
| 222 | `t_step` | found | display/diagnostics.c:drv_i915_lcd_kernel_step | match by tokens |
| 237 | `t_error` | found | display/diagnostics.c:drv_i915_lcd_kernel_error | match by tokens |
| 248 | `t_debug` | found | display/diagnostics.c:drv_i915_lcd_kernel_debug | match by tokens |
| 256 | `parity_lcd_trace_init` | found | display/diagnostics.c:drv_i915_lcd_trace_init |  |
| 296 | `parity_lcd_trace_phase` | found | display/diagnostics.c:drv_i915_lcd_trace_phase |  |
| 301 | `parity_lcd_trace_find` | found | display/diagnostics.c:drv_i915_lcd_trace_find |  |

#### `parity/lcd/parity_modeset_setup_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 50 | `parity_n1_crtc_at` | found | display/takeover.c:drv_i915_n1_crtc_at |  |
| 59 | `parity_n1_crtc_for_pipe` | found | display/takeover.c:drv_i915_n1_crtc_for_pipe |  |
| 66 | `parity_n1_plane_at` | found | display/takeover.c:drv_i915_n1_plane_at |  |
| 76 | `parity_n1_primary_plane` | found | display/takeover.c:drv_i915_n1_primary_plane |  |
| 81 | `parity_n1_encoder_at` | found | display/takeover.c:drv_i915_n1_encoder_at |  |
| 89 | `parity_n1_connector_at` | found | display/takeover.c:drv_i915_n1_connector_at |  |
| 101 | `parity_n1_crtc_state` | found | display/takeover.c:drv_i915_n1_crtc_state |  |
| 113 | `n1_plane_get_hw_state` | found | display/takeover.c:i915_n1_plane_get_hw_state |  |
| 129 | `parity_n1_power_get_if_enabled` | found | display/takeover.c:drv_i915_n1_power_get_if_enabled |  |
| 143 | `parity_n1_power_get_in_set_if_enabled` | found | display/takeover.c:drv_i915_n1_power_get_in_set_if_enabled |  |
| 152 | `parity_n1_power_put_all_in_set` | found | display/takeover.c:drv_i915_n1_power_put_all_in_set |  |
| 166 | `parity_n1_atomic_crtc_state` | found | display/takeover.c:drv_i915_n1_atomic_crtc_state |  |
| 181 | `parity_n1_atomic_state` | found | display/takeover.c:drv_i915_n1_atomic_state |  |
| 198 | `n1_build_device` | found | display/takeover.c:i915_n1_build_device |  |
| 266 | `n1_fill_report` | found | display/takeover.c:i915_n1_fill_report |  |
| 308 | `parity_n1_readout` | found | display/takeover.c:drv_i915_n1_readout |  |
| 346 | `parity_n1_takeover` | found | display/takeover.c:drv_i915_n1_takeover |  |
| 375 | `parity_n1_release` | found | display/takeover.c:drv_i915_n1_release |  |

#### `parity/lcd/parity_opregion_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 25 | `parity_opregion_warn_on` | found | display/opregion.c:drv_i915_opregion_warn_on |  |
| 32 | `parity_opregion_pci_read32` | found | display/opregion.c:drv_i915_opregion_pci_read32 |  |
| 41 | `parity_opregion_pci_access_unported` | found | display/opregion.c:drv_i915_opregion_pci_access_unported |  |
| 49 | `parity_opregion_memremap` | found | display/opregion.c:drv_i915_opregion_memremap |  |
| 62 | `parity_opregion_memunmap` | found | display/opregion.c:drv_i915_opregion_memunmap |  |
| 68 | `parity_opregion_dmi_check_system` | found | display/opregion.c:drv_i915_opregion_dmi_check_system |  |
| 74 | `parity_opregion_boundary` | found | display/opregion.c:drv_i915_opregion_boundary |  |
| 80 | `parity_opregion_cancel_work_sync` | found | display/opregion.c:drv_i915_opregion_cancel_work_sync |  |
| 88 | `parity_opregion_mailbox_backend` | found | display/opregion.c:drv_i915_opregion_mailbox_backend |  |
| 89 | `parity_opregion_service_epoch` | found | display/opregion.c:drv_i915_opregion_service_epoch |  |
| 91 | `parity_opregion_shadow_map` | found | display/opregion.c:drv_i915_opregion_shadow_map |  |
| 103 | `parity_opregion_shadow_setup` | found | display/opregion.c:drv_i915_opregion_shadow_setup |  |
| 121 | `parity_opregion_register` | found | display/opregion.c:drv_i915_opregion_register |  |
| 129 | `parity_opregion_unregister` | found | display/opregion.c:drv_i915_opregion_unregister |  |
| 136 | `parity_opregion_cleanup` | found | display/opregion.c:drv_i915_opregion_cleanup |  |
| 162 | `parity_opregion_firmware_setup` | found | display/opregion.c:drv_i915_opregion_firmware_setup |  |
| 212 | `parity_opregion_mbox_read` | found | display/opregion.c:drv_i915_opregion_mbox_read |  |
| 221 | `parity_opregion_mbox_write` | found | display/opregion.c:drv_i915_opregion_mbox_write |  |
| 226 | `parity_opregion_notify_adapter` | found | display/opregion.c:drv_i915_opregion_notify_adapter |  |
| 227 | `parity_opregion_vbt` | found | display/opregion.c:drv_i915_opregion_vbt |  |
| 232 | `parity_opregion_notifier_registered` | found | display/opregion.c:drv_i915_opregion_notifier_registered |  |
| 233 | `parity_opregion_counters` | found | display/opregion.c:drv_i915_opregion_counters |  |
| 253 | `parity_opregion_backlight_policy` | found | display/opregion.c:drv_i915_opregion_backlight_policy |  |
| 254 | `parity_opregion_connection_lock` | found | display/opregion.c:drv_i915_opregion_connection_lock |  |
| 255 | `parity_opregion_connection_unlock` | found | display/opregion.c:drv_i915_opregion_connection_unlock |  |
| 257 | `parity_opregion_connector_next` | found | display/opregion.c:drv_i915_opregion_connector_next |  |
| 264 | `parity_opregion_backlight_set_acpi` | found | display/opregion.c:drv_i915_opregion_backlight_set_acpi |  |
| 271 | `parity_opregion_service_start` | found | display/opregion.c:drv_i915_opregion_service_start |  |
| 284 | `parity_opregion_set_policy` | found | display/opregion.c:drv_i915_opregion_set_policy |  |
| 286 | `parity_opregion_add_connector` | found | display/opregion.c:drv_i915_opregion_add_connector |  |
| 302 | `parity_opregion_add_backlight` | found | display/opregion.c:drv_i915_opregion_add_backlight |  |
| 308 | `parity_opregion_gse_entry` | found | display/opregion.c:drv_i915_opregion_gse_entry |  |
| 317 | `parity_opregion_gate_counters` | found | display/opregion.c:drv_i915_opregion_gate_counters |  |
| 323 | `parity_opregion_asle_flush` | found | display/opregion.c:drv_i915_opregion_asle_flush |  |
| 330 | `parity_opregion_worker_stats_get` | found | display/opregion.c:drv_i915_opregion_worker_stats_get |  |
| 350 | `parity_opregion_notify_encoder` | found | display/opregion.c:drv_i915_opregion_notify_encoder |  |

#### `parity/lcd/parity_plane_emit_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `parity_plane_emit` | found | display/plane.c:drv_i915_plane_emit |  |
| 88 | `parity_lcd_ms_plane_prepare` | found | display/plane.c:drv_i915_lcd_ms_plane_prepare |  |
| 129 | `parity_lcd_ms_plane_update` | found | display/plane.c:drv_i915_lcd_ms_plane_update |  |
| 136 | `parity_lcd_ms_plane_disable` | found | display/plane.c:drv_i915_lcd_ms_plane_disable |  |
| 142 | `parity_lcd_ms_plane_min_cdclk` | found | display/plane.c:drv_i915_lcd_ms_plane_min_cdclk |  |
| 154 | `parity_lcd_ms_plane_update_flip` | found | display/plane.c:drv_i915_lcd_ms_plane_update_flip |  |
| 181 | `parity_lcd_plane_disable_arm` | found | display/plane.c:drv_i915_lcd_plane_disable_arm |  |

#### `parity/lcd/parity_wm_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 20 | `parity_lcd_dbuf_current` | found | display/watermark.c:drv_i915_lcd_dbuf_current |  |
| 28 | `parity_lcd_dbuf_publish` | found | display/watermark.c:drv_i915_lcd_dbuf_publish |  |
| 35 | `parity_lcd_dbuf_forget` | found | display/watermark.c:drv_i915_lcd_dbuf_forget |  |
| 41 | `parity_lcd_ms_wm_compute` | found | display/watermark.c:drv_i915_lcd_ms_wm_compute |  |
| 83 | `parity_lcd_ms_wm_compute_off` | found | display/watermark.c:drv_i915_lcd_ms_wm_compute_off |  |
| 105 | `parity_lcd_wm_get_hw_state` | found | display/watermark.c:drv_i915_lcd_wm_get_hw_state |  |

#### `parity/lcd/scanout.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 34 | `parity_scanout_create` | found | display/scanout.c:drv_i915_scanout_create |  |
| 73 | `parity_scanout_pin` | found | display/scanout.c:drv_i915_scanout_pin |  |
| 95 | `parity_scanout_publish` | found | display/scanout.c:drv_i915_scanout_publish |  |
| 104 | `parity_scanout_begin` | found | display/scanout.c:drv_i915_scanout_begin |  |
| 115 | `parity_scanout_end` | found | display/scanout.c:drv_i915_scanout_end |  |
| 123 | `parity_scanout_unpin` | found | display/scanout.c:drv_i915_scanout_unpin |  |
| 141 | `parity_scanout_destroy` | found | display/scanout.c:drv_i915_scanout_destroy |  |
| 157 | `parity_scanout_abandon` | found | display/scanout.c:drv_i915_scanout_abandon |  |

#### `parity/lcd/scanout_ktest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 23 | `table_fill` | test-moved | tests/display/scanout-ktest.c:i915_scanout_ktest_table_fill | S5 T4b |
| 32 | `stray_writes` | test-moved | tests/display/scanout-ktest.c:i915_scanout_ktest_stray_writes | S5 T4b |
| 45 | `ptes_match` | test-moved | tests/display/scanout-ktest.c:i915_scanout_ktest_ptes_match | S5 T4b |
| 61 | `guards_are_scratch` | test-moved | tests/display/scanout-ktest.c:i915_scanout_ktest_guards_are_scratch | S5 T4b |
| 73 | `parity_scanout_ktest` | test-moved | src/drivers/gpu/i915/tests/display/scanout-ktest.c:i915_scanout_ktest_body (function containing the matched text) | S5 T4b |

#### `parity/lcd/skl_plane_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 27 | `icl_hdr_plane_mask` | found | display/plane.c:drv_i915_icl_hdr_plane_mask |  |
| 32 | `icl_is_hdr_plane` | found | display/plane.c:i915_icl_is_hdr_plane |  |
| 38 | `skl_plane_stride_mult` | found | display/plane.c:i915_skl_plane_stride_mult |  |
| 53 | `skl_plane_stride` | found | display/plane.c:i915_skl_plane_stride |  |
| 66 | `skl_plane_ctl_format` | found | display/plane.c:i915_skl_plane_ctl_format |  |
| 128 | `skl_plane_ctl_alpha` | found | display/plane.c:i915_skl_plane_ctl_alpha |  |
| 146 | `glk_plane_color_ctl_alpha` | found | display/plane.c:i915_glk_plane_color_ctl_alpha |  |
| 164 | `skl_plane_ctl_tiling` | found | display/plane.c:i915_skl_plane_ctl_tiling |  |
| 213 | `skl_plane_ctl_rotate` | found | display/plane.c:i915_skl_plane_ctl_rotate |  |
| 235 | `icl_plane_ctl_flip` | found | display/plane.c:i915_icl_plane_ctl_flip |  |
| 250 | `adlp_plane_ctl_arb_slots` | found | display/plane.c:i915_adlp_plane_ctl_arb_slots |  |
| 273 | `skl_plane_ctl_crtc` | found | display/plane.c:i915_skl_plane_ctl_crtc |  |
| 290 | `skl_plane_ctl` | found | display/plane.c:i915_skl_plane_ctl |  |
| 333 | `glk_plane_color_ctl_crtc` | found | display/plane.c:i915_glk_plane_color_ctl_crtc |  |
| 350 | `glk_plane_color_ctl` | found | display/plane.c:i915_glk_plane_color_ctl |  |
| 389 | `skl_surf_address` | found | display/plane.c:i915_skl_surf_address |  |
| 411 | `skl_plane_surf` | found | display/plane.c:i915_skl_plane_surf |  |
| 425 | `skl_plane_aux_dist` | found | display/plane.c:i915_skl_plane_aux_dist |  |
| 445 | `skl_plane_keyval` | found | display/plane.c:i915_skl_plane_keyval |  |
| 452 | `skl_plane_keymsk` | found | display/plane.c:i915_skl_plane_keymsk |  |
| 465 | `skl_plane_keymax` | found | display/plane.c:i915_skl_plane_keymax |  |
| 473 | `icl_plane_color_plane` | found | display/plane.c:i915_icl_plane_color_plane |  |
| 482 | `icl_plane_update_sel_fetch_noarm` | found | display/plane.c:i915_icl_plane_update_sel_fetch_noarm |  |
| 525 | `icl_plane_update_noarm` | found | display/plane.c:i915_icl_plane_update_noarm |  |
| 599 | `icl_plane_disable_sel_fetch_arm` | found | display/plane.c:i915_icl_plane_disable_sel_fetch_arm |  |
| 611 | `icl_plane_update_sel_fetch_arm` | found | display/plane.c:i915_icl_plane_update_sel_fetch_arm |  |
| 629 | `icl_plane_update_arm` | found | display/plane.c:i915_icl_plane_update_arm |  |
| 665 | `icl_plane_disable_arm` | found | display/plane.c:i915_icl_plane_disable_arm |  |
| 682 | `icl_plane_min_cdclk` | found | display/plane.c:i915_icl_plane_min_cdclk |  |
| 692 | `skl_plane_get_hw_state` | found | display/plane.c:i915_skl_plane_get_hw_state |  |

#### `parity/lcd/skl_watermark_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 459 | `intel_dbuf_enabled_slices` | found | display/watermark.c:i915_intel_dbuf_enabled_slices |  |
| 477 | `check_mbus_joined` | found | display/watermark.c:i915_check_mbus_joined |  |
| 489 | `adlp_check_mbus_joined` | found | display/watermark.c:i915_adlp_check_mbus_joined |  |
| 494 | `compute_dbuf_slices` | found | display/watermark.c:i915_compute_dbuf_slices |  |
| 507 | `tgl_compute_dbuf_slices` | found | display/watermark.c:i915_tgl_compute_dbuf_slices |  |
| 513 | `adlp_compute_dbuf_slices` | found | display/watermark.c:i915_adlp_compute_dbuf_slices |  |
| 519 | `skl_compute_dbuf_slices` | found | display/watermark.c:i915_skl_compute_dbuf_slices |  |
| 539 | `intel_dbuf_slice_size` | found | display/watermark.c:i915_intel_dbuf_slice_size |  |
| 545 | `skl_ddb_entry_init` | found | display/watermark.c:i915_skl_ddb_entry_init |  |
| 554 | `mbus_ddb_offset` | found | display/watermark.c:i915_mbus_ddb_offset |  |
| 568 | `skl_watermark_ipc_enabled` | found | display/watermark.c:i915_skl_watermark_ipc_enabled |  |
| 577 | `skl_needs_memory_bw_wa` | found | display/watermark.c:i915_skl_needs_memory_bw_wa |  |
| 583 | `intel_get_linetime_us` | found | display/watermark.c:i915_intel_get_linetime_us |  |
| 605 | `skl_cursor_allocation` | found | display/watermark.c:i915_skl_cursor_allocation |  |
| 636 | `skl_total_relative_data_rate` | found | display/watermark.c:i915_skl_total_relative_data_rate |  |
| 656 | `intel_crtc_dbuf_weights` | found | display/watermark.c:i915_intel_crtc_dbuf_weights |  |
| 693 | `skl_wm_check_vblank` | found | display/watermark.c:i915_skl_wm_check_vblank |  |
| 748 | `skl_wm_latency` | found | display/watermark.c:i915_skl_wm_latency |  |
| 777 | `skl_wm_method1` | found | display/watermark.c:i915_skl_wm_method1 |  |
| 796 | `skl_wm_method2` | found | display/watermark.c:i915_skl_wm_method2 |  |
| 813 | `skl_compute_wm_params` | found | display/watermark.c:i915_skl_compute_wm_params |  |
| 902 | `skl_compute_plane_wm_params` | found | display/watermark.c:i915_skl_compute_plane_wm_params |  |
| 923 | `skl_wm_has_lines` | found | display/watermark.c:i915_skl_wm_has_lines |  |
| 932 | `skl_wm_max_lines` | found | display/watermark.c:i915_skl_wm_max_lines |  |
| 940 | `skl_compute_plane_wm` | found | display/watermark.c:i915_skl_compute_plane_wm |  |
| 1075 | `skl_compute_wm_levels` | found | display/watermark.c:i915_skl_compute_wm_levels |  |
| 1095 | `tgl_compute_sagv_wm` | found | display/watermark.c:i915_tgl_compute_sagv_wm |  |
| 1114 | `skl_compute_transition_wm` | found | display/watermark.c:i915_skl_compute_transition_wm |  |
| 1177 | `skl_build_plane_wm_single` | found | display/watermark.c:i915_skl_build_plane_wm_single |  |
| 1207 | `icl_build_plane_wm` | found | display/watermark.c:i915_icl_build_plane_wm |  |
| 1249 | `skl_build_plane_wm_uv` | found | display/watermark.c:i915_skl_build_plane_wm_uv |  |
| 1270 | `skl_build_plane_wm` | found | display/watermark.c:i915_skl_build_plane_wm |  |
| 1299 | `skl_max_wm0_lines` | found | display/watermark.c:i915_skl_max_wm0_lines |  |
| 1315 | `skl_max_wm_level_for_vblank` | found | display/watermark.c:i915_skl_max_wm_level_for_vblank |  |
| 1342 | `skl_is_vblank_too_short` | found | display/watermark.c:i915_skl_is_vblank_too_short |  |
| 1355 | `skl_build_pipe_wm` | found | display/watermark.c:i915_skl_build_pipe_wm |  |
| 1400 | `skl_check_wm_level` | found | display/watermark.c:i915_skl_check_wm_level |  |
| 1407 | `skl_check_nv12_wm_level` | found | display/watermark.c:i915_skl_check_nv12_wm_level |  |
| 1417 | `skl_need_wm_copy_wa` | found | display/watermark.c:i915_skl_need_wm_copy_wa |  |
| 1436 | `use_minimal_wm0_only` | found | display/watermark.c:i915_use_minimal_wm0_only |  |
| 1447 | `skl_allocate_plane_ddb` | found | display/watermark.c:i915_skl_allocate_plane_ddb |  |
| 1474 | `skl_crtc_allocate_plane_ddb` | found | display/watermark.c:i915_skl_crtc_allocate_plane_ddb |  |
| 1646 | `skl_ddb_entry_for_slices` | found | display/watermark.c:i915_skl_ddb_entry_for_slices |  |
| 1664 | `intel_crtc_ddb_weight` | found | display/watermark.c:i915_intel_crtc_ddb_weight |  |
| 1683 | `skl_crtc_allocate_ddb` | found | display/watermark.c:i915_skl_crtc_allocate_ddb |  |
| 1754 | `skl_plane_wm_level` | found | display/watermark.c:i915_skl_plane_wm_level |  |
| 1767 | `skl_plane_trans_wm` | found | display/watermark.c:i915_skl_plane_trans_wm |  |
| 1778 | `skl_write_wm_level` | found | display/watermark.c:i915_skl_write_wm_level |  |
| 1794 | `skl_ddb_entry_write` | found | display/watermark.c:i915_skl_ddb_entry_write |  |
| 1806 | `skl_write_plane_wm` | found | display/watermark.c:skl_write_plane_wm |  |
| 1843 | `intel_dbuf_mdclk_cdclk_ratio_update` | found | display/watermark.c:i915_intel_dbuf_mdclk_cdclk_ratio_update |  |
| 1862 | `update_mbus_pre_enable` | found | display/watermark.c:i915_update_mbus_pre_enable |  |
| 1890 | `intel_dbuf_pre_plane_update` | found | display/watermark.c:drv_i915_dbuf_pre_plane_update |  |
| 1911 | `intel_dbuf_post_plane_update` | found | display/watermark.c:drv_i915_dbuf_post_plane_update |  |
| 1930 | `xelpdp_is_only_pipe_per_dbuf_bank` | found | display/watermark.c:i915_xelpdp_is_only_pipe_per_dbuf_bank |  |
| 1949 | `intel_mbus_dbox_update` | found | display/watermark.c:drv_i915_mbus_dbox_update |  |
| 2013 | `skl_wm_level_from_reg_val` | found | display/watermark.c:i915_skl_wm_level_from_reg_val |  |
| 2021 | `skl_pipe_wm_get_hw_state` | found | display/watermark.c:i915_skl_pipe_wm_get_hw_state |  |
| 2070 | `skl_pipe_ddb_get_hw_state` | found | display/watermark.c:i915_skl_pipe_ddb_get_hw_state |  |
| 2095 | `skl_ddb_get_hw_plane_state` | found | display/watermark.c:i915_skl_ddb_get_hw_plane_state |  |
| 2120 | `skl_ddb_entry_union` | found | display/watermark.c:i915_skl_ddb_entry_union |  |
| 2132 | `skl_wm_get_hw_state` | found | display/watermark.c:i915_skl_wm_get_hw_state |  |
| 2200 | `skl_dbuf_is_misconfigured` | found | display/watermark.c:i915_skl_dbuf_is_misconfigured |  |
| 2232 | `skl_wm_sanitize` | found | display/watermark.c:i915_skl_wm_sanitize |  |
| 2268 | `skl_wm_get_hw_state_and_sanitize` | found | display/watermark.c:i915_skl_wm_get_hw_state_and_sanitize |  |
| 2274 | `skl_ddb_entry_init_from_hw` | found | display/watermark.c:i915_skl_ddb_entry_init_from_hw |  |
| 2283 | `skl_ddb_dbuf_slice_mask` | found | display/watermark.c:skl_ddb_dbuf_slice_mask |  |
| 2308 | `skl_ddb_entries_overlap` | found | display/watermark.c:i915_skl_ddb_entries_overlap |  |
| 2314 | `skl_ddb_allocation_overlaps` | found | display/watermark.c:skl_ddb_allocation_overlaps |  |

#### `parity/legacy_shim.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 99 | `to_errno` | found | worker.c (inlined; errno already positive per rules §3) |  |
| 105 | `shim_find` | found | worker.c:i915_worker_find |  |
| 118 | `parity_shim_lrc_create` | found | worker.c:drv_i915_worker_context_create (function containing the matched text) | match by string |
| 191 | `parity_shim_lrc_destroy` | found | worker.c:drv_i915_worker_context_destroy |  |
| 218 | `parity_shim_request_kick` | found | worker.c:drv_i915_worker_kick |  |
| 242 | `shim_run` | found | worker.c:i915_worker_run (function containing the matched text) | match by string |
| 333 | `shim_execute` | found | worker.c:i915_worker_run_request |  |
| 344 | `shim_sync_do` | found | worker.c:i915_worker_queue_sync (function containing the matched text) | match by string |
| 373 | `parity_shim_run_sync` | found | worker.c:drv_i915_worker_run_sync |  |
| 386 | `parity_shim_display_present` | found | display/present.c:drv_i915_present_display_present | match by tokens |
| 402 | `parity_shim_display_release` | found | display/present.c:drv_i915_present_display_release | match by tokens |
| 412 | `parity_shim_display_present_blob` | found | display/present.c (PRESENT_BLOB via drv_i915_worker_sync_display, L819) |  |
| 427 | `parity_shim_display_deps` | found | display/display.c:drv_i915_display_resident_deps (s4 §8 L365 said 廃止; a function still exists) |  |
| 437 | `shim_present` | found | display/present.c:drv_i915_present_frame (function containing the matched text) | match by string |
| 473 | `parity_shim_engine_reset` | found | worker.c:drv_i915_worker_engine_reset (function containing the matched text) | match by string |
| 481 | `parity_shim_engine_recover` | found | worker.c:drv_i915_worker_engine_recover (function containing the matched text) | match by string |
| 491 | `parity_shim_gt_reset` | found | worker.c:drv_i915_worker_gt_reset (function containing the matched text) | match by string |
| 505 | `shim_map_panel` | found | display/scanout.c:drv_i915_scanout_map_panel (function containing the matched text) | match by string |
| 545 | `shim_unmap_panel` | found | display/scanout.c:drv_i915_scanout_unmap_panel | match by tokens |
| 560 | `shim_present_blob` | found | display/present.c:i915_present_check_frame (function containing the matched text) | match by string |
| 619 | `shim_finish` | found | worker.c:i915_worker_run_sync_item (item->done = 1, L904) |  |
| 632 | `shim_serve` | found | worker.c:i915_worker_loop |  |
| 732 | `shim_display_window` | found | display/present.c:i915_present_window_serve |  |
| 744 | `parity_resident_serve` | found | worker.c:drv_i915_worker_serve ([s4 §8](i915-rebuild-s4.md) L360) |  |

#### `parity/native_decide.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 8 | `parity_native_decide` | found | display/takeover.c:drv_i915_native_decide |  |

#### `parity/native_precheck.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 20 | `cpu_hypervisor` | found | display/takeover.c:i915_cpu_hypervisor |  |
| 29 | `read_opregion` | found | display/takeover.c:i915_read_opregion |  |
| 76 | `parity_opregion_read_data` | found | display/takeover.c:drv_i915_opregion_read_data |  |
| 123 | `parity_opregion_log` | found | display/takeover.c:drv_i915_opregion_log |  |
| 138 | `read_vtd` | found | display/takeover.c:i915_read_vtd |  |
| 159 | `read_fb` | found | display/takeover.c:i915_read_fb |  |
| 175 | `read_display` | found | display/takeover.c:i915_read_display |  |
| 228 | `parity_native_precheck` | found | display/takeover.c:drv_i915_native_precheck |  |
| 260 | `parity_native_log_again` | found | display/takeover.c:drv_i915_native_log_again |  |
| 269 | `parity_native_log` | found | display/takeover.c:drv_i915_native_log |  |

#### `parity/opregion_service.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `parity_acpi_notifier_init` | found | display/opregion.c:drv_i915_acpi_notifier_init |  |
| 25 | `parity_register_acpi_notifier` | found | display/opregion.c:drv_i915_register_acpi_notifier |  |
| 52 | `parity_unregister_acpi_notifier` | found | display/opregion.c:drv_i915_unregister_acpi_notifier |  |
| 73 | `parity_acpi_notifier_call_chain` | found | display/opregion.c:drv_i915_acpi_notifier_call_chain |  |
| 107 | `parity_acpi_notifier_count` | found | display/opregion.c:drv_i915_acpi_notifier_count |  |

#### `parity/opregion_vbt.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 6 | `le32` | found | display/vbt.c:i915_opregion_le32 |  |
| 7 | `le64` | found | display/vbt.c:i915_opregion_le64 |  |
| 10 | `parity_opregion_locate_vbt` | found | display/vbt.c:drv_i915_opregion_locate_vbt |  |

#### `parity/osdep/address_types.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 33 | `osdep_cpu_phys` | found | dma.h:drv_i915_cpu_phys |  |
| 34 | `osdep_dma_addr` | found | dma.h:drv_i915_dma_addr |  |
| 35 | `osdep_gpu_vaddr` | found | dma.h:drv_i915_gpu_vaddr |  |
| 37 | `osdep_cpu_phys_raw` | found | dma.h:drv_i915_cpu_phys_raw |  |
| 38 | `osdep_dma_addr_raw` | found | dma.h:drv_i915_dma_addr_raw |  |
| 39 | `osdep_gpu_vaddr_raw` | found | dma.h:drv_i915_gpu_vaddr_raw |  |
| 47 | `osdep_dma_mapping_failed` | found | dma.h:drv_i915_dma_mapping_failed |  |

#### `parity/osdep/dma.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 14 | `tr` | found | dma.c:i915_dma_note |  |
| 21 | `osdep_dma_device_init` | found | dma.c:drv_i915_dma_init |  |
| 44 | `osdep_dma_set_info` | found | dma.c:drv_i915_dma_set_info |  |
| 74 | `osdep_dma_address_bits` | found | dma.c:drv_i915_dma_address_bits |  |
| 80 | `osdep_dma_max_segment` | found | dma.c:drv_i915_dma_max_segment |  |
| 86 | `osdep_dma_is_coherent` | found | dma.c:drv_i915_dma_is_coherent |  |
| 92 | `alloc_mapping` | found | dma.c:i915_dma_claim_mapping |  |
| 112 | `map_sg_core` | found | dma.c:drv_i915_dma_is_coherent (function containing the matched text) | match by string |
| 155 | `osdep_dma_map_sg` | found | dma.c:drv_i915_dma_map_sg |  |
| 164 | `osdep_dma_map_sgtable` | found | dma.c:drv_i915_dma_map_sgtable |  |
| 175 | `osdep_dma_unmap_sg` | found | dma.c:drv_i915_dma_unmap_sg |  |
| 195 | `osdep_dma_map_page` | found | dma.c:drv_i915_dma_map_page |  |
| 215 | `osdep_dma_unmap_page` | found | dma.c:drv_i915_dma_unmap_page |  |
| 224 | `osdep_dma_pin` | found | dma.c:drv_i915_dma_pin |  |
| 235 | `osdep_dma_unpin` | found | dma.c:drv_i915_dma_unpin |  |
| 244 | `osdep_dma_sync_for_device` | found | dma.c:drv_i915_dma_sync_for_device |  |
| 253 | `osdep_dma_sync_for_cpu` | found | dma.c:drv_i915_dma_sync_for_cpu |  |
| 262 | `osdep_dma_live_mappings` | found | dma.c:drv_i915_dma_live_mappings |  |

#### `parity/osdep/firmware.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 36 | `osdep_firmware_test_set` | found | firmware.c:drv_i915_firmware_set_override (test hook left in production; S1 report says move to tests in S5) |  |
| 42 | `name_eq` | found | firmware.c:i915_firmware_name_equal | match by substr |
| 49 | `osdep_request_firmware` | found | firmware.c:drv_i915_firmware_request | match by tokens |
| 69 | `osdep_release_firmware` | found | firmware.c:drv_i915_firmware_release | match by tokens |

#### `parity/osdep/mmio.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 11 | `tr` | found | mmio.c:i915_mmio_note |  |
| 18 | `osdep_mmio_init` | found | mmio.c:drv_i915_mmio_init |  |
| 40 | `osdep_mmio_domain_of` | found | mmio.c:drv_i915_mmio_domain_of |  |
| 51 | `osdep_fw_get` | found | mmio.c:drv_i915_forcewake_get |  |
| 79 | `osdep_fw_put` | found | mmio.c:drv_i915_forcewake_put |  |
| 101 | `osdep_fw_is_held` | found | mmio.c:drv_i915_forcewake_held |  |
| 109 | `osdep_mmio_read32_auto` | found | mmio.c:drv_i915_read32_auto |  |
| 124 | `osdep_mmio_write32_auto` | found | mmio.c:drv_i915_write32_auto |  |
| 136 | `osdep_mmio_read32` | found | mmio.c:drv_i915_read32 |  |
| 149 | `osdep_mmio_write32` | found | mmio.c:drv_i915_write32 |  |
| 161 | `osdep_mmio_raw_read32` | found | mmio.c:drv_i915_raw_read32 |  |
| 167 | `osdep_mmio_raw_write32` | found | mmio.c:drv_i915_raw_write32 |  |
| 173 | `osdep_mmio_posting_read32` | found | mmio.c:drv_i915_posting_read32 |  |
| 181 | `osdep_mmio_write32_masked` | found | mmio.c:drv_i915_write32_masked |  |
| 197 | `osdep_mmio_write32_mask_enable` | found | mmio.c:drv_i915_write32_masked (rules §2) |  |
| 210 | `osdep_mcr_lock` | found | mmio.c:drv_i915_mcr_lock |  |
| 224 | `osdep_mcr_unlock` | found | mmio.c:drv_i915_mcr_unlock |  |
| 233 | `osdep_mcr_is_locked` | found | mmio.c:drv_i915_mcr_locked |  |

#### `parity/osdep/pci.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 14 | `tr` | found | pci.c:i915_pci_note |  |
| 21 | `osdep_pci_init` | found | pci.c:drv_i915_pci_init |  |
| 35 | `osdep_pci_read8` | found | pci.c:drv_i915_pci_read8 |  |
| 36 | `osdep_pci_read16` | found | pci.c:drv_i915_pci_read16 |  |
| 37 | `osdep_pci_read32` | found | pci.c:drv_i915_pci_read32 |  |
| 38 | `osdep_pci_write8` | found | pci.c:drv_i915_pci_write8 |  |
| 39 | `osdep_pci_write16` | found | pci.c:drv_i915_pci_write16 |  |
| 40 | `osdep_pci_write32` | found | pci.c:drv_i915_pci_write32 |  |
| 43 | `osdep_pci_find_capability` | found | pci.c:drv_i915_pci_find_capability |  |
| 64 | `osdep_pci_bar_kind` | found | pci.c:drv_i915_pci_bar_kind |  |
| 86 | `osdep_pci_set_power_state` | found | pci.c:drv_i915_pci_set_power_state |  |
| 104 | `wanted_decode` | found | pci.c:i915_pci_wanted_decode | match by substr |
| 121 | `osdep_pci_enable_device` | found | pci.c:drv_i915_pci_enable_device |  |
| 150 | `osdep_pci_disable_device` | found | pci.c:drv_i915_pci_disable_device |  |
| 168 | `osdep_pci_is_enabled` | found | pci.c:drv_i915_pci_is_enabled |  |
| 174 | `osdep_pci_restore` | found | pci.c:drv_i915_pci_restore |  |
| 185 | `osdep_pci_set_bus_master` | found | pci.c:drv_i915_pci_set_bus_master |  |
| 201 | `msi_write_message` | found | pci.c:i915_pci_msi_write_message | match by substr |
| 212 | `msi_set_enable` | found | pci.c:i915_pci_msi_set_enable | match by substr |
| 224 | `osdep_pci_setup_msi` | found | pci.c:drv_i915_pci_setup_msi |  |
| 266 | `osdep_pci_teardown_msi` | found | pci.c:drv_i915_pci_teardown_msi |  |
| 283 | `osdep_pci_msi_enabled` | found | pci.c:drv_i915_pci_msi_enabled |  |

#### `parity/osdep/runtime_pm.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 5 | `tr` | found | runtime-pm.c:i915_rpm_note |  |
| 12 | `osdep_rpm_init_early` | found | runtime-pm.c:drv_i915_rpm_init_early |  |
| 29 | `osdep_rpm_enable` | found | runtime-pm.c:drv_i915_rpm_enable |  |
| 36 | `osdep_rpm_is_enabled` | found | runtime-pm.c:drv_i915_rpm_is_enabled |  |
| 42 | `osdep_rpm_get_noresume` | found | runtime-pm.c:drv_i915_rpm_get_noresume |  |
| 49 | `do_resume` | found | runtime-pm.c:i915_rpm_resume |  |
| 64 | `osdep_rpm_get_sync` | found | runtime-pm.c:drv_i915_rpm_get_sync |  |
| 80 | `osdep_rpm_resume_and_get` | found | runtime-pm.c:drv_i915_rpm_resume_and_get |  |
| 97 | `osdep_rpm_put` | found | runtime-pm.c:drv_i915_rpm_put |  |
| 113 | `osdep_rpm_usage` | found | runtime-pm.c:drv_i915_rpm_usage |  |
| 119 | `osdep_rpm_active` | found | runtime-pm.c:drv_i915_rpm_active |  |

#### `parity/osdep/sync.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 5 | `tr` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 14 | `osdep_completion_init` | found | sync.c:drv_i915_completion_init |  |
| 22 | `osdep_reinit_completion` | found | sync.c:drv_i915_reinit_completion |  |
| 30 | `osdep_complete` | found | sync.c:drv_i915_complete |  |
| 38 | `osdep_complete_all` | test-moved | src/drivers/gpu/i915/tests/contracts/README.md (content: "complete_all") | S5 T2; osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 45 | `osdep_completion_done` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 51 | `try_consume` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 63 | `osdep_wait_for_completion_timeout` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22 |
| 87 | `osdep_work_init` | found | workqueue.c:drv_i915_work_init |  |
| 99 | `osdep_workqueue_init` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 113 | `osdep_queue_work` | found | workqueue.c:drv_i915_queue_work |  |
| 137 | `remove_pending` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 161 | `osdep_cancel_work` | found | workqueue.c:drv_i915_cancel_work |  |
| 172 | `osdep_cancel_work_sync` | found | workqueue.c:drv_i915_cancel_work_sync |  |
| 189 | `osdep_flush_workqueue` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 217 | `osdep_workqueue_pending` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |
| 223 | `osdep_work_is_running` | test | T2 | osdep sync model, test-only [s1-reports](i915-rebuild-s1-reports.md) L22; caller parity/tests/sync_contract_test.c |

#### `parity/osdep/trace.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 5 | `osdep_trace_init` | found | trace.c:drv_i915_trace_init |  |
| 24 | `osdep_trace_emit` | found | trace.c:drv_i915_trace_record (rules §2) |  |
| 50 | `osdep_trace_count` | found | trace.c:drv_i915_trace_count |  |
| 58 | `osdep_trace_snapshot` | found | trace.c:drv_i915_trace_snapshot |  |
| 77 | `osdep_trace_op_name` | found | trace.c:drv_i915_trace_op_name |  |

#### `parity/pch.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 50 | `parity_pch_test_set_bridges` | test | T4a | test hook g_bridge_test [s4 §3/§8](i915-rebuild-s4.md) L247-249, L377-379; caller ktest.c |
| 56 | `parity_intel_pch_type` | found | display/display.c:drv_i915_pch_type |  |
| 128 | `parity_intel_is_virt_pch` | found | display/display.c:drv_i915_is_virt_pch |  |
| 144 | `intel_virt_detect_pch` | found | display/display.c:i915_virt_detect_pch |  |
| 162 | `next_isa_bridge` | found | display/display.c:i915_next_isa_bridge |  |
| 195 | `parity_intel_detect_pch` | found | display/display.c:drv_i915_detect_pch |  |

#### `parity/pcode.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 31 | `pcode_check_status` | found | power.c:i915_pcode_status |  |
| 48 | `parity_snb_pcode_rw` | found | power.c:i915_pcode_rw |  |
| 77 | `parity_pcode_read` | found | power.c:drv_i915_pcode_read |  |
| 92 | `parity_snb_pcode_write_timeout` | found | power.c:i915_pcode_write_timeout |  |
| 108 | `parity_snb_pcode_write` | found | power.c:drv_i915_snb_pcode_write |  |
| 120 | `parity_skl_pcode_try_request` | found | power.c:i915_pcode_try_request | match by tokens |
| 132 | `parity_pcode_poll` | found | power.c:i915_pcode_poll |  |
| 157 | `parity_skl_pcode_request` | found | power.c:drv_i915_skl_pcode_request |  |

#### `parity/power_domains.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 75 | `pw_dom` | found | display/power.c:i915_pw_dom |  |
| 82 | `get_allowed_dc_mask` | found | display/power.c:i915_get_allowed_dc_mask |  |
| 95 | `sanitize_target_dc_state` | found | display/power.c:i915_sanitize_target_dc_state |  |
| 112 | `sanitize_disable_power_well` | found | display/power.c:i915_sanitize_disable_power_well |  |
| 120 | `pw_add` | found | display/power.c:i915_pw_add |  |
| 155 | `power_map_init` | found | display/power.c:i915_power_map_init |  |
| 293 | `power_map_init_tgl` | found | display/power.c:i915_power_map_init_tgl |  |
| 417 | `parity_intel_power_domains_init` | found | display/power.c:drv_i915_power_domains_init |  |
| 478 | `parity_intel_power_domains_cleanup` | found | display/power.c:drv_i915_power_domains_cleanup |  |
| 490 | `parity_power_domain_wells` | found | display/power.c:drv_i915_power_domain_wells |  |
| 499 | `parity_power_well_by_id` | found | display/power.c:drv_i915_power_well_by_id |  |
| 516 | `pw_driver_reg` | found | display/power.c:i915_pw_driver_reg |  |
| 525 | `pw_req` | found | display/power.c:i915_pw_req |  |
| 526 | `pw_state` | found | display/power.c:i915_pw_state |  |
| 538 | `pw_wait_fuse` | found | display/power.c:i915_pw_wait_fuse |  |
| 554 | `pw_post_enable` | found | display/power.c:i915_pw_post_enable |  |
| 576 | `parity_power_well_enable` | found | display/power.c:drv_i915_power_well_enable |  |
| 650 | `parity_power_well_disable` | found | display/power.c:drv_i915_power_well_disable |  |
| 731 | `parity_power_well_is_enabled` | found | display/power.c:drv_i915_power_well_is_enabled |  |
| 753 | `parity_power_well_sync_hw` | found | display/power.c:drv_i915_power_well_sync_hw |  |
| 785 | `parity_power_well_get` | found | display/power.c:drv_i915_power_well_get |  |
| 799 | `parity_power_well_put` | found | display/power.c:drv_i915_power_well_put |  |
| 818 | `parity_display_power_is_enabled` | found | display/power.c:drv_i915_display_power_is_enabled |  |
| 851 | `parity_power_domain_hw_state_on` | found | display/power.c:drv_i915_power_domain_hw_state_on |  |
| 872 | `mask_test` | found | display/power.c:i915_mask_test |  |
| 878 | `mask_set` | found | display/power.c:i915_mask_set |  |
| 884 | `mask_clear` | found | display/power.c:i915_mask_clear |  |
| 890 | `mask_empty` | found | display/power.c:i915_mask_empty |  |
| 898 | `verify_async_put_domains_state` | found | display/power.c:i915_verify_async_put_domains_state |  |
| 926 | `cancel_async_put_work` | found | display/power.c:i915_cancel_async_put_work |  |
| 935 | `grab_async_put_ref` | found | display/power.c:i915_grab_async_put_ref |  |
| 956 | `get_domain_locked` | found | display/power.c:i915_get_domain_locked |  |
| 984 | `put_domain_locked` | found | display/power.c:i915_put_domain_locked |  |
| 1007 | `parity_display_power_get` | found | display/power.c:drv_i915_display_power_get |  |
| 1019 | `parity_display_power_put` | found | display/power.c:drv_i915_display_power_put |  |
| 1028 | `parity_display_power_async_bind` | found | display/power.c:drv_i915_display_power_async_bind |  |
| 1040 | `queue_async_put_domains_work` | found | display/power.c:i915_queue_async_put_domains_work |  |
| 1053 | `release_async_put_domains` | found | display/power.c:i915_release_async_put_domains |  |
| 1071 | `parity_display_power_put_async` | found | display/power.c:drv_i915_display_power_put_async |  |
| 1103 | `parity_display_power_async_work` | found | display/power.c:drv_i915_display_power_async_work |  |
| 1135 | `parity_display_power_flush_work` | found | display/power.c:drv_i915_display_power_flush_work |  |
| 1155 | `parity_display_power_flush_work_sync` | found | display/power.c:drv_i915_display_power_flush_work_sync |  |
| 1170 | `parity_intel_pmdemand_init_early` | found | display/power.c:drv_i915_pmdemand_init_early |  |
| 1179 | `gen9_dc_mask` | found | display/power.c:i915_gen9_dc_mask |  |
| 1195 | `gen9_write_dc_state` | found | display/power.c:i915_gen9_write_dc_state |  |
| 1218 | `parity_gen9_set_dc_state` | found | display/power.c:drv_i915_gen9_set_dc_state |  |

#### `parity/probe.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 68 | `parity_dump_trace` | found | device.c:i915_start_dump_trace |  |
| 96 | `devid_is_tigerlake` | found | device.c:i915_product_is_tigerlake |  |
| 108 | `n0_pipe_powered` | found | display/takeover.c:drv_i915_n0_pipe_powered |  |
| 118 | `outcome_name` | found | device.c (replaced: "start stopped at %s: %d" log, L432-438; outcome enum dropped) |  |
| 129 | `popcount32` | found | device.c:i915_popcount32 |  |
| 138 | `parity_cpu_phys_bits` | found | device.c:i915_cpu_physical_bits |  |
| 153 | `drv_i915_parity_attach` | found | device.c:drv_i915_device_start (+ i915_start_* stages) |  |

#### `parity/pte.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 5 | `parity_dma_in_range` | found | ggtt.c:drv_i915_dma_in_range |  |
| 18 | `encode_common` | found | ggtt.c:i915_ggtt_encode |  |
| 32 | `parity_ggtt_pte_encode` | found | ggtt.c:drv_i915_ggtt_pte_encode |  |
| 39 | `parity_ppgtt_pte_encode` | test | T2 | tests [s1-reports](i915-rebuild-s1-reports.md) L54; caller parity/tests/pte_contract_test.c |

#### `parity/pxp.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 14 | `fail` | found | display/dp-sink.c:i915_edp_fail |  |
| 24 | `parity_intel_pxp_init` | found | pxp.c:drv_i915_pxp_init |  |
| 77 | `parity_intel_pxp_fini` | found | pxp.c:drv_i915_pxp_fini |  |

#### `parity/reset.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 23 | `parity_gt_reset_all` | found | reset.c:drv_i915_gt_reset_all |  |

#### `parity/resident_display.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 62 | `rd_init` | found | display/display.c:drv_i915_display_bind_ops + display/present.c:drv_i915_present_lease_init ([s4 §8](i915-rebuild-s4.md) L366) |  |
| 72 | `rd_panel` | found | display/display.c:drv_i915_display_panel | match by tokens |
| 84 | `rd_device_query` | found | display/scanout.c:i915_scanout_device_query | match by tokens |
| 94 | `rd_constraints` | found | display/scanout.c:i915_scanout_constraints | match by tokens |
| 114 | `rd_query` | found | display/display.c:i915_display_query | match by tokens |
| 157 | `rd_mode` | found | display/display.c:i915_display_mode (function containing the matched text) | match by string |
| 202 | `rd_claim` | found | display/display.c:i915_display_claim (function containing the matched text) | match by string |
| 228 | `rd_release_locked` | found | display/display.c:i915_display_claim (function containing the matched text) | match by string |
| 245 | `rd_release` | found | display/present.c:drv_i915_present_release | match by tokens |
| 268 | `parity_shim_blit_source` | test | T4b | S5 T4b (destination not present yet) |
| 279 | `rd_blit_build` | found | display/present.c:i915_present_blit_build | match by tokens |
| 306 | `rd_present` | found | display/present.c:drv_i915_present_display_present (function containing the matched text) | match by string |
| 373 | `rd_wait` | found | display/present.c:drv_i915_present_display_wait | match by tokens |
| 395 | `rd_events` | found | display/hotplug.c:drv_i915_hpd_events | match by tokens |
| 413 | `drv_i915_resident_display_close` | found | display/display.c:drv_i915_display_session_close |  |

#### `parity/runner.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 59 | `ensure_lock` | found | device.c:i915_start_registry_init |  |
| 68 | `probe_status_name` | test | T4a | S5 T4a (destination not present yet) |
| 79 | `runner_thread` | found | device.c:i915_start_worker |  |
| 160 | `try_launch` | found | device.c:i915_start_launch |  |
| 191 | `drv_i915_parity_runner_register` | found | device.c:drv_i915_device_schedule_start |  |
| 205 | `drv_i915_parity_runner_start` | found | device.c:drv_i915_runtime_ready |  |

#### `parity/tests/dma_contract_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 22 | `trace_has_order` | test-moved | tests/contracts/dma_contract_test.c:dma_trace_has_order | S5 T2 |
| 39 | `main` | test-moved | {plan/ws029/tests/i915-backend-test.c,plan/ws029/tests/i915-gtt-test.c,plan/ws029/tests/i915-irq-test.c}:main | S5 T2 |

#### `parity/tests/mmio_contract_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 17 | `main` | test-moved | {plan/ws029/tests/i915-backend-test.c,plan/ws029/tests/i915-gtt-test.c,plan/ws029/tests/i915-irq-test.c}:main | S5 T2 |

#### `parity/tests/mock_dma.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 19 | `reverse24` | test | T2 | S5 T2 (destination not present yet) |
| 32 | `mock_dma_translate` | test-moved | tests/contracts/mock_dma.c:mock_dma_translate | S5 T2 |
| 40 | `mock_dma_untranslate` | test-moved | tests/contracts/mock_dma.c:mock_dma_untranslate | S5 T2 |
| 49 | `mock_set_info` | test-moved | tests/contracts/mock_dma.c:mock_dma_set_info | S5 T2 |
| 60 | `mock_map_sg` | test-moved | tests/contracts/mock_dma.c:mock_dma_map_sg | S5 T2 |
| 97 | `mock_unmap_sg` | test-moved | tests/contracts/mock_dma.c:mock_dma_unmap_sg | S5 T2 |
| 110 | `mock_map_page` | test-moved | tests/contracts/mock_dma.c:mock_dma_map_page | S5 T2 |
| 124 | `mock_unmap_page` | test-moved | tests/contracts/mock_dma.c:mock_dma_unmap_page | S5 T2 |
| 135 | `mock_sync_for_device` | test-moved | tests/contracts/mock_dma.c:mock_dma_sync_for_device | S5 T2 |
| 146 | `mock_sync_for_cpu` | test-moved | tests/contracts/mock_dma.c:mock_dma_sync_for_cpu | S5 T2 |
| 157 | `mock_dma_backend` | test | T2 | S5 T2 (destination not present yet) |
| 176 | `mock_dma_reset` | test-moved | tests/contracts/mock_dma.c:mock_dma_reset | S5 T2 |

#### `parity/tests/mock_mmio.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `find` | test-moved | tests/contracts/mock_mmio.c:mock_mmio_find | S5 T2 |
| 27 | `slot` | test-moved | tests/contracts/mock_mmio.c:mock_mmio_slot | S5 T2 |
| 43 | `m_raw_read32` | test | T2 | S5 T2 (destination not present yet) |
| 54 | `m_raw_write32` | test | T2 | S5 T2 (destination not present yet) |
| 66 | `m_fw_request` | test | T2 | S5 T2 (destination not present yet) |
| 75 | `m_fw_ack` | test | T2 | S5 T2 (destination not present yet) |
| 85 | `mock_mmio_backend` | test | T2 | S5 T2 (destination not present yet) |
| 98 | `mock_mmio_reset` | test-moved | tests/contracts/mock_mmio.c:mock_mmio_reset | S5 T2 |
| 118 | `mock_mmio_preset` | test-moved | tests/contracts/mock_mmio.c:mock_mmio_preset | S5 T2 |
| 126 | `mock_mmio_peek` | test-moved | tests/contracts/mock_mmio.c:mock_mmio_peek | S5 T2 |

#### `parity/tests/mock_pci.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 5 | `m_read8` | test | T2 | S5 T2 (destination not present yet) |
| 12 | `m_read16` | test | T2 | S5 T2 (destination not present yet) |
| 21 | `m_read32` | test | T2 | S5 T2 (destination not present yet) |
| 31 | `m_write8` | test | T2 | S5 T2 (destination not present yet) |
| 40 | `m_write16` | test | T2 | S5 T2 (destination not present yet) |
| 51 | `m_write32` | test | T2 | S5 T2 (destination not present yet) |
| 64 | `m_alloc_msi_vector` | test | T2 | S5 T2 (destination not present yet) |
| 80 | `m_free_msi_vector` | test | T2 | S5 T2 (destination not present yet) |
| 89 | `mock_pci_backend` | test | T2 | S5 T2 (destination not present yet) |
| 101 | `base` | test-moved | plan/ws031/tests/native-decide-host-test.c:base | S5 T2 |
| 121 | `mem_bar` | test-moved | tests/contracts/mock_pci.c:mock_pci_mem_bar | S5 T2 |
| 132 | `io_bar` | test-moved | tests/contracts/mock_pci.c:mock_pci_io_bar | S5 T2 |
| 143 | `pm_cap` | test-moved | tests/contracts/mock_pci.c:mock_pci_pm_capability | S5 T2 |
| 151 | `msi_cap` | test-moved | tests/contracts/mock_pci.c:mock_pci_msi_capability | S5 T2 |
| 160 | `pcie_cap` | test-moved | tests/contracts/mock_pci.c:mock_pci_pcie_capability | S5 T2 |
| 167 | `mock_pci_setup_full` | test-moved | tests/contracts/mock_pci.c:mock_pci_setup_full | S5 T2 |
| 178 | `mock_pci_setup_no_msi` | test-moved | tests/contracts/mock_pci.c:mock_pci_setup_no_msi | S5 T2 |
| 188 | `mock_pci_setup_no_pm` | test-moved | tests/contracts/mock_pci.c:mock_pci_setup_no_pm | S5 T2 |
| 198 | `mock_pci_setup_io_and_mem` | test-moved | tests/contracts/mock_pci.c:mock_pci_setup_io_and_mem | S5 T2 |

#### `parity/tests/pci_contract_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 19 | `main` | test-moved | {plan/ws029/tests/i915-backend-test.c,plan/ws029/tests/i915-gtt-test.c,plan/ws029/tests/i915-irq-test.c}:main | S5 T2 |

#### `parity/tests/pte_contract_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 21 | `main` | test-moved | {plan/ws029/tests/i915-backend-test.c,plan/ws029/tests/i915-gtt-test.c,plan/ws029/tests/i915-irq-test.c}:main | S5 T2 |

#### `parity/tests/rpm_contract_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 18 | `m_resume` | test | T2 | S5 T2 (destination not present yet) |
| 19 | `m_suspend` | test | T2 | S5 T2 (destination not present yet) |
| 22 | `main` | test-moved | {plan/ws029/tests/i915-backend-test.c,plan/ws029/tests/i915-gtt-test.c,plan/ws029/tests/i915-irq-test.c}:main | S5 T2 |

#### `parity/tests/sync_contract_test.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 20 | `tick_complete` | test | T2 | S5 T2 (destination not present yet) |
| 29 | `work_fn` | test | T2 | S5 T2 (destination not present yet) |
| 34 | `work_cancel_other` | test | T2 | S5 T2 (destination not present yet) |
| 43 | `work_requeue_self` | test | T2 | S5 T2 (destination not present yet) |
| 51 | `main` | test-moved | {plan/ws029/tests/i915-backend-test.c,plan/ws029/tests/i915-gtt-test.c,plan/ws029/tests/i915-irq-test.c}:main | S5 T2 |

#### `parity/timer_calc.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 9 | `parity_timer_calc_init` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |
| 19 | `parity_timer_set_sleep` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |
| 28 | `parity_timer_clear_sleep` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |
| 35 | `parity_timer_next_event` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |
| 43 | `parity_timer_on_fire` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |

#### `parity/vbt/intel_bios_port.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 87 | `_get_blocksize` | found | display/vbt.c:i915_get_blocksize | match by substr |
| 97 | `get_blocksize` | found | display/vbt.c:i915_get_blocksize |  |
| 103 | `find_raw_section` | found | display/vbt.c:i915_find_raw_section |  |
| 137 | `raw_block_offset` | found | display/vbt.c:i915_raw_block_offset |  |
| 155 | `bdb_find_section` | found | display/vbt.c:i915_bdb_find_section |  |
| 210 | `lfp_data_min_size` | found | display/vbt.c:i915_lfp_data_min_size |  |
| 227 | `validate_lfp_data_ptrs` | found | display/vbt.c:i915_validate_lfp_data_ptrs |  |
| 321 | `fixup_lfp_data_ptrs` | found | display/vbt.c:i915_fixup_lfp_data_ptrs |  |
| 350 | `make_lfp_data_ptr` | found | display/vbt.c:i915_make_lfp_data_ptr |  |
| 362 | `next_lfp_data_ptr` | found | display/vbt.c:i915_next_lfp_data_ptr |  |
| 370 | `generate_lfp_data_ptrs` | found | display/vbt.c:i915_generate_lfp_data_ptrs |  |
| 461 | `init_bdb_block` | found | display/vbt.c:i915_init_bdb_block |  |
| 518 | `init_bdb_blocks` | found | display/vbt.c:i915_init_bdb_blocks |  |
| 535 | `fill_detail_timing_data` | found | display/vbt.c:i915_fill_detail_timing_data |  |
| 592 | `get_lvds_dvo_timing` | found | display/vbt.c:i915_get_lvds_dvo_timing |  |
| 600 | `get_lvds_fp_timing` | found | display/vbt.c:i915_get_lvds_fp_timing |  |
| 608 | `get_lvds_pnp_id` | found | display/vbt.c:i915_get_lvds_pnp_id |  |
| 616 | `get_lfp_data_tail` | found | display/vbt.c:i915_get_lfp_data_tail |  |
| 625 | `dump_pnp_id` | found | display/vbt.c:i915_dump_pnp_id |  |
| 638 | `opregion_get_panel_type` | found | display/vbt.c:i915_opregion_get_panel_type |  |
| 645 | `vbt_get_panel_type` | found | display/vbt.c:i915_vbt_get_panel_type |  |
| 670 | `pnpid_get_panel_type` | found | display/vbt.c:i915_pnpid_get_panel_type |  |
| 720 | `fallback_get_panel_type` | found | display/vbt.c:i915_fallback_get_panel_type |  |
| 734 | `get_panel_type` | found | display/vbt.c:i915_get_panel_type |  |
| 793 | `panel_bits` | found | display/vbt.c:i915_panel_bits |  |
| 798 | `panel_bool` | found | display/vbt.c:i915_panel_bool |  |
| 805 | `parse_panel_options` | found | display/vbt.c:i915_parse_panel_options |  |
| 852 | `parse_lfp_panel_dtd` | found | display/vbt.c:i915_parse_lfp_panel_dtd |  |
| 893 | `parse_lfp_data` | found | display/vbt.c:i915_parse_lfp_data |  |
| 934 | `parse_generic_dtd` | found | display/vbt.c:i915_parse_generic_dtd |  |
| 1024 | `parse_lfp_backlight` | found | display/vbt.c:i915_parse_lfp_backlight |  |
| 1111 | `intel_bios_ssc_frequency` | found | display/vbt.c:i915_bios_ssc_frequency |  |
| 1126 | `parse_general_features` | found | display/vbt.c:i915_parse_general_features |  |
| 1168 | `child_device_ptr` | found | display/vbt.c:i915_child_device_ptr |  |
| 1175 | `parse_driver_features` | found | display/vbt.c:i915_parse_driver_features |  |
| 1211 | `parse_panel_driver_features` | found | display/vbt.c:i915_parse_panel_driver_features |  |
| 1245 | `parse_power_conservation_features` | found | display/vbt.c:i915_parse_power_conservation_features |  |
| 1288 | `parse_edp` | found | display/vbt.c:i915_parse_edp |  |
| 1430 | `translate_iboost` | found | display/vbt.c:i915_translate_iboost |  |
| 1491 | `map_ddc_pin` | found | display/vbt.c:i915_map_ddc_pin |  |
| 1533 | `dvo_port_type` | found | display/vbt.c:i915_dvo_port_type |  |
| 1566 | `__dvo_port_to_port` | found | display/vbt.c:i915_dvo_port_to_port | match by tokens |
| 1585 | `dvo_port_to_port` | found | display/vbt.c:i915_dvo_port_to_port |  |
| 1662 | `dsi_dvo_port_to_port` | found | display/vbt.c:i915_dsi_dvo_port_to_port |  |
| 1677 | `intel_bios_encoder_port` | found | display/vbt.c:i915_bios_encoder_port |  |
| 1690 | `parse_bdb_230_dp_max_link_rate` | found | display/vbt.c:i915_parse_bdb_230_dp_max_link_rate |  |
| 1713 | `parse_bdb_216_dp_max_link_rate` | found | display/vbt.c:i915_parse_bdb_216_dp_max_link_rate |  |
| 1728 | `intel_bios_dp_max_link_rate` | found | display/vbt.c:i915_bios_dp_max_link_rate |  |
| 1739 | `intel_bios_dp_max_lane_count` | found | display/vbt.c:i915_bios_dp_max_lane_count |  |
| 1747 | `sanitize_device_type` | found | display/vbt.c:i915_sanitize_device_type |  |
| 1768 | `sanitize_hdmi_level_shift` | found | display/vbt.c:i915_sanitize_hdmi_level_shift |  |
| 1790 | `intel_bios_encoder_supports_crt` | found | display/vbt.c:i915_bios_encoder_supports_crt |  |
| 1796 | `intel_bios_encoder_supports_dvi` | found | display/vbt.c:i915_bios_encoder_supports_dvi |  |
| 1802 | `intel_bios_encoder_supports_hdmi` | found | display/vbt.c:i915_bios_encoder_supports_hdmi |  |
| 1809 | `intel_bios_encoder_supports_dp` | found | display/vbt.c:i915_bios_encoder_supports_dp |  |
| 1815 | `intel_bios_encoder_supports_edp` | found | display/vbt.c:i915_bios_encoder_supports_edp |  |
| 1822 | `intel_bios_encoder_supports_dsi` | found | display/vbt.c:i915_bios_encoder_supports_dsi |  |
| 1828 | `intel_bios_encoder_is_lspcon` | found | display/vbt.c:i915_bios_encoder_is_lspcon |  |
| 1834 | `intel_bios_hdmi_level_shift` | found | display/vbt.c:drv_i915_bios_hdmi_level_shift |  |
| 1843 | `intel_bios_hdmi_max_tmds_clock` | found | display/vbt.c:i915_bios_hdmi_max_tmds_clock |  |
| 1867 | `is_port_valid` | found | display/vbt.c:i915_is_port_valid |  |
| 1880 | `print_ddi_port` | found | display/vbt.c:i915_print_ddi_port |  |
| 1951 | `parse_ddi_port` | found | display/vbt.c:i915_parse_ddi_port |  |
| 1971 | `has_ddi_port_info` | found | display/vbt.c:i915_has_ddi_port_info |  |
| 1976 | `parse_ddi_ports` | found | display/vbt.c:i915_parse_ddi_ports |  |
| 1991 | `parse_general_definitions` | found | display/vbt.c:i915_parse_general_definitions |  |
| 2091 | `init_vbt_defaults` | found | display/vbt.c:i915_init_vbt_defaults |  |
| 2116 | `init_vbt_panel_defaults` | found | display/vbt.c:i915_init_vbt_panel_defaults |  |
| 2127 | `init_vbt_missing_defaults` | found | display/vbt.c:i915_init_vbt_missing_defaults |  |
| 2183 | `get_bdb_header` | found | display/vbt.c:i915_get_bdb_header |  |
| 2197 | `intel_bios_is_valid_vbt` | found | display/vbt.c:drv_i915_bios_is_valid_vbt |  |
| 2250 | `intel_bios_init` | found | display/vbt.c:drv_i915_bios_init |  |
| 2299 | `intel_bios_init_panel` | found | display/vbt.c:i915_bios_init_panel |  |
| 2333 | `intel_bios_init_panel_early` | found | display/vbt.c:i915_bios_init_panel_early |  |
| 2340 | `intel_bios_init_panel_late` | found | display/vbt.c:i915_bios_init_panel_late |  |
| 2352 | `intel_bios_driver_remove` | found | display/vbt.c:drv_i915_bios_driver_remove |  |
| 2369 | `intel_bios_fini_panel` | found | display/vbt.c:i915_bios_fini_panel |  |
| 2394 | `intel_bios_is_port_present` | found | display/vbt.c:i915_bios_is_port_present |  |
| 2414 | `intel_bios_encoder_supports_dp_dual_mode` | found | display/vbt.c:i915_bios_encoder_supports_dp_dual_mode |  |
| 2486 | `map_aux_ch` | found | display/vbt.c:i915_map_aux_ch |  |
| 2517 | `intel_bios_dp_aux_ch` | found | display/vbt.c:i915_bios_dp_aux_ch |  |
| 2525 | `intel_bios_dp_has_shared_aux_ch` | found | display/vbt.c:i915_bios_dp_has_shared_aux_ch |  |
| 2546 | `intel_bios_dp_boost_level` | found | display/vbt.c:i915_bios_dp_boost_level |  |
| 2554 | `intel_bios_hdmi_boost_level` | found | display/vbt.c:i915_bios_hdmi_boost_level |  |
| 2562 | `intel_bios_hdmi_ddc_pin` | found | display/vbt.c:i915_bios_hdmi_ddc_pin |  |
| 2570 | `intel_bios_encoder_supports_typec_usb` | found | display/vbt.c:i915_bios_encoder_supports_typec_usb |  |
| 2575 | `intel_bios_encoder_supports_tbt` | found | display/vbt.c:i915_bios_encoder_supports_tbt |  |
| 2580 | `intel_bios_encoder_lane_reversal` | found | display/vbt.c:i915_bios_encoder_lane_reversal |  |
| 2585 | `intel_bios_encoder_hpd_invert` | found | display/vbt.c:i915_bios_encoder_hpd_invert |  |
| 2591 | `intel_bios_encoder_data_lookup` | found | display/vbt.c:i915_bios_encoder_data_lookup |  |
| 2603 | `intel_bios_for_each_encoder` | found | display/vbt.c:i915_bios_for_each_encoder |  |

#### `parity/vbt/parity_vbt_glue.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 23 | `parity_vbt_zalloc` | found | display/vbt.c:drv_i915_vbt_zalloc |  |
| 41 | `parity_vbt_free` | found | display/vbt.c:drv_i915_vbt_free |  |
| 48 | `parity_vbt_provider_get` | found | display/vbt.c:drv_i915_vbt_provider_get |  |
| 55 | `parity_vbt_set_log_level` | found | display/vbt.c:drv_i915_vbt_set_log_level |  |
| 61 | `parity_vbt_note` | found | display/vbt.c:drv_i915_vbt_note |  |
| 70 | `parity_vbt_log_enabled` | found | display/vbt.c:drv_i915_vbt_log_enabled |  |
| 78 | `drm_mode_set_name` | found | display/edid.c:drv_i915_lcd_drm_mode_set_name |  |
| 98 | `parity_vbt_validate` | found | display/vbt.c:drv_i915_vbt_validate |  |
| 104 | `parity_vbt_init` | found | display/vbt.c:drv_i915_vbt_init |  |
| 184 | `parity_vbt_encoder_for_port` | found | display/vbt.c:drv_i915_vbt_encoder_for_port |  |
| 197 | `parity_vbt_init_panel` | found | display/vbt.c:drv_i915_vbt_init_panel |  |
| 264 | `parity_vbt_fini` | found | display/vbt.c:drv_i915_vbt_fini |  |

#### `parity/vbt/vbt_compat.h`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 68 | `INIT_LIST_HEAD` | found | display/vbt.h:i915_init_list_head | match by tokens |
| 69 | `list_empty` | found | display/vbt.h:i915_list_empty |  |
| 70 | `list_add_tail` | found | display/vbt.h:i915_list_add_tail |  |
| 74 | `list_del` | found | display/vbt.h:i915_list_del |  |
| 95 | `kmemdup` | found | display/vbt.h:i915_vbt_kmemdup |  |
| 243 | `drm_edid_raw` | found | display/vbt.h:i915_drm_edid_raw |  |
| 245 | `drm_edid_decode_mfg_id` | found | display/vbt.h:i915_drm_edid_decode_mfg_id |  |
| 288 | `intel_port_to_phy` | found | display/takeover.c:drv_i915_port_to_phy |  |
| 295 | `intel_phy_is_tc` | found | display/takeover.c:drv_i915_phy_is_tc |  |
| 301 | `intel_gmbus_is_valid_pin` | found | display/vbt.h:i915_vbt_intel_gmbus_is_valid_pin |  |
| 307 | `intel_opregion_get_panel_type` | found | display/vbt.c:i915_opregion_get_panel_type |  |

#### `parity/vga.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 33 | `parity_intel_gmch_vga_set_state` | found | display/takeover.c:drv_i915_gmch_vga_set_state |  |
| 63 | `parity_intel_gmch_vga_set_decode` | found | display/takeover.c:drv_i915_gmch_vga_set_decode |  |
| 80 | `parity_vga_client_register` | found | display/takeover.c:i915_vga_client_register |  |
| 92 | `parity_intel_vga_register` | found | display/takeover.c:drv_i915_vga_register |  |
| 126 | `parity_intel_vga_unregister` | found | display/takeover.c:drv_i915_vga_unregister |  |
| 149 | `parity_vga_io_test_set` | found | display/takeover.c:drv_i915_vga_io_test_set |  |
| 155 | `vga_get_legacy_io` | found | display/takeover.c:i915_vga_get_legacy_io |  |
| 161 | `vga_in8` | found | display/takeover.c:i915_vga_in8 |  |
| 166 | `vga_out8` | found | display/takeover.c:i915_vga_out8 |  |
| 172 | `vga_put_legacy_io` | found | display/takeover.c:i915_vga_put_legacy_io |  |
| 178 | `parity_intel_vga_reset_io_mem` | found | display/takeover.c:drv_i915_vga_reset_io_mem |  |
| 200 | `parity_intel_vga_disable` | found | display/takeover.c:drv_i915_vga_disable |  |

#### `parity/wait.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 46 | `parity_time_base_set_fault` | found | sync.c:i915_time_base_latch |  |
| 55 | `parity_wait_time_base_faulted` | found | sync.c:drv_i915_time_base_faulted |  |
| 61 | `parity_wait_time_base_ok` | found | sync.c:drv_i915_time_base_ok |  |
| 72 | `parity_wait_test_set` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |
| 79 | `parity_wait_test_reset_fault` | test | T4a | test-only [s1-reports](i915-rebuild-s1-reports.md) L22; only caller ktest.c |
| 85 | `time_read` | found | sync.c:i915_time_start |  |
| 93 | `time_sleep` | found | sync.c:i915_wait_reg_slow (inlined) |  |
| 105 | `us_to_ticks` | found | sync.c:i915_us_to_ticks |  |
| 126 | `read_counter_consistent` | found | sync.c:i915_time_now |  |
| 138 | `parity_udelay` | found | sync.c:drv_i915_udelay |  |
| 158 | `parity_wait_reg` | found | sync.c:drv_i915_wait_reg |  |

#### `ppgtt.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 52 | `drv_i915_ppgtt_create` | found | ppgtt.c:drv_i915_ppgtt_create |  |
| 104 | `drv_i915_ppgtt_destroy` | found | ppgtt.c:drv_i915_ppgtt_destroy |  |
| 141 | `drv_i915_ppgtt_va_alloc` | found | ppgtt.c:drv_i915_ppgtt_va_alloc |  |
| 174 | `drv_i915_ppgtt_insert` | found | ppgtt.c:drv_i915_ppgtt_insert |  |
| 188 | `drv_i915_ppgtt_insert_uncached` | found | ppgtt.c:drv_i915_ppgtt_insert_uncached |  |
| 199 | `i915_ppgtt_insert_bits` | found | ppgtt.c:i915_ppgtt_insert_bits |  |
| 245 | `drv_i915_ppgtt_clear` | found | ppgtt.c:drv_i915_ppgtt_clear |  |
| 273 | `drv_i915_ppgtt_lookup` | retired |  | unused; [s1-reports](i915-rebuild-s1-reports.md) L54 |
| 289 | `i915_ppgtt_page_alloc` | found | ppgtt.c:i915_ppgtt_page_alloc |  |
| 320 | `i915_ppgtt_table` | found | ppgtt.c:i915_ppgtt_table |  |
| 335 | `i915_ppgtt_walk` | found | ppgtt.c:i915_ppgtt_walk |  |
| 392 | `i915_ppgtt_fill` | found | ppgtt.c:i915_ppgtt_fill |  |

#### `request.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 64 | `drv_i915_request_alloc` | found | request-queue.c:drv_i915_request_alloc |  |
| 105 | `drv_i915_request_release` | found | request-queue.c:drv_i915_request_release |  |
| 118 | `drv_i915_request_queue` | found | request-queue.c:drv_i915_request_queue |  |
| 139 | `drv_i915_request_kick` | **MISSING** |  | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| 193 | `drv_i915_request_retire` | **MISSING** |  | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| 227 | `drv_i915_request_fail` | found | request-queue.c:drv_i915_request_fail |  |
| 283 | `drv_i915_request_complete_list` | found | request-queue.c:drv_i915_request_complete_list |  |
| 337 | `i915_request_emit` | **MISSING** |  | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| 371 | `i915_request_emit_prologue` | **MISSING** |  | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| 424 | `i915_request_emit_breadcrumb` | **MISSING** |  | unreachable in the resident build: callers are the retired legacy engine.c and i915.c:1581, which PARITY_SHIM_REDIRECT (i915-old/i915.c:28, parity/legacy_shim.h:42) redirects to parity_shim_request_kick (now worker.c:drv_i915_worker_kick). No plan/report line records its retirement. |
| 476 | `i915_request_unlink` | found | request-queue.c:i915_request_unlink |  |

#### `selftest.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 48 | `drv_i915_selftest` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 140 | `drv_i915_clear_selftest` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 249 | `drv_i915_rcs_selftest` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 320 | `drv_i915_rt_selftest` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 425 | `i915_selftest_clflush` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 551 | `i915_draw_apply_engine_workarounds` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 595 | `i915_draw_fill_eot` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 614 | `i915_draw_emit` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 628 | `i915_draw_emit_disabled` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 642 | `i915_draw_emit_marker` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 655 | `i915_draw_emit_pipe_control` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 676 | `i915_draw_write_surface_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 713 | `i915_draw_write_dynamic_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 744 | `i915_draw_write_vertices` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 761 | `i915_draw_emit_vertex_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 829 | `i915_draw_emit_urb` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 876 | `i915_draw_emit_raster_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 959 | `i915_draw_emit_depth_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 990 | `i915_draw_build_batch` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1200 | `drv_i915_draw_fixture_write_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1214 | `drv_i915_draw_fixture_build_batch` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1253 | `drv_i915_tex_fixture_write_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1283 | `drv_i915_tex_fixture_write_state_ab_filter` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1295 | `drv_i915_tex_fixture_expected_pixel_linear` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1328 | `drv_i915_tex_fixture_write_state_ab` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1343 | `drv_i915_tex_fixture_build_batch` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1373 | `drv_i915_tex_fixture_fhd_write_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1401 | `drv_i915_tex_fixture_fhd_build_batch` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1415 | `drv_i915_tex_fixture_fhd_rt_rss` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1421 | `drv_i915_tex_fixture_fhd_ps_bytes` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1428 | `drv_i915_tex_fixture_fhd_same_texture_state` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1438 | `drv_i915_tex_fixture_pattern` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1469 | `drv_i915_tex_fixture_expected_pixel` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1479 | `drv_i915_draw_fixture_mocs` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1492 | `i915_draw_read_statistics` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1604 | `i915_compute_emit_sba` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1635 | `i915_compute_emit_copy` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1645 | `i915_compute_build_batch` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1757 | `i915_compute_power_probe` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1777 | `i915_golden_run` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 1826 | `drv_i915_compute_selftest` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |
| 2276 | `drv_i915_draw_selftest` | retired |  | legacy HW self-test [s1 §1](i915-rebuild-s1.md) L25; [s5](i915-rebuild-s5.md) L25 (T4a: legacy HW 試験は廃止) |

#### `uncore.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 44 | `drv_i915_read32` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (mmio.c:drv_i915_read32), which is the parity-side port, not this code |
| 69 | `drv_i915_write32` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (mmio.c:drv_i915_write32), which is the parity-side port, not this code |
| 94 | `drv_i915_wait32` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 136 | `drv_i915_forcewake_get` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (mmio.c:drv_i915_forcewake_get), which is the parity-side port, not this code |
| 169 | `drv_i915_forcewake_put` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (mmio.c:drv_i915_forcewake_put), which is the parity-side port, not this code |
| 206 | `drv_i915_uncore_init` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 241 | `drv_i915_gt_reset` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 263 | `drv_i915_domain_reset` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 290 | `i915_forcewake_domain_get` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 324 | `i915_forcewake_domain_put` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |
| 360 | `i915_forcewake_request_register` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (mmio.c:i915_forcewake_request_register), which is the parity-side port, not this code |
| 372 | `i915_forcewake_ack_register` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67; a same-named new function exists (mmio.c:i915_forcewake_ack_register), which is the parity-side port, not this code |
| 384 | `i915_timeout_ticks` | retired |  | [s1 §1](i915-rebuild-s1.md) L18-25; [plan §5](i915-rebuild-plan.md) L67 |

#### `vk/cmd.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 47 | `i915_vk_object_table_create` | found | render/object.c:drv_i915_object_table_create |  |
| 71 | `i915_vk_object_table_destroy` | found | render/object.c:drv_i915_object_table_destroy |  |
| 87 | `i915_vk_obj_insert` | found | render/object.c:drv_i915_object_insert |  |
| 139 | `i915_vk_obj_lookup` | found | render/object.c:drv_i915_object_lookup |  |
| 163 | `i915_vk_obj_remove` | found | render/object.c:drv_i915_object_remove |  |
| 186 | `i915_vk_read_u32` | found | render/codec.c:drv_i915_wire_read_u32 |  |
| 215 | `i915_vk_read_u64` | found | render/codec.c:drv_i915_wire_read_u64 |  |
| 230 | `i915_vk_read_handle` | found | render/codec.c:drv_i915_wire_read_handle |  |
| 238 | `i915_vk_read_array` | found | render/codec.c:drv_i915_wire_read_array |  |
| 266 | `i915_vk_reply_u32` | found | render/codec.c:drv_i915_wire_reply_u32 |  |
| 291 | `i915_vk_reply_u64` | found | render/codec.c:drv_i915_wire_reply_u64 |  |
| 302 | `i915_vk_reply_blob` | found | render/codec.c:drv_i915_wire_reply_bytes ([s3 §1](i915-rebuild-s3.md) L268) |  |
| 321 | `i915_vk_cmd_dispatch` | found | render/dispatch.c:drv_i915_render_dispatch |  |
| 376 | `i915_vk_route` | found | render/dispatch.c:i915_dispatch_route | match by substr |
| 413 | `i915_vk_cmd_set_reply` | found | render/transport.c:i915_transport_set_reply |  |
| 434 | `i915_vk_cmd_seek_reply` | found | render/transport.c:i915_transport_seek_reply |  |
| 452 | `i915_vk_cmd_builtin` | found | render/fence.c:drv_i915_render_fence_dispatch (function containing the matched text) | match by string |

#### `vk/cmdbuf.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 65 | `i915_vk_cmdbuf_result` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 77 | `i915_vk_cmdbuf_object_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 122 | `i915_vk_cmdbuf_object_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 142 | `i915_vk_cmdbuf_create_pool` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 191 | `i915_vk_cmdbuf_destroy_pool` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 220 | `i915_vk_cmdbuf_reset_pool` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 243 | `i915_vk_cmdbuf_allocate` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 310 | `i915_vk_cmdbuf_free` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 347 | `i915_vk_cmdbuf_begin_command` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 384 | `i915_vk_cmdbuf_end_command` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 412 | `i915_vk_cmdbuf_record_bind_pipeline` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 443 | `i915_vk_cmdbuf_record_bind_vertex` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 482 | `i915_vk_cmdbuf_record_draw` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 514 | `i915_vk_cmdbuf_record_end_render_pass` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 540 | `i915_vk_cmdbuf_queue_submit` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 610 | `i915_vk_cmdbuf_dispatch` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 657 | `i915_vk_cmdbuf_begin` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 668 | `i915_vk_cmdbuf_end` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 679 | `i915_vk_cmd_bind_pipeline` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 689 | `i915_vk_cmd_bind_vertex_buffers` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 706 | `i915_vk_cmd_bind_descriptor_sets` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 721 | `i915_vk_cmd_push_constants` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 736 | `i915_vk_cmd_begin_render_pass` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 751 | `i915_vk_cmd_end_render_pass` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 760 | `i915_vk_cmd_draw` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 791 | `i915_vk_queue_submit` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/command.c:i915_queue_submit |
| 849 | `i915_vk_cmdbuf_put` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |

#### `vk/codec-generated.inc`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 157 | `i915_vkc_dec_VkExtent2D` | found | data/vulkan-codec.inc:i915_vkc_dec_VkExtent2D |  |
| 168 | `i915_vkc_enc_VkExtent2D` | found | data/vulkan-codec.inc:i915_vkc_enc_VkExtent2D |  |
| 176 | `i915_vkc_dec_VkExtent3D` | found | data/vulkan-codec.inc:i915_vkc_dec_VkExtent3D |  |
| 188 | `i915_vkc_enc_VkExtent3D` | found | data/vulkan-codec.inc:i915_vkc_enc_VkExtent3D |  |
| 197 | `i915_vkc_dec_VkOffset2D` | found | data/vulkan-codec.inc:i915_vkc_dec_VkOffset2D |  |
| 208 | `i915_vkc_enc_VkOffset2D` | found | data/vulkan-codec.inc:i915_vkc_enc_VkOffset2D |  |
| 216 | `i915_vkc_dec_VkOffset3D` | found | data/vulkan-codec.inc:i915_vkc_dec_VkOffset3D |  |
| 228 | `i915_vkc_enc_VkOffset3D` | found | data/vulkan-codec.inc:i915_vkc_enc_VkOffset3D |  |
| 237 | `i915_vkc_dec_VkRect2D` | found | data/vulkan-codec.inc:i915_vkc_dec_VkRect2D |  |
| 248 | `i915_vkc_enc_VkRect2D` | found | data/vulkan-codec.inc:i915_vkc_enc_VkRect2D |  |
| 256 | `i915_vkc_dec_VkBufferMemoryBarrier` | found | data/vulkan-codec.inc:i915_vkc_dec_VkBufferMemoryBarrier |  |
| 274 | `i915_vkc_dec_VkDispatchIndirectCommand` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDispatchIndirectCommand |  |
| 286 | `i915_vkc_enc_VkDispatchIndirectCommand` | found | data/vulkan-codec.inc:i915_vkc_enc_VkDispatchIndirectCommand |  |
| 295 | `i915_vkc_dec_VkDrawIndexedIndirectCommand` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDrawIndexedIndirectCommand |  |
| 309 | `i915_vkc_enc_VkDrawIndexedIndirectCommand` | found | data/vulkan-codec.inc:i915_vkc_enc_VkDrawIndexedIndirectCommand |  |
| 320 | `i915_vkc_dec_VkDrawIndirectCommand` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDrawIndirectCommand |  |
| 333 | `i915_vkc_enc_VkDrawIndirectCommand` | found | data/vulkan-codec.inc:i915_vkc_enc_VkDrawIndirectCommand |  |
| 343 | `i915_vkc_dec_VkImageSubresourceRange` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageSubresourceRange |  |
| 357 | `i915_vkc_enc_VkImageSubresourceRange` | found | data/vulkan-codec.inc:i915_vkc_enc_VkImageSubresourceRange |  |
| 368 | `i915_vkc_dec_VkImageMemoryBarrier` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageMemoryBarrier |  |
| 387 | `i915_vkc_dec_VkMemoryBarrier` | found | data/vulkan-codec.inc:i915_vkc_dec_VkMemoryBarrier |  |
| 400 | `i915_vkc_dec_VkPipelineCacheHeaderVersionOne` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineCacheHeaderVersionOne |  |
| 416 | `i915_vkc_enc_VkPipelineCacheHeaderVersionOne` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPipelineCacheHeaderVersionOne |  |
| 428 | `i915_vkc_dec_VkApplicationInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkApplicationInfo |  |
| 444 | `i915_vkc_dec_VkFormatProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkFormatProperties |  |
| 456 | `i915_vkc_enc_VkFormatProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkFormatProperties |  |
| 465 | `i915_vkc_dec_VkImageFormatProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageFormatProperties |  |
| 479 | `i915_vkc_enc_VkImageFormatProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkImageFormatProperties |  |
| 490 | `i915_vkc_dec_VkInstanceCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkInstanceCreateInfo |  |
| 528 | `i915_vkc_dec_VkMemoryHeap` | found | data/vulkan-codec.inc:i915_vkc_dec_VkMemoryHeap |  |
| 539 | `i915_vkc_enc_VkMemoryHeap` | found | data/vulkan-codec.inc:i915_vkc_enc_VkMemoryHeap |  |
| 547 | `i915_vkc_dec_VkMemoryType` | found | data/vulkan-codec.inc:i915_vkc_dec_VkMemoryType |  |
| 558 | `i915_vkc_enc_VkMemoryType` | found | data/vulkan-codec.inc:i915_vkc_enc_VkMemoryType |  |
| 566 | `i915_vkc_dec_VkPhysicalDeviceFeatures` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPhysicalDeviceFeatures |  |
| 630 | `i915_vkc_enc_VkPhysicalDeviceFeatures` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPhysicalDeviceFeatures |  |
| 691 | `i915_vkc_dec_VkPhysicalDeviceLimits` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPhysicalDeviceLimits |  |
| 824 | `i915_vkc_enc_VkPhysicalDeviceLimits` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPhysicalDeviceLimits |  |
| 948 | `i915_vkc_dec_VkPhysicalDeviceMemoryProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPhysicalDeviceMemoryProperties |  |
| 967 | `i915_vkc_enc_VkPhysicalDeviceMemoryProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPhysicalDeviceMemoryProperties |  |
| 981 | `i915_vkc_dec_VkPhysicalDeviceSparseProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPhysicalDeviceSparseProperties |  |
| 995 | `i915_vkc_enc_VkPhysicalDeviceSparseProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPhysicalDeviceSparseProperties |  |
| 1006 | `i915_vkc_dec_VkPhysicalDeviceProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPhysicalDeviceProperties |  |
| 1028 | `i915_vkc_enc_VkPhysicalDeviceProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPhysicalDeviceProperties |  |
| 1045 | `i915_vkc_dec_VkQueueFamilyProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkQueueFamilyProperties |  |
| 1058 | `i915_vkc_enc_VkQueueFamilyProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkQueueFamilyProperties |  |
| 1068 | `i915_vkc_dec_VkDeviceQueueCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDeviceQueueCreateInfo |  |
| 1090 | `i915_vkc_dec_VkDeviceCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDeviceCreateInfo |  |
| 1137 | `i915_vkc_dec_VkExtensionProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkExtensionProperties |  |
| 1150 | `i915_vkc_enc_VkExtensionProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkExtensionProperties |  |
| 1159 | `i915_vkc_dec_VkLayerProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkLayerProperties |  |
| 1176 | `i915_vkc_enc_VkLayerProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkLayerProperties |  |
| 1188 | `i915_vkc_dec_VkSubmitInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSubmitInfo |  |
| 1234 | `i915_vkc_dec_VkMappedMemoryRange` | found | data/vulkan-codec.inc:i915_vkc_dec_VkMappedMemoryRange |  |
| 1248 | `i915_vkc_dec_VkMemoryAllocateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkMemoryAllocateInfo |  |
| 1261 | `i915_vkc_dec_VkMemoryRequirements` | found | data/vulkan-codec.inc:i915_vkc_dec_VkMemoryRequirements |  |
| 1273 | `i915_vkc_enc_VkMemoryRequirements` | found | data/vulkan-codec.inc:i915_vkc_enc_VkMemoryRequirements |  |
| 1282 | `i915_vkc_dec_VkSparseMemoryBind` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSparseMemoryBind |  |
| 1296 | `i915_vkc_dec_VkSparseBufferMemoryBindInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSparseBufferMemoryBindInfo |  |
| 1315 | `i915_vkc_dec_VkSparseImageOpaqueMemoryBindInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSparseImageOpaqueMemoryBindInfo |  |
| 1334 | `i915_vkc_dec_VkImageSubresource` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageSubresource |  |
| 1346 | `i915_vkc_enc_VkImageSubresource` | found | data/vulkan-codec.inc:i915_vkc_enc_VkImageSubresource |  |
| 1355 | `i915_vkc_dec_VkSparseImageMemoryBind` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSparseImageMemoryBind |  |
| 1370 | `i915_vkc_dec_VkSparseImageMemoryBindInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSparseImageMemoryBindInfo |  |
| 1389 | `i915_vkc_dec_VkBindSparseInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkBindSparseInfo |  |
| 1445 | `i915_vkc_dec_VkSparseImageFormatProperties` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSparseImageFormatProperties |  |
| 1457 | `i915_vkc_enc_VkSparseImageFormatProperties` | found | data/vulkan-codec.inc:i915_vkc_enc_VkSparseImageFormatProperties |  |
| 1466 | `i915_vkc_dec_VkSparseImageMemoryRequirements` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSparseImageMemoryRequirements |  |
| 1480 | `i915_vkc_enc_VkSparseImageMemoryRequirements` | found | data/vulkan-codec.inc:i915_vkc_enc_VkSparseImageMemoryRequirements |  |
| 1491 | `i915_vkc_dec_VkFenceCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkFenceCreateInfo |  |
| 1503 | `i915_vkc_dec_VkSemaphoreCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSemaphoreCreateInfo |  |
| 1515 | `i915_vkc_dec_VkEventCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkEventCreateInfo |  |
| 1527 | `i915_vkc_dec_VkQueryPoolCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkQueryPoolCreateInfo |  |
| 1542 | `i915_vkc_dec_VkBufferCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkBufferCreateInfo |  |
| 1566 | `i915_vkc_dec_VkBufferViewCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkBufferViewCreateInfo |  |
| 1582 | `i915_vkc_dec_VkImageCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageCreateInfo |  |
| 1613 | `i915_vkc_dec_VkSubresourceLayout` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSubresourceLayout |  |
| 1627 | `i915_vkc_enc_VkSubresourceLayout` | found | data/vulkan-codec.inc:i915_vkc_enc_VkSubresourceLayout |  |
| 1638 | `i915_vkc_dec_VkComponentMapping` | found | data/vulkan-codec.inc:i915_vkc_dec_VkComponentMapping |  |
| 1651 | `i915_vkc_enc_VkComponentMapping` | found | data/vulkan-codec.inc:i915_vkc_enc_VkComponentMapping |  |
| 1661 | `i915_vkc_dec_VkImageViewCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageViewCreateInfo |  |
| 1678 | `i915_vkc_dec_VkShaderModuleCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkShaderModuleCreateInfo |  |
| 1699 | `i915_vkc_dec_VkPipelineCacheCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineCacheCreateInfo |  |
| 1720 | `i915_vkc_dec_VkSpecializationMapEntry` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSpecializationMapEntry |  |
| 1732 | `i915_vkc_enc_VkSpecializationMapEntry` | found | data/vulkan-codec.inc:i915_vkc_enc_VkSpecializationMapEntry |  |
| 1741 | `i915_vkc_dec_VkSpecializationInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSpecializationInfo |  |
| 1768 | `i915_vkc_dec_VkPipelineShaderStageCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineShaderStageCreateInfo |  |
| 1791 | `i915_vkc_dec_VkComputePipelineCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkComputePipelineCreateInfo |  |
| 1807 | `i915_vkc_dec_VkVertexInputBindingDescription` | found | data/vulkan-codec.inc:i915_vkc_dec_VkVertexInputBindingDescription |  |
| 1819 | `i915_vkc_enc_VkVertexInputBindingDescription` | found | data/vulkan-codec.inc:i915_vkc_enc_VkVertexInputBindingDescription |  |
| 1828 | `i915_vkc_dec_VkVertexInputAttributeDescription` | found | data/vulkan-codec.inc:i915_vkc_dec_VkVertexInputAttributeDescription |  |
| 1841 | `i915_vkc_enc_VkVertexInputAttributeDescription` | found | data/vulkan-codec.inc:i915_vkc_enc_VkVertexInputAttributeDescription |  |
| 1851 | `i915_vkc_dec_VkPipelineVertexInputStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineVertexInputStateCreateInfo |  |
| 1881 | `i915_vkc_dec_VkPipelineInputAssemblyStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineInputAssemblyStateCreateInfo |  |
| 1895 | `i915_vkc_dec_VkPipelineTessellationStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineTessellationStateCreateInfo |  |
| 1908 | `i915_vkc_dec_VkViewport` | found | data/vulkan-codec.inc:i915_vkc_dec_VkViewport |  |
| 1923 | `i915_vkc_enc_VkViewport` | found | data/vulkan-codec.inc:i915_vkc_enc_VkViewport |  |
| 1935 | `i915_vkc_dec_VkPipelineViewportStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineViewportStateCreateInfo |  |
| 1965 | `i915_vkc_dec_VkPipelineRasterizationStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineRasterizationStateCreateInfo |  |
| 1987 | `i915_vkc_dec_VkPipelineMultisampleStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineMultisampleStateCreateInfo |  |
| 2012 | `i915_vkc_dec_VkStencilOpState` | found | data/vulkan-codec.inc:i915_vkc_dec_VkStencilOpState |  |
| 2028 | `i915_vkc_enc_VkStencilOpState` | found | data/vulkan-codec.inc:i915_vkc_enc_VkStencilOpState |  |
| 2041 | `i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo |  |
| 2062 | `i915_vkc_dec_VkPipelineColorBlendAttachmentState` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineColorBlendAttachmentState |  |
| 2079 | `i915_vkc_enc_VkPipelineColorBlendAttachmentState` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPipelineColorBlendAttachmentState |  |
| 2093 | `i915_vkc_dec_VkPipelineColorBlendStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineColorBlendStateCreateInfo |  |
| 2120 | `i915_vkc_dec_VkPipelineDynamicStateCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineDynamicStateCreateInfo |  |
| 2141 | `i915_vkc_dec_VkPushConstantRange` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPushConstantRange |  |
| 2153 | `i915_vkc_enc_VkPushConstantRange` | found | data/vulkan-codec.inc:i915_vkc_enc_VkPushConstantRange |  |
| 2162 | `i915_vkc_dec_VkPipelineLayoutCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkPipelineLayoutCreateInfo |  |
| 2192 | `i915_vkc_dec_VkSamplerCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSamplerCreateInfo |  |
| 2219 | `i915_vkc_dec_VkCopyDescriptorSet` | found | data/vulkan-codec.inc:i915_vkc_dec_VkCopyDescriptorSet |  |
| 2237 | `i915_vkc_dec_VkDescriptorBufferInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDescriptorBufferInfo |  |
| 2249 | `i915_vkc_dec_VkDescriptorPoolSize` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDescriptorPoolSize |  |
| 2260 | `i915_vkc_enc_VkDescriptorPoolSize` | found | data/vulkan-codec.inc:i915_vkc_enc_VkDescriptorPoolSize |  |
| 2268 | `i915_vkc_dec_VkDescriptorPoolCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDescriptorPoolCreateInfo |  |
| 2290 | `i915_vkc_dec_VkDescriptorSetAllocateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDescriptorSetAllocateInfo |  |
| 2311 | `i915_vkc_dec_VkDescriptorSetLayoutBinding` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDescriptorSetLayoutBinding |  |
| 2332 | `i915_vkc_dec_VkDescriptorSetLayoutCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkDescriptorSetLayoutCreateInfo |  |
| 2353 | `i915_vkc_dec_VkAttachmentDescription` | found | data/vulkan-codec.inc:i915_vkc_dec_VkAttachmentDescription |  |
| 2371 | `i915_vkc_enc_VkAttachmentDescription` | found | data/vulkan-codec.inc:i915_vkc_enc_VkAttachmentDescription |  |
| 2386 | `i915_vkc_dec_VkAttachmentReference` | found | data/vulkan-codec.inc:i915_vkc_dec_VkAttachmentReference |  |
| 2397 | `i915_vkc_enc_VkAttachmentReference` | found | data/vulkan-codec.inc:i915_vkc_enc_VkAttachmentReference |  |
| 2405 | `i915_vkc_dec_VkFramebufferCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkFramebufferCreateInfo |  |
| 2430 | `i915_vkc_dec_VkSubpassDescription` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSubpassDescription |  |
| 2484 | `i915_vkc_dec_VkSubpassDependency` | found | data/vulkan-codec.inc:i915_vkc_dec_VkSubpassDependency |  |
| 2500 | `i915_vkc_enc_VkSubpassDependency` | found | data/vulkan-codec.inc:i915_vkc_enc_VkSubpassDependency |  |
| 2513 | `i915_vkc_dec_VkRenderPassCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkRenderPassCreateInfo |  |
| 2552 | `i915_vkc_dec_VkCommandPoolCreateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkCommandPoolCreateInfo |  |
| 2565 | `i915_vkc_dec_VkCommandBufferAllocateInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkCommandBufferAllocateInfo |  |
| 2579 | `i915_vkc_dec_VkCommandBufferInheritanceInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkCommandBufferInheritanceInfo |  |
| 2596 | `i915_vkc_dec_VkCommandBufferBeginInfo` | found | data/vulkan-codec.inc:i915_vkc_dec_VkCommandBufferBeginInfo |  |
| 2616 | `i915_vkc_dec_VkBufferCopy` | found | data/vulkan-codec.inc:i915_vkc_dec_VkBufferCopy |  |
| 2628 | `i915_vkc_enc_VkBufferCopy` | found | data/vulkan-codec.inc:i915_vkc_enc_VkBufferCopy |  |
| 2637 | `i915_vkc_dec_VkImageSubresourceLayers` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageSubresourceLayers |  |
| 2650 | `i915_vkc_enc_VkImageSubresourceLayers` | found | data/vulkan-codec.inc:i915_vkc_enc_VkImageSubresourceLayers |  |
| 2660 | `i915_vkc_dec_VkBufferImageCopy` | found | data/vulkan-codec.inc:i915_vkc_dec_VkBufferImageCopy |  |
| 2675 | `i915_vkc_enc_VkBufferImageCopy` | found | data/vulkan-codec.inc:i915_vkc_enc_VkBufferImageCopy |  |
| 2687 | `i915_vkc_dec_VkClearDepthStencilValue` | found | data/vulkan-codec.inc:i915_vkc_dec_VkClearDepthStencilValue |  |
| 2698 | `i915_vkc_enc_VkClearDepthStencilValue` | found | data/vulkan-codec.inc:i915_vkc_enc_VkClearDepthStencilValue |  |
| 2706 | `i915_vkc_dec_VkClearRect` | found | data/vulkan-codec.inc:i915_vkc_dec_VkClearRect |  |
| 2718 | `i915_vkc_enc_VkClearRect` | found | data/vulkan-codec.inc:i915_vkc_enc_VkClearRect |  |
| 2727 | `i915_vkc_dec_VkImageBlit` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageBlit |  |
| 2746 | `i915_vkc_enc_VkImageBlit` | found | data/vulkan-codec.inc:i915_vkc_enc_VkImageBlit |  |
| 2760 | `i915_vkc_dec_VkImageCopy` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageCopy |  |
| 2774 | `i915_vkc_enc_VkImageCopy` | found | data/vulkan-codec.inc:i915_vkc_enc_VkImageCopy |  |
| 2785 | `i915_vkc_dec_VkImageResolve` | found | data/vulkan-codec.inc:i915_vkc_dec_VkImageResolve |  |
| 2799 | `i915_vkc_enc_VkImageResolve` | found | data/vulkan-codec.inc:i915_vkc_enc_VkImageResolve |  |

#### `vk/compile.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 103 | `i915_vk_compile` | found | compiler/compile.c:drv_i915_shader_compile |  |
| 205 | `i915_vk_shader_binary_free` | found | compiler/compile.c:drv_i915_shader_binary_free |  |
| 217 | `compile_sources` | found | compiler/compile.c:i915_compile_sources |  |
| 233 | `compile_grf` | found | compiler/compile.c:i915_compile_grf |  |
| 249 | `compile_define` | found | compiler/compile.c:i915_compile_define |  |
| 288 | `compile_release` | found | compiler/compile.c:i915_compile_release |  |
| 313 | `compile_instruction` | found | compiler/compile.c:i915_compile_instruction |  |
| 447 | `compile_rank` | found | compiler/compile.c:i915_compile_rank |  |
| 465 | `compile_note` | found | compiler/compile.c:i915_compile_note |  |
| 491 | `compile_interface` | found | compiler/compile.c:i915_compile_interface |  |
| 516 | `compile_prologue` | found | compiler/compile.c:i915_compile_prologue |  |
| 543 | `compile_terminate` | found | compiler/compile.c:i915_compile_terminate |  |

#### `vk/display.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 25 | `i915_vk_display_init` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): display/display.c:drv_i915_display_init_noirq |
| 37 | `i915_vk_display_mode` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): display/display.c:i915_display_mode |
| 51 | `i915_vk_display_flip` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 62 | `i915_vk_display_fini` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): display/display.c:drv_i915_display_fini |

#### `vk/eu.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 53 | `i915_vk_eu_init` | found | compiler/eu.c:drv_i915_eu_init |  |
| 65 | `i915_vk_eu_free` | found | compiler/eu.c:drv_i915_eu_free |  |
| 77 | `i915_vk_eu_data` | found | compiler/eu.c:drv_i915_eu_data |  |
| 87 | `i915_vk_eu_grf_typed` | found | compiler/eu.c:i915_eu_grf_typed |  |
| 105 | `i915_vk_eu_grf` | found | compiler/eu.c:drv_i915_eu_grf |  |
| 113 | `i915_vk_eu_grf_ud` | found | compiler/eu.c:drv_i915_eu_grf_ud |  |
| 121 | `i915_vk_eu_grf_scalar` | found | compiler/eu.c:drv_i915_eu_grf_scalar |  |
| 140 | `i915_vk_eu_negate` | found | compiler/eu.c:drv_i915_eu_negate |  |
| 149 | `i915_vk_eu_imm_f` | found | compiler/eu.c:drv_i915_eu_imm_f |  |
| 163 | `i915_vk_eu_imm_d` | found | compiler/eu.c:drv_i915_eu_imm_d |  |
| 177 | `i915_vk_eu_null` | found | compiler/eu.c:drv_i915_eu_null |  |
| 189 | `i915_vk_eu_mov` | found | compiler/eu.c:drv_i915_eu_mov |  |
| 207 | `i915_vk_eu_alu2` | found | compiler/eu.c:drv_i915_eu_alu2 |  |
| 245 | `i915_vk_eu_mad` | found | compiler/eu.c:drv_i915_eu_mad |  |
| 265 | `i915_vk_eu_math` | found | compiler/eu.c:drv_i915_eu_math |  |
| 312 | `i915_vk_eu_send` | found | compiler/eu.c:drv_i915_eu_send |  |
| 365 | `i915_vk_eu_nop` | found | compiler/eu.c:drv_i915_eu_nop |  |
| 379 | `i915_vk_eu_reserve` | found | compiler/eu.c:i915_eu_reserve |  |
| 420 | `i915_vk_eu_common` | found | compiler/eu.c:i915_eu_common |  |
| 444 | `i915_vk_eu_sync` | found | compiler/eu.c:i915_eu_sync |  |
| 462 | `i915_vk_eu_dst` | found | compiler/eu.c:i915_eu_dst |  |
| 475 | `i915_vk_eu_src0` | found | compiler/eu.c:i915_eu_src0 |  |
| 499 | `i915_vk_eu_src1` | found | compiler/eu.c:i915_eu_src1 |  |
| 522 | `i915_vk_eu_set` | found | compiler/eu.c:i915_eu_set |  |
| 537 | `i915_vk_eu_bit` | found | compiler/eu.c:i915_eu_bit |  |

#### `vk/gfx-draw.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 97 | `emit` | found | render/batch.c:drv_i915_batch_emit |  |
| 107 | `emit_zero` | found | render/batch.c:drv_i915_batch_zero |  |
| 117 | `emit_words` | found | render/batch.c:drv_i915_batch_words |  |
| 126 | `emit_pointer` | found | render/batch.c:drv_i915_batch_pointer |  |
| 134 | `emit_pc` | found | render/batch.c:drv_i915_batch_pipe_control |  |
| 148 | `sf_half` | found | render/math.c:drv_i915_float_half |  |
| 157 | `sf_add` | found | render/math.c:drv_i915_float_add |  |
| 201 | `sf_sub` | found | render/math.c:drv_i915_float_sub |  |
| 212 | `gfx_object` | found | render/draw.c:i915_draw_object_create | match by substr |
| 232 | `gfx_session` | found | render/draw.c:drv_i915_gfx_session_get | match by substr |
| 252 | `i915_vk_gfx_session_close` | found | render/draw.c:drv_i915_gfx_session_close |  |
| 292 | `gfx_fnv1a` | test | T4a | S5 T4a (destination not present yet) |
| 306 | `gfx_compile_stage` | found | render/pipeline-prepare.c:i915_pipeline_compile_stage (function containing the matched text) | match by string |
| 329 | `i915_vk_gfx_pipeline_prepare` | found | render/pipeline-prepare.c:drv_i915_gfx_pipeline_prepare |  |
| 382 | `i915_vk_gfx_pipeline_release` | found | render/pipeline-prepare.c:drv_i915_gfx_pipeline_release |  |
| 392 | `gfx_kernels` | found | render/pipeline-prepare.c:i915_pipeline_kernels_fit | match by substr |
| 429 | `gfx_surface_format` | found | render/state.c:i915_surface_format | match by stem |
| 444 | `gfx_format_components` | found | render/state.c:i915_format_components | match by stem |
| 460 | `gfx_write_rss` | found | render/state.c:i915_image_surface_write |  |
| 483 | `gfx_write_sampler` | found | render/state.c:drv_i915_gfx_sampler_write | match by tokens |
| 507 | `gfx_write_state` | found | render/state.c:drv_i915_gfx_write_state |  |
| 599 | `emit_sba` | found | render/state.c:drv_i915_gfx_emit_context_setup (STATE_BASE_ADDRESS, L331) |  |
| 632 | `emit_vertex_input` | found | render/state.c:drv_i915_gfx_emit_vertex_input |  |
| 706 | `emit_urb` | found | render/state.c:drv_i915_gfx_emit_urb |  |
| 729 | `emit_constants` | found | render/state.c:drv_i915_gfx_emit_constants |  |
| 756 | `emit_raster` | found | render/state.c:drv_i915_gfx_emit_raster |  |
| 785 | `emit_depth` | found | render/state.c:drv_i915_gfx_emit_depth |  |
| 839 | `emit_shader_state` | found | render/state.c:drv_i915_gfx_emit_vertex_shader + drv_i915_gfx_emit_pixel_shader |  |
| 890 | `gfx_build_batch` | found | render/blit.c:i915_blit_build_batch | match by substr |
| 999 | `gfx_census` | test | T4a | test-only [s3-reports](i915-rebuild-s3-reports.md) L25 |
| 1063 | `i915_vk_gfx_draw` | found | render/draw.c:drv_i915_gfx_draw |  |
| 1143 | `gfx_rect_compile` | found | render/blit.c:i915_blit_compile |  |
| 1195 | `i915_vk_gfx_rect_prepare` | found | render/blit.c:drv_i915_gfx_rect_prepare |  |
| 1220 | `sf_from_u32` | found | render/math.c:drv_i915_float_from_u32 |  |
| 1236 | `sf_ratio` | found | render/math.c:drv_i915_float_ratio |  |
| 1257 | `gfx_write_surface` | found | render/state.c:i915_state_write_surfaces | match by substr |
| 1279 | `i915_vk_gfx_rect_build` | found | render/blit.c:drv_i915_gfx_rect_build |  |
| 1497 | `i915_vk_gfx_rect` | found | render/blit.c:drv_i915_gfx_rect |  |

#### `vk/gfx-obj.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 36 | `gfx_result` | found | render/reply.c:drv_i915_gfx_result |  |
| 49 | `gfx_create_tail` | found | render/reply.c:drv_i915_gfx_create_tail |  |
| 58 | `gfx_create_reply` | found | render/reply.c:drv_i915_gfx_create_reply |  |
| 81 | `gfx_destroy_plain` | found | render/objects.c:i915_gfx_destroy_plain |  |
| 104 | `i915_vk_gfx_memory_cpu` | found | render/memory.c:drv_i915_gfx_memory_cpu |  |
| 113 | `i915_vk_gfx_memory_va` | found | render/memory.c:drv_i915_gfx_memory_va |  |
| 125 | `drv_i915_vk_blob_attach` | found | render/memory.c:drv_i915_render_blob_attach |  |
| 142 | `drv_i915_vk_blob_detach` | found | render/memory.c:drv_i915_render_blob_detach |  |
| 156 | `gfx_allocate_memory` | found | render/memory.c:drv_i915_gfx_allocate_memory |  |
| 199 | `gfx_free_memory` | found | render/memory.c:drv_i915_gfx_free_memory |  |
| 227 | `gfx_bind` | found | render/memory.c:drv_i915_gfx_bind |  |
| 267 | `gfx_requirements` | found | render/memory.c:drv_i915_gfx_requirements |  |
| 305 | `gfx_create_buffer` | found | render/memory.c:drv_i915_gfx_create_buffer |  |
| 329 | `gfx_format_bytes` | found | render/image.c:i915_gfx_format_bytes |  |
| 342 | `gfx_create_image` | found | render/image.c:drv_i915_gfx_create_image |  |
| 395 | `gfx_create_image_view` | found | render/image.c:drv_i915_gfx_create_image_view |  |
| 423 | `gfx_create_sampler` | found | render/image.c:drv_i915_gfx_create_sampler |  |
| 452 | `gfx_subresource_layout` | found | render/image.c:drv_i915_gfx_subresource_layout |  |
| 480 | `gfx_create_dsl` | found | render/descriptor.c:drv_i915_gfx_create_dsl |  |
| 515 | `gfx_create_dpool` | found | render/descriptor.c:drv_i915_gfx_create_dpool |  |
| 540 | `gfx_allocate_dsets` | found | render/descriptor.c:drv_i915_gfx_allocate_dsets |  |
| 591 | `gfx_update_dsets` | found | render/descriptor.c:drv_i915_gfx_update_dsets |  |
| 661 | `gfx_create_pipeline_layout` | found | render/pipeline.c:drv_i915_gfx_create_pipeline_layout |  |
| 684 | `gfx_create_render_pass` | found | render/render-pass.c:drv_i915_gfx_create_render_pass |  |
| 729 | `gfx_create_framebuffer` | found | render/render-pass.c:drv_i915_gfx_create_framebuffer |  |
| 768 | `gfx_create_shader` | found | render/pipeline.c:drv_i915_gfx_create_shader |  |
| 794 | `gfx_float_bits` | found | render/pipeline.c:i915_gfx_float_bits |  |
| 805 | `gfx_decode_pipeline` | found | render/pipeline.c:i915_gfx_decode_pipeline |  |
| 930 | `gfx_create_pipelines` | found | render/pipeline.c:drv_i915_gfx_create_pipelines |  |
| 994 | `gfx_destroy_pipeline` | found | render/pipeline.c:drv_i915_gfx_destroy_pipeline |  |
| 1022 | `gfx_create_semaphore` | found | render/sync.c:drv_i915_gfx_create_semaphore |  |
| 1043 | `i915_vk_gfx_obj_dispatch` | found | render/objects.c:drv_i915_gfx_obj_dispatch |  |

#### `vk/gfx-rec.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 52 | `gfx_result` | found | render/reply.c:drv_i915_gfx_result |  |
| 66 | `rec_create_pool` | found | render/command.c:i915_command_pool_create | match by tokens |
| 96 | `rec_free_buffer` | found | render/command.c:i915_command_buffer_release |  |
| 111 | `rec_destroy_pool` | found | render/command.c:i915_command_pool_destroy | match by tokens |
| 134 | `rec_reset_pool` | found | render/command.c:i915_command_pool_reset | match by tokens |
| 157 | `rec_allocate` | found | render/command.c:i915_command_buffers_allocate (function containing the matched text) | match by string |
| 214 | `rec_free` | found | render/command.c:i915_command_buffers_free | match by tokens |
| 236 | `rec_begin` | found | render/command.c:i915_command_buffer_begin | match by tokens |
| 258 | `rec_end` | found | render/command.c:i915_command_buffer_end (function containing the matched text) | match by string |
| 277 | `rec_op` | found | render/command.c:i915_command_op | match by tokens |
| 298 | `rec_image_copy` | found | render/command.c:i915_record_image_copy | match by tokens |
| 338 | `rec_clear_image` | found | render/command.c:i915_record_clear_image | match by tokens |
| 366 | `rec_barrier` | found | render/command.c:i915_record_barrier | match by tokens |
| 395 | `rec_copy` | found | render/command.c:i915_record_buffer_image_copy |  |
| 436 | `rec_begin_pass` | found | render/command.c:i915_record_begin_pass | match by tokens |
| 485 | `rec_bind_vertex` | found | render/command.c:i915_record_bind_vertex | match by tokens |
| 510 | `rec_bind_dsets` | found | render/command.c:i915_record_bind_descriptor_sets |  |
| 539 | `rec_push` | found | render/command.c:i915_record_push_constants | match by tokens |
| 558 | `rec_command` | found | render/command.c:i915_record_command |  |
| 613 | `image_surface` | found | render/command.c:i915_image_surface |  |
| 625 | `exec_clear` | found | render/command.c:i915_execute_clear |  |
| 674 | `exec_copy` | found | render/command.c:i915_execute_buffer_image_copy (function containing the matched text) | match by string |
| 723 | `exec_image` | found | render/command.c:i915_execute_clear_image / i915_execute_image_copy / i915_execute_image_blit |  |
| 780 | `exec_cmdbuf` | found | render/command.c:i915_command_buffer_execute (function containing the matched text) | match by string |
| 846 | `rec_submit` | found | render/command.c:i915_queue_submit | match by tokens |
| 913 | `i915_vk_gfx_rec_dispatch` | found | render/command.c:drv_i915_gfx_rec_dispatch |  |

#### `vk/inst.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 36 | `set_float` | found | render/instance.c:i915_instance_set_float | match by substr |
| 51 | `inst_create_instance` | found | render/instance.c:i915_instance_create | match by tokens |
| 83 | `inst_enumerate_physical_devices` | found | render/instance.c:i915_instance_enumerate_physical_devices | match by tokens |
| 117 | `inst_properties` | found | render/instance.c:i915_instance_properties (function containing the matched text) | match by string |
| 246 | `inst_features` | found | render/instance.c:i915_instance_features | match by tokens |
| 269 | `inst_memory_properties` | found | render/instance.c:i915_instance_memory_properties | match by tokens |
| 304 | `inst_queue_families` | found | render/instance.c:i915_instance_queue_families | match by tokens |
| 338 | `inst_format_features` | found | render/instance.c:i915_instance_format_features | match by tokens |
| 364 | `inst_format_properties` | found | render/instance.c:i915_instance_format_properties | match by tokens |
| 385 | `inst_image_format_properties` | found | render/instance.c:i915_instance_image_format_properties | match by tokens |
| 428 | `inst_create_device` | found | render/instance.c:i915_instance_create_device | match by tokens |
| 465 | `inst_get_device_queue2` | found | render/instance.c:i915_instance_get_device_queue2 | match by tokens |
| 495 | `inst_destroy` | found | render/instance.c:i915_instance_destroy | match by tokens |
| 513 | `inst_wait_idle` | found | render/instance.c:i915_instance_wait_idle | match by tokens |
| 536 | `inst_execute_streams` | found | render/transport.c:i915_transport_execute_streams | match by tokens |
| 590 | `i915_vk_inst_dispatch` | found | render/instance.c:drv_i915_render_instance_dispatch |  |

#### `vk/pipe.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 65 | `i915_vk_pipe_result` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 84 | `i915_vk_pipe_create_shader_module` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 157 | `i915_vk_pipe_destroy_shader_module` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 187 | `i915_vk_pipe_skip_string` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 205 | `i915_vk_pipe_decode_stage` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 223 | `i915_vk_pipe_skip_words` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 235 | `i915_vk_pipe_skip_array` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 249 | `i915_vk_pipe_place_shader` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 302 | `i915_vk_pipe_release_code` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 320 | `i915_vk_pipe_decode_graphics` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 460 | `i915_vk_pipe_create_graphics` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 537 | `i915_vk_pipe_destroy_pipeline` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 563 | `i915_vk_pipe_dispatch` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 592 | `i915_vk_pipeline_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/pipeline.c:drv_i915_gfx_create_pipeline_layout |
| 621 | `i915_vk_pipeline_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/pipeline.c:drv_i915_gfx_destroy_pipeline |
| 635 | `i915_vk_pipeline_emit` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 656 | `i915_vk_pipe_emit_base` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 672 | `i915_vk_batch_emit` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/batch.c:drv_i915_batch_emit |
| 690 | `i915_vk_batch_pad` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |

#### `vk/res.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 101 | `i915_vk_memory_alloc` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 152 | `i915_vk_memory_free` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/memory.c:drv_i915_gfx_free_memory |
| 172 | `i915_vk_memory_map` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 186 | `i915_vk_buffer_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/memory.c:drv_i915_gfx_create_buffer |
| 211 | `i915_vk_buffer_bind` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/memory.c:i915_gfx_bind_buffer |
| 223 | `i915_vk_buffer_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 232 | `i915_vk_image_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/image.c:drv_i915_gfx_create_image |
| 261 | `i915_vk_image_bind` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 280 | `i915_vk_image_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 289 | `i915_vk_image_view_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/image.c:drv_i915_gfx_create_image_view |
| 319 | `i915_vk_image_view_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 328 | `i915_vk_image_surface_state` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 342 | `i915_vk_surface_state` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 370 | `i915_vk_sampler_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/image.c:drv_i915_gfx_create_sampler |
| 398 | `i915_vk_sampler_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 407 | `i915_vk_sampler_state` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 415 | `i915_vk_dsl_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/descriptor.c:drv_i915_gfx_create_dsl |
| 449 | `i915_vk_dsl_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 458 | `i915_vk_dpool_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/descriptor.c:drv_i915_gfx_create_dpool |
| 479 | `i915_vk_dpool_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 488 | `i915_vk_dset_alloc` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 513 | `i915_vk_dset_free` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 526 | `i915_vk_dset_update` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 548 | `i915_vk_result` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 561 | `i915_vk_res_free_memory_obj` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 567 | `i915_vk_res_destroy_buffer_obj` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 573 | `i915_vk_res_destroy_image_obj` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 580 | `i915_vk_res_bind_buffer_obj` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 586 | `i915_vk_res_bind_image_obj` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 593 | `i915_vk_res_skip_extension` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 610 | `i915_vk_res_allocate_memory` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 663 | `i915_vk_res_create_buffer` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 722 | `i915_vk_res_create_image` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 794 | `i915_vk_res_bind` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 832 | `i915_vk_res_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 862 | `i915_vk_res_dispatch` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |

#### `vk/spirv.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 199 | `i915_vk_spirv_parse` | found | compiler/spirv.c:drv_i915_shader_parse |  |
| 209 | `i915_vk_spirv_parse_diag` | found | compiler/spirv.c:drv_i915_shader_parse |  |
| 292 | `i915_vk_spirv_free` | found | compiler/spirv.c:drv_i915_shader_ir_free |  |
| 311 | `spirv_refuse` | found | compiler/spirv.c:i915_spirv_refuse |  |
| 325 | `spirv_id` | found | compiler/spirv.c:i915_spirv_id |  |
| 334 | `spirv_float_components` | found | compiler/spirv.c:i915_spirv_float_components |  |
| 355 | `spirv_pass_declarations` | found | compiler/spirv.c:i915_spirv_pass_declarations |  |
| 565 | `spirv_add_io` | found | compiler/spirv.c:i915_spirv_add_io |  |
| 588 | `spirv_add_uniform` | found | compiler/spirv.c:i915_spirv_add_uniform |  |
| 605 | `spirv_emit` | found | compiler/spirv.c:i915_spirv_emit |  |
| 630 | `spirv_new_value` | found | compiler/spirv.c:i915_spirv_new_value |  |
| 642 | `spirv_operand` | found | compiler/spirv.c:i915_spirv_operand |  |
| 675 | `spirv_result` | found | compiler/spirv.c:i915_spirv_result |  |
| 697 | `spirv_pass_body` | found | compiler/spirv.c:i915_spirv_pass_body |  |

#### `vk/sync.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 52 | `i915_vk_fence_create` | found | render/fence.c:i915_fence_create | fence part ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 73 | `i915_vk_fence_destroy` | found | render/fence.c:i915_fence_destroy | fence part ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 82 | `i915_vk_fence_reset` | found | render/fence.c:i915_fence_reset | fence part ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 92 | `i915_vk_fence_signal` | found | render/fence.c:drv_i915_fence_signal | fence part ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 101 | `i915_vk_fence_arm` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 116 | `i915_vk_fence_status` | found | render/fence.c:i915_fence_status | fence part ported [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 138 | `i915_vk_fence_wait` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 170 | `i915_vk_fence_ready_locked` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 201 | `i915_vk_sync_create_fence` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 248 | `i915_vk_sync_destroy_fence` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 277 | `i915_vk_sync_reset_fences` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 311 | `i915_vk_sync_fence_status` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 337 | `i915_vk_sync_dispatch` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 367 | `i915_vk_semaphore_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51; nearest new-path equivalent (not a port): render/sync.c:drv_i915_gfx_create_semaphore |
| 386 | `i915_vk_semaphore_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 395 | `i915_vk_query_pool_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 418 | `i915_vk_query_pool_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |

#### `vk/vk.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 34 | `drv_i915_vk_attach` | found | render/vulkan.c:drv_i915_render_attach |  |
| 68 | `drv_i915_vk_detach` | found | render/vulkan.c:drv_i915_render_detach |  |
| 81 | `drv_i915_vk_open` | found | render/vulkan.c:drv_i915_render_open |  |
| 114 | `drv_i915_vk_close` | found | render/vulkan.c:drv_i915_render_close |  |
| 128 | `drv_i915_vk_command` | found | render/vulkan.c:drv_i915_render_execute |  |
| 179 | `i915_vk_capset_fill` | found | render/vulkan.c:i915_render_capset_fill |  |
| 210 | `i915_vk_errno` | found | render/vulkan.c:drv_i915_render_errno | match by substr |

#### `vk/vkc.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 16 | `i915_vkc_array` | found | render/codec.c:i915_vkc_array |  |
| 52 | `i915_vkc_read_string` | found | render/codec.c:i915_vkc_read_string |  |
| 78 | `i915_vkc_read_bytes` | found | render/codec.c:i915_vkc_read_bytes |  |
| 106 | `i915_vkc_read_float` | found | render/codec.c:i915_vkc_read_float |  |
| 118 | `i915_vkc_skip_external_chain` | found | render/codec.c:i915_vkc_skip_external_chain |  |
| 135 | `i915_vkc_reply_bytes` | found | render/codec.c:i915_vkc_reply_bytes |  |
| 151 | `i915_vkc_reply_float` | found | render/codec.c:i915_vkc_reply_float |  |

#### `vk/wsi.c`

| 行 | 旧関数 | 判定 | 新（file:function）／担当 | 注 |
| ---: | --- | --- | --- | --- |
| 47 | `i915_vk_wsi_dispatch` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 63 | `i915_vk_swapchain_create` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 116 | `i915_vk_swapchain_destroy` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 138 | `i915_vk_swapchain_acquire` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |
| 154 | `i915_vk_swapchain_present` | unported |  | old module not ported, unreachable from vkdemo [s3-reports](i915-rebuild-s3-reports.md) L17; [plan §4](i915-rebuild-plan.md) L51 |

## 4. レビュー §4 の非関数資材の行き先

[レビュー §4](i915-refactoring-review.md) の表の各行（L104-121）。場所は `src/drivers/gpu/i915/` 相対。

| レビュー §4 の資材 | 行き先（確認した場所） | 状態・注 |
| --- | --- | --- |
| i915.c の PCI ID 表、ops/sub-ops、capability、条件付き compile | ID 表: `data/i915-ids.inc`（`i915.c`・`device.c` が include）。ops: `i915.c`（publish/unpublish と render・display・scanout の bind）、sub-ops: `display/scanout.c`・`display/internal.h`・`display/present.c`。条件付き compile は `PARITY_RESIDENT(_DISPLAY)` を常に真として除去（[規則 §4](i915-rebuild-rules.md) L127） | 移動済み。設計 A10 の device 所有 `i915_ops_storage` は未導入（移動後の機能変更段階の項目） |
| runner.c の g_runner、readiness、device 保持、起動 lock | `device.c`: `struct i915_start_registry`（L74-94）、`drv_i915_runtime_ready`、`drv_i915_device_schedule_start`、`i915_start_launch`、`i915_start_worker`、`i915_start_registry_init` | 移動済み。試験結果の集計（`probe_status_name` ほか）は T4a |
| probe.c の依存順、rctx/rlcd、forcewake の取得順と逆順解放 | `device.c`: `i915_start_*` 段、`i915_forcewake_get_all`/`put_all`。`rctx`/`rlcd` は `display/internal.h` の `struct i915_display`（`display/display.c`、`present.c` が使う） | 移動済み（S4 で実機 E-130 再現） |
| legacy_shim.c の contexts、run queue、sync queue、work、sync_done、stop | `worker.c`（`i915_worker_loop`、`drv_i915_worker_run_sync`、`sync_done`、`i915_worker_find`、`drv_i915_worker_context_create/destroy`、`drv_i915_worker_kick`） | 移動済み |
| shim の map_vm/map_va[2]/map_pages[2]、display_up/failed | `display/internal.h` の `struct i915_present_window`（`window.map_*`、`display_up`）。map/unmap は `display/scanout.c: drv_i915_scanout_map_panel/unmap_panel`、使用は `display/present.c` | 移動済み |
| rd の mutex/owner/lease/next_lease/sequence/present_tick/active | `display/internal.h` の `struct i915_resident_display`（`display->rd`）。操作は `display/display.c`（query/mode/claim/close）、`display/present.c`（present/wait/release）、`display/scanout.c`（device_query/constraints） | 移動済み |
| ms_pool/ms_ops_pool/ms_retained_pool/ms_retained_ops_pool/ms_sel | `display/modeset-internal.h` の `struct i915_lcd_world`（`display->lcd_world`）。使用は `display/modeset.c`、`vblank.c`、`panel-backlight.c` | 移動済み（file-scope から world へ。[s4 §2.4](i915-rebuild-s4.md) L165） |
| parity_lcd_cur_i915 / parity_lcd_wm などの glue 選択状態 | `i915_lcd_world.i915_lcd_cur_i915`、`struct i915_wm_world`（`display/watermark-internal.h`）、明示引数化（[s4 §2.4](i915-rebuild-s4.md) L136-149） | 移動済み。ただし file-scope の「bound world」ポインタが 5 か所残る（`i915_dp_current_world`、`i915_vbt_bound_world`、`i915_opregion_bound_display`、`i915_state_sink_world`、lcd_display_ver 用。[s4-reports](i915-rebuild-s4-reports.md) L14、L21、L32、L74、いずれも XXX） |
| lk / lcdb_locks / lcdb_scanout と resident buffer | `display/internal.h` の `struct i915_display`: `lk`、`lcdb_locks`、`resident_buf[2]` ほか（[s4 §3](i915-rebuild-s4.md) L239）。`lcdb_scanout`・`lcdb_summary` は試験（L247、T4b） | 移動済み（本番部分） |
| i915_vk_object_entry/table、gfx_memories、inst_token | `render/object.c`（`drv_i915_object_insert/lookup/remove`、render device ごと）、`render/memory.c`（live memory list）、`render/instance.c`（token） | 移動済み。memory list は file-scope static のまま（[s3-reports](i915-rebuild-s3-reports.md) L5 XXX）、object 表は render device 単位のまま（A05 は後段、L21） |
| gfx_session/gfx_batch/gfx_kernels、GFX_* heap 配置 | `render/gfx.h`（`struct i915_gfx_*`、`I915_GFX_*`）、`render/heap.h`、`render/state.c`、`render/batch.c`、`render/draw.c` | 移動済み（S3c で state/batch が旧とバイト一致 12 例、[s3-reports](i915-rebuild-s3-reports.md) L24） |
| gfx_rect_fill_kernel / copy_kernel | `render/blit.c` | 移動済み。cache は file-scope static のまま（[s3-reports](i915-rebuild-s3-reports.md) L26 XXX）。設計の device/target 所有は未実施 |
| gfx_eot_only | `render/state.c:52` の `i915_gfx_eot_only`（L287-288 で instruction heap を埋める） | 移動済み。レビュー案の `data/render-eot.inc` ではなく state.c 内 |
| shader_binary の payload/URB/push/input metadata | `compiler/compiler.h` の `struct i915_shader_binary`（`dispatch_grf_start`、`push_regs`、`input_count`、`input_locations` ほか） | 移動済み（S2 で旧と出力一致 42 万行） |
| DPLL/CDCLK/PHY/WA/MOCS/PCI/EDID 表、register macro、firmware bytes | `data/display-clock-tables.inc`、`data/display-phy-buf-trans.inc`、`data/i915-gt-workarounds.inc`、`data/i915-mocs.inc`・`i915-gt-mocs-table.inc`、`data/i915-ids.inc`、`data/display-*.inc`（65 本、1 対 1）、`data/firmware/*.c`（4 本、本監査でバイト列一致を確認）、`data/forcewake-ranges.inc`（数値一致）、`data/i915-lrc-offsets.inc`（旧 parity 表とコンパイル結果がバイト一致） | 移動済み。MOCS entry 63 の Linux との差は XXX のまま（[s1-reports](i915-rebuild-s1-reports.md) L46） |
| callback initializer の関数アドレス、inline、iterator/lock macro | 環境ヘッダ `display/modeset-internal.h`、`watermark-internal.h`、`takeover-internal.h`、`dp-internal.h`、`hotplug-internal.h`、`opregion-internal.h`、`vbt.h` に明示名で展開（[s4 §1.2・§2.4](i915-rebuild-s4.md) L37-62、L126-227） | 移動済み（layout 統一と Linux 名への復帰は後段、s4 §9 L758-765） |
| generated codec と vkc arena、生成 shader fixture | codec: `data/vulkan-codec.inc`（生成器 `handover/tools/gen_vk_server_codec.py` は新パスへ更新済み、L10・L265）、arena: `render/internal.h`・`render/codec.c`。参照 shader `vk/vkref-generated.inc` は試験（T4a、未移動） | codec は移動済み。fixture は S5 |
| copyright/SPDX/出典 SHA/抽出 manifest | Linux 由来 display の MIT notice は復元済み（[s4-reports](i915-rebuild-s4-reports.md) L79）、`data/display-*.inc` に抽出元と sha256 を保持 | **manifest が未移動**: `parity/lcd/port_lcd_calc.manifest.json` は [s4 §9-6](i915-rebuild-s4.md) L763 で「data/provenance に残す」とされたが、新ツリーに `data/provenance` がない。`plan/ws031/license-inventory.md`（183 か所）と `provenance-ledger.md`（5 か所）は旧パスのまま。`data/display-acpi-display.inc` の GPL-2.0 由来はユーザー判断待ち（[s4-reports](i915-rebuild-s4-reports.md) L23） |

## 5. ツリー外から旧パスを指す参照

`grep -rn "i915-old\|drivers/gpu/i915/parity\|drivers/gpu/i915/vk/\|drivers/gpu/i915/linux/"`（build/、.git、旧ツリー自身を除く）に、
関連する生成器が `~/zedBSD/src/drivers/gpu/i915/parity` 形式で持つパスを加えた結果。build 定義（`platform/amd64/vmunix.mk`）、`include/`、`src/kern/` には旧パスの参照はない（`include/drivers/i915-parity.h` は削除済み）。

### 5.1 削除前に直す（今も旧ファイルを読む・書くもの）

| 参照元 | 内容 | 担当 |
| --- | --- | --- |
| `plan/ws031/tests/i915-vk-res-test.c:24-25`、`i915-vk-resdispatch-test.c:28-29`、`i915-vk-pipe-test.c:27-31`、`i915-vk-cmdbuf-test.c:27-33`、`i915-vk-sync-test.c:24-25` | `#include "../../../src/drivers/gpu/i915/vk/*.c"`。退避後のため**現在すでに壊れている**（`i915/vk/` はない） | S5 T1（新関数へ移すか廃止理由を記録） |
| `plan/ws031/tests/run-vk-analyzer.sh:14` | `src/drivers/gpu/i915/vk/$name.c` を解析 | S5 T1 |
| `plan/ws029/tests/i915-fixture.inc:26` | `#include ".../src/drivers/gpu/i915/linux/i915-commands.inc"`（新は `data/i915-commands.inc`） | S5 T1（ws029） |
| `plan/ws031/tests/run-dp-host-test.sh:4-5`、`run-lcd-host-test.sh:5`、`run-lcd-modeset-host-test.sh:6-8`、`run-opregion-host-test.sh:6`、`run-native-decide-host-test.sh:6` | `src/drivers/gpu/i915/parity/{dp,lcd,vbt}` と `parity/*.c` を compile | S5 T3（`tests/display/host-*.c` が作られ始めている） |
| `plan/ws031/handover/tools/gen_fw_ranges.py:14-16` | 出力 `…/i915/parity/gt_fw_ranges.inc`、入力 `parity/backend_mmio.c` | 生成器を `data/forcewake-ranges.inc` へ追従させるか、廃止を記録 |
| `plan/ws031/handover/tools/gen_lrc_offsets.py:12` | 出力 `…/i915/parity/gt_lrc_offsets.inc`（新は `data/i915-lrc-offsets.inc`） | 同上 |
| `plan/ws031/handover/tools/check_generated.sh:8`、`notice_map.py:6`、`engine_sseu.py:6,32,67`、`vk_opcode_survey.py:7` | `src/drivers/gpu/i915/parity` / `…/vk` を読む点検・調査ツール | 新パスへ追従か、履歴扱いと明記 |
| `plan/ws031/handover/tools/port_lcd_calc.py`、`port_dp_aux_pps.py`、`port_intel_bios.py` | 旧 glue・旧パスへ生成 | [s4 §9-6](i915-rebuild-s4.md) L763 で廃止決定済み。ファイルに廃止を明記するか削除 |
| `plan/ws031/license-inventory.md`（183 行）、`plan/ws031/provenance-ledger.md` L19・L75・L101・L122・L178 | 現行のライセンス・出典台帳が旧パスで記述 | 新パスで再生成（`handover/tools/license_inventory.py` は `src/drivers/gpu/i915` を走査するので再実行で追従） |
| `plan/ws031/handover/README.md` L22、L66、L125、L204-222（§7 コード地図） | 現行の引継ぎ文書が `parity/` を現在の配置として説明 | 新構成の地図へ更新 |
| `AGENTS.md:392`、`plan/AGENTS.md:138`、`plan/master.md:1795`、`plan/queue.md:78` | `src/drivers/gpu/i915/linux/i915-regs.inc` などを現在の配置として記載（新は `data/`） | 文書の追従 |
| `userland/base/tests/gpu-i915/main.c:32` | コメントが `src/drivers/gpu/i915/linux/i915-commands.inc` を指す | コメント修正（`data/i915-commands.inc`） |

### 5.2 直さなくてよい（履歴・出典の記述）

- `plan/ws031/handover/increment-results/*.patch|*.diff|report-*.md`、`handover/expert-reports/*`、`handover/tools/lcd-e1xx/*.py`・`vk-e110/*.py`（一回限りの過去の patch 手順）、`handover/notes/*`、`plan/ws031/phase00*/`、`plan/ws029/phase002/`、`plan/.sync/**`、`plan/history/index.md`、`plan/ws031/results-ws031.md`、`ws.md`、`external-design.md`、`native-vulkan-design.md`、`plan/ws029/*.md`: 当時の配置の記録。
- `plan/ws031/i915-refactoring-{assets,functions,design,review}.md` と `i915-rebuild-*.md`: 本再構築の基準資料（旧パスを意図して指す）。削除後は基準 commit `7e7ff337` のパスとして読む旨を注記するとよい。
- `src/drivers/gpu/i915/data/display-*.inc`（約 60 本）の「moved from src/drivers/gpu/i915/parity/lcd/…」: 出典の記録。旧ツリー削除後も意味が通るよう、基準 commit を併記するとよい（任意）。

## 6. 判定: レビュー後、旧ツリーを削除する前に残っていること

旧ツリーはユーザー決定により専門家レビューの参照として残す。以下はレビューの指摘を片付けた後、削除の前に満たすべき条件。

**本番コードの移植は完了している。** 本番経路の関数で行き先の分からないものはない。MISSING 5 件は旧 legacy `request.c` の
`drv_i915_request_kick`、`drv_i915_request_retire`、`i915_request_emit`、`i915_request_emit_prologue`、`i915_request_emit_breadcrumb`（§3.1）で、
resident 構成では到達しない（呼出し元は廃止済みの legacy `engine.c` と、`PARITY_SHIM_REDIRECT` で shim へ差し替えられていた `i915.c:1581` だけ）。

削除前に必要なこと:

1. **廃止の記録を補う**: (a) legacy `request.c` の上記 5 関数（と `internal.h` の `I915_REQUEST_MAX_DWORDS`）を計画 §5 の廃止行に加える。
   (b) 旧 vk module（`res`、`pipe`、`cmdbuf`、`wsi`、`display`、`sync` の fence 以外、`vk/linux/surface-state-gen12.inc`）は S3a が「到達しない」と報告しただけで、
   廃止の決定行がない（[計画 §5](i915-rebuild-plan.md) L67 は legacy 5 本のみ）。関数 103 件を正式に廃止と記録する。
   (c) `parity/legacy_shim.h` の redirect マクロの廃止も一行記録する。
2. **S5 の完了**（試験は欠落扱いにしていない）: 本書作成時点で試験ツリーに見つかった旧試験関数は 115、まだ見つからないもの 254（§1。担当別は §2・§3.2）。
   特に: T4a の `eu_test.c`（45 関数）・`tex_fixture*_gen.inc`・`draw_fixture.h`・`vkref-generated.inc`、T3 の `lcd_fake_hw.c`・`dp_fixture_latitude5330.h`、
   T4b の `edp_sync_ktest.c`・`opregion_fwtest.c`・`parity_hpd_test.c`・`lcd_show_ktest.c`・`lcd_hw_check.c` と `parity_lcd_kernel.c` のシナリオ、
   `selftest.c` の扱い（legacy HW 試験は廃止、T4a）。各担当が旧 → 新の対応または廃止理由を残すこと。
3. **§5.1 の参照を直す**: 試験 fixture・host script（T1/T3）、生成器 2 本（`gen_fw_ranges.py`、`gen_lrc_offsets.py`）と点検ツール、廃止済み生成器 3 本の扱い、
   `license-inventory.md`／`provenance-ledger.md`／`handover/README.md`／AGENTS 等の現行文書、`userland/base/tests/gpu-i915/main.c` のコメント。
4. **出典 manifest の置き場を作る**（後日: `intel/provenance/` に置いたが、2026-09-22 にユーザー決定で削除）: `parity/lcd/port_lcd_calc.manifest.json` を [s4 §9-6](i915-rebuild-s4.md) L763 のとおり `data/provenance/` へ移す
   （ファイル単位で唯一の MISSING）。
5. **本番に残った試験用コードを S5 で外す**（削除の前提ではないが、旧ツリーとの対応確認と同時に済ませる）: `firmware.c: drv_i915_firmware_set_override`
   （[s1-reports](i915-rebuild-s1-reports.md) L16）、`display/hotplug.c` の model 用 world 項目（`i915_hpd_fake_gmbus_write` など、[s4 §2.4](i915-rebuild-s4.md) L170）。
6. 削除の直前に本監査を再実行し（手順は §0。スクリプトは centris `/tmp/i915-audit.py`、`/tmp/i915-report.py`、保存はしていない）、found 以外の行が
   retired/unported（記録あり）と test（試験ツリーで確認済み）だけになったことを確かめる。

機械照合の限界: 「match by tokens/substr/stem」は名前の対応、「content」は文字列の一致にとどまる。本書は関数単位の対応地図であり、関数本体の意味の同一性は
各段階の実機受入（S1–S4 の記録）とレビューで確かめる前提。関数内の static 変数・型・macro の statement 単位の照合（保全台帳「再確認方法と限界」）は本監査の範囲外。
