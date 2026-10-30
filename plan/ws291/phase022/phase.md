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

## 道具（plan/ws291/tests/）

- `5330-mview.sh`: 1 本の script（命令ごとに `/tmp/i915-hw.lock`）。`backup`（NVMe の zedBSD の p4 の file・p5・p6・ESP の loader を Debian の `~/p1-p022/orig` へ、sum 付き）、`image IMG`、`kernel VMUNIX`（前は vmunix.prev）、`run OUT COPIES...`（session の中の mview、12 s、game・mailbox・4 images、MVIEW SPIN/STAGES、KWL SCANOUT、kernel の行、hw.gputelemetry の標本）、`shot`、`linux-build`・`linux-run`・`linux-shot`・`linux-exec`（Debian で iGPU を測る間だけ i915 に、weston の kiosk の DRM、ANV）、`linux`・`zedbsd`、`restore`（退避を書き戻して sum を照合）。
- `config-5330.mk`（`plan/tools/hw5330/config-5330.mk` ＋ mview・sysctl、`user` の自動 login、root の SSH の鍵）、`autologin-user`。
- `i915-stats-diag.patch`: 診断の kernel（pipeline statistics を batch の前後の SRM で数え 5 秒ごとに log、hw.gputelemetry の cur_mhz に RPSTAT1 の CAGF、objects_limit に GT0_PERF_LIMIT_REASONS 0x1381a8）。commit しない差分。
- `i915-host/shader-dump.c`: host で i915 の compiler に SPIR-V を通して EU の code を書く（Mesa の brw_disasm で読む）。

## 結果（実機 5330、2026-10-30〜31、P1）

### Linux の基準（Debian 13、Mesa 25.2.6 ANV、weston 14 kiosk DRM、eDP 1920x1080、plane-only）

| copies | fps |
| --- | --- |
| 1 | 1192.7 |
| 500 | 60.5 |
| 600 | 50.56・50.60・50.63 |
| 620 | 48.98・48.62 |
| 1000 | 30.6 |
| 1500 | 20.4 |

- **scene: qs40 × 600、1920x1080、mailbox 4 images、12 s、direct scanout（weston の plane-only）で 50.6 fps**。`--cull=all`（頂点の仕事だけ）は 65.1・60.7 fps。GT の実の周波数（gt_act_freq_mhz）は 1200 MHz。
- 見た目の参照: `linux-shot-1.png`・`linux-shot-600.png`（weston-screenshooter、build/p022-meas/shots、git の外）。

### zedBSD（main 48a27d158 の image を NVMe に）

- 起動: main の kernel は 5330 の NVMe で起動した（ssh・session・wayland-0 まで）。dmesg: `gt_init_mmio ... sseu(slice=0x1 dss=0x1f eu/ss=16 total=80)`、`rps: ... RP0=72`（1200 MHz）。
- mview は i915 の Vulkan で qs40 を描けた（index の draw・instance・mip は既にあった、足した機能は無し）。direct scanout（`KWL SCANOUT direct=1`）。
- 見た目: copies 1 は ANV の参照と全 pixel 一致、600 は 1 pixel が 1 階調違うだけ（MOCS の変更の後の kernel で）。

| kernel | c600 fps | GPU（submit の中の run） | 備考 |
| --- | --- | --- | --- |
| main 48a27d158 | 22.96・23.04 | 約 39.4 ms/frame | draw の全 surface が MOCS 3（L3・LLC uncached） |
| ＋ MOCS（入力・depth・state は index 2 WB、colour target は 61） | 30.10・30.16 | 約 30.7 ms/frame | 1.31 倍 |

- cull=all（MOCS の後）: 35.3 fps（前段だけで約 26 ms、Linux 約 16 ms）。
- pipeline statistics（診断の kernel、c600 の 1 frame）: IA 21.2M prims、VS 16.5M（頂点の cache は効いている、unique 15.5M）、CL 21.2M、PS 6.9M。
- **GT の実の周波数**（診断の kernel、mview の間）: 要求 1200 MHz に対し **実 800 MHz**。GT0_PERF_LIMIT_REASONS = 0x05000C00（PL1 と PL2 の電力の上限）。Linux は同じ負荷で 1200 MHz。CPU の idle が HLT（C1）だけ・HWP 無しの件（master.md の WS299 の続き・WS301）で package の電力が CPU 側に食われている見込み（未確認）。GPU の driver の範囲の外。
- VS の code: 134 命令（ANV 約 50）。MAD 無し、push の scalar を mov で広げる、VUE の 16 register を 0 で埋める、全命令が SWSB `@1`。
