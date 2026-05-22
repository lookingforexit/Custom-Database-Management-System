#include "server/entrypoint.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main() {
    const std::string root = "./test_data_access_logs";
    std::filesystem::remove_all(root);

    dbms::server::EntrypointServer server(root);
    dbms::network::RequestEnvelope request{
        .client_id = "log_client",
        .jwt_token = "",
        .payload = "CLUSTER PING",
    };
    auto response = server.HandleRequest(request);
    if (response.status_code != 200) return 1;

    request.payload = "BROKEN SQL";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    request.payload = "SELECT\n*\tFROM bad;";
    response = server.HandleRequest(request);
    if (response.status_code != 400) return 1;

    const std::string log_path = root + "/access.log";
    std::ifstream input(log_path);
    if (!input.is_open()) return 1;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.size() < 3) return 1;

    const std::string content =
        lines[0] + "\n" + lines[1] + "\n" + lines[2] + "\n";
    if (content.find("client_id=log_client") == std::string::npos) return 1;
    if (content.find("handler_id=handler-") == std::string::npos) return 1;
    if (content.find("start=") == std::string::npos) return 1;
    if (content.find("finish=") == std::string::npos) return 1;
    if (content.find("status_code=200") == std::string::npos) return 1;
    if (content.find("status_code=400") == std::string::npos) return 1;
    if (content.find("sql=\"CLUSTER PING\"") == std::string::npos) return 1;
    if (content.find("sql=\"BROKEN SQL\"") == std::string::npos) return 1;
    if (content.find("sql=\"SELECT * FROM bad;\"") == std::string::npos) return 1;

    for (const auto &entry : lines) {
        if (entry.find("start=") == std::string::npos) return 1;
        if (entry.find("finish=") == std::string::npos) return 1;
        if (entry.find("handler_id=") == std::string::npos) return 1;
        if (entry.find("status_code=") == std::string::npos) return 1;
        if (entry.find("latency_ms=") == std::string::npos) return 1;
        if (entry.find('\t') == std::string::npos) return 1;
    }

    if (lines[2].find('\n') != std::string::npos ||
        lines[2].find('\r') != std::string::npos) {
        return 1;
    }

    std::filesystem::remove_all(root);
    return 0;
}
