#include "runtime/telemetry.hpp"

// this file implements rolling telemetry windows that can also be merged
// across storage nodes through per-second request buckets.
namespace dbms::runtime {

    namespace {
        constexpr auto kWindow10Minutes = std::chrono::minutes(10);
        constexpr auto kWindow1Minute = std::chrono::minutes(1);
        constexpr auto kWindow10Seconds = std::chrono::seconds(10);
        constexpr auto kWindow1Second = std::chrono::seconds(1);
    }

    void TelemetryRegistry::RecordSuccess(double latency_ms) {
        const auto now = std::chrono::system_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        PruneLocked(now);
        events_.push_back({.time = now, .latency_ms = latency_ms, .failed = false});
    }

    void TelemetryRegistry::RecordFailure(double latency_ms) {
        const auto now = std::chrono::system_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        PruneLocked(now);
        events_.push_back({.time = now, .latency_ms = latency_ms, .failed = true});
    }

    TelemetrySnapshot TelemetryRegistry::Snapshot() const {
        const auto now = std::chrono::system_clock::now();
        std::lock_guard<std::mutex> lock(mutex_);
        PruneLocked(now);

        TelemetrySnapshot snapshot{};
        if (events_.empty()) {
            return snapshot;
        }

        for (const auto &event : events_) {
            const auto age = now - event.time;
            const auto second_key =
                std::chrono::duration_cast<std::chrono::seconds>(
                    event.time.time_since_epoch())
                    .count();
            ++snapshot.requests_per_second_10m[second_key];
            if (age <= kWindow1Minute) {
                ++snapshot.requests_1m;
                if (event.failed) {
                    ++snapshot.error_count_1m;
                }
            }
            if (age <= kWindow10Seconds) {
                snapshot.latency_sum_10s_ms += event.latency_ms;
                ++snapshot.latency_samples_10s;
            }
        }

        const auto current_second_key =
            std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch())
                .count();
        snapshot.current_rps = static_cast<double>(
            snapshot.requests_per_second_10m[current_second_key]);

        std::uint64_t total_requests_10m = 0;
        std::uint64_t max_bucket = 0;
        for (const auto &[second_key, count] : snapshot.requests_per_second_10m) {
            (void)second_key;
            total_requests_10m += count;
            max_bucket = std::max(max_bucket, count);
        }
        snapshot.average_rps_10m =
            static_cast<double>(total_requests_10m) / 600.0;
        snapshot.max_rps_10m = static_cast<double>(max_bucket);
        snapshot.average_latency_10s_ms =
            snapshot.latency_samples_10s == 0
                ? 0.0
                : (snapshot.latency_sum_10s_ms /
                   static_cast<double>(snapshot.latency_samples_10s));
        snapshot.error_rate_1m =
            snapshot.requests_1m == 0
                ? 0.0
                : (static_cast<double>(snapshot.error_count_1m) /
                   static_cast<double>(snapshot.requests_1m));
        return snapshot;
    }

    void TelemetryRegistry::PruneLocked(
        std::chrono::system_clock::time_point now) const {
        while (!events_.empty() && now - events_.front().time > kWindow10Minutes) {
            events_.pop_front();
        }
    }

} // namespace dbms::runtime
