# benchspec — rudp-bench v2 ワイヤ契約

言語非依存の契約仕様。server / client / orchestrator の実装はすべてこの文書に従う。
設計の背景と根拠は [v2 design spec](../docs/superpowers/specs/2026-07-02-rudp-bench-v2-design.md) 参照。

- version: 2(定常判定つき warmup を追加。version 1 との差分は
  「lifecycle と control channel」の rate / window / window_ack のみで、
  payload・計測の定義は不変。移行方針どおり全実装・orchestrator を同時移行。
  version 1 は校正5本 green・v1 突き合わせ・縮小 broadcast 検証を満たして
  凍結された契約で、その校正結果は payload/計測定義の不変性により引き継ぐ)

## 用語

| 用語 | 意味 |
|---|---|
| transport | 被測定対象(enet, msquic, magiconion, …)。server + client の idiomatic 実装対 |
| slot | client の送信計画上の1送信機会。`(conn, class)` ごとに 1 から連番 |
| traffic class | `loss-tolerant`(latest-value、落としてよい)/ `must-deliver`(必達) |
| distribution | `echo`(送信元にのみ返す)/ `broadcast`(全接続へ fanout) |
| global conn id | orchestrator が client proc ごとに割り当てる連番レンジから採番される接続 ID |

## payload

全メッセージは以下のヘッダで始まる。byte order は little-endian。最小 payload = 32B。

```
offset size field
0      8    seq          slot id(u64。(conn, class) ごとに 1 起点の連番。
                         未送信 slot も番号を消費する)
8      8    sched_ts_ns  この slot を本来送るべきだった予定時刻
                         (CLOCK_MONOTONIC ns。送信計画から決まる)
16     8    send_ts_ns   実送信直前の時刻(CLOCK_MONOTONIC ns)
24     1    flags        bit0: traffic class(1=must-deliver / 0=loss-tolerant)
                         bit1: measurement-window(start_at〜stop_at 内の送信なら 1)
                         bit2: distribution(0=echo / 1=broadcast)
                         bit3-7: reserved、0 固定
25     4    origin_id    送信元の global conn id(u32。常に送信者が記入)
29     3    reserved     0 固定
32-    pad  指定 payload サイズまで任意バイトで充填(受信側は解釈しない)
```

## server の意味論

server は設定を持たない単一プログラムで、受信 payload の flags だけで振る舞いを決める:

- `echo`: 受信した payload を**無変更で**送信元 conn へ、同一 traffic class で返す
- `broadcast`: 受信した payload を**無変更で**現在の全接続(origin 含む)へ、
  同一 class で fanout する。1 メッセージの期待受信数 = その時点の接続数

payload の書き換えは禁止(seq / ts / flags は origin のものが受信端まで透過する)。
server は class・distribution 別の受信数と送出 submit 数を集計し、DONE で報告する
(forward / return 分解用)。

## traffic class のマッピング

class → transport 機構への割り当ては実装が決め、`--describe` で開示する(下記)。

- RUDP/QUIC 系の想定: loss-tolerant → unreliable/datagram、must-deliver → reliable/stream
- TCP 系・reliable-only 実装: 両 class とも reliable stream

**coalescing 規則**: loss-tolerant class に限り、まだ transport へ submit していない
古い update を新しい値で置換・破棄してよい(送信側 app 層 coalescing)。実施の有無と
方式は `--describe` で開示する。coalesce / backpressure / 停滞で送信されなかった slot は
client が未送信 slot として記録する(seq は消費済みなので受信側統計と突き合わせ可能)。
must-deliver class の欠落・並べ替え・重複は不可。

## lifecycle と control channel

orchestrator からプロセスへの受け渡しは環境変数で行う:

| 変数 | 意味 |
|---|---|
| `BENCH_CONTROL_SOCK` | control channel の UDS path |
| `BENCH_METRICS_OUT` | このプロセスが metrics JSON を書き出すファイル path。プロセスは drain 完了後・`done` 送信前に書く |

control channel は **Unix domain socket**。netns をファイルシステム経由で越えるため、被測定経路の
netem の影響を受けない out-of-band 経路になる。プロトコルは line-delimited JSON:

```
process → orchestrator: {"type":"hello","role":"server|client","transport":"enet",
                          "pid":123,"proc_index":0}
process → orchestrator: {"type":"ready","conns":50}
                          (client: 全接続確立済み / server: listen 開始済み)
orchestrator → process: {"type":"schedule","start_at_ns":…,"stop_at_ns":…,
                          "drain_until_ns":…}
process → orchestrator: {"type":"sched_ack","margin_ns":…}
                          (margin = start_at − 受信時刻。負なら run は INVALID)
client → orchestrator:  {"type":"rate","sent":…,"received":…}     (v2、周期 250ms)
orchestrator → process: {"type":"window","start_at_ns":…,"stop_at_ns":…,
                          "drain_until_ns":…}                      (v2、0 or 1 回)
process → orchestrator: {"type":"window_ack","margin_ns":…}        (v2)
process → orchestrator: {"type":"done","stats":{…}}
```

- 時刻はすべて CLOCK_MONOTONIC の絶対値。同一ホスト内なので全プロセスで比較可能
- ready〜start_at の間が warmup: client は送信してよいが measurement bit は 0
- start_at〜stop_at が計測窓: この間の送信は measurement bit = 1
- stop_at で client は送信を止め、drain_until まで受信を続ける
- drain_until 後に stats を報告(`done`)して exit
- 計測に入れるのは measurement bit = 1 の message のみ(受信時刻が stop_at 以降でも、
  drain_until までに届けば staleness / hit rate の集計対象)

### 定常判定つき warmup(version 2)

接続ストーム直後の非定常区間が計測窓に入ると同一条件でも結果が二峰化する
(輻輳制御・throttle の整定が warmup を越えて残る)。これを排除するため、
計測窓は「時間経過」ではなく「送受レートの定常」で開く:

- schedule は**暫定窓**として届く(start_at = ready + warmup **上限**)。client は
  従来どおり受信直後から送信を開始する
- client は schedule 受信後、250ms 周期で累積の生カウント(送信 submit 数、
  受信数 — measurement bit の有無を問わない)を `rate` で報告する
- orchestrator は全 client について「直近 N 個(既定 4)の報告間隔の増分が
  すべて中央値 ±10% 以内、かつ増分が正」を満たしたら定常と判定し、確定窓を
  `window` で全参加者(server 含む)に配布する
- `window` の受信者は即座に `window_ack`(margin = start_at − 受信時刻。負なら
  run は INVALID)を返し、ローカルの窓(schedule と送信計画の measurement bit
  判定)を確定窓に差し替える
- 暫定 start_at までに定常が検出されなければ `window` は送られず、暫定窓が
  そのまま有効(run の結果に steady 未達として開示される。エラーではない)
- 窓確定後(window 受信 or 暫定 start_at 到達)、client は rate 報告を止める
- orchestrator は「定常が見えても宣言された最小 warmup より前に窓を開かない」
  制約を持てる(ワイヤ契約への影響なし)。レート形状から予測できない遅い
  過渡を持つ transport 向け — enet の packet throttle は接続ストーム後
  ~13s、レートが数秒 flat に見えた後で崩れることが 20 反復 × 2 系列で
  実測されている(定常は必要条件であって十分条件ではない)

## 計測の定義

- **staleness**(loss-tolerant の一次指標): 受信側が (origin, class) ごとに
  「最新受信 update の age = now − sched_ts」を 10ms 周期でサンプルし、その分布の
  percentile を取る。drop・HoL 遅延・coalescing がすべて同じ物差しに載る
- **deadline hit rate**(must-deliver の一次指標): profile が定義する締切 D に対し、
  受信時刻 − sched_ts ≤ D だった slot の割合。未送信 slot も分母に入る
- **診断用**: delivery_ratio(最終到達率)、send_ts 起点 latency(transport 単体の遅延)、
  forward / return 分解、per-class ヒストグラム
- 重複判定は **(受信側 local conn, origin_id, class, seq)** で行い、初観測のみ集計する。
  受信側 conn をキーに含めるのは、broadcast では同一メッセージの正当な複製が
  同一 proc 内の複数 conn に届くため(含めないと複製が duplicate 扱いになり
  delivery が壊れる)

## 開示 metadata(--describe)

server / client バイナリは `--describe` で以下を JSON 出力する:

```json
{
  "transport": "enet",
  "class_mapping": {"loss_tolerant": "unreliable-unsequenced", "must_deliver": "reliable"},
  "coalescing": "none | latest-value(方式の要約)",
  "cc_algo": "none | cubic | bbr | …",
  "thread_model": "single | multi | internal_worker",
  "encryption": true,
  "max_payload_bytes": 1370,
  "tuning": [{"knob": "…", "value": "…", "upstream_ref": "…"}]
}
```

`tuning` の各項目は upstream 公式ドキュメント・推奨への参照を必須とする。

## conformance(参加条件)

reference client が server 実装を1回の起動で検査する:

1. echo: 返送 payload が無変更で、送信元にのみ届く
2. broadcast: 全接続に届き、payload 無変更、期待受信数が接続数と一致
3. class マッピングが `--describe` の申告どおり(loss 注入時の挙動で判別)
4. must-deliver: loss 下で欠落・重複・payload 破損なし
5. control channel: hello → ready → sched_ack → done の順序と時刻整合
