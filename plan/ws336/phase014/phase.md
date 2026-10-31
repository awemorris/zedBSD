<!-- awesome-plan project=zedbsd record=ws336-p014 -->

# ws336-p014: ssh-keygen の program

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
- `userland/base/ssh/ssh-key.h`・`ssh-hostfile.h`
- `plan/ws336/design.md` §4.2（ssh-keygen の option）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-ssh-keygen.1`（SHA-256 `d40597a5a1d841a0…`、全桁は同じ directory の SHA256SUMS）: -t・-f・-N・-C・-q・-l・-y・-A・-R の振る舞いと出力の形
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-keygen-main.c`
- `plan/ws336/tests/keygen-host.sh`（host で build して流す 1 本の script。最後の行 `keygen-host: PASS`）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- -t ed25519 -f F -N "" -C c -q: F（0600）と F.pub（0644）、F.pub の行の形、`-l -f F.pub` の指紋の形、`-y -f F` が F.pub と同じ鍵
- -t rsa・-N の空でない値で断る（終了の値 1 と文）、既にある F を上書きしない（`-f` に既存の file、問わずに断る。OpenSSH は問うが、ここは問わない事を結果に書く）
- -A: 一時の root の代わりに `-f prefix` に相当する試験用の環境変数は作らない（試験用の切り替えを製品に入れない、coding-style §12）。-A は p019 の QEMU で確かめる
- -R host: `ssh_known_hosts_remove` で消して `.old` を残す
- 相互: host の OpenSSH の ssh-keygen が作った鍵を -l・-y で読み、自前が作った鍵を OpenSSH の `ssh-keygen -l -f`・`-y -f` が読む（interop は他の実装との相互の確かめで、期待の値の生成ではない）

## 4. 注意

- file は一時の名前で書いて rename（途中の file を残さない）。秘密鍵の mode は 0600 で作る（umask に頼らず open の mode）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/keygen-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
