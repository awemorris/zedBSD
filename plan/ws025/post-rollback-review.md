# 67b28ce0 修正後の計画照合

2026-09-11追記: ユーザー判断でp029/p030/p032/p038をcleared、WS025をcompletedとして閉鎖。以下は閉鎖前のソース照合と未確認事項の履歴であり、追加作業の指示ではない。今回の実行検証・実機成功を表すものではない。

2026-09-10。対象コミット: `67b28ce04445787ad9225be4a703e3323587b6f1`。
ユーザー修正後のソース読取りと計画更新のみ。ビルド・実行試験・ソース修正は
行っていない。新しい目標・Phase・実行Queueは作成しない。

| 項目 | 現行ソースで確認した内容 | 計画への反映 |
| --- | --- | --- |
| p028 | src/includeにview/output lease、INPUT_PROTECTED、IO_SCALAR、io_destination、try_upgradeの参照なし | キャンセル。旧測定と設計は履歴。再実装・追加最適化の対象から除外 |
| p029 | UAS transport/disk、streams、abort/cancel、URB待機yieldの実装あり | 修正後確認待ち。q230合格を現行commitの合格と扱わない |
| p030 | pci-xhci.cにIMOD既定4000、65535上限検査、runtime+0x24への設定、IRQ entry/owned/event統計あり | 実装確認済み。現行動作/比較は未確認。実機測定成功とはしない |
| p032 | ユーザーが未着手と指定 | 現行修正後の対応は未着手。V13/64MB/CF-IDEの現象と旧QEMU証拠は保持 |
| p036 | hal_space_* / hal_pmem_get_stats等の名称体系が残る | 名称統一の完了を維持 |
| p037 | HAL_SPACE_SYSは残存。hal_vmap/vm_kernel_mapは除去。hal_pmem_request構造体引数へ変更 | 旧VM移設・引数展開案は現行仕様ではない。復活させない |
| p038 | 固定syscall/user/sys faultあり。faultはcause, mode, pc, address, vector, error_codeの6引数。detail引数なし | 現行APIへ計画を追従。旧fixture/旧バイナリの合格を流用しない |
| DMA/scratch | scratchはhal_pmem_alloc、DMA vectorはdrv_dma_alloc_coherentで確保。vmap fallbackなし | 旧非連続backingの主張を現行へ流用しない。p029確認で利用側の整合を確認 |

## 既存Phaseに残す確認点

- p029: 現行DMA/scratch・ヘッダと関連テストの整合を先に確認し、HS/SSの
  QEMU通常I/Oと媒体喪失後の解除・再公開の変更箇所を確認する。過去の全試験を
  無条件に繰り返さない。現行差分の内容に応じて対象を選ぶ。実機は必須にしない。
- p030: 現行実装の存在は確認した。既定4000を維持。旧0/160/4000比較と現行
  ソースの差分を確認して再利用可能な証拠を決める。物理IRQ遅延/WLAN並行は
  未確認として残す。新しい性能改修は追加しない。
- p038: fault引数・cause数値の変更に古いテストを追従する必要がある。
  sparcv9_user_task_prepareは依然、初回prime失敗時に保存ユーザフレームなしで
  user faultへ入る構造。既存の方針確認事項として保持し、勝手に修正しない。
- p023/p024: 旧vmap実装・非連続DMA backingは現行で撤去/変更されている。
  旧完了記録は履歴として維持し、復活を新規目標にしない。

q303は停止済みの履歴Queueとして保持。今回の読取りはq303再実行ではない。
