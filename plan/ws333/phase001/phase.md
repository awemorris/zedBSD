<!-- awesome-plan project=zedbsd record=ws333-p001 -->

# ws333-p001: /dev/thermal で統一する電力・熱の設計

Status: in-progress
Parent: [WS333](../ws.md)
Queue: q1734（P5）

## 範囲

ws.md の p001 の行のとおり。WS301・WS303 の design.md・user-questions.md・review を入力にし、CPU の idle（C-state）・P-state（HWP と driver の P-state）・P/E core の相対性能・熱の zone・fan・strategy・非常の停止を 1 つの /dev/thermal の UAPI と kernel の枠組みにまとめる。p002 の amd64 の idle（MWAIT）は先に実装するので、その形と矛盾しない設計にする。ユーザーの判断が要る点は、背景と案と用語の説明を付けて一覧にする。design-reviewer を通す。
