<!-- awesome-plan project=zedbsd record=ws064p002 -->

# ws064-p002: 並列の make の性能

Phase ID: `ws064-p002`
Parent: [WS064](../ws.md)
Status: cleared
Queue: q449-i01（cleared）

## 目的

expat の `make -j4`（guest 4 vCPU）を host の `make -j4`（5.2 秒）と同等以上にする。並列にしたときの kernel（scheduler の配置、lock）と libc の費用を計測して詰める。WS064 の変更の規約の見直しは、2026-09-26 ユーザー「規約適合は最後でいいです」に従い [ws064-p003](../phase003/phase.md) に分けた。

## 受け入れ

WS064 の受け入れ。

## 進行の記録（q449-i01、2026-09-26）

### 出発点（QEMU 8 GiB 4 vCPU NVMe、ws064-p001 の make）

expat の `make -j4` 7.26〜7.81 秒、子の時間 user 9.88 秒・system 7.88 秒（4 CPU で平均 2.4 CPU が働く）。host は `-j4` 5.06 秒（同じ clang 23、64 core）。

QMP で全 vCPU の RIP を 300 回ずつ標本にした（`info registers -a`）: 4 CPU とも働いているのは 110/300。働いている標本の内訳は user 322、`spin_trylock` 124、`send_icr` 93、`sched_sleep_locked_interruptible` 39。`spin_trylock` の lock（RDI）は `metadata_lock`（VM の metadata の大域の mutex の guard）が 70/133。

### 1. mutex の短い回転

`mutex_lock_interruptible` は mutex が持たれていると、guard を取る前に `locked` を最大 256 回（`hal_atomic_relax` を挟む）見直し、その間に放されれば眠らずに取る（`src/kern/lock.c` の `MUTEX_SPIN_LIMIT`）。持ち主の thread の状態は見ない（持ち主が終わって解放された thread を読まない）。

| 測定 | 前 | 後 |
| --- | --- | --- |
| expat の `make -j4`（3 回） | 7.26〜7.81 秒 | 6.45〜6.67 秒 |
| 子の system の時間 | 7.88 秒 | 6.55〜6.91 秒 |

後の標本: `spin_trylock` 110（`metadata_lock` 62/120）、`mutex_lock_interruptible` の回転 73、`send_icr` 58。page fault・fork・exec・unmap の全部が 1 つの `metadata_lock` を 1 回の fault で 3 回ほど取る。並列の壁はこの lock。

### 2. VM の大域の lock の保持の内訳（一時的な計測）

`vm_metadata_enter`/`leave` に rdtsc と呼び出し元の表を一時的に入れて（commit しない）、expat の `make -j4` 1 回の保持時間を呼び出し元ごとに数えた: `vmspace_fork` 26〜29%（1 page ごとに lock を放して取り直す: 1 回の fork で約 380 回）、`vmspace_destroy` 25%（region ごとに page と backing の解放を lock の中で）、file の fault 19〜20%。

### 3. vfork と posix_spawn（2026-09-26 ユーザー「fork系にこだわらず、posix_spawnなども検討」「vforkもUAPIで公開」）

| 内容 | 場所 |
| --- | --- |
| system call `vfork`（169）: 子は親の address space を借り（`vmspace_ref`、同じ HAL space の task）、呼んだ thread は子の exec か終わりまで眠る（中断しない）。exec の差し替えの後と、子の最後の thread の退役で `process_vfork_release` が親を起こす。待ちは親の kernel stack の上。子の pid は子が走る前に控える（autoreap で子が先に消えてもよい） | `include/uapi/syscall.h`、`include/kern/process.h`、`src/kern/process.c`、`src/kern/exec.c`、`src/kern/syscall.c` |
| libc の `vfork()`（amd64 は assembly: 戻り番地を rdx に保って呼ぶ、失敗は `__vfork_error`、他の arch は fork）、`<unistd.h>` に宣言 | `userland/base/libc/syscall-amd64.S`、`src/libc/crt/crt0-amd64.S`、`userland/base/libc/posix.c`、`include/libc/unistd.h` |
| `posix_spawn`（amd64）: `__vfork_spawn(entry, arg, stack)` で子を 24 KiB の専用 stack で走らせ（親の stack に書かない）、全 signal を塞いだまま作り、子は handler のある signal を既定に戻してから呼び手の mask に戻して exec。exec の失敗は共有の request で親に返し、親が子を回収する。`ENOSYS` の kernel では従来の fork。libc の `sigaction` は handler を置いた signal を `__libc_caught_signals` に覚える | `userland/base/libc/posix.c`、`userland/base/libc/signal.c` |
| make の `spawn()` は `posix_spawn`（出力の dup2 は file action）。起動できなければ従来の fork で理由を出す | `userland/base/make/job.c` |
| sh の外部 command（前置の代入が無く、job control の無いとき）は `posix_spawn`（shell が自分で扱う signal は `POSIX_SPAWN_SETSIGDEF`）。それ以外は従来の fork | `userland/base/sh/jobs.c`（`sh_spawn`）、`exec.c`、`trap.c`（`sh_signals_shell_set`） |
| image の `/usr/lib/libc.so`（link の時に読む複写）が既定の build directory（`build/amd64/dynamic`、9/24 のもの）から取られ、別の `BUILD` の image では `/lib` より古かった（新しい `vfork` が link できなかった）。今の build の libc にした | `userland/packages/lang/clang/Makefile` |

### 4. fork と destroy の lock の保持を短く

| 内容 | 場所 |
| --- | --- |
| fork の private page の共有を 32 page ずつ: 1 組を lock の下で共有し BUSY にし、lock を放して親の PTE を読み取り専用に・子を map し、lock を取り直して 1 組をまとめて解放する（確かめることは 1 page のときと同じ） | `src/kern/vmspace.c`（`vmspace_fork_locked`、`struct vmspace_fork_entry`、`VM_FORK_BATCH`） |
| `vmspace_destroy` は lock の下で逆写像を外すだけにし（`detach_vm_page`）、backing と page の記録の解放は lock の外で（unmap と同じ 2 段）。page ごとの unmap はしない（space ごと壊す） | `src/kern/vmspace.c` |
| object page の逆写像の一覧から外すのを O(1) に（`vm_page.object_link`: 自分を指す link）。共有 library の page の一覧は process の数だけ長い | `include/kern/vmspace.h`、`src/kern/vm.c` |

### 5. kernel の mutex の速い道

mutex の `locked` を 3 状態（0 空き、1 保持、2 保持で眠る者がいるかもしれない）にした。空きの mutex は CAS 1 回で取り、放すのは交換 1 回で、2 だったときだけ guard を取って 1 人起こす。回転の後に眠る者は guard の下で 2 に交換してから眠る（交換で空きを見つけたらそのまま取る）。`mutex_wait` も同じ規約。今までは取る・放すたびに割り込みを止めて guard の spinlock を取っていた（`src/kern/lock.c`）。

### 結果の推移（expat の `make -j4`、guest、3 回）

| 段階 | 時間 | 子の system |
| --- | --- | --- |
| 出発点（p001） | 7.26〜7.81 秒 | 7.88 秒 |
| 1. mutex の回転 | 6.45〜6.67 秒 | 6.55〜6.91 秒 |
| 3. make が posix_spawn（vfork） | 6.03〜6.24 秒 | 6.1〜6.6 秒 |
| 3. sh も posix_spawn | 6.06〜6.29 秒 | 6.2〜6.4 秒 |
| 4. fork の一括・destroy の解放を外へ | 5.85〜6.02 秒 | 5.6〜6.1 秒 |
| 4. 逆写像の O(1) | 5.53〜5.87 秒 | 5.0〜5.5 秒 |
| 5. mutex の速い道 | **4.94〜5.00 秒** | 3.65〜3.97 秒 |

host（同じ時）: GNU make `-j4` 5.14〜5.18 秒、`-j1` 15.5〜15.7 秒。この make を host で jobserver 無しにすると `-j4` 15.3 秒（expat の compile は sub-make の中）で、jobserver は必須（p001 で実装済み）。

同じ image で: configure 8.15〜8.94 秒（host 10.66〜10.73 秒）、make（直列）11.28〜11.42 秒、`cc t.c -o t` 76〜84 ms（host 83〜85 ms）。

### 回帰（QEMU 8 GiB 4 vCPU NVMe、この Phase の全変更の後）

| 確認 | 結果 |
| --- | --- |
| boot test（`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-ws064p002/login.png`） |
| make の差分試験（`/root`） | 100/100 |
| sh の差分試験（guest、`guest-batches.sh` を NVMe の image に向けた複写） | 1389/1425（前回 1388。失敗は前回と同じものから 1 つ減った。`glob can expand to command and arg` は 9/24 から失敗） |
| SMP（`smpstress` 6 回）、COW、itimer、`lock-stress`、swaphog 700 MiB | 6/6、OK、OK、OK（10.7 秒）、bad=0 |
| vfork・posix_spawn（`vforktest.c`、guest で compile） | 子の書き込みが親に見える、子の exec、存在しない file の `ENOENT`、`posix_spawnp`、vfork 2000 回 |
| expat の `tests/runtests`（p001 で確認） | 4932/4932 |

実機は未実施。

## 結果（2026-09-26、cleared）

expat の `make -j4` は guest 4.94〜5.00 秒で host の GNU make（5.14〜5.18 秒）と同等以上。configure 8.2〜8.9 秒（host 10.7 秒）、直列の make 11.3〜11.4 秒（host 15.5 秒）、`cc` 76〜84 ms（host 83〜85 ms）。規約は ws064-p003。

