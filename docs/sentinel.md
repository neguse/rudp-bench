# 回帰検知(sentinel)

published 基準からの漂移を 26 プローブ・約 13 分で機械判定する定点再測チェック。

```sh
sudo scripts/run-sentinel.sh            # 既定 config(orchestrator/examples/sentinel-home.json)
sudo scripts/run-sentinel.sh my.json    # config 指定
```

ラッパーが CPU 隔離下の bench.slice 起動と netns 残留の掃除を面倒みる。
終了コード: 0 = 全 PASS / 3 = DRIFT(2 回連続で基準乖離)/ 4 = INVALID
(測定不成立 — 環境かインフラの故障。結果は漂移として読まないこと)。
単発のゆらぎは FLAP として報知されない。

## 運用ルール

- 測定器(orchestrator / benchkit / BenchKit.CS / servers / adapters)に触れる
  変更は、merge 前に sentinel を 1 回通すこと(2026-07-02 に adapter 改修で
  配送 0% の回帰が unit test 全 green のまま入った再発防止)
- 日次実行の例(root の crontab):
  `0 5 * * * /home/neguse/ghq/github.com/neguse/rudp-bench/scripts/run-sentinel.sh >> /var/log/rudp-sentinel.log 2>&1`
- 基準は published aggregate から生成される。プロトコル変更(benchspec version
  更新)をしたら基準の取り直しが必要(古い基準との比較は偽 DRIFT になる)

## 既知の限界(ledger 記帳分)

- 基準が単一ブロック参照のため IQR 際のセルで偽 drift が出うる
  ([ledger](ledger.md) #15、対処は基準の N=3-median 化)
- budget 際セルは 2 連続 flap 政策を稀にすり抜ける(ledger #18、偽報知率 <1%)
