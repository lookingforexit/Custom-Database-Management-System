#pragma once

// this file defines runtime telemetry snapshots and collection interfaces.
#include <cstdint>

namespace dbms::runtime {

    struct TelemetrySnapshot {
        double current_rps{0.0};
        double average_rps_10m{0.0};
        double max_rps_10m{0.0};
        double average_latency_10s_ms{0.0};
        std::uint64_t error_rate_1m{0};
    };

    class TelemetryRegistry {
      public:
        void RecordSuccess(double latency_ms);
        void RecordFailure(double latency_ms);
        [[nodiscard]] TelemetrySnapshot Snapshot() const;
    };

} // namespace dbms::runtime
