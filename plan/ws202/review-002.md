# WS202 design 第 2 版の敵対的 review（review-002）

対象: [design.md](design.md) 第 2 版（§16 の対応表を含む）、[ws.md](ws.md)、phase001〜phase015 の phase.md。前の review は [review-001](review-001.md)。
review の担当: design-reviewer（2026-10-10、2 回目）。読むだけで、source・設計文書は変えていない。この file だけを新しく書いた。

照らした code（main の作業ツリー、`0bc3d4d0f`）: `userland/desktop/libmedia/`（decoder.c・avcodec.c・media-decoder.h・media-private.h・exports.map・Makefile）、
`userland/desktop/mediafile/`（mediafile.h・mp4.c・ts.c・avi.c・mkv.c）、`userland/desktop/videoplayer/`（media.c・main.c）、`userland/desktop/music/`（main.c・play.c）、
`userland/desktop/libvulkan/dispatch.c`、`userland/tests/vkvideo-probe/`（main.c・dpb.c・h264.c・Makefile）、`src/drivers/gpu/i915/render/video.c`・`video-mfx.c`・
`src/drivers/gpu/i915/worker.c`、`include/libc/vulkan/vk_video/`、`config/ci/config-amd64.mk`、`plan/tools/guest/test-image.sh`、`plan/tools/media/run-host-codec.sh`、
`plan/tools/aat/scenarios/helpers_music.py`・`helpers_apps.py`、`tests/scenarios/apps/{music,videoplayer}/`、`plan/ws083/`（ws.md・tests/make-streams.sh）、
`plan/agents/protocol.md`、`plan/master.md`（5330 の運用の行）、規則（AGENTS.md の後半、plan/guardrail.md、plan/coding-style.md）。

host で確かめたこと（scratchpad の中だけ。tree には何も書いていない）:

- **R1**: `ffmpeg -c:v libx264 -x264-params bframes=3:b-pyramid=normal:open-gop=1:keyint=30:min-keyint=30:scenecut=0 … h.mp4` の後
  `ffmpeg -i h.mp4 -c copy h.avi`。AVI の最初の `00dc` chunk は `000002ac 06…`（**4 byte の長さ付きの NAL**、Annex B ではない）、video の `strf` の 40 byte の後に
  avcC（`0164000dffe1…`）がある。
- **R2**: 同じ h.mp4 を `-bsf:v trace_headers`。2 つ目の GOP の非 IDR の I（frame_num 15、POC lsb 60、recovery point SEI の recovery_frame_cnt 0）の後、decode の順に
  B（**nal_ref_idc 0**、POC 58 = leading）→ P（frame_num 0、POC lsb 4、`num_ref_idx_l0_active_minus1 = 0`、modification で I だけを参照、MMCO 1 を 3 回で前の GOP の
  参照を外す）→ B-ref（POC 64）・b（62）・b（66）。**leading の picture は非参照の B 1 枚だけで、seek の後に frame_num の gap は起きない。**

重さは review-001 と同じ: **H**（目的・受け入れを満たせない、または回帰）、**M**（特定の条件で壊れる、試験で捕まらない）、**L**（記述の誤り・小さな欠落）。
推測は「推測」と書く。

---

## 1. review-001 の指摘は直ったか

| 指摘 | 判定 | 根拠 |
| --- | --- | --- |
| H-01 apiVersion | **直った** | design §1.4・§5.4 D9、p009 成果 1。`vkvideo-probe/main.c:289-302` が 1.0 と properties2 で一致。偽の Vulkan の試験の項目にも入った |
| H-02 MMCO 5・gap・seek の DPB・空の slot | **未解決（新しい H2-01）** | 事実の訂正（§1.4）と p015 の新設は済み。しかし non-existing の frame の扱いが i915 の規則と矛盾し、D18 と組み合わさると壊れる。gap の試験は x264 の stream では空振り（R2、M2-09） |
| H-03 mp4 の作り方 | 直った | §10.1・p002（x264 で直に mp4、Annex B は mp4 から、ctts の確かめ）。ただし `h264.avi` の作り方に新しい誤り（M2-08） |
| H-04 表示順と時刻 | **一部**（新しい M2-01） | D22 で POC の bumping と add-in と同じ pts の整列は入った。捨てた AU の pts を列から外す規則が無く、seek の後に絵が 1 frame 早くなる（R2 の stream で起きる） |
| H-05 container の入力 | 一部（M2-03、L2-09） | §3.1 で ADTS・in-band・mkv・AVI を書いた。暗黙の HE-AAC の ADTS を release の image でも自前が取る回帰が残る |
| H-06 音の約束 | 直った | §8.5 D23、p003 成果 3、p006 成果 3。player の `time < skip_before` で sound を呼ばない道（`videoplayer/media.c:610-614`、`music/play.c:515-519`）と合う |
| H-07 試験の image | 一部（M2-07） | `config-media.mk` は `config/ci/config-amd64.mk` に libavcodec・libmedia・videoplayer・music・openssh・audiod があるので形は正しい（確かめた）。5330 に誰がどう image を載せるかが実行できない形のまま |
| M-01 共有の device | 直った（M2-06 の残り） | D9 は mutex と参照の数。通常の close の時に instance を壊すかが書かれていない |
| M-02 pool | 直った | D11・D21 |
| M-03 level | 直った | `vulkan_video_codec_h264std.h:67` で 5_1 = 14、`render/video.c:132-133`。D20 の表は列挙と一致 |
| M-04 M/S の除外 | 直った | §6.3 の 5、§10.2 の band の比べ |
| M-05 AAC の精度の試験 | 直った | §10.2・p006（decoder の指定を `-i` の前、切り詰めの後で比べる、5.1 の対応） |
| M-06 resampler | **一部**（M2-04） | 44.1→48 は直った。down sampling（96・88.2・64 kHz → 48）の仕様が入力の rate 基準のままで折り返しが残る |
| M-07 描画の fps | **一部**（M2-05） | 受け入れの条件にはなったが、測り方が定義できない |
| M-08 Music の helper | 直った | p007。`helpers_music.py:79・87` の待ちを外す |
| M-09 早い実機 | 一部（M2-07） | p010 の T1 の小さい依頼は入った。5330 の起動の手順が実行できない |
| M-10 media-probe の音の口 | 直った | §10.3（host の同じ code との比べ、export しない） |
| M-11 HE-AAC の試験と用語 | 直った | §6.1 の用語、p005 の `gen-asc.py` |
| M-12 seek の後の音 | 一部（M2-02） | `media_decoder_trim` は入った。Music では trim が overlap の無い最初の frame の音を出す |
| M-13 表の初期化 | 直った | §6.9 |
| L-01 open の thread | 一部（M2-02 と同じ根） | session を最初の decode の前に作るので、open で BUSY・D20・D21 を返せなくなった |
| L-02〜L-05、L-07〜L-11、L-13 | 直った | §16 の場所で確かめた |
| L-06 video の context 8 個 | 一部（M2-02） | BUSY は足したが、context は最初の video session で付く（`worker.c:748-758`）ので open では分からない |
| L-12 | Q1 の側 | — |

---

## 2. 新しい指摘

### H2-01 frame_num の gap の non-existing の frame: design・p015・i915 の規則が互いに矛盾する（H-02 の直しが成り立たない）

- 根拠:
  - design §5.3（design.md:206-208）: non-existing の frame は「slot は使わず、参照の list に載せず」。
  - p015 成果 4（phase015/phase.md:23）: 「計画の参照の slot が picture を持たない（**non-existing の frame を含む**）時は、その picture を『捨てる』」。
  - probe の DPB は slot を添字にした entry で、計画の参照は「DPB の全ての参照の picture」（`vkvideo-probe/dpb.c:47-50`・`87-103`）。sliding window は non-existing を数に入れる
    （8.2.5.2）ので、DPB の entry（= slot）を持たずに sliding window に入れる形は今の data の形に無い。
  - i915 の実行器: 参照は全部「begin が bind した active な slot」でなければ submit 全体を拒む（`render/video.c:2621-2631`、拒めば DEVICE_LOST）。slot が active になるのは
    参照の picture を setup に decode した時だけ（`render/video.c:2650-2655`）。一方で MFD は non-existing の印を DPB の state で受け、hardware が参照の list を作る
    （`video-mfx.c:412-417`、`I915_VIDEO_REFERENCE_NON_EXISTING`。slice ごとの list は driver が渡さない）。
- 起こること（どちらの読みでも壊れる）:
  1. p015 の読み（non-existing を計画に含め D18 で捨てる）: gap の後、non-existing が sliding window で押し出されるまで（`max_num_ref_frames` 枚）全ての picture が捨てられ、
     捨てた参照の picture の slot も空になるので、次の IDR まで連鎖して全部捨てる。IDR が先頭にしか無い stream（x264 の open GOP は IDR が 1 つ、design §15 E3）では
     残り全部が出ない。
  2. design §5.3 の読み（Vulkan に渡さない）: hardware は non-existing の無い DPB から既定の list を作るので、slice の有効な index の中に non-existing の後ろの entry があると
     **別の picture を参照して化ける**。result status は COMPLETE のままで、D18 も捕まえない。
  3. 規格どおり `is_non_existing` で渡そうとしても、その slot は一度も decode されていないので active でなく、i915 は submit を DEVICE_LOST で拒む。
- どこで当たるか: `gaps_in_frame_num_value_allowed_flag` 1 の stream、MMCO・gap の conformance、参照の leading の picture を持つ open GOP を seek した時（Blu-ray の rip に多い、
  推測）。release の image では libavcodec の代わりに自前が先に取るので、今は再生できるこれらの file の回帰になる。受け入れの試験（x264 の stream）はこれを通らない（M2-09）。
- 直し（どれかを design で決め、p015・p009 に書く）:
  - (a) 参照の list の構成（8.2.4、P と B の初期の順、modification、`num_ref_idx_active`）を libmedia で計算し、non-existing が各 list の有効な範囲に入らない時だけ
    Vulkan の参照から外して decode する。入る時はその picture を捨てる（規格の「推定した参照の使用は誤り」の扱い）。probe に無い新しい仕事（+2〜3 LW の見込み）。
  - (b) non-existing を、参照から外れたがまだ active な slot（前の picture が sliding window で unused になったが begin で deactivate していない slot）に載せ、
    `is_non_existing` を付けて渡す。i915 の規則の抜け道に頼るので WS083 の design と照らし、5330 で確かめる必要がある。
  - (c) gap を見たら次の IDR（または recovery point SEI の付いた I）まで捨てる、と割り切り、制限として docs に書く。
  - どれでも、合成の gap の stream（x264 の stream から参照の P を 1 枚抜き SPS の gap の flag を 1 にした物、または J4）で host と 5330 の両方を確かめる項目を p015・p012 に足す。
    (b) で i915 の変更が要る、と分かったら WS083 の範囲なので Q1 に戻す（WS202 は i915 を変えない）。

### M2-01 捨てた AU の pts が整列の列に残り、seek の後に絵が音より早く出る

- 根拠: design §5.6（design.md:259-260）・p010 成果 1: 「送った packet の pts を整列した列に入れ、出る picture に小さい順に当てる」。send で捨てる AU（§5.7 の leading の B、
  最初の I の前の AU、D18、冗長 slice だけの AU）と、result status ERROR で出さない picture の pts を列から外す規則が無い。
- 起こること: R2 の通り `h264-high-b-aac.mp4` の seek の先の I の後には leading の B が 1 枚あり、それを捨てると列に B の pts（I より 1 frame 前）が残る。I にはその pts が
  当たり、以後の全ての picture が 1 つ前の picture の時刻で出る。25 fps で絵が音より 40 ms 早いまま、次の flush まで直らない。in-band の track の先頭の捨て・ERROR の picture
  でも 1 枚ごとにずれが増える。
- 試験で捕まらない: `media-probe --video-hash` は hash だけを比べ、時刻を見ない。p010 の「時刻が単調、数が AU の数と同じ」は seek の後を見ない。
- 直し: send がその AU を捨てると決めた時、その packet の pts を列から（値で）外す。出さない picture（ERROR）も同じ。host 試験に「seek の後の各 picture の時刻が ffmpeg の
  `-show_frames` の pts と一致」を足し、media-probe は hash と一緒に時刻を出して参照の pts と比べる（L2-01 の位置合わせも解ける）。

### M2-02 BUSY・D20・D21 を open で返せない（session を最初の decode の前に作るため）。失敗は notice にならず黒い絵になる

- 根拠: design §5.1 の 1（open は device の参照と parser だけ、session・image は最初の decode の前）、p009 成果 2（capability・D20・D21・session は最初の decode の前）。
  video の hardware の context は最初の video session で付く（`worker.c:748-758`「the first video session of the session attaches the hardware context」、尽きれば ENOMEM）。
  一方 design §9.1・§9.3 は `MEDIA_PROBLEM_BUSY` と PROFILE（大きさ）の notice を open の問題として出す。
- 起こること: 9 個目の process・maxCodedExtent を越える file・D21 を越える file は open が 0 を返し、最初の send で失敗する。Video Player の `media_feed` は send の誤りを
  見ずに drain する（`videoplayer/media.c:497-511`）ので、絵は出ず音だけ進む。libavcodec があっても表の次へは回らない（open は済んでいる）。BUSY の notice は一度も出ない。
- 直し: (a) open で capability を問い、session（image を除く）を作る。video の context もここで付くので BUSY・D20・D21 が open の問題になり、libavcodec へも回る。
  session の作成の時間を L-01 の測りに含める。または (b) decoder が後から問題を報告する口（例 `media_decoder_problem`）を足し、player が send の EIO で notice を出す。
  (a) を推す（表の 2 段の試しと整合する）。

### M2-03 暗黙の signalling の HE-AAC（ADTS）を、libavcodec の入った release の image でも自前の LC の core が取る

- 根拠: design §3.1（design.md:142）「AAC の ADTS は 1 段目で受ける」、§6.6（暗黙の signalling は 1 段目で LC として受け core の rate で鳴らす）、J1 の理由「回帰の原因を
  取り除いた」。ADTS の header には SBR・PS の印が無く、open の時には packet も無い。
- 起こること: HE-AAC の ADTS（HLS の録画の .ts に多い。推測）と暗黙の signalling の mp4 は、今は libavcodec が SBR・PS まで復号しているが、第 2 版では release の image でも
  自前が先に取って、高域の欠けた音（SBR 無し）、HE-AAC v2 は **mono**（PS の core は mono）になる。J1 の推しの根拠と違う。
- 直し: J1 の判断の材料にこの回帰を書く。設計の選択肢: (a) 1 段目では ADTS と、ASC の rate が 24 kHz 以下（HE-AAC の core の rate）の track を FORMAT にし、2 段目で受ける
  （経験則。48 kHz の LC の ADTS は 1 段目で受ける）。(b) 受けたまま制限として docs に書く。(a) を推す。

### M2-04 resampler の仕様が down sampling（96・88.2・64 kHz → 48 kHz）で折り返しを許す

- 根拠: design §6.5（design.md:348-350）・p003 成果 3: 通過域「0〜0.41×**入力**の rate」、阻止域「**入力**の Nyquist より上」。試験は 44.1→48 と素通りだけ（§10.2）。
  `aac-lc-96k.m4a` は試料にあるが resample の試験に使われない。
- 起こること: 96→48 で 24〜48 kHz の成分が 0〜24 kHz に折り返す。filter の cutoff を出力の Nyquist に下げる時、位相ごとの tap の数（64）で阻止域 −80 dB が保てるかも未定。
- 直し: cutoff を `0.5 × min(入力, 出力)` に、通過域・阻止域をその rate 基準に書き直す。down sampling では kernel を比で広げる（tap の数が増える）か、64 tap で足りる
  遷移帯域を決める。試験に 96→48 の折り返しの測り（36 kHz の sine の出力の成分 ≦ −80 dB）を足す。

### M2-05 compositor の fps の受け入れ（完了の条件 4、p012 の 11）が測れない形

- 根拠: ws.md 完了の条件 4、design §10.4（design.md:547）、p012 の 11。compositor は damage の時に描くので、何も動かない時の fps は 0 に近く「再生していない時の 90%」が
  意味を持たない（推測を含む。compose の作りから）。frame の log は `--log-frames` を付けた時だけ（`wayland/main.c:124`、`compose.c:468` ほか）で、graphical login の
  session の compositor にどう付けるかが書かれていない。
- 直し: 測りの負荷を決める（例: zgears か一定の animation の window を出し、その present の数を、動画の再生あり・なしで同じ 10 秒ずつ）。log を出す方法（session の
  compositor の起動の引数を config-media.mk で変えられるか、無ければ zgears 側の fps）を design に書く。数値の閾値（90%）はユーザーの判断に回す。

### M2-06 通常の close で共有の device と instance を壊すかが書かれていない（idle の process が video の context を持ち続ける）

- 根拠: design §5.1 の 6（参照を 1 減らす）、§5.4 の「壊れた」時だけ「参照の数が 0 になった時に device と instance を壊し」、p010 成果 1 も壊れた時だけ。video の context は
  device に 8 個で、kernel session（= instance）に付く（`worker.c:73-78`）。
- 起こること: 動画を閉じた後も開いたままの Video Player・browser の process が video の context と render の context を持ち続け、8 個が尽きると他の process が BUSY になる
  （推測。context が video session の破棄で外れるか、kernel session の close でだけ外れるかは読んでいない）。
- 直し: 参照の数 0 で常に device と instance を壊す（「video の無い機械」の覚えは残す）と書く。または壊さない理由と context の外れ方を `worker.c`・`render/video.c` で確かめて書く。

### M2-07 5330 での実行の道（p010 の 7、p012 の C、p013、完了の条件 1）が今の運用で実行できない

- 根拠: master.md:15（T1-435 で「Claude Code の安全の判定で T1 の ESP の書き込みが拒否された」、ユーザーが image を作り直した後に SSH で）、master.md:24（5330 の今の image に
  harness の鍵が無く `sshpass` の kei で入る）。design §10.4・p010 の 7 は「image `config-media.mk`、`test-image.sh --file`、5330 を USB で起動し SSH」と書き、誰が USB に
  書き、誰が起動するかを書かない。AGENTS.md の security の判定の規則（回避しない）から、T1 が ESP に書くことはできない。
- 起こること: p010 の「結果を待たずに次へ」の小さい依頼が、ユーザーの手（USB の作成・起動）が要ることに気づかれずに止まる。p012 の QEMU の A・B は流れても C が止まる。
- 直し: 道を決めて書く: (a) Q1 が `config-media.mk` の image を build し、ユーザーが USB で 5330 を起動する（p013 と同じ形。p010・p012 の C の依頼に「ユーザーの起動が要る」と
  書き、Q1 が予定を合わせる）。(b) 今の 5330 の image（libavcodec 有り）に `libmedia.so`・`media-probe`・stream を scp で入れて流す（ユーザー 2026-10-08「アップデートや再起動は
  自由に」）。(b) では 2 段目の試し（`h264.ts`・`h264.mkv` の自前）と DEVICE の notice が確かめられないので、それは (a) に残す、と項目ごとに分ける。

### M2-08 `h264.avi` を `-c copy` で作ると長さ付きの NAL になり、どちらの back end も読めない（R1 で確かめた）

- 根拠: design §10.1（design.md:496）・p002 成果「`h264.{mkv,ts,avi}`（`-c copy`）」。R1 の通り ffmpeg の AVI は avcC を `strf` に置き、chunk は長さ付き。`avi.c` は H.264 の
  `strf` の後ろを private data にしない（`mediafile/avi.c:668-670`「H.264 in AVI carries its own in the stream」）。
- 起こること: libmedia は Annex B と見て start code を探し、全 AU が EINVAL。p010 の `run-host-codec.sh` の AVI の項と、§3.1 の「AVI は in-band だけ」の試験が、作り方の誤りで
  FAIL し、実装の担当が avi.c を直す（範囲の外）か試験を外すかに迷う。
- 直し: AVI だけ `-c copy -bsf:v h264_mp4toannexb` を書く。TS は ffmpeg の mpegts の muxer が自動で Annex B にする（推測、p002 の確認で chunk の先頭が `00000001` かを見る）。
  p002 の確認の表に「`h264.avi`・`h264.ts` の最初の video の packet が start code で始まる」を足す。

### M2-09 gap の host 試験（p015 の 5 の 1 つ目）は x264 の stream では空振りする。gap・MMCO 5 は実機で一度も通らない

- 根拠: R2: seek の先の I の後の leading は**非参照**の B 1 枚で、frame_num の gap は起きない。p015 の基準「捨てた数は leading の B だけ、D18 で捨てる物が 0」は gap の処理が
  無くても PASS する。J4 の conformance は host の parse だけで、p012 の 14 は「使う時」の任意。
- 起こること: H2-01 の誤りが、全ての受け入れの試験を通って release に入る。
- 直し: 合成の gap の stream（H2-01 の直しの最後の項）を p002 の stream に足し、p015 の host と p012 の 5330 の必須の項目にする。design §5.7 の「捨てた参照の B の分の
  frame_num の欠け」の文は、x264 では起きない（R2）と事実を直し、起きる stream の種類を書く。

### M2-10 Music の seek で trim が overlap の無い最初の frame の音を出す（click）

- 根拠: design §8.2 D14（改）・p007。音だけの file では mp4 の seek の lead は最初の track（`mp4.c:407-417`）で、時刻以前の最後の sync の sample へ移る（`mp4.c:419-431`。stss の無い
  track は全 sample が sync: `mp4.c:1568-1574`）。flush の後の最初の frame は前の frame の overlap が無いので、IMDCT の前半の折り返しが打ち消されない。今は player が
  `time < skip_before` の frame を丸ごと捨てるので、その frame は出なかった（`music/play.c:515-519`）。trim はその frame の中の目標の sample から出す。
- 起こること: Music の全ての seek で、最初の最大 23 ms（44.1 kHz）に雑音・click が出る。Video Player では音の track は video の sync の dts まで戻るので、目標が sync から
  1 frame 以内の時だけ起きる。
- 直し: flush の後の最初の frame を pre-roll として出さない（AAC の back end の規則）と書き、その分 1 frame 前から読むように音だけの seek を 1 sample 前に戻す
  （mediafile の側、または player が目標 − 1 frame で seek して trim は目標で）。host 試験（`run-host-aac.sh`）に「seek の後の最初の 1024 sample が通しの decode と
  max |差| ≦ 2^-14」を足す。

### L（軽い）

- L2-01 seek の後の hash の位置合わせが決まっていない: `media-probe --seek=2.5` の出力は leading の B を含まず、参照の `.sha256` は時刻を持たない（WS083 の形）。どの行から
  比べるかを決める（M2-01 の直しの時刻付きの出力で解ける）。p010 の確認・ws.md 完了の条件 1。
- L2-02 ws.md 完了の条件 1「WS083 と同じ素材の 6 本の mp4、B・open GOP・crop・SAR・ctts 無し」は 6 本（WS083 の素材）と WS202 の 4 本を混ぜている。p012 の 6 は 6＋4＋2 本。
  数と名を揃える。
- L2-03 U14（1 つの kernel session の中の 2 つの video session）を確かめるはずの p012 の 12 は media-probe を 2 process で走らせ、2 つの kernel session を試す。同じ process で
  2 decoder を開く option を media-probe に足すか、U14 の文を直す。
- L2-04 design §7.3「emulation prevention の除去は別の関数（slice の header だけ）」は誤り。SPS・PPS・SEI も除去が要る（probe は全 NAL で除く: `vkvideo-probe/h264.c:35`・`254`）。
- L2-05 build の確認が「libmedia の build」「`… ZEDBSD_USER_PROGRAMS="…" …` の成果」で、target と warning の数え方が無い。WS083 の phase の形（`make -j16 BUILD=build/<担当>
  ZEDBSD_CONFIG=… build/<担当>/dynamic/libmedia.so build/<担当>/bin/videoplayer …`）で書き、warning は出力の `warning:` の行の数で 0 と書く。
- L2-06 p006 成果 6「今の試料の TS の AAC が回帰しない」: 今の `run-host-codec.sh` の TS は `h264-mp3.ts` だけで AAC の TS は無い。新しい `aac-adts.ts` だけが確かめる、と直す。
- L2-07 host の `run-host-codec.sh` は libmedia の全部を link するので、vkvideo が host の Vulkan を dlopen する。dlopen の名を `/lib/libvulkan.so`（image の path、U7）と
  書けば Debian の host では見つからず DEVICE に決まる。名だけ（`libvulkan.so`）だと Mesa を読み、Vulkan Video のある GPU の host では zedBSD だけの de-tile が走る（推測）。
- L2-08 ADTS の複数の raw_data_block（最大 4）: receive は 1 frame ずつなので、send が decode した frame を 4 つまで持つ列が要る。p006 成果 3 に書く。
- L2-09 ADTS の profile が LC でない（Main 等）ことは最初の send で分かり EINVAL になる（libavcodec へ回らない）。AVI の tag `0x706d`（LATM）は codec 名 "aac"・ASC 無しで
  2 段目の「raw の LC」に来て読めない（`avi.c:710-712`）。前者は制限、後者は open で FORMAT（codec 名か tag で分ける）と書く。
- L2-10 design §5.4「SPS の大きさ・profile・参照の数の変化で session・image を作り直す」と ws.md の制限「途中で解像度…が変わる mp4 は扱わない」が食い違う。どちらかに揃える。
- L2-11 2 段の試しで 1 段目と 2 段目の問題が違う時（libavcodec 無しの QEMU の `h264.ts`: 1 段目 FORMAT、2 段目 DEVICE）にどちらを返すかが無い。2 段目の問題を優先、と書く。
- L2-12 U12（kei が読めるか）は今決められる: `test-image.sh` は `--mode DEST=MODE` を持つ（`test-image.sh:12`）。依頼に `--mode …=0644` を書く。
- L2-13 p012 の 7 の 1080p は p002 の担当の worktree の `build/ws202-large/`。T1 の worktree からは見えない。T1 が `make-streams.sh --large` を自分で走らせる（bitexact で同じ物）
  と書く。
- L2-14 p014 の 3 は `media-probe` を `plan/tools/media/` へ移す物に数える。media-probe は `userland/tests/` の package なので移さず、master.md の一覧に載せるだけ。
- L2-15 J1 で container を絞る案の「track の container の名を見て」: `struct media_track` に container の名は無い（`mediafile.h:58-71`）。open に `media_file_format_name` を
  渡す口が要る。
- L2-16 非 IDR の I から始める時の POC は type 0 だけが書かれている（design §5.7）。type 1・2 の FrameNumOffset の始め方も書く。
- L2-17 full range の stream（`h264-main-crop-sar`）の参照を `-pix_fmt nv12` で作ると、swscale が range を変えないかが未確認（推測）。p002 で、native の yuv420p の出力から
  作った NV12 と bytes が同じことを確かめる。
- L2-18 design §1.4「memory type は 1 つで HOST_CACHED」: HOST_COHERENT でなければ読む前に `vkInvalidateMappedMemoryRanges` が要る。probe の形（invalidate 無しで 5330 で一致）を
  事実として書くか、invalidate を足す。
- L2-19 media-probe の package の依存は `desktop/libmedia`（vkvideo-probe は `desktop/libvulkan`、`userland/tests/vkvideo-probe/Makefile`）。p010 成果 3 に書く。
- L2-20 idle の process の Vulkan の instance は i915 の render の context（32 個、`worker.c:60-70`）も 1 つ使う（推測: instance ごとに node を open）。M2-06 の直しで一緒に解ける。

---

## 3. phase.md だけで実行できるか

| Phase | 手順・確認の command・成果の file | 足りない物 |
| --- | --- | --- |
| p002 | 具体的。stream の名・ffmpeg の引数・確認の表がある | M2-08（avi の bsf）、L2-17、L2-13、合成の gap の stream（M2-09） |
| p003 | 関数の名まで具体的 | M2-04、L2-05、L2-11、M2-02 (b) を選ぶなら問題の口 |
| p004 | 具体的 | なし（L2-05 の build の形だけ） |
| p005 | 具体的。H3・H5 の回答待ち | なし |
| p006 | 具体的 | M2-03 の選択、M2-10 の pre-roll、L2-08、L2-09、L2-06 |
| p007 | 具体的 | M2-10（trim を使う時の pre-roll） |
| p008 | 具体的 | L2-04 の注意 |
| p015 | 項目はあるが中身が成り立たない | H2-01 の方式、M2-09 の試料 |
| p009 | 具体的だが session の時期が誤り | H2-01（参照の渡し方）、M2-02（session を open で）、M2-06 |
| p010 | 具体的 | M2-01、M2-07、L2-01、L2-12、L2-19 |
| p011 | 具体的 | M2-02 (b) を選ぶなら notice の道 |
| p012 | 項目は揃っている | M2-05、M2-07、L2-03、L2-13 |
| p013 | 具体的 | M2-07（image を誰が作り誰が起動するか。p013 は「Q1 が build、ユーザーが USB」で正しい） |
| p014 | 具体的 | L2-14 |

依存の順（p003 → AAC の列・H.264 の列、p006 の後に p010 の表）は ws.md の merge の順と一致し、循環は無い。H2-01 で (b) を選び i915 の変更が要ると分かった時は、
WS083 への計画に無い依存になる（その時は p009 を uncleared にして Q1 に戻す）。

## 4. 規則との照合

- HAL・UAPI: 変更は無い。HAL の承認は要らない。libvulkan・i915 も変えない（H2-01 (b) の結果次第で WS083 の範囲の変更が要りうる → Q1）。
- 試験の image: `config-media.mk` は `include` と `filter-out` だけで、stream は `test-image.sh --file`（元は `plan/ws202/tests/streams/`）。規則に合う。1080p と J4 の stream は
  image の入力ではなく scp。規則に合う。
- rm: `host-common.sh` は `fresh_out` で rm を書かない。U1 の file・開発だけの試験は Q1 が消す。合う。
- serial・console log: passthrough を使わず、判定は SSH・media-probe の出力・撮影。合う。
- subagent の範囲: `plan/tools/media/`・`plan/tools/aat/`・`tests/scenarios/` は差分を Q1 へ。合う。`docs/reference/media-playback.md` は WS の成果として新しく書くので可、
  `docs/reference/vulkan-video.md` への 1 文は他の WS の文書の変更なので Q1 の確認を入れるのがよい（L）。
- coding-style: p003・p014 で全文を当てる。host の C も p014 の範囲。§12（試験だけの環境変数）は偽の Vulkan が関数の表の差し替えなので触れない。host の cc の
  `-std=gnu99` は今の `run-host-codec.sh` と同じで、書く source は ANSI C の形に従う（p014 で見る）。
- license: H5 (a)（LGPL の `aactab.c` から値だけ）と J3（特許）はユーザーの判断のまま。J4 の conformance の bitstream の利用の条件（U13）を p015 で読む前に、tree に入れない・
  `build/` だけ、は合う。

## 5. 確かめていない物

- 通常の close（video session の破棄）で i915 の video の hardware の context が外れるか（M2-06）。
- Blu-ray 等の open GOP の leading の picture が参照になるか（H2-01 の当たる範囲、推測）。
- HE-AAC の ADTS の普及の度合い（M2-03、推測）。
- ffmpeg の mpegts の muxer が H.264 を自動で Annex B にするか（M2-08、推測。R1 は AVI だけ確かめた）。
- swscale が full range の yuv420p → nv12 で bytes を保つか（L2-17）。
- build・host 試験・QEMU・実機はどれも走らせていない。R1・R2 は host の ffmpeg 7.1.5・libx264 での確認。

---

## 6. Phase ごとの判定

| Phase | 判定 | 条件 |
| --- | --- | --- |
| p001 | 条件付き GO | 第 3 版で下の「設計者が直す物」を直し、ユーザーの回答を記録する。H2-01 は方式の変更なので、直した後に 3 回目の review（H2-01・M2-01・M2-02 の部分だけでよい）を推す |
| p002 | 条件付き GO | M2-08、M2-09（合成の gap の stream）、L2-13、L2-17 |
| p003 | 条件付き GO | M2-04、L2-11。M2-02 で (b) を選ぶなら問題の口 |
| p004 | GO | — |
| p005 | GO（H3・H5 の回答の後） | — |
| p006 | 条件付き GO | M2-03 の選択、M2-10、L2-08、L2-09 |
| p007 | 条件付き GO | M2-10 |
| p008 | GO | L2-04 は p008 で直せる |
| p015 | **NO-GO** | H2-01 の方式を決め、M2-09 の試料と試験に直すまで |
| p009 | **NO-GO** | H2-01（参照の渡し方）、M2-02（session を open で作る）を直すまで |
| p010 | 条件付き GO | M2-01、M2-06、M2-07、L2-01・L2-12・L2-19（p009 の後） |
| p011 | 条件付き GO | M2-02 の選択に合わせた notice |
| p012 | 条件付き GO | M2-05、M2-07、L2-03、L2-13、gap の stream の 5330 の確かめ（M2-09） |
| p013 | 条件付き GO | M2-07（UAT の image の作り方は今の形で可） |
| p014 | GO | L2-14 |

## 7. 設計者が直す物

H2-01（方式の選択肢と試験の書き方。選択そのものは i915 の規則に触れる (b) の時だけ Q1 へ）、M2-01（捨てた AU の pts）、M2-02（session を open で）、M2-04（down sampling）、
M2-06（close の時の device）、M2-08（avi の bsf）、M2-09（gap の試料、§5.7 の事実の訂正）、M2-10（pre-roll）、L2-01〜L2-20。
M2-03・M2-05・M2-07 は設計の案を書いた上で、下のユーザーの判断に回す。

## 8. ユーザーの判断に回す物

- **J1 の材料の追加（M2-03）**: 自前の AAC を全 container に使うと、libavcodec の入った release の image でも暗黙の signalling の HE-AAC（HLS の .ts 等）が SBR 無し・
  HE-AAC v2 は mono になる。推し: 1 段目では ADTS と 24 kHz 以下の track を libavcodec に譲る（libavcodec の無い image では今の設計どおり自前の core で鳴る）。
- **H2-01 の方式**: (a) 参照の list の計算（+2〜3 LW、i915 は変えない）／(b) 空いた active な slot に載せる（i915 の規則に頼る、WS083 との照合が要る）／(c) gap の後は次の IDR まで
  捨てる（制限）。推し (a)。見積もりの増えと、(b) で WS083 の変更が要る時の扱いの判断が要る。
- **完了の条件 4 の測り方（M2-05）**: 測る負荷（例 zgears）と閾値（90% のままか）。
- **5330 の実行の道（M2-07）**: (a) ユーザーが config-media の image で USB 起動（p010 の小さい確認と p012 の C の 2 回、または p012 にまとめて 1 回）／(b) 今の 5330 の image に
  scp で入れて流す（2 段目の試しと DEVICE の notice は (a) に残る）。推し: p010 は (b)、p012 と p013 は (a) を 1 回の起動にまとめる。
- 既存の H1〜H6・J2〜J6 は review-001・design §13 のまま（変更なし）。
