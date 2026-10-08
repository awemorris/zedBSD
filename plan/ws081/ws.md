<!-- awesome-plan project=zedbsd record=ws081 -->

# WS081: touch の操作の質（慣性のある scroll と、低い fps の touch の補間）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: 残りは Windows の機械での touch の計測（ユーザー）と L3 の p017（planned））
Primary Milestone: MG006
Related Milestones: MG002
Objectives: O1
Parent: [Master](../master.md)
Queue: なし
Resume point: 2026-09-30: L2 の準備の p016 cleared（道具と手順）。次は Windows の機械での計測（ユーザー、`plan/ws081/tests/windows-touch.md`。用意: touchlog 入りの image と boot.bat の ssh の転送）→ L3 の p017。p007（実機）→ p009。BUG-099 は tracking
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-28 ユーザー）

「タッチについては、ただマウスのように操作するのではなく、スクロールの操作みたいに余韻のあるやつとかも実装が必要で、これはHIDドライバ、
Waylandコンポジタ、ブラウザの3つに渡る実装と調整が必要な作業だと思います。計画は1カ所でやるのがいいですね。また、タッチのfpsが低い廉価な機種でも、
ある程度数式で補間して利用できるようにすることを目標に入れましょう。iPadみたいに120Hz駆動のタッチは、安い端末にはついてませんからね。」

1. **慣性のある scroll**（指を離した後も速度に応じて減速しながら続く、端での戻り）を、touch と touchpad で、ブラウザ・Files・Terminal・PDF Viewer 等で
   一貫した手触りにする。
2. **低い fps の touch の補間**: 報告の間隔が長い（例: 60 Hz 以下、不揃い）安い touch panel でも、数式による補間・予測（速度の推定、平滑化、
   表示の frame の時刻への resampling と短い外挿）で、scroll・drag・Notes の線が滑らかに見えるようにする。
3. 担当の層を 1 か所で設計する: HID の driver（正確な時刻の付与、報告の率の測定）、compositor（resampling・予測・gesture の判定・app への渡し方）、
   app（ブラウザ・Keiland の app の慣性の scroll）。

## 設計で決めること（p001）

- **時刻**: kernel が HID の報告に付ける時刻の精度（割り込みの時刻、USB の frame の番号）。evdev の event の時刻の意味。
- **補間・予測の数式**: 速度の推定（直近の点への最小二乗、1€ filter、Kalman 等の比較）、表示の vsync への resampling、予測の長さの上限と過剰な
  外挿の抑制。報告の率を自動で測り、率ごとに係数を選ぶ。
- **app への渡し方**: (a) compositor が生の `wl_touch` を渡し、app が慣性を計算する（GTK 等の形）、(b) compositor が scroll を
  `wl_pointer.axis`（`axis_source=finger`・`axis_stop`）に変換して app が慣性を付ける、(c) compositor が慣性まで作る。Wayland の標準の範囲で選ぶ。
  resampling・予測した点を app に渡すか、生の点を渡して app 側の共通の library で補間するか。
- **慣性の物理**: 減速の曲線（指数の減衰・摩擦）、速度の閾値、端での rubber band、二本指の scroll、fling の中断。Keiland の全 app で共通の
  library（libkeiland）にする。
- **試験**: touchinject（WS079 p012）で報告の率・jitter を変えた合成の入力（30・60・90・120 Hz、不揃い）を作り、補間の誤差と見た目を測る。

## Phase

2026-09-29 main の判断（design §8・§11）: p002 の範囲の変更を承認、注入の device と touchinject の Scan Time は p002 に、library の置き場所を承認、
p005 を scroller・gesture の library と app ごとの適用に分ける、p006 は WS074 に依存。design §10 のユーザーの判断は main が伝え、返事までは既定の案で進める。

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [ws081-p001](phase001/phase.md) | 設計（上の項目、数式の比較の host の試験を含む）→ [design.md](design.md) | cleared | WS079 p012・p013 |
| [ws081-p002](phase002/phase.md) | kernel: 1 報告 1 時刻（URB の完了の時刻）と Scan Time の `EV_MSC`/`MSC_TIMESTAMP`、input の層の `EV_MSC`、注入の device と touchinject の Scan Time・µs の間隔（率の測定は p003）、WS079 p009 から移管の規約の是正 | cleared | p001 |
| [ws081-p003](phase003/phase.md) | 補間・予測の library（host で試験、率・jitter ごとの誤差の測定）→ `userland/desktop/libkeiland/motion.c`（公開は p004） | cleared | p001 |
| [ws081-p004](phase004/phase.md) | compositor: resampling・予測の適用（window の drag・端のジェスチャー）、wl_touch の時刻（Scan Time の τ）、library の公開（Makefile・exports.map・keiland.h） | cleared | p002、p003 |
| [ws081-p005](phase005/phase.md) | 慣性の scroll と touch の gesture の共通の library（libkeiland の scroller・gesture、host 試験、`KEILAND_VERSION` 10） | cleared | p004 |
| [ws081-p014](phase014/phase.md) | 指の drag and drop（compositor の `data.c`・`touch.c`: wl_touch の down の serial で start_drag を受け、drag を指に付ける）と Terminal の選択の文字の指の drag（main の判断 (1)、2026-09-29） | cleared | p011 |
| [ws081-p010](phase010/phase.md) | Files への適用（tap・長押し・scroll と慣性、指の drag and drop） | cleared | p005、p014 |
| [ws081-p011](phase011/phase.md) | Terminal への適用（scrollback の px 単位の慣性の scroll、tap の click、長押しからの単語の選択） | cleared | p005 |
| [ws081-p012](phase012/phase.md) | PDF Viewer への適用（scroll と慣性、二本指の拡大、page mode の swipe） | cleared | p005 |
| [ws081-p013](phase013/phase.md) | Notes の指の scroll・pinch・double tap・toolbar の tap と掌の判定（ペンは線。design §3.8 の指の線は main の指示で取りやめ、§5.6） | cleared | p005 |
| [ws081-p015](phase015/phase.md) | Notes の「指で書く」の切り替え（2026-09-29 ユーザーの決定: 既定は今のまま指は scroll・pinch。toolbar に切り替えを足し、入れた間は一本指で線、二本指で scroll・pinch。実機でペンが使えない場合のデモの備え） | cleared | p013 |
| [ws081-p016](phase016/phase.md) | L2 の準備: touch の報告の記録の道具（`touchlog`）と集計、注入の touch での確かめ、Windows の手順（`windows-touch.md`）、画面の反応の時間の script（`touch-latency.py`、p017 用） | cleared（2026-09-30: `touchlog-check.sh` PASS（60 Hz・90 Hz の揺れ・3 つの欠けを正しく数える）。Windows の計測はユーザー） | p015 |
| [ws081-p018](phase018/phase.md) | Windows の QEMU 用の demo の image（`config-amd64-demo-win.mk`・`build-demo-win.sh`: 5330 の demo の app の一式を Venus で、touchlog、Text Editor）と `windows-touch.md` の用意 | cleared（2026-09-30: `build/ws081-demo-win/hdd-image.img`、boot-test PASS、Venus で session と ssh の password の 1 行の touchlog の SUMMARY。Windows は未実施） | p016 |
| [ws081-p006](phase006/phase.md) | ブラウザの慣性の scroll と touch の入力（browser の shell） | cleared | p005、WS074（`browser.h` の scroll の範囲・overscroll。main の判断でこの Phase が足した） |
| ws081-p007 | touch の実物での調整（報告の率の実測、係数の調整）。**2026-09-30 ユーザー: デモの touch は Windows 上の QEMU（WS085）で行う。外付けの touch LCD は間に合えば別の計画**。まず Windows の QEMU の touch の経路（host の touch → QEMU の入力 device → Kei）で報告の率と遅れを測る | planning | p005、p006、touch の USB |
| [ws081-p019](phase019/phase.md) | BUG-156: PS/2 の mouse・touchpad の wheel（IntelliMouse・Explorer の切替、REL_WHEEL・REL_HWHEEL、動きの 9 bit の符号） | cleared（2026-10-03 Q1、T1-015） | — |
| ws081-p009 | 全文規約確認と回帰（必須の最終確認） | planning | 全 Phase |

touchpad の二本指の scroll（目標 1）の Phase は、design §10 の 4 のユーザーの判断の後に置く。

## ユーザーの判断（2026-09-29 に master から移した）

| 項目 | 決定 | 記録先 |
| --- | --- | --- |
| Notes の指（2026-09-29） | 実機の AES ペンがまだ認識されていない件で、Notes の指の扱いをユーザーに確認:「切り替えを付ける」→ 既定は指で scroll・pinch（掌の誤りの線を防ぐ）、toolbar の切り替えで一本指で線を引ける（その間は二本指で scroll・pinch）。ws081-p015 |

## 段の計画（2026-09-30 Q1、広く浅くの方針）

デモの touch は Windows の上の QEMU（WS085 の Venus）で見せる（ユーザー、2026-09-30）。どの段で止まっても、それまでの段は Files・Terminal・
PDF Viewer・Notes・Text Editor・デスクトップの icon で動いていること。

| 段 | 数値目標 | 測り方 | Phase |
| --- | --- | --- | --- |
| L1（済み） | Linux の QEMU の注入の touch で、tap・double tap・長押し・慣性の scroll・pinch が 6 app で動く | 各 WS の touch の試験 | p001〜p006・p010〜p015 |
| L2 | Windows の QEMU で、host の touch が Kei に届く。報告の率 ≥ 60 Hz、報告の欠け 0（10 秒の drag） | Kei の evdev の時刻の列（MSC_TIMESTAMP）を記録する試験 | p016（経路の確立と計測。Windows の機械の操作はユーザー） |
| L3 | Windows の QEMU で、指を置いてから画面の反応まで p95 ≤ 50 ms、慣性の scroll の frame の間隔の最大 ≤ 33 ms | compositor の log の時刻と画面の撮影 | p017（計測）、p018（係数の調整） |
| L4 | 外付けの touch LCD（間に合えば。ユーザーが別に計画） | — | p007 |
| 最後 | 全文の規約と回帰 | — | p009 |

### 進め方とハーネス（2026-09-30 Q1 の補足）

- **土台はある**: WS085-p001 で、Windows の QEMU の fork の SDL に `SDL_FINGERDOWN/MOTION/UP` を足し、仮想の `usb-multitouch`（10 指、Scan Time 付き、
  63 byte の report）へ渡す経路ができている。起動は `C:\Work\winq-zedbsd\boot.bat`。QMP の 2 指の注入で Home が開くことまでは確かめてある。
  **物理の Windows の touch panel からの SDL の event は未試験** — L2 の最初の確かめはここ。
- **p016（L2）の作業**: Kei の側で evdev の時刻を記録する道具（`plan/ws084/tests/evlat.c` を元に、`MSC_TIMESTAMP` と到着の時刻を並べる）を作り、
  ユーザーが Windows の touch panel で 10 秒 drag する。道具の出力（報告の数・間隔・欠け）を ssh で読む。Windows の側の操作はユーザー、
  Kei の側の記録と集計はエージェント。報告の率が 60 Hz を下回ったら、SDL の event の間引き（QEMU の fork）を疑う。
- **p017（L3）の作業**: compositor の log（`ZWL TOUCH` の down の時刻と、その指で動いた最初の frame の時刻）から p95 を出す script。
  慣性の scroll の frame の間隔は `--log-frames` の `ZWL COMPOSE at_ms=`（ws099-p001 で足した時刻）で測る。
- **ハーネスの置き場所**: `plan/ws081/tests/` に Windows の手順書（`windows-touch.md`: boot.bat の起動 → Kei の記録を始める ssh の命令 → drag →
  記録を止める）と集計の script。Windows の機械はエージェントが操作できないので、ユーザーの手順は 3 行以内に収める。
