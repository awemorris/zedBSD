<!-- awesome-plan project=zedbsd record=ws009-p013 -->

# ws009-p013: DOC-54 gpu-sharing.md: resource と allocation の export・import、scanout の import

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
- src/drivers/gpu/gpu.c: `gpu_export_ioctl`・`gpu_export_install`・`gpu_import_ioctl`・`gpu_image_validate`・`gpu_scanout_import`・`gpu_shared_operations`・`gpu_allocation_validate`・`gpu_allocation_export_ioctl`・`gpu_allocation_import_ioctl`・`gpu_shared_hold_device`・`gpu_shared_release`・`drv_gpu_shared_backing`
- include/uapi/gpu.h・gpu-allocation.h・gpu-scanout.h の comment

## 2. 作る物

- `docs/reference/gpu-sharing.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: display-scanout（RESOURCE_EXPORT・IMPORT）。ALLOCATION_* は source tree の試験が無い（`no source-tree test`）。

## 4. 注意

- export の fd は出力の copyout の後に入る（copyout が失敗したら fd は入らない）。import に渡した fd は呼び手の物のまま。
- cross-device の条件（`GPU_BLOB_CROSS_DEVICE`・`GPU_SCANOUT_FOREIGN`・companion）は code で確かめる。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-sharing.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-sharing.md plan/ws009/phase013/phase.md plan/ws009/ws.md`。

## 結果

（2026-10-30 実施。Sonnet 5.5 high の 1 回の起動。cleared は Q1 が決める）

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし。着手の時と commit の前に 2 回）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-sharing.md`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 18 capset rows, 833 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (... final 0 files)`
- `timeout 120 noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (359 links)`
- `git diff --check`: 出力なし（終了 0）。

書いた行数: `docs/reference/gpu-sharing.md` は 265 行。`PENDING` と `<!-- spec` は 0。

### 自分で決めた点

- 要求の節の `Requires:`: 4 つとも open mode W（handler が `session->writable` を見る）。`GPU_RESOURCE_EXPORT` は `GPU_CAP_SHARE`、`GPU_ALLOCATION_EXPORT`・`GPU_ALLOCATION_IMPORT` は `GPU_CAP_ALLOCATION_SHARE`。`GPU_RESOURCE_IMPORT` は `GPU_CAP_SHARE` が無くても display の scanout import の operation があれば通る（bit でない条件との「どちらか」）ので、gpu-doc-rules.md 2 節の 6 の規則どおり capability は `none` とし、本文と `Errors:` に条件を書いた。統合の Phase が索引（gpu-requests.md）に写す時はこの `none` に合わせる。
- 構造体の表の Direction: 出力で入力時に 0 を求める欄は `out`（display の節の書き方に合わせた）、予約と `GPU_ALLOCATION_IMPORT` の `flags` は `zero`。入れ子の `struct gpu_image_descriptor`・`struct gpu_allocation_descriptor` は export では入力、import では出力なので、exporter が書く欄は `in/out`、`device_id` は `out`。
- 「Sharing model」は H5 の小見出し（What the descriptor is、Descriptor flags、Publication of the descriptor、Immutable description、Device identity and cross-device import、Scanout import、Capabilities）で構成した。見出し H1〜H4 は変えていない。
- `GPU_BLOB_CROSS_DEVICE`・`GPU_SCANOUT_FOREIGN`・companion は、code で確かめた範囲だけ書いた: core の export・import はどれも読まない（`GPU_BLOB_CROSS_DEVICE` は blob 作成の時の組み合わせの検査だけ、venus の `share_export` が画像 export で見る）。core の跨ぎの条件は `EXDEV`（`GPU_IMPORT_SCANOUT` 無し）と `gpu_scanout_import` の条件。
- 出典の試験: `GPU_RESOURCE_EXPORT`・`GPU_RESOURCE_IMPORT` は display-scanout の main.c と amdrdnactl の gpu-check.c（どちらも ioctl を直に呼ぶ）。`GPU_ALLOCATION_*` は libvulkan だけが呼ぶので `no source-tree test`。
- 「Other values」の driver の例は venus・nvrtx・ghci の share 実装から取った物だけ（driver ごとの全数ではない）。

### code と header の食い違い

なし。header の comment（gpu.h、gpu-allocation.h、gpu-scanout.h、include/drivers/gpu/gpu-share.h）と gpu.c の振る舞いは、読んだ範囲で合っていた。補足として、`GPU_ALLOCATION_IMPORT` の header の「Only version, size and fd are inputs」に対し、code は `flags` も 0 を求める（0 は入力の値としては同じ）。文書は `flags` を `zero` と書いた。

### 自己の照合

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| Sharing model: descriptor は export した open から独立で、import ごとに新しい resource（handle は open に局所） | gpu.c `gpu_export_ioctl`・`gpu_import_ioctl`・`gpu_shared_release`・`gpu_resource_reserve` | display-scanout（import だけが blob を保つ）、gpu-check.c |
| 生きた descriptor は device の in-place の回復を止める | gpu.c `gpu_shared_hold_device`、回復の判定の `device->shares == 0U` | 無し |
| flag の 2 bit は FILEDESC_CLOEXEC・FILEDESC_CLOFORK に対応し、他の bit は EINVAL | gpu.c `gpu_export_install`、`gpu_export_ioctl`；kern/filedesc.c の fork（CLOFORK を写さない） | display-scanout（GPU_HANDLE_CLOEXEC） |
| fd は出力の copyout の後に入り、copyout が失敗したら予約を取り下げる | gpu.c `gpu_export_install` | 無し |
| import は入力の description が全部 0、保存した description を返す | gpu.c `gpu_import_ioctl`・`gpu_allocation_import_ioctl`（`kern_memcmp` と `request.image = shared->image`） | display-scanout |
| image 形と allocation 形は互いの import で EINVAL | gpu.c の `shared->image.version == 0U`・`shared->allocation.version == 0U` | 無し |
| 別 node は EXDEV（`GPU_IMPORT_SCANOUT` の画像形だけ例外）、allocation 形は常に EXDEV | gpu.c `gpu_import_ioctl`・`gpu_allocation_import_ioctl` | 無し |
| scanout import: display と import op が要る、source が page を示せる、足りない/短い page は EIO、resource_id は 0 | gpu.c `gpu_scanout_import`・`gpu_import_ioctl` | 無し（QEMU・実機の試験が要る） |
| export の image の検査の順と値 | gpu.c `gpu_image_validate` | gpu-check.c（32×32、usage 1） |
| allocation の検査（schema 非 0、metadata_bytes ≤ 128、後ろは 0、device_id 0、size 一致） | gpu.c `gpu_allocation_validate` | 無し |
| export の errno（EACCES・EOPNOTSUPP・EOVERFLOW・EMFILE・ESRCH） | gpu.c `gpu_export_ioctl`・`gpu_shared_hold_device`・`gpu_export_install`；kern/filedesc.c `filedesc_reserve_many` | 無し |
| import の errno（ENOSPC・EINVAL の max_resource_bytes・EOVERFLOW） | gpu.c `gpu_resource_reserve`・`gpu_handle_allocate` | 無し |
| 能力の bit: image 形は `GPU_CAP_SHARE`、allocation 形は `GPU_CAP_ALLOCATION_SHARE`、import の `none` | gpu.c の各 handler の最初の検査；`GPU_BLOB_*` の検査は `gpu_blob_ioctl`；登録の検査は share op の組 | 無し |
| 定数・構造体の Meaning（`GPU_IMAGE_LINEAR` ほか、サイズ 88・96・184・192） | include/uapi/gpu.h・gpu-allocation.h・gpu-scanout.h、checker の構造体の照合 | checker |

読み直した 3 つ（`Errors:` の list から、handler の分岐を読み直して条件が合うことを確かめた）:

1. `GPU_ALLOCATION_EXPORT` の `EINVAL`: `gpu_allocation_validate` を読み直した。version・size、device_id 非 0、resource が blob でない、`allocation_bytes` が resource の大きさと違う、schema 0、`metadata_bytes` が上限を越える、metadata の後ろが非 0、の順で全てに一致。`fd != -1` と flags の未知の bit は handler の入口の検査。
2. `GPU_RESOURCE_IMPORT` の `EXDEV` と `EOPNOTSUPP`: `gpu_import_ioctl` を読み直した。`EXDEV` は `foreign && !(flags & GPU_IMPORT_SCANOUT)`。`EOPNOTSUPP` は入口（`GPU_CAP_SHARE` も scanout import も無い）、`gpu_scanout_import` の最初の検査、同 node で `ops->share == NULL` の else の枝。画像形でない descriptor は `EXDEV` より前に `EINVAL`（順序もそのとおり書いた）。
3. `GPU_RESOURCE_EXPORT` の image の `EINVAL`・`EOPNOTSUPP`: `gpu_image_validate` を読み直した。tiling と memory_type の `EOPNOTSUPP` が usage の `EINVAL` より前、format の `EOPNOTSUPP` が width・height の `EINVAL` の後、という順で、文書の分け方（`EOPNOTSUPP` は tiling・memory_type・format）と合う。

### 未確認・未実施

- 未確認: QEMU・実機での動作（この Phase は文書だけ。ALLOCATION_* と scanout の跨ぎの import は source tree の試験が無い）。driver ごとの export・import の errno の全数（venus・nvrtx・ghci だけ読んだ。powervr・mali・bcm2711・i915・amdrdna の share は読んでいない）。socket での descriptor の受け渡し（文書は書いていない）。
- 未実施: 統合の Phase が照らす項目（gpu-requests.md の索引の写し、並行の Phase との同じ事の照合）。gpu-memory.md の `GPU_BLOB_SHAREABLE`・`GPU_BLOB_CROSS_DEVICE` の Meaning との表現の合わせは、その file がまだ PENDING なので未実施。
