<!-- awesome-plan project=zedbsd record=ws202-p001 -->

# ws202-p001: 設計（design.md）と review

Status: uncleared（software実装/対象host・buildの証拠あり、whole条件は未達）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 4 LW（第 1 版 3、第 2 版 1）
依存: —

## 現在の適用方針

[最新ユーザー決定](../policy-20261010.md)が以下の旧第2版手順に優先する。

## 範囲

[design.md](../design.md) を書く。実装・build はしない（2026-10-11 ユーザー「設計だけ書いてください。実装は別なセッションで行います。」）。

## 経過

- 2026-10-10 第 1 版: 今の形の事実、mp4（既存の `mediafile/mp4.c`）、Vulkan Video の H.264、AAC-LC、共通の部品、A/V、API、試験、license、判断の点 H1〜H6。
- 2026-10-10 design-reviewer の [review-001](../review-001.md)（e8adcdf89）: H 7・M 13・L 13 と判断の点 J1〜J6。
- 2026-10-10 第 2 版: H-01〜H-07・M-01〜M-13・L-01〜L-11・L-13 を反映（L-12 は Q1 が WS083 の側で直した）。対応は design §16。review の
  「確かめていないこと」のうち host で確かめられた物は design §15（E1〜E9）、確かめられない物は design §14（U1〜U15）。Phase を p015 で 1 つ足し、
  見積もりは 62 → 75 LW。

## 残り

1. H1〜H6・J1〜J6 のユーザーの回答を ws.md と design.md に記録し、推しと違う回答の Phase の変更（例 H1 (b) なら CPU の decoder の Phase を足す、J1 で
   container を絞るなら §3.1 と p010 の試験を縮める）。
2. 変更が大きければ 2 回目の review（Q1 の判断）。
3. WS を planned に、この Phase を cleared に（Q1 の判定）。


## 構造改訂と部分結果（2026-10-10）

p001は設計だけの旧制限を今回ユーザーの実装依頼で置換。native-only library/app fallback/API readbackへ構造改訂し再reviewが必要。旧clearance条件はそのまま実装承認に使わない。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

## Native再生software結果（2026-10-10）

Event: `ws202-native-playback-software-20261010-p001`。Queue: [codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)。

ユーザー決定のnative-only library/app fallbackと標準readbackを実装へ適用し、第4版referenceを保持した。共有design/レビューとの意味の統合とp016 ID衝突解消はQ1 pending。

[最終source/command/結果・限界](../playback-result-20261010.md)、[Q1統合](../handoff-20261010.md)、[T1の準備済み依頼](../t1-playback-request-20261010.md)。旧第2版の手順・昔のpartial outcomeを保存し、最新記録が未実装記述の現在状態を置換する。whole criteriaを満たしたとは扱わず、Q1の意味の統合と未実施matrix/実機結果が再開条件。main/共有投影/GitHubの更新はQ1 pending。
