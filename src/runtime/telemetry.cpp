#include "runtime/telemetry.hpp"

// this file implements rolling rps, latency, and error-rate telemetry stubs.
namespace dbms::runtime {

    namespace {
        constexpr auto kWindow10Minutes = std::chrono::minutes(10);
        constexpr auto kWindow1Minute = std::chrono::minutes(1);
        constexpr auto kWindow10Seconds = std::chrono::seconds(10);
        constexpr auto kWindow1Second = std::chrono::seconds(1);
    }

    void TelemetryRegistry::RecordSuccess(double latency_ms) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        PruneLocked(now);
        events_.push_back({.time = now, .latency_ms = latency_ms, .failed = false});
    }

    void TelemetryRegistry::RecordFailure(double latency_ms) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        PruneLocked(now);
        events_.push_back({.time = now, .latency_ms = latency_ms, .failed = true});
    }

    TelemetrySnapshot TelemetryRegistry::Snapshot() const {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        PruneLocked(now);

        TelemetrySnapshot snapshot{};
        if (events_.empty()) {
            return snapshot;
        }

        std::uint64_t requests_last_second = 0;
        std::uint64_t errors_last_minute = 0;
        double latency_sum_10s = 0.0;
        std::uint64_t latency_count_10s = 0;

        for (const auto &event : events_) {
            const auto age = now - event.time;
            if (age <= kWindow1Second) {
                ++requests_last_second;
            }
            if (age <= kWindow1Minute && event.failed) {
                ++errors_last_minute;
            }
            if (age <= kWindow10Seconds) {
                latency_sum_10s += event.latency_ms;
                ++latency_count_10s;
            }
        }

        const auto total_requests = static_cast<double>(events_.size());
        const auto span =
            std::chrono::duration_cast<std::chrono::duration<double>>(
                events_.back().time - events_.front().time)
                .count();

        snapshot.current_rps = static_cast<double>(requests_last_second);
        snapshot.average_rps_10m = span > 0.0 ? total_requests / span : total_requests;
        snapshot.max_rps_10m = snapshot.current_rps;
        snapshot.average_latency_10s_ms =
            latency_count_10s > 0 ? (latency_sum_10s / latency_count_10s) : 0.0;
        snapshot.error_rate_1m = errors_last_minute;
        return snapshot;
    }

    void TelemetryRegistry::PruneLocked(
        std::chrono::steady_clock::time_point now) const {
        while (!events_.empty() && now - events_.front().time > kWindow10Minutes) {
            events_.pop_front();
        }
    }

} // namespace dbms::runtime
