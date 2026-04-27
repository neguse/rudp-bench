#include "harness/adapter_registry.h"

#include <unordered_map>

namespace rudp_bench {
namespace {
std::unordered_map<std::string, AdapterFactory>& registry() {
  static std::unordered_map<std::string, AdapterFactory> r;
  return r;
}
}  // namespace

void register_adapter(const std::string& name, AdapterFactory factory) {
  registry()[name] = std::move(factory);
}

std::unique_ptr<Adapter> create_adapter(const std::string& name) {
  auto it = registry().find(name);
  if (it == registry().end()) return nullptr;
  return it->second();
}

}  // namespace rudp_bench
