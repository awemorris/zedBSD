<!-- awesome-plan project=zedbsd record=ws009-p010 -->

# ws009-p010: DOC-54 gpu.md: node・権限・呼び方・誤り・並行と待ち・索引・互換

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: なし（骨格 5fedf634d と照合の script が main にあること）。p010〜p021 は互いに独立で、並べて起動してよい
骨格の基点: 5fedf634d

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- src/drivers/gpu/gpu.c: `gpu_publish_node`・`drv_gpu_register`（番号の割り当て）・`gpu_open`・`gpu_close`・`gpu_recover_open`・`gpu_ioctl`・`gpu_ioctl_body`・`gpu_session_enter`・`gpu_session_leave`・`gpu_poll`・`gpu_display_ungated`、各 handler の先頭（readable・writable・capability の照合。`grep -n 'readable\|writable' src/drivers/gpu/gpu.c`）
- src/drivers/gpu/gpu-fence.c の poll（並行と待ちの節の (5) のため）
- userland/desktop/sessiond/seat.c（node の mode、seat の user に渡す node）
- include/uapi/ioctl.h・include/uapi/hosted.h・include/uapi/errno.h

## 2. 作る物

- `docs/reference/gpu.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・`Errors:`・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: gpu-admission・gpu-recovery（open・権限・回復）。

## 4. 注意

- 索引の Open mode と Capability の列（46 行）は、各 handler を読んで決める。他の file の `Requires:` 行は別の Phase が別に決め、script が比べる。食い違いは統合の Phase が直す。
- 並行と待ちの表は、待つ要求ごとに header の comment と code の両方を読む（display の待ちの 1 秒の上限、`GPU_JOB_CAPACITY` の WAIT は flags 0）。
- 読む量が多い（gpu.c の約 1,500 行）。関数名で辿り、全文を読まない。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d --final docs/reference/gpu.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu.md plan/ws009/phase010/phase.md plan/ws009/ws.md`。

## 結果

（未実施）
