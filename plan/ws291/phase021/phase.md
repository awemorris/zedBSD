<!-- awesome-plan project=zedbsd record=ws291-p021 -->

# ws291-p021: Raspberry Pi 5（V3D 7.1）で重い 3D の model を、スペックの比で決めた目標の fps に

Status: in-progress（2026-11-01 02:10 P4 q1784-i04 で再開: ユーザーの決定で Pi 4（p025）から Pi 5 へ。4.2 の成果を 7.1 に移し、1920x1080 の image を作って Q1 が SD に書いた。実機の計測はこれから）
Parent: [WS291](../ws.md)
Queue: q1729（P4）、q1730（P4、2026-10-30 再起動）、q1784-i04（P4、2026-11-01、p025 から移る）

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

## 記録（P4 q1784-i04、2026-11-01 01:52〜）

- 2026-11-01 ユーザーから（Q1 経由）、Pi 4 は熱の throttling が疑われるので Pi 4 の作業を止め、冷却が良く GPU が近い Pi 5 でこの Phase を進める、
  4.2 の成果を 7.1 に当てはまる所で移す、desktop も Pi 5 で 60 fps を目標にする、という趣旨の指示を受けた。
- 目標の copies（スペックの比、実機の dmesg で確定する前の見込み）: FP32 ＝ QPU 数 × 8（4 lane × 積と和）× clock。12 QPU・1.15 GHz なら 110 GFLOPS
  （RTX 2070 の 7.46 TFLOPS の 1/68 → 5000 / 68 ≈ 74 copies）、960 MHz なら 92 GFLOPS（1/81 → 約 62 copies）。参考に Pi 4（8 QPU・500 MHz、32 GFLOPS）は 21。

### 4.2 の成果の 7.1 への移し（branch agent/p4-ws291pi、Pi 4・Pi 5 の vmunix は warning 0、実機は未実行）

- 2a53a5a6d: v3d.perf=1 の 2 s ごとの report の後に `v3d: board arm=設定/実測 core= v3d= MHz temp= throttled=0x…`（firmware の mailbox。Pi 4 と Pi 5）。
- 04609a6c0:
  - VCM: 7.1 も VPM の大きさから batch の数を選ぶ（4.2 と同じ式、Mesa の vcm_cache_size。今までは 0x22 の固定）。
  - TFU: job の語を `bcm2711_native_conversion_words` に分けた（4.2 は V3D33、7.1 は V3D71 の field。Mesa の v3d_tfu.h・v3dx_tfu.c）。4.2 の語は前と同じ。
  - 7.1 の self-test（V7）に raster → strict UIF の照合（RGBA8 の 5 形、R8・RG8 の各 1 形、見える texel を CPU の配置と 1 つずつ比べ、出力の後ろの guard も）。
    全て合えば 7.1 でも texture の変換を TFU で行う（今までは 7.1 は常に CPU）。合わなければ CPU のまま、v3d.tfu=0 でも CPU。
  - mip: tree の配置を共有の native-miptree.c に移し（7.1 の ENOTSUP の stub を外した）、7.1 の texture の record と sampler に level の数・max LOD・mip の filter。
  - uniform の stream（7974d3365）は共有の vk の code なので 7.1 に既に効く。
- ee3e43a88: 32-bit の index が全て 16 bit に入る時、16-bit の写しを snapshot（buffer の世代で持つ、host に map された buffer は対象外）として持ち、
  bin はそれを読む（instance ごとに index を読み直すので、bin の index の読みが半分）。入らない index（0xffff 以上）はその世代の間は試し直さない。
  Pi 4 と Pi 5 の両方に効く（共有の code）。
- 道具: `rpi5-mview.sh` に KERNEL=（/boot/boot0/vmunix へ、元は vmunix.p021old、再起動）と run ごとの最後の `v3d: perf`・`v3d: board` の行と
  board-range（ARM の実測 MHz・温度・throttled）。`rpi5-desk.sh`（pi4-desk.sh の Pi 5 版、MAC で板を探す、既定は CONSOLE=1、board の行）と
  `config-rpi5-desk.mk`（入力の注入と aat-input）。

### 試験の image（2026-11-01 01:55〜02:05）

- `sh plan/ws291/tests/rpi5-image.sh build/p021-img` rc 0（`Raspberry Pi 4 image check: PASS`・`canonical zedBSD UFS OK`、warning は外部 package の openssl・openssh と
  perl の locale だけ）、`rpi5-image-boot.sh build/p021-img build/p021-img/rpi5-p021.img`: **322961408 byte、sha256 3bf8f40c636ed8397b0355182131441399bb1e7fa9dc181d7f79da3b9ebf0c75**。
  kernel は 2a53a5a6d の source（FAT の vmunix cksum 130421877、image から取り出して確かめた）、config.txt 1920x1080、cmdline `login=console v3d.perf=1`、fallback 無し。
- Q1 が SD（もと Pi 4 のカード、ユーザーの許可）に書き、image と cmp で一致。

### 実機（Pi 5、10.0.30.7、2026-11-01 02:16〜）

- 02:16 板が起きた（zedBSD ce2d6f4、image の kernel cksum 130421877）。`sysctl hw.gpu.start=1` の後の dmesg: `V3D 7.1.10, 12 QPUs, 1150000000 Hz`
  （V0 は 500 MHz、最大 1150 MHz、V1 で 1150 MHz に上げる）、VPM 64 KiB、self-test は全て ok。rpi5-fan 29.25 °C・fan 0。
  **目標の copies は 74 に確定**（12 × 8 × 1.15 GHz ＝ 110 GFLOPS、RTX 2070 の 7.46 TFLOPS の 1/68、5000 / 68 ≈ 74、Q1 が受け入れた）。
- 表示が無い: firmware が `bcm2708_fb.fbwidth=640 fbheight=480` を渡し、kernel に framebuffer が無い（rpi5.framebuffer・P0 の行が無い）ので display を
  見つけず、compositor は `KWL VULKAN_ERROR operation=display result=-3` で止まる（build/tmp/p021-b0）。HDMI の画面が繋がっていない時の形。
  Q1 がユーザーに画面を繋ぐよう頼んだ。繋いだ後に再起動（SwitchBot 14C19FA80F86 は P4 が入れ直してよい、Q1）。
- 02:18 `KERNEL=build/p021-k5b/vmunix`（ee3e43a88、cksum 4200268633）を rpi5-mview.sh で置いて再起動（元は /boot/boot0/vmunix.p021old）。
  7.1 の TFU の UIF の self-test が実機で通った: `V7 TFU UIF 96x40 x4`・`33x9 x4`・`1x1 x4`・`200x150 x4`・`256x255 x4`・`70x37 x1`・`35x19 x2` の
  全てで見える texel が CPU の配置と一致、`texture conversion uses the unit`（build/tmp/p021-k1）。mview・mip・16-bit の index は表示が無いので未実行。

## 記録（P4 q1784-i05、2026-11-01 02:25〜）

- main 44ca7739c を取り込んだ（merge 342f00101、親 f8a620b21 44ca7739c、競合なし）。
- 2026-11-01 ユーザーから（Q1 経由）、Pi 5 の firmware の画面は HDMI の monitor に出ていたので monitor は繋がっている、「monitor が無い」の結論は誤り、原因を card の設定か kernel の framebuffer の求め方で調べる、という趣旨の指示を受けた。

### Pi 5 で framebuffer が無い原因（2026-11-01 02:30〜03:05、実機 10.0.30.7）

- 道具: kernel の 7.1 の attach が boot の framebuffer を見つけない時の報告（`bcm2712_screen_report`、`bcm2712_firmware_message`）。firmware に display の数と HDMI 0 の EDID を問い、
  firmware 自身の log（tree の `/chosen/log` の場所、0x3ff23000・0x80000 byte）の display の行を出す。何も変えない。`build/p021-k5c/vmunix`（warning 0）。
- 調べの途中で確かめた事（同じ板、kernel の報告で）: firmware の答えは GET_PHYSICAL 0x0、display の数 0、EDID block 0 は失敗（応答 0x80000001）、HAL と同じ set-up の要求も失敗（0x80000001）、
  tree に simple-framebuffer は無い。HVS の channel 0〜2 は全て無効（電源の入れ直しの後は ch0 に 640x480 の設定が残り無効）。firmware は 2026-05-11 の bootloader（66f33f7e）。
- **原因**: firmware の log（電源の入れ直し・SwitchBot 14C19FA80F86 と再起動の両方で同じ）:
  `Select resolution HDMI0/2 hotplug 1 max_mode 2` → `BSC_A no ACK`（×2）→ `HDMI0: Unable to read EDID block 0`、`Select resolution HDMI1/2 hotplug 0`。
  monitor は HDMI0 に繋がり hotplug は立つが、DDC（EDID を読む I2C）に monitor が応答しない。Pi 5 の firmware は EDID の無い display に OS の framebuffer を作らない
  （bootloader の診断の画面は EDID 無しの 640x480 で出るので、ユーザーが見た画面と矛盾しない）。WS279（2026-10-22）は同じ板で 1920x1080 の plane が firmware にあった。
- config.txt の試し（元は `/boot/boot0/config.txt.p021orig`、全て元に戻した）: WS279 と同じ形（pciex1 無し・1280x720）、`hdmi_force_hotplug=1`、`hdmi_force_hotplug:0=1`＋`:1=1`＋`hdmi_group=1`・`hdmi_mode=16`、
  `hdmi_ignore_edid=0xa5000080`＋group・mode、`hdmi_edid_file=1`（と `:0`・`hdmi_edid_filename:0=edid.dat`、1080p60 の EDID を自作して /boot/boot0/edid.dat に置いた）は、
  どれも firmware の log が同じ（EDID を読めない、display 0）。Pi 5 の firmware はこれらを使わない。
- 結論: card の設定・image の script・zedBSD の kernel の不具合ではない（BUG は立てない）。monitor 側の DDC（cable・変換・切替器・monitor の入力）の問題で、設定では直せない。
  直すには、EDID を返す接続（cable・micro HDMI の変換の交換、切替器を外す、monitor の電源と入力）が要る。ユーザーの手の作業。
- 板の状態: /boot/boot0/vmunix は build/p021-k5c/vmunix（cksum 1329488160）、元は vmunix.k5b（ee3e43a88）・vmunix.p021old（image の物）。config.txt は元のまま。
  /boot/boot0/edid.dat（128 byte、試しの物、firmware は読まない）が残る。

### 画面の無い原因の続き（2026-11-01 03:00〜03:15、Q1 経由のユーザーの仮説ごとに確かめた。全て SwitchBot の電源の入れ直しの後）

- ユーザーが LCD の電源を入れ直した後も同じ（`BSC_A no ACK`・`HDMI0: Unable to read EDID block 0`、display 0）。
- 起動の設定の仮説: config.txt を丸ごと置き換えて試した（元は config.txt.p021orig、試しの後に戻した）。
  (1) WS279 の時の config（git b7d9a75ba の platform/arm64/config-rpi5.txt、今との差は `dtparam=pciex1` だけ）、(2) Raspberry Pi OS の Pi 5 の既定（vc4-kms-v3d・max_framebuffers=2・disable_fw_kms_setup=1・display_auto_detect=1 ほか）＋kernel の行、
  (3) kernel・kernel_address・pciex4_reset だけ。どれも firmware の log は同じ。firmware は HDMI を config の dtb・cmdline・kernel を読む前に調べる（log の順: Select resolution → dtb_file → cmdline → Starting OS）。
- firmware の取り違えの仮説: FAT は dtb 2 つ（今の pin、10-21 からの物と byte で同じ）・COPYING.linux・config・cmdline・vmunix・data.img・swapfile・log/ と私の backup だけ。start*.elf・overlays・pieeprom.upd・recovery.bin は無い。
  bootloader は 66f33f7e（2026-05-11、update-timestamp 0）。10-22 の版は記録に無く比べられない（未確認）。image の script は rpi5 に Pi 4 の firmware を入れない。
- ユーザーの以前の image（/home/awe/rpi5.img、読むだけ）: dtb 2 つ・COPYING.linux は今と同じ、config.txt は WS279 の物（上の (1) で試した）、cmdline は `login=console`、違うのは vmunix（2026-10-22 の build、7536640 byte）だけ。
  この kernel は板で起動していない（今の rootfs で SSH が上がらないと FAT に戻れない危険、Q1 に判断を返した）。
- Q1 の判断で bug-analyzer が板を使う（2026-11-01 03:19）。P4 は板の使用を止めた。

### 板の状態（2026-11-01 03:19、P4 が手を離した時、10.0.30.7、鍵 /home/awe/zedBSD-worktrees/p2-ws309v2/plan/tmp/guest/id_ed25519 または この worktree の plan/tmp/guest/id_ed25519）

- /boot/boot0/vmunix: build/p021-k5c/vmunix、cksum 3716769917（tree fba292f54 = main 44ca7739c ＋ agent/p4-ws291pi。画面が無い時の報告 `rpi5-screen:`、bin の overflow の大きさ、7.1 の性能 counter を含む）。
- /boot/boot0/vmunix.k5b: build/p021-k5b/vmunix、cksum 4200268633（ee3e43a88）。/boot/boot0/vmunix.p021old: image の kernel、cksum 130421877（2a53a5a6d）。
- /boot/boot0/config.txt と config.txt.p021orig: 同じ（image の物、1920x1080、`dtparam=pciex1`）。cmdline.txt と cmdline.txt.p021orig: 同じ（`login=console v3d.perf=1`）。
- /root/p021/bin/mview-off（--offscreen の付いた mview）と /root/p021/qs40（model）。
- 入れ替え: `scp <vmunix> root@10.0.30.7:/boot/boot0/vmunix.new`、板で cksum を確かめて `mv /boot/boot0/vmunix.new /boot/boot0/vmunix && sync`、`(sleep 1; /sbin/reboot) &` か SwitchBot `python3 -I plan/tools/switchbot.py cycle 14C19FA80F86`。
  戻し: 板で `cp /boot/boot0/vmunix.k5b /boot/boot0/vmunix`（または vmunix.p021old）。config・cmdline の戻しは *.p021orig を写す。
- 画面の調べ: 今の kernel は boot の framebuffer が無い時に dmesg へ `rpi5-screen: no firmware framebuffer; displays N (..), HDMI 0 EDID ...` と firmware の log（/chosen/log、0x3ff23000）の display の行を出す。

### offscreen の mview と c74（2026-11-01 02:50〜03:15）

- mview に `--offscreen [--shot=FILE.ppm] [--zoom=N] [--look=XxY]`（1ec25b5d1、window と compositor 無しで自分の image に描き、最後の frame を読み戻す）。`plan/ws291/tests/rpi5-offscreen.sh`（TFU=0|1 は cmdline の v3d.tfu=0 と再起動、qs40 を /root/p021/qs40 へ写す。arm64 の image に mview の package の data と /bin/mview が無いため）。
- 1920x1080、qs40（37000 三角形）、kernel k5c: c1 2.77〜3.03 ms、c21 9.43 ms（v3d.tfu=0 では 15.95 ms）、c62 19.2 ms、c74 21.6〜22.9 ms（43.7〜46.3 fps、目標 50）。絵は正しい（build/tmp/off-k5c/c74.png ほか）。
- TFU と CPU の変換の比較: 枠の全体では細部だけ違う（TFU は mip tree を sample、CPU は level 0 だけ、設計どおり）。zoom 10 で bit 一致（dbaae78e）、zoom 6 の服の絵で 748 px・最大 11。
- c74 は bin の overflow（256 KiB × 4 の固定）が足りず ENOMEM だった → e80f836e5: 4 つの pool の大きさを pass の頂点・index × instance × 2 byte から（256 KiB 単位、1 つ最大 16 MiB）。4.2・7.1 共有。c1・c21 の checksum は変わらない。
- --inflight=2 は効かない（kernel は bin と render を順に流す）。
- 7.1 の性能 counter（fba292f54、Mesa 25.3.6 の 7.1 の表、register は 4.2 と同じ）。c74 の定常（88 run）: bin 14.73 M cycle（12.8 ms）、render 9.9 M cycle（8.6 ms）。
  bin の QPU active は 19.3 M（12 QPU で 11 %）、stall は少ない → bin は shader でなく前段（頂点の取り込み・PTB）で詰まる。binned 2.74 M、reversed 2.11 M。
- 次の手（未着手）: coordinate shader が位置に要る attribute だけを読む（今は CS も VS と同じく全ての attribute を全成分読む: 位置 3・法線 3・uv 2・instance 4 の 12 語、要るのは 7 語）。compiler の CS の入力を使う成分に絞り、attribute record の coordinate_values を location ごとに。
