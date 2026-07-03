# Workload 定義 — 負荷平面と archetype anchor

- 状態: draft(E2 着手前に凍結)
- 方針: workload はシチュエーション名で固定せず、**構造を categorical に固定し
  負荷2軸を sweep する平面**として定義する(netem を「mobile」でなく loss%×burst
  平面にしたのと同型の現象パラメータ化)。実在ユースケースは平面上の
  **anchor(注記点)**であり、測定単位ではない。archetype の数値が動いても
  (プラットフォーム更新等)、swept 範囲内なら注記の移動だけで再測定不要
- 主張は常にセル座標(rate, payload)条件付きで行う。anchor 名は読解補助ラベル
- anchor パラメータと鮮度予算は tuned-disclosed と同じ「外部参照必須」規律に従う

## 構造(固定部 — sweep しない)

- distribution: **対称 broadcast fanout**(全 conn が送信し、server が全接続へ
  payload 無変更で fanout)。relay 意味論に native な archetype(VRChat = Photon
  relay、SFU forward)を代表構造とする
- must-deliver: **1 Hz × 128 B broadcast** 固定(ownership 移譲・周期 resync・
  manual sync イベントの proxy。Rec Room 公式 eng blog の master-action broadcast +
  周期 snapshot resync パターン)。イベント駆動 reliable の固定周期 proxy である
  ことを開示
- loss-tolerant が複数の実在流(voice + pose 等)の合成である場合、pps と
  bytes/s を保存した単一流に合成する(内訳は anchor の根拠表に示す)

## 負荷平面(loss-tolerant class の2軸)

| 軸 | 値 | 律速の対応 |
|---|---|---|
| rate | **{10, 20, 60} pps** | packet 処理・syscall 律速を張る |
| payload | **{128, 200, 1000} B** | 帯域・copy 律速を張る |

セル名は `r{pps}p{bytes}`(例: r20p128)。3×3 = 9 セル。
pps 律速か bytes 律速かが平面の capacity 勾配の向きから読める。

**スライス規律**(full factorial は回さない):

| スライス | 対象セル |
|---|---|
| capacity @ wired | **全 9 セル** + synthetic echo 2 本 |
| capacity @ loss 最悪点(3%×burst16) | **anchor 3 セルのみ** |
| boundary(loss 平面 3×3 × 負荷アンカー) | **vr / video の 2 anchor のみ** |

**平面セルの capacity gate(archetype 非依存)**: validity gates +
delivery ≥ 0.95 × floor + **staleness p99 ≤ floor + 1 送信間隔**(latest-value 流が
「次の update 1 回分」以内の鮮度を保つ、rate に自己スケールする規則)。
archetype の絶対予算(下表)は anchor セルに対する**分析時の追加判定**であり、
run を増やさない(latency は固定 bin ヒストグラムで記録され再判定可能)。

## Anchor 1: br_fanout = **r20p128** — 大人数 BR state fanout(relay 近似)

大 room × 低 tick は実在パターン(100 人級は 20-30Hz。競技 FPS 64-128Hz は
room 10-12 とセットで別物)。実物の BR server は状態を集約して delta 圧縮する —
無集約 relay fanout は **interest management も集約もしない server の上限**として
読む(Apex 実測 60kB/s vs fanout 換算 154kB/s ≈ 2.5 倍)。

| param | 値 | 根拠 |
|---|---|---|
| rate 20 pps | Apex Legends 20Hz(Respawn 公式 deep-dive, 2021)、Fortnite 20→30Hz(二次)、PlanetSide2 大規模戦 10-20Hz | |
| payload 128 B | delta 圧縮 update 相当: Gaffer On Games 28-40B/entity × 数 entity、ShenZhou MMORPG 実測 ~100B/packet(peer-reviewed)、Photon 公式 perf 例 ~200B | |
| room 主張範囲 〜128 | 実在: 60(Apex)/ 100(Fortnite・PUBG)/ 128(BF2042)。超過は stress ceiling 表記 | |
| staleness 予算 **100 ms** / D **100 ms** | Source engine `cl_interp` 既定 100ms と知覚研究(CHI PLAY 2021: 100ms acceptable)が同値に収束 | |

## Anchor 2: vr_room = **r60p200** — social VR(pose + voice 対称 fanout)

VRChat の実構成(movement / voice / Udon networking がすべて Photon realtime 層の
relay 経由)と送信側まで含めて構造一致。loss-tolerant は合成流(内訳):

| 成分 | rate × payload | 根拠 |
|---|---|---|
| voice(Opus 20ms) | 50 pps × 160 B | Opus RTP 標準 20ms = 50pps(Xiph 公式推奨 / RFC7587)。~64kbps は Discord 既定と一致。VRChat は voice も Photon 層で relay |
| pose/IK + params | 10 Hz × ~310 B | 10Hz は VRChat 公式固定周期(IK・params とも。face tracking でも上がらない)。310B = IK2.0 の 10 点(公式)× 28B(Vector3 12B + Quaternion 16B、公式型表)+ synced params 256bit = 32B(公式上限。face+finger で 210-250bit 飽和の creator 実報告)。IK チャネル byte は非公開 — **型表からの導出値**と開示 |

合成 60pps・加重平均 185B → **r60p200** に配置(voice/pose の bimodal を uniform
近似。いずれも MTU から遠く transport 挙動差は小)。

| param | 値 | 根拠 |
|---|---|---|
| room 主張範囲 〜80 | VRChat platform hard cap 80(soft cap 40)。BigScreen 12、Horizon Worlds は 2025 更新で 100+(トレンド注記) | |
| staleness 予算 **150 ms** / D **150 ms** | ITU-T G.114(会話音声 one-way 150ms — 支配項が voice なので本線)+ avatar 知覚研究(操作 ~100ms・LPT 130-170ms)。interpolation/jitter buffer 前提の latest-value 消費(VRChat / Meta Avatars SDK 明文) | |

開示: (1) 全員発話・全員リッチトラッキングの saturated worst case(distance culling
なし・change-driven 抑制なし。実測 typical 総帯域 = VRChat 逸話値 50-300MB/h より
約1桁上の上限モデル)。(2) world object 同期(Udon 予算 ~11kB/s)は world 単位で
conn 数にスケールしないため除外。(3) ChilloutVR は param 予算 3200bit(低確度)—
richer tier の存在として注記。

## Anchor 3: video_room = **r20p1000** — 対称ビデオ通話(gallery / 画面付きボイチャ)

全 conn が映像 packet を送り全員へ fanout(SFU forward の対称形)。

| param | 値 | 根拠 |
|---|---|---|
| rate 20 pps | gallery の per-tile 実効 bitrate は simulcast 低層 ~150-400kbps(LiveKit 例示 320×180@150kbps / 360p 190-400kbps、Zoom gallery 実測 2Mbps÷25tile ≈ 80kbps/tile)。20pps×1000B = 160kbps はこの帯の中。fps ではなく pps(1 frame は複数 RTP packet に分割) | |
| payload 1000 B | WebRTC の RTP packet 上限 1200B(libwebrtc)から transport ヘッダ余地を引いた MTU 律速値 | |
| room 主張範囲 〜49 | 対称 full-gallery の実在上限: Zoom 25/49 tiles、Meet 49(7×7)、Jitsi 推奨 <50。**49 超は全実在系が active-speaker/Last-N に切替わる**ため超過は stress ceiling 表記 | |
| staleness 予算 **150 ms** / D **1 s** | G.114 one-way 150ms + WebRTC NetEQ jitter buffer 実務 80-200ms。D は control 面(RTCP 最小間隔 5s・signaling)の運用許容 — 知覚根拠でないと明記 | |

開示: (1) 実 SFU は音声を top-2〜3 speaker のみ選択転送(Jitsi Last-N 一次資料)—
音声は含めない(映像のみ)。(2) 実系は N 増で per-tile bitrate を下げ downlink を
有界に保つ — 固定 bitrate fanout は「層切替なし上限」。(3) NACK/RTX 修復層
(RFC4585/4588)は含めない — 修復層が埋める穴は staleness に現れる(それを測る)。
(4) control の実態は echo(非 fanout)だが平面の md は broadcast 固定 — 1Hz×128B
の差は微小、上限側の近似と開示。

## echo / reliable_echo — synthetic baseline(平面外)

実在 workload の主張をしない合成ベースライン。役割: transport 単体の応答特性の
floor(reliable_echo)と mixed 干渉の検出(echo)。レポートでは「synthetic」と
明記しユースケース名で語らない。

| param | 値 | 位置づけ |
|---|---|---|
| rate / payload | 50 Hz / 64 B | v1 から据え置き(比較連続性のみが根拠) |
| staleness 予算 / D | 100 ms / 100 ms | br_fanout と同値流用(独立根拠なし、gate 用の便宜) |

## 非目標(後続プール)

- 非対称 fanout(少数 publisher × 多数 subscriber、webinar/配信型)— 根拠は収集済み
  (mediasoup ~500 consumers/worker、LiveKit 5pub×500sub)だが対称平面で fanout 軸は
  張れているため後続
- 音声 Last-N 選択転送の明示モデル — relay 意味論で表現できない
- 平面の局所細分(anchor 近傍で capacity 勾配が急な場合のみ、boundary の等高線
  細分と同じ逐次実験ルールで追加)

## 出典(主要)

- VRChat creators docs — animator-parameters(10Hz・256bit・型別 wire size)、
  network-details(continuous sync ~200B、11KB/s)、IK 2.0 blog(10 点)
- VRChat wiki — instance cap 80 / Photon relay 構成(movement/voice/Udon)
- VRCFaceTracking / vrchat-community OSC issue #163 — face+finger の bit 実消費
- Rec Room eng blog(tyleo)— master-action broadcast + 周期 snapshot resync
- Meta Avatars SDK docs — StreamLOD、interpolation/jitter buffer 前提の pose stream
- PubMed 40773409 / CHI PLAY 2021 — VR/FPS の知覚許容(75-170ms / 100ms)
- Respawn, "Servers and Netcode Developer Deep-Dive" (2021) — Apex 20Hz・60kB/s
- Riot Games, "Valorant's 128-tick servers" (2020) — 競技系 128Hz×10 人(対比用)
- Valve Developer Wiki, Source Multiplayer Networking — cl_interp 100ms
- Gaffer On Games, "State Synchronization" — 28-40B/entity、playout delay 4-5 frame
- Chen et al., "Game Traffic Analysis: An MMORPG Perspective" (2005) — ~100B packet
- Photon docs/forum — 500 msg/s/room 予算、perf 例 4×10Hz×~200B
- Zoom 公式 bandwidth docs — gallery 25: 2Mbps / 49: 4Mbps、tile cap 25/49
- Google Workspace blog — Meet 49-tile、超過で active-speaker 切替
- Chang et al., IMC'21 "Can You See Me Now?" — Zoom/Webex/Meet 実測 bitrate
- Jitsi, NOSSDAV'15 Last-N paper — 音声含む top-N 選択転送(一次資料)
- LiveKit bitrate guide / docs — simulcast 層例(180p@150kbps)、解像度別 bitrate
- libwebrtc discuss-webrtc — RTP max 1200B
- RFC 3550 / 4585 / 4588 / 7587 — RTCP 5% / NACK/RTX / Opus 20ms=50pps
- ITU-T G.114 — one-way 150ms / 400ms 上限
- Discord docs / eng blog — Opus 20ms=50pps、SFU 単一 host、Go Live 50 viewers
