# rudp-bench

リアルタイム通信の workload を再現し、既存 solution の採用、追加実装、
自作の判断材料を得るための benchmark harness。

単一の「最速ランキング」は作らない。scenario、品質要求、回線条件、resource、
solution treatment を固定し、その条件で SLO を満たす範囲と破断理由を測る。

## Scenario

最初の scope は3種類だけとする。

| kind | 目的 | 主な負荷形状 |
|---|---|---|
| `environment_baseline` | rig、負荷生成器、計測器の校正 | 単純な raw echo probe |
| `authoritative_state` | authoritative server と N client | client input + server独立tick、概ね O(N) |
| `room_relay` | room内publisher/subscriber | fanout、対称条件では概ね O(N^2) |

`environment_baseline`はsolutionの推薦やproduction capacityの順位には使わない。
用途名を増やす代わりに、rate、payload、SLO、network、resourceを同じschemaで変更する。

現行schemaのtopologyは意図的に狭い。`authoritative_state`は各client向けの個別state、
`room_relay`は単一roomの対称all-to-allだけを実装している。非対称publisher/subscriber、
interest policy、複数room、churnはAcceptedな将来modelには含むが、現時点では再現できない。

目的とscopeは[ADR-0000](docs/adr/0000-project-purpose.md)と
[ADR-0001](docs/adr/0001-battle-purpose.md)、wire/metrics契約は
[benchspec](benchspec/README.md)を参照。

## Status

- プロジェクト目的と3 scenarioのscope: **Accepted**
- 測定方法: [ADR-0002](docs/adr/0002-benchmark-methodology.md)で **Proposed**
- reference presetの数値とconfirmatory protocol: 未凍結
- reference blockのdoctor＋前後baseline drift gate: 未実装のため実行を禁止
- CPU/useful-op、memory/conn、wire amplification、capability/security/license表: 未実装
- 2026-07-11 no-loss smoke: [clean sourceで12 / 12 PASS / VALID](docs/measurements/2026-07-11-scenario-conformance.md)（完全なconformance/推薦ではない）
- 2026-07-11 provisional loss pipeline smoke: [clean sourceで4 / 4 PASS / VALID](docs/measurements/2026-07-11-provisional-loss-smoke.md)（class-mapping conformance/推薦/性能比較ではない）
- fixed class-mapping conformance: 実行器と再開可能な証拠ledgerを実装済み。実測reportは未取得
- 旧broadcast battle結果: 過去条件の参考値。新scenarioの推薦データではない

数値が未凍結の間に得た値はsmoke、conformance、またはpilotとして扱い、reference比較表へ
混ぜない。

## Build

Linux、Go、CMake、C/C++ toolchainが必要。netemを使う測定には`iproute2`、
packet exposureを検証するloss conformanceには`ethtool`、netem gateには`ping`と
`iperf3`、一括conformanceの最終検査には`jq`も必要になる。

```sh
# orchestrator
go build -o build-v2/orchestrator ./orchestrator/cmd/orchestrator

# common C metrics/control library
cmake -S benchkit -B build-v2
cmake --build build-v2 -j
ctest --test-dir build-v2 --output-on-failure

# raw reference adapter（他のnative adapterもservers/<name>を同様にbuild）
cmake -S servers/raw_udp -B build-v2-rawudp
cmake --build build-v2-rawudp -j
ctest --test-dir build-v2-rawudp --output-on-failure
```

Managed adapterは.NET 10を使う。

```sh
dotnet build -c Release -m:1 servers/litenetlib/LiteNetLibBench.sln
dotnet build -c Release -m:1 servers/websocket/WebSocketBench.sln
dotnet build -c Release -m:1 servers/magiconion/MagicOnionBench.sln
dotnet run --no-build -c Release --project servers/magiconion/BenchKit.CS.Tests/BenchKit.CS.Tests.csproj
```

## Measurement Workflow

1. **Doctor**: rigのclocksource、bench CPU governor、隔離、nofile、残留qdisc、
   必要toolを検査し、環境metadataを保存する。
2. **Calibration**: known-answer accounting、fault injection、duration invariance、
   netem実効値を確認する。
3. **No-loss smoke**: 低負荷でscenario semantics、traffic別会計、SLO判定を確認する。
4. **Loss conformance**: class mappingとmust-deliverの欠落・重複・破損を検査する。
5. **Pilot**: warmup、duration、境界探索範囲、block分散、停止則を決める。
6. **Confirmatory blocks**: 凍結したpresetとprotocolをrandomized blockで反復する。
7. **Report**: `PASS / FAIL / INVALID / CENSORED / UNSUPPORTED / INCONCLUSIVE`を
   区別し、同じcomparison identityだけを集約する。

DoctorはFAILでもJSONを保存し、終了code 2を返す。

```sh
build-v2/orchestrator doctor \
  -rig orchestrator/rigs/home.json \
  -output results-v2/doctor.json
```

dirty treeで`-output`を指定した場合は、status、tracked binary patch、untracked source tarと
file hash manifestもreport横へ保存する。Git stateを読めない場合やdirty submoduleがある場合は
DoctorをFAILにする。referenceはこれに依存せずclean treeを必須とする。

capacity sweepは`measurement_mode`を`conformance`、`screening`、`pilot`、
`reference`から明示する。`reference`は直近15分のPASS doctor report、同じbinary/host、
clean source tree、`conns.min=1`、そして`baseline` block（前後のenvironment baselineと
drift許容幅）を必須とする。blockはbaseline→cells→baselineの順で実行され、driftが
許容幅を外れた場合は全cellが`block_invalid`となり数値aggregateから拒否される。
drift許容幅はpilotの観測から凍結する値で、configが宣言する。`loss_seed`を指定した
deterministic loss runは、losstrace packet counterのwindow内サンプルと再生成した
traceのpopcountによるknown-packet drop会計を`loss_evidence.deterministic`へ保存し、
gateがtrace hash・drop数・window境界を再検証する。randomとseedの混在は拒否される。

低負荷の3 scenarioはraw adapterでそのまま実行できる。

```sh
build-v2/orchestrator run -config orchestrator/examples/local-baseline-rawudp.json
build-v2/orchestrator run -config orchestrator/examples/local-authoritative-rawudp.json
build-v2/orchestrator run -config orchestrator/examples/local-room-relay-rawudp.json
```

duration不変性、doctor、raw environment baseline、6 solutionの2 use-case
no-loss smokeをまとめて実行する場合は、socket確認に必要な権限をこの1コマンドへ
まとめられる。

```sh
scripts/run-scenario-conformance-local.sh
```

random loss下の実行・判定・証拠保存を確認する暫定4セルsmokeは、root権限を1コマンドに
まとめる。条件と証拠の限界は[provisional loss smoke](docs/provisional-loss-smoke.md)に固定する。

```sh
sudo -E scripts/run-provisional-loss-smoke.sh
```

class mappingの正式な固定probeは、各transportについてclean 2 caseと片方向1% loss
4 caseを実行する。planと各attemptを実行前に永続化するため、中断後も同じattemptを
良い結果が出るまで再利用しない。doctorから全artifactまでをmanifestへ束縛し、権限が
必要な実行は次の1コマンドにまとめる。raw UDPは環境診断として含め、提供しないMD
caseは`UNSUPPORTED`のまま保存するが、候補6 solutionのpromotion判定とは分離する。

```sh
scripts/run-class-mapping-conformance.sh
```

途中で中断したsessionは、同じ`SESSION_DIR`を指定して同じコマンドを再実行すると再開する。
固定planだけを確認する場合は次を使う。

```sh
build-v2/orchestrator mapping-conformance \
  -config orchestrator/examples/class-mapping-conformance-local.json -plan
```

各JSONの`scenario`を複製してrate、payload、SLO、接続数、netem、CPU割当を変更すれば、
利用者自身のworkloadを同じ実行・判定経路で測れる。名前が同じでも定義hashが異なる
結果はcacheや集約で混合されない。現行の固定topologyで近似できないworkloadは、
近いと装わずschema拡張後に測る。

比較カタログ用の凍結条件は`preset`で指名する（[ADR-0004](docs/adr/0004-reference-preset.md)）。
presetはscenario、netem、warmup/duration/drain、staleness periodを固定し、config側の
上書きを拒否する。server resource budget（2 vCPU）として`server_cpus`がちょうど2 CPUで
あることを要求する。定義は`orchestrator/run/preset.go`、変更は新versionの追加で行う。

```sh
build-v2/orchestrator run -config orchestrator/examples/ref-auth-wan-s1000-enet.json
```

preset名: `ref-auth-{lan,wan}-s{1000,4000}-v1`（authoritative-state、state payloadは
MTU内1000 BとMTU超4000 Bの2点）、`ref-room-{lan,wan}-v1`（room-relay）。

## Verification

```sh
go vet ./orchestrator/...
go test -count=1 ./orchestrator/...
./calibration/duration_invariance.sh
```

公開値に必要な測定方法、outcome、再現性record、推薦規則、自作判断の基準は
[ADR-0002](docs/adr/0002-benchmark-methodology.md)に定義する。
