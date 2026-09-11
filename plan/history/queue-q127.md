# Queue q127: WS022 TLS契約とELF fixture

Date: 2026-09-09
Status: finished
Authorization: Priority全件のPhase設計・Queue化・自走をユーザー明示承認済み。
Timebox: 90 active minutesで証拠と残件を見直す。
Previous: [q126](queue-q126.md) finished / WS006-p009 uncleared。

| Order | Phase | Status | Scope |
| --- | --- | --- | --- |
| 1 | [ws022-p001](../ws022/phase001/phase.md) | completed | 現TCB・x86 HAL・rtldを調査し、static TLS layout、所有権、compiler ELFと不正ELF fixtureを確定。 |

amd64 FS-baseとi386の未実装GS descriptorを含めて契約化。既存dynamic TLSはDTV経由であり、static local-execとは異なる。p001でABIと検証経路を固定してからp002/p003を次Queueに入れる。

結果: [契約とfixture証拠](../ws022/phase001/contract.md)。p002/p003実行前のABI判断は確定。
