<!-- awesome-plan project=zedbsd record=ws157-p005 -->

# ws157-p005: Photos の app（取り込み・album・縮小画像の cache）

Status: cleared（2026-10-07 Q1 の判定: T1-331 で photos.browse の fail が消え（2026-08.tsv の sea.jpg の行にお気に入りと回転）、timeline の PNG を Q1 が目視）（旧: in-progress（2026-10-07 q835 P2: 実装と host の試験 PASS、zedBSD の build warning 0。QEMU は T1 に依頼、判定は Q1））
Disposition: normal
Parent: [WS157](../ws.md)
Queue: q835（2026-10-07、P2）
依存: [p004](../phase004/phase.md)

## 実装（2026-10-07 P2）

- `main.c`: 起動で `~/Pictures/Library` の db を読む。`photos --import=PATH`（窓の前に取り込み、db を書く。AAT はこれを使う）。File > Import Photo...（Ctrl+I）・
  Import Folder...（libkeiland の file chooser で写真を選ぶ。Folder はその写真の folder）→ 取り込み・db・view を作り直し「Imported N photos (M already in the library).」。
  Refresh は db を読み直す。印・album の変更は `ph_db_save`。`PHOTOS IMPORT done …`・`LIBRARY root=…`・`CHOOSER …`・`SAVE` の log。
- `view.c`: grid の見出しに Import（folder の取り込み）。Photo > Add to Album...（と A）で album の card（album の一覧の button、新しい album の名前の欄と New Album・Enter、
  Cancel・Esc）。album を作ると一覧の album の位置を保つ。空の library の文。
- `thumbs.c`: 縮小画像を `$XDG_CACHE_HOME/keiland/photos/<id>.ppm`（無ければ `~/.cache/...`。P6、160 の正方形、回転は掛けずに）に持ち、次からはそこから読む。
- AAT: `plan/tools/aat/scenarios/helpers_photos.py` と `tests/scenarios/apps/photos/browse.md` を取り込みの流れに書き直し（`--import`、db の行、album の file、2 度目は重複 9、cache 8）。

## 確認（2026-10-07）

- host: `sh plan/ws157/tests/run-host-photos.sh` → PASS 32（取り込んだ library と album 2 つで、p003 の grid・全面・回転・お気に入り・slideshow の確認に加え、
  album の card（New Album で Waves を作り写真を足す、save の印、card が閉じる）、Import の依頼、新しい view が縮小画像を cache から読む（8 枚））。PNG: `build/review/ws157/host-photos-*.png`（card を含む）。
- `sh plan/ws157/tests/run-host-photos-db.sh` → PASS。
- zedBSD: `make ZEDBSD_CONFIG=plan/ws157/tests/config-amd64-photos.mk BUILD=build/ws157-zed build/ws157-zed/bin/photos` warning 0。style-check 0。check-scenarios PASS。
- QEMU: 未実施（T1 に依頼: `--only 'apps\.photos\.'`）。file chooser からの取り込みは AAT では流さない（`--import` で代わり）。

## 積み残し

[WS177 backlog-p2](../../ws177/backlog-p2.md) の WS157 の行（2026-10-07 の作り直しで更新）。

## 2026-10-07 T1-324 の直し（P2）

`apps.photos.browse` の fail（2026-08.tsv の sea.jpg の行に favourite と turn が無い）: `view_turn`・`view_favorite` が `view->save` を立てるが写真の
`changed` を立てず、`ph_db_save` は changed の写真の月しか書かないので、SAVE error=0 で何も書かれていなかった（helper の待ちの問題ではない）。
直し: 両方で `photos[photo].changed = 1`。host の試験（`run-host-photos.sh`）に、取り込みの後に保存して changed を消し、R・F の後に changed を確かめる
check を足した（直しを外すと `FAIL save`、直しで 33 PASS）。zedBSD の build（`build/p2-ci/bin/photos`）warning 0。再試験は T1。

## q872（2026-10-08 P2）: T1-314 の apps.photos.browse の「no window mapped within 20 s」の切り分け

- **原因は試験の image が古かったこと（code の不具合ではない）**。T1-314 の log（`t1/build/t1-305/logs/apps.photos.browse.log`、T1-305 と同じ起動）に `usage: photos [--width=N] [--height=N] [--timeout-s=N] [FILE]` がある。これは p004・p005 の前の Photos の使い方で、`--import` を知らない。だから使い方を出して窓を作らずに終わり、IMPORT の行も出なかった。今の Photos の使い方は `[--import=PATH]`（main.c）。
  - T1-314 の image（`build/aat-t1202`）の photos は、T1 の記録の tree（main b4746472）より前に build された物だった見込み。
- その後の T1-324（main 1bda8a9f）は同じ scenario で import と窓まで進み、step 5（db の行）で fail した。T1-331（main f3d55d1c）で fail が消えた（needs-person）。p005 は 2026-10-07 に cleared。
- host: `plan/ws157/tests/run-host-photos.sh` は 2026-10-08 の main で PASS（import-saved・albums・thumb-broken・columns・favorites・broken-whole・import-request）。9 枚の import は host で 1 秒もかからない。
- helper の窓の待ちは、その後 40 s に延びている（5fda97c5a）。
- 直す code は無い。**再発の防止の案**: T1 は AAT の image を作る時に、使う tree で全ての package を build し直す（`build/aat-t1202` を使い回すと古い app が残る）。依頼の時、image の tree の SHA と、`/bin/photos --help` のような使い方の行を確かめる。

## 2026-10-10 後続設計

ユーザーのメディア管理指示でDB取得/追加/metadata更新は後続[p006](../phase006/phase.md)のCLIとcompositor APIへ移す。本Phaseの過去のcleared結果は保存し、変更後の検証を[p007](../phase007/phase.md)へ依存させる。Photosの再起動/監視更新とPhone利用は新Phaseが検証する。
