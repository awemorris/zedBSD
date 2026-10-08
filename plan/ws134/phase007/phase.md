<!-- awesome-plan project=zedbsd record=ws134-p007 -->
# ws134-p007: K3 kernel の GPU の telemetry `hw.gputelemetry`

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-066 PASS。i915 の実の値は p010 の実機）（旧: in-progress（q661、P2 generation8、2026-10-04。実装・build 済み、T の QEMU の試験待ち。i915 の値は実機（5330）でしか見えない））
Disposition: normal
Parent: [WS134](../ws.md)
設計: [design.md](../design.md) §1.2 の K3（Guardrail「compositor は GPU の UAPI を ioctl で呼ばない」により sysctl）
依存: なし。2026-10-04 Q1「i915 は実機の LCD の作業（WS075・WS118）で敏感な所なので、telemetry の読みは i915 の既存の状態を読むだけにし、
rps・GEM の動きを変えないで。QEMU で確かめられる範囲（Venus は要素なし、sysctl の形）を試験にして。」

## 実装

- `include/uapi/sysctl.h`: `HW_GPUTELEMETRY 8`、`GPU_TELEMETRY_VERSION 1`、valid の bit（BUSY・CUR_MHZ・REQ_MHZ・RANGE_MHZ・OBJECTS）、
  `struct gpu_telemetry_header`（24 byte）と `struct gpu_telemetry_entry`（driver[16]・valid・reserved・time_ns・busy_ns・objects_bytes・
  objects_limit・cur/req/min/max の MHz、72 byte）、`_Static_assert`。
- `include/kern/sysctl.h`・`src/kern/sysctl.c`: `kern_gpu_telemetry_register(driver, read, context)`（最大 8、追加だけ、lock の下で足して count を
  release で公開、読み手は acquire の count の下を lock なしで読む。同じ context は 1 回。context は kernel の寿命）。`sysctl_gputelemetry`
  （登録された GPU ごとに entry を driver の名前と 0 で用意し read を呼ぶ。失敗した GPU は出さない）。
- `src/drivers/gpu/i915/gt-power.c`: `drv_i915_rps_telemetry_read`（rps の lock の下で `busy_total_ns`＋実行中の分（`now - busy_since_ns`）、
  `last_freq`・`min_softlimit`・`max_softlimit`・`enabled` を読むだけ。register は読まず、rps と GEM の動きは変えない。MHz は 50/3 MHz の単位から）。
  cur_mhz（hardware の今の周波数、register の読みが要る）と GEM の objects は出さない（valid の bit なし）。
- `src/drivers/gpu/i915/device.c`: `drv_i915_rps_start` の直後に `kern_gpu_telemetry_register("i915", ..., &gt->init.rps)`（1 行）。
- Venus（virtio-gpu）は登録しない（要素なし）。
- `userland/base/sysctl/main.c`: `show_gputelemetry`（`hw.gputelemetry: gpus=` と GPU ごとの行）、`sysctl -a` にも。

## 確かめ

- build: kernel は amd64（i915 入り）・pcat・rpi4・intelmac で exit 0、warning 0。sysctl の CLI exit 0。style-check の新しい違反 0。
- QEMU（T に依頼）: `plan/ws134/tests/gputelemetry-p007.sh`（Venus で `hw.gputelemetry: gpus=0` と GPU の行なし、隣の hw.cputimes・
  hw.diskstats も読める、`sysctl -a` は記録だけ）。結果は未着。
- 実機（5330 の i915、busy・周波数が負荷で動く）: 未実施（Q1 が user と時間を決める）。

## QEMU の結果（Q1、2026-10-04、T1-066、Venus KVM）

`gputelemetry-p007: PASS`: `hw.gputelemetry: gpus=0`（Venus は要素なし）、hw.cputimes・hw.diskstats も読める。i915 の値（rps の busy・周波数）は実機（5330、S2 か passthrough）で確かめるまで in-progress。

## 結果（T1-066、QEMU Venus KVM、p2-q660 の image（1345e85）、2026-10-04）

`gputelemetry-p007: PASS`。Venus で `hw.gputelemetry: gpus=0`、hw.cputimes・hw.diskstats も読める（nvme0n1 kind=2 generation=1）、
`sysctl -a` exit 0（29 行、記録だけ）。証拠 `/home/awe/zedBSD-worktrees/t1/build/t1-066/out/`。i915 の値は実機（5330）で未確認。

## 2026-10-04 UAT（実機）

素の 5330 で `sysctl hw.gputelemetry` が `driver=i915 valid=0xd busy_ns=… cur_mhz=0 req_mhz=100 min_mhz=100 max_mhz=1200` を返した（idle で cur 0、電池の時 req 500）。実機の確認は OK（証拠 `plan/uat/2026-10-04/sysctl.txt`・`battery.txt`）。QEMU の試験の結果と合わせて Q1 が判定する。
