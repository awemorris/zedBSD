<!-- awesome-plan project=zedbsd record=ws056p001 -->

# ws056-p001: BUG-034・035・037 の修正

Phase ID: `ws056-p001`
Parent: [WS056](../ws.md)
Status: uncleared
Queue: q423-i01

## 受け入れ

- `POSIX-R2.ELF` と `POSIX-R2-REMAINING.ELF` が build でき、guest で status 0。
- guest の pax が、GNU tar の `--format=pax` と `--format=gnu` で作った coreutils の source（長い path を含む）を展開して、file の数と内容が host と一致する。
- 新しい・変えた code は規約の全文（`style-check.py`）。kernel の build（`limits.h` は uapi）と boot、guest の make の差分試験（pax を使う）。

## 設計（q423-i01）

- BUG-034: `include/uapi/limits.h` の `RTSIG_MAX` を 34 → 33。`SIGRTMIN` 30・`SIGRTMAX` 62（`<uapi/signal.h>`）で `sysconf(_SC_RTSIG_MAX)` は `SIGRTMAX - SIGRTMIN + 1` = 33 を返していたので、header の定数が誤り。kernel・libc に他の参照は無い（`getconf` は sysconf 経由）。
- BUG-035: `userland/base/tests/posix-r2-remaining.c` の `struct atomic_record` を `uint32_t counter; uint16_t left; uint16_t right;`（8 byte）に。試験の意味（counter の CAS の増分と left・right の保持）は同じ。
- BUG-037: `userland/base/pax/main.c` の読み手に `struct extension`（次の member の path・linkpath・size・mtime の上書き）を足し、typeflag `x`（pax の拡張 header: `"LEN key=value\n"` の record。path・linkpath・size・mtime を取り、他の keyword は無視、mtime の小数は捨てる）、`g`（global header: 本体を読み飛ばす。GNU tar は comment しか置かない）、`L`・`K`（GNU の長い名前・link 名: 本体がそのまま名前）を読む。
  `read_archive()` を、header の読みと選択（`read_archive`）と member の作成（`extract_member`・`extract_directory`・`extract_link`・`extract_fifo`）に分けた。拡張 header の本体は 1 MiB まで（`EXTENSION_MAX`）。
- 実行中に見つけた 2 件（受け入れ「両方の ELF が guest で status 0」の中なので同じ attempt で直す。範囲の拡大ではない）:
  - [BUG-042](../../bugs/BUG-042.md): `POSIX-R2.ELF` が BUG-034 の先で SIGUSR1 に殺される（status 144）。libc の SIGEV_THREAD の worker（`timer_worker`）が program の signal を block せず、process 宛の SIGUSR1 が worker に配られて既定の動作。`userland/base/libc/timer.c` の `timer_service_start_locked()` で `pthread_create` の前後に raw `sigprocmask` で全 block・復元。
  - [BUG-043](../../bugs/BUG-043.md): `POSIX-R2-REMAINING.ELF` の scm-emfile が `KERN_OPEN_MAX` 1024 では EMFILE に届かない（試験の前提の誤り）。試験が `RLIMIT_NOFILE` の soft limit を 16 に下げてから埋め、再試行の後で戻す。
  - BUG-042 の真因（probe `/tmp/vitest/sigprobe` で確定、詳細は ticket）: kernel の `thread_create(2)` が新しい thread の `signal_mask` を 0 で始めて作った thread の mask を継承しない。`src/kern/syscall.c` の `sys_thread_create_call()` で `thread_start()` の前に `thread->signal_mask = curthread->signal_mask`（kernel の変更、HAL ではない）。libc の worker の全 block も残す（library の thread として要る）。
  - `POSIX-R2-REMAINING.ELF` の 09（fexecve）は自分が `/bin/posix-r2-remaining` に置かれている前提。試験の実行時に `/bin` へ置いて走らせる（image には入っていない。試験の前提であり誤りではない）。
  - [BUG-044](../../bugs/BUG-044.md): 同 ELF の 11 posix-close が `posix_close()` の返り値に errno の値を期待していた（libc は POSIX どおり -1 と errno）。試験を直す。

## 結果（q423-i01、2026-09-25）

### host の証拠（pax の読み手を host で native に compile して確認）

試験の木: 名前 120 byte（ustar の name を超える）、prefix 180 byte 超の深い directory、100 byte 超の symlink の先、hard link、fifo、小数の mtime の file、prefix だけで足りる名前、coreutils 9.12 の `src`・`tests`（file 1101、directory 81、symlink 2）。
GNU tar 1.35 で `--format=pax`（`x` 1185・`5` 81・`0` 1100・`6` 1・`2` 2・`1` 1）、`--format=gnu`（`L` 5・`K` 1）、`--format=ustar`（短い名前だけ）の archive を作り、
`cc -std=gnu99 -D_GNU_SOURCE -Wall -Wextra` で host 向けに compile した `main.c`（warning 0）で展開。

| archive | status | file の cksum（1101） | symlink の先 | directory | hard link | fifo | mtime | 一覧の行数 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| pax | 0 | 一致 | 一致 | 一致 | 同じ inode | p | 1580576522（host と同じ） | 1185（拡張 header は出ない） |
| gnu | 0 | 一致 | 一致 | 一致 | 同じ inode | p | 同上 | 1185 |
| ustar | 0 | 一致 | — | — | — | — | 同上 | 8 |

壊れた拡張 header: record の長さが短すぎる・長すぎる → `corrupt extended header`、status 1、無限 loop 無し。末尾の dangling な `x` → 捨てて status 0。2 MiB の本体 → `extended header too large`、status 1。
未知の keyword（uid）は無視、`mtime=1000000000.5` は 1000000000、`path`・`size` の上書きが効く。
試験の手順は [tests/guest-pax-test.sh](../tests/guest-pax-test.sh)、archive と期待の作り方は [tests/make-pax-archives.sh](../tests/make-pax-archives.sh)（coreutils の source の tar は `build/ws046/coreutils-src.tar`）。

### guest の証拠（QEMU amd64、`build/ws053-full-hal-guest`、8 GiB）

| 検証 | 結果 |
| --- | --- |
| build（guest の disk image、full LTO。`limits.h` の変更で guest の LLVM package も再 build） | status 0、我々の code の warning 0 |
| `POSIX-R2-REMAINING.ELF`・`POSIX-R2.ELF` の build（amd64、`-Werror`） | 通る（BUG-035 の `-Watomic-alignment` は消えた） |
| boot test（`plan/tools/boot-test.sh`） | PASS、`build/boot-test-amd64-q423/login.png`（最初の image）、`q423b`（libc の修正の後）、`q423c`（kernel の thread の mask の修正の後） |
| signal の probe（kernel の修正の後） | 全 block で作った thread の mask が作った thread と同じ `3ffffffffff7feff`、変種 3・4・6 も `sigtimedwait` が SIGUSR1 を受け取る |
| `POSIX-R2-REMAINING.ELF`（`/bin/posix-r2-remaining` に置いて実行、BUG-043・044 の修正の後） | **status 0、`R2R:01-12:PASS`**（12 の atomic は BUG-035 の目標） |
| guest の make の差分試験（case の archive を pax で展開） | 91/91（最初の image）、91/91（libc の修正の後の image） |
| guest の sh の差分試験（libc の修正の後の image） | 1388/1425、落ちる 37 件は q422 と同じ集合 |
| kernel の修正の後の image: sh の差分試験 | 1388/1425、落ちる 37 件は q422 と同じ集合 |
| kernel の修正の後の image: make の差分試験・`SMP-STRESS.ELF` | 91/91。stress は起動直後の 1 回が status 7（resource-baseline、[BUG-045](../../bugs/BUG-045.md)）、新しい guest で 3 回連続 0 |
| `POSIX-R2.ELF`（libc の reaper の全 block の後、SSH で実行） | mqueue の通知を通り、`conformance-identity`（stdout が `/dev/console` で login が root であることを要る）で status 1。console で実行して確かめる |

guest の最初の展開の試み（NVMe の 1 GiB の作業 volume、`/work`）: 拡張 header は読めて directory 81 は全て作られ一覧も 1185 だったが、path 307 byte の file と 303 byte の symlink の先が `File name too long`。zedBSD の `PATH_MAX` は 256（POSIX の最小）で、試験の木が system の限界を超えていた（pax・kernel の誤りではない）。木を path 256 byte 未満（name 110 > 100、prefix 166 > 155、link の先 119 > 100）に作り直して再試験。guest の `/root`（31 MiB、大きい block）では 1101 file の展開が入らないので作業 volume を使う。

作り直した木での guest の展開（`build/ws056-work2.img` を `/work` に mount、`tests/guest-pax-test.sh`）:

| archive | status | file の cksum（1103） | symlink の先（2） | directory（85） | hard link | fifo | mtime | 一覧 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| pax（`x` 1191） | 0 | 一致 | 一致 | 一致 | 同じ inode | p | 1580576522 | 1191 |
| gnu（`L` 9・`K` 1） | 0 | 一致 | 一致 | 一致 | 同じ inode | p | 同上 | 1191 |
| ustar | 0 | 一致 | — | — | — | — | 同上 | 8 |

（比較は cksum の行を両方 `sort` して `cmp`。guest の `find | sort` は path 順、host の期待は行順だったので最初の比較は順序だけで違い、揃えて一致を確かめた。）

### 最終の image（全ての修正を含む libc.so・kernel・pax、`build/ws053-full-hal-guest`）での回帰

| 検証 | 結果 |
| --- | --- |
| build | status 0、我々の code の warning 0（libc の変更で guest の LLVM package が再 build、F-012） |
| boot test | PASS、`build/boot-test-amd64-q423d/login.png` |
| pax（pax・gnu の archive、作業 volume） | status 0、file・link・directory が host と一致 |
| `POSIX-R2-REMAINING.ELF`（`/bin/posix-r2-remaining`） | status 0、01-12 PASS |
| `POSIX-R2.ELF`（SSH） | `conformance-identity`（console が要る）まで通る |
| `POSIX-R2.ELF`（console） | `thread-timer-callback` EINTR（BUG-046） |
| make の差分試験 | 91/91 |
| sh の差分試験・`SMP-STRESS.ELF` | 最終 image では未実施（kernel の修正の後の image で 1388/1425・3/3。最終 image との差は libc の reaper と wake signal の block だけ） |

### 変更した file

- `include/uapi/limits.h`（BUG-034）、`userland/base/tests/posix-r2-remaining.c`（BUG-035・043・044）、`userland/base/pax/main.c`（BUG-037）。
- BUG-042: `src/kern/syscall.c`（`thread_create(2)` が mask を継承）、`userland/base/libc/timer.c`（worker を全 block で作る、callback の thread を wake signal を block して作る）、`userland/base/libc/pthread.c`（reaper を全 block で作る）、`userland/base/libc/posix.c`（`__libc_init` で wake signal 63 を block）。HAL は不変。
- 規約: 変えた行は `style-check.py` の報告 0（`clang-format` は host に無い。未実施）。

### 受け入れの判定と残り

| 受け入れ | 結果 |
| --- | --- |
| `POSIX-R2-REMAINING.ELF` が build でき guest で status 0 | **達成**（`/bin/posix-r2-remaining` に置いて実行、01-12 PASS） |
| `POSIX-R2.ELF` が build でき guest で status 0 | **未達**。BUG-034 の `realtime-signal-capacity` は通り、SSH では `conformance-identity`（console が要る）まで全て通る。console では SIGEV_THREAD の timer の試験が EINTR で落ちる（[BUG-046](../../bugs/BUG-046.md)、4/4、経路は未特定） |
| guest の pax が pax・gnu の archive を展開して host と一致 | **達成** |
| 規約・kernel の build・boot・make の差分試験 | **達成**（sh の差分試験も 2 回、同じ集合） |
| 実機 | 未実施 |

q423-i01 の outcome: **uncleared**（`POSIX-R2.ELF` の console の status 0 だけが残る）。ユーザーの判断: BUG-046 を non-blocking として Bug 追跡に移して clear するか、続きの Phase（p002: console の EINTR の経路の特定）を立てるか。
