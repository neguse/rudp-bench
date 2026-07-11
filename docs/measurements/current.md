# Current Reference Measurement

**None.** ADR-0000/0001で目的と3 scenarioを再定義し、ADR-0002の測定方法、
reference preset、confirmatory protocolはまだAcceptedになっていない。

現行contractのno-loss subset確認は
[`2026-07-11-scenario-conformance.md`](2026-07-11-scenario-conformance.md)に記録した。
これは低負荷smokeであり、完全なRQ0/RQ1証跡、reference比較、推薦データではない。
修正前runの9 PASS / 3 INVALIDを踏まえてdeadline accountingとvalidatorを修正し、clean commit
`11bfd16`による再実行ではduration invarianceがPASS、全12 cellが`PASS / VALID`だった。
ただしDoctorはclocksourceとIRQ affinityでFAILしており、loss conformanceも未実施である。

random loss下での暫定pipeline確認は
[`2026-07-11-provisional-loss-smoke.md`](2026-07-11-provisional-loss-smoke.md)に記録した。
clean commit `5c52aab`で前後baselineとENet/WebSocketの4セルが`PASS / VALID`だったが、
class-mapping conformance、reference、推薦、性能比較には使用しない。

fixed class-mapping probe（directional loss下のLT/MD semantics検査）は
[`2026-07-12-class-mapping-conformance.md`](2026-07-12-class-mapping-conformance.md)に
記録した。clean commit `e313a33`で候補6 solutionの全36 caseがPASS、raw_udpは
開示どおりLT PASS / MD UNSUPPORTEDだった。DoctorがFAIL（smoke rig方針）のため
Promotable=falseであり、昇格可能なconformance証跡はreference rig受入後に再取得する。

以下は旧broadcast-centric条件のlegacy canonical recordであり、新scenarioの比較表、
solution推薦、build-vs-buy判断には使用しない。

## Legacy Record

Open: [`2026-07-02-canonical-133439Z/report.md`](2026-07-02-canonical-133439Z/report.md)

Capacity data: [`2026-07-02-canonical-133439Z/capacity.csv`](2026-07-02-canonical-133439Z/capacity.csv)

Published from: `results/bench_20260702T111105Z`

元のlink部分は`rudp-benchctl`が生成したrecordを保持している。
