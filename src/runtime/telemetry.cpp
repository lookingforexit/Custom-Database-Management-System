#include "runtime/telemetry.hpp"

// this file implements rolling rps, latency, and error-rate telemetry stubs.
namespace dbms::runtime {

  void TelemetryRegistry::RecordSuccess(double) {}

  void TelemetryRegistry::RecordFailure(double) {}

  TelemetrySnapshot TelemetryRegistry::Snapshot() const { return {}; }

} // namespace dbms::runtime
