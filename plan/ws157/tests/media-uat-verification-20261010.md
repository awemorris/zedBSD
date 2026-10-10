# メディアUAT修正: 検証記録 (2026-10-10)

Scope: [media-uat-20261010](../../ws197/codex-queue.md)、ws197-p012 / ws127-p013 / ws157-p008 / p009。
Worktree: `.claude/worktrees/ws197-media-uat`、branch `codex/ws197-media-uat`、base `624a7301c`。
Source commit / main read-back: 統合待ち。共有master/Queue/cacheの更新とGitHub公開はQ1へ保留。

## 原因と変更

- Phoneが1分起動しない追加報告をSSHで再現。実機の旧Phone CRC311283164/121120はREADY後exit139。kernel fault PC d5a9、PIE bias1000を除いたc5a9はcaptionなしmediaでNULL本文を参照する命令。store再読込の空bodyはNULLとなる。timelineとsidebar previewの両方でNULLも空captionと扱い、再起動後の写真のみmessageを表示可能にした。
- 受信attachmentのdouble-clickはpathをrequestへcopyし、後続syncの並べ替えから独立させる。clipしたtimelineのcardにだけ入力を登録。Image Viewer / Video Playerへposix_spawnで渡し、16 child slotをWNOHANGで回収。送信はこのQueueでは実装しない。
- draft写真は既存decoderのbounded cacheから32pxのthumbnailを表示。ファイル名/remove/page切替は維持。
- Filesは2回目のpressをreleaseまで保留。drag開始の閾値を超えた時点でもopenとclick timeを取り消す。失敗したdragや3回目のtapも誤openしない。
- KL_VERSION83のKL_FILE_CHOOSER_FOLDERを追加。ファイルを列挙せず、現在folderまたは選択folderを返す。double-clickはfolder内へ移動。typed pathはdirectoryだけ受理。Photosは選んだdirectoryを直接importし、ファイルのparentへの変換を廃止。
- Photosはimport/listの同期API待ちをworker所有のWayland display/systemへ移す。UIだけがlibrary/viewを更新。snapshot FDの所有をtakeで移し、通知をcoalesceする。closeはworkerのprivate connectionだけshutdownしてjoin。取り込み中のmarksをlocal保持し、結果反映前または終了時にcommitして破棄を防ぐ。終了時に未保存marksがある場合、そのcommit完了を待つことはある。完了snapshotへ置き換える直前に旧modelをreleaseし、parse/view allocation失敗時はpartial modelをreleaseしてview indices/jobsもresetする。
- 添付原本は4080×3072、2330488 bytesのbaseline JPEG。実機でdecode554msは成功、texture vkAllocateMemory=-2で終了。i915のmax_resource_bytes=16MiBと約50MiB textureの衝突。Image Viewerはmemory errorの場合だけCPUでvisible viewportをsampleし、window-sized canvasをGPUへ渡す。原本/1:1/四方向回転/mip/animationを維持。通常はGPU textureの既存経路。HAL/driver/resource上限は変更しない。

## 短いhost確認

Private `build/ws197-media-uat/` の開発probeだけを使用し、恒久test/toolを増やさない。添付画像/EXIF/本文/番号はgitへ入れない。

- 元JPEGの現行独自libjpeg-compat decode: PASS、4080×3072、約0.24秒。
- folder-probe: PASS (ASan/UBSan)。current folder、selected child、double-click navigation、typed file拒否/typed directory受理。
- canvas-probe: PASS (ASan/UBSan)。1:1の4pixel一致、四方向rotation、nearest/bilinear、clipがcanvas外へ書かないこと。
- files-click-probe: PASS (ASan/UBSan)。production input_press_item/input_releaseを直接呼び、second pressでopenしない、releaseでopen、drag releaseでopenしない、single selection維持。motionによる取消はsourceをmanual確認。compositor/trackpadの実操作はユーザーUAT。
- worker-probe: PASS (ASan/UBSan)。遅いcompositor request中もqueue/takeが即時、EBUSY、FD handoff、copied path、blocked readをcloseで解除、stop/start。実media APIの応答はcontrolled double、Wayland descriptorはsocketpair。通常sandboxではshutdownの試験がtimeoutとなり、許可環境のstraceではshutdown/read解除/joinを確認、同環境の直接試験PASS。strace+LSanは非対応のためtrace終了のLSan errorは受入結果に使わない。

## build / 最終全文規約

```sh
make -j8 BUILD=build/ws197-media-uat ZEDBSD_CONFIG=config/current-uat.mk \
 ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot \
 -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete \
 -o /home/awe/zedBSD-claude1/.claude/worktrees/ws197-media-uat/build/llvm/.zedbsd-install-23.1.0-zedbsd8 \
 'DYNAMIC_CPPFLAGS=-nostdinc -I. -Iinclude -Iuserland/desktop/include -isystem /home/awe/zedBSD-claude1/build/amd64/sysroot/usr/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC' \
 build/ws197-media-uat/bin/phone build/ws197-media-uat/bin/photos \
 build/ws197-media-uat/bin/files build/ws197-media-uat/bin/imageview \
 build/ws197-media-uat/dynamic/libkeiland.so
make -f userland/desktop/keiland-linux.mk -j8 KEILAND_LINUX_BUILD=build/ws197-media-uat-linux \
 KEILAND_LINUX_EMOJI=/home/awe/zedBSD-claude1/build/distfiles/NotoColorEmoji-2.047.ttf \
 build/ws197-media-uat-linux/lib/libkeiland.so
git diff --check
```

Final named build5 targetsとLinux libkeiland: exit0 / warning0。最終manual reviewでsnapshot置換のmodel release/失敗resetを補い、Photos named buildを再実施してexit0/warning0、同sourceのstyle-checkは0。初回format後にfolder定数を戻す際の位置合わせミスを修正し、上記の最終buildを取り直して成功。private probeの初回helper名誤記も修正してPASS。共有sysroot/toolchainはreadonly利用、image入力には使っていない。QEMU、make check、負荷/耐久、実MMS送信は未実施。

[C全文](../../coding-style.md)・Guardrail・automationを全変更sourceへ適用。clang-format19 19.1.7はchanged range/new fileのみ、prototype一行/定義の引数TAB/comment/bracesを規約へ整えた。cc14.2、native LLVM23.1系の既存toolchain。manualでleading ANSI declarations、FD/child/mutex ownership、failure cleanup、pointer clip、NULL/empty、texture→CPU fallback、dirty marks/notificationの順序を確認。

style-checkの残り30件は全てbaseにもあるdiagnostic: files/ui-input.c5、imageview/main.c1、keiland.h24。git show HEADの各fileへ同じcheckerをかけ、diagnostic本文を比較して各件数/本文が一致。新worker/画像sampling/新hunkの新規findingは0。既存30件を解消したとは主張せず、この有限scopeの適合とWS全体の未完条件を分ける。

Source範囲: desktop/files (files.h/ui-input/ui-drag)、imageview (canvas/draw/main/presentと2header)、keiland.h、libkeiland/ui (chooser-model/view)、phone (main/view/header)、photos (main/Makefile/new media-worker.c/h)。kernel/HAL/toolchain/shared boardsは変更しない。host probesとlogsはprivate build内で保持、恒久試験の追加はない。

## SSH実機での確認と交換

- kei@10.0.30.3、既存承認のpassword認証/host鍵方針。Phoneの正しいWayland環境で旧版SIGSEGV、修正版8秒起動はREADY→LINK→SYNC→DONE timeout / exit0。修正版へ交換し通常起動、継続process176を確認。ユーザーが操作した受信写真のviewer child起動もlogで確認。本文/番号は記録しない。
- 元JPEGの旧Image Viewerはdecode成功→vkAllocateMemory=-2 / exit1。修正版は4080×3072のままsampling=cpu→READY→DONE timeout / exit0。pixel/回転/clipの正しさは上のhost試験。画面を撮った確認は未実施。
- 実機mediastorageのprivate --rootに同じ原本をadd: exit0、size2330488/width4080/height3072を確認。通常Media DBにこのfixtureを追加しない。
- 最終4app/libをbackup後new名でcopy/renameして交換。交換前は `/tmp/ws197-media-uat-final/original/`、最初のPhone SIGSEGV版は `/tmp/ws197-media-uat-phone-original`。desktop/Bluetoothのrestartと起動中appの終了は行わない。起動中のPhone/Filesは旧mappingを保持し、最終修正は次のapp起動で適用される。

| File | Final CRC | Bytes |
| --- | --- | --- |
| /bin/phone | 914109946 | 121368 |
| /bin/photos | 3049254075 | 154784 |
| /bin/files | 1170618958 | 298192 |
| /bin/imageview | 1636952486 | 126736 |
| /lib/libkeiland.so | 42534013 | 437880 |

実機交換後のcksumは5fileともlocalと一致。Photosは6秒起動でREADY、private workerのMEDIA error0/snapshot0、DONE timeout/exit0を確認。既存media番号0/1 (mp4) のサムネイルEINVALは旧版でも再現。新実行で番号2/3 (JPEG) もEINVALとなったが、旧版での同条件比較は未確認であり、原因や退行の有無は未判定。今回の添付JPEGのGPU allocation failureと同一原因とは判断しない。対象mediaのdecode調査は残件として保持。フォルダchooserの実操作、背景import中のGUI操作、draftthumbnailとtrackpadの実機操作、受信動画のplayer起動はユーザーUAT。MMS送信/HFP/PBAP、各WS全体の残件はこのQueueでclearしない。
