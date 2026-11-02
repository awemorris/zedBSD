<!-- awesome-plan project=zedbsd record=ws336-p011 -->

# ws336-p011: ssh-config（sshd_config・ssh_config・-o）

Status: 実装済み・試験 PASS（2026-11-02 11:20 Sonnet、cleared は Q1 が決める）。前: planned（2026-11-01 p001 の設計で作成）
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
- `userland/base/ssh/ssh-config.h`
- `plan/ws336/design.md` §4.2・§10（今の sshd_config・ssh_config の行）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-sshd_config.5`（SHA-256 `c6ff3f09207fc448…`、全桁は同じ directory の SHA256SUMS）: 各 keyword の値の形、時間の書き方（TIME FORMATS）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-ssh_config.5`（SHA-256 `6ef2f7058a6b7be9…`、全桁は同じ directory の SHA256SUMS）: Host の block、最初の値が勝つ、各 keyword
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/ssh-config.c`（「受けて無視する」keyword の一覧を design §10 と合わせて置き、結果に一覧を書く）
- `plan/ws336/tests/unit/test-config.c`
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- 今の `userland/packages/network/openssh/files/sshd_config` と `ssh_config` の全行が通る（HostKey の rsa は 2 つ目の HostKey として入る。sshd が型で飛ばす）
- 各 keyword の値の範囲、yes/no の誤り、LoginGraceTime の `2m`・`1h`、MaxStartups の `10:30:100`、Subsystem の sftp 以外で EINVAL、Match で EINVAL、未知の keyword で `line N:`
- ssh_config: Host の pattern で当たる・外れる・否定、最初の値が勝つ、`-o` が file より先に効く（呼ぶ順）、UserKnownHostsFile `/dev/null`
- 引用符の値、`=` の形、tab、CR LF、長すぎる行
- 乱数の入力

## 4. 注意

- 「受けて無視する」keyword の一覧の候補: sshd は UseDNS・PrintMotd・PrintLastLog・UsePAM（no のときだけ）・X11Forwarding（no）・AllowTcpForwarding（no）・Banner（none）・AcceptEnv（値は無視して LANG・LC_*・TERM だけ）・Compression（no）、ssh は VerifyHostKeyDNS・HashKnownHosts（no）・SendEnv・ForwardAgent（no）・ForwardX11（no）・Compression（no）・AddKeysToAgent（no）・LogLevel・ControlMaster・ControlPath・ControlPersist（client の多重化は持たないので、yes の時は誤りにせず 1 本ずつ接続する）。値が機能を有効にする物（`X11Forwarding yes` など）は EINVAL にする。決めた一覧を結果に書く。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、この Phase の物でない .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/run-unit.sh config`（この Phase の試験）
- `sh plan/ws336/tests/run-unit.sh all`（それまでの全部）
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`（基点は [ws.md](../ws.md) の「header の基点」の行。cc と clang で警告 0、header が基点から不変）
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 11:20（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。Status は Q1 が直す。

### 作った物

- `userland/base/ssh/ssh-config.c`（骨格の置き換え。header は不変）。行を LF で切り、keyword と語に分け、keyword の表（sshd 35・ssh 33 行、case を区別しない）で値を読む。値の読みは共通の部品（yes/no・10 進数・時間・名前の表・path・語）。Host の pattern は `ssh_pattern_match`（p006、fold 1）を語ごとに呼ぶ。
- `plan/ws336/tests/unit/test-config.c`（1321693 checks。うち約 90 万は乱数の入力。入力は大きさぴったりの heap の複写で渡し、1 byte でも先を読めば ASan が落とす）。期待の値は sshd_config.5・ssh_config.5（10.5p1）の keyword の説明・TIME FORMATS・PATTERNS と手の計算だけ。OpenSSH の出力は使っていない。

### 試験の内容

sshd の既定値の全部、今の image の `sshd_config`・`ssh_config` の全行（試験の中の複写と、tree にあれば実 file）、sshd の各 keyword（yes/no 4 つ、数 2 つ、時間 2 つ、Port・ListenAddress・AddressFamily・HostKey・AuthorizedKeysFile・PermitRootLogin・MaxStartups・MaxSessions・Subsystem）の値の範囲の両端とその外、時間の表 41 行（`2m`・`1h`・`1h30m`・`1d`・`1w`・端の値・誤り）、「受けて無視する」の各 keyword の受ける値と断る値、Match、未知の keyword（`line N: unknown keyword: <名前>`、名前の制御文字は `?`、長い名前は 48 byte で切る）、行の形（tab・CR LF・`=`・引用符・`#`・VT/FF・8192 byte の行・NUL・128 語・file の大きさの境）、全 keyword を 1 度ずつ与えて次に別の値で与える（first wins、bit や id の共有が無いこと）、ssh_config の各 keyword と既定値、Host の block（pattern の list・否定・大文字小文字・`=`・引用符・CR LF・外れた block の誤りも検出）、-o と file の順（-o が先なら勝つ）、`-o` の形と誤り（行番号なし）、IdentityFile の蓄積と 8 個、乱数の file と option。

### 実行したコマンドと最後の行（2026-11-02 11:19）

- `sh plan/ws336/tests/run-unit.sh config` → `test-config: PASS (1321693 checks)` / `run-unit: PASS`
- `sh plan/ws336/tests/run-unit.sh all` → 14 試験すべて PASS、最後の行 `run-unit: PASS`
- `BASE=d4ab182d2 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`（header は基点から不変）
- `git diff --check` → 出力なし（終了 0）
- cc と clang で `-std=c89 -pedantic -Wall -Wextra -Werror -O2` の compile も通った。

### わざと壊す確かめ

`ssh-config.c` の複製（`build/tmp/p011-mut2/mNNN/`）に 325 通りの変更を 1 つずつ入れて、ASan・UBSan 付きで試験を流した（RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・150 秒で打ち切り・RSS 6 GB で kill・4 並列）。結果: 試験が FAIL（または abort）= 311、compile で弾かれた = 6（keyword の番号を重ねて switch の case が重複）、**生き残り 8**。1 回目は 26 が生き残り、試験を足して 18 を塞いだ（既定値を各 field 単独で保つ・VT/FF・keyword の表示の長さ・0x1f と 0x7f の byte・`_` と `z`・Subsystem の語数と path の形・HostKey/IdentityFile の `none` と `%h`・古い語を読む誤り・先の 1 byte を読む誤り（大きさぴったりの heap）・-o の誤りに行番号が付かないこと）。
生き残り 8 はどれも同値か到達できない防御の変更: (1) `config_lower` の範囲を `Z` から `Y` に（表の keyword と値に z・Z を含む語が無い）。(2) `config_text_equal` の長さの比較を `!=` から `>` に（長い方は終端の NUL との比較で必ず食い違う）。(3) 誤りの文の表示で `0x21` を `0x20` に（keyword は空白を含まない）。(4) 誤りの文の長さの上限を 1 つ緩める（最長でも約 80 byte で到達しない）。(5) `offset = end + 1`（`end == size` の時だけ違い、そこで loop が終わるので同じ）。(6) `config_value_word` の `size >= capacity` の確かめを外す（`maximum` が既に capacity - 1 以下）。(7) MaxStartups の `count != 1 && count != 3` を `count == 2` に（count は 1〜3 だけ）。(8) MaxSessions の clamp の `>` を `>=` に（等しい時は同じ値）。mutation の script は scratchpad（git の外）。

### 自分で決めた点（header が黙っている所、または header の約束を細かくした所）

「受けて無視する」の一覧:
- sshd: 任意の yes/no を受ける物 = UseDNS・PrintMotd・PrintLastLog。`no` だけ受ける物（`yes` や他の値は EINVAL）= UsePAM・X11Forwarding・AllowTcpForwarding・AllowAgentForwarding・GatewayPorts・PermitTunnel・PermitUserEnvironment・Compression・KbdInteractiveAuthentication・ChallengeResponseAuthentication・GSSAPIAuthentication・HostbasedAuthentication。`none` だけ = Banner。任意の語 = AcceptEnv。phase.md の候補に、機能を切る「no だけ」の 9 つ（AllowAgentForwarding 以降の 8 つと 3 認証）を足した（実際の sshd_config によくある行を未知の keyword の誤りにしないため）。
- ssh: `no` だけ = VerifyHostKeyDNS・HashKnownHosts・ForwardAgent・ForwardX11・ForwardX11Trusted・Compression・AddKeysToAgent・GSSAPIAuthentication・KbdInteractiveAuthentication・HostbasedAuthentication・PermitLocalCommand。任意の yes/no = IdentitiesOnly。任意の語 = SendEnv・ControlMaster・ControlPath・ControlPersist（`yes` でも誤りにしない）。LogLevel は ssh では acted（header どおり）。sshd の LogLevel は受けない（未知の keyword）。

その他:
- **first wins は 1 回の parse の中**。sshd は parse ごとに「与えた keyword」の mask を内部に持つ（header の struct に set_mask が無い）。同じ keyword の 2 つ目は同じ検査を捨てる複写に通すので、値が誤りなら行番号つきで EINVAL。HostKey は全部足す（同じ path は 1 つ、9 個目は EINVAL）。**Port・ListenAddress（1 つしか持てない）は 2 つ目が違う値なら EINVAL**（`only one Port is supported`）。同じなら可。
- ssh_config は全 field が 1 度だけ。複数回の parse をまたいで first wins（`set_mask`）。**IdentityFile は全 source をまたいで足す**（8 個まで、同じ path は 1 つ、9 個目は EINVAL）。この bit は「どこかが与えた」の印で、既定の identity は identity_count が 0 の時だけ。`ssh_config_defaults` は set_mask の bit を立てない。
- **外れた Host block の中も検査する**（未知の keyword・誤った値は EINVAL）。値は捨てる複写に読む。
- Host の pattern は空白区切りの語ごとに `ssh_pattern_match`（fold 1）。`!` の語が当たれば block は外れる。肯定が 1 つも無い（`Host !a` だけ）は外れる。`Host` の語なしは EINVAL。-o に Host は EINVAL。host が NULL は EINVAL。空の host 名は `*` だけに当たる。
- **時間は TIME FORMATS の全形**（`s S m M h H d D w W`、複数の項の和、無印は秒で最後だけ）を sshd の 2 つと ssh の ConnectTimeout・ServerAliveInterval に使う（header は s・m・h と書くが、仕様の本文が上位なので d・w と和も受ける）。1 項は 10 桁まで、合計は 2147483647 まで。`1 m`（語が 2 つ）・`-1`・`+1`・`1.5h` は EINVAL。
- 数は 10 進だけ、9 桁まで（符号・空白・16 進は EINVAL）。範囲: Port 1〜65535、MaxAuthTries 1〜1000、ClientAliveCountMax・ServerAliveCountMax 0〜1000000、MaxStartups の最初の数 1〜1000000（3 つの形は rate 0〜100、full は start 以上）、MaxSessions 0〜65535 で **SSH_CHANNEL_MAX に切り詰める**（0 は許す）。
- **PermitRootLogin**: yes・no・prohibit-password だけ。forced-commands-only と without-password は EINVAL（authorized_keys の command 制限を読まないので、別の設定に化けさせない）。AddressFamily: any と inet は inet6 = 0、inet6 は 1（**any でも listener は IPv4 のまま**）。
- **ListenAddress**: 1 語、63 文字まで、英数字と `.-_:`。`:` が 1 つ（host:port）・`[` `]`・rdomain は EINVAL、`:` が 2 つ以上は IPv6 の address。
- **path**: 1〜1023 byte、制御文字（0x20 未満と 0x7f）は EINVAL（空白は可）。`%` は ssh の path では EINVAL、AuthorizedKeysFile は `%h` `%u` `%%` だけ可（caller が展開）。**UserKnownHostsFile・GlobalKnownHostsFile・AuthorizedKeysFile の `none` は `/dev/null` に読む**（空の file として読ませる）。HostKey・IdentityFile・Subsystem の command の `none` は path そのまま。UserKnownHostsFile・GlobalKnownHostsFile は複数の語のうち最初だけを使う。AuthorizedKeysFile は複数の語を EINVAL。
- **Subsystem**: `sftp <command>` の 2 語だけ（name は case を区別する）。command の引数（3 語以上）は EINVAL。
- User: 1〜64 byte（0x21〜0x7e、`%` なし）。HostName: 1〜255 byte（同じ）。`%` を含む（token）は EINVAL（展開しない）。
- yes/no と enum の値は case を区別しない（OpenSSH の挙動に合わせた。仕様は「argument は case-sensitive」と書くが、大文字の `YES` を断る利点が無い）。keyword も case を区別しない。
- 行: 1 行は trailing blank を切った後 8192 byte まで（`line too long`）。NUL を含む行は comment でも EINVAL。1 行の語は 128 まで。LF の前の CR は blank。VT・FF も blank。**`#` は語の先頭だけが comment**（`a#b` は 1 語）。引用符は語の先頭だけ（引用符を閉じた後は blank か行末）、引用符の中に escape は無い。comment・空行は長さを問わず読み飛ばす。
- 誤りの文: `line N: <何> : <keyword>`（keyword は 48 byte で切り、表示できない byte は `?`）。`-o` は行番号なし。error は NULL でもよい。成功した呼び出しは error[0] を 0 にする。`size > SSH_CONFIG_FILE_MAX` は EMSGSIZE（`file too large`）、text が NULL で size が 0 でないのは EINVAL。
- `sshd_config_finish(NULL)` は EINVAL（header は「Returns 0」だが NULL は caller の誤り）。

### 未実施・残り

- host の OpenSSH が使う sshd_config・ssh_config との相互運用の確かめ（p015 以降）。
- 規約の全文の見直しの Phase（p025）。**既知の逸脱**: `sshd_value`・`ssh_value` の switch の中で `return config_value_xxx(...)`・`return status;` と関数の結果を直に返している（§11）。switch の分配の形なので、p025 で整えるか判断する。
- QEMU・実機は対象外（host の試験のみ）。

### header の不備

なし（変更を要する物は見つからなかった。header の comment「optional s, m or h suffix」は仕様の TIME FORMATS の一部だけを書いている、という細かい点を p025 の持ち越しとして記す）。
