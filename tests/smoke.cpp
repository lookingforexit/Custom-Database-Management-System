#include <cassert>
#include <iostream>

#include "common/types.hpp"
#include "core/dbms_engine.hpp"

// this file provides a smoke test for the end-to-end dbms execution pipeline.
int main() {
    using namespace dbms::common;

    Value int_small = std::int64_t{2};
    Value int_large = std::int64_t{10};
    Value str_a = std::string{"abc"};
    Value str_b = std::string{"abd"};
    Value null_value = std::monostate{};

    assert(GetValueType(int_small) == ValueType::kInt64);
    assert(GetValueType(str_a) == ValueType::kString);
    assert(GetValueType(null_value) == ValueType::kNull);

    assert(CompareValues(int_small, int_large) < 0);
    assert(CompareValues(int_large, int_small) > 0);
    assert(CompareValues(int_large, std::int64_t{10}) == 0);
    assert(CompareValues(str_a, str_b) < 0);
    assert(CompareValues(null_value, std::monostate{}) == 0);

    assert(CanAssignToType(int_small, ValueType::kInt64, false));
    assert(!CanAssignToType(str_a, ValueType::kInt64, false));
    assert(CanAssignToType(null_value, ValueType::kString, true));
    assert(!CanAssignToType(null_value, ValueType::kString, false));

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
