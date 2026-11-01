<!-- awesome-plan project=zedbsd record=ws291-p026-design-review -->

# ws291-p026 設計の敵対的レビュー（design.md 第 1 版）

- 2026-11-01、design-reviewer。読むだけで、build・QEMU・実機は使っていない。
- 対象: [design.md](design.md) 第 1 版、[phase.md](phase.md)。照らした code は worktree p2-ws291g1（HEAD e4719497c5）。
- 分類: **blocker**（このまま実装すると壊れる。実装の前に直す）、**major**（設計・受け入れ・ユーザーへの問いを直す要がある）、**minor**（実装で直せばよい、または書き足し）。
- 「推測」と書いた物は code か計測で確かめていない。

## 確かめた事（設計の事実で正しかった物）

- 同じ open の ioctl は `gpu_session_enter`（`src/drivers/gpu/gpu.c` 1781 行〜）が待ち合わせで 1 本ずつにする。executor は `runtime_execute`（`runtime.c` 1783 行〜）の
  `exec_lock` と node の `lock` の下で走る。GPU_COMMAND（`runtime_command`）と GPU_JOB_SUBMIT（`runtime_job_command`）は呼び手の thread で走る。
- `GPU_CAP_JOB_SUBMIT` は `gpu.c` 2738〜2741 行で `jobs->command != NULL` の時だけ立ち、amdrdna（`runtime.c` 127 行）と ghci（`vk-node.c` 71 行）の表は NULL。
- `vkEndCommandBuffer` の保留は `command_batch_limit / 4`（64 KiB）以下（`commands.c` 548〜557 行）。Reset と Begin は次の記録の頭に載る（`commands.c` 480〜501 行）。
- `EXECUTE_FIRST_DRAWS` 256・`EXECUTE_RUN_DRAWS` 2048（`vk-execute.c` 108〜109 行、`execute_room`）。big-1000 の「GPU は submit の始めから走る」は p024 の
  submit 3.3〜3.5 ＋ fence 1.1〜1.2 ≒ GPU 4.5 ms と合う。§3.3 の結論（長い command buffer を End で組むと後退する）は正しい。
- session の channel は一度だけ決まり変わらない（`runtime.c` 1850 行 `runtime_channel`）。GPU の VA は全 session で 1 つ（`nvrtx-work.h` 38〜43 行）。
- WSI の lend は swapchain の作成の時に image ごとに import し、lend が変われば suboptimal で作り直す（`wsi-swapchain.c` 1249〜1262 行、873〜876 行）。VkImage の
  backing は変わらない。object は参照の数で生き、record が持つ参照の間は payload が残る（`vk-object.c` 166〜228 行）。
- fence の値の一周（wrap）は `nvrtx_work_channel_passed`（`work.c` 1213 行〜）が「newest より先の値は古い」として扱う。
- mview の bench（`rtx-p024.sh` 95 行、`--content=game`）は block-linear の lend（既定は `nvrtx.disp.bl` 入）で走るので、subs の pass は shadow を通らない見込み（下の 12 を参照）。

## 指摘

### 1. blocker: drain を置く所の一覧に、context の mutex の下で出る ioctl が入っていて、recording の thread と deadlock する

- 根拠: design.md §6.3 は「`sync.c`・`context.c` の直の ioctl（GPU_JOB_RESERVE・CAPACITY・ACTION・RETIRE・COMMAND_WAIT・GPU_JOB_SUBMIT）の前で
  `handoff_pending` が 0 になるのを待つ」とする。ところが
  - GPU_JOB_RESERVE は `sync.c` 648 行で `context->mutex` を取った後の 664 行、
  - GPU_JOB_COMMIT・CANCEL（ACTION）は `sync.c` 760〜762 行で `context->mutex` の下、
  - GPU_JOB_RETIRE（`sync_notifications_retire` 1838 行）と reap の GPU_COMMAND_WAIT（1903 行）は `sync_notifications_reap_if` 経由で、呼び手が
    `context->mutex` を持つ（`sync_notification_wait` 1974 行、`vulkan_sync_job_reserve` 648 行、`vulkan_sync_reap_locked` は context.c 898 行）、
  - GPU_JOB_SUBMIT（`context.c` 904 行）は `vulkan_context_transaction` の中、つまり `vulkan_context_execute` が取った `context->mutex`（516 行）の下。
  recording の thread は hand-off を送るのに `vulkan_context_execute` で同じ `context->mutex` を取る。
- 起こる状況: 描く thread が fence の無い submit を hand-off し続ける間に、別の thread が vkWaitForFences を呼ぶ → `sync_notification_wait` が
  `context->mutex` を取り ledger が閾値を越えていれば RETIRE → その前の drain が `handoff_pending` の 0 を待つ → recording の thread は `context->mutex` を
  待つ → 両方止まる。3 本の呼び出しの道（JOB_SUBMIT が DEFERRED の時の `vulkan_sync_job_reserve`）でも、同じ context の別の VkDevice（device の
  mutex は device ごと、context は共有され得る: `vulkan_kernel_error` の「every device sharing this context」）が hand-off していれば同じ。
  §6.4 の「drain の待ちは context の mutex を持たずに」と §6.3 の一覧が食い違っており、一覧どおりに入れると deadlock する。
- 直し方の案: drain は `vulkan_context_execute` の入口（`context->mutex` を取る前）だけに置く。JOB_SUBMIT・GPU_COMMAND・GPU_COMMAND_SUBMIT は
  全部ここを通るので、kernel が見る仕事の順はこれで保たれる。RESERVE・ACTION・RETIRE・COMMAND_WAIT は仕事を出さず順に頼らないので drain しない
  （そう書き、理由を残す）。`context->mutex` を持ったまま drain に入ったら abort する確かめ（mutex の持ち主の印か thread local の深さ）を入れ、H3 に
  「別の thread の fence の待ちと reap」の場合を足す（指摘 6）。

### 2. blocker: granule（16 KiB、push 10 KiB・内部の quad の constants 4 KiB）が、今の書き手が頼る「1 record の最大」を入れられない

- 根拠: 今の executor の method の書き手（`nvrtx_exec_*`・`nvrtx_push_draw_*`）は `push + at` に境を見ずに書く。安全は `execute_room`
  （`vk-execute.c` 935〜969 行）が record の前に最大の分（`EXECUTE_RECORD_ROOM` 32 KiB、constants `NVRTX_VK_REGIONS_MAX`(16) × `EXECUTE_CONSTANTS_SLOT`
  (0x500) = 20 KiB、pool の entry `REGIONS_MAX + TEXTURES_MAX` = 24）を空けておく事だけで守られる。`execute_constants`・`execute_pool_entry` は
  ENOSPC を返すが、push には確かめが無い。design.md §5.3 の granule は push 10 KiB・constants 4 KiB（3 quad）で、この前提の空きを一度も満たせない。
  §5.3 の「どれかが満ちたら segment を閉じ」は record の途中で閉じる事になるが、今の code は record の途中で push buffer を替えられない
  （target・held・quad の状態が続く）。
- 起こる状況: (a) 境を record の前の見積もりで確かめる形にすると、region が 4 つ以上の copy・blit・buffer と image の copy（quad が 4 つ以上）を持つ
  command buffer で、quad の constants か push が granule を越えて隣の granule（別の command buffer の、GPU が読んでいる最中の物でありうる）を CPU が
  上書きする（kernel の memory の越え書き、他の command buffer の GPU の読みを壊す）。(b) 越えないように record ごとに 32 KiB を求めると、10 KiB の
  push では全 record が「満ちた」になり何も組めない。
- 直し方の案: granule の大きさを record の最大から決める（push は少なくとも `2 × EXECUTE_RECORD_ROOM`、constants は `REGIONS_MAX × 0x500` を入れる）か、
  granule を可変の長さ（arena の続きの 64 KiB 単位など）にする。または record ごとの正確な上限（record の種類と region の数から）を計算する関数を
  定義し、BUILD の sink は record の前にそれで確かめ、足りなければ次の granule へ移ってから record を書く（record を跨がない）と明記する。
  どの形でも、sink の push の書きに debug の境の確かめ（越えたら build を refused）を入れ、H1 に「16 region の copy」「16 texture の draw が連続」の
  場合を足す。

### 3. major: §6.6 と Q1 の「B-K の後は非同期の部分が ring だけ」は subs の形では成り立たない

- 根拠: design.md §3.2 の表の B-U ＋ B-K の行自身が「組み立ては END、ring は QUEUE_SUBMIT、同じ transaction」とする。subs は毎 frame 記録し直すので、
  hand-off の transaction は RESET・BEGIN・records の decode・END（build）・QUEUE_SUBMIT を全部持つ（`queue_write_pending` → §6.2.1）。
  非同期に走るのは kernel の仕事の全部で、B-K で変わるのは「build の確保の失敗が fallback になる」事だけ。fallback の executor の確保
  （`execute_slot_make`、`execute_shadow` の `nvrtx_vk_gpu_alloc`）と record の decode の確保も非同期に残る。
- 起こる状況: ユーザーは Q1 の A を「B-K の後は失敗がほぼ GPU を失った時だけ」と読んで選ぶが、実際の差は B-K の有無でほとんど変わらない。
  （定常では確保が起きにくいので、OOM が DEVICE_LOST になる場面が稀なのは B-K と無関係に言える。）
- 直し方の案: §0.2・§3.2 の (iii)・§6.6・phase.md の Q1 の A の「失」を、「非同期になるのは submit の kernel の仕事の全部。定常の frame では kernel の
  確保は起きないので、遅れて見える失敗は実際にはほぼ GPU を失った時と初めの frame の確保だけ（B-K の有無によらない）」に直す。
  P-C（eager End）を使っても END は非同期なので同じ。

### 4. major: native の capset の flag は `include/uapi/` に書かれている（§0.5・§10・Q3 の前提が違う）

- 根拠: `include/uapi/gpu-op.h` 1869〜1880 行の comment が「the word at byte 172 of the 176-byte native capset (magic 0x5a4e4154 at byte 168)」の
  bit 0 の約束を述べている。bit 1（SYNC_COMMAND）はそこに無い（今の記述の漏れ）が、native の capset の word は UAPI の文書の一部として扱われている。
- 起こる状況: Q3 の背景「`include/uapi/` の外」を信じて答えると、P-C で bit 2 を足す時に uapi の comment を直す（`include/uapi/` の変更）か、uapi の
  記述と実装が食い違ったままになる。§0.5 の「`include/uapi/` も変えない」とも矛盾する。
- 直し方の案: §10 と Q3 を「native の capset の word は `include/uapi/gpu-op.h` の comment が約束を記す。bit 2 を足すなら同じ comment に bit 1・2 を書き足す
  （uapi の file の文書の変更。構造体と ioctl は変えない）」に直し、ユーザーにそれを含めて聞く。代わりの案（環境変数だけ）も今どおり並べる。

### 5. major: 1 つの ring の書きに複数の segment を並べると、texture の header・sampler の pool の base が WFI なしに替わる（推測を含む）

- 根拠: 今は pool の base（`nvrtx_push_draw_textures`、`draw-push.c` 895〜921 行）が替わるのは slot が替わる時だけで、その前の run の fence は idle を
  待つ（`vk-execute.c` 22〜23 行の「each after the one before has finished (its fence waits for idle)」）。slot の続き（`execute_continues`）は同じ base を
  書き直す。design.md §5.5 の list は、1 回の ring の書きに「command buffer ごと・granule ごとの segment」を並べ、各 segment の頭で別の base を
  `M_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI`・`..._SAMPLER_CACHE_NO_WFI` だけで設定する。前の segment の draw が GPU の中でまだ texture を読んでいる間に
  pool の base が替わる。
- 起こる状況（推測）: SET_TEX_HEADER_POOL が描画の流れで版を持たない（pipeline 中の前の draw にも効く）なら、前の segment の draw が次の segment の
  pool の index で header を読み、画素が壊れる（間欠的で、負荷に依る）。NVK は descriptor の heap を context に 1 つ置き、base を滅多に替えない形
  （読んでいないので推測）。
- 直し方の案: (a)（推し）session に 1 つの大きな header・sampler の pool（slot と同じ coherent な system memory）を置き、granule は index の範囲を
  借りる形にして pool の base を session の間変えない。method も減る。(b) list の segment の境に WFI を入れる（費用は 1 run の fence と同じ程度）。
  どちらにするかを実装の前に決め、H1 では区別できないので実機で「複数の command buffer の 1 回の submit」の画素の比べを受け入れに入れる。

### 6. major: 試験の計画が、上の 1・2・5 と内部の quad の誤りを見つけられない

- 根拠と状況:
  - H1 の「method の列が address（pool・segment の分け目）を除いて同じ」は、作りの上で成り立たない。prebuilt は granule の境で segment を閉じ全 state を
    書き直し、今の executor は 256 draw の早出しと slot の続きで閉じる。texture の handle の値（header の index）も、granule では 0 から、slot の続きでは
    `execute->headers` から始まる（`execute_pool_entry` 2113 行）。列の比べは、正しくても落ちるか、ゆるめて誤りを通す。
  - 内部の quad は corners を `execute->va + EXECUTE_CORNERS_AT`（`vk-execute.c` 2914 行）から読む。§5.3 の granule の並びに corners（と zero page）が無い。
    sink が `va` を granule のものにすれば quad は granule の中のごみを頂点として読む。H1 は address を比べから外すので見つけない。mview の subs は
    transfer の quad を使わないので実機の画素の比べも通る。
  - H3 は hand-off の queue の新しい file を偽の transport で試すだけで、`queue.c`・`sync.c`・`context.c` の lock（指摘 1）は通らない。
  - 共有の work channel（`channels_wanted` 0、`runtime.c` 1880・1926 行）での前置き（§5.5.2）の道に実機の腕が無い。
- 直し方の案:
  - H1 の比べを「method の列」から「draw・clear・quad ごとの GPU の状態の写し」に替える: host で method の列を読む小さな interpreter（3D class の
    register の写し、CB0 への load、pool の base と entry を解いた header・sampler の中身、vertex stream の address の中身（corners なら 6 頂点の値））を作り、
    各 draw の時点の意味の状態を比べる。address は「指す中身」で比べる。
  - corners と zero page は granule に置かず session の slot 0 の物（`slots[0].va`、session の間生きる）を使うと §5.3 に明記し、H1 に transfer・blit・
    clear_color_image を入れる。
  - H3 とは別に、本物の libvulkan（`queue.c`・`commands.c`・`context.c`・`sync.c`）を ioctl の shim（偽の kernel）に繋ぐ host の結合試験を P-A に入れ、
    描く thread・fence を待つ thread・descriptor を更新する thread の 3 本を TSan で回す。
  - 実機の受け入れに「`nvrtx.vk` の channel を作らない起動（共有の work channel）」の腕を 1 本（短い bench と画素の比べ）足す。
  - fault の注入（prebuilt が GPU にある間の channel の停止・session の close、granule の再利用）を H1 か実機の 1 本に足す。

### 7. major: drain が「0 になるまで」なので、別の thread の同期の呼び出しが描く thread に飢えさせられる

- 根拠: §6.3 は `handoff_pending` が 0 になるのを待つ。§6.4 の back-pressure で描く thread は 64 個の場所を埋め続けられ、その間 0 にならない。
  recording の thread は process に 1 つ（`commands.c` 38〜44 行）で、context をまたいで 1 本の FIFO。
- 起こる状況: 描く thread が 1 frame に 1000 回 hand-off する間、別の thread の vkUpdateDescriptorSets・vkCreate*・vkMapMemory 以外の wire の呼び出しは、
  描く thread が fence のある submit を出すまで（約 1 frame）待つ。別の VkDevice（別の GPU の context でも）の長い記録の batch や hand-off は同じ FIFO の
  後ろに並ぶ。
- 直し方の案: drain は「入口の時点の hand-off の通し番号まで送られた」を待つ（context ごとの enqueue の番号と sent の番号の 2 つの数）。R7 の
  「thread を増やしても並ばない」は同じ open の中だけの話なので、context ごとの FIFO（thread は 1 つのままでよい、context を順に回す）も検討に書く。

### 8. major: §3.2 の見積もりと P-A・P-B の受け入れの数が、GPU の 1 run の時間と back-pressure の起床を数えていない

- 根拠:
  - p024 の計測（phase024 の 39〜41 行）: subs-100（1 run 160 draw）で GPU の 1 run 約 20 µs、空の run 4 µs。ここから 16 draw の part は GPU 約 5〜6 µs と
    推せる（推測、timing 有りの値）。
  - §3.2 は「生産側（アプリ 約 7 µs）の方が速いので thread の queue は空にならず、起床の費用は最初の 1 回だけ」とするが、生産側が速いなら queue は
    満ち、§6.4 の規則（半分空くまで待つ）で描く thread が 1 frame に約 1000 ÷ 32 ≒ 30 回眠り、そのたびに recording の thread が起こす（futex の wake の
    system call と起床の遅れ）。眠りと起床が要るのは消費側でなく生産側。
- 起こる状況（推測）: P-B の `--record=once` では、prebuild 入の消費側は約 4.5 µs/part（ring と ioctl の固定の費用）で、GPU の約 5〜6 µs/part に
  律速が移る。切（約 9.5 µs）との比は最大でも 1.6〜1.7 で、fence の WFI や起床の費用で 1.5 を割りうる。P-A の 75.4 fps（13.3 µs/part）は届く
  見込みが残るが、余裕の根拠は書かれていない。
- 直し方の案: §3.2 に「GPU の 1 run（part）の時間」の列を足し、P-A の前に main で `nvrtx.vk.perf=1` の gpu_us と runs から subs-1000 の 1 run の GPU の
  時間（timing の有無の両方）を測って律速の式（max(アプリ, thread, GPU)）で見積もり直す。P-B の受け入れは「入が切より速く、律速が GPU か ring に
  移った事を perf の数で示す」か「1 part の thread の時間が半分以下」の形にする。back-pressure は「眠っている側だけを起こし、半分で起こす」を両側に
  当てる書き方にし、起床の回数を timing に数える。

### 9. major: `--record=once` の bench の形が mview の今の記録の仕方と合わない

- 根拠: `renderer.c` 393〜436 行は part ごとに `renderer_begin`（`vkResetCommandBuffer` と `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` で Begin、
  2101〜2111 行）、`renderer_record(..., image, part)` で取得した swapchain の image の framebuffer に描き、camera を push constants（2661・2702 行）で
  記録に入れる。command buffer は slot × part の数だけ（1467〜1478 行）で、swapchain の image ごとには無い。
- 起こる状況: 「最初の frame だけ記録し後は submit し直す」をそのまま作ると、次の frame で別の image を取得した時に古い image へ描く（画面の比べと
  frame の意味が壊れる）。ONE_TIME_SUBMIT を付けたまま出し直すのは Vulkan の使い方の誤り。camera が止まるので負荷も今の bench と別物。
- 直し方の案: §12 に「`--record=once` は image ごと・part ごとの command buffer を最初に記録し、ONE_TIME_SUBMIT を付けず、Reset しない。camera は
  固定（still の比べもその前提）」と書く。NVK の基準も同じ source の同じ mode で取る（今の書きどおり）。

### 10. minor: 共有の work channel の前置きは submit の時に確保と待ちをする（§5.9 の「submit では確保しない」は違う）

- 根拠: 前置きは「今の slot」に書く。slot の続きが無い時は `execute_start`（`vk-execute.c` 726〜783 行）が新しい slot を `execute_slot_make` で確保し
  （ENOMEM がありうる）、無ければ一番古い slot を `nvrtx_vk_gpu_wait` で待つ（node の lock を一度離す）。`nvrtx_vk_gpu_state_kept` は共有の channel で
  常に偽なので、共有の時は毎回の submit がこの道を通る。
- 起こる状況: 共有の時の prebuilt の submit は「ring だけ」でない。失敗は ring に出す前なので BUG-381 の規則は保たれる（今の失敗の道と同じ）。
- 直し方の案: §5.5・§5.9 に「共有の channel では submit ごとに前置きを slot に書き、slot の確保と待ちがありうる（失敗は ring の前で拒否）」と書く。
  前置きを session に 1 つの固定の小さな run（class の初期の state だけ、作った時に 1 回書く）に置けば、submit の時の確保と書きは無くなる。

### 11. minor: list の長さの上限と、all-or-nothing の言い方

- 根拠: §5.2 の `NVRTX_WORK_LIST_MAX` は 32、1 つの command buffer の segment は 16。複数の command buffer の vkQueueSubmit で 32 を越えれば ring の書きは
  分かれる。§5.9 の「prebuilt だけの vkQueueSubmit は 1 回の ring の書き」は常には成り立たない。
- 直し方の案: 「list が上限に達したら出し、次の list を始める。2 回目以降の失敗は `gpu_sends` の規則で EIO」と書く。`class_ready` と kept の更新は
  ring の書きが成功した時だけ（§5.5.3 の順を明記）。

### 12. minor: B-K が効く条件（block-linear の target）を受け入れと perf に書く

- 根拠: pitch-linear の色に深度のある pass は shadow（`vk-execute.c` 1145〜1149 行）で、§5.4 は shadow を refused にする。WSI の image は
  `vulkan_wsi_layout_choose`（`wsi-image.c` 103〜155 行）で両端が block-linear を取れる時だけ block-linear で、そうでなければ linear。
  `nvrtx.disp.bl=0` や block-linear を出せない表示では mview の全 part が refused になる。
- 直し方の案: §12 の実機の腕に「WSI の chain の tiling（`VULKAN WSI chain=... tiling=` の行、`wsi-swapchain.c` 1437 行）が block-linear である事」を
  前提として書き、perf の行に refused の理由ごとの数（shadow・CPU copy・video・kind・上限・確保）を出す。

### 13. minor: 長い記録の上限（2048 record）と早出し（256 draw）の間で GPU の開始が遅れる

- 根拠: 今は 256 draw で最初の push buffer を GPU に出す。B-K は 2048 record 以下を End で全部組み、submit で一度に出すので、256 < draw ≤ 約 700（1 draw に
  約 3 record）の command buffer では GPU の開始が組み立ての分（0.15 µs/draw × draw）遅れる。subs・big・scene の 6 bench はこの帯に入らない見込み
  （big は record が多すぎ、scene は draw が少ない: 推測）。
- 直し方の案: 適格を「draw が `EXECUTE_FIRST_DRAWS` 以下」か「record ≤ 2048 かつ draw ≤ 256」にし、帯の外は今の executor のままとする。

### 14. minor: granule の free list と ticket

- 根拠: §5.3 は「free list は ticket 順、先頭の ticket が passed なら使う」とするが、Reset の順は submit の順と限らないので、挿入で順を保つ規則が無ければ
  先頭が終わっていないだけで新しい run を取り、上限 32 MiB で fallback に落ちる。slot は終わった ticket を `nvrtx_vk_gpu_retire` で 0 にする
  （`vk-node.c` 662〜698 行）が、granule には同じ扱いが書かれていない。subs-1000 の inflight 2 では 2000 × 16 KiB = 32 MiB で上限にちょうど届く。
- 直し方の案: free list へ返す時に fence の順に挿入する（同じ channel の fence は単調）か、取る時に先頭からいくつか見る。passed を見た granule の ticket は
  0 にする。上限は inflight 2 を見込んで決めるか、perf の行に arena の大きさと fallback の数を出す（§14 の表にある）。

### 15. minor: hand-off の thread の側で触ってよい物を決める

- 根拠: 今の recording の thread は `sent_error`・`sent_pending`・`batch->waiting` だけを `command_sender_lock` の下で書く（`commands.c` 1221〜1231 行）。
  `vulkan_command_pending_read`（`commands.c` 733〜785 行）は `end_pending` が真の時だけ読み、`command->state`・`error` を書く。§6.2 は hand-off の時に
  `end_pending` を偽にするので、この関数は thread では使えない（§6.2 は返事の数と opcode を場所に写すと書いており、その方向は正しい）。
- 直し方の案: thread は command buffer の `state`・`error` を直に書かず `sent_error` だけに置き、次の Begin・End・Free の `command_sent_wait` が拾うと明記する。
  「thread が眠っている時だけ起こす」の眠りの印は `command_sender_lock` の下で読み書きする（起こし損ね）。timing の数（`vulkan_timing_add`）は今も 2 つの
  thread から触るので、TSan の結合試験で見る。

### 16. minor: session の close の順に頼っている事を書く

- 根拠: `nvrtx_vk_session_close`（`vk-session.c` 76〜77 行）は `nvrtx_vk_objects_close` の後に `nvrtx_vk_execute_close`。command buffer の destroy
  （`command_destroy` → `nvrtx_vk_command_reset`）が granule を execute の free list に返すのは、この順で execute がまだある時だけ安全。
- 直し方の案: §5.7 に「granule を返す処理は `session->execute` が NULL なら何もしない（run は execute_close が返す）」と書き、その確かめを code に入れる。

### 17. minor: drain しない raw の ioctl を一覧にして理由を書く

- 根拠: §6.3 の一覧に無い raw の ioctl: `memory.c` 363〜367 行（vkFreeMemory の blob の GPU_RESOURCE_DESTROY、native の free より先）・446〜456 行（mmap）・
  648〜1003 行（export・import・map）・1287 行（GPU_BLOB_CREATE_PLACED）、`external-fence.c` の GPU_FENCE_*、`wsi-image.c` 121・472 行、
  `wsi-swapchain.c` 3478・3665 行。どれも仕事を出さず、正しいアプリでは保留の hand-off と順に頼らない（kernel の VkDeviceMemory は backing の参照を持つ:
  `vk-node.c` 127〜129 行）と読めるが、設計に書かれていない。
- 直し方の案: §6.3 に「drain するのは `vulkan_context_execute` の入口だけ。上の raw の ioctl は仕事を出さず、hand-off の中の仕事が読む物を壊さない
  （理由）」を書き、P-A の review の確かめ項目にする。

### 18. minor: fork の後の子と、hand-off をまとめる時の失敗

- 根拠: `handoff_pending` は process の memory にあり、fork の子には thread が無い（`command_sender_start` は pthread_once で 1 回）。§6.2.3 の「続きの
  場所を 1 つの transaction にまとめる」は、途中の hand-off の失敗の後ろの hand-off が kernel で走りうる（stream の decode が command ごとに続く場合）。
  256 KiB にまとめると `GPU_COMMAND_MAX - 128` を越え、stream の blob への写し（`context.c` 841〜853 行）が入る。
- 直し方の案: pthread_atfork で子の数を 0 にし hand-off を切る（または子では drain しない）。まとめる時は合計を `GPU_COMMAND_MAX - 128` 以下に抑え、
  途中の失敗の後は context を lost にする（今の書きと同じ）と明記する。

### 19. minor: security の緩和の案（任意）

- 根拠: §8 のとおり共有の VA で shader が他の session の granule を書けば、壊れた push buffer が繰り返し走る（slot より長く残る）。
- 直し方の案（推測、`nvrtx_work_map_system` に読み取りだけの写しの口があるか未確認）: granule の run を GPU からは読み取りだけの PTE で写せば、
  shader の書きを MMU が拒む。push・pool・quad の constants は GPU が読むだけ。Future Work の候補として §8 に書き足す。

### 20. minor: Phase の分け方と Q2

- 根拠: §13 の P-B の受け入れは「どの腕も P-A の libvulkan」で測る。phase.md の Q2 の B（P-B → P-A）では P-B の受け入れを書いたとおりには測れない。
- 直し方の案: Q2 の B に「P-B の受け入れは P-A の前は `--record=once` を同期の submit で測る（数を書き直す）」を足すか、B を外す。

### 21. minor: 書きの細部

- §5.6 の set を書く所は `layout_update_write`（`vk-layout.c` 686 行）、copy は `layout_update_sets`（633 行）、free は `layout_free_sets`（586 行）・
  `layout_pool_sets_free`（466 行）・`layout_reset_pool`（430 行）、slot を空ける `layout_slot_clear`（868 行）。名前を合わせる。
- §5.2 の pool の entry の数は `nvrtx_push_draw_textures` に granule の entry の数（32）を渡す（今は `EXECUTE_POOL_ENTRIES` 2048）と書く。
- §1 の main の SHA（d4b741303）と worktree の HEAD（e4719497c5）が違う。読んだ物の版を合わせる。
- `vk-command.c` 16〜17 行の file の comment（「A submission runs the records at once and waits for the GPU」）は今の code（待たない）と合っていない。
  B-K の実装で同じ file を直す時に一緒に直す。

## 出典の規則（guardrail）について

- design.md §15 の書き方（code・comment・構造を写さない、block ごとの「Procedure referenced:」「Value(s) referenced:」、file の先頭に書かない）は
  guardrail の「MIT・GPL の出典のデータの引用」と合っている。設計の文書に外の source の code の写しは見当たらない。
- 実装で足す perf の行・log の文字列に WS・Phase・Bug の番号を入れない事（guardrail「source の中の開発の管理情報」）を §12 か §13 に一行足すとよい。

## 要約

- blocker 2: (1) drain の置き場所の一覧が context の mutex の下の ioctl を含み、recording の thread と deadlock する。(2) granule の大きさが今の書き手の
  安全の前提（record の前に最大 32 KiB・constants 20 KiB を空ける）を満たせず、越え書きか全 refused になる。
- major 7: (3) Q1 の「B-K の後は非同期が ring だけ」は違う、(4) native の capset の word は `include/uapi/gpu-op.h` に書かれている、(5) list の segment の境で
  pool の base が WFI なしに替わる（推測）、(6) 試験の計画が 1・2・5 と内部の quad の corners の誤りを見つけられない、(7) drain が 0 を待つので飢える、
  (8) 見積もりと受け入れが GPU の 1 run と back-pressure の起床を数えていない、(9) `--record=once` が mview の image ごとの記録と合わない。

## 第 1 回の指摘の反映（design.md 第 2 版、2026-11-01 P1、q1838-i01）

| # | 反映 |
| --- | --- |
| 1 | drain は `vulkan_context_execute` の入口だけ（§6.4）。RESERVE・ACTION・RETIRE・COMMAND_WAIT は drain しない理由を書いた。thread local の mutex の数の abort（sync.c の直の lock は包みに替える、第 2 回の N10） |
| 2 | 組み置きの chunk は push だけ、record の前に `EXECUTE_RECORD_ROOM` の確かめ、確保に 32 KiB の余白（§5.4） |
| 3 | §6.7 と phase.md の Q1 を「非同期は submit の kernel の仕事の全部、B-K の有無で変わらない」に |
| 4 | §10 と phase.md の Q3 を「約束は `include/uapi/gpu-op.h` の comment、bit を足すなら同じ comment に書き足す」に |
| 5 | session の heap（view・sampler ごとの固定の index）、pool の base は list の頭の固定の push だけが書く（§5.3） |
| 6 | H1 を意味の状態の比べに、H1b（work.c の list）、H3b（libvulkan の結合と TSan の 3 thread）、共有の work channel の腕、fault の注入、`--submit-all` の画素の比べを P-B に（§12） |
| 7 | drain は入口の番号まで（§6.4） |
| 8 | §3.2 に GPU の段と起床、根拠の計測を名指し。第 2 回の N1 の後、10.0.10.3 で測った値で置き換えた（§3.2） |
| 9 | `--record=once` は image ごと・part ごと、ONE_TIME_SUBMIT 無し、camera 固定、image ごとの fence（§12） |
| 10 | class の前置きは heap の run の固定の push h2（§5.3・§5.6） |
| 11 | list の上限で分け、2 回目以降の失敗は `gpu_sends` の規則、更新は ring の書きの成功の後（§5.6） |
| 12 | §12 の前提（WSI が block-linear）、perf の refused の理由ごとの数 |
| 13 | draw 256 以下だけ組む（§3.3・§5.5） |
| 14 | chunk の表を順に見る、build 中と追記先を回収から外す（第 2 回の B1）（§5.4） |
| 15 | thread は `sent_error` だけ、印は `command_sender_lock` の下、back-pressure は専用の cond（§6.3・§6.5） |
| 16 | chunk と heap の解放は `session->execute` が NULL なら何もしない（§5.3・§5.4） |
| 17 | drain しない raw の ioctl の一覧と理由（wsi-display.c も）（§6.4） |
| 18 | fork は prepare・parent・child、まとめる形は P-A では入れない（§6.3・§6.6） |
| 19 | 読み取りだけの PTE を Future Work の候補に（§8） |
| 20 | phase.md の Q2 を並べ直し、B-U を認めない時の P-B の受け入れを §13 に |
| 21 | 名前を合わせた。`vk-command.c` 16〜17 行の file の comment は P-B で直す（§13） |

## 第 2 回のレビュー（design.md 第 2 版、2026-11-01 design-reviewer、HEAD c74cd26650）

- 読むだけ。照らした code は worktree p1-ws291p026。分類は第 1 回と同じ。
- 確かめた事: view の header は memory の attach が 1 回だけなので view の寿命の間変わらない（vk-memory.c 116〜130・170〜191）。set の slot と record は
  object の参照を持つ。handle は header 20 bit・sampler 12 bit。fence は `OBJECTS_SEM_RELEASE_WFI`（objects.c 572）で、ring の書きの間の pool の base の
  替わりは安全。256 KiB の chunk は ring の 1 entry に入る。仕事を出す ioctl は全部 `vulkan_context_transaction` の中で、入口は `vulkan_context_execute`
  だけ（descriptor の更新・WSI の present も通る）。sync.c の wait は context の mutex を持たずに眠るので、第 1 回の指摘 1 の deadlock は第 2 版の置き場所では
  起きない。HAL・`include/uapi/`（P-A・P-B）を変えないという主張は正しい。
- 第 1 回の 21 件: 解決 12、部分的 6（1・6・16・17・18・21）、design.md では解決したが phase.md で未解決 2（3・4）、未解決 1（20）。

| # | 分類 | 指摘 | 根拠 | 反映（design.md 第 3 版） |
| --- | --- | --- | --- | --- |
| B1 | blocker | build 中の chunk が回収の候補になり、同じ build の前の segment を CPU が上書きしうる | §5.4 の回収の規則に build 中の chunk の扱いが無い | segment を開いた時に live を増やす、追記先を候補から外す、H1 の場合（§5.4） |
| N1 | major | §3.2 の GPU 5.6 µs の根拠（subs-100 の 1 run 160 draw）が誤り。subs の 1 run は約 16 draw で、timing 有り 約 20 µs | rtx-cmdprof.sh 31 行・kab.list、renderer.c 2665〜2672 | 10.0.10.3 で測り、§3.2・§13・Q1 を測った値で書き直した（§3.2） |
| N2 | major | hand-off の条件「recording の thread が動いている」が subs では成り立たない（thread は長い記録の時だけ起きる） | commands.c 1026 行の `pthread_once` | hand-off の道も `pthread_once` を呼ぶ、入った数を数える（§6.1） |
| N3 | major | set の上限 16 が bench とちょうど同じ（推測） | renderer.c 2681〜2698 | 固定の上限をやめ、伸ばせる配列に（§5.2・§5.5） |
| N4 | major | phase.md の要点・表・Q1〜Q3 が第 1 版のまま | phase.md 45・70・86〜87 行 | phase.md を第 3 版に合わせて書き直した |
| N5 | major | GPU の時間の受け入れが測れない（retire が slot の替わりの時だけ、組み置きの run は数えられず trap の確かめも漏れる）。runs で割ると誤る | vk-node.c 662〜698、vk-execute.c 776 | 組み置きの run を小さな輪から retire に通す（§5.6）、数の取り方（`gpu_us ÷ timed`、`held_us ÷ frames`）（§12・§13） |
| N6 | major | 複数の command buffer の list を実機で通すのが P-C だけ | §12・§14 | `--submit-all` を P-B に移し、prebuild 入で画素の比べ。heap の index の再利用の試験（§12） |
| N7 | major | back-pressure の起床が `command_sender_done` を共用すると 1 件ごとに起こされる | commands.c 1228 行 | queue ごとの専用の cond（§6.5） |
| N8 | minor | H1 は work.c の list の口を通らない（`entries[4]` の固定の配列） | work.c 1065 行 | H1b（§12）、entry の配列を広げる（§5.6） |
| N9 | minor | chunk の後の確かめは越え書きを防げず、説明も誤り | §5.4 | 前の確かめだけが守ると明記、確保に余白、後の確かめは診断（§5.4） |
| N10 | minor | thread local の mutex の数が sync.c の直の lock を数えない | sync.c の 11 か所 | 包みに替える、abort の前に lock の道を一覧で確かめる（§6.4） |
| N11 | minor | fork の handler は prepare と parent も要る | — | prepare で lock を取り parent・child で離す（§6.6） |
| N12 | minor | heap の index の解放に execute が無い時の守りが無い。`nvrtx_vk_command_reset` は session を受けない | vk-session.c 76〜83、vk-command.c 110 | 守りを書いた、引数を足す（§5.3・§5.4） |
| N13 | minor | free・reset が slot の関数を通るという記述は事実と違う（結論は保たれる） | vk-layout.c 430〜624・880〜906 | §5.7 の文を直した |
| N14 | minor | build の失敗を全部の error に広げる | vk-execute.c 1485〜1488・2040〜2042・1903〜1914 | 0 以外は全部 refused（§5.5） |
| N15 | minor | compositor が毎 frame build を途中まで走らせて捨てうる | — | opcode の pre-scan（§5.5） |
| N16 | minor | `--record=once` は image ごとの fence を要する。memory の量 | renderer.c 420〜427 | image ごとの fence、P-A の 0 番で 1 part の大きさ（§12） |
| N17 | minor | list の頭を上限に数えるか、`VULKAN_HANDOFF_COMMANDS` を越える時、latch の時の印、present の drain の遅れ | — | §5.6・§6.1・§6.4 に書いた |
| N18 | minor | P-C の引き戻しが FIFO の順を壊しうる | §7 | 後ろに同じ context の項が無い時だけ引き戻す（§7） |
| 第 1 回 7 の補足 | minor | 飢えの上限に 256 KiB の batch の分を数えていない | commands.c 22 行 | §6.4 に書いた |
| 第 1 回 17 の補足 | minor | wsi-display.c の raw の ioctl が一覧に無い | wsi-display.c 1460〜1479・1873 | §6.4 に足した |
| 記録 | minor | design.md の「反映の対応は design-review.md の末尾」に表が無かった | — | この節を足した |
