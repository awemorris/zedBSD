<!-- awesome-plan project=zedbsd record=ws353-p012 -->

# ws353-p012: cursor の thread（acquire は変化の後だけ、copy の fence をすぐ待つ）

Status: cleared（2026-11-14 P1 q2076-i01、Q1 の確認待ち。branch `agent/p1-ws353k`、8a198aee05、main の merge a92bfc0158・dd952aa9c3。3 機種の build warning 0、host 試験 `cursor-step`（ASan・TSan）PASS（基点の code では 11 件 FAIL）、QEMU（Venus）の `qemu-render-wait.sh` は merge の後の image で 2 回全 step PASS（回帰だけ。Venus に cursor plane が無く cursor の thread は動かない）。cursor plane の実の確かめ（移動・copy・drag・game mode・sleep の park）は実機が無く未実施、p016 へ申し送り）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.7、対応表の 1-09〜1-12・2-07。

## 目的

cursor の thread の 1 ms の再試行と 5 ms の pin の見回りを消す。idle で acquire に block しない。

## 変える file

- `cursor-thread.c`・`cursor-thread.h`: box の変化の後に image を timeout 0 で取り、取れなければ `vkAcquireNextImageKHR(UINT64_MAX)`（cursor の chain の image は前の cursor の present の job が終われば空く。先回りの acquire はしない。設計 §5.7、レビュー S4）。copy を submit したらその fence を無期限で待ち（今の present の経路の `KWL_CURSOR_FENCE_WAIT_NS` 100 ms、cursor-thread.c:477 を無期限に）、pin を外し、main に wake（cursor-plane.c の手放しの見張りを p005 で消した物と合わせる）。`CURSOR_RETRY_US`・`CURSOR_PIN_POLL_US`・`cursor_unpin_done` を消す。移動の hold（refresh の終わり近くまで）は cond の期限として残す。
- `cursor-plane.c`: 要る所だけ。

## 残す振る舞い

- 移動を 1 refresh に 1 回、最新の場所で present する事（ws213 の C0）、新しい像は hold しない事、park の答え（上限 2 s、`PLANE_PARK_WAIT_MS` cursor-plane.c:54）、停止の時の copy の fence の待ち（`kwl_cursor_thread_finish` の 100 ms と `stop_failed`、打ち切りとして残す。レビュー L6）。

## 確かめ

- build warning 0。QEMU: pointer の移動・client の cursor の像の変更・drag の icon、game mode の出入りの後の cursor plane、sleep の前の park（`sleep_hold`）が 2 s 以内に答える。WS352 の poll の診断で cursor の thread の timeout の起床が、移動の間の hold の他に 0。

## 受け入れ

- 上の判定が PASS。結果・SHA・未実施を書く。

## 依存

- p011（同じ queue の lock と acquire の扱いを揃える）。

## 結果（2026-11-14 P1、q2076-i01、branch `agent/p1-ws353k`、基点 main c3107a0d0b）

### commit

- 8a198aee05: `cursor-thread.c`・`cursor-thread.h`（cursor の thread の待ちの作り直しと数え）、`cursor-box.h`（stats の comment の 1 語）、host 試験 `plan/ws353/tests/cursor-step/`。
- a92bfc0158: main（23a9da01e5、printers-shell.c の `UNUSED_PARAMETER(server)` を含む）の merge。
- dd952aa9c3: main（179f3b1bd2、p010 を含む）の merge。衝突は無かった（main は cursor の file を触っていない）。
- この記録（phase.md・ws.md）と `qemu-render-wait.sh` の 1 行（guest の log の控えに `KWL CURSOR_PLANE` の行を足す）の commit は報告に書く。

### 実装

- 消した物: `CURSOR_RETRY_US`（1 ms の再試行）、`CURSOR_PIN_POLL_US`（5 ms の pin の見回り）、`cursor_unpin_done`（`vkGetFenceStatus` の覗き）、`CURSOR_RETRY` の結果、`pinned[]`（swapchain の image ごとの box の copy の pin）、present の経路の `KWL_CURSOR_FENCE_WAIT_NS`（100 ms）の待ち。
- acquire（新しい `cursor_acquire`）: box の変化の後の present でだけ呼ぶ。timeout 0 で一度取り、`VK_NOT_READY`・`VK_TIMEOUT` なら `UINT64_MAX` で待つ（再試行は無い）。先回りの acquire はしない。`UINT64_MAX` の acquire が `VK_NOT_READY`・`VK_TIMEOUT` を返したら（規格では無い）失敗として扱い、thread は Vulkan を止めて main に知らせる（plane を閉じ cursor を合成に戻す今の失敗の道）。
- copy: image の前の copy は present の後に終わりまで待ってあるので、fence は待たずに reset して submit。present の直後に、その copy の fence を `vkWaitForFences(UINT64_MAX)` で待ち、box の copy の pin を外す（`copy_wanted` なら main に wake、p005 の形のまま）。present が失敗しても copy は submit 済みなので先に待ってから失敗を知らせる。fence の待ちの失敗（device lost）は失敗として知らせ、pin は残す（main が plane を閉じ box を作り直す）。
- 待ちは一度に一つ: box の変化（期限なし）、hold の期限（移動の hold だけ）、acquire（変化の後に空きが無い時だけ）、copy の fence（copy の後だけ）。idle の thread は box の cond だけで眠る。
- 停止: `kwl_cursor_thread_finish` の 100 ms の待ちと `stop_failed` は打ち切りとして残した（設計 §5.10、レビュー L6）。present の経路で全部の copy を待つので、停止の時の fence は普通は既に signal している。
- 数え（`struct kwl_cursor_counts`、thread だけが書き join の後に読む）と、thread の最後の 1 行 `KWL CURSOR_PLANE stopped presents= copies= copy_waits= acquires_now= acquires_waiting= sleeps= holds=`。`holds` は期限付きの待ちの数（移動の hold だけ）、`sleeps` は期限なしの待ち。
- `cursor-plane.c` は変えていない（手放しの wake は p005 で既に `copy_wanted` の形。park の 2 s は `PLANE_PARK_WAIT_MS` のまま）。

### 確かめ（実行したコマンドと結果）

- host: `sh plan/ws353/tests/cursor-step/run.sh`（新しい試験。`cursor-thread.c` を偽の Vulkan（acquire・fence・submit・present を test が持つ）と本物の thread で動かす。ASan+UBSan と TSan）が両方 PASS（8a198aee05 で 2 回、dd952aa9c3 で 1 回）。場面: E（空の plane の最初の present は copy・present の後に fence を期限なしで待ち、signal まで他に何もしない）、I（idle で acquire も present も 0）、C（client の cursor の copy 0 は GPU が読む間 pin、copy の終わりで pin を外し `copy_wanted` の main を 1 回 wake）、H（静かな refresh の後の最初の移動はすぐ、直後の移動は hold の期限まで待ち copy なしで最新の場所に）、A（image が全部 display に持たれている時、変化の後に timeout 0 を 1 回、`UINT64_MAX` を 1 回、再試行なし、image が空いたらすぐ present）、P（idle の park はすぐ答え、park の間は何も呼ばず、終わりの後に移動を present）、B（acquire に block している間の park は image が空いた後に答える、2 s の内）、N（新しい像 copy 1 は hold せず copy、pin を外し、wake を頼まれていなければ wake しない）、S（停止の fence の待ちだけが期限付き、acquire は 0 か `UINT64_MAX`、present の経路の fence の待ちは全部 `UINT64_MAX`、copies = copy_waits = 3、acquires_waiting 2、holds 1）、F（present が OUT_OF_DATE で失敗しても copy を待ってから失敗を知らせ、pin を外し main を wake）。
  - 同じ試験を基点の `cursor-thread.c`（`git show c3107a0d0b:…`、数えの確かめを除き `vkGetFenceStatus` の偽を足した写し）で build すると 11 件 FAIL（copy の fence を present の後に待たない、acquire の 1 ms の再試行、block の acquire が無い、停止の外の 100 ms の待ち、失敗の present の前に copy を待たない など）。
- build（-Werror）: 8a198aee05 で `make -j4 BUILD_JOBS=4 sysroot` と `build/x86_64/packages/wayland/.stamp`、`CONFIGMK=config/config.rpi4.mk`（arm64）・`config/config.vf2.mk`（riscv64）で sysroot と `build/<arch>/packages/wayland/.stamp`: 3 機種 rc 0・warning 0、cursor-thread.c の compile を log で確かめた（`build/p12-x86-pkgs2.log`・`build/p12-{rpi4,vf2}-pkgs.log`）。dd952aa9c3（merge の後）で 3 機種の wayland の package を作り直して rc 0・warning 0、cursor-thread.c・compose.c の compile を確かめた（`build/p12-m2-{x86_64,rpi4,vf2}.log`）。
  - 注: compositor の package の compile の flag（`build/x86_64/packages/wayland/.flags`、`userland/base/package.mk` の `STANDALONE_PROGRAM_CFLAGS`）には `-Wall`・`-Wextra` が無い。そこで同じ flag に `-Wall -Wextra -Werror` を足した target の clang の `-fsyntax-only` でも `cursor-thread.c`・`cursor-plane.c`・`cursor-box.c`・`printers-shell.c` が 0 件である事を確かめた（merge の後）。基点の `printers-shell.c` はこの形で unused parameter の error（main の `UNUSED_PARAMETER(server)` で直った）。host の試験の build は gcc の `-Wall -Wextra -Werror`。
- QEMU（Venus、x86_64）: Venus の display には cursor plane が無い（`src/drivers/gpu/venus/display.c:168`）ので、QEMU では cursor の thread は起動しない（guest の log に `KWL CURSOR_PLANE` の行が無い事を確かめた）。QEMU は回帰だけ: `IMAGE=… sh plan/ws353/tests/qemu-render-wait.sh`（`CONFIGMK=plan/ws353/tests/config-amd64-render.mk` の `userland`・`disk` で作った image、build の warning 0）。
  - a92bfc0158 の image（`build/ws353/p12m.img`）: 全 step PASS（`build/ws353/p12-qemu-1/summary.txt`。stop の行 jobs=1260 fence_waits=1260 tasks=6 task_waits=6）。
  - dd952aa9c3（p010 を含む main の merge の後）の image（`build/ws353/p12m2.img`）: 2 回とも全 step PASS（`build/ws353/p12-qemu-m2-{1,2}/summary.txt`。stop の行 jobs=1151・1154、fence_waits は jobs と同じ、tasks=6 task_waits=6。errors 0）。wlshm 600 frame は約 41 s（merge の前の 24.5 s より遅い）。cursor の thread はこの guest で動かないので、p010 の記録の Venus での遅さ（wlshm 1 frame 41→68 ms）と同じ物と読む（p012 の変更に依らない）。

### 設計からの変更（理由）

1. copy の fence は「submit したらすぐ」ではなく「present の直後」に待つ。present は copy の semaphore を待つので、先に present を積んでから待つと present の遅れが増えない。待つ物と期限（無期限）は設計どおり。
2. block の acquire の後は、その前に読んだ box の写しで present する（acquire の後に box を読み直さない）。次の step が新しい変化を見て直ぐに present する（hold は present の始めから数えるので、acquire で待った分は hold に入る）。
3. 停止の時の 1 行 `KWL CURSOR_PLANE stopped …`（数え）を足した（phase.md に無い）。WS352 の kernel の poll の診断の代わりに、期限付きの待ちが hold だけである事を数えで見るため。p011 の `KWL RENDER stopped` と同じ形。
4. `UINT64_MAX` の acquire が image を返さない（規格に無い）時は失敗として plane を閉じる（空回りしない）。

### 未実施

- cursor plane の実の確かめ（pointer の移動・client の cursor の像の変更・drag の icon、game mode の出入りの後の cursor plane、sleep の前の park が 2 s 以内に答える）: QEMU（Venus）に cursor plane が無く、実機の割り当てが無いので未実施。host 試験で thread の待ちの形（acquire・copy の fence・pin・park・hold・失敗）だけを確かめた。p016 の 5330 で見る（下の申し送り）。
- WS352 の kernel の poll の診断: 未実施（`KWL CURSOR_PLANE stopped` の数えで代える。実機で見る）。
- arm64・riscv64 は build だけ。

### p016 への申し送り（5330 など cursor plane のある実機で、plane の経路が通ったと言える事）

compositor を `--log-frames` で起こし、log を file に取る。判定は log の行の数で（画面の目視は補助）。

1. 起動: `KWL CURSOR_PLANE open plane=… images=…` が 1 行。`KWL CURSOR_PLANE unavailable` が無い事（あれば plane の経路は通っていない）。
2. pointer の移動（数秒、なるべく速く）: `KWL CURSOR_PLANE present x= y= copy=0` が続き、x・y が動きに合う。`KWL PERF cursor … presents= held= copies= present_gap_max_us=` で presents が移動の間の refresh の数に近く（60 Hz なら 1 s に 60 前後）、`present_gap_max_us` が 1 refresh と少し（約 17〜20 ms）以下。copies は移動では増えない。
3. client の cursor の像の変更（text の field の上で I-beam、app の resize の縁など、wl_shm の cursor を出す client）: その時だけ `KWL CURSOR_PLANE present … copy=1`。像を何度か替えても `KWL CURSOR_PLANE failed` が無く、像が古いまま残らない（2 つの copy の pin が外れ main が書けている）。
4. drag の icon: drag の間は cursor が合成に回り plane は空（`present … copy=1` が一度出て、その後の移動は合成の frame）。drag の終わりで plane に戻り `present … copy=1`。
5. game mode の出入り（mview --content=game など）: 入る時に park（`KWL FAILED site=park … thread=cursor` が無い）、出た後の移動で `KWL CURSOR_PLANE present` が再び出る。
6. sleep（lid か menu の Sleep）: 前の park が 2 s 以内に答える（`KWL FAILED site=park … thread=cursor step=hold` が無い）、resume の後の移動で `present` が出る。i915 は suspend の間 present の worker が止まるので（p011 の申し送り）、resume の後に cursor が動く事まで見る。
7. 停止（compositor の終了、または plane を閉じる出力の切り替え）: `KWL CURSOR_PLANE stopped presents=P copies=C copy_waits=W acquires_now=N acquires_waiting=A sleeps=S holds=H` で、W = C（copy ごとに 1 回の待ち）、H ≤ P（期限付きの待ちは hold だけ）、acquires_now + acquires_waiting ≥ P（失敗の無い時は =）。A は普通は 0 か少し（多ければ plane の present の job が長い）。`KWL CURSOR_PLANE failed` が 0。
8. idle の起床（任意）: 移動を止めて 60 s 置いた run と、すぐ止めた run の `stopped` の行の sleeps・holds が同じなら、idle で cursor の thread は起きていない（WS352 の poll の診断があればそれで timeout の起床が 0 である事）。
