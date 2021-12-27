#include <chrono>
#include <iostream>
#include "thread_pool.h"

using namespace kthreadpool;
using namespace std;


int main(int argc, char* argv[])
{
    auto test_func = [](int a) -> int {
        return a * a;
    };

    ThreadPool tp(ThreadPoolSettings().SetMaxQueueSize(2).SetMaxDelayQueueSize(2));
    tp.Start();

    auto resp = tp.ScheduleTask(Context::WithTimeout(Context(), std::chrono::seconds(1)), test_func, 2);
    if (std::get<0>(resp) != StatusCode::Ok)
        return 1;
    cout << "2 * 2 = " << std::get<1>(resp).get() << endl;
    
    auto delay_resp = tp.ScheduleDelayTask(Context(), std::chrono::seconds(1), test_func, 3);
    if (std::get<0>(resp) != StatusCode::Ok)
        return 1;
    cout << "3 * 3 = " << std::get<1>(delay_resp).get() << endl;

    tp.Wait();
    tp.Stop();
}