<!-- awesome-plan project=zedbsd record=ws346-p007 -->

# ws346-p007: layout の再利用 B: 部分木の接ぎ木

Status: planned
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: 未割り当て（Q1 が Sonnet = phase-runner-low に渡す時に決める）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

根の条件と入力の組が合う時、前の木の部分木を新しい木へ写して layout を飛ばす（§5.3.4）。page の統合（§5.3.6）を終える。

## 書く物

- `layout/`（接ぎ木の深い写し、相対の幾何への戻し、後の段の field の初期化）、`page/page.c`（layout の bit の消去、container query の段の扱い）。
- 計数 `grafts`・`graft_boxes`。
- 試験の頁を p006 の reuse-*.html に足す（同じ頁で DOM を変えて幾何を読む繰り返し。根の外の変更・根の中の変更・根の大きさが変わる変更）。

## 確かめ

- `run-verify.sh` で `mismatches=0`、PASS の数が `off` と同じ。ASan の build で pages・DOM・JS。
- bench: 項目 1・2 が `off` の 10 倍以上、項目 3 が 3 倍以上（届かなければ理由と測った内訳を書く）。
- 前の木の arena を指す pointer が残らないこと（ASan で、前の木の解放の直後に新しい木を dump・描画する試験）。

## 完了の条件

上の全部。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.3.1・§5.3.4・§5.3.6）。
4. 触る source の今の code（下の「書く物」の file と、その呼び出し元）。

## 守る事

- 下の「書く物」の file と試験だけを変える。他の module・公開の header（`userland/desktop/include/browser/browser.h`）・HAL・toolchain・
  `plan/` の他の記録（AGENTS.md・master.md・queue.md ほか）を変えない。この phase.md の「結果」の節は書いてよい。
- design.md の契約（関数の意味・bit・mode・計数の行の形）を広げない・変えない。足りない・誤りがあれば止めて Q1 に報告する。
  契約が黙っている所は自分で決め、「結果」の節に列挙する。
- source の comment に WS・Phase の番号、plan/ への参照、日付の経緯を書かない。
- `rm` などで file を消さない。commit は `git commit -m WIP -- <path>...`（新しい file は先に `git add`）。push しない。
- build は `CC=clang sh plan/ws074/tests/host-build.sh plain`（warning 0）。大きな build の後は `sync`。
- 試験が通った後、git の外で実装を数通りわざと壊し、試験が FAIL することを確かめて戻す（`cmp` で戻ったことを確かめる）。
- `BROWSER_LAYOUT_CACHE=off`（と未設定）で、既存の試験の結果が基点と同じであること。

## 報告（Q1 へ）

commit の SHA、流した command と最後の行、自分で決めた点、未実施、わざと壊した方法と結果。

## 結果

途中の記録（2026-11-03、Q1 が直に実装）:

- 方式: 前の tree の箱を copy せず、そのまま新しい tree の親の下に付ける（構造共有）。`box_build_element` の先頭で
  `layout_reuse_take` が、DOM の bit が clear・style engine が style を stale にしていない・前の tree でその要素の箱が 1 つで
  plain flow（`reuse_plain`）・親が block の流れ、の時に箱を返し、その部分木は build しない。build した要素の前の箱の子が
  `layout_reuse_expand` で索引に入る（前の tree 全体は読まない）。新しい tree は前の tree の arena を引き取る
  （`wb_arena_adopt`、世代 24 か 1 GiB で全体を build し直す）。
- layout: 前の tree の箱は、同じ幅・高さ・flag で呼ばれたら中身を触らず（`reuse_skipped`）、最後の pass（`layout_absolute`）が
  `layout_reuse_shift` で中身を新しい位置へ平行移動する。違う入力なら箱のまま layout し直す。BFC の箱だけでなく、親の context の
  block も、float が周りにも中にも無い時（`reuse_leaky`、入力の bit 16）は同じ。list item は marker が変わると記録を消して layout し直す。
- 後の pass（`layout_position`・`sticky`・`scroll`・`box_apply_scale`）は plain flow の部分木（`layout_reuse_frozen`）を飛ばす。
- 失敗: 箱を取った後の失敗は前の tree を壊すので、`page_layout` は両方を捨てる。`@container` を使う頁は共有しない
  （前の tree から大きさを読むため）。
- DOM の記録: 子の増減・text の変更が親の subtree を restyle するのは、構造に関わる selector（`:first-child` `:last-child`
  `:nth-*` `:empty`、兄弟 combinator、`:has`）がある時だけにした（`css_engine_change_scope` が CHILDREN・TEXT も答える。UA の sheet の
  数え方は details の子だけ）。
- 計測（bench、1 回、off は約 14 s・toggle 0.70 s）: class 1.48 s、style 1.29 s、append 1.25 s、toggle 0.042 s。
- 試験: host-change 88/88、host-restyle-scope 76/76、run-pages 6/6（reuse-flow.html を足した）、run-verify.sh は off と verify の差分 0
  （dom 39/42・js 21/21・fetch 38/38・WPT 62/37/1・Acid3 100/100）、ASan の run-pages 6/6。
- 試験が見つけた誤り: 親の context の block は、周りの float の「有無」だけでなく形で行が変わる。有無が同じなら再利用する、と
  していたのを、前も今も float が無い時だけ再利用する、に直した（reuse-flow.html の「背の高い float を消す」の step）。
  list item の marker の幅が変わる step（reuse-marks.html、9 件の list の先頭に足す）も足した。
- わざと壊した確認（git の外、`cmp` で戻した）: 後の位置の移動を消す（FAIL）、DOM の bit の確認を消す（FAIL）、style が
  今のものかの確認を消す（FAIL）、float の確認を消す（FAIL）、marker が変わった list item の記録を消す（FAIL）。
  float を中に持つ block を避ける旗（`reuse_leaky`）は、記録が float を持った後に作られるため上の確認が必ず先に断り、
  旗は要らなかったので消した。
- 残り: DOM 全体を歩く pass（media・images・frame・animations・geometry の索引）は変更の量によらず残る。
