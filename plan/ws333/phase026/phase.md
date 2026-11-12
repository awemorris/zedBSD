<!-- awesome-plan project=zedbsd record=ws333-p026 -->

# ws333-p026: CPU の MHz を読み手の権限によらず新しい値で返す（UAPI の直し）

Status: planned（2026-11-13 Q1。ユーザーの承認の後、BUG-422 と p025 の 5330 の後に着手）
Parent: [WS333](../ws.md)

## 目的

`KERN_THERMAL_GET_CPU` の `delivered_mhz` を「今の周波数」にする。今は読み直し（`KERN_THERMAL_CPU_READ_SAMPLE`）を書き込みで開いた fd だけに許し、無ければいつ取ったか分からない最後の sample を印も無く返す。p014 の MHz の食い違いはこの古い値から生まれた。

## 決まった形（2026-11-13 ユーザーの承認の要点）

- 読み手の権限によらず、新しい値を返す。sample が約 1 秒より古ければ kernel が取り直す（約 100 ms）。
- 費用は権限でなく、新しい sample を全ての読み手で共有して抑える（同時の読み手は進行中の 1 回を待つ）。
- `READ_SAMPLE` の印と、書き込みだけに許す制限を外す（`include/uapi/thermal.h` の struct の `request` の扱い、`/sbin/thermal` の一覧、`docs/architecture/power-management.md`、Keiland の monitor の読み方を合わせる）。HAL の API は変えない。

## 受け入れ

- 読みだけの fd で 2 回続けて読むと、2 回目は共有の sample を返し、1 秒の後は取り直す（host の試験）。
- `thermal cpu` が読みだけの fd でも今の MHz を出す。x86_64・aarch64 の build warning 0、host の試験 PASS。5330 は次の測定にまとめる。
