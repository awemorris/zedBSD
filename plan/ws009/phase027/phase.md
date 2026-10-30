<!-- awesome-plan project=zedbsd record=ws009-p027 -->

# ws009-p027: 完了した producer の文書の照合（DOC-T70〜T72）を今の規則で作り直す（案 C）

Status: planned（2026-10-30 P1 の設計、q1705-i01。WS の受け入れの確認で見つけた満ちない条件の 5・6）
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

（未着手）
