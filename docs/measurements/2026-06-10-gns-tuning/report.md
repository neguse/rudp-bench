# gns 徹底チューニング — デバッグ計装ビルドと既定レートリミッタが正体だった

**測定日:** 2026-06-10
**目的:** 2026-06-09 canonical で gns が全 profile 最弱級(media_relay last_ok=5、全 profile で server CPU 160-190%)だった原因を特定し、adapter / ビルド構成 / 設定で改善する。

## セットアップ

- canonical 手順そのまま: 隔離 server=7,15 / client=5,6,13,14 procs=4、netem 25/5/1/100000、idle=adaptive、N=3 中央値、gate=delivery 0.95。
- gns のみで canonical conn スケジュールを全 profile 掃引(`results/gns_cert_canonical_n3`)。
- 診断は gprofng(server: game_server@64、client: media_relay@50 相当を手動再現)。

## 根本原因(3つ + 副次2つ)

### ① ビルドが Valve の retail 構成ではなかった(最大要因)

upstream GNS は `_CERT` 未定義だと **Release(-O3 -DNDEBUG)でも `DBGFLAG_ASSERT` を定義**し、
それが `STEAMNETWORKINGSOCKETS_LOCK_DEBUG_LEVEL=1` を連鎖的に有効化する。結果:

- 全ロック取得/解放で `Plat_USTime`(clock_gettime)を記録 — media_relay@50 の **client CPU の 64% が clock_gettime** だった(gprofng 実測)。
- `SSNPSenderState::DebugCheckReliable`(送信キュー O(n) 走査)等のデバッグ assert も計測対象に混入。

`adapters/gns/CMakeLists.txt` で `GameNetworkingSockets_s` に `_CERT` を定義(出荷相当)。
netem 無し media@50 相当で server CPU 154%→81%、client 合計 400%(飽和)→328%。
**これまでの全 gns 計測値はデバッグ計装込みの数字だった。**

### ② SendRateMax 既定 256KB/s が unreliable も絞る

GNS のトークンバケットは unreliable を含む全送信に効き、既定 `SendRateMax=256KB/s`。
media_relay@50 はサーバー片 conn あたり 50×30Hz×1000B=1.5MB/s 必要 → 観測 delivery 0.153 ≒ 256KB/1.5MB。
**公開値の media break@50 はワイヤでも CPU でもなく既定設定の壁。**
`SendRateMin/Max=0x10000000`(設定上限 256MB/s)に引き上げ。SendBufferSize 512KB→32MB、
RecvBufferSize/Messages も増強(c50: 0.153→0.715)。

### ③ NoNagle がフルファンアウトで syscall 律速

gprofng で server CPU の **64% が sendmsg**(パケット毎 1 syscall)。game_server@96 は
96×21×96≒190k msg/s が全て個別パケット化していた。GNS の MTU は 1300 固定(プロトコル定数、
変更=fork なので不採用)だが、**Nagle 5ms で同一 conn 宛 ~10 msg が 1 パケットに合体**する。
adapter 既定を Nagle ON に変更(game_server@64: CPU 151%→87%、media@50: 0.59→0.93)。
RTT p95 は ~100-120ms で gate 無し・許容範囲。

### ④ 暗号化を他 adapter と条件統一(副次)

他 adapter は全て平文。`Unencrypted=3`(両端で設定)にして条件を揃えた。AES-NI のおかげで
CPU 影響は小さかったが、`encryption_on()`/capabilities も off に更新。暗号化込みの計測用に
`gns_encrypted` バリアントを登録。

### ⑤ socket buffer 256KB→4MB(gns のみ L17 から逸脱、根拠付き)

gns は全 conn が 1 socket を共有し、送信は単一サービススレッド。media@50 の ~33k pkt/s では
256KB SO_SNDBUF ≒ 128 in-flight skb に対し netem 25ms の BDP ≒ 1100 packets で ENOBUFS ドロップ
(プロトコル損失ではなくバッファアーティファクト)。4MB(ホストの rmem_max/wmem_max 上限)で
c50 0.9389→0.9584、他 profile に regression 無し(N=2 A/B)。enet/kcp の「バッファ無効/有害」結論
(2026-05-31)は per-conn ARQ の話で、単一 socket 共有 + 高 BDP の gns には当てはまらない。
`g_cbUDPSocketBufferSize` は public config が無いため static link 前提で extern 設定
(SNP の packets-per-think 上限 `size>>11` も 128→2048 になる)。

### その他

- `send_many` を `AllocateMessage`+`SendMessages` バッチで実装(conn 毎にロック/タイムスタンプ/think が 1 回に集約)。
- バリアント整理: `gns`(tuned 既定)/ `gns_encrypted` / `gns_no_nagle` / `gns_smallbuf` / `gns_split_lanes`。旧 `gns_nagle`/`gns_split_*` は削除。

## 結果(gns のみ、canonical 手順 N=3)

`results/gns_cert_canonical_n3`(2026-06-10):

| profile | 公開値 6/9 last_ok | 今回 last_ok | delivery | server CPU | break |
|---|---:|---:|---:|---:|---|
| media_relay | 5 | **50** | 0.9671 | 118% | 75 (delivery 0.17) |
| game_server | 64 | **96** | 0.9748 | 95% | 128 (delivery 0.88) |
| echo | 600 | **1500** | 0.9900 | 172% | 2000 (client_tick) |
| reliable_echo | 1500 | **2000** | 1.0000 | 154% | 3000 (client_tick) |

公開値(6/9 全 lib)との相対位置:

- media_relay 50: enet/kcp/mini_rudp/raw_udp/litenetlib(50)と同列。coop 100 / apex 150 が上。**最弱(5)を脱却。**
- game_server 96: enet/coop/kcp/raknet(64)を上回り、litenetlib(128)/apex(256)に次ぐ。
- echo 1500: enet 600 / kcp 200 を大きく上回る。litenetlib 2000 / apex 3000 が上。
- reliable_echo 2000: coop と同列、enet/kcp(1000)超え。apex/litenetlib(3000)が上。

## 盛らない注記

- media@50 の 0.9671 は gate(0.95)に対しマージン小。run 間で 0.91-0.96 程度ぶれる
  (client 側 2 物理コアの SMT 飽和が近い)。break 認定は今後の run で揺れる可能性あり。
- echo 2000 / reliable_echo 3000 の break は delivery ではなく client_tick invalid
  (クライアント側のティック維持失敗)。サーバー側の限界ではない。
- ②③⑤は「ライブラリ既定値の不適合」を設定で正したもので、GNS のコード改変は無し。
  ①はビルドフラグのみ(upstream が出荷ビルドで使う構成に合わせた)。

## 生データ

- gns 単独 canonical 掃引: `results/gns_cert_canonical_n3/`(`results/` は gitignore)
- 中間 A/B: `results/gns_ab_nagle/`(Nagle)、`results/gns_ab_buf/`(socket buffer)
- gprofng: `results/gns_profile_game/`(server、_CERT 前)、`/tmp/gnscli/cli.er`(client、_CERT 前)
- 全 lib の canonical 再計測(公開済み): [`../2026-06-09-canonical-193536Z/report.md`](../2026-06-09-canonical-193536Z/report.md)
  — `current.md` もこの run を指す。公開値でも gns は media 50 / game **96**(coop・enet・kcp の 64 超え)/
  echo 1500 / reliable_echo 2000 で、全 profile で最弱を脱却。
