<!-- awesome-plan project=zedbsd record=ws336-p009 -->

# ws336-p009: ssh-auth（service request と userauth の message）

Status: 実装済み・試験 PASS（2026-11-02 11:10 Sonnet、cleared は Q1 が決める）。前: in-progress（2026-11-02 10:52）、planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p005
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-auth.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4252.txt`（SHA-256 `a36b882cf77b1073…`、全桁は同じ directory の SHA256SUMS）: §5（request の形）、§5.1（FAILURE）、§5.4（BANNER）、§7（publickey、署名される data、PK_OK）、§8（password と変更の形）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4253.txt`（SHA-256 `b471173305154f6c…`、全桁は同じ directory の SHA256SUMS）: §10（service request）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-auth.c`
- `plan/ws336/tests/unit/test-auth.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- 各 put を `wire.h` で組んだ期待の bytes と比べる
- parse_request: none・publickey（署名なし・あり）・password・password の変更の形・未知の method、`ssh-rsa` の algorithm で supported 0、blob の型と algorithm の違い、user 65 文字・NUL、password 257 bytes で EINVAL
- signed_size と put_signed_data: client が put_publickey_signed で作った request を server が parse し、signed data を作って `ssh_key_verify` で確かめが通る。session_id の 1 bit・user の変更で通らない
- failure・pk_ok・banner の parse（制御文字と不正な UTF-8 が `?` になる、ESC・CR・0x7f・0xc0 0x80）
- 乱数の入力

## 4. 注意

- password を持つ構造体と buffer は使い終わりに消す。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh auth`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 11:10（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。worktree `s-ws336-p009`、基点 main 084f264b5。header は変えていない。

### 作った物

- `userland/base/ssh/ssh-auth.c`（骨格の置き換え。全関数）
- `plan/ws336/tests/unit/test-auth.c`（2458 checks）。期待の値は `wire.h` で組んだ bytes（RFC 4253 10、RFC 4252 5・5.1・5.4・7・8 の形）、RFC 8032 7.1 の TEST 1（鍵）・TEST 2（公開鍵）、RFC 3629 4 の UTF-8 の範囲から選んだ手の例だけ。他の実装の出力は使っていない。parser は、message と丁度同じ大きさの heap の写しから読ませる（ASan が範囲外の読みを見る）。

### 試験の中身

put の全関数（service request・accept・none・publickey の問い合わせ・password・failure・success・pk_ok）を `wire.h` の bytes と比較。署名つきの request は、head が wire の bytes と一致、署名 blob が 83 bytes、server の parse の結果（signed_size・signature の位置・key_blob の位置）、`ssh_auth_put_signed_data` の出力が「session id の string + head」の wire の bytes と一致、`ssh_key_verify` が通る、session id の 256 bit のどの 1 bit でも通らない、user の変更で通らない、signed の head の全 bit（head_size x 8）の反転で通らない、既に前置の bytes がある out への追加、public-only の key・NULL の引数は EINVAL、入らない out は EMSGSIZE。parse_request は none・publickey（署名なし・あり、bool 2 は TRUE）・password・password の変更の形・未知の method（後ろの bytes を読まない）・method 名の大文字小文字・NUL・前後の違い、`ssh-rsa`・長い名前・型の違う blob・31/33 bytes の鍵・空の blob は supported 0、user 64/65 文字・NUL、service 64/65、algorithm 65・NUL、password 256/257・NUL、新しい password の同じ限界、全ての strict prefix は EINVAL で request が全て 0。failure・pk_ok・banner・service の parse（全 prefix、末尾の余り、capacity の境界、EMSGSIZE・EPROTO、制御文字と不正な UTF-8 が `?`: ESC・CR・DEL・NUL・0xc0 0x80・過長・surrogate・U+10FFFF 超・途中で切れた並び・C1 制御、U+00A0・U+07FF・U+FFFF・U+10FFFF は保つ、capacity と SSH_BANNER_MAX での切り（文字の境界）。乱数: request の template 6 種を 1〜3 byte 変えて切る 10 万回（成功の時は user・service・algorithm の長さ、pointer の範囲、signed_size + 4 + signature_size = size、supported なら algorithm が ssh-ed25519。失敗の時は request が全 0）、乱数の bytes を 5 つの parser に 10 万回、乱数の banner の出力に C0（LF・TAB 以外）・DEL・0xc0・0xc1・0xf5 以上が無いこと 1 万回。

### 実行した command と最後の行

- `sh plan/ws336/tests/run-unit.sh auth` → `test-auth: PASS (2458 checks)` / `run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all` → buf 323・channel 1603651・cipher 1064・common 2316・crypto 1656・hostfile 736314・kex 402989・key 2771・packet 369168・privsep 947・sftp 1836・tools 13・ttymodes 20682・auth 2458 の全部 PASS、最後の行 `run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（ws.md の基点のまま通った。cc と clang の警告 0 を含む）
- `git diff --check` → 出力なし（rc 0）

### わざと壊す確かめ

`build/tmp/p009-mut/mut.py`（worktree の file は書き換えず、変異の写しを別 directory に作って test-auth を build・実行。同時 4、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s、150 s と RSS 6 GB で kill）で 94 通り: message 番号 5 と 6、request の許す番号の片方を外す、EMSGSIZE を EINVAL にする、finish を error の確認にする、名前の capacity を +1、method 名 3 つの 1 文字変更、change の判定の反転、password の後の余りの確認を外す、OTHER で余りを読む、bool を u8 にする（2 つ）、signed data の session id の長さと本体の長さ、failure の partial、success の番号、pk_ok の algorithm の長さ、query・signed の has_signature、signed の `signed_size`・`out->data + start`・private の確認、head の service、password の flag、failure の番号・capacity の比較・partial・flag、pk_ok の algorithm の確認・ENOTSUP の扱い・ct_equal の長さ、banner の番号・limit・LF と TAB の例外・0x20 と 0x7f の境界・C1 の範囲の両端・C1 の lead・切りの `>` と `>=`・SSH_BANNER_MAX、UTF-8 の表の全ての範囲（C2・DF・E0 の A0・ED の 9F・F0 の 90・F4 の 8f・F1〜F3・E1〜EC・EE〜EF）、`left < need`、2 番目の byte の確認を外す、3 番目以降の始めの index・範囲、範囲の両端、method の長さと memcmp の長さ、password の上限 2 通り、NUL の検査、publickey の has_signature・signed_size・algorithm の確認・blob の確認・key_blob と signature の pointer と size、request の wipe（clear の中と parse の始め）と error の道の clear、signed data と signature の buffer の limit、blob の長さの 51、failure の NUL 終端・初期化、banner の NUL 終端。
結果: 94 通りのうち 91 が FAIL（試験が捕まえた）、3 が生き残った。生き残った 3 つは全て同値の変更:
(1) `ssh_auth_put_publickey_signed` の `key->private == 0` の確認を外すと、後ろの `ssh_key_sign` が同じ EINVAL を返し、取り戻しで out も元に戻るので観測できる動作が同じ。
(2) UTF-8 の表の `lead >= 0xf1U && lead <= 0xf3U` を `0xf0U` からにしても、F0 は前の分岐（`lead == 0xf0U`）が先に取る。
(3) user の名前の読みの失敗の道の `ssh_auth_request_clear` を外しても、始めに全消去してあり、失敗した `ssh_reader_get_cstring` は out を空にするので他に残る物が無い。
最初の流しで 1 つ見逃し（`limit > SSH_BANNER_MAX` を `+ 1U` に変える。capacity が丁度 SSH_BANNER_MAX + 2 の時だけ差が出る）があり、capacity が SSH_BANNER_MAX から +3 の境界の試験を足して FAIL するようにした。その前に method 名の 1 文字違い・service が 65 文字の時の request の clear・3 番目と 4 番目の byte が continuation でない banner の試験も足した。

### 自分で決めた点（header が黙っている所）

- `SSH_MSG_SERVICE_REQUEST`（5）と `SSH_MSG_SERVICE_ACCEPT`（6）の定数は header に無いので、.c の中に持つ（RFC 4250 4.1.2）。
- `ssh_auth_parse_request`: user・service・algorithm の名前の長さの超過・NUL は EINVAL（header の「NUL は EINVAL」と、phase.md の「user 65 文字は EINVAL」に合わせ、algorithm の 65 文字以上も同じく EINVAL。supported 0 にはしない）。method 名は長さに上限無し（未知の method は OTHER）。空の user は読む（policy は program）。password と新しい password は NUL を含めば EINVAL（text として使う程序で先に切れて別の password に一致するのを防ぐ）。password の変更の形は、新しい password を長さと NUL だけ確かめて structure には入れない（structure に field が無い）。署名なしの publickey は `signature` NULL・`signature_size` 0・`signed_size` 0。supported 0 の時も `key_blob` と `key_blob_size` は入れる（public_key は 0 のまま）。blob の型が algorithm と違う、algorithm が ssh-ed25519 でない、blob が壊れている、の全てを supported 0 に統一（請求は well formed）。最初に structure 全体を消し、どの失敗でも全 0 にする（NULL の payload で size > 0 や message 番号違いを含む）。
- `ssh_auth_parse_service`: 名前が長すぎる時だけ EMSGSIZE、他は EINVAL（header の「EINVAL or EMSGSIZE」）。失敗で `service[0] = 0`。
- `ssh_auth_parse_failure`: methods は name-list の確認（`ssh_reader_get_namelist`）を通った物だけ。capacity < 1 は EINVAL。list と NUL が入らなければ EMSGSIZE（methods は空）。`partial` は NULL 可（header に無いが便利）。成功と失敗のどちらでも出力は先に空/0 にする。
- `ssh_auth_parse_pk_ok`: algorithm が ssh-ed25519 でない、blob の型が別（ENOTSUP）、blob の key が違うは EPROTO。blob が壊れていれば EINVAL。public_key が NULL は EINVAL。
- `ssh_auth_parse_banner`: header の「LF と TAB 以外の制御」を C0 と DEL（0x7f）と C1（U+0080〜U+009F、C2 80〜C2 9F）にした。C1 は 1 文字を 1 つの `?`、不正な並びは 1 byte を 1 つの `?`（途中の byte の誤りで始めの byte だけ直し、残りは次の判定にする）。UTF-8 の判定は RFC 3629 4 の範囲（過長・surrogate・U+10FFFF 超を除く）。切りは min(capacity - 1, SSH_BANNER_MAX) で、文字の途中では切らない（入らない文字は落とす）。language tag は読んで捨てる（無い・余りがあれば EINVAL）。capacity 0 と text NULL は EINVAL。失敗で `text[0] = 0`。
- `ssh_auth_put_signed_data`: session_id が NULL か、payload が NULL で size > 0 なら out に EINVAL を記録（何も書かない）。session id の長さは `SSH_SHA256_SIZE`（32）。
- `ssh_auth_put_publickey_signed`: 引数が NULL・seed の無い鍵は EINVAL（out は変わらない）。out が既に error なら、その error を返す。署名の失敗（`ssh_key_sign` の EIO など）の時はこの呼び出しが足した分を `out->size = start` で取り戻す。署名する data の buffer の上限は `SSH_PACKET_LIMIT + 64`、署名 blob の buffer の上限は 128。user の長さは確かめない（program の側）。
- put 系の `auth_put_key_blob_string` は blob の長さ（51）を先に書いてから `ssh_key_put_public_blob` を呼ぶ（blob を別の buffer に組まない）。
- 表の値の block に当たる物（UTF-8 の範囲）は `auth_utf8_length` の comment に「Values referenced:」を置いた。RFC 3629 4 の ABNF は知識から書き、試験の例は手で作った。**この表の値の独立した 2 つの方法での照合は、RFC 3629 の text が手元の specs に無いため未実施**）。

### 契約への注意（header の変更案ではない）

- header の `ssh_auth_put_publickey_signed` の誤りの列挙は「0, EINVAL, EIO or the error recorded in out」で、`ssh_key_sign` が返す EMSGSIZE・ENOMEM の扱いは out の error の形で返すこともある（今の実装は署名 buffer の上限を十分に大きくしているので、実際には out の error だけ）。
- phase.md と仕様の食い違いは無し。

### 未実施

- 規約の全文の見直しの Phase（WS の最後）。ここでは coding-style §14 の「条件の中の関数呼び出し」「3 つ以上の clause」を自分の diff で見直し、直した。試験の file は一部（`setup_keys` の中の block、CHECK_BUFFER の macro）が規約の厳密な形ではない。
- ssh-crypto.c は host の OpenSSL 3.5 の shim で流した（libcrypto-compat との試験は p021）。zedBSD の build、QEMU・実機、host の OpenSSH との相互運用は未実施（p015 以降）。
- ENOMEM（malloc の失敗）の経路は host の試験で起こしていない。
