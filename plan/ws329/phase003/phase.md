<!-- awesome-plan project=zedbsd record=ws329-p003 -->

# ws329-p003 encoder の header と骨格を source に置く、試験の道具、基点

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-31 p001 で作成）。
- 依存: p002。
- 担当の想定: Opus 5.5（phase-runner）。

## 目的と範囲

- [proposed/](../proposed/) の header（shared を除く）を `userland/desktop/libmedia/` へ置く。WS326 p026 の名前（`vcodec-*` か別の名）と p002 の結果に合わせて参照を直す。関数の仕様は変えない（変えた点は design.md に記録）。
- 各 header の `.c` を骨格で（全関数 `return ENOSYS;` か何もしない、static の表は空でなく型だけ合わせた最小の data か、表の Phase が埋める旨の `{ 0 }`）。Makefile の `LIBMEDIA_SOURCES` に足す（backend の表には登録しない）。
- 試験の道具: `plan/ws329/tests/run-unit.sh`（WS326 の形: 全部の h264enc-*.c・vcodec-*.c・h264sw-*.c（backend を除く）・bits.c を ASan・UBSan で link、`-Iplan/ws326/tests/unit` で `check.h` を使う、出力は `build/tmp/ws329-unit` の fresh_out、最後の行 `run-unit: PASS`）、`plan/ws329/tests/check-skeleton.sh`（h264enc*.h・vcodec-*.h の単独 compile を cc と clang で、全 .c の compile、`BASE=` で header の不変）。p002 の test-cabac_ctx・test-levels をこれで流す。
- 試験の土台 `plan/ws329/tests/unit/fixture.h`（p016〜p019 と p022 が共有する、実装の担当は変えない）: 試験の画（乱数・平らな面・平行移動）を作る補助、frame の pool と参照の frame（指定の画素）、SPS・PPS の store（h264enc-ps の代わりに試験の bitwriter で書いて decoder で読む）、I・P・B の slice の header と decoder の picture・slice の context（list・weights・direct の準備まで）を組む補助、encoder が書いた slice の RBSP を同じ準備の decoder の context で `h264sw_slice_data` に通して decode し戻す補助、二つの picture の画素と `struct h264sw_mb` の entry（parser の field と motion）を比べる補助。fixture 自体の試験 `test-fixture.c`。
- 骨格を commit した SHA を基点（BASE）として ws.md と全 phase.md の「確かめの command」に書く。

## 受け入れ

- `sh plan/ws329/tests/check-skeleton.sh` PASS（cc と clang、warning 0）。
- `sh plan/ws329/tests/run-unit.sh all` PASS（p002 の 2 本）。WS326 の `run-unit.sh all` が変わらず PASS。
- amd64 の libmedia.so の build warning 0。
- ws.md と phase.md に BASE が書いてある。

## 変えてはいけない物

- decoder の `.c`・header（p002 の後は WS329 の Phase でも変えない）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- 上の受け入れの command。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、warning 0
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。
