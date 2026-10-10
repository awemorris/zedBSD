# mediastorage とメディア選択（2026-10-10）

## 保存場所

現在のユーザーの `$HOME/Pictures/Media/metadata.db` がversion 1のJSON。
原本を複写したファイルは `Media/Files/YYYY/MM/dd/ファイル名`。
JPEGのEXIF日時を使い、日時がなければ取り込み日を使う。PNG・GIF・動画も撮影日時を取得していないため取り込み日を使う。元ファイルのmtimeは使わない。
SHA256で重複を避け、同じ日・同じ名前の別内容は `名前-1.ext` 等へ保存する。

JSONの `media` 配列は `id, path, sha256, original_name, size, taken, imported, width, height, favorite, turns` を持つ。
`path` はMediaからの相対パス、`size` はバイト数、`width/height` はピクセル数（JPEG/PNG/GIFヘッダから取得、動画等の未知寸法は0）。
日付は既存のEXIF/Photos modelと同じ、ローカル暦をUTCとみなした秒数。`albums` はid・name・membersを持つ。
未知のroot・media・album JSON項目とそのネストした値は通常更新時にも保持する。撮影地等を後から拡張できる。

CLIは安定した `.lock` を使い同時処理を直列化する。metadataは新しいJSONをflush/fsyncしてrenameで置き換える。不正/未対応versionのmetadataはエラーとし上書きしない。原本は直接ファイルとして救出できる。
以前の `Pictures/Library` の自動移動は行っていない。必要な原本は下記addで再取り込みできる。

## CLI

```sh
/bin/mediastorage list
/bin/mediastorage add /absolute/path/photo.jpg
printf '%s\n' /absolute/path/photo.png /absolute/path/movie.mp4 | /bin/mediastorage add-list
```

成功時のstdoutは次のTSV metadata snapshot。メディア本体はstdoutに出さず、返された絶対パスから読み込む。

| 行 | 内容 |
| --- | --- |
| `# keiland-media 1<TAB>ROOT` | 応答version・絶対root |
| `P` | id, absolute path, sha256, size, taken, imported, width, height, favorite, turns, original name |
| `A` | album id, album name |
| `M` | album id, media id |

`apply` はstdinで `P<TAB>id<TAB>favorite<TAB>turns`、`A<TAB>id<TAB>name`、`M<TAB>album-id<TAB>media-id` を受ける。IDで最新DBへmergeし、別clientの追加を失わない。
`--root=/absolute/path` は復旧/hostテスト用の任意root。通常のbackendは指定しない。
失敗はstderrと非0 exit。複数追加の途中で失敗した場合、成功分は保存する。

## 通信

Apps/CLI → libkeiland → Wayland拡張 → compositor → 静的libkeiland-backend → posix_spawn → mediastorage。
backend/CLIのメタデータ・パス一覧はstdin/stdoutのパイプ。メディア本体は常にファイル経由。
CLIの保存通知はlibkeilandの `kl_system_media_notify`。直接UNIXソケットは使用しない。
compositorは `kl_system_media_watch` の登録先へchangedを送り、Photosが最新一覧を再取得する。CLIはcompositor不在でも保存可能。
backendは両パイプをnonblocking pumpし、完了したmetadataをWayland FDに渡すためだけに匿名一時ファイルへ保持する。上限は同時16件・120秒・入力4 MiB・出力64 MiB。

## 実機UAT（未実施、T1/ユーザー確認用）

1. Photosを起動し、PNG・EXIF JPEG・日時のないJPEG・MP4をCLI add-listで取り込む。起動中の一覧更新、サイズ、Files以下の日付を確認する。
2. Photosを終了して再起動し、一覧・お気に入り・回転・アルバムを確認する。Photosの取り込み操作からも原本が保存されることを確認する。
3. Phoneで会話を選び＋を押す。Media Libraryの名前/アイコン一覧から写真・動画を選び、未送信の添付カードを確認する。
4. Filesから写真・動画をdrop、テキストをdropして入力欄への追加、PNG画像dropも確認する。取消・会話切替で別受信者へ混入しないことを確認する。
5. 添付は一時的なdraftで、app再起動では保持しない。添付付きSendは現段階では案内を表示してdraftを維持する。MMS送受信への接続はws197-p010の後続作業。

この変更のhost試験は実際のGUI描画・実機MMS転送・動画再生を検証しない。Linux build/host fixtureとzedBSD named buildが検証範囲。
