<!-- awesome-plan project=zedbsd record=ws009-p021 -->

# ws009-p021: DOC-54 gpu-examples.md: compile できる例

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: なし（骨格 4dfcce008 と照合の script が main にあること）。p010〜p021 は互いに独立で、並べて起動してよい
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- include/uapi/gpu*.h（例で使う要求の構造体と comment）
- gpu.md・gpu-submission.md・gpu-display.md の骨格の spec（例が示す規約）
- userland/tests/gpu-admission・gpu-recovery・display-control・ghci-display の main.c（呼び方の参考。写さず、自分で書く）
- 例の中で独自の名前に `GPU_`・`gpu_` を付けない（gpu-doc-rules.md 2 節の 10）

## 2. 作る物

- `docs/reference/gpu-examples.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 表の Meaning の列（あれば）を全行。
- 節ごとの主張の表と、3 つの読み直し（gpu-doc-rules.md 4 節の要求の節の無い file の形）。
- 出典にする試験の見当: この file は要求の節を持たない。Where the tests are の節に userland/tests への link。

## 4. 注意

- 各例は ```c の block で、1 行目を `/* example: NAME */`（NAME は info・storage・submit-wait・display）。include は `<uapi/gpu.h>` などと POSIX の header。
- 使う構造体ごとに `_Static_assert(sizeof(struct ...) == N, ...)`。N は構造体の表の大きさ。
- submit-wait の stream は gpu-op.h の命令から最小の物を作るか、bytes を呼び手が用意する関数に分ける。動かす試験はしない（compile だけ）と文書に書かない（文書は例の性質だけを書く）。
- 確かめに `--examples` を足す（3 節）。sysroot の無い target は script が飛ばして表示する。どの target で compile したかを結果の節に書く。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final docs/reference/gpu-examples.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-examples.md plan/ws009/phase021/phase.md plan/ws009/ws.md`。

## 結果

2026-10-30 Sonnet 5.5 high が書いた。docs/reference/gpu-examples.md を 884 行にした（4 つの例と Where the tests are）。見出し（H1〜H4）は骨格 4dfcce008 のまま。cleared は Q1 が決める。

実行した command と最後の行（worktree の root）:

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final docs/reference/gpu-examples.md`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 705 placeholders, final 1 files, 4 example compiles)`
- 同じ command を `GPU_DOC_BUILD=/home/awe/zedBSD-claude1/build` 付きで: `... final 1 files, 12 example compiles)` PASS。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (... 705 placeholders, final 0 files)`
- `timeout 120 .../noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (505 links)`
- `git diff --check`: 出力なし（終了 0）。

どの target で compile したか: worktree の `build/` には amd64 の sysroot（`build/amd64/sysroot`、clang は `build/llvm/bin/clang`）だけがあるため、既定の実行は x86_64-unknown-zedbsd の 1 target × 4 例 = 4 compile。arm64 と riscv64 の sysroot は main の checkout の `/home/awe/zedBSD-claude1/build/{arm64,riscv64}/sysroot` にあり（読むだけ、build していない）、`GPU_DOC_BUILD` でそこを指して x86_64・aarch64・riscv64 の 3 target × 4 例 = 12 compile が PASS。どちらも C11、`-Wall -Wextra -Werror -fsyntax-only`。動かす試験はしていない（compile だけ）。

自分で決めた点:

- info: capability の表は gpu.h・gpu-allocation.h・gpu-fence.h・gpu-display.h・gpu-job.h の全 bit（17 個）。`GPU_CAP_DISPLAY` の bit を見てから `GPU_DISPLAY_QUERY`（`GPU_DISPLAY_COUNT_ONLY`）を呼ぶ形で「bit を見てから要求」を示した。
- storage: 蓄積の領域は 4096 bytes。blob の部分は `GPU_BLOB_MAPPABLE` の blob だけを map する（storage の map は driver 次第と gpu-memory.md が書くため）。
- submit-wait: stream は gpu-op.h の命令だけで作る 68 bytes（`GPU_OP_SET_REPLY_STREAM`・`GPU_OP_SEEK_REPLY_STREAM`・`GPU_OP_ENUMERATE_INSTANCE_VERSION`+`GPU_OP_FLAG_REPLY`）で、libvulkan の枠組みと venus-frame の client.c の呼び方に合わせた。reply は読まない（本文に書いた）。ETIMEDOUT は最大 5 回（5 秒）待ち直す、EINTR は待ち直す、EIO・ENODEV と非 0 の status（その後 `GPU_GET_INFO` が失敗した時）は close して開き直し 1 度だけやり直す。
- display: 出力は最初の connected・FIFO・非 LIMITED・BGRA・現在の mode の frame が `max_frame_bytes` に入る物。frame は storage に `GPU_COPY_MAX` ずつ書く。ESTALE は query からやり直して 3 回まで、EBUSY は報告して終わり、wait の ETIMEDOUT・EINTR は 3 回まで。
- 独自の名前に `GPU_`・`gpu_` を付けていない（`request`、`release`、`wire_u32`、`show_frame` など）。
- Where the tests: 直に要求を使う 12 個（gpu-admission・gpu-recovery・gpu-i915・venus-frame・amdrdnactl の gpu-check.c・display-inventory・display-scanout・display-control・gpu-bars・ghci-display・hvs-load・acquire-fence）。骨格の spec に無い gpu-bars・hvs-load・amdrdnactl・acquire-fence は grep で要求名を確かめて足した。gpu-share は WSI の helper を通すので載せなかった。Vulkan 経由は gpu-selftest・gpu-fence。display-events も Vulkan 経由だが骨格の指定に無いので載せなかった。

節ごとの主張の表（文書には置かない）:

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| info: `GPU_GET_INFO` は version・size を入力に取り、`GPU_DEVICE_QUERY` は bit を要らない | gpu-capabilities.md、gpu.c `gpu_info_ioctl`、`gpu_device_query_ioctl` | gpu-admission/main.c、display-scanout/main.c |
| info: `GPU_DISPLAY_QUERY` の `GPU_DISPLAY_COUNT_ONLY` は count だけを返す。bit が無ければ `EOPNOTSUPP` | gpu-display.h の comment、gpu.c `gpu_display_ioctl` | display-inventory/main.c |
| storage: 1 回の転送は `GPU_COPY_MAX` まで、address は 64 bit の user pointer | gpu.c `gpu_transfer_ioctl` | gpu-admission/main.c |
| storage: map は token を返し、mmap は `MAP_SHARED`、mmap の間は destroy が `EBUSY` | gpu.c `gpu_map_ioctl`、`gpu_mmap`、`gpu_destroy_ioctl` | amdrdnactl/gpu-check.c |
| storage: `GPU_RESOURCE_MAP` は読みの open を要る | gpu.c `gpu_map_ioctl` | amdrdnactl/gpu-check.c |
| submit-wait: 成功は受理であって完了ではない。`GPU_WAIT_CONSUME` が record を退役させる | gpu.c `gpu_wait_ioctl`、`gpu_completion_observer_leave` | gpu-recovery/main.c、gpu-i915/main.c |
| submit-wait: 有限の timeout の満了は `ETIMEDOUT`、record は不変、`EINTR` は再開しない | gpu.c `gpu_completion_observe`、src/kern/waitq.c `waitq_sleep` | gpu-recovery/main.c（status 側） |
| submit-wait: 失敗した open は `GPU_GET_INFO` も失敗し、close して開き直す | gpu-submission.md の Session failure and recovery、gpu.c `gpu_session_enter` | gpu-recovery/main.c |
| submit-wait: 68 bytes の stream の並びと reply の書き場所 | gpu-op.h の `GPU_OP_SET_REPLY_STREAM`・`GPU_OP_SEEK_REPLY_STREAM`・`GPU_OP_ENUMERATE_INSTANCE_VERSION` の comment | venus-frame/client.c |
| display: claim の `ESTALE`・`EBUSY`・`ENOSPC`・`ENXIO` は driver の答え | gpu-display.md、gpu.c `gpu_display_ioctl` | display-scanout/main.c |
| display: 通常の storage は present が返った後は借りられていない | gpu-display.h の comment、gpu-display.md | display-control/main.c、gpu-bars/main.c |
| display: wait は 1 回で最大 1 秒、timeout は `ETIMEDOUT` | gpu-display.h `struct gpu_display_wait` の comment | hvs-load/main.c |

表の根拠のうち code を直に読み直したのは次の 3 つ。他の行の根拠は gpu-*.md と header の comment による（code は未読）。読み直した 3 つは文書の主張に合った:

1. `gpu_wait_ioctl`・`gpu_completion_observe`: timeout 0 は `EAGAIN`、有限の期限の満了は `waitq_sleep` の `ETIMEDOUT`（src/kern/waitq.c の 221 行の `return ETIMEDOUT;`）、signal は `EINTR` で record に触らず、`GPU_WAIT_CONSUME` は copyout が成功した時だけ退役。例の待ち直しの形と合う。
2. `gpu_destroy_ioctl`: `mapping_count != 0` で `EBUSY`。例が `munmap` の後に destroy する理由と合う。
3. `gpu_transfer_ioctl`: `bytes == 0 || bytes > GPU_COPY_MAX` は `EINVAL`、offset と範囲の検査、address の検査。例が frame を `GPU_COPY_MAX` ずつ書く理由と合う。

source と header・spec・文書の食い違い: 見つからなかった。header の `struct gpu_display_wait` の comment と gpu-display.md の wait の節は合う。

未確認・未実施: 例を動かす試験（compile だけ）、実機・QEMU での動作、arm64・riscv64 の worktree 内の sysroot（main の checkout の sysroot で compile した）。`GPU_DISPLAY_WAIT` が `ETIMEDOUT` を返す具体的な driver の分岐は読んでいない（header の comment と文書による）。
