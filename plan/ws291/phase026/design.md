<!-- awesome-plan project=zedbsd record=ws291-p026-design -->

# ws291-p026 設計: NVIDIA（nvrtx）で記録の時に push buffer まで組み、submit は ring に載せるだけにする

- 版: 第 3 版（2026-11-01 P1、q1838-i01）。第 1 版（P2、q1832-i01）と第 2 版への敵対的レビュー 2 回（[design-review.md](design-review.md)）の指摘を
  反映した。指摘ごとの反映は design-review.md の表、版の差は末尾の「改訂の記録」。
- 親: [phase.md](phase.md)、[WS291](../ws.md)。前提の計測: [p024](../phase024/phase.md)（G1〜G4、NVK との段ごとの比べ、(a)(b)(c)、実機の A/B）。
- code は書かない。QEMU は使っていない。実機は §3.2 の GPU の 1 part の計測を 1 本だけ（10.0.10.3、診断）。他の「見積もり」は未計測（根拠の計測を名指す）。

## 0. 要旨

1. p024 の (b) は 2 つの仕組みの組で初めて subs-1000 に効く。
   - **B-K（kernel、nvrtx）**: 短い primary の command buffer の push buffer を `END_COMMAND_BUFFER` の時に組んでおく（組み置き、prebuilt）。
     `QUEUE_SUBMIT` は組み置きの segment を ring に載せるだけ。texture の header と sampler は session に 1 つの表（heap）に object ごとに 1 回書き、
     組み置きはその index だけを持つ（§5.3）。
   - **B-U（libvulkan）**: fence も semaphore も無い vkQueueSubmit（今の `queue_unfenced` の条件、nvrtx だけ）を、その transaction ごと recording の
     thread に渡し、kernel の返事を待たずに返す（hand-off、§6）。
2. **重なりの条件**: mview の subs は「記録 → End → submit → 次の記録」を繰り返す（`userland/tests/mview/renderer.c` 393〜436 行）。submit が kernel の
   仕事の終わりを待つ限り、組み立てを End に移しても別の thread に置いても、アプリの thread の 1 part は縮まない（§3.2）。subs の伸びは B-U からしか
   生まれない。B-U は vkQueueSubmit の失敗の報告を後へ遅らせる（§6.7）ので、ユーザーの判断が要る（phase.md の Q1）。
3. **B-K は今の 6 つの bench の fps を動かさない見込み**（毎 frame 記録し直すので組む回数は減らない）。記録し直さない bench（`--record=once`）でも、B-U の上では
   GPU の 1 part（実測 約 9 µs）が律速に近く、fps の伸びは 1 割に届かない見込みで、効き目は submit の CPU の時間（kernel の時間が半分以下）に出る。効くのは、記録し直さずに同じ command buffer を
   出し直す時（2 回目から ring だけ）と、P-C（End を先に送る）の時。実装はユーザーの (b) の決定に従って P-A の後に置き、受け入れは新しい bench の
   mode（`--record=once`）で測る（§12、§13）。
4. **big-1000 は (b) では縮まない**（inflight 1、GPU は今も submit の始めから走る）。B-K は draw が 256 を越える command buffer を組まない（§3.3、§5.4）。
5. push buffer は kernel が組むまま（NVK のように user が書く形は採らない。GPU の address space が全 session で 1 つ、§8）。
6. HAL は変えない。`include/uapi/` も P-A・P-B では変えない。P-C（任意）だけが native の capset の word に 1 bit を足し、その約束を記す
   `include/uapi/gpu-op.h` の comment を書き足す（§10、phase.md の Q3）。

## 1. 範囲と読んだ物

- 読んだ source（worktree p1-ws291p026、HEAD 272e6c2260。main 3b56148975 との差は plan/ の 4 file だけ）:
  - nvrtx: `src/drivers/gpu/nvrtx/vk/vk-execute.c`（slot・push・constants・pool・zero page・corners の区分 62〜81 行、`EXECUTE_RECORD_ROOM` 98 行、
    `EXECUTE_FIRST_DRAWS` 108 行、texture handle の sampler の位置 117 行、`nvrtx_vk_execute` 452 行、`execute_start`・`execute_continues`・
    `execute_begin`（825 行）・`execute_submit`・`execute_flush`・`execute_drain`・`execute_room`（935 行）・`execute_record`、`execute_begin_pass`・
    `execute_shadow`（1198 行）、`execute_draw_constants`（1802 行、application の constant buffer 0 は `slots[0]` の `EXECUTE_ROOT_AT`）、
    `execute_textures`（1935 行）・`execute_texture`・`execute_pool_entry`（2094 行）、`execute_transfer`・`execute_copy_buffer`（2735 行、CPU の copy と
    drain）・`execute_clear_image`（2781 行、method だけ）・`execute_quad`（2816 行、pool の entry と constants と corners を使う））、`vk-command.c`
    （`nvrtx_vk_command_reset` 110 行、`command_begin`・`command_end`・`command_reset`・`command_destroy`）、`vk-queue.c`（`queue_submit` 104 行、
    `gpu_sends` の規則）、`vk-memory.c`（`nvrtx_vk_image_address` 170 行: address は memory の va ＋ offset で、memory の寿命の間変わらない、
    `memory_destroy`）、`vk-layout.c`（`layout_update_write` 686 行、`layout_slot_clear` 868 行: set の slot は view・sampler・buffer の参照を持つ）、
    `vk-resource.c`（`resource_view_destroy` 976 行、sampler は `nvrtx_vk_free_payload` 902 行）、`vk-session.c`（close の順 76〜77 行）、
    `src/drivers/gpu/nvrtx/vk-node.c`（`nvrtx_vk_gpu_alloc_system` 375 行、`nvrtx_vk_gpu_free` 430 行: idle を待つ、`nvrtx_vk_gpu_submit` 482 行:
    quiet の epoch・`gpu_sends`・count）、`work.c`（`nvrtx_work_channel_submit` 1049 行: stamp・segment・(end stamp)・fence を空きを待ってから書き、
    put を 1 回、ring 512 entry）、`draw-push.c`（`nvrtx_push_draw_textures` 895 行: pool の base と `..._CACHE_NO_WFI` の invalidate）、`mmu.c`
    （`MMU_PTE_READ_ONLY` 61 行）、`runtime.c`（native の capset 85〜96・447〜452 行）。
  - libvulkan: `userland/desktop/libvulkan/context.c`（`vulkan_context_execute` 493 行: 516 行で `context->mutex`、`vulkan_context_transaction` 783 行:
    GPU_JOB_SUBMIT と GPU_COMMAND と GPU_COMMAND_SUBMIT は全部この中、`vulkan_kernel_error`、native の capset 38〜41 行）、`queue.c`
    （`queue_enqueue` 288 行・`queue_unfenced` 442 行・`queue_attempt` 475 行・`queue_submit_job`・`queue_write_pending`・`queue_execute_pending`）、
    `commands.c`（recording の thread 26〜44 行と `command_send_later` 1014 行・`command_sent_wait` 1112 行・`command_sender` 1183 行、
    vkBegin・vkEnd（64 KiB 以下の保留 549 行）・vkReset、`vulkan_command_pending_write`・`_read`）、`internal.h`（`struct vulkan_recording_batch` 212 行）、
    `sync.c`（RESERVE・ACTION・RETIRE・COMMAND_WAIT は `context->mutex` の下、レビューの指摘 1 の行）。
  - uapi: `include/uapi/gpu-op.h` 1869〜1880 行（native の capset の word の約束、bit 0 だけが書かれ bit 1 は漏れている）、`include/uapi/gpu.h` 97 行
    （`GPU_COMMAND_MAX` 64 KiB）。
  - 計測: `userland/tests/mview/renderer.c`（part の記録と submit 393〜436 行、`renderer_begin` 2095 行〜: 毎回 Reset と ONE_TIME_SUBMIT の Begin）、
    `plan/ws291/tests/rtx-p024.sh`・`rtx-runcost.sh`、`plan/tools/nvrtx/nvrtx-vk-d3-host-test.sh`（libvulkan の encoder と本物の dispatcher を host で
    繋ぐ試験）。
- 読んでいない物: NVK・nouveau・drm_sched の source。比べは p024 の計測と形だけ。SET_TEX_HEADER_POOL を描画の流れの途中で替えた時の
  hardware の振る舞いは確かめていない（§5.3 はそれに頼らない形にした）。

## 2. 今の流れ（事実）

subs-1000 の 1 part（p024 i03 の ac の内訳。1 part は 1 command buffer、約 16 の draw と 16 の texture）:

1. アプリの thread: vkResetCommandBuffer（libvulkan の中だけ。kernel の Reset は次の Begin の頭に載る）、vkBeginCommandBuffer（同じく頭）、
   vkCmd*（`recording` に encode、約 5.7 µs）、vkEndCommandBuffer（記録が 64 KiB 以下なので `end_pending`、kernel へは送らない）。
2. vkQueueSubmit: `queue_enqueue` → queue の mutex → `queue_attempt`（device の mutex）→ `queue_write_pending` で記録（RESET・BEGIN・records・END）を
   `QUEUE_SUBMIT` の前に同じ writer へ → part 1〜998 は fence も semaphore も無いので `queue_unfenced` が真で GPU_COMMAND 1 本（part 0 は acquire を
   待ち、part 999 は fence と signal を持つので job の道）→ `vulkan_context_execute`（`context->mutex`）→ 返事を読み、`vulkan_content_commit`。
3. kernel（呼び手の thread、`runtime_execute` が session の `exec_lock` と node の `lock` を持つ）: decode → RESET（record の object を離す、約 0.8 µs）→
   BEGIN → record の decode（約 1.7 µs）→ END（状態だけ）→ `QUEUE_SUBMIT` → `nvrtx_vk_execute`（slot の続きに push buffer、`execute_begin` で class の
   初期の state（channel が保つ時は省く）・slot の pool の base と invalidate・shader cache の invalidate、record ごとに method、`execute_submit` →
   `nvrtx_vk_gpu_submit` → `nvrtx_work_channel_submit`）。executor 約 6 µs（draw 16 本 3.3、ring 1.0）。
4. vkQueueSubmit 1 本 約 11 µs、record 約 5.7 µs、frame 17.1 ms（58.5 fps）。NVK は record 6.2・submit 4.65 µs（ring への書きは別の thread の kworker）。

submit の時にしか決まらない物と、組み置きでの扱い:

| 物 | 今どこで決まるか | 組み置きでの扱い |
| --- | --- | --- |
| push buffer の置き場所 | session の slot（3 × 2 MiB）の続き | session の組み置きの arena の chunk（§5.4） |
| texture header・sampler の pool の slot | slot ごとの pool、push buffer ごとに index を 0 から（`execute_pool_entry`） | session の heap の、view・sampler ごとの固定の index（§5.3） |
| pool の base | push buffer の頭ごと（slot が替わる時だけ値が変わる。前の run の fence が idle を待つ） | list の頭の固定の push（§5.3）。組み置きの segment は base を書かない |
| 内部の quad の constants・corners・quad の source の header | slot の constants・corners、quad ごとの pool の entry | v1 では quad を作る record を組まない（fallback、§5.5） |
| application の draw の constant buffer 0 | `slots[0]` の `EXECUTE_ROOT_AT` へ push buffer の中の load | 同じ（address は session の間変わらない） |
| class の初期の state | `class_ready`（channel が前の run の state を保つか） | submit の時に、session に 1 つの固定の前置きを list の頭に足すか否か（§5.6） |
| image・buffer・view の GPU の address、view の header | bind 済みの memory（`nvrtx_vk_image_address` は memory の va ＋ offset、memory の寿命の間変わらない） | 組んだ時のまま。record の参照が object を生かす |
| descriptor set の中身 | submit の時に set を読む | 組んだ時の写し。submit で世代を確かめる（§5.7） |
| render pass の target・clear の値 | record と framebuffer | 組んだ時のまま |
| shadow（pitch-linear の色と深度）と `kept` | session の shadow と `nvrtx_vk_gpu_quiet` の token | 組まない（fallback） |
| CPU の copy・`execute_drain`・video の decode・query | GPU を待つ | 組まない（fallback） |
| stencil つき深度の page の kind（`execute_depth_target` が初めての時 `nvrtx_vk_memory_kind`） | 初めての submit | address は変わらない（PTE の kind だけ）。build の中で行ってよい（node の lock の下、今と同じ） |

## 3. どこが律速か、(b) がどう効くか

### 3.1 G1 の中身

p024 の G1（約 8 µs）は decode 1.7・RESET 0.8・executor 6（ring 1.0）。組み立てそのものの費用はどこで走らせても消えない（毎 frame 記録し直す subs では
組む回数は減らない）。縮むのは「アプリの thread が待つ時間」だけ。

### 3.2 subs の 1 part の流れ（見積もり）

1 part を生産側（アプリの thread）・消費側（recording の thread）・GPU の 3 つの段に分け、定常の 1 part の時間を max(生産、消費、GPU) とする。

| 形 | 生産（アプリ） | 消費（thread） | GPU | 1 part | subs-1000 |
| --- | --- | --- | --- | --- | --- |
| 今（ac） | record 5.7 ＋ submit 11 | — | 約 9〜10（timing 有りの実測） | 約 17 µs | 58.5 fps（実測） |
| B-K だけ | 同じ（End と submit が同じ transaction） | — | 同じ | 約 17 µs | 変わらない |
| B-K ＋ End を thread で先に送る、submit は待つ | record 5.7 ＋ End の組み立てを待つ 約 9 ＋ ring の ioctl 約 3.5 | End の ioctl | 同じ | 約 18 µs | 下がる |
| **B-U** | record 5.7 ＋ hand-off 約 1〜2 | 今の submit の transaction 約 9.5〜10.5 | 同じ | 約 10 µs（消費） | 上限 約 95、見込み 75〜90 fps |
| B-U ＋ B-K | 同上 | 同上（END で組み、QUEUE_SUBMIT で ring、同じ transaction） | 同じ | 約 10 µs | B-U と同じ |

- GPU の列（2026-11-01 20:56〜 P1 が 10.0.10.3 で測った、build/tmp/p026-diag/subs1000/summary.txt）: kernel は p024 i3a（2764259344、ac の腕）、
  userland は p024 の base の 13 file と libvulkan lv9（509112954）、`rtx-runcost.sh` TIMING=on RUNS=6、subs-1000（`--draws=each --copies=1000
  --submits=1000`、1 part は約 16 draw: 第 1 版の「subs-100 の 1 run は 160 draw」は誤り）。
  - fps 56.4〜58.5（timing の分だけ下がる）、段は record 5.6〜5.8・submit 11.2〜11.8・**fence 0.00〜0.05 ms**（最後の submit の後に GPU が約 1 part で
    終わる = 今は GPU が CPU の 17 µs/part に遅れない）。
  - `nvrtx.vk.perf` の GPU の 1 run（`gpu_us ÷ timed`、timed は slot が替わる時に retire される run だけの標本で 1 窓 150〜580 個）: 最初の窓だけ
    avg 29 µs・max 1090 µs（起動の直後）、後の 10 窓は **avg 9〜10 µs**（max 10〜117 µs）。timing 有りは end stamp の WFI が足されるので、timing 無しは
    これ以下の見込み。runs は 1 窓 5 s に 21〜30 万（1 frame 1000 run と合う）。
  - よって B-U の後の律速は消費側（約 10 µs）で、GPU（約 9 µs 以下）は 1 part 13.3 µs（NVK の 75.4 fps）より短い。GPU と消費側の差が小さいので、
    見込みは 75〜90 fps のまま、上限は GPU と消費側の遅い方で 約 100 fps。P-A の 0 番では timing 無しの値と、標本の偏り（slot の替わりの run だけ）の
    無い数え方（§5.6 の retire の輪を P-A の前に入れない場合は、diag で全 run を数える）で確かめ直す。
- 同じ open の ioctl は GPU core が 1 本ずつに限り（`gpu_session_enter`）、executor は node の lock の下で走る。End と submit を 2 本の thread に分けても
  kernel の中では並ばない。重なりは「アプリの thread が kernel を待たない」事からしか生まれない。
- 生産側が速いので thread の queue は満ちる。満ちた時は生産側が眠り、半分空いたら起きる（§6.5）。1 frame に約 1000 ÷ 32 ≒ 30 回の眠りと起床は
  生産側に起き（消費側は queue が空にならないので眠らない）、1 回 数 µs として frame の 1 % 未満（推測。起床の回数を timing に数える）。
- B-K は subs の毎 frame の記録し直しでは効かない。効くのは (i) 同じ command buffer を記録し直さずに何度も submit する時（2 回目から ring だけ）、
  (ii) 複数の command buffer を記録してから 1 回の submit で出す時に End を先に送る形（P-C）。B-K があっても hand-off の非同期の部分は「submit の
  kernel の仕事の全部」のままで、失敗の意味は B-K の有無で変わらない（§6.7）。

### 3.3 big-1000（inflight 1）

- p024 i02 d1・d2: frame は record 3.7〜3.9・submit 3.3〜3.5・fence 1.1〜1.2 ms、GPU は submit の始めから約 4.5 ms。executor は 256 draw で最初の
  push buffer を GPU に出す（`execute_room`）。組み立て（1 draw 約 0.14〜0.17 µs）は GPU（約 0.22 µs）より速いので、submit の段は GPU の時間に隠れて
  いる。組み立てを submit から外しても、GPU は vkQueueSubmit より前に走れないので frame は約 record ＋ GPU のまま。
- 長い記録を End で組むと、GPU の開始が組み立ての分だけ遅れる。draw が 256 を越える command buffer では、今は 256 draw で GPU が走り始めるが、
  End で全部を組むと「全部の組み立て」の後になる（256 < draw の帯で (draw − 256) × 0.15 µs 遅れる）。よって B-K は draw が `EXECUTE_FIRST_DRAWS`（256）
  以下の command buffer だけを組む（§5.5）。big-1000 の受け入れは「後退なし」。NVK の 136 fps との差（client の encode と GPU の時間）はこの設計の外。

## 4. 全体の形

```
アプリの thread                       recording の thread（libvulkan、process に 1 つ）      kernel（nvrtx）
vkCmd* → recording に encode
vkEndCommandBuffer → end_pending
vkQueueSubmit（fence・semaphore 無し）
  → 今と同じ writer を組み、hand-off ─▶  vulkan_context_execute（GPU_COMMAND） ─────────▶  RESET・BEGIN・records の decode
  ← VK_SUCCESS（返事を待たない）                                                          END: 適格なら組み置き（B-K）
次の vkCmd* …                                                                            QUEUE_SUBMIT: 組み置きの list を ring へ
                                       返事を読む（失敗は context に latch）               （組み置きの無い物は今の executor）
vkQueueSubmit（fence あり）
  → vulkan_context_execute の入口で、この context の入口の時点までの hand-off が送られるのを待つ（drain） → 今の道
```

- B-U と B-K は独立に切り替えられる（§14）。B-U だけでも subs の伸びは出る。B-K だけでは subs は伸びない。
- P-C（任意）: B-K がある時、End の記録を submit を待たずに recording の thread で先に送る（eager End、§7）。

## 5. B-K: kernel の組み置き（prebuilt）の push buffer

### 5.1 責務と境界

- nvrtx の Vulkan の executor（`src/drivers/gpu/nvrtx/vk/`）と、nvrtx の内部の work channel の口（`work.c`・`vk-node.c`）の中だけ。GPU core（`gpu.c`）・
  uapi・libvulkan の wire は変えない。
- 組むのは `END_COMMAND_BUFFER` の handler（`command_end`）、使うのは `QUEUE_SUBMIT`（`queue_submit`）、捨てるのは Begin・Reset・Free・pool の Reset と
  Destroy・command buffer の destroy・session の close と、submit の時の世代の食い違い。
- 今の executor（submit の時に組む）は残す。組めない command buffer と、`nvrtx.vk.prebuild` が切りの時はそれを使う（fallback）。

### 5.2 内部の interface（案、名前は実装で決める）

```c
/* A run of the session's prebuild arena (vk-execute.c): push buffer bytes only, written front to back until it is reclaimed. */
struct nvrtx_vk_chunk {
	void *token;
	uint8_t *cpu;
	uint64_t va;
	size_t at;                        /* where the next build goes on */
	uint32_t live;                    /* prebuilt command buffers with a segment here */
	struct nvrtx_vk_ticket ticket;    /* the newest run that read it */
};

/* One segment of a prebuilt push buffer. */
struct nvrtx_vk_segment {
	uint64_t va;
	uint32_t bytes;
};

/* What End wrote for a command buffer (in struct nvrtx_vk_command_buffer, nvrtx-vk-objects.h). */
struct nvrtx_vk_prebuilt {
	bool built;
	bool refused;                                                   /* not tried again until the next recording */
	uint32_t refusal;                                               /* why, for the counts */
	struct nvrtx_vk_segment segments[NVRTX_VK_PREBUILT_SEGMENTS];   /* 4 at first */
	uint16_t chunks[NVRTX_VK_PREBUILT_SEGMENTS];                    /* each segment's chunk */
	uint32_t segment_count;
	uint32_t draws;
	uint64_t set_epoch;                                             /* the session's when built */
	struct nvrtx_vk_prebuilt_set *sets;                             /* each distinct set bound, with its generation; kept for the next build like records */
	uint32_t set_count;
	uint32_t set_capacity;
};
```

- work.c に、複数の segment を 1 つの stamp と 1 つの fence で載せる口（nvrtx の内部）:
  `int nvrtx_work_channel_submit_list(struct nvrtx_gpu *gpu, struct nvrtx_work_channel *channel, const struct nvrtx_work_segment *segments, uint32_t count, uint32_t *fence);`
  今の `nvrtx_work_channel_submit` は count 1 の包みにする。entry（stamp ＋ count ＋ timed の end ＋ fence）の空きを先に全部待ち、全部を書いてから put を
  1 回動かす（今と同じ all-or-nothing）。count の上限 `NVRTX_WORK_LIST_MAX`（32、ring 512 entry の内）。
- vk-node.c に `nvrtx_vk_gpu_submit_list(void *owner, const struct nvrtx_vk_segment *segments, uint32_t count, uint32_t bytes, struct nvrtx_vk_ticket *ticket)`
  （今の `nvrtx_vk_gpu_submit` と同じ lost と channel の確かめ・quiet の epoch・`gpu_sends` の 1 増し・runs と push_bytes の count）。
- executor の書き先（sink）: 今 `struct nvrtx_vk_execute` に散っている cursor（cpu・va・segment・at・draws・quads・started・early）を
  `struct execute_sink` にまとめ、GPU（今の slot、満ちたら ring に出して次の slot）と BUILD（chunk、満ちたら segment を閉じて次の chunk、GPU には出さない）
  の 2 つを持つ。method を書く code（`execute_record` 以下、`execute_draw` の差分、`execute_begin_pass`、`execute_clear_image`）は同じ物を通す。
  BUILD の sink では、GPU を待つ・CPU で書く・quad を作る処理（`execute_flush`・`execute_drain`・`execute_shadow`・`execute_quad`・`execute_cpu_copy`・
  `execute_copy_buffer`・`execute_decode`・`nvrtx_vk_video_run`）が「組まない」（内部の値 `EXECUTE_REFUSED`）を返す。texture の handle は heap から（§5.3）。

### 5.3 texture の header と sampler の heap（レビューの指摘 5 への答え）

- 問題: 今の pool は slot ごとで、push buffer の頭で base を書く。1 回の ring の書きに別々の pool の base を持つ segment を並べると、前の segment の draw が
  まだ texture を読む間に base が WFI なしに替わる（hardware の振る舞いは未確認）。今は base が替わるのは slot が替わる時だけで、前の run の fence が
  idle を待っている。
- 形: session に 1 つの heap（`nvrtx_vk_gpu_alloc_system` の 1 run、session の間生きる。初めて組む時に作る）。
  - header の表 `NVRTX_VK_HEAP_HEADERS`（16384 entry × 32 byte = 512 KiB）、sampler の表 `NVRTX_VK_HEAP_SAMPLERS`（4096 × 32 = 128 KiB）。sampler の
    index は texture handle の bit 20 から（`EXECUTE_HANDLE_SAMPLER_SHIFT`、handle は 32 bit）なので 4096 が上限。header は 2^20 未満。pool の
    MAXIMUM_INDEX の幅は実装で class の header（`Value referenced:`）で確かめる。
  - list の頭（§5.6）の固定の push 2 つ: (h1) heap の pool の base と 2 つの cache の invalidate と shader cache の invalidate、(h2) class の初期の state
    （vertex stream の代わりは `slots[0]` の zero page。今の `execute_begin` は今の slot の zero page を渡しているが、どの slot も同じ offset に zero page を
    持ち、slot は session の close まで返さないので、どちらでも正しい）＋ h1。作る時に 1 回書く。
  - view と sampler の object に heap の index（`heap_index`、0 は無し）を足す。組む時に初めて使う view・sampler に index を割り当て、header
    （`view->header`、今と同じく view ごとに 1 回作る）と sampler の words を heap に書く。handle は `view->heap_index | sampler->heap_index << 20`。
  - index の解放: view の payload の destroy（`resource_view_destroy`）と sampler の payload の destroy（今は `nvrtx_vk_free_payload`、専用の destructor
    を足す）で、session の最新の run の ticket（`nvrtx_vk_gpu_last`）を付けて退き待ちの list へ。`session->execute` が NULL（close の後）なら何もしない
    （heap は execute_close が返す）。割り当ては空きの index、無ければ退き待ちの先頭から passed を見た物（ticket を 0 にする）。heap が満ちたら組まない
    （fallback）。
  - 正しさ: 組み置きの record は set と framebuffer の参照を持ち、set の slot は view・sampler の参照を持つ（`layout_slot_clear` が離す）。組み置きが
    生きる間、その view・sampler は destroy されず index は変わらない。set の slot が書き換わると世代が変わり、組み置きは使われない（§5.7）。
    index の再利用は GPU が passed を越えた後。各 segment の頭には header と sampler の cache の invalidate（`_NO_WFI`）を置く（再利用された index の
    古い cache の行を落とす。中身を変えない entry を読む途中の draw には害が無い）。
- 組み置きの segment は pool の base を書かない。base は list の頭の h1 だけが、前の ring の書きの fence（idle を待つ）の後に書く。1 回の ring の書きの中で
  base は替わらない。fallback の run は今どおり自分の slot の base を書き、その間には必ず fence が入る（別の ring の書き）。
- 選ばなかった形: (a) segment の境に WFI を入れる（command buffer ごとに pipeline が空になる。P-C の複数 command buffer で効きを消す）、(b) list に
  同じ chunk の pool の物だけを並べる（chunk を跨ぐ command buffer で分かれ、規則が複雑）。heap は draw ごとの pool の書き（新しい texture ごとに
  64 byte）も無くす。

### 5.4 組み置きの arena（chunk）と memory の持ち方（レビューの指摘 2・14 への答え）

- chunk は `nvrtx_vk_gpu_alloc_system` の 256 KiB の run（初期値、実装で測って決める）。中身は push buffer の bytes だけ（v1 は quad を組まないので
  constants・pool・corners は要らない）。
- 書きの安全: 今の method の書き手は境を見ずに書き、安全は `execute_room` の「record の前に `EXECUTE_RECORD_ROOM`（32 KiB）を空ける」だけで守られる。
  BUILD の sink も同じ規則で、record の前に `chunk.at + EXECUTE_RECORD_ROOM ≤ chunk の大きさ` を確かめ、足りなければ segment をそこで閉じ、次の chunk の
  頭で新しい segment を始める（cache の invalidate、target の書き直し: 今の `execute_room` の後の `state->target_written = false` と同じ）。record を
  跨いで segment を閉じない。安全はこの前の確かめだけが守る。chunk は使う大きさ（256 KiB）に `EXECUTE_RECORD_ROOM` の余白を足して確保し
  （256 ＋ 32 KiB）、record の後に `at` が使う大きさを越えていれば kernel の log 1 行と refused にする（診断。越えるのは前の確かめの誤りの時だけで、
  その時も余白の中に収まる）。chunk の中は前から書くので、他の command buffer の bytes を越えて書く事は無い。
- build の始めに今の chunk の残りが `EXECUTE_RECORD_ROOM` の 2 倍に満たなければ次の chunk から始める（1 command buffer が 2 つに割れにくくする）。
  End の後、chunk の `at` は build の終わり（8 byte 境）へ進む。次の command buffer はその後に続ける。chunk の中は前から後ろへ書くだけで、
  回収するまで書き直さない。
- 回収: chunk は `live`（segment を持つ組み置きと、今の build の数）と `ticket`（読んだ最新の run）を持つ。build が segment を開いた時点で、その
  chunk の `live` を 1 増す（build の途中の chunk は回収されない）。`live` が 0 で `nvrtx_vk_gpu_passed(ticket)` の chunk は `at` を 0 に戻して使える。
  ただし今の追記先の chunk（`execute->chunk`）は回収の候補から外す（追記先が live 0 で passed なら、その場で `at` を 0 に戻してから build を始めて
  よい。build の途中では戻さない）。次の chunk が要る時は chunk の表（最大 128 個）を順に見る（順序の付いた free list は使わない: Reset の順は
  submit の順と限らない）。無ければ新しい chunk（session の上限 32 MiB = 128 chunk の内）、上限なら組まない（fallback）。GPU を待たない。
  H1 に「live 0 の追記先の途中から始まり 2 segment になる build」の場合を入れる（再レビューの B1）。
- 解放: Begin・Reset・Free・pool の Reset と Destroy・command buffer の destroy（`nvrtx_vk_command_reset` の中）で組み置きを捨て、各 chunk の `live` を
  1 減らす。`nvrtx_vk_command_reset` は今 session を受けない（vk-command.c 110 行）ので引数を足す（nvrtx の内部の関数）。`session->execute` が NULL の時
  （session の close で `nvrtx_vk_objects_close` の後。payload が他で持たれて EBUSY の後に走る destructor も同じ）は何もしない（chunk は
  `nvrtx_vk_execute_close` が返す。順は `vk-session.c` 76〜83 行）。chunk の run を返すのは session の close の時だけ（`nvrtx_vk_gpu_free` は idle を待つ。今の slot と同じ規則）。
- refused の build: build の始めの chunk と `at` を覚え、組めないと分かったら `at` を戻し、途中で取った chunk の `live` を戻す（ring に一度も出して
  いないので ticket は変えない）。record は変えない。
- 量: subs の 1 part は push 約 3〜4 KiB（推測）。1000 part で約 4 MiB。frame N＋1 は frame N の chunk がまだ live の間に別の chunk に組み、frame N の
  chunk は N＋1 の記録の終わりまでに live 0 で passed になる。定常は約 2 frame 分 8 MiB（inflight 2 で約 12 MiB）。1 つの長く生きる組み置きは
  chunk 1 つ（256 KiB）を止める。perf の行に chunk の数と refused の理由ごとの数を出す。

### 5.5 組める command buffer（適格）

End の時に次を全部満たす時だけ組む。どれかに当たれば `refused`（理由を数える）、submit は今の executor。End の返事は組めても組めなくても今と同じ
（組めない事は command buffer の失敗にしない）。

1. `nvrtx.vk.prebuild` が入（§14）、command buffer の状態が EXECUTABLE（記録の失敗なし）、primary（今も primary だけ: `command_allocate`）。
2. record の数が `NVRTX_VK_PREBUILD_RECORDS_MAX`（2048）以下、draw の数が `EXECUTE_FIRST_DRAWS`（256）以下（§3.3）。
3. 組む途中で BUILD の sink が `EXECUTE_REFUSED` に当たらない: quad を作る record（copy image・blit・buffer と image の copy、shadow の pass）、
   CPU の copy（copy buffer、`execute_cpu_copy`）、`execute_drain`・`execute_flush`、video と query、segment の数の上限（4）、chunk・heap の index が
   取れない、chunk の越えの確かめ。
4. 使った descriptor set（重ねを除く）の世代の写しが取れる（配列は record の配列と同じく command buffer に残して使い回し、足りなければ伸ばす。
   伸ばせない ENOMEM は refused）。固定の上限は置かない（mview の part は group ごとに texture の set を bind する: renderer.c 2681〜2698 行。
   group の数は model に依る）。
5. build の結果が 0 以外なら、`EXECUTE_REFUSED` に限らず全部 refused（理由を数える）。今の code は draw の EINVAL・memory の無い EIO・uniform の
   EINVAL などを返す。submit の時は今の executor が同じ error を返すので、End の返事は変えない。
6. build の前に record の opcode を一度見て（pre-scan）、組めない opcode（COPY_*・BLIT・BUFFER と IMAGE の copy・DECODE・video・query）があれば
   build を始めない（毎 frame 記録し直す compositor が blit を持つ時に、毎 frame 途中まで組んで捨てない）。shadow の pass は target が決まるまで
   分からないので build の中で refused。

- 組める record（v1）: bind の全部、push constants、viewport・scissor、render pass の begin・end（shadow を通らない物: block-linear の target か深度の無い
  pitch-linear）、draw・draw indexed、clear color image（method だけ）。mview の subs の part はこれだけで、WSI の image が block-linear の時に組める
  （§12 の前提）。
- quad の record を組む形（chunk に quad の constants、heap に組み置きの間だけ生きる quad の source の header の範囲）は P-D の候補。

### 5.6 submit（`queue_submit`）

1. vkQueueSubmit の command buffer を順に見て、組み置きがあり世代が合う（§5.7）物が続く間、その segment を list に集める（上限 `NVRTX_WORK_LIST_MAX`
   は list の頭を含めて 32）。`work.c` の今の固定の `entries[4]`（1065 行）は list の entry の数（stamp ＋ 32 ＋ timed の end ＋ fence = 35）に広げる。
2. list の頭に固定の push を 1 つ足す: channel が class の state を保たない（`class_ready` が偽: session の最初の run、共有の work channel）なら h2、保つなら h1。
   どちらも heap の run の中の書き済みの bytes なので、submit の時に確保も書きもしない（レビューの指摘 10 への答え）。
3. `nvrtx_vk_gpu_submit_list` で出す。成功した時だけ: 各組み置きの chunk の `ticket` を今の run に、`class_ready = nvrtx_vk_gpu_state_kept`、
   `execute->kept.valid = false`（組み置きの pass が shadow の写しの元を書きうる。quiet の epoch も submit で動く: 1 回の publish と 1 回の
   `nvrtx_gpu_submitted` を守る）、`nvrtx_vk_gpu_drawn` に draws。run の ticket を session の小さな輪（64）に積み、次の submit と chunk を探す時に、
   passed になった物から `nvrtx_vk_gpu_retire` に通す（今 retire は slot が替わる時にしか呼ばれず、GPU の時間 `gpu_us`・`timed` と engine の trap の
   確かめ `vk_node_gr_check` はそこで走る: vk-node.c 662〜698 行、vk-execute.c 776 行。組み置きの run もそこに数える。輪が満ちたら最古を passed の
   有無に関わらず数えずに捨て、捨てた数を perf に出す）。
4. list が上限に達した時、世代が合わない・組み置きの無い command buffer に来た時は、集めた list を先に出し、その command buffer は今の `nvrtx_vk_execute`。
   2 回目以降の ring の書きの失敗は今の `gpu_sends` の規則で EIO（出した後の失敗。BUG-381 の規則のまま）。
5. fence は今どおり session の最新の run（`nvrtx_vk_gpu_last`）。`queue_ran`・`queue_accepted` も今どおり。
- 組み置きの中身は submit で変えない（patch しない、§11 の R3）。同じ command buffer を何回 ring に載せても、同時に載っていてもよい（SIMULTANEOUS_USE、
  間違った二重の submit でも壊れない）。各 segment は GPU の前の状態に頼らない: 頭で cache の invalidate、最初の draw は全部の state（`held` は segment の
  始めで無効、今の `reuse.pools++` と同じ）、constant buffer 0 は最初の draw で全部 load、pass の中なら target を書き直す。前の状態に頼るのは class の
  state と pool の base だけで、どちらも list の頭が書く。

### 5.7 descriptor set の世代

- `struct nvrtx_vk_descriptor_set` に `generation`、session に `set_epoch` を足し、set の slot を変える所で両方を増やす: `layout_slot_set`・
  `layout_slot_buffer`・`layout_slot_clear`（`layout_update_write` 686 行と copy の `layout_update_sets` 633 行はこれらを通る）。`layout_free_sets`
  586 行・`layout_pool_sets_free` 466 行・`layout_reset_pool` 430 行は公開を外すだけで slot に触らず、slot が離されるのは最後の参照が消えた時の
  `layout_set_destroy`（880〜906 行）だけ。record が set を持つ間は中身が変わらないので、free と reset は世代を変えない（今の executor も submit の時に
  同じ set を読み、結果は同じ）。
- build は使った set とその世代を `prebuilt.sets` に残し、`set_epoch` を写す。submit は session の `set_epoch` が同じなら確かめ無しで使い、違えば残した set の
  世代を 1 つずつ比べる。違う set があれば組み置きを捨て（chunk の live を戻す、ticket はそのまま）、今の executor で走らせる。
- Vulkan では bind した set を更新すると command buffer は invalid になる（update-after-bind は nvrtx が報告していない）ので、正しいアプリでは起きない。
  起きても今と同じ結果（submit の時の set で描く）になる網で、heap の index が変わった view を古い index で読む事も防ぐ（§5.3 の正しさ）。
- image・view・sampler・pipeline・buffer は作った後に address も中身も変わらない（§2 の表）ので世代は要らない。

### 5.8 状態の遷移（command buffer の組み置き）

| 今 | 事 | 次 |
| --- | --- | --- |
| なし | END（適格） | built |
| なし | END（不適格） | なし（refused、この記録の間は試さない） |
| built | QUEUE_SUBMIT、世代が合う、ring の書きが成功 | built（chunk の ticket を更新） |
| built | QUEUE_SUBMIT、ring の書きが失敗 | built（ticket は前のまま。session は lost か ETIMEDOUT で、今の失敗の道） |
| built | QUEUE_SUBMIT、世代が違う | なし（chunk の live を戻す、今の executor で走る） |
| built・なし | Begin・Reset・Free・pool の Reset と Destroy・destroy | なし（chunk の live を戻す） |
| built | session の close | なし（`session->execute` が NULL なら何もしない。chunk は execute_close が返す） |

secondary の command buffer: nvrtx は今も primary だけ（`command_allocate` が ENOTSUP）。変えない。

### 5.9 並行性と lock

- build は `command_end` の中、submit は `queue_submit` の中で、どちらも `runtime_execute` が持つ session の `exec_lock` と node の `lock` の下（今の
  executor と同じ）。chunk の表・heap の割り当て・退き待ちの list は session の executor の物で、`exec_lock` が守る。ticket の比べは
  `nvrtx_vk_gpu_passed`（channel の fence の lock の無い読み、今と同じ）。
- ring は `nvrtx_work_channel_submit_list` が channel の lock の下で書く（今と同じ）。
- 新しい thread・割り込み・waitq は足さない。GPU が chunk と heap を読むのは DMA（今の slot と同じ coherent な system memory）。CPU の書き（build と heap の
  entry）は ring の publish の前に終わり、今の `work_ring_publish` の順序（write barrier、USERD の put）に乗る（実装で barrier の位置を確かめる）。
- 改善の余地（この設計の外、P-D）: build を node の lock の外（`exec_lock` だけ）で走らせれば他の session（compositor）が待たされない。

### 5.10 失敗と回復

- OOM（chunk・heap の確保）: build では fallback（End は成功）。submit では確保しない（list の頭は heap の書き済みの bytes）。
- 組み置きだけの vkQueueSubmit で list が 1 つなら、失敗は GPU に何も出す前（EIO = session が lost、ENODEV、ring の空きの 2 s の時間切れ ETIMEDOUT）
  だけ。list が 2 つ以上・今の道と混じる時は今の `gpu_sends` の規則（出した後の失敗は EIO）のまま。共有の work channel でも submit の時の確保は無い。
- GPU の fault・時間切れ: 今の recovery（reaper が channel を止め、session を lost に）。lost の session の submit は EIO。chunk と heap は session の close で
  今の slot と同じ規則で返す（止まらない channel の memory は device が持ったまま）。
- job の CANCEL: `runtime_job_command` の outcome（queue_ran・queue_accepted・gpu_sends）は変えない。組み置きの submit も `gpu_sends` を 1 増す。

## 6. B-U: fence の無い submit の hand-off（libvulkan）

### 6.1 条件

`queue_enqueue` で次を全部満たす時だけ: `queue_unfenced` が真（public、fence・semaphore・pNext 無し、WSI の lock を持たない、`GPU_CAP_JOB_SUBMIT` と同期の
command の kernel = 今は nvrtx だけ）、hand-off が入（環境変数 `VULKAN_ZEDBSD_SUBMIT_HANDOFF` が 0 でない、`sysconf(_SC_NPROCESSORS_ONLN)` が 2 以上、
fork の子でない）、recording の thread が動いている（今は長い記録の batch の `command_send_later` だけが `pthread_once` で起こすので、subs の 64 KiB
以下の part だけでは起きない。hand-off の道も同じ `pthread_once(&command_sender_once, command_sender_start)` を呼び、起こせなかった時だけ今の
同期の道へ）、1 回の vkQueueSubmit の command buffer の数が `VULKAN_HANDOFF_COMMANDS` 以下（越えれば同期の道）、context に失敗が latch されていない
（されていれば今の同期の道と同じく `vulkan_device_error` と、device の mutex の下の `vulkan_sync_device_error` で印を付けて DEVICE_LOST を返す）。
hand-off が入った数を timing に数え、P-A の受け入れで 0 でない事を確かめる。

### 6.2 hand-off の場所

```c
/* One fence-less submission handed to the recording thread (internal.h). */
struct vulkan_submit_handoff {
	struct vulkan_context *context;
	struct vulkan_writer transaction;          /* the submission's whole writer: recordings, then QUEUE_SUBMIT; storage kept for reuse */
	uint32_t replies;                          /* the recordings' replies, then the submission's */
	uint32_t expected[VULKAN_HANDOFF_REPLIES]; /* each reply's opcode, in order */
	struct VkCommandBuffer_T *commands[VULKAN_HANDOFF_COMMANDS]; /* whose sent_pending this holds */
	uint32_t command_count;
	uint64_t sequence;                         /* the context's hand-off number */
	VkBool32 waiting;
	struct vulkan_submit_handoff *next;
};
```

- queue ごとに 64 個の輪（`VULKAN_HANDOFF_SLOTS`）。recording の thread の FIFO は今の `struct vulkan_recording_batch` と同じ列に並べる（batch と hand-off は
  互いの順を保つ。列の要素に種類の印を足す）。
- context に 2 つの数: `handoff_queued`（app の thread が `command_sender_lock` の下で増やす）と `handoff_sent`（thread が送って返事を読んだ後に増やす、
  atomic）。

### 6.3 流れ

1. アプリの thread（queue と device の mutex の下、今の `queue_attempt` と同じ順）: 今と同じく `queue_write_pending` で保留の記録を writer に写し、
   QUEUE_SUBMIT を encode する。各 command buffer について、`vulkan_command_pending_read` が返事を読む代わりに、返事の opcode（頭と End）を場所の
   `expected` に写し、`end_pending` を偽にして `recording` を空にする（記録は writer に写った。同じ command buffer の後の submit は記録を二度送らない）。
   `vulkan_content_commit` と、待ちの software の印の扱い（今の `*accepted` の後の処理）をここで行う。各 command buffer の `sent_pending` を 1 増す。
   場所の writer と、場所が前から持つ storage を入れ替え（今の `command_send_later` と同じ形、malloc を毎回しない）、`handoff_queued` を増やして
   `sequence` を付け、FIFO の最後へ。VK_SUCCESS を返す。
2. recording の thread: FIFO から取り、`vulkan_context_execute(context, &transaction, replies × 8, &reader)` で送り、返事を順に読む（recording の頭と End、
   最後に submit）。どれかが失敗なら §6.7。`sent_pending` を減らし、`handoff_sent` を増やし、待つ者がいれば broadcast。
3. thread は command buffer の `state`・`error` を書かない（今の batch と同じく `sent_error` だけ。次の Begin・End・Free の `command_sent_wait` が拾う）。
   hand-off の失敗は context の lost なので、command buffer の印は要らない。
4. 複数の hand-off を 1 つの transaction にまとめる形は P-A では入れない（system call は減るが、途中の失敗の後ろが kernel で走りうる。まとめる時は合計を
   `GPU_COMMAND_MAX − 128` 以下にし、途中の失敗で context を lost にする。P-A で測ってから決める）。

### 6.4 順序（drain、レビューの指摘 1・7・17 への答え）

- drain を置くのは `vulkan_context_execute` の入口（`context->mutex` を取る前）だけ。入口で `t = handoff_queued` を読み、`handoff_sent ≥ t` になるまで
  `command_sender_done` で待つ。recording の thread 自身の呼び出し（thread local の印）は待たない。
- これで十分な理由: 仕事を出す ioctl（GPU_JOB_SUBMIT・GPU_COMMAND・GPU_COMMAND_SUBMIT）は全部 `vulkan_context_transaction` の中で、
  `vulkan_context_execute` を通る。fence のある submit・WaitIdle（空の job の submit）・present（`vulkan_queue_submit_locked`）・object の作り壊し・
  descriptor の更新は、前の hand-off の後に kernel に届く。
- drain しない raw の ioctl と理由: `sync.c` の GPU_JOB_RESERVE・ACTION（COMMIT・CANCEL）・RETIRE・GPU_COMMAND_WAIT（`context->mutex` の下で出る。仕事を
  出さず、hand-off は job を持たないので ledger と食い違わない）、`memory.c` の GPU_RESOURCE_DESTROY・mmap・export・import・GPU_BLOB_CREATE_PLACED、
  `external-fence.c` の GPU_FENCE_*、`wsi-image.c`・`wsi-swapchain.c` の raw の ioctl、`wsi-display.c` の GPU_DISPLAY_PRESENT・PRESENT_SYNC・
  GPU_FENCE_RESET（1460〜1479・1873 行、display node の fd。drain 済みの job の wait_fd に頼る）（どれも仕事を出さない。hand-off の中の仕事が読む物を壊すのは
  「GPU で使い終わる前に free する」誤ったアプリだけで、その時も kernel の VkDeviceMemory は backing の参照を持つ: `vk-node.c` 127〜129 行）。
  P-A のレビューの確かめ項目にする。
- deadlock が無い理由: drain の待ちは `context->mutex` を持たない（入口が mutex の前）。呼び手が queue・device の mutex を持つのはよい。thread は
  `command_sender_lock`（短く持つだけ）と `context->mutex`（`vulkan_context_execute` の中）だけを取り、queue・device の mutex を取らない。
  `vulkan_context_lock` で `context->mutex` を持ったまま `vulkan_context_execute` を呼ぶ道は今も自己 deadlock なので無い。これを code で守るため、
  thread local の「この thread が持つ context の mutex の数」を増減し、drain に入る時に 0 でなければ abort する（1 回の比べ）。`sync.c` は
  `pthread_mutex_lock(&context->mutex)` を直に取る（648・760・1067・1107・1153・1189・1575・1943・1974・2041・2112 行）ので、これらを
  `vulkan_context_lock`・`_unlock`（数を増減する包み）に替える。別の context の mutex を持ったまま execute を呼ぶ既存の道が無い事を、abort を
  入れる前に全部の lock の道の一覧で確かめる（P-A の作業。memory.c 1276〜1334 行の lock の中は raw の ioctl だけと読める）。
- 飢えない理由: drain は入口の時点の番号までしか待たない。描く thread が hand-off を続けても待ちは伸びず、上限はその時に FIFO に並んでいる物の
  時間（hand-off は約 10 µs × 64 ≒ 0.6 ms、加えて 256 KiB の長い記録の batch が並んでいればその decode の分）。FIFO は process に 1 本なので、別の
  context の物が前にあればそれも待つ（上限は同じ形）。context ごとの FIFO は同じ open の中で並ばない事（R7）から得が小さく、入れない。present の
  worker の drain も、その時に並んでいる次の frame の hand-off を待つ（present が最大 約 0.6 ms 遅れうる。推測、P-A の frame-ops で確かめる）。
- vkDestroyDevice・context の close は drain してから（close も `vulkan_context_execute` を通る物は自然に、通らない物は明示に）。

### 6.5 lock の順と待ち

- アプリの thread: queue の mutex → device の mutex → `command_sender_lock`。drain の待ちは `context->mutex` を持たずに。
- recording の thread: `command_sender_lock` と `context->mutex` だけ。device の失敗の印は context の atomic の latch（`vulkan_context_error`）で付け、
  device の mutex が要る処理（`vulkan_sync_device_error`）は次の同期の呼び出しが拾う（`queue_attempt` は `vulkan_context_execute` の DEVICE_LOST を見て
  `vulkan_sync_device_error` を呼ぶ: 今と同じ）。
- back-pressure: queue の 64 個の場所が全部使用中なら、アプリの thread は queue ごとの専用の cond（`handoff_room`）で「待つ者がいる」印を立てて
  眠り、使用中が半分（32 個）を下回った時に thread がその cond を 1 回 signal する。今の `command_sender_done` は 1 件ごとに broadcast され
  （commands.c 1228 行）、`command_sent_wait`・batch の場所・drain が使うので、back-pressure はそれを共用しない（共用すると 1 frame に約 1000 回
  起こされる）。thread は FIFO が空の時だけ「眠る」印を立てて眠り、アプリは印がある時だけ `command_sender_ready` を signal する（今は毎回）。
  どちらの印も `command_sender_lock` の下で読み書きする（起こし損ねを防ぐ）。眠りと起床の回数を timing（`VULKAN_ZEDBSD_SUBMIT_TIMING`）に数える。

### 6.6 1 CPU・fork・他の driver・Venus

- 1 CPU では hand-off は切り（費用だけ増える）。
- fork: `pthread_atfork` の prepare で `command_sender_lock` を取り、parent と child で離す（fork の瞬間に thread が持っていた lock で子が止まらない
  ように）。child では hand-off を切り、`command_sender_started` も偽にし（今の batch の道も子では同期に戻る）、子の `handoff_queued`・`handoff_sent` を
  揃える。thread が `context->mutex` を持つ瞬間の fork は今の batch の道と同じ危険で、子が親の device を使うのは Vulkan の外として扱う。
- `GPU_CAP_JOB_SUBMIT` は backend の job の表に `command` がある時だけ gpu.c が立て、それは nvrtx だけ（p024 i03 の (c) の調べ）。amdrdna・i915・powervr・
  bcm2711・venus・mali・ghci の道は変わらない。drain の確かめは全部の context に入る（hand-off が無ければ atomic の読み 2 回）。Venus の回帰は libvulkan の
  変更として流す（§12）。

### 6.7 失敗の意味（ユーザーの判断が要る点）

- 今: fence の無い submit の失敗は、確かな拒否（OOM）は vkQueueSubmit が OOM を返し、他は DEVICE_LOST（p024 i03 の (c)）。renderer だけが見る End の失敗は
  vkQueueSubmit が返す（OOM はそのまま、他は DEVICE_LOST）。
- B-U: vkQueueSubmit は kernel の返事の前に VK_SUCCESS を返す。後で失敗が分かったら、その仕事は走らず、後の仕事はそれに頼るので、context を
  DEVICE_LOST に latch し、次の呼び出しから VK_ERROR_DEVICE_LOST。**OOM も DEVICE_LOST になる**（Vulkan は DEVICE_LOST をいつ返してもよいが、アプリが
  OOM から立ち直る機会は無くなる）。thread は stderr に 1 行（`VULKAN handoff failed result=… device lost`）を出す。
- 非同期になるのは、その submit の kernel の仕事の全部（RESET・BEGIN・record の decode・END（B-K の build）・QUEUE_SUBMIT）。B-K の有無で変わらない
  （第 1 版の「B-K の後は ring だけ」は誤り）。定常の frame では kernel の確保は起きない（record の配列・slot・chunk は前の frame の物を使う）ので、遅れて
  見える失敗は実際にはほぼ GPU を失った時と、初めの frame の確保の失敗だけ（推測。perf の数で確かめる）。
- 選択肢は phase.md の Q1。

## 7. P-C（任意）: End を先に送る（eager End）と引き戻し

- kernel が「End で組む」事を知らせる時（§10 の capset の bit）、短い primary の vkEndCommandBuffer は記録（End 込み）を recording の thread に渡して返る
  （`end_pending` にしない）。kernel は END で組む。アプリが次の command buffer を記録する間に組み立てが走る。
- 同期の vkQueueSubmit（fence あり、または B-U が切り）がその command buffer を名指した時、その End がまだ FIFO にあり送り始めていなければ FIFO から
  引き戻し、今どおり submit の transaction に入れる（system call を増やさない）。送り始めていれば `command_sent_wait`。引き戻せるのは、FIFO の中で
  その End より後ろに同じ context の項が無い時だけ（後ろに同じ command buffer を名指す hand-off の QUEUE_SUBMIT があれば、引き戻すと QUEUE_SUBMIT が
  END より先に kernel に届く）。それ以外は End が送られるのを待つ。
- 効く場面: 複数の command buffer を記録してから 1 回 submit するアプリ。mview に `--submit-all` を足して測る（§12）。compositor（1 frame 1 command buffer、
  fence あり）は引き戻しで今と同じ。
- END が非同期になるので、End の失敗も B-U と同じく後の DEVICE_LOST になる（Q1 の答えに従う）。

## 8. security の境界

- push buffer は kernel が組む（今と同じ）。組み置き（chunk）と heap は kernel の memory（`nvrtx_vk_gpu_alloc_system`）で、user の address space に写さない。
  user が渡すのは今と同じ wire（record）で、object は session の表で引き、address は kernel が bind 済みの object から作る。heap の index は kernel が
  割り当て、user は見ない。
- user が push buffer を書く形（NVK の形）を採らない理由: nvrtx の GPU の address space は 1 つで、全 open の blob と driver の page（ring・semaphore・
  fence の push buffer・golden context の buffer）が同じ空間にある（`nvrtx-work.h`）。method（semaphore の release、copy、compute の launch など）は任意の
  VA を指せるので、GPU の VM は session を隔離しない。method の流れを kernel が検証するのは現実的でない（Linux の nouveau・NVIDIA の driver も process
  ごとの VM と channel で隔離し、method を検証しない）。GPU に固有の事は kernel の driver に置き libvulkan は共通に保つ構成（guardrail の「kernel と libc」）
  とも合う。
- 既にある危険（この設計で種類は増えない）: 共有の VA のため、範囲外に書ける shader があれば他の session の memory・slot・chunk・heap を壊しうる。
  chunk と heap は slot より長く残るので、壊れた時に効く時間は長い。緩和の候補（Future Work）: chunk と heap を GPU から読み取りだけの PTE で写す
  （`mmu.c` に `MMU_PTE_READ_ONLY` はあるが、`nvrtx_work_map_system` に読み取りだけの口は無い。nvrtx の中だけの変更で足せる見込み）。session ごとの VA は
  この設計の外。

## 9. 他の driver と libvulkan への影響

- B-K は nvrtx の中だけ。他の driver は変わらない。
- B-U は libvulkan の `queue.c`・`commands.c`・`context.c`・`internal.h` と、新しい file（hand-off の場所と FIFO の扱い。host 試験のために transport を
  callback で受ける形）。条件が `GPU_CAP_JOB_SUBMIT`（nvrtx だけ）なので他の driver の道は変わらないが、drain の確かめは全部の context に入る。
- P-C の eager End は capset の bit が立つ時だけ。

## 10. HAL と UAPI を変えない根拠

- HAL（`include/hal/hal.h`）: 触る所が無い。memory は nvrtx の既存の node の口（`nvrtx_vk_gpu_alloc_system`・`nvrtx_work_map_system`）、時刻は
  `nvrtx_now_us`、thread は libvulkan の pthread。driver から HAL を呼ばない規則にも触れない。HAL の差分の案は無い（`proposed/` は作らない）。
- `include/uapi/`（P-A・P-B）: 変えない。wire の opcode（`gpu-op.h`）は同じ物を同じ順で送る。GPU_COMMAND・GPU_JOB_SUBMIT の ioctl も同じ。
- P-C だけ: native の capset の word（176 byte の capset の byte 172、magic は byte 168）に bit 2 `PREBUILD` を足す。値は `runtime.c` の `RUNTIME_NATIVE_*`
  と libvulkan `context.c` の `VULKAN_NATIVE_CAPSET_*` が持つが、約束は `include/uapi/gpu-op.h` 1869〜1880 行の comment が記している（今は bit 0 の
  H.264 だけ。bit 1 の同期の command は書かれていない）。bit 2 を足すなら同じ comment に bit 1・2 を書き足す（uapi の file の文書の変更。構造体・
  ioctl・opcode は変えない）。代わりの案は環境変数だけで eager End を入れる形（既定は切）。phase.md の Q3。

## 11. 選ばなかった案

- R1 user（libvulkan）が NVIDIA の push buffer を vkCmd* の時に書く（NVK の形）: §8。security の境界と構成の規則に反する。
- R2 B-K だけ（End で組む、submit は待つ）: §3.2 のとおり subs の frame は縮まず、End を別の transaction にすれば system call が 1 本増えて下がる。
- R3 patch の表（組み置きを submit の時に少し書き換える）: 書き換えには同じ command buffer の前の run の終わりを待つか二重に持つ要があり、submit の時の
  CPU の仕事を戻す。submit の時にしか決まらない物は list の頭（class の state と pool の base）か fallback で足りる（§2 の表）。
- R4 kernel の中で非同期にする（GPU_COMMAND は decode して積んで返り、lane の worker が組み立てと ring）: 失敗の意味は B-U と同じく遅れ、job の marker の
  順を worker の ring の後に合わせる kernel の変更が要る。B-U は kernel を変えずに同じ形を作れる。
- R5 長い command buffer も End で組む: §3.3、frame が後退する。
- R6 長い command buffer を初めての submit で組んで保つ（retain）: 記録し直さないアプリでだけ効き、mview の bench では効かない。P-D に回す。
- R7 recording の thread を context ごと・queue ごとに増やす: 同じ open の ioctl は 1 本ずつなので並ばない。
- R8 組み置きの pool を chunk ごとに置き、list の segment の境に WFI（§5.3 の (a)）: 複数 command buffer の submit で pipeline が毎回空になる。
- R9 drain を `sync.c` の raw の ioctl の前にも置く（第 1 版）: `context->mutex` の下で待つので recording の thread と deadlock する（レビューの指摘 1）。

## 12. 試験の計画

host（担当が流す、各 Phase の受け入れの前提）:

- H1（P-B）`plan/tools/nvrtx/nvrtx-vk-d3-host-test.sh` の拡張: libvulkan が encode した stream を本物の dispatcher に通し、偽の node が ring に載った segment の
  bytes を写す。比べは method の列そのものでなく、host の小さな解釈器（3D class の register の写し、constant buffer 0 への load、pool の base と index から
  解いた header・sampler の中身、vertex stream と index の address、target）で、各 draw と clear の時点の「意味の状態」を prebuild の入と切で比べる。
  address は指す中身で比べ、texture の handle は解いた header と sampler の中身で比べる。場合: mview の subs の 1 part 相当、clear color image、uniform buffer、
  複数の pass、複数の command buffer の 1 回の submit、同じ command buffer の 2 回の submit、16 texture の draw が続く物、chunk の境を跨ぐ長い command buffer
  （256 draw 近く）、class の前置きの h1・h2。fallback（quad の record、copy buffer、shadow、video の stub、record と draw の上限、chunk と heap の確保の
  失敗の注入、End と submit の間の set の更新、segment の上限）がどれも切と同じ状態の列。Reset の後の chunk と退いた heap の index が、偽の fence の passed
  の前に使われない事。chunk の越えの確かめが働く事（注入）。
- H1b（P-B）`work.c` の ring の部分を偽の USERD と semaphore で繋ぐ小さな host 試験（新しい。H1 は node を偽にするので work.c の list の口を通らない）:
  count 1 と 32（entry 35）、ring 512 の折り返し、空きの待ち、2 s の時間切れ、all-or-nothing（空きが足りない時に何も書かない）、put が 1 回。
- H2 `plan/ws291/tests/run-gpu-job-host.sh` PASS（GPU core は変えないが job の outcome の確かめに）。
- H3a（P-A）hand-off の FIFO の単体の host 試験（新しい）: transport を偽の callback にして、順序、入口の番号までの drain、back-pressure と起床の印、
  失敗の latch、`command_sent_wait`、fork の子、引き戻し（P-C）を plain・ASan/UBSan・TSan で。
- H3b（P-A）libvulkan の結合の host 試験（新しい）: 本物の `queue.c`・`commands.c`・`context.c`・`sync.c` を ioctl の shim（GPU_COMMAND・GPU_JOB_SUBMIT・
  GPU_JOB_RESERVE・RETIRE などに決まった返事、遅れを入れられる）に繋ぎ、描く thread（fence 無しの submit を続ける）・fence を待つ thread（vkWaitForFences と
  reap）・descriptor を更新する thread の 3 本を TSan で回す。deadlock の検出は時間の上限（各 run 60 s）。thread local の mutex の数の abort が働く事も。
- build: config/ci の 19 の `$(BUILD)/vmunix` と RTX の config の vmunix が warning 0、libvulkan を amd64・rpi4・visionfive2 で warning 0。

QEMU:

- Venus の回帰（libvulkan を変える P-A・P-C）: `plan/ws291/tests/venus-reg-rtx.sh`（10.0.10.3 の Debian、p024 i03 で PASS した道）か `venus-reg.sh`。
  B-U が Venus では入らない（cap が無い）事も確かめる。nvrtx は QEMU に無いので B-K の QEMU 試験は無い。

実機（10.0.10.3 の RTX 2070、Q1 の割当と lock の中、担当が 1 本の script で）:

- 前提: WSI の chain が block-linear である事（`VULKAN WSI chain=... tiling=` の行、`wsi-swapchain.c` 1437 行、既定の `nvrtx.disp.bl` 入）。そうでなければ
  mview の pass は shadow を通り、B-K は全部 refused になる。
- `plan/ws291/tests/rtx-p024.sh` ROUNDS=2 SHOT=1 で main と各 Phase の腕を同じ window で: big-100・big-1000・subs-100・subs-1000・scene-1500・scene-5000 の
  fps と still の画素の比べ（31 の比べが max 0）。
- `plan/ws309/tests/rtx-p015.sh` PARTS='cycle scenes desk'（game mode の往復 3/3、desktop・Notes・browser の画面、frame-ops）。
- 共有の work channel の腕（P-B）: nvrtx が session に channel を作らない起動（`channels_wanted` 0 の形、`runtime.c` の channel の選び。入れ方は実装で
  token を確かめる）で、subs-100 と画素の比べを 1 本。h2 の前置きの道を通す。
- fault の注入（P-B、1 本）: 組み置きが GPU にある間の channel の停止（既存の recovery の試験の形）と session の close の後に、別の session が描ける事と
  kernel の log に chunk の越えの行が無い事。
- 新しい bench の mode（mview、P-B の範囲）:
  - `--record=once`: swapchain の image ごと・part ごとの command buffer を最初の frame の前に全部記録し（ONE_TIME_SUBMIT を付けず、Reset しない）、毎 frame
    取得した image の分を submit し直す。camera は固定（still の比べもその前提）。image ごとに「最後にその image の command buffer を出した frame の
    fence」を覚え、出し直す前にそれを待つ（mview の fence は slot ごと: renderer.c 420〜427 行。`--images=4` では slot の fence と一致しない）。
    SIMULTANEOUS_USE は使わない。組み置きの量は 4 image × 1000 part × 1 part の push で、part が 8 KiB なら session の上限 32 MiB に届く。P-A の 0 番で
    main の `push_bytes ÷ runs` から 1 part の大きさを測り、上限と mode の image の数を決める。
  - `--submit-all`: 全 part を記録してから 1 回の fence つきの vkQueueSubmit（eager End は要らない。複数の command buffer の list を実機で通す）。
    prebuild 入で画素の比べを 1 本（heap の base、segment の頭の invalidate、class の state の持ち越しは hardware でしか判らない）。P-C の受け入れにも使う。
  - heap の index の再利用: view と sampler を毎 frame 作り壊す短い試験（mview の mode か host の長い run）を 1 本。
  - NVK の基準も同じ source の同じ mode で（Debian の ~/linux-mview、Q1 の割当の window で 1 回）。
- 数の取り方: GPU の 1 run の時間は `gpu_us ÷ timed`（runs で割らない。timed が runs の何割かも出す）、GPU の使用率は「1 run の平均 × runs ÷ frame の
  時間」、1 submit の kernel の時間は runtime の `held_us ÷ frames`（runtime.c 1812〜1820 行）。timing 有り（end stamp の WFI が足される）と無しの差を
  P-A の 0 番で見ておく。
- TIMING=1 の段（vkQueueSubmit・record、hand-off の数・眠りと起床の数）と、`nvrtx.vk.perf=1` の runs・push_bytes・gpu の時間に、組み置きの submit・refused の
  理由ごと・世代の食い違い・chunk の数・heap の index の数を足して、通った道を確かめる。perf の行・log の文字列に WS・Phase・Bug の番号を書かない。
- console・serial の log で判定しない。数は mview の出力と perf の行（ssh で読む）、画面は keiland-shot の PNG。

## 13. Phase の分け方（案）と受け入れ

決定（2026-11-01 21:07 Q1 の判断、phase.md の「Q1 の判断」）: B-U は入れる（nvrtx だけ、環境変数で切れる。Q1 = A）。順は P-A → P-B で、P-A を
[p027](../phase027/phase.md)、P-B を [p028](../phase028/phase.md) にした。P-C は Future Work（Q2 = A）なので capset の bit と `include/uapi/gpu-op.h` の comment
は変えない（Q3 は不要）。mview の `--record=once`・`--submit-all` は足す（第 1 版の Q4）。

| Phase（案） | 中身 | 受け入れ |
| --- | --- | --- |
| P-A（libvulkan、約 1.5〜2 日） | 0. main で subs-1000 の GPU の 1 run の時間を測り §3.2 を置き換える。1. B-U: hand-off の場所と FIFO、`queue_enqueue` の分かれ道、入口の drain、back-pressure、失敗の latch、fork、環境変数の切り、timing の数。2. H3a・H3b | H3a・H3b PASS（TSan の警告 0、時間切れ 0）。実機の同じ window の 2 round で **subs-1000 ≥ 75.4 fps（NVK）**、subs-100 が main 比 +20 % 以上。big-100・big-1000・scene-1500・scene-5000 が main の 2 round の幅の内か上（−3 % まで）。画素の比べ 31 が max 0。rtx-p015.sh の往復 3/3、frame-ops の各段が main −2 fps の内。Venus の回帰 PASS。`VULKAN_ZEDBSD_SUBMIT_HANDOFF=0` で main と同じ fps の幅。timing の hand-off の数が subs-1000 で 1 frame 998 前後（0 でない）。0 番の計測で GPU が 1 part 13.3 µs を越えると分かれば、数の受け入れを Q1 と見直す |
| P-B（nvrtx、約 3 日） | B-K: sink の分け、heap（view・sampler の index、h1・h2）、chunk と回収、END の build と適格と refused、`nvrtx_work_channel_submit_list`、submit の list、set の世代、perf の数、`nvrtx.vk.prebuild=1`（入れる token、既定は切）、mview `--record=once`、H1 の解釈器 | H1・H2 PASS、build warning 0。実機（どの腕も P-A の libvulkan）: H1b PASS。`--record=once` の subs-1000 で、prebuild 入の 1 submit の kernel の時間（`held_us ÷ frames`）が切の半分以下、fps が切以上、GPU の使用率（`gpu_us ÷ timed` × runs ÷ frame）が入で 85 % 以上（律速が GPU に移った。§3.2 の実測では GPU の 1 part 約 9 µs と切の消費側 約 9.5 µs が近く、fps の伸びは 1 割に届かない見込みなので、fps の倍率は受け入れにしない）。`--submit-all` の prebuild 入で画素の比べ max 0。prebuild 入と切で 6 bench の fps が幅の内、画素の比べ max 0、rtx-p015.sh の往復・画面・frame-ops の後退なし。perf の数で `--record=once` の subs の part の 99 % 以上が組み置きで出た事。共有の work channel の腕と fault の注入の 1 本 |
| P-C（libvulkan、約 1 日、任意） | eager End と引き戻し、capset の bit（Q3 の答え次第）、mview `--submit-all` | H3a（引き戻し）PASS。`--submit-all` の subs-1000 で、eager End 入が切より frame −15 % 以上。compositor の frame-ops の後退なし、Venus PASS |
| P-D（任意、この設計の外） | build を node の lock の外へ、quad の record の組み置き、長い command buffer の retain（R6）、chunk と heap の読み取りだけの写し | 別に設計 |

- P-B の数の取り方は §12（1 submit の kernel の時間は `held_us ÷ frames`、GPU は `gpu_us ÷ timed` × runs ÷ frame）。P-B の範囲で `vk-command.c` 16〜17 行の
  file の comment（「submission は GPU を待つ」は今の code と違う）も直す。
- phase.md の Q1 で B-U を認めない（B）時の P-B: 腕の libvulkan は main のまま、`--record=once` の subs-1000 を同期の submit で測り、受け入れは
  「prebuild 入の 1 submit の kernel の時間が切の半分以下、fps が切の 1.2 倍以上」（同期の submit では vkQueueSubmit が組み立てを待たなくなる分だけアプリの thread の 1 part が縮む。切は record 無しで約 11 µs、入は約 5 µs の見込みで、GPU の約 9 µs が上限）と、上の後退なしの項目。
- どの Phase も既定の切り替え（§14）を残したまま入れ、受け入れの後に Q1 がユーザーに既定を入にするかを聞く（P-B の `nvrtx.vk.prebuild`）。
- WS291 の最後の「規約の全文の見直し」に、この Phase が変えた C の file を足す。
- P-A の見積もりは第 1 版の 1〜1.5 日から、H3b（libvulkan の結合の host 試験の shim は今の tree に無い）の分を足した。

## 14. 危険と戻し方

| 危険 | 見つけ方 | 戻し方 |
| --- | --- | --- |
| hand-off の順序の誤り（同期の呼び出しが前の hand-off を追い越す） | H3a・H3b の TSan と順序の試験、実機の画素の比べ、Venus | `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0`、P-A の commit の revert |
| hand-off の deadlock | H3b の 3 thread、thread local の mutex の数の abort、実機の SPIN の時間切れ | 同上 |
| 失敗が DEVICE_LOST にまとまり原因が見えない | thread の stderr の 1 行（kernel の返事の値） | 同上 |
| 組み置きの状態の違い（画素の差） | H1 の意味の状態の比べ、実機の画素の比べ | `nvrtx.vk.prebuild` を入れない（既定は切）、P-B の revert |
| chunk・heap の index の早い再利用（GPU が読んでいる間に CPU が書く） | H1 の偽の fence、実機の長い run | 同上 |
| chunk の越え書き | BUILD の sink の常の確かめ（log 1 行と refused）、H1 の注入 | 同上 |
| pool の base の扱いの誤り | H1 の h1・h2 の場合、実機の複数 command buffer の submit の画素の比べ（`--submit-all` の前は H1 だけ） | 同上 |
| 組み置きの memory の増え（多数の command buffer を持つアプリ） | perf の行の chunk の数 | session の上限 32 MiB で fallback |
| big の後退（長い記録を組んでしまう） | 6 bench の A/B | draw 256・record 2048 の上限、`nvrtx.vk.prebuild` 切 |
| 1 CPU や負荷の高い機械で hand-off が遅い | sysconf の判定、A/B | 環境変数 |

## 15. 出典と引用

- この文書の事実は zedBSD の tree（Zlib）と p024 の実機の計測。外の source の code・comment・構造は写していない。
- 比べに名を出した物（読んでいない、数は p024 の計測）: Mesa NVK（MIT、`src/nouveau/vulkan/`、vkCmd* が push buffer を直に書く形、device に 1 つの
  descriptor の表を持つ形）、Linux の drm_sched と nouveau（GPL-2.0、ring を別の thread で書く形）、NVIDIA open-gpu-kernel-modules（MIT、process ごとの VM）。
  heap の形（object ごとの固定の index）は NVK の形の名を挙げただけで、手順や値は引いていない。実装でこれらの手順や値を参考にする時は、guardrail の
  「MIT・GPL の出典のデータの引用」の規則（該当の block の直前に複数行の comment で「Procedure referenced: <版> <path> (<関数>)」「Value(s) referenced: …」、
  file の先頭に書かない、code・comment・構造・symbol の名を写さない）に従う。
- 実装で値として要る物: pool の MAXIMUM_INDEX の幅（class の header、tree の既存の引用 `clc597tex.h` と同じ Mesa の版の `Value referenced:` で引く）。GPFIFO の
  entry の形は `work.c` が既に引用済み。他の新しい外の値は見込んでいない。
- source の comment・log に開発の管理情報（WS・Phase・Bug の番号、plan/ への参照）を書かない（guardrail）。

## 改訂の記録

- 第 1 版（2026-11-01 P2、q1832-i01）。
- 第 2 版（2026-11-01 P1、q1838-i01）: レビューの blocker 2・major 7・minor 12 を反映。主な変更: drain を `vulkan_context_execute` の入口だけにし入口の番号まで
  待つ（§6.4）、texture の header・sampler を session の heap に置き pool の base を list の頭だけで書く（§5.3）、組み置きの arena を push だけの chunk と
  record の前の 32 KiB の確かめにした（§5.4）、quad・CPU copy の record を v1 で組まない（§5.5）、draw 256 以下だけ組む（§3.3）、class の前置きを
  固定の push に（§5.6）、失敗の意味の誤り（「ring だけ」）を直した（§6.7）、capset の約束が `include/uapi/gpu-op.h` にある事（§10）、見積もりに GPU の
  段と起床（§3.2）、`--record=once` の形（§12）、H1 を意味の状態の比べに、H3b（libvulkan の結合）を足した（§12）。
- 第 3 版（2026-11-01 P1、q1838-i01）: 2 回目のレビューの blocker 1・major 7・minor 11 を反映。build 中と追記先の chunk を回収から外す（§5.4）、
  GPU の 1 part を 10.0.10.3 で測り §3.2 を実測で置き換え（約 9〜10 µs、timing 有り）、P-B の受け入れを fps の倍率から kernel の時間と GPU の使用率へ（§13）、
  hand-off の道で thread を起こす（§6.1）、set の配列を伸ばせる形に（§5.5）、組み置きの run を retire に通す（§5.6）、`--submit-all` を P-B に（§12）、
  back-pressure の専用の cond（§6.5）、sync.c の直の lock の包み（§6.4）、fork の prepare・parent（§6.6）、H1b（§12）、build の失敗と pre-scan（§5.5）、
  P-C の引き戻しの条件（§7）。
