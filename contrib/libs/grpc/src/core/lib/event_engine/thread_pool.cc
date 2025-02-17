//
//
// Copyright 2015 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include <grpc/support/port_platform.h>

#include "src/core/lib/event_engine/thread_pool.h"

#include <atomic>
#include <memory>
#include <utility>

#include "y_absl/base/attributes.h"
#include "y_absl/time/clock.h"
#include "y_absl/time/time.h"

#include <grpc/support/log.h>

#include "src/core/lib/event_engine/thread_local.h"
#include "src/core/lib/gprpp/thd.h"
#include "src/core/lib/gprpp/time.h"

namespace {
    size_t threads_limit_ = 0;
}

namespace grpc_event_engine {
namespace experimental {

size_t ThreadPool::SetThreadsLimit(size_t count) {
    size_t prev = threads_limit_;
    threads_limit_ = count;
    return prev;
}

unsigned ThreadPool::GetMaxSystemThread() {
    unsigned max_threads = grpc_core::Clamp(gpr_cpu_num_cores(), 2u, 32u);

    if (threads_limit_) {
        unsigned new_max_threads = std::min(max_threads, static_cast<unsigned>(threads_limit_));
        gpr_log(GPR_INFO, "Threads limit changed from %u to %u", max_threads, new_max_threads);
        max_threads = new_max_threads;
    }
    return max_threads;
}

void ThreadPool::StartThread(StatePtr state, StartThreadReason reason) {
  state->thread_count.Add();
  const auto now = grpc_core::Timestamp::Now();
  switch (reason) {
    case StartThreadReason::kNoWaitersWhenScheduling: {
      auto time_since_last_start =
          now - grpc_core::Timestamp::FromMillisecondsAfterProcessEpoch(
                    state->last_started_thread.load(std::memory_order_relaxed));
      if (time_since_last_start < grpc_core::Duration::Seconds(1)) {
        state->thread_count.Remove();
        return;
      }
    }
      Y_ABSL_FALLTHROUGH_INTENDED;
    case StartThreadReason::kNoWaitersWhenFinishedStarting:
      if (state->currently_starting_one_thread.exchange(
              true, std::memory_order_relaxed)) {
        state->thread_count.Remove();
        return;
      }
      state->last_started_thread.store(now.milliseconds_after_process_epoch(),
                                       std::memory_order_relaxed);
      break;
    case StartThreadReason::kInitialPool:
      break;
  }
  struct ThreadArg {
    StatePtr state;
    StartThreadReason reason;
  };
  grpc_core::Thread(
      "event_engine",
      [](void* arg) {
        std::unique_ptr<ThreadArg> a(static_cast<ThreadArg*>(arg));
        ThreadLocal::SetIsEventEngineThread(true);
        switch (a->reason) {
          case StartThreadReason::kInitialPool:
            break;
          case StartThreadReason::kNoWaitersWhenFinishedStarting:
            a->state->queue.SleepIfRunning();
            Y_ABSL_FALLTHROUGH_INTENDED;
          case StartThreadReason::kNoWaitersWhenScheduling:
            // Release throttling variable
            GPR_ASSERT(a->state->currently_starting_one_thread.exchange(
                false, std::memory_order_relaxed));
            if (a->state->queue.IsBacklogged()) {
              StartThread(a->state,
                          StartThreadReason::kNoWaitersWhenFinishedStarting);
            }
            break;
        }
        ThreadFunc(a->state);
      },
      new ThreadArg{state, reason}, nullptr,
      grpc_core::Thread::Options().set_tracked(false).set_joinable(false))
      .Start();
}

void ThreadPool::ThreadFunc(StatePtr state) {
  while (state->queue.Step()) {
  }
  state->thread_count.Remove();
}

bool ThreadPool::Queue::Step() {
  grpc_core::ReleasableMutexLock lock(&queue_mu_);
  // Wait until work is available or we are shutting down.
  while (!shutdown_ && !forking_ && callbacks_.empty()) {
    // If there are too many threads waiting, then quit this thread.
    // TODO(ctiller): wait some time in this case to be sure.
    if (threads_waiting_ >= reserve_threads_) {
      threads_waiting_++;
      bool timeout = cv_.WaitWithTimeout(&queue_mu_, y_absl::Seconds(30));
      threads_waiting_--;
      if (timeout && threads_waiting_ >= reserve_threads_) {
        return false;
      }
    } else {
      threads_waiting_++;
      cv_.Wait(&queue_mu_);
      threads_waiting_--;
    }
  }
  if (forking_) return false;
  if (shutdown_ && callbacks_.empty()) return false;
  GPR_ASSERT(!callbacks_.empty());
  auto callback = std::move(callbacks_.front());
  callbacks_.pop();
  lock.Release();
  callback();
  return true;
}

ThreadPool::ThreadPool() {
  for (unsigned i = 0; i < reserve_threads_; i++) {
    StartThread(state_, StartThreadReason::kInitialPool);
  }
}

bool ThreadPool::IsThreadPoolThread() {
  return ThreadLocal::IsEventEngineThread();
}

void ThreadPool::Quiesce() {
  state_->queue.SetShutdown(true);
  // Wait until all threads are exited.
  // Note that if this is a threadpool thread then we won't exit this thread
  // until the callstack unwinds a little, so we need to wait for just one
  // thread running instead of zero.
  state_->thread_count.BlockUntilThreadCount(
      ThreadLocal::IsEventEngineThread() ? 1 : 0, "shutting down");
  quiesced_.store(true, std::memory_order_relaxed);
}

ThreadPool::~ThreadPool() {
  GPR_ASSERT(quiesced_.load(std::memory_order_relaxed));
}

void ThreadPool::Run(y_absl::AnyInvocable<void()> callback) {
  GPR_DEBUG_ASSERT(quiesced_.load(std::memory_order_relaxed) == false);
  if (state_->queue.Add(std::move(callback))) {
    StartThread(state_, StartThreadReason::kNoWaitersWhenScheduling);
  }
}

void ThreadPool::Run(EventEngine::Closure* closure) {
  Run([closure]() { closure->Run(); });
}

bool ThreadPool::Queue::Add(y_absl::AnyInvocable<void()> callback) {
  grpc_core::MutexLock lock(&queue_mu_);
  // Add works to the callbacks list
  callbacks_.push(std::move(callback));
  cv_.Signal();
  if (forking_) return false;
  return callbacks_.size() > threads_waiting_;
}

bool ThreadPool::Queue::IsBacklogged() {
  grpc_core::MutexLock lock(&queue_mu_);
  if (forking_) return false;
  return callbacks_.size() > 1;
}

void ThreadPool::Queue::SleepIfRunning() {
  grpc_core::MutexLock lock(&queue_mu_);
  auto end = grpc_core::Duration::Seconds(1) + grpc_core::Timestamp::Now();
  while (true) {
    grpc_core::Timestamp now = grpc_core::Timestamp::Now();
    if (now >= end || forking_) return;
    cv_.WaitWithTimeout(&queue_mu_, y_absl::Milliseconds((end - now).millis()));
  }
}

void ThreadPool::Queue::SetShutdown(bool is_shutdown) {
  grpc_core::MutexLock lock(&queue_mu_);
  auto was_shutdown = std::exchange(shutdown_, is_shutdown);
  GPR_ASSERT(is_shutdown != was_shutdown);
  cv_.SignalAll();
}

void ThreadPool::Queue::SetForking(bool is_forking) {
  grpc_core::MutexLock lock(&queue_mu_);
  auto was_forking = std::exchange(forking_, is_forking);
  GPR_ASSERT(is_forking != was_forking);
  cv_.SignalAll();
}

void ThreadPool::ThreadCount::Add() {
  grpc_core::MutexLock lock(&thread_count_mu_);
  ++threads_;
}

void ThreadPool::ThreadCount::Remove() {
  grpc_core::MutexLock lock(&thread_count_mu_);
  --threads_;
  cv_.Signal();
}

void ThreadPool::ThreadCount::BlockUntilThreadCount(int threads,
                                                    const char* why) {
  grpc_core::MutexLock lock(&thread_count_mu_);
  auto last_log = y_absl::Now();
  while (threads_ > threads) {
    // Wait for all threads to exit.
    // At least once every three seconds (but no faster than once per second in
    // the event of spurious wakeups) log a message indicating we're waiting to
    // fork.
    cv_.WaitWithTimeout(&thread_count_mu_, y_absl::Seconds(3));
    if (threads_ > threads && y_absl::Now() - last_log > y_absl::Seconds(1)) {
      gpr_log(GPR_ERROR, "Waiting for thread pool to idle before %s", why);
      last_log = y_absl::Now();
    }
  }
}

void ThreadPool::PrepareFork() {
  state_->queue.SetForking(true);
  state_->thread_count.BlockUntilThreadCount(0, "forking");
}

void ThreadPool::PostforkParent() { Postfork(); }

void ThreadPool::PostforkChild() { Postfork(); }

void ThreadPool::Postfork() {
  state_->queue.SetForking(false);
  for (unsigned i = 0; i < reserve_threads_; i++) {
    StartThread(state_, StartThreadReason::kInitialPool);
  }
}

}  // namespace experimental
}  // namespace grpc_event_engine
