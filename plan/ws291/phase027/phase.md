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

## 回帰 amd64（P3 q1859-i01、2026-11-01 22:16〜23:15、worktree p3-ws291reg）

- 対象: 候補 60cf76c13（main efd494305 に P1 の agent/p1-ws291p026 を merge）と main efd494305。2026-11-01 ユーザーから、WS291 は各 architecture で
  回帰を流して merge を急ぐ、という趣旨の指示を受けた（Q1 経由）。source は変えていない。
- build（`plan/ws291/tests/config.mk` の vmunix と 14 file）: 候補は `BUILD=build/p3reg/cand`、main は git archive efd494305 を
  build/tmp/p3reg/maintree に展開して `BUILD=build/m`。両方 rc=0、tree の warning 0（warning の行は外の OpenSSL と perl の locale だけ）。
  vmunix は両方 3328626646（kernel の source は同じ）、14 file で違うのは libvulkan だけ（main 509112954、候補 3050428356 = P1 の bu と同じ）。
- 実機の script: build/tmp/p3reg-hw/session.sh（git の外、lock /tmp/zedbsd-rdna.lock の中、22:23〜23:14）。A = `rtx-p024.sh` ROUNDS=2 SHOT=1、
  D = `plan/ws309/tests/rtx-p015.sh` PARTS='cycle scenes desk'、E = 新しい `plan/ws291/tests/rtx-apps.sh`（App Home から files・terminal・settings・
  browser を開き、process・画面・終了）。要約は build/tmp/p3reg-hw/{a,d,e}/summary.txt。

### A: 6 つの bench（fps、round 1 / round 2）

腕: main = main の libvulkan、cand = 候補、off = 候補に `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0`、offm = main の libvulkan に同じ変数（main は読まない。
環境の差だけを見る）。kernel は全部 3328626646、他の 13 file は候補の物（main と同じ cksum）。TIMING=0・PERF=0。

| bench | main | cand | off | offm |
| --- | --- | --- | --- | --- |
| big-100 | 586.9 / 574.4 | 580.0 / 577.8 | 573.6 / 562.3 | 555.8 / 578.9 |
| big-1000 | 113.5 / 97.6 | 114.9 / 115.1 | 101.4 / 107.0 | 106.2 / 101.4 |
| subs-100 | 461.8 / 470.9 | 619.5 / 625.4 | 477.1 / 477.1 | 456.8 / 476.8 |
| subs-1000 | 52.9 / 52.9 | 83.8 / 81.2 | 57.0 / 51.2 | 54.8 / 54.8 |
| scene-1500 | 157.4 / 157.4 | 157.2 / 157.6 | 157.4 / 157.8 | 156.1 / 156.8 |
| scene-5000 | 49.5 / 49.9 | 49.9 / 49.9 | 49.9 / 49.8 | 49.7 / 49.8 |

- 候補は main の幅の内か上: subs-100 +32〜35 %、subs-1000 +54〜58 %（record 6.5〜6.9 ms・submit 4.7〜5.4 ms 対 main record 6.3・submit 12.2）。
  big-100 は main の幅（574.4〜586.9）の内、scene-1500・5000 は同じ。
- 画素の比べ: 29（4 腕 × 2 round の still 3 つを最初の腕と、one と subs）が全部 max 0。mview の still は全部 frames=900 で終わった。
  各 bench の dmesg に fault・lost の行 0。
- **big-1000 の B-U を切った腕の −6 %（P1 の記録）**: off は 101.4・107.0 で再び main の round 1（113.5）より低い。ただし main 自身が round 2 で
  97.6（record 5.24 ms 対 3.84 ms）、main の libvulkan に同じ変数を付けただけの offm も 106.2・101.4。よって off の低さは B-U の code の切り替えでは
  なく、big-1000 の record の時間の揺れ（この機械で約 97〜115 fps）の内で、変数の有無（環境の大きさ）でも出る。候補の既定（cand）は 2 round とも
  114.9・115.1 で main の上限の側。原因（配置・cache の推測）は未確かめ。

### D: game mode の往復・画面・desktop の frame-ops（rtx-p015.sh、main と cand）

- 往復: 両腕とも 3/3（direct=3 left=3 window=3 failed=0 refused=0 compositor=1）、lend images=0x3f result=0。
- 画面（desktop・notes・browser）: cand と main の差は右上の時計（x=1868..1894 y=17..27）だけ。ただし、この起動では Settings の初回の
  案内（Welcome to zedBSD）の窓が前に出ていて、notes・browser の画面にも Notes・browser の窓は写っていない（両腕で同じ。script の前提が
  機械の状態と合わない、後退ではない）。
- frame-ops（fps、main / cand）: launch 63 / 64、type 61 / 61、keys 250 / 250（入力の間隔）、output 69 / 67、scroll 19 / 19 と hover 23 / 22
  （入力の間隔）、drag 61 / 60、home 63 / 64、wiseview 63 / 65。各段が main −2 fps の内。
- kernel の行: 両腕とも起動の時の `vram-window bar1 before the tables: fault ...`（と cand の起動では `retry 1 (the fault record was cleared)`）だけ。
  E の main の起動にも retry の行があり、起動ごとに出る既知の形で、GPU の仕事の fault・lost・executor fail は 0。

### E: desktop の app（rtx-apps.sh、main と cand）

| app | main | cand |
| --- | --- | --- |
| files | process 2、窓あり | process 2、窓あり（app-files.png） |
| terminal | process 1、窓あり | process 1、窓あり |
| settings | process 1 | process 1 |
| browser | process 0、窓なし | process 0、窓なし |

- 各 app を終えた後の残り 0、compositor は終始 1。cand と main の画面の差は時計だけ。
- settings: 初回の案内の Settings が既に動いているので、process 1 は新しい起動と見分けられない（未確かめ）。
- browser: 両腕とも App Home から開かなかった（main も同じなので候補の後退ではない）。原因（App Home の検索語、8 s の待ち、NVMe の root に
  /bin/browser があるか）は未確かめ。

### Venus（host の QEMU、候補の image）

- image: `plan/tools/gpu-boundary/build-forge-image.sh build/p3reg/forge`（cksum 3366497359）、`test-image.sh plan/tools/rtx-video/config-amd64-venus-video.mk
  build/p3reg/video`。どちらも rc=0。guest の uname は `1.0.0-beta2+g60cf76c`。
- boot-test.sh（forge の image、uefi-nvme）: PASS、build/tmp/p3reg/boot-test/login.png（console の login）。
- `plan/ws291/tests/venus-reg.sh`: **PASS**。cand・main の libvulkan とも forge-guest・fence-guest（600 frame）・wltest（mailbox と作り直し）・
  mview mailbox・mview submits・compositor の VULKAN_ERROR 0。
- glass の desktop に mview の窓（`--windowed`、10 体）が出て描く: build/tmp/p3reg/venus-mview-ab/mview-cand.png、30 s で status 0（14.2 fps、lavapipe）、
  VULKAN_ERROR 0。同じ guest で main の libvulkan に入れ替えた後の腕は、動いている compositor の写した libvulkan.so をその場で上書きしたため
  compositor が落ちて無効（試験の作りの誤り。main の mview は venus-reg で ok）。
- Video Player（`plan/tools/rtx-video/venus-videoplayer.sh`、clip は tree の plan/tools/media/sample.mp4 の MPEG-4 320x240）: own presenter
  （`PRESENT own=1 gpu_pictures=1`）、347 frame を 16 s で表示、FAILED 0、build/tmp/p3reg/vp/play.png に窓と画。

### 判定と未実施

- 判定（P3）: amd64 で後退なし。fps は main の揺れの内か上、画の比べ一致、device lost・fault・executor fail 0、game mode の往復 3/3、
  frame-ops は main −2 fps の内、Venus の回帰 PASS。cleared の判定は Q1。
- 機械の状態（23:14）: Debian 6.12.111、RTX の 4 function は vfio-pci、NVMe の kernel 1463007375 と 14 file を戻し cksum が expected と一致、
  cfg の nvrtx.vk.perf・nvrtx.disp.bl の行 0、lock は free。
- 未実施: Notes・browser の起動の確かめ（上の理由）、settings の新しい起動の見分け、hand-off の数の timing（TIMING=1）、big-1000 の揺れの原因、
  arm64・riscv64（この Queue は amd64 だけ）。
- 消してよい物（Q1 の pipeline へ）: build/tmp/p3reg（main の git archive と build、image の起動の作業 dir、Venus の runtime）、build/p3reg/forge・
  build/p3reg/video（image 各約 2.4 GB）、build/p3reg/cand。計測の出力 build/tmp/p3reg-hw は記録の元なので残すかは Q1 の判断。
