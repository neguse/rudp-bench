# benchspec — rudp-bench v3 ワイヤ契約

言語非依存の契約仕様。server / client / orchestrator の実装はすべてこの文書に従う。
設計の背景と根拠は [v2 design spec](../docs/superpowers/specs/2026-07-02-rudp-bench-v2-design.md) 参照。

- version: 4(`--describe.class_mapping` を機械検証可能な構造へ変更。wire payload、
  control protocol、metrics schema はv3から不変。v3で追加したserver-origin
  authoritative 1対Nと`(traffic_id, direction, class)`別の計測も維持する)

## 用語

| 用語 | 意味 |
|---|---|
| transport | 被測定対象(enet, msquic, magiconion, …)。server + client の idiomatic 実装対 |
| slot | logical originator の1送信機会。authoritative state は target ごとに1 slot |
| traffic class | `loss-tolerant`(latest-value、落としてよい)/ `must-deliver`(必達) |
| distribution | `echo`(送信元にのみ返す)/ `broadcast`(全接続へ fanout) |
| direction | `room_relay` / `client_to_server`(authoritative input) / `server_to_client`(authoritative state) |
| traffic_id | direction 内の logical traffic。0=room relay、1=authoritative input、2=authoritative state |
| global conn id | orchestrator が client proc ごとに割り当てる連番レンジから採番される接続 ID |

## payload

全メッセージは以下のヘッダで始まる。byte order は little-endian。最小 payload = 32B。

```
offset size field
0      8    seq          slot id(u64。logical stream / target ごとに 1 起点。
                         未送信 slot も番号を消費する)
8      8    sched_ts_ns  この slot を本来送るべきだった予定時刻
                         (CLOCK_MONOTONIC ns。送信計画から決まる)
16     8    send_ts_ns   実送信直前の時刻(CLOCK_MONOTONIC ns)
24     1    flags        bit0: traffic class(1=must-deliver / 0=loss-tolerant)
                         bit1: measurement-window(start_at〜stop_at 内の送信なら 1)
                         bit2: distribution(0=echo / 1=broadcast)
                         bit3-4: direction(0=room_relay / 1=client_to_server /
                                 2=server_to_client、3=invalid)
                         bit5-7: reserved、0 固定
25     4    origin_id    client 送信は global conn id。authoritative server は
                         total_conns(予約値、client id は 0..total_conns-1)
29     1    traffic_id   0=room relay / 1=authoritative input /
                         2=authoritative state、3..255 は profile が定義
30     2    reserved     0 固定
32-    body 指定 payload サイズまで、下記の決定的patternで充填
```

authoritative state以外のbody byte `i`（offset 32を`i=0`）は、headerの
little-endian byteを使って次で生成し、全受信端で検証する。

```
LE64(seq)[i % 8]
XOR LE64(sched_ts_ns)[(i + 3) % 8]
XOR LE64(send_ts_ns)[(i + 5) % 8]
XOR LE32(origin_id)[i % 4]
XOR flags XOR traffic_id XOR uint8(i)
```

不一致は配送成功として会計せず、`done.stats.invalid_payload`を増やす。

### authoritative state body

`server_to_client / traffic_id=2` の payload は 40B 以上とし、offset
32..39 にその target client について state へ反映済みの
loss-tolerant authoritative inputの最新反映済み`seq`
(`u64` little-endian、0=未反映)を入れる。must-deliver inputはこのfieldを
前進させないため、classごとに独立なseq空間を混同しない。
payload が 40B より大きければ、target client の global conn id を
`target_id` とし、offset 40 以降の pad index `i`(0 起点)を
`LE32(target_id)[i % 4] XOR uint8(i / 4)` で埋める。server は
target ごとに body をマテリアライズし、全 target で共有する
buffer を送る最適化は行わない。同じ tick の header `seq / sched_ts`
は target 間で同値でよい。

## scenario と server の意味論

### room relay(v2 互換)

server は設定を持たない単一プログラムで、受信 payload の flags だけで振る舞いを決める:

- `echo`: 受信した payload を**無変更で**送信元 conn へ、同一 traffic class で返す
- `broadcast`: 受信した payload を**無変更で**現在の全接続(origin 含む)へ、
  同一 class で fanout する。1 メッセージの期待受信数 = その時点の接続数

payload の書き換えは禁止(seq / ts / flags は origin のものが受信端まで透過する)。
server は class・distribution 別の受信数と送出 submit 数を集計し、DONE で報告する
(forward / return 分解用)。

### authoritative 1対N

- **input**: client が `client_to_server / traffic_id=1 / broadcast=0` で
  per-client input slot を送る。server は payload を受信会計し、送信元へ
  return/echo しない。
- **state**: server は schedule 受信後に1つの global tick plan を駆動する。
  schedule 時点の `total_conns` を frozen roster とし、各 tick を roster
  の各 target に展開する。各 target が別 logical slot であり、
  `server_to_client / traffic_id=2 / broadcast=0 / origin_id=total_conns`
  とする。1 tick の予期受信数は N だが、slot 自体が N 件なので
  broadcast 係数を再度掛けない。
- state の `send_ts_ns` は target ごとの transport submit 直前。
  `sched_ts_ns` は global tick による予定時刻で target 間で共通。
- roster からの切断、送信失敗、payload 生成失敗でも slot は消費し、
  `submitted=false` として分母に残す。

## traffic class のマッピング

class → transport 機構への割り当ては実装が決め、`--describe` で開示する(下記)。

- RUDP/QUIC 系の想定: loss-tolerant → unreliable/datagram、must-deliver → reliable/stream
- TCP 系・reliable-only 実装: 両 class とも reliable stream

各classのmappingは次の4 fieldを持つ。`delivery`と`ordering`はtransportへのsubmitが
成功したmessageについて、同一connection/channel/lane内で実際に提供される意味論を表す。
別connectionや別traffic seriesをまたぐ順序は保証しない。

| field | 値と意味 |
|---|---|
| `primitive` | 実際に使うtransport機構の安定した識別子。空文字は禁止 |
| `delivery` | `best_effort`（欠落し得る）/ `reliable`（接続維持中は再送等で必達） |
| `ordering` | `unordered` / `ordered` |
| `realization` | `native`（transport機構を直接利用）/ `emulated`（application層で実現）/ `reliable_fallback`（loss-tolerantをreliable機構へ載せる）/ `unsupported`（要求classの意味論を提供できない） |

server/clientは同じ2 classを過不足なく開示し、正規化JSONが完全一致しなければならない。
orchestratorは両endpointのmappingとSHA-256をresultへ保存する。旧string形式、未知field、
未知enum、server/client不一致、保存済みhashとの不一致は現行契約では`INVALID`とする。
`--describe`は申告証拠にすぎず、conformanceでは方向別loss注入時の配送挙動と照合する。

`realization=unsupported`は能力の開示であり、通常runを自動的に`UNSUPPORTED`へ変えない。
raw UDPのようなenvironment floorは、要求classを提供しないこと自体を観測するためにも実行する。
ただし該当classはclass-mapping conformanceでは`UNSUPPORTED`であり、用途solutionの適合候補や
推薦根拠にはできない。scenario自体の非広告やpayload上限不足による`UNSUPPORTED`とは区別する。

**coalescing 規則**: loss-tolerant class に限り、まだ transport へ submit していない
古い update を新しい値で置換・破棄してよい(送信側 app 層 coalescing)。実施の有無と
方式は `--describe` で開示する。coalesce / backpressure / 停滞で送信されなかった slot は
その originator(client または authoritative server)が未送信 slot として
記録する(seq は消費済みなので受信側統計と突き合わせ可能)。
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

scenario runの全processは`done.stats.invalid_payload`を必須の非負integerとして
返す。非zeroはSUTの`FAIL`、field/JSON欠落は測定契約の`INVALID`とする。
`authoritative_state`ではさらに次のobjectを返す。

```
"authoritative_progress": {
  "role": "server|client",
  "local_conns": 0,
  "roster_conns": 3,
  "input_last_sent_min": 0,
  "input_last_sent_max": 0,
  "state_header_seq_recv_min": 0,
  "state_header_seq_recv_max": 0,
  "state_applied_input_seq_recv_min": 0,
  "state_applied_input_seq_recv_max": 0,
  "server_state_ticks": 0
}
```

clientのmin/maxは各local connectionで計測窓中に観測した最終最大値を、connection間で
再集約した値である。serverの`server_state_ticks`はfanout後slot数ではなく、計測窓中の
loss-tolerant global tick数である。orchestratorは全connectionのprogress、roster、
lossless時の最終catch-up、scenario rateから期待する独立tick数をgateする。

- 時刻はすべて CLOCK_MONOTONIC の絶対値。同一ホスト内なので全プロセスで比較可能
- ready〜start_at の間が warmup: client と authoritative server は
  送信してよいが measurement bit は 0
- start_at〜stop_at が計測窓: この間の送信は measurement bit = 1
- stop_at で全 originator は送信を止め、drain_until まで受信を続ける
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

- 全 count/histogram は **`(traffic_id, direction, class)` 別**に持つ。
  class だけの legacy aggregate も出力するが、authoritative の SLO 判定に
  使ってはならない(input と state が混ざるため)。
- **staleness**(loss-tolerant の一次指標): 受信側が
  `(local conn, origin, traffic_id, direction, class)` ごとに
  「最新受信 update の age = now − sched_ts」を 10ms 周期でサンプルし、その分布の
  percentile を取る。drop・HoL 遅延・coalescing がすべて同じ物差しに載る
- 受信専用 authoritative client は計測窓前に全
  `(local conn, server origin, traffic_id=2)` flow を登録する。初回から
  一度も届かない flow の `first_sched_ts` は schedule 受信時の provisional
  `start_at` とし、starvation sentinel として staleness に入れる。window
  確定時の rebase は行わない。初回 update の観測後は wire header の実
  `sched_ts_ns` へ置き換える。server tick phase / 確定 window と sentinel の差は
  完全未受信 flow にだけ残り、その flow は
  `never_received_flows > 0` の独立 gate で FAIL とするため、通常の
  latency/staleness 合否へ混ぜない。
- **deadline hit rate**(must-deliver の一次指標): profile が定義する締切 D に対し、
  受信時刻 − sched_ts ≤ D だった slot の割合。D も traffic_id /
  direction 別で、未送信 slot も分母に入る
- **診断用**: delivery_ratio(最終到達率)、send_ts 起点 latency(transport 単体の遅延)、
  forward / return 分解、per-class ヒストグラム
- 重複判定は **(受信側 local conn, origin_id, traffic_id,
  direction, class, seq)** で行い、初観測のみ集計する。
  受信側 conn をキーに含めるのは、broadcast では同一メッセージの正当な複製が
  同一 proc 内の複数 conn に届くため(含めないと複製が duplicate 扱いになり
  delivery が壊れる)
- traffic series ごとに `expected_flows / observed_flows /
  never_received_flows` を出す。平均 staleness/delivery が一部 client の
  完全 starvation を隠していないことを gate で確認し、
  `never_received_flows > 0` は percentile と独立に FAIL とする。
- loss-tolerant trafficのstaleness histogram countは、`duration /
  staleness_period × expected_flows`に対してflowあたり前後2 tickの範囲で照合する。
  flow登録だけして一部sampleしか取らない実装は低いp99でも`INVALID`とする。
- sender の slot/submitted と receiver の delivered/histogram は異なる
  process にあってよい。orchestrator は authoritative で server metrics と
  全 client metrics を同じ traffic key で加算してから比率を計算する。

### metrics JSON(version 2)

必須 top-level field は `version / histogram / classes / traffic /
staleness_ns / raw`。`classes` と top-level `staleness_ns` は legacy
aggregate であり、v3 判定は `traffic` 配列を使う。数値 count と ns は
非負の JSON integer とする。

各`classes.<class>`の9 count fieldと3 histogramは、同じclassの全`traffic` seriesの
厳密な合計と一致させる。top-level `staleness_ns`も全loss-tolerant trafficの厳密な
合計とする。traffic固有deadlineを持つmust-deliverの`deadline_hit`は、そのresolved
deadlineでtrafficとclassを同時に加算する。不一致は`metrics contract INVALID`とする。

- `histogram`: `scheme="log2x16" / subbins=16 / min_ns=1000 /
  max_ns=100000000000`
- `classes`: `loss_tolerant / must_deliver` の2 object。各 object は
  count field と `latency_sched_ns / latency_send_ns / update_gap_ns`
- `traffic`: key `(traffic_id, direction, class)` ごとに最大1 object。
  count field、resolved `deadline_ns`、上記3 histogram と
  `staleness_ns` を持つ
- count field: `slots / slots_broadcast / submitted / delivered_unique /
  duplicates / deadline_hit / expected_flows / observed_flows /
  never_received_flows`
- histogram object: `scheme / min_ns / max_ns / count / p50_ns / p90_ns /
  p99_ns / bins`。`bins` は 448 個で、top-level `histogram` の layout に従う
- `raw`: `slots / submitted / recv_measured / recv_unmeasured`

同じ run の全 process は同じ histogram layout を使う。同じ traffic key
を出す process 間で resolved `deadline_ns` も一致させる。orchestrator は
count と bins を加算後に percentile / ratio を再計算し、process が出した
percentile を平均しない。

`direction` は `room_relay / client_to_server / server_to_client` のいずれか。

## 開示 metadata(--describe)

server / client バイナリは `--describe` で以下を JSON 出力する:

```json
{
  "transport": "enet",
  "class_mapping": {
    "loss_tolerant": {
      "primitive": "unreliable-unsequenced",
      "delivery": "best_effort",
      "ordering": "unordered",
      "realization": "native"
    },
    "must_deliver": {
      "primitive": "reliable",
      "delivery": "reliable",
      "ordering": "ordered",
      "realization": "native"
    }
  },
  "coalescing": "none | latest-value(方式の要約)",
  "cc_algo": "none | cubic | bbr | …",
  "thread_model": "single | multi | internal_worker",
  "encryption": true,
  "max_payload_bytes": 1370,
  "scenarios": ["environment_baseline", "authoritative_state", "room_relay"],
  "tuning": [{"knob": "…", "value": "…", "upstream_ref": "…"}]
}
```

`tuning` の各項目は upstream 公式ドキュメント・推奨への参照を必須とする。

## conformance(参加条件)

reference peer が server/client 実装を検査する:

1. echo: 返送 payload が無変更で、送信元にのみ届く
2. broadcast: 全接続に届き、payload 無変更、期待受信数が接続数と一致
3. authoritative input: `client_to_server / traffic_id=1` が server で
   1回だけ受信会計され、client へ一切 return されない
4. authoritative state: 1 global tick から target 数と同数の
   non-broadcast slot が生まれ、`origin_id=total_conns`、target 固有
   body、`applied_input_seq` がその client の受信済み input と整合
5. traffic separation: input/state が同じ class/seq でも重複扱いされず、
   別 traffic series に会計される
6. starvation: 一度も state が届かない target が
   `never_received_flows` と staleness に現れる
7. class マッピングが `--describe` の申告どおり(loss 注入時の挙動で判別)
8. must-deliver: loss 下で欠落・重複・payload 破損なし
9. control channel: hello → ready → sched_ack → done の順序と時刻整合

loss下の判定には、事前probeだけでなくeffective measurement window内でloss指定方向ごとの
dropが実際に発生した機械証拠を必要とする。random netemはqdisc counterで検査できる。
deterministic losstraceはwindow内drop counterが未統合の間、conformance証拠として扱わない。
