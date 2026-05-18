#include <iostream>

#include "network/protocol.hpp"
#include "server/entrypoint.hpp"

// this file boots the entrypoint server process and exercises the request path.
int main() {
    dbms::server::EntrypointServer server("./data");
    dbms::network::RequestEnvelope request{
        .client_id = "server",
        .jwt_token = "",
        .payload = "SELECT * FROM test;",
    };
    const auto response = server.HandleRequest(request);

    std::cout << "dbms_server stub\n";
    std::cout << "status=" << response.status_code << "\n";
    return 0;
}
