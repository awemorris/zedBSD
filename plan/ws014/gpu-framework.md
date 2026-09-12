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
