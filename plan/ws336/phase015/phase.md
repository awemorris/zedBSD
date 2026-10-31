<!-- awesome-plan project=zedbsd record=ws336-p015 -->

# ws336-p015: ssh の program と host の相互運用（相手 A: host の OpenSSH sshd）

Status: planned（2026-11-01 p001 の設計で作成）
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

（未着手）
