# E3 確認実験 — ブロック反復と CI 付き確定値

- 状態: 計画(実行には AWS アカウント・予算の承認が必要 — 「要ユーザー判断」参照)
- 前提: E2 完了(全セルに draft 値 or farm 下限、帰属3分類・隔離・farm 凍結済み)

## 目的

design spec の E3 完了条件「CI 付き曲線、unknown = 0」を満たす:

1. **N ≥ 3 の独立ブロック**(1 インスタンス = 1 ブロック = 全 transport の完全な1周)
   で中央値 + IQR / bootstrap CI を付ける
2. **cross-rig 一致**(home と aws-metal で順位一致)を校正項目として実施
3. anchor セルの **library-default 併記**(本命: gns SendRateMax、litenetlib UpdateTime)
4. farm 下限セルを metal の潤沢なコアで上に伸ばす

## ブロックの定義(1 インスタンスで実行する一周)

```
bootstrap → isolate setup → 校正(null/fault_inject/duration 不変性)
→ sweep wired(全 11 workload)→ sweep loss-worst(anchor 3)
→ boundary(loss 平面 × anchor 2 × 負荷 3)→ 結果 tar を回収
```

- seed はブロックごとに変える(実行順のランダム化はブロック内で有効)
- 負荷アンカーの conns は**そのブロックの capacity@wired から導出**
  (ブロック内で自己完結 — ブロック間で conns が違ってよい。比較は
  within-block paired が原則)
- 所要見込み: wired ~1.5h + loss-worst ~0.5h + boundary ~3h ≈ **5h/ブロック**
  (home 実測ベース。metal では短縮見込み)

## rig 差分(実装が必要なもの)

| 項目 | 現状 | E3 で必要 |
|---|---|---|
| CPU レイアウト | isolate.go / config に 5750GE(16 論理)決め打ち | rig 記述ファイル(os/client/server の cpus)で外出し。metal ではコア数に合わせ farm を拡張(farm 下限セルの解消) |
| ブロック runner | sweep/boundary を手で順に起動 | 1 コマンド化(校正→3 sweep→tar)+ ブロック metadata(rig, seed, commit) |
| 集約 | 単一ブロックの report のみ | `orchestrator aggregate`: ブロック横断の median/IQR/bootstrap CI、within-block 順位一致検定、report マーカー出力 |
| 非 metal fallback | — | /proc/stat steal delta を validity gate に追加(spec 記載済み) |

## dispatch 計画

1. AMI 化はしない(初回は cloud-init / bootstrap script で十分。AMI 化は反復が
   定着してから)
2. 5 インスタンス並列 × 1 ブロックずつ。spot は使わない(ブロック途中の中断は
   ブロックごと無効 — resume はあるが独立性の説明が濁る)
3. 回収: results-v2/block-<id>.tar.gz を S3 or scp → home で aggregate

## 概算コスト(要実勢確認)

- 候補: c6i.metal(128 vCPU)~$5.4/h 級、または c7a.metal-48xl ~$10/h 級。
  ベンチは 1 server コア + farm 数コアしか使わないため **metal の最小格で十分**
- 5 ブロック × ~6h × $5.4/h ≈ **$160 前後**(+転送・保存は誤差)
- 節約案: 3 ブロックなら ~$100。まず 1 ブロックで cross-rig 一致を確認してから
  残りを流すのが安全(逐次でも wall-clock 1 日で収まる)

## 要ユーザー判断

1. AWS アカウント・リージョン・予算上限(上の概算で GO/NO-GO)
2. インスタンス型(推奨: 最小格の x86 metal。Graviton は .NET/msquic 未検証のため今回は避ける)
3. ブロック数(推奨: まず 1 で cross-rig 確認 → 追加 4)

## 先行して home でできること(実施中/済)

- [x] boundary の隔離+新 farm での取り直し(実行中 — home ブロック #0 の一部を兼ねる)
- [ ] ブロック runner コマンド化(rig 非依存に書けるので今実装可能)
- [ ] aggregate 実装(合成 fixture でテスト可能)
- [ ] rig 記述ファイル化(home.json を書き、isolate/config が読む形に)
