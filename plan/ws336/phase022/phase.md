<!-- awesome-plan project=zedbsd record=ws336-p022 -->

# ws336-p022: QEMU の相互運用（root の sshd と privsep）

Status: planned（2026-11-01 p001 の設計で作成）
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

（未着手）
