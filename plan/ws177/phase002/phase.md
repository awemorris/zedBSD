<!-- awesome-plan project=zedbsd record=ws177-p002 -->

# ws177-p002: i915 の Type-C の残り — legacy の PHY の待ちを sleep に、停止で PHY を返す（案 F）

Parent: [WS177](../ws.md)
Status: in-progress（実装済み・host PASS・kernel の build warning 0。実機の確認は 5330 の後、QEMU では i915 を通らない）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q882 の 2（P1、2026-10-08、承認は Q1 の dispatch「Queue（q882、承認済み）: WS177 の案 E・F・G の host の分を順に」）
Origin: [backlog-p2](../backlog-p2.md) の 153 行（WS051 ws051-p002b の TC の核）、[案](../phasing-20261008.md) の F
Purpose / goal: (1) legacy の port の PHY の ready を待つ 500 ms を busy wait でなく sleep する待ちにする。(2) driver の停止で Type-C の port の PHY の所有と TC cold の block を Type-C の subsystem へ返す。

## 範囲の外（backlog-p2 153 の残り、そのまま残す）

- TBT の mode の TC_COLD_OFF の domain（ADL-P に well が無い、TBT-alt は範囲外）。
- suspend（S0ix）で PHY を返す: ws051-p005b の M10、WS052（ベータ3）。今回の `drv_i915_tc_stop` は停止の道だけ。suspend の hook に使うなら resume で readout をやり直す道が要る（M10 の時に決める）。
- AUX の転送ごとの lock・unlock（ws051-p004a）、lcd の world の STEP・0 の関数（ws051-p003）、HPD の pulse の後の DP の detect。
- 停止の時に Type-C の layer（`drv_typec_display_report`）の report を unknown に戻すこと: layer の API は report を必ず known にするので、消す口が要る（backlog-p2 155、ws050-p005 の「display の driver の停止で report を消す」、G の範囲で扱う）。

## 設計（2026-10-08 P1）

- **待ち**: `struct i915_tc_env` の `delay_us`（busy wait）を `now_us`（単調な clock、μs）と `sleep_us`（CPU を譲って少なくとも求めた時間待つ、tick まで延びてよい）に置き換えた。`tc_wait_ready` は時間を clock で測る（sleep が tick（例 10 ms）まで延びても 500 ms で終わり、読む回数が減るだけ）。clock が壊れたら ready の bit を 1 回だけ読んで終わる（無限に待たない）。待ちは readout（driver の開始の thread、port の mutex と PORT の power の参照を持つ、どちらも sleep してよい）からだけ呼ばれる。Linux の `tc_phy_wait_for_ready()` の `wait_for(..., 500)` と同じ。
- kernel の側（`tc-kern.c`）: `now_us` は `kern_rtc_read_counter`（秒と余りに分けて overflow しない変換）、`sleep_us` は `kern_usleep_range`（次の tick まで yield する）。`drv_i915_udelay` は TC から使わなくなった。
- **停止**: 新しい `drv_i915_tc_stop(tc)`（core）と `drv_i915_tc_kern_stop(display)`。宣言された各 port を lock の下で: 残った link を 0 に（output は先に止まっている。残っていれば log）、`tc_disconnect`（TC cold の block と ownership を返す）、`present = 0`（退役。lock の下で present を見る全ての step が、既に向かっていても何も取らない）。最後に `live = 0`（以後どの port も答えない）。2 回目は何もしない。
- 呼ぶ所: `drv_i915_display_stop_early` の hotplug の停止（`drv_i915_hpd_stop`）の直後、`i915_driver_unregister` の前（output は last resort で止まり、hotplug の work は消え、power domain はまだ動く）。Linux は `intel_tc_port_cleanup()`（encoder の destroy）と CRTC の disable の put_link で返す。

## 変更

- `src/drivers/gpu/i915/display/tc.h`（env の `now_us`・`sleep_us`、`drv_i915_tc_stop`、present・live の契約）、`tc.c`（`tc_wait_ready`、`drv_i915_tc_stop`）、`tc-kern.c`・`tc-kern.h`（`tc_kern_now_us`・`tc_kern_sleep_us`、`drv_i915_tc_kern_stop`）、`display.c`（停止の呼び出し）。
- 試験: master の Tools に登録済みの回帰 `plan/ws051/tests/host-tc.c` を env の変更に合わせて直し、`test_wait_ready`・`test_stop` を足した（WS051 の試験だが master 登録の回帰なので変更に追従させた）。

## 確認（host・build、2026-10-08）

- `sh plan/ws051/tests/host-tc.sh build/p1-tc-host` → `host-tc checks=112 failures=0`、`host-tc-tables checks=34 failures=0`（ASan/UBSan）。新しい check: 10 ms の tick で 50 回の sleep・0.5 s、1 ms で 500 回、3 回目の後に ready で止まる・timeout の log 無し、clock の失敗で sleep 0、USB-C の port は待たない、停止で link の残る TC1 の ownership・TC cold を返す・全 port 退役・live 0・power と lock の均衡、停止の後の get_link・lock・put_link・stop は書き込み・power・lock 無し。
- kernel の build（warning 0、-Werror）: `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=config/current-uat.mk build/p1-k/vmunix`。
- style-check: `tc-kern.c`・`tc.h` 0 → 0、`display.c` 69 → 69（既存）、`tc.c` 12 → 13（増えた 1 は停止の critical section の unlock の行で、既存の 12 と同じ形 = §5 の lock と unlock の段落の規則どおり。checker の誤検出の型）、`host-tc.c` 6 → 6。

## 未実施

- 実機（5330）: 停止の道（driver の unload）での PHY の返却、legacy の port（5330 には無い）。QEMU は i915 を通らないので T1 の依頼は無し。

## Event

2026-10-08 / q882-i02（P1）: 実装と host・build の確認。merge 依頼を Q1 へ。
