<!-- awesome-plan project=zedbsd record=ws336-p016 -->

# ws336-p016: sshd の monitor と login の確かめ（sshd-monitor.c・sshd-login.c）

Status: planned（2026-11-01 p001 の設計で作成）
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

（未着手）
