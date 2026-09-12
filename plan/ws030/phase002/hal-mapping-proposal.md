# q308 amd64 device mapping — 未適用のHAL変更案

2026-09-13。ユーザーの `hal_space_map_device()` に関する確認を受け、既存APIを使用する方針を明確化したレビュー資料。**HAL変更の許可はまだなく、候補の適用・コンパイル・実行はしていない。**

現在のVenusもPCI→kern_device_map→hal_space_map_deviceの経路を使用している。必要なのは新しいMMIO APIではなく、既存amd64実装の範囲拡張とユーザー空間への接続である。

## 適用を相談する具体範囲

1. `src/hal/amd64/space.c` の `hal_space_map_device` / `hal_space_unmap_device` を補完する。既存固定窓を維持し、それ以外のdevice物理領域は共有MMIOディレクトリの空き部分へ4KiB単位でマップする。仮想窓は512MiB、page tableは必要時に割当（最大1MiB、kernel lifetime）。物理RAMを512MiB確保する意味ではない。範囲外BARをPCI側で移動させずに扱える。
2. 同じファイルの `hal_space_map` は明示的な `HAL_SPACE_DEVICE` に限り、呼出元が所有するdevice領域をユーザー空間へマップする。通常RAMの検査は維持。CPU物理幅、RAM alias禁止、NXとcache policyを検査する。`hal_space_prot_query` も同じ分類を再確認して属性の抜け道を作らない。
3. `include/hal/hal.h` は既存 `hal_space_map` のコメントだけを補足する。新規HAL関数・引数の追加はない。

kernel/device user aliasesは同じuncached属性を使う。未実装のwrite-through指定を黙って置き換えず `HAL_ERR_UNSUPPORTED` とする。固定窓は永久map、動的viewは参照数で所有し、最後のunmapで全ready CPUのTLB退避を待ってからslotを再使用する。

## 静的レビュー

rootと別agentで範囲、cache alias、mprotect、CPU物理幅、割当・失敗rollback・参照寿命・SMPの順序を確認。初案の物理幅漏れ、protection変更による属性逸脱、kernel/user cache属性不一致、unsigned上限定義を修正した。現行hal_pmem_allocは非sleepでregistryへの逆lockを取得しない。page tableの初回割当・zeroingをIRQ無効のregistry lock内で行うため、最大窓では割込み遅延が増える点は実装後の確認対象。

## 許可後に必要な検証

- 既存固定窓とwindow末端を跨ぐ範囲、16MiBを超えるBAR、物理幅超過・RAM overlapの拒否。
- 同一viewの複数参照、最終unmap、slot再使用、途中のpage-table割当失敗とSMPでの失効。
- device usermapの読み書きとNX、mprotect後のdevice/cache分類保持、通常RAM mapping回帰。
- 対象amd64 buildとQEMU SMP起動、PCI/Venus既存経路、Vulkan memory visibility。まだいずれも実施していない。

候補差分はこの資料と同じdirectoryの `amd64-device-mapping-proposal.patch`。基準sourceと差分のSHA256は `hal-proposal-manifest.json`。source自体は未変更で、独立U作業だけ続行する。
