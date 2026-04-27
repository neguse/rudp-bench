#pragma once

#include "harness/adapter.h"
#include "harness/csv_writer.h"
#include "harness/scenario.h"

namespace rudp_bench {

CsvRow run_server(Adapter& a, const ScenarioConfig& cfg);
CsvRow run_client(Adapter& a, const ScenarioConfig& cfg);

}  // namespace rudp_bench
