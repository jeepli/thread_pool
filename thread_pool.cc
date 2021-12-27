#include "thread_pool.h"
#include <assert.h>
#include <iostream>
#include <sstream>
#include <functional>
#include <cassert>

namespace kthreadpool {

	ThreadPool::Worker::Worker(ThreadPool *p, int64_t id) : pool_(p), id_(id) 
	{
		shutdown_ = true;
	}

	ThreadPool::Worker::~Worker()
	{
	}

	void ThreadPool::Worker::Start()
	{
		bool expect = true;
		if (shutdown_.compare_exchange_strong(expect, false))
			thread_ = std::make_shared<std::thread>(std::thread(std::bind(&Worker::Run, this)));
	}

	bool ThreadPool::Worker::Shutdown()
	{
		bool expect = false;
		return shutdown_.compare_exchange_strong(expect, true);
	}

	void ThreadPool::Worker::Wait()
	{
		if (thread_->joinable())
			thread_->join();
	}

	bool ThreadPool::Worker::IsShutdown()
	{
		return shutdown_;
	}

	void ThreadPool::Worker::Run()
	{
		pool_->Run(this);
	}

	int64_t ThreadPool::Worker::GetId() const
	{
		return id_;
	}

	// ThreadPool
	ThreadPool::ThreadPool(ThreadPoolSettings settings)
		: settings_(settings), available_workers_size_(0), 
		thread_id_generator_(0),
		shutdown_(true)
	{
	}

	ThreadPool::~ThreadPool() 
	{
		Stop();
	}

	bool ThreadPool::Shutdown() 
	{
		bool expect = false;
		return shutdown_.compare_exchange_strong(expect, true);
	}

	void ThreadPool::Start() 
	{
		std::unique_lock<std::mutex> lock(thread_mutex_);

		if (!IsShutdown())
			return;

		assert(workers_.empty());
		shutdown_ = false;

		for (size_t i = 0; i < settings_.MinThreadSize(); ++i) 
		{
			available_workers_size_++;
			auto id = ++thread_id_generator_;
			auto w = std::make_shared<Worker>(this, id);
			workers_[w->GetId()] = w;
			w->Start();
		}
	}

	void ThreadPool::Stop() 
	{
		if (!Shutdown())
			return;

		{   // avoid deadlock
			std::unique_lock<std::mutex> lock(queue_mutex_);
			in_queue_cond_.notify_all();
			out_queue_cond_.notify_all();
		}
		
		GcWorkers();
	}

	void ThreadPool::Wait()
	{
		std::unique_lock<std::mutex> lock(queue_mutex_);
		wait_cond_.wait(lock, [&](){
			return queue_.empty() && delay_queue_.empty();
		});
	}

	std::chrono::system_clock::time_point ThreadPool::TimeAddDuration(std::chrono::system_clock::time_point tp, std::chrono::nanoseconds duration)
	{
		if (max_system_clock_ - duration > tp)
			tp = tp + duration; 
		else
			tp = max_system_clock_;
		return tp;
	}

	size_t ThreadPool::ThreadSize()
	{
		return available_workers_size_;
	}

	StatusCode ThreadPool::scheduleTask(const TaskFunc& func, std::chrono::nanoseconds timeout)
	{
		auto now = std::chrono::system_clock::now();
		auto expire_time = TimeAddDuration(now, timeout);
		auto timeout_time = expire_time;

		std::unique_lock<std::mutex> lock(queue_mutex_);
		while (queue_.size() >= settings_.MaxQueueSize() && !IsShutdown())
		{
			if (available_workers_size_ < settings_.max_thread_size_)
				expire_time = std::min(expire_time, TimeAddDuration(std::chrono::system_clock::now(), settings_.increase_thread_duration_));

			if (out_queue_cond_.wait_until(lock, expire_time) == std::cv_status::timeout)
			{
				if (std::chrono::system_clock::now() >= timeout_time)
					return StatusCode::Timeout;
				TryIncreaseThreads();
				GcIdleWorkers();
			}
		}

		if (IsShutdown())
			return StatusCode::Closed;

		queue_.push(Task(func));
		in_queue_cond_.notify_one();
		
		return StatusCode::Ok;
	}

	StatusCode ThreadPool::scheduleDelayTask(const TaskFunc& func, std::chrono::nanoseconds delay_duration, std::chrono::nanoseconds timeout)
	{
		auto now = std::chrono::system_clock::now();
		auto expire_time = TimeAddDuration(now, timeout);
		auto timeout_time = expire_time;

		std::unique_lock<std::mutex> lock(queue_mutex_);
		while (delay_queue_.size() >= settings_.MaxDelayQueueSize() && !IsShutdown())
		{
			if (available_workers_size_ < settings_.max_thread_size_)
				expire_time = std::min(expire_time, TimeAddDuration(std::chrono::system_clock::now(), settings_.increase_thread_duration_));

			if (out_queue_cond_.wait_until(lock, expire_time) == std::cv_status::timeout)
			{
				if (std::chrono::system_clock::now() >= timeout_time)
					return StatusCode::Timeout;
				TryIncreaseThreads();
				GcIdleWorkers();
			}
		}

		if (IsShutdown())
			return StatusCode::Closed;

		auto exec_time = std::chrono::system_clock::now() + delay_duration;
		delay_queue_.push(TimeTask(func, exec_time));
		in_queue_cond_.notify_one();
		
		return StatusCode::Ok;
	}

	size_t ThreadPool::QueueSize()
	{
		std::unique_lock<std::mutex> lock(queue_mutex_);
		return queue_.size();
	}
	
	size_t ThreadPool::DelayQueueSize()
	{
		std::unique_lock<std::mutex> lock(queue_mutex_);
		return delay_queue_.size();
	}

	void ThreadPool::TryIncreaseThreads()
	{
		std::unique_lock<std::mutex> lock(thread_mutex_);
		if (workers_.size() >= settings_.MaxThreadSize())
			return;
		available_workers_size_++;
		auto id = ++thread_id_generator_;
		auto w = std::make_shared<Worker>(this, id);
		workers_[w->GetId()] = w;
		w->Start();
	}

	void ThreadPool::Execute(const Task& task)
	{
		(task.func)();
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			if (queue_.empty() && delay_queue_.empty())
				wait_cond_.notify_all();
		}
	}

	void ThreadPool::GcIdleWorkers()
	{
		std::vector<std::shared_ptr<Worker>> gc_workers;
		{
			std::unique_lock<std::mutex> lock(thread_mutex_);
			for (auto it = workers_.begin(); it != workers_.end();)
			{
				if (it->second->IsShutdown())
				{
					gc_workers.push_back(it->second);
					it = workers_.erase(it);
				} else {
					it++;
				}
			}
		}
		for (auto& w : gc_workers)
		{
			w->Wait();
		}
	}

	void ThreadPool::GcWorkers()
	{
		std::vector<std::shared_ptr<Worker>> gc_workers;
		{
			std::unique_lock<std::mutex> lock(thread_mutex_);
			for (auto it = workers_.begin(); it != workers_.end(); it++)
			{
				gc_workers.push_back(it->second);
				it->second->Shutdown();
			}
			workers_.clear();
		}
		for (auto& w : gc_workers)
		{
			w->Wait();
		}
	}

	void ThreadPool::Run(Worker* worker)
	{
		while (true) 
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);		
			std::cv_status cv_status = std::cv_status::no_timeout;	
			if (queue_.empty() && delay_queue_.empty() && !IsShutdown() && !worker->IsShutdown())
			{
				cv_status = in_queue_cond_.wait_for(lock, settings_.IdleThreadDuration());
			}
			if (IsShutdown() || worker->IsShutdown())
			{
				available_workers_size_--;
				break;
			}
				
			if (queue_.empty() && delay_queue_.empty() && cv_status == std::cv_status::timeout)
			{
				auto size = available_workers_size_.load();
				if (size > settings_.MinThreadSize())
				{
					if (available_workers_size_.compare_exchange_strong(size, size-1))
					{
						worker->Shutdown();
						break;
					}
				}
			}

			if (!delay_queue_.empty())
			{
				auto task = delay_queue_.top();
				auto now = std::chrono::system_clock::now();
				if (task.exec_time < now)
				{
					delay_queue_.pop();
					lock.unlock();
					
					Execute(task);
					continue;
				} 
				if (queue_.empty())
				{
					auto wait = task.exec_time - now;
					in_queue_cond_.wait_for(lock, wait, [&](){
						return !queue_.empty();
					});

					continue;
				}
			}
			if (!queue_.empty())
			{
				auto task = queue_.front();
				queue_.pop();
				out_queue_cond_.notify_one();
				lock.unlock();
				Execute(task);
			}
		}
	}
}  // namespace kThreadPool
