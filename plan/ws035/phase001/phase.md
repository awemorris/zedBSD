<!-- awesome-plan project=zedbsd record=ws035p001 -->

# ws035-p001: refactorの移動対応表と影響範囲、VFIOホストの確認

Phase ID: `ws035-p001`
Parent: [WS035](../ws.md)
Status: cleared（q316-i01、2026-09-23。結果は [results.md](results.md)）
Phase disposition: normal
Queue: q316（active）
実行: `phase-runner`（Opus 5.5、High）

## 目的

refactor（p002 → p003 → p023 → p004）を機械的・確実に進めるための対応表と影響範囲を作る。
あわせて、後のPhaseが使うVFIOホストの現状を読み取りだけで確かめる。**ソースは変更しない。**

## 範囲

1. **移動の対応表**（ファイル単位、`plan/ws035/refactor-map.md`）
   - p002: `include/drivers/*` の各ファイル → `src/drivers/` と同じ階層の新しいパス。
   - p003: `libc/` のsource → `src/libc/`。
   - p023: `libc/include/*` → `include/`。既存の `include/` の名前との衝突の有無。
   - p004: `include/boot/*` と `include/kern/rpi4/boot.h`、`include/kern/sun4u/boot.h` → `include/kern/boot/`。
     既存の `include/kern/boot.h` の扱いの案。
2. **参照の一覧**（再生成できるscriptで作る。`plan/ws035/tests/refactor-refs.sh` など）
   - 各移動対象を参照する `#include`、Makefile・`*.mk`、sysroot生成、Noctの宣言生成、`plan/*/tests/` の試験、
     文書のパスを、移動対象ごとに件数とファイルで出す。
   - **HAL配下（`src/hal/`、`include/hal/`）で変わる行を別に一覧にする。** HALの変更には事前承認が要るため、
     refactorの前にユーザーへ「機械的なinclude置換だけ」の承認を求める材料にする。
3. **kernelのinclude経路の保証案**（p023の設計）
   - `libc/include/*` を `include/` へ移したとき、kernel・HALのbuildがlibcのヘッダ（`stdio.h`、`sys/` 等）を
     誤って拾わないようにする方法の案と、それを検査する方法（例: kernel buildの依存ファイル一覧の検査）。
4. **refactor前のbuildの基準**
   - 移動前の状態で、各platformの対象build（amd64、pcat i386、pc98、arm64 rpi4、sparcv9、x68k のうち、現状で
     buildできるもの）を実行し、結果（成功・失敗、warning数）を記録する。refactor後の回帰と区別するため。
   - i915を含むamd64 build（`CONFIG_DRIVER_PCI_I915`）も1回。
5. **VFIOホストの確認**（Latitude 5330、読み取りだけ）
   - `~/bigbang/igpu-mode.sh show`、`lspci -nnk -s 00:02.0`、PCHのHDA（`00:1f.3` 付近）のdriverとIOMMU group、
     そのgroupに含まれる他のdevice。結果を `plan/ws035/phase001/host-facts.json` に残す。
   - ホストの状態は変えない（driverの付替え、GDM、reboot等はしない）。

## 範囲外

ソースの移動・編集、HALの変更、ホストの状態変更、GitHub公開。

## 受け入れ

- `refactor-map.md` に4つの移動の対応表がそろい、参照一覧が再生成できるscriptから出ている。
- HAL配下で変わる行の一覧がある（件数とファイル）。
- kernelのinclude経路の保証案と検査方法が書かれている。
- refactor前のbuild結果が記録されている（失敗したplatformは、失敗したまま記録する）。
- `host-facts.json` がある。HDAが単独でパススルーできるかの判断材料（group構成）がそろっている。
- ソースに差分が無い（`git status` で `plan/` 以外に変更が無い）。

## 時間と上限

見積 180分。build 1回あたり1800秒、ssh 1回あたり60秒。同条件の変更なしretryは3回まで。
aggregate `make check` は使わない。
