<!-- awesome-plan project=zedbsd record=ws303 -->

# WS303 熱と fan（温度の読み・fan の回転数・thermal mode）

- Master: [plan/master.md](../master.md)（2026-10-27 09:39 作成）。Primary Milestone: MG004。関係: [WS301](../ws301/ws.md)（DVFS、熱の上限の口は WS301 p002）、WS039 p012（amdrdna の SMU の fan）。
- Status: canceled（2026-10-31 ユーザーの指示で WS333 に統合。/dev/thermal で統一して設計し直すため。この WS の設計は WS333 の入力）
- 出典: 2026-10-27 ユーザー「DVFSとP-core/E-coreは、/dev/systemに入れるのが適切ならそれでもいいですとお伝えください。ファン制御もあるといいですね。」、（クリック）fan は WS301 とは別の WS。

## 単一の到達目標

5330 を最初の対象に、温度の読み、fan の回転数の読み、DVFS の strategy（thermal profile）の選択、fan を上げる段の指定、非常の水準での停止を、`/dev/thermal` の口と `/sbin/thermal` で提供する（2026-10-29 ユーザーの回答 Q303-1・2・5 で改めた。旧: /dev/system の口）。5330 は EC が fan を回すので、OS からは回転数の読み、Dell の thermal mode への strategy の伝達、（確かめられれば）SMM の fan の state での上げになる。

## Phase

設計: [design.md](design.md)（**第 5 版**、2026-10-29 P5、q1666-i03、一つの本文にまとめた）。review: [design-review.md](design-review.md)（第 1 版）・[design-v3-review.md](design-v3-review.md)（第 3 版）・[design-v4-review.md](design-v4-review.md)（第 4 版）。Phase の表は design.md §7 が本体（下は写し）。

| Phase | 内容 | 依存 | LW | 状態 |
| --- | --- | --- | --- | --- |
| p001 | 調査と設計、design-reviewer | — | 残り 1 | in-progress（第 5 版。Q303-15〜19 待ち、第 5 版の review は未実施） |
| p002 | ACPI WMI の層、harness の移しと拡張 | p001 | 6 | planning |
| p003a | registry・`/dev/thermal`・UAPI の案（ユーザーの review）・`/sbin/thermal`・`hw.thermal` を同じ写しから | p001 | 6 | planning |
| p003b | 見張りの thread と非常の停止（init の SIGUSR2・直の切り）・試験の注入 | p003a | 5 | planning |
| p004 | Dell の provider と 5330 の試験 | p002、p003a | 7 | planning |
| p005 | sessiond の THERMAL の要求 | p003a | 2 | planning |
| p006 | Pi 5 の provider | p003a、p003b | 3 | planning |
| p007 | passive（WS301 の上限） | p003b、WS301 p002・p004 | 2 | planning |
| p009 | monitor と sysctl の道具を `/dev/thermal` へ、`hw.thermal` を外す | p003a | 2 | planning |
| p008 | 規約の全文と文書 | p004〜p007、p009 | 2 | planning |

合計 約 35 LW。WS301 の strategy の受け手は WS301 p004 が作る。

## ユーザーの回答（2026-10-29、Q1 の中継、原文）

- Q303-1「/dev/thermal」、Q303-2「rootだけが変えられるが、sessiond経由でも変えられる。/sbin/thermalからDVFSストラテジを選択。強制的にファンを大きくする（何段階かあり）ことはできるが、実際の温度に対して必要なファン回転数を下回らせるオーバーライドは不能。」、Q303-3「さっき書いた/sbin/thermal」、Q303-4「profile を作る」、
  Q303-5「温度に対して必要な回転数を下回らせることはできず、回転を上げる。非常水準に達したとき、電源を落とす。コントロールできないときはファームウェアにまかせたり、ストラテジだけファームウェアに伝える。」、Q303-6「入れる」、Q303-7「Debian で読む (推し)」、Q303-8「よい (推し)」。
- 解釈（P5、design.md の「第 3 版の改訂」）: UAPI は新しい `/dev/thermal`。変更は root と sessiond 経由。`/sbin/thermal` が strategy を選ぶ。fan は上げる段だけ。非常の水準で停止。passive を入れるので WS301 に依存する。確かめの問い Q303-9〜14 を聞き直し中。
- Q1 の決定（2026-10-29）: AML の harness は p002 で `plan/tools/aml-host/` に移す。`/dev/acpi` は BUG-351。
- ユーザーの回答の第 2 回（2026-10-29、Q1 の中継の原文と解釈は design.md の「第 4 版への追記」）: Q303-9/10「/dev/thermal に移す・root/wheel」、Q303-11「ストラテジーはかなり抽象化されたものとして、トータルの「performance or balanced or power save or manual」と、マニュアルなら実行速度、騒音、細粒度パワーゲートの3項目を0-100で設定可能に。具体的なプラットフォームへのマッピングはドライバが決める。」、Q303-12「GPL の値を出典つきで (推し)」、Q303-13/14「推しの組 (推し)」。残りの問い Q303-15〜17。


## 再開点（2026-10-29 P5、q1666-i02、古い。下の i03 が新しい）

- 設計の第 4 版（ユーザーの回答 Q303-1〜8 と第 3 版の review を反映）。聞き直し Q303-9〜15（design.md の第 4 版の問いが最新）の答えを待つ。p003 は 8 LW、合計 約 30 LW（design.md の第 4 版の変更）。
- 5330 の Debian の読みの記録（2026-10-29 02:42、読みだけ）は scratchpad にあり git に入れていない。要る値は design.md §2.3 に写した。

## 再開点（2026-10-29 P5、q1666-i03）

- 第 4 版の review（[design-v4-review.md](design-v4-review.md)、blocker 1・major 11・minor 16）を反映した第 5 版（一つの本文）を書いた。行き先は design.md §11。第 5 版の design-reviewer は未実施。問いは Q303-15〜19（design.md §9、ユーザー向けの説明は [user-questions.md](user-questions.md)）。

## 再開点（2026-10-29 P5、q1666-i04）

- Q1 の答え（Q303-9 は選択肢の原文、GPL の手順は採らない）を design.md §0・§8・§10 に反映（184a42bb0）。第 5 版の design-reviewer の結果は [design-v5-review.md](design-v5-review.md)（blocker 0・major 9・minor 25、2026-10-29）。**design.md に未反映**（ラップアップ）。次: 第 6 版で M1〜M9・m1〜m25 を反映（M2 の上限の口の引数と apply の戻りは WS301 と揃える、M9 は SMM の失敗の印を値として扱ってよいかを Q1 に確かめる）、その後に design-reviewer。問い Q303-15〜19 は [user-questions.md](user-questions.md) のまま未回答。
