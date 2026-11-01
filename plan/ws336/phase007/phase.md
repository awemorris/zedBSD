<!-- awesome-plan project=zedbsd record=ws336-p007 -->

# ws336-p007: ssh-kex（KEXINIT・交渉・hybrid の message・交換の hash・鍵の導出）

Status: in-progress（実装済み・試験 PASS、cleared は Q1 が決める）
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
- kexinit_parse: OpenSSH 10.5 の既定の一覧（`openssh-10.5p1-ssh_config.5` の KexAlgorithms・HostKeyAlgorithms・Ciphers・MACs の既定の値の節に書かれた名前の並びを試験の中に文字で書く。OpenSSH の source は読まない）を含む KEXINIT を読む、一覧の誤り・余りの bytes・短い payload
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

2026-11-01 09:44（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。Status は Q1 が直す。

### 作った物

- `userland/base/ssh/ssh-kex.c`（骨格の置き換え。header は不変）。KEXINIT の書き・読み、交渉、hybrid の 2 つの message、交換の hash H、鍵の導出。H と導出は SHA-256 の逐次の関数に K・H・各欄を直に渡し、K を別の buffer に写さない。
- `plan/ws336/tests/unit/test-kex.c`（402989 checks。うち約 20 万は乱数の入力）。期待の値は、`wire.h` で組んだ bytes、openssh-10.5p1-ssh_config.5 の既定の値の節（KexAlgorithms・HostKeyAlgorithms・Ciphers・MACs。OpenSSH の source は読んでいない）、H と導出を Python の hashlib で draft §2.5 と RFC 4253 §7.2 の連結から計算した既知の値、試験の中で `wire.h` と `ssh_sha256` で組み直した値だけ。OpenSSH の出力は使っていない。

### 試験の内容

kexinit_put（server・client、initial 0・1 の 4 通りを `wire.h` の期待の bytes と、cookie の 16 bytes を除いて比較。2 回目の cookie が違う。小さい limit は EMSGSIZE、ぴったりの limit は成功で 1 byte 少ないと EMSGSIZE、既に error の buffer は何も足さずその error）、kexinit_parse（put との往復で 10 本の list と flag、OpenSSH 10.5 の既定の一覧を持つ KEXINIT、flag の値 1・2、reserved の任意の値、message 番号の違い、余りの 1 byte、list の誤り 5 種を kex 欄に・他の 9 欄に、8193 bytes の list は EMSGSIZE・8192 bytes は可・8192 で末尾 comma は EINVAL、payload 16384 ちょうどは中身で判定し 16385 は EMSGSIZE、全ての短い prefix は EINVAL、誤りの時は構造体が 0）、choose（kex・host key・暗号 2 方向・圧縮 2 方向の欠けで ENOTSUP と kind、名前の前方一致は不可、MAC と language の中身は見ない、strict の印 4 通り（標準・旧・両方・無し）を server 側と client 側で、逆の側の印は不可、`kex-strict-c-v00` は不可、initial 0 では印を見ず strict を保つ、ENOTSUP は EPROTO より先で choice を変えない、first_kex_packet_follows の当たり・外れ（kex が先頭でない・host key が先頭でない・前方一致の先頭）、OpenSSH の既定の一覧から選ぶ）、hybrid の put・parse（byte 単位の期待、string の size は見ない、wrong 番号・余り・長さ 2^32-1・全ての prefix は EINVAL、出力は NULL と 0）、hash（既知の値、`wire.h` で組んだ 8 つの string との一致、K を 31 bytes の別の組み方にすると違う、NULL と size 0 の欄、欄の境界の移動・入れ替え・各欄の 1 byte の変更で H が変わる）、derive（C・D の 64 bytes、A の 256 bytes、F の 33 bytes の既知の値、K1 と K2 を試験の中で組んで一致、1〜256 の全ての size が 256 bytes の prefix で後ろを書かない、8 block の連鎖を組んで一致、6 つの letter が互いに違う、session_id・H・K の 1 bit で変わる、letter の範囲の外 5 通りと size 0・257・300・SIZE_MAX で EINVAL）、乱数（有効な KEXINIT を 1〜4 byte 壊して 10 万回 parse してから choose、乱数の列 10 万本を 3 つの parser に、どれも ASan の ちょうどの大きさの領域）。

### 実行したコマンドと最後の行（2026-11-01 09:44）

- `sh plan/ws336/tests/run-unit.sh kex` → `test-kex: PASS (402989 checks)` / `run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all` → 14 試験すべて PASS（buf 323・channel 1603651・cipher 1064・common 2316・crypto 1656・hostfile 736314・kex 402989・key 2771・packet 369168・privsep 947・sftp 1836・tools 13・ttymodes 20682）、最後の行 `run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし（終了 0）

### わざと壊す確かめ

`ssh-kex.c` の複製（git の外）に 25 通りの変更を 1 つずつ入れて `run-unit.sh kex` を流し、全部 FAIL し、最後に `cmp` で元に戻ったことを確かめた（同値の変更は無し）。内容: derive の size の上限 `>=`、letter の上端・下端、block 数の切り捨て、後の block の previous の長さ、letter を hash に入れない、H の V_C に V_S を入れる、K の長さを 31、K の length の符号、ignore_next を常に 0、host key の当たり判定を外す、標準の印・旧の印を見ない、印の側を逆にする、initial 0 で印を足す、later の exchange で strict を 0 にする、MAC の list を見る、first の名前の判定を前方一致に、KEXINIT の暗号 list を空に、first_kex_packet_follows を TRUE に、payload の上限 `>=`、parse の余りの確かめを外す、hybrid reply の余りの確かめを外す、hybrid init の message 番号、kind の名前。最初、印を見ない 2 通りは未使用の変数で build が通らず、`(void)` を足して作り直した。

### 自分で決めた点（header が黙っている所、または header の約束を細かくした所）

- **ssh_kexinit_put**: cookie を先に引き、失敗なら EIO を返して out に何も足さない。out に既に error があればそれを返して何も足さない。
- **ssh_kexinit_parse**: message 番号が 20 でなければ EINVAL。reserved の uint32 は値を問わず読み捨てる。flag は 0 以外を 1 にする。誤りの時 `*kexinit` は全て 0。list の誤りは reader が記録した EINVAL か EMSGSIZE（SSH_NAMELIST_MAX 超え）をそのまま返す。
- **ssh_kex_choose**: kind の文字列は `kex_algorithms`・`server_host_key_algorithms`・`encryption_algorithms_client_to_server`・`encryption_algorithms_server_to_client`・`compression_algorithms_client_to_server`・`compression_algorithms_server_to_client`・EPROTO は `kex_algorithms (strict key exchange)`。成功では `*kind = NULL`。検査の順は kex・host key・暗号 2 つ・圧縮 2 つ・strict の印。誤りの時は `*choice` を変えない。initial 0 では `choice->strict` を保つ。guess の当たりは「peer の kex の先頭が SSH_KEX_NAME で host key の先頭が SSH_KEX_HOSTKEY_NAME」。それ以外は ignore_next 1。NULL の peer・choice は渡されない前提（確かめない）。
- **hybrid の parse**: message 番号の違いは EINVAL。誤りの時、出力の pointer は NULL・size は 0。put の C_INIT・S_REPLY の長さは SSH_HYBRID_INIT_SIZE・SSH_HYBRID_REPLY_SIZE で固定。
- **ssh_kex_hash**: ある欄が 2^32-1 bytes を超える時は EINVAL を返す（header の誤りの一覧は ENOMEM・EIO だけ。32 bit より大きい size_t でしか起きない）。失敗時は h を wipe し、計算を free する。
- **ssh_kex_derive**: EINVAL（letter・size）の時は out に何も書かない。EIO・ENOMEM の時は out の size bytes を wipe する。途中の block は stack の 256 bytes に作り、終わりに wipe する。

### 未実施・header への指摘

- ENOMEM・EIO（crypto library の失敗）の経路は host の試験で起こせないので未実施（読んで確かめた）。
- host の OpenSSH との相互運用は p015 以降の別の確かめで、この Phase では未実施。
- header の不備は無し。上の「ssh_kex_hash の EINVAL」だけ header の誤りの一覧に無い値なので、Q1 が見て、規約の見直しの Phase で header の comment に足すかを決める。
