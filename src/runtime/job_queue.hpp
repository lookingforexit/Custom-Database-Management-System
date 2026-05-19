#pragma once

// this file defines async job queue primitives for long-running requests.
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace dbms::runtime {

    struct JobRecord {
        std::string job_id;
        std::string sql;
        std::string status;
        std::string result_payload;
        int result_code{0};
    };

    class JobQueue {
      public:
        using JobExecutor = std::function<std::pair<int, std::string>(const std::string &)>;

        JobQueue();
        ~JobQueue();

        void Start(JobExecutor executor);
        void Stop();
        void Enqueue(JobRecord job);
        [[nodiscard]] std::optional<JobRecord> Find(const std::string &job_id) const;
        [[nodiscard]] std::optional<JobRecord> TryDequeue();

      private:
        void WorkerLoop();

        mutable std::mutex mutex_;
        std::condition_variable condition_;
        std::queue<JobRecord> jobs_;
        std::unordered_map<std::string, JobRecord> records_;
        JobExecutor executor_;
        bool running_{false};
        std::thread worker_;
    };

} // namespace dbms::runtime
