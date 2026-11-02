<!-- awesome-plan project=zedbsd record=ws345-p009 -->

# ws345-p009 iframe ごとの script の thread（段 A）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

iframe（入れ子を含む）ごとに `browser-script` の thread を持たせる（D3、design.md §4.1・§5.3・§8）。段 A は全 frame が一つの agent（一つの heap と lock）。

## file

- `bind/frame.c`・`page/frame-load.c`・`page/page.h`・`bind/timer.c`: frame ごとの task の queue と thread、agent の lock、frame の木（page の lock）、frame の上限 64（32-bit の target は 16、J4）。page に一つの `fetches`・`scripts`・timer（`timer_run_children`）を frame ごとに分ける。frame ごとの loader の client。
- `vm/realm.c`: microtask の checkpoint を agent に一つ（lock の持ち主が agent の全 realm を空に）。
- `view/view.c`: 戻る・進むで保つ頁の iframe の thread を終えて join、戻す時に作り直す（作れなければ読み込み直す）。
- `vm/heap.c`: lock を取った thread の stack の底（R6）。
- `view/front.c`: 入力の routing（確定の iframe の枠の表で iframe の mailbox へ、J6 の答え次第）。
- `paint/`: iframe の frame の item（p006）を各 frame の確定に。

## 試験

- 新しい host の試験 `plan/ws345/tests/host-frames.c`: iframe の timer が top と別の thread（名前と thread の id）で走る、同じ origin の親子の同期の access（`parent.document`・関数の呼び出し）が今と同じ値、iframe の無限 loop の間も窓の代わりの loop の遅れが 20 ms 以下、iframe を外すと 1 s 以内にその thread が join される、65 個目の iframe は `contentWindow` が `null`、frame A が積んだ frame B の realm の promise の反応が A の checkpoint で走る、iframe の同期の XHR が top の thread を待たずに終わる（指摘 2 の deadlock が無い）、保った頁の iframe の thread が join され戻すと作り直される、thread の `start` が失敗する表で iframe が browsing context を持たず親が続く。
- 既存の iframe の試験（host の frame の試験、DOM の試験）が前と同じ。ThreadSanitizer の variant。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。
