<!-- awesome-plan project=zedbsd record=ws345-p004 -->

# ws345-p004 VM の打ち切り

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

長く走る script を外の thread から止める（navigation・閉じる、design.md §9）。

## file

- `vm/vm.h`・`vm/internal.h`・`vm/interpreter.c`（`VM_OP_LOOP_HINT`、関数の入口、`interpreter_unwind` が打ち切りを catch・finally に渡さない）・長い native（`builtin_array.c` の sort、正規表現の match、JSON、`String.prototype.repeat` など、回数で見る）。
- `bind/`・`page/`: 打ち切りで抜けた task の後片付け（microtask を捨てる、console に一行、頁は「打ち切られた」印で bfcache に残さない）。

## 手順

1. heap（段 B の agent）ごとの `atomic_int` の flag と、`vm_heap_request_termination`（どの thread からでも）・`vm_heap_clear_termination`。
2. 見る所を入れ、新しい status（`VM_TERMINATED`）で全ての frame を抜ける。native から呼んだ JS（callback）も抜ける。
3. 速さ: loop の印での atomic の relaxed の読み 1 回。BUG-411 の bench で遅くならない事を測る。

## 試験

- 新しい JS の host の試験 `plan/ws345/tests/host-terminate.c`: 別の thread が 200 ms 後に flag を立て、`for (;;) {}`、`while (1) try {} finally {}`、`try { for(;;){} } catch (e) {}`、`[...].sort(遅い比較)`、再帰、generator・async の中の loop が 300 ms 以内に抜け、catch・finally が走らない。flag を下ろすと次の script は普通に走る。
- `sh plan/ws074/tests/bench/bug411.sh` の前後（遅れは 2% 以内）。js、test262 の部分集合（`run-test262.py` の es5 と language/statements）が前と同じ。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通り、test262 の落ちた一覧が前と同じ。
