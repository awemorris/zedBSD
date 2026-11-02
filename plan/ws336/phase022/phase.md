<!-- awesome-plan project=zedbsd record=ws336-p022 -->

# ws336-p022: QEMU の相互運用（root の sshd と privsep）

Status: in-progress（2026-11-02 21:15 P6 q1991。2026-11-02 23:25 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p021
WS335 の待ち: **WS335 の実装**（p021 と同じ）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/ws336/design.md` §11.4
- `plan/tools/guest/`（guest.py・config-amd64-ssh.mk）
- `plan/tools.md` の「QEMU の解析」
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `plan/ws336/tests/config-amd64-ssh.mk`（`plan/tools/guest/config-amd64-ssh.mk` を元に openssh を自前の 4 つに替える）
- `plan/ws336/tests/qemu-interop.sh`（1 本の script: image の build → 起動 → 試験 → 回収 → 要約。最後の行 `qemu-interop: PASS`）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- image の build（警告 0）と `plan/tools/boot-test.sh`
- design §11.4 の全項目（root の sshd、鍵と password の login、scp、ControlMaster、`-tt`、re-key、guest の ssh から host の OpenSSH sshd と guest 自身へ、pre-auth の uid 22 と root が /var/empty、session が user の uid）
- `sshd-start` の初回の host key の作成（guest.py の鍵を入れない image で）

## 4. 注意

- QEMU の console・serial の log で判定しない。起動しない・固まる時は gdbstub・QMP（AGENTS.md §9）。QEMU の名前・port・directory は Q1 の割当。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての library の header、HAL・toolchain・共有の `build/`、OpenSSH の package、既定の config、他の WS の file。

## 6. 確かめの command

- `sh plan/ws336/tests/qemu-interop.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 23:25（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main 8e12d77f2b）。ssh の library・program の source、header・HAL・toolchain・OpenSSH の package・既定の config は変えていない。

### 作った物

- `plan/ws336/tests/config-amd64-ssh.mk`: `plan/tools/guest/config-amd64-ssh.mk` と同じく `plan/ws035/tests/config-amd64-userland.mk` を基にし、`openssh`・`openssl` を外して `libcrypto-compat ssh sshd ssh-keygen sftp-server` を足す。
- `plan/ws336/tests/qemu-interop.sh`（1 本の script: image の build → 起動 → 試験 → 回収 → 要約、最後の行 `qemu-interop: PASS`）と `qemu-helper.py`（port の持ち主の確かめ・port の待ち・鍵の交換の前の接続を保つ・端末で走らせる）。
  - image は 2 つ（`plan/tools/guest/test-image.sh`、同じ BUILD）: keyed（guest の harness の root の authorized_keys・net.conf・3 種の host key）と fresh（authorized_keys と net.conf だけ）。image の file はその run で写した物を渡す（BUILD の root の木の cache は file が新しい時だけ作り直すので、2 つ目の image の後の 1 つ目が詰め直されない事が 2 回目の run で分かったため）。keyed の guest に harness の rsa・ecdsa の鍵がある事も確かめる。
  - guest は `plan/tools/guest/guest.py`（KVM、NVMe、run の中の `GUEST_RUNTIME`、guest.py が選ぶ 127.0.0.1 の空き port。forward の port が自分の QEMU の pid の物かを `/proc/net/tcp` で確かめ、違えば FAIL）。判定は SSH の結果と script の要約だけ（console・serial の log は読まない）。起動の確認は `plan/tools/boot-test.sh`（`build/tmp/qemu-interop/boot-test/login.png`）。
  - host の test の sshd: host の OpenSSH sshd をこの user で 127.0.0.1 の空き port に（一時の設定・host key・authorized_keys、`ForceCommand eval`・`UsePAM no` など interop-host と同じ形）、test 自身の process かを確かめ、guest からは 10.0.2.2 で。
  - `QEMU_INTEROP_PARTS`（boot・transfer・fresh）で任意の部分を選べる（変異の確かめ用）。
- **libc の ttyname の修正**（`userland/base/libc/posix.c`、Q1 の OK で p022 に入れた）: `ttyname_r` が `/dev` を `/dev/pts` より先に探し、`/dev/stdin`（記述子と同じ inode）を先に見つけるため、pty の session の `tty` が `/dev/stdin` を出していた（自前の sshd の -tt で見つけた。OpenSSH の session でも同じはず）。`/dev/pts` を先に探し、`/dev` を探す時は記述子の別名（`stdin`・`stdout`・`stderr`・`fd`）を飛ばす（`ttyname_is_alias`）。誤りの値の扱いは前と同じ。`make BUILD=build/p022 build/p022/dynamic/libc.so` は rc 0・warning 0（make は worktree の sysroot の複写の `libc.a` も作り直した: sysroot.mk の入力に posix.c がある）。

### 実行した command と最後の行

- `sh plan/ws336/tests/qemu-interop.sh`（最後の run、23:24 に終わり）→ `qemu-interop: 53 checks, 0 failed` / `qemu-interop: PASS`。項目:
  - image: keyed・fresh とも build し compiler の warning 0（最初の BUILD からの全体の build（10 分 22 秒）も warning 0、`gmake` の jobserver の注意の 1 行だけ。OpenSSL は build されない）、boot-test が login prompt を撮る。
  - host の OpenSSH から guest の root の sshd へ: 識別の行 `zedBSD_1.0`・`mlkem768x25519-sha256`・ssh-ed25519 の harness の host key、image に OpenSSH の program が無い。root の鍵の login、user（1000）と root（PermitRootLogin yes）の password の login（shadow）、違う password と無い user の拒否。scp（SFTP）8 MiB の往復の cmp、`sftp -b` の mkdir・put・rename・ls・get、ControlMaster で 3 session、`-tt` で client の大きさ（37 123）・端末・`tty` が `/dev/pts/0`・その間の `who` に root の pts/0（LOGIN_RECORD、/dev/system の親の確かめを通る）・終わった後は無い（記録が閉じる）、RekeyLimit 1M で 64 MiB の往復と鍵の交換 117 回。
  - privsep: 鍵の交換の前で止めた接続の `-Zpreauth` の sshd が uid 22、親の monitor が uid 0、`fuser -u /var/empty` に `r`（root の directory が /var/empty）。user の session の `-Zsession` の sshd が uid 1000（root の session は 0）。
  - guest の ssh: guest の ssh-keygen の鍵で host の OpenSSH sshd へ（exit 7・出力・known_hosts に host の key・1 MiB の往復の cmp）、guest 自身の sshd へ（`self 0`）。
  - fresh: sshd-start が最初の起動で `/etc/ssh/ssh_host_ed25519_key`（0600）と .pub だけを作り（`/etc/ssh` は ssh_config・sshd_config とその 2 つ）、server がその鍵を出し、harness の鍵ではない。
- `sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p022-mut/mut.py`。image は tree から作るので、1 通りずつ tree の file を一時に変えて qemu-interop.sh を流し（必要な部分だけ、1 回約 9 分）、写しから戻して `cmp` で確かめ（新しい時刻で戻し、次の build が compile し直す）、git の作業の木に差分が残らない事を確かめた。

- FAIL した物（7）: pre-auth の chroot を外す（fuser の r）、pre-auth の uid を 1 ずらす（接続できない）、login record の親の確かめを逆にする（who に記録が無い）、session が user にならない（user の login と session の uid）、`PermitRootLogin prohibit-password`（root の password）、sshd-start の `ssh-keygen -A` を飛ばす（fresh の guest に接続できない）、ttyname の 2 つの直しを両方戻す（`tty` が `/dev/stdin`）。
- 生き残り（2、同値）: ttyname の別名の飛ばしだけを外す・探す順だけを戻す。どちらか一方が残れば pty の名前は正しい（2 重の守り）。両方を戻すと FAIL（上）。
- 結果は `build/tmp/p022-mut/run1.log`・`run2.log`・`run3.log`、各 `.log`・`results*.json`。

### 自分で決めた点

- host key の初回の作成は、harness の鍵を入れない 2 つ目の image（fresh）で確かめる。
- privsep の root の directory は zedBSD の `fuser -u` の `r` で、uid は `ps -A -o pid,ppid,uid,args`（自分を拾わないよう awk の語を分けた）で見る。
- guest から host の sshd へは QEMU の user net の 10.0.2.2（host の 127.0.0.1 に届く）。host の sshd は 127.0.0.1 だけで listen する。

### 未実施・残り

- `PermitRootLogin no` と `prohibit-password` の拒否の確かめ（既定の yes だけを試した）。
- 実機は p024 の後（ユーザーが拾える試験の一覧へ）。他の arch は p023。
- 規約の全文の見直しは p025。C の変更は libc の ttyname だけで、coding-style §14 で見直した（宣言は関数の先頭、条件の中で関数を呼ばない）。
- 消す物（Q1 に頼む）: `build/p022/`（試験の BUILD、image を含む）、`build/tmp/qemu-interop.run.*`（guest の disk の写しを含む）、`build/tmp/p022-*`（mut の写しと log、run の log）、`build/amd64/sysroot/usr/lib/libc.a` は libc の修正で作り直された（main の複写から変わった。消すか main の物で置き換えるかは Q1）。
