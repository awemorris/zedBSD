<!-- awesome-plan project=zedbsd record=ws125-p002 -->

# ws125-p002: package の staged tree を image に入れる共通の仕組み

Parent: [WS125](../ws.md)
Status: cleared 候補（2026-10-09 P1 q916: `--subtree DEST=DIRECTORY` を root の Makefile の tree の規則・UFS と FAT の image の道具と検査に足し、3243 entry の host 試験 PASS。boot-test は ws126-p005 の image と合わせて T1。Q1 の判定待ち）
Disposition: normal
Queue / attempts: q916（2026-10-09 Q1 承認、ユーザー「ベータ2のすべての作業をP1でスケジューリングして行います」）の 3（ws126-p005 の前提）
Goal: package が数千の file（vim の runtime、Emacs の lisp、Python の標準 library）を、file ごとの `--file` を並べずに image へ入れられるようにする。
Prerequisites: なし
Investigation bound: 3 時間。

## 背景

今の `ZEDBSD_PACKAGE_FILES` は `--file DEST=SRC` を file ごとに並べ、root の `Makefile` の rootfs の規則と config の stamp（`files='$(strip $(ZEDBSD_PACKAGE_FILES))'`）で 1 つの shell の引数に展開される。Linux の 1 引数の上限（MAX_ARG_STRLEN = 128 KiB）により、数千の file は入らない。`make-arch-overlay-image.py` にも同じ並びが渡る。

## 範囲

- 新しい指定（案: `--tree DEST=STAGEDIR`、または stage が書く manifest の file を `--manifest` で渡す）を、root の `Makefile` の rootfs の組み立て、config の stamp（中身ではなく manifest の hash で変化を検出）、`tools/build/make-arch-overlay-image.py` と検査の側に足す。mode（実行可能な file）と symlink を保つ。
- 既存の `--file`・`--mode` の振る舞いは変えない。
- 小さな試験の package（または vim の stage）で、tree の中身が image に入り、file 数・mode・link が一致することを確かめる。

## 受け入れ

- 3000 file 以上の tree を入れても build が通り、image の中身が stage と一致する（file の一覧・size・mode を照合）。既存の image の中身が変わらない（変更前後の一覧の差が 0）。

## 検証

image の中身の照合の script（`plan/ws125/tests/`）、build、`plan/tools/boot-test.sh`。

## 所有 path

root の `Makefile` の rootfs・stamp の部分、`tools/build/make-arch-overlay-image.py`・`check-arch-overlay-image.py`、`userland/base/package.mk`（要れば変数の定義）、`plan/ws125/`。これらは全 package が共有するので、Queue にするときに main がこの所有を確認する。

## 依存・未決の判断

依存なし。WS124 p004・WS126 p005 の前提。

## 2026-10-09 P1 q916 の結果（cleared 候補）

### 形

- 指定は `--subtree DEST=DIRECTORY`（package の `ZEDBSD_PACKAGE_FILES` に `--file`・`--mode` と並べる）。`--tree` は make-arch-overlay-ufs.noct で「出来上がった root 全体」の意味に既に使われているので別の名前にした。manifest の file は作らない（tree の規則の引数は 1 つで、長さの問題が無い）。
- 中身の変化の検出: 選択の stamp（`.rootfs-config`）には指定の文字だけが入る。directory の中身は package が自分の stage の stamp を `ZEDBSD_PACKAGE_INPUTS` に入れて表す（stage が作り直されれば root の tree も作り直される）。Makefile の comment に書いた。
- mode と link: file の mode は保ち（`cp -RPp`）、symbolic link は link のまま。directory は従来どおり最後に 0755。`--mode` は tree の後に当たるので、subtree の中の file にも使える。

### 変更

- `Makefile`: `ZEDBSD_ROOTFS_TREE_RULE` の file の段に `--subtree)`（directory でなければ止まる、`mkdir -p` と `cp -RPp DIR/. DEST/`）。説明の comment、`ZEDBSD_EXTRA_FILES` の comment、`ARCH_IMAGE_TOOLS` に `subtree_files.py`。
- `tools/build/make-arch-overlay-ufs.noct`（試験の UFS image）: `--subtree` を受け、`--file` の後・`--mode` の前に写す。`--tree` とは併用しない。subtree がある時だけ inode を数えて頼む（`ufsCountEntries`×1.5＋256、`--tree` と同じ）。無い時は従来の `ufsBuild` のまま。
- `tools/build/check-arch-overlay-ufs.py`: `--subtree` の通常の file を `--file` と同じに照合（sha256、実行の bit）、link は link であることを確かめる。
- `tools/build/make-arch-overlay-image.py`・`check-arch-overlay-image.py`（FAT16）: `--subtree` を file ごとの `--file` に展開（FAT の名前の規則はそのまま当たる）、link は FAT に置けないので止まる。
- 新しい `tools/build/subtree_files.py`: 上の 3 つが使う展開（`os.walk`、directory への link は辿らない）。

### 確かめ

- `plan/ws125/tests/subtree-host-test.sh`（host、build/ws125-subtree に毎回新しい directory）: 3200 file（20×10 の入れ子、7 つに 1 つ 0755、空 file 1、file への link と directory への link、計 3243 entry）の stage を `/usr/lib/aat-tree` に:
  1. root の Makefile の `ZEDBSD_ROOTFS_TREE_RULE` の define そのものを小さな makefile で eval して流し、root の中の一覧（path・種類・size・mode・link の先）が stage と一致（`--mode` で 0600 にした 1 つを除いて同じ）、`/bin/sh` も入る。
  2. make-arch-overlay-ufs.noct で UFS image を作り、check-arch-overlay-ufs.py `--subtree` が全 file の中身・実行の bit・link を照合して OK。
  3. FAT の展開が link を断る。
  結果: `subtree-host-test: PASS`。最初の試行で UFS が `zedimage-host: inode table full`（file の列の経路は size だけで inode を決めていた）→ subtree の時に数える形に直して PASS。
- 逆の確かめ: stage の 1 file を変えた写しで checker が `manifest hash mismatch` で止まる。
- 既存の image が変わらないこと: `--subtree` の無い file だけの UFS image は、変更前と変更後の noct の出力が byte まで一致（`cmp`）。FAT は `--subtree` の有無の両方で make と check が OK。root の tree の規則は case を 1 つ足しただけで、既存の `--file`・`--mode` の行は変えていない（全体の image の前後の一覧の比較は未実施、T1 の image で）。
- 未実施: 全体の image の build と boot-test（T1、ws126-p005 の python3 の image と合わせて）。vim・Emacs の package への適用は WS124・WS125 の他の Phase。
