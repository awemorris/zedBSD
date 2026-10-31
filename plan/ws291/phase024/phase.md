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
  GPU_JOB_RETIRE。BUG-381: 一度 libvulkan で全 driver の OOM を fault にする保守的な形を入れたが、Q1 の指摘（一時の OOM が全 driver で device lost）で
  取りやめ、答えを返す driver が「GPU に出した後の失敗」を EIO（device lost）で答える形にした。nvrtx は直した（queue_submit の間に gpu_sends が
  増えていたら EIO）。他の driver の調べと直し方は [BUG-381](../../bugs/BUG-381.md)。
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
- game mode の往復・画面・desktop（`rtx-p015.sh` PARTS='cycle scenes desk'、c8 と案 A・B（js = kernel 84f786dce と libvulkan lv2）、22:59〜23:14、
  build/tmp/p024-v2）: js の往復 3/3（direct=3 left=3 window=3、failed 0、refused 0）、desktop・Notes・browser は c8 との差が右上の時計の箱だけ、
  frame-ops は c8 63・61・67・60・63・60、js 64・61・65・61・64・63 fps（launch・type・output・drag・home・wiseview、後退なし）。戻し cksum 一致、bl 行 0、Debian。

### 再開点（2026-10-31 23:15 P3、q1785-i01 を返す）

- branch agent/p3-ws291wsi の commit: 613c725f1・692209073（nvrtx の submit の道と texture header）、ab2ddbf7c・9246cf6ea（WS014 の fixture）、
  91bb3b25c（記録）、84f786dce・5a03a0e95（案 A・B の kernel と uapi の layout）、e29e8a7d4（libvulkan）、8c2e9ddee（main cc74c9075 の取り込み）、
  a99e1411e（BUG-381 の driver の答えの形）と、この記録の commit。
- 到達（RTX 2070、同じ window の比べ）: subs-1000 19.9 → 32.6 fps（NVK 75.4）、subs-100 184 → 300 fps、big-100 536 → 568〜586 fps、big-1000 は
  108〜116 の揺れの内（NVK 136.2）。目標（NVK 並み）は未達。
- 残りの見立て: subs-1000 の 1 submit は約 24 µs（NVK 約 5 µs）: kernel の GPU_JOB_SUBMIT 約 20 µs（nvrtx の frame の decode・executor・ring と GPU core）
  と libvulkan の user の時間。次の候補: (1) 1 submit の kernel の frame の残り（executor 約 7 µs の見込み、未計測。最初の draw の全 state、pool の
  entry の書き直し）、(2) GPU core の monitor の起床の残り（runtime_retire の capacity の起床など 3 回）、(3) libvulkan の queue_enqueue の私用の
  fence と ledger（private_fence・reap）、(4) fence の無い submit の job（製品の判断）。big-1000 は record（client の encode と kernel の decode、
  約 3.8〜4.0 ms、NVK 3.15）と GPU の 1 draw（0.27 µs 対 NVK 約 0.256 µs）。big-100 の lend の遅れ（item 3）は未着手。
- 未実施: amdrdna・i915・powervr の BUG-381 の直し、Venus の host の側の部分の失敗の調べ、案 A・B の効果の 3 round 交互の比べ、
  nvrtx の runtime_job_command の失敗の道の実機の試し（fault の注入の knob が無い）。
- 追記（23:15〜23:28）: GPU core の capacity の変化の時の monitor の起床を、その device の open に停止か close が待っている時だけに
  （`gpu_capacity_changed_locked`、retirement の起床と同じ規則、`gpu_device_stop_waiting_locked`）。host: `run-gpu-job-host.sh` PASS（WS014 の
  supervision・job・fence の fixture を含む）、RTX・config-amd64・rpi5 の vmunix warning 0。実機（`rtx-p024.sh` ROUNDS=2、libvulkan lv2、js = a99e1411e の
  kernel、js3 = この変更、build/tmp/p024-js3）: subs-1000 js 32.16・28.56 → js3 34.67・34.31、subs-100 296.1・301.6 → 301.1・297.5、big は揺れの内。
  戻し cksum 一致、Debian。QEMU の Venus（この変更の kernel で forge の image を作り直し、23:30〜23:39）: `venus-reg: PASS`（forge・fence 600・wltest・
  mview mailbox・mview 20 submit・compositor の VULKAN_ERROR 0）。

## q1785-i02（2026-10-31 23:41〜 P3、worktree p3-ws291wsi を main 71f8b7d8b に fast-forward）

- 承認: Queue q1785-i02（上のユーザーの指示のまま、残りの一覧 1〜5 を順に）。fence の無い submit に job を付けるかは変えない（製品の判断、Q1 がユーザーに聞く）。
  実機 10.0.10.3（RTX 2070）、lock /tmp/zedbsd-rdna.lock を window ごと。各 window の終わりに 14 file と NVMe の kernel を戻して cksum 一致
  （vmunix 1463007375、nvrtx.vk.perf 行 0）、Debian（RTX 4 function vfio-pci）。main の腕 = kernel js3（3682022559、HEAD の build と cksum 一致）＋
  libvulkan lv2（3748472591）＋ base の 13 file。

### 1 submit の kernel の内訳（診断 kernel d1・d2、`nvrtx-submit-stages-diag.patch`、23:48〜00:03、build/tmp/p024i2-d1・d2）

- 診断: GPU_IOCTL_TIMING＝1 と nvrtx の perf の行に、GPU_JOB_SUBMIT の段（要求・stream の copyin・reserve・driver・finish・後始末）、executor の段
  （begin・draw・ring の submit）、opcode ごとの dispatch の時間（`plan/ws291/tests/nvrtx-submit-stages-diag.patch`、nvrtx-cmd-timing.patch を含む）。
  時計の読み 1 対が約 45 ns なので、細かい段ほど水増し。
- subs-1000（d2、5 s の窓、144739 submit）の GPU_JOB_SUBMIT 1 本 23.4 µs: stream の kern_malloc と copyin 3.8、reserve 1.6、driver 15.3、finish 1.0、
  後始末（pin を外す・copyout）1.5。driver の中: executor 10.0（begin 1.65 = 主に execute_state の kern_calloc、draw 16 本 3.5、ring の submit 1.0）、
  record の decode 約 2.2、RESET_COMMAND_BUFFER 1.0（record の object の参照を外す）。
- big-1000（d1・d2）: RESET_COMMAND_BUFFER 1 frame 330 µs（80000 record）、executor の draw 1 本 約 140 ns（時計込み）。frame は record（client の
  encode）3.7〜3.9 ms・submit 3.3〜3.5 ms・fence 1.1〜1.2 ms: GPU は submit の始めから約 4.5 ms（1 draw 約 0.22 µs、NVK の 0.256 より速い）で、
  残りの差は client の encode（NVK 136.2 fps = 7.35 ms には record を約 2.6 ms に縮める要）。

### 直したこと（i2a、commit 1e75e4de4）

- `vk-execute.c`: executor の state を session の executor に持ち、submit ごとの kern_calloc・kern_free を無くした（kernel の heap は first-fit で lock を取る）。
- `gpu.c`（GPU core）:
  - open ごとに stream の kernel の写しの buffer を持つ（16 KiB まで、GPU_COMMAND と GPU_JOB_SUBMIT。admission が 1 本ずつに限り、backend は
    callback の後に持たない: venus は自分の写しを作る）。submit ごとの kern_malloc・kern_free が無くなる。
  - 監督の thread（monitor）の起床を、それが眠っている時（全体の scan の後）は、record の期限が眠る期限より早い時と、open の停止か close が待つ時
    だけに（`gpu_monitor_notify_locked`、`monitor_asleep`・`monitor_until`）。scan の途中なら今どおり起こす。reserve・admit・pin を外す時の 5 か所。
    一覧の 2（monitor の起床の残り 3 回）。
- libvulkan: transaction の writer を最初に必要な大きさで取る（`context.c`、`commands.c` の pending の recording。256 byte から倍々の realloc が
  submit ごとに 2 つの writer で約 4 回ずつあった）。
- host: `run-gpu-job-host.sh` PASS（WS014 の fixture と gpu-job-submit、plain と ASan/UBSan）、nvrtx vk 94/0、vk-d3 228/0、sm75 308/0。
  config/ci の 19 の vmunix（commit 1e75e4de4 の写しから）と RTX の vmunix warning 0。
- QEMU の Venus（forge の image を i2a の tree から build/p024/forge2 に、libvulkan はその build）: 1 回目は guest が 240 s で SSH に答えず
  （変更なしでもう 1 回）、2 回目 `venus-reg: PASS`（forge・fence 600・wltest・mview mailbox・mview 20 submit・compositor の VULKAN_ERROR 0）。

### 実機の A/B（`rtx-p024.sh` ROUNDS=2 SHOT=1、00:07〜00:22、build/tmp/p024i2-ab1、腕ごとに kernel と libvulkan）

| 腕 | big-100 | big-1000 | subs-100 | subs-1000 | scene-1500 | scene-5000 |
| --- | --- | --- | --- | --- | --- | --- |
| main（js3 ＋ lv2）r1・r2 | 554.9・548.5 | 113.3・115.4 | 304.1・303.9 | 34.81・35.14 | 155.6・157.0 | 49.68・49.76 |
| i2a（49188371 ＋ lv3 4209590221）r1・r2 | 555.3・555.1 | 117.4・116.1 | 347.4・365.0 | 42.39・43.69 | 157.6・157.2 | 49.79・49.86 |

- subs-1000 の vkQueueSubmit 22.4 → 17.2 µs（+23 %）。big は揺れの内か小さく上。
- 画素: 100 copies の still（one・each・subs）は両 round とも main の round 1 と max 0・differ 0、one と subs も差 0。
- `rtx-p024.sh` に腕の 3 つ目の field（その腕の libvulkan）を足した。
- game mode の往復・画面・desktop（`plan/ws309/tests/rtx-p015.sh` PARTS='cycle scenes desk'、main と i2a、00:22〜00:37、build/tmp/p024i2-v1）:
  往復は両腕とも direct=3 left=3 window=3、failed 0、refused 0。画面（keiland-shot、i2a 対 main）: desktop・Notes・browser とも差は右上の時計の箱
  （x=1881..1894、y=17..27）だけ。desktop（frame-ops）: main launch 64・type 61・output 69・drag 60・home 63・wiseview 64、i2a 63・62・67・61・64・64 fps
  （後退なし）。起動の dmesg の `vram-window` の行は両腕に出る（前から）。戻し cksum 一致、bl 行 0、Debian。

### i2a の後の 1 submit の内訳（診断 d3 = i2a ＋ 診断、libvulkan lv3 の TIMING も、00:41〜00:48、build/tmp/p024i2-d3）

- subs-1000 の GPU_JOB_SUBMIT 1 本 15.8 µs（d2 の 23.4 から）: 要求の copyin と確かめ 1.0、stream の buffer 0.04（d2 の kern_malloc を含む 3.8 から）、
  stream の copyin 0.7、reserve 1.4（要求の copyout を含む）、driver 10.5、finish 0.9、後始末 1.2（outcome の copyout を含む）。
  driver の中: executor 6.0（d2 の 10.0 から。begin 0.1（d2 1.65）、draw 16 本 3.3、ring の submit 1.0）、record の decode 約 1.7、
  RESET_COMMAND_BUFFER 0.8、BEGIN 0.1。user の側は TIMING の水増しが大きく、reap が 7 submit に 1 回（1 回 約 7 µs）、private_fence 約 1 µs。
- big-1000: 段は record（client の encode）4.0・submit 4.0・fence 0.9 ms（timing と診断込み）。

### 試して採らなかった物: libvulkan の reap の閾値 8 → 24（i2b、00:48〜01:03、build/tmp/p024i2-ab2、kernel i2a、libvulkan lv4 3919355289）

| 腕 r1・r2 | big-100 | big-1000 | subs-100 | subs-1000 |
| --- | --- | --- | --- | --- |
| i2a（lv3） | 567.6・571.3 | 115.6・113.9 | 369.5・357.0 | 41.45・39.93 |
| i2b（lv4、閾値 24） | 552.5・549.9 | 115.6・115.7 | 374.6・367.6 | 43.85・44.33 |

- subs-1000 は同じ window で +6〜11 %（i2a の 2 round の揺れ 1.5 fps を越える）が、big-100 が −3 %（揺れ 3.7 を越える）。台帳に消費されない
  record が長く残ると fence・WSI の image の「終わったか」の問いが system call に回る。big-100（game mode の主の場面）を下げるので採らない
  （sync.c は戻した）。画素は両腕とも差 0。QEMU の Venus は lv4 で PASS（build/tmp/p024i2-venus3）。

### 試して採らなかった物: blob の stream の写しを session に持つ（i2c、01:04〜01:12、build/tmp/p024i2-ab3、kernel 1921706631、libvulkan lv3）

- big-1000 の長い recording は約 1 MiB の blob ごとに recording の thread が送る（d3: 1 本の transaction 約 0.7〜0.8 ms、client の 1 MiB の encode
  約 0.75 ms とほぼ同じ）。kernel の 1 MiB の kern_malloc を省けば送る側が速くなる見込みで、`vk-stream.c` の top level の写しを session に
  持つ形を試した（host: nvrtx vk の stream の試験の「写しを free」の数え方を直して 94/0）。

| 腕 r1・r2 | big-100 | big-1000 | subs-100 | subs-1000 |
| --- | --- | --- | --- | --- |
| i2a | 559.6・541.8 | 113.4・114.9 | 360.8・361.5 | 42.89・43.10 |
| i2c | 552.4・533.4 | 113.5・112.7 | 354.5・360.0 | 43.67・39.09 |

- 効果なし（big-1000 は揺れの内）。採らない（4 file を戻した）。画素は差 0。窓をまたぐ i2a の揺れ（big-100 541〜571、subs-1000 39.9〜43.7）が
  大きいので、上の i2b の big-100 の −3 % も確かではない（i2b は採らないまま、記録だけ）。

### 長い recording の batch を小さく（libvulkan `commands.c` の VULKAN_RECORDING_BATCH_BYTES、01:16〜01:33、build/tmp/p024i2-ab4、kernel i2a）

- 長い recording は recording の thread が batch ごとに送り、renderer がそれを decode する間に client は次を encode する。vkEndCommandBuffer は
  送り終わるのを待つので、最後の batch の送り（1 MiB で約 0.7 ms）がそのまま record の段に乗る。big-100（2000 draw、約 0.5 MiB）は 1 MiB に
  満たず、まったく重ならない。batch を小さくすれば最後の待ちが短くなり、big-100 も重なる。

| 腕 r1・r2 | big-100 | big-1000 | subs-100 | subs-1000 |
| --- | --- | --- | --- | --- |
| i2a（1 MiB、lv3） | 553.6・563.6 | 113.2・115.8 | 347.8・359.0 | 42.22・43.39 |
| b512（512 KiB、lv5 416424967） | 560.9・569.3 | 117.2・116.7 | 351.3・363.0 | 42.39・41.47 |
| b256（256 KiB、lv6 1775217122） | 610.3・596.3 | 119.0・118.3 | 362.4・354.3 | 41.38・43.24 |

- b256: big-100 +8 %（record の段 0.65 → 0.53 ms）、big-1000 +3〜4 %（record 3.9 → 3.55 ms）、subs は揺れの内。画素は 3 腕とも差 0。
  QEMU の Venus は lv6・lv5 とも PASS（build/tmp/p024i2-venus4）。
- 256 KiB と 128 KiB（01:35〜01:48、build/tmp/p024i2-ab5）: b256 big-100 608.5・587.5、big-1000 118.7・119.3、subs-100 359.7・361.2、
  subs-1000 41.66・40.34。b128（lv7 729938090）617.5・619.1、117.9・118.7、347.6・360.1、38.16・40.96。128 KiB は big-100 で少し上だが
  揺れの内で、transaction が倍になるので 256 KiB を採った（commit 8a3a95131、libvulkan 1775217122 = lv6 と同じ）。画素は差 0（13 比べ）。

### 再開点（2026-11-01 02:0x P3、q1785-i02 を返す）

- branch agent/p3-ws291wsi の commit（main 71f8b7d8b の上）: 1e75e4de4（GPU core の stream の buffer と monitor の起床、executor の state、
  libvulkan の writer、rtx-p024.sh、診断の patch）、09d54c61b（記録）、8a3a95131（recording の batch 256 KiB と記録）と、この記録の commit。
- 到達（RTX 2070、同じ window の main の腕との比べ、main = 案 A・B と js3 の後）: subs-1000 35.0 → 43 fps 前後（i2a・b256、窓ごとの揺れ 39〜44）、
  subs-100 304 → 355〜365、big-1000 114 → 118〜119、big-100 555 → 590〜610。目標（NVK subs-1000 75.4、big-1000 136.2）は未達。
- 残りの見立て（d3 の内訳から）:
  - subs-1000 の 1 submit は約 23 µs（vkQueueSubmit 約 17 µs、record 約 5.6 µs）。kernel の GPU_JOB_SUBMIT 約 15 µs は、executor 6（draw 16 本 3.3、
    ring 1.0）、decode 1.7、RESET 0.8、user の copy 4 回 約 3（要求の copyin 1.0、stream 0.7、copyout 2 回）、GPU core の記録の出し入れ約 3.5 と散らばり、
    1 つで 3.5 µs を越える物は無い。NVK（1 submit 約 13 µs、record 込み）に届くには形を変える要がある:
    (a) 小さな copyin・copyout の速い道（kernel の VM、`vmspace_copy_*` が page を pin する: WS291 の外、Q1 の判断）、
    (b) 記録の時に push buffer まで作る（kernel の decode と executor を recording の thread の側に寄せ、submit は ring だけ: 設計の Phase が要る）、
    (c) fence の無い submit の job（製品の判断、Q1 がユーザーに聞いている）。
  - big-1000（frame 約 8.4 ms）: client の encode 約 3.5 ms（GPU は待つ）＋ GPU 約 4.4 ms（submit の始めから、1 draw 約 0.22 µs で NVK の 0.256 より
    速い）＋ 0.3。136.2 fps（7.35 ms）には encode を約 2.6 ms に縮めるか、GPU を encode の間に走らせる（(b) と Vulkan の submit の意味の外）要がある。
    vkCmd* 1 本 約 45 ns（push 128 byte の draw で 4 本）。target 上に user の profiler が無く、encode の内訳は未計測。
  - 一覧の 5（game mode の lend の時の big-100 の遅れ）は未着手（b256 で big-100 は 555 → 600 前後に上がったが、lend との関係は調べていない）。
- 未実施: amdrdna・i915・powervr の BUG-381 の直し（前から）、i2a・b256 の 3 round 交互の比べ（2 round まで）。
- 消す物（Q1 へ）: build/tmp/p024i2-*（診断と A/B の出力、tree の写し p024i2-diag・p024i2-d3t・p024i2-main・p024i2-cfg）、build/p024/{bench0,bench0fp,
  bench1fp,cfg-i2a,i2c,lv4,lv5,lv7,u-i2a,u-main}（使い終わり）、build/p024/u-i2a/bin/'*' と u-main/bin/'*'（誤って作った壊れた symlink、u-main は v2 の後）。残す: build/p024/{base,js3,lv2,i2a,lv3,lv6,
  lv8,u-fin,forge2}。
