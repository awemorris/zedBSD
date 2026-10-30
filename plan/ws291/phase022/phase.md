<!-- awesome-plan project=zedbsd record=ws291-p022 -->

# ws291-p022: Intel（i915、Latitude 5330 の Alder Lake）で重い 3D の model を、スペックの比で決めた目標の fps に

Status: in-progress
Parent: [WS291](../ws.md)
Queue: q1731（P1）

## 目的

2026-10-30 ユーザーから、i915 についても WS291 に Phase を作り、5330 で最適化を行い、P1 を割り当てる、という趣旨の指示を受けた。

## 目標の決め方（2026-10-30 改め: 実測の Linux を基準に、RDNA・NVIDIA の p002・p010 と同じ形）

- 2026-10-30 Q1 から、ユーザーが「5330 は Linux も起動できるので、RTX の比ではなく、実測の Linux の性能をゴールにする」という趣旨の指示をした、と伝えられた（原文は Q1 の記録）。
- 基準: 5330 の Debian（Mesa の Intel の Vulkan の driver、ANV）で同じ mview（qs40、`plan/tools/gpu-perf/linux/build-mview.sh`）を weston の DRM の kiosk の上の fullscreen（plane-only、direct scanout）で測り、copies を変えて約 50 fps になる scene を決める。解像度は 5330 の内蔵の画面（1920x1080）で zedBSD と揃える。
- 受け入れ: その scene（copies・解像度・present の形を記録）で zedBSD の fps が Linux 以上、direct scanout。
- 参考（前の決め方、使わない）: RTX 2070 の FP32 との比。5330 の GT は 80 EU（`gt_init_mmio ... total=80`）、RP0 1200 MHz（72 × 16.67）で FP32 約 1.54 TFLOPS、比 約 4.86、copies 約 1030 だった。

## 範囲

1. 5330 の zedBSD（NVMe）で mview が動くかを確かめ、足りない Vulkan の機能（indexed draw・instance・mip など）があれば i915 の Vulkan の実行器に足す。
2. Linux（ANV）で約 50 fps の scene を決め、zedBSD の今の fps を測る。
3. profile を見て i915 の driver・Vulkan の backend・compiler を直す。HAL は変えない、driver は hal_* を使わない。改善ごとの fps と profile を記録。
