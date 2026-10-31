<!-- awesome-plan project=zedbsd record=ws334-p034 -->

# ws334-p034 試験: libbrowser の試験を Windows（Wine）で

- 親: [WS334](../ws.md)、設計 [design.md](../design.md) の §7.3 の「既存の libbrowser の試験のうち Windows で流す物」、O28・O29。
- 状態: planned（2026-11-01 p003 で作成）。
- 依存: p032。
- 担当の想定: Opus（phase-runner）を 1 回（約 5 LW）。WS074 の試験の file と runner は WS074 の物: 変えるなら差分を Q1 に（P8 と調整）。

## 中身

1. host-*.c（WS074 の 91 本前後。今の数を数える）を llvm-mingw で .exe にする script `plan/ws334/tests/host-tests-windows.sh`（`plan/ws074/tests/host-build.sh` の source の一覧と同じ物を `list-sources.sh` から、win32 の OS の層で）と Wine で流す。setenv(XDG_DATA_HOME) は p023 で要らなくなった物以外を記録。
2. `omnigrammaton-cli.exe` を program にして `run-js-tests.py`・`run-test262.py`（`fetch-suites.sh` の取得物、共有の場所を Q1 に確かめる）・`run-html5lib-tree.py`・`run-url-tests.py` を host の python から Wine の program で流す（runner が program の path を引数に取れない時は、runner を変えずに `wine` を前に付ける小さな wrapper の script を PATH に置く形を試す。それで足りなければ runner の変更の差分を Q1 に）。
3. `--render-gpu` の数枚を lavapipe（Wine）と Linux の lavapipe で比べる（`gpu-compare.py`）。
4. Interop 2025 の代表の 100 件（WS074 の一覧から、選び方を結果に書く）。
5. 差の表: 試験ごとに Linux の件数・Wine の件数・差と理由（O28 の strerror の文、時刻帯、path の形、Wine の不具合の疑い）。

## 受け入れ

- 件数の表が結果の節にある（PASS の基準は「差の全部に理由がある」。Windows の不具合と分かった物は Bug として Q1 に）。commit、ws.md の行。
