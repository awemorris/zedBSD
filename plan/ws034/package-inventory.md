# WS034 パッケージ一覧・依存グラフ・CA bundle・meson案

作成 2026-09-23（ws034-p001、q316-i02）。WS034の以降のPhaseが使う、版・入手元・検証値・ライセンス・
build系・既知のクロスbuild問題・置き場所と、package間の依存グラフの正本である。

- 値（size、SHA-256、展開後の根ディレクトリ）は、すべて `build/distfiles` に実際に取得して実測した。
  推測値は書いていない。実測の生データは `plan/ws034/phase001/distfiles-measured.json`、
  取得一覧は `plan/ws034/phase001/distfiles.tsv`。
- ここに書く「既知のクロスbuild問題」は、取得したtarballの `configure`・`meson.build`・`CMakeLists.txt` 等を
  読んで確かめたものである。**実際にクロスbuildはしていない**（p001の範囲外）。buildして初めて分かる問題は
  各Phaseで追記する。
- ライセンスは、tarball内のライセンスファイルと、meson.buildの `license:` 欄などで確かめた。各Phaseで
  パッケージを作るときに、WS032と同じ機械監査（`plan/tools/packages/audit-licenses.sh` 相当）を行う。

## 1. 取得と検証

### 1.1 方法

| 項目 | 内容 |
| --- | --- |
| 取得 | `plan/ws034/phase001/fetch-distfiles.sh`（`distfiles.tsv` の各行を `curl --fail --location --max-time 600` で取得。一時ファイルに取って成功時だけ正規名へ `mv`） |
| 実測・照合 | `plan/ws034/phase001/measure-distfiles.py`（size・SHA-256・tarの根ディレクトリを実測し、WS032の `userland/packages/tools/archive.sh verify <archive> <size> <sha256> <root>` を実測値で実行。upstreamの署名・公開digestと照合） |
| 署名の検証 | 使い捨ての `GNUPGHOME`（`/tmp/ws034p001/gnupg`）。利用者のkeyringには触れていない |
| 上限 | 1件600秒。合計 559,867,050 byte（約534 MiB、検証用ファイルとbashパッチを含む）。Chromiumとその依存は取得していない |

**archive.sh verify** は、表の全tarball（`.pem` を除く53件）で `ok` だった。つまり表の
`ARCHIVE`・`SIZE`・`SHA256`・`ROOT` をそのまま `ZEDBSD_EXT_<name>_*` に書けば、WS032の
`external.mk` の取得・検証・展開に通る（`.pem` は§7のとおり tar でないので通らない）。

### 1.2 実測値（`external.mk` の値）

`ARCHIVE_URL` は §2 の各行の入手元。ROOT は `ZEDBSD_EXT_<name>_ROOT` に書く値で、ファイル名と一致しないものがある
（Qt5は `opensource` が付かない、libxkbcommonは `libxkbcommon-xkbcommon-1.13.2`、Debianのca-certificatesは版が付かない）。

| key | 版 | ファイル | size (byte) | SHA-256 | ROOT | upstreamの検証値との照合 |
| --- | --- | --- | --- | --- | --- | --- |
| bash | 5.3 | `bash-5.3.tar.gz` | 11355854 | `0d5cd86965f869a26cf64f4b71be7b96f90a3ba8b3d74e27e8e9d9d5550f31ba` | `bash-5.3` | 署名 good（Chet Ramey） |
| coreutils | 9.12 | `coreutils-9.12.tar.xz` | 6704996 | `a480198559733e9b3da999e90543ac6f888a2caa544d8d664c5a1f17e528e210` | `coreutils-9.12` | 署名 good（Pádraig Brady） |
| ca-certificates（curl配布のPEM） | 2026-08-13 | `cacert-2026-08-13.pem` | 188900 | `f66dff1bdf8f96060b8177976f8b7d9254bc89bc4db933d769f7384d28480bc9` | （単一ファイル） | 公開SHA-256 一致 |
| ca-certificates（Debian、比較用） | 20260816 | `ca-certificates_20260816.tar.xz` | 271796 | `d939bcdd0cb058712cf4175bac76997676eb8b68fe9473765e1b40fb3d5b186a` | `ca-certificates` | 署名付き `.dsc` のSHA-256 一致・`.dsc` 署名 good（Julien Cristau） |
| zlib | 1.3.2 | `zlib-1.3.2.tar.xz` | 1320064 | `d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3` | `zlib-1.3.2` | 署名 good（Mark Adler） |
| expat | 2.8.5 | `expat-2.8.5.tar.xz` | 528380 | `1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182` | `expat-2.8.5` | 署名 good（Sebastian Pipping） |
| curl | 8.22.0 | `curl-8.22.0.tar.xz` | 2953092 | `f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7` | `curl-8.22.0` | 署名 good（Daniel Stenberg） |
| wget | 1.25.0 | `wget-1.25.0.tar.gz` | 5263736 | `766e48423e79359ea31e41db9e5c289675947a7fcf2efdcedb726ac9d0da3784` | `wget-1.25.0` | 署名 good（Darshit Shah） |
| git | 2.55.0 | `git-2.55.0.tar.xz` | 8177180 | `457fdb04dc8728e007d4688695e6912e6f680727920f2a40bf11eacc17505357` | `git-2.55.0` | 署名付き `sha256sums.asc` に一致・一覧の署名 good（kernel.org autosigner） |
| emacs | 31.1 | `emacs-31.1.tar.xz` | 57977476 | `1da5790d9580c81932b5bf700633114468da7b3412d69faa767daebf974f4586` | `emacs-31.1` | 署名 good（Sean Whitton） |
| vim | 9.2.1125 | `vim-9.2.1125.tar.gz` | 20194423 | `6242a0916e07d38c7c25b583e69f413806b772808e95130133a23c9d46cedb32` | `vim-9.2.1125` | 公開digest無し。tar内のcommit IDがtag `v9.2.1125` のcommitと一致（§1.3） |
| binutils | 2.47 | `binutils-2.47.tar.xz` | 29034716 | `154ab23b60070e8f27013c22977f1129425d67d1e8acd6e13010e617811e4cff` | `binutils-2.47` | 署名 good（Nick Clifton） |
| gmp | 6.3.0 | `gmp-6.3.0.tar.xz` | 2094196 | `a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898` | `gmp-6.3.0` | 署名 good（Niels Möller） |
| mpfr | 4.2.2 | `mpfr-4.2.2.tar.xz` | 1505596 | `b67ba0383ef7e8a8563734e2e889ef5ec3c3b898a01d00fa0a6869ad81c6ce01` | `mpfr-4.2.2` | 署名 good（Vincent Lefevre） |
| mpc | 1.3.1 | `mpc-1.3.1.tar.gz` | 773573 | `ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8` | `mpc-1.3.1` | 署名 good（Andreas Enge） |
| isl | 0.24 | `isl-0.24.tar.bz2` | 2261594 | `fcf78dd9656c10eb8cf9fbd5f59a0b6b01386205fe1934b3b287a0a1898145c0` | `isl-0.24` | 公開digest無し。別系統（libisl.sourceforge.io）の同名ファイルとSHA-256一致（§1.3） |
| gcc | 16.2.0 | `gcc-16.2.0.tar.xz` | 107200820 | `e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e` | `gcc-16.2.0` | 署名 good（Richard Guenther） |
| gdb | 17.2 | `gdb-17.2.tar.xz` | 24658624 | `1c036c0d72e4b3d1fb5c94c88632add6f9d76f4d7c4d2ea793c12a9f19a3228c` | `gdb-17.2` | 署名 good（Joel Brobecker） |
| meson | 1.12.0 | `meson-1.12.0.tar.gz` | 2518280 | `88afe0c20e52030218924ac37d0c81c59b4b5f3ae3752c8c6d7470c7d365886c` | `meson-1.12.0` | 署名 good（Jussi Pakkanen） |
| ninja | 1.13.2 | `ninja-1.13.2.tar.gz` | 292385 | `974d6b2f4eeefa25625d34da3cb36bdcebe7fbce40f4c16ac0835fd1c0cbae17` | `ninja-1.13.2` | 公開digest無し。tar内のcommit IDがtag `v1.13.2` と一致 |
| libffi | 3.8.0 | `libffi-3.8.0.tar.gz` | 1581626 | `7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4` | `libffi-3.8.0` | GitHub release assetのdigest一致 |
| pcre2 | 10.48 | `pcre2-10.48.tar.bz2` | 2213024 | `b6c68fdf6f3ac31388b50aa89ff0fc49c00c987c16e7b5146491d12003f2c8ed` | `pcre2-10.48` | 署名 good（Nicholas Wilson） |
| glib | 2.90.0 | `glib-2.90.0.tar.xz` | 5830480 | `17d15cac2af80a33271127408e0abc2748eb297c595c2a26409e81e14e7d1b8f` | `glib-2.90.0` | 公開SHA-256 一致 |
| libpng | 1.6.58 | `libpng-1.6.58.tar.xz` | 1070096 | `28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775` | `libpng-1.6.58` | SourceForge公開のMD5一致、展開内容がgit tag `v1.6.58` の木と全ファイル一致（§1.3） |
| freetype | 2.14.3 | `freetype-2.14.3.tar.xz` | 2670220 | `36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f` | `freetype-2.14.3` | 署名 good（Werner Lemberg） |
| harfbuzz | 14.5.0 | `harfbuzz-14.5.0.tar.xz` | 20262956 | `b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9` | `harfbuzz-14.5.0` | GitHub release assetのdigest一致 |
| fontconfig | 2.18.3 | `fontconfig-2.18.3.tar.xz` | 1172016 | `4f7b554a38cdf78c033f666c8871f3749e14a094f65a07f630c91ed0b43d35e3` | `fontconfig-2.18.3` | 公開SHA-256 一致 |
| pixman | 0.46.4 | `pixman-0.46.4.tar.xz` | 660536 | `a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239` | `pixman-0.46.4` | 公開SHA-512 一致 |
| cairo | 1.18.6 | `cairo-1.18.6.tar.xz` | 32725456 | `1c767308174337a74694da0f3ec069c271452163a1ef4540964c50c301f157d4` | `cairo-1.18.6` | 公開SHA-256 一致 |
| pango | 1.58.2 | `pango-1.58.2.tar.xz` | 1918112 | `342385b6ca3b7c73455d7c80a13b7dbe4489e00bc3bd4c5bd6ed4dce421e374a` | `pango-1.58.2` | 公開SHA-256 一致 |
| fribidi | 1.0.17 | `fribidi-1.0.17.tar.xz` | 1203284 | `6949dcde27d41cebad1fd741fcafc36d55a1020d2d872d4a6eb3914caabbada2` | `fribidi-1.0.17` | GitHub release assetのdigest一致 |
| gdk-pixbuf | 2.44.8 | `gdk-pixbuf-2.44.8.tar.xz` | 6037756 | `919f529512961a12e81cd4b4b466a48c3933469e7f9a310c6513cd4fb252ba3c` | `gdk-pixbuf-2.44.8` | 公開SHA-256 一致 |
| libjpeg-turbo | 3.2.0 | `libjpeg-turbo-3.2.0.tar.gz` | 2537858 | `6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e` | `libjpeg-turbo-3.2.0` | 署名 good（libjpeg-turbo Project） |
| graphene | 1.10.8 | `graphene-1.10.8.tar.xz` | 333924 | `a37bb0e78a419dcbeaa9c7027bcff52f5ec2367c25ec859da31dfde2928f279a` | `graphene-1.10.8` | 公開SHA-256 一致 |
| libepoxy | 1.5.10 | `libepoxy-1.5.10.tar.xz` | 223528 | `072cda4b59dd098bba8c2363a6247299db1fa89411dc221c8b81b8ee8192e623` | `libepoxy-1.5.10` | 公開SHA-256 一致 |
| libxkbcommon | 1.13.2 | `libxkbcommon-1.13.2.tar.gz` | 1243485 | `acc4d5f7c3cbba5f9f8d08d8bdbeede84ecede46792f47929aa9321873385528` | `libxkbcommon-xkbcommon-1.13.2` | 公開digest無し。tar内のcommit IDがtag `xkbcommon-1.13.2` と一致 |
| xkeyboard-config | 2.48 | `xkeyboard-config-2.48.tar.xz` | 953316 | `b77041324f0109f77161ee43743fe04baa485866af8460d31e476ad3f7648fd5` | `xkeyboard-config-2.48` | 署名 good（Sergey Udaltsov） |
| wayland（本家libwayland） | 1.26.0 | `wayland-1.26.0.tar.xz` | 612384 | `64176eaa46e4969903e286f8e5ef8331affc17fdf03ac9b58381d2b23162b7a3` | `wayland-1.26.0` | 署名 good（Simon Ser）・公開SHA-256 一致 |
| wayland-protocols | 1.49 | `wayland-protocols-1.49.tar.xz` | 150612 | `ec4c8f74942d6dff7ace8b4ce4764f0ef9ff618a935d974ea77edee2ad240b14` | `wayland-protocols-1.49` | 署名 good（Simon Ser）・公開SHA-256 一致 |
| gtk4 | 4.24.0 | `gtk-4.24.0.tar.xz` | 17652716 | `28ba4ac1c04f86eac09b79a163cb163a4c2b54442d9f7eccc04679062a581044` | `gtk-4.24.0` | 公開SHA-256 一致 |
| qt6 qtbase | 6.11.2 | `qtbase-everywhere-src-6.11.2.tar.xz` | 50582668 | `5b2e00eccaf5a4d8c14134ffa0ea8dfd0a35ae1ffc7f8d87fa4305a1ed23cf22` | `qtbase-everywhere-src-6.11.2` | 公開SHA-256 一致 |
| qt6 qtwayland | 6.11.2 | `qtwayland-everywhere-src-6.11.2.tar.xz` | 905288 | `8eb7615e39332a10f506e8dd70f02d5954bb5949ff54f6dcbf8bd6168222f9df` | `qtwayland-everywhere-src-6.11.2` | 公開SHA-256 一致 |
| gtk3 | 3.24.52 | `gtk-3.24.52.tar.xz` | 13578032 | `80931fa472a77b9a164f6740e3c0b444fac6770054632d35a7ff9d679e5e7b9f` | `gtk-3.24.52` | 公開SHA-256 一致 |
| atk（at-spi2-core） | 2.62.0.1 | `at-spi2-core-2.62.0.1.tar.xz` | 599704 | `fa462f1834bae569c5944c34608872f9447e5a2889ba2aa4d5ff9f2d6ff8a395` | `at-spi2-core-2.62.0.1` | 公開SHA-256 一致 |
| atk（単体の最終版。§2.8） | 2.38.0 | `atk-2.38.0.tar.xz` | 303952 | `ac4de2a4ef4bd5665052952fe169657e65e895c5057dffb3c2a810f6191a0c36` | `atk-2.38.0` | 公開SHA-256 一致 |
| qt5 qtbase | 5.15.19 | `qtbase-everywhere-opensource-src-5.15.19.tar.xz` | 51588440 | `51e91c73abacab81e64efd01bf95794e79c0e605ad80947a024769f0dd620a32` | `qtbase-everywhere-src-5.15.19` | 公開SHA-256 一致 |
| qt5 qtwayland | 5.15.19 | `qtwayland-everywhere-opensource-src-5.15.19.tar.xz` | 547652 | `9efee6413514986353faace0415ad238b5ab00f8ecb0693381f563076e95ec93` | `qtwayland-everywhere-src-5.15.19` | 公開SHA-256 一致 |
| qt5 qtsvg（VLCが必要。§4） | 5.15.19 | `qtsvg-everywhere-opensource-src-5.15.19.tar.xz` | 1867700 | `cabc95fc12fa84c92e95f08660742a0ce297e6db3deadbefbe7fbec4769b8c13` | `qtsvg-everywhere-src-5.15.19` | 公開SHA-256 一致 |
| ffmpeg | 8.1.3 | `ffmpeg-8.1.3.tar.xz` | 11732036 | `7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3` | `ffmpeg-8.1.3` | 署名 good（FFmpeg release signing key） |
| vlc | 3.0.24 | `vlc-3.0.24.tar.xz` | 27620304 | `e7cab503d1d7d5849b89d2cf0e1ee60d0ef6d012407791b644b9cfc0cc225fdf` | `vlc-3.0.24` | 公開SHA-256 一致。**署名は未検証**（鍵を入手できない。§1.3） |
| libtiff（追加。§4） | 4.7.2 | `tiff-4.7.2.tar.xz` | 2409652 | `4996f0c4f93094719b1ca5c6279b20e588773ba8a247533e486416fb662ddb88` | `tiff-4.7.2` | 署名 good（Even Rouault） |
| gperf（追加、host tool。§4） | 3.3 | `gperf-3.3.tar.gz` | 1831294 | `fd87e0aba7e43ae054837afd6cd4db03a3f2693deb3619085e6ed9d8d9604ad8` | `gperf-3.3` | 署名 good（Bruno Haible） |
| libiconv（候補。§4） | 1.19 | `libiconv-1.19.tar.gz` | 5921103 | `88dd96a8c0464eca144fc791ae60cd31cd8ee78321e67397e25fc095c4a19aa6` | `libiconv-1.19` | 署名 good（Bruno Haible） |
| ncurses（候補。§4） | 6.6 | `ncurses-6.6.tar.gz` | 3791150 | `355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11` | `ncurses-6.6` | 署名 good（Thomas E. Dickey） |

**bash公式パッチ**: `bash-5.3` にはupstreamの公式パッチ `bash53-001`〜`bash53-020`（2026-09-23時点）がある。
`build/distfiles/bash-5.3-patches/` に20件と `.sig` を取得し、20件すべての署名がgood（GNU keyringのChet Rameyの鍵）。
20件の連結は47,814 byte、各ファイルの `sha256sum` 出力をさらにSHA-256した値は
`51df03a9d332cc2a069701a0c786b01632b1726a106a1af7a13cf6210c205e64`。展開した `bash-5.3` に `patch -p0` で
順に当てると全件適用でき、`patchlevel.h` の `PATCHLEVEL` が20になることを確かめた。
**注意**: 公式パッチは `patch -p0` 形式（`../bash-5.3/jobs.c` と `jobs.c`）であり、`archive.sh extract` が使う
`patch -p1` では当たらない（`bash53-001` で "No file to patch" を確認）。p006では、20件を `-p1` 形式へ
変換したパッチを `packages/shell/bash/patches/` に置く（内容は変えない）。

### 1.3 検証の強さ

| 強さ | 対象 | 内容 |
| --- | --- | --- |
| 署名（公式配布元の鍵） | bash、coreutils、wget、emacs、binutils、gmp、mpfr、mpc、gcc、gdb、gperf、libiconv、ncurses | `https://ftp.gnu.org/gnu/gnu-keyring.gpg`（HTTPSで取得）の鍵で検証 |
| 署名（公式配布元の鍵） | curl、ffmpeg | curlは `https://daniel.haxx.se/mykey.asc`、FFmpegは `https://ffmpeg.org/ffmpeg-devel.asc` の鍵で検証 |
| 署名（keyserverの鍵） | zlib、expat、pcre2、libjpeg-turbo、wayland、wayland-protocols、libtiff（keys.openpgp.org）、git（sha256sums.asc）、meson、freetype、xkeyboard-config、Debian ca-certificates（keyserver.ubuntu.com） | 署名の発行者fingerprintで鍵を引いた。**鍵が本物であることを、配布元が公開するfingerprintと独立に照合してはいない**。「そのfingerprintの持ち主が署名した」ことまでを示す |
| 同一配布元の公開digest | glib、pango、gdk-pixbuf、gtk4、gtk3、graphene、libepoxy、at-spi2-core、cairo、fontconfig、pixman、Qt6/Qt5、VLC、curlのPEM | 同じサーバの `.sha256sum` 等と一致。改竄された配布元は検出できない |
| GitHub assetのdigest | libffi、harfbuzz、fribidi | GitHub APIがassetに付ける `digest`（`sha256:…`）と一致 |
| 独立照合のみ | vim、ninja、libxkbcommon（GitHubのtag archive） | `git get-tar-commit-id` でtar内のcommit IDを読み、`git ls-remote` のtag commitと一致を確かめた。GitHubのarchiveはbyte単位の安定性がGitHubの運用に依存する |
| 独立照合のみ | isl 0.24 | gcc.gnu.orgとlibisl.sourceforge.ioの2系統で同じSHA-256 |
| 独立照合のみ | libpng 1.6.58 | SourceForgeのRSSが公開するMD5が一致。さらに展開内容をgit tag `v1.6.58` のarchiveと `diff -r` して差分0件 |
| **未検証** | VLCの署名 | `.asc` の発行者は `A341FD7684DBC1FFD69BC25A1D77C0AE835B911E`（署名作成 2026-09-22）。keys.openpgp.org・keyserver.ubuntu.comともに該当鍵が無く、VideoLANのサイトからも取得できなかった。`.asc` は `build/distfiles/` に保存してある。公開SHA-256（同一配布元）とは一致 |

署名に使った鍵のfingerprint（全40桁）は `distfiles-measured.json` にある。

### 1.4 取得しなかったもの

| 対象 | 理由 |
| --- | --- |
| Rust | Phase指示どおり版だけ記録する（§9）。取得はp014で行う |
| Chromiumの依存（NSS、NSPR等） | Phase指示どおり名前と入手元だけ（§9） |
| FFmpeg 9.0.2 | 一度取得したが、VLC 3.0.24と組み合わせられない（§2.9）ため不採用とし、`build/distfiles` から削除した。`fetch.log` に取得の記録が残る |

## 2. パッケージ別

表の「理由」は版を選んだ理由。「置き場所」は `userland/packages/` からの相対パス。
build系の略: AC＝autoconf（`configure`）、LT＝libtool使用、CM＝CMake、MS＝meson、独自＝独自の構成スクリプト。

### 2.1 基本コマンド・エディタ

| パッケージ | 版・理由 | 入手元 | ライセンス（SPDX） | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| bash | 5.3＋公式パッチ20（5.3.20相当）。5系の最新。ws.mdの「5.x」に合う | `https://ftp.gnu.org/gnu/bash/bash-5.3.tar.gz`、パッチ `…/bash-5.3-patches/bash53-0NN` | GPL-3.0-or-later | AC | `shell/bash` |
| coreutils | 9.12。最新安定版（ws.mdの「9.x」） | `https://ftp.gnu.org/gnu/coreutils/coreutils-9.12.tar.xz` | GPL-3.0-or-later | AC（gnulib） | `utils/coreutils` |
| vim | 9.2.1125。vimは9.2系をパッチ番号のtagで出しており、取得時点の最新tag。**ws.mdは「vim 9.1」と書いているが、9.2が安定系になっている** | `https://github.com/vim/vim/archive/refs/tags/v9.2.1125.tar.gz` | Vim | AC（`src/configure`） | `editors/vim` |
| emacs | 31.1。最新安定版。**ws.mdは「emacs 30.x」と書いているが、31.1が出ている**。30.2を選ぶ理由は見当たらない | `https://ftp.gnu.org/gnu/emacs/emacs-31.1.tar.xz` | GPL-3.0-or-later | AC | `editors/emacs` |

既知のクロスbuild問題:

- **bash**: 23項目の実行時判定が、cross時に既定値へ落ちる（`cannot check … if cross compiling`。例: named pipes→missing、
  `getcwd` の確保→no、`WCONTINUED`→no、job control関連、`sys_siglist`）。`bash_cv_*` をcacheで与える
  （値は根拠付きでphase docに残す）。readlineは同梱版を使う。termcapライブラリが無い場合は同梱のGNU termcap
  （`lib/termcap`）が使われるので、baseのcurses不足（§3.8）の影響を受けない。`--without-bash-malloc` を検討する。
  公式パッチの `-p0` 問題は§1.2。
- **coreutils**: gnulibの置換が多数働く。`--disable-acl --disable-xattr --disable-libcap --without-selinux
  --without-openssl --disable-nls` から始める。manページはcross時に `help2man` で生成できないため、同梱の
  ダミーになる（hostにhelp2manは無い）。
- **vim**: cross時に `vim_cv_toupper_broken`・`vim_cv_terminfo`・`vim_cv_tgetent`・`vim_cv_getcwd_broken`・
  `vim_cv_timer_create_works`・`vim_cv_stat_ignores_slash`・`vim_cv_memmove_handles_overlap`・`vim_cv_uname_*` を
  与える必要がある（`src/configure.ac` の警告文で確認）。**`tgetent` 等のtermcap APIが必要**で、baseのcursesには無い（§3.8）。
- **emacs**: (1) `configure.ac` の `opsys` 判定がzedbsdを知らず `unported=yes` で止まる。(2) 一般のクロスbuildに
  対応していない（crossを想定するのはAndroidだけ）。buildの途中で作った `temacs` を実行してpdumpとlispの
  byte-compileを行うため、hostで同じ31.1をnative buildしてlispを先にbyte-compileし、dumpはターゲットで
  行うなどの工夫が要る（p010の最大のリスク。ws.mdの記述どおり）。(3) `tputs` が必須で、無いとconfigureが
  止まる（`The required function 'tputs' was not found`）。baseのcursesには無い（§3.8）。
  (4) `--without-x --without-gnutls --without-libgmp`（同梱mini-gmp） `--without-modules` から始める。

### 2.2 ネットワーク・CA

| パッケージ | 版・理由 | 入手元 | ライセンス（SPDX） | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| ca-certificates | curl配布の `cacert-2026-08-13.pem`（Mozillaの2026-08-13時点のroot証明書、121件）。§7 | `https://curl.se/ca/cacert-2026-08-13.pem`（公開SHA-256: 同名 `.sha256`） | MPL-2.0（証明書データ） | 無し（データ） | `security/ca-certificates` |
| zlib | 1.3.2。最新 | `https://zlib.net/zlib-1.3.2.tar.xz`（署名 `.asc`） | Zlib | 独自configure／CM | `libs/zlib` |
| expat | 2.8.5。最新（2026-09-22公開） | `https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.xz` | MIT | AC＋LT／CM | `libs/expat` |
| curl | 8.22.0。最新 | `https://curl.se/download/curl-8.22.0.tar.xz`（署名 `.asc`） | curl | AC＋LT／CM | `network/curl` |
| wget | 1.25.0。GNU Wget 1系の最新（ws.mdが指定するGPLのGNU Wget。wget2は別物） | `https://ftp.gnu.org/gnu/wget/wget-1.25.0.tar.gz` | GPL-3.0-or-later（OpenSSLとのlinkを許す追加許可付き） | AC（gnulib） | `network/wget` |
| git | 2.55.0。最新 | `https://mirrors.edge.kernel.org/pub/software/scm/git/git-2.55.0.tar.xz`（`sha256sums.asc`） | GPL-2.0-only（一部LGPL-2.1等） | 独自Makefile（＋AC／MS） | `development/git` |

既知のクロスbuild問題:

- **zlib**: 独自configureは `uname` で共有ライブラリの作り方を決め、`version-script` も使う。CMake（WS032の
  toolchain file）を使う方が素直。symbol versioningを無効にする（§3.3）。
- **expat**: libtoolを使うautotoolsでは共有ライブラリが作られない（§3.2）。CMakeを使う。
- **curl**: **libpslが既定で必須**で、見つからないと `libpsl libs and/or directories were not found!` で止まる。
  `--without-libpsl` にするか、libpslを加えるかを決める（§4）。cross時はCA bundleの場所を自動検出しないので
  `--with-ca-bundle=/etc/ssl/cert.pem`（§7）を明示する。CMakeもある（libtool問題を避けられる）。
- **wget**: `--with-ssl=openssl`（GnuTLSは使わない）。**IRI（国際化ドメイン）はiconvが必要**で、無い場合は
  自動で無効になる。libpsl・libidn2・pcre2・libuuidは任意（`--without-libpsl --disable-iri` 等から始める）。
- **git**: **2.55からRust部分が既定で有効**（`Documentation/RelNotes/2.55.0.adoc`）。`NO_RUST=1` で外せる。
  さらに `Documentation/BreakingChanges.adoc` は **Git 3.0でRustを必須にする**と予告している（§10）。
  ほかに `NO_PERL NO_PYTHON NO_TCLTK NO_GETTEXT`、iconvが無いので `NO_ICONV`（またはlibcへiconvを入れる。§3.7）。
  `uname_S` 等によるOS判定（`config.mak.uname`）にzedBSDの節が無いので、make変数で全部与えるか節を足す。

### 2.3 ツールチェイン（gcc 16・binutils・gdb）

| パッケージ | 版・理由 | 入手元 | ライセンス（SPDX） | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| binutils | 2.47。最新 | `https://ftp.gnu.org/gnu/binutils/binutils-2.47.tar.xz` | GPL-3.0-or-later（libiberty等はLGPL-2.1-or-later） | AC | `development/binutils`（ws.mdの既定案どおり） |
| GMP | 6.3.0。最新。gcc 16.2.0の `contrib/download_prerequisites` と同じ版 | `https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz` | LGPL-3.0-or-later OR GPL-2.0-or-later | AC＋LT | `libs/gmp` |
| MPFR | 4.2.2。最新。gccの指定と同じ | `https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz` | LGPL-3.0-or-later | AC＋LT | `libs/mpfr` |
| MPC | 1.3.1。最新。gccの指定と同じ | `https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz` | LGPL-3.0-or-later | AC＋LT | `libs/mpc` |
| ISL | 0.24。**最新は0.28**だが、gcc 16.2.0の `download_prerequisites` が0.24を指定しているので、gccが試験している版に揃える | `https://gcc.gnu.org/pub/gcc/infrastructure/isl-0.24.tar.bz2` | MIT | AC＋LT | `libs/isl` |
| gcc | 16.2.0。16系の最新（16.1.0の次） | `https://ftp.gnu.org/gnu/gcc/gcc-16.2.0/gcc-16.2.0.tar.xz` | GPL-3.0-or-later（runtimeは GPL-3.0-or-later WITH GCC-exception-3.1） | AC（独自のtop-level） | `lang/gcc-16` |
| gdb | 17.2。最新（ws.mdの「16/17」） | `https://ftp.gnu.org/gnu/gdb/gdb-17.2.tar.xz` | GPL-3.0-or-later | AC | `development/gdb` |

既知のクロスbuild問題:

- 全件、`config.sub` が `x86_64-unknown-zedbsd` を拒否する（§3.1）。
- **binutils/gcc/gdb**: config.subに加えて、`bfd/config.bfd`・`ld/configure.tgt`・`gcc/config.gcc`・
  `libgcc/config.host`・`libstdc++-v3/configure.host`・gdbのnative target（`gdb/configure.nat` 等）へzedbsdを
  加えるパッチが要る（ws.mdの記述どおり）。gdbは **GMPが必須**（MPFRは任意）、XML target description用に
  expat、readline（同梱）のためにtermcap APIが要る（§3.8）。gdbはC++17が必要で、WS032のlibc++で
  buildするかgcc 16のlibstdc++でbuildするかを決める。
- **GMP/MPFR/MPC/ISL**: libtoolの共有ライブラリ問題（§3.2）がある。gccとgdbへは静的にlinkするのが普通なので、
  **静的ライブラリだけ作れば足りる**（共有版は不要）。p012のbuild機上のクロスgccには、これらのbuild機用
  （host用）の版が要る。gccのtopへsourceを置く方式（`download_prerequisites` と同じ配置）なら、同じdistfileから
  gccのbuildの中で作れる。
- **gcc**: `download_prerequisites` の `gettext-0.22` は任意（NLSを使わなければ不要）。gfortranの
  libquadmath・libgfortranはgccのtarballに含まれる。

### 2.4 build系ツール（host上で動かすもの）

| パッケージ | 版・理由 | 入手元 | ライセンス | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| meson | 1.12.0。最新。**hostのmeson（Debian 13の1.7.0）では足りない**: GTK 4.24が `>= 1.8.0`、fontconfig 2.18.3が `>= 1.11.0` を要求する | `https://github.com/mesonbuild/meson/releases/download/1.12.0/meson-1.12.0.tar.gz`（署名 `.asc`） | Apache-2.0 | python（installせず `meson.py` を直接実行できる） | §8の案: `tools/host/meson` |
| ninja | 1.13.2。最新。hostの1.12.1でも足りる見込みだが、版を固定するなら使う | `https://github.com/ninja-build/ninja/archive/refs/tags/v1.13.2.tar.gz` | Apache-2.0 | CM／`configure.py` | §8の案: `tools/host/ninja` |

### 2.5 GLibとフォント

| パッケージ | 版・理由 | 入手元 | ライセンス（SPDX） | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| libffi | 3.8.0。最新 | `https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz` | MIT | AC＋LT | `libs/libffi` |
| pcre2 | 10.48。最新 | `https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.48/pcre2-10.48.tar.bz2`（署名 `.sig`） | BSD-3-Clause WITH PCRE2-exception | AC＋LT／CM | `libs/pcre2` |
| glib | 2.90.0。**GTK 4.24.0が `glib >= 2.89.3` を要求する**ので2.90系が必要（2.88系では足りない）。2.90.0はその最初の安定版 | `https://download.gnome.org/sources/glib/2.90/glib-2.90.0.tar.xz` | LGPL-2.1-or-later | MS（>= 1.4.0） | `libs/glib` |
| libpng | 1.6.58。最新（1.7はbeta） | `https://download.sourceforge.net/libpng/libpng-1.6.58.tar.xz` | Libpng-2.0 | AC＋LT／CM | `libs/libpng` |
| freetype | 2.14.3。最新 | `https://download.savannah.gnu.org/releases/freetype/freetype-2.14.3.tar.xz`（署名 `.sig`） | FTL OR GPL-2.0-or-later | MS／CM／AC | `libs/freetype` |
| harfbuzz | 14.5.0。最新 | `https://github.com/harfbuzz/harfbuzz/releases/download/14.5.0/harfbuzz-14.5.0.tar.xz` | MIT（"Old MIT"） | MS／CM | `libs/harfbuzz` |
| fontconfig | 2.18.3。最新（2.17以降はfreedesktop.orgのGitLab package registryで配布） | `https://gitlab.freedesktop.org/api/v4/projects/890/packages/generic/fontconfig/2.18.3/fontconfig-2.18.3.tar.xz` | MIT系の独自表記（HPND-sell-variant相当。SPDX名はp027で機械確認して確定） | MS（>= 1.11.0）／AC | `libs/fontconfig` |

既知のクロスbuild問題:

- **glib**: **iconvが必須**（`dependency('iconv')`）。zedBSDのlibcに `iconv.h`・`iconv_open` は無い（§3.7）。
  libintlはlibcを先に調べ、無ければ `proxy-libintl` subprojectへ落ちる。zedBSDのlibcには `libintl.h`
  （`dcgettext` 等）がある。glibのsubproject（libffi、pcre2、zlib、proxy-libintl等）はwrapで**ネットワークから
  取得しようとする**ので、`--wrap-mode=nodownload` で禁止する（§8）。
- **libffi**: autotoolsだけ（CMake・mesonが無い）で、libtoolの共有ライブラリ問題（§3.2）がそのまま当たる。
  version script（`libffi.map`）を使うので `--disable-symvers` 等で無効にする（§3.3）。
- **freetype**: 任意依存のharfbuzzと相互依存になる。まずharfbuzz無しのfreetypeを作り、harfbuzzを作る
  （freetypeをharfbuzz付きで作り直すかはp027で決める）。
- **harfbuzz**: pangoは `harfbuzz-gobject` を使うため、**harfbuzzはglib（gobject）付きで作る**必要がある。
- **fontconfig**: **host toolのgperfが必須**（`find_program('gperf')`）。hostには無く、package導入は許可外なので
  gperfをsourceからhost用にbuildする（§4）。wrapの `gperf.wrap` はネットワーク取得なので使わない。XMLは
  expat（p021）を使う。

### 2.6 本家libwayland

| パッケージ | 版・理由 | 入手元 | ライセンス | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| wayland | 1.26.0。最新安定版（1.25.91はRC） | `https://gitlab.freedesktop.org/-/project/121/uploads/5f5d2e19a230e8b8250d9a7c2f280846/wayland-1.26.0.tar.xz`（署名・`.sha256sum`） | MIT | MS（>= 0.64.0） | `desktop/libwayland` |
| wayland-protocols | 1.49。最新。GTK 4.24が `>= 1.48` を要求 | `https://gitlab.freedesktop.org/-/project/2891/uploads/7ed597f0cad076a17fe36f8860596f8c/wayland-protocols-1.49.tar.xz` | MIT | MS | `desktop/wayland-protocols`（別tarballなので別ディレクトリ案。`desktop/libwayland` の中で2つ目の `ZEDBSD_EXTERNAL_SOURCE` を呼ぶ案もある） |

既知のクロスbuild問題（**重要**）:

- **ライブラリのbuildに `sys/signalfd.h`（`SFD_CLOEXEC`）、`sys/timerfd.h`（`TFD_CLOEXEC`）、epoll が必須**
  （`meson.build` で無ければ `error('… is needed to compile Wayland libraries')`）。FreeBSDとOpenBSDは
  epoll-shim（kqueue上のwrapper）で補う。**zedBSDのlibcにはどれも無い**（`include/libc/sys/` に
  `epoll.h`・`signalfd.h`・`timerfd.h`・`event.h` が無い）。`libraries` optionはclientとserverを一緒に作るので、
  client だけが欲しい場合もこのcheckに当たる。対応は、(a) libc・カーネルにepoll/timerfd/signalfd相当を
  加える（p005）、(b) event loopをpoll等へ置き換えるパッチ（serverのevent loopはzdesktopが使うかで重さが変わる）、
  (c) epoll-shim相当をzedBSD向けに用意する、のいずれかで、**人間の判断が要る**（§10）。
- cross buildでは **同じ版（1.26.0）のnativeの `wayland-scanner`** を要求する
  （`dependency('wayland-scanner', native: true, version: meson.project_version())`）。hostのは1.23.1なので、
  同じtarballからhost用scannerを先にbuildする（`-Dlibraries=false`。hostのexpat 2.8.2で足りる）。
- scannerのDTD検証はlibxml2を使う（`dtd_validation`）。ターゲット用は `-Ddtd_validation=false` でよい。
- SONAMEは `libwayland-client.so.0`（ws.mdの記述どおり）。symlinkを含むinstall形がimageへ載るかは§3.4。

### 2.7 描画系

| パッケージ | 版・理由 | 入手元 | ライセンス（SPDX） | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| pixman | 0.46.4。最新 | `https://cairographics.org/releases/pixman-0.46.4.tar.xz`（`.sha512`） | MIT | MS | `libs/pixman` |
| cairo | 1.18.6。最新。GTK 4.24が `>= 1.18.2` | `https://cairographics.org/releases/cairo-1.18.6.tar.xz`（`.sha256sum`、その `.asc` もある） | LGPL-2.1-only OR MPL-1.1 | MS | `libs/cairo` |
| pango | 1.58.2。1.58系（安定）の最新。GTK 4.24が `>= 1.58`。1.90.0はPango 2の古い試作版で対象外 | `https://download.gnome.org/sources/pango/1.58/pango-1.58.2.tar.xz` | LGPL-2.1-or-later | MS | `libs/pango` |
| fribidi | 1.0.17。最新 | `https://github.com/fribidi/fribidi/releases/download/v1.0.17/fribidi-1.0.17.tar.xz` | LGPL-2.1-or-later | MS／AC | `libs/fribidi` |
| gdk-pixbuf | 2.44.8。最新 | `https://download.gnome.org/sources/gdk-pixbuf/2.44/gdk-pixbuf-2.44.8.tar.xz` | LGPL-2.1-or-later | MS | `libs/gdk-pixbuf` |
| libjpeg-turbo | 3.2.0。最新安定版（3.1.90はbeta） | `https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz`（署名 `.sig`） | IJG AND BSD-3-Clause AND Zlib | CM | `libs/libjpeg-turbo` |
| graphene | 1.10.8。最新（2022年から更新なし） | `https://download.gnome.org/sources/graphene/1.10/graphene-1.10.8.tar.xz` | MIT | MS | `libs/graphene` |
| libepoxy | 1.5.10。最新（2022年から更新なし） | `https://download.gnome.org/sources/libepoxy/1.5/libepoxy-1.5.10.tar.xz` | MIT | MS | `libs/libepoxy` |
| libxkbcommon | 1.13.2。安定版の最新（1.14.0はbeta）。1.8以降release tarballを配らず、git tagのarchiveを使う | `https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-1.13.2.tar.gz` | MIT（一部X11系。`LICENSE` に一覧） | MS | `libs/libxkbcommon` |
| xkeyboard-config | 2.48。最新 | `https://www.x.org/releases/individual/data/xkeyboard-config/xkeyboard-config-2.48.tar.xz`（署名 `.sig`） | MIT系（HPND等。meson.buildは `MIT/Expat`） | MS | `libs/xkeyboard-config`（データだけのpackage） |
| libtiff（追加） | 4.7.2。最新。GTK 4.24が必須にしている（§4） | `https://download.osgeo.org/libtiff/tiff-4.7.2.tar.xz`（署名 `.sig`） | libtiff | CM／AC | `libs/libtiff` |

既知のクロスbuild問題:

- **libepoxy**: GL/EGLを実行時に `dlopen` する。zedBSDにはEGL・GLが無い（WS030でEGLは取り消し）。
  `-Degl=no -Dglx=no` 相当で作るか、作ったうえで実行時に失敗させる。GTK4はlibepoxyの `epoxy_has_egl` を見て
  EGLの有無を決める。
- **libxkbcommon**: `xkbregistry` は既定で有効でlibxml2が要る（`-Denable-xkbregistry=false` で外す）。
  X11・Wayland用の道具は `-Denable-x11=false -Denable-wayland=false` で外す。keymapの読込先（xkeyboard-configの
  置き場所）はbuild時に決まるので、imageでの置き場所（例 `/usr/share/X11/xkb`）と揃える。
  `meson.build` がversion scriptを使う（§3.3）。
- **gdk-pixbuf**: `glycin`（外部loader）はLinuxのときだけ既定で有効になる（zedbsdでは無効）。`-Dman=false` にする
  （rst2manがhostに要る）。libtiffは任意。
- **libjpeg-turbo**: SIMDはhostのnasmでbuildする（hostにnasmあり）。version scriptを使う（§3.3）。
- **cairo**: `cairo-gobject`（GTKが必須）のためにglibを使う。X11・XCB・quartz等は無効にする。

### 2.8 GUI toolkit

| パッケージ | 版・理由 | 入手元 | ライセンス（SPDX） | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| GTK4 | 4.24.0。最新安定版（GNOMEの偶数minorが安定） | `https://download.gnome.org/sources/gtk/4.24/gtk-4.24.0.tar.xz` | LGPL-2.1-or-later | MS（>= 1.8.0） | `desktop/gtk4` |
| Qt6 qtbase・qtwayland | 6.11.2。最新（LTSは6.8系だが、新規の移植先には最新を採る） | `https://download.qt.io/official_releases/qt/6.11/6.11.2/submodules/qtbase-everywhere-src-6.11.2.tar.xz`、同 `qtwayland-…`（各 `.sha256`） | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only（一部toolはGPL-3.0-only WITH Qt-GPL-exception-1.0） | CM（>= 3.22） | `desktop/qt6` |
| GTK3 | 3.24.52。3.24系の最新 | `https://download.gnome.org/sources/gtk/3.24/gtk-3.24.52.tar.xz` | LGPL-2.1-or-later | MS | `desktop/gtk3` |
| atk | 2つの候補（§10）。(a) at-spi2-core 2.62.0.1 の `-Datk_only=true`（現行のsource。ただしlibxml2が要る。下記）、(b) 単体atkの最終版2.38.0（2022年。glibだけで作れる）。**単体のatkは2.38.0で終わり、以後はat-spi2-coreに統合された** | (a) `https://download.gnome.org/sources/at-spi2-core/2.62/at-spi2-core-2.62.0.1.tar.xz`、(b) `https://download.gnome.org/sources/atk/2.38/atk-2.38.0.tar.xz` | LGPL-2.1-or-later（(b)のCOPYINGはLGPL-2.0） | MS | `libs/atk` |
| Qt5 qtbase・qtwayland・qtsvg | 5.15.19。archiveに公開された5.15系open sourceの最新 | `https://download.qt.io/archive/qt/5.15/5.15.19/submodules/qtbase-everywhere-opensource-src-5.15.19.tar.xz`、同 `qtwayland-…`、`qtsvg-…` | LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only | qmake（独自configure） | `desktop/qt5` |

既知のクロスbuild問題:

- **GTK4**: 必須依存に **libtiff**（`dependency('libtiff-4', 'tiff')` がrequired）と `harfbuzz-subset`、
  `cairo-gobject`、`wayland-egl`（libwaylandが作る）がある。Vulkan rendererはhostの `glslc` が要る（hostにあり）。
  zedBSDのlibvulkan（WS030）を `vulkan` として見せるpkg-configが要る。`libdrm` はLinuxだけ必須。
  hostの `glib-compile-resources`（2.84.4）を使う。GLのcontextはEGLが無いので作れず、GSKはVulkanかcairoを使う
  （ws.mdの記述どおり）。meson >= 1.8.0。
- **Qt6**: cross buildには **同じ版のhost用Qt**（`QT_HOST_PATH`）が必要（無いと
  `You need to set QT_HOST_PATH to cross compile Qt.`）。qtbaseとqtwaylandをhost向けに先にbuildする。
  zlib・pcre2・freetype・harfbuzz・libpng・libjpegは同梱版（`src/3rdparty`）を使える。fontconfigは同梱されない。
  Wayland QPAはxkbcommonが要る。未知のOSなので、mkspec（`mkspecs/`）とCMakeのplatform判定にzedBSDを加える
  パッチが要る見込み。ELFのsymbol version tagging（`Qt_6`）を無効にする（§3.3）。
- **GTK3**: atk（`>= 2.35.1`）が必須。`atk-bridge`（at-spi2-atk、dbus）はX11 backendだけ。
  `-Dx11_backend=false -Dwayland_backend=true`。Wayland backendは `wayland-egl` が要る。
- **Qt5**: qmakeで `-xplatform`（zedBSD用mkspecの追加）。host用のqmake・moc・rcc・uicは同じbuildの中で作られる。
  qtwaylandはhost用の `qtwaylandscanner` を作る。5.15は古く、新しいclang（WS032のLLVM 23）で警告・エラーが
  出る可能性がある。
- **at-spi2-core**: `atk_only` でなければdbus-1が必須。`-Datk_only=true` にしてもdbusは外れるが、
  `dependency('libxml-2.0')`（`meson.build` 154行）が条件の外にあって**libxml2が必須のまま**である（使うのは試験だけ）。
  また `atk_only` はoption説明で "UNSUPPORTED" とされている。1行のパッチでlibxml2を条件内へ移すか、
  単体のatk 2.38.0（glibとgobjectだけに依存）を使うかを決める（§10）。

### 2.9 マルチメディア

| パッケージ | 版・理由 | 入手元 | ライセンス（SPDX） | build系 | 置き場所 |
| --- | --- | --- | --- | --- | --- |
| FFmpeg | 8.1.3。**最新は9.0.2だが、VLC 3.0.24は8.1と組み合わせる前提**（VLCのNEWS: "Use FFmpeg 8.1 (upgraded from 4.4)"、"Prepare compatibility for … FFmpeg8"）。9.0はlibavcodecのmajorが63に上がる（8.1は62） | `https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz`（署名 `.asc`） | LGPL-2.1-or-later（`--enable-gpl`/`--enable-nonfree` を使わない構成） | 独自configure | `multimedia/ffmpeg` |
| VLC | 3.0.24。最新（4.0は未リリース） | `https://download.videolan.org/pub/videolan/vlc/3.0.24/vlc-3.0.24.tar.xz`（`.sha256`、`.asc`） | LGPL-2.1-or-later（libvlc・libvlccore・多くのmodule）、GPL-2.0-or-later（Qt GUI moduleを含む一部module） | AC＋LT | `multimedia/vlc` |

既知のクロスbuild問題:

- **FFmpeg**: 独自configureで `--enable-cross-compile --target-os=…` を使うが、zedbsdは知られていない。
  configureへ加えるか、近いOS指定で代用するかを決める。x86のasmはhostのnasm（あり）か `--disable-x86asm`。
  version scriptを使う（§3.3）。
- **VLC**: (1) **Qt GUIは `Qt5Core Qt5Widgets Qt5Gui Qt5Svg` を要求**する（`configure.ac` 3922行）。qtsvgは
  Phase表に無かった（§4）。(2) moduleはlibtoolで作る共有objectなので、libtoolの問題（§3.2）が必ず当たる。
  (3) Luaは既定で有効（`--disable-lua` で外せる）。(4) gettextは `--disable-nls`。iconvは任意（無いと字幕の
  文字コード変換が効かない）。(5) **Wayland出力は「Incomplete Wayland support (default disabled)」**で、
  `--enable-wayland` が要る。VLC 3のQt GUIへの映像埋め込みはX11前提なので、Wayland上では映像を別windowに出す
  形になる見込み。(6) 音声はOSS（`/dev/dsp`、WS035の音声フレームワーク）とpulse（WS035-p019の互換libpulse）。

## 3. 横断的な問題（クロスbuild契約に効くもの）

### 3.1 config.subがzedbsdを知らない

展開した21個の `config.sub`（bash、binutils、coreutils、curl、emacs×2、expat、fontconfig、fribidi、gcc、gdb、
gmp、isl×2、libffi、libpng、mpc、mpfr、pcre2、vlc、wget）に `x86_64-unknown-zedbsd` を与えると、全件
`Invalid configuration … OS 'zedbsd' not recognized` になった。WS032のOpenSSHはパッケージごとのパッチ
（`0001-recognise-the-zedbsd-target.patch`）で対応している。autotoolsのpackageが十数件あるので、共通の方法を
p025（またはp021）で決める。案:
(a) パッケージごとのパッチ（現行。件数分の重複）、(b) `external.mk` の展開後処理で、tree内の全 `config.sub` の
OS一覧へ `zedbsd*` を足す小さなZlibのscriptを当てる（GPLのconfig.subをツリーに持たない）、
(c) GNU config（`config.git`）へzedbsdを登録してもらう（将来のtarballが自然に通る）。

### 3.2 libtoolが未知のOSで共有ライブラリを作らない

libtoolを使うconfigure（expatで確認）は、未知の `host_os` を `*) dynamic_linker=no` に落とし、
**共有ライブラリを作らない**（静的だけになる）。該当: expat、libffi、pcre2、libpng、freetype（AC）、fontconfig（AC）、
fribidi（AC）、curl（AC）、gmp、mpfr、mpc、isl、libtiff（AC）、libiconv、VLC。対応案:
(a) CMake・mesonのあるpackageはそちらを使う（zlib、expat、curl、pcre2、libpng、libjpeg-turbo、libtiff、freetype、
fontconfig、fribidi）、(b) GMP/MPFR/MPC/ISLは静的だけで足りる（§2.3）、(c) **libffiとVLCはautotoolsしか無い**ので、
`libtool.m4`（生成済み `configure` の該当節）へzedbsdを加えるパッチが要る。(c)は共通化（§3.1と同じ仕組み）を検討する。

### 3.3 symbol versioningの禁止

WS032の `tools/build/check-dynamic-elf.py` は、共有ライブラリと実行ファイルに `.gnu.version_d`/`.gnu.version_r`
があると失敗させる（rtldが扱わないため。external-design §2.1）。version scriptを明示的に使うもの:
zlib、expat、curl、libffi、pcre2、libpng、libjpeg-turbo、libxkbcommon、FFmpeg、VLC、Qt5（`qt_module.prf`）、
Qt6（`configure.cmake`）。各Phaseで無効化の方法（configure option、cache、パッチ）を決め、生成物を
`check-dynamic-elf.py` で確かめる。

### 3.4 SONAMEの版番号とimageへの載せ方

WS032のOpenSSLは版の無いSONAME（`libcrypto.so`）で `/lib` に入れている。GLib系・Wayland・Qt等は
`libglib-2.0.so.0`、`libwayland-client.so.0`、`libQt6Core.so.6` のような版付きSONAMEと、`libfoo.so` への
symlinkをinstallする。rtldは `DT_NEEDED` の名前でファイルを探すので、imageには**SONAMEの名前の実体**を置く
必要がある。image作成（`ZEDBSD_PACKAGE_FILES`）がsymlinkを扱えるか、実体を複数名で置くかをp015（または
最初の共有ライブラリpackageのp021）で決める。`/usr/lib` を `/lib` より先に探す変更（p007）とも関わる。

### 3.5 依存packageのheaderとライブラリの置き場所（pkg-config）

WS032の契約では、OpenSSHがOpenSSLのstage（`build/packages/openssl/stage/usr`）を直接 `--with-ssl-dir` と
`LDFLAGS` で指している。GTK4は約25の依存を `pkg-config`（mesonの `dependency()`）で探すので、この方式は
続かない。**p025で次を契約に加える**（§8）:
- 依存packageのstageをまとめて見せる「package prefix」（例 `build/<plat>/packages/prefix/usr`）へ、各packageの
  stageを集める（またはsearch pathを連ねる）。
- `zedbsd-pkg-config` wrapper（`PKG_CONFIG_LIBDIR` をprefixとsysrootの `.pc` だけに限り、
  `PKG_CONFIG_SYSROOT_DIR` を設定し、hostの `.pc` を読ませない）。autoconf・CMake・mesonの全部に渡す。
- zedBSDのlibvulkan（`/lib/libvulkan.so`、WS030）とlibc側のものを見せる `.pc` を用意する。

### 3.6 host上のbuild道具（同じ版が要るもの）

| 道具 | 要る理由 | hostの現状 |
| --- | --- | --- |
| meson 1.12.0 | GTK4（>= 1.8.0）、fontconfig（>= 1.11.0） | 1.7.0で足りない |
| gperf | fontconfigが必須 | 無い（package導入は許可外なのでsourceからbuild） |
| wayland-scanner 1.26.0 | libwaylandのcross buildが同じ版を要求 | 1.23.1 |
| host用Qt 6.11.2 | Qt6のcross buildの `QT_HOST_PATH` | 無い |
| host用emacs 31.1 | lispのbyte-compile（§2.1） | 無い |
| build機上のクロスbinutils・gcc | p012、p022 | 無い |
| glib-compile-resources等 | GTKが使う | 2.84.4（使える見込み。合わなければ同じtarballからhost用を作る） |
| nasm、glslc、python3、perl、bison、flex、pkg-config、cmake 3.31.6、ninja 1.12.1 | 各package | あり |

### 3.7 libcにiconvが無い

`include/libc/` に `iconv.h` が無く、libcに `iconv_open` の実装も無い（`userland/base/iconv` はコマンド）。
glibは必須、wgetのIRI・gitの文字コード変換・VLCの字幕・gdbの文字集合で使う。選択肢:
(a) libcにPOSIXの `iconv` を実装する（p005。base＝Zlib独自実装。WSの方針「原因側で直す」に合う）、
(b) GNU libiconv（LGPL-2.1-or-later、取得済み）をpackage `libs/libiconv` として入れる。**人間の判断が要る**（§10）。

### 3.8 termcap API（`tgetent`・`tgetstr`・`tgoto`・`tputs`）が無い

baseのcurses（`include/libc/curses.h`、`userland/base/curses`、静的な `/lib/libcurses.a`）は `setupterm`・
`tigetstr`・`putp` 等はあるが、termcap系の `tgetent`・`tgetstr`・`tgoto`・`tputs`（と `tparm`）が無い。
vim（`tgetent` 必須）、emacs（`tputs` 必須）、gdbのreadline、（bashは同梱termcapで回避可）が使う。選択肢:
(a) baseのcursesにtermcap互換APIを加える（p005、Zlib独自実装）、(b) ncurses（MIT系、取得済み）を
`libs/ncurses` として入れる。**人間の判断が要る**（§10）。

### 3.9 取得のオフライン性

meson（glib、fontconfig等の `subprojects/*.wrap`）は依存が見つからないとネットワークから取得しようとする。
WS032の「通常のbuildは暗黙にネットワークI/Oを起こさない」方針に合わせ、mesonには常に
`--wrap-mode=nodownload` を渡す（§8）。

## 4. Phase表に無かった依存（追加候補）

| 依存 | 必要とする側 | 必須か | 取得 | 案 |
| --- | --- | --- | --- | --- |
| libtiff 4.7.2 | GTK4（required） | 必須 | 済み | p028（描画系）の範囲へ加える。`libs/libtiff` |
| gperf 3.3（host tool） | fontconfig | 必須 | 済み | p025（build道具）かp027でhost用にbuild。`tools/host/gperf` |
| Qt5 qtsvg 5.15.19 | VLCのQt GUI | GUIには必須 | 済み | p032（Qt5）の範囲へ加える |
| iconv | glib（必須）ほか | 必須 | libiconv 1.19は済み | libcへの実装（p005）かlibiconv package。§3.7 |
| termcap API | vim・emacs（必須）、gdb | 必須 | ncurses 6.6は済み | baseのcurses拡張（p005）かncurses package。§3.8 |
| epoll・timerfd・signalfd | libwayland（必須） | 必須 | ― | libc・カーネル（p005）か、libwaylandへのパッチ。§2.6 |
| 同じ版のwayland-scanner（host） | libwayland | 必須 | 同じtarball | p034の中でhost用に作る |
| host用Qt 6.11.2 | Qt6 | 必須 | 同じtarball | p030の中でhost用に作る |
| host用emacs 31.1 | emacs | 事実上必須 | 同じtarball | p010の中で作る |
| libpsl | curl（既定で必須） | `--without-libpsl` で回避可 | 未取得 | 初期は `--without-libpsl`。入れる場合は `libs/libpsl` |
| libxml2 | libxkbcommonの `xkbregistry`、waylandのDTD検証 | optionで回避可 | 未取得 | 初期は無効 |
| Lua | VLC（既定で有効） | `--disable-lua` で回避可 | 未取得 | 初期は無効 |
| dbus | at-spi2-core | `atk_only` で回避可 | 未取得 | 使わない |
| libxml2（at-spi2-core） | at-spi2-coreの `atk_only` 構成でも必須 | パッチか単体atk 2.38.0で回避可 | 未取得（atk 2.38.0は取得済み） | §2.8、§10 |
| Rust toolchain | git 2.55（既定で有効、3.0で必須予定） | `NO_RUST` で回避可（2.x） | ― | §10 |

## 5. 依存グラフ

### 5.1 表（build時・実行時）

「build時」はそのpackageのbuildに要るもの（ターゲット用ライブラリとhost道具を分ける）。「実行時」はimageに
一緒に入っている必要があるもの。libc・rtld・カーネルは全packageの前提なので省く。

| package | build時（ターゲット用） | build時（host道具） | 実行時 |
| --- | --- | --- | --- |
| ca-certificates | ― | ― | （OpenSSL・curl・wgetが読む） |
| zlib | ― | cmake | ― |
| expat | ― | cmake | ― |
| bash | （同梱readline・termcap） | CC_FOR_BUILD | ― |
| coreutils | ― | ― | ― |
| curl | OpenSSL（WS032）、zlib | cmake／AC | libssl・libcrypto、libz、ca-certificates |
| wget | OpenSSL、zlib（任意）、iconv（IRI、任意） | ― | libssl・libcrypto、libz、ca-certificates |
| git | zlib、libcurl、expat、OpenSSL、iconv（任意） | perl不要（NO_PERL） | libcurl、libz、libexpat、ca-certificates（https）、ssh（WS032のOpenSSH、ssh URL） |
| vim | termcap API | ― | terminfoデータ |
| emacs | termcap API | host用emacs 31.1 | terminfoデータ |
| GMP | ― | m4 | （静的） |
| MPFR | GMP | ― | （静的） |
| MPC | GMP、MPFR | ― | （静的） |
| ISL | GMP | ― | （静的） |
| binutils（ターゲット上） | zlib（同梱可） | クロスgcc（p022の場合） | ― |
| gcc（build機上のクロス、p012） | zedBSDのsysroot（libcヘッダ、crt） | クロスbinutils、host用GMP/MPFR/MPC/ISL（同じdistfile） | ― |
| gcc（ターゲット上、p022） | GMP、MPFR、MPC、ISL（静的）、zlib | p012のクロスgcc | binutils（ターゲット上）、libgcc・libstdc++ |
| gfortran（p023） | p022と同じ | 同上 | libgfortran、libquadmath |
| gdb | GMP、MPFR（任意）、zlib、expat、termcap API、C++ランタイム | ― | 同左の共有物 |
| meson・ninja・gperf | ― | python3、host cc | （host上だけ） |
| libffi | ― | ― | ― |
| pcre2 | ― | cmake | ― |
| glib | libffi、pcre2、zlib、iconv、libintl（libc） | meson、python3 | libffi、libpcre2-8、libz、iconv |
| libpng | zlib | ― | libz |
| freetype | zlib、libpng（harfbuzzは任意） | meson | libz、libpng |
| harfbuzz | freetype、glib（gobject） | meson | libfreetype、libglib・libgobject |
| fontconfig | freetype、expat | meson >= 1.11、gperf | libfreetype、libexpat、フォントと `fonts.conf` |
| pixman | ― | meson | ― |
| cairo | pixman、freetype、fontconfig、libpng、zlib、glib | meson | 同左 |
| fribidi | ― | meson | ― |
| pango | glib、harfbuzz、fribidi、fontconfig、freetype、cairo | meson | 同左 |
| libjpeg-turbo | ― | cmake、nasm | ― |
| libtiff | zlib、libjpeg（任意） | cmake | libz |
| gdk-pixbuf | glib、libpng、libjpeg-turbo、libtiff（任意） | meson | 同左（loaderはbuiltin） |
| graphene | glib（gobject） | meson | libglib・libgobject |
| libepoxy | ― | meson | （EGL/GLをdlopen。zedBSDには無い） |
| xkeyboard-config | ― | meson、python3 | （データ） |
| libxkbcommon | （xkeyboard-configの置き場所） | meson、bison | xkeyboard-configのデータ |
| wayland（libwayland） | libffi、expat（scanner）、epoll・timerfd・signalfd | meson、同じ版のhost用wayland-scanner | libffi |
| wayland-protocols | ― | meson、wayland-scanner | （XMLデータ。build時だけ） |
| GTK4 | glib、pango、harfbuzz、fribidi、cairo、gdk-pixbuf、libpng、libtiff、libjpeg、graphene、libepoxy、libxkbcommon、wayland、wayland-protocols、libvulkan（WS030、任意） | meson >= 1.8、glib-compile-resources、glslc | 同左の共有物、xkeyboard-config、フォント、zdesktop（WS035-p028） |
| Qt6 | fontconfig、freetype・harfbuzz（同梱可）、libxkbcommon、wayland、C++ランタイム（WS032のlibc++） | cmake、host用Qt 6.11.2 | 同左、zdesktop（WS035-p028） |
| GTK3 | glib、pango、cairo、gdk-pixbuf、atk、libepoxy、libxkbcommon、wayland、wayland-protocols、fribidi、harfbuzz | meson | 同左、zdesktop |
| atk | glib（at-spi2-coreの場合はlibxml2も。§2.8） | meson | libglib・libgobject |
| Qt5（＋qtsvg） | fontconfig、freetype（同梱可）、libxkbcommon、wayland、C++ランタイム | qmake（buildで作る） | 同左、zdesktop |
| FFmpeg | zlib（任意） | nasm | ― |
| VLC | FFmpeg、Qt5（Core・Widgets・Gui・Svg）、libpulse（WS035-p019）、wayland（任意） | Qt5のmoc・rcc・uic | 同左、`/dev/dsp`（WS035の音声）、zdesktop |

### 5.2 図（build時の依存。→ は「右が左を使う」）

```
OpenSSL(WS032) ─┬→ curl ─→ git
ca-certificates ┤   ↑        ↑
zlib ───────────┼───┴────────┤
expat ──────────┼────────────┴→ gdb ←─ GMP ─→ MPFR ─→ MPC
                │                ↑      │  └───→ ISL
                └→ wget          │      └→ gcc(p012) ─→ gcc(p022) ─→ gfortran(p023)
termcap API(p005) ─→ vim, emacs, gdb           ↑
                                         binutils(p011)
meson/ninja/gperf(host, p025)
  └→ libffi ─┬→ glib ←─ pcre2, zlib, iconv(p005)
             │    ├→ harfbuzz ←─ freetype ←─ libpng ←─ zlib
             │    │      └──────→ fontconfig ←─ expat, gperf
             │    ├→ cairo ←─ pixman, freetype, fontconfig, libpng
             │    ├→ pango ←─ harfbuzz, fribidi, fontconfig, cairo
             │    ├→ gdk-pixbuf ←─ libpng, libjpeg-turbo, (libtiff)
             │    ├→ graphene
             │    └→ atk(at-spi2-core)
             └→ libwayland ←─ expat, epoll/timerfd/signalfd(p005)
xkeyboard-config ─→ libxkbcommon
libepoxy, libtiff, wayland-protocols
GTK4 ←─ glib, pango, cairo, gdk-pixbuf, libtiff, graphene, libepoxy, libxkbcommon, libwayland, wayland-protocols
GTK3 ←─ 同上（graphene・libtiff除く）＋ atk
Qt6  ←─ fontconfig, libxkbcommon, libwayland（freetype・harfbuzzは同梱可）
Qt5  ←─ 同上 ＋ qtsvg
FFmpeg ─→ VLC ←─ Qt5(+qtsvg), libpulse(WS035-p019), /dev/dsp(WS035)
```

## 6. Phase表との照合と修正案

`plan/ws034/ws.md` のPhase表（依存列）と§5を突き合わせた。**Phase表そのものは編集していない**（rootが直す）。
全Phase共通の「WS035のrefactor完了が前提」は変わらない。

| Phase | 現在の依存 | 修正案 | 理由 |
| --- | --- | --- | --- |
| ws034-p020 wget | p019 | p019, **p021** | zlibを使う（任意だが通常は使う）。IRIはiconvが要るので、iconvをp005で入れるならp005も |
| ws034-p009 git | p017, p021 | 変更なし。範囲に `NO_RUST=1` を明記 | git 2.55はRustを既定で有効にする。Git 3.0でRust必須の予告（§10） |
| ws034-p010 emacs | p005 | 変更なし。p005の範囲へ **termcap API** を明記（ncursesを選ぶ場合はそのpackage Phaseに依存） | `tputs` が必須 |
| ws034-p008 vim | p005 | 同上 | `tgetent` が必須 |
| ws034-p013 gdb | p005, p011 | p005, p011, **p021** | expat（XML target description）とzlib。termcap APIも要る（p005） |
| ws034-p011 binutils・GMP等 | p005 | 変更なし。範囲に「GMP/MPFR/MPC/ISLは静的ライブラリ」「ターゲット上のbinutilsとbuild機上のクロスbinutilsの両方」を明記 | §2.3 |
| ws034-p012 gcc（クロス） | p011 | 変更なし。範囲に「build機用のGMP/MPFR/MPC/ISLは同じdistfileからgccのbuildの中で作る」を明記 | p011のターゲット用ライブラリはbuild機で動くgccには使えない |
| ws034-p025 meson契約 | p001 | 変更なし。範囲に **gperf（host tool）、pkg-config wrapper、package prefix、config.sub・libtoolの共通対応** を加える | §3.1、§3.2、§3.5、§3.6 |
| ws034-p026 glib | p025 | p025, **p021**, **p005（iconv）** | glibはzlibとiconvが必須。libiconv packageを選ぶ場合はそのPhaseに依存 |
| ws034-p027 フォント系 | p021, p025 | p021, p025, **p026** | pangoが使う `harfbuzz-gobject` のため、harfbuzzをglib付きで作る |
| ws034-p034 本家libwayland | p021, p026 | p021, **p025**, p026, **p005** | mesonでbuildする（p025）。epoll・timerfd・signalfdが必須（p005、または§10の判断） |
| ws034-p028 描画系 | p026, p027 | 変更なし。範囲に **libtiff** を加える | GTK4がlibtiffを必須にしている |
| ws034-p029 GTK4 | p028, p034, WS035-p028 | 変更なし | （libtiffはp028へ入れる前提） |
| ws034-p030 Qt6 | p027, p034, WS035-p028 | p027, **p028**, p034, WS035-p028 | Wayland QPAはlibxkbcommon（p028）が必須 |
| ws034-p031 GTK3 | p028, p034, WS035-p028 | 変更なし | atkはp031の中（at-spi2-core `atk_only`） |
| ws034-p032 Qt5 | p027, p034, WS035-p028 | p027, **p028**, p034, WS035-p028。範囲に **qtsvg** を加える | libxkbcommon。VLCのQt GUIがQt5Svgを要求 |
| ws034-p024 FFmpeg | p005 | 変更なし（zlibを使うならp021） | ― |
| ws034-p018 VLC | p024, p032, WS035-p019 | 変更なし（WS035のOSS `/dev/dsp` はWS035-p019の前提で入る見込み。入らない場合はWS035-p006を明記） | ― |
| ws034-p019 ca-certificates | p001 | 変更なし。§7の方式では `external.mk` へ「tarでない単一ファイル」の取得を加える必要がある | §7 |
| ws034-p017 curl | p019, p021 | 変更なし。範囲に `--without-libpsl`（または libpsl の追加）を明記 | libpslが既定で必須 |

ws.mdの本文との食い違い: 「p001の要点」に書かれた版の目安のうち、**emacs 30.x → 31.1**、**vim 9.1 → 9.2** が
現在の安定版と異なる（§2.1）。

## 7. CA bundle（ca-certificates）

| 項目 | 内容 |
| --- | --- |
| 入手元（採用） | curlが配布する `https://curl.se/ca/cacert-YYYY-MM-DD.pem`（日付付きで不変）。Mozillaの `certdata.txt`（Firefoxのsource tree、NSS builtins）から `mk-ca-bundle.pl` で、TLSのserver認証を信頼するroot証明書だけを抜き出したもの。今回は `cacert-2026-08-13.pem`（"Certificate data from Mozilla as of: Thu Aug 13 03:12:01 2026 GMT"、121件。hostのOpenSSL 3.5.6で121件すべてparseできた） |
| 検証 | 同じ配布元の `cacert-2026-08-13.pem.sha256` と一致。署名は配布されていない。別系統の確認として、Debianの `ca-certificates_20260816`（Mozilla bundle 2.90、署名付き `.dsc` で検証済み）の `mozilla/certdata.txt` と突き合わせられる（p019で行う） |
| ライセンス | MPL-2.0（証明書データ、`certdata.txt` のheader）。`/usr/share/licenses/ca-certificates/` にMPL-2.0の本文と出所を置く。Debian版のscript（`certdata2pem.py` 等）はGPL-2.0-or-laterだが、curlのPEMを使えばそれらは使わない |
| 置き場所 | `/etc/ssl/cert.pem`（1ファイル）。WS032のOpenSSLは `--openssldir=/etc/ssl` なので、OpenSSLの既定のCA file（`X509_get_default_cert_file()`＝`OPENSSLDIR/cert.pem`）がちょうどこの名前になる。既定のCA directoryは `/etc/ssl/certs`（hash名のファイルが要る。最初は使わない） |
| 利用側 | curl: `--with-ca-bundle=/etc/ssl/cert.pem`（cross時は自動検出しないので明示）。wget: OpenSSLの既定のpathを使う（`SSL_CTX_set_default_verify_paths`）。git: libcurl経由。OpenSSLのコマンド: 既定のpathで読む |
| 更新の方法 | curlは、Mozillaのbundleが変わるたびに新しい日付のPEMを出す（`https://curl.se/docs/caextract.html`）。更新は、package Makefileの版・URL・size・SHA-256を新しい日付のものへ変え、公開 `.sha256` と照合し、証明書数とparse可否を確かめて記録する。期限切れや失効（distrust）の反映はMozillaとcurlの配布に従う |
| `external.mk` との関係 | `archive.sh verify` はtarのmemberを検査するので、`.pem` は**そのままでは通らない**（tar -tfが失敗する）。p019で次のどちらかにする: (a) `archive.sh`・`external.mk` に「単一ファイル」の取得（size・SHA-256だけ検査し、展開しない）を加える、(b) Debianの `ca-certificates_20260816.tar.xz`（tarなのでそのまま通る。ROOTは `ca-certificates`）を取得し、同梱の `mozilla/certdata.txt` からbuild時にPEMを作る（hostのpython3で `certdata2pem.py` を動かす。scriptはGPL-2.0-or-laterだがbuild時だけ使う）。**既定の案は(a)**（配布物がそのまま使え、変換処理を持たない） |

## 8. meson cross file の案（p025の入力）

WS032の `userland/packages/tools/gen-cross-toolchain.sh` が生成するもの（wrapper `zedbsd-clang` 等、CMakeの
`zedbsd.cmake` と `Platform/zedBSD.cmake`、autoconfの `zedbsd.cache`、`environment.sh`）に、次を加える。
**値の正本を二重に持たない**という既存の方針に従い、cross fileはwrapperの名前を指すだけにする。

### 8.1 生成するファイル（`build/<plat>/packages/toolchain/` の下）

1. `zedbsd-meson-cross.ini`（cross file）:

   ```ini
   # zedBSD meson cross file.  Generated; do not edit.
   [binaries]
   c = '<outdir>/bin/zedbsd-clang'
   cpp = '<outdir>/bin/zedbsd-clang++'
   ar = '<outdir>/bin/zedbsd-ar'
   nm = '<outdir>/bin/zedbsd-nm'
   strip = '<outdir>/bin/zedbsd-strip'
   objcopy = '<outdir>/bin/zedbsd-objcopy'
   readelf = '<outdir>/bin/zedbsd-readelf'
   pkg-config = '<outdir>/bin/zedbsd-pkg-config'
   # exe_wrapper は置かない（ターゲットの実行ファイルはbuild機で動かない）

   [host_machine]
   system = 'zedbsd'
   kernel = 'zedbsd'
   cpu_family = 'x86_64'      # i386のときは 'x86'
   cpu = 'x86_64'
   endian = 'little'

   [properties]
   sys_root = '<sysroot>'
   needs_exe_wrapper = true

   [built-in options]
   default_library = 'shared'
   wrap_mode = 'nodownload'
   ```

2. `zedbsd-meson-native.ini`（native file）: build機のcc・python3・pkg-config（hostの `.pc` を読む素のもの）を
   明示する。host用のwayland-scanner・gperf・glib道具等を先に作った場合、その置き場所を `[binaries]` で渡す。
3. `bin/zedbsd-pkg-config`: `PKG_CONFIG_LIBDIR` を「package prefixの `usr/lib/pkgconfig` と `usr/share/pkgconfig`、
   sysrootの `.pc`」だけにし、`PKG_CONFIG_SYSROOT_DIR` を設定してhostの `.pc` を読ませない。autoconf（`PKG_CONFIG`）、
   CMake（`PKG_CONFIG_EXECUTABLE`）、mesonの全部に同じものを渡す（§3.5）。
4. `environment.sh` に `ZEDBSD_CROSS_MESON_CROSS=…/zedbsd-meson-cross.ini`、
   `ZEDBSD_CROSS_MESON_NATIVE=…`、`PKG_CONFIG=…/zedbsd-pkg-config` を加える。

### 8.2 mesonとninja自体

- **meson 1.12.0をdistfileから使う**（installせず `python3 <src>/meson.py`）。hostの1.7.0はGTK4とfontconfigの
  下限に届かない（§2.4）。展開先は `build/host-tools/meson`（architectureに依存しない）。
- ninjaはhostの1.12.1をそのまま使える見込み。版を固定したい場合は1.13.2をhost ccで作る。
- 置き場所の案: `userland/packages/tools/host/{meson,ninja,gperf}/Makefile`（host道具で、rootfsへは入れない）。
  `external.mk` の `ZEDBSD_EXTERNAL_SOURCE` で取得・検証・展開は共通化できる。

### 8.3 確かめたこと

meson 1.12.0（distfileから展開）に、`system = 'zedbsd'`、`kernel = 'zedbsd'`、`needs_exe_wrapper = true` の
cross fileを与えて小さなprojectをsetupした（compilerはhostのccで代用。`/tmp/ws034p001/mtest`）。meson は未知の
systemを警告なしで受け付け、`host_machine.system()` は `zedbsd` を返し、実行ファイルと共有ライブラリ（SONAME
`libu.so`）が作られた。**zedBSDのクロスclang（`build/llvm`）はこのcheckoutに無いため、実際のwrapperを使った
setupは未確認**（p025で行う）。

### 8.4 package側の注意（p025以降）

- 多くの `meson.build` は `host_machine.system()` を `linux`・`freebsd` 等の一覧と比べる。zedbsdはどれにも
  当たらないので、「その他のUnix」の経路に入る。wayland（epoll-shimの判定）、gdk-pixbuf（glycin）、GTK4
  （libdrmはLinuxだけ必須）で確かめた。個別の分岐はpackageのPhaseで確かめる。
- `b_pie` は既定のまま（wrapperとclang driverが実行ファイルの作り方を決める。WS032の契約どおり）。
- 生成物は全件 `tools/build/check-dynamic-elf.py` で確かめる（symbol versioning、`DT_NEEDED`）。

## 9. 参考: Rust と Chromium の依存

- **Rust**: stable 1.98.1（`https://static.rust-lang.org/dist/channel-rust-stable.toml` の `pkg.rust` が
  `1.98.1 (48a229cea 2026-09-01)`、channelの日付 2026-09-03）。後回しなので版だけを仮に記録する。取得・検証は
  p014で行う（`rustc-1.98.1-src.tar.xz` と `.asc`、`.sha256`）。ライセンスは MIT OR Apache-2.0。
- **Chromiumの依存（取得していない）**:

  | 名前 | 版（参考） | 入手元 |
  | --- | --- | --- |
  | NSS | 3.129（`NSS_3_129_RTM`） | `https://ftp.mozilla.org/pub/security/nss/releases/` |
  | NSPR | 4.40 | `https://ftp.mozilla.org/pub/nspr/releases/` |
  | そのほか（名前だけ） | ― | dbus、at-spi2-core（atk-bridge）、libdrm、mesa（gbm）、cups、libxkbcommon、fontconfig、freetype、harfbuzz、glib、GTK3（任意）、libpulse（WS035-p019）、expat、zlib。多くは同梱（`third_party/`）で置き換えられる。WS035-p029で確定する |

## 10. 人間の判断が要る点

1. **iconv**: libcに実装するか（p005）、GNU libiconvをpackageにするか（§3.7）。
2. **termcap API**: baseのcursesに加えるか（p005）、ncursesをpackageにするか（§3.8）。
3. **libwaylandのepoll・timerfd・signalfd**: カーネル・libcに加えるか、libwaylandへパッチを当てるか、epoll-shim
   相当を作るか（§2.6）。本家libwayland（p034）とGTK・Qt全体の前提になる。
4. **gitとRust**: git 2.55は `NO_RUST=1` で作れるが、Git 3.0でRustが必須になる予告がある。Rustを「全体の最後」に
   置いたままだと、将来gitを3.xへ上げるときに詰まる。当面2.x系＋`NO_RUST` でよいか。
5. **CA bundleの配布形**: curlのPEM（`external.mk` に単一ファイル取得を加える）か、Debianのtarball（build時に変換）か
   （§7。既定案はcurlのPEM）。
6. **config.sub・libtoolの共通対応**: パッケージごとのパッチを続けるか、展開後の共通処理にするか（§3.1、§3.2）。
   共通処理にする場合、GPLのconfig.subをツリーに置かない方式（Zlibのscriptで編集）を推す。
7. **atkのsource**: at-spi2-core 2.62.0.1（`atk_only`、libxml2を外すパッチ）か、単体atk 2.38.0か（§2.8）。
8. **版の目安の更新**: ws.mdの「emacs 30.x」「vim 9.1」を現在の安定版（31.1、9.2）へ直してよいか。
