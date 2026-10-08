<!-- awesome-plan project=zedbsd record=ws177-p016 -->

# ws177-p016: Mail の IMAP・SMTP の互換（案 N）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc §2））（旧: test-wait → Q1 の判定待ち（2026-10-08 q902 P1 の照合: T1-422 PASS（apps.mailer.read-compose・sign-in-code が fail なし、PNG は Q1 の目視））（旧: test-wait（2026-10-08 P1 q889 の 3: 実装・host PASS・zedBSD の build warning 0。QEMU は T1 の AAT の回帰）））
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q889 の 3（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 69・70・72・73・74・75（WS169 ws169-p003）、[案](../phasing-20261008.md) の N。76（code の語の境）は [ws177-p014](../phase014/phase.md) で前倒しして済み

## 設計と変更（2026-10-08 P1）

- **日本語の文字集合（69）**: `mailer/jis.c`（新）が ISO-2022-JP（ESC `$@`・`$B`・`(B`・`(J`・`(I`、SO・SI は無視）、Shift_JIS（Windows-31J の名も、JIS X 0208 の範囲）、EUC-JP（SS2 の半角カナ、SS3 の JIS X 0212 は U+FFFD）を 1 文字ずつ code point にする。表は NoctLang の `jisx0208.h` を `mailer/jisx0208.h` に写した（同じ作者、Zlib。NoctLang の tree は toolchain なので include せず複写した）。`mime.c` の `mime_to_utf8` が本文と encoded word（件名・名前）に使う。文字でない byte と表に無い cell は U+FFFD。
- **1 MiB を超えるメール（70）**: fetch で `RFC822.SIZE` が `ML_FETCH_BYTES` を超えた物は、fetch の後で `ml_imap_fetch_large` が取り直す: `BODYSTRUCTURE`（`mailer/structure.c`（新）が読む: 最初の text/plain・text/html の section・charset・encoding、最初の file の名と復号後の大きさ）、`BODY.PEEK[HEADER]`、本文の section だけ（`BODY.PEEK[2]<0.N>` など）。`ml_mime_parse_large` が header と本文の part から message を作る。添付の大きさは base64 の行の CRLF を除いた見積もり（前は先頭 1 MiB からの見積もり）。structure に literal がある時は前の通り先頭 1 MiB を読む。1 回の fetch で 16 通まで（それ以上は先頭から読む）。
- **folder 名（72）**: LIST の名が literal（`{N}`）の時に読む。modified UTF-7（`&...-`、`,`）を UTF-8 に戻して usual name と比べる（日本語の名: 送信済み・送信済みメール・送信済みアイテム・下書き・アーカイブ・ごみ箱・ゴミ箱・削除済みアイテムを足した）。server には受け取った名のまま送る（sidebar は決まった名を出すので、名を画面には出さない）。`\Noselect` は前から除いていた。
- **UID MOVE・UIDPLUS（73）**: CAPABILITY（ws177-p015）に MOVE があれば `UID MOVE`、無ければ COPY・`\Deleted`・UIDPLUS があれば `UID EXPUNGE uid`（他の `\Deleted` の message を消さない）、無ければ `EXPUNGE`。
- **宛先の名の encoded word と折り返し（74）**: `compose.c` が To・Cc を項目ごとに分け、ASCII でない名を `=?UTF-8?B?...?= <addr>` にする。長い encoded の値は文字の境で 45 byte ずつの複数の word に分ける（前は 1 つの word で、長い件名は空になった）。To・Cc は項目の間の `,` で、ASCII の件名は空白で、78 文字の前に折り返す。
- **SMTP の AUTH LOGIN（75）**: EHLO の `AUTH` の行を読み、PLAIN が無く LOGIN だけなら `AUTH LOGIN`（334 に user と password を base64 で）。本文は quoted-printable（7 bit）なので 8BITMIME の無い server でもそのまま通る（変更なし、説明を source に書いた）。
- 変更: `userland/desktop/mailer/{mail.h,mime.c,imap.c,smtp.c,compose.c,sync.c,Makefile}`、新規 `jis.c`・`jisx0208.h`・`structure.c`。試験の道具: `plan/tools/mail/fake-mail-server.py` に任意の選択肢 `--caps`・`--large`・`--japanese-folders`・`--smtp-auth` と、FETCH の partial・`BODY.PEEK[section]`・`BODYSTRUCTURE`・`UID MOVE`・`UID EXPUNGE` を足した（選択肢の無い時の振る舞いは前と同じ。partial は小さい message では変わらない）。`plan/ws169/tests/run-host-mail-backend.sh` と `plan/ws177/tests/host-mail-n2.sh` の compile の一覧に jis.c・structure.c を足した。試験: `plan/ws177/tests/host-mail-n.{c,sh}`（新）。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-mail-n.sh` → PASS 19（ASan・UBSan）: ISO-2022-JP・Shift_JIS・EUC-JP の本文と半角カナ、ISO-2022-JP の encoded word の件名、文字でない byte の U+FFFD、BODYSTRUCTURE（file が先・入れ子の alternative・単独の part・literal は EPROTO）、日本語の名の encoded word・78 文字の折り返し・長い日本語の件名の読み戻し、偽の server で MOVE・UIDPLUS の capability、日本語の folder 名（1 つは literal）、4 通目の 2 MB のメールを structure で取る（件名・file の後の本文・file の大きさ）、UID MOVE、UID EXPUNGE が他の `\Deleted` を消さない、AUTH LOGIN。
- 回帰: `plan/ws169/tests/run-host-mail-backend.sh`（host-mail-backend・host-mail-sync PASS）、`run-host-mailer.sh`（18 PASS）、`plan/ws177/tests/host-mail-n2.sh`（n2 PASS 16・view PASS 10）、`host-mail-code.sh` PASS。
- build（warning 0）: `make -j16 BUILD=build/p1-mailer ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk build/p1-mailer/bin/mailer`。style-check（mailer の全 .c）0。`git diff --check` 0。
- license: `jisx0208.h` は NoctLang（Awe Morris、Zlib）の表の写し。他の新しい file は自作。

## 未実施・制限

- QEMU（T1）: AAT `apps.mailer.read-compose`・`apps.mailer.sign-in-code` の回帰（imap.c・mime.c・compose.c・smtp.c を変えた）。本物の Gmail・Outlook・日本の ISP の server では確かめていない。
- header の生の 8 bit（encoded word でない Shift_JIS の件名など）は bytes のまま。ISO-2022-JP-2 などの拡張の集合、JIS X 0212、Windows-31J の NEC・IBM の拡張文字は U+FFFD。
- 大きなメールの本文の section も `ML_FETCH_BYTES` で切る。
- folder の日本語の名を sidebar に出す機能は無い（sidebar は決まった 5 つ）。

## Event

2026-10-08 / q889-i03（P1）: 実装と host・build の確認。
