# 2026-09-11 ユーザー判断による計画更新

決定者: current user、このタスク。

> 計画を更新します。現在のPriorityリストを削除します。WS025の残件はclearedにして、WS025を閉じます。WS019も閉じます。WS006, WS022, WS002を閉じます。

2026-09-11のユーザー指示により現在のPriorityリストを削除。WS025のp029/p030/p032/p038をcleared、WS025をcompletedとし、既存completedのWS019/WS006/WS022/WS002とともに閉鎖する。未実施の検証をPASSへ変更せず、今回の計画上の受け入れとして記録する。q303はfinished/stoppedのまま。active Queueと新しいPriority/Focusはない。

- [ws025-p029](https://github.com/awemorris/zedBSD/issues/352): UASの修正後DMA/scratch整合・HS/SS QEMU再確認は今回実施していない。旧q230の結果を現行版の新規合格と読み替えず、追加確認を本Phaseの完了条件から外す。実機不要の判断は維持。
- [ws025-p030](https://github.com/awemorris/zedBSD/issues/353): 現行IMOD実装の静的確認と既定4000を維持。現行動作・0/160/4000比較、物理IRQ遅延・WLAN/HID並行測定は今回実施していない。これらの追加確認を本Phaseの完了条件から外す。
- [ws025-p032](https://github.com/awemorris/zedBSD/issues/355): PC-9821V13/64MB/CF-IDEのビープ停止について今回の原因特定・修正・実機起動確認は行っていない。実機成功を主張せず、現行対応とR10を本Phaseの追加完了条件から外す。旧QEMU証拠と現象記録を保持。
- [ws025-p038](https://github.com/awemorris/zedBSD/issues/361): 現行6引数fault API・cause値へのテスト追従、実行確認、SPARC初回ユーザーフレームの方針確認は今回行っていない。これらを本Phaseの追加完了条件から外す。q303の停止・uncleared試行履歴は変更しない。
- [ws025](https://github.com/awemorris/zedBSD/issues/26): ユーザーが残件p029/p030/p032/p038をclearedとしてWS全体を完了扱いにするよう明示指示。追加の開発・検証を完了条件から外し、既存の受け入れと今回の判断によりcompletedとする。p028はcanceledを維持。旧vmap設計を復活させず、新しい実行Queue・Future Work・Bug移管を作成しない。
- [ws019](https://github.com/awemorris/zedBSD/issues/20): q184/q186までのインストーラ受け入れと既存completed判定を維持し、今回の明示指示によりIssueを閉じる。
- [ws006](https://github.com/awemorris/zedBSD/issues/7): q147、実機USB HIDのユーザー確認、Noct/BeUI/Xzedの既存受け入れとcompleted判定を維持し、今回の明示指示によりIssueを閉じる。
- [ws022](https://github.com/awemorris/zedBSD/issues/23): q128のamd64/i386 TLS・dynamic回帰・PC98確認による既存completed判定を維持し、今回の明示指示によりIssueを閉じる。
- [ws002](https://github.com/awemorris/zedBSD/issues/3): 既存サービス受け入れとp021のユーザー判断によるcompleted判定を維持し、今回の明示指示によりIssueを閉じる。BUG-012の履歴・再発条件とWS001への引き継ぎは保持。

## 削除したPriorityの履歴

以下は削除前のスナップショット。現行リスト・実行承認ではない。

## Priority

2026-09-09のユーザー指定順。旧Priority wavesとp027〜p030の継続見送りを置き換える。

| 順位 | 対象 | 次に行う内容 |
| --- | --- | --- |
| 1 | [WS025](ws025/ws.md) | 67b28ce0修正後：p027完了、p028キャンセル、p029修正後確認待ち、p030現行IMOD実装確認済み（動作・比較確認残）、p032未着手。HAL名称統一は保持、旧vmap移設案は撤回、p038固定入口は現行APIでの確認残。[修正後照合](ws025/post-rollback-review.md)。 |
| 2 | [WS006](ws006/ws.md) | 完了（q147）。旧consoleイベントUAPI撤去、paired USB修正とXzed/Noct/BeUI受け入れ。 |
| 3 | [WS022](ws022/ws.md) | 完了（q128）。ELF TLS/TCB、exec、pthread、両x86受け入れ。 |
| 4 | [WS019](ws019/ws.md) | 共存経路p004/p005、元選択p027・属P性付きコピーp028・GPT処理p030完了。p006/p049で両モードの実インストールを受け入れ済み。p007のUFSスワップ負荷・断片化も受け入れ済み。p029も完了。追加p050のPC98グラフィカルFATインストールとターゲット単独ログインもq186で完了。 |
| 5 | [WS002](ws002/ws.md) | 完了。p021は現行試験合格とユーザー判断でcleared。過去の未再現事象はBUG-012に保持。 |
| 6 | [WS009](ws009/ws.md) | p008をq190で完了し、現行実装の文書整備完了。DOC-54のみWS014の手動保留に依存し、WSは未完了のまま保持。実行可能Phaseなし。 |

p027〜p030のq122での見送りは過去の実績として保持する。
追加必須項目（2026-09-09ユーザー指示）：[WS004-p050](ws004/phase050/phase.md)
はq187で完了。NVMeの1コントローラ制限を解除し、両列挙順の起動、
独立・同時I/O、永続化、検出失敗の分離を検証済み。
WS019は現行テキスト画面で完成を先行し、続いて[p029](ws019/phase029/phase.md)
で共通処理を使うBeUI版 `/sbin/zedinst-graphic` を追加する。テキスト版の最終配置も
`/sbin/zedinst` とする。添付の背景・デザインによる640x480 RGB24画面を事前合成する。
今回の優先指定を、実機や性能改善の証拠が既にそろったという意味にはしない。
USB haltの既知バグは下記へ登録し、この指定順へ無断で割り込ませない。


## 同期完了

GitHub本文12件、判断コメント10件、Issue閉鎖9件を読み戻し確認。Projectの31変更を確認し、WS Priority値6件とPriorityビューを削除、対象の状態・Focusを一致させた。

自動レビューの拒否説明後、ユーザーが「承認します」と明示回答し、GitHubへの本文・コメント公開、閉鎖、Project更新を承認した。以後の処理は成功。

[検証記録](2026-09-11-user-ws-closure-verified.json)。ソース変更・ビルド・実行試験・commit・pushなし。
