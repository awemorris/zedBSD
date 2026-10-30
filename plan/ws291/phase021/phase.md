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

### 再開点（2026-10-31 P4 のラップアップ、q1729-i01）

- Q1 の判断（2026-10-30）: 実機を待つ間に上の 1〜4（index の draw、instance の数と instance rate、mip の image、直の attribute の address）を host で実装してよい。
  条件: driver と Vulkan の backend の中だけ・HAL を変えない・hal_* を使わない、Pi 4 と共有の code なので Pi 4 を壊さない（Pi 4 の build と既存の host 試験）、改善ごとに commit と報告、実機は Q1 の連絡の後。
- 実装の進み: source は未変更（読んだだけ）。commit は e00f72140（道具）・ad8da825c（記録）。
- host 試験: `plan/tools/v3d-qpu/shader-host-test.sh`・`qpu-format-host-test.sh`・`glass-shader-host-test.sh` は `plan/ws141/temp/mesa/src/broadcom/` の Mesa の file
  （git に入らない oracle）が要り、どの checkout にも無い。Mesa 25.3.6 から取得した物（読むだけ、未実行）: `build/tmp/mesa-dl/`（この worktree）の
  qpu_instr.c f61745f35e5b34c74aad56b01afb9e068860ecb067361b8f9610dc1b673c8d4e、qpu_instr.h 4b1b90e3bb8ea614dea36484e24d83049392c55cf19b01125627fa5ff4c9d07f、
  qpu_pack.c abf436006dd3bed52cf1e77245d7267105a9100f02d8b3d643ddc6641fd6d551（3 つとも script の固定の値と一致）、v3d_packet.xml b13f995b1a4b606a1adcb2c048ff9bca067106be14e22ce1ee86e01055094e1b（packet の配置の参照）。
  `src/broadcom/common/v3d_device_info.h` も要る（未取得）。置いて試験を compile・実行する命令は auto mode の判定で拒否され、Q1 経由でユーザーの許可待ちだった（2026-10-30 Q1 から「許可が出た」）。
- 実装の設計（読んで決めた所）:
  - record: `vulkan-record.c` の dispatch・`vulkan-record-decode.c` に `GPU_OP_CMD_BIND_INDEX_BUFFER`（[buffer][u64 offset][u32 type]、objects[0]・offsets[0]・first=type）と
    `GPU_OP_CMD_DRAW_INDEXED`（5 word: indices・instances・first index・vertex offset・first instance）。`vulkan-record-validate.c` で INDEX_BUFFER の usage・offset の整列・
    UINT16/UINT32、DRAW の instance の数の制限（`words[1] > 1`・`words[3] != 0` の ENOTSUP）を外す。
  - 状態: `vulkan-draw.h` の draw_state に index の resource・offset・type、`vulkan-draw.c` の walk と `vulkan-draw-validate.c`（index の範囲・instance rate の attribute の範囲）、
    `vulkan-prepared.h` の event に index と `draw[5]`、`vulkan-prepared.c`、`vulkan-uniform.c:50`・`vulkan-native-pass.c:656`・`vulkan-native-draw.c:132` の `GPU_OP_CMD_DRAW` の判定を両方の draw に。
  - fetch: `vulkan-native-draw.c` の `prepare_fetch` を、user の buffer の GPU の address（`bcm2711_vulkan_resource_backing`、texture の TFU と同じく GPU が直に読む）・binding の stride・
    instance rate なら divisor 1（attribute の record の byte 6〜7）・maximum_index は buffer の範囲から計算（GPU が resource の外を読まない）にする。index の draw は直の形だけ
    （packed の形は index の範囲を CPU で読む必要がある）。shader の読む成分が format の成分より多い時は今の packed の形に残す。
  - BCL: draw の packet は 4.2・7.1 で同じ（36 Vertex Array Prims、38 Vertex Array Instanced Prims、34 Indexed Instanced Prim List、32 Indexed Prim List、44 Index Buffer Setup、
    43 Base Vertex Base Instance、1 NOP、0 は HALT）。`native-bin.h` の BCM2711_NATIVE_BIN_BYTES（133、末尾 10 byte が 36）を、状態 123 byte＋draw の 32 byte（44・43・34 の最大）に広げ、
    余りは NOP で埋める（0 で埋めると HALT）。pass の measure（`native-pass.c`・`native71-pass.c`）は draw ごとの固定の長さのまま使える。indexed・instanced では 43 を値付きで必ず出す。
  - mip: `vulkan-resource.c:278` の `mipLevels != 1` の拒否、copy・blit・sample の level の扱い（未調査）。RGBA8 に BLIT を報告しているので mview は mip を作る。
- 次の安全な地点: 上の record〜BCL を入れ、Pi 4（`config/ci/config-rpi4.mk` の build/arm64/vmunix）と Pi 5（`plan/ws291/tests/config-rpi5.mk`）の vmunix が warning 0、host 試験 PASS で commit。

- 2026-10-31 ユーザーから、Pi 5 の試験は今日の午後に行い、その時の main で最新の image を作り直して実施する、Q1 が声をかける、という趣旨の指示を受けた。
