add_executable(test_adapter_registry test_adapter_registry.cc)
target_link_libraries(test_adapter_registry PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_adapter_registry COMMAND test_adapter_registry)

add_executable(test_scenario test_scenario.cc)
target_link_libraries(test_scenario PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_scenario COMMAND test_scenario)

add_executable(test_metrics test_metrics.cc)
target_link_libraries(test_metrics PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_metrics COMMAND test_metrics)

add_executable(test_csv_writer test_csv_writer.cc)
target_link_libraries(test_csv_writer PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_csv_writer COMMAND test_csv_writer)

add_executable(test_proc_sampler test_proc_sampler.cc)
target_link_libraries(test_proc_sampler PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_proc_sampler COMMAND test_proc_sampler)

add_executable(test_raw_udp_smoke test_raw_udp_smoke.cc)
target_link_libraries(test_raw_udp_smoke PRIVATE adapter_raw_udp GTest::gtest_main)
add_test(NAME test_raw_udp_smoke COMMAND test_raw_udp_smoke)

add_executable(test_mini_rudp_smoke test_mini_rudp_smoke.cc)
target_link_libraries(test_mini_rudp_smoke PRIVATE adapter_mini_rudp GTest::gtest_main)
add_test(NAME test_mini_rudp_smoke COMMAND test_mini_rudp_smoke)

add_executable(test_runner_loopback test_runner_loopback.cc)
target_link_libraries(test_runner_loopback PRIVATE rudp_bench_harness adapter_raw_udp GTest::gtest_main)
add_test(NAME test_runner_loopback COMMAND test_runner_loopback)

add_executable(test_enet_smoke test_enet_smoke.cc)
target_link_libraries(test_enet_smoke PRIVATE adapter_enet GTest::gtest_main)
add_test(NAME test_enet_smoke COMMAND test_enet_smoke)

add_executable(test_kcp_smoke test_kcp_smoke.cc)
target_link_libraries(test_kcp_smoke PRIVATE adapter_kcp GTest::gtest_main)
add_test(NAME test_kcp_smoke COMMAND test_kcp_smoke)

add_executable(test_slikenet_smoke test_slikenet_smoke.cc)
target_link_libraries(test_slikenet_smoke PRIVATE adapter_slikenet GTest::gtest_main)
add_test(NAME test_slikenet_smoke COMMAND test_slikenet_smoke)

add_executable(test_yojimbo_smoke test_yojimbo_smoke.cc)
target_link_libraries(test_yojimbo_smoke PRIVATE adapter_yojimbo GTest::gtest_main)
add_test(NAME test_yojimbo_smoke COMMAND test_yojimbo_smoke)
