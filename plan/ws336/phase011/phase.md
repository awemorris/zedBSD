<!-- awesome-plan project=zedbsd record=ws336-p011 -->

# ws336-p011: ssh-config（sshd_config・ssh_config・-o）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p006
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-config.h`
- `plan/ws336/design.md` §4.2・§10（今の sshd_config・ssh_config の行）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-sshd_config.5`（SHA-256 `c6ff3f09207fc448…`、全桁は同じ directory の SHA256SUMS）: 各 keyword の値の形、時間の書き方（TIME FORMATS）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-ssh_config.5`（SHA-256 `6ef2f7058a6b7be9…`、全桁は同じ directory の SHA256SUMS）: Host の block、最初の値が勝つ、各 keyword
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-config.c`（「受けて無視する」keyword の一覧を design §10 と合わせて置き、結果に一覧を書く）
- `plan/ws336/tests/unit/test-config.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- 今の `userland/packages/network/openssh/files/sshd_config` と `ssh_config` の全行が通る（HostKey の rsa は 2 つ目の HostKey として入る。sshd が型で飛ばす）
- 各 keyword の値の範囲、yes/no の誤り、LoginGraceTime の `2m`・`1h`、MaxStartups の `10:30:100`、Subsystem の sftp 以外で EINVAL、Match で EINVAL、未知の keyword で `line N:`
- ssh_config: Host の pattern で当たる・外れる・否定、最初の値が勝つ、`-o` が file より先に効く（呼ぶ順）、UserKnownHostsFile `/dev/null`
- 引用符の値、`=` の形、tab、CR LF、長すぎる行
- 乱数の入力

## 4. 注意

- 「受けて無視する」keyword の一覧の候補: sshd は UseDNS・PrintMotd・PrintLastLog・UsePAM（no のときだけ）・X11Forwarding（no）・AllowTcpForwarding（no）・Banner（none）・AcceptEnv（値は無視して LANG・LC_*・TERM だけ）・Compression（no）、ssh は VerifyHostKeyDNS・HashKnownHosts（no）・SendEnv・ForwardAgent（no）・ForwardX11（no）・Compression（no）・AddKeysToAgent（no）・LogLevel・ControlMaster・ControlPath・ControlPersist（client の多重化は持たないので、yes の時は誤りにせず 1 本ずつ接続する）。値が機能を有効にする物（`X11Forwarding yes` など）は EINVAL にする。決めた一覧を結果に書く。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh config`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
