<!-- awesome-plan project=zedbsd record=ws329-p025 -->

# ws329-p025 速さ

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-31 p001 で作成）。
- 依存: p023、WS318 p008（thread の台帳）。
- 担当の想定: Opus 5.5（phase-runner）。実機は Q1 が割り当てる。

## 目的と範囲

- 計測の script（1 本: 配置 → 実行 → 回収 → 要約）で host・5330・Pi 5 の fps を preset と大きさごとに。
- 近道: 参照の面の作成の速さ、SAD・SATD の loop（自動の vector 化が効く形）、早い打ち切り、2 回の再構成の 1 回目を参照の面からの予測で置き換え（`h264sw_recon_mb` の 2 回目は残す: 一致の保証）、slice の並列（WS318 の `thread.c` の pool、名前の付いた thread と本数）。
- 近道の前後で全 roundtrip の MD5 が同じ（並列でも出力が同じ）。

## 受け入れ

- §9 の速さの目標（Q329-2 の答え）、roundtrip full が前後で同じ、run-unit all PASS。

## 変えてはいけない物

- 仕様の振る舞い（出力の bitstream は preset と config が同じなら近道の前後で同じ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- 計測の script の要約、`roundtrip.sh full`
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。
