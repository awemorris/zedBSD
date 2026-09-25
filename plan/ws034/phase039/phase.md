<!-- awesome-plan project=zedbsd record=ws034p039 -->

# ws034-p039: 開発用ファイルの menuconfig option と、base の既定を全部 ON

Phase ID: `ws034-p039`
Parent: [WS034](../ws.md)
Status: **cleared**（q349-i01、2026-09-24）
Phase disposition: normal
Queue: q349（q349-i01）
実行: メインセッション

## 変更

### 開発用ファイルの option

- `config/rootfs-options.list`（新規）に `ZEDBSD_ROOTFS_DEVELOPMENT`（bool、既定 `y`、全 platform）。
  menuconfig の主 menu に「Select root file system option」を足し、`all_option_files()` に加えたので
  defaults・save が扱う（生成される config に `ZEDBSD_ROOTFS_DEVELOPMENT := y` が出る）。
- `Makefile`: `n` のとき、package の組み込みの後で tree から開発用ファイルを除く:
  `/usr/include`、`/usr/lib/pkgconfig`、`/usr/share/pkgconfig`、`/lib`・`/usr/lib` 直下の `.so` の symlink と `.a`。
  library 本体は SONAME の名前の通常ファイルなので残る（loader の名前は壊れない）。`y` は従来どおり sysroot の
  header と link 用 object も入れる。
- rootfs の config stamp に `development=` を加えた。option を切り替えると tree が作り直される（stamp を手で消さなくてよい）。

### base の既定を全部 ON

base の group で既定 `n` だった 14 個（libvulkan、libwayland-client、mview、terminfo-extra、gpu-admission/fence/i915/recovery/share-test、
vkdemo、wltest、zedinst、zwl、venus-frame）を `y` にした。

ON にすると、**platform の欄が実際と合っていない** ことが分かった: libvulkan・libwayland-client・libtruetype・libzdesktop と、
それらを link する vkdemo・wltest・mview・gpu-share-test・gpu-fence-test は amd64 の動的 link の規則しか無いのに `*` だった。
libtruetype と libzdesktop は以前から既定 `y` なので、**既定の i386 の build は以前から壊れていた**
（`No rule to make target build/.../dynamic/libtruetype.so`）。この 9 個の platform を `amd64` にした。

platform の欄は menu の表示を絞るだけで、既定の選択にも Makefile にも効いていなかった。次の 2 か所で効かせた:

- `Makefile`: `ZEDBSD_DEFAULT_USER_PROGRAMS`（config が program の一覧を持たないときの既定）を platform で絞る
  （`user_program_applies`）。config が明示した選択はそのまま使う（build できなければ失敗で分かる）。
- `tools/menuconfig.py`: save は現在の platform で build できる program だけを書く。menu の状態には選択を残すので、
  target を戻せば選択も戻る。

## 検証

| 検証 | 結果 |
| --- | --- |
| menuconfig の既定（amd64・i386・pc98・rpi4）を save | amd64 は GPU/Wayland の 4 個を含む 173 語、他は含まない 166 語。`ZEDBSD_ROOTFS_DEVELOPMENT := y` |
| 既定 config の rootfs（i386・pc98・amd64） | 3 つとも build できる（i386 は変更前 mview、次に libtruetype で失敗していた） |
| `plan/ws034/tests/config-amd64-dev.mk`（p037 の image）で `y` | `/usr/include` 108、`/usr/lib/pkgconfig` に expat・libcurl・zlib、`.so` の link、`.a` がある |
| 同じ BUILD で `n` に切替 | stamp を消さずに作り直され、上の 4 種が無い。`libz.so.1` 等の本体と clang の resource dir は残る。壊れた symlink 0 |
| `git diff --check` | OK |

## 残り

- `n` でも clang package を選ぶと、clang が持ち込む `/usr/lib/crt1.o`・`libc.so` と resource の header は残る（clang の一部）。
- rpi4 の既定は保存できるが、rpi4 の kernel が build できないので rootfs までは試していない（別 Phase で直す）。
