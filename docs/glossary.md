# 用語集

本プロジェクトの well-defined な用語の正引き。定義の正は各出典 ADR で、
ここは一覧と平易な言い換え。会話・doc とも、ここに無い専門語を
定義なしで使わない。

## 登場人物

| 用語 | 意味 |
|---|---|
| SUT | 測定対象のサーバ実装。System Under Test |
| treatment | 比較の単位。library 名ではなく「実装 + 設定 + build 条件の一式」を固定したもの。設定が違えば別 treatment([ADR-0002](adr/0002-benchmark-methodology.md)) |
| client farm | 負荷を作るクライアント群。測定器であって採点対象ではない。farm が先に限界に達したら測定は打ち切り扱い(下の CENSORED) |
| coordinator | campaign を回すローカルマシン([ADR-0005](adr/0005-reference-fleet.md)) |
| raw_udp | 素の UDP の物差し実装。環境と計測器の上限を示す基準役で、競技者ではない。再送を持たないため必達系の SLO は対象外(設計開示) |

## トラフィックと合格ライン

| 用語 | 意味 |
|---|---|
| LT(loss-tolerant) | 多少落ちてよく、最新値が新鮮なら良いトラフィック(位置同期など)。測るのは届いた率(delivery ratio)と鮮度(staleness) |
| MD(must-deliver) | 必ず届けるトラフィック(イベント通知など)。測るのは期限内到達率(deadline hit)と最終到達(eventual delivery) |
| staleness | 受信側から見た「手元の最新値がどれだけ古いか」 |
| SLO | 合格ライン。例: LT は delivery ≥ 0.95 かつ staleness p99 ≤ 300 ms([ADR-0004](adr/0004-reference-preset.md)) |
| capacity(境界) | SLO を満たせた最大接続数。境界の 1 点は測るたび ±1 点揺れる(解像度の内) |

## 測定条件

| 用語 | 意味 |
|---|---|
| scenario | 通信形状 3 種: environment-baseline(環境の校正。raw_udp 担当)/ authoritative-state(1 server 対 N client の状態配信)/ room-relay(room 内 N 人の相互配信)([ADR-0001](adr/0001-battle-purpose.md)) |
| regime | 回線条件。`lan` = 遅延 1 ms・ロス 0% / `wan` = 遅延 25 ms・ロス 1%([ADR-0004](adr/0004-reference-preset.md)) |
| cell | treatment × scenario × regime の 1 マス。測定表の 1 セル |

## 実験の単位と手順

| 用語 | 意味 |
|---|---|
| run | 1 回の測定(warmup → 測定窓 → drain) |
| sweep | 接続数を 2 倍刻みで上げて境界を探す一連の run |
| block | 統計の反復単位。1 block = 同一条件の sweep 1 回分で、fleet への配布・retry もこの単位([ADR-0002](adr/0002-benchmark-methodology.md)、[ADR-0005](adr/0005-reference-fleet.md)) |
| campaign | fleet を起動して block 群を回す 1 回分のセッション |
| screening | 粗い探索(2 倍刻み、反復 1 回)。あたりを付けるだけで推薦には使わない |
| confirmatory | screening で見つけた境界の ±10% を block 反復つきで確認する本測定 |
| pilot | 本測定のパラメータ(block 反復数、停止規則、drift 許容幅)を決めるための予備測定。pilot の値は比較表に混ぜない([ADR-0004 §3](adr/0004-reference-preset.md)) |
| pre-registration | 測る前に条件・打ち切り時刻・判定基準を文書に固定すること。後出しの変更はしない |

## 判定(outcome states — [ADR-0002](adr/0002-benchmark-methodology.md) の 6 状態)

| 状態 | 意味 | 読み方 |
|---|---|---|
| PASS | 計測が有効で、SLO を全て満たした | 合格 |
| FAIL | 計測が有効で、SUT が SLO を破った・切断した・crash した | SUT の実力。境界の根拠になる |
| INVALID | 環境・計測・設定の側に問題があり評価不能 | SUT のせいにしない。原則これだけ再試行できる |
| CENSORED | farm・rig・探索範囲の限界で SUT の境界まで到達できなかった | 「実力は ≥N」としか言えない |
| UNSUPPORTED | その treatment が必要な機能を提供しない(事前開示) | 欠席。不合格とは区別する |
| INCONCLUSIVE | 反復の上限まで測っても事前登録した精度に収束しなかった | 決着つかず。無理に数字を出さない |

## fleet 運用の門番

| 用語 | 意味 |
|---|---|
| doctor | ホストが測定に適するかの環境検査(時刻源、CPU 隔離など) |
| calibration | 計測器自体の校正 run |
| anchor probe | raw_udp 固定 1 条件の短い測定。環境が前回と同じかの見張り番 |
| boot gate | fleet 各ホストの起動時受入検査 = doctor + calibration + anchor probe。落ちたホストは使わない([ADR-0005](adr/0005-reference-fleet.md)) |
| fleet median gate | 各ホストの anchor 値が fleet 中央値 ±10% に収まるかの検査 |
| drift gate | block の前後に置く baseline run のずれ検査。許容幅を外れた block は INVALID |
| fleet fingerprint | 「同一構成の fleet は同じ値を出す」ことを検証済みの構成単位。fingerprint が違う環境の値は同じ表に混ぜない([ADR-0005](adr/0005-reference-fleet.md)、[ADR-0002](adr/0002-benchmark-methodology.md)) |
| Promotable | そのセッションの証跡を reference(公式値)へ昇格してよいか。doctor PASS が必須条件 |
| MDE | これ未満の差は「同等」とみなす最小の意味ある差 = 10%([ADR-0004](adr/0004-reference-preset.md)) |
