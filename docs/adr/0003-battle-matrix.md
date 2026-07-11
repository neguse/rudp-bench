# ADR-0003: 旧 battle matrix の撤回

- Status: **Withdrawn before acceptance**
- Date: 2026-07-10
- Replaced by: ADR-0001, ADR-0002

## Context

この ADR は、プロジェクト目的が合意される前に、transport、workload、RTT、loss、
接続数範囲、deadline と実行順を固定しようとした案だった。いずれの値も
ADR-0000 の目的と利用 workflow から導出されておらず、Status は Proposed のまま
実験が先行していた。

## Decision

旧 battle matrix は採用しない。そこから得た結果は、当時の条件に対する参考実験と
してのみ保持する。

新しい reference campaign は次の順で別 ADR に定める。

1. ADR-0001 の3 scenarioを実行可能な共通schemaとbenchspecへ実装する
2. environment baselineとconformanceを通す
3. pilotで定常性、測定時間、反復間分散、実用的な解像度を観測する
4. reference preset、SLO、network regime、resource budgetを根拠付きで提案する
5. project ownerの合意後に固定し、confirmatory campaignを開始する

pilot前の便宜的な数値をreference presetや推薦根拠として固定しない。
