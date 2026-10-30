<!-- awesome-plan project=zedbsd record=ws009-p024 -->

# ws009-p024: DOC-54 正確さの敵対的レビュー（Opus を推す）

Status: in-progress（2026-10-30 P1、q1704-i01。review と直し済み・照合 PASS。cleared は Q1 が決める）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Opus（設計の担当の推し。推論が多い）
依存: p023 が main に merge 済み
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- docs/reference/gpu*.md の全文
- include/uapi/gpu*.h
- src/drivers/gpu/gpu.c・gpu-fence.c・各 driver（主張を確かめる所）

## 2. 作る物

- `docs/reference/gpu*.md（誤りの直しだけ）`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。
- 見つけた header・code の食い違い（Bug の候補）と、tree の他の comment と code の食い違い（例: userland/desktop/sessiond/seat.c の node の open の comment）の一覧を Q1 に送る。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 全 file で `--final` と `--examples` が PASS。4 節の照合の結果を表で結果の節に。

## 4. 注意

- 各 file から主張を抜き出し（errno の条件・寿命・並行・待ちの上限・capability の要否）、source と照らす。少なくとも要求ごとに 1 つ、全体で 60 以上。
- 誤りは直し、表（主張・file・根拠・結果）を結果の節に置く。header・code の側の食い違いは Q1 に送る。
- 書き方の規則（gpu-doc-rules.md 2 節、docs/style.md）の見直しも行う。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu*.md plan/ws009/phase024/phase.md plan/ws009/ws.md`。

## 結果

日付: 2026-10-30（host の `date`）。担当 P1（Opus 5.5）、q1704-i01。worktree `/home/awe/zedBSD-worktrees/p1-ws009r`、起点 d73c023ef。commit: 0b5cf893f・90fd118d8・67664f2df・b2f7ed761・8dbb0c0e5（文書）、ecbabaf61 と この記録（phase.md・ws.md）。

進め方: gpu.md・gpu-requests.md と文書どうしの食い違いは P1 が自分で読み、他の 12 file は 4 つの群（memory・sharing／submission・fences・jobs／display・display-control／capabilities・drivers・command-stream・sysctl・examples）に分けて、同じ規則で読む fork に review と直しを任せた。fork の直しは P1 が diff を読み、主張の幾つかを source で読み直してから commit した（下の「読み直し」）。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（着手の時と commit の前）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 0 placeholders, final 14 files, 12 example compiles)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: 同じ PASS（final 0 files。見出しの変更なし）。
- `timeout 120 .../noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (676 links)`
- `git diff --check`: 出力なし。
- 追加の文書の差分（d73c023ef から）に `plan/`・WS・Phase・Queue・Bug の番号・予定の語が無いことを grep で確かめた（0 件）。
- 注: script は `--final` の後ろを file の一覧として読むので、file を渡す時は `--final` を最後に置く（`--final --examples a.md` は unknown argument で止まる。script は変えていない）。

### 直した物（file ごと、要点）

- gpu.md: (1) `GPU_FENCE_CREATE` の descriptor は device を保たない（`gpu_shared_hold_device` を呼ぶのは resource と allocation の export だけ。fence は `device_id` だけで device を指す）。resource・allocation の descriptor だけが回復と取り外しを止める、に。(2) 番号の再利用は、全 open の close に加え export の descriptor の close も要る（`drv_gpu_unregister` は sessions か shares が 0 でないと EBUSY で、driver が繰り返す）。(3) 故障の device: 全ての要求が失敗するのではなく、admission を取る要求・admission の外の display の要求・`GPU_JOB_CAPACITY`・新しい submit と予約が記録の errno で失敗し、`GPU_COMMAND_WAIT`・`GPU_JOB_POLICY`・`GPU_FENCE_BIND` 以外の fence の要求は答える。`GPU_FENCE_BIND` と signal fence を結ぶ要求は記録の errno でなく `ENODEV`（`gpu_fence_binding_add`）。(4) reset の間に新しい故障が来た open は新しい記録の errno か `EIO`（`gpu_recover_open` の fault_epoch）。(5) 不明な要求も先に admission を取るので、故障の device では admission の errno、待ちの signal では `EINTR`。(6) 他の open の lease の errno は EBUSY だけでなく i915・venus の一部は EINVAL・EPERM。(7) `GPU_JOB_CAPACITY` は device の queue と open 自身の空き record を合わせて答える。(8) poll の POLLIN・POLLRDNORM はそれぞれ要求した時だけ。(9) image descriptor の EOPNOTSUPP に memory type を足した。(10) close の fence: open の記録の errno（未故障なら ENODEV）で終える、ただし走っている commit 済みの job の fence はその job が signal する（`gpu_session_fail_locked` の graceful の分岐）。
- gpu-requests.md: ETIMEDOUT（kernel・driver の 1 秒の上限、adoption の待ちの ETIMEDOUT）、EINTR（`GPU_RESOURCE_DESTROY` の pin の待ち、signal を見る driver の display の待ち）、ENODEV（BIND と signal fence を結ぶ要求）、EAGAIN（「job が持たない」を「commit 済みの job が持たない」に。BIND だけの fence と予約だけの job の fence は持たれない扱い：`drv_gpu_fence_admit` を呼ぶのは `gpu_job_admit` だけ）。
- gpu-memory.md: 第 3 の種類（foreign の scanout import）を「display の要求だけ」から事実に。`GPU_BLOB_MAPPABLE` を見る driver（PowerVR・Mali・BCM2711・Venus・ghci）と見ない driver（amdrdna・nvrtx・i915）。GPU の mmap では `MAP_FIXED` は hint で、`MAP_FIXED_NOREPLACE` だけが正確に置く（`EEXIST`）。cache の属性を driver ごとに正確に。`GPU_BLOB_SHAREABLE` を拒むのは Venus と BCM2711 だけ。DESTROY の Other values に記録の errno。MAP の EIO と Other values。
- gpu-sharing.md: EXPORT・ALLOCATION_EXPORT の Other values を driver ごとの実際の errno に。
- gpu-submission.md: kernel の stream の写しは driver の op が返った時に捨てる（「driver が終えるまで」は誤り）。4 の倍数の検査は driver ごと（Venus は拒む）。device の取り外しで driver の持つ record が ENODEV で終わる事。
- gpu-fences.md: `_SYNC` の EAGAIN を「commit 済みの job が持たない」に。open・device の故障は結んだ target を記録の errno で終える（pending に残らない）。PRESENT_SYNC の EPERM。FENCE_BIND の ESTALE を bind と release に分けた（release で binding が無ければ EPERM）。FENCE_SIGNAL の ESTALE（signal の後の reset）。
- gpu-jobs.md: commit の失敗の行と ENODEV（job・open・device が間で終わった時。EIO は fault cancel を driver が受けた時だけ）。capacity の sequence の上限。supervisor は close でも始まる。
- gpu-display.md: 未接続の ENXIO は「通常」（ghci の claim は通す）。lease の要らない要求に OVERLAY_INFO。`ack_sequence` の規則（受け取った最大を越えない・今の sequence を越えない）を 3 か所。最初の flip は planes によらず成功した commit も数える。WAIT: i915 の latch の待ちと EIO、bcm2711 の未知の sequence（待ってから EAGAIN・ETIMEDOUT）、venus の ENODEV。CLAIM の未知の display_id と EIO。MODE の bcm2711 ENOTSUP。CONSTRAINTS の reserved の EIO・bcm2711 ENXIO・記録の errno。`GPU_DISPLAY_ACTIVE`・`GPU_DISPLAY_BLOB` の行（bcm2711 の例外）。
- gpu-display-control.md: CURSOR_SET・MOVE・HIDE・COMMIT・OVERLAY_INFO の ENODEV を「取り外し」だけにし、記録の errno を足した（`gpu_display_admit`）。COMMIT の未使用の plane の検査を正確に、driver の ENODEV・EFAULT。venus・i915 の refresh の境、i915 の power、powervr の cursor の EOPNOTSUPP。
- gpu-capabilities.md: cursor・commit・overlay の op が無い時に EOPNOTSUPP を返すのは kernel。bcm2711 の 176 byte の capset の条件。
- gpu-drivers.md: i915 の最初の行の output flags（FIFO・BLOB は全 output、CONNECTED・REFRESH_COUNTER は常駐の output、POWER_CONTROL は eDP）。cursor の行。lease の後に「text console が描く」とある行は driver の依頼であり、最初の flip の後は効かない、と明記（`kern_text_end` は「for good」）。
- gpu-examples.md: display の例の「1 回の wait は最大 1 秒」を ghci・bcm2711 だけに、i915 は latch まで待つ（本文と例の comment）。
- gpu-command-stream.md・gpu-sysctl.md: 直し無し（読んで合っていた）。

### 4 節の照合の表（主張・file・根拠・結果）

| # | 主張（file の節と要約） | 根拠（file と関数） | 結果 |
| --- | --- | --- | --- |
| 1 | gpu.md 名前: 一番小さい空き番号、probe の順 | gpu.c `drv_gpu_register` | OK |
| 2 | gpu.md 名前: 番号の再利用の条件 | gpu.c `drv_gpu_unregister`（sessions・shares で EBUSY） | 直した |
| 3 | gpu.md: devfs の既定 root・0666、番号で owner・mode を覚える | devfs.c 作成の mode の分岐、`devfs_cdev_attributes_apply` | OK |
| 4 | gpu.md: sessiond は gpu0〜3 を 0600 で渡し、root・0666 に戻す | seat.c `sessiond_seat_give`・`sessiond_seat_restore` | OK |
| 5 | gpu.md: root と owner だけ、credential 無しも EACCES | gpu.c `gpu_open` | OK |
| 6 | gpu.md: open の mode を open の時に記録 | gpu.c `gpu_open`（readable・writable） | OK |
| 7 | gpu.md: mmap は readable、PROT は open の権限まで、mprotect も | gpu.c `gpu_mmap` | OK |
| 8 | gpu.md: 故障の device では要求が失敗 | gpu.c `gpu_session_error_locked` の呼び手、fence・policy・wait の関数 | 直した（範囲） |
| 9 | gpu.md: 回復は唯一の open・shares 0・reset op | gpu.c `gpu_recover_open` | OK |
| 10 | gpu.md: reset 中の新しい故障 | gpu.c `gpu_recover_open`（fault_epoch、EIO） | 足した |
| 11 | gpu.md: reset を待つ open は割り込み可、ENODEV、EOVERFLOW、ENOMEM | gpu.c `gpu_recover_open`、`gpu_open` | OK |
| 12 | gpu.md: version・size の例外（_SYNC の入れ子、placed の 64） | gpu.c `gpu_dependency_ioctl`、gpu-scanout.h | OK |
| 13 | gpu.md: import の descriptor は入力で全 0 | gpu.c `gpu_import_ioctl`・`gpu_allocation_import_ioctl` | OK |
| 14 | gpu.md: export の EINVAL・EOPNOTSUPP | gpu.c `gpu_image_validate` | 直した（memory type） |
| 15 | gpu.md: descriptor を返す欄は入力で -1 | gpu.c `gpu_export_ioctl`・`gpu_allocation_export_ioctl`・`gpu_fence_create_ioctl` | OK |
| 16 | gpu.md: 不明な要求は EOPNOTSUPP | gpu.c `gpu_ioctl_body`（default の前に `gpu_session_enter`） | 直した |
| 17 | gpu.md: handle 0・破棄・他の open は EINVAL、再利用しない | gpu.c `gpu_resource_lookup`、`gpu_handle_allocate` | OK |
| 18 | gpu.md: fence の EBADF・EINVAL・EXDEV・ESTALE | gpu.c `gpu_fence_get`、gpu-fence.c `fence_resolve`・`drv_gpu_fence_device` | OK |
| 19 | gpu.md: admission を取る要求の一覧 | gpu.c `gpu_ioctl_body` の switch、`gpu_mmap` | OK |
| 20 | gpu.md: admission の外の要求の一覧、ungated は ghci・bcm2711 | gpu.c `gpu_ioctl_body`、`gpu_display_ungated`、ghci/display.c・bcm2711 display-device.c の flags | OK |
| 21 | gpu.md: commit の op を持つのは ungated の driver だけ | gpu.c `gpu_commit_ioctl`、ghci の ops（commit NULL） | OK（bcm2711 だけ） |
| 22 | gpu.md: admission の待ちは EINTR・ENODEV・記録の errno | gpu.c `gpu_session_enter` | OK |
| 23 | gpu.md: export の descriptor が回復と取り外しを止める（fence を含む） | gpu.c `gpu_shared_hold_device` の呼び手は export 2 つだけ | 直した |
| 24 | gpu.md: lease の他の open は EBUSY | venus/display.c（EPERM）、i915/display/present.c（EINVAL） | 直した |
| 25 | gpu.md: JOB_CAPACITY は device 全体 | gpu.c `gpu_capacity_ioctl`（records も見る） | 直した |
| 26 | gpu.md: 待ちの deadline は tick に切り上げ | gpu.c `gpu_completion_deadline` | OK |
| 27 | gpu.md: COMMAND_WAIT の 0・有限・UINT64_MAX・EINTR | gpu.c `gpu_completion_observe`、waitq.c `waitq_sleep`（ETIMEDOUT） | OK |
| 28 | gpu.md: JOB_CAPACITY の 0 は変化も空きも無い時 EAGAIN | gpu.c `gpu_capacity_ioctl` | OK |
| 29 | gpu.md: DISPLAY_WAIT の ghci は 1 秒の上限、signal で終わらない | ghci/display.c `display_wait`（WAIT_MAX_NS） | OK |
| 30 | gpu.md: REFRESH は kernel が 1 秒に切る | gpu.c `gpu_refresh_ioctl` | OK |
| 31 | gpu.md: adoption の待ち ghci 約 12 秒 | ghci/display-scan.h `GHCI_DISPLAY_ADOPTION_MS`（HANG 10000 + 2000） | OK |
| 32 | gpu.md: DESTROY の pin の待ちは EINTR で変化なし | gpu.c `gpu_destroy_ioctl` | OK |
| 33 | gpu.md: SA_RESTART で ioctl・open を再実行 | syscall.c `syscall_restartable` | OK |
| 34 | gpu.md: close の fence の終わり方 | gpu.c `gpu_close`、`gpu_session_fail_locked`、`gpu_fence_retire` | 直した |
| 35 | gpu.md: close は lease の keyboard の hold を返す | gpu.c `gpu_close`（`tty_console_input_unhold`） | OK |
| 36 | gpu.md: poll の各 event | gpu.c `gpu_poll`、`gpu_events_snapshot`（sequence 0 は EIO） | 直した（POLLIN） |
| 37 | gpu.md: Headers の要求の番号の範囲 | include/uapi/gpu*.h の `_IO*('G', n)` | OK |
| 38 | gpu.md: `__ZEDBSD__`・`KERN_UAPI_NATIVE` が無いと host の macro | uapi/hosted.h、uapi/ioctl.h | OK |
| 39 | gpu-requests.md: 検査の順（display・EVENTS・REFRESH・fence は capability が先） | gpu.c `gpu_display_ioctl`・`gpu_events_ioctl`・`gpu_refresh_ioctl`・`gpu_fence_ioctl`・`gpu_job_*` | OK |
| 40 | gpu-requests.md: open mode none（GET_INFO・CAPSET・DEVICE_QUERY・CONSTRAINTS・JOB_CAPACITY・POLICY） | 各 handler に readable・writable の検査が無い | OK |
| 41 | gpu-requests.md: ESRCH・EMFILE は descriptor の導入 | gpu.c `gpu_export_install` | OK |
| 42 | gpu-requests.md: EAGAIN の fence | gpu-fence.c `fence_wait`（admitted）、gpu.c `gpu_job_admit` | 直した |
| 43 | gpu-requests.md: ETIMEDOUT・EINTR の範囲 | 上の 27〜32 | 直した |
| 44 | gpu-requests.md: ENODEV と binding | gpu.c `gpu_fence_binding_add` | 直した |
| 45 | memory: handle・sequence・device id は一つの counter | gpu.c `gpu_handle_allocate` | OK（fork） |
| 46 | memory: 第 3 の種類は display の要求だけ | gpu.c `gpu_transfer_ioctl`・`gpu_map_ioctl` は種類を見ない | 直した（fork） |
| 47 | memory: MAPPABLE を全 driver が見る | amdrdna・nvrtx `node_resource_map`、i915 `i915_resource_map` | 直した（fork） |
| 48 | memory: mmap の MAP_FIXED | syscall.c mmap の device の分岐（`device_exact`） | 足した（fork。P1 が読み直し） |
| 49 | memory: cache の属性 | bcm2711 `render_map`、ghci の host_uncached | 直した（fork） |
| 50 | memory: SHAREABLE を全 driver が拒む | venus `share_export`、bcm2711 `render_export`、他は見ない | 直した（fork） |
| 51 | memory: RESOURCE_CREATE・BLOB_CREATE・PLACED・READ・WRITE・DEVICE_QUERY の errno | gpu.c 各 handler、`gpu_resource_reserve` | OK（fork） |
| 52 | sharing: EXPORT の image の検査、IMPORT の EXDEV、allocation の metadata | gpu.c `gpu_export_ioctl`・`gpu_import_ioctl`・`gpu_allocation_validate` | OK（fork） |
| 53 | submission: SUBMIT の写しの寿命 | gpu.c `gpu_submit_ioctl` | 直した（fork） |
| 54 | submission: SUBMIT の sequence を driver の前に書き出す、断ると ENOENT | gpu.c `gpu_submit_ioctl`、`gpu_completion_discard` | OK（fork） |
| 55 | submission: GPU_PRESENT は venus だけ、その errno | venus.c `venus_present_image` | OK（fork） |
| 56 | fences: `_SYNC` の EAGAIN は job が持たない時 | gpu-fence.c `fence_wait`、gpu.c `gpu_job_admit` | 直した（fork。P1 が読み直し） |
| 57 | fences: 故障で target は pending に残る | gpu.c `gpu_dependency_ioctl`、`gpu_session_fail_locked` | 直した（fork） |
| 58 | fences: FENCE_BIND の release の ESTALE | gpu.c `gpu_fence_binding_end` | 直した（fork） |
| 59 | fences: descriptor の poll と ioctl の ENOTTY | gpu-fence.c `fence_poll`、syscall.c | OK（fork） |
| 60 | jobs: commit の失敗の行 | gpu.c `gpu_job_action_ioctl`、`gpu_job_admit` | 直した（fork） |
| 61 | jobs: supervisor は最初の予約で始まる | gpu.c `gpu_close` も `gpu_monitor_start` | 直した（fork） |
| 62 | jobs: POLICY は open・device の健康を見ない | gpu.c `gpu_policy_ioctl` | OK（fork） |
| 63 | display: `ack_sequence` は受け取った物だけ | gpu.c `gpu_events_ioctl`（observed と current との比較） | 直した（fork。P1 が読み直し） |
| 64 | display: 最初の flip の後 console は戻らない | gpu.c `gpu_console_end`、text-display.c `kern_text_end` | OK（P1 が読み直し） |
| 65 | display: WAIT の i915 は latch を待つ | i915 `drv_i915_present_display_wait` | 足した（fork） |
| 66 | display: bcm2711 の未知の sequence | `bcm2711_display_flip_wait_number` | 直した（fork） |
| 67 | display-control: cursor・commit・overlay の ENODEV | gpu.c `gpu_display_admit`（記録の errno） | 直した（fork） |
| 68 | display-control: COMMIT の signal_fd は ENODEV | gpu.c `gpu_fence_binding_add` | OK（P1 が読み直し） |
| 69 | capabilities: cursor・overlay の op が無い時の EOPNOTSUPP は driver | gpu.c `gpu_display_ioctl` | 直した（fork） |
| 70 | capabilities: bcm2711 の 176 byte の条件 | bcm2711 `runtime_capset` | 直した（fork） |
| 71 | capabilities: venus の strict queue は 168 byte だけ | venus/transport.c `venus_strict_queue_find` | OK（BUG-365 に既出） |
| 72 | drivers: i915 の output flags | `i915_display_query`・`i915_display_other_query` | 直した（fork） |
| 73 | drivers: lease の後に text console が描く | `kern_text_end`（for good）、`gpu_console_end` | 直した（P1） |
| 74 | command-stream: protocol 2、OWN_FIRST 0x10000、Venus 145 command、zedBSD 14 | gpu-op.h | OK（fork） |
| 75 | sysctl: 各 variable の権限・errno・上限 | sysctl.c の handler | OK（fork） |
| 76 | examples: display の wait の 1 秒 | i915 `drv_i915_present_display_wait` | 直した（fork） |

### 3 つの読み直し（`Errors:` の条件を handler の分岐で）

1. `GPU_DISPLAY_COMMIT` の ENODEV（gpu-display-control.md）: `gpu_commit_ioctl` は plane の wait の後に `gpu_fence_binding_add` を呼び、それは `device->online == 0 || device->error != 0` で ENODEV を返す。signal_fd の無い commit は `gpu_commit_record` の `gpu_display_admit` で記録の errno。文書の 2 つの条件と合う。
2. `GPU_DISPLAY_EVENTS` の EINVAL（gpu-display.md）: `gpu_events_ioctl` は flags 0 で ack_sequence 非 0、ACK で ack_sequence 0、`ack_sequence > topology_observed` か `> sequence` で EINVAL。新しい文（受け取った最大を越えない・今の sequence を越えない）と合う。
3. `GPU_RESOURCE_DESTROY` の EBUSY と EINTR（gpu-memory.md）: `gpu_destroy_ioctl` は mapping_count が 0 でないと EBUSY、pins の待ちは `WAITQ_INTERRUPTIBLE` で、割り込みでは list から外さずに返る。合う。

### 自分で決めた点

- 12 file は fork（同じ model、同じ規則）に読ませ、P1 は diff を読んで上の 48・56・63・64・68 を読み直してから commit した。fork が出した gpu.md の案（lease の errno、close の fence）は source を読み直して取り入れた。
- `MAP_FIXED` を hint とする動きは文書に「今の動き」として書いた（POSIX との違いは Bug の候補へ）。
- lease の後の画面の行（gpu-drivers.md）は行を消さず、段落で「driver の依頼で、最初の flip の後は効かない」と限定した。

### 新しい食い違いと Bug の候補（phase023 の一覧・BUG-364・BUG-365 に無い物。Q1 が登録を決める）

1. `src/kern/syscall.c` の mmap の device の分岐と `src/kern/vm/device.c` `vmspace_map_device`: character device の mapping では `MAP_FIXED` が hint（`MAP_FIXED_NOREPLACE` だけが正確に置く）。POSIX は `MAP_FIXED` に正確な番地か失敗を求める。
2. `GPU_BLOB_SHAREABLE` の header の約束を多くの driver が守らない（amdrdna・nvrtx・ghci・i915・powervr・mali は無くても export する。core の `gpu_export_ioctl` も見ない）。拒む driver の errno も EOPNOTSUPP（venus）・ENOTSUP（bcm2711 render）・EINVAL（bcm2711 display）とばらばら。
3. amdrdna・nvrtx・i915 の resource_map は `GPU_BLOB_MAPPABLE` を見ない。
4. bcm2711 `output_query` は `GPU_DISPLAY_BLOB` を立てないが `output_present` は `GPU_DISPLAY_PRESENT_BLOB` を受け、commit は blob を要る。`GPU_DISPLAY_ACTIVE` を常に立てる。
5. `gpu.c` `gpu_fence_binding_add`: 記録の errno のある device に ENODEV を返す（他の道は記録の errno）。`session->error`（open だけの故障）は見ない。
6. bcm2711 `output_mode` は表示できない mode に ENOTSUP（他は EINVAL）。bcm2711 `output_wait` は与えていない sequence を拒まず timeout まで待つ（他は EINVAL）。
7. `bcm2711_display_flip_commit` が EFAULT（kernel の mapping の無い cached image）を返し、user の copy の失敗に見える。
8. `gpu.c` `gpu_dependency_ioctl`（PRESENT_SYNC）: target を結んだ後に open が故障すると、target を終える所が EPERM を返し、frame は出たのに要求は EPERM。成功でないので `gpu_console_end` も呼ばれない。
9. `gpu.c` `gpu_fence_state_ioctl`（FENCE_SIGNAL）: 返事を signal の後の 2 回目の query で作るので、間の他 thread の reset で効いた signal が ESTALE を返す。
10. `gpu.c` `gpu_job_action_ioctl`: record が既に終わっただけで `gpu_job_admit` が失敗した時（deadline の切れなど）も driver の fault cancel と `gpu_session_fail_locked(EIO)` を呼ぶ。
11. `src/drivers/gpu/bcm2711/render-device.c` の `bind_render` の前の comment「Vulkan capabilities remain a later binding」は古い（登録の前に `bcm2711_render_runtime_bind` を呼ぶ）。
12. `gpu.c` `gpu_submit_ioctl` は `GPU_COMMAND_SUBMIT` の長さが 4 の倍数かを見ない（`gpu_command_ioctl` は見る）。header は無言、driver ごとに違う。
13. `gpu.c` `gpu_export_install`: fd の番号を copyout してから descriptor を commit する。commit が失敗すると呼び手の構造体に使えない番号が残る（gpu.md の「copy-out は成功の後」の例外）。
14. `src/kern/devfs.c` の node の mode の分岐の comment に開発の管理情報（ws161・ws143・D16・ws143-p005・WS286）がある（AGENTS.md §10）。
15. 細部: amdrdna・nvrtx・powervr・mali の executor は reply flag 付きの transport command と flag 無しの `GPU_OP_ENUMERATE_INSTANCE_VERSION` に EINVAL（gpu-command-stream.md は矛盾しないが触れない）。

### 未確認

- PowerVR・BCM2711 の allocator が常に物理的に連続な 1 つの区間を返すか（`GPU_PLACEMENT_CONTIGUOUS` を明示に見ない）。
- foreign の scanout import の resource に READ・WRITE・MAP を出した時の各 driver の動き。各 driver の `blob_create` の個々の errno（Venus・BCM2711 だけ読んだ）。
- i915 の xd9 と head の wait の道、`drv_i915_display_power_set`、ghci が lease の無い idle の frame に ACTIVE を立てるか、amdrdna・i915・nvrtx の events の sequence が 1 から始まるか、powervr・mali の cursor と present の errno の残り、cursor の「同じ行なら copy を省く」。
- Venus 以外の driver の submit が 4 の倍数でない長さを拒むか、job の callback と capacity の errno（Venus だけ読んだ）。libvulkan の VK_ERROR_DEVICE_LOST の段落の細部。
- gpu-drivers.md の driver ごとの present・wait の時間と lease の後の動き（flags 以外）は p019・p023 の結果に頼った。

### 未実施

- QEMU・実機の試験（文書の Phase。範囲の外）。
