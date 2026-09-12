<!-- awesome-plan project=zedbsd record=ws014-p002 -->

# WS014 p002: GPUフレームワークのみの実装

<!-- awesome-plan-current:start -->
Status: cleared
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Queue: q305 / q305-i01 (finished / cleared)
<!-- awesome-plan-current:end -->

## q305実行結果（2026-09-12）

q305-i01 / [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)の修正を完了し、Phaseをcleared、Queue q305をfinishedとする。ユーザー指定どおり、GPUコアは通常の `drv_gpu_register(ops, private_data, **device)` / `drv_gpu_unregister(device)` で個々のdeviceを登録・解除する。GPU公開ヘッダのPCI依存、registration wrapper、専用service table、一括publish APIを除いた。同じ `struct drv_gpu_ops` を共有する複数deviceがそれぞれのprivate dataを持つ。

GPUと共通cdev/devfsの固定台数制限を動的registry・snapshotへ変更し、VFS mount時の登録消去を除いた。早期・追加登録を通常のdevfs経路で扱う。使用中のunregisterはEBUSYでhandle/backendを保持し、解除成功後はhandleを消費する。古いinodeは世代の異なるdeviceや解放済みbackendへ接続しない。既存5 callbackとresource ioctlの責務は維持する。

実GPU/cdev/PCI coreを使う40 GPUの通常・ASan/UBSan試験、ILP32/LP64のUAPI照合、共通cdev/devfsの80 device登録・全件列挙・mount・割当失敗・世代と参照寿命の試験がPASS。共通層の限定runnerは既存GCC -fanalyzer gatesを含めPASS。amd64対象kernel buildとvmunix checkerもPASS。変更箇所の適用コーディング規約全文とlifetime/rollbackをレビューした。最終source hashと手順は `plan/history/queue-q305.md` に保持する。

WS014はincomplete、p001/p003/p004はplanning。QEMU＋Venus・i915は未実行で、新しいactive Queueはない。GPUフレームワーク資料とp001の責務表を現行ops/登録契約へ更新した。ソースと資料はローカル作業ツリーにあり、git add/commit/pushはユーザーが行う。GitHubは計画Issue・Projectを同期し、本文・native lifecycle・Projectフィールドを読み戻す。

## 現行contract（q305）

# WS014 p002: GPUフレームワークの登録契約

q305 / q305-i01、2026-09-12。ユーザーが指定した通常のデバイス登録APIへ修正した。
実装元は `include/drivers/gpu.h`、`include/uapi/gpu.h`、`src/drivers/gpu/gpu.c`。
q304のPCI専用公開table・登録wrapper・一括publish・固定8台の方式は置き換えた。
旧試験結果はq304履歴に保持し、この契約の検証結果と区別する。

## 公開API

```c
int
drv_gpu_register(
	const struct drv_gpu_ops *ops,
	void *private_data,
	struct drv_gpu_device **result);

int
drv_gpu_unregister(
	struct drv_gpu_device *device);
```

`ops`は実装ごとの不変の関数表、`private_data`は1台のhardwareを表すbackend state。
同じopsを複数deviceで共有できる。registerの各成功は独立したdeviceを返し、
`/dev/gpuN`へ公開する。新しいGPU実装を加えてもGPUコアの列挙表や登録tableの
編集は必要ない。GPUコアはバスの種類を知らず、公開ヘッダにPCI型・PCIヘッダへの
依存を持たない。

コアがdeviceを動的確保し、空いている最小番号を割り当てる。解除後は番号を再利用
できるが、古いinodeは元のcdev generationを保持する。GPU固有8台、共通cdev16個の
固定上限を除く。メモリや番号・generationの表現限界による失敗は通常のerrnoで返す。
1 sessionあたりのresource handle上限32個は、device台数とは独立した既存ABIの
資源上限であり、この変更では維持する。

| 操作 | 呼出し側とコアの所有権 |
| --- | --- |
| register前 | 呼出し側がhardwareを初期化し、opsとprivate_dataを保持する |
| register成功 | コアがdeviceとcdevを所有する。呼出し側は返却handleを保存し、ops/private_dataを有効に保つ |
| register失敗 | resultはNULL。コアが途中の確保・公開を巻き戻し、ops/private_dataの所有権は移らない |
| unregister開始 | コアが新規open/ioctlを停止し、nodeを非公開にする |
| 使用中のunregister | EBUSY。handleとbackend stateを保持し、最後のclose後に呼出し側が同じhandleで再試行する。lifecycle同時実行によるEBUSYならwithdraw前のこともある |
| unregister成功 | handleは消費され、呼出し側はops/private_dataを解放できる。成功後に同じhandleを再使用しない |
| 古いinode/fd | cdevの参照とgenerationで保護する。offline後のopen/ioctlはENODEV、pollはPOLLERR/POLLHUP。既存resource/sessionはcloseで回収 |

unregister成功後に残る古いinodeはofflineなコアwrapperだけを保持し、backendを
呼ばない。解除前に受け入れたcallbackはsession参照の内側で完了する。
同じregistrationに対するlifecycle操作は呼出し側が所有を管理する。

## PCIからの利用

PCI IDの照合とdriver種類の登録は既存の `drv_pci_driver_register()` が担う。
GPUのregisterは検出済みのdevice instanceをGPUコアへ渡すAPIである。

PCIがattach成功後の公開とhardware detach前の解除を管理する場合は、PCI側の
static service callbackから上記の共通APIを呼ぶ。既存の汎用
`drv_pci_service_interface`を使えるが、GPUヘッダにそのtableを公開しない。
backend instanceにops/private_dataと返却GPU handleを保持すればよい。

| PCI側の境界 | 通常のGPU API呼出し |
| --- | --- |
| publish | `drv_gpu_register(ops, instance, &instance->gpu)` |
| unpublish | `drv_gpu_unregister(instance->gpu)`。成功後にinstance->gpuをNULLへ戻す |
| hardware detach | unpublish成功後にhardware/private stateを解放。EBUSY中は保持 |

公開失敗の巻戻しやdetach失敗後の再試行は既存の汎用PCI lifecycleを使う。
PCI→GPUを接続する実production経路と最小adapterをホストfixtureで検証する。
これは実GPU backendの実装・描画成功を意味しない。

## 共通cdevとdevfs

cdev registryはkernelの初期BSS状態から利用でき、動的な登録listで管理する。
VFS初期化は登録をresetしない。attachがdevfs mountより早くても、登録した
デバイスをmount後にそのまま見つけられる。GPUだけの再公開関数や初期化hookは
不要である。`cdev_reset()`は明示的な全件解除操作として残る。

devfsのディレクトリsnapshotも実際の登録数に合わせて確保し、16件で打ち切らない。
snapshot作成時にはcdev参照を保持し、コピー後は名前とgenerationを保存して
readdir時に現行登録と照合する。解除や同名再登録でもgenerationを取り違えない。statvfsは固定device poolの空き数を報告しない。

## drv_gpu_ops

version=DRV_GPU_INTERFACE_VERSION、size=`sizeof(struct drv_gpu_ops)`、reserved=0。
capabilitiesは0またはGPU_CAP_RESOURCE。userlandのGPU_ABI_VERSIONとは別の
kernel内callback contractであり、UAPI layoutは変更しない。

| member | 引数 → 出力 | 必須性とcontract |
| --- | --- | --- |
| open | private_data, `void **session` → errno | 必須。open descriptionごとのbackend stateを作る。失敗時は自身で回収 |
| close | private_data, session → void | 必須。全resource_destroy後に呼ぶ。解放が完了してから戻る |
| get_info | private_data, session, `struct gpu_info *` → errno | 必須。ゼロ初期化された出力にdriver_nameとmax_resource_bytesを設定。coreがversion/size/capabilities/handle数を確定 |
| resource_create | private_data, session, `const struct gpu_resource_create *`, `void **object` → errno | capabilityとセットで任意。検証済みkernel copyと割当済みhandleを受け、成功時はNULL以外のobjectを返す。失敗時は自身で回収 |
| resource_destroy | private_data, session, object → void | resource_createと対。明示破棄、copyout失敗、final closeで解放を完了して戻る |

coreはcallback中にspinlockを保持しない。異なるsessionは並行してcallbackへ
入れるためbackendが共有stateを保護する。同じopen descriptionは1 ioctlのみ
受け入れ、並行・再入ioctlはEBUSY。dup/forkされたfdは同じsessionを共有する。

## /dev/gpuNのU/K契約

初期ABIはrootだけがopenでき、credential不在も拒否する。open時の書込み権利を
sessionへ保存し、後のcredentialやfile status flag変更で拡張しない。

| operation | Uの要求 | Kの検証・処理 |
| --- | --- | --- |
| GPU_GET_INFO | version/sizeを入れたgpu_infoを渡す | 固定長copyin、layout確認、get_info dispatch、初期化済みsnapshotをcopyout |
| GPU_RESOURCE_CREATE | bytes、STORAGE usage、flags=0、handle=0 | 書込み権利、capability、layout、上限と空きslotを確認。backend allocate後copyout。返却失敗ならdestroy |
| GPU_RESOURCE_DESTROY | 同一sessionで受け取ったhandle | 権利、layout、slot＋generation全体を確認してdestroy。無効・破棄済み・別sessionのhandleはEINVAL |
| poll | fdの切断を待つ | offlineでPOLLERR/POLLHUP。render完了やvblankは未実装 |
| final close | 最後のfd参照を解放 | 残存resource全破棄→backend close→session参照解除 |

payloadはpointerなしの固定幅整数。gpu_info=56 byte、gpu_resource_create=32 byte、
gpu_resource_destroy=16 byte。version/sizeは厳密一致。handleはkernel全体で単調な
generationとsession内slotから成るopaqueな64 bit値で、wrap前にEOVERFLOWを返す。
未知ioctlおよび未対応resource操作はEOPNOTSUPP。

## p003への引継ぎ

mmap、GPU VM/DMA、context/capset/blob、transport/submit/fence、scanout/present、
display権限、cursor/hotplugは未実装。libvulkan.soと実GPU backendも未実装。
現行VFSのcdev mmap dispatch不足を含め、p003でvirtio/Venusの利用から補う。
HAL責務変更はこの修正に含まれない。44 callback案は将来機能を含む検討資料で、
上の実装済み5 memberとは区別する。

## 検証

- `plan/ws014/tests/run-gpu-framework-test.sh`: 実GPU/cdev/PCI core、通常・ASan/UBSan、ILP32/LP64 ABI、複数device、handle/権限/rollback/解除。
- `plan/ws006/tests/run-dynamic-cdev-devfs-test.sh`: 共通cdev/devfsの動的登録・列挙・mount・generation/参照寿命。元WSの受け入れを変更せず、共通層修正の回帰確認に使用。
- `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix`: amd64対象kernel buildと既存checker。

q305の検証は全項目PASS。40 GPU、80 cdev、通常・ASan/UBSan、共通層の既存GCC静的解析、ILP32/LP64 ABI、amd64 buildを確認した。
具体的な手順と対象hashは [Queue q305履歴](../history/queue-q305.md) に記録した。

## 検証対象・証拠



| 実行した確認 | 結果と実際の範囲 |
| --- | --- |
| `plan/ws014/tests/run-gpu-framework-test.sh` | PASS。実gpu.c/cdev.c/pci.c、40 GPU、ops共有/private分離、動的snapshot、追加登録、名前再利用と古いinode、権限、session/handle、割当・copyout失敗rollback、PCI順序と使用中解除。通常＋ASan/UBSan/LeakSanitizer |
| 同runnerのUAPI照合 | PASS。ILP32/LP64サイズ・offset・ioctl encoding |
| `sh plan/ws006/tests/run-dynamic-cdev-devfs-test.sh` | PASS。80 cdev登録、全件lookup/readdir、mount前登録保持、snapshot/ディレクトリ割当失敗、割当中の空registry化、世代の差替えとold fd、並行reset参照寿命。通常＋ASan/UBSan＋既存GCC -fanalyzer全gate |
| `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix` | exit 0。amd64 vmunix check: PASS。config.mk変更なし |
| ソースレビュー | root＋独立agentでlifetime、rollback、ID再利用、snapshot bounds、変更箇所への適用規約全文を確認。empty snapshot契約とコメント/宣言順等を修正後、上記確認を完了 |

既存WS006 runnerの旧host fixture defines/UAPI includesと、testだけに残った旧hal_key_event名を現行kern名へ合わせた。WS006を再開した判断ではない。GPU fixtureの追加GNU11 declaration-after-statement確認もPASS。補助的なstrict C89 compileは既存HAL/atomic/fileヘッダのinlineとhost snprintfで成立せず、対応済みC11 runnerで検証した。HAL実装・ヘッダ変更はない。

QEMU、Venus、ネイティブi915、実GPU描画は検証していない。mmap/submit/fence/display等はp003へ引き継ぐ。q304の過去証拠は上書きせず、現行contractへの合格はこのq305証拠で判断する。

## 検証対象のSHA-256

Base commit: `16077b64937804d9f155e1eaea65c380abb54e6c`。以下は未コミット作業ツリーの検証対象であり、GitHub mainへコードをpushした意味ではない。

| File | SHA-256 |
| --- | --- |
| `include/drivers/gpu.h` | `4b9719e1a5bfc3c0f9c3defe5690e92f2d90211a9b96fe8cbb93b1935a095617` |
| `src/drivers/gpu/gpu.c` | `dc36561c17369fa124a506d74de5d41baed592a6b5431aaba5441da983715420` |
| `include/kern/cdev.h` | `65db2668e9d1c5718fdaf85052efeb976505df5c24f765bf08efcebf09963ae9` |
| `src/kern/cdev.c` | `cd33bf379c0dd735c184e42776adea32f1710ade623c4f054db8f934aea66b52` |
| `src/kern/devfs.c` | `1e42520343c15aa9c9062b2ceb6758fe95306d6af8c7de1be0e4b80638cadfaf` |
| `src/kern/vfs.c` | `3dda68eacfff7c8acc7ddefefa92d291edeea33a95a8700e578113a23f7139fc` |
| `src/drivers/pci/pci.c` | `998250d083fcca13461fc3c628b7b4214fe4ffb12314daf8d6b90bf7c29ecaa3` |
| `include/drivers/pci.h` | `e9ce00c11a77c94a2cd0f80b9e81f5c09fcc780425fec620b8685ab459bb9214` |
| `include/uapi/gpu.h` | `82de065c9ecdff56c93676d1360e1d9666366e983281c7fa60cdd3fec7163051` |
| `plan/ws014/tests/gpu-framework.c` | `a321a959ab64712db40e55fd39c5fd2877d65982364987fdb399a77f79ea4575` |
| `plan/ws014/tests/run-gpu-framework-test.sh` | `baede988792d32be8e38816b36ee3ce50d8d3a2c4c9e9d6808629f622281a598` |
| `plan/ws014/tests/gpu-uapi-layout.c` | `8c91c3bd3d3f7fc22a4526c6fe39237222464d0225ed00779621635af40ce00c` |
| `plan/ws006/tests/dynamic-cdev-devfs-test.c` | `69602d5ee5aec9d058656cc64448c4a09a943755e93faa63f2c119b1ece61f4a` |
| `plan/ws006/tests/run-dynamic-cdev-devfs-test.sh` | `6b647ac913113bb717a6079d4e827f19b589531bdcafcf73b0392713924df0b3` |
| `plan/ws006/tests/input-device-ownership-test.c` | `c6be33901e55268b9cb206b0b55cd7dfdde3df119cee4fbcb4ce1f20f4548569` |
| `build/amd64/vmunix` | `4c2538d768b678b22987422b0e79381767aa0bc6d73feb9f3cbafda20575b8eb` |

<details>
<summary>q304の旧contract・実装結果（修正前の履歴）</summary>

Combined ID: `ws014-p002`
Primary Milestone: MG006

## 目標・範囲

GPUコア、struct drv_gpu_interface、device/session/handleの共通管理、/dev/gpuNの入口とcallback dispatch、PCI側の登録/解除連携を実装する。GPU driverはstatic callbackを持つinterfaceとinstance固有データを提供し、PCI側がattach成功後にGPUコアへ登録する。構造体版の44 callback案は検討材料で、必要な最小contractから具体化する。

## 前提・手順

p001から必要なinterface/PCI引渡し/所有権の設計判断を得る。p001の未決定を黙って確定扱いしない。PCI attachは現状int戻り値でGPU descriptor引渡しがないため、その連携方式を先に設計する。共通の権限・handle・参照寿命、必須/任意callback検証、公開/rollback/unregisterの順序を実装する。

## 非対象

virtio-gpuの実デバイス実装、Venus移植、Vulkan描画、i915実装は含めない。callbackとライフサイクルを検証する最小のテスト用backendは使用可能だが、GPU描画成功とは呼ばない。

## 受け入れ・検証

登録→device公開→session操作→callback dispatch→close/unregisterが成立する。未対応callback・不正handleを拒否し、登録失敗のrollback、使用中参照を持った解除で資源を早期解放しないことを限定テストで確認する。対象platform buildを通す。実GPUなしで達成できるframeworkの受け入れとする。

## 残件と再開条件

U/K表、callback/型/PCI連携資料と利用例を更新し、p003が接続可能なcontractを引き渡す。q304で実装・検証完了。実装契約とp003引継ぎを下に記録。

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの全文を実装前に読む。HAL責務/hal.hの変更は別途適用承認が必要。既存PCI/VFS/VMの責務を確認し、大規模refactor前の配置を仮定しない。aggregate make checkは禁止。必要な対象buildはmake -j16と意味のある限定確認を用いる。無関係な変更を保護する。

ユーザーの実行指示によりq304でコード実装・限定test・amd64 buildを完了した。QEMU/実GPU描画は未実行。資料のgit add/commitはユーザーが行うためエージェントはadd/commit/pushしない。

## q304実行開始（履歴）

ユーザー承認の唯一の実装対象は[WS014 p002](https://github.com/awemorris/zedBSD/issues/383)。GPUフレームワークのみを実装し、コーディングスタイル全文に従う。p003/Venus/i915は未開始。p001の必要contractをp002に具体化し、p001全体をclearしたとは扱わない。

Queue: q304 / attempt: q304-i01 / active・in-progress。時間枠は120 active minutes、内容はこの単一Phaseに限定する。重大な未解決仕様・外部blockが生じた場合は根拠と再開条件を記録する。git add/commit/pushは行わない。

## q304で具体化するp002 contract

ユーザーがp002をQueueへ入れて実行し、先にGitHub同期することを指示し、続けて「実行をお願いします。コーディングスタイルを守ってくださいね」と再確認した。p001全体は未clearのまま、p002に必要な既合意のinterface/PCI/所有権設計を以下の限定contractへ具体化する。

- struct drv_gpu_interfaceにopen/close/get_infoと任意resource_create/destroyを置く最小framework。44 callback全体、virtio/Venus/i915、実GPU描画は対象外。
- PCIの汎用service publish/unpublish callbackでattach後の登録とdetach前の解除を所有する。失敗時rollback。解除がbusyならdriver/private dataを保持して再試行し、稼働中資源を早期解放しない。
- managed cdev、session、session内の型付きgeneration handleをGPUコアで管理。初期openはrootのみ、権利はsessionで保持。不正handle/未対応operationを拒否。ioctlは固定幅・version/sizeを検証しcopyin/out。ユーザーへの返却失敗時は作成資源を回収。
- PCI検出がVFSのcdev_resetより先なので、GPU登録を保持し、reset後にdevice nodeを公開する初期化フックを用意する。live sessionがあるunregisterは非公開化後EBUSYとし、close後にPCIが再試行する。
- mmapは現行VFSがcdevに未対応のため本Phaseで未実装と明示し、p003で具体化する。HAL責務を変更しない。
- 検証は実production経路＋最小テストbackendによる登録/公開/session/ioctl/資源回収/失敗rollback/不正・stale handle/retained session解除、およびamd64対象kernel buildと全文スタイルレビュー。実GPUなしでframework基準を確認する。

## q304実行結果（2026-09-12）

q304-i01 / [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)をclearedとし、単一PhaseのQueue q304をfinishedにした。GPUフレームワーク、5 callbackのstruct drv_gpu_interface、PCI所有のservice公開・解除、/dev/gpuN、session/世代handleを実装した。

実gpu.c/cdev.cと最小backendの通常・ASan/UBSanテスト、ILP32/LP64の固定ABI照合、実pci.cのlifecycleテスト、amd64対象kernel buildがPASS。全文規約レビューで目的コメント・参照寿命を確認し、git diff --checkもPASS。clang-format 19.1.7のdry-runは全文規約と衝突する関数定義/forward declaration整形等を指摘したため非zeroで、機械的整形は適用していない。詳細はPhase本文の検証記録。

WS014はincomplete。p001の残る設計判断とp003/p004はplanning、Venus/実GPU描画/i915は未開始。次はp003でmmap、submit/sync、display等の不足を実利用から補う。現在の実行Queueはなし。コードと資料はローカル作業ツリーにあり、git add/commit/pushはユーザーが行う。

# WS014 p002: GPUフレームワーク実装契約

q304 / q304-i01、2026-09-12。実装元は `include/drivers/gpu.h`、
`include/uapi/gpu.h`、`src/drivers/gpu/gpu.c`。この資料はp003へ渡す
実装済みの境界を示す。Vulkan API責務表の44 callbackは将来機能を含む設計案であり、
今回実装した5 callbackと同じ完成度を意味しない。

## PCIからの登録

GPU backendは既存の `struct drv_pci_driver` でhardware attachを行う。
instance内の `struct drv_gpu_registration` に不変のinterface、private_data、
NULLのdeviceを設定し、attach内で次を呼ぶ。

```c
error = drv_pci_device_set_service(device,
    &drv_gpu_pci_service_interface,
    &instance->gpu);
```

失敗時はattach自身が確保済みhardware stateを回収してerrorを返す。成功時も、
GPU driver自身がGPUコアへ公開するわけではない。PCIがattach成功を確認し、
staged serviceのpublishを呼ぶ。GPUコアが番号を割り当て、VFS初期化後なら
`/dev/gpu0`〜`/dev/gpu7`を公開する。初期PCI列挙がVFSより先の場合は
登録だけを保持し、`kern_vfs_init()`内のcdev reset後に
`drv_gpu_publish_devices()`が公開する。このhookはVFSの初回初期化用で、
稼働後に任意のcdev resetを行う復旧APIではない。

汎用PCI service interfaceはpublish/unpublishの2 callback。GPU依存をPCIへ
埋め込まず、attachのint戻り値も変更していない。1 attachmentにつきserviceは
1個で、stagingにはhardware detach callbackが必要。PCIのprobe/detachは
deviceごとに直列化し、同時・再入操作にはEBUSYを返す。

| 境界 | 所有者と失敗時の規則 |
| --- | --- |
| attach | backendがhardwareとregistrationを確保。失敗時は自身で回収。service stagingだけでは公開されない |
| publish | PCIが成功したattachの後に呼ぶ。GPUコアは失敗時に公開とwrapperを巻き戻す |
| publish失敗後 | PCIがhardware detachを行う。detach失敗ならbindingと借用descriptorを残して再試行可能にする |
| unpublish | PCIがhardware detachの前に呼ぶ。GPUをofflineにし、nodeを隠す |
| session残存 | unpublishはEBUSY。PCIはhardware/private dataを保持し、最後のclose後にdetachを再試行する。FORCEもこの参照寿命を飛び越えない |
| unpublish成功後 | callbackはもうbackendを参照しない。PCIがhardwareを解放。hardware detach失敗なら次回はhardwareだけを再試行する |
| 古いinode | managed cdevの参照がoffline wrapperを保持。新規openはENODEV、backendを呼ばない。最後のcdev参照でwrapperを解放 |

registrationとprivate_dataはPCI hardware detachが成功するまでbackendが保持する。
`registration.device`はGPUコアだけが書く。unpublishのEBUSY後に再公開するAPIは
ない。closeが終わっても自動でdetachを実行するworkerはないため、PCIの呼び元が
`drv_pci_device_detach()`を再試行する。

## 実装済みcallback

`struct drv_gpu_interface`はversion=1、size=`sizeof(struct drv_gpu_interface)`、
reserved=0。不変のtableを用意し、capabilitiesは0またはGPU_CAP_RESOURCEとする。
未知version/capabilityはEOPNOTSUPP、欠けた必須callback・size・pairはEINVAL。

| member | 引数 → 出力 | 必須性とcontract |
| --- | --- | --- |
| open | private_data, `void **session` → errno | 必須。open file descriptionごとのbackend stateを作る。失敗時は自身で回収する |
| close | private_data, session → void | 必須。全resource_destroyの後に呼ぶ。解放が完了してから戻る |
| get_info | private_data, session, `struct gpu_info *` → errno | 必須。ゼロ初期化された出力にdriver_nameとmax_resource_bytesを設定。coreがversion/size/capabilities/handle数を確定する |
| resource_create | private_data, session, `const struct gpu_resource_create *`, `void **object` → errno | GPU_CAP_RESOURCEとセットで任意。検証済みkernel copyと割当済みhandleを受け、成功時はNULL以外のobjectを返す。失敗時は自身で回収する |
| resource_destroy | private_data, session, object → void | resource_createと不可分。明示破棄、copyout失敗、final closeに使用。解放完了までに戻り、失敗を返さない |

coreはcallback中にspinlockを保持しない。異なるsessionは並行してcallbackへ
入れるためbackendは共有hardware stateを保護する。同じopen descriptionは
1 ioctlだけを受け入れ、並行・再入ioctlをEBUSYにする。dup/forkされたfdは同じ
sessionを共有する。final closeはVFSのfile参照が尽きた後で実行される。

resourceのサイズ上限はget_infoで照会する。coreはsessionあたり最大32個の
handleを管理する。総GPU memory/DMA量の制限や割当失敗はbackendの責任であり、
このframeworkが物理memoryやDMA mappingを実装したわけではない。

## /dev/gpuNのU/K境界

初期ABIはrootだけがopenでき、credential不在も拒否する。open時の書込み権利を
sessionへ保存し、後のprocess credentialやfile status flag変更で拡張しない。
非rootへのdevice権限委譲、display owner/leaseの設計は後続で必要になる。

| operation | Uの要求 | Kの検証・処理 |
| --- | --- | --- |
| GPU_GET_INFO | version/sizeを入れたgpu_infoを渡す | 固定長copyin、version/size確認、get_info dispatch、初期化済みsnapshotをcopyout |
| GPU_RESOURCE_CREATE | bytes、GPU_RESOURCE_USAGE_STORAGE、flags=0、handle=0を渡す | 書込み権利、capability、layout、上限、空きslot確認。backend allocate後copyout。返却失敗時はresource_destroyで回収 |
| GPU_RESOURCE_DESTROY | 同一sessionで受け取ったhandleを渡す | 書込み権利とlayoutを確認。slot＋generation全体を照合してdestroy。無効・破棄済み・別sessionのhandleはEINVAL |
| poll | fdの切断を待つ | offlineでPOLLERR/POLLHUP。render完了やvblank通知は未実装 |
| final close | 最後のfd参照を解放 | 残存resourceを全破棄→backend close→session参照解除 |

payloadはpointerなしの固定幅整数で、gpu_info=56 byte、
gpu_resource_create=32 byte、gpu_resource_destroy=16 byte。versionとsizeは
厳密一致を要求する。ioctl番号はzedBSDのsys/ioctl.hで定義する。
handleはkernel全体で単調なgenerationとsession内slotから作るopaqueな64 bit値。
0は無効で、wrap前にEOVERFLOWを返す。Uは値の内部を解釈しない。

未知ioctlおよび未対応resource操作はEOPNOTSUPP。offlineになったfdのioctlは
ENODEV。解除前に開始したcallbackはsession参照の内側で完了し、hardwareの
解放はそのsessionの最終closeまで待つ。

## p003への不足・引継ぎ

mmap、GPU VM/DMA、context/capset/blob、command transport/submit/fence、
scanout/present、display権限、cursor/hotplugは未実装。libvulkan.soも実GPU
backendも今回追加していない。mmapは現在のVFSにcdev mmap dispatchがないため、
p003で既存VM責務に沿って必要なcontractを具体化する。HAL責務の変更はこの
Phaseの承認に含まれない。

p003はまずこのframeworkにvirtio-gpuを接続し、実利用からAPI不足を補う。
2D画面出力、Venus/Vulkan描画、QMPキャプチャの成功は各々実行証拠で確認する。
今回のホストテストはそれらの実動作を証明しない。

## 検証の再現

- `plan/ws014/tests/run-gpu-framework-test.sh`: 実gpu.c/cdev.c＋最小backendで権限、handle寿命、copyout rollback、deferred publish、解除、allocator/publication失敗を確認。通常実行とASan/UBSanに加え、ILP32/LP64の全field offset・size・ioctl値を照合する。
- `plan/ws014/tests/run-pci-service-lifecycle-host.sh`: 実pci.cでattach/publish/unpublish/detachの順序とrollback、busy・retry・再入を確認。
- `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix`: amd64の対象kernel buildと既存vmunix checker。

具体的な結果・対象hash・規約レビューはq304履歴へ記録する。

<details>
<summary>q304検証記録と対象hash</summary>

# q304: GPUフレームワーク実装

Queue: finished / Attempt: q304-i01 cleared / Phase: ws014-p002 cleared

ユーザーがp002をQueueへ入れ、GitHub同期後の実行を指示した。続けてコーディングスタイル厳守の指示を受けた。120 active minutesの単一Phase。開始前にQueue/Phase/WS/Master/Past LogとProjectを公開・読み戻しし、その後コード実装を開始した。

## q304実行結果（2026-09-12）

q304-i01 / [ws014-p002](https://github.com/awemorris/zedBSD/issues/383)をclearedとし、単一PhaseのQueue q304をfinishedにした。GPUフレームワーク、5 callbackのstruct drv_gpu_interface、PCI所有のservice公開・解除、/dev/gpuN、session/世代handleを実装した。

実gpu.c/cdev.cと最小backendの通常・ASan/UBSanテスト、ILP32/LP64の固定ABI照合、実pci.cのlifecycleテスト、amd64対象kernel buildがPASS。全文規約レビューで目的コメント・参照寿命を確認し、git diff --checkもPASS。clang-format 19.1.7のdry-runは全文規約と衝突する関数定義/forward declaration整形等を指摘したため非zeroで、機械的整形は適用していない。詳細はPhase本文の検証記録。

WS014はincomplete。p001の残る設計判断とp003/p004はplanning、Venus/実GPU描画/i915は未開始。次はp003でmmap、submit/sync、display等の不足を実利用から補う。現在の実行Queueはなし。コードと資料はローカル作業ツリーにあり、git add/commit/pushはユーザーが行う。

## 検証記録

| 検証 | 実行結果と限界 |
| --- | --- |
| run-gpu-framework-test.sh | PASS。実gpu.c＋実cdev.c。通常実行とASan/UBSanでpublication/permissions/handles/rollback/retirementを確認。held spinlockなしのcallback、同session再入拒否、別session進行、32個の上限と他session分離も確認 |
| gpu-uapi-layout.c（同runner） | PASS。-m32/-m64、全field offset、56/32/16 byte構造体、3 ioctl値一致 |
| run-pci-service-lifecycle-host.sh | PASS。実pci.c。attach/publish/unpublish/detach順序、busy/forced detach、publication rollback、cleanup失敗保持とretry、再入拒否 |
| make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk vmunix | PASS。amd64 vmunix checkerもPASS。config.mkは変更していない |
| git diff --check | PASS |
| 全文規約レビュー | C89宣言/定義、forward、目的コメント/段落、型・変数とprotocol説明、名前、call/return/条件、lock/ownership/参照寿命、HAL非変更を確認。reviewで欠けたif/caseコメントとheader空行を修正 |
| clang-format --dry-run --Werror（19.1.7） | 非zero。root設定が要求された複数行関数定義を畳み、1行forwardを分割する等、全文規約と不一致。全文規約を優先し自動整形を適用しなかった。formatter PASSとは報告しない |

immutable callback tableの初期化には参照先の宣言が必要なため、そのforward declarationをtableより前に置く。通常変数→forward→callback table→public→staticの順。backend/private dataはPCI detachまで借用、stale cdevはoffline wrapperだけを保持する。これらを独立レビューでも確認した。

全6 platformのソースリストへGPU coreを組み込んだが、今回buildしたのはamd64。host fixtureはkernel allocator/lock/uaccessを置換する。実schedulerの競合、VFS syscall、QEMU/実GPU描画の検証とは呼ばない。mmap等の不足はp003へ明示し、p001/p003/p004やWS全体をclearしていない。

検証時点UTC: 2026-09-12T12:16:18.812792+00:00

Host compiler: cc (Debian 14.2.0-19) 14.2.0

Kernel compiler: clang version 23.1.0 (git@github.com:awemorris/zedBSD.git fbdda93b5dc2c19bae8970a4dcbaf8cb417a7fbf)

Base commit: `16077b64937804d9f155e1eaea65c380abb54e6c` + uncommitted q304 changes

## 検証対象SHA-256

| File | SHA-256 |
| --- | --- |
| `include/drivers/gpu.h` | `fbcdd1ec161f308b52399c78c10e28423e711dbc56a6538620484bf6d28c4a3e` |
| `include/uapi/gpu.h` | `82de065c9ecdff56c93676d1360e1d9666366e983281c7fa60cdd3fec7163051` |
| `src/drivers/gpu/gpu.c` | `34124b044ee8047303fe4a306ee262a79c2d9ca7d6490199dbed584369af6c5d` |
| `include/drivers/pci.h` | `e9ce00c11a77c94a2cd0f80b9e81f5c09fcc780425fec620b8685ab459bb9214` |
| `src/drivers/pci/pci.c` | `998250d083fcca13461fc3c628b7b4214fe4ffb12314daf8d6b90bf7c29ecaa3` |
| `src/kern/vfs.c` | `94d58a44d0343e19fb30e207401c75e0737ba6f0737151d743c0655911d8a656` |
| `build/amd64/vmunix` | `d14bbe72c88a1898e3d0f29f2572c44a3c49fd77dd1268d7cf874dbfa83a34dc` |
| `plan/ws014/tests/gpu-framework.c` | `f69803ba7f8ca7fde13843d904809390750e8d623f56946c3e2289670622e14a` |
| `plan/ws014/tests/gpu-uapi-layout.c` | `8c91c3bd3d3f7fc22a4526c6fe39237222464d0225ed00779621635af40ce00c` |
| `plan/ws014/tests/pci-service-lifecycle-host.c` | `37fb77c494feef3080262e8934440948ed27880ce7bc1b436447a23173a29bfa` |
| `plan/ws014/tests/run-gpu-framework-test.sh` | `0929a8767b2a4de9709f89fafb24fd276bce194eaf57cbe06a7cf16024e28039` |
| `plan/ws014/tests/run-pci-service-lifecycle-host.sh` | `901dd4fe07f5fa741776200638b1ec350aef4fc7f5ac506f4d1d6d1c79d5fa3f` |
| `platform/amd64/vmunix.mk` | `6813267fda3caecf0c7247e3f9d771b4f094f07633e802c5bb09ae100db96cbd` |
| `platform/arm64/vmunix.mk` | `04c4c94d380c331ca8666e6e9de8d1525033619980085c12eb94e99746d6baeb` |
| `platform/pc98/vmunix.mk` | `229e7924be69641d2d93380be58d29f3523e4b4900bdd4f916fc355c6522af7d` |
| `platform/pcat/vmunix.mk` | `94a37e070d1cda8b8c7b87ddc590c7f75a3d58fddcfa8a39463d42cbfa7c6317` |
| `platform/sparcv9/vmunix.mk` | `335206c29a28787f374f4032ab7eab59bacaa9f27a7ba20dcf5386e7d8acd399` |
| `platform/x68k/vmunix.mk` | `6bae84cf945636ebd1685a8edb1e942394f2ffc43d3b1c5a814182b9de8b443c` |

</details>

</details>
