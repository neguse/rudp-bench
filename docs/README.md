# docs — 構成と更新規律

このディレクトリは「live docs」と「decision log」に分かれる。

- **live docs** — 現状を記述する。実装・運用が変わったら同じ変更で更新し、
  常に repo の実態と一致させる。
- **decision log** — 日付つきの決定と記録。追記専用で、後から書き換えない。
  決定を変えるときは新しい ADR で supersede する。記録は古くなっても
  「当時の記録」として正しいままにする。

## Live docs

| doc | 内容 |
|---|---|
| [../README.md](../README.md) | v2 の入口: build、測定 workflow、現在の status |
| [battle.md](battle.md) | wired バトルのハブ(表は参考実験として凍結、手順と学びは現役) |
| [ledger.md](ledger.md) | 異常・疑問・改善候補の記帳所 |
| [reference-rig.md](reference-rig.md) | reference rig(EC2 c8g)の調達条件と受入手順 |
| [sentinel.md](sentinel.md) | 回帰検知(sentinel)の運用 |
| [provisional-loss-smoke.md](provisional-loss-smoke.md) | loss pipeline smoke の固定条件と手順 |
| [profiles.md](profiles.md) | workload セル語彙の定義と anchor 根拠 |
| [checklist.md](checklist.md) | 通信ライブラリ実装・ベンチのチェックリスト(時期非依存の知見) |
| [measurements/current.md](measurements/current.md) | 現在の reference 測定を指す唯一のポインタ |

このほか [`../benchspec/README.md`](../benchspec/README.md)(wire/metrics 契約)と
`../servers/<lib>/README.md`(各実装の設計ノート)も live。

## Decision log

| 置き場 | 内容 |
|---|---|
| [adr/](adr/) | 意思決定(status つき。変更は新 ADR で supersede) |
| [log/](log/) | 日付つきの設計 spec・実装 plan・レビュー・開発ノート・v1 運用記録 |
| [measurements/](measurements/) | 日付つき測定記録(`current.md` 以外は変更しない) |

記録には当時のパスや構成がそのまま残る(例: `log/` 内の旧 `docs/superpowers/` 参照)。
記録内のコード参照・数値は記載時点のもの — 現状の根拠には live docs とコードを使う。
