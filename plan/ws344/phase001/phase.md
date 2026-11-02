<!-- awesome-plan project=zedbsd record=ws344-p001 -->

# ws344-p001 設計（標準廉価実装手順 §4）

- Status: in-progress（P2、q1939-i01。成果は commit 済み、cleared は Q1 が決める）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。
- 範囲: libwebp-compat の module の分割、header（interface と仕様の comment）、.c の骨格、試験の道具、試験の画像の作り方、実装の Phase の表と各 phase.md、design-reviewer。

## 結果（2026-11-02 13:17、P2）

- 基点（BASE、header・骨格・試験の道具・build の規則・plan を凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。その後の commit は plan の文書だけ（phase.md・ws.md の BASE の SHA、この結果の節）。
- 作った物:
  - compat の header: `include/libc/compat/webp/types.h`・`decode.h`（libwebp 1.5.0 の名前と値、自分の宣言、ABI 0x0209、21 の関数と 4 つの macro）。
  - library: `userland/desktop/libwebp-compat/` の 19 の header と 20 の .c（common.c は実装済み、他は骨格で ENOSYS か 0）、`Makefile`（package の規則）・`sources.mk`・`exports.map`。`platform/common/userland-dynamic.mk` に libwebp-compat.so の規則。
  - 試験の道具: `plan/ws344/tests/`（run-unit.sh、check-skeleton.sh、target-build.sh、gen-sources.py、fixtures.list（364 件）、fixture-encoder.c、craft.py、make-fixtures.sh、fixtures.sha256、fixture-check.c・run-fixtures.sh、webp-driver.c・compare.sh、fuzz-target.c・mutate.sh・fuzz.sh、vp8-tables-extract.py、unit/check.h・lbitwriter.h・boolwriter.h・test-helpers.c・test-common.c）。
  - plan: design.md、ws.md の Phase の表（p002〜p022）、phase002〜phase022 の phase.md。
- 確かめた事（command と最後の行）:
  - `sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`（BASE= の照合も PASS）。
  - `sh plan/ws344/tests/run-unit.sh all` → `test-common: PASS (32 checks)`、`test-helpers: PASS (26 checks)`、`run-unit: PASS`。
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`。export は 21（llvm-nm -D で数えた）、check-dynamic-elf は make の中で通った。
  - `sh plan/ws344/tests/make-fixtures.sh` → `make-fixtures: PASS (364 files, 350 decoded by dwebp, hashes same; build/tmp/ws344-fixtures)`（2 回の生成で全 file の SHA-256 が同じ。dwebp が読めない 14 件は animation 2 と、わざと壊した container 12）。
  - 骨格に対して: `mutate.sh lossy-photo-64x48` → PASS（217 入力）、`fuzz.sh 5` → PASS（約 530 万回）、`compare.sh`・`run-fixtures.sh` は骨格なので FAIL（道具が動くことの確かめ）。
  - YUV → RGB の規則（output.h）を dwebp 1.5.0 の -yuv と -pam（fancy・nofancy）に対して 37x23 の試料で全画素一致、premultiply を libwebp の rgbA と全画素一致（host の小さな program、tree に入れていない）。
  - design-reviewer 1 本: 指摘 25 件（blocker 0、major 6、minor 19）、全部を反映か理由付きで記録（design.md §12）。
- 自分で決めた点: design.md §4 の表（置き場所、sysroot の複写、frame 全体の後の filter、bool の ended、16 bit の wrap、RFC と libwebp の差の扱い、table の上限、common.c を p001 で実装）。
- Q1 の判断（ws.md）: 1〜4 は回答済み（ユーザーの特許の決定を含む）。残り: design.md §11 の 5（公式の試験の data の取得）。
- 未実施: QEMU・実機・Windows の build（p020・p021）、arm64・riscv64 の target の build（sysroot の複写は amd64 だけ）。
- 消してほしい物: 無し（使い捨ての出力は worktree の `build/tmp/ws344-*` と `build/amd64/`、scratchpad）。
