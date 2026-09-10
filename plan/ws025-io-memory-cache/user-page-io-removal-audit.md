# ユーザページI/O除去：HEAD基準の差分監査

日付: 2026-09-10
状態: 初期監査。ソース再構成は未実施。

## 基準と保全

基準コミットは `a58650ef08cec009aabacf63df9935429b71685b`（WIP）。
HEADのuaccess.cには既存pin経路があり、現在のview/lease追加はない。
HEADのvm.cには現在のvm_kernel_mapとio_destination経路がない。
ただしHEADのHALにはhal_vmap_*が既に存在し、scratch/DMAが使っていた。
したがって「ユーザページI/O以前」と「仮想マッピング導入以前」は同じではない。

全追跡ファイルのHEADとの差分をbinary patchで保全し、src/includeの未追跡
ファイルも別tarへ保存した。temp/rollback-baseline/manifest.jsonに基準とハッシュを
記録。これは監査開始時のスナップショットであり、その後の変更は上書きしない。
作業ツリーへのgit reset/restore一括適用は行わない。

## 差分の初期分類

| 分類 | 対象 | 再構成方針 |
| --- | --- | --- |
| 除去 | uaccess入力/出力view、syscall分岐、実験設定/計測 | HEADの通常経路を出発点に、独立修正だけ再適用 |
| 除去 | vmspace lease、alias凍結、INPUT_PROTECTED、専用待機・復元 | HEADとの各hunkを照合し、既存pin/COWの動作を保持 |
| 除去 | file/vmのユーザページdestination接続 | 通常読出し・キャッシュ処理へ再構成。別目的のprefix/回収修正は個別監査 |
| 保持 | HAL_SPACE_SYS、space/pmem名称、pmem引数展開 | 承認済みHAL仕様を再適用。旧hal_vmapを復活させない |
| 保持 | 固定syscall/user/sys fault入口 | p038の差分を独立して保持。SPARC初回フレーム方針は未決定のまま |
| 保持 | map/protの不要shootdown省略 | 前提・エラー経路をhunk単位に確認して保持 |
| 保持 | io_destination散布ユーティリティ | 汎用処理のみ保持。syscall/viewへの接続は除去 |
| 独立機能 | UAS、媒体喪失、unmount、dirty回収等 | ユーザページI/Oの削除に巻き込まない。vm/fileにも混在している |
| 要再設計 | vm_kernel_map管理層とDMA/scratchの利用 | borrowed専用部は除去。管理層全体を除去するなら既存scratch/DMAのVA予約・物理所有・解放責務を置換 |

「名前が追加されたものを全削除」「vm.cをHEADで上書き」は採用しない。
特にvm.cにはmount discard等の独立した追加が同居している。

## 再構成の手順

1. HEADと現在差分を読取り用基準として、関連hunkを機能ごとに台帳化する。
   include、利用側、ビルド、設定、テストも同じ単位で追跡する。
2. 独立した作業領域でHEADの対象ファイルから保持差分を積み上げる。
   単純な逆patchだけに頼らず、除去後の所有権とエラー経路を点検する。
3. HAL共有マップの利用に必要な最小のVA/物理所有責務を定義し、DMAとscratchを
   先に追従させる。ユーザページI/O専用のborrow/leaseは移植しない。
4. 再構成した対象ファイルと現在ツリーの差分をレビューし、無関係な変更を
   保持していることを確認してから適用する。コミットは行わない。
5. 旧機能の参照除去、通常read/write・バッファ再利用、DMA/scratch、関連VMの
   限定確認と3構成ビルドを行う。不要になった実験テストを整理する。

ユーザページI/Oは性能目標達成としてではなく、ユーザー判断による採用撤回として
p028とmasterへ記録する。範囲invlpg、pin集約、read-ahead等は別の変更として扱う。
この初期監査は全差分の精査完了や再構成完了を意味しない。

## 添付レビューとの照合

ユーザー提示資料: diff-audit-2026-09-10.md。
これは外部監査の提案であり、資料内のブランチ作成・コミット手順を実行指示とは
扱わない。既存の無コミット方針とユーザーの個別指定を優先する。

`git show HEAD:plan/queue.md` はq182 in-progress。HEADはユーザページI/O導入の
直前ではなくq231より約50 Queue前である。「HEADにview/leaseがない」という
初期確認は正しいが、間に成立した独立機能をすべて再適用する必要がある。

| 群 | 照合結果・扱い |
| --- | --- |
| インストーラ/PC98、BUG-019/020 | q182/q186とknown-bugsの記録に一致。保持。file/vmの低メモリ再試行をC群へ誤分類しない |
| NVMe複数コントローラ | q187完了記録に一致。保持 |
| UAS/streams/force unmount、BUG-021/022 | q228–q230のknown-bugs記録に一致。保持。vmのmount discardも対象 |
| 非EXEC map・同一protのshootdown省略 | q270/q268に一致。p028がunclearedでも、この独立差分の検証は成立。保持 |
| HAL固定入口 | q295–q303。保持。q303の実行未完了を継続台帳に残す |
| io_destination | 添付C群は全削除としているが、先のユーザー提示方針は汎用spanユーティリティ保持。ユーティリティと専用file/VM接続を分ける |
| vm_kernel_map | 添付A群は全保持を推奨。直前の「カーネル追加コード除去」と境界が異なる。borrow専用部と既存DMA/scratchの所有管理を分離して判断する。全保持を承認済みとは扱わない |
| xHCI統計 | IO_XHCI_IRQ_*はIO_SCALAR_*と分離して保持。ユーザページI/Oの統計だけ除去対象 |
| plan/docs | 履歴を保持し、現行機能の説明と廃止済み設計を明確に区別する |

レポートのA〜K分類をhunk監査の索引として採用する。ここでの照合は由来と
分類の確認であり、全hunkの意味的監査・再適用・ビルド完了を意味しない。
新たな性能改善やSPARC未決定変更を除去作業へ混ぜない。
