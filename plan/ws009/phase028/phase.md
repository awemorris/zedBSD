<!-- awesome-plan project=zedbsd record=ws009-p028 -->

# ws009-p028: clean な tree からの build と amd64 QEMU の boot の再現、互換の主張の照合（案 D）

Status: in-progress（2026-10-30 P1。DOC-T60 済み、build と boot の再現は未着手）
Parent: [WS009](../ws.md)
Approval: 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」。2026-10-30 Q1: 案 D は P1 が実行、QEMU は P1 専用の名前・port・directory で流してよい。
実行の担当: Opus（P1）
依存: p026（build-from-source.md の Noct と variant の記述を直した後の文書で再現する）。DOC-T60 の部分は依存なし。

## 1. 目的

WS の完了の条件「build and boot instructions are reproduced in a clean environment」「compatibility claims agree with WS001」を、
今の main で確かめ直す。最後の再現は p002（2026-08-25）・q190 で、その後 amd64 の既定の profile が `native` になり、Noct が 2.0.3 になった。

## 2. 範囲と方法

- **clean な tree**: 確かめる commit を `git archive <SHA> | tar -x -C <worktree>/build/tmp/p028-src/` で展開した、build の状態を持たない tree。
  共有の toolchain（`build/llvm`・`build/NoctLang`・`build/sources`・`build/distfiles` など、文書が「取得する」と言う物）は、その tree の `build/` から
  main の checkout の物への読み取り専用の symlink で使う（LLVM・Noct を build・install しない。AGENTS.md §7）。symlink した物と、tree の中で
  新たに作られた物を結果に分けて書く。
- **手順は文書の通り**: `docs/howto/build-from-source.md` の command を順に（`make help`・`make list-targets`・既定の amd64 の image の build）。
  文書の command で足りない物・違う物があれば、その時点で記録し、文書の誤りとして直す（p028 の範囲の中、build-from-source.md だけ）。
- **boot**: `plan/tools/boot-test.sh`（画面を撮って login prompt を読む。console・serial の log では判定しない）。
  `native`（既定、`BOOT_MODE=uefi-nvme`）で login prompt。時間が許せば `hybrid`（OVMF と `bios-ide` の両方）・`uefi`（OVMF）・`bios`（`bios-ide`）も
  同じ tree で `ZEDBSD_VARIANT=` を変えて image だけを作り直して確かめる（DOC-T72）。i386 PC/AT・PC-98 は範囲の外（過去の WS010 の証拠のまま、結果にそう書く）。
- QEMU の名前・directory: `OUTPUT=build/tmp/p028-boot-<variant>`、`BOOT_TEST_WORK=build/tmp/p028-work-<variant>`。各 VM に `BOOT_TIMEOUT`、各 command に `timeout`。
- 同じ条件で変更なしの再試行は 3 回まで。
- **DOC-T60**: `docs/reference/compatibility-profile.md` の Feature selection の表の各行を `include/libc/features.h` の定義と照らす。
  `uapi_ptr_t` の選び方を `include/uapi/types.h` と照らす。[WS001](../../ws001/ws.md) の今の台帳（§12 など）と、文書が主張する
  「conformance work remains active」「完全な準拠を主張しない」が食い違わないか（文書が WS001 の未完の物を完了と言っていないか）を読む。

## 3. 作る物

- 1 本の script `plan/ws009/tests/p028-reproduce.sh`（展開 → symlink → make → boot-test → 要約）。出力は `build/tmp/p028-*` の新しい directory。
  削除をしない（古い出力は残し、新しい directory に書く）。
- 必要なら `docs/howto/build-from-source.md`・`docs/reference/compatibility-profile.md` の直し。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。PNG の path を結果に書く（Q1 がユーザーに見せる）。

## 4. 変えてはいけない物

- source・Makefile・tools/・toolchain・HAL。共有の `build/` に書かない（symlink は読むだけ）。

## 5. 終わりの条件

`native` の image が clean な tree の文書の手順だけで作れ、boot-test.sh が login prompt を読んだ（PNG がある）。他の variant は確かめた物と
未実施の物を分けて書く。DOC-T60 の照合の表（行・header の値・一致）。commit は
`git commit -m WIP -- plan/ws009/tests/p028-reproduce.sh plan/ws009/phase028/phase.md plan/ws009/ws.md`（と直した docs）。

## 結果

### DOC-T60（2026-10-30 P1、host の `date` 11:1x）

`docs/reference/compatibility-profile.md` を `include/libc/features.h`・`include/uapi/types.h` と [WS001](../../ws001/ws.md) の台帳と照らした。

| 文書の主張 | header・台帳 | 一致 |
| --- | --- | --- |
| `_POSIX_VERSION` 202405L | features.h 42 行 `202405L` | 一致 |
| `_POSIX2_VERSION` 200809L | features.h 43 行 `200809L`、WS001 §4「The advertised values remain `_POSIX2_VERSION=200809L`」 | 一致 |
| `_XOPEN_VERSION` 700 | features.h 44 行 `700`、WS001 §4 同じ | 一致 |
| `__KERN_POSIX_2024_VISIBLE` の条件 | features.h 17〜24 行（`_POSIX_C_SOURCE >= 202405L` か `_XOPEN_SOURCE >= 800`） | 一致 |
| THREADS・TIMERS・MONOTONIC_CLOCK 200809L、DEVICE_CONTROL 202405L | features.h 47・60・61・54 行 | 一致 |
| THREAD_SAFE_FUNCTIONS・PRIO_INHERIT・PRIO_PROTECT・ASYNCHRONOUS_IO・PRIORITIZED_IO・TYPED_MEMORY_OBJECTS は -1 | features.h 55〜59・62 行 | 一致 |
| `__KERN_LEGACY_VISIBLE` は既定と古い selector で見える | features.h 29〜37 行 | 一致 |
| 「完全な SUS Issue 8 の主張ではない」「conformance work remains active」 | WS001 は incomplete・保留、P1 の既知の非互換 75 ほか | 一致（文書は完了を主張しない） |
| `uapi_ptr_t` は **`ZEDBSD_USER_ABI_LP64`** の時 64 bit | types.h は `KERN_USER_ABI_LP64`（build が言わなければ `__LP64__` から定義）。`ZEDBSD_USER_ABI_LP64` は source の中に無い（plan の試験と Noct の patch の compile の旗だけ） | **不一致 → 直した**（`KERN_USER_ABI_LP64` と `__LP64__` からの既定を書いた） |
| `libGL.so` は OpenGL だけ、GLX は `libGLX.so` | userland/desktop/libGL/exports.map に `glX` 0 件、userland/desktop/xserver/libGLX がある | 一致 |
| `ttyname_r` の例 | userland/base/libc/posix.c に `ttyname_r` | 一致（振る舞いは実行で確かめていない） |

観察（直していない）: features.h の comment に「XXX: Rename __KERN_* to __LIBC_*」。`ZEDBSD_USER_ABI_LP64` は Noct の patch（userland/base/noct/patches/0002-use-project-llvm-sysroot.patch）が渡す旗で、types.h は見ない（toolchain の範囲、触らない）。

### build と boot の再現

（未着手。toolchain の扱いを Q1 に確かめてから）
