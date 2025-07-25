/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <folly/Unit.h>
#include <folly/init/Init.h>
#include <velox/exec/Driver.h>
#include <memory>
#include "folly/experimental/EventCount.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/dwio/common/tests/utils/BatchMaker.h"
#include "velox/exec/Cursor.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/Values.h"
#include "velox/exec/tests/utils/ArbitratorTestUtil.h"
#include "velox/exec/tests/utils/AssertQueryBuilder.h"
#include "velox/exec/tests/utils/OperatorTestBase.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/functions/Udf.h"

using namespace facebook::velox;
using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;

using namespace facebook::velox::common::testutil;
using facebook::velox::test::BatchMaker;

// A PlanNode that passes its input to its output and makes variable
// memory reservations.
// A PlanNode that passes its input to its output and periodically
// pauses and resumes other Tasks.
class TestingPauserNode : public core::PlanNode {
 public:
  explicit TestingPauserNode(core::PlanNodePtr input)
      : PlanNode("Pauser"), sources_{input} {}

  TestingPauserNode(const core::PlanNodeId& id, core::PlanNodePtr input)
      : PlanNode(id), sources_{input} {}

  const RowTypePtr& outputType() const override {
    return sources_[0]->outputType();
  }

  const std::vector<std::shared_ptr<const PlanNode>>& sources() const override {
    return sources_;
  }

  std::string_view name() const override {
    return "Pauser";
  }

 private:
  void addDetails(std::stringstream& /* stream */) const override {}

  std::vector<core::PlanNodePtr> sources_;
};

class DriverTest : public OperatorTestBase {
 protected:
  enum class ResultOperation {
    kRead,
    kReadSlow,
    kDrop,
    kCancel,
    kTerminate,
    kPause,
    kYield
  };

  void SetUp() override {
    OperatorTestBase::SetUp();
    Operator::unregisterAllOperators();
    rowType_ =
        ROW({"key", "m1", "m2", "m3", "m4", "m5", "m6", "m7"},
            {BIGINT(),
             BIGINT(),
             BIGINT(),
             BIGINT(),
             BIGINT(),
             BIGINT(),
             BIGINT(),
             BIGINT()});
  }

  void TearDown() override {
    for (auto& task : tasks_) {
      if (task != nullptr) {
        waitForTaskCompletion(task.get(), 1'000'000);
      }
    }
    // NOTE: destroy the tasks first to release all the allocated memory held
    // by the plan nodes (Values) in tasks.
    tasks_.clear();
    waitForAllTasksToBeDeleted();

    if (wakeupInitialized_) {
      wakeupCancelled_ = true;
      wakeupThread_.join();
    }
    OperatorTestBase::TearDown();
  }

  core::PlanNodePtr makeValuesFilterProject(
      const RowTypePtr& rowType,
      const std::string& filter,
      const std::string& project,
      int32_t numBatches,
      int32_t rowsInBatch,
      // applies to second column
      std::function<bool(int64_t)> filterFunc = nullptr,
      int32_t* filterHits = nullptr,
      bool addTestingPauser = false) {
    std::vector<RowVectorPtr> batches;
    for (int32_t i = 0; i < numBatches; ++i) {
      batches.push_back(std::dynamic_pointer_cast<RowVector>(
          BatchMaker::createBatch(rowType, rowsInBatch, *pool_)));
    }
    if (filterFunc) {
      int32_t hits = 0;
      for (auto& batch : batches) {
        auto child = batch->childAt(1)->as<FlatVector<int64_t>>();
        for (vector_size_t i = 0; i < child->size(); ++i) {
          if (!child->isNullAt(i) && filterFunc(child->valueAt(i))) {
            hits++;
          }
        }
      }
      *filterHits = hits;
    }

    PlanBuilder planBuilder;
    planBuilder.values(batches, true).planNode();

    if (!filter.empty()) {
      planBuilder.filter(filter);
    }

    if (!project.empty()) {
      auto expressions = rowType->names();
      expressions.push_back(fmt::format("{} AS expr", project));

      planBuilder.project(expressions);
    }
    if (addTestingPauser) {
      planBuilder.addNode([](std::string id, core::PlanNodePtr input) {
        return std::make_shared<TestingPauserNode>(id, input);
      });
    }

    return planBuilder.planNode();
  }

  // Opens a cursor and reads data. Takes action 'operation' every 'numRows'
  // rows of data. Increments the 'counter' for each successfully read row.
  void readResults(
      CursorParameters& params,
      ResultOperation operation,
      int32_t numRows,
      int32_t* counter,
      int32_t threadId = 0) {
    auto cursor = std::make_unique<RowCursor>(params);
    {
      std::lock_guard<std::mutex> l(mutex_);
      tasks_.push_back(cursor->task());
      // To be realized either after 1s wall time or when the corresponding Task
      // is no longer running.
      auto& executor = folly::QueuedImmediateExecutor::instance();
      auto future = tasks_.back()
                        ->taskCompletionFuture()
                        .within(std::chrono::microseconds(1'000'000))
                        .via(&executor);
      stateFutures_.emplace(threadId, std::move(future));

      EXPECT_FALSE(stateFutures_.at(threadId).isReady());
    }
    bool paused = false;
    for (;;) {
      if (operation == ResultOperation::kPause && paused) {
        if (!cursor->hasNext()) {
          paused = false;
          Task::resume(cursor->task());
        }
      }
      if (!cursor->next()) {
        break;
      }
      ++*counter;
      if (*counter % numRows == 0) {
        if (operation == ResultOperation::kDrop) {
          return;
        }
        if (operation == ResultOperation::kReadSlow) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          // If this is an EXPECT this is flaky when running on a
          // noisy test cloud.
          LOG(INFO) << "Task::toString() while probably blocked: "
                    << tasks_[0]->toString();
        } else if (operation == ResultOperation::kCancel) {
          cancelFuture_ = cursor->task()->requestCancel();
        } else if (operation == ResultOperation::kTerminate) {
          cancelFuture_ = cursor->task()->requestAbort();
        } else if (operation == ResultOperation::kYield) {
          if (*counter % 2 == 0) {
            auto time = getCurrentTimeMicro();
            cursor->task()->yieldIfDue(time - 10);
          } else {
            cursor->task()->requestYield();
          }
        } else if (operation == ResultOperation::kPause) {
          auto& executor = folly::QueuedImmediateExecutor::instance();
          auto future = cursor->task()->requestPause().via(&executor);
          future.wait();
          paused = true;
        }
      }
    }
  }

  // Checks that Test passes within a reasonable delay. The test can
  // be flaky under indeterminate timing (heavy load) because we wait
  // for a future that is realized after all threads have acknowledged
  // a stop or pause.  Setting the next state is not in the same
  // critical section as realizing the future, hence there can be a
  // delay of some hundreds of instructions before all the consequent
  // state changes occur. For cases where we have a cursor at end and
  // the final state is set only after the cursor at end is visible to
  // the caller, we do not have a good way to combine all inside the
  // same critical section.
  template <typename Test>
  void expectWithDelay(
      Test test,
      const char* file,
      int32_t line,
      const char* message) {
    constexpr int32_t kMaxWait = 1000;

    for (auto i = 0; i < kMaxWait; ++i) {
      if (test()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    FAIL() << file << ":" << line << " " << message << "not realized within 1s";
  }

  std::shared_ptr<Task> createAndStartTaskToReadValues(int numDrivers) {
    std::vector<RowVectorPtr> batches;
    for (int i = 0; i < 4; ++i) {
      batches.push_back(
          makeRowVector({"c0"}, {makeFlatVector<int32_t>({1, 2, 3})}));
    }
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto plan =
        PlanBuilder(planNodeIdGenerator).values(batches, true).planFragment();
    auto task = Task::create(
        "t0",
        plan,
        0,
        core::QueryCtx::create(driverExecutor_.get()),
        Task::ExecutionMode::kParallel,
        [](RowVectorPtr /*unused*/, bool drained, ContinueFuture* /*unused*/) {
          VELOX_CHECK(!drained);
          return exec::BlockingReason::kNotBlocked;
        });
    task->start(numDrivers, 1);
    return task;
  }

  void testDriverSuspensionWithTaskOperationRace(
      int numDrivers,
      StopReason expectedEnterSuspensionStopReason,
      std::optional<StopReason> expectedLeaveSuspensionStopReason,
      TaskState expectedTaskState,
      std::function<void(Task*)> preSuspensionTaskFunc = nullptr,
      std::function<void(Task*)> inSuspensionTaskFunc = nullptr,
      std::function<void(Task*)> leaveSuspensionTaskFunc = nullptr) {
    std::atomic_bool driverExecutionWaitFlag{true};
    folly::EventCount driverExecutionWait;
    std::atomic_bool enterSuspensionWaitFlag{true};
    folly::EventCount enterSuspensionWait;
    std::atomic_bool suspensionNotifyFlag{true};
    folly::EventCount suspensionNotify;
    std::atomic_bool leaveSuspensionWaitFlag{true};
    folly::EventCount leaveSuspensionWait;
    std::atomic_bool leaveSuspensionNotifyFlag{true};
    folly::EventCount leaveSuspensionNotify;

    std::atomic<bool> injectSuspensionOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Values::getOutput",
        std::function<void(const exec::Values*)>(
            ([&](const exec::Values* values) {
              driverExecutionWaitFlag = false;
              driverExecutionWait.notifyAll();
              if (!injectSuspensionOnce.exchange(false)) {
                return;
              }
              auto* driver = values->operatorCtx()->driver();
              enterSuspensionWait.await(
                  [&]() { return !enterSuspensionWaitFlag.load(); });
              ASSERT_EQ(
                  driver->task()->enterSuspended(driver->state()),
                  expectedEnterSuspensionStopReason);
              suspensionNotifyFlag = false;
              suspensionNotify.notifyAll();
              leaveSuspensionWait.await(
                  [&]() { return !leaveSuspensionWaitFlag.load(); });
              if (expectedLeaveSuspensionStopReason.has_value()) {
                ASSERT_EQ(
                    driver->task()->leaveSuspended(driver->state()),
                    expectedLeaveSuspensionStopReason.value());
              }
              leaveSuspensionNotifyFlag = false;
              leaveSuspensionNotify.notifyAll();
            })));

    auto task = createAndStartTaskToReadValues(numDrivers);

    driverExecutionWait.await(
        [&]() { return !driverExecutionWaitFlag.load(); });

    if (preSuspensionTaskFunc != nullptr) {
      preSuspensionTaskFunc(task.get());
    }
    enterSuspensionWaitFlag = false;
    enterSuspensionWait.notifyAll();

    suspensionNotify.await([&]() { return !suspensionNotifyFlag.load(); });
    if (inSuspensionTaskFunc != nullptr) {
      inSuspensionTaskFunc(task.get());
    }
    leaveSuspensionWaitFlag = false;
    leaveSuspensionWait.notifyAll();

    // NOTE: this callback is executed in par with driver suspension leave.
    if (leaveSuspensionTaskFunc != nullptr) {
      leaveSuspensionTaskFunc(task.get());
    }
    leaveSuspensionNotify.await(
        [&]() { return !leaveSuspensionNotifyFlag.load(); });
    if (expectedTaskState == TaskState::kFinished) {
      ASSERT_TRUE(waitForTaskCompletion(task.get(), 1000'000'000));
    } else if (expectedTaskState == TaskState::kCanceled) {
      ASSERT_TRUE(waitForTaskCancelled(task.get(), 1000'000'000));
    } else {
      ASSERT_TRUE(waitForTaskAborted(task.get(), 1000'000'000));
    }
  }

 public:
  // Sets 'future' to a future that will be realized within a random
  // delay of a few ms.
  void registerForWakeup(ContinueFuture* future) {
    std::lock_guard<std::mutex> l(wakeupMutex_);
    if (!wakeupInitialized_) {
      wakeupInitialized_ = true;
      wakeupThread_ = std::thread([this]() {
        int32_t counter = 0;
        for (;;) {
          {
            std::lock_guard<std::mutex> l2(wakeupMutex_);
            if (wakeupCancelled_) {
              return;
            }
          }
          // Wait a small interval and realize a small number of queued
          // promises, if any.
          auto units = 1 + (++counter % 5);

          // NOLINT
          std::this_thread::sleep_for(std::chrono::milliseconds(units));
          {
            std::lock_guard<std::mutex> l2(wakeupMutex_);
            auto count = 1 + (++counter % 4);
            for (auto i = 0; i < count; ++i) {
              if (wakeupPromises_.empty()) {
                break;
              }
              wakeupPromises_.front().setValue();
              wakeupPromises_.pop_front();
            }
          }
        }
      });
    }
    auto [promise, semiFuture] = makeVeloxContinuePromiseContract("wakeup");
    *future = std::move(semiFuture);
    wakeupPromises_.push_back(std::move(promise));
  }

  // Registers a Task for use in randomTask().
  void registerTask(std::shared_ptr<Task> task) {
    std::lock_guard<std::mutex> l(taskMutex_);
    if (std::find(allTasks_.begin(), allTasks_.end(), task) !=
        allTasks_.end()) {
      return;
    }
    allTasks_.push_back(task);
  }

  void unregisterTask(std::shared_ptr<Task> task) {
    std::lock_guard<std::mutex> l(taskMutex_);
    auto it = std::find(allTasks_.begin(), allTasks_.end(), task);
    if (it == allTasks_.end()) {
      return;
    }
    allTasks_.erase(it);
  }

  std::shared_ptr<Task> randomTask() {
    std::lock_guard<std::mutex> l(taskMutex_);
    if (allTasks_.empty()) {
      return nullptr;
    }
    return allTasks_[folly::Random::rand32() % allTasks_.size()];
  }

 protected:
  // State for registerForWakeup().
  std::mutex wakeupMutex_;
  std::thread wakeupThread_;
  std::deque<ContinuePromise> wakeupPromises_;
  bool wakeupInitialized_{false};
  // Set to true when it is time to exit 'wakeupThread_'.
  std::atomic<bool> wakeupCancelled_{false};

  RowTypePtr rowType_;
  std::mutex mutex_;
  std::vector<std::shared_ptr<Task>> tasks_;
  ContinueFuture cancelFuture_;
  std::unordered_map<int32_t, ContinueFuture> stateFutures_;

  // Mutex for randomTask()
  std::mutex taskMutex_;
  // Tasks registered for randomTask()
  std::vector<std::shared_ptr<Task>> allTasks_;

  folly::Random::DefaultGenerator rng_;
};

#define EXPECT_WITH_DELAY(test) \
  expectWithDelay([&]() { return test; }, __FILE__, __LINE__, #test)

TEST_F(DriverTest, error) {
  CursorParameters params;
  params.planNode =
      makeValuesFilterProject(rowType_, "m1 % 0 > 0", "", 100, 10);
  params.maxDrivers = 20;
  int32_t numRead = 0;
  try {
    readResults(params, ResultOperation::kRead, 1'000'000, &numRead);
    EXPECT_TRUE(false) << "Expected exception";
  } catch (const VeloxException& e) {
    EXPECT_NE(e.message().find("Cannot divide by 0"), std::string::npos);
  }
  EXPECT_EQ(numRead, 0);
  EXPECT_TRUE(stateFutures_.at(0).isReady());
  // Realized immediately since task not running.
  EXPECT_TRUE(tasks_[0]
                  ->taskCompletionFuture()
                  .within(std::chrono::microseconds(1'000'000))
                  .isReady());
  EXPECT_EQ(tasks_[0]->state(), TaskState::kFailed);
}

TEST_F(DriverTest, cancel) {
  CursorParameters params;
  params.planNode = makeValuesFilterProject(
      rowType_,
      "m1 % 10 > 0",
      "m1 % 3 + m2 % 5 + m3 % 7 + m4 % 11 + m5 % 13 + m6 % 17 + m7 % 19",
      1'000,
      1'000);
  params.maxDrivers = 10;
  int32_t numRead = 0;
  try {
    readResults(params, ResultOperation::kCancel, 1'000'000, &numRead);
    FAIL() << "Expected exception";
  } catch (const VeloxRuntimeError& e) {
    EXPECT_EQ("Cancelled", e.message());
  }
  EXPECT_GE(numRead, 1'000'000);
  auto& executor = folly::QueuedImmediateExecutor::instance();
  auto future = tasks_[0]
                    ->taskCompletionFuture()
                    .within(std::chrono::microseconds(1'000'000))
                    .via(&executor);
  future.wait();
  EXPECT_TRUE(stateFutures_.at(0).isReady());

  std::move(cancelFuture_).via(&executor).wait();

  EXPECT_EQ(tasks_[0]->numRunningDrivers(), 0);
}

TEST_F(DriverTest, terminate) {
  CursorParameters params;
  params.planNode = makeValuesFilterProject(
      rowType_,
      "m1 % 10 > 0",
      "m1 % 3 + m2 % 5 + m3 % 7 + m4 % 11 + m5 % 13 + m6 % 17 + m7 % 19",
      1'000,
      1'000);
  params.maxDrivers = 10;
  int32_t numRead = 0;
  try {
    readResults(params, ResultOperation::kTerminate, 1'000'000, &numRead);
    // Not necessarily an exception.
  } catch (const std::exception& e) {
    // If this is an exception, it will be a cancellation.
    EXPECT_TRUE(strstr(e.what(), "Aborted") != nullptr) << e.what();
  }

  ASSERT_TRUE(cancelFuture_.valid());
  auto& executor = folly::QueuedImmediateExecutor::instance();
  std::move(cancelFuture_).via(&executor).wait();

  EXPECT_GE(numRead, 1'000'000);
  EXPECT_TRUE(stateFutures_.at(0).isReady());
  EXPECT_EQ(tasks_[0]->state(), TaskState::kAborted);
}

TEST_F(DriverTest, slow) {
  CursorParameters params;
  params.planNode = makeValuesFilterProject(
      rowType_,
      "m1 % 10 > 0",
      "m1 % 3 + m2 % 5 + m3 % 7 + m4 % 11 + m5 % 13 + m6 % 17 + m7 % 19",
      300,
      1'000);
  params.maxDrivers = 10;
  int32_t numRead = 0;
  readResults(params, ResultOperation::kReadSlow, 50'000, &numRead);
  EXPECT_GE(numRead, 50'000);
  // Sync before checking end state. The cursor is at end as soon as
  // CallbackSink::finish is called. The thread count and task state
  // are updated some tens of instructions after this. Determinism
  // requires a barrier.
  auto& executor = folly::QueuedImmediateExecutor::instance();
  auto future = tasks_[0]
                    ->taskCompletionFuture()
                    .within(std::chrono::microseconds(1'000'000))
                    .via(&executor);
  future.wait();
  // Note that the driver count drops after the last thread stops and
  // realizes the future.
  EXPECT_WITH_DELAY(tasks_[0]->numRunningDrivers() == 0);
  const auto stats = tasks_[0]->taskStats().pipelineStats;
  ASSERT_TRUE(!stats.empty() && !stats[0].operatorStats.empty());
  // Check that the blocking of the CallbackSink at the end of the pipeline is
  // recorded.
  EXPECT_GT(stats[0].operatorStats.back().blockedWallNanos, 0);
  EXPECT_TRUE(stateFutures_.at(0).isReady());
  // The future was realized by timeout.
  EXPECT_TRUE(stateFutures_.at(0).hasException());
}

TEST_F(DriverTest, pause) {
  CursorParameters params;
  int32_t hits;
  params.planNode = makeValuesFilterProject(
      rowType_,
      "m1 % 10 > 0",
      "m1 % 3 + m2 % 5 + m3 % 7 + m4 % 11 + m5 % 13 + m6 % 17 + m7 % 19",
      1'000,
      1'000,
      [](int64_t num) { return num % 10 > 0; },
      &hits);
  params.maxDrivers = 10;
  // Make sure CPU usage tracking is enabled.
  std::unordered_map<std::string, std::string> queryConfig{
      {core::QueryConfig::kOperatorTrackCpuUsage, "true"}};
  params.queryCtx = core::QueryCtx::create(
      executor_.get(), core::QueryConfig(std::move(queryConfig)));
  int32_t numRead = 0;
  readResults(params, ResultOperation::kPause, 370'000'000, &numRead);
  // Each thread will fully read the 1M rows in values.
  EXPECT_EQ(numRead, 10 * hits);
  auto stateFuture = tasks_[0]->taskCompletionFuture().within(
      std::chrono::microseconds(100'000'000));
  auto& executor = folly::QueuedImmediateExecutor::instance();
  auto state = std::move(stateFuture).via(&executor);
  state.wait();
  EXPECT_TRUE(tasks_[0]->isFinished());
  EXPECT_EQ(tasks_[0]->numRunningDrivers(), 0);
  const auto taskStats = tasks_[0]->taskStats();
  ASSERT_EQ(taskStats.pipelineStats.size(), 1);
  const auto& operators = taskStats.pipelineStats[0].operatorStats;
  EXPECT_GT(operators[1].getOutputTiming.wallNanos, 0);
  EXPECT_EQ(operators[0].outputPositions, 10000000);
  EXPECT_EQ(operators[1].inputPositions, 10000000);
  EXPECT_EQ(operators[1].outputPositions, 10 * hits);
}

TEST_F(DriverTest, yield) {
  constexpr int32_t kNumTasks = 20;
  constexpr int32_t kThreadsPerTask = 5;
  std::vector<CursorParameters> params(kNumTasks);
  int32_t hits;
  for (int32_t i = 0; i < kNumTasks; ++i) {
    params[i].planNode = makeValuesFilterProject(
        rowType_,
        "m1 % 10 > 0",
        "m1 % 3 + m2 % 5 + m3 % 7 + m4 % 11 + m5 % 13 + m6 % 17 + m7 % 19",
        200,
        2'000,
        [](int64_t num) { return num % 10 > 0; },
        &hits);
    params[i].maxDrivers = kThreadsPerTask;
  }
  std::vector<int32_t> counters(kNumTasks, 0);
  std::vector<std::thread> threads;
  threads.reserve(kNumTasks);
  for (int32_t i = 0; i < kNumTasks; ++i) {
    threads.push_back(std::thread([this, &params, &counters, i]() {
      readResults(params[i], ResultOperation::kYield, 10'000, &counters[i], i);
    }));
  }
  for (int32_t i = 0; i < kNumTasks; ++i) {
    threads[i].join();
    EXPECT_WITH_DELAY(stateFutures_.at(i).isReady());
    EXPECT_EQ(counters[i], kThreadsPerTask * hits);
  }
}

// A testing Operator that periodically does one of the following:
//
// 1. Blocks and registers a resume that continues the Driver after a timed
// pause. This simulates blocking to wait for exchange or consumer.
//
// 2. Enters a suspended section where the Driver is on thread but is not
// counted as running and is therefore instantaneously cancellable and pausable.
// Comes back on thread after a timed pause. This simulates an RPC to an out of
// process service.
//
// 3.  Enters a suspended section where this pauses and resumes random Tasks,
// including its own Task. This simulates making Tasks release memory under
// memory contention, checkpointing Tasks for migration or fault tolerance and
// other process-wide coordination activities.
//
// These situations will occur with arbitrary concurrency and sequence and must
// therefore be in one test to check against deadlocks.
class TestingPauser : public Operator {
 public:
  TestingPauser(
      DriverCtx* ctx,
      int32_t id,
      std::shared_ptr<const TestingPauserNode> node,
      DriverTest* test,
      int32_t sequence)
      : Operator(ctx, node->outputType(), id, node->id(), "Pauser"),
        test_(test),
        counter_(sequence) {
    test_->registerTask(operatorCtx_->task());
  }

  bool needsInput() const override {
    return !noMoreInput_ && !input_;
  }

  void addInput(RowVectorPtr input) override {
    input_ = std::move(input);
  }

  void noMoreInput() override {
    test_->unregisterTask(operatorCtx_->task());
    Operator::noMoreInput();
  }

  RowVectorPtr getOutput() override {
    if (!input_) {
      return nullptr;
    }
    ++counter_;
    auto label = operatorCtx_->driver()->label();
    // Block for a time quantum evern 10th time.
    if (counter_ % 10 == 0) {
      test_->registerForWakeup(&future_);
      return nullptr;
    }
    {
      TestSuspendedSection noCancel(operatorCtx_->driver());
      sleep(1);
      if (counter_ % 7 == 0) {
        // Every 7th time, stop and resume other Tasks. This operation is
        // globally serialized.
        std::lock_guard<std::mutex> l(pauseMutex_);

        for (auto i = 0; i <= counter_ % 3; ++i) {
          auto task = test_->randomTask();
          if (!task) {
            continue;
          }
          auto& executor = folly::QueuedImmediateExecutor::instance();
          auto future = task->requestPause().via(&executor);
          future.wait();
          sleep(2);
          Task::resume(task);
        }
      }
    }

    return std::move(input_);
  }

  BlockingReason isBlocked(ContinueFuture* future) override {
    VELOX_CHECK(!operatorCtx_->driver()->state().suspended());
    if (future_.valid()) {
      *future = std::move(future_);
      return BlockingReason::kWaitForConsumer;
    }
    return BlockingReason::kNotBlocked;
  }

  bool isFinished() override {
    return noMoreInput_ && input_ == nullptr;
  }

 private:
  void sleep(int32_t units) {
    // NOLINT
    std::this_thread::sleep_for(std::chrono::milliseconds(units));
  }

  // The DriverTest under which this is running. Used for global context.
  DriverTest* test_;

  // Mutex to serialize the pause/restart exercise so that only one instance
  // does this at a time.
  static std::mutex pauseMutex_;

  // Counter deciding the next action in getOutput().
  int32_t counter_;
  ContinueFuture future_;
};

std::mutex TestingPauser ::pauseMutex_;

namespace {

class PauserNodeFactory : public Operator::PlanNodeTranslator {
 public:
  PauserNodeFactory(
      uint32_t maxDrivers,
      std::atomic<int32_t>& sequence,
      DriverTest* testInstance)
      : maxDrivers_{maxDrivers},
        sequence_{sequence},
        testInstance_{testInstance} {}

  std::unique_ptr<Operator> toOperator(
      DriverCtx* ctx,
      int32_t id,
      const core::PlanNodePtr& node) override {
    if (auto pauser =
            std::dynamic_pointer_cast<const TestingPauserNode>(node)) {
      return std::make_unique<TestingPauser>(
          ctx, id, pauser, testInstance_, ++sequence_);
    }
    return nullptr;
  }

  std::optional<uint32_t> maxDrivers(const core::PlanNodePtr& node) override {
    if (auto pauser =
            std::dynamic_pointer_cast<const TestingPauserNode>(node)) {
      return maxDrivers_;
    }
    return std::nullopt;
  }

 private:
  uint32_t maxDrivers_;
  std::atomic<int32_t>& sequence_;
  DriverTest* testInstance_;
};

} // namespace

TEST_F(DriverTest, pauserNode) {
  constexpr int32_t kNumTasks = 20;
  constexpr int32_t kThreadsPerTask = 5;
  // Run with a fraction of the testing threads fitting in the executor.
  auto executor = std::make_shared<folly::CPUThreadPoolExecutor>(20);
  static std::atomic<int32_t> sequence{0};
  // Use a static variable to pass the test instance to the create
  // function of the testing operator. The testing operator registers
  // all its Tasks in the test instance to create inter-Task pauses.
  static DriverTest* testInstance;
  testInstance = this;
  Operator::registerOperator(std::make_unique<PauserNodeFactory>(
      kThreadsPerTask, sequence, testInstance));

  std::vector<CursorParameters> params(kNumTasks);
  int32_t hits{0};
  for (int32_t i = 0; i < kNumTasks; ++i) {
    params[i].queryCtx = core::QueryCtx::create(executor.get());
    params[i].planNode = makeValuesFilterProject(
        rowType_,
        "m1 % 10 > 0",
        "m1 % 3 + m2 % 5 + m3 % 7 + m4 % 11 + m5 % 13 + m6 % 17 + m7 % 19",
        200,
        2'000,
        [](int64_t num) { return num % 10 > 0; },
        &hits,
        true);
    params[i].maxDrivers =
        kThreadsPerTask * 2; // a number larger than kThreadsPerTask
  }
  std::vector<int32_t> counters(kNumTasks, 0);
  std::vector<std::thread> threads;
  threads.reserve(kNumTasks);
  for (int32_t i = 0; i < kNumTasks; ++i) {
    threads.push_back(std::thread([this, &params, &counters, i]() {
      try {
        readResults(params[i], ResultOperation::kRead, 10'000, &counters[i], i);
      } catch (const std::exception& e) {
        LOG(INFO) << "Pauser task errored out " << e.what();
      }
    }));
  }
  for (int32_t i = 0; i < kNumTasks; ++i) {
    threads[i].join();
    EXPECT_EQ(counters[i], kThreadsPerTask * hits);
    EXPECT_TRUE(stateFutures_.at(i).isReady());
  }
  tasks_.clear();
}

namespace {

// Custom node for the custom factory.
class ThrowNode : public core::PlanNode {
 public:
  enum class OperatorMethod {
    kIsBlocked,
    kNeedsInput,
    kAddInput,
    kNoMoreInput,
    kGetOutput,
  };

  ThrowNode(
      const core::PlanNodeId& id,
      OperatorMethod throwingMethod,
      core::PlanNodePtr input)
      : PlanNode(id), throwingMethod_{throwingMethod}, sources_{input} {}

  const RowTypePtr& outputType() const override {
    return sources_[0]->outputType();
  }

  OperatorMethod throwingMethod() const {
    return throwingMethod_;
  }

  const std::vector<std::shared_ptr<const PlanNode>>& sources() const override {
    return sources_;
  }

  std::string_view name() const override {
    return "Throw";
  }

 private:
  void addDetails(std::stringstream& /* stream */) const override {}

  const OperatorMethod throwingMethod_;
  std::vector<core::PlanNodePtr> sources_;
};

// Custom operator for the custom factory.
class ThrowOperator : public Operator {
 public:
  ThrowOperator(
      DriverCtx* ctx,
      int32_t id,
      const std::shared_ptr<const ThrowNode>& node)
      : Operator(ctx, node->outputType(), id, node->id(), "Throw"),
        throwingMethod_{node->throwingMethod()} {}

  bool needsInput() const override {
    if (throwingMethod_ == ThrowNode::OperatorMethod::kNeedsInput) {
      // Trigger a std::bad_function_call exception.
      std::function<bool(vector_size_t)> nullFunction = nullptr;

      if (nullFunction(123)) {
        return false;
      }
    }
    return !noMoreInput_ && !input_;
  }

  void addInput(RowVectorPtr input) override {
    if (throwingMethod_ == ThrowNode::OperatorMethod::kAddInput) {
      // Trigger a std::bad_function_call exception.
      std::function<bool(vector_size_t)> nullFunction = nullptr;

      if (nullFunction(input->size())) {
        input_ = std::move(input);
      }
    }

    input_ = std::move(input);
  }

  void noMoreInput() override {
    if (throwingMethod_ == ThrowNode::OperatorMethod::kNoMoreInput) {
      // Trigger a std::bad_function_call exception.
      std::function<bool()> nullFunction = nullptr;

      if (nullFunction()) {
        Operator::noMoreInput();
      }
    }

    Operator::noMoreInput();
  }

  RowVectorPtr getOutput() override {
    if (throwingMethod_ == ThrowNode::OperatorMethod::kGetOutput) {
      // Trigger a std::bad_function_call exception.
      std::function<bool()> nullFunction = nullptr;

      if (nullFunction()) {
        return std::move(input_);
      }
    }
    return std::move(input_);
  }

  BlockingReason isBlocked(ContinueFuture* /*future*/) override {
    if (throwingMethod_ == ThrowNode::OperatorMethod::kIsBlocked) {
      // Trigger a std::bad_function_call exception.
      std::function<bool()> nullFunction = nullptr;

      if (nullFunction()) {
        return BlockingReason::kWaitForMemory;
      }
    }
    return BlockingReason::kNotBlocked;
  }

  bool isFinished() override {
    return noMoreInput_ && input_ == nullptr;
  }

 private:
  const ThrowNode::OperatorMethod throwingMethod_;
};

// Custom factory that throws during driver creation.
class ThrowNodeFactory : public Operator::PlanNodeTranslator {
 public:
  explicit ThrowNodeFactory(uint32_t maxDrivers) : maxDrivers_{maxDrivers} {}

  std::unique_ptr<Operator> toOperator(
      DriverCtx* ctx,
      int32_t id,
      const core::PlanNodePtr& node) override {
    if (auto throwNode = std::dynamic_pointer_cast<const ThrowNode>(node)) {
      VELOX_CHECK_LT(driversCreated, maxDrivers_, "Too many drivers");
      ++driversCreated;
      return std::make_unique<ThrowOperator>(ctx, id, throwNode);
    }
    return nullptr;
  }

  std::optional<uint32_t> maxDrivers(const core::PlanNodePtr& node) override {
    if (std::dynamic_pointer_cast<const ThrowNode>(node)) {
      return 5;
    }
    return std::nullopt;
  }

 private:
  const uint32_t maxDrivers_;
  uint32_t driversCreated{0};
};

class BlockedNoFutureNode : public core::PlanNode {
 public:
  BlockedNoFutureNode(
      const core::PlanNodeId& id,
      const core::PlanNodePtr& input)
      : PlanNode(id), sources_{input} {}

  const RowTypePtr& outputType() const override {
    return sources_[0]->outputType();
  }

  const std::vector<std::shared_ptr<const PlanNode>>& sources() const override {
    return sources_;
  }

  std::string_view name() const override {
    return "BlockedNoFuture";
  }

 private:
  void addDetails(std::stringstream& /* stream */) const override {}
  std::vector<core::PlanNodePtr> sources_;
};

class BlockedNoFutureOperator : public Operator {
 public:
  BlockedNoFutureOperator(
      DriverCtx* ctx,
      int32_t id,
      const std::shared_ptr<const BlockedNoFutureNode>& node)
      : Operator(ctx, node->outputType(), id, node->id(), "BlockedNoFuture") {}

  bool needsInput() const override {
    return !noMoreInput_ && !input_;
  }

  void addInput(RowVectorPtr input) override {
    input_ = std::move(input);
  }

  RowVectorPtr getOutput() override {
    return std::move(input_);
  }

  bool isFinished() override {
    return noMoreInput_ && input_ == nullptr;
  }

  BlockingReason isBlocked(ContinueFuture* /*future*/) override {
    // Report being blocked, but do not set the future to trigger the error.
    return BlockingReason::kYield;
  }
};

class BlockedNoFutureNodeFactory : public Operator::PlanNodeTranslator {
 public:
  std::unique_ptr<Operator> toOperator(
      DriverCtx* ctx,
      int32_t id,
      const core::PlanNodePtr& node) override {
    return std::make_unique<BlockedNoFutureOperator>(
        ctx, id, std::dynamic_pointer_cast<const BlockedNoFutureNode>(node));
  }

  std::optional<uint32_t> maxDrivers(const core::PlanNodePtr& node) override {
    return 1;
  }
};
} // namespace

// Use a node for which driver factory would throw on any driver beyond id 0.
// This is to test that we do not crash due to early driver destruction and we
// have a proper error being propagated out.
TEST_F(DriverTest, driverCreationThrow) {
  Operator::registerOperator(std::make_unique<ThrowNodeFactory>(1));

  auto rows = makeRowVector({makeFlatVector<int32_t>({1, 2, 3})});

  auto plan = PlanBuilder()
                  .values({rows}, true)
                  .addNode([](const core::PlanNodeId& id,
                              const core::PlanNodePtr& input) {
                    return std::make_shared<ThrowNode>(
                        id, ThrowNode::OperatorMethod::kAddInput, input);
                  })
                  .planNode();
  CursorParameters params;
  params.planNode = plan;
  params.maxDrivers = 5;
  auto cursor = TaskCursor::create(params);
  auto task = cursor->task();
  // Ensure execution threw correct error.
  VELOX_ASSERT_THROW(cursor->moveNext(), "Too many drivers");
  EXPECT_EQ(TaskState::kFailed, task->state());
}

TEST_F(DriverTest, blockedNoFuture) {
  Operator::registerOperator(std::make_unique<BlockedNoFutureNodeFactory>());

  auto rows = makeRowVector({makeFlatVector<int32_t>({1, 2, 3})});

  auto plan = PlanBuilder()
                  .values({rows}, true)
                  .addNode([](const core::PlanNodeId& id,
                              const core::PlanNodePtr& input) {
                    return std::make_shared<BlockedNoFutureNode>(id, input);
                  })
                  .planNode();
  // Ensure execution threw correct error.
  VELOX_ASSERT_THROW(
      AssertQueryBuilder(plan).copyResults(pool()),
      "The operator BlockedNoFuture is blocked but blocking future is not valid");
}

TEST_F(DriverTest, nonVeloxOperatorException) {
  Operator::registerOperator(
      std::make_unique<ThrowNodeFactory>(std::numeric_limits<uint32_t>::max()));

  auto rows = makeRowVector({makeFlatVector<int32_t>({1, 2, 3})});

  auto makePlan = [&](ThrowNode::OperatorMethod throwingMethod) {
    return PlanBuilder()
        .values({rows}, true)
        .addNode([throwingMethod](std::string id, core::PlanNodePtr input) {
          return std::make_shared<ThrowNode>(id, throwingMethod, input);
        })
        .planNode();
  };

  VELOX_ASSERT_THROW(
      AssertQueryBuilder(makePlan(ThrowNode::OperatorMethod::kIsBlocked))
          .copyResults(pool()),
      "Operator::isBlocked failed for [operator: Throw, plan node ID: 1]");

  VELOX_ASSERT_THROW(
      AssertQueryBuilder(makePlan(ThrowNode::OperatorMethod::kNeedsInput))
          .copyResults(pool()),
      "Operator::needsInput failed for [operator: Throw, plan node ID: 1]");

  VELOX_ASSERT_THROW(
      AssertQueryBuilder(makePlan(ThrowNode::OperatorMethod::kAddInput))
          .copyResults(pool()),
      "Operator::addInput failed for [operator: Throw, plan node ID: 1]");

  VELOX_ASSERT_THROW(
      AssertQueryBuilder(makePlan(ThrowNode::OperatorMethod::kNoMoreInput))
          .copyResults(pool()),
      "Operator::noMoreInput failed for [operator: Throw, plan node ID: 1]");

  VELOX_ASSERT_THROW(
      AssertQueryBuilder(makePlan(ThrowNode::OperatorMethod::kGetOutput))
          .copyResults(pool()),
      "Operator::getOutput failed for [operator: Throw, plan node ID: 1]");
}

TEST_F(DriverTest, enableOperatorBatchSizeStatsConfig) {
  CursorParameters params;
  int32_t hits;
  params.planNode = makeValuesFilterProject(
      rowType_,
      "m1 % 10 > 0",
      "m1 % 3 + m2 % 5 + m3 % 7 + m4 % 11 + m5 % 13 + m6 % 17 + m7 % 19",
      100,
      1'000,
      [](int64_t num) { return num % 10 > 0; },
      &hits);
  params.maxDrivers = 4;
  std::unordered_map<std::string, std::string> queryConfig{
      {core::QueryConfig::kEnableOperatorBatchSizeStats, "true"}};
  params.queryCtx = core::QueryCtx::create(
      executor_.get(), core::QueryConfig(std::move(queryConfig)));
  int32_t numRead = 0;
  readResults(params, ResultOperation::kRead, 1'000'000, &numRead);
  EXPECT_EQ(numRead, 4 * hits);
  auto stateFuture = tasks_[0]->taskCompletionFuture().within(
      std::chrono::microseconds(100'000'000));
  auto& executor = folly::QueuedImmediateExecutor::instance();
  auto state = std::move(stateFuture).via(&executor);
  state.wait();
  EXPECT_TRUE(tasks_[0]->isFinished());
  EXPECT_EQ(tasks_[0]->numRunningDrivers(), 0);
  const auto taskStats = tasks_[0]->taskStats();
  ASSERT_EQ(taskStats.pipelineStats.size(), 1);
  const auto& operatorStats = taskStats.pipelineStats[0].operatorStats;
  EXPECT_GT(operatorStats[1].getOutputTiming.wallNanos, 0);
  EXPECT_EQ(operatorStats[0].outputPositions, 400'000);
  EXPECT_GT(operatorStats[0].outputBytes, 0);
  EXPECT_EQ(operatorStats[1].inputPositions, 400'000);
  EXPECT_EQ(operatorStats[1].outputPositions, 4 * hits);
  EXPECT_GT(operatorStats[1].outputBytes, 0);
}

DEBUG_ONLY_TEST_F(DriverTest, driverSuspensionRaceWithTaskPause) {
  struct {
    int numDrivers;
    bool enterSuspensionAfterPauseStarted;
    bool leaveSuspensionDuringPause;

    std::string debugString() const {
      return fmt::format(
          "numDrivers:{} enterSuspensionAfterPauseStarted:{} leaveSuspensionDuringPause:{}",
          numDrivers,
          enterSuspensionAfterPauseStarted,
          leaveSuspensionDuringPause);
    }
  } testSettings[] = {
      // This test case (1) uses TestValue to block one of the task driver
      // threads when it processes the input values; (2) pauses the task; (3)
      // enters suspension from the blocked driver thread; (4) tries to leave
      // the suspension from the blocked driver thread while the task is paused
      // and expects the suspension leave is busy waiting; (5) resumes the task
      // and expects the task to complete successfully.
      {1, true, true},
      // The same as above with different number of driver threads.
      {4, true, true},
      // This test case (1) uses TestValue to block one of the task driver
      // threads when it processes the input values; (2) enters suspension from
      // the blocked driver thread; (3) pauses the task; (4) tries to leave the
      // suspension from the blocked driver thread while the task is paused and
      // expects the suspension leave is busy waiting; (5) resumes the task and
      // expects the task to complete successfully.
      {1, false, true},
      // The same as above with different number of driver threads.
      {4, false, true},
      // This test case (1) uses TestValue to block one of the task driver
      // threads when it processes the input values; (2) enters suspension from
      // the blocked driver thread; (3) resumes the task; (4) leaves the
      // suspension from the blocked driver thread and expects the task to
      // complete successfully.
      {1, false, false},
      // The same as above with different number of driver threads.
      {4, false, false},
      // This test case (1) uses TestValue to block one of the task driver
      // threads when it processes the input values; (2) pauses the task; (3)
      // enters suspension from the blocked driver thread; (4) resumes the task;
      // (5) leaves the suspension from the blocked driver thread and expects
      // the task to complete
      {1, true, false},
      // The same as above with different number of driver threads.
      {4, true, false}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    std::shared_ptr<Task> task;
    if (testData.enterSuspensionAfterPauseStarted &&
        testData.leaveSuspensionDuringPause) {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          StopReason::kNone,
          StopReason::kNone,
          TaskState::kFinished,
          [&](Task* task) { task->requestPause(); },
          [&](Task* task) { task->requestPause().wait(); },
          [&](Task* task) {
            // Let the suspension leave thread to run first.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bool hasSuspendedDriver{false};
            task->testingVisitDrivers([&](Driver* driver) {
              hasSuspendedDriver |= driver->state().suspended();
            });
            ASSERT_TRUE(hasSuspendedDriver);
            Task::resume(task->shared_from_this());
          });
    } else if (
        testData.enterSuspensionAfterPauseStarted &&
        !testData.leaveSuspensionDuringPause) {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          StopReason::kNone,
          StopReason::kNone,
          TaskState::kFinished,
          [&](Task* task) { task->requestPause(); },
          [&](Task* task) {
            task->requestPause().wait();
            Task::resume(task->shared_from_this());
          });
    } else if (
        !testData.enterSuspensionAfterPauseStarted &&
        testData.leaveSuspensionDuringPause) {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          StopReason::kNone,
          StopReason::kNone,
          TaskState::kFinished,
          nullptr,
          [&](Task* task) { task->requestPause().wait(); },
          [&](Task* task) {
            // Let the suspension leave thread to run first.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bool hasSuspendedDriver{false};
            task->testingVisitDrivers([&](Driver* driver) {
              hasSuspendedDriver |= driver->state().suspended();
            });
            ASSERT_TRUE(hasSuspendedDriver);
            Task::resume(task->shared_from_this());
          });
    } else {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          StopReason::kNone,
          StopReason::kNone,
          TaskState::kFinished,
          nullptr,
          [&](Task* task) {
            task->requestPause().wait();
            Task::resume(task->shared_from_this());
          });
    }
  }
}

DEBUG_ONLY_TEST_F(DriverTest, driverSuspensionRaceWithTaskTerminate) {
  struct {
    int numDrivers;
    bool enterSuspensionAfterTaskTerminated;
    bool abort;
    StopReason expectedEnterSuspensionStopReason;
    std::optional<StopReason> expectedLeaveSuspensionStopReason;

    std::string debugString() const {
      return fmt::format(
          "numDrivers:{} enterSuspensionAfterTaskTerminated:{} abort {} expectedEnterSuspensionStopReason:{} expectedLeaveSuspensionStopReason:{}",
          numDrivers,
          enterSuspensionAfterTaskTerminated,
          abort,
          expectedEnterSuspensionStopReason,
          expectedLeaveSuspensionStopReason.has_value()
              ? stopReasonString(expectedLeaveSuspensionStopReason.value())
              : "NULL");
    }
  } testSettings[] = {
      // This test case (1) uses TestValue to block one of the task driver
      // threads when it processes the input values; (2) terminates the task by
      // cancel; (3) enters suspension from the blocked driver thread and
      // expects to get kAlreadyTerminated stop reason as the task has been
      // terminated; (4) leaves the suspension from the blocked driver thread
      // and expects the same stop reason; (5) wait and expects the task to be
      // aborted.
      {1, true, true, StopReason::kAlreadyTerminated, std::nullopt},
      // The same as above with different number of driver threads.
      {4, true, true, StopReason::kAlreadyTerminated, std::nullopt},
      // This test case (1) uses TestValue to block one of the task driver
      // threads when it processes the input values; (2) enters suspension from
      // the blocked driver thread and expects to get kNone stop reason as the
      // task is still running; (3) terminates the task by cancel; (4) leaves
      // the suspension from the blocked driver thread and expects
      // kAlreadyTerminated stop reason as the task has been terminated same
      // stop reason; (5) wait and expects the task to be aborted.
      {1, false, true, StopReason::kNone, StopReason::kAlreadyTerminated},
      // The same as above with different number of driver threads.
      {4, false, true, StopReason::kNone, StopReason::kAlreadyTerminated},

      // Repeated the above test cases by terminating task by cancel.
      {1, true, false, StopReason::kAlreadyTerminated, std::nullopt},
      {4, true, false, StopReason::kAlreadyTerminated, std::nullopt},
      {1, false, false, StopReason::kNone, StopReason::kAlreadyTerminated},
      {4, false, false, StopReason::kNone, StopReason::kAlreadyTerminated}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    if (testData.enterSuspensionAfterTaskTerminated) {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          testData.expectedEnterSuspensionStopReason,
          testData.expectedLeaveSuspensionStopReason,
          testData.abort ? TaskState::kAborted : TaskState::kCanceled,
          [&](Task* task) {
            if (testData.abort) {
              task->requestAbort();
            } else {
              task->requestCancel();
            }
          });
    } else {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          testData.expectedEnterSuspensionStopReason,
          testData.expectedLeaveSuspensionStopReason,
          testData.abort ? TaskState::kAborted : TaskState::kCanceled,
          nullptr,
          [&](Task* task) {
            if (testData.abort) {
              task->requestAbort().wait();
            } else {
              task->requestCancel().wait();
            }
          });
    }
  }
}

DEBUG_ONLY_TEST_F(DriverTest, driverSuspensionRaceWithTaskYield) {
  struct {
    int numDrivers;
    bool enterSuspensionAfterTaskYielded;
    bool leaveSuspensionDuringTaskYielded;

    std::string debugString() const {
      return fmt::format(
          "numDrivers:{} enterSuspensionAfterTaskYielded:{} leaveSuspensionDuringTaskYielded:{}",
          numDrivers,
          enterSuspensionAfterTaskYielded,
          leaveSuspensionDuringTaskYielded);
    }
  } testSettings[] = {
      {1, true, true},
      {4, true, true},
      {1, false, true},
      {4, false, true},
      {1, true, false},
      {4, true, false}};
  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());

    std::shared_ptr<Task> task;
    if (testData.enterSuspensionAfterTaskYielded &&
        testData.leaveSuspensionDuringTaskYielded) {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          StopReason::kNone,
          StopReason::kNone,
          TaskState::kFinished,
          [&](Task* task) { task->requestYield(); },
          [&](Task* task) { task->requestYield(); });
    } else if (
        testData.enterSuspensionAfterTaskYielded &&
        !testData.leaveSuspensionDuringTaskYielded) {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          StopReason::kNone,
          StopReason::kNone,
          TaskState::kFinished,
          [&](Task* task) { task->requestYield(); });
    } else if (
        !testData.enterSuspensionAfterTaskYielded &&
        testData.leaveSuspensionDuringTaskYielded) {
      testDriverSuspensionWithTaskOperationRace(
          testData.numDrivers,
          StopReason::kNone,
          StopReason::kNone,
          TaskState::kFinished,
          nullptr,
          [&](Task* task) { task->requestYield(); });
    }
  }
}

DEBUG_ONLY_TEST_F(DriverTest, driverSuspensionCalledFromOffThread) {
  std::shared_ptr<Driver> driver;
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        driver = values->operatorCtx()->driver()->shared_from_this();
      }));

  auto task = createAndStartTaskToReadValues(1);

  ASSERT_TRUE(waitForTaskCompletion(task.get(), 100'000'000));
  while (driver->isOnThread()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  VELOX_ASSERT_THROW(driver->task()->enterSuspended(driver->state()), "");
  VELOX_ASSERT_THROW(driver->task()->leaveSuspended(driver->state()), "");
}

// This test case verifies that the driver thread leaves suspended state after
// task termiates and before resuming.
DEBUG_ONLY_TEST_F(DriverTest, driverSuspendedAfterTaskTerminateBeforeResume) {
  std::shared_ptr<Driver> driver;
  std::atomic_bool triggerSuspended{false};
  std::atomic_bool taskPaused{false};
  // std::atomic_bool driverExecutionWaitFlag{true};
  folly::EventCount taskPausedWait;
  std::atomic_bool driverLeaveSuspended{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        if (triggerSuspended.exchange(true)) {
          return;
        }
        driver = values->operatorCtx()->driver()->shared_from_this();
        driver->task()->enterSuspended(driver->state());
        driver->task()->requestPause().wait();
        taskPaused = true;
        taskPausedWait.notifyAll();
        const StopReason ret = driver->task()->leaveSuspended(driver->state());
        ASSERT_EQ(ret, StopReason::kAlreadyTerminated);
        driverLeaveSuspended = true;
      }));

  auto task = createAndStartTaskToReadValues(1);

  taskPausedWait.await([&]() { return taskPaused.load(); });
  task->requestCancel().wait();
  // Wait for 1 second and check the driver is still under suspended state
  // without resuming.
  std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
  ASSERT_FALSE(driverLeaveSuspended);

  Task::resume(task);
  std::this_thread::sleep_for(std::chrono::milliseconds(1'000));
  // Check the driver leaves the suspended state after task is resumed. Wait for
  // 1 second to avoid timing flakiness.
  ASSERT_TRUE(driverLeaveSuspended);

  ASSERT_TRUE(waitForTaskCancelled(task.get(), 100'000'000));
}

DEBUG_ONLY_TEST_F(DriverTest, driverThreadContext) {
  ASSERT_TRUE(driverThreadContext() == nullptr);
  std::thread nonDriverThread(
      [&]() { ASSERT_TRUE(driverThreadContext() == nullptr); });
  nonDriverThread.join();

  std::atomic<Task*> capturedTask{nullptr};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        ASSERT_TRUE(driverThreadContext() != nullptr);
        capturedTask = driverThreadContext()->driverCtx()->task.get();
      }));
  std::vector<RowVectorPtr> batches;
  for (int i = 0; i < 4; ++i) {
    batches.push_back(
        makeRowVector({"c0"}, {makeFlatVector<int32_t>({1, 2, 3})}));
  }
  createDuckDbTable(batches);

  auto plan = PlanBuilder().values(batches).planNode();
  auto task = AssertQueryBuilder(plan, duckDbQueryRunner_)
                  .assertResults("SELECT * FROM tmp");
  ASSERT_EQ(task.get(), capturedTask);
}

DEBUG_ONLY_TEST_F(DriverTest, nonReclaimableSection) {
  // The driver framework will set non-reclaimable section flag when start
  // executing operator method.
  // Checks before getOutput method called.
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        ASSERT_FALSE(values->testingNonReclaimable());
      }));
  // Checks inside getOutput method execution.
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        ASSERT_TRUE(values->testingNonReclaimable());
      }));

  std::vector<RowVectorPtr> batches;
  for (int i = 0; i < 2; ++i) {
    batches.push_back(makeRowVector({makeFlatVector<int32_t>({1, 2, 3})}));
  }
  auto plan = PlanBuilder().values(batches).planNode();
  ASSERT_NO_THROW(AssertQueryBuilder(plan).copyResults(pool()));
}

DEBUG_ONLY_TEST_F(DriverTest, driverCpuTimeSlicingCheck) {
  const int numBatches = 3;
  std::vector<RowVectorPtr> batches;
  for (int i = 0; i < numBatches; ++i) {
    batches.push_back(
        makeRowVector({"c0"}, {makeFlatVector<int32_t>({1, 2, 3})}));
  }

  struct TestParam {
    bool hasCpuTimeSliceLimit;
    Task::ExecutionMode executionMode;
  };
  std::vector<TestParam> testParams{
      {true, Task::ExecutionMode::kParallel},
      {false, Task::ExecutionMode::kParallel},
      {true, Task::ExecutionMode::kSerial},
      {false, Task::ExecutionMode::kSerial}};

  for (const auto& testParam : testParams) {
    SCOPED_TRACE(
        fmt::format("hasCpuSliceLimit: {}", testParam.hasCpuTimeSliceLimit));
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Values::getOutput",
        std::function<void(const exec::Values*)>(
            [&](const exec::Values* values) {
              // Verify that no matter driver cpu time slicing is enforced or
              // not, the driver start execution time is set properly.
              ASSERT_NE(
                  values->operatorCtx()->driver()->state().startExecTimeMs, 0);
              if (testParam.hasCpuTimeSliceLimit) {
                std::this_thread::sleep_for(std::chrono::seconds(1)); // NOLINT
                ASSERT_GT(
                    values->operatorCtx()->driver()->state().execTimeMs(), 0);
              }
            }));
    auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
    auto fragment =
        PlanBuilder(planNodeIdGenerator).values(batches).planFragment();
    std::unordered_map<std::string, std::string> queryConfig;
    if (testParam.hasCpuTimeSliceLimit) {
      queryConfig.emplace(core::QueryConfig::kDriverCpuTimeSliceLimitMs, "500");
    }
    const uint64_t oldYieldCount = Driver::yieldCount();

    std::shared_ptr<Task> task;
    if (testParam.executionMode == Task::ExecutionMode::kParallel) {
      task = Task::create(
          "t0",
          fragment,
          0,
          core::QueryCtx::create(
              driverExecutor_.get(), core::QueryConfig{std::move(queryConfig)}),
          testParam.executionMode,
          [](RowVectorPtr /*unused*/,
             bool drained,
             ContinueFuture* /*unused*/) {
            VELOX_CHECK(!drained);
            return exec::BlockingReason::kNotBlocked;
          });
      task->start(1, 1);
    } else {
      task = Task::create(
          "t0",
          fragment,
          0,
          core::QueryCtx::create(
              driverExecutor_.get(), core::QueryConfig{std::move(queryConfig)}),
          testParam.executionMode);
      while (task->next() != nullptr) {
      }
    }

    ASSERT_TRUE(waitForTaskCompletion(task.get(), 600'000'000));
    if (testParam.hasCpuTimeSliceLimit &&
        testParam.executionMode == Task::ExecutionMode::kParallel) {
      // NOTE: there is one additional yield for the empty output.
      ASSERT_GE(Driver::yieldCount(), oldYieldCount + numBatches + 1);
    } else {
      ASSERT_EQ(Driver::yieldCount(), oldYieldCount);
    }
  }
}

namespace {

template <typename T>
struct ThrowRuntimeExceptionFunction {
  template <typename TResult, typename TInput>
  void call(TResult& out, const TInput& in) {
    VELOX_CHECK(false, "Throwing exception");
  }
};
} // namespace

TEST_F(DriverTest, additionalContextInRuntimeException) {
  // Ensures that exceptions thrown during execution of an operator contain the
  // expected context. This is done by executing a plan using project filter
  // that uses expressions which setup hierarchical contexts. Finally, we verify
  // that all essential context are present.
  auto vector = makeRowVector({makeFlatVector<int64_t>({1, 2, 3, 4, 5, 6})});
  registerFunction<ThrowRuntimeExceptionFunction, int64_t, int64_t>(
      {"throwException"});
  auto op = PlanBuilder(std::make_shared<core::PlanNodeIdGenerator>(13))
                .values({vector})
                .project({"c0 + throwException(c0)"})
                .planNode();
  try {
    assertQuery(op, vector);
  } catch (VeloxException& e) {
    ASSERT_EQ(e.context(), "throwexception(c0)");
    auto additionalContext = e.additionalContext();
    // Remove the string following `TaskId` from the additional context since
    // its indeterministic.
    ASSERT_EQ(
        additionalContext,
        "Top-level Expression: plus(c0, throwexception(c0)) Operator: "
        "FilterProject[14] 1");
  }
}

class OpCallStatusTest : public OperatorTestBase {};

// Test that the opCallStatus is returned properly and formats the call as
// expected.
TEST_F(OpCallStatusTest, basic) {
  std::vector<RowVectorPtr> data{
      makeRowVector({"c0"}, {makeFlatVector<int32_t>({1, 2, 3})})};

  const int firstNodeId{17};
  auto planNodeIdGenerator =
      std::make_shared<core::PlanNodeIdGenerator>(firstNodeId);
  auto fragment = PlanBuilder(planNodeIdGenerator).values(data).planFragment();

  std::unordered_map<std::string, std::string> queryConfig;
  auto task = Task::create(
      "t19",
      fragment,
      0,
      core::QueryCtx::create(
          driverExecutor_.get(), core::QueryConfig{std::move(queryConfig)}),
      Task::ExecutionMode::kParallel,
      [](RowVectorPtr /*unused*/, bool drained, ContinueFuture* /*unused*/) {
        VELOX_CHECK(!drained);
        return exec::BlockingReason::kNotBlocked;
      });

  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto* driver = values->operatorCtx()->driver();
        auto ocs = driver->opCallStatus();
        // Check osc to be not empty and the correct format.
        EXPECT_FALSE(ocs.empty());
        const auto formattedOpCall =
            ocs.formatCall(driver->findOperatorNoThrow(ocs.opId), ocs.method);
        EXPECT_EQ(
            formattedOpCall,
            fmt::format("Values.{}::{}", firstNodeId, ocs.method));
        // Check the correct format when operator is not found.
        ocs.method = "randomName";
        EXPECT_EQ(
            ocs.formatCall(
                driver->findOperatorNoThrow(ocs.opId + 10), ocs.method),
            fmt::format("null::{}", ocs.method));

        // Check that the task returns correct long running op call.
        std::vector<Task::OpCallInfo> stuckCalls;
        const std::chrono::milliseconds lockTimeoutMs(10);
        task->getLongRunningOpCalls(lockTimeoutMs, 10, stuckCalls);
        EXPECT_EQ(stuckCalls.size(), 1);
        if (!stuckCalls.empty()) {
          const auto& stuckCall = stuckCalls[0];
          EXPECT_EQ(stuckCall.opId, ocs.opId);
          EXPECT_GE(stuckCall.durationMs, 100);
          EXPECT_EQ(stuckCall.tid, driver->state().tid);
          EXPECT_EQ(stuckCall.taskId, task->taskId());
          EXPECT_EQ(stuckCall.opCall, formattedOpCall);
        }
      }));

  task->start(1, 1);
  ASSERT_TRUE(waitForTaskCompletion(task.get(), 600'000'000));
  task.reset();
  waitForAllTasksToBeDeleted();
};

// This test verifies that TestSuspendedSection dtor won't throw with a
// terminated task. Otherwise, it might cause server crash in production use
// case.
DEBUG_ONLY_TEST_F(DriverTest, suspendedSectionLeaveWithTerminatedTask) {
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        auto* driver = values->operatorCtx()->driver();
        TestSuspendedSection suspendedSection(driver);
        {
          ASSERT_TRUE(driver->state().suspended());
          TestSuspendedSection suspendedSection(driver);
          ASSERT_TRUE(driver->state().suspended());
          values->operatorCtx()->task()->requestAbort();
        }
      }));

  auto task = createAndStartTaskToReadValues(1);
  task.reset();
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(DriverTest, recursiveSuspensionCheck) {
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        auto* driver = values->operatorCtx()->driver();
        {
          TestSuspendedSection suspendedSection1(driver);
          ASSERT_TRUE(driver->state().suspended());
          TestSuspendedSection suspendedSection2(driver);
          ASSERT_TRUE(driver->state().suspended());
          {
            ASSERT_TRUE(driver->state().suspended());
            TestSuspendedSection suspendedSection(driver);
            ASSERT_TRUE(driver->state().suspended());
          }
          ASSERT_TRUE(driver->state().suspended());
        }
        ASSERT_FALSE(driver->state().suspended());
        TestSuspendedSection suspendedSection(driver);
        ASSERT_TRUE(driver->state().suspended());
      }));

  createAndStartTaskToReadValues(1);
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_F(DriverTest, recursiveSuspensionThrow) {
  auto suspendDriverFn = [&](Driver* driver) {
    TestSuspendedSection suspendedSection(driver);
  };
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        auto* driver = values->operatorCtx()->driver();
        {
          TestSuspendedSection suspendedSection(driver);
          ASSERT_TRUE(driver->state().suspended());
          values->operatorCtx()->task()->requestAbort();
          {
            ASSERT_TRUE(driver->state().suspended());
            VELOX_ASSERT_THROW(suspendDriverFn(driver), "");
          }
          ASSERT_TRUE(driver->state().suspended());
        }
        ASSERT_FALSE(driver->state().suspended());
      }));

  createAndStartTaskToReadValues(1);
  waitForAllTasksToBeDeleted();
}
