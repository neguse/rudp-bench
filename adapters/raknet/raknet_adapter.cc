#include "rak_family_adapter.h"

#include "harness/adapter_registry.h"

#include <memory>

namespace rudp_bench {
void register_raknet_adapter() {
  register_adapter("raknet", []() {
    return std::make_unique<rak_family::RakFamilyAdapter>("raknet");
  });
}
}  // namespace rudp_bench
