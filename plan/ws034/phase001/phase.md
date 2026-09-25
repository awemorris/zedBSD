<!-- awesome-plan project=zedbsd record=ws034p001 -->

# ws034-p001: 版・入手元・依存グラフの確定

Phase ID: `ws034-p001`
Parent: [WS034](../ws.md)
Status: cleared（q316-i02、2026-09-23。結果は [results.md](results.md)）
Phase disposition: normal
Queue: q316（active）
実行: `phase-runner`（Opus 5.5、High）

## 目的

WS034の各パッケージについて、版・入手元・検証値・ライセンス・build方式・依存関係を確定し、
以降のPhaseがそのまま使える一覧と依存グラフを作る。**ソースとパッケージのMakefileは変更しない。**

## 範囲

1. **パッケージ一覧**（`plan/ws034/package-inventory.md`）
   対象: bash、coreutils、ca-certificates、zlib、expat、curl、wget、git、emacs、vim、binutils、GMP、MPFR、MPC、ISL、
   gcc 16、gdb、meson・ninja、libffi、pcre2、glib、libpng、freetype、harfbuzz、fontconfig、pixman、cairo、pango、fribidi、
   gdk-pixbuf、libjpeg-turbo、graphene、libepoxy、libxkbcommon（xkeyboard-configを含む）、本家libwayland、
   wayland-protocols、GTK4、Qt6（qtbase・qtwayland）、GTK3（atk）、Qt5（qtbase・qtwayland）、FFmpeg、VLC。
   Rustは後回しなので版だけ仮に記録する。Chromiumの依存（NSS等）は、名前と入手元だけを参考に書く。
   各項目に次を書く:
   - 版（その時点の安定版）と選んだ理由。
   - 入手元URLと、upstreamが公開する検証手段（署名、公開SHA-256）。
   - tarballを `build/distfiles` に取得し、SHA-256とsizeを記録する（WS032の `external.mk` と同じ形式で使える値）。
   - ライセンス（SPDX）と、packagesの境界での扱い。
   - build系（autoconf、CMake、meson、独自）と、クロスbuildで知られている問題。
   - 置き場所（ユーザー指定の分類。依存ライブラリは `libs/` 等の案）。
2. **依存グラフ**（同じ文書に、依存の表と図）
   - package間の依存（build時と実行時を分ける）。
   - WS034のPhase表の依存列と照合し、食い違いがあれば「Phase表の修正案」として書く（Phase表そのものは
     rootが直す）。
3. **CA bundle**: ca-certificatesの入手元（Mozillaのroot証明書由来の配布物）、ライセンス、更新の方法、
   `/etc/ssl` 等の置き場所とOpenSSL（WS032）の設定との対応。
4. **meson**: WS032のクロスbuild契約（wrapper、autoconf cache、CMake toolchain file）に、mesonのcross fileを
   どう加えるかの案（p025の入力）。

## 範囲外

パッケージのMakefile・パッチの作成、ソースの変更、HAL、GitHub公開。

## 受け入れ

- `package-inventory.md` に対象全件の版・URL・SHA-256・size・ライセンス・build系・置き場所がある。
  取得できなかったものは理由を書く。
- 依存グラフがあり、Phase表との食い違いが列挙されている（無ければ無いと書く）。
- CA bundleとmesonの案がある。
- ソースに差分が無い（`git status` で `plan/` 以外に変更が無い。`build/distfiles` はgit管理外）。

## 時間と上限

見積 180分。取得1件あたり600秒、合計の取得量は数GB以内（Chromiumは取得しない）。
同条件の変更なしretryは3回まで。
