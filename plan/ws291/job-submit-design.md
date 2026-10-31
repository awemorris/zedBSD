<!-- awesome-plan project=zedbsd record=ws291-job-submit-design -->

# 案: reserve・command・commit を 1 本の ioctl に（GPU core の job の ABI）

- 親: [WS291](ws.md)、[ws291-p006](phase006/phase.md) の q1668-i02 節。書いた: 2026-10-29 P2。状態: 案 A・B ともユーザーが採った（2026-10-29、
  ws.md の q1668-i02 節）。design-reviewer の後に Q1 へ送ってから実装。番号は WS306 の予定（'G' 46〜48、GPU_CAP 131072）と重ならない物（Q1）。
- 理由: subs-N（1 frame に 1000 回の vkQueueSubmit）は 1 submit 約 35 µs（Linux の RADV は約 10 µs の CS 1 本）。成功の道で GPU の ioctl が
  JOB_RESERVE（timing で約 5〜7 µs）・GPU_COMMAND（約 27 µs、kernel の frame 約 13 µs）・JOB_COMMIT（約 3 µs）の 3 本と、後の CONSUME の WAIT
  1 本（record ごとに 1 回、ledger の reap）。system call 自体は getppid 190 ns・GPU_GET_INFO 1150 ns（plan/ws291/tests/ioctl-cost.c、実機）。

## 今の形（main d94ceac00、gpu.c の行は目安）

1. GPU_JOB_RESERVE（gpu_job_reserve_ioctl、gpu.c 5517〜）: session の admission（gpu_session_enter）の外。monitor の起動、fence の参照（fd ≥ 0 の時）、
   sequence（gpu_handle_allocate）、completion の slot（gpu_completion_reserve、64 個、満ちたら EAGAIN）、registry lock の下で RESERVED と deadline
   （CONFIG_GPU_JOB_RESERVATION_MS）、fence の bind（gpu_job_bind、fences[] の空き、drv_gpu_fence_bind）、copyout、driver の jobs->reserve
   （amdrdna は runtime_reserve: timeline の確かめ、marker を取る、pending++。venus は transport の descriptor の組を取る）。
2. GPU_COMMAND（gpu_command_ioctl、3338〜、admission の中）: stream を kernel に写し、ops->command(priv, backend, buf, bytes)。sequence も reservation も
   渡らない。amdrdna の runtime_command は session の queue が空なら呼び手の thread で frame を走らせる（native word の flag 2、無理なら EAGAIN →
   libvulkan は GPU_COMMAND_SUBMIT と WAIT に戻る）。Vulkan の結果は reply の channel で libvulkan に返る。
3. GPU_JOB_COMMIT（gpu_job_action_ioctl、5642〜、admission の外）: gpu_job_observe で record を pin、gpu_job_admit（RESERVED → COMMITTED、
   drv_gpu_fence_admit、deadline を EXECUTION_MS に、monitor を起こす）、driver の jobs->commit（amdrdna は marker を queue の frame の後ろに。venus は
   descriptor を publish）。失敗は jobs->cancel(fault) で session を失う。
- reservation と command を結ぶのは各 driver の中の順だけ（commit した marker は、それより前に送った frame の後ろに並ぶ）。userspace は native の
  reply が成功の時だけ COMMIT し、確かな拒否（OOM）なら CANCEL、不確か・device lost なら CANCEL_FAULT。

## 案 A: GPU_JOB_SUBMIT（1 本で reserve → 同期の command → commit か cancel）

- uapi（include/uapi/gpu-job.h に足す、既存の ioctl・struct・GPU_ABI_VERSION は不変）:

```c
#define GPU_CAP_JOB_SUBMIT		262144U
#define GPU_JOB_SUBMIT			_IOWR('G', 49, struct gpu_job_submit)
#define GPU_JOB_SUBMIT_COMMITTED	1U	/* outcome */
#define GPU_JOB_SUBMIT_CANCELED		2U
#define GPU_JOB_SUBMIT_FAULTED		3U

/*
 * One reservation, its native command stream run at once on the caller's
 * thread, and the commit or cancel its outcome decides.  fd, generation and
 * timeline are as gpu_job_reserve; address and bytes as gpu_command.  The
 * kernel writes sequence before any native work (as GPU_JOB_RESERVE does),
 * then outcome; flags, reserved, sequence and outcome begin at zero.
 */
struct gpu_job_submit {
	uint32_t version;
	uint32_t size;
	int32_t fd;
	uint32_t flags;
	uint64_t generation;
	uint32_t timeline;
	uint32_t bytes;
	uint64_t address;
	uint64_t sequence;
	uint32_t outcome;
	uint32_t reserved;
};
```

- driver の新しい op（任意、include/drivers/gpu/gpu.h の drv_gpu_job_ops に足す）:
  `int (*command)(void *priv, void *backend, const void *buf, uint32_t bytes, uint32_t *outcome)`。
  約束: stream を今の ops->command と同じく呼び手の thread で走らせ、reply を書き、stream の最後の queue の操作（GPU_OP_QUEUE_SUBMIT か
  GPU_OP_QUEUE_BIND_SPARSE）の結果で outcome を返す: VK_SUCCESS → COMMITTED、確かな拒否（VK_ERROR_OUT_OF_HOST_MEMORY・
  VK_ERROR_OUT_OF_DEVICE_MEMORY）→ CANCELED、それ以外・frame の失敗（EIO）→ FAULTED。queue の操作の無い stream は走らせる前に EINVAL。
  今は走らせられない（amdrdna の session の queue に待つ frame がある）時は何もせず EBUSY。marker は並べない（commit は今の jobs->commit）。
- core（gpu.c、gpu_session_enter の admission の中、GPU_COMMAND と同じ所）:
  1. 今の gpu_job_reserve_ioctl の本体を copyin・copyout から分けた内部の関数（gpu_job_reserve_locked など）にし、同じ確かめ・monitor の起動・
     fence の参照・sequence・completion の slot・RESERVED と deadline・fence の bind を行い、**sequence を copyout してから** jobs->reserve
     （「No undisclosed identity is allowed to retain a backend callback」の順を保つ）。満杯（slot・fences[]・driver の marker）は EAGAIN で、
     何も残さない（今の reserve の失敗と同じ unbind と discard）。
  2. stream を kernel に写して jobs->command。
  3. outcome が COMMITTED: 今の gpu_job_action_ioctl の COMMIT の本体（gpu_job_admit → jobs->commit、失敗は jobs->cancel(fault) で session を
     失う）を内部の関数にして呼ぶ。CANCELED: jobs->cancel(0)・unbind・discard・drv_gpu_capacity_changed。FAULTED: jobs->cancel(1) で session を
     失う。EBUSY・stream の EINVAL: CANCELED と同じ戻し（native は何もしていない）で errno をそのまま返し、outcome は 0。
  4. outcome を copyout。copyout の失敗は EFAULT（job は outcome どおりの状態で残る。userspace は context を lost にする、を uapi の comment に）。
  - reservation の間（1〜3）は今の reserve と同じ job_action の pin で、monitor・別の thread の COMMIT・CANCEL（EBUSY）と並ばない。stop・drain・
    deadline は今の RESERVED・COMMITTED の扱いのまま（reserve の deadline は 3 の前に切れうる: その時 gpu_job_admit が今どおり失敗し FAULTED の道）。
- libvulkan: GPU_CAP_JOB_SUBMIT と同期の command の capset の flag の両方がある時だけ、queue_attempt の reserve・execute・finish を 1 本に。
  ledger の notification は ioctl の前に作って pin（waiters 1）し、返った sequence を入れてから公開する（今の reserve と同じ: 公開の前に
  別の reaper は知らない）。EAGAIN → 今の capacity の待ち（GPU_JOB_CAPACITY の query、sequence 0 の時の扱いは今の reserve の EAGAIN と同じ）。
  EBUSY → 今の 3 本の道（reserve から、GPU_COMMAND の EAGAIN で SUBMIT と WAIT）。outcome は今の finish の 3 つの結果と同じに扱う。
- 互換: 古い kernel は bit を出さないので今の道。古い libvulkan は新しい ioctl を使わない。jobs->command を持たない driver は bit を出さない
  （core の登録の時に ops から決める）。

## 案 B: GPU_JOB_RETIRE（終わった job の record をまとめて consume）

- GPU_COMMAND_WAIT（CONSUME）は 1 本で 1 sequence。libvulkan の ledger の reap は record ごとに 1 本（今の reap 1 回 約 10〜16 µs = 8〜14 本）。
- uapi:

```c
#define GPU_CAP_JOB_RETIRE		524288U
#define GPU_JOB_RETIRE			_IOWR('G', 50, struct gpu_job_retire)
#define GPU_JOB_RETIRE_MAX		GPU_SUBMIT_MAX
#define GPU_JOB_RETIRE_PENDING		0U	/* entry state */
#define GPU_JOB_RETIRE_RETIRED		1U
#define GPU_JOB_RETIRE_MISSING		2U

struct gpu_job_retire_entry {
	uint64_t sequence;	/* in */
	uint32_t state;		/* out */
	uint32_t status;	/* out: as gpu_command_wait.status when RETIRED */
};

/* Up to GPU_JOB_RETIRE_MAX named job sequences, each consumed when terminal. */
struct gpu_job_retire {
	uint32_t version;
	uint32_t size;
	uint32_t count;
	uint32_t flags;		/* 0 */
	uint64_t address;	/* count entries */
	uint32_t retired;	/* out */
	uint32_t reserved;
};
```

- 名指しの理由: libvulkan は自分の waiter が pin した record（vkWaitForFences の any の level の起床）を reap しない。kernel が自分で選んで
  consume すると、その約束が守れない。userspace が pin の無い record の sequence を渡し、kernel は渡された物だけを見る。
- core: entry を copyin（count ≤ 64）、registry lock の下で各 sequence を今の gpu_completion_observe の timeout 0 と同じ規則で探す（listed で、
  job の record だけ: GPU_COMMAND_SUBMIT の decoder の record は MISSING）。終わっていて consuming でない物は observers++ と consuming=1、
  他は PENDING・MISSING。entry と retired を copyout、成功の時だけ各 RETIRED を gpu_completion_observer_leave(consume=1)（今の WAIT と同じ:
  copyout の失敗は consume しない）。
- libvulkan: bit がある時、sync_notifications_reap は pin の無い record を 64 個ずつ 1 本で。RETIRED の status != 0 は今どおり context の loss、
  MISSING は今の ENOENT（別の reaper）と同じ。
- 互換: 案 A と同じ（bit、GPU_CAP_NOTIFICATION がある driver なら core だけで足りるので全部に出してよい）。

## 各 driver への影響

| driver | 案 A（jobs->command） | 案 B |
| --- | --- | --- |
| amdrdna | 足す: runtime_command の同期の道（flag 2）に outcome を足す。executor が queue の操作の VkResult を reply に書く所で outcome を決める。最初に実装する | core だけ |
| nvrtx・i915・ghci・powervr・bcm2711 | 足さない（bit を出さず今の道）。後で足すなら amdrdna と同じ形 | core だけ |
| venus | 足さない（同期の command が無い。transport の reserve と publish が別の段で host の往復は減らない） | core だけ（GPU_CAP_NOTIFICATION がある時） |

- gpu.c: reserve と commit・cancel の本体を内部の関数に分け、GPU_JOB_RESERVE・GPU_JOB_COMMIT・GPU_JOB_CANCEL と GPU_JOB_SUBMIT から呼ぶ
  （monitor・deadline・bind・admit の論理は 1 か所のまま）。GPU_JOB_RETIRE は gpu_completion_observe と observer_leave の同じ規則を
  複数の sequence に。

## 期待の効果（見積もり、未計測）

- 減る物: system call 2 本（入口・copyin・copyout・file の参照、約 1〜2 µs ずつ）、record の pin・observe の lookup の 2 回目、registry lock の
  取り直し。JOB_RESERVE（5〜7 µs）と JOB_COMMIT（約 3 µs）の大部分のうち、確保と状態の遷移の本体（約 2〜3 µs の見込み）は残る。
- subs-1000 の 1 submit 約 35 µs → 約 28〜30 µs（−15〜20 %）。subs-1000 の fps 24.7 → 約 29〜31。Linux（58 fps、10 µs）にはまだ届かない:
  残りは GPU_COMMAND の中の kernel の frame（decode・executor・ring、約 13 µs）と libvulkan の encode・system call。
- 案 B: reap の 8〜14 本が 1 本に。1 submit あたり約 1 µs の見込み（record ごとの system call 約 1 本分）。

## 効果の基準（測る前に決める。2026-10-29 ユーザーから、足して効果の無かった ioctl は採らなくてよい、という趣旨の答えを受けた）

- 実機（RDNA、timing 無し、bench の subs-1000 と subs-100、同じ kernel・libvulkan の組で前後を 3 run ずつ交互に）で、subs-1000 の submit の ms/frame
  の差が、同じ build の run の間の揺れ（最大 − 最小、2 つの値に分かれる時は同じ側の値どうし）を越えて減った時だけ効果ありとする。
  案 A と案 B は別に測る（A だけ、A + B）。効果の無い方は main に入れない（branch に残し、値と判断を phase.md に）。

## design-reviewer（opus、2026-10-29）の指摘と直した形（上の案 A・B の本文より、ここが優先）

- B1（blocker、outcome の決め方）: 走らせる前に stream から queue の操作を探さない（大きな stream は blob で、amdrdna は実行の途中で写す
  vk-stream.c、userspace が書き換えうる）。outcome は executor が実際に実行した queue の操作の結果（executor の内部の変数、共有の reply は
  読み直さない）から: 1 つも実行しなかった・全部が確かな拒否 → CANCELED、全部 SUCCESS → COMMITTED、それ以外（SUCCESS と拒否の混在、不確か）→
  FAULTED。走らせる前の EINVAL は無し。
- B2（blocker、errno と outcome の約束）: uapi の comment に表で書く。
  - errno（native を何も走らせていない、record も fence の bind も残らない、sequence は無効）: EINVAL・EACCES・EOPNOTSUPP・EFAULT（入力）・ENOMEM・
    EAGAIN（completion の slot・fences[]・driver の marker の満杯 → capacity の待ち）・ENODEV（device・session の失敗）。fence の bind の EBUSY は
    EAGAIN と区別し errno のまま（userspace は今の reserve の EBUSY と同じ扱い）。
  - return 0 と outcome（native を走らせた、または走らせようとした）: COMMITTED・CANCELED・FAULTED と、`GPU_JOB_SUBMIT_DEFERRED`（4、driver が今は
    同期で走らせられない、native は何もしていない、record は戻した → userspace は今の 3 本の道）。後戻りは errno でなく DEFERRED で返す。
  - admit・commit の失敗は cancel の結果によらず FAULTED、session を失う。outcome の copyout の失敗は EFAULT で、job は outcome どおりに残る
    （userspace は context を lost にする）。outcome が正: userspace は GPU_JOB_SUBMIT の job に COMMIT・CANCEL を出さない。kernel が CANCELED で
    libvulkan が reply を読めない時は、libvulkan が context を lost にする（共有の fence は unbind のまま: 今の CANCEL_FAULT の ERROR との違いを
    uapi に書く）。
- M1（major、DEFERRED の率）: amdrdna の同期の frame は session の queue（frame と marker）が空でない時に断られる（runtime.c 357〜370）。commit の
  marker は worker が park するまで queued に残るので、A で submit が速くなると DEFERRED が増え、後戻りの道（今より重い）で効果が負になりうる。
  → 実装の前に今の GPU_COMMAND の EAGAIN の率を subs-1000 で数える（libvulkan の timing に count を足す）。driver の側で「待つ物が marker だけ
  なら同期の frame を先に走らせる」（marker の runs は park の時に取るので、marker の完了が遅れるだけの安全な側）を検討し、効果の基準に DEFERRED の
  率を足す。
- M2（major）: native_submitted は jobs->command を呼ぶ前に必ず立てる（今の GPU_COMMAND と同じ、monitor の stop_begin の近道のため）。
- M3（major）: drv_gpu_job_ops に member を足すと位置で初期化している 7 driver の build が -Werror で壊れうる → 7 つ全部に NULL を書くか designated
  に。bit は core が gpu_info_ioctl で合成する（jobs->command がある時 GPU_CAP_JOB_SUBMIT、GPU_CAP_JOB がある時 GPU_CAP_JOB_RETIRE）と決め、
  登録の許可の表（gpu.c 1051〜）には入れない。
- M4（major）: reserve の内部の関数は pin した completion を返し、commit・cancel の内部の関数はそれを受ける（gpu_job_observe を通さない: job_action の
  pin で自分に EBUSY を返すため）。observer_leave は最後に 1 回。unpublished=0 と reservation_pending=1 の順は今のまま。
- M5（major、案 B の失敗の道）: copyout の失敗でも RETIRED にした各 record に observer_leave(c, 0, 1)（consuming と observers を戻す）。MISSING は
  reap の中では今の ENOENT（DEVICE_LOST）と同じ扱い。status ≠ 0 の entry があっても、まとめて consume した RETIRED は全部 ledger から外して free
  してから lost を返す。
- minor: stream の copyin は reserve の前。案 B の lock は slot を 1 回だけ走査して照合（64×64 を irqsave で持たない）。入力の確かめ（readable、
  sequence ≠ 0、state・status 0、count ≥ 1、重複は 2 つ目を PENDING、observers の UINT_MAX）、bit は GPU_CAP_JOB と合わせる、dispatch は WAIT と
  同じく admission の外。libvulkan では SUBMIT は context の mutex の下（vulkan_context_transaction の中）、external_fence_prepare・reap_if・
  2 回の capacity の snapshot の置き場を決める。
- 測り方: base・A・B・A+B を別に（A が採られない時でも B を判定できる）。DEFERRED（今は GPU_COMMAND の EAGAIN）の回数も記録。
- 試験: plan/ws014/tests の host の harness（gpu-job.c・gpu-supervision.c・gpu-uapi-layout.c）を広げる。layout（56・16・32 byte）、失敗の注入
  （reserve の各段の EAGAIN、fence の EBUSY、stream の EFAULT、command の DEFERRED・EIO、admit の失敗、commit・cancel の失敗、outcome の
  copyout の EFAULT、別の thread の COMMIT・CANCEL・WAIT CONSUME、B の copyout の失敗・重複・PENDING・MISSING）、venus-reg、全 driver の build。
- 問題が無いと確かめられた所（reviewer）: admission の中の reserve に deadlock は無い、deadline 切れ・device lost は admit の ENODEV で FAULTED、
  別の thread は job_action の EBUSY と後の EALREADY、取りこぼしは無い、marker の順は今より厳しい、layout に padding 無し、番号は空き。

## 2026-10-31 の改訂（P3、q1785、main b689bbf66 の上、nvrtx を最初の driver に）

2026-10-31 Q1 から、採られた案 A・B を q1785 の中で今の main に合わせて直し、design-reviewer に通して Q1 に送り、uapi・gpu.c・libvulkan・nvrtx に
実装する指示を受けた（他の driver は任意の op と今の道で動き続ける）。上の本文と reviewer の節を前提に、変わる所だけを書く。

### 今の main との違い

- 番号: p023 が 'G' 46（GPU_IMAGE_LAYOUTS）と GPU_CAP 131072（IMAGE_LAYOUTS）を使った。案 A・B は上の 'G' 49・50 と GPU_CAP 262144・524288
  のまま（'G' 47・48 は空き、WS306 の予定は Q1 が 'G' 51〜53・GPU_CAP 1048576 に移した）。
- 最初の driver は amdrdna でなく nvrtx（RX 5600 XT は抜かれている。amdrdna は後で同じ形、それまで bit を出さず今の道）。
- nvrtx の今（WS291 p024、692209073）: runtime_command は session の worker の queue が空なら呼び手の thread で frame を走らせる。runtime_commit は
  queue が空なら marker をその場で取る（park か完了）ので、reviewer の M1（marker が worker の queue に残って次の同期の frame が EAGAIN）は nvrtx
  では起きない（実機 subs-1000 で command_ran 61923 回に deferred 24 回）。
- 実機の内訳（RTX 2070、subs-1000、c5 の診断、`GPU_IOCTL_TIMING`）: 1 submit に RESERVE kernel 4 µs、GPU_COMMAND kernel 21 µs、COMMIT kernel 2 µs、
  reap の WAIT 平均 1 本 2 µs、ioctl の間の user 計 約 11 µs。GPU core の monitor の起床を外した診断で subs-1000 +9 %（1 submit 約 3 µs）。

### 足す・変える所（design-reviewer の 2 回目 [job-submit-review2.md](job-submit-review2.md) を反映）

1. **outcome の規則（BL1・M-A、上の B1 を置き換える）**: CANCELED は「stream の仕事が 1 つも GPU に届いていない」時だけ。nvrtx では OOM などの
   失敗でも、前の command buffer や最初の push buffer が既に GPU に出ていることがある（vk-queue.c の queue_submit は最初の失敗で止まる、
   vk-execute.c は最初の push buffer を早く出す）。よって driver は frame の間に GPU に出した run の数を数え（nvrtx: `nvrtx_vk_gpu_submit` の
   成功ごとに `gpu_sends`）、COMMITTED（queue の操作を 1 つ以上走らせ全部成功）、CANCELED（GPU に出した run が 0）、FAULTED（他）。stream が
   失敗しても run が 0 なら CANCELED で、その errno を `gpu_job_submit.error`（旧 reserved、出力）で返す（kernel の一時の OOM で context を
   失わない）。op の約束: 0 か stream の errno を返し、outcome は必ず置く（EAGAIN は返さない）。
2. **monitor の起床（M-B・M-C）**: GPU_JOB_SUBMIT の中は reserve の時の起床をせず、action の pin を外す時に 1 回。その代わり、admit の前に
   reservation の期限（10 s）が過ぎていないかを確かめ、過ぎていれば FAULTED（monitor が見ていれば session を失わせたのと同じ）。今の道の
   1 submit の起床は 5 回（runtime_retire の capacity を含む）で、新しい道は 3 回になる見込み。+9 % の診断（4 回を外した）は期待値ではない。
3. **fence の無い submit の job**: 案 A の中でも job を残す（WS014 の決定を変えない、2026-10-31 Q1 も同意）。
4. **libvulkan（M-F）**: context の mutex は再帰しないので、GPU_JOB_SUBMIT は transaction の中で reserve の準備（fence の prepare・notification の
   作成）を済ませてから 1 本で送り、DEFERRED・EBUSY の時は mutex を外してから今の 3 本の道に戻る。EBUSY（payload が他で bind 中）は今の
   reserve の EBUSY と同じ扱い。今の 3 本の道の OOM の CANCEL も規則 1 で直す（BUG-381、COMMAND の reply が OOM でも GPU に出たかもしれない時は
   CANCEL_FAULT）。
5. **RETIRE**: libvulkan の reap は pin の無い record を 64 個ずつ。kernel は entry を stack に 64 個（1 KiB）持ち、registry lock の下で
   64×64 を見る（reviewer の minor、今は許容して記録）。
6. **試験（M-D・M-E）**: GPU core の fixture（plan/ws014/tests、9246cf6ea で全部 PASS）と新しい `plan/ws291/tests/gpu-job-submit.c`
   （committed・canceled（errno 付き）・deferred・faulted・期限切れ・RETIRE の pending・retired・missing・重複）を
   `plan/ws291/tests/run-gpu-job-host.sh` で、plain と ASan/UBSan。nvrtx の数え（queue_ran・queue_accepted）は vk-d3 の host 試験。
   nvrtx の runtime_job_command 自体と gpu_sends は host 試験に入らない（runtime.c・vk-node.c は host で build しない）: 実機の RTX で
   成功の道、失敗の道は未試験（fault の注入の knob は後の課題）。Venus は SUBMIT の bit を出さず、今の道と RETIRE だけを通る。

### 実装と実機の結果（2026-10-31 P3、[ws291-p024](phase024/phase.md) に詳細）

- kernel 84f786dce・5a03a0e95、libvulkan は phase024 の記録の commit。RTX 2070 の subs-1000（timing 無し）: c8（3 本の道）28.7 → 案 A・B 32.6 fps、
  subs-100 258.5 → 299.5 fps、big は揺れの内、100 copies の still は画素差 0。効果の基準（上）は 1 round ずつの比べで、boot ごとの揺れ（base で
  17.0〜20.0）より大きいが、同じ window の 3 round 交互の比べは未実施。
