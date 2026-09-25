<!-- awesome-plan project=zedbsd record=ws034p053 -->

# ws034-p053: libc の `mkostemp`・`posix_fallocate`（と、pipe・socket の `fstat`）

Phase ID: `ws034-p053`
Parent: [WS034](../ws.md)
Status: **cleared**（q363-i01、2026-09-24）
Phase disposition: normal
Queue: q363（q363-i01）
実行: メインセッション

## 経緯

p050 の調査で、upstream の wayland-cursor の代替の経路などが使う `mkostemp`・`posix_fallocate` が libc に無いと分かった。

## 変更

- `src/libc/tempnam.c`: `mkostemps(path, suffix, flags)`・`mkostemp(path, flags)`（flags は `O_APPEND`・`O_CLOEXEC`・`O_SYNC` だけを受ける）。
  `mkstemp` は `mkostemps(path, 0, 0)` になった。`include/libc/stdlib.h` に宣言。
- `src/libc/fallocate.c`（新規）: `posix_fallocate()`。kernel に fallocate が無いので、`ftruncate` で伸ばしてから、伸ばした範囲の
  block ごとに 0 の 1 byte を書いて領域を確保する（glibc が kernel の支えの無いときに使う方法と同じ）。範囲が file の中なら何もしない。
  error は戻り値で返し errno を変えない（POSIX）。EINVAL（負の offset、0 以下の長さ）、EFBIG（桁あふれ。off_t の幅は ABI による）、
  ESPIPE（pipe・socket）、ENODEV（通常の file でない）。`include/libc/fcntl.h` に宣言、`src/libc/libc.mk` の 2 つの一覧に追加。
- **kernel: pipe と socket の `fstat`**（`src/kern/syscall.c`）。試験で分かった: inode を持たない file（無名の pipe、socket）の `fstat` が
  **EINVAL で失敗していた**（POSIX は `S_IFIFO`・`S_IFSOCK` として答えることを求める）。`posix_fallocate` が pipe を見分けられなかった。
  種類、link 1、呼び手の実効 uid・gid、file ごとに違う番号を返すようにした。大きさと時刻は 0（POSIX は未規定）。kernel の handle は従来どおり EINVAL。

## 検証

`plan/ws034/tests/fallocate-test.c`（SSH ハーネスのゲスト、amd64）: **16/16**。`mkostemp` の新しい名前と `FD_CLOEXEC`、`mkostemps` の suffix、
短い template と許さない flag の EINVAL、100000 byte の確保（大きさと block の数）、範囲が中なら大きさを変えない、端の先は offset＋len まで伸ばす、
EINVAL・EBADF・ESPIPE、`fstat` の pipe が `S_IFIFO`・socket が `S_IFSOCK`。

CI の kernel（amd64・pcat・pc98）warning 0、i386 の既定の rootfs（32 bit の `off_t` の libc）も build できる。
既定の BUILD（`build/amd64/dynamic/libc.so`、toolchain の wrapper が link に使う）も作り直した。
