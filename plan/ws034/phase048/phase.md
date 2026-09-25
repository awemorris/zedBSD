<!-- awesome-plan project=zedbsd record=ws034p048 -->

# ws034-p048: `FD_SETSIZE` を 1024 へ

Phase ID: `ws034-p048`
Parent: [WS034](../ws.md)
Status: **cleared**（q351-i01、2026-09-24）
Phase disposition: normal
Queue: q351（q351-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー決定「FD_SETSIZE はほかの POSIX と同様の数値にしましょう」。`fd_set` は 32 bit の 1 語で、
`FD_SET(40, ...)` は 32 bit を超える shift（未定義動作）だった。kernel の `pselect` も 1 語しか見ず、
`nfds` が 33〜`KERN_OPEN_MAX` のとき同じ shift をしていた。

## 変更

- `include/uapi/select.h`: `KERN_FD_SETSIZE` 1024、`KERN_NFDBITS` 32、`fd_set` は `uint32_t fds_bits[32]`（128 byte、
  両 ABI で同じ）。member 名を BSD と同じ `fds_bits` にした。
- `include/libc/sys/select.h`: `FD_SETSIZE`、`NFDBITS`、`fd_mask`、語の添字を取る `FD_SET`・`FD_CLR`・`FD_ISSET`、
  全語を消す `FD_ZERO`（文として使う形。glibc と同じ）。
- `src/kern/syscall.c` の `sys_pselect_call`:
  - `nfds` の上限を `KERN_FD_SETSIZE` にした（POSIX の EINVAL の条件）。
  - **`nfds` までの語（`(nfds+31)/32` 語）だけを pin・読み書きする**（他の系と同じ）。1 語の `fd_set` で build された
    既存の program は 4 byte の set を渡すが、その先を壊さない。
  - `KERN_OPEN_MAX`（32）以上の descriptor に bit が立っていれば EBADF。
  - 出力は各 descriptor の `events` から決めるので、入力と出力で set を 6 つ持たず 3 つを使い回す（kernel stack は
    128 byte × 3 と `pollfd[32]`）。

## 検証

`plan/ws034/tests/select-test.c`（toolchain の `zedbsd-clang` で build、SSH ハーネスのゲストで実行）: **10/10**。

| 項目 | 結果 |
| --- | --- |
| `FD_SETSIZE` = 1024、`sizeof(fd_set)` = 128 | ok |
| macro が最後の語（fd 1023）まで届く、`FD_CLR` | ok |
| 空の pipe を `nfds` = 1024 で: 読めない・書ける | ok（1） |
| data のある pipe を `nfds` = 100 で | ok（2） |
| 開いていない fd 700 の bit | EBADF |
| `nfds` = 1025 | EINVAL |
| 4 byte の set（旧 ABI）を渡すと、その後ろの語を書き換えない | ok（`0xdeadbeef` のまま） |
| `nfds` = 32 で 1 語 | ok |

CI の kernel（amd64・pcat・pc98）は build できる。compile の warning は 0。pcat の log の 1 行目に clang driver の
`-no-pie` 未使用の warning が 1 件出た（compile ではない。直前の p048 の同じ build では出ていなかった。今回の変更の file とは無関係）。
既定の i386 の rootfs（libc の header の変更）も build できる。SSH（sshd）で入って試験したので、既存の program も動く。

## 残り

- **1 process が開ける descriptor が 32**（`KERN_OPEN_MAX`）。`FD_SETSIZE` を上げても 32 以上は開けない。
  Chromium・GTK・clang の並列 build などは足りない → 新しい Phase ws034-p051 にした。
