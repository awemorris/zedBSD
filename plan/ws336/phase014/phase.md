<!-- awesome-plan project=zedbsd record=ws336-p014 -->

# ws336-p014: ssh-keygen の program

Status: in-progress（2026-11-02 10:52 Sonnet）。前: planned（2026-11-01 p001 の設計で作成）
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

2026-11-02 11:24（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。Status は in-progress のまま Q1 に渡す。

### 作った物

- `userland/base/ssh/ssh-keygen-main.c`（骨格の置き換え。header は不変）。ssh-key・ssh-hostfile・ssh-buf・ssh-crypto の口だけを使う。
- `plan/ws336/tests/keygen-host.sh`（host で ASan・UBSan 付きに build して 377 checks を流す 1 本の script。最後の行 `keygen-host: PASS`）。`KEYGEN_SOURCE`・`KEYGEN_OUT`（変えた複製を試す時の source と出力の名前）は script の変数で、製品には入れていない。

### 実行したコマンドと最後の行（2026-11-02 11:24）

- `sh plan/ws336/tests/keygen-host.sh` → `keygen-host: 377 checks, 0 failures` / `keygen-host: PASS`（OpenSSH 10.0p2 の ssh-keygen との相互運用の block を含む）
- `sh plan/ws336/tests/run-unit.sh all` → 13 試験 PASS、最後の行 `run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（cc・clang の警告 0、header は基点から不変、zedBSD の clang での syntax check を含む）
- `git diff --check` → 出力なし（終了 0）

### 試験の内容

- 生成: F が 0600、F.pub が 0644（umask 000 でも F は 0600、umask 077 では F.pub は 0600）、F.pub の行の形（`ssh-ed25519 <68 文字の base64> <comment>`）、blob が 51 bytes で先頭 19 bytes が RFC 8709 の形、秘密鍵の file の armour・行の幅 70 以下・magic `openssh-key-v1\0`・checkint が 2 回同じで 0 でない、途中の file が残らない、`-l` の指紋が blob の SHA-256（python の hashlib で独立に計算）と一致、`-l -f F`（秘密鍵）と `-l -f F.pub` が同じ、`-y -f F` が F.pub と一致、2 つの鍵が違う、-C なし（user@host）・空・256 bytes 以上（255 に切る）の comment、-q なしの出力。
- 断る場合（終了の値 1、file を作らない・既存の file を変えない）: `-t rsa`・`-t ecdsa`、`-N` が空でない、`-N` が無い、既に F がある、F.pub だけがある、dangling link、comment に LF・CR、無い directory、読み取り専用の directory、未知の option・値の欠け・余りの operand、`-l -y`・`-l -A`・`-R h -l` など 2 つの job、長すぎる path（4095 は通り 4096 は断る、`-R` の `.old` の分の境界 4091・4092）。
- 読む側: 空・text・無い file・directory・20000 bytes の file・切れた秘密鍵・rsa の公開鍵・`-y` に公開鍵の file はどれも終了 1 と文。公開鍵の行が LF なし・CR LF・2 行目あり・先頭に comment 行（これは断る）。
- 既定の path（HOME の `~/.ssh/id_ed25519`）: `.ssh` が 0700 で作られる、既存の `.ssh` の mode は変えない、`-l`・`-y`・`-R` が既定の file を使う。
- `-A -f prefix`: prefix/etc/ssh が無ければ断る、作る・既存を変えず何も出さない・`-q` は何も出さない、公開鍵だけが残っていれば置き換える、秘密鍵だけが残っていれば何も作らない、公開鍵の場所が directory なら秘密鍵は残して `ssh-keygen -y -f` の案内を出して 1。
- `-R`: ホストの行（`h2,h1` の list の行も）だけを消して他の行（comment・空行・hashed・@revoked・他の型・最後の LF なしの行）は byte 単位で保つ、`.old` が元の内容、元の file の mode（0640）を保つ、`[h]:port` はその port だけ（1・65535・9090・2200 を含む）、無いホストは何も変えず `.old` も作らず終了 0、ホスト名 255 は通り 256 は断る、不正な引数（`[h1`・`[h1]`・`[h1]:0`・`[h1]:65536`・`[h1]:22x`・`[]:22`・空・制御文字）は 1、file が無い・directory は 1、`.old` の場所が directory で書けない時は元の file を変えず temp も残さない、1 MiB ちょうどは読み 1 MiB + 1 は断る、既定の file。
- 左右の相互運用: OpenSSH の `ssh-keygen -l -f`・`-y -f` が自前の鍵（秘密・公開）を読んで同じ結果、自前が OpenSSH の鍵（秘密・公開）を `-l`・`-y` で読んで OpenSSH と同じ、passphrase 付き・rsa の鍵は 1 と文、`-R hostA`・`-R "[hostA]:2222"` の結果の known_hosts が OpenSSH の `-R` と byte 単位で同じ。
- 左記の期待の値は仕様の形・手の計算・python の独立の計算で作った。OpenSSH の出力は期待の生成に使わず、相互運用の block で読み合うことの確認だけに使った。

### わざと壊す確かめ

`ssh-keygen-main.c` の複製（`build/tmp/p014-mut/m<番号>-<名前>/`、元の file は変えない）に 80 通りの変更を 1 つずつ入れて、`KEYGEN_SOURCE` で `keygen-host.sh` を流した（並列 4、`RLIMIT_FSIZE` 64 MB・`RLIMIT_CPU` 400 s、wall 150 s・RSS 6 GB で kill。kill に至った物は無し）。最終の版での結果: **71 通りが FAIL（検出）、8 通りが生き残り、1 通りは変更の位置が無くなって適用できず（read-eintr、下）**。

生き残った 8 通りは全部同値か host の試験で作れない状態で、試験は足していない。
- 同値（もう 1 つの確かめが同じ結果を出す）: `join-first-boundary`（`first_size >= capacity` を `>` にしても、直後の `second_size >= capacity - first_size` が長さがちょうど capacity の時に断る）、`read-limit-off`（`keygen_read_file` の長さの上限を外しても `ssh_buf` の limit が同じ長さで EMSGSIZE にする）、`write-replace-check-off`（`keygen_write_file` の「既存を置き換えない」の再確認を外しても、書く前に `keygen_generate_pair` が同じ確かめを済ませている。再確認は競合の備え）、`pair-private-check-off`（`keygen_generate_pair` の先の確かめを外しても `keygen_write_file` の確かめが同じ文で断る）、`port-digits-missing`（`[h]:` の後に数字が無い時の確かめを外しても、port が 0 になり次の確かめで断る）。
- 試験では見えない: `write-fsync-off`（fsync を外す。出力は同じ）、`write-partial-off`（write が途中までで返った時の続きを外す。host の file では途中までの write が起きない）、`read-eintr`（read の EINTR の再試行を外す。signal を送る試験は作っていない。最後の版では comment を足したので変更の位置が一致せず適用されなかったが、前の版では生き残り）。
- 生き残りに数えない 1 つ: `hostkeys-prefix-default`（`-A` で `-f` が無い時の prefix を `""` から `"/tmp"` にする）。`-f` 無しの `-A` は実在の `/etc/ssh` に書くので host の試験では流さない。p019 の QEMU で確かめる。これも生き残りの 8 通りの中に数えている（8 通り = 同値 5 + fsync・partial の 2 + prefix の 1）。

最初の版（HOME を使う試験・境界の試験を足す前）では 15 通りが生き残り、その中で試験が足りなかった物（余りの operand が `-N` 無しの別の理由で 1 になっていた、`-l -A`・`-R h -l` が既定の file の失敗で 1 になっていた、1 MiB・path 長・`-R ""` の文・comment の LF・CR の文の確かめが無かった、など）は試験を足して検出するようにした。mutant の一覧の script は scratchpad の `p014/mut.py`（git に入れていない）、最終の結果は `build/tmp/p014-mut-final.txt`。

### 自分で決めた点（header が黙っている所、phase.md が決めていない所）

- **-N が無い生成は断る**（終了 1、「-N '' を使え」の文）。OpenSSH は passphrase を問うが、問う場所が無く、黙って passphrase 無しの鍵を作らない方が安全なので。`-N` が空でない値は常に 1（`-l`・`-y`・`-A`・`-R` でも）。
- **既存の F は問わずに断る**（OpenSSH は問う）。F.pub だけがある時も断る（`-A` だけは、秘密鍵が無く公開鍵だけが残っていれば公開鍵を置き換える。中断した run の残りを直すため）。
- **`-A` は `-f prefix` を取る**（仕様の `-A` の説明）: 鍵は `prefix + /etc/ssh/ssh_host_ed25519_key`、`-f` 無しなら `/etc/ssh/ssh_host_ed25519_key`。試験用の切り替えは足していない。prefix の下の `etc/ssh` が無ければ作らずに断る。秘密鍵がもう在れば何もしない（公開鍵が無くても作らない）。ed25519 だけを作る（rsa・ecdsa は作らない、design.md §4.2）。出力は `ssh-keygen: generating new host keys: ED25519`（`-q` で出さない）。
- **`-t` は ed25519 だけ受ける**（他は断る）。`-t` が無ければ ed25519。
- **既定の path**: `-f` が無ければ `$HOME/.ssh/id_ed25519`（HOME が空なら account の home）、`-l`・`-y` も同じ。生成の時だけ `~/.ssh` を 0700 で作る（在れば mode を変えない）。`-R` の既定は `~/.ssh/known_hosts`。
- **`-l`**: 秘密鍵の file も公開鍵の file も読む。公開鍵は最初の 1 行だけ（先頭の comment 行は読まない）。出力は `256 SHA256:… comment (ED25519)`（comment が空なら `no comment`）。`-y` は秘密鍵だけ（暗号化・他の型・公開鍵の file は 1）。
- **`-R`**: ホストの行が無い時は終了 0、何も変えず `.old` も作らず `Host H not found in PATH` を出す（OpenSSH の振る舞いと同じ）。あれば `.old` に元の内容を書いてから（temp + rename）新しい内容を rename で置く。mode は元の file の mode。`.old` が既にあれば置き換える。引数は `host` か `[host]:port`（port は 1〜65535 の 10 進だけ）。
- **comment**: `-C` が無ければ `user@host`（account の名前と gethostname、分からなければ `unknown`）。LF・CR を含めば断る。256 bytes 以上は ssh_key_generate の約束で 255 に切る（断らない）。
- **書き込み**: 一時の名前は `<path>.tmp.<pid>`（`O_EXCL`、同じ名前の物が在れば断って触らない）。open の mode で 0600（private）・0644（public、umask は効く）、fsync、rename。rename の前に宛先が無いことをもう一度確かめる（lstat。競合の窓は残る）。失敗すれば自分の temp を unlink する。private を先に、public を後に書く（public が失敗したら private は残し、`ssh-keygen -y -f` の案内を出す）。
- 出力は成功の情報が stdout、誤りが stderr（`ssh-keygen: …`）。終了の値は 0 と 1 だけ。usage は stderr、1。getopt を使い、operand は断る。
- 秘密鍵の読み込みは `ssh_buf`（secret）に入れて wipe し、鍵の struct も wipe する。

### 未実施・残り

- `-A` を `-f` 無しで流す（実在の `/etc/ssh` に書く）確かめ・zedBSD の上での実行は p019（QEMU）。
- HOME が空・無い時に account の home に落ちる枝（`getpwuid`）は、実在の home を触るので host では流していない。
- EINTR の再試行・部分の write の続きは試験していない（上）。
- `ssh_known_hosts_remove` が hashed の行を飛ばす・`@cert-authority` などは p006 の範囲（ここでは hashed の行が残ることだけを確かめた）。
- 規約の全文の見直しは p025。ここでは §14 を自分の diff で見直した（宣言の位置、call を条件に書かない、return の前の comment、cleanup は 1 つの label への forward goto だけ、関数の最後は成功の return）。「if の前の comment」は error check の後ろの二次的な `if` の一部に付けていない所がある（p025 で見る）。

### header の不備

なし（header は変えていない）。

### 試験の道具・他の file を触っていない

`plan/ws336/tests/` の既存の道具、header、他の .c、Makefile は変えていない。
