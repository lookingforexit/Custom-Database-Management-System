#include "server/entrypoint.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

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

    const std::string log_path = root + "/access.log";
    std::ifstream input(log_path);
    if (!input.is_open()) return 1;

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string content = buffer.str();

    if (content.find("client_id=log_client") == std::string::npos) return 1;
    if (content.find("status=200") == std::string::npos) return 1;
    if (content.find("status=400") == std::string::npos) return 1;
    if (content.find("sql=\"CLUSTER PING\"") == std::string::npos) return 1;
    if (content.find("sql=\"BROKEN SQL\"") == std::string::npos) return 1;
    const auto lines = static_cast<int>(std::count(content.begin(), content.end(), '\n'));
    if (lines < 2) return 1;

    std::filesystem::remove_all(root);
    return 0;
}
