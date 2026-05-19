cmake_minimum_required(VERSION 3.20)

project(DatabaseManagementSystem LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(dbms_core
    src/catalog/catalog.cpp
    src/catalog/rbac.cpp
    src/catalog/schema.cpp
    src/common/error_contract.cpp
    src/common/uuid.cpp
    src/common/types.cpp
    src/core/dbms_engine.cpp
    src/core/runtime_state.cpp
    src/core/runtime_persistence.cpp
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

add_executable(dbms_bstar_plus_tree tests/bstar_plus_tree.cpp)
target_link_libraries(dbms_bstar_plus_tree PRIVATE dbms_core)
add_test(NAME dbms_bstar_plus_tree COMMAND dbms_bstar_plus_tree)

add_executable(dbms_bstar_plus_tree_stress tests/bstar_plus_tree_stress.cpp)
target_link_libraries(dbms_bstar_plus_tree_stress PRIVATE dbms_core)
add_test(NAME dbms_bstar_plus_tree_stress COMMAND dbms_bstar_plus_tree_stress)

add_executable(dbms_persistence tests/persistence.cpp)
target_link_libraries(dbms_persistence PRIVATE dbms_core)
add_test(NAME dbms_persistence COMMAND dbms_persistence)

add_executable(dbms_revert tests/revert.cpp)
target_link_libraries(dbms_revert PRIVATE dbms_core)
add_test(NAME dbms_revert COMMAND dbms_revert)

add_executable(dbms_revert_persistence tests/revert_persistence.cpp)
target_link_libraries(dbms_revert_persistence PRIVATE dbms_core)
add_test(NAME dbms_revert_persistence COMMAND dbms_revert_persistence)

add_executable(dbms_revert_requirements tests/revert_requirements.cpp)
target_link_libraries(dbms_revert_requirements PRIVATE dbms_core)
add_test(NAME dbms_revert_requirements COMMAND dbms_revert_requirements)

add_executable(dbms_transactions tests/transactions.cpp)
target_link_libraries(dbms_transactions PRIVATE dbms_core)
add_test(NAME dbms_transactions COMMAND dbms_transactions)

add_executable(dbms_revert_transactions tests/revert_transactions.cpp)
target_link_libraries(dbms_revert_transactions PRIVATE dbms_core)
add_test(NAME dbms_revert_transactions COMMAND dbms_revert_transactions)

add_test(NAME dbms_cli_batch
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_cli_batch.sh
            $<TARGET_FILE:dbms_cli>)

add_executable(dbms_index_consistency tests/index_consistency.cpp)
target_link_libraries(dbms_index_consistency PRIVATE dbms_core)
add_test(NAME dbms_index_consistency COMMAND dbms_index_consistency)

add_executable(dbms_error_contract tests/error_contract.cpp)
target_link_libraries(dbms_error_contract PRIVATE dbms_core)
add_test(NAME dbms_error_contract COMMAND dbms_error_contract)

add_executable(dbms_core_integration_extended tests/core_integration_extended.cpp)
target_link_libraries(dbms_core_integration_extended PRIVATE dbms_core)
add_test(NAME dbms_core_integration_extended COMMAND dbms_core_integration_extended)

add_executable(dbms_index_no_data_duplication tests/index_no_data_duplication.cpp)
target_link_libraries(dbms_index_no_data_duplication PRIVATE dbms_core)
add_test(NAME dbms_index_no_data_duplication COMMAND dbms_index_no_data_duplication)

add_executable(dbms_revert_no_file_snapshot tests/revert_no_file_snapshot.cpp)
target_link_libraries(dbms_revert_no_file_snapshot PRIVATE dbms_core)
add_test(NAME dbms_revert_no_file_snapshot COMMAND dbms_revert_no_file_snapshot)

add_executable(dbms_string_interning tests/string_interning.cpp)
target_link_libraries(dbms_string_interning PRIVATE dbms_core)
add_test(NAME dbms_string_interning COMMAND dbms_string_interning)

add_executable(dbms_cluster_entrypoint tests/cluster_entrypoint.cpp)
target_link_libraries(dbms_cluster_entrypoint PRIVATE dbms_core)
add_test(NAME dbms_cluster_entrypoint COMMAND dbms_cluster_entrypoint)

add_executable(dbms_async_job_queue tests/async_job_queue.cpp)
target_link_libraries(dbms_async_job_queue PRIVATE dbms_core)
add_test(NAME dbms_async_job_queue COMMAND dbms_async_job_queue)

add_executable(dbms_access_logs tests/access_logs.cpp)
target_link_libraries(dbms_access_logs PRIVATE dbms_core)
add_test(NAME dbms_access_logs COMMAND dbms_access_logs)

add_executable(dbms_telemetry_realtime tests/telemetry_realtime.cpp)
target_link_libraries(dbms_telemetry_realtime PRIVATE dbms_core)
add_test(NAME dbms_telemetry_realtime COMMAND dbms_telemetry_realtime)

add_executable(dbms_auth_rbac_jwt tests/auth_rbac_jwt.cpp)
target_link_libraries(dbms_auth_rbac_jwt PRIVATE dbms_core)
add_test(NAME dbms_auth_rbac_jwt COMMAND dbms_auth_rbac_jwt)
