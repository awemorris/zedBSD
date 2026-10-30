<!-- awesome-plan project=zedbsd record=ws009-p015 -->

# ws009-p015: DOC-54 gpu-fences.md: fence の要求と *_SYNC

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
- src/drivers/gpu/gpu-fence.c（全体、626 行）
- src/drivers/gpu/gpu.c: `gpu_fence_ioctl`・`gpu_fence_create_ioctl`・`gpu_fence_get`・`gpu_fence_state_ioctl`・`gpu_fence_bind_ioctl`・`gpu_fence_binding_add`・`gpu_fence_binding_end`・`gpu_fence_retire`・`gpu_dependency_wait`・`gpu_dependency_ioctl`
- include/uapi/gpu-fence.h の comment

## 2. 作る物

- `docs/reference/gpu-fences.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: acquire-fence（FENCE_CREATE・BIND・SIGNAL）、gpu-fence（中を grep で確かめる）。QUERY・WAIT・RESET・*_SYNC は source tree の試験が無いかもしれない。

## 4. 注意

- `GPU_COMMAND_SUBMIT_SYNC`・`GPU_DISPLAY_PRESENT_SYNC` の外側は header を持たない。入れ子の command・present が version と入れ子の size を持つ。
- `GPU_DISPLAY_PRESENT_SYNC` の present の意味は gpu-display.md（別の Phase）が書く。ここは待ちと signal と errno を書き、present の欄は link で済ませる。
- fence の要求が `GPU_CAP_FENCE` を要るかは `gpu_fence_ioctl` を読んで要求ごとに決める。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-fences.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-fences.md plan/ws009/phase015/phase.md plan/ws009/ws.md`。

## 結果

2026-10-30、Sonnet 5.5 high（1 回の起動）。`docs/reference/gpu-fences.md` を 313 行にした（見出し・機械の列は変えていない）。

### 実行した command

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-fences.md`: 最後の行 `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 768 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: 最後の行 `check-gpu-uapi-doc: PASS (... final 0 files)`
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (394 links)`
- `git diff --check`: 出力なし。

### 自分で決めた点

- `Requires:` は fence の 6 つが `GPU_CAP_FENCE`（QUERY・WAIT が R、他が W。`gpu_fence_ioctl` で、bit の検査が open mode の検査より先）。`GPU_COMMAND_SUBMIT_SYNC` は W と `GPU_CAP_FENCE` and `GPU_CAP_NOTIFICATION`、`GPU_DISPLAY_PRESENT_SYNC` は W と `GPU_CAP_FENCE` and `GPU_CAP_DISPLAY`（入れ子の handler が必ず見る bit を含めた。`gpu_dependency_ioctl` では open mode の検査が bit の検査より先）。
- 骨格に無い小見出しは H5（Fence model の下に 6 つ）。
- `_SYNC` の prerequisite の待ちは、driver が完了の責任を引き受けた世代（job の commit で `admitted`）だけを待つ。`GPU_FENCE_BIND` だけで束縛した pending の世代は待たずに `EAGAIN`。`drv_gpu_fence_admit` の呼び手が `gpu_job_admit` だけである事を grep で確かめて書いた。
- kernel が失敗で終えた束縛は slot を open の close まで残す、と書いた（`gpu_fence_binding_end` が終了済みの世代に `EPERM` を返して slot を解放せず、他に解放する所が無い事を読んで書いた。試験は無い）。
- `GPU_DISPLAY_PRESENT_SYNC` の present が失敗した時、target は present の errno で `GPU_FENCE_ERROR` になる（`gpu_dependency_ioctl` の `gpu_fence_binding_end(..., error)`）。command の失敗は束縛を解放して pending のまま。
- fence の descriptor への read・write は `EBADF`、ioctl は `ENOTTY`（`sys_read_call`・`sys_write_call`・`sys_ioctl_call` を読んだ）。
- `session == NULL` の `ENODEV`（`gpu_ioctl_body`）は、成功した open では起きないので `Errors:` に書かなかった。`waitq_sleep` の内部の `EBUSY`・`EINVAL` も同様に書かなかった。

### source と spec・header の食い違い

- `gpu_fence_get` の `foreign` が 1（`GPU_DISPLAY_PRESENT_SYNC` の `wait_fd`。`gpu_dependency_wait`、display の commit の plane の fence も同じ）の時、device の同一性を一切比べない。comment は「approved companion GPU」とするが、承認の検査は code に無い。code に従い「device を比べない」と書いた。
- header の `struct gpu_fence_bind` の comment は「One GPU open owns pending work until verified signal, rollback, or final close」とだけ言い、kernel が起こした失敗で終えた束縛が slot を残す事と、device の失敗の時の `ENODEV`（open 自身の失敗では止まらない）には触れない。
- header の `struct gpu_command_submit_sync` の comment は kernel の依存の待ちを書くが、`GPU_FENCE_BIND` だけで束縛した pending の世代は待たずに `EAGAIN` になる点は書いていない。

### 自己の照合の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| 新しい fence は generation 1、signaled なら SIGNALED、所有者なし | gpu-fence.c `drv_gpu_fence_create`、gpu.c `gpu_fence_create_ioctl` | acquire-fence/main.c `fence_open` |
| 入力の検査（fd -1、signaled 0/1、reserved 0、generation 0、flags の bit） | gpu.c `gpu_fence_create_ioctl` | 無し |
| 失敗した descriptor の commit は fd を書いた後でも descriptor を作らない | gpu.c `gpu_export_install` | 無し |
| fence は device 固有。別 device は EXDEV。PRESENT_SYNC の wait_fd だけ比べない | gpu.c `gpu_fence_get`、gpu-fence.c `drv_gpu_fence_device` | 無し |
| 別の種類の GPU descriptor は EINVAL | gpu-fence.c `fence_resolve` | 無し |
| QUERY だけ generation 0 を受ける。他は EINVAL。違う generation は ESTALE | gpu.c `gpu_fence_state_ioctl`、gpu-fence.c `drv_gpu_fence_query`・`fence_wait` | 無し |
| WAIT: 0 は EAGAIN、有限は tick に切り上げて ETIMEDOUT、UINT64_MAX は無期限、EINTR、ERROR でも成功 | gpu.c `gpu_completion_deadline`、gpu-fence.c `fence_wait` | 無し |
| RESET: 次の世代、所有者がいれば EBUSY、UINT64_MAX で EOVERFLOW、待っている者は ESTALE | gpu-fence.c `drv_gpu_fence_reset`・`fence_wait` | 無し |
| SIGNAL: 束縛した open だけ。error は 0 以上。負は EINVAL | gpu.c `gpu_fence_state_ioctl`、`gpu_fence_binding_end`、gpu-fence.c `drv_gpu_fence_signal_deferred` | acquire-fence/main.c `fence_signal` |
| BIND: 一つの世代に所有者は一つ。pending でなければ EBUSY | gpu-fence.c `drv_gpu_fence_bind` | acquire-fence/main.c `fence_open` |
| BIND の sequence は listed の record だけ。無ければ ENOENT。その record の失敗で ERROR | gpu.c `gpu_fence_binding_add`、`drv_gpu_complete` | 無し |
| 束縛の数は `GPU_SUBMIT_MAX`。超えると EAGAIN | gpu.c `gpu_fence_binding_add` | 無し |
| RELEASE は sequence を持つ束縛に EBUSY、世代は pending のまま | gpu.c `gpu_fence_binding_end`、gpu-fence.c `drv_gpu_fence_unbind` | 無し |
| device が停止・失敗なら BIND は ENODEV。open 自身の失敗では止まらない | gpu.c `gpu_fence_binding_add` | 無し |
| open の失敗・device の失敗・最後の close で束縛された世代は ERROR（errno、errno、ENODEV） | gpu.c `gpu_session_fail_locked`、`drv_gpu_report_error`、`gpu_fence_retire` | 無し |
| job の束縛は利用者が signal・release できず EPERM | gpu.c `gpu_fence_binding_end`（`binding->job`） | 無し |
| descriptor は ERROR で POLLERR。terminal で POLLIN。reset で戻る | gpu-fence.c `fence_poll`、`drv_gpu_fence_reset` | 無し |
| read・write は EBADF、ioctl は ENOTTY | syscall.c `sys_read_call`・`sys_write_call`・`sys_ioctl_call` | 無し |
| fence の要求は admission の外。session の失敗では止まらない | gpu.c `gpu_ioctl_body`・`gpu_fence_ioctl` | 無し |
| `_SYNC`: 入れ子の version と size（40、80）だけを見る | gpu.c `gpu_dependency_ioctl` | 無し |
| `_SYNC`: 不在は -1 と 0、他の組は EINVAL | gpu.c `gpu_dependency_ioctl`・`gpu_dependency_wait` | 無し |
| `_SYNC`: 待ちは admission の前。ERROR の fence の error をそのまま返す | gpu.c `gpu_dependency_wait` | 無し |
| `_SYNC`: 待てるのは terminal か job が admit した世代。それ以外の pending は EAGAIN | gpu-fence.c `fence_wait`・`drv_gpu_fence_admit`、gpu.c `gpu_job_admit` | 無し |
| SUBMIT_SYNC: target は束縛してから submit。失敗なら解放。成功なら record に結ぶ | gpu.c `gpu_dependency_ioctl` | 無し |
| PRESENT_SYNC: target は present が返る時に SIGNALED か ERROR | gpu.c `gpu_dependency_ioctl` | 無し |
| PRESENT_SYNC の成功で console が離れる | gpu.c `gpu_console_end` | 無し |

### 3 つの読み直し

1. `GPU_FENCE_RESET` の `EBUSY`: gpu-fence.c の `drv_gpu_fence_reset` は generation の一致を見た後に `fence->owner != NULL` で `EBUSY` を返す。所有者は束縛の open か job。signal・unbind で `owner` は NULL に戻る。文書と一致。
2. `GPU_FENCE_BIND` の release の `EBUSY`: `gpu_fence_binding_end` は `release != 0U && binding->sequence != 0U` で `EBUSY` を返す（gpu.c の該当の行を読み直した）。束縛の追加（`EBUSY`）は `drv_gpu_fence_bind` の「pending でない、または所有者あり」。文書の二つの項と一致。
3. `GPU_FENCE_WAIT` の `EAGAIN`: `fence_wait` は pending の間のループで `immediate != 0U`（`timeout_ns` が 0）なら `EAGAIN`。`work` の経路（`_SYNC` の待ち）は `admitted == 0U` で `EAGAIN`。文書の WAIT と `_SYNC` の項と一致。

### 未確認・未実施

- fence の要求の QUERY・WAIT・RESET・BIND の release・`_SYNC` の二つは source tree に直の試験が無い（`gpu-fence` は Vulkan を通すだけ。libvulkan の `external-fence.c`・`wsi-display.c` が使うが tests ではない）。実機・QEMU での確認は未実施。
- `_SYNC` の入れ子の submit・present の errno は gpu-submission.md・gpu-display.md の列挙を参照する形にした。driver の返す値は網羅していない。
- 他の file（gpu-jobs.md、gpu-requests.md、gpu-display-control.md の COMMIT の fence）との二重の主張の照合は統合の Phase に残す。
