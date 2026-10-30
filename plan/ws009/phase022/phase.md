<!-- awesome-plan project=zedbsd record=ws009-p022 -->

# ws009-p022: DOC-54 gpu-requests.md: 要求の索引の 2 列と共通の errno の表

Status: in-progress（書いた・照合 PASS。cleared は Q1 が決める。2026-10-30 の起動）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: p010〜p021 の全部が main に merge 済み
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- p010〜p021 の各 file の要求の節の `Requires:`・`Errors:` の行
- src/drivers/gpu/gpu.c: `gpu_ioctl`・`gpu_ioctl_body`・各 handler の入口（readable・writable・capability の照合。`grep -n 'readable\|writable\|GPU_CAP_' src/drivers/gpu/gpu.c`）

## 2. 作る物

- `docs/reference/gpu-requests.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 表の Meaning の列（あれば）を全行。
- 節ごとの主張の表と、3 つの読み直し（gpu-doc-rules.md 4 節の要求の節の無い file の形）。
- 出典にする試験の見当: この file は要求の節を持たない。

## 4. 注意

- 索引の Open mode と Capability は、各節の `Requires:` から集め、handler の入口で一つずつ確かめる。code と違えば code に揃え、その節の `Requires:` も直す（この Phase は他の gpu*.md の `Requires:` の行だけを直してよい）。
- 条件つきの capability（flag で変わる・op の有無）は Y に入れない規則（gpu-doc-rules.md 2 節の 6）。review が挙げた例: `GPU_RESOURCE_DESTROY`・`GPU_RESOURCE_IMPORT`・`GPU_DISPLAY_CONSTRAINTS`・`GPU_BLOB_CREATE`・`GPU_BLOB_CREATE_PLACED`・`GPU_DISPLAY_COMMIT`・`GPU_COMMAND_SUBMIT_SYNC`・`GPU_DISPLAY_POWER`。
- errno の表は各節の `Errors:` から集め、意味を gpu.c で確かめる。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-requests.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-requests.md plan/ws009/phase022/phase.md plan/ws009/ws.md`。

## 結果

2026-10-30、Sonnet 5.5 high（1 回の起動）。作った物: `docs/reference/gpu-requests.md`（134 行。導入・索引の 2 列の説明 3 段落・errno の表 21 行・driverが返す値の注意）。
基点の後の UAPI の変化: `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c` は 0 件（出力なし）。

### 実行した command と最後の行

- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-requests.md` → `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 0 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008` → `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 0 placeholders, final 0 files)`
- `timeout 120 .../noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture` → `Markdown relative-link check: PASS (663 links)`
- `git diff --check` → 出力なし（終了 0）

### 自分で決めた点

- 索引の Open mode・Capability は 46 要求の `Requires:` の行から script で写し、その後 gpu.c の入口（`gpu_ioctl_body` と各 handler。`gpu_display_ioctl`・`gpu_fence_ioctl`・`gpu_dependency_ioctl`・`gpu_commit_ioctl`・`gpu_export_ioctl`・`gpu_import_ioctl` など）で一つずつ読み直した。食い違いは無く、どの file の `Requires:` も直していない。
- `GPU_RESOURCE_IMPORT` の capability は `none`（`GPU_CAP_SHARE` または scanout の import_image。bit でない条件が片方なので Y に書かない規則）。`GPU_RESOURCE_DESTROY` も `none`（`GPU_CAP_RESOURCE | GPU_CAP_BLOB` または scanout の import_image）。索引の前の説明に理由を書いた。
- どの要求も Open mode が `RW` ではない（`RW` は表に現れない）。
- 二つの検査（open mode と capability）の順は要求で違う（display の要求・`GPU_DISPLAY_EVENTS`・`GPU_DISPLAY_REFRESH`・`GPU_FENCE_*` は capability が先、他は open mode が先）。code で確かめて索引の前に書いた。
- errno の表は各節の `Errors:` から集めた 21 値。`ENXIO` と `ECANCELED` は kernel 自身の検査では返さない（driver と記録された完了の error）ので表に入れず、表の後に注意として書いた。`ENOTSUP` は `EOPNOTSUPP` と同じ値なので 1 行にした。

### 統合の Phase（p023）への引き継ぎ

- `Requires:` と gpu.c の食い違い: 無し（46 件とも一致）。
- gpu.md の「Requests that observe state need a readable open ... a few need both or neither」の「both」: 全要求を見て `RW` の要求は無い（`neither` だけ）。gpu.md の文は「both」を含み、索引と合わない。p023 で gpu.md を直すか判断を。
- gpu.md の「A descriptor that does not name a GPU fence fails with EBADF or EINVAL」: gpu.c の `gpu_fence_get` は `EBADF` を返す（`handle_fd_get` が NULL）。`EINVAL` になる条件は未確認。

### 節ごとの主張の表（文書には置かない）

| 主張（節と要約） | 根拠 | 試験 |
| --- | --- | --- |
| 索引: 各要求の Open mode と Capability | `gpu_ioctl_body`・各 handler の入口（`session->readable`・`writable`・`GPU_CAP_*` の検査） | 無し（照合の script が `Requires:` と索引を照らす） |
| 索引: 検査の順（display・fence は capability が先、他は open mode が先） | `gpu_display_ioctl`・`gpu_commit_ioctl`・`gpu_events_ioctl`・`gpu_refresh_ioctl`・`gpu_fence_ioctl`（capability が先）、`gpu_create_ioctl`・`gpu_blob_ioctl`・`gpu_submit_ioctl`・`gpu_job_reserve_ioctl`・`gpu_dependency_ioctl` ほか（writable が先） | 無し |
| errno `EINVAL`: 資源の handle が不明・破棄済み・他の open の物は `EINVAL` | `gpu_resource_lookup` | 無し |
| errno `EOPNOTSUPP`: 不明な要求番号 | `gpu_ioctl_body` の `default:` | 無し |
| errno `EACCES`: open mode の不足 | 各 handler の入口 | 無し |
| errno `ENOENT`: 完了 record の sequence が無い | `gpu_wait_ioctl` ほか（`return ENOENT`） | 無し |
| errno `EBADF`/`EPERM`: fence の descriptor・所有 | `gpu_fence_get`、`gpu-fence.c` と `gpu_fence_state_ioctl` の `EPERM` | 無し |
| errno `ENODEV`/`EIO`: 撤去と記録された失敗 | `gpu_session_enter`、`gpu_session_error_locked`、`gpu_session_fail_locked` | 無し |
| errno `EBUSY`: 再入 | `gpu_session_enter` | 無し |
| errno `ESRCH`/`E2BIG`/`ENOSPC` | `gpu_export_install`、`gpu_submit_ioctl`、`gpu_create_ioctl` | 無し |
| errno `ECANCELED`/`ENXIO` は kernel の検査では返さない | `grep ECANCELED`（`gpu_completion_discard` の completion の status と driver の完了）、`ENXIO` は gpu.c に無い | 無し |

### 3 つの読み直し（gpu.c の code を読み直した）

1. `GPU_RESOURCE_DESTROY` の capability が `none`: `gpu_destroy_ioctl` は writable を検査し、`(GPU_CAP_RESOURCE | GPU_CAP_BLOB) == 0` の時だけ scanout の import_image の有無で `EOPNOTSUPP` を返す。bit と bit でない条件の「どれか」なので Y は `none`。合う。
2. `resource handle` は `EINVAL`: `gpu_resource_lookup` は handle が 0 でも見つからなくても `EINVAL` を返す。`ENOENT` は完了 record（`gpu_wait_ioctl`、job の commit・cancel）にだけ。合う。
3. 検査の順: `gpu_display_ioctl` は `GPU_CAP_DISPLAY` の後に `readable`/`writable`、`gpu_fence_ioctl` は `GPU_CAP_FENCE` の後に access、`gpu_job_reserve_ioctl` は `writable` の後に `GPU_CAP_JOB`。合う。

### 未確認・未実施

- 表の `ESRCH`・`EMFILE` の `GPU_FENCE_CREATE` の分は、各節の `Errors:` から写した（`gpu_fence_create_ioctl` の descriptor の install の経路は読み切っていない）。`EMFILE` は gpu.c に文字が無く、descriptor table の側の値。
- QEMU・実機の試験は無し（文書のみ）。
