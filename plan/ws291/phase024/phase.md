<!-- awesome-plan project=zedbsd record=ws291-p024 -->

# ws291-p024: NVIDIA: subs-1000 の GPU の 1 回の実行ごとの固定の費用（約 56 µs）を内訳ごとに測って縮める

Status: in-progress（2026-10-31 21:04 P3 q1785 で着手）
Parent: [WS291](../ws.md)
依存: [p023](../phase023/phase.md)（block-linear の置き方で pass ごとの copy が消えた後）

## 目的

2026-10-31 ユーザーから、p023 の実装の後に続けて subs-1000 の固定の費用の内訳を測る、という趣旨の答えを受けた（p023 の Q2 の (b)）。
p014 の nc の診断で、copy を除いた GPU の 1 run は約 60 µs（描画は約 4 µs）、NVK は 1 submit 約 5〜7 µs。

## 範囲（案、Queue を立てる時に確定）

1. run ごとの固定の段（texture pool と shader cache の invalidate、最初の draw の全 state、pass の始めと終わりの barrier の WFI、run の終わりの WFI の fence、
   channel の切り替えの有無）を 1 つずつ外した変種で測る（p014 の rtx-runcost.sh）。
2. 外せる物・軽くできる物を直し、subs-100・subs-1000・big-1000・scene・desktop の後退と画素の比べ。HAL は変えない。

## ユーザーの指示（2026-10-31）

- 2026-10-31 ユーザーから、P3 は RTX での描画の最適化を目標の達成まで進める、という趣旨の指示を受けた。目標は p013・p014 の NVK 並み（big-1000 136.2 fps、subs-1000 75.4 fps）。
  この Queue では p024（subs の 1 run の固定の費用）に加え、big-1000 の残りの差と、zero copy の game mode での big-100 の約 20% の遅れも扱う。

## q1785-i01（2026-10-31 P3、worktree p3-ws291wsi、main b689bbf66 を ad4e8876b で取り込み）

- 承認: Queue q1785-i01（上のユーザーの指示）。実機 10.0.10.3（RTX 2070）、lock /tmp/zedbsd-rdna.lock を 21:12 から window ごとに取る。
  計測の userland は全部この tree の 14 file（`make ZEDBSD_CONFIG=plan/ws291/tests/config.mk BUILD=build/p024/base` の個別の target、
  base の kernel 3084829296 は main の RTX kernel と cksum 一致）、kernel だけを入れ替える。各 window の終わりに 14 file と NVMe の kernel を戻して
  cksum 一致（vmunix-va1 1463007375、cfg の nvrtx.vk.perf 行 0）、Debian（RTX 4 function vfio-pci）。

### 1 run の固定の費用の内訳（rtx-runcost.sh、診断 kernel、21:12〜21:25）

- 診断の差分 `plan/ws291/tests/nvrtx-run-cost-diag.patch`（今の tree 向けに作り直し、session ごとに 8 つの変種を順に）: 0 そのまま、1 push buffer の
  始めの shader cache の invalidate なし、2 barrier の WFI なし、3 run の fence の WFI なし、4 fence の membar と non-stall 割り込みなし、
  5 record を 1 つも実行しない（空の run）、6 = 1+2+3+4、7 = 5+3+4。`rtx-runcost.sh` に `TIMING=on|off` を足した。
- subs-1000（timing off、16 run、build/tmp/p024-d1、worktree の build、git の外）: 0・1・2・4・6 は 19.8〜20.6 fps（submit 42〜43.5 ms/frame）、3 は 21.7、
  **5・7（空の run）でも 23.7〜24.4 fps（submit 34.3〜35.2 ms = 1 submit 約 34 µs）**。GPU の中身を全部外しても 1 submit 約 34 µs が残る。
- subs-100（timing on、16 run、build/tmp/p024-d2）: GPU の 1 run は 0 で約 20 µs、空の run で 4 µs。executor が GPU を待った時間（blocked）は 5 s に
  0.1〜0.2 s（2〜4 %）。→ subs の律速は GPU ではなく CPU の 1 submit の道（約 34〜40 µs）。p014 の「GPU の 1 run 約 56 µs」は、copy と timed の
  end stamp の WFI を含んだ値で、今は GPU の余裕の方が大きい。

### CPU の 1 submit の内訳（libvulkan の VULKAN_ZEDBSD_SUBMIT_TIMING、rtx-p024.sh、21:25〜21:33、build/tmp/p024-ab1）

- base の subs-1000（timing 自体が 1 部分 約 2 µs かかるので fps は 12.4 に落ちる）: vkQueueSubmit 75 µs = reserve（GPU_JOB_RESERVE）7・execute 46
  （GPU_COMMAND の ioctl 41、command_ran 61923 回・deferred 24 回 = ほぼ全部が呼び手の thread で同期に走った）・finish（GPU_JOB_COMMIT）3・
  private_fence 2・reap（8677 回、1 回 16 µs）。
- c2（下の USERD の put を 1 submit に 1 回）で GPU_COMMAND の ioctl 41 → 31 µs。register window（PRAMIN）の書き 1 回は rd32 2 回と wr32 3 回で約 5 µs、
  1 submit に 3 回書いていた。register の読みが 1 回 約 2 µs かかる。

### 直したこと（nvrtx だけ、HAL・uapi・libvulkan・GPU core は変えない）

- commit 613c725f1（c5 の形）:
  - `work.c`: ring の 1 submit の entry（start stamp・segment・fence）を書いた後、USERD の put を 1 回だけ書く（今まで entry ごとに 3 回、register window で
    1 回に rd32 2 回と wr32 3 回）。session の channel の USERD の page を BAR1 の window に写し、put は 1 回の store（写せない時は今の register window）。
    手本は NVIDIA open-gpu-kernel-modules 570.144 `uvm_channel.c`（mapped USERD に put、write barrier、doorbell）。`vram.c` に `nvrtx_bo_unmap`
    （window からだけ外す）、channel の free で外す。
  - `vk-node.c`: run の後の GR の trap の register 3 つの読みを 1 ms に 1 回までに（trap は残るので見落とさない）。
  - `runtime.c`: session に worker の待ちが無い時、commit した marker をその thread で worker と同じに扱う（park か完了）。次の GPU_COMMAND が
    queued を見ずに同期で走る。
  - `vk-execute.c`: 1 つの slot の中で、前の submit の続きに次の push buffer を書く（slot に余裕のある間、slot の ticket は最新の run）。
- 未 commit（c8 の形、下の試験の後に commit）:
  - `exec-push.c`: texture header・sampler の bit field を 1 bit ずつでなく word で書く。
  - `vk-execute.c`・`nvrtx-vk-objects.h`: view の texture header と sampler の words を object に覚え、image の storage の VA が同じ間は作り直さない
    （subs の 1 submit に約 16 の texture があり、header を毎 submit 作っていた）。
  - `wire.c`: `nvrtx_put32` を little-endian の CPU で word の境界なら 1 回の store に。
- host: nvrtx-vk 94/0、vk-d3 225/0、gsp 393・qmd 733・identify PASS、bench（1000×1・100×100）の draw ごとの state の hash が base と一致、bench の executor
  1000×1 4.07 → 2.97 ms、100×100 1.00 → 0.59 ms。RTX の組と config-amd64・rpi5・ppc32-mac99・m68k-virt の vmunix warning 0。

### 1 submit の残り（診断 kernel diag3 = c5 ＋ 段の時間、`GPU_IOCTL_TIMING`、21:45〜21:50、build/tmp/p024-ab3）

- subs-1000 で 1 submit: GPU_COMMAND の kernel 21 µs（うち runtime_command 16、stream の decode と executor 15.6、executor 11.5、ring の submit 0.8）、
  GPU_JOB_RESERVE 4 µs、GPU_JOB_COMMIT 2 µs、ledger の reap の GPU_COMMAND_WAIT が平均 1 本 2 µs、ioctl の間の libvulkan の user の時間 計 約 11 µs。
- 診断 diag2（c5 ＋ GPU core の reserve・admit・action の monitor の起床を外す、正しさは保たない）: subs-1000 25.5 → 27.8 fps、subs-100 238 → 257。
  1 submit に monitor の thread を 4 回起こしていて約 3 µs。GPU core（gpu.c）の変更なので入れていない（下の提案）。

### subs-1000 の残りの差（NVK 75.4 fps = 1 submit 約 5 µs）と提案（未実装、判断が要る）

- c7 の 1 submit は約 27.6 µs。nvrtx の kernel の frame（decode・executor・ring）は c5 で約 16 µs、c7 で約 11 µs の見込み（未計測）。残りは nvrtx の
  外: 1 submit に GPU の ioctl が 4 本前後（GPU_JOB_RESERVE・GPU_COMMAND・GPU_JOB_COMMIT・ledger の reap の GPU_COMMAND_WAIT、fence の無い submit
  にも libvulkan が私用の fence の job を付けるため）と、その間の libvulkan の user の時間 約 11 µs、GPU core の monitor の起床 約 3 µs。
- 提案 1（GPU core、gpu.c）: reserve・admit・action の後の monitor の起床を、新しい期限が monitor の眠る期限より早い時と、その open の停止が待つ時
  （`gpu_stop_waiting_locked`、retirement の起床と同じ規則）だけに。diag2 で subs-1000 +9 %。plan/ws014/tests の GPU core の fixture は今の tree で
  build できない（gpu-framework.c が HAL_SPACE_* を知らない）ので、直すならその fixture の修復から。
- 提案 2（libvulkan と GPU core、WS291 の job-submit-design.md の案 A・B、2026-10-29 にユーザーが採った）: reserve・command・commit を 1 本、reap を
  1 本に。今は amdrdna が先の予定で nvrtx は対象外。
- 提案 3（libvulkan、製品の判断）: fence も signal の semaphore も無い submit に私用の fence の job を付けない（queue の後の job が順に監督する）。
  NVK と同じ 1 submit 1 ioctl に近づく唯一の道だが、WS014 の「native の submission は全部 job の監督の下」の決定を変える。

### 実機の A/B（10.0.10.3、`rtx-p024.sh`、main の 14 file、timing 無し、SPIN 8 s の game mode の mview、fps）

| kernel（window） | subs-1000 | subs-100 | big-1000 | big-100 |
| --- | --- | --- | --- | --- |
| base（main の RTX kernel 3084829296）ab2・ab4 ×2・ab5 | 19.93・17.04・19.21・19.97 | 184.1・187.9・186.7・176.0 | 109.8・110.4・111.2・110.4 | 536.3・535.8・531.9・541.5 |
| c3（slot の続き＋USERD 1 回＋marker をその場で）ab2 | 21.92 | 224.5 | 110.9 | 567.4 |
| c5（＋USERD を BAR1、GR の読みを 1 ms に 1 回 = 613c725f1）ab2 | 25.46 | 238.3 | 110.8 | 583.4 |
| c7（＋header・sampler を object に）ab4 ×2 | 29.43・29.24 | 258.7・256.3 | 109.7・113.7 | 582.4・568.8 |
| c8（＋put32 の word の store = 692209073、kernel 2191527267）ab5 | 28.72 | 258.5 | 113.3 | 586.0 |
| NVK（p010・p013） | 75.4 | — | 136.2 | — |

- 証拠: build/tmp/p024-ab2・ab4・ab5（worktree の build、git の外）の summary.txt。subs-1000 は boot ごとの揺れが大きい（base 17.0〜20.0）。
- 画素（ab5、SHOT=1）: 100 copies の still（one・each・subs）の keiland-shot を base の round 1 と比べ、c8 は 3 枚とも max 0・differ 0（2073600 画素）、
  c8 の one と subs も差 0。base の one と subs も差 0。
- 各 window の終わりに 14 file と NVMe の kernel を戻して cksum 一致（vmunix 1463007375、nvrtx.vk.perf 行 0）、Debian（RTX 4 function vfio-pci）。
- game mode の往復・画面・desktop（`plan/ws309/tests/rtx-p015.sh` PARTS='cycle scenes desk'、base と c8、22:12〜22:27、build/tmp/p024-v1）:
  - 往復: 両腕とも mview 3 回が direct=3 left=3 window=3、failed 0、refused 0、`lend images=0x3f`。
  - 画面（keiland-shot、c8 対 base）: desktop・Notes・browser とも差は右上の時計の箱（x=1881..1894、y=17..27）だけ（96〜105 画素）。
  - desktop（frame-ops）: base launch 63・type 61・output 65・drag 61・home 63・wiseview 64、c8 63・61・66・61・63・64 fps（scroll・hover・keys は入力の間隔）。後退なし。
  - 起動の dmesg の `vram-window retry` の行は base・c8 の両方に出る（前からの行）。戻し: 14 file と NVMe の kernel の cksum 一致、cfg の bl 行 0、Debian。

### 案 A・B（GPU_JOB_SUBMIT・GPU_JOB_RETIRE、[job-submit-design.md](../job-submit-design.md)）の実装（2026-10-31 Q1 の指示、q1785 の中）

- 2026-10-31 Q1 から、ユーザーが 2026-10-29 に採った案 A・B を今の main に合わせて直し、design-reviewer に通して Q1 に送り、uapi・gpu.c・libvulkan・
  nvrtx に実装する指示を受けた（他の driver は任意の op と今の道で動く。fence の無い submit も job を残す）。あわせて WS014 の GPU core の fixture を
  今の tree で動くように直す指示（ab2ddbf7c・9246cf6ea、`plan/ws291/tests/run-gpu-job-host.sh`、21 run PASS）。
- design-reviewer の 2 回目 [job-submit-review2.md](../job-submit-review2.md): blocker 1（BL1: nvrtx では OOM でも前の仕事が GPU に出ている → CANCELED は
  GPU に何も出していない時だけ）、major 6（M-A〜M-F）、minor 10。design の末尾の節に反映。今の 3 本の道の同じ欠陥は Q1 が BUG-381 に。
- kernel の部分（84f786dce）: uapi（'G' 49・50、GPU_CAP 262144・524288、`gpu_job_submit.error` は出力）、gpu.h の任意の `jobs->command`（8 driver の表に
  NULL）、gpu.c（SUBMIT は admission の中で reserve → backend の command → admit・commit か cancel、起床は最後の 1 回、admit の前に reservation の期限、
  RETIRE は名指しの job の record を 64 個まで consume、bit は backend が JOB を報告する時だけ core が足す）、nvrtx（`runtime_job_command`、frame ごとに
  queue の submit の数・成功の数・GPU に出した run の数 `gpu_sends` を数えて outcome）。host: `run-gpu-job-host.sh`（ws014 の 10 fixture と新しい
  `gpu-job-submit.c`、plain と ASan/UBSan）PASS、nvrtx vk-d3 228/0、config/ci の 19 と RTX の vmunix warning 0。host 試験に入らない所: nvrtx の
  runtime_job_command と gpu_sends（runtime.c・vk-node.c は host で build しない）。
- libvulkan（未 commit の時点の記録）: `vulkan_writer.job` を付けた queue の transaction は GPU_JOB_SUBMIT 1 本（context の mutex の中、fence の prepare と
  ledger の entry は前に作る）。ioctl の失敗・DEFERRED は何も走っていないので今の 3 本の道へ。ledger の reap は `GPU_CAP_JOB_RETIRE` の時 64 個ずつ
  GPU_JOB_RETIRE。BUG-381: 今の道で command buffer を含む submit の OOM の答えは CANCEL でなく fault（libvulkan は GPU に出たかを知れないので、
  全 driver で保守的に。transport の OOM（送る前）も device lost になる）。
- 実機（10.0.10.3、`rtx-p024.sh`、kernel 84f786dce = js-config 1912060278、libvulkan はこの節の未 commit の版 lv1、他は main の 13 file、22:47〜）:
  - js1（TIMING=1、SHOT=1）: subs-1000 の vkQueueSubmit は reserve の数 177/97455（3 本の道に戻ったのは 0.2 %）、submit 43 µs（timing 込み、
    c2 の 65 µs・base の 75 µs）、context_hold 28 µs。still の one・each・subs は ab5 の base と 3 枚とも max 0・differ 0、one と subs も差 0。
    戻し cksum 一致、Debian。
  - js2（timing 無し）: subs-1000 32.56（c8 28.72）、subs-100 299.5（c8 258.5）、big-1000 115.7、big-100 568.4 fps。
  - c8b（同じ window の直後、kernel 692209073、main の 14 file = 3 本の道）: subs-1000 27.89・subs-100 255.3 fps。案 A・B は subs-1000 +17 %・subs-100 +17 %。
- QEMU の Venus（23:00 前後、`plan/ws291/tests/venus-reg.sh`、forge の image を今の tree（kernel 84f786dce・libvulkan のこの版）で
  `build-forge-image.sh build/p024/forge` から作り、libvulkan は lv2）: 起動、forge-guest・fence-guest（600 frame）・wltest（mailbox、100 frame で
  swapchain を作り直し）・mview mailbox・mview の draw ごと・20 submit、compositor の VULKAN_ERROR 0、`venus-reg: PASS`。Venus は jobs->command を
  持たないので SUBMIT は通らず（今の 3 本の道）、RETIRE は通る（GPU_CAP_JOB を報告する backend）。
