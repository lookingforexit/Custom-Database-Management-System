#pragma once

// this file defines the executor interface for physical plan operators.
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "common/result.hpp"
#include "common/types.hpp"
#include "core/runtime_state.hpp"
#include "core/session_context.hpp"
#include "planner/plan.hpp"
#include "storage/string_pool.hpp"
#include "versioning/version_store.hpp"

namespace dbms::execution {

  struct QueryResult {
    std::vector<common::RowData> rows;
    std::string message;
  };

  class ExecutionEngine {
  public:
    ExecutionEngine(catalog::Catalog &catalog,
                    core::RuntimeState &runtime_state,
                    versioning::VersionStore &version_store,
                    storage::StringPool &string_pool);

    [[nodiscard]] common::Result<QueryResult>
    Execute(core::SessionContext &session, const planner::PlanNode &plan);

  private:
    catalog::Catalog &catalog_;
    core::RuntimeState &runtime_state_;
    versioning::VersionStore &version_store_;
    storage::StringPool &string_pool_;
  };

} // namespace dbms::execution
