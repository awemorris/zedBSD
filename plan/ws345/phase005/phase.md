<!-- awesome-plan project=zedbsd record=ws345-p005 -->

# ws345-p005 view の front と top の script の thread

- WS: [ws.md](../ws.md)。設計: [design.md](../design.md)。
- 状態: planned

## 目的

`BROWSER_THREADING_OWN` の view: top の browsing context を `browser-script` の thread に移し、窓の thread は front の口（積む・snapshot を読む）だけを使う（D1・D3、design.md §5.2〜§5.4・§9）。この Phase では style・layout・display list は script の thread（p008 で style の thread へ）、描画は CPU の確定した frame（p006 で GPU と frame の箱）。

## file

- `include/browser/browser.h`: `threading`、`frame_wake`、`browser_view_request_image` と callback `image`、`_OWN` で `ENOTSUP` の口の一覧。
- `view/front.c`（新）: mailbox（FIFO、lock と condition、入力の合わせ方、離散の入力 1024 件の上限）、snapshot（url・title・can_go・progress・scroll・document の高さ・scroll の範囲・文字入力の状態）、通知の queue と `browser_view_process` での callback。
- `view/view.c`: 今の view を script の thread の中の物にする（front から呼ばれる）。script の thread の loop（mailbox、loader の client、timer の期限の `cond_wait_for`）。
- process に一つの可変の static（design.md §11 R1: `bind/computed.c`・`page/svg.c`・`css/cascade.c`・`layout/position.c`・`net/http.c`）を page・thread の context に移す。
- 打ち切り（p004）を navigation・destroy に結ぶ。

## 手順

1. front と mailbox、snapshot、通知。`_CALLER` の道は今のまま（front が直に呼ぶ）。
2. script の thread の始めと終わり（`browser_view_create` が始め、`_destroy` が打ち切り → 終わりを積む → join）。
3. 入力の順と合わせ方、routing は top だけ（iframe は p009）。
4. CPU の確定した frame: 描画の更新の後に pixel ではなく display list を確定し、`browser_view_render_pixels` がどの thread からでも最新を描ける（p006 で frame の箱に広げる）。

## 試験

- 新しい host の試験 `plan/ws345/tests/host-threaded-view.c`（窓の代わりの thread と描画の代わりの thread を持つ、probe の形）: (a) timer の script が 5 s 回る間と同期の XHR が connect の掛かる listener を待つ間に、窓の代わりの loop の event の遅れの最長が 20 ms 以下、(b) 押し・key・移動の順が page の listener の見た順と同じ、(c) 移動の合わせ方（連続の移動が 1 つ）、(d) `for (;;) {}` の間の destroy が 1 s 以内に戻る、(e) navigation が無限 loop の頁から 1 s 以内に新しい頁を出す。
- 両方の mode で host-navigation-wait と同じ場面（新しい試験の中で）。
- ThreadSanitizer の variant で (a)〜(e) に race の報告が無い。
- 回帰（この Phase の終わりに 1 回）: `sh plan/ws074/tests/host-build.sh plain`（warning 0）、host-* の exit が前と同じ、js、http・http-async・loader・fetch、component（`plan/tools/browser-component/run.py`）、`sh plan/ws334/tests/stage1.sh TREE plain` の前後、`sh plan/ws334/tests/cross-build.sh`。

## 受け入れ

- 上の試験が通る。`_CALLER` の回帰が前と同じ。
