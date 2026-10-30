<!-- awesome-plan project=zedbsd record=ws291-p022 -->

# ws291-p022: Intel（i915、Latitude 5330 の Alder Lake）で重い 3D の model を、スペックの比で決めた目標の fps に

Status: in-progress
Parent: [WS291](../ws.md)
Queue: q1731（P1）

## 目的

2026-10-30 ユーザーから、i915 についても WS291 に Phase を作り、5330 で最適化を行い、P1 を割り当てる、という趣旨の指示を受けた。

## 目標の決め方（PowerVR・Pi 5 と同じ形）

- 基準: Linux（NVK）の RTX 2070 の scene-5000（qs40 × 5000、1920x1080、約 50 fps）、FP32 約 7.46 TFLOPS。
- 5330 の iGPU（Alder Lake の Xe-LP、EU の数と最大の clock は実機で確かめる）: FP32 = EU 数 × 8 lane × 2（積和）× clock。例: 96 EU・1.25 GHz なら約 1.92 TFLOPS（比 約 3.9 倍 → copies 約 1,290）。実機の EU の数と clock で確定し、ws.md に記録する。
- 受け入れ: 確定した copies、1920x1080（5330 の内蔵の画面の解像度が違えばその画面で、比で割る）、direct scanout で 50 fps 以上。

## 範囲

1. 5330 の zedBSD（NVMe）で mview が動くかを確かめ、足りない Vulkan の機能（indexed draw・instance・mip など）があれば i915 の Vulkan の実行器に足す。
2. EU の数と clock を確かめて目標を確定し、今の fps を測る。
3. profile を見て i915 の driver・Vulkan の backend・compiler を直す。HAL は変えない、driver は hal_* を使わない。改善ごとの fps と profile を記録。
