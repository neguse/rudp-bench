# Provisional Loss Pipeline Smoke

このrunnerはloss条件でscenario pipelineと証跡生成が一巡することを確認するための
**暫定smoke**である。loss conformance、凍結preset、capacity測定、transport推薦、
既存採用対自作の判断には使用しない。

## 固定条件

- 対象: ENet、WebSocket
- scenario: `authoritative_state`、`room_relay`（合計4セル）
- topology: server/client netnsとveth、clientはserver側`10.200.0.1`へ接続
- 各方向egress: delay 25 ms、simple random loss 1%
- `loss_seed`、`loss_burst_len`とも指定しない。現行schemaにはcorrelation専用fieldがなく、
  burst/correlation条件は省略時の最小状態とする。burst耐性は次段のloss conformanceで扱う
- connections: 3、client process: 1
- timing: warmup 25 s、measurement 30 s、drain 5 s
- staleness sampling period: 10 ms

`authoritative_state`はclient inputをLT 13 Hz / 64 bytes、server stateをLT 20 Hz /
64 bytesとする。両方向のMDは10 Hz / 64 bytesである。`room_relay`はpublishを
LT 20 Hz / 128 bytes、MD 10 Hz / 64 bytesとする。

全LT seriesの一次SLOはdelivery ratio 0.95以上かつstaleness p99 300 ms以下、
全MD seriesはeventual delivery ratio 1.0、200 ms deadline hit ratio 0.95以上とする。

## 実行単位

[`run-provisional-loss-smoke.sh`](../scripts/run-provisional-loss-smoke.sh)は次を1 sessionに
保存する。

1. Doctor report
2. 同じnetns/netem条件のraw UDP `environment_baseline`（before）
3. 4セルのrandomized sweep
4. raw UDP `environment_baseline`（after）
5. 全run bundleのregular fileを固定する`bundle-manifest.json`
6. 入力とbundle manifestのSHA-256を含む`session-manifest.json`

Doctorの終了code 2は記録したうえで継続する。これは環境不成立を無視するためではなく、
pipeline smokeがreference evidenceへ昇格しないことを前提に失敗情報を一式残すためである。
前後baselineは両方とも`PASS / VALID`を要求するが、現段階では両者の統計的drift判定はしない。

必要なbinaryをbuildし、netns/netem操作権限を持つ環境で次の1コマンドを実行する。
runnerは一度だけ`bench.slice`へ入り直し、開始前と終了時に`rudpbench-srv` / `rudpbench-cli`
を冪等に掃除する。

```sh
sudo -E scripts/run-provisional-loss-smoke.sh
```

sessionは`results-v2/provisional-loss-smoke-sessions/<UTC timestamp>-<pid>`へ出力される。
初回の実測値を見て条件を事後変更しても、この設定をconformanceやpresetとは呼ばない。

## Measurement Record

2026-07-11にclean commit `5c52aab`で完走したsessionの条件、Doctor、前後baseline、
traffic別metrics、方向別loss evidence、manifest hashは
[`2026-07-11-provisional-loss-smoke.md`](measurements/2026-07-11-provisional-loss-smoke.md)に記録した。
4セルは初回attemptで`PASS / VALID`だったが、これは本書どおりpipeline smokeの結果であり、
class-mapping conformance、推薦、性能比較へ昇格させない。
