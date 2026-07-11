# 2026-07-11 Scenario No-Loss Smoke

- Status: **no-loss implementation smoke only**
- Executed: 2026-07-11 06:40--06:42 UTC
- Host: `smolcenter` / Linux `7.0.14-arch1-1` / amd64
- Source: clean commit `11bfd169b11c4c04cdcd70b1207553c37ae18d27`
- Campaign identity:
  `32f34e20e9c92029dcad781253684f029f364b998f8d643ecd26db7db961b002`
- Local raw bundle (gitignore対象):
  `results-v2/conformance-sessions/20260711T064045Z-2139892`

このrunが確認するのはADR-0002のRQ0/RQ1の一部だけである。6 solutionが低負荷・
no-lossで`authoritative_state`と`room_relay`のscenario/accounting subsetを実行できた。
loss時のclass mappingとmust-deliver動作は未検証であり、完全なconformance認定ではない。
性能順位、capacity、solution推薦、build-vs-buyの根拠には使用しない。

## Conditions

- loopback、netemなし、client 1 process、3 connections、warmup 2 s、measurement 2 s、
  drain 500 ms
- `authoritative_state`: client LT input 13 Hz x 64 B、server LT state 20 Hz x 64 B、
  双方向MD 1 Hz x 64 B
- `room_relay`: LT publish 20 Hz x 128 B、MD publish 1 Hz x 64 B、対称all-to-all
- 全primary threshold: delivery/deadline hit 1.0、LT staleness p99 <= 200 ms
- environment baseline: raw UDP、3 connections、LT echo 100 Hz x 64 B
- sweep範囲は`conns.min=max=3`。表示上のcapacity 3は`range_limited=true`であり、
  最大収容数を意味しない

実行定義は
[`sweep-scenario-conformance-local.json`](../../orchestrator/examples/sweep-scenario-conformance-local.json)
と[`local-baseline-rawudp.json`](../../orchestrator/examples/local-baseline-rawudp.json)に固定した。

## Calibration And Doctor

duration invarianceは4 sと12 sを比較し、LT/MD両classでdelivery 1.0、offered slot rate
200.00/s、差分0だった。tolerance 0.005に対してPASSし、short/long bundleと
`duration-invariance/summary.json`をsession内に保存した。

raw UDP baselineは`PASS / VALID`だった。delivery 1.0、3 expected flows、
staleness p99 10.24 ms、両participantの`invalid_payload=0`を確認した。

Doctorは`FAIL`だった。FAIL checkは次の2件だけである。

- clocksourceが期待した`tsc`ではなく`hpet`。利用可能一覧も`hpet, acpi_pm`
- requested affinity上でIRQ `0, 2, 54-58`がbenchmark CPUと交差

raw Doctor artifactはrequested affinityを判定したため、IRQ 0と2もFAIL detailへ含めた。実行後に
`effective_affinity_list`を監査すると、IRQ 0はCPU 0のみ、IRQ 2は実効配送先なしで、この2件は
誤検知だった。一方、NVMe IRQ 54-58は実際にbenchmark CPU 11-15へ配送されていたため、IRQ隔離の
FAIL自体は有効である。以後のDoctor判定はeffective affinityを基準にする。

kernel logではboot中にTSCへ切り替えた後、clocksource watchdogがHPET比
`-3.387825 ms / 507 ms`のskewを検出してTSCをunstableと判定し、HPETへfallbackしていた。
kernel command lineによるTSC強制はなく、CPUには`constant_tsc`と`nonstop_tsc`がある。
正式測定では`tsc=reliable`等で検査を隠さず、BIOS/firmware/kernel側の原因を解消する。

CPU layout、benchmark CPUのperformance governor、PID 1とsystem/user/init sliceの隔離、
nofile、残留netem/netns、必須toolはPASSした。DoctorがFAILのため、このhost状態の値を
reference performance dataへ昇格させない。

source state checkはclean commit `11bfd16`でPASSした。完了manifestは12 result cellを記録し、
duration summary、Doctor、baseline、実行config、results、capacityのSHA-256を
`session-manifest.json`に保存した。raw bundle自体はgitignore対象であり、この文書には追加しない。

## Results

全12 cellを初回attemptで`PASS / VALID`と記録し、12件すべてが固有acquisition IDを持つ。
全traffic seriesでdelivery 1.0、MD deadline hit 1.0となり、contract validatorはclass aggregate、
traffic aggregate、histogram、top-level stalenessの一致を確認した。表のp99はこの低負荷smokeの
観測値であり、性能順位やcapacity比較には使用しない。

| solution | authoritative input/state p99 (ms) | room p99 (ms) | current outcome (auth / room) |
|---|---:|---:|---|
| ENet | 77.824 / 40.960 | 51.200 | PASS / PASS |
| GameNetworkingSockets | 77.824 / 51.200 | 51.200 | PASS / PASS |
| LiteNetLib | 77.824 / 47.104 | 49.152 | PASS / PASS |
| MagicOnion | 69.632 / 43.008 | 51.200 | PASS / PASS |
| MsQuic | 77.824 / 45.056 | 51.200 | PASS / PASS |
| WebSocket | 69.632 / 43.008 | 40.960 | PASS / PASS |

全12 runのno-loss traffic seriesについて、次は観測できた。

- 全24 process reportの`invalid_payload`合計は0
- authoritativeは各solutionでroster/local connectionsが3、expected receiver flowsが3
- authoritativeの全connectionで最終LT input sequenceが52までstateへ反映
- authoritativeのglobal state header最終値は全connectionで80、measurement中のserver tickは40
- room relayのexpected receiver flowsは3 x 3 = 9
- offered slot、traffic key、flow cardinality、staleness sample coverageがschemaと一致

## Superseded Run And Fix

同日05:27 UTCの修正前run
`results-v2/conformance-sessions/20260711T052725Z-2051907`は、実行時binaryでは12 PASSを
記録したが、監査後の現行contract分類では9 PASS / 3 INVALIDだった。INVALIDはLiteNetLib、
MagicOnion、WebSocketの`authoritative_state`で、共通矛盾は
`must_deliver class deadline_hit=6, traffic sum=12`だった。rejudge保存上は数値的なSUT breakと
混同しないよう3件を`CENSORED / measurement_invalid`として扱った。

原因はmanaged metricsがclass aggregateをglobal deadline、traffic seriesをtraffic固有deadlineで
別々に判定したことにある。C/C#実装はresolved traffic deadlineを共通判定源に修正し、
orchestratorもaggregate一致を必須化した。上表と12 / 12の判定は、この修正を含むclean commit
`11bfd16`でsocket workloadを再実行した結果であり、旧INVALIDを後からPASSへ書き換えたものではない。

旧runのsourceはcommit `a0fac12`とdirty source hash
`a03266c1b8917ba8ebe4eac7ffeb64419cac075cc8c60d521fb8f8587c0da6b4`だった。patch本文、
duration bundle、完了manifestが残っていないため、旧runは再現可能な証跡としても使用しない。

未通過のconformance項目は、loss注入による`--describe`どおりのclass mapping判別と、
must-deliverのloss下での欠落・重複・payload破損検査である。

## Interpretation

現時点で有効なのは6 solution x 2 use-case、計12 cellのno-loss low-load smokeである。
これは実装と会計契約の確認であり、「これならこれ」やsolution推薦の証拠ではない。
推薦へ進むには、残るloss conformanceを通し、DoctorをPASSさせ、reference preset、
network regime、resource budget、pilot precision、
confirmatory stopping ruleを合意してからRQ2-RQ5を測る必要がある。非対称publisher/subscriber、
複数room、interest、churn、CPU/useful-op、memory/connection、wire amplificationも未測定である。
