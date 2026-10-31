<!-- awesome-plan project=zedbsd record=ws336-p013 -->

# ws336-p013: ssh-sftp（SFTP v3 の request と reply）

Status: cleared（2026-11-01 06:09 Q1、main 8872d89fe）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p002
WS335 の待ち: 無し。Q3 は A（sftp-server を入れる、2026-11-01）なので行う

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-sftp.h`
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-secsh-filexfer-02.txt`（SHA-256 `3f69664d8050a28a…`、全桁は同じ directory の SHA256SUMS）: §3（packet）、§4（INIT・VERSION）、§5（attrs）、§6（各 request）、§7（各 reply、STATUS の code、longname）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-PROTOCOL`（SHA-256 `4a9e7f6d26d3f59d…`、全桁は同じ directory の SHA256SUMS）: 冒頭（v3 を話す）と sftp の拡張の節（announce しない理由）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-sftp.c`
- `plan/ws336/tests/unit/test-sftp.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- 各 request を `wire.h` で組んで parse、attrs の各 flag の組と EXTENDED の pair（上限 64 +1 で EINVAL）、path の NUL・4097 bytes、未知の型で ENOTSUP と id
- 各 reply の put を `wire.h` の期待と比べる、begin_name・entry・end_packet の長さ
- long_name: 期待は手で書いた文字列（ls -l の形、6 か月の境）
- next_packet の境界、乱数の入力

## 4. 注意

- vendor の拡張は announce しない（header）。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh sftp`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

実施: 2026-11-01 06:06（host の `date`）。worktree `s-ws336-p013`、基点 main 1ce92e0eb。実装済み・試験 PASS（cleared は Q1 が決める）。

### 作った物

- `userland/base/ssh/ssh-sftp.c`（骨格を実装: next_packet、parse_request、put_version・status・handle・data・attrs、begin_name・put_name_entry・end_packet、long_name）
- `plan/ws336/tests/unit/test-sftp.c`

header は変えていない。

### 実行した command と最後の行

- `sh plan/ws336/tests/run-unit.sh sftp` → `test-sftp: PASS (1836 checks)` / `run-unit: PASS`（ASan・UBSan・`-std=c89 -Wall -Wextra -Werror`）
- `sh plan/ws336/tests/run-unit.sh all` → `test-buf: PASS (323 checks)`、`test-common: PASS (2316 checks)`、`test-sftp: PASS (1836 checks)`、`test-tools: PASS (13 checks)`、`run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし（終了 0）

### わざと壊す確かめ（git の外、`build/tmp/p013/mutate.py`）

32 通りの変異（packet 上限・body 待ちの比較、consume の長さ、extended 上限 >=・+1、path 上限、path の NUL の確かめ、handle 上限（読みと書き）、attrs の uid/gid の順、EXTENDED の書き出し、six months の境の <=、READ の clamp、未定義 flag の確かめ、end_packet の上限、閏年の 100・400 の規則、setuid・sticky の文字、STATUS の language tag、残りの byte の確かめ、9 月の日数、owner の幅、version の値、RENAME の path の取り違え、READ の offset と length の順、minute、INIT の extension の読み飛ばし、NULL owner、cut の境）。
30 通りは試験が FAIL した。見逃し 2 通りは等価な変異だった: (1) `now >= mtime - 1`（mtime より 1 秒前の now の時の age は符号なしの桁あふれで巨大になり、年の形になるので結果が変わらない）、(2) parse_attrs の失敗時の memset の除去（parse_request が失敗時に request 全体を 0 にするので観測できない）。
未定義 flag の確かめの最初の変異は build が通らず無効だったので、`& 0x00ffffffU` に変えて再実行し、FAIL を確認した。元に戻したことは `cmp` で確認（RESTORED）。

### 自分で決めた点（header が黙っている所）

- next_packet: packet_out に入らない（上限・memory）時は、その error（EMSGSIZE・ENOMEM）を返し、input は消費しない（header の 0・EAGAIN・EINVAL の外）。長さ 0 または上限超は length の 4 byte が揃った時点で EINVAL（body を待たない）。
- parse_request の失敗時: request は 0 にし、type と（読めれば）id だけを入れる（BAD_MESSAGE の返答に id が要るため）。INIT は id を 0。type の byte が無い（size 0）は EINVAL。未知の type で id（4 byte）が読めない時は ENOTSUP でなく EINVAL。
- 要求でない type（VERSION・STATUS などの reply の型、EXTENDED、欠番）は、id が読めれば ENOTSUP（後ろの byte は読まない・trailing も見ない）。EXTENDED の名前も読まない。
- INIT: version はそのまま（値の確かめなし）。extension の pair は何組でも読み飛ばす（packet が 256 KiB までなので有界）、名前だけで data が無い pair・残りの端数は EINVAL。
- attrs: 未定義の flag bit（0x10〜0x40000000）は EINVAL（draft §5 の「未対応の bit は protocol error」。v3 だけを announce するため）。EXTENDED の pair は上限 64 まで読み飛ばし、65 以上・count が無い・pair が足りない・多すぎは EINVAL。読んだ後の `attrs.flags` は EXTENDED の bit を落とす（保持しないため）。
- pflags: 未定義の bit も、TRUNC・EXCL が CREAT を伴わない場合も、そのまま渡す（server が open(2) の flag に写す）。
- path: 空は有効。4096 は有効、4097 は EINVAL。NUL を含めば EINVAL。handle: 上限 256 byte（draft §6）、超えると EINVAL、空は parse する（有効かの判定は server）。handle の中身（NUL など）は検査しない。
- READ の length: SSH_SFTP_READ_MAX（64 KiB）を超える値は parser がその値に切る（draft が許す短い read。server が確保する大きさを誤らないため）。offset は 64 bit そのまま。
- put_handle: 256 byte 超は何も書かずに out->error = EMSGSIZE を記録する。put_* の packet 長が SSH_SFTP_PACKET_MAX を超える時（end_packet が EMSGSIZE）、put_data などは out->error にそれを記録する（packet は buffer に残るので、caller は error を見て捨てる）。
- put_status の message と put_name_entry の filename・longname は NULL を空文字列にする。put_attrs と name entry は SIZE・UIDGID・PERMISSIONS・ACMODTIME の 4 つだけを書き（EXTENDED と未定義 bit は落とす）、extended_count を書かない。
- end_packet: 記録済みの out->error があればそれを返す。start が packet を成さない（start が size 以上、または length と type の 5 byte に満たない）時は EINVAL。packet が SSH_SFTP_PACKET_MAX を超えれば EMSGSIZE（長さは書かない）。これらの時 caller は start 以降を捨てる。
- long_name: 欄は draft §7 の例の幅（link 3 右寄せ、owner・group 8 左寄せ、size 8 右寄せ、日付 12）の最小幅で、長ければ伸ばす。日付は「mtime が now 以下で now との差が 15778476 秒（平均 Gregorian 年の半分）未満」なら `Mon dd hh:mm`、そうでなければ `Mon dd  yyyy`（未来の時刻は年の形）。mtime が無い時は 0、permissions が無い時は `?---------`、type が不明な mode は先頭が `?`。setuid・setgid・sticky は s・S・t・T。owner・group が NULL なら `?`、name が NULL なら空。日付は UTC で、暦は自分の loop で計算する（gmtime を使わない）。capacity 0 は何も書かない。
- symlink の path・target の順は header の約束（OpenSSH の順）に従い、parser は読んだ順に path・target に入れる。

### phase.md と仕様の食い違い

見つからなかった。draft-ietf-secsh-filexfer-02 の SHA-256 は `sha256sum -c SHA256SUMS` で OK を確認した。

### 未実施

- host の OpenSSH の sftp・scp との相互運用（別の確かめ、p019 以降）。
- QEMU・実機の試験（この Phase は host の単体試験だけ）。
