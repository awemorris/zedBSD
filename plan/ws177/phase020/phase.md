<!-- awesome-plan project=zedbsd record=ws177-p020 -->

# ws177-p020: 音楽の準正常系の 1 — collection と cover（案 M）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P2 q903 の 1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q903（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 106・107・108・112（ws120-p008・p009）、[案](../phasing-20261008.md) の M。111（playlist など）はユーザーの決定で範囲外。106 の MP3・FLAC・WAV・Ogg は container の reader が無く範囲外（案の「外」の 106 の一部 形式）。

## 決めたこと（2026-10-08 P2）

- **106**: ~/Music の .mp4 は、絵があっても音があれば曲（音だけを鳴らす。前は絵のある .mp4 を外した）。曲の数の上限 4096 を外す（view の一覧も可変の大きさに）。folder は深さ 16 まで、同じ folder を二度辿らない（dev・inode を祖先と比べる、symlink の輪）。
- **107**: tags の cache（`$XDG_CACHE_HOME/music/tags`、無ければ `~/.cache/music/tags`）: file ごとに path・大きさ・mtime と title・artist・album artist・album・track・長さ・音・絵・cover の有無。大きさと mtime が同じ file は file を開かない。cover の bytes は cache に入れず、album の picture を初めて描く時にその曲の file から読む（起動の memory と時間を減らす）。folder の変化: 走査した folder の mtime を覚え、5 秒ごとに stat して変わっていたら cache を使って走査し直し、一覧を作り直す（再生中の曲と選んだ album は path と名前で引き継ぐ）。
- **108**: album の同じさ: 名前は大小（ASCII・Latin-1・Latin Extended-A・Greek・Cyrillic・全角の英字）と前後と続く空白を無視して比べる。album artist がある曲は（album 名、album artist）で 1 つ。無い曲は（album 名、folder）で 1 つにし、artist が曲で違えば album の artist を「Various Artists」に。folder は file の folder、その名が「CD n」「Disc n」「Disk n」なら親の folder（disc ごとの folder の album を 1 つに）。並びと検索も同じ大小の無視で。
- **112**: cover は正方形でなければ中央を正方形に切り抜く（前は引き伸ばし）。読めない cover・4096 px を超える cover は音符の tile のまま、log に理由、最初の 1 回だけ notice「A cover could not be shown.」。

## 確認

（実装の後に書く）
