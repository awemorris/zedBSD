<!-- awesome-plan project=zedbsd record=ws128-p008 -->

# ws128-p008: 全文規約と回帰（WS の最後）

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T2-025 PASS 8/8（QEMU）。FreeBSD の build の分は 10/13 以降）（旧: in-progress（q667、P2、2026-10-04。規約の見直し・build・host は済み、QEMU の回帰と FreeBSD の build は T1 待ち。実機の p007 は実機待ち））
Disposition: normal
Parent: [WS128](../ws.md)
Queue: q667（Q1 の dispatch、2026-10-04。今できる分）
依存: 実装の Phase の全て
目安: 2h（1 Queue）。実行者の目安: phase-runner-mid
所有 path: WS128 で変えた source、`plan/ws128/`

## 範囲

WS128 で変えた全ての source を coding-style.md の全文で見直し、style-check・build（warning 0）・A1 の回帰の全て・boot test。

## 受け入れ

範囲の中の違反 0、全て PASS（A1・A6）。WS の完了の判定を Q1 に依頼（残す試験は `plan/tools/` へ）。

## 検証の方法と範囲

QEMU の Venus と host。 やっていない確認は「未実施」と書く。

## 未決の判断

なし。

## 実施（2026-10-04、q667、WS127 p008 と同じ回）

- 対象: WS127（Files: p002・p003・p004・p006）と WS128（Notes p002・Text Editor p003・Image Viewer p005・Terminal p006・p009・p010・p011）で
  変えた source。`plan/tools/style-check.py` を files・imageview・terminal・textedit・notes の全ての .c/.h と `libkeiui/scroll-bar.c` に流し、
  指摘を直した: files/main.c（閉じ括弧の後の段落 2）、files/ui-preview.c・ui-grid.c・ui-desktop.c（条件の中の `fm_thumb_kind` 3）、
  textedit/draw.c・canvas.c・app.c（段落の comment 5）、libkeiui/scroll-bar.c（1、ws127-p002 の file で整形のみ）。files/thumb-cache.c は p004 で 21 件。
  全文の規約の追加の確かめ（`plan/tools/imageview/style-extra.py` を WS の変更行に当てる）で、q666 の自分の行の 1 行 3 節以上の条件・Boolean の式 8 件を直した
  （terminal/menu.c の radio の checked を `menu_same()` に、render.c・window.c の条件を複数行に）。
- 残す指摘: files/places.c の `goto cleanup` 4 件（1 つの後片付けの label への前向きの jump で、規約 §14 が許す形。checker は全ての goto を数える）。
  style-extra の既存の行（WS127・WS128 の前からの window.c・tablet.c など）は範囲の外。host の試験の C（host-share.c・host-pdf-thumb.c・
  terminal-p006.c）は既存の試験（host-model.c）と同じ簡潔な形で、本体の規約は当てていない。
- 結果: 上の全ての file で style-check 0、`git diff --check` 0。
- build: zedBSD amd64（`ZEDBSD_CONFIG=plan/ws128/tests/config-amd64-imageview.mk BUILD=build/p2-files`、files・terminal・imageview・textedit・
  libkeiui.so）rc=0 warning 0。Linux: `make -j16 keiland-linux KEILAND_LINUX_BUILD=build/p2-keiland-linux` rc=0 warning 0（files・imageview・
  terminal・textedit・notes を含む）、`plan/tools/keiland-linux/makefile-sync.sh` PASS。FreeBSD は guest の native build が要るので T1 に依頼
  （`plan/tools/keiland-freebsd/backend-test.sh`）。
- host 試験（全て PASS）: files の host-model（121）・host-p010・host-default・host-png・host-spring・scroll-bar-test・host-pdf-thumb、
  imageview の run-host・host-share、terminal-p006・terminal-p009、textedit の host-core（53/53）。
- 未実施（T1 へ依頼）: imageview-p005・imageview-guest（既存）・terminal-p006-guest・terminal-p009-guest・menu-p003・textedit-p003・notes-p002 の
  guest 試験と boot test、FreeBSD の native build。実機（ws128-p007）は実機待ち。

## Event

2026-10-02 / ws128-beta1-plan: fg019 の計画で新設。

## QEMU と FreeBSD の回帰（Q1、2026-10-04、T2-025）

WS128 の guest 7/7 PASS（imageview-p005・terminal-p006-guest・imageview-guest・textedit-p003・notes-p002・menu-p003・terminal-p009-guest）、FreeBSD の backend-test PASS（files・imageview・terminal・textedit・notes を含む native build の warning 0）。実機の p007 は実機待ち。
