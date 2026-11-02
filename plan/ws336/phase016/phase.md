<!-- awesome-plan project=zedbsd record=ws336-p016 -->

# ws336-p016: sshd の monitor と login の確かめ（sshd-monitor.c・sshd-login.c）

Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-02 11:26 P6。2026-11-02 12:34 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p009・p011・p012・p014
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/sshd.h`・`ssh-privsep.h`（monitor の約束、受け入れた危険）
- `plan/ws336/design.md` §6.2・§8・§9
- `userland/base/login/verify.c`・`main.c`（login の確かめと utmpx の書き方）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-sshd.8`（SHA-256 `74dde0667a4c99e6…`、全桁は同じ directory の SHA256SUMS）: AUTHORIZED_KEYS FILE FORMAT と StrictModes
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4252.txt`（SHA-256 `a36b882cf77b1073…`、全桁は同じ directory の SHA256SUMS）: §7（署名される data）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/sshd-monitor.c`・`sshd-login.c`（必要なら monitor の内部の関数を出す `sshd-monitor.h` を足してよい。sshd.h の宣言は変えない）
- `plan/ws336/tests/unit/test-monitor.c`（` * run-unit-sources:` の行で `userland/base/ssh/sshd-monitor.c`・`sshd-login.c` と `userland/base/login/verify.c` を名指す。shadow を読む所は host では root が要るので、試験は password の道を crypt の呼び出しの回数と時間で確かめ、実の shadow での確かめは p022）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- monitor の request の確かめを、乗っ取られた子を模した request の列で: 署名 data の session id・user・service・method・algorithm・blob の各 1 つの不一致、末尾の余り、AUTH_DONE の前の SESSION 系、2 回目の AUTH_DONE、SSHD_PREAUTH_SIGN_MAX +1 回目の SIGN、長さ 31・33 の h、PASSWORD_LOGIN の後の PUBKEY_LOGIN の試行回数の合計（MaxAuthTries）
- SIGN の間隔: session の段で続けての SIGN が SSHD_SIGN_INTERVAL_MS 待たされる
- 時間: 応答の時刻が要求から SSHD_FAIL_DELAY_MS 以上後（clock_gettime で測る）、存在しない user でも crypt が走る（呼ばれた回数を試験で数える）
- authorized_keys の子: FIFO・device・symlink・group 書き込み可の file・directory を拒み、user の uid で読む（host の試験では自分の uid で、他の user の file を置けない場合は理由を結果に書く）
- login_record: /dev/pts 以外・他の user の tty を拒む

## 4. 注意

- monitor の子は fork と execve（sshd.h）。この Phase の試験は host で root なしに流すので、execve と chroot を通る道は p017・p018 と p022 で確かめる。
- 環境変数は LANG・LC_*・TERM だけを受ける（session の process の仕事だが、monitor から CONFIG で渡す値に入れない事をここで決める）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、library の module の .c（ssh-*.c）、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh monitor`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 12:34（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main 613d0117d）。凍結の header（`ssh-*.h`・`sshd.h`）は変えていない。

### 作った物

- `userland/base/ssh/sshd-monitor.c`（骨格の置き換え）: `sshd_monitor_run`・`sshd_monitor_run_program`（子の fork と execve、CONFIG・SESSION_START、LoginGraceTime、MaxStartups の pipe、AUTH_DONE の後に pre-auth を kill、session の終わりで LOGOUT を閉じる、host key の消去）、request の確かめ（`sshd_monitor_handle`）、設定の text の書き出し（`sshd_config_put_text`）。
- `userland/base/ssh/sshd-login.c`（骨格の置き換え）: `sshd_account_allowed`・`sshd_key_authorized`（user の ids の子が読む、StrictModes の walk）・`sshd_password_ok`（login_verify と dummy の crypt）・`sshd_login_record`（pututxline）と、内部の `sshd_account_policy`・`sshd_authorized_path`・`sshd_tty_path_ok`・`sshd_tty_check`。
- `userland/base/ssh/sshd-monitor.h`（新、program の内部の header。凍結の対象の外）: monitor の状態（`struct sshd_monitor`、段 PREAUTH・GRANTED・SESSION）、`sshd_monitor_handle` の誤りの約束（EPROTO は接続を切る）、試行の数え方、上の内部の関数。
- `plan/ws336/tests/unit/test-monitor.c`（1435 checks、` * run-unit-sources:` で sshd-monitor.c・sshd-login.c・login/verify.c）。期待の値は ssh-privsep.h・sshd.h・sshd-monitor.h の約束、RFC 4252 7 の署名の data を `wire.h` で 1 field ずつ壊した bytes、RFC 8032 7.1 TEST 1（host key）・TEST 2（user key）。他の実装の出力は使っていない。`crypt` と `syslog` は試験の中で置き換え、crypt の回数と salt、log の理由の行を見る（host の log に何も出さない）。monitor の全体（`sshd_monitor_run_program`）は試験の program 自身を `-Zpreauth`・`-Zsession` の偽の子として起こして流す。

### 試験の中身

- request の確かめ（乗っ取られた子の模倣）: 段ごとに許されない全ての型（PREAUTH で AUTH_DONE・LOGIN/LOGOUT_RECORD・SIGNATURE ほか、GRANTED で login・query・record、SESSION で 2 回目の AUTH_DONE と pre-auth の要求）が EPROTO。SIGN の前の認証の要求が EPROTO。h の長さ 31・33・0・余り・長さが端を越えるのが EPROTO。SSHD_PREAUTH_SIGN_MAX（4）回の後の 5 回目が EPROTO（GRANTED の SIGN も同じ数に入る）。session id は最初の h。
- 署名の data（RFC 4252 7）の 1 field ずつの不一致 17 通り: session id の 1 bit・31 byte・33 byte・後の交換の H、message 番号、user（別・1 byte 多い）、service、method、FALSE、algorithm、別の鍵の blob、blob の 1 byte 短い・1 byte 長い、末尾の余り、blob 無し、別の鍵の署名 → 全部 RESULT EACCES（試行 1 回、段は PREAUTH のまま）。全部正しい物だけ RESULT 0 で GRANTED。署名の 1 byte 変更、file に無い鍵、PubkeyAuthentication no も EACCES。
- 形の誤り（EPROTO）: login・query の余り、署名の切れ、ssh-rsa の blob、65 文字と NUL を含む user、password の 257 byte と NUL（256 byte は受ける）、record の余りと切れ、AUTH_DONE の余りと空。user の途中の変更も EPROTO。
- 試行: password 3 回 + publickey 3 回で 6 回、7 回目（publickey・query・password のどれも）が EPROTO。5 回の失敗の後の 6 回目の正しい login は許可。断られた query も 1 回に数える。許された query は別の上限（MaxAuthTries）で、その後も login はできる。
- 時間（clock_gettime の CLOCK_MONOTONIC）: 断る query・存在しない user の password（crypt 1 回）・許可の login の応答が要求から SSHD_FAIL_DELAY_MS 以上後。2 秒前に着いた要求は待たない、0.5 秒前の物は残りの約 0.5 秒、300 ms の delay も守る。pre-auth の SIGN は待たない。session の段の続けての SIGN は SSHD_SIGN_INTERVAL_MS 以上の間隔（数の上限は無い）。新しい monitor の delay と間隔は header の定数。
- password: 存在しない user・自分の user（root でない）・空の password で crypt がちょうど 1 回、salt は dummy の `$6$` と 16 文字、crypt の答えと password は消える。PasswordAuthentication no では crypt 0 回で消す。
- account: 作った passwd の項目で、root の時の shadow 無し・`!`・`*` の拒否、root でない時の他の uid の拒否、`/sbin/nologin`・`/usr/sbin/nologin`、root と PermitRootLogin の yes・no・prohibit-password（password は拒否）・範囲外、login でない method。host の passwd で、自分は許可、無い user・空・65 文字は ENOENT、root は EACCES。
- authorized_keys の子（自分の uid、作った home）: 鍵がある・無い、`//` と `.` と home の末尾の `/`、group・others の書ける file（StrictModes no なら読む）、読めない 0000 の file（子は user の権利で開く）と 0400、`.ssh`・home の 0770・0702・0777、symlink の file（StrictModes に関わらず拒否）、symlink の directory、FIFO（待たずに 5 秒以内に拒否）、directory、`/dev/zero`、`..`、無い file、`/`で終わる path、none、home の名前に続く別の directory（`<home>X/…`）、ちょうど SSH_HOSTFILE_MAX byte（末尾の行の鍵を見つける）と 1 byte 多い file、sshd が動く user でない account（log に理由）。
- path の展開: 相対、`%h`・`%u`・`%%`、他の token、末尾の `%`、room の境の両側、相対の home。
- login_record: `/dev/pts/` と 1〜8 桁だけ（9 桁・数字以外・`/dev/tty1`・`/dev/pts/`・NULL を拒否）。LOGIN_RECORD は理由の log で、どの確かめが断ったかを見る: path の形、17 byte は受けて 18 byte は形の誤り、NUL・空・pid 0・2^31 は field の誤り、自分の pty を他の uid で（持ち主の確かめ）、自分の uid で（親の確かめ、host では常に断る）、記録済みの tty、MaxSessions 0。LOGOUT_RECORD は書いていない record・pid 違い・tty 違いを拒否。
- 設定の text: 既定の text を手で書いた期待の bytes と一致、全 field を既定から変えた設定（空白・`#`・`=` で始まる path は引用、`"` を含む path）の往復、表せない値（空白と `"`、`"` で始まる、制御文字、bool の 2、範囲外の PermitRootLogin、MaxAuthTries 0、HostKey 9 個、空の ListenAddress）は EINVAL で out は変わらない、out の既存の誤り、小さすぎる out。
- monitor の全体（偽の子）: login と session（CONFIG の中身、SIGN、PUBKEY_LOGIN、AUTH_DONE、SESSION_START の user と transport、session の SIGN の間隔、LOGIN_RECORD の拒否）、子の descriptor は 0〜4 だけ（5〜255 が閉じている）、環境は空、session が始まった後に monitor が接続を持っていない（/proc/<親>/fd、試験の側の Linux の口）、MaxStartups の pipe に 1 byte、host key の消去、全ての子が monitor より先に終わる（接続の EOF）。pre-auth の LOGIN_RECORD・型の無い frame（LoginGraceTime より早く切る）・LoginGraceTime 1 秒の超過・login せずに終わる、context の blob の不一致（子を起こさない）、session の 2 回目の AUTH_DONE → どれも 1 を返し、session を起こさない（または kill）。

### 実行した command と最後の行

- `sh plan/ws336/tests/run-unit.sh monitor` → `test-monitor: PASS (1435 checks)` / `run-unit: PASS`（約 15 秒。monitor の待ちを含む）
- `sh plan/ws336/tests/run-unit.sh all` → auth 2458・buf 323・channel 1603651・cipher 1064・common 2316・config 1321693・crypto 1656・hostfile 736314・kex 402989・key 2771・monitor 1435・packet 369168・privsep 947・sftp 1836・tools 13・ttymodes 20682 の 16 本が全部 PASS、最後の行 `run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（cc・clang の警告 0、zedBSD の header と clang での compile を含む）
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: FAIL`（`ssh-transport.h` の差だけ。この branch は main 613d0117d から分かれ、p008 の header の変更 0e9b68f948 を持たない。自分は header を変えていない。main に merge した後に Q1 が 0e9b68f948 で流し直す）
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p016-mut/mut.py` と `mutants.py`（worktree の file は書き換えず、変異の写しを別の directory に作って test-monitor を build・実行。同時 4、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s、150 s と RSS 6 GB で kill、変異ごとに新しい directory）。1 回目は 132 通りで 104 が FAIL・28 が生き残り、生き残りのうち試験で見分けられる物に試験を足した（署名の data の blob の 1 byte 長い形、300 ms の delay、GRANTED の段の正しい形の login、LOGIN_RECORD の拒否の理由を log で見る、`sshd_tty_path_ok` を出して path の規則を直に、18 byte と 17 byte の tty、型の無い frame の偽の子、context の blob の不一致、session の開始の後に monitor が接続を持たない、home の名前に続く別の directory）。2 回目（新しい directory で全部やり直し、record の確かめの 4 通りを足して 136 通り）: **123 が FAIL**（うち 3 は timeout で捕まえた）、**13 が生き残り**、適用不能 0。生き残りは全て同値の変更か host で作れない物:

- 設定の text の 5 通り（読み戻しの確かめを外す、等しさの port・HostKey の比較を外す、語の制御文字の拒否を外す、空白と `"` の拒否を外す）: どれも残りの層（語の規則・読み戻し・全 field の比較）が同じ設定を断るので結果が変わらない（各層が他の層の代わりになる）。
- `allowed: shadow used without root`: root でない host では shadow が読めず hash が無い（root で p022）。
- `lookup: user length edge`（65 文字を受ける）: 65 文字の user は passwd に無く getpwnam が失敗する。
- `tty: not a device accepted`: `/dev/pts/<数字>` は host では character device だけで、他の種類を作れない。
- `keys: no size limit`（子の上限を外す）: monitor の側の受け取りの上限（SSH_HOSTFILE_MAX + 1）で断る。
- `keys: none opened`（`/dev/null` を開く）: 普通の file でないので断る。
- `keys: trailing slash of home not trimmed`: 展開した path も同じ home の文字列から作るので同じ所を歩く（違いは絶対の path を設定した時だけで、その時は host の `/` からの directory を歩き、試験が持ち物を決められない）。
- `keys: empty last name accepted`: `openat(…, "")` が ENOENT で断る。
- `keys: dot not skipped`: `.` は同じ directory を開き、同じ確かめを通るので結果が同じ。

壊した写しは別の directory の物だけで、worktree の file は変えていない（`git status` は自分の変更の 4 file だけ、`run-unit all` の再実行で PASS）。結果は `build/tmp/p016-mut/run1.log`・`run2.log`・`results.json`。

### 自分で決めた点（header が黙っている所、Q1 と決めた所）

- **CONFIG の text**（Q1 と合意）: `sshd_monitor_run` は parse 済みの `struct sshd_config` しか受けないので、monitor が struct から正規の sshd_config の text を書き（`sshd_config_put_text`、keyword 1 行ずつ、空白などを含む語は `"` で囲む）、自分で `sshd_config_parse` で読み戻して全 field が同じ時だけ送る。表せない設定（空白と `"` を両方含む path など）は monitor を起こさない。ssh-privsep.h の「the file as the listener read it」とは文字列が違い、意味（子が読む設定）が同じ。
- **LOGIN_RECORD の「pid が session の子」**（Q1 と合意）: zedBSD（`__ZEDBSD__`）では `/dev/system` の `KERN_SYSTEM_GET_PROCESS`（pid - 1 の次の process）で pid・ppid == session の pid・uid == user を確かめる。他の OS（root でない試験の mode）では確かめられないので EACCES。host の試験は肯定の道を通せない（p019・p022 の QEMU で）。
- **子の program の path**: `sshd_monitor_run` は `SSHD_PROGRAM`（`/usr/sbin/sshd`）。他の path（host の試験）は `sshd_monitor_run_program(…, program)`（sshd-monitor.h）。子は `argv = {program, -Zpreauth|-Zsession}`、**空の環境**、signal の mask を空に、SIGPIPE を既定に戻して execve（子は自分で SIGPIPE を無視すること）。descriptor は 10 以上に退避してから 3・4 に dup2、`closefrom(5)`。
- **user**: 最初の認証の要求（query・login）の user に固定し、別の user の要求は EPROTO（接続を切る）。pre-auth も同じ user を保ち、変えた client を自分で切ること（p017）。
- **試行の数え方**: PUBKEY_LOGIN・PASSWORD_LOGIN は 1 回ずつ、断られた PUBKEY_QUERY も 1 回。許された query は別に MaxAuthTries まで。上限の後の要求は EPROTO なので、pre-auth は自分で数えて DISCONNECT 14 を先に送ること（p017）。認証の要求は session id（最初の SIGN）の前は EPROTO。
- **断る（EACCES）と切る（EPROTO）の分け方**: frame の形の誤り（余り・切れ・型・長さ・NUL・ed25519 でない blob・長すぎる password）と順序の誤りは切る。client が影響する中身（署名の data の各 field、鍵、password、tty、pid）の不一致は RESULT EACCES で答え、試行に数える。LOGIN_RECORD の tty の長さ・NUL・pid の範囲も EACCES（log に理由）。
- **GRANTED の段の SIGN**: USERAUTH_SUCCESS と AUTH_DONE の間の re-key のために許し、pre-auth の 4 回に数える。
- **AUTH_DONE の後**: pre-auth を待たずに socket を閉じて SIGKILL し reap する（session が接続を持った後に pre-auth が触れないように）。session が socket を閉じたら waitpid で待ち（kill しない）、違反なら kill。どちらも残った login record の DEAD_PROCESS を書く。終わりの値は session が普通に終わった時 0、他は 1。MaxStartups の pipe は許可の時に 1 byte を書いて閉じる（一度だけ）。
- **LoginGraceTime** は pre-auth の開始からの全体（0 は無制限）。超えたら kill。
- **password**: `sshd_password_ok` は PasswordAuthentication が on なら必ず crypt を 1 回走らせる（存在しない・locked・root でない・空の password が禁止、の全部で dummy の hash）。例外は保存の hash が空の account（login_verify が crypt を呼ばない）。off なら crypt なしで断る（account に依らない）。dummy の hash は SHA-512 crypt（`$6$`、zedBSD の account の hash と同じ種類、salt 16 文字、hash 86 文字）。monitor は account の確かめと password の確かめを互いの答えに関わらず両方走らせる。crypt が static の buffer に残す hash は dummy の時に消す（login_verify の中の crypt の答えは verify.c の物で消せない。成功なら shadow と同じ hash、失敗なら誤りの推測の hash）。
- **root でない mode**: account は uid == euid の物だけ、shadow は見ない、password は常に断る（crypt は走る）、authorized_keys の子は uid を変えず、他の uid の account なら読まずに断る。
- **account の規則**: root の時は shadow の entry が要る（無ければ console と同じく拒否）。shell は `/sbin/nologin` と host の慣習の `/usr/sbin/nologin` を拒否（空・NULL の shell は許す、login は /bin/sh を使う）。sp_expire などの期限は login と同じく見ない。PIN・passkey は sessiond の画面の login の物で、sshd は使わない（header の「console login の規則」）。
- **authorized_keys**: path は `%h`・`%u`・`%%` を展開し、相対なら home から（`%h` で始まる物は絶対）。`/dev/null`（none）は開かずに拒否。FIFO・device・directory は StrictModes に関わらず拒否（普通の file だけ）、最後の名前の symlink も常に拒否（O_NOFOLLOW）。StrictModes の時は home（path が home の下の時。home 自身は symlink でも辿る）か `/` から 1 つずつ O_NOFOLLOW|O_DIRECTORY で開き、各 directory と file が user か root の持ち物で group・others が書けないことを fstat で確かめる。home の下の directory の symlink と `..` は拒否（OpenSSH より厳しい）。子は 10 秒（`SSHD_KEYS_TIMEOUT_MS`）で kill。子は fork した monitor の写しで host key の seed を memory に持つが、uid を変えた process は他から trace できない（kernel、design §2.3）ので execve しない。子が root の ids を取れない時は EIO、他は EACCES。
- **utmpx**: ut_line は `/dev/` の後（`pts/3`）、ut_id は端末の番号（8 桁まで）で console の id と重ならない。ut_host は空（`sshd_login_record` に host の引数が無い）。tty は `/dev/pts/` と 1〜8 桁だけ（`SSHD_TTY_MAX` 18 byte）。
- **環境変数**（phase.md §4）: CONFIG で送る text に AcceptEnv を入れない。LANG・LC_*・TERM だけを受けるのは session の process が固定で持つ（p017）。
- **log**: syslog の LOG_AUTH に、拒否の理由（invalid user、not allowed、authorized keys の理由、Failed publickey/password、record の理由、違反の型と段）。

### 契約への注意（header の変更案ではない）

- ssh-privsep.h の CONFIG は「the file as the listener read it」だが、上のとおり正規の text を送る（規約の見直しの Phase で comment を直すかを Q1 が決める）。
- `sshd_login_record` の約束は 0 か EIO なので、形の誤りの tty も EIO。持ち主・親の確かめは monitor の側（sshd-monitor.h の `sshd_tty_check` と内部）。

### 未実施・残り

- root での確かめ（shadow・`!`・`*` の実の account、user の ids への変更、他の user の持ち物の file、utmpx の実の書き込み、LOGIN_RECORD の肯定の道、`/dev/system` の親の確かめ）は host で root が無いので未実施。p017（sshd-pair-host.sh）・p022（QEMU）で。
- 変異の生き残り（上）のうち、root か zedBSD が要る物は p022 で確かめる。
- 規約の全文の見直しの Phase（p025）。ここでは coding-style §14 で自分の diff を見直し、3 つ以上の clause の分け方・条件の中の関数呼び出しを直した。試験の file は条件の中の呼び出しなど規約の厳密な形でない所がある（他の Phase の試験と同じ）。
- 消す物（Q1 に頼む）: `build/tmp/p016-mut/`（変異の写しと build、約 270 の directory）。`build/tmp/ssh-unit.run.*` の古い run（run-unit が作る）。
