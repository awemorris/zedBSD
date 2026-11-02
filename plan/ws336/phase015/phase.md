<!-- awesome-plan project=zedbsd record=ws336-p015 -->

# ws336-p015: ssh の program と host の相互運用（相手 A: host の OpenSSH sshd）

Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-02 11:37 P5）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p006・p008・p009・p010・p011
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/ws336/design.md` §4・§6.3・§8・§9・§11.3
- library の header 全部
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-ssh.1`（SHA-256 `c500e8177346aa12…`、全桁は同じ directory の SHA256SUMS）: option・終了の値・escape
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-ssh_config.5`（SHA-256 `6ef2f7058a6b7be9…`、全桁は同じ directory の SHA256SUMS）: StrictHostKeyChecking・BatchMode・RequestTTY・ServerAliveInterval
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-main.c`（必要なら client 内部の header と .c を足してよい。名前は `ssh-client-*.{h,c}`）
- `plan/ws336/tests/build-host.sh`（ssh・ssh-keygen・（p016 以降）sshd・sftp-server を host 向けに build、host の OpenSSL を shim で link）
- `plan/ws336/tests/interop-host.sh`（相手 A の部分。最後の行 `interop-host: PASS`）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- design §11.3 の相手 A の全項目（exec と exit status、8 MiB の stdin の往復の cmp、`-tt` と `stty size`、known_hosts の accept-new・CHANGED の拒否・BatchMode、`RekeyLimit 1M` の相手で 64 MiB、ServerAliveInterval）
- 相手が KexAlgorithms=curve25519-sha256 だけの時に「no matching」で 255
- 終了の値: 相手の exit 3 → 3、signal → 255
- known_hosts は HostName の名前で引き、書く（ssh_config の HostName を使った接続で、OpenSSH の ssh が同じ known_hosts を受ける）

## 4. 注意

- host の OpenSSH sshd は root でなく、一時の directory の設定（`HostKey`・`AuthorizedKeysFile`・`PidFile none`・`UsePAM no`・`StrictModes no`・`ListenAddress 127.0.0.1`・空いた port）で起こし、試験の後に止める。各 command に timeout。
- library の header に不備を見つけたら直さず Q1 に返す（この Phase は program の file だけを変える）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、library の module の .c（ssh-*.c、この Phase が足す client 内部の file を除く）、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/interop-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

- （2026-11-02 11:37 Q1 の条件）ssh は再鍵の時に server の host key が最初の鍵の交換と同じ事を check_host_key で確かめ、違えば切る（ssh-transport は覚える場所を持たない、p008 の結果）。→ 満たした（下の「再鍵の host key」）。

2026-11-02 13:40（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。worktree `p5-ws336`（branch agent/p5-ws336、基点 main 531e51df0）。header（基点 0e9b68f948 の library の header 16 本）は変えていない。

### 作った物

- `userland/base/ssh/ssh-main.c`（骨格の置き換え）: 設定の順（command line → `-F` の file だけ、無ければ `~/.ssh/config` と `/etc/ssh/ssh_config` → 既定）、HostName・user・port を決め、connect → transport → 認証 → session（か `-N`）→ 終了の値。
- program の内部の file（`ssh-client-*`、run-unit.sh・check-skeleton.sh の約束どおり library の外）:
  - `ssh-client-common.h`（内部の header: struct `ssh_client_args`・`ssh_client`、各 file の口）
  - `ssh-client-options.c`（command line、`-l`・`-p`・`-i`・tty と log の flag を ssh_config の行にして `ssh_config_option` へ）
  - `ssh-client-file.c`（HOME と `~/`、file の読み、stderr への message、`/dev/tty` の問い、monotonic の時計）
  - `ssh-client-net.c`（getaddrinfo と ConnectTimeout 付きの connect、socket と transport の間の bytes、ENOBUFS の時の backlog、message を待つ loop、終わり方の message、DISCONNECT の送りと閉じ）
  - `ssh-client-hostkey.c`（`check_host_key`: known_hosts 2 本・StrictHostKeyChecking・BatchMode・問い・行の追記、再鍵の同じ鍵の確かめ）
  - `ssh-client-auth.c`（service request → none → identity ごとに publickey の query と署名 → password を端末で最大 3 回）
  - `ssh-client-session.c`（session channel、pty-req・window-change・raw mode、shell/exec、標準の stream の poll loop、escape、ServerAlive、exit-status/exit-signal）
- `plan/ws336/tests/build-host.sh`（ssh・ssh-keygen・sshd・sftp-server を ASan・UBSan と host の OpenSSL（shim）で host 向けに build。`SSH_SOURCE_DIR` で壊した複写を build できる。sshd は sshd-*.c と login/verify.c と -lcrypt。今は sshd・sftp-server は骨格のまま link できることだけ確かめた）
- `plan/ws336/tests/interop-host.sh`（相手 A。最後の行 `interop-host: PASS`）
- `plan/ws336/tests/unit/test-client.c`（program の内部の部品の単体試験。` * run-unit-sources:` で ssh-client-*.c を名指す。run-unit.sh・check-skeleton.sh は変えていない）

### 実行した command と最後の行

最終の版で（2026-11-02 13:37〜13:40）:
- `sh plan/ws336/tests/interop-host.sh` → `interop-host: 93 checks, 0 failed` / `interop-host: PASS`（約 45 秒）
- `sh plan/ws336/tests/run-unit.sh all` → 17 試験すべて PASS（auth 2458・buf 323・channel 1603651・cipher 1064・**client 1046**・common 2316・config 1321693・crypto 1656・hostfile 736314・kex 402989・key 2771・packet 369168・privsep 947・sftp 1836・tools 13・transport 128389・ttymodes 20682）、最後の行 `run-unit: PASS`
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（7 番目の zedBSD の header と clang での compile も通った）
- `git diff --check HEAD` → 出力なし（rc 0）
- `sh plan/ws336/tests/build-host.sh build/tmp/p015-bin` → ssh・ssh-keygen・sshd・sftp-server すべて ok、`build-host: PASS`
- cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で ssh-main.c と ssh-client-*.c が警告 0（check-skeleton の中と手で）。

### 相互運用（相手 A: host の OpenSSH 10.0p2 の sshd）

host の `/usr/sbin/sshd` を自分の user（root でない）で、一時の directory の設定（`Port`・`ListenAddress 127.0.0.1`・`HostKey`・`AuthorizedKeysFile`・`PidFile none`・`UsePAM no`・`StrictModes no`・`HostKeyAlgorithms ssh-ed25519`・`Ciphers chacha20-poly1305@openssh.com`・`KexAlgorithms mlkem768x25519-sha256`）で、試験ごとの port に 9 つ起こした（a・rekey（RekeyLimit 1M）・old（KexAlgorithms curve25519-sha256 だけ）・password（PasswordAuthentication だけ）・alive（ClientAliveInterval 1・CountMax 2）・shell（ForceCommand `tty; exit 6`）・sh（ForceCommand `exec /bin/sh`）・tries（MaxAuthTries 1）・nosession（MaxSessions 0）・notty（PermitTTY no））。加えて python の「何も送らない listener」と「すぐ閉じる listener」と、誰も listen しない port。自前の ssh は `HOME` を一時の directory、`-F` を一時の設定にして動かし、実際の `~/.ssh`・`/etc/ssh`・system の sshd は読み書きしない。各 command に timeout、終わりと TERM・INT・HUP で sshd とその子を全部止める（止めた子は CONT してから KILL）。host key は OpenSSH の ssh-keygen、user key は自前の ssh-keygen（p014）で作った。

**host の account への影響と守り**（Q1 の指示、2026-11-02 13:0x）: host の sshd はこの account（awe）で動くので、command は必ず account の login shell（bash）の `-c` で、account の home を cwd にして動く（sshd の仕様で避けられない。bash は sshd から起こされると非対話でも `~/.bashrc` を読む。書き込みは無い）。それ以上に account に触れないように: 全部の sshd に `ForceCommand eval "$SSH_ORIGINAL_COMMAND"`（shell と sh の 2 つは自分の決まった command か `exec /bin/sh`）を置き、command の無い session（shell の要求）でも対話の login shell が決して起きないようにした（対話の bash は `~/.bash_history` を書く）。`PermitUserRC no`（`~/.ssh/rc` を動かさない）、`PermitUserEnvironment no`、転送は全部 no、`AuthorizedKeysFile` は試験の物（account の `~/.ssh` を読まない）。各 port は起動の後に「listen している socket が試験の起こした process（とその子）の物か」を `/proc/net/tcp` と `/proc/<pid>/fd` で確かめ、違えば試験を FAIL で止める（fail closed）。最後にもう一度全部の port で確かめる。**実際に触れてしまった事**: この守りの前の版の interop-host.sh（1 回、12:18）と、壊した mutant 2 つ（12:35 頃。pty を誤って付ける変更で、`a` の server の shell の要求が対話の bash になった）が対話の login shell を動かし、bash が `/home/awe/.bash_history` の末尾に 3 行（`tty; exit 6`・`echo shell-ran; exit 4`・`tty`）を足した。file は直していない（Q1 に報告済み、Q1 がユーザーに知らせる）。その後の試験と mutant の run の後で `.bash_history` の時刻と大きさが変わっていないことを確かめた。

確かめた項目（93 checks、判定は終了の値・cmp・出力の text。期待の値は known_hosts の形（sshd(8)）と試験自身の data から作り、OpenSSH の出力を期待の値にしていない）:
- exec と exit status（`exit 3` → 3、`exit 0` → 0、`exit 255` → 255）、`kill -TERM $$` → 255、引数を空白 1 つで繋ぐ。
- 8 MiB の stdin → `cat` → stdout の cmp、`cat 1>&2` で stderr の cmp、pipe への cmp。`-n`（空の入力）、stdin で動く shell（`exit 4`）、`-N`（server の global の keepalive に答えて 5 秒後も接続、signal で 255 と「Killed by signal 15.」）。
- 端末: `-tt` で `stty size` が 37 123（python の pty で大きさを決める）、途中で 40 100 に変えると remote も 40 100（SIGWINCH → window-change）、`\r~.` で 255 で直ぐに終わる、`~~tilde a~b` が `~tilde a~b` で届く、command 無し・stdin が端末で pty が付く（`/dev/pts/`）、command ありで `-t` 無しなら pty 無し、`-t` で stdin が端末でなければ pty 無しで「Pseudo-terminal will not be allocated…」、PermitTTY no の server で「PTY allocation request failed」と command は動く（5）。
- known_hosts: accept-new が `[127.0.0.1]:port ssh-ed25519 <host の .pub の base64>` の 1 行だけを書く、StrictHostKeyChecking yes で通り行は増えない、別の鍵の行で 255（StrictHostKeyChecking no でも 255、file は不変）、BatchMode の ask と yes の不明な host で 255 と file を作らない、pty で問いに yes → 接続と行の追記、no → 255 と file 無し。
- HostName: `Host alias-a` の HostName 127.0.0.1 と Port で接続し、known_hosts の行は `[127.0.0.1]:port`、その file を OpenSSH の ssh（`-F /dev/null`、StrictHostKeyChecking yes）が受けて接続できる、同じ file で 127.0.0.1 を直に名指しても通る。
- 再鍵: RekeyLimit 1M の server と 64 MiB を往復して cmp が同じ、`-vv` の「key re-exchange: the host key checked again is the same」が 60 回以上（実測 87）。
- ServerAliveInterval 1・CountMax 2 で 6 秒の command が続く（答えのある keepalive）、`kill -STOP $PPID` で server が止まると 8 秒以内に 255 と「Timeout, server … not responding.」。ClientAliveInterval 1 の server の channel の keepalive に答えて 6 秒の command が続く。
- KexAlgorithms curve25519-sha256 だけの server で 255 と「Unable to negotiate with 127.0.0.1 port N: no matching key exchange method found」、端末が無い時の message に CR が無い。
- ConnectTimeout=2 で identification を送らない server に 10 秒以内に 255（「Connection timed out during banner exchange」）。
- 認証の失敗: authorized_keys に無い鍵で 255 と「Permission denied (publickey,…)」、居ない user で 255、`-i` の無い file は「not accessible」と 255、mode 0604 と 0640 の秘密鍵は使わない、BatchMode では password を聞かない（pipe でも pty でも）、pty で 3 回の誤った password（3 回聞く、echo しない）で 255、MaxAuthTries 1 の server の DISCONNECT を「Received disconnect from 127.0.0.1 port N:2: Too many authentication failures」で見せて 255。
- MaxSessions 0 の server で「channel 0: open failed: …」と 255、command の中から `kill -KILL $PPID` で接続が切れると「closed by remote host」と 255、すぐ閉じる server で「Connection closed by …」と 255、誰も居ない port で「connect to host … Connection refused」と 255。最後に server がまだ動くこと。

### わざと壊す確かめ

`build/tmp/p015-mut/mut.py`（git の外、mutant の表は同じ directory の `mutants.py`）で、`userland/base/ssh` の**複写**（`build/tmp/p015-mut*/<名前>/ssh/`）の client の file（ssh-main.c と ssh-client-*.c）に 1 つずつ変更を入れ、test-client（ASan・UBSan）と `SSH_SOURCE_DIR` で interop-host.sh を流した（RLIMIT_FSIZE 64 MiB、RLIMIT_CPU 400 秒、RSS 6 GB か 150 秒で process group を TERM・KILL、並列 4）。前後で元の source の SHA-256 が同じことを script が確かめた（`source unchanged: True`）。

- 1 回目（試験の最初の版、92 通り）: 生き残り 25。試験を足した: `ssh -` と host、値の無い option の文、command の長さの境界（大きな buffer でも）、file の読みの上限、理由の切りの境界（capacity が長さと同じ）、backlog の上限の境界（長さの欄を数える）、port 22 の名前の message と `/dev/null` の時に message を出さないこと（stderr を捕まえて）、key directory を他の場所には作らないこと（unit）、端末の無い時の message に CR が無い、すぐ閉じる server、command の中で切られた接続、誰も居ない port、MaxAuthTries 1 の server の DISCONNECT、mode 0604・0640 の鍵、pty の上の BatchMode で password を聞かない、keepalive の時間の上限を 8 秒に、答えのある keepalive が数を戻すこと（6 秒）、-N で server の global の keepalive に答えること（interop）。
- 最後（試験の最終の版、`build/tmp/p015-mut4/result.txt` と `p015-mut5`）: **98 通りのうち 83 が FAIL**（unit 43・interop 40）、**6 は compile で弾かれた**（`-Werror` の未使用の変数・常に真の比較。同じ所を compile できる形に変えた 6 通りを足して流した）、**生き残り 9**。全部同値か host の試験で作れない状態:
  1. `file-line-cr`（`/dev/tty` の答えの CR を捨てない）: 端末は ICRNL で CR を LF にするので CR は届かない（同値）。
  2. `net-feed-emsgsize`（transport の EMSGSIZE を誤りにする）: 1 回に渡す 4096 bytes は、未完の packet の横に必ず入る（SSH_PACKET_INPUT_MAX の余裕 4096）ので起きない（到達しない防御）。
  3. `net-connect-timeout`（connect の待ちを長く）: 127.0.0.1 の connect は直ぐ終わり、SYN を捨てる相手は host で作れない。
  4. `host-size`（blob の大きさの確かめを `>` に）: `ssh_key_parse_public_blob` が 51 bytes ちょうどだけを通すので同値。
  5. `ses-exec-fail`（exec の拒否で 255 にしない）: OpenSSH の sshd に exec を拒ませる設定が無い（未確かめ、相手 B の p018 で自前の sshd に拒ませて確かめられる）。
  6. `ses-input-room`（window より 1 byte 多く送る）: OpenSSH は window を早く足すので、16 KiB の入力が残りの window を越える時が来ない。
  7. `net-reason-number2`（DISCONNECT 以外の packet も読む）: 出力の前の packet は KEXINIT で、乱数の cookie を長さに読むと必ず末尾を越えて誤りになる（実際上同値）。
  8. `ses-window-threshold2`（queue が空の時だけ window を返す）: 流れは正しく、速さだけが落ちる（同値）。
  9. `ses-chunk2`（regular file に 512、他に 64 KiB）: pipe に 64 KiB の write は block するが相手が読むので進む（試験で見えない）。
- 最後の run で wall の 150 秒で止めた物が 10（並列 4 の負荷で遅くなった物。9 つは止める前に FAIL の行が出ている。`ses-signal-quit` は signal で -N が終わらず掛かる本物）。1 回目と 2 回目の間で一度、並列の run の port が重なって他の instance の sshd に繋がった疑いがあった（free-port の後の bind までの競合）ので、port の持ち主の確かめ（上）を入れ、重なれば FAIL で止まるようにした（その FAIL は「見逃しの無い FAIL」と数えうるので、生き残りの判定は誤らない）。

### 自分で決めた点（header・phase.md が黙っている所）

- **command line**: option は destination の前だけ（ssh(1) の SYNOPSIS）。GNU の getopt は引数の並べ替えをする（`ssh host ls -l` の `-l` を option にしてしまう）ので、自前の小さな parser にした。flag の結合（`-ttv`）、値の結合（`-p22`）と分離（`-p 22`）、`--`。destination は最後の `@` で分け、host が `-` で始まれば断る（option の注入を防ぐ）。user は 1〜64、host は 1〜255。`-N` と command の併用は断る。
- **設定の順と優先**: 先に与えた値が勝つ（ssh_config(5)）ので、`user@host` の user → `-T`/`-t`/`-tt`（-T が勝つ）→ `-q`/`-v`（-q が勝つ、-v は VERBOSE、-vv 以上は DEBUG）→ `-l`・`-p`・`-i`・`-o` を与えた順 → `-F` の file だけ（`none` は file 無し）、`-F` が無ければ `~/.ssh/config`、`/etc/ssh/ssh_config`（無い file は飛ばす、誤りは止める）→ 既定。`-l` などは `Keyword "value"` の行にして `ssh_config_option` に渡す（`"` を含む値は断る）。IdentityFile を設定が与えた時だけ、無い file を警告する（既定の `~/.ssh/id_ed25519` が無いのは黙って飛ばす）。
- **`~` と HOME**: `~/` は環境の HOME、無ければ account の home（p014 の ssh-keygen と同じ。OpenSSH は account の home を使う）。HOME を変えれば試験が実際の `~/.ssh` に触れない。
- **known_hosts の判定**（`ssh_client_host_decide`、test-client で全 200 通り）: どちらかの file に @revoked → 断る。どちらかに一致 → 通す（CHANGED より強い）。どちらかに別の ssh-ed25519 の鍵 → **StrictHostKeyChecking no でも断る**（OpenSSH は no なら制限付きで続けるが、design §6.3 の「CHANGED・REVOKED は断る」を全ての設定に当てた）。他の種類の鍵だけ（OTHER_TYPES）は不明な host と同じ。不明な host は no・accept-new で追記して通す、yes で断る、ask は BatchMode なら断り、でなければ `/dev/tty` で問う（yes・no・指紋を受け、他は聞き直す。端末が無ければ断る）。global の file は読むだけで書かない。読めない・大きすぎる file は断る（fail closed）。
- **行の追記**: `ssh_known_hosts_put_line(HostName, port)`、file の最後に LF が無ければ先に LF、1 回の write で O_APPEND、作る file の mode は 0644（umask が効く）。UserKnownHostsFile が `/dev/null`（`none`）なら書かず、message も出さない。file の directory が無く、それが `~/.ssh` そのものである時だけ 0700 で作る。message は「Warning: Permanently added '<name>' (ED25519) to the list of known hosts.」（LogLevel INFO、-q で出ない）。
- **再鍵の host key**（Q1 の条件）: 最初に受け入れた blob を `struct ssh_client` に持ち、2 回目以降の `check_host_key` は byte で比べ、違えば message を出して EACCES（transport が DISCONNECT 9 で切る）。同じなら DEBUG の 1 行（試験で再鍵を数えるのに使う）。test-client で、同じ鍵は 0、別の鍵は EACCES を確かめた。
- **認証の順**: SERVICE_ACCEPT の service 名を確かめる → `none` → publickey を identity ごとに（query → PK_OK なら署名付き）→ password（BatchMode なら聞かない、最大 3 回、`user@host's password: `、echo を切る、PASSWD_CHANGEREQ は断って止める）。keyboard-interactive はしない（design §4.1）。BANNER は LogLevel INFO 以上で stderr に（ssh_auth_parse_banner が制御文字を消した物）。全部失敗で「user@host: Permission denied (methods).」と DISCONNECT 14、255。秘密鍵の file は group か other の bit が 1 つでも立っていれば使わない（ssh(1) FILES「accessible by others なら無視」を group にも当てた）。暗号化された鍵は「only unencrypted … supported」。
- **session**: channel を開いた後、pty-req（TERM、無いか 64 文字を越えれば `dumb`、stdin の端末の大きさと mode、端末でなければ大きさ 0 と TTY_OP_END だけ）→ no-more-sessions@openssh.com → shell か exec。pty の拒否は message だけで続け、shell・exec の拒否は channel を閉じて 255。SendEnv は受けて無視する keyword なので env は送らない。server からの CHANNEL_OPEN は全部 OPEN_FAILURE（administratively prohibited）、global request は FAILURE（ssh_channel_global_request）、want_reply の channel request は FAILURE。
- **RequestTTY**: auto は command 無しで stdin が端末の時、yes は stdin が端末の時（でなければ message）、force は常に、no は付けない。escape（`~.`・`~~`）は pty を頼んで stdin が端末の時だけ。行の始めは CR か LF の後と最初。他の `~X` は両方を送る。
- **raw mode**: POSIX の flag で自分で作る（ISIG・ICANON・ECHO・IEXTEN・IXON・ICRNL・OPOST などを切る、VMIN 1）。全ての終わり方で戻す。raw の間の message は CR LF で終える。
- **終了の値**: exit-status の下位 8 bit、exit-signal は 255（VERBOSE で signal の名前）、`~.`・signal（SIGINT・SIGTERM・SIGHUP、「Killed by signal N.」）・失敗・exit-status 無しは 255。`-N` は常に 255。pty を使った時は終わりに「Connection to <host> closed.」。
- **ServerAliveInterval**: session（と -N）の間だけ。上の layer の message を受けるたびに数を 0 に戻し、interval ごとに `keepalive@openssh.com`（want_reply）を送り、数が ServerAliveCountMax に届いていれば「Timeout, server <host> not responding.」で 255。
- **ConnectTimeout**: address ごとの connect と、server の identification の行が来るまで（OpenSSH の「banner exchange」と同じ範囲）。
- **I/O**: socket は non-blocking、標準の stream は mode を変えない（他の process と共有しうる）。regular file でない stream への 1 回の write は 512 bytes（POSIX の最小の PIPE_BUF。zedBSD の limits.h に PIPE_BUF は無く `_POSIX_PIPE_BUF` だけ）、regular file は 64 KiB。stream の queue は offset で読み、半 window ごとに詰める（write ごとの memmove で 64 MiB が 100 秒かかったのを直した）。**window**: `ssh_channel_consumed` は size に依らず window が半分を切れば満杯に戻すので、queue が半 window 未満の時だけ呼ぶ（でないと遅い stdout で queue が際限なく伸びた。最初の試験で 4 MiB の上限に当たって EMSGSIZE）。stdin は backlog が空で transport の出力が 1 MiB 未満で window に余地がある時だけ読む。
- **backlog**: transport の ENOBUFS（出力・held が満杯）の message は順を保って最大 1 MiB 溜め、毎回の loop で渡し直す。
- **失敗の message**: 自分で書いた文（OpenSSH の文に寄せた所は利用者や script が見分ける「Permission denied」「Host key verification failed.」「Warning: Permanently added」「Received disconnect from」「Connection closed by」「Unable to negotiate … no matching … found」）。初回の交換で自分が切った理由は transport の出力に積まれた平文の DISCONNECT（最初の NEWKEYS の前は暗号化されない、RFC 4253 §6・§11.1）から読む（transport は自分の側の説明を field に残さないため。`have_session_id` の後は読まない）。「no matching kex_algorithms」などの kind を「key exchange method」「host key type」「cipher」「compression method」に言い換える。
- TCP_NODELAY を常に付ける。SIGPIPE は無視（write の誤りとして扱う）。`struct ssh_client` は大きい（transport・受けの buffer）ので main の file の static。

### header についての所見（header の不備ではない、変更の案も無い）

- `ssh_channel_consumed` は size を数えず window の残りだけで WINDOW_ADJUST を出す。header の「consumed した時に呼ぶ」の約束どおりに、呼び手が queue の量で呼ぶ時を絞った（上の I/O）。
- `struct ssh_transport` は自分の側の失敗の説明を持たない（`reason` は相手の DISCONNECT）。client は出力の平文から読んだ（上）。

### 未実施・残り

- password の**成功**の経路は未確かめ（root でない host の sshd は shadow を読めず password を確かめられない）。誤った password・BatchMode・問いと echo は確かめた。相手 B（自前の sshd、p018）で確かめる。
- 既定の設定 file（`~/.ssh/config`・`/etc/ssh/ssh_config`）の読みは試験で流していない（host の `/etc/ssh/ssh_config` は Debian の Include などを持ち、試験は `-F` だけを使う）。`-F` の file の読みと Host block は確かめた。
- IPv6 の接続、名前解決の失敗、connect の ConnectTimeout（相手が黙って SYN を捨てる場合）は host の試験で作れず未実施。
- zedBSD の build（Makefile・libcrypto-compat の link）は p021、QEMU は p022（未実施）。check-skeleton の 7 番目で zedBSD の header と clang で compile が通ることだけ確かめた。
- **試験の副作用（Q1 に報告済み）**: 上の「host の account への影響と守り」。`/home/awe/.bash_history` に 3 行が足された。file は直していない。