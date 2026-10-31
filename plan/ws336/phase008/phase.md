<!-- awesome-plan project=zedbsd record=ws336-p008 -->

# ws336-p008: ssh-transport（transport の状態機械・re-key・export と import）

Status: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus。状態機械と安全の要）
依存: p005・p007
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-transport.h`（冒頭に交換の順・strict・re-key）
- `plan/ws336/design.md` §6.1・§8・§9
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4253.txt`（SHA-256 `b471173305154f6c…`、全桁は同じ directory の SHA256SUMS）: §4.2、§7、§7.1（KEXINIT の後に送ってよい message）、§7.3、§9、§11
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-strict-kex-02.txt`（SHA-256 `c08126dcaaf63958…`、全桁は同じ directory の SHA256SUMS）: §3.2・§3.3
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-sshm-mlkem-hybrid-kex-10.txt`（SHA-256 `d7e1ff8723466a80…`、全桁は同じ directory の SHA256SUMS）: §2.1
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/rfc4250.txt`（SHA-256 `eb3a9d7c3e1fae53…`、全桁は同じ directory の SHA256SUMS）: §4.1.2（message 番号の範囲）、§4.2.2（disconnect の理由）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-transport.c`
- `plan/ws336/tests/unit/test-transport.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- client と server の 2 つの transport を memory の中でつなぐ（pending → input）。host_key と sign は試験の中の鍵（ssh-key）、check_host_key は受ける・拒む
- 初回の交換の後に session_id が両方で同じ、upper の message の往復（5・50・94・200）
- 相手が始めた re-key の間（自分の KEXINIT を送った後、自分の NEWKEYS の前）の `ssh_transport_send` が held に溜まり、自分の NEWKEYS の後に順に出る。held と出力が満ちると ENOBUFS で何も積まれない
- re-key: tick で 1 時間を越える・byte の上限を越える（上限は試験で到達しにくいので、packet の counter を直に大きくして tick）で KEXINIT、相手からの re-key、re-key の間の upper の message が held に溜まり NEWKEYS の後に順に出る、session_id は変わらない
- strict: 最初の受けが IGNORE で EPROTO、初回の交換の中の DEBUG・IGNORE・SERVICE_REQUEST・2 回目の HYBRID_INIT で EPROTO、相手の印が無いと EPROTO と DISCONNECT 3、seq の 0 への戻り
- 相手の識別の行: 前置きの行（client だけ）、65 行目で EPROTO、`SSH-1.5-` で EPROTO、253 文字ちょうど
- 交換の誤り: 署名の 1 bit、host key の拒否（EACCES と DISCONNECT 9）、S_REPLY の長さ、KEXINIT の一覧の不一致で DISCONNECT 3
- UNIMPLEMENTED: 番号 8・128 の message に seq 付きの UNIMPLEMENTED が返る。EXT_INFO（7）は捨てる。交換の外の 30・31 と、段に合わない 21 は EPROTO
- DISCONNECT の受けで ECONNABORTED と理由
- export → import の往復で通信が続く（鍵・seq・残りの input）、import の壊れた入力（各欄の長さ・role）で EINVAL
- 乱数の入力: 交換の各段の後で random な message を送り込み、落ちずに誤りで終わる

## 4. 注意

- header にない内部の状態（kex_step の値）は .c が決める。header の struct の field は変えない。
- 交換の間の upper の message の受け: 相手の KEXINIT を受ける前の upper の message は届ける（RFC 4253 §7.1）。
- disconnect の後の pending の出力は残す（呼び手が書いて閉じる）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh transport`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

（未着手）
