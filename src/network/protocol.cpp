#include "network/protocol.hpp"

#include <sstream>
#include <vector>

// this file implements protocol serialization and transport helpers.
namespace dbms::network {

    namespace {

        std::string EscapeField(const std::string &value) {
            std::string escaped;
            escaped.reserve(value.size());
            for (char ch : value) {
                if (ch == '\\' || ch == '\t' || ch == '\n') {
                    escaped.push_back('\\');
                }
                escaped.push_back(ch);
            }
            return escaped;
        }

        std::string UnescapeField(const std::string &value) {
            std::string unescaped;
            unescaped.reserve(value.size());
            bool escaped = false;
            for (char ch : value) {
                if (escaped) {
                    unescaped.push_back(ch);
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else {
                    unescaped.push_back(ch);
                }
            }
            return unescaped;
        }

        std::vector<std::string> SplitByTab(const std::string &line) {
            std::vector<std::string> parts;
            std::string current;
            bool escaped = false;
            for (char ch : line) {
                if (escaped) {
                    current.push_back(ch);
                    escaped = false;
                    continue;
                }
                if (ch == '\\') {
                    escaped = true;
                    current.push_back(ch);
                    continue;
                }
                if (ch == '\t') {
                    parts.push_back(current);
                    current.clear();
                    continue;
                }
                current.push_back(ch);
            }
            parts.push_back(current);
            return parts;
        }

    } // namespace

    std::string SerializeRequest(const RequestEnvelope &request) {
        return EscapeField(request.client_id) + "\t" +
               EscapeField(request.jwt_token) + "\t" +
               EscapeField(request.payload) + "\n";
    }

    std::string SerializeResponse(const ResponseEnvelope &response) {
        return std::to_string(response.status_code) + "\t" +
               EscapeField(response.payload) + "\n";
    }

    bool DeserializeRequest(const std::string &wire, RequestEnvelope &request) {
        const auto parts = SplitByTab(wire);
        if (parts.size() != 3) {
            return false;
        }
        request.client_id = UnescapeField(parts[0]);
        request.jwt_token = UnescapeField(parts[1]);
        request.payload = UnescapeField(parts[2]);
        return true;
    }

    bool DeserializeResponse(const std::string &wire, ResponseEnvelope &response) {
        const auto parts = SplitByTab(wire);
        if (parts.size() != 2) {
            return false;
        }
        try {
            response.status_code = std::stoi(parts[0]);
        } catch (...) {
            return false;
        }
        response.payload = UnescapeField(parts[1]);
        return true;
    }

} // namespace dbms::network
