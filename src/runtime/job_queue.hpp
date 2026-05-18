#pragma once

// this file defines async job queue primitives for long-running requests.
#include <optional>
#include <queue>
#include <string>

namespace dbms::runtime {

    struct JobRecord {
        std::string job_id;
        std::string sql;
        std::string status;
    };

    class JobQueue {
      public:
        void Enqueue(JobRecord job);
        [[nodiscard]] std::optional<JobRecord> TryDequeue();

      private:
        std::queue<JobRecord> jobs_;
    };

} // namespace dbms::runtime
