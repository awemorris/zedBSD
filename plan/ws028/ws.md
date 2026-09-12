<!-- awesome-plan project=zedbsd record=ws028 -->

# WS028: インストーラ実機動作

<!-- awesome-plan-current:start -->

Status: planning
Primary Milestone: MG003
Related Milestones: MG004
Objectives: O2, O4
Focused Goal: fg004
Parent: [Master](https://github.com/awemorris/zedBSD/issues/1)
Queue: none

<!-- awesome-plan-current:end -->

## 単一の到達目標

合意した対象実機でzedBSDインストーラによりインストールが完了し、インストール先から単独起動してinit/loginへ到達する。機種横断の分類はMG003に置き、このWSはインストーラ実機動作という一つの目標だけを扱う。PowerPC移植はWS027であり本WSに含めない。終了済みWS003/WS019/WS025は再利用しない。

従来合意済みの対象を引き継ぐ:

| 対象 | 受け入れ |
| --- | --- |
| PC-9821V13 / 64MB / CF-IDE | 通常のインストール完了とインストール先からの起動・login |
| Dell Latitude 5320 | 同上。実際に使う媒体・起動モードを選定して確認 |
| Let's Note SV7 | 同上 |
| Let's Note LX6 | 同上。USB起動時のbootパーティション選択問題も切り分ける |

全機種で全起動形式を網羅する条件は追加しない。書込み先・既存データ保持条件は実行計画で具体化する。現時点では実機操作・書込み・buildを実施していない。

## NVMe: ユーザー報告と仮説（2026-09-12）

- ユーザー報告: NVMeドライバが実機で動作していない。
- ユーザーの原因仮説: menuconfigにNVMeドライバの項目がないだけかもしれない。選択・設定の問題の可能性を保持する。
- 未特定: 対象機種、NVMeのPCI ID、利用したconfigと起動image、症状が未列挙/初期化失敗/I/O失敗のどこか、ログ。特定機種やドライバのコード欠陥とはまだ断定しない。
- 静的確認: 現行 `config/drivers/pci.drivers` に `CONFIG_DRIVER_PCI_NVME|bool|PCI NVMe storage controller|i386,amd64|y|` が存在する。`tools/menuconfig.py` はPCI drivers一覧を読む。
- amd64/i386は `platform/amd64/vmunix.mk` / `platform/pcat/vmunix.mk` の条件でNVMeソースを組み込み、`src/kern/platform/pcat.c` が条件付きで登録・namespace probeを行う。現行amd64/pcat/intelmac CI configはy、pc98/rpi4はn。
- したがって現行ソース上は「選択項目そのものが無い」とは一致しない。ただし実機で使用したmenuconfig版・選択platform・保存config・ビルド・配置imageのどこで無効化/不一致が起きたかは未確認。表示操作や実機再現を確認したという意味ではない。

切り分け順:

1. 使用機種・PCI ID・症状と、実際に起動したimage/hash・configを確定。
2. menuconfigで選択したplatformとNVMe項目、保存したCONFIG_DRIVER_PCI_NVME、コンパイル設定・リンクされたdriverを照合。
3. 実機に置いたカーネルがその生成物か確認し、PCI列挙→driver登録→controller初期化→namespace検出→I/Oの最後の成功点をログで特定。
4. 根拠から設定/組込み問題かdriver動作問題かを判断し、対象Phaseと最小修正を具体化。QEMU NVMeの成功だけで実機成功としない。

## WS003からの引継ぎ

[Future Work F-004](https://github.com/awemorris/zedBSD/issues/364)に保持したインストーラ関連部分（旧WS003 p018/p019、p026-p032）を本WSの計画入力として引き継ぐ。旧IDは終了済みの履歴として保持し、本WSで実行を具体化するときは新Phaseを作る。PPCの旧p033-p039はWS027に移管済みで対象外。

引き継ぐ課題はPC98 QEMUの/sbin表示、PC98 PCI/USBのmenuconfig、LX6のUSB bootパーティション選択、機種別受け入れと最終規約確認。ただし大規模リファクタリングとその後の修正があるため、以前の報告を今も未修正と断定せず最新コード/実機結果と照合する。旧p022-p024のユーザー完了判断を取り消さない。BUGの解決状態も証拠なしに変更しない。

## 計画の具体化と検証

NVMeと各機種の現在の停止点を整理して、単一目標に必要な有限Phaseに分解する。まだ未取得の実機ログを捏造した詳細設計や自動実行Queueは作らない。コード変更を含む場合は終盤に適用規約全文と既存機種への回帰を確認するPhaseを設ける。

Guardrail、C規約全文、ドライバのkern_* API境界、媒体保護の既存判断に従う。必要な限定テスト・選択構成のmake -j16・対象実機の通常インストール経路で検証し、aggregate make checkや未承認commit/pushを行わない。完了時にこのWSも終了し、別の目標には再利用しない。
