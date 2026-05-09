#include "runtime/telemetry.hpp"

// this file will compute rolling rps, latency, and error-rate metrics.
namespace dbms::runtime {

void TelemetryRegistry::RecordSuccess(double) {}

void TelemetryRegistry::RecordFailure(double) {}

TelemetrySnapshot TelemetryRegistry::Snapshot() const {
    return {};
}

}  // namespace dbms::runtime
