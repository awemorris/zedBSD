<!-- awesome-plan project=zedbsd record=ws189-p004 -->
# ws189-p004: Mail の作成の添付（multipart/mixed・base64、添付の一覧の UI）と drop の受け

Status: cleared（2026-10-08 Q1: T1-441 PASS 1〜6、eml は multipart/mixed で SHA-256 一致。差: Photos からの drop は uri-list（sample.png、type=2）でシナリオの image.png・type=4 と違う → シナリオの期待を直す（P1））
Disposition: normal
Parent: [WS189](../ws.md)
Queue: q892（P1）
Design: [p001](../phase001/phase.md) §4.8。由来: 2026-10-08 ユーザーの決定「WS189 で添付も作る」（Q1 経由）

## 設計

- `mail.h`: `struct ml_attachment`（名前・MIME の型・bytes・長さ）、`ML_ATTACH_MAX` = 16、`ML_ATTACH_TOTAL_MAX` = 25 MiB（送る添付の合計）。
  `ml_compose_with(..., attachments, count, ...)`、`ml_compose` は count 0 の形（今と同じ単一の text の本文、ws169 の試験は変えない）。
- `compose.c`: 添付がある時は `Content-Type: multipart/mixed; boundary="=_keiland_<時刻>_<pid>_<連番>"`、本文の part（今の text/plain の quoted-printable）、各添付の part
  （`Content-Type: <型>; name=...`、`Content-Disposition: attachment; filename=...`、`Content-Transfer-Encoding: base64`、76 桁の行）、終わりの boundary。
  名前は ASCII の印字できる文字（`"` と `\` を除く）なら quoted string、ほかは RFC 2231 の `filename*=UTF-8''%XX…`（`name*` も同じ）。
- 作成の pane（`view.c`・`mailer.h`）: 下の行（今の「Attachments and drafts are not kept yet.」）を添付の行に: 「Attach…」の button と、添付ごとの chip（名前・大きさ・外す ×）。
  入りきらない chip は「+N」。drag が作成の pane の上にある間は添付の行を `kl_drop_frame` で光らせる。
- `main.c`: 「Attach…」は `kl_file_chooser`（開く）で file を読み込む（1 つ 25 MiB まで、合計も）。窓は `KL_DROP_URIS | KL_DROP_IMAGE` を受け、作成中で問いの無い時だけ
  copy と答える（ほかは 0）。drop: file 名は `file://` を %XX から戻して各 file を読む、画像は `image.png`（在れば `image 2.png`）。送った時と Cancel で添付を空にする。
  型は拡張子から（png・jpg・jpeg・gif・pdf・txt、ほかは application/octet-stream）。
- 試験: host `plan/ws189/tests/host-mail-attach.c`（compose.c と mime.c を host で: 添付 2 つの message を書き、Mail 自身の `ml_mime_parse` で本文と添付の名前・大きさを読み戻す、
  base64 を自前で読み戻して bytes が一致、日本語の名前の RFC 2231）。AAT `apps.mailer.attach-drop`（偽の server、tests/scenarios/apps/mailer/）。

## 記録

### 実装（2026-10-08、P1、q896 の前に区切り）

- `mailer/mail.h`・`compose.c`: `struct ml_attachment`・`ML_ATTACH_MAX`（16）・`ML_ATTACH_TOTAL_MAX`（25 MiB）、`ml_compose_with`（添付があれば multipart/mixed、各添付は
  base64 の 76 桁、名前は quoted string か RFC 2231 の `filename*=UTF-8''`）。`ml_compose` は添付 0 の形。
- `mailer/mailer.h`・`view.c`: 添付の配列と `ml_view_attach`・`_remove`・`_clear`（New・Reply・Forward の始まり、送れた時に空にする）、作成の pane の下の行に Attach... と
  chip（名前と KB、外す x、入りきらない分は +N）、drop の間は行を `kl_drop_frame` で光らせる。
- `mailer/main.c`: Attach... の `kl_file_chooser`、file を読んで添付（25 MiB まで）、窓は `KL_DROP_URIS | KL_DROP_IMAGE` を受け、作成中（form・問いの無い時）だけ copy と答える。
  drop: file 名（`file://`、%XX を戻す）の各 file、画像は `image.png`（重なれば `image 2.png`）。送る時は `ml_compose_with`。log `ATTACH add|remove|asked|chooser`・`DND drop`。
- 試験: `plan/ws189/tests/host-mail-attach.c`・`run-host-mail-attach.sh`（ASan・UBSan）。他の WS の host 試験の source の一覧に新しい file を足した（機能に追従）:
  `plan/ws169/tests/run-host-mailer.sh`（libkeiland の drop-look.c）、`plan/ws175/tests/run-host-notes-edit.sh`（picture/png-write.c）。

### 確認（2026-10-08、P1）

- `make ... BUILD=build/ws189 build/ws189/bin/mailer`: rc 0、warning 0。
- `sh plan/ws189/tests/run-host-mail-attach.sh`: ok（multipart、ASCII と日本語の名前、base64 の bytes の一致、Mail 自身の `ml_mime_parse` で本文と 1 つ目の添付の名前、添付 0 は単一の part、17 個は EINVAL）。
- 回帰の host 試験（変えた file を compile する物）: `run-host-mailer.sh` PASS、`run-host-notes-edit.sh` PASS、`run-pdfviewer-host.sh` ok、`run-host-pdfviewer-find.sh` PASS。
- style-check（変えた file）: 新しい指摘 0。
- 未実施（2026-10-08 の時点）: AAT `apps.mailer.attach-drop` のシナリオ（未作成）、QEMU（T1）、自己レビュー。

### 再開（2026-10-08 夕、q896 と ws189 F2〜F6 の後）

- 自己レビュー: drop の data は libkeiland が NUL で終える（`kl_window_receive_drop`）ので uri-list の行の走査は安全。`file://`（host 無しか localhost）だけを
  path にし、%XX を戻し、`\r\n` を落とす。作成中でない・問いのある時は 0 と答え、drop は finish 0。名前は 255 bytes までなので RFC 2231 の行（%XX で 3 倍）も
  998 文字に収まる。添付の file は 25 MiB を越えると読まない、合計と 16 個は `ml_view_attach` が断る。直すべき点は見つからなかった。
- AAT: `tests/scenarios/apps/mailer/attach-drop.md`（`apps.mailer.attach-drop`、active）: Attach... と chooser、Files からの file の drop、Photos からの画像の
  drop、chip の x、送った message の multipart/mixed と base64 の中身（SHA-256）、作成していない時の refused。
- 確認: `check-scenarios.py` PASS。build と host 試験は 2026-10-08 の記録のまま（その後の mailer の変更は無い）。

### シナリオの直し（2026-10-08、P1、T1-441 の差）

- T1-441 の手順 3: Photos からの drop は `MAIL DND drop type=2`（file 名、`name=sample.png`）で、シナリオの期待 `type=4`・`image.png` と違った。**期待が誤り**: ws189 の設計 p001 §2.1 は「Photos の drag（uri-list と png）は Files・desktop・Mail には file として、Notes には画像として届く」（受ける型の順は file 名が先）。Photos は元の file を text/uri-list でも出すので、Mail は元の file を名前のまま添付する（`image.png` は image/png だけの drag、例えば Notes・PDF Viewer・Browser からの時）。
- `tests/scenarios/apps/mailer/attach-drop.md` の手順 3（`name=sample.png`・`type=2`・chip「sample.png」）と手順 5（`filename="sample.png"`、Photos の試料と SHA-256 が一致）を直した。`check-scenarios.py` PASS。実装は変えない。
