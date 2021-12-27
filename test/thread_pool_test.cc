#include <gtest/gtest.h>
#include <memory>
#include <future>
#include "thread_pool.h"
#include <map>
using namespace kthreadpool;

auto func_sleep = [](std::chrono::nanoseconds ns) {std::this_thread::sleep_for(ns);};
auto func_noop = []()  {};

class ctpTest : public testing::Test
{
};

class testThreadPool : public ThreadPool
{
public:
    testThreadPool(ThreadPoolSettings set) : ThreadPool(set) {}

    bool IsStopped() 
    {
        return ThreadSize() == 0;
    }

    std::chrono::system_clock::time_point TimeAddDuration(std::chrono::system_clock::time_point tp, std::chrono::nanoseconds duration)
    {
        return ThreadPool::TimeAddDuration(tp, duration);
    }

    void testWorker()
    {
        auto wid = 100;
        auto ww = std::make_shared<Worker>(this, wid);
        thread_mutex_.lock();
        available_workers_size_++;
        workers_[ww->GetId()] = ww;
        thread_mutex_.unlock();

        ASSERT_EQ(true, ww->IsShutdown());
        ASSERT_EQ(wid, ww->GetId());

        ww->Start();

        std::condition_variable cv;
        std::mutex mutex;
        bool reach = false;
        
        auto fn = [&]() {
            std::unique_lock<std::mutex> lock(mutex);
            reach=true;
            cv.notify_all();
        };
        
        ScheduleTask(Context(), fn);
        {
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&]() {
                return reach;
            });
        }
        ASSERT_EQ(1, ThreadSize());

        ASSERT_EQ(true, ww->Shutdown());
        ww->Wait();

        ASSERT_EQ(0, ThreadSize());
        {
            std::unique_lock<std::mutex> lock(thread_mutex_);
            ASSERT_EQ(1, workers_.size());
        }
        GcIdleWorkers();
        {
            std::unique_lock<std::mutex> lock(thread_mutex_);
            ASSERT_EQ(0, workers_.size());
        }
    }
};


TEST_F(ctpTest, TestWorker)
{
    ThreadPoolSettings settings;
    auto idle = std::chrono::seconds(1);
    auto incr = std::chrono::milliseconds(10);
    settings.SetIdleThreadDuration(idle).SetMaxDelayQueueSize(2).SetMaxQueueSize(3).SetMaxThreadSize(4).SetMinThreadSize(0);
    settings.SetIncreaseThreadDuration(incr);

    testThreadPool tp(settings);
    tp.Start();

    tp.testWorker();
}


TEST_F(ctpTest, TestSettings)
{
    ThreadPoolSettings settings;
    auto idle = std::chrono::seconds(1);
    auto incr = std::chrono::milliseconds(1);
    settings.SetIdleThreadDuration(idle).SetMaxDelayQueueSize(2).SetMaxQueueSize(3).SetMaxThreadSize(4).SetMinThreadSize(5);
    settings.SetIncreaseThreadDuration(incr);

    ASSERT_EQ(idle, settings.IdleThreadDuration());
    ASSERT_EQ(incr, settings.IncreaseThreadDuration());
    ASSERT_EQ(2, settings.MaxDelayQueueSize());
    ASSERT_EQ(3, settings.MaxQueueSize());
    ASSERT_EQ(4, settings.MaxThreadSize());
    ASSERT_EQ(5, settings.MinThreadSize());
}

TEST_F(ctpTest, TestTimeAddDuration)
{
    ThreadPoolSettings settings;
    testThreadPool tp(settings);

    auto now = std::chrono::system_clock::now();
    auto max = std::chrono::system_clock::time_point::max();
    auto timeout = max - now;

    ASSERT_EQ(max, tp.TimeAddDuration(now, timeout + std::chrono::seconds(1)));
    ASSERT_EQ(max, tp.TimeAddDuration(now, timeout));
    ASSERT_EQ(now + std::chrono::seconds(2), tp.TimeAddDuration(now, std::chrono::seconds(2)));
}

TEST_F(ctpTest, TestThreadNormalCases)
{
    ThreadPoolSettings settings;
    auto idle = std::chrono::seconds(1);
    auto increase_duration = std::chrono::milliseconds(100);
    settings.SetIdleThreadDuration(idle).SetMaxDelayQueueSize(1).SetMaxQueueSize(1).SetMaxThreadSize(2).SetMinThreadSize(1);
    settings.SetIncreaseThreadDuration(increase_duration);

    auto delay_dur3 = std::chrono::seconds(3);
    auto sleep_dur1 = std::chrono::seconds(1);

    testThreadPool tp(settings);
    tp.Start();

    // use case : min thread size
    ASSERT_EQ(1, tp.ThreadSize());

    tp.ScheduleDelayTask(Context(), delay_dur3, func_noop);

    // use case : delay queue size
    ASSERT_EQ(1, tp.DelayQueueSize());

    // use case : thread is sleeping(queue is empty), 
    //            schedule a new task and wake the thread up
    auto ret1 = tp.ScheduleTask(Context(), func_sleep, sleep_dur1);   // 1 thread is running
    ASSERT_EQ(StatusCode::Ok, std::get<0>(ret1));
    ASSERT_EQ(1, tp.DelayQueueSize());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_EQ(0, tp.QueueSize());

    // use case : increase thread
    tp.ScheduleTask(Context(), func_sleep, sleep_dur1);   // 1 thread is running
    ASSERT_EQ(StatusCode::Ok, std::get<0>(ret1));
    tp.ScheduleTask(Context(), func_sleep, sleep_dur1);   // queue size
    ASSERT_EQ(StatusCode::Ok, std::get<0>(ret1));
    ASSERT_EQ(2, tp.ThreadSize());
    ASSERT_EQ(1, tp.QueueSize());
    ASSERT_EQ(1, tp.DelayQueueSize());

    // use case : ScheduleTask with timeout
    std::async([&]() {
        ret1 = tp.ScheduleTask(Context::WithTimeout(Context(), sleep_dur1), func_noop);
        ASSERT_EQ(StatusCode::Ok, std::get<0>(ret1));
    });
    ret1 = tp.ScheduleTask(Context::WithTimeout(Context(), increase_duration), func_sleep, sleep_dur1); 
    ASSERT_EQ(StatusCode::Timeout, std::get<0>(ret1));

    // use case : Wait() function
    tp.Wait();

    // use case : descrease idle thread
    std::this_thread::sleep_for(idle + std::chrono::milliseconds(20));
    ASSERT_EQ(1, tp.ThreadSize());

    tp.Stop();
    ASSERT_EQ(0, tp.QueueSize());
    ASSERT_EQ(0, tp.DelayQueueSize());
    ASSERT_TRUE(tp.IsStopped());
}


TEST_F(ctpTest, TestThreadDynamicScale)
{
    ThreadPoolSettings settings;
    auto idle = std::chrono::milliseconds(200);
    auto increase_duration = std::chrono::milliseconds(50);
    settings.SetIdleThreadDuration(idle).SetMaxDelayQueueSize(1).SetMaxQueueSize(1).SetMaxThreadSize(4).SetMinThreadSize(0);
    settings.SetIncreaseThreadDuration(increase_duration);

    testThreadPool tp(settings);
    tp.Start();

    auto second1 = std::chrono::seconds(1);

    auto scale_func = [&](){
        // 4 second(parallel) + 1 second
        for (size_t i = 0; i < 5; i++)
        {
            auto ret1 = tp.ScheduleTask(Context(), func_sleep, second1);
            ASSERT_EQ(StatusCode::Ok, std::get<0>(ret1));
        }
        
        // waiting all threads to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        ASSERT_EQ(4, tp.ThreadSize());
        
        std::this_thread::sleep_for(second1 * 2 + idle);

        ASSERT_EQ(0, tp.ThreadSize());
    };

    scale_func();
    scale_func();
    scale_func();

    // use case : Wait() function
    tp.Wait();

    tp.Stop();
    ASSERT_EQ(0, tp.QueueSize());
    ASSERT_EQ(0, tp.DelayQueueSize());
    ASSERT_TRUE(tp.IsStopped());
}


TEST_F(ctpTest, TestThreadValue)
{
    ThreadPoolSettings settings;
    auto idle = std::chrono::milliseconds(200);
    auto increase_duration = std::chrono::milliseconds(50);
    settings.SetIdleThreadDuration(idle).SetMaxDelayQueueSize(1).SetMaxQueueSize(1).SetMaxThreadSize(4).SetMinThreadSize(0);
    settings.SetIncreaseThreadDuration(increase_duration);

    testThreadPool tp(settings);
    tp.Start();

    auto plus_func = [=](int a, int b) -> int {
        std::this_thread::sleep_for(increase_duration);
        return a + b;
    };

    std::vector<int> args;
    std::map<int, std::future<int>> rets;
    for (int i = 0; i < 4; i++)
    {
        args.push_back(i);
        auto ret = tp.ScheduleTask(Context(), plus_func, i, i);
        ASSERT_EQ(StatusCode::Ok,std::get<0>(ret));
        rets[i] = (std::move(std::get<1>(ret)));
    }
    
    for (auto& it : rets)
    {
        int ret = it.second.get();
        ASSERT_EQ(it.first + it.first, ret);
    }

    // use case : Wait() function
    tp.Wait();

    tp.Stop();
    ASSERT_EQ(0, tp.QueueSize());
    ASSERT_EQ(0, tp.DelayQueueSize());
    ASSERT_TRUE(tp.IsStopped());
}
