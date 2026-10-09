# WS141: Normal non-cacheable RAM mappingの依存提案

2026-10-09、p005/p006/i13/i14の同一software scope内。差分は[patch](uncached-ram-mapping-proposal.patch)、まだ実source未適用。

## 理由

libvulkan `physical_load`はHOST_VISIBLE|HOST_COHERENTな標準memory typeを必須とし、非coherent host mappingを非公開にする。BCM2711はCPU cacheをsnoopせず、既存blobはcached RAM。単にCOHERENT flagを立てるのは誤り。既存 `kern_pmem_map_uncached`/arm64 `hal_pmem_map_uncached`は、direct mapをclean/invalidateしてNormal non-cacheable kernel aliasを用意できる。しかしcommon GPU/VM mappingはcached RAMかMMIOのみ。MMIOを流用するとDevice memoryのunalignedアクセス制約をRAMへ持ち込む。

## 具体的な変更とownership

- `include/drivers/gpu/gpu.h`: backend-private mapping flag UNCACHED_RAM=2を追加。user UAPI ABI/struct size変更無し。
- `include/kern/vm-device.h`: VM retained mappingのNormal uncached RAM属性=2を追加。
- `src/drivers/gpu/gpu.c`:既知属性の検査とMMIO併用拒否、mapping token→VMへの属性変換。
- `src/kern/vm-device.c`:既知属性の検査とMMIO併用拒否、Normal NCを既存HAL_SPACE_NOCACHEへ変換、RAMコピーは既存retained aliasを継続使用。

HAL API・HAL責務・既存driverのmapping policy・cached/MMIO defaultは変更しない。UNCACHED_RAMを選ぶbackendは同allocationのdirect-map/cached aliasを一切使用せず、native CPU/kernel/user aliasを同じNormal NC policyに保つ。user mapping lifetimeは既存coreがresource/fileをpinする。WS141のprivate bufferのuncached map/unmap所有は別途p006で実装する。native TLB/cache/fenceは依然必要。

## 現在の検証

`git apply --check` PASS。実sourceとは別のignored tempへpatch後source/headerを用意し、既存named rpi4 yと同じAArch64 compiler・flagsでgpu.c/vm-device.cの`-fsyntax-only`を実行: 両方exit0、warning/error0。変更内容をfull C/manualで確認。まだlink/build/host mapping属性試験・実機未実施、適用許可後に実施する。

## 承認の必要性

ユーザー指定の共有tree/別session運用とAGENTSの割当source境界により、この4 shared pathはWS141担当から勝手に適用しない。具体的patchへの承認を求める。HAL API変更の承認を求めるものではない。待つ間はprivate instance/device/typed runtimeを継続する。Master/Guardrail/他WS projectionはQ1。


## 承認と適用（2026-10-09）

上記exact question/4 pathのpatchへユーザー「OKです。」と回答。shared source境界の例外をこの差分に限り承認。`git apply`で担当worktreeの実sourceへ適用。HAL API/user UAPI変更無し。Master/Guardrailのexception投影はQ1。実sourceのnamed build/host属性確認は後続の同checkpointで行う。
