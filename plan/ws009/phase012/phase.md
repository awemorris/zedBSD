<!-- awesome-plan project=zedbsd record=ws009-p012 -->

# ws009-p012: DOC-54 gpu-memory.md: resource・blob・placed blob・transfer・map と mmap・device の問い合わせ

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
- src/drivers/gpu/gpu.c: `gpu_handle_allocate`・`gpu_resource_lookup`・`gpu_resource_publish`・`gpu_resource_pin`・`gpu_resource_unpin`・`gpu_resource_reserve`・`gpu_create_ioctl`・`gpu_destroy_ioctl`・`gpu_blob_ioctl`（placed の道を含む）・`gpu_user_range`・`gpu_transfer_ioctl`・`gpu_map_ioctl`・`gpu_mmap`・`gpu_mapping_release`・`gpu_device_query_ioctl`・`gpu_close`（object の解放）
- include/drivers/gpu/ の ops で placed の allocator と blob の op（どの driver が実装するかを grep で数える。ops 自体は文書に書かない）
- include/uapi/gpu-scanout.h の comment（placement と `GPU_BLOB_CREATE_PLACED`）

## 2. 作る物

- `docs/reference/gpu-memory.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: gpu-admission・gpu-recovery・gpu-bars・display-control（RESOURCE_*）、display-scanout（DEVICE_QUERY・BLOB_CREATE）。

## 4. 注意

- `GPU_BLOB_CREATE`（40 byte）と `GPU_BLOB_CREATE_PLACED`（64 byte、入れ子の blob の size に 64）は別の要求。
- mmap の errno は ioctl と別に書く（「Mapping into a process」の節）。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-memory.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-memory.md plan/ws009/phase012/phase.md plan/ws009/ws.md`。

## 結果

実施日: 2026-10-30（host の date）。担当: Sonnet 5.5 high（1 回の起動）。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし。着手の時）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-memory.md`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 18 capset rows, 1057 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (14 files, ..., final 0 files)`
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (334 links)`
- `git diff --check`: 出力なし（終了 0）。

書いた行数: docs/reference/gpu-memory.md は 344 行。

### 自分で決めた点

- `GPU_RESOURCE_DESTROY` の `Requires:` の capability は none。code は `GPU_CAP_RESOURCE` か `GPU_CAP_BLOB`、または scanout の `import_image` op のどれかを要求し、後者は bit でないので、gpu-doc-rules.md 2 節の 6 に従って本文に書いた。
- `GPU_DEVICE_QUERY` は open mode none・capability none（gpu_open が付けた readable・writable を見ない。admission の errno は出る）。
- `GPU_BLOB_CREATE` の `flags` の Direction は in/out。`GPU_BLOB_HOST` は `GPU_CAP_HOST_BLOB` が無い node で kernel が消し、書き戻した構造体でも消えている。
- `gpu_device_info.flags` は Direction を out（入力は 0 を要求、返りも 0）、`reserved` は zero。
- `GPU_BLOB_CREATE_PLACED` の errno は `EOPNOTSUPP` と `ENOTSUP`（同じ値）を別の項に分けた。source が使う名前（gpu.c の placed の道は `ENOTSUP`、他は `EOPNOTSUPP`）に従った。
- placed を実装する driver は PowerVR の render、BCM2711 の render と display の 3 つ（`blob_create_placed` を ops に入れている物を grep で数えた）。
- `GPU_RESOURCE_MAP` の cache の説明は、view の attributes（通常・uncached RAM・device・write-combine）の説明と、PowerVR・Mali・BCM2711 V3D の render が uncached を使うことまで。`GPU_BLOB_HOST` を cached で作る node（`GPU_CAP_HOST_BLOB`）は header の comment に従い「may」と書いた。この cache の挙動の driver ごとの確認は未確認の項を参照。
- mmap の errno は syscall 層（`src/kern/syscall.c`: `MAP_SHARED` 必須・`PROT_EXEC` 拒否・open mode）と `gpu_mmap`、`vm_device_create`、`vmspace_map_device` の errno を合わせて書いた。

### 食い違い（source・header・骨格の spec）

1. 骨格の spec（「device_id ... same for every node of one device?」）と [gpu.md](../../../docs/reference/gpu.md) の spec の「2 つの node が同じ device かを GPU_DEVICE_QUERY の device_id で知る」: code は `drv_gpu_register` の 1 回の登録ごとに 1 node で、登録ごとに別の `device_id` を `gpu_handle_allocate` で割り当てる。同じ物理 GPU の render node と display node でも `device_id` は異なる。文書は「node ごとに異なる」と書いた。gpu.md の担当の確認が要る。
2. `gpu_blob_create` の header の comment は `flags` を入力として説明するが、code は `GPU_BLOB_HOST` を消した値を書き戻す（`extended.blob = request` の後に `copyout`）。文書は in/out と書いた。
3. 骨格の spec は `GPU_RESOURCE_MAP` で「storage resource を map できるか code を読め」と指示。core は kind を見ず driver に任せる（`gpu_map_ioctl` に kind の検査がない）。文書は「driver の決定」と書いた。
4. 骨格の spec の「destroy が mapped・display が借りている・pending work・export 済みの object に何をするか」: code は mapped なら `EBUSY`、ungated の display 呼び出しが pin している間は待つ、pending work は見ない、export 済みの descriptor は resource とは別の kernel handle（`gpu_export_ioctl` で `export_resource` が別の object を作る）。
5. header・comment と code の矛盾は他には見つからなかった。

### 未確認

- 各 driver が `GPU_RESOURCE_READ`・`WRITE`・`MAP` に返す errno のすべて。PowerVR・BCM2711・Mali・Venus・GHCI の代表だけを読み、「Other values」に代表値を書いた。
- `GPU_BLOB_HOST` を cached memory で作る node（`GPU_CAP_HOST_BLOB` を出す amdrdna・nvrtx）の実際の cache attribute。
- VM_DEVICE_WRITE_COMBINE が実際に使える architecture（`vm_device_create` が `ENOTSUP` を返す条件は読んだが、arch ごとの値は読んでいない）。
- mmap した process が死んだ時の mapping の解放の順序（vm 層。文書は「最後の mapping が消えた時」とだけ書いた）。

### 未実施

- QEMU・実機の試験は未実施（文書の phase のため）。
- 出典に書いた試験の program を実行して errno を確かめていない。source の読みだけ。

### 自己の照合の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| Object model: handle は kernel 全体で一意の counter から、再利用なし | gpu.c `gpu_handle_allocate`（generation の増加、UINT64_MAX で EOVERFLOW） | 無し |
| Object model: 古い・他の open・0 の handle は EINVAL | gpu.c `gpu_resource_lookup`（0 と見つからないで EINVAL） | gpu-admission/main.c（destroy 後の再利用の試験は未確認） |
| Object model: close で残りを全部 destroy | gpu.c `gpu_close`（drain の後 resources を順に `resource_destroy`） | 無し |
| Object model: mapping は file を保持し、destroy は EBUSY | gpu.c `gpu_mmap`（`vm_device_create` が file を保持、`mapping_count`）・`gpu_destroy_ioctl`（`mapping_count != 0` で EBUSY） | 無し |
| Object model: pin 中の destroy は待つ | gpu.c `gpu_destroy_ioctl`（`pin_waitq` の sleep）・`gpu_resource_pin`・`gpu_resource_unpin` | 無し |
| Object model: GPU_PRESENT と複写の display present は storage だけ | gpu.c `gpu_present_ioctl`（`kind != GPU_RESOURCE_STORAGE` で EINVAL）・`gpu_display_present_resource` | venus-frame/venus-frame.c |
| Object model: 数の上限 max_resources・max_resource_bytes | gpu.c `gpu_resource_reserve`（bytes 超過 EINVAL、count で ENOSPC） | 無し |
| Object model: device_id は登録ごとに一意、再利用なし | gpu.c `drv_gpu_register`（`gpu_handle_allocate(&device->identity)`）・`gpu_publish_node` | display-scanout/main.c |
| Mapping: token は offset、1 ページから増え再利用なし | gpu.c `gpu_map_ioctl`（`next_mapping_offset`） | amdrdnactl/gpu-check.c |
| Mapping: 同じ resource の再問い合わせは同じ値 | gpu.c `gpu_map_ioctl`（`mapping_offset != 0` なら cache を返す） | 無し |
| Mapping: view の検査は EIO と EOVERFLOW | gpu.c `gpu_map_ioctl` | 無し |
| Mapping: MAP_SHARED 必須・PROT_EXEC 禁止・open mode との比較は EACCES | syscall.c の mmap（char device の枝）・gpu.c `gpu_mmap`（readable と prot の上限） | 無し |
| Mapping: token の範囲外・非整列は EINVAL | gpu.c `gpu_mmap` | 無し |
| Mapping: cache attribute は driver の view から | gpu.c `gpu_mmap`（attributes を `vm_device_create` に渡す） | 無し |
| GPU_RESOURCE_CREATE: usage と flags と bytes と handle の検査 | gpu.c `gpu_create_ioctl` | gpu-admission/main.c |
| GPU_RESOURCE_CREATE: 書き戻しの失敗で rollback | gpu.c `gpu_create_ioctl`（copyout 失敗で `resource_destroy`） | 無し |
| GPU_RESOURCE_DESTROY: capability の条件が bit でない | gpu.c `gpu_destroy_ioctl`（`GPU_CAP_RESOURCE｜GPU_CAP_BLOB` か `scanout->import_image`） | 無し |
| GPU_RESOURCE_DESTROY: Other values は none | include/drivers/gpu/gpu.h `resource_destroy` は void | 無し |
| GPU_BLOB_CREATE: 40 byte、size 検査 | gpu.c `gpu_blob_ioctl`（`request.size != bytes`） | venus-frame/client.c |
| GPU_BLOB_CREATE: flags の検査の順（未知 bit→HOST と共有→CROSS 単独→共有の capability） | gpu.c `gpu_blob_ioctl` | 無し |
| GPU_BLOB_CREATE: HOST は `GPU_CAP_HOST_BLOB` が無ければ消える | gpu.c `gpu_blob_ioctl` | 無し |
| GPU_BLOB_CREATE: resource_id 0 や object NULL は EIO | gpu.c `gpu_blob_ioctl` | 無し |
| GPU_BLOB_CREATE_PLACED: 64 byte、placement の検査 | gpu.c `gpu_blob_ioctl`（reserved・flags・alignment 2 の冪） | 無し |
| GPU_BLOB_CREATE_PLACED: 条件が全部 0 なら通常の allocator | gpu.c `gpu_blob_ioctl`（`constrained` の判定） | 無し |
| GPU_BLOB_CREATE_PLACED: placed が無ければ ENOTSUP、COHERENT 拒否 | gpu.c `gpu_blob_ioctl`・powervr/render.c `blob_make`・bcm2711/share.c `bcm2711_blob_allocate` | 無し |
| GPU_RESOURCE_READ/WRITE: 検査の順（reserved→bytes→handle→offset→address） | gpu.c `gpu_transfer_ioctl`・`gpu_user_range` | gpu-admission/main.c（read）・venus-frame/client.c（write） |
| GPU_RESOURCE_READ/WRITE: kernel の buffer 経由、同期点ではない | gpu.c `gpu_transfer_ioctl`（`kern_calloc`、wait なし） | 無し |
| GPU_RESOURCE_MAP: readable 必須、offset・bytes は 0 | gpu.c `gpu_map_ioctl` | amdrdnactl/gpu-check.c |
| GPU_DEVICE_QUERY: 入力の 0 検査、roles の導出、EIO | gpu.c `gpu_device_query_ioctl` | display-scanout/main.c |
| GPU_DEVICE_QUERY: companion は全 in-tree driver で 0 | 各 driver の `query_device`（grep） | 無し |

### Errors から読み直した 3 つ

1. `GPU_RESOURCE_DESTROY` の `EBUSY`（mapped）: `gpu_destroy_ioctl` は lookup の後 `mapping_count` を読み、非 0 なら pin の待ちより前に `EBUSY` を返す。文書の「変更しない」の記述と合う。
2. `GPU_RESOURCE_READ` の `EFAULT`（address 0 など）が handle と範囲の `EINVAL` の後に出る点: `gpu_transfer_ioctl` は reserved→bytes→`gpu_resource_lookup`→offset→範囲→`gpu_user_range` の順。文書の「address は handle と resource の範囲の後に検査」と合う。
3. `GPU_BLOB_CREATE` の `HOST` と共有 flag の `EINVAL` が `GPU_CAP_HOST_BLOB` の無い node でも出る点: `gpu_blob_ioctl` は HOST と SHAREABLE/CROSS_DEVICE の検査を HOST を消す処理の前に置く。文書の記述（capability が無くても組み合わせは `EINVAL`）と合う。この点は本文には明示せず `EINVAL` の項に組み合わせの禁止として書いた。
