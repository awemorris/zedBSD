<!-- awesome-plan project=zedbsd record=ws078-p007 -->

# ws078-p007（提案）: 注釈の `zdesktop` を一度に置き換える

Status: cleared（2026-10-08 Q1 判定、T1-401・403）（旧: in-progress（2026-10-08 P1 q879: 置き換え・確かめ・build warning 0 まで。boot test は T1。下の「q879」）（旧: planning（2026-10-01 の提案。main が ws.md の Phase の表に入れるまで提案のまま）））
Disposition: normal
Parent: [WS078](../ws.md)
Queue: q879（P1、2026-10-08 Q1 の承認の 3 番）
依存: **WS104 の p001〜p007 が main に入った後**（WS104 の patch は `zdesktop` を含む文脈の行を持つ）。`userland/desktop` と `platform/amd64/vmunix.mk` を変える
worktree が無い静かな時点（ws.md「改名は … 静かな時点で main か 1 つの agent が一度に」、main の判断「WS074・WS081 の作業が落ち着いた後、1 つの agent で一斉に」）。

## 範囲

[guide.md](../guide.md) §2.3 の B1・B4・B9: plan の外の注釈の `zdesktop`（2026-10-01 で 678 行 / 175 file。C の file では全て注釈）、「the zedBSD desktop」（10 行）、
`keiland-x11` の注釈の「zdesktop-x11」。

除く: `userland/desktop/ime/`（29 行 / 7 file、IME は人間が作業中）、`plan/` の全て（記録と試験、試験の log の印 `ZWL` 等と `/tmp/zdesktop.log`）、
make の変数（`DYNAMIC_ZDESKTOP_*`・`LIBZDESKTOP_*`、p008 提案）、`zed-*-v1` の file 名（p008 提案）、`userland/retro/`。

## 手順（2026-10-01 追記）

1. 始める前の確かめ（main）: `git worktree list` と master の割り当て、`git log --oneline -1 -- plan/ws104` で WS104 の p007 まで入ったこと。
2. 数え直し（guide.md §5.1 の 1〜3 行目）。値を記録する。
3. before の build（guide.md §5.3 の 1〜2 行目、`<W>` = `ws078-p007`）。
4. 置き換え。語の既定（文脈で読み、文が通ることを確かめる）:
   - 「zdesktop's」→「the compositor's」、「zdesktop」→「the compositor」（文頭は「The compositor」）。
   - 「the zedBSD desktop」→「the desktop」、「zedBSD zdesktop:」（shader の先頭）→「Keiland compositor:」。
   - 「zdesktop-x11」→「keiland-x11」。
   - 機械的な置き換えの後、`git diff` を file ごとに読んで文の誤り（「the the compositor」、冠詞の重なり、大文字）を直す。**1 行を 1 行で置き換え、行を足し引きしない**。
   - shader（`userland/desktop/wayland/shaders/*.frag`・`*.vert`・`regenerate.py`）は注釈だけ変え、`regenerate.py` は流さない。
5. after の build と object の比較（guide.md §5.3 の残り）。差が 0 であること。
6. style: 変えた C の file に `python3 plan/tools/style-check.py <file>…`（新しい違反 0。注釈の行の長さの規則に注意: 置き換えで行が長くなったら、その段落の中で
   折り返しを直す。その時は行の数が変わるので、object の比較で同じであることを確かめる）。`git diff --check`。
7. boot test（guide.md §5.4）。
8. 記録と、各 agent への通知（main）: 新しい語と、IME の 29 行が残ること。

## 完了の条件

- `git grep -I -c -i 'zdesktop' -- ':!plan' ':!.internal' ':!userland/desktop/ime'` の数が、make の変数（p008 の分）と header の file 名の参照（p008 の分）だけ。
- 前後の object が byte で同じ（`object-diff.txt` が 0 行）、build の warning 0、style の新しい違反 0、`boot-test: PASS`（PNG をユーザーに見せる）。

## q879（P1、2026-10-08）

- 前提: WS104 は completed、p008（make の変数・header の file 名）は 3a61b1139 で main に入った。IME（`userland/desktop/ime/`）は人間の作業から戻っているので、今回は含めた（9 file）。
- 数え直し: plan と `*.md` の外で 661 行・192 file（C 141・h 29・shader 4・Makefile ほか）。
- 置き換え（1 行を 1 行、`scratchpad` の script で機械的に。語は手順 4 のとおり）:
  - 「zdesktop's」→「the compositor's」、「zdesktop」→「the compositor」。文頭（前の行が終わった comment の最初、または「.」の後）は「The …」。冠詞・所有の後（the・a・its など）は「compositor」だけ。
  - 「zedBSD zdesktop:」（shader の先頭）→「Keiland compositor:」。「zdesktop-x11」→「keiland-x11」。
  - file 名の一部（`config-amd64-zdesktop.mk` など）は残した。
  - 結果: 660 行・191 file。残る 1 行は `userland/tests/kuidemo/Makefile` の `plan/ws035/tests/config-amd64-zdesktop.mk`（本当の file 名）。
  - `tests/scenarios` の md 2 file と `userland/tests/mview/README.md` は文書なので、この Phase では変えていない。
- 確かめ:
  - 変えた C・h・shader の 174 file で、`gcc -fpreprocessed -dD -E -P`（注釈を除いた中身）が前後で同じ（違い 0）。行の数も同じなので、object（`__LINE__` を含む）は変わらない。
  - Makefile・sh・py・conf の変更も、注釈と regenerate.py の docstring だけ。
  - `git diff --check` は空。style-check の出力は前後とも 225 行（新しい指摘 0）。
  - build（warning 0、target を名指し）: libkeiland・libvulkan・libwayland-client・wayland・files・settings・terminal・keiland-ime・xserver・browser。keiland-linux の all も rc 0。
- 未実施: boot test（T1）。


## Q1 の判定（2026-10-08）

T1-401: 標準の image（AAT・files・criteria）が作れ boot-test PASS、C9 は p072 以外 PASS。p072 は試験 file が 83f5512ce で消されているのに C9 の一覧に残っていた（Q1 が plan/ws099/tests/criteria.sh から外した）。T1-403: boot-test PASS。
