<!-- awesome-plan project=zedbsd record=ws336-p019 -->

# ws336-p019: sftp-server の program

Status: in-progress（2026-11-02 P6 q1975。2026-11-02 17:59 実装済み・試験 PASS、cleared は Q1）。前: planned（2026-11-01 p001 の設計で作成）
Parent: [WS336](../ws.md)
担当（推し）: phase-runner-low（Sonnet 5.5 high）
依存: p013・p018
WS335 の待ち: 無し。Q3 は A（sftp-server だけ、旧の scp の受け手は作らない、2026-11-01）

## 1. 読む物

- `AGENTS.md` の §4（担当の共通の規則）・§7（禁止と承認）・§9（検証）・§10（コード）
- `plan/coding-style.md` の全文（ANSI C、宣言の位置、comment、§14 の checklist）
- `plan/guardrail.md` の「MIT・GPL の出典のデータの引用」と「source の中の開発の管理情報」
- `docs/agent/spec-module-delegation.md` の §5.3（実装の担当が守ること）と §7（出典と license）
- `userland/base/ssh/ssh-common.h` の冒頭の約束（誤りの値・信用できない入力・秘密・ANSI C）
- `userland/base/ssh/ssh-sftp.h`（SYMLINK の引数の順の注意）
- `plan/ws336/design.md` §4.2・§11.3
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/draft-ietf-secsh-filexfer-02.txt`（SHA-256 `3f69664d8050a28a…`、全桁は同じ directory の SHA256SUMS）: §6・§7
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-PROTOCOL`（SHA-256 `4a9e7f6d26d3f59d…`、全桁は同じ directory の SHA256SUMS）: §4.1（SYMLINK の逆順）
- `/home/awe/zedBSD-claude1/build/sources/specs/ssh/openssh-10.5p1-scp.1`（SHA-256 `32966f40938f813b…`、全桁は同じ directory の SHA256SUMS）: SFTP mode と -O
- `plan/ws336/design.md` の §1（出典の扱い）・§5（module）・§8（安全）

## 2. 作る物

- `userland/base/ssh/sftp-server-main.c`
- `plan/ws336/tests/interop-host.sh` に scp・sftp を足す
- この phase.md の「結果」の節、[ws.md](../ws.md) の表のこの行

## 3. 試験の項目

期待の値は仕様の表・式・試験の vector と手の計算、`plan/ws336/tests/unit/wire.h` で組んだ bytes だけから作る。他の実装の出力を期待の値にしない（host の OpenSSH との相互運用は別の確かめで、期待の値の生成ではない）。

- host の OpenSSH の scp（既定の SFTP mode）で 8 MiB の往復の cmp、`scp -r` の directory、`scp -p` の時刻と mode
- `sftp -b` で ls・put・get・mkdir・rmdir・rename・rm・symlink（OpenSSH の順で正しい向きの link ができる）・readlink
- 権限の無い file で PERMISSION_DENIED、無い file で NO_SUCH_FILE
- 壊れた packet で落ちない（sftp-server を stdin に random な bytes で 1 万回起こす）

## 4. 注意

- handle は sftp-server の中の表の番号（4 bytes）。開ける数の上限を決めて結果に書く。
- 他の実装（OpenSSH・libssh・Dropbear・PuTTY など）の code・comment・構造・名前を写さない。仕様の本文から書く。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）を書かない。

## 5. 変えてはいけない物

全ての header（`userland/base/ssh/*.h`）、library の module の .c、`plan/ws336/tests/` の道具（`run-unit.sh`・`check-skeleton.sh`・`crypto-contract.*`・`unit/check.h`・`unit/wire.h`・`host-crypto/`）、Makefile（この WS はまだ Makefile を置かない）、HAL・toolchain・共有の `build/`、他の WS の file。header の不備（足りない関数・誤った約束・矛盾）を見つけたら直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、この phase.md の「結果」に列挙する。phase.md と仕様が食い違えば仕様に従い、食い違いを報告する。

## 6. 確かめの command

- `sh plan/ws336/tests/interop-host.sh`
- `sh plan/ws336/tests/run-unit.sh all`
- `BASE=<header の基点> sh plan/ws336/tests/check-skeleton.sh`
- `git diff --check`

## 7. 終わりの条件

上の command が全部 PASS（出力の最後の行を自分で読む）。試験が通った後、実装を git の外でわざと数通りに壊し（境界の比較を 1 つずらす、byte 順を逆にする、範囲の確かめを 1 つ外す など）、試験が FAIL することを確かめて元に戻す（`cmp` で戻ったことを確かめる。見逃しがあれば試験を足す）。規約の checklist（coding-style §14）で自分の diff を見直す。表の値には block ごとに複数行の「Values referenced:」の comment。commit は新しい file を `git add` してから `git commit -m WIP -- <この Phase の path>`。この phase.md の「結果」の節（日付は host の `date`、実行した command と最後の行、件数、自分で決めた点、未実施）と [ws.md](../ws.md) の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が決める）。push しない、rm しない。

## 結果

2026-11-02 17:59（host の `date`）。**実装済み・試験 PASS**（cleared は Q1 が決める）。P6、worktree `p6-ws336`（branch `agent/p6-ws336`、基点 main 51d0669efb）。凍結の header（`ssh-*.h`・`sshd.h`）と library の module は変えていない。

### 作った物

- `userland/base/ssh/sftp-server-main.c`（骨格の置き換え）: SFTP v3 を標準の入出力で（sshd の subsystem）。最初の packet は INIT で VERSION 3（拡張なし）、INIT の前の要求は答えずに 1 で終わる。OPEN・CLOSE・READ・WRITE・LSTAT・STAT・FSTAT・SETSTAT・FSETSTAT・OPENDIR・READDIR・REMOVE・MKDIR・RMDIR・REALPATH・RENAME・READLINK・SYMLINK、EXTENDED と未知の型は OP_UNSUPPORTED、返事の型は BAD_MESSAGE、壊れた要求は型と id が読めれば BAD_MESSAGE で続ける、長さ 0 や上限超えの枠は 1 で終わる。
- 試験の道具: `plan/ws336/tests/sftp-host.py`（新: sftp-server の標準の入出力に直に、draft から手で組んだ bytes との比較、構造つきの壊れた入力、random な入力）、`plan/ws336/tests/sftp-fs-stub.c`（新: random な入力を流す build の全部の file system の呼び出しを system call 無しで答える stub）、`plan/ws336/tests/interop-host.sh`（相手 B の Subsystem を自前の sftp-server に替え、scp・sftp と上の 2 つを足した）。

### 試験の作り（host を触らない）

- scp・sftp は p018 と同じく相手 B（自前の sshd、試験の shim）を通る。sftp-server は試験の account の home の中で動く。
- 壊れた入力（Q1 の指示、2026-11-02）: **構造つき**の 5000 回は path を `a b c ab bc ca` の 6 つだけにし（`/`・`.`・`..` が無い）、空の試験の directory を cwd にして実の file system で。**完全に random** の 5000 回は `sftp-fs-stub.c` を link した build にだけ流し、file system の呼び出し（open・stat・unlink・rename・chmod・chown・truncate・utimensat・opendir・realpath・readlink・symlink・link など全部）は stub が偽の descriptor・偽の directory・"/stub" で答えて実の file に届かない。fail closed: `sftp-server-main.c` だけの object の未定義の symbol を `nm -u` で読み、stub が答える物と file system に触れない libc の一覧（malloc・read・write・snprintf など）の外が 1 つでもあれば FAIL で止め、stub の build を作らない（実際に malloc・strcmp・strrchr、signal を一覧に足すまで止まった）。

### 試験の中身

- 相手 B（OpenSSH 10.0p2 の scp・sftp → 自前の sshd → 自前の sftp-server）: scp（既定の SFTP mode）で 8 MiB の上りと下りの cmp、`scp -r` の 3 段の tree の上りと下りの `diff -r`、`scp -p` の mode と時刻の上り（640・1000000000）と下り（604・1100000000）、`sftp -b` の mkdir・put・rename・ls・get、mkdir・rmdir、put・symlink（link が target を指す正しい向き）・rm（file は消え link は残る）、無い file の get で「not found」（NO_SUCH_FILE）、権限の無い file の get で「Permission denied」。
- protocol（`sftp-host.py raw`、期待の bytes は draft-ietf-secsh-filexfer-02 の 3・4・5・6・7 と OpenSSH PROTOCOL 4.1 から手で）: INIT → `00 00 00 05 02 00 00 00 03`、INIT の前の要求は答え無しで 1、NO_SUCH_FILE（2）・PERMISSION_DENIED（3）、SYMLINK（target、新しい link の順）と READLINK の NAME の bytes、既存の名前への RENAME は FAILURE、拡張は OP_UNSUPPORTED（8）、壊れた READ は BAD_MESSAGE（5）と id、表の外の handle と 5 bytes の handle は FAILURE、handle の bytes（`00 00 00 00`）、READ の DATA と EOF（1）、READ 65537 は 65536 まで、REALPATH の `.`・`""`・まだ無い名前、無い directory の下は NO_SUCH_FILE、STAT の ATTRS の bytes（atime と mtime が別）、LSTAT の link の型の bit、OPEN CREAT の権限、TRUNC、FSETSTAT の権限、SETSTAT の大きさ・権限・両方の時刻、READDIR が 5 項目を 1 回で返し次は EOF、RMDIR・REMOVE、64 個の handle の後の 65 個目は FAILURE で CLOSE の後にまた出る、開いたままの handle は終わりに閉じる、長さ 0 の packet で 1、引数があれば 1。
- 壊れた入力: 構造つき 5000 回と random 5000 回（合わせて 1 万回）で、どれも終わりの値が 0 か 1、ASan・UBSan の報告無し、20 秒の上限内。

### 実行した command と最後の行

- `sh plan/ws336/tests/interop-host.sh` → `interop-host: 161 checks, 0 failed` / `interop-host: PASS`（相手 A・B 全部、壊れた入力の 1 万回を含め約 20 分）
- `sh plan/ws336/tests/run-unit.sh all` → `run-unit: PASS`（18 本）
- `BASE=0e9b68f948 sh plan/ws336/tests/check-skeleton.sh` → `check-skeleton: PASS`
- `git diff --check` → 出力なし

### わざと壊す確かめ

`build/tmp/p019-mut/mut.py`・`mutants.py`・`check.sh`。全体の interop は 1 回 200 秒を越え 150 秒の上限に入らないので、相手 B から sftp-server の部分を切り出した `check.sh`（自前の sshd は 1 度だけ build して写し、変異ごとに mutated の sftp-server・stub の build、scp 1 MB・`-r`・`-p`・`sftp -b`・protocol の確かめ・構造つき 200 回・random 200 回）で流した（1 回約 30 秒）。worktree の file は変えない。RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 s、150 s と RSS 6 GB で kill、同時 3。

- 1 回目（43 通り）: 23 が FAIL、20 が生き残り。基点の run が構造つきの入力の 1 つで SIGXFSZ（RLIMIT_FSIZE の下の大きな offset の WRITE）で落ちたので、sftp-server が SIGXFSZ を無視して EFBIG を FAILURE で答えるようにした。protocol の確かめを足した（上の 2 回目の run の項目）。
- 2・3 回目（生き残りだけ）: 14 が FAIL に変わった。
- **最後: 43 通りのうち 37 が FAIL、6 は同値**: 新しい handle の探しを 1 つ広げて範囲の確かめを残す（作った変異が同値）、READ の長さの上限を 1 ずらす（parser が既に 64 KiB に切る、ssh-sftp の p013 の決定）、WRITE の部分の書きを続けない（普通の file では起きない）、RENAME の link の道を飛ばす・EEXIST の確かめを外す（どちらも lstat の確かめが同じ答えを出す）、終わりの closedir を外す（handle の表から届くので leak と数えられない、descriptor は process の終わりで閉じる）。

結果は `build/tmp/p019-mut/run1.log`・`run2.log` と `*results.json`。

### 自分で決めた点

- **handle**: 4 bytes の big-endian の表の番号、上限 64（`SFTP_HANDLES_MAX`）。範囲外・空き・別の種類（file と directory）の handle は FAILURE「Invalid handle」。
- **INIT**: 最初に要る、2 回目は答えない。INIT の前の要求は答えずに終わる（client と話が通じない）。
- **状態の code**（7）: ENOENT・ENOTDIR・ELOOP・ENAMETOOLONG は NO_SUCH_FILE、EACCES・EPERM・EROFS は PERMISSION_DENIED、他は FAILURE。文は英語の短い文。
- **OPEN**: 6.3 の flag をそのまま open に、新しい file は attrs の権限（無ければ 0666、umask が効く）、directory は「Is a directory」で断る。
- **READ・WRITE**: READ は 1 回 64 KiB まで（parser の上限）、off_t に入らない offset は EOF・FAILURE。WRITE は全部を書く。
- **SETSTAT**: 大きさ・owner と group・権限（07777）・両方の時刻を、この順に、最初の失敗で止めて答える。
- **READDIR**: 1 回 64 項目まで、`.` と `..` も含む、long name の owner・group は名前（無ければ番号）。
- **RENAME**（draft 6.5: 既存の名前を置き換えない）: file は link と unlink（既存なら EEXIST）、link できない物（directory）は lstat で空きを確かめてから rename（間に競合の窓が残る）。
- **SYMLINK**: OpenSSH の順（target、新しい link の path）。
- **REALPATH**: 空の path は cwd。最後の名前がまだ無い時は親を realpath して名前を繋ぐ（OpenSSH の scp の upload が送り先の名前で REALPATH を問うため）。親が無ければ NO_SUCH_FILE。
- **SIGXFSZ** を無視する（上の変異の確かめの節）。
- **引数**: 受けない（usage と 1）。sshd の Subsystem は引数を持たない（ssh-config の約束）。

### 未実施・残り

- root でない host でだけ試した。zedBSD の上（QEMU）での scp・sftp は p022。
- 拡張（posix-rename・statvfs・fsync・hardlink・limits など、ws.md の T6）は作らない。
- 規約の全文の見直しは p025。ここでは coding-style §14 で自分の diff を見直した。
- 消す物（Q1 に頼む）: `build/tmp/p019-mut/`、`build/tmp/sftp-dbg`・`sftp-dbg2`、`build/tmp/b-run.txt`・`full-run*.txt`、`build/tmp/interop-host.run.*` の古い run。
