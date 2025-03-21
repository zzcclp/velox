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

#include <folly/container/F14Map.h>
#include <cstdint>
#include <mutex>

#include "velox/common/base/BitUtil.h"
#include "velox/common/base/Exceptions.h"

namespace facebook::velox::cache {

/// Represents a stream in a table, e.g. nulls/lengths/data of a particular
/// column. Column-level access tracking uses this to identify the column within
/// a file or partition. The low 5 bits are the stream kind, e.g. nulls, data
/// etc. The high 27 bits are the node number in the file schema tree, i.e. the
/// column.
class TrackingId {
 public:
  TrackingId() : id_(-1) {}

  explicit TrackingId(int32_t id) : id_(id) {}

  size_t hash() const {
    return std::hash<int32_t>()(id_);
  }

  bool operator==(const TrackingId& other) const {
    return id_ == other.id_;
  }

  bool empty() const {
    return id_ == -1;
  }

  int32_t id() const {
    return id_;
  }

 private:
  int32_t id_;
};

} // namespace facebook::velox::cache

namespace std {
template <>
struct hash<::facebook::velox::cache::TrackingId> {
  size_t operator()(const ::facebook::velox::cache::TrackingId id) const {
    return id.hash();
  }
};
} // namespace std

namespace facebook::velox::cache {

class FileGroupStats;

/// Records references and actual uses of a stream.
struct TrackingData {
  double referencedBytes{};
  double lastReferencedBytes{};
  double readBytes{};
};

/// Tracks column access frequency during execution of a query. A ScanTracker is
/// created at the level of a Task/TableScan, so that all threads of a scan
/// report in the same tracker. The same ScanTracker tracks all reads of all
/// partitions of the scan. The groupId argument identifies the file group (e.g.
/// partition) a tracking event pertains to, since a single ScanTracker can
/// range over multiple partitions.
class ScanTracker {
 public:
  ScanTracker() : ScanTracker({}, nullptr, 1) {}

  /// Constructs a tracker with 'id'. The tracker will be owned by shared_ptr
  /// and will be referenced from a map from id to weak_ptr to 'this'.
  /// 'unregisterer' is supplied so that the destructor can remove the weak_ptr
  /// from the map of pending trackers. 'loadQuantum' is the largest single IO
  /// size for read.
  ScanTracker(
      std::string_view id,
      std::function<void(ScanTracker*)> unregisterer,
      int32_t loadQuantum,
      FileGroupStats* fileGroupStats = nullptr)
      : id_(id),
        unregisterer_(std::move(unregisterer)),
        fileGroupStats_(fileGroupStats) {}

  ~ScanTracker() {
    if (unregisterer_) {
      unregisterer_(this);
    }
  }

  /// Records that a scan references 'bytes' bytes of the stream given by 'id'.
  /// This is called when preparing to read a stripe.
  void recordReference(
      const TrackingId id,
      uint64_t bytes,
      uint64_t fileId,
      uint64_t groupId);

  /// Records that 'bytes' bytes have actually been read from the stream given
  /// by 'id'.
  void recordRead(
      const TrackingId id,
      uint64_t bytes,
      uint64_t fileId,
      uint64_t groupId);

  /// True if 'trackingId' is read at least 'minReadPct' % of the time.
  bool shouldPrefetch(TrackingId id, int32_t minReadPct) {
    return readPct(id) >= minReadPct;
  }

  /// Returns the percentage of referenced columns that are actually read. 100%
  /// if no data.
  int32_t readPct(TrackingId id) {
    std::lock_guard<std::mutex> l(mutex_);
    const auto& data = data_[id];
    if (data.referencedBytes == 0) {
      return 100;
    }
    return data.readBytes / data.referencedBytes * 100;
  }

  TrackingData trackingData(TrackingId id) {
    std::lock_guard<std::mutex> l(mutex_);
    return data_[id];
  }

  std::string_view id() const {
    return id_;
  }

  FileGroupStats* fileGroupStats() const {
    return fileGroupStats_;
  }

  std::string toString() const;

 private:
  // Id of query + scan operator to track.
  const std::string id_;
  const std::function<void(ScanTracker*)> unregisterer_{nullptr};
  FileGroupStats* const fileGroupStats_;

  std::mutex mutex_;
  folly::F14FastMap<TrackingId, TrackingData> data_;
  TrackingData sum_;
};

} // namespace facebook::velox::cache
