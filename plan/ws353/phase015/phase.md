<!-- awesome-plan project=zedbsd record=ws353-p015 -->

# ws353-p015: backend と WSI の小さな直し

Status: cleared（2026-11-14 P2 q2066-i01、Q1 の確認待ち。branch `agent/p2-ws353c`、a7f8bb3f34・3fabe10acb。3 機種の build warning 0、host の daemon-link 試験 PASS、QEMU（Venus）の `qemu-answer-wake.sh` が全 step PASS（4 回目。3 回目は試験の harness の stop の確かめだけ FAIL で直した）。settings の画面からの network の操作は未実施（下の「未実施」））
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.11・§6、対応表の 1-16、「poll-report に無く見つけた物」。

## 目的

daemon link の network の答えの 10 ms と、Wayland の WSI の 50 ms の slice を消す。

## 変える file

- `userland/desktop/libkeiland-backend-zedbsd/network-zedbsd.c`: `NETWORK_ANSWER_LOOK_MS` をやめ、答えの fd を link の poll（`network_poll_fill`）に入れる（自分の書きの shutdown の POLLERR は kernel の BUG-149 の直しで起きない事を確かめる）。
- `userland/desktop/libvulkan/wsi-wayland.c`: frame の進みの待ちを、接続の fd と progress pipe の poll の無期限（呼び手の期限と 1 s の pace の残り）に。`WAYLAND_WAIT_SLICE_MS` を消す。1 s の pace は残す（設計 §6）。

## 確かめ

- build warning 0。QEMU: network の scan・接続の要求の答え（settings の network の面）、FIFO の Vulkan の client が止まらない・隠れた surface の app が 1 s の pace で進む。poll の診断で daemon link（network）と client の 50 ms の起床が無い。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p001 だけ（別の file・別の thread で、main loop の Phase に依らない。いつでも並べて進めてよい。レビュー L11）。

## 結果（2026-11-14 P2、q2066-i01、branch `agent/p2-ws353c`、基点 main 504b6cc1b0）

### commit

- a7f8bb3f34: `userland/desktop/libkeiland-backend-zedbsd/network-zedbsd.c`・`userland/desktop/libvulkan/wsi-wayland.c`。
- 3fabe10acb: QEMU の試験（`plan/ws353/tests/qemu-answer-wake.sh`・`config-amd64-answer.mk`・`network-answer-test.c`・`poll-count.c`）。
- この記録（phase.md・ws.md）の commit は報告に書く。

### 実装

- network-zedbsd.c: `NETWORK_ANSWER_LOOK_MS`（10 ms）を消した。`network_poll_fill` は watch に加えて、出ている要求の接続（`request_fd`）と scan の接続（`scan_fd`）を POLLIN で link の poll に入れる（容量 `KL_DAEMON_LINK_POLL_MAX`＝8 の中で 3 つまで）。scan の frame の打ち切り（`scan_given_up_ms`、15 s）は前は 10 ms の見回りで見ていたので、期限（due）に足した。期限が無ければ link の poll は timeout 無し。
- network-zedbsd.c の `network_readable`: poll の後の `recv(MSG_PEEK|MSG_DONTWAIT)` の確かめ（自分の SHUT_WR を kernel が POLLERR と出していた頃の回避）を消し、timeout 0 の poll だけにした。kernel は BUG-149 の直しから、自分の書きの shutdown では POLLIN・POLLERR を出さない（`src/kern/net/unix-socket.c` の stream の poll、2569〜2574 行の comment と処理を読んで確かめ、QEMU でも POLLERR 0 を観測）。
- wsi-wayland.c: `WAYLAND_WAIT_SLICE_MS`（50 ms）を消した。`wayland_wait` の眠り（`wayland_sleep`、接続の fd と progress pipe の poll）は、期限（呼び手の timeout）の残りを 1 ms 切り上げた値（INT_MAX で頭打ち）、期限が無限なら -1。frame の callback はどの thread で走っても progress pipe を書くので、slice で見回る物は無い。1 s の pace（`WAYLAND_PACE_NS`）は今どおり（`wayland_commit` の FIFO の待ちの期限）。`wayland_wait` の呼び手は `wayland_commit` だけで、期限はいつも 1 s（generic の `ops->wait` を呼ぶ所は無い、grep で確かめた）。

### 確かめ

- build（-Werror、warning 0）: `make -j4 BUILD_JOBS=4 sysroot` と `build/x86_64/packages/libvulkan/.stamp`・`build/x86_64/packages/wayland/.stamp`（network-zedbsd.c は wayland の package に入る）、`CONFIGMK=config/config.rpi4.mk`（arm64）・`config/config.vf2.mk`（riscv64）で同じ。全部 rc 0、log（`build/p15-{x86,rpi4,vf2}-{sysroot,pkgs}.log`）に warning 0、2 つの file が 3 機種で compile された事を log で確かめた。
- host: `sh plan/ws309/tests/daemon-link/run.sh build/ws353-p015/host-daemon-link`（ASan+UBSan と TSan、networkd の watch・join・EBUSY・答え・ENOENT を含む）が 2 つとも 17/17 PASS。
- QEMU（Venus、x86_64）: `sh plan/ws353/tests/qemu-answer-wake.sh OUT`。判定は guest の結果の file を serial の短い命令で読む（console の log は読まない）。
  - network（`network-answer-test.c`）: 本物の `network-zedbsd.c`・`daemon-link-zedbsd.c` を `-Wl,--wrap=poll` で link し、link の thread の poll を数える（眠った待ち＝timeout で返ったか 1 ms 以上かかった物。frame の reader が届いた byte ごとに呼ぶ poll は待ちに数えない）。stand-in の networkd（join の答えは 3 s 後、scan の frame は 1 s 後）: watch の state、idle 3 s の待ち 0、join は 3002 ms で答え（error 0）、stand-in が書いてから 0〜1 ms で取れた、join の間の待ち 2・timeout 0・POLLERR 0（timeout 付きの poll は 16、frame の byte ごとの物）、scan は stand-in の network（home）が取れ待ち 4・POLLERR 0、scan の止めが stand-in に届く、close で thread が止まる。本物の networkd: watch の state、scan の要求の答え（error 0）が 5〜8 ms、待ち 1・POLLERR 0。前の 10 ms の形なら idle 3 s で 300 回、join の 3 s で 300 回の timeout になる（試験の閾値は 2・4）。
  - WSI（wltest の source に `poll-count.c` を足した `wltest-polls`。program の `poll` が dynamic loader の検索で先に来るので libvulkan の poll も数える）: compositor（`/bin/wayland --testing`）に FIFO で 120 frame（visible）: 全部 DONE、2 fd の poll（WSI の眠りと swapchain の acquire）の短い timeout（100 ms 以下）は 0、全部 1 s 近い期限。compositor を SIGSTOP で約 5.1 s 止めて FIFO で 200 frame（held）: 1 s の pace の期限で返った待ちが 2（止まっている間の present が 1 s ごとに進む）、短い timeout で返った待ち 0（50 ms の slice なら 1 s に 20）、SIGCONT の後に 200 frame 全部 DONE。
  - compositor の SIGTERM で KWL EXIT、KWL FAILED 0。
  - 1〜2 回目は試験の不備（config の変数の順、link の thread の poll の数え方と watch の最初の空の state）で FAIL、直した。3 回目（`build/ws353-p015-qemu3/summary.txt`）は network・WSI の全 step PASS で、stop の確かめだけ serial の答えの乱れで 20 s に KWL EXIT を見られず FAIL（後で読んだ log には KWL EXIT がある）。待ちを 60 s にして 4 回目（同じ image、`build/ws353-p015-qemu4/summary.txt`）が全 step PASS（`qemu-answer-wake: PASS`）。

### 設計からの変更（理由）

1. `network_readable` の MSG_PEEK の確かめも消した（phase.md は答えの fd を poll に入れる事だけ）。回避の理由（自分の SHUT_WR を POLLERR とする kernel）は BUG-149 で直っていて、comment が今の kernel と食い違う名残りだったため。答えの前に timed read が始まらない事は QEMU の join（3 s 後の答えを error 0 で取る、POLLERR 0）で確かめた。
2. scan の frame の打ち切り（15 s）を link の poll の期限に足した。10 ms の見回りが消えると、答えない daemon の打ち切りを見る時が無くなるため。

### 未実施

- settings の network の画面からの操作（scan・接続の要求を画面で出して答えを見る）。QEMU に Wi-Fi が無く、compositor の network の画面は同じ backend（`kl_backend_network_*`）を通るので、backend を直に使う試験（stand-in と本物の networkd）で代えた。
- 前の形（10 ms・50 ms）の build で同じ試験を流して数が閾値を越える事の確かめ（試験の閾値は code から見積もった値）。
- 実機（5330 の idle の起床の測り）は p016。
