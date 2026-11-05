# ADR-0009: 劣悪ベンチの方法論 — 条件・判定・実行環境

- Status: **Draft**(未合意 — owner レビュー待ち。2026-11-05 owner 指示によりローカル実施を先行)
- Date: 2026-11-05
- 依存: ADR-0008(目的と「使える」の定義)、ADR-0001(authoritative-state の通信形状と LT/MD の区別)
- Decision owner: project owner

## Context

ADR-0008 は「使える」を 5 条件で定義し、数値基準をこの ADR に回した。
本 ADR は劣悪ベンチの条件・判定・実行環境を、測定の前に固定する。
実装は `scripts/badnet/`(`gen.py` が条件、`analyze.py` が判定)で、本 ADR と食い違う場合は本 ADR が正。

## Decision

### 1. 負荷

authoritative-state の 1 server 対 8 client とする。接続数は性能の限界に届かない少なさに固定し、回線だけを振る。

| traffic | 向き | LT | MD |
|---|---|---|---|
| client input | 上り | 30 Hz / 64 B | 10 Hz / 64 B |
| server state | 下り | 20 Hz / 1000 B | 10 Hz / 64 B |

レートとサイズは ADR-0004 の `state-1000` と同じ。下りの合計は約 1.4 Mbit/s。

### 2. 回線条件

基準の経路は片道遅延 50 ms(往復 100 ms)、MTU 1500、上下対称とする(RFC 8867 §4.2 の既定値)。

| 条件 | 内容 | 種別 |
|---|---|---|
| `base` | 基準の経路のみ | 定常 |
| `loss1` / `loss2` / `loss5` / `loss10` / `loss20` | ランダムロス 1/2/5/10/20%(上下とも) | 定常 |
| `jitter30-loss2` | ゆらぎ 30 ms(正規分布、RFC 8867 §4.2 の最大ジッタ)+ ロス 2% | 定常 |
| `burst5x10` | 平均ロス 5%、平均 10 パケット連続で落ちるバースト(Gilbert-Elliott) | 定常 |
| `congest1m` | 下りを 1 Mbit/s(RFC 8867 の基準容量)に絞り、待ち行列を約 1000 ms 分(120 パケット、RFC 8867 §5.6)にする | 混雑 |
| `outage5` / `outage15` | 計測窓の 10 秒目から 5 / 15 秒間、上下とも全断 | 回線断 |
| `loss20-then-clean` | 計測窓の 10 秒目から 20 秒間、上下ともロス 20% | 回線断 |

計測窓は、定常と混雑が 30 秒、回線断が 40 秒とする。warmup は全条件で 20 秒固定。drain は定常と混雑が 5 秒、回線断が 10 秒。
回線断の条件では、orchestrator の `netem_schedule` で計測窓の途中に netem を差し替える。

### 3. 判定

上り・下りのそれぞれで判定し、どちらかが外れたら不合格とする。

| 条件 | 適用 | 合格基準 |
|---|---|---|
| C1 勝手に切らない | 全条件 | server・client とも exit 0、`peer_disconnects` = 0 |
| C2 reliable の遅れに上限 | 定常 | MD の最終到達率 = 1、MD 遅延 p99 ≤ 1000 ms |
| C3 混雑とロスを取り違えない | 定常 | LT 到達率 ≥ 0.9 ×(1 − 片道ロス率)、LT staleness p99 ≤ 500 ms |
| 同上 | 混雑 | MD の最終到達率 = 1、MD 遅延 p99 ≤ 500 ms、LT staleness p99 ≤ 500 ms |
| C4 回線が戻れば品質も戻る | 回線断 | MD の最終到達率 = 1、MD 遅延の最大 ≤ 断の長さ + 1 秒、LT staleness の最大 ≤ 断の長さ + 1 秒、LT 到達率 ≥ 0.9 × 回線どおりの期待値 |
| C5 送信待ちの量が見える | 測定しない | ライブラリの公開 API を読み、送信待ちの量か推定帯域を取れるかを判定する |

遅延は送信予定時刻(sched)から受信までで測る。最大値はヒストグラムの最上位の bin の上端で、分解能は約 6%。
orchestrator 自身の SLO 判定(`outcome`)は参考とし、上の基準で置き換える。`verdict` が INVALID の run は判定しない。

### 4. 結果の扱い

- 各 treatment × 条件を 1 回ずつ測る。基準の境目にある結果(基準値の ±10% 以内)だけを再測し、合否が割れたらその旨を書く。
- 不合格は、ソースの該当行で仕組みを説明できるまでを 1 つの結果とする。説明できないものは「未解明」と明記する。
- treatment は `servers/<lib>` の現在の実装と設定のまま測る。設定を変えた変種は別の treatment として扱う。

### 5. 実行環境

smoke 用の現 PC(smolcenter)で実行する。ゲームサーバと CPU を分け合わないよう、rig 設定(`orchestrator/rigs/home.json`)の bench_cpus に限定した bench.slice の中で動かす。
doctor の合否は問わない。本 ADR の問いは結果が大きく割れるものに限っており、数 % の環境差では答えが変わらないためである。

run 前の netem gate(ping と iperf3)は切る。iperf3 は UDP 試験の開始時に UDP の合図を 1 往復だけ交わして再送しないため、ロス 20% では 12 回中 3 回失敗した。
ロスは計測窓の中の qdisc の送信・破棄カウンタ(loss evidence)で、遅延は `base` 条件の遅延で確かめる。
ARP も netem を通るため、veth の MAC を固定して近隣エントリを恒久登録する(ARP が落ちると ping や SUT のパケットが秒単位で止まる。ロス 10% で ping の平均が 376 ms になった)。

## Non-goals

- 接続数を振った限界の測定(ADR-0008 の大規模ベンチ)
- 実回線の統計に合わせた条件の再現

## Consequences

- 結果は `docs/measurements/` に日付つきで記録する。
- 不合格の原因説明は `servers/<lib>/README.md` の設計ノートか ledger に書く。
