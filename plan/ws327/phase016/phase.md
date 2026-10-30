<!-- awesome-plan project=zedbsd record=ws327-p016 -->

# ws327-p016 受け入れ: 境界の check、全 arch・Linux・FreeBSD の build、QEMU の回帰、Win32 の報告

- 親: [WS327](../ws.md)。設計: [design.md](../design.md) §1.3・§6.4・§8・§13。担当: P2（設計の担当）。依存: p008〜p015 が main に merge され cleared。
- Status: planned
- Disposition: normal

## 目的

実装の Phase の成果を一つの tree（main の merge の後）で通して確かめ、WS の目標（app が pthread・socket を直に呼ばず、POSIX と Win32 で
同じ interface）の達成を示す。直しが要れば小さな物は P2 が直し、大きな物は Q1 に返す。

## 確かめ

1. **境界**: `python3 -I plan/ws327/tests/check-layers.py`（範囲を絞らず）→ `ws327-check-layers: PASS`（T1・N1 が 0、許可の行だけ）。
   `sh plan/tools/keiland-os-boundary/check.sh` の FAIL の一覧を main の基点（WS327 の前）の一覧と比べ、増えた物が無い（B5 は
   `#include <keiland/keiland_*.h>` を全て拾う既存の誤検出で、`plan/ws327/tests/` の新しい試験の行が増える: Q1 に報告）。
   T1・N1 を check.sh へ入れる案（check-layers.py の中身と layers-allow.tsv を A1〜A5 の形で）を差分にして Q1 に出す（`plan/tools/` は
   Q1 の範囲）。
2. **build**: `sh plan/ws327/tests/build-zedbsd.sh p016 <arch> "mailer music videoplayer photos settings keiland-ime terminal"` を
   amd64・arm64・riscv64 → 各 `make=0 warnings=0`。Linux: libkeiland・settings・keiland-ime の build、warning 0。
   FreeBSD: P1 の guest（`plan/tools/keiland-freebsd/`、書き込まない overlay）で libkeiland・settings・keiland-ime の build と
   thread-test・socket-test（design §8）。
3. **host**: `sh plan/ws327/tests/host-test.sh build/ws327-p016-linux`（12 build）、`sh plan/ws327/tests/host-apps.sh build/ws327-p016-linux ime picker`、
   `sh plan/tools/mail/run-host-mail-backend.sh`、`sh plan/tools/music/host-music-play.sh`、`sh plan/tools/media/run-host-codec.sh`、
   `sh plan/ws327/tests/build-win32.sh`、`sh plan/ws314/tests/doc-examples.sh build/ws327-p016-linux/lib`。
4. **QEMU の回帰**（design §6.4、`plan/tools/guest/test-image.sh plan/ws327/tests/config-amd64-guest.mk build/ws327-p016-img`、1 本の script
   `plan/ws327/tests/guest-apps.sh` を P2 が書く: 配置 → 起動 → 実行 → 回収 → 要約。判定は画面の PNG と SSH での program の終了の status と
   app の log の行。console は読まない）:
   - Mailer: host で `plan/tools/mail/fake-mail-server.py`（host の 127.0.0.1 に bind、guest は slirp の 10.0.2.2 で届く、
     `--arrivals 1`。名前でなく address で指す: zedBSD に `/etc/hosts` が無い）、guest の Mailer が接続・sign-in・IDLE・新着・送信（`MAIL SIGNED-IN`・`MAIL REFRESHED`・
     `MAIL MESSAGE ... arrived=1`・`MAIL SENT` の行）。
   - Music・Video Player: `plan/tools/media/sample.mp4` の再生の開始と停止（窓の写真と、停止の後に process が残らない）。
   - Photos: 起動の直後（縮小の thread の start の前後）の縮小の要求と一覧の写真（静的な条件変数の once、design-reviewer #10）。
     Settings: storage の page（走査の終わりの log）と見た目の page。IME: 辞書の書きを 2 回（1 回目の flush で thread が終わった後に
     もう 1 回: once で作った条件変数の再使用）、`~/.config` の辞書の file の中身。Video Player の `kl_cond_wait_for` は再生の途切れの
     無い事（写真 2 枚の間の時間）で。Terminal の起動（回帰）。
   - 8 章の例: `sh plan/ws314/tests/doc-examples.sh guest build/ws327-p016-img 08-worker 08-language` の写真、08-network は同じ方法で撮り
     `docs/howto/keiland-examples/08-network.png` と本文の画像の行を足す（p015 が置かなかった物）。
5. **Win32**: p013・p014 の結果の「Q1 へ」をまとめ、interface を Windows で守れない所（例: key の destroy の回数、`FlsFree` の destroy、
   SetThreadDescription の無い Windows、`select` の FD の数の上限が無い事の確かめ）を design.md §5 に記録して Q1 に返す。

## 所見の引き継ぎ（p007 から）

- zedBSD の image に `/etc/hosts` が無く "localhost" が解けない（p003）。Q1 が Bug にするか決める。
- 閉じた WS の試験で、移した source を `-pthread` だけで build する物（`plan/ws095/tests/host-engine.sh`、`plan/ws089/tests/host-wallpaper.sh`・
  `host-build.sh`、`plan/ws191/tests/host-audio-stream.sh`（main で既に FAIL））: 削除の候補として Q1 に。
- `plan/tools/mail/host-mail-sync.c` は 6a53bce99 から build できなかった（p008 で直す）。

## 受け入れ

上の 1〜5 が全て済み、FAIL は 0 か Q1 が受け入れた物だけ。結果をこの phase.md に、ws.md の Resume point に要約。

## 結果

P2（q1703-i01、worktree p2-ws327、基点 main b901b1968）。日付は host の `date`。

### 1. 境界（2026-10-30 10:15）

- `python3 -I plan/ws327/tests/check-layers.py`（範囲を絞らず）→ `check: T1 0 found`・`check: N1 0 found`・`ws327-check-layers: PASS`。
- `sh plan/tools/keiland-os-boundary/check.sh` を今の tree（b901b1968）と WS327 の前（e05900bef = 7b1a79c23 の親、`build/tmp/p016-base-src` の
  scratch の clone）で流し、FAIL の行を比べた（`build/tmp/p016-boundary-base.txt`・`p016-boundary-head.txt`）: 110 → 148 行。
  増えた 38 行は全て B5（`#include <keiland/keiland_thread.h>`・`keiland_socket.h` の名前を旧い接頭辞 `keiland_` と拾う既存の誤検出。
  keiland.h・keiland_version.h・platform/・app の include と `plan/ws327/tests/`・`plan/tools/mail/` の試験）。他の check の増減は無い
  （A1 の libbrowser の行番号の移動 1 件だけ、中身は同じ）。
- B5 の直し（Q1 の指示「今直してよい」2026-10-30）: `check.sh` の B5 が `keiland/keiland_*.h`（directory の付いた今の公開の header の名前）を
  一つの語として拾い、許す（旧い `<keiland.h>`・`keiui.h` などは今どおり FAIL）。`plan/ws327/tests/win32-link.c` の comment の
  directory の無い header の名前も直した。数え直し（新しい check.sh を両方の tree で）: WS327 の前 64 行・今 64 行、行番号を除いて
  同じ一覧（`diff` で差 0）。残る B5 は前からの `plan/tools/powervr/p009/vk-host-test.c` の `KEILAND_B_*` 8 行。
- T1・N1 を check.sh に入れる案: [check-t1-n1.patch](check-t1-n1.patch)（B5 を直した check.sh の上に `git apply` できる。A1〜A5 の
  python に T1・N1 の pattern と、libbrowser・libmedia・`libkeiland/platform/` を除く範囲、`app-allow.tsv` に layers-allow.tsv の 19 行）。
  scratch の clone で確かめた: T1 PASS・N1 PASS・全体 64 行（今と同じ）、突然変異（music/play.c の kl_mutex_lock を pthread_mutex_lock に、
  settings/look.c に `#include <netdb.h>`）で `T1 FAIL …play.c:133: pthread_mutex_lock`・`N1 FAIL …look.c:977: #include <netdb.h>`。
  入れたら `plan/ws327/tests/check-layers.py`・`layers-allow.tsv` は WS の完了で消せる（Q1 へ）。
- Q1「T1・N1 を check.sh に入れる案は採る」（2026-10-30）: patch を当てた（`check.sh`・`app-allow.tsv`）。今の tree で `check: T1 PASS`・
  `check: N1 PASS`、FAIL は 64 行のまま。patch の file（`plan/ws327/phase016/check-t1-n1.patch`）は削除の候補（Q1 へ）。
  plan/tools.md の 121 行（Keiland の OS 境界 checker）の説明の末尾に足す文案:
  「2026-10-30 WS327 p016: T1・N1（app と libkeiland の platform/ の外が libkeiland の thread・network の層を使う: pthread_*・C11 threads・
  <pthread.h>（T1）、socket・connect・poll・select・send・recv などと <sys/socket.h>・<poll.h>・<netdb.h> など（N1）。libbrowser・libmedia・
  libkeiland/platform/ は見ない。許可は同じ app-allow.tsv）。B5 は directory の付いた公開の header の名前 `keiland/keiland_*.h` を許す」

### 2. build（2026-10-30 10:16〜10:20）

- `sh plan/ws327/tests/build-zedbsd.sh p016 <arch> "mailer music videoplayer photos settings keiland-ime terminal"`:
  `zedbsd amd64 make=0 warnings=0`・`zedbsd arm64 make=0 warnings=0`・`zedbsd riscv64 make=0 warnings=0`（7 program と libkeiland.so が
  3 arch とも在る）。
- Linux: `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/ws327-p016-linux …/lib/libkeiland.so …/bin/settings
  …/libexec/keiland-ime` → `linux make=0 warnings=0`。

### 3. host（2026-10-30 10:18〜10:22、log `build/ws327-p016-host.log`）

| command | 最後の行 |
| --- | --- |
| `host-test.sh build/ws327-p016-linux` | `SUMMARY pass=12 fail=0`（thread 27・socket 53 の各 build） |
| `host-apps.sh build/ws327-p016-linux ime picker` | `ime: PASS (host-engine: 230 passed, 0 failed)`・`picker: PASS`・`ws327-host-apps: PASS` |
| `plan/tools/mail/run-host-mail-backend.sh` | `host-mail-sync: PASS`（backend も PASS） |
| `plan/tools/music/host-music-play.sh` | `host-music-play: PASS` |
| `plan/tools/media/run-host-codec.sh` | `run-host-codec: PASS` |
| `plan/ws327/tests/build-win32.sh` | `ws327-win32: PASS`（x86_64・i686 の compile warning 0 と link） |
| `plan/ws314/tests/doc-examples.sh build/ws327-p016-linux/lib` | `SUMMARY pass=76 fail=0 not-done=0` |

### 4. 閉じた WS の試験（AGENTS.md §9 の基準、2026-10-30 10:22〜10:25）

| 試験 | 生きた参照 | 扱い | 結果 |
| --- | --- | --- | --- |
| `plan/ws089/tests/host-build.sh` | `plan/tools/keiland-linux/zedbsd-commands.md`（tools.md に登録）・`plan/tools/keiland/run-host-welcome.sh` | 直した: `platform/thread-posix.c` を `shared-thread-posix.o` として link | `built build/ws089-host/settings-render`（exit 0） |
| `plan/ws089/tests/host-wallpaper.sh` | 無い（ws100 の閉じた Phase だけ） | 直しは不要: host-build.sh の objects.list の `shared-*.o` を拾うので上の直しで通る。削除の候補から外した | `host-wallpaper: PASS` |
| `plan/ws095/tests/host-engine.sh` | `userland/desktop/ime/Makefile` の comment | 直した: thread-posix.c を gnu17 で compile して link、`-Iuserland/desktop/include` | `host-engine: 233 passed, 0 failed` |
| `plan/ws191/tests/host-audio-stream.sh` | `libkeiland-backend-zedbsd/audio-stream-zedbsd.c` の comment（`AUDIO_SOCKET_PATH` の差し替えはこの試験のため） | 直した: client の側に thread-posix.c。2 つ目の部分の `host-audiod.c` は ws209-p002（0349a118c）の `AUDIOD_VERSION` → `AUDIOD_SHM_VERSION` に追従していなかった（WS327 の前から FAIL の原因）: 1 行を直した | `host-audio-stream: PASS`・`host-audiod: PASS`・`host-pump: PASS`（exit 0） |

削除の候補は無い（4 本とも直した）。`plan/ws327/tests/host-apps.sh` の「audio.c に host 試験が無い」の comment を直した。

### 6. `network_finish` の待ち方（2026-10-30 10:35〜10:45）

決めた形: **終わりは thread を先に join し、その後に post を待たずに取る**（08-language.c が既にこの形）。理由:
- 前の `network_finish` は FINISHED の post を `kl_app_dispatch(app, -1)` で待つ。thread の `kl_app_post` は ENOMEM で失敗しうる
  （header の約束）ので、失った時に終わりが止まる。
- 前の `primes_finish` は timeout 0 で、take が 0 を返すと抜ける。worker がまだ走っていて FINISHED を post する前なら、thread を join せずに
  抜け、`kl_app_close` の後に worker が閉じた app へ post しうる（解放の後の使用）。
- join の後は thread の post は全て list に在り（`keiui_app_post_event`）、`kl_app_dispatch(app, 0)` が移す。一つも運ばない dispatch で
  残りが無いと分かる。post を失った record（count・exchange）は finish が free する（network は waker も join の後に閉じる）。
  FINISHED の handler（`primes_post`・`network_post`）は thread が NULL なら join しない（二重の join を避ける）。

直した file: `docs/howto/keiland-examples/08-worker.c`（`primes_post`・`primes_finish`）、`08-network.c`（`network_post`・`network_finish`）、
`docs/howto/keiland-programming.md`（例の全文の写し 2 つ、「Taking a post」「The end of the program」「The rules」の該当の行、network の
「The end」に finish の 1 段落、動いた行の link）。確かめ: `python3 -I plan/ws327/tests/doc-lines.py 08-worker.c 08-network.c` →
`ws327-doc-lines: PASS`（全ての参照の最初と最後の行を目で見た）、`sh plan/ws314/tests/doc-examples.sh build/ws327-p016-linux/lib` →
`SUMMARY pass=76 fail=0 not-done=0`（clang・gcc の -Werror、link、写しの一致）。ENOMEM の道は走らせていない（未実施）。窓の道は下の QEMU で。

### 5. Win32（2026-10-30 10:50）

p013・p014 の「Q1 へ」と p016 の確かめ（`kl_cond_wait_for`・`kl_socket_poll` の時間、select の数）を [design.md §5.4](../design.md) の
表（W1〜W8・S1〜S9）にまとめた。**header を変える必要は無い**（どれも header の「another errno value」「on some systems」「未規定」「at most」の内、
または実装・build の側で直せる）。Q1 に判断を頼む物: S7（Win32 の poll の時間を QPC で測り待ち直すか。W1 と揃える実装の直し、走らせられない）、
S9（`kl_socket_close(NULL)` の comment を p017 で header に足す）、W5・S8（Windows の Makefile を作る時の約束、Future Work の候補）。

Q1 の判断（2026-10-30）: S7 は直す、S9 は p017 で header の comment に（API・ABI・KL_VERSION は変えない）、W5・S8 は Q1 が Future Work
（F-WS327-win32-build）に。
- S7 を直した: `socket-win32.c` の `kl_socket_poll` が開始の QPC と時間の tick を覚え、`select` が時間の前に 0 で返れば残り（切り上げの ms）を
  待ち直す（`socket_clock`・`socket_poll_left` を足した）。waits が 0 の時は `Sleep` でなく `kl_thread_sleep`。`sh plan/ws327/tests/build-win32.sh`
  → x86_64・i686 とも compile warning 0・link PASS、`ws327-win32: PASS`。走らせていない（決定 3）。
- F-WS327-win32-build の文案（一行）: 「libkeiland の Windows の build（Makefile）を作る時の約束: `-lws2_32 -liphlpapi`、Windows 10 を前提
  （`_WIN32_WINNT=0x0A00`、`SetThreadDescription`・`inet_pton`・`WSA_FLAG_NO_HANDLE_INHERIT`）、libkeiland の DLL は thread の生きている間に
  unload しない（FLS の callback が消えた code を指す）。WS327 design §5.4 W5・S8」

（以下 QEMU・FreeBSD は続き）

## Q1 から（2026-10-30）: 移行で壊れた閉じた WS の試験

p011 の後に `plan/ws089/tests/host-wallpaper.sh`（と `host-build.sh` の見込み）が link で壊れる（look.c・storage-trash.c を `-pthread` だけで build）。p012 の後の `plan/ws095/tests/host-engine.sh`、前から FAIL の `plan/ws191/tests/host-audio-stream.sh` も同じ扱い。`plan/ws196/ws.md` が ws089/tests を参照しているので、AGENTS.md §9 の試験の整理の基準で、参照が生きていれば層を link するよう直し、生きていなければ削除の一覧を Q1 に送る。

## p015 から（2026-10-30 Q1）

- `08-network.c` の窓の道（進みの post、join の後の waker の close、`network_finish`）は build だけで走っていない。guest で走らせ画面を撮る（08-network.png）。
- `network_finish` は FINISHED を `kl_app_dispatch(app, -1)` で待つので、thread の FINISHED の `kl_app_post` が ENOMEM で失敗すると終わりで止まる（`primes_finish` は timeout 0 で post を取り逃がしうる）。どちらの形が正しいかを決め、解説と例を合わせる。
