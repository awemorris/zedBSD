<!-- awesome-plan project=zedbsd record=ws345-p015 -->

# ws345-p015 headless の `_OWN` の道（`--threads`）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md) §11 の R12。
- 状態: 実装済み・試験 PASS（2026-11-15 P3 q2115-i01、cleared は Q1。手順 1 の「仮想の時計」は、iframe の thread が走れるよう page の時計に替えた: 結果と design.md §5.5）
- 出所: 2026-11-14 Q1 の判断（p011 の手順 3 が求める headless の `--threads` がどの Phase にも無かった。P3 の報告から新しい Phase に）。

## 目的

browser の headless の mode（`--run`・`--dump=*`・`--render`・`--render-gpu`）を `BROWSER_THREADING_OWN` の view でも回せるようにし、WPT の reftest の見本・DOM の試験・component・段 1 の host の試験が出荷する道（`_OWN`）を通るようにする（R12）。

## 背景

- 今の headless の mode は `browser_view_settle`（仮想の時計で timer を予算まで回す）・`browser_view_dump`・`browser_view_draw_pixels` に頼る。これらは `_OWN` の view では `ENOTSUP`（design.md §5 の表）。
- 二つの道は front（積むか、直に呼ぶか）だけが違い page の code は同じ、というのが R12 の前提。だから settle と dump も front の命令にして script の thread で同じ code を走らせれば、二つの道で同じ結果になるはず。

## 手順

1. `browser_view_settle`・`browser_view_dump` を `_OWN` の view でも受ける: front が命令として mailbox に積み、script の thread が今の `_CALLER` の code（仮想の時計の予算、`BROWSER_SETTLE_LAYOUT`、dump の文字列）を走らせ、呼んだ thread は返事を待つ（待つ間も描画の thread と他の命令は止めない）。`browser.h` の説明を合わせ、公開 API の変更の理由を design.md に書く（HAL ではないので承認は要らない、Q1）。
2. `_OWN` の view の絵は `browser_view_render_pixels`（確定した frame）で取る。settle の返事の前にその時の状態で確定する。
3. `userland/desktop/browser/main.c` に `--threads`（headless の各 mode の view を `_OWN` で作る）。
4. component の client（`plan/tools/browser-component/client.c`）にも同じ選択（引数か環境）を足す。

## 試験

- host: `host-threaded-view` に settle・dump の `_OWN` の check、ThreadSanitizer の variant。
- R12: WPT の reftest の見本・DOM の試験（`run-dom-tests.py`）・component・`plan/ws334/tests/stage1.sh` の host の試験を `--threads` 有りと無しで流し、結果が同じ（違えば原因）。
- 回帰: host-build（plain・tsan）warning 0、zedBSD 3 機種の build warning 0、cross-build。

## 受け入れ

- 上の試験が通り、`--threads` 有りと無しで DOM・WPT の見本・component・段 1 の結果が同じ。

## 結果（2026-11-15 P3 q2115-i01）

状態: 実装済み・試験 PASS（cleared は Q1）。基点 main 1bf2cbe5e3、commit dc0a1d8021・9de4466730・eece2c969a と記録の commit。build は空の build/ から（`make CONFIGMK=config/config.<機種>.mk llvm-toolchain-cached` → `sysroot` → `build/<機種>/packages/browser/.stamp`、symlink 無し）。

### 実装

- 公開 API（`browser.h`）: 関数・型の追加は無く、`browser_view_settle`・`_dump`・`_draw_pixels` の契約を `_OWN` の view に広げた（前は `ENOTSUP`、`BROWSER_API_VERSION` は 5 のまま）。理由と形は [design.md](../design.md) §5.5。
- `view/front.c`: 三つを mailbox の命令（`FRONT_SETTLE`・`FRONT_DUMP`・`FRONT_FRAME`）にし、呼んだ thread は返事を待つ（`front_call`、待つ間に publish された news を `browser_view_process` と同じく呼んだ thread の callback に）。settle は仮想の時計でなく page の時計で、各 round の publish の後に script の thread が `view_settle_state`（取得・frame の load が無く、budget に達したか、それより前に budget までの timer・present 待ちの animation frame・来る途中の画像・navigation・iframe の thread が無い、取得が続けば budget の後 30 s で `ETIMEDOUT`）を見て、終われば確定して返事。dump は script の thread で `_CALLER` の code。draw_pixels はその時点で確定し `browser_view_render_pixels` の code で描く。
- `view/view.c`: 上の入口、`view_settle_state`、`view_rendering_checkpoint`。`bind/timer.c`・`bind.h`: `bind_frames_waiting`（present を待つ animation frame があるか）。
- `browser/main.c`: `--threads`（`_OWN` の view、`--async` でない時は `BROWSER_FETCH_DEFAULT`、描画の代わりの thread `browser-draw` が確定した frame を CPU で描いて `browser_view_presented`、script の navigation は link の callback で `--follow` の数まで、`--render-gpu` は確定した frame を `browser_view_render` で program の command buffer に）。
- `plan/tools/browser-component/client.c`・`run.sh`・`run.py`: `--threads`（`run.sh plain threads`）。`_OWN` では入力・load の callback が後の settle で来る、GPU は確定した frame を `browser_view_render` で、navigation の strdup の拒否の確かめは view の thread の copy なので行わない。
- `plan/ws345/tests/host-threaded-view.c` に (s)（settle・dump・draw_pixels が `_CALLER` と同じ、budget の後の timer が走らず settle を延ばさない、fonts.ready と rAF、iframe）。`plan/ws345/tests/r12.sh`（新、R12 の 1 本の script）。

### R12 で見つけて直した `_OWN` の道の誤り（窓でも同じ）

1. commit の layout が `document.fonts.ready` を解いても、その反応（microtask）が次の task まで走らなかった（DOM の `fonts-ready-frames` が `ready true` 以降を出さない）。round の publish の後に microtask の checkpoint を task として走らせ、頁が変われば確定し直す。
2. 確定した frame が display list を取った後の paint の dump が空（`canvas 0.00 x 0.00`）。dump の時に paint し直す。
3. `browser_view_gpu_failure` が `_OWN` で常に 0 を返した（描画の thread の失敗が読めない。shell の `present.c` も呼ぶ）。view の記録を返す。
4. `_OWN` の view を `BROWSER_FETCH_AT_ONCE` で作ると loader が無く iframe が thread を持てない（DOM の 5 件が「no thread could run it」）。`--threads` は窓と同じ `BROWSER_FETCH_DEFAULT` にした（design.md §5.5）。

### R12: `_CALLER` と `_OWN`（`--threads`）の比べ（`sh plan/ws345/tests/r12.sh plain 400`、http・http-async は runner の `-I` の直しの後に流し直し）

| 試験 | `_CALLER` | `_OWN` | 比べ |
| --- | --- | --- | --- |
| DOM（`run-dom-tests.py`） | 39/42 | 39/42 | 同じ（出力の全行） |
| WPT reftest の見本 100 件 | pass 62・fail 37・error 1 | 同じ | 各 test の結果が同じ |
| http（`run-http-tests.py`） | 15/15 | 15/15 | 同じ |
| http-async | 19/20（`async-sheets`、前から） | 19/20 | 同じ |
| fetch | blocking 38/38・async 38/38 | 同じ | 同じ |
| component | 83 checks PASS | 99 checks PASS | 両方 PASS |
| cross-origin（`run-cross-origin-tests.py`） | 21/21 | 14/21 | 設計上の違い（下） |
| WPT cross-origin の部分集合（`wpt-cross-origin.py`） | 69 files、55 with results、40/80 | 69 files、54 with results、42/78 | 比べない（他の agent の frame は `_OWN` だけ） |

- cross-origin の 7 件（message-origin・message-source・message-ports・port-start-event・port-render-event・port-reply・frame-shown）: `_OWN` では localhost と 127.0.0.1 の frame が別の agent になり、子が MessagePort を transfer する postMessage が `DataCloneError`（design.md §8: agent を越える MessagePort の transfer は段 B の外、Future Work）。7 件とも子のその message に依存。`_CALLER`（同じ agent）の 21/21 を合格の根拠とする（2026-11-15 Q1 の判断）。
- WPT cross-origin（他の agent の frame が通る事）: `_OWN` で `event.source.xorigin`・`postMessage_Date`・`postMessage_solidus_xorigin` が 0/1 → 1/1（他の origin の iframe が自分の agent で走り、message が届く）。`Transferred_objects_unusable` は `_CALLER` 1/2 → NO-RESULT（MessagePort を agent を越えて transfer し `DataCloneError`、async_test が終わらず settle の 5 s に harness が結果を出さない。上と同じ原因、2 回流して同じ）。他の 64 件は同じ。
- 段 1（`plan/ws334/tests/stage1.sh`）の browser の program を使う部分（http・http-async・fetch・component）を上で両 mode に流した。js（`--js`）・loader（host-loader）・host-* は view の mode を持たないので対象外。

### 試験

| 試験 | 結果 |
| --- | --- |
| `BROWSER_HOST_BUILD=build/ws345-host sh plan/ws074/tests/host-build.sh plain`・`tsan` | 両方 exit 0、warning 0、built 104 |
| `sh plan/ws345/tests/host-threaded-view.sh plain` | `host-threaded-view: 81 checks, 0 failed`（(s) を含む） |
| 同 `tsan` | ThreadSanitizer の報告 0。失敗 3 は時間の上限で前と同じ tsan の遅さ（(e) 1164 ms・(h) 1922 ms・navigation-wait の caller 856 ms。p008・p009 の記録で基点でも落ちる）。(s) は PASS |
| `sh plan/ws345/tests/host-frames.sh plain`・`tsan` | 両方 `33 checks, 0 failed`、tsan の報告 0 |
| `sh plan/ws345/tests/r12.sh tsan 400 dom fetch` | dom 両方 39/42 で同じ、fetch 両方 38/38（比べの初版が test server の接続の例外の行を拾い differ と出たので、fetch は結果の行を比べる形に直した）、ThreadSanitizer の報告 0 |
| `make -j4 CONFIGMK=config/config.{x86_64,rpi5,vf2}.mk build/<機種>/packages/browser/.stamp` | 3 つとも exit 0、warning 0 |
| `BUILD_JOBS=4 sh plan/ws334/tests/cross-build.sh` | `cross-build: PASS` |
| `sh plan/ws345/tests/shell-wine.sh build/tmp/p3-p015-shell-wine` | `shell-wine: 23 passed, 0 failed`、`shell-wine: PASS`（1 回目で PASS） |

### 未実施・残り

- zedBSD の QEMU・実機（この Phase は headless の host の道。窓の道は p011 の shell-guest と同じ code の上で、今回の変更は commit の後の checkpoint と gpu_failure だけ）。
- 消してほしい物（Q1 へ）: `build/tmp/ws345-r12-*`、`build/tmp/p015-wpt1`、`build/tmp/p3-p015-shell-wine`、`build/review/ws345-p007/`（shell-wine の写し）、`build/p015/`（log と wrapper）。
