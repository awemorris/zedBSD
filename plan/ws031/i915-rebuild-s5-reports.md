# S5 agent reports (condensed)

## T2 contracts (done)
- src/drivers/gpu/i915/tests/contracts/: mocks mmio/dma/pci/rpm, 6 contract tests, contract.[ch], host_kernel.[ch], host_thread.c, host_unreached.c, run.sh, README.md.
- run.sh PASS: mmio 43/0 dma 41/0 pci 46/0 rpm 20/0 pte 21/0 sync 42/0 (plain + ASan/UBSan).
- dropped: complete_all (not in production); WQ-2/4/6/7 need the scheduler (kernel test only).
- PRODUCTION ISSUES (for review list): (1) mmio.c read32_auto/write32_auto: on forcewake ack timeout still accesses and then put() underflows -> false underflow (inherited from old). (2) pci.c setup_msi: no guard against second call (vector leak) — code reading only.

## Coverage audit (done) -> plan/ws031/i915-rebuild-coverage.md (reviewer's map)
- functions: found 2403, test-moved 115, test 254 (S5 pending), retired 108, unported 103, MISSING 5 (legacy request kick/retire/emit* — unreachable; now recorded as retired in plan §5.1).
- files: moved 310, test 46, unported 11, retired 7, partial 3, missing 1 (manifest -> now data/provenance/).
- data tables byte-identical (firmware, forcewake ranges, LRC offsets).
- remaining refs to old paths: tests (T1, T3), generators gen_fw_ranges.py/gen_lrc_offsets.py (write to parity/), check_generated.sh, notice_map.py, engine_sseu.py, vk_opcode_survey.py; retired generators port_*.py still present; docs license-inventory.md (183 rows), provenance-ledger.md, handover/README.md §7, AGENTS.md:392, plan/AGENTS.md:138, plan/master.md:1795, plan/queue.md:78, userland/base/tests/gpu-i915/main.c:32 comment.
- test hooks in production: firmware set_override (-> T4a), hotplug fake GMBUS model (-> T4b).
- reviewer attention: 5 file-scope bound-world pointers, render memory list / rect kernel cache statics, A10 ops storage not introduced, GPL-2.0 ACPI.

## T3 host display (done)
- tests/display/{dp-fake-hw,lcd-fake-hw}.c/.h, host-kernel.c, host-test.c/.h, host-{dp,lcd,lcd-modeset,opregion,native-decide}-test.c, README.md; plan/ws031/tests/run-*-host-test.sh updated + display-host-lib.sh.
- PASS: dp 72/0, lcd 56/0, lcd-modeset 123/0, opregion 12/0, native-decide 14/0; same counts as old; dp/lcd/lcd-modeset/opregion full output identical to old run (incl. modeset register trace); ASan/UBSan clean.
- review items: takeover.c:622 drv_i915_n1_crtc_state derefs takeover world without NULL check (modeset path depends on takeover world existing); gcc -Waddress-of-packed-member at dmc.c:909-925 (Linux same; host build disables it).

## Tools & docs (done)
- kept/retargeted: gen_fw_ranges.py (byte-identical), check_generated.sh (exit 0), notice_map.py, vk_opcode_survey.py, license_inventory.py (repo root from own path).
- retired -> plan/ws031/handover/tools/retired/ (+README): port_lcd_calc.py(+json), port_dp_aux_pps.py, port_intel_bios.py, gen_lrc_offsets.py, engine_sseu.py. (git sees delete + untracked: stage as renames)
- docs updated: license-inventory.md (196 rows remapped, +78 new notice files, §3 GPL question), provenance-ledger.md (§0 map), handover/README.md §7, gpu-i915/main.c:32 comment, data/provenance/README.md.
- GOVERNANCE lines for user: AGENTS.md:392, plan/AGENTS.md:138, plan/master.md:1795, plan/queue.md:78 (linux/i915-regs.inc, linux/i915-ids.inc -> data/...); WS029 sections describe legacy files (historical).
- follow-ups: forcewake-ranges.inc lacks notice text (MIT intel_uncore.c; old same); gen_dp_fixture.py can't reproduce S5 fixture; Makefile:162 CONFIG_DRIVER_PCI_I915_PARITY unused (+ SELFTEST option); gen-inc.py hard-codes ~/zedBSD root.

## T1 host render/legacy (done)
- PASS: run-vk-host-tests.sh full default list (plain+ASan/UBSan); run-vk-analyzer.sh 0 warnings (25 sources); ws029 run-i915-host-tests.sh "ppgtt stream"; run-i915-analyzer.sh gcc 0 / clang 2; run-i915-build-selection-test.py PASS (was failing).
- new stubs i915-vk-render-stubs.inc, README-vk-host-tests.md; ws029 README-retired.md, i915-ppgtt-test.c, stubs; retired uncore/gtt/irq/lrc/backend tests.
- analyzer logs rewritten (plan/ws031/phase012/analyzer-gcc.log, plan/ws029/phase007/analyzer-*.log（git の履歴、2026-10-08 の WS の完了で削除）).
- production issues: pipeline leak on compile failure (known XXX); descriptor sets never freed (78 refused); reply bodies written when reply flag 0 (latent); clang: submit.c:577 shift by 32 when ring size<=1 (UB, not reachable), workarounds.c:428 null wal (likely false positive); CONFIG_DRIVER_PCI_I915_SELFTEST selects nothing now.

## T4b kernel display tests (done)
- tests/display/: lcd-run.c (lcdb/lcdr/n1), hdmi-output.c, lcd-flip.c (lcdc), lcd-opregion.c (lcdo), lcd-gpu.c (lcdg/lcdd), display-ktest.c, ktest parts (edp, edp-sync, lcd-modeset, scanout, lcd-show, lcdg, opregion, hpd), aux.c, hdmi-hotplug.c, hpd-model.c, dp-fixture-latitude5330.h; added to I915_TESTS list.
- HW: lcdb PASS (17/17, pattern hash match), display_ktest 182/0 (1 skip DW-FLUSH), lcdc PASS 4/4 flips, lcdd PASS 8 draws/7 flips (needed -DI915_TEST_LCDD_HOLD_MS=1000: vkdemo service fails+watcher kills QEMU at 12 s).
- not run on HW: lcdr, lcdo, lcdg, hdmib, dual, dual_share, aux, hdmi_edid, hdmi_hpd.
- dropped: n1 (start refuses active firmware display; takeover needs production switch), opregion_fwtest (needs mid-start checkpoint), lcdb_summary.
- production changes (behaviour unchanged): modeset.c run steps public (locks_init, bind_ops, preflight, fill_cfg, at_stage; i915_lcd_run_params -> modeset.h); hotplug fake model moved to tests via weak checkpoints drv_i915_hpd_model_read/write/rmw_write; drv_i915_hpd_icp_entry exported.
- review items: opregion.c accessors deref display->opregion_world without NULL checks; single-instance file-scope worlds (tests swap worlds in/out); static TLB engine table.
- vkloop-hw.sh: syntax error line 96 (T4a), summary filter misses display verdicts, long scenarios race vkdemo kill timer.

## T4a kernel runner/ktest/EU (done)
- hook: device.c weak drv_i915_test_after_start(device) before worker_serve. tests/execution/{runner.c, ktest*.c, eu-test.c, draw-test.c, fhd-render.c, ppgtt-walk.c, firmware-override.c, README.md}, display/scenarios.h, fixtures/ (draw, tex, vkref). vmunix.mk I915_TESTS=y (+ I915_TEST_ORACLE=y for readback). vkloop-hw.sh `test <scenario>` (TEST_WAIT_S delays vkdemo, default 90), atomic install.
- HW: ktest 382/0 (13 skip); eu PASS batch 5dfb47d3c10b0560 fixture 444e3a7a4e9c1abd, EU-REPEAT 5/5 (= E-99/E-100); draw 1024/1024 (= E-101/102 pins); r1 12/12; tex 1024/1024 (= E-103); t3 9/9; bl 4/4 max diff 0 (= E-105); production offscreen 7523debe unchanged.
- firmware.c: set_override removed; weak drv_i915_firmware_test_request checkpoint.
- dropped: fault-injection hooks, static helpers' checks (8), 13 runtime skips (VBT slot singleton 8, kern_diag_oneshot_arm 5), legacy selftest.c, reference-kernel build (render seam insufficient).
- review items: driver-wide singletons vbt.c:266, opregion.c:119, dp-sink.c:136, state.c:99, modeset.c:134; container_of embedding requirement power.c:2604/2618, dmc.c:606, takeover.c:2483; power.c:2174 vga NULL; sync.c:70 global unclearable time-base fault; pcode_poll EIO without latch; no lrc_keep; MOCS_UNCACHED_INDEX only in render/heap.h; unused i915_pch_bridge_ops; firmware blob symbols no header; no wedged marker after hang (old same). old-test bug fixed test-side: R1/T3 PTE scrub.
