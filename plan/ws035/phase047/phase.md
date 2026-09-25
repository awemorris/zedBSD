<!-- awesome-plan project=zedbsd record=ws035p047 -->

# ws035-p047: networkd が `net dhcp` の直後に DHCP をやり直す

Phase ID: `ws035-p047`
Parent: [WS035](../ws.md)
Status: **cleared**（q357-i01、2026-09-24）
Phase disposition: normal
Queue: q357（q357-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー指示「networkd が DHCP のあとにすぐ DHCP を行う問題は、phase を作って修正してください」。
実機の NCM（RTL8156、q345 の追記）で、差した直後に `net dhcp ue0` を打つと、networkd がもう一度 DHCP をやり直し、
その数秒間の `connect()` がすぐ失敗した。

## 再現（QEMU、SSH ハーネスの ECM）

QMP で `usb-net` を抜いて差し直し、`filter-dump` で DHCP の packet を数えた（`plan/ws035/tests/run-networkd-replug.sh`、
`plan/ws035/tests/pcap-dhcp.py`）。

| 場面 | 修正前 | 修正後 |
| --- | --- | --- |
| 差し直した直後（`ue0` が現れた瞬間）に `net dhcp ue0` | **DHCP が 2 回**（94 ms 差、[before-fix-manual](evidence/before-fix-manual.txt)） | 1 回（3 回とも） |
| 差し直して何もしない | **DHCP 0 回**、`ue0` は UP にならず `unconfigured offline` のまま | 1 回、address が付く（3 回とも） |
| 起動後の定常状態で `net dhcp ue0` | 1 回 | 1 回 |

## 原因（2 つ）

1. **kernel が新しい interface の到着を知らせていなかった**。route socket の事象は carrier の上下と取り外しだけで、
   networkd が新しい adapter を知るのは、その interface の事象（carrier）が来たときだけだった。USB の adapter は
   UP にされるまで carrier を報告しない（p038 で networkd に RAISE を足した理由）ので、**差し直した adapter は誰にも UP にされず、
   永久に設定されなかった**。
2. **手の `net dhcp` の成功を、wired の方針が知らなかった**。手の DHCP の `dhcpc` が interface を UP にすると carrier が上がり、
   その事象で networkd は「cable があって未設定」と判断して自分でも DHCP をした。修正前に 2 回になったのはこの経路
   （1 の到着の知らせが無いので、手の DHCP が carrier を起こすまで networkd は新しい adapter を知らなかった）。

## 変更

- `include/uapi/route.h`: **`RTM_IFINFO_ARRIVAL`**（4）。`src/kern/net/net-device.c` の `net_device_create` が公開の後に送る。
  `src/kern/net/route-socket.c` が受け付ける。networkd の wired の方針は、知らない index の事象で全 interface を読み直す
  （既存の動き）ので、読み直し → RAISE → carrier → 設定、と進む。WLAN の側は知らない transition を無視する（既存）。
- `userland/base/networkd/main.c`: 手の DHCP・static が成功して使える address があれば、その interface を
  **設定済み**として方針に記録する（`lan_note_configured`）。到着したばかりで方針の表にまだ無い interface は、先に読み直す。
- `userland/base/networkd/managed-lan.c`: 同じ名前を別の adapter が使った（generation が違う）とき、state だけでなく
  `raised`・`carrier` も戻す。前の adapter について決めたことは新しい adapter には当てはまらない。

## 検証

| 試験 | 結果 |
| --- | --- |
| `make managed-lan-host-test` | PASS（新しい 4 項目: 到着で読み直し、名前を継いだ adapter の再 RAISE、手の設定の記録、その後の carrier で何もしない）。再 RAISE の項目は修正前の `managed-lan.c` で FAIL することを確かめた |
| QEMU の差し直し（上の表） | 修正後は手の DHCP の競合 3 回・自動 3 回とも DHCP 1 回、`ue0` に 10.0.2.15 |
| CI の kernel（amd64・pcat・pc98） | warning 0 |

## 残り

- 実機の NCM での再確認はしていない（adapter が host に戻っているため）。実機の DHCP server は QEMU より遅いので、
  手の DHCP と自動の設定がもっと重なりやすいが、仕組みは同じ。
