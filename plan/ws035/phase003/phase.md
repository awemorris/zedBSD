<!-- awesome-plan project=zedbsd record=ws035p003 -->

# ws035-p003: `libc/` のsourceを `src/libc/` へ移す

Phase ID: `ws035-p003`
Parent: [WS035](../ws.md)
Status: cleared（q319-i01、2026-09-23）
Phase disposition: normal
Queue: q319（q319-i01）
実行: メインセッション（サブエージェントは使わない。Master「実行体制とQueue運用方針」）

## 範囲

1. **libcのsource**: `libc/*.c`・`*.h`（50ファイル）と `libc/regex/` を `src/libc/` へ移す。
   `libc/libc.mk` も `src/libc/libc.mk` へ移す。`libc/include/` は動かさない（p023の担当）。
2. **crt**（2026-09-23ユーザー指示で追加）: `src/crt/` の9ファイルを `src/libc/crt/` へ移す。
   crt0はstatic link、crt1はdynamic linkの起動コードで、libcと一緒に配られるもの。
3. **crt0.Sの改名**（同指示）: `src/crt/crt0.S` は `.code32` で始まるi386専用のコードで、pcatとpc98だけが使う。
   他が `crt0-<arch>.S` の形なので `crt0-i386.S` にする。
4. 参照の置換: `#include`、Makefile・`*.mk`、sysroot生成、`plan/*/tests/` の試験、文書のパス。
   計画の記録（過去の結果文書）は書き換えない。

## 入力

- 対応表 [refactor-map.md](../refactor-map.md) のp003の節と `../refactor-refs/refs.tsv`。
- 再生成: `python3 plan/ws035/tests/refactor-refs.py`。
- p003の参照は計9,343行。うち計画の記録が9,101行で、書き換えが要るのは216行
  （build 188、plan-tests 24+2、c-include 13、sysroot 11、source-text 3、docs 1）。

## 受け入れ

- `libc/` に残るのは `include/` だけ。`src/crt/` は無い。
- 旧パスへの参照が、計画の記録を除いて0件（`refactor-refs.py` の `stale.tsv` で確認）。
- amd64とi915-amd64のkernelとuserlandのbuildが通り、warning 0。
- `kernel-include-audit.py --require-none` がPASS（libcヘッダの読込み0）。
- `make disk-image` が通り、Phaseの最後に `plan/tools/boot-test.sh` を1回実行してPASS（行頭の `login:`）。
  host fixtureの46件は流さない（2026-09-23ユーザー指示。移動だけのPhaseでは、buildの正常性とlogin promptで足りる）。
- `git diff --check` PASS。

## 範囲外

`libc/include/` の移動（p023）、bootヘッダの移動（p004）、amd64以外のbuild、実機。

## 結果（2026-09-23）

- **移動**: `libc/` の50ファイルと `regex/` を `src/libc/` へ、`src/crt/` の9ファイルを `src/libc/crt/` へ
  （計65ファイル、`git mv` で履歴を保持）。`crt0.S` → `crt0-i386.S`（`.code32` のi386専用で、pcatとpc98だけが使う）。
  `libc/include/` は動かしていない（p023）。
- **参照の置換**: 41ファイル。Makefile、6 platformの `vmunix.mk`、`toolchain/llvm/sysroot.mk`、`src/softfloat/softfloat.mk`、
  libcとuserlandのsource、`plan/*/tests/` の試験。過去の記録は書き換えていない。
- **build**: amd64 `BUILD=build/p003-check` warning 0、`amd64 vmunix check: PASS`（282 object）。
  i915構成 `BUILD=build/p003-i915` warning 0、check PASS。`make -j32 disk-image` エラー0、797,966,336 byte。
- **include監査**: `kernel-include-audit.py --require-none` **PASS**（kernel・HALのlibcヘッダ0、`libc/vulkan` 3のみ）。
  結果は `include-audit/`。
- **boot-test**: **PASS**。画面の行頭に本物の `login:`。`boot-test/login.png`、`login.txt`。
- **旧パス参照**: buildに関わるものは0件（残るのは過去の記録と設計文書の記述）。`git diff --check` PASS。
- host fixtureの46件は、ユーザー指示により流していない（移動だけのPhaseのため）。
