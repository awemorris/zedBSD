# WS197 p010 受信部分: 最終確認 (2026-10-10)

Scope: [受信Queue](../codex-queue.md) の media-rx-i01 / media-rx-check-i01。写真・動画のMMS取得、匿名FD、mediastorageの原本保存、Phoneの画像表示とmessageの再読込。送信・動画player起動・WS全体のclearanceはこの結果に含まない。

## 原因と変更

main c43a01797はMAP GetMessageのAttachment=0、MMS text-only reader、Phoneの添付取得/保存未接続だった。実機ではMNS通知が到達していたが、画像本体を取り込む経路がなかった。

Attachment=1、16MiBまでのMIME input / 計8MiBまでのmedia / 最大16 partを実装。独自Zlib codecはOS非依存の `userland/desktop/libmms/` に配置する。bluetoothdとzedBSD backendが本文を検証し、backendの匿名FDをWayland v30の新eventでlibkeilandへ運ぶ。lib APIはKL_VERSION82、旧item event・旧構造体sizeを保持する。FDのqueue overflow/legacy caller/releaseを含む所有を確認した。

Phoneはlibkeilandのpath-list APIでmediastorageへ原本を渡す。戻ったmetadataのSHA256から正規パスを解決し、messageへMedia-Photo/Media-Video headerとしてatomicに保存する。失敗時には旧messageと旧pathを維持する。元ファイルは `~/Pictures/Media/Files/YYYY/MM/dd/`、metadataはMedia/metadata.db。写真は既存Photos JPEG/PNG/GIF decoderで描く。動画は保存済み項目として表示し、playerへの起動接続は残る。

Phone/CLIのdaemon UNIX socketは禁止を維持する。OS処理はbackend、CLIの起動は既存posix_spawn/pipes、通知はlibkeiland/Waylandを使用。Phoneにはlibmmsの純粋なbyte codecだけを共有し、backendをlinkしない。

## ホストの確認

Worktree: `.claude/worktrees/ws197-media-receive`、branch codex/ws197-media-receive、base c43a01797。

- MIME/bMessage: binary262147 bytesのimage/video、media-only、UTF-8 caption、roundtrip、上限: PASS (ASan/UBSan)。
- MAP: GetMessage Attachment=1、MIME byte length、通知/履歴とSMS既存経路: 172 checks / 0 failed。
- MMS本文: bt-mms-host-test PASS。従来のmixed fixtureの不正image base64を有効なbytesへ修正し、媒体のみは新parseでは受理・text-only wrapperではENODATAとする。
- backend: fragmented MIME完成前はFDを公開せず、captionと原本spool byte一致、次のtakeのborrowed FD終了: PASS。
- compositor/libkeiland: v30 MIME FD、v27旧caption event、旧sizeof callerのFD解放、SMS/PBAP、queue/drop: phone-shell-host-test PASS。
- 保存/再読込: 実client→compositor→backend→CLIを通すfixtureで原PNG byte完全一致、実decoderの2x3画像、SHA dedup、Phone caption/pathのclose/reopenと重複sync: PASS。
- mediastorage JSON/EXIF/import日/画像寸法/拡張field/並行import、CLI外部通知とwatch、両方向pipe、Phone chooser/DnDの既存試験: PASS。
- Phone store既存SMS/PBAPの短いASan/UBSan試験: PASS。

主要コマンド (worktree内):

```sh
ASAN_OPTIONS=detect_leaks=0 sh plan/ws157/tests/run-host-mediastorage.sh build/ws197-media-receive-host build/ws197-media-receive-linux
ASAN_OPTIONS=detect_leaks=0 OUT=build/ws197-media-receive-host/backend sh plan/ws197/tests/phone-backend-host-test.sh
ASAN_OPTIONS=detect_leaks=0 OUT=build/ws197-media-receive-host/shell sh plan/ws197/tests/phone-shell-host-test.sh
ASAN_OPTIONS=detect_leaks=0 OUT=build/ws197-media-receive-host/store sh plan/ws197/tests/phone-store-host-test.sh
ASAN_OPTIONS=detect_leaks=0 build/ws197-media-receive/bt-map-host-test
ASAN_OPTIONS=detect_leaks=0 build/ws197-media-receive/bt-mms-host-test
ASAN_OPTIONS=detect_leaks=0 build/ws197-media-receive/mms-media-host-test
```

実Wayland socketをbindするhost試験は通常sandboxで拒否されるため、同じ試験を承認されたescalationで実行した。回避実装やproductionのtest-only switchは追加していない。

## build / 全文規約

```sh
make -j8 BUILD=build/ws197-media-receive ZEDBSD_CONFIG=config/current-uat.mk \
 ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot \
 -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete \
 -o /home/awe/zedBSD-claude1/.claude/worktrees/ws197-media-receive/build/llvm/.zedbsd-install-23.1.0-zedbsd8 \
 'DYNAMIC_CPPFLAGS=-nostdinc -I. -Iinclude -Iuserland/desktop/include -isystem /home/awe/zedBSD-claude1/build/amd64/sysroot/usr/include -DHAL_ARCH_AMD64 -DKERN_USER_ABI_LP64 -DKERN_DYNAMIC_LIBC' \
 build/ws197-media-receive/bin/bluetoothd build/ws197-media-receive/bin/wayland \
 build/ws197-media-receive/bin/phone build/ws197-media-receive/dynamic/libkeiland.so
make -f userland/desktop/keiland-linux.mk -j8 KEILAND_LINUX_BUILD=build/ws197-media-receive-linux \
 KEILAND_LINUX_EMOJI=/home/awe/zedBSD-claude1/build/distfiles/NotoColorEmoji-2.047.ttf \
 build/ws197-media-receive-linux/lib/libkeiland.so build/ws197-media-receive-linux/bin/wayland
BUILD=build/ws197-media-receive KEILAND_LINUX_BUILD=build/ws197-media-receive-linux sh plan/tools/keiland-os-boundary/check.sh
git diff --check
```

zedBSD 4 targets / ELF DT_NEEDED gates: PASS、warning0。Linux libkeiland/compositor: PASS、warning0。初回Linux target名bin/keilandは存在しなかったので、正しいbin/waylandに修正して成功。OS境界checker初回はbase以下codecのincludeでA5失敗、codecを純粋な共通desktop componentに置き直し、全項目PASS。

[C全文](../../coding-style.md)、Guardrail、automationを最終変更sourceへ適用。clang-format19 19.1.7で変更hunk/新sourceを整え、定義の各引数/TAB、public/static順、leading declarations、FD所有、失敗時unwind、loop/paragraph/returnコメントを手動確認。style-checkは旧keiland.hと同じ24件の既存paragraph-commentだけ。git show HEADのheaderを同じcheckerにかけ、診断本文一致・新規診断0を確認した。既存24件をWS変更の規約適合と偽ってclearしない。新しいsource/API hunkは当該診断に含まれない。WS全体p009の最終reviewは残る。

Logs: private buildの `ws197-media-receive-{build,linux-final,tests,map,mms-text,mime,backend,shell,store,boundary,style}.log`。toolchain、共有build、mainのmaster/Queue/cacheは変更しない。QEMU、aggregate make check、負荷/耐久、実送信、FreeBSD runtimeは未実施。

## SSH実機更新と現在の限界

ユーザーが4ファイル更新・Bluetooth/デスクトップ再起動 (起動中app終了) を具体的に承認した。実機10.0.30.3、zedBSD beta2+gc43a017、kei uid1000。host鍵の更新とパスワード認証も既存承認済み。本文・番号・媒体内容は証拠へ記録しない。

元の4ファイルを `/tmp/ws197-media-receive.XIm3CU/original/` に保存し、rootでnew名へcopy/chmod後renameして交換した。bluetoothd restartはOK、greeter/sessiond restart後、新wayland PID401の自動login/handshakeとPhone PID426の起動を確認した。SSHはdesktop終了時に切断したため認証して再接続した。

更新時のcksum (machine/local一致):

| File | CRC | Bytes |
| --- | --- | --- |
| /sbin/bluetoothd | 2744362826 | 293936 |
| /bin/wayland | 4236053064 | 1041984 |
| /bin/phone | 3804185665 | 121160 |
| /lib/libkeiland.so | 3530951064 | 437880 |

その後の最終reviewでPhone保存のinvalid-path/allocate failureを改善し、最終Phone CRCは3345269913 / 121160 bytes。その他3ファイルのCRCは同じ。実機へのこの最終Phone差し替えは接続回復後に行う。

再起動後、Phoneはmessages=0 / why=unreachable。bluetoothdはrunningだがMAPの再接続ログはSDP channel openで停止した。承認範囲の再restartを実行したところSSHが応答しなくなった。既存接続の15秒probeと独立SSHのConnectTimeout5秒はtimeout (banner exchange以前)。ホスト全体のfreezeや原因は未確定で、ユーザーへ画面応答/必要なら実機restartを依頼した。

写真の実機受信はまだ成功としない。Media/Filesに3件はあったが、この受信経路で保存した証拠ではない。新しい写真の時刻/Phone画像表示と保存pathを待つ。接続回復後は最終Phoneを更新し、MNS/MAP→FD→CLI→Media-Photoの保存と実表示を確認する。WS197/p010全体の送信と動画player起動、PBAP UAT/HFP/p009の未完条件は維持する。

共有master/Queue/historyへの投影はQ1担当につき保留。GitHub同期状態がこのcheckoutにないため、local記録を公開/同期済みとはしない。pushは行わない。

## main統合の確認

source/evidence commit `035d1d25ba6f9e7fb20ad97544e6c38f3f258f03` (WIP) をmain c43a01797からfast-forward統合した。main/private worktreeともclean、変更sourceは検証したものと同一。READMEの既存変更c43a01797を保持し、共有master/Queue/Guardrail/cacheは未変更。pushなし。SSH受信UATと最終Phone更新は未完のまま保持する。

## 2026-10-10 イメージ再作成前のconfig確認

ユーザー報告: Phone起動に数十秒、Bluetoothとの不整合を感じるためイメージを作り直す。トップレベルconfig.mkを確認。Phone/bluetoothd/wayland/libkeiland、USB BT driver、PNG/JPEG/GIF/zは選択済み。mediastorageは明示一覧になかったが、Photos/waylandのpackage依存によりMakefileが自動で選択し、AMD64_ARCH_FILESへ `/bin/mediastorage=` が入ることを実Make評価で確認した。従って一覧からの欠落が保存不能の原因とは判断しない。

ユーザーのイメージ再作成指示に合わせ、mainのgitignore対象config.mkへ `ZEDBSD_USER_PROGRAMS += mediastorage` を明示追加した。他の設定 (WS083 video probe/streams/libavcodec除外等) は保持。main最新sourceとこのconfigでPhone・compositor・libkeiland・bluetoothd・mediastorageを同じイメージへ組み込む。イメージ自体のbuild/書込はユーザーが行う。起動の遅れの原因は未確定で、前回のBluetooth再接続停止やWayland初期状態の待ちとの関係を再作成後に確認する。

## 2026-10-10 再作成後のMIME回帰 (media-rx-i02)

最新指示は「テキストは受信できましたが、なんとMIMEヘッダも見えてしまってます。画像のMMSは、今度は通知が来ませんでした。SSHで見てみてください。」。実機beta2+g50feed3の4ファイルCRCは統合sourceと一致し、mediastorageと空のversion1 DBも存在。MNS NewMessage/MMSは届いていた。MAPにENODATA(62)、Phone media保存にEOPNOTSUPP(21)。保存本文を内容を出力せず構造だけ確認するとboundary行、text/plain/8BITのpart headersが残っていた。従って通知transportの欠落ではなくMIME解析/保存失敗。

Android MAPはrootのleaf Content-Typeにもboundaryを追加してpartを送る。[Android公式source](https://android.googlesource.com/platform/packages/apps/Bluetooth/+/4a9f9b0/src/com/android/bluetooth/map/BluetoothMapbMessageMime.java)のencodeHeaders/part writer/parserの境界規約を参照し、コードは独自C実装のまま修正。`mms_part`はtype分類前に明示boundaryを評価し、text/plain、image/jpeg、application/vnd.wap.multipart.relatedを同じpart traversalへ通す。空/不正boundaryと標準multipartのboundary欠落はエラー、既存上限とAPIを維持。

確認:

- `mms-media-host-test`: ASan/UBSan PASS。上記3外側typeのsynthetic phone-shaped MIMEで、本文にheadersを含めずdecoded image byteを保持。
- `bt-mms-host-test`: ASan/UBSan PASS。
- `sh plan/ws157/tests/run-host-mediastorage.sh build/ws197-media-receive-host build/ws197-media-receive-linux`: PASS。production Phone fixtureもleaf root+boundaryへ変更し、実Wayland/compositor/CLIを通すPNG原本byte/decode2×3/重複/保存message再openを確認。既存runnerのCLI failure表示は期待された失敗ケース。
- 前節のnamed buildと同じsysroot/CPPFLAGSでbluetoothd、wayland、phoneの3targetを再build: exit0、warning/error0。libkeiland ABI/sourceは今回変更なし。
- `python3 plan/tools/style-check.py userland/desktop/libmms/mms.c plan/ws197/tests/mms-media-host-test.c plan/ws197/tests/host-media-receive.c --summary`: total0。C全文/manual review、edited hunk format、`git diff --check` PASS。

ログ: private buildの `ws197-media-receive-followup-{build,host}.log` と `ws197-media-receive-mms-followup.log`。初回codec test compileはmapxml依存の指定不足でlink失敗、mapxml.cを追加した上記最終実行はPASS。QEMU/aggregate make check/toolchain変更は無し。

以前の更新・Bluetooth/desktop restart承認を保持し、実機 `/tmp/ws197-mime-fix.9Llg4N/original/` へ3実行ファイルを保存後、new名へのcopy/chmod/renameで交換。machine/localのCRC一致:

| File | CRC | Bytes |
| --- | --- | --- |
| /sbin/bluetoothd | 2702255473 | 293896 |
| /bin/wayland | 1532947064 | 1041944 |
| /bin/phone | 311283164 | 121120 |

bluetoothd restartはOK、MAP/PBAP ready。greeter restartで旧sessionとSSHが終了し、password再接続でwayland PID188とPhone PID210を確認。履歴再同期はmedia error0、Media/Filesは0→2→7件、Media-Photo付きmessageは5→10件。元ファイルを保存したまま変更し、既存メッセージの削除/一括書換えは行わない。実機raw logの12:31/12:32等はremote clockのまま扱い、JSTと断定しない。

新規テキスト/写真MMSの時刻・MIMEヘッダ消失・実画像表示をユーザーへasync確認中。再同期で履歴画像保存が成功した事実と新規受信の画面UATは分ける。raw MIMEを入力とする既存MMS keyはboundaryの変化で再同期時に別keyとなりうるため、Media CLIの原本重複排除とmessage identityの安定性を同一の証拠とはしない。p010全体の送信/動画player起動、WSのHFP/PBAP UAT/p009、Q1への共有投影は未完を保持。
