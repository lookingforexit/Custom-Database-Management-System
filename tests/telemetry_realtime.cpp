#include "server/entrypoint.hpp"

#include <filesystem>
#include <string>

int main() {
    const std::string root = "./test_data_telemetry";
    std::filesystem::remove_all(root);

    dbms::server::EntrypointServer server(root);
    dbms::network::RequestEnvelope request{
        .client_id = "telemetry",
        .jwt_token = "",
        .payload = "CLUSTER PING",
    };
    auto response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "BROKEN SQL";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;
    request.payload = "CLUSTER PING";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    request.payload = "CLUSTER PING";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "TELEMETRY LOCAL";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload.find("current_rps=") == std::string::npos) return 1;
    if (response.payload.find("average_rps_10m=") == std::string::npos) return 1;
    if (response.payload.find("max_rps_10m=") == std::string::npos) return 1;
    if (response.payload.find("average_latency_10s_ms=") == std::string::npos)
        return 1;
    if (response.payload.find("error_count_1m=") == std::string::npos) return 1;
    if (response.payload.find("error_rate_1m=") == std::string::npos) return 1;
    if (response.payload.find("requests_1m=") == std::string::npos) return 1;
    if (response.payload.find("buckets=") == std::string::npos) return 1;
    if (response.payload.find("error_count_1m=0") != std::string::npos) return 1;

    request.payload = "TELEMETRY SNAPSHOT";
    response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;
    if (response.payload.find("current_rps=") == std::string::npos) return 1;
    if (response.payload.find("average_rps_10m=") == std::string::npos) return 1;
    if (response.payload.find("max_rps_10m=") == std::string::npos) return 1;
    if (response.payload.find("average_latency_10s_ms=") == std::string::npos)
        return 1;
    if (response.payload.find("error_count_1m=") == std::string::npos) return 1;
    if (response.payload.find("error_rate_1m=") == std::string::npos) return 1;
    if (response.payload.find("nodes_total=1") == std::string::npos) return 1;
    if (response.payload.find("nodes_available=1") == std::string::npos) return 1;

    std::filesystem::remove_all(root);
    return 0;
}
