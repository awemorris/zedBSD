<!-- awesome-plan project=zedbsd record=ws009-p027 -->

# ws009-p027: 完了した producer の文書の照合（DOC-T70〜T72）を今の規則で作り直す（案 C）

Status: in-progress（書いた・PASS、2026-10-30 実装の担当。cleared は Q1。2026-10-30 P1 の設計、q1705-i01。WS の受け入れの確認で見つけた満ちない条件の 5・6）
Parent: [WS009](../ws.md)
Approval: 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」。2026-10-30 Q1: 案 C は Sonnet、plan/tools/ へ移す案を作る。
実装の担当: Sonnet 5.5 high（1 回の起動）。Q1 が起動する。
依存: p025・p026（照合する文書の文が、その 2 つで決まる）。両方が main に merge されてから起動する。

## 1. 目的

`plan/ws009/tests/check-completed-producer-docs.noct` は、最初の失敗で止まり、固定の文字列（Noct 2.0.1、3 つの variant、`set-key`、
evdev の「remain implemented and are deprecated」、`current/experimental/deprecated/planned` の状態の語）を照合するので、
source と docs/style.md が変わった後は正しい文書でも落ちる。同じ役目（完了した producer の主張を今の source と照らす）を、
**値を source から読む**形で作り直す。

## 2. 読む物

- AGENTS.md の §4・§7・§9（試験の整理）。
- 今の script `plan/ws009/tests/check-completed-producer-docs.noct`（何を照合していたかの一覧として。形は写さない）と
  [tests/README.md](../tests/README.md) の「DOC-T70--T72 command」。
- [docs/style.md](../../../docs/style.md) の Writing の節（状態の行・索引から辿れる事）。
- 照合の相手: `userland/base/net/main.c`（usage の文字列）、`docs/reference/managed-wlan.md`（p025 の後）、`userland/base/networkd/main.c`、
  `userland/base/net/wifi-conf.h`、`docs/reference/evdev.md`、`Makefile`（`ZEDBSD_VARIANTS_amd64_pcat :=`）、`tools/menuconfig.py`（variant の label）、
  `userland/base/noct/version.mk`、`toolchain/llvm/version.mk`、`docs/howto/build-from-source.md`（p026 の後）。
- 書き方の手本: `plan/ws009/tests/check-gpu-uapi-doc.py`（python3 -I、標準 library だけ、最後の 1 行の PASS・FAIL）。

## 3. 作る物

- 新しい script `plan/ws009/tests/check-docs-current.py`（python3、`-I` で動き、標準 library だけ。引数は repository の root、既定 `.`）:
  1. **失敗を全部集めてから**終わる。各失敗を `FAIL <file>: <理由>` の 1 行で出し、最後の行を
     `check-docs-current: PASS (<数えた物の要約>)` か `check-docs-current: FAIL (<n> problems)`、終了の値を 0 か 1。
  2. WLAN: `net/main.c` の usage から `net wifi <動詞>` の形を抜き出し（`"  net wifi ` で始まる文字列の行）、各形の先頭の語の列
     （例 `net wifi add SSID`）が managed-wlan.md にある事。managed-wlan.md に `set-key` が無い事。状態の名前は networkd の
     `managed-wlan-state.h`（か p025 の結果が示す定義の場所）から読み、各名前が backquote 付きで文書にある事。`WIFI_CONF_FILE_MAX` の値を
     header から読み、文書がその値を書くなら一致する事。
  3. evdev: `rejectText` だった 2 文（`Live USB HID binding is not implemented`・`p008 will reserve`）が無い事。legacy の console の event の
     ioctl が「removed」と書かれている事（今の evdev.md の「have been removed」）。
  4. build: Makefile の `ZEDBSD_VARIANTS_amd64_pcat` の各 variant が build-from-source.md の表の行（``| `<variant>` ``で始まる行）にある事。
     `tools/menuconfig.py` の各 label が backquote 付きで文書にある事。version.mk の `ZEDBSD_NOCT_TAG` の値と
     `ZEDBSD_NOCT_ARCHIVE_SHA256` の値が build-from-source.md にある事、`ZEDBSD_NOCT_VERSION` の値が `v<版>` の形である事。
     `toolchain/llvm/version.mk` の `ZEDBSD_LLVM_VERSION` が文書にある事。`build/amd64/sysroot/`・`build/i386/sysroot/`・`Target Noct` の 3 文（今と同じ）。
  5. 状態の行: `docs/` の `.md` のうち `docs/agent/` と `docs/release/zedbsd-*.md`（release の本文）を除く全部で、先頭 12 行の中に `Status: ` で始まる行がある事。
     `.internal/` を含まない事。
  6. 索引: `docs/README.md` から相対 link を辿り、5 と同じ除外の後の全ての `.md` に届く事（届かない file を全部出す）。
- `plan/ws009/tests/README.md` の「DOC-T70--T72 command」の節を新しい script の command と照合の項目に書き換える。
- この phase.md の「結果」の節（下の「移す案」を含む）、[ws.md](../ws.md) の Phase の表のこの行。
- 古い `check-completed-producer-docs.noct` は消さない（Q1 に path と理由を返す）。

## 4. 試験の項目（script 自体の確かめ。git の外の写しで行い、元に戻す）

- 今の tree で PASS。
- わざと壊して FAIL を確かめ、戻す（`cp` で写しを取り、`cmp` で戻ったことを確かめる）。少なくとも次の 5 通り:
  managed-wlan.md の `net wifi add` の 1 字を変える、evdev.md に `p008 will reserve` を足す、build-from-source.md の Noct の commit の 1 字を変える、
  docs/reference/README.md から 1 行の link を消す、docs/reference/tls.md の Status の行を消す。各回で FAIL の行が出て終了の値が 1。
  1 回で 2 つ壊して、2 つとも出る（全部集める事の確かめ）。

## 5. 移す案（結果の節に書く。移すのは WS009 の完了の時に Q1）

次の 3 本を `plan/tools/docs-check/` へ移す案と、[plan/tools.md](../../tools.md) に足す行の文案を書く。
`check-relative-links.noct`（DOC-T00）・`check-docs-current.py`（DOC-T70〜T72 と状態の行・索引）・`check-gpu-uapi-doc.py`（DOC-54。
`gpu-doc-rules.md` を参照するので、その文書の行き先（例: `plan/tools/docs-check/gpu-doc-rules.md`）と、script の docstring の path の直しも案に入れる）。
移した後の path で動かす command を各 1 行で。

## 6. 変えてはいけない物

- 3 節の file 以外の全部。docs/（文書が script と食い違えば、文書を直さず結果に書く）。source・Makefile・tools/・他の試験・HAL・toolchain・共有の build。

## 7. 確かめの command

```sh
python3 -I plan/ws009/tests/check-docs-current.py .
timeout 120 build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/howto docs/architecture docs/release
git diff --check
```

## 8. 終わりの条件

7 節が PASS、4 節のわざと壊す確かめを全部した（どれも FAIL を出した、戻した事を `cmp` で確かめた）。commit は
`git add plan/ws009/tests/check-docs-current.py && git commit -m WIP -- plan/ws009/tests/check-docs-current.py plan/ws009/tests/README.md plan/ws009/phase027/phase.md plan/ws009/ws.md`。
結果の節（日付は host の `date`、command と最後の行、照合の項目の数、壊した 5 通りとその FAIL の行、自分で決めた点、移す案と tools.md の行の文案、
消す物の一覧（古い .noct））。ws.md の行は「書いた・PASS」（cleared は Q1）。

## 結果

2026-10-30、実装の担当（Sonnet 5.5）。worktree `s-ws009-p027`（main 00396e3da から）。

### 作った物

- `plan/ws009/tests/check-docs-current.py`（python3 -I、標準 library だけ、引数は root で既定 `.`）。失敗を全部集め、最後の行が
  `check-docs-current: PASS (...)` か `FAIL (<n> problems)`、終了の値 0 か 1。値は全て source から読む（usage の行、
  `managed-wlan-state.h` の enum と `networkd/main.c` の `managed_state_name` の綴り、`wifi-conf.h`、Makefile、`tools/menuconfig.py`
  を `ast` で読んだ `BOARD_VARIANTS`、2 つの `version.mk`）。
- `plan/ws009/tests/README.md` の「DOC-T70--T72 command」を新しい command と照合の項目に書き換えた。

### command と最後の行

```
$ python3 -I plan/ws009/tests/check-docs-current.py .
check-docs-current: PASS (49 indexed-documents, 55 items, 4 labels, 49 status-documents, 4 variants, 9 wlan-forms, 7 wlan-states)
$ timeout 120 build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/howto docs/architecture docs/release
Markdown relative-link check: PASS (1048 links)
$ git diff --check     (出力なし)
```

照合の数: wlan の形 9、wlan の状態 7、variant 4、menu の label 4、状態の行 49 件、索引 49 件、その他の項目 55 件。
古い `check-completed-producer-docs.noct` は今の tree で `userland/base/net/main.c: missing required text: net wifi set-key ...` で止まる（変更前と同じ）。

### わざと壊した確かめ（scratchpad の写し。docs と source の写しで行い、`cp` で戻して `cmp` で一致を確かめた）

| 壊し方 | FAIL の行 | 終了の値 |
| --- | --- | --- |
| managed-wlan.md の `net wifi add SSID` を `net wifi ad SSID` | `missing command form of the usage: net wifi add SSID` と `` `net wifi ad` is not a verb of the usage `` | 1 |
| evdev.md に `p008 will reserve` を足す | `contains retired sentence: p008 will reserve` | 1 |
| build-from-source.md の Noct の commit の末尾 1 字 | `missing Noct tag (ZEDBSD_NOCT_TAG): f6efa83a...` | 1 |
| docs/reference/README.md から `security-keys.md` の行 | `docs/reference/security-keys.md: not reachable from docs/README.md by relative links` | 1 |
| tls.md の Status の行を消す | `docs/reference/tls.md: no line starting "Status: " in the first 12 lines` | 1 |
| 上の 1 と 5 を同時に | 3 行（add の 2 行と tls.md の 1 行）が全部出る、`FAIL (3 problems)` | 1 |

### 自分で決めた点

- phase.md の索引の壊し方は `docs/reference/README.md` から tls.md の行を消す例だが、tls.md は `compatibility-profile.md` からも link されるので、
  行を消しても届く（正しく PASS）。README だけが link する `security-keys.md` で確かめた。
- 「`net wifi` の形」は usage の最初の block（`"  net wifi ` で始まる文字列）から、最初の `[` の前までの語を取る。
  逆の向きも足した: 文書が書く `net wifi <動詞>` は全て usage の動詞である事。
- 状態の名前は enum（`managed-wlan-state.h`）から取り、綴りは `networkd/main.c` の `managed_state_name` に合わせる
  （`RETIRING` は `disconnecting`）。
- ファイルの大きさの限度は、文書が `limited to N bytes` と書く時だけ `WIFI_CONF_FILE_MAX` と比べる。
- 足した照合（source から導ける物）: Noct の archive の大きさ（`2,529,835 bytes` の形）、evdev.md の `Noct <版> BeUI`、
  `ZEDBSD_LLVM_CACHE_TAG`。
- 索引は、code の fence の外の `](...)` の link を辿り、directory の link は `README.md` とみなす。

### 文書の誤り（直していない。報告）

- `docs/howto/build-from-source.md` は LLVM の cache の asset を `zedbsd-llvm-23.1.0-zedbsd9-x86_64-linux.tar.gz` と書くが、
  `toolchain/llvm/version.mk` の `ZEDBSD_LLVM_CACHE_ASSET` は `zedbsd-llvm-23.1.0-zedbsd11-x86_64-linux.tar.gz`。
  この照合を足すと今の tree で FAIL になるので足していない（文書を直した後に `ZEDBSD_LLVM_CACHE_ASSET` を `need` する 1 行を足せる）。

### 移す案（移すのは WS009 の完了の時に Q1。未実施）

`plan/tools/docs-check/` へ次の 3 本と 1 文書を移す。

| 今の path | 移した後の path | 動かす command |
| --- | --- | --- |
| `plan/ws009/tests/check-relative-links.noct` | `plan/tools/docs-check/check-relative-links.noct` | `build/NoctLang/build-static/noct plan/tools/docs-check/check-relative-links.noct docs plan` |
| `plan/ws009/tests/check-docs-current.py` | `plan/tools/docs-check/check-docs-current.py` | `python3 -I plan/tools/docs-check/check-docs-current.py .` |
| `plan/ws009/tests/check-gpu-uapi-doc.py` | `plan/tools/docs-check/check-gpu-uapi-doc.py` | `python3 -I plan/tools/docs-check/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final` |
| `plan/ws009/gpu-doc-rules.md` | `plan/tools/docs-check/gpu-doc-rules.md` | （規則の文書。script の docstring の path を `plan/tools/docs-check/gpu-doc-rules.md` に直す。`--base 4dfcce008` の基点が履歴から消える時は `--base` を外す） |

[plan/tools.md](../../tools.md) に足す行の文案:

```
| 文書の照合（docs/） | `plan/tools/docs-check/` | `check-relative-links.noct`（docs の相対 link の存在）、`check-docs-current.py`（文書を今の source と照らす: net wifi の形と状態、evdev、build の variant・Noct・LLVM、状態の行、索引から届く事。失敗を全部出す）、`check-gpu-uapi-doc.py`（/dev/gpu の UAPI の文書を header と照らす。規則は `gpu-doc-rules.md`）。`python3 -I plan/tools/docs-check/check-docs-current.py .` ほか |
```

（tools.md の表の列の形に合わせて Q1 が整える。）

### 消す物（Q1）

- `plan/ws009/tests/check-completed-producer-docs.noct`: `check-docs-current.py` に置き換わった。参照が残る所: `plan/ws009/ws.md`（66・78・87 行の経緯の記述）、
  `plan/ws009/phase006/phase.md`・`phase023/phase.md`（履歴）。

### 未実施

- 移す案の実行（`plan/tools/docs-check/` への移動、tools.md の行）。
- docs/ の誤りの修正（上）。
