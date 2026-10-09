<!-- awesome-plan project=zedbsd record=ws083-p007 -->

# ws083-p007: VCS0 の engine 単位の reset（GRDOM_MEDIA）と video の hang の回復

Status: in-progress（2026-10-10 P2: host の範囲は全部済み。受け入れの残りは実機の人工の hang からの回復だけで、ユーザーが hang の image（下の「2026-10-10 実機の手順」）で起動し、Q1 が SSH で流す）（旧: in-progress（q876、P2。2026-10-08 夜 host で進められる範囲を実装と host 試験。実機の人工の hang からの回復は 5330 が戻ってから T1））
Disposition: normal
Parent: [WS083](../ws.md)
Queue: q876（Q1 の dispatch、承認済み）

## 範囲

[design.md](../design.md) §9 の p007（ws.md の表の旧 p007 の行は design の p008 の内容だったので、表を design に合わせた）: `GRDOM_MEDIA` の engine 単位の reset と VCS の hang の回復。design §6.1 の「p007 の engine reset で `i915_quarantine_release` に保留の list の解放を足す」。HD2 は既定の (a)（p002〜p006 は reset 無しで進め、WS121・WS122 に video を開く前に reset を入れる）。

この attempt は host で進められる部分: reset の手順（reset.c）、engine の再開（engine.c・submit.c）、worker の状態機械と保持物の回収（worker.c）、quarantine の解放、video の batch の扱い（render/video.c）、host の試験。実機（5330）での人工の hang からの回復は未実施。

## 見つけた不具合（直した）

- **reset domain の値の誤り**（`device-info.c`）: VCS0 の GDRST の bit が `1<<3`（Gen11 では GEN11_GRDOM_GUC）、VCS2 が `1<<5`（GEN11_GRDOM_MEDIA）、VECS0 が `1<<7`（GEN11_GRDOM_MEDIA3）だった。Linux 6.8（`intel_gt_regs.h`、`intel_engine_cs.c` の `get_reset_domain`）は VCS0 = MEDIA `1<<5`、VCS2 = MEDIA3 `1<<7`、VECS0 = VECS `1<<13`。今までは log（5330 の dmesg の `reset_domain=0x8`）にしか使われていなかったが、このまま engine reset を書けば VCS0 の代わりに GuC の domain を reset していた。直した後の dmesg の行は `vcs0 … reset_domain=0x20`。
- **SFC の割り当ての規則**（`device-info.c`）: media 12 の Alder Lake-P に Gen11 の規則（logical の偶数）を使っていて VCS2 に converter が無いことになっていた。Linux の `gen11_vdbox_has_sfc` の MEDIA_VER >= 12 の規則（physical の偶数、奇数は前の偶数が fuse で無い時）に直した。VCS2 の uabi の caps に SFC が付く。VCS0 は変わらない。

## 設計の判断（記録、ユーザーの判断が要るものは無し。変えたい時は Q1 経由で）

| ID | 判断 | 理由 | 他の案 |
| --- | --- | --- | --- |
| R1 | engine の reset は Linux の `__intel_gt_reset(engine->mask)` と同じく **1 回だけ**試す（ready にならない engine は reset しない）。失敗したら video を止める（下の DEAD） | engine mask の時の reference の形。Linux は失敗の後に full GT reset に上げるが、ここでは full reset は表示も止めるので上げない | 2 回目は ready を待たずに reset（Linux の full reset の retry の形）。実機で ready にならない例が出たら考える |
| R2 | hang の検出から reset・再開までを **worker の thread で同期に**行う（hang を見つけた `i915_worker_run` の中） | worker が execlists の状態の唯一の持ち主で、forcewake を握っている。次の request は reset の後に走るので、reset 中に別の thread が video を拒む必要が無い（`drv_i915_worker_video_state` は reset 中も 0） | 別の work に回す |
| R3 | 保持した context の record（LRC・ring・timeline page）は reset の後すぐには解放せず、**device の mutex の下**（次の attach、video の context の destroy、`i915_quarantine_release`）で回収する | record の表は device の mutex の下で埋めて空ける約束。worker の thread で GT の memory を解放すると attach と競合する | worker が解放する |
| R4 | 保持の印は「何番目の hang で保持したか」（`retained`）、worker は `video_hangs` と「reset が済んだ hang の番号」`video_recovered` を持つ。`retained <= video_recovered` の record だけ解放できる。番号の書き換えは IRQ lock の下 | 回収と次の hang が重なっても、reset の済んでいない record を解放しない | 状態の enum |
| R5 | hang の回数の上限 `I915_WORKER_VIDEO_HANG_LIMIT = 3`。4 回目の hang では reset せず video を止める（checked reset まで） | hang の 1 回ごとに 1 つの worker（render も）が request の timeout（10 秒）止まる。壊れた stream を繰り返す app に engine を与え続けない | 上限なし（Linux は context 単位の ban だけ）、時間で減る数え方 |
| R6 | rewind は CSB を処理せずに port を空け、context id を返し、`csb_errors` を 0 にして CSB の pointer を reset する | 1 つの engine に request は 1 つで、hang した request は捨てる。`csb_errors` は累積で、残すと以後の request が全部 EIO になる | reference の通り CSB を先に処理 |
| R7 | hang した session（quarantine）の video の batch は session の他の object と同じ quarantine に入れ、checked reset で解放する。quarantine でない session の batch は video の状態に関係なく解放する（今までは video が死んでいれば全部の batch を leak させていた） | engine は 1 度に 1 つの request しか走らせず、hang したのは quarantine の session の request。他の session の batch は hang した engine が読まない | video の状態で全部保持（今まで） |
| R8 | hang した session の quarantine は外さない（vm・object は checked reset まで残る。`drv_i915_render_close` の draw の object は今まで通り quarantine を見ない） | session の VkDevice は DEVICE_LOST で、Vulkan の規則では戻らない。quarantine の目印は device 全体の object の印で、hang の原因（VCS0 だけか RCS もか）を区別していないので、VCS0 の reset だけで解放すると RCS の hang の時の安全を崩す | VCS0 の hang だけが原因の quarantine を reset の後に解放する（印に原因を足す、別 Phase） |

## 実装（2026-10-08 夜、P2）

- `device-info.c`: GRDOM の値と SFC の規則（上の「見つけた不具合」）。
- `reset.c`・`reset.h`: `drv_i915_gt_reset_engine`（GT と engine の forcewake、uncore lock の下で `RING_RESET_CTL` の ready の要求（700 us、catastrophic error は handshake を飛ばす）→ converter の forced lock（VCS が MFX で使っていれば自分、HCP だけが使っていれば対の VECS（Wa_14010733141）、lock を得たら SFC の GDRST の bit を足す）→ GDRST の domain の reset（2 回、`i915_gt_domain_reset` を full reset と共通に）→ settle 50 us → unlock → ready の取り下げ）。`i915_quarantine_release` の最後で `drv_i915_worker_video_reclaim`。
- `engine.c`・`engine.h`: `drv_i915_engine_reset`（`__intel_engine_reset_bh`: reset_prepare（pause・stop_cs・MI_FORCE_WAKE の待ち）→ `drv_i915_gt_reset_engine` → rewind → serial++ → engine の workaround → render なら L3CC → execlists の有効化 → STOP_RING の解除）。`engine_resets`・`engine_reset_failures` の数。
- `submit.c`・`submit.h`: `drv_i915_execlists_reset_rewind`（R6）。
- `worker.c`・`worker.h`: R2〜R5。`i915_worker_video_hung` は record を保持 → 上限を超えたら停止 → `drv_i915_engine_reset` → 失敗なら停止、成功なら `video_recovered`。`drv_i915_worker_video_reclaim`（新、公開）。`drv_i915_worker_engine_reset` は VCS0 の record で reset と再開をし、全部の hang を recovered にして停止を解く（checked reset の経路。render と copy は今まで通り ENOTSUP）。record の解放は `i915_worker_context_release` に共通化。log: `i915: video: request failed (error E); hang N, context sw_id=S retained`、`i915: video: engine reset after hang N; video takes work again`、`i915: video: engine reset failed (rc=E); video engine stopped until a checked reset`、`i915: video: N hangs; video engine stopped until a checked reset`、`i915: engine_reset vcs0 domains=0x20 passes=2 rc=0`、`i915: vcs0: engine reset; the engine takes work again`。
- `render/video.c`: R7。log の文言（`session quarantined`）。

## 確認

| コマンド | 結果 |
| --- | --- |
| `make -j16 BUILD=build/p2-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p2-k/vmunix` | 成功、warning 0（`-Werror`）、kernel include check・amd64 vmunix check PASS |
| `sh plan/ws083/tests/run-host-vcs-worker.sh` | plain・ASan/UBSan とも PASS。追加の case: hang → reset 成功（reset は VCS0 の index に 1 回、video は続く、record は retained のまま）→ destroy で回収（lrc の解放 1、live 0）→ 次の attach が同じ record を使い request は VCS0 へ。上限: 3 回は reset、4 回目は reset せず停止、回収は recovered の物だけ。reset の失敗で停止、`drv_i915_worker_engine_reset(VCS0)` の失敗は停止のまま、成功で停止が解け回収できる、RCS0・BCS0 は ENOTSUP |
| `sh plan/ws083/tests/run-host-video-roundtrip.sh` | PASS（plain・ASan/UBSan、genxml 36 instruction 89 check）。追加: decode が ETIMEDOUT で終わると DEVICE_LOST・session が quarantine・destroy_session で batch が quarantine に入る（`stub_gem_retained == 1`） |
| reset の contract 試験（新 `src/drivers/gpu/i915/tests/contracts/reset_contract_test.c`）を run.sh と同じ compile で `build/tmp` で手で流した（run.sh は trap に rm があるので Q1 に依頼） | ordinary・ASan/UBSan とも 34 check 0 failure: domain の無い engine は EINVAL で何も書かない、VCS0 の reset は ready の要求 → GDRST に 0x20 を 2 回 → ready の取り下げ、converter が使用中なら lock → GDRST 0x20020 → unlock、ready にならなければ GDRST を書かず ETIMEDOUT・取り下げはする、catastrophic error は handshake を飛ばして reset、GDRST が消えなければ 1 回で ETIMEDOUT、checked reset は GT の reset・3 つの engine record の reset・quarantine の object と vm の解放・video の回収・failed の解除 |
| mmio の contract 試験（mock_mmio の変更の確認、手で） | 43 check 0 failure |

未実施: 実機（5330）での VCS0 の reset（人工の hang: 終わらない batch（`MI_SEMAPHORE_WAIT` など）を VCS0 に流し、timeout → reset → 次の decode が通ることを `i915.debug=video` で）。GDRST・RESET_CTL・SFC の lock の実機の応答。i915 の contract 試験の run.sh（Q1 に依頼）。

## 実機で見る log（5330 が戻った時の T1 の依頼の材料）

- 起動: `i915: engine[2] vcs0: class=1 inst=0 base=0x1c0000 reset_domain=0x20 …`（0x8 でないこと）。
- 人工の hang の後: `i915: resident shim: XXX request seqno=… did not complete in 10000 ms` → `i915: video: request failed (error 110); hang 1, context sw_id=… retained` → `i915: engine_reset vcs0 domains=0x20 passes=2 rc=0` → `i915: vcs0: engine reset; the engine takes work again` → `i915: video: engine reset after hang 1; video takes work again`。その後の正しい stream の decode の hash が p005 の値と一致すること、render（desktop）が止まらないこと。
- 失敗の形: `i915: vcs0 reset request timed out: request 00000001 RESET_CTL ……`（ready にならない）、`engine_reset vcs0 … rc=110`（GDRST が消えない）。

## 残り

- 実機の確認（上）。人工の hang を起こす試験の app（vkvideo-probe の option など）は p005 の bring-up の後に作る。
- R8 の「VCS0 の hang だけの quarantine を reset の後に解放する」は必要なら別 Phase。
- 他の WS の試験の runner の壊れ（下の「Q1 への連絡」）。

## Q1 への連絡（この Phase の外）

- `plan/ws031/tests/run-vk-host-tests.sh` と `plan/ws075/tests/run-host-layered.sh` の executor の list に `render/forget.c`（BUG-260 で増えた）が無く、main の時点で link に失敗する（`drv_i915_gfx_forget`・`drv_i915_gfx_view_lookup`・`drv_i915_gfx_op_names`）。WS083 の roundtrip の runner は直した（`forget` を足した）。
- `plan/ws031/tests/i915-vk-render-stubs.inc` に `drv_i915_gem_destroy` の stand-in と `stub_batch_run_error` を足した（video.c の R7 のため。WS083 の video の stand-in が既にこの file にある先例に合わせた）。

## 人工の hang の道具（2026-10-09 夜、P1。Q1 の指示「T1-435 の前に host でできる残り」）

- 形: 試験の build だけの compile 時の fault injection（Makefile の `ZEDBSD_TEST_CPPFLAGS`、「Private test builds may add compile-time fault injection」。先例 `I915_TEST_HDMI_ABSENT`）。`-DI915_TEST_VIDEO_HANG_AT=N`（任意で `-DI915_TEST_VIDEO_HANG_COUNT=K`、既定 1）を与えた kernel では、起動からの video の run（1 decode = 1 run、全 session を通した番号、1 から）のうち N〜N+K−1 番の batch の decode の後に `MI_ARB_CHECK` と自分へ戻る `MI_BATCH_BUFFER_START`（PPGTT）を置く（igt の spinner と同じ形）。decode は走り、engine は loop を回り続け、request は VCS0 の 1 秒の timeout で失敗し、worker が本物の hang と同じ経路で engine を reset する。log `i915: video: I915_TEST_VIDEO_HANG_AT: run N ends in a loop that does not finish`。macro が無い build（製品）では code も変数も無い（`render/video.c` の `#ifdef I915_TEST_VIDEO_HANG_AT` の中だけ）。
- 確かめ（host・build）:
  - `make -j16 BUILD=build/p1-ws083 ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p1-ws083/vmunix`（macro なし）と `… BUILD=build/p1-ws083-hang "ZEDBSD_TEST_CPPFLAGS=-DI915_TEST_VIDEO_HANG_AT=2" build/p1-ws083-hang/vmunix` の両方 rc 0・warning 0（kernel include check・amd64 vmunix check PASS）。log の文字列は hang の vmunix に 2、製品の vmunix に 0。
  - executor の host 試験（`run-host-video-roundtrip.sh` と同じ compile、ASan・UBSan）を `-DI915_TEST_VIDEO_HANG_AT=1 -DI915_TEST_VIDEO_HANG_COUNT=100000` で: PASS、`idr.bin` の 2 つの run の末尾に `0x02800000 0x18800101 <va>` があり、飛び先が各 run の MI_ARB_CHECK の位置（batch の va 0x7000020000 ＋ 309×4・298×4）と一致。
  - style-check の指摘は変更の前と同じ（新しい指摘 0）。
- 実機の手順（T1-435 の A〜E の後、同じ 5330 の passthrough、`/tmp/i915-hw.lock` の下。T1 への依頼の文の案、番号は Q1）:
  - F1 回復: image `BUILD=build/t1-ws083hang ZEDBSD_CONFIG=plan/ws083/tests/config-video-hw.mk`、起動は `plan/ws075/tests/test-hw.sh "vkx -DI915_TEST_VIDEO_HANG_AT=2" OUT`（vkloop-hw.sh が scenario の後の flag を `ZEDBSD_TEST_CPPFLAGS` に足す）。SSH で `vkvideo-probe --expect=/tmp/v/i-baseline-64.sha256 /tmp/v/i-baseline-64.h264` → 2 番目の decode が hang し、probe は DEVICE_LOST で非 0 の exit。dmesg に順に `run 2 ends in a loop`、`did not complete in 1000 ms`、`video: request failed (error 110); hang 1, context sw_id=… retained`、`engine_reset vcs0 domains=0x20 passes=2 rc=0`、`vcs0: engine reset; the engine takes work again`、`video: engine reset after hang 1; video takes work again`、`decode failed on VCS0 … session quarantined`。続けて同じ命令をもう一度 → exit 0・`3 frames decoded, 3 match the reference`、`pb-main-352` も一致。desktop（render）が止まらない（dmesg に rcs0 の hang・reset の行が無い、SSH が切れない）。
  - F2 上限（R5）: 新しい image `test-hw.sh "vkx -DI915_TEST_VIDEO_HANG_AT=1 -DI915_TEST_VIDEO_HANG_COUNT=4" OUT`。probe を 5 回（各回の最初の decode が hang）→ 1〜3 回目は reset、4 回目は `video: 4 hangs; video engine stopped until a checked reset`（reset しない）、5 回目は `the video engine is stopped; the decode did not run`。SSH と desktop は生きている。
  - 失敗の形（返す物）: `vcs0 reset request timed out: request 00000001 RESET_CTL …`（ready にならない、R-S4 の材料）、`engine_reset vcs0 … rc=110`（GDRST が消えない）、F1 の 2 回目の probe の不一致。dmesg の `i915:` の全行と probe の全出力。
- 状態: 道具は済み。Phase は実機（F1・F2）まで in-progress。
- 2026-10-10: この F1・F2 の手順（T1 が ESP に kernel を置く形）は、T1 の ESP の書き込みが Claude Code の安全の判定で止められたので、下の「2026-10-10 実機の手順」（ユーザーが image か kernel を置く、1 回の起動で F1・F2）に置き換えた。

## 2026-10-10 P2: host の残りと実機の手順（ユーザー「P2はWS083を完了させたらp007, p008を完了させたらラップアップ。実機でテストするので詳細なQEMUテストは不要です。」）

### host の範囲（済み）

- i915 の contract 試験（reset・mmio）: run.sh は trap に rm があるので、同じ compile を `build/tmp/p2-contracts.run.*`（`fresh_out`）で手で流した。reset 34 check・mmio 43 check、ordinary・ASan/UBSan とも 0 failure（main 1b08d90b9 の上）。
- WS083 の host 試験 8 本（`run-host-*.sh`）は 1b08d90b9 で全部 PASS（p002 等の照合の時）。
- 他の WS の runner の `forget` の欠け（上の「Q1 への連絡」）は main で直っている（ws031・ws075 の list に `forget` がある）。
- **人工の hang の間隔**（新、試験の build だけ）: `render/video.c` の `#ifdef I915_TEST_VIDEO_HANG_AT` の中に `I915_TEST_VIDEO_HANG_STEP`（既定 1 = 今までどおり連続）を足した。hang する run は AT から STEP おきに COUNT 個。STEP 2 なら hang の間に 1 つ decode が通るので、**1 回の起動で F1（reset の後の回復）と F2（4 回目の hang で停止）の両方**を見られ、ユーザーが image（か kernel）を置くのは 1 回で済む。STEP が 1 未満なら `#error`。製品の build（macro なし）には code も文字列も無い。
  - 確かめ: roundtrip の executor（`run-host-video-roundtrip.sh` を CC の包みで `-DI915_TEST_VIDEO_HANG_AT=1 -DI915_TEST_VIDEO_HANG_STEP=2 -DI915_TEST_VIDEO_HANG_COUNT=100000`）は plain・ASan/UBSan とも PASS、`idr.bin`（IDR と P の 2 run）の loop は 1 つ（run 1、dword 310 の `MI_BATCH_BUFFER_START` の飛び先が dword 309 の `MI_ARB_CHECK` = va 0x70000204d4）。STEP 無し（`AT=1 COUNT=100000`）は前と同じ 2 つ（dword 310・612）。genxml の照合はどちらも loop の分で 38 命令になり不一致（試験の build の batch なので期待どおり、製品の build の roundtrip は PASS）。
  - build: `make -j16 ZEDBSD_CONFIG=plan/ws083/tests/config-video-hang.mk BUILD=build/p2-hang build/p2-hang/vmunix` rc 0・warning 0（kernel include check・amd64 vmunix check PASS、stamp に `-DI915_TEST_VIDEO_HANG_AT=2 -DI915_TEST_VIDEO_HANG_STEP=2 -DI915_TEST_VIDEO_HANG_COUNT=4`、log の文字列 2 つ）。製品 `make -j16 BUILD=build/p2-k ZEDBSD_CONFIG=config/ci/config-amd64.mk build/p2-k/vmunix` rc 0・warning 0、`HANG_AT` の文字列 0。
- 新 `plan/ws083/tests/config-video-hang.mk`: `config/current-uat.mk` ＋ `i915.debug=video` ＋ vkvideo-probe ＋ 試験の stream を `/root/ws083/` ＋ `ZEDBSD_TEST_CPPFLAGS`（既定 `AT=2 STEP=2 COUNT=4` = 起動から 2・4・6・8 番目の video の run が hang）。release の image には使わない。
- R-S4（ready にならない時の 2 回目の GDRST）は R1 のとおり実機で ready にならない例が出たら考える（host でできる事は無い）。

### 2026-10-10 実機の手順（ユーザーに頼む形、ESP に書くのはユーザー）

前提: T1・Q1 は ESP に書かない（2026-10-10 の安全の判定）。5330 の SSH は kei@10.0.30.3。hang の kernel は video の engine を**わざと**止めるので、試験の後は必ず元に戻す。

**1. 準備（ユーザー、どちらか 1 つ。推しは (b)、USB の中身が残り、置くのは file 1 つ）**

- (a) image ごと: tree の上で `make -j16 ZEDBSD_CONFIG=plan/ws083/tests/config-video-hang.mk BUILD=build/ws083-hang disk-image` → `build/ws083-hang/hdd-image.img` を USB に書き（UAT の image と同じ書き方、USB の中身は消える）、5330 をその USB で起動。試験の後は普段の UAT の image を書き戻す。
- (b) kernel だけ（今の 5330 の image が `i915.debug=video` 入りで、kernel と同じ tree の時。Q1 が先に SSH の `uname -a` の revision と、その tree との間に kernel と userland の ABI を変える commit が無いことを確かめる）: Q1 が `make -j16 ZEDBSD_CONFIG=plan/ws083/tests/config-video-hang.mk BUILD=build/ws083-hang build/ws083-hang/vmunix` を作り、5330 の `/tmp/vmunix-hang` に scp する（ESP には書かない）。ユーザーが 5330 の Terminal で:
  ```
  sudo mount -t msdos /dev/sda1 /mnt        # ESP（USB の 1 番目の FAT。違えば ls /dev/sda* で）
  sudo cp /mnt/vmunix /mnt/vmunix.orig      # 元の kernel を残す
  sudo cp /tmp/vmunix-hang /mnt/vmunix
  grep i915.debug /mnt/zedbsd.cfg           # i915.debug=video の行があること（無ければ 1 行足す）
  sudo umount /mnt
  sudo reboot
  ```
  元に戻す: `sudo mount -t msdos /dev/sda1 /mnt && sudo cp /mnt/vmunix.orig /mnt/vmunix && sudo umount /mnt && sudo reboot`（`vmunix.orig` は残っても害は無い）。

**2. 試験（Q1 か T1 が SSH で、起動の後に video を使う物を他に動かさない。各行は 1 decode = 1 run）**

`P` = `sudo vkvideo-probe --frames=1 --expect=/root/ws083/i-baseline-64.sha256 /root/ws083/i-baseline-64.h264`（(b) で probe・stream が image に無ければ `/tmp/v/` に scp したもの）。起動の後、`sudo dmesg | grep 'i915:'` を各段の後に取る。

| 段 | 命令 | 期待（probe） | 期待（dmesg の新しい行） |
| --- | --- | --- | --- |
| 0 | `sudo dmesg \| grep i915:` | — | `Vulkan video decode is offered on a GT with VCS0`、`engine[2] vcs0 … reset_domain=0x20` |
| 1 | P（run 1） | exit 0、`1 frames decoded, 1 match the reference` | 無し |
| 2 | P（run 2、hang 1） | 非 0 の exit（DEVICE_LOST） | `I915_TEST_VIDEO_HANG_AT: run 2 ends in a loop`、`did not complete in 1000 ms`、`video: request failed (error 110); hang 1, context sw_id=… retained`、`engine_reset vcs0 domains=0x20 passes=2 rc=0`、`vcs0: engine reset; the engine takes work again`、`video: engine reset after hang 1; video takes work again`、`decode failed on VCS0 … session quarantined` |
| 3 | P（run 3） | exit 0・1 match（**F1: reset の後に正しく decode**） | 無し |
| 4 | P（run 4、hang 2） | 非 0 | 段 2 と同じ形で hang 2 |
| 5 | P（run 5） | exit 0・1 match | 無し |
| 6 | P（run 6、hang 3） | 非 0 | 同じ形で hang 3（reset する） |
| 7 | P（run 7） | exit 0・1 match（3 回の reset の後も回復） | 無し |
| 8 | P（run 8、hang 4） | 非 0 | `video: 4 hangs; video engine stopped until a checked reset`（**F2: reset しない**、`engine_reset vcs0` の行は無い） |
| 9 | P、続けて `sudo vkvideo-probe --list` | P は非 0 で `vkvideo-probe: no queue family decodes H.264`（停止した engine は新しい device に video の family を出さない）、`--list` は `video families 0, video extensions 0` | 新しい `capset declares H.264 video decode` の行が出ない |
| 10 | `ps ax \| grep -c wayland`、SSH が生きている | compositor が居る | rcs0 の hang・reset の行が無い（desktop は止まらない） |

返す物: 各段の probe の全出力と dmesg の `i915:` の全行。失敗の形: `vcs0 reset request timed out: request 00000001 RESET_CTL …`（ready にならない、R-S4 の材料）、`engine_reset vcs0 … rc=110`（GDRST が消えない）、段 3・5・7 の不一致。hang が段の外で起きたら止めて Q1 へ。所要は 10 分ほど（hang 1 回 1 秒）。

**3. 後始末（ユーザー）**: (a) は UAT の image を書き戻す、(b) は上の「元に戻す」。`uname -a` が元の kernel。

受け入れ（design §9 の p007: host の試験、実機で人工の hang からの回復）: 段 1〜10 が期待どおりなら cleared 候補。

### 再開点

host の範囲は終わり。次はユーザーの準備（1）と Q1 の SSH（2）。結果が来たら Q1 が判定（FAIL の解析は新しい attempt）。
