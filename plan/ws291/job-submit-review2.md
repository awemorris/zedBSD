<!-- awesome-plan project=zedbsd record=ws291-job-submit-review2 -->

# design-reviewer（2 回目）: job-submit-design.md の「2026-10-31 の改訂」（nvrtx を最初の driver に）

- 対象: [job-submit-design.md](job-submit-design.md) の全文（案 A・B、1 回目の reviewer の節、2026-10-31 の改訂）。書いた: 2026-10-31 design-reviewer。
- 照らした code: worktree `p3-ws291wsi` の HEAD 692209073（行番号は全て HEAD の物、`git show HEAD:<path>` で数えた）。
- 注意（観測した事実）: review の途中（2026-10-31 22:27 の `git status`）で、同じ worktree に実装の未 commit の差分が現れた
  （gpu.c +354、nvrtx runtime.c +85、vk-queue.c +5、gpu-job.h +86、gpu.h +8、7 driver の ops の表）。以下で「WIP」と書く所はその時点の差分
  （変わりうる）。設計の文と WIP が食い違う所も指摘に入れた（設計が本体なので、文を直すか WIP を直すかを決める必要がある）。
- 件数: blocker 1、major 6、minor 10。

## Blocker

### BL1. 「全部が確かな拒否（OOM）→ CANCELED」は nvrtx では誤り: 拒否の前に一部の run が GPU に載っている

- 根拠:
  - `src/drivers/gpu/nvrtx/vk/vk-queue.c:141〜166`（queue_submit）: command buffer を順に `nvrtx_vk_execute` し「the first that fails ends the
    submission」。2 本目以降が失敗した時、1 本目の run は既に GPU にある。結果は `queue_result(error)`、ENOMEM は
    `vk-reply.c:35` で `VK_ERROR_OUT_OF_DEVICE_MEMORY`。
  - `src/drivers/gpu/nvrtx/vk/vk-execute.c:491`（`execute->early = true`）と `:961`（execute_room → `execute_flush`）: 1 本の command buffer の
    中でも、最初の push buffer は数 draw で GPU に送られる。その後の record・slot の確保の ENOMEM（`:473`・`:566`・`:658`）は「一部が走った後の
    OOM」になる。
  - 設計の B1 の規則（design.md 153〜156 行「全部が確かな拒否 → CANCELED」）と、改訂の 1（211〜213 行「確かな拒否（ENOMEM 系）の数」を数える）。
    WIP の uapi の comment（gpu-job.h の CANCELED「each was definitely refused」）と gpu.h の comment も同じ文。
- 起こる状況: vkQueueSubmit の 2 本目の command buffer（または長い 1 本の後半）で kern_calloc・GPU の確保が失敗 → CANCELED → core は
  `jobs->cancel(0)`・`gpu_job_unbind`・`gpu_completion_discard`（gpu.c:6136〜6143 の CANCEL の本体と同じ）で marker を外し、fence を
  pending・未 bind に戻す。GPU は前半の run を監督なしで走らせ続ける。アプリは OOM を受け、Vulkan の約束（失敗した submit は資源の状態を
  変えない）を信じて buffer を解放しうる → 走っている GPU の DMA が解放した memory を読む・書く（use-after-free の DMA）。私用の fence と次の
  submit の marker は後の run に付くので、この前半の run の完了を誰も待たない。
- 既存の不具合でもある（推測ではなく code で確かめた）: 今の 3 本の道でも libvulkan の `sync.c:749` は native_status が OOM で accepted で無い時
  `GPU_JOB_CANCEL`（fault 無し）を出す。nvrtx の queue_submit が部分的に走った後の OOM を返すと同じ事が起きる。案 A はこれを kernel の規則として
  固めてしまう。
- WIP との関係: WIP の `runtime_job_command` は「失敗した submission は一部が GPU に載りうる」として、失敗が 1 つでもあれば FAULTED にしている
  （規則としては安全側）。つまり設計の文・uapi の comment・gpu.h の comment と WIP が食い違っている。amdrdna を「後で同じ形」で足す担当は設計の
  文に従うので、文を直さないと同じ誤りが入る。
- 直し方の案:
  1. outcome の規則を「CANCELED は、走った queue の操作が GPU に run を 1 つも載せていない時だけ」に書き換える（拒否の種類で決めない）。
     nvrtx は executor が「この frame で run を GPU に送った数」を数える（`execute_flush`・`execute_submit` が `nvrtx_vk_gpu_submit` を呼んだ数、
     または frame の前後の `session->run_fence` の比較）。送った数が 0 で全ての queue の操作が拒否 → CANCELED、送った数 > 0 で拒否あり → FAULTED。
  2. uapi の comment・gpu.h の op の comment・design の B1 と改訂の 1 を同じ文に直す。
  3. 今の 3 本の道の不具合は別に扱う（nvrtx の queue_submit が一部を送った後の失敗を `VK_ERROR_DEVICE_LOST` で返す、が最小）。Bug Board に
     載せるかは Q1。

## Major

### M-A. 「frame 自体の失敗は FAULTED」で、今は戻せる OOM が context の喪失になる

- 根拠: `vk-stream.c:371〜373`（外の blob の stream を `kern_malloc(length)` で写す、length は最大 `STREAM_COPY_BYTES` 64 MiB）。
  libvulkan は `GPU_COMMAND_MAX - 128` を越える stream を必ず blob で送る（context.c の vulkan_command_execute、`writer->bytes > GPU_COMMAND_MAX - 128`）。
  今の道: GPU_COMMAND が ENOMEM → `vulkan_kernel_error`（context.c:576〜582）が `VK_ERROR_OUT_OF_DEVICE_MEMORY` → finish が CANCEL（fault 無し）、
  context は生きる。設計の改訂の 1（213 行「frame 自体の失敗（EIO など）は FAULTED」）と WIP（`if (error != 0) outcome = FAULTED`）では
  session を失う。
- 起こる状況: 大きな command buffer の submit の時に kernel の memory が一時的に足りない → 今は vkQueueSubmit が OOM を返して続くのに、
  案 A では device lost でアプリが終わる。End の失敗（`vk-command.c:470` command_end は結果を reply に書いて 0 を返す → 次の queue_submit が
  executable でない command buffer で EINVAL → INITIALIZATION_FAILED → FAULTED）も、GPU に何も送っていないのに context を失う（今の道も
  CANCEL_FAULT なので後退ではないが、BL1 の「送った数」で救える）。
- 直し方の案: frame が失敗しても「queue の操作で run を 1 つも送っていない」なら CANCELED にし、frame の errno を userspace に返す。
  `struct gpu_job_submit` の `reserved` を out の `error`（CANCELED・FAULTED の時の frame の errno、他は 0）にする（56 byte のまま、offset 52）。
  libvulkan は CANCELED で reply が読めない時、`error` を `vulkan_kernel_error` と同じ表で VkResult にする（ENOMEM・ENOSPC は OOM、他は lost）。
  WIP の header は未 commit なので今なら変えられる。

### M-B. monitor の起床を 1 回にまとめると、RESERVED の期限が frame の間に効かなくなる（1 回目の reviewer の「deadline 切れは admit の ENODEV」は成り立たない）

- 根拠: monitor は眠る期限を scan で決め（gpu.c:7966〜7984、`job_deadline` の最小、期限が無ければ deadline 0 = 無期限で眠る、gpu.c:7905〜7910）、
  期限切れの検出は起きた時だけ（gpu.c:7974〜7978）。今の reserve は RESERVED と `RESERVATION_MS` を置いた直後に起こす（gpu.c:6008）。
  `gpu_job_admit`（gpu.c:6284〜6343）は `completed`・`job_state`・error だけを見て、`job_deadline <= now` を見ずに EXECUTION の期限で上書きする。
  WIP の gpu_job_submit_ioctl は RESERVED を置く所で起こさない（最後に 1 回だけ）。
- 起こる状況: 定常の subs-N では monitor は既存の COMMITTED の 60 s の期限まで眠っている。SUBMIT の frame が長引いても（executor は slot の空き
  を `nvrtx_vk_gpu_wait` で待つ、vk-execute.c:768）10 s の期限は誰も見ず、admit が期限を 60 s に延ばす。共有の fence を待つ他の process
  （compositor）に ETIMEDOUT が届く時刻が、今の「reserve から 10 s」から「次に何かが monitor を起こした時」に変わる。nvrtx の待ちは run の時間で
  有界なので実害は小さい見込み（推測）だが、design（189 行の「確かめられた所」）と WS014 の monitor の契約の説明は今のままでは偽になる。
- 直し方の案（どれか）:
  1. SUBMIT の道の admit の前に `job_deadline <= sched_ticks()` を registry lock の下で見て、切れていれば `gpu_session_fail_locked(ETIMEDOUT, 1)`
     と同じ扱い（FAULTED）にする。起床を足さずに今の契約を保てる。
  2. RESERVED を置く時の起床だけ残す（2 回）。
  どちらにしても設計に「SUBMIT では RESERVED の期限を何が守るか」を書く。

### M-C. 起床を減らす効果の見込みが実際の起床の数と合っていない

- 根拠: 1 submit の monitor の起床は今 5 回: reserve の 2 回（gpu.c:6008・6044）、admit（6336）、COMMIT の解放（6166）、marker の retire の
  `drv_gpu_capacity_changed`（nvrtx runtime.c:1254 → gpu.c:938 → `gpu_capacity_changed_locked` の gpu.c:7585、無条件）。phase024 の diag2 は
  最初の 4 回を外して +9 %。WIP の SUBMIT は `gpu_job_admit` を再利用するので admit の起床（6336）が残り、最後の 1 回と retire の 1 回で 3 回。
  設計の「4 回を 1 回に」は 5 → 3 で、diag2（5 → 1）の効果を見込みに使えない。
- 起こる状況: 効果の基準（design 145〜149 行）で A が「効果なし」と出た時、ioctl の数の効果か monitor の起床の効果かが分からない。
- 直し方の案: 期待の効果の節を nvrtx の数で書き直し（今は RDNA の 35 µs の見積もりのまま、137〜143 行）、SUBMIT の中の起床の数を 1 と書くなら
  admit の起床を条件付きにする内部の関数（`gpu_job_admit` の wake を呼び手に任せる引数）を設計に書く。`gpu_capacity_changed_locked` の monitor の
  起床を `gpu_stop_waiting_locked` の時だけにする案（`gpu_retirement_changed_locked`、gpu.c:7599〜7622 と同じ規則）は既存の道も変えるので、
  別の提案として diag で測ってから。

### M-D. 試験の計画の前提が誤り: WS014 の GPU core の fixture は今の tree で build でき、PASS する

- 根拠（自分で走らせた、出力は scratchpad、repo は変えていない）: `plan/ws291/tests/run-gpu-layouts-host.sh` と同じ flag
  （`-include include/libc/sys/ioctl.h -include include/hal/hal.h -DKERN_UAPI_NATIVE -DKERN_USER_ABI_LP64`、同じ source の組、`-Wl,--gc-sections`）に
  `-Wno-unused-function` を足すと、
  - `plan/ws014/tests/gpu-supervision.c`（gpu.c を include）: build 成功、実行 rc=0「GPU supervision: foreign capacity, 64-record self-reap,
    observer pin, ..., reservation reclaim, reserved close and metadata-only close PASS」。
  - `plan/ws014/tests/gpu-job.c` ＋ `src/drivers/gpu/gpu.c`: build 成功、実行 rc=0「GPU jobs: rollback, admission, no-allocation commit, exact
    completion, consume race, fault isolation and retained close PASS」。
  - `-Werror` のままだと落ちる理由は `-Wunused-function`（job_stop_begin・fence_notification など）だけで、HAL_SPACE_*・tid_t・KERN_CLOCK_HZ の
    error は `-include hal/hal.h` で出ない。design 225〜226 行の「今の tree で build できない（範囲の外）」は誤り。
- 起こる状況: 計画のままだと、monitor・期限・reclaim・observer の pin を一番よく見る既存の回帰（M-B・M-C で触る所そのもの）を流さずに gpu.c を
  変える。
- 直し方の案: `plan/ws291/tests/` に run script を足し（layouts の script と同じ形、WS014 の file は `#include` で使い変えない）、改訂の前後で
  gpu-supervision と gpu-job を流す。SUBMIT・RETIRE の case は `gpu-layouts-host.c` と同じく ws014 の fixture を include した新しい host 試験に書く
  （1 回目の reviewer の失敗の注入の一覧、design 186〜188 行）。

### M-E. runtime.c の新しい道（DEFERRED・inline commit の順・cancel）と失敗の outcome を試す物が無い

- 根拠: nvrtx の host 試験（`plan/tools/nvrtx/nvrtx-vk-host-test.sh:21`、`nvrtx-vk-d3-host-test.sh:28`）は vk-queue.c・vk-execute.c を入れるが
  runtime.c を入れない。改訂の試験（222〜226 行）の実機は bench の成功の道と画素だけ。QEMU の Venus は jobs->command が無いので案 A の bit が
  出ず、今の 3 本の道しか通らない（案 B は通る）。
- 起こる状況: CANCELED・FAULTED・DEFERRED・EFAULT の後の libvulkan の扱い、DEFERRED の後の 3 本の道への戻り、outcome の数え方（BL1）が、
  一度も走らずに main に入る。
- 直し方の案: (1) nvrtx-vk の host 試験に queue_submit の数え方の case（0 本・1 本成功・2 本目で ENOMEM・End の失敗・入れ子の blob の中の
  queue の操作）を足す。(2) 失敗の注入の knob（debug の boot 変数か sysctl で、N 回に 1 回 DEFERRED・OOM を返す）を nvrtx に入れ、実機で
  vkcube か mview を短く回して後戻りの道を通す。(3) M-D の core の host 試験に stub の driver の `command` を置き、4 つの outcome と errno を全部通す。

### M-F. libvulkan の組み込みが未定義で、素直に書くと自分で deadlock する

- 根拠: `context->mutex` は既定の（再帰でない）pthread mutex（context.c:97）。返信の取り出しは `vulkan_context_execute` が mutex を取って
  `vulkan_context_transaction` を呼ぶ（context.c:512〜521）。今の reserve は自分で mutex を取る（sync.c:647）、finish も取る（sync.c:759）。
  返信の storage（`context->reply_mapping`）は context に 1 つなので、SUBMIT は transaction の中（mutex を持ったまま）で出すしかない。
- 起こる状況: DEFERRED の時「今の 3 本の道」（design 220 行）を transaction の中から `vulkan_sync_job_reserve` で始めると同じ mutex を二重に取る。
  EAGAIN の後の capacity の 2 回の試し（sync.c の attempt の loop、snapshot の後にもう 1 回）と `VK_NOT_READY` で queue・device の lock を手放す
  道（queue.c:362〜389）も、transaction の外に出てからでないと書けない。
  さらに設計の中で EBUSY の扱いが食い違う: 1 回目の reviewer の B2（160 行）は「fence の bind の EBUSY は今の reserve の EBUSY と同じ扱い」
  （今の reserve は EAGAIN・ENOMEM 以外を device lost、sync.c:675〜680）、libvulkan の節（80 行、220 行）は「EBUSY → 今の 3 本の道」。
- 直し方の案: 設計に libvulkan の流れを書く。例: queue_attempt で cap がある時は「SUBMIT の transaction」を 1 つの関数（mutex の下で reap_if、
  ledger の notification を作って pin、GPU_JOB_SUBMIT、sequence を入れて公開、返信を読む）にし、戻り値で COMMITTED・CANCELED・FAULTED・
  DEFERRED・EAGAIN・その他の errno を返す。DEFERRED と EAGAIN は mutex を放した後に、今の reserve（fence の prepare は `*prepared = VK_TRUE` の
  まま）・`vulkan_command_execute`・finish の道に入る。EBUSY は今の reserve と同じく device lost に揃える（3 本の道に行っても RESERVE が同じ
  EBUSY を返すだけ）。

## Minor

1. **capability の合成の文**（design 174 行「jobs->command がある時 GPU_CAP_JOB_SUBMIT」）: backend の `get_info` は登録より少ない cap を報告して
   よい（gpu.c:2680〜2687 の comment と mask）。合成は mask の後の `GPU_CAP_JOB` と `GPU_CAP_COMMAND` の両方を条件にする、と書く。WIP はそうして
   いる（文を WIP に合わせる）。
2. **op の契約**: 改訂の本文は op の戻り値を決めていない（元の案は EBUSY、B2 は DEFERRED）。WIP の gpu.h の comment「returns 0 with the outcome
   ... an error means the stream failed」を設計に写し、「op は EAGAIN・EBUSY を返さない（DEFERRED で返す）」「走らせる前の拒否（faulted・stopping の
   EIO、bytes の EINVAL）も FAULTED」を明記する。runtime_command の EAGAIN（runtime.c:501）をそのまま流用すると、core が errno の EAGAIN
   （capacity の待ち）と取り違える。
3. **FAULTED と「open は失われる」の約束**: COMMIT の本体は `cancel(fault)` が失敗すると session を失わせない（gpu.c:6128〜6132、`if (cancel_error == 0) lost = 1`）。
   WIP の `gpu_job_submit_finish` も同じ。uapi の comment は FAULTED で「the open is lost」と言うので、cancel の結果によらず
   `gpu_session_fail_locked` するか、comment を「backend が callback を手放せない時は期限で失われる」に直す。
4. **errno の表に EINTR と session の error が無い**: SUBMIT は admission の中なので `gpu_session_enter` の割り込み可能な待ち（gpu.c:1776〜1781）で
   EINTR、`gpu_session_error_locked` で EIO・ETIMEDOUT なども返る。表に足し、libvulkan は EINTR を再試行する（今の `vulkan_kernel_error` は EINTR を
   device lost にする、context.c:576〜601。今の GPU_COMMAND にもある既存の弱さ）。
5. **outcome の数え方の変数**: WIP は `owner->vk->queue_ran` を `runtime_execute` の外（exec_lock の外）で 0 にし、後で読む。同じ session の別の frame
   は admission と `queued == 0` の確かめで走らないので今は安全だが、その不変条件に依っている。runtime_execute の中（exec_lock の下）で数え、frame の
   局所の struct で返す形にするか、不変条件を comment に書く。
6. **RETIRE の kernel stack**: WIP は entries[64]（1 KiB）と taken[64]（512 byte、LP64）を stack に置き、registry lock を irqsave で持ったまま
   64 × 64 を走査する（1 回目の reviewer の minor「slot を 1 回だけ走査」と違う）。stack の小さい arch（m68k・ppc32・armv7）の kernel stack の大きさを
   確かめるか kern_malloc に。走査は slot を外側の loop にする。
7. **uapi の comment の文**（WIP）: 「Return 0: sequence names the job」は CANCELED・DEFERRED では偽（record は捨てた、WAIT は ENOENT）。
   EFAULT の後に libvulkan が ledger の notification をどうするか（sequence は先の copyout で分かっている）も設計に 1 行。
8. **期待の効果と効果の基準**: 137〜143 行は RDNA の数のまま。nvrtx の内訳（RESERVE 4 µs・COMMIT 2 µs・reap の WAIT 2 µs・monitor 約 3 µs、
   phase024/phase.md 71〜75 行）で書き直す。RTX の base の run の間の揺れは 17.04〜19.97 fps（phase024 の表の base の行）と大きいので、
   3 run ずつでは 10 % 前後の効果が揺れに埋もれうる。libvulkan の timing（1 submit の µs）の中央値も判定に足すか、run を増やす。
9. **記録の規則**: design 145 行にユーザーの発言の原文がある（AGENTS.md §8、2026-10-30 の決定で原文を載せない）。要点の形に直す。親の link が
   ws291-p006（RDNA、実機が戻るまで止め）のままで、今の作業（nvrtx の subs-1000）は p013・p014・p024 の続き。どの Phase の記録かを ws.md の表と
   合わせる。
10. **順序（process）**: design 5〜6 行は「design-reviewer の後に Q1 へ送ってから実装」だが、review の途中で実装の差分が同じ worktree にあった。
    review の指摘（特に BL1・M-A の uapi の field）で header と driver の comment が変わるので、commit の前に合わせる。

## 確かめて問題が無かった所

- **marker は frame の後ろ**: SUBMIT の commit は frame の後。nvrtx の inline commit（runtime.c:704〜759、`owner->queued == 0` の時）は
  `runtime_park`（runtime.c:1170〜）で node の lock の下で `session->run_fence` を取るので、直前の frame の run を必ず含む。
  queued が 0 でない時は queue の後ろに並ぶ（runtime.c:735）。frame と commit の間に他の thread が並べうるのは CANCEL_FAULT の marker だけで、
  それも後ろへずれるだけ。
- **他の thread との競合**: 同じ sequence の COMMIT・CANCEL は `job_action` の pin で EBUSY（gpu.c:6382〜6386）、SUBMIT の後は EALREADY か
  CANCEL_FAULT（COMMITTED に対して許される、gpu.c:6110〜6117）。WAIT・RETIRE が SUBMIT の中で完了した record を consume しても、SUBMIT の
  observer の pin が slot を保つ（gpu.c:6831〜6860）。
- **stop・close・monitor**: close は ioctl と並ばない。monitor の停止の action 1・2 は `session->busy`（admission）と `job_action` の間は選ばれない
  （gpu.c:8009〜8022）。action 3（期限切れの昇格）は stop が PENDING の後だけで、PENDING の session は `session->error` があるので
  `gpu_session_enter` が SUBMIT を入れない。`gpu_session_reclaim` も busy の間は走らない。
- **0 個の queue の操作 → CANCELED**: nvrtx で run を GPU に送るのは queue_submit（vk-queue.c:150 → vk-execute.c → `nvrtx_vk_gpu_submit`）だけ
  （grep で確かめた）。queue の操作の無い stream は今の GPU_COMMAND と同じく監督なしの道と等しく、後退ではない。
- **fence の無い submit に job を残す**: WS014 の決定と今の libvulkan（私用の fence、queue.c:349〜357）のまま。ledger の notification の数も今と同じ。
- **ABI**: `gpu_job_submit` 56 byte（u64 は offset 16・32・40）、`gpu_job_retire_entry` 16、`gpu_job_retire` 32。ILP32・m68k（u64 の 2 byte 整列）
  でも padding が無い。'G' 47・48 は空き、49・50 は HEAD で未使用（`_IO*('G', 4x/5x)` を include・src・userland で grep）。
- **driver の ops の表**: drv_gpu_job_ops を持つのは 8 driver（amdrdna・nvrtx・i915・venus・ghci・powervr・bcm2711・mali）。mali は designated
  （sched.c:100〜105）、他の 7 つは WIP で NULL を足している。設計の M3 の「7 driver」と改訂の試験の一覧（223 行）に ghci が無いので足す。
- **案 B の consume の規則**: `gpu_completion_observe` の timeout 0 と `observer_leave(consume, reserved)` の規則（gpu.c:6728〜6860）を複数に広げる
  形で、copyout の失敗は consume しない。libvulkan は ledger に job の record だけを置く（sync.c:701 だけが足す）ので、decoder の record が
  MISSING になって device lost になる事は無い。

## 推測・未確認

- M-B の実害の大きさ（nvrtx の frame がどれだけ長く止まりうるか）は、`nvrtx_vk_gpu_wait` の上限を読んでいないので推測。
- M-E の QEMU の Venus の道具（WS014）が今動くかは確かめていない。
- kernel の build の warning の flag（`-Wmissing-field-initializers` が vmunix で効くか）は確かめていない（WIP は全部の表に NULL を足しているので
  どちらでも build は通る見込み）。
- 走らせたのは WS014 の host fixture 2 本だけ（出力は scratchpad、repo の file は変えていない）。WIP を含む tree での build・試験はしていない。
