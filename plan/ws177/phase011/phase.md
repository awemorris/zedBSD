<!-- awesome-plan project=zedbsd record=ws177-p011 -->

# ws177-p011: Notes の Save Clean Copy の仕上げ（案 K3）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-10 Q1 判定: ユーザーの 5330 の UAT「WS177 はOK」、host・build は前の記録のとおり）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q887 の 1（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 62・63・64（WS175 ws175-p009）、[案](../phasing-20261008.md) の K3、設計 [ws175 design.md](../../ws175/phase001/design.md) の D1・[M11]

## 設計と変更（2026-10-08 P1）

| 行 | 形 |
| --- | --- |
| 62 form・Type 3 font・tiling pattern の /Resources | `libpdf/clean.c`: 頁の内容が名指す form・Type 3 font・tiling pattern のうち自分の /Resources を持たない物（頁の resource を借りる物）は、その内容（form と pattern の stream、Type 3 の /CharProcs の各 glyph）の名も頁の resource に印を付ける（`clean_follow_borrowed`、新しく名指された物も順に辿る。前は頁の resource を全部残していた）。自分の /Resources を持つ物は、object を書く時にその /Resources を自分の内容の名で刈り込む（`clean_own_resources`・`clean_write_resources` を頁・stream・Type 3 の 3 つの源に一般化）。内容が読めない時は全部残す |
| 63 name tree の /Limits と直接の file specification | 名の木の葉の中に直接書かれた file specification と、catalog の /AF の中に直接書かれた物も落とす（`clean_drop_direct`、最大 64）。名を落とした木の node の /Limits は、残る名の最小と最大から書き直す（`clean_write_limits`・`clean_tree_bounds`。名が残らない node は /Limits を書かない。number tree の node はそのまま） |
| 63 で見つけた不具合 | `clean_is_tree_node` が /Type の無い /Kids の dictionary を頁の木の node と見ていたので、**名の木の根（/Kids だけ）が copy で頁の木（2 0 R）に置き換わっていた**。/Type の無い物は /Kids と /Count の両方がある時だけ頁の木の node とした |
| 64 Save Clean Copy の後の案内 | Notes: 保存した clean copy を覚え、File の menu の「Open Clean Copy」（`NOTES_ACTION_OPEN_CLEAN`、copy を作った後だけ有効）で開く。status は「Saved a clean copy. File > Open Clean Copy opens it」。初めての clean copy の時だけ（Notes の設定 `notes.clean-copy-told`、settings-keys に登録）「Clean copy saved. This notebook keeps removed items in its history」を 2 倍の時間出す（D1 (a) の一度だけの注意）。log `NOTES CLEAN-COPY ... told=N`。問いの窓（dialog）は Notes に kl_ui の dialog の口が無いので、menu の項目と status で提案する形にした |

- 変更: `userland/base/libpdf/clean.c`、`userland/desktop/notes/main.c`・`menu.c`・`app.h`、`userland/desktop/settings-keys/settings-keys.c`。
- 試験: `plan/ws177/tests/host-clean-forms.{sh,c}`・`make-clean-forms.py`（新）。回帰に WS175 の `plan/ws175/tests/run-host-clean.sh`。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-clean-forms.sh` → PASS（plain と ASan/UBSan、19 の ok）: Fm2・P1 が借りて描く ImB・ImP、Fm1 の ImIn、Type 3 の ImG が残り、ImC・ImQ・ImOut・ImH は bytes も名も無い。名の木の 2 つの葉が残り、1 つ目の /Limits は (a.txt) (a.txt)、2 つ目は (z.txt) (z.txt)。/AF は 1 つ、kei-notes.bin の文字は無い。`qpdf --check` OK、`pdftoppm` で元と同じ絵。
- `sh plan/ws175/tests/run-host-clean.sh build/p1-clean` → PASS（plain・ASan・UBSan、29 の ok、qpdf・pdftotext・pdftoppm）。
- build（warning 0）: `make -j16 BUILD=build/p1-k ZEDBSD_CONFIG=plan/ws089/tests/config-amd64-settings.mk build/p1-k/bin/notes build/p1-k/dynamic/libpdf.so build/p1-k/dynamic/libkeiland.so build/p1-k/bin/settings`。style-check: 変えた file は 0 件。

## 未実施

- Notes の案内と Open Clean Copy の画面（QEMU・UAT）: AAT に System Menu を操る口が無いので、自動の試験は作っていない。Linux・FreeBSD の build。

## Event

2026-10-08 / q887-i01（P1）: 実装と host・build の確認。
