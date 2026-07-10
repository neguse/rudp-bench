# ADR-0002: バトルの方法論

- Status: **Proposed**
- Date: 2026-07-10
- 依存: ADR-0001

## Decision

既にこの日までに合意・実践済みの方法を正式化する:

1. **対戦資格**: tune-to-plateau 済み実装のみ。チューニングは upstream
   公式ノブ/API の範囲、`--describe` の tuning に開示、`servers/<lib>/README.md`
   にソース行引用つき設計ノート(監査可能性)
2. **指標**: capacity = benchspec v2 の品質ゲート(delivery・staleness・
   deadline・attempted)内で維持できた最大 conns。ゲートの定義は変更しない
3. **基準線**: raw_udp 天井を同 regime × shape で取り、全数字を天井比で
   併記する。天井比 >100% は coalesce 系の正当な利得、100% 張り付きは
   ハーネス律速(≥N 読み)
4. **正直な開示**: censoring(farm_limited / measurement_invalid)は
   下限(≥N)として表に残す。破断原因と破断の形(緩やか/崖)も記録する
5. **運用**: 1 セッション = raw_udp アンカー 1 点 + 1 対象。開始前に
   時間とアウトプットを契約、超過は打ち切って調査。手順詳細と学びは
   docs/battle.md
6. **意思決定**: 目的・方法論・手順の変更は ADR の supersede でのみ行う。
   accepted な ADR の内容は議論を巻き返さない

## Consequences

- 「もっと速い設定がある」提案は、公式ノブ + 開示の枠内なら随時取り込める
  (新 ADR 不要)。枠そのものを変える提案(例: ライブラリ改造の解禁)は
  supersede が必要
