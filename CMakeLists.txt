cmake_minimum_required(VERSION 3.20)
project(rudp-bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_subdirectory(harness)
add_subdirectory(coop_rudp)
add_subdirectory(adapters/raw_udp)
add_subdirectory(adapters/mini_rudp)
add_subdirectory(adapters/coop_rudp)
add_subdirectory(adapters/apex_rudp)
add_subdirectory(adapters/enet)
add_subdirectory(adapters/kcp)
add_subdirectory(adapters/slikenet)
add_subdirectory(adapters/raknet)
add_subdirectory(adapters/udt4)
add_subdirectory(adapters/yojimbo)
add_subdirectory(adapters/gns)
add_subdirectory(adapters/msquic)
add_subdirectory(adapters/quiche)
add_subdirectory(adapters/lsquic)

# Shared library adapters: set RPATH so the binary finds .so files at runtime
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_BUILD_RPATH
    ${CMAKE_BINARY_DIR}/quiche_cargo/release
    ${CMAKE_BINARY_DIR}/adapters/lsquic
)

# LiteNetLib: .NET 10 独立バイナリ — cmake --build がここで dotnet build を呼ぶ
find_program(DOTNET_EXECUTABLE dotnet)
if(DOTNET_EXECUTABLE)
    add_custom_target(litenetlib_build ALL
        COMMAND ${DOTNET_EXECUTABLE} build -c Release
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/adapters/litenetlib"
        COMMENT "Building LiteNetLib .NET adapter (adapters/litenetlib)"
        VERBATIM
    )
    message(STATUS "dotnet found: ${DOTNET_EXECUTABLE} — litenetlib_build target added")
else()
    message(WARNING "dotnet not found; LiteNetLib adapter will not be built (install a .NET 10 SDK)")
endif()

add_subdirectory(tests)
