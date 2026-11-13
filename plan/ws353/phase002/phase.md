<!-- awesome-plan project=zedbsd record=ws353-p002 -->

# ws353-p002: kernel の display の事象の fence（HOTPLUG）

Status: planned
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §2.1・§3。

## 目的

display node の topology の変化で、登録された fence を kernel が signal する口を作る。libvulkan が問い合わせで判定する形（ユーザーの決定 2 でやめる形）の土台を置き換える。

## 変える file

- `include/uapi/gpu-display.h`: `GPU_DISPLAY_EVENT_FENCE` と `struct gpu_display_event_fence`、`GPU_DISPLAY_EVENT_HOTPLUG`・`GPU_DISPLAY_EVENT_REFRESH`（REFRESH は p014 まで EOPNOTSUPP）、flag `GPU_DISPLAY_EVENT_FENCE_CANCEL`。
- `include/drivers/gpu/gpu.h`（driver と枠組みの口、HAL ではない）: `drv_gpu_display_changed(device, sequence)`。
- `src/drivers/gpu/gpu.c`: ioctl（`GPU_CAP_DISPLAY_EVENTS`・読みの権限、`GPU_CAP_FENCE` は要らない）、open ごとの登録の list（上限 16）、登録の後の読み直し、changed での signal と外し、CANCEL、close・offline・故障での片付け（故障は error で signal）。`gpu-fence.c` は今の `drv_gpu_fence_create`・signal を使う（足りなければ最小の口を足す）。
- driver（`events` の実装 8 つ）: i915（`display/hpd.c` の `topology_sequence++` の 2 か所。`display.c`・`bootfb.c`・`capture.c` は番号が動く所があれば）、venus（`display.c`）、amdrdna（`present.c`）、nvrtx（`display/present.c`）、ghci（`display.c`）、videocore（`display/display-device.c`）。番号を上げた後の `poll_notify()` を `drv_gpu_display_changed()` に置き換える（中で `poll_notify()` を呼ぶので POLLPRI の待ち手は今のまま）。

## 残す振る舞い

- `GPU_DISPLAY_EVENTS` の QUERY・ACK と POLLPRI（swapchain の acquire の待ちが使う）は変えない。
- HAL（`include/hal/hal.h`）・Makefile・mk/ は変えない。

## 先に確かめる事

- 8 つの `events` の実装で、topology の番号が device（node）ごとに一つで open によらない事。違う driver があれば止まって Q1 に報告（設計の契約が崩れる）。
- `drv_gpu_display_changed` を呼ぶ文脈（spinlock を持たない、IRQ の外）が今の `poll_notify()` の場所で成り立つ事。

## 確かめ

- build: x86_64・arm64・riscv64 の kernel（変えた driver を含む config）warning 0。
- QEMU（Venus、P1 の名前・port）: `plan/ws353/tests/` に guest の小さな program（ioctl で HOTPLUG の fence を作り、fd を poll(-1)）と 1 本の script（配置 → 起動 → program → `plan/tools/guest/venus-head.sh` で head を抜き差し → 回収 → 要約）。判定: 抜き差しから 1 s 以内に POLLIN、抜き差しが無い 30 s は起きない、登録の直前の変化（cursor の競い）は登録の直後に POLLIN、CANCEL の後は起きない、open の close で登録が残らない（同じ open で 16 を越えると ENOSPC、close して作り直せる）。
- 判定は guest の file と SSH・QMP（console・serial の log を読まない）。

## 受け入れ

- 上の build と QEMU の判定が PASS。i915 などの実機は未実施でよい（p016 で 5330）。
- 結果・SHA・未実施をこの phase.md に書く。

## 依存

- p001（設計）。
