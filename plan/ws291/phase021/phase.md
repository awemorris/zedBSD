<!-- awesome-plan project=zedbsd record=ws291-p021 -->

# ws291-p021: Raspberry Pi 5（V3D 7.1）で重い 3D の model を、スペックの比で決めた目標の fps に

Status: in-progress
Parent: [WS291](../ws.md)
Queue: q1729（P4）

## 目的

2026-10-30 ユーザーから、P4 で Pi 5 の GPU の改善を行い、PowerVR と同じくスペックの比から目標を決める、という趣旨の指示を受けた。Pi 5 の試験の image を作ってユーザーに渡す。

## 目標の決め方（PowerVR と同じ形）

- 基準: Linux（NVK）の RTX 2070 の scene-5000（qs40 × 5000、1920x1080、約 50 fps）。RTX 2070 の FP32 は約 7.46 TFLOPS。
- Pi 5 の V3D 7.1: 実機の dmesg（WS279、2026-10-22）で 12 QPU、clock 1.15 GHz（公表の値は 800 MHz）。FP32 は QPU 数 × 4 lane × 2（積と和）× clock で、1.15 GHz なら約 110 GFLOPS（比 約 68 倍 → copies 約 74）、800 MHz なら約 77 GFLOPS（比 約 97 倍 → copies 約 51）。走っている clock を実機で確かめて確定し、ws.md に記録する。
- 受け入れ: qs40 × copies（上の確定値）、1920x1080、direct scanout で 50 fps 以上。

## 範囲

1. Pi 5 の GPU（CONFIG_DRIVER_BCM2712_GPU=y）と Keiland の desktop と mview を入れた試験の image を作り、ユーザーに渡す（path・大きさ・sha256・書き込みの命令）。
2. ユーザーが起動した後、SSH で clock を確かめて目標を確定し、今の fps を測る。
3. profile を見て driver（src/drivers/gpu/bcm2711/ の V3D 7.1、Vulkan の backend、compiler）を直す。HAL は変えない、driver は hal_* を使わない。
