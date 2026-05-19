#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "network/protocol.hpp"
#include "server/entrypoint.hpp"

namespace {

    bool ReadLine(int socket_fd, std::string &line) {
        line.clear();
        char ch = '\0';
        while (true) {
            const auto bytes = recv(socket_fd, &ch, 1, 0);
            if (bytes <= 0) {
                return false;
            }
            if (ch == '\n') {
                return true;
            }
            line.push_back(ch);
        }
    }

    bool WriteAll(int socket_fd, const std::string &buffer) {
        std::size_t sent = 0;
        while (sent < buffer.size()) {
            const auto bytes =
                send(socket_fd, buffer.data() + static_cast<long>(sent),
                     buffer.size() - sent, 0);
            if (bytes <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(bytes);
        }
        return true;
    }

    void HandleClientSocket(int client_fd, dbms::server::EntrypointServer &server) {
        std::string wire_request;
        if (!ReadLine(client_fd, wire_request)) {
            close(client_fd);
            return;
        }

        dbms::network::RequestEnvelope request;
        dbms::network::ResponseEnvelope response;
        if (!dbms::network::DeserializeRequest(wire_request, request)) {
            response.status_code = 400;
            response.payload =
                "type=PARSE_ERROR code=1 message=invalid request envelope sql=\"\"";
        } else {
            response = server.HandleRequest(request);
        }

        const auto wire_response = dbms::network::SerializeResponse(response);
        WriteAll(client_fd, wire_response);
        close(client_fd);
    }

} // namespace

// this file starts a storage node process for shard-local data work.
int main(int argc, char **argv) {
    int port = 4546;
    if (argc >= 2) {
        try {
            port = std::stoi(argv[1]);
        } catch (...) {
            std::cerr << "invalid port\n";
            return 1;
        }
    }
    if (port <= 0 || port > 65535) {
        std::cerr << "invalid port range\n";
        return 1;
    }

    const std::string root_path = "./storage_data_" + std::to_string(port);
    dbms::server::EntrypointServer server(root_path);

    const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket create failed\n";
        return 1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listen_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
        0) {
        std::cerr << "bind failed\n";
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 16) < 0) {
        std::cerr << "listen failed\n";
        close(listen_fd);
        return 1;
    }

    std::cout << "dbms_storage_node listening on port " << port << "\n";
    while (true) {
        const int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            continue;
        }
        std::thread(HandleClientSocket, client_fd, std::ref(server)).detach();
    }
}
