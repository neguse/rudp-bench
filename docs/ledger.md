# unknown ledger

異常・疑問・気になる点の記帳所。**現 epoch では追わず**ここに積む。
done の定義 = このファイルが空(全行が解決済みとして削除される)。

| # | 事象 | 発見 epoch | 状態 |
|---|---|---|---|
| 1 | enet UNSEQUENCED が normal 分布 jitter で delivery -2%(reorder 窓ドロップ疑い)。uniform なら v1 と一致 | v2.0 | 記帳 — boundary 解釈時に jitter 条件の注記が必要かだけ判断 |
| 2 | iperf3 の loss 実測が低 loss 率(0.1%)で +0.2pt 程度上振れ(終端 in-flight 計上疑い)。1% 以上では一致 | v2.0 | 記帳 — gate 許容内。低 loss regime を将来使うときに再確認 |
| 3 | loss 平面の burst 軸(b16 セル)は 5s run ではイベント数不足で p99 が不安定 | v2.0 | 記帳 — E3 で burst セルのみ duration 延長 or 反復増 |
| 4 | coalesced slot の会計(attempted gate との衝突)は FireAndForget で顕在化しなくなったが契約上は未解決 | v2.0 | 記帳 — 本物の coalesce が観測されたら slots_coalesced を契約化(version 2) |
