# ADR-0005: Reference fleet — virt c8g / fleet fingerprint / spot 実行 / 1h campaign

- Status: **Accepted**（2026-07-16 owner 承認。Open Decisions は A/A 実験後に凍結）
- Date: 2026-07-16
- 依存: ADR-0000, ADR-0002, ADR-0004
- Accept 時に supersede するもの:
  - ADR-0002「Reproducibility Record」の host fingerprint 集約規則（fleet
    fingerprint class へ読み替え）
  - ルート CLAUDE.md「rig 運用」の c8g.metal-24xl 指定と「confirmatory は
    on-demand」（2026-07-12 owner 表明）
- Decision owner: project owner

## Context

現行設計は「単一 c8g.metal-24xl を campaign 単位で保持し、rig-global lock で
全 run 直列」。ADR-0004 preset v1 の全体量（3 scenario cell × 2 regime ×
9 treatment、cell あたり screening + confirmatory 3 block + 前後 baseline）を
1 run ≈ 2 分で見積もると campaign は直列 12–15 時間（机上概算・pilot 未実測）。

owner 要求（2026-07-16）: **campaign wall-clock を 1 時間にする。**

ADR-0002 の「host fingerprint が違う cell は同じ comparison に集約しない」を
守る限り、1 つの比較（1 scenario×regime の全 treatment）は 1 ホスト直列 =
2–2.5 時間となり、何台並べても 1h に入らない。1h は treatment 単位の分散を
強制し、同規則の supersede が必須条件になる。

## Decision（2026-07-16 owner 合意）

### 1. rig は仮想化 c8g とし、metal は perf 診断専用へ降格する

- 根拠:
  - **target 一致**: production target（GameLift の c8g）は仮想化インスタンス。
    metal で測る方が target から遠い
  - metal 選定の動機だった clocksource 問題は x86（tsc/hpet）のもの。Graviton は
    virt でも `arch_sys_counter` で、固定周波数・SMT なしも共通
  - boot/調達が速く 1h 予算に収まる。campaign 単価も概算 1/3（未検証）
- 代償: noisy neighbor（LLC・メモリ帯域の他テナント干渉）が非制御分散として
  入る。これは A/A 実験（下記）と per-host anchor gate・前後 baseline drift
  gate で検出する
- PMU（perf ハードウェアカウンタ）は virt で使えないため、perf 深掘りは
  metal を単発で借りる**診断専用セッション**とする（診断 run は従来どおり
  capacity 主張から除外）

### 2. fleet fingerprint

- 定義: **同一 instance type + 同一 pinned stock AMI ID + 受入 gate PASS +
  anchor gate 内**の
  ホスト群を単一の fingerprint class とみなし、class 内の cross-host 集約を
  許可する。ADR-0002 の規則は「fleet fingerprint class が違う cell は同じ
  comparison に集約しない」へ読み替える
- 成立条件: A/A 実験で、ホスト間・時間帯間の分散が MDE 10%（ADR-0004 §2）より
  十分小さいこと。判定基準の提案値: **同一条件のホスト間 median の全幅が
  MDE/2 = 5% 以内**（境界 flap ±1 点と同オーダー）
- per-host gate: 各ホストは boot 後に raw_udp anchor を 1 点 probe し、fleet
  median から許容幅（A/A の観測分散から凍結）を外れたホストは全 block を
  aggregate から拒否する。現行「セッション冒頭 anchor」の fleet 版
- **サイズ混在の禁止**: instance size が違えば別 class。1 campaign は単一
  サイズで構成する

### 3. spot 実行（on-demand fallback なし）

- cell（treatment×scenario×regime）単位の work queue + 冪等 retry で配る。
  fleet fingerprint により代替ホストは定義上「同じ rig」なので、spot 中断の
  コストは validity ではなく wall-clock のみになる。これが confirmatory を
  spot に出せる根拠（従来 on-demand だったのは、単一ホスト campaign では
  中断 = fingerprint 喪失 = campaign 廃棄だったため）
- 中断された block は丸ごと捨てて別ホストで再実行する（部分 block の救済は
  しない — ADR-0002「中断 run は捨てる」の原則のまま）。coordinator への
  回収完了 + gate PASS の block のみ有効
- IMDS の interruption notice（2 分前）で実行中 run を interrupted とマークし
  cell を requeue する。retry は spot のまま、campaign の打ち切り時刻
  （ADR-0002 pre-registration の「予定時間・打ち切り条件」）まで繰り返す
- **on-demand fallback は置かない**（2026-07-16 owner 決定）。打ち切りまでに
  完了しなかった cell は未取得として記録し、campaign の穴とする。中断が
  多くて回りきらないのは購入形態で吸収する事象ではなく、**打ち切って原因を
  調査する事象**（AZ・時間帯・サイズ・fleet 構成の見直し）。campaign 単価が
  安いため、穴が多ければ campaign ごと再実行する方が単純
- 多様化は AZ 方向のみ（AZ が fingerprint に効かないことは A/A に混ぜて
  確認する）。campaign 記録に中断率を残す

### 4. 1h campaign protocol

campaign を次の phase で回す。1h は typical 目標（hard SLA ではない）。
打ち切り時刻は campaign ごとに pre-registration で固定し、超えたら未取得
cell を残したまま終了して原因を調査する。

| phase | 内容 | 予算 |
|---|---|---|
| 0 | pinned stock AMI から fleet（20–30 台想定）boot、cloud-init + 入力 bundle pull、per-host doctor + anchor gate | ~10 分 |
| 1 | screening: single-run connection ramp を cell ごとに並列 | ~15 分 |
| 2 | confirmatory: 境界 ±10%、N=3 block、前後 baseline | ~20 分 |
| 3 | coordinator が bundle を逐次回収済み → fleet gate 判定、集約、PR 作成 | ~10 分 |

- screening は 2 倍刻み探索を single-run ramp（CI bench に実装済み）で置換
  する。**昇格前に ramp と 2 倍刻みが同じ境界を出すことの検証を要する**
- pre-registration・outcome states・stopping rule は ADR-0002/0004 のまま。
  本 ADR は実行基盤のみを変える

### 5. 配布と回収は GitHub、AWS は ephemeral compute のみ（2026-07-16 owner 決定）

golden AMI は**作らない**。恒久資産は GitHub に置き、AWS 側には campaign 中の
EC2 以外の永続資産（AMI、S3、IAM role）を持たない。

- **入力**: 公式 Ubuntu 24.04 arm64 stock AMI を **ID pin**（fingerprint の
  構成要素）。実行物は CI の arm64 job（public リポのため無料）が Release で
  ビルドした self-contained bundle（.NET は self-contained publish、Go は
  static、C/C++ は同一 distro ビルド）を **GitHub Release asset** へ push し、
  fleet は cloud-init で無認証 curl する
- **fleet ホストは credential を一切持たない**（GitHub token も AWS role も
  なし）。結果 bundle はホストローカルに置き、coordinator（campaign を回す
  ローカルマシン、SSH 鍵保持）が block 完了ごとに逐次 scp 回収する
  （campaign 末尾一括だと spot 中断でロストするため）
- **結果**: coordinator が手元の gh 認証で campaign 結果を **PR として提出**
  する。PR には `docs/measurements/<campaign>.md` + compact な機械可読
  aggregate（判定・capacity 表・gate 結果）のみ。生 run bundle の tar 一式は
  campaign タグの **Release asset** に置き、ADR-0002 の再現性記録はそちらで
  満たす（git 履歴を肥大させない）
- 副次効果: campaign 結果の landing が PR レビューになり、aggregate への
  混入可否（gate 判定・INVALID の扱い）を merge 前に人間が確認する関門が
  自然にできる
- **IaC（Terraform 等）は使わない**（2026-07-16 owner 合意）。管理対象が
  campaign 中の約 1h しか生きず、spot 中断の動的対応は coordinator のループの
  仕事で宣言的 apply と噛み合わないため。安全網は state ではなく tag:
  全リソースに `Project=rudp-bench, Campaign=<id>` を付け、coordinator の
  `cleanup` が tag 検索で orphan を含めて全削除する。恒久 substrate を持つ
  決断をしたときに限り、その部分だけ IaC 化を再検討する

## 具体手順

### A/A 実験（fleet fingerprint の立証）

1. 候補サイズ: c8g.8xlarge / c8g.16xlarge（+ 参照点として metal 1 台を含め、
   virt バイアスの大きさ自体を記録する）
2. 各サイズ N=5 ホスト × 2 treatment（raw_udp + managed 代表 1 つ）×
   3 block、時間帯を変えて 2 セッション（neighbor noise の時間依存を見る）
3. 判定: ホスト間 median 全幅 5% 以内で PASS。**PASS する最小サイズを採用**。
   ADR-0004 の farm gate が破れるサイズは下限から除外
4. A/A 自体は spot 可（中断は捨てて再実行）
5. 結果から凍結するもの: fleet の instance size、anchor gate 許容幅、
   drift 許容幅（ADR-0004 §3 の pilot 凍結値と統合）
6. **A/A は platform（instance size・region）につき一度の立証実験**であり、
   campaign ごとには再実行しない。以降の常設 gate は boot 時 per-host anchor
   probe と前後 baseline drift のみ。再実験が要るのは instance size・世代・
   region の変更時、または anchor 乖離が campaign をまたいで継続し A/A の
   前提を疑うとき。入力 bundle や stock AMI の pin 更新は platform ノイズに
   効かないため再 A/A 不要（campaign 内は単一 AMI + 単一 bundle で、乖離は
   anchor gate が検出する）

### enabler の実装順

1. **CI arm64 bundle job**: 全ビルド成果物（self-contained）を
   `bundle-<commit>.tar.zst` として Release asset へ push
2. **cloud-init + boot 時 gate**: stock AMI 上で最小 setup（CPU 隔離、nofile、
   数点の apt）→ bundle pull → doctor + anchor + calibration
3. **coordinator**: fleet 起動 → SSH で cell 配布 → 逐次 scp 回収 →
   interruption handler → 集約 → PR 作成。Batch / Step Functions は使わない。
   SSM は使わない(instance profile = IAM credential が必要になり、
   zero-credential 方針と矛盾する — 2026-07-16 修正)。配布・回収とも
   per-campaign の SSH key pair + coordinator IP のみ許す SG で行う
4. **ramp screening の等価性検証**（A/A と同一 fleet 上で実施可）
5. **A/A campaign** → 判定 → 本 ADR の Open Decisions を凍結

## Open Decisions（A/A 後に凍結）

**凍結済み（2026-07-19 owner 承認。証跡:
[A/A campaign](../measurements/2026-07-18-aa-session1-8xlarge.md)）**

- **fleet instance size = c8g.16xlarge**。A/A 全セッションで両 treatment が
  ホスト間全幅 ≤5% を満たす最小サイズ（raw_udp 1.3–1.4% / magiconion
  1.3–3.9%）。8xlarge は raw_udp 単独では PASS(0.7–2.8%) するが、managed
  treatment は farm コア余裕不足で ~6%（ledger #26）のため除外。
  raw_udp のみの campaign には 8xlarge を使ってよい
- **anchor gate = fleet median ±10%（+ probe PASS 必須）**。probe は
  raw_udp ref-room-lan c128（45 run で全幅 5.3% = ヒストグラム 1 bin。
  ±10% は bin 量子化 2 個分）。boot.sh が probe を実行し、coordinator の
  aggregate が判定する（実装・実 rejection とも検証済み）
- 台数は campaign ごとの pre-registration で指定する（A/A は 4–5 台で実証。
  spot vCPU quota 300 の現状では 16xlarge ≤4 台、増枠申請 512 は審査中）
- drift gate（block 前後 baseline）の許容幅は従来どおり ADR-0004 §3 の
  pilot で凍結する（本 ADR の凍結対象から変更なし）

## Consequences

- `docs/reference-rig.md` は全面改訂（調達条件を virt fleet へ、受入
  チェックリストを「AMI 焼き込み時」と「boot 時 gate」に分割）
- ルート CLAUDE.md「rig 運用」の該当決定を本 ADR 参照へ更新（accept 後）
- ADR-0004 の pilot はこの fleet 上で実行し、block 所要時間の実測で本 ADR の
  phase 予算（机上値）を検証する
- コスト概算（未検証）: virt fleet spot で campaign $20–30。月予算 5 万円で
  campaign を反復可能にし、「気軽に回せる」ことを前提とした運用に変わる
