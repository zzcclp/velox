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

#include <fmt/format.h>
#include <string>

namespace facebook::velox::core {

/// Generates unique sequential plan node IDs starting with zero or specified
/// value.
class PlanNodeIdGenerator {
 public:
  explicit PlanNodeIdGenerator(int startId = 0) : nextId_{startId} {}

  std::string next() {
    return fmt::format("{}", nextId_++);
  }

  void reset(int startId = 0) {
    nextId_ = startId;
  }

 private:
  int nextId_;
};

} // namespace facebook::velox::core
