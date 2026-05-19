#include "common/uuid.hpp"

// this file implements guid generation helpers for async jobs and tracing.
#include <array>
#include <chrono>
#include <random>
#include <sstream>

namespace dbms::common {

    std::string UuidGenerator::NewGuidV4() {
        static thread_local std::mt19937_64 rng(
            static_cast<std::mt19937_64::result_type>(
                std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<std::uint32_t> distribution(0, 255);

        std::array<unsigned int, 16> bytes{};
        for (auto &value : bytes) {
            value = distribution(rng);
        }

        bytes[6] = (bytes[6] & 0x0F) | 0x40;
        bytes[8] = (bytes[8] & 0x3F) | 0x80;

        std::ostringstream output;
        output << std::hex;
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                output << "-";
            }
            if (bytes[index] < 16) {
                output << "0";
            }
            output << bytes[index];
        }
        return output.str();
    }

} // namespace dbms::common
