<!-- awesome-plan project=zedbsd record=ws329-p023 -->

# ws329-p023 品質と適合の行列

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-31 p001 で作成）。
- 依存: p022。
- 担当の想定: Opus 5.5（phase-runner）。

## 目的と範囲

- design §10.2 の全組（preset 3・rate control 3・大きさ 1280x720 の短い物・forced key・drain・QP 0 と 51・I_PCM が起きる低い QP・cabac_zero_word が要る組）を `roundtrip.sh full` に。
- 画質: libx264 との BD-rate（4 点、4 本の試料）を出す script `plan/ws329/tests/quality.sh`（最後の行に数値と目標の合否）。目標に届かなければ、仕様に無い判断（mode の選び・量子化の丸め）の調整を design.md に記録して行う（header の数値の変更は Q1 と相談）。
- JM の参照 decoder を Q1 が許可すれば、その oracle を roundtrip に足す。

## 受け入れ

- §9 の正しさの門が全組で PASS。
- 画質の目標（Q329-1 の答え）。
- 結果の数値（BD-rate、fps の目安）を design.md と ws.md に。

## 変えてはいけない物

- decoder の `.c`・header。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- `timeout 7200 sh plan/ws329/tests/roundtrip.sh full`（background で時間の上限付き）
- `timeout 7200 sh plan/ws329/tests/quality.sh`
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。
