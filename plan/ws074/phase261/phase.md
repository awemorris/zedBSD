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
