# rudp-bench

## 目的

誰かが自分のゲームに合う RUDP(既存ライブラリか自作か)を選べるようにする。

- 渡すものは、ライブラリ × 回線条件ごとの合否と、不合格の仕組みの説明(ライブラリのソースの該当行つき)である。読む人は、自分のゲームに近い条件の行を見て選ぶ。
- 測定の道具は 1 台の PC で回せる形で公開する。ただし、誰が回しても同じ値が出ることは保証しない。
- 結果が大きく割れる問い(切れるか、遅延に上限があるか、戻るか)だけを扱う。数 % の差の判定と順位付けはしない。環境による数 % の差では答えが変わらないようにするためである。

## 「使える」の条件

ゲームの通信が成り立たなくなる振る舞いをしないことを「使える」とする。回線が良いときの性能差は見ない。

| | 条件 | 意味 |
|---|---|---|
| C1 | 勝手に切らない | 経路が生きている間はライブラリの判断で切断しない |
| C2 | reliable の遅れに上限がある | ロス下でも、再送の待ち時間が倍々に伸びて秒単位で止まることがない |
| C3 | 混雑と、混雑によらないロスを取り違えない | 混雑時は送る量を減らして遅延を低く保つ。混雑によらないロスには過剰に絞らない |
| C4 | 回線が戻れば品質も戻る | 悪い時間帯が過ぎた後、元の品質へ戻る |
| C5 | 送信待ちの量がアプリから見える | アプリ側で送る量を落とす判断ができる |

## 劣悪ベンチ

少ない接続数で回線の悪さを振り、C1〜C5 の合否を見る。

### 負荷

1 server 対 8 client の状態配信(authoritative-state)。接続数は性能の限界に届かない少なさに固定し、回線だけを振る。

| 通信 | 向き | LT | MD |
|---|---|---|---|
| client の入力 | 上り | 30 Hz / 64 B | 10 Hz / 64 B |
| server の状態 | 下り | 20 Hz / 1000 B | 10 Hz / 64 B |

### 回線条件

基準の経路は片道遅延 50 ms(往復 100 ms)、MTU 1500、上下対称(RFC 8867 §4.2 の既定値)。回線は Linux の netem で再現する。

| 条件 | 内容 | 種別 |
|---|---|---|
| `base` | 基準の経路のみ | 定常 |
| `loss1` / `loss2` / `loss5` / `loss10` / `loss20` | ランダムロス 1/2/5/10/20%(上下とも) | 定常 |
| `jitter30-loss2` | ゆらぎ 30 ms(正規分布、RFC 8867 §4.2 の最大ジッタ)+ ロス 2% | 定常 |
| `burst5x10` | 平均ロス 5%、平均 10 パケット連続で落ちるバースト(Gilbert-Elliott) | 定常 |
| `congest1m` | 下りを 1 Mbit/s(RFC 8867 の基準容量)に絞り、待ち行列を約 1000 ms 分(120 パケット、RFC 8867 §5.6)にする | 混雑 |
| `outage5` / `outage15` | 計測窓の 10 秒目から 5 / 15 秒間、上下とも全断 | 回線断 |
| `loss20-then-clean` | 計測窓の 10 秒目から 20 秒間、上下ともロス 20% | 回線断 |

計測窓は、定常と混雑が 30 秒、回線断が 40 秒。warmup は 20 秒。drain は定常と混雑が 5 秒、回線断が 10 秒。
回線断の条件では、orchestrator の `netem_schedule` で計測窓の途中に netem を差し替える。

### 判定

上り・下りのそれぞれで判定し、どちらかが外れたら不合格とする。

| 条件 | 適用 | 合格基準 |
|---|---|---|
| C1 | 全条件 | server・client とも exit 0、`peer_disconnects` = 0 |
| C2 | 定常 | MD の最終到達率 = 1、MD 遅延 p99 ≤ 1000 ms |
| C3 | 定常 | LT 到達率 ≥ 0.9 ×(1 − 片道ロス率)、LT staleness p99 ≤ 500 ms |
| C3 | 混雑 | MD の最終到達率 = 1、MD 遅延 p99 ≤ 500 ms、LT staleness p99 ≤ 500 ms |
| C4 | 回線断 | MD の最終到達率 = 1、MD 遅延の最大 ≤ 断の長さ + 1 秒、LT staleness の最大 ≤ 断の長さ + 1 秒、LT 到達率 ≥ 0.9 × 回線どおりの期待値 |
| C5 | 測定しない | ライブラリの公開 API を読み、送信待ちの量か推定帯域を取れるかを判定する |

- 遅延は送信予定時刻から受信までで測る。最大値はヒストグラムの最上位の区切りの上端で、分解能は約 6%。
- orchestrator 自身の合否(`outcome`)は使わず、上の基準で判定する。`verdict` が INVALID の run は判定しない。
- 判定は `scripts/badnet/analyze.py` が行う。この節と食い違ったら、この節が正しい。

### 結果の扱い

- 各 treatment × 条件を 1 回ずつ測る。基準値の ±10% 以内の結果だけを測り直し、合否が割れたら「境目」と書く。
- 不合格は、ソースの該当行で仕組みを説明できるまでを 1 つの結果とする。説明できないものは「未解明」と書く。
- 判定基準を変えるときは、測る前にこの README を書き換えてコミットする。測定記録には、測ったときの commit を書く。

### 実行環境の注意

- ARP も netem を通るため、veth の MAC を固定して近隣エントリを恒久登録している(ARP が落ちるとパケットが秒単位で止まる)。
- 測る前の回線の検査(ping と iperf3)は切っている。iperf3 は UDP 試験の開始時の合図を再送しないため、ロス 20% では失敗する。実際のロス率は、計測窓の中の qdisc の送信・破棄の数で確かめる。

## SUT の作り

`servers/<lib>` を次の形に作り直す。

- 負荷の生成、計測、orchestrator とのやり取りは、言語ごとに共通の 1 つだけ持つ(C と C++ は `benchkit`、C# は 1 つの共有ライブラリ)。
- ライブラリごとに書くのは、接続、送信(LT と MD の振り分け)、受信、切断の検知、送信待ちの量の取得だけにする。
- 各ライブラリは、まずライブラリの既定の設定で測る。既定から変えるときは、ゲームで実際にそう使う理由を書く。性能を上げるためだけの変更はしない。
- LT と MD をどのチャネル・送信方法に載せるかは、ライブラリの文書かソースが勧める使い方に従い、出典を書く。
- 最初に共通部分、raw_udp、ENet を作る。ENet は今の実装と同じ設定でも作り、劣悪ベンチの合否が [2026-11-05 の結果](docs/measurements/2026-11-05-badnet-local.md)と一致することを確かめてから、他のライブラリに進む。

対象は今ある 7 つ(ENet、KCP、GNS、msquic、LiteNetLib、websocket、MagicOnion)。追加の候補(yojimbo、SLikeNet、quiche、lsquic、自作の coop_rudp)は未定。

## 回し方

Linux、Go、CMake、C/C++ の toolchain、.NET 10、`iproute2`、`jq` が要る。ゲームサーバなど他の処理と CPU を分け合わないよう、ビルドも測定も `orchestrator/rigs/home.json` の `bench_cpus` に限った systemd の slice の中で動かす。

```sh
go build -o build-v2/orchestrator ./orchestrator/cmd/orchestrator

for t in enet kcp gns msquic; do
  cmake -S servers/$t -B build-v2-$t -DCMAKE_BUILD_TYPE=Release && cmake --build build-v2-$t -j
done
for s in litenetlib/LiteNetLibBench websocket/WebSocketBench magiconion/MagicOnionBench; do
  for role in Server Client; do
    dotnet publish -c Release -r linux-x64 --self-contained true \
      -o build-v2-dotnet/${s#*/}.$role servers/$s.$role/${s#*/}.$role.csproj
  done
done

scripts/badnet/gen.py --out <出力先>                                      # 条件ごとの run config を作る
sudo scripts/badnet/run.sh build-v2/orchestrator <出力先>/configs/*.json
scripts/badnet/analyze.py <出力先>                                        # 判定表を出す
```

テスト:

```sh
go vet ./orchestrator/... && go test -count=1 ./orchestrator/...
```

## 用語

| 用語 | 意味 |
|---|---|
| SUT | 測る対象の実装(`servers/<lib>` の server と client) |
| treatment | 比べる単位。ライブラリ名ではなく、実装と設定の組。設定が違えば別の treatment |
| raw_udp | 再送を持たない素の UDP の実装。回線再現と計測器が正しく動くかを確かめる物差しで、候補ではない |
| LT(loss-tolerant) | 多少落ちてよく、最新の値が新しければよい通信(位置の同期など)。到達率と staleness で見る |
| MD(must-deliver) | 必ず届けたい通信(イベントの通知など)。最終到達率と遅延で見る |
| staleness | 受信側から見て、手元の最新の値がどれだけ古いか |
| run | 1 回の測定。warmup → 計測窓 → drain |
| 計測窓 | run の中で値を数える時間帯。warmup は立ち上がりを除くため、drain は送り終わりを待つための時間 |
| PASS / FAIL / INVALID | 合格 / SUT が基準を満たさなかった / 環境や計測の問題で判定できない |
| 劣悪ベンチ | 少ない接続数で回線の悪さを振り、C1〜C5 の合否を見るベンチ |
| ゆらぎ(jitter) | パケットごとの遅延のばらつき |
| 混雑 | 送る量が回線の容量を超え、経路上の待ち行列が溜まる状態。これによるロスは送る量を減らせば減る |
| bufferbloat | 混雑時に送り続けた結果、待ち行列が溜まって遅延が膨らむこと |
| 回線断 | 一定時間、上下ともパケットが全く通らない状態 |

## 以前の記録

以前の目的(横並びの比較、接続数を振る大規模な測定)の設計、ADR、測定記録、ledger は、ブランチ [`archive/before-reset`](https://github.com/neguse/rudp-bench/tree/archive/before-reset) に残してある。
