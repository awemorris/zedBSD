<!-- awesome-plan project=zedbsd record=ws336-p010 -->

# ws336-p010: ssh-ttymodes・ssh-channel（端末の mode と connection protocol）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p002
WS335 の待ち: 無し

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-ttymodes.h`・`ssh-channel.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4254.txt`（SHA-256 `bfc727e9350067ea…`、全桁は同じ directory の SHA256SUMS）: §5（channel・window・data・EOF・CLOSE）、§6（session の request）、§8（端末の mode の opcode の表）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc8160.txt`（SHA-256 `65eefa2fbc0db6bb…`、全桁は同じ directory の SHA256SUMS）: §3（IUTF8 = 42）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-PROTOCOL`（SHA-256 `4a9e7f6d26d3f59d…`、全桁は同じ directory の SHA256SUMS）: §2.1（eow）・§2.2（no-more-sessions）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4250.txt`（SHA-256 `eb3a9d7c3e1fae53…`、全桁は同じ directory の SHA256SUMS）: §4.1.2・§4.3（channel の open の失敗の理由）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-ttymodes.c`（opcode の表は data として block ごとに出典）・`ssh-channel.c`
- `plan/ws336/tests/unit/test-ttymodes.c`・`test-channel.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- ttymodes: 既知の termios（cfmakeraw の後・既定）を put → apply で同じ flag と文字、opcode 160 で止まる、未知の 1〜159 を飛ばす、長さの途中で切れた引数で EINVAL かつ terminal が変わらない、1024 bytes を超えて EINVAL、速度 9600・38400・未知
- channel: open_session → accept_open → open_reply の往復、11 個目で RESOURCE_SHORTAGE、session 以外の型・no_more_sessions で ADMINISTRATIVELY_PROHIBITED、data の window の消費と window の超過・SSH_CHANNEL_PACKET の超過で EPROTO、consumed による WINDOW_ADJUST の量、adjust で 2^32 を越えて EPROTO、EOF・CLOSE の二重で EPROTO、EOF の後の data で EPROTO、closed で slot が戻る
- request の各型の parse（`wire.h` で組む）と put の往復、長さの上限 +1 で EINVAL、未知の request は SSH_CREQ_OTHER と name
- global_request: keepalive@openssh.com が want_reply で REQUEST_FAILURE、no-more-sessions が印を立てる
- 乱数の入力

## 4. 注意

- ttymodes の表の flag や文字が `<termios.h>` に無い時は `#ifdef` で飛ばす（zedBSD と Linux の host の両方で build する）。表の値は RFC 4254 §8 と RFC 8160 から。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh ttymodes channel`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
