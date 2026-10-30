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
- `i915-stats-diag.patch`: 診断の kernel（pipeline statistics を batch の前後の SRM で数え 5 秒ごとに log、hw.gputelemetry の cur_mhz に RPSTAT1 の CAGF、objects_limit に GT0_PERF_LIMIT_REASONS 0x1381a8、objects_bytes に package の energy、min/max_mhz に package の power limit の register）。source に入れない差分（`git apply` して build）。
- `telemetry-sum.py`: run の標本から実の周波数・package の電力・limit の理由を要約（zedBSD と Linux）。
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
| ＋ compiler: push 定数・float の即値・頂点の attribute を register に写さず直に読む、always 書く VUE の register を 0 で埋めない（mview の VS 134 → 83 命令） | 31.37・31.62 | 約 29.7 ms/frame | ＋4.5%。shot は ANV と同じ（c1 一致、c600 1 pixel 1 階調）、desktop の keiland-shot も正常。tree の VS 21 本は全て compile でき、全部縮んだ |
| ＋ compiler: MAD（Gen12 の 3-source の encode、Mesa の brw_asm と bit 一致を host で確かめた。一度だけ読まれる積を和・差と一つの MAD に。mview の VS 83 → 67 命令） | 30.78・30.95 | 約 30.2 ms/frame | 同じ時間帯の MAD 無し（直読みだけ）の kernel は 30.46・30.66・30.74 で、差は電力の上限の揺れの内（熱で前の 31.4〜31.6 から下がった）。shot は ANV と同じ。tree の VS 21 本は compile でき、Mesa の disasm → asm で bit 一致 |

- cull=all（MOCS の後）: 35.3 fps（前段だけで約 26 ms、Linux 約 16 ms）。
- pipeline statistics（診断の kernel、c600 の 1 frame）: IA 21.2M prims、VS 16.5M（頂点の cache は効いている、unique 15.5M）、CL 21.2M、PS 6.9M。
- **GT の実の周波数**（診断の kernel、mview の間）: 要求 1200 MHz に対し **実 800 MHz**。GT0_PERF_LIMIT_REASONS = 0x05000C00（PL1 と PL2 の電力の上限）。Linux は同じ負荷で 1200 MHz。CPU の idle が HLT（C1）だけ・HWP 無しの件（master.md の WS299 の続き・WS301）で package の電力が CPU 側に食われている見込み（未確認）。GPU の driver の範囲の外。
- **package の電力**（診断の kernel が GT の MMIO の MCHBAR の写し 0x14593C・0x145938・0x1459A0 を読む、`telemetry-sum.py`）: zedBSD は mview c600 で 13.0 W、PL1（MMIO）13.0 W に張り付き、GT 800 MHz。**zedBSD の idle（desktop の session、GPU は止まっている）で既に 12.99 W**。Linux（intel-rapl:0）は idle 3.2〜3.5 W（i915・weston の kiosk）、mview c600 で 14.6 W（12 s、1200 MHz）・14.3 W（60 s、42 s 後に 1100 MHz、49.4 fps）。zedBSD は idle の時点で約 10 W を余計に使い、GT の電力の枠を食っている。CPU の idle（HLT だけ）・P-state（HWP 無し）の見込み。Linux の cpuidle を C1E だけにしても Linux は 1200 MHz・50.99 fps（C1E は電圧を下げる、zedBSD の HLT の C1 とは違う）。GPU の driver の範囲の外の依存（Q1 に報告済み）。
- VS の code: 134 命令（ANV 約 50）。MAD 無し、push の scalar を mov で広げる、VUE の 16 register を 0 で埋める、全命令が SWSB `@1`。

### Notes の起動（2026-10-31 Q1 の依頼、ユーザーの指示「5330 で Notes が起動できるかを確かめておく」の趣旨）

- 5330 の NVMe の zedBSD（main 48a27d158 の image、kernel は p022 の MAD 入りの build）の desktop の session で、root から `su user -c '... /bin/notes'` で起動: 窓が出た（keiland-shot の PNG、build/p022-meas/shots/notes.png、git の外。「note-20261030-161726.pdf — Notes」の窓、道具の列と白い page）。notes の log: `NOTES START width=1024 height=768`、`NOTES FRAME first draws=52`。
- 終わり: SIGTERM で終わった（exit=143、signal 15 の既定の終わり方）。dmesg に fault・trap の行は無し。**窓の閉じる button・File の終わり（`NOTES EXIT` の行の出る通常の終わり方）は未実施**（この image に試験の入力の注入 `CONFIG_INPUT_TEST_INJECT` が無い）。

### CPU の idle の MWAIT（WS333 p002、q1735）の後

- WS333 p002（commit e44a18dd7、`hal_cpu_idle` を MWAIT C6 に）を足した kernel（p022 の GPU の変更すべて＋MWAIT、診断なし）: c1 265.6 fps、**c600 32.52・32.64 fps**（direct scanout）。c600 の shot は ANV と 1 pixel 1 階調差。idle の package 13.00 → 11.70 W、c600 の GT 800〜850 → 850〜900 MHz（WS333 p002 の表）。

## 状態と再開点（2026-10-31、P1 の q1731-i01 の終わり）

- 受け入れ（qs40 × 600、1920x1080、direct scanout で Linux の 50.6 fps 以上）: **未達**（32.6 fps）。
- 主因: package の電力の上限（PL1 13 W）で GT が 850〜900 MHz（Linux 1100〜1200 MHz）。zedBSD の idle が 11.6 W（Linux 3.2〜3.5 W）で枠を食う。同じ周波数に換算した GPU の前段（頂点）の時間は Linux と約 8% の差。
- 次に効く物（効く順の見込み）: (1) WS333 の package の省電力の続き（P-state・HWP、tick、ASPM・PSR など、idle 11.6 W の内訳の調査）。(2) GPU 側: 非同期の submit（frame あたり GPU の空き約 1 ms）、present の GPU の copy（1.2 ms/frame、真の flip に）、HiZ、texture の tiling、FS の SIMD16、SWSB の依存の解析（全命令 `@1`）。
- 5330 の後片付け: `5330-mview.sh restore` で NVMe の zedBSD を退避の時の物に戻した（結果は下）。Debian の `~/p1-p022/`（退避 orig 約 6.6 GB、image、linux-mview、src）は残してある（消すのは Q1）。weston・seatd を Debian に入れた（apt）。
- 戻しの結果（2026-10-31）: p5・p6 は退避の sha256 と一致、p4 の file（vmunix 3510016546・vmunix.old 2965513653・zedbsd.cfg・logo.ppm・元の log）と ESP の `\EFI\zedbsd\BOOTX64.EFI`（1896835601）も退避の cksum と一致。`restore` は FAT への `cp -a` の owner の保存で失敗して途中で止まったので、残り（p4 の写し・umount・loader）は手で同じ事をして照合した（script は `cp --preserve=timestamps` に直した）。**p4 に残った物**（消していない、削除は Q1）: `log/boot-000137.txt`〜`boot-000157.txt`（この作業の起動の log）と `vmunix.prev`。5330 は Debian で起動中、BootNext は無し。
