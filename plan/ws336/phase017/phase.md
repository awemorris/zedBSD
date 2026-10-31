<!-- awesome-plan project=zedbsd record=ws336-p017 -->

# ws336-p017: sshd の pre-auth と session の process（sshd-preauth.c・sshd-session.c）

Status: planned（2026-11-01 p001 の設計で作成）
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

（未着手）
