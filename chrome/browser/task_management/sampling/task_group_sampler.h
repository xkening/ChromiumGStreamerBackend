// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_TASK_MANAGEMENT_SAMPLING_TASK_GROUP_SAMPLER_H_
#define CHROME_BROWSER_TASK_MANAGEMENT_SAMPLING_TASK_GROUP_SAMPLER_H_

#include "base/callback.h"
#include "base/memory/ref_counted.h"
#include "base/process/process_handle.h"
#include "base/process/process_metrics.h"
#include "base/sequence_checker.h"
#include "base/sequenced_task_runner.h"

namespace task_management {

// Wraps the memory usage stats values together so that it can be sent between
// the UI and the worker threads.
struct MemoryUsageStats {
  int64 private_bytes;
  int64 shared_bytes;
  int64 physical_bytes;

  MemoryUsageStats()
      : private_bytes(-1),
        shared_bytes(-1),
        physical_bytes(-1) {
  }
};

// Defines the expensive process' stats sampler that will calculate these
// resources on the worker thread.
class TaskGroupSampler : public base::RefCountedThreadSafe<TaskGroupSampler> {
 public:
  // The below are the types of the callbacks to the UI thread upon their
  // corresponding refresh are done on the worker thread.
  typedef base::Callback<void(double)> OnCpuRefreshCallback;
  typedef base::Callback<void(MemoryUsageStats)> OnMemoryRefreshCallback;
  typedef base::Callback<void(int)> OnIdleWakeupsCallback;

  TaskGroupSampler(
      base::ProcessHandle proc_handle,
      const scoped_refptr<base::SequencedTaskRunner>& blocking_pool_runner,
      const OnCpuRefreshCallback& on_cpu_refresh,
      const OnMemoryRefreshCallback& on_memory_refresh,
      const OnIdleWakeupsCallback& on_idle_wakeups);

  // Refreshes the expensive process' stats (CPU usage, memory usage, and idle
  // wakeups per second) on the worker thread. It will crash if
  // SetOnRefreshCallbacks() hasn't been called yet.
  void Refresh(int64 refresh_flags);

 private:
  friend class base::RefCountedThreadSafe<TaskGroupSampler>;
  ~TaskGroupSampler();

  // The refresh calls that will be done on the worker thread.
  double RefreshCpuUsage();
  MemoryUsageStats RefreshMemoryUsage();
  int RefreshIdleWakeupsPerSecond();

  scoped_ptr<base::ProcessMetrics> process_metrics_;

  // The specific blocking pool SequencedTaskRunner that will be used to post
  // the refresh tasks onto serially.
  scoped_refptr<base::SequencedTaskRunner> blocking_pool_runner_;

  // The UI-thread callbacks in TaskGroup to be called when their corresponding
  // refreshes on the worker thread are done.
  OnCpuRefreshCallback on_cpu_refresh_callback_;
  OnMemoryRefreshCallback on_memory_refresh_callback_;
  OnIdleWakeupsCallback on_idle_wakeups_callback_;

  // To assert we're running on the correct thread.
  base::SequenceChecker sequenced_checker_;

  DISALLOW_COPY_AND_ASSIGN(TaskGroupSampler);
};

}  // namespace task_management


#endif  // CHROME_BROWSER_TASK_MANAGEMENT_SAMPLING_TASK_GROUP_SAMPLER_H_
