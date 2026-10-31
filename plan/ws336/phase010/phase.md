<!-- awesome-plan project=zedbsd record=ws336-p010 -->

# ws336-p010: ssh-ttymodes・ssh-channel（端末の mode と connection protocol）

Status: cleared（2026-11-01 06:19 Q1、main 290b99bab）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p002
WS335 の待ち: 無し

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-ttymodes.h`・`ssh-channel.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4254.txt`（SHA-256 `bfc727e9350067ea…`、全桁は同じ directory の SHA256SUMS）: §5（channel・window・data・EOF・CLOSE）、§6（session の request）、§8（端末の mode の opcode の表）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc8160.txt`（SHA-256 `65eefa2fbc0db6bb…`、全桁は同じ directory の SHA256SUMS）: §3（IUTF8 = 42）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-PROTOCOL`（SHA-256 `4a9e7f6d26d3f59d…`、全桁は同じ directory の SHA256SUMS）: §2.1（eow）・§2.2（no-more-sessions）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4250.txt`（SHA-256 `eb3a9d7c3e1fae53…`、全桁は同じ directory の SHA256SUMS）: §4.1.2・§4.3（channel の open の失敗の理由）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-ttymodes.c`（opcode の表は data として block ごとに出典）・`ssh-channel.c`
- `plan/ws336/tests/unit/test-ttymodes.c`・`test-channel.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- ttymodes: 既知の termios（cfmakeraw の後・既定）を put → apply で同じ flag と文字、opcode 160 で止まる、未知の 1〜159 を飛ばす、長さの途中で切れた引数で EINVAL かつ terminal が変わらない、1024 bytes を超えて EINVAL、速度 9600・38400・未知
- channel: open_session → accept_open → open_reply の往復、11 個目で RESOURCE_SHORTAGE、session 以外の型・no_more_sessions で ADMINISTRATIVELY_PROHIBITED、data の window の消費と window の超過・SSH_CHANNEL_PACKET の超過で EPROTO、consumed による WINDOW_ADJUST の量、adjust で 2^32 を越えて EPROTO、EOF・CLOSE の二重で EPROTO、EOF の後の data で EPROTO、closed で slot が戻る
- request の各型の parse（`wire.h` で組む）と put の往復、長さの上限 +1 で EINVAL、未知の request は SSH_CREQ_OTHER と name
- global_request: keepalive@openssh.com が want_reply で REQUEST_FAILURE、no-more-sessions が印を立てる、hostkeys-00@openssh.com の後に key blob の string が 2 つ続く payload を EINVAL にせず want_reply FALSE で何も返さない、名前 100 bytes を切って読む
- channel request: x11-req・auth-agent-req@openssh.com（余りの data 付き）・100 bytes の名前・上限 +1 の env の値・exec の command が SSH_CREQ_OTHER で 0 を返す
- CLOSING: 自分が CLOSE を送った後の相手の data・EOF・window-change を読める（data は window から引く）
- put_global_request の bytes
- 乱数の入力

## 4. 注意

- ttymodes の表の flag や文字が `<termios.h>` に無い時は `#ifdef` で飛ばす（zedBSD と Linux の host の両方で build する）。表の値は RFC 4254 §8 と RFC 8160 から。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh ttymodes channel`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

実施: 2026-11-01（host の `date`）。worktree `s-ws336-p010`、基点 main（p002 の merge 後を `git merge main` で取り込み）。実装済み・試験 PASS（cleared は Q1 が決める）。

### 作った物

- `userland/base/ssh/ssh-ttymodes.c`（opcode の表と速度の表は data。block ごとに「Values referenced:」）
- `userland/base/ssh/ssh-channel.c`（channel の表、window、open・data・EOF・CLOSE、channel request、global request。request 名の表も data）
- `plan/ws336/tests/unit/test-ttymodes.c`（20682 checks）、`plan/ws336/tests/unit/test-channel.c`（1603651 checks。うち約 160 万は乱数の payload）

### 実行した command と最後の行

- `sh plan/ws336/tests/run-unit.sh ttymodes channel`: `test-channel: PASS (1603651 checks)`、`test-ttymodes: PASS (20682 checks)`、`run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all`: `test-buf: PASS (323 checks)`、`test-channel: PASS (1603651 checks)`、`test-common: PASS (2316 checks)`、`test-tools: PASS (13 checks)`、`test-ttymodes: PASS (20682 checks)`、`run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh`: `check-skeleton: PASS`
- `git diff --check`: 出力なし（成功）
- run-unit.sh の設定（`-std=c89 -O1 -Wall -Wextra -Werror`、ASan・UBSan、leak 検出あり）。cc と clang の警告 0 は check-skeleton の中で確認。

### 試験の期待値の出所

opcode の番号は RFC 4254 §8 の表と RFC 8160 §3（IUTF8 = 42）。message の bytes は RFC 4254 §4・§5・§6・§9 と OpenSSH の PROTOCOL §2.1・§2.2 から `wire.h` で組み、module の出力と比べた。window の値は注釈の手計算（2097152 - 6 など、半分 1048576 の境界、2^32 - 1 の境界）。ttymodes は読み返しに試験の中の別の解析器を使い、flag と文字の値は host の `<termios.h>` の名前。他の実装の出力は使っていない。

### 壊して確かめた物（git の外、元に戻して `cmp` で一致を確認）

31 通り（channel 21、ttymodes 10）: window の比較（`>` を `>=`）、2^32 - 1 の比較、半分の境界、EOF 後の data の確認を外す、confirmation の欄の入れ替え、packet 0 の確認を外す、closed の close_received の確認を外す、send_room の min を max、名前の切り詰め、文字列の上限 +1、CLOSE の二重の確認を外す、no-more-sessions の印、window-change の列と行の入れ替え、exec の上限、data の packet の上限、open failure で slot を戻さない、send と receive の window の引き算を外す、extended の type 0、no_more_sessions の判定、recipient の範囲の確認を外す（`% SSH_CHANNEL_MAX`）、opcode 160 の境界、引数の切れを無視、1024 の境界、IUTF8 の番号、255 の判定、速度の表の値、apply の結果を書き戻さない、flag の反転、表の先頭の行を飛ばす。
見逃し 2 件: (1) recipient の範囲の確認を外す変異（`recipient % SSH_CHANNEL_MAX`）は、待ち状態の slot に折り返す入力が無く通った。試験を足し（待ち状態の slot 2 に対し recipient 12）、FAIL することを確かめた。(2) 速度で `cfsetispeed` と `cfsetospeed` を入れ替える変異は host で通る。host の glibc は入力速度と出力速度を同じ `c_cflag` の欄に持つので、2 つを別の値にできず区別できない（試験では入力と出力を個別に設定して確認しているが、この host では差が出ない）。別の速度を持てる系（zedBSD の libc）では効く。
最終の実装は変異の script が `cmp` で元と一致することを確認済み。

### 計画（phase.md の「試験の項目」）と header の食い違い

- 試験の項目の「長さの上限 +1 で EINVAL」は、header の説明（上限を超える文字列を持つ既知の request は SSH_CREQ_OTHER で 0 を返し、EINVAL は枠の切れだけ）と食い違う。header に従い、上限 +1 は SSH_CREQ_OTHER で試験した（同じ項目の後ろの「上限 +1 の env の値 ... SSH_CREQ_OTHER」とは整合する）。
- `ssh_channel_global_request` の header は先頭で「no-more-sessions は honour する」と言い、関数の注釈は「every global request is refused」で want_reply なら REQUEST_FAILURE と言う。honour した要求に FAILURE を返すと矛盾するので、**want_reply が TRUE の no-more-sessions だけ REQUEST_SUCCESS を返し、他は REQUEST_FAILURE** にした（OpenSSH は want_reply FALSE で送るので通常は返事が要らない）。Q1 の判断が要る点。

### header が黙っていて自分で決めた点

ttymodes:
1. 表に無い（local の `<termios.h>` に名前が無い）opcode は `#ifdef` で表から外れ、`put` は書かず、`apply` は未知として引数ごと飛ばす。
2. `put` の文字: c_cc の値が `_POSIX_VDISABLE`（無ければ 255）なら 255 を送る。`apply`: 引数の下位 8 bit が 255 なら `_POSIX_VDISABLE`、それ以外はその値。
3. CS7（90）・CS8（91）は CSIZE の欄の値。`put` は欄がちょうどその値なら 1。`apply` は 0 以外でその欄を設定し、0 は何もしない（もう片方の opcode か既定が決める）。
4. 速度: 表は B50 から B4000000 の標準の速度（B0 は含めない、B134 は 134.5 baud で bps に直せないので除く）。`put` は表に無い速度と B0 を書かない。`apply` は bps が表に無い値と 0 を無視する。`cfset*speed` の失敗も無視する（速度は変えない）。
5. `apply` は terminal の複写の上で作業し、最後まで読めた時だけ書き戻す。size が 1024 を超える・NULL で size > 0・引数が途中で切れる場合は EINVAL で terminal は不変。opcode 0 と 160 以上はそこで読みを止める（残りは読まない）。
6. `put` が使う buffer の limit に達した時は buffer の誤り（EMSGSIZE）に任せる。

channel:
7. payload は message 番号を含む（他の module の約束に合わせる）。番号が違えば EINVAL。枠の確認（EINVAL）を先に行い、状態の確認（EPROTO）はその後。誤りの時は何も変えない。
8. `ssh_channel_find` は OPEN と CLOSING を返す。`recipient` が表の外なら NULL。
9. 相手が告げた最大の packet を 32768（SSH_CHANNEL_PACKET）に切り詰めて `remote_packet` に保存する（accept_open と open_reply の両方）。window はそのまま。confirmation の packet が 0 は EPROTO。
10. open の枠の後ろの型固有の data は読まない（session でも）。OPEN_FAILURE の説明は理由ごとの固定の文（"administratively prohibited"、"resource shortage"）で、language tag は空。理由は session 以外の型・no_more_sessions が 1、slot 無しが 4。session の packet 0 は型が session の時に EPROTO（no_more_sessions より先）。
11. 受信の data: 拡張 data の type 0 は EPROTO（呼び出し側が 0 を「普通の data」の印にするため）。EOF 受信後・CLOSE 受信後の data は EPROTO。window を引くのは全部の確認の後。
12. `ssh_channel_consumed`: size 0、状態が OPEN でない、window が半分（1048576）以上、output が既に誤りの時は何もしない。window が半分未満なら `SSH_CHANNEL_WINDOW - local_window` を送って window を満杯に戻す。output に書けなければ window を変えない。
13. `ssh_channel_put_data`: EPIPE は EOF か CLOSE を送った後。OPEN でない channel は EINVAL。size 0 は room 内なら空の message を書く。output が書けなければ remote_window を引かない。data が NULL で size > 0 は EINVAL。
14. `put_eof`・`put_close` は OPEN と CLOSING の channel だけに書く（OPENING・FREE は何もしない）。CLOSE の後の EOF も書かない。書けなかった時は印を立てない。`parse_end` で CLOSE を受けると状態は CLOSING になる。`ssh_channel_closed` は表の外の address と FREE を 0 で返し、`user` に触れない。slot を取る時に `user` を NULL にする。
15. request: 既知の型の枠の後ろの余りの bytes は読まない。文字列が上限を超える、または NUL を含む（term・env の名前と値・subsystem・signal・exit-signal の名前）場合は SSH_CREQ_OTHER とし、name に request 名を入れる。request 名・global request の名前は 64 文字に切り、0x21〜0x7e でない byte を `?` に置き換える（log に出せる一行にするため）。比較は相手が送った名前の全体で行う（NUL で切れた名前は一致しない）。pty の modes は長さを制限しない（ttymodes の apply が 1024 で EINVAL にする）。
16. `put_*` の request は OPEN か CLOSING の channel にだけ書ける。OPENING・FREE の channel では out に EINVAL を記録する（呼び出し側の誤り）。`put_reply` も同じ。長さの上限は put 側で確かめない（相手が OTHER として拒否する）。
17. `ssh_channel_global_request`: 名前の切り詰めは上記。keepalive の want_reply TRUE は REQUEST_FAILURE、hostkeys-00@openssh.com は後ろの key blob を読まず、want_reply FALSE なら何も返さない。output が既に誤りなら、その誤りを返す。

### 未実施

- 実機・QEMU・host の OpenSSH との相互運用（この Phase の範囲外。p015 以降）。
- clang での sanitizer 付きの単体試験（run-unit.sh は cc のみ。clang は check-skeleton の警告 0 だけ）。
- zedBSD の libc の `<termios.h>` に対する ttymodes の表の確認（zedBSD の build は check-skeleton の 7 番目の確認で compile のみ。結果は上の `check-skeleton: PASS`）。
