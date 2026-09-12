<!-- awesome-plan project=zedbsd record=ws014-p004 -->

# WS014 p004: 最終API整理・規約全文確認

<!-- awesome-plan-current:start -->
Status: planning
Phase disposition: normal
Parent: [WS014](https://github.com/awemorris/zedBSD/issues/15)
Queue: none
<!-- awesome-plan-current:end -->

Combined ID: `ws014-p004`
Primary Milestone: MG006

## 目標・依存

p002/p003で修正された最終ソースとU/K・callback・PCI連携資料を照合し、WS014の受け入れを確認する。p003で必要出力が成立してから行う。

## 手順・受け入れ

適用するcoding-style.md/Guardrail全文を読み、変更範囲の規約・層分け・所有権・参照寿命・エラー経路をレビューして残る問題を解決する。公開するversion/feature、必須/任意callback、未対応機能を整理する。p003の最終ソースでのbuild・画面取得ループ証拠を確認し、修正によって必要な限定回帰だけを行う。変更がない場合は既存の有効な検証を無意味に繰り返さない。

## 引き渡し

既知の制限、API差分、再現手順、host構成を後続i915 WSへ渡す。WS014完了は本Phaseの終了だけで自動判定せず、framework＋virtio/Venusで宣言した表示経路の受け入れを確認する。現在未着手。

## 適用規約・実行境界

[Guardrail](https://github.com/awemorris/zedBSD/issues/363)とローカルplan/coding-style.mdの全文を実装前に読む。HAL責務/hal.hの変更は別途適用承認が必要。既存PCI/VFS/VMの責務を確認し、大規模refactor前の配置を仮定しない。aggregate make checkは禁止。必要な対象buildはmake -j16と意味のある限定確認を用いる。無関係な変更を保護する。

ユーザーは計画・GitHub公開を指示した。まだ有限Queue、実行範囲と調査上限は選択していない。コード実装/build/QEMUは未実行。資料のgit add/commitはユーザーが行うためエージェントはadd/commit/pushしない。
