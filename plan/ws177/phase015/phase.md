<!-- awesome-plan project=zedbsd record=ws177-p015 -->

# ws177-p015: Mail の app の基本の操作（案 N2）

Parent: [WS177](../ws.md)
Status: test-wait（2026-10-08 P1 q889 の 2: 実装・host PASS・zedBSD の build warning 0。QEMU は T1 の AAT の回帰、新しい UI は UAT）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q889 の 2（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 71・80・82・83・84（WS169 ws169-p003・p004）、[案](../phasing-20261008.md) の N2

## 設計と変更（2026-10-08 P1）

- **TLS の失敗と理由（71）**: `tls.c` は handshake を verify の結果に関わらず最後まで進め（verify の callback は 1 を返す）、何も送る前に `tls_decide` で決める: 検証できない証明書は `ML_ERROR_UNTRUSTED`（新しい定数、`mail.h`）で、SHA-256 の指紋を `ml_tls_fingerprint` で返す。server の `pin`（利用者が信頼した指紋、`struct ml_server`）と同じ指紋なら受け入れる。STARTTLS を断る IMAP の server、STARTTLS を出さない SMTP の server は `ML_ERROR_NO_TLS`（password を平文で送らない）。接続は 15 秒で `ETIMEDOUT`（`conn.c` の nonblocking の connect）。server の BYE の言葉を失敗の言葉にする。古い TLS の理由を別の失敗に付けない。`sync.c` の失敗は「Cannot sign in to imap.example.net: the server did not answer in time.」の形で、何を・どの server で・なぜ（server・OpenSSL の言葉、無ければ errno の意味）を言う。
- **自己署名を許すか聞く（71）**: 窓は `ML_ERROR_UNTRUSTED` の失敗で、server の名と指紋（2 桁ずつ `:` で区切る）を出して「Trust the certificate of HOST?」と聞く（libkeiland の `kl_dialog`、窓の上に。聞いている間は入力を dialog の `kl_ui` に回す）。Trust: form の account（新規・編集）は pin を付けて試し直す。既存の account は pin を account に付けて保存し（`mailer.conf` の `imap_pin`・`smtp_pin`）、thread を作り直す（SMTP なら「Send the message again」）。Cancel: その指紋は同じ実行の間は聞き直さない。
- **Trash の Delete・Gmail の Sent（80）**: Trash の中の Delete は完全な削除（`ML_JOB_DELETE`、`ml_imap_delete` = `\Deleted` と、UIDPLUS があれば `UID EXPUNGE`、無ければ `EXPUNGE`）。login の後に CAPABILITY を読む（MOVE・UIDPLUS・X-GM-EXT-1）。Gmail（X-GM-EXT-1）は送った物を自分で Sent に置くので APPEND しない（Sent を取り直すだけ）。Outlook は確かめられないので今まで通り APPEND する。
- **account の編集・削除・5 個目（82）**: File の menu に「Edit Account」。form に表示中の account を入れ、「Save」で新しい設定を別の session で試し（`ML_JOB_CHECK`、取り込まない）、通れば置き換え、その account の message を捨てて thread を作り直す（取り直す）。server の host・port が同じなら信頼した pin を保つ。address を変えたら古い address の password を忘れる（`ml_secret_save` に空の password = 忘れる、secret.c の 2 関数のまま）。「Remove Account」（危険の色）は確かめの dialog の後に account・その message・password を消し、thread を作り直す。account の上限を 4 から 16 に（`ML_ACCOUNTS_MAX`）、sidebar の account の欄は収まらなければ scroll する。
- **日付の語（83）**: `ml_store_redate` が日が変わった時に全ての短い日付を書き直す（main の loop が毎回呼ぶ、loop は最長 1 秒で回る）。
- **512 通の上限（84）**: 一覧の message の index を view が持つ伸びる配列に（`view->shown`、`ML_MESSAGES_MAX` を廃止）、並べ替えは qsort。
- 変更: `userland/desktop/mailer/{mail.h,mailer.h,sync.h,tls.c,conn.c,imap.c,smtp.c,sync.c,store.c,secret.c,account.c,view.c,main.c}`。試験: `plan/ws177/tests/host-mail-n2.{c,sh}`・`host-mail-view.c`（新）。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-mail-n2.sh` → host-mail-n2 PASS 16（CA を渡さない偽の server で IMAP・SMTP の untrusted と 64 桁の指紋、違う pin は untrusted、正しい pin で login・送信、CAPABILITY、Trash の削除で INBOX が 3→2、STARTTLS を断る IMAP と出さない SMTP の `ML_ERROR_NO_TLS`、日付が「Yesterday」になる・同じ日は書き直さない、account の変更で message を捨てる・削除で後ろの account の番号が詰まる、pin の保存と読み戻し、password を忘れる）、host-mail-view PASS 10（600 通の folder で Up・Down が最新と最古に届く、Edit Account の form、Remove の Esc で残る・Enter で index 付きの request、Add Account の form が空、証明書の問いの Esc と Enter、16 個の account の sidebar が窓より高い）。ASan・UBSan。
- 回帰: `plan/ws169/tests/run-host-mail-backend.sh`（host-mail-backend・host-mail-sync PASS）、`run-host-mailer.sh`（18 PASS）。
- build（warning 0）: `make -j16 BUILD=build/p1-mailer ZEDBSD_CONFIG=plan/ws169/tests/config-amd64-mailer.mk build/p1-mailer/bin/mailer`。style-check（mailer の全 .c）0。`git diff --check` 0。

## 未実施・制限

- QEMU（T1）: AAT `apps.mailer.read-compose`・`apps.mailer.sign-in-code` の回帰（main.c の入力の回し方を変えた）。Edit Account・Remove・証明書の dialog・Trash の削除・16 個の account の sidebar は QEMU・画面で見ていない（host の試験だけ、UAT）。
- thread の作り直し（edit・remove・trust）は古い thread の今の仕事の終わりを待つ（最長で接続 15 秒・読み 30 秒、窓が止まる）。**残り（2026-10-08 Q1）: N の後で直す候補**（作り直しを thread の中の job にする、または古い thread を待たずに切り離す）。作り直すと各 folder の最新 50 通を取り直す（store は重複を除く）。
- Outlook が送った物を Sent に自分で置くかは確かめていない（APPEND する）。

## Event

2026-10-08 / q889-i02（P1）: 実装と host・build の確認。
