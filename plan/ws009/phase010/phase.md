<!-- awesome-plan project=zedbsd record=ws009-p010 -->

# ws009-p010: DOC-54 gpu.md: node・権限・呼び方・並行と待ち・互換

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: なし（骨格 4dfcce008 と照合の script が main にあること）。p010〜p021 は互いに独立で、並べて起動してよい
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- src/drivers/gpu/gpu.c: `gpu_publish_node`・`drv_gpu_register`（番号の割り当て）・`gpu_open`・`gpu_close`・`gpu_recover_open`・`gpu_ioctl`・`gpu_ioctl_body`・`gpu_session_enter`・`gpu_session_leave`・`gpu_poll`・`gpu_display_ungated`、各 handler の先頭（readable・writable・capability の照合。`grep -n 'readable\|writable' src/drivers/gpu/gpu.c`）
- src/drivers/gpu/gpu-fence.c の poll（並行と待ちの節の (5) のため）
- 待ちの上限: 各 driver の display の wait の operation（`grep -rn '\.wait\b\|adoption_wait\|output_wait' src/drivers/gpu`）。上限が driver ごとの物は driver ごとと書く
- src/kern/text-display.c `kern_text_end`（console の終わりが boot の残りの間か）、include/uapi/fcntl.h の O_ACCMODE
- userland/desktop/sessiond/seat.c（node の mode、seat の user に渡す node）
- include/uapi/ioctl.h・include/uapi/hosted.h・include/uapi/errno.h

## 2. 作る物

- `docs/reference/gpu.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 表の Meaning の列（あれば）を全行。
- 節ごとの主張の表と、3 つの読み直し（gpu-doc-rules.md 4 節の要求の節の無い file の形）。
- 出典にする試験の見当: gpu-admission・gpu-recovery（open・権限・回復）。

## 4. 注意

- 索引と共通の errno の表は gpu-requests.md（p022）に移した。この Phase は書かない。
- 並行と待ちの表は、待つ要求ごとに header の comment と code の両方を読む（display の待ちの 1 秒の上限、`GPU_JOB_CAPACITY` の WAIT は flags 0）。
- 読む量が多い。関数名で辿り、gpu.c の全文（約 8,300 行）を読まない。handler を一つずつ調べる仕事（失敗の時の出力の欄など）は共通の道（`gpu_ioctl`・`gpu_ioctl_body`）の規則だけを書く。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu.md plan/ws009/phase010/phase.md plan/ws009/ws.md`。

## 結果

日付: 2026-10-30（host の `date`）。書いた物: `docs/reference/gpu.md`（436 行）。状態: 書いた・照合 PASS（cleared は Q1 が決める）。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c` -> 出力なし（基点の後の UAPI・gpu.c・gpu-fence.c・libvulkan の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu.md` -> `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 18 capset rows, 1270 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008` -> `check-gpu-uapi-doc: PASS (14 files, ..., 1270 placeholders, final 0 files)`（他の file の PENDING 1270 は数えるだけ）
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture` -> `Markdown relative-link check: PASS (320 links)`
- `git diff --check` -> 出力なし（exit 0）

### 自分で決めた点

- gpu_open の規則は code のとおり（superuser か、実効 uid が node の所有者）。VFS の mode 検査が先に走ることは gpu_open の comment と devfs の既定（owner root、mode 0666）から書いた（VFS 側の open の検査そのものは読んでいない）。
- 対応する architecture は platform の build 規則から amd64（amdrdna・ghci・i915・nvrtx・venus）、aarch64（platform/arm64 の bcm2711・mali）、riscv64（powervr・ghci）。driver の source directory は gpu.md に書かず、driver 名だけを書いた。
- 「並行と待ち」の表: display の wait は driver ごとに分け（ghci・bcm2711 は最大 1 秒で kernel でなく driver が切る、他は待たずに答える、i915 は timeout_ns を使わず直近の present の flip を待ちうる）、refresh は kernel が 1 秒に切り、読んだ 4 つの driver（amdrdna・i915・nvrtx・venus）は signal を見ない sleep、と書いた。ungated の present・commit の adoption の待ちは bcm2711 が 100 ms、ghci が約 12 秒（GHCI_HANG_MS 10 秒 + 2 秒）。
- SA_RESTART: syscall の層が ioctl を再開する物に数えるので、再開では原引数で handler が最初から走り、timeout_ns は全量で数え直す（GPU の handler は毎回 sched_ticks から deadline を作る）と書いた。
- 「別 thread の close は待ちを終わらせない」は sys_ioctl_call が ioctl の間 file の参照を持つこと（と gpu_close の「最後の参照」）からの推論。実機・QEMU では未確認。

### header・comment と code の食い違い

1. `seat.c` の `sessiond_seat_restore` の comment「Every display, which anyone may open again」は、gpu_open が root と所有者しか通さないこと（root 所有の mode 0666 でも非 root は EACCES）と違う。code に従って書いた。
2. `include/uapi/gpu-display.h` の struct gpu_display_wait の comment は「EINTR on a signal」と言うが、ghci の display_wait・display_adoption_wait は waitq_sleep を flags 0 で呼ぶので signal で終わらない（bcm2711 の output_wait は WAITQ_INTERRUPTIBLE で EINTR）。表に driver ごとに書いた。
3. GPU_DISPLAY_OVERLAY_INFO: `gpu_display_ioctl` に case はあるが、`gpu_ioctl_body` の switch には case が無く、`gpu_display_ungated` が通す時（driver が DRV_GPU_DISPLAY_UNGATED を宣言した時）だけ届く。宣言しない driver では default の EOPNOTSUPP になる。overlay_info を持つ driver は commit も持ち、それは ungated を要る（登録の検査）ので実害は無いが、gpu-display-control.md の書き手には「GPU_CAP_DISPLAY で届く」と書かない注意が要る。
4. `include/uapi/gpu-fence.h`・`gpu-job.h` の comment との食い違いは、読んだ範囲で無かった（COMMAND_WAIT・FENCE_WAIT・JOB_CAPACITY の零・有限・UINT64_MAX は code と合う）。

### 自己の照合: 節ごとの主張の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| Nodes: 番号は最小の空き、probe 順 | gpu.c `drv_gpu_register` | 無し |
| Nodes: 番号は unregister が成功する（open が無い）まで再利用されない | gpu.c `drv_gpu_unregister`（EBUSY の間は list に残る） | 無し |
| Nodes: device_id は番号と別で再利用されない | gpu.c `gpu_handle_allocate`・`gpu_device_query_ioctl` | 無し |
| Nodes: open は superuser か node の所有者、他は EACCES | gpu.c `gpu_open` | 無し（gpu-admission は root で O_RDWR） |
| Nodes: node は owner root・mode 0666、seat が gpu0〜gpu3 を seat user・0600 に、止まると root・0666 | devfs.c（mode の既定と属性の記録）、seat.c `sessiond_seat_give`・`sessiond_seat_restore` | 無し |
| Nodes: access mode と readable・writable の対応 | gpu.c `gpu_open`、fcntl.h | 無し |
| Nodes: mmap は readable 必須、PROT_WRITE は writable 必須 | gpu.c `gpu_mmap` | 無し |
| Nodes: 回復は唯一の open かつ古い open・共有が無く reset がある時だけ。それ以外は device の記録した error | gpu.c `gpu_recover_open` | gpu-recovery main.c（古い open が残る間の open は元の error、閉じた後の open で回復） |
| Nodes: 回復中の open は割り込み可能に待つ | gpu.c `gpu_recover_open`（waitq_sleep、WAITQ_INTERRUPTIBLE） | 無し |
| Calling: version・size の不一致は EINVAL（driver に届かない） | gpu.c 各 handler の先頭（32 箇所）、`gpu_info_ioctl` | gpu-admission（正常側のみ） |
| Calling: SYNC の入れ子の header、blob_create_placed の size 64、descriptor の検査 | gpu.c `gpu_dependency_ioctl`・`gpu_blob_ioctl`・`gpu_image_validate`・`gpu_allocation_validate`・`gpu_import_ioctl` | 無し |
| Calling: 失敗の時は出力を書かない（例外 GPU_JOB_RESERVE） | gpu.c の各 copyout の位置、`gpu_job_reserve_ioctl` | 無し |
| Calling: 未知の request は EOPNOTSUPP | gpu.c `gpu_ioctl_body`（default）、cdev.c `cdev_ioctl_file` | 無し |
| Calling: 古い・他 open の資源 handle は EINVAL、fence は EXDEV・ESTALE、sequence は ENOENT | gpu.c `gpu_resource_lookup`、gpu-fence.c `drv_gpu_fence_device`・`fence_wait`、gpu.c `gpu_completion_observe` | 無し |
| Calling: GPU_PIXEL_* は値、GPU_DISPLAY_FORMAT_* は mask | gpu.h、gpu-display.h、gpu-scanout.h | 無し |
| Concurrency: admission を取る要求と取らない要求の一覧 | gpu.c `gpu_ioctl_body`・`gpu_dependency_ioctl`・`gpu_commit_ioctl`・`gpu_mmap` | gpu-admission（同じ open の thread が並ぶ） |
| Concurrency: ungated を宣言する driver は ghci と bcm2711 | grep DRV_GPU_DISPLAY_UNGATED（ghci/display.c の 2 表、bcm2711/display/display-device.c） | 無し |
| Concurrency: admission の待ちは割り込み可能で、device・open の error で終わる | gpu.c `gpu_session_enter` | 無し |
| Concurrency: 待つ要求の表（各行） | gpu.c `gpu_completion_observe`・`gpu_capacity_ioctl`・`gpu_refresh_ioctl`・`gpu_destroy_ioctl`・`gpu_dependency_wait`、gpu-fence.c `fence_wait`、各 driver の wait・adoption_wait・refresh の operation | gpu-recovery（COMMAND_WAIT の timeout）。他は無し |
| Concurrency: SA_RESTART で再開、中断は状態を変えない | syscall.c `syscall_restartable`・restart の層、gpu.c の各待ち | 無し |
| Concurrency: close の順序 | gpu.c `gpu_close`・`gpu_session_fail_locked`・`gpu_fence_retire` | gpu-recovery（close の後の回復） |
| Concurrency: poll の event | gpu.c `gpu_poll`、gpu-fence.c `fence_poll` | gpu-recovery（peer の POLLERR） |
| Compatibility: Venus の wire format 1 の command 番号を使う、GPU_OP_PROTOCOL_VERSION は 2 | gpu-op.h の comment | 無し |
| Compatibility: VK_EXT_display_control・vkGetDisplayPlaneCapabilitiesKHR との対応 | gpu-display.h の comment | 無し |

### 読み直し 3 件

1. gpu_open の規則: `cred_is_superuser(credential)` か `credential->euid == file->f_inode->i_uid` でなければ EACCES、credential が無くても EACCES。文書と合う。
2. GPU_DISPLAY_REFRESH の零の行: gpu_refresh_ioctl は 1 秒より大きい timeout を 1 秒にする。amdrdna_display_refresh_wait は cursor が 0 でなく境界がまだ来ていなければ、deadline が今なので直ちに ETIMEDOUT。表の「One check: ETIMEDOUT（cursor 0 は直ちに答える）」と合う。
3. gpu_poll の POLLPRI: events に POLLPRI があり、session が readable で、device が GPU_CAP_DISPLAY_EVENTS を持ち、driver の sequence が session->topology_acknowledged（open で 0）より大きい時。snapshot の失敗は POLLERR。表と合う。

### 未確認・未実施

- QEMU・実機での実行は未実施（文書だけの Phase）。上の「試験」の欄は読んだだけで走らせていない。
- 未確認: devfs の node の既定の owner が root であること（gpu_open の comment と devfs の mode から。inode_alloc の既定は読んでいない）、VFS が open で mode を先に見ること、別 thread の close が待ちを終わらせないこと（上記）、i915 の display wait の flip の待ちの上限、gpu-recovery・gpu-admission 以外の試験が GPU_DISPLAY_* の待ちを通ること。
- gpu_close が display の lease を終わらせる点は、powervr の display_close（lease を手放す）と gpu_close の display_claims の処理から書いた。全 driver の close は読んでいない。
