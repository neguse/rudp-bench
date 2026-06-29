# Tasks

`docs/adapter-audit.md` の改善で、adapter 変更だけでは完了しない残作業。

- [ ] `coop_rudp` core に per-conn close/abort と max retransmit/idle timeout を追加し、crashed peer の reliable retransmit queue を解放できるようにする。
- [ ] 全 native adapter で `setsockopt(SO_RCVBUF/SO_SNDBUF)` 後に `getsockopt` で実値を確認し、sysctl clamp を run metadata または diagnostics に出す。
- [ ] `msquic` datapath の UDP socket buffer 要求を現実的な値にし、SO_SNDBUF も明示設定する。adapter から制御できない場合は vendored MsQuic patch として管理する。
- [ ] `udt4` adapter が UDT 内部の EXP timeout / broken 状態を `is_connected()` と `send()` に反映する。
- [ ] `raknet`/`slikenet` の `RAKPEER_USER_THREADED=1` でも recv thread が生成される問題を vendored SLikeNet 側で修正し、adapter の abandon leak 回避を不要にする。
- [ ] adapter inbox / pending queue が無制限または実質無制限の経路に上限と backpressure を入れる: `enet` reliable, `kcp` send queue, `gns` inbox, `msquic` inbox, `lsquic` stream `pending_writes`, `udt4` adapter `out_pending`, `raknet`/`slikenet` outgoing, `litenetlib` outgoing。
- [ ] `quiche` stream path の partial write を破棄せず、残りを pending 化するか明示 backpressure として返す。
- [ ] 固定 RTO かつ backoff なしの adapter（`coop_rudp`, `apex_rudp`, `mini_rudp`, `yojimbo`）に RTT ベース RTO/backoff を入れるか、固定 RTO を使うベンチ前提を結果 metadata に明示する。
- [ ] `mini_rudp` の per-packet 専用 ACK を piggyback / cumulative ACK に置き換え、ACK traffic と syscall 数を下げる。
- [ ] 単発 syscall 経路の adapter/library（`raw_udp`, `mini_rudp`, `kcp`, `raknet`/`slikenet`, `udt4`, `yojimbo`, `gns`, `litenetlib`）に対して、batch send/recv, ACK piggyback, GSO/GRO の適用可否を評価し、できないものは結果 metadata に制約として出す。
- [ ] ベンチ用にライブラリ既定を上書きしている adapter（`kcp` nocwnd/snd_wnd/fastresend, `gns` socket/send buffer/send rate, `msquic` BBR, `quiche` BBRv2, `lsquic` BBR, `udt4` BenchCCC, `yojimbo` queue size, `litenetlib` PacketPoolSize）に library-default variant を追加し、結果解釈で区別できるようにする。
- [ ] 輻輳制御がない/弱い/回復が遅い経路（`apex_rudp`, `mini_rudp`, `yojimbo`, `litenetlib`, `coop_rudp` 初期 UINT32 rate, `enet` slow start なし, `raknet`/`slikenet` Tahoe 風 cwnd reset, `quiche` BBRv2 beta=0.3）を metadata 化し、必要なら workload 別 variant を追加する。
- [ ] 15/16/24-bit など狭い sequence number を使うライブラリ（`enet`, `yojimbo`, `raknet`/`slikenet`, `litenetlib`）に長時間/高 PPS wrap test を追加し、`raknet`/`slikenet` の uint24 と 32-bit halfSpan の型幅不一致を調査する。
- [ ] MTU/fragmentation 制約を capability として明示し、multi-fragment 非対応/制限ありの adapter（`mini_rudp`, `apex_rudp`, `coop_rudp` adapter payload cap, `kcp` in-order fragment HoL, `raknet`/`slikenet` unreliable fragment の reliable 昇格, `gns` unreliable 超過破棄, `litenetlib` adapter 1000B cap）を payload-size sweep で検証する。
- [ ] worker thread / lock 競合の既知ボトルネック（`msquic` adapter 単一 mutex, `udt4` shared mux lock, `raknet`/`slikenet` bufferedPacketsQueueMutex, `litenetlib` `_poolLock` と既定 thread mode）を profile し、必要なら sharding/manual mode/lock 分割を入れる。
- [ ] `gns` の 1000conn global lock collapse を poll group 分割などで再評価し、暗号なし/暗号あり variant の差も metadata に出す。
- [ ] `litenetlib` の cumulative mean RTT 推定、`lsquic` の ACK-only datagram、`udt4` の NAK/EXP timer など、adapter では変更しない protocol 固有の挙動を report metadata に出す。
