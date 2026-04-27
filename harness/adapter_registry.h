#pragma once

#include <functional>
#include <memory>
#include <string>

#include "harness/adapter.h"

namespace rudp_bench {

using AdapterFactory = std::function<std::unique_ptr<Adapter>()>;

void register_adapter(const std::string& name, AdapterFactory factory);
std::unique_ptr<Adapter> create_adapter(const std::string& name);

}  // namespace rudp_bench
