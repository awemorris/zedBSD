<!-- awesome-plan project=zedbsd record=ws291-p021 -->

# ws291-p021: Raspberry Pi 5（V3D 7.1）で重い 3D の model を、スペックの比で決めた目標の fps に

Status: in-progress（2026-10-30 23:50 P4: 試験の image を作った、実機は未起動。Q1 の判断で実機を待つ間に mview の要る欠けを host で実装）
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

## 道具（この Phase で足した物）

- `plan/ws291/tests/config-rpi5.mk`: CI の `config/ci/config-rpi5.mk` に `CONFIG_DRIVER_BCM2712_GPU=y` と mview（qs40 は package の `/usr/share/mview/qs40`）。
  en0 を DHCP（`net-rpi5.conf`）、root の `authorized_keys` に guest harness の鍵（`plan/tmp/guest/id_ed25519.pub`）、固定の SSH の host key（`ZEDBSD_EXTRA_FILES`）。
  keiland-shot は rpi5 の package に無い（platform の list が amd64・rpi4・visionfive2）ので入れていない。
- `plan/ws291/tests/rpi5-image.sh BUILD`: 上の config で `disk-image`（`flock /tmp/zedbsd-image-build.lock`、make の stdin は `/dev/null` で firmware の license の問いに答えない。
  共有の cache の rpi5-boot・bcm43455・brcmbt は受け入れ済み）。
- `plan/ws291/tests/rpi5-image-boot.sh BUILD OUT`（`FALLBACK=KERNEL`・`CMDLINE=`）: BUILD の hdd-image.img を OUT に写し、FAT の区画（mtools）の
  config.txt の framebuffer を 1920x1080 に（CI は 1280x720。HVS6 の driver は firmware の plane の大きさを画面にする）、cmdline.txt を `login=console v3d.perf=1` に、
  FALLBACK を `vmunix-fallback` として置く。
- `plan/ws291/tests/rpi5-mview.sh OUT`（`PI5=`・`COPIES=`・`SPIN=`・`MVIEW_ARGS=`・`PROGRAMS=BUILD`）: 板を MAC 98:fe:54:22:65:c3 で 10.0.30.0/24 から探し、
  uname で zedBSD を確かめ、GPU が止まっていれば `sysctl hw.gpu.start=1`、stage の行（QPU・clock）を残し、compositor を自分の socket で起こして
  COPIES ごとに mview を game・mailbox・4 枚で回し、kernel の行（`v3d: perf`）と compositor の scanout の行を集める。電源・再起動はしない。

## 記録

### 試験の image（2026-10-30 23:50 P4、q1729-i01）

- tree: agent/p4-ws291pi e00f72140（src は main 20d472f25 と同じ）。`sh plan/ws291/tests/rpi5-image.sh build/ws291-p021` exit 0、zedBSD の source の compiler の warning 0
  （外部 package の openssl・openssh の warning は除く）。`Raspberry Pi 4 image check: PASS`・`ufs-root.img: canonical zedBSD UFS OK`。
  arm64 の sysroot は自分の worktree の `build/arm64/sysroot` に `make … sysroot-arm64` で作った（LLVM の build は無し）。
- 戻しの kernel: WS331 p007 の前の main 627bd83ff を `git archive` で `build/tmp/k627` に出し、同じ config で `build/rpi5/vmunix`（exit 0、warning 0、cksum 2365958026）。
- `FALLBACK=build/tmp/k627/build/rpi5/vmunix sh plan/ws291/tests/rpi5-image-boot.sh build/ws291-p021 build/ws291-p021/rpi5-p021.img`:
  `/home/awe/zedBSD-worktrees/p4-ws291pi/build/ws291-p021/rpi5-p021.img`、321912832 byte、sha256 cddc266e014800728b47f127c71d455d52e0175024a611b49e791c723edeafd6。
  FAT: vmunix（cksum 3796828842）・vmunix-fallback・2 つの dtb・config.txt（`kernel=vmunix`、1920x1080）・cmdline.txt（`login=console v3d.perf=1`）。
- 書き込み: `sudo dd if=<image> of=/dev/sdX bs=4M conv=fsync status=progress`。起動しない時は別の PC で FAT の config.txt の `kernel=vmunix` を `kernel=vmunix-fallback` に。
- 未確認: Pi 5 の実機での起動（今の main の kernel は WS331 p008 以降、Pi 5 で起動を確かめていない。627bd83ff の kernel も未確認）。

### 実機の前に source で分かった欠け（2026-10-30 P4、host で読んだだけ、未確かめ）

V3D の Vulkan の実行器（`src/drivers/gpu/bcm2711/vk/`、Pi 4 と共有）は Keiland の compositor の quad の形だけを受ける。mview に足りない物:

1. index の draw: `vkCmdBindIndexBuffer`・`vkCmdDrawIndexed` は record の検証（`vulkan-record-validate.c`）で ENOTSUP。
2. instance: instance の数 > 1 は ENOTSUP、instance rate の頂点の入力は `vulkan-native-draw.c` の `resolve_fetch` が EINVAL。
3. mip: `mipLevels != 1` の image は `vulkan-resource.c` が拒否。RGBA8 に BLIT_SRC/DST を報告しているので mview は mip を作ろうとする。
4. 性能の構造: 毎 draw の頂点を CPU で詰め直して写す（`prepare_fetch`）。user の buffer の GPU の address を attribute の record に直に書けば要らない。
5. 性能の構造: sample する image は raster のまま持ち、pass ごとに CPU で tile の形へ写す（`prepare_texture_image`、perf の行の `tex`）。

PowerVR（p020）と同じく、1〜3 を入れるまで「今の fps」は測れない（mview の失敗の記録になる）。
