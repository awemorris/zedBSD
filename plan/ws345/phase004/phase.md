<!-- awesome-plan project=zedbsd record=ws345-p004 -->

# ws345-p004 VM の打ち切り

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: cleared（2026-11-02 P5 q1973-i01。結果は下）

## 目的

長く走る script を外の thread から止める（navigation・閉じる、design.md §9）。

## file

- `vm/vm.h`・`vm/internal.h`・`vm/interpreter.c`（`VM_OP_LOOP_HINT`、関数の入口、`interpreter_unwind` が打ち切りを catch・finally に渡さない）・長い native（`builtin_array.c` の sort、正規表現の match、JSON、`String.prototype.repeat` など、回数で見る）。
- `bind/`・`page/`: 打ち切りで抜けた task の後片付け（microtask を捨てる、console に一行、頁は「打ち切られた」印で bfcache に残さない）。

## 手順

1. heap（段 B の agent）ごとに、今の task の通し番号と、終わらせる task の番号（atomic、0 は無し）。`vm_heap_task_begin`（番号を進める）・`vm_heap_request_termination(heap, task)`（どの thread からでも）、task を抜けた thread が 0 に戻す（design.md §9）。番号が合う時だけ抜ける。
2. 見る所を入れ、新しい status（`VM_TERMINATED`）で全ての frame を抜ける。native から呼んだ JS（callback）も抜ける。
3. 速さ: loop の印での atomic の relaxed の読み 1 回。BUG-411 の bench で遅くならない事を測る。

## 試験

- 新しい JS の host の試験 `plan/ws345/tests/host-terminate.c`: 別の thread が 200 ms 後に flag を立て、`for (;;) {}`、`while (1) try {} finally {}`、`try { for(;;){} } catch (e) {}`、`[...].sort(遅い比較)`、再帰、generator・async の中の loop が 300 ms 以内に抜け、catch・finally が走らない。前の task の番号を書いても今の task は抜けない（次の task に落ちない）。task を抜けた後、次の script は普通に走る。
- `sh plan/ws074/tests/bench/bug411.sh` の前後（遅れは 2% 以内）。js、test262 の部分集合（`run-test262.py` の es5 と language/statements）が前と同じ。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通り、test262 の落ちた一覧が前と同じ。

## 結果（2026-11-02 P5、q1973-i01）

基点 d635cd1027。branch `agent/p5-ws345`。

### 作った物

- `vm/vm.h`・`vm/internal.h`・`vm/heap.c`: 新しい status `VM_TERMINATED`（-3、VM_THROWN と別。catch・finally に渡らない）。heap（今は tab に一つ、段 B で agent）ごとに task の番号 `task`（heap の thread だけが書き、どの thread も `__atomic` で読む、0 は task の外）・`task_last`・終わらせる task の番号 `terminate`（どの thread も書く、0 は無し）。口: `vm_heap_task_begin`（番号を進め、前の要求を 0 に）・`vm_heap_task_end`（task と要求を 0 に）・`vm_heap_task_current`（どの thread から）・`vm_heap_request_termination(heap, task)`（どの thread から、0 は何もしない）・`vm_heap_check_termination`・`vm_heap_count_step`（長い native が 1 歩ごとに呼び、4096 歩ごとに見る）。見る所は inline の `vm_termination_poll`（要求が無ければ relaxed の読み 1 回、有れば今の task の番号と比べる）。
- `vm/interpreter.c`: `VM_OP_LOOP_HINT`、関数の入口（`interpreter_push`、C からの入りと bytecode の呼び出しの両方）、generator・async の再開（`vm_interpret_resume`）、`interpreter_unwind` の始め（要求が有れば例外を捨てて `VM_TERMINATED`、handler を探さない）、Wasm の後ろへの `br_if`。`VM_TERMINATED` は今の「その他の失敗」の道で全 frame を落とす。
- `vm/realm.c`: `vm_run_jobs` が job ごとに見て、要求が有れば queue を捨てて `VM_TERMINATED`。
- 長い native: `builtin_array.c`（merge sort の比較ごと、`array_get`・`array_set` を通る全ての要素の walk: fill など）、`builtin_json.c`（parse の値ごと、stringify の property ごと）、`builtin_string.c`（`repeat` の写しごと）、正規表現（`js/regexp.h` の `js_regexp_input` に `poll` の callback、matcher は 65536 歩ごとに呼ぶ。`regexp.c` は VM を知らないまま、`builtin_regexp.c` が `vm_heap_check_termination` を渡す）。
- `bind/`: `bind_window_terminated`（task の microtask と未処理の rejection を捨て、window に印、console に task ごとに 1 行の警告）・`bind_window_was_terminated`。`bind_run_script_source`・timer の callback・listener・checkpoint（`window_report_stop`）が `VM_TERMINATED` で呼び、status を上へ返す（microtask の checkpoint を飛ばす）。
- `view/view.c`: `browser_view_process` の 2 か所が `VM_TERMINATED` を script の error として shell に渡さない。`view_keep` は打ち切りの印の付いた頁を保たない（bfcache に残さない）。
- 今の production の道は `vm_heap_task_begin` を呼ばない（task の番号が 0 のまま、要求は何も終わらせない）。task の始めと終わり・navigation と destroy の要求は p005（script の thread の task）が結ぶ。

### 試験（host、2026-11-02）

| 試験 | 前（d635cd1027） | 後 |
| --- | --- | --- |
| `plan/ws345/tests/host-terminate.sh plain`（新、`host-terminate.c`） | — | 39/39 PASS。16 の場面（`for (;;) {}`、`while (1) try {} finally {}`、`try { for(;;){} } catch … finally`、遅い比較の sort、比較の無い sort（native の比較）、再帰 2^40、generator の中の loop と generator 自身の loop、async の中の loop、await の loop、promise の鎖、正規表現の backtrack、JSON.parse・JSON.stringify の長い物、`repeat`、forEach の callback）が要求の後 0.0〜13.9 ms で `VM_TERMINATED`、catch・finally の print 無し、depth・stack・jobs 0。各々の後の次の task の script は普通に走る。前の task の番号の要求は今の task を終わらせない、task の後に来た要求は次の task に落ちない。頁の window: microtask を走らせず console 1 行、同じ task の次の script も走らず行は増えない、window に印、次の task の script は走る |
| 同 `tsan` | — | 39/39 PASS、ThreadSanitizer の報告 0 |
| `sh plan/ws074/tests/bench/bug411.sh 前 後 前 後`（中央値 ms、2 回） | big-object-reads 1004・1036、compile-many-names 13・13、computed-after-change 35・35、grow-with-lookups 68・72 | 966・1029、12・12、34・33、65・68（遅れ無し） |
| 関数の中の loop 3e7 回（`--js`、5 回の中央値） | 4136 ms | 4180 ms（+1.1%） |
| `run-test262.py`（全体、`--jobs 32`） | test262 31598/47792、es5 7908/8087、落ちた 16194 件（language/statements 2446） | 同じ数、落ちた一覧（path）が前と同じ（diff 無し） |
| `sh plan/ws334/tests/stage1.sh TREE plain`（前は `git archive d635cd1027` の tree） | host-build warning 0・built 102、host-tests 82 exit 0・17 other、js 19/19、loader 11/11、http 15/15、http-async 19/20（async-sheets）、fetch 38/38、component 83 checks PASS | 全て同じ。host-* の exit の一覧が前と同じ（diff 無し） |
| `sh plan/ws334/tests/cross-build.sh` | — | 3 回目に PASS（win64・winarm64、compat と libtruetype の warning 0）。1 回目・2 回目は FAIL: OpenSSL の初めての build が host の負荷（load 18、段 1・tsan の build と並べた）で make の時間切れ 900 s に掛かった（`make-win64.log`・`make-winarm64.log` の Terminated）と、2 回目の win64 の Wine の vulkan-1.dll の確かめ（libbrowser を link しない delay-load.c）が 120 s の時間切れ。libbrowser の object は 1 回目から -Werror で通っていた |

- test262 は `build/ws074-suites/test262` を p8-uat3 の worktree の suite（7ab7fafa）への symlink で読んだ（読むだけ）。
- 変えた試験の file: 新しい `plan/ws345/tests/host-terminate.c`・`host-terminate.sh` だけ（WS074 の試験・host-build.sh は変えていない）。
- 未実施: QEMU・実機・Wine での打ち切り（この Phase は host の試験だけ）。task の番号を production の道（view・page）で始める事と navigation・destroy の要求は p005。

### 判断（P5、範囲の中の技術）

- 打ち切りの後の頁は生きたまま（Firefox の「script を止める」と同じ）。止められた task の残りの script は入口で次々に止まり、console の行は task に 1 行。`VM_TERMINATED` は bind から page・view へ上がる（task の持ち主が抜ける）。
- 番号は 32 bit（32-bit の target でも 1 語の atomic、0 を飛ばして回る）。
- 長い native は個々の数え方でなく heap の 1 つの数（`vm_heap_count_step`）で 4096 歩ごと。`array_get`・`array_set` に入れたので Array.prototype の walk は全部それで見る（fill の 4e7 要素が 18 s 掛かり、最初の試験で見つかった）。
