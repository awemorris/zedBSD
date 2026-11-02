<!-- awesome-plan project=zedbsd record=ws345-p010 -->

# ws345-p010 origin ごとの agent（段 B）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

cross-origin の iframe を別の agent（別の heap と lock）にし、並べて走らせる（D3、design.md §4.1・§8）。

## file

- `vm/`（heap の複数）・`bind/frame.c`・`bind/window.c`（遠くの WindowProxy・Location）・`bind/cloning.c`（heap の外の bytes の形の structured clone）・`page/frame-load.c`・`view/front.c`（agent ごとの打ち切り）。

## 試験

- `plan/ws345/tests/host-frames.c` に足す: cross-origin の iframe（試験の server の 2 つの port）の無限 loop の間に top の timer が走る、postMessage が両方向に届き値が同じ（Array・Map・Date・ArrayBuffer の transfer）、`MessagePort` の transfer は `DataCloneError`、cross-origin の `location` の代入が子を navigation する、`frames.length`・`parent`・`top` が正しい、cross-origin の DOM の access が `SecurityError`。
- WPT の `html/webappapis/` と `html/browsers/` の postMessage・cross-origin の部分集合（Q1 と範囲を決める）。ThreadSanitizer の variant。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。
