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

#pragma once

#include "velox/common/base/RandomUtil.h"
#include "velox/vector/LazyVector.h"

#include <cstdint>

namespace facebook::velox::dwio::common {

/// Top row level mutations.
struct Mutation {
  /// Bit masks for row numbers to be deleted.
  const uint64_t* deletedRows = nullptr;

  random::RandomSkipTracker* randomSkip = nullptr;
};

inline bool hasDeletion(const Mutation* mutation) {
  return mutation && (mutation->deletedRows || mutation->randomSkip);
}

class DeltaColumnUpdater {
 public:
  virtual ~DeltaColumnUpdater() = default;

  /// Update the values in `result' to reflect the delta updates on `baseRows'.
  /// `baseRows' are the rows starting from the beginning of the current scan
  /// (so that the delta readers can use this to choose which lines to read as
  /// well), not based on the positions in `result'.
  virtual void update(const RowSet& baseRows, VectorPtr& result) = 0;
};

} // namespace facebook::velox::dwio::common
