<!-- awesome-plan project=zedbsd record=ws177-p020 -->

# ws177-p020: 音楽の準正常系の 1 — collection と cover（案 M）

Parent: [WS177](../ws.md)
Status: test-wait（T1-450、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q903（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 106・107・108・112（ws120-p008・p009）、[案](../phasing-20261008.md) の M。111（playlist など）はユーザーの決定で範囲外。106 の MP3・FLAC・WAV・Ogg は container の reader が無く範囲外（案の「外」の 106 の一部 形式）。

## 決めたこと（2026-10-08 P2）

- **106**: ~/Music の .mp4 は、絵があっても音があれば曲（音だけを鳴らす。前は絵のある .mp4 を外した）。曲の数の上限 4096 を外す（view の一覧も可変の大きさに）。folder は深さ 16 まで、同じ folder を二度辿らない（dev・inode を祖先と比べる、symlink の輪）。
- **107**: tags の cache（`$XDG_CACHE_HOME/music/tags`、無ければ `~/.cache/music/tags`）: file ごとに path・大きさ・mtime と title・artist・album artist・album・track・長さ・音・絵・cover の有無。大きさと mtime が同じ file は file を開かない。cover の bytes は cache に入れず、album の picture を初めて描く時にその曲の file から読む（起動の memory と時間を減らす）。folder の変化: 走査した folder の mtime を覚え、5 秒ごとに stat して変わっていたら cache を使って走査し直し、一覧を作り直す（再生中の曲と選んだ album は path と名前で引き継ぐ）。
- **108**: album の同じさ: 名前は大小（ASCII・Latin-1・Latin Extended-A・Greek・Cyrillic・全角の英字）と前後と続く空白を無視して比べる。album artist がある曲は（album 名、album artist）で 1 つ。無い曲は（album 名、folder）で 1 つにし、artist が曲で違えば album の artist を「Various Artists」に。folder は file の folder、その名が「CD n」「Disc n」「Disk n」なら親の folder（disc ごとの folder の album を 1 つに）。並びと検索も同じ大小の無視で。
- **112**: cover は正方形でなければ中央を正方形に切り抜く（前は引き伸ばし）。読めない cover・4096 px を超える cover は音符の tile のまま、log に理由、最初の 1 回だけ notice「A cover could not be shown.」。

## 実装（2026-10-08 P2）

- `userland/desktop/music/library.c`（書き直し）: 深さ 16・祖先の dev/inode で輪を切る、tags の cache（`music-tags 1`、12 の tab 区切りの欄、path の順、`.new` に書いて rename、時刻は ns）、走査した folder と mtime（`mu_library_changed`）、外から足した file を覚えて `mu_library_rescan` で足し直す、album の鍵（大小と空白を畳んだ名前・artist・folder、album artist の有無）、同じ番号が別の folder → 別の album、Various Artists、後の曲が album artist を名乗ったらその名に、CD/Disc/Disk n の folder は親、cover は path だけ覚え `mu_library_cover_load` で初めて描く時に読む。曲の数の上限（MU_LIST_MAX）を外す。
- `tags.c`: `has_cover`。`cover.c`: 中央の正方形を切り抜いて縮める。
- `view.c`: 一覧を可変長（`view->shown`）、cover は描く時に読んで作り、失敗は log と最初の 1 回の notice「A cover could not be shown.」、`mu_view_forget_pictures`。
- `main.c`: 5 秒ごとに `mu_library_changed`、変わったら `mu_reload`（再生中・選んだ曲は path、album は名前と artist で引き継ぐ。再生中の曲が消えたら止めて「This song's file is gone.」、log `MUSIC RESCAN songs= albums= error=`・`MUSIC GONE`）。
- 制限: folder の変化は folder の mtime で見る（file をその場で書き換えただけでは次の folder の変化か起動まで古い tags）。同じ clock の tick の中の変化は次の変化まで見えないことがある（ext4 などの粗い時刻）。

## 確認

- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/music` → exit 0、warning 0（-Werror）。`python3 plan/tools/style-check.py userland/desktop/music/*.c userland/desktop/music/*.h` → 指摘なし。
- host（新規）: `sh plan/ws177/tests/host-music-m.sh build/ws177-p020/host-music-m` → PASS（ASan・UBSan、4 回連続）。大小と空白・Greek・Cyrillic・全角の畳み、Greek・Cyrillic の検索、Various Artists、後の曲の album artist、disc の folder、同じ番号の別 folder は別 album、10 段下、輪の symlink、絵と音の .mp4、cover の後読みと中央の切り抜き（左端が中央の緑）、cache の書き出しと利用（同じ大きさ・時刻の file は読まない）・時刻が変われば読み直す、folder の追加・移動の検出と rescan、外の file の保持。
- host（既存を追随）: `plan/ws120/tests/run-host-music-library.sh` → PASS（8 曲 4 album に、cover の後読み、cache を使わない）、`plan/ws120/tests/run-host-music.sh` → PASS（cache を使わない。album の鍵の修正で Demos が 1 つのまま）。
- QEMU: 未実施。T1 に `apps.music.play`（手順 1 に `MUSIC COVER album=0 error=0`、手順 6 に folder の追加と `MUSIC RESCAN songs=3 albums=2 error=0` を足した）を p021 と一緒に依頼する。
