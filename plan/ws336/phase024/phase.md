<!-- awesome-plan project=zedbsd record=ws336-p024 -->

# ws336-p024: OpenSSH からの入れ替え

Status: cleared（2026-11-14 22:3x Q1。build system の作り直しの後、全機種の既定の config が自前の ssh を選び OpenSSH を選ばない形になり、入れ替えは済んでいた。Pi 5 の自前の sshd を host の OpenSSH で確かめた。下の「結果（Q1、2026-11-14）」）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p022・p023
WS335 の待ち: **WS335 の実装**。Q6 は A（p022 が通ったら既定の config をまとめて替える、OpenSSH の package は残す、2026-11-01）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/ws336/design.md` §10・§11.5
- [user-questions.md](../user-questions.md) の Q3・Q6 の答え
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- 既定の config（Q6 の範囲。`config/ci/config-amd64-server.mk` はユーザーの物なので Q1 に頼む）
- 自前の package の menu を `base`・既定 y に
- Q3 の A に従い `scp -O` を使う 4 本（plan/tools/gpu-perf/measure.sh、plan/tools/rtx-video/rtx-*-check.sh）から `-O` を外す（他の WS の道具なので Q1 の承認の後）
- 実機の試験の一覧（Q1 がユーザーの拾える試験の一覧に置く）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- amd64 の既定の image で `plan/tools/guest/guest.sh` の start・wait・run・put・get が通る
- `plan/tools/ssh-rtt/qemu-ssh-rtt.sh` が PASS（SSH の往復の回帰）
- `plan/tools/tcp-close/qemu-close.sh` が PASS
- 他 arch の既定の config の build（警告 0）

## 4. 注意

- OpenSSH の package は tree に残す（消すのは別の判断）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての library の header、HAL・toolchain・共有の `build/`、ユーザーの物（`config/ci/config-amd64-server.mk`）。

## 6. 確かめの command

- 上の 3 本の script
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果（引き継ぎ）

2026-11-03 00:05（host の `date`）。**途中**（Q1 のラップアップの指示で区切った。実装済み・試験 PASS ではない）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main 5fb95f5669）。

### 範囲（2026-11-02 Q1 の決め）

- `config/ci/*` はこの Phase では変えない（WS343 がユーザーの WS として、OpenSSH・OpenSSL・curl・libcurl を CI の config から外し、自前の ssh・sshd・libssl.so・libcrypto.so を base に入れる）。p024 は入れ替えの準備（同じ path・service を出す package、rootfs の規則、OpenSSH の package との衝突、host key の引き継ぎ）を作り、自分の試験の config で確かめ、`config/ci` の差分の案を下に書く。`config/ci/config-amd64-server.mk` はユーザーの物で、案にも入れない。
- 自前の package は既定 n のまま（衝突の規則があるので、openssh を選ぶ config/ci が WS343 の変更の前に build できなくなるため）。既定 y は config/ci の変更と一緒に WS343 で（案の 2）。
- `scp -O` の 4 本の道具は Q1 の OK で直した。

### 済んだ事

1. **sshd の log の行を OpenSSH の形に**（`userland/base/ssh/sshd-monitor.c`、header の変更なし）: `plan/tools/ssh-rtt/qemu-ssh-rtt.sh`（この Phase の確かめの 1 つ）が `/var/log/messages` の "Accepted publickey" を数えるのに、monitor は "Accepted login for USER" を書いていた（design.md の sshd の行と §8 の約束は OpenSSH と同じ形）。今は "Accepted publickey for U from A port P ssh2: ED25519 SHA256:…"・"Failed publickey …"・"Accepted password for U from A port P ssh2"・"Failed password …"。peer は monitor の process ごとに 1 度だけ置く file-scope の文字列（`sshd_monitor_run_program` が context の peer を log に出せる形で写す）。peer の無い時（試験の monitor）は from・port を省く。test-monitor（1455 checks、受け入れの行の check を足した）・sshd-pair-host・interop-host（数える語）を新しい行に合わせた。
2. **package**（`userland/base/ssh/Makefile`）: 4 つを menu `base` に（既定 n のまま）。`openssh` と自前のどれかを両方選ぶ config は make の始めに止める（同じ file を置くため、`$(error … choose one)`）。`config/ci/config-amd64.mk` に sshd を足した config で止まる事を確かめた。
3. **armv7 の root**（`platform/armv7/userland.mk`・`rootfs.mk`）: 自前の 4 つを選ぶと root の command（`ARMV7_SSH_COMMANDS`、自前の link の規則で、basic の規則から外す）と runtime の `libcrypto-compat.so` に入り、sshd-start が使う `chmod mkdir mv` も（OpenSSH の時と同じく）入る。`make ZEDBSD_CONFIG=plan/ws336/tests/config-armv7-ssh.mk BUILD=build/p024-zybo build/p024-zybo/root.img` → rc 0・warning 0、root に ssh・sshd・ssh-keygen・sftp-server・libcrypto-compat.so・sshd_config・service・sshd-start・chmod・mkdir・mv、4 MiB の root に入った（OpenSSH の時の 16 MiB は要らない）。
4. **host key の引き継ぎ**: 自前の sshd-start は ed25519 の鍵が無い時だけ作り、ある鍵（OpenSSH の時に作られた rsa・ecdsa・ed25519）には触らない。自前の sshd は ed25519 の鍵だけを使い、rsa などの HostKey の行は読み飛ばす（p016・p019 で確かめ済み、interop-host の相手 B は rsa の行が先の設定で動く）。だから OpenSSH の image から入れ替えても既存の ed25519 の host key がそのまま使われ、client の known_hosts は変わらない（OpenSSH の client が ed25519 を選んでいた場合）。
5. **`scp -O` を外した**（Q1 の OK、SFTP mode。OpenSSH の guest でも動く）:
   - `plan/tools/gpu-perf/measure.sh:97` `timeout 120 scp -O -i "$KEY" $ZSSHOPT "$@" && return 0` → `scp -i …`（:94 の comment も「old SCP protocol」→「its SFTP mode」）
   - `plan/tools/rtx-video/rtx-s5-check.sh:70`、`rtx-video-check.sh:67`、`rtx-notes-check.sh:57`・`:60`: 同じく `scp -O -i` → `scp -i`
   - 4 本とも実機（RTX の機械）が要り、host の自己試験や dry run は無い。`sh -n` だけ通した（未実施: 実機での実行）。
6. **試験の config**（`plan/ws336/tests/`）: `config-amd64-ci-ssh.mk`・`config-rpi5-ssh.mk`・`config-odroid-c5-ssh.mk`（新）、`config-arm64-ssh.mk`（rpi4）・`config-riscv64-ssh.mk`（visionfive2-keiland）は openssh だけを外すように直した（OpenSSL は curl のために残る）、`config-armv7-ssh.mk`（zybo、openssh と openssl を外す）、`config-amd64-tcp-ssh.mk`（qemu-close.sh 用、ws221 の tcp の config の自前版）。7 つとも make が読める（`list-user-programs` rc 0）。
7. **1 本の script** `plan/ws336/tests/replace-check.sh`（書いた、**まだ流していない**）: amd64 の既定の image と TCP の image を build（warning 0）、guest.sh の start・wait・run・put・get（1 MiB の往復）と server が自前で OpenSSH の program が無い事、qemu-ssh-rtt.sh と qemu-close.sh の PASS と "Accepted publickey" の数、他の arch（rpi4・rpi5・odroid-c5・visionfive2-keiland・zybo）の image の build と root の中身。最後の行 `replace-check: PASS`。
8. amd64 の既定の image（`plan/tools/guest/test-image.sh plan/ws336/tests/config-amd64-ci-ssh.mk build/p024-amd64`）: 10 分 31 秒、rc 0。compiler の warning 15 行は全部 OpenSSL（curl のために残る外部の package、`../src/crypto/…`）の物で、自前の code は 0。

### 確かめた command と最後の行

- `sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`（test-monitor 1455 checks）
- `sh plan/ws336/tests/sshd-pair-host.sh` → `sshd-pair-host: PASS`（新しい log の行で）
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし
- zybo の root.img（上の 3）、amd64 の既定の image の build（上の 8）

### 残り（次の担当へ）

1. `sh plan/ws336/tests/replace-check.sh` を流す（guest.sh・qemu-ssh-rtt・qemu-close・他 arch の image）。期待: 最後の行 PASS。amd64 の image の build で OpenSSL の warning が数えられて FAIL になるので、`warnings` を OpenSSL（外部の package の `../src/`）を除いて数えるか、OpenSSL を外した後（WS343）に流すかを決める（OpenSSL の warning は今の main の既定の image でも出る）。
2. interop-host.sh を流し直す（log の行の語を変えた。sshd-pair-host は PASS）。fuzz-host は monitor の log だけの変更なので要らないはず（未実施）。
3. 壊す確かめ（log の行の形、衝突の規則、armv7 の root の規則）と coding-style §14 の見直し（sshd-monitor.c の変更は §14 で見た: 宣言は先頭、呼び出しに comment、条件の中で関数を呼ばない）。
4. 実機の試験の一覧（Q1 がユーザーの拾える試験の一覧に置く）: 5330（NVMe の zedBSD）・VF2・ZYBO・RPi で、自前の sshd の image に入れ替えた後、今の道具（sshpass・scp・`plan/tools/aat`・`plan/tools/hw5330/5330-run.sh`・`plan/tools/zybo/deploy.sh`）がそのまま通る事、既存の host key が引き継がれ known_hosts が変わらない事。
5. phase.md の「作る物」の既定の config の変更は WS343 で（下の案）。

### config/ci の差分の案（WS343 が当てる）

`config/ci/config-amd64-server.mk` は案に入れない（ユーザーの物。同じ 1 語の置き換えが当てはまる）。

1. 次の 7 つの `ZEDBSD_USER_PROGRAMS :=` の行で、語 `openssh` を `libcrypto-compat ssh sshd ssh-keygen sftp-server` に置き換える（`openssl` は curl・libcurl が要るので、それらを外す WS343 の同じ変更で外す）:
   `config/ci/config-amd64.mk:57`、`config-rpi4.mk:57`、`config-rpi4-server.mk`、`config-rpi5.mk:74`、`config-odroid-c5.mk:67`、`config-odroid-c5-keiland.mk`、`config-visionfive2-keiland.mk:59`。
   （確かめ: `sed 's/ openssh / libcrypto-compat ssh sshd ssh-keygen sftp-server /'` で各 file の `openssh` は 0 になる。）
2. `config/ci/config-zybo.mk:56`: ` ca-certificates openssh openssl zlib` → ` ca-certificates libcrypto-compat ssh sshd ssh-keygen sftp-server zlib`（zybo では OpenSSL を使うのは OpenSSH だけ。zlib を残すかは WS343 で: OpenSSH の package の外の利用者を確かめる）。root は 4 MiB に入る（上の 3）。
3. 同じ変更で `userland/base/ssh/Makefile` の 4 つの `ZEDBSD_USER_PROGRAMS` の登録の 4 番目の引数を `n` から `y` に（既定 y）。libcrypto-compat も既定 y にするか（require で選ばれる）は WS343 で。
4. 試験の config（`plan/ws336/tests/config-<board>-ssh.mk`）は上の案と同じ program の集合を作る（順だけ違う）。WS343 は案を当てた後、`replace-check.sh` を config/ci の config で流せる。

### 消す物（Q1 に頼む）

`build/p024-amd64/`・`build/p024-zybo/`・`build/p024-x`（無ければ無し）・`build/amd64/packages/toolchain/` と worktree の `build/packages/openssl`・`build/packages/curl` など（amd64 の既定の image の build が作った外部の package）、`build/tmp/p024-*`、`build/tmp/ssh-unit.run.*`・`ssh-skeleton.run.*`・`sshd-pair.run.*`。

## 結果（Q1、2026-11-14）

### 範囲の書き直し

build system の作り直し（top の Makefile と `mk/platform-<機種>/`）で、上の引き継ぎの前提が変わった。

- 既定の config（`mk/platform-x86_64・rpi4・rpi5・vf2・i386・pc9821/defaults.mk`）は全部が自前の `ssh`（`userland/base/ssh/`、ssh・sshd・ssh-keygen・sftp-server）を選び、`openssh` を選ばない。「作る物」の既定の config の入れ替えは、作り直しで済んだ。
- armv7（zybo）は今の build に platform が無いので、zybo の root の確かめは外す。
- `config/ci/*.mk` は全部が古い形（`ZEDBSD_USER_PROGRAMS`）で、その扱いは [WS343](../../ws343/ws.md) と [BUG-435](../../bugs/BUG-435.md) に任せる。上の「config/ci の差分の案」は使わない。
- `replace-check.sh` は古い変数と config を前提にした物で、流さない。代わりに、すでに自前の sshd で動いている Pi 5 で実機の相互運用を確かめた。

### Pi 5 での確かめ（10.0.30.2、MAC 98:fe:54:22:65:c3、zedBSD aarch64、22:2x）

相手は host の OpenSSH 10.0p2（Debian）の client。known_hosts は Q1 の scratchpad の file。

- banner `zedBSD_1.0`、鍵の交換 `mlkem768x25519-sha256`、host key `ssh-ed25519`、cipher `chacha20-poly1305@openssh.com`。`/usr/libexec/sshd-session` は無い（OpenSSH の program は無い）。`/etc/ssh` は `ssh_host_ed25519_key` と config だけ。
- password の login、公開鍵（ed25519、試験の鍵を authorized_keys に足して確かめた後に消し、q1- の行が 0 である事を確かめた）の login、command の終わりの値（`exit 7` → host の rc 7）、`-tt` の pty。
- `scp`（SFTP の mode）8 MiB の上り（`RekeyLimit=1M` で再鍵を挟む）と下り、`cmp` で一致。guest の sha256 も一致。
- `sftp`: put・get・mkdir・rename・ls・rm・rmdir、get した file が `cmp` で一致。
- Pi 5 の自前の ssh（client）から自前の sshd（127.0.0.1）へ、一時の鍵で `loop-ok`（rc 0）。一時の鍵・known_hosts は消した。

全部 PASS。試験で Pi に置いた file（/tmp/q1-*）は消した。

### 未実施

- interop-host.sh の流し直し（11-03 の log の語の変更の後）、壊す確かめ、coding-style §14 の見直しは p025（規約の全文の見直し）でまとめて行う。
- QEMU の image での guest.sh・qemu-ssh-rtt・qemu-close は、今の担当の QEMU の試験（WS353 p016 などが guest.sh で自前の sshd を使う）で日々通っている事を根拠に、この Phase では流していない。
