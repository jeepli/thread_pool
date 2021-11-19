#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <priority_queue>
#include <unordered_map>

namespace kThreadPool {

	class ThreadPool {

	private:
		typedef std::function<void(void*)> TaskFunc;

		class Task
		{
		public:
			Task(TaskFunc f, void* p) : func(f), arg(p) {}
			bool operator()() const {
				return func != nullptr;
			}
			TaskFunc func = nullptr;
			void* arg = nullptr;
		};

		class TimeTask : public Task
		{
		public:
			std::chrono::system_clock::time_point exec_time;

			TimeTask(TaskFunc f, void* p, std::chrono::system_clock::time_point tp) :
			Task(f, p), exec_time(tp) {} 

			bool operator<(const TimeTask& task) const {
				return exec_time < task.exec_time;
			}
		};

		class Worker
		{
		public:
			Worker(ThreadPool *p, int64_t id) : pool_(p), id_(id) {}
			
			void Start();
			void Stop();
			int64_t GetId() const;

		private:
			void Run();
			Worker(const Worker&);
			const Worker& operator=(const Worker&);

			ThreadPool *pool_;
			std::thread *thread_ = nullptr;
			std::atomic_bool start_ = false;

			int64_t id_ = 0;
		}

	public:
		
		ThreadPool(int min_threads_size = 1, int max_threads_size=2);
		~ThreadPool();

		void Start();
		void Stop();
		void Schedule(const TaskFunc&t, void* p);
		void ScheduleDelay(const TaskFunc&t, void* p, std::chrono::milliseconds timeout);

		ThreadPool(const ThreadPool&) = delete;
		const ThreadPool& operator=(const ThreadPool&) = delete;

	private:
		
		void Run(Worker* w);
		bool IsStarted() { return is_started_; }

		bool CanShrinkIdle();
		void ShrinkIdle(Worker* worker);

		typedef std::unordered_map<int64_t, std::shared_ptr<Worker>> Workers;
		typedef std::queue<Task> Tasks;
		
		std::atomic_int thread_size_;
		int min_thread_size_;
		int max_thread_size_;
		std::mutex thread_mutex_;
		std::chrono::seconds idle_duration_ = std::chrono::seconds(10);

		Workers workers_;
		Tasks queue_;
		std::priority_queue<TimeTask> time_queue_;

		std::mutex mutex_;
		std::condition_variable in_queue_cond_;
		std::condition_variable out_queue_cond_;
		std::atomic_bool is_started_;

		std::atomic<int64_t> thread_id_generator_ = 0;
	};

}  // namespace 

