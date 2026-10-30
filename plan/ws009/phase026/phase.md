<!-- awesome-plan project=zedbsd record=ws009-p026 -->

# ws009-p026: build-from-source.md の Noct と variant の label、索引から辿れない文書、状態の行（案 B）

Status: planned（2026-10-30 P1 の設計、q1705-i01。WS の受け入れの確認で見つけた満ちない条件の 1・2・4）
Parent: [WS009](../ws.md)
Approval: 2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」。2026-10-30 Q1: 案 B は Sonnet。
実装の担当: Sonnet 5.5 high（1 回の起動）。Q1 が起動する。
依存: なし（p025 と並べてよい。触る file が重ならない）

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) の全文（特に「Begin each document with a status line that says what kind of document it is」と「Every document is reachable from the documentation index or a section index」）。
- ws.md の「WS の受け入れの確認（2026-10-30）」の節。
- 正の値の出所（この file の値を写す。推測しない）:
  - `userland/base/noct/version.mk`: `ZEDBSD_NOCT_VERSION`（今 2.0.3）、`ZEDBSD_NOCT_TAG`（commit）、`ZEDBSD_NOCT_ARCHIVE_SIZE`、`ZEDBSD_NOCT_ARCHIVE_SHA256`、版の comment（「Upstream release v2.0.3 (tag v2.0.3 is this commit)」）。
  - `tools/menuconfig.py` の 60〜63 行の variant の label（`native` → `UEFI, UFS root partition (for PC/AT)`、`hybrid` → `UEFI + BIOS (for PC/AT)`、`uefi` → `UEFI (for Apple)`、`bios` → `BIOS (for PC/AT)`）。
  - `Makefile` の `ZEDBSD_VARIANTS_amd64_pcat :=`（今 `native hybrid uefi bios`）。
  - `userland/base/noct/patches/` の中身（evdev.md が link する patch が今もあるか、BeUI の backend がまだ patch か upstream か。version.mk の comment は「terminal and BeUI work that used to be local patches」が upstream に入り「only the cross-build patches remain」と言う）。

## 2. 作る物（この file だけを変える）

1. `docs/howto/build-from-source.md`:
   - 62〜66 行あたりの Noct の段落を version.mk の値に合わせる（release `v2.0.3`、commit `f6efa83a8ccb9262ce7f802c0e89e23a39c31c5a`、archive の大きさ 2,529,835 bytes、SHA-256 `b46863cecfc885a98d3d84e8ff892d0205408e47cf08be024f5e0d502ac0006c`）。値は書く前に version.mk から読み直す。「LP64 File.seek fix」の類の、今の version.mk の comment に無い主張は消すか comment に合わせる。
   - 150〜153 行の `ZEDBSD_VARIANT` の表に menuconfig の label の列（`Menu label`）を足し、4 行それぞれに `tools/menuconfig.py` の label を backquote で書く。
   - 他に `2.0.1` の記述があれば version.mk に合わせる。
2. `docs/reference/evdev.md` の 186 行・197 行の「Noct 2.0.1 BeUI」: version.mk と patches/ を見て今の版と出所に合わせる（patch の link が実在しなくなっていれば link 先を直す。実在すれば版の数字だけ直す）。
3. 索引: `docs/reference/README.md` に `media-playback.md` への 1 行（他の行と同じ形、1 文の説明）、`docs/release/README.md` の「Kei/zedBSD 1.0.0 Beta 2」の list に `zedbsd-1.0.0-beta2.md`（GitHub の release の本文）への 1 行。
4. 状態の行: 次の 11 file の H1（setext の見出しなら下線）の直後に空行と `Status: ...` の 1 行を足す。語は既存の文書の語に揃える。
   - `docs/README.md`・`docs/architecture/README.md`・`docs/reference/README.md` → `Status: current navigation index`（`docs/howto/README.md` と同じ）。
   - `docs/style.md` → `Status: current documentation rules`。
   - `docs/reference/agent-acceptance-test.md`・`atomic-publication.md`・`block-administration.md`・`media-playback.md`・`security-keys.md`・`tls.md`・`vulkan-video.md` → `Status: reference.`。ただし本文が「まだ作っていない設計」と自分で言う物は `Status: design.`、「pending」「not yet」の類で一部が未確認と言う物は `Status: reference; <未確認の点を本文の語で 1 句>.`。どれにしたかを結果に書く。
   - `docs/release/zedbsd-1.0.0-beta2.md` には足さない（`.github/workflows/release.yml` が GitHub の release の本文として読む file。変えない）。

## 3. 確かめの項目

- version.mk・menuconfig.py・Makefile の値と文書の値が一字一句で合う（grep で両方を出して比べ、結果に貼る）。
- 足した Status の行の数が 11（README 3・style 1・reference 7）で、`docs/release/zedbsd-1.0.0-beta2.md` は無変更。
- docs/README.md から相対 link を辿って届く `docs/` の `.md` が、`docs/agent/` の 4 file を除いて全部（下の command の 2 本目）。
- 本文の意味を変えない（状態の行・索引の行・Noct の段落・variant の列・evdev の 2 か所の版の他は触らない）。

## 4. 注意

- `docs/agent/` は agent 向けの文書で、この Phase の対象の外（触らない）。
- `docs/release/zedbsd-1.0.0-beta2.md` の先頭の HTML comment は触らない（Q1 に報告するだけ）。
- source の側（version.mk・menuconfig.py・Makefile）が正。文書の他の所が source と食い違うのを見つけたら、直さずに結果に書く。

## 5. 変えてはいけない物

- 2 節の file 以外の全部。source・Makefile・tools/・試験の道具（plan/ws009/tests/）・HAL・toolchain・共有の build。
- 不備（2 節の指示が source と食い違う等）を見つけたら、source に従い、食い違いを結果に書く。

## 6. 確かめの command

```sh
grep -n 'ZEDBSD_NOCT_VERSION\|ZEDBSD_NOCT_TAG \|ARCHIVE_SIZE\|ARCHIVE_SHA256' userland/base/noct/version.mk
grep -n 'f6efa83\|2,529,835\|b46863ce\|v2\.0\.3\|2\.0\.1' docs/howto/build-from-source.md docs/reference/evdev.md
sed -n 58,64p tools/menuconfig.py; grep -n 'for PC/AT)\|for Apple)' docs/howto/build-from-source.md
python3 -I - <<'EOF'
import os, re
start = os.path.normpath('docs/README.md'); seen = set(); todo = [start]
pat = re.compile(r'\]\(<?([^)>#?\s]+)')
while todo:
    p = todo.pop()
    if p in seen or not p.endswith('.md') or not os.path.isfile(p): continue
    seen.add(p)
    for t in pat.findall(open(p, encoding='utf-8').read()):
        if '://' in t or t.startswith(('/', 'mailto:')): continue
        q = os.path.normpath(os.path.join(os.path.dirname(p), t))
        if q.startswith('docs' + os.sep): todo.append(q)
alld = {os.path.normpath(os.path.join(d, f)) for d, _, fs in os.walk('docs') for f in fs if f.endswith('.md')}
miss = sorted(x for x in alld - seen if not x.startswith('docs/agent/'))
nost = sorted(x for x in alld if not x.startswith('docs/agent/') and x != 'docs/release/zedbsd-1.0.0-beta2.md'
              and not re.search(r'^Status:', open(x, encoding='utf-8').read(), re.M))
print('unreached', miss); print('no status', nost)
EOF
timeout 120 build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/howto docs/architecture docs/release
git diff --check
git diff --stat
```

2 本目の終わりの 2 行が `unreached []` と `no status []`。link の検査が PASS。

## 7. 終わりの条件

6 節の command が全部期待どおり（出力を自分で読む）。commit は
`git commit -m WIP -- docs/howto/build-from-source.md docs/reference/evdev.md docs/reference/README.md docs/release/README.md docs/README.md docs/architecture/README.md docs/style.md docs/reference/agent-acceptance-test.md docs/reference/atomic-publication.md docs/reference/block-administration.md docs/reference/media-playback.md docs/reference/security-keys.md docs/reference/tls.md docs/reference/vulkan-video.md plan/ws009/phase026/phase.md plan/ws009/ws.md`。
結果の節（日付は host の `date`、command と最後の行、file ごとに選んだ Status の語、自分で決めた点、source と文書の食い違い、未実施）。ws.md の行は「書いた・確かめ PASS」（cleared は Q1）。

## 結果

（未着手）
