<!-- awesome-plan project=zedbsd record=ws177-p021 -->

# ws177-p021: 音楽の準正常系の 2 — 再生の失敗と key（案 M）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P2 q903 の 2）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q903（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 113・114（ws120-p009）、[案](../phasing-20261008.md) の M。音の stream は WS191（kl_audio_stream、lost と開き直し）に合わせる。

## 決めたこと（2026-10-08 P2）

- **113**:
  - 再生中の decode の失敗（音の packet が続けて 16 個 decode できない）: 止めて notice「This song could not be decoded.」、次の曲へ。
  - 読みの失敗（mf_read の ENODATA 以外）: notice「This song's file could not be read.」、次の曲へ。
  - 曲の file が無い（開く時の ENOENT）: notice「This song's file is gone.」、collection を走査し直す（p020 の folder の変化と同じ道）、次の曲へ。
  - Files から開いた曲が鳴らせない: 理由の notice（音の無い file、libavcodec が無い、file が無い、読めない）。
  - 音の service が再生中に去った（kl_audio_stream の lost、WS191）: 再生を閉じ、stream を開き直して同じ曲を同じ位置から続ける。service が無ければ止めて「There is no sound: the sound service is not running.」。次に曲を選んだ時にも開き直す（vp_audio_renew）。
- **114**:
  - 検索の field に focus がある時の Space は field に打つ（今のまま、標準の振る舞い）。Escape と Enter で field を離れ、その後の Space は再生・一時停止。
  - Previous の 3 秒の規則は今のまま（3 秒より後は曲の始め、以内は前の曲）。
  - Next・Previous の連打: 同じ pass の要求を 1 つの step（和）にまとめ、曲を開くのは 1 回だけ。

## 確認

（実装の後に書く）
