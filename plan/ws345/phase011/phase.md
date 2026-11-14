<!-- awesome-plan project=zedbsd record=ws345-p011 -->

# ws345-p011 受け入れと回帰

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: in-progress（2026-11-15 P3 q2097-i01: 受け入れの試験を全部流した。R12（headless の `--threads`）は p015 待ち、Wine の窓の待ちの上限は Wine 自身の一回の止まり（下）で未達。cleared の判定は Q1。下の「結果（2026-11-15 P3）」）

## 目的

WS345 の目標（窓の event が止まらない）を測って確かめ、全体の回帰を流す。

## 手順と試験

1. `plan/ws345/tests/stall-probe.c` を `_OWN` の形に直した受け入れの試験 `plan/ws345/tests/threaded-latency.sh`（窓の代わりの thread と描画の代わりの thread）: 次の各場面で、窓の代わりの loop の event の遅れの最長が 20 ms 以下、100 ms を超える event が 0、描画の代わりの thread は resize の後 100 ms 以内に present する。
   - timer の script が 10 s 回る、mousedown の listener が 2 s 回る
   - 同期の XHR が connect の掛かる listener を待つ（30 s、loader の時間切れまで）、read の掛かる listener を待つ
   - `document.write` の `<script src>` が connect の掛かる listener を待つ
   - 6000 個の box の頁の resize、3000 × 3000 の PNG の decode
   - cross-origin の iframe の無限 loop
2. ThreadSanitizer の variant（p002 の `tsan`）で 1 の全場面と host-threaded-view・host-frames。
3a. guest の計測（1 本の script `plan/ws345/tests/guest-latency.sh`、配置 → 起動 → 実行 → 回収 → 要約）: zedBSD の QEMU と Windows の Wine で、browser を到達しない address（`192.0.2.1`、TEST-NET-1）への同期の XHR と 10 s の loop の頁で開き、shell の窓の thread が受けた event の時刻と取った時刻の差の最長を shell の診断の行（`BROWSER_LATENCY` の時だけ、終わりに stdout へ 1 行）で集める。guest の stdout は SSH（Wine は host の process）で読み、QEMU の console・serial の log では判定しない。入力は WS074 の guest の道具で送る（Q1 が割り当てる名前・port）。上限は 1 と同じ。
3. 回帰: host-build（plain・asan・tsan）warning 0、host-* が前と同じ、js、test262 の部分集合（前後で落ちた一覧が同じ）、http・http-async・loader・fetch・component、WPT の reftest の見本と DOM の試験、段 1 の前後、cross-build、BUG-411 の bench。component・WPT の見本・DOM の試験・段 1 の host の試験は、headless の `--threads`（`_OWN` の道）でも流す（R12）。
4. zedBSD の QEMU（boot-test.sh と browser の画面の PNG）、Linux の Keiland の guest（SSH と画面の PNG）、Windows の Wine。本物の Windows・実機はユーザーが拾える試験の一覧に（Q1）。

## 受け入れ

- 1 の上限を全場面で満たし、2・3 に新しい失敗が無く、4 で三つの platform が頁を出す。

## 結果（2026-11-15 P3 q2097-i01）

基点 main 81a18eb954。commit 89413084a8・def1f9cd59（試験の道具・image の config・p015）、a252fe910e・0959f21e7d（shell の計時と試験の直し）、b9efdcd777（Wine の対照）、記録の commit。build は空の build/ から（`make CONFIGMK=config/config.<機種>.mk llvm-toolchain-cached` → `sysroot` → `build/<機種>/packages/browser/.stamp`、symlink 無し）。

### 作った物・直した物

- `tests/threaded-latency.c`・`.sh`（新）: `stall-probe.c` を `_OWN` だけにした受け入れの試験。窓の代わりの loop（5 ms ごとの event）と描画の代わりの thread（`browser_view_render_pixels`、resize から新しい大きさの描画の終わりまでを計る）、10 場面（idle・loop 10 s・handler 2 s・sync-xhr-connect・sync-xhr-read（loader の 30 s まで）・docwrite-connect・async-connect・resize-big・decode-big・iframe-loop（第 2 の origin の無限 loop の iframe と頁の interval））。各場面に頁がその事をした印（console の行、iframe-loop は頁の tick 10 以上）。上限: 窓の event の最長 20 ms 以下・100 ms 超 0・resize の present 100 ms 以内・loop が終わりに止まっていない。tsan の variant は時間を判定せず（2 倍の秒）、ThreadSanitizer の報告 0 と印。
- `tests/guest-latency.sh`（新）・`tests/pages/latency.html`: 1 本の script で zedBSD の QEMU（Venus と `--no-gpu`）と Wine（lavapipe と `--no-gpu`）の browser を 192.0.2.1 への同期の XHR と 10 s の loop の頁で開き、pointer を 0.5 s ごとに動かし、Ctrl+Q の後の `ZBROWSER LATENCY` の行を判定（最長 20 ms 以下・100 ms 超 0・標本 100 超・頁の script の終わり）。guest の行は SSH、Wine は host の process の stdout。Wine の後に対照 `tests/wine-post-probe.c`（browser の無い Windows の program、判定しない）。
- `tests/config-amd64-browser.mk`（新、2026-11-14 Q1 の案）: 既定の x86_64 の image の program（browser・libbrowser・compositor・自前の ssh・Venus）に browser-probe、firmware と入力法を外し、console の login。`shell-guest.sh` はこれを使う（`plan/ws074/tests/config-amd64-browser.mk` は今の build が読まない `ZEDBSD_USER_PROGRAMS` と、今の tree で作れない openssl・openssh を含むので使わない）。
- `tests/wpt-cross-origin.py`・`tests/wpt-hosts.c`（新、範囲は 2026-11-14 Q1 の判断、p010 の phase.md にも書いた）: WPT の `html/browsers/origin/cross-origin-objects/*.html`・`webmessaging/*.htm(l)`・`webmessaging/without-ports/*.html` を suite 自身の `wpt serve`（P3 の port 18310〜）で、WPT の host の名は getaddrinfo の LD_PRELOAD の shim で 127.0.0.1 に（host の /etc/hosts は変えない）、`browser --run --async` で。
- `userland/desktop/browser/shell/shell.c`: 窓の thread の診断の計時（`BROWSER_LATENCY`）を event の loop の直前から始める。前は窓の起動の仕事（問いの font の読み込みなど）の前から計り、起動直後の最初の tick が毎回 27〜34 ms 待って（計時の行で 0.032 s と確かめた）上限を超えていた。起動の仕事は event の待ちではない。
- `shell/latency.c`・`internal.h`: LATENCY の行に `longest_at_s`（最長の待ちの時刻）。どこで待ったかを試験の要約で読めるように。
- `shell-guest.sh`: (1) `wait_more` を guest の中の 0.1 s の poll（1 回の SSH）に。前は SSH の往復ごとに見たので問いの行を数秒遅れて見て、Wait（Enter）の窓を逃して FAIL した。(2) B の resize の判定を browser の log の順（新しい大きさの最初の FRAME が script の end の行より前）に。前は frame を見た後に数え、その間に XHR が時間切れで終わると誤って FAIL。(3) E の問いの時刻は shell の `seconds=`（task の走った時間）で判定（host が spin start の行を読むのは session の起動の SSH の分だけ遅れる）。`pages/spin.html` を 17 s に（Wait の窓 7 s、2 度目の問いは 20 s）、`shell-wine.sh` の説明を合わせた。

### 試験（host）

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`asan`・`tsan` | 3 つとも exit 0、warning 0、built 104 |
| `sh plan/ws345/tests/threaded-latency.sh plain` | `threaded-latency: PASS (10 scenarios, plain)`。窓の event の最長 0.1〜6.9 ms（loop 3.5、sync-xhr 4.0、resize-big 6.9）、20 ms 超 0、resize の present の最長 1.5〜45.2 ms（resize-big 45.2、decode-big 29.7）、late 0。印: loop の spun 10454 ms、handler 2503 ms、xhr ended 30474・30527 ms、docwrite の after 30172 ms、decoded 1873 ms、iframe-loop の頁の tick 76 |
| `sh plan/ws345/tests/threaded-latency.sh tsan` | `threaded-latency: PASS (10 scenarios, tsan)`、10 場面とも ThreadSanitizer の報告 0（初回は試験の側の thread の detach 漏れの報告と、tsan の遅さで handler の押しが listener の前・decode が 8 s に収まらない事を試験側で直した） |
| `sh plan/ws345/tests/host-frames.sh plain`・`tsan` | 両方 `33 checks, 0 failed`、tsan の報告 0 |
| `sh plan/ws345/tests/host-threaded-view.sh plain`・`tsan` | plain `55 checks, 0 failed`。tsan は報告 0、失敗 2 は p010・p014 の記録と同じ tsan の遅さの時間の上限（(h) 1749.6 ms、caller の navigation-wait 778.55 ms） |
| `run-test262.py --driver build/ws345-host/plain/host-js --jobs 3`（suite 7ab7fafa、`fetch-suites.sh test262` で取得・license 確認）と基点 6c1af97bc1（p010 の前）の host-js | 両方 `test262 32069/47792 (67.1%) es5 7908/8087`、落ちた一覧 15723 件が同じ（sort して diff 無し） |
| `python3 -I plan/ws345/tests/wpt-cross-origin.py --jobs 3`（suite 2d66b9b7、`fetch-suites.sh wpt`）と基点の browser | 両方 `69 files, 55 with results, 40/80 subtests passed`、results.txt の diff 無し。`_CALLER` の道（headless）なので他の agent の frame は通らない。`_OWN` の道は p015 の `--threads` の後 |
| `sh plan/ws334/tests/stage1.sh TREE plain`（今の tree と基点 6c1af97bc1 の `git archive`、port offset 0・100） | 今: host-build warning 0・built 104、host-* 86 exit 0・15 other、js 21/21、loader 11/11、http 15/15、http-async 19/20（`async-sheets`、前から）、fetch 38/38、component 83 checks PASS。基点: built 102、82 exit 0・17 other、他は同じ数。exit の一覧の違いは今に足された host-change・host-restyle-scope（0）と、基点で 2・今 0 の host-xml・host-xml-dom（WS345 の外の直し）。新しい失敗は無し |
| `run-dom-tests.py`・`run-cross-origin-tests.py`・`run-wpt-reftests.py`（既定の見本 100 件）を今と基点の browser で | dom-tests 39/42 が両方（出力の diff 無し）、cross-origin-tests 21/21 が両方、reftest pass 62・fail 37・error 1 が両方で各 test の結果が同じ |
| `sh plan/ws074/tests/bench/bug411.sh 基点 今` | big-object-reads 1062 / 1029 ms、compile-many-names 14 / 12、computed-after-change 36 / 2、grow-with-lookups 73 / 72、check の値が同じ |
| `make -j2 CONFIGMK=config/config.{x86_64,rpi5,vf2}.mk build/<機種>/packages/browser/.stamp`（0959f21e7d の tree、shell.c・latency.c を build し直した） | 3 つとも exit 0、warning 0 |
| `BUILD_JOBS=2 sh plan/ws334/tests/cross-build.sh`（b9efdcd777 の tree） | `cross-build: PASS`（前の tree でも PASS） |

### 試験（zedBSD の QEMU、Venus、KVM）

image: `plan/tools/guest/test-image.sh plan/ws345/tests/config-amd64-browser.mk build/ws345-p007-image`（ZEDBSD_JOBS=4、tree の ws074・ws345 の頁を複写）、warning 0、`check-x86_64-hdd-image: PASS`。guest は GUEST_RUNTIME=build/p3-guest-run。Venus の renderer は main の build/ に無く、他の worktree の `build/ws035-sq-venus/install` を自分の build/ に複写した（virgl_render_server の sha256 は tools の記録と同じ 745bef59…、libvirglrenderer.so.1.9.0 は記録の 38447dea… と違う dae4cdf2…、Q1 へ）。判定は SSH の browser・compositor の log と VNC の PNG、serial は読まない。

| 試験 | 結果 |
| --- | --- |
| `GUEST_RUNTIME=build/p3-guest-run sh plan/ws345/tests/shell-guest.sh build/tmp/p3-shell-guest7 build/ws345-p007-image/hdd-image.img` | `shell-guest: 27 passed, 0 failed`、`shell-guest: PASS`。窓の thread の最長: busy 17.6・xhr 9.4・busy-cpu 16.3・prompt 1.9・prompt-cpu 14.8 ms、全部 20 ms 超 0。問いは script の 10 s で出て、Stop・Wait とも PASS。画面は build/tmp/p3-shell-guest7/*.png |
| `GUEST_LATENCY_CASES="qemu qemu-cpu" sh plan/ws345/tests/guest-latency.sh build/tmp/p3-guest-latency-qemu4 build/ws345-p007-image/hdd-image.img` | `guest-latency: PASS`。qemu `samples=7089 longest_ms=14.2 over_20ms=0 over_100ms=0`、qemu-cpu `samples=7049 longest_ms=17.9 over_20ms=0 over_100ms=0` |
| 途中の run（記録のため） | 直す前の shell では毎回 over_20ms=1（最長 27〜34 ms、計時の行で起動直後 0.032 s）。shell-guest の 2 回は、私の前の run の残りの process が同じ runtime の guest を止めて途中から「no guest」になった（私の運用の誤り。最後の run では無し） |

### 試験（Windows の Wine、Xvfb、lavapipe）

| 試験 | 結果 |
| --- | --- |
| `sh plan/ws345/tests/shell-wine.sh build/tmp/p3-shell-wine` | `shell-wine: 23 passed, 0 failed`、`shell-wine: PASS`。窓の待ち最長 5.5〜16.2 ms、20 ms 超 0 |
| `GUEST_LATENCY_CASES="wine wine-cpu" sh plan/ws345/tests/guest-latency.sh build/tmp/p3-guest-latency-wine2` | FAIL: wine `longest_ms=33.9 over_20ms=1 over_100ms=0 longest_at_s=28.047`、wine-cpu `55.6 … over_20ms=1 … longest_at_s=28.110`。100 ms 超は 0。対照 `wine-post-probe.exe 60`（browser の無い Windows の program）が同じ run で `post stalled 47.5 ms at 27.9 s`、`message waited 47.5 ms at 28.0 s` |
| 原因の切り分け | 20 ms を超える待ちは全 run で一回、process の起動から約 28 s（browser でも対照でも）。対照は 3 回とも 28.0〜28.2 s に 39.6〜73.3 ms の止まり（socket の有無に関係無く、止まるのは PostThreadMessage そのもの、Wine の server の呼び出し）。browser の窓の thread の中の各段（frames・process・titlebar の progress・touch）を一時の計時で囲んだ run では 10 ms を超える段は無かった（dispatch と入力の受け渡しは囲んでいない。一時の変更は戻した）。対照で同じ時刻に post と GetMessage が止まるので、Wine 自身の一回の止まりで、browser の窓の thread の仕事ではないと判断。本物の Windows での計測はユーザーが拾える試験の一覧に（Q1） |

### 受け入れの照合

- 1: 全 10 場面で上限を満たす（plain）。
- 2: tsan の全場面と host-threaded-view・host-frames で ThreadSanitizer の報告 0（host-threaded-view の 2 件は時間の上限、前と同じ）。
- 3a: zedBSD の QEMU は上限を満たす。Wine は Wine 自身の約 28 s の一回の止まり（対照で再現）で 20 ms を超え、100 ms 超は 0。
- 3: host-build 3 variant warning 0、host-* に新しい失敗無し、js・test262（落ちた一覧が同じ）・http・http-async・loader・fetch・component・WPT の見本・DOM・段 1 の前後・bench が前と同じ、cross-build PASS。R12（同じ試験を headless の `--threads` で）は未実施: `--threads` がどの Phase にも無く、2026-11-14 Q1 の判断で p015 に分けた。
- 4: zedBSD の QEMU（shell-guest 27/27 と画面の PNG）と Wine（shell-wine 23/23）が頁を出す。Linux の Keiland の guest は未実施（Linux 向けの browser の build が無い、p007 の記録と同じ）。本物の Windows・実機は未実施（ユーザーが拾える試験の一覧に、Q1）。

### 未実施・残り

- R12 の `--threads` の回帰（p015 の後）。
- Linux の Keiland の guest（browser の Linux の build が無い）。
- 消してほしい物（Q1 へ）: `build/p011/base/`（基点の tree の archive と build、約 2 GB）、`build/tmp/p3-*`、`build/tmp/ws345-wpt*`、`build/ws345-p007-image`（必要なら残す）。
