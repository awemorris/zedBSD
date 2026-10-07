<!-- awesome-plan project=zedbsd record=ws083-p007 -->

# ws083-p007: VCS0 の engine 単位の reset（GRDOM_MEDIA）と video の hang の回復

Status: in-progress（q876、P2。2026-10-08 夜 host で進められる範囲を実装と host 試験。実機の人工の hang からの回復は 5330 が戻ってから T1）
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
