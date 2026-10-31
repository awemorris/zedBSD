<!-- awesome-plan project=zedbsd record=ws334-p012 -->

# ws334-p012 storage の thread と queue: queue

- 親: [WS334](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-01 p002 で作成）。
- 依存: p005・p010（records の `wb_storage_key_valid`・sink の関数を使い、os-system の thread の名前を link する。p010 の実装が無いと試験が通らない）（依存の Phase の実装が main に merge され cleared になってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順、ws.md の決定 22）。header は p002 の設計で、p004 で確定した物を変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、docs/agent/spec-module-delegation.md の §5・§7、plan/guardrail.md の「出典」「開発の管理情報」。
2. [design.md](../design.md) の §3.5.2 の並行の段落・lock の順、§3.6、§3.7。
3. 実装する header（各関数の comment が仕様）: `userland/desktop/include/browser/storage.h`、`userland/desktop/libbrowser/storage/queue.h`、`userland/desktop/libbrowser/storage/records.h（wb_storage_sink_*・wb_storage_key_valid）`、`userland/desktop/libbrowser/base/os-system.h（wb_os_thread_name）`。それが include する header も読む。
4. 仕様: header だけ。thread は `<pthread.h>`（Windows は llvm-mingw の winpthreads）。

## 作る物

- `userland/desktop/libbrowser/storage/queue.c`（今は骨格。全部の関数を書く）。
- 単体試験 `plan/ws334/tests/unit/test-queue.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 試験は memory の fake の backend を試験の中に書く（呼ばれた順・thread の ID を記録し、遅延・誤りを注入できる）。records の sink の関数を使う。
- 開く: calls NULL の queue（thread 無し: write は 0 で捨て、read は found 0、list は 0 個）、version 違い・member の欠け → ENOTSUP で close が呼ばれない、queue NULL → EINVAL。
- 順: write A・write B・remove A・read A（found 0）・read B（bytes）。backend の呼び出しが全部 1 つの同じ thread（試験の thread と違う）から、重ならない（fake が呼び出しの中に入っている数を数えて 1 を超えない）。
- write-behind: fake の write が 200 ms 眠る時、10 回の write が試験の thread を待たせない（合計 50 ms 未満で返る）。その後の read は 10 回の write の後の値を見る。
- 合わせ: 同じ key の write を thread が始める前に積むと最後の bytes だけが届いてよい（届いた bytes が最後の物であることだけを確かめる）。fake を止めた状態で write(k, A)・remove(k)・write(k, B) を積み、動かした後に k が B で残る（remove が後ろへ回らない）。write(k, A)・read(k)・write(k, B) で read が A を見る。
- 上限: fake を止めた状態で合計が WB_STORAGE_QUEUE_LIMIT を超える write を積むと、超えた write が ENOSPC で待たずに返る（試験の thread が 50 ms 未満で戻る）。length > LIMIT → EFBIG。
- 誤り: fake の write が EIO を返すと `take_error` が EIO を 1 回だけ返し、次は 0。read の誤りは found 0 と誤りの値。不正な key → EINVAL。
- list: fake の不正な key を飛ばし、"key\0key\0" の形と count。
- flush は全部が済むまで待つ。close は積んだ物を流し切り、backend の close を 1 回、thread を止める（close の後に fake の呼び出しが無い）。close(NULL)。
- 4 つの thread から 1000 回ずつ write・read を混ぜて run-unit.sh の build（posix は ASan・UBSan、win32 は UBSan）で落ちず、順の約束（各 thread の自分の書きを自分の読みが見る）。

## 注意

- lock は queue の 1 つの mutex と 2 つの cond（仕事が来た・仕事が済んだ）。lock の中で backend も他の lock も取らない（header の通り）。
- 読みの答えは頼んだ thread の stack の要求の構造体に書き、済んだ印と cond で知らせる（memory を thread の間で渡さない形）。
- thread の名前は `wb_os_thread_name(WB_STORAGE_QUEUE_THREAD)`。thread は joinable で close が join する。
- POSIX の file（`*-posix.c`）と OS に依らない file に `#ifdef _WIN32`・`<windows.h>` を書かない（`check-skeleton.sh` が見る）。Windows の file（`*-win32.c`）は `<windows.h>`・`<winsock2.h>` などを include してよいが、W の API だけを使い（A の API を使わない）、`<jpeglib.h>` を include しない。
- 他の実装（Chromium・Firefox・Wine・curl・libuv・glib・Qt など）の code・comment・構造・名前を写さない。Win32・POSIX の API の使い方は API の reference から書く。
- source の comment に WS・Phase の番号・plan/ への参照・日付を書かない。各関数は coding-style の形（multi-line の定義、宣言の群、comment の付いた段落、成功の return を最後に）。
- 試験は header の約束を全部の誤りの場合まで確かめる。期待の値は header・API の reference・手の計算から作り、他の実装の出力を使わない。Windows の側の期待を Windows の API で独立に求めてよい（試験の file の中で、実装と別の道で）。
- 試験の file は `plan/ws334/tests/unit/test-<名前>.c`。`#include "check.h"` の `CHECK`・`CHECK_EQ`・`check_finish("<名前>")` を使い、`main(argc, argv)` の `argv[1]` が "posix" か "win32"。今の directory は試験ごとの空の directory で、file を作ってよい。

## 変えてはいけない物

- 全部の module の header（`plan/ws334/tests/modules.sh` の `ws334_headers` の全部）。header の不備・矛盾・足りない口を見つけたら直さずに止めて Q1 に返す（どの header のどの関数か、何が足りないか、案の差分）。header が黙っている所は自分で決め、「結果」に列挙する。
- 担当の外の `.c`（他の Phase の module、libbrowser・libmedia の既存の file）、`apps/browser-win32/Makefile`・一番上の `Makefile`、試験の道具（`plan/ws334/tests/modules.sh`・`run-unit.sh`・`check-skeleton.sh`・`unit/check.h`・`unit/test-tools.c`）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。push しない。

## 確かめの command（worktree の root で）

- `timeout 1800 sh plan/ws334/tests/run-unit.sh queue` → 最後の行 `run-unit: PASS`（posix と win32 の両方が流れたことを出力で見る。win32 は初回に Wine の prefix を作るので数分かかる）。
- `timeout 3600 sh plan/ws334/tests/run-unit.sh all` → `run-unit: PASS`。
- `BASE=d393d3267 timeout 1800 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`（header と試験の道具が変わっていない、全部の .c が cc・clang・x86_64 と aarch64 の llvm-mingw で warning 0、`make browser-win64`・`browser-winarm64` が通る）。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で。例: 誤りの写しを 1 つ違える、境界の比較を 1 つずらす、Windows の側の変換の長さを 1 つ違える）試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の件数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

## 結果（2026-11-01 03:56 実装・試験、担当 Sonnet 5.5）

- 作った物: `userland/desktop/libbrowser/storage/queue.c`（全部の関数）、`plan/ws334/tests/unit/test-queue.c`。header・試験の道具・他の module は変えていない。
- 試験（`LC_ALL=C.UTF-8`、出力を読んだ最後の行）:
  - `sh plan/ws334/tests/run-unit.sh queue` → `test-queue: PASS (13174 checks) (posix)`・`(win32)`、`run-unit: PASS`。
  - `sh plan/ws334/tests/run-unit.sh all` → 全部 PASS（queue は posix・win32 とも 13174 checks）、`run-unit: PASS`。
  - `BASE=d393d3267 sh plan/ws334/tests/check-skeleton.sh` → `check-skeleton: PASS`。`git diff --check` は出力なし。
  - 追加で（run-unit.sh の外、scratch）同じ試験を `-fsanitize=thread` で build して流した: `test-queue: PASS (13174 checks)`、データ競争の報告なし。
- 試験の内容: phase.md の項目に加え、全ての関数の NULL・不正な key の EINVAL（backend に届かない事）、200 文字の key、長さ 0 の record、space ごとの分離と clear、
  merge は remove・clear・別 space・別 key をまたぐ場合（またげる場合とまたげない場合）、read をまたがない事（別 thread の read を使い 5 回まで試す）、
  byte の上限ちょうど・+1・置換での再利用・flush 後の回復、件数の上限（65535 件の remove の後で ENOSPC）、first error の保持と take_error の消費、
  不明な errno 値の素通し、read の失敗で buffer が元に戻る事（1 byte 書いた後の失敗、backend が sink の失敗を無視して 0 を返す場合）、
  list の失敗で keys と count が元のままの事、backend の呼び出しが 1 つの別 thread・重ならない・thread 名 `browser-storage`（posix）、
  close が積んだ物を流し切り backend の close を 1 回・最後に呼ぶ事（gate で止めた間に積んだ 5 件）、書いた物が flush や別の呼び出し無しで backend に届く事（起こし忘れの検出）、
  4 thread×1000 回の write・read・remove・list の混在（各 thread が自分の書きを自分の読みで確かめる）。
  期待値は header の約束と手の計算から作った（fake の backend は試験の中、他の実装は使わない）。
- 壊しての確認（git の外、`cmp` と `git diff` 空で戻りを確認、21 通り）: remove をまたぐ merge、read をまたぐ merge、clear をまたぐ merge、space を無視した merge、byte 上限の `>`→`>=`、
  件数上限の `>=`→`>`、take_error が消さない、後の error が勝つ、list が不正な key を残す、close が積んだ物を流さない、backend の close が 2 回・0 回、
  read の失敗で buffer を戻さない、list の失敗で keys を戻さない、flush が待たない、済んだ byte を引かない、sink の error を無視、新しい要求を先頭へ積む、
  write の起こし忘れ、EFBIG の検査の削除。19 通りが初めから FAIL。初めに見逃した 2 つ（sink の error の無視・write の起こし忘れ）は試験を足して FAIL するようにした。
  残る 1 つ（read の `found` を失敗時に 0 にしない）は同値: 失敗時は `*found` が 0 のまま返るので公開の結果が変わらない。
  lock を持ったまま backend を呼ぶ変異は deadlock になるだけなので走らせていない。
- 自分で決めた点（header が黙っている所）:
  - 要求の件数の上限 65536（write・remove・clear が待っている数、実行中の 1 件を含む）。超えると ENOSPC。header の byte の上限は remove・clear を数えないので、止まった backend に対する memory の上限として足した
    （read・list・flush は呼び手が待つので数えない）。同じ key の write が置換できる時は件数が増えないので超えても置換できる。
  - byte の数え方は write の bytes の長さだけ。実行中の write も backend が終わるまで数える。長さ ちょうど `WB_STORAGE_QUEUE_LIMIT` の write は空の queue で通る。置換は古い bytes を引いてから上限を見る。
  - merge は後ろから最大 256 件だけ見る（長い queue での走査を抑える）。flush の印は壁として merge を止める。別 space の要求は無関係として通り過ぎる。
    置換は古い write の位置に新しい write が入る（順番は変わらない）。
  - `take_error` は backend の値をそのまま返す（EIO に直さない）。read・list の誤りは呼び手へ返し take_error には入れない。NULL の queue の take_error は 0、flush(NULL)・close(NULL) は何もしない。
  - list の `count` は成功した時だけ書く（失敗では触れない）。成功の時は今回足した数で、元の値に足さない。read の失敗・未発見では、sink が足した bytes を取り除き buffer を元の長さと NUL に戻す。
  - backend の無い queue でも引数の検査（EINVAL・EFBIG）はする。その後 write・remove・clear は 0、read は found 0 で 0、list は count 0 で 0。
  - open の引数の誤りは queue NULL → EINVAL を最初に見る。mutex・cond の初期化の失敗は ENOMEM、`pthread_create` の失敗は種類によらず EAGAIN。
  - 長さ 0 の write の bytes は backend に NULL でない pointer（record の後ろ）で渡す。
  - 試験の `struct fake` は大きい（約 1.2 MiB）ので static に置く（Windows の stack は 1 MiB で、初めに stack overflow した）。
- 未実施: ENOMEM の経路（確保の失敗の注入）、`pthread_create`・mutex の初期化の失敗の経路、aarch64 の Windows の実行（arm64 の Wine が無い、compile は check-skeleton で通る）、
  実際の `files` backend との結合（p011 と統合の phase）、起こし忘れのような競合は TSan と繰り返しで見たが形式的な証明ではない。
