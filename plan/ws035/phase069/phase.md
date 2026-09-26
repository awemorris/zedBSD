<!-- awesome-plan project=zedbsd record=ws035p069 -->

# ws035-p069: App Home の PoC（デスクトップをめくってアプリを起動する）

Phase ID: `ws035-p069`
Parent: [WS035](../ws.md)
Status: planned
Phase disposition: normal
Queue: —
承認: 2026-09-26 ユーザー「下記のアプリケーションランチャーも取り組んでほしいです。まずはPoCでよいです。私が実機で起動したときに、mviewやzdesktop-terminalを起動できるようにしたいからです。」
設計: [app-home-design.md](../app-home-design.md)

## 範囲（PoC）

1. zwl --glass に App Home: 左上の zedBSD アイコンのクリックと左上端から右下への drag で開く。デスクトップ（窓と壁紙）が
   右下へ退き少し縮み、右端と下端に角と影が残る。下から明るい Home（ぼかした壁紙を明るく）が現れる。
2. アプリの格子（6 列、64〜72 px のアイコンと名前）。アプリの一覧は設定 file（名前・command・印）から。最低限 Model viewer と Terminal。
3. アイコンのクリックでアプリを起動（fork と exec、`WAYLAND_DISPLAY` を渡す）し、Home を閉じる。
4. 閉じる: アイコンの再クリック、残った端のクリック、Esc、右下から左上への drag。
5. 文字を打つと検索（上部中央に文字、格子を絞る）、Backspace で空・Esc で解除。Enter で先頭を起動。
6. ページング・並べ替え・起動のアニメーションの細部は範囲外（1 ページ）。

## 受け入れ

1. Venus の guest で Home を開き、画面（VNC）で退いたデスクトップの角と格子を確かめ、Terminal と Model viewer を起動して窓が出る。
2. 検索で絞り込める。
3. 実機（i915、capture）の zdesktop の scenario で Home が開き、アプリを起動できる（可能なら）。
4. p052・p059・p062 の回帰、build は warning 0、style-check 0。
