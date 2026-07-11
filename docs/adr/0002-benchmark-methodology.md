# ADR-0002: Benchmark の測定方法

- Status: **Accepted**（2026-07-12 owner 承認。Open Decisions は ADR-0004 で決定）
- Date: 2026-07-10
- 依存: ADR-0000, ADR-0001
- Decision owner: project owner

## Context

reference data から solution を選べることと、利用者が自身の環境で再検証できる
ことの両方を満たすには、単発の最大接続数では不十分である。用途 SLO、測定環境の
限界、solution の設定、反復間のばらつき、性能以外の要件を分離して記録する。

## Research Questions

本 benchmark は次の順で問いに答える。

1. **RQ0 Validity**: この rig と計測器は、予定した負荷を生成・観測できるか
2. **RQ1 Feasibility**: solution は対象 scenario の意味論と必須要件を実装できるか
3. **RQ2 Quality**: 目標負荷で用途 SLO を満たせるか
4. **RQ3 Capacity**: 固定 resource 条件で、SLO を維持できる負荷範囲はどこまでか
5. **RQ4 Cost**: その範囲で必要な CPU、memory、wire traffic、運用上の代償は何か
6. **RQ5 Mechanism**: 品質が破断する原因と、solution 間の差を何が説明するか

RQ0/RQ1 を満たさない結果を、RQ2 以降の比較には使用しない。

## Unit Under Test

比較単位は library 名ではなく、次を固定した **solution treatment** とする。

- server と client の実装
- transport / protocol library と version
- runtime、compiler、build mode
- security、reliability、thread、buffer、congestion control などの設定
- scenario への mapping と application-level framing / serialization

同じ library でも上記が異なれば別 treatment である。公開値には完全な設定を付ける。
client farm は原則として測定器であり、server treatment と分離して十分性を検証する。
client 自体の resource cost や接続確立性能を測る実験では、その旨を明記して client を
measurand に含める。

## Scenario And Preset

- ADR-0001 の3 scenario は共通 schema から実行 plan を生成する
- versioned reference preset は比較カタログ用で、値を上書きできない
- custom scenario は同じ schema の任意設定で、利用者の候補検証に使う
- preset の hash が異なる結果を同じ比較表へ混ぜない
- scenario 名ではなく、正規化した全パラメータを結果へ保存する

`environment-baseline`、`authoritative-state`、`room-relay` の結果は別表にし、共通の
最大接続数や総合 score へ集約しない。

## Measurands

### Quality

- latest-value traffic: receiver-flow ごとの staleness 分布、deadline hit、starvation
- must-deliver traffic: deadline hit、eventual delivery、duplicate、corruption
- ordering が要件の場合: reorder と head-of-line delay
- 共通: attempted load と achieved load、disconnect / crash、queue / drop

全 flow の合算平均だけで品質を判定しない。少数の receiver が完全に飢える状態を
隠さない集約規則を用いる。ネットワークの物理 floor は実行可能性と原因説明に使い、
用途の絶対 SLO を回線条件に応じて緩めるためには使わない。

primary判定はenabledな`(traffic_id, direction, class)`ごとに行い、全seriesが
PASSした場合だけscenarioをPASSとする。

- loss-tolerant: `min_delivery_ratio`、`staleness_p99_ns`、starvationを判定する。
  authoritativeのreceiver flowは計測前に全件登録し、
  `never_received_flows > 0`をpercentileと独立したFAILにする
- must-deliver: 未送信slotを分母に含む`min_eventual_delivery_ratio`、
  resolved deadlineに対する`min_deadline_hit_ratio`、duplicateを判定する
- SLO未設定の診断値は記録するが、暗黙の合否閾値を後付けしない
- adapterのtraffic key、flow cardinality、deadline、metrics schema欠落はSUTの
  capacity breakではなく`INVALID`とする

### Capacity

capacity は単発の整数ではなく、固定した scenario、SLO、resource、treatment に対する
次の組で報告する。

- 反復測定で支持された最大負荷
- 最初に支持されなかった負荷
- 探索範囲または測定器の限界による censoring
- block ごとの生値と不確かさ
- 各境界で最初に破った SLO と破断原因

`authoritative-state` の単位は `clients/server`、`room-relay` の主単位は
`participants/room` とする。複数 room の総収容数は独立した因子であり、同じ値として
扱わない。

### Cost And Overhead

- server CPU time / useful application operation
- server RSS と増分 memory / connection
- application payload に対する wire bytes と packet amplification
- client resource は farm 十分性と client cost に分けて記録
- thread、syscall、GC、queue、retransmission は破断原因の診断値として記録
- connection setup、reconnect、churn は steady-state capacity と別に報告

raw UDP / TCP や null transport は特定層の environment floor であり、security、
reliability、congestion control を持つ solution の普遍的な性能上限とは呼ばない。

## Experiment Structure

### 1. Pre-registration

測定 campaign ごとに、変更前の文書または machine-readable manifest へ次を固定する。

- hypothesis と比較対象
- preset、SLO、network regime、resource budget
- primary / secondary metrics
- screening 範囲、確認点、block 数または precision-based stopping rule
- invalid / censored / unsupported の規則
- 実行順、random seed、予定時間、打ち切り条件

### 2. Environment Baseline

本測定の前後に同じ baseline を実行する。

- known-answer accounting と deterministic fault injection
- clock、scheduler、barrier、duration invariance
- delay、jitter、loss、reorder、bandwidth の実効値
- client generator / sink の余力と raw datagram / stream floor
- block 前後の drift

baseline が事前に定めた許容範囲を外れた block は `INVALID` とし、solution の失敗に
数えない。許容範囲自体は pilot の観測後、本測定前に固定する。

静的preflightは`orchestrator doctor`で実施し、rigが宣言するclocksource、bench CPU
governor、CPU隔離、nofile、残留qdisc、必要toolとsource状態をJSONへ保存する。
低負荷のraw echo probeは静的preflightの代替ではなく、生成・送受信・集計経路の
動的known-answer checkである。

lossを指定したrandom netem runは、setup時のping/iperf実効値gateに加え、controlが確定した
measurement windowの内側で両egress qdisc counterを取得する。loss指定方向ごとにdrop deltaが
非zeroであり、取得時刻、qdisc設定、counter差分が整合することを有効性条件とする。これは
warmupや事前probeだけにlossが当たったrunを合格証拠にしないためである。deterministic
losstraceはeBPF drop counterを同じwindowへ統合するまでloss exposure未検証として`INVALID`にする。

### 3. Conformance And Screening

- treatment が scenario の traffic semantics と control protocol を満たすか検証する
- 低負荷点で accounting と SLO 判定を確認する
- 広い負荷範囲を粗く探索し、境界候補と farm / rig の必要容量を把握する
- screening の N=1 値は exploratory と明記し、推薦には使わない

#### Fixed Class-Mapping Probe

solutionが開示した`loss_tolerant` / `must_deliver` mappingのdelivery semanticsは、
各transportで次の6 caseを固定して検査する。

| case | class | loss |
|---|---|---|
| clean control x 2 | LT / MD | なし |
| directional loss x 4 | LT / MD | client egress / server egressの片側だけ1% random netem |

probeは1 connectionのclass-exclusive echo、50 Hz、1000 byte、20秒の1000 slotとし、
warmup 25秒、drain 5秒、link MTU 1500を使う。`splitmix64-v1` payload、wire compression
なし、送信slotの全submit、`sched <= send <= recv`、両vethのsegmentation/coalescing
offload無効を検証する。qdiscは設定loss以外のimpairment、相関、requeue、queue overflowを
許さず、保存したraw `tc`出力から設定とcounterを再parseする。measurement window内で設定方向の
dropが実際に発生し、同方向のqdisc送信byte/packet数がeligible payloadから求めた下限以上である
ことを確認する。pre-run iperf gateはpacket数も保存し、固定1%条件で実測0%を許可しない。
process recordは計画したnetns、privilege drop、role別CPU affinity、展開後argvと完全一致させる。

family-wise alphaはtransportごとに0.01、4 loss caseへBonferroni分割して各0.0025とする。
window境界から十分離れて実際に届いたpayload byteをMTUで割った保守的packet trial下限に対し、
`(1 - 0.01)^trials <= 0.0025`をloss exposure成立条件とする。LTで欠落を観測すれば開示した
best-effortと整合する。欠落0はbest-effortを反証できないため`INCONCLUSIVE`とする。
MDは固定drainまで欠落・重複・破損0かつloss exposure成立で整合し、欠落等があれば`FAIL`とする。
clean controlの欠落・重複・破損、endpoint crash、切断は`FAIL`、測定証拠の不整合は`INVALID`、
明示的に提供しないmappingは`UNSUPPORTED`とする。

各caseは有効なacquisitionを1件だけ採用し、事前に固定した最大2 attemptを超えて再試行しない。
plan receipt、resultのsize/hash、extraction、失敗・中断をappend-only ledgerへ先に永続化する。
clean controlがPASSしないclassのloss caseは実行せずdependency skipとして残す。sessionの
`Promotable`は全candidate transportの最終outcomeに加え、束縛したDoctor reportがPASSの
場合だけtrueとする。raw UDPのような環境診断transportは、実装済みcaseのPASSを要求する一方、
開示どおり提供しないcaseの`UNSUPPORTED`をcandidateの不合格へ変換しない。
同一host上の全runはrig-global acquisition lockで直列化し、別sessionの同時計測を許可しない。

### 4. Pilot

代表的な2 treatment と3 scenario を使い、warmup、duration、block 間分散、境界の
安定性、順序効果、必要 event 数を調べる。反復数、precision、drift 許容幅は pilot
から決め、reference campaign 開始前に固定する。pilot data は最終比較へ混ぜない。

### 5. Confirmatory Blocks

- 独立 block を実験単位とする。packet や message を独立反復として数えない
- 各 block 内で同じ候補を同じ条件で測り、実行順を randomize または balance する
- 各 solution の screening で得た境界近傍を反復する
- 同じ失敗を理由なく再試行して良い値だけを採用しない
- 事前登録した stopping rule を満たすまで反復し、上限到達時は `INCONCLUSIVE` とする

## Outcome States

各実行は次のいずれかとして保存し、失敗実行も削除しない。

| state | 意味 |
|---|---|
| `PASS` | 計測が有効で、全 primary SLO を満たした |
| `FAIL` | 計測が有効で、SUT が SLO を破った、切断した、または crash した |
| `INVALID` | 環境、計測、同期、設定適用に問題があり評価不能 |
| `CENSORED` | farm、rig、探索範囲の限界で SUT の境界を観測できなかった |
| `UNSUPPORTED` | treatment が必須 semantics / capability を提供しない |
| `INCONCLUSIVE` | 事前登録した precision / stability に到達しなかった |

再試行できるのは原則 `INVALID` のみで、元の run と再試行理由を残す。
retryは同じdirectoryを上書きせず、同一run identity配下の別`attempt-NNN` bundleへ保存する。
再試行後も`INVALID`なら探索は`measurement_invalid`付きで打ち切るが、通常のright-censored
capacity下限には数えず、block横断の数値aggregateは再取得まで拒否する。
`UNSUPPORTED`と`INCONCLUSIVE`はcapacity 0へ変換せずcell outcomeとして保存し、他cellの
実行を継続する。数値capacityのaggregateへterminal outcomeを混入させない。

## Reproducibility Record

各 run bundle は少なくとも次を含む。

- scenario、SLO、resource、全 command / environment、seed
- source commit、dirty patch、binary / dependency hash、compiler / runtime
- kernel、microcode、CPU topology、clocksource、governor、affinity、IRQ、offload
- sysctl、ulimit、cgroup、network namespace と実効 network measurement
- treatment の machine-readable description と tuning 根拠
- raw event / histogram、process sample、判定、log

dirty treeを許す診断実行では、hashだけでなくtracked binary patch、status、untracked
source archiveとfile hash manifestをDoctor report横へ保存する。referenceはclean treeを
必須とし、この例外を使わない。

run/comparison identityにはcommandとbinaryだけでなく、hostname、kernel、CPU/microcode、
topology、clocksource、governor、machine-id hashと挙動関連environmentを含める。cache は
この再現性 recordから作るrun identityが一致し、かつ同じcampaignの場合だけ使用する。
metrics v2の必須field、448-bin layout、countとbin合計、全processのversion、
scenario別traffic contractも読み込み時に検証する。別campaignで同じrunを再利用する
場合はcampaign membershipを明示し、比較定義が異なるblockを同じ統計へ集約しない。
全measurement modeでcross-campaign cacheを禁止し、各capacity cellが参照する
acquisition IDのblock間重複も集約時に拒否する。host fingerprintが違うcellは同じ
comparisonとして集約しない。

## Recommendation Rule

reference report は次の順で候補を評価する。

1. platform、security、semantics、license などの hard requirement で除外する
2. 必須 scenario / regime の SLO と目標負荷を満たすか判定する
3. 統計的不確かさを含む capacity headroom を比較する
4. 実用上区別できない候補は同等群とし、CPU、memory、wire cost、API、保守性、
   interoperability、運用成熟度で選ぶ
5. 条件、根拠、代替候補、適用限界を併記する

全 scenario を平均した総合 score は作らない。

## Existing Solution Versus Custom Build

既存 solution を初期候補とする。custom build の検討へ進む根拠は、少なくとも次の
いずれかを実証した場合に限る。

- 既存候補が必須 semantics、platform、license、interoperability を満たさない
- 有効な反復測定で、全既存候補が必要な SLO / capacity / cost を満たさない
- profiling と層別 baseline により、変更不能な library 固有 bottleneck が支配的と
  確認できた

custom prototype も既存 solution と同じ conformance、security、loss / congestion、
運用条件で測る。性能差だけでなく、実装、検証、保守、脆弱性対応、相互運用の継続費を
上回る便益を示せない場合、自作を推薦しない。

## Open Decisions Before Acceptance

以下の 4 点は [ADR-0004](0004-reference-preset.md) で決定した（2026-07-12）。

- reference preset の topology と数値
- 実用上意味のある最小差と、必要 precision
- pilot から confirmatory protocol を凍結する具体手順
- performance 以外の hard requirement / capability matrix

## Current Execution Gate

2026-07-11にpayload corruption、authoritative input反映進捗、global tick独立性、
staleness coverageを含む強化後のgateで実走した。managed 3 solutionのauthoritativeは
class/traffic aggregate consistencyの矛盾でINVALIDとなったが、deadline判定源を統一する
修正の後、clean commit `11bfd16`で6 solution x 2 use-caseの12 cellを再実行し全PASSを
記録した。結果と適用限界は
[measurement record](../measurements/2026-07-11-scenario-conformance.md)に固定した。
loss時のclass mappingとmust-deliver検査はFixed Class-Mapping Probe（前掲）で行う。

home rigのdoctorは`clocksource=hpet`（期待値`tsc`）とbenchmark CPUへ交差する
IRQ affinityでFAILする。home rigは24時間サーバ同居のためsmoke専用とし
（ルートCLAUDE.md「rig運用」2026-07-12決定）、この2件はhome rigでは是正しない。
reference開始には少なくとも次が残る。

- reference rig（EC2 c8g、target silicon一致）を調達し、doctorをPASSさせる
- loss注入のclass mapping / must-deliver conformanceは2026-07-12にsmoke rigで
  全candidate PASSを確認済み（[record](../measurements/2026-07-12-class-mapping-conformance.md)、
  Promotable=false）。昇格可能な証跡はreference rigで同一probeを再実行して取得する
- clean sourceから校正を含むrunを再実行し、source/calibration bundleを永続化する
- reference preset、pilot precision、confirmatory stopping ruleを合意・凍結する
- ~~blockへdoctorと前後baseline drift gateを統合する~~（2026-07-12 実装済み —
  reference modeはPASS doctor + baseline blockを必須とし、drift外れは
  `block_invalid`としてaggregateから拒否される。許容幅の凍結値はpilotで決める）
- topology schemaの必要範囲とRQ4/RQ5のcost/mechanism出力を実装する
- deterministic `loss_seed`のknown-packet trace gateを実装する

これらが完了するまで今回の値はno-loss implementation smokeに限定し、solution推薦や
build-vs-buy結論には使用しない。
