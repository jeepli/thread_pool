C++ thread pool



[TOC]


# Introduction

Lightweight, generic, pure C++11 thread pool.  

It supports 

- task : thread pool executes it ASAP
- delay task : thread pool execute it at a point in time
- increase or decrease threads dynamically



# Design



## APIs



### enum

```
enum StatusCode
{
    Ok = 1,             // 
    Timeout = -0x2,     // schedule timeout, 
    Closed = -0x3,      // thread pool is closed
};
```



### class




#### ThreadPoolSettings (configurations)


**SetMinThreadSize**

```
ThreadPoolSettings& SetMinThreadSize(size_t t)
```

Set minimum number of threads, default value is 1.


**SetMaxThreadSize**

```
ThreadPoolSettings& SetMaxThreadSize(size_t t)
```

Set maximum number of threads, default value is the number of CPU cores


**SetMaxQueueSize**

```
ThreadPoolSettings& SetMaxQueueSize(size_t t)
```

Set max queue size, if reach this value, `ScheduleTask` will be blocked. Default value is 1000.


**SetMaxDelayQueueSize**

```
ThreadPoolSettings& SetMaxDelayQueueSize(size_t t)
```

Set max delay queue size, if reach this value, `ScheduleDelayTask` will be blocked. Default value is 1000.


**SetIdleThreadDuration**

```
ThreadPoolSettings& SetIdleThreadDuration(std::chrono::nanoseconds idle_duration)
```

Set a maximum duration, if a thread waits a task more than this value, the thread will be destroy. 
Default value is 10 seconds.


**SetIncreaseThreadDuration**

```
ThreadPoolSettings& SetIncreaseThreadDuration(std::chrono::nanoseconds duration)
```

If queue is full and waits to enqueue more than this duration value, create a new thread when thread number is less `SetMaxThreadSize`.
Default value is 300 milliseconds.



#### **Context**

**WithTimeout**

```
static Context WithTimeout(const Context& ctx = Context(), std::chrono::nanoseconds t = std::chrono::nanoseconds::max())
```

Create a new context with timeout.





#### ThreadPool

**Start**

```
void Start();
```

Initliaze minimum number of threads.



**Stop**

```
void Stop();
```

Wait thread pool to complete all running tasks, the tasks which are in task queue will be discarded.


**Wait**

```
void Wait();
```

Wait to complete all tasks.



**ScheduleTask**

```
template<class F, class... Args>
auto ScheduleTask(const Context& ctx, F&& function, Args&&... args) 
        -> std::tuple<StatusCode, std::future<typename std::result_of<F(Args...)>::type>>
```

Push a task to queue and execute it as soon as possible.

- Context : 
  - `Context()`  : blocks until the task be queued
  - `Context::WithTimeout(Context(), duration_timeout)` : blocks until the task be queued or timeout
  - function 
  - args : variadic function arguments 


**ScheduleDelayTask**

```
template<class D, class F, class... Args>
auto ScheduleDelayTask(const Context& ctx, D delay_duration, F&& function, Args&&... args) 
        -> std::tuple<StatusCode, std::future<typename std::result_of<F(Args...)>::type>>
```

Push a delay task to delay queue and execute it at a point in time.

- Context
- delay_duration : executes the function after delay_duration
  - Function
  - args : variadic function arguments 


**ThreadSize**

```
size_t ThreadSize();
```

The number of runnable threads.


**QueueSize**

```
size_t QueueSize();
```

How many tasks in queue.


**DelayQueueSize**

```
size_t DelayQueueSize();
```

How many tasks in delay queue.





# Usage



## Usage



**1. create thread pool**


create a settings first

```
kthreadpool::ThreadPoolSettings settings;
settings.SetIdleThreadDuration(idle).SetMaxDelayQueueSize(2).SetMaxQueueSize(3).SetMaxThreadSize(4).SetMinThreadSize(1);
```


create thread pool

```
kthreadpool::ThreadPool tp(settings);
```


start 1 (settings.MinThreadSize()) thread

```
tp.Start();
```



**2. schedule task**


schedule a delay task

```
auto plus_func = [=](int a, int b) -> int {
    return a + b;
};

auto resp = tp.ScheduleDelayTask(Context::WithTimeout(Context(), std::chrono::seconds(1), plus_func, 1, 2);
auto status_code = std::get<0>(resp);
if (status_code != kthreadpool::StatusCode::Ok)
    error

// waiting until the future has a valid result
auto result = std::get<1>(resp).get();
if (result != 3)
    error
```


schedule a task

```
auto plus_func = [=](int a, int b) -> int {
    return a + b;
};

auto resp = tp.ScheduleTask(Context(), plus_func, 1, 2);
auto status_code = std::get<0>(resp);
if (status_code != kthreadpool::StatusCode::Ok)
    error

// waiting until the future has a valid result
auto result = std::get<1>(resp).get();
if (result != 3)
    error
```



**3. stop thread pool**

wait threads to complete all running tasks, the tasks which are in task queue will be discarded.

```
tp.Stop();
```

**wait all tasks to complete**

```
tp.Wait();
```





## Example

```
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
```





# Build

ThreadPool is platform independent. It's pure C++11.

cmake

```
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
add_executable(binary_name thread_pool.cc)
target_link_libraries(binary_name  pthread)
```

make on Linux

```
g++ main.cc thread_pool.cc -o binary_name -std=c++11 -lpthread
```
