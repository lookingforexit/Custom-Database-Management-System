#pragma once

// this file defines the top-level dbms facade that orchestrates sql execution.

#include "catalog/catalog.hpp"
#include "common/result.hpp"
#include "execution/execution_engine.hpp"
#include "parser/parser.hpp"
#include "planner/planner.hpp"
#include "runtime_state.hpp"
#include "session_context.hpp"
#include "storage/string_pool.hpp"
#include "versioning/version_store.hpp"

namespace dbms::core {

  class DbmsEngine {
  public:
    explicit DbmsEngine(std::string root_path);

    [[nodiscard]] RuntimeState &runtime_state();
    [[nodiscard]] catalog::Catalog &catalog();

    [[nodiscard]] common::Result<execution::QueryResult>
    ExecuteSql(SessionContext &session, const std::string &sql);

  private:
    std::string root_path_;
    RuntimeState runtime_state_;

    parser::Parser parser_;
    planner::Planner planner_;
    catalog::Catalog catalog_;
    versioning::VersionStore version_store_;
    storage::StringPool string_pool_;
    execution::ExecutionEngine execution_;
  };

} // namespace dbms::core
