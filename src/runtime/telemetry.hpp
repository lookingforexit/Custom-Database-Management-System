#pragma once

// this file defines runtime telemetry snapshots and collection interfaces.
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>

namespace dbms::runtime {

    struct TelemetrySnapshot {
        double current_rps{0.0};
        double average_rps_10m{0.0};
        double max_rps_10m{0.0};
        double average_latency_10s_ms{0.0};
        std::uint64_t error_count_1m{0};
        double error_rate_1m{0.0};
        std::uint64_t requests_1m{0};
        std::uint64_t latency_samples_10s{0};
        double latency_sum_10s_ms{0.0};
        std::map<std::int64_t, std::uint64_t> requests_per_second_10m;
    };

    class TelemetryRegistry {
      public:
        void RecordSuccess(double latency_ms);
        void RecordFailure(double latency_ms);
        [[nodiscard]] TelemetrySnapshot Snapshot() const;

      private:
        struct RequestEvent {
            std::chrono::system_clock::time_point time;
            double latency_ms{0.0};
            bool failed{false};
        };

        void PruneLocked(std::chrono::system_clock::time_point now) const;

        mutable std::mutex mutex_;
        mutable std::deque<RequestEvent> events_;
    };

} // namespace dbms::runtime
