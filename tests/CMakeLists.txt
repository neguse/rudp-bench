# lo に netem が残っているとループバック越しの smoke テストが
# 遅延/ロスで黙って落ちる(ベンチ後の netem clear 忘れが典型)。
# 最初に明示的なメッセージで fail させて原因を分かりやすくする。
add_test(
    NAME test_no_netem_on_lo
    COMMAND sh -c "if tc qdisc show dev lo | grep -q netem; then echo 'ERROR: netem is still applied to lo (run: sudo tc qdisc del dev lo root). Loopback smoke tests would fail spuriously.' >&2; exit 1; fi"
)

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

add_executable(test_inbound_queue test_inbound_queue.cc)
target_link_libraries(test_inbound_queue PRIVATE rudp_bench_harness GTest::gtest_main)
add_test(NAME test_inbound_queue COMMAND test_inbound_queue)

add_executable(test_coop_rudp test_coop_rudp.cc)
target_link_libraries(test_coop_rudp PRIVATE coop_rudp GTest::gtest_main)
add_test(NAME test_coop_rudp COMMAND test_coop_rudp)

add_executable(test_coop_rudp_adapter_smoke test_coop_rudp_adapter_smoke.cc)
target_link_libraries(test_coop_rudp_adapter_smoke PRIVATE adapter_coop_rudp GTest::gtest_main)
add_test(NAME test_coop_rudp_adapter_smoke COMMAND test_coop_rudp_adapter_smoke)

add_executable(test_raw_udp_smoke test_raw_udp_smoke.cc)
target_link_libraries(test_raw_udp_smoke PRIVATE adapter_raw_udp GTest::gtest_main)
add_test(NAME test_raw_udp_smoke COMMAND test_raw_udp_smoke)

add_executable(test_mini_rudp_smoke test_mini_rudp_smoke.cc)
target_link_libraries(test_mini_rudp_smoke PRIVATE adapter_mini_rudp GTest::gtest_main)
add_test(NAME test_mini_rudp_smoke COMMAND test_mini_rudp_smoke)

add_executable(test_apex_rudp_smoke test_apex_rudp_smoke.cc)
target_link_libraries(test_apex_rudp_smoke PRIVATE adapter_apex_rudp GTest::gtest_main)
add_test(NAME test_apex_rudp_smoke COMMAND test_apex_rudp_smoke)

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

add_executable(test_raknet_smoke test_raknet_smoke.cc)
target_link_libraries(test_raknet_smoke PRIVATE adapter_raknet GTest::gtest_main)
add_test(NAME test_raknet_smoke COMMAND test_raknet_smoke)

add_executable(test_udt4_smoke test_udt4_smoke.cc)
target_link_libraries(test_udt4_smoke PRIVATE adapter_udt4 GTest::gtest_main)
add_test(NAME test_udt4_smoke COMMAND test_udt4_smoke)

add_executable(test_yojimbo_smoke test_yojimbo_smoke.cc)
target_link_libraries(test_yojimbo_smoke PRIVATE adapter_yojimbo GTest::gtest_main)
add_test(NAME test_yojimbo_smoke COMMAND test_yojimbo_smoke)

add_executable(test_gns_smoke test_gns_smoke.cc)
target_link_libraries(test_gns_smoke PRIVATE adapter_gns GTest::gtest_main)
add_test(NAME test_gns_smoke COMMAND test_gns_smoke)

add_executable(test_msquic_smoke test_msquic_smoke.cc)
target_link_libraries(test_msquic_smoke PRIVATE adapter_msquic GTest::gtest_main)
add_test(NAME test_msquic_smoke COMMAND test_msquic_smoke)

add_executable(test_quiche_smoke test_quiche_smoke.cc)
target_link_libraries(test_quiche_smoke PRIVATE adapter_quiche GTest::gtest_main)
add_test(NAME test_quiche_smoke COMMAND test_quiche_smoke)

# LiteNetLib smoke test — shell script (NOT a C++ GTest; invokes the .NET binary on loopback)
find_program(DOTNET_EXECUTABLE dotnet)
find_program(BASH_EXECUTABLE bash)
if(DOTNET_EXECUTABLE AND BASH_EXECUTABLE)
    add_test(
        NAME test_litenetlib_smoke
        COMMAND ${BASH_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tests/test_litenetlib_smoke.sh"
    )
    set_tests_properties(test_litenetlib_smoke PROPERTIES TIMEOUT 60)
endif()
