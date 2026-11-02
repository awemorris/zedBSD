<!-- awesome-plan project=zedbsd record=ws336-p020 -->

# ws336-p020: 敵対的な入力と privsep の見直し

Status: cleared（2026-11-03 Q1、ws.md の表のとおり）。前: in-progress（2026-11-02 18:04 P6 q1983。2026-11-02 19:20 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner（Opus）
依存: p018・p019
WS335 の待ち: 無し

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `plan/ws336/design.md` §6.2・§8・§9
- sshd・ssh・sftp-server の全 source
- [design-review.md](../design-review.md)（見送った指摘と受け入れた危険）
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `plan/ws336/tests/fuzz-host.sh`（各 parser と transport・auth・channel・monitor の request の確かめ・sftp に、仕様の形を保った変異の入力を ASan・UBSan で長く流す 1 本の script。最後の行 `fuzz-host: PASS`）
- 見つけた不具合の修正（library の .c の修正は可。header の修正が要る時は Q1 に返す）
- 見直しの記録（この phase.md の結果）
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- 各 parser 100 万回以上（host の時間で 30 分以内）
- monitor: 乗っ取られた pre-auth・session を模した request の列で許可が出ない事（p016 の試験の延長）
- 時間の差: 存在する user と存在しない user、正しい password と違う password の応答の時間の差が 50 ms 未満（sshd.h の Timing の規則で）
- descriptor の漏れ: shell の子に monitor の socket・listener の socket が無い

## 4. 注意

- clang の libFuzzer は toolchain の変更なしに host の clang で使えるなら使ってよい（host の clang は Debian 19.1.7）。使えなければ自前の変異の loop。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、（library の .c の不具合の修正は可）、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/fuzz-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 19:20（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main 70fc667564）。凍結の header（`userland/base/ssh/*.h`）・試験の道具（run-unit.sh・check-skeleton.sh・crypto-contract.*・unit/check.h・unit/wire.h・host-crypto/）・HAL・toolchain は変えていない。host の clang 19.1.7 の libFuzzer を toolchain の変更なしで使った。

### 作った物

- `plan/ws336/tests/fuzz-host.sh`（1 本の script、最後の行 `fuzz-host: PASS`）:
  1. libFuzzer の 11 個の target（`plan/ws336/tests/fuzz/fuzz-*.c`、ASan・UBSan、leak も FAIL）: common（name-list・base64・RFC 4251 の reader）、kex（KEXINIT・選択の両側と初回／再交換・hybrid の init と reply）、key（public の blob と行、private key の file、署名の検証）、auth（service・request と署名の data・failure・pk_ok・banner）、channel（server の channel の表に message の列、pty-req の ttymodes を含む）、config（sshd_config・ssh_config・`-o`）、hostfile（authorized_keys・known_hosts の確かめと削除・pattern）、sftp（packet と要求）、privsep（monitor の frame の流れ）、transport（先頭の byte で server か client、受けた bytes を 1500 byte ずつ、上の layer の送りと tick）、monitor（下）。各 `FUZZ_RUNS`（既定 3000000、100 万未満は FAIL）。seed は仕様から組んだ bytes（識別の行と鍵の無い KEXINIT、RFC 4254 の CHANNEL_OPEN など）。
  2. sftp-server の program: random の bytes は file system の呼び出しを全部 `sftp-fs-stub.c` に置いた build だけへ（`nm -u` の許した名前の表の外の記号があれば build せずに FAIL、fail closed）。名前を持つ要求は `/` の無い短い名前だけを空の directory で（`sftp-host.py`）。各 `SFTP_RUNS`（既定 10000）。
  3. 時間の差（`plan/ws336/tests/timing-host.c`）。
  4. descriptor の漏れ: 自前の sshd（interop-host.sh の shim、127.0.0.1、port が test 自身の process の物かを `/proc/net/tcp` と process の木で確かめ、違えば FAIL）で host の OpenSSH の ssh から command と `-tt` の session を流し、shell の子（`ls`）から shell の `/proc/<pid>/fd` を読む。
- `fuzz/fuzz-monitor.c`（乗っ取られた子を模した request の列）: `sshd-monitor.c` を link し、account の確かめを置き換える（`fuzzuser` uid 4242 だけ、鍵は RFC 8032 7.1 TEST 2 の public key だけ、password は秘密の 32 byte の SHA-256 と比べる、login record は呼ばれたら abort）。host の account・file には届かない。`LLVMFuzzerInitialize` で本物の署名つき login が GRANTED になることを確かめる（置き換えが効いている証拠）。入力の先頭の byte で始めの段（新しい接続・SIGN の後・SESSION の段）を選び、frame ごとに不変の条件を見る: GRANTED にならない、SESSION の段に新しくならない、tries ≤ MaxAuthTries、pre-auth の署名 ≤ `SSHD_PREAUTH_SIGN_MAX`。
- `timing-host.c`: `geteuid`（0）・`getpwnam_r`・`getspnam_r`（test の account の alice（"right" の SHA-512 crypt）と locked（"!" つき））・`syslog` を置き換え、本物の `sshd_monitor_handle`・`sshd-login.c`・`login/verify.c`・host の crypt を 1000 ms の delay のまま流す。7 通り（alice の正しい・違う・空の password、locked、存在しない user、鍵の問いの alice と存在しない user）×5 回、全部が `SSHD_FAIL_DELAY_MS` 以上で、中央値の差が 50 ms 未満。
- **見つけた不具合の修正**（log の注入）: 相手が選んだ user の名前（client から pre-auth へ、pre-auth から monitor へ）と session の子が送る terminal の field が、そのまま syslog に入っていた。改行で偽の行を作る、ESC で log を見る端末に制御の列を送ることができた。`sshd-monitor.c`・`sshd-login.c`・`sshd-preauth.c` に static の `*_log_text`（printable ASCII 0x20〜0x7e の外の byte を 1 byte ずつ `?` に、領域で切る）を置き、相手の選んだ文字列を log に書く 9 か所（invalid user・not allowed・signed data・Failed publickey・Failed password・login record・logout record の拒否・too many authentication failures）で通す。header を変えないため、3 つの file にそれぞれ static で置いた（同じ 20 行）。
- 試験の追加: `unit/test-monitor.c`（3 checks: account の確かめの log、Failed password の log（改行・ESC・BEL・DEL・UTF-8 の 2 byte）、login record の拒否の log の terminal）、`sshd-pair-host.sh`（「name in the log」: 改行と ESC を持つ user の名前で 3 process の log の行がどれも `?` になり、process id で始まらない行も ESC の byte も無い）、`unit/test-channel.c`（3 checks、下の変異の確かめで見つけた: 表の直後の番号 `SSH_CHANNEL_MAX` の OPEN_CONFIRMATION と find が、表の後ろの field の値によらず拒まれる）。

### 実行した command と最後の行

- `sh plan/ws336/tests/fuzz-host.sh`（18:40〜19:06、約 26 分）→ `fuzz-host: PASS`。11 target 全部 `exit 0, 3000000 runs`（monitor 339 秒、hostfile 300 秒、transport 181 秒、config 93 秒、他は 12〜37 秒）。`sftp-server random: ok 10000 runs, none crashed or stopped (0 failed)`、`sftp-server names: ok 10000 runs ... (0 failed)`。時間: 7 通りの中央値と最小・最大が全部 1000 ms、`spread of the medians: 0 ms`、`timing: PASS`。descriptor: command は `0 1 2` が pipe 3 本だけ、端末の session は `0 1 2` が `/dev/pts/4` だけ、socket は無い。（この run の途中で log の helper の comment と空行だけを足した。fuzz の target はその前に build、時間と descriptor の build は後。動作は同じ。）
- `sh plan/ws336/tests/sshd-pair-host.sh` → `ok name in the log` / `sshd-pair-host: PASS`（105 秒）
- `sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`（18 本。test の追加の後の 19:23 の最後の run。monitor 1453 checks、channel 1603654 checks）
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p020-mut/mut.py`・`check.sh`（worktree の file は書き換えず、`userland/base/ssh` の写しを変異ごとの新しい directory に作って 1 か所を変え、その写しで target を build・実行。fuzz の target は fuzz-host の run の corpus から 35 秒、unit は run-unit と同じ flag、時間は timing-host 2 回、descriptor と pair はそれぞれの script の写しを写しの source で。RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s、150 秒と RSS 6 GB で kill、同時 4）。

- 1 回目（30 通り）: 17 が FAIL、13 が生き残り。fuzz の 35 秒で FAIL: reader の長さの確かめを 1 ずらす 2 通り、config の語の数、hostfile の offset。monitor の 7 通り（署名の検証を通す、password の `&&` を `||`、tries と pre-auth の署名の上限を 1 ずらす、user の変更を許す、署名の data の blob の長さを見ない、GRANTED の段で PASSWORD_LOGIN を受ける）は fuzz-monitor では見つからず test-monitor で FAIL（fuzz-monitor の不変の条件は、正しい形の login を乱数で組めないため、ここでは生き残りの見張りでしかない）。時間の 2 通り（password の正しい時に待たない、鍵の問いを 100 ms 早く）は timing-host で FAIL。log の 3 通りと descriptor（session の `closefrom(3)` を 5 に）は FAIL。
- 2 回目（生き残り 13 に module の unit を先に足して）: 10 が FAIL（cstring の領域・name の長さ・base64 の領域・鍵の comment・sftp の待ちと path・privsep の型の上限・packet の待ちと padding は各 module の unit が捕まえる。monitor の log の DEL を見逃していたので test-monitor の名前に DEL と UTF-8 を足した）。channel の 2 通り（番号の確かめ `>=` を `>`）は表の直後が同じ struct の `no_more_sessions` で ASan に見えず生き残り → test-channel に表の直後の番号の check を足した。
- 3 回目（channel の 2 通り）: 2 とも FAIL。
- **最後: 30 通りのうち 29 が FAIL、1 は同値**: `sshd-child.c` の `closefrom(SSHD_FD_MONITOR + 1)` を広げる変異は、monitor が子を execve する前に同じ closefrom をしている（`monitor_exec_child`）ので二重の守りの片方で、観測できない。

結果は `build/tmp/p020-mut/run1.log`・`run2.log`・`run3.log` と各 `results.json`。

### 見直しの記録（privsep と敵対的な入力）

- **monitor**: request の段（PREAUTH・GRANTED・SESSION）ごとに受ける型が決まり、それ以外は EPROTO で接続を終える。GRANTED へは署名の検証か password の一致の後の `monitor_grant` だけ。fuzz-monitor の 300 万回で不変の条件は破られなかった。
- **時間**: 全部の応答が要求から 1000 ms 後（`monitor_wait_until`）。存在しない・locked の user も crypt を 1 回流す（test-monitor が数える）。差は 0 ms。
- **descriptor**: 子の program は monitor の `closefrom(SSHD_FD_MONITOR + 1)` の後に execve、shell は session の `closefrom(3)` の後。listener の socket は `FD_CLOEXEC`。command・端末の両方で shell は 3 本の流れだけ。
- **log**: 上の修正。peer の address は getpeername の数字の形で、相手は選べない。sshd-session.c の user の名前（monitor が許した account の名前）と login の記録の tty（`sshd_tty_path_ok` を通った物）は相手の選んだ文字を含まないので変えていない。
- 見つからなかった物: parser の memory の誤り・UB・leak・timeout は 11 target の 3300 万回で 0。sftp-server の random と構造つきの 2 万回で落ちる・止まるは 0。

### 自分で決めた点

- log に書く相手の文字は printable ASCII（0x20〜0x7e）だけを残し、他の各 byte を `?` にする（UTF-8 の名前も byte ごとに `?`。ssh-auth.h の banner の置き換えは LF と TAB を残すので、log の行には使えない）。名前を拒むのではなく log の時に置き換える（login の動作は変えない）。
- fuzz の monitor は account の確かめを置き換える（本物の account の確かめは test-monitor と timing-host が見る）。
- fuzz の時間の上限は target ごとに 1500 秒、全体で 30 分の内（今回 26 分）。

### 未実施・残り

- root でない host でだけ試した。root の privsep の本番の形（account の切り替え・chroot の無い確かめ）と zedBSD の上は p022。
- 時間の差は host（Linux）で測った。zedBSD の上の時間は p022。
- 規約の全文の見直しは p025。ここでは coding-style §14 で自分の diff を見直した（宣言は関数の先頭、loop と呼び出しに comment、split した for の 3 つの式、loop の後の空行）。
- 消す物（Q1 に頼む）: `build/tmp/p020-mut/`（変異の写しと build、45 の directory と corpus の写し）、`build/tmp/p020-dbg/`、`build/tmp/fuzz-desc.run.*`、`build/tmp/fuzz-host.run.*` の古い run（最新を残してよい）、`build/tmp/p020-fuzz.txt`・`build/tmp/p020-pair.txt`、`build/tmp/sshd-pair.run.*`・`build/tmp/ssh-unit.run.*` の古い run。
