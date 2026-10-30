<!-- awesome-plan project=zedbsd record=ws291-p021 -->

# ws291-p021: Raspberry Pi 5（V3D 7.1）で重い 3D の model を、スペックの比で決めた目標の fps に

Status: in-progress（2026-10-31 00:06 P4 q1730-i01: mview の要る欠け 1〜4（index の draw・instance・直の fetch・mip の image（sample は level 0））を実装、Pi 4・Pi 5 の build と host 試験まで。実機は未起動、mview は未実行）
Parent: [WS291](../ws.md)
Queue: q1729（P4）、q1730（P4、2026-10-30 再起動）

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

### 照合の file と基準の host 試験（2026-10-30 P4、q1730-i01）

- 2026-10-30 ユーザーから、P4 に plan/ws141/temp/ の利用（Mesa（MIT）の照合用の file を置き、V3D の host 試験を compile・実行する）を許可する、という趣旨の指示を受けた（ws.md・guardrail.md の記録）。
- 置いた物（この worktree の `plan/ws141/temp/mesa/src/broadcom/`、git に入らない）: qpu/qpu_instr.c（f61745f3…）・qpu_instr.h（4b1b90e3…）・qpu_pack.c（abf43600…）（Mesa 25.3.6、前の P4 が `build/tmp/mesa-dl/` に取得、script の固定の sha256 と一致）、
  common/v3d_device_info.h（Mesa 25.3.6、`https://gitlab.freedesktop.org/mesa/mesa/-/raw/mesa-25.3.6/src/broadcom/common/v3d_device_info.h` をパラメータ無しの GET、
  sha256 27f8e9adbd7710eb3927ddd8d9789926503497b383fb728b3cf71b1ca39c6c68）、cle/v3d_packet.xml（b13f995b…）。
- 基準（main 48a27d158）: `qpu-format-host-test.sh` PASS、`shader-host-test.sh` PASS、`glass-shader-host-test.sh` は FAIL（assert `largest < 1e-5`、差 0.998）。
  10-28 の ab12d4f70 の tree でも同じ FAIL。原因: 試験の参照（ws211-p011 の前の glass を C で評価した物）が、10-22 に意図して見た目を変えた panel.frag に追従していない。
- 直し（321042a29、Q1 の指示「試験の古さは見つけた担当が直す」）: 正とする物は、native の program（語は Mesa の decoder で読む）と SPIR-V の source の解釈の bit 一致。
  pre-p011 との差は情報として表示するだけにした。結果: 3856896 fragment で bit 一致、PASS。

### driver の欠けの実装（2026-10-31 P4、q1730-i01）

変えた所は `src/drivers/gpu/bcm2711/`（Pi 4 と共有）と `include/uapi/gpu-op.h` の comment だけ。HAL は変えていない、hal_* は使っていない。

- 6334ae147（欠け 1・2・4）:
  - record: `GPU_OP_CMD_BIND_INDEX_BUFFER`（INDEX_BUFFER の usage、UINT16・UINT32、offset の整列）・`GPU_OP_CMD_DRAW_INDEXED` を受ける。DRAW の instance の数・first instance の制限を外した。
    draw の状態に index の buffer、prepared event に index と `draw[5]`。index の範囲は record の walk で buffer の中か確かめる。
  - pipeline: `VK_VERTEX_INPUT_RATE_INSTANCE` を受ける。attribute の record に instance divisor（instance rate は 1）。
  - fetch: user の draw は各 location を user の vertex buffer から GPU が直に読む（address・stride・divisor、maximum index は buffer の中の最後の要素、24 bit で止める）。
    copy しない。shader の読む成分が format より多い・address か stride が 4 の倍数でない location と meta の quad は今の packed の copy（1 instance の vertex array だけ）。
  - BCL: draw ごとに 155 byte = 状態 123 byte＋draw（43 Base Vertex Base Instance、index なら 44 Index Buffer Setup と 32/34、無ければ 36/38、1 instance は Mesa と同じく instance 無しの packet）、余りは NOP（1）。
    4.2（native-bin.c）と 7.1（native71-bin.c）で同じ。
  - host 試験 `plan/ws291/tests/bin-draw-host-test.sh`（新）: 両 encoder の列を Mesa の v3d_packet.xml で packet ごとに decode し、状態 23 packet・draw の field・NOP・拒否・attribute の divisor を確かめる。PASS（4.2・7.1）。
- 2a4dc413a（欠け 3、第 1 段）: mip の image。
  - 作り: OPTIMAL の RGBA8・BGRA8 だけ mipLevels を受ける（最大 13）。level 0 は今の raster のまま、小さい level はその後に raster で並ぶ（pitch は 64 byte の倍数）。必要な byte は全 level。
    image の format の問い合わせは OPTIMAL の RGBA8・BGRA8 で maxMipLevels 13。
  - layout は level ごと（level 0 は今の `layout`）。barrier・buffer↔image の copy（CPU の行の copy）・blit・copy image が level を受ける。
    blit は level を 1 level の image として扱う（`bcm2711_vulkan_image_level_view`）。同じ image の別の level の間の blit も受ける（mview の mip の作り方）。
  - view は level 0 から複数の level を持てる。sample は level 0 だけ（hardware の mip の sample は未実装）。
- 確かめ: Pi 5（`plan/ws291/tests/config-rpi5.mk`、`build/p021-k5/vmunix`）と Pi 4（`config/ci/config-rpi4.mk`、`build/p021-k4/vmunix`）の vmunix が exit 0・warning 0（両 commit の後）。
  host 試験: bin-draw-host-test PASS。compiler の host 試験（qpu-format・shader・glass）は compiler を変えていないので基準のまま。
- 未実施: 実機（Pi 4・Pi 5）、mview の実行、Keiland の desktop の回帰。
- 注意（Q1 へ報告済み）: Pi 4 の compositor の draw も直の fetch に変わった（前は FIFO の準備の時に vertex を写していた。今は GPU が実行の時に user の buffer を読む）。
  Q1 は Pi 4・Pi 5 の実機で desktop の回帰を確かめるまで main への merge を保留（2026-10-30）。

### 再開点（2026-10-31 00:06 P4、q1730-i01）

- 次の段（未着手）:
  1. 実機（Q1 の連絡の後）: image を今の tree で作り直す（`sh plan/ws291/tests/rpi5-image.sh BUILD`、`rpi5-image-boot.sh`）。desktop の回帰（Pi 5、可能なら Pi 4）、clock・QPU を確かめて copies を確定、
     `plan/ws291/tests/rpi5-mview.sh` で mview の fps。mview が失敗したら kernel の行（`v3d:`）と mview の stderr で欠けを特定する。
  2. mip の第 2 段（hardware の mip の sample）: 縮小した texture を level 0 で sample すると TMU の cache を浪費する。TFU で各 level の raster を miptree（Mesa の v3d_setup_slices の配置、
     level ごとの tiling）へ変換し、texture の record に level の数と base の level を入れる。sampler の mipmap mode・LOD を texture の record に。
  3. 欠け 5（sample する image を pass ごとに CPU で tile へ写す件）は perf の行の `tex` を実機で見てから。
- 既知の制限: index の draw は直の fetch でしか動かない（packed の copy は index を CPU で読まないため）。primitive restart は off。

- 2026-10-31 ユーザーから、Pi 5 の試験は今日の午後に行い、その時の main で最新の image を作り直して実施する、Q1 が声をかける、という趣旨の指示を受けた。

### 実機の前の準備（2026-10-31 02:00〜02:45 P4、Q1 の指示「main の最新＋agent/p4-ws291pi の image を build だけ、main への merge はしない」）

- main d26c657c7 を agent/p4-ws291pi に取り込んだ（merge e2feb0871、親 e4f77b9e5 d26c657c7）。競合は plan/ws291/ws.md の p021・p022 の行と、この phase.md だけ（source の競合なし）。
- vmunix: Pi 5（`plan/ws291/tests/config-rpi5.mk`、`build/p021c-k5`、cksum 2551665408）と Pi 4（`config/ci/config-rpi4.mk`、`build/p021c-k4`、cksum 1523912276）が exit 0・warning 0。
  host 試験 `plan/ws291/tests/bin-draw-host-test.sh` PASS（4.2・7.1）。
- 試験の image: host の / が満杯で worktree の build は失敗（clang の No space left on device）、Q1 の指示で BUILD を /tmp（tmpfs）に置いた。
  `sh plan/ws291/tests/rpi5-image.sh /tmp/p4-p021c` exit 0（`Raspberry Pi 4 image check: PASS`・`ufs-root.img: canonical zedBSD UFS OK`、zedBSD の source の warning 0、
  外部 package の openssl・openssh の warning は除く）、`FALLBACK=build/tmp/k627/build/rpi5/vmunix sh plan/ws291/tests/rpi5-image-boot.sh /tmp/p4-p021c /tmp/p4-p021c/rpi5-p021.img` exit 0。
  **`/tmp/p4-p021c/rpi5-p021.img`、321912832 byte、sha256 dcadd719d98e94b4972a55adcf467678daa284e328882954b2c8f9342a3bbcc9**（tree e2feb0871、vmunix-fallback cksum 2365958026、
  config.txt `kernel=vmunix`・1920x1080、cmdline `login=console v3d.perf=1`）。tmpfs なので host の再起動で消える。git と worktree には写していない。SD への書き込みは Q1。
- 未実施: 実機（Pi 5・Pi 4）の起動・desktop の回帰・mview。
