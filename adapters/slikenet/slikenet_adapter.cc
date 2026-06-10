// bundled SLikeNet を RakNet 系共通 adapter (per-connection RakPeer) で計測する。
// 旧実装は単一 RakPeer 共有のため同一 server への多重接続が張れず
// max_connections=1 のスタブだった。
#include "../raknet/rak_family_adapter.h"

#include "harness/adapter_registry.h"

#include <memory>

namespace rudp_bench {
void register_slikenet_adapter() {
  register_adapter("slikenet", []() {
    return std::make_unique<rak_family::RakFamilyAdapter>("slikenet");
  });
}
}  // namespace rudp_bench
