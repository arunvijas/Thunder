/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 Metrological
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../IPTestAdministrator.h"

#include <gtest/gtest.h>
#include <core/core.h>
#include <thread>

using namespace WPEFramework;
using namespace WPEFramework::Core;

static constexpr uint32_t MaxJobWaitTime = 1000; // In milliseconds
static constexpr uint8_t MaxAdditionalWorker = 5;

class EventControl {
public:
    EventControl(const EventControl&) = delete;
    EventControl& operator=(const EventControl&) = delete;
    EventControl()
        : _event(false, true)
    {
    }
    ~EventControl() = default;

public:
    void Notify()
    {
        _event.SetEvent();
    }
    uint32_t WaitForEvent(uint32_t waitTime)
    {
        return _event.Lock(waitTime);
    }
    void Reset()
    {
        _event.ResetEvent();
    }

private:
    Event _event;
};

template<typename IMPLEMENTATION>
class TestJob : public Core::IDispatch, public EventControl {
public:
    enum Status {
         INITIATED,
         CANCELED,
         COMPLETED,
    };

    TestJob() = delete;
    TestJob(const TestJob& copy) = delete;
    TestJob& operator=(const TestJob& RHS) = delete;
    ~TestJob() override = default;
    TestJob(IMPLEMENTATION& parent, const Status status, const uint32_t waitTime = 0, const bool checkParentState = false)
        : _parent(parent)
        , _status(status)
        , _waitTime(waitTime)
        , _checkParentState(checkParentState)
    {
    }

public:
    Status GetStatus()
    {
        return _status;
    }
    void Cancelled()
    {
        _status = (_status != COMPLETED) ? CANCELED : _status;
    }
    void Dispatch() override
    {
        _status = COMPLETED;
        usleep(_waitTime);
        Notify();
        if (_checkParentState) {
//            _parent.WaitForReady(this, _waitTime * 10);
        }
    }

private:
    IMPLEMENTATION& _parent;
    Status _status;
    uint32_t _waitTime;
    bool _checkParentState;
//    static std::thread::id _parentJobId;
};

template<typename IMPLEMENTATION>
class JobControl {
private:
    typedef std::map<Core::IDispatch*, EventControl*> JobMap;
private:
    template<typename PARENTIMPL = IMPLEMENTATION>
    class ExternalWorker : public Thread {
    public:
        ExternalWorker() = delete;
        ExternalWorker& operator=(const ExternalWorker&) = delete;
        ExternalWorker(const ExternalWorker& copy)
            : _job(copy._job)
            , _parent(copy._parent)
            , _waitTime(copy._waitTime)
        {
        }

        ExternalWorker(PARENTIMPL &parent)
            : _job(nullptr)
            , _parent(parent)
            , _waitTime(0)
        {
        }
        ~ExternalWorker()
        {
            Stop();
        }

    public:
        void Stop()
        {
            Core::Thread::Wait(Core::Thread::STOPPED|Core::Thread::BLOCKED, Core::infinite);
        }
        virtual uint32_t Worker() override
        {
            if (IsRunning()) {
                _parent.SubmitJob(*_job);
            }
            Core::Thread::Block();
            return (Core::infinite);
        }

        void Submit(Core::ProxyType<IDispatch>& job, const uint32_t waitTime = 0)
        {
            _job = &job;
            _waitTime = waitTime;
            Core::Thread::Run();
        }
    private:
        Core::ProxyType<IDispatch>* _job;
        IMPLEMENTATION& _parent;
        uint32_t _waitTime;
    };

public:
    JobControl() = delete;
    JobControl(const JobControl&) = delete;
    JobControl& operator=(const JobControl&) = delete;
    JobControl(IMPLEMENTATION &parent)
        : _index(0)
        , _parent(parent)
        , _external()
    {
        for (uint8_t index = 0; index < MaxAdditionalWorker; ++index) {
            _external.push_back(*(new ExternalWorker<IMPLEMENTATION>(parent)));
        }
    }
    ~JobControl()
    {
        for (auto& job: _jobs) {
            delete job.second;
        }
        _jobs.clear();
        Singleton::Dispose();
    }
public:
    uint32_t WaitForReady(IDispatch* job, const uint32_t waitTime = 0)
    {
        uint32_t result = Core::ERROR_NONE;
        JobMap::iterator index = _jobs.find(job);
        if (index != _jobs.end()) {
            result = index->second->WaitForEvent(waitTime);
        }
        return result;
    }
        void NotifyReady(const Core::ProxyType<IDispatch>& job)
    {
        JobMap::iterator index = _jobs.find(job.operator->());
        if (index != _jobs.end()) {
            index->second->Notify();
        }
    }
    void SubmitUsingSelfWorker(Core::ProxyType<IDispatch>& job, const uint32_t waitTime = 0)
    {
        _jobs.emplace(std::piecewise_construct, std::forward_as_tuple(job.operator->()), std::forward_as_tuple(new EventControl()));

        _parent.Submit(job);
    }
    void SubmitUsingExternalWorker(Core::ProxyType<IDispatch>& job, const uint32_t waitTime = 0)
    {
        if (_index < MaxAdditionalWorker) {
            _jobs.emplace(std::piecewise_construct, std::forward_as_tuple(job.operator->()), std::forward_as_tuple(new EventControl()));

            _external[_index++].Submit(job, waitTime);
        }
    }

private:
    uint8_t _index;
    JobMap _jobs;
    IMPLEMENTATION& _parent;
    std::vector<ExternalWorker<IMPLEMENTATION>> _external;
};

class WorkerPoolTester : public Core::WorkerPool, public JobControl<WorkerPoolTester> {
public:
    WorkerPoolTester() = delete;
    WorkerPoolTester(const WorkerPoolTester&) = delete;
    WorkerPoolTester& operator=(const WorkerPoolTester&) = delete;

    WorkerPoolTester(const uint8_t threads, const uint32_t stackSize, const uint32_t queueSize)
        : WorkerPool(threads, stackSize, queueSize, &_dispatcher)
        , JobControl(*this)
    {
    }

    ~WorkerPoolTester()
    {
        // Diable the queue so the minions can stop, even if they are processing and waiting for work..
        Stop();
        Core::Singleton::Dispose();
    }

public:
    void Stop()
    {
        Core::WorkerPool::Stop();
    }

    uint32_t WaitForJobEvent(const Core::ProxyType<IDispatch>& job, const uint32_t waitTime = 0)
    {
        return static_cast<TestJob<WorkerPoolTester>&>(*job).WaitForEvent(waitTime);
    }
    void SubmitJob(const Core::ProxyType<IDispatch>& job)
    {
        Submit(job);
    }
private:
    class Dispatcher : public Core::ThreadPool::IDispatcher {
    public:
      Dispatcher(const Dispatcher&) = delete;
      Dispatcher& operator=(const Dispatcher&) = delete;

      Dispatcher() = default;
      ~Dispatcher() override = default;

    private:
      void Initialize() override { }
      void Deinitialize() override { }
      void Dispatch(Core::IDispatch* job) override
        { job->Dispatch(); }
    };

    Dispatcher _dispatcher;
};

Core::ProxyType<WorkerPoolTester> workerpool = Core::ProxyType<WorkerPoolTester>::Create(2, Core::Thread::DefaultStackSize(), 8);

class WorkerThreadClass : public Core::Thread {
public:
    WorkerThreadClass() = delete;
    WorkerThreadClass(const WorkerThreadClass&) = delete;
    WorkerThreadClass& operator=(const WorkerThreadClass&) = delete;

    WorkerThreadClass(std::thread::id parentworkerId)
        : Core::Thread(Core::Thread::DefaultStackSize(), _T("Test"))
        , _parentworkerId(parentworkerId)
        , _threadDone(false)
    {
    }

    virtual ~WorkerThreadClass()
    {
    }

    virtual uint32_t Worker() override
    {
        while (IsRunning() && (!_threadDone)) {
            EXPECT_TRUE(_parentworkerId != std::this_thread::get_id());
            ::SleepMs(250);
            _threadDone = true;
            workerpool->Stop();
        }
        return (Core::infinite);
    }

private:
    std::thread::id _parentworkerId;
    volatile bool _threadDone;
};


//std::thread::id TestJob::_parentJobId;

TEST(Core_WorkerPool, CheckWorkerStaticMethods)
{
    uint8_t queueSize = 5;
    uint8_t threadCount = 1;
    WorkerPoolTester workerPool(threadCount, 0, queueSize);

    EXPECT_EQ(Core::WorkerPool::IsAvailable(), false);

    Core::WorkerPool::Assign(&workerPool);
    EXPECT_EQ(&Core::WorkerPool::Instance(), &workerPool);
    EXPECT_EQ(Core::WorkerPool::IsAvailable(), true);

    Core::WorkerPool::Assign(nullptr);
    EXPECT_EQ(Core::WorkerPool::IsAvailable(), false);
}
TEST(Core_WorkerPool, Check_WorkerPool_WithSingleJob)
{
    uint8_t queueSize = 5;
    uint8_t threadCount = 1;
    WorkerPoolTester workerPool(threadCount, 0, queueSize);
    Core::WorkerPool::Assign(&workerPool);

    Core::ProxyType<Core::IDispatch> job = Core::ProxyType<Core::IDispatch>(Core::ProxyType<TestJob<WorkerPoolTester>>::Create(workerPool, TestJob<WorkerPoolTester>::INITIATED, 500));
    EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*job).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
    workerPool.Submit(job);
    workerPool.Run();
    EXPECT_EQ(workerPool.WaitForJobEvent(job, MaxJobWaitTime), Core::ERROR_NONE);
    workerPool.Stop();

    EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*job).GetStatus(), TestJob<WorkerPoolTester>::COMPLETED);
    Core::WorkerPool::Assign(nullptr);
    job.Release();
}
TEST(Core_WorkerPool, Check_WorkerPool_WithSingleJob_CancelJob_BeforeProcessing)
{
    uint8_t queueSize = 5;
    uint8_t threadCount = 1;
    WorkerPoolTester workerPool(threadCount, 0, queueSize);
    Core::WorkerPool::Assign(&workerPool);

    Core::ProxyType<Core::IDispatch> job = Core::ProxyType<Core::IDispatch>(Core::ProxyType<TestJob<WorkerPoolTester>>::Create(workerPool, TestJob<WorkerPoolTester>::INITIATED));
    EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*job).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
    workerPool.Submit(job);
    workerPool.Revoke(job);
    workerPool.Run();
    EXPECT_EQ(workerPool.WaitForJobEvent(job, MaxJobWaitTime), Core::ERROR_TIMEDOUT);
    workerPool.Stop();

    EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*job).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
    Core::WorkerPool::Assign(nullptr);
    job.Release();
}
TEST(Core_WorkerPool, Check_WorkerPool_WithSingleJob_CancelJob_WhileProcessing)
{
    uint8_t queueSize = 5;
    uint8_t threadCount = 1;
    WorkerPoolTester workerPool(threadCount, 0, queueSize);
    Core::WorkerPool::Assign(&workerPool);

    Core::ProxyType<Core::IDispatch> job = Core::ProxyType<Core::IDispatch>(Core::ProxyType<TestJob<WorkerPoolTester>>::Create(workerPool, TestJob<WorkerPoolTester>::INITIATED, 1000));
    EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*job).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
    workerPool.Submit(job);
    workerPool.Run();
    usleep(100);
    workerPool.Revoke(job);
    EXPECT_EQ(workerPool.WaitForJobEvent(job, MaxJobWaitTime), Core::ERROR_NONE);
    workerPool.Stop();

    EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*job).GetStatus(), TestJob<WorkerPoolTester>::COMPLETED);
    Core::WorkerPool::Assign(nullptr);
    job.Release();
}
void CheckWorkerPool_MultipleJobs(uint8_t threadCount, uint8_t additonalJobs, uint8_t queueSize)
{
    WorkerPoolTester workerPool(threadCount, 0, queueSize);
    Core::WorkerPool::Assign(&workerPool);

    std::vector<Core::ProxyType<Core::IDispatch>> jobs;
    // Create Jobs with more than Queue size. i.e, queueSize + additonalJobs
    for (uint8_t i = 0; i < queueSize + additonalJobs; ++i) {
        jobs.push_back(Core::ProxyType<Core::IDispatch>(Core::ProxyType<TestJob<WorkerPoolTester>>::Create(workerPool, TestJob<WorkerPoolTester>::INITIATED, 100)));
    }

    for (uint8_t i = 0; i < queueSize; ++i) {
        EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*jobs[i]).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
        workerPool.SubmitUsingSelfWorker(jobs[i]);
    }
    for (uint8_t i = queueSize; i < jobs.size(); ++i) {
        EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*jobs[i]).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
        workerPool.SubmitUsingExternalWorker(jobs[i]);
    }

    workerPool.Run();
    usleep(MaxJobWaitTime);
    for (auto& job: jobs) {
        EXPECT_EQ(workerPool.WaitForJobEvent(job, MaxJobWaitTime * 3), Core::ERROR_NONE);
    }

    for (auto& job: jobs) {
        EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*job).GetStatus(), TestJob<WorkerPoolTester>::COMPLETED);
    }

    workerPool.Stop();

    for (auto& job: jobs) {
        job.Release();
    }
    jobs.clear();
    Core::WorkerPool::Assign(nullptr);
}
TEST(Core_WorkerPool, Check_WorkerPool_SinglePool_WithMultipleJobs)
{
    CheckWorkerPool_MultipleJobs(1, 0, 5);
}
TEST(Core_WorkerPool, Check_WorkerPool_MultiplePool_WithMultipleJobs)
{
    CheckWorkerPool_MultipleJobs(5, 0, 5);
}
TEST(Core_WorkerPool, Check_WorkerPool_SinglePool_WithMultipleJobs_AdditionalJobs)
{
    CheckWorkerPool_MultipleJobs(5, 2, 2);
    CheckWorkerPool_MultipleJobs(5, 2, 1);
    CheckWorkerPool_MultipleJobs(5, MaxAdditionalWorker, 5);
}

void CheckWorkerPool_MultipleJobs_CancelJobs_InBetween(uint8_t threadCount, uint8_t additonalJobs, uint8_t queueSize, uint8_t cancelJobsCount, uint8_t cancelJobsId[])
{
    WorkerPoolTester workerPool(threadCount, 0, queueSize);
    Core::WorkerPool::Assign(&workerPool);

    std::vector<Core::ProxyType<Core::IDispatch>> jobs;
    // Create Jobs with more than Queue size. i.e, queueSize + additonalJobs
    for (uint8_t i = 0; i < queueSize + additonalJobs; ++i) {
        jobs.push_back(Core::ProxyType<Core::IDispatch>(Core::ProxyType<TestJob<WorkerPoolTester>>::Create(workerPool, TestJob<WorkerPoolTester>::INITIATED, 1000)));
    }

    for (uint8_t i = 0; i < queueSize; ++i) {
        EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*jobs[i]).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
        workerPool.SubmitUsingSelfWorker(jobs[i]);
    }
    for (uint8_t i = queueSize; i < jobs.size(); ++i) {
        EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*jobs[i]).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
        workerPool.SubmitUsingExternalWorker(jobs[i]);
    }

    if (threadCount == 1) {
        workerPool.Run();
        for (uint8_t index = 0; index < cancelJobsCount; index++) {
            workerPool.Revoke(jobs[cancelJobsId[index]], 0);
        }
    } else {
        // Multi Pool case jobs can be run in parallel once we start thread, hence revoke inbetween maynot work
        // it just have to wait for proceesing jobs completion.
        // Hence revoking before starting the job. Just to ensure the status meets
        for (uint8_t index = 0; index < cancelJobsCount; index++) {
            workerPool.Revoke(jobs[cancelJobsId[index]], 0);
        }
        workerPool.Run();
    }

    for (uint8_t index = 0; index < jobs.size(); index++) {
        bool isCanceledJob = false;
        for (uint8_t cancelIndex = 0; cancelIndex < cancelJobsCount; cancelIndex++)
        {
            if (index == cancelJobsId[cancelIndex]) {
                isCanceledJob = true;
                break;
            }
        }
        if (isCanceledJob == true) {
            EXPECT_EQ(workerPool.WaitForJobEvent(jobs[index], MaxJobWaitTime * 3), Core::ERROR_TIMEDOUT);
        } else {
            EXPECT_EQ(workerPool.WaitForJobEvent(jobs[index], MaxJobWaitTime * 3), Core::ERROR_NONE);
        }
    }

    for (uint8_t index = 0; index < jobs.size(); index++) {
        bool isCanceledJob = false;
        for (uint8_t cancelIndex = 0; cancelIndex < cancelJobsCount; cancelIndex++)
        {
            if (index == cancelJobsId[cancelIndex]) {
                isCanceledJob = true;
                break;
            }
        }
        if (isCanceledJob == true) {
            EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*jobs[index]).GetStatus(), TestJob<WorkerPoolTester>::INITIATED);
        } else {
            EXPECT_EQ(static_cast<TestJob<WorkerPoolTester>&>(*jobs[index]).GetStatus(), TestJob<WorkerPoolTester>::COMPLETED);
        }
    }

    workerPool.Stop();

    for (auto& job: jobs) {
        job.Release();
    }
    jobs.clear();
    Core::WorkerPool::Assign(nullptr);
}
TEST(Core_WorkerPool, Check_WorkerPool_SinglePool_WithMultipleJobs_CancelJobs_InBetween)
{
    uint8_t cancelJobsId[] = {3, 4};
    CheckWorkerPool_MultipleJobs_CancelJobs_InBetween(1, 0, 5, sizeof(cancelJobsId), cancelJobsId);
}
TEST(Core_WorkerPool, Check_WorkerPool_SinglePool_WithMultipleJobs_AdditionalJobs_CancelJobs_InBetween)
{
    uint8_t cancelJobsId[] = {3, 4};
    CheckWorkerPool_MultipleJobs_CancelJobs_InBetween(1, 2, 5, sizeof(cancelJobsId), cancelJobsId);
}
TEST(Core_WorkerPool, Check_WorkerPool_MultiplePool_WithMultipleJobs_AdditionalJobs_CancelJobs_InBetween)
{
    uint8_t cancelJobsId[] = {3, 4};
    CheckWorkerPool_MultipleJobs_CancelJobs_InBetween(5, 2, 5, sizeof(cancelJobsId), cancelJobsId);
}

