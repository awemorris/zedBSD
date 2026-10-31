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

- game mode の往復・画面・desktop（rtx-p015.sh PARTS='cycle scenes desk'、main と fin = kernel i2a ＋ libvulkan 1775217122、01:48〜02:03、
  build/tmp/p024i2-v2）: 往復は両腕 direct=3 left=3 window=3、failed 0、refused 0。画面は desktop・Notes・browser とも差が右上の時計の箱
  （x=1888..1894、y=17..26）だけ。desktop: main launch 63・type 61・output 65・drag 60・home 63・wiseview 65、fin 64・61・66・60・64・59 fps。
  wiseview の 59 を確かめに desk だけ i2a（1 MiB）と fin を同じ window で（02:05〜02:16、build/tmp/p024i2-v3）: i2a 62・61・68・60・64・64、
  fin 62・61・66・61・64・64 fps（wiseview 64 対 64、v2 の 59 は揺れ）。どの window も戻し cksum 一致、bl 行 0、Debian。

### 再開点（2026-11-01 02:20 P3、q1785-i02 を返す）

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

## q1785-i03（2026-11-01 04:38〜 P3、worktree p3-ws291wsi に main bad74894e を取り込み）

- 承認: Queue q1785-i03。2026-11-01 ユーザーから、0m の問いへの答えとして次の趣旨の指示を受けた: (1) Linux（Debian の NVK）と段ごとに比べ（submit
  ごとの user の時間・ioctl の数と費用・kernel の時間・GPU の時間・record と encode）、遅い所を全部名指しして直し、目標まで続ける。(2) (a) kernel の VM の
  小さな copyin・copyout の速い道は、主の道と判断すれば行う（copy の道だけ、HAL の API は変えない、全 arch の kernel を warning 0、QEMU の回帰を amd64 と
  arm64 の 1 つ以上）。(3) (c) fence も signal の semaphore も無い submit に私用の fence の job を付けない形は、libvulkan の変更で他の GPU driver
  （amdrdna・i915・powervr・v3d・Venus）に副作用が無い時だけ（nvrtx だけが使う GPU_JOB_SUBMIT の道に限るか、他の driver が変わらない事を示す。
  Venus の回帰）。副作用無しにできなければ行わず理由を報告。(4) (b) 記録の時の push buffer は始めない。代わりに短い説明を書く。

### NVK（Debian）との段ごとの比べ（`plan/ws291/tests/nvk-profile.sh`・`nvk-stat.py`・`nvk-profile.list`、04:41〜04:47、build/tmp/p024i3-nvk3）

- 手順: RTX を vfio-pci から nouveau（GSP）へ移し、weston（kiosk、DRM）の上の Debian の ~/linux-mview（cksum 209491786）を 4 回: そのまま（fps と段）、
  `perf stat -I 1000`（root、system call の数を 1 秒ごとに、中央の秒 ÷ fps で 1 frame あたり）、`perf trace -s`（system call の数と平均の時間）、
  `perf record -a`（4 s 目から、comm と DSO ごと）。終わりに weston を止め vfio-pci に戻した（4 function とも vfio-pci）。linux-perf を Debian に入れた。

| 段（subs-1000、1 submit あたり） | NVK | zedBSD main（i2a ＋ lv6、d3 の内訳） |
| --- | --- | --- |
| fps（frame） | 75.4（13.25 ms） | 42.5（23.45 ms） |
| record（client） | 6.2 µs（vkCmd* が push buffer を直に書く） | 5.7 µs（stream に encode するだけ） |
| vkQueueSubmit | 4.65 µs | 17.4 µs |
| GPU の ioctl | 1.02 本（DRM_NOUVEAU_EXEC、perf trace の平均 約 4.7 µs） | 1 本（GPU_JOB_SUBMIT 約 15.8 µs）＋ 7 submit に 1 本の RETIRE |
| kernel の中身 | job を drm_sched に積むだけ。ring への書きは kworker（別の CPU、perf record の 24〜30 %） | 呼び手の thread で: user の copy 4 回 約 3、GPU core の job の出し入れ 約 3.5、nvrtx の decode・executor・reset 約 8.5、ring 約 1 |
| libvulkan の user の時間 | — | 私用の fence と ledger の reap 約 2 |

- subs-100: NVK 683 fps（1 submit の record 5.6・submit 4.6 µs）、zedBSD 362。big-1000: NVK 127〜135 fps（record 3.2〜3.4 ms、ioctl 17 本 / frame）、zedBSD 118
  （record 3.7・submit 3.3・fence 1.1 ms）。
- 差の名指し（subs-1000 の 1 submit、計 約 12 µs）: G1 push buffer を submit の時に呼び手の thread で組む（約 8 µs。NVK は record の中で組み、ring は
  別の thread）、G2 user の copy 4 回（約 3 µs、Linux の copy_from_user は 0.1 µs 程度）、G3 fence の無い submit にも付く job の出し入れ（約 3.5 µs）、
  G4 私用の fence と reap（約 2 µs）。big-1000 の差は client の encode（約 0.4 ms）と submit の段の kernel の decode・executor（約 3.3 ms、NVK は 0.03）。

### 直したこと（i3）

- (c) libvulkan `queue.c`（`queue_unfenced`）: fence が無く、どの submit にも wait・signal の semaphore も pNext も無い公の vkQueueSubmit（WSI の
  lock を持つ呼び手と sparse を除く）は、私用の fence も job も作らず GPU_COMMAND 1 本で送る。条件に `GPU_CAP_JOB_SUBMIT` と同期の command を入れた。
  kernel（gpu.c `gpu_get_info`）はこの cap を backend の job の表に `command` がある時だけ立て、その表を持つのは nvrtx だけ（amdrdna・i915・powervr・
  bcm2711（v3d）・venus・mali・ghci の表の `command` は NULL）。よって他の driver の道は変わらない。その仕事は queue の順で次の job の marker より前に
  走るので、次の job（frame の fence、vkQueueWaitIdle の空の job）が監督し退かせる。job の無い submit の native の失敗は、確かな拒否（OOM）の他は
  device lost（job の取り消しと同じ扱い）。
- (a) kernel の VM `src/kern/vm/vmspace-copy.c` の `copy_backing`: 1 page に収まる範囲で、1 つの region の、resident の private page（copy-on-write
  の後）は、locks を 1 回取って region を 1 回たどり、同じ確かめ（範囲・権限・BUSY と MAPPED・private が 1 つ・resident・書きは COW でない）の後に
  pin して copy する。それ以外（複数 page、device・object の page、準備のできていない page、pin の EBUSY）は今の道へ。今の道は 1 page の copy で
  region の list を 4 回たどっていた（範囲の確かめ・準備の確かめ・検証・pin）。HAL の API は変えない。触った file: `src/kern/vm/vmspace-copy.c` と
  その host 試験 `plan/tools/kernel-lock/pin-host-test.sh`。
- `pin-host-test.sh`: main で build できなかった（HAL_SPACE_* と hal_physaddr_t が hal.h に移った）ので `#include <hal/hal.h>` を足した。1 page の
  copy の場合（1 entry、fault 無し、bytes、pin の解放、region の外は EFAULT）と、並行の試験に 1 page の範囲の 4 つ目の thread を足した。偽の
  `vm_private_page_mark_dirty` の数え方を atomic に（2 つの thread が同じ page に書くと ThreadSanitizer が偽の競合を出した）。

### 確かめ（i3）

- host: `pin-host-test.sh` PASS（plain・ASan/UBSan・TSan、TSan の警告 0）、`run-gpu-job-host.sh` PASS、nvrtx vk 94/0、vk-d3 228/0、sm75 308/0。
- build（warning 0）: config/ci の 19 の vmunix（amd64・amd64-server・intelmac・pcat・pc98・rpi4・rpi4-server・rpi5・odroid-c5・odroid-c5-keiland・
  armv7-virt・zybo・riscv32-virt・riscv64-virt・visionfive2・visionfive2-keiland・m68k-virt・ppc32-mac99・ppc64-pseries）と plan/tools/ports の
  mips32-malta・mips64-malta・sun4u・x68k、RTX の config の vmunix（i3a 2764259344）と libvulkan（lv9 509112954）。
- QEMU の回帰（copy の道）: `plan/tools/process/forkrace-qemu.sh`（fork・posix_spawn と並行の copyin・copyout の COW fault）amd64 PASS、rpi4（arm64、
  raspi4b）PASS（build/tmp/p024i3-fr-amd64・-rpi4）。

### (b)「記録の時に push buffer まで作る」の説明（2026-11-01 ユーザーの指示で、始めずに説明だけ）

- 今の流れ: アプリが vkCmd* を呼ぶと libvulkan は命令を中間の形（stream）に書くだけ（record）。vkQueueSubmit の時に kernel がその stream を読み解き
  （decode）、GPU が読む命令の列（push buffer）に組み立て（executor）、ring に入れる。組み立ては毎回の submit で、submit を呼んだ thread の上で行う。
- (b) の形: vkEndCommandBuffer の時（記録の終わり）に push buffer まで組み立てておき、vkQueueSubmit は出来上がった push buffer を ring に入れるだけに
  する。Linux の NVK は vkCmd* の時に push buffer を直に書くので、これに近い。組み立ては libvulkan の recording の thread（今も長い記録を送っている
  別の thread）の側で走り、アプリが次の command buffer を記録する間に重なる。同じ command buffer を何度も submit する時は組み立てが 1 回で済む。
- 見込み: subs-1000 の 1 submit 約 17 µs（(a)(c) の後 約 11 µs）のうち、decode と executor の約 8 µs が submit から消える。毎 frame 記録し直す subs では
  組み立ての費用は消えずに record の側へ移るが、別の thread に重なれば 1 submit 約 5〜6 µs（NVK 4.65 µs）に近づき、subs-1000 は 59 → 70 fps 台の見込み
  （未計測の見積もり）。big-1000 の submit の段（約 3.3 ms）も同じ形で縮む。
- 費用: nvrtx の Vulkan の backend の作り直し。submit の時にしか決まらない物（image の置き場所、texture・sampler の pool の slot、前の submit の後の
  GPU の状態を知った上での省略、render pass の target）を記録の時に決めるか、submit の時に少しだけ書き換える仕組みが要る。command buffer の
  reset・再記録・同時の使用、記録した push buffer の memory の持ち方と解放も決め直す。設計の Phase と design review が要り、数日の規模。HAL と
  uapi は変えない見込み。
- 実機の A/B（10.0.10.3、`rtx-p024.sh` ROUNDS=2 SHOT=1、04:59〜05:35、build/tmp/p024i3-ab2、userland は build/p024/base の 14 file（14 番目は
  /lib/libwayland-client.so: 1 回目 ab1 は 13 file で入れて compositor が `ld.so: undefined symbol` になり、止めて戻した）、kernel main = k-main
  49188371（HEAD の tree の写しから build、i2a と cksum 一致）、i3a 2764259344、libvulkan main = lv6 1775217122、c = lv9 509112954）:

| 腕 r1・r2 | big-100 | big-1000 | subs-100 | subs-1000 | scene-1500 | scene-5000 |
| --- | --- | --- | --- | --- | --- | --- |
| main | 582.2・596.4 | 118.0・117.6 | 362.4・362.6 | 42.52・42.84 | 156.1・156.6 | 49.68・49.71 |
| c（job の無い submit） | 589.6・598.2 | 119.7・119.9 | 445.6・443.7 | 55.11・53.35 | 157.7・158.1 | 49.90・49.82 |
| a（copy の速い道） | 602.2・610.8 | 118.3・119.5 | 409.8・421.1 | 46.06・47.39 | 158.3・158.3 | 49.89・49.79 |
| ac（両方） | 621.8・623.6 | 115.1・118.3 | 479.9・481.8 | 58.68・58.41 | 157.7・157.9 | 49.96・49.78 |
| NVK | — | 136.2（i3 で 127〜135） | 683 | 75.4 | 165.6 | 50.1 |

- subs-1000 の vkQueueSubmit（frame の submit の段 ÷ 1000）: main 17.3 µs → c 12.2〜12.6 → a 14.9〜15.4 → ac 11.0〜11.1。record 5.6〜5.9 µs は不変。
  ac は subs-1000 +37 %、subs-100 +33 %、big-100 +5 %、big-1000 と scene は揺れの内。
- 画素: 100 copies の still（one・each・subs）を main の round 1 と比べ、31 の比べが max 0・differ 0。c の round 2 の one の PNG だけ取り込みが切れて
  （688128 byte、PIL の truncated）比べられなかった（同じ腕の each・subs と round 1 の 3 枚は差 0）。
- 戻し: 14 file と NVMe の kernel の cksum が元と一致（vmunix 1463007375、nvrtx.vk.perf 行 0）、Debian（RTX 4 function vfio-pci）。

### 再開点（2026-11-01 05:40 P3、q1785-i03 の途中、BUG-383・BUG-381 へ移る前）

- この記録の commit に入る物: `src/kern/vm/vmspace-copy.c`（a）、`userland/desktop/libvulkan/queue.c`（c）、`plan/tools/kernel-lock/pin-host-test.sh`、
  `plan/ws291/tests/nvk-profile.sh`・`nvk-stat.py`・`nvk-profile.list`、この phase.md。
- 走っている物（結果はこの後の commit で足す）: QEMU の Venus の回帰（forge の image を build/p024/forge3 にこの tree から、`venus-reg.sh`、
  build/tmp/p024i3-venus.txt）と、game mode の往復・画面・desktop（`rtx-p015.sh` PARTS='cycle scenes desk'、main と ac、build/tmp/p024i3-v1）。
- 到達: subs-1000 42.7 → 58.5 fps（NVK 75.4）、subs-100 362 → 481（NVK 683）、big-1000 118 前後のまま（NVK 136.2）。目標は未達。
- 次の手（残りの差、subs-1000 の 1 submit 約 11 µs 対 NVK 4.65）: (1) G1 nvrtx の decode・executor・reset 約 8 µs（submit の時に呼び手の thread で
  push buffer を組む。(b) の形か、executor の draw ごとの費用の削り: 16 draw で 3.3 µs、texture の handle と constants）、(2) GPU_COMMAND の残りの
  kernel の費用（要求と stream の copy 2 回、admission、GPU core の stream の写し）、(3) libvulkan の queue_attempt の user の時間（writer の組み立て、
  device の mutex）。big-1000 は client の encode（3.6 ms 対 NVK 3.2〜3.4）と submit の段の decode・executor（3.3 ms 対 NVK 0.03）。

### 再開点の後の結果（2026-11-01 05:35〜06:45）

- game mode の往復・画面・desktop（`plan/ws309/tests/rtx-p015.sh` PARTS='cycle scenes desk'、main = k-main ＋ lv6（build/p024/u-m3）と ac = i3a ＋ lv9
  （build/p024/u-i3）、05:35〜05:51、build/tmp/p024i3-v1）: 往復は両腕とも direct=3 left=3 window=3、failed 0、refused 0。画面（keiland-shot、ac 対
  main）: desktop・Notes・browser とも差は右上の時計の箱（x=1880..1894、y=17..27）だけ。desktop（frame-ops）: main launch 63・type 61・output 65・
  drag 60・home 64・wiseview 60、ac 62・61・67・60・64・64 fps（後退なし）。起動の dmesg の `vram-window bar1 before the tables` の行は前から。戻し:
  14 file と NVMe の kernel の cksum 一致、bl 行 0、Debian（4 function vfio-pci）。
- QEMU の Venus の回帰: 延期（host の I/O、前に PASS した image も同じに落ちる）。forge の image は build/p024/forge3 にこの tree から作った（OK）。
  1 回目と 3 回目は venus-reg の start の `timeout 300` が image の写し（2.4 GB）の途中で切った。2 回目は起動したが host の renderer の link
  （build/ws035-sq-venus）が worktree に無く、compositor が device を作れなかった（link を作り直した）。`venus-reg.sh` に START_TIMEOUT（既定 300）を足し、
  4 回目（1200）は guest が起動しても SSH が 240 s 答えない。手で起動した forge3（この tree）と forge2（i2 の tree、i02 で venus-reg PASS の image）の
  両方が 800 s 答えず、QMP の `info registers -a` で 4 vCPU とも sched_idle で HLT、qemu.log は Slirp の送りの失敗と host の renderer の
  「Unable to get a valid memory fd」。host は約 8 つの担当の build と image の写しで load 15〜18、I/O 待ちの task が多い（Q1 の確かめ）。
  libvulkan の (c) は Venus の道を通らない: `GPU_CAP_JOB_SUBMIT` は gpu.c が backend の job の表に `command` がある時だけ立て、venus.c の表の
  `command` は NULL なので、Venus の context では `queue_unfenced` が常に偽で、今までどおり私用の fence の job の 3 本の道。(a) の kernel の copy の
  道は forkrace の amd64・rpi4 で確かめた。host が静かになったら `START_TIMEOUT=1200 sh plan/ws291/tests/venus-reg.sh OUT build/p024/forge3/hdd-image.img
  build/p024/forge3/dynamic/libvulkan.so` を流す（Q1 が知らせる）。
- QEMU の Venus の回帰を 10.0.10.3 の Debian で（2026-11-01 Q1 経由のユーザーの許可: dev host の I/O の負荷は背景の Vivado で避けられない）:
  `plan/ws291/tests/venus-reg-rtx.sh OUT IMAGE LIBVULKAN...`（lock の中、Debian で）。Debian の QEMU 10.0.13（qemu-system-modules-opengl、入っていた）、
  main の build/ws035-sq-venus/install の strict queue の renderer を写し、Mesa の lavapipe の上。egl-headless の render node は vgem（RTX は vfio-pci の
  まま、CPU に iGPU は無い）。run の間だけ vgem を入れ /dev/kvm・/dev/udmabuf・/dev/dri/renderD128 を 666 に（dev host では awe が kvm group に
  居る。/dev/udmabuf が開けないと lavapipe の swapchain の memory が作れず VK_ERROR_OUT_OF_DEVICE_MEMORY になった: 06:51 の 1 回目）、後で 660 に
  戻して vgem を外す。package は入れていない。写した物は Debian の ~/p3-venus（scripts・image の写し 2.4 GB・renderer）。
  - 2 回目（06:55〜06:58、build/tmp/p024i3-venus-rtx2、forge3 の image 3508997510 とその libvulkan）: forge-guest・fence-guest（600 frame）・wltest・
    mview mailbox・mview 20 submit・compositor の VULKAN_ERROR 0、`venus-reg: PASS`。戻し: /dev/kvm・/dev/udmabuf 660、/dev/dri 無し（vgem を外した）、
    qemu 0、RTX の 4 function は vfio-pci。上の「延期」はこれで済み。

### 再開点（2026-11-01 07:00 P3、q1785-i03 を返す。ユーザーの指示で wrap up）

- branch agent/p3-ws291wsi の commit: b5f705dc0（(a)・(c)・NVK の profile、main b2604051a）、2567c80b0（main の取り込み）、d3f41f695（BUG-383）、
  6769998d2（Venus の延期の記録・ws.md・venus-reg.sh の START_TIMEOUT）、b737842bd（BUG-381、main 3e59321f1）と、この記録と venus-reg-rtx.sh の commit。
- 到達（RTX 2070、同じ window）: subs-1000 42.7 → 58.5〜58.7 fps（NVK 75.4）、subs-100 362 → 481（NVK 683）、big-100 590 → 622、big-1000 118 前後（NVK 136.2）。
  目標は未達。
- 次: 残りの最大の差 G1（nvrtx の decode・executor・reset 約 8 µs / submit を submit の時に呼び手の thread で行う）は (b)「記録の時に push buffer まで
  作る」で、設計の Phase と design review が要る（ユーザーの判断、上の説明）。その他の候補: executor の draw ごとの費用（16 draw で 3.3 µs）、
  GPU_COMMAND の残りの kernel の費用（copy 2 回・admission・stream の写し）、libvulkan の queue_attempt の user の時間。big-1000 は client の encode と
  submit の段の decode・executor（3.3 ms 対 NVK 0.03 ms）。
