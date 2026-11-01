<!-- awesome-plan project=zedbsd record=ws291-p028 -->

# ws291-p028 NVIDIA: kernel の組み置きの push buffer（B-K、design の P-B）

Status: planned（2026-11-01 21:07 Q1。依存: p027）

- WS: [WS291](../ws.md)。設計: [phase026/design.md](../phase026/design.md) の P-B（B-K）。ユーザーの (b) の本体。
- 範囲: design.md の P-B（heap、chunk と回収、End の組み置きと適格、ring の list、set の世代、組み置きの run の GPU 時間の数え、`nvrtx.vk.prebuild=1`、mview の `--record=once`・`--submit-all`、host 試験、実機）。
- 受け入れ: design.md の P-B の受け入れ（fps の倍率でなく、submit の kernel の時間と GPU の使用率）。P-C（eager End と capset の bit）は Future Work（Q1 の判断）。
