#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <queue>
#include <unordered_map>
#include <future>
#include <tuple>
namespace kthreadpool {

	class Context
	{
	public:
		Context() {timeout_ = std::chrono::seconds::max();}

		static Context WithTimeout(const Context& ctx = Context(), std::chrono::nanoseconds t = std::chrono::nanoseconds::max())
		{
			Context ctx2 = ctx;
			ctx2.timeout_ = t;
			return ctx2;
		}

		std::chrono::nanoseconds timeout() const {return timeout_;}
	private:
		std::chrono::nanoseconds timeout_;
	};

	class ThreadPoolSettings
	{
	public:
		// Default value is 1
		ThreadPoolSettings& SetMinThreadSize(size_t t)
		{
			min_thread_size_ = t;
			return *this;
		}
		// Default value is the number of CPU cores
		ThreadPoolSettings& SetMaxThreadSize(size_t t)
		{
			max_thread_size_ = t;
			return *this;
		}
		// Default value is 1000
		ThreadPoolSettings& SetMaxQueueSize(size_t t)
		{
			max_queue_size_ = t;
			return *this;
		}
		// Default value is 1000
		ThreadPoolSettings& SetMaxDelayQueueSize(size_t t)
		{
			max_delay_queue_size_ = t;
			return *this;
		}

		// Delete idle threads which are waiting for task more than idle_duration seconds.
		// Default value is 10 seconds.
		ThreadPoolSettings& SetIdleThreadDuration(std::chrono::nanoseconds idle_duration)
		{
			idle_duration_ = idle_duration;
			return *this;
		}

		// If queue is full and waits to enqueue more than this duration value, create a new thread when thread number is less `SetMaxThreadSize`
		// Default value is 300 milli seconds
		ThreadPoolSettings& SetIncreaseThreadDuration(std::chrono::nanoseconds duration)
		{
			increase_thread_duration_ = duration;
			return *this;
		}

		size_t MinThreadSize() {return min_thread_size_;}
		size_t MaxThreadSize() {return max_thread_size_;}
		size_t MaxQueueSize() {return max_queue_size_;}
		size_t MaxDelayQueueSize() {return max_delay_queue_size_;}
		std::chrono::nanoseconds IdleThreadDuration() {return idle_duration_;}
		std::chrono::nanoseconds IncreaseThreadDuration() {return increase_thread_duration_;}


	private:
		size_t min_thread_size_ = 1;
		size_t max_thread_size_ = std::thread::hardware_concurrency();
		size_t max_queue_size_ = 1000;
		size_t max_delay_queue_size_ = 1000;
		std::chrono::nanoseconds idle_duration_ = std::chrono::seconds(10);
		std::chrono::nanoseconds increase_thread_duration_ = std::chrono::milliseconds(300);

		friend class ThreadPool;
	};

	enum StatusCode
	{
		Ok = 1,
		Timeout = -0x2,
		Closed = -0x3,
	};

	class ThreadPool {

	protected:
		class Worker;
		class Task;
		class TimeTask;
		typedef std::function<void()> TaskFunc;

	public:
		
		ThreadPool(ThreadPoolSettings settings);
		~ThreadPool();

		ThreadPool(const ThreadPool &) = delete;
		ThreadPool(ThreadPool &&) = delete;
		ThreadPool & operator=(const ThreadPool &) = delete;
		ThreadPool & operator=(ThreadPool &&) = delete;

		// Start : start worker threads
		void Start();

		// Stop : stop all worker threads and waiting to complete ongoing tasks.
		void Stop();

		// Wait : waiting to complete all tasks
		void Wait();

		// ScheduleTask : schedule a task to thread pool.
		// return : std::tuple<StatusCode, std::future<typename std::result_of<F(Args...)>::type>>
		//     Result : status code. 
		//     std::future : returned value from function F
		template<class F, class... Args>
		auto ScheduleTask(const Context& ctx, F&& f, Args&&... args) 
			-> std::tuple<StatusCode, std::future<typename std::result_of<F(Args...)>::type>>
		{
			using return_type = typename std::result_of<F(Args...)>::type;

			auto task = std::make_shared<std::packaged_task<return_type()> >(
				std::bind(std::forward<F>(f), std::forward<Args>(args)...)
			);
				
			std::future<return_type> res = task->get_future();
			auto ret = scheduleTask([task](){(*task)();}, ctx.timeout());
			return std::make_tuple(ret, std::move(res));
		}

		// ScheduleDelayTask : schedule a delay task to thread pool.
		template<class D, class F, class... Args>
		auto ScheduleDelayTask(const Context& ctx, D delay_duration, F&& f, Args&&... args) 
			-> std::tuple<StatusCode, std::future<typename std::result_of<F(Args...)>::type>>
		{
			using return_type = typename std::result_of<F(Args...)>::type;

			auto task = std::make_shared<std::packaged_task<return_type()> >(
				std::bind(std::forward<F>(f), std::forward<Args>(args)...)
			);
				
			std::future<return_type> res = task->get_future();
			auto ret = scheduleDelayTask([task](){(*task)();}, delay_duration, ctx.timeout());
			return std::make_tuple(ret, std::move(res));
		}

		// the number of runnable threads
		size_t ThreadSize();
		size_t QueueSize();
		size_t DelayQueueSize();

	protected:

		StatusCode scheduleTask(const TaskFunc& t, std::chrono::nanoseconds timeout = std::chrono::nanoseconds::max());
		StatusCode scheduleDelayTask(const TaskFunc& t, std::chrono::nanoseconds delay_duration, std::chrono::nanoseconds timeout = std::chrono::nanoseconds::max());
		
		void Run(Worker* w);
		bool IsShutdown() { return shutdown_; }
		bool Shutdown();

		void TryIncreaseThreads();

		void Execute(const Task& task);

		std::chrono::system_clock::time_point TimeAddDuration(std::chrono::system_clock::time_point tp, std::chrono::nanoseconds duration);
		void GcIdleWorkers();
		void GcWorkers();
	
	protected:
		
		ThreadPoolSettings settings_;

		std::mutex thread_mutex_;
		std::unordered_map<int64_t, std::shared_ptr<Worker>> workers_;
		std::atomic_uint available_workers_size_;
		std::atomic<int64_t> thread_id_generator_;

		std::mutex queue_mutex_;
		std::queue<Task> queue_;
		std::priority_queue<TimeTask> delay_queue_;
		std::condition_variable in_queue_cond_;
		std::condition_variable out_queue_cond_;

		std::condition_variable wait_cond_;
		std::atomic_bool shutdown_;

		const std::chrono::system_clock::time_point max_system_clock_ = std::chrono::time_point<std::chrono::system_clock>::max();
	
	protected:
		class Task
		{
		public:
			Task(TaskFunc f) : func(f){}
			bool operator()() const {
				return func != nullptr;
			}
			TaskFunc func = nullptr;
		};

		class TimeTask : public Task
		{
		public:
			std::chrono::system_clock::time_point exec_time;

			TimeTask(TaskFunc f, std::chrono::system_clock::time_point tp) :
			Task(f), exec_time(tp) {} 

			bool operator<(const TimeTask& task) const {
				return exec_time < task.exec_time;
			}
		};

		class Worker
		{
		public:
			Worker(ThreadPool *p, int64_t id);
			~Worker();
			void Start();
			bool Shutdown();
			void Wait();

			int64_t GetId() const;
			bool IsShutdown();

			Worker(const Worker&) = delete;
			const Worker& operator=(const Worker&) = delete;
			Worker(Worker&) = delete;
			const Worker& operator=(Worker&) = delete;
		private:
			void Run();

			ThreadPool* pool_;
			std::shared_ptr<std::thread> thread_;
			std::atomic_bool shutdown_;

			int64_t id_;
		};
	};
}  // namespace 

