# ADR-0001: Reference scenario のスコープ

- Status: **Accepted**
- Date: 2026-07-10
- 依存: ADR-0000
- Scope: scenario の種類だけを固定する。rate、payload、SLO、network 条件などの
  数値は後続の決定で定める

## Context

ADR-0000 の目的には、利用者が自分の workload に近い条件を再現できることと、
典型的な条件の比較データから技術選択できることの両方が含まれる。一方、用途名を
増やしすぎると、互いに似た synthetic workload が並び、どの違いが結果を変えたかを
説明しにくくなる。

最小の scenario 集合で、測定環境の限界、接続数にほぼ線形な負荷、room 内 fanout
による非線形な負荷を分離する。

## Decision

reference scenario は次の 3 種類だけから始める。

### 1. `environment-baseline` - 測定環境と計測器の基準測定

用途選定のための workload ではなく、本測定が成立する環境かを確認する校正である。

- OS、CPU、clock、network namespace、network emulation の設定と実効値を確認する
- client farm、server runner、計測・集計経路が生成可能な負荷の上限を確認する
- transport 固有機能を極力含まない単純な probe と raw transport を使い、
  base RTT、loss、jitter、packet rate、throughput、CPU、欠測を測る
- 後続 scenario の結果が SUT ではなく測定環境で打ち切られた場合に、その限界を
  特定して `CENSORED` と判定する根拠にする

この結果から solution の優劣や production capacity は推薦しない。

### 2. `authoritative-state` - 1 server 対 N client

authoritative server を持つリアルタイム application の基本形である。

- client は input / command / telemetry を server へ送る
- server は各 client に、その client が必要とする state / event を送る
- 1 client あたりの送受信量を固定した場合、集約負荷は概ね接続数に比例する
- 接続数、small-message 処理、双方向通信、reliability class の混在、server resource
  当たりの capacity と overhead を評価する

全 client の入力を無条件に全 client へ複製する workload は、この scenario には
含めない。interest 範囲の大きさは設定可能にするが、既定値は後続で定める。

### 3. `room-relay` - room 内 N publisher 対 N subscriber

room 内の参加者が update を publish し、他の参加者が購読する relay の基本形である。

- 各参加者は latest-value state などを server へ送る
- server は同じ room の subscriber へ update を転送する
- publisher、subscriber、room size、fanout policy により server の送信量が増える
- 対称 all-to-all はこの scenario の stress 条件の一つであり、scenario 自体の唯一の
  topology ではない
- fanout、coalescing、backpressure、staleness、slow receiver の影響を評価する

音声、pose、位置同期などの用途名は、この通信形状に対応付ける説明例として扱う。
用途名だけから rate、payload、同時 publisher 数を固定しない。

## Shared Model

2つの use-case scenario は同じ語彙で設定できるようにする。

- topology: rooms、clients、publishers、subscribers、fanout / interest policy
- traffic: direction、rate、payload size または分布、burst
- semantics: reliable / loss-tolerant、ordered、replaceable、deadline / staleness SLO
- lifecycle: steady-state、connect / disconnect、churn
- network: delay、jitter、loss、burst loss、reorder、bandwidth、MTU
- resources: server CPU / memory、client farm の割当て

固定された reference preset と、利用者が変更できる custom scenario は同じ schema を
使う。preset から値を変更した結果は、その preset の公式比較表には混ぜない。

### Current implementation boundary

このShared ModelはAcceptedなtarget modelであり、2026-07-11時点のschema実装完了を
意味しない。現行v1 plan generatorが実装するtopologyは次に限定される。

- `authoritative-state`: 1 server、N client、clientごとのpersonalized state
- `room-relay`: 単一room、全participantがpublisherかつsubscriberの対称all-to-all
- scalar rate/payload（分布とburst指定なし）、steady-state（churn指定なし）

publishers/subscribersの非対称性、interest/fanout policy、複数room、payload/rate分布、
churn、reorder/MTU指定は未実装である。これらが必要なworkloadを現行schemaで「近い」と
主張せず、reference preset凍結前に必要範囲を実装する。

## Non-goals

- voice、video、VR、個別ゲームごとに独立した benchmark を増やすこと
- `environment-baseline` を最速 transport のランキングに使うこと
- `authoritative-state` と `room-relay` を合算して総合順位を作ること
- reference preset が production application の全処理を忠実に模倣すると主張すること

## Consequences

- 現行の `echo` は `environment-baseline` または scenario 実装の診断 probe として
  再配置し、単独で実用途名を付けない
- 現行の対称 broadcast workload は `room-relay` の一条件として再利用できるが、
  `authoritative-state` を代表しない
- workload の hard-coded name lookup では、publisher / subscriber の非対称性や
  interest policy を表現できないため、scenario schema から実行 plan を生成する
- reference preset の具体値、品質判定、統計手順、推薦規則は後続 ADR で合意する
