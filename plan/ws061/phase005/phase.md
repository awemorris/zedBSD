<!-- awesome-plan project=zedbsd record=ws061p005 -->

# ws061-p005: process の生成と終了の固定費用（CPU の配置・新しい page・HAL の lock と byte loop）

Phase ID: `ws061-p005`
Parent: [WS061](../ws.md)
Status: cleared
Queue: q438-i01（cleared）
Disposition: normal

## 目的

expat の configure（162 check）の guest の時間は kernel の system 時間が主で、`true` 1 回の fork・exec・exit・wait が 4 vCPU の QEMU で 3.4 ms（host 0.5 ms、1 vCPU の QEMU で 1.0 ms）かかる。host の `perf`（guest の cycles を vmunix の symbol に解決）と KVM の exit 統計で見つけた固定費用を潰す。

## 見つけた原因（q437 の計測、2026-09-25）

| # | 原因 | 証拠 |
| --- | --- | --- |
| 1 | **fork した子を round-robin で別の CPU に置く**（`choose_cpu`）。親は wait で寝て CPU は HLT、子は別 CPU で IPI で起こされ、終了でまた親を IPI で起こす。QEMU では HLT の出入りと IPI が 1 回 10〜50 µs、cache も冷える | 1 vCPU の guest で `true` 2000 回が 2 秒（1.0 ms/回）、4 vCPU で 3.4 ms/回。KVM の exit: 8 秒で HLT 5.6 万・APIC write 4.6 万（`true` 1 回に HLT 23 回・APIC write 19 回） |
| 2 | **物理 page の allocator が next-fit（rotor）で、解放した page を使い回さず未使用の page を前へ前へ触る。** 1 page ごとに QEMU の EPT violation（3.4 µs）と cache miss（4 KiB = 64 line） | KVM の EPT violation 8 秒で 27 万回（33k/s、`true` 1 回に約 90 page）、guest 物理の 4 KiB 連続の新しい page、guest の RIP は page の 0 埋め・buf_read の複写・page table の 0 埋め |
| 3 | HAL の `hal_memset`・`hal_memcpy` が 1 byte ずつのループ（page table の 0 埋めが cycles の 8%） | perf: `walk_leaf` の標本の 2/3 が `movb $0,(%r12,%rax,1); inc; cmp $0x1000` |
| 4 | `hal_get_memstat` を user page の確保のたびに呼ぶ（`vm_free_pages`）。pmem の lock・task registry の lock・struct の byte 埋め・extent の集計 | perf 1.3% + spin の一部 |
| 5 | `mutex_owned` が guard の spinlock を取る（assert で hot path から呼ばれる） | perf 2.4% |
| 6 | HAL の `space_op_enter`/`leave` が page の map/unmap ごとに global の registry lock を取って registry の list を線形に探す | perf 1.4% + 1.2% |
| 7 | `vmspace_destroy` が page ごとに `hal_space_unmap`（walk・shootdown・registry）を呼ぶ。HAL は destroy で table を全部解放するので不要 | perf の exit 経路 |
| 8 | `hal_space_unmap` が leaf table の無い範囲も 4 KiB ごとに walk する | code |
| 9 | `process_group_deliver_notifications` が exit・setpgid のたびに process 表を O(n²) で歩く（印が無くても） | code |

perf の leaf の上位（`true` 8000 回、guest の cycles 7.5 万標本、kernel 79%）: spin_trylock 12%（`xchg` の cache line の往復）、walk_leaf 8%（byte の 0 埋め）、syscall 入口 3%、vmspace_fault 3%、shootdown・send_icr・EOI・割り込み入口 9%、mutex_owned 2.4%、hal_get_memstat 1.3%。

## 変更

| # | 変更 | 場所 |
| --- | --- | --- |
| 1 | **新しい thread は作った CPU に置く**（その CPU に待ち行列が無いとき。あれば idle の CPU、無ければ round-robin）。**idle の CPU は halt の前に他の CPU の待ち行列から thread を盗む**（`steal_runnable`。migrating の印、`hal_task_transfer`）。忙しい CPU は tick で待ち行列があれば idle の CPU を 1 つ起こす。idle の mask（`scheduler_idle_mask`）は halt の前後と idle thread が CPU を離れるときに更新 | `src/kern/sched.c` |
| 2 | **HAL: task の switch が終わるまで元の task を running のままにする**（`switching_from` を per-CPU に記録し、切り替わった先（resume・新 task の 3 つの入口）が `amd64_task_finish_switch()` で `run_cpu = -1` にする）。盗む側は `hal_task_transfer` の BUSY で、まだ stack を離れていない task を避ける | `src/hal/amd64/task.c`、`task.h`、`percpu.h`、`dispatch.S` |
| 3 | **HAL: 解放した 1 page を LIFO の stack（最大 8192 page = 32 MiB）に置き、次の 1 page の確保で先に使う**。stack の間は extent の head bit を消して二重解放を検出。`hal_get_memstat` は stack の page を free に数える | `src/hal/amd64/page.c` |
| 4 | HAL: `hal_memset`・`hal_memset16`・`hal_memset32`・`hal_memcpy` を `rep stos`・`rep movs` に（`cld` 付き） | `src/hal/amd64/lib.c` |
| 5 | `vm_free_pages` は allocator の読みを snapshot にして 64 回まで減算で見積もる（予備 + 256 page に近づいたら読み直す）。worker は正確な読み。buffer cache の optional の admission も同じ見積もり（`vm_free_bytes_estimate`） | `src/kern/vm.c`、`include/kern/vm-reclaim.h`、`src/kern/cache.c` |
| 6 | `mutex_owned` は guard を取らず owner を acquire で読む | `src/kern/lock.c` |
| 7 | HAL: `space_op_enter`/`leave` は space ごとの atomic の count と `destroying` の flag（seq_cst）。destroy は count が 0 になるのを待つ | `src/hal/amd64/space.c` |
| 8 | `vmspace_destroy` の page は HAL の unmap を呼ばない（`free_vm_page(..., unmap)`。brk の縮小は unmap する） | `src/kern/vmspace.c` |
| 9 | HAL: `hal_space_unmap` は leaf table が無ければ次の 2 MiB 境界まで飛ぶ | `src/hal/amd64/space.c` |
| 10 | `PROCESS_PGRP_NOTIFY` の数を数え、0 なら `process_group_deliver_notifications` の表の走査を省く | `src/kern/process.c` |

## 受け入れ

- QEMU（8 GiB、4 vCPU、NVMe、4 GiB の root）で `true` のループが 1 vCPU の guest と同程度（約 1 ms/回）。
- expat の configure（`/root`、UFS）と `make -j4` が host（11.4 秒・6.0 秒）の 2 倍以内。届かなければ残りの原因を記録して次の Phase。
- 回帰: boot test（`boot-test.sh`、NVMe）、make の差分試験（8 GiB・512 MiB）、`SMP-STRESS.ELF`、COW、sh の差分試験、swaphog 450 MiB（512 MiB の guest）。

## 進行の記録（q438-i01、2026-09-25）

### 1 回目の走行（前のセッション）

- 最初の image は起動の途中で CPU1 が `hal_fatal`（新しい task の入口で割り込みが有効なまま `finish_switch` を呼んでいた）。直して boot test は PASS（NVMe、`build/boot-test-amd64-p005/login.png`）。
- 計測と回帰の guest 3 つは 19:43 に host の init の signal 15 で終わった（host の停止）。結果は残っていない。

### 2 回目の走行（このセッション）

- `true` 2000 回: 4.79〜4.90 秒（**2.4 ms/回**。変更前 3.43 ms、目標 約 1 ms）。
- expat の configure（`/root`）の 1 回目の途中で kernel panic: `src/kern/vm.c:4020: VM metadata lock ownership mismatch`（画面。QMP の RIP は CPU0 が `amd64_lapic_panic_all`、他は `hal_cpu_park`）。

**原因**: idle の CPU が thread を盗むようになり（変更 1）、thread が CPU を移ることが日常になった。そのとき、読んだ CPU と使う CPU が食い違う窓が表に出た:

| 窓 | 結果 | 修正 |
| --- | --- | --- |
| `hal_task_get_current` が `%gs:0`（per-CPU の構造体）を読み、次にその `running_task` を読む 2 段の load。間で移ると元の CPU の task（別の thread）を返す | `thread_current()` が別の thread を返し、`mutex_owned` が偽 → metadata lock の fatal | `running_task` を gs 相対の 1 命令で読む（`src/hal/amd64/task.c`） |
| `shootdown` が送り手の CPU を先に取り、preempt され得る | 移ると元の CPU を対象から外し（まだ space を載せている）、待ちの間に元の CPU の bit を新しい CPU で ack する（古い TLB） | shootdown 全体を割り込み禁止に（相互の要求は polling で処理する）（`src/hal/amd64/space.c`） |
| `kern_preempt_disable`・`enable`・count の読み、`sched_exit`、`switch_without_enqueue` が CPU を読んでから割り込みを禁じる | 元の CPU の count を増やす、`invalid scheduler exit` の fatal | 割り込みを禁じてから CPU を読む（`src/kern/sched.c`） |

- 上の修正の image では、`true` のループの途中で SSH が止まった（serial は応答）。ue0（USB CDC-ECM）の送受信の counter が止まり、ping も通らない。QMP で見ると 4 CPU とも `sched_idle` の hlt。gdbstub で scheduler の per-CPU 状態を読むと（型の offset は sched.c を scratch で `-g` で compile して得た）、CPU3 の sleep 行列に `network_worker`（tid 3）が **`need_migrate=3`（移動中＋wakeup 保留）のまま SLEEPING** で残っていた。移動中の印がある thread への wakeup は保留されるので、二度と起きない。

**原因**: steal は移動を終えるとき `sched.cpu` と `need_migrate` を移動先の lock だけで書き換える。wakeup は元の CPU の lock の下で `need_migrate |= SCHED_WAKE_PENDING` を行う。同時に走ると `|=` が `= 0` を上書きし、印が残る。既存の `sched_set_cpu` も同じ手順（i915 の試験からしか呼ばれないので表に出ていなかった）。

**修正**: `retarget_migrating()` で、元の CPU の lock の下で `sched.cpu` を新しい CPU に書き換えてから、新しい CPU の lock の下で `need_migrate` を消す。wakeup は `sched.cpu` が名指す CPU の lock を取り直すので、印の読み書きがどちらかの lock で必ず直列になる。steal と `sched_set_cpu` の両方に適用（`src/kern/sched.c`）。

規約: p005 の変更 file（sched.c・task.c・space.c・page.c・lib.c・vm.c・process.c・lock.c・vmspace.c・cache.c）の `style-check.py` の指摘は HEAD より増えていない（増えた 9 件と今回の分は直した）。

build: `make -j64 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-guest.mk BUILD=build/ws053-full-hal-guest ZEDBSD_VARIANT=native $(guest.py extra-files) disk-image`、warning 0（計測の image の設定。config.mk で作ると true・make の無い rootfs になる）。

### 3 回目以降（このセッション、IPI と HLT の原因）

修正後の計測は `true` 2.4 ms/回、configure（`/root`）30.7〜32.7 秒、make 28.5 秒で、目標に遠い。host の `perf kvm --guest -e cycles:G`（vmunix の `nm` から kallsyms を作って address を集計）と `perf kvm stat`（exit の理由）、gdbstub の breakpoint で `send_icr` の呼び出し元を frame pointer でたどって（300 回）原因を分けた。

| # | 原因（IPI の送り手の上位） | 変更 | 場所 |
| --- | --- | --- | --- |
| 11 | descriptor の表の変化ごとに `poll_notify()` が**全 system の poll を起こす**（fork の表の登録、close、exit の表の破棄）。子の表の変化は他の process の poll に関係しない | 表に `pollers`（その表を走査中の poll の数）を持たせ、0 なら通知しない。poll は sequence を読む前に数え（seq_cst）、変化の側は fence の後に読む | `include/kern/filedesc.h`、`src/kern/filedesc.c`、`src/kern/poll.c` |
| 12 | exit した子の vmspace を reaper が別 CPU で壊し、全体で 1 つの VM の metadata の mutex を、wait から戻った親の次の fork・fault と奪い合う（mutex の unlock の IPI が首位） | 親が wait で眠っているときは zombie に vmspace を残し、親の回収（`process_free_mem`、親の文脈）で壊す。それ以外は従来どおり reaper | `src/kern/process.c` |
| 13 | `readahead_cancel` が file を閉じるたびに取り消す job が無くても readahead の worker を全部起こす | 取り消した job を持つ worker だけ起こす | `src/kern/readahead.c` |
| 14 | idle の CPU が自分の tick で、親が wait に入る前の生まれたばかりの子を盗み、ループが CPU を渡り歩く（毎回 vCPU の起床） | run の行列に入った tick を記録し（`sched.queued_tick`）、同じ tick（1 ms）の間に入った thread は盗まない | `include/kern/sched.h`、`src/kern/sched.c` |
| 15 | `src/hal/amd64/defs.h` の `CLOCK_HZ 100` は使われていない古い値（tick は `include/hal/arch/amd64.h` の `HAL_TIMER_FREQUENCY` 1000 Hz）。誤読の元なので削除（2026-09-25 ユーザー指示「defs.h は適宜修正」） | 削除 | `src/hal/amd64/defs.h` |

途中の案（exit の後始末で vmspace を reaper を起こさずに列に入れ、wait が起きたら空にする）は、親が列に入る前に回収して戻ると取り残す窓があり、古い `SMP-STRESS.ELF` で漏れとして出た。zombie に残す形に置き換えた。

KVM の exit（`true` のループの間、1 回あたり）: APIC write 31 → 1.4、HLT 22 → 8、EPT violation 90 → ほぼ 0。

make は base の make が `-j` を無視する設計（`userland/base/make/main.c`）で、`make -j4` は直列。host の比較を `make -j1`（15.2 秒）に直した。host（Linux、同じ clang）: configure 11.2 秒、`make -j1` 15.2 秒、`make -j4` 5.2 秒。

| # | 退行（回帰で見つけた） | 変更 | 場所 |
| --- | --- | --- | --- |
| 16 | 512 MiB の guest で swaphog 450 MiB の後に USB の network（CDC-ECM）の送信が止まる（p005 の前の kernel を worktree で build し、同じ userland の image で比べて p005 が原因と確定）。stack の page は統計では空きだが extent では使用中のままで、連続・条件つきの確保（DMA の buffer）が memory の逼迫で失敗する | extent からの確保が失敗し stack に page があれば、stack を全部 extent に戻してやり直す（`page_stack_drain`・`pmem_alloc_or_drain`、`hal_pmem_alloc` と `hal_pmem_alloc_limited` の両方） | `src/hal/amd64/page.c` |

`/root` に置いていた `SMP-STRESS.ELF`（12:35 の build）は後始末を 5 秒待つ retry（485dbd53、12:36）の前の版で、reaper の非同期の後始末に当たると 7（資源が戻らない）を返す。stdout にも何も出さない。回帰は今の source（`userland/base/tests/smp-resource-stress.c`）を guest の libc に対して build した版で判定した。

## 結果（2026-09-25、cleared）

最終の image（QEMU、8 GiB、4 vCPU、NVMe、4 GiB の UFS root、`build/ws053-full-hal-guest`）。host は Linux、64 core、同じ clang。

| 測定 | p005 の前 | p005 の後 | host | 倍率 |
| --- | --- | --- | --- | --- |
| `true` 1 回（2000 回の sh のループ） | 3.43 ms | **1.1 ms**（1 vCPU の guest と同じ） | 0.5 ms | 受け入れの「約 1 ms」を達成 |
| expat の configure（`/root`、UFS） | 35 秒 | **17.2〜19.8 秒**（途中の計測では 23〜25 秒） | 11.2 秒 | 1.5〜1.8（2 倍以内） |
| expat の configure（`/tmp`、tmpfs） | 29 秒 | 12.8〜13.3 秒 | 11.2 秒 | 1.15〜1.2 |
| expat の make（`/root`） | 34 秒 | 20.4〜24.6 秒 | `-j1` 15.2 秒・`-j4` 5.2 秒 | `-j1` の 1.3〜1.6 |
| expat の make（`/tmp`） | — | 17.4〜18.8 秒 | 同上 | `-j1` の 1.15〜1.25 |

- 受け入れの「`make -j4` が host の 6.0 秒の 2 倍以内」は未達: base の make は `-j` を無視する設計で、guest の `make -j4` は直列（`sleep 2` の job 4 つに 8 秒）。直列どうしなら 2 倍以内。並列の make は [F-016](../../future-work.md)（ユーザーの判断）。
- configure の `/root` と `/tmp` の差（4〜7 秒）は UFS の write-through と flush（[F-015](../../future-work.md)、耐久性の方針はユーザーの判断）。
- 計測のばらつきが大きい（configure の `/root` は同じ image の走行でも 17〜25 秒）。上の値は最後の image で host が静かなときの 3 回。

回帰（最終の image）:

| 試験 | 結果 |
| --- | --- |
| boot test（`boot-test.sh`、`BOOT_MODE=uefi-nvme`） | PASS（`build/boot-test-amd64-p005c/login.png`） |
| make の差分試験 | 91/91（8 GiB）、91/91（512 MiB） |
| `smp-resource-stress`（source の版） | 6/6（8 GiB）。途中の image で 15/15 |
| COW（`cowtest`）、itimer | OK、`bad=0` |
| swaphog 450 MiB（512 MiB の guest） | 14.6 秒、`bad=0`。後も network・SSH が生きている |
| sh の差分試験 | 1390/1425（前回 1388。新しい FAIL は無い） |
| 規約（`style-check.py`） | 変更した file の指摘は HEAD より増えていない |

- QEMU だけ。実機は未実施。
- 試験の program（`swaphog`・`itimer-test`・`vmstatprobe` など）の source は repo に無く、古い image の `/root` から取り出して使った（scratch）。`plan/tools/` への登録は未実施。

