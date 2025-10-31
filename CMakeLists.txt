cmake_minimum_required(VERSION 3.16)
project(edge_trader_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Включаем SSL для httplib ---
add_definitions(-DCPPHTTPLIB_OPENSSL_SUPPORT)

find_package(OpenSSL REQUIRED)
find_package(Armadillo REQUIRED)

add_executable(edge_trader_server
    src/metrics.cpp
    src/rewardv2_accessors.cpp
    src/main.cpp

    # --- Core ---
    src/ppo_pro.cpp
    src/ppo.cpp
    src/optim/adam.cpp
    src/utils_data.cpp
    src/rt_metrics.cpp
    src/infer_policy.cpp
    src/train_logic.cpp
    src/server_accessors.cpp
    src/features/features.cpp

    # --- Agents ---
    src/agents/agent_long.cpp
    src/agents/agent_short.cpp
    src/agents/agent_flat.cpp
    src/agents/agent_correction.cpp
    src/agents/agent_breakout.cpp
)

target_include_directories(edge_trader_server PRIVATE src)
target_link_libraries(edge_trader_server PRIVATE armadillo OpenSSL::SSL OpenSSL::Crypto)
