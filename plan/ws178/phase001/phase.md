<!-- awesome-plan project=zedbsd record=ws178-p001 -->

# ws178-p001: libGL を OpenGL（Desktop）と GLX（xserver）に分ける

Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-340 で p005 PASS。x11-p004 の段 3 の FAIL は WS178 の前から（T1-341）で BUG-273（tracking））（旧: in-progress（q836、P1。2026-10-07 実装済み、build と host の確認済み、T1 待ち））
Disposition: normal
Parent: [WS178](../ws.md)

## 範囲

ws.md の「目標」のとおり。libGL.so の soname は変えない。export は 2026-10-07 ユーザーの決定「完全に分ける」で変わる（glX* は libGLX.so へ移り、libGL.so から消える。当初の「export は変えない」を置き換え）。

## 確認

- build（warning 0）、`make -s list-user-programs` で OpenGL が desktop、GLX が xserver の一部、`plan/tools/menuconfig-target-host-test.py`。
- T1: zgears・glxtest（X11 の image）の回帰。

## 実装（2026-10-07 夕、P1）

- tree: `userland/x11/libGL/` → `userland/desktop/libGL/`（`git mv`）。package `libgl` の label「OpenGL」、menu `desktop`、path `desktop/libGL`。sources から `glx.c` と private の `libX11/xlib.c` を外し、`context.c`（新）を足した。
- GLX: `glx.c` → `userland/desktop/xserver/libGLX/glx.c`。xserver の package（`userland/desktop/xserver/Makefile`）が `KEILAND_LIBGLX_SOURCES`（glx.c と private の xlib.c）と image の data `/lib/libGLX.so` を持ち、REQUIRE に `desktop/libGL` を足した（xserver を選ぶと libGLX.so と libGL・libEGL が入る）。menu に別の項目は作らない。
- GL と GLX の間の相互の呼び出し（glx.c → `fixed_install`、fixed.c・immediate.c → `glx_version`・`glx_profile`）は zedBSD の私的な口 `userland/desktop/libGL/zgl-glx.h` に: libGL.so が `zgl_glx_attach(const struct zgl_glx_query *)` を export し（fixed の hook も入れる）、libGLX の `glXCreateContext` 系が呼ぶ。libGL の `gl_context_version`・`gl_context_profile`（`context.c`）は attach が無ければ 1.4・flags 0・profile 0（以前の「context が無い時」と同じ）。glx.c の `glx_version`・`glx_profile` は static に。
- exports: `libGL/exports.map` は gl* 384 と `zgl_glx_attach`、`xserver/libGLX/exports.map`（新）は glX* 28 と `zglx_swap_step`。
- `platform/amd64/vmunix.mk`: libGL.so の規則の path、`libGLX.so`（`-l:libGL.so -l:libEGL.so -l:libc.so`、`check-dynamic-elf`）、zgears・glxtest は `-l:libGL.so -l:libGLX.so`。zgears・glxtest の REQUIRE は `desktop/libGL desktop/xserver`。
- libepoxy の patch 0001 の `GLX_LIB` を `libGLX.so` に（epoxy は `-Dglx=no` で build しているので今は効かない、名だけ正しく）。
- 文書: `include/libc/GL/glx.h` の注、`docs/reference/compatibility-profile.md` の「OpenGL and GLX」、`userland/desktop/xserver/libGLX/README.md`（外から移植する X の GL の program は `-lGLX` を足す）。`shaders/regenerate.py` の path。

### 確認

| コマンド | 結果 |
| --- | --- |
| `make -j16 BUILD=build/p1-ws178 ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p1-ws178/dynamic/libGL.so …/libGLX.so build/p1-ws178/bin/zgears …/glxtest` | 成功、warning 0（`check-dynamic-elf` を含む） |
| `llvm-nm -D --defined-only` | libGL.so: glX 0、gl* 384、`zgl_glx_attach`。libGLX.so: glX* 28 と `zglx_swap_step` だけ（xlib は local） |
| `llvm-readelf -d` | libGLX.so NEEDED libGL・libEGL・libc。zgears・glxtest NEEDED libGL・libGLX・libc。両 program の未定義の glX* は全部 libGLX.so にある |
| `make -s list-user-programs` | `libgl|OpenGL|…|desktop|desktop/libGL|desktop/libegl`、xserver の REQUIRE に desktop/libGL、zgears・glxtest は `desktop/libGL desktop/xserver` |
| `plan/tools/menuconfig-target-host-test.py` | **FAIL（WS178 と無関係、既存）**: `fidoctl is offered only on amd64`（その後 `libavcodec` も同じ）。base・packages の platform の縛りの検査が amd64 だけの package に古い。この検査だけを外した写し（scratchpad）では `MAC-T001 menuconfig round-trip: PASS`（requirements の解決を含む） |

未実施: QEMU（T1: zgears・glxtest の回帰）、実機。
