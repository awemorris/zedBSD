<!-- awesome-plan project=zedbsd record=ws336-p012 -->

# ws336-p012: ssh-privsep（monitor との frame と同期の呼び出し）

Status: in-progress（実装済み・試験 PASS、cleared は Q1 が決める。2026-11-01 06:16）
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
- `userland/base/ssh/ssh-privsep.h`（frame と全 request の形）
- `plan/ws336/design.md` §6.2
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-privsep.c`
- `plan/ws336/tests/unit/test-privsep.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- put_frame → next_frame の往復（各型）、長さ 0・上限 +1・未知の型で EINVAL、途中で EAGAIN、2 つの frame の連続
- mon_call: socketpair と fork（試験の中の子が答える）で往復、子が閉じると EIO、EINTR（SIGALRM を挟む）で続く
- 乱数の入力

## 4. 注意

- この Phase は frame だけ。各 request の中身の確かめは p016 の monitor が行う。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh privsep`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-01 06:16（host の `date`）、実装済み・試験 PASS。cleared は Q1 が決める。

作った物: `userland/base/ssh/ssh-privsep.c`（frame の書き・読み・同期の呼び出し）、`plan/ws336/tests/unit/test-privsep.c`。header・他の .c・試験の道具は変えていない（`BASE=d4ab182d2` で header 不変を確認）。

実行した command と最後の行:

- `sh plan/ws336/tests/run-unit.sh privsep` → `test-privsep: PASS (947 checks)` / `run-unit: PASS`（ASan・UBSan）
- `sh plan/ws336/tests/run-unit.sh all` → test-buf 323・test-common 2316・test-privsep 947・test-tools 13 checks、`run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（cc・clang 警告 0、zedBSD の clang での compile を含む）
- `git diff --check` → 出力なし（exit 0）

試験の内容: 限界の値の手の計算（FRAME_MAX 65536、STATE_MAX 331796）、全 11 型の put_frame を wire.h の bytes と比較、各型の限界の両側（長さ = 限界は可、+1 は EMSGSIZE で out 不変）、parser の全 prefix で EAGAIN と入力の不変、長さ 0・未知の型 0..255・限界 +1・0x80000000・0xffffffff の EINVAL（body を待たずに）、2 つの frame の連続と 1 byte ずつの供給、body の buffer が secret にされること、body の limit が小さい時の EMSGSIZE と入力の不変、ランダム 15 万回（受理・拒否・不足の 3 通りが各 1000 回超）と 1 byte 変異 9000 回、mon_call は fork した子と socketpair で往復・返事を 1 byte ずつ・返事の後ろの frame を読み残すこと・250000 byte の要求と 331795 byte の返事・子が閉じる（要求の後・返事の途中・要求の前）で EIO・SIGALRM（SA_RESTART 無し）が read と write（短い write）を中断しても続くこと・敵対的な返事 7 通りで EINVAL・要求の限界超え/未知の型/fd -1/返事の buffer の limit。

試験の確かめ（実装を git の外でわざと壊し、元に戻して `cmp` で一致を確認）: 限界の比較を 1 つずらす（2 通り）、`length > limit` を `limit + 1`、length 0 の拒否を外す、STATE_MAX を超える拒否を外す、長さに +1 を足さない、必要な分だけ読まず chunk 分を読む、EINTR の再試行を外す、body の secret 化を外す、SESSION_START の限界を FRAME_MAX に、RESULT を大きい型に、frame の完了の判定を 1 ずらす、body の長さを 1 ずらす、consume を 1 ずらす、短い write を全部書けたと扱う、4 byte・5 byte の待ちを 1 ずらす、NULL の body の確かめを外す、入力 buffer の limit を小さく、は全て FAIL（または hang。hang は子の alarm と待ちの上限で FAIL に変えた）。FAIL しなかった 5 通りは同値だった: parser の `limit == 0` の確かめ（後の `length > limit` が同じ結果）、put_frame の型の上限 255→12（`mon_length_limit` が 0 を返す）、put_frame 冒頭の `out->error` の確かめ（`ssh_buf_reserve` が同じ誤りを返す）、write が 0 を返す場合（実在しない）の EIO、失敗時の `ssh_buf_reset(body)`（reserve が先に失敗し body は空のまま）。

自分で決めた点（header が黙っている所）:

- 長さ（4 byte の field）は型の byte と body の和。限界はこの長さに掛ける: 長さ ≤ SSH_MON_FRAME_MAX（CONFIG・AUTH_DONE・SESSION_START は SSH_MON_STATE_MAX）、つまり body は限界 - 1 byte まで。
- put_frame は型が列挙の外（負の値を含む）・body が NULL で size が 0 でないとき EINVAL を返す（header は EMSGSIZE か out の誤りだけを書く）。これらと body の限界超え（EMSGSIZE）では out に誤りを記録せず out は不変で使い続けられる。out が元から誤りを持てばその誤りを返す。
- next_frame は body を必ず `secret = 1` にして空にしてから始める（呼び出し側が非 secret を渡しても秘密を漏らさない）。失敗では入力は不変で body は空。body の limit が小さい時は header の列挙外の EMSGSIZE を返す（body の buffer の誤りをそのまま）。
- next_frame は、長さ 4 byte が揃えば長さ 0 と STATE_MAX 超えを、型 byte が揃えば型の範囲とその型の限界を、body が届く前に EINVAL にする。
- mon_call は返事の frame に必要な byte だけを読み（後続の frame を読み込まない）、要求の frame・読み途中の入力・読みの chunk を secret の buffer に置いて最後に消す（password を含み得る）。blocking の descriptor を前提にし、EAGAIN を含む他の read/write の失敗は EIO。SIGPIPE を無視するのは呼び出す process の側（無視しないと相手が閉じた descriptor への write で process が落ちる）。失敗時に `*reply_type` は触らない。
- 型の limit は表でなく switch（`mon_length_limit`）で与える。

未実施: ssh-crypto.c が骨格のため `ssh_wipe` は何もしない。secret の消去が実際に memory を 0 にすることの確かめは crypto の phase 以降（この試験は消去を見ない）。実際の monitor・pre-auth の process を使う確かめは p016・p017。phase.md の第 3 節は put_frame の限界超えを EINVAL と書くが header は EMSGSIZE で、header に従った（next_frame の限界超えは phase.md・header とも EINVAL）。
