<!-- awesome-plan project=zedbsd record=ws291-p020 -->

# ws291-p020 PowerVR（VisionFive 2）: model の 50 fps

- 親: [WS291](../ws.md)。
- 状態: in-progress（2026-10-31 01:5x P2 q1733-i01: SLC を既定にし（回帰 PASS）、present の copy を geometry が待たない形にして direct scanout の c13 45.8 fps・c1 86.9 fps（目標 50）。下の「q1733-i01」節）。前: in-progress（2026-10-31 01:1x P2 q1726-i01: direct scanout の c13 42.35 fps（目標 50）。前の「direct」は合成だった。下の「q1726-i01」節と「訂正」節。権限の判定で板の回帰の前に止まった）。前: in-progress（2026-10-30 16:50 P3、q1715-i02: 表示が出て、driver の欠け（instance rate・discard・vkCmdCopyBuffer・mip の transfer・direct scanout の BLOB）を
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

### q1726-i01（2026-10-30 22:50〜 P2、worktree p2-ws291v、基点 main 780e7349b）

- 承認: 2026-10-30 ユーザーの指示（WS291 の PowerVR の最適化を P2 で）。目標は ws.md の末尾の決定のとおり qs40 × 13・1920x1080・direct scanout で 50 fps 以上。
- 道具: `vf2-mview.sh` に `OLD=`（板に残す元の kernel の名前、既定 p7old）を足した。P2 は `OLD=p2old`。

#### 足した物（commit e14843450、板では未確かめ）

1. 計測: timing の build（`-DPOWERVR_SUBMIT_TIMING=1`）の kernel の行に `gpu ta`・`gpu 3d`・`gpu frame` を足した。service の pass が起きるたびに、走っている render job の
   geometry と fragment の timeline を読み、届いた時刻を job に付ける（render-job.c `powervr_render_jobs_stamp`）。reap の時に、TA ＝（kick・前の TA の終わり・geometry が待った 3D の終わりの遅い方）から TA の終わりまで、
   3D ＝（TA の終わり・前の 3D の終わりの遅い方）から 3D の終わりまで、frame ＝ 前の 3D の終わりから。時刻は service の thread が起きた時なので、割り込みから thread までの遅れを含む。
2. TA と 3D の重なり: これまで render の geometry は context の直前の 3D（`fragment.submitted`）を待っていた → 次の frame の TA は前の frame の 3D が終わるまで始まらず、frame ＝ TA ＋ 3D だった。
   Linux v7.3 の pvr_job.c（geometry は同じ HWRT data の job だけを待つ）と Mesa（data set の 2 つの rt_data を交互に）に合わせ、target の half ごとに最後の 3D の値を覚え（render-target.c `powervr_target_half_fragment`・`_note`）、
   geometry は同じ half の前の 3D だけを待つ形にした。half は既に交互（`powervr_target_take_half`）。注意: vertex の段が前の pass の fragment の出力を読む使い方（同じ session の前の 3D に依存する TA）は、これで順が保証されなくなる（mview・compositor には無い）。
3. cutout の shader: `if (a < 0.5) discard;` が EMC の if・else・end の分岐になり 29 group（不透明の物は 8）。kill だけの arm と merge の selection を、分岐なしの discard の select に直した（shader-spirv-control.c `reader_kill_only`）→ 14 group。
- host 試験: p010-compiler 3062 PASS・group 29 PASS（cutout の 2 つは lane の一部を drop し machine と interpreter が一致）、p010 全 PASS（job-host-test に stand-in 4 つ）。build（vf2b、timing 付き）warning 0。

#### 板（23:0x）: 起動しない

- 22:55 に `OLD=p2old TIMING=1 KEEP=1 COPIES='1 13'` で main 780e7349b ＋ 1 の kernel（build/ws291/vf2t、cksum 4291544047）を入れた（読み返しで一致）→ run.sh の 8 回の入れ直しで SSH が来ず、
  23:08 にもう 1 回入れ直して 3 分 ping も無し。元の kernel は板の /boot/boot0/vmunix.p2old・.p7old（2825517684）。この host に VF2 の serial（/dev/ttyUSB0）が無く戻せない → Q1 に回復を頼んだ。
- 見込み（未確かめ）: 1 は service の pass で timeline を読むだけで起動に効かない。main の riscv64 の変更（WS331 p007〜p011）の VF2 の退行の疑い。P3 の起動した kernel は main 5aca73738 の上。
- 23:12〜23:40 の 25 分も ping は無い（SwitchBot は on）。この host に VF2 の serial も HDMI の capture も無く、私からは戻せない。
- main の VF2 の起動の確かめ（ws331 の記録から）: p005・p007 は VF2 の実機で起動（p007 は `end-vf2.sh` PASS、画面あり）。p008〜p011（Pi の共通化、kernel の header、atomic.h、spinlock の macro。d272f0a05 は 22:28）は QEMU の amd64・rpi4・m68k・riscv64-virt だけ → VF2 では未確かめ。
- 備え（未 commit の build だけ）: 板で起動した P3 の基点（cb03c9983、main の祖先）に e14843450 の driver の差分を当てた tree を build/tmp/p3src に作り、`build/tmp/p3src/build/b`（vmunix cksum 3248386891・wayland・mview・library、timing 付き、warning 0）を build。
  plain main（780e7349b、timing 付き）の kernel は build/ws291/vf2m（1895595753）、main ＋ 差分は build/ws291/vf2b（2042075504）。

#### 再開点（2026-10-30 23:40 P2）

1. 板の回復（人手）: SD の 3 番目の FAT の区画（/boot/boot0）の vmunix を vmunix.p2old（2825517684）で上書き（または U-Boot の serial で fatload・fatwrite）。
2. 戻ったら、まず起動の分かっている基点の上の差分で測る: `VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=… VF2_PLUG=14C19FA80F86 OLD=p2old TIMING=1 KEEP=1 COPIES='1 13' SPIN=12 sh plan/ws291/tests/vf2-mview.sh OUT build/tmp/p3src/build/b`
   （c1・c13 の fps と kernel の `gpu ta`・`gpu 3d`・`gpu frame` の行）。TA/3D の重なりと cutout の効果を分けるには、P3 の基点そのまま（P3 の build/ws291/vf2t、計測の stamp は無い）と比べる。
3. main の kernel（build/ws291/vf2m）の VF2 の起動は、回復の手段（serial）がある時だけ試す（起動しないと板がまた止まる）。
4. その後の手（phase020 の次の手の 1〜3）: c1 の 1080p の非線形な 3D の費用（texture の mip 0 の読み・cache）、頂点の側（mview.vert 49 group、TA の固定機能・parameter buffer の書き）、cutout の残り（14 group: 比較・select・tstz を 1 つの test に寄せる、feedback を色の後ろへ）。

#### 板が「起動しない」の訂正と回復（2026-10-30 23:41〜10-31 00:4x P2）

- ユーザーが VF2 の serial をつないだ（/dev/ttyUSB0、by-id `usb-FTDI_TTL232R_FTBTJOV8-if00-port0`）。serial を読みながら入れ直すと、main 780e7349b ＋ 1 の kernel は止まっておらず init・`login:` まで出ていた。
  SSH が来ない原因は network: `networkd: initial Wi-Fi normalization: Invalid argument` → `init: networkd: FAIL 3 wifi-normalize` → `init: skipped networking: required dependency failed`（en0 は link up）。
  板の userland（10-21 の 5c83c16）のままでも、main の rootfs の networkd（3095223911）に揃えても同じ EINVAL（networkd の retire_managed_policy、VF2 に Wi-Fi は無い）→ main の VF2 では network が上がらない。Q1 が Bug にした: BUG-371（Wi-Fi の正規化の失敗だけで init が有線の en0 を含む networking 全体を飛ばす）、BUG-372（main の VF2 の kernel で serial の入力が効かない。ws331 p005 の疑い）。
- 計測の差分の kernel の不具合（直した、ef5879abb）: 計測の stamp が、render context を作っている途中の session の timeline を読んで NULL の load（`riscv64 trap scause=d stval=0`、`powervr_render_jobs_stamp`）。走っている job がある session だけを見るようにした。
- 回復: U-Boot の入力は効く（autoboot の 2 秒に空白を送る）。`fatload mmc 1:3 0x60000000 vmunix.p2old` → `fatwrite mmc 1:3 0x60000000 vmunix ${filesize}` → `boot` で元の kernel に戻せる。
- **事故（P2 の手順の誤り）**: userland を main に揃える時、file ごとに「mv 元 → 元.p2old、cp 新 → 元」とした。/bin/cp 自身と /lib/ld.so を退けた所で cp（動的）が動かなくなり、/lib/ld.so が無い板になった（exec できず SSH も入れない）。
  回復: static の helper（build/tmp/p2swap、/bin・/sbin・/lib・/usr の *.p2old を rename で戻し、/lib/ld.so と /sbin/init を stat で確かめてから /sbin/init を exec。pid 1 で走る）を U-Boot の `loady`（host の lrzsz の sb、ymodem、crc32 で照合）で読ませて区画 3 に fatwrite、
  zedbsd.cfg（元を zedbsd.cfg.p2orig に写した）に `init=/boot/boot0/p2swap` を 1 行足して起動 → `p2swap: 178 done, 0 failed` → init・sshd → SSH。cfg は元に戻して cksum 一致（632416842）。
- 手順の直し（Q1 の条件）: 入れ替えは、新しい物を同じ directory の `FILE.p2new` に写し（mode と owner は元の物）、static の helper が `link(FILE, FILE.p2old)` と `rename(FILE.p2new, FILE)` を exec を挟まずに行う。ld.so と libc は最後。mv で先に退ける形は使わない。
  root（UFS 256 MiB）は inode が足りず /usr/include・*.a・pkgconfig・/lib/debug は除いた（256 file）。この形で main の userland に揃えたが上の EINVAL で network が上がらず、同じ init= の手順（`254 done, 0 failed`）で元に戻した。
- 板の今（00:4x）: kernel は元（/boot/boot0/vmunix 2825517684）、userland は元（入れ替えた 279 file が作業前の cksum と全部一致）、zedbsd.cfg は元（login の行 0）。
  残る物: 区画 3 の p2swap・zedbsd.cfg.p2orig（P2 の回復の道具）、main の rootfs の新しい file（元に無かった物、空の directory を含む。元の file とは名前が重ならない）。元の 279 file の写しは host の build/ws291/board-orig/orig.tar。

#### 計測 b2（2026-10-30 23:57、P3 の基点 cb03c9983 ＋ e14843450・ef5879abb の差分、kernel 5686341、板の古い userland、direct scanout、TIMING=1）

| scene | fps | frame ms | fence ms | gpu ta ms | gpu 3d ms | gpu frame ms |
| --- | --- | --- | --- | --- | --- | --- |
| c1 | 66.48（P3 の前 62.29） | 15.03 | 11.76 | 4.9 | 5.5 | 15.8 |
| c13 | 27.44（P3 の前 26.57） | 36.33 | 31.28 | 20.7 | 7.8 | 36.1 |

- 分かった事: c13 の GPU は TA（頂点の処理と tiling）が 20.7 ms、3D が 7.8 ms。TA ＋ 3D ＝ 28.5 ms に対し frame は 36 ms。mview は frame ごとに fence を待つ（`vkWaitForFences` を毎 frame、command buffer は 1 組）ので
  同時に走る frame は 1 つ。GPU は CPU の record・submit・present（約 4.5 ms）の間に止まり、TA と 3D の重なり（2）は効く場面が無い（効果は未確かめ）。
- TA の費用: c1 4.9 ms、c13 20.7 ms → 1 copy（25861 頂点、37000 三角形）あたり約 1.3 ms ＋ 固定 約 3.6 ms。目標の 20 ms（50 fps）には、mview が 1 frame しか重ねない限り CPU ＋ TA ＋ 3D ≦ 20 ms が要る。
- cutout の分岐なし（3）は 3D の側。c1 の 3D 5.5 ms（P3 の c1 の fence 13.0 ms の時の分けは無い）。

#### 訂正: これまでの「direct」は合成を経た数（2026-10-31 01:0x P2）

- P3（q1715-i02）の「direct」の数と、P2 の b2・p8〜p13 の数は、**direct scanout ではなく合成**だった。compositor の log: `KWL SCANOUT direct=1` の直後に `KL SCANOUT refused step=import errno=48`（EXDEV）→ `KWL SCANOUT direct=0 reason=refused`。
  診断の kernel（1 frame ごとの job と transfer の記録）で、mview の session が毎 frame `GPU_OP_CMD_COPY_IMAGE`（WSI の present の copy、1080p）を 1 つ、compositor の session が毎 frame 1920x1080 の render job（1 draw）を 1 つ出していた。
- 原因: Keiland の zedBSD backend の scanout_import（userland/desktop/libkeiland-backend-zedbsd/scanout-zedbsd.c）が GPU_RESOURCE_IMPORT を flags 0 で呼ぶ。VF2 では display（gpu1、dc8200）と render（gpu0、PowerVR）が別の device なので、
  gpu.c の import は「foreign かつ GPU_IMPORT_SCANOUT 無し」で EXDEV を返す。直し（Q1 の判断で範囲に含めた、d983c4ee5）: `import.flags = GPU_IMPORT_SCANOUT`（libvulkan の wsi-display.c と同じ）。
- 同じ device の GPU で動きが変わらない事（gpu.c の import を読んだ）: foreign でない時、flags は device の share の import_resource にそのまま渡る。amdrdna（node_import）・nvrtx（share_import）・i915（i915_share_import）・powervr（render_import）・
  venus（share_import）・mali・bcm2711 の render と display はどれも `(void)flags` で使わない。GHCI（ghci/share.c）だけ SCANOUT を ENOTSUP で断るが、GHCI は display を持たず scanout の道を通らない。
- host 試験: `plan/tools/gpu-boundary/v1-check.sh build/ws291/vf2b` は 1（grep）と 3 で FAIL 無し、2（poison した header での compile）は amd64 の sysroot がこの worktree に無く 0 source（未実施と同じ）。
- 直した後は direct が続く（c1 717 frame、c13 508 frame、refused 無し、`reason=no-window` は mview の終わり）。ただし game mode の終わりに compositor が window mode に戻る時、display の swapchain の作成が
  `VULKAN kernel error errno=17 device lost`（EBUSY、display の lease）→ `KWL FAILED site=window_mode` → compositor が終了する事がある（2 回中 2 回目、別の run では 1 回目）。Q1 が BUG-375 に登録（P7 が直す）。BUG-375 が直るまで d983c4ee5 は main に入れない（Q1）。
- 残る copy: mview は Wayland の client なので、WSI は game mode でも自分の image から共有の image へ毎 frame copy する（libvulkan の swapchain_direct_possible は VK_KHR_display の面だけ direct）。zero copy は WS309 p015 の候補（compositor が game mode の image を貸す形）。

#### 計測（2026-10-31、main 1f8960e4c を merge した 0d68d5341 ＋ 差分、TIMING=1、qs40、1920x1080、mailbox 4 枚）

direct scanout（d983c4ee5 の直しあり、refused 無し）:

| kernel | scene | fps | frame ms | fence ms | gpu ta ms | gpu 3d ms |
| --- | --- | --- | --- | --- | --- | --- |
| m1（SLC あり） | c1 | 83.43 | 11.97 | 9.85 | 7.7 ※ | 2.9 |
| m1（SLC あり） | c13 | 42.35 | 23.57 | 20.84 | 15.2 ※ | 6.8 |
| m2（m1 ＋ transfer の待ちを外す試し、安全でない） | c1 | 87.05 | 11.48 | 9.22 | 1.7 | 8.3 ※※ |

※ TA の数は前の frame の present の copy を待つ時間を含む（geometry は session の最後の transfer を待つ）。※※ 待ちを外すと copy（3D の hardware、約 5 ms）が次の 3D の前に入り、3D の数に移る。c13 は compositor が BUG-375 で落ちて測れず。
P3 の基点（cb03c9983 ＋ 差分、p15、scratch の tree に同じ直し）でも c1 83.35・c13 42.40（同じ）。

合成（直す前、P3 の基点 ＋ 差分、kernel 5686341 系、板の古い userland、TIMING=1）:

| 試し | scene | fps | gpu ta ms | gpu 3d ms | 分かった事 |
| --- | --- | --- | --- | --- | --- |
| b2 / p2 基点 | c1・c13 | 66.5・27.4 | 4.9・20.7 | 5.5・7.8 | TA が律速 |
| p3・p4・p6・p7 window 320x180〜1600x900 | c13 | 45.3〜31.4 | 16.2〜19.2 | 0.97〜6.2 | TA の約 15 ms は解像度に依らない頂点の側、1080p で ＋約 4.5 ms（tiling）。3D は画面の面積に比例 |
| p8 vertex shader に 48 group の水増し（49→91 group） | c13 | 27.43 | 20.7 | 7.8 | TA は不変 → shader の長さは律速でない |
| p9 POWERVR_DEVICE_MEMORY_CACHED=1（device-local の Vulkan memory を SLC に通す） | c1・c13 | 71.2・38.2 | 4.6・11.0 | 5.1・7.8 | **TA 20.7→11.0 ms**。memory の読みが律速 |
| p11 同じ、window 320x180 | c13 | 81.2 | 6.2 | 0.85 | 頂点の側 15→6 ms |
| p10 ＋ POWERVR_PROGRAM_SLC_CACHED=1 | c1・c13 | 71.4・38.2 | 4.6・11.1 | 5.2・8.0 | 効果なし |
| p12 ＋ render target の memory（macrotile・region header・vheap・tails）も SLC（scratch の試し） | c1・c13 | 71.2・38.1 | 4.5・11.0 | 5.1・8.1 | 効果なし（戻した） |
| p13 SLC、画面の PNG（build/ws291/run/p13/shot-c1.png・shot-c13.png） | c1・c13 | 69.7・37.8 | — | — | 見た目は崩れなし（cutout の髪の毛先、13 体の並び。P2 の目視） |

- SLC（POWERVR_DEVICE_MEMORY_CACHED=1）は 10-21 に「板を log 無しで再起動させた」記録があるが、今回は 12 run（mview の起動と終わりを含む）で再起動・fault 無し（serial の log も）。既定はまだ 0（既定を 1 にする前に vkmatrix・vkcoherency・desktop の回帰が要る。
  vkmatrix を板で流す command は権限の判定で拒否された（下）ので未実施）。
- 他: `--shading=pixel` は c1 から device lost（vkWaitForFences -4、記録だけ）。`--cull=all`（VK_CULL_MODE_FRONT_AND_BACK）は pipeline の作成で ENOTSUP（`a state the executor does not take`）。
- vertex input の直読み（vertex shader が入力を temp に写さず vtxin の register を float の演算で直に読む、e180f3f03）: mview.vert 49→45 group、host 試験 PASS。TA には効かない（上の水増しの結果のとおり）。
- mview に `--inflight=N`（1〜2、既定 1）を足した（d983c4ee5、Q1 の OK、参考の数のため。受け入れは既定の 1 のまま）。板では未実施。

#### 再開点（2026-10-31 01:1x P2、権限の判定で止まった）

- 止まった操作: 板で SLC の kernel（build/ws291/m1）を run.sh で入れ、ssh・sudo で ~/p7/bin/vkmatrix を流す 1 本の command が auto mode の判定（Interfere With Workloads）で拒否された。回避していない。
- 次の手（順に）:
  1. SLC を既定にする前の回帰: vkmatrix・vkcoherency・gpu-selftest・Keiland の desktop（frame-ops）を SLC の kernel で（権限の扱いを Q1・ユーザーが決めてから）。通れば `POWERVR_DEVICE_MEMORY_CACHED` の既定を 1 に（interface.h）。
  2. present の copy を次の geometry が待つ件: transfer が buffer を書いた時と、job の vertex shader が image を読む時だけ geometry を待たせ、他は fragment（と partial render）に待たせる（render-kick-layout.c に fragment の waits を足す）。c13 で TA と copy が重なり 約 4 ms 縮む見込み。
  3. copy 自体（1080p、約 5 ms）を無くす zero copy は WS309 p015（範囲の外）。
  4. BUG-375 が直るまで、direct の計測は COPIES を 1 つずつ別の run（compositor を毎回起こし直す）にする。
- 再開の command: `VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=… VF2_PLUG=14C19FA80F86 OLD=p2old TIMING=1 KEEP=1 COPIES=13 sh plan/ws291/tests/vf2-mview.sh OUT build/ws291/m1`
  （build: `make ZEDBSD_CONFIG=plan/ws291/tests/config-vf2.mk BUILD=build/ws291/m1 ZEDBSD_TEST_CPPFLAGS="-DPOWERVR_SUBMIT_TIMING=1 -DPOWERVR_DEVICE_MEMORY_CACHED=1" …/vmunix …/bin/wayland …/bin/mview`）。serial: /dev/serial/by-id/usb-FTDI_TTL232R_FTBTJOV8-if00-port0。

### q1733-i01（2026-10-31 01:1x〜01:5x P2、worktree p2-ws291v、main 095ed8acb を merge した 50487dbe4 の上）

- 承認: 2026-10-30 ユーザーの指示（WS291 の PowerVR の最適化を P2 で）。2026-10-31 ユーザーから、P2 が VF2 で SSH・sudo を使って kernel・userland の入れ替えと計測の program の実行を続けてよい、という趣旨の指示（Q1 経由）。bug-analyzer の subagent の利用の許可（Q1 経由）。
- 道具（c8f69f7fa）: `plan/ws291/tests/vf2-regress.sh OUT BUILD`（`KERNEL=`・`KEEP=1`・`OLD=p2old`。板の kernel を入れ、~/p7 に置いた tree の program を root で: vkmatrix・`-p 3`・`-r 3`・vkcoherency・vkcompute・desktop（compositor ＋ Files の desktop ＋ Terminal、keiland-shot の PNG）、step ごとの kernel の fault・lost の行、/boot/boot0/log の最新の名で再起動の有無）、`config-vf2-shot.mk`（config-vf2.mk ＋ 画面の capture と keiland-shot）。
  `vf2-mview.sh` は compositor の log を丸ごと OUT/wayland.log に回収する（BUG-375 の解析のため）。

#### 1. SLC（device-local の Vulkan memory を SLC に通す）を既定に

- 板の回帰（build/ws291/run/r-s0・r-s1・r-s2・r-s3/summary.txt）: SLC なし（s0、kernel 3264574974）・あり（s1、160846527）・下の 2 を足した物（s2、3547555133）・既定 1 と下の直し（s3、83139905）の 4 つとも
  vkmatrix 109/109、`-p 3` PASS、`-r 3` 327/327、vkcoherency PASS、vkcompute 4096/4096、desktop の compositor の error 0。kernel の fault・lost・reset の行 0、再起動 0。desktop の PNG は 4 つとも同じ cksum（518389155、build/review/ws291/p020-s3-desk.png）。
- 10-21 の再起動の原因の調べ（bug-analyzer、code を読むだけ）: 平常の道（参照 → unmap → SLC の flush → free）に欠けは無い。欠けは異常系: fault・flush の失敗の時に flush 無しか未完で page を kernel に返す、時間切れの flush の command が後で走る、isolate・abandoned の session の buffer。
  firmware の command（KCCB 105、data の配置）は Linux v7.3 の pvr_rogue_fwif.h と一致。log 無しの再起動の第一候補は「flush の完了を早く取り違えて解放の後に書き戻しが落ちる」（未確かめ）。原因は確定していない。
- 直した物（c8f69f7fa、driver の中だけ、HAL は変えない）: fault か flush の失敗の時は flush した buffer を kept に回し reset（core の電源を落とす）の後に解放、flush の失敗は quarantine、flush 済みの buffer は kernel でなく pool に戻す（gpu_cached=0。後の書き戻しが kernel の memory に落ちない）、cached の memory に blob（CPU・display の view）を作らない（EINVAL）。
  `POWERVR_DEVICE_MEMORY_CACHED` の既定を 1 に（interface.h、source の comment の日付の経緯も外した）。
- 未実施: isolate・abandoned の session の buffer を kept に回す直し（cached=0 でも同じ既存の道）、flush の後の SLC_STATUS0 の pending の読み（GPU の電源が落ちている時の register の読みを避けて見送った）、解放の多い負荷と page の pattern の照合、vkreset との組み合わせ。

#### 2. present の copy を geometry が待たない

- これまで transfer の後の render は geometry から transfer を待った（次の frame の TA が前の frame の present の copy（約 5 ms）の後に始まった）。
  直し（c8f69f7fa）: render の 3D（partial render と fragment）は最後の transfer を待ち、geometry は transfer が buffer を書いた時（vkCmdCopyBuffer・copy image to buffer、session に最新の値を覚える）と、job の vertex shader が image・sampler を読むか store する時（compiler の `program.stores` を足した）だけ待つ。
  render-kick-layout の sequence に 3D だけの waits を足した（partial render の fence に geometry の waits・3D の waits・geometry の完了、fragment の fence に 3D の waits・geometry の完了）。試しの switch RENDER_TRANSFER_WAIT_TRIAL は外した。
- host 試験: p010 全 PASS（kick-host-test 87、3D の waits の 5 件を足した。selftest-host は 1 回目に host の負荷 45 で時間の判定が外れ、再実行で PASS）、p010-compiler 3062 PASS・group 29 PASS。build（s1・s2・s3・d1）warning 0。

#### 計測（direct scanout、1920x1080、qs40、mailbox 4 枚、TIMING=1、SPIN 12、COPIES は 1 つずつ別の run。build/ws291/run/v-*/summary.txt）

| kernel | c1 fps | c13 fps | c13 frame ms | c13 fence ms | c13 gpu ta ms | c13 gpu 3d ms |
| --- | --- | --- | --- | --- | --- | --- |
| s1（SLC の試し、2 なし） | 83.41 | 42.30 | 23.59 | 20.97 | 15.2 | 6.7 |
| s2（s1 ＋ 2） | 86.92 | 45.85 | 21.77 | 18.99 | 13.2 | 7.0 |
| s3（SLC 既定 ＋ 直し ＋ 2） | 86.94 | 45.78 | 21.80 | 18.98 | 13.1 | 6.7 |
| d1（s2 ＋ BUG-375 の診断） | 86.93 | 45.84 | 21.77 | 18.97 | — | — |
| s3、`--inflight=2`（参考） | — | 43.42 | 22.98（min 10・max 30） | 20.70 | — | — |

- 見た目: s3 の c13 を window（合成）で撮った PNG（build/review/ws291/p020-s3-c13-window.png）で 13 体の並び・cutout の髪・崩れなしを P2 が目視。direct scanout の間は keiland-shot が `ERROR inactive`（game mode では撮れない）。
- 残りの差（c13 で 21.8 ms → 20 ms に約 1.8 ms）: frame ≒ CPU（acquire 0.15・record 0.34・submit 1.30 ms、この間 GPU は止まる）＋ TA 13.1 ＋ 3D 6.7。TA は copy と重ならない時 約 11 ms（前の p9）で、copy（1080p、約 5 ms、3D の hardware）と重なる帯域の競合で 約 2 ms 延びる。
  kernel の submit の中は job build 162 µs・map 80・kick 19・mmu flush 2 回で 約 0.35 ms（libvulkan の submit 1.3 ms の残りは wire と ioctl）。copy を無くす zero copy（WS309 p015、範囲の外）で TA の競合 約 2 ms が消える見込み（未確かめ）。`--inflight=2` は速くならず（43.4、frame が 10〜30 ms に揺れる。原因は未調べ）。

#### BUG-375 の資料（P7 へ、Q1 経由）

- 取り直した compositor の log（丸ごと、順のまま）: build/ws291/run/v-s1-c1/wayland.log・v-s1-c13・v-s2run-c1・v-s2run-c13（各 `KWL FAILED site=window_mode` 2 回）。mview の出力は同じ dir の c1.txt・c13.txt。
  並び（v-s1-c1）: `KWL SCANOUT direct=0 reason=no-window` → `KWL STARTUP step=output-swapchain ms=34` → **compositor の** `VULKAN kernel error errno=17 device lost`（zedBSD の errno 17 は EBUSY）→ `KWL CURSOR_PLANE unavailable step=swapchain result=-4` → `KWL VULKAN_ERROR operation=targets result=-4` → `KWL MODE_ERROR errno=5` → `KWL FAILED site=window_mode`。mview の c1.txt にも同じ errno=17 の行がある。
- 診断の build（build/ws291/d1、CONFIG_POWERVR_DEBUG=y、display.c の EBUSY の 6 箇所に関数名の行、libvulkan の device lost の行に context.c の行番号。plan/ws291/tests/bug375-diag.patch、tree からは戻した）で c1 → c13 を 1 つの compositor で流したが、この run では再現しなかった（build/ws291/run/d1-c1c13、lease 1〜5 の claim・release の行は c1.kernel・c13.kernel・summary.txt）。s3 の 2 run でも再現なし。再現は s1・s2 の 4 run の全部。

#### 板（01:5x）

- 元に戻して照合: /boot/boot0/vmunix 2825517684（vmunix.p2old も 2825517684）、zedbsd.cfg 632416842、login の行 0。userland は触っていない（program は ~/p7 から LD_LIBRARY_PATH で）。
- 板に残る物: 区画 3 の p2swap・zedbsd.cfg.p2orig・vmunix.p2old、~/p7（program・library・qs40・regress.sh・r-*.txt・desk.png）。

#### 再開点（2026-10-31 01:5x P2）

- 今: c13 45.8 fps（direct、目標 50）。commit c8f69f7fa（SLC 既定と異常系の直し、transfer の待ちの分け、道具）。
- 次の手の候補: (1) zero copy（WS309 p015、範囲の外: Q1 の判断）で copy と TA の競合を無くす。(2) submit の CPU（frame あたり GPU が止まる 約 1.8 ms）を縮める: libvulkan の submit 1.3 ms の内訳（wire の encode・ioctl）、kernel の job map 80 µs。(3) `--inflight=2` が遅くなる理由。(4) SLC の残りの直し（isolate・abandoned の buffer、解放の多い負荷での照合）。

