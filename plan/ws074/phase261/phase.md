<!-- awesome-plan project=zedbsd record=ws074-p261 -->

# ws074-p261: History と Navigation API（同じ文書の中の navigation）

Status: in-progress
Disposition: normal
Parent: [WS074](../ws.md)
Primary Milestone: MG006
Queue: なし（p260 の一部。web のセッションが担当の agent に渡す）
Origin: p260 の gap の測定（2026-10-27）。Chromium が通り browser が落とす harness の約 330 件が navigation-api/ で、`navigation` が無い。

## 目標

`window.navigation`（Navigation、NavigationHistoryEntry、NavigateEvent、NavigationTransition、NavigationDestination、
NavigationCurrentEntryChangeEvent）と `history`（pushState・replaceState・state・length・back/forward/go と popstate）を、
同じ文書の中の navigation（fragment、pushState/replaceState、`navigation.navigate()` の intercept）について仕様どおりに実装する。
location.hash の変更と hashchange もこの model に載せる。

## 受け入れ

- iframe・window.open を使わない navigation-api/ の試験のうち、Chromium が通す物を browser も通す（`interop2025-chrome-gap.py` の harness）。
- 後退 0（同じ測定の他の harness 試験で、以前 PASS の物が FAIL にならない）。host build の warning 0。
- iframe を使う試験（文書をまたぐ navigation）は後続。ここでは扱わない。

## 方法

- 主に `plan/ws074/phase230/prelude.js`（`make-prelude.py` で `bind/prelude-source.inc` を作る）。足りない native（location との接続など）は
  `bind/` に最小限。
- 仕様: https://html.spec.whatwg.org/multipage/nav-history-apis.html

## 状態

2026-10-27（branch `worktree-agent-a570073a76f5fe6d3`）

- 実装: `prelude.js` に session history の model（entry の列・現在の index・classic の state・navigation API の state・scroll の保存）を置き、
  その上に `History`（pushState・replaceState・state・length・scrollRestoration・back/forward/go と popstate）、Location の setter と
  assign・replace・reload（fragment は hashchange 付き。`window.location =`・`document.location =` も）、Navigation API
  （Navigation・NavigationHistoryEntry・NavigateEvent〔intercept・scroll・signal・focusReset〕・NavigationDestination・
  NavigationTransition・NavigationCurrentEntryChangeEvent・NavigationActivation、navigate・reload・traverseTo・back・forward・
  updateCurrentEntry と method tracker の committed/finished）、`window.stop()`（進行中の navigate event の中止だけ）。
- native（`bind/`・`page/`）: host の `fragment` を `url_set`（文書の URL を同じ文書のまま変える）に替え、location.hash の native setter を
  `bind_scroll_to_fragment` に分けた。prelude の natives に setURL・scrollToFragment・fire（UA の trusted な dispatch）・
  setHyperlinkFollower を足した。`a`/`area`/SVG `a` の `click()` と script の click の dispatch で link を辿る（`bind_click_follow`）。
  load の後に `pageshow` を発火する。
- 測定（`run-interop2025-harness.py`、host build `build/ws074-nav`、warning 0）: 対象 173 件（Chromium が通り dev1 が落とす
  navigation-api/ で iframe・contentWindow・window.open を使わない物）は 0 → 160 PASS。
- 全 harness（`run-interop2025-harness-all.py`、1508 件）: PASS 506（dev1）→ 664。dev1 で PASS だった物のうち 12 件（css-grid）が非 PASS だが、
  この branch の基点 0500e8c8d の build でも同じ 12 件が FAIL（dev1 の build との基点の差で、この変更による後退ではない）。
  navigation-api/ は 168/343。iframe を使う navigation-api/ の 85 件が fail → no_output に変わった（どちらも非 PASS）。
- 残り（13 件）: form の submit・requestSubmit が無い（3）、HTMLAnchorElement の href・download の反映が無い（1）、navigateerror の
  lineno/colno（script の位置の情報が無い、4）、WritableStream・WebAssembly が無い（2）、CSS の `:target`（1）、読み込み時の autofocus（2）。
  文書をまたぐ navigation（intercept しない別文書への遷移・reload は navigate event だけで読み込まない）と iframe は後続。
  利用者のクリック（GUI）は navigate event を通らない。
