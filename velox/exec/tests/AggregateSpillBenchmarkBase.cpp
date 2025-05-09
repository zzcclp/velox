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

#include "velox/exec/tests/AggregateSpillBenchmarkBase.h"
#include "velox/exec/GroupingSet.h"

#include <gflags/gflags.h>

using namespace facebook::velox;
using namespace facebook::velox::common;
using namespace facebook::velox::memory;
using namespace facebook::velox::exec;

namespace facebook::velox::exec::test {

namespace {
std::unique_ptr<RowContainer> makeRowContainer(
    const std::vector<TypePtr>& keyTypes,
    const std::vector<TypePtr>& dependentTypes,
    std::shared_ptr<velox::memory::MemoryPool>& pool) {
  return std::make_unique<RowContainer>(
      keyTypes,
      true, // nullableKeys
      std::vector<Accumulator>{},
      dependentTypes,
      false, // hasNext
      false, // isJoinBuild
      false, // hasProbedFlag
      false, // hasNormalizedKey
      pool.get());
}

std::unique_ptr<RowContainer> setupSpillContainer(
    const RowTypePtr& rowType,
    uint32_t numKeys,
    std::shared_ptr<velox::memory::MemoryPool>& pool) {
  const auto& childTypes = rowType->children();
  std::vector<TypePtr> keys(childTypes.begin(), childTypes.begin() + numKeys);
  std::vector<TypePtr> dependents;
  if (numKeys < childTypes.size()) {
    dependents.insert(
        dependents.end(), childTypes.begin() + numKeys, childTypes.end());
  }
  return makeRowContainer(keys, dependents, pool);
}
} // namespace

void AggregateSpillBenchmarkBase::setUp() {
  SpillerBenchmarkBase::setUp();

  rowContainer_ = setupSpillContainer(
      rowType_, FLAGS_spiller_benchmark_num_key_columns, pool_);
  writeSpillData();
  spiller_ = makeSpiller();
}

void AggregateSpillBenchmarkBase::run() {
  MicrosecondTimer timer(&executionTimeUs_);
  if (spillerType_ == std::string(AggregationInputSpiller::kType)) {
    auto aggregationInputSpiller =
        dynamic_cast<AggregationInputSpiller*>(spiller_.get());
    VELOX_CHECK_NOT_NULL(aggregationInputSpiller);
    aggregationInputSpiller->spill();
  } else {
    auto aggregationOutputSpiller =
        dynamic_cast<AggregationOutputSpiller*>(spiller_.get());
    VELOX_CHECK_NOT_NULL(aggregationOutputSpiller);
    aggregationOutputSpiller->spill(RowContainerIterator{});
  }
  rowContainer_->clear();
}

void AggregateSpillBenchmarkBase::printStats() const {
  LOG(INFO) << "======Aggregate " << spillerType_
            << " spilling statistics======";
  LOG(INFO) << "total execution time: " << succinctMicros(executionTimeUs_);
  LOG(INFO) << numInputVectors_ << " vectors each with " << inputVectorSize_
            << " rows have been processed";
  const auto memStats = memory::spillMemoryPool()->stats();
  LOG(INFO) << "peak memory usage[" << succinctBytes(memStats.peakBytes)
            << "] cumulative memory usage["
            << succinctBytes(memStats.cumulativeBytes) << "]";
  LOG(INFO) << spiller_->stats().toString();
  // List files under file path.
  SpillPartitionSet partitionSet;
  spiller_->finishSpill(partitionSet);
  const auto files = fs_->list(spillDir_);
  for (const auto& file : files) {
    auto rfile = fs_->openFileForRead(file);
    LOG(INFO) << "spilled file " << file << " size "
              << succinctBytes(rfile->size());
  }
}

void AggregateSpillBenchmarkBase::writeSpillData() {
  vector_size_t numRows = 0;
  for (const auto& rowVector : rowVectors_) {
    numRows += rowVector->size();
  }

  std::vector<char*> rows;
  rows.resize(numRows);
  for (int i = 0; i < numRows; ++i) {
    rows[i] = rowContainer_->newRow();
  }

  vector_size_t nextRow = 0;
  for (const auto& rowVector : rowVectors_) {
    const SelectivityVector allRows(rowVector->size());
    for (int index = 0; index < rowVector->size(); ++index, ++nextRow) {
      for (int i = 0; i < rowType_->size(); ++i) {
        DecodedVector decodedVector(*rowVector->childAt(i), allRows);
        rowContainer_->store(decodedVector, index, rows[nextRow], i);
      }
    }
  }
}

std::unique_ptr<SpillerBase> AggregateSpillBenchmarkBase::makeSpiller() {
  common::SpillConfig spillConfig;
  spillConfig.getSpillDirPathCb = [&]() -> std::string_view {
    return spillDir_;
  };
  spillConfig.updateAndCheckSpillLimitCb = [&](uint64_t) {};
  spillConfig.fileNamePrefix = FLAGS_spiller_benchmark_name;
  spillConfig.writeBufferSize = FLAGS_spiller_benchmark_write_buffer_size;
  spillConfig.executor = executor_.get();
  spillConfig.compressionKind =
      stringToCompressionKind(FLAGS_spiller_benchmark_compression_kind);
  spillConfig.maxSpillRunRows = 0;
  spillConfig.fileCreateConfig = {};
  // The default value of QueryConfig kSpillStartPartitionBit.
  spillConfig.startPartitionBit = 48;
  // The default value of QueryConfig spillNumPartitionBits.
  spillConfig.numPartitionBits = 3;

  if (spillerType_ == AggregationInputSpiller::kType) {
    const auto sortingKeys = SpillState::makeSortingKeys(
        std::vector<CompareFlags>(rowContainer_->keyTypes().size()));
    return std::make_unique<AggregationInputSpiller>(
        rowContainer_.get(),
        rowType_,
        HashBitRange{
            spillConfig.startPartitionBit,
            static_cast<uint8_t>(
                spillConfig.startPartitionBit + spillConfig.numPartitionBits)},
        sortingKeys,
        &spillConfig,
        &spillStats_);
  } else {
    VELOX_CHECK_EQ(spillerType_, AggregationOutputSpiller::kType);
    // TODO: Add config flag to control the max spill rows.
    return std::make_unique<AggregationOutputSpiller>(
        rowContainer_.get(), rowType_, &spillConfig, &spillStats_);
  }
}
} // namespace facebook::velox::exec::test
