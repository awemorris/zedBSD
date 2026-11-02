<!-- awesome-plan project=zedbsd record=ws336-p018 -->

# ws336-p018: sshd の listener と相互運用（相手 B）

Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-02 14:36 P6 q1965。2026-11-02 16:25 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p015・p017
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/sshd.h`（MaxStartups の pipe、SIGCHLD）
- `plan/ws336/design.md` §4.2・§11.3
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-sshd.8`（SHA-256 `74dde0667a4c99e6…`、全桁は同じ directory の SHA256SUMS）: option -D・-f・-p・-t・-d
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/sshd-main.c`
- `plan/ws336/tests/interop-host.sh` に相手 B と自前どうしを足す
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- design §11.3 の相手 B の全項目（host の OpenSSH の ssh からの exec・`-tt`・ControlMaster 3 session・PQ の警告が出ない・KexAlgorithms と Ciphers の拒否の後も sshd が生きる、hostkeys-00 が無くても OpenSSH が困らない、`ssh -X`・`-A` を付けても x11-req・auth-agent-req が FAILURE で session は続く）
- 自前の ssh → 自前の sshd
- MaxStartups: 認証前の接続を 11 本開くと 11 本目が即閉じ、1 本が login すると次が受けられる
- `sshd -t` が設定の誤りを `line N:` で出し 1 で終わる

## 4. 注意

- listener の descriptor は全部 close-on-exec。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、library の module の .c（ssh-*.c）、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/interop-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 16:25（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main 800c03c401、P5 の ssh（p015）を含む）。凍結の header（`ssh-*.h`・`sshd.h`）は変えていない。

### 作った物・変えた物

- `userland/base/ssh/sshd-main.c`（骨格の置き換え）: `sshd [-D] [-e] [-t] [-f config]`。設定（既定 `/etc/ssh/sshd_config`）を読み、誤りは `sshd: <file>: line N: …` を stderr に出して 1。HostKey は順に読み、最初の ssh-ed25519 の秘密鍵を使う（他の型は log して飛ばす、無ければ「no ssh-ed25519 host key」で 1）。`-t` はここで 0。ListenAddress と Port で listen（AddressFamily inet6 なら IPv6）、接続ごとに monitor を fork（`sshd_monitor_run_program`、子は自分の path から起こす）。MaxStartups は monitor ごとの pipe で数え、認証前の接続が MaxStartups あれば新しい接続を accept の後すぐ閉じる。SIGCHLD で reap、SIGTERM・SIGINT・SIGHUP で accept を止めて終わる（monitor は続く）。descriptor は全部 close-on-exec。最初の引数が `-Zpreauth`・`-Zsession` ならその子として動く。
- `userland/base/ssh/sshd-preauth.c`・`sshd-session.c`: 接続の 1 回の read を 4096 bytes まで（packet 層が未完の packet の横に持つ余裕。16 KiB だと、未完の大きな packet の横で `ssh_transport_input` が EMSGSIZE を返し得て、それを誤りとして接続を切っていた）。session の process が接続を誤りで終える時に理由を log に出す。
- `userland/base/ssh/sshd-session.c`（Q1 の OK、2026-11-02）: session の process（user になった後）が user の shell を `access(X_OK)` で実行できないと分かる時、shell・exec・subsystem を CHANNEL_FAILURE にし、理由を log に出す。check の後に shell が消えた時は今までどおり子が 127 で終わる。
- 試験の道具: `plan/ws336/tests/sshd-host-shim.c`（新、interop の自前の sshd の host build にだけ link: `getpwnam_r` は試験の account だけを答え、home と shell は sshd の binary の directory の `home`・`shell.sh`、`syslog`・`vsyslog`・`openlog` は同じ directory の `log` へ）。`plan/ws336/tests/interop-host.sh` に相手 B と自前どうしを足した（相手 A の確かめは `peer_a` にまとめ、`INTEROP_PART=b` で B だけを流せる。変異の確かめ用）。全体が 110 秒を越えるようになったので、相手 A の黙る listener と閉じる listener の寿命を 110 秒から 890 秒にした（最後の「port がまだ試験の物か」の確かめが、寿命の尽きた listener で誤って FAIL していた。止めるのは今までどおり最後の stop_servers）。

### 試験の作り（host を触らない、fail closed）

- 自前の sshd は sshd-main.c をそのまま（listener・monitor・pre-auth・session の全部が本物）、試験の shim だけを足して build。試験の account の home と shell は run directory の中、shell は `-c` の command を `/bin/sh -c` で動かすだけで login shell は印を出して終わる。host の account の login shell・home・`~/.ssh`・`/etc/ssh` には触れない。log は run directory の file。
- sshd を起こした後、その port の listen の socket が起こした process（の木）の物であることを確かめてから client を向ける（違えば FAIL で止める）。最後にもう一度確かめる。全部の server・agent・ControlMaster は最後に止める。
- OpenSSH の ssh・scp・sftp は `env -i`、`-F /dev/null`、試験の鍵と known_hosts。自前の ssh は相手 A と同じく HOME を run directory に。
- 期待の値は試験の data（cmp）、sshd(8) の known_hosts の形、仕様（RFC 4254）と server の約束から。OpenSSH は相手で、期待の値の出所ではない。

### 試験の中身（相手 B と自前どうし、全 143 checks の中の B の 53）

- `sshd -t`（各 30 秒の上限）: 使える設定は 0 で何も出さない、未知の keyword は 1 と `line 3:`、使える HostKey の後の未知の keyword も 1、operand があれば 1、RSA の HostKey の後の ssh-ed25519 は 0、RSA だけなら 1 と「no ssh-ed25519 host key」。
- OpenSSH の ssh → 自前の sshd: exec と exit 3、post-quantum の警告が出ない、known_hosts の `[127.0.0.1]:port ssh-ed25519 …` の行、signal で 255、8 MiB の stdin の往復の cmp、`-tt` で `stty size` が 37 123 と `/dev/pts/`、ControlMaster の 1 接続に 3 session、scp（既定の SFTP mode）の 8 MiB の上りと下りの cmp、`sftp -b` の mkdir・put・rename・ls・get、`-o KexAlgorithms=curve25519-sha256` と `-o Ciphers=aes128-ctr` が 255 で、その後も sshd が答える、`-X`（DISPLAY=:99）で「X11 forwarding request failed」と session が続く、`-A`（試験の ssh-agent）で client が agent forwarding を頼み（auth-agent-req@openssh.com）、session は SSH_AUTH_SOCK 無しで続く、shell の無い account で「exec request failed」と 255、server の log に理由。hostkeys-00 を送らなくても OpenSSH の ssh は全部の case で困らない（警告も誤りも無い）。
- 自前の ssh → 自前の sshd: exec と exit 4、8 MiB の往復の cmp、signal で 255、`-tt` の大きさ、exec の拒否で「exec request failed on channel 0」と 255（p015 の未実施の `ses-exec-fail` の場合）。
- MaxStartups（既定 10）: 何も送らない接続 10 本はそれぞれ識別の行を受け、11 本目はすぐ閉じる。1 本を離して自前の ssh で login し（6 秒の command）、login の後に 1 本受けられ、次はまた閉じる。その後も普通に login できる。
- listener: 全部の接続の後、listener の descriptor の数が起動の直後と同じ（monitor に渡した接続・pipe を持ち続けない）、monitor が log に peer（`127.0.0.1 <port>`）を書く、session の途中で listener に SIGTERM を送ると port は接続を受けなくなり（monitor が listen の socket を持たない）、session は終わりまで続く。
- どの自前の sshd の process にも ASan・UBSan の報告が無い。

### 実行した command と最後の行

- `sh plan/ws336/tests/interop-host.sh` → `interop-host: 143 checks, 0 failed` / `interop-host: PASS`（約 110 秒）
- `sh plan/ws336/tests/sshd-pair-host.sh` → `sshd-pair-host: PASS`（session の変更の後）
- `sh plan/ws336/tests/run-unit.sh all` → 18 本（auth・buf・channel・cipher・client・common・config・crypto・hostfile・kex・key・monitor・packet・privsep・sftp・tools・transport・ttymodes）が全部 PASS、最後の行 `run-unit: PASS`
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p018-mut/mut.py` と `mutants.py`。変異ごとに `userland/base/ssh` の写しを作り、その中の sshd-main.c か sshd-session.c を 1 か所変えて、`SSH_SOURCE_DIR=<写し> INTEROP_PART=b sh plan/ws336/tests/interop-host.sh` を流す（worktree の file は変えない）。RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s、150 s と RSS 6 GB で kill、同時 3。150 s で kill された物は run.log に FAIL の行があれば捕まえたと数え、その行が変異の場所の確かめであることを読んで確かめた。0.0.0.0 に listen させる変異は host の外に port を開くので作らなかった。

- 1 回目（23 通り）: 16 が FAIL、7 が生き残り。試験を足した: 使える HostKey の後の誤り（`-t` が誤りを見ない変異）、`sshd -t` に 30 秒の上限（`-t` を無視して listen を始める変異が試験を止めていた）、listener の descriptor の数、log の peer、listener を止めた後の port と session。
- 途中の 2 回（2 回目は子の process を止めた、3 回目は baseline の scp が 1 度だけ「Connection reset by peer」で落ちた。上の 4096 bytes の直しと理由の log を入れ、4 本並べた B だけの run を 8 回と全体を 3 回流して再現しないことを確かめた）。
- 最後（4 回目、新しい directory、同時 3）: **23 通り全部 FAIL**（うち 9 は 150 s の kill で、全部その変異の確かめの FAIL の行がある）、生き残り 0。

結果は `build/tmp/p018-mut/run1.log`〜`run4.log` と `results.json`・`r4-results.json`。

### 自分で決めた点

- **option**: `-D` は受けて何もしない（常に foreground。service の `sshd -D` のため）、`-e` は log を stderr にも（`LOG_PERROR`）、`-t`・`-f`。他の option と operand は usage と 1。`-p` などは無い。
- **HostKey**: 最初の ssh-ed25519 だけを使う。他の型（image の sshd_config の RSA）は log して飛ばし、無い file も飛ばす。他の誤りは stderr に出して次へ。
- **子の program の path**: `realpath(argv[0])` が取れればそれ、取れなければ `SSHD_PROGRAM`。root が起こすので argv[0] はその人の選んだ物。
- **MaxStartups**: 数えるのは MaxStartups の pipe がまだ開いている monitor（byte が来るか、monitor が閉じれば数えない）。table（1024）が埋まっても閉じる。閉じた接続は log に残す。
- **終わり方**: SIGTERM・SIGINT・SIGHUP で listen を止めて 0 で終わり、monitor（接続）はそのまま続ける。
- **root でない mode**: log に「not running as root: serving only uid N」。
- **shell の確かめ**（Q1 の OK）: 上の作った物を参照。

### 未実施・残り

- root での listener（`/var/empty`、`sshd` user、PermitRootLogin、shadow の password）と IPv6 の listen は host で試していない（p022 の QEMU）。
- SIGHUP で設定を読み直すことはしない（終わるだけ）。`ListenAddress` は 1 つ（ssh-config.h の約束）。
- sftp の subsystem は host の `/usr/lib/openssh/sftp-server` で試した（自前の sftp-server は p019）。
- 規約の全文の見直しは p025。ここでは coding-style §14 で自分の diff を見直した。
- 消す物（Q1 に頼む）: `build/tmp/p018-mut/`（変異の写しと各 interop の出力）、`build/tmp/interop-host.run.*` と `build/tmp/sshd-pair.run.*`・`build/tmp/ssh-unit.run.*` の古い run。
