<!-- awesome-plan project=zedbsd record=ws336-p007 -->

# ws336-p007: ssh-kex（KEXINIT・交渉・hybrid の message・交換の hash・鍵の導出）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p004
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-kex.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4253.txt`（SHA-256 `b471173305154f6c…`、全桁は同じ directory の SHA256SUMS）: §7.1（KEXINIT の形と交渉、first_kex_packet_follows）、§7.2（鍵の導出）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-mlkem-hybrid-kex-10.txt`（SHA-256 `d7e1ff8723466a80…`、全桁は同じ directory の SHA256SUMS）: §2.1・§2.2・§2.5（H の中身、K は string）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-strict-kex-02.txt`（SHA-256 `c08126dcaaf63958…`、全桁は同じ directory の SHA256SUMS）: §3.1（印の名前と組）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-chacha20-poly1305-04.txt`（SHA-256 `a8aa91696db885d2…`、全桁は同じ directory の SHA256SUMS）: §5（MAC の一覧を見ない）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-kex.c`
- `plan/ws336/tests/unit/test-kex.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- kexinit_put: server・client、initial 0・1 の各一覧を `wire.h` で組んだ期待の bytes（cookie の 16 bytes は除いて）と比べる
- kexinit_parse: OpenSSH 10.5 の既定の一覧（myproposal.h の名前の並びを試験の中に文字で書く。他の実装の出力ではなく公開の名前の一覧）を含む KEXINIT を読む、一覧の誤り・余りの bytes・短い payload
- choose: 相手に名前が無い各種類で ENOTSUP と kind、strict の印の 4 通り（標準・旧・両方・無し）、initial 0 では印を見ない、first_kex_packet_follows の当たり・外れ（ignore_next）
- hybrid の message の put・parse の往復と誤り
- hash: 試験の中で `wire.h` で H の入力を組んで `ssh_sha256` した値と一致（各欄の string の符号）
- derive: 1 回目の HASH と 2 回目の連結を試験の中で組んで 64 bytes が一致、letter の範囲・size 0・257 で EINVAL

## 4. 注意

- MAC の一覧は空で送り、相手の MAC の一覧は見ない（design D3）。
- K は 32 bytes の string（uint32 32 と bytes）で H と導出に入る（mpint にしない、draft §2.5）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh kex`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
