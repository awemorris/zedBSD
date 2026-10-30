<!-- awesome-plan project=zedbsd record=ws009-p014 -->

# ws009-p014: DOC-54 gpu-submission.md: GPU_COMMAND・SUBMIT・WAIT・GPU_PRESENT・session の失敗と回復

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
- src/drivers/gpu/gpu.c: `gpu_command_ioctl`・`gpu_submit_ioctl`・`gpu_wait_ioctl`・`gpu_completion_reserve`・`gpu_completion_discard`・`gpu_completion_observe`・`gpu_completion_observer_leave`・`gpu_completion_deadline`・`drv_gpu_complete`・`gpu_present_ioctl`・`gpu_console_end`（GPU_PRESENT の最初の flip）・`gpu_session_error_locked`・`gpu_session_fail_locked`・`drv_gpu_report_error`・`drv_gpu_report_session_error`・`gpu_poll`・`gpu_recover_open`・`gpu_session_reclaim`
- include/uapi/gpu.h の `struct gpu_command_submit`・`gpu_command_wait` の comment
- userland/desktop/libvulkan の VK_ERROR_DEVICE_LOST の扱い（1 文のため。`grep -rn DEVICE_LOST userland/desktop/libvulkan`）

## 2. 作る物

- `docs/reference/gpu-submission.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: gpu-recovery・gpu-i915（COMMAND_SUBMIT・WAIT）、venus-frame（venus-frame.c: GPU_COMMAND・GPU_PRESENT）。

## 4. 注意

- 受け付け（成功の戻り）は完了ではない。Vulkan の結果は stream の reply に入る（gpu-command-stream.md）。
- `GPU_COMMAND_WAIT` の 0 の timeout と有限の timeout で返る errno（EAGAIN か ETIMEDOUT）を code で確かめる。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-submission.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-submission.md plan/ws009/phase014/phase.md plan/ws009/ws.md`。

## 結果

2026-10-30、Sonnet 5.5 high（1 回の起動）。`docs/reference/gpu-submission.md` を 198 行にした（見出し・機械の列は変えていない）。

### 実行した command

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-submission.md`: 最後の行 `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 18 capset rows, 1196 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: 最後の行 `check-gpu-uapi-doc: PASS (... final 0 files)`
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (296 links)`
- `git diff --check`: 出力なし。

### 自分で決めた点

- `Requires:` は GPU_COMMAND が W と `GPU_CAP_COMMAND`、GPU_COMMAND_SUBMIT が W と `GPU_CAP_NOTIFICATION`（`GPU_CAP_COMMAND` は見ない）、GPU_COMMAND_WAIT が R と `GPU_CAP_NOTIFICATION`、GPU_PRESENT が W と `GPU_CAP_PRESENT`。gpu.c の各 handler の先頭で確認した。
- `GPU_PIXEL_*` の byte 順（名前の順、低い address から）は header に comment が無く、venus の driver の format 変換（BGRA は 1、RGBA は 67）と名前から書いた。
- timeline の意味は driver 定義とし、kernel が見ない事と、amdrdna の 64 以上の拒否だけを具体的に書いた（i915 は全ての値を受ける）。
- poll の POLLHUP は device の取り外し（online 0）の時だけ。spec は「device failure は POLLERR|POLLHUP」と書くが、code は登録されたまま失敗した device には POLLERR だけ。code に従った。
- GPU_PRESENT を受ける node は、`GPU_CAP_PRESENT` を立てる in-tree の driver が venus だけ（`VENUS_CAPABILITIES` の 63U に 32 を含む）。他の driver にこの bit を立てる所は grep で見つからなかった。

### source と spec・header の食い違い

- spec: device failure の poll は POLLERR|POLLHUP（上記）。code は POLLERR のみ（取り外しで POLLHUP）。
- header の `struct gpu_command_wait` の comment は UINT64_MAX を「completion or interruption」まで待つとするが、code は device の取り外し（ENODEV）でも終わる。
- `GPU_COMMAND` の `bytes` 超過は EINVAL、`GPU_COMMAND_SUBMIT` は E2BIG（code の差。header に記述なし）。
- `GPU_COMMAND_SUBMIT` は `GPU_CAP_COMMAND` ではなく `GPU_CAP_NOTIFICATION` で受ける。
- `GPU_COMMAND_SUBMIT` は driver が拒否しても `sequence` を利用者の構造に先に書き戻す（その sequence は何も指さない）。
- `GPU_PRESENT` の不明・他 open の handle は ENOENT でなく EINVAL。

### 自己の照合の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| 受け付けは完了でない。record は pending から terminal | gpu.c `gpu_submit_ioctl`、`drv_gpu_complete` | gpu-recovery/main.c（wait.status が ETIMEDOUT） |
| sequence は kernel 全体の counter から。他の open では ENOENT | `gpu_handle_allocate`、`gpu_completion_observe` | 無し |
| `GPU_SUBMIT_MAX` 個の record。使い切ると EAGAIN。job reserve と共用 | `gpu_completion_reserve`（`gpu_job_reserve_ioctl` と、`gpu_dependency_ioctl` が呼ぶ `gpu_submit_ioctl` からも呼ぶ） | 無し |
| 空の marker は bytes 0 と CONTEXT_FENCE と address 0 | `gpu_submit_ioctl` | gpu-recovery/main.c（recovery_decoder） |
| flags 0 で timeline 非 0 は EINVAL | `gpu_submit_ioctl` | 無し |
| 消費は copyout の後。失敗なら record は残る | `gpu_wait_ioctl`、`gpu_completion_observer_leave` | gpu-i915/main.c、gpu-recovery/main.c（CONSUME で wait） |
| timeout 0 は EAGAIN、有限は ETIMEDOUT（tick に切り上げ）、UINT64_MAX は無期限 | `gpu_completion_observe`、`gpu_completion_deadline`、`waitq_sleep` | 無し（gpu-recovery は有限の timeout の終了後の status を見る） |
| 消費中の並行の消費は EAGAIN | `gpu_completion_observe`（consuming） | 無し |
| WAIT は admission の外 | `gpu_ioctl_body` | 無し |
| session failure で pending の record は errno で terminal | `gpu_session_fail_locked` | gpu-recovery/main.c |
| 失敗した open は POLLERR。他の open も device 失敗で POLLERR | `gpu_poll`、`drv_gpu_report_error` | gpu-recovery/main.c（peer error readiness） |
| device 失敗の間、open は device の errno を返す。全ての古い参照を閉じた後の open が reset | `gpu_recover_open`、`gpu_open` | gpu-recovery/main.c（retirement gate errno、fresh recovery open） |
| 並行の open は reset の結果を待つ（EINTR あり） | `gpu_recover_open` | 無し |
| GPU_PRESENT の検査（4096、stride、storage、offset） | `gpu_present_ioctl` | venus-frame.c |
| 最初の flip で console が framebuffer を離れる | `gpu_ioctl`、`gpu_console_end`、`kern_text_end` | 無し |
| VK_ERROR_DEVICE_LOST は submit・wait の失敗、非 0 の status、reply の欠落から | userland/desktop/libvulkan/context.c の `vulkan_kernel_error`、`vulkan_decoder_wait`、wire.c | 無し |

### 3 つの読み直し

1. `GPU_COMMAND_WAIT` の `EAGAIN`: `gpu_completion_observe` で、timeout_ns が 0 かつ未 terminal なら `EAGAIN`、`GPU_WAIT_CONSUME` で `consuming` が立っていても `EAGAIN`。文書と一致。
2. `GPU_PRESENT` の `EINVAL` の handle: `gpu_resource_lookup` は handle 0 と見つからない handle の両方に `EINVAL` を返し、`ENOENT` は返さない。文書と一致。
3. `GPU_COMMAND_SUBMIT` の `E2BIG`: `bytes > GPU_COMMAND_MAX` は `E2BIG`（`GPU_COMMAND` は `EINVAL`）。`bytes == 0 && address != 0` は `EINVAL`。文書と一致。

### 未確認・未実施

- driver の `command`・`submit`・`present` が返す errno の網羅（文書の「Other values」は venus と amdrdna で見た値の例だけ）。
- 隔離（isolate）をする driver で、閉じた後の次の open が reset を走らせる経路は `gpu_recover_open` の条件（quarantined）から書いた。実機・QEMU では未確認。
- 記載のどの試験も実行していない（文書の phase なので host の検査のみ）。
