cmake_minimum_required(VERSION 3.20)

project(DatabaseManagementSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dbms_core
    src/catalog/catalog.cpp
    src/catalog/rbac.cpp
    src/catalog/schema.cpp
    src/common/types.cpp
    src/core/dbms_engine.cpp
    src/execution/execution_engine.cpp
    src/index/b_star_plus_tree.cpp
    src/network/protocol.cpp
    src/parser/lexer.cpp
    src/parser/parser.cpp
    src/planner/planner.cpp
    src/runtime/job_queue.cpp
    src/runtime/telemetry.cpp
    src/server/entrypoint.cpp
    src/storage/page_manager.cpp
    src/storage/string_pool.cpp
    src/storage/table_heap.cpp
    src/transaction/transaction_manager.cpp
    src/versioning/version_store.cpp
)

target_include_directories(dbms_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(dbms_cli apps/cli/main.cpp)
target_link_libraries(dbms_cli PRIVATE dbms_core)

add_executable(dbms_server apps/server/main.cpp)
target_link_libraries(dbms_server PRIVATE dbms_core)

add_executable(dbms_storage_node apps/storage_node/main.cpp)
target_link_libraries(dbms_storage_node PRIVATE dbms_core)

enable_testing()

add_executable(dbms_smoke tests/smoke.cpp)
target_link_libraries(dbms_smoke PRIVATE dbms_core)
add_test(NAME dbms_smoke COMMAND dbms_smoke)

add_executable(dbms_parser_validation tests/parser_validation.cpp)
target_link_libraries(dbms_parser_validation PRIVATE dbms_core)
add_test(NAME dbms_parser_validation COMMAND dbms_parser_validation)

add_executable(dbms_planner_mapping tests/planner_mapping.cpp)
target_link_libraries(dbms_planner_mapping PRIVATE dbms_core)
add_test(NAME dbms_planner_mapping COMMAND dbms_planner_mapping)
