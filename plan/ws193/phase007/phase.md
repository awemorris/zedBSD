<!-- awesome-plan project=zedbsd record=ws193-p007 -->
# WS193 p007: DriversメニューとCPU共通の選択

Status: cleared
Disposition: normal
Parent: [WS193](../ws.md)
Queue: [Codex Drivers Queue](../codex-drivers-queue.md), menuconfig-drivers-20261010-i01

## 承認・範囲・設計

2026-10-10 current user: 「menuconfigのBoot OptionとDevelopmentの間に、Driversを入れてほしいです。これも全CPU共通で互換性と関係なく選べるとうれしいです。ただ、ドライバって選択できなかったりします？たとえば、USB CDC ECMをON/OFFにするビルドオプションとかって存在するんですか？」
既存config/drivers/*.driversとarchitecture/*.driversのCONFIG_DRIVER_* boolを一つのCPU共通一覧にする。重複keyは1行にまとめ、各boolをON/OFFできる。normalize/save/CPU切替で互換性を理由に選択を消さない。初期driver既定値は既存metadataのplatform条件を使い、未指定configはそのtargetの既定値を補う。ユーザーが指定した値はCPU切替で保持。
固定組込み（key '-' / kind fixed）にはflagが無いので選択対象に捏造しない。ECMにはCONFIG_DRIVER_USB_CDC_ECMがある。x86のsource選択は既に接続済み、arm64 source一覧へECMを移植する作業は本件に含めない。
有限範囲: menu/source、既存host fixtureの期待階層更新、target host/PTY/Make条件分岐確認、変更全文規則reviewと記録。driver/kernel/HAL/toolchain実装変更、image/network/実機試験/pushは含めない。main統合は具体的commitの承認を確認する。

## 依存・適用規則・受入

前提source c2b97e953がmainにある。p001の当時のdriverメニュー削除方針を、上の追加指示で置換する。旧scope/結果は保持。AGENTS.md/Guardrailのownership・検証限定・WIP・共有tree read-onlyを適用。Pythonは既存規約、C編集なし。新方針のshared Master/Queue/Guardrail投影・GitHub公開はQ1へ保留。
受入: 指定位置にDrivers、全6platformで同一一覧・ON/OFF・保存/load・CPU切替保持、initial/missing defaultsはplatform条件を維持。USB CDC ECMがy/nで保存され、x86 Make source一覧に反映する。arm64の選択とsource移植の状態は区別する。対象fixture/構文/diff-check/適用規則全文review。

## 初期検証 / menuconfig-drivers-20261010-i01（2026-10-11、階層化前）

flat UIの初期確認はPASS（階層化の追加指示前、未統合）。23boolをCPU共通のDriversへ追加。全6platformのtoggle/save/load/default parity/Make validation、real CPU切替選択保持、ECMのx86 Make条件分岐とrpi4保存、RPi4 real PTYの指定位置/ECM toggle PASS。既存対象fixture・Python構文・diff-check・変更全文規則review PASS。[証拠・commands・制限](../tests/drivers-menu-20261010.md)。
mainへ具体的成果commitの承認を確認する。arm64 ECMの実装移植は含めず、ユーザーへ既存sourceの差を説明済み。sourceの移植とUIの設定保存は別々に記録。WS全体のp003/受入は未完。shared投影/GitHubはQ1へ保留、pushなし、次Queueなし。

## 2026-10-11 UI階層の追加指定

user「Disk, Input, GPU, Audio, Ethernet, WiFi, USBみたいに階層化してほしいです。」。flat一覧の初期検証結果は上に保持し、未統合のUIを機能別階層へ変更して再確認する。driver keyの正本/CPU共通選択/初期値/保存のscopeは維持。config/drivers/menu.listへCPU共通の機能分類だけを置き、同じdriver_rows()をカテゴリへ振り分ける。ACPIと今後の未分類driverはPlatform。全23件が重複/欠落なく届き、EthernetにECM、DiskにUSB storage、GPUにBCM2711等が表示されること、全6platformで同じ階層の操作を確認する。

## 最終結果 / 階層化後（2026-10-11）

menuconfig-drivers-20261010-i01 cleared。Disk/Input/GPU/Audio/Ethernet/WiFi/USB/Platformの8分類へ23boolを重複・欠落なしで配置。全6platformで実drivers_menu/driver_optionsによる全カテゴリ全設定のtoggle/save/load、初期既定値の旧実装との一致、Make validation PASS。CPU切替保持、x86 ECMのsource条件分岐も再確認PASS。RPi4 real PTYでDriversの位置、8分類、EthernetのECMをOFFへ変更して保存を確認。Python構文/対象既存fixture/diff-checkと変更全文の適用規則review PASS。
最終差分はtools/menuconfig.py、config/drivers/menu.list、既存fixtureのtop-level期待値と本WS記録。分類metadataはUI配置だけを持ち、設定名・label・kind・defaultの正本は既存driver定義のまま。[証拠](../tests/drivers-menu-20261010.md)。main統合は具体的commitの承認待ち、pushなし。WS全体のp003は未完、shared投影/GitHub公開はQ1へ保留。
