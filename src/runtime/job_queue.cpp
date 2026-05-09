#include "runtime/job_queue.hpp"

// this file will manage async request scheduling and dequeue behavior.
namespace dbms::runtime {

void JobQueue::Enqueue(JobRecord job) {
    jobs_.push(std::move(job));
}

std::optional<JobRecord> JobQueue::TryDequeue() {
    if (jobs_.empty()) {
        return std::nullopt;
    }

    JobRecord job = std::move(jobs_.front());
    jobs_.pop();
    return job;
}

}  // namespace dbms::runtime
