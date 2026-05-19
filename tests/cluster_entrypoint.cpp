#include "server/entrypoint.hpp"

int main() {
    dbms::server::EntrypointServer server("./test_data_cluster_entrypoint");

    dbms::network::RequestEnvelope request{
        .client_id = "cluster",
        .jwt_token = "",
        .payload = "CLUSTER LIST_NODES",
    };
    auto response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER ADD_NODE 127.0.0.1:4546";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER LIST_NODES";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload.find("nodes=1") == std::string::npos) return 1;

    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:4546";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER LIST_NODES";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload.find("nodes=0") == std::string::npos) return 1;

    return 0;
}
