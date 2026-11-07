# 2026-11-07 劣悪ベンチ(直した coop_rudp)

[追加の 5 ライブラリの記録](2026-11-07-badnet-added.md#coop_rudp)で仕組みが分かった coop_rudp の不合格を、ライブラリ(`coop_rudp/rudp.c`)で直し、[劣悪ベンチ](../../README.md#劣悪ベンチ)の全条件で 1 回ずつ測った記録。
SUT(`servers/coop_rudp/coop_rudp.c`)と設定は変えていない。

## 直したこと

| 直したこと | 直す前の不合格 | 出典(`coop_rudp/rudp.c`) |
|---|---|---|
| `RUDP_UNRELIABLE_SEQUENCED` は、新しいものを送信待ちに足すとき、同じ flow・channel のまだ送っていない古いものを捨てる | 送信率が需要を下回ると LT が古いまま送信待ちに溜まり、数秒〜20 秒古い値が届いた(loss5 / loss10)。断の間に溜めた LT を断の後に約 600 KB まとめて送り、client の受信バッファが溢れた(outage5 の INVALID) | `:1514-1521` |
| 送信率をロスでは下げない。直近 8 つの RTT 標本がどれも最小 RTT より 100 ms 以上大きいときだけ半分にし(1 SRTT に 1 回まで、無制限から初めて絞るときは直近の送信の速さを起点にする)、待ち行列がなければ 1 秒に 1 MTU 分ずつ上げる | reliable のパケットを失うたびに送信率を半分にしたので、混雑によらないロスで送る量が需要を下回った(loss5 / loss10 C3、loss20 で送信を断る) | `:772-823` |
| ACK が来たら、時間切れで失ったと扱った後でも RTT を測る。送った全パケットの seq と時刻を接続ごとに直近 64 個持ち、新しく確認された最大の seq から測る | 初期 RTO(100 ms)が RTT とほぼ同じで、何か受信するたびに RTO の倍化が 0 に戻る。待ち行列ができると、reliable のパケットが毎回 ACK の前に時間切れになり、その送信記録が消えるので、RTT を一度も測れず、送信率も RTO も混雑に反応しなかった(congest1m で送信を断る) | `:1155-1161`, `:1981-1985` |
| reliable が SACK の窓(確認応答の済んでいない最古から 64 パケット)に入らなくても、unreliable は送り続ける | 窓が埋まると flush のループごと止まり、LT も出なくなった。断の後、次の RTO(最大で基準の 64 倍、約 6.4 秒)まで相手に何も届かず、戻りが約 2 秒遅れた(outage5 C4。直す前は INVALID に隠れていた) | `:1770-1773`, `:2251-2261` |

どれも `coop_rudp/tests/rudp_test.c` の、仮想の時計と回線の上で動かすテストで、直す前に落ちることを確かめてから直した(ロス 10%、ゆらぎ 30 ms + ロス 2%、125 kbit/s・待ち行列 1 秒の混雑、5 秒の回線断、RTT が初期 RTO より長い場合、sequenced の置き換え)。

## 実行条件

| 項目 | 値 |
|---|---|
| commit | `9611c58`(coop_rudp と SUT)。orchestrator は `2ee4d1f` |
| 実行 | `scripts/badnet/gen.py` → `sudo scripts/badnet/run.sh` → `scripts/badnet/analyze.py` |
| CPU | bench.slice(CPU 3-7,11-15) |
| 検証 | 判定に使った run はすべて `verdict=VALID` |

## 結果

| 条件 | 直す前 | 直した後 |
|---|---|---|
| base | PASS | PASS |
| loss1 | PASS | PASS |
| loss2 | PASS | PASS |
| loss5 | FAIL C3 | PASS |
| loss10 | FAIL C3 | PASS |
| loss20 | FAIL C2/C3 | PASS |
| jitter30-loss2 | FAIL C3 | FAIL C3 |
| burst5x10 | PASS | PASS |
| congest1m | FAIL C2/C3 | PASS |
| outage5 | INVALID | PASS |
| outage15 | PASS | PASS |
| loss20-then-clean | PASS | PASS |

congest1m に合格したのは、これまでに測った treatment の中で coop_rudp だけ。下りの負荷(約 1.3 Mbit/s)が容量(1 Mbit/s)を超えても、待ち行列の伸びで送信率を下げ、送れない LT を捨て、MD を先に送るので、回線の待ち行列が溜まらない。

### 主要な数値(上り / 下り、ms)

| 条件 | 指標 | 直す前 | 直した後 |
|---|---|---|---|
| loss10 | LT staleness p99 | 127 / 19923 | 119 / 156 |
| loss20 | 下り MD 到達率 | 0.840 | 1 |
| loss20 | MD 遅延 p99 | 459 / 688 | 426 / 410 |
| congest1m | 下り MD 遅延 p99 | 344(到達率 0.402) | 295(到達率 1) |
| congest1m | 下り LT staleness p99 | 30409(到達率 0) | 344(到達率 0.474) |
| outage5 | MD 遅延 max | 7078 / 7078(INVALID) | 5243 / 5243 |
| outage15 | MD 遅延 max | 15729 / 15729 | 15204 / 15204 |

### 境目の再測定

outage15(最大 15204 ms、基準 16000 ms)を測り直し、PASS(15204 ms)だった。

結果は `results-v2/badnet-20261107-coopfix3{,-rerun}`(リポジトリには入れていない)。

## 残った不合格の仕組み

| 不合格 | 仕組み | 出典 |
|---|---|---|
| jitter30-loss2 C3(LT 到達率 上り 0.764 / 下り 0.851) | ENet と同じ。`RUDP_UNRELIABLE_SEQUENCED` は最後に渡したものより新しくない番号を捨てるので、ゆらぎで順序が入れ替わった分が消える | `coop_rudp/rudp.c:2438-2444` |
