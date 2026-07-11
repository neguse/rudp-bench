# ADR-0000: プロジェクトの目的と成功条件

- Status: **Accepted**
- Date: 2026-07-10
- Decision owner: project owner
- Scope: 最上位。この決定を変更する場合は新しい ADR で supersede する

## Context

これまでの実験は transport 間の capacity 比較を先行させたため、個々の
workload、回線条件、品質 gate が何の判断に必要なのかを一貫して説明できて
いなかった。固定された対戦表だけでは、利用者が自分の workload に結果を
対応付けることも、自分の環境で再現することも難しい。

本 ADR は「何を測るか」より上位にある、プロジェクトの利用者、利用目的、
成果物、成功条件を定める。具体的な reference scenario と測定方法は、ここから
導出する別 ADR で定める。

## Decision

本プロジェクトの目的は、**リアルタイム通信を必要とする人が、自身の
workload に近いシチュエーションを選び、自身の環境で再現可能な実験を行い、
実測に基づいて技術選択できるようにすること**である。

成果は次の 3 層で提供する。

### 1. 再現可能な実験基盤

- 利用者は reference scenario を起点に、自身の workload と実行環境へ寄せた
  scenario を明示的な設定として作れる
- scenario は少なくとも通信 topology、traffic、接続規模、品質要求、回線条件、
  resource 条件を表現できる
- 実行結果には scenario、実装と tuning、実行環境、計測器の版、生データを残し、
  同じ条件で再実行できる
- quick check、校正、本測定を分離し、利用者が結果の有効性を確認できる

### 2. 典型的なシチュエーションの比較データ

- project owner が「よくある」と判断した複数の real-time use case を、根拠と
  限界を明記した reference scenario として用意する
- reference scenario ごとに複数の既存 transport / solution を測定し、
  「この条件なら何を選ぶか」をすぐ判断できる decision table を提供する
- 単一の総合順位ではなく、workload、品質要求、回線、運用制約を条件とした
  推奨を示す

### 3. 技術的知見

- capacity の大小だけでなく、破断原因、遅延と損失への応答、CPU・memory・
  network overhead、運用上の制約を分析する
- 既存 solution で要求を満たせる範囲と、要求を満たせない理由を明らかにする
- その証拠から、既存 solution の採用・組み合わせ・追加実装・一からの自作の
  どれが妥当かを判断できるようにする

## Success Criteria

1. 利用者が reference scenario を選択し、対応する設定の内容を確認できる
2. 利用者が設定を変更し、対応環境上で doctor、校正、短時間試験、本測定を
   文書化された手順で実行できる
3. 公開する数値には、実行条件、生データ、反復結果、不確かさ、無効・打ち切りの
   理由が付随する
4. 各推奨には「どの条件と証拠から、その選択になるか」と「適用できない条件」が
   付随する
5. 既存 solution と自作の判断は、性能だけでなく、要件充足、変更可能性、実装・
   保守コスト、相互運用性、security、運用リスクを含む

## Non-goals

- あらゆる用途に通用する単一の最速ランキングを作ること
- benchmark 結果だけで production 環境の性能を保証すること
- 内部設計の異なる solution を、同一実装方式へ強制して比較すること
- 既存 solution より benchmark の数値が高いという理由だけで、自作を正当化すること
- project owner の reference scenario を、全利用者に共通する唯一の典型と主張すること

## Consequences

- reference scenario は固定された試験だけでなく、利用者が変更できる template とする
- 測定対象は「library 単体」に限定せず、利用者が採用する idiomatic な full solution
  とし、実装・tuning・運用条件を開示する
- 現在の broadcast-only battle 表は本目的を満たす最終成果ではなく、過去条件に
  対する参考実験として扱う
- ADR-0001 以降の目的、方法、matrix は本 ADR から改めて導出し、合意する
- reference scenario の選定と推奨ルールは、測定開始前に別途 Accepted にする
