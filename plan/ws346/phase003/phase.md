<!-- awesome-plan project=zedbsd record=ws346-p003 -->

# ws346-p003: 照合（verify）の道具と bench

Status: cleared（2026-11-03 Q1。verify で §6.2 の組が off と同じ・差異 0、わざと壊して検出、bench の基準を取った）
Disposition: normal
Parent: [WS346](../ws.md)
Primary Milestone: MG006
Queue: [q2003](../../queue.md)（Q1 が直に実装、2026-11-03 ユーザーの指示）
基点: Q1 が worktree を作る時の main（design.md の基点 8f181e0e1f 以降）

## 目標

`BROWSER_LAYOUT_CACHE=verify` の layout の照合の骨組み（§5.5.2 の layout の部分）と、速さの bench（§8）を作る。
まだ差分の計算は無いので、`verify` は「同じ build を 2 回して比べる」形になり、違いは 0 のはず（道具の自己検査）。

## 書く物

- `page/page.c`（`page_layout_work` の後に、mode が `verify` の時だけ scratch の木へ `layout_build` して `layout_dump` を比べ、違いを報告・計数）。
  比べる時、両方の木は同じ入力（style の engine・viewport・scroll・transition）で作る。違いの報告の形は §5.5.2 の通り。
- `plan/ws346/tests/bench/restyle.html`・`plan/ws346/tests/bench/restyle.sh`（§8 の 4 項目。`plan/ws074/tests/bench/bug411.html`・`bug411.sh` の形）。
  各項目は `bench <名前> <ms> <check>` を console に出す。check は結果の値（例 offsetWidth の合計）で、mode で変わらないこと。
- `plan/ws346/tests/run-verify.sh`: 渡した browser で §6.2 の試験の組を `BROWSER_LAYOUT_CACHE=verify` と `off` で流し、PASS の数と
  `mismatches` の合計を並べて出す（live の頁は含めない）。

## 確かめ

- `verify` で §6.2 の試験の組の `mismatches=0`、PASS の数が `off` と同じ。わざと壊す: 照合の比較で 1 行目を書き換える一時の改変で違いが報告されること。
- `sh plan/ws346/tests/bench/restyle.sh build/ws074-host/plain/browser` が 4 項目を出す。`off` の基準の数を「結果」に書く。

## 完了の条件

上の全部。

## 読む物

1. `AGENTS.md` の §4（担当の共通の規則）・§6（git）・§7（禁止）・§9（検証）・§10（コード）。
2. [plan/coding-style.md](../../coding-style.md) の全文（新しいコードに適用）。
3. [design.md](../design.md) の §0〜§9（この Phase は特に §5.5.2・§8）。
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

2026-11-03 Q1（main の checkout で実装、p002 の main `02be09ad11` の上）。

### したこと

- `page/page.c`: `BROWSER_LAYOUT_CACHE=verify` の時、`page_layout_work` が（container query の段の後に）`page_layout_verify` を呼ぶ。同じ入力で全体の木を `layout_build` で別に作り、
  `layout_dump` の 2 つを行ごとに比べる。違えば `layout-cache: layout mismatch line=N` と両方のその行を stderr に出し、`mismatches` を数え、全体の木を page の木にする
  （差分の木は解放。同じなら全体の木を解放）。確保・build の失敗は error を返す（今の layout の失敗と同じ扱い）。
- `plan/ws346/tests/bench/restyle.html`・`restyle.sh`（4 項目、3 回の中央値、mode ごと、run 間で check が違えば MISMATCH）。
- `plan/ws346/tests/run-verify.sh`: dom・js・fetch・WPT reftest（既定 100 件）・Acid の 5 つを off と verify で流し、最後の行と verify の差異の数を並べる（browser の stderr を
  wrapper で log に写して数える）。scripted reftests（独自の client）は別に流す。

### 自分で決めた点

1. style の照合は p005 で足す（今の verify は layout の木だけ。scratch の木は page の engine を使うので style の cache を共有する）。
2. 比べるのは `layout_dump` の文字列（R1 の契約）。dump に出ない field（描画の display list など）は比べない。
3. WPT の checkout: `build/ws074-suites/wpt` は `/tmp/zedbsd-ws074/build/ws074-suites/wpt` を指す壊れた symlink だった（/tmp が消えた）。symlink は残し、指す先の directory を作り、
   fetch-suites.sh と同じ手順（`git init`・pin の 2d66b9b7998b を `fetch --depth 1`・checkout、LICENSE.md の BSD を確認）で入れた（fetch-suites.sh は directory が在ると
   「Queue manager が消せ」と止まるため、手で同じ手順を行った。何も消していない）。`/tmp` なので再起動で消える。

### 確かめ

- host build warning 0。
- `sh plan/ws346/tests/run-verify.sh`（rc 0）: off と verify で同じ。run-dom-tests 39/42、run-js-tests 21/21、run-fetch-tests 38/38（blocking・`--async`）、
  run-wpt-reftests 62 pass・37 fail・1 error・0 timeout of 100、run-acid-tests Acid3 100/100・pixel 一致 100.00%（exact match True）。**verify の差異 0**。
- わざと壊した: 全体の木の `document_height` に 64 を足す一時の改変で、`layout mismatch line=1`（`746.00` と `747.00`、1 行目は layout 単位の丸めで 1 の違い）と `mismatches=1` が出た。
  元に戻して `cmp` で一致を確かめた。
- bench の基準（`BROWSER_LAYOUT_CACHE=off`、1 回、Linux host、plain build `-O1`）: class-then-offset 14183 ms・style-then-offset 14140 ms・append-then-rect 14395 ms・
  toggle-then-computed 646 ms（check は 111000・22400・123800・12550）。1 回の全体の layout は約 70 ms（2000 個の element）。3 回の中央値は p005 以降の測りで取る。

### 未実施

- bench の 3 回の中央値（1 回だけ取った）。
- scripted reftests（`run-interop2025-scripted-reftests.py`）の verify は p005 から。
