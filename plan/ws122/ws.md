<!-- awesome-plan project=zedbsd record=ws122 -->

# WS122: 動画プレーヤアプリ

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 q910 P2 の照合: p001〜p005 は全部 cleared。残りは規約（ベータ3）と別 WS（GPU の decode＝WS083）。WS の完了を Q1 が判定）（2026-10-07 q831 P2: p001・p002・p004 cleared、p003・p005 は Q1 の判定待ち、残りは全文規約の Phase と別 WS（GPU decode＝WS083、独自 AAC））
Primary Milestone: MG006
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1）
Queue: q831（P2）
Resume point: p003・p005 の判定（Q1）、その後は全文規約の Phase（WS177 の後）。
2026-10-02 user: 動画関連（WS083・WS121・WS122）は**別セッション**でユーザーがノウハウを提供しステップバイステップで進める。このセッション（Q1/P1〜P8）は割り当てない。ベータ1 では最悪 drop してもリリース可能とする（努力目標）。VA-API（WS123）は canceled、アプリが Vulkan Video を直接使う。ブラウザへの組み込み（WS121）は Codex 側と調整。
2026-10-05 user（方針の改訂、原文）:「動画プレイヤーは、H.264の動画再生支援を実装してからにしようと思っていましたが、userland/packages/multimedia/libavcodec/を追加して、今すぐ着手できるように思います。ベータ2では、libavcodecを不要にして、独自のコンテナ読み込みライブラリと、動画再生支援のみを使うようにするほか、libavcodecが存在すれば、利用できるという、ダイナミックリンクのアドインにします。コンパイル時にヘッダもいらなくて、純粋にdlopenするなら、ライセンス的にも問題ないと思います。」→ このセッションで今すぐ着手できる（2026-10-02 の「別セッション」の扱いをこの指示で置き換える。WS083・WS121 は別のまま）。段は下の「2026-10-05 の計画」。
<!-- awesome-plan-current:end -->

## 2026-10-05 の計画（ユーザーの方針）

1. **今（ベータ1 の後すぐ）**: 外部 package `userland/packages/multimedia/libavcodec/`（FFmpeg の libavcodec・libavformat・libavutil・libswscale・libswresample の必要な分。tarball の取得と検証と patch、Guardrail の外部 package の規則）を足し、動画 player の app をそれで作る。**license の監査**: FFmpeg を LGPL の構成で build し、GPL・nonfree の部品（`--enable-gpl` の codec・filter）を入れない。configure の option と出来た library の license の表を記録する（`plan/tools/packages/audit-licenses.sh`）。
2. **ベータ2**: player の既定の経路から libavcodec を外し、**独自の container の読み込みの library**（MP4・MKV などの demux、Zlib の独自実装）と **動画の再生の支援**（GPU の decode、[WS083](../ws083/ws.md) の Vulkan Video と i915）だけで再生する。
3. **ベータ2 の add-in**: libavcodec が system にあれば使える、**dynamic link の add-in**。build の時に libavcodec の header を使わず、実行の時に `dlopen`・`dlsym` だけで呼ぶ（ユーザーの見立て:「コンパイル時にヘッダもいらなくて、純粋にdlopenするなら、ライセンス的にも問題ない」）。呼ぶ関数の宣言は自分で書く（ABI の版の確かめ、無い時は add-in を使わない）。この方式の license の扱い（LGPL の library を dlopen で使う app の義務、header を使わない宣言の独自の記述）は p001 で確かめて記録する。
4. 段 1 の package を image の既定に入れるか → **2026-10-05 未明ユーザーの決定: ベータ1 から image に入れる**。あわせて「ベータ1 に簡単な player を入れる」（libavcodec で再生する最小の player: 開く・再生・停止・シーク、10/13 の RC まで）。LGPL の義務は release の license の一覧と notes で果たす（ws129）。


## 目標（2026-10-02 ユーザー（ベータ1、リリース目標 10/17））

「動画プレーヤアプリを追加する。」

- Keiland の動画プレーヤ。Vulkan Video（WS083）を直接使いで H.264 を decode し、表示・音声・seek を行う。

## Phase

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 要件・設計（2026-10-05 の計画の段 1〜4、libavcodec の package の license の構成と dlopen の add-in の扱いを含む） | cleared（2026-10-05 Q1） | — |
| [p002](phase002/phase.md) | 簡単な player（開く・再生・一時停止・停止・シーク、audiod の音）、ベータ1 | cleared（2026-10-05 Q1） | p001 |
| [p003](phase003/phase.md) | 独自の container の読み込み（MP4・Matroska/WebM の demux）、ベータ2 の段 2 の前半 | cleared（2026-10-07） | p002 |
| [p004](phase004/phase.md) | player を mediafile と libavcodec の dlopen の add-in（software decode、header 無し）で完成（2026-10-05 夕のユーザーの決定。GPU の decode は別の WS） | cleared（2026-10-05 Q1、T1-191） | p003 |
| 最後 | 全文規約と回帰 | planning | 実装 Phase |

## 2026-10-06 UAT のフィードバック

- mp4 の再生と frame の落ち無しを実機で確認（ユーザー）。BUG-223 F11・Alt+Enter の全画面（direct scanout）

## Phase（2026-10-06 追加: 再設計）

- [ws122-p005](phase005/phase.md) 設計と実装: 動画の全画面と直接の scanout（test-wait: p005a T1-237・p005b T1-244 済み、実機は UAT、判定は Q1）
