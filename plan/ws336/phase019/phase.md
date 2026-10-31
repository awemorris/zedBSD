<!-- awesome-plan project=zedbsd record=ws336-p019 -->

# ws336-p019: sftp-server の program（と Q3 次第で旧の scp の受け手）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p013・p018
WS335 の待ち: 無し。**Q3 の答え次第**（sftp を入れない時は canceled、その時 p020 の依存は p018 になる）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-sftp.h`（SYMLINK の引数の順の注意）
- `plan/ws336/design.md` §4.2・§11.3
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-secsh-filexfer-02.txt`（SHA-256 `3f69664d8050a28a…`、全桁は同じ directory の SHA256SUMS）: §6・§7
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-PROTOCOL`（SHA-256 `4a9e7f6d26d3f59d…`、全桁は同じ directory の SHA256SUMS）: §4.1（SYMLINK の逆順）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-scp.1`（SHA-256 `32966f40938f813b…`、全桁は同じ directory の SHA256SUMS）: SFTP mode と -O
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/sftp-server-main.c`
- （Q3 で旧の scp を入れる時）`userland/base/ssh/scp-main.c`（protocol の扱いは Q1 の判断で design に足す）
- `plan/ws336/tests/interop-host.sh` に scp・sftp を足す
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- host の OpenSSH の scp（既定の SFTP mode）で 8 MiB の往復の cmp、`scp -r` の directory、`scp -p` の時刻と mode
- `sftp -b` で ls・put・get・mkdir・rmdir・rename・rm・symlink（OpenSSH の順で正しい向きの link ができる）・readlink
- 権限の無い file で PERMISSION_DENIED、無い file で NO_SUCH_FILE
- 壊れた packet で落ちない（sftp-server を stdin に random な bytes で 1 万回起こす）

## 4. 注意

- handle は sftp-server の中の表の番号（4 bytes）。開ける数の上限を決めて結果に書く。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、library の module の .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/interop-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
