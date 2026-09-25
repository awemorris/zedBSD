<!-- awesome-plan project=zedbsd record=ws061p010 -->

# ws061-p010: system call の入口を `syscall`/`sysret` に、libc の lock の adaptive spin

Phase ID: `ws061-p010`
Parent: [WS061](../ws.md)
Status: cleared
Queue: q446-i01（cleared）
Disposition: normal

## 目的

2026-09-26 ユーザー「system call の入口を int 0xc2 から syscall/sysret に替えるのは、優先でお願いします。libc の lock の adaptive spinも優先でお願いします。規約適合は最後でいいです。」

configure の profile で `amd64_syscall_entry`（`int 0xc2` の門を通る費用、先頭の命令に集中）が 4.6%。

## 設計と変更

| # | 内容 | 場所 |
| --- | --- | --- |
| 1 | `syscall` の入口の stub: per-CPU の退避場所に user の RSP、per-CPU の `syscall_rsp`（TSS の `rsp0` と同じ、task の切り替えで更新）に切り替え、割り込みの門と同じ形の frame（SS、RSP、RFLAGS=r11、CS、RIP=rcx、error 0、印 `INT_SYSCALL_FAST` 0xc3）を積んで共通の経路へ。FMASK が IF・DF・TF・AC・NT を消すので、frame を積むまで割り込みは止まっている | `src/hal/amd64/trap.S` |
| 2 | 戻り: frame の印が 0xc3 のとき `sysretq`（rcx・r11 に frame の RIP・RFLAGS、割り込みを止めてから user の stack）。`int_handler` の最後で、user の code・data の segment で canonical な RIP のときだけ印を残し、それ以外は 0xc2 にして `iretq`（非 canonical な RIP への `sysret` は ring 0 で fault する） | `trap.S`、`int.c` |
| 3 | 第 2 引数は `r10`（`syscall` は rcx を戻り先に使う）。kernel は印が 0xc3 のとき `r10` から読む。`int 0xc2` は互換で残す | `int.c` |
| 4 | GDT の user の data と code を入れ替え（`sysret` は STAR[63:48]+8 を SS、+16 を CS にする）: `SEG_USER_DATA` 0x18、`SEG_USER_CODE` 0x20。STAR・LSTAR・FMASK・EFER.SCE を各 CPU で設定（`amd64_syscall_init`） | `defs.h`、`descriptor.c`、`cmain.c`、`smp.c` |
| 5 | NMI を IST2 の専用 stack に（`syscall` の直後、stack を切り替える前の NMI が user の stack に積まれないように） | `descriptor.c`、`int.c` |
| 6 | register を丸ごと設定する経路（`frame->rcx`・`r11` も設定する）は印を 0xc2 にして `iretq`。signal からの復帰は保存した frame の印をそのまま使う（0xc3 の文脈の rcx・r11 は入口の時の戻り先と flags そのもの） | `task.c` |
| 7 | libc・crt0・rtld の stub を `syscall` 命令に（第 2 引数を `r10`）。sigreturn の stub は `int 0xc2` のまま | `userland/base/libc/syscall-amd64.S`、`src/libc/crt/crt0-amd64.S`、`src/rtld/entry-amd64.S` |
| 8 | adaptive spin: lock が取れないとき、眠る前に `pause` を挟んで最大 N 回 word を見て、空いたら取る。既定 N=100（glibc の adaptive mutex と同じ）、環境変数 `LIBC_LOCK_SPIN`（起動時に libc が自分で読む、0 で無効）。`word_lock`、`pthread_mutex_lock`、rwlock と barrier の guard | `userland/base/libc/pthread.c`、`posix.c` |

HAL の API（`hal.h`）は変えない。

## 受け入れ

起動、system call が `syscall` で入り `sysret` で戻る（計測で確かめる）、signal・fork・exec・ptrace を含む回帰（make・sh の差分試験、SMP、COW、itimer、boot）。configure・make・`cc` の時間。`lock-stress` の時間を spin 0 と既定で比べる。

## 進行の記録（q446-i01、2026-09-26）

### 確認（QEMU 8 GiB 4 vCPU NVMe）

| 確認 | 結果 |
| --- | --- |
| 入口 | gdb の数える breakpoint（3 秒、`true` のループ）: `amd64_syscall_fast_entry` 56 回、`amd64_syscall_entry`（`int 0xc2`）0 回 |
| 入口の費用（configure の host の perf） | `amd64_syscall_entry` 4.65% → `amd64_syscall_fast_entry` 1.54%、`amd64_sysret` 0.01% |
| configure（`/root`、host と交互に 3 回） | guest 9.78〜10.19 秒、host 10.95〜11.0 秒（直前の 10.65〜11.23 秒は host の load average 16、LLVM の build の直後で乱れていた） |
| make（直列） | 12.6〜12.77 秒（host `-j1` 15.2 秒） |
| `cc t.c -o t` | 87 ms（前 99 ms、host 84 ms） |
| `lock-stress`（spin 0 と既定 100） | 0: 14.3・9.6 秒、100: 13.5・9.8 秒。差はばらつきの内。既定は glibc と同じ 100 のまま（`LIBC_LOCK_SPIN` で変えられる） |
| lldb（break、`register write rcx`・`r11`、続行） | 書いた値が読め、process は状態 7 で正常に終わる（`iretq` を強制する経路） |
| 回帰 | boot test PASS（`build/boot-test-amd64-p010/login.png`）、make の差分試験 91/91（8 GiB・512 MiB）、SMP 6/6（両方）、`lock-stress` OK、COW、itimer、swaphog、`dir-grow.sh`、sh の差分試験 1389/1425（順序に依存する 1 件のみ） |

## 結果（2026-09-26、cleared）

system call は `syscall` で入り `sysret` で戻る。入口の費用は 1/3。adaptive spin は入れたが、今の負荷では効果が計測のばらつきに埋もれる（並列の make で改めて測る）。QEMU だけ、実機は未実施。
