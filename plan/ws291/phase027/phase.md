<!-- awesome-plan project=zedbsd record=ws291-p027 -->

# ws291-p027 NVIDIA: libvulkan の submit の hand-off（B-U、design の P-A）

Status: in-progress（2026-11-01 22:10 P1、q1849-i01。実装・host 試験・実機の A を終え、Q1 の指示で WS291 のラップアップのため途中で止めた。cleared の判定は Q1）

- WS: [WS291](../ws.md)。設計: [phase026/design.md](../phase026/design.md) の P-A（B-U）と [phase026/phase.md](../phase026/phase.md) の Phase の表・受け入れ。
- 範囲: design.md の P-A のとおり（0. GPU の 1 run を timing 無しで測り直す、1. B-U: hand-off・入口の drain・back-pressure・失敗の latch・fork・環境変数の切り、2. host 試験、3. 10.0.10.3 の RTX 2070 で subs-100・subs-1000 ほかの計測と回帰、Venus の回帰）。
- Q1 の判断（p026 の問い、2026-11-01 21:07）: fence の無い vkQueueSubmit は返事を待たずに返す（A。nvrtx だけ、環境変数で切れる。その submit の memory の不足は後で DEVICE_LOST になる）。mview の bench の mode（`--record=once`・`--submit-all`）は足す。
- 受け入れ: design.md の P-A の受け入れ（subs-1000 の fps が NVK の 75.4 に近づく見込みを実測で確かめる、後退なし）。

## q1849-i01（2026-11-01 P1、worktree p1-ws291p026、基点 main 25ce6d12e）

- commit: bd9271091f（B-U の実装と H3a）、43d2caaaf5（__thread と link の形、rtx-p024.sh の腕ごとの環境変数）、98cc82013a（H3b と 0 番の診断の差分）。

### 実装（B-U、design.md §6）

- 新しい file `userland/desktop/libvulkan/sender.c`: recording の thread を commands.c から移した。長い記録の batch と hand-off を 1 本の FIFO
  （`struct vulkan_sender_item`、send は lock の外、sent は lock の下）で順に送る。thread は FIFO が空の時だけ眠り、queue に積む側は眠っている時だけ
  signal する。done の broadcast は待つ者がいる時だけ。
- hand-off（`queue.c` の `queue_handoff`）: `queue_unfenced` が真の submit を、queue ごとの 64 の場所（`struct vulkan_handoff_ring`、最初の hand-off で作る）
  に encode し（保留の記録 → QUEUE_SUBMIT、返事の opcode の列）、返事を待たずに VK_SUCCESS。command buffer の数が 16 を越える、encode の失敗、
  context の失敗の latch、device の失敗、環境変数 `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0`、1 CPU、fork の子、thread が起きない時は今の同期の道。
  hand-off の道で `pthread_once` から thread を起こす。
- back-pressure: 場所が全部使用中なら、ring 専用の cond で眠り、使用中が半分以下になった時に thread が 1 回 signal。
- drain: `vulkan_context_execute` の入口（context の mutex の前）で、入口の時点の `handoff_queued` まで `handoff_sent` が届くのを待つ。thread 自身は待たない。
  `vulkan_context_close` と `vulkan_queue_finish`（場所を返す前）も drain。
- 失敗: thread が読んだ返事の失敗は context を DEVICE_LOST に latch し、stderr に 1 行（`VULKAN handoff failed result=… device lost`、最初の 1 回）。
- sync.c が context の mutex を直に取る 11 か所を `vulkan_context_lock`・`_unlock`（thread ごとの数を増減）に替え、drain が待つ時に数が 0 でなければ abort。
- fork: `pthread_atfork` の prepare・parent・child（子は thread 無し・hand-off 無し・親の分を待たない）。
- timing（`VULKAN_ZEDBSD_SUBMIT_TIMING`）に handoff・handoff_room・sender_sleep・sender_wake・drain を足し、和を atomic にした（thread も足すため）。
- 設計からの技術の判断（範囲の中）: hand-off は command buffer の `sent_pending` を増やさない（design §6.3 の 1）。kernel への順序は drain が守る
  （Reset・Begin・Free・End の送りは全部 `vulkan_context_execute` を通る）。記録は場所の writer に写るので、command buffer の storage も持たない。
- TLS（2026-11-01 Q1 経由のユーザーの指示: libvulkan.so で __thread を使えるようにする）: 最初は libvulkan.so の `-z defs` の link で `__tls_get_addr` が
  解けず pthread の key にしたが、指示で __thread に戻した。
  - amd64: libvulkan の object を `-mtls-dialect=gnu2`（TLSDESC、`platform/amd64/vmunix.mk` の `DYNAMIC_VULKAN_TLS_CFLAGS`）。arm64 は既定で TLSDESC。
    どちらも `-z defs` のまま link し、readelf で R_X86_64_TLSDESC 1・R_AARCH64_TLSDESC 2、未定義の `__tls_get_addr` 無し。
  - riscv64（VF2）: rtld に TLSDESC の resolver が無く、initial-exec も rtld が拒む。DT_NEEDED の ld.so は rtld が自分と見分けない（`load_object_file`
    は stat の identity で探し、interpreter は premapped で identity を持たないので、2 つ目を写しうる）ので link の相手に足さない。rtld は
    `__tls_get_addr` を全 object のために自分から解く（`reserved_loader_symbol`、`lookup_symbol_version`）。よって armv7 の abitest の前例の形:
    riscv64 の libvulkan だけ `-z undefs` で link し、`check-dynamic-elf.py --provider libwayland-client.so --provider libc.so` で、未定義の symbol が
    依存の物か loader の入口（PRIVATE_RTLD）だけである事を確かめる（-z defs と同じ保証）。`platform/common/userland-dynamic.mk` の
    `DYNAMIC_VULKAN_DEFS`・`DYNAMIC_VULKAN_PROVIDERS`（既定 `-z defs`・空）、`platform/riscv64/userland.mk`。HAL・toolchain・rtld は変えていない。
- HAL・`include/uapi/` は変えていない。

### build と host 試験

- libvulkan.so: amd64（`plan/ws291/tests/config.mk`）・rpi4・visionfive2 とも rc=0、warning 0（`-Werror`）。計測の 14 file と vmunix
  （`BUILD=build/p027/base`）も rc=0（warning の行は外の OpenSSL と perl の locale だけ）。main の libvulkan は main 3f6d5ea98 の git archive から
  build（cksum 509112954 = p024・p026 の lv9 と同じ）。
- H3a `sh plan/ws291/tests/handoff-host-test.sh`（新しい。sender.c と wire.c、transport は偽）: plain・ASan+UBSan・TSan とも PASS
  （18239 checks、sanitizer の報告 0、60 s の上限の内）。順序（3 producer × 3000）、入口の番号までの drain、back-pressure（room の眠り 94〜179 回、
  1 回ごとではない）、thread の起床は眠った時だけ、失敗の latch（後ろは送らず数だけ進む）、batch と hand-off の順、fork の子、context の mutex を
  持つ drain の abort。mutation（drain を外す・半分の hysteresis を外す・毎回 signal）で落ちる事を確かめた。
- H3b `sh plan/ws291/tests/handoff-lib-host-test.sh`（新しい。libvulkan の全 file（wsi-wayland を除く）を host で build し、ioctl・mmap・munmap を偽の
  node に繋ぐ: GPU_GET_INFO と capset は nvrtx と同じ、命令の流れは返事を求める header で見つけて答え、job は 50 µs で終わる）: 描く thread
  （150 frame × 40 part、最後の part に fence、slot の fence を待つ）・fence を読む thread・fence を作り壊す thread の 3 本、hand-off 入と切、
  plain と TSan で 4 run とも PASS（TSan の報告 0、60 s の内）。偽の node が確かめる事: command buffer は End の後にだけ submit される、frame の
  part は順に届く。hand-off 入で 5850 = 150 × 39 が hand-off され全部送られた。queue の後始末（`vulkan_queue_finish`）で場所が消える。
  context の mutex を持って transaction を始めると abort（hand-off を偽の node で止めて）。
  - 設計の H3b の「descriptor を更新する thread」は、fence を作り壊す thread（同じく `vulkan_context_execute` を通る transaction）に替えた。
  - 偽の node で、fence の状態を休みなく読む thread が device の mutex を握り続けて描く thread が進まなかった（glibc の mutex は公平でない）。
    hand-off の有無に関わらない試験の作りの問題で、読む thread に 300 µs の休みを入れた。

### 実機（10.0.10.3 の RTX 2070、2026-11-01 21:33〜21:54、lock /tmp/zedbsd-rdna.lock）

- script: `build/tmp/p027-hw/session.sh`（git の外）の A だけを流し、B〜D の前に止めた（Q1 の指示）。A = `plan/ws291/tests/rtx-p024.sh` ROUNDS=2 SHOT=1、
  腕 main（kernel 3328626646 = この tree の vmunix、kernel の source は main と同じ、libvulkan main 509112954）・bu（同じ kernel、libvulkan 3050428356）・
  off（bu の libvulkan に `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0`）。出力 build/tmp/p027-hw/a/summary.txt。TIMING=0・PERF=0。

| bench | main r1 | main r2 | bu r1 | bu r2 | off r1 | off r2 |
| --- | --- | --- | --- | --- | --- | --- |
| big-100 | 562.3 | 581.1 | 570.0 | 577.9 | 576.8 | 570.4 |
| big-1000 | 114.5 | 113.5 | 112.4 | 114.0 | 106.5 | 107.1 |
| subs-100 | 459.1 | 472.9 | 605.4 | 622.4 | 462.8 | 453.2 |
| subs-1000 | 55.5 | 59.0 | 86.4 | 82.9 | 56.3 | 57.1 |
| scene-1500 | 156.7 | 156.1 | 156.6 | 157.6 | 159.1 | 157.5 |
| scene-5000 | 49.5 | 49.7 | 49.7 | 49.8 | 49.7 | 49.8 |

- subs-1000 の段（ms/frame）: main record 6.05/5.78・submit 11.51/10.74、bu record 6.64/6.79・submit 4.57/4.92・fence 0.03/0.02。1 part は約 11.5〜12 µs
  （record 約 6.7 ＋ hand-off 約 4.7）で、律速は生産側（アプリの thread）。hand-off 1 本は約 4.6〜4.9 µs で、設計の見込み（1〜2 µs）より重い。
  fence が 0.02〜0.03 ms なので、GPU と消費側（recording の thread）は 1 part に遅れていない。
- 受け入れの数（design.md §13 の P-A）: subs-1000 ≥ 75.4 fps（NVK）→ 86.4・82.9（達した）。subs-100 が main 比 +20 % 以上 → +32 %（605.4・622.4 対
  459.1・472.9）。big-100・big-1000・scene-1500・scene-5000 は bu が main の 2 round の幅の内か −3 % の内（big-1000 112.4 は main の下限 113.5 の −1 %）。
  画素の比べ: 21 の比べ（腕 3 × round 2 × still 3 と one-vs-subs）が全部 max 0。
- `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0`（off）: subs・scene・big-100 は main の幅の内。**big-1000 だけ 106.5・107.1 で main（113.5〜114.5）より約 6 % 低い**
  （record 4.37 ms 対 3.8 ms）。big-1000 は fence つきの 1 submit で hand-off の道を通らず、bu と off の違いは環境変数だけなので、原因は未確かめ
  （環境の大きさによる配置の差の推測。bu は main の幅の内）。
- 戻し: 14 file と NVMe の kernel（1463007375）を戻し cksum 一致（summary の back と expected が同じ）、Debian（6.12.111、RTX の 4 function が vfio-pci）、
  lock は free（22:08 に確かめた）。

### 未実施（Q1 の指示で止めた）

- 0 番（GPU の 1 run を timing 無しで、slot の標本に偏らずに）: 診断の kernel（`plan/ws291/tests/nvrtx-allruns-diag.patch`、全 run を輪で retire し、
  timed の時間と start stamp の間隔を 5 s ごとに 1 行。tree の source は戻した。build/p027/kdiag/vmunix 548579364）は作ったが流していない（B・C）。
  ただし A の bu の fence 0.02〜0.03 ms から、GPU は 1 part 約 11.5 µs に遅れていない（GPU の 1 run は 11.5 µs より短い）。
- hand-off の数・眠りと起床の数の timing（TIMING=1、B）。
- rtx-p015.sh（game mode の往復・画面・desktop の frame-ops、D）。
- Venus の回帰（`venus-reg-rtx.sh`）: forge の image を build/p027/forge/hdd-image.img（この tree）に作ったが流していない。
- arm64・riscv64 の上で __thread の libvulkan を動かす事（build と link と readelf だけ）。

### 残りと問い（Q1 へ）

- 次の改善の候補: hand-off 1 本の約 4.7 µs（queue と device の mutex、保留の記録の写し、送り手の lock）。生産側が律速なので、ここを縮めれば
  subs-1000 はさらに伸びる見込み（推測）。
- big-1000 の off の −6 % の原因（上）。
- H3a・H3b は plan/ws291/tests/ に置いた。今後も回帰に使うなら plan/tools/ へ移して tools.md に登録するのは Q1 の判断。
- 消してよい物（Q1 の pipeline へ）: build/tmp/p027-maintree（main の git archive、約 700 MB と build）、build/tmp/handoff-host.*・handoff-lib.*・
  p027-h3a・p027-h3b（host 試験の出力）、build/p027/forge（image 2.4 GB）。計測の出力 build/tmp/p027-hw は記録の元なので残すかは Q1 の判断。

## 回帰 arm64・riscv64（P4 q1860-i01、2026-11-01 22:19〜22:47）

- 2026-11-01 ユーザーから、WS291 は各 architecture で回帰を流して merge を急ぐ、という趣旨の指示を受けた（Q1 経由）。source は変えていない。
- 候補: 60cf76c13（main efd494305 に agent/p1-ws291p026 を merge した物）。V3D と PowerVR では hand-off は通らない（nvrtx だけ）ので、確かめる事は
  __thread の TLS の解決と recording の thread の sender.c への移しが、arm64（TLSDESC）と riscv64（`__tls_get_addr` を rtld が解く）で動く事。
- build（warning 0）: Pi 5 `make -j16 ZEDBSD_CONFIG=plan/ws291/tests/config-rpi5-shot.mk BUILD=build/q1860-pi build/q1860-pi/{vmunix,bin/mview,bin/wayland,bin/aat-input,bin/keiland-shot}`
  （vmunix 4017073610、libvulkan.so 4160257452、R_AARCH64_TLSDESC 2、PT_TLS 8 byte）。VF2 `ZEDBSD_CONFIG=plan/ws291/tests/config-vf2-shot.mk BUILD=build/q1860-vf`
  の vmunix・wayland・files・terminal・keiland-shot・vkmatrix・vkcoherency・vkcompute・mview（vmunix 2999248326、libvulkan.so 415768463、R_RISCV_TLS_DTPMOD64 2、
  `__tls_get_addr` は UND、link は `--provider libwayland-client.so --provider libc.so` の check を通った）。
- 道具（この Queue で足した物）: `plan/ws291/tests/rpi5-reg.sh OUT`（`CAND=BUILD`、`PI5=`）: 板の変える file を板の上に退避して cksum を残し、
  rpi5-mview.sh（板の /root/p021 の program ＝ base と候補）・rpi5-offscreen.sh（板の /lib と、候補の libvulkan・libkeiland を入れた /lib）・rpi5-desk.sh
  （graphical login、2 起動、候補の wayland）を流し、全部を戻して reboot、cksum を比べる。`plan/ws291/tests/vf2-reg.sh OUT BUILD`: vf2-regress.sh（KEEP=1）の後に
  同じ起動で vf2-mview.sh（`COPIES='1 13' SPIN=12`）、板の kernel を戻す。vf2-regress.sh・vf2-mview.sh に `VF2_DIR`（板の上の置き場、既定 p7、絶対の path も可）を足した。

### Pi 5（arm64、10.0.30.7、`CAND=build/q1860-pi sh plan/ws291/tests/rpi5-reg.sh build/tmp/q1860/pi5`、22:22〜22:35）

- offscreen（1920x1080、301 frame、TFU=1、viewer 132219966）の checksum: 板の /lib（off-a、libvulkan 4289068210）と候補の /lib（off-b、4160257452）の両方で
  c1 0c6e08c7・c21 dd5f82af・c62 90ab3e0f・c74 224da3e7（p021 の値と同じ）。

| fps | offscreen 板の /lib | offscreen 候補 | 画面 base（p3-b の program、libvulkan 482186205） | 画面 候補 | p021 の記録（P3、21:16） |
| --- | --- | --- | --- | --- | --- |
| c1 | 353.36 | 352.53 | 310.67 | 304.08 | — |
| c21 | 117.69 | 117.51 | 110.81 | 111.39 | 画面 110.6〜125.1、offscreen 117.3〜123.5 |
| c62 | 60.78 | 60.77 | 59.11 | 59.12 | offscreen 59.44 |
| c74 | 53.31 | 52.98 | 52.64 | 51.67 | 画面 51.58〜52.93、offscreen 53.5〜55.5 |

- 画面の mview は同じ起動で base と候補を続けて（kernel は候補）。両方とも 4 つの spin で game の direct scanout が終わりに `direct=0 reason=no-window frames=…`
  で閉じ、`MVIEW FAILED`・`VULKAN_ERROR` なし。base の libvulkan の source は main と同じ（p3-b の 5b25760f3 から efd494305 まで libvulkan の変更なし）。
- desktop（graphical login、候補の wayland 2111495716 と /lib の候補の libvulkan・libkeiland、frame-ops、fps、起動 1・2）: launch 60・59、type 61・61、
  output 64・69、drag 59・60、home 61・62、wiseview 63・60（P3 の p021: 60・61、61・61、65・64、60・59、60・60、62・63 と同じ帯）。
- kernel: 各 step の kernel の行に fault・lost・hang・reset・job failed なし。desktop の dmesg の該当の行は起動の self-test（V9 の hang と reset、PCIe の reset）だけ。
  libvulkan・compositor の失敗の行（device lost・handoff failed・VULKAN_ERROR・FAILED・undefined symbol・relocation）0。
- 戻し: vmunix 604485635・cmdline.txt `login=console v3d.perf=1`・/lib/libvulkan.so・/lib/libkeiland.so・/bin/wayland・/bin/aat-input・/etc/keiland/session・
  /root/p021/bin・lib の 19 file が前と cksum 一致（script の最後の読みは reboot の直後で SSH が答えず空だったので、手で読み直して一致を確かめ、script に再読みを足した）。
  板の上の退避物（*.q1860old・/root/q1860save・rpi5-desk.sh が作った /bin/wayland.p021old）は板の中で消した。

### VF2（riscv64、10.0.30.2、`VF2_HOST=kei@10.0.30.2 VF2_PASSWORD=… VF2_PLUG=AC276E432AE6 sh plan/ws291/tests/vf2-reg.sh build/tmp/q1860/vf2 build/q1860-vf`、22:41〜22:45）

- 回帰（候補の kernel と /tmp/q1860（tmpfs）に置いた候補の program・library、root）: vkmatrix 109/109、`-p 3` PASS、`-r 3` 327/327、vkcoherency PASS、vkcompute 4096/4096、
  desktop の compositor の error 0・desktop の起動 1、keiland-shot の PNG の cksum 518389155（main の q1817・q1839 と同じ）。step の間の再起動なし（boot-000314）。
- mview（game、mailbox 4 枚、1920x1080、qs40）: c1 173.86 fps、c13 56.27 fps（main b2392803f の P4 q1817: 167.97・169.04、55.99・56.04、P2 q1839: 167.1〜167.2、55.84〜56.33）。
  2 つの game の chain とも `KWL SCANOUT direct=1`・`lend images=0x3f result=0`。各 run の kernel の行に fault・lost・hang・reset・HWR・quarantine なし。
- 既存の行（main の log にも同じ形、候補の差ではない）: mview と compositor の終わりの `VULKAN kernel error errno=17 device lost`（display の lease の EBUSY、
  BUG-375 の記録と同じ行）と、compositor の起動の時の `ld.so: undefined symbol`（spawn した /bin/settings が LD_LIBRARY_PATH の library を読む）。
  P4 q1817 の vf2-regress（1・1）、vf2-p015（16・6）、P2 q1839 の r1（1・1）・m1（6・3）にも同じ行がある。
- 1 回目（22:35〜22:39、build/tmp/q1860/vf2-try1）: 置き場を ~kei/q1860 にしたら板の /（ufs 256 MB）が満杯になり、qs40 の texture の写しが途中で切れて
  mview は model の読みで `MVIEW FAILED`（回帰の部分は 2 回目と同じ結果）。自分の ~kei/q1860 は消し、2 回目は tmpfs の /tmp/q1860 で流した。
- 戻し: /boot/boot0/vmunix 3758577736・/etc/group 4260090434・zedbsd.cfg 632416842 が前と一致、/boot/boot0 の file の一覧も前と同じ（自分の vmunix.q1860old は
  一致を確かめて消した）。~kei/p7・~kei/p15 は触っていない。/ の空きは 14.8 MB（前の値は測っていない）。

### 判定（P4）

- arm64（Pi 5）と riscv64（VF2）の実機で、候補の __thread の libvulkan が読み込まれて動き、TLS の解決の誤りで落ちる事はなかった。
  試験の結果は main と同じ（checksum・vkmatrix などの数・PNG の cksum が同じ、fps は main の幅の内）、HWR・device lost 0（上の既存の teardown の行を除く）。
- 未実施: Pi 4（SwitchBot に無い、arm64 は Pi 5 で代えた）、VF2 の main の libvulkan との同じ起動の比べ（main の記録との比べだけ）、長い soak。
- 消してよい物（Q1 の pipeline へ）: build/q1860-pi・build/q1860-vf（build）、build/tmp/q1860/vf2-try1（失敗した 1 回目）。build/tmp/q1860 の他は記録の元。
