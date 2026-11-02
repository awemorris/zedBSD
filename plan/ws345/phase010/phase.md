<!-- awesome-plan project=zedbsd record=ws345-p010 -->

# ws345-p010 origin ごとの agent（段 B）

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

cross-origin の iframe を別の agent（別の heap と lock）にし、並べて走らせる（D3、design.md §4.1・§8）。

## file

- `vm/`（heap の複数）・`bind/frame.c`・`bind/window.c`（遠くの WindowProxy・Location）・`bind/cloning.c`（heap の外の bytes の形の structured clone）・`page/frame-load.c`・`view/front.c`（agent ごとの打ち切り）。

- p009 から移した物（2026-11-02 Q1 の判断）: iframe を自分の frame として layout・確定する事（親の確定した list の「frame の item」）、`page/frame-load.c`・`paint/list.c`・`paint/scene.c`・CPU と Vulkan の renderer・`view/front.c` の frame の箱。設計の覚え（P5）:
  - 今（p009 の段 A）は top の script の thread が `front_publish` で頁を確定する時、親の paint（`list_frame` → `page_frame_paint` → `frame_make`）が子の文書を agent の下で layout・paint し、子の list を親の list に写す。iframe の thread の 1 周の後は必ず top が起こされて確定し直すので、確定は自足し正しい。段 B では親の thread が cross-origin の子の heap を読めないので、この形は使えない。
  - 形: iframe の thread がその 1 周の終わりに（子の文書の世代か、親が最後に決めた枠の大きさ・zoom が変わった時）自分の文書を `frame_tree`・`page_lend_paint` で layout・paint し、view の「frame の箱」（frame の id → 最新の確定、lock の下、参照の数は atomic）に置いて `frame_wake` を呼ぶ。親の `list_frame` は子の item を写さず、新しい種類の item（frame の id・枠・clip・canvas の色）を一つ置く。枠の大きさと zoom は親の layout が決めて箱に書き（今の `frame_size_walk` の値）、子の thread を起こす。
  - 描画の thread は frame の item に出会うと、その id の最新の確定を箱から引いて（無ければ canvas の色）中の item を枠の位置へずらして描く（CPU は `paint/raster`、GPU は `paint/vulkan.c` の記録の再帰。深さは `FRAME_MAX_DEPTH` で抑える）。scene の画像の hold と font の bytes は子の確定が自分で持つ。
  - 入力の routing（design.md §5.3、J6）も同じ確定の iframe の枠の表で行う（p009 では top の thread が今のとおり子の文書に event を出す。段 A では同じ agent なので意味は同じ）。

## 試験

- `plan/ws345/tests/host-frames.c` に足す: cross-origin の iframe（試験の server の 2 つの port）の無限 loop の間に top の timer が走る、postMessage が両方向に届き値が同じ（Array・Map・Date・ArrayBuffer の transfer）、`MessagePort` の transfer は `DataCloneError`、cross-origin の `location` の代入が子を navigation する、`frames.length`・`parent`・`top` が正しい、cross-origin の DOM の access が `SecurityError`。
- WPT の `html/webappapis/` と `html/browsers/` の postMessage・cross-origin の部分集合（Q1 と範囲を決める）。ThreadSanitizer の variant。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。
- cross-origin の iframe の timer が無限 loop の間も、top の文書の script（timer・入力の listener）が走り続ける（2026-11-02 Q1 の判断で p009 の受け入れから移した。段 A の p009 では同じ agent の iframe の task が agent を持つので top の script は待ち、窓と描画だけが止まらない）。
