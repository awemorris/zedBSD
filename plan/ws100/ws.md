<!-- awesome-plan project=zedbsd record=ws100 -->

# WS100: system bar の音量（icon・slider・確かめの音）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p008 は表の cleared（Q1 2026-09-30）に phase.md を合わせた、p013 は T1-087 PASS で Q1 の判定待ち。残り: p006（5330 の HDA、実機）、p009（L3 の音量の曲線、planned））
Primary Milestone: MG006
Related Milestones: MG003
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: p005 cleared（2026-09-30、Settings の Sound の頁）。L1（まず動く）がそろった（下の「段の計画」）。次は L2 の p006（5330 の実機、A7、ユーザー）
<!-- awesome-plan-current:end -->
作業の手引き（2026-10-01）: [guide.md](guide.md)

## 目標（2026-09-30 ユーザー）

「音声については、サウンドの音量を右上の通知領域にアイコンとして追加し、ボリュームを調整できるようにしたいです。また、動画再生はOSCデモの
あとで実装するとして、ボリューム調整のフィードバックの音だけは鳴るとうれしいです。」

## 達成基準（2026-09-30 main の案、同日朝ユーザーが「案のまま確定」。p005 の Sound の頁も入れる）

| # | 基準 | 確かめ方 |
| --- | --- | --- |
| A1 | system bar の右上の通知領域に音量の icon があり、今の音量（無音・小・中・大）を絵で示す | QEMU の画面 |
| A2 | icon を click（touch では tap）すると slider の popup が出て、drag で 0〜100% を変えられる。mute の切り替えがある。popup の外で閉じる | QEMU の自動の試験 |
| A3 | icon の上の wheel で音量が 5% ずつ変わる | QEMU の自動の試験 |
| A4 | 音量を変えるたびに短い確かめの音（約 100 ms）が、その音量で鳴る。連続の操作では重ならない | QEMU（`intel-hda` と `hda-duplex` を wav に録る）、実機はユーザーの耳 |
| A5 | 音量は再起動の後も保たれる（利用者の設定の file） | QEMU の自動の試験 |
| A6 | audiod が無い、または音の device が無いときは、icon が「音なし」の印になり、popup にその旨を出す。落ちない | QEMU（audio の device なし） |
| A7 | 5330 の実機の内蔵の speaker と headphone の端子から音が出る（HDA 8086:51c8、Alder Lake-P）。**デモに必須ではない**（下の判断） | 実機（ユーザー） |

動画の再生と、app ごとの音量は範囲の外（デモの後）。

## ユーザーの判断（2026-09-30）

「HDAが鳴らない場合は、デモでは鳴らさなくてもクリティカルではないことにしておきましょう。legacy modeのHDAはfirmwareがいらないのではないかなと
期待しています。legacy modeが削除されたとかだったら困りますが。」→ A7 はデモに必須ではない。p001 でまず legacy の HDA（DSP を使わない、firmware
不要の経路）で 5330 が鳴るかを調べる。鳴らなければ A1〜A6（icon・slider・設定・無音の扱い）だけでデモに出し、A7 は Future Work に回す。
legacy の HDA が 5330 の firmware の設定で無効にされていないか（PCI の class が 0x0403 の HDA として見えるか、BIOS の audio の DSP の設定）も確かめる。

## データシート（2026-09-30 main が調べた。ユーザー「legacy の経路があるかないかは、データシートを見るのがいいです。Core Ultraならlegacyがないかもしれないけど、Alder Lakeなら残っている気がするなあ。」）

- Intel 600 Series Chipset Family On-Package PCH（Alder Lake-P）の datasheet（Intel EDC、ID 691222）の「Intel HD Audio Controller Capabilities」:
  controller は **baseline の Intel HD Audio の動作（legacy DMA で codec と stream をやりとりし、音の処理は host の CPU）** と、Audio DSP の offload の
  低電力の動作の両方を持つ、とある。つまり Alder Lake-P には legacy の経路が残っている（ユーザーの見立てどおり）。
  - https://edc.intel.com/content/www/us/en/design/ipla/software-development-platforms/client/platforms/alder-lake-mobile-p/intel-600-series-chipset-family-on-package-platform-controller-hub-pch-datash/intel-high-definition-audio-interface-capabilities/
  - desktop の 600 series（ID 648364）の同じ節も同じ記述。
- 残る危険: 5330 の内蔵の speaker の amplifier や codec が HDA link の analog codec に付いているか（DMIC・SoundWire だけの構成でないか）。
  p001 で Linux の `/sys/bus/hdaudio` や codec の dump、または Kei の pci-hda の codec の列挙で確かめる。

## 前提と危険

- 音の経路は audiod（ws035-p009・p049・p050、unix socket と共有メモリ）と `src/drivers/pci/pci-hda.c`。QEMU の起動の log は `audiod: no device`
  （今の試験の QEMU に音の device が無い）。
- **5330 の音の controller は Alder Lake PCH-P の HDA（8086:51c8、Dell 1028:0b02）**。Linux は SOF（DSP）か legacy の HDA で扱う。legacy の HDA の
  経路で内蔵の codec（analog の speaker・headphone）が鳴るかは未確認で、鳴らなければ DSP の firmware が要る恐れがある（A7 の危険、p001 で調べる）。
  内蔵の DMIC は範囲の外。
- 音量の設定は WS089 の desktop の設定の file（`keiland_preferences_*`）に置くか、audiod が持つかを p001 で決める。Settings の Sound の頁（今は表示だけ）
  との関係も決める。

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [ws100-p001](phase001/phase.md) | 設計（[design.md](design.md)）: icon と popup、audiod の音量と確かめの音、保存、QEMU の音の試験、5330 の HDA の調べ | cleared（2026-09-30: 5330 は Linux も legacy の HDA を選ぶ（DMIC なし）が codec は未確認） | — |
| [ws100-p002](phase002/phase.md) | audiod: `AUDIOD_FEEDBACK`（約 100 ms の内蔵の音、重ならない）と、amplifier の無い device の software の音量。audiod-feedback と QEMU の wav | cleared（2026-09-30: `audiod-qemu.sh` PASS: 音 -12 dBFS、連打で重ならない、soft 100/50/10/0 = 0/-30/-54 dB/無音、mute、device なし） | p001 |
| [ws100-p003](phase003/phase.md) | libkeiland: `keiland_audio_*`（WS089 の案 + `keiland_audio_feedback`）、KEILAND_VERSION 15、host の試験 | cleared（2026-09-30: host 14/14、target の build warning 0） | p002 |
| [ws100-p004](phase004/phase.md) | zdesktop: icon・popup・wheel・確かめの音・設定の保存（`sound.volume`・`sound.muted`）、QEMU の試験（A1〜A6）、WS099 の C9 | cleared（2026-09-30: `volume-p004.sh` PASS（A1〜A6）、WS099 の C7・C8・C9 PASS） | p003 |
| [ws100-p005](phase005/phase.md) | Settings の Sound の頁に slider と mute（2026-09-30 朝ユーザー「入れる」） | cleared（2026-09-30: `volume-p005.sh` PASS: Settings → audiod・desktop.conf・system bar、system bar → Settings（数秒以内）、確かめの音の規則。WS089 の回帰、boot） | p003 |
| ws100-p006（案） | 5330: Kei を直に起動して pci-hda の codec と pin を読み、鳴るかをユーザーが聞く（A7、デモに必須ではない） | planning | p002、実機 |
| [ws100-p007](phase007/phase.md) | 規約（coding-style の全文）への合わせと全体の確かめ（audiod は WS100 で変えた関数とその周りだけ） | cleared（2026-09-30: 変えた関数と新しい code は style-check 0（sigsetjmp の例外 1）、audiod の残り 89 件は一覧、audiod-qemu・volume-p004・host-audio PASS） | p002〜p004 |
| [ws100-p008](phase008/phase.md) | L3: 確かめの音の遅れの計測と短縮（操作から音の始まりまで 50 ms 以内） | cleared（2026-09-30 Q1 の判断で。P4 は判定の扱いを待って uncleared で報告。QEMU の guest の中の経路（音量の変更から HDA の DMA がその byte を取るまで）の中央値 31〜37 ms・最大 42 ms で 50 ms 以内。host の WAV（QEMU の codec の buffer 42.7 ms と USB の経路を含む）は 106〜111 ms で参考。直しは入れていない。5330 は未実施） | p004 |
| [ws100-p010](phase010/phase.md) | BUG-153: system bar の slider の click が zdesktop 自身の保存の読み戻しで戻される（50 → 100）のを直す | cleared（2026-10-03 Q1、T1-018） | p004 |
| [ws100-p012](phase012/phase.md) | BUG-161: 音量の変更のたびに desktop.conf へ書かない。session の間は audiod だけが持ち、session の終わり（Log Out・終了）に zdesktop が一度だけ書く。BUG-153 の読み戻しの仕組みを外す | cleared（T2-009 PASS 3/3） | p004・p005・p010 |
| [ws100-p013](phase013/phase.md) | BUG-170: slider のドラッグでのフリーズ。確認の音はドラッグの途中で鳴らさず離した時に 1 回（system bar と Settings）。速いドラッグの試験 `volume-bug170.sh` | in-progress（q683、実装済み・T1 の試験待ち） | p004・p005 |

## 段の計画（2026-09-30 main 経由のユーザーの方針「広く浅く」: まず動く段をそろえ、磨き込みは段ごとの数値目標の小さな Phase）

| 段 | 内容 | Phase（案） | 数値目標 | 測り方 |
| --- | --- | --- | --- | --- |
| **L1（まず動く）** | A1〜A6 と Settings の Sound の頁 | p001〜p005・p007（cleared） | A1〜A6 の PASS、Settings と system bar の同期 3 秒以内 | `volume-p004.sh`・`volume-p005.sh`（QEMU） |
| L2（実機で鳴る） | 5330 の内蔵の speaker と headphone（A7、デモに必須ではない） | p006-a: pci-hda の codec・pin の log（diagnostics）<br>p006-b: 5330 で直に起動して読む、鳴るかをユーザーが聞く<br>p006-c（要れば）: Intel 固有の設定（TCSEL・snoop・EM2）か codec の quirk | 5330 で確かめの音が speaker と headphone の両方から聞こえる。codec の log が 1 行以上 | 実機（ユーザーの耳）、SSH の kernel log |
| L3（磨き込み） | 確かめの音の遅れ、音量の曲線 | p008: 確かめの音の遅れの計測と短縮<br>p009: 音量の曲線の実機の調整 | 操作（release・wheel の notch）から音の始まりまで **50 ms 以内**（QEMU の wav と log の時刻）。0〜100% の段ごとの大きさの差が耳で等しく聞こえる（実機で 25・50・75・100% の dB が -30・-15・-7・0 dB ± 3 dB） | QEMU: `hda-wav-check.py` の時刻と zdesktop の log の時刻の差。実機: 録音か騒音計（ユーザー） |
| L4（後） | app ごとの音量、出力の選択（headphone・HDMI）、動画の音 | 範囲の外（デモの後） | — | — |

- 各段の Phase は、前の段がそろってから Queue に入れる（L1 は済み）。L2 は実機とユーザーの時間が要る。L3 の数値は案で、ユーザーの確認が要る。

## ユーザーの判断（2026-09-30 朝）

L3 の数値目標（操作から確かめの音が鳴り始めるまで 50 ms 以内、実機の音量の曲線 25・50・75・100% が -30・-15・-7・0 dB ± 3 dB）は「案のまま」で確定。

### 進め方とハーネス（2026-09-30 Q1 の補足）

- **L2（実機で鳴る）の作業像**: p006-a で pci-hda に codec・widget・pin の一覧を dmesg に出す診断を足す（QEMU で形を確かめる）。
  p006-b はユーザーが demo の image を 5330 で起動し、エージェントが ssh で dmesg を読んで speaker・headphone の pin と amplifier を特定し、
  `audiod-feedback`（試験の client）で確かめの音を鳴らす。聞くのはユーザー。鳴らなければ p006-c で codec の pin の設定か GPIO の quirk を足す。
- **L3 の音量の曲線は、実機でも mic 無しで測れる見込み**: codec の amplifier の capability（step の数と 1 step の dB）を読めば、百分率ごとの
  gain は計算で出る。実機で要るのは「codec の値が意図どおりに書かれたこと」の確かめ（ssh で codec の amp の値を読み戻す）だけ。
  software の音量の側は QEMU の wav で測れる（p002 の `audiod-qemu.sh` と同じ）。
- **L3 の遅れの測り方**: QEMU の wav の音の始まりの時刻と、zdesktop の log の操作の時刻（同じ clock に揃える）の差。

### Q1 の判断（2026-09-30、p008）

- L3 の確かめの音の遅れは、QEMU では **guest の中の経路**（zdesktop の音量の変更から HDA の DMA がその byte を取るまで、`feedback-latency.sh`）で判定する。
  host の WAV の値は、QEMU の hda-codec の 8 KiB（42.7 ms）と host → usb-tablet の経路を含み、実機に無い分なので参考にする。合否は 5330 の実機で
  （guest の log の部分はそのまま使える）。これは WS094・WS102 の判断（QEMU の値は参考、実機で判定）と同じ扱い。
- kernel の audio の fragment（4096 byte = 21.3 ms）を小さくする直しは、今は入れない（割り込みの増加、lead と drain tail の直し、QEMU で途切れる恐れ。
  音はデモに必須ではない）。再検討のきっかけ: 5330 の実機で操作から音まで 50 ms を超えたとき。


## 2026-10-04 UAT の結果（Q1）

実機で音量の click（BUG-153）と session の終わりの保存（BUG-161）は OK。**slider のドラッグでしばらくフリーズ**（system bar・Settings、[BUG-170](../bugs/BUG-170.md)、最優先）。確認の音を値の変化ごとに鳴らしている疑い。
