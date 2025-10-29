cmake_minimum_required(VERSION 3.16)
project(edge_trader_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Armadillo
find_package(Armadillo REQUIRED)

# Исполняемый файл и исходники
add_executable(edge_trader_server
  src/main.cpp
  src/ppo.cpp
  src/ppo_pro.cpp
  src/rt_metrics.cpp
  src/utils_data.cpp
  src/train_logic.cpp
  src/optim/adam.cpp
  src/features/features.cpp
  src/infer_policy.cpp
  # Роуты
  src/routes/health.cpp
  src/routes/infer.cpp
  src/routes/agents.cpp
  src/routes/model.cpp
  # Инклюд-блоки (подключаются через #include)
  src/routes/backfill.inc.cpp
  src/routes/health_base.inc.cpp
  src/routes/health_ai.inc.cpp
  src/routes/metrics.cpp
  src/routes/metrics.inc.cpp
  src/routes/model.inc.cpp
  src/routes/train.cpp
  src/routes/train.inc.cpp
)

target_include_directories(edge_trader_server PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Включаем поддержку OpenSSL в cpp-httplib
target_compile_definitions(edge_trader_server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# Линкуем системные библиотеки
target_link_libraries(edge_trader_server PRIVATE
  armadillo
  ssl
  crypto
  pthread
)

# set(CMAKE_VERBOSE_MAKEFILE ON)
