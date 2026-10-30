# /dev/gpu の UAPI の文書（DOC-54）の書き方の共通の規則

Parent: [WS009](ws.md)。p010〜p022 の phase.md が「読む物」に挙げる。設計は p009 を分けた物（[ws.md](ws.md) の Phase の表）。

標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）を文書の仕事に当てた形:
header の仕様の comment に当たる物が、骨格の各節の `<!-- spec: ... -->`。.c の骨格に当たる物が、PENDING の印の入った骨格の文書。
単体試験と骨格の照合に当たる物が `plan/ws009/tests/check-gpu-uapi-doc.py`。

## 1. 基点と正

- 骨格の基点の SHA: **5fedf634d**（`git cat-file -t 5fedf634d` は commit）。全ての書き手の Phase は、見出し（H1〜H4）がこの SHA から変わっていないことを `--base 5fedf634d` で照合する。
- 正は source（`include/uapi/` の header と、`src/drivers/gpu/` などの code）。`<!-- spec: -->` は何を書くかの案内で、source の言い換え。source と食い違えば source に従い、phase.md の結果の節に食い違いを書き、Q1 に報告する。
- header の comment と code が食い違えば、code の今の振る舞いを書き、食い違いを結果の節に書いて Q1 に報告する（Bug の候補）。文書の中で「誤り」「不具合」と書かず、予定（直す・変える）も書かない。
- 書くのは今の ABI（ABI version 1、`GPU_OP_PROTOCOL_VERSION` 2）。予定・設計中の UAPI は書かない。範囲の外: driver の開発用の制御の device（`/dev/amdrdna0`、`include/uapi/amdrdna.h`）、driver の内部の ops（`include/drivers/gpu/`）。
- 推測を書かない。読み切れなかった事は文書に書かず、結果の節に「未確認」として残す。

## 2. 文書の形（照合の script が読む規則。script の先頭の docstring が正本）

1. 言語は英語（docs/ の全文書と同じ）。[docs/style.md](../../docs/style.md) に従う。各 file の 3 行目の `Status: experimental reference for GPU ABI version 1.` を変えない。
2. `<!-- spec: ... -->` を本文で置き換え、comment は残さない。`PENDING` を全部埋める。
3. 見出しの H1〜H4 を変えない・足さない・消さない・順を変えない（`--base` で照合）。小見出しが要る時は H5（`#####`）を足してよい。
4. 機械の列を変えない: 構造体の表の Offset・Bytes・Type・Field、定数の表の Name・Value、索引の Request・Nr・Direction・Argument・Bytes・Described in、capset の表の Offset・Bytes・Payload field・Capset constant・Value。構造体の表と定数の表に行を足さない（capset の payload の表には範囲の行を足してよい）。
5. 構造体の表の Direction は `in`・`out`・`in/out`・`zero`（入力で 0 を求める予約の欄）のどれか。Meaning は意味・単位・範囲・0 の意味・誰が書くか。
6. 要求の節（`### \`GPU_NAME\``）の形。順に:
   - `Requires: open mode X; capability Y.` の 1 行。X は `R`（読みの open が要る）・`W`（書きの open が要る）・`RW`（両方）・`none`（どちらも見ない）。Y は `none` か、backtick の `GPU_CAP_*` を ` and `（全部要る）か ` or `（どれか）で繋いだ物。**code を読んで決める**（gpu.md の索引の同じ値は別の Phase が別に読んで決め、script が両方を比べる。写さない）。
   - 本文: 何をするか、入力と出力の欄の働き（表の Meaning を越える事）、順序・寿命・副作用。
   - `Errors:` の行と、その下に `- \`EXXX\`: 条件.` の list。handler と共通の道（`gpu_ioctl`・`gpu_ioctl_body`）が返す errno を全部。errno の名前は `include/uapi/errno.h` の物（`ENOTSUP` と `EOPNOTSUPP` は同じ値。source が使う名前を書く）。
   - `Sources:` の 1 行: header への link、実装の file への link と関数名（backtick）、試験の program の file への link（`../../userland/tests/<名前>/main.c`）か、無ければ `no source-tree test`。
7. link は docs/reference からの相対（`../../include/uapi/gpu.h`、`../../src/drivers/gpu/gpu.c`）。行番号を書かない（動く）。関数名を書く。docs/ から plan/ へ link しない。
8. source の comment・文書に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/、日付の経緯）を書かない。
9. 他の OS の文書・code（Linux DRM、FreeBSD drm-kmod、Mesa、virglrenderer）の文を写さない。互換の語は版を名指す（docs/style.md）。

## 3. 確かめの command（worktree の root で。全部の出力の最後の行を自分で読む）

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d --final docs/reference/<自分の file>
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 5fedf634d
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
git diff --name-only <起点の commit> HEAD   # 自分の file・自分の phase.md・ws.md だけ
```

- 1 本目が `check-gpu-uapi-doc: PASS` で終わる。2 本目（全体）も PASS（他の file の PENDING は数えるだけで FAIL にしない）。
- `Requires:` と索引の食い違いの FAIL は、両方が書かれた時だけ出る。自分の Phase で出たら code を読み直し、自分の側が正しいと確かめたら変えずに結果の節に書いて報告する（索引の側は統合の Phase が直す）。
- noct の link 検査は最初の誤りで止まる。PASS と件数を記録する。

## 4. 自己の照合（わざと壊す確かめの代わり）

文書には機械で照らせない主張（errno の条件・順序・寿命）が多い。結果の節に次の表を置く（文書には置かない）:

| 主張（節と要約） | 根拠（file と関数） | 試験（userland/tests の file と関数、無ければ「無し」） |

各要求の節の主張を全部挙げる。その上で、書いた `Errors:` の list から 3 つを選び、handler の code の該当の分岐を読み直して条件が合っていることを確かめ、結果の節に書く。
script のわざと壊す確かめは設計の Phase が済ませた（script は変えない）。

## 5. 変えてはいけない物

自分の Phase の file（phase.md の「作る物」）以外の全部: 他の gpu*.md、docs/ の他の文書（統合の Phase を除く）、`plan/ws009/tests/` の script、header・source、HAL、toolchain、共有の build。
script・骨格の不備（見出しが足りない、機械の列の誤り、照合の規則が source と合わない）を見つけたら、直さずに止め、どの file のどこで何が足りないかを報告する。

## 6. 終わりの条件

1. 3 節の command が全部 PASS（出力の最後の行を自分で読んだ）。
2. 4 節の自己の照合の表と 3 つの読み直しが結果の節にある。
3. commit: `git commit -m WIP -- docs/reference/<自分の file> plan/ws009/phase<NNN>/phase.md plan/ws009/ws.md`。push しない。rm しない。
4. phase.md の結果の節: 日付は host の `date`、実行した command と最後の行、書いた行数、自分で決めた点、source と spec・header の食い違い、未確認、未実施。
5. ws.md の Phase の表のこの行を「書いた・照合 PASS」にする（cleared は Q1 が決める）。
6. 報告: commit の SHA、command と最後の行、自分で決めた点、食い違い、未実施。
