# メディア管理 最終検証（2026-10-10）

Base: main `9dfebc99b`。branch `codex/ws197-media-library`、専用worktree `/.claude/worktrees/ws197-media-library`。main統合はユーザーの既存承認。master/共有Queue/cache/Guardrailは未編集、Q1への投影待ち。

## 結果

- zedBSD named build: `mediastorage`, `libkeiland.so`, `photos`, `wayland`, `phone` PASS、warning 0、dynamic ELF gate PASS。
- 共通Linux `all` build PASS、warning 0。FreeBSD source/Makefile登録済み、FreeBSD native buildは環境不在で未実施。
- `run-host-mediastorage.sh` PASS。変更したCLI/parser/backend/relay/draft/modelをASan+UBSanでcompile。実際のLinux libkeilandは通常buildを使用。
- CLI: パス/metadataのみ、原本copyのbyte一致、SHA256 dedup、12件/4workerの競合追加、favorite/rotation/album、partial成功保持、未対応AVIF拒否、default Media root、JSON再open、未知location/nested array/decimal/Unicodeの保持、破損JSON上書き拒否。
- 取り込み日: PNG・EXIFのないJPEGを古いmtimeから取り込み、保存pathは取り込み日。EXIF JPEGは2024/08/15、JPEG/PNGの81x43・29x17・37x19を確認。
- 実際のclient/compositor/CLIをhost Waylandで接続: list/add/apply/watch、別CLI commitのWayland通知、最初のFD送信失敗でduplicateが消費されてもretain/retry後に正しいsnapshotを返す。
- backend: posix_spawn・双方向pipe、1000 album/約156 KiBで両方向ともpipe容量超、全行維持、Homeのwaitpid(-1)が先にreapした時の所有処理、明示errno。
- Phone/chooser: metadata一覧→絶対path、Media modeでfilesystem移動しない、contact IDに結びつくdraft、重複/full-capacity、URI percent decode、text/Japanese append、binary/overlimit拒否、原本を削除せず所有tempだけcleanup。送り先/transportは呼ばない。
- 既存 `run-host-photos-db.sh` PASS。共通importを新Files/import日へ追従し、旧月別DB fixtureのdedup/name collision/reopen/album/favorite/非書換monthも確認。旧storageは製品CLIにはリンクしない。
- `keiland-os-boundary/check.sh`: C1〜C5、L1〜L7、M1/X1、B1/B2/B3/B5/S1、A1〜A5 PASS。Apps/CLIにraw UNIX socketなし。`exports.py --check` PASS。
- clang-format19 changed hunk/new source整形後、引数を各行TABという全文規約へ復元。style-check: 今回導入違反0。既存未変更箇所はkeiland.h 24件、home.c 3件、HEAD版とのrule/text比較で同数・追加0。全文規約の手動確認: source/API層境界、public-before-static、先頭宣言、段落comment、結果/条件分離、ownership、pipe EOF/close/spawn error、late FD、failed update保持、未送信draftを確認。
- `git diff --check` PASS。

## コマンド・環境

Debian host GCC 14.2.0、clang-format 19.1.7、host Wayland server 1.23.1。zedBSD toolchain/sysrootは既存read-onlyのものを使用。toolchain/sysrootの再buildなし。

```sh
make -j8 BUILD=build/ws197-media-library ZEDBSD_CONFIG=config/current-uat.mk \
 ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot \
 -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete \
 -o /home/awe/zedBSD-claude1/.claude/worktrees/ws197-media-library/build/llvm/.zedbsd-install-23.1.0-zedbsd8 \
 'DYNAMIC_CPPFLAGS=-nostdinc -I. -Iinclude -Iuserland/desktop/include -isystem /home/awe/zedBSD-claude1/build/amd64/sysroot/usr/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC' \
 build/ws197-media-library/bin/mediastorage build/ws197-media-library/dynamic/libkeiland.so \
 build/ws197-media-library/bin/photos build/ws197-media-library/bin/wayland build/ws197-media-library/bin/phone
make -f userland/desktop/keiland-linux.mk -j8 KEILAND_LINUX_BUILD=build/ws197-media-library-linux \
 KEILAND_LINUX_EMOJI=/home/awe/zedBSD-claude1/build/distfiles/NotoColorEmoji-2.047.ttf all
sh plan/ws157/tests/run-host-mediastorage.sh build/ws197-media-library-host build/ws197-media-library-linux
sh plan/ws157/tests/run-host-photos-db.sh build/ws197-media-library-host/host-photos-db
BUILD=build/ws197-media-library KEILAND_LINUX_BUILD=build/ws197-media-library-linux \
 sh plan/tools/keiland-os-boundary/check.sh
python3 userland/desktop/libkeiland/exports.py --check
```

Logs（worktreeの一時build artifact）: `build/ws197-media-library-build.log`, `build/ws197-media-library-linux-build.log`, `build/ws197-media-library-host/{final-tests,legacy-db,boundary,final-style}.log`, `style-baseline.txt`。再現可能なsource fixture/scriptはversion controlへ保存。

## 限界・引き継ぎ

実機GUI/UAT、MMS添付送受信、動画再生、FreeBSD native build、OS image生成/QEMU全試験、make checkは未実施。実機を更新/再起動せず、外部messageを送信していない。GUIは[利用法と実機手順](mediastorage-usage.md)で確認する。MMS接続は[ws197-p010](../../ws197/phase010/phase.md)、未統合の試作treeを保管。WS157/197全体はincompleteのまま。今回のfinite implementation/build/host範囲だけを判定する。

## 最終sourceレビュー対象（41ファイル）

```text
plan/ws157/tests/host-media-backend.c
plan/ws157/tests/host-media-wire.c
plan/ws157/tests/host-photos-db.c
plan/ws197/tests/host-media-select.c
userland/desktop/include/keiland/keiland.h
userland/desktop/libkeiland-backend/keiland-backend.h
userland/desktop/libkeiland-backend/media/media.c
userland/desktop/libkeiland/system/kl-system-protocol.h
userland/desktop/libkeiland/system/system-protocol.c
userland/desktop/libkeiland/system/system-protocol.h
userland/desktop/libkeiland/system/system.c
userland/desktop/libkeiland/ui/chooser-model.c
userland/desktop/libkeiland/ui/chooser.c
userland/desktop/libkeiland/ui/chooser.h
userland/desktop/mediastorage/database.c
userland/desktop/mediastorage/dimensions.c
userland/desktop/mediastorage/dimensions.h
userland/desktop/mediastorage/json.c
userland/desktop/mediastorage/json.h
userland/desktop/mediastorage/main.c
userland/desktop/mediastorage/notify.c
userland/desktop/mediastorage/notify.h
userland/desktop/phone/main.c
userland/desktop/phone/media.c
userland/desktop/phone/phone.h
userland/desktop/phone/view.c
userland/desktop/photos/app.h
userland/desktop/photos/cache-folders.c
userland/desktop/photos/exif.c
userland/desktop/photos/import.c
userland/desktop/photos/library.c
userland/desktop/photos/main.c
userland/desktop/photos/photos.h
userland/desktop/photos/snapshot.c
userland/desktop/picture/media-kind.h
userland/desktop/wayland/home.c
userland/desktop/wayland/kwl.h
userland/desktop/wayland/media-library.c
userland/desktop/wayland/objects.c
userland/desktop/wayland/protocol.c
userland/desktop/wayland/system.c
```
