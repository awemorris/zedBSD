<!-- awesome-plan project=zedbsd record=ws291 -->

# WS291 GPU の driver の描画の速さ: desktop 60 fps と重い 3D（AMD RDNA → NVIDIA → PowerVR）

- Master: [plan/master.md](../master.md)（2026-10-26 23:05 作成）。Primary Milestone: MG006。前提: [WS039](../ws039/ws.md)（amdrdna の driver、display・executor・desktop は実機で動く）。手本: [ws270-p007](../ws270/ws.md)（VF2 の PowerVR の desktop の速さ）。
- 追加（2026-10-26 23:08 ユーザー「WS291にさらにフェーズを追加して、DebianマシンでGPUを入れ替えて、NVIDIAでも同様に、デスクトップの60fpsと、モデルの50fpsをやります。これはP2がamdrnaの作業を終えてからです。また、さらにフェーズを増やして、PowerVRのドライバでも、50fpsをやります。VF2での作業は、RDNAで作業手順を確立したあとで、P3に依頼します。」）: NVIDIA（同じ Debian の機械で GPU を入れ替え、P2、RDNA の後）と PowerVR（VF2、P3、RDNA で手順を確立した後）を同じ WS に足す。
- Status: incomplete（2026-10-26 23:05 新設、p001 cleared）
- 出典: 2026-10-26 23:05 ユーザー「P2も立てます。ssh 10.0.10.3で、RDNAの接続されたDebianに入れます。USBメモリにはzedBSDが入っています。この環境でカーネルをアップデートしてUEFIの起動項目を変更しながら再起動し、amdrdnaドライバの改善を行ってほしいです。要領としては、PowerVRのドライバの改善のときと同じようにコンポジタを操作して時間を計測し、改善できる部分を改善してほしいです。デスクトップで60fpsをまずは達成します。スクリプトは既存のものがあると思います。もしかしたらすでに60fpsになっているかもしれません。そのあと、Vulkan Model Viewerを使ったり、その改良版を作ったりして、重たい3DシーンをLinuxドライバと同じfpsで描画できるように調整します。一つのやり方として思うのは、Linuxで60fpsを切るような重さに3Dモデルの量を調整し、たとえば50fpsにします。これをzedBSDでも50fps達成できるように調整し、できれば55fpsくらいまで、Linuxよりよくなるまでプロファイルベースで改善します。できそうですか？」

## 単一の到達目標

GPU ごとに同じ手順（desktop の 60 fps、重い 3D の model の 50 fps）で driver の描画の速さを揃える。最初は AMD RDNA で手順を確立し、NVIDIA・PowerVR に当てる。

- AMD RDNA: 実機 10.0.10.3（Navi10 の RX 5600 XT、Debian 13 と USB の zedBSD）で、(1) Keiland の desktop の操作（ws270-p007 の frame-ops の段）が 60 fps、(2) Linux（Debian の RADV）で約 50 fps になる重さの 3D の scene（mview か改良版）を、zedBSD の amdrdna で Linux と同じ fps、できれば 55 fps 前後の Linux 超えまで、profile を見て改善する。
- NVIDIA（nvrtx、10.0.10.3 で GPU を入れ替え）: desktop 60 fps と model 50 fps（RDNA と同じ形）。
- PowerVR（VF2）: model 50 fps（desktop の 60 fps は ws270-p007 で済み）。

## 手順と道具

- 実機の操作: Debian（ssh 10.0.10.3）から USB の zedBSD の /boot に vmunix・package を入れ、`efibootmgr --bootnext`（zedBSD USB の Boot0001 か KIOXIA の項目）で再起動、終わったら Debian へ戻る（zedBSD の BootOrder は変えない、BootNext の 1 回だけ）。zedBSD の側は ssh か serial の対話。console・serial の log で判定しない。画面の判定は keiland-shot の PNG。
- 計測: `plan/tools/frame-ops/`（frame-ops.sh・frame-times.py・key-latency.py・shot-diff.py）、mview の `--spin=N`（fps の報告）。Linux の基準は Debian の上の同じ mview（Wayland の上の Vulkan、RADV）を同じ解像度・同じ model で。
- 試験の方針（AGENTS.md）の例外: 実機の計測はこの WS の本体なので担当が直に行う（WS039 と同じ）。QEMU は使わない。

## 計測の決定

- 2026-10-26 23:10 ユーザー「zedBSDでは、graphicalログインは必要なく、graphicalログインをするなら、direct scanoutで限界まで3D性能を正しく計測しましょう。USBはまるごと書き換えてOKです。」→ login=graphical の起動は要らない。3D（mview）の計測は fullscreen の direct scanout（compositor が合成しない）で限界の性能を測り、direct scanout であることを確かめて記録する。Linux 側も同じ条件で比べる。10.0.10.3 の USB は丸ごと書き換えてよい。VF2（p020）でも同じ。

- 2026-10-26 23:52 ユーザー「そのベンチマークですが、command組み立ては毎回やってたらVF2の方がCPUが遅いので確実に遅くなります。頂点データやインデックスはバッファに入れておいて、描画コマンドの組み立て時間が最小限になるように変更してください。でないと実際のユースケースと合わないです。ただ、別途、たくさんのコマンドをキューするようなベンチマークも、必要かもしれませんね。」→ 主の 3D の benchmark は、頂点・index を device の buffer に置き、毎 frame の command の記録を最小にする形（instancing か記録済みの command buffer の再利用）に変え、Linux の基準を測り直す。別の benchmark として、毎 frame たくさんの command を記録・submit する mode も残して測る。
- 2026-10-26 23:54 ユーザー「コマンド投入がzedBSDでは遅い可能性もあるので、コマンドが大きいか、コマンドがたくさんあるか、は、RDNAのLinuxとzedBSDで比較できるといいですね。」→ command の投入の benchmark を 2 軸に: (a) 大きな command buffer（1 回の submit に多数の draw）、(b) 多数の submit（小さな command buffer）。量を段で振り、frame あたりの記録・submit・GPU の待ちの ms を RDNA の Linux と zedBSD で並べる。

## 受け入れ

- desktop: frame-ops の各段が 60 fps（ws270-p007 の基準の表と同じ形で記録）。
- 3D: Linux で約 50 fps の scene を定め（model・数・解像度・shading を記録）、zedBSD の fps が Linux 以上。各改善の前後の数値と profile を記録。
- amd64 の vmunix・変えた userland が warning 0。実機の証拠と未実施の事を分けて記録。

## Phase

- 2026-10-29 ユーザー（原文）: 「WS291は、Keilandのスレッドの修正を適用後でないと、正確にならないかもしれないです。」→ Q1: desktop の fps の受け入れ（p010 の 60 fps）は WS309（Keiland の thread の整理）の後に測り直して判定する。それまでは driver の側の直し（cursor・lock）だけを進め、compositor の側の調整はしない。

| Phase | 内容 | 依存 | 状態 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | desktop の計測と 60 fps（既に 60 fps なら記録して閉じる） | WS039 p009 | cleared（2026-10-26、main bad29b52f、全段 60 fps） |
| [p002](phase002/phase.md) | Linux で約 50 fps の 3D の scene を決める（mview の拡張を含む）、zedBSD との差を測る | p001、p002a | cleared（2026-10-27 00:43 Q1。qs40 ×6500 instanced で zedBSD 20.1・Linux 50.1 fps、command の重さの組 big-N・subs-N も） |
| [p002a](phase002a/phase.md) | amdrdna の executor に depth・index の draw・mip の texture（mview が動くまで） | p002 の Linux の基準 | cleared（2026-10-27 00:43 Q1。実機で Linux と pixel 一致） |
| [p003](phase003/phase.md) | RDNA: 投入の経路（vkQueueSubmit が GPU の完了を待たない非同期、record の 1 command と submit の費用を削る）。big-N・subs-N で Linux に近づける | p002 | cleared（2026-10-27 04:31 Q1） |
| [p004](phase004/phase.md) | RDNA: PA の small primitive filter・back-face cull・VGT の vertex reuse を RADV の値に、scene-6500 の GPU の ms | p003 | cleared（2026-10-27 08:34、driver の変更なし、差は NGG の culling） |
| [p005](phase005/phase.md) | RDNA: NGG A（passthrough、wave32 1 つの subgroup） | p004 | cleared（2026-10-27 13:52 Q1 判定、q1611。受け入れ: export の EXEC の wait の修正、1 体 pixel 差 0、6500 体 8 pixel、hang 回復、GPU 41.4 ms） |
| [p006](phase006/phase.md) | RDNA: NGG B+C（primitive の culling と頂点の詰め直し・属性の後回しを Mesa の形で一緒に。2026-10-27 14:50 Q1: compaction 無しの culling は GFX10 で止まる（仮説 A）ので p007 と統合） | p005 | in-progress・実機が戻るまで止め（2026-10-30 Q1。q1668: big-1000 35.5 → 57〜59 fps（Linux 87）、subs-1000 1 submit 48 → 39 µs（Linux 10）、scene-1500 209・scene-6500 49.6（Linux 200.4・50.1）） |
| p007 | RDNA: NGG C（vertex の詰め直しと属性の後回し、wave をまたぐ subgroup）、目標 GPU 約 20 ms | p006 | p006 に統合（2026-10-27 14:50 Q1。目標 GPU 約 20 ms は p006 の受け入れへ） |
| p008〜p009 | RDNA: HTILE、guard band、CPU 側の record（2.3 ms）の短縮 | p007 | 実機が戻るまで止め（2026-10-30 Q1: RX 5600 XT が抜かれている） |
| [p010](phase010/phase.md) | NVIDIA（RTX 2070）: nvrtx の起動、desktop の計測と 60 fps、Linux の NVK の基準 | 機械の入れ替え（2026-10-29 ユーザー）、P2 | cleared（2026-10-30 14:10 P7 q1712-i01: WS309 の後の main 5aca73738 で frame-ops 2 回、launch 63・64、type 61・62、output 68・65、drag 61・60、home 63・64、wiseview 65・60 fps、scroll・hover・keys は入力の間隔） |
| [p010a](phase010a/phase.md) | NVIDIA: nvrtx の executor に mview が要る物（depth・index の draw・instance・mip・uniform buffer・cull） | p010 | cleared（2026-10-29 10:48 q1670-i04: 実機で depth・texture が参照と一致、scene-5000 42.2 fps。残り: instance の rate の実機の個別の確かめ） |
| [p011](phase011/phase.md) | NVIDIA: Linux（NVK）で約 50 fps の scene（qs40 ×5000）での差の計測と 50 fps（p012〜p019 は改善の予備）。GPU の待ちを lock の外へ出す件（非同期の submit）は 2026-10-29 Q1 の判断でこの p011 で行う（WS250 は担当が無く止まっている。WS250 の design.md の期限の STOP・session ごとの worker と食い違わない形にし、WS250 はその上に作る） | p010a | cleared（2026-10-30 Q1 の判定: NVK と揺れの内で同等、文の「50 fps 以上」は未達（49.5〜49.7 fps）。2026-10-29: scene-5000 は 49.1〜49.5 fps で、同じ機械の NVK の単独の run 49.1・平均 50.1 と揺れの内で並ぶ。残りは GPU 19.5 ms、50 fps を明らかに超えるのは p012 以降の予備（vertex の program の質）。2026-10-30 q1693-i01: compiler の FFMA と operand の直読み（VS 108 → 59 命令）でも GPU の時間は不変、49.50・49.69 fps。固定機能で決まる見込み） |
| [p012](phase012/phase.md) | NVIDIA: scene-5000 を 50 fps を明らかに超える所へ（p011 の予備） | p011 | uncleared（2026-10-30 14:45 P7 q1712-i01: GPU の 19.4 ms は頂点の側の固定機能の hardware の限界で NVK と同等（続けての 6 回の平均 49.05 対 NVK 49.02）、cull=all・boost・VPC/VTG の method で不変、上限は約 51.3 fps。driver の変更なし） |
| [p020](phase020/phase.md) | PowerVR（VF2）: model の 50 fps（RDNA で確立した手順） | RDNA の手順の確立（p002 と最初の改善） | planned（2026-10-30 P7 q1712-i01: 道具と riscv64 の mview の直しまで。板に表示が無く（no boot framebuffer）mview が動かない、50 fps の基準は未決でユーザーに問い合わせ中。15:05 P3 q1715-i01: 元の kernel でも 10-28 以降の全起動でも framebuffer 無し、退行ではなく HDMI の接続の見込み、止め） |
| 最後 | 規約の全文の見直し | 実装の全部 | — |

- 2026-10-27 04:44 ユーザー（Q1 の問い「NGG の A〜C をどう進めますか？」へのクリック）「A〜C を今進める（推し）」。

- 再開点（2026-10-27 16:55 P2、q1623）: RDNA の次の改善の候補と計測は [rdna-next.md](rdna-next.md)（属性の後回し、BAR の WC の map、acquire の待ち、画素の側の小物の順）。
- WS306 との順（2026-10-27 ユーザー「WS306ですが、AMD RDNAドライバの最適化をWS291でやりきったあとに、要素の最適化をオプションに移して実装し直す作業という認識なので、両方のWSに記録してください。」）: RDNA の最適化はこの WS291 で最後までやりきる（NGG の culling などは今の形で入れる）。その後に [WS306](../ws306/ws.md) で、各最適化の要素をゲーム最適化の option に移して実装し直す。

## VF2（p020）の実機

- 2026-10-26 23:09 ユーザー「VF2はssh kei@10.0.30.2で、パスワード（略）です。VF2はSwitchbotで電源を入れられます。」→ VF2 は ssh kei@10.0.30.2（password は tree に書かない、Q1 が担当に渡す）。電源は SwitchBot のプラグ（AC276E42B252）で Q1 が `plan/tools/switchbot.py` で入れ切りする（担当は Q1 に頼む）。serial は host の /dev/ttyUSB0。
- 2026-10-26 23:09 ユーザー「VF2には現在、PowerVRドライバのカーネルやファームウェアが入っていないと思うので、SSH経由で更新が必要です。」→ p020 の最初に、main の config-visionfive2.mk（desktop の構成、PowerVR の driver あり）で vmunix と PowerVR の firmware（zedBSD-firmware から）・desktop の package を作り、ssh で VF2 の /boot/boot0/vmunix などを入れ替える（今の vmunix は WS288 の GHCI の kernel、P3 の ac7ee7d64 の物。入れ替える前の物は .old 等で残す）。起動の後に PowerVR の attach と firmware の load を確かめてから計測する。
- WS288 の作業で VF2 の zedbsd.cfg から login=graphical を外したまま（元は zedbsd.cfg.p044orig）。上の計測の決定で login=graphical は要らないので戻さない。

## 未決（その Phase の前に確かめる）

- （決まった 2026-10-29）NVIDIA の Linux の基準の driver: ユーザーの回答（クリック）「NVK（nouveau） (推し)」。GPU は RTX 2070（TU106）。
- PowerVR の 50 fps の基準: Linux との比較か、50 fps の絶対値か（VF2 の Linux の PowerVR の driver の有無で決まる）。

## 2026-10-28 の再開（q1635、P2）

- 2026-10-28 ユーザー「P2はRDNAマシンで作業します。RDNAの高速化をお願いします。まずNGGなどの高速化を取り入れてLinuxと同等以上の描画速度を達成して、そのあとで、ゲーム用最適化をオプションに分けていく段取りになっているので、確認してください。」
- 確かめ（Q1）: 段取りは記録のとおり。WS291 で RDNA の最適化（NGG の culling・compaction・属性の後回しなど）を今の形で入れて Linux 以上（scene-6500 で GPU 約 20 ms 以下、fps が Linux 以上）にし、その後に [WS306](../ws306/ws.md) で各要素をゲーム最適化の option に移して実装し直す。q1635 は p006 の続き → p008〜p009（[rdna-next.md](rdna-next.md) の順）。

- 再開点（2026-10-28 04:45 P2、q1635、worktree p2-ws291b）: 実装と実機の数は [phase006](phase006/phase.md) の「q1635 の続き」以下。scene-6500 は
  29.3 → 30.7 fps（WC で record −1.9 ms）、GPU 30.6 ms は動かず。次は phase006 の「残りの見立て」の (1)（我々の IB を RADV の culling の IB と全部突き合わせる）。
  道具: `plan/tools/gpu-perf/round.sh`（deploy・nggshape・shot・bench・hang）、`plan/ws291/tests/nowc-build.sh`（診断 patch を当てた tree の build）、
  build/ws291/linux-ibdump/ibdump.so（host で build、Debian の /tmp に置く）。機械: stick は vmunix-lat1 と ngg=2（cfg に診断の nggx=0 の行、無害）。

## 2026-10-28 自律の継続（q1639、P2）

- 2026-10-28 ユーザー「P2は自律的にループして、Linuxと同等以上の性能をゴールに、継続してください。hangが繰り返し起きるなら調査して修正してください。30分調査してわからなかった場合は、Fable 5.1 Highのサブエージェントを作成して、事情を説明した上で解決までどういう進め方をすればいいかのアドバイスを得て、さらに継続してください。どうしても解決できない場合、停止してユーザに判断を求めてください。これらはP2が自律的に行います。」

- 再開点（2026-10-28 07:55 P2、q1639、worktree p2-ws291c）: [phase006](phase006/phase.md) の「q1639」節。scene-6500 は 44.4〜45.7 fps（frame 22.2 ms、fence 20.5〜20.7、
  Linux 49.7〜50.2 fps・19.5〜19.7）。直した事: VRAM の PTE の fragment（eb0ec22fd、30.7 → 44.7 fps）、ring の空きの計算（d218e4dc7、hang の後の compute の EIO）、
  thermal fault で仕事を止め fan を 100 %（31129fc30・1b9cc7865）、SMU の電圧の feature（1b9cc7865）、guard.sh。次: (1) fence の約 1.3 ms の固定の遅れ（完了の通知の
  thread の起床 3 回、phase006 の見立て）、(2) 頂点の側の 0.8 ms（s_barrier の数、position の fetch）、(3) big-N・subs-N の CPU 側（executor の外の往復が大半）、
  (4) SMU が長い負荷で壊れないかを guard の下で見続ける（壊れたら mode1 の reset と shutdown の時の reset が候補）。長い試験は必ず round.sh か guard.sh の下で。
- 2026-10-28 ユーザー「P2は、USB NICが通信できない（DHCPが取得できないとか、ue0が異常状態）のとき、自分で再起動するように、serivceを登録すれば、とりあえず問題を回避して継続できると思いますので、P2の判断でこれを行ってよいです。」「P2は、5330と同じように、NVMeにzedBSDをインストールしてから作業を継続する方がいいような気がします。USBメモリが半導体供給不足で手に入りづらいからです。」 → P2 が NIC の監視の service（WS291 の image だけ）と、RDNA の機械の NVMe の zedBSD の区画への移行を行う（空きが無く ext4 を縮める時はユーザーに確かめる）。
- 2026-10-28 RDNA の機械の NVMe の区画: ユーザーの答え（クリック）「ext4 を縮め、root 32 GiB」→ live USB で Debian の ext4 を縮め、zedBSD の boot 256 MiB・root 32 GiB・swap 1 GiB（P2）。
- 2026-10-28 ユーザー「amdrdnaドライバが、初期化段階で画面に表示を行っていると思いますが、これをコメントアウトしてほしいです。…診断用の処理を関数に分けて、呼び出し元をコメントアウトして説明のコメントを残すのでもいいです。この作業のあと、次の作業を進めてください。」→ P2（q1645）。
- 2026-10-28 ユーザー「P2のテスト中に、OSの起動ロゴ画面がちらついて表示されています。GOPの内容でしょうか？もしかして、メッセージ表示のたびにコンソールを書き換えていたりしますか？GPU使用中はコンソール出力は完全に止めて、内容をアップデートしないようになっているかを確認していただき、さらに、GOPのバッファが一瞬ひょうじされてしまっている理由を解析し、修正してください。」→ P2（q1645）、案 D の前に。

## 2026-10-28 q1645（P2、worktree p2-ws291d）

- 承認: q1639 と同じユーザーの文に加え「P2は何度でもFableに聞いてください。」。ユーザーの指示（2026-10-28）: USB NIC が通信できない時に自分で再起動する
  service（nicwatch）、NVMe への zedBSD の移行（ext4 を縮め root 32 GiB）、amdrdna の初期化の画面の表示を診断用の関数に分けて呼び出しを comment に。
- 再開点（2026-10-28 10:40 P2）: [phase006](phase006/phase.md) の「q1645」節。scene-6500 は 45.4 → 49.2 fps（Linux 50.0）、GPU の時間（fence 18.9〜19.2 ms）は
  Linux（19.56）より短い。scene-1500 は 70 → 180〜184 fps（Linux 201）。直した事: Keiland の game mode の present を thread に（5f9ec7f08）、libvulkan の
  Wayland の acquire を fd で待つ（9b876b5fe）、mview --images と bench 4 枚（632d172dc）、memory clock（UCLK）を最上段に固定（146fdfe5d）、初期化の
  画面の表示の無効化（dfcaa14e5）、NVMe の区画と道具（6574b457c）。次: 残りの CPU 側（acquire・record・present）、big-N・subs-N の record・submit
  （phase006 の「残り」）。機械は NVMe の zedBSD（Boot0004）。
- 2026-10-28 ユーザーの補足「P2のモデル表示のテスト中に、起動ロゴ画面が見えてしまう、というバグです」（mview の表示の試験の間に起動の logo の画面が見える）。
- 2026-10-28 ユーザー「どこかの経路で、ゲームモードに切り替えるときにGOPのバッファが表示されてしまうのが仕方がないとしても、そのバッファはクリアして黒にしておいてほしいです。」→ display を takeover した後（GPU の使用中）は GOP の buffer を黒で塗り、その後 console が書かないようにする（新しい P2 の最初の作業）。
- 2026-10-28 ユーザー「P2のCPU時間の問題ですが、案Dがだめだったことと、状況を、Fable 5.1 Highのサブエージェントを作って伝えて、今後の作業方針について助言をもらってOKです。」→ q1651 で logo の件の後に。
- 2026-10-28 ユーザー「P2が自律的に助言をもらってOKです」→ Fable の助言は P2 の判断でいつでも。

## 2026-10-28 q1651（P2、worktree p2-ws291e）

- 再開点（2026-10-28 11:15 P2、13 時までの待機の指示で返却）: 起動の logo の件は直して実機で確かめた（[phase006](phase006/phase.md) の「q1651」節:
  GOP の buffer を黒に塗り、lease の間の切り替えは黒、console は 3 s の rest と shutdown の時だけ再開。logo3 で console の画面の黒でない pixel 0）。
  未実施は 3 s の rest の後の resume と shutdown の実機、ユーザーの目。CPU 時間は Fable 5.1 High に聞いた（同じ節）: 次は M2（present の copy を
  飛ばす試し）→ 案 E（libvulkan の Wayland の直の present、Q1 へ案）、executor だけの push の inline と execute_draw の state の差分。
  機械: NVMe の zedBSD（kernel vmunix-logo2 = この節の build、amdrdnactl に watch）で idle、guard は止めた。今日の P2 の SwitchBot の入れ直しは
  11:04:25 が 6 回目。

## 2026-10-28 q1653（P2、worktree p2-ws291f）

- 再開点（2026-10-28 16:45 P2）: [phase006](phase006/phase.md) の「q1653」節。commit: 8e4463127（push の領域・draw の state の差分）、b93e355ea
  （audit の 'mapped'）、b4897a17b（案 1 swapchain +1 枚）、d162d6700（案 2 直の encode）、6ac6d3754（表の再利用）、2ff3d0c73・e27a16d15・53213c001
  （measure.sh timing・cfg、soak.sh、guard.sh の dmesg）、b113329f3（dedicated-host の試験の直し）、141ce391d（案 3a Reset・Begin を head に）、
  142fd7b88（SMU: memory 側の feature の既定を off、hang の mark、実験の memory_allow）。
- 分かった事: bench の big-N・subs-N は cfg の amdrdna.vk.timing の上乗せ込みだった（外して取る）。SMU の idle の停止は memory 側の feature（UCLK 固定）
  が原因（対照 40 分 0 回、あり 16・20 分で停止）、固定の後も DCN の force allow を残すと 40 分 0 回。既定は安全のため off（UCLK 100 MHz）。
- （取り消し）(B) の長い soak は下のユーザーの決定でやめた。
- 2026-10-28 UCLK (B)（固定の後も DCN の p-state の force allow を残す）の 2 時間以上の soak: ユーザー（クリック）「夜間に流す (推し)」。それまで既定は off。
- 2026-10-28 ユーザー「P2は、負荷に応じて上げ下げさせる形、を取ってください。今夜のsoakは不要です。最短ではないものの、よりよい形に仕上げたいです。」 → UCLK は固定せず、DC の watermark・SMU の watermark table・NumOfDisplays・DPM_DCEFCLK・vblank の中の p-state の handshake で負荷に応じた DPM にする（P2、q1655）。(B) の夜間の soak は取り消し。

## 2026-10-28 q1655（P2、worktree p2-ws291g）

- ユーザーの決定（2026-10-28、Q1 経由、原文）「P2は、負荷に応じて上げ下げさせる形、を取ってください。今夜のsoakは不要です。最短ではないものの、
  よりよい形に仕上げたいです。」→ UCLK は固定せず Linux の DC の手順で PMFW に上げ下げさせる。受け入れは Linux の数以上（Q1）。
- 再開点（2026-10-28 19:50 P2）: [phase006](phase006/phase.md) の「q1655」節。commit: cf2118327（案 3b、Q1 の条件を全部通過: 実機・他 arch・
  dedicated-host・Venus）、c3a316269（venus-reg.sh）、3f4bec256・877809e6c（UCLK の設計とレビュー）、240ac2197（診断: amdrdnactl の clocks の行、
  status 352 → 384 byte、SMU 停止の mark に handshake、GetUMCFWWA）、fb8c0f844（memory_allow=2、未実機）。案 4 は patch で後回し
  （build/ws291/case4.patch、window mode の shot で SURFACE_LOST errno 24）。案 5 は未着手。
- 次: 表・NumOfDisplays・hard min・watermark の display の段の実装（負荷に応じた DPM）→ 受け入れの bench（Linux の数以上）。
  UCLK を固定した soak（増分 B）はしない（2026-10-28 20:06 ユーザー「UCLK固定のsoakはやらなくていいと伝えたはずですが。」「P2はsoakはやめさせて、実装に集中させてください。」）。
  機械は既定（memory=0）、kernel vmunix-d3、libvulkan 3b、amdrdnactl d2。

## 2026-10-28 q1658（P2、worktree p2-ws291h）

- ユーザーの決定（Q1 経由、原文）「P2はsoakはやめさせて、実装に集中させてください。」「P2が使っている性能評価スクリプトは、今後ほかのGPUのドライバの性能向上に標準で活用したいので、tools/以下のスクリプトとして保存するのが望ましいと思います。」
  「P2は、評価を中断して、画面へ出す回路のunderflowを先に修正してください。目視でも、かなりの割合で数秒に一回くらい、画面出力が途切れる瞬間があります。解決してから評価に進みます。UCLKの問題は解決とマークしていいです。」
- 結果: 負荷に応じた UCLK（amdrdna.smu.memory=2、既定は 0 のまま）を実装し SMU の停止 0（UCLK の件は解決）。計測の道具は plan/tools/gpu-perf/ へ（9213987d1）。
  display の underflow: p-state の plan を 404 us → 520 us にして 0（[phase006](phase006/phase.md) の「q1658」節）。
- 再開点（2026-10-28 23:01 P2）: 画面の途切れは underflow ではなく vkAcquireNextImageKHR の止まり（[phase006](phase006/phase.md) の「画面の途切れ」）。
  libvulkan の Wayland の待ちの取りこぼし（50 ms）は直した（7650291df）。残りは約 2 s 周期の 75〜97 ms の止まり（compositor の release か
  present worker の GPU 完了の待ちの見込み）。未実施: venus の回帰・他 arch の libvulkan の build。評価（bench）はこの後。
- 再開点（2026-10-28 23:40 P2、q1658-i02）: 約 2 s 周期の止まりの原因は Keiland の 2 s ごとの /dev/input の scan で、mouse の無い PS/2 の
  node（event4）の open が毎回 約 80 ms 待って EIO を返し main loop が止まっていた。直し b574fbf6a（開けなかった node を INPUT の event まで
  開き直さない）で window の HITCH 0、game は run の開始の direct scanout への切り替えの 1 回だけ。Keiland の protocol の順（frame done・release）は
  変えていない。venus-reg PASS、arm64・riscv64 の libvulkan と Keiland warning 0。評価: scene-6500 49.2 fps（Linux 50.1、fence は同じ 19.5 ms）、
  scene-1500 201.8（Linux 200.4）、big・subs は Linux の 1/3〜1/10（[phase006](phase006/phase.md) の「受け入れの評価」）。次は案 4・5 を Q1 と相談。
  別件（範囲外）: kernel の PS/2 の aux の node が mouse 無しで作られ open が 80 ms 待つ。

## 2026-10-29 q1663（P2、worktree p2-ws291j）

- ユーザーの回答（クリック、2026-10-29 01:3x）「両方 (推し)」→ 案 5（gpu.c の起床の抑制）を先に、続けて案 4（小さな transaction を呼び手の thread で同期の GPU_COMMAND、window mode の SURFACE_LOST errno 24 の解析を含む）。指示の原文「P2にWS291の続き」。
- 再開点（2026-10-29 02:15 P2）: 案 5（gpu.c の起床の抑制）を実装・実機・venus-reg PASS（[phase006](phase006/phase.md) の「q1663」節）。shot の SURFACE_LOST は main でも起き、keiland-shot の後に compositor が frame callback を送らない事による 10 s の watchdog（Keiland、P1）。案 4 も実装・実機・venus-reg PASS（同じ節）。次の候補は big-N の record（libvulkan の encode）と executor の draw の費用、subs-N の残り（1 submit 約 47 µs 対 Linux 10）。

## 2026-10-29 ユーザーの決定（WS306 との関係）

- 原文: 「まずはデフォルト有効で実装して、NGGはゲーム用に作り替えます。つまりWSに依存関係があります。」→ 案 4・5 は既定で有効のまま。NGG は WS306 でゲーム用の option に作り替える（WS306 が WS291 に依存）。
- 次の Queue: q1668（P2）big-N と subs-N の続き、RDNA で P1 の shot の直し（p019）の確かめ。ユーザーの回答（クリック）「WS291 の続き (推し)」。

## 2026-10-29 q1668（P2、worktree p2-ws291k）

- 承認: ユーザーの回答（クリック、2026-10-29）「WS291 の続き (推し)」。ユーザーの決定（2026-10-29）「まずはデフォルト有効で実装して、NGGはゲーム用に作り替えます。
  つまりWSに依存関係があります。」→ 案 4・5 は既定で有効、NGG の既定は変えず WS306 で option に作り替える（WS306 が WS291 の結果に依存）。
- 再開点（2026-10-29 04:15 P2）: [phase006](phase006/phase.md) の「q1668」節。P1 の keiland-shot の直しは RDNA で確かめた（shot 4 回とも DONE）。commit 8f8533e87・
  b3173b786・790aa60d1・bc65b40c0・1824111c8・1132ec5b6。big-1000 35.5 → 57〜59 fps（Linux 87）、subs-1000 1 submit 48 → 39 µs（Linux 10）、scene は不変。
  次: subs-N は GPU core の job の ioctl を 1 本にする設計（Q1 と）、big-N は kernel の decode と executor の draw、GPU の flush の barrier。

## 2026-10-29 ユーザーの決定（submit の ioctl）

- P2 の設計の案（plan/ws291/job-submit-design.md）へのユーザーの回答（クリック）: 案 A（GPU_JOB_SUBMIT）「採る (推し)」、案 B（GPU_JOB_RETIRE）「案 A と一緒に」。続けて原文「WS291: 足してみて効果のなかったioctlは、採用しなくてもいいです。」
- Q1: 番号は WS306 の予定（'G' 46〜48、GPU_CAP 131072）を避けて 'G' 49・50、GPU_CAP 262144・524288。outcome を driver が返す形は Q1 が OK。実装して実機で測り、効果が揺れを超えない ioctl は main に入れない。
## 2026-10-29 q1668-i02（P2、worktree p2-ws291l）

- ユーザーの回答（クリック、2026-10-29、Q1 経由）: 1 本の ioctl の案 A「採る (推し)」、案 B「案 A と一緒に」（[job-submit-design.md](job-submit-design.md)）。
  outcome を driver が返す形は Q1 の OK（技術の判断）。番号は WS306 の予定と重ならない 'G' 49・50、GPU_CAP 262144・524288（Q1）。
  進め方（Q1）: 今の commit を venus-reg と arm64・riscv64 の build で確かめて送る → 案 A・B の design を design-reviewer に通す → Q1 に送ってから実装
  （q1668 の続き、全 driver の build と venus-reg を merge の前に）。
- ユーザー（2026-10-29、Q1 経由）原文「WS291: 足してみて効果のなかったioctlは、採用しなくてもいいです。」→ 案 A・B は実装して実機で測り、効果が無い
  （測りの揺れの中）と分かった ioctl は main に入れない（branch に残し、測った値と判断を phase.md に）。効果の基準は測る前に phase.md に書く。
- 再開点（2026-10-29 05:05 P2、ユーザーの指示で計測を止めて返却）: [phase006](phase006/phase.md) の「q1668-i02」節の最後の再開点。
  libvulkan の private fence の直しで subs-1000 の 1 submit 約 3.5 µs 減（効果あり）、kernel の flush の barrier 無しと record の直の decode で
  big-1000 約 +2.5 %（効果あり、小さい）。案 A・B の design は reviewer 済み、実装は Q1 の OK の後。

## 2026-10-29 RX 5600 XT を外した後の P2（ユーザーの決定）

- 機械の変更（原文）「再起動後、RTL8127と、RTX 2070が接続されています。RX 5600 XTはPCIスロットの高さの関係で、RTL8127と排他だからです。」
- Q1 の問い「RX 5600 XT を外している間、P2 は何をしますか？」への回答（クリック）「WS291 の NVIDIA の段 (推し)」→ RTX 2070 で desktop 60 fps と model 50 fps（計画の順どおり）。RDNA の案 A・B の実装は実機の要らない所（host・QEMU）まで並べて進め、測るのは RX 5600 XT を挿し直した時。RDNA 機は時間の枠（plan/tools.md、ネットワーク優先）で P1（WS235 の RTL8127）と分ける。

## 2026-10-29 q1670（P2、worktree p2-ws291m、NVIDIA の段）

- 承認: 上の節のユーザーの回答「WS291 の NVIDIA の段 (推し)」。Linux の基準: ユーザーの回答（クリック、Q1 経由）「NVK（nouveau） (推し)」→ 設定の file は変えず、
  計測の時だけ sysfs で vfio-pci から外して nouveau（GSP）を読み込み、終わったら vfio-pci に戻す。
- Q1 の判断: nvrtx の BAR1 の window の満杯（device-local も常に map）は NVIDIA の段の目標を塞ぐので WS291 の中で直す（形は host-visible で要る物だけ map、
  前後の使用量を記録、WS250 の design.md の前提と食い違わない形）。
- 再開点（2026-10-29 07:20 P2）: [phase010](phase010/phase.md) の「再開点」。BAR1 の window の直し（06cdbf381）で window の peak 246 → 107 MiB、
  terminal が開き frame-ops の全段が動く（scroll・hover・drag は 20〜35 fps）。次の壁: work channel の VA（約 383 MiB）の満杯と、executor に depth の形式が無い
  （mview が動かない）。NVK の基準: scene-5000 50.1 fps（weston は render-only、direct scanout でない）。

## 2026-10-29 q1670-i02（P2、worktree p2-ws291n）

- 承認は q1670 と同じ（ユーザーの回答「WS291 の NVIDIA の段 (推し)」、Linux の基準「NVK（nouveau） (推し)」）。Q1 の判断: VA の直しと p010a は WS291 の中、その後 p011。
- 再開点（2026-10-29 08:57 P2）: VA の拡張は実機で OK（[phase010](phase010/phase.md) の最後の節）。p010a の実装（depth・DrawIndexed・cull・UNIFORM_BUFFER・mip（tiled の texture）・
  CopyBuffer）は host で確かめた（[phase010a](phase010a/phase.md)）、実機は次の枠。移設の後の「zedBSD が出ない」は新しい worktree の鍵の mode（0664）だった。

## 2026-10-29 q1670-i04（P2、worktree p2-ws291p）

- 承認は q1670 と同じ。再開点（10:46 P2）: depth は実機で動く（mview copies 1 が Linux の参照と 2 画素差、[p010a](phase010a/phase.md) の q1670-i04 の節）。
  前回の「全体が消える」は、mailbox の mview の後の mview の window が出ない別の件だった。scene-5000 は 42.2 fps（GPU は 1 frame に約 19.6 ms）。
  p010a は cleared（Q1 の許可）。次は p011: vkQueueSubmit を GPU を待たない形にする（amdrdna の手本、WS250 の design.md の期限の STOP・session ごとの worker と食い違わない形）。

## 2026-10-29 q1670-i05（P2、worktree p2-ws291q）

- 承認は q1670 と同じ。Q1 の判断（2026-10-29）: GPU の待ちを lock の外へ出す件は p011 で行い、WS250 はその上に作る。hang の後に他の session が続く件は
  p011 の範囲の外（WS250 の取り消しと復帰）、p011 の受け入れは scene-5000 の fps（50）。
- 再開点（11:31 P2）: [phase011](phase011/phase.md) の「q1670-i05」節。非同期の submit（2c3453e93）と system memory の slot（fa978f0a0）で scene-5000 は
  42.24 → 45.3 fps（submit 21.6 → 0.16 ms）。残りは mview の record 1.36 ms（UC の user の map の見込み、339ddc909 で write-combine、実機は次の枠）と GPU の約 0.85 ms。
- 再開点（12:15 P2）: 339ddc909（blob の user の map を write-combine、record 1.36 → 0.08 ms）と de077be78（completer の起床の遅れ）で scene-5000 は
  49.1〜49.5 fps（NVK 50.1、単独の run 49.1）。frame のうち GPU 19.5 ms。50 以上を確かにするのは GPU の側（p012 以降の予備）。判定は Q1。

## 2026-10-30 q1693-i01（P5、worktree p5-ws291、NVIDIA の段）

- 承認: Queue q1693-i01（2026-10-30 ユーザー「…そのあと、デスクトップ機では、RTXドライバの最適化に進んでください。エージェントは再利用しても新規で立ててもOKです。」）。
  RDNA の案 4・5 と同じ形（native の sync command）を nvrtx に当てるのは Q1 の判断（WS291 の範囲の普通の技術の判断、ユーザーの回答「両方 (推し)」「まずはデフォルト有効で実装して…」に沿う）。
- 再開点（05:05 P5）: [phase011](phase011/phase.md) の「q1693-i01」節。compiler の FFMA（効果なし・merge 済み 374eb20c3）、nvrtx の bench（big-1000 1.32 fps 対 NVK 136.2、
  subs-1000 6.65 対 75.4）、host blob（e8f83ab8f、big-1000 → 20〜24 fps）、sync command（5a2c5f4ff）。dirty state（5ec449b99）と slot の続き（57f4c5a62）は効果なし。
  次は kernel の record の decode（1 MiB に 3.7 ms）と subs の 1 submit の kernel 66 µs の内訳。

- 続き（2026-10-30 05:10〜 P5、同じ Queue）: [phase011](phase011/phase.md) の「q1693-i01 の続き」節。commit 118a97a14（record をその場に読む）・b94178d5f（class の初期 state を 1 回、
  texture の handle と viewport の再利用）で big-1000 は 19.4 → 51.3 fps（perf 有りの診断の比較）。subs の GPU の主は swapchain の pitch-linear の image に depth 付きで描く時の
  shadow の全画面 copy（1 pass に 2 回）。Q1 の判断（2026-10-30）で (A) LOAD の copy の省略（CPU が触れていない・export されていない blob、GPU 全体の submit の数で他の書き手を検出）を入れた（070da1b1c）。
  perf 無しの A/B: big-1000 23.8 → 52.4 fps、subs-100 67 → 104、subs-1000 6.7 → 10.9、scene 不変。回帰（shot・hang・desktop・subs の still）は phase011。
- WS309 p015 への候補（2026-10-30 Q1）: (B) WSI の image を block-linear にし scanout も block-linear にすれば、nvrtx の shadow の copy（pass の終わりの 1 回）も無くなる。
  WS291 の範囲の外。WS309 p015（compositor が game mode の image を貸す時の image の形式と tiling の選び方）の候補として残す。

## 2026-10-30 q1700-i01（P5、worktree p5-ws291n、NVIDIA の段）

- 承認: ユーザー（2026-10-30 原文）「WS291はNVIDIAですか？それなら今実行してOKです。」。範囲: p010 の desktop の測り直し（WS309 の後）、p012 以降で scene-5000 を 50 fps を明らかに超える所へ。
- 再開点（09:13 P5、利用の上限で Q1 がラップアップ）: 実機の計測は未実施。main の kernel の build（build/ws291/vmunix-m0、warning 0）まで。
  次の 1 手と手順は [phase010](phase010/phase.md) の「q1700-i01」節（先に main の Keiland・libvulkan・libc を NVMe の zedBSD に入れてから rtx-desk-profile.sh、
  その後 p012 を立てて `--cull=all`・`--size=` で GPU の内訳を NVK と比べる）。機械は Debian・RTX は vfio-pci、NVMe の kernel は触っていない、lock は返した。

## 2026-10-30 q1712-i01（P7、worktree p7-ws291、NVIDIA の段）

- 承認: ユーザー（2026-10-30 原文）「WS291をP7で実行します。P7はこのWSが終わったら返却してください。」
- 結果: p010 cleared（WS309 の後の main 5aca73738 で frame-ops 2 回、frame で決まる段は全部 60 fps 以上）。p012 uncleared（頂点の側の固定機能の hardware の限界、
  NVK と同等。Q1 の判断「(A) の時は、p012 を uncleared … として記録し、ws.md の残りへ進んでください。」）。道具: `plan/ws291/tests/rtx-desk-main.sh`・
  `rtx-scene-main.sh`・`rtx-thermal.sh`・`rtx-files.sh`（NVMe の kernel を読み戻し、main の desktop の program を入れて測り、終わりに戻して照合）。
- Q1 の指示（2026-10-30）: 「RDNA の p006・p008・p009 は、RX 5600 XT が抜かれていて実機が無いので、今回は進めません。ws.md に「実機が戻るまで止め」と記録してください。」→ p006・p008・p009 は実機（RX 5600 XT）が戻るまで止め。規約の見直しは RDNA の後。
- p020（15:00 P7）: VF2 で main の kernel は起動し PowerVR の render node は ok だが、板に表示が無く（`powervr: no display: no boot framebuffer`）compositor・mview が動かない。HDMI の画面をつなぐ人の作業と、50 fps の基準の決定（Q1 がユーザーに問い合わせ中）を待つ。[phase020](phase020/phase.md)。
- p020（2026-10-30 15:05 P3、q1715-i01）: 板の元の kernel（2825517684）でも、板に残る 10-28 19:47 以降の起動の記録 32 個でも全部 `no boot framebuffer (6)` → main の kernel の退行ではなく、U-Boot の GOP が無い（HDMI に画面が無い見込み）。物理の接続を Q1 に報告して止めた。画面なしで測る案（display の node を own mode の画面で attach、driver の側）は Q1 の判断待ち。[phase020](phase020/phase.md)。
- WS291 は incomplete のまま返却（Q1 の指示「p020 が終わったら（または進めなくなったら）、規約の見直しは RDNA の後に回し、WS291 は incomplete のまま返却してください。」）。
- 再開点（14:45 P7）: NVIDIA の段の残りは無い。ws.md の残りは p008〜p009（RDNA、RX 5600 XT を挿し直すまで実機が無い）、p020（PowerVR、VF2 の割り当てが要る）、最後の規約の見直し。

## PowerVR（p020）の性能目標（2026-10-30）

ユーザー（原文）「WS291 PowerVRの性能目標は、RTX 2070とIMG B XHE-32-4 MC1の性能比率から立ててください。」

- 基準の scene: RTX 2070 の scene-5000（`mview --content=game --present=mailbox --copies=5000`、qs40（37000 三角形）× 5000 = 1 frame 約 1.85 億三角形、1920x1080、vertex shading）で NVK・zedBSD とも約 49〜50 fps。
- 性能比（Q1 の案、理論の FP32）: RTX 2070 は 2304 core × 1620 MHz × 2 = 約 7.46 TFLOPS。VF2 の IMG BXE-4-32 MC1 は 32 FLOPs/clock × 600 MHz = 19.2 GFLOPS（既定の 400 MHz なら 12.8 GFLOPS）。比は約 389 倍（400 MHz なら約 583 倍）。参考に画素の fill は 64 ROP × 1620 MHz = 103.7 Gpix/s 対 4 pixel/clock × 600 MHz = 2.4 Gpix/s で約 43 倍。三角形の処理の速さの公表値は見つからない。
- 目標（Q1 の案）: 同じ 1920x1080・qs40 で copies を比で割った scene、600 MHz なら copies 13（5000 ÷ 389、約 48 万三角形）、400 MHz なら copies 9（5000 ÷ 583）で 50 fps 以上。VF2 の GPU の clock は P3 が確かめる。

- 2026-10-30 ユーザー（原文）「PowerVRはLinuxではドライバがぜんぜんダメで、実用水準ではないです。Linuxで、の定義にこだわらなくていいです。指示通り、LinuxでのRTX 2070での50fpsとのきのモデル規模と、RTX 2070とのカタログスペック比で、50fpsになるべきモデル規模を決定してください。」 → 決定（Q1）: 基準は Linux（NVK）の RTX 2070 の scene-5000（qs40 × 5000、1920x1080、50.1 fps）。カタログの FP32 の比（RTX 2070 約 7.46 TFLOPS 対 VF2 の BXE-4-32 MC1 の 594 MHz（P3 が dmesg で確かめた）で 32 × 594 MHz = 19.0 GFLOPS、約 392 倍）で割り、**qs40 × 13（copies 13、1920x1080）で 50 fps 以上**を p020 の受け入れにする。
