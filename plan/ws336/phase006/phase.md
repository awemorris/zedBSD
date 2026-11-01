<!-- awesome-plan project=zedbsd record=ws336-p006 -->

# ws336-p006: ssh-hostfile（authorized_keys・known_hosts・host の pattern）

Status: in-progress（実装済み・試験 PASS、cleared は Q1 が決める）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p005
WS335 の待ち: p003 経由

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-hostfile.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-sshd.8`（SHA-256 `74dde0667a4c99e6…`、全桁は同じ directory の SHA256SUMS）: AUTHORIZED_KEYS FILE FORMAT・SSH_KNOWN_HOSTS FILE FORMAT（`[host]:port`、@revoked、@cert-authority、`|1|`）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-ssh_config.5`（SHA-256 `6ef2f7058a6b7be9…`、全桁は同じ directory の SHA256SUMS）: PATTERNS（`*`・`?`・`!` の否定）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-hostfile.c`
- `plan/ws336/tests/unit/test-hostfile.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- pattern: `*`・`?`・空、`*.example.com`、大文字小文字（fold）、`a*b*c*d` の長い入力で時間が線形に近い（10 万文字で 1 秒未満）、list の否定が勝つ（`!bad,*`）
- authorized_keys: 一致・不一致、options 付きの行・`ssh-rsa` の行・壊れた行・長すぎる行が skipped に数えられ一致にならない、comment 行・空行・CR LF、最後の行に LF が無い
- known_hosts: host と `[host]:2222`、port 22 の時に `[host]:22` の行は一致しない（OpenSSH の約束どおり。sshd.8 を読んで確かめ、結果に書く）、MATCH・CHANGED・REVOKED・OTHER_TYPES・ABSENT の各場合と優先の順、`|1|` の行と @cert-authority の行が skipped、host の誤り（空・256 文字・制御文字）で EINVAL
- put_line → check の往復
- 乱数の入力

## 4. 注意

- pattern の match は再帰を使わず、最後の `*` への戻りだけで線形に近くする（header の約束）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh hostfile`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-01 09:32（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。Status は Q1 が直す。

### 作った物

- `userland/base/ssh/ssh-hostfile.c`（骨格の置き換え。header は不変）。pattern の match は再帰を使わず、最後の `*` への戻りだけ（時間は長さの積まで）。list は否定が勝つ。authorized_keys と known_hosts は、行を LF で切り、`ssh_key_parse_public_line`（p005）で鍵を読む。
- `plan/ws336/tests/unit/test-hostfile.c`（736314 checks。うち約 73 万は乱数の入力）。期待の値は ssh_config.5 の PATTERNS（`*`・`?`・`!`・host3 の例）と sshd.8 の 2 節、RFC 8032 7.1 の TEST 1・2 の公開鍵と 0..31 の鍵、`wire.h` で組んだ blob、試験の中の独立の base64 だけ。OpenSSH の出力は使っていない。

### 試験の内容

pattern 約 80 行の表（空・`*`・`?`・`*.example.com`・fold・ASCII の大文字だけが畳まれる境界 `@`/`` ` ``/`[`/`{`・size で切る・pattern 中の NUL）、`a*b*c*d` ほか 100000 文字で 1 秒未満（実測は約 0 秒）、list（否定が勝つ・並びの前後・`!host1,!host2` は単独では 0・空の entry・`!` だけ）、authorized_keys（一致・不一致・複数・重複・comment/空行・CR LF・LF 無しの最後・tab・options 付き 3 通りと `ssh-rsa` ほか他の型・壊れた行 6 通り・NUL・行の長さ 8192 ちょうどは読み 8193 は skipped・CR を含む長さ・長い comment 行は数えない・file 1 MiB ちょうどは読み +1 は EMSGSIZE・size で切る・NULL の引数）、known_hosts（host と `[host]:2222`・port 22 で `[host]:22` の行は一致しない・0/65535 の port・wildcard と list と否定・IPv6・REVOKED/MATCH/CHANGED/OTHER_TYPES/ABSENT の優先順を並びを入れ替えて・@revoked は host に依らず・hashed の @revoked も効く・@revoked の他の型や壊れた鍵は skipped・`@Revoked` などの他の marker は skipped・@cert-authority と `|1|` は skipped で一致しない・host の誤り 13 通りで EINVAL・255 文字は可、256 は EINVAL）、put_line（byte 単位の期待、port 0/7/10/100/65535、往復、host の `,*?![]` と先頭 `|@#` を EINVAL、小さい buffer は何も足さず EMSGSIZE、前の内容は保つ、out の既存の error を返す）、remove（byte 単位で他の行を保つ・CR LF・LF 無しの最後・port ごと・wildcard と否定・@revoked は消す・hashed、@cert-authority、他の marker、壊れた行、長すぎる行、NUL の行は残す・2 回目は 0・buffer が小さい時は何も足さない）。

### 実行したコマンドと最後の行（2026-11-01 09:32）

- `sh plan/ws336/tests/run-unit.sh hostfile` → `test-hostfile: PASS (736314 checks)` / `run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all` → 12 試験すべて PASS、最後の行 `run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし（終了 0。新しい file は `git add` 後に `git diff --cached --check` も 0）

### わざと壊す確かめ

`ssh-hostfile.c` の複製（`build/tmp/mut6/ssh-hostfile.c.orig`）に 48 通りの変更を 1 つずつ入れて `run-unit.sh hostfile` を流し、最後に `cmp` で元に戻ったことを確かめた。例: `*` の戻りで進めない（無限 loop。60 秒で打ち切り、FAIL 扱い）・戻りの pattern 位置を戻さない・末尾の `*` を飛ばさない・fold の範囲の端 2 通り・fold を無視・`?` を外す・否定が勝たない・`!` だけの扱い・空 entry を許す・行の長さの境界 2 通り・file の大きさの境界・comment の判定・port 22 の名前・`]` を `[` に・port の桁の順・known_hosts の fold・@revoked が host に依る・hashed を数えない・他の marker を受ける・壊れた鍵を host の一致に使う・他の型を CHANGED にする・結果の優先順 3 通り・host の長さ・host の byte の範囲 2 通り・writable の確かめ全部・コンマ・先頭 `|`・put の空白・put の reserve・remove が消さない・remove が hashed や他の marker を消す・remove の LF・remove の件数・remove の reserve・split の鍵の欄・`@revoked` の綴り・CR の trim。
- 最初の版で 5 つが生き残った。試験を足して 3 つは FAIL するようになった（`!` だけの list を空の string に、host に hashed の欄の文字列そのものを渡す remove、CR だけの行）。
- **残る 2 つは同値の変更**: (1) list の最後の entry の判定を `end >= size` から `end + 1 >= size` にしても、違うのは末尾が `,` の時の後ろの空 entry だけで、空 entry は読み飛ばすので結果が同じ。(2) next_line の `*offset` の書き方を変えた変更は意味が同じ。

### 自分で決めた点（header が黙っている所、または header の約束を細かくした所）

- **port 22 と `[host]:22`**: sshd.8 は「`[` `]` で囲み `:` と non-standard の port を続ける」と書く。よって port 22 の名前は host だけで、`[host]:22` の行は port 22 にも一致しない（試験で確かめた）。
- **host の byte の範囲**: header の「printable ASCII」を、空白（0x20）も除く 0x21〜0x7e にした（空白は行の欄を壊す）。長さは 1〜255。
- **put_line の host はさらに狭い**: `,` `*` `?` `!` `[` `]` を含む host と、先頭が `|` `@` `#` の host は EINVAL（1 行が 1 つの host だけを指すように。wildcard の行を書かせない）。check と remove の host にはこの制限をかけない。
- **skipped の数え方（known_hosts）**: 数えるのは、許されない marker（`@cert-authority` ほか）・hashed・host/型/鍵の欄が足りない行・鍵が壊れた行・長すぎる行・NUL を含む行・他の型の @revoked の行。host に一致しない他の型の行と、host に一致する他の型の行（OTHER_TYPES に使う）は数えない。authorized_keys では他の型・options 付き・壊れた・長すぎる行をすべて数える。
- **@revoked**: host の欄を見ない（hashed でも効く）。鍵が ssh-ed25519 で同じ鍵なら REVOKED。他の鍵の @revoked は何もしない。
- **壊れた鍵の行**: host に一致していても「その host が知られている」とは扱わない（skipped に数えるだけ）。
- **remove**: `@cert-authority` と他の marker の行は host に一致しても残す（この programs は使わない行なので byte のまま）。host/型/鍵の欄が揃わない行・長すぎる行・NUL の行も残す。鍵の中身は見ず、型の欄と鍵の欄があれば host の欄の一致だけで消す。全体が入る分を先に `ssh_buf_reserve` するので、入らない時は out に何も足さない。
- **list の空 entry**: 読み飛ばす（`!` だけも）。**NULL**: 引数が欠けた時、pattern は 0 を返し、known/authorized/put/remove は EINVAL。`removed`・`result`・`found` が NULL でも EINVAL（header は skipped だけ NULL 可）。
- **行の判定の順**: 空行と `#` の行（長さに関わらず）を先に無視し、その次に長さ（CR を含む 8192 まで）、NUL の順。
- authorized_keys は全行を読んでから skipped を返す（一致しても途中で止めない）。
- 結果は成功した時だけでなく、EMSGSIZE・EINVAL でも `*found = 0`・`*result = ABSENT`・`*skipped = 0` にしてある（引数の NULL を除く）。

### 未実施

- host の OpenSSH が書いた known_hosts・authorized_keys を読む相互運用（p015 以降）。
- 規約の全文の見直しの Phase（WS の最後）。ここでは §14 の「条件の中の関数呼び出し」「初期値の関数呼び出し」「scope だけの block」を自分の diff で見直しただけ。
- QEMU・実機は対象外（host の試験のみ）。

### header の不備

なし（header は変えていない）。
