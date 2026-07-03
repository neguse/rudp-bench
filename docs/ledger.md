# unknown ledger

異常・疑問・改善候補の記帳所。**現 epoch では追わず**ここに積む。
done の定義 = このファイルが空(全行が解決済みとして削除される)。
改善系の作業は「どのセル群に効くか」を宣言できれば即着手可、
できなければここに記帳して寝かせる。

| # | 事象 | 発見 epoch | 状態 |
|---|---|---|---|
| 1 | enet UNSEQUENCED が normal 分布 jitter で delivery -2%(reorder 窓ドロップ疑い)。uniform なら v1 と一致 | v2.0 | 記帳 — boundary 解釈時に jitter 条件の注記が必要かだけ判断 |
| 2 | iperf3 の loss 実測が低 loss 率(0.1%)で +0.2pt 程度上振れ(終端 in-flight 計上疑い)。1% 以上では一致 | v2.0 | 記帳 — gate 許容内。低 loss regime を将来使うときに再確認 |
| 4 | coalesced slot の会計(attempted gate との衝突)は FireAndForget で顕在化しなくなったが契約上は未解決 | v2.0 | 記帳 — 本物の coalesce が観測されたら slots_coalesced を契約化(version 2) |
| 5 | farm 受信 rcvbuf: enet client は 4MB 明示(broadcast fanout 高 pps で kernel 既定が溢れ UDP drop delta 発火)。他 transport は同シグナル発火時に同処置 | E2 | 記帳 — 発火した transport から順次。server 側は触らない |
| 6 | farm CPU 律速の機械判定が未実装(attempted 低下は backpressure と区別できないため metric gate 委譲に変更)。sampler の client CPU を使った判別が改善候補 | E2 | 記帳 — E3 の validity gate 拡充で検討 |
