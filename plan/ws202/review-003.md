# WS202 design 第 3 版の敵対的 review（review-003、範囲を絞った 3 回目）

対象: [design.md](design.md) 第 3 版の §1.4・§5.1〜§5.9（特に §5.3・§5.3.1 D25・§5.6 D29・§5.1 D28）、§16.2 の対応表、[p009](phase009/phase.md)・[p015](phase015/phase.md)・
[p016](phase016/phase.md)。前の review は [review-002](review-002.md)（p015・p009 が NO-GO、原因は H2-01・M2-01・M2-02）。他の Phase は §16.2 の反映を軽く見ただけ。
review の担当: design-reviewer（2026-10-10、3 回目）。読むだけで、source・設計文書は変えていない。この file だけを新しく書いた。

照らした code（main の作業ツリー、`3b697e81e`）: `userland/tests/vkvideo-probe/`（`dpb.c` 全文、`h264.c` の picture の組み立て・slice header・POC、`h264.h`）、
`userland/desktop/libvulkan/video.c`（`vkCreateVideoSessionKHR`・参照の flag の詰め方）・`instance.c`・`objects.c`、`src/drivers/gpu/i915/render/video.c`
（17〜25、646〜684、1040〜1100、2520〜2660、2291〜2372）・`video-mfx.c`（1〜22、386〜440）、`src/drivers/gpu/i915/worker.c`（60〜90、757〜803）・`session.c`（276〜290）、
`userland/desktop/libmedia/avcodec.c`（440〜460、925〜956）、`src/rtld/rtld.c`（1660〜1668）。H.264 の規格の本文は手元に無く、8.2.1・8.2.4・8.2.5 は記憶で照らした
（規格の文に頼る所は「推測」と書く）。

host で確かめたこと（scratchpad の中だけ。tree には何も書いていない。ffmpeg 7.1.5・libx264）:

- **R3-1**（ctts 無しの mp4 の参照）: design §10.1 と同じ作り方（`bframes=3:b-pyramid=normal:open-gop=1:keyint=30:min-keyint=30:scenecut=0` の 320x240・25 fps・4 s を
  直に mp4 → `-c copy -bsf:v h264_mp4toannexb -f h264` → `-r 25 -i X.h264 -c copy nocts.mp4`）。packet は 100 個で pts = dts（-1024 から 512 刻み、time base 1/12800）。
  `ffmpeg -i nocts.mp4 -fps_mode passthrough -pix_fmt nv12 -f framehash -hash sha256 -` は **98 行**（ctts 有りの元は 100 行）で、元の 1 枚目（I）と 5 枚目の hash が無い。
  `ffprobe -count_frames` の `nb_read_frames` も **98**。「Non-monotonic DTS」の警告が出る。
- **R3-2**（同じ 2 本の 2 つ目の GOP）: sync sample は 1・30・61・91（1 始まり）。ctts 有りでは packet 30 が I（pts 15360・dts 13824）、31 が leading の B（pts 14848 =
  I の 1 frame 前）。ctts 無しでは 30 が pts = dts = 13824、31 が 14336。
- **R3-3**（gap の試料の素）: `ref=3:bframes=3:b-pyramid=strict:keyint=60` の 25 fps・3 s は 75 frame で、key frame は **0 と 60 だけ**（scenecut は既定のまま）。
  `-c copy -bsf:v "noise=drop=eq(n\,40)"` で mp4 から packet を 1 つ抜くと、残りの packet の pts・dts はそのまま（ctts が保たれる）。

重さは review-001・002 と同じ: **H**（目的・受け入れを満たせない、または回帰）、**M**（特定の条件で壊れる、試験で捕まらない）、**L**（記述の誤り・小さな欠落）。

---

## 1. review-002 の 3 つの指摘は直ったか

| 指摘 | 判定 | 根拠 |
| --- | --- | --- |
| H2-01 欠けた参照 | **方式は成り立つ。定義の穴が 1 つ（新しい H3-01）** | D25（§5.3.1）は「渡した DPB から hardware が作る list」と「規格の list」を両方計算し、先頭 `num_ref_idx_active` 個の一致と欠けの無さで decode を決める。i915 の規則（参照は begin が bind した active な slot だけ、`render/video.c` 2621〜2633、active は setup の decode でだけ 2650〜2655）にも、MFD が short format で slice header から list を作ること（`video-mfx.c` 11〜15・386〜440）にも合う。slot を持たない参照を Vulkan に渡さず、参照の数は `max_references` を越えない（2618）。skip された decode は result status が ERROR（`render/video.c` 172・2291〜2295・2372）で、§5.9 の「ERROR の参照は欠けた参照に」と合う。ただし B の slice の「規格の list」を作るのに non-existing の frame の POC が要り、それが定義されていない（H3-01） |
| M2-01 捨てた AU の pts | **一部（新しい M3-02）** | D29 の「値で外す」は container の pts が本当の表示の時刻の時（ctts 有りの mp4・mkv・TS）は正しい。decode の順の時刻しか無い container（ctts 無しの mp4、AVI）では、外す値が捨てた picture の表示の位置と合わず、seek の後に 1 frame ずれる（R3-2 の数で再現する） |
| M2-02 session を open で | **直った**（L3-01 の細部） | D28 で session・image・pool まで open で作る。kernel の video の context は `vkCreateVideoSessionKHR` の中の `drv_i915_worker_context_attach`（`render/video.c` 1077）で付き、尽きると `VK_ERROR_OUT_OF_DEVICE_MEMORY`（1082〜1088）、engine の hang・quarantine は `VK_ERROR_INITIALIZATION_FAILED`（1044〜1049）。libvulkan は renderer の結果をそのまま返す（`objects.c` 474〜477、読んだ範囲。推測を含む）。**U10 は今答えが出る**: BUSY は `vkCreateVideoSessionKHR` の `VK_ERROR_OUT_OF_DEVICE_MEMORY` で分けられる |

M2-06（参照の数 0 で instance を壊す）も code と合う（context を外すのは `session.c` 287 の kernel session の close だけ）。

---

## 2. 新しい指摘

### H3-01 non-existing の frame の POC と POC の状態が決まっていない。B の slice で D25 の「規格の list」が作れず、h264-gap で黙って化ける

- 根拠:
  - design §5.3（design.md:227）・p015 成果 2（phase015/phase.md:21）は non-existing の frame を「短期の参照として sliding window に入れる」とだけ書き、その POC を書かない。
  - D25（design.md:240〜246）・p016 成果 1（phase016/phase.md:21〜22）の B の初期の順は「POC の前後の順」で、欠けた参照を含む DPB から作る。non-existing の frame の POC が
    要る。
  - 規格は non-existing の frame の sample を不定とし、inter 予測で参照してはならない、とする（8.2.5.2）。pic_order_cnt_type 0 の non-existing の frame の POC は
    slice header が無いので求まらない（推測: 規格の文は手元に無い。JM の `fill_frame_num_gap` は type 1・2 だけ POC を計算し、type 0 は計算しない、と記憶している）。
    x264 の B のある stream（h264-gap.mp4 の素）は type 0。
  - POC の状態も同じ穴: type 0 の `prevPicOrderCntMsb`・`prevPicOrderCntLsb` を non-existing の frame で更新するか、type 1・2 の `prevFrameNum`・`prevFrameNumOffset` を
    non-existing の frame ごとに進めるかが書かれていない（probe は gap で止まるので手本が無い、`dpb.c` 15〜16・82〜85）。
- 起こること: 実装の担当は non-existing の frame に何かの POC（0 や前の値）を入れる。例えば h264-gap.mp4 で参照の P を抜くと、元の stream では後の非参照の b の
  **L1[0] がその P** だった。POC 0 の non-existing は L0・L1 の末尾に並ぶので「規格の list」の有効な範囲の外になり、D25 は「decode」と判断する。hardware の list は
  抜けた P の次の picture を L1[0] にし、b は別の picture を参照して化ける。result status は COMPLETE で、D25 も D18 も捕まえない。p016 の host 試験の gap の項は
  「判定の列を記録」だけ（phase016/phase.md:32〜33）なので通り、5330 の p012 の hash で初めて分かる。type 0 の prev を non-existing で 0 にすると、以後の全 picture の
  POC が狂い、表示順と B の list が全部ずれる。
- 直し（design §5.3・§5.3.1、p015 成果 2、p016 成果 2 に書く）:
  1. non-existing の frame の POC: type 1・2 は 8.2.1.2・8.2.1.3 の式で nal_ref_idc ≠ 0 として計算する。type 0 は「不明」の印を持つ。
  2. POC の状態: non-existing の frame は type 0 の prevPicOrderCntMsb・Lsb を変えない。type 1・2 は prevFrameNum・prevFrameNumOffset を non-existing の frame ごとに
     進める（推測: JM・ffmpeg の扱いに合わせる。p015 で規格の 8.2.1 を読めれば照らす）。
  3. D25 の B の slice で、DPB に POC が不明の non-existing の frame がある時は、**その POC がどこにあっても**先頭 `num_ref_idx_active` 個に欠けた参照が入らず
     hardware の list と一致する時だけ decode する。簡単で十分な形: 各 list の先頭 N 個が modification の命令で全部決まる（N 個の命令が全部 slot を持つ別々の
     picture を指す）時だけ decode、他は捨てる。P の list は PicNum で決まるので今のままでよい。
  4. 捨てる picture が増える（gap の後、non-existing が sliding window で消えるまでの B）。J8 の判断の材料に書く。

### M3-02 D29 の「値で外す」は decode の順の時刻しか無い container で誤り。ctts 無しの mp4・AVI の seek の後に絵が 1 frame 早い（M2-01 が残る）

- 根拠: design §5.6（design.md:299〜303）・p010 成果 1（phase010/phase.md:18〜19）。整列した列は「出る picture に小さい順に当てる」ので、捨てた picture が占めるべき時刻は
  **その picture の表示の順の位置**で決まる。ctts 有りでは pts の順 = 表示の順なので「その packet の pts」で正しいが、ctts 無し（`h264-nocts.mp4`）と AVI の pts は dts で、
  表示の順の位置と合わない。
- 起こること（R3-2 の ctts 無しの数）: sync sample 30 へ seek すると、I（13824）と leading の B（14336）を送り、B を捨てて 14336 を外す。I は 13824 を受ける。
  通しの decode では B が表示の順で I の前なので B が 13824、I が 14336 を受ける。seek の後は全 picture が 1 frame（40 ms）早いまま、次の flush まで直らない。
  `h264.avi` でも同じ（avi.c の時刻は decode の順）。
- 試験で捕まらない: p010 成果 5 の D29 の確かめ（phase010/phase.md:34〜35）は「各 sync sample から seek した後の時刻が `.sha256` の pts と一致」で、stream を
  `h264-high-b-aac.mp4`（ctts 有り）・`h264-gap.mp4` にしか書いていない。ctts 無しの参照は M3-05 の通り ffmpeg から作れない。
- 直し: 捨てた picture（POC が分かる物: leading の B、D25 で捨てた picture、ERROR の picture）は、**POC を持つ空の entry として表示順の待ち行列に入れ**、bumping で
  出る時に列の最小の pts を 1 つ消費して捨てる。ctts 有りでは「値で外す」と同じ結果になり、ctts 無し・AVI でも通しの decode と同じ時刻になる。bumping の数え方も
  通しの decode と同じになる。POC の分からない AU（最初の I の前、parse の誤り）だけ値で外す。試験に「`h264-nocts.mp4` の各 sync sample から seek した後の時刻が、
  通しの decode で同じ picture（hash で合わせる）が受けた時刻と一致」を足す（ffmpeg の参照を使わない）。

### M3-03 `h264-gap.mp4` が回復と D25 の正しさを確かめない形

- 根拠と事実:
  - p002（phase002/phase.md:27〜31）・design §10.1（design.md:564）は「`keyint=60` の 3 s」の「2 つ目の GOP の中」から 1 つ抜く。R3-3 の通り 25 fps・3 s は I が 0 と 60 だけで、
    2 つ目の GOP（60〜74）の後に I が無い。open-gop も付けていないので 60 は IDR。
  - p016 の gap の試験（phase016/phase.md:32〜33）は判定の列を「記録」するだけで、正解と比べない。
  - gen-gap.py は「mp4 を書く」とだけあり、抜いた後の mp4 で ctts と各 sample の pts を保つ方法が書かれていない（raw の Annex B を `-c copy` で mp4 に戻すと ctts が
    無くなる、design §15 E1）。「元の stream の同じ pts の frame と一致」（design.md:574）は pts が保たれる時だけ意味を持つ。
- 起こること: ws.md 完了の条件 1・p015・p016 の「次の I の後は全部 decode」が一度も通らない（抜いた後に I が無い）。1 つ目の GOP から抜いても、回復は IDR で
  DPB を全部空にするだけで、問題の形（非 IDR の I の後に欠けた参照が DPB に残り、sliding window・MMCO で消えていく）を通らない。H3-01 の誤りも host で捕まらない。
- 直し:
  1. 素の stream を `open-gop=1:keyint=25:min-keyint=25:scenecut=0`、4〜5 s にし、抜く picture は 1 つ目か 2 つ目の GOP の中（後に非 IDR の I が 2 つ以上ある）。
  2. 抜き方は `ffmpeg -i h264-gap-orig.mp4 -c copy -bsf:v "noise=drop=eq(n\,K)" ...`（R3-3 で pts・dts が保たれることを確かめた）にし、gen-gap.py は avcC（と in-band）の
     SPS の flag を立てるだけにする。flag を立てて emulation prevention で SPS の長さが変わる時は box の大きさの直しが要る（moov が mdat の後なら stco は変わらない）ので、
     p002 の確認に「avcC の長さが元と同じ、または box を直した」を足す。
  3. host の正解との比べ（p016 に足す）: D25 が「decode」とした各 picture について、gap の stream の hardware の list（欠けを除いた DPB）の先頭 N 個が、
     **h264-gap-orig.mp4 の同じ picture（pts で合わせる）の規格の list の先頭 N 個**と同じ picture（POC で同定）を指すこと。H3-01 の誤りと D25 の誤りが host で捕まる。

### M3-04 seek の後の leading の**参照**の picture の marking が書かれていない（§5.3 の「seek の後の先頭の前の参照」は作れない entry）

- 根拠: design §5.3（design.md:228）は欠けた entry の 2 つ目の種類に「seek の後の先頭の前の参照」を挙げる。seek の前の picture は parse していないので frame_num・POC を
  持てず、entry は作れない。一方で §5.7（design.md:317）・p015 成果 5（phase015/phase.md:26）は leading の B を「捨てる」とだけ書き、nal_ref_idc ≠ 0 の leading の
  picture（Blu-ray 等の open GOP、design §5.7 の推測）を marking に通すかを書かない。
- 起こること: 参照の leading の picture を marking に通さないと、次の picture の frame_num が prevRefFrameNum から 2 つ飛んで gap と見なされ、POC の分からない
  non-existing の frame が代わりに入る（H3-01 の道）。H3-01 の保守的な直しの後は、non-existing が押し出されるまで B が捨てられ、seek の後に絵が止まる時間が延びる。
  x264 の stream（leading は非参照の B だけ、review-002 R2）では起きないので試験で捕まらない。
- 直し: 「leading の picture は decode しないが、参照なら D25 で捨てた参照と同じく、slot 無しの entry として marking（frame_num・POC・MMCO・sliding window）に通す」と
  §5.3・§5.7・p015 成果 5 に書き、§5.3 の「seek の後の先頭の前の参照」を消す。seek の前の picture を参照する trailing の picture は検出できない（list が短くなるだけ）ことを
  制限に書く（x264 は P の modification と MMCO 1 で避ける、review-002 R2）。

### M3-05 `h264-nocts.mp4` の参照を ffmpeg から作ると frame が欠ける（R3-1）

- 根拠: design §10.1（design.md:563・571〜573）・p002 は全 mp4 の参照を `ffmpeg … -f framehash` で作り、数を `ffprobe -count_frames` と照らす。p008 成果 3
  （phase008/phase.md:35）は nocts の表示順を `ffprobe -show_frames` の順と比べる。
- 起こること: R3-1 の通り、ffmpeg は ctts 無しの B のある mp4 で 100 枚のうち 2 枚を出さない。`nb_read_frames` も 98 なので p002 の数の照らしは通ってしまう。
  自前の decoder は 100 枚出すので p008 の表示順の比べ、p010・p012 の `--expect`、完了の条件 1 の nocts が偽の FAIL になる（または担当が decoder の側で frame を
  捨てて合わせる）。
- 直し: `h264-nocts.mp4` の参照は `h264-high-b-aac.mp4` の参照の hash（同じ bitstream、同じ表示の順）を使い、pts は「packet の時刻を整列して表示の順に当てた値」
  （mediafile の時刻から試験の script が作る）にする。p008 の nocts の比べも high-b-aac の POC の順と比べる。p002 の確認に「nocts の参照の行の数 = packet の数」を足す。

### L（軽い）

- L3-01（D28・BUSY の細部）: (1) BUSY は `vkCreateVideoSessionKHR` の `VK_ERROR_OUT_OF_DEVICE_MEMORY` だけに写す（`vkAllocateMemory` の同じ結果は本物の memory 不足）。
  U10 は閉じてよい（§1 の表）。(2) session の作成の `VK_ERROR_INITIALIZATION_FAILED`（engine の hang・quarantine、`render/video.c` 1044〜1049）で「video の無い機械」の
  覚え（§5.4）を立てない（一時的）。(3) open の ENOMEM が §9.1 の問題のどれになるかが無い。(4) 1 段目が BUSY・PROFILE で失敗した avc1 の track は、2 段目で vkvideo を
  もう一度試さない（参照の数 0 で instance を壊し、作り直して同じ失敗を繰り返す）。p009 成果 2 に書く。
- L3-02: p016 成果 4 の最後の項（phase016/phase.md:34）「欠けた参照が無い時に (1) と (2) が一致」は、同じ DPB から同じ手順で計算するので必ず一致し、何も確かめない。
  M3-03 の 3 の比べに置き換える。
- L3-03: entry を slot から分けた後も、probe の slot ごとの `device_active` と begin での deactivate（`dpb.c` 126〜133・160〜171）は slot の側に残す、と p015 成果 1 に書く。
  ERROR の参照の picture は i915 では slot が active（setup の decode で動く、`render/video.c` 2650〜2655）だが、libmedia では slot 無しの entry になる。
- L3-04: D29 の pts は send が受けた時だけ列に足す（EAGAIN で送り直す packet を二重に足さない）。今の add-in も受けた後に足す（`avcodec.c` 443〜452）。p010 に 1 行。
- L3-05: frame_num の gap が `max_num_ref_frames` より大きい時は、短期の参照を全部外して最後の `max_num_ref_frames −（長期の数）` 個の non-existing だけを入れれば同じ結果
  （MaxFrameNum 65536 の飛びで 1 つずつ回さない）。
- L3-06: p009 は p016 に依存する（ws.md の表、phase009/phase.md:9）が、p009 が要るのは D25 の判定の口と「slot を持つ entry だけを積む」計画の形（p015）だけ。p009 を
  p015 の後に p016 と並べると H.264 の列が 3 LW 短くなる（p010 が両方に依存）。
- L3-07: §14 U10（design.md:696）と p009 成果 2 の U10 の文（phase009/phase.md:34〜35）を、L3-01 の事実で閉じる。

---

## 3. §16.2 の反映の確かめ（軽く）

p002（gen-gap・`h264_mp4toannexb`・`--large`・start code の確認）、p003（`min(入力, 出力)`・128 tap・D27・`media_decoder_frame_us`・D30）、p004（`container`）、
p005（D26・複数の block）、p006（D30・pre-roll・LATM `0x706d`・4 つの block・`aac-adts.ts`・D26）、p007（pre-roll・`frame_us`）、p010（D29・scp・`--mode`・
`LD_LIBRARY_PATH`・`desktop/libmedia`・`--twice`）、p011（D28 の notice・BUSY）、p012（zgears・`--twice`・`h264-gap`・USB・`--large`）、p013（USB・config-media）、
p014（media-probe を移さない）に、§16.2 の行の語が入っていることを grep で確かめた。design の行の参照（`instance.c` 59〜66、`render/video.c` 17〜25・660〜684・
2621〜2631・2650〜2655、`video-mfx.c` 412〜417、`worker.c` 78、`session.c` 287、`libvulkan/Makefile` 23、`rtld.c` 1662〜1668）は code と合う。
中身の正しさは M3-02（p010）・M3-03（p002）・M3-05（p002・p008）の他は見ていない。

## 4. 規則との照合

- HAL・UAPI・i915・libvulkan の変更は無い（D25 は i915 の規則の中に収まる）。HAL の承認は要らない。
- 新しい試験の手段（`noise` の bsf）は host の ffmpeg だけで、tree の外の入力を使わない。rm を書かない形のまま。

## 5. 確かめていない物

- H.264 の規格の 8.2.1・8.2.4.2.3・8.2.5.2 の本文（non-existing の frame の POC、B の list の入れ替えが切り詰めの前か）。記憶と JM・ffmpeg の扱いの記憶による（推測）。
- MFD が渡した DPB から規格どおりに list を作ること（U16）は WS083 の実機の hash からの推しのまま。
- libvulkan の `vulkan_command_execute` が renderer の `VK_ERROR_OUT_OF_DEVICE_MEMORY` を変えずに返すこと（`objects.c` の流れから推測）。
- R3-1 は design の素材と同じ x264 の引数の別の素材（testsrc2 320x240）で確かめた。p002 の実際の `h264-nocts.mp4` で欠ける枚数は違いうる。
- build・host 試験・QEMU・実機はどれも走らせていない。

---

## 6. 判定

| Phase | 判定 | 条件 |
| --- | --- | --- |
| p015 | **条件付き GO** | H3-01 の 1・2（non-existing の POC と POC の状態）と M3-04（leading の参照の marking）を design §5.3・§5.7 と成果 2・5 に書く。L3-03・L3-05 |
| p016 | **条件付き GO** | H3-01 の 3（POC の不明な non-existing がある B の slice の判定）を D25 と成果 2 に書き、M3-03 の 3（h264-gap-orig との正解の比べ）を成果 4 に足し、L3-02 の項を置き換える。p002 の gap の試料を M3-03 の 1・2 に直す（p002 の条件） |
| p009 | **条件付き GO** | L3-01 を成果 2 に書く（U10 の答え、INITIALIZATION_FAILED で覚えを立てない、1 段目の失敗の後の 2 段目）。依存は p016 のままでも実行できる（L3-06 は計画の改善） |

方式の選び直しは要らない（H2-01・M2-02 の直しは成り立つ）。H3-01 は定義の追加で、設計者の直しだけで閉じる。4 回目の review は要らず、直しの差分を Q1 が照らせばよい
（推し）。他の Phase への波及: p010 は M3-02（D29 の形）と M3-05、p002 は M3-03 の 1・2 と M3-05、p008 は M3-05 の比べの直し。

## 7. 設計者が直す物

H3-01（§5.3・§5.3.1、p015・p016）、M3-02（§5.6 D29、p010 成果 1・5）、M3-03（§10.1、p002、p016 成果 4）、M3-04（§5.3・§5.7、p015 成果 5、ws.md の制限）、
M3-05（§10.1 の参照、p002・p008・p010）、L3-01〜L3-07。

## 8. ユーザーの判断に回す物

- 新しい判断は無い。J8（欠けた参照の方式）の材料に足す: H3-01 の保守的な判定で、gap の後は non-existing の frame が sliding window で消えるまで（最大
  `max_num_ref_frames` 枚の参照の picture の間）B の picture が出ない。(a) の利点（i915 を変えない、IDR が先頭だけの stream でも続きが出る）は変わらない。
