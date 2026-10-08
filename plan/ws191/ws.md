<!-- awesome-plan project=zedbsd record=ws191 -->

# WS191: 再生の音を libkeiland の audio stream の口へ（compositor・backend、3 OS）

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG007
Objectives: O2
Parent: [Master](../master.md)
Queue: q895（P2、WS083 の後に回した 2026-10-08 午後）
<!-- awesome-plan-current:end -->

## 由来（2026-10-08 昼 ユーザー、クリックの回答）

WS188 p003 の検査で、動画（videoplayer/audio.c）・音楽（music）・libmedia が再生の音を zedBSD の audiod に直に流している（userland/base/audiod/protocol.h を include、AF_UNIX で AUDIOD_SOCKET_PATH、shm の ring）と分かった。ユーザー:「libkeilandのaudio streamの口に移す」（Guardrail「Bluetooth と Display も compositor 経由」の規則、サウンドも同様）。

## 到達目標

- libkeiland に音の stream の口（開く・形式・書く・止める・遅れの問い合わせ）を作り、compositor・libkeiland-backend が OS ごとに流す: zedBSD は audiod、Linux は alsa-lib を dlopen した PCM の default（普通は pipewire-alsa を通って PipeWire、ユーザーの決定 H2）、FreeBSD は OSS。音の出力は libkeiland-backend の中、公開 API は kl_audio_（ユーザー 2026-10-08 午後）。
- videoplayer・music・libmedia をその口に移し、境界の検査の許可の表の PENDING の行を外す。
- 遅れ（A/V の同期）と大きなデータの受け渡し（shm）を compositor を通しても保つ設計。

## Phase（案）

| Phase | 目的 | Status | 依存 |
| --- | --- | --- | --- |
| p001 | 設計（stream の口、compositor の中継か fd の受け渡しか、遅れ、3 OS の backend）、design-reviewer | planned | — |
| [p002](phase002/phase.md) | libkeiland・compositor・zedBSD の backend | in-progress（実装と host 試験 PASS、2026-10-08 夜） | p001 |
| p003 | app の移行（videoplayer・music・libmedia） | planned | p002 |
| p004 | Linux・FreeBSD の backend | planned | p002 |
| p005 | 規約の全文の見直し | planned | p003・p004 |
