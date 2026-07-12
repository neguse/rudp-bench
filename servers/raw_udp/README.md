# raw_udp — 素 UDP の environment floor(reference 実装)

benchspec/README.md 準拠の実装対。候補 solution ではなく、**非信頼の下限
(environment floor)** と測定環境の診断に使う基準 transport。tuning は無い
(`--describe` の `tuning` は空)。

## class mapping

| class | wire | realization |
|---|---|---|
| loss-tolerant | 素 UDP datagram | `native` |
| must-deliver | 素 UDP datagram(同じ send()) | **`unsupported`** |

must-deliver も loss-tolerant と同じ非信頼 datagram で送る。ロス下では MD も
落ちるが、それが raw_udp の正しい挙動 = 非信頼フロアであり、`--describe` は
MD を `unsupported` と開示する(`server.c` の `print_describe`)。class-mapping
conformance では環境診断として実行し、MD case は `UNSUPPORTED` のまま保存する
(候補 solution の promotion 判定とは分離 — ルート README 参照)。

## 構成

- **server**: 単一 socket・単一スレッド。client の登録 packet(flags=0 の
  非計測 payload)で送信元アドレスを学習し、origin→addr の peer table を持つ。
  scenario に応じて echo(room_relay / baseline)または authoritative state
  fanout を行う
- **client**: conn ごとに socket を作って `connect(2)`、READY 前に登録
  packet を 1 発送る(echo で戻るが MEASURE flag が無いため metrics は汚さない)
- **drain budget 512/呼び出し**(`server.c` の `RAWUDP_SERVER_DRAIN_BUDGET`):
  EAGAIN まで無制限に drain すると fanout の持続負荷で socket が空にならず
  benchkit 制御チャネルが飢える(`docs/ledger.md` #20 の機構)。
  `docs/battle.md`「運用の学び」の server 版 drain budget

## ramp モード

orchestrator の ramp(単一 run 内の接続数段階増加。契約は
`benchspec/README.md`「ramp mode」)に対応済み(`../ramp.h` を使用)。
`BENCH_RAMP_*` が揃うと phase ごとに接続を追加して per-phase snapshot
(`$BENCH_METRICS_OUT.ramp-*.json`)を書き、最終の cumulative metrics は
書かない。

## build / test

```sh
cmake -S servers/raw_udp -B build-v2-rawudp
cmake --build build-v2-rawudp -j
ctest --test-dir build-v2-rawudp --output-on-failure
go run ./orchestrator/cmd/orchestrator run -config orchestrator/examples/local-baseline-rawudp.json
```
