<!-- awesome-plan project=zedbsd record=ws009-p023 -->

# ws009-p023: DOC-54 統合: 全文書の最終の照合、二重の主張の照合、既存の文書の直し

Status: 書いた・照合 PASS（2026-10-30 10:40 実装の担当。cleared は Q1 が決める。元: planned、2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: p022 が main に merge 済み
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- p010〜p022 の phase.md の結果の節（食い違い・未確認・自分で決めた点）
- src/drivers/gpu/gpu.c（食い違いの解消に要る handler だけ）

## 2. 作る物

- `docs/reference/gpu*.md（食い違いの直しだけ）`
- `docs/reference/control-devices.md`
- `docs/architecture/kernel-and-hal.md`
- `docs/reference/visionfive2-gpu.md`
- `docs/reference/kernel-boot-parameters.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。
- 見つけた header・code の食い違い（Bug の候補）と、tree の他の comment と code の食い違い（例: userland/desktop/sessiond/seat.c の node の open の comment）の一覧を Q1 に送る。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 全 file で `--final` と `--examples` が PASS。4 節の照合の結果を表で結果の節に。

## 4. 注意

- `--final`（全 file）で残る FAIL を直す。
- 並行の Phase が別々に書いた同じ主張を照らし、code に揃える: bit が node の寿命の中で変わるか（gpu-capabilities.md・gpu-drivers.md）、venus の JOB（gpu-capabilities.md・gpu-jobs.md・gpu-drivers.md）、最初の flip（gpu-submission.md・gpu-display.md）、lease の後の画面（gpu-display.md・gpu-display-control.md・gpu-drivers.md）、待ちの errno と上限（gpu.md の表・gpu-submission.md〜gpu-display-control.md・gpu-examples.md）。
- visionfive2-gpu.md の「`/dev/gpu0` は display、`/dev/gpu1` は render」は、番号が probe の順の空き番号である事（gpu.md）と合わせる（例として書くなら、そう明記する）。
- kernel-boot-parameters.md を編集するので、その file の開発の管理情報（WS・Phase・Queue の番号、日付の経緯）も外す（意味を変えずに言い換える）。
- control-devices.md の「There is no published current GPU object ABI to enumerate here.」を gpu.md への link と 1〜2 文に。kernel-and-hal.md の GPU の段（text console が firmware の framebuffer に描き続けるという文）を今の振る舞い（最初の flip の後、text console は display を使わない。gpu-display.md の User-visible side effects）に。visionfive2-gpu.md と kernel-boot-parameters.md から gpu.md へ link。
- 各 Phase の報告の header・code の食い違い（Bug の候補）を一覧にして Q1 に送る。文書は今の振る舞いのまま。
- 見出しの変更は禁止のまま（`--base`）。
- `noct plan/ws009/tests/check-completed-producer-docs.noct .` も流し、gpu*.md が status の違反を足さないことを確かめる（手前の照合で止まるなら、その最後の行を記録）。

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

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu*.md docs/reference/control-devices.md docs/architecture/kernel-and-hal.md docs/reference/visionfive2-gpu.md docs/reference/kernel-boot-parameters.md plan/ws009/phase023/phase.md plan/ws009/ws.md`。
加えて: `--final` を file の指定なしで（全 file）PASS、`--examples` で amd64 の compile が 1 回以上。

## 結果

日付: 2026-10-30（host の `date`）。worktree `/home/awe/zedBSD-worktrees/s-ws009-p023`、起点 28047ec5a。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の UAPI・gpu.c の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final --examples`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 0 placeholders, final 14 files, 12 example compiles)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final`（file 指定なし）: 同じ PASS（final 14 files）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: 手直しの後に PASS（見出しの変更なし）。
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (673 links)`
- `git diff --check`: 出力なし。
- `noct plan/ws009/tests/check-completed-producer-docs.noct .`: 手前の照合で止まる。最後の行 `:7: Error: ./userland/base/net/main.c: missing required text: net wifi set-key SSID PASSPHRASE [auto]`（gpu*.md と無関係の file。gpu*.md の status の検査までは進まない）。

### 直した物（file ごと）

- gpu.md: `device_id` は node ごと（同じ GPU の 2 つの node でも別の値）で、物理 device の同一判定には使えないと書いた（`companion_id` は hint、roles で見分ける）。「a few need both or neither」を「a few need neither。No request needs both」に（全 46 要求の Requires に RW は無い）。fence の descriptor の errno を `gpu_fence_get`（EBADF）と `fence_resolve`（driver 種別の別物は EINVAL）に分けて正確に。
- gpu-fences.md: 他 device の fence を EXDEV とする規則の例外を、`GPU_DISPLAY_PRESENT_SYNC` の `wait_fd` に加え `GPU_DISPLAY_COMMIT` の plane の `wait_fd`（`gpu_dependency_wait(..., 1U)`）に広げ、signal_fd は比べると明記。SA_RESTART の時の再実行を gpu.md に合わせた。
- gpu-capabilities.md: `GPU_CAP_JOB_CAPACITY` を「あと幾つ」でなく「`available` は 0 か 1」に。
- gpu-submission.md: ECANCELED の文を、flags 0 の cancel・driver が断った reservation は record を捨てるので後の wait は ENOENT、ECANCELED は既に待っていた wait だけ、に。EINTR の再実行の文を gpu.md に合わせた。
- gpu-display.md: COMMIT/PRESENT の sequence は多くの driver が lease ごとに数え直し、bcm2711 は戻さない。`GPU_DISPLAY_WAIT` の 1 秒の上限と timeout は ghci・bcm2711 だけ、EINTR は bcm2711 だけ（ghci は signal で終わらない）。
- gpu-requests.md: 共通の errno の表の EBADF（driver 種別の別物は EINVAL）と EXDEV（PRESENT_SYNC と COMMIT の plane の wait は比べない）。索引の `GPU_RESOURCE_IMPORT` は既に none（直し不要）。
- gpu-memory.md: `GPU_BLOB_SHAREABLE`・`GPU_BLOB_CROSS_DEVICE` の文を gpu-sharing.md と合わせた（核は capability と組み合わせだけを見る、flag の効果は driver）。`GPU_DEVICE_QUERY` の導入文の「device_id と companion_id で pair を決める」を、device_id は node ごと・companion_id は今どの driver も 0・roles と constraints で決める、に。
- gpu-examples.md: SA_RESTART の無い signal の時だけ EINTR、display の wait は adoption 待ちの driver の話と明記。
- control-devices.md: 「There is no published current GPU object ABI…」を gpu.md への link 付きの 2 文に。
- kernel-and-hal.md: GPU の段の「text console は firmware の framebuffer に描き続ける」を、最初の flip（4 つの要求）で text console が display の使用をやめ、lease の後も戻らない、に。表に gpu.md・gpu-drivers.md への link。
- visionfive2-gpu.md: 「`/dev/gpu0` は display、`/dev/gpu1` は render」を、display node を先に登録するので通常はそうなるが番号は probe の順の空き番号であり、roles で見分ける、に。gpu.md・gpu-drivers.md へ link。
- kernel-boot-parameters.md: 開発の管理情報（ws035-p097・p098、ws289-p005、U5、WS238、WS013、q015 の語、日付の経緯）を意味を変えずに外した。`login=` の節に gpu.md への link。

### 4 節の照合の表（二重の主張）

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| bit は node の寿命の中で変わらない（gpu-capabilities.md 2、gpu-drivers.md 各行の「登録の前に足す」） | `drv_gpu_register` の検査、各 driver の登録順: amdrdna `node.c`（`amdrdna_runtime_prepare` → `drv_gpu_register`(156) → `amdrdna_runtime_start`、`|=` は `runtime_bind`(204) で登録の前）、ghci `ghci.c`(652/671)、i915 `display.c`(958)・`bootfb.c`(286)・`capture.c`(219)、nvrtx `node.c`(157)・`runtime.c`(235) | 無し |
| venus は JOB・JOB_CAPACITY を GET_INFO で常に立て、strict queue の無い host では RESERVE・CAPACITY が ENOTSUP（gpu-capabilities.md・gpu-jobs.md・gpu-drivers.md） | `venus.c` `venus_get_info`(614-616) は bit を落とすが `gpu.c` `gpu_info_ioctl` が `ops->capabilities` で上書き。`venus/transport.c` `venus_strict_queue_find` | 無し |
| 最初の flip は 4 つの要求（PRESENT・DISPLAY_PRESENT・PRESENT_SYNC・COMMIT）の最初の成功で system に 1 回、戻らない（gpu-submission.md・gpu-display.md・gpu-display-control.md・gpu-drivers.md・kernel-and-hal.md） | `gpu.c` `gpu_console_end`（`gpu_console_ended` の compare-exchange、`kern_text_end(NULL, 0)`）、`text-display.c` `kern_text_end` | 無し |
| lease の後の画面は driver 次第（gpu-display.md・gpu-display-control.md・gpu-drivers.md） | `gpu-drivers.md` の各行は driver の release の code。核は戻さない | 無し |
| 待ちの errno と上限（gpu.md の表 と gpu-submission/fences/jobs/display/display-control/examples） | `gpu.c` `gpu_refresh_ioctl`(2541: 1 秒に切る)、`ghci/display.c` `WAIT_MAX_NS`(57,490)、`bcm2711/display/display-device.c`(988-994)、`gpu_completion_deadline`、`gpu_dependency_wait` | 無し（`userland/tests/gpu-admission`・`gpu-recovery` は並行と回復だけ） |
| GPU_JOB_CAPACITY の `available` は 0 か 1 | `gpu.c` `gpu_capacity_ioctl` | 無し |
| 取り下げた予約は ENOENT（ECANCELED は既に待っていた wait だけ） | `gpu.c` `gpu_job_action_ioctl`(5777-5783: `gpu_job_unbind` + `gpu_completion_discard`)、`gpu_completion_discard`(6353) | 無し |
| COMMIT・PRESENT の sequence は lease ごとに数え直す（driver による） | nvrtx `present.c`(951)・amdrdna `present.c`(550)・ghci `display.c`(326)・venus `display.c`(986)・powervr `display.c`(916) は claim で 0、bcm2711 `display-flip.c`(270-276) は `state_serial` を戻さない | `userland/tests/hvs-load` は COMMIT の使用だけ |
| 他 device の fence は EXDEV、ただし PRESENT_SYNC・COMMIT の wait は比べない | `gpu.c` `gpu_fence_get`(6625)、`gpu_dependency_wait` の `foreign`、`gpu_commit_ioctl`(4420 で 1U)、`gpu_dependency_ioctl`(7099 で `display`) | 無し |
| fence の descriptor: EBADF は driver 種別でない、EINVAL は driver 種別の別物 | `gpu_fence_get`(`handle_fd_get(fd, KERNEL_HANDLE_DRIVER)`)、`gpu-fence.c` `fence_resolve`(598-607) | 無し |
| `device_id` は node ごと | `gpu.c` `drv_gpu_register`(746 `gpu_handle_allocate(&device->identity)`)、`gpu_device_query_ioctl`(4997) | `userland/tests/display-scanout` |
| 全ての要求で RW を求める物は無い | 46 の `Requires:` 行（集計: R 11・W 29・none 6。RW は 0） | 無し |

3 つの読み直し（`Errors:` の条件が code と合うか）: (1) `GPU_FENCE_QUERY` の EINVAL（driver 種別の別物）は `fence_resolve` の handle->ops 比較で返る。(2) `GPU_DISPLAY_COMMIT` の EXDEV は signal_fd だけ（`gpu_fence_get(..., 0U)` が signal、wait は `1U`）。(3) `GPU_JOB_CANCEL` flags 0 の後の `GPU_COMMAND_WAIT` が ENOENT になるのは `gpu_job_unbind` と `gpu_completion_discard` が sequence を 0 にするから（observers が 0 の時）。

### 自分で決めた点

- gpu.md の「EBADF か EINVAL」は p022 の見込み（EBADF だけ）と違い、EINVAL の出所が `fence_resolve` にあったので消さず、2 つに分けた。
- gpu-display.md の `GPU_DISPLAY_WAIT` は header の「signal で EINTR」でなく code（bcm2711 だけ）に従った。
- kernel-boot-parameters.md の BR-T42〜46・CT-T008〜031 の試験の符号は残し（意味を持つ名前）、WS・q の番号だけ外した。

### 食い違いと Bug の候補（Q1 が Bug に登録する。順位は重大さと確かめの要る度合い）

1. `src/drivers/gpu/gpu.c` `gpu_fence_get`: foreign=1 は fence の device を比べない。`GPU_DISPLAY_PRESENT_SYNC` の wait_fd（`gpu_dependency_ioctl`）と `GPU_DISPLAY_COMMIT` の plane の wait_fd（`gpu_commit_ioctl`）で、どの GPU の fence でも受ける。comment は「approved companion GPU」だが承認の確かめが無い。設計の確かめが要る。
2. `gpu.c` `gpu_fence_binding_end`: kernel が終えた binding が open を閉じるまで slot（`GPU_SUBMIT_MAX` の 1 つ）を持ち続ける（他に解放が無い）。
3. `gpu.c` `gpu_dependency_wait`（`drv_gpu_fence_wait_work`）: `GPU_FENCE_BIND` だけで結んだ pending の fence は `_SYNC` で待たれず EAGAIN（header に無い）。
4. `gpu.c` `gpu_console_end` と `include/uapi/gpu-display.h`: header は lease の release・close で output が console に戻ると書くが、最初の flip で `kern_text_end` が 1 回呼ばれ戻らない。powervr の log「the text console resumes」も同じ（`src/drivers/gpu/powervr/display/display.c`）。
5. `userland/desktop/sessiond/seat.c` `sessiond_seat_restore` の comment: root に返した node を「誰でも開ける」と書くが `gpu_open` は root と owner だけ（mode 0666 でも EACCES）。
6. `gpu.c` `gpu_info_ioctl` と `src/drivers/gpu/venus/venus.c` `venus_get_info`: strict queue が無い時の JOB・JOB_CAPACITY の削除を core が上書きし、GET_INFO は立てたまま RESERVE・CAPACITY は ENOTSUP。
7. `src/drivers/gpu/venus/transport.c` `venus_strict_queue_find`: capset がちょうど 168 byte（`VENUS_VENDOR_CAPSET_BYTES`）の時だけ vendor suffix を読む。libvulkan は 168 と 176 で読む（176 byte の host で食い違う）。
8. `src/drivers/gpu/i915` の capset ハンドラ: `capset_id`・`version` を見ず、どの id にも同じ payload。他 driver は 4/0 以外 ENOTSUP。i915 は契約が未実装（XXX）のまま vendor flags 7 を返す。
9. `gpu.c` `gpu_submit_ioctl`: driver を呼ぶ前に `sequence` を書き出すので、driver が断ると何も指さない sequence が残り wait は ENOENT。`GPU_JOB_RESERVE` も同じ（`gpu_job_reserve_ioctl`）。
10. `include/uapi/gpu-display.h` `struct gpu_display_wait`: 「signal で EINTR」と書くが ghci の display の wait・adoption の wait は signal で終わらない（`ghci/display.c`）。`GPU_DISPLAY_REFRESH` の amdrdna・i915・nvrtx・venus も signal を見ない（bcm2711 だけ中断できる）。
11. `gpu-display.h`: lease が他の open の物の WAIT の errno が driver でばらばら（header は EBUSY、venus は EPERM、i915 は EINVAL）。i915 の mode の ENOENT、bcm2711 の plane_index の EBUSY（他は EINVAL）。
12. `gpu.c` `gpu_refresh_ioctl`/display の wait: `timeout_ns` の 1 秒の上限は core に無く、bcm2711・ghci の driver だけ（他の driver の wait は timeout を見ない）。
13. `gpu.c` `gpu_blob_ioctl`: `GPU_BLOB_CREATE` が `flags` を書き戻す（HOST_BLOB の無い node で `GPU_BLOB_HOST` を落とす）が header は入力だけ。`GPU_RESOURCE_MAP` は resource の種類を見ない。`GPU_RESOURCE_DESTROY` は map 中 EBUSY、display の使用中は待つ、投入済みの仕事は見ない、export 済みの descriptor は取り消さない。
14. `gpu.c` の GPU_COMMAND（EINVAL）と `GPU_COMMAND_SUBMIT`（E2BIG）の大きすぎの errno の差（header は無言）。`GPU_PRESENT` の不明な handle は EINVAL（他の sequence は ENOENT）。device の失敗の poll は POLLERR だけ、POLLHUP は device が外れた時だけ（骨格の spec は POLLERR|POLLHUP）。UINT64_MAX の wait は device が外れると ENODEV。
15. `src/drivers/gpu/amdrdna/present.c` `present_present` の comment: flip の始まりで返ると書くが、code は flip を運ぶ update を待つ。`src/drivers/gpu/i915/display/present.c` の file の comment（output 1 つ、hot-plug・topology の event 無し）が code と食い違う。
16. `config/drivers/pci.drivers` と Makefile: `CONFIG_DRIVER_PCI_AMDRDNA` の既定が n と y で違う。
17. nvrtx・i915 は `GPU_CAP_DISPLAY_CONTROL` を立てるが `GPU_DISPLAY_POWER` は EOPNOTSUPP（flag `GPU_DISPLAY_POWER_CONTROL` の無い output）。
18. `include/uapi/gpu-op.h` の先頭の comment: executor を i915・bcm2711 だけと書き、file の名前が古い（i915 は `vk/vk-transport.c` など）。video の properties の comment は i915 だけと書くが nvrtx と bcm2711（video 付きの build）も `NATIVE_VIDEO_DECODE_H264` を立てる。`include/uapi/sysctl.h` の `hw.gpu.start` は bcm2711 の Pi 5 の start を書いていない。
19. `gpu-display-control` の plane_state: `include/uapi/gpu-display.h` の comment「storage of this open」と code（blob か scanout の allocation だけ、他は EINVAL）。cursor の EOPNOTSUPP は header の `GPU_DISPLAY_HAS_CURSOR` でなく op の有無。REFRESH の ENODEV の記録に誤りを含む。
20. `GPU_DISPLAY_OVERLAY_INFO` は ungated の経路でしか届かない（gpu-display-control.md は書いてある。header は触れない）。
21. `GPU_ALLOCATION_IMPORT`（`include/uapi/gpu-allocation.h`）: 入力を version・size・fd と書くが code は flags 0 も要る。
22. PRESENT がいつ返るかを示す output の flag が無い（driver ごとに採用後・表示後・flip の arm 後）。
23. 未確かめ: job の callback の errno・timeline の範囲は venus だけ読んだ（他の driver は未確かめ）。

（既に Q1 が直した物: `sysctl` の `show_name` が 4 byte の値を初期化しない `uint64_t` に読んでいた件。）

### 未確認・未実施

- 13 の `GPU_RESOURCE_*` の個別の errno と 23 は p012・p016 の結果に頼り、この Phase では読み直していない。
- `noct check-completed-producer-docs.noct` は無関係の file で止まるため、gpu*.md の status の検査は未実施。
- 4 節の表の「試験」は、直に当たる program が無い物を「無し」とした。QEMU・実機の試験は未実施（文書の Phase）。

## p014 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- `GPU_COMMAND_SUBMIT` は driver を呼ぶ前に `sequence` を写し出すので、driver が断ると何も指さない sequence が残り、その wait は ENOENT。
- device の失敗の poll は POLLERR だけ、POLLHUP は device が外れた時だけ（skeleton の spec は POLLERR|POLLHUP だった）。UINT64_MAX の wait は device が外れると ENODEV。
- `GPU_COMMAND` の大きすぎは EINVAL、`GPU_COMMAND_SUBMIT` は E2BIG（header は無言）。`GPU_PRESENT` の不明な handle は ENOENT でなく EINVAL。

## p017 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- gpu-display.h は lease の release・close で output が console に戻ると書くが、最初の flip で `gpu_console_end` が `kern_text_end` を system で 1 回呼び、戻らない（powervr は「the text console resumes」と log する）。
- PRESENT がいつ返るかを示す output の flag が無い（driver ごとに採用後・表示後・flip の arm 後で違う）。
- 他の open の lease の WAIT の errno: header は EBUSY、venus は EPERM、i915 は EINVAL。i915 の mode の ENOENT、bcm2711 の plane_index の EBUSY（他は EINVAL）。
- `timeout_ns` の 1 秒の上限は core に無く bcm2711・ghci だけ、他の driver の wait は timeout を見ない。

## p011 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- i915 の capset は capset_id・version を見ず、どの id にも同じ payload を返す（他の driver は 4/0 以外を ENOTSUP）。
- `venus_get_info` が strict の queue が無い時に JOB・JOB_CAPACITY を落としても、`gpu_info_ioctl` が `ops->capabilities` で上書きするので GET_INFO は立てたまま、`GPU_JOB_RESERVE`・`CAPACITY` は ENOTSUP（venus.c の comment と食い違い）。
- Venus の kernel は host の capset がちょうど 168 byte の時だけ vendor の suffix を読み、libvulkan は 168 と 176 で読む（176 byte の host で strict が食い違う）。
- gpu-op.h の video の properties の comment は i915 だけと書くが、nvrtx と bcm2711（video 付きの build）も NATIVE_VIDEO_DECODE_H264 を立てる。
- i915 は契約が未実装（XXX）のまま vendor flags 7 を返す。

## p019 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- amdrdna の `present_present` の comment は flip の始まりで返ると書くが、code は flip を運ぶ update を待つ。
- `i915/display/present.c` の file の comment（output 1 つ、hot-plug・topology の event 無し）が code（複数の connector、GPU_DISPLAY_LIMITED、display の event）と食い違う。
- `CONFIG_DRIVER_PCI_AMDRDNA` の既定が config/drivers/pci.drivers は n、Makefile の amd64 は y。
- nvrtx・i915 は GPU_CAP_DISPLAY_CONTROL を立てるが GPU_DISPLAY_POWER に EOPNOTSUPP。

## p010 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- sessiond の `seat.c` の `sessiond_seat_restore` の comment は root に返した node を「誰でも開ける」と書くが、`gpu_open` は root と owner だけ（mode 0666 でも EACCES）。
- `gpu-display.h` の `struct gpu_display_wait` は「signal で EINTR」と書くが、ghci の display の wait と adoption の wait は flags 0 の `waitq_sleep` で signal で終わらない（bcm2711 だけ中断できる）。`GPU_DISPLAY_REFRESH` の 4 driver も signal を見ない。
- `GPU_DISPLAY_OVERLAY_INFO` は ungated の経路でしか届かない（gpu-display-control.md の p018 に知らせ済みでなければ統合で合わせる）。

## p012 から（2026-10-30 Q1）

- **gpu.md と食い違う見込み（統合で直す）**: `device_id` は物理の device ごとでなく node ごと（同じ GPU の render と display の node は別の id）。gpu.md と骨格の spec が「device_id で同じ device と分かる」と仮定していないか確かめ、合わせる。
- `GPU_BLOB_CREATE` は `flags` を書き戻す（HOST_BLOB の無い node で GPU_BLOB_HOST を落とす）が header は入力だけ。`GPU_RESOURCE_MAP` は resource の種類を見ない。`GPU_RESOURCE_DESTROY` は map 中 EBUSY、display の使用中は待つ、投入済みの仕事は見ない、export した descriptor は取り消さない。

## p016 から（2026-10-30 Q1、統合で直す）

- gpu-capabilities.md は GPU_CAP_JOB_CAPACITY を「timeline があと幾つ予約を取れるか」と書くが、`gpu_capacity_ioctl` の `available` は 0 か 1。
- gpu-submission.md の ECANCELED（取り下げた予約）は、flags 0 の cancel が record を捨てるので wait は ENOENT。ECANCELED は既に待っていた thread だけ。文を正確にする。
- job の callback は venus だけ読んだ。他の driver の errno・timeline の範囲は未確かめ。

## p015 から（2026-10-30 Q1、Bug の候補の一覧に入れる。1 つ目は優先して確かめる）

- **`gpu_fence_get` の foreign=1 は fence の device を比べない**（`GPU_DISPLAY_PRESENT_SYNC` の wait_fd、`GPU_DISPLAY_COMMIT` の plane の fence）。comment は「approved companion GPU」と書くが承認の確かめが無い。別の GPU の fence を受けて良いかの設計の確かめが要る。
- kernel が終えた binding は open を閉じるまで slot（GPU_SUBMIT_MAX の 1 つ）を持ち続ける（`gpu_fence_binding_end`、他に解放が無い）。
- `GPU_FENCE_BIND` だけで結んだ pending の fence は `_SYNC` で待たれず EAGAIN（header に無い）。

## p013 から（2026-10-30 Q1）

- `GPU_RESOURCE_IMPORT` の Requires は capability none（GPU_CAP_SHARE か display の import の op）。gpu-requests.md の索引に none を写す。
- gpu-memory.md の GPU_BLOB_SHAREABLE・GPU_BLOB_CROSS_DEVICE の文を gpu-sharing.md と合わせる（core は CROSS_DEVICE・SCANOUT_FOREIGN・companion_id を見ない、venus の image の export だけ CROSS_DEVICE を読む）。
- `GPU_ALLOCATION_IMPORT` の header は入力を version・size・fd と書くが、code は flags 0 も要る。

## p018 から（2026-10-30 Q1）

- COMMIT の `sequence` が lease ごとに数え直すか: gpu-display.md は数え直すと書くが bcm2711 の code は戻さない。統合で事実に合わせる。
- plane_state の comment「storage of this open」と code（blob か scanout の allocation だけ、他は EINVAL）の食い違い。cursor の EOPNOTSUPP は header の GPU_DISPLAY_HAS_CURSOR でなく op の有無。REFRESH の ENODEV は記録された誤りも含む。

## p020 から（2026-10-30 Q1）

- gpu-op.h の先頭の comment は executor を i915・bcm2711 だけと書き、file の名前も古い（i915 は vk/vk-transport.c など）。video の節も nvrtx・bcm2711 の decode と capset の bit の門を書いていない。sysctl.h の `hw.gpu.start` は bcm2711 の Pi 5 の start を書いていない。
- （p020 の Bug の候補）`sysctl` の `show_name` が 4 byte の値（`hw.gpu.attaching` など）を初期化しない `uint64_t` に読んでいた件は、Q1 が直した（2026-10-30。返った長さ 1・2・4・8 で読み分け、他は EIO。amd64・arm64 の target clang で -std=c89 -Wall -Wextra -Werror PASS、実行は未確かめ）。文書の「4 byte の buffer で読む」はそのままで良い。

## p022 から（2026-10-30 Q1）

- gpu.md の「a few need both or neither」: both の要求は無い（neither だけ）。直す。
- gpu.md の「GPU の fence を名指さない descriptor は EBADF か EINVAL」: `gpu_fence_get` は EBADF だけ。EINVAL の出所を確かめ、無ければ直す。
