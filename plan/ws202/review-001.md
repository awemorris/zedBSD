# WS202 design 第 1 版の敵対的 review（review-001）

対象: [design.md](design.md) 第 1 版（2026-10-10）、[ws.md](ws.md)、phase001〜phase014 の phase.md。
review の担当: design-reviewer（2026-10-10）。読むだけで、source・設計文書は変えていない。この file だけを新しく書いた。

照らした code（main の作業ツリー、2026-10-10）: `userland/desktop/libmedia/`（decoder.c・avcodec.c・bitstream.c・media-decoder.h・
media-private.h・exports.map・Makefile・engine.c）、`userland/desktop/mediafile/`（mp4.c・ts.c・mediafile.h）、`userland/desktop/videoplayer/`
（main.c・media.c・audio.c）、`userland/desktop/music/`（main.c・play.c）、`userland/desktop/libvulkan/`（instance.c・dispatch.c・video.c）、
`userland/tests/vkvideo-probe/`（main.c・h264.c・dpb.c・frame.h）、`src/drivers/gpu/i915/render/video.c`・`worker.c`・`device.c`、
`src/drivers/gpu/gpu.c`、`plan/ws083/`（ws.md・design.md・tests/）、`plan/ws075/tests/`・`plan/ws031/tests/`・`plan/ws035/tests/` の config、
`config/ci/config-amd64.mk`・`config/release/config-amd64-beta2.mk`、`tests/scenarios/apps/{music,videoplayer}/`、`plan/tools/aat/scenarios/helpers_music.py`、
`docs/reference/vulkan-video.md`、規則（AGENTS.md の後半、plan/guardrail.md、plan/coding-style.md）。

host で実際に確かめたこと（scratchpad の中だけで行い、tree には何も書いていない）:

- E1: `ffmpeg -r 25 -i plan/ws083/tests/streams/pb-high-352-pyramid.h264 -c copy -f mp4` と `pb-main-352` で、ffmpeg 7.1.5 は
  「Timestamps are unset」「pts has no value」を 15 回出し、`ffprobe` の packet は全部 pts = dts（ctts 無し）。その mp4 を ffmpeg が decode した
  frame hash は WS083 の `.sha256` と違い、frame の数も 15 → 12。`-fflags +genpts` を足しても同じ。
- E2: `ffmpeg -f lavfi -i sine… -c:a aac -f ipod` の .m4a の `elst` は 1 entry、segment_duration 1000（movie timescale 1000）、media_time **1024**。

重さ: **H**（このままでは目的・受け入れを満たせない、または回帰を起こす）、**M**（直さないと特定の条件で壊れる、試験で捕まらない）、
**L**（記述の誤り・小さな欠落）。推測は「推測」と書く。

---

## H（重い）

### H-01 p009 の instance の apiVersion 1.1 は zedBSD の libvulkan に拒まれる

- 根拠: `plan/ws202/phase009/phase.md:21`「共有の device: `pthread_once` で作る instance（apiVersion 1.1 を要求、probe と同じ）」。
  実際の libvulkan は 1.0 以外を拒む: `userland/desktop/libvulkan/instance.c:59-66`（`VK_VERSION_MINOR(version) != 0` で
  `VK_ERROR_INCOMPATIBLE_DRIVER`）。probe は 1.0 で、instance の拡張 `VK_KHR_get_physical_device_properties2` を有効にしている
  （`userland/tests/vkvideo-probe/main.c:292-302`）。video の family の問い合わせは `vkGetPhysicalDeviceQueueFamilyProperties2KHR`
  （同 main.c の `probe_video_family`、416 行付近）で、この拡張が要る（`dispatch.c:68-120` は instance の拡張の関数を有効にした時だけ返す）。
- 起こること: 5330 でも `vkCreateInstance` が失敗し、vkvideo は毎回 DEVICE。libavcodec の無い image では H.264 が一切再生されず、
  完了の条件 1 が満たせない。偽の Vulkan の host 試験（p009）はこの失敗を捕まえない。
- 直し: design §5.4 と p009 を「apiVersion 1.0、instance の拡張 `VK_KHR_get_physical_device_properties2`、関数は
  `vkGetPhysicalDeviceQueueFamilyProperties2KHR`・`vkGetPhysicalDeviceFeatures2KHR` 等の KHR の名で引く」に直す。「probe と同じ」の
  主張は probe の実物の行で裏付ける。host 試験の偽の表に「instance の apiVersion が 1.0、properties2 の拡張が有効」を足す。

### H-02 vkvideo-probe の DPB は MMCO 5 と frame_num の gap を扱わない（design は「ある」と書く）。seek の設計がこれに依存する

- 根拠: design.md:73-75（「DPB（`dpb.c`: …、frame_num の gap）」）・design.md:175（「MMCO 1〜6、frame_num の gap の補いを写す」）・
  design.md:226-227（非 IDR の I から始める時「frame_num の gap の補いは使わない」、leading の B と参照の無い P・B を「decode せずに捨てる」）。
  実際の probe: `userland/tests/vkvideo-probe/dpb.c:15-16`「Operation 5 and gaps in frame_num are not followed (the probe stops)」、
  `dpb.c:82-85`（gap で `"a gap in frame_num"` を返す）、`dpb.c:346`（`"memory management operation 5"` を返す）、
  `h264.c:786-788`（POC の計算も MMCO 5 を追わない）。
- 起こること:
  1. open GOP（x264 の `open-gop=1`、B pyramid の既定 `b-pyramid normal` は真ん中の B を参照にする）で非 IDR の I に seek すると、
     design は leading の参照 B を decode せずに捨てる。次の P の frame_num は I から 2 進み、写した DPB は「a gap in frame_num」を返し、
     次の IDR まで全 picture が失敗する。`h264-high-b-aac.mp4` の `--seek=2.5`（p012 の 8）と UAT の seek で再現する見込み（推測、x264 が
     open GOP の I を stss に入れる前提）。
  2. 参照の管理を誤って空の slot を参照すると、i915 は submit 全体を `VK_ERROR_DEVICE_LOST` で拒む（`src/drivers/gpu/i915/render/video.c:17-25`・
     `660-684`）。design §5.9 では decoder が失敗し player は FAILED、process の共有の device も「壊れた」になる。
  3. MMCO 5 のある stream（x264 は出さないが、他の encoder・放送の録画は出す）で POC が誤り、temporal direct・implicit weighted の B が化ける。
  4. p008 の基準「probe と全 picture で一致」（phase008:33-36、ws.md の完了の条件 2）は、probe が止まる・誤る所を写した code で同じく誤るので、
     この誤りを原理的に検出できない。
- 直し: design §1.4・§5.3 の事実を直し、p008 に「8.2.5.2 の gap（non-existing の frame を短期の参照として sliding window に入れる。
  slot は使わず、参照の list に載せない）」「MMCO 5（8.2.1 の tempPicOrderCnt、prevPicOrderCntMsb/Lsb・prevFrameNumOffset の reset、
  marking）」を新しい仕事として見積もりに足す（probe と一致の試験の外）。seek の後は「leading の picture を捨てても frame_num の gap の処理で
  DPB が保たれる」ことを host 試験（open GOP の stream の途中の sync sample から）で確かめる。参照の slot を submit の前に必ず検べ、
  空の slot を参照に積まない（DEVICE_LOST を起こさない）ことを vkvideo の host 試験の項目にする。MMCO 5・gap の stream の入手は
  U の項目にする（下の「ユーザーの判断」J4）。

### H-03 p002 の `h264-*.mp4` の作り方（raw の .h264 を `-c copy`）は ctts の無い mp4 を作る（E1 で確認）

- 根拠: design.md:477「WS083 の `streams/*.h264` を `-c copy` で mp4 に（6 本）」、phase002:34。E1 の通り、B のある 2 本は pts = dts の mp4 になり、
  ffmpeg 自身の decode でも hash と frame の数が WS083 の参照と合わない。
- 起こること: design §5.6 は表示順を container の pts で決めるので、B のある stream は decode の順で出て、`media-probe --video-hash` は
  WS083 の参照と一致しない。p008 の「mp4 の AU と probe の .h264 を比べる」試験も、入力の作り方が壊れたまま始まる。
- 直し: H.264 の mp4 は libx264 で**直に mp4 へ** encode し（WS083 の make-streams.sh と同じ合成の素材と x264 の引数、`-fps_mode passthrough`）、
  probe に渡す Annex B はその mp4 から `-c copy -bsf:v h264_mp4toannexb -f h264` で取り出す（同じ bytes を両方の試験に使う）。参照の hash は
  mp4 から `-fps_mode passthrough` で作る（ffmpeg の rawvideo の出力の既定は CFR で、開始の時刻のずれで frame を複製しうる。推測を含むので
  p002 で frame の数を ffprobe の `nb_read_frames` と照らす）。p002 の確認に「全 mp4 の video の packet に ctts がある（B のある物）」を足す。

### H-04 表示順を container の pts で決める設計は、mp4 以外と ctts の無い file で今の add-in より悪くなる

- 根拠: design.md:216-220（pts の小さい順、「pts が無い場合は…mp4 では起きない」）。今の add-in は送った packet の時刻を整列して
  出てくる picture に順に当てる（`userland/desktop/libmedia/avcodec.c:486-497`・`933-957`）ので、decode の順の時刻しか持たない
  container（AVI、ctts の無い mp4。E1 の file）でも表示順は decoder（POC）が決め、時刻は正しく付く。D1 で vkvideo は全 container の
  H.264 で add-in より先に試される（decoder.c の表、design.md:111-112）。
- 起こること: AVI の H.264（B あり）や E1 のような remux の mp4 で、自前の経路は B を decode の順に出す（絵が前後に揺れる）。libavcodec の入った
  release の image でも自前が先なので、今は正しく再生できる file が回帰する。
- 直し: 表示順は H.264 の規則（POC の順、`max_num_reorder_frames`／§5.3 の深さでの bumping、IDR・MMCO 5 で全部出す）で決め、時刻は
  add-in と同じ「送った packet の pts を整列し、出る picture に小さい順に当てる」にする。同じ pts の捨て（design.md:220）は不要になる。
  host 試験に「pts = dts の mp4」「AVI」を足す。

### H-05 mp4 以外の container の AAC・H.264 が自前の back end に来る（TS の AAC は回帰する）

- 根拠: 表は全 container の track に使われる（decoder.c:53-55、design D1）。TS の AAC は ADTS の header 付きの frame を 1 packet ずつ、
  private data 無し・codec 名 "adts" で渡す（`userland/desktop/mediafile/ts.c:972-975`・`1490-1499`・`1646-1648`）。design.md:274 は
  「private data が無い track は…raw の AU と見なす（ADTS ではない）」。TS の H.264 は avcC 無しの Annex B、mp4 の `avc3` は avcC に
  SPS・PPS が無くてよい。design.md:142 は open で avcC の SPS を読み、無い時の扱いが無い。
- 起こること: libavcodec の入った image でも、自前の AAC が TS の track を受けて ADTS の header を raw_data_block として読み、全 frame が
  EINVAL（Video Player は音無し、Music は 16 回で曲を止める）。今は再生できる .ts の音が消える回帰。TS・avc3 の H.264 は open で失敗するか
  誤った state で始まる（どちらになるかは実装次第、推測）。
- 直し: design に「container ごとの入力の形」の節を足す。AAC: ADTS（`codec_name` "adts" か header の syncword）なら header を読んで
  外し、ASC の代わりにする（または FORMAT を返して add-in に回すと決める）。H.264: avcC に SPS が無い時は最初の in-band の SPS・PPS まで
  session を作らない（open は profile・大きさ未知で受けるか、FORMAT にするかを決める）。mkv（CodecPrivate の ASC・avcC）は mp4 と同じ。
  `run-host-codec.sh` に TS（AAC・H.264）、mkv、AVI の試料を足し、回帰しないことを受け入れの条件にする。

### H-06 音の持ち越し（design §6.5）と player の `skip_before` の組み合わせで、seek の後に音がずれる

- 根拠: design.md:323-324（capacity を越える分は持ち越し、add-in のように捨てない）、phase003:27-29（`_push` は send、`_take` は sound）、
  phase006:25-28（send で push、receive は frame ごとに 1）。player は `time < skip_before` の frame で `media_decoder_sound` を**呼ばない**
  （`userland/desktop/videoplayer/media.c:608-616`、`userland/desktop/music/play.c:513-519`）。今の add-in は receive が frame を置き換えるので、
  呼ばれなかった frame は暗黙に捨てられる（`avcodec.c:468-515`・`537-615`）。
- 起こること: seek は video の sync sample の dts まで音の track を戻し（`mp4.c:392-458`）、そこから seek の目標までの frame（GOP 2 秒なら約 90 frame）は
  sound を呼ばれないまま push され続け、持ち越しの buffer に溜まる。目標を越えた最初の frame で溜まった全部が出て、音が数秒遅れる
  （A/V の同期が壊れる。上限の無い buffer なら memory も増える）。drain の後の残りも、次の receive が 0 なら出ない。
- 直し: `media-decoder.h` の約束として「`media_decoder_sound` は直前の `receive` の frame の分だけを変換する。次の `receive` は変換されなかった
  残りを捨てる」と書き、自前の back end もそうする。capacity は player が 8192 frame（`media.c:30`、`play.c:32`）で、1024 sample の
  frame は 8 kHz からでも 6144 frame に収まる。持ち越しが要るなら sound を 0 まで繰り返し呼ぶ約束にし、player も直す（D14 の変更として）。
  host 試験に「receive の後 sound を呼ばずに次へ」「seek の後の最初の sample の時刻」を足す。

### H-07 試験の image の構成（`config-media-hw.mk`）では p012 の C と p013 の UAT ができない

- 根拠: phase011:29-30・design.md:511-512「`config-media-hw.mk`: `include plan/ws075/tests/config-test-hw.mk` の後に filter-out libavcodec と
  media-probe」。`config-test-hw.mk:1-5` は「i915 test build on the passthrough of the 5330」（`plan/ws075/tests/test-hw.sh:2-5` は guest の
  serial の log から判定を読む）。その連鎖（`plan/ws031/tests/config-zdesktop-hw.mk:4-12` → `plan/ws035/tests/config-amd64-userland.mk:28-39`）の
  `ZEDBSD_USER_PROGRAMS` には **libmedia・videoplayer・music・openssh が無い**（grep で 0 件）。`ZEDBSD_GRAPHICAL_BOOT := n`（同 :10）。
- 起こること: `media-probe` は libmedia が無く動かない。Video Player・Music も無い。SSH（p012:28「SSH」）ができない。passthrough の guest で
  流すと AGENTS.md の「serial log を読んで判定しない」に触れ、証拠も「実機」ではなく「QEMU（GPU だけ passthrough）」になる。
  p013 の UAT（phase013:15「ユーザーが USB に書く」）は console login の試験の image になる。
- 直し: 2 つとも `config/ci/config-amd64.mk`（`CONFIG_DRIVER_PCI_I915 := y`、openssh・libmedia・videoplayer・music・audiod、graphical login、
  :19-47）を基にし、`ZEDBSD_USER_PROGRAMS := $(filter-out libavcodec,$(ZEDBSD_USER_PROGRAMS)) media-probe` にする（1 つの config で QEMU と
  5330 を兼ねてよい）。5330 は WS083 p005 と同じ「USB で起動した実機に SSH」と書き、passthrough は使わないと明記する。stream の置き場所は
  WS083 の例（`plan/ws083/tests/config-video-hang.mk:16` の `ZEDBSD_TEST_EXTRA_FILES += --file /root/ws083/…`）に倣いつつ、Video Player が
  kei で読める場所（`/root` は不可）にする。p011 の確認に「image の package の一覧に libmedia・videoplayer・music・openssh・media-probe があり
  libavcodec が無い」を足す。なお test-image.sh は `ZEDBSD_TEST_EXTRA_FILES` を make の command line で渡す（`plan/tools/guest/test-image.sh:57`）ので、
  config の `+=` が上書きされないか確かめる（推測、確認していない）。

---

## M（中）

### M-01 共有の device（D9）: `pthread_once` では作り直せない。DEVICE_LOST は process の外にも及ぶ。WS083 p007 に実は依存する

- 根拠: design.md:186-188（`pthread_once` で作り、最後の close で壊す）と design.md:249・phase010:21（DEVICE_LOST の後「次の open で作り直す」）は
  両立しない（`pthread_once` は process で 1 回）。kernel の GPU の session は instance ごと（`instance.c` の `instance_add_context` が node を
  open）で、hang した session は quarantine され video の submit・session の作成を拒む（`render/video.c:661-672`、WS083 R-S3）。
  video の「止まった」印は device 全体（`render/video.c:660-663` の `drv_i915_worker_video_state`）で、engine の reset と回復は WS083 p007 が
  実機で未確認（`plan/ws083/ws.md:53`・`:91`）。ws.md:59-60 は「p007 は依存しない」。
- 起こること: 作り直しの実装が `pthread_once` のままだと 2 回目の作成ができず、作り直しを足すと once と状態の印が食い違う。device だけ壊して
  instance を残すと同じ kernel session（quarantine 済み）を使い続け、回復しない。1 つの process の hang で、全 process の H.264 の再生が
  engine の reset まで（p007 が実機で動かなければ再起動まで）失敗する。再生の途中の DEVICE_LOST は libavcodec が入っていても再生の失敗になる。
- 直し: mutex と参照の数による遅延の作成にし、「壊れた」時は device と **instance** を壊して作り直すと書く。再生の途中で vkvideo が失敗した時に
  add-in へ切り替えるか（次の IDR から）を決める。ws.md の依存に「p007 の実機の結果で §5.9 の回復の形が決まる」と書くか、回復を
  「この file の再生を失敗にする」だけに縮める。

### M-02 picture の pool の枚数が 1 枚足りず、尽きた時の振る舞いが無い。memory の見積もりが無い

- 根拠: design.md:364・phase003:22-23（最大「深さ＋ring 8＋2」、空きが無ければ count まで malloc）。数えると、並べ替えの待ち行列は出す直前に
  深さ＋1、Video Player の ring 8（`videoplayer.h:35`）、window は `player->picture` と `vp_media_take` の取り替えの間の `taken` で 2
  （`videoplayer/main.c:760-766`）、decode 中の 1 枚で、深さ＋11 になりうる。engine は ring 4（`engine.c:38`）と shown で別の数。
- 起こること: 尽きた時に NULL → ENOMEM なら player は FAILED。4K（12.4 MB の NV12）では slot の image 17 枚と pool 26 枚で約 530 MB を 1 本の
  動画が持つ（推測の計算）。
- 直し: pool は「空きの list の上限」だけにし、上限を越える取得は malloc、上限を越える返却は free（尽きない）。memory の上限（例 1 decoder で
  slot＋pool が N MB を越えたら PROFILE）を決めて design に書く。host 試験で ring 8＋window 2＋深さ 16 の模擬を流す。

### M-03 level の比べ方: `maxLevelIdc` は `StdVideoH264LevelIdc` の列挙で、SPS の level_idc（51 等）と直に比べられない

- 根拠: design.md:193-194・237（maxLevelIdc 5.1 まで、外れれば PROFILE）。Vulkan の `maxLevelIdc` は列挙（5.1 は 15）、SPS は 10 倍の値と
  level 1b（level_idc 11＋constraint_set3、または 9）。
- 起こること: そのまま比べると全部が外れか全部が通る。また level_idc が 5.2 以上と書かれているが大きさ・DPB は範囲内の file（4K60・一部の
  encoder の 1080p）を断る。
- 直し: 変換の表を design に書く。level では断らず、実際の制約（大きさ、MB の数 36864、DPB の数）で判断し、level_idc は Vulkan に渡す時に
  maxLevelIdc に丸めるかを決める（規格の VUID に触れないか p009 で確かめる。確かめていない）。

### M-04 AAC の M/S を intensity と PNS の band に掛けてはいけない（design に無い）。PNS の試験はそれを捕まえない

- 根拠: design.md:297-300（4 PNS → 5 M/S（ms_used の band）→ 6 intensity）、phase006:17-19。ISO/IEC 14496-3 では M/S は右の channel が
  INTENSITY_HCB・HCB2 の band と NOISE_HCB の band には掛けない（intensity では ms_used は符号の反転、PNS では左右の同じ雑音の印）。
  `aac-tools-low` の基準は frame ごとの RMS の比 ±1 dB（design.md:493、phase006:35）。
- 起こること: noise の band に M/S を掛けると、その band のエネルギーが変わり左右が崩れるが、frame 全体の RMS の ±1 dB に埋もれる。
- 直し: §6.3 に除外を書く。試験に「PNS・intensity の band を除いた band の波形の比べ」と「PNS の band ごとのエネルギーの比」を足す
  （channel ごと、scalefactor band ごと）。

### M-05 AAC の精度の試験の合わせ方が決まっていない（priming、channel の順、ffmpeg の command、無音の frame）

- 根拠: phase002:23-24（参照は ffmpeg が edts を当てた float）と phase006:32（試験は `aac_decode_frame` を直に呼んでよい = 切り詰め無し）。
  phase006:33「`-c:a aac -f f32le`」（`-i` の後に置くと encoder の指定になり失敗する）。5.1 は ffmpeg の出力が FL FR FC LFE BL BR、bitstream は
  SCE(C)・CPE(L,R)・CPE(Ls,Rs)・LFE。design.md:136・471 の「priming は 2112」は E2 の通り ffmpeg の encoder では **1024**。
  iTunes の file は priming を `iTunSMPB` の metadata で示す物があり、design は読まない。
- 起こること: 1024 sample ずれて全部 FAIL するか、実装の担当が基準を合わせるために試験を曲げる。5.1 は channel の対応が無く比べられない。
  `media-probe --audio-rms` の「相対 1e-4」（design.md:505）は無音・ごく小さい frame で 0 除算・ノイズで外れ、target（clang）と host（gcc）の
  浮動小数の違い（FMA の縮約）でも揺れうる（推測）。
- 直し: 参照は `ffmpeg -c:a aac -i X -c:a pcm_f32le -f f32le -`（decoder の指定は `-i` の前）と書き、比べる口（切り詰めの後か前か）を 1 つに決める。
  5.1 の channel の対応の表を design に書く。RMS の比べは「絶対の下限（例 -90 dBFS 未満の frame は絶対差で判定）＋相対」。priming の記述を
  1024 に直し、iTunSMPB を読むかを決める（読まないなら UAT の iTunes の曲の頭の約 46 ms の無音・切れを制限に書く）。

### M-06 resampler: 位相 256 の最近傍では SNR 90 dB に届かない。帯域の仕様が無い

- 根拠: design.md:319-320、phase003:30（32 tap、Kaiser、位相 256）、phase003:53（1 kHz の 44.1→48 kHz で SNR ≧ 90 dB）。
- 起こること: 位相を最近傍で選ぶと時刻の誤差は最大 1/512 sample で、1 kHz・44.1 kHz で約 2π·(1000/44100)/512 ≈ 2.8e-4（約 -71 dB）、
  高い周波数ではさらに悪い（計算による見込み）。32 tap で 90 dB の阻止域にすると遷移帯域が入力の rate の約 0.18 倍になり、44.1 kHz で
  約 14 kHz から上が減衰するか折り返しが残る。1 kHz だけの試験はこれを見ない。48 kHz の入力（動画の大半）でも filter を通る。
- 直し: 位相の間を線形補間する（または 44.1→48 の 147:160 のような有理の比は正確な位相の表）と書き、通過域（例 0〜18 kHz で ±0.1 dB）と
  阻止域の目標を決め、試験を 1・10・18 kHz と sweep にする。入力が 48 kHz の時は通さない速い道を足す。

### M-07 decode と desktop の描画が kernel の 1 本の worker を取り合う（U8）のに、受け入れの基準が無い

- 根拠: design.md:154-155（D5 同期）・564（U8 は p012 で記録だけ）。`plan/ws083/design.md:307`・`420`（decode と描画は互いに待つ）。
  1080p で 1 decode 4.2〜5.2 ms（design.md:71）。
- 起こること: 1080p30 で worker の時間の約 15%、60 fps の動画で約 30% を video が塞ぎ、compositor の frame が落ちうる（推測）。p012 は
  late の数（player の側）だけを基準にし、desktop の側の劣化を合否にしない。
- 直し: p012 の受け入れに「1080p30 の再生中の compositor の fps（log）が再生していない時の X% 以上」を足し、満たさない時の扱い
  （別の WS で非同期化、または解像度の上限）を design に書く。

### M-08 Music の AAT の helper が `MUSIC CODEC load error=0` を待つ。p007 は helper を直す範囲に入れていない

- 根拠: `plan/tools/aat/scenarios/helpers_music.py:78`・`:87`（`MUSIC CODEC load error=\d+` を待ち、`error=0` でなければ
  「libavcodec did not load」）、`tests/scenarios/apps/music/play.md` の手順 1 の正解。この行は Music の起動の `media_codec_load()`
  （`userland/desktop/music/main.c:191`）から `avcodec.c:768` の `media_log` で出る。p007（phase007:17）はこの呼び出しを外す。
- 起こること: p012 の A-3（libavcodec 無しの QEMU の Music の scenario）が helper の検べで FAIL する（行が出ない、または `error=1`）。
- 直し: p007 の成果に `helpers_music.py` の codec の検べの変更（`MUSIC PLAY open codec=aac backend=libmedia` を見る）と scenario の手順 1 の
  正解の変更を足す（plan/tools は Q1 へ差分）。

### M-09 vkvideo の back end が本物の GPU に触れるのは p012 が最初（p008→p009→p010→p011 の後）

- 根拠: ws.md:79-82、phase009:53、phase010。host の試験は偽の Vulkan の表だけ。
- 起こること: session・image・DPB の計画・de-tile の誤り（H-01 のような）が WS の最後に見つかり、AAC の列を含む T1 の 1 回の依頼が FAIL して
  p009・p010 に戻る。
- 直し: media-probe（の video の部分）を p010 に移し、p010 の終わりに T1 へ「5330 で WS083 の 6 本の mp4 の hash」の小さい依頼を出す
  （AGENTS.md の「意味のまとまった単位」で許される範囲）。p011 は Video Player・文書・音の probe に絞る。

### M-10 media-probe の音の口が p011 まで決まらないのに、p002 が参照の形を先に決める。試験の口が export される

- 根拠: design.md:503-505・phase011:24-26（`media_decoder_sound_float` を足すか 16 bit の RMS にするかは p011 で決める）、phase002:23-24
  （参照は core の rate・float・channel ごと）。`exports.map:158` の `media_decoder_*` の wildcard は新しい関数を全部 export する。
- 起こること: p011 で 16 bit を選ぶと p002 の参照を作り直す（p012 の直前の手戻り）。float の口を足すと試験のための関数が全 app の ABI になる。
- 直し: 今決める。案: host 試験が float を比べ（p006）、target の media-probe は 16 bit・48 kHz・stereo の出力の RMS を、同じ host の ffmpeg の
  `-ar 48000 -ac 2` の参照ではなく**自前の decoder を host で走らせた結果**と比べる（target と host で同じ code）。export しない内部の口が要るなら
  名前を `media_decoder_*` から外す。

### M-11 HE-AAC の (a) の経路（degraded の 2 段の試し、SBR の signalling の読み）を試す stream が無い

- 根拠: design.md:333-335（host の ffmpeg は HE-AAC を作れない）、ws.md:92、phase013:25（UAT の「手元にあれば」）。design.md:271-272 は
  `syncExtensionType 0x2b7` を「暗黙の signalling」と呼ぶが、これは後方互換の明示の signalling（暗黙は ASC に何も無く FIL の SBR だけ、
  design.md:335 の方）。
- 起こること: 2 段の試し・ASC の AOT 5/29・0x2b7 の読みが一度も動かずに release に入る。用語の取り違えで実装の担当が分岐を誤りうる。
- 直し: LC の .m4a の esds の ASC を host の script で書き換え（AOT 5 の明示、0x2b7＋sbrPresentFlag）、core は LC のまま鳴ることと
  degraded の時だけ受けることを host 試験にする。用語を直す。

### M-12 seek の後の音の位置の誤差の記述が逆で、rate に依る

- 根拠: design.md:398「最大 21 ms 早く始まる」。player は `time < skip_before` の frame を丸ごと捨て（`media.c:608-611`）、時計を目標の時刻に
  合わせる（`media.c:673-677`）。
- 起こること: 最初に書かれる sample は目標より**後**（最大 1 frame）なので、音は絵より最大 1 frame 遅れる。8 kHz なら 128 ms、22.05 kHz なら 46 ms。
- 直し: 記述を直す。自前の AAC は sample の時刻を正確に知るので、back end が `skip_before` の位置で切る口（または receive が返す時刻を frame の
  途中にする）を設けるかを決める（D14 を見直す）。

### M-13 共有の表の初期化の同期が書かれていない

- 根拠: design.md:296・307・320、phase005:37（Huffman の 2 段の表、`|q|^(4/3)`、KBD、twiddle、sinc を init の時に計算）。
- 起こること: browser の engine の thread と別の decoder が同時に初めての open をすると、表の作成が競合する（data race）。
- 直し: 表は `pthread_once` で 1 回作る（または static const に生成する）と design に書き、host 試験を 2 thread の同時 open で TSan に通す。

---

## L（軽い）

- L-01 open の場所: Video Player は `vp_media_open` の中、つまり window の thread で decoder を開く（`videoplayer/media.c:97-103`）。vkvideo の
  open（instance の作成、capability、session、slot の image 17 枚の確保と bind）は UI を止める。D5 は decode だけを media の thread と書く。
  時間を p012 で測り、長ければ open を media の thread へ移す（player の変更）。
- L-02 libvulkan の dlclose: 最後の close で dlclose・再 dlopen を繰り返すと libvulkan の static な状態の作り直しに頼る。一度読んだら
  dlclose しないと書く。
- L-03 VUI の読み誤り（途中で切れた VUI）で SPS 全体を捨てない（probe は `bits.error` で SPS を拒む: `h264.c:540-542`）。VUI の誤りは
  VUI だけを捨てる。`colr` の `nclc`（QuickTime）も読むか決める。SAR は VUI と pasp のどちらを先にするかの理由を書く。
- L-04 log の互換: 今の `FRAMES shown=%u time_ms=%lld` は 1 枚目と 100 枚ごと（`videoplayer/main.c:770-771`）。`late=` は足すだけにし
  `time_ms=` を残す。p012 の 10 秒の判定のために、止めた時・終わりにも 1 行出す。
- L-05 `media_decoder_sound` の `rate` の引数を自前の back end は無視して 48 kHz に固定する（phase003:27）。引数の rate に従うと書く。
- L-06 kernel の video の context は 8 個（`src/drivers/gpu/i915/worker.c:78`）。9 個目の process の open は失敗し、notice は「この GPU は
  decode できない」になる。数の上限の時の問題の code と文を分ける。
- L-07 並べ替えの待ち行列が満ちた時の `send` は EAGAIN を返す（player はそれで drain する: `media.c:493-515`）と書く。fence の
  ETIMEDOUT の後に資源を壊してよいか（GPU が使い続けていないか）を書く。
- L-08 冗長 slice（PPS の redundant_pic_cnt_present、`redundant_pic_cnt > 0`）は捨てる、ASO は Baseline で起こりうると §5.8 に書く。
- L-09 p004 の U1 の 1 GB 級の file は自分の `build/` に作り、消すのは Q1。測るだけなら 100〜200 MB で外挿で足りる。
- L-10 phase002:33 の `-channel_layout 5.1` は ffmpeg 7 では `-ch_layout 5.1`。
- L-11 試験の残し方（2026-10-06 の規則）: WS の終わりに残す回帰の試験（`run-host-aac.sh`、`media-probe`、`make-streams.sh` 等）を
  master.md の Tools に移す手順が p014 にも ws.md にも無い。p014 か WS の終わりの手順に足す。
- L-12 `plan/ws083/ws.md:86` は「既定は OFF」のままで、code（`src/drivers/gpu/i915/device.c:227-228`）と `docs/reference/vulkan-video.md` は
  既定 ON。WS202 の design（design.md:63）は正しい。WS083 の ws.md の更新は Q1 の仕事。
- L-13 p003 と p006・p010 で 2 人の担当が `decoder.c` の表、`media-private.h`、`Makefile`、`plan/tools/media/run-host-codec.sh` の期待を
  別々に変える。merge の順（Q1）を ws.md に書く。

---

## 規則との照合

- HAL・UAPI: 変更は無い（libvulkan・i915 も変えない）。HAL の承認は要らない。
- coding-style §12（試験だけの環境変数）: 偽の Vulkan の表は関数の表の差し替えで、環境変数ではない。ただし「Tests must exercise the default
  production path」に照らし、dlopen と `vkGetInstanceProcAddr` で表を埋める本番の道は host 試験で通らない。H-01 のような誤りはそこに出るので、
  M-09 の早い実機の確認が要る。
- 「試験の image は config.mk と `--file`（元は tree の中）だけ」: p011 の config は形として合うが H-07 の通り中身が足りない。p012 の 7
  （`/home/awe/zedbsd-media/` の 1080p を T1 が scp し、参照も T1 がその場で作る）は image の入力ではないが、完了の条件 1 がtree の外の試料と
  記録の無い参照に依る（下の J5）。
- license: 新しい code は Zlib、FFmpeg の code は写さない、は合う。H5 (a) は guardrail の「値は事実」の決定が WS141・WS037 に限られる
  （plan/guardrail.md の WS141 の節「他の WS の GPL の参照には適用しない」）ので、ユーザーの決定が要る（design が H5 として出している通り）。
  特許（AAC・H.264 の decoder を任意の add-in ではなく base の libmedia に入れること）は design に記述が無い（J3）。

## 確かめていない物

- ISO/IEC 14496-26 の基準の値（design の U6）、ISO/IEC 14496-3 の表の番号（phase005:34 の「4.129-4.147」）。
- FFmpeg 9.0.2 の `libavcodec/aactab.c` の表の形（H5 (a) の script が読めるか）。
- x264 の `open-gop=1` の I を ffmpeg の mp4 の muxer が stss に入れるか（H-02 の再現の前提）。
- ffmpeg の mov の demuxer が edts の末尾も切るか（E2 の file は segment が 1.000 s で、ffprobe の duration も 1.000 だった）。
- 1 つの kernel session（instance）の中の 2 つの video session（D9 の同時 2 本）を WS083 が実機で試したか。
- `ZEDBSD_TEST_EXTRA_FILES` を command line で渡す時に config の `+=` が残るか（H-07）。
- build・host 試験・QEMU・実機はどれも走らせていない（review だけ）。E1・E2 は host の ffmpeg 7.1.5 での確認。

---

## 設計者が直す物

H-01（apiVersion 1.0 と properties2）、H-02（MMCO 5・gap の事実と仕事、seek の DPB、空の slot の検べ）、H-03（mp4 の試験の stream の作り方）、
H-04（POC の表示順と add-in と同じ時刻の付け方）、H-05（ADTS・avc3・TS・mkv・AVI の入力の扱いと回帰の試験）、H-06（sound の約束）、
H-07（試験の image の config と 5330 の流し方）、M-01〜M-13、L-01〜L-11・L-13。

## ユーザーの判断に回す物

- J1（H1〜H6 は design の通り）: 加えて H2 (a) は release の image で全 container の AAC と（Intel の機械で）H.264 を自前が先に取るので、H-04・H-05 の
  直しが済むまで、または表を mp4・m4a だけに絞る（他の container は add-in のまま）かを選んでもらう。
- J2: browser の `<video>`（engine）は自動で自前の decoder（browser の process の中で Vulkan Video）を使う。WS074 を止めている間、engine だけ
  add-in のままにするか（design.md:457 は範囲の外としている）。
- J3: AAC・H.264 の decoder を base の libmedia に入れることの特許の扱い（今は libavcodec の任意の add-in で分けている）。
- J4: MMCO 5・frame_num の gap・冗長 slice の試験の stream の出典（ITU-T H.264.1 の conformance の bitstream を tree の外で使うか、手で bit 列を作るか）。
- J5: 1080p の試料（`/home/awe/zedbsd-media/`、tree の外）と T1 がその場で作る参照を完了の条件 1 に使ってよいか。使わないなら合成の 1080p を
  make-streams.sh で作る（tree に入れない大きさなら作り方だけを tree に置く）か。
- J6: 再生の途中で vkvideo が DEVICE_LOST になった時、libavcodec があれば add-in へ切り替えるか、その file の再生を失敗にするか（M-01）。
