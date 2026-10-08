<!-- awesome-plan project=zedbsd record=ws127-p008 -->

# ws127-p008: 全文規約と回帰（WS の最後）

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T2-025 PASS 8/8（QEMU）。FreeBSD の build の分は 10/13 以降）（旧: in-progress（q667、P2、2026-10-04。規約の見直し・build・host は済み、QEMU の回帰と FreeBSD の build は T1 待ち。実機の p007 は実機待ち））
Disposition: normal
Parent: [WS127](../ws.md)
Queue: q667（Q1 の dispatch、2026-10-04。今できる分）
依存: 実装の Phase の全て
目安: 2h（1 Queue）。実行者の目安: phase-runner-mid
所有 path: WS127 で変えた source、`plan/ws127/`

## 範囲

WS127 で変えた全ての source を coding-style.md の全文で見直し、style-check と build（warning 0）、Files の回帰 14 本・host・Settings の host（共有 file を変えたとき）・WS094 の desktop の手順（`files-desktop-guest.sh install show input menu`）・boot test。

## 受け入れ

範囲の中の違反 0、全て PASS。WS の完了の判定を Q1 に依頼（試験は `plan/tools/files/` へ）。
変えた source の style-check の違反 0、build の warning 0、`files-regress.sh` の 14 本と boot test PASS。

## 未決の判断

なし。

## 実施（2026-10-04、q667、WS128 p008 と同じ回）

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
- system の mount の Trash（q666 の報告の残り、Q1 の指示で判断）: GIO が system-internal の mount に trash を作らないのに合わせ、`/`・`/tmp`・
  `/var/tmp`・`/var` の top と、`/dev`・`/proc`・`/sys`・`/run`・`/boot`・`/snap`・`/var/lib` の下の mount には volume の trash を作らず、home の
  trash（copy）へ（`files/trash.c` の `trash_system_top`）。`/tmp/vol` のような利用者の mount は今のまま volume の trash。host-model は
  /dev/shm を system の mount として「home の trash へ」を確かめ（8a）、volume の trash（8b）は sudo で build/ の下に mount した tmpfs で確かめる
  （sudo が無ければ skip）。
- build: zedBSD amd64（`ZEDBSD_CONFIG=plan/ws128/tests/config-amd64-imageview.mk BUILD=build/p2-files`、files・terminal・imageview・textedit・
  libkeiui.so）rc=0 warning 0。Linux: `make -j16 keiland-linux KEILAND_LINUX_BUILD=build/p2-keiland-linux` rc=0 warning 0（files・imageview・
  terminal・textedit・notes を含む）、`plan/tools/keiland-linux/makefile-sync.sh` PASS。FreeBSD は guest の native build が要るので T1 に依頼
  （`plan/tools/keiland-freebsd/backend-test.sh`）。
- host 試験（全て PASS）: files の host-model（121）・host-p010・host-default・host-png・host-spring・scroll-bar-test・host-pdf-thumb、
  imageview の run-host・host-share、terminal-p006・terminal-p009、textedit の host-core（53/53）。
- 未実施（T1 へ依頼）: `files-regress.sh`（14）・files-p002・files-p003・files-p006・WS094 の `files-desktop-guest.sh install show input menu`・
  boot test、FreeBSD の native build。実機（ws127-p007）は実機待ち。

## 検証の方法と範囲

QEMU の Venus（`plan/tools/files/files-guest.sh`、`files-regress.sh`）と host の試験。console・serial の log では判定しない。各 Phase の最後に `files-regress.sh` の 14 本と boot test。
Settings と共有の `canvas.c`・`text.c`・`icons.c` を変えたら `sh plan/ws089/tests/host-build.sh` と Settings の host 試験も流す。

## Event

2026-10-02 / ws127-beta1-plan: fg019 の計画で新設（planning）。p001 の結果とユーザーの選択で範囲を確定し planned にする。

## QEMU の回帰（Q1、2026-10-04）

T1-081（QEMU、main e3f0d16）: build-files-image.sh の image で files-p002・files-p006・files-regress 14 本 PASS、imageview の image で files-p003・WS094 の desktop・boot-test PASS（imageview の image には見本の home の make-home.sh が無く Files の試験は前提を満たさない、試験の image の違い）。全文規約は P2 の f873f4f。実機の p007 が残るので in-progress。
