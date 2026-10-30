<!-- awesome-plan project=zedbsd record=ws291-p020 -->

# ws291-p020 PowerVR（VisionFive 2）: model の 50 fps

- 親: [WS291](../ws.md)。
- 状態: in-progress（2026-10-30 16:50 P3、q1715-i02: 表示が出て、driver の欠け（instance rate・discard・vkCmdCopyBuffer・mip の transfer・direct scanout の BLOB）を
  埋めて mview の qs40 が描けた。目標の copies 13 は 26.6 fps（目標 50）。profile の途中。下の「q1715-i02」節）。
- 承認: ユーザー（2026-10-30 原文）「WS291をP7で実行します。P7はこのWSが終わったら返却してください。」。VF2 の割り当て: Q1（2026-10-30）「VF2 を P7 に割り当てます。」。
- 未決（ws.md）: 50 fps の基準（Linux との比較か、絶対値か）。VF2 の Linux の PowerVR の driver は今の板の SD に無い。Q1 がユーザーに (a) Linux の基準を立てる、
  (b) zedBSD の今の fps を起点に scene を決め profile で 50 以上、(c) 基準が決まるまで止め、を聞く。

## 道具（この Phase で足した物）

- `plan/ws291/tests/config-vf2.mk`: CI の `config/ci/config-visionfive2-keiland.mk` に mview。
- `plan/ws291/tests/vf2-mview.sh OUT BUILD`（`VF2_HOST`・`VF2_PASSWORD`（環境変数だけ、file に書かない）・`VF2_PLUG=14C19FA80F86`・`COPIES`・`SPIN`）:
  板の kernel を読み戻して vmunix.p7old に残し、BUILD の kernel を `plan/tools/vf2/run.sh` で入れて電源を入れ直し、program・library・qs40 の model を
  user の ~/p7 に置いて（板の system の file は変えず LD_LIBRARY_PATH で走らせる）、compositor を自分の socket で起こし、COPIES ごとに mview を game・mailbox・4 枚で回す。
  終わりに compositor を止め、板の kernel を戻して読み返しで照合、電源を入れ直す。注意: run.sh の既定のプラグ AC276E42B252 は今は RDNA 機なので VF2_PLUG を必ず付ける。
- mview（userland/tests/mview/renderer.c）: riscv64 の cycle の読みが無く build できなかった（`rdtsc` の asm）→ `rdtime`（kernel が scounteren.TM で user に許す）を足した。
  amd64 の mview は byte 一致（cksum 1110185251）、Linux の gcc -Werror の構文の確かめも通る。

## 記録

### 起点の計測の試み（2026-10-30 14:58〜14:59、build/ws291/vf2run/m1）

- build（main 5aca73738 + 上の mview の直し）: `make ZEDBSD_CONFIG=plan/ws291/tests/config-vf2.mk BUILD=build/ws291/vf2` の vmunix（cksum 2980506080）・
  bin/wayland・bin/mview・dynamic の 9 個の library、warning 0。
- 板の前の状態: zedBSD 5c83c16（10-21 の userland）、/boot/boot0/vmunix cksum 2825517684（10-29 の build）、zedbsd.cfg は kernel・rootpart の 2 行（login=graphical 無し、触らない）。
  新しい wayland と mview は板の古い ld.so で LD_LIBRARY_PATH の新しい library を使って起動する（usage の行を確かめた）。
- 結果: kernel は起動し PowerVR は `firmware=ok render-node=ok`、firmware の health check ok。しかし `visionfive2: no framebuffer (6)`・
  `powervr: no display: no boot framebuffer (6)`（U-Boot が framebuffer を作っていない: HDMI に画面がつながっていない見込み、未確かめ）で、compositor は
  `KWL VULKAN_ERROR operation=device result=-3`・`KWL COMPOSE unavailable errno=5` で終わり、mview は全 copies で `mview_window_open errno=6`。fps は 0 個。
- 戻し: 板の kernel を戻して読み返しで照合（2825517684、vmunix.p7old も 2825517684）、電源を入れ直して起動、zedbsd.cfg の login の行 0。
  板に残る物: /boot/boot0/vmunix.p7old（元の kernel の写し）と user kei の ~/p7（program・library・model、次の計測で使う）。

### 進めなくなった点（15:00 P7）

1. 板に表示が無い（boot framebuffer が無い）。mview は Wayland の compositor が要り、compositor は表示が要る。HDMI に画面（か dummy plug）をつなぐ人の作業が要る（KC705 には触らない）。
2. 50 fps の基準が未決（上）。
- 再開の手順: 表示がつながった後に `VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=… VF2_PLUG=14C19FA80F86 sh plan/ws291/tests/vf2-mview.sh OUT build/ws291/vf2`
  で copies 1・10・50・100・200 の fps を取り、基準の決定に沿って scene を決める。

### 表示が無い原因の切り分け（2026-10-30 15:05 P3、q1715-i01）

- 板の今の kernel は元の物（/boot/boot0/vmunix cksum 2825517684、uname は zedBSD 5c83c16 の userland）。SSH で読んだだけで、板の file・電源は変えていない。
- 元の kernel でも `visionfive2: no framebuffer (6)`・`powervr: no display: no boot framebuffer (6)`。/dev に gpu0 だけ（display の node が無い）。
- 板の /boot/boot0/log の起動の記録 32 個（boot-000118〜000149、2026-10-28 19:47〜）は全部 `no framebuffer (6)`・`no display: no boot framebuffer (6)`。
  10-28 の WS288 の GHCI の kernel（vmunix.p3orig）の頃から、どの kernel でも loader の framebuffer が無い → main の kernel の退行ではない。
- 経路: U-Boot の boot.scr → zedBSD の UEFI の loader（BOOTRISCV64.EFI）→ kernel。loader の framebuffer は U-Boot の GOP から来る。
  GOP が無い（U-Boot の HDMI の video が上がっていない）見込み: HDMI に画面（か dummy plug）がつながっていない・電源が切れている、が最もありそう（未確かめ: 板の HDMI の HPD を読む道具は無い）。
- 参考: kernel の自前の点灯（ws277-p005 の own mode）は動いている（`dc8200: own mode 1920x1080@60 clock 148500 kHz … fb 404b0000/7680`、HDMI PLL の未 lock の行なし）。
  しかし display の node（`src/drivers/gpu/powervr/display/display.c` の attach）は loader の framebuffer だけを見るので、own mode の画面では node が出来ず、compositor が動かない。
  画面をつながずに測る案（driver の側の変更、HAL は変えない）: loader の framebuffer が無い時に display の node を own mode の画面（dc8200 の own_screen の run）で attach する。範囲の外の新しい作業なので Q1 の判断を待つ。
- 判断（範囲の 1 のとおり）: 物理の接続の見込みが高いのでここで止め、Q1 に報告した。

### q1715-i02（2026-10-30 15:35〜 P3）

- 承認（2026-10-30 ユーザーの決定）: VF2 が空いているので PowerVR の最適化を P3 で実行してよい。HDMI をつないだので P3 を再始動し、必要なら VF2 を SwitchBot で入れ直してよい。
- 50 fps の基準（2026-10-30 ユーザーの決定、ws.md に記録）: Linux の RTX 2070 の scene-5000（約 50 fps）をカタログの FP32 の比で割った重さ＝qs40 × 13、1920x1080 で 50 fps 以上
  （600 MHz の時。400 MHz なら copies 9）。**板の GPU は 594 MHz**（dmesg `powervr: BXE 36.50.54.182 at 594000000 Hz`、
  firmware `core clock 594000000 Hz`）→ **目標は copies 13 で 50 fps**（1920x1080、mailbox 4 枚、vertex shading、direct scanout）。
- 表示: 15:35 に SwitchBot 14C19FA80F86 で入れ直した後、元の kernel で `visionfive2: framebuffer 1920x1080`・/dev/gpu1 が出た（HDMI の接続で解消）。

#### 計画に無い依存として足した物（discard は Q1 の判断 2026-10-30 で p020 の範囲に含めた）

mview が qs40 を描くのに driver に欠けていた物（どれも HAL は変えない、driver は hal_* を新しく使わない）:

1. compositor・mview を root で走らせる（display の node は root か owner だけが開ける: `gpu: open refused: uid 1000`）。vf2-mview.sh を直した。
2. instance rate の頂点の入力（mview の copies の位置）: `powervr: graphics pipeline: a state the executor does not take`。DDMAD の index を instance の temp（32-bit temp 1）から
   （Mesa の pvr_pds_generate_vertex_primary_program の divisor 1）。render-draw.c/h・data-program.c/h・render-job.c・render-program.c。
3. discard（OpKill・OpTerminateInvocation）: compiler の SPIR-V の reader が拒否（cutout.frag、qs40 の 16 material の 10 が cutout）。Mesa の pco と同じ形:
   kill を demote（discard の値を立てて続ける）、出力の前に POWERVR_IR_FEEDBACK →（TSTZ で p0 = discard が 0）＋ p0 で走る ALPHAF（vistest atst、ifb 0、0x82）＋ WDF、
   discard する pipeline は ISP の pass type を punch-through。shader-ir.h・shader.h（program.discards）・shader-spirv*.c・shader-backend.c・usc-encode.c/h・render-draw.c/h・vk-draw.c。
4. vkCmdCopyBuffer（mview の staging → device-local）: raw の 32-bit texel の transfer（行 1024 texel、矩形は最大 2048 行、余りは 1 行）。vk-transfer.c・vk-command.c・vk-queue.c。
5. blit と buffer↔image の mip level（mview の mipmap の生成）: level を独立の twiddled の画像として（level の端が padded の 2 の冪と合わない物は ENOTSUP）。vk-transfer.c。
6. direct scanout: PowerVR の display node は import と BLOB の present（check_direct_scan で zero copy）を持つのに output_query が GPU_DISPLAY_BLOB を出さず、
   Keiland の game mode が `KWL SCANOUT direct=0 reason=backend errno=21` だった → flag を足して `KWL SCANOUT direct=1` を確かめた（display.c）。
7. kernel の fatal（板で 2 回観測、16:2x、c1 の keiland-shot の後）: `fatal: supervisor fault vector=2 pc=…80045654`（powervr_stage_ok の spin_unlock の trap）と
   `pc=…8007c5f6`（spin_trylock の再帰の trap）。stage-log.c の stage_lock を thread から spin_lock（割り込みも preempt も止めない）で取っていて、clock の tick で
   preempt された holder が別の CPU で release して trap、または同じ CPU の別 thread が再帰と見なされて trap。spin_lock_irqsave に直した（kern/lock.c の spin_lock の注意書きどおり）。直した後は再現なし（観測は 4 run）。

- host 試験: `plan/tools/powervr/p010-compiler/run.sh` 3062 PASS・group 29 PASS（mview の 6 shader を IR の interpreter と machine の simulator で照合、
  cutout の 2 つは 32 lane の一部を drop し両者が一致、vistest の byte 0x82・0x87）。`plan/tools/powervr/p010/run.sh` 全 PASS（data-host-test に instance の DDMAD）。
- 見た目: keiland-shot（ZEDBSD_TEST_SCREEN_CAPTURE=y の compositor）の c1 の画面を build/review/ws291/p020-c1.png に置いた（cutout の髪の毛先が抜けて背景が見える、穴や崩れなし。P3 の目視）。
- commit: b158c438d（2〜5）、e37d5681a（7 と道具）、この節と 6 は次の commit。

#### 計測（1920x1080、mailbox 4 枚、vertex shading、SPIN 8〜12 s。道具 `plan/ws291/tests/vf2-mview.sh`、要約は build/ws291/run/*/summary.txt）

| scene | kernel | 表示 | fps | frame ms | fence ms |
| --- | --- | --- | --- | --- | --- |
| c1 | 3706684313 | 合成 | 62.19 | 16.05 | 13.94 |
| c3・c5・c7 | 79879895 | 合成 | 49.74・43.24・36.51 | 20.1・23.1・27.3 | 17.2・19.5・24.4 |
| c9・c13 | 850469337（timing） | 合成 | 31.83・26.56 | 31.3・37.5 | 28.2・33.7 |
| c1・c13 | 1341490516（timing、BLOB） | **direct**（`SCANOUT direct=1`） | 62.29・26.57 | 16.0・37.5 | 13.0・34.2 |
| c13、320x180 の window | 850469337 | 合成 | 45.40 | 22.0 | 17.0 |
| c1、960x540・1280x720・1920x1080 の window | 試し（punch-through なし） | 合成 | 185.1・141.3・70.1 | 5.4・7.1・14.2 | 0.60・4.59・11.59 |
| c1・c13 | 試し（punch-through なし、direct） | direct | 70.29・28.08 | 14.2・35.5 | 10.9・30.4 |

- CPU の側（record 0.3、submit 1.5、present 1〜2 ms）は小さく、GPU 律速（fence が frame の 9 割）。kernel の timing（c13）: submit 0.37〜0.46 ms、job build 0.13〜0.14 ms、
  render jobs 0.12 ms、job wait 平均 31〜36 ms。
- 合成と direct で fps は同じ（compositor の合成は律速でない）。punch-through（cutout）の費用は c1 で約 2 ms、c13 で約 4 ms。
- c1 の 1920x1080 の固定の費用が大きい: 960x540 で fence 0.60 ms、1280x720 で 4.59、1920x1080 で 11.59（画素は 4 倍で時間は 19 倍、tile あたり 1.2 → 5.7 µs）。
  partial render の log は無い。解像度に対して非線形な何か（memory の配置、PBE の linear の書き出し、cache・TLB）が疑い。c13 の小さい window は 17 ms で、頂点の側も重い。
- copies 50 は device lost（16 MiB の global free list が尽きて `parameter memory ran out`→`partial render`→`ta: fail(free list)`。64 MiB にすると log の無い device lost に変わり、元に戻した）。目標の c13 には要らない。
- 板の SD が不調（dw-mmc の read の失敗が毎回十数回、16:11 に card が消えて `ufs: … refuses writes` → SwitchBot で入れ直して戻った）。
- 試した事（16:5x）: vkmatrix の 1080p の perf（kernel 1990096130 の build の vkmatrix、render node だけ）: clear 2.4 ms、flat 2.6、texture 3.1、tri128 2.7 ms/frame
  → 1080p の clear・store の基本の経路は速い。mview の c1 の 1080p の 11〜13 ms は model の描画の側。texture を 1/8 にした model（build/ws291/qs40small）は
  mview の model の読み込みで失敗（`mview_model_load`、原因は未調べ）。

#### 再開点と次の手（2026-10-30 17:00 P3）

- 今: c13 は direct scanout で 26.57 fps（目標 50）。GPU 律速。改善は未着手（profile の途中）。板は元の kernel に戻した（下の「板」）。
- 次の手（順に）:
  1. 解像度に非線形な費用の切り分け: 960x540 で 0.6 ms、1080p で 11.6 ms（c1）。texture の大きさ（mip の level 0 の読み、texture の memory の cache の属性）、
     fragment の数、ISP の tile の重なりのどれかを、texture を小さくした model（mview_model_load の失敗を直す）・単色の material・小さい window で分ける。
  2. 頂点の側（c13 の 320x180 で 17 ms）: vertex shader の命令数（mview.vert 632 B）、CAM・instance の設定、TA の PPP の状態、1 draw あたりの頂点の処理を見る。
  3. punch-through（cutout）の費用 2〜4 ms: Mesa と同じく discard を後ろに寄せる・feedback の位置を見直す。
  4. 道具: kernel の timing は TA と 3D を分けない（job wait だけ）。firmware の時刻か render の TA・3D の完了の時刻を timing に足すと割合が出る。
- copies 50 の device lost（free list の grow が無い）は目標の c13 には要らないので残した。
- 板（17:0x）: 元の kernel に戻して読み返しで照合（/boot/boot0/vmunix cksum 2825517684、vmunix.p7old も 2825517684）、SwitchBot で入れ直して起動、zedbsd.cfg の login の行 0。
  板に残る物: /boot/boot0/vmunix.p7old（元の写し）、user kei の ~/p7（program・library・qs40・qs40s・vkmatrix・keiland-shot・画面の PNG。次の計測で使う）。
- 再開の手順: `VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=… VF2_PLUG=14C19FA80F86 [TIMING=1] [KEEP=1] COPIES=13 sh plan/ws291/tests/vf2-mview.sh OUT BUILD`
  （BUILD は `make ZEDBSD_CONFIG=plan/ws291/tests/config-vf2.mk BUILD=… …/vmunix …/bin/wayland …/bin/mview`、timing は `ZEDBSD_TEST_CPPFLAGS=-DPOWERVR_SUBMIT_TIMING=1`）。
