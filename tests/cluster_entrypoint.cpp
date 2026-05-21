#include "server/entrypoint.hpp"
#include <filesystem>

int main() {
    const std::string root = "./test_data_cluster_entrypoint";
    std::filesystem::remove_all(root);
    dbms::server::EntrypointServer server(root);

    dbms::network::RequestEnvelope request{
        .client_id = "cluster",
        .jwt_token = "",
        .payload = "CLUSTER LIST_NODES",
    };
    auto response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER PING";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload != "pong") return 1;

    // Participant-side 2PC commands on single node instance.
    request.payload = "CLUSTER PREPARE_TX tx_local_1 CREATE DATABASE prepared_db;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    request.payload = "CLUSTER COMMIT_TX tx_local_1";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    request.payload = "USE prepared_db;";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER ADD_NODE 127.0.0.1:4546";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER ADD_NODE 127.0.0.1:4547 MANAGED";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER LIST_NODES";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload.find("nodes=2") == std::string::npos) return 1;
    if (response.payload.find("managed=true") == std::string::npos) return 1;

    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:4546";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:4547";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    // Coordinator 2PC path must fail if storage node is unavailable.
    request.payload = "CLUSTER ADD_NODE 127.0.0.1:49999";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    request.payload = "CREATE DATABASE fail_2pc;";
    response = server.HandleRequest(request);
    if (response.status_code != 502) return 1;
    request.payload = "CLUSTER REMOVE_NODE 127.0.0.1:49999";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "CLUSTER LIST_NODES";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload.find("nodes=0") == std::string::npos) return 1;

    std::filesystem::remove_all(root);
    return 0;
}
