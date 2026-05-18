#include <iostream>

#include "core/dbms_engine.hpp"

// this file provides a smoke test for the end-to-end dbms execution pipeline.
int main() {
  dbms::core::DbmsEngine engine("./test_data");
  dbms::core::SessionContext session;
  session.client_id = "smoke";

  auto result = engine.ExecuteSql(session, "SELECT * FROM test;");
  if (!result.ok()) {
    std::cout << "pipeline_error\n";
    return 1;
  }

  std::cout << "pipeline_ok\n";
  return 0;
}
