<!-- awesome-plan project=zedbsd record=ws009-p016 -->

# ws009-p016: DOC-54 gpu-jobs.md: job の reserve・commit・cancel・capacity・policy

Status: in-progress（2026-10-30 書いた・照合 PASS。cleared は Q1 が決める）
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
- src/drivers/gpu/gpu.c: `gpu_job_reserve_ioctl`・`gpu_job_action_ioctl`・`gpu_job_bind`・`gpu_job_unbind`・`gpu_job_admit`・`gpu_job_observe`・`gpu_capacity_ioctl`・`gpu_policy_ioctl`・`gpu_capacity_changed_locked`・`gpu_retirement_changed_locked`・`gpu_stop_waiting_locked`・`gpu_monitor_deadline`・`gpu_monitor_start`・`gpu_monitor_stop`・`gpu_monitor`・`gpu_monitor_step`・`gpu_monitor_close`、先頭の `CONFIG_GPU_JOB_*_MS` の既定
- include/uapi/gpu-job.h の comment
- src/drivers/gpu/venus/transport.c・venus/venus.c `venus_get_info`（strict queue）

## 2. 作る物

- `docs/reference/gpu-jobs.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: gpu-i915（JOB_RESERVE・COMMIT）。CANCEL・CAPACITY・POLICY は source tree の試験が無い。

## 4. 注意

- `GPU_JOB_CAPACITY` の WAIT は macro が無く flags 0。
- 期限の値は kernel の build の config で決まる（既定を書き、config の名前を書く。config/ci/*.mk の値は書かない）。
- 状態の遷移は表で。状態は gpu.c の `enum gpu_job_state` の物だけを使う（骨格の spec の言い換えより code が正）。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-jobs.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-jobs.md plan/ws009/phase016/phase.md plan/ws009/ws.md`。

## 結果

日付: 2026-10-30（host の `date`）。担当: Sonnet 5.5 high（1 回の起動）。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-jobs.md`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 781 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (... 781 placeholders, final 0 files)`
- `timeout 120 .../noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (420 links)`
- `git diff --check`: 出力なし（終了 0）。

書いた行数: docs/reference/gpu-jobs.md は 269 行（骨格 178 行）。

### 自分で決めた点

- 状態は gpu.c の `enum gpu_job_state` の 3 つ（reserved・committed・finished）を単語で書いた。`GPU_JOB_RESERVED` などの enum の名前は header に無く照合が FAIL にするため、文書に書かない。`GPU_JOB_NONE` は job でない記録の印なので「no record」と書いた。
- `Requires:` は RESERVE・COMMIT・CANCEL が「open mode W; capability `GPU_CAP_JOB`」、CAPACITY が「open mode none; capability `GPU_CAP_JOB_CAPACITY`」、POLICY が「open mode none; capability `GPU_CAP_JOB`」。code は writable と capability の bit の他に open mode を見ない。fence の fd を使う RESERVE も `GPU_CAP_FENCE` を見ない（`gpu_fence_get` は fd の型だけ）ので Y に入れていない。
- COMMIT が fence の遷移か driver の commit で失敗した時、kernel が driver の cancel を fault 付きで呼び、受け入れられると open が EIO で失敗する、という code の動きを本文に書いた（`gpu_job_action_ioctl`）。
- 「記録済みの失敗で終わった job への COMMIT・CANCEL はその記録の errno を返す」（`gpu_job_observe`）は kernel の側の errno なので `Errors:` の独立の項に書いた。
- venus の `ENOTSUP`・`EINVAL`（timeline 0 と 64 以上）・`EAGAIN`・`ESTALE` などは `Other values:` に「Venus driver の例」として書いた。venus 以外の driver の timeline の範囲は読んでいないので「Venus は 1〜63」だけを書いた。
- `available` は個数でなく 0/1 の hint と書いた（code は `request.available = 1U`）。

### source と header・他の文書の食い違い

- gpu-capabilities.md の `GPU_CAP_JOB_CAPACITY` の行は「how many reservations a timeline can still take」と書くが、code（`gpu_capacity_ioctl`）の `available` は 1 か 0 の hint。gpu-capabilities.md は触らない（他の phase の file）。統合の Phase で直すこと。
- gpu-submission.md は completion の `status` の例に「`ECANCELED` for a job reservation that was withdrawn」を挙げる。code では flags 0 の CANCEL は記録を discard して `listed` を 0 にするので GPU_COMMAND_WAIT は `ENOENT` を返し、`ECANCELED` を見るのはその記録を既に待っていた他の thread だけ。kernel が失敗の後に未 commit の予約を引き上げる時（`gpu_session_reclaim`）は先に付いた errno を上書きしない。header と code の矛盾でなく、他の file の例の精度の問題。
- header（gpu-job.h）と code の矛盾は見つからなかった。

### 自己の照合の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| RESERVE: writable が要り、`GPU_CAP_JOB` が無ければ EOPNOTSUPP。EACCES が先 | gpu.c `gpu_job_reserve_ioctl` | gpu-i915 main.c `test_job`（成功の路だけ） |
| RESERVE: flags・reserved・sequence 非 0、fd < -1、fd と generation の組が合わないと EINVAL | `gpu_job_reserve_ioctl` | 無し |
| RESERVE: fd は fence でなければ EBADF（型違い）か EINVAL（driver の物で fence でない）、別の device は EXDEV、古い generation は ESTALE、既に producer がいると EBUSY | `gpu_fence_get`、gpu-fence.c `fence_resolve`・`drv_gpu_fence_device`・`drv_gpu_fence_bind` | 無し |
| RESERVE: sequence は driver を呼ぶ前に書かれ、driver が拒むと何も指さない | `gpu_job_reserve_ioctl`（copyout の後に reserve、失敗で `gpu_completion_discard`） | 無し |
| RESERVE: admission を取らない | `gpu_ioctl_body`（`gpu_session_enter` の前に分岐） | 無し |
| RESERVE: EAGAIN は記録の slot 満杯か fence の binding 満杯、driver の空き無し | `gpu_completion_reserve`、`gpu_job_bind`、venus `drv_venus_transport_job_reserve` | 無し |
| RESERVE: 既に失敗した open は記録された errno、実行中の失敗は ENODEV | `gpu_completion_reserve`（`gpu_session_error_locked`）、`gpu_job_bind` | 無し |
| RESERVE: venus は timeline 0・64 以上が EINVAL、strict でなければ ENOTSUP（EINVAL の後） | venus transport.c `drv_venus_transport_job_reserve` | 無し |
| COMMIT: reserved の job だけ。committed なら EALREADY、記録が無ければ ENOENT、処理中なら EBUSY | `gpu_job_observe`、`gpu_job_action_ioctl` | gpu-i915 main.c `test_job`（成功の路だけ） |
| COMMIT: flags 非 0 は EINVAL | `gpu_job_action_ioctl` | 無し |
| COMMIT: 成功で committed になり execution deadline に替わり、fence が dependency になる | `gpu_job_admit`、gpu-fence.c `drv_gpu_fence_admit`・`fence_wait`（work） | 無し |
| COMMIT: 失敗は fault 付き cancel と EIO の open 失敗になる | `gpu_job_action_ioctl`（`lost`、`gpu_session_fail_locked`） | 無し |
| CANCEL: flags 0 は reserved のみ。記録を捨て fence を unbind。committed なら EALREADY | `gpu_job_action_ioctl`、`gpu_job_unbind`、`gpu_completion_discard` | 無し |
| CANCEL: CANCEL_FAULT は reserved・committed の両方で通り、open が EIO で失敗する | `gpu_job_action_ioctl` | 無し |
| CANCEL: flags の他の bit は EINVAL | `gpu_job_action_ioctl` | 無し |
| 終了済みの job への COMMIT・CANCEL は記録の errno（status 0 なら EALREADY） | `gpu_job_observe` | 無し |
| CAPACITY: `GPU_CAP_JOB_CAPACITY` が無ければ EOPNOTSUPP、open mode は見ない | `gpu_capacity_ioctl` | 無し |
| CAPACITY: QUERY は observed・timeout が 0、WAIT は observed が非 0 | `gpu_capacity_ioctl` | 無し |
| CAPACITY: observed が現在の sequence より大きいと EINVAL、sequence の飽和は EOVERFLOW | `gpu_capacity_ioctl` | 無し |
| CAPACITY: timeout 0 は EAGAIN、有限は ETIMEDOUT、UINT64_MAX は EINTR まで | `gpu_capacity_ioctl`、waitq.c `waitq_sleep` | 無し |
| CAPACITY: available は 0/1 で driver の空きと open の空き slot の両方 | `gpu_capacity_ioctl` | 無し |
| CAPACITY: 失敗した open は待たずに errno | `gpu_session_error_locked` の呼び出し | 無し |
| CAPACITY: sequence は 1 から、記録の退役・cancel・失敗・reset で進む | `drv_gpu_register`（1 の初期化）、`gpu_capacity_changed_locked`、`gpu_retirement_changed_locked` | 無し |
| POLICY: `GPU_CAP_JOB` が要り、open mode と健康は見ない | `gpu_policy_ioctl` | 無し |
| POLICY: 入力の他の欄が非 0 なら EINVAL、出力は ms * 1000000 | `gpu_policy_ioctl` | 無し |
| 既定は 10000・60000・10000 ms、1 未満は build の失敗 | gpu.c 先頭の `CONFIG_GPU_JOB_*_MS` | 無し |
| 期限切れは open を ETIMEDOUT で失敗させる。stop は 100 ms ごとの poll と stop の期限、超えると isolate か device の失敗 | `gpu_monitor_step`、`gpu_session_fail_locked` | 無し（gpu-i915 main.c `test_hang` は command の記録の hang を扱い、job の期限を直には使わない。内容は未確認） |
| この監督の期限は job の記録だけで、`GPU_COMMAND_SUBMIT` の記録に付かない | gpu.c の `job_deadline` の代入は 2 か所だけ（予約と commit） | 無し |
| close: reserved は ENODEV で終わり、committed は走り続ける | `gpu_session_fail_locked`（hard 0）、gpu.c の close、`gpu_session_reclaim` | 無し |
| reserved の予約が成功を報告されると EIO | `drv_gpu_complete` | 無し |

### `Errors:` から選んだ 3 つの読み直し

1. RESERVE の `EAGAIN`（記録 slot か fence binding の満杯）: `gpu_completion_reserve` が `completion->sequence != 0` の slot を飛ばし、全部使用中なら EAGAIN を返す。`gpu_job_bind` が `fences[]` に空きが無ければ EAGAIN を返す。書いた条件と合う。driver の EAGAIN は venus の `drv_venus_transport_job_reserve` の空き slot 無しの分岐と合う。
2. COMMIT の `EALREADY`（committed、または status 0 で終了済み）: `gpu_job_observe` が `completed != 0` なら `completion->error != 0 ? completion->error : EALREADY` を返し、続く `gpu_job_action_ioctl` が `state != reserved` かつ（CANCEL・fault・committed）でなければ EALREADY にする。書いた条件と合う。
3. CAPACITY の `EINVAL`（WAIT で observed が 0、または現在の sequence より大きい）: `request.flags == 0` の時に `observed_sequence == 0` が EINVAL、後でロックの下で `request.observed_sequence > device->capacity_sequence` が EINVAL になる。書いた条件と合う。補足: この比較は後の代入なので、open の失敗や sequence の飽和の errno より優先される。文書には順序まで書いていない。

### 未確認・未実施

- venus 以外の driver（i915・amdrdna・nvrtx・powervr・mali・bcm2711・ghci）の `reserve`・`commit`・`cancel`・`capacity` の errno と timeline の範囲は読んでいない。文書は venus の値だけを例に書いた。
- 試験は走らせていない（文書の phase。gpu-i915 は成功の路の出典としてだけ挙げた）。
- ISO の日付・管理番号・plan/ への link が文書に無いことは照合の script が確かめた。
