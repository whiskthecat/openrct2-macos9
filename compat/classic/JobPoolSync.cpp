// Single-thread job pool for Mac OS 9: work runs immediately when added and
// its completion callback runs when the pool is joined, as callers expect.
#include <openrct2/core/JobPool.h>

JobPool::TaskData::TaskData(std::function<void()> workFn, std::function<void()> completionFn)
    : WorkFn(std::move(workFn))
    , CompletionFn(std::move(completionFn))
{
}

JobPool::JobPool(size_t)
{
}

JobPool::~JobPool()
{
}

void JobPool::AddTask(std::function<void()> workFn, std::function<void()> completionFn)
{
    workFn();
    _completed.emplace_back(nullptr, std::move(completionFn));
}

void JobPool::Join(std::function<void()> reportFn)
{
    while (!_completed.empty())
    {
        auto task = std::move(_completed.front());
        _completed.pop_front();
        if (task.CompletionFn)
            task.CompletionFn();
        if (reportFn)
            reportFn();
    }
}

bool JobPool::IsBusy()
{
    // Work runs synchronously in AddTask, so nothing is ever still running.
    // (Completion callbacks wait for Join, as in the threaded pool.)
    return false;
}

void JobPool::ProcessQueue()
{
}
