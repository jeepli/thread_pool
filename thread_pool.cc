#include "thread_pool.h"
#include <assert.h>
#include <iostream>
#include <sstream>

namespace kThreadPool {

	void Worker::Start()
	{
		if (!start_)
			thread_ = new std::thread(std::bind(&Worker::run, this));
		start_ = true;
	}

	void Worker::Stop()
	{
		if (start_)
		{
			start_ = false;
			thread_->join();
			thread_ = nullptr;
		}
	}

	void Worker::Run()
	{
		pool_->Run(this);
	}

	int64_t Worker::GetId() const
	{
		return id_;
	}


	// ThreadPool
	ThreadPool::ThreadPool(int min_threads_size, int max_threads_size)
		: min_thread_size_(min_threads_size),
		max_threads_size_(max_threads_size),
		is_started_(false), thread_size_(0) 
	{
	}

	ThreadPool::~ThreadPool() 
	{
		if (is_started_) {
			Stop();
		}
	}

	void ThreadPool::Start() 
	{
		if (IsStarted())
			return;

		assert(threads_.empty());
		is_started_ = true;
		for (int i = 0; i < threads_size_; ++i) 
		{
			auto id = ++thread_id_generator_;
			auto w = std::make_shared<Worker>(this, id);
			workers_[w.GetId()] = w;
			w->Start();
		}
	}

	void ThreadPool::stop() 
	{
		if (!IsStarted())
			return;

		{   // scope block : avoid deadlock
			is_started_ = false;
			std::unique_lock<std::mutex> lock(mutex_);
			in_queue_cond_.notify_all();
			out_queue_cond.notify_all();
		}
		
		std::unique_lock<std::mutex> lock(thread_mutex_);
		for (auto& it : workers_)
		{
			it.second->Stop();
		}
		workers_.clear();
	}

	void ThreadPool::Schedule(const TaskFunc& func, void* arg)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		while (queue_.size() >= max_queue_size_ && IsStarted())
		{
			out_queue_cond.wait(&lock);
		}

		if (IsStarted())
		{
			queue_.emplace_back(func, arg);
			in_queue_cond_.notify_one();
		}
	}

	// @TODO expand thread
	void ScheduleDelay(const TaskFunc& func, void* arg, std::chrono::milliseconds timeout)
	{
		auto exec_time = std::chrono::system_clock::now() + timeout;
		
		std::unique_lock<std::mutex> lock(mutex_);
		if (IsStarted())
		{
			time_queue_.emplace_back(func, arg, exec_time);
			in_queue_cond_.notify_one();
		}
	}

	bool ThreadPool::CanShrinkIdle()
	{
		return (thread_size_ > min_thread_size_)
	}

	void ThreadPool::ShrinkIdle(Worker* worker)
	{
		auto id = worker->GetId();

		std::unique_lock<std::mutex> lock(thread_mutex_);
		auto it = workers_.find(id);
		if (workers_.end() == it)
			return;
		
		it.second->Stop();
		workers_.erase(it);
		thread_size_--;
	}

	void ThreadPool::Run(Worker* worker)
	{
		while (IsStarted()) 
		{
			mutex_.lock();
			if (queue_.empty() && time_queue_.empty() && IsStarted())
			{
				in_queue_cond_.wait_for(lock, idle_duration_);
			}
			if (!IsStarted())
			{
				mutex_.unlock();
				return;
			}
			
			if (queue_.empty() && time_queue_.empty())
			{
				if (CanShrinkIdle())
				{
					ShrinkIdle(worker);
				}
				
				mutex_.unlock();
				return;
			}

			if (!time_queue_.empty())
			{
				auto task = time_queue_.top();
				auto now = std::chrono::system_clock::now();
				if (task.exec_time < now)
				{
					time_queue_.pop();
					mutex_.unlock();
					
					(*task.func)(task.arg);
					continue;
				} 
				if (queue_.empty())
				{
					auto wait = task.exec_time - now;
					in_queue_cond_.wait_for(lock, wait, [](){
						return !queue_.empty();
					});

					mutex_.unlock();
					continue;
				}
			}
			if (!queue_.empty())
			{
				auto task = queue_.pop();
				out_queue_cond_.notify_one();
				mutex_.unlock();
				(*task.func)(task.arg);
			}
		}
	}
}  // namespace kThreadPool
