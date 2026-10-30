<!-- awesome-plan project=zedbsd record=ws291-p020 -->

# ws291-p020 PowerVR（VisionFive 2）: model の 50 fps

- 親: [WS291](../ws.md)。
- 状態: planned（2026-10-30 15:00 P7、q1712-i01: 準備と起点の計測を試みたが、板に表示が無く mview が動かない。基準は未決（ユーザーに Q1 が問い合わせ中）。
  Q1 の指示（2026-10-30）「結果が来るまで、scene の改善には入らないでください。」）。
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
