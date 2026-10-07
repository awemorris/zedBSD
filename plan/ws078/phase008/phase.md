<!-- awesome-plan project=zedbsd record=ws078-p008 -->
# ws078-p008: make の変数と protocol の header の file 名の旧名（[guide.md](../guide.md) §2.3 の B2・B3）

Status: in-progress（2026-10-08 P1 q879: 改名と build warning 0 まで。boot test・WS099 の C9 は T1）
Disposition: normal
Parent: [WS078](../ws.md)
Queue: q879（P1、2026-10-08 Q1 の承認の 2 番）

## 範囲

- B2: make の変数の `ZDESKTOP`。
- B3: protocol の header の file 名 `zed-*-v1-client-protocol.h`。

意味は変えない（改名だけ）。

## 実装（2026-10-08 P1）

- `platform/amd64/vmunix.mk`（28 行）。新しい名は、既存の変数に同じ名が無いことを `git grep -w` で確かめた。
  - `DYNAMIC_ZDESKTOP_OBJS` → `DYNAMIC_LIBKEILAND_OBJS`
  - `DYNAMIC_ZDESKTOP_PROGRAM_OBJS` → `DYNAMIC_WAYLAND_PROGRAM_OBJS`
  - `DYNAMIC_ZDESKTOP_{TERMINAL,FILES,SETTINGS,NOTES,MONITOR,BROWSER,X11SERVER}_OBJS` → `DYNAMIC_{…}_OBJS`
- `userland/desktop/libkeiland/Makefile`: `LIBZDESKTOP_IMAGE_DATA` → `LIBKEILAND_IMAGE_DATA`（3 行）。`LIBZDESKTOP_SOURCES` は WS104 で既に `LIBKEILAND_SOURCES`。
- B3:
  - `git mv` で `userland/desktop/libwayland/zed-{edit,glass,gpu-buffer,ime-status,keyboard-inset,theme,titlebar}-v1-client-protocol.h` → `keiland-…`（7 file。guide の 6 に theme が足されていた）。
  - include の 18 行（libwayland・libkeiland・libvulkan の wsi-wayland.c・ime/program.h・userland/tests の 3 つ）と `API-PROVENANCE.md` の 1 行を新しい名に。
  - ime-status の header の guard `ZED_IME_STATUS_V1_…` を `KEILAND_IME_STATUS_V1_…` に、gpu-buffer の header の comment の「zed protocol」を「Keiland protocol」に。
  - 中の protocol の名（`keiland_*`）は p006 で済んでいて、変えていない。
- plan の古い記録（終わった Phase の証拠、ws035 の refactor の表など）にある旧名は履歴なので残す。
- 確かめ: `git grep -n -E 'ZDESKTOP|zed-[a-z-]+-v1' -- ':!plan'` が 0 行。

## 確認

- build（warning 0、target を名指し）: `make -j16 BUILD=build/bug221 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk` で次を名指し（rc 0）。
  - dynamic/libwayland-client.so・libkeiland.so・libvulkan.so
  - bin/wayland・keiland-ime・acquire-fence-test・gpu-forge-test・titlebar-probe・terminal・files・settings・notes・monitor・browser・xserver
- `make -f userland/desktop/keiland-linux.mk … all`（rc 0）。
- T1（未実施）: boot test と WS099 の C9（guide の p008 の受け入れ）。改名だけなので、image が build でき起動して desktop が出れば十分という案。

## 経緯の記録

確かめの最初の build で target を名指さずに make を流し、既定の goal（disk-image）が worktree の build/packages で clang・libcxx の package の build を始めた（AGENTS の toolchain の規則に反する）。約 10 分で止めた。共有の toolchain は書き込み不可のまま変わっていない。途中の出力は Q1 が消した。以後は target を名指す（Q1 が protocol に記録）。
