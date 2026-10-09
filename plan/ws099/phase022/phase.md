<!-- awesome-plan project=zedbsd record=ws099-p022 -->

# ws099-p022: WS099 の全文規約と回帰（WS の最後）

Status: planning（最後の Phase。p019〜p021 と p012 の結果の後に planned へ）
Disposition: normal
Parent: [WS099](../ws.md)
Queue: なし
依存: p019・p020・p021 cleared、p012 の結果（実機の C1）。C6 の扱いのユーザーの判断（p006）
目安: 3h（1 Queue）。実行者の目安: phase-runner（high）
所有 path: WS099 で変えた source（`userland/desktop/wayland/`・`userland/desktop/sessiond/`・greeter の該当の所）、`plan/ws099/`

## 範囲

Awesome Plan §6 の code を作る WS の最後の conformance。[guide.md](../guide.md) の「提案 p018」をこの番号で正式にした。

1. WS099 の commit（p002・p005・p007・p009・p010・p011・p015・p016・p019・p020・p021）の変えた file の一覧を `git log` から作る。
2. [coding-style.md](../../coding-style.md) の全文で見直し、`plan/tools/style-check.py` と範囲の中の違反を直す。新しい機能は足さない。
3. build（warning 0）、`criteria.sh` の C1〜C5・C7〜C10（C10 は 1 時間）を同じ最終の image で、boot test。

## 受け入れ

- 範囲の中の style の違反 0（既存の例外は表に記録）、warning 0。
- `criteria.sh` の C1〜C5・C7〜C10 が全て PASS（ベータ1 の B5）。p076 を含む C9 が安定（BUG-125 resolved の後）。
- 実機の証拠（C1・C5・C10 の 5330）と QEMU の証拠を分けて記録し、C6 の値と扱い（ユーザーの判断）を書く。
- 満たせば WS099 の完了の判定を Q1 に依頼する（完了の書き直しは AGENTS.md の形）。

## 未決の判断

- C6（窓 10 個で中央値 50 ms、今 56〜59 ms）を WS099 の完了の条件に残すか（2026-09-30 の決定「10/10 に見直す」、ユーザー）。

## Event

2026-10-02 / ws099-beta1-plan-p022: fg019 の計画で新設（guide の提案 p018 を正式化）。

## T1-477 の回帰の FAIL の解析（2026-10-09 深夜、P1）

T1-477（image の tree 0d39d675e）: C4・C8・C9 PASS、C1 p126・C2・C3 c3-swipe-back・C5・C7 FAIL。出力 /home/awe/zedBSD-worktrees/t1/build/t1-477/out/。分けた結果（製品の回帰は見つからない）:

| 試験 | 原因 | 区分 | 直し |
| --- | --- | --- | --- |
| C2 maximize | docked の幅 1904 at x=8 を「1920 と等しい」で見ていた。ws099-p038 の `KWL_GLASS_DOCK_PAD`（8）の余白は設計どおり（maximized.png） | 試験 | 幅＋2×x が画面の幅、x は 0〜16 |
| C2 unminimize の後の move | surface の番号は client ごと。Welcome の Settings（client 2）と Files（client 3）が同じ surface=19 で、Wiseview の tile の行の最後（Welcome の物）を押した（`WISEVIEW select surface=19 client=2`、unminimized.png は Welcome） | 試験（Welcome、ws164-p002 が入った後） | launch の行の client で tile・select・CONFIGURE を引く |
| C3 c3-swipe-back の段 6 | zdesktop.log の `unfullscreen-swipe start` は段 2・4 の 2 行だけで、段 6 で増えていない（製品は正しい）。段 6 の前の count が数でない答え（SSH の時間切れで空、`found 2 (more than 0)`）だった見込み | 試験の harness（推定） | count が数でない答えを 3 回まで聞き直し、stderr に harness の行 |
| C5 | 1 回目の開閉だけ first・gap が約 200 ms、2・3 回目は 94〜96 ms・gap 126〜135 ms。T1-006 と同じ形で、2026-10-03 ユーザー「C5の200msは問題視しません。clearでOKです。」 | 既知（判断済み） | c5-parse.py は種類ごとの最初の開閉を warm-up として記録し（first_max・gap_max に入る）、frame が 1 枚も無い時だけ FAIL。T1-477 の log で `pass=12 fail=0`（host） |
| C7 | 全 70 の FAIL は Settings・Files の窓が無い画面の測定（Aurora-settings.png は壁紙だけ）。`$ends; … /bin/settings …` の guest の shell の行が BUG-274 の後の `ps -o args` で `[s]ettings` に当たり、shell が自分を kill して Settings・Files を起動しなかった | 試験（ps の args の変更） | `ps -o pid,comm`（argv[0]）で `(^|/)(wayland|popup-probe|settings|files)$` を終える（fake の ps で host 確認） |
| C1 p126 | 最初の boot の自動 login の session が 30 秒以内に READY を出さず（`HANDOFF session ready=0 waited_ms=30001`、GO 無し）、`HANDOFF go written=3` の最初の待ちが MISSING。その後の 2 回の login・logout は黒 0・文字 0。同じ image の C9 の p126 は `ready=1 waited_ms=1485` で PASS | 未再現（1 回だけ） | 直し無し。T1 の再試験で C1 を見る。再現したら gdbstub で session の compositor の READY の前を止めて見る（推定: image の最初の起動で host の Venus の pipeline の compile が冷えていた） |

注: criteria.sh の C1 と C9 の p126 はどちらも `$out/p126.log` に書いていて、C1 の log は C9 の物で上書きされた（results.txt と c1/ の出力が C1 の証拠）。C1 の名前を `c1-p126` に変えた。
確かめ: `sh -n`（c2・c3・c7）、`c5-parse.py` を T1-477 の log に流して `C5 RESULT pass=12 fail=0`。QEMU は T1。

## T1-504 の C7 の残り 7 の解析（2026-10-10、P1）

T1-504（image の tree b07e745d6）: C1・C2・C3・C5 PASS、C7 `pass=77 fail=7`。7 つの FAIL は全て s-section（Settings の Home の群の見出し「Connectivity」）で、7 枚の壁紙の全部で `text≈glass`（contrast 1.00〜1.02）、つまり測る箱に文字が無い。
Settings の窓の PNG（c7/*-settings.png）では見出しの墨は x 340〜418・y 207〜217 にあり、箱（340 216 436 233、T1-169 の 2026-10-05 の測り）は見出しの下の空白だった。
見出しが 12 px 上がったのは ws090-p023（2026-10-06、`se_page_header` を libkeiland の `kl_header` に）の後の設計どおりの配置で、画面（タイトル・説明・見出し・tile）は正しい。区分は**試験**（箱の位置が古い）。製品の直しは無し。

直し: `plan/ws099/tests/c7-contrast.sh` の s-section を `340 204 436 222` に。確かめ（host）: `c7-contrast.py` を T1-504 の 7 枚の settings の PNG に流して contrast 4.90〜5.81（全て ≥ 4.5）。`sh -n` ok。QEMU の再試験は T1（C7 だけでよい）。
