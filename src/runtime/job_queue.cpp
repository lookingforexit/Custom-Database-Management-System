#include "runtime/job_queue.hpp"

// this file implements async request scheduling and dequeue behavior.
namespace dbms::runtime {

    JobQueue::JobQueue() = default;

    JobQueue::~JobQueue() { Stop(); }

    void JobQueue::Start(JobExecutor executor) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
        executor_ = std::move(executor);
        running_ = true;
        worker_ = std::thread(&JobQueue::WorkerLoop, this);
    }

    void JobQueue::Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
            running_ = false;
        }
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void JobQueue::Enqueue(JobRecord job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job.status = "QUEUED";
            records_[job.job_id] = job;
            jobs_.push(std::move(job));
        }
        condition_.notify_one();
    }

    std::optional<JobRecord> JobQueue::Find(const std::string &job_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = records_.find(job_id);
        if (it == records_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<JobRecord> JobQueue::TryDequeue() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (jobs_.empty()) {
            return std::nullopt;
        }

        JobRecord job = std::move(jobs_.front());
        jobs_.pop();
        return job;
    }

    void JobQueue::WorkerLoop() {
        while (true) {
            JobRecord current;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&]() { return !running_ || !jobs_.empty(); });
                if (!running_ && jobs_.empty()) {
                    return;
                }
                current = std::move(jobs_.front());
                jobs_.pop();
                auto it = records_.find(current.job_id);
                if (it != records_.end()) {
                    it->second.status = "RUNNING";
                }
            }

            int result_code = 500;
            std::string result_payload =
                "type=INTERNAL_ERROR code=9 message=async executor unavailable sql=\"\"";
            if (executor_) {
                auto result = executor_(current.sql);
                result_code = result.first;
                result_payload = std::move(result.second);
            }

            std::lock_guard<std::mutex> lock(mutex_);
            auto it = records_.find(current.job_id);
            if (it != records_.end()) {
                it->second.result_code = result_code;
                it->second.result_payload = std::move(result_payload);
                it->second.status = (result_code == 200) ? "DONE" : "FAILED";
            }
        }
    }

} // namespace dbms::runtime
