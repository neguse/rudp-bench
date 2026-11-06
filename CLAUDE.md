# CLAUDE.md

## 目的と方法(2026-11-06 owner 決定)

- 目的・「使える」の条件・劣悪ベンチの条件と判定基準・SUT の作りは [README.md](README.md) が正。決定を変えるときは README を書き換える。履歴は git に任せ、ADR は書かない。
- 判定基準は測る前に README に書いてコミットする。測定記録(`docs/measurements/`、日付つき)には測ったときの commit を書く。結果を見てから基準を動かさない。
- 結果が大きく割れる問いだけを扱う。不合格はライブラリのソースの該当行で仕組みを説明するまでを 1 つの結果とする。数 % の差を詰めるための仕組み(反復の統計、環境の一致の保証)は作らない。
- 以前の目的のための ADR・記録・コードは `archive/before-reset` ブランチにある。参照はしてよいが、master に戻さない。

## 実行環境(2026-11-05 owner 指示)

- smolcenter(現 PC)はゲームサーバと同居している。ビルドも測定も `orchestrator/rigs/home.json` の `bench_cpus` に限った bench.slice の中で動かし、`os_cpus` を使わない。

## 会話(2026-07-23 owner 指示、2026-11-06 改定)

- 会話と文書は平易な日本語で書く。造語しない。一般に通じる語で書き、定義が要る語は README の「用語」に書く。比喩で仕組みを説明しない。
- owner が全体の方向を話しているときに、実装の細部に入らない。
- Codex への委任はしない。調査・実装とも自分で行う。

## 削除予定のコード

v1(`harness/`、`adapters/`、`cmd/`、`internal/`、`scenarios/`、`tests/`、`coop_rudp/`)と、以前の目的の大規模な測定のための仕組み(orchestrator の該当部分、`scripts/fleet` など)は削除予定。手を入れない。
