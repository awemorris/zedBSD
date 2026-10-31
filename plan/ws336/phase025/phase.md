<!-- awesome-plan project=zedbsd record=ws336-p025 -->

# ws336-p025: 規約の全文の見直し

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: conformance-reviewer
依存: p024
WS335 の待ち: 無し

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/coding-style.md` の全文
- この WS が足した全 source（`userland/base/ssh/`、`plan/ws336/tests/` の C）
- 各 Phase の結果の「header の持ち越しの直し」
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- 書き方だけの修正（動作は変えない）。実装の間に見つかった header の comment の誤りの直し（この Phase でだけ header を変えてよい。BASE は新しい SHA になる）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- `run-unit.sh all`・`crypto-contract.sh`・`interop-host.sh`・`sshd-pair-host.sh`・`check-skeleton.sh`（BASE なし）・p022 の `qemu-interop.sh` が見直しの前後で PASS

## 4. 注意

- 無し
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

HAL・toolchain・共有の `build/`、他の WS の file。動作を変える修正は別の Phase に回す。

## 6. 確かめの command

- `sh plan/ws336/tests/interop-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
