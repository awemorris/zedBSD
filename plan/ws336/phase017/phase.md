<!-- awesome-plan project=zedbsd record=ws336-p017 -->

# ws336-p017: sshd の pre-auth と session の process（sshd-preauth.c・sshd-session.c）

Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-02 12:37 P6 q1944。2026-11-02 14:28 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p008・p010・p016
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/sshd.h`（process・descriptor・root なしの mode）
- `plan/ws336/design.md` §6.2・§6.4・§8
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4254.txt`（SHA-256 `bfc727e9350067ea…`、全桁は同じ directory の SHA256SUMS）: §6（session: pty-req・env・shell・exec・subsystem・window-change・signal・exit-status・exit-signal）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-sshd.8`（SHA-256 `74dde0667a4c99e6…`、全桁は同じ directory の SHA256SUMS）: LOGIN PROCESS（子の環境）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/sshd-preauth.c`・`sshd-session.c`（必要なら `sshd-*.{h,c}` を足してよい）
- 試験の道具: `plan/ws336/tests/sshd-pair-host.sh`（host で root なしに monitor・pre-auth・session の 3 つの process を自前の ssh とつなぎ、AUTH_DONE・export・import・SESSION_START・LOGIN_RECORD を通す 1 本の script。最後の行 `sshd-pair-host: PASS`）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- pre-auth: CONFIG を受け、crypto を 1 回ずつ使ってから（root の時は）chroot と setresuid、`closefrom(SSHD_FD_MONITOR + 1)`、USERAUTH_SUCCESS を書き切ってから AUTH_DONE
- session: SESSION_START の後に ids（root の時）、import、shell・exec・subsystem、pty を自分で開く、環境は LANG・LC_*・TERM だけを受ける、exit-status と exit-signal、子の出力を全部送ってから EOF と CLOSE、CLOSING の channel の request に FAILURE
- descriptor: shell・command の子に monitor の socket が無い（子で `ls -l /proc/self/fd` 相当、または fcntl で確かめる）
- re-key の SIGN が monitor を通る（相手 OpenSSH の `RekeyLimit` で）

## 4. 注意

- root でない mode でも 3 つの process と socketpair を使う（sshd.h）。
- pty は session の process が user の uid で開く（kernel が持ち主を ruid にする、design §2.3）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、library の module の .c（ssh-*.c）、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/sshd-pair-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 14:28（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main cfe01c3647）。凍結の header（`ssh-*.h`・`sshd.h`）は変えていない。

### 作った物

- `userland/base/ssh/sshd-preauth.c`（骨格の置き換え）: CONFIG を受け、crypto を 1 回ずつ使い（乱数・SHA-256・Ed25519 の鍵・署名・確かめ・hybrid の両側・ChaCha20・Poly1305）、root の時は chroot `/var/empty`・`setgroups`・`setgid`・`setuid`（root に戻れないことを確かめる）、transport（署名は monitor の SIGN）、service request と user 認証、USERAUTH_SUCCESS を書き切ってから export と AUTH_DONE。
- `userland/base/ssh/sshd-session.c`（骨格の置き換え）: CONFIG と SESSION_START、root の時は initgroups・setgid・setuid の後に import、session channel（pty-req は自分で openpty、env は LANG・LC_*・TERM だけ、shell・exec・subsystem sftp、window-change、signal、exit-status・exit-signal・EOF・CLOSE、CLOSING の channel の request に FAILURE）、re-key の署名は monitor、pty の LOGIN_RECORD・LOGOUT_RECORD、ClientAlive。
- `userland/base/ssh/sshd-child.{h,c}`（新、2 つの子の共通）: 子の始め（closefrom・SIGPIPE・接続を non-blocking・openlog を先に）、monitor の frame と CONFIG、monitor 経由の host_key・sign の ops、RESULT の問い、AUTH_DONE の送り、pending の blocking の flush。
- 試験の道具: `plan/ws336/tests/sshd-pair-host.sh`（1 本の script、最後の行 `sshd-pair-host: PASS`）、`sshd-pair-host.c`（listener の代わりの driver: 設定と host key を読み 127.0.0.1 の 1 接続で `sshd_monitor_run_program` を自分を子として呼ぶ）、`sshd-pair-client.c`（library の client 部品だけで作った client、OpenSSH が送らない場合のため）。

### 試験の作り（host を触らない）

- 自前の ssh（p015）は作業中なので、相手は host の OpenSSH 10.0p2 の ssh・sftp と上の client。期待の値は仕様（RFC 4252 5、RFC 4254 5.2・5.3・6.4・6.9・6.10）と server の約束から書いた（OpenSSH の出力を期待の値にしていない。OpenSSH は相互運用の相手）。
- Q1 の注意（2026-11-02）に従い、host の account の login shell・home には触れない: driver の中で `getpwnam_r` を置き換え、試験の account に run directory の `home` と `shell.sh`（command は `/bin/sh -c`、login は印を出して終わるだけ）を与える。他の名前の account は無い。`syslog` も driver の中で run directory の `log` に置き換えた（host の log に出さない）。ssh・sftp は `env -i`、`-F /dev/null`、専用の known_hosts と鍵（host の ssh-keygen で作る入力）。書くのは worktree の `build/tmp/sshd-pair` の下だけ。

### 試験の中身（`sh plan/ws336/tests/sshd-pair-host.sh`、1 接続 1 case）

- exec: stdout・stderr・exit 3、monitor の終わり 0 と MaxStartups の pipe に 1 byte、log に Accepted。
- env: SetEnv の LANG・LC_TIME は受け、FOO・LD_PRELOAD は受けない。HOME・USER・SHELL が試験の account、SSH_CONNECTION（client の address・port・server の address・port）、SSH_CLIENT（address・port・server port）、pty の無い時に SSH_TTY が無い。
- rekey: 3 MB を cat に通して往復（window が両方向に動く）、RekeyLimit 512K で再交換 24 回（session の process は host key を持たないので、全部 monitor の SIGN を通った）。
- pty（`-tt`）: `/dev/pts/N`、TERM が pty-req の物、SSH_TTY がその tty、command の descriptor は 0・1・2 だけ、LOGIN_RECORD が monitor に届き（monitor の log「not a process of the session」、host では親の確かめができないので断る）、session は続く。login shell（command 無し）が terminal で動く。
- slow: `sleep 2; cat` に 1 MB（client の EOF は保った data を全部書いてから子の入力を閉じる）。
- late: shell が終わった後に残った process が書く stdout・stderr が届いてから channel が終わる（stdout が後に終わる run と stderr が後に終わる run）。
- descriptors: pipe の command の descriptor は 0・1・2 だけ（monitor の socket・接続が無い）。
- signal: `kill -TERM $$` で exit-signal。
- sftp: subsystem（host の `/usr/lib/openssh/sftp-server`）で put と get、内容が一致。
- refused: 認められていない鍵は Permission denied、monitor の終わり 1・grant 0。
- tries: MaxAuthTries 2 と認められない鍵 3 つで「Too many authentication failures」（DISCONNECT 14）、monitor が上限を越えた要求を見ていない（log に refused request が無い）。edge: MaxAuthTries 2 で、断られる鍵の後の正しい鍵は通る（none は試行に数えない）。
- client の case（`sshd-pair-client.c`）: closing（output、exit-status、EOF、CLOSE がこの順、server が閉じた channel への request に FAILURE、その後に次の channel が動く）、user-change と service-change（DISCONNECT 2）、service（最初の request の service が ssh-connection でない: DISCONNECT 7）、service-request（ssh-userauth でない: DISCONNECT 7）、early（login の前の CHANNEL_OPEN: DISCONNECT 2）、requests（env の LD_PRELOAD・LC_・lc_all・LC_A-B・LC_A=B・LANGUAGE は FAILURE、LANG・LC_ALL・TERM は SUCCESS、2 回目の pty-req・exec、sftp でない subsystem、start の後の env は FAILURE）、signal（"signal" TERM で exit-signal TERM）、max-sessions（MaxSessions 1 で 2 つ目の channel は確かめの後すぐ CLOSE、そこへの request は FAILURE、1 つ目は続く）、window（window を SSH_CHANNEL_WINDOW + 1000 にして返さないと server はちょうどその byte 数で止まり、返すと残り 2200000 byte まで全部来る）、limit-key・limit-password・limit-off（MaxAuthTries 1 で 1 回目は FAILURE、2 回目は DISCONNECT 14。鍵、PasswordAuthentication yes の password、no の password）。
- どの process にも ASan・UBSan の報告が無い（子の stderr は driver の file）。

### 実行した command と最後の行

- `sh plan/ws336/tests/sshd-pair-host.sh` → `sshd-pair-host: PASS`（約 80 秒）
- `sh plan/ws336/tests/run-unit.sh all` → 17 本が全部 PASS（auth 2458・buf 323・channel 1603651・cipher 1064・common 2316・config 1321693・crypto 1656・hostfile 736314・kex 402989・key 2771・monitor 1435・packet 369168・privsep 947・sftp 1836・tools 13・transport 128389・ttymodes 20682）、最後の行 `run-unit: PASS`
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（新しい sshd-child.c を含む全 .c が cc・clang の警告 0、zedBSD の header と clang でも compile）
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p017-mut/mut.py` と `mutants.py`。変異ごとに必要な tree（userland/base/ssh、login/verify、試験の道具、host-crypto、fresh-out.sh）の写しを作り、その中の sshd-preauth.c・sshd-session.c・sshd-child.c の 1 つを変えて `sshd-pair-host.sh` を流す（worktree の file は変えない）。RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s、150 s と RSS 6 GB で kill。150 s で kill された物は run.log に FAIL の行があれば捕まえた、最後の case まで FAIL が無ければ生き残りと数えた。

- 1 回目（54 通り、同時 4）: 42 が FAIL、12 が生き残り（うち 4 つは最後の case まで通り終わりの sync の途中で kill された物を見直して生き残りと数えた: password の試行を数えない、他の method を数えない、service の変更、ssh-userauth 以外の service request）。試験を足した: MaxAuthTries 2 で断られる鍵の後の正しい鍵（none を数えない）、client の early・limit-key・limit-password・limit-off・service-change・service-request、max-sessions の閉じた channel への request、slow（EOF の後の data）、late、pty の monitor の log、window。
- 2 回目（54 通り、新しい directory、同時 3）: 48 が FAIL、6 が生き残り。late を stdout と stderr のどちらが後に終わるかの 2 run に分け、window の大きさを読みの大きさに揃わない値（+1000）にして、3 つ（stderr を待たない、stdout を待たない、send の room を見ない）を流し直し、3 つとも FAIL（`MUT_PREFIX=r3-`）。
- **残る 3 つは同値**: 子が始まる前の data を保つ（捨てずに、子が始まってから書く。誤りにならない振る舞いの違い）、SIGNATURE 以外の返事の型の確かめを外す（monitor は SIGNATURE しか返さない）、flush の EAGAIN を誤りにする（USERAUTH_SUCCESS の後の出力は小さく、試験の中では一度で書ける）。

結果は `build/tmp/p017-mut/run1.log`・`run2.log`・`results.json`・`r2-results.json`・`r3-results.json`。

### 自分で決めた点（header が黙っている所）

- **共通の file** `sshd-child.{h,c}` を足した（phase.md の「必要なら sshd-*.{h,c} を足してよい」）。
- **pre-auth の試行の数え方**（p016 の monitor の決定に合わせる）: monitor に問う login（publickey・password）と断られた query は 1 回、許された query は別に MaxAuthTries まで。加えて monitor に問わない失敗（他の method、off の method、他の型の鍵、password の変更の形、monitor に渡せない大きさの request）も 1 回に数えるので、こちらの数は monitor 以上で、上限には monitor より先に着く。上限で次の試行の前に DISCONNECT 14（「Too many authentication failures」）。"none" は数えない（RFC 4252 5.2: method を知るための物）。
- **user と service**: 最初の USERAUTH_REQUEST の user と service に固定し、変えた client は DISCONNECT 2。最初の service が ssh-connection でなければ DISCONNECT 7、SERVICE_REQUEST が ssh-userauth でなければ DISCONNECT 7。login の前の他の message（channel など）は DISCONNECT 2。
- **権限を落とす**: root の時は `setresgid`・`setresuid` の代わりに `setgid`・`setuid`（root では real・effective・saved を全部変える、POSIX）を使い、uid・gid を確かめ、`setuid(0)` が失敗することを確かめる（glibc は `_GNU_SOURCE` 無しに setres* を宣言しないため。zedBSD でも同じ結果）。pre-auth は `setgroups(1, sshd の gid)`、session は `initgroups`。passwd の検索は chroot の前。
- **log**: `openlog(LOG_NDELAY)` を chroot の前に。
- **hand-over**: USERAUTH_SUCCESS を書き切ってから export。client が始めた再交換の途中なら終わるまで進めるが、その間に上の layer の message が来たら失われるので接続を切る。
- **session の子**: `setsid`、pty なら `TIOCSCTTY` と 0・1・2 に dup2、`closefrom(3)`、signal の mask を空に、SIGPIPE・SIGCHLD を既定に。shell は account の shell（空なら /bin/sh）で、shell request は `-<basename>`、exec と subsystem は `<basename> -c <command>`（sshd(8) の「全部の command は login shell の下で」）。NUL を含む command は FAILURE。
- **環境**: HOME・USER・LOGNAME・SHELL・PATH（`/bin:/sbin:/usr/bin:/usr/sbin`）・SSH_CONNECTION・SSH_CLIENT、pty の時は SSH_TTY と TERM（pty-req の TERM が client の env の TERM に勝つ）、client の env は LANG・TERM・`LC_` に 1 文字以上（名前は A〜Z・0〜9・`_` だけ）を 32 個まで、start の後の env は FAILURE。MAIL・/etc/nologin・motd・lastlog・~/.ssh/rc は扱わない（後の判断）。
- **pty**: `openpty`（session の process が user の uid で開くので持ち主は user）、client の mode を `ssh_ttymodes_apply` で slave に、size を winsize に（65535 を越える値は 65535）。child が終わった後、master に残る出力を読み、何も無くなったら出力の終わり（他の process が slave を持っていても待たない）。pipe の時は EOF まで待つ（残った process の出力も送る）。
- **exit**: exit-status、signal で終わった時は RFC 4254 6.10 の 13 の名前なら exit-signal（core は FALSE）、他の signal は exit-status 128 + 番号。その後に EOF と CLOSE（それぞれ別の message）。
- **MaxSessions**: 上限を越える session channel は確かめた後すぐ CLOSE（channel の表に OPEN_FAILURE の口が無い）。
- **CLOSE を受けた時**: こちらも CLOSE、子の descriptor を閉じ、まだ動く子の process group に SIGHUP、terminal の LOGOUT_RECORD。接続の終わりも同じ。
- **message の扱い**: 50〜79 は login の後なので無視（RFC 4252 5.1）、192〜255 は無視、5・6 と他は DISCONNECT 2。global request は表の通り（want_reply なら FAILURE）。CHANNEL_SUCCESS・FAILURE は捨てる。
- **出力の制御**: transport の出力が 1 MiB を越える間は子の出力を読まない（ENOBUFS を起こさない）。子への data は window の中で保ち、書けた分だけ window に返す。
- **ClientAliveInterval**: client から何も来ない interval ごとに `keepalive@openssh.com`（want_reply）、ClientAliveCountMax 回答えが無ければ切る（試験は未実施）。
- **login record**: pty の子を起こした後に LOGIN_RECORD（tty・子の pid）、断られたら log だけ。

### 未実施・残り

- root での確かめ（chroot、`sshd` user への変更、session の initgroups と ids、LOGIN_RECORD の肯定の道）は host に root が無いので未実施。p019・p022 の QEMU で。
- ClientAlive、global request の返事、50〜79 の無視、/etc/nologin などは試験していない。
- 自前の ssh（p015）との相互運用は p018 以降。
- 規約の全文の見直しは p025。ここでは coding-style §14 で自分の diff を見直し、3 つ以上の clause の条件を分け、条件の中の関数呼び出しを外した。呼び出しの結果を変数に入れて直に返す所（`status = f(); return status;`）が残る。試験の file は規約の厳密な形でない。
- 消す物（Q1 に頼む）: `build/tmp/p017-mut/`（変異の写し、約 110 の directory）、`build/tmp/sshd-pair.run.*` の古い run。
