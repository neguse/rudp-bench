# 2026-07-11 Scenario No-Loss Smoke

- Status: **no-loss implementation smoke only**
- Executed: 2026-07-11 05:27 UTC
- Host: `smolcenter` / Linux `7.0.14-arch1-1` / amd64
- Source: commit `a0fac12b22dcf669fc98a9a3b10dc6a654f98c1c` plus dirty patch
  `a03266c1b8917ba8ebe4eac7ffeb64419cac075cc8c60d521fb8f8587c0da6b4`
- Campaign identity:
  `e38fe05390b6e68b05c91b9ba8e235e9b85dc50db6d453b0564e9d4898de7c85`
- Local raw bundle (gitignore対象):
  `results-v2/conformance-sessions/20260711T052725Z-2051907`

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

一括実行のterminalには、ENet、4 connections、LT/MD各50 Hz/connectionで4 sと12 sを
比較し、両classともdelivery 1.0000、offered slot rate 200.00/sでPASSと表示された。
しかし当時のscriptは生runとlogを保存していない。この値は第三者が監査できないため、
本recordの有効なcalibration evidenceおよびRQ0成立根拠から除外する。以後は
`CALIBRATION_DIR`指定によりshort/long bundleと`summary.json`を保存する。

raw UDP baselineは`PASS / VALID`だった。delivery 1.0、3 expected flows、
staleness p99 10.24 ms、両participantの`invalid_payload=0`を確認した。

Doctorは`FAIL`だった。主な阻害条件は次の2件である。

- clocksourceが期待した`tsc`ではなく`hpet`。利用可能一覧も`hpet, acpi_pm`
- IRQ `0, 2, 54-58`のaffinityがbenchmark CPUと交差

CPU layout、benchmark CPUのperformance governor、PID 1とsystem/user/init sliceの隔離、
nofile、残留netem/netns、必須toolはPASSした。DoctorがFAILのため、このhost状態の値を
reference performance dataへ昇格させない。

実行時sourceはclean commitではなく、Doctorに残ったのはcommitとdirty source hashだけで、
tracked patch本文とuntracked source archiveは保存されなかった。現在のworktreeは実行後に
変化しており、このhashから実行時sourceを復元できない。そのため本runは観測値の内部整合を
確認するsmoke recordであり、再現可能な公開証跡ではない。以後のDoctorはdirty実行時に
status、binary patch、untracked tarとfile hash manifestをreport横へ保存する。

このsessionは完了時のartifact manifestも出力していない。本recordが正式対象として列挙する
のは`doctor.json`、`environment-baseline/result.json`、`solutions/results.jsonl`、
`solutions/capacity.json`と各run bundleだけである。後付けのsandbox再試行はsession外へ分離した。
以後の一括runnerは校正を含む正式artifactのSHA-256を`session-manifest.json`へ保存する。

## Results

実行時binaryは全12 cellを初回attemptで`PASS / VALID`と記録し、各cellは固有acquisition
IDを持つ。しかし実行後の監査で、managed 3 solutionのauthoritative resultにlegacy
class aggregateとtraffic seriesの不一致が見つかった。現行validatorで元bundleをコピーして
再判定したcontract分類は`PASS=9 / INVALID=3`である。再測不能な過去のINVALIDをSUTの
capacity breakへ数えないため、rejudge output上は3件を`CENSORED / measurement_invalid`
として保存する。

INVALID 3件でもprimary判定に使うtraffic series自体はdelivery 1.0、MD deadline hit 1.0を
示すが、同一file内のaccounting contractが矛盾するため合格証拠には採用しない。表のp99も
性能比較値ではなく、invalid rowでは参考値に限る。

| solution | authoritative input/state p99 (ms) | room p99 (ms) | current outcome (auth / room) |
|---|---:|---:|---|
| ENet | 77.824 / 40.960 | 51.200 | PASS / PASS |
| GameNetworkingSockets | 77.824 / 51.200 | 51.200 | PASS / PASS |
| LiteNetLib | 77.824 / 51.200 | 49.152 | INVALID / PASS |
| MagicOnion | 69.632 / 45.056 | 40.960 | INVALID / PASS |
| MsQuic | 77.824 / 45.056 | 51.200 | PASS / PASS |
| WebSocket | 69.632 / 45.056 | 51.200 | INVALID / PASS |

3件の共通原因は`must_deliver class deadline_hit=6, traffic sum=12`である。managed metricsが
class aggregateをglobal deadline、traffic seriesをtraffic固有deadlineで別々に数えていた。
C/C#実装はresolved traffic deadlineを共通判定源に修正し、orchestratorもclass count/histogram
とtraffic合計、top-level stalenessの一致を必須化した。修正後binaryによるsocket runは
再実施していないため、このrecordから3件をPASSへ戻さない。

全12 runのno-loss traffic seriesについて、次は観測できた。

- 全24 process reportの`invalid_payload`合計は0
- authoritativeは各solutionでroster/local connectionsが3、expected receiver flowsが3
- authoritativeの全connectionで最終LT input sequenceが52までstateへ反映
- authoritativeのglobal state header最終値は全connectionで80、measurement中のserver tickは40
- room relayのexpected receiver flowsは3 x 3 = 9
- offered slot、traffic key、flow cardinality、staleness sample coverageがschemaと一致

ただしmanaged authoritative 3件は上記aggregate不整合によりrun全体がINVALIDである。

未通過のconformance項目は、loss注入による`--describe`どおりのclass mapping判別と、
must-deliverのloss下での欠落・重複・payload破損検査である。

## Interpretation

現時点で有効なのはnative 3 solutionの2 use-caseとmanaged 3 solutionのroom relayに対する、
計9 cellのno-loss smokeである。managed authoritativeは修正後再実行まで未確認とする。
「これならこれ」を決めるには、全cellをclean sourceで再実行し、残るloss conformanceを通し、
DoctorをPASSさせ、reference preset、network regime、resource budget、pilot precision、
confirmatory stopping ruleを合意してからRQ2-RQ5を測る必要がある。非対称publisher/subscriber、
複数room、interest、churn、CPU/useful-op、memory/connection、wire amplificationも未測定である。
