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

#include "velox/common/base/SpillStats.h"
#include <gtest/gtest.h>
#include "velox/common/base/VeloxException.h"
#include "velox/common/base/tests/GTestUtils.h"

using namespace facebook::velox::common;

TEST(SpillStatsTest, spillStats) {
  SpillStats stats1;
  ASSERT_TRUE(stats1.empty());
  stats1.spillRuns = 100;
  stats1.spilledInputBytes = 2048;
  stats1.spilledBytes = 1024;
  stats1.spilledPartitions = 1024;
  stats1.spilledFiles = 1023;
  stats1.spillWriteTimeNanos = 1023;
  stats1.spillFlushTimeNanos = 1023;
  stats1.spillWrites = 1023;
  stats1.spillSortTimeNanos = 1023;
  stats1.spillExtractVectorTimeNanos = 1023;
  stats1.spillFillTimeNanos = 1023;
  stats1.spilledRows = 1023;
  stats1.spillSerializationTimeNanos = 1023;
  stats1.spillMaxLevelExceededCount = 3;
  stats1.spillReadBytes = 1024;
  stats1.spillReads = 10;
  stats1.spillReadTimeNanos = 100;
  stats1.spillDeserializationTimeNanos = 100;
  ASSERT_FALSE(stats1.empty());
  SpillStats stats2;
  stats2.spillRuns = 100;
  stats2.spilledInputBytes = 2048;
  stats2.spilledBytes = 1024;
  stats2.spilledPartitions = 1025;
  stats2.spilledFiles = 1026;
  stats2.spillWriteTimeNanos = 1026;
  stats2.spillFlushTimeNanos = 1027;
  stats2.spillWrites = 1028;
  stats2.spillSortTimeNanos = 1029;
  stats2.spillExtractVectorTimeNanos = 1033;
  stats2.spillFillTimeNanos = 1030;
  stats2.spilledRows = 1031;
  stats2.spillSerializationTimeNanos = 1032;
  stats2.spillMaxLevelExceededCount = 4;
  stats2.spillReadBytes = 2048;
  stats2.spillReads = 10;
  stats2.spillReadTimeNanos = 100;
  stats2.spillDeserializationTimeNanos = 100;
  ASSERT_TRUE(stats1 < stats2);
  ASSERT_TRUE(stats1 <= stats2);
  ASSERT_FALSE(stats1 > stats2);
  ASSERT_FALSE(stats1 >= stats2);
  ASSERT_TRUE(stats1 != stats2);
  ASSERT_FALSE(stats1 == stats2);

  ASSERT_TRUE(stats1 == stats1);
  ASSERT_FALSE(stats1 != stats1);
  ASSERT_FALSE(stats1 > stats1);
  ASSERT_TRUE(stats1 >= stats1);
  ASSERT_FALSE(stats1 < stats1);
  ASSERT_TRUE(stats1 <= stats1);

  SpillStats delta = stats2 - stats1;
  ASSERT_EQ(delta.spilledInputBytes, 0);
  ASSERT_EQ(delta.spilledBytes, 0);
  ASSERT_EQ(delta.spilledPartitions, 1);
  ASSERT_EQ(delta.spilledFiles, 3);
  ASSERT_EQ(delta.spillWriteTimeNanos, 3);
  ASSERT_EQ(delta.spillFlushTimeNanos, 4);
  ASSERT_EQ(delta.spillWrites, 5);
  ASSERT_EQ(delta.spillSortTimeNanos, 6);
  ASSERT_EQ(delta.spillExtractVectorTimeNanos, 10);
  ASSERT_EQ(delta.spillFillTimeNanos, 7);
  ASSERT_EQ(delta.spilledRows, 8);
  ASSERT_EQ(delta.spillSerializationTimeNanos, 9);
  ASSERT_EQ(delta.spillReadBytes, 1024);
  ASSERT_EQ(delta.spillReads, 0);
  ASSERT_EQ(delta.spillReadTimeNanos, 0);
  ASSERT_EQ(delta.spillDeserializationTimeNanos, 0);
  delta = stats1 - stats2;
  ASSERT_EQ(delta.spilledInputBytes, 0);
  ASSERT_EQ(delta.spilledBytes, 0);
  ASSERT_EQ(delta.spilledPartitions, -1);
  ASSERT_EQ(delta.spilledFiles, -3);
  ASSERT_EQ(delta.spillWriteTimeNanos, -3);
  ASSERT_EQ(delta.spillFlushTimeNanos, -4);
  ASSERT_EQ(delta.spillWrites, -5);
  ASSERT_EQ(delta.spillSortTimeNanos, -6);
  ASSERT_EQ(delta.spillExtractVectorTimeNanos, -10);
  ASSERT_EQ(delta.spillFillTimeNanos, -7);
  ASSERT_EQ(delta.spilledRows, -8);
  ASSERT_EQ(delta.spillSerializationTimeNanos, -9);
  ASSERT_EQ(delta.spillMaxLevelExceededCount, -1);
  ASSERT_EQ(delta.spillReadBytes, -1024);
  ASSERT_EQ(delta.spillReads, 0);
  ASSERT_EQ(delta.spillReadTimeNanos, 0);
  ASSERT_EQ(delta.spillDeserializationTimeNanos, 0);
  stats1.spilledInputBytes = 2060;
  stats1.spilledBytes = 1030;
  stats1.spillReadBytes = 4096;
  VELOX_ASSERT_THROW(stats1 < stats2, "");
  VELOX_ASSERT_THROW(stats1 > stats2, "");
  VELOX_ASSERT_THROW(stats1 <= stats2, "");
  VELOX_ASSERT_THROW(stats1 >= stats2, "");
  ASSERT_TRUE(stats1 != stats2);
  ASSERT_FALSE(stats1 == stats2);
  const SpillStats zeroStats;
  stats1.reset();
  ASSERT_EQ(zeroStats, stats1);
  ASSERT_EQ(
      stats2.toString(),
      "spillRuns[100] spilledInputBytes[2.00KB] spilledBytes[1.00KB] "
      "spilledRows[1031] spilledPartitions[1025] spilledFiles[1026] "
      "spillFillTimeNanos[1.03us] spillSortTimeNanos[1.03us] spillExtractVectorTime[1.03us] "
      "spillSerializationTimeNanos[1.03us] spillWrites[1028] spillFlushTimeNanos[1.03us] "
      "spillWriteTimeNanos[1.03us] maxSpillExceededLimitCount[4] "
      "spillReadBytes[2.00KB] spillReads[10] spillReadTimeNanos[100ns] "
      "spillReadDeserializationTimeNanos[100ns]");
  ASSERT_EQ(
      fmt::format("{}", stats2),
      "spillRuns[100] spilledInputBytes[2.00KB] spilledBytes[1.00KB] "
      "spilledRows[1031] spilledPartitions[1025] spilledFiles[1026] "
      "spillFillTimeNanos[1.03us] spillSortTimeNanos[1.03us] spillExtractVectorTime[1.03us] "
      "spillSerializationTimeNanos[1.03us] spillWrites[1028] "
      "spillFlushTimeNanos[1.03us] spillWriteTimeNanos[1.03us] "
      "maxSpillExceededLimitCount[4] "
      "spillReadBytes[2.00KB] spillReads[10] spillReadTimeNanos[100ns] "
      "spillReadDeserializationTimeNanos[100ns]");
}
