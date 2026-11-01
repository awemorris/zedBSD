<!-- awesome-plan project=zedbsd record=ws291-p026-design -->

# ws291-p026 設計: NVIDIA（nvrtx）で記録の時に push buffer まで組み、submit は ring に載せるだけにする

- 版: 第 1 版（2026-11-01 P2、q1832-i01）。敵対的レビューの後の版は末尾の「改訂の記録」。
- 親: [phase.md](phase.md)、[WS291](../ws.md)。前提の計測: [p024](../phase024/phase.md)（G1〜G4、NVK との段ごとの比べ、(a)(b)(c)、実機の A/B）。
- code は書かない。実機・QEMU は使っていない。この文書の数の「見積もり」は全部未計測。

## 0. 要旨

1. p024 の (b) は 2 つの仕組みの組で初めて subs-1000 に効く。
   - **B-K（kernel）**: nvrtx の executor が、短い command buffer の push buffer を `END_COMMAND_BUFFER` の時に、その command buffer が持つ GPU の
     memory（以下 granule）へ組んでおく。`QUEUE_SUBMIT` はその segment を ring に載せるだけ（1 回の ring の書き、all-or-nothing）。
   - **B-U（libvulkan）**: fence も semaphore も無い vkQueueSubmit（今の `queue_unfenced` の条件）を、その command buffer の記録と一緒に
     libvulkan の recording の thread に渡し、返事を待たずに返す（hand-off）。kernel の仕事（decode・組み立て・ring）は recording の thread の
     上で走り、アプリの thread は次の command buffer の記録へ進む。
2. **重なりの条件**: mview の subs は「記録 → End → submit → 次の記録」を繰り返す（`userland/tests/mview/renderer.c` の `mview_renderer_draw`）。
   submit が組み立ての終わりを待つ限り、組み立てを End の時に動かしても、別の thread で走らせても、アプリの thread の 1 part の時間は縮まない
   （§3.2）。p024 の見積もり（59 → 70 fps 台）が数えた重なりは、submit を待たずに返す B-U があって初めて生まれる。B-U は vkQueueSubmit の失敗の
   報告を遅らせる（§6.6）ので、ユーザーの判断が要る（phase.md の Q1）。
3. **big-1000 は (b) では縮まない**（inflight=1）。今も GPU は submit の時の組み立てと重なって走っている（`vk-execute.c` の `EXECUTE_FIRST_DRAWS`
   256 で最初の push buffer を早く出す）ので、組み立てを submit から外しても frame は約 record ＋ GPU のまま（§3.3）。長い command buffer を End
   で組むと、GPU の開始が組み立ての分だけ遅れて後退する。B-K は長い command buffer を End では組まない。
4. push buffer は kernel が組むまま（NVK のように user が書く形は採らない）: nvrtx の GPU の address space は全 session で 1 つ
   （`nvrtx-work.h`）で、user の push buffer は他の session と driver の page に届く（§8）。
5. HAL は変えない。`include/uapi/` も変えない。kernel の renderer と libvulkan の間の capset の native の flag に 1 bit を足す（P-C だけ、§10、
   phase.md の Q3）。

## 1. 範囲と読んだ物

- 読んだ source（worktree p2-ws291g1、main d4b741303）:
  - nvrtx: `src/drivers/gpu/nvrtx/vk/vk-execute.c`（executor、slot、push・constants・pool の区分、draw の差分、shadow、CPU copy）、`vk-queue.c`
    （queue_submit、fence、BUG-381 の gpu_sends）、`vk-command.c`（Begin・End・Reset・allocate、primary だけ）、`vk-record.c`（record の decode）、
    `nvrtx-vk.h`・`nvrtx-vk-objects.h`・`nvrtx-exec.h`、`vk-node.c`（gpu_alloc・gpu_alloc_system・gpu_free・gpu_submit）、`work.c`
    （`nvrtx_work_channel_submit` の ring の entry）、`nvrtx-work.h`（address space）、`runtime.c`（runtime_command・runtime_job_command・
    runtime_execute、exec_lock と node の lock、lane と worker）、`nvrtx-node.h`（session）、`stage.c`（起動の token）。
  - GPU core: `src/drivers/gpu/gpu.c` の `gpu_session_enter`（1 つの open は 1 本ずつ）。
  - libvulkan: `userland/desktop/libvulkan/commands.c`（End の保留 `end_pending`、recording の thread `command_sender`、`command_sent_wait`、
    `VULKAN_RECORDING_BATCH_BYTES` 256 KiB、短い記録は batch の 1/4 = 64 KiB まで submit と一緒）、`queue.c`（`queue_enqueue`・`queue_unfenced`・
    `queue_attempt`・`queue_submit_job`・`queue_write_pending`）、`context.c`（`vulkan_context_execute`・`vulkan_context_transaction`、capset の
    native の flag）、`internal.h`、`content.c`、`wsi-image.c`（lend の import）、`sync2.c`（vkQueueSubmit2 も `queue_enqueue` へ）。
  - 計測: `userland/tests/mview/renderer.c`・`main.c`（`--submits`・`--inflight` 既定 1）、`plan/ws291/tests/rtx-p024.sh`・`rtx-ab.list`。
- 読んでいない物: NVK・nouveau・drm_sched の source（この文書は形の比べだけで、数は p024 の NVK の計測）。実装で手順を参考にする時は
  guardrail の「Procedure referenced:」の規則で block ごとに書く（§15）。

## 2. 今の流れ（事実）

subs-1000 の 1 part（p024 i03 の ac の内訳。1 part は 1 command buffer、約 16 の draw と 16 の texture）:

1. アプリの thread: vkResetCommandBuffer（libvulkan の中だけ。kernel の Reset は次の Begin の頭に載る）、vkBeginCommandBuffer（同じく頭に載る）、
   vkCmd*（`recording` に encode、約 5.7 µs）、vkEndCommandBuffer（記録が 64 KiB 以下なので `end_pending`、kernel へは送らない）。
2. vkQueueSubmit: `queue_enqueue` → `queue_attempt`（queue と device の mutex）→ `queue_write_pending` で記録（RESET・BEGIN・records・END）を
   `QUEUE_SUBMIT` の前に同じ transaction へ → part 1〜998 は fence も semaphore も無いので `queue_unfenced` が真、GPU_COMMAND 1 本
   （part 0 は acquire を待ち、part 999 は fence と signal を持つので job の道）。
3. kernel（呼び手の thread、`runtime_command` → `runtime_execute`: session の `exec_lock` と node の `lock` を持つ）: stream の decode →
   `RESET_COMMAND_BUFFER`（record の object を離す、約 0.8 µs）→ BEGIN → record の decode（約 1.7 µs）→ END（状態だけ）→ `QUEUE_SUBMIT` →
   `nvrtx_vk_execute`: slot の続きに push buffer を書き（`execute_begin`: class の初期の state は channel が保つ時は省く、texture の pool の
   address、shader cache の invalidate）、record ごとに method、`execute_submit` → `nvrtx_vk_gpu_submit` → `nvrtx_work_channel_submit`
   （start stamp・segment・fence の 3 entry、USERD の put 1 回）。executor 約 6 µs（draw 16 本 3.3、ring 1.0）。
4. vkQueueSubmit 1 本 約 11 µs、record 約 5.7 µs、frame 17.1 ms（58.5 fps）。NVK は record 6.2・submit 4.65 µs（ring への書きは別の thread の kworker）。

submit の時にしか決まらない物（今の executor が submit の時に読む物）:

| 物 | 今どこで決まるか | 記録の時に決まるか |
| --- | --- | --- |
| push buffer の置き場所 | session の slot（3 × 2 MiB、`execute_start`・`execute_continues`）、前の submit の続き | 決められる（command buffer の granule） |
| texture header・sampler の pool の slot | slot ごとの pool（`EXECUTE_HEADERS_AT`・`EXECUTE_SAMPLERS_AT`）、push buffer ごとに番号 | 決められる（granule の中の pool） |
| 内部の quad の constant buffer | slot の constants（`execute_constants`） | 決められる（granule） |
| application の draw の constant buffer 0 | session の 1 つ（slot 0 の `EXECUTE_ROOT_AT`）へ push buffer の中の load で書く | 同じ（address は session の間変わらない） |
| class の初期の state | `class_ready`（channel が前の run の state を保つか、`nvrtx_vk_gpu_state_kept`） | 決まらない → submit の前置きの segment |
| image・buffer・view の GPU の address、texture header の中身 | bind 済みの memory（Vulkan では bind は 1 回で変わらない）、view の header の cache | 決められる |
| descriptor set の中身 | submit の時に set を読む | Vulkan の規則では bind した set の更新は command buffer を invalid にする。記録の時の写しを使い、submit で世代を確かめる（§5.6） |
| render pass の target・clear の値 | record と framebuffer | 決められる（WSI の image は image ごとの import で、lend でも VkImage の backing は変わらない: `wsi-image.c`） |
| shadow（pitch-linear の色と深度）と、前の pass の写しを省く `kept` | session の shadow と `nvrtx_vk_gpu_quiet` の token（GPU の状態） | 決まらない → この形の command buffer は組まない（fallback） |
| CPU の copy（`execute_cpu_copy` と `execute_drain`）・video の decode・query | GPU を待つ | 決まらない → fallback |
| stencil つき深度の page の kind（初めて target になる時 `nvrtx_vk_memory_kind`） | 初めての submit | その 1 回だけ fallback（次からは kinded） |

## 3. どこが律速か、(b) がどう効くか

### 3.1 G1 の中身

p024 の G1（submit の時に呼び手の thread で push buffer を組む、約 8 µs）は decode 1.7・RESET 0.8・executor 6（そのうち ring 1.0）。組み立て
そのものの費用は、どこで走らせても消えない（毎 frame 記録し直す subs では「組む回数」は減らない）。縮むのは「アプリの thread が待つ時間」だけ。

### 3.2 subs の 1 part の流れでの重なり（見積もり）

| 形 | アプリの thread の 1 part | 別の thread の 1 part | 1 part の律速 | subs-1000 の見積もり |
| --- | --- | --- | --- | --- |
| 今（ac） | record 5.7 ＋ submit 11（kernel の全部） | — | 約 17 µs | 58.5 fps（実測） |
| B-K だけ（End で組み、記録は今どおり submit と一緒） | 同じ（End と submit が同じ transaction） | — | 約 17 µs | 変わらない |
| B-K ＋ End を recording の thread で先に送る、submit は待つ | record 5.7 ＋ End の組み立てを待つ 約 9 ＋ ring の ioctl 約 3.5 | End の ioctl | 約 18 µs | 下がる（system call が 1 本増える） |
| **B-U**（End と submit を 1 つの transaction で recording の thread へ） | record 5.7 ＋ hand-off 約 1〜2 | 今の submit の transaction 約 9.5〜10.5 | 約 10 µs（別の thread） | 85〜95 fps が上限、実際は 75〜90 の見込み |
| B-U ＋ B-K | 同上 | 同上（組み立ては END、ring は QUEUE_SUBMIT、同じ transaction） | 約 10 µs | B-U と同じ |

- 同じ open の ioctl は GPU core が 1 本ずつに限り（`gpu_session_enter`）、executor は node の lock の下で走る（`runtime_execute`）。よって End を
  別の thread、submit をアプリの thread、と 2 本に分けても kernel の中では並ばない。重なりは「アプリの thread が kernel を待たない」事からしか
  生まれない。
- B-U の律速は recording の thread の 1 transaction（約 10 µs）。生産側（アプリ 約 7 µs）の方が速いので thread の queue は空にならず、起床の
  費用は最初の 1 回だけ（§6.4）。NVK（75.4 fps = 1 part 13.25 µs）を越える見込みだが未計測。
- B-K は subs の毎 frame の記録し直しでは効かない。効くのは (i) 同じ command buffer を記録し直さずに何度も submit する時（2 回目から ring だけ）、
  (ii) 複数の command buffer を記録してから 1 回の submit で出す時に End を先に送る形（P-C、組み立てがアプリの記録と重なる）、(iii) B-U の
  非同期の部分が ring だけになり、そこでの失敗が GPU を失った時だけに近づく事（§6.6）。

### 3.3 big-1000（inflight=1）

- p024 i02 d1・d2: frame は record 3.7〜3.9・submit 3.3〜3.5・fence 1.1〜1.2 ms、GPU は submit の始めから約 4.5 ms。executor は 256 draw で最初の
  push buffer を GPU に出し、その後 2048 draw ごとに出す。組み立て（1 draw 約 0.14〜0.17 µs）は GPU（約 0.22 µs）より速いので、GPU は submit の
  始めから止まらずに走り、submit の段は GPU の時間に隠れている。
- 組み立てを submit から外しても、GPU は vkQueueSubmit より前に走れない（Vulkan の意味）ので frame は約 record ＋ GPU ＋ 数十 µs のまま。
  長い記録を End で組む（recording の thread で decode と組み立て）と、thread の 1 MiB あたりの仕事は decode 約 0.75 ms ＋ 組み立て約 0.95 ms
  （3.3 ms ÷ 約 3.5 MiB）で client の encode（約 0.75 ms/MiB）より遅く、End が待ち、GPU の開始が遅れる: frame が数 ms 後退する。
- よって B-K は長い command buffer（record の数が上限を越える物）を End で組まない。big-1000 の受け入れは「後退なし」。NVK の 136 fps との差
  （client の encode 3.55 対 3.2〜3.4 ms、GPU の時間）はこの設計の外（phase.md の「範囲の外」）。

## 4. 全体の形

```
アプリの thread                         recording の thread（libvulkan、process に 1 つ）     kernel（nvrtx）
vkCmd* → recording に encode
vkEndCommandBuffer → end_pending
vkQueueSubmit（fence・semaphore 無し）
  → transaction を組んで hand-off ───▶  transaction を送る（GPU_COMMAND） ──────────▶  RESET・BEGIN・records の decode
  ← VK_SUCCESS（返事を待たない）                                                       END: 短ければ granule に組む（B-K）
次の vkCmd* …                                                                          QUEUE_SUBMIT: segment を ring へ（B-K）
                                         返事を読む（失敗は context に latch）           （組んでいない物は今の executor）
vkQueueSubmit（fence あり）
  → hand-off の残りを待つ（drain） → 今の道（GPU_JOB_SUBMIT）
```

- B-U と B-K は独立に切り替えられる（§14）。B-U だけでも subs の伸びは出る。B-K だけでは subs は伸びない。
- P-C（任意）: B-K がある時、End の記録を submit を待たずに recording の thread で先に送る（eager End）。

## 5. B-K: kernel の組み置き（prebuilt）の push buffer

### 5.1 責務と境界

- nvrtx の Vulkan の executor（`src/drivers/gpu/nvrtx/vk/`）の中だけ。GPU core（`gpu.c`）・uapi・libvulkan の wire は変えない。
- 組み置き（以下 prebuilt）を作るのは `END_COMMAND_BUFFER` の handler（`command_end`）、使うのは `QUEUE_SUBMIT`（`queue_submit`）、捨てるのは
  Begin・Reset・Free・pool の Reset と Destroy・session の close と、submit の時の世代の食い違い。
- 今の executor（submit の時に組む）は残し、組めない command buffer と `nvrtx.vk.prebuild` が切りの時はそれを使う（fallback）。

### 5.2 内部の interface（案、名前は実装で決める）

```c
/* A run of executor memory a prebuilt push buffer is written into (vk-execute.c). */
struct nvrtx_vk_granule {
	struct nvrtx_vk_granule *next;      /* the session's free list, or the command buffer's list */
	uint8_t *cpu;
	uint64_t va;
	struct nvrtx_vk_ticket ticket;      /* the newest run that read it */
};

/* One segment of a prebuilt push buffer. */
struct nvrtx_vk_segment {
	uint64_t va;
	uint32_t bytes;
};

/* What End wrote for a command buffer (nvrtx-vk-objects.h, in struct nvrtx_vk_command_buffer). */
struct nvrtx_vk_prebuilt {
	bool built;
	struct nvrtx_vk_granule *granules;
	struct nvrtx_vk_segment segments[NVRTX_VK_PREBUILT_SEGMENTS];   /* 16 at first */
	uint32_t segment_count;
	uint32_t draws, quads;
	uint64_t set_epoch;                                               /* the session's when built */
	struct { const struct nvrtx_vk_descriptor_set *set; uint64_t generation; } sets[NVRTX_VK_PREBUILT_SETS]; /* 16 */
	uint32_t set_count;
	bool refused;                                                     /* not to be tried again this recording */
};
```

- work.c に複数の segment を 1 つの stamp と 1 つの fence で載せる口（nvrtx の内部）:
  `int nvrtx_work_channel_submit_list(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel, const struct nvrtx_work_segment *segments, uint32_t count, uint32_t *fence);`
  今の `nvrtx_work_channel_submit` は count 1 の包み。entry の数（stamp ＋ count ＋ timed の end ＋ fence）の空きを先に全部待ち、全部を書いてから
  put を 1 回動かす（今と同じ all-or-nothing）。count の上限 `NVRTX_WORK_LIST_MAX`（32）。
- vk-node.c に `nvrtx_vk_gpu_submit_list(void *owner, const struct nvrtx_vk_segment *segments, uint32_t count, struct nvrtx_vk_ticket *ticket)`
  （今の `nvrtx_vk_gpu_submit` と同じ確かめ・数え方・quiet の epoch・`gpu_sends` の 1 増し）。
- executor の内部の「書き先」（sink）: 今の `nvrtx_vk_execute` の slot の cursor（cpu・va・segment・at・constants・headers・samplers・
  draws・quads・started・early）を `struct execute_sink` にまとめ、mode を GPU（今: 満ちたら ring に出し、slot を待つ）と BUILD（満ちたら
  segment を閉じて次の granule、GPU には出さない、GPU を待つ処理は fallback を返す）にする。`execute_record` 以下の method の書き方
  （`execute_draw` の差分、`execute_textures` の pool、`execute_begin_pass`）は同じ code を通す（同じ method の列になる事を host 試験で見る）。

### 5.3 granule と memory の持ち方

- granule は 16 KiB（初期値、実装で調整）: push 10 KiB、header の pool 32 entry（1 KiB）、sampler の pool 32 entry（1 KiB）、内部の quad の
  constants 4 KiB（3 quad）。どれかが満ちたら segment を閉じ、次の granule の始めで pool の address と invalidate を書き直す（今の `execute_begin`
  と同じ method）。subs の 1 part（約 16 draw・16 texture）は約 3〜4 KiB の push で 1 granule。
- granule は session の arena から取る: `nvrtx_vk_gpu_alloc_system`（今の slot と同じ、CPU が cache を通して書く coherent な system memory を GPU の
  VA に写す）で 2 MiB の run を取り、128 個の granule に切る。free list は ticket 順。取る時は先頭の ticket が `nvrtx_vk_gpu_passed` なら使い、
  そうでなければ新しい run（session の上限 32 MiB = 16 run の内）、上限なら組まない（fallback）。GPU を待たない。
- command buffer の Begin・Reset・Free・pool の Reset と Destroy では、granule を command buffer の最後の run の ticket（`prebuilt` の granule の
  ticket は submit ごとに更新）のまま free list へ返す（`nvrtx_vk_gpu_free` は GPU の idle を待つので使わない）。run（2 MiB）を返すのは session の
  close の時だけで、今の `nvrtx_vk_execute_close` と同じ（node は session の run の終わりを待ち、終わらない channel の memory は返さない）。
- subs-1000（inflight 1）: 1000 × 16 KiB = 16 MiB。次の frame は fence を待った後に記録し直すので、前の granule は passed で再利用（定常 16 MiB）。
- record の配列は残す（object の参照を持ち、fallback と組み直しに要る）。object（pipeline の code・image の memory・buffer）の寿命は今どおり
  record の参照と、node の「session の run が終わるまで返さない」（`nvrtx_vk_memory_detach`・`nvrtx_vk_gpu_free`）で守られる。

### 5.4 組める command buffer（適格）と fallback

End の時に次を全部満たす時だけ組む（どれかに当たれば `refused`、submit は今の executor）:

1. `nvrtx.vk.prebuild` が入（§14）、command buffer の状態が EXECUTABLE（記録の失敗なし）。
2. record の数が `NVRTX_VK_PREBUILD_RECORDS_MAX`（2048、初期値）以下（長い記録は組まない、§3.3）。
3. 組む途中で BUILD の sink が次に当たらない: CPU の copy（`execute_cpu_copy` へ行く copy の形）・`execute_drain`・shadow（pitch-linear の色と
   深度、`execute_shadow`）・video と query（`nvrtx_vk_video_run`）・kinded でない stencil の深度・segment の数の上限・granule が取れない。
   当たったら、その build の granule をすぐ free list に返し（GPU に一度も出していないので ticket 0）、record は変えずに `refused`。
4. 使った descriptor set の数が `NVRTX_VK_PREBUILT_SETS`（16）以下。

End の返事は組めても組めなくても今と同じ（組み置きの失敗は command buffer の失敗にしない）。

### 5.5 submit（`queue_submit`）

1. vkQueueSubmit の command buffer を順に見て、prebuilt があり世代が合う（§5.6）物が続く間、その segment を 1 つの list に集める。
2. list を出す前に、session の channel が class の state を保っていなければ（`class_ready` が偽: 最初の run、共有の work channel）、今の slot に
   class の初期の state だけの短い push buffer を書いて list の先頭に足す（前置き）。channel が保つなら何も足さない。
3. `nvrtx_vk_gpu_submit_list` で 1 回で出す。ticket を各 command buffer の granule と slot に写す（slot は前置きを書いた時だけ）。
   `class_ready = nvrtx_vk_gpu_state_kept`、`execute->kept.valid = false`（prebuilt の pass が shadow の写しの元を書きうる）、
   `nvrtx_vk_gpu_drawn` に draws・quads、`queue_ran`・`queue_accepted` は今どおり。
4. 世代が合わない・prebuilt が無い command buffer に来たら、集めた list を先に出し、その command buffer は今の `nvrtx_vk_execute`。
5. fence は今どおり session の最新の run（`nvrtx_vk_gpu_last`）。job の marker も今どおり。

- 1 つの prebuilt の segment の列は、今の slot の続きの push buffer と同じく GPU の前の状態に頼らない: 各 segment の始めに pool・invalidate、
  最初の draw は全部の state（`held` は segment の始めで無効、今の `reuse.pools++` と同じ）、constant buffer 0 は最初の draw で全部 load。
  application の constant buffer 0（slot 0 の `EXECUTE_ROOT_AT`）は push buffer の中の load でしか書かれないので、順に走る限り他の command
  buffer と競合しない（今と同じ）。
- patch（submit の時に prebuilt の中を書き換える）は使わない（§11 の R3）。prebuilt の中身は submit で変えないので、同じ command buffer を
  同時に何回 ring に載せてもよい（SIMULTANEOUS_USE、間違った二重の submit も壊れない）。

### 5.6 descriptor set の世代

- `struct nvrtx_vk_descriptor_set` に `generation`、session に `set_epoch` を足し、set を書く所（`vk-layout.c` の `layout_update_write`・copy・
  pool の Reset と Free）で両方を増やす。
- build は使った set とその世代を `prebuilt.sets` に残し、`set_epoch` を写す。submit は session の `set_epoch` が同じなら確かめ無しで使い、違えば
  残した set の世代を 1 つずつ比べる。違う set があれば prebuilt を捨て（granule を ticket 付きで返す）、今の executor で走らせる。
- Vulkan では bind した set を更新すると command buffer は invalid になる（update-after-bind は nvrtx が報告していない）ので、ここは正しい
  アプリでは起きない。起きても今と同じ結果（submit の時の set で描く）になるための網。
- image・view・sampler・pipeline・buffer は作った後に変わらない（bind は 1 回、view の header は image の address で cache 済み）ので世代は要らない。

### 5.7 状態の遷移（command buffer の prebuilt）

| 今 | 事 | 次 |
| --- | --- | --- |
| なし | END（適格） | built |
| なし | END（不適格） | なし（refused、この記録の間は試さない） |
| built | QUEUE_SUBMIT、世代が合う | built（granule の ticket を更新） |
| built | QUEUE_SUBMIT、世代が違う | なし（granule を返す、今の executor で走る） |
| built・なし | Begin・Reset・Free・pool の Reset と Destroy | なし（granule を ticket 付きで返す） |
| built | session の close | なし（run ごと返す、今の close の規則） |

secondary の command buffer: nvrtx は今も primary だけ（`command_allocate` が ENOTSUP）。変えない。

### 5.8 並行性と lock

- build は `command_end` の中で走る: `runtime_execute` が持つ session の `exec_lock` と node の `lock` の下（今の submit の組み立てと同じ）。
  granule の free list と arena は session の executor の物で `exec_lock` が守る。ticket の比べは `nvrtx_vk_gpu_passed`（channel の fence の
  lock の無い読み、今と同じ）。
- ring は `nvrtx_work_channel_submit_list` が channel の lock の下で書く（今と同じ）。
- 新しい thread・割り込み・waitq は足さない。GPU が granule を読むのは DMA（今の slot と同じ coherent な system memory）。CPU の書きは ring の
  publish の前に終わり、今の `work_ring_publish` の順序（write barrier、USERD の put）に乗る（実装で barrier の位置を確かめる）。
- 改善の余地（この設計の外）: build を node の lock の外（`exec_lock` だけ）で走らせれば、他の session（compositor）が待たされない。
  node の lock は alloc・submit の所だけに要る。P-D の候補。

### 5.9 失敗と回復

- OOM（granule・run の確保）: build では fallback（End は成功）。submit では確保しない（前置きの slot の push は今の slot の道で、確保と待ちは
  ring に出す前）。
- prebuilt だけの vkQueueSubmit は 1 回の ring の書きなので、失敗は GPU に何も出す前（EIO = session が lost、ENODEV、ring の空きの 2 s の
  時間切れ ETIMEDOUT）だけ。BUG-381 の「一部を GPU に出した後の OOM」はこの道では起きない。prebuilt と今の道が混じる vkQueueSubmit は今の
  `gpu_sends` の規則（出した後の失敗は EIO）のまま。
- GPU の fault・時間切れ: 今の recovery（reaper が channel を止め、session を lost に）。lost の session の submit は EIO。granule は session の
  close で今の slot と同じ規則で返す（止まらない channel の memory は device が持ったまま）。
- job の CANCEL: `runtime_job_command` の outcome（queue_ran・queue_accepted・gpu_sends）は変えない。prebuilt の submit も `gpu_sends` を 1 増す。

## 6. B-U: fence の無い submit の hand-off（libvulkan）

### 6.1 条件

`queue_enqueue` で次を全部満たす時だけ: `queue_unfenced` が真（public、fence・semaphore・pNext 無し、WSI の lock を持たない、
`GPU_CAP_JOB_SUBMIT` と同期の command の kernel = 今は nvrtx だけ）、hand-off が入（環境変数 `VULKAN_ZEDBSD_SUBMIT_HANDOFF` が 0 でない、
`sysconf(_SC_NPROCESSORS_ONLN)` が 2 以上、recording の thread が動いている）、context に失敗が latch されていない。

### 6.2 流れ

1. アプリの thread（queue と device の mutex の下、今の `queue_attempt` の前半と同じ encode）: `queue_write_pending` の代わりに、各 command buffer の
   保留の記録（`end_pending`）を submission の場所（新しい `struct vulkan_submit_handoff`、queue ごとに 64 個の輪）の writer へ写すか storage を
   移し、`end_pending` をすぐ偽に（同じ command buffer の後の submit は記録を二度送らない）、頭と End の返事の数と opcode を場所に写す。続けて
   `QUEUE_SUBMIT` を encode。`vulkan_content_commit` をここで行う。各 command buffer の `sent_pending` を 1 増す（Begin・Reset・End・Free は
   今の `command_sent_wait` で送り終わりを待つ）。context の `handoff_pending` を 1 増し、recording の thread の queue の最後へ。VK_SUCCESS を返す。
2. recording の thread: 今の batch と同じ FIFO で取り、`vulkan_context_execute` で送り、返事を読む（記録の返事 → command buffer の
   `sent_error`、submit の返事）。失敗は §6.6。`sent_pending` と `handoff_pending` を減らし、broadcast。
3. 続きの 2 つの場所が同じ context の物なら、batch の上限（256 KiB）まで 1 つの transaction にまとめてよい（system call を減らす。実装で測って決める）。

### 6.3 順序（drain）

- 同じ context の同期の transaction は、その context の hand-off が全部送られてから: `vulkan_context_execute` の頭（context の mutex を取る前）と、
  `sync.c`・`context.c` の直の ioctl（GPU_JOB_RESERVE・CAPACITY・ACTION・RETIRE・COMMAND_WAIT・GPU_JOB_SUBMIT）の前で `handoff_pending` が 0 に
  なるのを待つ（recording の thread 自身の呼び出しは待たない: thread の印）。これで kernel が見る wire の順は今と同じ（fence ありの submit・
  WaitIdle・present・object の作り壊し・descriptor の更新は、前の hand-off の後に kernel に届く）。
- 記録の batch（長い記録）と hand-off は同じ FIFO なので互いの順は保たれる。
- vkDestroyDevice・context の close は drain してから。

### 6.4 lock の順と待ち

- アプリの thread: queue の mutex → device の mutex → command_sender_lock。drain の待ちは context の mutex を持たずに（device・queue の mutex は
  持っていてよい）。
- recording の thread は command_sender_lock と context の mutex（`vulkan_context_execute` の中）だけを取り、queue・device の mutex を取らない。
  device の失敗の印は atomic（`vulkan_context_error` の latch と同じ形）で付け、device の mutex を要る処理（`vulkan_sync_device_error`）は次の
  同期の呼び出しが拾う。これで drain の待ちと thread の間に輪ができない。
- back-pressure: queue の 64 個の場所が全部使用中ならアプリの thread は待つ（半分空くまで、起床を減らす）。thread は queue が空の時だけ眠り、
  アプリは thread が眠っている時だけ起こす（今の `command_sender_ready` の signal を、眠っている時だけに）。

### 6.5 1 CPU・他の driver・Venus

- 1 CPU では hand-off は切り（費用だけ増える）。
- `GPU_CAP_JOB_SUBMIT` は backend の job の表に `command` がある時だけ gpu.c が立て、それは nvrtx だけ（p024 i03 の (c) の調べ）。amdrdna・i915・
  powervr・bcm2711・venus・mali・ghci の道は変わらない。Venus の回帰は libvulkan の変更として流す（§12）。

### 6.6 失敗の意味（ユーザーの判断が要る点）

- 今: fence の無い submit の失敗は、確かな拒否（OOM）の時は vkQueueSubmit が OOM を返し、他は DEVICE_LOST（p024 i03 の (c)）。End の失敗
  （renderer だけが見る記録の失敗）は vkQueueSubmit が返す。
- B-U: vkQueueSubmit は kernel の返事の前に VK_SUCCESS を返す。後で失敗が分かったら、その仕事は走らず、後の仕事はそれに頼るので、context を
  DEVICE_LOST に latch し、次の呼び出しから VK_ERROR_DEVICE_LOST。**OOM も DEVICE_LOST になる**（Vulkan は DEVICE_LOST をいつ返してもよいが、
  アプリが OOM から立ち直る機会は無くなる）。
- B-K があると非同期の部分の kernel の仕事は ring だけ（組み立ては END で、組めない時の fallback だけが submit の時に確保する）なので、
  非同期の失敗は実際には GPU を失った時と fallback の command buffer の OOM だけ。
- 選択肢は phase.md の Q1。

## 7. P-C（任意）: End を先に送る（eager End）と引き戻し

- B-K がある kernel（capset の native の flag `PREBUILD`、§10）の時、短い primary の vkEndCommandBuffer は記録（End 込み）を recording の thread に
  渡して返る（`end_pending` にしない）。kernel は END で組む。アプリが次の command buffer を記録する間に組み立てが走る。
- 同期の vkQueueSubmit（fence あり、または B-U が切り）がその command buffer を名指した時、その End がまだ thread の queue にあり送り始めて
  いなければ queue から引き戻し、今どおり submit の transaction に入れる（system call を増やさない）。送り始めていれば `command_sent_wait`。
- 効く場面: 複数の command buffer を記録してから 1 回 submit するアプリ（compositor でない一般の game・engine）。mview に `--submit-all`
  （全 part を記録してから 1 回の vkQueueSubmit）を足して測る（§12）。compositor（1 frame 1 command buffer、fence あり）は引き戻しで今と同じ。

## 8. security の境界

- push buffer は kernel が組む（今と同じ）。prebuilt は kernel の memory（`nvrtx_vk_gpu_alloc_system`）で、user の address space に写さない。
  user が渡すのは今と同じ wire（record）で、object は session の表で引き、address は kernel が bind 済みの object から作る。
- user が push buffer を書く形（NVK の形）を採らない理由: nvrtx の GPU の address space は 1 つで、全 open の blob と driver の page（ring・
  semaphore・fence の push buffer・golden context の buffer）が同じ空間にある（`nvrtx-work.h`、`vk-node.c` の map）。method（semaphore の
  release、copy、compute の launch など）は任意の VA を指せるので、GPU の VM は session を隔離しない。NVIDIA の method の流れを kernel が検証
  するのは現実的でない（Linux の nouveau・NVIDIA の driver も process ごとの VM と channel で隔離し、method を検証しない）。加えて、GPU に
  固有の事は kernel の driver に置き libvulkan は共通に保つ構成（guardrail の「kernel と libc」の節、SPIR-V の compile は kernel の driver）とも合わない。
- 既にある危険（この設計で増えない種類）: 共有の VA のため、範囲外に書ける shader があれば他の session の memory や slot・prebuilt を壊しうる。
  prebuilt は slot より長く残るので、壊れた時に効く時間は長い。session ごとの VA はこの設計の外（Future Work の候補として phase.md に）。

## 9. 他の driver と libvulkan への影響

- B-K は nvrtx の中だけ。他の driver は変わらない。
- B-U は libvulkan の `queue.c`・`commands.c`・`context.c`・`sync.c`（drain）と新しい file（hand-off の queue、host 試験のために transport を
  callback で受ける形）。条件が `GPU_CAP_JOB_SUBMIT`（nvrtx だけ）なので他の driver の道は変わらないが、drain の確かめは全部の context に入る
  （`handoff_pending` が 0 の時は atomic の読み 1 回）。
- P-C の eager End は native の flag が立つ時だけ。

## 10. HAL と UAPI を変えない根拠

- HAL（`include/hal/hal.h`）: 触る所が無い。memory は nvrtx の既存の node の口（`nvrtx_vk_gpu_alloc_system`、`nvrtx_work_map_system`）、時刻は
  `nvrtx_now_us`、thread は libvulkan の pthread。driver から HAL を呼ばない規則にも触れない。
- `include/uapi/`: 変えない。wire の opcode（`gpu-op.h`）は同じ物を同じ順で送る。GPU_COMMAND・GPU_JOB_SUBMIT の ioctl も同じ。
- capset の native の flag（P-C だけ）: `runtime.c` の `RUNTIME_NATIVE_*` と libvulkan `context.c` の `VULKAN_NATIVE_CAPSET_*` が同じ値を持つ
  renderer の取り決めで、`include/uapi/` には無い（今 bit 0 が H.264、bit 1 が同期の command）。bit 2 `PREBUILD` を足す。ABI に近いので
  phase.md の Q3 で確かめる。flag を使わない代わりの案は環境変数だけで eager End を入れる形（既定は切り）。

## 11. 選ばなかった案

- R1 user（libvulkan）が NVIDIA の push buffer を vkCmd* の時に書く（NVK の形）: §8。security の境界と構成の規則に反する。
- R2 B-K だけ（End で組む、submit は待つ）: §3.2 のとおり subs の frame は縮まず、End を別の transaction にすれば system call が 1 本増えて下がる。
- R3 patch の表（prebuilt を submit の時に少し書き換える）: 書き換えには同じ command buffer の前の run の終わりを待つ（GPU が読んでいる memory を
  CPU が書けない）か二重に持つ要があり、submit の時の CPU の仕事を戻してしまう。submit の時にしか決まらない物は前置きの segment（class の
  state）か fallback（shadow・CPU copy・video）で足りる（§2 の表）。
- R4 kernel の中で非同期にする（GPU_COMMAND は decode して queue に積んで返り、lane の worker が組み立てと ring）: アプリの thread の 1 part は
  record ＋ copy・decode（約 4 µs）で B-U と同程度の上限だが、失敗の意味は B-U と同じく遅れ、job の marker の順（marker は frame の後に完了）を
  worker の ring の後に合わせる kernel の変更が要る。B-U は kernel を変えずに同じ形を作れる。
- R5 長い command buffer も End で組む: §3.3、frame が後退する。
- R6 1 回の submit で組んだ物を毎回取っておく（retain、記録の時でなく初めての submit で組んで保つ）を長い command buffer に: 記録し直さない
  アプリでだけ効く。mview の bench では効かないので P-D（任意）に回す。
- R7 recording の thread を context ごと・queue ごとに増やす: 同じ open の ioctl は 1 本ずつなので並ばない。

## 12. 試験の計画

host（担当が流す、各 Phase の受け入れの前提）:

- H1 `plan/tools/nvrtx/nvrtx-vk-d3-host-test.sh` の拡張（P-B）: libvulkan が encode した stream を本物の dispatcher に通し、偽の node が ring に
  載った segment の bytes を写す。prebuild の入と切で同じ stream（mview の subs の 1 part 相当、clear と transfer の quad、uniform buffer、
  複数の pass、複数の command buffer の 1 回の submit、同じ command buffer の 2 回の submit）を流し、method の列が address（pool・segment の
  分け目）を除いて同じ事。fallback（CPU copy、shadow、video の stub、kinded でない stencil、record の上限、granule の確保の失敗の注入、
  End と submit の間の set の更新で世代が違う、segment の上限）がどれも切と同じ列。Reset の後の granule が ticket の passed の前に使われない
  事（偽の fence）。
- H2 `plan/ws291/tests/run-gpu-job-host.sh` PASS（GPU core は変えないが job の outcome の確かめに）。
- H3 libvulkan の hand-off の queue の host 試験（P-A、新しい）: transport を偽の callback にして、順序（同期の transaction が前の hand-off の
  後）、drain、back-pressure、失敗の latch、`command_sent_wait`、引き戻し（P-C）を plain・ASan/UBSan・TSan で。
- build: config/ci の 19 の `$(BUILD)/vmunix` と RTX の config の vmunix が warning 0、libvulkan を amd64・rpi4・visionfive2 で warning 0。

QEMU:

- Venus の回帰（libvulkan を変える P-A・P-C）: `plan/ws291/tests/venus-reg-rtx.sh`（10.0.10.3 の Debian、p024 i03 で PASS した道）か
  `venus-reg.sh`。B-U は Venus では入らない（cap が無い）事も確かめる。nvrtx は QEMU に無いので B-K の QEMU 試験は無い。

実機（10.0.10.3 の RTX 2070、Q1 の割当と lock の中、担当が 1 本の script で）:

- `plan/ws291/tests/rtx-p024.sh` ROUNDS=2 SHOT=1 で main と各 Phase の腕を同じ window で: big-100・big-1000・subs-100・subs-1000・scene-1500・
  scene-5000 の fps と still の画素の比べ（31 の比べが max 0）。腕ごとの kernel と libvulkan は rtx-p024.sh の 3 つ目の field。
- `plan/ws309/tests/rtx-p015.sh` PARTS='cycle scenes desk'（game mode の往復 3/3、desktop・Notes・browser の画面、frame-ops）。
- P-B・P-C の新しい bench（mview に足す。実装の Phase の範囲）: `--record=once`（part を最初の frame だけ記録し、後は submit し直す。
  SIMULTANEOUS_USE は使わず fence の後に）、`--submit-all`（全 part を記録してから 1 回の fence つきの vkQueueSubmit）。NVK の基準も同じ mode で
  （Debian の ~/linux-mview を同じ source から build、Q1 の割当）。
- TIMING=1 の段（vkQueueSubmit・record）と、`nvrtx.vk.perf=1` の runs・push_bytes で、hand-off と prebuilt が実際に通った数を確かめる
  （libvulkan の timing に hand-off の数、nvrtx の perf の行に prebuilt の submit・fallback・世代の食い違いの数を足す）。
- console・serial の log で判定しない。数は mview の出力と perf の行（ssh で読む）、画面は keiland-shot の PNG。

## 13. Phase の分け方（案）と受け入れ

順は phase.md の Q2 で決める。推しは P-A → P-B → P-C。

| Phase（案） | 中身 | 受け入れ |
| --- | --- | --- |
| P-A（libvulkan、約 1〜1.5 日） | B-U: hand-off の queue（新しい file）、`queue_enqueue` の分かれ道、drain、back-pressure、失敗の latch、環境変数の切り、timing の数 | H3 PASS。実機の同じ window の 2 round で **subs-1000 ≥ 75.4 fps（NVK）**、subs-100 が main 比 +20 % 以上。big-100・big-1000・scene-1500・scene-5000 が main の 2 round の幅の内か上（−3 % まで）。画素の比べ 31 が max 0。rtx-p015.sh の往復 3/3、frame-ops の各段が main −2 fps の内。Venus の回帰 PASS。`VULKAN_ZEDBSD_SUBMIT_HANDOFF=0` で main と同じ fps の幅 |
| P-B（nvrtx、約 2〜3 日） | B-K: sink の分け、granule と arena、END の build と適格と fallback、`nvrtx_work_channel_submit_list`、submit の list、前置き、set の世代、perf の数、`nvrtx.vk.prebuild=1`（入れる token、既定は切） | H1・H2 PASS、build warning 0。実機（どの腕も P-A の libvulkan）: `--record=once` の subs-1000 で prebuild 入の fps が切の 1.5 倍以上（見積もり: 切は submit ごとに組み直して recording の thread の 1 part 約 9 µs、入は ring だけで約 4〜5 µs）。prebuild 入と切で 6 bench の fps が幅の内（subs は P-A と同じか上）、画素の比べ max 0、rtx-p015.sh の往復・画面・frame-ops の後退なし。perf の数で subs の part の 99 % 以上が prebuilt で出た事 |
| P-C（libvulkan、約 1 日、任意） | eager End と引き戻し、native の flag `PREBUILD`（Q3 の答え次第） | H3（引き戻し）PASS。`--submit-all` の subs-1000 で、eager End 入が切より frame −15 % 以上。compositor の frame-ops の後退なし、Venus PASS |
| P-D（任意、この設計の外の候補） | build を node の lock の外へ、長い command buffer の retain（R6） | 別に設計 |

- どの Phase も既定の切り替え（§14）を残したまま入れ、受け入れの後に Q1 がユーザーに既定を入にするかを聞く（P-B の `nvrtx.vk.prebuild`）。
- WS291 の最後の「規約の全文の見直し」に、この Phase が変えた C の file を足す。

## 14. 危険と戻し方

| 危険 | 見つけ方 | 戻し方 |
| --- | --- | --- |
| hand-off の順序の誤り（同期の呼び出しが前の hand-off を追い越す） | H3 の TSan と順序の試験、実機の画素の比べ、Venus | `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0`（環境変数）、P-A の commit の revert |
| hand-off の deadlock | H3、実機の SPIN の時間切れ | 同上 |
| 失敗が DEVICE_LOST にまとまり原因が見えない | thread の失敗の行（`VULKAN` の stderr の 1 行、kernel の返事の値） | 同上 |
| prebuilt の method の列の違い（画素の差） | H1、実機の画素の比べ | `nvrtx.vk.prebuild` を入れない（既定は切）、P-B の revert |
| granule の早い再利用（GPU が読んでいる間に CPU が書く） | H1 の偽の fence、実機の長い run | 同上 |
| prebuilt の memory の増え（多数の command buffer を持つアプリ） | perf の行の arena の大きさ | session の上限 32 MiB で fallback |
| big の後退（長い記録を組んでしまう） | 6 bench の A/B | record の上限、`nvrtx.vk.prebuild` 切 |
| 1 CPU や負荷の高い機械で hand-off が遅い | sysconf の判定、A/B | 環境変数 |

## 15. 出典と引用

- この文書の事実は zedBSD の tree（Zlib）と p024 の実機の計測。外の source の code・comment・構造は写していない。
- 比べに名を出した物（読んでいない、数は p024 の計測）: Mesa NVK（MIT、`src/nouveau/vulkan/`、vkCmd* が push buffer を直に書く形）、Linux の
  drm_sched と nouveau（GPL-2.0、ring を別の thread で書く形）、NVIDIA open-gpu-kernel-modules（MIT、process ごとの VM）。実装でこれらの
  手順や値を参考にする時は、guardrail の「MIT・GPL の出典のデータの引用」の規則（該当の block の直前に複数行の comment で「Procedure
  referenced: <版> <path> (<関数>)」「Value(s) referenced: …」、file の先頭に書かない、code・comment・構造・symbol の名を写さない）に従う。
- 実装で値として要る物は今の tree に既にある（GPFIFO の entry の形は `work.c` が open-gpu-doc `clc46f.h` を引用済み）。新しい外の値は見込んでいない。

## 改訂の記録

- 第 1 版（2026-11-01 P2）。
