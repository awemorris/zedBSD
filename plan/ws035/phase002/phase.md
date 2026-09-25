<!-- awesome-plan project=zedbsd record=ws035p002 -->

# ws035-p002: `include/drivers/` を `src/drivers/` の階層に揃える

Phase ID: `ws035-p002`
Parent: [WS035](../ws.md)
Status: cleared（q317-i01、2026-09-23。結果は [results.md](results.md)）
Phase disposition: normal
Queue: q317（q317-i01）
実行: `phase-runner`（Opus 5.5、High）

## 目的

フラットな `include/drivers/` を、`src/drivers/` と同じ階層に移す。中身は変えず、移動とinclude・build参照の置換に留める。

## 入力

- 移動対応表: [refactor-map.md](../refactor-map.md) の p002 の節、`../refactor-refs/moves.tsv`・`refs.tsv`。
- 参照一覧の再生成: `plan/ws035/tests/refactor-refs.py`。
- ws035-p001の結果: [phase001/results.md](../phase001/results.md)。

## 決定事項（ユーザー、2026-09-23）

- i915とVenusの公開ヘッダは、PCIドライバの規則どおり `include/drivers/pci/pci-i915.h`・`include/drivers/pci/pci-venus.h` とする。
  `include/drivers/gpu/i915.h` は作らない。
- 登録関数を改名する: `drv_i915_pci_driver_register` → `drv_pci_i915_driver_register`、
  `drv_venus_pci_driver_register` → `drv_pci_venus_driver_register`。呼出し側（platformの登録、試験）もすべて直す。
- xHCIのヘッダはsourceの場所（`src/drivers/pci/`）に合わせる。`hid`・`graphics` のヘッダは実装の隣へ置く。
- buildされない `src/drivers/gpu/i915-old/` は書き換えない。
- HALは `#include` のパス変更だけ承認済み（p002ではHALの変更は0行の見込み）。
- amd64以外のbuildは壊れてもよい。

## 手順

1. p001の対応表と、現在のtreeを照合する（p001後に増えた参照が無いか `refactor-refs.py` で再確認）。
2. `git mv` でヘッダを移す（履歴を保つ）。
3. `#include`、Makefile・`*.mk`、生成script、`plan/*/tests/` の試験のパスを置換する。計画の記録（過去の結果文書）は書き換えない。
4. 登録関数を改名する。
5. 確認する。

## 受け入れ

- `include/drivers/` の直下にフラットなヘッダが残っていない（対応表どおりの階層）。
- 旧パスへの参照が、計画の記録と `i915-old/` を除いて0件（`refactor-refs.py` で確認）。
- amd64とi915-amd64（`CONFIG_DRIVER_PCI_I915`）のkernelとuserlandのbuildが通り、warning 0。
- `plan/ws035/tests/kernel-include-audit.py --compare` で、p001の基準からlibcヘッダの読込みが増えていない。
- i915・GPU coreのhost試験（`plan/ws031/tests/run-vk-host-tests.sh`、`src/drivers/gpu/i915/tests/contracts/run.sh` 等、
  既存のもの）がPASS。
- `git diff --check` がPASS。

## 範囲外

libcの移動（p003・p023）、kernelからのlibcの切り離し（p034・p035）、bootヘッダの移動（p004）、amd64以外のbuildの修復、
QEMU・実機の試験。
