#include <iostream>
#include <string>

#include "core/dbms_engine.hpp"

// this file boots the cli client and routes commands through the dbms engine.
int main() {
    dbms::core::DbmsEngine engine("./data");
    dbms::core::SessionContext session;
    session.client_id = "cli";

    std::string sql;
    std::getline(std::cin, sql);

    auto result = engine.ExecuteSql(session, sql);
    if (!result.ok()) {
        std::cout << "error\n";
        return 1;
    }

    std::cout << result.value->message << "\n";
    return 0;
}
