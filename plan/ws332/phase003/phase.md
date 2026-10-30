<!-- awesome-plan project=zedbsd record=ws332-p003 -->

# ws332-p003: 率を仮定する残りを時間の単位に

Status: in-progress（2026-10-31 P6）
Parent: [WS332](../ws.md)
Queue: q1738-i01（P6、P3 の列から移した）

## 範囲

design.md §2 の最後に挙げた、`KERN_CLOCK_HZ` を使わずに tick の率を仮定している所を、時間の単位（ms・us）で書き、tick への変換を
clock.c の関数（`kern_ms_to_ticks()`・`kern_us_to_ticks()`）に任せる。

- tick の数の literal: `VM_FREE_WAIT_TICKS`（src/kern/vm）、rpi4-vchiq.c の `sched_ticks() + 2U`、i915 の `I915_WORKER_WAIT_TICKS`・`I915_XD_SCREEN_EVADE_TICKS`。
- 「次の tick まで」の `+ 1`（8 か所）: process.c（_exit）、exec.c（兄弟の thread の退場）、clock.c（`kern_usleep_range()`）、writeback.c（pause）、
  lpss-i2c.c、amdrdna cp.c（fence）、bcm2711 の v3d-hardware.c・v3d71-hardware.c。
- 範囲の外: 期限の後ろの補いの `+ 1`（`kern_ms_to_ticks(x) + 1U`、ahci・hda・rpi4-mmal・vchiq・i2c-hid・tcp.c など。期限が最後の tick から
  数えるので最大 1 周期早く起きるのを補う物で、どの率でも 1 周期の補いとして正しい。design.md §5 の拡張で扱う）、host 試験の偽の時計の tick。

## 置き換えの規則

- 「次の tick まで」の短い待ちは、その所の意味の名前の付いた ms の長さにする。値は今の最も短い tick（1 ms）にし、`kern_ms_to_ticks()` の切り上げで
  今のどの率でも 1 tick になる（今の挙動と同じ）。
- `kern_usleep_range()` は「次の tick まで」を繰り返す代わりに、残りの時間（counter の差を us にした物）を `kern_us_to_ticks()` で待つ。
  期限は最後の tick から数えるので早く起きうるが、loop が counter を読み直して残りを待つ。

## 結果（2026-10-31 P6、branch agent/p6-ws332、基点 71aca4b70）

| 所 | 前 | 後 | 今の率での値 |
| --- | --- | --- | --- |
| src/kern/vm（reclaim.c・pageout.c） | `VM_FREE_WAIT_TICKS 10U` | `VM_FREE_WAIT_MS 10U`、`kern_ms_to_ticks()` | 1000 Hz は 10 tick（同じ）。100 Hz は 10 tick（100 ms）→ 1 tick（10 ms） |
| rpi4-vchiq.c（slot の回収待ち） | `sched_ticks() + 2U` | `VCHIQ_RECYCLE_WAIT_MS 2U` | aarch64（1000 Hz）で 2 tick（同じ） |
| i915 worker.c | `I915_WORKER_WAIT_TICKS 1U` | `I915_WORKER_WAIT_MS 1U` を関数の頭で tick に | amd64 で 1 tick（同じ） |
| i915 xd-screen.c | `I915_XD_SCREEN_EVADE_TICKS 2L` | `I915_XD_SCREEN_EVADE_MS 1U` を tick に切り上げて + 1（Linux の msecs_to_jiffies_timeout と同じ形） | amd64 で 2 tick（同じ）。host 試験の 1 ms の tick でも 2 |
| process.c（_exit）・exec.c（兄弟の退場）・writeback.c（pause）・lpss-i2c.c・amdrdna cp.c・v3d-hardware.c・v3d71-hardware.c | `+ 1U` | 各所の 1 ms の名前（`PROCESS_EXIT_POLL_MS` など）を `kern_ms_to_ticks()` | どの率でも 1 tick（同じ） |
| clock.c `kern_usleep_range()` | 毎 tick 起きて counter を見直す | 残りの時間（`scale_ceil()` で us に切り上げ）を `kern_us_to_ticks()` で待ち、起きたら見直す | 戻る時刻は同じ（最小の時間を過ぎてから）。min_us が 1 tick より長い時に途中で起きる回数が減る |

- 挙動が変わる所: 100 Hz の arch（i386・m68k・ppc・sparcv9）の空きの memory の待ち 1 回が 100 ms から 10 ms になり、object の page の fault が諦めるまでが
  comment どおりの約 10 秒（1000 回）になった（前は約 100 秒）。`kern_usleep_range()` の途中の起床の回数。他は今のどの率でも同じ値。
- 書き換えに伴う comment: xd-screen.h の hook の説明「10 ms ticks」→「scheduler ticks」、clock.h の `kern_usleep_range()` の説明（busy の delay と書いていたのを
  実際の yield する sleep に）。`kern_usleep_range()` は書き換えたので、関数の形（宣言・comment・名前）も規約に合わせた。
- 範囲の外として残した物: 期限の補いの `+ 1U`（上の「範囲の外」）、host 試験の偽の時計の tick（i915 contracts の sync_contract_test.c の `+ N`）。

### 確かめ（P6）

- 全 port の vmunix（`make ZEDBSD_CONFIG=<config/ci か plan/tools/ports の config> BUILD=/tmp/p6-ws332/ports/<port> <BUILD>/vmunix`）: amd64・pcat・pc98・intelmac・rpi4・rpi5・
  odroid-c5・visionfive2・riscv64-virt・riscv32-virt・armv7-virt・zybo・m68k-virt・ppc32-mac99・ppc64-pseries・mips32-malta・mips64-malta・sun4u・x68k の 19 が rc=0、warning 0
  （要約 /tmp/p6-ws332/ports/summary.txt）。
- host 試験: `sh plan/tools/i915-display/run-xd-host-tests.sh /tmp/p6-ws332/i915-xd` → `run-xd-host-tests: all passed`（xd-screen の evade の 2 tick を含む）。
- QEMU の起動は p004 の後の先端で 1 回（下の p004 の記録）。共有の image の lock が他の担当の build で塞がっていたため、p003 だけの image は作っていない。
